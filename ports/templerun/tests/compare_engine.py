#!/usr/bin/env python3
"""Run the original engine code and the translated engine side by side and compare memory.

Both get the same random seed and the same scripted input (a swipe every 40 frames, a tilt
that changes every 25); afterwards the data segment and the whole heap must match byte for
byte. Needs the original executable (untracked) and the translated engine built as
build/engine_run (see README).

usage: compare_engine.py [frames] [seed]
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import original_oracle as oracle     # noqa: E402

frames = int(sys.argv[1]) if len(sys.argv) > 1 else 300
seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1


def run_original():
    vm = oracle.Original(320.0, 480.0, 1.0, seed=seed)
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
    return vm


t = time.time()
vm = run_original()
a = vm.dump()
print('original: %d frames in %.1f s, heap %d bytes, random() calls %d' % (frames, time.time() - t, vm.next - oracle.HEAP, vm.random.calls))
dump = ROOT / 'build/dump_c.bin'
if '--dump' in sys.argv:            # a dump made elsewhere (the ARM build under an emulator)
    dump = Path(sys.argv[sys.argv.index('--dump') + 1])
else:
    subprocess.run([str(ROOT / 'build/engine_run'), str(ROOT / 'build/engine'), str(oracle.APP), str(frames), str(seed), str(dump)],
                   check=True)
b = dump.read_bytes()
regions = [(0xd9000, 0xea000 - 0xd9000), (oracle.EXTERN, oracle.LITERALS - oracle.EXTERN), (oracle.HEAP, None)]
brk_a, brk_b = struct.unpack_from('<I', a)[0], struct.unpack_from('<I', b)[0]
if brk_a != brk_b:
    print('heap sizes differ: original %x, translated %x' % (brk_a, brk_b))
la = range(0xd92ec, 0xd92ec + 0x578)           # lazy import pointers: only the original uses them
diffs, pos = [], 4
for base, size in regions:
    if size is None:
        size = min(brk_a, brk_b) - base
    for off in range(0, size, 4):
        if a[pos + off:pos + off + 4] != b[pos + off:pos + off + 4] and base + off not in la:
            diffs.append((base + off, struct.unpack_from('<I', a, pos + off)[0], struct.unpack_from('<I', b, pos + off)[0]))
    pos += size
print('%d differing words' % len(diffs))
for addr, x, y in diffs[:12]:
    print('  %x: original %08x, translated %08x' % (addr, x, y))
sys.exit(1 if diffs or brk_a != brk_b else 0)
