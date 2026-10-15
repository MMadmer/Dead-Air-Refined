# The Grizzly emblem of the engine's splash: a grizzly's head in neon lines in the manner of Perm's bear (straight
# strokes of one width, angular), the name under it. Lines only on black - no fills, no facets inside the head: the
# silhouette and what a grizzly is read by (the ears, the brow, the eyes, the long bridge, the nose, the mouth).
#
# The design lives here as data; the engine draws it with a pixel shader at the splash window's own resolution
# (src/xrEngine/grizzly_splash.cpp). This writes the table the engine compiles in and, with --preview, a PNG of
# the finished frame drawn by the same formula.
#
#   python tools\branding\grizzly_emblem.py [--preview out.png [width height]]
#
# Frame: 1 = half the window's height, y up, the origin in the middle of the window.
import math
import os
import sys

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
TABLE = os.path.join(ROOT, 'src', 'xrEngine', 'grizzly_emblem.inl')

# The whole emblem lowered by this much: the head is taller than the name, the pair sits in the middle.
LIFT = -0.10

# The right half of the head (mirrored to the left). A grizzly, not a bear in general: small round ears set far
# apart, a broad forehead stepping down to a long wide bridge, small deep eyes by the bridge, a big nose, the
# cheeks' fur widest below the eyes.
OUTLINE = [(0.0, 0.66), (0.19, 0.69), (0.30, 0.70), (0.31, 0.79), (0.37, 0.865), (0.45, 0.87), (0.51, 0.80),
           (0.50, 0.67), (0.60, 0.55), (0.69, 0.33), (0.66, 0.14), (0.56, -0.03), (0.40, -0.15), (0.20, -0.25),
           (0.0, -0.27)]
LINES = [
    [(0.34, 0.73), (0.38, 0.81), (0.45, 0.815), (0.475, 0.72)],              # the ear's inside
    [(0.13, 0.505), (0.29, 0.535)],                                          # the heavy brow
    [(0.14, 0.445), (0.27, 0.47), (0.22, 0.41), (0.14, 0.445)],             # the eye, slanted down to the bridge
    [(0.10, 0.43), (0.17, 0.19), (0.20, 0.05), (0.12, -0.10), (0.0, -0.13)], # the bridge and the muzzle
    [(0.0, 0.05), (0.0, -0.03), (0.07, -0.065)],                             # the mouth
]
# The nose: its outline, whole (it crosses the middle).
NOSE = [(-0.12, 0.18), (0.12, 0.18), (0.10, 0.10), (0.03, 0.05), (-0.03, 0.05), (-0.10, 0.10), (-0.12, 0.18)]

# The name: monoline capitals with cut corners.
TEXT, TEXT_Y, TEXT_W, TEXT_H, TEXT_GAP = "GRIZZLY", -0.60, 0.115, 0.15, 0.055


def glyph(ch, w, h):
    c = 0.22 * w
    if ch == "G":
        return [[(w, h - c), (w - c, h), (c, h), (0, h - c), (0, c), (c, 0), (w - c, 0), (w, c), (w, h * 0.48),
                 (w * 0.52, h * 0.48)]]
    if ch == "R":
        return [[(0, 0), (0, h), (w - c, h), (w, h - c), (w, h * 0.62), (w - c, h * 0.46), (0, h * 0.46)],
                [(w * 0.5, h * 0.46), (w, 0)]]
    if ch == "I":
        return [[(0, 0), (0, h)]]
    if ch == "Z":
        return [[(0, h), (w, h), (0, 0), (w, 0)]]
    if ch == "L":
        return [[(0, h), (0, 0), (w, 0)]]
    if ch == "Y":
        return [[(0, h), (w * 0.5, h * 0.5), (w, h)], [(w * 0.5, h * 0.5), (w * 0.5, 0)]]
    raise KeyError(ch)


# How the neon lights up: the head from the nose outward, then the name from left to right; a stroke is traced in
# STROKE seconds.
STROKE = 0.16
HEAD_FROM, HEAD_SPAN = 0.10, 0.85
TEXT_FROM, TEXT_SPAN = 0.95, 0.40
NOSE_AT = (0.0, 0.12)


