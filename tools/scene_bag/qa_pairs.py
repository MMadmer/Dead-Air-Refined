"""Pairs of in-game frames, the original bag (bags-ref) | ours (bags-new), per pass of the bags scenario. The
two captures start at slightly different moments of the scene, so they are aligned first: the hand's skin
pixels give its centroid per frame, and the frame offset that best matches the two tracks is used.
    python qa_pairs.py [ref_dir new_dir]"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, 'qa', sys.argv[1] if len(sys.argv) > 2 else 'bags-ref')
NEW = os.path.join(HERE, 'qa', sys.argv[2] if len(sys.argv) > 2 else 'bags-new')
BOX = (560, 440, 2560, 1440)
NAMES = ['1 empty, suit on', '2 empty', '3 kit_hunt', '4 backpack_light', '5 backpack_heavy', '6 backpack_travel',
         '7 backpack_stalker', '8 exobackpack']


def caps(d):
    return sorted(x for x in os.listdir(d) if x.startswith('capture_'))


def track(folder):
    """Hand centroid (x, y) per frame from skin-coloured pixels in the lower part of the frame, NaN without
    a hand."""
    out = []
    for f in sorted(os.listdir(folder)):
        im = np.asarray(Image.open(os.path.join(folder, f)).convert('RGB').resize((320, 180))).astype(np.int16)
        r, g, b = im[..., 0], im[..., 1], im[..., 2]
        m = (r > 140) & (r - g > 28) & (g - b > 8)
        m[:60] = False
        ys, xs = np.nonzero(m)
        out.append((xs.mean(), ys.mean()) if len(xs) > 40 else (np.nan, np.nan))
    return np.array(out)


def best_offset(ta, tb, span=8):
    best = (1e9, 0)
    for k in range(-span, span + 1):
        d = []
        for i in range(len(ta)):
            j = i + k
            if 0 <= j < len(tb) and not np.isnan(ta[i, 0]) and not np.isnan(tb[j, 0]):
                d.append(np.hypot(*(ta[i] - tb[j])))
        if len(d) >= 5 and np.mean(d) < best[0]:
            best = (float(np.mean(d)), k)
    return best[1], best[0]


ref, new = caps(REF), caps(NEW)
for k, (a, b) in enumerate(zip(ref, new)):
    da, db = os.path.join(REF, a), os.path.join(NEW, b)
    fa, fb = sorted(os.listdir(da)), sorted(os.listdir(db))
    off, err = best_offset(track(da), track(db))
    # the reference frames where the hand reaches for the pull, pulls it and the bag is held
    frames = [10, 13, 15, 16, 17, 18, 19, 20, 24, 32]
    s = 0.36
    w, h = int((BOX[2] - BOX[0]) * s), int((BOX[3] - BOX[1]) * s)
    rows = [f for f in frames if f < len(fa) and 0 <= f + off < len(fb)]
    sheet = Image.new('RGB', (w * 2 + 6, h * len(rows)), (20, 20, 20))
    d = ImageDraw.Draw(sheet)
    for r, f in enumerate(rows):
        ia = Image.open(os.path.join(da, fa[f])).convert('RGB').crop(BOX).resize((w, h))
        ib = Image.open(os.path.join(db, fb[f + off])).convert('RGB').crop(BOX).resize((w, h))
        sheet.paste(ia, (0, r * h))
        sheet.paste(ib, (w + 6, r * h))
        d.text((6, r * h + 4), f'{NAMES[k] if k < len(NAMES) else k}: ref {f} | ours {f + off}', fill=(255, 255, 0))
    out = os.path.join(HERE, 'qa', f'pairs_{k + 1}.jpg')
    sheet.save(out, quality=86)
    print(out, 'offset', off, 'track error %.1f px' % err)
