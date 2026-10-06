"""The zipper centreline the slider actually rides: every key position of the slider bone over the open and
close cycles (bag frame, root at rest), ordered along the arch, a centripetal Catmull-Rom through them,
lightly smoothed, resampled every 0.5 mm. Saves zip_curve.npz {p, s, s_rest, s_open, keys}.
    python bp_zipcurve.py"""
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
paths = np.load(os.path.join(HERE, 'bp_paths.npz'))
keys = []
for name in ('open_b_backpack_itm', 'close_b_backpack_itm'):
    keys.extend(paths[name][:, :3, 3])
keys = np.array(keys)
# distinct positions only
uniq = []
for k in keys:
    if all(np.linalg.norm(k - u) > 3e-4 for u in uniq):
        uniq.append(k)
uniq = np.array(uniq)
rest = paths['open_b_backpack_itm'][0, :3, 3]
end = paths['open_b_backpack_itm'][-1, :3, 3]
# order along the arch by angle around its centre in the face plane (x, y)
c = np.array([0.006, 0.02])
ang = np.arctan2(uniq[:, 1] - c[1], uniq[:, 0] - c[0])
order = np.argsort(-ang)          # left (pi) to right (0)
pts = uniq[order]
print('keys', len(pts))
for p in pts:
    print('  ', np.round(p, 4))


def catmull(P, n_per=40, alpha=0.5):
    P = np.vstack([2 * P[0] - P[1], P, 2 * P[-1] - P[-2]])
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        t0 = 0.0
        t1 = t0 + max(np.linalg.norm(p1 - p0), 1e-6) ** alpha
        t2 = t1 + max(np.linalg.norm(p2 - p1), 1e-6) ** alpha
        t3 = t2 + max(np.linalg.norm(p3 - p2), 1e-6) ** alpha
        for t in np.linspace(t1, t2, n_per, endpoint=False):
            a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1
            a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2
            a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3
            b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2
            b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3
            out.append((t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2)
    out.append(P[-2])
    return np.array(out)


def resample(Q, step):
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(Q, axis=0), axis=1))]
    s = np.arange(0, d[-1], step)
    return np.array([np.interp(s, d, Q[:, k]) for k in range(3)]).T, s


dense = catmull(pts)
dense, _ = resample(dense, 0.0005)
# light smoothing, ends pinned
for _ in range(40):
    dense[1:-1] = dense[1:-1] * 0.5 + 0.25 * (dense[:-2] + dense[2:])
dense, s = resample(dense, 0.0005)
dk = [float(np.min(np.linalg.norm(dense - k, axis=1))) for k in pts]
print('curve length %.1f mm, key deviation max %.2f mm mean %.2f mm' % (s[-1] * 1e3, max(dk) * 1e3, np.mean(dk) * 1e3))
s_rest = s[np.argmin(np.linalg.norm(dense - rest, axis=1))]
s_open = s[np.argmin(np.linalg.norm(dense - end, axis=1))]
print('rest at %.1f mm, open end at %.1f mm' % (s_rest * 1e3, s_open * 1e3))
np.savez(os.path.join(HERE, 'zip_curve.npz'), p=dense, s=s, s_rest=s_rest, s_open=s_open, keys=pts)
