# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Leaf texture layers of the procedural shrubs and trees (docs/dead-air/SHRUBS.md, docs/dead-air/TREES_TEXTURES.md;
the shrubs' sizes from tools/shrubs/make_shrubs.py, the trees' from tree_leaf_botany.py).

Every species' leaf is drawn from its botany - outline, margin, venation architecture, surface - in physical
millimetres at 512 x SS texels and box-filtered to 512 (README_leaf.md: the channels, the coordinates):

  build/leaf/<layer>_d.png   R upper luminance x0.5, G vein mask, B transmission, A coverage
  build/leaf/<layer>_n.png   RG upper tangent normal (0.5 + 0.5 n), B gloss, A underside luminance x0.5
  leaf_manifest.json         the layers in array order (the shrubs', then the trees'), each species' a / b / card /
                             leaflet layer (a conifer's needles too)
  build/leaf_sheet.png       review: every layer lit as the engine lights it, and its coverage
  build/leaf_review_*.png    review pages: lit, at the object's real proportions, underside, coverage
  build/leaf_crop_*.png      review: full-resolution crops

usage: make_leaf_textures.py [--only sid,sid] [--ss 3] [--workers N] [--fresh-trees]
  --fresh-trees  the trees' boxes from their drawing alone (the manifest's own otherwise only grow)
