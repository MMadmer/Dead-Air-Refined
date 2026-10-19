"""Winter versions of the ground materials (textures/grnd) and the terrain detail layers (textures/detail/detail_grnd_*).

The base is whatever the 1.6.0 tree shows (our upscale, or Dead Air's own file); snow cover depends on the
material: soil, grass and leaves go under, asphalt, concrete and gravel only get a dusting.
Output: work/seasons_ground_out/winter/<gamedata path>.png and a job list.
"""
import json
import os
import struct
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import TREE16, WORK, load_rgba, orig_path, save_png  # noqa: E402

OUT = os.path.join(WORK, 'seasons_ground_out')
DA = r"D:/Games/Dead Air/_work/texaudit/stock"


def fmt(p):
    h = open(p, 'rb').read(128)
    four = h[84:88].decode('ascii', 'replace').strip('\0')
    mips = max(1, struct.unpack_from('<I', h, 28)[0])
    return {'DXT1': 'BC1_UNORM', 'DXT3': 'BC2_UNORM', 'DXT5': 'BC3_UNORM'}.get(four, 'B8G8R8A8_UNORM'), mips


LIGHT = ('asphalt', 'asfalt', 'beton', 'crete', 'bitum', 'plit', 'road', 'doroga', 'kamen', 'ston', 'gravel', 'sheben', 'pebble', 'rail', 'shpal')


def wblur(a, sigma):
    """Gaussian blur that wraps around the edges, so a tiling texture stays seamless."""
    pad = int(sigma * 3) + 1
    big = np.pad(a, ((pad, pad), (pad, pad)) + ((0, 0),) * (a.ndim - 2), mode='wrap')
    return cv2.GaussianBlur(big, (0, 0), sigma)[pad:-pad, pad:-pad]


def cover_for(name):
    n = name.lower()
    return 0.25 if any(k in n for k in LIGHT) else 0.82


def snowy(img, cover, seed, relief=0.07):
    """Snow over a ground material: the material's relief shades the snow, so bumps and ruts still read."""
    rng = np.random.default_rng(seed)
    H, W = img.shape[:2]
    rgb = img[..., :3].astype(np.float32)
    lum = rgb.mean(2) / 255.0

    # Tileable noise: a periodic random field resized as a 3x3 tile and cropped keeps the texture seamless
    def tnoise(cell):
        g = rng.random((max(2, H // cell), max(2, W // cell))).astype(np.float32)
        g = np.tile(g, (3, 3))
        g = cv2.resize(g, (W * 3, H * 3), interpolation=cv2.INTER_CUBIC)
        return g[H:2 * H, W:2 * W]

    n = 0.5 * tnoise(24) + 0.5 * tnoise(8)
    hp = lum - wblur(lum, 3.0)
    hp /= max(float(hp.std()), 1e-4)
    # Cover: hollows (negative relief) fill first
    c = np.clip(cover + 0.15 * (n - 0.5) - 0.12 * hp, 0, 1)
    c = wblur(c, 1.0)
    shade = np.clip(0.86 + relief * hp + 0.04 * (n - 0.5), 0.6, 1.05)
    grain = 1.0 + 0.03 * (rng.random((H, W)).astype(np.float32) - 0.5)
    snowc = np.array([226, 231, 238], np.float32) * (shade * grain)[..., None]
    grey = rgb.mean(2, keepdims=True)
    base = (rgb * 0.5 + grey * 0.5) * 0.9
    out = base * (1 - c[..., None]) + snowc * c[..., None]
    return np.dstack([np.clip(out + 0.5, 0, 255).astype(np.uint8), img[..., 3]])


def main():
    # Retired by the WP7 season cleanup (08.10.2026): the ground category never wears a season look (the snow layer
    # whitens the ground), so the winter grnd/detail set left the game. Its helpers stay (seasons_winter_extra.py).
    sys.exit('seasons_ground.py is retired (WP7): no band wears a winter ground set')
    plan = []
    for sub, pref in (('grnd', ''), ('detail', 'detail_grnd')):
        names = set()
        for root in (os.path.join(TREE16, 'textures', sub), os.path.join(DA, 'textures', sub)):
            if os.path.isdir(root):
                names |= {f for f in os.listdir(root) if f.endswith('.dds') and f.startswith(pref) and '_bump' not in f}
        for k, f in enumerate(sorted(names)):
            rel = f'textures/{sub}/{f}'
            src = os.path.join(TREE16, rel)
            if not os.path.isfile(src):
                src = orig_path(rel)
            if not src:
                continue
            img = load_rgba(src)
            out = snowy(img, cover_for(f), k)
            save_png(os.path.join(OUT, 'winter', rel[len('textures/'):-4] + '.png'), out)
            fm, mips = fmt(src)
            plan.append({'rel': rel[len('textures/'):], 'fmt': fm, 'mips': mips})
            print(rel, img.shape[1], fm, flush=True)
    json.dump(plan, open(os.path.join(OUT, 'plan.json'), 'w'), indent=0)


if __name__ == '__main__':
    main()
