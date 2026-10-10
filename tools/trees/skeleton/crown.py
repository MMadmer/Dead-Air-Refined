# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Stage 5: the crown field. The stock cards' opaque texels (alpha >= 0.5) splatted into a model-space grid
(0.25-0.5 m cells), occupancy = 1 - exp(-opaque card area in the cell / cell^2), closed (3x3x3), inner cavities
filled with a floor (branch space), smoothed, and clipped by the passable crown hull of the CFORM
(materials\\bush / bush_sux of the sampled placements in model space, solidified, grown by a margin)."""
import numpy as np

import geom as G

CAVITY_FLOOR = 40      # u8 occupancy of enclosed empty cells (room for branches, not for leaves)
CLOSE_FILL = 64        # u8 occupancy of the cells the closing adds
HULL_MARGIN = 0.75     # m the clip keeps outside the hull (the hull is a coarse, simplified copy)


def shift_or(m):
    """3x3x3 (cube) dilation of a boolean grid: separable one-cell dilations along x, y and z."""
    out = m.copy()
    for ax in range(3):
        sl1 = [slice(None)] * 3
        sl2 = [slice(None)] * 3
        sl1[ax], sl2[ax] = slice(1, None), slice(None, -1)
        a = out.copy()
        a[tuple(sl1)] |= out[tuple(sl2)]
        a[tuple(sl2)] |= out[tuple(sl1)]
        out = a
    return out


def dilate(m, n=1):
    for _ in range(n):
        m = shift_or(m)
    return m


def erode(m, n=1):
    for _ in range(n):
        m = ~shift_or(~m)
    return m


def exterior(solid):
    """Cells reachable from the grid border through non-solid cells (6-connected flood fill)."""
    ext = np.zeros_like(solid)
    free = ~solid
    fr = np.zeros_like(solid)
    fr[0, :, :] = fr[-1, :, :] = True
    fr[:, 0, :] = fr[:, -1, :] = True
    fr[:, :, 0] = fr[:, :, -1] = True
    fr &= free
    ext |= fr
    while fr.any():
        nb = np.zeros_like(solid)
        nb[1:] |= fr[:-1]
        nb[:-1] |= fr[1:]
        nb[:, 1:] |= fr[:, :-1]
        nb[:, :-1] |= fr[:, 1:]
        nb[:, :, 1:] |= fr[:, :, :-1]
        nb[:, :, :-1] |= fr[:, :, 1:]
        fr = nb & free & ~ext
        ext |= fr
    return ext


def box_blur(a):
    """3x3x3 box blur with clamped borders."""
    out = a.astype(np.float64)
    for ax in range(3):
        p = np.pad(out, [(1, 1) if i == ax else (0, 0) for i in range(3)], mode='edge')
        sl = [slice(None)] * 3
        acc = 0
        for o in range(3):
            sl[ax] = slice(o, o + out.shape[ax])
            acc = acc + p[tuple(sl)]
        out = acc / 3.0
    return out


class Field:
    def __init__(self, origin, cell, dims):
        self.origin = np.asarray(origin, np.float64)
        self.cell = float(cell)
        self.nx, self.ny, self.nz = (int(d) for d in dims)
        self.occ = np.zeros((self.nx, self.ny, self.nz), np.uint8)   # indexed [x, y, z]
        self.flags = 0
        self.stats = {}

    def index(self, P):
        return np.floor((P - self.origin) / self.cell).astype(np.int64)

    def centres(self):
        ix, iy, iz = np.meshgrid(np.arange(self.nx), np.arange(self.ny), np.arange(self.nz), indexing='ij')
        return self.origin + (np.stack([ix, iy, iz], -1) + 0.5) * self.cell

    def bytes(self):
        """x fastest, then z, then y (FORMAT.md)."""
        return np.ascontiguousarray(self.occ.transpose(1, 2, 0)).tobytes()


def make_grid(points, extra=None, max_cells=64):
    lo = points.min(0)
    hi = points.max(0)
    if extra is not None and len(extra):
        lo = np.minimum(lo, extra.min(0))
        hi = np.maximum(hi, extra.max(0))
    ext = float((hi - lo).max())
    cell = float(np.clip(ext / max_cells, 0.25, 0.5))
    lo = lo - cell
    hi = hi + cell
    dims = np.maximum(1, np.ceil((hi - lo) / cell).astype(np.int64))
    return Field(lo, cell, dims)


def lattice_points(V, F, spacing):
    """Barycentric lattice on every triangle with steps below `spacing` (watertight at a third of a cell)."""
    out = [V]
    for f in F:
        a, b, c = V[f[0]], V[f[1]], V[f[2]]
        L = max(np.linalg.norm(b - a), np.linalg.norm(c - a), np.linalg.norm(c - b))
        n = int(np.ceil(L / spacing))
        if n <= 0:
            continue
        i, j = np.meshgrid(np.arange(n + 1), np.arange(n + 1), indexing='ij')
        m = (i + j) <= n
        u, v = i[m] / n, j[m] / n
        out.append(a + (b - a) * u[:, None] + (c - a) * v[:, None])
    return np.concatenate(out)


def voxel_surface(field, V, F, spacing=None):
    """Cells touched by triangles (a lattice on each triangle finer than the cells)."""
    sp = spacing or field.cell / 3.0
    pts = lattice_points(V, F, sp)
    idx = field.index(pts)
    ok = np.all((idx >= 0) & (idx < (field.nx, field.ny, field.nz)), axis=1)
    m = np.zeros((field.nx, field.ny, field.nz), bool)
    m[idx[ok, 0], idx[ok, 1], idx[ok, 2]] = True
    return m


def winding(points, V, F):
    """Generalised winding number of the triangle soup (V, F) at the points (Van Oosterom & Strackee solid
    angles): about +-1 inside a closed surface, 0 outside, robust to the small holes of simplified hulls."""
    w = np.zeros(len(points))
    for f in F:
        a = V[f[0]] - points
        b = V[f[1]] - points
        c = V[f[2]] - points
        la, lb, lc = np.linalg.norm(a, axis=1), np.linalg.norm(b, axis=1), np.linalg.norm(c, axis=1)
        num = np.einsum('ij,ij->i', a, np.cross(b, c))
        den = la * lb * lc + np.einsum('ij,ij->i', a, b) * lc + np.einsum('ij,ij->i', a, c) * lb +             np.einsum('ij,ij->i', b, c) * la
        w += 2.0 * np.arctan2(num, den)
    return w / (4.0 * np.pi)


def hull_solid(field, hulls, card_mask, max_hulls=4):
    """Union of the placements' hull solids (cells whose winding number about the hull is beyond 0.5). A hull that
    encloses almost nothing of the cards or the whole grid is skipped. Returns (solid, used)."""
    solid = np.zeros((field.nx, field.ny, field.nz), bool)
    used = 0
    cen = field.centres().reshape(-1, 3)
    for V, F in hulls[:max_hulls]:
        if len(F) < 4:
            continue
        # outside the hull's box nothing is inside: only the cells of its box are tested
        P = V[F.ravel()]
        inb = np.all((cen >= P.min(0) - field.cell) & (cen <= P.max(0) + field.cell), axis=1)
        wn = np.zeros(len(cen))
        wn[inb] = winding(cen[inb], V, F)
        inside = (np.abs(wn) > 0.5).reshape(solid.shape)
        inside |= voxel_surface(field, V, F)
        cov = (inside & card_mask).sum() / max(card_mask.sum(), 1)
        if inside.mean() > 0.95 or cov < 0.25:
            continue
        solid |= inside
        used += 1
    return solid, used


def own_hull(V, F, lo, hi):
    """The hull pieces of this tree among the passable triangles in its placement box: connected pieces whose box
    lies mostly inside the card box (grown by 1 m); neighbours' hulls are dropped."""
    if not len(F):
        return V, F
    q = np.round(V * 1000).astype(np.int64)
    _, inv = np.unique(q, axis=0, return_inverse=True)
    inv = inv.ravel()
    Fi = inv[F]
    n = inv.max() + 1
    par = np.arange(n)

    def find(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    for a_, b_, c_ in Fi:
        ra, rb, rc = find(a_), find(b_), find(c_)
        par[ra] = rc
        par[rb] = rc
    lab = np.array([find(f[0]) for f in Fi])
    glo, ghi = lo - 1.0, hi + 1.0
    keep = np.zeros(len(F), bool)
    for g in np.unique(lab):
        m = lab == g
        P = V[F[m].ravel()]
        blo, bhi = P.min(0), P.max(0)
        vol = np.prod(np.maximum(bhi - blo, 1e-3))
        ov = np.prod(np.maximum(np.minimum(bhi, ghi) - np.maximum(blo, glo), 0))
        if ov >= 0.6 * vol:
            keep |= m
    return V, F[keep]


def build(points, weights, hulls, extra_points=None):
    """Crown field from opaque texel points (model space) and their areas; hulls = [(V, F) model space]."""
    if not len(points):
        return None
    lo, hi = points.min(0), points.max(0)
    hulls = [own_hull(V, F, lo, hi) for V, F in hulls]
    hv = [V[F.ravel()] for V, F in hulls if len(F)]
    extra = np.concatenate(hv) if hv else None
    if extra_points is not None and len(extra_points):
        extra = extra_points if extra is None else np.concatenate([extra, extra_points])
    f = make_grid(points, extra)
    idx = f.index(points)
    area = np.zeros((f.nx, f.ny, f.nz))
    np.add.at(area, (idx[:, 0], idx[:, 1], idx[:, 2]), weights)
    occ = 1.0 - np.exp(-area / (f.cell * f.cell))
    raw = occ > 0.02
    closed = erode(dilate(raw, 1), 1)
    occ = np.where(closed & ~raw, np.maximum(occ, CLOSE_FILL / 255.0), occ)
    # enclosed empty cells: branch space
    filled = ~exterior(closed)
    cav = filled & ~closed
    occ = np.where(cav, np.maximum(occ, CAVITY_FLOOR / 255.0), occ)
    f.flags |= 4 if cav.any() else 0
    sm = box_blur(occ)
    occ = np.maximum(sm, 0.5 * occ)
    occ[occ < 1.5 / 255.0] = 0.0
    before = float((occ > 0).sum())
    card_mask = occ > 0.05
    solid, used = hull_solid(f, hulls, card_mask)
    f.stats = {'cells': int(f.nx * f.ny * f.nz), 'occupied': int(before), 'hulls_used': used, 'hulls': len(hulls)}
    if used:
        margin = max(1, int(round(HULL_MARGIN / f.cell)))
        keep = dilate(solid, margin)
        clipped = (occ > 0) & ~keep
        f.stats['clipped_cells'] = int(clipped.sum())
        f.stats['clipped_area_share'] = float(area[clipped].sum() / max(area.sum(), 1e-12))
        occ = np.where(keep, occ, 0.0)
        f.flags |= 1
    else:
        f.flags |= 2
        f.stats['clipped_cells'] = 0
        f.stats['clipped_area_share'] = 0.0
    f.occ = np.clip(np.round(occ * 255.0), 0, 255).astype(np.uint8)
    f.stats['occupied_final'] = int((f.occ > 0).sum())
    return f
