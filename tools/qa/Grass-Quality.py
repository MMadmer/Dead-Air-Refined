"""The grass's look in two runs' shots, view by view (docs/dead-air/VEGETATION_GOT.md, "Measuring").

    python -I tools/qa/Grass-Quality.py <run A> <run B> [ground share of the frame, from the bottom; 0.45]

Over the ground band of each pair of shots (NN.jpg, the same view in both runs): the vegetation's cover (pixels whose
green leads red and blue with some saturation), its detail (the variance of the luma's Laplacian, the blades' edges),
the mean luma and the green pixels' mean chroma. Prints each view and the mean change B against A; the wind's sway
alone moves the detail and the cover by a few per cent between two runs of one build.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def stats(path, share):
    rgb = np.asarray(Image.open(path).convert('RGB'), np.float32)
    h = rgb.shape[0]
    g = rgb[int((1.0 - share) * h):]
    r_, g_, b_ = g[..., 0], g[..., 1], g[..., 2]
    luma = 0.299 * r_ + 0.587 * g_ + 0.114 * b_
    mx, mn = g.max(axis=2), g.min(axis=2)
    sat = (mx - mn) / np.maximum(mx, 1.0)
    green = (g_ >= r_) & (g_ > b_) & (sat > 0.12) & (luma > 6.0)
    lap = luma[1:-1, 1:-1] * 4.0 - luma[:-2, 1:-1] - luma[2:, 1:-1] - luma[1:-1, :-2] - luma[1:-1, 2:]
    chroma = (g_ - 0.5 * (r_ + b_))[green].mean() if green.any() else 0.0
    return {'cover': float(green.mean()), 'detail': float(lap.var()), 'luma': float(luma.mean()), 'chroma': float(chroma)}


def main():
    a_dir, b_dir = sys.argv[1], sys.argv[2]
    share = float(sys.argv[3]) if len(sys.argv) > 3 else 0.45
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(a_dir, '*.jpg')))
    names = [n for n in names if os.path.isfile(os.path.join(b_dir, n))]
    keys = ('cover', 'detail', 'luma', 'chroma')
    deltas = {k: [] for k in keys}
    print('%-8s ' % 'view' + ' '.join('%-24s' % k for k in keys))
    for n in names:
        a, b = stats(os.path.join(a_dir, n), share), stats(os.path.join(b_dir, n), share)
        cells = []
        for k in keys:
            d = 100.0 * (b[k] / a[k] - 1.0) if a[k] else 0.0
            deltas[k].append(d)
            cells.append('%8.3f -> %8.3f %+5.1f%%' % (a[k], b[k], d))
        print('%-8s ' % n + ' '.join(cells))
    print('mean     ' + ' '.join('%+23.1f%%' % (sum(v) / len(v)) for v in deltas.values()))


if __name__ == '__main__':
    main()
