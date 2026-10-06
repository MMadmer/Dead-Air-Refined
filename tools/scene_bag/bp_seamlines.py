"""Seam lines of the bag, found on the hull (run inside Blender, bag frame): the front and back panel rims
(where the smoothed normal turns from the face to the side), the ledge under the front pocket and the
pocket's sides from the zipper ends down to the ledge (largest drop of the surface).
    seam_lines(hull_obj) -> dict name -> (n,3) polyline, 2 mm spacing"""
import math
import os

import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))


def _resample(P, step=0.002, closed=False):
    if closed:
        P = np.vstack([P, P[:1]])
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))]
    s = np.arange(0, d[-1], step)
    return np.array([np.interp(s, d, P[:, k]) for k in range(3)]).T


def _smooth(P, it=20, closed=False):
    P = P.copy()
    for _ in range(it):
        if closed:
            P = 0.5 * P + 0.25 * (np.roll(P, 1, 0) + np.roll(P, -1, 0))
        else:
            P[1:-1] = 0.5 * P[1:-1] + 0.25 * (P[:-2] + P[2:])
    return P


def iso_loop(obj, value, axis=2, smooth_normals=6):
    """The longest closed iso-line of the vertex normal's `axis` component."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.normal_update()
    nrm = {v: Vector(v.normal) for v in bm.verts}
    for _ in range(smooth_normals):
        nn = {}
        for v in bm.verts:
            acc = Vector(nrm[v])
            for e in v.link_edges:
                acc += nrm[e.other_vert(v)]
            nn[v] = acc.normalized()
        nrm = nn
    f = {v: nrm[v][axis] - value for v in bm.verts}
    bmesh.ops.triangulate(bm, faces=bm.faces)
    # segments per triangle
    def cut(e):
        a, b = e.verts
        t = f[a] / (f[a] - f[b])
        return e.index, a.co.lerp(b.co, t)
    bm.edges.index_update()
    segs = []
    for face in bm.faces:
        hits = [cut(e) for e in face.edges if (f[e.verts[0]] > 0) != (f[e.verts[1]] > 0)]
        if len(hits) == 2:
            segs.append(hits)
    # chain by shared edge index
    by_edge = {}
    for i, (h0, h1) in enumerate(segs):
        by_edge.setdefault(h0[0], []).append(i)
        by_edge.setdefault(h1[0], []).append(i)
    used = set()
    loops = []
    for i in range(len(segs)):
        if i in used:
            continue
        used.add(i)
        h0, h1 = segs[i]
        pts = [h0[1], h1[1]]
        cur = h1[0]
        while True:
            nxt = [j for j in by_edge.get(cur, []) if j not in used]
            if not nxt:
                break
            j = nxt[0]
            used.add(j)
            a, b = segs[j]
            if a[0] == cur:
                pts.append(b[1])
                cur = b[0]
            else:
                pts.append(a[1])
                cur = a[0]
        loops.append(np.array([p[:] for p in pts]))
    bm.free()
    loops.sort(key=len, reverse=True)
    return loops[0] if loops else None


def front_height(bvh, x, y):
    hit = bvh.ray_cast(Vector((x, y, 0.4)), Vector((0, 0, -1)))
    return hit[0].z if hit[0] is not None else None


def ledge_line(bvh, x0=-0.095, x1=0.095, y0=-0.125, y1=-0.065, step=0.004):
    pts = []
    for x in np.arange(x0, x1 + 1e-9, step):
        ys = np.arange(y0, y1, 0.0008)
        zs = [front_height(bvh, x, y) for y in ys]
        zs = np.array([z if z is not None else np.nan for z in zs])
        g = np.gradient(zs, ys)
        k = int(np.nanargmax(g))
        pts.append((x, ys[k], zs[k]))
    return np.array(pts)


def pocket_side(bvh, side, y_top, y_bottom, step=0.004):
    """Largest |dz/dx| on one side of the front (side -1 left, +1 right) for each y."""
    pts = []
    for y in np.arange(y_bottom, y_top + 1e-9, step):
        xs = np.arange(0.07, 0.13, 0.0008) * side
        zs = np.array([front_height(bvh, x, y) or np.nan for x in xs])
        g = np.abs(np.gradient(zs, xs))
        k = int(np.nanargmax(g))
        pts.append((xs[k], y, zs[k]))
    return np.array(pts)


def seam_lines(hull_obj, zip_frames):
    import bpy
    dg = bpy.context.evaluated_depsgraph_get()
    bvh = BVHTree.FromObject(hull_obj, dg)
    out = {}
    fr = iso_loop(hull_obj, 0.45)
    if fr is not None:
        out['rim_front'] = _resample(_smooth(_resample(fr, 0.002, True), 25, True), 0.002, True)
    bk = iso_loop(hull_obj, -0.45)
    if bk is not None:
        out['rim_back'] = _resample(_smooth(_resample(bk, 0.002, True), 25, True), 0.002, True)
    led = ledge_line(bvh)
    out['ledge'] = _resample(_smooth(led, 8), 0.002)
    s, pts = zip_frames[0], zip_frames[1]
    for name, end, side in (('side_l', pts[0], -1), ('side_r', pts[-1], 1)):
        y_end = float(end[1])
        ps = pocket_side(bvh, side, y_end, float(np.interp(end[0], led[:, 0], led[:, 1])))
        out[name] = _resample(_smooth(ps, 6), 0.002)
    return out
