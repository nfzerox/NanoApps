#!/usr/bin/env python3
"""Put the original yellow bird sprite in the existing NanoApps icon frame."""
import argparse,plistlib,sys
from pathlib import Path
from PIL import Image

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--atlas',type=Path,required=True,help='decoded atlas.png (build/png/atlas.png after convert_assets.py)')
    p.add_argument('--atlas-text',type=Path,required=True,help='the app\'s atlas.txt')
    p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    sys.path.insert(0,str(a.sdk.resolve()/'tools'));import build_apps
    atlas=Image.open(a.atlas).convert('RGBA')
    for line in a.atlas_text.read_text().splitlines():
        f=line.split()
        if f and f[0]=='bird0_1':
            w,h,x,y=int(f[1]),int(f[2]),round(float(f[3])*1024),round(float(f[4])*1024)
            art=atlas.crop((x,y,x+w,y+h));break
    else:raise ValueError('bird0_1 not in the atlas')
    art=art.crop(art.getchannel('A').getbbox())
    art=art.resize((art.width*4,art.height*4),Image.Resampling.NEAREST)   # keep the pixel art crisp
    metadata=Path(__file__).resolve().parents[1]/'Info.plist'
    color=plistlib.loads(metadata.read_bytes())['HBIconColor'].removeprefix('#')
    rgb=tuple(int(color[i:i+2],16) for i in (0,2,4))
    a.out.parent.mkdir(parents=True,exist_ok=True);build_apps.make_png(None,rgb,a.out,art=art)
    print('Wrote',a.out)
if __name__=='__main__':main()
