# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Tileable bark texture sets of the procedural shrubs (docs/dead-air/SHRUBS.md, tools/shrubs/make_shrubs.py).

One tile is 6 x 6 cm of bark: image x = u, round the stem; image y = v, along it (row 0 at the lower end). The
engine wraps a whole number of tiles round each stem (r3/da_shrub_wood_vs.h), so every feature keeps its real size.

  build/bark/<type>_d.png   RGB albedo (sRGB, measured bark levels), A cavity (1 open, ~0.3-0.5 in deep fissures)
  build/bark/<type>_n.png   RG tangent normal (0.5 + 0.5 n; x toward +u, y toward +v), B gloss, A lichen mask
  bark_manifest.json        the types in layer order and each species' layer (build_arrays.py, make_shrubs.py)
  build/bark_sheet.png      review: each type 2x2 flat (v up) and on lit stems near and at play distance
  build/bark_crops.png      review: full-resolution crops, lit flat, cavity / gloss / lichen

The relief is a height field in millimetres; the normals, the cavity and the lichen mask are derived from it. All
fields are periodic (FFT noise, Voronoi cells on a torus, stamps wrapped at the edges): the tiles join without seams.

usage: make_bark_textures.py [--only type,type] [--inspect DIR]
"""
import argparse
import json
import os
import sys
import time
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, 'build')
OUT = os.path.join(BUILD, 'bark')
MANIFEST = os.path.join(HERE, 'bark_manifest.json')

N = 512
METRES = 0.06
MM = N / (METRES * 1000.0)


def set_scale(scale):
    """The texels per mm of the types built next: a tree type is drawn at its real size over 6 cm x its scale (the
    species' bark_scale stretches the tile by as much in the engine)."""
    global MM
    MM = N / (METRES * scale * 1000.0)

_KY = (np.fft.fftfreq(N) * N)[:, None]
_KX = (np.fft.rfftfreq(N) * N)[None, :]
_YY, _XX = np.mgrid[0:N, 0:N].astype(np.float64)
PX, PY = _XX + 0.5, _YY + 0.5


# ======================================================================================================
# Periodic fields
# ======================================================================================================

def _gauss(sx, sy):
    return np.exp(-2.0 * np.pi ** 2 * ((sx * _KX / N) ** 2 + (sy * _KY / N) ** 2))


def blur(a, s, sy=None):
    """Gaussian blur on the torus; sigma in mm."""
    f = _gauss(s * MM, (s if sy is None else sy) * MM)
    if a.ndim == 3:
        return np.stack([np.fft.irfft2(np.fft.rfft2(a[..., c]) * f, s=(N, N)) for c in range(a.shape[2])], -1)
    return np.fft.irfft2(np.fft.rfft2(a) * f, s=(N, N))


def norm(a):
    a = a - a.mean()
    return a / (a.std() + 1e-12)


def nz(r, sx, sy=None):
    """Smooth periodic noise of unit deviation; sx, sy: the feature's sigma in mm across and along the stem."""
    return norm(blur(r.standard_normal((N, N)), sx, sy))


def fbm(r, sx, sy=None, octaves=4, gain=0.55):
    """Fractal periodic noise: octaves down from sigma (sx, sy) mm, each a gain weaker."""
    sy = sx if sy is None else sy
    F = np.fft.rfft2(r.standard_normal((N, N)))
    filt, a = 0.0, 1.0
    for i in range(octaves):
        g = _gauss(sx * MM / 2.0 ** i, sy * MM / 2.0 ** i)
        filt = filt + a * g / np.sqrt((g ** 2).sum())
        a *= gain
    return norm(np.fft.irfft2(F * filt, s=(N, N)))


def ss(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lines(n, w):
    """Thin creases along the zero set of a field (w: their width in the field's deviations)."""
    return np.exp(-(n / w) ** 2)


def grad_mm(a):
    gx = (np.roll(a, -1, 1) - np.roll(a, 1, 1)) * (MM / 2.0)
    gy = (np.roll(a, -1, 0) - np.roll(a, 1, 0)) * (MM / 2.0)
    return gx, gy


def level_set(n, cover):
    """A field cut at the level leaving `cover` of the tile above it, as a signed distance in mm (+ above)."""
    thr = np.quantile(n, 1.0 - cover)
    gx, gy = grad_mm(n)
    return (n - thr) / (np.sqrt(gx * gx + gy * gy) + 1e-3)


def cells(r, nx, ny, aspect=1.0, warp=None, jitter=1.0, R=2):
    """Voronoi cells of a jittered nx x ny grid on the torus; distances with v shrunk by `aspect` (cells that long).

    e: the true distance to the cell's border (mm), id: the cell, lx/ly: the offset from the cell's seed (mm)."""
    cw, ch = N / nx, N / ny
    gx = (np.arange(nx)[None, :] + 0.5 + jitter * (r.random((ny, nx)) - 0.5)) * cw
    gy = (np.arange(ny)[:, None] + 0.5 + jitter * (r.random((ny, nx)) - 0.5)) * ch
    X, Y = (PX, PY) if warp is None else (PX + warp[0], PY + warp[1])
    ci = np.floor(X / cw).astype(np.int64)
    cj = np.floor(Y / ch).astype(np.int64)
    b1 = np.full((N, N), np.inf)
    b2 = np.full((N, N), np.inf)
    i1 = np.zeros((N, N), np.int64)
    p1x, p1y, p2x, p2y = (np.zeros((N, N)) for _ in range(4))
    for dj in range(-R, R + 1):
        nj = cj + dj
        wj = nj % ny
        for di in range(-R, R + 1):
            ni = ci + di
            wi = ni % nx
            qx = gx[wj, wi] + (ni - wi) * cw
            qy = gy[wj, wi] + (nj - wj) * ch
            d = (X - qx) ** 2 + ((Y - qy) / aspect) ** 2
            c1 = d < b1
            c2 = ~c1 & (d < b2)
            b2 = np.where(c1, b1, np.where(c2, d, b2))
            p2x = np.where(c1, p1x, np.where(c2, qx, p2x))
            p2y = np.where(c1, p1y, np.where(c2, qy, p2y))
            b1 = np.where(c1, d, b1)
            p1x = np.where(c1, qx, p1x)
            p1y = np.where(c1, qy, p1y)
            i1 = np.where(c1, wj * nx + wi, i1)
    # Distance to the bisector in the shrunk metric, then mapped back to true distance.
    ax, ay = p2x - p1x, (p2y - p1y) / aspect
    ln = np.sqrt(ax * ax + ay * ay) + 1e-9
    ux, uy = ax / ln, ay / ln
    es = -((X - 0.5 * (p1x + p2x)) * ux + ((Y - 0.5 * (p1y + p2y)) / aspect) * uy)
    sn = np.sqrt(ux * ux + (uy / aspect) ** 2)
    e = es / sn
    return dict(e=e / MM, id=i1, n=nx * ny, lx=(X - p1x) / MM, ly=(Y - p1y) / MM, f1=np.sqrt(b1) / MM, bn=ux / sn)


def cattr(r, W, lo=0.0, hi=1.0):
    """A random value per cell."""
    return (lo + (hi - lo) * r.random(W['n']))[W['id']]


def warp_xy(r, amp_mm, sx, sy=None, amp_y=None):
    return (amp_mm * MM * nz(r, sx, sy), (amp_mm if amp_y is None else amp_y) * MM * nz(r, sx, sy))


def points(r, n, dx, dy=None, tries=40, weight=None):
    """Up to n points on the torus no closer than an ellipse of radii dx, dy mm (dart throwing).

    weight: an optional (N, N) acceptance probability field."""
    dy = dx if dy is None else dy
    cand = r.random((n * tries, 2)) * N
    if weight is not None:
        keep = r.random(len(cand)) < weight[cand[:, 1].astype(int), cand[:, 0].astype(int)]
        cand = cand[keep]
    acc = np.zeros((n, 2))
    k = 0
    rx2, ry2 = (dx * MM) ** 2, (dy * MM) ** 2
    for c in cand:
        if k >= n:
            break
        if k:
            d = np.abs(acc[:k] - c)
            d = np.minimum(d, N - d)
            if np.any(d[:, 0] ** 2 / rx2 + d[:, 1] ** 2 / ry2 < 1.0):
                continue
        acc[k] = c
        k += 1
    return acc[:k]


def splat(pts, ext, fn, chans):
    """Stamps a sprite per point, wrapped round the tile.

    ext(i) -> half extents (mm); fn(lx, ly, idx, i) -> dict of arrays with 'cov'. chans: {name: 'max' | 'add' |
    'over'}; 'over' takes the sprite's value where its coverage beats the canvas'."""
    out = {k: np.zeros((N, N)) for k in chans}
    out['cov'] = np.zeros((N, N))
    for i, (x, y) in enumerate(pts):
        hx, hy = ext(i)
        xs = np.arange(int(np.floor(x - hx * MM)), int(np.ceil(x + hx * MM)) + 1)
        ys = np.arange(int(np.floor(y - hy * MM)), int(np.ceil(y + hy * MM)) + 1)
        idx = np.ix_(ys % N, xs % N)
        s = fn(((xs + 0.5 - x) / MM)[None, :], ((ys + 0.5 - y) / MM)[:, None], idx, i)
        cov = np.broadcast_to(s['cov'], (len(ys), len(xs)))
        win = cov > out['cov'][idx]
        for k, mode in chans.items():
            if k not in s:
                continue
            if mode == 'max':
                out[k][idx] = np.maximum(out[k][idx], s[k])
            elif mode == 'add':
                out[k][idx] = out[k][idx] + s[k]
            else:
                out[k][idx] = np.where(win, s[k], out[k][idx])
        out['cov'][idx] = np.maximum(out['cov'][idx], cov)
    return out


# ======================================================================================================
# Colour
# ======================================================================================================

def C(h):
    h = h.lstrip('#')
    c = np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)]) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)


