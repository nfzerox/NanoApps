#!/usr/bin/env python3
"""Compare the original accelerometer callback and a nonzero-tilt main loop."""
import ctypes,math,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from original_oracle import Original,ROOT
lib=ctypes.CDLL(str(ROOT/'build/core.dylib'))
lib.probe_acceleration.argtypes=[ctypes.c_double];lib.probe_acceleration_value.restype=ctypes.c_double
lib.probe_frame_state.argtypes=[ctypes.POINTER(ctypes.c_float)]
vm=Original();g=vm.new('JumpAppDelegate');sample=vm.allocate('UIAcceleration')
lib.probe_acceleration_reset()
for i in range(1000):
    value=math.sin(i*.09)*1.2
    vm.double_properties[sample,'x']=value
    vm.invoke(g,'accelerometer:didAccelerate:',0,sample);lib.probe_acceleration(value)
    assert vm.get(g,'accelX')==lib.probe_acceleration_value(),(i,vm.get(g,'accelX'),lib.probe_acceleration_value())
print('PASS: 1,000 original ARM/C accelerometer filter samples, bit-identical doubles',flush=True)
vm=Original();g=vm.new('JumpAppDelegate');sample=vm.allocate('UIAcceleration')
for n in ('visiblePlatformsArray','bonusObjectsArray','projectileObjectsArray','holeObjectsArray',
          'ufoObjectsArray','monsterObjectsArray','highScoreTexturesArray'):vm.set(g,n,vm.array([]))
vm.ignore.update(('mainGameLoop','requestWorldHighScores'))
system=ctypes.CDLL(None);system.srandom.argtypes=[ctypes.c_uint];system.random.restype=ctypes.c_long
system.srandom(42);vm.random_values=iter([system.random() for _ in range(10000)])
lib.probe_acceleration_reset();lib.probe_start(42);vm.invoke(g,'initNewGame');vm.ignore.remove('mainGameLoop')
frames=0
for clock in range(1,3001):
    if clock%3==0:
        value=ctypes.c_float(math.sin(clock*.023)*.5).value
        vm.double_properties[sample,'x']=value;vm.invoke(g,'accelerometer:didAccelerate:',0,sample)
        lib.probe_acceleration(value)
    if clock%5==0:
        vm.invoke(g,'mainGameLoop');lib.probe_tick_current();frames+=1
        out=(ctypes.c_float*8)();lib.probe_frame_state(out)
        names=('playerX','playerY','jumpOffset','mainScreenUpOffset','mainScreenDownOffset','score')
        for i,n in enumerate(names):assert out[i]==vm.get(g,n),(frames,n,out[i],vm.get(g,n))
        if vm.get(g,'gameState')==0:
            assert out[7];break
print(f'PASS: original ARM/C tilted main loop, {frames} frames with 100 Hz input and 60 Hz simulation')
