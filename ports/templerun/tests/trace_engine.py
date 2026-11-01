#!/usr/bin/env python3
"""Find where the translated engine first departs from the original.

Logs every function entry (address and r0-r3) in both, running the same script as
compare_engine.py, and prints the first entries that differ. Needs build/engine_trace
(engine_run built with -DRT_TRACE).

usage: trace_engine.py [frames] [seed]
"""
import re
import struct
import subprocess
import sys
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import original_oracle as oracle     # noqa: E402

frames = int(sys.argv[1]) if len(sys.argv) > 1 else 3
seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
table = (ROOT / 'build/recomp/recomp_table.c').read_text()
functions = [int(x, 16) for x in re.findall(r'\{ 0x([0-9a-f]+)u, f_', table)]
names = {v: k for k, v in oracle.Original.__dict__.items() if False}

vm = oracle.Original(320.0, 480.0, 1.0, seed=seed, trace_functions=functions)
touch = vm.alloc(32)
vm.initialize()
vm.start()
f32 = lambda x: struct.pack('<f', x)
for f in range(frames):
    phase, kind = f % 40, (f // 40) % 4
    dx = -120.0 if kind == 0 else 120.0 if kind == 1 else 0.0
    dy = -120.0 if kind == 2 else 120.0 if kind == 3 else 0.0
    if phase == 10:
        vm.uc.mem_write(touch, f32(160.0) + f32(240.0))
        vm.run('__ZN15cGameController16handleTouchBeganE4vec2ii', vm.game, touch, 1, 1)
    if phase == 11:
        vm.uc.mem_write(touch, f32(160.0 + dx * 0.5) + f32(240.0 + dy * 0.5))
        vm.run('__ZN15cGameController16handleTouchMovedE4vec2ii', vm.game, touch, 1, 1)
    if phase == 12:
        vm.uc.mem_write(touch, f32(160.0 + dx) + f32(240.0 + dy))
        vm.run('__ZN15cGameController16handleTouchMovedE4vec2ii', vm.game, touch, 1, 1)
    if phase == 13:
        vm.run('__ZN15cGameController16handleTouchEndedE4vec2ii', vm.game, touch, 1, 1)
    vm.uc.mem_write(touch + 8, f32(((f // 25) % 5 - 2) * 0.125) + f32(-0.75) + f32(0.0))
    vm.run('__ZN15cGameController24handleAccelerometerForceERK4vec3', vm.game, touch + 8)
    vm.simulate(1.0 / 30.0)
    vm.draw()
original = vm.trace
trace = ROOT / 'build/trace_c.txt'
subprocess.run([str(ROOT / 'build/engine_trace'), str(ROOT / 'build/engine'), str(oracle.APP), str(frames), str(seed),
                str(ROOT / 'build/dump_c.bin'), str(trace)], check=True)
translated = [tuple(int(x, 16) for x in line.split()) for line in trace.read_text().splitlines()]
by_addr = {a: n for n, a in vm.sym.items()}
by_addr[0xa110c], by_addr[0xf4ee] = 'alloc(size) -> address', 'free(size, address)'
demangle = lambda a: subprocess.run(['c++filt', by_addr.get(a, '?')], capture_output=True, text=True).stdout.strip()
print('%d entries in the original, %d in the translation' % (len(original), len(translated)))
strict = '--strict' in sys.argv
for i, (x, y) in enumerate(zip(original, translated)):
    if x[0] != y[0] or x[1] != y[1] or (strict and x != y) or (x[0] in (0xa110c, 0xf4ee) and x[2] != y[2]):
        print('first difference at entry %d:' % i)
        for j in range(max(0, i - 14), min(len(original), i + 3)):
            mark = '>>' if j == i else '  '
            print('%s original   %-48s %s' % (mark, demangle(original[j][0])[:48], ' '.join('%08x' % v for v in original[j][1:])))
            if j < len(translated):
                print('%s translated %-48s %s' % (mark, demangle(translated[j][0])[:48], ' '.join('%08x' % v for v in translated[j][1:])))
        break
else:
    print('no difference in the common part')
