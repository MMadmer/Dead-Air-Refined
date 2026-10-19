"""Every look of each outdoor level's tree LOD atlas (level_lods), rendered from the cards the near trees wear.

Dead Air's atlas was baked from its original cards, so a far tree used to keep Dead Air's crown in every season and
even in autumn no longer matched the 1.6.0 cards. Each vegetation object of the atlas is rendered again (lod_render)
in all six looks, autumn included. Colour and alpha follow Dead Air's own bake: the same objects are rendered with
Dead Air's original textures, and the mapping from that render to Dead Air's atlas (alpha by quantiles, colour by
linear gains) is applied to every look. Cells of cars, ruins and rocks keep Dead Air's pictures in every look.

Output: work/seasons_levels_out/<look>/levels/<level>/level_lods.png for the six looks.
"""
import json
import os
import sys
import tempfile
import time
import zipfile
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'trees'))
from common import ORIG, TREE16, WORK, push_pull_fill, save_png  # noqa: E402
import lod_render as R  # noqa: E402
import xr_level as X  # noqa: E402

DA_LEVELS = os.path.join(WORK, 'seasons', 'da_levels', 'levels')
OUT = os.path.join(WORK, 'seasons_levels_out')
CARDS = os.path.join(WORK, 'seasons_out')
CACHE = os.path.join(WORK, 'lod_cache')
LOOKS = ['autumn', 'summer', 'winter', 'spring_early', 'spring_mid', 'spring_late']
OUTDOOR = ['jupiter', 'k00_marsh', 'k01_darkscape', 'k02_trucks_cemetery', 'l01_escape', 'l02_garbage', 'l03_agroprom',
           'l04_darkvalley', 'l05_bar', 'l06_rostok', 'l08_yantar', 'l09_deadcity', 'l10_limansk', 'l10_radar',
           'l10_red_forest', 'l11_hospital', 'l11_pripyat', 'l12_stancia', 'l12_stancia_2', 'l13_generators',
           'new_military', 'pripyat', 'zaton']
BS = chr(92)
SEASON_CARDS = {f[:-4].lower() for f in os.listdir(os.path.join(CARDS, 'autumn')) if f.endswith('.png')}


def resolver(look):
    def r(name):
        rel = name.replace(BS, '/')
        if look != 'stock':
            if rel.lower().startswith('trees/'):
                for lk in (look, 'autumn'):
                    p = os.path.join(CARDS, lk, rel[6:] + '.png')
                    if os.path.isfile(p):
                        return p
            p = os.path.join(TREE16, 'textures', rel + '.dds')
            if os.path.isfile(p):
                return p
        for o in ORIG:
            p = os.path.join(o, 'textures', rel + '.dds')
            if os.path.isfile(p):
                return p
        return None
    return r


def is_vegetation(meshes):
    return any(m[3].lower().startswith('trees' + BS) and m[3].lower().split(BS)[-1] in SEASON_CARDS for m in meshes)


# Looks to render again over an existing cache (cards of these looks changed); all looks when unset. An entry
# redone for them carries a tag, so an object shared by several levels is redone once.
REDO = [lk for lk in os.environ.get('LOD_LOOKS', '').split(',') if lk]
REDO_TAG = 'redo_' + '_'.join(sorted(REDO))


def render_object(v, cells, meshes, texs, key):
    """{look: (col[8,h,w,3] float16, cov[8,h,w] float16)}, cached on disk per geometry and cell size."""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, f'{key}_{cells[0][2]}x{cells[0][3]}.npz')
    res, tags = {}, []
    if os.path.isfile(path):
        try:
            with open(path, 'rb') as fh, np.load(fh) as z:
                res = {lk: (z[lk + '_c'], z[lk + '_a']) for lk in ['stock'] + LOOKS}
                tags = [k for k in z.files if k.startswith('redo_')]
        except (OSError, ValueError, KeyError, zipfile.BadZipFile):
            # A torn entry (power lost after the rename) is rendered again from scratch
            res, tags = {}, []
        if res and (not REDO or REDO_TAG in tags):
            return res
    todo = REDO if res else ['stock'] + LOOKS
    out = {lk: ([], []) for lk in todo}
    for f in range(8):
        x0, y0, cw, ch = cells[f]
        frags, W, H = R.fragments(meshes, v['facets'][f], cw, ch)
        for lk in out:
            rgb, mask = R.shade(frags, W, H, texs[lk])
            col, cov = R.downsample(rgb, mask, cw, ch)
            out[lk][0].append(col.astype(np.float16))
            out[lk][1].append(cov.astype(np.float16))
    res.update({lk: (np.stack(c), np.stack(a)) for lk, (c, a) in out.items()})
    extra = {REDO_TAG: np.zeros(1, np.uint8)} if REDO else {}
    fd, tmp = tempfile.mkstemp(dir=CACHE, suffix='.npz')
    os.close(fd)
    np.savez(tmp, **{f'{lk}_c': c for lk, (c, a) in res.items()}, **{f'{lk}_a': a for lk, (c, a) in res.items()}, **extra)
    # Another level's worker may be reading the entry this instant: retry, and keep the result in memory regardless
    for _ in range(20):
        try:
            os.replace(tmp, path)
            break
        except PermissionError:
            time.sleep(0.25)
    else:
        os.remove(tmp)
    return res


