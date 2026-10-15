# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Stage 3b: the refit measured against the stock mesh, refinement, patches and per-ring errors."""
import collections

import numpy as np

import fit as FT
import geom as G
import xsection as PF
import tube as TB


class StockRef:
    """The stock wood surface as the reference: welded bark faces plus the cap faces, with face normals."""

    def __init__(self, wood, caps):
        V = [wood.topo.V]
        F = [wood.topo.F]
        self.n_bark = len(wood.topo.F)
        off = len(wood.topo.V)
        for cp in caps:
            F.append(cp.T + off)
            V.append(cp.P)
            off += len(cp.P)
        self.V = np.concatenate(V)
        self.F = np.concatenate(F) if F else np.zeros((0, 3), np.int64)
        self.fn, self.fa = G.tri_normals(self.V, self.F)
        self.reach = 0.08
        self.grid = G.TriGrid(self.V, self.F, reach=self.reach, cell=0.06)
        self.fcomp = np.concatenate([wood.topo.comp[wood.topo.F[:, 0]] if self.n_bark else np.zeros(0, np.int64),
                                     np.full(len(self.F) - self.n_bark, -1)]).astype(np.int64)
        self._own = {}

    def own_grid(self, comps):
        """The stock surface of some wood pieces (and every cut-end cap): a tube is measured against its own pieces
        only, a limb's end pushed into the trunk is no surface of the trunk (else the trunk's tube, outside that
        buried end, is pulled in round after round)."""
        key = tuple(sorted(comps))
        if key not in self._own:
            idx = np.flatnonzero(np.isin(self.fcomp, list(key)) | (self.fcomp < 0))
            if len(idx) == len(self.F):
                self._own[key] = (self.grid, None)
            else:
                self._own[key] = (G.TriGrid(self.V, self.F[idx], reach=self.reach, cell=0.06), idx)
        return self._own[key]

    def signed(self, Q, comps=None):
        """Distance to the stock surface (of the pieces `comps`, all by default), its side (+ outside) and the face
        (an index into F)."""
        grid, idx = (self.grid, None) if comps is None else self.own_grid(comps)
        d, tri, cp = grid.query(Q)
        if idx is not None:
            tri = np.where(tri >= 0, idx[np.maximum(tri, 0)], -1)
        sgn = np.ones(len(Q))
        ok = tri >= 0
        sgn[ok] = np.sign(np.einsum('ij,ij->i', Q[ok] - cp[ok], self.fn[tri[ok]]) + 1e-12)
        return d, sgn, tri


def forward(fields, ref, others=None):
    """Tube -> stock: per axis signed distance of every tube sample (rows x th) and the hidden mask (inside
    another axis' tube; `others` = all tubes when only some are measured)."""
    out = {}
    others = fields if others is None else others
    for fd in fields:
        P = fd.ev['P']
        Q = P.reshape(-1, 3)
        d, sgn, tri = ref.signed(Q, getattr(fd.af, 'comps', None))
        hidden = np.zeros(len(Q), bool)
        for other in FT.fields_near(others, Q, 0.05):
            if other is fd:
                continue
            sel = np.flatnonzero(np.all((Q >= other.lo) & (Q <= other.hi), axis=1) & ~hidden)
            if not len(sel):
                continue
            hidden[sel[other.inside_fast(Q[sel])]] = True
        out[fd.af.id] = {'d': d.reshape(P.shape[:2]), 'sgn': sgn.reshape(P.shape[:2]),
                         'hidden': hidden.reshape(P.shape[:2]), 'tri': tri.reshape(P.shape[:2])}
    return out


