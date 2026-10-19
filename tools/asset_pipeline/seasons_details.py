"""Season looks of each level's grass atlas (build_details), sprite by sprite.

level.details names every detail object's mesh and its rectangle in the atlas, so each sprite is known with its
height above the ground (the mesh's vertical extent along the atlas v axis, times the object's mean scale) and its
kind, matched against a labelled catalogue of Dead Air's sprites (work/detail_clusters.pkl, _detail_clusters.py):
  winter        the engine takes the grass away ([season_grass]); what stands - the shrubs - keeps its whole
                height with snow at its foot and frost above; fallen leaves, ferns and broad leaves are hidden too
  spring_early  last year's grass pale straw with the first green at its foot; ferns and broad leaves are not up
                yet; old fallen leaves dark and wet
  spring_mid, spring_late: the colour walks from early spring to summer (seasons_levels.py); every sprite is back
Output: work/seasons_levels_out/<look>/levels/<level>/build_details.png for the four looks above
"""
import os
import pickle
import sys
from concurrent.futures import ProcessPoolExecutor

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'trees'))
from common import WORK, load_rgba, push_pull_fill, save_png  # noqa: E402
import cards  # noqa: E402
import seasons_levels as SL  # noqa: E402
import xr_level as X  # noqa: E402

DA_LEVELS = os.path.join(WORK, 'seasons', 'da_levels', 'levels')
OUT = os.path.join(WORK, 'seasons_levels_out')
OUTDOOR = ['jupiter', 'k00_marsh', 'k01_darkscape', 'k02_trucks_cemetery', 'l01_escape', 'l02_garbage', 'l03_agroprom',
           'l04_darkvalley', 'l05_bar', 'l06_rostok', 'l08_yantar', 'l09_deadcity', 'l10_limansk', 'l10_radar',
           'l10_red_forest', 'l11_hospital', 'l11_pripyat', 'l12_stancia', 'l12_stancia_2', 'l13_generators',
           'new_military', 'pripyat', 'zaton']
# Catalogue clusters by kind; anything else is grass (grasses, sedges, herbs and their dry stalks)
KIND = {**{i: 'fern' for i in (23, 24, 25)}, **{i: 'broad' for i in (15, 16, 17, 31, 32, 33, 36, 74)},
        **{i: 'shrub' for i in (8, 9, 10, 26, 27, 50, 51, 52, 53)}}
# Snow at the foot of a winter sprite, metres
SNOW_LINE = {'shrub': 0.15}
FRESH = np.array([96, 118, 64], np.float32)
CATALOGUE = pickle.load(open(os.path.join(WORK, 'detail_clusters.pkl'), 'rb'))


def corr(a, b):
    a = a.ravel() - a.mean()
    b = b.ravel() - b.mean()
    return float(a @ b / max(np.linalg.norm(a) * np.linalg.norm(b), 1e-6))


def sprites(lvl, img):
    """[(rect, kind, height of each atlas row in metres)] for the level's detail objects."""
    H, W = img.shape[:2]
    out, seen = [], set()
    for flags, mn, mx, V in X.detail_objects(os.path.join(DA_LEVELS, lvl, 'level.details')):
        u0, u1, v0, v1 = V[:, 3].min(), V[:, 3].max(), V[:, 4].min(), V[:, 4].max()
        # Rounded to the nearest texel edge, so sprites that touch in the atlas do not share a row
        r = (max(0, int(round(u0 * W))), max(0, int(round(v0 * H))), min(W, int(round(u1 * W))), min(H, int(round(v1 * H))))
        if r in seen or r[2] - r[0] < 4 or r[3] - r[1] < 4:
            continue
        seen.add(r)
        if np.ptp(V[:, 1]) < 0.05:
            out.append((r, 'flat', None))
            continue
        mask = cv2.resize((img[r[1]:r[3], r[0]:r[2], 3] > 127).astype(np.float32), (24, 48), interpolation=cv2.INTER_AREA)
        scores = [corr(mask, c['mask']) for c in CATALOGUE]
        best = int(np.argmax(scores))
        kind = KIND.get(best, 'grass') if scores[best] > 0.8 else 'grass'
        # The sprite's vertical axis runs along v: height of the mesh at each atlas row, in metres
        b, a = np.linalg.lstsq(np.c_[V[:, 4], np.ones(len(V))], V[:, 1], rcond=None)[0]
        rows = (np.arange(r[1], r[3]) + 0.5) / H
        h = np.maximum(a + b * rows, 0) * 0.5 * (mn + mx)
        out.append((r, kind, h))
    return out


