#!/usr/bin/env python3
"""Reproduce local method/ivar metadata from the inspected ARMv6 executable."""
import argparse,hashlib,json,re,struct,subprocess
from pathlib import Path

def inspect(binary,out):
    data=binary.read_bytes()
    if hashlib.sha256(data).hexdigest()!='68b29c08bcbe25e760dfd92aee05d92d3f3cae394789e7a029c53b210ff0e6d4':
        raise ValueError('This analysis targets Doodle Jump 1.0 only')
    segments=[];pos=28
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        cmd,size=struct.unpack_from('<II',data,pos)
        if cmd==1:
            va,_,off,length=struct.unpack_from('<4I',data,pos+24);segments.append((va,off,length))
        pos+=size
    def offset(va):
        for start,off,length in segments:
            if start<=va<start+length:return off+va-start
        raise ValueError(f'Unmapped address {va:x}')
    def word(va):return struct.unpack_from('<I',data,offset(va))[0]
    def string(va):
        p=offset(va);return data[p:data.index(b'\0',p)].decode('utf8')
    symbols=subprocess.check_output(['nm','-n',str(binary)],text=True)
    methods=[(int(a,16)&~1,n) for a,n in re.findall(r'^([0-9a-f]+) [tT] ([+-]\[.+\])$',symbols,re.M)]
    ivars=[]
    for a,owner in re.findall(r'^([0-9a-f]+) . _OBJC_CLASS_\$_([^\s]+)$',symbols,re.M):
        ro=word(int(a,16)+16)&~3;table=word(ro+28)
        if not table:continue
        stride,count=word(table),word(table+4)
        if stride!=20 or count>256:raise ValueError('Unexpected Objective-C ivar table')
        for i in range(count):
            entry=table+8+stride*i;address=word(entry)
            ivars.append(dict(address=hex(address),owner=owner,name=string(word(entry+4)),
                              offset=word(address),type=string(word(entry+8))))
    if len(methods)!=205 or len(ivars)!=184:raise ValueError(f'Unexpected coverage: {len(methods)} methods, {len(ivars)} ivars')
    out.mkdir(parents=True,exist_ok=True)
    (out/'symbols.txt').write_text(symbols)
    (out/'methods.tsv').write_text(''.join(f'{a:08x}\t{n}\n' for a,n in sorted(methods)))
    (out/'ivars.json').write_text(json.dumps(ivars,indent=2)+'\n')
    print(f'{len(methods)} methods and {len(ivars)} instance variables written to {out}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path)
    p.add_argument('--out',type=Path,default=Path('analysis'));a=p.parse_args();inspect(a.binary,a.out)
