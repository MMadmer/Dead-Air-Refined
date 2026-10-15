# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""level.cform and gamemtl.xr readers, and the collision of sampled placements of a kind.

level.cform: hdrCFORM {u32 version 4, u32 verts, u32 faces, Fbox}, Fvector verts[], CDB::TRI {u32 v[3];
u32 material:14, suppress_shadows:1, suppress_wm:1, sector:16}; material = the game material id of gamemtl.xr.
xrLC copies the trunk faces of an MU model whose compile shader collides (materials\\tree_trunk) into the CFORM,
transformed by the placement, and simplifies the whole model by edge collapses; crowns carry a passable hull
(materials\\bush / bush_sux)."""
import os
import struct

import numpy as np

import common as C

TRUNK_MTL = ('materials\\tree_trunk',)
HULL_MTL = ('materials\\bush', 'materials\\bush_sux')


def chunks(buf, off=0, end=None):
    end = len(buf) if end is None else end
    out = []
    while off + 8 <= end:
        cid, size = struct.unpack_from('<II', buf, off)
        out.append((cid & 0x7FFFFFFF, off + 8, size))
        off += 8 + size
    return out


def read_gamemtl(path=C.GAMEMTL):
    b = open(path, 'rb').read()
    top = {cid: (o, s) for cid, o, s in chunks(b)}
    o, s = top[0x1002]
    out = {}
    for cid, mo, ms in chunks(b, o, o + s):
        cm = {k: (oo, ss) for k, oo, ss in chunks(b, mo, mo + ms)}
        po, _ = cm[0x1000]
        mid = struct.unpack_from('<I', b, po)[0]
        e = b.index(b'\0', po + 4)
        out[mid] = b[po + 4:e].decode('cp1251')
    return out


def read_cform(level):
    b = open(os.path.join(C.LEVELS, level, 'level.cform'), 'rb').read()
    ver, nv, nf = struct.unpack_from('<III', b, 0)
    if ver != 4:
        raise ValueError(f'{level}: level.cform version {ver}')
    off = 36
    V = np.frombuffer(b, '<f4', nv * 3, off).reshape(nv, 3).astype(np.float64)
    off += nv * 12
    T = np.frombuffer(b, '<u4', nf * 4, off).reshape(nf, 4)
    return V, T[:, :3].astype(np.int64), (T[:, 3] & 0x3FFF).astype(np.int64)


class LevelCform:
    """The vegetation-material triangles of one level (trunk collision and passable crown hulls)."""

    def __init__(self, level, mtl):
        V, F, M = read_cform(level)
        names = {m: mtl.get(m, f'#{m}') for m in np.unique(M)}
        trunk = np.array([names[m] in TRUNK_MTL for m in M])
        hull = np.array([names[m] in HULL_MTL for m in M])
        keep = trunk | hull
        self.F = F[keep]
        self.kind = np.where(trunk[keep], 1, 2)  # 1 trunk, 2 hull
        self.V = V
        T = V[self.F]
        self.lo = T.min(1)
        self.hi = T.max(1)

    def in_box(self, bmin, bmax):
        sel = np.all((self.lo <= bmax) & (self.hi >= bmin), axis=1)
        idx = np.flatnonzero(sel)
        return self.V[self.F[idx]], self.kind[idx]


def world_box(xform, bmin, bmax, margin=0.25):
    corners = np.array([[x, y, z, 1.0] for x in (bmin[0], bmax[0]) for y in (bmin[1], bmax[1])
                        for z in (bmin[2], bmax[2])])
    W = corners @ xform
    return W[:, :3].min(0) - margin, W[:, :3].max(0) + margin


def to_world(P, xform):
    return P @ xform[:3, :3] + xform[3, :3]


def to_model(Pw, xform):
    return (Pw - xform[3, :3]) @ np.linalg.inv(xform[:3, :3])


def pick_placements(instances, n_max=12, prefer_kept=True):
    """Up to n_max placements spread round-robin over levels (stock-kept ones first)."""
    by_level = {}
    for i, inst in enumerate(instances):
        by_level.setdefault(inst['level'], []).append(i)
    for lv in by_level:
        by_level[lv].sort(key=lambda i: (instances[i]['replaced'] if prefer_kept else 0, i))
    out = []
    levels = sorted(by_level, key=lambda lv: -len(by_level[lv]))
    k = 0
    while len(out) < n_max and any(by_level.values()):
        lv = levels[k % len(levels)]
        if by_level[lv]:
            out.append(by_level[lv].pop(0))
        k += 1
        if k > 10 * n_max * max(1, len(levels)):
            break
    return [instances[i] for i in out]
