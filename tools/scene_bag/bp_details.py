"""Detail parts laid on the bag's front (run inside Blender). Shapes are drawn in the front plane (x right,
y up, metres, bag frame) and dropped onto the hull along -Z, so they hug the cloth. Where the hands pass
(the upper front, the sides) details stay flush (under a millimetre); the free zone is the lower front,
x -0.08..0.08, y -0.165..-0.03."""
import math

import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

MM = 0.001


def _drop(bvh, x, y, ztop=0.3):
    hit = bvh.ray_cast(Vector((x, y, ztop)), Vector((0, 0, -1)))
    if hit[0] is None:
        return None, None
    return np.array(hit[0]), np.array(hit[1])


def _smooth_normal(surf, p):
    return surf.normal(p, 0.005)


def slab(bm, surf, bvh, outline, thick, sink=0.5 * MM, res=0.003, mat=0, uvl=None, edge_round=0.6 * MM):
    """A cloth/plastic piece: the 2D outline (list of (x, y), closed, convex enough) filled with a grid,
    dropped on the hull, given `thick` along the local normal; the rim goes `sink` below the surface so
    no gap shows. Returns its faces."""
    xs = [p[0] for p in outline]
    ys = [p[1] for p in outline]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    nx = max(2, int(math.ceil((x1 - x0) / res)))
    ny = max(2, int(math.ceil((y1 - y0) / res)))
    poly = np.array(outline)

    def inside(x, y):
        # even-odd rule
        c = False
        j = len(poly) - 1
        for i in range(len(poly)):
            xi, yi = poly[i]
            xj, yj = poly[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi:
                c = not c
            j = i
        return c

    # grid points inside the outline, and the outline itself as the rim
    top, bot = {}, {}
    faces = []

    def vert_pair(x, y, key):
        p, _ = _drop(bvh, x, y)
        if p is None:
            return None
        n = _smooth_normal(surf, p)
        # distance to the rim: round the edge
        d = _dist_to_outline(poly, x, y)
        t = thick * min(1.0, math.sqrt(max(d, 0.0) / max(edge_round, 1e-6))) if edge_round > 0 else thick
        top[key] = bm.verts.new(p + n * t)
        bot[key] = bm.verts.new(p - n * sink)
        return True

    grid = {}
    for i in range(nx + 1):
        for j in range(ny + 1):
            x = x0 + (x1 - x0) * i / nx
            y = y0 + (y1 - y0) * j / ny
            if inside(x, y) and vert_pair(x, y, (i, j)):
                grid[(i, j)] = True
    for i in range(nx):
        for j in range(ny):
            q = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
            if all(k in grid for k in q):
                f = bm.faces.new([top[k] for k in q])
                f.material_index = mat
                faces.append(f)
    # rim: boundary edges of the top grid get a skirt down to the sunk copy
    edges = {}
    for f in faces:
        for e in f.edges:
            edges[e] = edges.get(e, 0) + 1
    inv_top = {v: k for k, v in top.items()}
    for e, cnt in edges.items():
        if cnt != 1:
            continue
        a, b = e.verts
        ka, kb = inv_top[a], inv_top[b]
        f = bm.faces.new((a, b, bot[kb], bot[ka]))
        f.material_index = mat
        faces.append(f)
    # unused sunk verts
    used = {v for f in faces for v in f.verts}
    for k, v in list(bot.items()):
        if v not in used:
            bm.verts.remove(v)
    bmesh.ops.recalc_face_normals(bm, faces=faces)
    if uvl is not None:
        for f in faces:
            for loop in f.loops:
                loop[uvl].uv = (loop.vert.co.x * 1.0, loop.vert.co.y * 1.0)
    return faces


def _dist_to_outline(poly, x, y):
    best = 1e9
    for i in range(len(poly)):
        a = poly[i]
        b = poly[(i + 1) % len(poly)]
        ab = b - a
        t = np.clip(np.dot([x - a[0], y - a[1]], ab) / max(np.dot(ab, ab), 1e-12), 0, 1)
        q = a + ab * t
        best = min(best, math.hypot(x - q[0], y - q[1]))
    return best


def rounded_rect(cx, cy, w, h, r, seg=4):
    pts = []
    for (sx, sy, a0) in ((1, 1, 0), (-1, 1, 90), (-1, -1, 180), (1, -1, 270)):
        ccx, ccy = cx + sx * (w / 2 - r), cy + sy * (h / 2 - r)
        for k in range(seg + 1):
            a = math.radians(a0 + 90 * k / seg)
            pts.append((ccx + r * math.cos(a), ccy + r * math.sin(a)))
    return pts


def strap(bm, surf, bvh, x0, x1, y, w, thick, mat, uvl=None):
    """A horizontal webbing strap across the front from x0 to x1 at height y, width w."""
    return slab(bm, surf, bvh, rounded_rect((x0 + x1) / 2, y, abs(x1 - x0), w, 0.0015), thick,
                res=0.0025, mat=mat, uvl=uvl, edge_round=0.4 * MM)


def buckle(bm, surf, bvh, cx, cy, w, h, thick, mat):
    """A plastic side-release buckle: a rounded block with a recessed window (drawn as two slabs)."""
    f1 = slab(bm, surf, bvh, rounded_rect(cx, cy, w, h, min(w, h) * 0.22), thick, res=0.002, mat=mat,
              edge_round=1.2 * MM)
    return f1


def cord_zigzag(bm, surf, bvh, x0, x1, y0, y1, n, radius, mat):
    """Bungee cord laced in a zigzag between two rows of anchor points: tubes hugging the cloth."""
    pts = []
    for k in range(n + 1):
        x = x0 + (x1 - x0) * k / n
        y = y1 if k % 2 == 0 else y0
        pts.append((x, y))
    faces = []
    for (ax, ay), (bx, by) in zip(pts[:-1], pts[1:]):
        steps = max(4, int(math.hypot(bx - ax, by - ay) / 0.003))
        rings = []
        for s in range(steps + 1):
            t = s / steps
            x, y = ax + (bx - ax) * t, ay + (by - ay) * t
            p, _ = _drop(bvh, x, y)
            if p is None:
                continue
            n_ = _smooth_normal(surf, p)
            tang = np.array([bx - ax, by - ay, 0.0])
            tang -= n_ * np.dot(tang, n_)
            tang /= max(np.linalg.norm(tang), 1e-9)
            side = np.cross(n_, tang)
            c = p + n_ * radius * 0.85
            ring = [bm.verts.new(c + (side * math.cos(a) + n_ * math.sin(a)) * radius)
                    for a in np.linspace(0, 2 * math.pi, 8, endpoint=False)]
            rings.append(ring)
        for r0, r1 in zip(rings[:-1], rings[1:]):
            for i in range(8):
                f = bm.faces.new((r0[i], r0[(i + 1) % 8], r1[(i + 1) % 8], r1[i]))
                f.material_index = mat
                faces.append(f)
    return faces


def surface_bvh(obj):
    import bpy
    dg = bpy.context.evaluated_depsgraph_get()
    return BVHTree.FromObject(obj, dg)


def frame_slab(bm, surf, origin, t, b, len_t, len_b, thick, r, mat, sink=0.5 * MM, res=0.0015):
    """A rounded rectangle in the surface's tangent plane at origin (t along, b across), laid onto the
    surface by nearest-point projection - for pieces on the sides where a drop along -Z does not reach."""
    origin, t, b = np.array(origin), np.array(t), np.array(b)
    outline = np.array(rounded_rect(0.0, 0.0, len_t, len_b, r))
    nu = max(2, int(math.ceil(len_t / res)))
    nv = max(2, int(math.ceil(len_b / res)))
    top, bot, grid = {}, {}, {}
    for i in range(nu + 1):
        for j in range(nv + 1):
            u = -len_t / 2 + len_t * i / nu
            v = -len_b / 2 + len_b * j / nv
            if not _inside(outline, u, v):
                continue
            p, _, _ = surf.nearest(origin + t * u + b * v)
            n = surf.normal(p, 0.004)
            d = _dist_to_outline(outline, u, v)
            th = thick * min(1.0, math.sqrt(max(d, 0.0) / (0.8 * MM)))
            top[(i, j)] = bm.verts.new(p + n * th)
            bot[(i, j)] = bm.verts.new(p - n * sink)
            grid[(i, j)] = True
    faces = []
    for i in range(nu):
        for j in range(nv):
            q = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
            if all(k in grid for k in q):
                f = bm.faces.new([top[k] for k in q])
                f.material_index = mat
                faces.append(f)
    edges = {}
    for f in faces:
        for e in f.edges:
            edges[e] = edges.get(e, 0) + 1
    inv_top = {v: k for k, v in top.items()}
    for e, cnt in edges.items():
        if cnt == 1:
            a, c = e.verts
            f = bm.faces.new((a, c, bot[inv_top[c]], bot[inv_top[a]]))
            f.material_index = mat
            faces.append(f)
    used = {v for f in faces for v in f.verts}
    for v in list(bot.values()):
        if v not in used:
            bm.verts.remove(v)
    bmesh.ops.recalc_face_normals(bm, faces=faces)
    return faces


def _inside(poly, x, y):
    c = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi:
            c = not c
        j = i
    return c


def stitch_ring_front(bm, BP, surf, bvh, outline, lift, mat, uvl, step=0.002):
    """A stitch ribbon round a front piece, `lift` above the bag (the piece's top), following `outline`
    (already inset) dropped onto the bag."""
    P = np.array(outline + outline[:1])
    d = np.r_[0, np.cumsum(np.hypot(np.diff(P[:, 0]), np.diff(P[:, 1])))]
    s = np.arange(0, d[-1], step)
    xy = np.array([np.interp(s, d, P[:, 0]), np.interp(s, d, P[:, 1])]).T
    pts = []
    for x, y in xy:
        p, _ = _drop(bvh, x, y)
        if p is not None:
            pts.append(p)
    fr = BP.line_frames(np.array(pts), surf, closed=True)
    faces, _ = BP.sweep(bm, fr, BP.ribbon_profile(1.2 * MM, lift), closed=True, uv_layer=uvl)
    for f in faces:
        f.material_index = mat
    return faces


class Stack:
    """Ray casts against the bag and the pieces already built, the first surface from above wins: a strap
    laid over a pocket lies on the pocket."""

    def __init__(self, body_bvh):
        self.trees = [body_bvh]

    def add(self, bm):
        self.trees = self.trees[:1] + [BVHTree.FromBMesh(bm)]

    def ray_cast(self, origin, direction):
        best = (None, None, None, None)
        for t in self.trees:
            hit = t.ray_cast(origin, direction)
            if hit[0] is not None and (best[3] is None or hit[3] < best[3]):
                best = hit
        return best
