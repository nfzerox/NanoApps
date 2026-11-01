#!/usr/bin/env python3
"""Put the original app icon artwork in the existing NanoApps icon frame."""
import argparse,plistlib,subprocess,sys,tempfile
from pathlib import Path
from PIL import Image

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,required=True);p.add_argument('--source',type=Path,required=True,help='the original .app')
    p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    sys.path.insert(0,str(a.sdk.resolve()/'tools'));import build_apps
    with tempfile.TemporaryDirectory(prefix='tr-icon-') as temporary:
        decoded=Path(temporary)/'icon.png'
        subprocess.run(['sips','-s','format','png',str(a.source/'Icon@2x.png'),'--out',str(decoded)],check=True,stdout=subprocess.DEVNULL)
        art=Image.open(decoded).convert('RGBA')
        # the idol's face fills the middle of the square icon; the round frame crops the corners
        w,h=art.size;art=art.crop((w//10,h//10,w-w//10,h-h//10))
        metadata=Path(__file__).resolve().parents[1]/'port/nano/app/Info.plist'
        color=plistlib.loads(metadata.read_bytes())['HBIconColor'].removeprefix('#')
        rgb=tuple(int(color[i:i+2],16) for i in (0,2,4))
        a.out.parent.mkdir(parents=True,exist_ok=True);build_apps.make_png(None,rgb,a.out,art=art)
    print('Wrote',a.out)
if __name__=='__main__':main()
