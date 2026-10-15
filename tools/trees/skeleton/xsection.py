# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Ring cross-sections: the stock surface cut by the ring plane, its radial function and the Fourier profile
r(th) = r0 * (1 + sum_k a_k cos k th + b_k sin k th), k <= 3.

Placement (AAA_TREES.md 6.3): a round tube over an n-gon misses the flat faces somewhere; the minimax profile sits
halfway between the faces and the vertices (half the worst case of a ring through the vertices). Rings with 8 or
more stock sides go through the stock vertices (least squares) when that stays within the tolerance. The harmonics
are limited to k < n/2 so the profile never copies the n-gon's own facets."""
import numpy as np

NTH = 180
TH = np.arange(NTH) * (2 * np.pi / NTH)


def tol_of(r):
    """Shape tolerance of the refit: 2 cm or 5 % of the radius, whichever is larger."""
    return np.maximum(0.02, 0.05 * np.asarray(r, np.float64))


def basis(th, K):
    cols = [np.ones_like(th)]
    for k in range(1, K + 1):
        cols += [np.cos(k * th), np.sin(k * th)]
    return np.stack(cols, 1)


def radial_function(Q2, closed):
    """Radius of the polygon/polyline Q2 (m, 2; origin = ring centre) along the rays TH: outermost crossing, nan
    where a ray misses. Also the number of crossings per ray (> 1 = not star-shaped)."""
    m = len(Q2)
    if m < 2:
        return np.full(NTH, np.nan), np.zeros(NTH, np.int64)
    A = Q2
    B = np.roll(Q2, -1, 0) if closed else Q2[1:]
    A = A if closed else Q2[:-1]
    d = np.stack([np.cos(TH), np.sin(TH)], 1)            # (NTH, 2)
    e = B - A                                            # (E, 2)
    # Solve A + e*t = d*r  ->  [d, -e] [r, t]^T = A
    det = d[:, None, 0] * (-e[None, :, 1]) - d[:, None, 1] * (-e[None, :, 0])
    with np.errstate(divide='ignore', invalid='ignore'):
        r = (A[None, :, 0] * (-e[None, :, 1]) - A[None, :, 1] * (-e[None, :, 0])) / det
        t = (d[:, None, 0] * A[None, :, 1] - d[:, None, 1] * A[None, :, 0]) / det
    ok = np.isfinite(r) & (r > 0) & (t >= -1e-9) & (t <= 1 + 1e-9)
    rr = np.where(ok, r, -np.inf).max(1)
    n = ok.sum(1)
    rr[~np.isfinite(rr)] = np.nan
    return rr, n


def polygon_area_centroid(Q):
    """Area centroid of a closed planar polygon given in 2D."""
    x, y = Q[:, 0], Q[:, 1]
    x1, y1 = np.roll(x, -1), np.roll(y, -1)
    cr = x * y1 - x1 * y
    a = cr.sum() / 2
    if abs(a) < 1e-12:
        return Q.mean(0), 0.0
    cx = ((x + x1) * cr).sum() / (6 * a)
    cy = ((y + y1) * cr).sum() / (6 * a)
    return np.array([cx, cy]), abs(a)


def lawson_minimax(Phi, y, iters=60):
    """Chebyshev (minimax) linear fit by Lawson's iteratively reweighted least squares (weighted normal equations;
    the best iterate is kept)."""
    n = len(y)
    w = np.full(n, 1.0 / n)
    c = np.linalg.lstsq(Phi, y, rcond=None)[0]
    best, best_e = c, float(np.abs(y - Phi @ c).max())
    ridge = 1e-12 * np.eye(Phi.shape[1])
    for _ in range(iters):
        try:
            c = np.linalg.solve(Phi.T @ (Phi * w[:, None]) + ridge, Phi.T @ (w * y))
        except np.linalg.LinAlgError:
            break
        e = np.abs(y - Phi @ c)
        em = float(e.max())
        if em < best_e:
            best, best_e = c, em
        w = w * np.maximum(e, 1e-12)
        s = w.sum()
        if s <= 0:
            break
        w /= s
    return best


class Profile:
    __slots__ = ('r0', 'f', 'mode', 'K', 'sides', 'coverage', 'err_poly', 'out_excess', 'star', 'r0_fit')

    def __init__(self):
        self.r0 = 0.0
        self.f = np.zeros(6)
        self.mode = 'none'
        self.K = 0
        self.sides = 0
        self.coverage = 0.0
        self.err_poly = 0.0
        self.out_excess = 0.0
        self.star = True
        self.r0_fit = None

    def radius(self, th):
        th = np.asarray(th, np.float64)
        r = np.ones_like(th)
        for k in range(1, 4):
            r = r + self.f[2 * k - 2] * np.cos(k * th) + self.f[2 * k - 1] * np.sin(k * th)
        return self.r0 * r


