"""Season looks for the per-level textures (terrain colour, grass atlas, tree LOD atlas) and the PDA maps.

All of them are Dead Air's own files recoloured: towards the summer pack's file where one exists, towards the
average summer shift otherwise. Winter adds snow; spring walks from wet bare ground to fresh green.
Output: work/seasons_levels_out/<look>/<gamedata-relative path>.png plus a job list with the formats.
"""
import json
import os
import struct
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'trees'))
from common import WORK, load_rgba, save_png  # noqa: E402
import cards  # noqa: E402

DA_LEVELS = os.path.join(WORK, 'seasons', 'da_levels')
SUM_LEVELS = os.path.join(WORK, 'seasons', 'summer_green')
SUM_MAP = os.path.join(WORK, 'seasons', 'summer_karta')
DA_TEX = r"D:/Games/Dead Air/_work/texaudit/stock"
OUT = os.path.join(WORK, 'seasons_levels_out')
LOOKS = ['summer', 'winter', 'spring_early', 'spring_mid', 'spring_late']


def fmt(p):
    h = open(p, 'rb').read(128)
    four = h[84:88].decode('ascii', 'replace').strip('\0')
    mips = max(1, struct.unpack_from('<I', h, 28)[0])
    return {'DXT1': 'BC1_UNORM', 'DXT3': 'BC2_UNORM', 'DXT5': 'BC3_UNORM'}.get(four, 'B8G8R8A8_UNORM'), mips


def lab(img):
    return cv2.cvtColor(img[..., :3], cv2.COLOR_RGB2LAB).astype(np.float32)


def from_lab(L, alpha):
    return np.dstack([cv2.cvtColor(np.clip(L + 0.5, 0, 255).astype(np.uint8), cv2.COLOR_LAB2RGB), alpha])


def stats(img):
    m = img[..., 3] > 16 if img[..., 3].min() < 250 else np.ones(img.shape[:2], bool)
    L = lab(img)[m]
    return L.mean(0), L.std(0)


def transfer(img, tgt_mean, tgt_std):
    L = lab(img)
    sm, ss = stats(img)
    out = (L - sm) * (tgt_std / np.maximum(ss, 1e-3)) + tgt_mean
    return from_lab(out, img[..., 3])


def shift(img, dL=0, da=0, db=0, chroma=1.0):
    L = lab(img)
    L[..., 0] += dL
    L[..., 1] = 128 + (L[..., 1] - 128) * chroma + da
    L[..., 2] = 128 + (L[..., 2] - 128) * chroma + db
    return from_lab(L, img[..., 3])


def blend(a, b, t):
    return np.dstack([np.clip(a[..., :3] * (1 - t) + b[..., :3] * t + 0.5, 0, 255).astype(np.uint8), a[..., 3]])


