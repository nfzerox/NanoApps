#!/usr/bin/env python3
"""A small PVRTC 4-bits-per-pixel encoder (the format the nano takes large textures in).

Each 4x4 block stores two colours and a 2-bit weight per pixel; the decoder spreads the two
colour sets smoothly across neighbouring blocks and mixes them by the weight. This encoder
takes the darkest and brightest corner of each block's colour range as its two colours (with
care at the edges of sprites) and gives every pixel the nearest of the four mixes: quick,
and good enough for sprite sheets shown at a fraction of their size.
"""
import struct
import numpy as np


def _interleave(x, y):
    """Block order in the file: x in the odd bits, y in the even bits."""
    out = np.zeros_like(x)
    for bit in range(12):
        out |= ((y >> bit) & 1) << (2 * bit)
        out |= ((x >> bit) & 1) << (2 * bit + 1)
    return out


def encode_level(rgba):
    """rgba: square power-of-two image, at least 8x8, as a (h, w, 4) uint8 array."""
    h, w, _ = rgba.shape
    bh, bw = h // 4, w // 4
    blocks = rgba.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 4).astype(np.int32)
    lo, hi = blocks.min(axis=2), blocks.max(axis=2)
    # A block's two colours reach every pixel within four of its centre (an 8x8 window that
    # starts two pixels before the block). Where that window holds a fully clear pixel the
    # low colour is clear, so clear pixels beside a sprite stay clear whatever the neighbours
    # are; and a block that is all clear borrows the window's high colour, so the sprite's
    # edge beside it is not dimmed.
    shifted = np.roll(rgba, (2, 2), axis=(0, 1))
    cells = shifted.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 4).astype(np.int32)

    def window(c, pick):
        right, down = np.roll(c, -1, axis=1), np.roll(c, -1, axis=0)
        return pick(pick(c, right), pick(down, np.roll(down, -1, axis=1)))
    window_lo, window_hi = window(cells.min(axis=2), np.minimum), window(cells.max(axis=2), np.maximum)
    edge = window_lo[..., 3] == 0
    # with the low colour clear, a solid pixel can only take the high colour: make that the
    # average of the block's solid pixels rather than its brightest corner
    solid = blocks[..., 3:4] >= np.maximum(hi[..., None, 3:4] * 9 // 10, 1)
    mean = (blocks * solid).sum(axis=2) // np.maximum(solid.sum(axis=2), 1)
    lo = np.where(edge[..., None], 0, lo)
    hi = np.where((hi[..., 3] == 0)[..., None], window_hi, np.where(edge[..., None], mean, hi))
    opaque = lo[..., 3] == 255

    def ceil_bits(v, bits):
        return np.minimum((v + (1 << (8 - bits)) - 1) >> (8 - bits), (1 << bits) - 1)

    # opaque blocks: A = R5 G5 B4, B = R5 G5 B5; others: A = A3 R4 G4 B3, B = A3 R4 G4 B4
    a_op = (1 << 14) | (lo[..., 0] >> 3) << 9 | (lo[..., 1] >> 3) << 4 | (lo[..., 2] >> 4)
    b_op = (1 << 15) | ceil_bits(hi[..., 0], 5) << 10 | ceil_bits(hi[..., 1], 5) << 5 | ceil_bits(hi[..., 2], 5)
    a_tr = (lo[..., 3] >> 5) << 11 | (lo[..., 0] >> 4) << 7 | (lo[..., 1] >> 4) << 3 | (lo[..., 2] >> 5)
    b_tr = ceil_bits(hi[..., 3], 3) << 12 | ceil_bits(hi[..., 0], 4) << 8 | ceil_bits(hi[..., 1], 4) << 4 | ceil_bits(hi[..., 2], 4)
    colour_a = np.where(opaque, a_op, a_tr)        # 15 bits
    colour_b = np.where(opaque, b_op, b_tr)        # 16 bits

    def expand(v, bits):
        v = v.astype(np.float64)
        return v * 255.0 / ((1 << bits) - 1)

    def decode_a():
        r = np.where(opaque, expand((colour_a >> 9) & 31, 5), expand((colour_a >> 7) & 15, 4))
        g = np.where(opaque, expand((colour_a >> 4) & 31, 5), expand((colour_a >> 3) & 15, 4))
        b = np.where(opaque, expand(colour_a & 15, 4), expand(colour_a & 7, 3))
        a = np.where(opaque, 255.0, expand((colour_a >> 11) & 7, 3))
        return np.stack([r, g, b, a], axis=-1)

    def decode_b():
        r = np.where(opaque, expand((colour_b >> 10) & 31, 5), expand((colour_b >> 8) & 15, 4))
        g = np.where(opaque, expand((colour_b >> 5) & 31, 5), expand((colour_b >> 4) & 15, 4))
        b = np.where(opaque, expand(colour_b & 31, 5), expand(colour_b & 15, 4))
        a = np.where(opaque, 255.0, expand((colour_b >> 12) & 7, 3))
        return np.stack([r, g, b, a], axis=-1)

    def upscale(c):
        """The decoder's view: each block's colour sits at its pixel (2, 2); bilinear between, wrapping."""
        ys, xs = (np.arange(h) - 2) / 4.0, (np.arange(w) - 2) / 4.0
        y0, x0 = np.floor(ys).astype(int), np.floor(xs).astype(int)
        ty, tx = (ys - y0)[:, None, None], (xs - x0)[None, :, None]
        y0, y1, x0, x1 = y0 % bh, (y0 + 1) % bh, x0 % bw, (x0 + 1) % bw
        top = c[y0][:, x0] * (1 - tx) + c[y0][:, x1] * tx
        bottom = c[y1][:, x0] * (1 - tx) + c[y1][:, x1] * tx
        return top * (1 - ty) + bottom * ty

    a_up, b_up = upscale(decode_a()), upscale(decode_b())
    # each pixel takes whichever of the four mixes is nearest
    pixels = rgba.astype(np.float64)
    best, level = None, None
    for index, weight in enumerate((0.0, 3 / 8, 5 / 8, 1.0)):
        error = ((a_up + (b_up - a_up) * weight - pixels) ** 2).sum(axis=-1)
        if best is None:
            best, level = error, np.zeros(error.shape, dtype=np.uint32)
        else:
            closer = error < best
            best = np.where(closer, error, best)
            level = np.where(closer, np.uint32(index), level)
    shifts = (2 * (4 * np.arange(4)[:, None] + np.arange(4)[None, :])).astype(np.uint32)
    mod = (level.reshape(bh, 4, bw, 4).transpose(0, 2, 1, 3) << shifts).sum(axis=(2, 3)).astype(np.uint32)
    colour = (colour_a.astype(np.uint32) << 1) | (colour_b.astype(np.uint32) << 16)
    by, bx = np.meshgrid(np.arange(bh), np.arange(bw), indexing='ij')
    order = _interleave(bx, by)
    out = np.zeros((bh * bw, 2), dtype='<u4')
    out[order.ravel(), 0] = mod.ravel()
    out[order.ravel(), 1] = colour.ravel()
    return out.tobytes()


def encode_pvr(image, mipmaps=True):
    """A PVR (version 2) file holding the PIL image as PVRTC 4bpp with its mipmap chain.
    The image must be square with a power-of-two side; alpha is kept if any pixel has it."""
    from PIL import Image
    image = image.convert('RGBA')
    w, h = image.size
    if w != h or w & (w - 1) or w < 8:
        raise ValueError('PVRTC wants a square power-of-two image of at least 8 pixels')
    alpha = image.getextrema()[3][0] < 255
    levels, level = [], image
    while True:
        side = level.size[0]
        source = level if side >= 8 else level.resize((8, 8), Image.NEAREST)
        levels.append(encode_level(np.asarray(source, dtype=np.uint8)))
        if side == 1 or not mipmaps:
            break
        level = level.resize((side // 2, side // 2), Image.BOX)
    data = b''.join(levels)
    flags = 0x19 | (0x100 if len(levels) > 1 else 0) | (0x8000 if alpha else 0)
    header = struct.pack('<11I4sI', 52, h, w, len(levels) - 1, flags, len(data), 4, 0, 0, 0, 1 if alpha else 0, b'PVR!', 1)
    return header + data