def reverse(Q, fields, owner_axis=None, pad=0.4):
    """Stock samples -> nearest tube: distance, axis, row; hidden where inside another axis' tube."""
    n = len(Q)
    best = np.full(n, np.inf)
    bax = np.full(n, -1, np.int64)
    brow = np.zeros(n, np.int64)
    hidden = np.zeros(n, bool)
    for fd in fields:
        sel = np.flatnonzero(np.all((Q >= fd.lo - pad) & (Q <= fd.hi + pad), axis=1))
        if not len(sel):
            continue
        dd, inside, row = fd.distance(Q[sel])
        better = dd < best[sel]
        best[sel[better]] = dd[better]
        bax[sel[better]] = fd.af.id
        brow[sel[better]] = row[better]
        if owner_axis is not None:
            other = owner_axis[sel] != fd.af.id
            hidden[sel[other & inside & (dd > 0.01)]] = True
    return best, bax, brow, hidden


def insert_station(sec, af, i):
    """A station in the middle of segment i from the stock section there."""
    R = af.R
    P, seg, u = G.catmull_rom(R.c, 2)
    mid = P[2 * i + 1]
    t = R.t[i] + R.t[i + 1]
    t /= np.linalg.norm(t)
    n = R.n[i] + R.n[i + 1]
    n = n - t * np.dot(n, t)
    n /= max(np.linalg.norm(n), 1e-12)
    r_est = 0.5 * (R.r0[i] + R.r0[i + 1])
    s_ = sec.section(af.comp, mid, t, n, max(r_est, 0.01), getattr(af, 'face_mask', None))
    st = {'c': mid.copy(), 'node': -1, 'clean': False, 'verts': np.zeros(0, np.int64), 'sides': 0,
          'r_est': r_est, 'tip': False, 'inserted': True}
    pr = None
    if s_ is not None:
        q, closed, facets, pts = s_
        c_sec = mid.copy()
        if closed:
            cen2, area = PF.polygon_area_centroid(q)
            b = np.cross(t, n)
            c_sec = mid + cen2[0] * n + cen2[1] * b
            q = q - cen2
        pr = PF.fit(q, closed, facets, None)
        # between two fitted rings the radius is known: a section far off it caught another piece
        if pr.r0 <= 0 or not (0.5 * r_est <= pr.r0 <= 2.0 * r_est + 0.005):
            pr = None
        else:
            # a section centroid that would fold the spine (a neighbouring branch's loop at a fork) is not used
            a_ = c_sec - R.c[i]
            b_ = R.c[i + 1] - c_sec
            fold = np.dot(a_, b_) < np.cos(np.radians(60)) * np.linalg.norm(a_) * np.linalg.norm(b_)
            if fold or np.linalg.norm(c_sec - mid) > max(r_est, 0.01):
                pr = None
            else:
                st['c'] = c_sec
    if pr is None:
        # nothing the stock section can add here: an interpolated station would change nothing
        return False
    af.st.insert(i + 1, st)
    af.prof.insert(i + 1, pr)
    af.sections.insert(i + 1, s_)
    c = np.insert(R.c, i + 1, st['c'], axis=0)
    rebuild(af, c)
    return True


def xs_interp(R, i):
    pr = PF.Profile()
    pr.r0 = 0.5 * (R.r0[i] + R.r0[i + 1])
    pr.f = 0.5 * (R.f[i] + R.f[i + 1])
    pr.mode = 'interp'
    return pr


def rebuild(af, c=None):
    c = af.R.c if c is None else c
    af.R = TB.Rings(c, np.array([p.r0 for p in af.prof]), np.array([p.f for p in af.prof]),
                    np.array([p.sides if p.mode != 'tip' else 0 for p in af.prof]))


CLIMB_TOL = 0.02   # m: refit tolerance on climbable wood (+ 1 cm stock-to-collision = the 3 cm gate)


def sample_tol(fd, res, ref, climb, climb_tol=CLIMB_TOL):
    """Tolerance per tube sample: max(2 cm, 5 % r); at most `climb_tol` where the nearest stock face is climbable
    (only when the CFORM gate asks for it)."""
    tol = np.broadcast_to(PF.tol_of(fd.ev['r0'])[:, None], res['d'].shape).copy()
    if climb is not None and climb_tol is not None:
        tri = res['tri']
        c = (tri >= 0) & (tri < len(climb))
        c[c] = climb[tri[c]]
        tol[c] = np.minimum(tol[c], climb_tol)
    return tol


