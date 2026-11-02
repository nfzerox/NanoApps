#!/usr/bin/env python3
"""Convert a local Flappy Bird 1.2 app into small nano texture pages.

The original keeps every image in one 1024x1024 atlas (atlas.png + atlas.txt). The nano's
GL driver is only proven with 256x128 pages, so each image is cut into pieces that fit one
and repacked; the pieces keep the original resolution (the game draws at its own 288x512
and the whole canvas is scaled to the panel, as the original scales it to the phone).

Requires Pillow, and macOS sips/afconvert for Apple's PNG variant and the sound formats.
No game data is embedded here.
"""
import argparse
import struct
import subprocess
from pathlib import Path
from PIL import Image
from app_binary import thin_armv7

# Never drawn by 1.2: the solid overlays are drawn as plain quads, the green pipes are the
# only pipes used.
UNUSED = {'black', 'white', 'pipe2_down', 'pipe2_up'}
SOUNDS = ('sfx_point.aif', 'sfx_swooshing.caf', 'sfx_die.caf', 'sfx_hit.caf', 'sfx_wing.caf')
PAGE_W, PAGE_H = 256, 128


def pack(tiles):
    """Shelf-pack tiles (each with a 1 px extruded border) into pages; returns the pages."""
    pages, shelves = [], []
    for tile in sorted(tiles, key=lambda t: -t['im'].height):
        im = tile['im']
        w, h = im.size
        placement = None
        for pi, shelf in enumerate(shelves):
            for row in shelf:
                if row[2] >= h + 2 and row[1] + w + 2 <= PAGE_W:
                    placement = pi, row[1], row[0]
                    row[1] += w + 2
                    break
            if placement:
                break
            bottom = sum(row[2] for row in shelf)
            if bottom + h + 2 <= PAGE_H:
                shelf.append([bottom, w + 2, h + 2])
                placement = pi, 0, bottom
                break
        if placement is None:
            placement = len(pages), 0, 0
            pages.append(Image.new('RGBA', (PAGE_W, PAGE_H)))
            shelves.append([[0, w + 2, h + 2]])
        pi, x, y = placement
        padded = Image.new('RGBA', (w + 2, h + 2))
        padded.paste(im, (1, 1))
        padded.paste(im.crop((0, 0, 1, h)).resize((1, h + 2)), (0, 0))
        padded.paste(im.crop((w - 1, 0, w, h)).resize((1, h + 2)), (w + 1, 0))
        padded.paste(im.crop((0, 0, w, 1)), (1, 0))
        padded.paste(im.crop((0, h - 1, w, h)), (1, h + 1))
        padded.putpixel((0, 0), im.getpixel((0, 0)))
        padded.putpixel((w + 1, 0), im.getpixel((w - 1, 0)))
        padded.putpixel((0, h + 1), im.getpixel((0, h - 1)))
        padded.putpixel((w + 1, h + 1), im.getpixel((w - 1, h - 1)))
        pages[pi].paste(padded, (x, y))
        tile['packed'] = pi, x + 1, y + 1
    return pages


def convert(app, output):
    thin_armv7(app / 'flap')
    output.mkdir(parents=True, exist_ok=True)
    decoded = output.parent / 'png'
    decoded.mkdir(exist_ok=True)
    subprocess.run(['sips', '-s', 'format', 'png', str(app / 'atlas.png'), '--out', str(decoded / 'atlas.png')],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    atlas = Image.open(decoded / 'atlas.png').convert('RGBA')
    if atlas.size != (1024, 1024):
        raise ValueError('unexpected atlas size')

    images = []      # in atlas.txt order: the game finds animation frames by position
    for line in (app / 'atlas.txt').read_text(encoding='ascii').splitlines():
        parts = line.split()
        if len(parts) != 7:
            continue
        name, w, h = parts[0], int(parts[1]), int(parts[2])
        x, y = round(float(parts[3]) * 1024), round(float(parts[4]) * 1024)
        if not (0 < w <= 1024 and 0 < h <= 1024 and x + w <= 1024 and y + h <= 1024) or len(name) > 31:
            raise ValueError('unexpected atlas entry: ' + line)
        im = None if name in UNUSED else atlas.crop((x, y, x + w, y + h))
        opaque = im is not None and im.getchannel('A').getextrema()[0] == 255
        images.append(dict(name=name, w=w, h=h, im=im, opaque=opaque))

    groups = {False: [], True: []}
    for image_id, image in enumerate(images):
        if image['im'] is None:
            continue
        for y in range(0, image['h'], PAGE_H - 4):
            for x in range(0, image['w'], PAGE_W - 4):
                crop = image['im'].crop((x, y, min(x + PAGE_W - 4, image['w']), min(y + PAGE_H - 4, image['h'])))
                groups[image['opaque']].append(dict(image=image_id, x=x, y=y, im=crop))
    # Opaque images (the backgrounds, mostly) go to 16-bit pages; the rest keep 8 bits a
    # channel with premultiplied alpha.
    page_formats, files, tiles = [], [], []
    for opaque in (True, False):
        pages = pack(groups[opaque])
        for t in groups[opaque]:
            pi, x, y = t['packed']
            t['packed'] = pi + len(page_formats), x, y
            tiles.append(t)
        for page in pages:
            raw = bytearray()
            if opaque:
                for r, g, b, a in page.getdata():
                    raw += struct.pack('<H', (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
            else:
                for r, g, b, a in page.getdata():
                    raw += bytes((r * a // 255, g * a // 255, b * a // 255, a))
            name = 'page%02d.bin' % len(page_formats)
            (output / name).write_bytes(raw)
            files.append(name)
            page_formats.append(0 if opaque else 2)
    if len(page_formats) > 40:
        raise ValueError('texture budget exceeded')

    parts, records = bytearray(), bytearray()
    for image_id, image in enumerate(images):
        start = len(parts) // 16
        for t in tiles:
            if t['image'] == image_id:
                pi, x, y = t['packed']
                parts += struct.pack('<8H', pi, x, y, t['im'].width, t['im'].height, t['x'], t['y'], 0)
        records += struct.pack('<32s6H', image['name'].encode(), image['w'], image['h'], start,
                               len(parts) // 16 - start, 1 if image['opaque'] else 0, 0)
    header = struct.pack('<4s4H', b'FBA1', len(page_formats), len(images), len(parts) // 16, 0)
    (output / 'sprites.bin').write_bytes(header + bytes(page_formats) + bytes(-len(page_formats) % 4) + records + parts)
    files.insert(0, 'sprites.bin')

    for sound in SOUNDS:
        wav = Path(sound).with_suffix('.wav').name
        subprocess.run(['afconvert', '-f', 'WAVE', '-d', 'LEI16', str(app / sound), str(output / wav)], check=True)
        files.append(wav)
    (output / 'files.lst').write_text(''.join('%s %d\n' % (p, (output / p).stat().st_size) for p in files))
    texture = sum(65536 if f == 0 else 131072 for f in page_formats)
    print('%d images, %d pieces, %d pages (%d opaque), %d KiB texture' %
          (len(images), len(tiles), len(page_formats), page_formats.count(0), texture // 1024))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    convert(args.app, args.output)
