#!/usr/bin/env python3
"""Compare full original main-loop state with the translated game simulation."""
import ctypes,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from original_oracle import Original,ROOT
lib=ctypes.CDLL(str(ROOT/'build/core.dylib'))
lib.probe_tick.argtypes=[ctypes.c_float]
lib.probe_frame_state.argtypes=[ctypes.POINTER(ctypes.c_float)]
system=ctypes.CDLL(None);system.srandom.argtypes=[ctypes.c_uint];system.random.restype=ctypes.c_long

for seed in (1,42,123):
    vm=Original();g=vm.new('JumpAppDelegate')
    for n in ('visiblePlatformsArray','bonusObjectsArray','projectileObjectsArray','holeObjectsArray',
              'ufoObjectsArray','monsterObjectsArray','highScoreTexturesArray'):
        vm.set(g,n,vm.array([]))
    vm.ignore.update(('mainGameLoop','requestWorldHighScores'))
    system.srandom(seed);vm.random_values=iter([system.random() for _ in range(10000)])
    vm.invoke(g,'initNewGame');lib.probe_start(seed);vm.ignore.remove('mainGameLoop')
    for frame in range(600):
        # Constant zero tilt isolates every vertical/scrolling/death arithmetic step.
        vm.set(g,'accelX',0.);vm.invoke(g,'mainGameLoop');lib.probe_tick(0.)
        out=(ctypes.c_float*8)();lib.probe_frame_state(out)
        names=('playerX','playerY','jumpOffset','mainScreenUpOffset','mainScreenDownOffset','score')
        for i,name in enumerate(names):
            value=vm.get(g,name)
            assert out[i]==value,(seed,frame,name,out[i],value)
        assert lib.probe_jump_texture()==vm.get(g,'isJumpTexture'),(seed,frame,'jump texture')
        assert lib.probe_player_texture()==vm.get(g,'playerTexture'),(seed,frame,'player texture')
        original_finished=vm.get(g,'gameState')==0
        assert bool(out[7])==original_finished,(seed,frame,'finished',out[7],original_finished)
        if original_finished:break
    print(f'PASS: original ARM/C main loop seed {seed}, {frame+1} frames',flush=True)

lib.probe_capture_state.argtypes=[ctypes.POINTER(ctypes.c_float)]
for kind,owner in ((4,'HoleObject'),(5,'UfoObject')):
    vm=Original();g=vm.new('JumpAppDelegate')
    for n in ('visiblePlatformsArray','bonusObjectsArray','projectileObjectsArray','holeObjectsArray',
              'ufoObjectsArray','monsterObjectsArray','highScoreTexturesArray'):
        vm.set(g,n,vm.array([]))
    vm.ignore.add('requestWorldHighScores')
    vm.set(g,'gameState',2);vm.set(g,'playerCanMove',1);vm.set(g,'shouldCheckForCollisions',1)
    vm.set(g,'playerX',150.);vm.set(g,'playerY',200.);vm.set(g,'jumpOffset',2.)
    vm.set(g,'playerSize',(46.,59.));vm.set(g,'playerBoundingBox',(-15.,-27.,28.,35.))
    p=vm.new(owner);vm.set(p,'objPosition',(150.,200.))
    vm.arrays[vm.get(g,'holeObjectsArray' if kind==4 else 'ufoObjectsArray')].append(p)
    floor=vm.new('PlatformObject');vm.invoke(floor,'setType:',0);vm.set(floor,'objPosition',(20.,1000.))
    vm.arrays[vm.get(g,'visiblePlatformsArray')].append(floor)
    vm.invoke(g,'checkForCollisions');lib.probe_capture(kind)
    for frame in range(90):
        vm.invoke(g,'mainGameLoop');lib.probe_tick(0.)
        out=(ctypes.c_float*6)();lib.probe_capture_state(out)
        expected=(vm.get(g,'playerX'),vm.get(g,'playerY'),*vm.get(g,'playerSize'),
                  vm.get(g,'framesToTargetPosition'),vm.get(g,'mainScreenDownOffset'))
        assert tuple(out)==expected,(owner,frame,tuple(out),expected)
        assert lib.probe_jump_texture()==vm.get(g,'isJumpTexture'),(owner,frame,'jump texture')
        assert lib.probe_player_texture()==vm.get(g,'playerTexture'),(owner,frame,'player texture')
        if vm.get(g,'gameState')==0:break
    print(f'PASS: original ARM/C {owner} capture, {frame+1} frames',flush=True)
