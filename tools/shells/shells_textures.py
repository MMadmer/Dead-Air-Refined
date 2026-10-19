"""The shared atlas of the spent cases (wpn\\dar_shells): the side of every finish, the heads, the primer and
the inside of the mouth, with the X-Ray bump pair (gloss in its alpha) and both .thm files.

    python shells_textures.py <out dir> [install]

Fired cases: soot darkens the neck and mouth, lathe lines run round the brass, lacquered steel is mottled.
"""
import os
import shutil
import struct
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'asset_pipeline'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bump  # noqa: E402
from common import install, save_png, texconv  # noqa: E402
from shells_data import ATLAS, BAND_PX, FINISHES  # noqa: E402

OUT = sys.argv[1]
NAME = 'dar_shells'
TEX = 'wpn\\' + NAME

# sRGB colour, gloss, lathe-line strength, mottling
LOOK = {
    'brass': ((204, 162, 88), 0.78, 0.06, 0.05),
    'steel_green': ((92, 98, 70), 0.42, 0.02, 0.10),
    'steel_brown': ((110, 84, 56), 0.40, 0.02, 0.10),
    'nickel': ((186, 188, 184), 0.85, 0.05, 0.04),
    'hull_red': ((150, 34, 30), 0.32, 0.0, 0.06),
}
SOOT = np.array((38, 33, 28), np.float32)
PRIMER = (206, 146, 92)
rng = np.random.default_rng(1833)


def write_thm(path, res, bump_name=None, is_bump=False):
    """X-Ray texture description, the SDK's own layout (version 0x12), as tools/scene_bag writes it."""
    def ch(cid, data):
        return struct.pack('<II', cid, len(data)) + data
    out = ch(0x810, struct.pack('<H', 0x12))
    out += ch(0x813, struct.pack('<I', 1))
    out += ch(0x812, struct.pack('<8I', 2 if is_bump else 3, 0x02000100, 0, 0, 0, 0, res, res))
    out += ch(0x814, struct.pack('<I', 2 if is_bump else 0))
    out += ch(0x815, bytes(1) + struct.pack('<f', 1.0))
    out += ch(0x816, struct.pack('<If', 1, 0.0 if is_bump else 0.7))
    if is_bump:
        out += ch(0x817, struct.pack('<fI', 0.05, 1) + bytes(1))
    else:
        out += ch(0x817, struct.pack('<fI', 0.05, 2) + bump_name.encode('cp1251') + bytes(1))
    out += ch(0x818, bytes(1))
    out += ch(0x819, bytes(1))
    with open(path, 'wb') as f:
        f.write(out)


