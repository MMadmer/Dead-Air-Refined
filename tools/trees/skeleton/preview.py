# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Rebuild a skeleton straight from its .dtsk file (FORMAT.md reconstruction) and render it: tubes by level,
patches, card branch records, crown field. An independent check of what the engine will read.

    python tools/trees/skeleton/preview.py <skel> [...] [--obj]     -> REPORT_PNG/preview_<skel>.png (+ .obj)
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import common as C  # noqa: E402
import dtsk as DT  # noqa: E402
import render as RD  # noqa: E402
import tube as TB  # noqa: E402


class FileRings:
    """Ring arrays read from the file, in the shape tube.evaluate expects."""

    def __init__(self, r):
        self.c = r['c'].astype(np.float64)
        self.t = r['t'].astype(np.float64)
        self.n = r['n'].astype(np.float64)
        self.s = r['s'].astype(np.float64)
        self.r0 = r['r0'].astype(np.float64)
        self.f = r['f'].astype(np.float64)

    def __len__(self):
        return len(self.c)


def meshes(sk, n_sub=6, n_th=16):
    out = []
    for i, a in enumerate(sk.axes):
        r = sk.rings[a['first_ring']:a['first_ring'] + a['n_rings']]
        V, F, _ = TB.mesh(TB.evaluate(FileRings(r), n_sub, n_th))
        out.append((i, a, V, F))
    return out


def write_obj(path, sk, ms):
    lines = []
    off = 1
    for i, a, V, F in ms:
        lines.append(f'o axis_{i}_level{a["level"]}')
        lines += [f'v {x:.4f} {y:.4f} {-z:.4f}' for x, y, z in V]
        lines += [f'f {p + off} {r + off} {q + off}' for p, q, r in F]
        off += len(V)
    if len(sk.ptris):
        lines.append('o patches')
        lines += [f'v {x:.4f} {y:.4f} {-z:.4f}' for x, y, z in sk.pverts['p']]
        lines += [f'f {p + off} {r + off} {q + off}' for p, q, r in sk.ptris]
    C.atomic_text(path, '\n'.join(lines) + '\n', crlf=False)


def render(sid, write_obj_file=False):
    data = open(os.path.join(C.OUT_DTSK, f'{sid}.dtsk'), 'rb').read()
    sk = DT.unpack(data)
    bad = DT.validate(sk)
    ms = meshes(sk)
    bmin, bmax = sk.bmin.astype(np.float64), sk.bmax.astype(np.float64)
    cols = {0: (205, 170, 120), 1: (175, 140, 100), 2: (90, 170, 80)}
    panels = []
    for view in ('front', 'side'):
        cv = RD.Canvas(640, 820, view, bmin, bmax)
        for i, a, V, F in ms:
            cv.tris(V, F, np.array([cols[a['level']]]))
        if len(sk.ptris):
            nb = int(sk.crown['reserved'])
            P = sk.pverts['p'].astype(np.float64)
            T = sk.ptris.astype(np.int64)
            col = np.where((np.arange(len(T)) < nb)[:, None], np.array([[235, 135, 30]]), np.array([[250, 230, 90]]))
            cv.tris(P, T, col)
        for c in sk.cards[:4000]:
            b = c['base'].astype(np.float64)
            cv.lines(np.stack([b, b + c['dir'] * c['len']]), (40, 120, 40), 1)
        cv.label(f'{sid} {view}: from the file (tubes by level, patches orange, caps yellow, card records green)')
        panels.append(cv.flush())
    img = Image.new('RGB', (1280, 820))
    img.paste(panels[0], (0, 0))
    img.paste(panels[1], (640, 0))
    C.atomic_png(os.path.join(C.REPORT_PNG, f'preview_{sid}.png'), img)
    if write_obj_file:
        write_obj(os.path.join(C.REPORT_PNG, f'preview_{sid}.obj'), sk, ms)
    return bad


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    for sid in args:
        problems = render(sid, '--obj' in sys.argv)
        print(sid, 'valid' if not problems else problems)
