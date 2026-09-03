"""Cloud noise volumes for the volumetric deck (Schneider/Nubis layout), written as DDS
volume textures with a full mip chain.

  da_cloud_shape.dds  128^3 RGBA8: R = Perlin-Worley (base shape), G/B/A = Worley FBM at
                      rising frequencies (the shape erosion octaves)
  da_cloud_detail.dds  32^3 RGBA8: RGB = Worley FBM at three frequencies (edge erosion), A = 1

Everything tiles. Deterministic seed, so a rebuild produces identical files.
"""
import sys, struct, os
import numpy as np

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
rng = np.random.default_rng(1337)


def worley_slice(z_coords, n, cells, seed_pts, invert=True):
    """Distance to the nearest feature point for one z-slice, tiling on `cells` cells.
    seed_pts: (cells, cells, cells, 3) points in cell-local [0,1)."""
    zs = z_coords[:, None, None]
    y, x = np.mgrid[0:n, 0:n]
    px = (x[None] + 0.5) / n * cells  # in cell units
    py = (y[None] + 0.5) / n * cells
    pz = (zs + 0.5) / n * cells
    cx = np.floor(px).astype(int); cy = np.floor(py).astype(int); cz = np.floor(pz).astype(int)
    best = np.full(px.shape, 9.0, dtype=np.float32)
    for dz in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                ix = (cx + dx) % cells; iy = (cy + dy) % cells; iz = (cz + dz) % cells
                pts = seed_pts[iz, iy, ix]  # (..., 3)
                fx = (cx + dx) + pts[..., 0]
                fy = (cy + dy) + pts[..., 1]
                fz = (cz + dz) + pts[..., 2]
                d = np.sqrt((px - fx) ** 2 + (py - fy) ** 2 + (pz - fz) ** 2)
                best = np.minimum(best, d)
    best = np.clip(best, 0, 1)
    return (1.0 - best) if invert else best


def worley(n, cells):
    pts = rng.random((cells, cells, cells, 3)).astype(np.float32)
    out = np.empty((n, n, n), dtype=np.float32)
    step = 16
    for z0 in range(0, n, step):
        zc = np.arange(z0, min(z0 + step, n))
        out[z0:z0 + len(zc)] = worley_slice(zc, n, cells, pts)
    return out


def fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)


def perlin(n, cells):
    """Tiling 3D gradient noise, output remapped to ~[0,1]."""
    grads = rng.normal(size=(cells, cells, cells, 3)).astype(np.float32)
    grads /= np.linalg.norm(grads, axis=-1, keepdims=True)
    out = np.empty((n, n, n), dtype=np.float32)
    y, x = np.mgrid[0:n, 0:n]
    px = (x + 0.5) / n * cells; py = (y + 0.5) / n * cells
    for z in range(n):
        pz = (z + 0.5) / n * cells
        x0 = np.floor(px).astype(int); y0 = np.floor(py).astype(int); z0 = int(np.floor(pz))
        fx = px - x0; fy = py - y0; fz = pz - z0
        ux, uy, uz = fade(fx), fade(fy), fade(fz)
        acc = 0.0
        val = np.zeros_like(px)
        for dz in (0, 1):
            for dy in (0, 1):
                for dx in (0, 1):
                    g = grads[(z0 + dz) % cells, (y0 + dy) % cells, (x0 + dx) % cells]
                    dot = g[..., 0] * (fx - dx) + g[..., 1] * (fy - dy) + g[..., 2] * (fz - dz)
                    w = (ux if dx else 1 - ux) * (uy if dy else 1 - uy) * (uz if dz else 1 - uz)
                    val += w * dot
        out[z] = val
    out = out / (np.abs(out).max() + 1e-6)
    return out * 0.5 + 0.5


def fbm(fn, n, base_cells, octaves, gain=0.5):
    total = np.zeros((n, n, n), dtype=np.float32)
    amp = 1.0; norm = 0.0
    for i in range(octaves):
        total += amp * fn(n, base_cells * (2 ** i))
        norm += amp
        amp *= gain
    return total / norm


def remap(v, lo0, hi0, lo1, hi1):
    return lo1 + (v - lo0) / (hi0 - lo0 + 1e-6) * (hi1 - lo1)


def to_u8(a):
    return np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8)


def mips(vol):
    """vol: (d, h, w, 4) uint8 -> list of levels down to 1^3 (box filter)."""
    levels = [vol]
    cur = vol.astype(np.float32)
    while cur.shape[0] > 1:
        d, h, w, c = cur.shape
        cur = cur.reshape(d // 2, 2, h // 2, 2, w // 2, 2, c).mean(axis=(1, 3, 5))
        levels.append(np.clip(cur + 0.5, 0, 255).astype(np.uint8))
    return levels


def write_dds(path, levels):
    d, h, w, _ = levels[0].shape
    DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PITCH, DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT, DDSD_DEPTH = \
        0x1, 0x2, 0x4, 0x8, 0x1000, 0x20000, 0x800000
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_DEPTH
    pf = struct.pack('<IIIIIIII', 32, 0x41, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000)
    caps = 0x1000 | 0x8 | 0x400000  # TEXTURE | COMPLEX | MIPMAP
    caps2 = 0x200000  # VOLUME
    header = struct.pack('<4sIIIIIII', b'DDS ', 124, flags, h, w, w * 4, d, len(levels))
    header += b'\0' * 44 + pf + struct.pack('<IIIII', caps, caps2, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(header)
        for lv in levels:
            f.write(np.ascontiguousarray(lv).tobytes())
    print("wrote", path, "%dx%dx%d" % (w, h, d), "mips", len(levels), "bytes", os.path.getsize(path))



def stretch(a, lo_p=2.0, hi_p=98.0):
    """Percentile contrast stretch: the raw noises sit in a narrow band (max-normalised Perlin,
    clipped Worley), which the density remaps then flatten into one uniform slab."""
    lo, hi = np.percentile(a, [lo_p, hi_p])
    return np.clip((a - lo) / max(hi - lo, 1e-6), 0, 1).astype(np.float32)


# ---- shape: 128^3 ----
N = 128
print("shape: perlin fbm")
p = stretch(fbm(perlin, N, 4, 3))
print("shape: worley octaves")
w1 = stretch(fbm(worley, N, 4, 3))
w2 = stretch(fbm(worley, N, 8, 3))
w3 = stretch(fbm(worley, N, 16, 3))
# Perlin-Worley: the Perlin billows dilated by the inverted Worley - the classic base shape.
pw = stretch(np.clip(remap(p, -(1.0 - w1), 1.0, 0.0, 1.0), 0, 1))
shape = np.stack([to_u8(pw), to_u8(w1), to_u8(w2), to_u8(w3)], axis=-1)
write_dds(os.path.join(OUT, "da_cloud_shape.dds"), mips(shape))

# ---- detail: 32^3 ----
M = 32
print("detail: worley octaves")
d1 = stretch(fbm(worley, M, 2, 3))
d2 = stretch(fbm(worley, M, 4, 3))
d3 = stretch(fbm(worley, M, 8, 3))
detail = np.stack([to_u8(d1), to_u8(d2), to_u8(d3), np.full((M, M, M), 255, np.uint8)], axis=-1)
write_dds(os.path.join(OUT, "da_cloud_detail.dds"), mips(detail))
print("done")
