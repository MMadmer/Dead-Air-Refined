# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Skeleton of wood whose hop levels are no rings (coarse or irregular triangulations: one hop spans metres on one
side of a limb and centimetres on the other). The piece is refined (every triangle cut into k x k), the geodesic
distance from its root runs over the refined edges, and the iso-contours of that distance every `delta` metres are
loops around the wood. Their Reeb graph (a loop's parent: the loop below it on the same band of the surface) gives
the skeleton nodes - loop centroids, mean radii, branch tips, open (cut) ends - in the voxel skeleton's format
(voxel.py), so fallback.py makes axes of either."""
import heapq

import numpy as np

MAX_FACES = 150_000     # refined faces at most
MAX_LEVELS = 400


def _grid(k):
    """The k x k barycentric grid of a triangle: points (i along v1, j along v2) and its sub-triangles."""
    pts = [(i, j) for j in range(k + 1) for i in range(k + 1 - j)]
    idx = {p: n for n, p in enumerate(pts)}
    tris = []
    for j in range(k):
        for i in range(k - j):
            tris.append((idx[(i, j)], idx[(i + 1, j)], idx[(i, j + 1)]))
            if i + j <= k - 2:
                tris.append((idx[(i + 1, j)], idx[(i + 1, j + 1)], idx[(i, j + 1)]))
    return np.array(pts, np.int64), np.array(tris, np.int64)


def refine(V, F, k):
    """Every triangle cut into k x k, shared edges cut alike: (V', F', E); the original vertices keep their ids, the
    points cut into edge e (of the sorted edge table E) are n + e (k - 1) + 0 .. k - 2 from its lower vertex id on."""
    if k <= 1:
        e = np.concatenate([F[:, [0, 1]], F[:, [0, 2]], F[:, [1, 2]]])
        return V.copy(), F.copy(), np.unique(np.sort(e, axis=1), axis=0)
    n, m = len(V), len(F)
    e = np.concatenate([F[:, [0, 1]], F[:, [0, 2]], F[:, [1, 2]]])
    E, inv = np.unique(np.sort(e, axis=1), axis=0, return_inverse=True)
    inv = inv.ravel()
    fe = inv.reshape(3, m).T            # edge ids of (v0 v1), (v0 v2), (v1 v2)
    ne = len(E)
    pts, tris = _grid(k)
    i, j = pts[:, 0], pts[:, 1]
    l_ = k - i - j
    n_int = (k - 1) * (k - 2) // 2
    ids = np.zeros((m, len(pts)), np.int64)
    v0, v1, v2 = F[:, 0], F[:, 1], F[:, 2]
    int_rank = np.cumsum((i > 0) & (j > 0) & (l_ > 0)) - 1
    for p in range(len(pts)):
        ip, jp, lp = int(i[p]), int(j[p]), int(l_[p])
        if ip == 0 and jp == 0:
            ids[:, p] = v0
        elif ip == k:
            ids[:, p] = v1
        elif jp == k:
            ids[:, p] = v2
        elif jp == 0:
            # edge v0 -> v1 at i/k; the canonical edge runs from its lower vertex id
            pos = np.where(v0 < v1, ip, k - ip)
            ids[:, p] = n + fe[:, 0] * (k - 1) + pos - 1
        elif ip == 0:
            pos = np.where(v0 < v2, jp, k - jp)
            ids[:, p] = n + fe[:, 1] * (k - 1) + pos - 1
        elif lp == 0:
            pos = np.where(v1 < v2, jp, k - jp)
            ids[:, p] = n + fe[:, 2] * (k - 1) + pos - 1
        else:
            ids[:, p] = n + ne * (k - 1) + np.arange(m) * n_int + int_rank[p]
    t = np.arange(1, k) / k
    VE = V[E[:, 0]][:, None, :] + t[None, :, None] * (V[E[:, 1]] - V[E[:, 0]])[:, None, :]
    inner = (i > 0) & (j > 0) & (l_ > 0)
    bi, bj = i[inner] / k, j[inner] / k
    VI = (V[v0][:, None, :] + bi[None, :, None] * (V[v1] - V[v0])[:, None, :] +
          bj[None, :, None] * (V[v2] - V[v0])[:, None, :])
    Vn = np.concatenate([V, VE.reshape(-1, 3), VI.reshape(-1, 3)])
    Fn = ids[:, tris].reshape(-1, 3)
    return Vn, Fn, E


def labels(n, a, b):
    """Connected components of n nodes joined by the pairs (a, b): a label (the smallest member) per node."""
    lab = np.arange(n)
    if not len(a):
        return lab
    while True:
        la, lb = lab[a], lab[b]
        lo = np.minimum(la, lb)
        np.minimum.at(lab, la, lo)
        np.minimum.at(lab, lb, lo)
        while True:
            nl = lab[lab]
            if np.array_equal(nl, lab):
                break
            lab = nl
        if np.array_equal(lab[a], lab[b]):
            return lab


def geodesic(V, E, src):
    """Shortest path length over the edges from the source vertices (Dijkstra)."""
    n = len(V)
    w = np.linalg.norm(V[E[:, 0]] - V[E[:, 1]], axis=1)
    a = np.concatenate([E[:, 0], E[:, 1]])
    b = np.concatenate([E[:, 1], E[:, 0]])
    ww = np.concatenate([w, w])
    o = np.argsort(a, kind='stable')
    start = np.concatenate([[0], np.cumsum(np.bincount(a, minlength=n))]).tolist()
    nb, wt = b[o].tolist(), ww[o].tolist()
    d = [float('inf')] * n
    heap = []
    for s in np.unique(src).tolist():
        d[s] = 0.0
        heap.append((0.0, s))
    heapq.heapify(heap)
    while heap:
        dv, v = heapq.heappop(heap)
        if dv > d[v]:
            continue
        for q in range(start[v], start[v + 1]):
            u = nb[q]
            nd = dv + wt[q]
            if nd < d[u]:
                d[u] = nd
                heapq.heappush(heap, (nd, u))
    return np.array(d)


class Loop:
    __slots__ = ('level', 'centre', 'radius', 'length', 'closed', 'faces', 'pts', 'parent', 'kids', 'tip',
                 'open_end', 'merged', 'npar', 'id')

    def __init__(self, level, centre, radius, length=0.0, closed=False, faces=None, pts=None):
        self.level, self.centre, self.radius, self.length, self.closed = level, centre, radius, length, closed
        self.faces = np.zeros(0, np.int64) if faces is None else faces
        self.pts = np.zeros((0, 3)) if pts is None else pts
        self.parent, self.kids, self.tip, self.merged, self.npar, self.id = None, [], False, False, 0, -1
        self.open_end = False


def skeleton(V, F, root, delta=None):
    """Skeleton nodes of one wood piece (V, F; root: vertex ids of its root set): list of (centre, radius, level,
    parent node, size) like voxel.skeleton, and per node a dict (tip, closed, merged_end, pts)."""
    if not len(F) or not len(root):
        return [], []
    used = np.unique(F)
    ext = V[used].max(0) - V[used].min(0)
    # ---- refined copy: edges about half a contour step long, within the face budget
    e = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    el = np.linalg.norm(V[e[:, 0]] - V[e[:, 1]], axis=1)
    if delta is None:
        # the step from the piece's own extent: ~120 contours along its longest path
        delta = float(np.clip(1.15 * float(np.linalg.norm(ext)) / 120.0, 0.02, 0.25))
    k = int(np.ceil(np.percentile(el, 90) / (0.5 * delta)))
    k = int(np.clip(k, 1, max(1, int(np.sqrt(MAX_FACES / max(len(F), 1))))))
    Vr, Fr, E0 = refine(V, F, k)
    # the root set with the points cut into the edges between its vertices (a ground loop stays at distance 0
    # along its whole length, not only at its old vertices)
    root = np.asarray(root, np.int64)
    rin = np.zeros(len(V), bool)
    rin[root] = True
    re = np.flatnonzero(rin[E0[:, 0]] & rin[E0[:, 1]])
    if k > 1 and len(re):
        root = np.concatenate([root, (len(V) + re[:, None] * (k - 1) + np.arange(k - 1)[None, :]).ravel()])
    E = np.unique(np.sort(np.concatenate([Fr[:, [0, 1]], Fr[:, [1, 2]], Fr[:, [2, 0]]]), axis=1), axis=0)
    g = geodesic(Vr, E, root)
    fin = np.isfinite(g)
    if not fin[np.unique(Fr)].any():
        return [], []
    gmax = float(g[fin].max())
    if gmax / delta > MAX_LEVELS:
        delta = gmax / MAX_LEVELS
    # unreached vertices (another piece glued in this one's faces) count as far
    g = np.where(fin, g, gmax + delta)
    # ---- edge table of the refined mesh and the faces' edges
    e3 = np.concatenate([Fr[:, [0, 1]], Fr[:, [1, 2]], Fr[:, [2, 0]]])
    Eu, inv, ecount = np.unique(np.sort(e3, axis=1), axis=0, return_inverse=True, return_counts=True)
    fe = inv.ravel().reshape(3, -1).T
    ga, gb = g[Eu[:, 0]], g[Eu[:, 1]]
    fgmin, fgmax = g[Fr].min(1), g[Fr].max(1)
    # face pairs over interior edges (for the bands)
    eo = np.argsort(inv.ravel(), kind='stable')
    ef = (np.arange(3 * len(Fr)) % len(Fr))[eo]
    first = np.concatenate([[0], np.cumsum(ecount)])
    two = np.flatnonzero(ecount == 2)
    pair_f = np.stack([ef[first[two]], ef[first[two] + 1]], 1)
    pair_lo, pair_hi = np.minimum(ga[two], gb[two]), np.maximum(ga[two], gb[two])
    n_lev = int(np.floor(gmax / delta - 0.5)) + 1
    levels = []         # per level: list of Loop
    for lv in range(max(n_lev, 0)):
        h = (lv + 0.5) * delta
        cross = (ga < h) != (gb < h)
        fc = cross[fe]
        sf = np.flatnonzero(fc.sum(1) == 2)
        if not len(sf):
            levels.append([])
            continue
        # the two crossing edges of each straddling face
        ce = fe[sf][fc[sf]].reshape(-1, 2)
        cid = np.flatnonzero(cross)
        remap = np.full(len(Eu), -1, np.int64)
        remap[cid] = np.arange(len(cid))
        a_, b_ = remap[ce[:, 0]], remap[ce[:, 1]]
        lab = labels(len(cid), a_, b_)
        ea, eb = Eu[cid, 0], Eu[cid, 1]
        t = np.clip((h - g[ea]) / np.where(np.abs(g[eb] - g[ea]) > 1e-12, g[eb] - g[ea], 1.0), 0, 1)
        P = Vr[ea] + t[:, None] * (Vr[eb] - Vr[ea])
        seg_len = np.linalg.norm(P[a_] - P[b_], axis=1)
        mid = 0.5 * (P[a_] + P[b_])
        flab = lab[a_]
        loops = []
        for u in np.unique(flab):
            s = flab == u
            L = float(seg_len[s].sum())
            members = np.flatnonzero(lab == u)
            if L < 1e-9:
                cen = P[members].mean(0)
                rad = 0.0
            else:
                cen = (mid[s] * seg_len[s, None]).sum(0) / L
                rad = float((np.linalg.norm(mid[s] - cen, axis=1) * seg_len[s]).sum() / L)
            loops.append(Loop(lv, cen, rad, L, bool(np.all(ecount[cid[members]] == 2)), sf[s], P[members]))
        levels.append(loops)
    # ---- bands between consecutive contours: which loops above belong to which loop below
    nodes = []
    for lv, loops in enumerate(levels):
        for lp in loops:
            lp.id = len(nodes)
            nodes.append(lp)
    roots = []
    for lv in range(-1, len(levels)):
        h0, h1 = (lv + 0.5) * delta if lv >= 0 else -1.0, (lv + 1.5) * delta
        fsel = np.flatnonzero((fgmin < h1) & (fgmax >= h0))
        if not len(fsel):
            continue
        pm = (pair_lo < h1) & (pair_hi >= h0)
        fr = np.full(len(Fr), -1, np.int64)
        fr[fsel] = np.arange(len(fsel))
        pa, pb = fr[pair_f[pm, 0]], fr[pair_f[pm, 1]]
        ok = (pa >= 0) & (pb >= 0)
        blab = labels(len(fsel), pa[ok], pb[ok])
        lower = levels[lv] if lv >= 0 else []
        upper = levels[lv + 1] if lv + 1 < len(levels) else []
        comp_lo, comp_up = {}, {}
        for lp in lower:
            comp_lo.setdefault(int(blab[fr[lp.faces[0]]]), []).append(lp)
        for lp in upper:
            comp_up.setdefault(int(blab[fr[lp.faces[0]]]), []).append(lp)
        if lv < 0:
            # the root's band: one root node per piece of it that holds root vertices
            Fs = Fr[fsel]
            rmask = np.isin(Fs, root).any(1)
            for c in np.unique(blab[rmask]):
                vs = np.unique(Fs[(blab == c) & rmask])
                vs = vs[np.isin(vs, root)]
                cen = Vr[vs].mean(0)
                rn = Loop(-1, cen, float(np.linalg.norm(Vr[vs] - cen, axis=1).mean()) if len(vs) > 1 else 0.0,
                          pts=Vr[vs])
                rn.id = len(nodes)
                nodes.append(rn)
                roots.append(rn)
                for up in comp_up.get(int(c), []):
                    up.parent = rn
                    rn.kids.append(up)
            continue
        for c, los in comp_lo.items():
            ups = comp_up.get(c, [])
            big = max(los, key=lambda q: q.length)
            if not ups:
                # the wood ends in this band: at an open boundary (a cut or broken end) the end ring is that loop,
                # else a tip at the band's farthest point
                cf_ = fsel[blab == c]
                be = np.unique(fe[cf_].ravel())
                be = be[(ecount[be] == 1) & (0.5 * (ga[be] + gb[be]) >= h0)]
                bl = np.linalg.norm(Vr[Eu[be, 0]] - Vr[Eu[be, 1]], axis=1)
                if len(be) >= 3 and bl.sum() >= 0.5 * big.length:
                    mid_ = 0.5 * (Vr[Eu[be, 0]] + Vr[Eu[be, 1]])
                    cen = (mid_ * bl[:, None]).sum(0) / bl.sum()
                    rad = float((np.linalg.norm(mid_ - cen, axis=1) * bl).sum() / bl.sum())
                    tp = Loop(lv + 0.5, cen, rad, float(bl.sum()), False, pts=mid_)
                    tp.open_end = True
                else:
                    vs = np.unique(Fr[cf_])
                    v = vs[np.argmax(g[vs])]
                    tp = Loop(lv + 0.5, Vr[v].copy(), 0.0, pts=Vr[v][None])
                    tp.tip = True
                tp.parent, tp.id = big, len(nodes)
                nodes.append(tp)
                big.kids.append(tp)
                for q in los:
                    if q is not big:
                        q.merged = True
                continue
            for up in ups:
                up.parent = big
                up.npar = len(los)
                big.kids.append(up)
            # loops that run into a bigger one (a cycle of welded wood, root fingers): their chain ends here
            for q in los:
                if q is not big:
                    q.merged = True
    # loops without a parent (an unreached island): off the skeleton
    keep = []
    seen = set()
    stack = list(roots)
    while stack:
        q = stack.pop()
        if q.id in seen:
            continue
        seen.add(q.id)
        keep.append(q)
        stack += q.kids
    # several root pieces (a base on root fingers): the trunk starts where they merge (the lowest loop with several
    # loops below it that carries a good part of the wood), the fingers stay stock
    if len(roots) > 1:
        size = {}
        for q in sorted(keep, key=lambda q: -q.level):
            size[q.id] = q.length * delta + sum(size.get(c.id, 0.0) for c in q.kids)
        total = sum(size[r.id] for r in roots)
        cand = [q for q in keep if q.level >= 0 and q.npar >= 2 and size[q.id] >= 0.3 * total]
        if cand:
            m0 = min(cand, key=lambda q: q.level)
            m0.parent = None
            keep, stack, seen = [], [m0], set()
            while stack:
                q = stack.pop()
                if q.id in seen:
                    continue
                seen.add(q.id)
                keep.append(q)
                stack += q.kids
    order = sorted(keep, key=lambda q: (q.level, q.id))
    index = {q.id: n for n, q in enumerate(order)}
    out, meta = [], []
    for q in order:
        par = index.get(q.parent.id, -1) if q.parent is not None else -1
        out.append((q.centre, q.radius, q.level, par, max(q.length * delta, 1e-6)))
        meta.append({'tip': q.tip, 'open_end': q.open_end, 'closed': q.closed,
                     'merged_end': q.merged and not q.kids, 'pts': q.pts})
    return out, meta