def strokes():
    out = []

    def poly(pts, kind):
        for a, b in zip(pts, pts[1:]):
            out.append((a, b, kind))

    poly(OUTLINE, 'head')
    poly([(-x, y) for x, y in OUTLINE], 'head')
    for line in LINES:
        poly(line, 'head')
        if any(x != 0.0 for x, _ in line):
            poly([(-x, y) for x, y in line], 'head')
    poly(NOSE, 'head')
    widths = [0.0 if ch == "I" else TEXT_W for ch in TEXT]
    x = -(sum(widths) + TEXT_GAP * (len(TEXT) - 1)) / 2
    for ch, w in zip(TEXT, widths):
        for line in glyph(ch, w, TEXT_H):
            poly([(x + px, TEXT_Y + py) for px, py in line], 'text')
        x += w + TEXT_GAP

    # (a stroke on the middle line listed twice by the mirror: once)
    seen, unique = set(), []
    for a, b, kind in out:
        key = tuple(sorted((a, b)))
        if key not in seen:
            seen.add(key)
            unique.append((a, b, kind))

    far = max(math.dist(NOSE_AT, ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)) for a, b, k in unique if k == 'head')
    text_x = [p[0] for a, b, k in unique if k == 'text' for p in (a, b)]
    x0, x1 = min(text_x), max(text_x)
    table = []
    for a, b, kind in unique:
        if kind == 'head':
            # traced outward: from the end nearer the nose
            if math.dist(NOSE_AT, b) < math.dist(NOSE_AT, a):
                a, b = b, a
            mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
            start = HEAD_FROM + HEAD_SPAN * math.dist(NOSE_AT, mid) / far
        else:
            if b[0] < a[0]:
                a, b = b, a
            start = TEXT_FROM + TEXT_SPAN * ((a[0] + b[0]) / 2 - x0) / (x1 - x0)
        table.append((a[0], a[1] + LIFT, b[0], b[1] + LIFT, start))
    return table


def write_table(table):
    lines = ['// Generated by tools/branding/grizzly_emblem.py: the design is edited there, not here.',
             '// x0, y0, x1, y1 (1 = half the window\'s height, y up, the origin in the middle), the second it lights up.',
             'constexpr GrizzlyStroke grizzly_strokes[] = {']
    for x0, y0, x1, y1, s in table:
        lines.append('    { %.4ff, %.4ff, %.4ff, %.4ff, %.4ff },' % (x0, y0, x1, y1, s))
    lines.append('};')
    lines.append('constexpr float grizzly_stroke_seconds = %.4ff;' % STROKE)
    with open(TABLE, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', os.path.normpath(TABLE), len(table), 'strokes')


def preview(table, path, W, H):
    # the finished frame by the shader's formula (grizzly_splash.cpp)
    import numpy as np
    from PIL import Image
    unit = H / 2.0
    ys, xs = np.mgrid[0:H, 0:W]
    px = (xs + 0.5 - W / 2) / unit
    py = (H / 2 - ys - 0.5) / unit
    d = np.full((H, W), 1e9)
    for x0, y0, x1, y1, _ in table:
        bx, by = x1 - x0, y1 - y0
        ax, ay = px - x0, py - y0
        h = np.clip((ax * bx + ay * by) / (bx * bx + by * by), 0, 1)
        d = np.minimum(d, np.hypot(ax - bx * h, ay - by * h))
    w, aa = 0.0072, 1.0 / unit
    core = np.clip((w + aa - d) / (2 * aa), 0, 1)
    glow = 0.60 * np.exp(-d / 0.011) + 0.24 * np.exp(-d / 0.05)
    col = core[..., None] * np.array([0.93, 0.80, 1.0]) + glow[..., None] * np.array([0.55, 0.16, 1.0])
    col = 1 - np.exp(-col * 1.5)
    Image.fromarray((np.clip(col, 0, 1) * 255).astype(np.uint8)).save(path)
    print('wrote', path)


if __name__ == '__main__':
    table = strokes()
    write_table(table)
    if '--preview' in sys.argv:
        i = sys.argv.index('--preview')
        args = sys.argv[i + 1:]
        W, H = (int(args[1]), int(args[2])) if len(args) >= 3 else (1280, 800)
        preview(table, args[0], W, H)
