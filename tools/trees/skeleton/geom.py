# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Mesh and curve helpers in numpy only: welding, topology, closest points on triangles through a hashed grid,
plane sections, centripetal Catmull-Rom, monotone cubic radii, parallel transport."""
import collections

import numpy as np


# ------------------------------------------------------------------------------------------------ welding / topology
def weld(P, tol=2e-4):
    """Welded vertex id of every position (duplicates from UV seams and float noise within `tol` merge)."""
    q = np.floor(P / tol + 0.5).astype(np.int64)
    _, first, inv = np.unique(q, axis=0, return_index=True, return_inverse=True)
    inv = inv.ravel()
    # Neighbouring cells: union what lies within tol across a cell border
    nuq = len(first)
    par = np.arange(nuq)
    U = P[first]
    order = np.lexsort((q[first][:, 2], q[first][:, 1], q[first][:, 0]))
    keys = q[first][order]
    for d in ((1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (1, 0, 1), (0, 1, 1), (1, 1, 1)):
        k2 = keys + np.array(d)
        # match rows of k2 among keys
        a = np.ascontiguousarray(keys).view([('', keys.dtype)] * 3).ravel()
        b = np.ascontiguousarray(k2).view([('', keys.dtype)] * 3).ravel()
        srt = np.argsort(a)
        pos = np.searchsorted(a[srt], b)
        pos = np.minimum(pos, len(a) - 1)
        hit = a[srt][pos] == b
        if not hit.any():
            continue
        i = order[np.flatnonzero(hit)]
        j = order[srt[pos[hit]]]
        close = np.linalg.norm(U[i] - U[j], axis=1) <= tol
        for x, y in zip(i[close], j[close]):
            rx, ry = _find(par, x), _find(par, y)
            if rx != ry:
                par[max(rx, ry)] = min(rx, ry)
    root = np.array([_find(par, x) for x in range(nuq)])
    _, comp = np.unique(root, return_inverse=True)
    wid = comp.ravel()[inv]
    nW = wid.max() + 1 if len(wid) else 0
    rep = np.full(nW, -1, np.int64)
    rep[wid[::-1]] = np.arange(len(wid))[::-1]
    return wid, rep


def _find(par, a):
    while par[a] != a:
        par[a] = par[par[a]]
        a = par[a]
    return a


class Topo:
    """Welded triangle mesh: vertices V, faces F (degenerate faces dropped), edge table, adjacency (CSR), boundary
    loops, connected components."""

    def __init__(self, V, F):
        self.V = V
        F = F[(F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])]
        self.F = F
        nv = len(V)
        e = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
        es = np.sort(e, axis=1)
        self.E, inv, self.ecount = np.unique(es, axis=0, return_inverse=True, return_counts=True)
        self.face_edges = inv.ravel().reshape(3, -1).T
        a = np.concatenate([self.E[:, 0], self.E[:, 1]])
        b = np.concatenate([self.E[:, 1], self.E[:, 0]])
        o = np.argsort(a, kind='stable')
        self.adj_idx = np.concatenate([[0], np.cumsum(np.bincount(a, minlength=nv))])
        self.adj = b[o]
        self.nv = nv
        # components over edges
        par = np.arange(nv)
        for x, y in self.E:
            rx, ry = _find(par, x), _find(par, y)
            if rx != ry:
                par[max(rx, ry)] = min(rx, ry)
        root = np.array([_find(par, x) for x in range(nv)]) if nv else np.zeros(0, np.int64)
        _, self.comp = np.unique(root, return_inverse=True)
        self.comp = self.comp.ravel()
        self.ncomp = int(self.comp.max() + 1) if nv else 0
        used = np.zeros(nv, bool)
        used[F.ravel()] = True
        self.used = used
        self.loops = self._boundary_loops()

    def neighbours(self, v):
        return self.adj[self.adj_idx[v]:self.adj_idx[v + 1]]

    def _boundary_loops(self):
        bnd = self.E[self.ecount == 1]
        nxt = collections.defaultdict(list)
        for x, y in bnd:
            nxt[x].append(y)
            nxt[y].append(x)
        seen = set()
        loops = []
        for s in list(nxt):
            if s in seen:
                continue
            loop = [s]
            seen.add(s)
            prev, cur = -1, s
            while True:
                cand = [x for x in nxt[cur] if x != prev and x not in seen]
                if not cand:
                    break
                prev, cur = cur, cand[0]
                seen.add(cur)
                loop.append(cur)
            if len(loop) >= 2:
                loops.append(np.array(loop, np.int64))
        return loops


def bfs_levels(topo, sources, mask=None):
    """Hop distance of every vertex from `sources` (within `mask` if given); -1 where unreached."""
    lev = np.full(topo.nv, -1, np.int64)
    fr = np.unique(np.asarray(sources, np.int64))
    lev[fr] = 0
    h = 0
    while len(fr):
        idx = np.concatenate([topo.adj[topo.adj_idx[v]:topo.adj_idx[v + 1]] for v in fr]) if len(fr) else fr
        idx = np.unique(idx)
        idx = idx[lev[idx] < 0]
        if mask is not None:
            idx = idx[mask[idx]]
        h += 1
        lev[idx] = h
        fr = idx
    return lev


# ------------------------------------------------------------------------------------------------ closest points
def closest_on_tris(p, a, b, c):
    """Closest point on triangles (a, b, c) to points p, all (n, 3) row-aligned (Ericson, Real-Time Collision
    Detection 5.1.5). Regions are written in reverse order of the reference so the first match wins."""
    ab, ac, ap = b - a, c - a, p - a
    d1 = np.einsum('ij,ij->i', ab, ap)
    d2 = np.einsum('ij,ij->i', ac, ap)
    bp = p - b
    d3 = np.einsum('ij,ij->i', ab, bp)
    d4 = np.einsum('ij,ij->i', ac, bp)
    cp = p - c
    d5 = np.einsum('ij,ij->i', ab, cp)
    d6 = np.einsum('ij,ij->i', ac, cp)
    vc = d1 * d4 - d3 * d2
    vb = d5 * d2 - d1 * d6
    va = d3 * d6 - d5 * d4
    with np.errstate(divide='ignore', invalid='ignore'):
        den = va + vb + vc
        v = np.where(np.abs(den) > 1e-30, vb / den, 0.0)
        w = np.where(np.abs(den) > 1e-30, vc / den, 0.0)
        res = a + ab * v[:, None] + ac * w[:, None]
        m = (va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0)
        t = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        res[m] = (b + (c - b) * t[:, None])[m]
        m = (vb <= 0) & (d2 >= 0) & (d6 <= 0)
        t = d2 / (d2 - d6)
        res[m] = (a + ac * t[:, None])[m]
        m = (d6 >= 0) & (d5 <= d6)
        res[m] = c[m]
        m = (vc <= 0) & (d1 >= 0) & (d3 <= 0)
        t = d1 / (d1 - d3)
        res[m] = (a + ab * t[:, None])[m]
        m = (d3 >= 0) & (d4 <= d3)
        res[m] = b[m]
        m = (d1 <= 0) & (d2 <= 0)
        res[m] = a[m]
    bad = ~np.isfinite(res).all(1)
    if bad.any():
        # Degenerate triangle: nearest of its vertices
        cand = np.stack([a[bad], b[bad], c[bad]], 1)
        k = np.argmin(((cand - p[bad][:, None, :]) ** 2).sum(-1), 1)
        res[bad] = cand[np.arange(len(k)), k]
    return res


class TriGrid:
    """Hashed uniform grid over triangles for 'nearest triangle within `reach`' queries."""

    def __init__(self, V, F, reach, cell=None):
        self.V = np.asarray(V, np.float64)
        self.F = np.asarray(F, np.int64)
        self.reach = float(reach)
        self.cell = float(cell or max(reach, 0.05))
        A, B, C = self.V[self.F[:, 0]], self.V[self.F[:, 1]], self.V[self.F[:, 2]]
        lo = np.minimum(np.minimum(A, B), C) - self.reach
        hi = np.maximum(np.maximum(A, B), C) + self.reach
        self.org = (lo.min(0) if len(lo) else np.zeros(3)) - self.cell
        i0 = np.floor((lo - self.org) / self.cell).astype(np.int64)
        i1 = np.floor((hi - self.org) / self.cell).astype(np.int64)
        self.dim = (i1.max(0) + 2) if len(i1) else np.ones(3, np.int64)
        span = i1 - i0 + 1
        cnt = span.prod(1)
        tri = np.repeat(np.arange(len(self.F)), cnt)
        # Enumerate the cells of every triangle box
        off = np.arange(cnt.sum()) - np.repeat(np.cumsum(cnt) - cnt, cnt)
        sx = np.repeat(span[:, 0], cnt)
        sy = np.repeat(span[:, 1], cnt)
        ix = np.repeat(i0[:, 0], cnt) + off % sx
        iy = np.repeat(i0[:, 1], cnt) + (off // sx) % sy
        iz = np.repeat(i0[:, 2], cnt) + off // (sx * sy)
        key = (ix * self.dim[1] + iy) * self.dim[2] + iz
        o = np.argsort(key, kind='stable')
        self.keys = key[o]
        self.tris = tri[o]

    def query(self, Q, chunk=40000):
        """(distance, triangle, closest point) per query point; distance inf where nothing lies within reach."""
        Q = np.asarray(Q, np.float64)
        n = len(Q)
        dist = np.full(n, np.inf)
        tri = np.full(n, -1, np.int64)
        cp = np.zeros((n, 3))
        if not len(self.F) or not n:
            return dist, tri, cp
        for s in range(0, n, chunk):
            q = Q[s:s + chunk]
            ii = np.floor((q - self.org) / self.cell).astype(np.int64)
            ok = np.all((ii >= 0) & (ii < self.dim), axis=1)
            key = (ii[:, 0] * self.dim[1] + ii[:, 1]) * self.dim[2] + ii[:, 2]
            l = np.searchsorted(self.keys, key, 'left')
            r = np.searchsorted(self.keys, key, 'right')
            r[~ok] = l[~ok]
            c = r - l
            if c.sum() == 0:
                continue
            qi = np.repeat(np.arange(len(q)), c)
            off = np.arange(c.sum()) - np.repeat(np.cumsum(c) - c, c)
            ti = self.tris[np.repeat(l, c) + off]
            f = self.F[ti]
            pts = closest_on_tris(q[qi], self.V[f[:, 0]], self.V[f[:, 1]], self.V[f[:, 2]])
            d = np.linalg.norm(pts - q[qi], axis=1)
            # per query minimum (pairs are grouped by query in order)
            starts = np.cumsum(c) - c
            has = c > 0
            dm = np.full(len(q), np.inf)
            dm[has] = np.minimum.reduceat(d, starts[has])
            # argmin per group
            best = np.full(len(q), -1, np.int64)
            isbest = d <= dm[qi] + 1e-15
            cand = np.flatnonzero(isbest)
            best[qi[cand[::-1]]] = cand[::-1]
            sel = has & (dm <= self.reach)
            dist[s:s + chunk][sel] = dm[sel]
            tri[s:s + chunk][sel] = ti[best[sel]]
            cp[s:s + chunk][sel] = pts[best[sel]]
        return dist, tri, cp


class SegGrid:
    """Hashed grid over the segments of a polyline for 'nearest segment within `reach`' queries."""

    def __init__(self, C, reach, cell=None):
        C = np.asarray(C, np.float64)
        self.A = C[:-1]
        self.B = C[1:]
        self.reach = float(reach)
        self.cell = float(cell or max(reach * 0.5, 0.05))
        lo = np.minimum(self.A, self.B) - self.reach
        hi = np.maximum(self.A, self.B) + self.reach
        self.org = (lo.min(0) if len(lo) else np.zeros(3)) - self.cell
        i0 = np.floor((lo - self.org) / self.cell).astype(np.int64)
        i1 = np.floor((hi - self.org) / self.cell).astype(np.int64)
        self.dim = (i1.max(0) + 2) if len(i1) else np.ones(3, np.int64)
        span = i1 - i0 + 1
        cnt = span.prod(1)
        seg = np.repeat(np.arange(len(self.A)), cnt)
        off = np.arange(cnt.sum()) - np.repeat(np.cumsum(cnt) - cnt, cnt)
        sx = np.repeat(span[:, 0], cnt)
        sy = np.repeat(span[:, 1], cnt)
        ix = np.repeat(i0[:, 0], cnt) + off % sx
        iy = np.repeat(i0[:, 1], cnt) + (off // sx) % sy
        iz = np.repeat(i0[:, 2], cnt) + off // (sx * sy)
        key = (ix * self.dim[1] + iy) * self.dim[2] + iz
        o = np.argsort(key, kind='stable')
        self.keys = key[o]
        self.segs = seg[o]

    def query(self, Q, chunk=60000):
        """(segment index, parameter t in [0, 1], distance) per point; -1 / inf beyond reach."""
        Q = np.asarray(Q, np.float64)
        n = len(Q)
        si = np.full(n, -1, np.int64)
        st = np.zeros(n)
        sd = np.full(n, np.inf)
        if not len(self.A) or not n:
            return si, st, sd
        for s0 in range(0, n, chunk):
            q = Q[s0:s0 + chunk]
            ii = np.floor((q - self.org) / self.cell).astype(np.int64)
            ok = np.all((ii >= 0) & (ii < self.dim), axis=1)
            key = (ii[:, 0] * self.dim[1] + ii[:, 1]) * self.dim[2] + ii[:, 2]
            l = np.searchsorted(self.keys, key, 'left')
            r = np.searchsorted(self.keys, key, 'right')
            r[~ok] = l[~ok]
            c = r - l
            if c.sum() == 0:
                continue
            qi = np.repeat(np.arange(len(q)), c)
            off = np.arange(c.sum()) - np.repeat(np.cumsum(c) - c, c)
            sg = self.segs[np.repeat(l, c) + off]
            a, b = self.A[sg], self.B[sg]
            ab = b - a
            L2 = np.maximum(np.einsum('ij,ij->i', ab, ab), 1e-24)
            t = np.clip(np.einsum('ij,ij->i', q[qi] - a, ab) / L2, 0, 1)
            d = np.linalg.norm(q[qi] - (a + ab * t[:, None]), axis=1)
            starts = np.cumsum(c) - c
            has = c > 0
            dm = np.full(len(q), np.inf)
            dm[has] = np.minimum.reduceat(d, starts[has])
            best = np.full(len(q), -1, np.int64)
            isb = d <= dm[qi] + 1e-15
            cand = np.flatnonzero(isb)
            best[qi[cand[::-1]]] = cand[::-1]
            sel = has & (dm <= self.reach)
            si[s0:s0 + chunk][sel] = sg[best[sel]]
            st[s0:s0 + chunk][sel] = t[best[sel]]
            sd[s0:s0 + chunk][sel] = dm[sel]
        return si, st, sd


def point_set_distance(Q, P, reach):
    """Distance from each Q to the nearest point of P (inf beyond reach), through the same grid idea."""
    F = np.repeat(np.arange(len(P))[:, None], 3, 1)
    g = TriGrid(P, F, reach)
    return g.query(Q)


# ------------------------------------------------------------------------------------------------ triangles
def tri_normals(V, F):
    n = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]])
    a = np.linalg.norm(n, axis=1)
    return n / np.maximum(a, 1e-30)[:, None], a * 0.5


def sample_tris(V, F, spacing, rng=None, min_per_tri=1):
    """Points on triangles: vertices, centroids and a stratified fill at about `spacing`; returns (points, tri)."""
    A, B, C = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
    area = 0.5 * np.linalg.norm(np.cross(B - A, C - A), axis=1)
    k = np.maximum(min_per_tri, np.ceil(area / (spacing * spacing)).astype(np.int64))
    k = np.minimum(k, 400)
    tri = np.repeat(np.arange(len(F)), k)
    rng = rng or np.random.default_rng(1)
    u = rng.random(len(tri))
    v = rng.random(len(tri))
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    pts = A[tri] + (B - A)[tri] * u[:, None] + (C - A)[tri] * v[:, None]
    pts = np.concatenate([pts, (A + B + C) / 3])
    tri = np.concatenate([tri, np.arange(len(F))])
    return pts, tri


# ------------------------------------------------------------------------------------------------ plane sections
def plane_section(V, F, c, nrm):
    """Section of triangles by the plane through c with normal nrm: list of polylines (each an (m, 3) array and a
    closed flag), chained through the mesh edges they cross."""
    d = (V - c) @ nrm
    s = d[F]
    pos = s > 0
    cross = pos.any(1) & (~pos).any(1)
    if not cross.any():
        return []
    Fi = F[cross]
    si = s[cross]
    segs = []
    for f, sd in zip(Fi, si):
        pts = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            if (sd[a] > 0) != (sd[b] > 0):
                va, vb = f[a], f[b]
                t = sd[a] / (sd[a] - sd[b])
                key = (min(va, vb), max(va, vb))
                pts.append((key, V[va] + (V[vb] - V[va]) * t))
        if len(pts) == 2:
            segs.append(pts)
    # chain segments by shared edge keys
    by_key = collections.defaultdict(list)
    for i, sg in enumerate(segs):
        by_key[sg[0][0]].append(i)
        by_key[sg[1][0]].append(i)
    used = np.zeros(len(segs), bool)
    out = []
    for i0 in range(len(segs)):
        if used[i0]:
            continue
        used[i0] = True
        chain = [segs[i0][0], segs[i0][1]]
        # extend forward
        for direction in (1, 0):
            while True:
                end_key = chain[-1][0] if direction else chain[0][0]
                nxt = [j for j in by_key[end_key] if not used[j]]
                if not nxt:
                    break
                j = nxt[0]
                used[j] = True
                a, b = segs[j]
                other = b if a[0] == end_key else a
                if direction:
                    chain.append(other)
                else:
                    chain.insert(0, other)
        closed = chain[0][0] == chain[-1][0] and len(chain) > 3
        pts = np.array([p for _, p in chain])
        if closed:
            pts = pts[:-1]
        out.append((pts, closed))
    return out


# ------------------------------------------------------------------------------------------------ curves
def catmull_rom(P, n_sub, alpha=0.5):
    """Centripetal Catmull-Rom through P (m, 3); `n_sub` points per segment (u in [0, 1)), plus the last point.
    End tangents use reflected phantom points. Returns (points, segment index, u)."""
    P = np.asarray(P, np.float64)
    m = len(P)
    if m == 1:
        return P.copy(), np.zeros(1, np.int64), np.zeros(1)
    ext = np.concatenate([[2 * P[0] - P[1]], P, [2 * P[-1] - P[-2]]])
    u = np.arange(n_sub) / n_sub
    pts = cr_points(ext[:-3], ext[1:-2], ext[2:-1], ext[3:], u, alpha).reshape(-1, 3)
    seg = np.repeat(np.arange(m - 1), n_sub)
    return (np.concatenate([pts, P[-1:]]), np.concatenate([seg, [m - 2]]).astype(np.int64),
            np.concatenate([np.tile(u, m - 1), [1.0]]))


def cr_points(p0, p1, p2, p3, u, alpha=0.5):
    """Points of the centripetal Catmull-Rom segments p1 -> p2 (k segments, (k, 3) each) at u (n,) in [0, 1]
    (Barry-Goldman pyramid): (k, n, 3)."""
    def knot(a, b):
        return np.maximum(np.linalg.norm(b - a, axis=1), 1e-9) ** alpha
    t1 = knot(p0, p1)[:, None]
    t2 = t1 + knot(p1, p2)[:, None]
    t3 = t2 + knot(p2, p3)[:, None]
    t = t1 + (t2 - t1) * np.asarray(u, np.float64)[None, :]
    p0, p1, p2, p3 = (x[:, None, :] for x in (p0, p1, p2, p3))
    t1, t2, t3, t = t1[..., None], t2[..., None], t3[..., None], t[..., None]
    A1 = (t1 - t) / t1 * p0 + t / t1 * p1
    A2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2
    A3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3
    B1 = (t2 - t) / t2 * A1 + t / t2 * A2
    B2 = (t3 - t) / (t3 - t1) * A2 + (t - t1) / (t3 - t1) * A3
    return (t2 - t) / (t2 - t1) * B1 + (t - t1) / (t2 - t1) * B2


def cr_segment(p0, p1, p2, p3, u, alpha=0.5):
    """Points of the centripetal Catmull-Rom segment p1 -> p2 at u in [0, 1] (Barry-Goldman pyramid)."""
    return cr_points(p0[None], p1[None], p2[None], p3[None], np.atleast_1d(u), alpha)[0]


def cr_tangents(P, alpha=0.5):
    """Unit tangent of the centripetal Catmull-Rom curve at every knot (numerical derivative of the segments)."""
    P = np.asarray(P, np.float64)
    m = len(P)
    if m == 1:
        return np.array([[0.0, 1.0, 0.0]])
    ext = np.concatenate([[2 * P[0] - P[1]], P, [2 * P[-1] - P[-2]]])
    h = 1e-4
    T = np.zeros((m, 3))
    # knots 0 .. m-2 at the start of their segment, the last one at the end of the last segment
    a = cr_points(ext[:-3], ext[1:-2], ext[2:-1], ext[3:], np.array([0.0, h]), alpha)
    T[:-1] = a[:, 1] - a[:, 0]
    b = cr_points(ext[m - 2:m - 1], ext[m - 1:m], ext[m:m + 1], ext[m + 1:m + 2], np.array([1.0 - h, 1.0]), alpha)
    T[-1] = b[0, 1] - b[0, 0]
    nd = np.linalg.norm(T, axis=1)
    for i in np.flatnonzero(nd <= 1e-12):
        T[i] = P[min(i + 1, m - 1)] - P[max(i - 1, 0)]
    T /= np.maximum(np.linalg.norm(T, axis=1), 1e-12)[:, None]
    return T


def monotone_slopes(x, y):
    """Fritsch-Carlson slopes of the monotone cubic Hermite through (x, y)."""
    x = np.asarray(x, np.float64)
    y = np.asarray(y, np.float64)
    n = len(x)
    if n < 2:
        return np.zeros(n)
    h = np.maximum(np.diff(x), 1e-9)
    dlt = np.diff(y) / h
    m = np.zeros(n)
    m[0], m[-1] = dlt[0], dlt[-1]
    for i in range(1, n - 1):
        if dlt[i - 1] * dlt[i] <= 0:
            m[i] = 0.0
        else:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / dlt[i - 1] + w2 / dlt[i])
    return m


def hermite(y0, y1, m0, m1, h, u):
    u = np.asarray(u, np.float64)
    u2, u3 = u * u, u * u * u
    return (2 * u3 - 3 * u2 + 1) * y0 + (u3 - 2 * u2 + u) * h * m0 + (-2 * u3 + 3 * u2) * y1 + (u3 - u2) * h * m1


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def transport(n_prev, t_prev, t_next):
    """Parallel transport of the unit normal n_prev from tangent t_prev to t_next (minimal rotation)."""
    n_prev, t_prev, t_next = (tuple(float(x) for x in v) for v in (n_prev, t_prev, t_next))
    v = _cross(t_prev, t_next)
    c = t_prev[0] * t_next[0] + t_prev[1] * t_next[1] + t_prev[2] * t_next[2]
    sv = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
    if sv < 1e-9:
        n = n_prev
    else:
        k = (v[0] / sv, v[1] / sv, v[2] / sv)
        kn = _cross(k, n_prev)
        kd = k[0] * n_prev[0] + k[1] * n_prev[1] + k[2] * n_prev[2]
        n = tuple(n_prev[i] * c + kn[i] * sv + k[i] * kd * (1 - c) for i in range(3))
    d = n[0] * t_next[0] + n[1] * t_next[1] + n[2] * t_next[2]
    n = (n[0] - t_next[0] * d, n[1] - t_next[1] * d, n[2] - t_next[2] * d)
    nn = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
    if nn < 1e-9:
        return any_perp(np.array(t_next))
    return np.array(n) / nn


def any_perp(t):
    t = np.asarray(t, np.float64)
    ref = np.array([1.0, 0.0, 0.0]) if abs(t[0]) < 0.9 else np.array([0.0, 0.0, 1.0])
    n = ref - t * np.dot(ref, t)
    return n / np.linalg.norm(n)


def polyline_project(Q, C):
    """Arclength parameter and distance of the projection of points Q onto the polyline C (m, 3)."""
    C = np.asarray(C, np.float64)
    if len(C) == 1:
        return np.zeros(len(Q)), np.linalg.norm(Q - C[0], axis=1), np.zeros(len(Q), np.int64)
    seg = C[1:] - C[:-1]
    L = np.linalg.norm(seg, axis=1)
    cum = np.concatenate([[0], np.cumsum(L)])
    L2 = np.maximum(L * L, 1e-24)
    n, m = len(Q), len(seg)
    best_d = np.full(n, np.inf)
    best_s = np.zeros(n)
    best_i = np.zeros(n, np.int64)
    # points x segments in blocks of about 4M pairs
    step = max(1, 4_000_000 // max(m, 1))
    for a in range(0, n, step):
        q = Q[a:a + step]
        rel = q[:, None, :] - C[None, :-1, :]
        t = np.clip(np.einsum('nmk,mk->nm', rel, seg) / L2[None, :], 0, 1)
        d2 = ((rel - t[..., None] * seg[None]) ** 2).sum(-1)
        d2[:, L < 1e-12] = np.inf
        j = np.argmin(d2, axis=1)
        r = np.arange(len(q))
        best_d[a:a + step] = np.sqrt(d2[r, j])
        best_s[a:a + step] = cum[j] + t[r, j] * L[j]
        best_i[a:a + step] = j
    bad = ~np.isfinite(best_d)
    if bad.any():
        best_d[bad] = np.linalg.norm(Q[bad] - C[0], axis=1)
    return best_s, best_d, best_i
