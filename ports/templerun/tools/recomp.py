#!/usr/bin/env python3
"""Translate the engine's ARM machine code into C, one C function per original function.

The game's C++ engine in the Temple Run 1.0 executable is ARM-mode code with full symbols.
This reads the functions the engine needs, follows each one's control flow, and writes the
same instructions as C statements working on local register variables and on the guest
memory image (port/src/rt.h). Calls to iOS and C++ library functions become calls to the
port's own runtime; the Objective-C half of the app is left out.

The output is derived from the original executable, so it is generated at build time and
never committed.

usage: recomp.py <out directory>
"""
import re
import struct
import subprocess
import sys
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from capstone.arm import *

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'build/TempleRun-armv7'
BASE = 0x1000

# Engine functions the port supplies itself (the same ones the reference harness replaces).
OVERRIDE_PREFIXES = ('__ZN13cSoundManager', '__ZNK13cSoundManager')
OVERRIDES = {
    '__ZN12ImangiConfig12getBoolValueERKSs', '__ZN12ImangiConfig13getFloatValueERKSs',
    '__ZN12ImangiConfig11getIntValueERKSs', '__ZN17ImangiPreferences17isTutorialEnabledEv',
    '__ZN17ImangiPreferences20setIsTutorialEnabledEb', '__ZN17ImangiPreferences15savePreferencesEv',
    '__Z24getPathForImangiResourceRKSsb', '__ZN15cTextureManager12getTextureIdESs',
    '__ZN15cTextureManager14loadPVRTextureERKSsS1_b', '__ZN15cTextureManager11loadTextureERKSsS1_bbb',
}
NORETURN = {'__ZSt20__throw_out_of_rangePKc', '__ZSt17__throw_bad_allocv', '__ZSt20__throw_length_errorPKc', '_abort',
            '__Unwind_SjLj_Resume', '___cxa_rethrow', '__ZSt9terminatev', '_exit', '___cxa_throw',
            '___cxa_call_unexpected', '___stack_chk_fail', '___cxa_bad_cast', '___cxa_pure_virtual'}
ROOT_PATTERNS = ('__ZN15cGameController', '__ZNK15cGameController')

CC = {ARM_CC_EQ: 'z', ARM_CC_NE: '!z', ARM_CC_HS: 'c', ARM_CC_LO: '!c', ARM_CC_MI: 'n', ARM_CC_PL: '!n',
      ARM_CC_VS: 'v', ARM_CC_VC: '!v', ARM_CC_HI: '(c && !z)', ARM_CC_LS: '(!c || z)', ARM_CC_GE: '(n == v)',
      ARM_CC_LT: '(n != v)', ARM_CC_GT: '(!z && n == v)', ARM_CC_LE: '(z || n != v)'}


class Unsupported(Exception):
    pass