"""
import argparse
import json
import math
import os
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import make_shrubs as MS  # noqa: E402
sys.path.insert(0, HERE)
import tree_leaf_botany as TB  # noqa: E402

# Every species the layers are drawn for: the shrubs', then the trees' (their own sizes, the same keys).
SPEC = dict(MS.SP)
SPEC.update(TB.TREES)

BUILD = os.path.join(HERE, 'build')
OUT = os.path.join(BUILD, 'leaf')
MANIFEST = os.path.join(HERE, 'leaf_manifest.json')
SIZE = 512
F32 = np.float32
DEG = math.pi / 180.0
NT = 4097
# The relief's strength in the normals (a height field in mm read as is).
NSTR = 1.0


def seed_of(*parts):
    return zlib.crc32('|'.join(str(p) for p in parts).encode('ascii'))


def mm(metres):
    return float(metres) * 1000.0


def mean2(v):
    return 0.5 * (float(v[0]) + float(v[1]))


def sstep(e0, e1, x):
    if np.isscalar(x):
        t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
        return t * t * (3.0 - 2.0 * t)
    t = (x - F32(e0)) * F32(1.0 / (e1 - e0))
    np.clip(t, 0.0, 1.0, out=t)
    return t * t * (F32(3.0) - F32(2.0) * t)


def smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b + (a - b) * h - k * h * (1.0 - h)


# ======================================================================================================
# Noise
# ======================================================================================================

def hash2(ix, iy, seed):
    h = np.asarray(ix).astype(np.uint32) * np.uint32(0x8DA6B343)
    h ^= np.asarray(iy).astype(np.uint32) * np.uint32(0xD8163841)
    h ^= np.uint32((seed * 0x9E3779B1) & 0xFFFFFFFF)
    h ^= h >> np.uint32(16)
    h *= np.uint32(0x85EBCA6B)
    h ^= h >> np.uint32(13)
    h *= np.uint32(0xC2B2AE35)
    h ^= h >> np.uint32(16)
    return h


def u01(h):
    return (h >> np.uint32(8)).astype(F32) * F32(1.0 / 16777216.0)


# Periodic noise textures: 'fbm' an amplitude 1/f spectrum, 'warp' the same without its fine detail (a coordinate
# warp must be smooth: its fine ripples would roughen every normal it bends), 'band20' and 'band80' rings at 20 and
# 80 cycles. A feature period p (mm) samples them at TEX_K[name] / p texels per mm.
_TEX = {}
TEX_K = {'fbm': 64.0, 'warp': 64.0, 'band20': 25.6, 'band80': 6.4}


def tex(name):
    if name not in _TEX:
        n = 512
        rng = np.random.default_rng({'fbm': 101, 'warp': 104, 'band20': 102, 'band80': 103}[name])
        w = np.fft.fft2(rng.standard_normal((n, n)))
        fy = np.fft.fftfreq(n)[:, None] * n
        fx = np.fft.fftfreq(n)[None, :] * n
        f = np.hypot(fx, fy)
        if name == 'fbm':
            amp = np.where(f < 1.5, 0.0, 1.0 / np.maximum(f, 1.0))
        elif name == 'warp':
            amp = np.where(f < 1.5, 0.0, np.exp(-(f / 10.0) ** 2) / np.maximum(f, 1.0))
        else:
            f0 = 20.0 if name == 'band20' else 80.0
            amp = np.exp(-((f - f0) / (0.4 * f0)) ** 2)
        t = np.real(np.fft.ifft2(w * amp))
        _TEX[name] = ((t - t.mean()) / t.std()).astype(F32)
    return _TEX[name]


def nz(name, a, b, period, off=0):
    """Smooth zero-mean unit noise of a feature period (mm) at the coordinates (a, b) in mm."""
    T = tex(name)
    k = TEX_K[name] / period
    x = a * F32(k) + F32((off * 97.31) % 512.0)
    y = b * F32(k) + F32((off * 41.77) % 512.0)
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = x - x0
    fy = y - y0
    ix = x0.astype(np.int32) & 511
    iy = (y0.astype(np.int32) & 511) << 9
    ix1 = (ix + 1) & 511
    iy1 = (iy + 512) & (511 << 9)
    Tf = T.ravel()
    a00 = Tf.take(iy + ix)
    a01 = Tf.take(iy + ix1)
    a10 = Tf.take(iy1 + ix)
    a11 = Tf.take(iy1 + ix1)
    return a00 + (a01 - a00) * fx + (a10 - a00) * fy + (a00 - a01 - a10 + a11) * (fx * fy)


def coarse(fn, a, b, step):
    """fn on every step-th texel of the grids a, b, brought back to the full grid (bilinear)."""
    h, w = a.shape
    if step <= 1 or min(h, w) < 4 * step:
        return fn(a, b)
    o = step // 2
    v = fn(a[o::step, o::step], b[o::step, o::step])
    return np.asarray(Image.fromarray(np.ascontiguousarray(v, dtype=F32), 'F').resize((w, h), Image.BILINEAR), F32)


def nzc(name, a, b, period, off=0, px=1.0):
    """nz for a feature period that spans many texels (px: the grid's texel in mm): sampled coarse."""
    step = int(np.clip(period / max(px, 1e-6) / 10.0, 1, 4))
    return coarse(lambda x, y: nz(name, x, y, period, off), a, b, step)


def worley(a, b, cell, seed, jit=0.9, want_id=False, f1=False):
    """Distance (mm) to the nearest Voronoi edge of jittered cells of size cell (f1: to the nearest cell's point),
    and the cell's random value."""
    x = a * F32(1.0 / cell)
    y = b * F32(1.0 / cell)
    xf = np.floor(x)
    yf = np.floor(y)
    fx = x - xf
    fy = y - yf
    xi = xf.astype(np.int32)
    yi = yf.astype(np.int32)
    x0, y0 = int(xi.min()) - 1, int(yi.min()) - 1
    nx_, ny_ = int(xi.max()) - x0 + 2, int(yi.max()) - y0 + 2
    gy, gx = np.mgrid[y0:y0 + ny_, x0:x0 + nx_]
    px = (0.5 + jit * (u01(hash2(gx, gy, seed)) - 0.5)).ravel()
    py = (0.5 + jit * (u01(hash2(gx, gy, seed + 7)) - 0.5)).ravel()
    cid = u01(hash2(gx, gy, seed + 13)).ravel()
    base = (yi - y0) * nx_ + (xi - x0)
    want_f1 = f1
    f1 = np.full(a.shape, 9.0, F32)
    f2 = np.full(a.shape, 9.0, F32)
    ids = np.zeros(a.shape, F32) if want_id else None
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            idx = base + (dy * nx_ + dx)
            ddx = px.take(idx) + F32(dx) - fx
            ddy = py.take(idx) + F32(dy) - fy
            d2 = ddx * ddx + ddy * ddy
            if want_id:
                ids = np.where(d2 < f1, cid.take(idx), ids)
            np.minimum(f2, np.maximum(f1, d2), out=f2)
            np.minimum(f1, d2, out=f1)
    if want_f1:
        return np.sqrt(f1) * F32(cell), ids
    edge = F32(0.5 * cell) * (np.sqrt(f2) - np.sqrt(f1))
    return edge, ids


def noise1d(s, rng, k=6):
    out = np.zeros_like(s)
    for i in range(1, k + 1):
        out += rng.normal() / i * np.sin(math.pi * i * s + rng.uniform(0.0, 2.0 * math.pi))
    return out / 1.4


# ======================================================================================================
# Morphology: the species' leaves (botany, docs/dead-air/SHRUBS.md; sizes from make_shrubs.py)
# ======================================================================================================
# Outline (relative half width f(s), s 0 at the blade's base .. 1 at the apex; widest at smax):
#   g = x^eb (1 - x^k)^ea (k puts its maximum at smax), f = 1 - (1 - g)^full; eb, ea the base's and the apex's
#   exponents (0.5 rounded, 1 straight, > 1 attenuate / acuminate), full the sides' fullness (> 1 rounder
#   shoulders, oblong); acu (length, width, exponent) an abrupt acumen; mucro a short point on a rounded apex;
#   cord (depth, sinus width) a cordate base, both shares of the length; asym the oblique base; bend the midrib's.
# Margin: entire / serrate / doubly / crenserr / crenate / dentate / denticulate / glandular; tooth height and
#   period (mm), sub teeth between the main ones (doubly: the main ones at the secondaries' ends), pt the tooth tip's
#   place in its period (forward-pointing > 0.5), irr their irregularity, t_from / t_to the toothed stretch,
#   grad the teeth's growth toward the apex, wavy / wavy_z the margin's waves in the plane and out of it (mm).
# Venation: vtype cras (to the teeth) / broch (loops) / eucamp (upturned, fading) / acro (curved from the base) /
#   none; nsec pairs, from s0 to s1, ang (base, apex) their angle to the midrib, curv their bend toward the apex,
#   elbow the share of the half width where they loop; abmed outward branches to the sub teeth; acro the curved
#   basal pairs (start, end, reach, strength); tert perc (ladders, tsp mm apart) / ret (net of tret mm);
#   areole the finest mesh (mm); widths: wmid share of the length, wsec / wter / ware mm.
# Surface: rug the interveinal puckering, vimp the veins' impression (mm), vfold the halves' V, gloss, hair,
#   mottle, margin_dark, revolute (mm the margin rolls under); rug the domes of the fields
#   between the major veins (mm, quilted), cush the fine cushions between the finest veins (mm), tgroove the
#   tertiary net's grooves (share of vimp); vtone the upper side's R on the midrib (< 1 impressed veins read by
#   their grooves, not lighter lines; > 1 raised or smooth glossy veins); relief the normal map's slope (p95
#   inside the blade, at the layer's texels) the relief is scaled to; vraise > 0 the veins raised (ridges, that
#   share of their height) instead of impressed; cav the darkening along an impressed vein's groove (beside its
#   line), gwide the groove's width over the vein's; dshade the domes' crests lighter, their folds darker (the
#   lamina's luminance spread is then set to SPREAD); under the underside's
#   luminance, uvein how far its raised veins read; glands_under, scales, rust, gdots: dots of glands and scales.
BASE = dict(
    smax=0.5, eb=0.8, ea=1.0, full=1.2, acu=None, ang0=None, cord=None, asym=0.03, bend=0.015, wob=0.012,
    mucro=0.0,
    margin='entire', tooth=0.3, period=2.0, sub=0, sub_h=0.45, pt=0.72, irr=0.25, t_from=0.12, t_to=0.92, grad=0.0,
    wavy=0.0, wavy_len=8.0, wavy_z=0.0,
    vtype='broch', nsec=(6, 8), s0=0.07, s1=0.86, ang=(55.0, 42.0), curv=0.25, cexp=1.5, elbow=0.78, up_len=1.0,
    abmed=0, acro=None,
    tert='perc', tsp=1.6, tret=1.6, areole=0.8,
    wmid=0.011, wsec=0.3, wter=0.09, ware=0.045,
    rug=0.04, quilt=0.0, cush=0.008, tgroove=0.4, vimp=0.05, vfold=0.05, gloss=0.6, hair=0.1, mottle=0.025,
    margin_dark=0.06, vtone=0.98, ksm=0.1, relief=0.3, vraise=0.0, cav=0.06, gwide=1.6, dshade=0.2,
    revolute=0.0, under=1.05, uvein=1.0, glands_under=0.0, scales=0.0, rust=0.0, gdots=0.0,
    pet_glands=0, pet_r=0.6, pet_R=1.02, pet_groove=0.5,
)

M = {
    # Willows: many fine brochidodromous secondaries, a net of tertiaries.
    'sal_cin': dict(dshade=0.25, gwide=2.0, relief=0.35, cav=0.18, tgroove=0.5, cush=0.008, vtone=0.95, smax=0.62,
                    eb=1.15, ea=0.85, full=1.25, margin='serrate', tooth=0.2, period=2.3, irr=0.7, wavy=0.18,
                    wavy_len=9.0, wavy_z=0.15, nsec=(11, 14), s0=0.05, s1=0.9, ang=(56, 44), elbow=0.8, tert='ret',
                    tret=1.3, areole=0.55, wmid=0.012, wsec=0.17, wter=0.075, rug=0.12, vimp=0.09, gloss=0.35,
                    hair=0.6, under=1.28, uvein=1.2, pet_r=0.5),
    'sal_aur': dict(dshade=0.25, gwide=2.0, relief=0.35, cav=0.18, tgroove=0.5, cush=0.008, vtone=0.95, smax=0.65,
                    eb=1.1, ea=0.7, full=1.3, bend=0.045, margin='serrate', tooth=0.22, period=2.0, irr=0.8,
                    wavy=0.22, wavy_len=7.0, wavy_z=0.18, nsec=(8, 10), ang=(60, 46), elbow=0.8, tert='ret',
                    tret=1.15, areole=0.5, wsec=0.16, wter=0.075, rug=0.14, vimp=0.1, gloss=0.3, hair=0.7,
                    under=1.25, uvein=1.2, pet_r=0.45),
    'sal_pen': dict(vtone=0.97, gwide=1.8, relief=0.27, cav=0.13, smax=0.42, eb=0.85, ea=1.6, full=1.3,
                    margin='glandular', tooth=0.16, period=1.5, irr=0.2, pt=0.75, nsec=(13, 17), s0=0.04, s1=0.9,
                    ang=(58, 46), elbow=0.8, tert='ret', tret=1.3, areole=0.6, wsec=0.15, wter=0.06, rug=0.015,
                    vimp=0.03, gloss=0.95, hair=0.0, mottle=0.025, under=1.05, pet_glands=2, pet_r=0.55),
    'sal_cap': dict(dshade=0.25, gwide=2.0, relief=0.35, cav=0.17, tgroove=0.5, cush=0.008, vtone=0.95, smax=0.45,
                    eb=0.6, ea=0.75, full=1.15, acu=(0.07, 0.15, 2.0), bend=0.03, margin='crenserr', tooth=0.3,
                    period=3.4, irr=0.6, wavy=0.22, wavy_len=11.0, wavy_z=0.18, nsec=(9, 12), ang=(62, 46),
                    elbow=0.8, tert='ret', tret=1.6, areole=0.7, wsec=0.22, wter=0.085, rug=0.09, vimp=0.08,
                    gloss=0.45, hair=0.3, under=1.38, uvein=1.15, pet_r=0.6),
    'sal_acu': dict(vtone=0.97, gwide=1.8, relief=0.26, cav=0.12, smax=0.36, eb=0.95, ea=2, full=1.5,
                    margin='glandular', tooth=0.13, period=1.4, pt=0.78, nsec=(17, 22), s0=0.03, s1=0.9,
                    ang=(60, 46), elbow=0.72, curv=0.3, tert='ret', tret=1.0, areole=0.5, wmid=0.013, wsec=0.1,
                    wter=0.05, rug=0.01, vimp=0.02, gloss=0.85, mottle=0.025, under=1.12, uvein=0.9, pet_r=0.45),
    'sal_tri': dict(vtone=0.97, gwide=1.8, relief=0.27, cav=0.13, smax=0.42, eb=0.8, ea=1.5, full=1.35,
                    margin='serrate', tooth=0.22, period=1.9, pt=0.75, irr=0.2, nsec=(12, 16), ang=(58, 44),
                    elbow=0.78, tert='ret', tret=1.2, areole=0.55, wsec=0.13, wter=0.055, rug=0.015, vimp=0.025,
                    gloss=0.85, under=1.06, pet_r=0.5),
    'sal_vim': dict(vtone=0.97, gwide=1.8, relief=0.27, cav=0.12, smax=0.42, eb=1, ea=1.6, full=1.9, bend=0.008,
                    margin='entire', wavy=0.05, wavy_len=12.0, nsec=(28, 36), s0=0.03, s1=0.92, ang=(66, 56),
                    elbow=0.62, curv=0.4, tert='ret', tret=0.9, areole=0.45, wmid=0.008, wsec=0.08, wter=0.04,
                    rug=0.01, vimp=0.03, gloss=0.45, hair=0.2, revolute=0.09, under=1.45, uvein=0.6, pet_r=0.5),
    'sal_ros': dict(vtone=0.97, gwide=1.8, relief=0.27, cav=0.12, smax=0.45, eb=1, ea=1.1, full=1.35,
                    margin='entire', nsec=(8, 11), ang=(55, 40), elbow=0.7, tert='ret', tret=0.9, areole=0.45,
                    wmid=0.012, wsec=0.07, wter=0.035, ware=0.03, rug=0.01, vimp=0.015, gloss=0.4, hair=0.5,
                    revolute=0.08, under=1.45, uvein=0.6, mottle=0.025, pet_r=0.3),
    # Forest and edge shrubs.
    'fra_aln': dict(ksm=0.05, mucro=0.0, gwide=2.0, relief=0.28, cav=0.13, pet_R=1.06, vtone=1.08, smax=0.54,
                    eb=0.85, ea=0.8, full=1.1, acu=(0.07, 0.15, 1.05, 0.035), margin='entire', wavy=0.12,
                    wavy_len=9.0, wavy_z=0.1, vtype='eucamp', nsec=(8, 10), s0=0.08, s1=0.86, ang=(54, 40),
                    curv=0.75, cexp=1.0, elbow=0.84, up_len=0.9, tert='perc', tsp=1.3, areole=0.6, wmid=0.012,
                    wsec=0.24, wter=0.07, rug=0.02, vimp=0.05, gloss=0.9, mottle=0.025, under=1.05, uvein=1.1,
                    pet_r=0.6),
    'cor_ave': dict(dshade=0.3, gwide=2.4, relief=0.5, cav=0.2, cush=0.012, ksm=0.03, quilt=0.25, vtone=0.95,
                    smax=0.56, eb=0.5, ea=0.8, full=1.0, acu=(0.17, 0.13, 1.2, 0.13), ang0=78, cord=(0.1, 0.035),
                    asym=0.07, s0=0.03, margin='doubly', tooth=1.4, sub=2, sub_h=0.4, t_from=0.06, t_to=0.93,
                    irr=0.15, pt=0.78, grad=0.2, vtype='cras', nsec=(7, 8), s1=0.84, ang=(52, 42), curv=0.12,
                    abmed=2, tert='perc', tsp=1.8, areole=0.85, wmid=0.012, wsec=0.42, wter=0.11, ware=0.05,
                    rug=0.45, vimp=0.1, gloss=0.4, hair=0.6, under=1.12, uvein=1.25, pet_r=0.9, pet_R=1.0),
    'euo_eur': dict(relief=0.27, cav=0.0, vraise=0.8, pet_R=1.06, vtone=1.13, smax=0.45, eb=0.95, ea=1.5, full=1.2,
                    margin='crenserr', tooth=0.18, period=1.8, pt=0.62, nsec=(6, 8), ang=(55, 45), elbow=0.7,
                    curv=0.35, tert='ret', tret=1.8, areole=0.7, wsec=0.2, wter=0.07, rug=0.02, vimp=0.03,
                    gloss=0.5, under=1.05, pet_r=0.55),
    'euo_ver': dict(relief=0.27, cav=0.0, vraise=0.8, pet_R=1.06, vtone=1.13, smax=0.42, eb=0.7, ea=1.4, full=1.2,
                    margin='serrate', tooth=0.14, period=1.3, pt=0.72, nsec=(5, 7), ang=(55, 45), elbow=0.72,
                    curv=0.35, tert='ret', tret=1.4, areole=0.6, wsec=0.15, wter=0.06, rug=0.015, vimp=0.03,
                    gloss=0.5, under=1.05, pet_r=0.4),
    'vib_opu': dict(dshade=0.25, gwide=2.0, relief=0.35, cav=0.17, cush=0.012, vtone=0.95, smax=0.42, eb=0.6,
                    ea=1.15, full=1.15, margin='serrate', tooth=1.5, period=5.0, pt=0.64, irr=1.0, t_from=0.3,
                    vtype='cras', nsec=(4, 5), s0=0.22, s1=0.8, ang=(52, 42), curv=0.15, tert='ret', tret=1.8,
                    tsp=2.0, areole=0.8, wmid=0.011, wsec=0.32, wter=0.1, rug=0.15, vimp=0.1, gloss=0.45, hair=0.2,
                    under=1.1, uvein=1.2, pet_glands=3, pet_r=0.95),
    'lon_xyl': dict(relief=0.28, cav=0.08, smax=0.55, eb=0.8, ea=0.7, full=1.1, margin='entire', nsec=(4, 6),
                    s0=0.1, s1=0.8, ang=(55, 40), elbow=0.75, curv=0.35, tert='ret', tret=2.0, areole=0.8,
                    wsec=0.18, wter=0.07, rug=0.04, vimp=0.05, gloss=0.3, hair=0.9, mottle=0.025, under=1.28,
                    pet_r=0.5),
    'pad_avi': dict(relief=0.26, cav=0.0, vraise=0.8, pet_R=1.06, vtone=1.13, smax=0.58, eb=1.1, ea=0.8, full=1.1,
                    acu=(0.1, 0.15, 2.0), margin='serrate', tooth=0.28, period=1.7, pt=0.82, irr=0.2, nsec=(8, 11),
                    s0=0.06, s1=0.86, ang=(58, 46), elbow=0.8, curv=0.3, tert='perc', tsp=1.3, areole=0.7,
                    wsec=0.22, wter=0.07, rug=0.03, vimp=0.04, gloss=0.85, under=1.05, uvein=1.15, pet_glands=2,
                    pet_r=0.6),
    'ame_spi': dict(relief=0.28, cav=0.08, smax=0.5, eb=0.55, ea=0.55, full=1.05, mucro=0.02, margin='serrate',
                    tooth=0.22, period=1.5, pt=0.75, t_from=0.3, nsec=(8, 11), s0=0.07, s1=0.84, ang=(55, 45),
                    vtype='cras', curv=0.3, tert='perc', tsp=1.2, areole=0.6, wsec=0.18, wter=0.06, rug=0.02,
                    vimp=0.04, gloss=0.55, under=1.15, pet_r=0.45),
    'cra_mon': dict(relief=0.27, cav=0.07, smax=0.55, eb=0.7, ea=0.9, full=1.2, margin='serrate', tooth=0.45,
                    period=2.0, pt=0.7, irr=0.5, t_from=0.55, vtype='cras', nsec=(2, 3), s0=0.25, s1=0.75,
                    ang=(45, 38), curv=0.2, tert='ret', tret=1.6, areole=0.6, wmid=0.012, wsec=0.18, wter=0.06,
                    rug=0.02, vimp=0.03, gloss=0.8, under=1.05, pet_r=0.5),
    'syr_vul': dict(relief=0.26, cav=0.0, vraise=0.8, cush=0.012, vtone=1.13, smax=0.32, eb=0.45, ea=1.5,
                    full=1.15, cord=(0.035, 0.035), margin='entire', nsec=(4, 5), s0=0.2, s1=0.75, ang=(50, 44),
                    elbow=0.72, curv=0.4, acro=[(0.02, 0.5, 0.62, 0.85, 0.5)], tert='ret', tret=2.4, areole=0.9,
                    wmid=0.012, wsec=0.34, wter=0.1, rug=0.05, vimp=0.045, gloss=0.85, mottle=0.025, under=1.02,
                    pet_r=0.9),
    'phi_cor': dict(relief=0.3, cav=0.1, cush=0.012, smax=0.38, eb=0.75, ea=1.6, full=1.15, margin='denticulate',
                    tooth=0.4, period=4.5, irr=0.5, vtype='broch', nsec=(2, 3), s0=0.45, s1=0.78, ang=(45, 40),
                    elbow=0.75, acro=[(0.015, 0.9, 0.6, 1.0, 0.96), (0.0, 0.55, 0.86, 0.55, 0.96)], tert='perc',
                    tsp=1.5, areole=0.7, wsec=0.28, wter=0.08, rug=0.06, vimp=0.05, gloss=0.5, hair=0.3,
                    under=1.12, uvein=1.2, pet_r=0.5),
    'rib_nig': dict(dshade=0.25, gwide=2.0, relief=0.35, cav=0.17, cush=0.012, vtone=0.95, smax=0.45, eb=0.6, ea=1,
                    full=1.15, margin='serrate', tooth=0.9, period=3.2, sub=1, sub_h=0.5, pt=0.66, irr=0.45,
                    t_from=0.3, vtype='cras', nsec=(3, 5), s0=0.2, s1=0.8, ang=(50, 40), curv=0.15, tert='ret',
                    tret=1.4, areole=0.7, wmid=0.011, wsec=0.24, wter=0.08, rug=0.14, vimp=0.09, gloss=0.45,
                    hair=0.15, under=1.1, uvein=1.15, glands_under=1.0, pet_r=0.8),
    'rib_uva': dict(gwide=2.0, relief=0.32, cav=0.15, cush=0.012, vtone=0.95, smax=0.5, eb=0.6, ea=0.6, full=1.15,
                    margin='crenate', tooth=0.8, period=2.6, pt=0.55, irr=0.4, t_from=0.35, vtype='cras',
                    nsec=(2, 4), s0=0.25, s1=0.8, ang=(50, 40), curv=0.2, tert='ret', tret=1.3, areole=0.6,
                    wmid=0.012, wsec=0.18, wter=0.07, rug=0.08, vimp=0.05, gloss=0.35, hair=0.6, under=1.15,
                    pet_r=0.6),
    # Compound leaves: the leaflet.
    'rub_ida': dict(dshade=0.25, gwide=2.0, relief=0.35, cav=0.17, cush=0.012, vtone=0.95, smax=0.4, eb=0.8,
                    ea=1.5, full=1.15, margin='doubly', tooth=0.8, sub=2, sub_h=0.45, t_from=0.08, pt=0.7,
                    irr=0.35, vtype='cras', nsec=(8, 10), s0=0.07, s1=0.84, ang=(50, 40), curv=0.15, abmed=1,
                    tert='perc', tsp=1.4, areole=0.6, wsec=0.28, wter=0.08, rug=0.14, vimp=0.09, gloss=0.45,
                    hair=0.3, under=1.6, uvein=0.55, pet_r=0.75),
    'rub_cae': dict(gwide=2.0, relief=0.33, cav=0.15, cush=0.012, vtone=0.95, smax=0.42, eb=0.85, ea=1.1, full=1.2,
                    margin='doubly', tooth=1.0, sub=2, sub_h=0.45, t_from=0.1, pt=0.68, irr=0.45, vtype='cras',
                    nsec=(6, 8), s0=0.08, s1=0.82, ang=(50, 40), curv=0.15, abmed=1, tert='perc', tsp=1.5,
                    areole=0.6, wsec=0.26, wter=0.08, rug=0.1, vimp=0.06, gloss=0.5, hair=0.3, under=1.18,
                    pet_r=0.65),
    'ros_can': dict(relief=0.28, cav=0.0, vtone=1.13, vraise=0.8, smax=0.45, eb=0.7, ea=0.9, full=1.2,
                    margin='serrate', tooth=0.34, period=1.5, pt=0.8, irr=0.45, t_from=0.15, vtype='cras',
                    nsec=(6, 8), s0=0.08, s1=0.82, ang=(52, 42), curv=0.35, tert='ret', tret=1.0, areole=0.5,
                    wmid=0.012, wsec=0.12, wter=0.05, rug=0.02, vimp=0.03, gloss=0.7, under=1.05, pet_r=0.55),
    'ros_maj': dict(relief=0.28, cav=0.0, vtone=1.13, vraise=0.8, smax=0.5, eb=0.8, ea=0.75, full=1.5,
                    margin='serrate', tooth=0.22, period=1.2, pt=0.78, irr=0.4, t_from=0.3, vtype='cras',
                    nsec=(7, 9), s0=0.08, s1=0.82, ang=(52, 42), curv=0.35, tert='ret', tret=1.0, areole=0.5,
                    wsec=0.11, wter=0.05, rug=0.03, vimp=0.035, gloss=0.35, hair=0.4, under=1.3, pet_r=0.5),
    'sam_nig': dict(relief=0.3, cav=0.0, vtone=1.13, vraise=0.8, smax=0.4, eb=1, ea=1.7, full=1.2, asym=0.1,
                    margin='serrate', tooth=0.5, period=2.4, pt=0.75, irr=0.3, nsec=(6, 9), s0=0.07, s1=0.84,
                    ang=(55, 44), elbow=0.84, tert='perc', tsp=1.8, areole=0.8, wsec=0.25, wter=0.08, rug=0.04,
                    vimp=0.05, gloss=0.4, under=1.08, pet_r=1.1),
    'sam_rac': dict(relief=0.3, cav=0.0, vtone=1.13, vraise=0.8, smax=0.38, eb=1, ea=2, full=1.3, asym=0.1,
                    margin='serrate', tooth=0.55, period=2.1, pt=0.82, irr=0.25, nsec=(8, 11), s0=0.06, s1=0.86,
                    ang=(55, 44), elbow=0.84, tert='perc', tsp=1.6, areole=0.75, wsec=0.22, wter=0.075, rug=0.03,
                    vimp=0.04, gloss=0.35, under=1.06, pet_r=0.95),
    'amo_fru': dict(relief=0.25, cav=0.05, smax=0.5, eb=0.55, ea=0.5, full=1.35, mucro=0.05, margin='entire',
                    nsec=(5, 7), ang=(58, 46), elbow=0.75, tert='ret', tret=1.0, areole=0.45, wmid=0.013,
                    wsec=0.08, wter=0.04, rug=0.015, vimp=0.02, gloss=0.45, under=1.1, gdots=1.0, pet_r=0.55),
    # Village and road shrubs.
    'pru_cer': dict(relief=0.26, cav=0.0, vraise=0.8, pet_R=1.06, vtone=1.13, smax=0.45, eb=0.85, ea=1.3, full=1.2,
                    margin='crenserr', tooth=0.3, period=2.0, sub=1, sub_h=0.5, pt=0.6, nsec=(7, 9), ang=(55, 45),
                    elbow=0.8, tert='ret', tret=1.6, areole=0.7, wsec=0.22, wter=0.07, rug=0.02, vimp=0.035,
                    gloss=0.9, under=1.03, pet_glands=2, pet_r=0.6),
    'cyt_sco': dict(relief=0.25, cav=0.04, mottle=0.025, uvein=0.6, vtone=0.99, smax=0.7, eb=1.5, ea=0.55,
                    full=1.05, margin='entire', vtype='broch', nsec=(2, 3), s0=0.2, s1=0.6, ang=(40, 35),
                    elbow=0.6, tert=None, areole=0.35, wmid=0.014, wsec=0.05, wter=0.03, ware=0.025, rug=0.01,
                    vimp=0.008, gloss=0.3, hair=0.6, under=1.15, pet_r=0.3),
    'cha_rut': dict(relief=0.25, cav=0.04, mottle=0.025, uvein=0.6, vtone=0.99, smax=0.66, eb=1.4, ea=0.6,
                    full=1.05, margin='entire', vtype='broch', nsec=(2, 4), s0=0.2, s1=0.65, ang=(40, 35),
                    elbow=0.6, tert=None, areole=0.4, wmid=0.014, wsec=0.06, wter=0.03, ware=0.025, rug=0.015,
                    vimp=0.008, gloss=0.28, hair=0.85, under=1.35, pet_r=0.35),
    'hip_rha': dict(relief=0.25, cav=0.04, smax=0.45, eb=1, ea=1.1, full=1.9, margin='entire', vtype='none',
                    tert=None, areole=0.0, wmid=0.014, rug=0.0, vimp=0.03, gloss=0.55, hair=0.0, mottle=0.025,
                    scales=1.0, rust=1.0, revolute=0.06, under=1.45, uvein=0.5, pet_r=0.35),
}
# The trees' leaves (tree_leaf_botany.py: their sizes): outline, margin, venation and surface by the same keys as
# the shrubs'. A compound or palmate species' entry is its leaflet.
M_TREES = {
    # Birches: rhombic to ovate, doubly serrate, craspedodromous straight secondaries.
    'bet_pen': dict(relief=0.25, cav=0.08, vtone=1.04, smax=0.35, eb=0.9, ea=1.0, full=1.25,
                    acu=(0.2, 0.06, 2.2), margin='doubly', tooth=1.5, sub=2, sub_h=0.45, t_from=0.15, t_to=0.9,
                    pt=0.75, irr=0.3, grad=0.2, vtype='cras', nsec=(6, 8), s0=0.08, s1=0.82, ang=(45, 38),
                    curv=0.1, abmed=1, tert='perc', tsp=1.4, areole=0.7, wmid=0.011, wsec=0.25, wter=0.08,
                    rug=0.02, vimp=0.04, gloss=0.45, hair=0.0, mottle=0.025, under=1.15, uvein=1.05, pet_r=0.4),
    'bet_pub': dict(relief=0.27, cav=0.1, vtone=0.99, smax=0.42, eb=0.55, ea=1.2, full=1.2, acu=(0.1, 0.12, 1.2),
                    cord=(0.015, 0.02), margin='doubly', tooth=0.9, sub=1, sub_h=0.5, t_from=0.15, pt=0.68, irr=0.4,
                    vtype='cras', nsec=(5, 7), s0=0.1, s1=0.8, ang=(50, 40), curv=0.12, abmed=1, tert='perc',
                    tsp=1.5, areole=0.75, wsec=0.24, wter=0.08, rug=0.05, vimp=0.05, gloss=0.3, hair=0.5,
                    under=1.25, uvein=1.15, pet_r=0.5),
    # Aspen: nearly round, sinuate-crenate, a strong basal pair of veins.
    'pop_tre': dict(relief=0.25, cav=0.06, vtone=1.05, smax=0.45, eb=0.55, ea=0.7, full=1.0,
                    acu=(0.06, 0.08, 1.0, 0.04), cord=(0.015, 0.02), margin='crenate', tooth=0.9, period=5.0, pt=0.55,
                    irr=0.45, t_from=0.12,
                    t_to=0.94, vtype='broch', acro=[(0.0, 0.55, 0.62, 0.9, 0.55)], nsec=(3, 5), s0=0.22, s1=0.75,
                    ang=(55, 45), elbow=0.75, tert='ret', tret=2.0, areole=0.8, wsec=0.26, wter=0.08, rug=0.02,
                    vimp=0.04, gloss=0.5, hair=0.0, under=1.12, pet_r=0.7, pet_groove=0.0),
    # Oaks: the lobes' own outline (LOBES), entire (robur) or bristle-toothed (rubra).
    'que_rob': dict(relief=0.27, cav=0.08, vtone=1.03, smax=0.62, eb=0.9, ea=0.6, full=1.3, asym=0.04,
                    cord=(0.05, 0.025),
                    margin='entire', wavy=0.3, wavy_len=14.0, wavy_z=0.25, vtype='broch', nsec=(2, 4), s0=0.15,
                    s1=0.8, ang=(50, 40), elbow=0.75, curv=0.25, tert='ret', tret=1.8, areole=0.75, wmid=0.012,
                    wsec=0.25, wter=0.08, rug=0.04, vimp=0.05, gloss=0.5, hair=0.05, under=1.15, pet_r=0.9),
    'que_rub': dict(relief=0.25, cav=0.06, vtone=1.04, smax=0.55, eb=0.8, ea=0.9, full=1.1, acu=(0.12, 0.04, 1.8),
                    margin='aristate', tooth=3.2, period=13.0, pt=0.6, irr=0.45, t_from=0.5, t_to=0.9,
                    vtype='broch', nsec=(2, 4), s0=0.2, s1=0.75, ang=(50, 40), elbow=0.75, tert='ret', tret=1.8,
                    areole=0.75, wmid=0.01, wsec=0.22, wter=0.07, rug=0.03, vimp=0.04, gloss=0.35, under=1.15,
                    pet_r=0.9, ksin_k=0.02),
    # Hornbeam: oblong-ovate, doubly serrate, many straight parallel secondaries, pleated between them.
    'car_bet': dict(dshade=0.3, gwide=2.2, relief=0.42, cav=0.18, cush=0.01, vtone=0.95, smax=0.45, eb=0.55, ea=1.3,
                    full=1.15, acu=(0.12, 0.08, 1.4, 0.06), cord=(0.02, 0.02), asym=0.06, margin='doubly', tooth=0.9,
                    sub=2,
                    sub_h=0.4, t_from=0.06, pt=0.75, irr=0.25, vtype='cras', nsec=(10, 14), s0=0.05, s1=0.88,
                    ang=(45, 40), curv=0.08, abmed=1, tert='perc', tsp=1.2, areole=0.7, wsec=0.3, wter=0.09,
                    rug=0.35, vimp=0.08, gloss=0.35, hair=0.2, under=1.12, uvein=1.2, pet_r=0.6),
    # Maples: palmately lobed (LOBES), Norway maple's lobes with long sharp teeth, sycamore's coarsely crenate.
    'ace_pla': dict(relief=0.25, cav=0.06, vtone=1.04, smax=0.4, eb=0.6, ea=1.0, full=1.15, acu=(0.16, 0.05, 1.6),
                    margin='dentate', tooth=6.5, period=22.0, pt=0.62, irr=0.5, t_from=0.35, t_to=0.85,
                    vtype='broch', nsec=(3, 5), s0=0.12, s1=0.8, ang=(55, 45), elbow=0.75, tert='ret', tret=2.0,
                    areole=0.8, wmid=0.01, wsec=0.24, wter=0.08, rug=0.02, vimp=0.04, gloss=0.55, under=1.1,
                    pet_r=1.0, ksin_k=0.04),
    'ace_pse': dict(relief=0.3, cav=0.12, vtone=0.97, smax=0.42, eb=0.6, ea=1.0, full=1.15, acu=(0.06, 0.08, 1.2),
                    margin='crenserr', tooth=2.2, period=7.5, pt=0.6, irr=0.6, t_from=0.25, t_to=0.9,
                    vtype='cras', nsec=(4, 6), s0=0.12, s1=0.8, ang=(55, 45), curv=0.15, tert='ret', tret=1.8,
                    areole=0.75, wmid=0.011, wsec=0.28, wter=0.09, rug=0.08, vimp=0.06, gloss=0.3, hair=0.1,
                    under=1.35, uvein=1.15, pet_r=1.1, ksin_k=0.008),
    # Lime: heart-shaped, oblique, abruptly acuminate, finely serrate, glaucous beneath.
    'til_cor': dict(relief=0.25, cav=0.06, vtone=1.04, smax=0.36, eb=0.4, ea=1.15, full=1.35, acu=(0.12, 0.05, 1.8),
                    cord=(0.13, 0.09), asym=0.14, margin='serrate', tooth=0.6, period=2.2, pt=0.72, irr=0.3,
                    t_from=0.08, vtype='broch', acro=[(0.0, 0.45, 0.65, 0.8, 0.6)], nsec=(4, 6), s0=0.2, s1=0.8,
                    ang=(50, 40), elbow=0.78, tert='perc', tsp=1.5, areole=0.7, wsec=0.24, wter=0.08, rug=0.02,
                    vimp=0.04, gloss=0.5, under=1.25, uvein=1.1, pet_r=0.6),
    # Elms: oblique base, doubly serrate, many straight secondaries, some forking.
    'ulm_lae': dict(dshade=0.25, gwide=2.0, relief=0.33, cav=0.14, vtone=0.97, smax=0.55, eb=0.7, ea=1.2, full=1.15,
                    acu=(0.1, 0.07, 1.5, 0.06), asym=0.28, margin='doubly', tooth=1.3, sub=1, sub_h=0.5, t_from=0.1,
                    pt=0.78, irr=0.3, vtype='cras', nsec=(13, 17), s0=0.06, s1=0.88, ang=(48, 42), curv=0.08,
                    abmed=1, tert='perc', tsp=1.3, areole=0.65, wsec=0.28, wter=0.08, rug=0.12, vimp=0.07,
                    gloss=0.35, hair=0.3, under=1.2, uvein=1.15, pet_r=0.6),
    'ulm_min': dict(relief=0.28, cav=0.1, vtone=1.0, smax=0.5, eb=0.7, ea=1.1, full=1.15, acu=(0.08, 0.07, 1.4, 0.05),
                    asym=0.25, margin='doubly', tooth=0.9, sub=1, sub_h=0.5, t_from=0.1, pt=0.75, irr=0.3,
                    vtype='cras', nsec=(9, 12), s0=0.07, s1=0.86, ang=(48, 42), curv=0.1, abmed=1, tert='perc',
                    tsp=1.2, areole=0.6, wsec=0.24, wter=0.07, rug=0.04, vimp=0.05, gloss=0.75, hair=0.05,
                    under=1.1, pet_r=0.5),
    # Alder: obovate, notched apex, irregularly doubly serrate, straight secondaries, glossy.
    'aln_glu': dict(relief=0.28, cav=0.08, vtone=1.02, smax=0.6, eb=0.8, ea=0.35, full=1.3, emarg=(0.06, 0.08),
                    margin='doubly', tooth=0.8, sub=1, sub_h=0.5, pt=0.65, irr=0.5, t_from=0.22, t_to=0.95,
                    vtype='cras', nsec=(5, 8), s0=0.1, s1=0.85, ang=(50, 42), curv=0.1, tert='perc', tsp=1.5,
                    areole=0.7, wsec=0.26, wter=0.08, rug=0.03, vimp=0.05, gloss=0.8, hair=0.1, under=1.1,
                    glands_under=0.3, pet_r=0.6),
    # Willows: lanceolate; white willow silky-silver, crack willow glossy and coarsely glandular-serrate.
    'sal_alb': dict(vtone=0.98, gwide=1.8, relief=0.25, cav=0.1, smax=0.45, eb=0.95, ea=1.6, full=1.35,
                    margin='glandular', tooth=0.15, period=1.3, pt=0.75, irr=0.2, nsec=(14, 20), s0=0.04, s1=0.9,
                    ang=(58, 46), elbow=0.75, tert='ret', tret=1.1, areole=0.5, wmid=0.012, wsec=0.12, wter=0.05,
                    rug=0.01, vimp=0.025, gloss=0.25, hair=0.8, mottle=0.025, under=1.6, uvein=0.6, pet_r=0.45),
    'sal_fra': dict(vtone=0.98, gwide=1.8, relief=0.27, cav=0.12, smax=0.42, eb=0.85, ea=1.7, full=1.3, bend=0.04,
                    margin='glandular', tooth=0.35, period=2.5, pt=0.78, irr=0.2, nsec=(12, 16), s0=0.04, s1=0.9,
                    ang=(58, 46), elbow=0.78, tert='ret', tret=1.3, areole=0.6, wmid=0.012, wsec=0.15, wter=0.06,
                    rug=0.015, vimp=0.03, gloss=0.9, hair=0.0, under=1.25, uvein=0.9, pet_r=0.55),
    # Poplars: black and hybrid triangular on flattened petioles, white poplar lobed and white-felted beneath.
    'pop_nig': dict(relief=0.22, cav=0.05, vtone=1.04, smax=0.3, eb=0.75, ea=1.0, full=1.25, acu=(0.2, 0.06, 2.0),
                    margin='crenserr', tooth=0.9, period=4.0, pt=0.6, irr=0.3, t_from=0.1, vtype='broch',
                    acro=[(0.0, 0.4, 0.6, 0.85, 0.6)], nsec=(5, 7), s0=0.15, s1=0.8, ang=(50, 40), elbow=0.75,
                    tert='ret', tret=1.6, areole=0.7, wsec=0.22, wter=0.07, rug=0.02, vimp=0.03, gloss=0.7,
                    under=1.05, pet_r=0.65, pet_groove=0.0),
    'pop_alb': dict(relief=0.25, cav=0.06, vtone=1.03, smax=0.45, eb=0.6, ea=0.9, full=1.15, margin='crenate',
                    tooth=2.5, period=10.0, pt=0.55, irr=0.7, t_from=0.25, t_to=0.9, vtype='broch', nsec=(2, 4),
                    s0=0.15, s1=0.8, ang=(50, 40), elbow=0.75, tert='ret', tret=1.8, areole=0.8, wsec=0.24,
                    wter=0.08, rug=0.03, vimp=0.04, gloss=0.35, hair=0.3, under=2.2, uvein=0.4, pet_r=0.8,
                    ksin_k=0.03),
    'pop_can': dict(relief=0.22, cav=0.05, vtone=1.04, smax=0.2, eb=0.45, ea=1.1, full=1.25, acu=(0.16, 0.07, 2.0),
                    margin='crenserr', tooth=0.9, period=4.2, pt=0.62, irr=0.3, t_from=0.08, vtype='broch',
                    acro=[(0.0, 0.35, 0.6, 0.85, 0.6)], nsec=(5, 7), s0=0.15, s1=0.8, ang=(50, 40), elbow=0.75,
                    tert='ret', tret=1.6, areole=0.7, wsec=0.22, wter=0.07, rug=0.02, vimp=0.03, gloss=0.65,
                    under=1.05, pet_r=0.7, pet_groove=0.0),
    # Fruit trees.
    'mal_dom': dict(relief=0.3, cav=0.1, vtone=0.98, smax=0.45, eb=0.6, ea=1.2, full=1.15, acu=(0.08, 0.1, 1.3),
                    margin='crenserr', tooth=0.6, period=2.6, pt=0.6, irr=0.35, nsec=(5, 7), s0=0.08, s1=0.82,
                    ang=(55, 45), elbow=0.8, tert='ret', tret=1.6, areole=0.7, wsec=0.24, wter=0.08, rug=0.06,
                    vimp=0.05, gloss=0.3, hair=0.4, under=1.4, uvein=1.15, pet_r=0.6),
    'pyr_com': dict(relief=0.24, cav=0.05, vtone=1.08, smax=0.45, eb=0.5, ea=1.0, full=1.25, acu=(0.06, 0.1, 1.2),
                    margin='crenserr', tooth=0.35, period=1.8, pt=0.6, irr=0.3, t_from=0.15, nsec=(6, 9), s0=0.08,
                    s1=0.82, ang=(55, 45), elbow=0.75, tert='ret', tret=1.5, areole=0.65, wsec=0.22, wter=0.07,
                    rug=0.02, vimp=0.04, gloss=0.95, hair=0.0, under=1.1, pet_r=0.6),
    'pru_dom': dict(dshade=0.25, gwide=2.0, relief=0.33, cav=0.15, tgroove=0.5, vtone=0.96, smax=0.58, eb=0.9,
                    ea=0.9, full=1.15, acu=(0.06, 0.12, 1.2), margin='crenserr', tooth=0.5, period=2.2, sub=1,
                    sub_h=0.5, pt=0.6, irr=0.35, nsec=(5, 7), s0=0.08, s1=0.82, ang=(55, 45), elbow=0.8, tert='ret',
                    tret=1.4, areole=0.6, wsec=0.24, wter=0.08, rug=0.15, vimp=0.09, gloss=0.25, hair=0.3,
                    under=1.3, uvein=1.2, pet_glands=2, pet_r=0.6),
    # Compound leaves: the leaflet.
    'fra_exc': dict(relief=0.25, cav=0.06, vtone=1.03, smax=0.42, eb=0.85, ea=1.6, full=1.25,
                    acu=(0.1, 0.05, 1.5, 0.06),
                    margin='serrate', tooth=0.45, period=3.0, pt=0.72, irr=0.3, t_from=0.15, nsec=(9, 13),
                    s0=0.06, s1=0.86, ang=(55, 45), elbow=0.8, tert='ret', tret=1.5, areole=0.6, wsec=0.18,
                    wter=0.06, rug=0.02, vimp=0.035, gloss=0.5, under=1.1, pet_r=0.5),
    'fra_pen': dict(relief=0.25, cav=0.06, vtone=1.03, smax=0.45, eb=0.8, ea=1.3, full=1.2, acu=(0.1, 0.06, 1.4, 0.06),
                    margin='serrate', tooth=0.25, period=4.0, pt=0.7, irr=0.4, t_from=0.45, nsec=(7, 10),
                    s0=0.07, s1=0.85, ang=(55, 45), elbow=0.8, tert='ret', tret=1.5, areole=0.6, wsec=0.18,
                    wter=0.06, rug=0.02, vimp=0.035, gloss=0.55, hair=0.2, under=1.15, pet_r=0.5),
    'sor_auc': dict(relief=0.28, cav=0.08, vtone=1.0, smax=0.5, eb=0.4, ea=0.9, full=1.4, asym=0.12,
                    margin='serrate', tooth=0.6, period=2.2, pt=0.75, irr=0.3, t_from=0.3, vtype='cras',
                    nsec=(10, 14), s0=0.06, s1=0.86, ang=(52, 44), curv=0.2, tert='ret', tret=1.2, areole=0.55,
                    wsec=0.14, wter=0.05, rug=0.04, vimp=0.05, gloss=0.35, hair=0.3, under=1.3, pet_r=0.4),
    'rob_pse': dict(relief=0.2, cav=0.04, vtone=1.02, smax=0.5, eb=0.55, ea=0.55, full=1.25, mucro=0.04,
                    margin='entire', nsec=(5, 7), s0=0.1, s1=0.8, ang=(55, 45), elbow=0.7, tert='ret', tret=1.2,
                    areole=0.55, wmid=0.012, wsec=0.12, wter=0.05, rug=0.01, vimp=0.02, gloss=0.25, under=1.25,
                    pet_r=0.4),
    'ace_neg': dict(relief=0.25, cav=0.06, vtone=1.02, smax=0.4, eb=0.55, ea=1.3, full=1.15,
                    acu=(0.12, 0.06, 1.4, 0.07),
                    asym=0.08, margin='dentate', tooth=2.5, period=9.0, pt=0.6, irr=0.8, t_from=0.35, t_to=0.88,
                    vtype='cras', nsec=(5, 7), s0=0.1, s1=0.82, ang=(52, 42), curv=0.15, tert='ret', tret=1.6,
                    areole=0.7, wsec=0.22, wter=0.07, rug=0.03, vimp=0.04, gloss=0.4, under=1.15, pet_r=0.6),
    'jug_reg': dict(relief=0.26, cav=0.06, vtone=1.04, smax=0.5, eb=0.6, ea=1.0, full=1.2, acu=(0.06, 0.08, 1.3),
                    asym=0.06, margin='entire', wavy=0.3, wavy_len=16.0, wavy_z=0.2, nsec=(11, 16), s0=0.05,
                    s1=0.88, ang=(60, 50), elbow=0.82, tert='perc', tsp=1.6, areole=0.7, wsec=0.26, wter=0.08,
                    rug=0.02, vimp=0.04, gloss=0.55, under=1.1, pet_r=0.7),
    'aes_hip': dict(dshade=0.25, gwide=2.0, relief=0.35, cav=0.15, tgroove=0.4, vtone=0.97, smax=0.7, eb=1.7, ea=0.5,
                    full=1.5, acu=(0.06, 0.05, 1.5, 0.04), margin='doubly', tooth=0.8, sub=1, sub_h=0.5, pt=0.62,
                    irr=0.3,
                    t_from=0.25, vtype='cras', nsec=(16, 22), s0=0.06, s1=0.9, ang=(60, 50), curv=0.1, tert='perc',
                    tsp=1.6, areole=0.7, wsec=0.3, wter=0.09, rug=0.18, vimp=0.08, gloss=0.35, hair=0.15,
                    under=1.2, uvein=1.15, pet_r=1.4),
}

MORPH = {sid: dict(BASE, **over) for sid, over in list(M.items()) + list(M_TREES.items())}

# Lobed leaves: lobes from the junction (palmate) or the midrib (pinnatifid): origin (shares of the length), angle
# to the midrib (deg), length and width (shares of the length), the inner and outer sides' scale; mirrored.
LOBES = {
    'vib_opu': [dict(psi=0, len=1.0, wid=0.5, P=dict(nsec=(5, 6))),
                dict(psi=52, len=0.72, wid=0.42, sides=(0.9, 1.05), mirror=True,
                     P=dict(smax=0.4, ea=1.2, nsec=(5, 6), s0=0.15))],
    'rib_nig': [dict(psi=0, len=1.0, wid=0.5),
                dict(psi=50, len=0.74, wid=0.42, sides=(0.9, 1.1), mirror=True, P=dict(nsec=(4, 5), s0=0.15)),
                dict(psi=118, len=0.42, wid=0.36, sides=(1.0, 1.0), mirror=True,
                     P=dict(nsec=(2, 3), t_from=0.4))],
    'rib_uva': [dict(psi=0, len=1.0, wid=0.6),
                dict(psi=56, len=0.74, wid=0.5, sides=(0.9, 1.1), mirror=True, P=dict(nsec=(2, 3))),
                dict(psi=122, len=0.4, wid=0.42, mirror=True, P=dict(nsec=(1, 2), t_from=0.45))],
    'cra_mon': [dict(psi=0, len=1.0, wid=0.3, P=dict(smax=0.7, eb=2.0, ea=0.85, full=1.1, t_from=0.6,
                                                     nsec=(2, 3), s0=0.55, s1=0.85)),
                dict(a0=0.42, psi=32, len=0.52, wid=0.2, mirror=True, P=dict(smax=0.6, eb=0.8, ea=1.0, t_from=0.5)),
                dict(a0=0.14, psi=48, len=0.5, wid=0.19, mirror=True, P=dict(smax=0.6, eb=0.8, ea=1.0, t_from=0.5))],
}
LOBES_TREES = {
    # Pedunculate oak: five pairs of rounded lobes along the midrib, the broadest above the middle, ear-like at the
    # base.
    'que_rob': [dict(psi=0, len=1.0, wid=0.17, P=dict(smax=0.75, eb=1.2, ea=0.6, nsec=(2, 3), s0=0.6, s1=0.85)),
                dict(a0=0.74, psi=38, len=0.22, wid=0.13, mirror=True),
                dict(a0=0.58, psi=48, len=0.28, wid=0.15, mirror=True),
                dict(a0=0.42, psi=55, len=0.29, wid=0.15, mirror=True),
                dict(a0=0.27, psi=60, len=0.24, wid=0.13, mirror=True),
                dict(a0=0.14, psi=66, len=0.16, wid=0.1, mirror=True, P=dict(nsec=(1, 2)))],
    # Red oak: four pairs of long lobes reaching half way to the midrib, bristle-toothed at their ends.
    'que_rub': [dict(psi=0, len=1.0, wid=0.24, P=dict(smax=0.62, eb=1.3, ea=1.0, nsec=(2, 3), s0=0.55, s1=0.85,
                                                      t_from=0.7)),
                dict(a0=0.68, psi=42, len=0.42, wid=0.14, mirror=True),
                dict(a0=0.48, psi=52, len=0.46, wid=0.15, mirror=True),
                dict(a0=0.28, psi=62, len=0.36, wid=0.13, mirror=True),
                dict(a0=0.1, psi=72, len=0.2, wid=0.1, mirror=True, P=dict(nsec=(1, 2), t_from=0.6))],
    # Norway maple: five (seven) lobes from the junction, each long-acuminate with a few long sharp teeth.
    'ace_pla': [dict(psi=0, len=1.0, wid=0.6),
                dict(psi=54, len=0.92, wid=0.58, sides=(0.95, 1.05), mirror=True),
                dict(psi=110, len=0.6, wid=0.42, sides=(1.0, 1.0), mirror=True, P=dict(nsec=(2, 3), t_from=0.5))],
    # Sycamore: five ovate lobes in narrow sinuses, coarsely crenate-serrate.
    'ace_pse': [dict(psi=0, len=1.0, wid=0.52),
                dict(psi=54, len=0.86, wid=0.5, sides=(0.95, 1.05), mirror=True),
                dict(psi=108, len=0.52, wid=0.36, mirror=True, P=dict(nsec=(2, 3)))],
    # White poplar, a long shoot's leaf: three to five broad lobes, coarse blunt teeth.
    'pop_alb': [dict(psi=0, len=1.0, wid=0.55),
                dict(psi=58, len=0.7, wid=0.45, sides=(0.9, 1.05), mirror=True, P=dict(nsec=(2, 3))),
                dict(psi=115, len=0.38, wid=0.3, mirror=True, P=dict(nsec=(1, 2), t_from=0.4))],
}
# A species' second leaf of another form (white poplar's short-shoot leaf: ovate, shallowly lobed, sinuate-toothed).
LOBES_B = {
    'pop_alb': [dict(psi=0, len=1.0, wid=0.78, P=dict(smax=0.42, eb=0.5, ea=0.9, nsec=(4, 5))),
                dict(psi=55, len=0.5, wid=0.42, mirror=True, P=dict(nsec=(1, 2)))],
}

LOBES.update(LOBES_TREES)

# Compound leaves: leaflets of the layers, the size of the pairs from the base up, the terminal's, the leaflets'
# stalk (shares of the leaflet's length), stipules.
COMPOUND = {
    'rub_ida': dict(n=(3, 5), sizes=(0.82, 0.92), sizes_b=(0.6, 0.76), term=1.0, stalk=0.0, tstalk=0.0,
                    cordate_term=True),
    'rub_cae': dict(n=(3, 3), sizes=(0.86, 0.86), term=1.0, stalk=0.0, tstalk=0.0),
    'ros_can': dict(n=(7, 5), sizes=(0.78, 1.0), term=1.05, stalk=0.04, tstalk=0.1, stipules=(1.0, 0.26)),
    'ros_maj': dict(n=(7, 5), sizes=(0.8, 1.0), term=1.02, stalk=0.04, tstalk=0.1, stipules=(1.0, 0.36)),
    'sam_nig': dict(n=(5, 7), sizes=(0.86, 1.0), term=1.1, stalk=0.05, tstalk=0.12),
    'sam_rac': dict(n=(5, 7), sizes=(0.86, 1.0), term=1.08, stalk=0.04, tstalk=0.1),
    'amo_fru': dict(n=(17, 13), sizes=(0.75, 1.0), term=0.95, stalk=0.08, tstalk=0.12),
}
COMPOUND_TREES = {
    'fra_exc': dict(n=(11, 9), sizes=(0.72, 1.0), term=1.0, stalk=0.0, tstalk=0.08),
    'fra_pen': dict(n=(7, 9), sizes=(0.8, 1.0), term=1.05, stalk=0.04, tstalk=0.1),
    'sor_auc': dict(n=(13, 15), sizes=(0.85, 1.0), term=0.9, stalk=0.0, tstalk=0.06),
    'rob_pse': dict(n=(15, 13), sizes=(0.85, 1.0), term=0.95, stalk=0.08, tstalk=0.1),
    'ace_neg': dict(n=(5, 3), sizes=(0.85, 1.0), term=1.15, stalk=0.08, tstalk=0.18, lobed_lateral=(False, True)),
    'jug_reg': dict(n=(7, 9), sizes=(0.62, 1.0), term=1.15, stalk=0.0, tstalk=0.12),
}
# Palmately compound leaves: leaflets fanning from the petiole's tip (the middle largest), their spread.
PALMATE = {'aes_hip': dict(n=(7, 5), spread=88.0, sizes=(1.0, 0.85, 0.66, 0.46))}

COMPOUND.update(COMPOUND_TREES)
# Trifoliate spray leaves (the brooms): petiole and three leaflets.
TRIFOLIATE = {'cyt_sco': dict(ratio=0.42, spread=40.0), 'cha_rut': dict(ratio=0.45, spread=44.0)}
# The cards' shoots: twig radius (mm), its bark, buds, stipules (length mm), thorns.
CARD = {
    'sal_cin': dict(r=1.3, tex='hair', bud=2.6), 'sal_aur': dict(r=1.1, tex='bark', bud=2.2, stip=5.0),
    'sal_pen': dict(r=1.4, tex='gloss', bud=2.8, stip=2.0), 'sal_cap': dict(r=1.7, tex='hair', bud=3.2, stip=2.5),
    'sal_acu': dict(r=1.2, tex='gloss', bud=2.6), 'sal_tri': dict(r=1.3, tex='ridged', bud=2.4, stip=3.5),
    'sal_vim': dict(r=1.5, tex='hair', bud=3.0), 'sal_ros': dict(r=0.8, tex='hair', bud=1.6),
    'fra_aln': dict(r=1.4, tex='lent', bud=2.6), 'euo_eur': dict(r=1.5, tex='ridged4', bud=2.4),
    'euo_ver': dict(r=1.2, tex='warts', bud=2.2), 'lon_xyl': dict(r=1.1, tex='hair', bud=2.0),
    'pad_avi': dict(r=1.5, tex='lent', bud=3.4), 'ame_spi': dict(r=1.1, tex='lent', bud=3.0),
    'cra_mon': dict(r=1.4, tex='lent', bud=1.8, thorn=11.0), 'pru_cer': dict(r=1.4, tex='lent', bud=2.6),
    'cyt_sco': dict(r=1.0, tex='ridged', bud=0.0), 'cha_rut': dict(r=0.9, tex='hair', bud=0.0),
    'hip_rha': dict(r=1.2, tex='scales', bud=1.6, thorn=9.0),
}
CARD_TREES = {
    'bet_pen': dict(r=1.0, tex='lent', bud=4.0), 'bet_pub': dict(r=1.1, tex='hair', bud=4.0),
    'pop_tre': dict(r=1.6, tex='gloss', bud=7.0), 'que_rob': dict(r=1.8, tex='lent', bud=4.0),
    'car_bet': dict(r=1.0, tex='lent', bud=6.0), 'ace_pla': dict(r=2.2, tex='gloss', bud=6.0),
    'til_cor': dict(r=1.3, tex='gloss', bud=5.0), 'ulm_lae': dict(r=1.2, tex='hair', bud=4.0),
    'ulm_min': dict(r=1.0, tex='bark', bud=3.0), 'aln_glu': dict(r=1.4, tex='lent', bud=6.0),
    'sal_alb': dict(r=1.3, tex='hair', bud=3.0), 'sal_fra': dict(r=1.6, tex='gloss', bud=4.0),
    'pop_nig': dict(r=1.8, tex='gloss', bud=8.0), 'pop_alb': dict(r=1.8, tex='hair', bud=5.0),
    'pop_can': dict(r=2.0, tex='ridged', bud=8.0), 'ace_pse': dict(r=2.4, tex='gloss', bud=7.0),
    'que_rub': dict(r=2.0, tex='gloss', bud=5.0), 'mal_dom': dict(r=1.6, tex='hair', bud=4.0),
    'pyr_com': dict(r=1.6, tex='gloss', bud=4.0), 'pru_dom': dict(r=1.4, tex='gloss', bud=3.0),
    'fra_exc': dict(r=3.2, tex='gloss', bud=8.0), 'fra_pen': dict(r=2.8, tex='gloss', bud=6.0),
    'sor_auc': dict(r=2.4, tex='hair', bud=8.0), 'rob_pse': dict(r=2.0, tex='ridged', bud=0.0, thorn=9.0),
    'ace_neg': dict(r=2.6, tex='gloss', bud=4.0), 'jug_reg': dict(r=4.0, tex='gloss', bud=8.0),
    'aes_hip': dict(r=4.0, tex='lent', bud=12.0),
}
CARD.update(CARD_TREES)
# The spray species with a second leaf layer (all of them).
B_SPRAY = {'sal_cin', 'sal_pen', 'sal_cap', 'sal_tri', 'sal_vim', 'sal_acu', 'fra_aln', 'euo_eur', 'pad_avi',
           'pru_cer', 'lon_xyl', 'cra_mon', 'ame_spi', 'hip_rha', 'euo_ver', 'sal_aur', 'sal_ros', 'cyt_sco',
           'cha_rut'}
# The lamina's luminance spread (p95 - p5 of R at the layer's texels, its mean 1).
SPREAD = 0.48


def vary(P, rng, k=1.0):
    """A leaf of its own: its outline's and veins' small jitter."""
    Q = dict(P)
    Q['smax'] = float(np.clip(P['smax'] + k * rng.uniform(-0.035, 0.035), 0.2, 0.8))
    Q['eb'] = P['eb'] * (1.0 + k * rng.uniform(-0.08, 0.08))
    Q['ea'] = P['ea'] * (1.0 + k * rng.uniform(-0.08, 0.08))
    Q['full'] = max(P['full'] * (1.0 + k * rng.uniform(-0.08, 0.08)), 1.0)
    Q['asym'] = P['asym'] * rng.uniform(0.5, 1.5)
    Q['tooth'] = P['tooth'] * (1.0 + k * rng.uniform(-0.1, 0.1))
    Q['elbow'] = P['elbow'] + k * rng.uniform(-0.03, 0.03)
    return Q


# ======================================================================================================
# Canvas and poses
# ======================================================================================================
CHANNELS = ('R', 'G', 'B', 'nx', 'ny', 'nz', 'gl', 'un')


class Canvas:
    """The square in mm: X across (u - 0.5) * W, Y along v * L; premultiplied channels and coverage."""

    def __init__(self, n, L, W):
        self.n, self.L, self.W = n, float(L), float(W)
        self.dx, self.dy = self.W / n, self.L / n
        self.cov = np.zeros((n, n), F32)
        self.ch = {k: np.zeros((n, n), F32) for k in CHANNELS}

    def bbox(self, X, Y, pad_px=4):
        X = np.asarray(X, float)
        Y = np.asarray(Y, float)
        r = Y / self.dy - 0.5
        c = X / self.dx + self.n * 0.5 - 0.5
        i0 = max(int(math.floor(r.min())) - pad_px, 0)
        i1 = min(int(math.ceil(r.max())) + pad_px + 1, self.n)
        j0 = max(int(math.floor(c.min())) - pad_px, 0)
        j1 = min(int(math.ceil(c.max())) + pad_px + 1, self.n)
        if i1 - i0 < 3 or j1 - j0 < 3:
            return None
        return i0, i1, j0, j1

    def grid(self, i0, i1, j0, j1):
        Y = ((np.arange(i0, i1, dtype=F32) + F32(0.5)) * F32(self.dy))[:, None]
        X = ((np.arange(j0, j1, dtype=F32) + F32(0.5 - self.n * 0.5)) * F32(self.dx))[None, :]
        return X, Y

    def over(self, bb, a, vals):
        i0, i1, j0, j1 = bb
        sl = (slice(i0, i1), slice(j0, j1))
        k = 1.0 - a
        self.cov[sl] = a + self.cov[sl] * k
        for name, v in vals.items():
            self.ch[name][sl] = v * a + self.ch[name][sl] * k


class Pose:
    """A part's place: origin, direction (theta from +Y toward +X), foreshortening along / across, mirror, and the
    normal its plane faces (cards)."""

    def __init__(self, ox=0.0, oy=0.0, theta=0.0, fa=1.0, fb=1.0, flip=False, nb=(0.0, 0.0, 1.0)):
        self.ox, self.oy = float(ox), float(oy)
        self.d = (math.sin(theta), math.cos(theta))
        n = (math.cos(theta), -math.sin(theta))
        self.n = (-n[0], -n[1]) if flip else n
        self.fa, self.fb = float(fa), float(fb)
        v = np.asarray(nb, float)
        self.nb = tuple(v / np.linalg.norm(v))

    def to_canvas(self, a, b):
        return (self.ox + self.d[0] * a * self.fa + self.n[0] * b * self.fb,
                self.oy + self.d[1] * a * self.fa + self.n[1] * b * self.fb)

    def to_local(self, X, Y):
        dX = X - F32(self.ox)
        dY = Y - F32(self.oy)
        return ((dX * F32(self.d[0]) + dY * F32(self.d[1])) * F32(1.0 / self.fa),
                (dX * F32(self.n[0]) + dY * F32(self.n[1])) * F32(1.0 / self.fb))

    def steps(self, cv):
        """The local displacement of one texel along X and along Y."""
        return ((cv.dx * self.d[0] / self.fa, cv.dx * self.n[0] / self.fb),
                (cv.dy * self.d[1] / self.fa, cv.dy * self.n[1] / self.fb))


# ======================================================================================================
# Blades: lobes, outline, teeth, veins
# ======================================================================================================

_KW = {}


def warp_k(xm, eb, ea):
    """The k that puts the maximum of x^eb (1 - x^k)^ea at xm (it lies between exp(-ea/eb) and 1)."""
    key = (round(xm, 5), round(eb, 5), round(ea, 5))
    if key not in _KW:
        at = lambda k: math.exp(math.log(eb / (eb + k * ea)) / k)
        lo, hi = 1e-3, 80.0
        if at(lo) >= xm:
            k = lo
        elif at(hi) <= xm:
            k = hi
        else:
            for _ in range(60):
                mid = math.sqrt(lo * hi)
                if at(mid) < xm:
                    lo = mid
                else:
                    hi = mid
            k = math.sqrt(lo * hi)
        _KW[key] = k
    return _KW[key]


def outline_f(s, P, s_lo):
    eb, ea = P['eb'], P['ea']
    acu = P.get('acu')
    # An abrupt acumen (a fourth term): the rounded body closes that share short of the tip, the point beyond.
    s_end = 1.0 - (acu[3] if acu and len(acu) > 3 else 0.0)
    xm = (P['smax'] - s_lo) / (s_end - s_lo)
    k = warp_k(xm, eb, ea)
    x = np.clip((s - s_lo) / (s_end - s_lo), 0.0, 1.0)
    xk = np.power(x, k)
    g = np.power(x, eb) * np.power(np.clip(1.0 - xk, 0.0, 1.0), ea)
    t = eb / (eb + k * ea)
    gmax = t ** (eb / k) * (1.0 - t) ** ea
    f = 1.0 - np.power(np.clip(1.0 - g / gmax, 0.0, 1.0), P.get('full', 1.0))
    f = np.where((s < s_lo) | (s > s_end), 0.0, f)
    if acu:
        cl, cw, ce = acu[:3]
        t = np.clip((1.0 - s) / cl, 0.0, None)
        if len(acu) > 3:
            # The point, its base merging into the body's top (below it the body alone).
            cusp = cw * np.power(np.minimum(t, 1.0), ce) * np.clip(1.0 - (t - 1.0) * 4.0, 0.0, 1.0)
            f = np.clip(-smin(-f, -cusp, P.get('ksm', 0.1)), 0.0, None)
            f = np.where(s > 1.0, 0.0, f)
        else:
            cusp = cw * np.power(t, ce)
            f = np.clip(smin(f, cusp, P.get('ksm', 0.1)), 0.0, None)
    if P.get('mucro'):
        # A short bristle point on a rounded apex.
        m = P['mucro']
        f = np.maximum(f, 0.025 * np.clip(1.0 - (1.0 - s) / max(m, 1e-3), 0.0, 1.0) ** 0.5 * (s < 1.0))
    return f


class Lobe:
    def __init__(self, P, a0, b0, psi, L, W, side_scale, rng):
        self.P, self.a0, self.b0, self.psi, self.L, self.W = P, a0, b0, psi, L, W
        self.cs, self.sn = math.cos(psi), math.sin(psi)
        cord = P.get('cord')
        self.s_lo = -cord[0] if cord else 0.0
        self.notch = cord[1] if cord else 0.0
        self.emarg = P.get('emarg')
        self.st = np.linspace(self.s_lo, 1.0, NT)
        self.ds = self.st[1] - self.st[0]
        self.bend_amp = min(P['bend'], 0.08 * W / L) * float(rng.choice([-1.0, 1.0])) * rng.uniform(0.4, 1.2)
        fmax = outline_f(self.st, P, self.s_lo).max()
        hw, sl, arc = [], [], []
        for side in (0, 1):
            sg = 1.0 if side else -1.0
            sh = P['asym'] * 0.5 * sg * rng.uniform(0.6, 1.0)
            ss = self.st - sh * np.clip(1.0 - (self.st - self.s_lo) / 0.45, 0.0, 1.0) ** 2
            f = outline_f(ss, P, self.s_lo) / fmax
            f = f * (1.0 + P['wob'] * noise1d(self.st, rng))
            h = 0.5 * W * np.clip(f, 0.0, None) * side_scale[side] * (1.0 + sg * P['asym'] * 0.25)
            hw.append(h)
            sl.append(np.gradient(h, self.ds * L))
            seg = np.sqrt((self.ds * L) ** 2 + np.diff(h) ** 2)
            arc.append(np.concatenate([[0.0], np.cumsum(seg)]))
        self.hw_t = np.concatenate(hw).astype(F32)
        self.sl_t = np.concatenate(sl).astype(F32)
        self.arc = arc
        self.dt = np.zeros(2 * NT, F32)
        self.tips = {0: [], 1: []}

    def hw_at(self, s, side):
        return float(np.interp(s, self.st, self.hw_t[side * NT:(side + 1) * NT]))

    def arc_at(self, s, side):
        return float(np.interp(s, self.st, self.arc[side]))

    def s_of_arc(self, A, side):
        return float(np.interp(A, self.arc[side], self.st))

    def bend(self, s):
        return self.bend_amp * self.L * s * s

    def to_blade(self, ak, bl):
        bk = bl + self.bend(ak / self.L)
        return self.a0 + ak * self.cs - bk * self.sn, self.b0 + ak * self.sn + bk * self.cs

    def look(self, T, s, sidei):
        x = np.clip((s - F32(self.s_lo)) * F32(1.0 / self.ds), 0.0, NT - 1.001)
        i = x.astype(np.int32)
        f = x - i
        i += sidei * NT
        t0 = T[i]
        return t0 + (T[i + 1] - t0) * f

    def field(self, A, B, sx, sy):
        da = A - F32(self.a0)
        db = B - F32(self.b0)
        ak = da * F32(self.cs) + db * F32(self.sn)
        bk = db * F32(self.cs) - da * F32(self.sn)
        s = ak * F32(1.0 / self.L)
        bl = bk - F32(self.bend_amp * self.L) * s * s
        side = bl >= 0
        sidei = side.astype(np.int32)
        hw = self.look(self.hw_t, s, sidei)
        sl = self.look(self.sl_t, s, sidei)
        abl = np.abs(bl)
        # The teeth by the nearest margin point (one Newton step along the normal): where the margin runs across
        # the axis its teeth spread along it, not along s.
        sc = s + (abl - hw) * sl / (1.0 + sl * sl) * F32(1.0 / self.L)
        dl = self.look(self.dt, sc, sidei)
        nrm = np.sqrt(1.0 + sl * sl)
        F = (abl - hw) / nrm - dl
        F = np.where(s > 1.0, np.hypot(ak - F32(self.L), bl), F)
        F = np.where(s < self.s_lo, np.hypot(ak - F32(self.s_lo * self.L), bl), F)
        if self.notch > 0.0:
            sw = F32(self.notch * self.L) * np.clip(s * F32(1.0 / self.s_lo), 0.0, 1.0) ** 0.6
            F = np.where(s < 0.0, np.maximum(F, sw - abl), F)
        if self.emarg:
            # A notch cut into the apex (the alder's): its depth and its half width, shares of the length.
            dep, wid = self.emarg
            sw = F32(wid * self.L) * np.clip((s - F32(1.0 - dep)) * F32(1.0 / dep), 0.0, 1.0) ** 0.7
            F = np.where(s > 1.0 - dep, np.maximum(F, sw - abl), F)
        m = np.where((s >= self.s_lo) & (s <= 1.0), abl / np.maximum(hw, 1e-3), 50.0)
        # The margin's outward normal in the lobe, turned into the blade: the texel's footprint across the edge.
        na = -sl / nrm
        nb_ = np.where(side, 1.0, -1.0) / nrm
        ma = na * F32(self.cs) - nb_ * F32(self.sn)
        mb = na * F32(self.sn) + nb_ * F32(self.cs)
        aa = np.sqrt((ma * sx[0] + mb * sx[1]) ** 2 + (ma * sy[0] + mb * sy[1]) ** 2)
        return F, m, aa, s, bl, sidei, hw

    def set_teeth(self, side, tips, hs, kinds, shape, pt):
        arc = self.arc[side]
        d = np.zeros(NT)
        dsub = np.zeros(NT)
        n = len(tips)
        doubly = shape == 'doubly'
        mains = [i for i in range(n) if kinds[i]]
        for i in range(n):
            A, h = tips[i], hs[i]
            if h <= 0.0:
                continue
            if doubly and kinds[i]:
                # A main tooth rises over its whole interval from the last main one.
                j = mains.index(i)
                gp = A - tips[mains[j - 1]] if j > 0 else (tips[mains[1]] - A if len(mains) > 1 else 4.0 * h)
                gn = tips[mains[j + 1]] - A if j + 1 < len(mains) else gp
            else:
                gp = tips[i] - tips[i - 1] if i > 0 else (tips[1] - tips[0] if n > 1 else 2.0)
                gn = tips[i + 1] - tips[i] if i + 1 < n else gp
            wb = gp * pt
            wa = gn * (1.0 - pt)
            if shape == 'denticulate':
                wb = min(wb, 2.6 * h)
                wa = min(wa, 1.2 * h)
            i0 = int(np.searchsorted(arc, A - wb))
            i1 = int(np.searchsorted(arc, A + wa))
            if i1 <= i0:
                continue
            x = arc[i0:i1]
            up = np.clip((x - (A - wb)) / max(wb, 1e-6), 0.0, 1.0)
            dn = np.clip((x - A) / max(wa, 1e-6), 0.0, 1.0)
            if shape == 'crenate':
                y = np.where(x <= A, np.sin(0.5 * math.pi * up) ** 0.6, np.cos(0.5 * math.pi * dn) ** 0.6)
            elif shape == 'crenserr':
                y = np.where(x <= A, np.sin(0.5 * math.pi * up) ** 0.8, (1.0 - dn) ** 1.3)
            elif shape == 'dentate':
                y = np.where(x <= A, up ** 1.05, (1.0 - dn) ** 1.05)
            elif shape == 'denticulate':
                y = np.where(x <= A, up ** 1.4, (1.0 - dn) ** 1.2)
            elif shape == 'aristate':
                # Concave flanks drawn out into a bristle (red oak's lobe teeth).
                y = np.where(x <= A, up ** 2.4, (1.0 - dn) ** 2.8)
            else:
                y = np.where(x <= A, up ** 1.0, (1.0 - dn) ** 1.25)
            if doubly and not kinds[i]:
                dsub[i0:i1] = np.maximum(dsub[i0:i1], h * y)
            else:
                d[i0:i1] = np.maximum(d[i0:i1], h * y)
        self.dt[side * NT:(side + 1) * NT] = (d + dsub).astype(F32)
        self.tips[side] = list(zip(tips, hs))


class Blade:
    def __init__(self, P, L, W, lobes, seed):
        self.P, self.L, self.W, self.lobes, self.seed = P, L, W, lobes, seed
        self.veins = []
        self.spots = []
        self.glands = []
        self.xscale = 1.0
        self.ksin = 0.5
        self.chlor = []
        self.brown = 0.0

    def add_vein(self, k, pts, w, g, r, b, u, h, own=True, order=2):
        lobe = self.lobes[k]
        pts = np.asarray(pts, float)
        if len(pts) < 2:
            return
        a, bb = lobe.to_blade(pts[:, 0], pts[:, 1])
        n = len(pts)
        arr = lambda v: np.broadcast_to(np.asarray(v, float), (n,)).copy()
        self.veins.append(dict(pts=np.stack([a, bb], 1), w=arr(w), g=arr(g), r=arr(r), b=arr(b), u=arr(u),
                               h=arr(h), lobe=(k if own and len(self.lobes) > 1 else -1), order=order))

    def field_at(self, A, B):
        """The union's signed distance at points (mm)."""
        A = np.asarray(A, F32)
        B = np.asarray(B, F32)
        sx = sy = (0.01, 0.01)
        F = None
        for lobe in self.lobes:
            Fk = lobe.field(A, B, sx, sy)[0]
            F = Fk if F is None else smin(F, Fk, self.ksin)
        return F

    def outline(self):
        pts = []
        for lobe in self.lobes:
            for side in (0, 1):
                h = lobe.hw_t[side * NT:(side + 1) * NT] + lobe.dt[side * NT:(side + 1) * NT]
                a, b = lobe.to_blade(lobe.st * lobe.L, (1.0 if side else -1.0) * h)
                pts.append(np.stack([a, b], 1))
        p = np.concatenate(pts)
        self.opts = p[::4]
        return p


def resample(pts, step):
    pts = np.asarray(pts, float)
    if len(pts) < 2:
        return pts
    seg = np.hypot(*np.diff(pts, axis=0).T)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    n = max(int(cum[-1] / step), 1) + 1
    t = np.linspace(0.0, cum[-1], n)
    return np.stack([np.interp(t, cum, pts[:, 0]), np.interp(t, cum, pts[:, 1])], 1)


def along(pts):
    seg = np.hypot(*np.diff(pts, axis=0).T)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    return cum / max(cum[-1], 1e-9), cum[-1]


def at_frac(pts, r):
    t, _ = along(pts)
    return np.array([np.interp(r, t, pts[:, 0]), np.interp(r, t, pts[:, 1])])


def bezier(p0, p1, p2, p3, n=14):
    t = np.linspace(0.0, 1.0, n)[:, None]
    return ((1 - t) ** 3) * p0 + 3 * ((1 - t) ** 2) * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3


def trace(lobe, ak, bl, sg, alpha, curv, stop, step, max_len=1e9, cexp=1.5):
    """A vein from (ak, bl) at alpha to the lobe's axis, bending toward the apex, to the share stop of the half
    width (the margin at 1)."""
    L = lobe.L
    side = 1 if sg > 0 else 0
    pts = [(ak, bl)]
    total = 0.0
    rho = 0.0
    for _ in range(3000):
        s = ak / L
        if s >= 0.99 or s < lobe.s_lo:
            break
        hw = lobe.hw_at(s, side)
        rho = abs(bl) / max(hw, 1e-3)
        if rho >= stop:
            break
        phi = alpha - curv * rho ** cexp
        ak += step * math.cos(phi)
        bl += sg * step * math.sin(phi)
        pts.append((ak, bl))
        total += step
        if total > max_len:
            break
    pts = np.array(pts)
    if stop >= 1.0 and len(pts) >= 2 and rho >= stop:
        # The last step's crossing of the margin.
        p, q = pts[-2], pts[-1]
        lo, hi = 0.0, 1.0
        for _ in range(16):
            mid = 0.5 * (lo + hi)
            c = p + (q - p) * mid
            if abs(c[1]) / max(lobe.hw_at(c[0] / L, side), 1e-3) >= 1.0:
                hi = mid
            else:
                lo = mid
        pts[-1] = p + (q - p) * hi
    return pts


def rho_of(lobe, pts, side):
    return np.array([abs(p[1]) / max(lobe.hw_at(p[0] / lobe.L, side), 1e-3) for p in pts])


def gen_lobe_veins(bl, k, rng):
    lobe = bl.lobes[k]
    P = lobe.P
    L = lobe.L
    rel = L / bl.L
    step = float(np.clip(L / 140.0, 0.12, 0.7))
    vt = P['vtype']
    vimp = P['vimp']
    # The primary: the midrib, or the lobe's own primary from the junction.
    ss = np.linspace(0.0 if k == 0 else 0.02, 0.985, 48)
    w0 = max(P['wmid'] * 0.5 * (bl.L + bl.W) * rel ** 0.8, 0.12)
    wt = max(0.15 * w0, 0.06)
    w = wt + (w0 - wt) * (1.0 - ss) ** 0.8
    bl.add_vein(k, np.stack([ss * L, np.zeros_like(ss)], 1), w, 1.0 if k == 0 else 0.75, 1.0 - 0.15 * ss, 0.68,
                0.42 - 0.12 * ss,
                vimp * (1.3 - 0.6 * ss), own=False, order=1)
    if vt == 'none':
        return
    ws0 = P['wsec'] * rel ** 0.5
    ends = {0: [], 1: []}
    bends = {0: [], 1: []}
    rails = {0: [], 1: []}
    # Curved basal pairs (acrodromous, lilac's strong basal pair).
    for (sa, se, reach, stren, conv) in (P['acro'] or []):
        for side in (0, 1):
            sg = 1.0 if side else -1.0
            s = np.linspace(sa, se, 40)
            u = np.clip((s - sa) / (se - sa), 0.0, 1.0)
            rho = reach * np.sin(math.pi * np.clip(u * conv + 0.02, 0.0, 1.0)) ** 0.55
            hw = np.array([lobe.hw_at(x, side) for x in s])
            pts = np.stack([s * L, sg * rho * hw], 1)
            pts[0] = (sa * L, 0.0)
            ww = ws0 * stren * (1.0 - (0.65 if conv > 0.8 else 0.9) * u)
            bl.add_vein(k, pts, ww, 0.55 * stren, 0.9 * stren, 0.6, 0.28 * stren, vimp * (0.9 - 0.4 * u), order=2)
            rails[side].append((sa, pts))
    nsec = int(rng.integers(P['nsec'][0], P['nsec'][1] + 1))
    nsec = max(int(round(nsec * (rel if k else 1.0) ** 0.6)), 1 if P['nsec'][1] > 0 else 0)
    if nsec <= 0:
        return
    s0, s1 = P['s0'], P['s1']
    sp = (s1 - s0) / max(nsec - 1, 1)
    secs = {0: [], 1: []}
    for side in (0, 1):
        sg = 1.0 if side else -1.0
        off = rng.uniform(-0.35, 0.35) * sp
        for j in range(nsec):
            sj = s0 + sp * j + rng.uniform(-0.18, 0.18) * sp + (off if j else 0.5 * off)
            sj = float(np.clip(sj, max(lobe.s_lo, 0.0) + 0.01, 0.95))
            t = (sj - s0) / max(s1 - s0, 1e-3)
            alpha = (P['ang'][0] + (P['ang'][1] - P['ang'][0]) * t + rng.uniform(-4.0, 4.0)) * DEG
            if j < 2 and P.get('ang0'):
                alpha += (P['ang0'] - P['ang'][0]) * (1.0 - 0.6 * j) * DEG
            stop = 1.0 if vt == 'cras' else P['elbow'] + rng.uniform(-0.04, 0.04)
            pts = trace(lobe, sj * L, 0.0, sg, alpha, P['curv'], stop, step, cexp=P['cexp'])
            if len(pts) < 3:
                continue
            secs[side].append((sj, pts, alpha))
    for side in (0, 1):
        sg = 1.0 if side else -1.0
        lst = sorted(secs[side], key=lambda x: x[0])
        for j, (sj, pts, alpha) in enumerate(lst):
            wsc = ws0 * (1.0 - 0.35 * sj)
            u, tot = along(pts)
            if vt == 'cras':
                # To the tooth: the vein runs on into it.
                dvec = pts[-1] - pts[-2]
                dvec /= max(np.linalg.norm(dvec), 1e-9)
                pts = np.vstack([pts, pts[-1] + dvec * 0.55 * P['tooth']])
                u, tot = along(pts)
                ends[side].append(lobe.arc_at(pts[-2][0] / L, side))
            pr = resample(pts, max(step * 3.0, 0.5))
            uu, _ = along(pr)
            bl.add_vein(k, pr, wsc * (1.0 - 0.6 * uu), 0.42 - 0.1 * uu, 0.9 - 0.35 * uu, 0.6 - 0.15 * uu,
                        0.25 - 0.12 * uu, vimp * (1.0 - 0.4 * uu))
            rails[side].append((sj, pts))
            if vt == 'cras' and P['abmed']:
                nb = P['abmed'] + (2 if j == 0 else 0)
                for q in range(nb):
                    r = 0.58 + 0.3 * (q + rng.uniform(0.2, 0.8)) / nb
                    if r > 0.9:
                        continue
                    p0 = at_frac(pts, r)
                    i0 = max(int(r * (len(pts) - 1)) - 1, 0)
                    dv = pts[min(i0 + 2, len(pts) - 1)] - pts[i0]
                    phi = math.atan2(abs(dv[1]), dv[0])
                    br = trace(lobe, p0[0], p0[1], sg, phi + (30.0 + rng.uniform(-5, 6)) * DEG, 0.05, 1.0,
                               step, max_len=(1.0 - r) * tot * 1.3)
                    if abs(br[-1][1]) < 0.97 * lobe.hw_at(br[-1][0] / L, side):
                        continue
                    if len(br) < 3:
                        continue
                    dvec = br[-1] - br[-2]
                    dvec /= max(np.linalg.norm(dvec), 1e-9)
                    br = np.vstack([br, br[-1] + dvec * 0.4 * P['tooth'] * P['sub_h']])
                    bends[side].append(lobe.arc_at(br[-2][0] / L, side))
                    br = resample(br, max(step * 3.0, 0.4))
                    bu, _ = along(br)
                    bl.add_vein(k, br, wsc * 0.38 * (1.0 - 0.5 * bu), 0.3, 0.45, 0.38, 0.12, vimp * 0.6, order=3)
        if vt in ('broch', 'eucamp'):
            for j, (sj, pts, alpha) in enumerate(lst):
                E = pts[-1]
                dE = pts[-1] - pts[-3]
                dE /= max(np.linalg.norm(dE), 1e-9)
                wl = ws0 * (1.0 - 0.35 * sj) * 0.45
                up = np.array([math.cos(math.atan2(dE[1], dE[0]) - sg * 50 * DEG),
                               math.sin(math.atan2(dE[1], dE[0]) - sg * 50 * DEG)])
                joined = False
                if vt == 'broch' and j + 1 < len(lst):
                    nxt = lst[j + 1][1]
                    rh = rho_of(lobe, nxt, side)
                    target = rh[-1] * rng.uniform(0.74, 0.86)
                    idx = int(np.argmax(rh >= target)) if (rh >= target).any() else len(nxt) - 1
                    while idx < len(nxt) - 1 and nxt[idx][0] < E[0] + 0.25 * step:
                        idx += 1
                    J = nxt[idx]
                    if J[0] > E[0]:
                        dJ = nxt[min(idx + 1, len(nxt) - 1)] - nxt[max(idx - 1, 0)]
                        dJ /= max(np.linalg.norm(dJ), 1e-9)
                        nrm = np.array([dJ[1], -dJ[0]])
                        if nrm[0] > 0:
                            nrm = -nrm
                        D = np.linalg.norm(J - E)
                        loop = bezier(E, E + up * 0.5 * D, J + nrm * 0.45 * D, J)
                        loop = clamp_rho(lobe, loop, side, 0.95)
                        bl.add_vein(k, loop, wl, 0.35, 0.5, 0.42, 0.1, vimp * 0.5, order=2)
                        # The outer series: veinlets from the loop toward the margin.
                        mid = loop[len(loop) // 2]
                        tip = np.array([mid[0] + 0.2 * D, sg * 0.97 * lobe.hw_at((mid[0] + 0.2 * D) / L, side)])
                        bl.add_vein(k, bezier(mid, mid + (tip - mid) * 0.3 + np.array([0.15 * D, 0]),
                                              tip - (tip - mid) * 0.3, tip, 6), wl * 0.45, 0.18, 0.25, 0.25, 0.06,
                                    vimp * 0.3, order=3)
                        joined = True
                if not joined:
                    # Upturned along the margin, fading (and the topmost loop's end).
                    s_e = E[0] / L
                    gap = (lst[j + 1][0] - sj) if j + 1 < len(lst) else 0.1
                    sT = min(s_e + max(gap, 0.04) * P['up_len'] * rng.uniform(0.75, 1.0), 0.97)
                    rT = min(abs(E[1]) / max(lobe.hw_at(s_e, side), 1e-3) + 0.08, 0.94)
                    T = np.array([sT * L, sg * rT * lobe.hw_at(sT, side)])
                    D = np.linalg.norm(T - E)
                    curve = clamp_rho(lobe, bezier(E, E + up * 0.45 * D, T - np.array([0.45 * D, 0.0]), T), side,
                                      0.95)
                    cu = np.linspace(0.0, 1.0, len(curve))
                    bl.add_vein(k, curve, wl * (1.0 - 0.85 * cu) + 0.01, 0.35 - 0.2 * cu, 0.5 - 0.3 * cu, 0.4,
                                0.1, vimp * 0.5 * (1.0 - cu), order=2)
    # Percurrent tertiaries: ladders between neighbouring rails.
    if P['tert'] == 'perc':
        for side in (0, 1):
            rl = [p for _, p in sorted(rails[side], key=lambda x: x[0])]
            for j in range(len(rl) - 1):
                A_, B_ = rl[j], rl[j + 1]
                _, la = along(A_)
                _, lb = along(B_)
                Bd = resample(B_, 0.15)
                n = int(0.5 * (la + lb) / P['tsp'])
                for q in range(n):
                    if rng.random() < 0.08:
                        continue
                    r = (q + 0.5 + rng.uniform(-0.42, 0.42)) / max(n, 1)
                    if r < 0.06 or r > 0.96:
                        continue
                    pa = at_frac(A_, r)
                    dd = np.hypot(Bd[:, 0] - pa[0], Bd[:, 1] - pa[1])
                    q0 = int(np.argmin(dd))
                    pb = Bd[min(max(q0 + int(rng.integers(-1, 2)), 0), len(Bd) - 1)]
                    dv = pb - pa
                    ln = np.linalg.norm(dv)
                    if ln < 0.2 or ln > 0.3 * L or pb[0] < pa[0] - 0.2 * ln:
                        continue
                    nv = np.array([-dv[1], dv[0]]) / ln
                    o1, o2 = rng.uniform(-0.2, 0.2, 2) * ln
                    pts = bezier(pa, pa + dv / 3.0 + nv * o1, pa + 2.0 * dv / 3.0 + nv * o2, pb, 8)
                    if rng.random() < 0.2:
                        pts = pts[:int(rng.integers(3, 6))]
                    bl.add_vein(k, pts, P['wter'] * rel ** 0.3 * rng.uniform(0.6, 1.1), 0.15, 0.2, 0.3, 0.1,
                                vimp * 0.1, order=4)
    # Teeth: the main ones where the secondaries end (doubly serrate, craspedodromous), or regular.
    shape = P['margin']
    if shape != 'entire':
        for side in (0, 1):
            tips, hs, kinds = make_tips(lobe, side, sorted(ends[side]), sorted(bends[side]), P, rng)
            lobe.set_teeth(side, tips, hs, kinds, shape, P['pt'])
            if shape == 'glandular' or (shape == 'serrate' and P['gloss'] > 0.8):
                for A, h in zip(tips, hs):
                    s = lobe.s_of_arc(A, side)
                    if s <= 0.0:
                        continue
                    hw = lobe.hw_at(s, side) + h * 0.85
                    a, b = lobe.to_blade(s * L, (1.0 if side else -1.0) * hw)
                    bl.glands.append((a, b, max(0.6 * h, 0.06)))


def clamp_rho(lobe, pts, side, cap):
    out = pts.copy()
    for i, p in enumerate(out):
        hw = lobe.hw_at(p[0] / lobe.L, side)
        if abs(p[1]) > cap * hw:
            out[i, 1] = math.copysign(cap * hw, p[1])
    return out


def make_tips(lobe, side, ends, branch_ends, P, rng):
    st, arc = lobe.st, lobe.arc[side]
    A_from = float(np.interp(P['t_from'], st, arc))
    A_to = float(np.interp(P['t_to'] + 0.04, st, arc))
    h0 = P['tooth']
    irr = P['irr']
    tips, kinds = [], []
    main = [e for e in ends if A_from - 1.0 <= e <= A_to]
    if P['margin'] == 'doubly' and len(main) >= 2:
        # Where the secondaries' ends lie far apart (the basal lobes, the shoulders) branches end in teeth of
        # their own: main teeth kept about the median spacing.
        g = float(np.clip(np.median(np.diff(main)), 3.0, 14.0))
        full_ = sorted(set([round(e, 3) for e in main] + [round(e, 3) for e in branch_ends
                                                             if A_from <= e <= A_to]))
        filled = [full_[0]]
        for e in full_[1:]:
            gap = e - filled[-1]
            if gap < 0.45 * g:
                continue
            k_ = int(round(gap / g))
            for q in range(1, k_):
                filled.append(filled[-1] + gap / k_ if q == 1 else filled[-1] + gap / k_)
            filled.append(e)
        main = filled
        branch_ends = []
        m = P['sub']
        A = main[0] - g / (m + 1)
        lead = []
        while A > A_from:
            lead.append(A)
            A -= g / (m + 1) * rng.uniform(0.85, 1.15)
        for A in reversed(lead):
            tips.append(A)
            kinds.append(0)
        for i, A in enumerate(main):
            tips.append(A)
            kinds.append(1)
            if i + 1 < len(main):
                gp = main[i + 1] - A
                for q in range(m):
                    tips.append(A + gp * (q + 1 + rng.uniform(-0.15, 0.15)) / (m + 1))
                    kinds.append(0)
        A = main[-1] + g / (m + 1)
        while A < A_to:
            tips.append(A)
            kinds.append(0)
            A += g / (m + 1) * rng.uniform(0.85, 1.15)
        # The sub teeth the abmedial branches end in.
        tips = np.array(tips)
        for e in branch_ends:
            cand = [i for i in range(len(tips)) if kinds[i] == 0]
            if not cand:
                break
            i = min(cand, key=lambda c: abs(tips[c] - e))
            if abs(tips[i] - e) < 0.6 * g / (m + 1):
                tips[i] = e
        order = np.argsort(tips)
        tips = list(tips[order])
        kinds = [kinds[i] for i in order]
    else:
        per = P['period']
        m = P['sub']
        step = per / (m + 1)
        A = A_from + rng.uniform(0.2, 1.0) * step
        q = int(rng.integers(0, m + 1))
        while A < A_to:
            tips.append(A)
            kinds.append(1 if (m == 0 or q % (m + 1) == 0) else 0)
            A += step * (1.0 + irr * 0.35 * rng.uniform(-1.0, 1.0))
            q += 1
    hs = []
    for A, kd in zip(tips, kinds):
        s = float(np.interp(A, arc, st))
        env = float(sstep(P['t_from'] - 0.03, P['t_from'] + 0.12, s) *
                    (1.0 - sstep(P['t_to'] - 0.07, P['t_to'] + 0.02, s)))
        env *= 1.0 + P['grad'] * (s - 0.5)
        h = h0 * (1.0 if kd else P['sub_h']) * (1.0 + irr * rng.uniform(-0.5, 0.5)) * env
        h = min(h, 0.3 * lobe.hw_at(s, side))
        hs.append(max(h, 0.0))
    return tips, hs, kinds


def build_blade(P, L, W, seed, lobes=None, blem=0, chlor=0.0, brown=False):
    rng = np.random.default_rng(seed)
    P = vary(P, rng)
    specs = []
    for sp in (lobes or [dict(psi=0, len=1.0, wid=W / L)]):
        specs.append(sp)
        if sp.get('mirror'):
            q = dict(sp)
            q['psi'] = -sp['psi']
            q['b0'] = -sp.get('b0', 0.0)
            specs.append(q)
    objs = []
    for i, sp in enumerate(specs):
        Pk = dict(P, **sp.get('P', {}))
        if i:
            Pk['cord'] = None
        psi = (sp['psi'] + (rng.uniform(-3.0, 3.0) if sp['psi'] else 0.0)) * DEG
        inner, outer = sp.get('sides', (1.0, 1.0))
        side_scale = (inner, outer) if psi > 0 else ((outer, inner) if psi < 0 else (1.0, 1.0))
        ln = sp['len'] * L * (rng.uniform(0.95, 1.05) if i else 1.0)
        objs.append(Lobe(Pk, sp.get('a0', 0.0) * L, sp.get('b0', 0.0) * L, psi, ln, sp['wid'] * L, side_scale, rng))
    bl = Blade(P, L, W, objs, seed)
    bl.ksin = P.get('ksin_k', 0.012) * L if len(objs) > 1 else 0.5
    for k in range(len(objs)):
        gen_lobe_veins(bl, k, rng)
    if len(objs) > 1:
        sinus_veins(bl, rng)
    p = bl.outline()
    bl.ext = (p[:, 0].min(), p[:, 0].max(), p[:, 1].min(), p[:, 1].max())
    if blem:
        blemish(bl, rng, blem)
    mr = np.random.default_rng(seed_of(seed, 'marks'))
    if mr.random() < chlor:
        lobe = bl.lobes[0]
        for _ in range(int(mr.integers(1, 4))):
            side = int(mr.integers(0, 2))
            s_ = mr.uniform(0.15, 0.85)
            a_, b_ = lobe.to_blade(s_ * L, (1.0 if side else -1.0) * lobe.hw_at(s_, side) * mr.uniform(0.1, 0.85))
            bl.chlor.append((a_, b_, mr.uniform(0.06, 0.16) * min(L, 2.0 * W), mr.uniform(0.08, 0.16)))
    if brown:
        bl.brown = float(min(mr.uniform(0.8, 2.2), 0.06 * L))
    return bl


def sinus_veins(bl, rng):
    """Between neighbouring lobes a vein runs from the junction (or the midrib) to the sinus."""
    lobes = bl.lobes
    order = sorted(range(len(lobes)), key=lambda i: lobes[i].psi)
    for x, y in zip(order[:-1], order[1:]):
        la, lb = lobes[x], lobes[y]
        o = np.array([0.5 * (la.a0 + lb.a0), 0.5 * (la.b0 + lb.b0)])
        ang = 0.5 * (la.psi + lb.psi)
        d = np.array([math.cos(ang), math.sin(ang)])
        r = np.arange(0.0, bl.L, 0.15)
        P = o[None, :] + r[:, None] * d[None, :]
        F = bl.field_at(P[:, 0], P[:, 1])
        out = np.nonzero(F > -0.5)[0]
        if len(out) == 0 or out[0] < 4:
            continue
        end = P[out[0] - 1]
        start = o + d * min(0.1 * bl.L, 0.3 * r[out[0]])
        pts = np.stack([start, 0.5 * (start + end) + rng.uniform(-0.03, 0.03) * bl.L * np.array([-d[1], d[0]]), end])
        # (in the blade's frame, no lobe's own)
        bl.veins.append(dict(pts=pts, w=np.array([0.5, 0.4, 0.25]) * bl.P['wsec'], g=np.full(3, 0.3),
                             r=np.full(3, 0.45), b=np.full(3, 0.35), u=np.full(3, 0.1),
                             h=np.full(3, 0.6 * bl.P['vimp']), lobe=-1, order=2))


def blemish(bl, rng, level):
    """A few small necrotic or chlorotic spots (no holes, bites or tears: the shader bites the leaves itself)."""
    lobe = bl.lobes[0]
    L = lobe.L
    for _ in range(int(rng.choice([1, 2, 2, 3])) if level >= 1 else 0):
        side = int(rng.integers(0, 2))
        s = rng.uniform(0.2, 0.85)
        hw = lobe.hw_at(s, side)
        a, b = lobe.to_blade(s * L, (1.0 if side else -1.0) * hw * rng.uniform(0.15, 0.8))
        bl.spots.append((a, b, rng.uniform(0.004, 0.009) * L, rng.choice([0, 0, 1])))


# ======================================================================================================
# Rasterising
# ======================================================================================================

class Acc:
    def __init__(self, shape):
        z = lambda: np.zeros(shape, F32)
        self.g, self.r, self.b, self.u, self.h = z(), z(), z(), z(), z()


def band_cov(d, w, aa):
    """The share of a texel (footprint aa) a band of width w covers at distance d from its centre line."""
    lo = np.maximum(d - 0.5 * aa, -0.5 * w)
    hi = np.minimum(d + 0.5 * aa, 0.5 * w)
    return np.clip(hi - lo, 0.0, None) / aa


def window(A, pose, cv, bb, a, b, r):
    """The texels of the part's box within r (mm, local) of the local point (a, b), as slices."""
    X, Y = pose.to_canvas(a, b)
    k = max(pose.fa, pose.fb)
    c = X / cv.dx + cv.n * 0.5 - 0.5 - bb[2]
    q = Y / cv.dy - 0.5 - bb[0]
    rc, rq = r * k / cv.dx + 2.0, r * k / cv.dy + 2.0
    j0, j1 = max(int(c - rc), 0), min(int(c + rc) + 1, A.shape[1])
    i0, i1 = max(int(q - rq), 0), min(int(q + rq) + 1, A.shape[0])
    if j0 >= j1 or i0 >= i1:
        return None
    return (slice(i0, i1), slice(j0, j1))


def raster_veins(bl, A, B, pose, cv, bb, sx, sy, own, acc, gwide=1.6):
    i0, i1, j0, j1 = bb
    H, Wd = A.shape
    cc = (pose.d[0] * pose.fa / cv.dx, pose.n[0] * pose.fb / cv.dx, pose.ox / cv.dx + cv.n * 0.5 - 0.5 - j0)
    cr = (pose.d[1] * pose.fa / cv.dy, pose.n[1] * pose.fb / cv.dy, pose.oy / cv.dy - 0.5 - i0)
    kc = abs(cc[0]) + abs(cc[1])
    kr = abs(cr[0]) + abs(cr[1])
    for v in bl.veins:
        pts, w = v['pts'], v['w']
        lob = v['lobe']
        ow = own[lob] if (own is not None and lob >= 0) else None
        for q in range(len(pts) - 1):
            p0, p1 = pts[q], pts[q + 1]
            w0, w1 = w[q], w[q + 1]
            if w0 <= 0.0 and w1 <= 0.0:
                continue
            g = 0.5 * (v['g'][q] + v['g'][q + 1])
            r = 0.5 * (v['r'][q] + v['r'][q + 1])
            bt = 0.5 * (v['b'][q] + v['b'][q + 1])
            u = 0.5 * (v['u'][q] + v['u'][q + 1])
            hd = 0.5 * (v['h'][q] + v['h'][q + 1])
            wmax = max(w0, w1)
            ex, ey = p1[0] - p0[0], p1[1] - p0[1]
            ln = math.hypot(ex, ey)
            if ln < 1e-6:
                continue
            ma, mb = -ey / ln, ex / ln
            aa = max(math.hypot(ma * sx[0] + mb * sx[1], ma * sy[0] + mb * sy[1]), 1e-4)
            gw = max(gwide * wmax, 1.5 * aa)
            R = gw + aa
            cs = [cc[0] * p0[0] + cc[1] * p0[1] + cc[2], cc[0] * p1[0] + cc[1] * p1[1] + cc[2]]
            rs = [cr[0] * p0[0] + cr[1] * p0[1] + cr[2], cr[0] * p1[0] + cr[1] * p1[1] + cr[2]]
            ja = max(int(min(cs) - R * kc) - 1, 0)
            jb = min(int(max(cs) + R * kc) + 2, Wd)
            ia = max(int(min(rs) - R * kr) - 1, 0)
            ib = min(int(max(rs) + R * kr) + 2, H)
            if ja >= jb or ia >= ib:
                continue
            sl = (slice(ia, ib), slice(ja, jb))
            As = A[sl] - F32(p0[0])
            Bs = B[sl] - F32(p0[1])
            t = np.clip((As * F32(ex) + Bs * F32(ey)) * F32(1.0 / (ln * ln)), 0.0, 1.0)
            qx = As - t * F32(ex)
            qy = Bs - t * F32(ey)
            d = np.sqrt(qx * qx + qy * qy)
            wv = F32(w0) + F32(w1 - w0) * t
            c = band_cov(d, wv, F32(aa))
            gr = np.clip(1.0 - (d / np.maximum(F32(gwide) * wv, F32(1.5 * aa))) ** 2, 0.0, 1.0) ** 2
            if ow is not None:
                o = ow[sl]
                c = c * o
                gr = gr * o
            np.maximum(acc.g[sl], c * F32(g), out=acc.g[sl])
            np.maximum(acc.r[sl], c * F32(r), out=acc.r[sl])
            np.maximum(acc.b[sl], c * F32(bt), out=acc.b[sl])
            np.maximum(acc.u[sl], c * F32(u), out=acc.u[sl])
            np.maximum(acc.h[sl], gr * F32(hd), out=acc.h[sl])


def add_net(acc, e, w, aa, g, r, b, u, h, gw=None):
    c = band_cov(e, F32(w), aa)
    np.maximum(acc.g, c * F32(g), out=acc.g)
    np.maximum(acc.r, c * F32(r), out=acc.r)
    np.maximum(acc.b, c * F32(b), out=acc.b)
    np.maximum(acc.u, c * F32(u), out=acc.u)
    gr = np.clip(1.0 - (e / np.maximum(F32(gw or 1.6 * w), 1.5 * aa)) ** 2, 0.0, 1.0) ** 2
    np.maximum(acc.h, gr * F32(h), out=acc.h)


def blocks(a, k):
    """Means over k x k blocks (the layer's texels of a part's box)."""
    h, w = a.shape[0] // k * k, a.shape[1] // k * k
    return a[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))


def relief_gain(H, cv, pose, cov, dm, aa_iso, k, target):
    """The factor that sets the relief's slope - in the leaf's own frame, at the layer's texels, p95 over the blade
    three texels in from its outline - to target."""
    gy, gx = np.gradient(H, cv.dy, cv.dx)
    ga = (gx * F32(pose.d[0]) + gy * F32(pose.d[1])) * F32(pose.fa)
    gb = (gx * F32(pose.n[0]) + gy * F32(pose.n[1])) * F32(pose.fb)
    if min(H.shape) < 2 * k:
        return 1.0
    sl = np.hypot(blocks(ga, k), blocks(gb, k))
    inside = (blocks(cov, k) > 0.999) & (blocks(dm, k) > 3.0 * k * float(aa_iso))
    if inside.sum() < 30:
        return 1.0
    p = float(np.percentile(sl[inside], 95))
    return float(np.clip(target / max(p, 1e-5), 0.1, 20.0))


def spread_gain(V0, QM, cov, g, k, target):
    """The gain of the lamina's own variation V0 (and its mean) that sets the spread p95 - p5 of R = (1 + (V0 - m)
    gain) QM over the lamina at the layer's texels to target."""
    if min(V0.shape) < 2 * k:
        return 1.0, 1.0
    lam = (blocks(cov, k) > 0.999) & (blocks(g, k) < 0.05)
    if lam.sum() < 30:
        return 1.0, float(V0.mean())
    v = blocks(V0, k)[lam]
    q = blocks(QM, k)[lam]
    if v.size > 60000:
        v, q = v[::v.size // 60000 + 1], q[::q.size // 60000 + 1]
    m = float(v.mean())
    lo, hi = 0.2, 6.0
    for _ in range(20):
        kk = 0.5 * (lo + hi)
        r = (1.0 + (v - m) * kk) * q
        p5, p95 = np.percentile(r / r.mean(), [5, 95])
        if p95 - p5 < target:
            lo = kk
        else:
            hi = kk
    return 0.5 * (lo + hi), m


def field_scale(bl):
    """Half the spacing (mm) of the secondaries across their run: the size of the fields between them."""
    P = bl.P
    n = max(0.5 * (P['nsec'][0] + P['nsec'][1]), 1.0)
    ang = 0.5 * (P['ang'][0] + P['ang'][1]) * DEG
    sp = (P['s1'] - P['s0']) * bl.lobes[0].L / max(n - 0.5, 1.0) * math.sin(ang)
    return float(np.clip(0.45 * sp, 0.6, 6.0))


def major_dist(bl, A, B, pose, cv, bb, own, cap, step=3):
    """Distance (mm) to the nearest major vein (primaries, secondaries, their loops) on every step-th texel,
    interpolated: the fields' domes need no more."""
    o = step // 2
    Ac = A[o::step, o::step]
    Bc = B[o::step, o::step]
    D = np.full(Ac.shape, cap, F32)
    for v in bl.veins:
        if v['order'] > 2:
            continue
        pts, w = v['pts'], v['w']
        lob = v['lobe']
        ow = own[lob][o::step, o::step] if (own is not None and lob >= 0) else None
        for q in range(len(pts) - 1):
            p0, p1 = pts[q], pts[q + 1]
            ex, ey = p1[0] - p0[0], p1[1] - p0[1]
            ln = math.hypot(ex, ey)
            if ln < 1e-6:
                continue
            win = window(A, pose, cv, bb, 0.5 * (p0[0] + p1[0]), 0.5 * (p0[1] + p1[1]), 0.5 * ln + cap)
            if win is None:
                continue
            i0 = (win[0].start - o + step - 1) // step
            i1 = (win[0].stop - o + step - 1) // step
            j0 = (win[1].start - o + step - 1) // step
            j1 = (win[1].stop - o + step - 1) // step
            if i1 <= i0 or j1 <= j0:
                continue
            sl = (slice(i0, i1), slice(j0, j1))
            As = Ac[sl] - F32(p0[0])
            Bs = Bc[sl] - F32(p0[1])
            t = np.clip((As * F32(ex) + Bs * F32(ey)) * F32(1.0 / (ln * ln)), 0.0, 1.0)
            qx = As - t * F32(ex)
            qy = Bs - t * F32(ey)
            d = np.maximum(np.sqrt(qx * qx + qy * qy) - F32(0.5 * w[q]), 0.0)
            if ow is not None:
                d = np.where(ow[sl] > 0.5, d, F32(cap))
            np.minimum(D[sl], d, out=D[sl])
    im = Image.fromarray(D, 'F').resize((A.shape[1], A.shape[0]), Image.BILINEAR)
    return np.asarray(im, F32)


def normals(H, cv, nb):
    gy, gx = np.gradient(H, cv.dy, cv.dx)
    nx = -gx * F32(NSTR)
    ny = -gy * F32(NSTR)
    if nb != (0.0, 0.0, 1.0):
        nx = nx + F32(nb[0])
        ny = ny + F32(nb[1])
        nzz = np.full(H.shape, nb[2], F32)
    else:
        nzz = np.ones(H.shape, F32)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nzz * nzz)
    return nx * inv, ny * inv, nzz * inv


def render_blade(cv, bl, pose):
    P = bl.P
    X, Y = pose.to_canvas(bl.opts[:, 0], bl.opts[:, 1])
    pad = int(3 + 2.0 * max(P['tooth'], 0.3) / min(cv.dx, cv.dy))
    bb = cv.bbox(X, Y, pad_px=min(pad, 12))
    if bb is None:
        return
    Xg, Yg = cv.grid(*bb)
    A, B = pose.to_local(Xg, Yg)
    sx, sy = pose.steps(cv)
    aa_iso = F32(0.5 * (math.hypot(*sx) + math.hypot(*sy)))
    F = aa = None
    ms = []
    main = None
    for k, lobe in enumerate(bl.lobes):
        Fk, mk, aak, s, blat, sidei, hw = lobe.field(A, B, sx, sy)
        if F is None:
            F, aa = Fk, aak
            main = (s, blat, sidei, hw)
        else:
            aa = np.where(Fk < F, aak, aa)
            F = smin(F, Fk, F32(bl.ksin))
        ms.append(mk)
    aa = np.maximum(aa, 1e-4)
    cov = np.clip(0.5 - F / aa, 0.0, 1.0)
    if not cov.any():
        return
    own = None
    if len(ms) > 1:
        Ms = np.stack(ms)
        part = np.partition(Ms, 1, axis=0)
        m1, m2 = part[0], part[1]
        own = []
        for k in range(len(ms)):
            other = np.where(Ms[k] <= m1, m2, m1)
            own.append(np.clip((other - Ms[k]) / 0.08 + 0.5, 0.0, 1.0).astype(F32))
        del Ms, part
    s, blat, sidei, hw = main
    lobe0 = bl.lobes[0]
    seed = bl.seed % 100000
    L = bl.L
    acc = Acc(A.shape)
    raster_veins(bl, A, B, pose, cv, bb, sx, sy, own, acc, P['gwide'])
    dm = np.maximum(-F, 0.0)
    px = float(aa_iso)
    mott = nzc('fbm', A, B, 0.7 * L, seed, px)
    mid = nzc('band20', A, B, 2.5 + 0.03 * L, seed + 3, px)
    cid = None
    cid3 = None
    Dq = None
    if P['tert'] == 'ret':
        wa = A + F32(0.3 * P['tret']) * nzc('warp', A, B, 4.0 * P['tret'], seed + 5, px)
        wb = B + F32(0.3 * P['tret']) * nzc('warp', A, B, 4.0 * P['tret'], seed + 6, px)
        e, cid3 = worley(wa, wb, P['tret'], seed + 11, want_id=True)
        add_net(acc, e, P['wter'], aa_iso, 0.18, 0.3, 0.28, 0.09, P['tgroove'] * P['vimp'],
                gw=max(3.0 * P['wter'], 0.16 * P['tret']))
        del e
    if P['tert'] == 'perc':
        cq3 = 1.5 * P['tsp']
        wa = A + F32(0.3 * cq3) * nzc('warp', A, B, 4.0 * cq3, seed + 15, px)
        wb = B + F32(0.3 * cq3) * nzc('warp', A, B, 4.0 * cq3, seed + 16, px)
        cid3 = coarse(lambda x, y: worley(x, y, cq3, seed + 17, want_id=True)[1], wa, wb, 2)
        del wa, wb
    if P['areole'] > 0.0:
        ar = P['areole']
        wa = A + F32(0.25 * ar) * nzc('warp', A, B, 5.0 * ar, seed + 8, px)
        wb = B + F32(0.25 * ar) * nzc('warp', A, B, 5.0 * ar, seed + 9, px)
        e, cid = worley(wa, wb, ar, seed + 12, want_id=True)
        add_net(acc, e, P['ware'], aa_iso, 0.05, 0.12, 0.12, 0.035, 0.15 * P['vimp'])
        Dq = np.maximum(e - F32(0.5 * P['ware']), 0.0)
        del e, wa, wb
    hwm = float(lobe0.hw_t.max())
    vimp = max(P['vimp'], 1e-4)
    k_out = max(cv.n // SIZE, 1)
    # ---- Relief (mm). Every term runs on past the outline (none falls off at the margin): the margin's normal has
    # no cliff, the teeth no beads.
    Hh = acc.h * F32(P['vraise']) if P['vraise'] > 0.0 else -acc.h
    dome = None
    if P['rug'] > 0.0:
        # Each field between the major veins domed (pleats between hazel's secondaries).
        lam2 = field_scale(bl)
        D2 = major_dist(bl, A, B, pose, cv, bb, own, 2.5 * lam2)
        dome = 1.0 - np.exp(-(D2 * F32(1.0 / lam2)) ** 2)
        if P['quilt'] > 0.0:
            cq = 1.3 * lam2
            wa = A + F32(0.3 * cq) * nzc('warp', A, B, 3.0 * cq, seed + 71, px)
            wb = B + F32(0.3 * cq) * nzc('warp', A, B, 3.0 * cq, seed + 72, px)
            eq = coarse(lambda x, y: worley(x, y, cq, seed + 73)[0], wa, wb, 2)
            dome = dome * (1.0 - F32(P['quilt']) * np.exp(-(eq * F32(1.0 / (0.3 * cq))) ** 2))
            del eq, wa, wb
        Hh = Hh + F32(P['rug']) * dome
        del D2
    if Dq is not None and P['cush'] > 0.0:
        Hh += F32(P['cush']) * (1.0 - np.exp(-(Dq * F32(1.0 / (0.4 * P['areole']))) ** 2))
    vw = 0.35 * hwm
    Hh += F32(P['vfold'] * vw) * np.tanh(np.abs(blat) * F32(1.0 / vw))
    wave = None
    if P['wavy_z'] > 0.0:
        arcs = lobe0.look(np.concatenate(lobe0.arc).astype(F32), s, sidei)
        ph = F32(seed % 17)
        wave = np.sin(arcs * F32(2 * math.pi / P['wavy_len']) + ph + sidei * F32(1.7))
        Hh += F32(P['wavy_z']) * wave * (1.0 - sstep(0.0, 0.3 * hwm, dm)) ** 2
    if P['hair'] > 0.0:
        Hh += F32(0.0015 * P['hair']) * nz('band80', A, B, 0.3, seed + 14)
    # Its strength: the slope the species shows.
    kh = relief_gain(Hh, cv, pose, cov, dm, aa_iso, k_out, P['relief'])
    Hh *= F32(kh)
    if P['revolute'] > 0.0:
        # A margin rolled under (willows' narrow leaves): its own physical height, steepest (2 revolute / rw) a third
        # of the way in, flat at the very edge.
        rw = max(0.35, 3.0 * float(aa_iso), 0.08 * hwm)
        Hh -= F32(P['revolute']) * (1.0 - sstep(0.0, rw, dm)) ** 2
    # ---- Upper luminance. V0: the lamina's own variation (mottling, the areoles' cells, hairs, the margin, the domes'
    # crests lighter and their folds darker); Q: the veins (an impressed vein's line a little darker and its groove
    # darker beside it; a raised or glossy one lighter); M: the marks. V0's spread is set so the lamina's R spans
    # SPREAD between p5 and p95 about its mean of 1.
    V0 = 1.0 + F32(P['mottle']) * mott + F32(0.025) * mid + F32(0.015) * nzc('fbm', A, B, 0.2 * L, seed + 41, px)
    if cid is not None:
        V0 += F32(0.08) * (cid - 0.5)
    if cid3 is not None:
        V0 += F32(0.09) * (cid3 - 0.5)
    if P['hair'] > 0.0:
        V0 += F32(0.04 * P['hair']) * nz('band80', A, B, 0.3, seed + 4)
    md = max(1.0, 0.04 * L)
    V0 *= 1.0 - F32(P['margin_dark']) * (1.0 - sstep(0.0, md, dm))
    if dome is not None:
        V0 *= 1.0 + F32(P['dshade']) * (dome - F32(0.6))
    if wave is not None:
        V0 *= 1.0 + F32(0.02) * wave * (1.0 - sstep(0.0, 0.45 * hwm, dm))
    groove = np.clip(acc.h * F32(1.0 / vimp), 0.0, 1.0)
    side_ = np.clip(groove - acc.r, 0.0, 1.0)
    Q = (1.0 + acc.r * F32(P['vtone'] - 1.0)) * (1.0 - F32(P['cav']) * side_)
    del groove, side_
    M = np.ones_like(V0)
    Bt = (1.0 - acc.b) * F32(1.0 - 0.12 * P['hair'])
    gl = F32(P['gloss']) * (1.0 + F32(0.07) * mid) * (1.0 - 0.4 * acc.g)
    un = F32(P['under']) * (1.0 + F32(0.025) * mott) + acc.u * F32(P['uvein'])
    # Glands (dark dots at the teeth, no relief), scales, dots, spots, pale patches, a browned margin.
    for (ga, gb, gr_) in bl.glands:
        w = window(A, pose, cv, bb, ga, gb, gr_)
        if w is None:
            continue
        d = np.hypot(A[w] - F32(ga), B[w] - F32(gb))
        M[w] *= 1.0 - 0.2 * (1.0 - sstep(0.5 * gr_, gr_, d))
    if P['scales'] > 0.0:
        e1, cs = worley(A, B, 0.22, seed + 21, jit=1.0, want_id=True, f1=True)
        dots = 1.0 - sstep(0.05, 0.085, e1)
        M += F32(0.2 * P['scales']) * dots * (0.6 + 0.4 * cs)
        gl = gl * (1.0 + 0.15 * dots)
    if P['rust'] > 0.0:
        e2, _ = worley(A + 13.1, B, 0.7, seed + 22, jit=1.0, f1=True)
        rd = 1.0 - sstep(0.07, 0.11, e2)
        un = un * (1.0 - F32(0.4 * P['rust']) * rd)
        un = un + F32(0.05) * nz('band80', A, B, 0.2, seed + 23)
    if P['glands_under'] > 0.0:
        e3, _ = worley(A + 7.7, B, 1.1, seed + 24, jit=1.0, f1=True)
        gd = 1.0 - sstep(0.08, 0.13, e3)
        un = un + F32(0.45 * P['glands_under']) * gd
    if P['gdots'] > 0.0:
        e4, _ = worley(A + 3.3, B, 0.8, seed + 25, jit=1.0, f1=True)
        gd = 1.0 - sstep(0.05, 0.08, e4)
        M *= 1.0 - F32(0.07) * gd
        un = un * (1.0 - F32(0.18) * gd)
        Bt = Bt * (1.0 + F32(0.15) * gd)
    for (sa, sb, sr, sk) in bl.spots:
        w = window(A, pose, cv, bb, sa, sb, 3.0 * sr)
        if w is None:
            continue
        Aw, Bw = A[w], B[w]
        d = np.hypot(Aw - F32(sa), Bw - F32(sb))
        dn = d * (1.0 + 0.25 * nz('band20', Aw, Bw, 0.8 * sr, seed + 31))
        core = 1.0 - sstep(0.55 * sr, sr, dn)
        halo = (1.0 - sstep(sr, 2.4 * sr, dn)) - core
        if sk == 0:
            M[w] *= (1.0 - 0.3 * core) * (1.0 + 0.1 * halo)
        else:
            M[w] *= 1.0 + 0.2 * core + 0.08 * halo
        Bt[w] *= 1.0 - 0.3 * core
        gl[w] *= 1.0 - 0.4 * core
        un[w] *= 1.0 - 0.2 * core
    for (ca, cb, cr, camp) in bl.chlor:
        w = window(A, pose, cv, bb, ca, cb, 2.6 * cr)
        if w is None:
            continue
        Aw, Bw = A[w], B[w]
        dn = np.hypot(Aw - F32(ca), Bw - F32(cb)) * (1.0 + 0.3 * nz('fbm', Aw, Bw, 1.5 * cr, seed + 61))
        blob = np.exp(-(dn * F32(1.0 / cr)) ** 2)
        M[w] *= 1.0 + F32(camp) * blob
        un[w] *= 1.0 + F32(0.4 * camp) * blob
        gl[w] *= 1.0 - F32(0.5 * camp) * blob
    if bl.brown > 0.0:
        patchy = sstep(-0.7, 0.7, nzc('fbm', A, B, 0.35 * L, seed + 51, px))
        edge = dm * (1.0 + 0.45 * np.clip(nz('band20', A, B, 2.0, seed + 52), -1.5, 1.5))
        m = (1.0 - sstep(0.0, bl.brown, edge)) * (0.3 + 0.7 * patchy)
        M *= 1.0 - F32(0.2) * m
        gl = gl * (1.0 - F32(0.35) * m)
        Bt = Bt * (1.0 - F32(0.3) * m)
        un = un * (1.0 - F32(0.12) * m)
    QM = Q * M
    kv, mv = spread_gain(V0, QM, cov, acc.g, k_out, SPREAD)
    if os.environ.get('LEAF_DEBUG'):
        print('  blade %.0f mm: relief gain %.2f, spread gain %.2f' % (L, kh, kv))
    R = (1.0 + (V0 - F32(mv)) * F32(kv)) * QM
    del V0, Q, M, QM
    # The lamina's mean luminance at 1.
    lam_m = (cov > 0.6) & (acc.g < 0.05)
    if lam_m.sum() > 50:
        R = R / F32(float(R[lam_m].mean()))
    nx, ny, nzz = normals(Hh, cv, pose.nb)
    cv.over(bb, cov, dict(R=R, G=acc.g, B=np.minimum(Bt, 1.0), nx=nx, ny=ny, nz=nzz, gl=np.clip(gl, 0.0, 1.0), un=un))


def needle_part(c, ax, r0, a, phi, L, W, T, shade, twist=0.0, tilt=0.0, zc=0.0):
    """A juniper needle in 3D, projected on the card. Its base sits on the shoot (point c, direction ax on the card,
    radius r0) at azimuth phi (0 to the side, 90 toward the viewer); it leaves the shoot at a toward the shoot's tip:
    direction cos(a) axis + sin(a) (cos(phi) side + sin(phi) viewer). Its section is a flattened ellipse W x T, the
    upper face (with the white band) turned to the shoot and its tip, the keeled lower face outward; twist turns it a
    little out of its radial plane (no needle stands exactly radial). tilt: the shoot's tip turned that angle toward
    the viewer (the card sees it a little from its tip's side), zc the depth of its point c."""
    ax = np.asarray(ax, float) / np.linalg.norm(ax)
    sd = np.array([ax[1], -ax[0]])
    ca, sa, cp, sp_ = math.cos(a), math.sin(a), math.cos(phi), math.sin(phi)
    # (side, axis, viewer) components: the direction, the section's width and its upper face's normal.
    d3 = np.array([sa * cp, ca, sa * sp_])
    w3 = np.array([sp_, 0.0, -cp])
    n3 = np.array([-ca * cp, sa, -ca * sp_])
    if twist:
        # Turned about the needle's own normal: its direction swings toward its width (and back).
        ct, st = math.cos(twist), math.sin(twist)
        d3, w3 = d3 * ct + w3 * st, w3 * ct - d3 * st
    # The shoot frame tilted into the card's: (side, axis, viewer) -> (side, card axis, depth).
    cb, sb = math.cos(tilt), math.sin(tilt)

    def tl(v):
        return np.array([v[0], v[1] * cb - v[2] * sb, v[1] * sb + v[2] * cb])

    d3, w3, n3 = tl(d3), tl(w3), tl(n3)
    rad = tl(np.array([cp, 0.0, sp_]))
    P0 = np.asarray(c, float) + r0 * (rad[0] * sd + rad[1] * ax)
    proj = d3[0] * sd + d3[1] * ax
    l2 = L * float(np.hypot(d3[0], d3[1]))
    u2 = proj / np.linalg.norm(proj)
    e2 = np.array([u2[1], -u2[0]])
    es, ea = float(e2 @ sd), float(e2 @ ax)
    ha, hb = 0.5 * W, 0.5 * T
    # The section's half extent across the projected needle and the section angle of its widest point.
    wE, nE = es * w3[0] + ea * w3[1], es * n3[0] + ea * n3[1]
    hwp = math.hypot(ha * wE, hb * nE)
    # Depth relative to the shoot at the base and its change along the needle (the brush's self-shadow); the drawing
    # order by the absolute depth.
    zb = r0 * rad[2]
    zmid = zc + zb + 0.5 * L * d3[2]
    return ('needle', zmid, dict(P0=P0, P1=P0 + L * proj, u2=u2, e2=e2, l2=l2, hwp=hwp,
                                  th0=math.atan2(hb * nE, ha * wE), ha=ha, hb=hb, w3=w3, n3=n3, sd=sd, ax=ax,
                                  r0=r0, sa=sa, zb=zb, dz=L * d3[2], L=L, shade=shade))


def render_needle(cv, nd):
    """A needle's projection: straight, parallel-sided, a sharp point over its last third; each texel shows the
    nearer side of the section (its normal, the white band where the upper face turns to the viewer; the underside
    luminance from the far side); darker inside the brush and behind the shoot."""
    P0, P1, hwp = nd['P0'], nd['P1'], nd['hwp']
    bb = cv.bbox([P0[0] - hwp, P0[0] + hwp, P1[0]], [P0[1] - hwp, P0[1] + hwp, P1[1]], pad_px=3)
    if bb is None:
        return
    X, Y = cv.grid(*bb)
    u2, e2, l2 = nd['u2'], nd['e2'], nd['l2']
    qx = X - F32(P0[0])
    qy = Y - F32(P0[1])
    al = qx * F32(u2[0]) + qy * F32(u2[1])
    ac = qx * F32(e2[0]) + qy * F32(e2[1])
    t = np.clip(al * F32(1.0 / l2), 0.0, 1.0)
    tip = np.clip((1.0 - t) * F32(1.0 / 0.34), 0.0, 1.0)
    hw = F32(hwp) * tip ** F32(0.9) * (F32(0.8) + F32(0.2) * sstep(0.0, 0.08, t))
    slope = F32(hwp * 0.9 / (0.34 * l2)) * (tip < 1.0)
    F = (np.abs(ac) - hw) / np.sqrt(1.0 + slope * slope)
    F = np.where(al > l2, np.hypot(al - F32(l2), ac), F)
    F = np.where(al < 0.0, np.hypot(al, ac) - F32(0.8 * hwp), F)
    aa = F32(math.hypot(e2[0] * cv.dx, e2[1] * cv.dy))
    cov = np.clip(0.5 - F / aa, 0.0, 1.0)
    if not cov.any():
        return
    # The section point under each texel: u = cos(theta - th0) across the needle, the nearer root in front.
    ha, hb, w3, n3 = nd['ha'], nd['hb'], nd['w3'], nd['n3']
    c = np.arccos(np.clip(ac / np.maximum(hw, F32(1e-4)), -1.0, 1.0))
    t1 = F32(nd['th0']) + c
    t2 = F32(nd['th0']) - c

    def depth(th):
        return F32(ha * w3[2]) * np.cos(th) + F32(hb * n3[2]) * np.sin(th)

    near = depth(t1) >= depth(t2)
    thf = np.where(near, t1, t2)
    thb = np.where(near, t2, t1)
    cf, sf = np.cos(thf), np.sin(thf)
    ns = cf * F32(w3[0] / ha) + sf * F32(n3[0] / hb)
    na = cf * F32(w3[1] / ha) + sf * F32(n3[1] / hb)
    nv = np.maximum(cf * F32(w3[2] / ha) + sf * F32(n3[2] / hb), 0.0)
    sd, ax = nd['sd'], nd['ax']
    nl = np.sqrt(ns * ns + na * na + nv * nv) + F32(1e-6)
    # The section's own normal half blended with a round one across the stroke: every needle keeps a lit ridge
    # along it whatever way its faces turn (a thin needle reads by its silhouette and its highlight).
    u = np.clip(ac / np.maximum(hw, F32(1e-4)), -1.0, 1.0)
    rz = np.sqrt(np.clip(1.0 - F32(0.8) * u * u, 0.0, 1.0))
    k3 = JUNIPER['nsec']
    nx = F32(k3) * (ns * F32(sd[0]) + na * F32(ax[0])) / nl + F32((1.0 - k3) * 0.9 * e2[0]) * u
    ny = F32(k3) * (ns * F32(sd[1]) + na * F32(ax[1])) / nl + F32((1.0 - k3) * 0.9 * e2[1]) * u
    nzz = F32(k3) * nv / nl + F32(1.0 - k3) * rz + F32(0.1)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nzz * nzz)
    nx, ny, nzz = nx * inv, ny * inv, nzz * inv
    alk = sstep(0.03, 0.1, t) * (1.0 - sstep(0.8, 0.92, t))

    def band(th):
        return sstep(0.25, 0.5, np.sin(th)) * (1.0 - sstep(0.5, 0.7, np.abs(np.cos(th)))) * alk

    bf = band(thf)
    bk = band(thb)
    # Self-shadow: darker deep inside the brush and behind the shoot (the underside sees the brush from behind).
    r3 = F32(nd['r0']) + t * F32(nd['L'] * nd['sa'])
    z3 = F32(nd['zb']) + t * F32(nd['dz'])
    inner = (1.0 - F32(0.3) * np.exp(-r3 * F32(1.0 / 3.0))) * F32(nd['shade'])
    zf = np.clip(z3 * F32(1.0 / 10.0), -1.0, 1.0)
    R = (F32(0.92) + F32(0.72) * bf) * inner * (F32(0.9) + F32(0.16) * zf)
    un = (F32(0.92) + F32(0.72) * bk) * inner * (F32(0.9) - F32(0.16) * zf)
    cv.over(bb, cov, dict(R=R, G=np.zeros_like(R), B=F32(0.3) - F32(0.1) * bf, nx=nx, ny=ny, nz=nzz,
                          gl=F32(0.72) - F32(0.35) * bf, un=un))


def render_tube(cv, pts, rad, pr):
    """A stalk: petiole, rachis, twig, bud or thorn, round in section."""
    pts = np.asarray(pts, float)
    rad = np.asarray(rad, float)
    rmax = float(rad.max())
    bb = cv.bbox(np.concatenate([pts[:, 0] - rmax, pts[:, 0] + rmax]),
                 np.concatenate([pts[:, 1] - rmax, pts[:, 1] + rmax]), pad_px=3)
    if bb is None:
        return
    X, Y = cv.grid(*bb)
    shape = (bb[1] - bb[0], bb[3] - bb[2])
    best = np.full(shape, 1e9, F32)
    E = np.zeros(shape, F32)
    T = np.zeros(shape, F32)
    RR = np.ones(shape, F32)
    PX = np.ones(shape, F32)
    PY = np.zeros(shape, F32)
    AA = np.ones(shape, F32)
    D = np.full(shape, 1e9, F32)
    cum = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(pts, axis=0).T))])
    for k in range(len(pts) - 1):
        p0, p1 = pts[k], pts[k + 1]
        r0, r1 = rad[k], rad[k + 1]
        ex, ey = p1 - p0
        ln = math.hypot(ex, ey)
        if ln < 1e-6:
            continue
        ux, uy = ex / ln, ey / ln
        px_, py_ = uy, -ux
        R_ = max(r0, r1) + 2.0 * max(cv.dx, cv.dy)
        c0 = (min(p0[0], p1[0]) - R_) / cv.dx + cv.n * 0.5 - 0.5 - bb[2]
        c1 = (max(p0[0], p1[0]) + R_) / cv.dx + cv.n * 0.5 - 0.5 - bb[2]
        q0 = (min(p0[1], p1[1]) - R_) / cv.dy - 0.5 - bb[0]
        q1 = (max(p0[1], p1[1]) + R_) / cv.dy - 0.5 - bb[0]
        ja, jb = max(int(c0) - 1, 0), min(int(c1) + 2, shape[1])
        ia, ib = max(int(q0) - 1, 0), min(int(q1) + 2, shape[0])
        if ja >= jb or ia >= ib:
            continue
        sl = (slice(ia, ib), slice(ja, jb))
        Xs = X[:, ja:jb] - F32(p0[0])
        Ys = Y[ia:ib, :] - F32(p0[1])
        t = np.clip((Xs * F32(ux) + Ys * F32(uy)) * F32(1.0 / ln), 0.0, 1.0)
        qx = Xs - t * F32(ex)
        qy = Ys - t * F32(ey)
        d = np.sqrt(qx * qx + qy * qy)
        r = F32(r0) + F32(r1 - r0) * t
        key = d - r
        upd = key < best[sl]
        best[sl] = np.where(upd, key, best[sl])
        D[sl] = np.where(upd, d, D[sl])
        E[sl] = np.where(upd, qx * F32(px_) + qy * F32(py_), E[sl])
        T[sl] = np.where(upd, F32(cum[k]) + t * F32(ln), T[sl])
        RR[sl] = np.where(upd, r, RR[sl])
        PX[sl] = np.where(upd, F32(px_), PX[sl])
        PY[sl] = np.where(upd, F32(py_), PY[sl])
        AA[sl] = np.where(upd, F32(math.hypot(px_ * cv.dx, py_ * cv.dy)), AA[sl])
    RR = np.maximum(RR, 1e-3)
    cov = band_cov(D, 2.0 * RR, AA)
    if not cov.any():
        return
    e = np.clip(E / RR, -1.0, 1.0)
    th = np.arcsin(e)
    arc_ = th * RR
    seed = pr.get('seed', 0) % 100000
    kind = pr.get('tex', 'bark')
    R = np.full(shape, pr.get('R', 0.9), F32)
    # Fibres along the stalk, finer across.
    streak = nz('band20', T * F32(0.08), arc_ * F32(2.5), 1.0, seed)
    R += F32(pr.get('streak', 0.05)) * streak
    ee = e.copy()
    if pr.get('groove', 0.0) > 0.0:
        gz = np.clip(1.0 - np.abs(e) / 0.45, 0.0, 1.0)
        ee = e - F32(pr['groove']) * np.sign(e) * gz * 0.9
        R *= 1.0 - F32(0.05) * gz
    if kind in ('ridged', 'ridged4'):
        k = 4.0 if kind == 'ridged4' else 6.0
        wv = np.cos(k * th + seed % 5)
        ee = ee + F32(0.25) * np.sin(k * th + seed % 5) * np.sqrt(np.clip(1.0 - e * e, 0.0, 1.0))
        R *= 1.0 + F32(0.07) * wv
    if kind in ('lent', 'gloss', 'hair', 'bark'):
        cell = 1.4 if kind == 'lent' else 2.2
        e1, _ = worley(T * F32(0.55), arc_ * F32(1.4), cell, seed + 3, jit=1.0, f1=True)
        dot = 1.0 - sstep(0.1, 0.17, e1)
        R += F32(0.28 if kind == 'lent' else 0.12) * dot
    if kind == 'warts':
        e1, _ = worley(T, arc_ * F32(1.2), 1.6, seed + 4, jit=1.0, f1=True)
        dot = 1.0 - sstep(0.18, 0.3, e1)
        R *= 1.0 - F32(0.35) * dot
        ee = ee + F32(0.2) * dot * np.sign(e)
    if kind in ('hair', 'scales'):
        R += F32(0.06) * nz('band80', T, arc_, 0.2, seed + 5)
    if kind == 'scales':
        R += F32(0.12)
    if kind == 'bud':
        R *= 1.0 + F32(0.12) * np.cos(T * F32(2 * math.pi / max(pr.get('scale_len', 1.2), 0.3)))
    if kind == 'gland':
        ee = -0.5 * e
    ee = np.clip(ee, -0.97, 0.97)
    nzz = np.sqrt(1.0 - ee * ee)
    nx = ee * PX
    ny = ee * PY
    G = np.full(shape, pr.get('G', 1.0), F32)
    Bt = np.full(shape, pr.get('B', 0.0), F32)
    gl = np.full(shape, pr.get('gl', 0.3), F32) * (1.0 + F32(0.1) * streak)
    un = R * F32(pr.get('un', 1.0)) if pr.get('un_from_R', True) else np.full(shape, pr['un'], F32)
    cv.over(bb, cov, dict(R=R, G=G, B=Bt, nx=nx, ny=ny, nz=nzz, gl=gl, un=un))


