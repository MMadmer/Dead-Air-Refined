# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""The reference reconstruction of an axis' tube from its rings, exactly as FORMAT.md defines it for the engine:

  spine    centripetal Catmull-Rom through the ring centroids (phantom end points reflected);
  frame    t(u) = normalize(lerp(t_i, t_i+1, u)), n(u) = normalize(lerp(n_i, n_i+1, u)) made orthogonal to t(u),
           b(u) = cross(t(u), n(u));
  radius   r0(u) = monotone cubic Hermite (Fritsch-Carlson slopes over (s_i, r0_i)) with h = s_i+1 - s_i;
  profile  a_k, b_k lerped; P(u, th) = C(u) + r0(u) (1 + sum a_k cos k th + b_k sin k th) (cos th n(u) + sin th b(u)).
"""
import numpy as np

import geom as G


class Rings:
    """Ring arrays of one axis (index order = from the base)."""

    def __init__(self, c, r0, f, sides=None):
        self.c = np.asarray(c, np.float64).reshape(-1, 3)
        m = len(self.c)
        self.r0 = np.asarray(r0, np.float64).reshape(m)
        self.f = np.asarray(f, np.float64).reshape(m, 6)
        self.sides = np.zeros(m, np.int64) if sides is None else np.asarray(sides, np.int64)
        self.t = np.zeros((m, 3))
        self.n = np.zeros((m, 3))
        self.s = np.zeros(m)
        self.fit_err = np.zeros(m)
        self.frames()

    def __len__(self):
        return len(self.c)

    def frames(self, n0=None):
        m = len(self.c)
        if m == 0:
            return
        if m == 1:
            self.t[0] = (0, 1, 0)
            self.n[0] = G.any_perp(self.t[0]) if n0 is None else n0
            self.s[0] = 0
            return
        self.t = G.cr_tangents(self.c)
        if n0 is None:
            # the reference normal starts from model +X (or +Z for a near-X axis), projected
            n0 = G.any_perp(self.t[0])
        n = np.zeros((m, 3))
        n[0] = n0 - self.t[0] * np.dot(n0, self.t[0])
        n[0] /= np.linalg.norm(n[0])
        for i in range(1, m):
            n[i] = G.transport(n[i - 1], self.t[i - 1], self.t[i])
        self.n = n
        # arclength along the spline (32 sub-steps per segment)
        P, seg, u = G.catmull_rom(self.c, 32)
        d = np.linalg.norm(np.diff(P, axis=0), axis=1)
        cum = np.concatenate([[0], np.cumsum(d)])
        self.s = cum[np.arange(m) * 32]

    def subset(self, idx):
        r = Rings(self.c[idx], self.r0[idx], self.f[idx], self.sides[idx])
        r.fit_err = self.fit_err[idx]
        return r


def evaluate(R, n_sub=8, n_th=48, th=None):
    """Surface samples of the tube. Returns dict with P (n_seg*n_sub+1, n_th, 3), seg, u, centre C, radius etc."""
    m = len(R)
    if th is None:
        th = np.arange(n_th) * (2 * np.pi / n_th)
    if m < 2:
        C = R.c.copy()
        return {'P': C[:, None, :].repeat(len(th), 1), 'C': C, 'seg': np.zeros(1, np.int64), 'u': np.zeros(1),
                'th': th, 'r': np.zeros((1, len(th))), 'T': R.t.copy(), 'N': R.n.copy()}
    C, seg, u = G.catmull_rom(R.c, n_sub)
    i0, i1 = seg, np.minimum(seg + 1, m - 1)
    uu = u[:, None]
    T = R.t[i0] * (1 - uu) + R.t[i1] * uu
    T /= np.maximum(np.linalg.norm(T, axis=1), 1e-12)[:, None]
    N = R.n[i0] * (1 - uu) + R.n[i1] * uu
    N = N - T * np.einsum('ij,ij->i', N, T)[:, None]
    nn = np.linalg.norm(N, axis=1)
    bad = nn < 1e-9
    if bad.any():
        for k in np.flatnonzero(bad):
            N[k] = G.any_perp(T[k])
        nn = np.linalg.norm(N, axis=1)
    N /= nn[:, None]
    B = np.cross(T, N)
    slopes = G.monotone_slopes(R.s, R.r0)
    h = R.s[i1] - R.s[i0]
    r0 = G.hermite(R.r0[i0], R.r0[i1], slopes[i0], slopes[i1], h, u)
    r0 = np.maximum(r0, 0.0)
    F = R.f[i0] * (1 - uu) + R.f[i1] * uu
    prof = np.ones((len(u), len(th)))
    for k in range(1, 4):
        prof += F[:, 2 * k - 2, None] * np.cos(k * th)[None] + F[:, 2 * k - 1, None] * np.sin(k * th)[None]
    r = r0[:, None] * prof
    dirs = np.cos(th)[None, :, None] * N[:, None, :] + np.sin(th)[None, :, None] * B[:, None, :]
    P = C[:, None, :] + r[..., None] * dirs
    return {'P': P, 'C': C, 'seg': seg, 'u': u, 'th': th, 'r': r, 'T': T, 'N': N, 'B': B, 'r0': r0}


def mesh(ev):
    """Triangle mesh of evaluated samples (rows along the axis, columns around); returns (V, F, row of each face)."""
    P = ev['P']
    nr, nc = P.shape[0], P.shape[1]
    V = P.reshape(-1, 3)
    if nr < 2:
        return V, np.zeros((0, 3), np.int64), np.zeros(0, np.int64)
    i = np.arange(nr - 1)[:, None]
    j = np.arange(nc)[None, :]
    a = i * nc + j
    b = i * nc + (j + 1) % nc
    c = (i + 1) * nc + j
    d = (i + 1) * nc + (j + 1) % nc
    F = np.concatenate([np.stack([a, c, b], -1).reshape(-1, 3), np.stack([b, c, d], -1).reshape(-1, 3)])
    row = np.concatenate([np.repeat(np.arange(nr - 1), nc), np.repeat(np.arange(nr - 1), nc)])
    return V, F, row