def from_srgb(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lum(c):
    return c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722


def mix(a, b, t):
    t = np.asarray(t, dtype=np.float64)
    if t.ndim == 2:
        t = t[..., None]
    return a * (1.0 - t) + b * t


def field(c1, c2, t):
    """A colour field between two colours."""
    return mix(np.broadcast_to(C(c1), (N, N, 3)), C(c2), t)


def vary(alb, amt, n):
    """Brightness variation (log-normal) by a unit noise."""
    return alb * np.exp(amt * n)[..., None]


def algae(r, alb, amount, where=1.0, col='#56643a', s=5.0):
    """Green algae (Desmococcus) tint in patches, humid Polesie."""
    a = amount * ss(0.0, 1.6, fbm(r, s, s * 1.4, 3)) * where
    g = C(col)
    tgt = g[None, None, :] * (lum(alb) / lum(g))[..., None] * 1.05
    return mix(alb, tgt, np.clip(a, 0, 1))


# ======================================================================================================
# Sprites
# ======================================================================================================

def lens_sprite(L, W, ang, J, RG, H, S, tone, taper=0.55, rough=0.25):
    """A lenticel: a lens L long (along its axis, transverse by default) and W wide, corky lips about a slit."""
    def fn(lx, ly, idx, i):
        c, s = np.cos(ang[i]), np.sin(ang[i])
        x, y = lx * c + ly * s, -lx * s + ly * c
        a, b = 0.5 * L[i], 0.5 * W[i]
        t = np.clip(1.0 - (x / a) ** 2, 0.0, 1.0)
        hw = b * t ** taper + 1e-4
        d = np.abs(y) / hw * (1.0 + 0.12 * J[idx])
        cov = ss(1.1, 0.8, d) * ss(0.0, 0.12, t)
        body = np.clip(1.0 - d * d, 0.0, 1.0) * np.sqrt(t)
        slit = np.exp(-(y / (0.25 * b + 0.03)) ** 2) * ss(0.15, 0.55, t) * cov
        hh = cov * H[i] * (body + rough * RG[idx]) - S[i] * slit
        halo = ss(2.4, 1.0, d) * ss(0.0, 0.1, t) * (1.0 - cov)
        return {'cov': cov, 'h': hh, 'slit': slit, 'tone': tone[i], 'halo': halo}
    return fn


def wart_sprite(A, B, H, J, RG, tone, crater=0.0, cleft=0.0):
    """A corky wart: a rough dome A x B (half axes, mm), optionally cratered or cleft along v."""
    def fn(lx, ly, idx, i):
        d = np.sqrt((lx / A[i]) ** 2 + (ly / B[i]) ** 2) * (1.0 + 0.1 * J[idx])
        cov = ss(1.08, 0.82, d)
        body = np.sqrt(np.clip(1.0 - d * d, 0.0, 1.0))
        hh = H[i] * body ** 0.7 * (1.0 + 0.25 * RG[idx])
        if crater:
            hh = hh - crater * H[i] * np.exp(-(d / 0.35) ** 2)
        cl = 0.0
        if cleft:
            cl = np.exp(-(lx / (0.16 * A[i] + 0.03)) ** 2) * ss(0.95, 0.4, np.abs(ly) / B[i]) * cov
            hh = hh - cleft * H[i] * cl
        return {'cov': cov, 'h': hh * cov, 'tone': tone[i], 'cleft': cl, 'top': body * cov}
    return fn


def prickle_sprite(A, B, H, off, gam, J):
    """A prickle: a cone on an elliptic base (A across, B along v, half axes mm) whose tip leans toward the stem's
    foot (-v) by `off` of the base; gam > 1 hollow flanks."""
    def fn(lx, ly, idx, i):
        a, b = A[i], B[i]
        ay = -off[i] * b
        qx, qy = lx, ly - ay
        qa = (qx / a) ** 2 + (qy / b) ** 2
        qb = 2.0 * (ay * qy / b ** 2)
        qc = (ay / b) ** 2 - 1.0
        t = (-qb + np.sqrt(np.maximum(qb * qb - 4.0 * qa * qc, 0.0))) / (2.0 * qa + 1e-12)
        rho = 1.0 / np.maximum(t, 1e-6)
        cov = ss(1.05, 0.88, rho)
        hh = H[i] * np.clip(1.0 - rho, 0.0, 1.0) ** gam[i]
        tip = ss(0.45, 0.85, 1.0 - rho) * cov
        return {'cov': cov, 'h': hh, 'tip': tip}
    return fn


def film(r, cover, sx, sy, jag=0.45):
    """A thin outer skin torn into patches elongated sx x sy (mm): coverage and signed distance to the torn edge."""
    n = norm(fbm(r, sx, sy, 3) + jag * fbm(r, sx * 0.22, sy * 0.3, 3))
    sd = level_set(n, cover)
    return ss(-0.05, 0.05, sd), sd


# ======================================================================================================
# Bark building blocks
# ======================================================================================================

def smooth_relief(r, macro=0.03, striae=0.005, wrinkles=0.004, s_w=0.09):
    """Smooth bark: a gentle swell, longitudinal stretch striae and fine transverse wrinkles."""
    h = macro * fbm(r, 5.0, 8.0, 3)
    h = h + striae * lines(nz(r, 0.12, 1.8), 0.45)
    h = h + wrinkles * lines(nz(r, 2.0, s_w), 0.35)
    return h


def bark_base(r, c1, c2, sx=2.5, sy=14.0, streak=0.06, grain=0.05):
    """A bark colour between two tones in long streaks along the stem, with streaky and fine grain."""
    alb = field(c1, c2, ss(-1.3, 1.3, fbm(r, sx, sy, 3)))
    alb = vary(alb, streak, fbm(r, 0.35, 4.0, 3))
    return vary(alb, grain, fbm(r, 0.12, 0.45, 2))


def zero_lines(r, sx, sy, octaves=2, gain=0.35, along=False):
    """The distance (mm) to the zero set of an anisotropic field: long lines along the stem that split and merge.

    The gradient is floored, so the near-flat crossings stay lines instead of swelling into blobs. along: also how
    nearly the line runs along the stem there (1) rather than across it (0)."""
    n = fbm(r, sx, sy, octaves, gain)
    gx, gy = grad_mm(n)
    g = np.sqrt(gx * gx + gy * gy)
    d = np.abs(n) / np.maximum(g, 0.5 * np.median(g))
    return (d, np.abs(gx) / (g + 1e-9)) if along else d


def fissure_net(r, sx, sy, width, depth, closing=0.4, rag=0.2, octaves=2):
    """Longitudinal fissures on the zero lines, opening and closing along their length.

    Returns the relief (0 on the ridges, -depth in an open fissure), t (0 in the fissure .. 1 off it), the distance and
    the openness."""
    d, al = zero_lines(r, sx, sy, octaves, along=True)
    d = d * (1.0 + rag * nz(r, 0.15, 0.5))
    # A closed contour's turn across the stem is no fissure: the loops break into separate tapering cracks.
    op = ss(-closing, 0.9, nz(r, sx * 1.5, sy * 0.9)) * ss(0.6, 0.95, blur(al, 0.15))
    w = width * (0.12 + 0.88 * op)
    t = np.clip(d / w, 0.0, 1.0)
    hh = -depth * (0.15 + 0.85 * op) * (1.0 - ss(0.0, 1.0, t))
    return hh, t, d, op


def cross_cracks(r, sx, sy, width, share):
    """Short transverse cracks: zero lines of a field long across the stem, kept on `share` of their length."""
    c = zero_lines(r, sx, sy, 1)
    m = ss(1.0 - 2.0 * share, 1.2 - 2.0 * share, nz(r, sx * 0.7, sy * 3.0) * 0.5 + 0.5)
    return np.exp(-(c / width) ** 2) * m


def periderm(r, scale=1.0, aspect=2.0):
    """Fine bark grain: tiny periderm scales (cells of 0.7 x 1.4 mm by default, each a little raised or sunk, hairline
    borders) and fibres, along the stem (aspect > 1) or round it (aspect < 1: the birch and cherry kin's transverse
    periderm). Returns a relief of unit amplitude and a brightness field of unit deviation."""
    nx = int(round(60.0 / (0.7 * scale)))
    ny = max(1, int(round(60.0 / (0.7 * scale * aspect))))
    W = cells(r, nx, ny, aspect=aspect, warp=warp_xy(r, 0.25 * scale, 0.4 * scale, 1.0 * scale), R=1)
    lv = cattr(r, W, -1.0, 1.0)
    edge = ss(0.0, 0.07 * scale, W['e'])
    fib = lines(nz(r, 0.1, 1.2 * scale) if aspect >= 1.0 else nz(r, 1.2 * scale, 0.1), 0.4)
    hh = 0.7 * lv * edge + 0.25 * (edge - 1.0) + 0.4 * fib
    return hh, norm(0.8 * lv + 0.5 * edge + 0.3 * fib)


def specks(r, alb, n, size, col, amt=0.8):
    """Sparse tiny dark dots (fungal fruit bodies, dirt) on old bark."""
    pts = points(r, n, size * 3.0, size * 3.0)
    k = len(pts)
    A = size * r.uniform(0.5, 1.2, k)
    S = splat(pts, lambda i: (A[i] * 1.5 + 0.05, A[i] * 1.5 + 0.05),
              wart_sprite(A, A * r.uniform(0.8, 1.4, k), np.full(k, 0.02), nz(r, 0.1), nz(r, 0.05), r.random(k)),
              {'tone': 'over'})
    return mix(alb, C(col), amt * S['cov'])


def lenticel_layer(r, n, sep, L_mean, L_sd, W_ratio, H, S, ang_sd=0.05, weight=None, Lmin=0.8, Lmax=6.0, Wmin=0.25,
                   Wmax=1.0, vertical=False, rough=0.25, pts=None, taper=0.55):
    if pts is None:
        pts = points(r, n, sep[0], sep[1], weight=weight)
    k = len(pts)
    Lr = np.clip(r.lognormal(np.log(L_mean), L_sd, k), Lmin, Lmax)
    Wr = np.clip(Lr * r.uniform(W_ratio[0], W_ratio[1], k), Wmin, Wmax)
    ang = r.normal(np.pi / 2 if vertical else 0.0, ang_sd, k)
    J, RG = nz(r, 0.16), nz(r, 0.07)
    Hs = H * r.uniform(0.6, 1.3, k)
    Ss = S * r.uniform(0.5, 1.4, k)
    ext = (lambda i: (1.3 * Wr[i] + 0.25, 0.6 * Lr[i] + 0.2)) if vertical else \
        (lambda i: (0.6 * Lr[i] + 0.2, 1.3 * Wr[i] + 0.25))
    return splat(pts, ext, lens_sprite(Lr, Wr, ang, J, RG, Hs, Ss, r.random(k), rough=rough, taper=taper),
                 {'h': 'add', 'slit': 'max', 'tone': 'over', 'halo': 'max'})


def paint_lenticels(alb, L, pale, pale2, slit, slit_amt=0.55, cover=0.92, halo=None, halo_amt=0.0):
    lc = mix(np.broadcast_to(C(pale), (N, N, 3)), C(pale2), L['tone'])
    if halo is not None:
        alb = mix(alb, C(halo), halo_amt * L['halo'])
    alb = mix(alb, lc, cover * L['cov'])
    return mix(alb, C(slit), slit_amt * L['slit'])


def peel(r, res, m, sd, lift_h, curl, rim_col, rim_amt=0.4, shade_amt=0.3, under=None, under_gloss=None):
    """A torn outer skin m over the bark beneath: its edge lifts and catches the light, the bark under it shaded."""
    lift = ss(0.0, 0.04, sd) * np.exp(-np.clip(sd, 0, None) / curl) * ss(-0.6, 1.0, nz(r, 2.0, 1.0))
    res['h'] = res['h'] + lift_h * lift
    shadow = np.exp(np.clip(sd, None, 0.0) / 0.14) * (1.0 - m)
    rim = np.exp(-np.abs(sd) / 0.05) * m
    res['alb'] = mix(res['alb'], C(rim_col), rim_amt * rim) * (1.0 - shade_amt * shadow)[..., None]
    res['ao_mul'] = res.get('ao_mul', 1.0) * (1.0 - 0.35 * shadow)
    return lift


def add_prickles(r, res, n, sep, A, B, H, off, base, tip, tip_mix, gloss_p, gam=(1.3, 1.8), base_mix=0.7):
    pts = points(r, n, sep[0], sep[1])
    k = len(pts)
    t = r.random(k)
    Aa = A[0] + (A[1] - A[0]) * t
    Bb = B[0] + (B[1] - B[0]) * np.clip(0.7 * t + 0.3 * r.random(k), 0.0, 1.0)
    Hh = r.uniform(H[0], H[1], k)
    P = splat(pts, lambda i: (Aa[i] * 1.15 + 0.1, Bb[i] * 1.15 + 0.1),
              prickle_sprite(Aa, Bb, Hh, r.uniform(off[0], off[1], k), r.uniform(gam[0], gam[1], k), nz(r, 0.15)),
              {'h': 'max', 'tip': 'max'})
    res['h'] = res['h'] + P['h']
    alb = mix(res['alb'], C(base), base_mix * P['cov'])
    res['alb'] = mix(alb, C(tip), tip_mix * P['tip'])
    res['gloss'] = res['gloss'] * (1.0 - P['cov']) + gloss_p * P['cov']
    res['fresh'] = np.maximum(res.get('fresh', 0.0), P['cov'])
    return P


def shreds(r, layers, sx, sy, cover, thick, cols, under_dark, fib_h=0.03, lift_p=0.4, jag=0.5):
    """Bark shredding in long fibrous strips: stacked layers of strands (level sets of fields very long along the
    stem, splitting, merging and tapering) over a fibrous under-bark, the outer layers weathered. Returns the relief,
    the albedo and the (soft) index of the top layer, 0 = the under-bark."""
    fibres = lines(nz(r, 0.09, 2.5), 0.4) + 0.6 * lines(nz(r, 0.16, 4.0), 0.4)
    h = 0.02 * fbm(r, 0.4, 3.0, 3)
    alb = vary(field(cols[0][0], cols[0][1], ss(-1, 1, fbm(r, 0.6, 8.0, 3))), 0.08, fbm(r, 0.12, 1.5, 3))
    top = np.zeros((N, N))
    shade = np.zeros((N, N))
    for k in range(1, layers + 1):
        n = norm(fbm(r, sx, sy, 3, 0.5) + jag * fbm(r, sx * 0.25, sy * 0.2, 2))
        sd = level_set(n, cover)
        cov = ss(-0.05, 0.05, sd)
        # The strand's edge lifts here and there, the layer under it lies in its shadow.
        lift = 0.18 * np.exp(-np.clip(sd, 0.0, None) / 0.12) * ss(1.0 - 2.0 * lift_p, 1.0, nz(r, 0.8, 4.0))
        hk = thick * k + lift - 0.06 * ss(0.15, 0.0, sd)
        shade = np.maximum(shade * (1.0 - cov), blur(cov, 0.2) * (1.0 - cov))
        h = mix(h[..., None], np.maximum(h, hk)[..., None], cov)[..., 0]
        c = vary(field(cols[k][0], cols[k][1], ss(-1, 1, fbm(r, 0.6, 8.0, 3))), 0.08, fbm(r, 0.12, 1.5, 3))
        alb = mix(alb, c, cov)
        top = top * (1.0 - cov) + k * cov
    h = h + fib_h * fibres + 0.03 * fbm(r, 4, 8, 3)
    alb = vary(alb, 0.07, fibres - 0.5)
    alb = mix(alb, C(under_dark), 0.55 * ss(0.0, 0.6, shade))
    return h, alb, top


# ======================================================================================================
# Bark types
# ======================================================================================================

def grain(r, res, h_amt, c_amt, scale=1.0, aspect=2.0):
    """Adds the fine periderm grain to a type's relief and colour."""
    mh, mc = periderm(r, scale, aspect)
    res['h'] = res['h'] + h_amt * mh
    res['alb'] = vary(res['alb'], c_amt, mc)
    return res


def t_hazel(r):
    """Corylus avellana: smooth glossy grey-brown bark with a faint coppery cast, silvery in streaks, fine transverse
    wrinkles; short transverse lenticels, pale with dark lips; the thin outer skin peels here and there in papery
    transverse strips."""
    h = smooth_relief(r, 0.03, 0.0015, 0.009, s_w=0.08)
    alb = bark_base(r, '#6a5c50', '#6e5c4e', sx=4.0, sy=5.0, streak=0.03)
    alb = mix(alb, C('#76695e'), 0.35 * ss(0.0, 1.5, fbm(r, 3.0, 2.5, 3)))
    alb = vary(alb, 0.05, lines(nz(r, 2.2, 0.1), 0.4) - 0.3)
    m, sd = film(r, 0.9, 6.0, 0.45, jag=0.55)
    under = vary(field('#6c5648', '#6e5646', 0.5), 0.04, fbm(r, 0.8, 0.2, 3))
    alb = mix(under, alb, m)
    res = dict(h=h + 0.012 * m, alb=alb, gloss=0.58 * m + 0.66 * (1.0 - m), susc=0.6, fresh=1.0 - m)
    peel(r, res, m, sd, 0.05, 0.25, '#8a8076', 0.3, 0.2)
    L = lenticel_layer(r, 150, (1.8, 1.2), 1.1, 0.4, (0.2, 0.32), 0.04, 0.03, Lmax=2.8, Lmin=0.5, Wmin=0.2,
                       Wmax=0.6)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(res['alb'], L, '#a69684', '#8c7c6a', '#3a2e26', slit_amt=0.7, halo='#4a4038',
                                 halo_amt=0.3, cover=0.9)
    res['gloss'] = res['gloss'] - 0.35 * L['cov']
    grain(r, res, 0.006, 0.04, aspect=0.5)
    res['alb'] = algae(r, res['alb'], 0.1, where=m)
    return res


def t_alder_buckthorn(r):
    """Frangula alnus: dark purplish grey-brown smooth bark, densely set with small whitish transverse lenticels."""
    h = smooth_relief(r, 0.03, 0.005, 0.005)
    alb = bark_base(r, '#3e3634', '#4a3d38', sx=3.0, sy=5.0, streak=0.025)
    alb = mix(alb, C('#5c5652'), 0.3 * ss(0.2, 1.6, fbm(r, 2.0, 4.0, 3)))
    res = grain(r, dict(h=h, alb=alb), 0.005, 0.05, aspect=0.5)
    L = lenticel_layer(r, 420, (1.25, 0.7), 1.2, 0.5, (0.18, 0.4), 0.05, 0.04, Lmax=4.0, Lmin=0.4, Wmin=0.2,
                       Wmax=0.6)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(algae(r, res['alb'], 0.15), L, '#aaa59a', '#8c857a', '#5e4c3e',
                                 slit_amt=0.6)
    res.update(gloss=0.55 + 0.05 * nz(r, 3.0) - 0.4 * L['cov'], susc=0.5)
    return res


def t_bird_cherry(r):
    """Padus avium (with amorpha, guelder rose, serviceberry): dark grey-brown smooth bark, faintly striped, many small
    pale transverse lenticels."""
    stripe = fbm(r, 0.45, 7.0, 3)
    h = smooth_relief(r, 0.03, 0.006, 0.006) + 0.008 * stripe
    alb = vary(bark_base(r, '#463f3b', '#52473f', sx=3.0, sy=6.0, streak=0.03), 0.03, stripe)
    alb = mix(alb, C('#66605a'), 0.28 * ss(0.2, 1.6, fbm(r, 1.2, 6.0, 3)))
    res = grain(r, dict(h=h, alb=algae(r, alb, 0.12)), 0.005, 0.05, aspect=0.6)
    L = lenticel_layer(r, 320, (1.4, 0.75), 1.3, 0.35, (0.22, 0.34), 0.05, 0.03, Lmax=2.8, Lmin=0.5, Wmin=0.22,
                       Wmax=0.6)
    res['h'] = res['h'] + L['h']
    res['alb'] = specks(r, paint_lenticels(res['alb'], L, '#9e917c', '#86796a', '#4e4036', slit_amt=0.5),
                        25, 0.15, '#1e1a18')
    res.update(gloss=0.62 + 0.05 * nz(r, 3.0) - 0.45 * L['cov'], susc=0.45)
    return res


def t_cherry(r):
    """Prunus cerasus: glossy dull purplish-brown bark ringed by transverse lenticel bands; the old outer skin peels in
    curling transverse strips."""
    h = smooth_relief(r, 0.035, 0.003, 0.009, s_w=0.08)
    alb = bark_base(r, '#544240', '#5c4442', sx=4.0, sy=10.0)
    alb = vary(alb, 0.07, lines(nz(r, 2.5, 0.12), 0.5) - 0.3)
    alb = mix(alb, C('#5e5450'), 0.3 * ss(0.3, 1.6, fbm(r, 4.0, 3.0, 3)))
    res = grain(r, dict(h=h, alb=alb), 0.004, 0.04, aspect=0.5)
    rows = 13
    ys = (np.arange(rows) + r.uniform(-0.3, 0.3, rows)) * N / rows
    pts = []
    for y0 in ys:
        x = r.uniform(0, N)
        end = x + N
        while x < end - 4.0 * MM:
            pts.append((x % N, (y0 + r.normal(0, 0.25) * MM) % N))
            x += r.uniform(5.0, 11.0) * MM
    L = lenticel_layer(r, 0, None, 4.0, 0.35, (0.1, 0.16), 0.06, 0.05, Lmin=1.8, Lmax=8.0, Wmin=0.35, Wmax=0.9,
                       ang_sd=0.03, pts=np.array(pts))
    res['h'] = res['h'] + L['h']
    alb = paint_lenticels(res['alb'], L, '#94806e', '#7e6a5c', '#3e2c28', slit_amt=0.6)
    m, sd = film(r, 0.14, 5.0, 0.6, jag=0.5)
    skin = vary(field('#625654', '#58504a', ss(-1, 1, fbm(r, 2, 4, 3))), 0.06, fbm(r, 0.6, 0.15, 3))
    res.update(h=res['h'] + 0.025 * m, alb=mix(alb, skin, m), gloss=0.75 * (1.0 - m) + 0.25 * m - 0.45 * L['cov'],
               susc=0.2)
    peel(r, res, m, sd, 0.14, 0.35, '#8a7e76', 0.35, 0.25)
    return res


def t_willow_grey(r):
    """Salix cinerea (with the osier and violet willow): grey bark with an olive cast, shallow longitudinal fissures
    on the older stems as soft grooves, fibrous between them."""
    hf, t, d, op = fissure_net(r, 0.6, 6.5, 0.55, 0.3, closing=0.2)
    h = hf + 0.012 * lines(nz(r, 0.12, 2.2), 0.4) + 0.008 * fbm(r, 0.4, 1.4, 3) + 0.03 * fbm(r, 5, 8, 3)
    cr = cross_cracks(r, 1.2, 0.25, 0.1, 0.3) * t
    h = h - 0.025 * cr
    alb = bark_base(r, '#67645c', '#5b5750', sx=2.0, sy=10.0)
    alb = mix(alb, C('#6a6652'), 0.35 * ss(-0.5, 1.5, fbm(r, 1.5, 8.0, 3)))
    groove = blur(1.0 - t, 0.12)
    alb = mix(alb, C('#4c453c'), 0.55 * groove * (0.4 + 0.6 * op))
    alb = mix(alb, C('#76726a'), 0.25 * ss(0.15, 0.0, groove) * ss(0.0, 0.5, blur(1.0 - t, 0.6)))
    res = grain(r, dict(h=h, alb=alb), 0.007, 0.05, scale=0.8, aspect=4.0)
    res['alb'] = algae(r, res['alb'], 0.3, where=0.4 + 0.6 * ss(0.0, 0.3, blur(1.0 - t, 0.5)))
    res['alb'] = specks(r, res['alb'], 40, 0.14, '#24201c')
    res.update(gloss=0.28 - 0.15 * (1.0 - t) + 0.04 * nz(r, 3.0), susc=0.75)
    return res


def diamond_sprite(A, B, J, RG, tone):
    def fn(lx, ly, idx, i):
        d = (np.abs(lx) / A[i] + np.abs(ly) / B[i]) * (1.0 + 0.06 * J[idx])
        cov = ss(1.1, 0.88, d)
        centre = ss(0.62, 0.25, d)
        rim = np.exp(-((d - 0.78) / 0.2) ** 2) * cov
        crack = np.exp(-(ly / (0.08 * B[i] + 0.03)) ** 2) * ss(0.7, 0.2, np.abs(lx) / A[i]) * cov
        hh = 0.07 * rim + cov * 0.05 * RG[idx] - 0.12 * centre - 0.12 * crack
        return {'cov': cov, 'h': hh, 'centre': np.maximum(centre, crack), 'rim': rim, 'tone': tone[i]}
    return fn


def t_willow_goat(r):
    """Salix caprea: smooth olive grey-green bark with dark diamond-shaped lenticels in transverse rows, a few shallow
    fissures setting in."""
    h = smooth_relief(r, 0.03, 0.006, 0.005)
    hf, t, d, op = fissure_net(r, 0.9, 7.0, 0.22, 0.06, closing=0.5)
    h = h + hf
    rows = 11
    ys = (np.arange(rows) + r.uniform(-0.35, 0.35, rows)) * N / rows
    pts = []
    for y0 in ys:
        x = r.uniform(0, N)
        end = x + N
        while x < end - 3.0 * MM:
            if r.random() < 0.75:
                pts.append((x % N, (y0 + r.normal(0, 0.4) * MM) % N))
            x += r.uniform(3.0, 8.0) * MM
    pts = np.array(pts)
    k = len(pts)
    A = np.clip(r.lognormal(np.log(1.1), 0.35, k), 0.5, 2.4)
    B = A * r.uniform(0.4, 0.65, k)
    D = splat(pts, lambda i: (A[i] * 1.2 + 0.2, B[i] * 1.2 + 0.2),
              diamond_sprite(A, B, nz(r, 0.12), nz(r, 0.07), r.random(k)),
              {'h': 'add', 'centre': 'max', 'rim': 'max', 'tone': 'over'})
    h = h + D['h']
    alb = bark_base(r, '#727060', '#6c6a58', sx=3.0, sy=12.0)
    alb = mix(alb, C('#76715c'), 0.3 * ss(-0.5, 1.5, fbm(r, 1.0, 8.0, 3)))
    alb = mix(alb, C('#58524a'), 0.5 * blur(1.0 - t, 0.12))
    res = grain(r, dict(h=h, alb=algae(r, alb, 0.12)), 0.006, 0.05)
    dc = mix(np.broadcast_to(C('#5e5444'), (N, N, 3)), C('#4e4438'), D['tone'])
    alb = mix(res['alb'], dc, 0.8 * D['cov'])
    alb = mix(alb, C('#2e2620'), 0.8 * D['centre'])
    res.update(alb=specks(r, alb, 30, 0.14, '#24201c'), gloss=0.35 - 0.2 * D['cov'] - 0.1 * (1.0 - t), susc=0.65)
    return res


def t_willow_smooth(r):
    """Salix aurita, rosmarinifolia, pentandra, triandra: smooth glossy olive- to red-brown bark, fine striae, a few
    lenticels; the thin outer layer flakes off in plates (almond willow)."""
    h = smooth_relief(r, 0.03, 0.009, 0.003) + 0.012 * fbm(r, 0.35, 5.0, 3)
    alb = bark_base(r, '#5c4e42', '#5e4a42', sx=2.0, sy=14.0, streak=0.08)
    alb = mix(alb, C('#5c5644'), 0.4 * ss(-0.5, 1.5, fbm(r, 0.8, 12.0, 3)))
    m, sd = film(r, 0.9, 1.2, 4.0, jag=0.6)
    under = vary(field('#746452', '#6e5e4c', 0.5), 0.05, fbm(r, 0.15, 1.5, 3))
    alb = mix(under, alb, m)
    L = lenticel_layer(r, 60, (3.0, 2.0), 1.0, 0.3, (0.3, 0.5), 0.04, 0.02, Lmax=2.0)
    h = h + 0.02 * m + L['h']
    alb = paint_lenticels(alb, L, '#8a7c66', '#7a6c58', '#4a3c30', slit_amt=0.4, cover=0.75)
    res = dict(h=h, alb=alb, gloss=0.5 * m + 0.4 * (1.0 - m) - 0.3 * L['cov'], susc=0.4, fresh=1.0 - m)
    peel(r, res, m, sd, 0.07, 0.25, '#867a6a', 0.3, 0.25)
    grain(r, res, 0.005, 0.05, aspect=3.0)
    res['alb'] = algae(r, res['alb'], 0.12)
    return res


def t_spindle(r):
    """Euonymus europaeus: olive-green to grey-brown bark with longitudinal corky lines, finely fissured when older."""
    h = smooth_relief(r, 0.03, 0.006, 0.003)
    hf, t, d, op = fissure_net(r, 0.5, 5.0, 0.3, 0.1, closing=0.0)
    L = lenticel_layer(r, 30, (3.8, 10.0), 13.0, 0.5, (0.04, 0.07), 0.07, 0.03, Lmin=5.0, Lmax=30.0, Wmin=0.4,
                       Wmax=1.1, vertical=True, ang_sd=0.03, rough=0.15, taper=0.5)
    cork_cracks = lines(nz(r, 0.5, 0.09), 0.3) * ss(-0.3, 0.8, nz(r, 0.3, 0.6)) * L['cov']
    h = h + hf + L['h'] - 0.07 * cork_cracks
    alb = bark_base(r, '#575c41', '#5e5d47', sx=2.0, sy=12.0)
    alb = mix(alb, C('#66625a'), 0.4 * ss(-0.2, 1.5, fbm(r, 1.5, 10.0, 3)))
    alb = mix(alb, C('#4a4836'), 0.4 * blur(1.0 - t, 0.12))
    res = grain(r, dict(h=h, alb=alb), 0.006, 0.05)
    lc = vary(mix(np.broadcast_to(C('#786a58'), (N, N, 3)), C('#665a4a'), L['tone']), 0.12, fbm(r, 0.12, 0.3, 2))
    alb = mix(res['alb'], lc, 0.85 * L['cov'])
    alb = mix(alb, C('#54483a'), 0.5 * cork_cracks)
    res.update(alb=algae(r, alb, 0.12), gloss=0.38 - 0.28 * L['cov'] - 0.08 * (1.0 - t), susc=0.3)
    return res


def t_warty_spindle(r):
    """Euonymus verrucosus: brown-green bark densely set with corky warts, buff on top and brown to blackish-brown."""
    h = smooth_relief(r, 0.03, 0.006, 0.003)
    alb = bark_base(r, '#5a604a', '#5e5c46', sx=2.0, sy=10.0)
    alb = mix(alb, C('#625a4c'), 0.3 * ss(0.0, 1.5, fbm(r, 1.2, 8.0, 3)))
    res = grain(r, dict(h=h, alb=alb), 0.005, 0.05)
    pts = points(r, 650, 1.3, 1.4)
    k = len(pts)
    A = np.clip(r.lognormal(np.log(0.42), 0.3, k), 0.2, 0.9)
    B = A * r.uniform(0.9, 1.4, k)
    Hh = 0.15 + 0.5 * A * r.uniform(0.6, 1.2, k)
    Wt = splat(pts, lambda i: (A[i] * 1.3 + 0.1, B[i] * 1.3 + 0.1),
               wart_sprite(A, B, Hh, nz(r, 0.1), nz(r, 0.05), r.random(k), crater=0.2),
               {'h': 'max', 'tone': 'over', 'top': 'max'})
    dark = mix(np.broadcast_to(C('#3e3228'), (N, N, 3)), C('#4e3e30'), Wt['tone'])
    light = mix(np.broadcast_to(C('#7a6a56'), (N, N, 3)), C('#94846a'), Wt['tone'])
    wc = vary(mix(dark, light, ss(0.2, 0.9, Wt['top']) * ss(0.25, 0.75, Wt['tone'])), 0.15, nz(r, 0.06))
    res.update(h=res['h'] + Wt['h'], alb=mix(res['alb'], wc, Wt['cov']), gloss=0.35 - 0.25 * Wt['cov'], susc=0.3)
    return res


def t_hawthorn(r):
    """Crataegus monogyna (with lilac and sea buckthorn): grey-brown bark broken into small oblong scales, bevelled and
    tilted, some lifting at the edge, the orange-brown under-bark showing between them and where scales fell."""
    W = cells(r, 20, 7, aspect=3.0, warp=(0.7 * MM * nz(r, 1.2, 5.0), 1.1 * MM * nz(r, 2.5, 2.5)))
    e = W['e'] + 0.14 * fbm(r, 0.2, 0.5, 3)
    shut = ss(-0.4, 0.6, fbm(r, 0.8, 2.0, 3))
    g = (0.05 + 0.3 * cattr(r, W)) * (0.2 + 0.8 * W['bn'] ** 2) * shut
    plate = ss(g, g + 0.25, e) * (1.0 - 0.6 * np.exp(-(e / 0.05) ** 2))
    gone = (cattr(r, W) < 0.14).astype(np.float64)
    present = plate * (1.0 - gone)
    tilt = cattr(r, W, -0.05, 0.05) * W['lx'] + cattr(r, W, -0.025, 0.025) * W['ly']
    curl = (cattr(r, W) < 0.35) * ss(1.0, 0.0, e - g)
    under_h = 0.02 * lines(nz(r, 0.1, 1.5), 0.4) + 0.015 * fbm(r, 0.3, 1.0, 3)
    top_h = 0.3 + cattr(r, W, 0.0, 0.12) + tilt + 0.08 * np.sqrt(np.clip((e - g) / 1.5, 0.0, 1.0)) + 0.12 * curl
    h = under_h + present * top_h + 0.03 * fbm(r, 4, 6, 3)
    hair = cross_cracks(r, 0.8, 0.25, 0.1, 0.25) * present
    h = h - 0.05 * hair
    tone = cattr(r, W)
    alb = vary(field('#625a52', '#6c625a', tone), 0.06, fbm(r, 0.25, 0.6, 3))
    alb = mix(alb, C('#7a746c'), 0.3 * ss(0.2, 1.5, fbm(r, 1.0, 2.0, 3)))
    under = vary(field('#7a6450', '#6e5646', ss(-1, 1, fbm(r, 1.0, 3.0, 3))), 0.07, fbm(r, 0.12, 0.8, 3))
    under = mix(under, C('#5a4a3e'), 0.35 * (1.0 - plate) * (1.0 - gone))
    alb = mix(under, alb, present)
    res = grain(r, dict(h=h, alb=alb), 0.012, 0.06, scale=0.9, aspect=1.8)
    res['ao_mul'] = 1.0 - 0.3 * ss(0.5, 0.0, e - g) * (1.0 - present) * (1.0 - gone)
    res.update(gloss=0.15 + 0.1 * gone * plate - 0.05 * (1.0 - plate), susc=0.75, fresh=1.0 - present)
    res['alb'] = algae(r, res['alb'], 0.22, where=present)
    res['alb'] = specks(r, res['alb'], 35, 0.15, '#201a16')
    return res


def t_shreddy(r):
    """Lonicera xylosteum, Philadelphus coronarius: grey-brown bark peeling in long fibrous strips off a brown
    under-bark."""
    cols = [('#5a483c', '#544238'), ('#62564a', '#5c5044'), ('#6a5e52', '#645a50'), ('#726a62', '#6a6058')]
    h, alb, top = shreds(r, 3, 1.0, 10.0, 0.55, 0.12, cols, '#2e241c')
    res = grain(r, dict(h=h, alb=alb), 0.004, 0.04, scale=0.8, aspect=4.0)
    res.update(alb=algae(r, res['alb'], 0.1, where=ss(1.5, 2.5, top)), gloss=0.2 - 0.06 * ss(0.0, 1.0, top), susc=0.55,
               fresh=1.0 - ss(1.5, 3.0, top))
    return res


def t_juniper(r):
    """Juniperus communis: reddish-brown bark shredding in long thin strips, weathered grey-brown on the outside."""
    cols = [('#5a3c30', '#56382c'), ('#664a3e', '#604438'), ('#6a564a', '#665044'), ('#706460', '#6a5e56')]
    h, alb, top = shreds(r, 3, 0.55, 14.0, 0.5, 0.1, cols, '#2a1a14', fib_h=0.04)
    res = grain(r, dict(h=h, alb=alb), 0.004, 0.04, scale=0.6, aspect=5.0)
    res.update(gloss=0.17 - 0.05 * ss(0.0, 1.0, top), susc=0.45, fresh=1.0 - ss(1.5, 3.0, top))
    return res


def t_currant(r, prickles=False):
    """Ribes nigrum: brown-grey bark whose thin outer skin peels in papery flakes off a reddish-brown under-bark."""
    h = smooth_relief(r, 0.03, 0.007, 0.003)
    m, sd = film(r, 0.78 if prickles else 0.72, 0.8 if prickles else 1.0, 3.0 if prickles else 4.0, jag=0.6)
    h = h + 0.025 * m + 0.01 * m * lines(nz(r, 0.12, 1.5), 0.4)
    under = vary(field('#5e4a42', '#644c40', ss(-1, 1, fbm(r, 2, 4, 3))), 0.06, fbm(r, 0.15, 1.2, 3))
    skin = bark_base(r, '#686054', '#5c5448', sx=1.5, sy=8.0) if prickles else \
        bark_base(r, '#5a4e46', '#52463e', sx=1.5, sy=8.0)
    res = dict(h=h, alb=mix(under, skin, m), gloss=0.28 * m + 0.35 * (1.0 - m), susc=0.3, fresh=1.0 - m)
    peel(r, res, m, sd, 0.1, 0.3, '#857a70', 0.35, 0.3)
    grain(r, res, 0.005, 0.05, scale=0.8, aspect=3.0)
    if prickles:
        add_prickles(r, res, n=9, sep=(9.0, 9.0), A=(0.3, 0.5), B=(0.6, 1.1), H=(0.7, 1.1), off=(0.1, 0.35),
                     base='#6a5c4a', tip='#a49274', tip_mix=0.8, gloss_p=0.45, gam=(2.0, 2.6))
        add_prickles(r, res, n=40, sep=(3.0, 3.0), A=(0.12, 0.2), B=(0.15, 0.25), H=(0.2, 0.35), off=(0.0, 0.1),
                     base='#6a5c4a', tip='#968666', tip_mix=0.6, gloss_p=0.3)
    return res


def t_gooseberry(r):
    """Ribes uva-crispa: grey-brown flaking bark with small pale prickles (the texture's only)."""
    return t_currant(r, prickles=True)


def t_rose(r):
    """Rosa canina, R. majalis: smooth green to reddish-brown canes, grey-brown where old, scattered dark-tipped
    prickles hooked toward the foot."""
    h = smooth_relief(r, 0.03, 0.008, 0.002)
    t = 0.35 + 0.3 * ss(-1.2, 1.2, fbm(r, 1.5, 15.0, 3))
    alb = mix(np.broadcast_to(C('#555a44'), (N, N, 3)), C('#604a40'), t)
    alb = vary(alb, 0.06, fbm(r, 0.3, 5.0, 3))
    old = ss(0.8, 2.0, fbm(r, 0.8, 12.0, 3))
    alb = mix(alb, C('#66605a'), 0.4 * old)
    res = grain(r, dict(h=h + 0.008 * old * fbm(r, 0.3, 0.5, 3), alb=alb), 0.004, 0.04, aspect=3.0)
    res.update(gloss=0.5 - 0.25 * old, susc=0.25)
    add_prickles(r, res, n=8, sep=(10.0, 11.0), A=(1.2, 2.0), B=(2.6, 4.0), H=(1.0, 1.5), off=(0.45, 0.7),
                 base='#5e5242', tip='#3a2a22', tip_mix=0.75, gloss_p=0.55, gam=(1.6, 2.2), base_mix=0.35)
    add_prickles(r, res, n=8, sep=(7.0, 7.0), A=(0.35, 0.55), B=(0.45, 0.75), H=(0.8, 1.3), off=(0.0, 0.15),
                 base='#624a3e', tip='#3a2a24', tip_mix=0.8, gloss_p=0.5)
    return res


def bloom_field(r, base, rub):
    """A waxy bloom: fine-grained and fairly even, thinner where rubbed in long streaks along the cane."""
    fine = 0.5 + 0.5 * ss(-1.5, 1.5, fbm(r, 0.12, 0.6, 2))
    rubbed = ss(0.3, 1.6, fbm(r, 0.5, 8.0, 3))
    return np.clip(base * fine * (1.0 - rub * rubbed), 0.0, 1.0)


def t_raspberry(r):
    """Rubus idaeus: light brown second-year canes under a thin grey bloom, the skin peeling a little; fine dark
    bristles and their scars."""
    h = smooth_relief(r, 0.03, 0.009, 0.002)
    alb = bark_base(r, '#6e5c4e', '#665446', sx=2.0, sy=14.0)
    bloom = bloom_field(r, 0.38, 0.7)
    alb = mix(alb, C('#8e8a88'), bloom)
    m, sd = film(r, 0.9, 1.0, 5.0, jag=0.5)
    alb = mix(vary(field('#76604e', '#705a4a', 0.5), 0.05, fbm(r, 0.15, 1.5, 3)), alb, m)
    res = dict(h=h + 0.015 * m, alb=alb, gloss=0.3 - 0.2 * bloom, susc=0.12, fresh=1.0 - m)
    peel(r, res, m, sd, 0.05, 0.25, '#9e948a', 0.35, 0.25)
    grain(r, res, 0.003, 0.03, aspect=3.0)
    add_prickles(r, res, n=150, sep=(2.6, 2.2), A=(0.13, 0.22), B=(0.16, 0.28), H=(0.3, 0.6), off=(0.1, 0.4),
                 base='#563e3a', tip='#3a262a', tip_mix=0.85, gloss_p=0.35)
    pts = points(r, 120, 2.0, 2.0)
    k = len(pts)
    S = splat(pts, lambda i: (0.3, 0.3), wart_sprite(np.full(k, 0.12), np.full(k, 0.15), np.full(k, 0.06), nz(r, 0.1),
                                                     nz(r, 0.06), r.random(k)), {'h': 'max'})
    res['h'] = res['h'] + S['h']
    res['alb'] = mix(res['alb'], C('#40302c'), 0.75 * S['cov'])
    return res


def t_dewberry(r):
    """Rubus caesius: green canes flushed purple under a glaucous blue-grey bloom, small hooked prickles."""
    h = smooth_relief(r, 0.03, 0.008, 0.002)
    alb = bark_base(r, '#4e5c48', '#53604c', sx=2.0, sy=12.0)
    alb = mix(alb, C('#5a4246'), 0.6 * ss(-0.3, 1.4, fbm(r, 6.0, 20.0, 2)))
    bloom = bloom_field(r, 0.45, 0.6)
    alb = mix(alb, C('#7e8a86'), bloom)
    res = grain(r, dict(h=h, alb=alb), 0.003, 0.03, aspect=3.0)
    res.update(gloss=0.3 - 0.22 * bloom, susc=0.05)
    add_prickles(r, res, n=36, sep=(4.5, 4.5), A=(0.25, 0.45), B=(0.4, 0.7), H=(0.5, 0.9), off=(0.4, 0.65),
                 base='#5a4044', tip='#4a2e34', tip_mix=0.8, gloss_p=0.45, gam=(1.4, 2.0))
    return res


def t_elder(r):
    """Sambucus nigra, S. racemosa: light grey-brown corky bark, deeply furrowed in an interlaced network; the corky
    ridges finely fissured and pitted, a few raised lenticels."""
    # The furrows: borders of a net of long cells pointed into lenses, the ridges interlacing; narrower where a
    # border runs across the stem, so the ridges merge at their tips.
    W = cells(r, 11, 4, aspect=4.0, warp=(0.9 * MM * nz(r, 1.5, 8.0), 1.0 * MM * nz(r, 3.0, 6.0)))
    d = W['e'] + 0.12 * fbm(r, 0.4, 1.0, 2)
    along = ss(0.2, 0.8, np.abs(W['bn']))
    w = (0.6 + 0.8 * ss(-1.0, 1.0, nz(r, 1.5, 6.0))) * (0.15 + 0.85 * along)
    t = np.clip(d / (w + 1e-3), 0.0, 1.0)
    furrow = (1.0 - ss(0.0, 1.0, t)) ** 0.8 * (0.25 + 0.75 * along)
    top = ss(0.0, 1.0, np.clip(d / (2.0 * w + 0.6), 0.0, 1.0)) ** 0.7
    h = -1.4 * furrow + 0.15 * top
    # The corky ridges: a fine fissure net, cork scales and pits.
    hf2, t2, d2, op2 = fissure_net(r, 0.45, 3.5, 0.22, 0.3, closing=0.1)
    ridge = ss(0.3, 0.8, t)
    h = h + hf2 * ridge
    cork = cells(r, 90, 55, aspect=1.4, warp=warp_xy(r, 0.3, 0.5, 1.0))
    ck = cattr(r, cork, -1.0, 1.0) * ss(0.0, 0.08, cork['e']) - 0.6 * np.exp(-(cork['e'] / 0.05) ** 2)
    pores = cells(r, 170, 130, R=1)
    h = h + (0.06 * ck + 0.05 * fbm(r, 0.25, 0.5, 3) + 0.03 * ss(0.0, 0.12, pores['e'])) * ss(0.1, 0.6, t)
    h = h + 0.015 * lines(nz(r, 0.12, 1.4), 0.4)
    alb = bark_base(r, '#6a6256', '#6e665a', sx=1.5, sy=6.0, streak=0.04, grain=0.08)
    alb = mix(alb, C('#78736c'), 0.35 * top * ss(0.0, 1.4, fbm(r, 1.0, 3.0, 3)))
    alb = mix(alb, C('#3c362f'), 0.75 * furrow ** 0.8)
    alb = mix(alb, C('#58504a'), 0.4 * blur(1.0 - t2, 0.1) * ridge)
    alb = vary(alb, 0.07, ck)
    res = grain(r, dict(h=h, alb=alb), 0.015, 0.05, scale=1.2, aspect=1.5)
    pts = points(r, 30, 5.0, 6.0, weight=ss(0.7, 1.0, top))
    k = len(pts)
    A = np.clip(r.lognormal(np.log(0.5), 0.3, k), 0.3, 1.0)
    B = A * r.uniform(1.1, 1.7, k)
    Wt = splat(pts, lambda i: (A[i] * 1.3 + 0.1, B[i] * 1.3 + 0.1),
               wart_sprite(A, B, 0.12 + 0.12 * A, nz(r, 0.12), nz(r, 0.06), r.random(k), cleft=0.4),
               {'h': 'max', 'tone': 'over', 'cleft': 'max'})
    res['h'] = res['h'] + Wt['h']
    alb = algae(r, res['alb'], 0.12, where=0.3 + 0.4 * ss(0.2, 0.6, t) * (1.0 - top))
    alb = paint_lenticels(alb, dict(tone=Wt['tone'], cov=Wt['cov'], slit=Wt['cleft'], halo=0.0 * Wt['cov']),
                          '#7e766a', '#746c60', '#4e443a', slit_amt=0.5, cover=0.7)
    res.update(alb=specks(r, alb, 40, 0.15, '#201a16'), gloss=0.1 + 0.03 * nz(r, 2.0), susc=0.85)
    return res


def t_broom(r):
    """Cytisus scoparius, Chamaecytisus ruthenicus: dull green ridged (angled) stems, brown-grey corky streaks where
    older."""
    # The engine wraps one tile round every stem up to 2 cm thick: the ribs per tile are the ribs round a branch
    # (ten round the older ones, a few millimetres apart on a branch of 1 cm).
    ribs = 10
    sp = 60.0 / ribs
    xw = PX / MM + r.uniform(0.0, sp) + 0.4 * nz(r, 1.5, 12.0)
    ph = (xw / sp) % 1.0
    dd = np.minimum(ph, 1.0 - ph) * sp
    amp = 0.75 + 0.25 * ss(-1, 1, nz(r, 1.0, 8.0))
    h = amp * (0.14 * np.exp(-(dd / 0.35) ** 2) - 0.14 * ss(0.2, 2.6, dd))
    h = h + 0.006 * lines(nz(r, 0.1, 1.8), 0.4) + 0.02 * fbm(r, 4, 9, 3)
    old = ss(0.7, 1.8, fbm(r, 1.2, 12.0, 3)) * ss(0.2, 0.8, dd / (0.5 * sp))
    h = h + 0.02 * fissure_net(r, 0.4, 4.0, 0.15, 1.0, closing=0.3)[0]
    h = h + 0.03 * old * fbm(r, 0.2, 0.6, 3)
    alb = bark_base(r, '#4a5640', '#4e5a40', sx=2.0, sy=14.0, streak=0.05)
    alb = mix(alb, C('#5a644a'), 0.45 * np.exp(-(dd / 0.35) ** 2))
    alb = mix(alb, C('#3c4634'), 0.4 * ss(0.8, 2.8, dd))
    alb = mix(alb, C('#727a64'), 0.2 * lines(nz(r, 0.5, 0.05), 0.2) * ss(0.6, 2.0, dd))
    alb = mix(alb, C('#605a4c'), 0.8 * old)
    res = grain(r, dict(h=h, alb=alb), 0.003, 0.03, scale=0.7, aspect=4.0)
    res.update(gloss=0.38 - 0.1 * ss(0.6, 2.6, dd) - 0.25 * old, susc=0.15)
    return res


TYPES = [
    ('hazel', ['cor_ave'], t_hazel),
    ('alder_buckthorn', ['fra_aln'], t_alder_buckthorn),
    ('bird_cherry', ['pad_avi', 'amo_fru', 'vib_opu', 'ame_spi'], t_bird_cherry),
    ('cherry', ['pru_cer'], t_cherry),
    ('willow_grey', ['sal_cin', 'sal_vim', 'sal_acu'], t_willow_grey),
    ('willow_goat', ['sal_cap'], t_willow_goat),
    ('willow_smooth', ['sal_aur', 'sal_ros', 'sal_pen', 'sal_tri'], t_willow_smooth),
    ('spindle', ['euo_eur'], t_spindle),
    ('warty_spindle', ['euo_ver'], t_warty_spindle),
    ('hawthorn', ['cra_mon', 'syr_vul', 'hip_rha'], t_hawthorn),
    ('shreddy', ['lon_xyl', 'phi_cor'], t_shreddy),
    ('currant', ['rib_nig'], t_currant),
    ('gooseberry', ['rib_uva'], t_gooseberry),
    ('rose', ['ros_can', 'ros_maj'], t_rose),
    ('raspberry', ['rub_ida'], t_raspberry),
    ('dewberry', ['rub_cae'], t_dewberry),
    ('elder', ['sam_nig', 'sam_rac'], t_elder),
    ('broom', ['cyt_sco', 'cha_rut'], t_broom),
    ('juniper', ['jun_com'], t_juniper),
]


# ======================================================================================================
# Tree bark: the trunks and limbs of the trees (docs/dead-air/TREES_TEXTURES.md). A type's tile is 6 cm x its scale
# (TREE_TYPES: the species' bark_scale); everything below is in real millimetres. Two topologies of an old rhytidome:
# plates (Voronoi cells) between fissures on their borders, and a net of ridges on the borders round lens-shaped
# furrows.
# ======================================================================================================

def tbase(r, c1, c2, sx, sy, streak=0.05, grain=0.05, s=1.0):
    """A tree bark's colour between two tones in patches sx x sy mm, streaked along the stem and grained (s: the
    streaks' and the grain's size over the shrubs' 0.35 x 4 and 0.12 x 0.45 mm)."""
    alb = field(c1, c2, ss(-1.3, 1.3, fbm(r, sx, sy, 3)))
    alb = vary(alb, streak, fbm(r, 0.35 * s, 4.0 * s, 3))
    return vary(alb, grain, fbm(r, 0.12 * s, 0.45 * s, 2))


def warp2(r, a1, s1, a2, s2):
    """A two-octave domain warp in texels: a1 mm over features s1 mm, a2 mm over s2 mm (the across and the along
    displacement each its own field)."""
    wx = a1 * MM * nz(r, s1, s1 * 1.4) + a2 * MM * nz(r, s2, s2 * 1.4)
    wy = 0.6 * (a1 * MM * nz(r, s1 * 1.4, s1) + a2 * MM * nz(r, s2 * 1.4, s2))
    return wx, wy


def plates(r, nx, ny, aspect, w1=(4.0, 30.0), w2=(1.0, 8.0), jitter=1.0):
    """Plates of a rhytidome: cells nx x ny on the tile (the grid's shape and `aspect` make them long), their borders
    wandering by a two-octave warp (w1, w2: amplitude and feature size, mm); `along` 1 where a border runs along the
    stem, 0 across it."""
    W = cells(r, nx, ny, aspect=aspect, warp=warp2(r, w1[0], w1[1], w2[0], w2[1]), jitter=jitter)
    W['along'] = ss(0.15, 0.85, np.abs(W['bn']))
    return W


def rag(r, amp, s):
    """A fissure edge's raggedness: an isotropic field of amp mm over features s mm, with a finer octave."""
    return amp * (fbm(r, s, s * 1.2, 3) + 0.35 * fbm(r, 0.3 * s, 0.35 * s, 2))


def furrows(W, width, depth, cross=0.35, prof=0.8, rg=None):
    """The fissures on the plates' borders: `width` (mm, half the fissure's width at the surface) and `depth` (mm)
    where the border runs along the stem, `cross` of both where it runs across. Returns the relief (0 on the plates),
    t (0 in the fissure .. 1 off it) and the fissure's share."""
    k = cross + (1.0 - cross) * W['along']
    e = W['e'] if rg is None else np.maximum(W['e'] + rg, 0.0)
    t = np.clip(e / (width * k + 1e-3), 0.0, 1.0)
    fur = (1.0 - ss(0.0, 1.0, t)) ** prof
    return -depth * k * fur, t, fur


def lens_net(r, n, sep, a, b, a_sd=0.25, b_sd=0.3, tilt=5.0, bend=0.2, sharp=1.6, q=1.8, rg=0.6, dmin=0.6,
             wave=0.0, wave_s=40.0, weight=None):
    """Lens-shaped furrows between interlacing ridges (oak, the poplars, willows, robinia, ash, walnut, lime, maple):
    up to n furrows thrown at least sep mm (across, along) apart, each `a` mm half wide and `b` mm half long (their
    spread a_sd, b_sd), turned up to `tilt` degrees, bowed by `bend` of its width, its ends pointed (`sharp`: 1 a
    diamond, 2 a lens), its floor's depth share dmin..1.2 with walls the steeper at q > 1, its edge ragged by rg mm,
    all of them swaying across by `wave` mm over wave_s mm along the stem (one field: neighbours sway together, the
    furrows run in sinuous streams). Where two furrows end near each other the ridges between them merge: the ridges
    interlace. Returns the furrows' depth share (0 on a ridge) and their cover."""
    pts = points(r, n, sep[0], sep[1], weight=weight)
    k = len(pts)
    A = a * np.clip(r.lognormal(0.0, a_sd, k), 0.45, 1.9)
    B = b * np.clip(r.lognormal(0.0, b_sd, k), 0.4, 2.0)
    T = np.radians(np.clip(r.normal(0.0, tilt, k), -2.5 * tilt, 2.5 * tilt))
    Bd = bend * r.normal(0.0, 1.0, k)
    Dp = r.uniform(dmin, 1.2, k)
    J = fbm(r, 1.5, 2.0, 3)
    Wv = wave * nz(r, 0.35 * wave_s, wave_s)

    def fn(lx, ly, idx, i):
        c, s = np.cos(T[i]), np.sin(T[i])
        x = lx * c + ly * s + Wv[idx]
        y = -lx * s + ly * c
        u = np.clip(y / B[i], -1.0, 1.0)
        x = x - Bd[i] * A[i] * (1.0 - u * u)
        hw = A[i] * np.clip(1.0 - np.abs(u) ** sharp, 0.0, 1.0) ** 0.7
        d = (np.abs(x) + rg * J[idx]) / (hw + 1e-3)
        live = (np.abs(y) < B[i]).astype(np.float64)
        f = Dp[i] * (1.0 - np.clip(d, 0.0, 1.0) ** q) * live
        return {'cov': ss(1.0, 0.85, d) * live, 'f': f}

    def ext(i):
        c, s = abs(np.cos(T[i])), abs(np.sin(T[i]))
        ax = A[i] * (1.0 + abs(Bd[i])) + rg + 1.0 + 2.5 * wave
        return ax * c + B[i] * s + 0.5, ax * s + B[i] * c + 0.5

    S = splat(pts, ext, fn, {'f': 'max'})
    return np.clip(S['f'], 0.0, None), S['cov']


def terraces(r, sx, sy, levels, step, sharp=0.15, jag=0.35):
    """Layered flakes: a field's level sets as stacked layers `step` mm thick, each edge a small cliff (its outline
    jagged by a finer field). Returns the relief, the layer (0 the deepest) and the risers."""
    f = norm(fbm(r, sx, sy, 3, 0.5) + jag * fbm(r, sx * 0.3, sy * 0.3, 2))
    q = np.clip((f + 2.2) / 4.4, 0.0, 0.9999) * levels
    lv = np.floor(q)
    fr = q - lv
    h = step * (lv + ss(1.0 - sharp, 1.0, fr))
    return h, lv, np.exp(-((1.0 - fr) / 0.07) ** 2)


def tsmooth(r, macro=0.3, s=1.0):
    """A smooth young bark's relief: a gentle swell, stretch striae along the stem and fine wrinkles across it."""
    h = macro * fbm(r, 12.0 * s, 25.0 * s, 3)
    h = h + 0.03 * lines(nz(r, 0.35 * s, 6.0 * s), 0.45)
    return h + 0.02 * lines(nz(r, 5.0 * s, 0.3 * s), 0.35)


def tgrain(r, res, h_amt, c_amt, scale, aspect=2.0):
    """The fine periderm grain at a tree tile's texel size (cells of 0.7 x scale mm)."""
    return grain(r, res, h_amt, c_amt, scale=scale, aspect=aspect)


def fibres(r, s, along=8.0):
    """Long fibres along the stem (a corky ridge's stringy surface), about unit amplitude."""
    return lines(nz(r, 0.3 * s, along * s), 0.4) + 0.6 * lines(nz(r, 0.5 * s, 1.6 * along * s), 0.4)


def blots(r, n, size, sep, aspect=1.0):
    """Sparse rough blots (a birch's black marks, a white poplar's dark patches): their cover."""
    pts = points(r, n, sep, sep * aspect)
    k = len(pts)
    A = size * r.uniform(0.5, 1.4, k)
    J = fbm(r, 0.8 * size, 0.8 * size, 3)

    def fn(lx, ly, idx, i):
        d = np.sqrt((lx / A[i]) ** 2 + (ly / (A[i] * aspect)) ** 2) * (1.0 + 0.45 * J[idx])
        return {'cov': ss(1.0, 0.7, d)}
    return splat(pts, lambda i: (A[i] * 2.0, A[i] * aspect * 2.0), fn, {})['cov']


def dome(on, s1, s2):
    """A ridge's or a plate's rounded section between its fissures: their cover blurred at two widths (mm)."""
    return 0.6 * blur(on, s1) + 0.4 * blur(on, s2)


def ridge_surface(r, s, crack_share=0.25):
    """The rough top of an old ridge or plate: flaky blocks a centimetre or two (terraces), fine cracks, sparse
    transverse breaks (s: the type's scale). Returns the relief (mm) and the cracks' mask."""
    th, lv, edge = terraces(r, 2.5 * s, 4.0 * s, 4, 0.3 * s, sharp=0.2)
    hf2, t2, d2, op2 = fissure_net(r, 0.8 * s, 3.5 * s, 0.25 * s, 0.4 * s, closing=0.3)
    br = cross_cracks(r, 3.0 * s, 0.5 * s, 0.3 * s, crack_share)
    return th + hf2 - 0.8 * s * br + 0.12 * s * fbm(r, 0.3 * s, 0.4 * s, 3), np.maximum(1.0 - t2, br)


def furrowed(r, f, depth, dome_h, dome_s, surf, cols, s, top_k=0.45, crk=None, floor=0.05):
    """A lens-netted bark's relief and colour: the furrows `depth` mm deep, the ridges domed `dome_h` mm (blurred over
    dome_s mm), their surface relief `surf` on top; cols: the ridge's pair of tones, its lighter top's and the
    furrow's (from `floor` of the furrow's depth down: a deep furrow's walls in the ridge's colour). Returns the relief,
    the albedo, the ridges' share and the dome."""
    on = 1.0 - ss(0.03, 0.4, f)
    dm = dome(on, dome_s[0], dome_s[1])
    h = -depth * f + dome_h * dm + on * surf
    alb = tbase(r, cols[0][0], cols[0][1], 12.0 * s, 30.0 * s, s=s)
    top = top_k * ss(0.5, 0.95, dm) * ss(-0.5, 1.2, fbm(r, 3.0 * s, 8.0 * s, 3))
    alb = mix(alb, tbase(r, cols[1][0], cols[1][1], 4.0 * s, 10.0 * s, s=s), top)
    if crk is not None:
        alb = mix(alb, C(cols[2][1]), 0.45 * crk * on)
    alb = mix(alb, tbase(r, cols[2][0], cols[2][1], 4.0 * s, 12.0 * s, s=s), ss(floor, floor + 0.5, f))
    return h, alb, on, dm


def peel_s(r, res, m, sd, lift_h, curl, rim_col, rim_amt, shade_amt, s):
    """peel() at a tree tile's scale: the lifted edge's reach, its rim and the shadow under it s times as wide."""
    lift = ss(0.0, 0.04 * s, sd) * np.exp(-np.clip(sd, 0, None) / curl) * ss(-0.6, 1.0, nz(r, 2.0 * s, 1.0 * s))
    res['h'] = res['h'] + lift_h * lift
    shadow = np.exp(np.clip(sd, None, 0.0) / (0.14 * s)) * (1.0 - m)
    rim = np.exp(-np.abs(sd) / (0.05 * s)) * m
    res['alb'] = mix(res['alb'], C(rim_col), rim_amt * rim) * (1.0 - shade_amt * shadow)[..., None]
    res['ao_mul'] = res.get('ao_mul', 1.0) * (1.0 - 0.35 * shadow)
    return lift


def birch_lenticels(r, n, L_mean, sep, H=0.12):
    """A birch's lenticels: dark transverse dashes L_mean mm long, a millimetre or two wide, roughly in rows."""
    return lenticel_layer(r, n, sep, L_mean, 0.55, (0.08, 0.16), H, 0.06, Lmin=3.0, Lmax=48.0, Wmin=0.8, Wmax=2.6,
                          ang_sd=0.04, rough=0.4, taper=0.45)


def t_birch_white(r):
    """Betula pendula / pubescens, the trunk above the base and the limbs: chalk-white papery bark, cream and pinkish
    where an outer layer peeled in transverse strips (their edges lifting), dark transverse lenticel dashes 0.5-4 cm,
    grey patches and a few small black rough marks."""
    h = 0.1 * fbm(r, 15.0, 30.0, 3) + 0.02 * lines(nz(r, 6.0, 0.3), 0.4)
    alb = tbase(r, '#d6d4cd', '#c8c6be', 20.0, 30.0, streak=0.03, grain=0.03, s=2.0)
    alb = mix(alb, C('#a8a6a0'), 0.4 * ss(0.6, 2.0, fbm(r, 10.0, 18.0, 3)))
    res = dict(h=h, alb=alb, gloss=np.full((N, N), 0.16), fresh=np.zeros((N, N)))
    m, sd = film(r, 0.8, 40.0, 4.0, jag=0.55)
    under = tbase(r, '#d8cbb8', '#cfbca6', 15.0, 4.0, s=2.0)
    res['alb'] = mix(under, res['alb'], m)
    res['h'] = res['h'] + 0.1 * m
    res['fresh'] = 1.0 - m
    peel_s(r, res, m, sd, 0.4, 1.2, '#f0eee8', 0.5, 0.35, 2.0)
    L = birch_lenticels(r, 170, 11.0, (14.0, 7.0))
    res['h'] = res['h'] + L['h'] - 0.15 * L['cov']
    lc = mix(np.broadcast_to(C('#4a403a'), (N, N, 3)), C('#2e2622'), L['tone'])
    res['alb'] = mix(res['alb'], lc, 0.9 * L['cov'])
    res['alb'] = mix(res['alb'], C('#8a827a'), 0.35 * L['halo'])
    # Two small black rough marks, wider than tall: the rhytidome setting in (the tile repeats every 18 cm).
    bk = blots(r, 2, 6.0, 80.0, 0.6)
    res['h'] = res['h'] + 0.6 * bk * (1.0 + 0.5 * fbm(r, 0.8, 1.5, 3))
    res['alb'] = mix(res['alb'], vary(field('#2e2826', '#3c3532', 0.5), 0.1, fbm(r, 0.8, 1.2, 3)), 0.92 * bk)
    res['gloss'] = res['gloss'] - 0.1 * L['cov'] - 0.08 * bk
    tgrain(r, res, 0.01, 0.03, scale=1.8, aspect=0.4)
    res.update(susc=0.3, nstr=1.0, tint=0.06, lichen_s=1.5)
    return res


def diamonds(r, n, rows, A_mean, ratio, sep, jit_row=0.6, keep=0.75):
    """Diamond lenticels (aspen, white poplar) in loose transverse rows: half axes A_mean mm across, ratio along."""
    ys = (np.arange(rows) + r.uniform(-0.35, 0.35, rows)) * N / rows
    pts = []
    for y0 in ys:
        x = r.uniform(0, N)
        end = x + N
        while x < end - sep[0] * MM:
            if r.random() < keep:
                pts.append((x % N, (y0 + r.normal(0, jit_row) * sep[1] * MM) % N))
            x += r.uniform(sep[0], sep[0] * 2.2) * MM
    pts = np.array(pts[:n])
    k = len(pts)
    A = np.clip(r.lognormal(np.log(A_mean), 0.35, k), 0.4 * A_mean, 2.4 * A_mean)
    B = A * r.uniform(ratio[0], ratio[1], k)
    return splat(pts, lambda i: (A[i] * 1.25 + 0.3, B[i] * 1.25 + 0.3),
                 diamond_sprite(A, B, nz(r, 0.4), nz(r, 0.25), r.random(k)),
                 {'h': 'add', 'centre': 'max', 'rim': 'max', 'tone': 'over'})


def lenticel_bands(r, rows, width, cluster):
    """Where a smooth poplar's lenticels sit: loose transverse bands (rows across the tile at irregular steps, each a
    few millimetres wide: width, low and high), clustering along the band in stretches about cluster mm long (an
    acceptance field for lenticel_layer)."""
    ys = (np.arange(rows) + r.uniform(-0.32, 0.32, rows)) * N / rows
    band = np.zeros((N, N))
    for y0 in ys:
        w = r.uniform(width[0], width[1]) * MM
        dy = np.abs((PY - y0 + 0.5 * N) % N - 0.5 * N)
        band = np.maximum(band, np.exp(-(dy / w) ** 2) * r.uniform(0.6, 1.0))
    clus = ss(-0.4, 0.9, nz(r, cluster, 0.3 * cluster))
    return np.clip(0.03 + 0.97 * band * clus, 0.0, 1.0)


def poplar_smooth(r, P):
    """A smooth poplar's bark (the white poplar's and the aspen's above their fissured foot): a smooth chalky skin
    with gentle swells and fine transverse wrinkles, its tone in patches (paler chalky, greyer and greener ones, algae);
    its dark lenticels - dots and short dashes of 1-6 mm - in loose transverse bands, clustering and where dense merging
    into rough dark patches a centimetre or two across; a few larger rough blots; dark rhombic scars (diamonds: the
    aspen's many, the white poplar's few); the lenticels and scars corky, raised a little."""
    h = tsmooth(r, 0.25, 2.5) + 0.015 * lines(nz(r, 7.0, 0.35), 0.4)
    alb = tbase(r, P['base'][0], P['base'][1], 20.0, 30.0, streak=0.025, grain=0.03, s=2.5)
    alb = mix(alb, C(P['pale']), 0.35 * ss(0.5, 1.6, fbm(r, 8.0, 12.0, 3)))
    alb = mix(alb, C(P['grey']), P.get('grey_k', 0.4) * ss(0.5, 1.7, fbm(r, 12.0, 20.0, 3)))
    alb = vary(alb, 0.03, lines(nz(r, 8.0, 0.4), 0.4) - 0.3)
    wgt = lenticel_bands(r, P['rows'], P.get('band_w', (2.5, 6.0)), P.get('cluster', 10.0))
    L = lenticel_layer(r, P['n'], (2.0, 1.5), P.get('len_l', 2.4), 0.5, (0.35, 0.7), 0.06, 0.04, weight=wgt, Lmin=0.7,
                       Lmax=7.0, Wmin=0.45, Wmax=2.0, ang_sd=0.14, rough=0.5)
    merged = ss(0.22, 0.5, blur(L['cov'], 1.1))
    bl = blots(r, P.get('blots', 3), P.get('blot_a', 6.0), 45.0, 0.55)
    dark = np.maximum(np.maximum(L['cov'], merged), bl)
    rough = fbm(r, 0.5, 0.6, 3)
    h = h + L['h'] + (0.12 + 0.08 * rough) * np.maximum(merged, bl)
    D = diamonds(r, P.get('dn', 6), P.get('drows', 3), P.get('da', 9.0), P.get('dratio', (0.55, 0.85)),
                 P.get('dsep', (45.0, 20.0)),
                 keep=0.8)
    h = h + 3.0 * D['h']
    res = tgrain(r, dict(h=h, alb=algae(r, alb, P.get('algae', 0.1), s=15.0)), 0.01, 0.03, scale=1.8, aspect=0.5)
    lc = mix(np.broadcast_to(C(P['len'][0]), (N, N, 3)), C(P['len'][1]), L['tone'])
    lc = vary(lc, 0.12, rough)
    res['alb'] = mix(res['alb'], lc, 0.9 * dark)
    dc = mix(np.broadcast_to(C(P['len'][0]), (N, N, 3)), C(P['len'][1]), D['tone'])
    res['alb'] = mix(mix(res['alb'], dc, 0.88 * D['cov']), C('#1a1816'), 0.8 * D['centre'])
    res.update(gloss=P.get('gloss', 0.16) - 0.1 * np.maximum(dark, D['cov']), susc=P.get('susc', 0.4), tint=0.05)
    return res


def t_aspen_smooth(r):
    """Populus tremula, the trunk above the base and the limbs (photo pack pop_tre): smooth green-grey to grey-white
    bark, dark lenticels in loose transverse bands and many dark diamond-shaped lenticels 0.5-2 cm, darker grey and
    green patches."""
    return poplar_smooth(r, dict(base=('#a7ab9e', '#9a9f91'), pale='#b6baae', grey='#7e8276', grey_k=0.35, rows=6,
                                 n=520, len=('#4a4a42', '#2e2e2a'), dn=16, drows=5, da=6.5, dratio=(0.7, 1.3),
                                 dsep=(28.0, 12.0), algae=0.14,
                                 gloss=0.2, susc=0.45))


def t_poplar_white(r):
    """Populus alba, the trunk above the base and the limbs (photo pack pop_alb): chalky white to light grey smooth
    bark with a cool, faintly green cast, its dark lenticels in loose transverse bands of dots and dashes merging into
    rough dark patches, a few dark rhombic scars, grey-green patches of algae and dust."""
    return poplar_smooth(r, dict(base=('#cdd0cb', '#c1c5bf'), pale='#dcdeda', grey='#a9aea6', grey_k=0.4, rows=7, n=900,
                                 len=('#3e403c', '#232523'), blots=4, blot_a=6.5, dn=4, drows=2, da=10.0,
                                 dsep=(70.0, 40.0), algae=0.08, gloss=0.15, susc=0.4))


def t_oak_young(r):
    """Quercus robur, a young trunk and the limbs (to some 20-30 years): smooth grey to silver-grey 'mirror' bark,
    faintly glossy, with darker grey mottling, fine shallow cracks setting in along the stem and small lenticels."""
    h = tsmooth(r, 0.2, 2.0)
    hf, t, d, op = fissure_net(r, 2.5, 18.0, 0.7, 0.5, closing=0.0)
    h = h + hf
    alb = tbase(r, '#8c887e', '#7c786e', 14.0, 25.0, streak=0.04, grain=0.04, s=2.0)
    alb = mix(alb, C('#66625a'), 0.35 * ss(0.5, 1.8, fbm(r, 6.0, 10.0, 3)))
    alb = mix(alb, C('#54504a'), 0.55 * blur(1.0 - t, 0.2))
    res = tgrain(r, dict(h=h, alb=alb), 0.012, 0.035, scale=1.6, aspect=1.5)
    L = lenticel_layer(r, 60, (8.0, 5.0), 2.2, 0.35, (0.3, 0.5), 0.05, 0.03, Lmin=1.0, Lmax=4.0, Wmin=0.4, Wmax=1.2)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(algae(r, res['alb'], 0.18, s=10.0), L, '#a09888', '#8c8476', '#4a443c', 0.5, 0.7)
    res.update(gloss=0.3 - 0.15 * (1.0 - t), susc=0.55, tint=0.06)
    return res


def t_hornbeam(r):
    """Carpinus betulus: smooth grey to silver-grey bark all its life, a darker grey net of wavy streaks along the
    stem (stretch marks over the fluted 'muscles'), mottled with darker patches and silvery ones, faint lenticels."""
    h = tsmooth(r, 0.25, 2.5) + 0.6 * fbm(r, 8.0, 50.0, 3)
    d, al = zero_lines(r, 3.0, 16.0, 2, along=True)
    streak = np.exp(-(d / 0.7) ** 2) * ss(-0.6, 0.6, nz(r, 6.0, 14.0)) * ss(0.25, 0.7, blur(al, 0.4))
    d2, al2 = zero_lines(r, 1.5, 6.0, 1, along=True)
    fine = np.exp(-(d2 / 0.35) ** 2) * ss(0.2, 1.0, nz(r, 4.0, 8.0))
    h = h - 0.2 * streak - 0.08 * fine
    alb = tbase(r, '#8e8b84', '#82807a', 15.0, 35.0, streak=0.03, grain=0.03, s=2.5)
    alb = mix(alb, C('#a8a6a0'), 0.5 * ss(0.4, 1.6, fbm(r, 6.0, 12.0, 3)))
    alb = mix(alb, C('#5e5a52'), 0.55 * ss(0.3, 1.5, fbm(r, 3.0, 5.0, 3)))
    alb = mix(alb, C('#4a4640'), 0.85 * blur(streak, 0.25))
    alb = mix(alb, C('#5e5a54'), 0.6 * fine)
    res = tgrain(r, dict(h=h, alb=alb), 0.01, 0.03, scale=1.8, aspect=1.2)
    L = lenticel_layer(r, 50, (10.0, 6.0), 2.5, 0.35, (0.25, 0.4), 0.03, 0.02, Lmin=1.0, Lmax=5.0, Wmin=0.4, Wmax=1.2)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(res['alb'], L, '#a8a298', '#968e84', '#5a544c', 0.4, 0.6)
    res['alb'] = algae(r, res['alb'], 0.3, s=18.0)
    res.update(gloss=0.22, susc=0.65, tint=0.05)
    return res


def t_maple(r):
    """Acer platanoides, an old trunk: grey bark finely fissured in a dense net of narrow flat ridges along the stem
    (not flaking), the ridges pale grey, the fissures dark."""
    f, cov = lens_net(r, 900, (6.5, 17.0), 1.4, 16.0, tilt=7.0, bend=0.3, sharp=1.6, q=1.0, rg=0.5, wave=1.5,
                      wave_s=25.0)
    rs, crk = ridge_surface(r, 1.2, 0.2)
    h, alb, on, dm = furrowed(r, f, 5.0, 2.0, (1.6, 3.5), 0.5 * rs,
                              (('#7e7a74', '#726e68'), ('#928e88', '#8a8680'), ('#423c38', '#4c4640')), 2.0, crk=crk)
    h = h + 0.5 * fbm(r, 20.0, 50.0, 2)
    res = tgrain(r, dict(h=h, alb=alb), 0.02, 0.05, scale=2.0, aspect=1.6)
    res['alb'] = algae(r, res['alb'], 0.22, where=on, s=15.0)
    res.update(gloss=0.12, susc=0.6, ao_s=1.0, tint=0.1)
    return res


def t_ash_young(r):
    """Fraxinus excelsior, a young trunk and the limbs: smooth olive-grey to green-grey bark, fine shallow cracks
    setting in along the stem, small pale lenticels."""
    h = tsmooth(r, 0.2, 2.0)
    hf, t, d, op = fissure_net(r, 2.0, 14.0, 0.6, 0.4, closing=0.1)
    h = h + hf
    alb = tbase(r, '#868678', '#7a7a6c', 14.0, 25.0, streak=0.04, grain=0.04, s=2.0)
    alb = mix(alb, C('#96968a'), 0.3 * ss(0.4, 1.6, fbm(r, 6.0, 12.0, 3)))
    alb = mix(alb, C('#5a5a50'), 0.5 * blur(1.0 - t, 0.2))
    res = tgrain(r, dict(h=h, alb=alb), 0.012, 0.035, scale=1.6, aspect=1.5)
    L = lenticel_layer(r, 70, (7.0, 5.0), 1.8, 0.35, (0.35, 0.55), 0.04, 0.02, Lmin=0.8, Lmax=3.5, Wmin=0.4, Wmax=1.2)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(algae(r, res['alb'], 0.15, s=10.0), L, '#a6a294', '#908c7e', '#4e4c42', 0.4, 0.75)
    res.update(gloss=0.28 - 0.12 * (1.0 - t), susc=0.5, tint=0.06)
    return res


def t_elm(r):
    """Ulmus laevis and U. minor, an old trunk: grey-brown bark, corky interlacing ridges cut by fissures and broken
    across into plates, their surfaces flaking in thin scales."""
    f, cov = lens_net(r, 680, (11.0, 28.0), 2.8, 26.0, tilt=9.0, bend=0.35, sharp=1.5, q=1.0, rg=0.8, wave=2.0,
                      wave_s=35.0)
    th, lv, edge = terraces(r, 5.0, 7.0, 3, 0.45, sharp=0.15)
    cr = cross_cracks(r, 6.0, 1.2, 0.7, 0.3)
    rs, crk = ridge_surface(r, 2.0, 0.2)
    h, alb, on, dm = furrowed(r, f, 8.0, 3.0, (3.0, 6.0), 0.5 * rs + th - 2.5 * cr,
                              (('#7a7068', '#6e645a'), ('#8a8076', '#80766c'), ('#38302a', '#443a32')), 2.4,
                              crk=np.maximum(crk, cr), floor=0.2)
    h = h + 0.6 * fbm(r, 25.0, 40.0, 2)
    alb = mix(alb, C('#7a6452'), 0.2 * on * ss(0.5, 1.5, fbm(r, 3.0, 6.0, 3)))
    res = tgrain(r, dict(h=h, alb=alb), 0.03, 0.05, scale=2.2, aspect=1.4)
    res['alb'] = algae(r, res['alb'], 0.2, where=on, s=15.0)
    res.update(gloss=0.1, susc=0.6, ao_s=1.1, tint=0.1)
    return res


def t_rowan(r):
    """Sorbus aucuparia: smooth, glossy grey to olive-grey bark all its life, transverse lenticels, fine transverse
    wrinkles; slightly fissured at an old trunk's foot."""
    h = tsmooth(r, 0.15, 1.6)
    alb = tbase(r, '#7a776e', '#6e6b62', 12.0, 20.0, streak=0.04, grain=0.04, s=1.6)
    alb = mix(alb, C('#88867c'), 0.25 * ss(0.6, 1.8, fbm(r, 8.0, 12.0, 3)))
    alb = vary(alb, 0.05, lines(nz(r, 4.0, 0.25), 0.4) - 0.3)
    res = tgrain(r, dict(h=h, alb=algae(r, alb, 0.1, s=10.0)), 0.01, 0.035, scale=1.5, aspect=0.6)
    L = lenticel_layer(r, 170, (6.0, 3.0), 4.0, 0.45, (0.15, 0.28), 0.06, 0.05, Lmin=1.5, Lmax=10.0, Wmin=0.6, Wmax=1.6)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(res['alb'], L, '#aaa292', '#968e7e', '#3e3830', 0.65, 0.9, halo='#5a564e',
                                 halo_amt=0.3)
    res.update(gloss=0.45 - 0.3 * L['cov'], susc=0.4, tint=0.05)
    return res


def t_alder_young(r):
    """Alnus glutinosa, a young trunk and the limbs: smooth olive-brown to grey-brown bark, glossy, densely set with
    pale ochre transverse lenticels."""
    h = tsmooth(r, 0.15, 1.5)
    alb = tbase(r, '#6a6658', '#5e5a4c', 10.0, 20.0, streak=0.04, grain=0.04, s=1.5)
    alb = mix(alb, C('#767264'), 0.3 * ss(0.4, 1.6, fbm(r, 5.0, 10.0, 3)))
    res = tgrain(r, dict(h=h, alb=algae(r, alb, 0.1, s=8.0)), 0.01, 0.035, scale=1.4, aspect=0.6)
    L = lenticel_layer(r, 300, (4.0, 2.4), 2.4, 0.45, (0.2, 0.35), 0.05, 0.03, Lmin=1.0, Lmax=6.0, Wmin=0.4, Wmax=1.3)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(res['alb'], L, '#aca286', '#988e74', '#4e4434', 0.5, 0.9)
    res.update(gloss=0.45 - 0.3 * L['cov'], susc=0.35, tint=0.05)
    return res


def t_pear(r):
    """Pyrus communis / pyraster, an old trunk: dark grey to grey-brown bark cracked into small squarish blocks
    1-2.5 cm, a little longer than wide, the cracks deep both along and across the stem, orange-brown in them."""
    W = plates(r, 9, 6, 1.5, w1=(3.0, 14.0), w2=(0.8, 3.5))
    hf, t, fur = furrows(W, 2.2, 5.0, cross=0.8, prof=0.65, rg=rag(r, 0.6, 1.5))
    on = ss(0.25, 0.8, t)
    dm = dome(on, 2.0, 4.0)
    rs, crk = ridge_surface(r, 1.0, 0.1)
    h = hf + 2.0 * dm + on * (0.5 * rs + cattr(r, W, -0.3, 0.3)) + 0.4 * fbm(r, 25.0, 30.0, 2)
    tone = cattr(r, W)
    alb = vary(field('#746c62', '#665e56', tone), 0.06, fbm(r, 1.5, 3.0, 3))
    alb = mix(alb, C('#88807a'), 0.35 * ss(0.55, 0.95, dm) * ss(-0.5, 1.2, fbm(r, 2.0, 3.0, 3)))
    crack = tbase(r, '#463628', '#3a2c22', 3.0, 6.0, s=1.8)
    alb = mix(crack, alb, ss(0.05, 0.65, t))
    res = tgrain(r, dict(h=h, alb=alb), 0.025, 0.05, scale=1.8, aspect=1.2)
    res['alb'] = algae(r, res['alb'], 0.22, where=on, s=12.0)
    res.update(gloss=0.1, susc=0.6, ao_s=1.2, tint=0.1)
    return res


def t_walnut_young(r):
    """Juglans regia, a young trunk and the limbs: smooth silver-grey bark, faint streaks along the stem, small
    lenticels, shallow cracks just setting in."""
    h = tsmooth(r, 0.18, 2.0)
    hf, t, d, op = fissure_net(r, 3.0, 20.0, 0.6, 0.35, closing=-0.2)
    h = h + hf
    alb = tbase(r, '#a29c94', '#948e86', 14.0, 30.0, streak=0.04, grain=0.04, s=2.0)
    alb = mix(alb, C('#b2aca4'), 0.3 * ss(0.4, 1.6, fbm(r, 6.0, 12.0, 3)))
    alb = mix(alb, C('#6c6660'), 0.5 * blur(1.0 - t, 0.2))
    res = tgrain(r, dict(h=h, alb=alb), 0.012, 0.035, scale=1.6, aspect=1.5)
    L = lenticel_layer(r, 50, (9.0, 6.0), 2.0, 0.35, (0.3, 0.5), 0.04, 0.02, Lmin=0.8, Lmax=4.0, Wmin=0.4, Wmax=1.2)
    res['h'] = res['h'] + L['h']
    res['alb'] = paint_lenticels(algae(r, res['alb'], 0.12, s=10.0), L, '#bab2a6', '#a69e92', '#5a544c', 0.4, 0.7)
    res.update(gloss=0.25, susc=0.45, tint=0.06)
    return res


def t_char(r):
    """A trunk charred by a ground fire (the zone's fires of 2015 and 2020): the old bark's deep furrows still there,
    its plates burnt to a matte black char cracked into convex blocks of 2-6 cm, a little longer than wide (alligator
    checking), their tops a dull charcoal grey with a satin sheen, the checks black; grey-white ash lodged in some of
    them; glossy black where the resin boiled out and burnt; a few dull brown streaks of unburnt bark between the
    burnt plates."""
    # The bark's own furrows under the char (a pine's or an oak's: the deepest relief).
    W = plates(r, 5, 2, 2.8, w1=(8.0, 45.0), w2=(2.5, 10.0))
    hf, t, fur = furrows(W, 7.0, 12.0, cross=0.45, prof=0.65, rg=rag(r, 1.4, 4.0))
    on = ss(0.25, 0.8, t)
    # The char's checks: convex blocks, a little longer than wide, finer crazing on them.
    C1 = plates(r, 8, 5, 1.5, w1=(3.0, 20.0), w2=(1.0, 5.0))
    hc, tc, fc = furrows(C1, 2.4, 4.0, cross=0.8, prof=0.75, rg=rag(r, 0.5, 1.5))
    blk = dome(ss(0.2, 0.8, tc), 2.0, 4.0)
    C2 = plates(r, 18, 12, 1.4, w1=(1.2, 8.0), w2=(0.4, 2.5))
    hc2, tc2, fc2 = furrows(C2, 0.7, 1.0, cross=0.8, prof=0.8)
    resin = ss(1.1, 1.7, fbm(r, 8.0, 16.0, 3)) * on
    # Unburnt bark: a few streaks along the stem on the plates' sheltered sides.
    unb = ss(1.5, 2.1, fbm(r, 4.0, 18.0, 3) + 0.6 * nz(r, 12.0, 30.0)) * on * (1.0 - resin)
    grain_h = 0.2 * fbm(r, 0.5, 0.7, 3)
    h = hf + 3.0 * dome(on, 6.0, 14.0) + on * ((hc + 1.6 * blk + hc2 + grain_h) * (1.0 - 0.7 * resin)) + 0.6 * unb
    h = h + 1.0 * fbm(r, 40.0, 70.0, 2)
    char = tbase(r, '#211f1d', '#2a2826', 10.0, 14.0, streak=0.04, grain=0.08, s=3.0)
    # The blocks' tops a dull charcoal grey, their checks black.
    char = mix(char, C('#4a4844'), 0.6 * ss(0.5, 1.0, blk) * ss(-0.6, 1.0, fbm(r, 3.0, 5.0, 3)))
    char = mix(char, C('#0e0d0c'), 0.85 * fc)
    char = mix(char, C('#141312'), 0.5 * fc2)
    alb = mix(char, C('#121110'), 0.7 * resin)
    ash = tbase(r, '#a8a49c', '#8c8882', 3.0, 5.0, s=2.0)
    alb = mix(alb, ash, 0.8 * ss(0.3, 0.8, fc) * ss(0.5, 1.4, fbm(r, 6.0, 10.0, 3)))
    alb = mix(alb, ash, 0.6 * ss(0.3, 0.8, fur) * ss(0.2, 1.2, fbm(r, 8.0, 14.0, 3)))
    alb = mix(alb, tbase(r, '#4a3a2e', '#42342a', 4.0, 12.0, s=2.0), 0.85 * unb)
    alb = mix(tbase(r, '#0c0b0a', '#141210', 6.0, 20.0, s=3.0), alb, ss(0.05, 0.6, t))
    res = tgrain(r, dict(h=h, alb=alb), 0.04, 0.06, scale=2.5, aspect=1.4)
    res['ao_mul'] = 1.0 - 0.2 * ss(0.4, 0.0, t)
    res.update(gloss=0.1 + 0.08 * blk * on + 0.55 * resin + 0.05 * unb, susc=0.02, nstr=0.9, ao_s=1.8, tint=0.06,
               fresh=None)
    return res


# ======================================================================================================
# Tree bark, second round: rhytidomes built as relief systems rather than line patterns. Fissures are V-shaped
# valleys round plates or ridges (Voronoi cells on the torus, some of their borders healed so the plates come in many
# sizes); the ridges carry layered strata and longitudinal splits; flaking barks are flakes lying over one another
# (a dead-leaves model) over their under-bark, each flake's rim lifting and casting a hairline of shadow. Every plate,
# ridge and flake has its own tone. The tiles are measured against the photo packs (docs/dead-air/TREES_TEXTURES.md).
# ======================================================================================================

def edt(mask, seeds=False):
    """The distance (mm) from every texel to the nearest texel of `mask` on the torus (jump flooding: steps N/2 .. 1,
    then 2 and 1 again); seeds: also that texel's x and y."""
    sx = np.where(mask, _XX, -1.0)
    sy = np.where(mask, _YY, -1.0)

    def d2(cx, cy):
        dx = np.abs(_XX - cx)
        dy = np.abs(_YY - cy)
        dx = np.minimum(dx, N - dx)
        dy = np.minimum(dy, N - dy)
        return np.where(cx >= 0, dx * dx + dy * dy, 1e18)

    best = d2(sx, sy)
    k, steps = N // 2, []
    while k >= 1:
        steps.append(k)
        k //= 2
    for k in steps + [2, 1]:
        for oy in (-k, 0, k):
            for ox in (-k, 0, k):
                if ox or oy:
                    cx = np.roll(sx, (oy, ox), axis=(0, 1))
                    cy = np.roll(sy, (oy, ox), axis=(0, 1))
                    dd = d2(cx, cy)
                    b = dd < best
                    best = np.where(b, dd, best)
                    sx = np.where(b, cx, sx)
                    sy = np.where(b, cy, sy)
    d = np.sqrt(best) / MM
    return (d, sx.astype(np.int64), sy.astype(np.int64)) if seeds else d


def hash01(x):
    """A repeatable pseudo-random value in [0, 1) per integer-valued x (a layer's, a plate's, a flake's own)."""
    return np.modf(np.abs(np.sin(x * 12.9898 + 78.233) * 43758.5453))[0]


def cells2(r, nx, ny, aspect=1.0, warp=None, jitter=1.0, R=2, stagger=False):
    """cells() with the second nearest seed's id too (id2: the cell across the nearest border). stagger: every other
    row of seeds shifted half a cell across (ny even): cells with pointed ends, a diamond net of borders."""
    cw, ch = N / nx, N / ny
    sh = (0.5 * (np.arange(ny) % 2))[:, None] if stagger else 0.0
    gx = (np.arange(nx)[None, :] + 0.5 + sh + jitter * (r.random((ny, nx)) - 0.5)) * cw
    gy = (np.arange(ny)[:, None] + 0.5 + jitter * (r.random((ny, nx)) - 0.5)) * ch
    X, Y = (PX, PY) if warp is None else (PX + warp[0], PY + warp[1])
    ci = np.floor(X / cw).astype(np.int64)
    cj = np.floor(Y / ch).astype(np.int64)
    b1 = np.full((N, N), np.inf)
    b2 = np.full((N, N), np.inf)
    i1 = np.zeros((N, N), np.int64)
    i2 = np.zeros((N, N), np.int64)
    p1x, p1y, p2x, p2y = (np.zeros((N, N)) for _ in range(4))
    for dj in range(-R, R + 1):
        nj = cj + dj
        wj = nj % ny
        for di in range(-R, R + 1):
            ni = ci + di
            wi = ni % nx
            qx = gx[wj, wi] + (ni - wi) * cw
            qy = gy[wj, wi] + (nj - wj) * ch
            d = (X - qx) ** 2 + ((Y - qy) / aspect) ** 2
            idn = wj * nx + wi
            c1 = d < b1
            c2 = ~c1 & (d < b2)
            b2 = np.where(c1, b1, np.where(c2, d, b2))
            i2 = np.where(c1, i1, np.where(c2, idn, i2))
            p2x = np.where(c1, p1x, np.where(c2, qx, p2x))
            p2y = np.where(c1, p1y, np.where(c2, qy, p2y))
            b1 = np.where(c1, d, b1)
            p1x = np.where(c1, qx, p1x)
            p1y = np.where(c1, qy, p1y)
            i1 = np.where(c1, idn, i1)
    ax, ay = p2x - p1x, (p2y - p1y) / aspect
    ln = np.sqrt(ax * ax + ay * ay) + 1e-9
    ux, uy = ax / ln, ay / ln
    es = -((X - 0.5 * (p1x + p2x)) * ux + ((Y - 0.5 * (p1y + p2y)) / aspect) * uy)
    sn = np.sqrt(ux * ux + (uy / aspect) ** 2)
    return dict(e=es / sn / MM, id=i1, id2=i2, n=nx * ny, bn=ux / sn, lx=(X - p1x) / MM, ly=(Y - p1y) / MM)


def plates2(r, nx, ny, aspect, w1, w2, keep_along=0.9, keep_cross=0.6, jitter=1.0, stagger=False):
    """Plates of a rhytidome as Voronoi cells nx x ny (borders wandering by a two-octave warp, w1 and w2: amplitude
    and feature size, mm), some borders healed (kept: keep_along of those running along the stem, keep_cross of
    those across it), so the plates come in many sizes: e the distance (mm) to the nearest open border, id the
    plate (the cells merged across healed borders), along 1 where that border runs along the stem."""
    W = cells2(r, nx, ny, aspect=aspect, warp=warp2(r, w1[0], w1[1], w2[0], w2[1]), jitter=jitter, stagger=stagger)
    a, b = np.minimum(W['id'], W['id2']), np.maximum(W['id'], W['id2'])
    along = ss(0.15, 0.85, np.abs(W['bn']))
    u = hash01(a * 977.0 + b * 131.0 + float(r.integers(1, 1000)))
    # (A cell's border with its own image a tile away is no fissure: the cell runs on through it.)
    keep = (u < keep_cross + (keep_along - keep_cross) * along) & (a != b)
    line = (W['e'] * MM < 0.75) & keep
    e = edt(line)
    # The plates: cells joined across their healed borders (union-find over the borders seen).
    par = list(range(W['n']))

    def find(i):
        while par[i] != i:
            par[i] = par[par[i]]
            i = par[i]
        return i
    pairs = set(zip(a[(W['e'] * MM < 1.0) & ~keep].tolist(), b[(W['e'] * MM < 1.0) & ~keep].tolist()))
    for i, j in pairs:
        pi, pj = find(i), find(j)
        if pi != pj:
            par[pi] = pj
    root = np.array([find(i) for i in range(W['n'])])
    gx, gy = grad_mm(e)
    g = np.sqrt(gx * gx + gy * gy) + 1e-9
    return dict(e=e, id=root[W['id']], n=W['n'], along=ss(0.3, 0.9, blur(np.abs(gx) / g, 1.0)))


def region_frame(lab):
    """Each region's own frame: the offset (mm) of every texel from its region's centre (circular means: a region
    across the tile's edge too) and the region's index 0..n-1."""
    u, inv = np.unique(lab, return_inverse=True)
    inv = inv.reshape(lab.shape)
    cnt = np.bincount(inv.ravel()).astype(np.float64)
    out = []
    for A in (_XX, _YY):
        ang = 2.0 * np.pi * (A + 0.5) / N
        cs = np.bincount(inv.ravel(), np.cos(ang).ravel())
        sn = np.bincount(inv.ravel(), np.sin(ang).ravel())
        c = np.arctan2(sn, cs) / (2.0 * np.pi) * N
        dd = (A + 0.5 - c[inv] + N / 2.0) % N - N / 2.0
        # (A region wrapping round the tile has no centre: no frame, offsets 0.)
        ok = np.hypot(cs, sn) / cnt > 0.55
        out.append(np.where(ok[inv], dd, 0.0) / MM)
    return out[0], out[1], inv, len(u)


def fissured_plates(r, nx, ny, aspect, warp1, warp2_, keep_along, keep_cross, half_w, cross_k, rg, jitter=1.0,
                    stagger=False):
    """Plates of a rhytidome between fissures on the borders of warped Voronoi cells (plates2): the signed distance
    sd (mm) to the nearest fissure's edge (< 0 in it), the fissure's half width there, the plates' labels and every
    texel's offset (mm) from its plate's middle. half_w: the half width (mm) of a fissure running along the stem,
    its spread over the tile (low, high); cross_k: a transverse one's share of it; rg: its edge's raggedness
    (amplitude, feature size mm)."""
    W = plates2(r, nx, ny, aspect, w1=warp1, w2=warp2_, keep_along=keep_along, keep_cross=keep_cross, jitter=jitter,
                stagger=stagger)
    k = cross_k + (1.0 - cross_k) * W['along']
    hw = (half_w[0] + (half_w[1] - half_w[0]) * ss(-1.0, 1.0, nz(r, 25.0, 60.0) + 0.5 * nz(r, 6.0, 15.0))) * k
    e = W['e'] + rg[0] * (fbm(r, rg[1], rg[1] * 1.3, 3) + 0.4 * fbm(r, 0.3 * rg[1], 0.35 * rg[1], 2))
    sd = e - hw
    # (A fissure's texels go with the nearest seed's plate.)
    lab = W['id']
    ox, oy, inv, n = region_frame(lab)
    return dict(sd=sd, hw=hw, lab=lab, ox=ox, oy=oy, inv=inv, n=n, along=W['along'])


def vwall(sd, depth, w, p=1.0):
    """A fissure's V: 0 at its rim (sd = 0), down to -depth over w mm inward (sd < 0); p > 1 a slot at the floor,
    p < 1 a U."""
    t = np.clip(-sd / w, 0.0, 1.0)
    return -depth * t ** p


def stack(q, nmax, sharp=0.18, tilt=0.25):
    """Stacked layers along a coordinate q (layer units, rising inward): the layer index (0 the outermost-lying,
    lowest, nmax the top) and the height in layer steps, each layer's riser at its outer edge, its surface tilting
    a little outward."""
    qc = np.clip(q, 0.0, nmax + 0.999)
    L = np.floor(qc)
    fr = qc - L
    top = q >= nmax
    hh = L + tilt * fr + (1.0 - tilt) * ss(1.0 - sharp, 1.0, fr)
    hh = np.where(top, nmax + tilt * np.clip(q - nmax, 0.0, 1.0), hh)
    L = np.where(top, float(nmax), L)
    riser = np.where(top, 0.0, np.exp(-((1.0 - fr) / (0.6 * sharp)) ** 2))
    return L, hh, riser


def facets(r, nx, ny, tilt, lift):
    """Crunchy micro-relief: Voronoi flakes nx x ny on the tile, each a little plane tilted up to `tilt` (slope) and
    raised up to `lift` mm, so they catch the light each its own way."""
    W = cells(r, nx, ny, aspect=1.4, warp=warp_xy(r, 60.0 / nx * 0.25, 60.0 / nx * 0.6))
    a, b, c = (cattr(r, W, -1.0, 1.0) for _ in range(3))
    return tilt * (a * W['lx'] + b * W['ly']) + lift * c, W


def rough_bark(r, s, amt=1.0, flake=2.5, crack=0.4, fib=0.0, tone=0.6):
    """The rough surface of old bark at a few millimetres: lumps, small flakes each tilted its own way (their edges
    little steps), hairline splits along the fibres, fibres (fib). Returns the relief (mm, about +-amt) and a
    brightness factor (each flake its own by `tone`, a little darker in the splits)."""
    lump = fbm(r, 1.5 * s, 2.2 * s, 3)
    fh, FW = facets(r, max(8, int(round(N / MM / (flake * s)))), max(6, int(round(N / MM / (flake * s * 1.4)))),
                    0.25, 0.35 * s)
    sp = lines(nz(r, 0.2 * s, 2.5 * s), 0.3) * ss(1.0 - 2.0 * crack, 1.2 - 2.0 * crack,
                                                   nz(r, 2.0 * s, 5.0 * s) * 0.5 + 0.5)
    h = amt * (0.45 * lump + fh / max(0.35 * s, 1e-3) * 0.35 - 0.5 * sp)
    b = np.exp(tone * (0.07 * lump + 0.08 * cattr(r, FW, -1.0, 1.0))) * (1.0 - tone * 0.15 * sp)
    if fib:
        f = fibres(r, s, 8.0)
        h = h + amt * fib * 0.3 * f
        b = b * np.exp(0.05 * fib * (f - 0.5))
    return h, b


def strata(r, sx, sy, levels, step, jag=0.45, sharp=0.15):
    """Layered flakes on a ridge: an anisotropic field's level sets (sx x sy mm, jagged by a finer octave) as
    stacked layers `step` mm thick, each edge a small cliff. Returns the relief, the layer index and the risers."""
    f = norm(fbm(r, sx, sy, 3, 0.5) + jag * fbm(r, 0.3 * sx, 0.25 * sy, 2))
    q = np.clip((f + 2.2) / 4.4, 0.0, 0.9999) * levels
    lv = np.floor(q)
    fr = q - lv
    h = step * (lv + ss(1.0 - sharp, 1.0, fr))
    return h, lv, np.exp(-((1.0 - fr) / 0.08) ** 2)


def splits(r, sx, sy, width, depth, open_share=0.55, open_s=(6.0, 25.0)):
    """Longitudinal splits in a ridge: the zero lines of an anisotropic field (sx x sy mm), V-shaped, `width` mm
    half wide and `depth` deep where open (open_share of their length, in stretches open_s mm), closing to nothing
    between; where the line turns across the stem it closes too. Returns the relief (<= 0) and the splits' share
    (1 at the line)."""
    n = fbm(r, sx, sy, 2, 0.35)
    gx, gy = grad_mm(n)
    g = np.sqrt(gx * gx + gy * gy)
    d = np.abs(n) / np.maximum(g, 0.5 * np.median(g))
    along = ss(0.55, 0.9, np.abs(gx) / (g + 1e-9))
    op = ss(1.0 - 2.0 * open_share - 0.2, 1.0 - 2.0 * open_share + 0.2,
            0.5 * nz(r, open_s[0], open_s[1]) + 0.5) * along
    w = width * (0.25 + 0.75 * op)
    t = np.clip(1.0 - d / np.maximum(w, 0.05), 0.0, 1.0) * ss(0.0, 0.3, op)
    return -depth * t ** 0.8, t


def mottle(r, alb, k, s):
    """Crustose lichen flecks, pale grey-green, and dark dirt flecks, a few millimetres (s: their size over a
    millimetre - a fissured type's own `s`, 3 for the oak), on their own stream; k their share."""
    rr = np.random.default_rng(int(r.integers(1 << 30)))

    def nzr(sx, sy):
        return norm(blur(rr.standard_normal((N, N)), sx, sy))
    pale = ss(1.4, 2.0, nzr(0.6 * s, 0.8 * s) + 0.6 * nzr(3.0 * s, 4.0 * s)) * k
    dirt = ss(1.5, 2.1, nzr(0.4 * s, 0.6 * s) + 0.5 * nzr(2.0 * s, 3.0 * s)) * k
    return mix(alb, C('#a8aa9c'), 0.45 * pale) * (1.0 - 0.3 * dirt)[..., None]


def fissured_bark(r, P):
    """A fissured bark (TREE_FISSURED): plates or ridges between V-shaped fissures, the ridges layered and split,
    every ridge its own tone, the bark mottled with lichen and dirt flecks."""
    c = P['cells']
    F = fissured_plates(r, c[0], c[1], c[2], P.get('warp1', (10.0, 70.0)), P.get('warp2', (3.0, 12.0)), 1.0,
                        P.get('keep_cross', 0.5), P['hw'], P.get('cross_k', 0.5), P.get('rag', (1.2, 3.0)),
                        jitter=P.get('jitter', 0.8), stagger=P.get('stagger', True))
    sd, hw, inv, n = F['sd'], F['hw'], F['inv'], F['n']
    s = P['s']
    rng = np.random.default_rng(int(r.integers(1 << 30)))
    pa = rng.normal(0.0, 1.0, n)[inv]
    # The ridge's section: domed from the fissure's rim over `dome` of its own half width (the plate's widest
    # distance to its rim), its top the flatter the smaller `dome`; the fissure a V down to its floor, the walls
    # running wv times the rim's half width.
    rmax = np.zeros(n)
    np.maximum.at(rmax, inv.ravel(), np.maximum(sd, 0.0).ravel())
    R = np.maximum(rmax[inv], 1.0)
    shoulder = 1.0 - (1.0 - np.clip(sd / (P.get('dome', 0.8) * R), 0.0, 1.0)) ** 2
    wall = hw * P.get('wv', 1.0) + 0.3
    h = np.where(sd > 0.0, P['H'] * shoulder, vwall(sd, P['D'], wall, P.get('p', 0.85)))
    on = ss(-1.0, 1.5, sd)
    tone = np.zeros((N, N))
    lift = np.zeros((N, N))
    if P.get('st'):
        sx, sy, lvn, step, tn = P['st']
        hs, lv, rs = strata(r, sx, sy, lvn, step)
        h = h + on * hs
        tone = tone + tn * (2.0 * hash01(lv * 1.0 + 0.37 + 13.0 * hash01(np.floor(pa * 3.0))) - 1.0)
        lift = np.maximum(lift, rs * on)
    sdark = np.zeros((N, N))
    for key in ('sp1', 'sp2'):
        if P.get(key):
            sx, sy, wd_, dp, osh = P[key]
            hsp, tsp = splits(r, sx, sy, wd_, dp, osh)
            h = h + on * hsp
            sdark = np.maximum(sdark, tsp * on)
    hr, br = rough_bark(r, s, P.get('rough', 0.3), flake=P.get('flake', 1.5), crack=P.get('crack', 0.3),
                        fib=P.get('fib', 0.0))
    h = h + (0.4 + 0.6 * on) * hr + P.get('tilt', 0.08) * pa * F['ox'] * ss(0.0, 8.0, sd)
    h = h + P.get('swell', 1.0) * fbm(r, 35.0, 70.0, 2)
    # Colour: the ridges' flanks, their crowns weathered (each ridge its own tone, each layer of the strata its
    # own); the strata's exposed edges fresher; the fissure's walls in the bark's colour, darker toward the floor
    # (wall_p: how soon), the floor dark only at its bottom; the splits dark.
    alb = tbase(r, P['side'][0], P['side'][1], 10.0 * s, 25.0 * s, s=s)
    topc = tbase(r, P['top'][0], P['top'][1], 6.0 * s, 15.0 * s, s=s)
    alb = mix(alb, topc, P.get('top_k', 0.6) * ss(0.3, 1.0, shoulder) * ss(-0.5, 1.3, fbm(r, 3.0 * s, 6.0 * s, 3)
                                                                          + 0.4 * pa))
    alb = alb * np.exp(P.get('tone', 0.08) * pa + tone)[..., None]
    alb = mix(alb, C(P.get('edge_col', P['top'][0])), P.get('edge_k', 0.2) * lift)
    if P.get('island_len'):
        # (A birch's islands of white bark between its black furrows: its dark transverse lenticels on them.)
        L = birch_lenticels(r, P.get('len_n', 90), P.get('len_l', 9.0), P.get('len_sep', (16.0, 8.0)))
        isl = ss(1.0, 3.0, sd)
        h = h + (L['h'] - 0.15 * L['cov']) * isl
        lc = mix(np.broadcast_to(C('#4a403a'), (N, N, 3)), C('#2e2622'), L['tone'])
        alb = mix(alb, lc, 0.85 * L['cov'] * isl)
    dt = np.clip(-sd / wall, 0.0, 1.0)
    alb = alb * (1.0 - P.get('wall_dark', 0.3) * dt ** P.get('wall_p', 1.3))[..., None]
    floor = tbase(r, P['floor'][0], P['floor'][1], 4.0 * s, 12.0 * s, s=s)
    alb = mix(alb, floor, ss(P.get('floor_t', 0.75), 1.0, dt))
    alb = mix(alb, floor, P.get('split_dark', 0.4) * ss(0.2, 0.9, sdark))
    alb = alb * br[..., None]
    res = dict(h=h, alb=alb)
    if P.get('algae'):
        res['alb'] = algae(r, res['alb'], P['algae'], where=0.35 + 0.65 * np.maximum(ss(0.5, -2.0, sd), sdark),
                           s=6.0 * s)
    res.update(gloss=P.get('gloss', 0.1), susc=P.get('susc', 0.6), nstr=P.get('nstr', 0.85), ao_s=P.get('ao_s', 1.0),
               ao_k=P.get('ao_k', 2.6))
    k = P.get('mottle', 1.0)
    if k:
        res['alb'] = mottle(r, res['alb'], k, s)
    # The crust at the texel scale; the colour following the relief over the fissures' width (tint_s: build()'s
    # scale), the hollows darker, the crowns paler.
    hm, bm = micro(r, **P.get('micro', {}))
    res['alb'] = res['alb'] * bm[..., None]
    res['h'] = res['h'] + hm
    res.update(tint=P.get('tint', 0.3), tint_s=P.get('tint_s', 3.0))
    return res


def dead_leaves(r, n, a_mm, aspect, lob=0.4, size_sd=0.45, rot=0.25, weight=None, amin=0.35, amax=2.6, lob_s=0.22):
    """Thin scales or flakes lying over one another (a dead-leaves model): n lobed ovals a_mm half across (lognormal
    spread size_sd, clipped to amin..amax of it) and aspect times as long, thrown at random (the density by
    `weight`), each over the ones thrown before. Returns per texel: how many cover it (soft), the top one's index
    (-1 bare) and, in its own frame, the nearness rho (0 at its middle, 1 at its rim) and its offsets u (across)
    and v (along the stem, + up) over its half axes."""
    pts = r.random((n, 2)) * N
    if weight is not None:
        keep = r.random(n) < weight[pts[:, 1].astype(int) % N, pts[:, 0].astype(int) % N]
        pts = pts[keep]
    k = len(pts)
    A = a_mm * np.clip(r.lognormal(0.0, size_sd, k), amin, amax)
    B = A * aspect * r.uniform(0.8, 1.25, k)
    T = r.normal(0.0, rot, k)
    J = fbm(r, lob_s * a_mm, lob_s * a_mm * aspect, 3, 0.45)
    cnt = np.zeros((N, N))
    top = np.full((N, N), -1, np.int64)
    rho_t = np.full((N, N), 2.0)
    u_t = np.zeros((N, N))
    v_t = np.zeros((N, N))
    for i in range(k):
        x0, y0 = pts[i]
        ex = (max(A[i], B[i]) * (1.0 + 2.5 * lob) + 0.5) * MM
        xs = np.arange(int(np.floor(x0 - ex)), int(np.ceil(x0 + ex)) + 1)
        ys = np.arange(int(np.floor(y0 - ex)), int(np.ceil(y0 + ex)) + 1)
        idx = np.ix_(ys % N, xs % N)
        lx = ((xs + 0.5 - x0) / MM)[None, :]
        ly = ((ys + 0.5 - y0) / MM)[:, None]
        c, sn = np.cos(T[i]), np.sin(T[i])
        uu, vv = (lx * c + ly * sn) / A[i], (-lx * sn + ly * c) / B[i]
        rho = np.sqrt(uu * uu + vv * vv) * (1.0 + lob * J[idx])
        cov = ss(1.0, 0.92, rho)
        cnt[idx] = cnt[idx] + cov
        win = rho < 1.0
        top[idx] = np.where(win, i, top[idx])
        rho_t[idx] = np.where(win, rho, rho_t[idx])
        u_t[idx] = np.where(win, np.broadcast_to(uu, rho.shape), u_t[idx])
        v_t[idx] = np.where(win, np.broadcast_to(vv, rho.shape), v_t[idx])
    return dict(cnt=cnt, top=top, rho=rho_t, u=u_t, v=v_t, n=k)


def flaking_bark(r, P):
    """A flaking bark (TREE_FLAKING): the under-bark under flakes lying over one another, each flake's rim lifting
    on one side, a thin light line on its edge and a thin shadow line under it on the flake below, every flake its
    own tone."""
    s = P.get('s', 2.0)
    D = dead_leaves(r, P['n'], P['a'], P['aspect'], lob=P.get('lob', 0.4), size_sd=P.get('size_sd', 0.45),
                    rot=P.get('rot', 0.25), amin=P.get('amin', 0.35), lob_s=P.get('lob_s', 0.22))
    cnt = np.minimum(D['cnt'], P.get('cap', 4.0))
    top, rho, v = D['top'], D['rho'], D['v']
    ontop = top >= 0
    sc = np.where(ontop, hash01(top * 1.0 + 0.37), 0.5)
    # Which part of the rim lifts: the lower edge (down 1, roof tiles), else a side of each flake's own choosing,
    # `curl` of its round; the lift rising over the outer `rim` of the flake.
    ang = np.where(ontop, hash01(top * 1.0 + 0.71) * 2.0 * np.pi, 0.0)
    side = np.cos(ang) * D['u'] + np.sin(ang) * v
    own = ss(1.0 - 2.0 * P.get('curl', 0.5), 1.2 - 2.0 * P.get('curl', 0.5), 0.5 + 0.5 * side / np.maximum(rho, 1e-3))
    dirw = (1.0 - P.get('down', 0.5)) * own + P.get('down', 0.5) * ss(0.2, -0.6, v)
    rim = ss(1.0 - P.get('rim', 0.3), 1.0, rho) * ontop
    lift = P.get('lift', 0.5) * rim * dirw * (0.4 + 0.6 * sc)
    h = P.get('thick', 0.3) * cnt + lift + P.get('swell', 0.3) * fbm(r, 6.0 * s, 10.0 * s, 3)
    h = h + P.get('dome', 0.0) * np.clip(1.0 - rho * rho, 0.0, 1.0) * ontop
    hr, br = rough_bark(r, s, P.get('rough', 0.3), flake=P.get('flake', 1.5), crack=P.get('crack', 0.3),
                        fib=P.get('fib', 0.0))
    h = h + hr
    # Colour: the under-bark, the flakes each their own tone, weathered patches (grey: by place; grey_flakes:
    # whole flakes), some flakes lighter (light_flakes); a flake the fresher the fewer lie under it.
    under = tbase(r, P['under'][0], P['under'][1], 4.0 * s, 6.0 * s, s=s)
    scol = mix(np.broadcast_to(C(P['cols'][0]), (N, N, 3)), C(P['cols'][1]), sc)
    scol = vary(scol, P.get('vary', 0.06), fbm(r, 1.5 * s, 2.5 * s, 3)) * np.exp(P.get('tone', 0.08) *
                                                                                   (2.0 * sc - 1.0))[..., None]
    if P.get('grey'):
        gr = ss(P['grey'][1], P['grey'][1] + 0.8, fbm(r, P.get('grey_s', 10.0), P.get('grey_s', 10.0) * 1.5, 3))
        scol = mix(scol, vary(field(P['grey'][0], P['grey'][0], 0.5), 0.06, fbm(r, 1.0 * s, 2.0 * s, 3)),
                   P['grey'][2] * gr)
    if P.get('grey_flakes'):
        gsel = (hash01(top * 1.0 + 0.13) < P['grey_flakes'][1]) & ontop
        scol = mix(scol, vary(field(P['grey_flakes'][0], P['grey_flakes'][0], 0.5), 0.06,
                              fbm(r, 1.0 * s, 2.0 * s, 3)), 0.85 * gsel)
    if P.get('light_flakes'):
        lsel = (hash01(top * 1.0 + 0.29) < P['light_flakes']) & ontop
        scol = mix(scol, C(P['light_col']), 0.6 * lsel * (0.5 + 0.5 * ss(0.0, 1.0, rho)))
    if P.get('fresh_col'):
        scol = mix(scol, C(P['fresh_col']), P.get('fresh_k', 0.5) * ss(P.get('fresh_c', 1.6), 0.6, cnt))
    alb = mix(under, scol, ss(0.25, 0.75, D['cnt']))
    if P.get('patches'):
        # Patches of flakes of one age, a few centimetres to a decimetre (patches: the older's and the fresher's
        # tints, size mm, amount): the mottle that still reads from ten metres, where the flakes are under a pixel.
        pa_, pb_, psz, pk = P['patches']
        f = norm(fbm(r, psz, psz * 1.5, 3) + 0.4 * fbm(r, 0.35 * psz, 0.5 * psz, 2))
        alb = alb * (1.0 + pk * ss(0.2, 1.4, f)[..., None] * (np.array(pa_) - 1.0))
        alb = alb * (1.0 + pk * ss(0.2, 1.4, -f)[..., None] * (np.array(pb_) - 1.0))
    if P.get('scabs'):
        # Islands of the old scaly bark left on it (scabs: its pair of tones, share, size mm): rough, their rims
        # ragged, a millimetre proud.
        sa_, sb_, share, ssz = P['scabs']
        g = fbm(r, ssz, ssz * 1.6, 3) + 0.4 * fbm(r, 0.3 * ssz, 0.4 * ssz, 2)
        isl = ss(-0.6, 0.8, level_set(g, share))
        hs, bs = rough_bark(r, s, 1.0, flake=2.2, crack=0.5)
        alb = mix(alb, tbase(r, sa_, sb_, 3.0 * s, 6.0 * s, s=s) * bs[..., None], isl)
        h = h + isl * (0.8 + hs)
    if P.get('pits'):
        # Small pits scattered through the bark - resin ducts, insect holes, a few millimetres each, each its own size
        # and darkness, a little taller than wide (a single big island a tile read as evenly spaced dark dots over a
        # trunk).
        n_p, (r0, r1), pc, pd = P['pits']
        rng = np.random.default_rng(int(r.integers(1 << 30)))
        yy, xx = np.mgrid[0:N, 0:N].astype(np.float64)
        pit = np.zeros((N, N))
        for _ in range(n_p):
            cx, cy = rng.uniform(0.0, N, 2)
            rad = rng.uniform(r0, r1) * MM
            dx = (xx - cx + N / 2) % N - N / 2
            dy = ((yy - cy + N / 2) % N - N / 2) / 1.4
            pit = np.maximum(pit, rng.uniform(0.5, 1.0) * ss(rad + 0.6, rad - 0.6, np.sqrt(dx * dx + dy * dy)))
        alb = mix(alb, C(pc), 0.85 * pit)
        h = h - pd * pit
    # The edge: a thin light line where the rim lifts (the flake's cut edge catching the light).
    edge = ss(1.0 - P.get('rim_c', 0.08), 1.0, rho) * ontop * dirw
    alb = mix(alb, C(P['edge_col']), P.get('edge_k', 0.4) * edge)
    # The shadow line: under a lifted edge, on what lies below it (down the stem, a little blurred).
    k = max(1, int(round(P.get('shadow_d', 0.6) * s * MM)))
    sl = np.clip(np.roll(blur(lift, P.get('shadow_w', 0.3) * s), -k, axis=0) - lift, 0.0, None)
    sl = np.clip(2.5 * sl / max(P.get('lift', 0.5), 1e-3), 0.0, 1.0)
    alb = alb * (1.0 - P.get('shadow', 0.5) * sl)[..., None] * br[..., None]
    res = dict(h=h, alb=alb, gloss=P.get('gloss', 0.12) + P.get('gloss_under', 0.0) * (1.0 - ss(0.25, 0.75, D['cnt'])),
               fresh=1.0 - ss(0.25, 0.75, D['cnt']))
    res['ao_mul'] = 1.0 - 0.4 * sl
    if P.get('algae'):
        res['alb'] = algae(r, res['alb'], P['algae'], s=6.0 * s)
    res.update(susc=P.get('susc', 0.5), nstr=P.get('nstr', 1.0), ao_s=P.get('ao_s', 1.0), ao_k=P.get('ao_k', 1.0),
               tint=P.get('tint', 0.06))
    if P.get('micro') is not None:
        hm, bm = micro(r, **P['micro'])
        res['alb'] = res['alb'] * bm[..., None]
        res['h'] = res['h'] + hm
    return res


def scaly_bark(r, P):
    """A bark of thin plates between narrow cracks (TREE_SCALY): the plates from fissured_plates (cells, keep_cross,
    the cracks' half width hw), a share `gone` of them fallen (the under-bark `fresh` there, a little lower), the
    others thin (`thick` mm), each lifting along one side of its rim (its own direction) by `lift` mm over `lift_w`
    mm, a thin shadow line under the lifted rim; colours: the plates (`cols` pair, each its own between them, `tone`
    more, weathered patches `grey`), the cracks' floor `floor`, the under-bark `fresh`."""
    c = P['cells']
    s = P.get('s', 2.0)
    F = fissured_plates(r, c[0], c[1], c[2], P.get('warp1', (6.0, 50.0)), P.get('warp2', (2.0, 10.0)),
                        P.get('keep_along', 1.0), P.get('keep_cross', 0.8), P['hw'], P.get('cross_k', 0.8),
                        P.get('rag', (0.8, 2.5)), jitter=P.get('jitter', 0.9), stagger=P.get('stagger', False))
    sd, hw, inv, n = F['sd'], F['hw'], F['inv'], F['n']
    rng = np.random.default_rng(int(r.integers(1 << 30)))
    gone = (rng.random(n) < P.get('gone', 0.15))[inv].astype(np.float64)
    tone = rng.random(n)[inv]
    ang = rng.uniform(0.0, 2.0 * np.pi, n)[inv]
    tilt = rng.normal(0.0, 1.0, n)[inv]
    keep = 1.0 - gone
    # The plate: flat, lifting on one side of its rim (where its frame points the plate's way).
    on = ss(0.0, 0.8, sd)
    rr = np.sqrt(F['ox'] ** 2 + F['oy'] ** 2) + 1e-6
    side = ss(0.0, 0.6, (np.cos(ang) * F['ox'] + np.sin(ang) * F['oy']) / rr)
    lw = P.get('lift_w', 6.0)
    lift = P.get('lift', 1.5) * ss(lw, 0.0, sd) * side * keep * on
    hr, br = rough_bark(r, s, P.get('rough', 0.4), flake=P.get('flake', 1.6), crack=P.get('crack', 0.3))
    plate = P.get('thick', 0.8) * on + lift + P.get('tilt', 0.03) * tilt * F['ox'] * on + hr * on
    under = -0.4 * P.get('thick', 0.8) + 0.4 * hr
    crack = vwall(sd, P.get('D', 2.5), hw + 0.3, 0.8)
    h = np.where(sd > 0.0, keep * plate + gone * under, crack) + P.get('swell', 0.6) * fbm(r, 30.0, 50.0, 2)
    pc = mix(np.broadcast_to(C(P['cols'][0]), (N, N, 3)), C(P['cols'][1]), tone)
    pc = vary(pc, P.get('vary', 0.06), fbm(r, 2.0 * s, 3.0 * s, 3)) * np.exp(P.get('tone', 0.08) *
                                                                              (2.0 * tone - 1.0))[..., None]
    if P.get('grey'):
        g = P['grey']
        pc = mix(pc, C(g[0]), g[2] * ss(g[1], g[1] + 0.8, fbm(r, P.get('grey_s', 30.0), P.get('grey_s', 30.0) * 1.4,
                                                              3)))
    fr = tbase(r, P['fresh'][0], P['fresh'][1], 4.0 * s, 6.0 * s, s=s)
    alb = mix(pc * br[..., None], fr, gone)
    # The lifted rim's cut edge a thin light line; the shadow line under it, down the stem.
    edge = ss(0.6, 0.0, sd) * side * keep * ss(-0.2, 0.2, sd)
    alb = mix(alb, C(P.get('edge_col', P['cols'][0])), P.get('edge_k', 0.3) * edge)
    k = max(1, int(round(P.get('shadow_d', 0.6) * s * MM)))
    sl = np.clip(np.roll(blur(lift, P.get('shadow_w', 0.3) * s), -k, axis=0) - lift, 0.0, None)
    sl = np.clip(2.5 * sl / max(P.get('lift', 1.5), 1e-3), 0.0, 1.0)
    alb = alb * (1.0 - P.get('shadow', 0.6) * sl)[..., None]
    dt = np.clip(-sd / np.maximum(hw + 0.3, 0.4), 0.0, 1.0)
    alb = mix(alb, tbase(r, P['floor'][0], P['floor'][1], 3.0 * s, 6.0 * s, s=s), ss(0.0, 0.8, dt))
    res = dict(h=h, alb=alb, gloss=P.get('gloss', 0.12) + 0.05 * gone, fresh=gone * on)
    res['ao_mul'] = 1.0 - 0.35 * sl
    if P.get('algae'):
        res['alb'] = algae(r, res['alb'], P['algae'], where=keep, s=6.0 * s)
    res.update(susc=P.get('susc', 0.55), nstr=P.get('nstr', 1.0), ao_s=P.get('ao_s', 1.0), ao_k=P.get('ao_k', 1.0),
               tint=P.get('tint', 0.06))
    if P.get('micro') is not None:
        hm, bm = micro(r, **P['micro'])
        res['alb'] = res['alb'] * bm[..., None]
        res['h'] = res['h'] + hm
    return res


# The fissured barks (fissured_bark): cells (across, along the tile, their aspect), stagger (alternate rows offset:
# pointed plates, a diamond net), keep_cross (the share of transverse borders open), jitter; the fissures' half
# width hw (mm, low and high over the tile), cross_k (a transverse one's share of it), depth D (mm), wall shape p
# and ragged edge rag, the walls' run wv (over the rim's half width), their darkening wall_dark (wall_p: how soon);
# the ridges' height H, domed over `dome` of their own half width; st (strata: sx, sy mm, levels, step mm, tone),
# sp1 and an optional sp2 (splits: sx, sy mm, half width mm, depth mm, open share); the surface (rough_bark at
# scale s: rough, flake, crack, fib); colours: the ridges' tops, flanks and the fissures' floor (pairs), top_k,
# tone, floor_t, split_dark; mottle (the lichen and dirt flecks' share); algae, the lichen's susceptibility susc;
# ao_k the cavity's strength (the fissures' floors darker in the ambient light); micro (micro()'s arguments: the
# crust at the texel scale); tint, tint_s (the colour following the relief, 0.3 over 3 x build()'s scale).
TREE_FISSURED = {
    'willow_tree': dict(cells=(3, 2, 3.0), keep_cross=0.5, jitter=0.9, hw=(8.0, 18.0), cross_k=0.5, D=18.0, p=0.5,
                        H=10.0, rag=(2.0, 4.0), s=3.0, rough=0.8, flake=2.5, crack=0.5, fib=0.25,
                        st=(6.0, 35.0, 4, 0.7, 0.12), sp1=(3.5, 25.0, 1.1, 2.0, 0.552), split_dark=0.15,
                        top=('#9a948a', '#908a80'), side=('#6e685e', '#645e54'), floor=('#2a2622', '#34302a'),
                        algae=0.25, susc=0.7, dome=0.9, wv=2.0, wall_dark=0.75, floor_t=0.45, wall_p=0.8, mottle=1.4,
                        ao_k=2.6, tone=0.15),
    'robinia': dict(cells=(3, 2, 3.8), keep_cross=0.4, jitter=0.9, hw=(4.0, 10.0), cross_k=0.5, D=18.0, p=0.75,
                    H=10.0, rag=(1.4, 3.5), s=3.0, rough=0.64, flake=2.5, crack=0.5, fib=0.0,
                    st=(4.0, 40.0, 4, 0.5, 0.1), sp1=(2.5, 16.5, 0.9, 1.6, 0.595), split_dark=0.15,
                    top=('#a49a88', '#988e7c'), side=('#6a604e', '#5e5444'), floor=('#4e3626', '#422e20'), algae=0.15,
                    susc=0.5, dome=0.9, wv=2.0, wall_dark=0.7, floor_t=0.5, mottle=1.4, ao_k=2.6, tone=0.15),
    'red_oak': dict(cells=(4, 1, 3.0), keep_cross=0.12, jitter=0.7, hw=(2.5, 6.0), cross_k=0.4, D=7.0, p=0.8, H=3.0,
                    rag=(1.0, 3.0), s=2.5, rough=0.225, flake=3.0, crack=0.3, st=(5.0, 14.0, 3, 0.3, 0.06),
                    sp1=(1.5, 7.0, 0.5, 0.6, 0.212), split_dark=0.15, top=('#a8a49c', '#a09c94'),
                    side=('#6e6a64', '#66625c'), floor=('#2e2a26', '#38322c'), top_k=0.5, wall_dark=0.65, floor_t=0.4,
                    algae=0.15, susc=0.5, dome=0.4, wv=1.4, ao_k=2.6, tone=0.15),
    'walnut': dict(cells=(4, 2, 2.6), keep_cross=0.3, jitter=0.9, hw=(3.5, 8.0), cross_k=0.45, D=14.0, p=0.75, H=7.0,
                   rag=(1.4, 3.0), s=2.5, rough=0.8, flake=2.0, crack=0.4, st=(3.0, 10.0, 5, 0.6, 0.12),
                   top=('#b4aca2', '#aaa298'), side=('#8a8278', '#80786e'), floor=('#3a322a', '#443a32'),
                   wall_dark=0.5, algae=0.15, susc=0.55, dome=0.85, wv=1.6, floor_t=0.7, mottle=1.4, ao_k=2.6,
                   tone=0.15),
    'box_elder': dict(cells=(6, 3, 3.4), keep_cross=0.2, jitter=0.8, hw=(3.5, 7.0), cross_k=0.45, D=5.0, p=0.8, H=3.0,
                      rag=(0.8, 2.5), s=2.0, rough=0.64, flake=1.6, crack=0.4, st=(2.5, 8.0, 5, 0.5, 0.1),
                      split_dark=0.3, top=('#a4988a', '#9a8e80'), side=('#7e7266', '#74685c'),
                      floor=('#3a322a', '#443a32'), wall_dark=0.6, algae=0.2, susc=0.5, dome=0.8, wv=1.6, floor_t=0.7,
                      wall_p=1.0, mottle=1.4, ao_k=2.6, tone=0.15),
    'lime': dict(cells=(2, 2, 2.6), keep_cross=0.3, jitter=0.9, hw=(3.0, 7.0), cross_k=0.45, D=8.0, p=0.8, H=4.0,
                 rag=(1.4, 3.0), s=2.5, rough=0.4, flake=2.2, crack=0.45, fib=0.0, st=(2.0, 12.0, 5, 0.5, 0.1),
                 sp1=(2.0, 12.0, 0.7, 1.2, 0.51), split_dark=0.15, top=('#8e8882', '#86807a'),
                 side=('#6e6862', '#646058'), floor=('#363230', '#403a36'), top_k=0.4, wall_dark=0.5, algae=0.25,
                 dome=0.8, wv=1.6, floor_t=0.7, stagger=True, mottle=1.4, ao_k=2.6, tone=0.15),
    'ash': dict(cells=(4, 3, 2.4), keep_cross=0.25, jitter=0.5, hw=(2.5, 5.0), cross_k=0.5, D=7.0, p=0.8, H=4.0,
                rag=(0.8, 2.5), s=2.0, rough=0.4, flake=1.4, crack=0.35, st=(2.0, 7.0, 4, 0.4, 0.1),
                split_dark=0.2, top=('#a8a49c', '#a09c94'), side=('#7a766e', '#706c64'), floor=('#4a463e', '#544e46'),
                top_k=0.6, wall_dark=0.5, floor_t=0.7, algae=0.25, susc=0.65, dome=0.9, wv=1.6, mottle=1.4, ao_k=2.6,
                tone=0.15),
    'plum': dict(cells=(4, 3, 2.6), keep_cross=0.6, jitter=0.8, hw=(1.6, 3.6), cross_k=0.6, D=5.0, p=0.8, H=2.5,
                 rag=(0.8, 2.5), s=1.8, rough=0.8, flake=1.6, crack=0.5, st=(3.0, 8.0, 5, 0.5, 0.14),
                 top=('#8a8076', '#7e746a'), side=('#4e443c', '#463c34'), floor=('#2a241e', '#322a24'), wall_dark=0.7,
                 algae=0.2, susc=0.5, dome=0.7, wv=1.4, floor_t=0.45, mottle=1.4, ao_k=2.6, tone=0.15),
    'birch_downy_base': dict(cells=(2, 2, 2.2), stagger=False, keep_cross=0.25, jitter=0.9, hw=(2.0, 5.0),
                             cross_k=0.55, D=5.0, p=0.8, H=2.0, rag=(1.2, 3.0), s=2.5, rough=0.35, flake=2.0,
                             crack=0.4, st=(10.0, 14.0, 2, 0.25, 0.06), island_len=True, len_n=20,
                             top=('#b0aea6', '#a4a29a'), side=('#8a8680', '#7e7a74'), floor=('#2c2826', '#363230'),
                             top_k=0.7, tone=0.15, wall_dark=0.6, floor_t=0.35, algae=0.12, susc=0.55, dome=0.6,
                             wv=1.3, mottle=0.6, ao_k=2.6, micro={'con': 0.2, 'grit': (0.14, 0.18)}),
}

# The flaking barks (flaking_bark): the flakes (dead_leaves: n of them, half size a mm, aspect, lobes lob over
# lob_s of the size, size spread size_sd), each thick mm, cap of them counted; the rim's lift (mm) over the outer
# rim of a flake, down (the lower edge only) or curl (a side's share), dome; swell; the surface (rough_bark at
# scale s); colours: the under-bark and the flakes (pairs), tone, the lifted edge's line edge_col / edge_k over
# the outer rim_c of the flake, the shadow line under it (shadow, its blur shadow_w and offset shadow_d), grey
# (colour, level, share) over patches grey_s mm, grey_flakes, light_flakes, fresh_col / fresh_k; scabs (islands of an
# older bark: tones, share, size mm), pits (count, radius range mm, colour, depth mm); micro (the crust at the texel
# scale, micro()'s arguments), ao_k and ao_s (the cavity's strength and scale).
TREE_FLAKING = {
    'pine_mirror': dict(s=2.0, n=160, a=20.0, aspect=0.7, lob=0.2, lob_s=0.35, size_sd=0.5, thick=0.15, cap=3.0,
                        lift=1.0, rim=0.25, down=0.35, curl=0.45, swell=0.12, rough=0.05, flake=1.2,
                        under=('#b85a34', '#9e4a2c'), cols=('#c8844c', '#a8603c'), tone=0.3, edge_col='#e0b48a',
                        edge_k=0.35, shadow=0.75, shadow_w=0.4, grey=('#b8694a', 0.3, 0.3), grey_s=4.0,
                        grey_flakes=('#a9a070', 0.05), light_flakes=0.35, light_col='#e6b07a', fresh_col='#c8683a',
                        fresh_k=0.4, gloss=0.3, susc=0.2, tint=0.05, rim_c=0.06, shadow_d=0.8, vary=0.05,
                        patches=((0.7, 0.56, 0.5), (1.25, 1.16, 1.02), 120.0, 1.0),
                        pits=(36, (0.6, 1.6), '#3a2416', 0.5)),
    'spruce': dict(s=2.0, n=96, a=15.0, aspect=0.85, lob=0.45, lob_s=0.2, size_sd=0.6, thick=0.9, cap=3.0, lift=2.5,
                   rim=0.45, down=0.9, curl=0.35, dome=0.6, swell=0.3, rough=0.4, flake=1.4,
                   under=('#4e3426', '#3e2a1e'), cols=('#b2a99a', '#857868'), tone=0.5, edge_col='#c4bcae',
                   edge_k=0.2, shadow=0.9, shadow_w=0.45, grey=('#a09a90', 0.4, 0.25), grey_s=12.0,
                   grey_flakes=('#b0aa9e', 0.1), fresh_col='#76523f', fresh_k=0.3, algae=0.18, susc=0.65, gloss=0.1,
                   rim_c=0.14, shadow_d=1.0, micro={'big': 0.0, 'con': 0.12}, ao_k=3.5, ao_s=2.0,
                   patches=((0.8, 0.76, 0.74), (1.15, 1.15, 1.13), 70.0, 1.0)),
    'sycamore': dict(s=3.0, n=33, a=26.0, aspect=1.2, lob=0.2, lob_s=0.35, size_sd=0.45, thick=1.5, cap=3.0, lift=2.1,
                     rim=0.2, down=0.0, curl=0.4, dome=0.2, swell=0.6, rough=0.5, flake=2.5,
                     under=('#b8907a', '#ac8470'), cols=('#8a847c', '#79736c'), tone=0.08, edge_col='#a49a8e',
                     edge_k=0.3, shadow=0.9, shadow_w=0.6, grey=('#9a968e', 0.6, 0.3), grey_s=40.0,
                     fresh_col='#a88e7c', fresh_k=0.5, algae=0.15, susc=0.5, gloss=0.14, rim_c=0.06, shadow_d=0.5,
                     micro={'big': 0.0, 'con': 0.2, 'grit': (0.14, 0.15)}, ao_k=3.0),
    'apple': dict(s=2.0, n=113, a=9.0, aspect=1.3, lob=0.3, lob_s=0.25, size_sd=0.45, thick=1.25, cap=3.0, lift=2.1,
                  rim=0.25, down=0.0, curl=0.5, dome=0.3, swell=0.6, rough=0.6, flake=1.6,
                  under=('#94684a', '#865c40'), cols=('#74695f', '#62584f'), tone=0.12, edge_col='#948a80', edge_k=0.3,
                  shadow=0.9, shadow_w=0.6, grey=('#8a847c', 0.5, 0.3), grey_s=30.0, fresh_col='#80644e', fresh_k=0.45,
                  algae=0.2, susc=0.6, gloss=0.1, rim_c=0.08, shadow_d=0.5, micro={'big': 0.0}, ao_k=3.5, ao_s=2.0),
}


# The scaly barks (scaly_bark): plates from fissured_plates (cells, keep_cross, jitter, warp1, warp2; the cracks'
# half width hw, cross_k, depth D) a share `gone` of them fallen, each `thick` mm, lifting by `lift` mm over lift_w mm
# of one side; the surface (rough_bark at scale s); colours: the plates (cols), the under-bark (fresh), the cracks'
# floor; grey patches, the lifted edge's line (edge_col, edge_k) and the shadow under it.
TREE_SCALY = {
    'horse_chestnut': dict(s=2.0, cells=(3, 4, 1.4), keep_cross=0.65, hw=(1.5, 4.0), cross_k=0.7, D=5.0, gone=0.08,
                           thick=1.0, lift=2.0, lift_w=6.0, rough=0.8, flake=1.6, cols=('#8c8072', '#786c60'),
                           tone=0.1, grey=('#9a958c', 0.5, 0.3), grey_s=35.0, fresh=('#7a6c60', '#6e6054'),
                           floor=('#2e241c', '#382c22'), edge_col='#a89a88', edge_k=0.35, shadow=0.85, algae=0.15,
                           susc=0.55, warp1=(10.0, 40.0), warp2=(3.0, 10.0), jitter=1.0,
                           micro={'big': 0.0, 'con': 0.25, 'grit': (0.16, 0.2)}, ao_k=3.5, ao_s=2.0),
}


def shingles(r, n, a_mm, aspect, lob=0.35, weight=None, size_sd=0.45, rot=0.25):
    """Thin scales lying over one another (a dead-leaves model, the pine's plates): n lobed ovals a_mm across
    (lognormal spread size_sd) and aspect times as long, thrown at random (the density by `weight`), each over the
    ones thrown before. Returns how many cover each texel (soft), the top one's index (-1 bare) and its centre's
    nearness (1 at the middle .. 0 at the edge)."""
    pts = r.random((n, 2)) * N
    if weight is not None:
        keep = r.random(n) < weight[pts[:, 1].astype(int) % N, pts[:, 0].astype(int) % N]
        pts = pts[keep]
    k = len(pts)
    A = a_mm * np.clip(r.lognormal(0.0, size_sd, k), 0.35, 2.6)
    B = A * aspect * r.uniform(0.8, 1.25, k)
    T = r.normal(0.0, rot, k)
    J = fbm(r, 0.22 * a_mm, 0.22 * a_mm * aspect, 3)
    cnt = np.zeros((N, N))
    top = np.full((N, N), -1, np.int64)
    mid = np.zeros((N, N))
    for i in range(k):
        x0, y0 = pts[i]
        ex = (max(A[i], B[i]) * (1.0 + 2.5 * lob) + 0.5) * MM
        xs = np.arange(int(np.floor(x0 - ex)), int(np.ceil(x0 + ex)) + 1)
        ys = np.arange(int(np.floor(y0 - ex)), int(np.ceil(y0 + ex)) + 1)
        idx = np.ix_(ys % N, xs % N)
        lx = ((xs + 0.5 - x0) / MM)[None, :]
        ly = ((ys + 0.5 - y0) / MM)[:, None]
        c, sn = np.cos(T[i]), np.sin(T[i])
        u, v = lx * c + ly * sn, -lx * sn + ly * c
        rho = np.sqrt((u / A[i]) ** 2 + (v / B[i]) ** 2) * (1.0 + lob * J[idx])
        cov = ss(1.0, 0.9, rho)
        cnt[idx] = cnt[idx] + cov
        win = cov > 0.5
        top[idx] = np.where(win, i, top[idx])
        mid[idx] = np.where(win, np.clip(1.0 - rho * rho, 0.0, 1.0), mid[idx])
    return cnt, top, mid


def crust(r, cell, contrast=0.15, step=0.12, crack=0.5, crack_amt=0.35):
    """The crisp roughness of a plate's surface at the texel scale: tiny flakes (Voronoi cells `cell` mm across,
    each its own brightness by `contrast` and height by `step` mm), hairline cracks between some of them. Returns
    the relief (mm) and a brightness factor."""
    nx = max(4, int(round(N / MM / cell)))
    W = cells(r, nx, max(2, int(round(nx / 1.3))), aspect=1.3, warp=warp_xy(r, 0.3 * cell, 0.8 * cell), R=1)
    lv = cattr(r, W, -1.0, 1.0)
    edge = ss(0.0, 0.12 * cell, W['e'])
    cut = (cattr(r, W) < crack).astype(np.float64)
    hcr = step * (lv + 0.8 * (edge - 1.0) * (0.4 + 0.6 * cut))
    br = np.exp(contrast * lv) * (1.0 - crack_amt * (1.0 - edge) * cut)
    return hcr, br


def micro(r, px=3.0, con=0.32, step=0.15, crack_amt=0.12, grit=(0.22, 0.25), big=0.4):
    """An old bark's crust at the texel scale, without lines: flakes about `px` texels across, each its own
    brightness (con) and height (step mm), faint cracks between some (crack_amt), and soft isotropic grit (fbm over
    1.5 and 4 flakes; grit: its brightness and relief, `big` the coarser octave's share). Returns the relief (mm)
    and a brightness factor."""
    cell = px / MM
    hc, bc = crust(r, cell, contrast=con, step=step, crack=0.5, crack_amt=crack_amt)
    g = (1.0 - big) * fbm(r, 1.5 * cell, 1.8 * cell, 3) + big * fbm(r, 4.0 * cell, 4.5 * cell, 2)
    return hc + grit[1] * g, bc * np.exp(grit[0] * g)


def t_pine_plates(r):
    """Pinus sylvestris, the lower trunk of an old pine (to a third of its height and more): long furrows 1-3 cm wide
    running along the stem, wandering, merging and parting in a net (grown as drying checks, crack_net), between them
    plates 4-15 cm wide and several times as long, each a domed block 1-2 cm thick broken across here and there by a
    short shallow crack; each plate a jigsaw of thin scales 2-4 cm lying over one another, their lobed edges thin rusty
    lines, pinkish-buff where fresh, the tops weathered grey, each plate its own cast (rustier or greyer); the plates'
    shoulders stepping down into the furrows in layers with rusty risers; the furrows' walls in strata of reddish-brown
    layers, darker down to a near-black floor; pale green-grey powdery lichen in the cracks and furrows."""
    # The furrows: long checks along the stem, wandering, joining older ones; the widest first.
    wr = crack_net(r, 10, length=(450.0, 900.0), width=(4.0, 8.0), step=1.5, wobble=0.12, pull=0.06, tilt0=0.3,
                   join=24.0, taper=0.8, meet=True)
    sd, w = net_sd(wr, rg=(1.6, 6.0), r=r)
    # (The plates' edges jagged along the fibres rather than rounded.)
    sd = sd + 1.2 * fbm(r, 2.0, 7.0, 2)
    hw = 0.5 * np.maximum(w, 3.5)
    # The plates' blocks: a few short cracks a plate across the stem, ending in the furrows.
    cr = cross_breaks(r, 16, (35.0, 80.0), (3.0, 6.0))
    # The plate's own frame for its tone and tilt: a low noise a plate wide.
    pt = fbm(r, 30.0, 70.0, 2)
    pa = fbm(r, 25.0, 60.0, 2)
    rmax = 30.0
    u = np.clip(sd / rmax, 0.0, 1.0)
    dome = 1.0 - (1.0 - u) ** 2.0
    q = (sd + 1.2 * fbm(r, 3.0, 5.0, 3)) / 2.0
    _, hh, riser = stack(q, 4, sharp=0.3)
    inner = ss(3.0, 5.0, q)
    cnt, top, mid = shingles(r, 200, 18.0, 1.3, lob=0.5, weight=ss(1.0, 8.0, sd))
    cnt = np.minimum(cnt, 6.0)
    hcr, br = crust(r, 2.6, contrast=0.06, step=0.08, crack=0.4, crack_amt=0.2)
    thick = 12.0 + 4.0 * pa
    base = np.where(sd > 0.0, thick * dome * (0.6 + 0.4 * ss(0.0, 1.0, hh / 4.0)),
                    vwall(sd, 1.6 * hw + 7.0, hw + 0.6, p=0.7))
    h = base + inner * (0.8 * cnt + 0.15 * mid) + hcr * ss(-0.5, 0.5, sd) - 3.0 * cr * ss(-0.5, 1.5, sd)
    h = h + 1.2 * fbm(r, 40.0, 90.0, 2)
    # Colour: the scales pinkish-buff where fresh, the tops weathered grey; each plate its own cast (rustier or greyer)
    # by the plate-wide noise; the slopes toward the furrow rusty.
    sc = np.where(inner > 0.5, hash01(top * 1.0 + 0.5), 0.5 + 0.5 * pt)
    pink = mix(np.broadcast_to(C('#a88a7a'), (N, N, 3)), C('#c4a492'), sc)
    grey = mix(np.broadcast_to(C('#86807a'), (N, N, 3)), C('#a39c94'), hash01(top * 1.0 + 0.9))
    old = ss(1.2, 4.0, cnt + 1.5 * (sc - 0.5) + 1.2 * pt) * inner
    alb = mix(pink, grey, np.clip(0.85 * old + 0.3 * dome, 0.0, 0.95))
    alb = vary(alb, 0.05, fbm(r, 4.0, 6.0, 3))
    alb = mix(alb, alb * np.array([1.2, 0.95, 0.78]), np.clip(0.7 * pa + 0.15, 0.0, 0.65))
    gx, gy = grad_mm(blur(cnt, 0.25))
    edge = np.clip(np.sqrt(gx * gx + gy * gy) * 0.45, 0.0, 1.0) * inner
    alb = mix(alb, C('#9c5c3c'), 0.55 * edge)
    alb = mix(alb, C('#a8603c'), 0.45 * riser * (1.0 - inner))
    alb = mix(alb, C('#8a5038'), 0.35 * ss(6.0, 1.0, sd) * (1.0 - inner))
    # The transverse cracks dark, out to the furrows' rims.
    alb = mix(alb, C('#3a2a20'), 0.55 * ss(0.3, 0.9, cr) * ss(-0.5, 1.5, sd))
    alb = alb * br[..., None]
    dt = np.clip(-sd / (hw + 0.6), 0.0, 1.0)
    layers = 0.5 + 0.5 * np.sin(-sd * 2.2 + 3.0 * fbm(r, 2.0, 8.0, 2))
    wall = mix(np.broadcast_to(C('#74503c'), (N, N, 3)), C('#4c3428'), layers * 0.6)
    wall = mix(wall, C('#36281e'), ss(0.4, 1.0, dt))
    alb = mix(alb, wall, ss(0.6, -0.6, sd))
    lp = ss(0.3, 1.2, fbm(r, 10.0, 18.0, 3)) * np.clip(ss(3.0, -1.0, sd) + 0.8 * blur(edge, 0.6), 0.0, 1.0)
    lp = lp * ss(-0.5, 0.8, fbm(r, 0.8, 1.0, 2))
    alb = mix(alb, C('#a4ae98'), 0.35 * lp)
    res = dict(h=h + 0.2 * lp, alb=alb)
    res.update(gloss=0.1 + 0.05 * (1.0 - old), susc=0.5, nstr=1.0, ao_s=2.0, ao_k=2.5, tint=0.05, fresh=None)
    hm, bm = micro(r, big=0.0, con=0.16)
    res['alb'] = res['alb'] * bm[..., None]
    res['h'] = res['h'] + hm
    return res


def t_pine_mirror(r):
    """Pinus sylvestris, the upper trunk and the limbs: thin papery 'mirror' bark, orange to reddish-orange,
    peeling in irregular thin flakes 1-4 cm whose edges curl up, lighter, each over a thin line of shadow on the
    layer beneath; a few whole flakes weathered grey-buff, a few lighter ones lifting."""
    return flaking_bark(r, TREE_FLAKING['pine_mirror'])


def t_spruce(r):
    """Picea abies, an old trunk (photo pack pic_abi): pale grey to grey-brown rounded scales 1.5-4 cm of many sizes
    overlapping downward like roof tiles, their lower edges lifting over a dark shadow, the dark reddish-brown
    under-bark in the gaps where they flaked (small matte scales over a mid brown read as suede); greyer and browner
    patches a hand across, a little algae."""
    return flaking_bark(r, TREE_FLAKING['spruce'])


def t_birch_base(r):
    """Betula pendula, the base of an old trunk (to 1-5 m; photo pack bet_pen): thick black-brown corky ridges, very
    rough, split along and cracked across, rising out of the white bark in a coarse net of long ridges that merge and
    part round tall lens-shaped islands of the white bark, smooth with its dark transverse lenticels, grey and dirty
    toward the cork. (Upward the white takes the islands' rims first: the ridges stay as dark lenses in the white.)"""
    F = fissured_plates(r, 5, 3, 3.5, (8.0, 60.0), (1.5, 12.0), 0.97, 0.25, (9.0, 20.0), 0.45, (1.5, 6.0), jitter=1.0)
    sd, inv, n = F['sd'], F['inv'], F['n']
    rng = np.random.default_rng(int(r.integers(1 << 30)))
    pa = rng.normal(0.0, 1.0, n)[inv]
    # The cork: the net along the cells' borders (their "fissures"), rising from its edge over 4 mm to 5-8 mm proud,
    # its surface rough, split along its middle and the fibres, cracked across; the white islands between smooth, a
    # little wrinkled across.
    cork = ss(0.8, -0.8, sd)
    rise = 1.0 - (1.0 - np.clip(-sd / 3.0, 0.0, 1.0)) ** 2
    hr, br = rough_bark(r, 2.5, 1.3, flake=2.6, crack=0.6)
    hs, ts = splits(r, 1.6, 7.0, 0.7, 2.0, 0.5)
    cr = cross_cracks(r, 5.0, 1.0, 0.7, 0.4)
    mid = ss(0.35, 0.0, np.clip(-sd / np.maximum(F['hw'], 1.0), 0.0, 2.0) - 0.95) * cork
    h_cork = (6.0 + 1.5 * pa) * rise + hr + hs - 2.2 * cr - 1.0 * mid
    h_white = 0.3 * fbm(r, 6.0, 10.0, 3) + 0.03 * lines(nz(r, 6.0, 0.3), 0.4)
    h = cork * h_cork + (1.0 - cork) * h_white + 0.6 * fbm(r, 30.0, 60.0, 2)
    # Colour: the cork black-brown, its crowns weathered greyer, its splits and cracks black; the white bark chalky,
    # dirty grey toward the cork, its lenticels dark dashes.
    corkc = tbase(r, '#332e2a', '#29241f', 6.0, 14.0, s=2.5)
    corkc = mix(corkc, C('#3e342c'), 0.4 * ss(-0.5, 1.0, fbm(r, 8.0, 20.0, 3)))
    corkc = mix(corkc, C('#655e57'), 0.7 * ss(0.3, 1.0, rise) * ss(-0.7, 0.9, fbm(r, 3.0, 6.0, 3) + 0.4 * pa))
    corkc = mix(corkc, C('#141210'), 0.7 * np.maximum(np.maximum(ts, cr), mid)) * br[..., None]
    white = tbase(r, '#c6c3bb', '#b4b1a9', 15.0, 8.0, streak=0.03, grain=0.03, s=2.0)
    white = mix(white, C('#8a8780'), 0.45 * ss(-0.4, 1.2, fbm(r, 4.0, 3.0, 3)))
    white = mix(white, C('#6e6b65'), 0.7 * ss(6.0, 0.0, sd))
    L = birch_lenticels(r, 60, 9.0, (16.0, 8.0))
    lc = mix(np.broadcast_to(C('#4a403a'), (N, N, 3)), C('#2e2622'), L['tone'])
    white = mix(white, lc, 0.85 * L['cov'])
    alb = mix(white, corkc, cork)
    res = tgrain(r, dict(h=h + (L['h'] - 0.15 * L['cov']) * (1.0 - cork),
                         alb=algae(r, alb, 0.12, where=cork, s=10.0)), 0.02,
                 0.04, scale=2.0, aspect=1.4)
    res.update(gloss=0.08 + 0.1 * (1.0 - cork), susc=0.55, nstr=0.9, ao_s=1.2, ao_k=2.2, tint=0.08, fresh=None)
    return res


def t_birch_downy_base(r):
    """Betula pubescens, the base of an old trunk: greyer and less furrowed than the silver birch's, shallow dark
    fissures between patches of whitish-grey bark with a few dark lenticels."""
    return fissured_bark(r, TREE_FISSURED['birch_downy_base'])


def t_aspen_base(r):
    """Populus tremula and P. alba, the foot of an old trunk (photo packs pop_tre, pop_alb): long dark furrows 1-3 cm
    wide wandering up the stem, merging and parting (grown as drying checks, crack_net: a Voronoi net of lens-shaped
    cells read as a woven diamond net, its small cracks as ladders of dashes under a high sun), between them broad
    rough grey ridges flaky at a few millimetres, broken across here and there by a short shallow crack; the furrows'
    floors near black, their walls dark grey-brown; algae in the furrows."""
    wr = crack_net(r, 10, length=(180.0, 420.0), width=(14.0, 30.0), step=1.5, wobble=0.14, pull=0.06, tilt0=0.25,
                   join=14.0, taper=0.4)
    sd, w = net_sd(wr, rg=(2.0, 5.0), r=r)
    hw = 0.5 * np.maximum(w, 3.0)
    cr = cross_cracks(r, 16.0, 10.0, 1.0, 0.12)
    pa = fbm(r, 25.0, 60.0, 2)
    u = np.clip(sd / 25.0, 0.0, 1.0)
    dome = 1.0 - (1.0 - u) ** 2
    hr, br = rough_bark(r, 2.5, 0.8, flake=2.2, crack=0.3)
    base = np.where(sd > 0.0, 6.0 * dome, vwall(sd, 1.4 * hw + 6.0, hw + 0.6, p=0.75))
    h = base + hr * ss(-0.5, 1.0, sd) - 2.0 * cr * ss(2.0, 6.0, sd) + 1.0 * fbm(r, 40.0, 90.0, 2)
    top = tbase(r, '#7a766e', '#6c6862', 8.0, 20.0, s=2.5)
    side = tbase(r, '#4c4842', '#423e38', 8.0, 20.0, s=2.5)
    alb = mix(side, top, 0.7 * ss(0.2, 0.9, dome) * ss(-0.5, 1.2, fbm(r, 3.0, 6.0, 3) + 0.4 * pa))
    alb = alb * np.exp(0.12 * pa)[..., None]
    dt = np.clip(-sd / (hw + 0.6), 0.0, 1.0)
    wall = mix(np.broadcast_to(C('#4a4640'), (N, N, 3)), C('#161412'), ss(0.2, 0.9, dt))
    alb = mix(alb, wall, ss(0.6, -0.6, sd))
    alb = mix(alb, C('#262220'), 0.6 * ss(0.3, 0.9, cr) * ss(2.0, 6.0, sd))
    alb = alb * br[..., None]
    res = dict(h=h, alb=algae(r, alb, 0.25, where=0.35 + 0.65 * ss(0.5, -2.0, sd), s=15.0))
    res.update(gloss=0.1, susc=0.6, nstr=0.9, ao_s=1.5, ao_k=2.6, tint=0.25, tint_s=3.0, fresh=None)
    hm, bm = micro(r, big=0.0, con=0.2)
    res['alb'] = res['alb'] * bm[..., None]
    res['h'] = res['h'] + hm
    return res


def t_poplar_black(r):
    """Populus nigra and P. x canadensis, an old trunk (photo packs pop_nig, pop_can): deep near-black furrows 1-2 cm
    wide interlacing round thick grey ridges 4-7 cm wide, long and leaning a little each way (an X-net, not lozenges of
    one size: those read as wallpaper and repeated), their surface layered and split along the fibres; algae in the
    furrows."""
    return furrowed_bark(r, TREE_FURROWED['poplar_black'])


def furrowed_bark(r, P):
    """A furrowed bark (TREE_FURROWED: the oak, the black poplar, the alder): long furrows along the stem grown as
    drying checks, each running on until it meets an older one (crack_net: a net round long lens-shaped ridges, not
    strokes ending side by side), the widest first; the ridges' edges jagged along the fibres, broken across here and
    there by a short crack reaching the furrows (cross_breaks); the ridges flat-topped with steep shoulders, their tops
    very rough (layered flakes, lumps) and grey, the flanks darker; the furrows' walls darker down to a near-black
    floor; lichen and algae on the ridges."""
    wr = crack_net(r, P['n'], length=P['length'], width=P['width'], step=1.5, wobble=P['wobble'], pull=P['pull'],
                   tilt0=P['tilt0'], join=P['join'], taper=P['taper'], meet=True)
    sd, w = net_sd(wr, rg=P['rg'], r=r)
    sd = sd + P['jag'] * fbm(r, 2.5, 10.0, 2)
    hw = 0.5 * np.maximum(w, P['hw_min'])
    bn, bl, bw = P['breaks']
    cr = cross_breaks(r, bn, bl, bw) * ss(-0.5, 1.5, sd)
    pa = fbm(r, 20.0, 50.0, 2)
    # The ridge's section: a flat top, a steep shoulder from the rim; the furrow a U down to its floor.
    sh = 1.0 - (1.0 - np.clip(sd / P['shoulder'], 0.0, 1.0)) ** 2
    hr, br = rough_bark(r, 3.0, P['rough'], flake=2.2, crack=0.5)
    hs, lv, rs = strata(r, *P['strata'])
    on = ss(-1.0, 1.5, sd)
    base = np.where(sd > 0.0, (P['H'] + 3.0 * pa) * sh, vwall(sd, P['D'], hw + 0.3, p=0.5))
    h = base + on * (hr + hs + 2.0 * fbm(r, 6.0, 10.0, 2)) - 5.0 * cr + 0.8 * fbm(r, 35.0, 70.0, 2)
    # Colour: the tops grey, weathered paler, the flanks darker, each ridge and layer its own tone, the layers' edges
    # fresher; the furrows' walls darker down to a near-black floor; the breaks dark.
    side = tbase(r, P['side'][0], P['side'][1], 30.0, 75.0, s=3.0)
    top = tbase(r, P['top'][0], P['top'][1], 18.0, 45.0, s=3.0)
    alb = mix(side, top, 0.8 * ss(0.3, 1.0, sh) * ss(-0.5, 1.3, fbm(r, 9.0, 18.0, 3) + 0.4 * pa))
    alb = alb * np.exp(0.15 * pa + 0.1 * (2.0 * hash01(lv * 1.0 + 0.37) - 1.0))[..., None]
    alb = mix(alb, C(P['top'][0]), 0.2 * rs * on)
    dt = np.clip(-sd / (hw + 0.3), 0.0, 1.0)
    alb = alb * (1.0 - 0.6 * dt ** 0.8)[..., None]
    alb = mix(alb, tbase(r, P['floor'][0], P['floor'][1], 12.0, 36.0, s=3.0), ss(0.6, 1.0, dt))
    alb = mix(alb, C('#262220'), 0.35 * ss(0.2, 0.8, cr))
    alb = alb * br[..., None]
    alb = algae(r, alb, P['algae'], where=0.35 + 0.65 * ss(0.5, -2.0, sd), s=18.0)
    res = dict(h=h, alb=mottle(r, alb, 1.2, 3.0))
    hm, bm = micro(r)
    res['alb'] = res['alb'] * bm[..., None]
    res['h'] = res['h'] + hm
    res.update(gloss=0.1, susc=P['susc'], nstr=0.6, ao_s=1.0, ao_k=3.0, tint=0.3, tint_s=3.0, fresh=None)
    return res


# The furrowed barks (furrowed_bark): the furrows (crack_net: n of them, length and width in mm, wobble, pull, tilt0,
# join, taper; rg the rims' raggedness, jag their fibres' jitter in mm, hw_min the narrowest wall's width), the breaks
# across (count, length, width in mm), the ridges (H mm tall over a `shoulder` mm wide shoulder, rough), the furrows D
# mm deep, the strata (sx, sy mm, levels, step mm); colours: the ridges' tops and flanks, the furrows' floor (pairs);
# algae, the lichen's susceptibility susc.
TREE_FURROWED = {
    'oak': dict(n=17, length=(300.0, 700.0), width=(4.5, 8.0), wobble=0.07, pull=0.08, tilt0=0.12, join=20.0,
                taper=0.8, rg=(1.6, 8.0), jag=1.0, hw_min=3.5, breaks=(20, (18.0, 36.0), (4.0, 7.0)), shoulder=5.0,
                H=12.0, D=16.0, rough=0.9, strata=(9.0, 14.0, 3, 0.6), top=('#a29e96', '#96928a'),
                side=('#6c6660', '#625c56'), floor=('#1c1814', '#241f1a'), algae=0.25, susc=0.7),
    'poplar_black': dict(n=14, length=(500.0, 1000.0), width=(16.0, 28.0), wobble=0.14, pull=0.05, tilt0=0.35,
                         join=22.0, taper=0.7, rg=(2.0, 9.0), jag=1.4, hw_min=10.0,
                         breaks=(10, (25.0, 50.0), (4.0, 8.0)), shoulder=7.0, H=16.0, D=22.0, rough=1.2,
                         strata=(8.0, 30.0, 3, 0.8), top=('#7a766e', '#706c64'), side=('#4c4842', '#44403a'),
                         floor=('#161412', '#1e1b18'), algae=0.3, susc=0.65),
    'alder': dict(n=16, length=(60.0, 180.0), width=(3.5, 6.5), wobble=0.12, pull=0.08, tilt0=0.25, join=10.0,
                  taper=0.6, rg=(1.2, 5.0), jag=0.8, hw_min=3.0, breaks=(26, (10.0, 26.0), (2.0, 4.5)), shoulder=3.0,
                  H=6.0, D=9.0, rough=0.6, strata=(4.0, 8.0, 3, 0.4), top=('#8a857a', '#7e796e'),
                  side=('#5c574e', '#544f46'), floor=('#2a241e', '#322a24'), algae=0.3, susc=0.7),
}


def t_oak(r):
    """Quercus robur, an old trunk (photo pack que_rob): long furrows along the stem 0.8-1.4 cm wide and 2-3 cm deep,
    wandering a little, merging and parting; between them long grey ridges 2-4 cm wide, their edges jagged along the
    fibres, broken across here and there by a short crack; their tops very rough and grey, the flanks darker, the
    furrows' floors near black."""
    return furrowed_bark(r, TREE_FURROWED['oak'])


def t_red_oak(r):
    """Quercus rubra: wide flat-topped smooth light grey ridges running long along the stem ('ski tracks') between
    shallow darker fissures."""
    return fissured_bark(r, TREE_FISSURED['red_oak'])


def t_sycamore(r):
    """Acer pseudoplatanus, an old trunk: smooth grey to pinkish-grey bark flaking off in irregular plates 3-8 cm,
    the smoother buff to pinkish-orange under-bark where a plate fell, dark under the plates' lifting edges."""
    return flaking_bark(r, TREE_FLAKING['sycamore'])


def t_box_elder(r):
    """Acer negundo: light grey-brown bark, shallowly fissured into narrow interlacing ridges."""
    return fissured_bark(r, TREE_FISSURED['box_elder'])


def t_lime(r):
    """Tilia cordata, an old trunk: grey-brown bark with shallow fissures in a net round flat fibrous ridges 1-2 cm
    wide that run far along the stem and interlace."""
    return fissured_bark(r, TREE_FISSURED['lime'])


def t_ash(r):
    """Fraxinus excelsior and F. pennsylvanica, an old trunk: grey bark in a regular diamond net of narrow
    interlacing ridges round furrows 1-2 cm wide and 3-5 cm long."""
    return fissured_bark(r, TREE_FISSURED['ash'])


def t_alder(r):
    """Alnus glutinosa, an old trunk (photo pack aln_glu): olive-grey bark broken into plates 2-5 cm wide and taller
    than wide with rough layered tops between dark fissures (small dark squares of one size read as dried mud)."""
    return furrowed_bark(r, TREE_FURROWED['alder'])


def t_willow_tree(r):
    """Salix alba and S. x fragilis, an old trunk: grey-brown bark deeply fissured criss-cross, very coarse thick
    corky ridges interlacing round deep furrows, layered and split along the fibres; algae in the furrows."""
    return fissured_bark(r, TREE_FISSURED['willow_tree'])


def t_robinia(r):
    """Robinia pseudoacacia: thick grey-brown bark, deeply furrowed, long rope-like fibrous ridges interlacing round
    deep furrows whose floor shows the orange-brown inner bark."""
    return fissured_bark(r, TREE_FISSURED['robinia'])


def t_horse_chestnut(r):
    """Aesculus hippocastanum, an old trunk: lighter grey-brown bark cracked into thin irregular plates 3-8 cm,
    longer than wide, their edges curling up over a thin shadow, the browner under-bark where a plate fell."""
    return scaly_bark(r, TREE_SCALY['horse_chestnut'])


def t_apple(r):
    """Malus domestica, an old trunk: grey-brown scaly bark of thin irregular plates 1-4 cm curling at their edges
    and flaking, the orange-brown under-bark where they fell."""
    return flaking_bark(r, TREE_FLAKING['apple'])


def t_cherry_trunk(r):
    """Prunus cerasus, a trunk: smooth glossy red-brown bark ringed by bands of transverse lenticels 1-2.5 cm, the
    outer skin peeling in horizontal curling strips, grey-brown where weathered."""
    h = tsmooth(r, 0.2, 2.0)
    alb = tbase(r, '#6a4840', '#5a3c36', 14.0, 25.0, streak=0.04, grain=0.04, s=1.8)
    alb = mix(alb, C('#6e6058'), 0.3 * ss(0.4, 1.6, fbm(r, 12.0, 8.0, 3)))
    res = tgrain(r, dict(h=h, alb=alb), 0.01, 0.035, scale=1.5, aspect=0.4)
    # Lenticel bands: rows of transverse lenticels a few centimetres apart, each a band of a few rows.
    rows = 7
    ys = (np.arange(rows) + r.uniform(-0.25, 0.25, rows)) * N / rows
    pts = []
    for y0 in ys:
        for sub in range(int(r.integers(1, 3))):
            x = r.uniform(0, N)
            end = x + N
            yy = y0 + sub * r.uniform(2.0, 4.0) * MM
            while x < end - 8.0 * MM:
                pts.append((x % N, (yy + r.normal(0, 0.5) * MM) % N))
                x += r.uniform(12.0, 24.0) * MM
    L = lenticel_layer(r, 0, None, 13.0, 0.35, (0.07, 0.12), 0.14, 0.08, Lmin=6.0, Lmax=26.0, Wmin=0.8, Wmax=2.2,
                       ang_sd=0.025, pts=np.array(pts))
    res['h'] = res['h'] + L['h']
    alb = paint_lenticels(res['alb'], L, '#9a8270', '#867060', '#2a1c18', slit_amt=0.75)
    # The outer skin: horizontal strips curling at their torn edges, a thin shadow under each lifted edge.
    m, sd = film(r, 0.32, 40.0, 5.0, jag=0.45)
    skin = vary(field('#6e625a', '#625650', ss(-1, 1, fbm(r, 10.0, 6.0, 3))), 0.06, fbm(r, 2.5, 0.6, 3))
    res.update(h=res['h'] + 0.08 * m, alb=mix(alb, skin, m), gloss=0.65 * (1.0 - m) + 0.22 * m - 0.4 * L['cov'],
               susc=0.2)
    peel_s(r, res, m, sd, 0.5, 1.2, '#a49488', 0.45, 0.5, 1.5)
    res['tint'] = 0.05
    return res


def t_plum(r):
    """Prunus domestica, an old trunk: dark grey-brown bark, fissured and flaking in small plates."""
    return fissured_bark(r, TREE_FISSURED['plum'])


def t_walnut(r):
    """Juglans regia, an old trunk: silver-grey bark, deep dark fissures in a vertical net round long flat-topped
    ridges, layered and split."""
    return fissured_bark(r, TREE_FISSURED['walnut'])


def crack_net(r, n, length=(80.0, 300.0), width=(4.0, 12.0), step=1.5, wobble=0.08, pull=0.04, tilt0=0.25,
              join=5.0, taper=0.35, angle=0.0, meet=False):
    """Cracks grown like drying checks: n seeds in turn (jittered on a grid), each crack growing both ways from its
    seed along one line (`angle` radians off the stem's axis, wandering by `wobble` a step, pulled back by `pull`, set
    off by up to `tilt0`) until it reaches its length or comes within `join` mm of an older crack or of its own older
    part (round the torus: a crack longer than the tile meets itself), where it stops - or with `meet` runs on into it,
    curving from its own direction to the other's nearest texel (a net of furrows that merge and part, not a row of
    strokes ending side by side). The first cracks are the widest (width: the last one's and the first one's, mm),
    each tapering to `taper` of its width at a free tip. Returns the centre lines' raster (their width at each texel, 0
    off them)."""
    occ = np.zeros((N, N), bool)
    wr = np.zeros((N, N))
    jr = max(1, int(round(join * MM)))
    dd = np.arange(-jr, jr + 1)
    disc = (dd[:, None] ** 2 + dd[None, :] ** 2) <= jr * jr
    dyo, dxo = np.nonzero(disc)
    dyo, dxo = dyo - jr, dxo - jr
    gx = max(1, int(round(np.sqrt(n * 0.6))))
    gy = max(1, int(np.ceil(n / gx)))
    cx, cy = np.meshgrid(np.arange(gx), np.arange(gy))
    pts = np.stack([(cx.ravel() + r.random(gx * gy)) * N / gx, (cy.ravel() + r.random(gx * gy)) * N / gy], -1)
    pts = pts[r.permutation(len(pts))[:n]]
    sp = step * MM
    # (A point of the crack's own path counts once the tip is twice the join zone past it, and never within that of the
    # seed: the other half starts there.)
    lag = int(np.ceil(2.0 * jr / sp)) + 2

    def cell(x, y):
        return int(np.floor(y)) % N, int(np.floor(x)) % N
    for c, (x0, y0) in enumerate(pts):
        if occ[cell(x0, y0)]:
            continue
        L = r.uniform(*length) * MM
        w = width[1] - (width[1] - width[0]) * c / max(len(pts) - 1, 1)
        w *= r.uniform(0.8, 1.2)
        th0 = r.normal(angle, tilt0)
        mine = np.zeros((N, N), bool)
        own = []

        def mark(pt):
            iy, ix = cell(*pt)
            mine[(iy + dyo) % N, (ix + dxo) % N] = True
            own.append(pt)
        halves, ends = [], []
        for sgn in (1.0, -1.0):
            x, y, th = x0, y0, th0
            seg = [(x, y)]
            run = 0.0
            hit = None
            while run < 0.5 * L:
                th = th + r.normal(0.0, wobble)
                th = th - pull * (th - angle)
                x += sgn * sp * np.sin(th)
                y += sgn * sp * np.cos(th)
                run += sp
                seg.append((x, y))
                if len(seg) > 2 * lag:
                    mark(seg[-1 - lag])
                if occ[cell(x, y)]:
                    hit = 'old'
                    break
                if mine[cell(x, y)]:
                    hit = 'own'
                    break
            if hit and meet:
                target = None
                if hit == 'old':
                    rad = jr + 2
                    fy, fx = int(np.floor(y)), int(np.floor(x))
                    win = wr[np.ix_((fy + np.arange(-rad, rad + 1)) % N, (fx + np.arange(-rad, rad + 1)) % N)] > 0.0
                    if win.any():
                        oy, ox = np.nonzero(win)
                        k = np.argmin((oy - rad) ** 2 + (ox - rad) ** 2)
                        target = (fx + ox[k] - rad + 0.5, fy + oy[k] - rad + 0.5)
                else:
                    # (Its own older part, the nearest point round the torus, in this half's unwrapped frame.)
                    O = np.array(own)
                    d = (O - (x, y) + 0.5 * N) % N - 0.5 * N
                    k = np.argmin((d ** 2).sum(axis=1))
                    target = (x + d[k, 0], y + d[k, 1])
                if target:
                    tx, ty = target
                    dist = np.hypot(tx - x, ty - y)
                    bx, by = x + sgn * 0.5 * dist * np.sin(th), y + sgn * 0.5 * dist * np.cos(th)
                    for q in np.linspace(0.0, 1.0, max(2, int(dist / sp) + 2))[1:]:
                        seg.append(((1 - q) ** 2 * x + 2 * (1 - q) * q * bx + q * q * tx,
                                    (1 - q) ** 2 * y + 2 * (1 - q) * q * by + q * q * ty))
            # (The first half whole for the second, but for its stretch by the seed.)
            if sgn > 0.0:
                for pt in seg[max(lag, len(seg) - lag):]:
                    mark(pt)
            halves.append(seg)
            ends.append(hit is not None)
        # (From the first half's end through the seed to the second's: a free tip tapers, an end that met a crack stays
        # open.)
        P = np.array(halves[0][::-1] + halves[1][1:])
        s = np.linspace(0.0, 1.0, len(P))
        lo, hi = (1.0 if e else taper for e in ends)
        wt = w * np.minimum(lo + (1.0 - lo) * ss(0.0, 0.3, s), hi + (1.0 - hi) * ss(1.0, 0.7, s))
        # Rasterised densely (half-texel steps), the join zone marked round it.
        tot = np.concatenate([[0.0], np.cumsum(np.hypot(np.diff(P[:, 0]), np.diff(P[:, 1])))])
        u = np.arange(0.0, tot[-1] + 1e-6, 0.5)
        ix = np.floor(np.interp(u, tot, P[:, 0])).astype(int) % N
        iy = np.floor(np.interp(u, tot, P[:, 1])).astype(int) % N
        wr[iy, ix] = np.maximum(wr[iy, ix], np.interp(u, tot, wt))
        for k in range(0, len(ix), max(1, jr // 2)):
            occ[(iy[k] + dyo) % N, (ix[k] + dxo) % N] = True
    return wr


def cross_breaks(r, n, length, width):
    """Short cracks across the stem (crack_net at a right angle: n of them, length and width in mm), each 1 on its
    centre line falling to 0 at its edge: the breaks of a ridge or plate into blocks."""
    wx = crack_net(r, n, length=length, width=width, step=1.0, wobble=0.12, pull=0.1, tilt0=0.25, join=3.0, taper=0.3,
                   angle=0.5 * np.pi)
    sd, w = net_sd(wx, rg=(0.6, 2.0), r=r)
    return np.clip(-sd / np.maximum(0.5 * w, 0.5), 0.0, 1.0) ** 0.7


def net_sd(wr, rg=(1.0, 3.5), r=None):
    """The signed distance (mm) to the edge of the cracks drawn by crack_net (< 0 inside), and the local width."""
    d, qx, qy = edt(wr > 0.0, seeds=True)
    w = wr[qy, qx]
    sd = d - 0.5 * w
    if r is not None and rg[0]:
        sd = sd + rg[0] * (fbm(r, rg[1], rg[1] * 1.3, 3) + 0.4 * fbm(r, 0.3 * rg[1], 0.35 * rg[1], 2))
    return sd, w


def t_deadwood_bark(r):
    """A standing dead pine some years on: its plated bark bleached grey and loosening, a third of the plates fallen
    off, the silver-grey weathered wood under them, the remaining plates' edges lifting off the wood."""
    P = fissured_plates(r, 5, 3, 2.4, (12.0, 70.0), (3.0, 14.0), 1.0, 0.35, (2.0, 6.0), 0.4, (1.2, 3.5), jitter=1.0)
    sd, hw, inv, n = P['sd'], P['hw'], P['inv'], P['n']
    pid = r.random(n)[inv]
    gone = (pid < 0.35).astype(np.float64)
    keep = 1.0 - gone
    q = (sd + 1.6 * fbm(r, 3.0, 5.0, 3) + 0.6 * fbm(r, 1.0, 1.4, 2)) / 2.8
    L, hh, riser = stack(q, 3, sharp=0.2)
    hr, br = rough_bark(r, 3.0, 1.0, flake=2.0, crack=0.5)
    wh = -0.12 * (lines(nz(r, 0.35, 25.0), 0.35) + 0.7 * lines(nz(r, 0.8, 40.0), 0.35)) + 0.2 * fbm(r, 2.0, 30.0, 3)
    plate = np.where(sd > 0.0, 1.4 * hh, vwall(sd, 1.4 * hw + 3.0, hw + 0.6, 0.85)) + hr * ss(-1.5, 1.0, sd)
    # (A fallen plate leaves the wood 4-6 mm below the plates round it; their edges lift off it.)
    lift = 1.5 * ss(4.0, 0.0, sd) * keep
    h = keep * (plate + lift) + gone * (wh - 5.0) + 0.8 * fbm(r, 35.0, 70.0, 2)
    pl = tbase(r, '#8c887e', '#7e7a70', 20.0, 50.0, s=3.0) * np.exp(0.08 * r.normal(0, 1, n)[inv])[..., None]
    pl = mix(pl, C('#6e6458'), 0.3 * ss(2.5, 0.5, L) * (L < 3))
    dt = np.clip(-sd / np.maximum(hw + 0.6, 0.5), 0.0, 1.0)
    pl = pl * (1.0 - 0.35 * ss(0.0, 1.0, dt))[..., None]
    pl = mix(pl, tbase(r, '#2e2a26', '#3a3632', 6.0, 20.0, s=3.0), ss(0.6, 1.0, dt))
    wood = tbase(r, '#a49c90', '#b2ab9f', 10.0, 50.0, streak=0.07, s=2.0)
    wood = mix(wood, C('#6e685e'), 0.35 * ss(0.4, 1.6, fbm(r, 3.0, 70.0, 3)))
    # (The wood in the plates' shadow near their edges.)
    wood = wood * (1.0 - 0.45 * ss(-6.0, 0.0, -edt(keep > 0.5)))[..., None]
    alb = mix(pl * br[..., None], wood, gone)
    res = tgrain(r, dict(h=h, alb=alb), 0.04, 0.05, scale=2.8, aspect=1.8)
    res.update(gloss=0.08, susc=0.8, nstr=0.85, ao_s=1.0, ao_k=1.3, tint=0.06, fresh=None)
    return res


def t_deadwood_bare(r):
    """A dead pine's trunk bare of bark: weathered silver-grey wood with a faint warm cast, fine fibres along the
    grain, a few long drying checks along it opening and closing, darker weathering streaks running down the
    stem."""
    gr = lines(nz(r, 0.35, 25.0), 0.35) + 0.7 * lines(nz(r, 0.8, 40.0), 0.35) + 0.5 * lines(nz(r, 0.2, 12.0), 0.3)
    wr = crack_net(r, 5, length=(150.0, 340.0), width=(2.5, 8.0), join=8.0, wobble=0.02, pull=0.05, tilt0=0.04,
                   taper=0.15)
    sd, w = net_sd(wr, rg=(0.4, 2.5), r=r)
    hw = 0.5 * w
    chk = vwall(sd, 2.0 * hw + 1.5, hw + 0.2, p=0.8)
    h = -0.12 * gr + chk + 0.25 * fbm(r, 2.0, 30.0, 3) + 0.6 * fbm(r, 30.0, 60.0, 2)
    alb = tbase(r, '#a49c90', '#b2ab9f', 10.0, 50.0, streak=0.07, s=2.0)
    alb = vary(alb, 0.08, -gr + 0.5)
    st = ss(0.4, 1.6, fbm(r, 8.0, 70.0, 3) + 0.4 * nz(r, 3.0, 30.0))
    alb = mix(alb, C('#706a60'), 0.5 * st)
    alb = mix(alb, C('#8a7058'), 0.15 * ss(1.2, 2.4, fbm(r, 8.0, 30.0, 3)))
    dt = np.clip(-sd / np.maximum(hw + 0.2, 0.3), 0.0, 1.0)
    alb = mix(alb, C('#3a342e'), 0.85 * ss(0.0, 0.9, dt))
    res = tgrain(r, dict(h=h, alb=alb), 0.02, 0.04, scale=2.0, aspect=6.0)
    res.update(gloss=0.06, susc=0.85, tint=0.06, fresh=None)
    return res


# The trees' bark types (appended to the shrubs' layers): name, the tree species wearing it as their base / only bark
# and as their upper bark (bark_tex_upper: the pine's orange mirror from about 4 m, the birch's white over its black
# foot, the aspen's and white poplar's smooth grey over their dark fissured foot, the young bark of the oak, ash,
# alder and walnut on the limbs and the upper trunk), the function, the tile's scale over 6 cm. The dead and burnt
# trees' (deadwood_bark, deadwood_bare, char) belong to no species: the data's dead and burnt variants take them.
TREE_TYPES = [
    ('pine_plates', ['pin_syl'], [], t_pine_plates, 8.0),
    ('pine_mirror', [], ['pin_syl'], t_pine_mirror, 4.0),
    ('spruce', ['pic_abi'], [], t_spruce, 3.0),
    ('birch_white', [], ['bet_pen', 'bet_pub'], t_birch_white, 3.0),
    ('birch_base', ['bet_pen'], [], t_birch_base, 4.0),
    ('birch_downy_base', ['bet_pub'], [], t_birch_downy_base, 5.0),
    ('aspen_smooth', [], ['pop_tre'], t_aspen_smooth, 3.0),
    ('aspen_base', ['pop_tre', 'pop_alb'], [], t_aspen_base, 3.0),
    ('poplar_white', [], ['pop_alb'], t_poplar_white, 3.0),
    ('poplar_black', ['pop_nig', 'pop_can'], [], t_poplar_black, 8.0),
    ('oak', ['que_rob'], [], t_oak, 6.0),
    ('oak_young', [], ['que_rob'], t_oak_young, 2.0),
    ('red_oak', ['que_rub'], [], t_red_oak, 3.0),
    ('hornbeam', ['car_bet'], [], t_hornbeam, 3.0),
    ('maple', ['ace_pla'], [], t_maple, 2.5),
    ('sycamore', ['ace_pse'], [], t_sycamore, 4.0),
    ('box_elder', ['ace_neg'], [], t_box_elder, 2.5),
    ('lime', ['til_cor'], [], t_lime, 4.0),
    ('ash', ['fra_exc', 'fra_pen'], [], t_ash, 3.0),
    ('ash_young', [], ['fra_exc'], t_ash_young, 2.0),
    ('elm', ['ulm_lae', 'ulm_min'], [], t_elm, 3.0),
    ('rowan', ['sor_auc'], [], t_rowan, 2.0),
    ('alder', ['aln_glu'], [], t_alder, 4.5),
    ('alder_young', [], ['aln_glu'], t_alder_young, 2.0),
    ('willow_tree', ['sal_alb', 'sal_fra'], [], t_willow_tree, 5.0),
    ('robinia', ['rob_pse'], [], t_robinia, 6.0),
    ('horse_chestnut', ['aes_hip'], [], t_horse_chestnut, 3.0),
    ('apple', ['mal_dom'], [], t_apple, 2.5),
    ('pear', ['pyr_com'], [], t_pear, 2.5),
    ('cherry_trunk', ['t_pru_cer'], [], t_cherry_trunk, 2.5),
    ('plum', ['pru_dom'], [], t_plum, 2.5),
    ('walnut', ['jug_reg'], [], t_walnut, 3.5),
    ('walnut_young', [], ['jug_reg'], t_walnut_young, 2.0),
    ('deadwood_bark', [], [], t_deadwood_bark, 4.0),
    ('deadwood_bare', [], [], t_deadwood_bare, 3.0),
    ('char', [], [], t_char, 4.0),
]


# ======================================================================================================
# Maps
# ======================================================================================================

def cavity(h, k=1.0, s=1.0):
    """Cavity from the relief: how far the neighbourhood rises above a texel, at four scales (s: a tree type's scale,
    the same scales in texels)."""
    occ = 0.0
    for rad, w in ((0.12, 0.25), (0.35, 0.6), (0.9, 0.6), (2.2, 0.4)):
        occ = occ + w * np.clip(blur(h, rad * s) - h, 0.0, None) / (rad * s)
    return 1.0 / (1.0 + k * occ)


def normals(h, s=1.0, pre=0.06):
    gx, gy = grad_mm(blur(h, pre))
    n = np.stack([-gx * s, -gy * s, np.ones_like(h)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def lichen(r, ao, nrm, gloss, susc, fresh, ks=1.0):
    """Where crustose lichen settles: discrete patches of 1-5 cm over a share of the bark (a third of the
    susceptibility), 0 elsewhere; the patches seek the fissures' edges and flat old bark, shun the glossy, and never
    lie on fresh surfaces (peeled bark, prickles). The shader's own patches pick from these. (ks: a tree type's scale,
    the patches as many to a tile.)"""
    cav = 1.0 - ao
    edge = ss(0.0, 0.35, np.clip(3.0 * blur(cav, 0.8 * ks) - 2.0 * cav, 0.0, 1.0))
    flat = np.clip(nrm[..., 2], 0.0, 1.0) ** 6
    fr = np.zeros((N, N)) if fresh is None else np.clip(np.broadcast_to(fresh, (N, N)), 0.0, 1.0)
    n = norm(fbm(r, 5.0 * ks, 6.5 * ks, 2, 0.4) + 0.3 * fbm(r, 0.8 * ks, 1.0 * ks, 2))
    n = n + 0.5 * blur(edge, 1.0 * ks) + 0.3 * flat - 0.8 * np.clip(gloss, 0.0, 1.0) - 3.0 * blur(fr, 0.3 * ks)
    sd = level_set(n, 0.33 * susc)
    m = ss(0.0, 0.5, sd) * (0.7 + 0.3 * edge) * (1.0 - 0.4 * ss(0.4, 0.7, cav))
    return np.clip(m * (1.0 - fr), 0.0, 1.0)


def to8(a):
    return np.clip(np.round(a * 255.0), 0, 255).astype(np.uint8)


# The tree types' albedo toward their photo packs' bark (tools/trees/data/tree_photo_colours.json, the masked bark
# pixels' median; the tile's median with its cavity in the engine's ambient light against it, then the frames' red over
# blue against the packs' under a neutral grey of the same frame): a linear factor a channel on the finished albedo -
# the pine's plates two fifths too light and too red (r/b 1.36 against 1.07 and olive, g/b 1.25 against 1.02), the
# alder's and black poplar's dark bark half the photos' value and red (r/b 1.55 against 1.07, 1.51 against 1.18), the
# oak's red (1.46 against 1.13), the spruce's (1.40 against 1.19); the pine's mirror bark a
# deeper red-orange (in the game r/g 1.9 and r/b 5.1 against an upper trunk's 3.0 and 4.0 under an overcast sky of the
# frame's own balance); the hue of the others within a few per cent. (The redrawn tiles of the last rounds - the oak,
# the black poplar and the alder on furrowed_bark, the spruce's flakes, the pine's furrows' net, the aspen's foot - keep
# the median in the ambient light the frames had been measured with: the oak's '#5d5850', the black poplar's
# '#5b5e5a' (its frames' red over blue the pack's), the pine's '#5d5b5a', the aspen's '#3a3731'; the spruce's a
# little less saturated than its '#6a635a' (the frames' 0.28 against the pack's 0.22), the alder's olive-grey as its
# pack and a tenth lighter (its frames' hue 34 degrees against the pack's 48).)
TREE_GRADE = {'pine_plates': (0.318, 0.454, 0.693), 'alder': (1.12, 1.255, 1.804),
              'poplar_black': (1.702, 2.111, 2.4), 'spruce': (0.68, 0.705, 0.906), 'oak': (0.794, 1.017, 1.158),
              'pine_mirror': (1.02, 0.95, 1.54), 'aspen_base': (0.925, 0.929, 0.925), 'birch_white': (0.956, 1.011, 1.052)}


def build(name, fn, scale=1.0):
    # (A tree type: the tile 6 cm x its scale; the maps' filters, the cavity's and the lichen's scales the same in
    # texels as a shrub type's.)
    set_scale(scale)
    s = float(scale)
    r = np.random.default_rng(zlib.crc32(name.encode('ascii')))
    res = fn(r)
    if name in TREE_GRADE:
        res['alb'] = res['alb'] * np.array(TREE_GRADE[name])
    h = res['h']
    ao = cavity(h, res.get('ao_k', 1.0), s * res.get('ao_s', 1.0))
    if res.get('ao_mul') is not None:
        ao = ao * res['ao_mul']
    ao = np.clip(ao, res.get('ao_min', 0.3), 1.0)
    nrm = normals(h, res.get('nstr', 1.0), 0.06 * s)
    gloss = np.clip(np.broadcast_to(res['gloss'], (N, N)), 0.02, 0.95)
    lich = lichen(r, ao, nrm, gloss, res.get('susc', 0.3), res.get('fresh'), s * res.get('lichen_s', 1.0))
    # Colour follows the relief: the tops weathered paler, the hollows darker (both scales).
    k = res.get('tint', 0.12)
    loc = h - blur(h, 0.8 * s * res.get('tint_s', 1.0))
    fine = h - blur(h, 0.15 * s)
    t = np.clip(0.7 * loc / (loc.std() + 1e-6) + 0.3 * fine / (fine.std() + 1e-6), -2.5, 2.5)
    # (A sub-texel prefilter: no stair-stepped edges.)
    alb = to_srgb(np.clip(blur(res['alb'] * np.exp(k * t)[..., None], 0.045 * s), 0.0, 1.0))
    d = to8(np.dstack([alb, ao]))
    n = to8(np.dstack([0.5 + 0.5 * nrm[..., 0], 0.5 + 0.5 * nrm[..., 1], gloss, lich]))
    return d, n


# ======================================================================================================
# Output
# ======================================================================================================

def save_png(a, path):
    tmp = path + '.tmp'
    Image.fromarray(a).save(tmp, format='PNG')
    os.replace(tmp, path)


def save_text(path, text):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(text)
    os.replace(tmp, path)


def seam_rank(a):
    """The percentile of the step across the wrap among all the steps between neighbouring columns (x) and rows (y):
    a seamless tile puts it anywhere, a seam at the top."""
    a = a.astype(np.float64)
    cx = np.abs(np.roll(a, -1, axis=1) - a).mean(axis=tuple(i for i in range(a.ndim) if i != 1))
    cy = np.abs(np.roll(a, -1, axis=0) - a).mean(axis=tuple(i for i in range(a.ndim) if i != 0))
    return 100.0 * (cx < cx[-1]).mean(), 100.0 * (cy < cy[-1]).mean()


# ======================================================================================================
# Review renders (the engine's use: albedo x lerp(1, cavity, 0.35), ambient x lerp(1, cavity, 0.7))
# ======================================================================================================

SUN_DIR = np.array([-0.55, 0.62, 0.56])
SUN_DIR = SUN_DIR / np.linalg.norm(SUN_DIR)
SUN = np.array([1.0, 0.95, 0.88]) * 1.45
SKY = np.array([0.52, 0.60, 0.74]) * 0.42
GND = np.array([0.30, 0.27, 0.22]) * 0.2


def decode(d, n):
    alb = from_srgb(d[..., :3] / 255.0)
    cav = d[..., 3] / 255.0
    nx, ny = n[..., 0] / 255.0 * 2.0 - 1.0, n[..., 1] / 255.0 * 2.0 - 1.0
    nzc = np.sqrt(np.clip(1.0 - nx * nx - ny * ny, 0.0, 1.0))
    return dict(alb=alb, cav=cav, nt=np.dstack([nx, ny, nzc]), gloss=n[..., 2] / 255.0, lich=n[..., 3] / 255.0)


def aces(x):
    return np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0)


def shade(alb, nw, gloss, cav, view=np.array([0.0, 0.0, 1.0]), expo=1.0):
    ndl = np.clip((nw * SUN_DIR).sum(-1), 0.0, None)
    up = nw[..., 1]
    amb = (SKY * (0.5 + 0.5 * up)[..., None] + GND * (0.5 - 0.5 * up)[..., None]) * (1.0 - 0.7 * (1.0 - cav))[..., None]
    c = alb * (1.0 - 0.35 * (1.0 - cav))[..., None]
    hv = SUN_DIR + view
    hv = hv / np.linalg.norm(hv)
    ndh = np.clip((nw * hv).sum(-1), 0.0, 1.0)
    ndv = np.clip((nw * view).sum(-1), 1e-3, 1.0)
    rough = np.clip(1.0 - gloss, 0.12, 1.0)
    a2 = rough ** 4
    D = a2 / (np.pi * (ndh * ndh * (a2 - 1.0) + 1.0) ** 2)
    k = (rough + 1.0) ** 2 / 8.0
    G = ndv / (ndv * (1.0 - k) + k) * ndl / (ndl * (1.0 - k) + k + 1e-6)
    F = 0.04 + 0.96 * (1.0 - float(np.dot(view, hv))) ** 5
    spec = (D * G * F / (4.0 * ndv * np.maximum(ndl, 1e-3) + 1e-4) * ndl)[..., None] * SUN
    out = c * (SUN * ndl[..., None] + amb) + spec
    return to8(to_srgb(aces(out * expo)))


def sample(a, U, V):
    """Bilinear sample with wrap; U, V in texels."""
    S = a.shape[0]
    x0, y0 = np.floor(U - 0.5), np.floor(V - 0.5)
    fx, fy = U - 0.5 - x0, V - 0.5 - y0
    x0, y0 = x0.astype(np.int64) % S, y0.astype(np.int64) % S
    x1, y1 = (x0 + 1) % S, (y0 + 1) % S
    if a.ndim == 3:
        fx, fy = fx[..., None], fy[..., None]
    return (a[y0, x0] * (1 - fx) + a[y0, x1] * fx) * (1 - fy) + (a[y1, x0] * (1 - fx) + a[y1, x1] * fx) * fy


def mip(a, lv):
    for _ in range(lv):
        a = 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])
    return a


def flat_lit(m, flipv=True):
    """The tile lit flat (v up when flipv), at full resolution."""
    nt = m['nt'][::-1] if flipv else m['nt']
    nw = np.dstack([nt[..., 0], nt[..., 1] if flipv else -nt[..., 1], nt[..., 2]])
    f = (lambda a: a[::-1]) if flipv else (lambda a: a)
    return shade(f(m['alb']), nw, f(m['gloss']), f(m['cav']))


def cylinder(m, diam, length, ppm, ssf=3, theta0=0.3, v0=0.0, tile_mm=60.0):
    """A stem diam mm thick, length mm of it, seen side on at ppm pixels per mm (supersampled ssf)."""
    W, H = int(round(diam * ppm)), int(round(length * ppm))
    Ws, Hs = W * ssf, H * ssf
    ntile = max(1, int(round(np.pi * diam / tile_mm)))
    lv = max(0, int(round(np.log2(max(MM / (ppm * ssf), 1.0)))))
    S = N >> lv
    mm = {k: mip(m[k], lv) for k in ('alb', 'cav', 'nt', 'gloss')}
    xs = (np.arange(Ws) + 0.5) / Ws * 2.0 - 1.0
    ys = (np.arange(Hs) + 0.5) / Hs
    th = np.arcsin(np.clip(xs, -0.9999, 0.9999))
    U = (th + theta0) / (2.0 * np.pi) * ntile * S
    V = (length * (1.0 - ys) + v0) * MM / 2 ** lv
    U2, V2 = np.meshgrid(U, V)
    alb = sample(mm['alb'], U2, V2)
    cav = sample(mm['cav'], U2, V2)
    gl = sample(mm['gloss'], U2, V2)
    nt = sample(mm['nt'], U2, V2)
    nt = nt / np.linalg.norm(nt, axis=-1, keepdims=True)
    T = np.stack([np.cos(th), np.zeros_like(th), -np.sin(th)], -1)[None]
    Ng = np.stack([np.sin(th), np.zeros_like(th), np.cos(th)], -1)[None]
    B = np.array([0.0, 1.0, 0.0])
    nw = nt[..., 0:1] * T + nt[..., 1:2] * B + nt[..., 2:3] * Ng
    nw = nw / np.linalg.norm(nw, axis=-1, keepdims=True)
    img = shade(alb, nw, gl, cav).astype(np.float64)
    img = img.reshape(H, ssf, W, ssf, 3).mean(axis=(1, 3))
    return np.clip(np.round(img), 0, 255).astype(np.uint8)


def font(sz):
    for f in ('arial.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(f, sz)
        except OSError:
            pass
    return ImageFont.load_default()


BG = (54, 56, 60)


def sheet(maps, path):
    """Each type: 2x2 albedo (v up, 12 cm), a 28 mm stem lit near (4 px/mm) and at play distance (1.5 px/mm)."""
    cw, chh = 450, 290
    cols = 4
    rows = (len(maps) + cols - 1) // cols
    img = Image.new('RGB', (cols * cw + 10, rows * chh + 10), BG)
    dr = ImageDraw.Draw(img)
    f = font(13)
    for i, (name, sp, m, d) in enumerate(maps):
        x0, y0 = 10 + (i % cols) * cw, 10 + (i // cols) * chh
        dr.text((x0, y0), '%d %s: %s' % (i, name, ' '.join(sp)), fill=(230, 230, 230), font=f)
        tile = np.tile(d[::-1, :, :3], (2, 2, 1))
        t = Image.fromarray(tile).resize((240, 240), Image.LANCZOS)
        img.paste(t, (x0, y0 + 20))
        c1 = cylinder(m, 28.0, 60.0, 4.0)
        img.paste(Image.fromarray(c1), (x0 + 252, y0 + 20))
        c2 = cylinder(m, 28.0, 160.0, 1.5)
        img.paste(Image.fromarray(c2), (x0 + 252 + c1.shape[1] + 14, y0 + 20))
    save_png(np.asarray(img), path)


def crops(maps, path):
    """Full-resolution crops: albedo, lit flat, and the cavity / gloss / lichen channels at half size."""
    cw, chh = 790, 290
    cols = 2
    rows = (len(maps) + cols - 1) // cols
    img = Image.new('RGB', (cols * cw + 10, rows * chh + 10), BG)
    dr = ImageDraw.Draw(img)
    f = font(13)
    for i, (name, sp, m, d) in enumerate(maps):
        x0, y0 = 10 + (i % cols) * cw, 10 + (i // cols) * chh
        dr.text((x0, y0), '%d %s' % (i, name), fill=(230, 230, 230), font=f)
        a = d[::-1, :, :3][128:384, 128:384]
        img.paste(Image.fromarray(np.ascontiguousarray(a)), (x0, y0 + 20))
        lit = flat_lit(m)[128:384, 128:384]
        img.paste(Image.fromarray(np.ascontiguousarray(lit)), (x0 + 262, y0 + 20))
        for j, ch in enumerate(('cav', 'gloss', 'lich')):
            g = to8(m[ch][::-1])
            g = Image.fromarray(g).resize((128, 128), Image.LANCZOS)
            img.paste(g.convert('RGB'), (x0 + 524 + (j % 2) * 132, y0 + 20 + (j // 2) * 132))
    save_png(np.asarray(img), path)


def inspect(name, m, d, outdir):
    """A close look at one type: albedo, lit flat (both 1:1, v up) and a 25 mm stem at 1:1 texel density."""
    lit = flat_lit(m)
    alb = d[::-1, :, :3]
    cyl = cylinder(m, 25.0, 60.0, MM, ssf=2)
    W = 512 * 2 + cyl.shape[1] + 30
    img = Image.new('RGB', (W, 512), BG)
    img.paste(Image.fromarray(np.ascontiguousarray(alb)), (0, 0))
    img.paste(Image.fromarray(np.ascontiguousarray(lit)), (522, 0))
    img.paste(Image.fromarray(cyl), (1044, 0))
    save_png(np.asarray(img), os.path.join(outdir, 'insp_%s.png' % name))


def tree_sheet(maps, path):
    """Each tree type: 2x2 albedo (v up) and its lit tile, a 30 cm trunk 60 cm long lit at 1 px/mm (arm's length at
    1440p) and at 0.3 px/mm (three metres)."""
    cw, chh = 1130, 640
    cols = 2
    rows = (len(maps) + cols - 1) // cols
    img = Image.new('RGB', (cols * cw + 10, rows * chh + 10), BG)
    dr = ImageDraw.Draw(img)
    f = font(13)
    for i, (name, sp, m, d, scale) in enumerate(maps):
        set_scale(scale)
        x0, y0 = 10 + (i % cols) * cw, 10 + (i // cols) * chh
        dr.text((x0, y0), '%s (tile %.0f cm): %s' % (name, 6.0 * scale, ' '.join(sp)), fill=(230, 230, 230), font=f)
        t = Image.fromarray(np.tile(d[::-1, :, :3], (2, 2, 1))).resize((300, 300), Image.LANCZOS)
        img.paste(t, (x0, y0 + 20))
        lit = Image.fromarray(flat_lit(m)).resize((300, 300), Image.LANCZOS)
        img.paste(lit, (x0, y0 + 326))
        c1 = cylinder(m, 300.0, 600.0, 1.0, ssf=2, tile_mm=60.0 * scale)
        img.paste(Image.fromarray(c1), (x0 + 312, y0 + 20))
        c2 = cylinder(m, 300.0, 1600.0, 0.3, ssf=3, tile_mm=60.0 * scale)
        img.paste(Image.fromarray(c2), (x0 + 312 + c1.shape[1] + 14, y0 + 20))
    set_scale(1.0)
    save_png(np.asarray(img), path)


def report(name, d, n, secs):
    sd, sn = seam_rank(d), seam_rank(n)
    a = d[..., :3].astype(np.float64)
    y = lum(from_srgb(a / 255.0))
    srgb_y = to_srgb(y) * 255.0
    sat = (a.max(-1) - a.min(-1)) / np.maximum(a.max(-1), 1.0)
    print('%-16s %5.1fs  albedo sRGB-Y p5 %3.0f median %3.0f p95 %3.0f  cavity min %.2f  gloss %.2f  lichen>0 %3.0f%%  '
          'S %.2f/%.2f  seam pct d %3.0f/%3.0f n %3.0f/%3.0f'
          % (name, secs, np.percentile(srgb_y, 5), np.median(srgb_y), np.percentile(srgb_y, 95),
             d[..., 3].min() / 255.0, n[..., 2].mean() / 255.0, 100.0 * (n[..., 3] > 12).mean(), np.median(sat),
             np.percentile(sat, 90), sd[0], sd[1], sn[0], sn[1]), flush=True)


def build_tree(job):
    name, fn, scale = job
    t1 = time.time()
    d, n = build(name, fn, scale)
    save_png(d, os.path.join(OUT, name + '_d.png'))
    save_png(n, os.path.join(OUT, name + '_n.png'))
    return name, d, n, time.time() - t1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--inspect', default='')
    ap.add_argument('--workers', type=int, default=int(os.environ.get('DA_THREADS') or os.cpu_count() or 4))
    args = ap.parse_args()
    only = [s for s in args.only.split(',') if s]
    os.makedirs(OUT, exist_ok=True)
    t0 = time.time()
    maps = []
    for name, sp, fn in TYPES:
        if only and name not in only:
            continue
        t1 = time.time()
        d, n = build(name, fn)
        save_png(d, os.path.join(OUT, name + '_d.png'))
        save_png(n, os.path.join(OUT, name + '_n.png'))
        report(name, d, n, time.time() - t1)
        maps.append((name, sp, decode(d, n), d))
        if args.inspect:
            os.makedirs(args.inspect, exist_ok=True)
            inspect(name, maps[-1][2], d, args.inspect)
    # The trees' types, each on its own worker (the module's scale is per process).
    jobs = [(name, fn, scale) for name, base, upper, fn, scale in TREE_TYPES if not only or name in only]
    tmaps = []
    if jobs:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=max(1, min(args.workers, len(jobs)))) as ex:
            done = {nm: (d, n, secs) for nm, d, n, secs in ex.map(build_tree, jobs)}
        for name, base, upper, fn, scale in TREE_TYPES:
            if name in done:
                d, n, secs = done[name]
                report(name, d, n, secs)
                tmaps.append((name, base + ['%s (upper)' % s for s in upper], decode(d, n), d, scale))
    set_scale(1.0)
    # (The manifest follows from the type lists alone: written on every run, a partial one too.)
    species = {}
    for i, (name, sp, fn) in enumerate(TYPES):
        for s in sp:
            species[s] = i
    upper = {}
    types = [{'name': n_, 'species': sp} for n_, sp, _ in TYPES]
    for j, (name, base, up, fn, scale) in enumerate(TREE_TYPES):
        i = len(TYPES) + j
        types.append({'name': name, 'species': base, 'upper': up, 'scale': scale, 'tree': True})
        for s in base:
            species[s] = i
        for s in up:
            upper[s] = i
    # (Every tree species has a base bark; one with no zone pair wears it all the way up.)
    for s, i in list(species.items()):
        if any(s in b for _, b, _, _, _ in TREE_TYPES):
            upper.setdefault(s, i)
    man = {'size': N, 'metres': METRES, 'types': types, 'species': dict(sorted(species.items())),
           'species_upper': dict(sorted(upper.items()))}
    save_text(MANIFEST, json.dumps(man, indent=1) + '\n')
    if maps:
        sheet(maps, os.path.join(BUILD, 'bark_sheet.png' if not only else 'bark_sheet_partial.png'))
        crops(maps, os.path.join(BUILD, 'bark_crops.png' if not only else 'bark_crops_partial.png'))
    if tmaps:
        tree_sheet(tmaps, os.path.join(BUILD, 'bark_sheet_trees.png' if not only else 'bark_sheet_trees_partial.png'))
    print('%d types in %.1fs' % (len(maps) + len(tmaps), time.time() - t0))


if __name__ == '__main__':
    main()