def to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def calibration(pairs):
    """pairs: list of (stock col [n,3] 0..1, stock cov [n], DA rgba [n,4] uint8) over the vegetation cells.
    Returns the alpha LUT (xs, ys) and per-channel linear gains that take a render to Dead Air's bake.

    Gains, not a per-pixel regression: the render and the bake do not line up texel for texel, and a regression
    over misaligned pixels flattens the slope. A gain also carries over to colours the autumn bake never had (snow).
    """
    sc = np.concatenate([p[0] for p in pairs])
    sa = np.concatenate([p[1] for p in pairs]).astype(np.float32)
    da = np.concatenate([p[2] for p in pairs])
    a_da = da[:, 3].astype(np.float32) / 255
    # Alpha: quantile mapping of the rendered coverage onto Dead Air's alpha over the cells' pixels
    q = np.linspace(0, 1, 101)
    xs = np.quantile(sa, q)
    ys = np.quantile(a_da, q)
    xs, idx = np.unique(xs, return_index=True)
    ys = np.maximum.accumulate(ys[idx])
    m = (sa > 0.6) & (a_da > 0.6)
    if m.sum() < 200:
        return xs, ys, None
    g = to_lin(da[m, :3].astype(np.float32) / 255).mean(0) / np.maximum(to_lin(sc[m]).mean(0), 1e-4)
    return xs, ys, np.clip(g, 0.25, 2.0)


def apply_colour(col, gain):
    if gain is None:
        return np.clip(col * 255 + 0.5, 0, 255).astype(np.uint8)
    return np.clip(to_srgb(to_lin(col) * gain) * 255 + 0.5, 0, 255).astype(np.uint8)


def level(lvl):
    t0 = time.time()
    folder = os.path.join(DA_LEVELS, lvl)
    L = X.Level(folder)
    atlas = np.asarray(Image.open(os.path.join(folder, 'level_lods.dds')).convert('RGBA')).copy()
    AH, AW = atlas.shape[:2]
    texs = {lk: R.TexCache(resolver(lk)) for lk in ['stock'] + LOOKS}
    objs = R.lod_objects(L, AW, AH)
    veg = []
    for v, cells in objs:
        ms = R.leaf_meshes(L, v)
        if is_vegetation(ms):
            veg.append((v, cells, ms, R.object_key(ms)))
    renders = [render_object(v, cells, ms, texs, key) for v, cells, ms, key in veg]
    # Calibration against Dead Air's bake, pooled over the level
    pairs = []
    for (v, cells, ms, key), rr in zip(veg, renders):
        c, a = rr['stock']
        for f, (x0, y0, cw, ch) in enumerate(cells):
            da = atlas[y0:y0 + ch, x0:x0 + cw].reshape(-1, 4)
            pairs.append((c[f].reshape(-1, 3).astype(np.float32), a[f].reshape(-1).astype(np.float32), da))
    # A level without vegetation LODs keeps Dead Air's atlas in every look
    xs, ys, fit = calibration(pairs) if pairs else (np.array([0.0, 1.0]), np.array([0.0, 1.0]), None)
    # Cars, ruins and rocks keep Dead Air's cells in every look: their near models do not change either
    for look in LOOKS:
        img = atlas.copy()
        for (v, cells, ms, key), rr in zip(veg, renders):
            c, a = rr[look]
            for f, (x0, y0, cw, ch) in enumerate(cells):
                alpha = np.interp(a[f].astype(np.float32), xs, ys)
                img[y0:y0 + ch, x0:x0 + cw, :3] = apply_colour(c[f].astype(np.float32), fit)
                img[y0:y0 + ch, x0:x0 + cw, 3] = np.clip(alpha * 255 + 0.5, 0, 255).astype(np.uint8)
        rgb = push_pull_fill(img[..., :3], img[..., 3], 0.02)
        save_png(os.path.join(OUT, look, 'levels', lvl, 'level_lods.png'), np.dstack([rgb, img[..., 3]]))
    return lvl, len(objs), len(veg), fit, round(time.time() - t0)


def main():
    # Retired by the WP7 season cleanup (08.10.2026): level_lods has no season look; the engine composes the stock
    # path's atlas at runtime from the latched cards (DeadAir-x64-1.6.0 src/Layers/xrRender/da_trees_bake.cpp).
    sys.exit('seasons_lods.py is retired (WP7): the engine composes level_lods')
    lvls = sys.argv[1:] or OUTDOOR
    with ProcessPoolExecutor(max_workers=min(6, len(lvls))) as ex:
        for lvl, n, nv, fit, dt in ex.map(level, lvls):
            print(f'{lvl}: {nv}/{n} objects rendered, gain {None if fit is None else tuple(round(float(x), 2) for x in fit)}, {dt}s', flush=True)


if __name__ == '__main__':
    main()