# ======================================================================================================
# Scenes
# ======================================================================================================

def species_dims(sid, scale=1.0):
    a = SPEC[sid]['arch']
    return mm(mean2(a['leaf_len'])) * scale, mm(mean2(a['leaf_wid'])) * scale, mm(a['petiole']) * scale


def tube_pts(x, y, theta, length, n=8, bow=0.0):
    t = np.linspace(0.0, 1.0, n)
    d = np.array([math.sin(theta), math.cos(theta)])
    p = np.array([math.cos(theta), -math.sin(theta)])
    return np.array([x, y]) + t[:, None] * length * d + (bow * length * 4.0 * t * (1.0 - t))[:, None] * p


def petiole_props(P, seed):
    return dict(R=P['pet_R'], G=1.0, B=0.15, gl=0.45, un=1.05, un_from_R=False, groove=P['pet_groove'],
                tex='petiole', streak=0.035, seed=seed)


def add_leaf(parts, sid, x, y, theta, fa=1.0, fb=1.0, flip=False, nb=(0.0, 0.0, 1.0), scale=1.0, seed=0, z=0.0,
             blem=0, petiole=True, chlor=0.0, brown=False, form_b=False):
    """One leaf of the species (its petiole and blade; a trifoliate leaf's three leaflets) at (x, y)."""
    P = MORPH[sid]
    L, W, pet = species_dims(sid, scale)
    rng = np.random.default_rng(seed)
    d = (math.sin(theta), math.cos(theta))
    if sid in TRIFOLIATE:
        tf = TRIFOLIATE[sid]
        pet_d = pet * fa
        if petiole:
            parts.append(('tube', z, tube_pts(x, y, theta, pet_d + 0.4, 6, rng.uniform(-0.04, 0.04)),
                          np.linspace(P['pet_r'] * 1.1, P['pet_r'] * 0.8, 6) * scale ** 0.5,
                          petiole_props(P, seed)))
        bx, by = x + d[0] * pet_d, y + d[1] * pet_d
        lt = L * 0.86
        for q, ang in enumerate((0.0, tf['spread'], -tf['spread'])):
            sc = 1.0 if q == 0 else rng.uniform(0.78, 0.88)
            ll = lt * sc
            bl = build_blade(P, ll, ll * tf['ratio'], seed_of(seed, q), blem=0, chlor=chlor)
            th = theta + (ang + rng.uniform(-5, 5)) * DEG * (1.0 if not flip else -1.0)
            st = (0.07 if q == 0 else 0.045) * ll * scale ** 0.5
            sdx, sdy = math.sin(th) * fa, math.cos(th) * fa
            parts.append(('tube', z + 0.005, np.array([[bx, by], [bx + sdx * (st + 0.3), by + sdy * (st + 0.3)]]),
                          np.array([0.6, 0.45]) * P['pet_r'], petiole_props(P, seed + q)))
            parts.append(('blade', z + 0.01 + 0.001 * q, bl, Pose(bx + sdx * st, by + sdy * st, th, fa, fb, flip, nb)))
        return
    lobes = (LOBES_B.get(sid) if form_b else None) or LOBES.get(sid)
    bl = build_blade(P, L, W, seed, lobes=lobes, blem=blem, chlor=chlor, brown=brown)
    if lobes:
        # A lobed leaf keeps its lobes' angles; its width comes part way toward the species' own.
        wd = bl.ext[3] - bl.ext[2]
        bl.xscale = float(np.clip(W / wd, 0.85, 1.15))
    if petiole:
        pd = pet * fa
        ext = min(0.07 * L, 4.0)
        pts = tube_pts(x, y, theta, pd + ext, 10, rng.uniform(-0.05, 0.05))
        r0 = P['pet_r'] * min(scale, 1.0) ** 0.5
        rm = max(0.5 * P['wmid'] * L, 0.08)
        rad = np.interp(np.linspace(0.0, pd + ext, 10), [0.0, pd, pd + ext], [r0 * 1.15, r0 * 0.85, min(rm, r0 * 0.6)])
        parts.append(('tube', z, pts, rad, petiole_props(P, seed)))
        if P['pet_glands']:
            ng = P['pet_glands']
            for q in range(ng):
                t = 0.72 + 0.08 * q + rng.uniform(-0.03, 0.03)
                sg = 1.0 if q % 2 == 0 else -1.0
                gx = x + d[0] * pd * t + d[1] * sg * P['pet_r'] * 0.9
                gy = y + d[1] * pd * t - d[0] * sg * P['pet_r'] * 0.9
                gr = P['pet_r'] * (0.75 if sid == 'vib_opu' else 0.5)
                parts.append(('tube', z + 0.005, np.array([[gx, gy], [gx + 0.01, gy + 0.01]]), np.array([gr, gr]),
                              dict(R=0.85, G=1.0, B=0.1, gl=0.7, tex='gland', streak=0.0, seed=seed + q)))
        x, y = x + d[0] * pd, y + d[1] * pd
    parts.append(('blade', z + 0.01, bl, Pose(x, y, theta, fa, fb * bl.xscale, flip, nb)))


