#!/usr/bin/env python3
"""Run bounded Doodle Jump 1.0 object methods from the local ARM executable.

This is a gameplay reference harness, not an iOS emulator: Objective-C dispatch,
libm and ARMv6 arithmetic imports are supplied by the host. No firmware or device
code is executed. The original game executable is required and stays untracked.
"""
import hashlib
import json
import math
import re
import struct
import subprocess
from functools import lru_cache
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

ROOT=Path(__file__).resolve().parents[1]
REGS=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3]

@lru_cache(maxsize=4)
def inspect_tool(*args):
    return subprocess.check_output(args,text=True)

def f32(x):
    return struct.unpack('<f',struct.pack('<f',x))[0]


class Original:
    def __init__(self):
        binary=ROOT/'original/v1.0/Payload/DoodleJump.app/DoodleJump'
        data=binary.read_bytes()
        assert hashlib.sha256(data).hexdigest()=='68b29c08bcbe25e760dfd92aee05d92d3f3cae394789e7a029c53b210ff0e6d4'
        self.uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
        self.uc.mem_map(0x1000,0x30000)
        self.uc.mem_map(0x100000,0x100000) # object and stack storage
        self.uc.mem_map(0x400000,0x1000000) # reference objects; kept away from stack
        self.stop=0x30000
        pos=28
        for _ in range(struct.unpack_from('<I',data,16)[0]):
            cmd,size=struct.unpack_from('<II',data,pos)
            if cmd==1:
                va,vs,off,length=struct.unpack_from('<4I',data,pos+24)
                if length:self.uc.mem_write(va,data[off:off+length])
            pos+=size
        self.methods={}
        for line in (ROOT/'analysis/methods.tsv').read_text().splitlines():
            address,name=line.split('\t')
            owner,method=name[2:-1].split(' ',1)
            self.methods[owner,method]=int(address,16)
        self.ivars={}
        for v in json.loads((ROOT/'analysis/ivars.json').read_text()):
            self.ivars[v['owner'],v['name']]=v
        text=inspect_tool('otool','-Iv',str(binary))
        self.imports={int(a,16):n for a,n in re.findall(r'^(0x[0-9a-f]+)\s+\d+\s+(\S+)$',text,re.M)}
        self.objects={};self.arrays={};self.strings={};self.next_object=0x400000;self.random_values=iter([])
        self.ignore=set();self.sound_events=[];self.double_properties={}
        syms=inspect_tool('nm',str(binary))
        self.classes={int(a,16):n for a,n in re.findall(r'^([0-9a-f]+) . _OBJC_CLASS_\$_([^\s]+)',syms,re.M)}
        for a,n in re.findall(r'^([0-9a-f]+) . (_SoundEngine_\S+)',syms,re.M):
            address=int(a,16)
            self.uc.hook_add(UC_HOOK_CODE,self.sound,begin=address,end=address)
        self.uc.hook_add(UC_HOOK_CODE,self.hook,begin=0xfb04,end=0x101b7)
        self.uc.hook_add(UC_HOOK_CODE,lambda uc,a,s,u: uc.emu_stop(),begin=self.stop,end=self.stop)

    def word(self,addr):return struct.unpack('<I',self.uc.mem_read(addr,4))[0]
    def string(self,addr):
        out=bytearray()
        while self.uc.mem_read(addr,1)!=b'\0':out+=self.uc.mem_read(addr,1);addr+=1
        return out.decode()
    def arg(self,i):return self.uc.reg_read(REGS[i])
    def float_arg(self,i):return struct.unpack('<f',struct.pack('<I',self.arg(i)))[0]
    def double_arg(self,i):return struct.unpack('<d',struct.pack('<II',self.arg(i),self.arg(i+1)))[0]
    def result(self,value,kind='I'):
        data=struct.pack('<'+kind,value)
        for i in range(len(data)//4):self.uc.reg_write(REGS[i],struct.unpack_from('<I',data,i*4)[0])
        self.uc.reg_write(UC_ARM_REG_PC,self.uc.reg_read(UC_ARM_REG_LR))
    def dispatch(self,stret=False):
        obj=self.arg(1 if stret else 0);sel=self.string(self.arg(2 if stret else 1))
        if sel in self.ignore:self.result(0);return
        if (obj,sel) in self.double_properties:self.result(self.double_properties[obj,sel],'d');return
        if sel=='stringWithFormat:':
            text=str(self.arg(3));p=self.allocate('NSString');self.strings[p]=text;self.result(p);return
        if obj in self.strings:
            text=self.strings[obj]
            if sel=='length':self.result(len(text));return
            if sel=='intValue':self.result(int(text));return
            if sel=='substringWithRange:':
                p=self.allocate('NSString');self.strings[p]=text[self.arg(2):self.arg(2)+self.arg(3)]
                self.result(p);return
        if sel=='array':self.result(self.array([]));return
        if sel=='alloc' and obj in self.classes:
            self.result(self.allocate(self.classes[obj]));return
        if sel.startswith('scheduledTimerWithTimeInterval:'):self.result(0);return
        if obj in self.arrays:
            values=self.arrays[obj]
            if sel=='count':self.result(len(values));return
            if sel=='lastObject':self.result(values[-1] if values else 0);return
            if sel=='objectAtIndex:':self.result(values[self.arg(2)]);return
            if sel=='addObject:':values.append(self.arg(2));self.result(0);return
            if sel=='removeAllObjects':values.clear();self.result(0);return
            if sel=='removeObjectAtIndex:':values.pop(self.arg(2));self.result(0);return
            if sel=='removeObjectsInArray:':
                remove=self.arrays[self.arg(2)];values[:]=[x for x in values if x not in remove]
                self.result(0);return
            if sel=='makeObjectsPerformSelector:':
                selector=self.string(self.arg(2))
                if not values:self.result(0);return
                if len(values)>255:raise RuntimeError('Reference array limit exceeded')
                # Thumb trampoline calls each original method then returns to the
                # caller. Keeping dispatch inside the emulated stack avoids a
                # recursive emu_start that would overwrite the parent's registers.
                base=0x1d0000
                words=[0xb570,0x4c03,0x2500|len(values),0xcc09,0x4798,0x3d01,0xd1fb,0xbd70]
                code=struct.pack('<8H',*words)+struct.pack('<I',base+20)
                for value in values:
                    code+=struct.pack('<II',value,self.methods[self.objects[value],selector]|1)
                self.uc.mem_write(base,code);self.uc.ctl_remove_cache(base,base+len(code))
                self.uc.reg_write(UC_ARM_REG_PC,base|1);return
            if sel=='countByEnumeratingWithState:objects:count:':
                state,buffer=self.arg(2),self.arg(3);start=self.word(state)
                capacity=self.word(self.uc.reg_read(UC_ARM_REG_SP));chunk=values[start:start+capacity]
                self.uc.mem_write(state,struct.pack('<III',start+len(chunk),buffer,0x1ef000))
                if chunk:self.uc.mem_write(buffer,struct.pack('<'+'I'*len(chunk),*chunk))
                self.result(len(chunk));return
        if not obj:self.result(0);return
        owner=self.objects[obj]
        if sel in ('retain','autorelease','release'):
            self.result(obj);return
        key=owner,sel
        if key not in self.methods:raise RuntimeError(f'Missing dispatch {key}')
        self.uc.reg_write(UC_ARM_REG_PC,self.methods[key]|1)
    def hook(self,uc,address,size,user):
        name=self.imports.get(address)
        if not name:return
        if name=='_objc_msgSend':self.dispatch();return
        if name=='_objc_msgSend_stret':self.dispatch(True);return
        if name=='_objc_msgSendSuper2':self.result(self.word(self.arg(0)));return
        if name=='_AudioServicesPlaySystemSound':self.result(0);return
        if name.startswith('_gl'):self.result(0);return
        if name.startswith('_CGRect'):
            a=[self.float_arg(i) for i in range(4)]
            n=4 if name=='_CGRectIntersectsRect' else 2
            b=struct.unpack('<'+'f'*n,self.uc.mem_read(self.uc.reg_read(UC_ARM_REG_SP),4*n))
            x,y,w,h=a
            if n==4:
                bx,by,bw,bh=b;value=x<bx+bw and x+w>bx and y<by+bh and y+h>by
            else:value=x<=b[0]<f32(x+w) and y<=b[1]<f32(y+h)
            self.result(int(value));return
        if name=='_objc_copyStruct':
            self.uc.mem_write(self.arg(0),bytes(self.uc.mem_read(self.arg(1),self.arg(2))))
            self.result(0);return
        if name=='_random':self.result(next(self.random_values,1));return
        if name=='_sin':self.result(math.sin(self.double_arg(0)),'d');return
        if name=='___modsi3':
            a,b=struct.unpack('<ii',struct.pack('<II',self.arg(0),self.arg(1)))
            self.result(a-int(a/b)*b,'i');return
        m=re.fullmatch(r'___(add|sub|mul|div|eq|ne|gt|ge|lt|le|unord)(sf|df)[23]vfp',name)
        if m:
            op,kind=m.groups();a=self.float_arg(0) if kind=='sf' else self.double_arg(0)
            b=self.float_arg(1) if kind=='sf' else self.double_arg(2)
            arithmetic={'add':lambda:a+b,'sub':lambda:a-b,'mul':lambda:a*b,'div':lambda:a/b}
            compare={'eq':lambda:a==b,'ne':lambda:a!=b,'gt':lambda:a>b,'ge':lambda:a>=b,
                     'lt':lambda:a<b,'le':lambda:a<=b,'unord':lambda:math.isnan(a) or math.isnan(b)}
            if op in arithmetic:self.result(arithmetic[op](),'f' if kind=='sf' else 'd')
            else:self.result(int(compare[op]()))
            return
        if name=='___extendsfdf2vfp':self.result(self.float_arg(0),'d');return
        if name=='___truncdfsf2vfp':self.result(self.double_arg(0),'f');return
        m=re.fullmatch(r'___float(un)?si(sf|df)vfp',name)
        if m:
            value=self.arg(0)
            if not m[1] and value>=0x80000000:value-=0x100000000
            self.result(float(value),'f' if m[2]=='sf' else 'd');return
        m=re.fullmatch(r'___fix(sf|df)sivfp',name)
        if m:self.result(int(self.float_arg(0) if m[1]=='sf' else self.double_arg(0)),'i');return
        raise RuntimeError(f'Unsupported import {name} at {address:x}')
    def invoke(self,obj,selector,*args):
        self.uc.reg_write(UC_ARM_REG_SP,0x1ff000)
        self.uc.reg_write(UC_ARM_REG_LR,self.stop|1)
        self.uc.reg_write(UC_ARM_REG_R0,obj);self.uc.reg_write(UC_ARM_REG_R1,0)
        for i,a in enumerate(args):self.uc.reg_write(REGS[i+2],a)
        self.uc.emu_start(self.methods[self.objects[obj],selector]|1,self.stop,count=50000)
        if self.uc.reg_read(UC_ARM_REG_PC)!=self.stop:raise RuntimeError('Method did not return within instruction limit')
    def sound(self,uc,address,size,user):
        self.sound_events.append(self.arg(0));self.result(0)
    def allocate(self,owner):
        obj=self.next_object;self.next_object+=0x400;self.objects[obj]=owner
        return obj
    def new(self,owner):
        obj=self.allocate(owner)
        if (owner,'init') in self.methods:self.invoke(obj,'init')
        return obj
    def array(self,values):
        obj=self.allocate('NSMutableArray');self.arrays[obj]=list(values);return obj
    def set(self,obj,name,value):
        v=self.ivars[self.objects[obj],name];kind=v['type'] if v['type'] in ('f','d') else 'i'
        if isinstance(value,tuple):kind='f'*len(value)
        self.uc.mem_write(obj+v['offset'],struct.pack('<'+kind,*(value if isinstance(value,tuple) else (value,))))
    def get(self,obj,name):
        v=self.ivars[self.objects[obj],name];kind=v['type'] if v['type'] in ('f','d') else 'i'
        if v['type'].startswith('{'):kind='ff'
        out=struct.unpack('<'+kind,self.uc.mem_read(obj+v['offset'],struct.calcsize(kind)))
        return out if len(out)>1 else out[0]


if __name__=='__main__':
    vm=Original();p=vm.new('PlatformObject');vm.invoke(p,'setType:',1)
    vm.set(p,'objPosition',(293.,150.));vm.set(p,'isMoving',1);vm.set(p,'objSpeed',1.25)
    vm.invoke(p,'tick')
    print('Original platform tick:',vm.get(p,'objPosition'),'speed',vm.get(p,'objSpeed'))
    m=vm.new('MonsterObject');vm.invoke(m,'setType:',0);vm.set(m,'objPosition',(100.,200.))
    for _ in range(10):vm.invoke(m,'tick')
    print('Original monster after 10 ticks:',vm.get(m,'objPosition'))