def snow_ground(img, seed, cover=0.93):
    """Ground under snow: mostly white, the darkest structures (roads, banks, ruts) showing through."""
    rng = np.random.default_rng(seed)
    H, W = img.shape[:2]
    lum = lab(img)[..., 0] / 255.0
    n = cv2.resize(rng.random((max(4, H // 48), max(4, W // 48))).astype(np.float32), (W, H), interpolation=cv2.INTER_CUBIC)
    n2 = cv2.resize(rng.random((max(8, H // 12), max(8, W // 12))).astype(np.float32), (W, H), interpolation=cv2.INTER_CUBIC)
    dark = np.clip((0.32 - lum) / 0.2, 0, 1)
    c = np.clip(cover + 0.12 * (n - 0.5) + 0.06 * (n2 - 0.5) - 0.35 * dark, 0, 1)
    snowc = np.array([226, 231, 238], np.float32) * (0.92 + 0.08 * n2[..., None])
    base = shift(img, dL=-6, chroma=0.45)[..., :3].astype(np.float32)
    out = base * (1 - c[..., None]) + snowc * c[..., None]
    return np.dstack([np.clip(out + 0.5, 0, 255).astype(np.uint8), img[..., 3]])


def jobs():
    out = []
    for lvl in sorted(os.listdir(os.path.join(DA_LEVELS, 'levels'))):
        root = os.path.join(DA_LEVELS, 'levels', lvl)
        for rel in ['build_details.dds', 'level_lods.dds'] + ['terrain/' + f for f in sorted(os.listdir(os.path.join(root, 'terrain')))
                                                                 if f.endswith('.dds') and not f.endswith('_lm.dds')] if os.path.isdir(os.path.join(root, 'terrain')) else []:
            p = os.path.join(root, rel)
            if os.path.isfile(p):
                s = os.path.join(SUM_LEVELS, 'levels', lvl, rel)
                kind = 'terrain' if rel.startswith('terrain') else rel[:-4]
                out.append({'src': p, 'sum': s if os.path.isfile(s) else None, 'rel': f'levels/{lvl}/{rel}', 'kind': kind})
    for f in sorted(os.listdir(os.path.join(DA_TEX, 'textures', 'map'))):
        if not f.endswith('.dds'):
            continue
        if any(k in f for k in ('underground', 'lab', 'u_', 'warlab', 'bunker', 'sarcofag', 'control', 'nomap', 'fake_start', '#')):
            continue  # indoor maps do not change with the season
        s = os.path.join(SUM_MAP, 'textures', 'map', f)
        same = False
        if os.path.isfile(s):
            a, b = load_rgba(os.path.join(DA_TEX, 'textures', 'map', f)), load_rgba(s)
            same = a.shape == b.shape and np.abs(a.astype(np.int16) - b.astype(np.int16)).mean() < 1.0
        # Dead Air's maps are already summer renders: summer keeps them unless the summer pack differs
        out.append({'src': os.path.join(DA_TEX, 'textures', 'map', f), 'sum': s if os.path.isfile(s) and not same else None,
                    'rel': f'map/{f}', 'kind': 'map', 'summer_is_base': not os.path.isfile(s) or same})
    ug = os.path.join(DA_TEX, 'textures', 'ui', 'ui_global_map.dds')
    if os.path.isfile(ug):
        out.append({'src': ug, 'sum': os.path.join(SUM_MAP, 'textures', 'ui', 'ui_global_map.dds'), 'rel': 'ui/ui_global_map.dds', 'kind': 'map'})
    return out


def main():
    js = jobs()
    if sys.argv[1:] == ['maps']:
        js = [j for j in js if j['kind'] == 'map']
    if sys.argv[1:] == ['terrain']:
        js = [j for j in js if j['kind'] == 'terrain']
    # Average summer shift per kind, for files the summer pack does not cover
    avg = {}
    for j in js:
        if j['sum']:
            a, b = stats(load_rgba(j['src'])), stats(load_rgba(j['sum']))
            avg.setdefault(j['kind'], []).append((b[0] - a[0], b[1] / np.maximum(a[1], 1e-3)))
    avg = {k: (np.mean([x[0] for x in v], 0), np.mean([x[1] for x in v], 0)) for k, v in avg.items()}
    plan = []
    for k, j in enumerate(js):
        img = load_rgba(j['src'])
        if j['sum']:
            ref = load_rgba(j['sum'])
            tm, ts = stats(ref)
        elif j.get('summer_is_base'):
            tm, ts = stats(img)
        else:
            dm, ds = avg.get(j['kind'], avg.get('terrain'))
            sm, ss = stats(img)
            tm, ts = sm + dm, ss * ds
        summer = img.copy() if j.get('summer_is_base') else transfer(img, tm, ts)
        # Early spring: wet ground and last year's matted straw, neutral to warm (a cooler shift read as purple)
        early = shift(img, dL=0, chroma=0.6, da=-1, db=2)
        if j['kind'] == 'terrain':
            winter = snow_ground(img, k)
        elif j['kind'] == 'map':
            winter = snow_ground(img, k, cover=0.55)
        else:
            w = shift(img, dL=-8, chroma=0.45)
            winter = np.dstack([cards.snow(w[..., :3], w[..., 3], amount=1.2, frost=0.3, seed=k, depth_rel=0.004), w[..., 3]])
        looks = {'summer': summer, 'winter': winter, 'spring_early': early,
                 'spring_mid': blend(early, shift(summer, dL=8, db=8), 0.35),
                 'spring_late': blend(early, shift(summer, dL=6, db=5), 0.78)}
        f, mips = fmt(j['src'])
        for look, im in looks.items():
            save_png(os.path.join(OUT, look, j['rel'][:-4] + '.png'), im)
        plan.append({'rel': j['rel'], 'fmt': f, 'mips': mips, 'summer_ref': bool(j['sum']), 'skip_summer': bool(j.get('summer_is_base'))})
        print(j['rel'], f, 'ref' if j['sum'] else 'avg', flush=True)
    if sys.argv[1:] in (['maps'], ['terrain']) and os.path.isfile(os.path.join(OUT, 'plan.json')):
        mine = {p['rel'] for p in plan}
        prev = [p for p in json.load(open(os.path.join(OUT, 'plan.json'))) if p['rel'] not in mine]
        plan = prev + plan
    json.dump(plan, open(os.path.join(OUT, 'plan.json'), 'w'), indent=0)


if __name__ == '__main__':
    main()