def smooth_noise(h, w, scale):
    """Value noise at a coarse grid, bilinear up: lacquer mottling and wear."""
    gh, gw = max(2, h // scale + 2), max(2, w // scale + 2)
    g = rng.random((gh, gw)).astype(np.float32)
    ys = np.linspace(0, gh - 1.001, h)
    xs = np.linspace(0, gw - 1.001, w)
    y0, x0 = ys.astype(int), xs.astype(int)
    fy, fx = (ys - y0)[:, None], (xs - x0)[None, :]
    a = g[y0][:, x0]
    b = g[y0][:, x0 + 1]
    c = g[y0 + 1][:, x0]
    d = g[y0 + 1][:, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def main():
    col = np.zeros((ATLAS, ATLAS, 3), np.float32)
    gloss = np.full((ATLAS, ATLAS), 0.3, np.float32)
    height = np.full((ATLAS, ATLAS), 0.5, np.float32)

    # the sides: one band each, mouth at the top of the band
    for row, finish in enumerate(FINISHES):
        rgb, gl, lathe, mottle = LOOK[finish]
        y0 = row * BAND_PX
        t = 1.0 - (np.arange(BAND_PX, dtype=np.float32) - 2) / (BAND_PX - 4)
        t = np.clip(t, 0, 1)[:, None]
        base = np.array(rgb, np.float32)[None, None, :] * np.ones((BAND_PX, ATLAS, 1), np.float32)
        m = smooth_noise(BAND_PX, ATLAS, 16) - 0.5
        base *= (1.0 + mottle * 2.0 * m)[..., None]
        lines = np.sin(np.arange(BAND_PX, dtype=np.float32) * 2.9 + rng.random() * 6) * 0.5 + \
            (rng.random(BAND_PX).astype(np.float32) - 0.5)
        base *= (1.0 + lathe * lines)[:, None, None]
        # a fired case: soot up the neck, the extractor's scuffs at the head
        soot = np.clip((t - 0.80) / 0.20, 0, 1) ** 1.5 * (0.55 if finish == 'brass' else 0.7)
        streak = smooth_noise(BAND_PX, ATLAS, 8)
        soot = soot * (0.75 + 0.5 * streak)
        base = base * (1 - soot[..., None]) + SOOT[None, None, :] * soot[..., None]
        head = (t < 0.05).astype(np.float32) * 0.15
        base *= (1 - head)[..., None]
        col[y0:y0 + BAND_PX] = base
        gloss[y0:y0 + BAND_PX] = gl * (1 - 0.7 * soot[:, :] * np.ones((1, ATLAS)))
        height[y0:y0 + BAND_PX] = 0.5 + 0.08 * lathe * lines[:, None]

    # heads: a tile per finish, a headstamp of four dashes, the primer tile, the inside
    yy, xx = np.mgrid[0:BAND_PX, 0:BAND_PX].astype(np.float32)
    r = np.hypot(xx - BAND_PX / 2 + 0.5, yy - BAND_PX / 2 + 0.5) / (BAND_PX / 2 - 3)
    ang = np.arctan2(yy - BAND_PX / 2, xx - BAND_PX / 2)
    hy = 5 * BAND_PX
    for i, finish in enumerate(FINISHES):
        rgb, gl, _, _ = LOOK['brass' if finish == 'hull_red' else finish]
        tile = np.array(rgb, np.float32)[None, None, :] * np.ones((BAND_PX, BAND_PX, 1), np.float32) * 0.92
        stamp = ((np.abs(r - 0.68) < 0.07) & ((np.mod(ang + 0.3, np.pi / 2)) < 0.45)).astype(np.float32)
        tile *= (1 - 0.35 * stamp)[..., None]
        tile *= (1 - 0.25 * np.clip((r - 0.93) / 0.07, 0, 1))[..., None]
        col[hy:hy + BAND_PX, i * BAND_PX:(i + 1) * BAND_PX] = tile
        gloss[hy:hy + BAND_PX, i * BAND_PX:(i + 1) * BAND_PX] = gl * 0.8
        height[hy:hy + BAND_PX, i * BAND_PX:(i + 1) * BAND_PX] = 0.5 - 0.1 * stamp
    px = 6 * BAND_PX
    primer = np.array(PRIMER, np.float32)[None, None, :] * np.ones((BAND_PX, BAND_PX, 1), np.float32)
    dimple = np.clip(1 - r / 0.28, 0, 1)
    primer *= (1 - 0.45 * dimple - 0.3 * np.clip((r - 0.85) / 0.15, 0, 1))[..., None]
    col[hy:hy + BAND_PX, px:px + BAND_PX] = primer
    gloss[hy:hy + BAND_PX, px:px + BAND_PX] = 0.7
    height[hy:hy + BAND_PX, px:px + BAND_PX] = 0.5 - 0.3 * dimple
    iy = 6 * BAND_PX
    depth = (yy / BAND_PX)[..., None]
    col[iy:iy + BAND_PX, 0:BAND_PX] = SOOT[None, None, :] * (1.0 - 0.6 * depth)
    col[iy:iy + BAND_PX, BAND_PX:2 * BAND_PX] = 10
    gloss[iy:iy + BAND_PX, 0:2 * BAND_PX] = 0.05

    col8 = np.clip(col, 0, 255).astype(np.uint8)
    # normals from the height: lathe lines, the stamp, the dimple
    gy, gx = np.gradient(height * 6.0)
    n = np.dstack([-gx, -gy, np.ones_like(height)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    n_xr = np.dstack([n[..., 0], -n[..., 1], n[..., 2]])

    game = os.path.join(OUT, 'game')
    os.makedirs(game, exist_ok=True)
    work = tempfile.mkdtemp(dir=OUT)
    try:
        png = os.path.join(work, NAME + '.png')
        save_png(png, col8)
        os.replace(texconv(png, work, 'BC1_UNORM', 0), os.path.join(game, NAME + '.dds'))
        bump.encode(n_xr, gloss, height, os.path.join(game, NAME + '_bump.dds'), os.path.join(game, NAME + '_bump#.dds'))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    write_thm(os.path.join(game, NAME + '.thm'), ATLAS, bump_name=TEX + '_bump')
    write_thm(os.path.join(game, NAME + '_bump.thm'), ATLAS, is_bump=True)
    files = sorted(os.listdir(game))
    print('ATLAS', files)
    if len(sys.argv) > 2 and sys.argv[2] == 'install':
        for f in files:
            install(os.path.join(game, f), 'textures/wpn/' + f)
            print('installed textures/wpn/' + f)


main()