def level(lvl):
    src = os.path.join(DA_LEVELS, lvl, 'build_details.dds')
    img = load_rgba(src)
    H, W = img.shape[:2]
    rng = np.random.default_rng(len(lvl))
    # Summer stays what seasons_levels.py made of it (towards the summer pack); the spring stages walk to it
    summer = load_rgba(os.path.join(OUT, 'summer', 'levels', lvl, 'build_details.png'))
    straw = SL.shift(img, dL=18, chroma=0.55, db=6)
    wet = SL.shift(img, dL=-8, chroma=0.6)
    woody = SL.shift(img, dL=-2, chroma=0.8)
    frost_straw = SL.shift(img, dL=10, chroma=0.5, db=4)
    frost_straw = np.dstack([cards.snow(frost_straw[..., :3], img[..., 3], amount=1.0, frost=0.25, seed=3, depth_rel=0.003), img[..., 3]])
    frost_wood = SL.shift(img, dL=-4, chroma=0.6)
    frost_wood = np.dstack([cards.snow(frost_wood[..., :3], img[..., 3], amount=1.2, frost=0.3, seed=5, depth_rel=0.003), img[..., 3]])
    young = SL.shift(summer, dL=12, db=10)
    early, winter = img.copy(), img.copy()
    mid = SL.blend(img, SL.shift(summer, dL=8, db=8), 0.35)
    late = SL.blend(img, SL.shift(summer, dL=6, db=5), 0.78)
    snowc = np.array([226, 231, 238], np.float32)
    # Hidden sprites go last: whatever a neighbour's rectangle still covers of them is cleared again
    for (x0, y0, x1, y1), kind, h in sorted(sprites(lvl, img), key=lambda s: s[1] in ('flat', 'fern', 'broad')):
        sl = np.s_[y0:y1, x0:x1]
        if kind == 'flat':
            early[sl] = wet[sl]
            winter[sl][..., 3] = 0
            mid[sl][..., :3] = SL.blend(wet[sl], summer[sl], 0.5)[..., :3]
            continue
        if kind in ('fern', 'broad'):
            winter[sl][..., 3] = 0
            early[sl][..., 3] = 0
            mid[sl][..., :3] = young[sl][..., :3]
            late[sl][..., :3] = SL.blend(young[sl], summer[sl], 0.85)[..., :3]
            continue
        hh = h[:, None] * np.ones((1, x1 - x0))
        # Early spring: pale straw; the first green comes up at the foot of the tuft
        base = (woody if kind == 'shrub' else straw)[sl]
        if kind == 'grass':
            g = np.clip(1 - hh / 0.12, 0, 1)[..., None] * 0.3
            base = np.dstack([np.clip(base[..., :3] * (1 - g) + FRESH * g + 0.5, 0, 255).astype(np.uint8), base[..., 3]])
        early[sl] = base
        mid[sl][..., :3] = SL.blend(base, SL.shift(summer[sl], dL=8, db=8), 0.35)[..., :3]
        late[sl][..., :3] = SL.blend(base, SL.shift(summer[sl], dL=6, db=5), 0.78)[..., :3]
        # Winter: the sprite keeps its whole height (the engine takes the grass away by season, the shrubs
        # stand on the ground, not over a cut); snow lies at the foot and clings a little higher, thinning out
        line = SNOW_LINE['shrub'] + 0.05 * (cv2.resize(rng.random((1, max(2, (x1 - x0) // 24))).astype(np.float32), (x1 - x0, 1),
                                                       interpolation=cv2.INTER_CUBIC) - 0.5)
        w = (frost_wood if kind == 'shrub' else frost_straw)[sl].copy()
        above = hh - line
        cling = np.where(above < 0, 1.0, np.clip(1 - above / 0.08, 0, 1) * (rng.random(above.shape) < 0.6))
        w[..., :3] = np.clip(w[..., :3] * (1 - cling[..., None]) + snowc * cling[..., None] + 0.5, 0, 255).astype(np.uint8)
        winter[sl] = w
    # WP7: no band wears a winter grass atlas (the default category has none), so the winter look is not written
    looks = {'spring_early': early, 'spring_mid': mid, 'spring_late': late}
    for look, im in looks.items():
        rgb = push_pull_fill(im[..., :3], im[..., 3], 0.02)
        save_png(os.path.join(OUT, look, 'levels', lvl, 'build_details.png'), np.dstack([rgb, im[..., 3]]))
    return lvl


def main():
    lvls = sys.argv[1:] or OUTDOOR
    with ProcessPoolExecutor(max_workers=min(6, len(lvls))) as ex:
        for lvl in ex.map(level, lvls):
            print(lvl, flush=True)


if __name__ == '__main__':
    main()
