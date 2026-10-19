"""AI upscale of a texture with its colour pinned to the source at the source's own scale.

Real-ESRGAN (x4plus, BSD-3) only contributes detail finer than one source texel; everything coarser
is the source itself. So the palette, the stains and the layout stay Dead Air's, the edges get sharp.
"""
import os
import shutil
import subprocess
import time

import cv2
import numpy as np

from common import REALESRGAN, load_rgba, push_pull_fill, resize, save_png

AI_SCALE = 4
MAX_FACTOR = 4.0


def plan_size(src_w, src_h, want_w, want_h):
    """Target size: what the borrowed file had, but never more than 4x the source per side."""
    k = min(want_w / src_w, want_h / src_h, MAX_FACTOR)
    k = max(k, 1.0)
    w, h = int(round(src_w * k)), int(round(src_h * k))
    # Keep powers of two where the source had them
    p2 = lambda v: 1 << int(round(np.log2(v)))
    if src_w & (src_w - 1) == 0:
        w = p2(w)
    if src_h & (src_h - 1) == 0:
        h = p2(h)
    return w, h


def alpha_kind(a):
    if a.min() >= 250:
        return 'none'
    binary = ((a < 16) | (a > 239)).mean()
    return 'cutout' if binary > 0.9 else 'smooth'


def prepare(src_rgba, in_png):
    """AI input: RGB with colour bled under transparency."""
    rgb = push_pull_fill(src_rgba[..., :3], src_rgba[..., 3])
    save_png(in_png, rgb)


def run_ai(in_dir, out_dir, tile=0, threads='1:2:2'):
    os.makedirs(out_dir, exist_ok=True)
    t = time.time()
    r = subprocess.run([REALESRGAN, '-i', in_dir, '-o', out_dir, '-n', 'realesrgan-x4plus', '-s', str(AI_SCALE),
                        '-f', 'png', '-t', str(tile), '-g', '1', '-j', threads], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('realesrgan failed: ' + r.stderr[-800:])
    return time.time() - t


def lock_palette(ai_rgb, src_rgb, w, h):
    """Detail from the AI image, everything coarser than a source texel from the source."""
    ai = resize(ai_rgb.astype(np.float32), w, h)
    base = resize(src_rgb.astype(np.float32), w, h, cv2.INTER_CUBIC)
    k = w / src_rgb.shape[1]
    sigma = max(0.6, 0.55 * k)
    hf = ai - cv2.GaussianBlur(ai, (0, 0), sigma)
    lf = cv2.GaussianBlur(base, (0, 0), sigma)
    return np.clip(lf + hf + 0.5, 0, 255).astype(np.uint8)


def add_grain(rgb, src_rgb, amount=0.35, seed=0, cap=12.0):
    """Texel-scale grain the AI smooths away, sized by the source's own fine-scale energy."""
    h, w = rgb.shape[:2]
    lum = src_rgb.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    hf = lum - cv2.GaussianBlur(lum, (0, 0), 1.0)
    energy = np.minimum(np.sqrt(cv2.GaussianBlur(hf * hf, (0, 0), 2.0)), cap)
    energy = resize(energy, w, h, cv2.INTER_CUBIC)
    rng = np.random.default_rng(seed)
    n = cv2.GaussianBlur(rng.standard_normal((h, w)).astype(np.float32), (0, 0), 0.8)
    n /= max(float(n.std()), 1e-6)
    out = rgb.astype(np.float32) + (amount * n * energy)[..., None]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def upscale_alpha(a, w, h, kind):
    up = resize(a.astype(np.float32), w, h, cv2.INTER_CUBIC)
    if kind == 'cutout':
        # Re-sharpen the mask edge to about a texel of the new size
        k = w / a.shape[1]
        up = (up - 127.5) * max(1.0, k * 0.9) + 127.5
    return np.clip(up + 0.5, 0, 255).astype(np.uint8)


def match_colour(rgb, ref):
    """Per-channel mean/spread transfer in Lab: the look of ref, the structure of rgb."""
    a = cv2.cvtColor(rgb, cv2.COLOR_RGB2LAB).astype(np.float32)
    b = cv2.cvtColor(ref, cv2.COLOR_RGB2LAB).astype(np.float32)
    for c in range(3):
        sa, sb = a[..., c].std(), b[..., c].std()
        a[..., c] = (a[..., c] - a[..., c].mean()) * (sb / sa if sa > 1e-3 else 1.0) + b[..., c].mean()
    return cv2.cvtColor(np.clip(a + 0.5, 0, 255).astype(np.uint8), cv2.COLOR_LAB2RGB)


def finish(src_rgba, ai_png, w, h, normal_like=False, grain=0.35, seed=0, base_rgba=None):
    """Final RGBA at w x h. base_rgba (Dead Air's own file) owns the palette when the AI ran on another source."""
    base = src_rgba
    if base_rgba is not None:
        if base_rgba.shape[:2] == src_rgba.shape[:2]:
            base = base_rgba
        else:
            # A bigger GSC source keeps its own coarse detail, recoloured to Dead Air's file
            base = np.dstack([match_colour(src_rgba[..., :3], base_rgba[..., :3]), src_rgba[..., 3]])
    a = base[..., 3]
    kind = alpha_kind(a)
    if normal_like:
        n = resize(base[..., :3].astype(np.float32) / 127.5 - 1.0, w, h, cv2.INTER_CUBIC)
        n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
        rgb = np.clip((n + 1.0) * 127.5 + 0.5, 0, 255).astype(np.uint8)
    elif ai_png is None:
        rgb = np.clip(resize(base[..., :3].astype(np.float32), w, h) + 0.5, 0, 255).astype(np.uint8)
    else:
        ai = cv2.cvtColor(cv2.imread(ai_png, cv2.IMREAD_COLOR), cv2.COLOR_BGR2RGB)
        rgb = lock_palette(ai, base[..., :3], w, h)
        if grain > 0 and w > base.shape[1]:
            rgb = add_grain(rgb, base[..., :3], grain, seed)
    alpha = upscale_alpha(a, w, h, kind) if kind != 'none' else np.full((h, w), 255, np.uint8)
    return np.dstack([rgb, alpha]), kind
