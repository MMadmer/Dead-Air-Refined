# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Review sheets of the trees' texture layers against photo packs (docs/dead-air/TREES_TEXTURES.md).

Per tree species its leaf layers (lit as make_leaf_textures.py lights them, at their real proportions) beside
photos of its leaves; per bark type its tile and a 30 cm trunk lit near, near in a low grazing light and at three
metres beside photos of its bark, with the measures of the tile (in the engine's ambient light), of the pack's bark
photos and of the trees' research's curated close-ups (--research): the bark's median colour, the fissure contrast
(linear luminance p20 / p80), the dark share (under half the median) and the anisotropy (horizontal over vertical
gradients: vertical fissures > 1); and the scale measures, the ridges' or plates' spacing across the stem and the
fissures' width in millimetres, of the tile (lit flat by the review's light, and in the ambient light) against the
pack's photos whose trunk is annotated (pack/measure.json: per photo the trunk's edges as fractions of the width at
a height or two, the rows to take, its diameter in cm, the bark's zone, optionally the arc of the round to take),
each photo's trunk unwrapped to a strip at 1 px/mm.

The packs are folders of photos (pack/NN.jpg, pack/index.json with each file's title and url, pack/exclude.json with
the files judged off-topic, pack/first.json the close-ups to show first, pack/measure.json the trunks annotated for
the scale measures): Wikimedia Commons categories of each species' leaves and bark, fetched by the review's own
script; they stay outside the repository (out/review_trees/sources.json lists them for the attribution).

usage: review_trees.py --photos <dir of packs> [--research tree_colours.json] [--only name,name] [--part bark,leaves]
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
OUT = os.path.join(HERE, 'out', 'review_trees')

# The bark types' packs (the species wearing them).
BARK_PACKS = {
    'pine_plates': ['pin_syl_bark'], 'pine_mirror': ['pin_syl_bark'], 'spruce': ['pic_abi_bark'],
    'birch_white': ['bet_pen_bark', 'bet_pub_bark'], 'birch_base': ['bet_pen_bark'],
    'birch_downy_base': ['bet_pub_bark'],
    'aspen_smooth': ['pop_tre_bark'], 'aspen_base': ['pop_tre_bark'], 'poplar_white': ['pop_alb_bark'],
    'poplar_black': ['pop_nig_bark', 'pop_can_bark'], 'oak': ['que_rob_bark'], 'oak_young': ['que_rob_bark'],
    'red_oak': ['que_rub_bark'], 'hornbeam': ['car_bet_bark'], 'maple': ['ace_pla_bark'], 'sycamore': ['ace_pse_bark'],
    'box_elder': ['ace_neg_bark'], 'lime': ['til_cor_bark'], 'ash': ['fra_exc_bark', 'fra_pen_bark'],
    'ash_young': ['fra_exc_bark'], 'elm': ['ulm_lae_bark', 'ulm_min_bark'], 'rowan': ['sor_auc_bark'],
    'alder': ['aln_glu_bark'], 'alder_young': ['aln_glu_bark'], 'willow_tree': ['sal_alb_bark', 'sal_fra_bark'],
    'robinia': ['rob_pse_bark'], 'horse_chestnut': ['aes_hip_bark'], 'apple': ['mal_dom_bark'],
    'pear': ['pyr_com_bark'],
    'cherry_trunk': ['pru_cer_bark'], 'plum': ['pru_dom_bark'], 'walnut': ['jug_reg_bark'],
    'walnut_young': ['jug_reg_bark'], 'deadwood_bark': ['dead_wood'], 'deadwood_bare': ['dead_wood'], 'char': ['char'],
}


def srgb2lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin2srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, 12.92 * c, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def hexc(lin):
    return '#%02x%02x%02x' % tuple(int(round(float(x) * 255)) for x in lin2srgb(np.asarray(lin)))


