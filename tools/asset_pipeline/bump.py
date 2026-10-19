"""X-Ray bump pairs (`_bump` + `_bump#`), decoded and re-encoded the way the SDK does it.

_bump  RGBA = gloss, N.z, N.y, N.x   (packed (v+1)/2; X = +U, Y = +V i.e. down the image, Z out)
_bump# RGBA = error X, error Y, error Z (128 + 2/3 of the DXT5 error), height (white = top)
The shader rebuilds N = bump.wzy + bump#.xyz - 1.
"""
import os
import shutil

import cv2
import numpy as np
from PIL import Image

from common import load_rgba, resize, save_png, texconv


def decode(bump_path, bumpx_path=None):
    nu = load_rgba(bump_path).astype(np.float32) / 255.0
    h, w = nu.shape[:2]
    packed = np.stack([nu[..., 3], nu[..., 2], nu[..., 1]], -1)
    height = None
    if bumpx_path and os.path.isfile(bumpx_path):
        ne = load_rgba(bumpx_path).astype(np.float32) / 255.0
        if ne.shape[:2] != (h, w):
            ne = resize(ne, w, h, cv2.INTER_CUBIC)
        packed = packed + ne[..., :3] - 0.5
        height = ne[..., 3]
    n = packed * 2.0 - 1.0
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    return n, nu[..., 0], height


def detail_from(diffuse_rgb, w, h, strength=0.9, sigma=1.4):
    """Fine slope and height from a diffuse's luminance high-pass (SDK convention: n = (-dh/du, -dh/dv, 1))."""
    lum = diffuse_rgb.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32) / 255.0
    lum = resize(lum, w, h, cv2.INTER_AREA)
    hp = lum - cv2.GaussianBlur(lum, (0, 0), sigma * 3)
    hp = cv2.GaussianBlur(hp, (0, 0), 0.6)
    hp /= max(float(hp.std()), 1e-4)
    gx = cv2.Sobel(hp, cv2.CV_32F, 1, 0, ksize=3) / 8.0
    gy = cv2.Sobel(hp, cv2.CV_32F, 0, 1, ksize=3) / 8.0
    return -gx * strength * 0.12, -gy * strength * 0.12, hp


def upscale_pair(n, gloss, height, w, h, diffuse_rgb=None):
    n = resize(n, w, h, cv2.INTER_CUBIC)
    gloss = np.clip(resize(gloss, w, h, cv2.INTER_CUBIC), 0, 1)
    height = np.clip(resize(height, w, h, cv2.INTER_CUBIC), 0, 1) if height is not None else None
    if diffuse_rgb is not None:
        dx, dy, hp = detail_from(diffuse_rgb, w, h)
        # Whiteout-style blend: add the detail slope, keep the macro normal's Z
        n = np.dstack([n[..., 0] + dx * np.maximum(n[..., 2], 0.2), n[..., 1] + dy * np.maximum(n[..., 2], 0.2), n[..., 2]])
        if height is not None:
            height = np.clip(height + 0.035 * hp, 0, 1)
        elif True:
            height = np.clip(0.5 + 0.06 * cv2.GaussianBlur(hp, (0, 0), 2.0), 0, 1)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    if height is None:
        height = np.full((h, w), 127.0 / 255.0, np.float32)
    return n, gloss, height


def encode(n, gloss, height, out_bump, out_bumpx, mips=0):
    """Writes both DDS files (BC3, full mips) like the SDK's DXTCompressBump."""
    work = out_bump + '.work'
    os.makedirs(work, exist_ok=True)
    pk = np.clip(np.round((n + 1.0) * 127.5), 0, 255).astype(np.uint8)
    g8 = np.clip(np.round(gloss * 255.0), 1, 255).astype(np.uint8)
    rgba = np.dstack([g8, pk[..., 2], pk[..., 1], pk[..., 0]])
    p1 = os.path.join(work, 'b.png')
    save_png(p1, rgba)
    d1 = texconv(p1, work, 'BC3_UNORM', mips)
    dec = np.asarray(Image.open(d1).convert('RGBA')).astype(np.int32)
    orig = rgba.astype(np.int32)
    # X error from A, Y from B, Z from G (2/3 weight, as the SDK stores it)
    err = [(128 + np.trunc(2 * (orig[..., c] - dec[..., c]) / 3)).astype(np.int32) & 0xFF for c in (3, 2, 1)]
    h8 = np.clip(np.round(height * 255.0), 0, 255).astype(np.uint8)
    rgbx = np.dstack([err[0].astype(np.uint8), err[1].astype(np.uint8), err[2].astype(np.uint8), h8])
    p2 = os.path.join(work, 'x.png')
    save_png(p2, rgbx)
    d2 = texconv(p2, work, 'BC3_UNORM', mips)
    os.replace(d1, out_bump)
    os.replace(d2, out_bumpx)
    shutil.rmtree(work, ignore_errors=True)
