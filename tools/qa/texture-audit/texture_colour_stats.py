# Per-texture colour statistics for a tree of DDS files (DXT1/3/5, uncompressed).
# usage: tex_stats.py <root> <out.csv>
import os, sys, csv, struct
import numpy as np
from PIL import Image
from multiprocessing import Pool

ROOT, OUT = sys.argv[1], sys.argv[2]
LUM = np.array([0.2126, 0.7152, 0.0722], np.float32)


def fmt_of(path):
    with open(path, 'rb') as f:
        h = f.read(148)
    if h[:4] != b'DDS ':
        return 'notdds'
    fourcc = h[84:88]
    if fourcc == b'DX10':
        return 'DX10:%d' % struct.unpack_from('<I', h, 128)[0]
    if fourcc.strip(b'\0'):
        return fourcc.decode('latin1')
    return 'rgb%d' % struct.unpack_from('<I', h, 88)[0]


def kind_of(rel):
    low = rel.lower().replace(os.sep, '/')
    base = os.path.basename(low)
    if base.endswith('_bump.dds') or '_bump#' in base:
        return 'bump'
    if low.startswith('ui/') or '/ui/' in low:
        return 'ui'
    return 'diffuse'


def stats(path):
    rel = os.path.relpath(path, ROOT).lower().replace(os.sep, '/')
    if rel.startswith('textures/'):
        rel = rel[len('textures/'):]
    row = dict(path=rel, kind=kind_of(rel), fmt=fmt_of(path))
    try:
        im = Image.open(path)
        w, h = im.size
        row.update(w=w, h=h)
        f = max(1, max(w, h) // 512)
        if f > 1:
            im = im.reduce(f)
        has_alpha = 'A' in im.getbands()
        arr = np.asarray(im.convert('RGBA'), np.float32) / 255.0
        rgb, a = arr[..., :3].reshape(-1, 3), arr[..., 3].reshape(-1)
        mask = a > 0.5 if has_alpha else np.ones(a.shape, bool)
        if mask.sum() < 16:
            mask[:] = True
        px = rgb[mask]
        mx, mn = px.max(1), px.min(1)
        sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)
        val = mx
        lum = px @ LUM
        lit = val > 0.08
        s = sat[lit] if lit.any() else sat
        gdom = (px[:, 1] - np.maximum(px[:, 0], px[:, 2])) / np.maximum(px[:, 1], 1e-4)
        green = (gdom > 0.12) & lit
        rg = (px[:, 0] - px[:, 1]) * 255.0
        yb = (0.5 * (px[:, 0] + px[:, 1]) - px[:, 2]) * 255.0
        colourful = float(np.sqrt(rg.std() ** 2 + yb.std() ** 2) + 0.3 * np.sqrt(rg.mean() ** 2 + yb.mean() ** 2))
        row.update(alpha=int(has_alpha), n=int(len(px)), lum=float(lum.mean()),
                   sat_mean=float(s.mean()), sat_p90=float(np.percentile(s, 90)),
                   vivid=float(((sat > 0.6) & (val > 0.2)).mean()),
                   green_frac=float(green.mean()),
                   green_sat=float(sat[green].mean()) if green.any() else 0.0,
                   green_val=float(val[green].mean()) if green.any() else 0.0,
                   green_chroma=float((mx - mn)[green].mean()) if green.any() else 0.0,
                   colourful=colourful, err='')
    except Exception as e:
        row.update(err=repr(e)[:80])
    return row


if __name__ == '__main__':
    files = [os.path.join(dp, f) for dp, _, fs in os.walk(ROOT) for f in fs if f.lower().endswith('.dds')]
    files.sort()
    cols = ['path', 'kind', 'fmt', 'w', 'h', 'alpha', 'n', 'lum', 'sat_mean', 'sat_p90', 'vivid', 'green_frac',
            'green_sat', 'green_val', 'green_chroma', 'colourful', 'err']
    with Pool(8) as pool, open(OUT, 'w', newline='') as out:
        wr = csv.DictWriter(out, fieldnames=cols, extrasaction='ignore')
        wr.writeheader()
        for i, row in enumerate(pool.imap_unordered(stats, files, chunksize=4)):
            wr.writerow({c: row.get(c, '') for c in cols})
            if i % 500 == 0:
                print(i, len(files), flush=True)
    print('done', len(files))