def scene_leaf(sid, var):
    """A leaf layer: the species' leaf on its petiole, base at v 0, the midrib at u 0.5."""
    parts = []
    seed = seed_of(sid, var, 'leaf')
    rng = np.random.default_rng(seed)
    # Blemishes, pale patches and browned margins on the b layers only.
    b = var == 'b'
    add_leaf(parts, sid, 0.0, 0.0, 0.0, seed=seed, blem=(1 if b else 0), chlor=(0.6 if b else 0.0),
             brown=(b and rng.random() < 0.5), form_b=b)
    kind = 'lobed' if sid in LOBES else 'simple'
    return dict(parts=parts, hull=(kind == 'simple'), kind=kind)


def scene_leaflet(sid):
    """A compound species' single leaflet (the middle distance's cells)."""
    P = MORPH[sid]
    a = SPEC[sid]['arch']
    if sid in TRIFOLIATE:
        L = mm(mean2(a['leaf_len'])) * 0.92
        W = L * TRIFOLIATE[sid]['ratio']
    else:
        L = mm(mean2(a['leaf_len']))
        W = L * SPEC[sid]['draw'].get('leaflet_ratio', 0.55)
    seed = seed_of(sid, 'leaflet')
    bl = build_blade(P, L, W, seed)
    parts = []
    stalk = max(0.04 * L, 0.8)
    parts.append(('tube', 0.0, tube_pts(0.0, 0.0, 0.0, stalk + 0.6, 4), np.full(4, max(0.18, P['pet_r'] * 0.45)),
                  petiole_props(P, seed)))
    parts.append(('blade', 0.1, bl, Pose(0.0, stalk, 0.0)))
    return dict(parts=parts, hull=True, kind='simple')


