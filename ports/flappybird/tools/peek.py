import struct,sys
data=open('build/flap-armv7','rb').read()
segs=[];pos=28
for _ in range(struct.unpack_from('<I',data,16)[0]):
    cmd,size=struct.unpack_from('<II',data,pos)
    if cmd==1:
        va,vs,off,fs=struct.unpack_from('<4I',data,pos+24);segs.append((va,off,fs))
    pos+=size
def off(va):
    for s,o,l in segs:
        if s<=va<s+l:return o+va-s
    raise ValueError(hex(va))
def u32(va):return struct.unpack_from('<I',data,off(va))[0]
def f32(va):return struct.unpack_from('<f',data,off(va))[0]
def f64(va):return struct.unpack_from('<d',data,off(va))[0]
def i32(va):return struct.unpack_from('<i',data,off(va))[0]
