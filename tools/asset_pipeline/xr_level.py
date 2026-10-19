"""Reader for the parts of a compiled X-Ray level that the LOD re-render needs.

level       shaders (chunk 2), visuals (chunk 3: one OGF per visual)
level.geom  vertex buffers (chunk 9, D3D9 declarations), index buffers (chunk 10), sliding windows (chunk 11)
"""
import os
import struct

import numpy as np

MT_NORMAL, MT_HIERRARHY, MT_PROGRESSIVE, MT_LOD, MT_TREE_ST, MT_TREE_PM = 0, 1, 2, 6, 7, 11
OGF_HEADER, OGF_CHILDREN_L, OGF_LODDEF2, OGF_TREEDEF2, OGF_SWIDATA, OGF_SWICONTAINER, OGF_GCONTAINER = 1, 10, 11, 12, 6, 20, 21

# D3DDECLTYPE -> (numpy dtype, components)
DECL_TYPES = {0: ('<f4', 1), 1: ('<f4', 2), 2: ('<f4', 3), 3: ('<f4', 4), 4: ('u1', 4), 5: ('u1', 4), 6: ('<i2', 2),
              7: ('<i2', 4), 8: ('u1', 4), 9: ('<i2', 2), 10: ('<i2', 4), 11: ('<u2', 2), 12: ('<u2', 4), 13: ('<u4', 1),
              14: ('<u4', 1), 15: ('<f2', 2), 16: ('<f2', 4)}
USAGE = {0: 'POSITION', 3: 'NORMAL', 5: 'TEXCOORD', 6: 'TANGENT', 7: 'BINORMAL', 10: 'COLOR'}
TREE_QUANT = 32768 / 16


def chunks(buf, off=0, end=None):
    end = len(buf) if end is None else end
    out = []
    while off + 8 <= end:
        cid, size = struct.unpack_from('<II', buf, off)
        out.append((cid & 0x7FFFFFFF, off + 8, size))
        off += 8 + size
    return out


def chunk_map(buf, off=0, end=None):
    return {cid: (o, s) for cid, o, s in chunks(buf, off, end)}


class VB:
    def __init__(self, decl, count, data):
        self.decl, self.count, self.data = decl, count, data
        self.stride = max(e[1] + np.dtype(DECL_TYPES[e[2]][0]).itemsize * DECL_TYPES[e[2]][1] for e in decl)

    def element(self, usage, index=0):
        for (stream, off, typ, _, use, idx) in self.decl:
            if use == usage and idx == index:
                dt, n = DECL_TYPES[typ]
                return off, np.dtype(dt), n, typ
        return None

    def read(self, usage, base, count, index=0):
        e = self.element(usage, index)
        if e is None:
            return None
        off, dt, n, typ = e
        raw = np.frombuffer(self.data, np.uint8, count * self.stride, base * self.stride).reshape(count, self.stride)
        col = raw[:, off:off + dt.itemsize * n].copy().view(dt).reshape(count, n)
        return col, typ


