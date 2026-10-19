"""Run the upscale on a handful of textures and lay out crops: source (nearest) | ours | borrowed, at 1:1 of the target."""
import json
import os
import shutil
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

import upscale
from common import BORROWED, WORK, load_rgba, orig_path, save_png

SAMPLES = sys.argv[1:] or [
    'textures/crete/crete_beton_3.dds', 'textures/crete/crete_podval.dds', 'textures/briks/briks_br2.dds',
    'textures/mtl/mtl_angar_a.dds', 'textures/grnd/grnd_obriv_01.dds', 'textures/wood/wood_board_01.dds',
    'textures/wall/wall_stena_01.dds', 'textures/wind/wind_01.dds', 'textures/veh/veh_nivawheels1a.dds',
    'textures/prop/prop_elektro_2.dds', 'textures/ston/ston_beton_ch_01.dds', 'textures/tile/tile_green.dds',
]
D = os.path.join(WORK, 'samples')
shutil.rmtree(D, ignore_errors=True)
os.makedirs(os.path.join(D, 'in'))
meta = {}
for rel in SAMPLES:
    src = load_rgba(orig_path(rel))
    bor = Image.open(os.path.join(BORROWED, rel))
    w, h = upscale.plan_size(src.shape[1], src.shape[0], *bor.size)
    key = rel.replace('/', '__')[:-4]
    upscale.prepare(src, os.path.join(D, 'in', key + '.png'))
    meta[key] = (rel, w, h)
t = upscale.run_ai(os.path.join(D, 'in'), os.path.join(D, 'ai'))
print(f'AI: {t:.1f}s for {len(SAMPLES)}')
rows = []
for key, (rel, w, h) in meta.items():
    src = load_rgba(orig_path(rel))
    out, kind = upscale.finish(src, os.path.join(D, 'ai', key + '.png'), w, h)
    save_png(os.path.join(D, 'out', key + '.png'), out)
    bor = np.asarray(Image.open(os.path.join(BORROWED, rel)).convert('RGBA').resize((w, h), Image.LANCZOS))
    near = np.asarray(Image.fromarray(src).resize((w, h), Image.NEAREST))
    # 512x512 crop from the middle of each, side by side
    cw = min(512, w, h)
    y0, x0 = (h - cw) // 2, (w - cw) // 2
    tiles = [x[y0:y0 + cw, x0:x0 + cw, :3] for x in (near, out, bor)]
    rows.append((rel, f'{src.shape[1]}x{src.shape[0]} -> {w}x{h} alpha={kind}', np.hstack(tiles)))
    print(rel, src.shape[1], '->', w, kind)
H = sum(r[2].shape[0] + 20 for r in rows)
W = max(r[2].shape[1] for r in rows)
sheet = Image.new('RGB', (W, H), (40, 40, 40))
d = ImageDraw.Draw(sheet)
y = 0
for rel, label, img in rows:
    d.text((4, y + 4), f'{rel}  {label}   [DA nearest | ours | borrowed]', fill=(255, 255, 255))
    sheet.paste(Image.fromarray(img), (0, y + 20))
    y += img.shape[0] + 20
sheet.save(os.path.join(D, 'sheet.png'))
