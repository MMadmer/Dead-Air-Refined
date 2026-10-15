# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""One skeleton end to end: stock axes and refit (or the synthetic axes of a procedural kind), patches, CFORM
gate, card records and level-2 axes, crown field, the .dtsk file and the measurements for the report."""
import collections
import json
import os
import time

import numpy as np

import axes as AX
import cards as CD
import cform as CF
import common as C
import crown as CR
import dtsk as DT
import fallback as FB
import fit as FT
import geom as G
import measure as MS
import xsection as PF
import tube as TB

CLIMB_DIST = 0.01      # m: stock wood within this of tree_trunk collision is climbable
GATE_P99 = 0.03        # m: p99 distance of the visible refit wood to the collision on climbable wood
CLIMB_TIERS = (None, 0.02, 0.015, 0.01, 0.005, 0.0025, 0.0)


# ------------------------------------------------------------------------------------------------ helpers
def load_cform(sid):
    p = os.path.join(C.WORK, 'cform', f'{sid}.npz')
    if not os.path.exists(p):
        return None
    z = np.load(p, allow_pickle=False)
    return {k: z[k] for k in z.files}


def placement_tris(cf, slot, kind):
    m = (cf['owner'] == slot) & (cf['kind'] == kind)
    return cf['tri'][m].astype(np.float64)


def model_mesh(Tw, xform):
    """World triangles (t, 3, 3) -> model-space vertices/faces."""
    if not len(Tw):
        return np.zeros((0, 3)), np.zeros((0, 3), np.int64)
    V = CF.to_model(Tw.reshape(-1, 3), xform)
    return V, np.arange(len(V)).reshape(-1, 3)


def spine_projection(fields, Q):
    """Nearest axis spine point of every point: (axis id, ring-scale arclength, distance)."""
    best = np.full(len(Q), np.inf)
    bax = np.full(len(Q), -1, np.int64)
    bs = np.zeros(len(Q))
    for fd in fields:
        Cc = fd.C
        if len(Cc) < 2:
            continue
        s, d, seg = G.polyline_project(Q, Cc)
        frac = np.clip((s - fd.cum[seg]) / np.maximum(fd.L[seg], 1e-12), 0, 1)
        # row arclength -> ring arclength
        rows_s = ring_s_of_rows(fd)
        sr = rows_s[seg] * (1 - frac) + rows_s[np.minimum(seg + 1, len(rows_s) - 1)] * frac
        better = d < best
        best[better] = d[better]
        bax[better] = fd.af.out_id
        bs[better] = sr[better]
    return bax, bs, best


def ring_s_of_rows(fd):
    if hasattr(fd, '_rs'):
        return fd._rs
    R = fd.af.R
    seg, u = fd.ev['seg'], fd.ev['u']
    i1 = np.minimum(seg + 1, len(R) - 1)
    fd._rs = R.s[seg] + u * (R.s[i1] - R.s[seg])
    return fd._rs


# ------------------------------------------------------------------------------------------------ stock axes
def climbable_faces(ref, cf):
    """Stock reference faces (bark, then caps) touching tree_trunk collision: a sample of the face within 1 cm of
    the collision in any sampled placement (inclusive: the gate's climbable wood is judged point by point)."""
    nf = len(ref.F)
    if cf is None or not nf:
        return np.zeros(nf, bool)
    pts, tri = MS.faces_samples(ref.V, ref.F)
    cnt = np.maximum(np.bincount(tri, minlength=nf), 1)
    votes = np.zeros(nf)
    n_pl = 0
    for slot in range(len(cf['xforms'])):
        Tw = placement_tris(cf, slot, 1)
        if not len(Tw):
            continue
        n_pl += 1
        Vm = CF.to_model(Tw.reshape(-1, 3), cf['xforms'][slot])
        g = G.TriGrid(Vm, np.arange(len(Vm)).reshape(-1, 3), reach=0.03, cell=0.06)
        d, _, _ = g.query(pts)
        on = np.isfinite(d) & (d <= CLIMB_DIST)
        votes += np.bincount(tri[on], minlength=nf) >= 1
    return votes >= 1 if n_pl else np.zeros(nf, bool)


def drop_junk(good, fields, fw, ref, notes):
    """Axes that carry no wood: no radius (a chain of single vertices), or a tube that runs mostly through the air
    (a level-set chain that jumped across a crotch). Their stock triangles become patches; their children hang on
    the dropped axis' parent."""
    keep = []
    fmap = {fd.af.id: fd for fd in fields}
    for af in good:
        fd = fmap.get(af.id)
        junk = af.R is None or float(np.max(af.R.r0)) < 0.005
        if not junk and fd is not None and af.level != 0:
            res = fw[af.id]
            tol = MS.sample_tol(fd, res, ref, None, None)
            vis = ~res['hidden']
            d = np.where(np.isfinite(res['d']), res['d'], ref.reach)
            air = vis & (d > 2 * tol) & (res['sgn'] > 0)
            junk = vis.sum() > 0 and air.sum() > 0.5 * vis.sum()
        if junk:
            notes['dropped_axes'] += 1
        else:
            keep.append(af)
    return keep


def stock_axes(rep, notes, cf=None):
    wood = AX.Wood(rep.bark_parts())
    nodes, axes, info = AX.build(wood)
    fits, sec = FT.build_fits(wood, nodes, axes, info)
    FT.link_axes(wood, nodes, axes, fits, info)
    ref = MS.StockRef(wood, rep.cap_parts())
    ref.climb = climbable_faces(ref, cf)
    good = MS.refine(fits, ref, sec, notes=notes)
    axis_end_flags(wood, good, rep.cap_parts())
    return wood, nodes, axes, info, fits, good, ref, sec


def axis_end_flags(wood, good, caps):
    """Cut/broken ends (an open boundary loop at the axis end) and caps (a cut-end cap part next to the end). A
    capped end does not run on into the cap as a cone: the tube stops at the last bark ring, the cap closes it."""
    T = wood.topo
    loop_c = [(T.V[lp].mean(0), len(lp)) for lp in T.loops]
    cap_pts = np.concatenate([p.P for p in caps]) if caps else np.zeros((0, 3))
    for af in good:
        R = af.R
        end = R.c[-1]
        r = max(R.r0[-2] if len(R) > 1 else 0.0, R.r0[-1], 0.02)
        if any(np.linalg.norm(c - end) < max(1.5 * r, 0.05) for c, n in loop_c):
            af.flags |= DT.A_CUT_END
        if len(cap_pts) and np.min(np.linalg.norm(cap_pts - end, axis=1)) < max(2.0 * r, 0.08):
            af.flags |= DT.A_CAP | DT.A_CUT_END
            if len(R) >= 3 and af.prof[-1].mode == 'tip':
                af.st, af.prof, af.sections = af.st[:-1], af.prof[:-1], af.sections[:-1]
                MS.rebuild(af, R.c[:-1])


RING_SHARE = 0.7        # pieces with less of their vertices on clean stock rings also try the voxel skeleton
MAX_PIECE_FACES = 8000  # larger pieces keep the hop-level reading (the second opinion's cost)
SWITCH_GAIN = 0.05      # score margin a second reading needs to replace the hop levels


def ring_vertex_share(wood, nodes):
    """Per wood piece: the share of its vertices that lie on clean level-set rings (3 vertices or more)."""
    T = wood.topo
    tot = np.bincount(T.comp[T.used], minlength=T.ncomp)
    on = np.zeros(T.ncomp)
    for nd in nodes:
        if nd.clean and len(nd.verts) >= 3:
            on[nd.comp] += len(nd.verts)
    return {c: float(on[c] / tot[c]) for c in range(T.ncomp) if tot[c]}


def piece_metrics(wood, ref, fits_all, fields, fw, face_axis, comp):
    """How well a set of axes explains one wood piece: the patch share of its bark area, the share of its stock
    surface within max(35 % r, 3 cm) of a tube (covered), the share of its tubes' visible surface out in the air by
    more than that (air). Score = patch share + (1 - covered) + 2 air (lower is better)."""
    T = wood.topo
    pbad, rev = MS.patches(wood, fits_all, fields, face_axis, ref.climb, fw, ref, None)
    area = G.tri_normals(T.V, T.F)[1]
    fc = T.comp[T.F[:, 0]]
    m = fc == comp
    share = float(area[m & pbad].sum() / max(area[m].sum(), 1e-12))
    pts, tri, d, bax, brow, hidden = rev
    fmap = {fd.af.id: fd for fd in fields}
    rad = np.zeros(len(pts))
    for aid in np.unique(bax[bax >= 0]):
        s = bax == aid
        rad[s] = fmap[aid].ev['r0'][brow[s]]
    sel = fc[tri] == comp
    cov = float(((d <= np.maximum(0.35 * rad, 0.03)) | hidden)[sel].mean()) if sel.any() else 0.0
    tot = air = 0
    for fd in fields:
        if fd.af.comp != comp:
            continue
        res = fw[fd.af.id]
        vis = ~res['hidden']
        dd = np.where(np.isfinite(res['d']), res['d'], ref.reach)
        out = vis & (res['sgn'] > 0) & (dd > np.maximum(0.35 * fd.ev['r0'][:, None], 0.03))
        tot += int(vis.sum())
        air += int(out.sum())
    air_share = air / max(tot, 1)
    return {'share': round(share, 3), 'covered': round(cov, 3), 'air': round(air_share, 3),
            'score': round(share + (1.0 - cov) + 2.0 * air_share, 3)}


# ------------------------------------------------------------------------------------------------ gate
def gate(cf, wood, ref, fields, fw, pbad, rev, face_axis):
    """CFORM gate. Per sampled placement its tree_trunk triangles are mapped into model space; a point of the
    visible refit wood (tube samples not inside another tube and not under a patch, plus the patch triangles)
    is on climbable wood when the stock surface next to it lies within 1 cm of that collision. Gate: p99 of the
    distance of those points to the collision <= 3 cm. The same distances in world units (times the placement's
    scale) are reported beside it."""
    out = {'placements': 0, 'with_trunk': 0, 'p99': None, 'p50': None, 'n': 0, 'climb_share': 0.0}
    T = wood.topo
    pts, tri, d, bax, brow, hidden = rev
    n_pl = len(cf['xforms']) if cf is not None else 0
    out['placements'] = n_pl
    nF = len(T.F)
    # visible tube samples and their nearest stock point
    tp, tf, ta = [], [], []
    for fd in fields:
        res = fw[fd.af.id]
        P = fd.ev['P'].reshape(-1, 3)
        trif = res['tri'].reshape(-1)
        # off the patches: a sample over a patched bark face or over a cut-end cap shows the stock there
        vis = ~res['hidden'].reshape(-1) & (trif >= 0) & (trif < nF)
        vis[vis] &= ~pbad[trif[vis]]
        tp.append(P[vis])
        tf.append(trif[vis])
        ta.append(np.full(int(vis.sum()), fd.af.id))
    tube_pts = np.concatenate(tp) if tp else np.zeros((0, 3))
    tube_tri = np.concatenate(tf) if tf else np.zeros(0, np.int64)
    tube_ax = np.concatenate(ta) if ta else np.zeros(0, np.int64)
    offending = []
    patch_ax = face_axis[tri[pbad[tri]]] if len(pts) else np.zeros(0, np.int64)
    per_axis = {}
    _, _, tube_cp = ref.grid.query(tube_pts) if len(tube_pts) else (None, None, np.zeros((0, 3)))
    patch_pts = pts[pbad[tri]] if len(pts) else np.zeros((0, 3))
    pf = np.concatenate([T.F[pbad], ref.F[ref.n_bark:]]) if nF else ref.F[ref.n_bark:]
    patch_grid = G.TriGrid(ref.V, pf, reach=0.3, cell=0.25) if len(pf) else None
    votes = np.zeros(len(pts))
    gate_d, world_d, rev_d, stock_w = [], [], [], []
    per_pl = []
    for slot in range(n_pl):
        Tw = placement_tris(cf, slot, 1)
        if not len(Tw):
            continue
        out['with_trunk'] += 1
        xf = cf['xforms'][slot]
        Vm = CF.to_model(Tw.reshape(-1, 3), xf)
        Fm = np.arange(len(Vm)).reshape(-1, 3)
        g = G.TriGrid(Vm, Fm, reach=0.1, cell=0.08)
        scale = float(np.cbrt(abs(np.linalg.det(xf[:3, :3]))))
        ds, _, _ = g.query(pts)
        votes += np.isfinite(ds) & (ds <= CLIMB_DIST)
        # the stock wood's own distance to its collision on climbable wood (the baseline: a placement scaled up
        # three times puts the stock trunk itself up to 3 cm off its collision in world units)
        on_s = np.isfinite(ds) & (ds <= CLIMB_DIST)
        stock_w.append(ds[on_s] * scale)
        dcp, _, _ = g.query(tube_cp)
        on_t = np.isfinite(dcp) & (dcp <= CLIMB_DIST)
        dt_, _, _ = g.query(tube_pts[on_t])
        # stock faces under tube samples over the bar (model or world units): climbable from the next round on
        # (the face sampling of climbable_faces can miss a corner that touches the collision)
        over = ~np.isfinite(dt_) | (dt_ > GATE_P99) | (dt_ * scale > GATE_P99)
        offending.append(tube_tri[on_t][over])
        dp_, _, _ = g.query(patch_pts)
        on_p = np.isfinite(dp_) & (dp_ <= CLIMB_DIST)
        dd = np.concatenate([np.where(np.isfinite(dt_), dt_, 0.1), dp_[on_p]])
        da = np.concatenate([tube_ax[on_t], patch_ax[on_p]])
        for aid in np.unique(da):
            per_axis.setdefault(int(aid), []).append(dd[da == aid])
        if len(dd):
            gate_d.append(dd)
            world_d.append(dd * scale)
            per_pl.append(float(np.percentile(dd, 99)))
        # reverse: climbable collision (within 1 cm of the stock surface) -> the refit tubes and the patches
        cs, _ = G.sample_tris(Vm, Fm, 0.08)
        dstock, _, _ = ref.grid.query(cs)
        csel = cs[np.isfinite(dstock) & (dstock <= CLIMB_DIST)]
        if len(csel):
            # collision buried inside the refit wood (limb bases inside the trunk) is out of reach: left out
            buried = np.zeros(len(csel), bool)
            for fd in fields:
                sel = np.flatnonzero(np.all((csel >= fd.lo) & (csel <= fd.hi), axis=1))
                if len(sel):
                    dd_, ins_, _ = fd.distance(csel[sel])
                    buried[sel[ins_ & (dd_ > CLIMB_DIST)]] = True
            csel = csel[~buried]
        if len(csel):
            dr, _, _, _ = MS.reverse(csel, fields, None, pad=0.3)
            dr = np.minimum(np.where(np.isfinite(dr), dr, 0.3), 0.3)
            if patch_grid is not None:
                dpg, _, _ = patch_grid.query(csel)
                dr = np.minimum(dr, np.where(np.isfinite(dpg), dpg, 0.3))
            rev_d.append(dr)
    k = max(1, out['with_trunk'])
    climb = votes >= 0.5 * k if out['with_trunk'] else np.zeros(len(pts), bool)
    out['climb_pts'] = climb
    area = G.tri_normals(T.V, T.F)[1] if nF else np.zeros(0)
    cf_face = np.zeros(nF)
    np.add.at(cf_face, tri, climb.astype(float))
    cnt = np.maximum(np.bincount(tri, minlength=nF), 1)
    out['climb_share'] = float((area * (cf_face / cnt)).sum() / max(area.sum(), 1e-12)) if nF else 0.0
    if gate_d:
        allg = np.concatenate(gate_d)
        out['n'] = int(len(allg))
        out['p99'] = float(np.percentile(allg, 99))
        out['p50'] = float(np.percentile(allg, 50))
        out['max'] = float(allg.max())
        out['world_p99'] = float(np.percentile(np.concatenate(world_d), 99))
        out['per_placement_p99'] = per_pl
    if rev_d:
        rd = np.concatenate(rev_d)
        out['reverse_p99'] = float(np.percentile(rd, 99))
    sw = np.concatenate(stock_w) if stock_w else np.zeros(0)
    out['stock_world_p99'] = float(np.percentile(sw, 99)) if len(sw) else None
    out['per_axis_p99'] = {a: float(np.percentile(np.concatenate(v), 99)) for a, v in per_axis.items()}
    out['offending_faces'] = np.unique(np.concatenate(offending)) if offending else np.zeros(0, np.int64)
    return out


def gate_ok(g):
    """Model space (the skeleton's units) and world space (times each placement's scale) both hold the gate; in world
    space a stock trunk already further than 3 cm from its own collision (scaled-up placements) is the bar instead."""
    if g.get('p99') is None:
        return True
    world_bar = max(GATE_P99, (g.get('stock_world_p99') or 0.0) + 0.0005)
    return g['p99'] <= GATE_P99 and (g.get('world_p99') or 0.0) <= world_bar


def ring_climbable(fields, rev, climb_pts):
    """Per axis: the share of its rings whose nearby stock wood lies within 1 cm of tree_trunk collision."""
    pts, tri, d, bax, brow, hidden = rev
    out = {}
    for fd in fields:
        af = fd.af
        m = len(af.R)
        sel = (bax == af.id) & ~hidden
        if not sel.any():
            out[af.id] = (0.0, np.zeros(m, bool))
            continue
        ring = np.clip(np.round(brow[sel] / fd.n_sub).astype(np.int64), 0, m - 1)
        c = climb_pts[sel]
        tot = np.bincount(ring, minlength=m)
        on = np.bincount(ring[c], minlength=m)
        rc = (tot > 0) & (on >= 0.5 * tot)
        out[af.id] = (float(rc.mean()), rc)
    return out


# ------------------------------------------------------------------------------------------------ synthetic axes
def synthetic_axes(card_list, rep):
    """Procedural kinds: one vertical synthetic level-0 axis per ground clump of card bases (single linkage in
    x/z within 1.5 m), from the lowest card point to the clump's crown centre."""
    if not card_list:
        return []
    B = np.array([c.base for c in card_list])
    pts = [c.opaque for c in card_list]
    allp = np.concatenate([p for p in pts if len(p)]) if any(len(p) for p in pts) else B
    y0 = float(min(allp[:, 1].min(), B[:, 1].min()))
    if rep.stems:
        seeds = np.array([[s[0], s[1]] for s in rep.stems])
    else:
        seeds = None
    # clumps by x/z single linkage of card bases
    n = len(B)
    lab = np.arange(n)
    xz = B[:, [0, 2]]
    for i in range(n):
        d = np.linalg.norm(xz - xz[i], axis=1)
        nb = np.flatnonzero(d < 1.5)
        root = lab[nb].min()
        for j in nb:
            lab[lab == lab[j]] = root
    out = []
    for g in np.unique(lab):
        mem = np.flatnonzero(lab == g)
        if len(mem) < max(2, 0.03 * n) and len(np.unique(lab)) > 1:
            continue
        P = np.concatenate([card_list[i].opaque for i in mem if len(card_list[i].opaque)] or [B[mem]])
        W = np.concatenate([card_list[i].opaque_w for i in mem if len(card_list[i].opaque)] or [np.ones(len(mem))])
        cen = (P * W[:, None]).sum(0) / max(W.sum(), 1e-12)
        x, z = cen[0], cen[2]
        if seeds is not None:
            k = int(np.argmin(np.hypot(seeds[:, 0] - x, seeds[:, 1] - z)))
            if np.hypot(seeds[k, 0] - x, seeds[k, 1] - z) < 2.0:
                x, z = seeds[k]
        top = max(cen[1], y0 + 0.5)
        h = top - y0
        r_base = float(np.clip(0.012 * h + 0.012, 0.015, 0.15))
        m = 5
        ys = np.linspace(y0, top, m)
        c = np.stack([np.full(m, x), ys, np.full(m, z)], 1)
        r = r_base * np.linspace(1.0, 0.45, m)
        out.append((c, r))
    return out


# ------------------------------------------------------------------------------------------------ the job
def run(sid, entry, write=True, report=True):
    t0 = time.time()
    notes = collections.Counter()
    kinds = [C.Kind(k) for k in entry['kinds']]
    rep = kinds[0]
    procedural = entry['procedural']
    cf = load_cform(sid)
    res = {'skel': sid, 'kinds': entry['kinds'], 'procedural': procedural, 'categories': entry['categories'],
           'states': entry['states'], 'species': entry['species']}
    final = []          # (AxisFit-like, level, parent final idx, attach, flags, climbable, rings: Rings)
    wood = None
    patch = None
    gate_res = None
    fields = []
    fw = rev = pbad = None
    if not procedural:
        wood, nodes, axes_src, info, fits, good, ref, sec = stock_axes(rep, notes, cf)
        caps = rep.cap_parts()
        fields = [FT.TubeField(af) for af in good]
        fw = MS.forward(fields, ref)
        kept = drop_junk(good, fields, fw, ref, notes)
        if len(kept) != len(good):
            good = kept
            fields = [FT.TubeField(af) for af in good]
            fw = MS.forward(fields, ref)
        face_axis = FT.assign_faces(wood, nodes, info)
        alias = {af.id: getattr(af, 'merge_target', af.id) for af in fits}
        face_axis = np.array([alias.get(int(a), int(a)) for a in face_axis], np.int64)
        # second opinion for pieces mostly left as patches (hop levels that are no rings: coarse or irregular
        # triangulations): the Reeb graph of geodesic contours and the voxel skeleton; the reading that explains the
        # piece best wins
        pbad0, _ = MS.patches(wood, fits, fields, face_axis, ref.climb, fw, ref, None)
        ring_share = ring_vertex_share(wood, nodes)
        fcomp = wood.topo.comp[wood.topo.F[:, 0]]
        for comp, share in FB.poor_components(wood, pbad0):
            if (fcomp == comp).sum() > MAX_PIECE_FACES:
                continue
            ground = bool(info['info'].get(comp, {}).get('ground', False))
            cur = piece_metrics(wood, ref, fits, fields, fw, face_axis, comp)
            rec = {'piece': comp, 'ring_share': round(ring_share.get(comp, 1.0), 3), 'levelsets': cur}
            # replace_component re-hangs other pieces' axes: their links are restored before every candidate
            links = [(af, af.parent, af.attach, af.level) for af in good]
            best = None
            # the voxel skeleton only for pieces whose hop levels are mostly no rings (it is slow and rarely wins
            # where the stock rings read)
            makers = [('contour', FB.contour_fits)]
            if ring_share.get(comp, 1.0) < RING_SHARE:
                makers.append(('voxel', FB.voxel_fits))
            for how, make in makers:
                for af, p_, a_, l_ in links:
                    af.parent, af.attach, af.level = p_, a_, l_
                try:
                    new = make(wood, info, sec, comp, max(af.id for af in fits) + 1)
                    new = MS.refine(new, ref, sec) if new else []
                    if new:
                        axis_end_flags(wood, new, caps)
                except Exception as e:  # a reading that breaks on a degenerate piece is no candidate
                    rec[how] = {'error': f'{type(e).__name__}: {e}'}
                    continue
                if not new:
                    continue
                c_good = FB.replace_component(good, comp, new, ground)
                c_fields = [FT.TubeField(af) for af in c_good]
                c_fw = MS.forward(c_fields, ref)
                kept = drop_junk(c_good, c_fields, c_fw, ref, collections.Counter())
                if len(kept) != len(c_good):
                    c_good = kept
                    c_fields = [FT.TubeField(af) for af in c_good]
                    c_fw = MS.forward(c_fields, ref)
                live = [af for af in new if af in c_good]
                if not live:
                    continue
                c_face = FB.face_axis_for(wood, comp, live, face_axis)
                m = piece_metrics(wood, ref, fits + new, c_fields, c_fw, c_face, comp)
                rec[how] = m
                if m['score'] < (best[0] if best else cur['score'] - SWITCH_GAIN):
                    best = (m['score'], how, new, {id(af) for af in c_good}, c_face, c_fields, c_fw)
            for af, p_, a_, l_ in links:
                af.parent, af.attach, af.level = p_, a_, l_
            if best is not None:
                _, how, new, kept_ids, face_axis, fields, fw = best
                good = [af for af in FB.replace_component(good, comp, new, ground) if id(af) in kept_ids]
                fits = fits + new
                rec['chosen'] = how
                notes[how + '_pieces'] += 1
            else:
                rec['chosen'] = 'levelsets'
            res.setdefault('second_opinion', []).append(rec)
        # patches at the shape tolerance; while the CFORM gate fails, climbable wood keeps more stock triangles
        # (tolerance on climbable faces 2 cm, then 1.5, 1, 0.5, 0.25 cm; at 0 all climbable wood stays stock, which
        # holds the gate even on placements scaled up three times)
        gate_res = None
        tier, extra = 0, 0
        while tier < len(CLIMB_TIERS):
            climb_tol = CLIMB_TIERS[tier]
            pbad, rev = MS.patches(wood, fits, fields, face_axis, ref.climb, fw, ref, climb_tol)
            if cf is None or rev is None:
                break
            gate_res = gate(cf, wood, ref, fields, fw, pbad, rev, face_axis)
            gate_res['climb_tol'] = climb_tol
            off = gate_res.pop('offending_faces')
            if gate_ok(gate_res):
                break
            # faces under the samples that broke the bar join the climbable wood; the last tier is tried again
            # while that still adds faces (at most 3 times)
            off = off[(off >= 0) & (off < len(ref.climb))]
            off = off[~ref.climb[off]]
            ref.climb[off] = True
            notes['gate_climb_faces'] += len(off)
            if tier == len(CLIMB_TIERS) - 1:
                if not len(off) or extra >= 3:
                    break
                extra += 1
                continue
            tier += 1
        MS.ring_errors(fields, fw, rev)
        rcl = ring_climbable(fields, rev, gate_res['climb_pts']) if gate_res is not None else {}
        for af in good:
            af.climbable, af.ring_climb = rcl.get(af.id, (0.0, np.zeros(len(af.R), bool)))
        res['notes'] = dict(notes)
    # ---- order the stock axes: parents first
    order = []
    if not procedural:
        by_id = {af.id: af for af in good}
        # a parent that was merged or dropped: walk to its trunk / nearest kept ancestor
        for af in good:
            p = af.parent
            while p >= 0 and p not in by_id:
                src = next((x for x in fits if x.id == p), None)
                if src is None:
                    p = -1
                    break
                p = getattr(src, 'merge_target', src.parent)
            af.parent = p
        roots = [af for af in good if af.parent < 0]
        queue = sorted(roots, key=lambda a: (a.level, -len(a.R)))
        seen = set()
        while queue:
            af = queue.pop(0)
            if af.id in seen:
                continue
            seen.add(af.id)
            order.append(af)
            queue += sorted([x for x in good if x.parent == af.id and x.id not in seen], key=lambda a: a.attach)
        # a limb piece without a parent (floating) hangs on the nearest trunk point
        trunks = [af for af in order if af.parent < 0 and af.level == 0]
        for af in order:
            if af.parent < 0 and af.level != 0 and trunks:
                best = None
                for tr in trunks:
                    P, seg, u = G.catmull_rom(tr.R.c, 16)
                    d = np.linalg.norm(P - af.R.c[0], axis=1)
                    j = int(np.argmin(d))
                    if best is None or d[j] < best[0]:
                        i1 = min(seg[j] + 1, len(tr.R) - 1)
                        best = (d[j], tr, float(tr.R.s[seg[j]] + u[j] * (tr.R.s[i1] - tr.R.s[seg[j]])))
                af.parent, af.attach = best[1].id, best[2]
        order = [af for af in order if af.parent < 0] + [af for af in order if af.parent >= 0]
        # parents first again after the re-hang
        placed, ordered = set(), []
        pending = list(order)
        while pending:
            nxt = [af for af in pending if af.parent < 0 or af.parent in placed]
            if not nxt:
                nxt = pending[:1]
            for af in nxt:
                ordered.append(af)
                placed.add(af.id)
                pending.remove(af)
        order = ordered
        for i, af in enumerate(order):
            af.out_id = i
        for af in order:
            af.out_parent = by_id[af.parent].out_id if af.parent >= 0 else -1
        if not order and wood is not None and wood.topo.used.any():
            # no readable stock axis at all: the stock wood stays as patches around one synthetic trunk line
            V = wood.topo.V[wood.topo.used]
            lo, hi = V.min(0), V.max(0)
            cx, cz = V[:, 0].mean(), V[:, 2].mean()
            c = np.stack([np.full(5, cx), np.linspace(lo[1], hi[1], 5), np.full(5, cz)], 1)
            r = np.full(5, 0.25 * min(hi[0] - lo[0], hi[2] - lo[2]) + 0.01) * np.linspace(1.0, 0.4, 5)
            af = FT.AxisFit(AX.Axis(0, -1, -1, 0, []))
            af.R = TB.Rings(c, r, np.zeros((5, 6)), np.zeros(5, np.int64))
            af.level, af.flags, af.out_id, af.out_parent, af.climbable = 0, DT.A_SYNTHETIC, 0, -1, 0.0
            order = [af]
            pbad = np.ones(len(wood.topo.F), bool)
            notes['synthetic_trunk'] += 1
    # ---- cards
    wood_grid = None
    if wood is not None and len(wood.topo.F):
        wood_grid = G.TriGrid(wood.topo.V, wood.topo.F, reach=1.0, cell=0.5)
    card_list = CD.collect(kinds)
    spines = [af.R.c for af in order if af.level == 0 and af.R is not None and len(af.R) >= 2]
    for cd in card_list:
        CD.analyse_card(cd, wood_grid, lowest=procedural, spines=spines)
    # ---- procedural: synthetic level-0 axes
    if procedural:
        syn = synthetic_axes(card_list, rep)
        order = []
        for i, (c, r) in enumerate(syn):
            af = FT.AxisFit(AX.Axis(i, -1, -1, 0, []))
            af.R = TB.Rings(c, r, np.zeros((len(c), 6)), np.zeros(len(c), np.int64))
            af.level = 0
            af.flags = DT.A_SYNTHETIC
            af.out_id = i
            af.out_parent = -1
            af.climbable = 0.0
            order.append(af)
        fields = [FT.TubeField(af) for af in order]
    else:
        fields_by_id = {fd.af.id: fd for fd in fields}
        fields = [fields_by_id[af.id] for af in order if af.id in fields_by_id and fields_by_id[af.id].af is af]
        if not fields and order:
            fields = [FT.TubeField(af) for af in order]
    # ---- link cards to the nearest stock/synthetic axis; level-2 axes along the painted branches
    card_axes = []
    if card_list and fields:
        bases = np.array([cd.base for cd in card_list])
        lax, ls, ld = spine_projection(fields, bases)
        for i, cd in enumerate(card_list):
            cd.link_axis, cd.link_s = int(lax[i]), float(ls[i])
    for ci, cd in enumerate(card_list):
        cd.axis = DT.NO_AXIS
        cd.index = ci
        if not fields or cd.link_axis < 0:
            continue
        par = order[cd.link_axis]
        R = par.R
        # parent spine point and radius at the attachment
        k = int(np.clip(np.searchsorted(R.s, cd.link_s), 1, len(R) - 1))
        u = (cd.link_s - R.s[k - 1]) / max(R.s[k] - R.s[k - 1], 1e-9)
        u = float(np.clip(u, 0, 1))
        sp = attach_point(R, cd.link_s)
        r_par = float(R.r0[k - 1] * (1 - u) + R.r0[k] * u)
        path = cd.path
        # the painted path simplified (Douglas-Peucker, 4 % of its length, at most 4 points base .. tip)
        L = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(path, axis=0), axis=1))])
        if L[-1] < 1e-3:
            continue
        keep = simplify_path(path, max(0.02, 0.04 * L[-1]), 4)
        pp = path[keep]
        rr = cd.path_r[keep]
        r_cap = min(0.7 * max(r_par, 0.004), 0.06)
        rr = np.minimum(np.maximum.accumulate(rr[::-1])[::-1], r_cap)
        rr = np.clip(np.minimum.accumulate(rr), 0.003, None)
        c = pp
        r = rr
        if np.linalg.norm(pp[0] - sp) > 0.01:
            c = np.concatenate([[sp], pp])
            r = np.concatenate([[rr[0]], rr])
        card_axes.append((cd, c, r, cd.link_axis, cd.link_s))
    # ---- assemble the file
    sk = DT.Skeleton()
    trunk_id = entry['skel']
    sk.trunk_key = int(trunk_id[1:] if trunk_id.startswith('p') else trunk_id, 16)
    rings = []
    axes_out = []
    ring_tol_over = 0
    for af in order:
        R = af.R
        first = sum(len(x) for x in rings)
        rr = np.zeros(len(R), DT.RING)
        rr['c'], rr['t'], rr['n'], rr['s'], rr['r0'], rr['f'] = R.c, R.t, R.n, R.s, R.r0, R.f
        rr['stock_sides'] = 0 if procedural else R.sides
        rr['fit_err'] = 0.0 if procedural else R.fit_err
        ring_tol_over += int((R.fit_err > PF.tol_of(R.r0)).sum()) if not procedural else 0
        rings.append(rr)
        flags = af.flags | (DT.A_SYNTHETIC if procedural else 0)
        p_end = float(order[af.out_parent].R.s[-1]) if af.out_parent >= 0 else 0.0
        axes_out.append({'parent': af.out_parent, 'level': 0 if af.level == 0 else 1,
                         'attach': float(np.clip(af.attach, 0.0, p_end)) if af.out_parent >= 0 else 0.0,
                         'first_ring': first,
                         'n_rings': len(R), 'climbable': float(af.climbable), 'flags': flags,
                         'reserved': DT.NO_AXIS})
    for ci, (cd, c, r, lax_, ls_) in enumerate(card_axes):
        R = TB.Rings(c, r, np.zeros((len(c), 6)), np.zeros(len(c), np.int64))
        first = sum(len(x) for x in rings)
        rr = np.zeros(len(R), DT.RING)
        rr['c'], rr['t'], rr['n'], rr['s'], rr['r0'], rr['f'] = R.c, R.t, R.n, R.s, R.r0, R.f
        rings.append(rr)
        pR = order[lax_].R
        cd.axis = len(axes_out)
        axes_out.append({'parent': lax_, 'level': 2, 'attach': float(np.clip(ls_, 0, pR.s[-1])), 'first_ring': first,
                         'n_rings': len(R), 'climbable': 0.0, 'flags': DT.A_FROM_CARDS | DT.A_SYNTHETIC,
                         'reserved': cd.index})
    sk.axes = axes_out
    sk.rings = np.concatenate(rings) if rings else np.zeros(0, DT.RING)
    # ---- patches: bark faces first, then the cut-end caps
    pv, pt = [], []
    n_bark_tris = 0
    if not procedural and wood is not None and pbad is not None and pbad.any():
        raw_faces = wood.Fraw[wood.face_raw[np.flatnonzero(pbad)]]
        n_bark_tris = len(raw_faces)
        pv_raw = np.unique(raw_faces)
        remap = np.full(len(wood.P), -1, np.int64)
        remap[pv_raw] = np.arange(len(pv_raw))
        P = wood.P[pv_raw]
        N = wood.N[pv_raw]
        UV = wood.UV[pv_raw]
        pv.append((P, N, UV))
        pt.append(remap[raw_faces])
    if not procedural:
        off = sum(len(x[0]) for x in pv)
        for cp in rep.cap_parts():
            vs = np.unique(cp.T)
            remap = np.full(len(cp.P), -1, np.int64)
            remap[vs] = np.arange(len(vs)) + off
            pv.append((cp.P[vs], cp.N[vs], cp.uv[vs]))
            pt.append(remap[cp.T])
            off += len(vs)
    if pv:
        P = np.concatenate([x[0] for x in pv])
        N = np.concatenate([x[1] for x in pv])
        N = N / np.maximum(np.linalg.norm(N, axis=1), 1e-9)[:, None]
        UV = np.concatenate([x[2] for x in pv])
        pax, ps, _ = spine_projection(fields, P) if fields else (np.zeros(len(P), np.int64), np.zeros(len(P)), None)
        sk.pverts = np.zeros(len(P), DT.PVERT)
        sk.pverts['p'], sk.pverts['n'], sk.pverts['uv'] = P, N, UV
        sk.pverts['axis'] = np.maximum(pax, 0)
        sk.pverts['s'] = ps
        sk.ptris = np.concatenate(pt).astype(np.uint32)
    # ---- cards
    if card_list:
        cr = np.zeros(len(card_list), DT.CARD)
        for i, cd in enumerate(card_list):
            cr[i] = (cd.base, cd.dir, cd.up, cd.len, cd.width, cd.axis)
        sk.cards = cr
    # ---- crown field
    opq = [cd.opaque for cd in card_list if len(cd.opaque)]
    hulls = []
    if cf is not None:
        for slot in range(len(cf['xforms'])):
            Tw = placement_tris(cf, slot, 2)
            if len(Tw):
                V, F = model_mesh(Tw, cf['xforms'][slot])
                hulls.append((V, F))
    field = None
    if opq:
        pts = np.concatenate(opq)
        w = np.concatenate([cd.opaque_w for cd in card_list if len(cd.opaque)])
        field = CR.build(pts, w, hulls)
    if field is not None:
        sk.crown = {'n': (field.nx, field.ny, field.nz), 'origin': field.origin, 'cell': field.cell,
                    'reserved': n_bark_tris}
        sk.grid = field.bytes()
    else:
        sk.crown = {'n': (0, 0, 0), 'origin': np.zeros(3), 'cell': 0.0, 'reserved': n_bark_tris}
        sk.grid = b''
    # ---- header
    pts_all = [sk.rings['c']] if len(sk.rings) else []
    if len(sk.pverts):
        pts_all.append(sk.pverts['p'])
    for cd in card_list:
        pts_all.append(cd.P)
    allp = np.concatenate(pts_all) if pts_all else np.zeros((1, 3))
    rmax = float(sk.rings['r0'].max()) if len(sk.rings) else 0.0
    sk.bmin = allp.min(0) - rmax
    sk.bmax = allp.max(0) + rmax
    fl = 0
    if entry['conifer']:
        fl |= DT.H_CONIFER
    if all(s in ('DEAD', 'BROKEN') for s in entry['states']):
        fl |= DT.H_DEAD
    # a broken top: a BROKEN kind, or a trunk whose cut/open end is the top of the wood (a snag, not a trunk that
    # ends inside the crown where the limbs take over)
    top_cut = False
    if order and not procedural and wood is not None:
        wy = wood.topo.V[wood.topo.used, 1]
        for af in order:
            if af.level == 0 and af.flags & DT.A_CUT_END:
                top_cut |= af.R.c[-1, 1] >= wy.min() + 0.9 * (wy.max() - wy.min())
    if 'BROKEN' in entry['states'] or top_cut:
        fl |= DT.H_BROKEN_TOP
    if procedural:
        fl |= DT.H_PROCEDURAL
    sk.flags = fl
    problems = DT.validate(sk)
    data = DT.pack(sk)
    back = DT.unpack(data)
    if len(back.rings) != len(sk.rings) or len(back.axes) != len(sk.axes):
        problems.append('round trip mismatch')
    if write:
        C.atomic_bytes(os.path.join(C.OUT_DTSK, f'{sid}.dtsk'), data)
    # ---- measurements
    res.update({'bytes': len(data), 'axes': len(sk.axes), 'axes_stock': len(order),
                'axes_level0': sum(1 for a in sk.axes if a['level'] == 0),
                'axes_level1': sum(1 for a in sk.axes if a['level'] == 1),
                'axes_level2': sum(1 for a in sk.axes if a['level'] == 2), 'rings': len(sk.rings),
                'rings_stock': int(sum(len(af.R) for af in order)), 'patch_verts': len(sk.pverts),
                'patch_tris': len(sk.ptris), 'patch_tris_bark': n_bark_tris, 'cards': len(sk.cards),
                'flags': fl, 'problems': problems, 'rings_over_tol': ring_tol_over,
                'crown': field.stats if field is not None else None,
                'crown_cell': field.cell if field is not None else None,
                'crown_dims': [field.nx, field.ny, field.nz] if field is not None else None})
    if not procedural and wood is not None:
        area = G.tri_normals(wood.topo.V, wood.topo.F)[1]
        res['bark_area'] = float(area.sum())
        res['patch_area_share'] = float(area[pbad].sum() / max(area.sum(), 1e-12)) if pbad is not None else 0.0
        res['patch_face_share'] = float(pbad.mean()) if pbad is not None and len(pbad) else 0.0
        fe = np.concatenate([af.R.fit_err for af in order]) if order else np.zeros(0)
        res['fit_err_p50'] = float(np.percentile(fe, 50)) if len(fe) else None
        res['fit_err_p99'] = float(np.percentile(fe, 99)) if len(fe) else None
        res['gate'] = {k: v for k, v in (gate_res or {}).items() if k != 'climb_pts'}
        # per axis rows
        rows = []
        for i, af in enumerate(order):
            R = af.R
            # stock -> tube errors of this axis' samples
            pts, tri, d, bax, brow, hidden = rev
            sel = (bax == af.id) & ~hidden & np.isfinite(d)
            fd = fields_by_id.get(af.id)
            if fd is not None:
                sel[sel] = d[sel] <= MS.near_reach(fd, brow[sel])
            dd = d[sel]
            rows.append({'axis': i, 'level': sk.axes[i]['level'], 'parent': sk.axes[i]['parent'], 'rings': len(R),
                         'stock_rings': int((R.sides > 0).sum()), 'length': float(R.s[-1]),
                         'r_base': float(R.r0[0]), 'max_err': float(R.fit_err.max()) if len(R) else 0.0,
                         'p99_err': float(np.percentile(dd, 99)) if len(dd) else 0.0,
                         'p99_cform': (gate_res or {}).get('per_axis_p99', {}).get(af.id),
                         'climbable': float(af.climbable), 'flags': int(sk.axes[i]['flags'])})
        res['axis_rows'] = rows
    res['time'] = time.time() - t0
    if report:
        import report as RP
        RP.sheet(sid, res, sk, kinds, wood, order, fields, fw, pbad, card_list, field, cf, gate_res)
    return res


