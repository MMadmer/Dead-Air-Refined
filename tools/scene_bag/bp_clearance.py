"""Clearance of the hands over the front, per 1 cm cell: min over all frames of (hand z - surface z) for hand
vertices above the cell. Prints a map (mm, '--' no hand ever over it)."""
import numpy as np, json
H = np.load('hands_wpn_hand_00.npz')
c = np.load('contact_wpn_hand_00.npz')
d = json.load(open('bp_orig.json'))
V = np.array(d['mesh']['verts']); G = np.array([g == 'zipper' for g in d['mesh']['group']])
F = [f for f in d['mesh']['faces'] if not any(G[v] for v in f)]
# surface height z(x,y) of the front by sampling the gauge triangles
from math import inf
tris = []
for f in F:
    for k in range(1, len(f) - 1):
        tris.append((V[f[0]], V[f[k]], V[f[k + 1]]))
def zsurf(x, y):
    best = -inf
    for a, b, cc in tris:
        v0, v1 = b - a, cc - a
        den = v0[0] * v1[1] - v1[0] * v0[1]
        if abs(den) < 1e-12: continue
        px, py = x - a[0], y - a[1]
        s = (px * v1[1] - v1[0] * py) / den
        t = (v0[0] * py - px * v0[1]) / den
        if s >= 0 and t >= 0 and s + t <= 1:
            best = max(best, a[2] + s * v0[2] + t * v1[2])
    return best
xs = np.arange(-0.12, 0.125, 0.01); ys = np.arange(0.18, -0.175, -0.01)
P = np.concatenate([H['open'].reshape(-1, 3), H['close'].reshape(-1, 3)])
print('     ' + ''.join('%5.0f' % (x * 1000) for x in xs))
for y in ys:
    row = []
    for x in xs:
        z = zsurf(x, y)
        if z == -inf: row.append('   ..'); continue
        m = (np.abs(P[:, 0] - x) < 0.005) & (np.abs(P[:, 1] - y) < 0.005) & (P[:, 2] > z - 0.03)
        if not m.any(): row.append('   --'); continue
        g = (P[m, 2] - z).min() * 1000
        row.append('%5.0f' % g)
    print('%5.0f' % (y * 1000) + ''.join(row))
