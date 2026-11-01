#!/usr/bin/env python3
"""Test original collision ordering, multi-hit shots, edges and UFO rescues."""
import ctypes,sys,itertools
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from original_oracle import Original,ROOT
lib=ctypes.CDLL(str(ROOT/'build/core.dylib'))
lib.probe_compound.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_float,ctypes.c_float,ctypes.c_int]
lib.probe_compound_result.argtypes=[ctypes.POINTER(ctypes.c_float)]
arrays=('visiblePlatformsArray','bonusObjectsArray','projectileObjectsArray','holeObjectsArray','ufoObjectsArray','monsterObjectsArray')
checks=0
for first,second in itertools.product((4,5,7,8,9),repeat=2):
 for sx,sy in ((-1,0),(150,200),(150,245),(150,215),(188,240)):
  for capturing in ((0,1) if first==5 else (0,)):
   vm=Original();g=vm.new('JumpAppDelegate')
   for name in arrays:vm.set(g,name,vm.array([]))
   vm.set(g,'playerX',150.);vm.set(g,'playerY',200.);vm.set(g,'jumpOffset',2.)
   vm.set(g,'playerBoundingBox',(-15.,-27.,28.,35.));vm.set(g,'playerSize',(46.,59.))
   objects=[]
   for kind in (first,second):
       owner='HoleObject' if kind==4 else 'UfoObject' if kind==5 else 'MonsterObject'
       p=vm.new(owner)
       if kind>=7:vm.invoke(p,'setType:',kind-7)
       vm.set(p,'objPosition',(150.,200.));objects.append(p)
       vm.arrays[vm.get(g,arrays[3 if kind==4 else 4 if kind==5 else 5])].append(p)
   if capturing:
       vm.set(g,'playerDidHitUfo',1);vm.set(g,'playerHitUfoObject',objects[0]);vm.set(g,'playerSize',(20.,30.))
   if sx>=0:
       shot=vm.new('ProjectileObject');vm.set(shot,'objPosition',(float(sx),float(sy)))
       vm.arrays[vm.get(g,'projectileObjectsArray')].append(shot)
   vm.invoke(g,'checkForCollisions');lib.probe_compound(first,second,sx,sy,capturing)
   out=(ctypes.c_float*10)();lib.probe_compound_result(out)
   alive=set(v for name in arrays for v in vm.arrays[vm.get(g,name)])
   death=3 if vm.get(g,'playerDidHitHole') else 4 if vm.get(g,'playerDidHitUfo') else 2 if vm.get(g,'playerDidHitMonster') else 0
   target=vm.get(g,'playerHitHoleObject' if death==3 else 'playerHitUfoObject') if death in (3,4) else 0
   expected=(int(objects[0] in alive),int(objects[1] in alive),len(vm.arrays[vm.get(g,'projectileObjectsArray')]),
             vm.get(g,'jumpOffset'),death,vm.get(g,'framesToTargetPosition'),*vm.get(g,'playerSize'),
             vm.get(g,'playerDidHitMonster'),objects.index(target) if target in objects else -1)
   assert tuple(out)==expected,((first,second,sx,sy,capturing),tuple(out),expected)
   checks+=1
print(f'PASS: original ARM/C projectile and overlapping-enemy collisions, {checks} scenarios')