def scene_compound(sid, var):
    """A whole compound leaf: petiole, rachis, the leaflet pairs and the terminal leaflet."""
    P = MORPH[sid]
    a = SPEC[sid]['arch']
    cp = COMPOUND[sid]
    n = cp['n'][0 if var == 'a' else 1]
    seed = seed_of(sid, var, 'compound')
    rng = np.random.default_rng(seed)
    ll = mm(mean2(a['leaf_len']))
    ratio = SPEC[sid]['draw'].get('leaflet_ratio', 0.55)
    pet = mm(a['petiole'])
    rach = mm(a['rachis'])
    ang = a['leaflet_angle']
    npairs = (n - 1) // 2
    parts = []
    top = pet + rach
    bow = rng.uniform(-0.02, 0.02)
    axis = tube_pts(0.0, 0.0, rng.uniform(-0.02, 0.02), top + 0.5, 24, bow)

    def axis_at(y):
        return float(np.interp(y, axis[:, 1], axis[:, 0]))

    rr = P['pet_r']
    rad = np.interp(axis[:, 1], [0.0, pet, top], [rr * 1.25, rr, rr * 0.6])
    parts.append(('tube', 1.0, axis, rad, petiole_props(P, seed)))
    if cp.get('stipules'):
        sl_, sw_ = cp['stipules']
        for sg in (1.0, -1.0):
            Ps = dict(P, smax=0.55, eb=0.6, ea=1.6, full=1.6, acu=None, margin='glandular', tooth=0.12,
                      period=0.7, t_from=0.1, t_to=0.95, vtype='none', tert=None, areole=0.35, ware=0.03, rug=0.0,
                      cord=None, asym=0.0)
            Ls = pet * sl_ + 4.0
            bl = build_blade(Ps, Ls, Ls * sw_ * 0.5, seed_of(seed, 'stip', sg))
            parts.append(('blade', 0.5, bl, Pose(sg * rr * 0.6, 0.6, sg * 9.0 * DEG, flip=(sg < 0))))
    lam = ll * ratio
    for j in range(npairs):
        t = j / max(npairs - 1, 1) if npairs > 1 else 0.0
        g_last = 0.45 * rach / max(npairs, 1)
        y = pet + ((rach - g_last) * j / (npairs - 1) if npairs > 1 else 0.0) + rng.uniform(-0.02, 0.02) * rach
        sz = cp['sizes_b'] if (var == 'b' and cp.get('sizes_b')) else cp['sizes']
        size = sz[0] + (sz[1] - sz[0]) * t
        for sg in (1.0, -1.0):
            yy = y + sg * rng.uniform(0.0, 0.03) * rach
            sc = size * rng.uniform(0.94, 1.06)
            L = ll * sc
            W = lam * sc
            th = sg * (ang + 8.0 * (0.5 - t) + rng.uniform(-6.0, 6.0)) * DEG
            lobes = None
            if cp.get('lobed_lateral') and cp['lobed_lateral'][0 if var == 'a' else 1]:
                lobes = [dict(psi=0, len=1.0, wid=W / L),
                         dict(psi=78, len=0.42, wid=0.3, a0=0.06, P=dict(nsec=(2, 3), t_from=0.2))]
            Pl = dict(P)
            if sid.startswith('rub'):
                Pl['asym'] = 0.18
            mark = rng.random()
            bl = build_blade(Pl, L, W, seed_of(seed, j, sg), lobes=lobes, blem=(1 if var == 'b' and mark < 0.3 else 0),
                             chlor=(0.3 if var == 'b' else 0.0), brown=(var == 'b' and mark > 0.8))
            x0 = axis_at(yy)
            st = cp['stalk'] * L
            dvec = (math.sin(th), math.cos(th))
            if st > 0.3:
                parts.append(('tube', 0.2, tube_pts(x0, yy, th, st + 0.5, 4), np.full(4, rr * 0.45),
                              petiole_props(P, seed + j)))
            parts.append(('blade', 0.3 + 0.01 * j, bl, Pose(x0 + dvec[0] * st, yy + dvec[1] * st, th,
                                                           flip=(sg < 0))))
    sc = cp['term'] * rng.uniform(0.97, 1.03)
    L = ll * sc
    Pt = dict(P)
    if cp.get('cordate_term'):
        Pt['cord'] = (0.05, 0.03)
    bl = build_blade(Pt, L, lam * sc, seed_of(seed, 'term'), blem=(1 if var == 'b' else 0),
                     chlor=(0.4 if var == 'b' else 0.0), brown=(var == 'b' and rng.random() < 0.5))
    st = cp['tstalk'] * L
    th = rng.uniform(-3.0, 3.0) * DEG
    x0 = axis_at(top)
    if st > 0.3:
        parts.append(('tube', 0.2, tube_pts(x0, top, th, st + 0.6, 4), np.full(4, rr * 0.5), petiole_props(P, seed)))
    parts.append(('blade', 0.4, bl, Pose(x0 + math.sin(th) * st, top + math.cos(th) * st, th)))
    return dict(parts=parts, hull=False, kind='compound')


