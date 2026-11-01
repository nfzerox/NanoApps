#!/usr/bin/env python3
"""Read Temple Run's .bksb model files (cMesh3D::loadFileBin, versions 3 and 4)."""
import struct
import sys
from pathlib import Path


class Mesh:
    pass


def load(path):
    d = Path(path).read_bytes()
    assert d[0:1] == b'v', path
    version = struct.unpack_from('<i', d, 1)[0]
    assert version in (3, 4), (path, version)
    p = 5
    m = Mesh()
    m.version = version

    def i32():
        nonlocal p
        v = struct.unpack_from('<i', d, p)[0]
        p += 4
        return v

    def u8():
        nonlocal p
        v = d[p]
        p += 1
        return v

    nframes = i32()
    m.frames = [struct.unpack_from('<4i', d, p + 16 * k) for k in range(nframes)]   # vcount, icount, vstart, istart
    p += 16 * nframes
    m.mesh_type = i32()
    total = i32()
    m.vertex_size = 0
    m.pos = m.normal = m.color = None
    m.tex = []
    m.vertex_data = b''
    if total > 0:
        m.vertex_size = i32()
        m.pos = (i32(), i32(), i32(), i32())            # type, components, stride, offset
        if u8():
            m.normal = (i32(), i32(), i32(), i32())
        if u8():
            m.color = (i32(), i32(), i32(), i32())
        channels = i32() if version == 4 else u8()
        for _ in range(channels):
            m.tex.append((i32(), i32(), i32(), i32()))
        m.vertex_data = d[p:p + total * m.vertex_size]
        p += total * m.vertex_size
    m.vertex_count = total
    nindex = i32()
    m.indices = ()
    if nindex > 0:
        itype, a, b = i32(), i32(), i32()
        assert itype == 0
        m.indices = struct.unpack_from('<%dH' % nindex, d, p)
        p += 2 * nindex
    assert p == len(d), (path, p, len(d))
    return m


def vertex(m, i):
    base = i * m.vertex_size
    out = {'pos': struct.unpack_from('<3f', m.vertex_data, base + m.pos[3])}
    if m.normal:
        out['normal'] = struct.unpack_from('<3f', m.vertex_data, base + m.normal[3])
    if m.color:
        out['color'] = struct.unpack_from('<4B', m.vertex_data, base + m.color[3])
    out['uv'] = [struct.unpack_from('<2f', m.vertex_data, base + t[3]) for t in m.tex]
    return out


if __name__ == '__main__':
    for path in sys.argv[1:]:
        m = load(path)
        xs = [vertex(m, i)['pos'] for i in range(m.frames[0][0])] if m.vertex_count else []
        lo = tuple(round(min(v[k] for v in xs), 1) for k in range(3)) if xs else None
        hi = tuple(round(max(v[k] for v in xs), 1) for k in range(3)) if xs else None
        print('%-38s v%d frames=%3d verts/frame=%5d tris=%5d vsize=%2d n=%d c=%d uv=%d  %s..%s' % (
            Path(path).name, m.version, len(m.frames), m.frames[0][0], m.frames[0][1] // 3, m.vertex_size,
            bool(m.normal), bool(m.color), len(m.tex), lo, hi))
