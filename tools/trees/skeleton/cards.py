# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Stage 4: the stock crown cards (flora\\leaf_wave parts) as branch records and level-2 axes.

Per card texture (crop of the card's uv box): the opaque mask (alpha >= 0.5), its chamfer distance transform, the
base (the thickest opaque run touching the border, as the shrub/tree card tools find it), the painted main branch
(cheapest path from the base through the opaque texels, cost 1/(1 + distance) per step, to the farthest texel) and
its painted half-width. Per card: the uv -> model map of its triangles carries the base, the path and the opaque
extent into model space; the base is the path end nearer to the stock wood."""
import heapq
import math
import os

import numpy as np
from PIL import Image

import common as C
import geom as G

ALPHA_CUT = 0.5
MASK_RES = 64


# ------------------------------------------------------------------------------------------------ textures
_TEX = {}


def texture_alpha(name):
    """Alpha of a stock texture (inventory PNG copy, <= 1024 px) as float [0, 1], or None."""
    if name in _TEX:
        return _TEX[name]
    p = os.path.join(C.INVENTORY, 'textures', name + '.png')
    a = None
    if os.path.exists(p):
        im = Image.open(p)
        if im.mode != 'RGBA':
            im = im.convert('RGBA')
        a = np.asarray(im, np.float32)[..., 3] / 255.0
    _TEX[name] = a
    return a


def chamfer(mask):
    """Chamfer (3-4) distance of every True pixel to the nearest False pixel or the border, in pixels."""
    H, W = mask.shape
    INF = 1e9
    d = np.where(mask, INF, 0.0)
    d = np.pad(d, 1, constant_values=0.0)
    for y in range(1, H + 1):
        row = d[y]
        up = d[y - 1]
        cand = np.minimum(np.minimum(up[1:-1] + 3, up[:-2] + 4), up[2:] + 4)
        row[1:-1] = np.minimum(row[1:-1], cand)
        for x in range(1, W + 1):
            if row[x] > row[x - 1] + 3:
                row[x] = row[x - 1] + 3
    for y in range(H, 0, -1):
        row = d[y]
        dn = d[y + 1]
        cand = np.minimum(np.minimum(dn[1:-1] + 3, dn[:-2] + 4), dn[2:] + 4)
        row[1:-1] = np.minimum(row[1:-1], cand)
        for x in range(W, 0, -1):
            if row[x] > row[x + 1] + 3:
                row[x] = row[x + 1] + 3
    return d[1:-1, 1:-1] / 3.0


_ANA = {}
_PATH = {}
PATH_RES = 128


def _crop(a, uv_lo, uv_hi):
    H, W = a.shape
    # crop in pixels; uv may wrap past 1 on tiled cards: clamp to the texture
    x0 = int(np.clip(math.floor(uv_lo[0] * W), 0, W - 1))
    x1 = int(np.clip(math.ceil(uv_hi[0] * W), x0 + 1, W))
    y0 = int(np.clip(math.floor(uv_lo[1] * H), 0, H - 1))
    y1 = int(np.clip(math.ceil(uv_hi[1] * H), y0 + 1, H))
    return a[y0:y1, x0:x1], x0 / W, y0 / H


def _coverage(crop, n):
    h, w = crop.shape
    sy, sx = max(1, h // n), max(1, w // n)
    ch, cw = (h // sy) * sy, (w // sx) * sx
    cov = (crop[:ch, :cw] >= ALPHA_CUT).reshape(ch // sy, sy, cw // sx, sx).mean((1, 3))
    return cov, ch, cw


def analyse(tex, uv_lo, uv_hi):
    """Opaque coverage (MASK_RES, for the crown field) and the branch analysis mask (PATH_RES, any opaque pixel:
    thin twigs stay connected) of the card texture crop [uv_lo, uv_hi] (uv, v down); its distance transform and
    the base candidates: per opaque run on the crop border, its thickest texel. None without a texture."""
    key = (tex, tuple(np.round(uv_lo, 3)), tuple(np.round(uv_hi, 3)))
    if key in _ANA:
        return _ANA[key]
    a = texture_alpha(tex)
    if a is None:
        _ANA[key] = None
        return None
    H, W = a.shape
    crop, u0, v0 = _crop(a, uv_lo, uv_hi)
    cov, ch, cw = _coverage(crop, MASK_RES)
    res = {'key': key, 'cov': cov, 'mask': cov >= 0.5, 'uv_lo': np.array([u0, v0]),
           'uv_px': np.array([(cw / W) / cov.shape[1], (ch / H) / cov.shape[0]])}
    pc, pch, pcw = _coverage(crop, PATH_RES)
    m = pc > 0.15
    res['pmask'] = m
    res['puv_px'] = np.array([(pcw / W) / pc.shape[1], (pch / H) / pc.shape[0]])
    if m.sum() < 4:
        res.update(dt=None, cands=[], tex_base=None)
        _ANA[key] = res
        return res
    dt = chamfer(m)
    res['dt'] = dt
    mh, mw = m.shape
    # border runs (around the crop) and their thickest texel
    ring = [(0, x) for x in range(mw)] + [(y, mw - 1) for y in range(1, mh)] + \
           [(mh - 1, x) for x in range(mw - 2, -1, -1)] + [(y, 0) for y in range(mh - 2, 0, -1)]
    cands = []
    run = []
    for (y, x) in ring + ring[:1]:
        if m[y, x]:
            run.append((y, x))
        elif run:
            cands.append(max(run, key=lambda q: dt[q]))
            run = []
    if run:
        cands.append(max(run, key=lambda q: dt[q]))
    if not cands:
        y, x = np.unravel_index(int(np.argmax(dt)), dt.shape)
        cands = [(int(y), int(x))]
    cands = sorted(set((int(y), int(x)) for y, x in cands), key=lambda q: -dt[q])[:48]
    res['cands'] = cands
    res['tex_base'] = cands[0]
    _ANA[key] = res
    return res


def px_to_uv(res, p):
    p = np.atleast_2d(np.asarray(p, np.float64))
    return res['uv_lo'] + (p[:, ::-1] + 0.5) * res['puv_px']


def path_from(res, base):
    """The painted main branch from a base texel: cheapest path through the analysis mask (cost 1 / (1 + distance
    to the transparent)) to the texel farthest from the base. Returns (uv path (k, 2), half-width in uv)."""
    ck = (res['key'], tuple(base))
    if ck in _PATH:
        return _PATH[ck]
    m, dt = res['pmask'], res['dt']
    mh, mw = m.shape
    by, bx = base
    cost = np.full(m.shape, np.inf)
    prev = np.full(m.shape + (2,), -1, np.int64)
    cost[by, bx] = 0.0
    pq = [(0.0, by, bx)]
    step = [(-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0), (-1, -1, 1.414), (-1, 1, 1.414), (1, -1, 1.414),
            (1, 1, 1.414)]
    while pq:
        c, y, x = heapq.heappop(pq)
        if c > cost[y, x]:
            continue
        for dy, dx, L in step:
            yy, xx = y + dy, x + dx
            if 0 <= yy < mh and 0 <= xx < mw and m[yy, xx]:
                nc = c + L / (1.0 + dt[yy, xx])
                if nc < cost[yy, xx]:
                    cost[yy, xx] = nc
                    prev[yy, xx] = (y, x)
                    heapq.heappush(pq, (nc, yy, xx))
    reach = np.isfinite(cost)
    yy, xx = np.nonzero(reach)
    far = int(np.argmax((yy - by) ** 2 + (xx - bx) ** 2))
    path = [(yy[far], xx[far])]
    while (path[-1][0], path[-1][1]) != (by, bx):
        py, px = prev[path[-1][0], path[-1][1]]
        if py < 0:
            break
        path.append((py, px))
    path = np.array(path[::-1], np.float64)
    hw = dt[path[:, 0].astype(int), path[:, 1].astype(int)] * float(np.mean(res['puv_px']))
    out = (px_to_uv(res, path), hw)
    _PATH[ck] = out
    return out


# ------------------------------------------------------------------------------------------------ cards
class Card:
    __slots__ = ('P', 'UV', 'T', 'tex', 'part', 'base', 'dir', 'up', 'len', 'width', 'path', 'path_r', 'axis',
                 'link_axis', 'link_s', 'opaque', 'opaque_w', 'normal', 'index')


def card_components(part):
    """Connected pieces (cards) of one card part: vertex index lists and triangles."""
    T = part.T
    n = len(part.P)
    par = np.arange(n)

    def find(a):
        while par[a] != a:
            par[a] = par[par[a]]
            a = par[a]
        return a
    for a, b, c in T:
        ra, rb, rc = find(a), find(b), find(c)
        par[ra] = rc
        par[rb] = rc
    lab = np.array([find(i) for i in range(n)])
    tl = lab[T[:, 0]]
    out = []
    for g in np.unique(tl):
        tri = T[tl == g]
        vs = np.unique(tri)
        out.append((vs, tri))
    return out


def _bary(card):
    A = card.UV[card.T[:, 0]]
    B = card.UV[card.T[:, 1]]
    Cc = card.UV[card.T[:, 2]]
    v0, v1 = B - A, Cc - A
    den = v0[:, 0] * v1[:, 1] - v0[:, 1] * v1[:, 0]
    return A, v0, v1, den


def uv_to_model(card, uv):
    """Model-space points of uv coordinates through the card triangles (barycentric). Where several triangles
    share the uv (crossed planes of one card mesh) the one continuing the previous point wins; outside every
    triangle the least-outside triangle's plane extends."""
    uv = np.atleast_2d(uv)
    A, v0, v1, den = _bary(card)
    ok = np.abs(den) > 1e-12
    out = np.zeros((len(uv), 3))
    prev = None
    for i, p in enumerate(uv):
        d = p - A
        with np.errstate(divide='ignore', invalid='ignore'):
            b1 = (d[:, 0] * v1[:, 1] - d[:, 1] * v1[:, 0]) / den
            b2 = (v0[:, 0] * d[:, 1] - v0[:, 1] * d[:, 0]) / den
        b0 = 1 - b1 - b2
        inside = ok & (b0 >= -1e-6) & (b1 >= -1e-6) & (b2 >= -1e-6)
        if inside.any():
            ks = np.flatnonzero(inside)
        else:
            worst = np.where(ok, np.minimum(np.minimum(b0, b1), b2), -np.inf)
            ks = np.array([int(np.argmax(worst))])
        t = card.T[ks]
        P = card.P[t[:, 0]] * b0[ks, None] + card.P[t[:, 1]] * b1[ks, None] + card.P[t[:, 2]] * b2[ks, None]
        j = 0 if prev is None else int(np.argmin(np.linalg.norm(P - prev, axis=1)))
        out[i] = P[j]
        prev = out[i]
    return out


def texel_points(card, ana):
    """Model-space centres and areas (m^2) of the opaque mask texels of a card (every triangle splats the texels
    of its own uv triangle, so crossed planes sharing uv each get theirs)."""
    if ana is None:
        return np.zeros((0, 3)), np.zeros(0)
    cov = ana['cov']
    yy, xx = np.nonzero(cov > 0)
    if not len(yy):
        return np.zeros((0, 3)), np.zeros(0)
    uv = ana['uv_lo'] + (np.stack([xx, yy], 1) + 0.5) * ana['uv_px']
    A, v0, v1, den = _bary(card)
    texel_uv = float(ana['uv_px'][0] * ana['uv_px'][1])
    Ps, Ws = [], []
    for k in np.flatnonzero(np.abs(den) > 1e-12):
        d = uv - A[k]
        b1 = (d[:, 0] * v1[k, 1] - d[:, 1] * v1[k, 0]) / den[k]
        b2 = (v0[k, 0] * d[:, 1] - v0[k, 1] * d[:, 0]) / den[k]
        b0 = 1 - b1 - b2
        ins = (b0 >= -1e-6) & (b1 >= -1e-6) & (b2 >= -1e-6)
        if not ins.any():
            continue
        t = card.T[k]
        Ps.append(card.P[t[0]] * b0[ins, None] + card.P[t[1]] * b1[ins, None] + card.P[t[2]] * b2[ins, None])
        a3 = 0.5 * np.linalg.norm(np.cross(card.P[t[1]] - card.P[t[0]], card.P[t[2]] - card.P[t[0]]))
        Ws.append(cov[yy[ins], xx[ins]] * texel_uv * (a3 / (0.5 * abs(den[k]))))
    if not Ps:
        return np.zeros((0, 3)), np.zeros(0)
    return np.concatenate(Ps), np.concatenate(Ws)


def collect(kinds):
    """Unique cards over the kinds of one skeleton (duplicates: two-sided copies and the same card in another
    dressing; the first kind's texture names the card)."""
    seen = {}
    cards = []
    for kind in kinds:
        for part in kind.card_parts():
            for vs, tri in card_components(part):
                key = tuple(sorted(map(tuple, np.round(part.P[vs] * 200).astype(np.int64))))
                if key in seen:
                    continue
                cd = Card()
                remap = {v: i for i, v in enumerate(vs)}
                cd.P = part.P[vs]
                cd.UV = part.uv[vs]
                cd.T = np.array([[remap[a], remap[b], remap[c]] for a, b, c in tri], np.int64)
                cd.tex = part.texture
                cd.part = part.index
                seen[key] = len(cards)
                cards.append(cd)
    return cards


def analyse_card(card, wood_grid=None, lowest=False, spines=None):
    """Base, direction, up, length and width of one card in model space; the painted path and its radii. The base
    is the card's border entry nearest to the stock wood (`wood_grid`, within 0.75 m), else the entry nearest to the
    trunk spines (`spines`: crown cards far out hang on branches that come from the trunk), else the lowest entry
    (`lowest`, procedural bushes standing on the ground), else the thickest entry of the texture."""
    lo, hi = card.UV.min(0), card.UV.max(0)
    ana = analyse(card.tex, lo, hi)
    pts, w = texel_points(card, ana)
    card.opaque, card.opaque_w = pts, w
    nrm = np.cross(card.P[card.T[:, 1]] - card.P[card.T[:, 0]], card.P[card.T[:, 2]] - card.P[card.T[:, 0]]).sum(0)
    nn = np.linalg.norm(nrm)
    card.normal = nrm / nn if nn > 1e-12 else np.array([0.0, 0.0, 1.0])
    path = None
    if ana is not None and ana.get('cands'):
        cands = ana['cands']
        P3 = uv_to_model(card, px_to_uv(ana, cands))
        base = ana['tex_base']
        chosen = False
        if wood_grid is not None:
            d, _, _ = wood_grid.query(P3)
            dd = np.where(np.isfinite(d), d, np.inf)
            if dd.min() < 0.75:
                base = cands[int(np.argmin(dd))]
                chosen = True
        if not chosen and spines:
            ds = np.min([G.polyline_project(P3, sp)[1] for sp in spines if len(sp) >= 2], axis=0)
            base = cands[int(np.argmin(ds))]
            chosen = True
        if not chosen and lowest:
            base = cands[int(np.argmin(P3[:, 1]))]
        puv, hw_uv = path_from(ana, base)
        if len(puv) >= 2:
            path = uv_to_model(card, puv)
            area3 = 0.5 * np.linalg.norm(np.cross(card.P[card.T[:, 1]] - card.P[card.T[:, 0]],
                                                  card.P[card.T[:, 2]] - card.P[card.T[:, 0]]), axis=1).sum()
            area_uv = 0.5 * np.abs(np.cross(card.UV[card.T[:, 1]] - card.UV[card.T[:, 0]],
                                            card.UV[card.T[:, 2]] - card.UV[card.T[:, 0]])).sum()
            hw = hw_uv * math.sqrt(max(area3, 1e-12) / max(area_uv, 1e-12))
    if path is None:
        # no texture: the card's long axis through its centre
        c = card.P.mean(0)
        X = card.P - c
        wv, U = np.linalg.eigh(X.T @ X)
        ax = U[:, -1]
        pr = X @ ax
        path = np.stack([c + ax * pr.min(), c + ax * pr.max()])
        hw = np.array([0.01, 0.003])
        if wood_grid is not None:
            d, _, _ = wood_grid.query(path)
            d = np.where(np.isfinite(d), d, 1e3)
            if d[1] < d[0]:
                path = path[::-1]
        if not len(pts):
            card.opaque, card.opaque_w = card.P.copy(), np.full(len(card.P), 0.01)
            pts = card.opaque
    card.path = path
    card.path_r = hw
    card.base = path[0].copy()
    tip = path[-1]
    dvec = tip - card.base
    if np.linalg.norm(dvec) < 1e-6 and len(pts):
        dvec = pts.mean(0) - card.base
    if np.linalg.norm(dvec) < 1e-6:
        dvec = np.array([0.0, 1.0, 0.0])
    card.dir = dvec / np.linalg.norm(dvec)
    # up: in the card plane, perpendicular to dir, towards model +Y (or the card normal side when level)
    up = np.cross(card.normal, card.dir)
    if np.linalg.norm(up) < 1e-9:
        up = G.any_perp(card.dir)
    up /= np.linalg.norm(up)
    if up[1] < 0 or (abs(up[1]) < 1e-6 and np.dot(up, card.normal) < 0):
        up = -up
    card.up = up
    if len(pts):
        rel = pts - card.base
        card.len = float(max(np.max(rel @ card.dir), np.linalg.norm(dvec), 1e-3))
        acr = rel @ card.up
        card.width = float(max(np.ptp(acr), 1e-3))
    else:
        card.len = float(np.linalg.norm(dvec))
        card.width = 0.0
    return card