def twig_props(sid, seed):
    c = CARD[sid]
    tex_ = c['tex']
    return dict(R=0.9, G=1.0, B=0.0, gl=0.45 if tex_ == 'gloss' else 0.25, tex=tex_, streak=0.06, seed=seed)


def scene_card(sid):
    """A spray: its stretch of shoot seen from the side with the leaves round it."""
    a = SPEC[sid]['arch']
    c = CARD[sid]
    seed = seed_of(sid, 'card')
    rng = np.random.default_rng(seed)
    n = max(int(round(a['leaflets'])), 1)
    per = int(a.get('leaf_per_node', 1))
    phyl = a.get('leaf_phyllo', 137.5) * DEG
    splay = mean2(a['leaf_angle']) * DEG
    L, W, pet = species_dims(sid)
    spray = mm(a['spray_len'])
    proj = (pet + L) * math.cos(splay)
    Lc = max(spray, proj + 0.5 * spray)
    nodes = int(math.ceil(n / per))
    y_lo, y_hi = 0.07 * Lc, max(Lc - proj - 0.02 * Lc, 0.12 * Lc)
    ys = np.linspace(y_lo, y_hi, nodes) if nodes > 1 else np.array([y_lo])
    if nodes > 1:
        ys += rng.uniform(-0.15, 0.15, nodes) * (y_hi - y_lo) / (nodes - 1)
    r_tw = c['r']
    # The shoot: a little zigzag at the nodes, tapering.
    knots = [(0.0, -0.5)]
    for q, y in enumerate(ys):
        knots.append(((0.25 if q % 2 else -0.25) * r_tw + rng.uniform(-0.1, 0.1), y))
    knots.append((rng.uniform(-0.4, 0.4) * r_tw, Lc + 0.5))
    kn = np.array(knots)
    yy = np.linspace(-0.5, Lc + 0.5, 60)
    xx = np.interp(yy, kn[:, 1], kn[:, 0])
    tw = np.stack([xx, yy], 1)
    parts = [('tube', 0.0, tw, np.linspace(r_tw, 0.85 * r_tw, 60), twig_props(sid, seed))]
    az0 = rng.uniform(0.0, 2.0 * math.pi)
    for j in range(n):
        node = j // per
        m = j % per
        y = float(ys[node])
        az = az0 + node * phyl + m * math.pi + rng.uniform(-0.35, 0.35)
        if per == 1 and math.cos(az) * (1.0 if node % 2 else -1.0) < 0.0:
            az = math.pi - az
        s = splay + rng.uniform(-8.0, 8.0) * DEG
        dx, dy = math.sin(s) * math.cos(az), math.cos(s)
        th = math.atan2(dx, dy)
        fa = math.hypot(dx, dy)
        roll = rng.uniform(0.0, 65.0) * DEG
        fb = 0.5 + 0.5 * math.cos(roll)
        tilt = math.acos(min(fb, 1.0)) * float(rng.choice([-1.0, 1.0]))
        pitch = math.acos(min(fa, 1.0)) * (1.0 if math.sin(az) > 0 else -1.0) * 0.6
        nbv = (math.cos(th) * math.sin(tilt) + math.sin(th) * math.sin(pitch),
               -math.sin(th) * math.sin(tilt) + math.cos(th) * math.sin(pitch), 1.0)
        depth = math.sin(az)
        x0 = float(np.interp(y, yy, xx)) + r_tw * 0.75 * math.cos(az)
        add_leaf(parts, sid, x0, y, th, fa, fb, flip=bool(rng.random() < 0.5), nb=nbv,
                 scale=rng.uniform(0.86, 1.05), seed=seed_of(seed, j), z=depth)
        # (the draws the cards' former blemishes took: the layouts stay)
        if rng.random() >= 0.15:
            rng.random()
        if c.get('bud', 0.0) > 0.0:
            bl_ = c['bud'] * rng.uniform(0.8, 1.1)
            bx = float(np.interp(y, yy, xx)) + math.copysign(r_tw * 0.6, dx if abs(dx) > 1e-3 else 1.0)
            bth = math.copysign(12.0 * DEG, dx if abs(dx) > 1e-3 else 1.0)
            parts.append(('tube', depth - 0.002, tube_pts(bx, y + 0.3, bth, bl_, 6),
                          np.array([0.45, 0.6, 0.62, 0.55, 0.35, 0.05]) * bl_ / 2.6,
                          dict(R=0.82, G=1.0, B=0.0, gl=0.45, tex='bud', scale_len=bl_ * 0.45, streak=0.03,
                               seed=seed + j)))
        if c.get('stip', 0.0) > 0.0:
            Ps = dict(MORPH[sid], smax=0.5, eb=0.5, ea=0.6, full=1.3, acu=None, cord=None,
                      margin='glandular', tooth=0.1, period=0.8, vtype='none', tert=None, areole=0.4, rug=0.0,
                      asym=0.0)
            for sg in (1.0, -1.0):
                ls = c['stip'] * rng.uniform(0.8, 1.1)
                bl = build_blade(Ps, ls, ls * 0.75, seed_of(seed, j, 'st', sg))
                ox = x0 + math.sin(th) * pet * fa * 0.2
                parts.append(('blade', depth + 0.004, bl,
                              Pose(ox, y + 0.3, th + sg * 50.0 * DEG, fa, fb, sg < 0, nbv)))
    if c.get('thorn'):
        y = float(rng.uniform(0.3, 0.6) * Lc)
        sg = float(rng.choice([-1.0, 1.0]))
        x0 = float(np.interp(y, yy, xx))
        th = sg * rng.uniform(40.0, 55.0) * DEG
        ln = c['thorn'] * rng.uniform(0.8, 1.1)
        parts.append(('tube', 0.001, tube_pts(x0, y, th, ln, 8, 0.03 * sg),
                      np.linspace(0.75 * r_tw, 0.04, 8), twig_props(sid, seed + 99)))
    return dict(parts=parts, hull=False, kind='card')


# The juniper sprig (the whorls' step along the shoot and turn from the last, mm and degrees; the needles' angle to
# the shoot, length, width, thickness, twist; the shoot's radius; the share of the needles' own section normals; the
# view: the shoot's tip turned that much toward the viewer, so the needles in front cross the ones behind - the
# engine shows the sprig broadside, and a juniper shoot seen square from its side is a herringbone, a fern frond in a
# row of cards; seen 40 degrees off its side it is the prickly brush a bush shows; its small turns between whorls;
# the drawn shoot's length, mm: the shipped layers' 1.1 x 35 mm of leaf spacing - the species' spacing in
# make_shrubs.py moved on since, and its elements scale the square to their own length by leaf_fit).
JUNIPER = dict(step=(3.0, 3.6), turn=(25.0, 55.0), ang=(45.0, 75.0), length=12.5, ljit=0.25, width=(1.3, 1.7),
               thick=(0.6, 0.8), twist=14.0, r0=0.3, nsec=0.8, tilt=40.0, tilt_end=None, wobble=6.0, sprig=38.5)


def scene_needles(var):
    """A juniper sprig: a slightly bowed shoot along v with whorls of three needles every 3-3.7 mm, each whorl
    turned about 40 degrees from the last, the needles 10-15 mm at 45-75 degrees toward the tip, projected from 3D and
    drawn far to near: a prickly bottlebrush round a shoot seen only in its gaps."""
    J = JUNIPER
    seed = seed_of('jun_com', var, 'brush')
    rng = np.random.default_rng(seed)
    Lc = J['sprig']
    r0 = J['r0']
    tilt = J['tilt'] * DEG
    kz = math.cos(tilt)
    bow = float(rng.choice([-1.0, 1.0])) * rng.uniform(0.012, 0.03)
    pts = tube_pts(0.0, 0.0, rng.uniform(-0.03, 0.03), Lc, 40, bow)
    # The young shoot: green, ridged by the needles' decurrent bases, in the brush's shade (no bark shows on a
    # sprig: no twig flag).
    parts = [('tube', -0.5, pts, np.linspace(r0, 0.85 * r0, 40),
              dict(R=0.6, G=0.0, B=0.1, gl=0.4, tex='ridged', streak=0.06, seed=seed % 100000))]
    cum = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(pts, axis=0).T))])
    y = rng.uniform(0.8, 2.0) * kz
    az = rng.uniform(0.0, 2.0 * math.pi)
    while y < Lc - 0.5:
        c = np.array([np.interp(y, cum, pts[:, 0]), np.interp(y, cum, pts[:, 1])])
        k = min(int(np.searchsorted(cum, y)), len(pts) - 1)
        ax = pts[k] - pts[k - 1]
        # The shoot's own small turns between whorls (in the card and in depth).
        wa = rng.normal(0.0, J['wobble']) * DEG
        ax = np.array([ax[0] * math.cos(wa) + ax[1] * math.sin(wa), -ax[0] * math.sin(wa) + ax[1] * math.cos(wa)])
        te = J['tilt_end'] if J['tilt_end'] is not None else J['tilt']
        tw = (J['tilt'] + (te - J['tilt']) * y / Lc + rng.normal(0.0, J['wobble'])) * DEG
        for q in range(3):
            phi = az + q * 120.0 * DEG + rng.uniform(-10.0, 10.0) * DEG
            ang = rng.uniform(*J['ang']) * DEG
            ln = J['length'] * rng.uniform(1.0 - J['ljit'], 1.0 + J['ljit'])
            parts.append(needle_part(c, ax, r0, ang, phi, ln, rng.uniform(*J['width']), rng.uniform(*J['thick']),
                                     rng.uniform(0.93, 1.06), rng.uniform(-J['twist'], J['twist']) * DEG, tw,
                                     y * math.tan(tilt)))
        az += rng.uniform(*J['turn']) * DEG
        y += rng.uniform(*J['step']) * kz
    return dict(parts=parts, hull=False, kind='needles')


def compose_pose(R, t, scale, pose):
    """A part's pose carried into a card: the card's rotation R (2x2, det +-1), offset t (mm) and scale over the
    part's own (an exact rotation and scale; a foreshortened leaf keeps its own fa and fb)."""
    o = R @ np.array([pose.ox, pose.oy]) * scale + t
    d2 = R @ np.array(pose.d)
    n2 = R @ np.array(pose.n)
    th = math.atan2(d2[0], d2[1])
    flip = bool(n2[0] * math.cos(th) - n2[1] * math.sin(th) < 0.0)
    nb = np.array(pose.nb, float)
    nb[:2] = R @ nb[:2]
    return Pose(o[0], o[1], th, pose.fa * scale, pose.fb * scale, flip, tuple(nb))


