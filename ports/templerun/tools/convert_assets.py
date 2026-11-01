#!/usr/bin/env python3
"""Prepare the original app's data for the port. Everything written here comes from the
original app and stays untracked.

  <out>/host   decoded textures for the Mac build (<name>.rgba: width, height, RGBA8)
  <out>/nano   what goes on the iPod: the engine image and tunables (tools/make_image.py),
               the model, atlas, font and level files as they are, the PVRTC textures as
               they are, the PNG sheets re-encoded as PVRTC (tools/pvrtc.py), and files.lst

usage: convert_assets.py <out directory>
"""
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from PIL import Image
import texture2ddecoder

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pvrtc     # noqa: E402
from apple_png import read_png     # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'original/v1.0/Payload/TempleRun.app'
PVR = ['terrainTexture', 'playerTexture', 'playerGlowTexture', 'enemyTexture', 'wallTexture', 'treeTexture', 'lightMapTexture']
# name the engine loads -> (largest side on the iPod)
# The sheets are halved to 512 and get no smaller levels. Three 1024-pixel sheets were more
# than the iPod's driver would take: it rebooted the first time the third one was drawn with,
# whichever that was, and no other texture is sent with a level above 512 (the loader leaves
# out the largest level of the 1024-pixel scenery). On a 240-pixel panel a 512 sheet is still
# shown smaller than it is. (The tutorial sheet is not used: the tutorial is off.)
SHEET = 512
PNG = {'interfaceTexture': SHEET, 'effectsTexture': SHEET, 'tutorialTexture': 64, 'fontNumbers': 256, 'fontCountdown': 256}

out = Path(sys.argv[1])
host, nano = out / 'host', out / 'nano'
for d in (host, nano):
    d.mkdir(parents=True, exist_ok=True)


