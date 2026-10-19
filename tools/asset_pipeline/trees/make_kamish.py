"""Reeds from GSC's own CoP textures (same three-clump layout as Dead Air's), upscaled and coloured to Dead Air."""
import os
import shutil
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import BORROWED, GSC_RAW, WORK, install, load_rgba, orig_path, save_png, texconv  # noqa: E402
import upscale  # noqa: E402

OUT = os.path.join(WORK, 'trees', 'kamish')
# variant -> GSC source picture
JOBS = {'trees_kamish_big_01': 'trees_kamish_big_01', 'trees_kamish_big_01_dry': 'trees_kamish_big_01',
        'trees_kamish_sux': 'trees_kamish_sux', 'trees_kamish_sux_dry': 'trees_kamish_sux'}


def main():
    ind, aid = os.path.join(OUT, 'in'), os.path.join(OUT, 'ai')
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(ind)
    srcs = {}
    for g in set(JOBS.values()):
        src = load_rgba(os.path.join(GSC_RAW, 'trees', g + '.tga'))
        srcs[g] = src
        upscale.prepare(src, os.path.join(ind, g + '.png'))
    upscale.run_ai(ind, aid)
    for name, g in JOBS.items():
        rel = f'textures/trees/{name}.dds'
        W, H = Image.open(os.path.join(BORROWED, rel)).size
        src = srcs[g]
        out, kind = upscale.finish(src, os.path.join(aid, g + '.png'), W, H, grain=0.2)
        da = load_rgba(orig_path(rel))
        # Colour of Dead Air's variant on the opaque pixels only
        m = out[..., 3] > 127
        md = da[..., 3] > 127
        rgb = out[..., :3].copy()
        sel = rgb[m][None]
        ref = da[..., :3][md][None]
        rgb[m] = upscale.match_colour(sel, ref)[0]
        img = np.dstack([rgb, out[..., 3]])
        png = os.path.join(OUT, name + '.png')
        save_png(png, img)
        dds = texconv(png, OUT, 'BC1_UNORM', 0, alpha_cutout=True)
        install(dds, rel)
        print(name, W, H, kind)


if __name__ == '__main__':
    main()