def simplify_path(P, eps, max_pts):
    """Douglas-Peucker indices of a polyline (ends kept), the farthest point first, at most max_pts."""
    keep = [0, len(P) - 1]
    while len(keep) < max_pts:
        best = (eps, None)
        ks = sorted(keep)
        for a, b in zip(ks[:-1], ks[1:]):
            if b - a < 2:
                continue
            seg = P[b] - P[a]
            L2 = max(float(seg @ seg), 1e-12)
            t = np.clip((P[a + 1:b] - P[a]) @ seg / L2, 0, 1)
            d = np.linalg.norm(P[a + 1:b] - (P[a] + t[:, None] * seg), axis=1)
            j = int(np.argmax(d))
            if d[j] > best[0]:
                best = (d[j], a + 1 + j)
        if best[1] is None:
            break
        keep.append(best[1])
    return np.array(sorted(keep))


def attach_point(R, s):
    """Spine point at arclength s (centripetal Catmull-Rom, linear in the segment parameter)."""
    m = len(R)
    k = int(np.clip(np.searchsorted(R.s, s), 1, m - 1))
    u = float(np.clip((s - R.s[k - 1]) / max(R.s[k] - R.s[k - 1], 1e-9), 0, 1))
    ext = np.concatenate([[2 * R.c[0] - R.c[1]], R.c, [2 * R.c[-1] - R.c[-2]]])
    return G.cr_segment(ext[k - 1], ext[k], ext[k + 1], ext[k + 2], np.array([u]))[0]
