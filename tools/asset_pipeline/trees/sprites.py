"""Leaf and needle-spray sprites cut out of CC0 atlases (ambientCG, Poly Haven).

Each sprite is stored upright: its attachment point (petiole or twig base) at the bottom centre.
"""
import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import TREE16  # noqa: E402

# The CC0 scans sit next to the asset source, outside git.
SRC = os.environ.get('DAR_ASSET_SOURCES', os.path.join(os.path.dirname(TREE16), 'cc0'))
ACG = os.path.join(SRC, 'ambientcg')
PH = os.path.join(SRC, 'polyhaven')


class Sprite:
    def __init__(self, rgba):
        self.rgba = rgba  # float32 HxWx4, premultiplied=False, 0..1
        self.h, self.w = rgba.shape[:2]


def _components(rgb, alpha, min_area):
    mask = (alpha > 0.5).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    out = []
    for i in range(1, n):
        x, y, w, h, area = stats[i]
        if area < min_area:
            continue
        sel = (lab[y:y + h, x:x + w] == i)
        a = alpha[y:y + h, x:x + w] * sel
        out.append((rgb[y:y + h, x:x + w], a))
    return out


def _upright(rgb, a):
    """Rotate so the long axis is vertical and the thin end (petiole/stem) is at the bottom."""
    ys, xs = np.nonzero(a > 0.5)
    pts = np.stack([xs, ys], 1).astype(np.float32)
    c = pts.mean(0)
    cov = np.cov((pts - c).T)
    evals, evecs = np.linalg.eigh(cov)
    axis = evecs[:, 1]
    proj = (pts - c) @ axis
    perp = (pts - c) @ evecs[:, 0]
    lo, hi = proj.min(), proj.max()
    span = hi - lo
    # Width near each end: the petiole end is the thin one
    end_lo = perp[proj < lo + 0.12 * span]
    end_hi = perp[proj > hi - 0.12 * span]
    w_lo = end_lo.max() - end_lo.min() if len(end_lo) else 0
    w_hi = end_hi.max() - end_hi.min() if len(end_hi) else 0
    if w_lo < w_hi:
        axis = -axis  # make +axis point from the petiole towards the tip
    # Angle that maps +axis to "up" (-y in image space)
    ang = np.degrees(np.arctan2(axis[1], axis[0])) + 90.0
    h, w = a.shape
    d = int(np.ceil(np.hypot(h, w))) + 4
    canvas = np.zeros((d, d, 4), np.float32)
    oy, ox = (d - h) // 2, (d - w) // 2
    canvas[oy:oy + h, ox:ox + w, :3] = rgb
    canvas[oy:oy + h, ox:ox + w, 3] = a
    M = cv2.getRotationMatrix2D((d / 2, d / 2), ang, 1.0)
    rot = cv2.warpAffine(canvas, M, (d, d), flags=cv2.INTER_CUBIC, borderValue=0)
    rot[..., 3] = np.clip(rot[..., 3], 0, 1)
    ys, xs = np.nonzero(rot[..., 3] > 0.05)
    rot = rot[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    # Bleed colour under transparency to avoid dark fringes when the sprite is scaled
    al = rot[..., 3]
    col = rot[..., :3]
    k = cv2.dilate((al > 0.5).astype(np.uint8), np.ones((5, 5), np.uint8))
    blur = cv2.GaussianBlur(col * (al > 0.5)[..., None], (0, 0), 3) / np.maximum(cv2.GaussianBlur((al > 0.5).astype(np.float32), (0, 0), 3), 1e-4)[..., None]
    rot[..., :3] = np.where((al > 0.5)[..., None], col, blur)
    return rot


def leaf_set(name, min_area=4000):
    base = os.path.join(ACG, name, f'{name}_2K-JPG')
    rgb = np.asarray(Image.open(base + '_Color.jpg').convert('RGB'), np.float32) / 255.0
    alpha = np.asarray(Image.open(base + '_Opacity.jpg').convert('L'), np.float32) / 255.0
    return [Sprite(_upright(c, a)) for c, a in _components(rgb, alpha, min_area)]


def _ph_alpha(path):
    a = np.asarray(Image.open(path))
    a = a.astype(np.float32)
    if a.ndim == 3:
        a = a[..., 0]
    return a / max(float(a.max()), 1.0)


def _stem_down(rgba):
    """Sprays: the woody stem end (least green) goes to the bottom."""
    a = rgba[..., 3] > 0.5
    h = rgba.shape[0]
    g = rgba[..., 1] - rgba[..., 0]
    top, bot = g[:h // 6][a[:h // 6]], g[-h // 6:][a[-h // 6:]]
    if len(top) and len(bot) and top.mean() < bot.mean():
        rgba = rgba[::-1, ::-1].copy()
    return rgba


# Hand-checked picks from each atlas: the rest are bark bands, cones or padding streaks
SPRAY_PICK = {'fir': [0, 1, 2, 3, 4, 5, 6], 'pine': [0, 1], 'fir_sapling': [0]}


def needle_sprays(name, min_area=6000):
    """Twig sprays from a Poly Haven conifer atlas."""
    prefix = {'fir': 'fir_tree_01_twig', 'pine': 'pine_tree_01_twig', 'fir_sapling': 'fir_sapling_twigs'}[name]
    rgb = np.asarray(Image.open(os.path.join(PH, prefix + '_diff_2k.png')).convert('RGB'), np.float32) / 255.0
    alpha = _ph_alpha(os.path.join(PH, prefix + '_alpha_2k.png'))
    comps = []
    for c, a in _components(rgb, alpha, min_area):
        h, w = a.shape
        m = a > 0.5
        mean = c[m].mean(0)
        # Sprays are green needles; long strips and solid blocks are bark, trunk or padding
        if max(h, w) / max(1, min(h, w)) > 12 or m.mean() > 0.75 or not (mean[1] >= mean[0] * 0.95 and mean[1] >= mean[2]):
            continue
        comps.append((c, a))
    return [Sprite(_stem_down(_upright(*comps[i]))) for i in SPRAY_PICK[name] if i < len(comps)]


if __name__ == '__main__':
    import sys
    out = os.path.join(os.path.dirname(SRC), 'work', 'sprites')
    os.makedirs(out, exist_ok=True)
    for name in ['LeafSet004', 'LeafSet007', 'LeafSet009', 'LeafSet012', 'LeafSet013', 'LeafSet014', 'LeafSet016', 'LeafSet024']:
        sp = leaf_set(name)
        print(name, len(sp), [s.rgba.shape[:2] for s in sp[:3]])
    for name in ['fir', 'pine', 'fir_sapling']:
        sp = needle_sprays(name)
        print(name, len(sp), [s.rgba.shape[:2] for s in sp[:6]])
        # contact sheet
        tiles = []
        for s in sp:
            t = s.rgba
            bg = np.full(t.shape[:2] + (3,), 0.43, np.float32)
            img = t[..., :3] * t[..., 3:] + bg * (1 - t[..., 3:])
            tiles.append(cv2.resize((img * 255).astype(np.uint8), (160, int(160 * t.shape[0] / t.shape[1]))))
        hmax = max(t.shape[0] for t in tiles)
        sheet = np.hstack([np.vstack([t, np.full((hmax - t.shape[0], 160, 3), 110, np.uint8)]) for t in tiles])
        Image.fromarray(sheet).save(os.path.join(out, f'{name}.png'))
