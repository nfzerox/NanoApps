#!/usr/bin/env python3
"""Compare all 19 original obstacle arrangements and their object constructors."""
import ctypes,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from original_oracle import Original,ROOT
from convert_assets import scenes
lib=ctypes.CDLL(str(ROOT/'build/core.dylib'))
lib.probe_scene.argtypes=[ctypes.c_float]
lib.probe_generated.argtypes=[ctypes.c_int,ctypes.POINTER(ctypes.c_float)]
lib.probe_bonus.argtypes=[ctypes.c_int,ctypes.POINTER(ctypes.c_float)]
assert lib.probe_load_scenes()==0
system=ctypes.CDLL(None);system.srandom.argtypes=[ctypes.c_uint];system.random.restype=ctypes.c_long
data=scenes(ROOT/'original/v1.0/Payload/DoodleJump.app/DoodleJump')
tables=[];off=8
for _ in range(19):
    n=struct.unpack_from('<I',data,off)[0];off+=4
    tables.append([struct.unpack_from('<3h',data,off+6*i) for i in range(n)]);off+=6*n
seeds={}
for seed in range(200):
    system.srandom(seed)
    for _ in range(42):system.random()
    value=system.random();seeds.setdefault(value%14,seed);seeds.setdefault(14+value%5,seed)
assert len(seeds)==19
count=0
for scene,seed in sorted(seeds.items()):
    vm=Original();g=vm.new('JumpAppDelegate')
    arrays=('visiblePlatformsArray','bonusObjectsArray','holeObjectsArray','ufoObjectsArray','monsterObjectsArray')
    for name in arrays:vm.set(g,name,vm.array([]))
    vm.ignore.add('mainGameLoop')
    for i,table in enumerate(tables):
        rows=[]
        for row in table:
            values=[]
            for number in row:
                p=vm.allocate('NSString');vm.strings[p]=str(number);values.append(p)
            rows.append(vm.array(values))
        address=g+vm.ivars['JumpAppDelegate','sceneArray']['offset']+4*i
        vm.uc.mem_write(address,struct.pack('<I',vm.array(rows)))
    system.srandom(seed);vm.random_values=iter([system.random() for _ in range(500)])
    vm.invoke(g,'initNewGame');lib.probe_start(seed)
    height=10000. if scene>=14 else 20000.
    vm.set(g,'mainScreenUpOffset',height);vm.invoke(g,'generateNewSceneAboveScreen');lib.probe_scene(height)
    actual={name:[] for name in arrays}
    for i in range(lib.probe_count()):
        out=(ctypes.c_float*5)();lib.probe_generated(i,out);x,y,t,vx,spring=out
        name=arrays[0 if t<4 else 2 if t==4 else 3 if t==5 else 4]
        actual[name].append((x,y,t,vx,spring))
    for name in (arrays[0],*arrays[2:]):
        expected=[]
        for p in vm.arrays[vm.get(g,name)]:
            owner=vm.objects[p]
            t=vm.get(p,'objType') if owner=='PlatformObject' else 4 if owner=='HoleObject' else 5 if owner=='UfoObject' else 7+vm.get(p,'objType')
            vx=vm.get(p,'objSpeed') if owner=='PlatformObject' and vm.get(p,'isMoving') else 2 if owner=='MonsterObject' else 0
            bonus=vm.get(p,'hasBonusObject') if owner=='PlatformObject' else 0
            expected.append((*vm.get(p,'objPosition'),t,vx,bonus))
        assert actual[name]==expected,(scene,name,actual[name],expected)
        count+=len(expected)-(21 if name==arrays[0] else 0)
    bonuses=vm.arrays[vm.get(g,'bonusObjectsArray')]
    assert lib.probe_bonus_count()==len(bonuses)
    for i,p in enumerate(bonuses):
        out=(ctypes.c_float*4)();lib.probe_bonus(i,out)
        assert tuple(out)==(*vm.get(p,'objPosition'),0.,0.),(scene,'bonus',tuple(out))
print(f'PASS: original ARM/C all 19 scenes, {count} scene objects, including springs and moving platforms')