class Image:
    def __init__(self):
        data = self.data = BIN.read_bytes()
        self.sections = {}
        pos = 28
        for _ in range(struct.unpack_from('<I', data, 16)[0]):
            cmd, size = struct.unpack_from('<II', data, pos)
            if cmd == 1:
                nsects = struct.unpack_from('<I', data, pos + 48)[0]
                seg = data[pos + 8:pos + 24].rstrip(b'\0').decode()
                for s in range(nsects):
                    base = pos + 56 + 68 * s
                    name = data[base:base + 16].rstrip(b'\0').decode()
                    addr, ssize = struct.unpack_from('<II', data, base + 32)
                    self.sections[seg + ',' + name] = (addr, ssize, struct.unpack_from('<I', data, base + 60)[0])
            elif cmd == 0xb:
                indirect_off = struct.unpack_from('<I', data, pos + 56)[0]
            elif cmd == 2:
                symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, pos + 8)
            pos += size

        def name_at(index):
            strx = struct.unpack_from('<I', data, symoff + 12 * index)[0]
            return data[stroff + strx:data.index(b'\0', stroff + strx)].decode()

        self.text = self.sections['__TEXT,__text'][:2]
        lo, size = self.text
        hi = lo + size
        funcs = {}
        for i in range(nsyms):
            strx, ntype, _, _, value = struct.unpack_from('<IBBHI', data, symoff + 12 * i)
            if ntype & 0xe0 or ntype & 0x0e != 0x0e or not lo <= value < hi:
                continue
            name = data[stroff + strx:data.index(b'\0', stroff + strx)].decode()
            if value not in funcs or name.startswith('__Z'):
                funcs[value] = name
        self.funcs = funcs
        starts = sorted(funcs)
        self.end = {a: b for a, b in zip(starts, starts[1:] + [hi])}
        self.by_name = {n: a for a, n in funcs.items()}
        # symbol stubs: one 12-byte stub per imported function
        addr, size, first = self.sections['__TEXT,__symbol_stub4']
        self.stubs = {}
        for i in range(size // 12):
            self.stubs[addr + 12 * i] = name_at(struct.unpack_from('<I', data, indirect_off + 4 * (first + i))[0])
        self.nonlazy = {}
        addr, size, first = self.sections['__DATA,__nl_symbol_ptr']
        for i in range(size // 4):
            index = struct.unpack_from('<I', data, indirect_off + 4 * (first + i))[0]
            if not index & 0xc0000000:
                self.nonlazy[addr + 4 * i] = name_at(index)

    def word(self, va):
        return struct.unpack_from('<I', self.data, va - BASE)[0]

    def in_text(self, va):
        return self.text[0] <= va < self.text[0] + self.text[1]

    def is_objc(self, name):
        return '[' in name or name.startswith(('___', '_OBJC', '__GLOBAL__I_OBJC'))


# Engine functions that tell the port they are starting (the function itself still runs).
NOTIFY = {'__ZN7cMesh3D25createVertexBufferObjectsEv': 'rt_note_mesh'}
# Two distances the engine gives in points of the phone's 320-wide screen without applying
# its own display scale (cGameController::handleBatching places the "250m" text 6 left of and
# 55 below the board it hangs on). After the instruction at each address the register is
# multiplied by rt_display_scale: 1 on the phone and in the tests, so the translation is still
# checked exactly as the original; the port sets it to its panel's scale.
DISPLAY_OFFSETS = {0x1dcac: 'd3', 0x1dcd4: 'd1'}
CALLEE_SAVED = {'r4', 'r5', 'r6', 'r7', 'r8', 'r10', 'r11', 'lr'}
LITERALS_BASE = 0xf4000
LITERALS = {}           # original address -> (new address, bytes)


def literal_address(im, target, end):
    if target not in LITERALS:
        offset = sum(len(b) for _, b in LITERALS.values())
        LITERALS[target] = (LITERALS_BASE + offset, im.data[target - BASE:end - BASE])
    return LITERALS[target][0]


def is_s(name):
    return bool(name) and re.fullmatch(r's\d+', name) is not None


def is_d(name):
    return bool(name) and re.fullmatch(r'd\d+', name) is not None


def creg(name):
    return {'sb': 'r9', 'sl': 'r10', 'fp': 'r11', 'ip': 'r12'}.get(name, name)


class Function:
    def __init__(self, image, md, addr):
        self.im, self.md, self.addr = image, md, addr
        self.end = image.end[addr]
        self.name = image.funcs[addr]
        self.ins = {}
        self.leaders = {addr}
        self.calls = set()
        self.switches = {}
        self.never = set()          # conditional branches that are never taken (setjmp dispatch)
        self.table_loads = {}
        self.used = set()
        self.decode()

    def decode(self):
        im, work = self.im, [self.addr]
        while work:
            pc = work.pop()
            while self.addr <= pc < self.end and pc not in self.ins:
                ins = next(self.md.disasm(im.data[pc - BASE:pc - BASE + 4], pc), None)
                if ins is None:
                    raise Unsupported('%s: cannot decode %08x at %x' % (self.name, im.word(pc), pc))
                self.ins[pc] = ins
                cond = ins.cc not in (ARM_CC_AL, ARM_CC_INVALID)
                if ins.id == ARM_INS_B:
                    target = ins.operands[0].imm
                    if pc in self.never:
                        pc += 4
                        continue
                    if self.addr <= target < self.end:
                        self.leaders.add(target)
                        work.append(target)
                    else:
                        self.calls.add(target)
                    if not cond:
                        break
                    self.leaders.add(pc + 4)
                elif ins.id == ARM_INS_BL or (ins.id == ARM_INS_BLX and ins.operands[0].type == ARM_OP_IMM):
                    target = ins.operands[0].imm
                    self.calls.add(target)
                    if im.stubs.get(target) in NORETURN and not cond:
                        break
                elif ins.id == ARM_INS_BX:
                    if not cond:
                        break
                elif self.writes_pc(ins):
                    if ins.id == ARM_INS_ADD and ins.operands[1].type == ARM_OP_REG and ins.operands[1].reg == ARM_REG_PC \
                            and ins.operands[2].type == ARM_OP_IMM:
                        target = pc + 8 + ins.operands[2].imm        # the setjmp idiom: skip ahead
                        self.leaders.add(target)
                        work.append(target)
                        nxt = next(self.md.disasm(im.data[target + 4 - BASE:target + 8 - BASE], target + 4), None)
                        if ins.operands[2].imm == 0 and nxt is not None and nxt.id == ARM_INS_B and nxt.cc == ARM_CC_NE:
                            self.never.add(target + 4)
                        self.switches[pc] = [target]
                    elif ins.id == ARM_INS_ADD:
                        targets = self.switch_targets(pc)
                        self.switches[pc] = targets
                        for t in targets:
                            self.leaders.add(t)
                            work.append(t)
                    if not cond:
                        break
                pc += 4
            if not self.addr <= pc < self.end and pc not in self.ins and pc == self.end:
                raise Unsupported('%s: runs off its end at %x' % (self.name, pc))

    def writes_pc(self, ins):
        if ins.id in (ARM_INS_POP, ARM_INS_LDM, ARM_INS_LDMIB, ARM_INS_LDMDA, ARM_INS_LDMDB):
            return any(o.type == ARM_OP_REG and o.reg == ARM_REG_PC for o in ins.operands[(0 if ins.id == ARM_INS_POP else 1):])
        if ins.id in (ARM_INS_CMP, ARM_INS_CMN, ARM_INS_TST, ARM_INS_TEQ, ARM_INS_PUSH, ARM_INS_STR, ARM_INS_STM,
                      ARM_INS_STMIB, ARM_INS_STMDB, ARM_INS_STMDA, ARM_INS_STRB, ARM_INS_STRH, ARM_INS_STRD):
            return False
        return bool(ins.operands) and ins.operands[0].type == ARM_OP_REG and ins.operands[0].reg == ARM_REG_PC

    def switch_targets(self, pc):
        """add pc, rA, rB: the offsets are in a table right after, bounded by a compare."""
        base = count = None
        back = pc - 4
        while back >= self.addr and back >= pc - 40 and (base is None or count is None):
            ins = self.ins.get(back)
            if ins is None:
                break
            if base is None and ins.id == ARM_INS_ADD and len(ins.operands) == 3 and ins.operands[1].type == ARM_OP_REG \
                    and ins.operands[1].reg == ARM_REG_PC and ins.operands[2].type == ARM_OP_IMM:
                base = back + 8 + ins.operands[2].imm
            if count is None and ins.id == ARM_INS_CMP and ins.operands[1].type == ARM_OP_IMM:
                count = ins.operands[1].imm + 1
            back -= 4
        if base is None or count is None or base != pc + 4:
            raise Unsupported('%s: unrecognised computed jump at %x' % (self.name, pc))
        # The offsets are read from the table in the code section, which the port does not
        # keep in memory: the load just before becomes a lookup in a constant array.
        load = self.ins.get(pc - 4)
        if load is None or load.id != ARM_INS_LDR or not load.operands[1].mem.index \
                or load.operands[1].shift.type != ARM_SFT_LSL or load.operands[1].shift.value != 2:
            raise Unsupported('%s: computed jump at %x without the usual table load' % (self.name, pc))
        self.table_loads[pc - 4] = (base, [self.im.word(base + 4 * i) for i in range(count)])
        targets = []
        for i in range(count):
            t = (base + self.im.word(base + 4 * i)) & 0xffffffff
            if not self.addr <= t < self.end:
                raise Unsupported('%s: switch target %x outside function at %x' % (self.name, t, pc))
            targets.append(t)
        return targets

    def is_literal(self, target):
        """An address inside this function's range that is data, not code or a switch table."""
        return self.addr <= target < self.end and target not in self.ins \
            and not any(base == target for base, _ in self.table_loads.values())

    def literal_end(self, target):
        end = target
        while end < self.end and end not in self.ins and end < target + 64:
            end += 4
        return end

    # ---- operands -----------------------------------------------------------------------
    def reg(self, r, pc):
        name = creg(self.md.reg_name(r))
        if name == 'pc':
            return '0x%xu' % (pc + 8)
        self.used.add(name)
        return name

    def vf(self, r):
        """The C lvalue (as float) for an S register, or a tuple for D/Q."""
        name = self.md.reg_name(r)
        return name

    def sreg(self, name, kind='f'):
        n = int(name[1:])
        self.used.add('d%d' % (n >> 1))
        return 'd%d.%s[%d]' % (n >> 1, kind, n & 1)

    def dreg(self, name, kind='d'):
        self.used.add(name)
        return '%s.%s' % (name, kind)

    def shifted(self, ins, op, pc, carry=False):
        """C expression for a register operand with its shift; optionally the carry out."""
        value = self.reg(op.reg, pc)
        t, n = op.shift.type, op.shift.value
        if t == ARM_SFT_INVALID or (t == ARM_SFT_LSL and n == 0):
            return (value, None) if carry else value
        if t in (ARM_SFT_LSL, ARM_SFT_LSR, ARM_SFT_ASR, ARM_SFT_ROR):
            if t == ARM_SFT_LSL:
                e, co = '(%s << %d)' % (value, n), '((%s >> %d) & 1)' % (value, 32 - n)
            elif t == ARM_SFT_LSR:
                e = '(%s >> %d)' % (value, n) if n < 32 else '0u'
                co = '((%s >> %d) & 1)' % (value, n - 1)
            elif t == ARM_SFT_ASR:
                e = '((uint32_t)((int32_t)%s >> %d))' % (value, min(n, 31))
                co = '((%s >> %d) & 1)' % (value, min(n, 32) - 1)
            else:
                e = '((%s >> %d) | (%s << %d))' % (value, n, value, 32 - n)
                co = '((%s >> %d) & 1)' % (value, n - 1)
            return (e, co) if carry else e
        if t in (ARM_SFT_LSL_REG, ARM_SFT_LSR_REG, ARM_SFT_ASR_REG):
            amount = self.reg(n, pc)
            fn = {ARM_SFT_LSL_REG: 'lsl_reg', ARM_SFT_LSR_REG: 'lsr_reg', ARM_SFT_ASR_REG: 'asr_reg'}[t]
            e = '%s(%s, %s)' % (fn, value, amount)
            return (e, '%s_c(%s, %s, c)' % (fn, value, amount)) if carry else e
        raise Unsupported('shift type %d' % t)

    def operand2(self, ins, op, pc, carry=False):
        if op.type == ARM_OP_IMM:
            e = '0x%xu' % (op.imm & 0xffffffff)
            return (e, None) if carry else e
        return self.shifted(ins, op, pc, carry)

    def address(self, ins, op, pc):
        """(address expression, base register name or None)"""
        m = op.mem
        base = self.reg(m.base, pc)
        if m.index:
            index = self.reg(m.index, pc)
            t, n = op.shift.type, op.shift.value
            if t == ARM_SFT_LSL and n:
                index = '(%s << %d)' % (index, n)
            elif t == ARM_SFT_ASR and n:
                index = '((uint32_t)((int32_t)%s >> %d))' % (index, n)
            elif t == ARM_SFT_LSR and n:
                index = '(%s >> %d)' % (index, n)
            elif t not in (ARM_SFT_INVALID, ARM_SFT_LSL):
                raise Unsupported('index shift %d' % t)
            sign = '-' if (op.subtracted or m.scale == -1) else '+'
            return '(%s %s %s)' % (base, sign, index), base
        if m.disp:
            return '(%s + 0x%xu)' % (base, m.disp & 0xffffffff), base
        return base, base

    # ---- translation --------------------------------------------------------------------
    def flags_nz(self, e):
        return 'n = (%s) >> 31; z = (%s) == 0;' % (e, e)

    def translate(self, pc, ins):
        im = self.im
        o = ins.operands
        m = ins.id
        out = []
        R = lambda i: self.reg(o[i].reg, pc)
        S = ins.update_flags

        def set_reg(i, expr):
            if o[i].reg == ARM_REG_PC:
                raise Unsupported('write to pc')
            return '%s = %s;' % (R(i), expr)

        if m in (ARM_INS_MOV, ARM_INS_MVN, ARM_INS_MOVW):
            if o[0].reg == ARM_REG_PC:
                raise Unsupported('mov pc')
            e, co = self.operand2(ins, o[1], pc, True)
            if m == ARM_INS_MVN:
                e = '~%s' % e
            out.append(set_reg(0, e))
            if S:
                out.append(self.flags_nz(R(0)))
                if co:
                    out.append('c = %s;' % co)
        elif m == ARM_INS_MOVT:
            out.append('%s = (%s & 0xffffu) | 0x%xu;' % (R(0), R(0), (o[1].imm & 0xffff) << 16))
        elif m in (ARM_INS_LSL, ARM_INS_LSR, ARM_INS_ASR, ARM_INS_ROR):
            src = R(1)
            if len(o) == 2:
                e, co = self.shifted(ins, o[1], pc, True)
                if S:
                    out.append('{ uint32_t t = %s; c = %s; %s = t; %s }' % (e, co, R(0), self.flags_nz('t')))
                else:
                    out.append(set_reg(0, e))
            elif o[2].type == ARM_OP_IMM:
                n = o[2].imm
                if m == ARM_INS_LSL:
                    e, co = '%s << %d' % (src, n), '(%s >> %d) & 1' % (src, 32 - n)
                elif m == ARM_INS_LSR:
                    e, co = '%s >> %d' % (src, n), '(%s >> %d) & 1' % (src, n - 1)
                elif m == ARM_INS_ASR:
                    e, co = '(uint32_t)((int32_t)%s >> %d)' % (src, n), '(%s >> %d) & 1' % (src, n - 1)
                else:
                    e, co = '(%s >> %d) | (%s << %d)' % (src, n, src, 32 - n), '(%s >> %d) & 1' % (src, n - 1)
                if S:
                    out.append('{ uint32_t t = %s; c = %s; %s = t; %s }' % (e, co, R(0), self.flags_nz('t')))
                else:
                    out.append(set_reg(0, e))
            else:
                if S:
                    raise Unsupported('flag-setting register shift')
                fn = {ARM_INS_LSL: 'lsl_reg', ARM_INS_LSR: 'lsr_reg', ARM_INS_ASR: 'asr_reg'}[m]
                out.append(set_reg(0, '%s(%s, %s)' % (fn, src, R(2))))
        elif m == ARM_INS_ADD and len(o) == 3 and o[1].type == ARM_OP_REG and o[1].reg == ARM_REG_PC \
                and o[2].type == ARM_OP_IMM and o[0].reg != ARM_REG_PC and self.is_literal(pc + 8 + o[2].imm):
            # The address of constants stored among the code. The port keeps no code in memory,
            # so the constants are copied to a table of their own and the address points there.
            out.append(set_reg(0, '0x%xu' % literal_address(im, pc + 8 + o[2].imm, self.literal_end(pc + 8 + o[2].imm))))
        elif m in (ARM_INS_ADD, ARM_INS_SUB, ARM_INS_RSB, ARM_INS_ADC, ARM_INS_SBC, ARM_INS_RSC, ARM_INS_AND,
                   ARM_INS_ORR, ARM_INS_EOR, ARM_INS_BIC):
            if len(o) == 2:
                a, (b, co) = R(0), self.operand2(ins, o[1], pc, True)
            else:
                a, (b, co) = R(1), self.operand2(ins, o[2], pc, True)
            if o[0].reg == ARM_REG_PC:
                targets = self.switches[pc]
                if len(targets) == 1 and m == ARM_INS_ADD and o[1].reg == ARM_REG_PC:
                    out.append('goto L_%x;' % targets[0])
                else:
                    cases = ' '.join('case 0x%xu: goto L_%x;' % (t, t) for t in sorted(set(targets)))
                    out.append('switch ((uint32_t)(%s + %s)) { %s default: rt_bad_jump(0x%xu); }' % (a, b, cases, pc))
                return out
            if m in (ARM_INS_AND, ARM_INS_ORR, ARM_INS_EOR, ARM_INS_BIC):
                op = {ARM_INS_AND: '%s & %s', ARM_INS_ORR: '%s | %s', ARM_INS_EOR: '%s ^ %s', ARM_INS_BIC: '%s & ~%s'}[m]
                out.append(set_reg(0, op % (a, b)))
                if S:
                    out.append(self.flags_nz(R(0)))
                    if co:
                        out.append('c = %s;' % co)
            elif not S:
                e = {ARM_INS_ADD: '%s + %s', ARM_INS_SUB: '%s - %s', ARM_INS_RSB: '%s - %s', ARM_INS_ADC: '%s + %s + c',
                     ARM_INS_SBC: '%s - %s - !c', ARM_INS_RSC: '%s - %s - !c'}[m]
                if m in (ARM_INS_RSB, ARM_INS_RSC):
                    a, b = b, a
                out.append(set_reg(0, e % (a, b)))
            else:
                if m in (ARM_INS_RSB, ARM_INS_RSC):
                    a, b = b, a
                if m == ARM_INS_ADD:
                    out.append('{ uint32_t a = %s, b = %s, t = a + b; c = t < a; v = (~(a ^ b) & (a ^ t)) >> 31; %s = t; %s }'
                               % (a, b, R(0), self.flags_nz('t')))
                elif m in (ARM_INS_SUB, ARM_INS_RSB):
                    out.append('{ uint32_t a = %s, b = %s, t = a - b; c = a >= b; v = ((a ^ b) & (a ^ t)) >> 31; %s = t; %s }'
                               % (a, b, R(0), self.flags_nz('t')))
                elif m == ARM_INS_ADC:
                    out.append('{ uint32_t a = %s, b = %s; uint64_t w = (uint64_t)a + b + c; uint32_t t = (uint32_t)w; '
                               'c = (uint32_t)(w >> 32); v = (~(a ^ b) & (a ^ t)) >> 31; %s = t; %s }'
                               % (a, b, R(0), self.flags_nz('t')))
                else:
                    out.append('{ uint32_t a = %s, b = %s; uint64_t w = (uint64_t)a - b - !c; uint32_t t = (uint32_t)w; '
                               'c = !(w >> 63); v = ((a ^ b) & (a ^ t)) >> 31; %s = t; %s }'
                               % (a, b, R(0), self.flags_nz('t')))
        elif m in (ARM_INS_CMP, ARM_INS_CMN, ARM_INS_TST, ARM_INS_TEQ):
            a, (b, co) = R(0), self.operand2(ins, o[1], pc, True)
            if m == ARM_INS_CMP:
                out.append('{ uint32_t a = %s, b = %s, t = a - b; c = a >= b; v = ((a ^ b) & (a ^ t)) >> 31; %s }'
                           % (a, b, self.flags_nz('t')))
            elif m == ARM_INS_CMN:
                out.append('{ uint32_t a = %s, b = %s, t = a + b; c = t < a; v = (~(a ^ b) & (a ^ t)) >> 31; %s }'
                           % (a, b, self.flags_nz('t')))
            else:
                out.append('{ uint32_t t = %s %s %s; %s%s }' % (a, '&' if m == ARM_INS_TST else '^', b, self.flags_nz('t'),
                                                              ' c = %s;' % co if co else ''))
        elif m == ARM_INS_MUL:
            a, b = (R(0), R(1)) if len(o) == 2 else (R(1), R(2))
            out.append(set_reg(0, '%s * %s' % (a, b)))
            if S:
                out.append(self.flags_nz(R(0)))
        elif m == ARM_INS_MLA:
            out.append(set_reg(0, '%s * %s + %s' % (R(1), R(2), R(3))))
        elif m == ARM_INS_MLS:
            out.append(set_reg(0, '%s - %s * %s' % (R(3), R(1), R(2))))
        elif m in (ARM_INS_UMULL, ARM_INS_SMULL):
            cast = '(uint64_t)' if m == ARM_INS_UMULL else '(int64_t)(int32_t)'
            out.append('{ uint64_t w = (uint64_t)(%s%s * %s%s); %s = (uint32_t)w; %s = (uint32_t)(w >> 32); }'
                       % (cast, R(2), cast, R(3), R(0), R(1)))
        elif m == ARM_INS_SMMUL:
            out.append(set_reg(0, '(uint32_t)(((int64_t)(int32_t)%s * (int32_t)%s) >> 32)' % (R(1), R(2))))
        elif m == ARM_INS_SMMLA:
            out.append(set_reg(0, '(uint32_t)((((int64_t)(int32_t)%s << 32) + (int64_t)(int32_t)%s * (int32_t)%s) >> 32)'
                               % (R(3), R(1), R(2))))
        elif m == ARM_INS_UXTB:
            out.append(set_reg(0, '%s & 0xffu' % R(1)))
        elif m == ARM_INS_UXTH:
            out.append(set_reg(0, '%s & 0xffffu' % R(1)))
        elif m == ARM_INS_SXTB:
            out.append(set_reg(0, '(uint32_t)(int8_t)%s' % R(1)))
        elif m == ARM_INS_SXTH:
            out.append(set_reg(0, '(uint32_t)(int16_t)%s' % R(1)))
        elif m == ARM_INS_UBFX:
            out.append(set_reg(0, '(%s >> %d) & 0x%xu' % (R(1), o[2].imm, (1 << o[3].imm) - 1)))
        elif m == ARM_INS_SBFX:
            out.append(set_reg(0, '(uint32_t)((int32_t)(%s << %d) >> %d)' % (R(1), 32 - o[2].imm - o[3].imm, 32 - o[3].imm)))
        elif m == ARM_INS_BFC:
            mask = ((1 << o[2].imm) - 1) << o[1].imm
            out.append('%s &= 0x%xu;' % (R(0), ~mask & 0xffffffff))
        elif m == ARM_INS_BFI:
            mask = ((1 << o[3].imm) - 1) << o[2].imm
            out.append('%s = (%s & 0x%xu) | ((%s << %d) & 0x%xu);' % (R(0), R(0), ~mask & 0xffffffff, R(1), o[2].imm, mask))
        elif m == ARM_INS_CLZ:
            out.append(set_reg(0, 'clz32(%s)' % R(1)))
        elif m in (ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRH, ARM_INS_LDRSB, ARM_INS_LDRSH, ARM_INS_STR, ARM_INS_STRB,
                   ARM_INS_STRH):
            mem = o[1]
            if m == ARM_INS_LDR and mem.mem.base == ARM_REG_PC and not mem.mem.index:
                out.append(set_reg(0, '0x%xu' % im.word(pc + 8 + mem.mem.disp)))
                return out
            if pc in self.table_loads:
                base, values = self.table_loads[pc]
                self.tables.append('static const uint32_t T_%x[%d] = { %s };' % (base, len(values), ', '.join('0x%xu' % x for x in values)))
                index = self.reg(mem.mem.index, pc)
                out.append(set_reg(0, '%s < %du ? T_%x[%s] : 0' % (index, len(values), base, index)))
                return out
            addr, base = self.address(ins, mem, pc)
            post = ins.post_index if hasattr(ins, 'post_index') else False
            if len(o) == 3:            # post-indexed with a separate offset operand
                post = True
                step = self.operand2(ins, o[2], pc)
                if o[2].subtracted:
                    step = '-%s' % step
                ea, update = base, '%s += %s;' % (base, step)
            elif post:
                ea, update = base, '%s = %s;' % (base, addr)
            elif ins.writeback:
                ea, update = addr, '%s = %s;' % (base, addr)
            else:
                ea, update = addr, ''
            load = {ARM_INS_LDR: 'M32(%s)', ARM_INS_LDRB: 'M8(%s)', ARM_INS_LDRH: 'M16(%s)',
                    ARM_INS_LDRSB: '(uint32_t)(int8_t)M8(%s)', ARM_INS_LDRSH: '(uint32_t)(int16_t)M16(%s)'}.get(m)
            if load:
                if o[0].reg == ARM_REG_PC:
                    raise Unsupported('ldr pc')
                if update:
                    out.append('{ uint32_t ea = %s; %s %s = %s; }' % (ea, update, R(0), load % 'ea'))
                else:
                    out.append(set_reg(0, load % ea))
            else:
                store = {ARM_INS_STR: 'M32(%s) = %s;', ARM_INS_STRB: 'M8(%s) = (uint8_t)%s;',
                         ARM_INS_STRH: 'M16(%s) = (uint16_t)%s;'}[m]
                if update:
                    out.append('{ uint32_t ea = %s; uint32_t t = %s; %s %s }' % (ea, R(0), update, store % ('ea', 't')))
                else:
                    out.append(store % (ea, R(0)))
        elif m in (ARM_INS_LDRD, ARM_INS_STRD):
            addr, base = self.address(ins, o[2], pc)
            if ins.writeback or len(o) > 3:
                raise Unsupported('ldrd/strd writeback')
            if m == ARM_INS_LDRD:
                if o[2].mem.base == ARM_REG_PC:
                    a = pc + 8 + o[2].mem.disp
                    out.append('%s = 0x%xu; %s = 0x%xu;' % (R(0), im.word(a), R(1), im.word(a + 4)))
                else:
                    out.append('{ uint32_t ea = %s; %s = M32(ea); %s = M32(ea + 4); }' % (addr, R(0), R(1)))
            else:
                out.append('{ uint32_t ea = %s; M32(ea) = %s; M32(ea + 4) = %s; }' % (addr, R(0), R(1)))
        elif m in (ARM_INS_PUSH, ARM_INS_POP, ARM_INS_LDM, ARM_INS_LDMIB, ARM_INS_LDMDA, ARM_INS_LDMDB, ARM_INS_STM,
                   ARM_INS_STMIB, ARM_INS_STMDA, ARM_INS_STMDB):
            if m in (ARM_INS_PUSH, ARM_INS_POP):
                base, regs, wb = 'sp', o, True
                self.used.add('sp')
            else:
                base, regs, wb = R(0), o[1:], ins.writeback
            names = [creg(self.md.reg_name(x.reg)) for x in regs]
            count = len(names)
            load = m in (ARM_INS_POP, ARM_INS_LDM, ARM_INS_LDMIB, ARM_INS_LDMDA, ARM_INS_LDMDB)
            if m in (ARM_INS_PUSH, ARM_INS_STMDB, ARM_INS_LDMDB):
                start, final = '%s - %d' % (base, 4 * count), '%s - %d' % (base, 4 * count)
            elif m in (ARM_INS_POP, ARM_INS_LDM, ARM_INS_STM):
                start, final = base, '%s + %d' % (base, 4 * count)
            elif m in (ARM_INS_LDMIB, ARM_INS_STMIB):
                start, final = '%s + 4' % base, '%s + %d' % (base, 4 * count)
            else:
                start, final = '%s - %d' % (base, 4 * count - 4), '%s - %d' % (base, 4 * count)
            body = ['uint32_t ea = %s;' % start]
            ret = False
            for i, name in enumerate(names):
                if name == 'pc':
                    if not load:
                        raise Unsupported('store of pc')
                    ret = True
                    continue
                # The registers a function must preserve are saved on entry and restored on exit.
                # Here they are C locals of the caller, so the save and the restore have nothing to
                # do: only the stack pointer moves.
                if m in (ARM_INS_PUSH, ARM_INS_POP) and name in CALLEE_SAVED:
                    continue
                self.used.add(name)
                if load:
                    body.append('%s = M32(ea + %d);' % (name, 4 * i))
                else:
                    body.append('M32(ea + %d) = %s;' % (4 * i, name))
            if wb and not (load and base in names):
                body.append('%s = %s;' % (base, final.replace(base, 'ea_base') if False else final))
            # writeback uses the original base value: compute before loads when the base is not reloaded
            if wb and not (load and base in names):
                body = ['uint32_t ea = %s;' % start, 'uint32_t wb = %s;' % final] + body[1:-1] + ['%s = wb;' % base]
            out.append('{ %s }' % ' '.join(body))
            if ret:
                out.append('RETURN();')
        elif m == ARM_INS_B:
            target = o[0].imm
            if pc in self.never:
                return ['/* setjmp dispatch: never taken */']
            if self.addr <= target < self.end:
                out.append('goto L_%x;' % target)
            else:
                out.append(self.tail_call(target, pc))
        elif m == ARM_INS_BL or (m == ARM_INS_BLX and o[0].type == ARM_OP_IMM):
            out.append(self.call(o[0].imm, pc))
        elif m == ARM_INS_BLX:
            out.append('CALL(rt_call(%s, r0, r1, r2, r3));' % R(0))
        elif m == ARM_INS_BX:
            if creg(self.md.reg_name(o[0].reg)) == 'lr':
                out.append('RETURN();')
            else:
                out.append('return rt_call(%s, r0, r1, r2, r3);' % R(0))
        elif ins.mnemonic.startswith('v'):
            out += self.vfp(pc, ins)
        elif m == ARM_INS_NOP:
            pass
        else:
            raise Unsupported('instruction %s %s' % (ins.mnemonic, ins.op_str))
        return out

    def tail_call(self, target, pc):
        text = self.call(target, pc)
        if text.startswith('CALL(') and text.endswith(');'):
            return 'return %s;' % text[5:-2]
        return text + ' RETURN();'

    def call(self, target, pc):
        im = self.im
        if target in im.stubs:
            name = im.stubs[target]
            IMPORTS.add(name)
            if name in NORETURN:
                return 'CALL_IMP(imp%s); return 0;' % name
            return 'CALL_IMP(imp%s);' % name
        name = im.funcs.get(target)
        if name is None:
            raise Unsupported('call into the middle of a function: %x' % target)
        if name in OVERRIDES or name.startswith(OVERRIDE_PREFIXES):
            USED_OVERRIDES.add(name)
            return 'CALL_IMP(ovr%s);' % name
        if target not in WANTED:
            MISSING.add(name)
            return 'rt_missing("%s");' % name
        return 'CALL(f_%x(r0, r1, r2, r3));' % target

    # ---- floating point -----------------------------------------------------------------
    def vfp(self, pc, ins):
        im, o, m = self.im, ins.operands, ins.id
        mn = ins.mnemonic
        base_mn = re.sub(r'(eq|ne|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(?=$|\.)', '', mn) if ins.cc not in (ARM_CC_AL, ARM_CC_INVALID) else mn
        suffix = base_mn.split('.', 1)[1] if '.' in base_mn else ''
        m = VFP_NAMES.get(base_mn.split('.')[0])
        if m is None:
            raise Unsupported('%s %s' % (mn, ins.op_str))
        names = [self.md.reg_name(x.reg) if x.type == ARM_OP_REG else None for x in o]
        out = []

        def fs(i):
            return self.sreg(names[i], 'f')

        def us(i):
            return self.sreg(names[i], 'u')

        if m in (ARM_INS_VLDR, ARM_INS_VSTR):
            mem = o[1].mem
            disp = mem.disp if not o[1].subtracted else -mem.disp
            single = is_s(names[0])
            if mem.base == ARM_REG_PC:
                a = pc + 8 + disp
                if m == ARM_INS_VSTR:
                    raise Unsupported('vstr to literal')
                if single:
                    out.append('%s = 0x%xu;' % (us(0), im.word(a)))
                else:
                    out.append('%s = 0x%xull;' % (self.dreg(names[0], 'q'), im.word(a) | im.word(a + 4) << 32))
                return out
            addr = '(%s + 0x%xu)' % (self.reg(mem.base, pc), disp & 0xffffffff) if disp else self.reg(mem.base, pc)
            if m == ARM_INS_VLDR:
                out.append('%s = M32(%s);' % (us(0), addr) if single else '%s = M64(%s);' % (self.dreg(names[0], 'q'), addr))
            else:
                out.append('M32(%s) = %s;' % (addr, us(0)) if single else 'M64(%s) = %s;' % (addr, self.dreg(names[0], 'q')))
        elif m == ARM_INS_VMOV:
            if len(o) == 2 and o[1].type == ARM_OP_FP:
                value = o[1].fp
                if is_s(names[0]):
                    out.append('%s = %sf;' % (fs(0), repr(float(value))))
                elif suffix == 'f64':
                    out.append('%s = %s;' % (self.dreg(names[0], 'd'), repr(float(value))))
                else:
                    raise Unsupported('vmov fp to %s' % names[0])
            elif len(o) == 2 and o[1].type == ARM_OP_IMM:
                if is_d(names[0]) and suffix == 'i32':
                    out.append('%s = 0x%xull;' % (self.dreg(names[0], 'q'), (o[1].imm & 0xffffffff) * 0x100000001))
                else:
                    raise Unsupported('vmov imm %s' % ins.op_str)
            elif len(o) == 2:
                a, b = names
                if is_s(a) and is_s(b):
                    out.append('%s = %s;' % (us(0), us(1)))
                elif is_d(a) and is_d(b):
                    out.append('%s = %s;' % (self.dreg(a, 'q'), self.dreg(b, 'q')))
                elif is_s(a):
                    out.append('%s = %s;' % (us(0), self.reg(o[1].reg, pc)))
                elif is_s(b):
                    out.append('%s = %s;' % (self.reg(o[0].reg, pc), us(1)))
                else:
                    raise Unsupported('vmov %s' % ins.op_str)
            elif len(o) == 3:
                a, b, c3 = names
                if is_d(a):
                    out.append('%s = %s | (uint64_t)%s << 32;' % (self.dreg(a, 'q'), self.reg(o[1].reg, pc), self.reg(o[2].reg, pc)))
                elif is_d(c3):
                    out.append('{ uint64_t w = %s; %s = (uint32_t)w; %s = (uint32_t)(w >> 32); }'
                               % (self.dreg(c3, 'q'), self.reg(o[0].reg, pc), self.reg(o[1].reg, pc)))
                else:
                    raise Unsupported('vmov %s' % ins.op_str)
            else:
                raise Unsupported('vmov %s' % ins.op_str)
        elif m == ARM_INS_VORR:
            if names[1] != names[2]:
                raise Unsupported('vorr %s' % ins.op_str)
            out.append('%s = %s;' % (self.dreg(names[0], 'q'), self.dreg(names[1], 'q')))
        elif m in (ARM_INS_VADD, ARM_INS_VSUB, ARM_INS_VMUL, ARM_INS_VDIV, ARM_INS_VNMUL, ARM_INS_VMIN, ARM_INS_VMAX):
            op = {ARM_INS_VADD: '+', ARM_INS_VSUB: '-', ARM_INS_VMUL: '*', ARM_INS_VDIV: '/', ARM_INS_VNMUL: '*'}.get(m)
            if len(o) == 2:
                names = [names[0], names[0], names[1]]
            a, b, c3 = names
            neg = '-' if m == ARM_INS_VNMUL else ''
            if is_s(a):
                if m in (ARM_INS_VMIN, ARM_INS_VMAX):
                    raise Unsupported('scalar vmin')
                out.append('%s = %s(%s %s %s);' % (self.sreg(a), neg, self.sreg(b), op, self.sreg(c3)))
            elif suffix == 'f64':
                out.append('%s = %s(%s %s %s);' % (self.dreg(a), neg, self.dreg(b), op, self.dreg(c3)))
            elif suffix == 'f32':                  # NEON: both lanes
                for lane in (0, 1):
                    x, y = '%s[%d]' % (self.dreg(b, 'f'), lane), '%s[%d]' % (self.dreg(c3, 'f'), lane)
                    if m in (ARM_INS_VMIN, ARM_INS_VMAX):
                        e = '%s(%s, %s)' % ('neon_min' if m == ARM_INS_VMIN else 'neon_max', x, y)
                    else:
                        e = '%s %s %s' % (x, op, y)
                    out.append('float t%d = %s;' % (lane, e))
                out = ['{ %s %s[0] = t0; %s[1] = t1; }' % (' '.join(out), self.dreg(a, 'f'), self.dreg(a, 'f'))]
            else:
                raise Unsupported('%s %s' % (mn, ins.op_str))
        elif m in (ARM_INS_VMLA, ARM_INS_VMLS, ARM_INS_VNMLS, ARM_INS_VNMLA):
            a, b, c3 = names
            form = {ARM_INS_VMLA: '%s + %s * %s', ARM_INS_VMLS: '%s - %s * %s', ARM_INS_VNMLS: '-%s + %s * %s',
                    ARM_INS_VNMLA: '-%s - %s * %s'}[m]
            if is_s(a):
                out.append('%s = %s;' % (self.sreg(a), form % (self.sreg(a), self.sreg(b), self.sreg(c3))))
            elif suffix == 'f64':
                out.append('%s = %s;' % (self.dreg(a), form % (self.dreg(a), self.dreg(b), self.dreg(c3))))
            elif suffix == 'f32' and m in (ARM_INS_VMLA, ARM_INS_VMLS):
                d, x, y = self.dreg(a, 'f'), self.dreg(b, 'f'), self.dreg(c3, 'f')
                out.append('{ float t0 = %s, t1 = %s; %s[0] = t0; %s[1] = t1; }' % (
                    form % (d + '[0]', x + '[0]', y + '[0]'), form % (d + '[1]', x + '[1]', y + '[1]'), d, d))
            else:
                raise Unsupported('%s %s' % (mn, ins.op_str))
        elif m in (ARM_INS_VNEG, ARM_INS_VABS, ARM_INS_VSQRT):
            a, b = names
            if is_s(a):
                e = {ARM_INS_VNEG: '-%s', ARM_INS_VABS: 'fabsf(%s)', ARM_INS_VSQRT: 'sqrtf(%s)'}[m] % self.sreg(b)
                out.append('%s = %s;' % (self.sreg(a), e))
            elif suffix == 'f64':
                e = {ARM_INS_VNEG: '-%s', ARM_INS_VABS: 'fabs(%s)', ARM_INS_VSQRT: 'sqrt(%s)'}[m] % self.dreg(b)
                out.append('%s = %s;' % (self.dreg(a), e))
            elif suffix == 'f32' and m != ARM_INS_VSQRT:
                f = '-%s' if m == ARM_INS_VNEG else 'fabsf(%s)'
                d, x = self.dreg(a, 'f'), self.dreg(b, 'f')
                out.append('{ float t0 = %s, t1 = %s; %s[0] = t0; %s[1] = t1; }' % (f % (x + '[0]'), f % (x + '[1]'), d, d))
            else:
                raise Unsupported('%s %s' % (mn, ins.op_str))
        elif m in (ARM_INS_VCMP, ARM_INS_VCMPE):
            a = names[0]
            if is_s(a):
                x = self.sreg(a)
                y = self.sreg(names[1]) if names[1] else '0.0f'
            else:
                x = self.dreg(a)
                y = self.dreg(names[1]) if names[1] else '0.0'
            out.append('{ double a = %s, b = %s; fn = a < b; fz = a == b; fc = !(a < b); fv = (a != a) || (b != b); }' % (x, y))
        elif m == ARM_INS_VMRS:
            out.append('n = fn; z = fz; c = fc; v = fv;')
        elif m == ARM_INS_VCVT:
            a, b = names[0], names[1]
            if len(o) > 2:
                raise Unsupported('fixed-point vcvt')
            if suffix == 'f64.f32':
                out.append('%s = (double)%s;' % (self.dreg(a), self.sreg(b)))
            elif suffix == 'f32.f64':
                out.append('%s = (float)%s;' % (self.sreg(a), self.dreg(b)))
            elif suffix in ('s32.f32', 'u32.f32', 's32.f64', 'u32.f64'):
                fn = 'f2i' if suffix[0] == 's' else 'f2u'
                if is_s(b) or suffix.endswith('f64'):
                    src = self.sreg(b) if is_s(b) else self.dreg(b)
                    out.append('%s = (uint32_t)%s((double)%s);' % (self.sreg(a, 'u'), fn, src))
                else:
                    d, x = self.dreg(a, 'u'), self.dreg(b, 'f')
                    out.append('{ uint32_t t0 = (uint32_t)%s(%s[0]), t1 = (uint32_t)%s(%s[1]); %s[0] = t0; %s[1] = t1; }'
                               % (fn, x, fn, x, d, d))
            elif suffix in ('f32.s32', 'f32.u32', 'f64.s32', 'f64.u32'):
                cast = '(int32_t)' if suffix.endswith('s32') else ''
                if suffix.startswith('f64'):
                    out.append('%s = (double)%s%s;' % (self.dreg(a), cast, self.sreg(b, 'u')))
                elif is_s(b):
                    out.append('%s = (float)%s%s;' % (self.sreg(a), cast, self.sreg(b, 'u')))
                else:
                    d, x = self.dreg(a, 'f'), self.dreg(b, 'u')
                    out.append('{ float t0 = (float)%s%s[0], t1 = (float)%s%s[1]; %s[0] = t0; %s[1] = t1; }'
                               % (cast, x, cast, x, d, d))
            else:
                raise Unsupported('%s %s' % (mn, ins.op_str))
        elif m in (ARM_INS_VPUSH, ARM_INS_VPOP, ARM_INS_VLDMIA, ARM_INS_VSTMIA, ARM_INS_VLDMDB, ARM_INS_VSTMDB):
            if m in (ARM_INS_VPUSH, ARM_INS_VPOP):
                base, regs, wb = 'sp', names, True
                self.used.add('sp')
            else:
                base, regs, wb = self.reg(o[0].reg, pc), names[1:], ins.writeback
            size = 8 if is_d(regs[0]) else 4
            total = size * len(regs)
            down = m in (ARM_INS_VPUSH, ARM_INS_VSTMDB, ARM_INS_VLDMDB)
            load = m in (ARM_INS_VPOP, ARM_INS_VLDMIA, ARM_INS_VLDMDB)
            body = ['uint32_t ea = %s%s;' % (base, ' - %d' % total if down else '')]
            for i, name in enumerate(regs):
                if m in (ARM_INS_VPUSH, ARM_INS_VPOP) and size == 8 and 8 <= int(name[1:]) <= 15:
                    continue                        # preserved registers: as for push and pop
                lv = self.dreg(name, 'q') if size == 8 else self.sreg(name, 'u')
                acc = 'M64(ea + %d)' % (size * i) if size == 8 else 'M32(ea + %d)' % (size * i)
                body.append('%s = %s;' % ((lv, acc) if load else (acc, lv)))
            if wb:
                body.append('%s = ea%s;' % (base, '' if down else ' + %d' % total))
            out.append('{ %s }' % ' '.join(body))
        else:
            raise Unsupported('%s %s' % (mn, ins.op_str))
        return out

    def emit(self):
        body = []
        self.tables = []
        for pc in sorted(self.ins):
            ins = self.ins[pc]
            try:
                stmts = self.translate(pc, ins)
            except Unsupported as e:
                raise Unsupported('%s at %x: %s %s: %s' % (self.name, pc, ins.mnemonic, ins.op_str, e)) from None
            except (IndexError, KeyError, AttributeError, TypeError) as e:
                raise Unsupported('%s at %x: %s %s: internal %r' % (self.name, pc, ins.mnemonic, ins.op_str, e)) from None
            if pc in DISPLAY_OFFSETS:
                stmts = list(stmts) + ['%s.f[0] *= rt_display_scale;' % DISPLAY_OFFSETS[pc]]
            text = ' '.join(stmts)
            if ins.cc not in (ARM_CC_AL, ARM_CC_INVALID) and pc not in self.never:
                text = 'if (%s) { %s }' % (CC[ins.cc], text)
            label = 'L_%x: ' % pc if pc in self.leaders and pc != self.addr else ''
            body.append('  %s%s' % (label, text))
            # falling off a decoded run into undecoded space cannot happen: decode() stops only at ends
            nxt = pc + 4
            if nxt not in self.ins and not self.ends_flow(ins, pc):
                body.append('  rt_bad_jump(0x%xu);' % nxt)
        core = sorted(self.used & {'r%d' % i for i in range(4, 13)}, key=lambda s: (len(s), s))
        decl = ['%s = 0' % name for name in core]
        if 'lr' in self.used:
            decl.append('lr = 0')
        lines = self.tables + ['uint64_t f_%x(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) { /* %s */' % (self.addr, self.name)]
        if decl:
            lines.append('  uint32_t %s;' % ', '.join(decl))
        lines += ['  uint32_t n = 0, z = 0, c = 0, v = 0, fn = 0, fz = 0, fc = 0, fv = 0;']
        vregs = sorted((x for x in self.used if x[0] == 'd'), key=lambda s: int(s[1:]))
        if vregs:
            lines.append('  vreg %s;' % ', '.join('%s = {0}' % x for x in vregs))
        lines.append('  (void)n; (void)z; (void)c; (void)v; (void)fn; (void)fz; (void)fc; (void)fv; ENTER(0x%xu);' % self.addr)
        if self.name in NOTIFY:
            lines.append('  %s(r0);' % NOTIFY[self.name])
        lines += body
        lines.append('}')
        return '\n'.join(lines)

    def ends_flow(self, ins, pc):
        if ins.cc not in (ARM_CC_AL, ARM_CC_INVALID):
            return False
        if ins.id in (ARM_INS_B, ARM_INS_BX):
            return pc not in self.never
        if ins.id in (ARM_INS_BL, ARM_INS_BLX):
            return ins.operands[0].type == ARM_OP_IMM and self.im.stubs.get(ins.operands[0].imm) in NORETURN
        return self.writes_pc(ins)


VFP = {ARM_INS_VLDR, ARM_INS_VSTR, ARM_INS_VMOV, ARM_INS_VADD, ARM_INS_VSUB, ARM_INS_VMUL, ARM_INS_VDIV, ARM_INS_VNMUL,
       ARM_INS_VMLA, ARM_INS_VMLS, ARM_INS_VNMLS, ARM_INS_VNMLA, ARM_INS_VNEG, ARM_INS_VABS, ARM_INS_VSQRT, ARM_INS_VCMP,
       ARM_INS_VCMPE, ARM_INS_VMRS, ARM_INS_VCVT, ARM_INS_VPUSH, ARM_INS_VPOP, ARM_INS_VLDMIA, ARM_INS_VSTMIA,
       ARM_INS_VLDMDB, ARM_INS_VSTMDB, ARM_INS_VMIN, ARM_INS_VMAX, ARM_INS_VORR}
VFP_NAMES = {'vldr': ARM_INS_VLDR, 'vstr': ARM_INS_VSTR, 'vmov': ARM_INS_VMOV, 'vadd': ARM_INS_VADD, 'vsub': ARM_INS_VSUB,
             'vmul': ARM_INS_VMUL, 'vdiv': ARM_INS_VDIV, 'vnmul': ARM_INS_VNMUL, 'vmla': ARM_INS_VMLA, 'vmls': ARM_INS_VMLS,
             'vnmls': ARM_INS_VNMLS, 'vnmla': ARM_INS_VNMLA, 'vneg': ARM_INS_VNEG, 'vabs': ARM_INS_VABS,
             'vsqrt': ARM_INS_VSQRT, 'vcmp': ARM_INS_VCMP, 'vcmpe': ARM_INS_VCMPE, 'vmrs': ARM_INS_VMRS,
             'vcvt': ARM_INS_VCVT, 'vpush': ARM_INS_VPUSH, 'vpop': ARM_INS_VPOP, 'vldmia': ARM_INS_VLDMIA,
             'vstmia': ARM_INS_VSTMIA, 'vldmdb': ARM_INS_VLDMDB, 'vstmdb': ARM_INS_VSTMDB, 'vmin': ARM_INS_VMIN,
             'vmax': ARM_INS_VMAX, 'vorr': ARM_INS_VORR}
IMPORTS, USED_OVERRIDES, MISSING, WANTED = set(), set(), set(), set()


def main():
    out_dir = Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)
    im = Image()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True

    addr, size, _ = im.sections['__DATA,__mod_init_func']
    initialisers = {im.word(addr + 4 * i) for i in range(size // 4)}

    def wanted(addr):
        name = im.funcs.get(addr)
        if addr in initialisers:
            return True
        return name is not None and not im.is_objc(name) and name not in OVERRIDES \
            and not name.startswith(OVERRIDE_PREFIXES) and name not in ('_main', 'start', 'dyld_stub_binding_helper')

    # Roots: the controller's methods, the static initialisers, and every engine function
    # whose address is stored in the data sections (virtual tables, callbacks).
    roots = {a for a, n in im.funcs.items() if n.startswith(ROOT_PATTERNS)}
    init = []
    addr, size, _ = im.sections['__DATA,__mod_init_func']
    for i in range(size // 4):
        target = im.word(addr + 4 * i)
        init.append(target)
        if wanted(target):
            roots.add(target)
    for section in ('__DATA,__const', '__DATA,__data'):
        addr, size, _ = im.sections[section]
        for a in range(addr, addr + size, 4):
            if wanted(im.word(a)):
                roots.add(im.word(a))
    functions, work = {}, sorted(roots)
    failures = []
    while work:
        addr = work.pop()
        if addr in functions or not wanted(addr):
            continue
        try:
            fn = functions[addr] = Function(im, md, addr)
        except Unsupported as e:
            failures.append(str(e))
            functions[addr] = None
            continue
        work.extend(t for t in fn.calls if t in im.funcs)
    WANTED.update(a for a, f in functions.items() if f is not None)
    texts = {}
    for addr in sorted(WANTED):
        try:
            texts[addr] = functions[addr].emit()
        except Unsupported as e:
            failures.append(str(e))
    if failures:
        print('%d failures:' % len(failures))
        for f in failures[:60]:
            print('  ' + f)
        sys.exit(1)

    count = sum(len(functions[a].ins) for a in texts)
    header = ['/* Generated by tools/recomp.py from the original executable. Do not commit. */', '#define RT_GENERATED 1',
              '#include "rt.h"', '']
    protos = ['uint64_t f_%x(uint32_t, uint32_t, uint32_t, uint32_t);' % a for a in sorted(texts)]
    protos += ['void imp%s(void);' % n for n in sorted(IMPORTS)]
    protos += ['void ovr%s(void);' % n for n in sorted(USED_OVERRIDES)]
    (out_dir / 'recomp.h').write_text('\n'.join(['/* Generated by tools/recomp.py. Do not commit. */'] + protos) + '\n')
    parts = 8
    order = sorted(texts)
    for p in range(parts):
        chunk = order[p * len(order) // parts:(p + 1) * len(order) // parts]
        (out_dir / ('recomp_%d.c' % p)).write_text('\n'.join(header + ['#include "recomp.h"', ''] + [texts[a] for a in chunk]) + '\n')
    table = ['/* Generated by tools/recomp.py. Do not commit. */', '#define RT_GENERATED 1', '#include "rt.h"',
             '#include "recomp.h"', '',
             'const rt_func rt_functions[] = {']
    table += ['  { 0x%xu, f_%x },' % (a, a) for a in order]
    table += ['};', 'const int rt_function_count = %d;' % len(order), '',
              'const uint32_t rt_static_init[] = { %s };' % ', '.join('0x%xu' % a for a in init if a in texts),
              'const int rt_static_init_count = %d;' % sum(1 for a in init if a in texts), '']
    names = {n: a for a, n in im.funcs.items() if a in texts}
    table.append('const rt_symbol rt_symbols[] = {')
    table += ['  { "%s", 0x%xu },' % (n, a) for n, a in sorted(names.items())]
    table += ['};', 'const int rt_symbol_count = %d;' % len(names), '']
    blob = b''.join(b for _, b in sorted(LITERALS.values()))
    table.append('const uint8_t rt_literals[] = { %s };' % ', '.join('%d' % x for x in blob))
    table.append('const uint32_t rt_literals_size = %d;' % len(blob))
    table.append('')
    table += ['/* A call through a pointer (virtual functions): find the translated function. */',
              'uint64_t rt_call(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {',
              '    int lo = 0, hi = rt_function_count - 1;',
              '    while (lo <= hi) {',
              '        int mid = (lo + hi) / 2;',
              '        if (rt_functions[mid].addr == addr) return rt_functions[mid].fn(r0, r1, r2, r3);',
              '        if (rt_functions[mid].addr < addr) lo = mid + 1; else hi = mid - 1;',
              '    }',
              '    rt_bad_call(addr);',
              '    return 0;',
              '}', '',
              '/* The way in from the port\'s own code. Where the stack pointer and memory base live in',
              ' * reserved registers, the caller\'s values are put back afterwards. */',
              'uint64_t rt_enter(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {',
              '    RT_ENTER_SAVE();',
              '    uint64_t result = rt_call(addr, r0, r1, r2, r3);',
              '    RT_ENTER_RESTORE();',
              '    return result;',
              '}', '']
    table.append('const rt_extern rt_externs[] = {')
    table += ['  { 0x%xu, "%s" },' % (a, n) for a, n in sorted(im.nonlazy.items())]
    table += ['};', 'const int rt_extern_count = %d;' % len(im.nonlazy)]
    (out_dir / 'recomp_table.c').write_text('\n'.join(table) + '\n')
    print('%d constants among the code moved to a %d-byte table' % (len(LITERALS), len(blob)))
    print('%d functions, %d instructions; %d imports, %d overrides, %d calls to functions left out'
          % (len(texts), count, len(IMPORTS), len(USED_OVERRIDES), len(MISSING)))
    (out_dir / 'imports.txt').write_text('\n'.join(sorted(IMPORTS)) + '\n')
    (out_dir / 'overrides.txt').write_text('\n'.join(sorted(USED_OVERRIDES)) + '\n')
    (out_dir / 'missing.txt').write_text('\n'.join(sorted(MISSING)) + '\n')


if __name__ == '__main__':
    main()
