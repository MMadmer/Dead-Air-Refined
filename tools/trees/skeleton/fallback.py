# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Second opinion for stock wood pieces the level-set decomposition reads badly (most of the piece ends up as
patches): the voxel skeleton of voxel.py gives new axes for the piece, the stations are fitted on the stock
sections exactly like the ring stations, and the version that keeps fewer stock triangles as patches wins."""
import numpy as np

import axes as AX
import fit as FT
import geom as G
import tube as TB
import contour as CT
import voxel as VX

POOR_SHARE = 0.5        # patch share of a piece's bark area above which the voxel skeleton is tried
MIN_FACES = 30


def component_share(wood, pbad, comp):
    T = wood.topo
    area = G.tri_normals(T.V, T.F)[1]
    m = T.comp[T.F[:, 0]] == comp
    return float(area[m & pbad].sum() / max(area[m].sum(), 1e-12))


def poor_components(wood, pbad):
    T = wood.topo
    if not len(T.F):
        return []
    area = G.tri_normals(T.V, T.F)[1]
    fc = T.comp[T.F[:, 0]]
    out = []
    for c in np.unique(fc):
        m = fc == c
        if m.sum() < MIN_FACES:
            continue
        share = float(area[m & pbad].sum() / max(area[m].sum(), 1e-12))
        if share > POOR_SHARE:
            out.append((int(c), share))
    return out


def piece_root(wood, info, comp, F):
    T = wood.topo
    ci = info['info'].get(comp, {})
    root = ci.get('root')
    return np.asarray(root, np.int64) if root is not None and len(root) else F[:1, 0]


def voxel_fits(wood, info, sec, comp, next_id):
    """Axis fits of one piece from its voxel skeleton (ids from next_id on)."""
    T = wood.topo
    F = T.F[T.comp[T.F[:, 0]] == comp]
    if not len(F):
        return []
    nodes = VX.skeleton(T.V, F, T.V[piece_root(wood, info, comp, F)])
    return chain_fits(wood, sec, comp, next_id, nodes, None, 'voxel')


def contour_fits(wood, info, sec, comp, next_id):
    """Axis fits of one piece from the Reeb graph of its geodesic contours (contour.py)."""
    T = wood.topo
    F = T.F[T.comp[T.F[:, 0]] == comp]
    if not len(F):
        return []
    nodes, meta = CT.skeleton(T.V, F, piece_root(wood, info, comp, F))
    return chain_fits(wood, sec, comp, next_id, nodes, meta, 'contour')


def chain_fits(wood, sec, comp, next_id, nodes, meta, how):
    """Axes from skeleton nodes (centre, radius, level, parent, size): chains by the larger subtree, stations on
    the node centres (about one every 0.6 local radius), fitted on the stock sections like the ring stations."""
    if len(nodes) < 2:
        return []
    chains = VX.axes_from_nodes(nodes)
    fits = []
    for k, (chain, pchain, att) in enumerate(chains):
        # thin slabs give dense nodes: keep one every ~0.6 local radius (ends kept)
        sel = [chain[0]]
        acc = 0.0
        for a, b in zip(chain[:-1], chain[1:]):
            acc += float(np.linalg.norm(nodes[b][0] - nodes[a][0]))
            if acc >= max(0.6 * nodes[b][1], 0.03) or b == chain[-1]:
                sel.append(b)
                acc = 0.0
        st = [{'c': np.asarray(nodes[n][0], np.float64), 'node': -1, 'clean': False,
               'verts': np.zeros(0, np.int64), 'sides': 0, 'r_est': max(float(nodes[n][1]), 0.01),
               'tip': bool(meta is not None and meta[n]['tip'])}
              for n in sel]
        if pchain >= 0:
            st.insert(0, {'c': np.asarray(nodes[att][0], np.float64), 'node': -1, 'clean': False,
                          'verts': np.zeros(0, np.int64), 'sides': 0, 'r_est': st[0]['r_est'], 'tip': False,
                          'join': True})
        ax = AX.Axis(next_id + k, comp, -1, 1, [])
        af = FT.AxisFit(ax)
        af.voxel = True
        af.how = how
        af.chain_parent = pchain
        af.att_centre = np.asarray(nodes[att][0], np.float64) if att >= 0 else None
        af.st = FT.clean_spine(st)
        if len(af.st) < 2:
            af.R = None
        else:
            FT.fit_stations(wood, sec, af)
        fits.append(af)
    # links inside the piece
    for af in fits:
        if af.R is None:
            continue
        if af.chain_parent >= 0:
            par = fits[af.chain_parent]
            if par.R is None:
                continue
            af.parent = par.id
            k = int(np.argmin(np.linalg.norm(par.R.c - af.att_centre, axis=1)))
            af.attach = float(par.R.s[k])
            af.level = 1
    return [af for af in fits if af.R is not None]


def replace_component(fits, comp, new, ground=True):
    """Swap the axes of one piece for `new`: the piece's root axis takes over the old root's parent, attach and
    level (without an old root: a trunk on a ground piece, else a floating limb the job hangs on the nearest trunk);
    axes of other pieces that hung on the old ones hang on the nearest new axis."""
    old = [af for af in fits if af.comp == comp and af.R is not None and not af.merged]
    old_ids = {af.id for af in old}
    old_root = next((af for af in old if af.parent not in old_ids), None)
    roots = [af for af in new if af.parent < 0]
    if old_root is not None:
        for r in roots:
            r.parent = old_root.parent
            r.attach = old_root.attach
            r.level = old_root.level
    else:
        for r in roots:
            r.parent = -1
            r.level = 0 if ground else 1
    kept = [af for af in fits if af.id not in old_ids]
    for af in kept:
        if af.parent in old_ids and af.R is not None:
            best = None
            for n in new:
                P, seg, u = G.catmull_rom(n.R.c, 8)
                d = np.linalg.norm(P - af.R.c[0], axis=1)
                j = int(np.argmin(d))
                if best is None or d[j] < best[0]:
                    i1 = min(seg[j] + 1, len(n.R) - 1)
                    best = (d[j], n.id, float(n.R.s[seg[j]] + u[j] * (n.R.s[i1] - n.R.s[seg[j]])))
            if best is not None:
                af.parent, af.attach = best[1], best[2]
    return kept + new


def face_axis_for(wood, comp, new, face_axis):
    """Faces of the piece go to the new axis whose spine is nearest."""
    T = wood.topo
    m = T.comp[T.F[:, 0]] == comp
    if not m.any() or not new:
        return face_axis
    cen = T.V[T.F[m]].mean(1)
    best = np.full(len(cen), np.inf)
    ids = np.full(len(cen), -1, np.int64)
    for af in new:
        C, _, _ = G.catmull_rom(af.R.c, 8)
        _, d, _ = G.polyline_project(cen, C)
        b = d < best
        best[b] = d[b]
        ids[b] = af.id
    out = face_axis.copy()
    out[np.flatnonzero(m)] = ids
    return out
