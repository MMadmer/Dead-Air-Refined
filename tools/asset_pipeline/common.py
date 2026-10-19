"""Shared paths and image helpers for the 1.6.0 replacement pipeline."""
import os
import struct
import subprocess
import tempfile

import cv2
import numpy as np
from PIL import Image

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
# Scratch outputs, the external binaries and the asset source; each can be moved with an env var.
WORK = os.environ.get('DAR_ASSET_WORK', os.path.join(REPO, '_work', 'asset_pipeline'))
TOOLS = os.environ.get('DAR_ASSET_TOOLS', r"D:/Tools/asset-pipeline")
TEXCONV = os.path.join(TOOLS, 'dxtex-build', 'bin', 'texconv.exe')
REALESRGAN = os.path.join(TOOLS, 'realesrgan', 'realesrgan-ncnn-vulkan.exe')

TREE16 = os.environ.get('DAR_ASSETS_GAMEDATA', os.path.join(REPO, '..', '..', 'Dead-Air-Refined_Assets', 'source', 'gamedata'))
# The 1.5.x content tree (published as content-1.4.0); only a rebuild from scratch reads it.
BORROWED = os.environ.get('DAR_ASSETS_1_5', os.path.join(os.path.dirname(TREE16), 'gamedata-1.5'))
ORIG = [r"D:/Games/Dead Air/_work/texaudit/dar2", r"D:/Games/Dead Air/_work/texaudit/stock"]
GSC_RAW = r"D:/Games/X-Ray CoP SDK/editors/rawdata/textures"


def orig_path(rel):
    """Dead Air's own file for a gamedata-relative path; DAR2 is mounted over the base archives."""
    for r in ORIG:
        p = os.path.join(r, rel)
        if os.path.isfile(p):
            return p
    return None


def dds_head(p):
    h = open(p, 'rb').read(148)
    if h[:4] != b'DDS ':
        return None
    height, width = struct.unpack_from('<II', h, 12)
    mips = max(1, struct.unpack_from('<I', h, 28)[0])
    pf_flags = struct.unpack_from('<I', h, 80)[0]
    caps2 = struct.unpack_from('<I', h, 112)[0]
    if pf_flags & 4:
        fmt = h[84:88].decode('ascii', 'replace').strip('\0')
    else:
        fmt = 'RGBA8' if pf_flags & 1 else 'RGB8'
    return {'w': width, 'h': height, 'mips': mips, 'fmt': fmt, 'cube': bool(caps2 & 0x200), 'vol': bool(caps2 & 0x200000)}


def load_rgba(path):
    """uint8 HxWx4."""
    return np.asarray(Image.open(path).convert('RGBA')).copy()


def save_png(path, arr):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = {2: 'L', 3: 'RGB'}.get(arr.ndim, 'RGBA') if arr.ndim != 3 else ('RGB' if arr.shape[2] == 3 else 'RGBA')
    Image.fromarray(arr, mode).save(path, compress_level=1)


def resize(arr, w, h, interp=cv2.INTER_LANCZOS4):
    """Float-safe resize; arr float32 or uint8, any channel count."""
    if arr.shape[1] == w and arr.shape[0] == h:
        return arr.copy()
    if w < arr.shape[1] and h < arr.shape[0] and interp == cv2.INTER_LANCZOS4:
        interp = cv2.INTER_AREA
    return cv2.resize(arr, (w, h), interpolation=interp)


def push_pull_fill(rgb, alpha, thresh=0.5):
    """Colour under transparent pixels taken from the nearest opaque ones, so filters do not drag black halos inward."""
    w = (alpha.astype(np.float32) / 255.0 > thresh).astype(np.float32)
    if w.min() > 0 or w.max() == 0:
        return rgb
    # Pull: premultiplied colour and coverage pyramids (means, so the ratio stays the colour)
    cs, ws = [rgb.astype(np.float32) * w[..., None]], [w]
    while min(ws[-1].shape) > 1:
        h2, w2 = max(1, ws[-1].shape[0] // 2), max(1, ws[-1].shape[1] // 2)
        cs.append(cv2.resize(cs[-1], (w2, h2), interpolation=cv2.INTER_AREA).reshape(h2, w2, 3))
        ws.append(cv2.resize(ws[-1], (w2, h2), interpolation=cv2.INTER_AREA).reshape(h2, w2))
    # Push: coarse colour fills where a level has no coverage of its own
    filled = cs[-1] / np.maximum(ws[-1], 1e-6)[..., None]
    for c0, w0 in zip(reversed(cs[:-1]), reversed(ws[:-1])):
        up = cv2.resize(filled, (c0.shape[1], c0.shape[0]), interpolation=cv2.INTER_LINEAR).reshape(c0.shape)
        own = c0 / np.maximum(w0, 1e-6)[..., None]
        k = np.clip(w0 * 2.0, 0, 1)[..., None]
        filled = own * k + up * (1 - k)
    out = np.where(w[..., None] > 0, rgb.astype(np.float32), filled)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def texconv(png, out_dir, fmt, mips=0, alpha_cutout=False, extra=()):
    """PNG -> legacy-header DDS (DXT1/3/5 FourCC like the game's own files). Returns the DDS path."""
    args = [TEXCONV, '-nologo', '-y', '-dx9', '-f', fmt, '-m', str(mips), '-o', out_dir]
    if alpha_cutout:
        # No -at here: in this texconv build it turns every BC1 block opaque
        args += ['-keepcoverage', '0.5']
    args += list(extra) + [png]
    r = subprocess.run(args, capture_output=True, text=True)
    out = os.path.join(out_dir, os.path.splitext(os.path.basename(png))[0] + '.dds')
    if r.returncode != 0 or not os.path.isfile(out):
        raise RuntimeError(f'texconv failed for {png}: {r.stdout[-400:]} {r.stderr[-400:]}')
    return out


DDS_FORMAT = {'DXT1': 'BC1_UNORM', 'DXT3': 'BC2_UNORM', 'DXT5': 'BC3_UNORM', 'RGBA8': 'B8G8R8A8_UNORM', 'RGB8': 'B8G8R8X8_UNORM'}


# WP7 season cleanup (08.10.2026): the season looks nothing draws any more left the game, and a re-run must not
# make them again. The list holds gamedata paths, forward slashes, lower case.
RETIRED_LIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'retired_season_looks.json')
_retired = None


def retired(rel):
    global _retired
    if _retired is None:
        import json
        _retired = set()
        if os.path.isfile(RETIRED_LIST):
            with open(RETIRED_LIST, encoding='utf-8') as f:
                _retired = set(json.load(f))
    return rel.replace('\\', '/').lower() in _retired


def install(src_file, rel):
    """Move a finished file into the 1.6.0 tree by rename, so a hardlinked file shared with the main tree is never written through."""
    if retired(rel):
        os.remove(src_file)
        print('retired by the WP7 season cleanup, not installed:', rel, flush=True)
        return
    dst = os.path.join(TREE16, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    os.replace(src_file, dst)


def remove16(rel):
    p = os.path.join(TREE16, rel)
    if os.path.isfile(p):
        os.remove(p)
        return True
    return False


def is_shared(rel):
    """True while the 1.6.0 file is still the main tree's file (same hardlink)."""
    a, b = os.path.join(TREE16, rel), os.path.join(BORROWED, rel)
    return os.path.isfile(a) and os.path.isfile(b) and os.path.samefile(a, b)
