# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""The .dtsk skeleton file, version 1 (tools/trees/FORMAT.md): writer, reader and validator.

Little-endian, every float f32. Sections in order: header 64 B, crown 32 B, axes 32 B each, rings 76 B each,
patch vertices 40 B each, patch triangles 12 B each, cards 48 B each, crown grid nx*ny*nz bytes."""
import struct

import numpy as np

MAGIC = b'DTSK'
VERSION = 1

# header flags
H_CONIFER, H_DEAD, H_BROKEN_TOP, H_CUT_BASE, H_PROCEDURAL = 1, 2, 4, 8, 16
# axis flags
A_FROM_CARDS, A_CUT_END, A_CAP, A_SYNTHETIC = 1, 2, 4, 8
NO_AXIS = 0xFFFFFFFF

HEADER = struct.Struct('<4sIQ3f3f5II')
CROWN = struct.Struct('<3I3ffI')
AXIS = struct.Struct('<iIfIIfII')
RING = np.dtype([('c', '<f4', 3), ('t', '<f4', 3), ('n', '<f4', 3), ('s', '<f4'), ('r0', '<f4'), ('f', '<f4', 6),
                 ('stock_sides', '<u4'), ('fit_err', '<f4')])
PVERT = np.dtype([('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2), ('axis', '<u4'), ('s', '<f4')])
CARD = np.dtype([('base', '<f4', 3), ('dir', '<f4', 3), ('up', '<f4', 3), ('len', '<f4'), ('width', '<f4'),
                 ('axis', '<u4')])
assert HEADER.size == 64 and CROWN.size == 32 and AXIS.size == 32
assert RING.itemsize == 76 and PVERT.itemsize == 40 and CARD.itemsize == 48


class Skeleton:
    def __init__(self):
        self.trunk_key = 0
        self.bmin = np.zeros(3)
        self.bmax = np.zeros(3)
        self.flags = 0
        self.crown = {'n': (0, 0, 0), 'origin': np.zeros(3), 'cell': 0.0, 'reserved': 0}
        self.axes = []            # dicts: parent, level, attach, first_ring, n_rings, climbable, flags, reserved
        self.rings = np.zeros(0, RING)
        self.pverts = np.zeros(0, PVERT)
        self.ptris = np.zeros((0, 3), np.uint32)
        self.cards = np.zeros(0, CARD)
        self.grid = b''


def pack(sk):
    nx, ny, nz = sk.crown['n']
    head = HEADER.pack(MAGIC, VERSION, int(sk.trunk_key) & ((1 << 64) - 1), *map(float, sk.bmin), *map(float, sk.bmax),
                       len(sk.axes), len(sk.rings), len(sk.pverts), len(sk.ptris), len(sk.cards), int(sk.flags))
    crown = CROWN.pack(nx, ny, nz, *map(float, sk.crown['origin']), float(sk.crown['cell']), int(sk.crown['reserved']))
    ax = b''.join(AXIS.pack(int(a['parent']), int(a['level']), float(a['attach']), int(a['first_ring']),
                            int(a['n_rings']), float(a['climbable']), int(a['flags']), int(a['reserved']) & 0xFFFFFFFF)
                  for a in sk.axes)
    grid = sk.grid if isinstance(sk.grid, (bytes, bytearray)) else bytes(sk.grid)
    if len(grid) != nx * ny * nz:
        raise ValueError(f'crown grid {len(grid)} bytes, expected {nx * ny * nz}')
    return b''.join([head, crown, ax, sk.rings.astype(RING).tobytes(), sk.pverts.astype(PVERT).tobytes(),
                     np.ascontiguousarray(sk.ptris, '<u4').tobytes(), sk.cards.astype(CARD).tobytes(), grid])


def unpack(data):
    sk = Skeleton()
    magic, ver, key, *rest = HEADER.unpack_from(data, 0)
    if magic != MAGIC or ver != VERSION:
        raise ValueError(f'not a DTSK v1 file: {magic!r} {ver}')
    sk.trunk_key = key
    sk.bmin = np.array(rest[0:3])
    sk.bmax = np.array(rest[3:6])
    na, nr, npv, npt, nc, sk.flags = rest[6:12]
    o = HEADER.size
    nx, ny, nz, ox, oy, oz, cell, res = CROWN.unpack_from(data, o)
    sk.crown = {'n': (nx, ny, nz), 'origin': np.array([ox, oy, oz]), 'cell': cell, 'reserved': res}
    o += CROWN.size
    for i in range(na):
        p, lv, at, fr, n, cl, fl, rs = AXIS.unpack_from(data, o)
        sk.axes.append({'parent': p, 'level': lv, 'attach': at, 'first_ring': fr, 'n_rings': n, 'climbable': cl,
                        'flags': fl, 'reserved': rs})
        o += AXIS.size
    sk.rings = np.frombuffer(data, RING, nr, o).copy()
    o += nr * RING.itemsize
    sk.pverts = np.frombuffer(data, PVERT, npv, o).copy()
    o += npv * PVERT.itemsize
    sk.ptris = np.frombuffer(data, '<u4', npt * 3, o).reshape(-1, 3).copy()
    o += npt * 12
    sk.cards = np.frombuffer(data, CARD, nc, o).copy()
    o += nc * CARD.itemsize
    sk.grid = data[o:o + nx * ny * nz]
    o += nx * ny * nz
    if o != len(data):
        raise ValueError(f'size mismatch: parsed {o} of {len(data)} bytes')
    return sk


def expected_size(na, nr, npv, npt, nc, nx, ny, nz):
    return 64 + 32 + 32 * na + RING.itemsize * nr + 40 * npv + 12 * npt + 48 * nc + nx * ny * nz


def validate(sk):
    """Invariants the engine relies on; returns a list of problems (empty = valid)."""
    bad = []
    nr = len(sk.rings)
    used = np.zeros(nr, bool)
    for i, a in enumerate(sk.axes):
        if a['parent'] >= i:
            bad.append(f'axis {i}: parent {a["parent"]} not before it')
        if a['parent'] < -1:
            bad.append(f'axis {i}: parent {a["parent"]}')
        if a['first_ring'] + a['n_rings'] > nr or a['n_rings'] < 2:
            bad.append(f'axis {i}: rings {a["first_ring"]}+{a["n_rings"]} of {nr}')
            continue
        r = sk.rings[a['first_ring']:a['first_ring'] + a['n_rings']]
        used[a['first_ring']:a['first_ring'] + a['n_rings']] = True
        if not (0.0 <= a['climbable'] <= 1.0):
            bad.append(f'axis {i}: climbable {a["climbable"]}')
        if a['level'] > 2:
            bad.append(f'axis {i}: level {a["level"]}')
        if a['parent'] >= 0:
            pa = sk.axes[a['parent']]
            pr = sk.rings[pa['first_ring']:pa['first_ring'] + pa['n_rings']]
            if a['attach'] < -1e-3 or a['attach'] > pr['s'][-1] + 1e-3:
                bad.append(f'axis {i}: attach {a["attach"]:.3f} outside the parent 0..{pr["s"][-1]:.3f}')
        if np.any(np.diff(r['s']) < -1e-5) or abs(r['s'][0]) > 1e-4:
            bad.append(f'axis {i}: arclength not increasing from 0')
        tn = np.linalg.norm(r['t'], axis=1)
        nn = np.linalg.norm(r['n'], axis=1)
        if np.any(np.abs(tn - 1) > 1e-3) or np.any(np.abs(nn - 1) > 1e-3):
            bad.append(f'axis {i}: non-unit t/n')
        if np.any(np.abs(np.einsum('ij,ij->i', r['t'], r['n'])) > 2e-3):
            bad.append(f'axis {i}: n not normal to t')
        if np.any(r['r0'] < 0) or not np.all(np.isfinite(r['f'])):
            bad.append(f'axis {i}: bad radius/profile')
        # (A profile's harmonics past 0.6 of its mean radius turn the tube inside out; a limb's ring past two and a
        # half times its parent's widest is a misfit - the engine holds both, a fit must not make them.)
        amp = np.sqrt(r['f'][:, 0::2] ** 2 + r['f'][:, 1::2] ** 2).sum(axis=1)
        if np.any(amp > 0.6 + 1e-4):
            bad.append(f'axis {i}: {int((amp > 0.6 + 1e-4).sum())} profiles past 0.6 of the radius')
        if a['parent'] >= 0 and not a['flags'] & A_FROM_CARDS:
            pa = sk.axes[a['parent']]
            widest = float(sk.rings[pa['first_ring']:pa['first_ring'] + pa['n_rings']]['r0'].max())
            if np.any(r['r0'] > max(2.5 * widest, 0.02)):
                bad.append('axis %d: rings past 2.5 times the parent' % i + "'s widest (%.3f m against %.3f m)" % (float(r['r0'].max()), widest))
    if nr and not used.all():
        bad.append(f'{int((~used).sum())} rings belong to no axis')
    if len(sk.ptris) and (sk.ptris.max() >= len(sk.pverts)):
        bad.append('patch triangle index out of range')
    if len(sk.pverts) and np.any(sk.pverts['axis'] >= max(len(sk.axes), 1)):
        bad.append('patch vertex axis out of range')
    for i, c in enumerate(sk.cards):
        if c['axis'] != NO_AXIS and c['axis'] >= len(sk.axes):
            bad.append(f'card {i}: axis {c["axis"]}')
    nx, ny, nz = sk.crown['n']
    if len(sk.grid) != nx * ny * nz:
        bad.append('crown grid size')
    if sk.crown['reserved'] > len(sk.ptris):
        bad.append('bark patch triangle count above the patch triangles')
    return bad
