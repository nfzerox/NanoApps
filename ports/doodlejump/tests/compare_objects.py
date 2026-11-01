#!/usr/bin/env python3
"""Differential checks: translated C vs original ARM instructions, every tick."""
import ctypes
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from original_oracle import Original,ROOT

class Object(ctypes.Structure):
    _fields_=[(n,ctypes.c_float) for n in ('x','y','vx','alpha','offset_x','offset_y','spring_offset')]+[
        (n,ctypes.c_int) for n in ('type','active','age','falling','fading','spring','spring_used','range_x','range_y','texture')]

lib=ctypes.CDLL(str(ROOT/'build/objects.dylib'))
for n in ('platform_tick','monster_tick','ufo_tick'):
    getattr(lib,n).argtypes=[ctypes.POINTER(Object)]+([ctypes.c_uint] if n=='ufo_tick' else [])

def equal(a,b,field,frame):
    # Require identical float32 values; Python receives exact expanded float32s.
    assert struct.pack('<f',a)==struct.pack('<f',b),(field,frame,a,b)

checks=0
for moving in (False,True):
 for falling in (False,True):
  for fading in (False,True):
    vm=Original();p=vm.new('PlatformObject');vm.invoke(p,'setType:',2)
    vm.set(p,'objPosition',(293.,150.));vm.set(p,'isMoving',int(moving));vm.set(p,'objSpeed',1.25)
    vm.set(p,'isFalling',int(falling));vm.set(p,'isFading',int(fading))
    o=Object(x=293,y=150,vx=1.25 if moving else 0,alpha=1,type=2,active=1,texture=2,
             falling=falling,fading=fading)
    for frame in range(600):
        vm.invoke(p,'tick');lib.platform_tick(ctypes.byref(o))
        x,y=vm.get(p,'objPosition')
        for a,b,k in [(o.x,x,'x'),(o.y,y,'y'),(o.alpha,vm.get(p,'objAlpha'),'alpha')]:equal(a,b,k,frame)
        assert o.texture==vm.get(p,'objTexture')
        assert o.fading==vm.get(p,'isFading');checks+=1
for kind in range(3):
    vm=Original();p=vm.new('MonsterObject');vm.invoke(p,'setType:',kind);vm.set(p,'objPosition',(293.,200.))
    o=Object(x=293,y=200,vx=2,alpha=1,type=kind+7,active=1,texture=kind)
    for frame in range(1800):
        vm.invoke(p,'tick');lib.monster_tick(ctypes.byref(o))
        x,y=vm.get(p,'objPosition');equal(o.x,x,'monster x',frame);equal(o.y,y,'monster y',frame)
        assert o.texture==vm.get(p,'objTexture');checks+=1
vm=Original();p=vm.new('UfoObject');vm.set(p,'objPosition',(200.,300.))
o=Object(x=200,y=300,alpha=1,type=5,active=1)
vm.random_values=iter(range(1800))
for frame in range(1800):
    vm.invoke(p,'tick');lib.ufo_tick(ctypes.byref(o),frame)
    x,y=vm.get(p,'objPosition');equal(o.x,x,'ufo x',frame);equal(o.y,y,'ufo y',frame)
    assert o.texture==vm.get(p,'objTexture');checks+=1
print(f'PASS: {checks} original ARM/C tick comparisons (position, fade, texture)')

class Bonus(ctypes.Structure):
    _fields_=[(n,ctypes.c_float) for n in ('x','y','speed','offset')]+[(n,ctypes.c_int) for n in ('active','type','moving')]
class Shot(ctypes.Structure):
    _fields_=[('x',ctypes.c_float),('y',ctypes.c_float),('active',ctypes.c_int)]
lib.bonus_tick.argtypes=[ctypes.POINTER(Bonus)];lib.projectile_tick.argtypes=[ctypes.POINTER(Shot)]
extra=0
for moving in (0,1):
 for speed in (-1.25,1.25):
  for x in (10.,310.):
    vm=Original();b=vm.new('BonusObject')
    vm.set(b,'objPosition',(x,300.));vm.set(b,'posOffset',13.5)
    vm.set(b,'objSpeed',speed);vm.set(b,'isMoving',moving)
    o=Bonus(x=x,y=300,speed=speed,offset=13.5,active=1,moving=moving)
    for frame in range(600):
        vm.invoke(b,'tick');lib.bonus_tick(ctypes.byref(o))
        assert (o.x,o.y)==vm.get(b,'objPosition'),(moving,speed,x,frame)
        assert o.speed==vm.get(b,'objSpeed');extra+=1
vm=Original();p=vm.new('ProjectileObject');vm.set(p,'objPosition',(150.,250.))
o=Shot(x=150,y=250,active=1)
for frame in range(200):
    vm.invoke(p,'tick');lib.projectile_tick(ctypes.byref(o))
    assert (o.x,o.y)==vm.get(p,'objPosition');extra+=1
print(f'PASS: {extra} original ARM/C spring and projectile ticks')