def pack_files(photos, pack):
    d = os.path.join(photos, pack)
    if not os.path.isdir(d):
        return []
    ex = set()
    if os.path.exists(os.path.join(d, 'exclude.json')):
        ex = set(json.load(open(os.path.join(d, 'exclude.json'), encoding='utf-8')))
    return [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.jpg') and f not in ex]


def bark_mask(a):
    """The bark pixels of a photo's middle: not sky (blue or bright and flat), not foliage green, not blown out."""
    mx = a.max(-1)
    mn = a.min(-1)
    s = (mx - mn) / np.maximum(mx, 1e-6)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    blue = (b > r * 1.08) & (b > g) & (mx > 0.35) & (s > 0.12)
    bright_flat = (mx > 0.9) & (s < 0.08)
    green = (g > r * 1.08) & (g > b * 1.1) & (s > 0.18)
    return ~(blue | bright_flat | green) & (mx > 0.03)


def metrics(lin, m):
    """Median colour, fissure contrast p20/p80, dark share and anisotropy of the masked linear pixels."""
    y = lin[..., 0] * 0.2126 + lin[..., 1] * 0.7152 + lin[..., 2] * 0.0722
    ym = y[m]
    if ym.size < 500:
        return None
    p20, p50, p80 = np.percentile(ym, [20, 50, 80])
    gx = np.abs(np.diff(y, axis=1))[:-1, :]
    gy = np.abs(np.diff(y, axis=0))[:, :-1]
    mm = m[:-1, :-1]
    return dict(rgb=np.median(lin[m], axis=0), contrast=float(p20 / max(p80, 1e-6)),
                dark=float((ym < 0.5 * p50).mean()), aniso=float(gx[mm].sum() / max(gy[mm].sum(), 1e-9)))


def photo_metrics(path):
    """A photo's bark measures (None unless its middle is mostly bark: a close-up) and its bark share there."""
    im = Image.open(path).convert('RGB')
    im.thumbnail((640, 640))
    a = np.asarray(im, np.float64) / 255.0
    h, w = a.shape[:2]
    a = a[int(0.2 * h):int(0.8 * h), int(0.2 * w):int(0.8 * w)]
    m = bark_mask(a)
    share = float(m.mean())
    if share < 0.6:
        return None, share
    return metrics(srgb2lin(a), m), share


def agg(ms):
    ms = [m for m in ms if m]
    if not ms:
        return None
    return dict(rgb=np.median([m['rgb'] for m in ms], axis=0), contrast=float(np.median([m['contrast'] for m in ms])),
                dark=float(np.median([m['dark'] for m in ms])), aniso=float(np.median([m['aniso'] for m in ms])),
                n=len(ms))


def fmt(m):
    if not m:
        return 'no close-up bark'
    return '%s  p20/p80 %.2f  dark %.2f  aniso %.2f%s' % (hexc(m['rgb']), m['contrast'], m['dark'], m['aniso'],
                                                        ('  (%d photos)' % m['n']) if 'n' in m else '')


def thumbs(files, T, n):
    out = []
    for p in files[:n]:
        im = Image.open(p).convert('RGB')
        im.thumbnail((T, T))
        out.append(im)
    return out


def research_measures(t, research):
    """The trees' research's curated bark close-ups of the type's wearers (tree_colours.json: median, fissure contrast
    p20 / p80, dark share, anisotropy), as a line."""
    if not research:
        return None
    out = []
    for sid in t.get('species', []) + t.get('upper', []):
        a = ((research.get(sid.replace('t_', '')) or {}).get('bark') or {}).get('all') or {}
        if a:
            out.append('%s %s p20/p80 %.2f dark %.2f aniso %.2f (%d)' % (sid, a['hex'], a.get('contrast', 0.0),
                                                                        a.get('dark_frac', 0.0), a.get('aniso', 0.0),
                                                                        a.get('n', 0)))
    return '; '.join(out[:2]) or None


# ======================================================================================================
# Scale measures: the ridges' or plates' spacing across the stem and the fissures' width, in millimetres, on the
# tiles and on the pack photos whose trunk is annotated (pack/measure.json), both at RES px/mm.
# ======================================================================================================

# The types' pack photos at scale: (pack, zone of the trunk in measure.json).
BARK_ZONES = {
    'pine_plates': [('pin_syl_bark', 'base')], 'spruce': [('pic_abi_bark', 'base')],
    'birch_base': [('bet_pen_bark', 'base')], 'birch_downy_base': [('bet_pub_bark', 'base')],
    'birch_white': [('bet_pen_bark', 'upper'), ('bet_pub_bark', 'upper')],
    'aspen_smooth': [('pop_tre_bark', 'upper')], 'aspen_base': [('pop_tre_bark', 'base'), ('pop_alb_bark', 'base')],
    'poplar_black': [('pop_nig_bark', 'base'), ('pop_can_bark', 'base')], 'oak': [('que_rob_bark', 'base')],
    'red_oak': [('que_rub_bark', 'base')], 'maple': [('ace_pla_bark', 'base')], 'sycamore': [('ace_pse_bark', 'base')],
    'box_elder': [('ace_neg_bark', 'base')], 'lime': [('til_cor_bark', 'base')],
    'ash': [('fra_exc_bark', 'base'), ('fra_pen_bark', 'base')], 'ash_young': [('fra_exc_bark', 'upper')],
    'alder': [('aln_glu_bark', 'base')], 'alder_young': [('aln_glu_bark', 'upper')],
    'willow_tree': [('sal_alb_bark', 'base'), ('sal_fra_bark', 'base')], 'robinia': [('rob_pse_bark', 'base')],
    'horse_chestnut': [('aes_hip_bark', 'base')], 'apple': [('mal_dom_bark', 'base')],
    'cherry_trunk': [('pru_cer_bark', 'base')], 'plum': [('pru_dom_bark', 'base')], 'walnut': [('jug_reg_bark', 'base')],
    'walnut_young': [('jug_reg_bark', 'upper')], 'deadwood_bark': [('dead_wood', 'bark')],
    'deadwood_bare': [('dead_wood', 'bare')], 'char': [('char', 'base')],
}
RES = 1.0
# (A photo under 0.6 px/mm blurs fissures of a few millimetres: not measured.)
MIN_PPM = 0.6


def lumi(lin):
    return lin[..., 0] * 0.2126 + lin[..., 1] * 0.7152 + lin[..., 2] * 0.0722


def gblur(a, s):
    """Gaussian blur, sigma s px, the edges mirrored."""
    if s <= 0:
        return a
    k = int(np.ceil(3 * s))
    w = np.exp(-0.5 * (np.arange(-k, k + 1) / s) ** 2)
    w /= w.sum()
    a = np.apply_along_axis(lambda c: np.convolve(c, w, 'valid'), 0, np.pad(a, ((k, k), (0, 0)), mode='reflect'))
    return np.apply_along_axis(lambda c: np.convolve(c, w, 'valid'), 1, np.pad(a, ((0, 0), (k, k)), mode='reflect'))


def acf_rows(z, maxlag):
    """The rows' mean autocorrelation (each row zero-mean), unbiased, lags 0..maxlag."""
    n = z.shape[1]
    z = z - z.mean(axis=1, keepdims=True)
    F = np.fft.rfft(z, 2 * n, axis=1)
    ac = np.fft.irfft(F * np.conj(F), axis=1)[:, :maxlag + 1].sum(axis=0) / (n - np.arange(maxlag + 1))
    return ac / ac[0]


def spacing_mm(Y, maxlag_mm=None):
    """The ridges' or plates' spacing across the stem (mm): twice the lag of the first minimum of the rows'
    autocorrelation of the log luminance (its trend over 60 mm removed, smoothed over 2 mm); lags up to half the
    strip or maxlag_mm (a tile's half: a texture repeats beyond it)."""
    L = np.log(np.maximum(Y, 1e-4))
    L = gblur(L - gblur(L, 60.0 * RES), 2.0 * RES)
    maxlag = Y.shape[1] // 2 if not maxlag_mm else min(Y.shape[1] // 2, int(maxlag_mm * RES))
    ac = acf_rows(L, maxlag)
    i = 1
    while i < maxlag - 1 and not (ac[i] <= ac[i - 1] and ac[i] <= ac[i + 1]):
        i += 1
    return 2.0 * i / RES


def fissure_mm(Y):
    """The fissures' mean width (mm): the dark runs across the stem (luminance under half its mean over 30 mm), runs
    of 2 mm and more that do not touch the strip's ends."""
    Yn = gblur(Y / np.maximum(gblur(Y, 30.0 * RES), 1e-6), 0.35)
    runs = []
    for row in Yn < 0.5:
        d = np.diff(np.concatenate([[0], row.astype(np.int8), [0]]))
        s, e = np.where(d == 1)[0], np.where(d == -1)[0]
        ok = (s > 0) & (e < len(row))
        runs.extend((e - s)[ok].tolist())
    runs = np.array(runs, np.float64) / RES
    runs = runs[runs >= 2.0]
    return float(runs.mean()) if len(runs) else 0.0


def edges_at(ann, y):
    """The trunk's edges (fractions of the width) at height y, linear through the annotated heights."""
    E = {float(k): v for k, v in ann['edges'].items()}
    ks = sorted(E)
    if len(ks) == 1:
        return E[ks[0]]
    t = (y - ks[0]) / (ks[-1] - ks[0])
    return [E[ks[0]][i] * (1 - t) + E[ks[-1]][i] * t for i in (0, 1)]


def trunk_strip(path, ann):
    """A photo's trunk unwrapped over its arc (degrees from the middle, +-50 by default) at RES px/mm: the linear
    luminance (rows along the stem) and the photo's px/mm (the trunk's width over its diameter, ann['dbh'] cm)."""
    im = Image.open(path).convert('RGB')
    Y = lumi(srgb2lin(np.asarray(im, np.float64) / 255.0))
    H, W = Y.shape
    r0, r1 = ann.get('rows', [0.2, 0.8])
    ys = np.arange(int(r0 * H), int(r1 * H))
    E = np.array([edges_at(ann, (y + 0.5) / H) for y in ys]) * W
    ppm = float(np.median(E[:, 1] - E[:, 0]) / (ann['dbh'] * 10.0))
    step = ppm / RES
    if step > 1.0:
        Y = gblur(Y, 0.5 * step)
    yo = np.arange(int(r0 * H), int(r1 * H), step)
    Eo = np.array([edges_at(ann, (y + 0.5) / H) for y in yo]) * W
    Ro, Xc = 0.5 * (Eo[:, 1] - Eo[:, 0]), 0.5 * (Eo[:, 1] + Eo[:, 0])
    a0, a1 = np.radians(ann.get('arc', [-50.0, 50.0]))
    Rm = float(np.median(Ro))
    s = np.arange(a0 * Rm, a1 * Rm, step)
    X = Xc[:, None] + Ro[:, None] * np.sin(np.clip(s[None, :] / Ro[:, None], a0, a1))
    Yy = np.broadcast_to(yo[:, None], X.shape)
    x0 = np.clip(np.floor(X - 0.5).astype(int), 0, W - 2)
    y0 = np.clip(np.floor(Yy - 0.5).astype(int), 0, H - 2)
    fx, fy = np.clip(X - 0.5 - x0, 0, 1), np.clip(Yy - 0.5 - y0, 0, 1)
    out = ((Y[y0, x0] * (1 - fx) + Y[y0, x0 + 1] * fx) * (1 - fy)
           + (Y[y0 + 1, x0] * (1 - fx) + Y[y0 + 1, x0 + 1] * fx) * fy)
    return out, ppm


def pack_scale(photos, name, scale):
    """The pack's scale measures for a type: per annotated photo of its zone, spacing and fissure width; the
    medians and the strips' median width."""
    rows = []
    for pack, zone in BARK_ZONES.get(name, []):
        mj = os.path.join(photos, pack, 'measure.json')
        if not os.path.exists(mj):
            continue
        ex = set()
        if os.path.exists(os.path.join(photos, pack, 'exclude.json')):
            ex = set(json.load(open(os.path.join(photos, pack, 'exclude.json'), encoding='utf-8')))
        for f, a in sorted(json.load(open(mj, encoding='utf-8')).items()):
            if a.get('zone') != zone or not a.get('edges') or f in ex:
                continue
            Y, ppm = trunk_strip(os.path.join(photos, pack, f), a)
            if ppm < MIN_PPM:
                continue
            rows.append(dict(file='%s/%s' % (pack, f), ppm=round(ppm, 2), width=Y.shape[1] / RES,
                             spacing=spacing_mm(Y, 30.0 * scale), fissure=fissure_mm(Y)))
    if not rows:
        return None
    med = {k: float(np.median([r[k] for r in rows])) for k in ('spacing', 'fissure', 'width')}
    med.update(n=len(rows), photos=rows)
    return med


def tile_scale(Y, scale, width):
    """A tile's scale measures (its luminance Y), tiled to strips as wide as the pack's and measured as they are."""
    from PIL import Image as _I
    ppm = 512.0 / (60.0 * scale)
    k = max(2, int(np.ceil(width / (60.0 * scale))) + 2)
    Y2 = np.tile(Y, (k, k))
    w = int(round(Y2.shape[1] * RES / ppm))
    Y2 = np.asarray(_I.fromarray(Y2.astype(np.float32), 'F').resize((w, w), _I.BOX), np.float64)
    ww = int(round(width * RES))
    ms = [(spacing_mm(Y2[:, i:i + ww], 30.0 * scale), fissure_mm(Y2[:, i:i + ww]))
          for i in range(0, Y2.shape[1] - ww + 1, max(1, ww // 3))]
    return float(np.median([m[0] for m in ms])), float(np.median([m[1] for m in ms]))


# A low grazing light from the side (the review's second near view): the relief in relief.
GRAZE = np.array([-0.97, 0.18, 0.16])
GRAZE = GRAZE / np.linalg.norm(GRAZE)


def pct(a, b):
    return '%+.0f%%' % (100.0 * (a / b - 1.0)) if b else '-'


def bark_sheets(photos, only, research=None):
    import make_bark_textures as MB
    man = json.load(open(os.path.join(HERE, 'bark_manifest.json'), encoding='utf-8'))
    tree = [t for t in man['types'] if t.get('tree') and (not only or t['name'] in only)]
    rows, table = [], {}
    for t in tree:
        name, scale = t['name'], t.get('scale', 1.0)
        d = np.asarray(Image.open(os.path.join(HERE, 'build', 'bark', name + '_d.png')))
        n = np.asarray(Image.open(os.path.join(HERE, 'build', 'bark', name + '_n.png')))
        m = MB.decode(d, n)
        MB.set_scale(scale)
        lit = MB.flat_lit(m)
        # (The tile's measures in the engine's ambient light: the albedo by the cavity's two factors, as
        # r3/da_shrub_wood.ps and the combine take it.)
        amb = m['alb'] * ((1.0 - 0.35 * (1.0 - m['cav'])) * (1.0 - 0.7 * (1.0 - m['cav'])))[..., None]
        tm = metrics(amb, np.ones(amb.shape[:2], bool))
        near = MB.cylinder(m, 300.0, 520.0, 0.7, ssf=2, tile_mm=60.0 * scale)
        sun = MB.SUN_DIR
        MB.SUN_DIR = GRAZE
        graze = MB.cylinder(m, 300.0, 520.0, 0.7, ssf=2, tile_mm=60.0 * scale)
        MB.SUN_DIR = sun
        far = MB.cylinder(m, 300.0, 1500.0, 0.24, ssf=3, tile_mm=60.0 * scale)
        # The scale measures: the tile lit flat (as the photos are lit) and in the ambient light, at the pack's strips'
        # width.
        ps = pack_scale(photos, name, scale)
        width = ps['width'] if ps else 300.0
        tl = tile_scale(lumi(srgb2lin(MB.flat_lit(m, flipv=False) / 255.0)), scale, width)
        ta = tile_scale(lumi(amb), scale, width)
        files = []
        for p in BARK_PACKS.get(name, []):
            files += pack_files(photos, p)
        pms = [(f,) + photo_metrics(f) for f in files]
        pm = agg([x[1] for x in pms])
        # (The close-ups first on the sheet: a pack's own choice (first.json), then by their middle's bark share.)
        first = []
        for p in BARK_PACKS.get(name, []):
            fj = os.path.join(photos, p, 'first.json')
            if os.path.exists(fj):
                first += [os.path.join(photos, p, f) for f in json.load(open(fj, encoding='utf-8'))]
        files = [f for f in first if f in files] + [x[0] for x in sorted(pms, key=lambda x: -x[2]) if x[0] not in first]
        rs = research_measures(t, research)
        table[name] = dict(texture=fmt(tm), photos=fmt(pm), research=rs, packs=BARK_PACKS.get(name, []),
                           photo_count=len(files),
                           spacing_mm=dict(texture_lit=round(tl[0], 1), texture_ambient=round(ta[0], 1),
                                           pack=round(ps['spacing'], 1) if ps else None,
                                           lit_vs_pack=pct(tl[0], ps['spacing']) if ps else None),
                           fissure_mm=dict(texture_lit=round(tl[1], 1), texture_ambient=round(ta[1], 1),
                                           pack=round(ps['fissure'], 1) if ps else None,
                                           lit_vs_pack=pct(tl[1], ps['fissure']) if ps else None),
                           scaled_photos=[dict(file=p['file'], px_per_mm=p['ppm'], spacing_mm=round(p['spacing'], 1),
                                               fissure_mm=round(p['fissure'], 1)) for p in ps['photos']] if ps else [])
        T = 360
        tiles = [Image.fromarray(np.tile(lit, (1, 1, 1))).resize((T, T), Image.LANCZOS), Image.fromarray(near),
                 Image.fromarray(graze), Image.fromarray(far)] + thumbs(files, T, 6)
        W = sum(x.size[0] for x in tiles) + 6 * len(tiles)
        row = Image.new('RGB', (max(W, 1800), T + 60), (36, 36, 38))
        x = 0
        for im in tiles:
            row.paste(im, (x, 56))
            x += im.size[0] + 6
        dr = ImageDraw.Draw(row)
        dr.text((4, 2), '%s (tile %.0f cm; species %s; upper bark of %s)' % (name, 6 * scale,
                                                                           ' '.join(t['species']) or '-',
                                                                           ' '.join(t.get('upper', [])) or '-'),
                fill=(235, 235, 235))
        dr.text((4, 20), 'texture (ambient): %s     pack: %s     research close-ups: %s' % (fmt(tm), fmt(pm),
                                                                                             rs or '-'),
                fill=(200, 210, 160))
        if ps:
            sc_line = ('spacing lit %.0f mm (%s), ambient %.0f mm; fissure lit %.1f mm (%s), ambient %.1f mm; '
                       'pack %.0f mm, %.1f mm (%d photos at scale)' % (tl[0], pct(tl[0], ps['spacing']), ta[0], tl[1],
                                                                       pct(tl[1], ps['fissure']), ta[1], ps['spacing'],
                                                                       ps['fissure'], ps['n']))
        else:
            sc_line = 'spacing lit %.0f mm, ambient %.0f mm; fissure lit %.1f mm, ambient %.1f mm; no photo at scale' % (
                tl[0], ta[0], tl[1], ta[1])
        dr.text((4, 36), sc_line + '   (views: lit tile, trunk near, near in a grazing light, at 3 m)',
                fill=(170, 200, 230))
        rows.append(row)
    MB.set_scale(1.0)
    write_rows(rows, 'bark', 6)
    return table


def leaf_sheets(photos, only):
    import make_leaf_textures as ML
    import tree_leaf_botany as TB
    man = json.load(open(os.path.join(HERE, 'leaf_manifest.json'), encoding='utf-8'))
    layers = man['layers']
    rows = []
    for sid in TB.ORDER:
        if only and sid not in only:
            continue
        src = TB.SHARED.get(sid, sid)
        e = man['species'].get(sid) or man['species'].get(src)
        if not e:
            continue
        idx = [i for i in (e['a'], e.get('b', -1), e.get('leaflet', -1), e.get('card', -1)) if i >= 0]
        idx = list(dict.fromkeys(idx))
        T = 300
        tiles = []
        for i in idx:
            l = layers[i]
            d = np.asarray(Image.open(os.path.join(HERE, 'build', 'leaf', l['name'] + '_d.png')))
            n = np.asarray(Image.open(os.path.join(HERE, 'build', 'leaf', l['name'] + '_n.png')))
            lit = Image.fromarray(ML.shade(d, n))
            L, W = l['box_mm']
            k = min(T / L, T / W)
            tiles.append(lit.resize((max(int(W * k), 1), max(int(L * k), 1)), Image.LANCZOS))
        pk = 'pin_syl_needles' if sid == 'pin_syl' else ('pic_abi_needles' if sid == 'pic_abi' else src + '_leaves')
        files = pack_files(photos, pk)
        tiles += thumbs(files, T, 7)
        Wd = sum(x.size[0] for x in tiles) + 6 * len(tiles)
        row = Image.new('RGB', (max(Wd, 1800), T + 24), (36, 36, 38))
        x = 0
        for im in tiles:
            row.paste(im, (x, 22))
            x += im.size[0] + 6
        names = ', '.join('%s %s %.0fx%.0f mm' % (layers[i]['name'], layers[i]['kind'], layers[i]['box_mm'][0],
                                                   layers[i]['box_mm'][1]) for i in idx)
        ImageDraw.Draw(row).text((4, 3), '%s (%s): %s   |   photos: %s (%d)' % (
            sid, (TB.TREES.get(src) or {}).get('latin', src), names, pk, len(files)), fill=(235, 235, 235))
        rows.append(row)
    write_rows(rows, 'leaves', 6)


def write_rows(rows, prefix, per):
    os.makedirs(OUT, exist_ok=True)
    for k in range(0, len(rows), per):
        ch = rows[k:k + per]
        W = max(r.size[0] for r in ch)
        img = Image.new('RGB', (W, sum(r.size[1] for r in ch)), (36, 36, 38))
        y = 0
        for r in ch:
            img.paste(r, (0, y))
            y += r.size[1]
        path = os.path.join(OUT, '%s_%02d.png' % (prefix, k // per + 1))
        img.save(path + '.tmp.png')
        os.replace(path + '.tmp.png', path)


def sources(photos):
    out = {}
    for p in sorted(os.listdir(photos)):
        idx = os.path.join(photos, p, 'index.json')
        if os.path.exists(idx):
            ex = set()
            if os.path.exists(os.path.join(photos, p, 'exclude.json')):
                ex = set(json.load(open(os.path.join(photos, p, 'exclude.json'), encoding='utf-8')))
            out[p] = [dict(file=e['file'], title=e['title'], url=e['url']) for e in
                      json.load(open(idx, encoding='utf-8')) if e['file'] not in ex]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--photos', required=True)
    ap.add_argument('--only', default='')
    ap.add_argument('--part', default='bark,leaves')
    ap.add_argument('--research', default='', help='the trees research tree_colours.json (its curated bark measures)')
    args = ap.parse_args()
    only = [x for x in args.only.split(',') if x]
    os.makedirs(OUT, exist_ok=True)
    if 'bark' in args.part:
        research = json.load(open(args.research, encoding='utf-8')) if args.research else None
        table = bark_sheets(args.photos, only, research)
        with open(os.path.join(OUT, 'bark_measures.json.tmp'), 'w', encoding='utf-8', newline='\r\n') as f:
            json.dump(table, f, indent=1)
        os.replace(os.path.join(OUT, 'bark_measures.json.tmp'), os.path.join(OUT, 'bark_measures.json'))
        for k, v in table.items():
            print('%-17s texture  %s\n%-17s pack     %s\n%-17s research %s' % (k, v['texture'], '', v['photos'], '',
                                                                          v['research']))
    if 'leaves' in args.part:
        leaf_sheets(args.photos, only)
    with open(os.path.join(OUT, 'sources.json.tmp'), 'w', encoding='utf-8', newline='\r\n') as f:
        json.dump(sources(args.photos), f, indent=1, ensure_ascii=False)
    os.replace(os.path.join(OUT, 'sources.json.tmp'), os.path.join(OUT, 'sources.json'))


if __name__ == '__main__':
    main()
