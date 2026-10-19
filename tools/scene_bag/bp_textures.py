"""Bakes of one scene bag -> game files in out/<v>/game: the albedo (DXT1), the X-Ray bump pair (gloss in
the bump alpha, the bake's tangent normal with Y turned to run down the image), both .thm files and the
OGF; `install` moves them into the 1.6.0 tree.

    python bp_textures.py <variant> [install]"""
import os
import shutil
import struct
import sys
import tempfile

os.environ['OPENCV_IO_ENABLE_OPENEXR'] = '1'
import cv2  # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'asset_pipeline'))
import bump  # noqa: E402
from common import install, push_pull_fill, save_png, texconv  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = sys.argv[1]
OUT = os.path.join(HERE, 'out', NAME)
GAME = os.path.join(OUT, 'game')
TEX = f'act\\dar_bag_{NAME}'
OGF_REL = f'meshes/dynamics/weapons/wpn_eat/backpack/dar_bag_{NAME}_hud.ogf'


def write_thm(path, res, bump_name=None, is_bump=False):
    """X-Ray texture description, the layout of the SDK's own files (version 0x12)."""
    def ch(cid, data):
        return struct.pack('<II', cid, len(data)) + data
    out = ch(0x810, struct.pack('<H', 0x12))
    out += ch(0x813, struct.pack('<I', 1))
    fmt = 2 if is_bump else 3
    out += ch(0x812, struct.pack('<8I', fmt, 0x02000100, 0, 0, 0, 0, res, res))
    out += ch(0x814, struct.pack('<I', 2 if is_bump else 0))
    out += ch(0x815, b'\x00' + struct.pack('<f', 1.0))
    out += ch(0x816, struct.pack('<If', 1, 0.0 if is_bump else 0.7))
    if is_bump:
        out += ch(0x817, struct.pack('<fI', 0.05, 1) + b'\x00')
    else:
        out += ch(0x817, struct.pack('<fI', 0.05, 2) + bump_name.encode('cp1251') + b'\x00')
    out += ch(0x818, b'\x00')
    out += ch(0x819, b'\x00')
    with open(path, 'wb') as f:
        f.write(out)


def main():
    os.makedirs(GAME, exist_ok=True)
    col = np.asarray(Image.open(os.path.join(OUT, 'bake_col.png')).convert('RGB'))
    gl = np.asarray(Image.open(os.path.join(OUT, 'bake_gloss.png')).convert('L')).astype(np.float32) / 255.0
    nrm = cv2.imread(os.path.join(OUT, 'bake_normal.exr'), cv2.IMREAD_UNCHANGED)[..., ::-1][..., :3].astype(np.float32)
    res = col.shape[0]
    # coverage: what the islands hold (the bake's margin already extends them; keep everything not black)
    cov = ((col.max(axis=2) > 2) | (np.abs(nrm - 0.5).sum(axis=2) > 0.02)).astype(np.uint8) * 255
    col8 = push_pull_fill(col.copy(), cov)
    n = nrm * 2.0 - 1.0
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    # X-Ray: Y runs down the image
    n_xr = np.dstack([n[..., 0], -n[..., 1], n[..., 2]])
    hgt = np.full(gl.shape, 0.5, np.float32)
    work = tempfile.mkdtemp(dir=OUT)
    try:
        base = os.path.join(work, f'dar_bag_{NAME}.png')
        save_png(base, col8)
        d = texconv(base, work, 'BC1_UNORM', 0)
        os.replace(d, os.path.join(GAME, f'dar_bag_{NAME}.dds'))
        ob = os.path.join(GAME, f'dar_bag_{NAME}_bump.dds')
        obx = os.path.join(GAME, f'dar_bag_{NAME}_bump#.dds')
        bump.encode(n_xr, gl, hgt, ob, obx)
        write_thm(os.path.join(GAME, f'dar_bag_{NAME}.thm'), res, bump_name=TEX + '_bump')
        write_thm(os.path.join(GAME, f'dar_bag_{NAME}_bump.thm'), res, is_bump=True)
        shutil.copyfile(os.path.join(OUT, f'dar_bag_{NAME}_hud.ogf'), os.path.join(GAME, f'dar_bag_{NAME}_hud.ogf'))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    # only the finished files: an encoder's work folder left by an interrupted run is not content
    files = sorted(f for f in os.listdir(GAME) if os.path.isfile(os.path.join(GAME, f))
                   and f.endswith(('.dds', '.thm', '.ogf')))
    print('GAME files', files)
    # a little gamedata of its own for the previews (the importer finds the textures there)
    gd = os.path.join(OUT, 'gd', 'textures', 'act')
    os.makedirs(gd, exist_ok=True)
    for f in files:
        if f.endswith('.dds'):
            shutil.copyfile(os.path.join(GAME, f), os.path.join(gd, f))
    if len(sys.argv) > 2 and sys.argv[2] == 'install':
        for f in files:
            src = os.path.join(GAME, f)
            rel = OGF_REL if f.endswith('.ogf') else 'textures/act/' + f
            install(src, rel)
            print('installed', rel)


main()
