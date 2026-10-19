"""Bark strips from CC0 scans: pine (Poly Haven pine_bark), spruce (ambientCG Bark014).

A strip wraps around the trunk (U) and repeats along it (V): built from a seamless tile, stacked with
per-tile horizontal shifts and cyclic cross-fades so the repeat is not obvious. Colour is Dead Air's.
"""
import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import BORROWED, WORK, install, load_rgba, orig_path, save_png, texconv  # noqa: E402
import bump  # noqa: E402
import upscale  # noqa: E402
from sprites import ACG, PH  # noqa: E402

OUT = os.path.join(WORK, 'trees', 'bark')

SOURCES = {
    'trees_atp_pine_bark': dict(color=os.path.join(ACG, 'Bark001', 'Bark001_2K-JPG_Color.jpg'),
                                normal=os.path.join(ACG, 'Bark001', 'Bark001_2K-JPG_NormalDX.jpg'), normal_gl=False,
                                height=os.path.join(ACG, 'Bark001', 'Bark001_2K-JPG_Displacement.jpg'), tile=512, crop=0.5),
    'trees_atp_spruce_bark': dict(color=os.path.join(ACG, 'Bark006', 'Bark006_2K-JPG_Color.jpg'),
                                  normal=os.path.join(ACG, 'Bark006', 'Bark006_2K-JPG_NormalDX.jpg'), normal_gl=False,
                                  height=os.path.join(ACG, 'Bark006', 'Bark006_2K-JPG_Displacement.jpg'), tile=512, crop=0.5),
}


def load_f(path, gray=False):
    im = Image.open(path)
    if gray:
        a = np.asarray(im).astype(np.float32)
        if a.ndim == 3:
            a = a[..., 0]
        return a / (65535.0 if a.max() > 255 else 255.0)
    return np.asarray(im.convert('RGB')).astype(np.float32) / 255.0


def wrap_seamless(t, band=0.18):
    """Horizontally seamless: edges come from a half-width rolled copy whose seam lands in the middle."""
    W = t.shape[1]
    x = np.arange(W) / (W - 1)
    edge = np.clip(np.minimum(x, 1 - x) / band, 0, 1)
    edge = edge * edge * (3 - 2 * edge)
    m = edge.reshape(1, W, *([1] * (t.ndim - 2)))
    return t * m + np.roll(t, W // 2, axis=1) * (1 - m)


def strip(maps, tile, W, H, rng, fade=48, crop=1.0):
    """maps: list of HxWxC float tiles (seamless). Returns the same maps as W x H strips."""
    reps = int(np.ceil(H / tile))
    shifts = [int(rng.integers(0, W)) for _ in range(reps)]
    outs = []
    src_h, src_w = maps[0].shape[:2]
    cw, ch = int(src_w * crop), int(src_h * crop)
    ox, oy = int(rng.integers(0, src_w - cw + 1)), int(rng.integers(0, src_h - ch + 1))
    for m in maps:
        m = m[oy:oy + ch, ox:ox + cw]
        t = cv2.resize(m, (W, tile), interpolation=cv2.INTER_AREA)
        if crop < 1.0:
            t = wrap_seamless(t)
        if t.ndim == 2:
            t = t[..., None]
        rows = [np.roll(t, s, axis=1) for s in shifts]
        out = np.concatenate(rows, 0)[:H].copy()
        # Before each boundary the previous tile fades into the next tile's own (seamless) continuation above its top
        w = ((np.arange(fade) + 0.5) / fade)[:, None, None]
        for k in range(1, reps + 1):
            y = (k * tile) % H if k < reps else H
            prv, nxt = rows[k - 1], rows[k % reps]
            seg = slice(y - fade, y)
            out[seg] = prv[tile - fade:tile] * (1 - w) + nxt[tile - fade:tile] * w
        outs.append(out[..., 0] if m.ndim == 2 else out)
    return outs


def make(name, cfg):
    rel = f'textures/trees/{name}.dds'
    W, H = Image.open(os.path.join(BORROWED, rel)).size
    rng = np.random.default_rng(abs(hash(name)) % (2 ** 32))
    col = load_f(cfg['color'])
    nrm = load_f(cfg['normal']) * 2 - 1
    if cfg['normal_gl']:
        nrm[..., 1] = -nrm[..., 1]  # OpenGL green up -> X-Ray/DirectX (+V down)
    hgt = load_f(cfg['height'], gray=True)
    col, nrm, hgt = strip([col, nrm, hgt], cfg['tile'], W, H, rng, crop=cfg.get('crop', 1.0))
    nrm /= np.maximum(np.linalg.norm(nrm, axis=2, keepdims=True), 1e-6)
    da = load_rgba(orig_path(rel))
    rgb = upscale.match_colour(np.clip(col * 255 + 0.5, 0, 255).astype(np.uint8), da[..., :3])
    os.makedirs(OUT, exist_ok=True)
    png = os.path.join(OUT, name + '.png')
    save_png(png, np.dstack([rgb, np.full(rgb.shape[:2], 255, np.uint8)]))
    dds = texconv(png, OUT, 'BC1_UNORM', 0)
    install(dds, rel)
    # Bump pair: scan normal, scan height re-centred on 127, low gloss from the albedo
    brel = f'textures/trees/{name}_bump.dds'
    bw, bh = Image.open(os.path.join(BORROWED, brel)).size
    n = cv2.resize(nrm, (bw, bh), interpolation=cv2.INTER_AREA)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    h = cv2.resize(hgt, (bw, bh), interpolation=cv2.INTER_AREA)
    h = np.clip(0.5 + (h - np.median(h)) * 0.9, 0, 1)
    lum = cv2.resize(rgb.astype(np.float32).mean(2) / 255.0, (bw, bh), interpolation=cv2.INTER_AREA)
    gloss = np.clip(0.06 + 0.12 * lum, 0, 1)
    ob, ox = os.path.join(OUT, name + '_bump.dds'), os.path.join(OUT, name + '_bumpX.dds')
    bump.encode(n, gloss, h, ob, ox)
    install(ob, brel)
    install(ox, f'textures/trees/{name}_bump#.dds')
    print(name, W, H, 'bump', bw, bh)


if __name__ == '__main__':
    for k, v in SOURCES.items():
        make(k, v)
