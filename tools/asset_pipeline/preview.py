"""Side-by-side preview: Dead Air | ours | borrowed, on a mid-grey ground."""
import os, sys
import numpy as np
from PIL import Image, ImageDraw
from common import BORROWED, WORK, orig_path

def comp(p, size):
    im = Image.open(p).convert('RGBA').resize(size, Image.LANCZOS)
    bg = Image.new('RGBA', size, (118, 118, 118, 255)); bg.alpha_composite(im); return bg.convert('RGB')

def sheet(names, out, h=520, ours_dir=os.path.join(WORK, 'trees', 'png')):
    rows = []
    for n in names:
        rel = f'textures/trees/{n}.dds'
        w0, h0 = Image.open(os.path.join(BORROWED, rel)).size
        size = (int(h * w0 / h0), h)
        tiles = [comp(orig_path(rel), size), comp(os.path.join(ours_dir, n + '.png'), size), comp(os.path.join(BORROWED, rel), size)]
        row = Image.new('RGB', (size[0] * 3 + 20, h + 18), (60, 60, 60))
        for i, t in enumerate(tiles): row.paste(t, (i * (size[0] + 10), 18))
        ImageDraw.Draw(row).text((4, 2), n + '   [DA | ours | AN4]', fill=(255, 255, 255))
        rows.append(row)
    W = max(r.size[0] for r in rows); H = sum(r.size[1] for r in rows)
    s = Image.new('RGB', (W, H), (60, 60, 60)); y = 0
    for r in rows: s.paste(r, (0, y)); y += r.size[1]
    s.save(out)

if __name__ == '__main__':
    sheet(sys.argv[2:], sys.argv[1])
