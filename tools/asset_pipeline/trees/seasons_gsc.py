"""Season looks for the reed / sedge / sprout / forest-wall / bush cards, from GSC's own CoP pictures.

The GSC picture is upscaled once (Real-ESRGAN x4plus); each look only recolours it to its target:
Dead Air's card (autumn), the summer pack's card (summer), autumn frosted with snow (winter),
last year's dry stems greening through the spring stages.
"""
import os
import re
import shutil
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import BORROWED, GSC_RAW, WORK, load_rgba, save_png  # noqa: E402
import cards  # noqa: E402
import upscale  # noqa: E402

STOCK = r"D:/Games/Dead Air/_work/texaudit/stock/textures/trees"
SUMMER = os.path.join(WORK, 'seasons', 'summer_green', 'textures', 'trees')
OUT = os.path.join(WORK, 'seasons_out')
TMP = os.path.join(WORK, 'seasons_gsc_tmp')
PREFIX = ('trees_kamish', 'trees_kamysh', 'trees_osoka', 'trees_tina', 'trees_sprouts', 'trees_forestwall', 'trees_a_bush',
          'trees_fake', 'trees_det2', 'trees_sosna_red_low')
VARIANT = re.compile(r'(_dry|_red|_sux|_green)+$')


def names():
    out = []
    for f in sorted(os.listdir(STOCK)):
        n = f[:-4]
        if f.endswith('.dds') and n.startswith(PREFIX) and '_bump' not in n:
            out.append(n)
    return out


def source_for(n):
    """GSC picture for the card itself, else for its family root, else Dead Air's own (already GSC's)."""
    for cand in (n, VARIANT.sub('', n), re.sub(r'_sux.*$', '_sux', n), VARIANT.sub('', n) + '_sux'):
        p = os.path.join(GSC_RAW, 'trees', cand + '.tga')
        if os.path.isfile(p):
            return p
    return os.path.join(STOCK, n + '.dds')


def out_size(n):
    p = os.path.join(BORROWED, f'textures/trees/{n}.dds')
    if os.path.isfile(p):
        return Image.open(p).size
    w, h = Image.open(os.path.join(STOCK, n + '.dds')).size
    k = min(2.0, 2048 / max(w, h))
    return int(w * k), int(h * k)


def recolour(img, ref):
    """Opaque pixels of img take the colour statistics of ref's opaque pixels."""
    m, mr = img[..., 3] > 127, ref[..., 3] > 127
    if m.sum() < 50 or mr.sum() < 50:
        return img
    out = img.copy()
    out[..., :3][m] = upscale.match_colour(img[..., :3][m][None], ref[..., :3][mr][None])[0]
    return out


def shifted(img, dL=0, da=0, db=0, chroma=1.0):
    lab = cv2.cvtColor(img[..., :3], cv2.COLOR_RGB2LAB).astype(np.float32)
    lab[..., 0] += dL
    lab[..., 1] = 128 + (lab[..., 1] - 128) * chroma + da
    lab[..., 2] = 128 + (lab[..., 2] - 128) * chroma + db
    rgb = cv2.cvtColor(np.clip(lab + 0.5, 0, 255).astype(np.uint8), cv2.COLOR_LAB2RGB)
    return np.dstack([rgb, img[..., 3]])


def blend(a, b, t):
    return np.dstack([np.clip(a[..., :3] * (1 - t) + b[..., :3] * t + 0.5, 0, 255).astype(np.uint8), a[..., 3]])


def main():
    ns = names()
    shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(os.path.join(TMP, 'in'))
    srcs = {}
    for n in ns:
        sp = source_for(n)
        if sp not in srcs:
            srcs[sp] = 'src%03d' % len(srcs)
            upscale.prepare(load_rgba(sp), os.path.join(TMP, 'in', srcs[sp] + '.png'))
    upscale.run_ai(os.path.join(TMP, 'in'), os.path.join(TMP, 'ai'))
    for n in ns:
        sp = source_for(n)
        W, H = out_size(n)
        src = load_rgba(sp)
        base, kind = upscale.finish(src, os.path.join(TMP, 'ai', srcs[sp] + '.png'), W, H, grain=0.2)
        da = load_rgba(os.path.join(STOCK, n + '.dds'))
        da_up = cv2.resize(da, (W, H), interpolation=cv2.INTER_AREA if da.shape[1] > W else cv2.INTER_CUBIC)
        autumn = recolour(base, da_up)
        sp_sum = os.path.join(SUMMER, n + '.dds')
        summer = recolour(base, load_rgba(sp_sum)) if os.path.isfile(sp_sum) else shifted(autumn, dL=-4, da=-16, db=4)
        dry_ref = os.path.join(STOCK, VARIANT.sub('', n) + '_dry.dds')
        early = recolour(base, load_rgba(dry_ref)) if os.path.isfile(dry_ref) and 'dry' not in n else shifted(autumn, dL=4, chroma=0.7)
        winter = shifted(autumn, dL=-8, chroma=0.5)
        winter = np.dstack([cards.snow(winter[..., :3], winter[..., 3], amount=1.2, frost=0.3, seed=len(n)), winter[..., 3]])
        looks = {'autumn': autumn, 'summer': summer, 'winter': winter, 'spring_early': early,
                 'spring_mid': blend(early, shifted(summer, dL=10, db=8), 0.35),
                 'spring_late': blend(early, shifted(summer, dL=8, db=6), 0.78)}
        for look, img in looks.items():
            os.makedirs(os.path.join(OUT, look), exist_ok=True)
            save_png(os.path.join(OUT, look, n + '.png'), img)
        print(n, W, H, kind, os.path.basename(sp), flush=True)


if __name__ == '__main__':
    main()
