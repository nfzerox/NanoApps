#!/usr/bin/env python3
"""Run the original Temple Run 1.0 engine from the local armv7 executable.

A gameplay reference harness, not an iOS emulator. The game's C++ engine (cGameController
and everything under it) runs in Unicorn; the C and C++ runtime calls it makes are supplied
here in Python (strings, file streams, the red-black tree helpers, libm, random()), OpenGL
calls are recorded instead of drawn, and the Objective-C layer (views, menus, sound, Game
Center, the store) is not run. The original executable and its data files are required and
stay untracked.
"""
import math
import plistlib
import struct
import subprocess
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UcError
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC)

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'original/v1.0/Payload/TempleRun.app'
BIN = ROOT / 'build/TempleRun-armv7'
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
# The same guest layout as the port's runtime (port/src/rt.h), so that the original engine here
# and the translated engine there can be compared byte for byte.
GUEST_SIZE = 0x6000000
EXTERN, FAKE_VTABLE, SOUND_MANAGER, FAKE_FUNCS = 0xea000, 0xf3e20, 0xf3c00, 0xf3f00
LITERALS, STACK_TOP, HEAP = 0xf4000, 0x100000, 0x100000
IMPORTS = 0x8000000
SMALL_MAX = 65536
BX_LR = 0xe12fff1e


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def unbits(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]


class BsdRandom:
    """Libc random(): the additive feedback generator, degree 31, as seeded by srandom()."""

    def __init__(self, seed=1):
        self.seed(seed)

    def seed(self, seed):
        r = [0] * 31
        r[0] = seed & 0xffffffff
        for i in range(1, 31):
            x = r[i - 1]
            if x >= 0x80000000:
                x -= 1 << 32
            if x == 0:
                x = 123459876
            hi, lo = int(x / 127773), int(math.fmod(x, 127773))
            x = 16807 * lo - 2836 * hi
            if x < 0:
                x += 0x7fffffff
            r[i] = x
        self.state, self.f, self.r = r, 3, 0
        self.calls = 0
        for _ in range(310):
            self.next()
        self.calls = 0

    def next(self):
        s = self.state
        s[self.f] = (s[self.f] + s[self.r]) & 0xffffffff
        out = (s[self.f] >> 1) & 0x7fffffff
        self.f = (self.f + 1) % 31
        self.r = (self.r + 1) % 31
        self.calls += 1
        return out


class Stream:
    def __init__(self, data=b'', name=''):
        self.data, self.pos, self.fail, self.name, self.out = bytearray(data), 0, False, name, bytearray()

    def token(self):
        d = self.data
        while self.pos < len(d) and d[self.pos] in b' \t\r\n\v\f':
            self.pos += 1
        start = self.pos
        while self.pos < len(d) and d[self.pos] not in b' \t\r\n\v\f':
            self.pos += 1
        if start == self.pos:
            self.fail = True
            return None
        return bytes(d[start:self.pos])


