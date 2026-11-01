#!/usr/bin/env python3
"""Convert a local Doodle Jump 1.0 app into small, preloaded nano texture pages.

Requires Pillow and macOS sips for Apple's CgBI PNG variant. No game data is
embedded here. Scene data is extracted from the matching original executable.
"""
import argparse
import hashlib
import struct
import subprocess
import shutil
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def scenes(binary):
    data = binary.read_bytes()
    if hashlib.sha256(data).hexdigest() != '68b29c08bcbe25e760dfd92aee05d92d3f3cae394789e7a029c53b210ff0e6d4':
        raise ValueError('Use the inspected Doodle Jump 1.0 executable; other builds need analysis')
    # This extractor targets the inspected ARMv6 1.0 executable only.
    if len(data) < 0x17000 or data[:4] != b'\xce\xfa\xed\xfe':
        raise ValueError('Expected the original 32-bit Mach-O executable')
    segments = []
    pos = 28
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, pos)
        if cmd == 1:
            va, _, off, length = struct.unpack_from('<4I', data, pos + 24)
            segments.append((va, off, length))
        pos += size

    def offset(va):
        for start, off, length in segments:
            if start <= va < start + length:
                return off + va - start
        raise ValueError('Bad scene pointer')

    def word(va):
        return struct.unpack_from('<I', data, offset(va))[0]

    out = bytearray(b'DJS1' + struct.pack('<I', 19))
    for address in range(0x91d4, 0x9220, 4):
        p = offset(word(word(address) + 8))
        text = data[p:data.index(b'\0', p)].decode('ascii')
        rows = [tuple(map(int, row.split(','))) for row in text.split('|')]
        if not 1 <= len(rows) <= 64 or any(len(r) != 3 or not 0 <= r[0] <= 9 or
                not 0 <= r[1] <= 320 or not 0 <= r[2] <= 2000 for r in rows):
            raise ValueError('Unexpected scene layout: use the inspected 1.0 build')
        out += struct.pack('<I', len(rows))
        for row in rows:
            out += struct.pack('<3h', *row)
    return out


def convert(app, output, scale):
    output.mkdir(parents=True, exist_ok=True)
    decoded = output.parent / 'png'
    decoded.mkdir(exist_ok=True)
    images, tiles = [], []
    sources=[]
    for source in sorted(app.glob('*.png')):
        if source.stem in ('Default', 'Icon'):
            continue
        dest = decoded / source.name
        subprocess.run(['sips', '-s', 'format', 'png', str(source), '--out', str(dest)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        sources.append((source.stem,Image.open(dest).convert('RGBA')))
    # UILabel replacements for the offline scores page. These are platform UI,
    # explicitly separate from the original artwork and gameplay.
    font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',16)
    for name,text in (('local-score-label','Local high score'),('offline-label','Online scores unavailable')):
        bounds=font.getbbox(text);im=Image.new('RGBA',(bounds[2]+2,bounds[3]-bounds[1]+2))
        ImageDraw.Draw(im).text((1,1-bounds[1]),text,font=font,fill=(40,40,40,255))
        sources.append((name,im))
    for name,original in sources:
        w, h = original.size
        im = original.resize((max(1, round(w*scale)), max(1, round(h*scale))), Image.Resampling.LANCZOS)
        image_id = len(images)
        images.append((name, w, h))
        # Tiles include an extruded pixel on every edge. Texture pages never exceed
        # the dimensions already tested with the Angry Birds renderer on the nano.
        for y in range(0, im.height, 124):
            for x in range(0, im.width, 252):
                crop = im.crop((x, y, min(x+252, im.width), min(y+124, im.height)))
                tiles.append(dict(image=image_id, x=x, y=y, im=crop))
    pages, shelves = [], []
    for tile in sorted(tiles, key=lambda t: -t['im'].height):
        im = tile['im']; w, h = im.size
        placement = None
        for pi, shelf in enumerate(shelves):
            for row in shelf:
                if row[2] >= h+2 and row[1]+w+2 <= 256:
                    placement = pi, row[1], row[0]
                    row[1] += w+2
                    break
            if placement:
                break
            bottom = sum(row[2] for row in shelf)
            if bottom+h+2 <= 128:
                shelf.append([bottom, w+2, h+2]); placement = pi, 0, bottom
                break
        if placement is None:
            placement = len(pages), 0, 0
            pages.append(Image.new('RGBA', (256, 128)))
            shelves.append([[0, w+2, h+2]])
        pi, x, y = placement
        page = pages[pi]
        padded = Image.new('RGBA', (w+2, h+2))
        padded.paste(im, (1, 1))
        padded.paste(im.crop((0, 0, 1, h)).resize((1, h+2)), (0, 0))
        padded.paste(im.crop((w-1, 0, w, h)).resize((1, h+2)), (w+1, 0))
        padded.paste(im.crop((0, 0, w, 1)), (1, 0))
        padded.paste(im.crop((0, h-1, w, h)), (1, h+1))
        page.paste(padded, (x, y))
        tile['packed'] = pi, x+1, y+1
    if len(pages) > 32:
        raise ValueError('Texture budget exceeded')
    parts, records = bytearray(), bytearray()
    for image_id, (name, w, h) in enumerate(images):
        start = len(parts)//16
        for t in tiles:
            if t['image'] == image_id:
                pi, x, y = t['packed']
                parts += struct.pack('<8H', pi, x, y, t['im'].width, t['im'].height, t['x'], t['y'], 0)
        records += struct.pack('<40s4H', name.encode(), w, h, start, len(parts)//16-start)
    header = struct.pack('<4s4Hf', b'DJA1', len(pages), len(images), len(parts)//16, 2, scale)
    (output/'sprites.bin').write_bytes(header+records+parts)
    for i, page in enumerate(pages):
        raw = bytearray()
        # Premultiplied RGBA8888 keeps the original pale paper and fine outlines.
        for r, g, b, a in page.getdata():
            raw += bytes((r*a//255, g*a//255, b*a//255, a))
        (output/f'page{i:02}.bin').write_bytes(raw)
    (output/'scenes.bin').write_bytes(scenes(app/'DoodleJump'))
    paths = ['sprites.bin', 'scenes.bin'] + [f'page{i:02}.bin' for i in range(len(pages))]
    for wav in sorted(app.glob('*.wav')):
        shutil.copyfile(wav,output/wav.name);paths.append(wav.name)
    (output/'files.lst').write_text(''.join(f'{p} {(output/p).stat().st_size}\n' for p in paths))
    print(f'{len(images)} sprites, {len(tiles)} pieces, {len(pages)} pages, {len(pages)*128} KiB texture')
    print('Executable SHA256:', hashlib.sha256((app/'DoodleJump').read_bytes()).hexdigest())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--scale', type=float, default=.75)
    args = parser.parse_args()
    if not .25 <= args.scale <= 1:
        parser.error('scale must be between 0.25 and 1')
    convert(args.app, args.output, args.scale)
