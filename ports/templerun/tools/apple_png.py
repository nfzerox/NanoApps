"""Read the app's PNG files, which are in Apple's "CgBI" variant."""
import struct
from PIL import Image


def read_png(path):
    """A PNG as premultiplied RGBA. The app's PNGs are in Apple's "CgBI" variant (raw
    deflate, BGRA, already premultiplied), which ordinary readers reject."""
    data = path.read_bytes()
    if data[12:16] != b'CgBI':
        return premultiplied(Image.open(path))
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            w, h, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if (depth, colour, interlace) != (8, 6, 0):
                raise ValueError('unexpected PNG layout: %s' % path.name)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    import zlib
    raw = zlib.decompressobj(-15).decompress(idat)
    stride = w * 4
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - 4] if i >= 4 else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return Image.frombytes('RGBA', (w, h), bytes(out), 'raw', 'BGRA')


def premultiplied(im):
    im = im.convert('RGBA')
    px = bytearray(im.tobytes())
    for i in range(0, len(px), 4):
        a = px[i + 3]
        if a != 255:
            px[i] = (px[i] * a + 127) // 255
            px[i + 1] = (px[i + 1] * a + 127) // 255
            px[i + 2] = (px[i + 2] * a + 127) // 255
    return Image.frombytes('RGBA', im.size, bytes(px))