class Original:
    def __init__(self, width=320.0, height=480.0, scale=1.0, seed=1, trace=False, files=None, trace_functions=()):
        data = self.data = BIN.read_bytes()
        self.trace = trace
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0, GUEST_SIZE)
        uc.mem_map(IMPORTS, 0x80000)
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        sections = self.sections = {}
        pos = 28
        for _ in range(struct.unpack_from('<I', data, 16)[0]):
            cmd, size = struct.unpack_from('<II', data, pos)
            if cmd == 1:
                va, vs, off, length, _, _, nsects = struct.unpack_from('<7I', data, pos + 24)
                if length and va and data[pos + 8:pos + 18] != b'__LINKEDIT':
                    uc.mem_write(va, data[off:off + length])
                for s in range(nsects):
                    base = pos + 56 + 68 * s
                    name = data[base:base + 16].rstrip(b'\0').decode()
                    addr, ssize = struct.unpack_from('<II', data, base + 32)
                    sections.setdefault(name, (addr, ssize, struct.unpack_from('<I', data, base + 60)[0]))
            elif cmd == 0xb:
                indirect_off, _ = struct.unpack_from('<II', data, pos + 56)
            elif cmd == 2:
                symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, pos + 8)
            pos += size

        def symbol(index):
            strx = struct.unpack_from('<I', data, symoff + 12 * index)[0]
            return data[stroff + strx:data.index(b'\0', stroff + strx)].decode()

        # The executable's own symbols, for calling and replacing functions by name.
        self.sym = {}
        for i in range(nsyms):
            strx, ntype, _, _, value = struct.unpack_from('<IBBHI', data, symoff + 12 * i)
            if ntype & 0x0e == 0x0e and not ntype & 0xe0:
                self.sym.setdefault(data[stroff + strx:data.index(b'\0', stroff + strx)].decode(), value)

        # Every imported function gets its own "bx lr" slot; a hook there does the work in
        # Python. Imported data symbols get a zeroed block each.
        uc.mem_write(IMPORTS, struct.pack('<I', BX_LR) * 0x10000)
        self.imports, self.slots = {}, {}
        self.next_slot = 0x100
        self.extern, self.fake_funcs = {}, {}
        next_extern = EXTERN + 0x1000
        for section in ('__la_symbol_ptr', '__nl_symbol_ptr'):
            addr, size, first = sections[section]
            for i in range(size // 4):
                index = struct.unpack_from('<I', data, indirect_off + 4 * (first + i))[0]
                if index & 0xc0000000:
                    continue
                name = symbol(index)
                if section == '__nl_symbol_ptr':
                    if name.startswith(('__ZSt4endl', '__ZSt4ends', '__ZSt5flush')):
                        if name not in self.fake_funcs:
                            self.fake_funcs[name] = FAKE_FUNCS + 4 * len(self.fake_funcs)
                        target = self.fake_funcs[name]
                    else:
                        if name not in self.extern:
                            self.extern[name] = next_extern
                            next_extern += 0x1000 if name == '__DefaultRuneLocale' else 0x100
                        target = self.extern[name]
                else:
                    target = self.slot(name)
                uc.mem_write(addr + 4 * i, struct.pack('<I', target))
        self.fill_rune_locale()
        self.stop = IMPORTS + 0x40
        uc.hook_add(UC_HOOK_CODE, self.on_import, begin=IMPORTS, end=IMPORTS + 0x3ffff)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)

        self.next = HEAP
        self.tracing = False
        self.bins, self.top_size, self.heap_used = [0] * 28, 0, 0
        self.random = BsdRandom(seed)
        self.seed_override = seed
        self.streams = {}
        self.files = dict(files or {})
        self.config = plistlib.loads((APP / 'ImangiConfigDefaults.plist').read_bytes())
        self.calls = {}
        self.gl = []
        self.record_gl = False
        self.log = []
        self.textures = {}
        self.clock = 1000.0
        self.tutorial = False
        self.sounds = []
        self.fake_vtable = FAKE_VTABLE
        self.depth = 0

        self.replace('__ZN12ImangiConfig12getBoolValueERKSs', lambda: self.ret(int(bool(self.config_value()))))
        self.replace('__ZN12ImangiConfig13getFloatValueERKSs', lambda: self.ret(f32(float(self.config_value())), 'f'))
        self.replace('__ZN12ImangiConfig11getIntValueERKSs', lambda: self.ret(int(self.config_value()), 'i'))
        self.replace('__ZN17ImangiPreferences17isTutorialEnabledEv', lambda: self.ret(int(self.tutorial)))
        self.replace('__ZN17ImangiPreferences20setIsTutorialEnabledEb', lambda: self.ret(0))
        self.replace('__ZN17ImangiPreferences15savePreferencesEv', lambda: self.ret(0))
        self.sound_manager = SOUND_MANAGER
        for name in list(self.sym):
            if (name.startswith('__ZN13cSoundManager') or name.startswith('__ZNK13cSoundManager')) \
                    and self.sym[name] < sections['__symbol_stub4'][0]:        # functions, not its static data
                if 'getInstance' in name:
                    self.replace(name, lambda: self.ret(self.sound_manager))
                elif 'playSound' in name:
                    self.replace(name, self.on_play_sound)
                elif 'C1' not in name and 'C2' not in name:
                    self.replace(name, lambda: self.ret(0))
        self.replace('__Z24getPathForImangiResourceRKSsb', lambda: self.set_string(self.arg(0), self.string(self.arg(1))))
        self.replace('__ZN15cTextureManager12getTextureIdESs', lambda: self.ret(
            self.textures.get(self.string(self.arg(1)).decode(), 0)))
        self.replace('__ZN15cTextureManager14loadPVRTextureERKSsS1_b', self.on_load_texture)
        self.replace('__ZN15cTextureManager11loadTextureERKSsS1_bbb', self.on_load_texture)

        self.trace = []
        self.tracing = bool(trace_functions)
        for function in trace_functions:
            uc.hook_add(UC_HOOK_CODE, self.on_function, begin=function, end=function)
        addr, size, _ = sections['__mod_init_func']
        for i in range(size // 4):
            self.call(self.word(addr + 4 * i))

        self.width, self.height = width, height
        self.game = self.alloc(596)
        self.call(self.sym['__ZN15cGameControllerC1Efffb'], self.game, bits(width), bits(height), bits(scale), stack=[0])

    def slot(self, name):
        if name not in self.slots:
            target = IMPORTS + 4 * self.next_slot
            self.next_slot += 1
            self.slots[name] = target
            self.imports[target] = name
        return self.slots[name]

    def replace(self, name, handler):
        """Send calls of one of the executable's own functions to Python."""
        target = IMPORTS + 4 * self.next_slot
        self.next_slot += 1
        self.imports[target] = handler
        self.uc.mem_write(self.sym[name], struct.pack('<II', 0xe51ff004, target))    # ldr pc, [pc, #-4]

    def fill_rune_locale(self):
        base = self.extern.get('__DefaultRuneLocale')
        if base is None:
            return
        A, C, D, G, L, P, S, U, X, B, R = (0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000,
                                           0x20000, 0x40000)
        table, lower, upper = [], [], []
        for c in range(256):
            ch, t = chr(c), 0
            if c < 128:
                if ch.isalpha():
                    t |= A | G | R | (L if ch.islower() else U)
                    if ch in 'abcdefABCDEF':
                        t |= X
                elif ch.isdigit():
                    t |= D | G | R | X
                elif ch in ' \t\n\v\f\r':
                    t |= S | (B | R if ch == ' ' else 0) | (B if ch == '\t' else 0) | (C if ch != ' ' else 0)
                elif c < 32 or c == 127:
                    t |= C
                else:
                    t |= P | G | R
            table.append(t)
            lower.append(ord(ch.lower()) if c < 128 else c)
            upper.append(ord(ch.upper()) if c < 128 else c)
        self.uc.mem_write(base + 0x34, struct.pack('<256I', *table) + struct.pack('<256I', *lower)
                          + struct.pack('<256I', *upper))

    # ---- memory -------------------------------------------------------------------------
    # The allocator of port/src/rt.c, statement for statement: 16-byte headers (size and
    # flags, size of the block before, free list links), merging of free neighbours, lists
    # by size class, a free block at the top given back, memory zeroed when handed out.
    def block_size(self, b):
        return self.word(b) & ~15

    @staticmethod
    def bin_for(size):
        return size.bit_length() - 5

    def insert_free(self, b):
        k = self.bin_for(self.block_size(b))
        self.put(b + 8, 0)
        self.put(b + 12, self.bins[k])
        if self.bins[k]:
            self.put(self.bins[k] + 8, b)
        self.bins[k] = b

    def remove_free(self, b):
        prev, nxt = self.word(b + 8), self.word(b + 12)
        if prev:
            self.put(prev + 12, nxt)
        else:
            self.bins[self.bin_for(self.block_size(b))] = nxt
        if nxt:
            self.put(nxt + 8, prev)

    def set_next_prev(self, b):
        nxt = b + self.block_size(b)
        if nxt < self.next:
            self.put(nxt + 4, self.block_size(b))
        else:
            self.top_size = self.block_size(b)

    def release_block(self, b):
        self.put(b, self.block_size(b))
        nxt = b + self.block_size(b)
        if nxt < self.next and not self.word(nxt) & 1:
            self.remove_free(nxt)
            self.put(b, self.word(b) + self.block_size(nxt))
        if self.word(b + 4):
            prev = b - self.word(b + 4)
            if not self.word(prev) & 1:
                self.remove_free(prev)
                self.put(prev, self.word(prev) + self.block_size(b))
                b = prev
        if b + self.block_size(b) == self.next:
            self.next = b
            self.top_size = self.word(b + 4)
            return
        self.set_next_prev(b)
        self.insert_free(b)

    def alloc(self, size):
        need = max(16, (size + 15) & ~15) + 16
        b = 0
        for k in range(self.bin_for(need), 28):
            c = self.bins[k]
            while c:
                if self.block_size(c) >= need:
                    b = c
                    break
                c = self.word(c + 12)
            if b:
                break
        if b:
            self.remove_free(b)
            available = self.block_size(b)
            if available - need >= 64:
                tail = b + need
                self.put(b, need | 1)
                self.put(tail, available - need)
                self.put(tail + 4, need)
                self.set_next_prev(tail)
                self.insert_free(tail)
            else:
                self.put(b, available | 1)
        else:
            if self.next + need > GUEST_SIZE:
                raise MemoryError('reference heap exhausted')
            b = self.next
            self.put(b, need | 1)
            self.put(b + 4, 0 if self.next == HEAP else self.top_size)
            self.next += need
            self.top_size = need
        self.put(b + 8, 0)
        self.put(b + 12, 0)
        self.uc.mem_write(b + 16, bytes(self.block_size(b) - 16))
        if self.tracing:
            self.trace.append((0xa110c, size, b + 16, 0, 0))
        self.heap_used += self.block_size(b)
        return b + 16

    def free(self, addr):
        if addr < HEAP + 16 or addr >= self.next or addr & 15 or not self.word(addr - 16) & 1:
            return
        b = addr - 16
        if self.tracing:
            self.trace.append((0xf4ee, self.block_size(b), addr, 0, 0))
        self.heap_used -= self.block_size(b)
        self.release_block(b)

    def word(self, addr):
        return struct.unpack('<I', self.uc.mem_read(addr, 4))[0]

    def int(self, addr):
        return struct.unpack('<i', self.uc.mem_read(addr, 4))[0]

    def float(self, addr):
        return struct.unpack('<f', self.uc.mem_read(addr, 4))[0]

    def floats(self, addr, n):
        return struct.unpack('<%df' % n, self.uc.mem_read(addr, 4 * n))

    def byte(self, addr):
        return self.uc.mem_read(addr, 1)[0]

    def put(self, addr, value):
        self.uc.mem_write(addr, struct.pack('<I', value & 0xffffffff))

    def cstr(self, addr):
        if addr == 0:
            return b''
        out = bytearray()
        while True:
            chunk = bytes(self.uc.mem_read(addr, 64))
            end = chunk.find(b'\0')
            if end >= 0:
                return bytes(out + chunk[:end])
            out += chunk
            addr += 64

    # std::string as libstdc++ lays it out: one pointer to the characters, with the
    # length, capacity and reference count in the three words before them.
    def string(self, obj):
        p = self.word(obj)
        return bytes(self.uc.mem_read(p, self.word(p - 12)))

    def set_string(self, obj, value):
        block = self.alloc(13 + len(value))
        self.uc.mem_write(block, struct.pack('<IIi', len(value), len(value), 0) + bytes(value) + b'\0')
        self.put(obj, block + 12)

    def assign_string(self, obj, value):
        """Replace a constructed string's value, releasing the old characters."""
        old = self.word(obj)
        self.set_string(obj, value)
        self.free(old - 12)

    def new_string(self, value):
        obj = self.alloc(4)
        self.set_string(obj, value if isinstance(value, bytes) else value.encode())
        return obj

    # ---- calls --------------------------------------------------------------------------
    def call(self, address, *args, stack=()):
        uc = self.uc
        saved = None
        if self.depth:
            saved = [uc.reg_read(r) for r in range(66, 66 + 13)] + [uc.reg_read(UC_ARM_REG_SP), uc.reg_read(UC_ARM_REG_LR),
                                                                  uc.reg_read(UC_ARM_REG_PC)]
            sp = (uc.reg_read(UC_ARM_REG_SP) - 0x400) & ~7
        else:
            sp = STACK_TOP - 0x1000
        sp -= 4 * len(stack)
        if stack:
            uc.mem_write(sp, struct.pack('<%dI' % len(stack), *[v & 0xffffffff for v in stack]))
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, self.stop)
        for reg, value in zip(REGS, args):
            uc.reg_write(reg, value & 0xffffffff)
        self.depth += 1
        try:
            uc.emu_start(address, self.stop)
        finally:
            self.depth -= 1
        if uc.reg_read(UC_ARM_REG_PC) != self.stop:
            raise RuntimeError('call to %x did not return (pc %x)' % (address, uc.reg_read(UC_ARM_REG_PC)))
        result = uc.reg_read(UC_ARM_REG_R0)
        if saved:
            for r, v in zip(range(66, 66 + 13), saved):
                uc.reg_write(r, v)
            uc.reg_write(UC_ARM_REG_SP, saved[13])
            uc.reg_write(UC_ARM_REG_LR, saved[14])
        return result

    def callf(self, name, *args, stack=()):
        return self.call(self.sym[name], *args, stack=stack)

    def arg(self, i):
        return self.uc.reg_read(REGS[i])

    def sarg(self, i):
        v = self.arg(i)
        return v - (1 << 32) if v & 0x80000000 else v

    def farg(self, i):
        return unbits(self.arg(i))

    def darg(self, i):
        return struct.unpack('<d', struct.pack('<II', self.arg(i), self.arg(i + 1)))[0]

    def stack_arg(self, i):
        return self.word(self.uc.reg_read(UC_ARM_REG_SP) + 4 * i)

    def ret(self, value=0, kind='I'):
        if kind == 'I':
            value &= 0xffffffff
        raw = struct.pack('<' + kind, value)
        for i in range(len(raw) // 4):
            self.uc.reg_write(REGS[i], struct.unpack_from('<I', raw, 4 * i)[0])

    def on_function(self, uc, address, size, user):
        self.trace.append((address, uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2),
                           uc.reg_read(UC_ARM_REG_R3)))

    def on_unmapped(self, uc, access, address, size, value, user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        self.fault = 'unmapped access to %x from pc %x (lr %x)' % (address, pc, uc.reg_read(UC_ARM_REG_LR))
        return False

    def on_import(self, uc, address, size, user):
        if address == self.stop:
            return
        name = self.imports.get(address)
        if name is None:
            raise RuntimeError('jump to unknown import address %x' % address)
        if callable(name):
            name()
            # a replaced function was entered by a jump; return to its caller
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
            return
        self.calls[name] = self.calls.get(name, 0) + 1
        handler = getattr(self, 'i' + name, None)
        if handler is not None:
            handler()
        elif name.startswith('_gl'):
            self.on_gl(name)
        elif name in self.NOOPS or name.startswith(('_objc_', '_NS', '_CF', '_UI', '_CG', '_Audio', '_al', '_SC')):
            self.ret(0)
        else:
            raise RuntimeError('unsupported import %s (lr %x)' % (name, uc.reg_read(UC_ARM_REG_LR)))

    NOOPS = {'__Unwind_SjLj_Register', '__Unwind_SjLj_Unregister', '__ZNSt8ios_base4InitC1Ev', '__ZNSt8ios_base4InitD1Ev',
             '___cxa_atexit', '__ZNSaIcEC1Ev', '__ZNSaIcED1Ev', '_usleep', '_dispatch_async', '_printf', '_puts',
             '__ZNSo5flushEv', '_sysctlbyname', '_host_statistics', '_mach_host_self', '__Block_object_assign',
             '__Block_object_dispose'}

    # ---- C library ----------------------------------------------------------------------
    def i_malloc(self):
        self.ret(self.alloc(self.arg(0)))

    i__Znwm = i__Znam = i_malloc

    def i_free(self):
        self.free(self.arg(0))
        self.ret(0)

    i__ZdlPv = i__ZdaPv = i_free

    def i_memset(self):
        self.uc.mem_write(self.arg(0), bytes([self.arg(1) & 255]) * self.arg(2))

    def i_memcpy(self):
        if self.arg(2):
            self.uc.mem_write(self.arg(0), bytes(self.uc.mem_read(self.arg(1), self.arg(2))))

    i_memmove = i_memcpy

    def i_strlen(self):
        self.ret(len(self.cstr(self.arg(0))))

    def i_strncmp(self):
        a, b, n = self.cstr(self.arg(0))[:self.arg(2)], self.cstr(self.arg(1))[:self.arg(2)], self.arg(2)
        self.ret((a > b) - (a < b), 'i')

    def i_strnstr(self):
        hay = self.cstr(self.arg(0))[:self.arg(2)]
        at = hay.find(self.cstr(self.arg(1)))
        self.ret(self.arg(0) + at if at >= 0 else 0)

    def i_strtoull(self):
        text = self.cstr(self.arg(0)).decode('latin1').strip()
        try:
            self.ret(int(text, self.arg(2) or 10), 'Q')
        except ValueError:
            self.ret(0, 'Q')

    def i_sprintf(self):
        import re
        fmt = self.cstr(self.arg(1)).decode('latin1')
        words = [self.arg(2), self.arg(3)] + [self.stack_arg(i) for i in range(8)]
        values, at = [], 0
        for spec in re.findall(r'%[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h)?([a-zA-Z%])', fmt):
            if spec == '%':
                continue
            if spec in 'di':
                v = words[at]
                values.append(v - (1 << 32) if v & 0x80000000 else v)
                at += 1
            elif spec in 'uxX':
                values.append(words[at])
                at += 1
            elif spec == 's':
                values.append(self.cstr(words[at]).decode('latin1'))
                at += 1
            elif spec in 'fgeG':
                values.append(struct.unpack('<d', struct.pack('<II', words[at], words[at + 1]))[0])
                at += 2
            else:
                raise RuntimeError('sprintf format %r' % fmt)
        text = (re.sub(r'(l|ll|h)(?=[a-zA-Z])', '', fmt) % tuple(values)).encode('latin1')
        self.uc.mem_write(self.arg(0), text + b'\0')
        self.ret(len(text))

    def i_random(self):
        self.ret(self.random.next())

    def i_srandom(self):
        self.log.append('srandom(%d)' % self.arg(0))
        self.random.seed(self.seed_override if self.seed_override is not None else self.arg(0))

    def i_srand(self):
        self.ret(0)

    def i_abort(self):
        raise RuntimeError('abort() called (lr %x)' % self.uc.reg_read(UC_ARM_REG_LR))

    def i_CACurrentMediaTime(self):
        self.ret(self.clock, 'd')

    def i___maskrune(self):
        self.ret(0)

    def i___modsi3(self):
        a, b = self.sarg(0), self.sarg(1)
        self.ret(int(math.fmod(a, b)) if b else 0, 'i')

    def i___udivsi3(self):
        self.ret(self.arg(0) // self.arg(1) if self.arg(1) else 0)

    def i___umodsi3(self):
        self.ret(self.arg(0) % self.arg(1) if self.arg(1) else 0)

    def i___udivdi3(self):
        a = self.arg(0) | self.arg(1) << 32
        b = self.arg(2) | self.arg(3) << 32
        self.ret(a // b if b else 0, 'Q')

    def i___floatdidf(self):
        v = self.arg(0) | self.arg(1) << 32
        self.ret(float(v - (1 << 64) if v >> 63 else v), 'd')

    def i___floatundidf(self):
        self.ret(float(self.arg(0) | self.arg(1) << 32), 'd')

    def i___floatundisf(self):
        self.ret(f32(float(self.arg(0) | self.arg(1) << 32)), 'f')

    def i_sin(self):
        self.ret(math.sin(self.darg(0)), 'd')

    def i_cos(self):
        self.ret(math.cos(self.darg(0)), 'd')

    def i_tan(self):
        self.ret(math.tan(self.darg(0)), 'd')

    def i_floor(self):
        self.ret(float(math.floor(self.darg(0))), 'd')

    def i_atan2(self):
        self.ret(math.atan2(self.darg(0), self.darg(2)), 'd')

    def i_sinf(self):
        self.ret(f32(math.sin(self.farg(0))), 'f')

    def i_tanf(self):
        self.ret(f32(math.tan(self.farg(0))), 'f')

    def i_floorf(self):
        self.ret(f32(math.floor(self.farg(0))), 'f')

    def i_ceilf(self):
        self.ret(f32(math.ceil(self.farg(0))), 'f')

    def i___dynamic_cast(self):
        ptr, target = self.arg(0), self.arg(2)
        if not ptr:
            return self.ret(0)
        info = self.word(self.word(ptr) - 4)
        for _ in range(8):
            if info == target:
                return self.ret(ptr)
            if not (0x1000 <= info < 0xea000):
                break
            base = self.word(info + 8)          # single-inheritance type_info: the base class
            if not (0xd9000 <= base < 0xea000) or base == info:
                break
            info = base
        self.ret(0)

    # ---- std::string --------------------------------------------------------------------
    def i__ZNSsC1EPKcRKSaIcE(self):
        self.set_string(self.arg(0), self.cstr(self.arg(1)))

    def i__ZNSsC1ERKSs(self):
        self.set_string(self.arg(0), self.string(self.arg(1)))

    def i__ZNSsC1Ev(self):
        self.set_string(self.arg(0), b'')

    def i__ZNSsaSEPKc(self):
        self.assign_string(self.arg(0), self.cstr(self.arg(1)))

    def i__ZNSsD1Ev(self):
        self.free(self.word(self.arg(0)) - 12)
        self.ret(0)

    def i__ZNSsaSERKSs(self):
        if self.arg(0) != self.arg(1):
            self.assign_string(self.arg(0), self.string(self.arg(1)))

    def i__ZNSs6appendEmc(self):
        self.assign_string(self.arg(0), self.string(self.arg(0)) + bytes([self.arg(2) & 255]) * self.arg(1))

    def i__ZNSs5eraseEmm(self):
        s, pos, n = self.string(self.arg(0)), self.arg(1), self.arg(2)
        end = len(s) if n == 0xffffffff else min(len(s), pos + n)
        self.assign_string(self.arg(0), s[:pos] + s[end:])

    def i__ZNSs5eraseEN9__gnu_cxx17__normal_iteratorIPcSsEES2_(self):
        obj = self.arg(0)
        p, s = self.word(obj), self.string(obj)
        first, last = self.arg(1) - p, self.arg(2) - p
        self.assign_string(obj, s[:first] + s[last:])
        self.ret(self.word(obj) + first)

    def i__ZNSsixEm(self):
        self.ret(self.word(self.arg(0)) + self.arg(1))

    i__ZNKSs2atEm = i__ZNSsixEm

    def i__ZNSs5beginEv(self):
        self.ret(self.word(self.arg(0)))

    i__ZNKSs5c_strEv = i__ZNSs5beginEv

    def i__ZNSs3endEv(self):
        p = self.word(self.arg(0))
        self.ret(p + self.word(p - 12))

    def i__ZNKSs4sizeEv(self):
        self.ret(self.word(self.word(self.arg(0)) - 12))

    i__ZNKSs6lengthEv = i__ZNKSs4sizeEv

    def i__ZNKSs5emptyEv(self):
        self.ret(int(self.word(self.word(self.arg(0)) - 12) == 0))

    def i__ZNKSs4findEcm(self):
        self.ret(self.string(self.arg(0)).find(bytes([self.arg(1) & 255]), self.arg(2)))

    def i__ZNKSs17find_first_not_ofEcm(self):
        s, c = self.string(self.arg(0)), self.arg(1) & 255
        for i in range(self.arg(2), len(s)):
            if s[i] != c:
                return self.ret(i)
        self.ret(0xffffffff)

    def i__ZNKSs16find_last_not_ofEcm(self):
        s, c = self.string(self.arg(0)), self.arg(1) & 255
        for i in range(min(self.arg(2), len(s) - 1), -1, -1):
            if s[i] != c:
                return self.ret(i)
        self.ret(0xffffffff)

    def i__ZNKSs6substrEmm(self):
        s, pos, n = self.string(self.arg(1)), self.arg(2), self.arg(3)
        self.set_string(self.arg(0), s[pos:] if n == 0xffffffff else s[pos:pos + n])

    def i__ZNKSs7compareEPKc(self):
        a, b = self.string(self.arg(0)), self.cstr(self.arg(1))
        self.ret((a > b) - (a < b), 'i')

    def i__ZNKSs7compareERKSs(self):
        a, b = self.string(self.arg(0)), self.string(self.arg(1))
        self.ret((a > b) - (a < b), 'i')

    # ---- red-black tree helpers (kept as a plain search tree: lookups only follow links) ----
    def i__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(self):
        left, x, p, header = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
        self.uc.mem_write(x, struct.pack('<IIII', 0, p, 0, 0))
        if left:
            self.put(p + 8, x)
            if p == header:
                self.put(header + 4, x)
                self.put(header + 12, x)
            elif p == self.word(header + 8):
                self.put(header + 8, x)
        else:
            self.put(p + 12, x)
            if p == self.word(header + 12):
                self.put(header + 12, x)
        self.put(self.word(header + 4), 1)      # a black root, as the iterators expect

    def tree_next(self, x):
        right = self.word(x + 12)
        if right:
            x = right
            while self.word(x + 8):
                x = self.word(x + 8)
            return x
        y = self.word(x + 4)
        while x == self.word(y + 12):
            x, y = y, self.word(y + 4)
        return y if self.word(x + 12) != y else x

    def tree_prev(self, x):
        if self.word(x) == 0 and self.word(self.word(x + 4) + 4) == x:
            return self.word(x + 12)
        left = self.word(x + 8)
        if left:
            y = left
            while self.word(y + 12):
                y = self.word(y + 12)
            return y
        y = self.word(x + 4)
        while x == self.word(y + 8):
            x, y = y, self.word(y + 4)
        return y

    def i__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(self):
        self.ret(self.tree_next(self.arg(0)))

    def i__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(self):
        self.ret(self.tree_prev(self.arg(0)))

    def i__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_(self):
        z, header = self.arg(0), self.arg(1)
        w = self.word
        left, right, parent = w(z + 8), w(z + 12), w(z + 4)

        def relink(old, new):
            if w(header + 4) == old:
                self.put(header + 4, new)
            elif w(parent + 8) == old:
                self.put(parent + 8, new)
            else:
                self.put(parent + 12, new)
            if new:
                self.put(new + 4, parent)
        if w(header + 8) == z:
            self.put(header + 8, self.tree_next(z) if w(header + 4) != z or right else header)
        if w(header + 12) == z:
            self.put(header + 12, self.tree_prev(z) if w(header + 4) != z or left else header)
        if not left or not right:
            relink(z, left or right)
        else:
            y = right
            while w(y + 8):
                y = w(y + 8)
            if y != right:
                yp, yr = w(y + 4), w(y + 12)
                self.put(yp + 8, yr)
                if yr:
                    self.put(yr + 4, yp)
                self.put(y + 12, right)
                self.put(right + 4, y)
            self.put(y + 8, left)
            self.put(left + 4, y)
            relink(z, y)
        root = w(header + 4)
        if root:
            self.put(root + 4, header)
            self.put(root, 1)
        else:
            self.put(header + 8, header)
            self.put(header + 12, header)
        self.ret(z)

    # ---- streams ------------------------------------------------------------------------
    def find_file(self, name):
        name = name.decode('latin1')
        if name in self.files:
            return self.files[name]
        base = name.rsplit('/', 1)[-1]
        if base in self.files:
            return self.files[base]
        path = APP / base
        return path.read_bytes() if base and path.is_file() else None

    def open_stream(self, obj, stream):
        self.put(obj, self.fake_vtable)
        self.put(self.fake_vtable - 12, 0)
        self.put(obj + 8, self.fake_vtable + 16)
        self.put(self.fake_vtable + 4, 0xfffffff8)
        self.streams[obj] = stream

    def stream(self, addr):
        """The stream an address belongs to: the object itself, its ostream half at +8, or the
        basic_ios part that sits at the end of the object."""
        s = self.streams.get(addr)
        if s is None:
            best = max((base for base in self.streams if base <= addr < base + 0x400), default=None)
            s = self.streams.get(best)
        return s

    def i__ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode(self):
        name = self.cstr(self.arg(1))
        data = self.find_file(name)
        s = Stream(data or b'', name.decode('latin1'))
        s.fail = data is None
        self.log.append('open %s%s' % (s.name.rsplit('/', 1)[-1], ' (missing)' if data is None else ''))
        self.open_stream(self.arg(0), s)

    def i__ZNSt13basic_fstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode(self):
        if self.arg(2) & 0x10:
            name = self.cstr(self.arg(1))
            s = Stream(b'', name.decode('latin1'))
            s.writing = True
            self.log.append('write %s' % s.name.rsplit('/', 1)[-1])
            self.open_stream(self.arg(0), s)
        else:
            self.i__ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode()

    def i__ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode(self):
        name = self.cstr(self.arg(1))
        s = Stream(b'', name.decode('latin1'))
        s.writing = True
        self.log.append('write %s' % s.name.rsplit('/', 1)[-1])
        self.open_stream(self.arg(0), s)

    def close_stream(self):
        s = self.streams.get(self.arg(0))
        if s is not None and getattr(s, 'writing', False):
            self.files[s.name.rsplit('/', 1)[-1]] = bytes(s.out)

    i__ZNSt14basic_ifstreamIcSt11char_traitsIcEE5closeEv = close_stream
    i__ZNSt13basic_fstreamIcSt11char_traitsIcEE5closeEv = close_stream
    i__ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv = close_stream

    def destroy_stream(self):
        self.close_stream()
        self.streams.pop(self.arg(0), None)

    i__ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev = destroy_stream
    i__ZNSt13basic_fstreamIcSt11char_traitsIcEED1Ev = destroy_stream
    i__ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev = destroy_stream
    i__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev = destroy_stream
    i__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev = destroy_stream

    def i__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode(self):
        s = Stream()
        s.out = s.data
        self.open_stream(self.arg(0), s)

    i__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode = \
        i__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode

    def i__ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv(self):
        self.set_string(self.arg(0), bytes(self.stream(self.arg(1)).out))

    def i__ZNSi4readEPci(self):
        s, n = self.stream(self.arg(0)), self.sarg(2)
        chunk = bytes(s.data[s.pos:s.pos + n])
        s.pos += len(chunk)
        if len(chunk) < n:
            s.fail = True
        if chunk:
            self.uc.mem_write(self.arg(1), chunk)

    def i__ZNSi4peekEv(self):
        s = self.stream(self.arg(0))
        self.ret(s.data[s.pos] if s.pos < len(s.data) and not s.fail else 0xffffffff)

    def i__ZNSi7getlineEPci(self):
        s, n = self.stream(self.arg(0)), self.sarg(2)
        end = s.data.find(b'\n', s.pos)
        line = bytes(s.data[s.pos:end if end >= 0 else len(s.data)])
        if end < 0 and not line:
            s.fail = True
        if len(line) > n - 1:
            line = line[:n - 1]
            s.pos += len(line)
            s.fail = True
        else:
            s.pos += len(line) + (1 if end >= 0 else 0)
        self.uc.mem_write(self.arg(1), line + b'\0')

    def extract(self, convert, pack):
        s = self.stream(self.arg(0))
        token = None if s.fail else s.token()
        if token is None:
            return
        try:
            self.uc.mem_write(self.arg(1), struct.pack(pack, convert(token)))
        except ValueError:
            s.fail = True

    def i__ZNSirsERi(self):
        self.extract(lambda t: int(t), '<i')

    def i__ZNSirsERf(self):
        self.extract(lambda t: float(t), '<f')

    def i__ZNSirsERb(self):
        self.extract(lambda t: int(t) != 0, '<B')

    def i__ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E(self):
        s = self.stream(self.arg(0))
        token = None if s.fail else s.token()
        if token is not None:
            self.set_string(self.arg(1), token)

    def i__ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_(self):
        s = self.stream(self.arg(0))
        while s.pos < len(s.data) and s.data[s.pos] in b' \t\r\n\v\f':
            s.pos += 1
        if s.pos < len(s.data):
            self.uc.mem_write(self.arg(1), bytes([s.data[s.pos]]))
            s.pos += 1
        else:
            s.fail = True

    def i__ZNKSt9basic_iosIcSt11char_traitsIcEE4failEv(self):
        s = self.stream(self.arg(0))
        self.ret(int(s is None or s.fail))

    i__ZNKSt9basic_iosIcSt11char_traitsIcEEntEv = i__ZNKSt9basic_iosIcSt11char_traitsIcEE4failEv

    def i__ZNKSt9basic_iosIcSt11char_traitsIcEEcvPvEv(self):
        s = self.stream(self.arg(0))
        self.ret(0 if s is None or s.fail else self.arg(0))

    def emit(self, text):
        s = self.stream(self.arg(0))
        if s is None:
            self.log.append(text if isinstance(text, str) else text.decode('latin1'))
        else:
            s.out += text.encode() if isinstance(text, str) else text

    def i__ZNSolsEi(self):
        self.emit('%d' % self.sarg(1))

    def i__ZNSolsEj(self):
        self.emit('%d' % self.arg(1))

    i__ZNSolsEm = i__ZNSolsEj

    def i__ZNSolsEb(self):
        self.emit('%d' % (self.arg(1) & 1))

    def i__ZNSolsEf(self):
        self.emit('%g' % self.farg(1))

    def i__ZNSolsEPFRSoS_E(self):
        if self.arg(1) == self.fake_funcs.get('__ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_'):
            self.emit('\n')

    def i__ZNSo5writeEPKci(self):
        self.emit(bytes(self.uc.mem_read(self.arg(1), self.arg(2))))

    def i__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(self):
        self.emit(self.cstr(self.arg(1)))

    i__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKh = i__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc

    def i__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E(self):
        self.emit(self.string(self.arg(1)))

    # ---- the pieces of the engine that are replaced -----------------------------------------
    def config_value(self):
        key = self.string(self.arg(0)).decode()
        if key not in self.config:
            self.log.append('config key missing: ' + key)
            return 0
        return self.config[key]

    def on_load_texture(self):
        name = self.string(self.arg(1)).decode()
        self.textures.setdefault(name, len(self.textures) + 1)
        self.log.append('texture %s <- %s' % (name, self.string(self.arg(2)).decode()))
        self.ret(1)

    def on_play_sound(self):
        self.sounds.append(self.arg(1))
        self.ret(0)

    def on_gl(self, name):
        uc = self.uc
        if name in ('_glGenTextures', '_glGenBuffers'):
            for i in range(self.arg(0)):
                self.gl_names = getattr(self, 'gl_names', 100) + 1
                self.put(self.arg(1) + 4 * i, self.gl_names)
        elif name == '_glGetString':
            return self.ret(self.sym_empty())
        if self.record_gl:
            self.gl.append((name, self.arg(0), self.arg(1), self.arg(2), self.arg(3),
                            self.stack_arg(0), self.stack_arg(1), self.stack_arg(2)))
        self.ret(0)

    def sym_empty(self):
        return SOUND_MANAGER + 0x100

    # ---- the game -----------------------------------------------------------------------
    def run(self, name, *args, stack=()):
        self.fault = None
        try:
            return self.callf(name, *args, stack=stack)
        except UcError as e:
            raise RuntimeError('%s: %s; %s' % (name, e, self.fault)) from None

    def dump(self):
        """Guest memory in the layout port/tests/engine_run.c writes."""
        mem = self.uc.mem_read
        return struct.pack('<I', self.next) + bytes(mem(0xd9000, 0xea000 - 0xd9000)) \
            + bytes(mem(EXTERN, LITERALS - EXTERN)) + bytes(mem(HEAP, self.next - HEAP))

    def initialize(self):
        g = self.game
        self.run('__ZN15cGameController9initalizeEv', g)
        self.run('__ZN15cGameController20loadLevelInformationEv', g)
        self.simulate(0.01)

    def start(self):
        self.run('__ZN15cGameController5startEv', self.game)

    def simulate(self, dt):
        self.run('__ZN15cGameController8simulateEf', self.game, bits(dt))

    def draw(self):
        self.gl = []
        self.record_gl = True
        self.run('__ZN15cGameController4drawEv', self.game)
        self.record_gl = False
        return self.gl


if __name__ == '__main__':
    vm = Original()
    print('constructed; log:', vm.log[:10])
    import time
    t = time.time()
    vm.initialize()
    print('initialized in %.1f s; %d log lines; heap %.1f MB, %.1f MB in use' % (time.time() - t, len(vm.log), (vm.next - HEAP) / 1e6, vm.heap_used / 1e6))
    print('\n'.join(l for l in vm.log if not l.startswith(('open ', 'texture '))))
    vm.start()
    t = time.time()
    for i in range(120):
        vm.simulate(1 / 30)
    print('120 frames in %.1f s; random calls %d' % (time.time() - t, vm.random.calls))
    print('imports used:', ' '.join(sorted(n for n in vm.calls if not n.startswith('__Z'))))
    gl = vm.draw()
    print('draw: %d GL calls' % len(gl))
    print('\n'.join(l for l in vm.log if not l.startswith(('open ', 'texture ')))[-600:])
