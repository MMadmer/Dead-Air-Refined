# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Stage 3a: rings of every stock axis, the refit tubes and the patches of original triangles.

Per axis: one station per level-set node (the stock ring; at junctions the fragment), the ring plane through the
station's stock centroid normal to the Catmull-Rom spine, the stock surface cut by that plane -> Fourier profile
(profile.py). The tubes are rebuilt exactly as the engine will (tube.py) and measured both ways against the stock
mesh: tube -> stock (signed: outside / inside the stock surface) and stock -> tubes. Segments that miss inside get
a mid station from the stock section (finer rings); rings that stay outside by more than the tolerance are pulled in;
stock triangles still further than the tolerance from every tube are kept as patches (original vertices, normals,
uv), as are all cut-end caps."""
import collections

import numpy as np

import axes as AX
import geom as G
import xsection as PF
import tube as TB

N_SUB = 6        # tube rows per ring segment for the measurements
N_TH = 32        # samples around


class AxisFit:
    face_mask = None

    def __init__(self, ax):
        self.id = ax.id
        self.comp = ax.comp
        self.comps = {ax.comp}  # the wood pieces it runs through (continuations merged in)
        self.src = ax
        self.parent = ax.parent
        self.level = 1
        self.attach = 0.0
        self.flags = 0
        self.climbable = 0.0
        self.st = []            # stations: dict(c, sides, node, clean, verts)
        self.R = None           # tube.Rings
        self.prof = []
        self.ev = None
        self.merged = False
        self.end_open = False
        self.base_open = False


# ------------------------------------------------------------------------------------------------ stations
def node_radius(V, nd):
    if len(nd.verts) < 2:
        return 0.0
    return float(np.linalg.norm(V[nd.verts] - nd.centroid, axis=1).mean())


def is_split(nodes, nid):
    return len(nodes[nid].children) >= 2


def join_node(nodes, nid, steps=3):
    """The last clean ring at or below a split node (the fork's own level set mixes both branches)."""
    cur = nid
    for _ in range(steps):
        if nodes[cur].clean or nodes[cur].parent < 0:
            return cur
        cur = nodes[cur].parent
    return cur if nodes[cur].clean else nid


def stations_of(wood, nodes, ax):
    V = wood.topo.V
    st = []
    ids = ax.nodes
    after_split = False
    for j, nid in enumerate(ids):
        nd = nodes[nid]
        last = j == len(ids) - 1
        if len(nd.verts) < 3 and not last and j != 0:
            continue
        # a fork's level sets mix the branches: no station on them (the junction is kept as a patch)
        if 0 < j and not last and not nd.clean and (is_split(nodes, nid) or after_split):
            after_split = True
            continue
        after_split = False
        # a trunk starts at its first stock ring (a root set of scattered low vertices is no ring)
        if j == 0 and ax.parent < 0 and not nd.clean and len(ids) > 2:
            continue
        st.append({'c': nd.centroid.copy(), 'node': nid, 'clean': nd.clean, 'verts': nd.verts,
                   'sides': len(nd.verts) if nd.clean else 0, 'r_est': node_radius(V, nd), 'tip': len(nd.verts) == 1})
    # drop stations closer than 2 mm to the previous one (duplicate rings after welding)
    out = []
    for s in st:
        if out and np.linalg.norm(s['c'] - out[-1]['c']) < 0.002:
            if s['tip']:
                continue
            out[-1] = s
            continue
        out.append(s)
    return out


class Sectioner:
    """Plane sections of one component's faces near a point."""

    def __init__(self, wood):
        T = wood.topo
        self.V = T.V
        self.F = T.F
        self.fcomp = T.comp[T.F[:, 0]] if len(T.F) else np.zeros(0, np.int64)
        self.fcen = self.V[self.F].mean(1) if len(T.F) else np.zeros((0, 3))
        self.frad = np.linalg.norm(self.V[self.F] - self.fcen[:, None, :], axis=2).max(1) if len(T.F) else np.zeros(0)

    def section(self, comp, c, t, n, r_est, faces=None):
        """Section loop around c: (points 2D in the (n, b) frame, closed, facets, points 3D) or None."""
        R = 3.0 * max(r_est, 0.01) + 0.05
        sel = (self.fcomp == comp) & (np.linalg.norm(self.fcen - c, axis=1) <= R + self.frad)
        if faces is not None:
            sel &= faces
        F = self.F[sel]
        if not len(F):
            return None
        loops = G.plane_section(self.V, F, c, t)
        if not loops:
            return None
        b = np.cross(t, n)
        best = None
        opens = []
        r_ref = max(r_est, 0.005)
        for pts, closed in loops:
            q = np.stack([(pts - c) @ n, (pts - c) @ b], 1)
            if closed and len(q) >= 3:
                g, area = PF.polygon_area_centroid(q)
                rho = float(np.linalg.norm(q - g, axis=1).mean())
                off = float(np.linalg.norm(g))
                # the loop of this axis: around (or near) the expected centre, of about the expected radius
                if off > 1.5 * r_ref + 0.03 or rho < 0.2 * r_ref or rho > 2.5 * r_ref + 0.02:
                    continue
                score = off / r_ref + abs(np.log(max(rho, 1e-4) / r_ref)) + (0.0 if winding(q) != 0 else 1.0)
                if best is None or score < best[1]:
                    best = ((q, True, len(q), pts), score)
            else:
                if len(q) and np.linalg.norm(q, axis=1).min() <= 2.0 * r_ref + 0.02:
                    opens.append((q, pts))
        if best is not None:
            return best[0]
        if opens:
            q = np.concatenate([o[0] for o in opens])
            p = np.concatenate([o[1] for o in opens])
            # order by angle so the open pieces form one polyline around the centre
            o = np.argsort(np.arctan2(q[:, 1], q[:, 0]))
            return (q[o], False, len(q), p[o])
        return None


def clean_spine(st):
    """Drop stations that fold the spine back on itself: junction fragments and recentred stations whose
    centroid makes the path turn by more than 75 degrees, or that sit closer than a quarter radius to the previous
    station. Stock rings, joins and tips are kept."""
    st = list(st)
    changed = True
    while changed and len(st) > 2:
        changed = False
        for i in range(1, len(st) - 1):
            s = st[i]
            if s['clean'] or s.get('join') or s['tip']:
                continue
            a = s['c'] - st[i - 1]['c']
            b = st[i + 1]['c'] - s['c']
            la, lb = np.linalg.norm(a), np.linalg.norm(b)
            r = max(s['r_est'], 0.005)
            if la < 0.25 * r or lb < 1e-6 or np.dot(a, b) < np.cos(np.radians(75)) * la * lb:
                del st[i]
                changed = True
                break
    return st


def winding(q):
    """Winding number of the closed polygon q (2D) around the origin."""
    a = np.arctan2(q[:, 1], q[:, 0])
    d = np.diff(np.concatenate([a, a[:1]]))
    d = (d + np.pi) % (2 * np.pi) - np.pi
    return int(round(d.sum() / (2 * np.pi)))


def fit_stations(wood, sec, af):
    """Rings of one axis from its stations. Two passes: the first recentres fragment stations on their section's
    area centroid and drops stations without an acceptable section, the second fits the profiles."""
    V = wood.topo.V
    for it in range(2):
        st = af.st
        if len(st) < 2:
            af.R = None
            return
        c = np.array([s['c'] for s in st])
        R = TB.Rings(c, np.zeros(len(c)), np.zeros((len(c), 6)))
        profs = []
        keep = []
        r_prev = None
        for i, s in enumerate(st):
            if s['tip']:
                pr = PF.Profile()
                pr.mode = 'tip'
                profs.append((pr, None))
                keep.append(True)
                continue
            if s.get('join'):
                profs.append((None, None))
                keep.append(True)
                continue
            vert2 = None
            sec_ = None
            if s['clean']:
                d = V[s['verts']] - c[i]
                along = d @ R.t[i]
                d = d - np.outer(along, R.t[i])
                b = np.cross(R.t[i], R.n[i])
                vert2 = np.stack([d @ R.n[i], d @ b], 1)
                # a planar stock ring is its own cross-section: the ring polygon in the ring plane
                if np.ptp(along) <= max(0.5 * s['r_est'], 0.02):
                    o = np.argsort(np.arctan2(vert2[:, 1], vert2[:, 0]))
                    sec_ = (vert2[o], True, len(vert2), V[s['verts']][o])
            if sec_ is None:
                # fragments (junction level sets) have no reliable spread of their own: the previous ring's radius
                r_ref = s['r_est'] if (s['clean'] or r_prev is None) else min(s['r_est'], 1.5 * r_prev)
                sec_ = sec.section(af.comp, c[i], R.t[i], R.n[i], r_ref, getattr(af, 'face_mask', None))
            if sec_ is None:
                ends = i == 0 or i == len(st) - 1
                if ends:
                    pr = PF.Profile()
                    pr.r0 = s['r_est'] if r_prev is None else min(s['r_est'], r_prev)
                    if not s['clean'] and r_prev is None and i == 0:
                        # unknown at the very base: decided by the sanity pass from the neighbours
                        pr.r0 = s['r_est']
                    pr.mode = 'node'
                    pr.sides = s['sides']
                    profs.append((pr, None))
                    keep.append(True)
                else:
                    profs.append((None, None))
                    keep.append(False)
                continue
            q, closed, facets, pts = sec_
            if not s['clean'] and closed:
                cen2, area = PF.polygon_area_centroid(q)
                b = np.cross(R.t[i], R.n[i])
                s['c'] = c[i] + cen2[0] * R.n[i] + cen2[1] * b
                q = q - cen2
                if vert2 is not None:
                    vert2 = vert2 - cen2
                sec_ = (q, closed, facets, pts)
            sides = s['sides'] if s['clean'] else facets
            pr = PF.fit(q, closed, sides, vert2)
            if pr.r0 <= 0:
                profs.append((None, None))
                keep.append(i == 0 or i == len(st) - 1)
                continue
            r_prev = pr.r0
            profs.append((pr, sec_))
            keep.append(True)
        if it == 0:
            af.st = clean_spine([s for s, k in zip(st, keep) if k])
            continue
        profs = [p for p, k in zip(profs, keep) if k]
        af.st = [s for s, k in zip(st, keep) if k]
    st = af.st
    # radius sanity: a section that caught the wrong loop shows as a ring far off its neighbours
    good_r = [p.r0 if (p is not None and p.mode in ('minimax', 'vertices')) else np.nan for p, _ in profs]
    for i, (p, x) in enumerate(profs):
        # stock rings are their own section: only fragment/section stations can have caught a wrong loop
        if p is None or p.mode in ('tip', 'join') or st[i]['clean']:
            continue
        lo_, hi_ = max(0, i - 2), min(len(profs), i + 3)
        nb = [good_r[j] for j in range(lo_, hi_) if j != i and np.isfinite(good_r[j])]
        if len(nb) < 2:
            continue
        med = float(np.median(nb))
        own = st[i]['r_est']
        # off its neighbours and off its own node's spread: the section caught another piece's loop
        if (p.r0 > 2.5 * med + 0.01 or p.r0 < 0.3 * med) and abs(np.log(max(p.r0, 1e-4) / max(own, 1e-4))) > np.log(2.0):
            q = PF.Profile()
            q.r0 = med
            q.mode = 'interp'
            q.sides = p.sides
            profs[i] = (q, None)
            good_r[i] = np.nan
    c = np.array([s['c'] for s in st])
    # a join station (the parent's last clean ring) carries the branch's first section, round, inside the parent
    for i, (p, _) in enumerate(profs):
        if p is None:
            nxt = next((q for q, _ in profs[i + 1:] if q is not None and q.mode != 'tip'), None)
            pr = PF.Profile()
            pr.r0 = nxt.r0 if nxt is not None else st[i]['r_est']
            pr.mode = 'join'
            profs[i] = (pr, None)
    af.prof = [p for p, _ in profs]
    af.sections = [x for _, x in profs]
    if len(st) < 2:
        af.R = None
        return
    r0 = np.array([p.r0 for p in af.prof])
    f = np.array([p.f for p in af.prof])
    sides = np.array([p.sides if p.mode != 'tip' else 0 for p in af.prof])
    af.R = TB.Rings(c, r0, f, sides)


# ------------------------------------------------------------------------------------------------ tube fields
class TubeField:
    """Dense evaluation of an axis' tube for distance and inside tests."""

    def __init__(self, af, n_sub=N_SUB, n_th=N_TH):
        self.af = af
        self.ev = TB.evaluate(af.R, n_sub, n_th)
        self.n_sub = n_sub
        ev = self.ev
        self.C = ev['C']
        self.rmax = float(ev['r'].max()) if ev['r'].size else 0.0
        P = ev['P'].reshape(-1, 3)
        self.lo = np.minimum(P.min(0), self.C.min(0)) - 1e-3
        self.hi = np.maximum(P.max(0), self.C.max(0)) + 1e-3
        seg = np.diff(self.C, axis=0)
        self.L = np.linalg.norm(seg, axis=1)
        self.cum = np.concatenate([[0], np.cumsum(self.L)])
        # profile coefficients per row (lerped between rings like the reconstruction)
        Fr = af.R.f
        i0 = ev['seg']
        i1 = np.minimum(i0 + 1, len(af.R) - 1)
        uu = ev['u'][:, None]
        self.Frow = Fr[i0] * (1 - uu) + Fr[i1] * uu
        # nearest-segment index of the spine (points beyond the reach are far from this tube anyway)
        self.reach = self.rmax + 0.35
        self.sgrid = G.SegGrid(self.C, self.reach) if len(self.C) >= 2 else None

    def radial(self, Q):
        """Per point: (rho - r(th)) signed radial offset from the tube surface, axial overshoot beyond the ends,
        row index, angle."""
        ev = self.ev
        C = self.C
        if len(C) < 2:
            d = np.linalg.norm(Q - C[0], axis=1)
            return d, np.zeros(len(Q)), np.zeros(len(Q), np.int64), np.zeros(len(Q))
        seg, frac, dist = self.sgrid.query(Q)
        far = seg < 0
        seg = np.maximum(seg, 0)
        # everything interpolated at the projection between the two rows of the polyline segment
        f1 = frac[:, None]
        nxt = np.minimum(seg + 1, len(C) - 1)
        Cp = C[seg] * (1 - f1) + C[nxt] * f1
        T = ev['T'][seg] * (1 - f1) + ev['T'][nxt] * f1
        T /= np.maximum(np.linalg.norm(T, axis=1), 1e-12)[:, None]
        N = ev['N'][seg] * (1 - f1) + ev['N'][nxt] * f1
        N = N - T * np.einsum('ij,ij->i', N, T)[:, None]
        N /= np.maximum(np.linalg.norm(N, axis=1), 1e-12)[:, None]
        B = np.cross(T, N)
        d = Q - Cp
        ax_ = np.einsum('ij,ij->i', d, T)
        dp = d - T * ax_[:, None]
        rho = np.linalg.norm(dp, axis=1)
        th = np.arctan2(np.einsum('ij,ij->i', dp, B), np.einsum('ij,ij->i', dp, N))
        Fc = self.Frow[seg] * (1 - f1) + self.Frow[nxt] * f1
        prof = np.ones(len(Q))
        for k in range(1, 4):
            prof += Fc[:, 2 * k - 2] * np.cos(k * th) + Fc[:, 2 * k - 1] * np.sin(k * th)
        r = (ev['r0'][seg] * (1 - frac) + ev['r0'][nxt] * frac) * prof
        row = np.where(frac < 0.5, seg, nxt)
        over = np.zeros(len(Q))
        first = (seg == 0) & (frac <= 0) & (ax_ < 0)
        last = (nxt == len(C) - 1) & (frac >= 1) & (ax_ > 0)
        over[first] = -ax_[first]
        over[last] = ax_[last]
        off = rho - r
        # beyond the reach: far outside, never inside
        off[far] = self.reach
        over[far] = 0.0
        return off, over, row, th

    def inside_fast(self, Q, margin=0.01):
        """Approximate 'deeper than margin inside this tube' from the ring polyline and the ring radii (no
        Catmull-Rom, no profile): cheap enough for the hidden test of every tube sample."""
        R = self.af.R
        if len(R) < 2:
            return np.zeros(len(Q), bool)
        if getattr(self, '_rgrid', None) is None:
            self._rgrid = G.SegGrid(R.c, float(R.r0.max()) + 0.01)
        seg, u, d = self._rgrid.query(Q)
        near = seg >= 0
        out = np.zeros(len(Q), bool)
        if not near.any():
            return out
        q, sg, uu, dd = Q[near], seg[near], u[near], d[near]
        r = R.r0[sg] * (1 - uu) + R.r0[sg + 1] * uu
        # points beyond the end rings are not inside
        beyond = (((sg == 0) & ((q - R.c[0]) @ (R.c[1] - R.c[0]) < 0)) |
                  ((sg == len(R) - 2) & (uu >= 1) & ((q - R.c[-1]) @ (R.c[-1] - R.c[-2]) > 0)))
        out[near] = (dd < r - margin) & ~beyond
        return out

    def distance(self, Q):
        off, over, row, th = self.radial(Q)
        # beyond an end the nearest surface is the end disc rim (or the disc for points inside it)
        dd = np.where(over > 0, np.hypot(np.maximum(off, 0), over), np.abs(off))
        inside = (off < 0) & (over <= 0)
        return dd, inside, row


def fields_near(fields, P, pad):
    lo, hi = P.min(0) - pad, P.max(0) + pad
    return [f for f in fields if np.all(f.lo <= hi) and np.all(f.hi >= lo)]


# ------------------------------------------------------------------------------------------------ the kind
class KindFit:
    def __init__(self, kind, wood, nodes, axes, info):
        self.kind = kind
        self.wood = wood
        self.nodes = nodes
        self.axes_src = axes
        self.info = info
        self.af = []
        self.notes = collections.Counter()


def assign_faces(wood, nodes, info):
    """Axis of every welded face: majority of its vertices' nodes."""
    T = wood.topo
    nax = np.array([nodes[n].axis if n >= 0 else -1 for n in info['node_of']]) if len(info['node_of']) else np.zeros(0)
    fa = nax[T.F]
    out = fa[:, 0].copy()
    two = (fa[:, 1] == fa[:, 2]) & (fa[:, 1] >= 0)
    out[two] = fa[two, 1]
    return out


def axis_face_masks(wood, nodes, info):
    """Faces of every axis grown by one ring of neighbours (sections of welded pieces see only their own wood)."""
    T = wood.topo
    fa = assign_faces(wood, nodes, info)
    out = {}
    for a in np.unique(fa):
        m = fa == a
        vb = np.zeros(T.nv, bool)
        vb[T.F[m].ravel()] = True
        out[int(a)] = vb[T.F].any(1)
    return out


def build_fits(wood, nodes, axes, info):
    sec = Sectioner(wood)
    masks = axis_face_masks(wood, nodes, info) if len(wood.topo.F) else {}
    fits = []
    for ax in axes:
        af = AxisFit(ax)
        af.face_mask = masks.get(ax.id)
        af.st = stations_of(wood, nodes, ax)
        if ax.parent >= 0 and ax.attach_node >= 0 and af.st:
            # the branch leaves from its parent's last clean ring below the split: the tube starts there,
            # inside the parent
            jid = join_node(nodes, ax.attach_node)
            ax.attach_node = jid
            jn = nodes[jid]
            if np.linalg.norm(jn.centroid - af.st[0]['c']) > 0.005:
                af.st.insert(0, {'c': jn.centroid.copy(), 'node': jid, 'clean': False,
                                 'verts': np.zeros(0, np.int64), 'sides': 0, 'r_est': af.st[0]['r_est'],
                                 'tip': False, 'join': True})
        if len(af.st) < 2:
            af.R = None
            fits.append(af)
            continue
        fit_stations(wood, sec, af)
        if af.R is not None and ax.parent < 0 and info['info'].get(ax.comp, {}).get('ground'):
            extend_base(wood, ax, af)
        fits.append(af)
    return fits, sec


def extend_base(wood, ax, af):
    """A trunk whose first stock ring sits above the bottom of its stock piece (a closed or scattered base, no
    ring): the first ring's section is carried down along the axis to the lowest stock point of the piece."""
    T = wood.topo
    vs = np.flatnonzero((T.comp == ax.comp) & T.used)
    if not len(vs) or af.prof[0].mode in ('tip', 'join'):
        return
    R = af.R
    low = T.V[vs[np.argmin(T.V[vs, 1])]]
    t0 = R.t[0]
    drop = float(np.dot(R.c[0] - low, t0))
    if drop <= 0.02 or t0[1] <= 0.3:
        return
    base = R.c[0] - t0 * drop
    pr = PF.Profile()
    p0 = af.prof[0]
    pr.r0, pr.f, pr.K, pr.sides, pr.mode = p0.r0, p0.f.copy(), p0.K, p0.sides, 'base'
    af.st.insert(0, {'c': base, 'node': -1, 'clean': False, 'verts': np.zeros(0, np.int64), 'sides': p0.sides,
                     'r_est': p0.r0, 'tip': False, 'base': True})
    af.prof.insert(0, pr)
    af.sections.insert(0, None)
    c = np.concatenate([[base], R.c])
    af.R = TB.Rings(c, np.array([p.r0 for p in af.prof]), np.array([p.f for p in af.prof]),
                    np.array([p.sides if p.mode != 'tip' else 0 for p in af.prof]))


def link_axes(wood, nodes, axes, fits, info):
    """Parents, attachment arclengths, levels; continuation merges of trunk pieces."""
    by_id = {af.id: af for af in fits}
    comps_info = info['info']
    # in-component parents: attach at the parent's station nearest to the split node
    for af in fits:
        if af.R is None:
            continue
        ax = af.src
        if ax.parent >= 0:
            par = by_id[ax.parent]
            if par.R is not None:
                cen = nodes[ax.attach_node].centroid
                k = int(np.argmin(np.linalg.norm(par.R.c - cen, axis=1)))
                af.attach = float(par.R.s[k])
            af.parent = ax.parent
    # component roots: nearest spine point of the parent component's axes
    root_axis = {}
    for af in fits:
        if af.src.parent < 0:
            root_axis.setdefault(af.comp, af)
    for comp, af in root_axis.items():
        ci = comps_info.get(comp, {})
        pc = ci.get('parent', -1)
        if pc < 0 or af.R is None:
            af.parent = -1
            af.level = 0 if ci.get('ground', False) else 1
            continue
        best = None
        for o in fits:
            if o.comp != pc or o.R is None:
                continue
            P, seg, u = G.catmull_rom(o.R.c, 8)
            d = np.linalg.norm(P - af.R.c[0], axis=1)
            j = int(np.argmin(d))
            if best is None or d[j] < best[0]:
                sj = o.R.s[seg[j]] + u[j] * (o.R.s[min(seg[j] + 1, len(o.R) - 1)] - o.R.s[seg[j]])
                best = (d[j], o, float(sj))
        if best is None:
            af.parent = -1
            af.level = 1
            continue
        af.parent = best[1].id
        af.attach = best[2]
    # levels: roots of ground components are trunks; a piece that continues a trunk's open end is the trunk
    changed = True
    while changed:
        changed = False
        for af in fits:
            if af.R is None or af.merged or af.parent < 0:
                continue
            par = by_id[af.parent]
            if par.R is None or par.level != 0 or af.src.parent >= 0:
                continue
            if continues(par, af):
                merge_into(par, af, fits)
                changed = True
    for af in fits:
        if af.R is None or af.merged:
            continue
        if af.parent >= 0 and af.level != 0:
            af.level = 1
        elif af.parent < 0 and comps_info.get(af.comp, {}).get('ground', False):
            af.level = 0


def continues(par, ch):
    """A component that starts at a trunk's last ring along its direction continues the trunk."""
    if len(par.R) < 2 or len(ch.R) < 2:
        return False
    end_r = max(par.R.r0[-1], par.R.r0[-2] * 0.5, 0.02)
    gap = np.linalg.norm(ch.R.c[0] - par.R.c[-1])
    if gap > max(2.0 * end_r, 0.12):
        return False
    if ch.attach < par.R.s[-1] - max(3 * end_r, 0.3):
        return False
    cosang = float(np.dot(par.R.t[-1], ch.R.t[0]))
    return cosang > np.cos(np.radians(35))


def merge_into(par, ch, fits):
    """Append the rings of `ch` to `par` (the trunk continues in another component / bark part)."""
    st = list(par.st)
    prof = list(par.prof)
    secs = list(par.sections)
    c = list(par.R.c)
    # drop the trunk's last ring when the continuation's first ring lies on it
    if np.linalg.norm(ch.R.c[0] - par.R.c[-1]) < max(0.5 * par.R.r0[-1], 0.03):
        st, prof, secs, c = st[:-1], prof[:-1], secs[:-1], c[:-1]
    off = len(st)
    st += ch.st
    prof += ch.prof
    secs += ch.sections
    c += list(ch.R.c)
    par.st, par.prof, par.sections = st, prof, secs
    par.R = TB.Rings(np.array(c), np.array([p.r0 for p in prof]), np.array([p.f for p in prof]),
                     np.array([p.sides if p.mode != 'tip' else 0 for p in prof]))
    par.merged_children = getattr(par, 'merged_children', []) + [ch.id]
    par.comps = par.comps | ch.comps
    if getattr(par, 'face_mask', None) is not None and getattr(ch, 'face_mask', None) is not None:
        par.face_mask = par.face_mask | ch.face_mask
    ch.merged = True
    ch.merge_target = par.id
    ch.merge_offset = off
    # children of the merged piece hang on the trunk now
    s_shift = par.R.s[off] if off < len(par.R) else 0.0
    for o in fits:
        if o.parent == ch.id and not o.merged:
            o.parent = par.id
            o.attach = o.attach + s_shift
