#!/usr/bin/env python3
"""Original ARM/C comparisons for initial layout and collision branches."""
import ctypes,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from original_oracle import Original,ROOT
lib=ctypes.CDLL(str(ROOT/'build/core.dylib'))
lib.probe_start.argtypes=[ctypes.c_uint]
lib.probe_object.argtypes=[ctypes.c_int,ctypes.POINTER(ctypes.c_float)]
lib.probe_collision.argtypes=[ctypes.c_int]+[ctypes.c_float]*3+[ctypes.c_int]
lib.probe_result.argtypes=[ctypes.POINTER(ctypes.c_float)]
system=ctypes.CDLL(None);system.srandom.argtypes=[ctypes.c_uint];system.random.restype=ctypes.c_long
arrays=('visiblePlatformsArray','bonusObjectsArray','projectileObjectsArray','holeObjectsArray','ufoObjectsArray','monsterObjectsArray')

def game(vm):
    g=vm.new('JumpAppDelegate')
    for n in arrays:vm.set(g,n,vm.array([]))
    vm.set(g,'playerCanMove',1);vm.set(g,'gameState',2)
    vm.set(g,'playerBoundingBox',(-15.,-27.,28.,35.))
    return g

for seed in (0,1,42,1234567,0xdeadbeef):
    vm=Original();g=game(vm);vm.ignore.add('mainGameLoop');system.srandom(seed)
    vm.random_values=iter([system.random() for _ in range(100)])
    vm.invoke(g,'initNewGame');lib.probe_start(seed)
    platforms=vm.arrays[vm.get(g,'visiblePlatformsArray')]
    assert len(platforms)==lib.probe_count()==21
    for i,p in enumerate(platforms):
        out=(ctypes.c_float*3)();lib.probe_object(i,out)
        assert tuple(out[:2])==vm.get(p,'objPosition'),(seed,i,tuple(out),vm.get(p,'objPosition'))
print('PASS: original ARM/C initial layouts, 5 seeds, 105 platforms',flush=True)

lib.probe_generate.argtypes=[ctypes.c_float,ctypes.c_int]
lib.probe_generated.argtypes=[ctypes.c_int,ctypes.POINTER(ctypes.c_float)]
lib.probe_bonus.argtypes=[ctypes.c_int,ctypes.POINTER(ctypes.c_float)]
for height in (0.,5501.,15000.,40000.):
    vm=Original();g=game(vm);vm.ignore.add('mainGameLoop');system.srandom(42)
    vm.random_values=iter([system.random() for _ in range(3000)])
    vm.invoke(g,'initNewGame');lib.probe_start(42)
    vm.set(g,'mainScreenUpOffset',height)
    for _ in range(80):vm.invoke(g,'generateNewObjectAboveScreen')
    lib.probe_generate(height,80)
    platforms=vm.arrays[vm.get(g,'visiblePlatformsArray')]
    assert len(platforms)==lib.probe_count()==101
    for i,p in enumerate(platforms):
        out=(ctypes.c_float*5)();lib.probe_generated(i,out)
        expected=(*vm.get(p,'objPosition'),vm.get(p,'objType'),
                  vm.get(p,'objSpeed') if vm.get(p,'isMoving') else 0,vm.get(p,'hasBonusObject'))
        assert tuple(out)==expected,(height,i,tuple(out),expected)
    bonuses=vm.arrays[vm.get(g,'bonusObjectsArray')]
    assert len(bonuses)==lib.probe_bonus_count()
    for i,b in enumerate(bonuses):
        out=(ctypes.c_float*4)();lib.probe_bonus(i,out)
        expected=(*vm.get(b,'objPosition'),vm.get(b,'objSpeed') if vm.get(b,'isMoving') else 0,vm.get(b,'objType'))
        assert tuple(out)==expected,(height,'bonus',i,tuple(out),expected)
print('PASS: original ARM/C procedural generation, 320 platforms at four difficulty heights',flush=True)

checks=0
for kind in (0,1,2,3,4,5,7,8,9):
 for vy in (-2.,2.):
  for dx,dy in ((0,0),(0,29),(0,34),(25,29),(-25,29),(70,29),(0,60),(0,-20)):
   vm=Original();g=game(vm)
   vm.set(g,'playerX',150.+dx);vm.set(g,'playerY',200.+dy);vm.set(g,'jumpOffset',vy)
   vm.set(g,'playerIsFalling',int(vy<0))
   owner='PlatformObject' if kind<4 else 'HoleObject' if kind==4 else 'UfoObject' if kind==5 else 'MonsterObject'
   p=vm.new(owner)
   if kind<4:vm.invoke(p,'setType:',kind)
   if kind>=7:vm.invoke(p,'setType:',kind-7)
   vm.set(p,'objPosition',(150.,200.))
   arr=arrays[0 if kind<4 else 3 if kind==4 else 4 if kind==5 else 5]
   vm.arrays[vm.get(g,arr)].append(p)
   vm.invoke(g,'checkForCollisions')
   lib.probe_collision(kind,150.+dx,200.+dy,vy,0)
   out=(ctypes.c_float*9)();lib.probe_result(out)
   death=3 if vm.get(g,'playerDidHitHole') else 4 if vm.get(g,'playerDidHitUfo') else 2 if vm.get(g,'playerDidHitMonster') else 0
   expected=[vm.get(g,'jumpOffset'),death,vm.get(g,'framesToTargetPosition')]
   if kind<4:expected += [vm.get(p,'isFalling'),vm.get(p,'isFading')]
   for i,v in enumerate(expected):assert out[i]==v,(kind,vy,dx,dy,i,out[i],v)
   if death in (3,4):
       assert out[7]==vm.get(g,'xStepToTargetPoisition')
       assert out[8]==vm.get(g,'yStepToTargetPoisition')
   checks+=1
print(f'PASS: original ARM/C collision outcomes, {checks} scenarios')