def carry(parts, R, t, scale, z0=0.0):
    """A scene's parts carried into a card by the rotation R, offset t and scale (tubes exactly, blades by their
    poses)."""
    out = []
    for p in parts:
        if p[0] == 'blade':
            _, z, bl, pose = p
            out.append(('blade', z0 + 0.001 * z, bl, compose_pose(R, t, scale, pose)))
        elif p[0] == 'tube':
            _, z, pts, rad, pr = p
            q = (R @ (np.asarray(pts, float).T * scale)).T + t
            out.append(('tube', z0 + 0.001 * z, q, np.asarray(rad, float) * scale, pr))
    return out


def scene_palmate(sid, var):
    """A palmately compound leaf (horse chestnut): the petiole and its leaflets fanning from its tip, the middle one
    the largest."""
    P = MORPH[sid]
    a = SPEC[sid]['arch']
    pm = PALMATE[sid]
    n = pm['n'][0 if var == 'a' else 1]
    seed = seed_of(sid, var, 'palmate')
    rng = np.random.default_rng(seed)
    ll = mm(mean2(a['leaf_len']))
    ratio = SPEC[sid]['draw'].get('leaflet_ratio', 0.4)
    pet = mm(a['petiole'])
    parts = []
    rr = P['pet_r']
    pts = tube_pts(0.0, 0.0, rng.uniform(-0.02, 0.02), pet + 1.0, 14, rng.uniform(-0.03, 0.03))
    parts.append(('tube', 0.0, pts, np.linspace(rr * 1.3, rr * 1.0, 14), petiole_props(P, seed)))
    tip = pts[-2]
    half = n // 2
    for j in range(n):
        k = j - half
        rank = min(abs(k), len(pm['sizes']) - 1)
        sc = pm['sizes'][rank] * rng.uniform(0.95, 1.05)
        ang = (k / max(half, 1)) * pm['spread'] * rng.uniform(0.92, 1.05)
        L = ll * sc
        mark = rng.random()
        bl = build_blade(P, L, L * ratio, seed_of(seed, j), blem=(1 if var == 'b' and mark < 0.4 else 0),
                         chlor=(0.4 if var == 'b' else 0.0), brown=(var == 'b' and mark > 0.75))
        th = ang * DEG
        parts.append(('blade', 0.3 + 0.01 * (half - abs(k)), bl, Pose(tip[0] + math.sin(th) * 3.0,
                                                                         tip[1] + math.cos(th) * 3.0, th,
                                                                         flip=(k < 0))))
    return dict(parts=parts, hull=False, kind='compound')


def scene_compound_card(sid):
    """A compound-leaved species' spray for the far tiers: a stretch of shoot with its leaves (pairs or alternate),
    each a whole compound leaf turned out and foreshortened by its own angle, its buds."""
    a = SPEC[sid]['arch']
    c = CARD[sid]
    seed = seed_of(sid, 'card')
    rng = np.random.default_rng(seed)
    n = int(a.get('card_leaves', 4))
    per = int(a.get('card_per_node', 1))
    spacing = mm(a.get('card_spacing', 0.05))
    nodes = int(math.ceil(n / per))
    r_tw = c['r']
    length = spacing * (nodes + 0.6)
    yy = np.linspace(-0.5, length, 40)
    xx = 0.15 * r_tw * np.sin(yy / max(spacing, 1.0) * math.pi)
    parts = [('tube', 0.0, np.stack([xx, yy], 1), np.linspace(r_tw, 0.8 * r_tw, 40), twig_props(sid, seed))]
    builder = scene_palmate if sid in PALMATE else scene_compound
    for j in range(n):
        node = j // per
        m = j % per
        y = spacing * (node + 0.35) + rng.uniform(-0.08, 0.08) * spacing
        if per == 2:
            sgn = 1.0 if m == 0 else -1.0
        else:
            sgn = 1.0 if j % 2 == 0 else -1.0
        # The leaf's spread from the shoot (the top ones more upright) and its turn about its own axis.
        ang = (68.0 - 28.0 * node / max(nodes - 1, 1) + rng.uniform(-10.0, 10.0)) * DEG
        sub = builder(sid, 'a' if (j % 2 == 0) else 'b')['parts']
        th = sgn * ang
        R = np.array([[math.cos(th), math.sin(th)], [-math.sin(th), math.cos(th)]])
        sc = rng.uniform(0.85, 1.0)
        x0 = float(np.interp(y, yy, xx)) + sgn * r_tw * 0.6
        parts += carry(sub, R, np.array([x0, y]), sc, z0=0.1 + 0.1 * j)
        if c.get('bud', 0.0) > 0.0:
            bl_ = c['bud'] * rng.uniform(0.8, 1.1)
            parts.append(('tube', 0.05, tube_pts(x0, y + 0.5, sgn * 20.0 * DEG, bl_, 6),
                          np.array([0.45, 0.6, 0.62, 0.55, 0.35, 0.05]) * bl_ / 2.6,
                          dict(R=0.8, G=1.0, B=0.0, gl=0.45, tex='bud', scale_len=bl_ * 0.45, streak=0.03,
                               seed=seed + j)))
    if c.get('bud', 0.0) > 0.0:
        bl_ = c['bud'] * 1.3
        parts.append(('tube', 0.05, tube_pts(float(xx[-1]), length - 0.5, 0.0, bl_, 6),
                      np.array([0.5, 0.66, 0.68, 0.6, 0.4, 0.05]) * bl_ / 2.6,
                      dict(R=0.8, G=1.0, B=0.0, gl=0.45, tex='bud', scale_len=bl_ * 0.45, streak=0.03, seed=seed)))
    if c.get('thorn'):
        for k in range(nodes):
            y = spacing * (k + 0.35)
            for sg in (-1.0, 1.0):
                th = sg * rng.uniform(35.0, 55.0) * DEG
                x0 = float(np.interp(y, yy, xx))
                ln = c['thorn'] * rng.uniform(0.7, 1.1)
                parts.append(('tube', 0.002, tube_pts(x0, y - 1.0, th, ln, 8, 0.05 * sg),
                              np.linspace(0.6 * r_tw, 0.04, 8), twig_props(sid, seed + 99 + k)))
    return dict(parts=parts, hull=False, kind='card')


# ------------------------------------------------------------------------------------------------------
# Needles of the trees: Scots pine's pairs in their sheaths, Norway spruce's single needles on their pegs
# ------------------------------------------------------------------------------------------------------
# A needle tuft is a stretch of shoot in its needles drawn from a 3D model and projected as seen `tilt` degrees off
# the shoot's side (the near tier crosses two such quads about the shoot, the far tiers turn one to the eye): the
# shoot's tip comes toward the viewer, so the needles in front cross the ones behind.
# (A Scots pine's needles stand out wide all round the shoot, a bottle brush - at 24-58 degrees and seen at 35 the
# tufts and the far cards read as a fir's flat fishbone; the far crown a mass of stars.)
PINE = dict(shoot=110.0, r0=2.4, L=(45.0, 72.0), W=(1.2, 1.6), twist=(0.35, 0.9), step=(1.25, 1.95),
            ang_tip=40.0, ang_base=75.0, jit=18.0, pair=(6.0, 18.0), bow=(0.02, 0.12), tilt=42.0, sheath=5.0)
SPRUCE = dict(shoot=72.0, r0=1.1, L=(14.0, 22.0), W=(0.9, 1.1), step=(0.36, 0.52), ang_tip=48.0, ang_base=72.0,
              jit=12.0, bow=(0.02, 0.08), tilt=40.0, gain=1.2)
# (gain: the short needles' brush is deep in its own shade; their mean brought to the layers' 1.)


def view_point(p, tilt):
    """A shoot-frame point (side, along, toward the viewer) seen with the shoot's tip turned `tilt` toward the
    viewer: (X, Y) on the card and the depth Z."""
    ct, st = math.cos(tilt), math.sin(tilt)
    return np.array([p[0], p[1] * ct - p[2] * st]), p[1] * st + p[2] * ct


def needle_curve(base, d, L, bow_v, bow, k=9):
    """A needle's 3D centreline from its base along d, bowed by bow of its length toward bow_v."""
    t = np.linspace(0.0, 1.0, k)[:, None]
    return base[None, :] + L * t * d[None, :] + (4.0 * bow * L) * (t * (1.0 - t)) * bow_v[None, :]


def stroke_part(curve3, rad, half_w, tilt, shade, phase, twist, kind, r_axis, z_ref, band=1.0):
    """A needle as a tapered stroke on the card: its projected centreline, half widths, depths, the turn of its flat
    face (pine: twisted along it), its self-shade."""
    pts, zs = [], []
    for p in curve3:
        q, z = view_point(p, tilt)
        pts.append(q)
        zs.append(z)
    pts = np.array(pts)
    zs = np.array(zs)
    n = len(pts)
    u = np.linspace(0.0, 1.0, n)
    hw = half_w * np.clip((1.0 - u) / 0.16, 0.0, 1.0) ** 0.8 * (0.82 + 0.18 * np.clip(u / 0.06, 0.0, 1.0))
    hw[-1] = 0.0
    return ('stroke', float(zs.mean()), dict(pts=pts, hw=hw, z=zs, rad=rad, shade=shade, phase=phase, twist=twist,
                                              kind=kind, r_axis=r_axis, z_ref=z_ref, band=band))


def render_stroke(cv, nd):
    """A needle's stroke: the nearest point of its centreline under each texel, its width tapering to a sharp tip;
    the section's normal across it (round, a ridge along it), the pine's flat glaucous face where its twist turns it
    to the viewer (stomatal bands, paler and duller), darker deep in the brush and behind the shoot."""
    pts, hw = nd['pts'], nd['hw']
    rmax = float(hw.max())
    bb = cv.bbox(np.concatenate([pts[:, 0] - rmax, pts[:, 0] + rmax]),
                 np.concatenate([pts[:, 1] - rmax, pts[:, 1] + rmax]), pad_px=3)
    if bb is None:
        return
    X, Y = cv.grid(*bb)
    shape = (bb[1] - bb[0], bb[3] - bb[2])
    best = np.full(shape, 1e9, F32)
    E = np.zeros(shape, F32)
    U = np.zeros(shape, F32)
    RR = np.full(shape, 1e-3, F32)
    PX = np.ones(shape, F32)
    PY = np.zeros(shape, F32)
    AA = np.ones(shape, F32)
    D = np.full(shape, 1e9, F32)
    seg = np.hypot(*np.diff(pts, axis=0).T)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    tot = max(cum[-1], 1e-6)
    for k in range(len(pts) - 1):
        p0, p1 = pts[k], pts[k + 1]
        r0, r1 = hw[k], hw[k + 1]
        ex, ey = p1 - p0
        ln = math.hypot(ex, ey)
        if ln < 1e-6:
            continue
        ux, uy = ex / ln, ey / ln
        px_, py_ = uy, -ux
        R_ = max(r0, r1) + 2.0 * max(cv.dx, cv.dy)
        c0 = (min(p0[0], p1[0]) - R_) / cv.dx + cv.n * 0.5 - 0.5 - bb[2]
        c1 = (max(p0[0], p1[0]) + R_) / cv.dx + cv.n * 0.5 - 0.5 - bb[2]
        q0 = (min(p0[1], p1[1]) - R_) / cv.dy - 0.5 - bb[0]
        q1 = (max(p0[1], p1[1]) + R_) / cv.dy - 0.5 - bb[0]
        ja, jb = max(int(c0) - 1, 0), min(int(c1) + 2, shape[1])
        ia, ib = max(int(q0) - 1, 0), min(int(q1) + 2, shape[0])
        if ja >= jb or ia >= ib:
            continue
        sl = (slice(ia, ib), slice(ja, jb))
        Xs = X[:, ja:jb] - F32(p0[0])
        Ys = Y[ia:ib, :] - F32(p0[1])
        t = np.clip((Xs * F32(ux) + Ys * F32(uy)) * F32(1.0 / ln), 0.0, 1.0)
        qx = Xs - t * F32(ex)
        qy = Ys - t * F32(ey)
        d = np.sqrt(qx * qx + qy * qy)
        r = F32(r0) + F32(r1 - r0) * t
        key = d - r
        upd = key < best[sl]
        best[sl] = np.where(upd, key, best[sl])
        D[sl] = np.where(upd, d, D[sl])
        E[sl] = np.where(upd, qx * F32(px_) + qy * F32(py_), E[sl])
        U[sl] = np.where(upd, (F32(cum[k]) + t * F32(ln)) * F32(1.0 / tot), U[sl])
        RR[sl] = np.where(upd, np.maximum(r, F32(1e-3)), RR[sl])
        PX[sl] = np.where(upd, F32(px_), PX[sl])
        PY[sl] = np.where(upd, F32(py_), PY[sl])
        AA[sl] = np.where(upd, F32(math.hypot(px_ * cv.dx, py_ * cv.dy)), AA[sl])
    cov = band_cov(D, 2.0 * RR, AA)
    if not cov.any():
        return
    e = np.clip(E / RR, -1.0, 1.0)
    nzv = np.interp(U, np.linspace(0.0, 1.0, len(nd['z'])), nd['z']).astype(F32)
    radv = np.interp(U, np.linspace(0.0, 1.0, len(nd['rad'])), nd['rad']).astype(F32)
    if nd['kind'] == 'pine':
        # The flat inner face with its white stomatal lines shows where the twist turns it to the viewer.
        psi = F32(nd['phase']) + F32(2.0 * math.pi * nd['twist']) * U
        face = np.cos(psi)
        flat = sstep(0.15, 0.55, face) * (1.0 - sstep(0.75, 0.95, np.abs(e)))
        # The rounded back: a ridge of light along it; the flat face: flat, its normal toward the viewer.
        ee = e * (1.0 - 0.75 * flat)
        stom = flat * (0.7 + 0.3 * np.cos(e * F32(9.0) + F32(nd['phase'])) ** 2) * F32(nd['band'])
        gl = F32(0.62) - F32(0.3) * flat
    else:
        # A four-sided needle: a keel along its middle, faint stomatal lines on every face.
        ee = np.sign(e) * np.abs(e) ** 0.6 * 0.85
        stom = F32(0.18 * nd['band']) * (0.5 + 0.5 * np.cos(e * F32(7.0))) * sstep(0.05, 0.2, U)
        gl = np.full(shape, 0.66, F32)
    ee = np.clip(ee, -0.95, 0.95)
    nzz = np.sqrt(1.0 - ee * ee)
    nx = ee * PX
    ny = ee * PY
    # Self-shade: deep inside the brush (near the shoot) and behind the shoot darker.
    inner = (1.0 - F32(0.32) * np.exp(-radv * F32(1.0 / 4.0))) * F32(nd['shade'])
    behind = F32(0.86) + F32(0.14) * np.clip((nzv - F32(nd['z_ref'])) * F32(1.0 / 12.0) + 0.5, 0.0, 1.0)
    base = F32(0.9) + F32(0.08) * np.cos(U * F32(3.0) + F32(nd['phase']))
    R = (base + F32(0.55) * stom) * inner * behind
    un = (base + F32(0.35) * stom) * inner * (F32(1.86) - behind)
    cv.over(bb, cov, dict(R=R, G=np.zeros(shape, F32), B=F32(0.28) - F32(0.08) * stom, nx=nx, ny=ny, nz=nzz,
                          gl=np.clip(gl, 0.0, 1.0), un=un))


def shoot_parts(L, r0, tilt, kind, seed, z_ref=0.0, x0=0.0, y0=0.0, bend=0.0, k=24):
    """A shoot along v, its tip toward the viewer by `tilt`: tube segments each at its own depth (the needles round
    it sort in front of or behind it), the bark of a young shoot (twig flag on: the species' twig colour)."""
    parts = []
    ys = np.linspace(0.0, L, k + 1)
    for i in range(k):
        a3 = np.array([x0 + bend * (ys[i] / L) ** 2 * L, y0 + ys[i], 0.0])
        b3 = np.array([x0 + bend * (ys[i + 1] / L) ** 2 * L, y0 + ys[i + 1], 0.0])
        qa, za = view_point(a3, tilt)
        qb, zb = view_point(b3, tilt)
        r = r0 * (1.0 - 0.25 * ys[i] / L)
        parts.append(('tube', 0.5 * (za + zb), np.stack([qa, qb]), np.array([r, r * 0.98]),
                      dict(R=0.85, G=1.0, B=0.0, gl=0.3, tex=kind, streak=0.08, seed=seed + i)))
    return parts


def pine_brush(rng, L, r0, tilt, older=False, x0=0.0, y0=0.0, axis_turn=0.0, seed=0, buds=True, scale=1.0):
    """A Scots pine shoot in its needles (shoot frame: side, along, toward the viewer): pairs in grey-brown sheaths
    every few millimetres round the shoot by the golden angle, the young needles near the tip forward, the older ones
    spread; each needle 4-7 cm, twisted, gently bowed; the bud cluster at the tip."""
    Pn = PINE
    parts = []
    ca, sa = math.cos(axis_turn), math.sin(axis_turn)

    def to_frame(p):
        # The shoot turned in the card plane by axis_turn and placed at (x0, y0).
        return np.array([x0 + p[0] * ca + p[1] * sa, y0 - p[0] * sa + p[1] * ca, p[2]])

    # The shoot itself.
    ns = 18
    for i in range(ns):
        a3, b3 = to_frame(np.array([0.0, L * i / ns, 0.0])), to_frame(np.array([0.0, L * (i + 1) / ns, 0.0]))
        qa, za = view_point(a3, tilt)
        qb, zb = view_point(b3, tilt)
        r = r0 * (1.0 - 0.3 * i / ns) * scale
        parts.append(('tube', 0.5 * (za + zb) - 0.5, np.stack([qa, qb]), np.array([r, r * 0.98]),
                      dict(R=0.82, G=1.0, B=0.0, gl=0.3, tex='scales', streak=0.08, seed=seed + i)))
    y = rng.uniform(4.0, 8.0)
    az = rng.uniform(0.0, 2.0 * math.pi)
    step_k = 1.25 if older else 1.0
    while y < L - 3.0:
        s = y / L
        ang = (Pn['ang_base'] + (Pn['ang_tip'] - Pn['ang_base']) * s ** 0.8) * DEG
        if older:
            ang = ang + rng.uniform(5.0, 18.0) * DEG
        if older and rng.random() < 0.12:
            y += rng.uniform(*Pn['step']) * step_k
            az += 137.5 * DEG
            continue
        base_shoot = np.array([0.0, y, 0.0])
        Ln = rng.uniform(*Pn['L']) * (0.85 + 0.15 * min(s * 1.5, 1.0)) * scale
        pair = rng.uniform(*Pn['pair']) * DEG
        for q in (-1.0, 1.0):
            phi = az + q * 0.5 * pair + rng.normal(0.0, 0.05)
            a_ = ang + q * rng.uniform(0.0, 4.0) * DEG + rng.normal(0.0, Pn['jit'] * DEG * 0.5)
            radial = np.array([math.cos(phi), 0.0, math.sin(phi)])
            d = math.cos(a_) * np.array([0.0, 1.0, 0.0]) + math.sin(a_) * radial
            # Bowed a little, some toward the shoot's tip, some out.
            bow_v = np.array([0.0, 1.0, 0.0]) - d * d[1]
            bow_v = bow_v / max(np.linalg.norm(bow_v), 1e-6) * rng.choice([-1.0, 1.0])
            base = base_shoot + radial * r0 * scale
            c3 = needle_curve(base + d * Pn['sheath'] * 0.4 * scale, d, Ln, bow_v, rng.uniform(*Pn['bow']))
            rad = np.hypot(c3[:, 0], c3[:, 2])
            c3 = np.array([to_frame(p) for p in c3])
            parts.append(stroke_part(c3, rad, 0.5 * rng.uniform(*Pn['W']) * scale, tilt, rng.uniform(0.9, 1.06),
                                     rng.uniform(0.0, 2.0 * math.pi), rng.uniform(*Pn['twist']), 'pine',
                                     r0, view_point(to_frame(base_shoot), tilt)[1]))
        # The sheath binding the pair at its base.
        sb = base_shoot + np.array([math.cos(az), 0.0, math.sin(az)]) * r0 * scale
        sd = math.cos(ang) * np.array([0.0, 1.0, 0.0]) + math.sin(ang) * np.array([math.cos(az), 0.0, math.sin(az)])
        s0, z0 = view_point(to_frame(sb), tilt)
        s1, z1 = view_point(to_frame(sb + sd * Pn['sheath'] * scale), tilt)
        parts.append(('tube', 0.5 * (z0 + z1) + 0.01, np.stack([s0, s1]), np.array([0.75, 0.55]) * scale,
                      dict(R=0.78, G=1.0, B=0.0, gl=0.25, tex='bark', streak=0.05, seed=seed + int(y * 10))))
        y += rng.uniform(*Pn['step']) * step_k
        az += 137.5 * DEG
    if buds:
        # The bud cluster: a leading bud and a few round it, resinous red-brown scales.
        tipb = np.array([0.0, L - 1.0, 0.0])
        for j, (blen, ang) in enumerate([(10.0, 0.0)] + [(6.5, 28.0)] * int(rng.integers(2, 5))):
            phi = rng.uniform(0.0, 2.0 * math.pi)
            d = math.cos(ang * DEG) * np.array([0.0, 1.0, 0.0]) + math.sin(ang * DEG) * np.array(
                [math.cos(phi), 0.0, math.sin(phi)])
            pts3 = np.array([to_frame(tipb + d * blen * scale * t) for t in np.linspace(0.0, 1.0, 6)])
            q = np.array([view_point(p, tilt)[0] for p in pts3])
            z = float(np.mean([view_point(p, tilt)[1] for p in pts3]))
            parts.append(('tube', z + 0.2, q, np.array([1.6, 2.4, 2.5, 2.2, 1.4, 0.15]) * scale * blen / 10.0,
                          dict(R=0.82, G=1.0, B=0.0, gl=0.55, tex='bud', scale_len=2.2, streak=0.03,
                               seed=seed + 500 + j)))
    return parts


def spruce_brush(rng, L, r0, tilt, flat=False, x0=0.0, y0=0.0, axis_turn=0.0, seed=0, scale=1.0, buds=True):
    """A Norway spruce twig in its needles: single four-sided needles 1.5-2.5 cm on pegs all round the twig, densest
    and longest on top, pointing forward; `flat`: a shade shoot's needles parted to the sides (a comb)."""
    Sp = SPRUCE
    parts = []
    ca, sa = math.cos(axis_turn), math.sin(axis_turn)

    def to_frame(p):
        return np.array([x0 + p[0] * ca + p[1] * sa, y0 - p[0] * sa + p[1] * ca, p[2]])

    ns = 14
    for i in range(ns):
        a3, b3 = to_frame(np.array([0.0, L * i / ns, 0.0])), to_frame(np.array([0.0, L * (i + 1) / ns, 0.0]))
        qa, za = view_point(a3, tilt)
        qb, zb = view_point(b3, tilt)
        r = r0 * (1.0 - 0.3 * i / ns) * scale
        parts.append(('tube', 0.5 * (za + zb) - 0.3, np.stack([qa, qb]), np.array([r, r * 0.98]),
                      dict(R=0.85, G=1.0, B=0.0, gl=0.35, tex='ridged', streak=0.08, seed=seed + i)))
    y = rng.uniform(2.0, 4.0)
    az = rng.uniform(0.0, 2.0 * math.pi)
    while y < L - 1.5:
        s = y / L
        ang = (Sp['ang_base'] + (Sp['ang_tip'] - Sp['ang_base']) * s) * DEG + rng.normal(0.0, Sp['jit'] * DEG * 0.5)
        phi = az
        if flat:
            # Parted to the sides: the azimuth pulled toward the shoot's flanks (0 and 180 deg), the lower side's
            # needles turned up and out.
            phi = math.atan2(0.45 * math.sin(phi), math.cos(phi))
        top = 0.5 + 0.5 * math.sin(phi)
        Ln = rng.uniform(*Sp['L']) * (0.85 + 0.2 * top) * scale
        radial = np.array([math.cos(phi), 0.0, math.sin(phi)])
        d = math.cos(ang) * np.array([0.0, 1.0, 0.0]) + math.sin(ang) * radial
        bow_v = np.array([0.0, 1.0, 0.0]) - d * d[1]
        bow_v = bow_v / max(np.linalg.norm(bow_v), 1e-6)
        base_shoot = np.array([0.0, y, 0.0])
        c3 = needle_curve(base_shoot + radial * r0 * scale, d, Ln, bow_v, rng.uniform(*Sp['bow']), k=7)
        rad = np.hypot(c3[:, 0], c3[:, 2])
        c3 = np.array([to_frame(p) for p in c3])
        parts.append(stroke_part(c3, rad, 0.5 * rng.uniform(*Sp['W']) * scale, tilt,
                                 Sp['gain'] * rng.uniform(0.9, 1.06),
                                 rng.uniform(0.0, 2.0 * math.pi), 0.0, 'spruce', r0,
                                 view_point(to_frame(base_shoot), tilt)[1]))
        y += rng.uniform(*Sp['step'])
        az += 137.5 * DEG
    if buds:
        tipb = np.array([0.0, L - 0.5, 0.0])
        pts3 = np.array([to_frame(tipb + np.array([0.0, 1.0, 0.0]) * 5.0 * scale * t) for t in np.linspace(0, 1, 5)])
        q = np.array([view_point(p, tilt)[0] for p in pts3])
        z = float(np.mean([view_point(p, tilt)[1] for p in pts3]))
        parts.append(('tube', z + 0.2, q, np.array([1.0, 1.5, 1.5, 1.0, 0.1]) * scale,
                      dict(R=0.82, G=1.0, B=0.0, gl=0.45, tex='bud', scale_len=1.2, streak=0.03, seed=seed + 900)))
    return parts


def scene_pine(var):
    """A Scots pine needle tuft: a: a young shoot's end with its bud cluster; b: an older stretch, needles spread
    wider, a few gone."""
    seed = seed_of('pin_syl', var, 'tuft')
    rng = np.random.default_rng(seed)
    Pn = PINE
    parts = pine_brush(rng, Pn['shoot'], Pn['r0'], Pn['tilt'] * DEG, older=(var == 'b'), seed=seed % 100000,
                       buds=(var == 'a'))
    return dict(parts=parts, hull=False, kind='needles')


def scene_spruce(var):
    """A Norway spruce needle tuft: a: a twig with its needles all round it; b: a shade shoot, its needles parted to
    the sides."""
    seed = seed_of('pic_abi', var, 'tuft')
    rng = np.random.default_rng(seed)
    Sp = SPRUCE
    parts = spruce_brush(rng, Sp['shoot'], Sp['r0'], Sp['tilt'] * DEG, flat=(var == 'b'), seed=seed % 100000)
    return dict(parts=parts, hull=False, kind='needles')