def fit(Q2, closed, sides, vert2=None, prefer_vertices=True):
    """Profile of one cross-section. Q2: section polygon (2D, origin = ring centre); vert2: the stock ring vertices
    in the same frame (for the through-the-vertices fit). Returns a Profile."""
    pr = Profile()
    rr, ncross = radial_function(Q2, closed)
    have = np.isfinite(rr)
    pr.coverage = float(have.mean())
    pr.sides = int(sides)
    pr.star = bool((ncross[have] <= 1).mean() > 0.97) if have.any() else False
    if have.sum() < 8:
        if len(Q2):
            pr.r0 = own_radius(Q2)
            pr.mode = 'mean'
        return pr
    K = 3 if sides >= 7 else max(1, (sides - 1) // 2)
    if pr.coverage < 0.85:
        K = min(K, 1)
    pr.K = K
    th, y = TH[have], rr[have]
    Phi = basis(th, K)
    c = lawson_minimax(Phi, y)
    pr.mode = 'minimax'
    e = Phi @ c - y
    if prefer_vertices and sides >= 8 and vert2 is not None and len(vert2) >= 8:
        # through the stock vertices only where that costs little against the minimax placement: the many-sided
        # ring keeps its silhouette, the faces stay well inside the tolerance
        tv = np.arctan2(vert2[:, 1], vert2[:, 0])
        rv = np.linalg.norm(vert2, axis=1)
        cv = np.linalg.lstsq(basis(tv, K), rv, rcond=None)[0]
        ev = Phi @ cv - y
        if np.abs(ev).max() <= min(0.5 * float(tol_of(cv[0])), 1.5 * np.abs(e).max()):
            c, pr.mode = cv, 'vertices'
    if pr.mode == 'minimax':
        # Coarse rings (3-6 sides) of thick wood: the round profile misses the faces by more than the
        # tolerance; the higher harmonics (k <= 3) then follow the stock section
        if K < 3 and pr.coverage >= 0.85 and np.abs(e).max() > 0.7 * float(tol_of(c[0])):
            Phi3 = basis(th, 3)
            c3 = lawson_minimax(Phi3, y)
            e3 = Phi3 @ c3 - y
            if np.abs(e3).max() < 0.8 * np.abs(e).max():
                c, Phi, K = c3, Phi3, 3
                pr.K = 3
    e = Phi @ c - y
    pr.err_poly = float(np.abs(e).max())
    pr.out_excess = float(max(0.0, e.max()))
    r0 = float(c[0])
    if r0 <= 1e-5:
        pr.r0 = own_radius(Q2) if len(Q2) else float(np.nanmedian(rr))
        pr.mode = 'mean'
        return pr
    pr.r0 = r0
    f = np.zeros(6)
    f[:2 * K] = c[1:] / r0
    # (Harmonics past 0.6 of the mean radius turn the tube inside out: a round profile instead.)
    if harmonic_sum(f) > 0.6:
        f[:] = 0.0
        pr.mode = 'round'
    pr.f = f
    return pr


def harmonic_sum(f):
    """The profile's harmonics' amplitudes summed, a share of the mean radius."""
    return float(sum(np.hypot(f[2 * k], f[2 * k + 1]) for k in range(3)))


def own_radius(Q2):
    """The mean distance of the section's own contour from the ring centre: the points within two and a half times
    the nearest quarter's distance (the plane cut takes other wood too, metres away - its mean made rings of ten)."""
    d = np.linalg.norm(Q2, axis=1)
    near = d[d <= 2.5 * max(float(np.percentile(d, 25)), 1e-4)]
    return float(near.mean()) if len(near) else float(np.median(d))


def shrink(pr, amount):
    """Lower r0 by `amount` keeping the absolute harmonic amplitudes (the tube moves inwards everywhere); never
    below 60 % of the radius the section fit gave (a junction's air is no reason to pinch the tube)."""
    if amount <= 0 or pr.r0 <= 0:
        return pr
    if not hasattr(pr, 'r0_fit') or pr.r0_fit is None:
        pr.r0_fit = pr.r0
    amp = pr.f * pr.r0
    pr.r0 = max(pr.r0 - amount, 0.6 * pr.r0_fit)
    pr.f = amp / pr.r0
    # (The kept amplitudes over a smaller radius: held at 0.6 of it.)
    s = harmonic_sum(pr.f)
    if s > 0.6:
        pr.f = pr.f * (0.6 / s)
    return pr
