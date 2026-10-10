# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Orthographic painter's-algorithm renderer for the report sheets (PIL only)."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# view name -> (screen x axis, screen y axis (up), depth axis towards the viewer is negative)
VIEWS = {
    'front': (np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), np.array([0, 0, 1.0])),   # from -Z, X right
    'side': (np.array([0, 0, 1.0]), np.array([0, 1.0, 0]), np.array([-1.0, 0, 0])),   # from +X, Z right
    'top': (np.array([1.0, 0, 0]), np.array([0, 0, 1.0]), np.array([0, -1.0, 0])),    # from +Y, X right, Z up
}
LIGHT = np.array([-0.4, 0.75, -0.55])
LIGHT = LIGHT / np.linalg.norm(LIGHT)


def font(size=12):
    try:
        return ImageFont.truetype('arial.ttf', size)
    except OSError:
        return ImageFont.load_default()


class Canvas:
    """One panel: a model-space box mapped into a w x h pixel frame of a view."""

    def __init__(self, w, h, view, box_min, box_max, bg=(152, 162, 172), margin=10):
        self.w, self.h = w, h
        self.ax, self.ay, self.az = VIEWS[view]
        corners = np.array([[x, y, z] for x in (box_min[0], box_max[0]) for y in (box_min[1], box_max[1])
                            for z in (box_min[2], box_max[2])])
        sx, sy = corners @ self.ax, corners @ self.ay
        self.x0, self.x1, self.y0, self.y1 = sx.min(), sx.max(), sy.min(), sy.max()
        span = max(self.x1 - self.x0, 1e-3) / (w - 2 * margin), max(self.y1 - self.y0, 1e-3) / (h - 2 * margin)
        self.scale = 1.0 / max(span)
        self.cx = (self.x0 + self.x1) / 2
        self.cy = (self.y0 + self.y1) / 2
        self.img = Image.new('RGB', (w, h), bg)
        self.draw = ImageDraw.Draw(self.img)
        self.items = []  # (depth, kind, data)

    def project(self, P):
        P = np.asarray(P, np.float64)
        X = (P @ self.ax - self.cx) * self.scale + self.w / 2
        Y = self.h / 2 - (P @ self.ay - self.cy) * self.scale
        D = P @ self.az
        return X, Y, D

    def tris(self, V, F, colors, shade=True, outline=None):
        """Triangles with per-triangle colours (n, 3) uint8 or one colour; Lambert shading on face normals."""
        if not len(F):
            return
        X, Y, D = self.project(V)
        cols = np.broadcast_to(np.asarray(colors, np.float64), (len(F), 3)).copy()
        if shade:
            n = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]])
            n /= np.maximum(np.linalg.norm(n, axis=1), 1e-12)[:, None]
            # two-sided Lambert with an ambient floor
            lam = np.abs(n @ LIGHT)
            cols = cols * (0.45 + 0.55 * lam)[:, None]
        cols = np.clip(cols, 0, 255).astype(np.uint8)
        depth = D[F].mean(1)
        for i in range(len(F)):
            f = F[i]
            pts = [(X[f[0]], Y[f[0]]), (X[f[1]], Y[f[1]]), (X[f[2]], Y[f[2]])]
            self.items.append((depth[i], 0, (pts, tuple(cols[i]), outline)))

    def lines(self, P, color, width=1, closed=False):
        if len(P) < 2:
            return
        X, Y, D = self.project(P)
        pts = list(zip(X, Y))
        if closed:
            pts.append(pts[0])
        self.items.append((float(D.min()) - 1e3, 1, (pts, tuple(color), width)))

    def points(self, P, color, r=1.5):
        X, Y, D = self.project(P)
        for x, y, d in zip(X, Y, D):
            self.items.append((d - 1e3, 2, ((x, y), tuple(color), r)))

    def flush(self):
        # far to near: depth axis points away from the viewer
        self.items.sort(key=lambda it: -it[0])
        for depth, kind, data in self.items:
            if kind == 0:
                pts, col, outline = data
                self.draw.polygon(pts, fill=col, outline=outline)
            elif kind == 1:
                pts, col, width = data
                self.draw.line(pts, fill=col, width=width)
            else:
                (x, y), col, r = data
                self.draw.ellipse((x - r, y - r, x + r, y + r), fill=col)
        self.items = []
        return self.img

    def label(self, text, xy=(4, 2), color=(255, 255, 255), size=12):
        self.draw.text(xy, text, fill=color, font=font(size))


def heat(v, lo, hi):
    """Blue -> green -> yellow -> red colour ramp of v in [lo, hi]."""
    t = np.clip((np.asarray(v, np.float64) - lo) / max(hi - lo, 1e-12), 0, 1)
    stops = np.array([[40, 70, 200], [40, 190, 90], [235, 220, 50], [220, 40, 30]], np.float64)
    x = t * (len(stops) - 1)
    i = np.minimum(x.astype(int), len(stops) - 2)
    f = (x - i)[..., None]
    return stops[i] * (1 - f) + stops[i + 1] * f


def legend_bar(img, xy, w, h, lo, hi, unit, title):
    d = ImageDraw.Draw(img)
    x0, y0 = xy
    for i in range(w):
        c = heat(lo + (hi - lo) * i / max(w - 1, 1), lo, hi)
        d.line([(x0 + i, y0), (x0 + i, y0 + h)], fill=tuple(int(v) for v in c))
    f = font(11)
    d.text((x0, y0 - 14), title, fill=(255, 255, 255), font=f)
    d.text((x0, y0 + h + 1), f'{lo:g}', fill=(255, 255, 255), font=f)
    d.text((x0 + w - 40, y0 + h + 1), f'{hi:g} {unit}', fill=(255, 255, 255), font=f)
