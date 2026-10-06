"""Zipper slider path in the bag frame (root locked to identity in game) and where it runs on the body:
nearest body surface points, their UVs, drawn over the bag texture.  python bp_path.py"""
import json
import os

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
d = json.load(open(os.path.join(HERE, 'bp_orig.json')))
V = np.array(d['mesh']['verts'])
G = np.array([g == 'zipper' for g in d['mesh']['group']])
F = d['mesh']['faces']
FUV = d['mesh']['face_uv']
zb = np.array(d['bones']['zipper']['matrix_local'])
print('zipper bind (blender arm space)\n', np.round(zb, 4))

paths = {}
# in game the root is held at its rest (identity in X-Ray): the slider in mesh space is rest @ body^-1 @ zipper
M0 = np.array(d['bones']['wpn_body']['matrix_local'])
for name, m in d['motions'].items():
    rel = []
    for fr in m['frames']:
        B = np.array(fr['wpn_body'])
        Z = np.array(fr['zipper'])
        rel.append(M0 @ np.linalg.inv(B) @ Z)
    rel = np.array(rel)
    paths[name] = rel
    p = rel[:, :3, 3]
    print(name, 'frames', len(rel), 'path len', round(float(np.linalg.norm(np.diff(p, axis=0), axis=1).sum()), 4),
          'bbox', np.round(p.min(0), 4), np.round(p.max(0), 4))
    if len(rel) > 1:
        for i in range(0, len(rel), max(1, len(rel) // 13)):
            print('   ', i, np.round(p[i], 4))

# body triangles for closest-point queries
tris, tuv = [], []
for f, uv in zip(F, FUV):
    if any(G[v] for v in f):
        continue
    for k in range(1, len(f) - 1):
        tris.append((V[f[0]], V[f[k]], V[f[k + 1]]))
        tuv.append((uv[0], uv[k], uv[k + 1]))
tris = np.array(tris)
tuv = np.array(tuv)


def closest(p):
    best = (1e9, None, None)
    for t, uv in zip(tris, tuv):
        a, b, c = t
        ab, ac, ap = b - a, c - a, p - a
        # project onto plane, clamp barycentrics coarsely
        n = np.cross(ab, ac)
        nn = np.dot(n, n)
        if nn < 1e-14:
            continue
        w = np.cross(ab, ap)
        v = np.cross(ap, ac)
        bc = np.array([0, np.dot(v, n) / nn, np.dot(w, n) / nn])
        bc[0] = 1 - bc[1] - bc[2]
        bc = np.clip(bc, 0, 1)
        bc /= bc.sum()
        q = bc[0] * a + bc[1] * b + bc[2] * c
        dist = np.linalg.norm(p - q)
        if dist < best[0]:
            best = (dist, q, bc[0] * uv[0] + bc[1] * uv[1] + bc[2] * uv[2])
    return best


# slider rest geometry centroid relative to its bone: where the slider body sits on the track
zv = V[G]
zl = (np.linalg.inv(zb) @ np.c_[zv, np.ones(len(zv))].T).T[:, :3]
print('slider verts in bone space: min', np.round(zl.min(0), 4), 'max', np.round(zl.max(0), 4))

tex = Image.open(os.path.join(HERE, 'act_morthbag.png')).convert('RGB')
W, H = tex.size
dr = ImageDraw.Draw(tex)
for name, rel in paths.items():
    if len(rel) < 2:
        continue
    pts = []
    for M in rel:
        # the slider's track point: bone origin
        dist, q, uv = closest(M[:3, 3])
        pts.append((uv[0] * W, (1 - uv[1]) * H))
        if len(pts) % 6 == 1:
            print(name, 'bone at', np.round(M[:3, 3], 4), 'surface dist', round(float(dist), 4), 'uv', np.round(uv, 4))
    dr.line(pts, fill=(255, 0, 0) if 'open' in name else (0, 255, 255), width=3)
tex.save(os.path.join(HERE, 'act_morthbag_path.png'))
np.savez(os.path.join(HERE, 'bp_paths.npz'), **{k: v for k, v in paths.items()})
print('ok')