for name in PVR:
    data = (APP / (name + '.pvr')).read_bytes()
    header, h, w, mips, flags, size = struct.unpack_from('<6I', data)
    if header != 52 or data[44:48] != b'PVR!' or flags & 0xff != 0x19:
        raise ValueError('unexpected PVR file: ' + name)
    bgra = texture2ddecoder.decode_pvrtc(data[52:52 + w * h // 2], w, h, 0)
    im = Image.frombytes('RGBA', (w, h), bgra, 'raw', 'BGRA')
    (host / (name + '.rgba')).write_bytes(struct.pack('<II', w, h) + im.tobytes())
    shutil.copyfile(APP / (name + '.pvr'), nano / (name + '.pvr'))

# The PNG sheets go to the iPod as PVRTC too: its driver reboots on large uncompressed
# textures (the other ports keep those to 256x128). The Mac build is given the decoded result,
# so it shows what the iPod will.
for name, side in PNG.items():
    im = read_png(APP / (name + '.png'))
    if max(im.size) > side:
        im = im.resize((side, side), Image.BOX)     # plain averaging: nothing spreads past a pixel
    pvr = pvrtc.encode_pvr(im, mipmaps=False)
    (nano / (name + '.pvr')).write_bytes(pvr)
    w, h = im.size
    decoded = Image.frombytes('RGBA', (w, h), texture2ddecoder.decode_pvrtc(pvr[52:52 + w * h // 2], w, h, 0), 'raw', 'BGRA')
    (host / (name + '.rgba')).write_bytes(struct.pack('<II', w, h) + decoded.tobytes())
    stale = nano / (name + '.4444')
    if stale.exists():
        stale.unlink()

# The title, pause and game-over screens were iPhone (UIKit) views built from loose PNGs; the
# port draws its own from the same pictures, packed here into one sheet with a list of where
# each one is. The numbers are set in the app's own font.
def build_ui_sheet():
    from PIL import ImageDraw, ImageFont
    sheet = Image.new('RGBA', (1024, 1024))
    places = []

    def put(name, image, x, y, size=None):
        if size:
            image = image.resize(size, Image.LANCZOS)
        sheet.paste(image, (x, y))
        places.append('%s %d %d %d %d' % (name, x, y, image.width, image.height))

    def art(name):
        big = APP / (name + '@2x.png')
        return read_png(big if big.exists() else APP / (name + '.png'))

    put('logo', art('mainMenuLogo'), 0, 0)
    put('idol', art('mainMenuIdol'), 664, 0)
    put('coin', art('coinLarge'), 672, 292)
    put('runAgain', art('endGamePlayAgainButton'), 0, 372)
    put('paused', art('pausedHeader'), 564, 372)
    put('resume', art('pausedResumeButton'), 0, 508)
    put('score', art('endGameScore'), 564, 500)
    put('panel', read_png(APP / 'endGameBackground.png'), 0, 644, (246, 380))
    deaths = ['FallA', 'Water', 'Tree', 'Slide', 'Ledge', 'Burnt', 'Eaten', 'Tangle', 'Fossil']
    for i, name in enumerate(deaths):
        put('death' + name, read_png(APP / ('deathIllustration%s.png' % name)), 262 + 252 * (i % 3), 644 + 128 * (i // 3))
    font = ImageFont.truetype(str(APP / 'Cheboyga.ttf'), 40)
    x = 564
    for ch in '0123456789,m':
        box = font.getbbox(ch)
        w = box[2] + 2
        glyph = Image.new('RGBA', (w, 44))
        ImageDraw.Draw(glyph).text((1, 0), ch, font=font, fill=(255, 255, 255, 255))
        put('glyph' + ('Comma' if ch == ',' else ch), glyph, x, 592)
        x += w + 6
    if x > 1024:
        raise ValueError('the digits do not fit the sheet')
    box = font.getbbox('BEST')
    word = Image.new('RGBA', (box[2] + 2, 44))
    ImageDraw.Draw(word).text((1, 0), 'BEST', font=font, fill=(255, 255, 255, 255))
    if 744 + word.width > 1024:
        raise ValueError('the label does not fit the sheet')
    put('best', word, 744, 300)
    return sheet, places


sheet, places = build_ui_sheet()            # laid out at 1024 (ui.txt is in those units), sent at half that
sheet = sheet.resize((SHEET, SHEET), Image.BOX)
pvr = pvrtc.encode_pvr(sheet, mipmaps=False)
(nano / 'uiSheet.pvr').write_bytes(pvr)
(nano / 'ui.txt').write_text('\n'.join(places) + '\n')
decoded = Image.frombytes('RGBA', (SHEET, SHEET), texture2ddecoder.decode_pvrtc(pvr[52:], SHEET, SHEET, 0), 'raw', 'BGRA')
(host / 'uiSheet.rgba').write_bytes(struct.pack('<II', SHEET, SHEET) + decoded.tobytes())

# The loading screen is the phone's launch picture (Default.png), cut to the panel's shape
# and squeezed into a square texture; like the scenery it has its smaller levels.
launch = read_png(APP / 'Default@2x.png').convert('RGB')
lw, lh = launch.size
cw = lh * 240 // 432
launch = launch.crop(((lw - cw) // 2, 0, (lw - cw) // 2 + cw, lh)).resize((512, 512), Image.LANCZOS)
pvr = pvrtc.encode_pvr(launch, mipmaps=True)
(nano / 'splash.pvr').write_bytes(pvr)
decoded = Image.frombytes('RGBA', (512, 512), texture2ddecoder.decode_pvrtc(pvr[52:52 + 512 * 512 // 2], 512, 512, 0), 'raw', 'BGRA')
(host / 'splash.rgba').write_bytes(struct.pack('<II', 512, 512) + decoded.tobytes())

for stale in ('testMip256', 'testMip1024'):     # test textures of earlier builds
    for f in (nano / (stale + '.pvr'), host / (stale + '.rgba')):
        if f.exists():
            f.unlink()

for pattern in ('*.bksb', '*.atlas', '*.fnt', 'modelRegistry.lvl'):
    for f in sorted(APP.glob(pattern)):
        shutil.copyfile(f, nano / f.name)
subprocess.run([sys.executable, str(ROOT / 'tools/make_image.py'), str(nano)], check=True)
files = sorted(f for f in nano.iterdir() if f.name != 'files.lst')
(nano / 'files.lst').write_text(''.join('%s %d\n' % (f.name, f.stat().st_size) for f in files))
print('%d files for the iPod, %.1f MB' % (len(files), sum(f.stat().st_size for f in files) / 1e6))