class Level:
    def __init__(self, folder):
        self.folder = folder
        b = open(os.path.join(folder, 'level'), 'rb').read()
        top = chunk_map(b)
        # Shaders: "shader/texture[,texture...]"
        o, s = top[2]
        n = struct.unpack_from('<I', b, o)[0]
        p = o + 4
        self.shaders = []
        for _ in range(n):
            q = b.index(b'\0', p)
            self.shaders.append(b[p:q].decode('cp1251', 'replace'))
            p = q + 1
        # Visuals
        o, s = top[3]
        self.visuals = []
        for cid, vo, vs in chunks(b, o, o + s):
            self.visuals.append(self._visual(b, vo, vs))
        g = open(os.path.join(folder, 'level.geom'), 'rb').read()
        gt = chunk_map(g)
        self.vbs = self._vbs(g, *gt[9])
        self.ibs = self._ibs(g, *gt[10])
        self.swis = self._swis(g, *gt[11]) if 11 in gt else []

    @staticmethod
    def _visual(b, o, s):
        cm = chunk_map(b, o, o + s)
        ho, _ = cm[OGF_HEADER]
        ver, typ, shader = struct.unpack_from('<BBH', b, ho)
        box = struct.unpack_from('<6f', b, ho + 4)
        sph = struct.unpack_from('<4f', b, ho + 28)
        v = {'type': typ, 'shader': shader, 'box': box, 'sphere': sph}
        if OGF_CHILDREN_L in cm:
            co, _ = cm[OGF_CHILDREN_L]
            cnt = struct.unpack_from('<I', b, co)[0]
            v['children'] = list(struct.unpack_from(f'<{cnt}I', b, co + 4))
        if OGF_GCONTAINER in cm:
            go, _ = cm[OGF_GCONTAINER]
            v['gc'] = struct.unpack_from('<6I', b, go)
        if OGF_TREEDEF2 in cm:
            to, _ = cm[OGF_TREEDEF2]
            v['xform'] = np.array(struct.unpack_from('<16f', b, to), np.float64).reshape(4, 4)
        if OGF_SWICONTAINER in cm:
            so, _ = cm[OGF_SWICONTAINER]
            v['swi'] = struct.unpack_from('<I', b, so)[0]
        if OGF_LODDEF2 in cm:
            lo, ls = cm[OGF_LODDEF2]
            stride = ls // 32
            facets = []
            for f in range(8):
                quad = []
                for k in range(4):
                    q = lo + (f * 4 + k) * stride
                    x, y, z, u, w = struct.unpack_from('<5f', b, q)
                    rgbh = struct.unpack_from('<I', b, q + 20)[0]
                    sun = b[q + 24]
                    quad.append(((x, y, z), (u, w), rgbh, sun))
                facets.append(quad)
            v['facets'] = facets
            v['lod_stride'] = stride
        return v

    @staticmethod
    def _vbs(g, o, s):
        n = struct.unpack_from('<I', g, o)[0]
        p = o + 4
        out = []
        for _ in range(n):
            decl = []
            while True:
                e = struct.unpack_from('<HHBBBB', g, p)
                p += 8
                if e[0] == 0xFF:
                    break
                decl.append(e)
            cnt = struct.unpack_from('<I', g, p)[0]
            p += 4
            vb = VB(decl, cnt, memoryview(g)[p:])
            nbytes = cnt * vb.stride
            vb.data = memoryview(g)[p:p + nbytes]
            p += nbytes
            out.append(vb)
        return out

    @staticmethod
    def _ibs(g, o, s):
        n = struct.unpack_from('<I', g, o)[0]
        p = o + 4
        out = []
        for _ in range(n):
            cnt = struct.unpack_from('<I', g, p)[0]
            p += 4
            out.append(np.frombuffer(g, '<u2', cnt, p))
            p += cnt * 2
        return out

    @staticmethod
    def _swis(g, o, s):
        n = struct.unpack_from('<I', g, o)[0]
        p = o + 4
        out = []
        for _ in range(n):
            p += 16  # reserved
            cnt = struct.unpack_from('<I', g, p)[0]
            p += 4
            sw = [struct.unpack_from('<IHH', g, p + k * 8) for k in range(cnt)]
            p += cnt * 8
            out.append(sw)
        return out

    def texture_of(self, v):
        sh = self.shaders[v['shader']] if v['shader'] < len(self.shaders) else ''
        if '/' not in sh:
            return sh, ''
        name, tex = sh.split('/', 1)
        return name, tex.split(',')[0]

    def tree_mesh(self, v):
        """World-space triangles of a tree visual: (P[n,3], UV[n,2], tris[m,3])."""
        vb_i, v_base, v_cnt, ib_i, i_base, i_cnt = v['gc']
        vb = self.vbs[vb_i]
        if v['type'] == MT_TREE_PM and 'swi' in v:
            off, ntris, nverts = self.swis[v['swi']][0]
            idx = np.asarray(self.ibs[ib_i][i_base + off:i_base + off + ntris * 3], np.int64)
        else:
            idx = np.asarray(self.ibs[ib_i][i_base:i_base + i_cnt], np.int64)
        P, _ = vb.read(0, v_base, v_cnt)
        P = P[:, :3].astype(np.float64)
        tc, typ = vb.read(5, v_base, v_cnt)
        uv = tc[:, :2].astype(np.float64)
        if typ in (6, 7):
            uv = uv / TREE_QUANT
        M = v.get('xform')
        if M is not None:
            P = np.c_[P, np.ones(len(P))] @ M
            P = P[:, :3]
        return P, uv, idx.reshape(-1, 3)

    def normal_mesh(self, v):
        """World-space triangles of a static (non-tree) visual."""
        vb_i, v_base, v_cnt, ib_i, i_base, i_cnt = v['gc']
        vb = self.vbs[vb_i]
        if v['type'] == MT_PROGRESSIVE and 'swi' in v:
            off, ntris, nverts = self.swis[v['swi']][0]
            idx = np.asarray(self.ibs[ib_i][i_base + off:i_base + off + ntris * 3], np.int64)
        else:
            idx = np.asarray(self.ibs[ib_i][i_base:i_base + i_cnt], np.int64)
        P, _ = vb.read(0, v_base, v_cnt)
        tc, typ = vb.read(5, v_base, v_cnt)
        uv = tc[:, :2].astype(np.float64)
        if typ in (6, 7, 9, 10):
            uv = uv / 2048.0
        return P[:, :3].astype(np.float64), uv, idx.reshape(-1, 3)


def detail_objects(path):
    """Detail objects of a level.details: [(flags, min_scale, max_scale, verts [n,5] = x,y,z,u,v)]."""
    b = open(path, 'rb').read()
    cm = chunk_map(b)
    o, s = cm[1]
    out = []
    for cid, q, ss in chunks(b, o, o + s):
        e = b.index(b'\0', q)
        q = e + 1
        e = b.index(b'\0', q)
        q = e + 1
        flags, mn, mx, nv, ni = struct.unpack_from('<IffII', b, q)
        q += 20
        V = np.frombuffer(b, '<f4', nv * 5, q).reshape(nv, 5).astype(np.float64)
        out.append((flags, mn, mx, V))
    return out