def scene_pine_card():
    """A Scots pine branch's tip for the far tiers: last year's leader and the whorl of laterals at its foot, the
    older stretch below, each a brush of needles, seen a little from above."""
    seed = seed_of('pin_syl', 'card')
    rng = np.random.default_rng(seed)
    # (Turned well toward the eye: the tufts read as the brushes they are, not as combs side on.)
    tilt = 40.0 * DEG
    parts = []
    # The older stretch (two-year), then the whorl: the leader on, three laterals out.
    parts += pine_brush(rng, 120.0, 3.2, tilt, older=True, seed=seed % 1000, buds=False)
    parts += pine_brush(rng, 140.0, 2.6, tilt, x0=0.0, y0=118.0, seed=seed % 1000 + 100)
    for j, ang in enumerate((-48.0, 42.0, -8.0 + rng.uniform(-6, 6))):
        if j == 2 and rng.random() < 0.5:
            continue
        parts += pine_brush(rng, rng.uniform(85.0, 110.0), 2.1, tilt, x0=0.0, y0=120.0, axis_turn=ang * DEG,
                            seed=seed % 1000 + 200 + 50 * j, scale=0.92)
    return dict(parts=parts, hull=False, kind='card')


def scene_spruce_card():
    """A Norway spruce branchlet for the far tiers: a flattened spray, the axis with its alternate laterals spreading
    in one plane (the comb's hanging curtains), each in its needles, seen from above."""
    seed = seed_of('pic_abi', 'card')
    rng = np.random.default_rng(seed)
    tilt = 12.0 * DEG
    parts = spruce_brush(rng, 250.0, 1.8, tilt, flat=True, seed=seed % 1000)
    y = 22.0
    side = 1.0
    while y < 215.0:
        Ll = (115.0 - 0.38 * y) * rng.uniform(0.85, 1.1)
        ang = side * rng.uniform(48.0, 62.0)
        parts += spruce_brush(rng, Ll, 1.2, tilt, flat=True, x0=0.0, y0=y, axis_turn=ang * DEG,
                              seed=seed % 1000 + int(y), scale=0.95)
        y += rng.uniform(17.0, 24.0)
        side = -side
    return dict(parts=parts, hull=False, kind='card')


def build_scene(sid, var):
    kind = SPEC[sid]['arch'].get('leaf_kind', 'simple')
    if sid == 'jun_com':
        return scene_needles(var)
    if sid == 'pin_syl':
        return scene_pine_card() if var == 'card' else scene_pine(var)
    if sid == 'pic_abi':
        return scene_spruce_card() if var == 'card' else scene_spruce(var)
    if var == 'card' and kind in ('compound', 'palmate'):
        return scene_compound_card(sid)
    if kind == 'palmate' and var in ('a', 'b'):
        return scene_palmate(sid, var)
    if var == 'card':
        return scene_card(sid)
    if var == 'leaflet':
        return scene_leaflet(sid)
    if kind == 'compound':
        return scene_compound(sid, var)
    sc = scene_leaf(sid, var)
    if sid in TRIFOLIATE:
        sc['form'] = 'trifoliate'
    return sc


# ======================================================================================================
# Layers
# ======================================================================================================

def part_extent(p):
    if p[0] == 'blade':
        _, _, bl, pose = p
        X, Y = pose.to_canvas(bl.opts[:, 0], bl.opts[:, 1])
        return np.asarray(X), np.asarray(Y)
    if p[0] == 'stroke':
        nd = p[2]
        h = float(nd['hw'].max())
        return np.concatenate([nd['pts'][:, 0] - h, nd['pts'][:, 0] + h]), np.concatenate([nd['pts'][:, 1] - h,
                                                                                          nd['pts'][:, 1] + h])
    if p[0] == 'needle':
        nd = p[2]
        h = nd['hwp']
        return (np.array([nd['P0'][0] - h, nd['P0'][0] + h, nd['P1'][0]]),
                np.array([nd['P0'][1] - h, nd['P0'][1] + h, nd['P1'][1]]))
    _, _, pts, rad, _ = p
    r = np.asarray(rad)
    return np.concatenate([pts[:, 0] - r, pts[:, 0] + r]), np.concatenate([pts[:, 1] - r, pts[:, 1] + r])


def fit_box(scene, at_least=None):
    """The square's length and width (mm) and the shift that puts the base at its v: the object fills the square
    (at_least: a box it must not be smaller than - a species' leaves a and b share one, the engine draws b in a's)."""
    xs, ys = [], []
    for p in scene['parts']:
        X, Y = part_extent(p)
        xs.append(X)
        ys.append(Y)
    X = np.concatenate(xs)
    Y = np.concatenate(ys)
    v0, v1 = (0.016, 0.984) if scene['hull'] else (0.006, 0.99)
    Lb = (Y.max() - Y.min()) / (v1 - v0)
    Wb = 2.0 * np.abs(X).max() / 0.94
    if at_least:
        Lb, Wb = max(Lb, at_least[0]), max(Wb, at_least[1])
    dy = -Y.min() + v0 * Lb
    return float(Lb), float(Wb), float(dy)


def shift(p, dy):
    if p[0] == 'blade':
        _, z, bl, pose = p
        pose.oy += dy
        return p
    if p[0] == 'stroke':
        nd = dict(p[2])
        nd['pts'] = nd['pts'] + np.array([0.0, dy])
        return ('stroke', p[1], nd)
    if p[0] == 'needle':
        nd = dict(p[2])
        nd['P0'] = nd['P0'] + np.array([0.0, dy])
        nd['P1'] = nd['P1'] + np.array([0.0, dy])
        return ('needle', p[1], nd)
    _, z, pts, rad, pr = p
    pts = pts.copy()
    pts[:, 1] += dy
    return ('tube', z, pts, rad, pr)


def downsample(a, ss):
    n = a.shape[0] // ss
    return a.reshape(n, ss, n, ss).mean(axis=(1, 3), dtype=np.float64)


def finalize(cv, ss):
    cov = downsample(cv.cov, ss)
    inside = cov > 1e-6
    out = {}
    icov = (1.0 / np.maximum(cov, 1e-6)).astype(F32)
    for k in CHANNELS:
        out[k] = np.where(inside, downsample(cv.ch[k], ss) * icov, 0.0)
    nl = np.sqrt(out['nx'] ** 2 + out['ny'] ** 2 + out['nz'] ** 2)
    nl = np.where(nl > 1e-6, nl, 1.0)
    for k in ('nx', 'ny', 'nz'):
        out[k] = out[k] / nl
    alpha8 = np.clip(np.round(cov * 255.0), 0, 255).astype(np.uint8)
    filled = alpha8 > 0
    vals = np.stack([out[k] for k in ('R', 'G', 'B', 'nx', 'ny', 'gl', 'un')], 0).astype(F32)
    # The inside values pushed a few texels outward (the mips' edges), the rest the neutral defaults.
    vals = np.where(filled[None], vals, 0.0).astype(F32)
    wsum = filled.astype(F32)
    n_ = SIZE
    for _ in range(6):
        vp = np.pad(vals * wsum[None], ((0, 0), (1, 1), (1, 1)))
        wp = np.pad(wsum, 1)
        acc = np.zeros_like(vals)
        cnt = np.zeros_like(wsum)
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                if dx == 1 and dy == 1:
                    continue
                acc += vp[:, dy:dy + n_, dx:dx + n_]
                cnt += wp[dy:dy + n_, dx:dx + n_]
        grow = (wsum == 0) & (cnt > 0)
        vals = np.where(grow[None], acc / np.maximum(cnt, 1e-9)[None], vals)
        wsum = np.where(grow, F32(1.0), wsum)
    R, G, B, nx, ny, gl, un = [vals[i] for i in range(7)]
    have = wsum > 0
    d = np.zeros((SIZE, SIZE, 4), np.uint8)
    d[..., 0] = np.where(have, np.clip(np.round(R * 128.0), 0, 255), 128)
    d[..., 1] = np.where(have, np.clip(np.round(G * 255.0), 0, 255), 0)
    d[..., 2] = np.where(have, np.clip(np.round(B * 255.0), 0, 255), 0)
    d[..., 3] = alpha8
    n = np.zeros((SIZE, SIZE, 4), np.uint8)
    n[..., 0] = np.where(have, np.clip(np.round(128.0 + 127.0 * nx), 0, 255), 128)
    n[..., 1] = np.where(have, np.clip(np.round(128.0 + 127.0 * ny), 0, 255), 128)
    n[..., 2] = np.where(have, np.clip(np.round(gl * 255.0), 0, 255), 128)
    n[..., 3] = np.where(have, np.clip(np.round(un * 128.0), 0, 255), 128)
    return d, n, cov


def compute_hull(alpha8):
    """Base (u .5, v 0), three pairs (v_i, full width w_i <= 1), tip (u .5, v 1): the least-area such polygon holding
    every texel with any coverage, 1.5 texels of slack across and 2 along. Knots on texel rows; for each knot triple
    the middle half width is searched and the outer ones are the least that hold their segments."""
    n = alpha8.shape[0]
    cols = (np.arange(n) + 0.5) / n - 0.5
    hw = np.where(alpha8 > 0, np.abs(cols)[None, :] + 0.5 / n, 0.0).max(axis=1)
    nd = np.where(hw > 0, hw + 1.5 / n, 0.0)
    nr = nd.copy()
    for k in (1, 2):
        nr[k:] = np.maximum(nr[k:], nd[:-k])
        nr[:-k] = np.maximum(nr[:-k], nd[k:])
    # The requirement at the row boundaries v = k / n (the polygon is linear between knots on boundaries).
    need = np.maximum(np.concatenate([[0.0], nr]), np.concatenate([nr, [0.0]]))
    vk = np.arange(n + 1) / n
    H2 = np.linspace(0.0, 0.5, 129)

    def solve(k1, k2, k3):
        v1, v2, v3 = vk[k1], vk[k2], vk[k3]
        if need[0] > 0 or need[n] > 0:
            return None
        s1 = slice(1, k1)
        L1 = max(need[k1], float((need[s1] * v1 / vk[s1]).max()) if k1 > 1 else 0.0)
        s4 = slice(k3 + 1, n)
        L3 = max(need[k3], float((need[s4] * (1.0 - v3) / (1.0 - vk[s4])).max()) if k3 + 1 < n else 0.0)
        if L1 > 0.5 or L3 > 0.5 or need[k2] > 0.5:
            return None
        H = H2[H2 >= need[k2]]
        lam = (vk[k1 + 1:k2] - v1) / (v2 - v1)
        nq = need[k1 + 1:k2]
        h1 = np.full(H.shape, L1)
        if len(lam):
            h1 = np.maximum(h1, ((nq[None, :] - lam[None, :] * H[:, None]) / (1.0 - lam[None, :])).max(axis=1))
        mu = (vk[k2 + 1:k3] - v2) / (v3 - v2)
        nm = need[k2 + 1:k3]
        h3 = np.full(H.shape, L3)
        if len(mu):
            h3 = np.maximum(h3, ((nm[None, :] - (1.0 - mu[None, :]) * H[:, None]) / mu[None, :]).max(axis=1))
        ok = (h1 <= 0.5) & (h3 <= 0.5)
        if not ok.any():
            return None
        area = h1 * v2 + H * (v3 - v1) + h3 * (1.0 - v2)
        area = np.where(ok, area, np.inf)
        i = int(np.argmin(area))
        return float(area[i]), (v1, h1[i]), (v2, H[i]), (v3, h3[i])

    best = None
    step = n // 32
    grid = range(step, n, step)
    for k1 in grid:
        for k2 in range(k1 + step, n, step):
            for k3 in range(k2 + step, n, step):
                r = solve(k1, k2, k3)
                if r and (best is None or r[0] < best[0]):
                    best = r + ((k1, k2, k3),)
    if best is None:
        return [[0.02, 1.0], [0.5, 1.0], [0.98, 1.0]]
    # Refined about the coarse best.
    c1, c2, c3 = best[4]
    f = step // 4
    for k1 in range(max(c1 - step, 1), c1 + step + 1, f):
        for k2 in range(max(c2 - step, k1 + 1), c2 + step + 1, f):
            for k3 in range(max(c3 - step, k2 + 1), min(c3 + step + 1, n), f):
                r = solve(k1, k2, k3)
                if r and r[0] < best[0]:
                    best = r + ((k1, k2, k3),)
    return [[round(float(v), 5), round(float(min(2.0 * h + 2e-4, 1.0)), 5)] for v, h in best[1:4]]


def hull_slack(alpha8, hull):
    """The least slack (texels) between the hull and the covered texels' corners (negative: a texel sticks out)."""
    n = alpha8.shape[0]
    kv = np.array([0.0] + [h[0] for h in hull] + [1.0])
    kh = np.array([0.0] + [0.5 * h[1] for h in hull] + [0.0])
    rows, cols = np.nonzero(alpha8 > 0)
    need = np.abs((cols + 0.5) / n - 0.5) + 0.5 / n
    v_lo, v_hi = rows / n, (rows + 1.0) / n
    have = np.minimum(np.interp(v_lo, kv, kh), np.interp(v_hi, kv, kh))
    for k in range(1, 4):
        inside = (kv[k] > v_lo) & (kv[k] < v_hi)
        have = np.where(inside, np.minimum(have, kh[k]), have)
    return float(((have - need) * n).min())


def write_png(path, arr):
    tmp = path + '.tmp.png'
    Image.fromarray(arr, 'RGBA').save(tmp)
    os.replace(tmp, path)


def load_prev_boxes():
    try:
        with open(MANIFEST, encoding='utf-8') as f:
            return {l['name']: l['box_mm'] for l in json.load(f)['layers']}
    except (OSError, ValueError, KeyError):
        return {}


PREV_BOX = load_prev_boxes()
FRESH_TREES = '--fresh-trees' in sys.argv


def layer_metrics(d, n):
    """R's p5 / p95 over the lamina (covered, off the veins), the normal map's slope p95 inside (three texels in from
    the outline, off the midrib and stalks) and at the outline (its outer two texels)."""
    a = d[..., 3]
    G = d[..., 1]

    def erode(m, k):
        for _ in range(k):
            m = m & np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1)
        return m

    full = a == 255
    lam = full & (G < 13)
    Rv = d[..., 0] / 128.0
    nx = (n[..., 0].astype(float) - 128.0) / 127.0
    ny = (n[..., 1].astype(float) - 128.0) / 127.0
    nzz = np.sqrt(np.clip(1.0 - nx * nx - ny * ny, 1e-4, 1.0))
    sl = np.hypot(nx, ny) / nzz
    inside = erode(full, 3) & (G < 250)
    edge = (a > 0) & ~erode(a > 0, 2) & (G < 250)
    out = {}
    if lam.sum() > 100:
        out['r_p5'], out['r_p95'] = [float(x) for x in np.percentile(Rv[lam], [5, 95])]
    if inside.sum() > 100:
        out['slope_in'] = float(np.percentile(sl[inside], 95))
    if edge.sum() > 30:
        out['slope_edge'] = float(np.percentile(sl[edge], 95))
    return out


def run_layer(job):
    sid, var, ss = job
    t0 = time.time()
    scene = build_scene(sid, var)
    shared = None
    names = [var]
    if var in ('a', 'b') and (sid, 'b') in layer_jobs():
        # Leaves a and b in one box (each at its own physical size within it).
        other = fit_box(build_scene(sid, 'b' if var == 'a' else 'a'))
        shared = other[:2]
        names = ['a', 'b']
    # The manifest's box stays (the engine sizes the elements by it): a layer may only grow it.
    for nm in (names if not (FRESH_TREES and sid in TB.TREES) else []):
        old = PREV_BOX.get('%s_%s' % (sid, nm))
        if old:
            shared = (max(old[0], shared[0]), max(old[1], shared[1])) if shared else tuple(old)
    Lb, Wb, dy = fit_box(scene, shared)
    parts = [shift(p, dy) for p in scene['parts']]
    cv = Canvas(SIZE * ss, Lb, Wb)
    for p in sorted(parts, key=lambda q: q[1]):
        if p[0] == 'blade':
            render_blade(cv, p[2], p[3])
        elif p[0] == 'needle':
            render_needle(cv, p[2])
        elif p[0] == 'stroke':
            render_stroke(cv, p[2])
        else:
            render_tube(cv, p[2], p[3], p[4])
    d, n, cov = finalize(cv, ss)
    name = '%s_%s' % (sid, var)
    write_png(os.path.join(OUT, name + '_d.png'), d)
    write_png(os.path.join(OUT, name + '_n.png'), n)
    hull = compute_hull(d[..., 3]) if scene['hull'] else None
    slack = hull_slack(d[..., 3], hull) if hull else None
    cols = np.nonzero((d[..., 3] > 0).any(axis=0))[0]
    rows = np.nonzero((d[..., 3] > 0).any(axis=1))[0]
    ext = [float(cols[0]) / SIZE, float(cols[-1] + 1) / SIZE, float(rows[0]) / SIZE, float(rows[-1] + 1) / SIZE] \
        if len(cols) else [0, 0, 0, 0]
    lam = (d[..., 3] > 200) & (d[..., 1] < 10)
    met = layer_metrics(d, n)
    review = dict(lit=shade(d, n), und=shade(d, n, under=True), al=np.ascontiguousarray(d[..., 3][::-1]))
    return dict(name=name, species=sid, var=var, kind=scene['kind'], hull=hull, met=met, review=review,
                box_mm=[round(float(Lb), 2), round(float(Wb), 2)],
                form=scene.get('form'), ext=ext, secs=time.time() - t0, slack=slack,
                r_mean=float(d[..., 0][lam].mean() / 128.0) if lam.any() else 0.0)


def layer_jobs():
    jobs = []
    for sid in MS.SP:
        kind = MS.SP[sid]['arch'].get('leaf_kind', 'simple')
        if sid == 'jun_com':
            jobs += [(sid, 'a'), (sid, 'b')]
        elif kind == 'compound':
            jobs += [(sid, 'a'), (sid, 'b'), (sid, 'leaflet')]
        elif kind == 'spray':
            jobs.append((sid, 'a'))
            if sid in B_SPRAY:
                jobs.append((sid, 'b'))
            if sid in TRIFOLIATE:
                jobs.append((sid, 'leaflet'))
            jobs.append((sid, 'card'))
        else:
            jobs += [(sid, 'a'), (sid, 'b')]
    for sid in TB.ORDER:
        if sid not in TB.TREES:
            continue
        kind = TB.TREES[sid]['arch']['leaf_kind']
        jobs += [(sid, 'a'), (sid, 'b')]
        if kind in ('compound', 'palmate'):
            jobs.append((sid, 'leaflet'))
        jobs.append((sid, 'card'))
    return jobs


# ======================================================================================================
# Review
# ======================================================================================================
LIGHT = np.array([-0.5, 0.55, 0.67])
LIGHT = LIGHT / np.linalg.norm(LIGHT)
HALF = (LIGHT + np.array([0.0, 0.0, 1.0]))
HALF = HALF / np.linalg.norm(HALF)
BG = 0.2159


def srgb(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def shade(d, n, under=False):
    """The layer lit as the engine lights it: summer green x R, the sun from the upper left (tip up), a little sky,
    over mid grey. (under: the underside's luminance on a paler green, flat.)"""
    d = d.astype(float)
    n = n.astype(float)
    a = d[..., 3] / 255.0
    nx = (n[..., 0] - 128.0) / 127.0
    ny = (n[..., 1] - 128.0) / 127.0
    nzz = np.sqrt(np.clip(1.0 - nx * nx - ny * ny, 0.0, 1.0))
    if under:
        alb = np.array([0.14, 0.22, 0.09])[None, None, :] * (n[..., 3] / 128.0)[..., None]
        lit = alb * (2.2 * 0.75 + 0.6)
    else:
        gloss = n[..., 2] / 255.0
        alb = np.array([0.10, 0.20, 0.05])[None, None, :] * (d[..., 0] / 128.0)[..., None]
        ndl = np.clip(nx * LIGHT[0] + ny * LIGHT[1] + nzz * LIGHT[2], 0.0, 1.0)
        ndh = np.clip(nx * HALF[0] + ny * HALF[1] + nzz * HALF[2], 0.0, 1.0)
        spec = 0.05 * gloss * np.power(ndh, 8.0 + 120.0 * gloss ** 2) * (2.0 + 6.0 * gloss ** 2)
        sky = 0.45 * (0.55 + 0.45 * nzz)
        lit = alb * (2.6 * ndl[..., None] * np.array([1.0, 0.97, 0.9]) + sky[..., None] * np.array([0.6, 0.7, 0.9]))
        lit += (2.6 * spec)[..., None] * np.array([1.0, 0.97, 0.9])
    out = lit * a[..., None] + BG * (1.0 - a[..., None])
    return (srgb(out)[::-1] * 255.0 + 0.5).astype(np.uint8)


def sheets(results, prefix=''):
    rows = []
    for r in results:
        lit = Image.fromarray(r['review']['lit'])
        und = Image.fromarray(r['review']['und'])
        al = Image.fromarray(r['review']['al']).convert('RGB')
        L, W = r['box_mm']
        rows.append((r, lit, und, al, L, W))
    # The overview: lit and coverage, six layers to a row.
    T = 168
    per = 6
    nrow = (len(rows) + per - 1) // per
    sheet = Image.new('RGB', (per * (2 * T + 8), nrow * (T + 16)), (40, 40, 40))
    for i, (r, lit, und, al, L, W) in enumerate(rows):
        x = (i % per) * (2 * T + 8)
        y = (i // per) * (T + 16)
        sheet.paste(lit.resize((T, T), Image.LANCZOS), (x, y + 14))
        sheet.paste(al.resize((T, T), Image.LANCZOS), (x + T, y + 14))
        label_img = ImageDraw.Draw(sheet)
        label_img.text((x + 2, y + 1), '%s %s' % (r['name'], r['kind']), fill=(230, 230, 230))
    tmp = os.path.join(BUILD, prefix + 'leaf_sheet.tmp.png')
    sheet.save(tmp)
    os.replace(tmp, os.path.join(BUILD, prefix + 'leaf_sheet.png'))
    # Review pages: lit, at the object's own proportions, underside, coverage.
    T = 256
    per_page = 8
    for pg in range((len(rows) + per_page - 1) // per_page):
        chunk = rows[pg * per_page:(pg + 1) * per_page]
        page = Image.new('RGB', (2 * (4 * T + 12), ((len(chunk) + 1) // 2) * (T + 18)), (40, 40, 40))
        for i, (r, lit, und, al, L, W) in enumerate(chunk):
            x = (i % 2) * (4 * T + 12)
            y = (i // 2) * (T + 18)
            page.paste(lit.resize((T, T), Image.LANCZOS), (x, y + 16))
            k = min(T / L, T / W)
            ph = lit.resize((max(int(W * k), 1), max(int(L * k), 1)), Image.LANCZOS)
            page.paste(ph, (x + T + (T - ph.size[0]) // 2, y + 16 + (T - ph.size[1]) // 2))
            page.paste(und.resize((T, T), Image.LANCZOS), (x + 2 * T, y + 16))
            page.paste(al.resize((T, T), Image.LANCZOS), (x + 3 * T, y + 16))
            ImageDraw.Draw(page).text((x + 2, y + 2), '%s  %s  %.0fx%.0f mm  R %.2f  %s' % (
                r['name'], r['kind'], L, W, r['r_mean'], r['hull'] or ''), fill=(230, 230, 230))
        tmp = os.path.join(BUILD, '%sleaf_review_%02d.tmp.png' % (prefix, pg + 1))
        page.save(tmp)
        os.replace(tmp, os.path.join(BUILD, '%sleaf_review_%02d.png' % (prefix, pg + 1)))


def crops(names):
    for nm in names:
        d = np.asarray(Image.open(os.path.join(OUT, nm + '_d.png')))
        n = np.asarray(Image.open(os.path.join(OUT, nm + '_n.png')))
        img = Image.fromarray(np.concatenate([shade(d, n), shade(d, n, under=True)], 1))
        tmp = os.path.join(BUILD, 'leaf_crop_%s.tmp.png' % nm)
        img.save(tmp)
        os.replace(tmp, os.path.join(BUILD, 'leaf_crop_%s.png' % nm))


def write_manifest(results):
    layers = []
    species = {}
    for i, r in enumerate(results):
        e = dict(name=r['name'], species=r['species'], kind=r['kind'], hull=r['hull'], box_mm=r['box_mm'])
        if r.get('form'):
            e['form'] = r['form']
        layers.append(e)
        sp = species.setdefault(r['species'], dict(a=-1, b=-1, card=-1, leaflet=-1))
        sp[r['var']] = i
    for sid, sp in species.items():
        if sp['b'] < 0:
            sp['b'] = sp['a']
        if SPEC.get(sid, {}).get('arch', {}).get('leaf_kind') == 'needles' and sid in TB.TREES:
            sp['needles'] = sp['a']
    for sid, src in TB.SHARED.items():
        if src in species:
            species[sid] = dict(species[src])
    lines = ['{', ' "size": %d,' % SIZE, ' "layers": [']
    for i, e in enumerate(layers):
        lines.append('  ' + json.dumps(e) + (',' if i + 1 < len(layers) else ''))
    lines.append(' ],')
    lines.append(' "species": {')
    keys = list(species.keys())
    for i, k in enumerate(keys):
        lines.append('  "%s": %s%s' % (k, json.dumps(species[k]), ',' if i + 1 < len(keys) else ''))
    lines.append(' }')
    lines.append('}')
    tmp = MANIFEST + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(tmp, MANIFEST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--ss', type=int, default=3)
    ap.add_argument('--workers', type=int, default=int(os.environ.get('DA_THREADS') or os.cpu_count() or 4))
    ap.add_argument('--crops', default='cor_ave_a,fra_aln_a,sal_cin_a')
    ap.add_argument('--fresh-trees', action='store_true')
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    jobs = layer_jobs()
    only = [x for x in args.only.split(',') if x]
    if only:
        jobs = [j for j in jobs if j[0] in only or '%s_%s' % j in only]
    t0 = time.time()
    order = {j: i for i, j in enumerate(jobs)}
    # The slow layers first.
    heavy = sorted(jobs, key=lambda j: (j[1] not in ('card', 'b', 'a'),
                                       SPEC[j[0]]['arch'].get('leaf_kind') not in ('compound', 'palmate', 'needles')))
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        res = list(ex.map(run_layer, [(s, v, args.ss) for s, v in heavy]))
    res.sort(key=lambda r: order[(r['species'], r['var'])])
    t1 = time.time()
    for r in res:
        warn = ''
        e = r['ext']
        if e[0] < 0.015 or e[1] > 0.985 or e[3] > 0.997:
            warn = '  !extent'
        if r['slack'] is not None:
            warn += '  hull slack %.1f' % r['slack'] + (' !' if r['slack'] < 1.0 else '')
        m = r['met']
        if 'r_p5' in m:
            warn += '  R p5 %.2f p95 %.2f' % (m['r_p5'], m['r_p95'])
        if 'slope_in' in m:
            warn += '  slope in %.2f edge %.2f' % (m['slope_in'], m.get('slope_edge', 0.0))
        print('%-16s %-8s %5.1fs  box %6.1f x %5.1f mm  u %.3f-%.3f v %.3f-%.3f  R %.2f%s' % (
            r['name'], r['kind'], r['secs'], r['box_mm'][0], r['box_mm'][1], e[0], e[1], e[2], e[3], r['r_mean'],
            warn))
    print('%d layers in %.1f s (render), ss %d' % (len(res), t1 - t0, args.ss))
    if not only:
        write_manifest(res)
    sheets(res, prefix='' if not only else 'only_')
    crops([c for c in args.crops.split(',') if any(r['name'] == c for r in res)])
    print('total %.1f s' % (time.time() - t0))


if __name__ == '__main__':
    main()
