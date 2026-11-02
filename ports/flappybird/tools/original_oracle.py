#!/usr/bin/env python3
"""Run the original Flappy Bird 1.2 game code from the local armv7 executable.

A gameplay reference harness, not an iOS emulator: the engine's own functions run in
Unicorn; the C library, libm and OpenGL imports are supplied here (OpenGL calls do
nothing; the vertex list the engine built is read just before it would be drawn).
The Objective-C layer (view, sound, Game Center, adverts) is not run. The original
executable is required and stays untracked.
"""
import math
import re
import struct
import subprocess
from pathlib import Path
from app_binary import thin_armv7
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC)

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'original/v1.2/Payload/Flap.app'
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
HEAP, STACK, IMPORTS = 0x400000, 0x900000, 0xa00000
GAME_CTOR, ENGINE_INIT, FRAME, SET_TOUCHES = 0x10574, 0xd268, 0xdba0, 0xe8e4
GAME_GLOBAL, SEED_W, SEED_Z = 0xfd614, 0xfd61c, 0xfd620


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


class Original:
    def __init__(self, seed, best=0, plays=0, screen_width=240.0):
        data = thin_armv7(APP / 'flap')
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(0, 0x200000)
        uc.mem_map(HEAP, 0x400000)
        uc.mem_map(STACK, 0x100000)
        uc.mem_map(IMPORTS, 0x10000)
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        sections = {}
        pos = 28
        for _ in range(struct.unpack_from('<I', data, 16)[0]):
            cmd, size = struct.unpack_from('<II', data, pos)
            if cmd == 1:
                va, vs, off, length, _, _, nsects = struct.unpack_from('<7I', data, pos + 24)
                if length and va:
                    uc.mem_write(va, data[off:off + length])
                for s in range(nsects):
                    base = pos + 56 + 68 * s
                    name = data[base:base + 16].rstrip(b'\0').decode()
                    addr, ssize = struct.unpack_from('<II', data, base + 32)
                    reserved1 = struct.unpack_from('<I', data, base + 60)[0]
                    sections[name] = (addr, ssize, reserved1)
            elif cmd == 0xb:        # LC_DYSYMTAB
                indirect_off, indirect_n = struct.unpack_from('<II', data, pos + 56)
            elif cmd == 2:          # LC_SYMTAB
                symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, pos + 8)
            pos += size

        def symbol(index):
            strx = struct.unpack_from('<I', data, symoff + 12 * index)[0]
            end = data.index(b'\0', stroff + strx)
            return data[stroff + strx:end].decode()

        # Every imported function gets its own address in a page of "bx lr"; a hook there
        # does the work in Python. Imported data (the stack guard) points at a spare word.
        uc.mem_write(IMPORTS, struct.pack('<I', 0xe12fff1e) * 0x3000)
        self.imports = {}
        slot = 0x100
        for section in ('__lazy_symbol', '__nl_symbol_ptr'):
            addr, size, first = sections[section]
            for i in range(size // 4):
                index = struct.unpack_from('<I', data, indirect_off + 4 * (first + i))[0]
                if index & 0xc0000000:              # local or absolute: already a real address
                    continue
                name = symbol(index)
                if section == '__nl_symbol_ptr':
                    if struct.unpack('<I', uc.mem_read(addr + 4 * i, 4))[0] == 0:
                        uc.mem_write(addr + 4 * i, struct.pack('<I', IMPORTS + 0x8000))
                    continue
                target = IMPORTS + 4 * slot
                slot += 1
                self.imports[target] = name
                uc.mem_write(addr + 4 * i, struct.pack('<I', target))
        self.stop = IMPORTS + 0x40
        uc.hook_add(UC_HOOK_CODE, self.on_import, begin=IMPORTS, end=IMPORTS + 0x7fff)
        self.next = HEAP
        self.rand_state = 1
        self.draws = 0
        self.scene = []
        self.calls = {}

        atlas = (APP / 'atlas.txt').read_bytes()
        text = self.alloc(len(atlas) + 1)
        uc.mem_write(text, atlas)
        self.game = self.alloc(0x5d4)
        self.call(GAME_CTOR, self.game, best, plays, text, stack=[len(atlas)])
        uc.mem_write(GAME_GLOBAL, struct.pack('<I', self.game))
        # dot_EAGLView setup seeds both generators from the clock
        uc.mem_write(SEED_W, struct.pack('<i', seed % 32000))
        uc.mem_write(SEED_Z, struct.pack('<i', seed % 65535))
        self.call(ENGINE_INIT, self.game)
        # layoutSubviews would set the scale from the view; only the screen shake reads it
        uc.mem_write(self.game + 0x3a0, struct.pack('<f', screen_width / 288.0))
        self.touch_x = self.alloc(64)
        self.touch_y = self.alloc(64)
        self.down = False

    # ---- memory -------------------------------------------------------------------------
    def alloc(self, size):
        addr = self.next
        self.next += (size + 15) & ~15
        if self.next > HEAP + 0x400000:
            raise MemoryError('reference heap exhausted')
        return addr

    def word(self, addr):
        return struct.unpack('<I', self.uc.mem_read(addr, 4))[0]

    def int(self, addr):
        return struct.unpack('<i', self.uc.mem_read(addr, 4))[0]

    def float(self, addr):
        return struct.unpack('<f', self.uc.mem_read(addr, 4))[0]

    def byte(self, addr):
        return self.uc.mem_read(addr, 1)[0]

    def string(self, addr):
        out = bytearray()
        while True:
            c = self.uc.mem_read(addr, 1)[0]
            if not c:
                return out.decode('latin1')
            out.append(c)
            addr += 1

    # ---- calls --------------------------------------------------------------------------
    def call(self, address, *args, stack=()):
        uc = self.uc
        sp = STACK + 0xff000 - 4 * len(stack)
        if stack:
            uc.mem_write(sp, struct.pack('<%dI' % len(stack), *stack))
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, self.stop)
        for reg, value in zip(REGS, args):
            uc.reg_write(reg, value & 0xffffffff)
        uc.emu_start(address | 1, self.stop, count=20000000)
        if uc.reg_read(UC_ARM_REG_PC) != self.stop:
            raise RuntimeError('call to %x did not return' % address)
        return uc.reg_read(UC_ARM_REG_R0)

    def arg(self, i):
        return self.uc.reg_read(REGS[i])

    def sarg(self, i):
        v = self.arg(i)
        return v - (1 << 32) if v & 0x80000000 else v

    def stack_arg(self, i):
        return self.word(self.uc.reg_read(UC_ARM_REG_SP) + 4 * i)

    def ret(self, value=0, kind='I'):
        raw = struct.pack('<' + kind, value)
        for i in range(len(raw) // 4):
            self.uc.reg_write(REGS[i], struct.unpack_from('<I', raw, 4 * i)[0])

    def on_import(self, uc, address, size, user):
        if address == self.stop:
            return
        name = self.imports.get(address)
        if name is None:
            raise RuntimeError('jump to unknown import address %x' % address)
        self.calls[name] = self.calls.get(name, 0) + 1
        if name.startswith('_gl'):
            if name == '_glDrawElements':
                self.draws += 1
                if self.draws == 1:
                    self.scene = self.read_quads(self.arg(1) // 6)
            self.ret(0)
        elif name in ('_malloc', '__Znwm'):
            self.ret(self.alloc(self.arg(0)))
        elif name in ('_free', '__ZdlPv', '_printf', '_puts', '__ZNSt8ios_base4InitC1Ev', '___cxa_atexit'):
            self.ret(0)
        elif name == '_memset':
            uc.mem_write(self.arg(0), bytes([self.arg(1) & 255]) * self.arg(2))
            self.ret(self.arg(0))
        elif name == '_memcpy':
            uc.mem_write(self.arg(0), bytes(uc.mem_read(self.arg(1), self.arg(2))))
            self.ret(self.arg(0))
        elif name == '_strlen':
            self.ret(len(self.string(self.arg(0))))
        elif name == '_strcpy':
            uc.mem_write(self.arg(0), self.string(self.arg(1)).encode('latin1') + b'\0')
            self.ret(self.arg(0))
        elif name == '_sscanf':
            text, fmt = self.string(self.arg(0)), self.string(self.arg(1))
            if fmt == '%d':
                uc.mem_write(self.arg(2), struct.pack('<i', int(re.match(r'\s*(-?\d+)', text)[1])))
                self.ret(1)
            elif fmt == '%s %d %d %f %f %f %f':
                parts = text.split()
                out = [self.arg(2), self.arg(3)] + [self.stack_arg(i) for i in range(5)]
                uc.mem_write(out[0], parts[0].encode() + b'\0')
                uc.mem_write(out[1], struct.pack('<i', int(parts[1])))
                uc.mem_write(out[2], struct.pack('<i', int(parts[2])))
                for target, value in zip(out[3:], parts[3:7]):
                    uc.mem_write(target, struct.pack('<f', float(value)))
                self.ret(7)
            else:
                raise RuntimeError('unsupported sscanf format ' + fmt)
        elif name == '_vsprintf':
            fmt = self.string(self.arg(1))
            if fmt != '%u':
                raise RuntimeError('unsupported vsprintf format ' + fmt)
            text = b'%d' % self.word(self.arg(2))
            uc.mem_write(self.arg(0), text + b'\0')
            self.ret(len(text))
        elif name == '_rand':
            # Libc stdlib/FreeBSD/rand.c do_rand; the game never calls srand
            x = self.rand_state or 123459876
            hi, lo = x // 127773, x % 127773
            x = 16807 * lo - 2836 * hi
            if x < 0:
                x += 0x7fffffff
            self.rand_state = x
            self.ret(x % 0x80000000)
        elif name in ('_sinf', '_cosf'):
            x = struct.unpack('<f', struct.pack('<I', self.arg(0)))[0]
            self.ret(f32(math.sin(x) if name == '_sinf' else math.cos(x)), 'f')
        elif name == '_exp2':
            x = struct.unpack('<d', struct.pack('<II', self.arg(0), self.arg(1)))[0]
            self.ret(2.0 ** x, 'd')
        elif name == '_round':
            x = struct.unpack('<d', struct.pack('<II', self.arg(0), self.arg(1)))[0]
            self.ret(math.copysign(math.floor(abs(x) + 0.5), x), 'd')
        elif name in ('___modsi3', '___divsi3'):
            a, b = self.sarg(0), self.sarg(1)
            q = int(a / b)
            self.ret(a - q * b if name == '___modsi3' else q, 'i')
        else:
            raise RuntimeError('unsupported import %s' % name)

    # ---- the game -----------------------------------------------------------------------
    def read_quads(self, count):
        """The scene's quads as (u0, v0, corners top-left/top-right/bottom-right/bottom-left, alpha)."""
        g = self.game
        verts = struct.unpack('<%dh' % (8 * count), self.uc.mem_read(self.word(g + 0x50), 16 * count))
        uvs = struct.unpack('<%df' % (8 * count), self.uc.mem_read(self.word(g + 0x54), 32 * count))
        alphas = struct.unpack('<%df' % (4 * count), self.uc.mem_read(self.word(g + 0x58), 16 * count))
        out = []
        for i in range(count):
            v, t = verts[8 * i:8 * i + 8], uvs[8 * i:8 * i + 8]
            # stored bottom-left, bottom-right, top-right, top-left
            out.append(((t[6], t[7]), (v[6], v[7], v[4], v[5], v[2], v[3], v[0], v[1]), alphas[4 * i]))
        return out

    def frame(self, down=False, x=0, y=0):
        """One displayed frame; a finger going down is delivered first, as iOS does."""
        uc = self.uc
        if down:
            uc.mem_write(self.touch_x, struct.pack('<i', x))
            uc.mem_write(self.touch_y, struct.pack('<i', y))
            if not self.down:
                vtable = self.word(self.game)
                self.call(self.word(vtable + 0xc) & ~1, self.game, x, y)
        self.down = down
        self.call(SET_TOUCHES, self.game, self.touch_x, self.touch_y, 1 if down else 0)
        self.draws = 0
        self.scene = []
        self.call(FRAME, self.game)

    def events(self):
        g = self.game
        out = []
        for i in range(self.int(g + 0x444)):
            value = struct.unpack('<d', self.uc.mem_read(g + 0x498 + 8 * i, 8))[0]
            out.append((self.int(g + 0x448 + 4 * i), int(value)))
        return out

    def state(self):
        g = self.game
        b = self.word(g + 0x554)
        floats = (self.float(b + 0x60), self.float(b + 0x64), self.float(b + 0x70), self.float(b + 0x74),
                  self.float(b + 0x80), self.float(self.word(g + 0x384) + 0xc), self.float(self.word(g + 0x38c) + 0xc))
        ints = [self.int(b + 0x30), self.int(b + 0x34), self.byte(b + 0x78), self.byte(b + 0x79), self.int(b + 0x7c),
                self.int(b + 0x84), self.int(g + 0x538), self.int(g + 0x53c), self.int(g + 0x540), self.int(g + 0x5d0),
                self.int(g + 0x5c0), self.int(g + 0x5a0)]
        ints += [self.int(g + 0x5b0 + 4 * i) for i in range(3)]
        ints += [self.int(g + 0x5a4 + 4 * i) for i in range(3)]
        ints += [self.int(g + 0x390), self.int(g + 0x394), self.byte(g + 0x550),
                 int(self.word(g + 0x57c) == self.word(g + 0x584)), self.int(SEED_Z), self.int(SEED_W)]
        return floats, ints


if __name__ == '__main__':
    vm = Original(12345)
    for _ in range(3):
        vm.frame()
    print('state after 3 frames:', vm.state())
    print('%d scene quads; imports used: %s' % (len(vm.scene), ', '.join(sorted(vm.calls))))