def refine(fits, ref, sec, climb=None, notes=None, split_rounds=3, shrink_rounds=6):
    """Finer rings first: every segment whose tube misses the stock by more than the tolerance anywhere gets a
    station in its middle from the stock section there (down to segments of 0.6 r, at most as many added as the
    axis had). Then rings whose neighbourhood still lies outside the stock surface (bulges, junction voids) are
    pulled in by the excess, half the radius at most per round; the stock patches carry the shape there.
    `climb`: per stock reference face, climbable (the tolerance tightens to 2 cm)."""
    fits = [af for af in fits if af.R is not None and not af.merged and len(af.R) >= 2]
    inserted = collections.Counter()
    base_n = {af.id: len(af.R) for af in fits}
    hopeless = set()          # segments (pairs of station dicts) no section can improve
    shrunk = 0
    fmap = {af.id: FT.TubeField(af) for af in fits}
    fw = forward(list(fmap.values()), ref)
    for rnd in range(split_rounds + shrink_rounds):
        splitting = rnd < split_rounds
        dirty = []
        for fd in list(fmap.values()):
            af = fd.af
            m = len(af.R)
            ev = fd.ev
            res = fw[af.id]
            tol = sample_tol(fd, res, ref, climb)
            seg = ev['seg']
            if splitting:
                # beyond the grid's reach counts as the reach (a miss either way)
                rel = np.where(res['hidden'], 0.0, np.where(np.isfinite(res['d']), res['d'], ref.reach)) / tol
                ins = []
                for i in range(m - 1):
                    rows = np.flatnonzero(seg == i)
                    if not len(rows) or rel[rows].max() <= 1.0:
                        continue
                    if (id(af.st[i]), id(af.st[i + 1])) in hopeless:
                        continue
                    L = af.R.s[i + 1] - af.R.s[i]
                    rmin = min(af.R.r0[i], af.R.r0[i + 1])
                    if L > max(0.04, 0.6 * rmin) and inserted[af.id] < max(4, base_n[af.id]):
                        ins.append(i)
                done = False
                for i in sorted(ins, reverse=True):
                    pair = (id(af.st[i]), id(af.st[i + 1]))
                    if insert_station(sec, af, i):
                        inserted[af.id] += 1
                        done = True
                    else:
                        hopeless.add(pair)
                if done:
                    dirty.append(af)
                continue
            # pull in rings whose neighbourhood stays outside the stock surface
            vis = ~res['hidden'] & np.isfinite(res['d'])
            sd = res['d'] * res['sgn']
            outx = np.where(vis, sd - tol, -1.0)
            rows = np.arange(len(seg))
            shr = np.zeros(m)
            for i in range(m):
                near = np.flatnonzero(np.abs(rows - i * fd.n_sub) <= fd.n_sub // 2)
                e = outx[near].max() if len(near) else -1.0
                if e > 0.001 and af.prof[i].mode not in ('tip', 'join'):
                    shr[i] = e + 0.001
            if shr.any():
                for i in np.flatnonzero(shr):
                    PF.shrink(af.prof[i], shr[i])
                    af.prof[i].out_excess = 0.0
                    shrunk += 1
                rebuild(af)
                dirty.append(af)
        if not dirty:
            if splitting:
                continue
            break
        # only the changed tubes are rebuilt and measured again (against all tubes)
        for af in dirty:
            fmap[af.id] = FT.TubeField(af)
        fw.update(forward([fmap[af.id] for af in dirty], ref, list(fmap.values())))
    if notes is not None:
        notes['inserted_stations'] += int(sum(inserted.values()))
        notes['shrunk_rings'] += shrunk
    return fits


def faces_samples(V, F, spacing=0.06):
    pts, tri = G.sample_tris(V, F, spacing)
    first_face = np.full(len(V), -1, np.int64)
    for k in range(3):
        first_face[F[:, k]] = np.arange(len(F))
    vv = np.flatnonzero(first_face >= 0)
    return np.concatenate([pts, V[vv]]), np.concatenate([tri, first_face[vv]])


def patches(wood, fits_all, fields, face_axis, climb=None, fw=None, ref=None, climb_tol=None):
    """Bark faces the tubes do not reproduce within the tolerance (plus one ring of neighbours)."""
    T = wood.topo
    V, F = T.V, T.F
    if not len(F):
        return np.zeros(0, bool), None
    pts, tri = faces_samples(V, F)
    alias = {af.id: getattr(af, 'merge_target', af.id) for af in fits_all}
    fa = np.array([alias.get(int(a), int(a)) for a in face_axis])
    owner = fa[tri]
    d, bax, brow, hidden = reverse(pts, fields, owner)
    rad = np.zeros(len(pts))
    fmap = {fd.af.id: fd for fd in fields}
    for aid in np.unique(bax[bax >= 0]):
        m = bax == aid
        rad[m] = fmap[aid].ev['r0'][brow[m]]
    tol = PF.tol_of(rad)
    if climb is not None and climb_tol is not None:
        cb = climb[:len(F)][tri]
        tol = np.where(cb, np.minimum(tol, climb_tol), tol)
    bad = (d > tol) & ~hidden
    face_bad = np.zeros(len(F), bool)
    np.logical_or.at(face_bad, tri, bad)
    valid = set(fmap)
    face_bad |= np.array([a not in valid for a in fa])
    # tube samples that miss the stock (a tube too thin or off the stock surface where no other tube covers it):
    # the stock face under them is kept, so the visible wood there is the stock's
    if fw is not None and ref is not None:
        for fd in fields:
            res = fw[fd.af.id]
            tolf = sample_tol(fd, res, ref, climb, climb_tol)
            miss = ~res['hidden'] & np.isfinite(res['d']) & (res['d'] > tolf)
            t = res['tri'][miss]
            t = t[(t >= 0) & (t < len(F))]
            face_bad[t] = True
    # No growth ring: the tubes run on under the patches, so a patch border never opens a hole (the seam is
    # within the tolerance by definition)
    return face_bad, (pts, tri, d, bax, brow, hidden)


def near_reach(fd, rows):
    """How far from a tube stock wood still counts as its own: its radius there + 5 cm."""
    return fd.ev['r0'][rows] + 0.05


def ring_errors(fields, fw, rev):
    """fit_err of every ring: the largest tube->stock (visible samples) and stock->tube miss near it."""
    pts, tri, d, bax, brow, hidden = rev if rev is not None else (None,) * 6
    for fd in fields:
        af = fd.af
        m = len(af.R)
        res = fw[af.id]
        e = np.where(res['hidden'], 0.0, np.where(np.isfinite(res['d']), res['d'], 0.15)).max(1)
        nrow = len(e)
        fe = np.zeros(m)
        for i in range(m):
            lo, hi = max(0, i * fd.n_sub - fd.n_sub // 2), min(nrow, i * fd.n_sub + fd.n_sub // 2 + 1)
            fe[i] = e[lo:hi].max() if hi > lo else 0.0
        if d is not None:
            # stock wood far beyond this tube (another limb left without a tube) is not this ring's error
            sel = (bax == af.id) & ~hidden & np.isfinite(d)
            sel[sel] = d[sel] <= near_reach(fd, brow[sel])
            if sel.any():
                ring = np.clip(np.round(brow[sel] / fd.n_sub).astype(np.int64), 0, m - 1)
                np.maximum.at(fe, ring, np.minimum(d[sel], 1.0))
        af.R.fit_err = fe
