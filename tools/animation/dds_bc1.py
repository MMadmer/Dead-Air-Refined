#!/usr/bin/env python3
"""Write an opaque RGB image as a DXT1 (BC1) .dds with a full mip chain.

Pillow reads DDS but cannot write block-compressed data, and X-Ray ships every diffuse as
DXT1+mips - handing the engine an uncompressed, mipless 2048x2048 costs 8x the file size,
8x the VRAM and makes the surface shimmer at distance. This is a straight vectorised BC1
encoder: bounding-box endpoints per 4x4 block (the classic scheme), 565 quantisation, four
palette entries, nearest-index assignment. Mips are box-filtered.

Usage: python dds_bc1.py <input image> <output .dds>
"""
import struct
import sys

import numpy as np
from PIL import Image

DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000
DDPF_FOURCC = 0x4
DDSCAPS_COMPLEX = 0x8
DDSCAPS_TEXTURE = 0x1000
DDSCAPS_MIPMAP = 0x400


def to565(c):
    """c: (...,3) float 0..255 -> uint16 565 and the color it decodes back to."""
    r = np.clip(np.rint(c[..., 0] * 31.0 / 255.0), 0, 31).astype(np.uint16)
    g = np.clip(np.rint(c[..., 1] * 63.0 / 255.0), 0, 63).astype(np.uint16)
    b = np.clip(np.rint(c[..., 2] * 31.0 / 255.0), 0, 31).astype(np.uint16)
    packed = (r << 11) | (g << 5) | b
    dec = np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], -1)
    return packed, dec.astype(np.float32)


def encode_bc1(img):
    """img: (H,W,3) uint8, H and W multiples of 4 -> bytes"""
    h, w = img.shape[:2]
    a = img.astype(np.float32)
    # (nby, 4, nbx, 4, 3) -> (nblocks, 16, 3)
    blocks = a.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)

    lo = blocks.min(1)
    hi = blocks.max(1)
    # Inset the bounding box: pulls the endpoints off the extremes, which is what keeps
    # flat-ish blocks from banding (the standard 1/16 inset).
    inset = (hi - lo) / 16.0
    lo_i = np.clip(lo + inset, 0, 255)
    hi_i = np.clip(hi - inset, 0, 255)

    c0p, c0 = to565(hi_i)
    c1p, c1 = to565(lo_i)

    # BC1 opaque mode requires c0 > c1; swap where needed (palette order flips with it).
    swap = c0p <= c1p
    c0p_f = np.where(swap, c1p, c0p)
    c1p_f = np.where(swap, c0p, c1p)
    c0_f = np.where(swap[:, None], c1, c0)
    c1_f = np.where(swap[:, None], c0, c1)
    # Degenerate block (single color): c0 == c1 is legal and decodes to that color.
    equal = c0p_f == c1p_f

    pal = np.stack([c0_f, c1_f, (2 * c0_f + c1_f) / 3.0, (c0_f + 2 * c1_f) / 3.0], 1)  # (n,4,3)
    d = ((blocks[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)  # (n,16,4)
    idx = d.argmin(-1).astype(np.uint32)
    idx[equal] = 0

    packed_idx = np.zeros(len(blocks), dtype=np.uint32)
    for i in range(16):
        packed_idx |= (idx[:, i] & 0x3) << (2 * i)

    out = np.empty((len(blocks), 8), dtype=np.uint8)
    out[:, 0] = c0p_f & 0xFF
    out[:, 1] = (c0p_f >> 8) & 0xFF
    out[:, 2] = c1p_f & 0xFF
    out[:, 3] = (c1p_f >> 8) & 0xFF
    out[:, 4] = packed_idx & 0xFF
    out[:, 5] = (packed_idx >> 8) & 0xFF
    out[:, 6] = (packed_idx >> 16) & 0xFF
    out[:, 7] = (packed_idx >> 24) & 0xFF
    return out.tobytes()


def mip_chain(img):
    """Box-filtered mips down to 1x1 (levels stop being 4-aligned but BC1 pads to 4)."""
    levels = [img]
    cur = img
    while max(cur.shape[:2]) > 1:
        h, w = cur.shape[:2]
        nh, nw = max(h // 2, 1), max(w // 2, 1)
        c = cur.astype(np.float32)
        if h > 1 and w > 1:
            c = c.reshape(nh, 2, nw, 2, 3).mean((1, 3))
        elif h > 1:
            c = c.reshape(nh, 2, w, 3).mean(1)
        else:
            c = c.reshape(h, nw, 2, 3).mean(2)
        cur = np.clip(np.rint(c), 0, 255).astype(np.uint8)
        levels.append(cur)
    return levels


def pad4(img):
    h, w = img.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        img = np.pad(img, ((0, ph), (0, pw), (0, 0)), mode="edge")
    return img


def write_dds(path, levels):
    h, w = levels[0].shape[:2]
    linear = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
    # DDS_HEADER is exactly 124 bytes after the magic: 7 dwords, 44 reserved, the 32-byte
    # DDS_PIXELFORMAT (size, flags, fourCC, bitcount, 4 masks) and 5 caps dwords.
    header = struct.pack(
        "<4sIIIIIII44sII4sIIIIIIIIII",
        b"DDS ", 124,
        DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE,
        h, w, linear, 0, len(levels),
        b"\0" * 44,
        32, DDPF_FOURCC, b"DXT1", 0, 0, 0, 0, 0,
        DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, 0, 0, 0, 0)
    assert len(header) == 128, len(header)
    with open(path, "wb") as f:
        f.write(header)
        for lv in levels:
            f.write(encode_bc1(pad4(lv)))


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGB")
    arr = np.asarray(im)
    levels = mip_chain(arr)
    write_dds(dst, levels)
    import os
    print(f"{dst}: {arr.shape[1]}x{arr.shape[0]} DXT1 mips={len(levels)} "
          f"{os.path.getsize(dst)} bytes")
