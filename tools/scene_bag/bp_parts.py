"""Geometry of our scene bags (run inside Blender): the zipper laid along the slider's own centreline on the
hull, the end stops, and the pull in the slider bone's frame (same footprint as the original's: 8 -> 12 mm
wide, 22.5 mm long, 3 mm thick, base at the bone origin), plus small helpers for the detail parts."""
import math
import os

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
MM = 0.001


# ---------------------------------------------------------------------------------------------- frames
class Surface:
    """Nearest point and a smoothed normal on a mesh object (world = object space here)."""

    def __init__(self, obj):
        dg = bpy.context.evaluated_depsgraph_get()
        self.bvh = BVHTree.FromObject(obj, dg)
        me = obj.data
        self.vn = np.array([v.normal[:] for v in me.vertices])
        self.vco = np.array([v.co[:] for v in me.vertices])

    def nearest(self, p):
        loc, nrm, _, d = self.bvh.find_nearest(Vector(p))
        return np.array(loc), np.array(nrm), d

    def normal(self, p, radius=0.006):
        # vertex normals averaged around p: the gauge is faceted, the cloth is not
        d = np.linalg.norm(self.vco - p, axis=1)
        w = np.exp(-(d / radius) ** 2)
        if w.sum() < 1e-6:
            return self.nearest(p)[1]
        n = (self.vn * w[:, None]).sum(0)
        return n / np.linalg.norm(n)


def zip_frames(surf, ext_before=0.012, ext_after=0.014, step=0.0005):
    """Samples along the zipper: (s, point on the hull, tangent, normal, binormal). s = 0 at the slider's
    rest, s_open at the open end; the ends are carried on over the hull by ext_before / ext_after."""
    z = np.load(os.path.join(HERE, 'zip_curve.npz'))
    P, S = z['p'], z['s']
    t0 = P[1] - P[0]
    t0 /= np.linalg.norm(t0)
    t1 = P[-1] - P[-2]
    t1 /= np.linalg.norm(t1)
    pre = [P[0] - t0 * d for d in np.arange(ext_before, 0, -step)]
    post = [P[-1] + t1 * d for d in np.arange(step, ext_after + 1e-9, step)]
    raw = np.vstack([pre, P, post]) if pre else np.vstack([P, post])
    s = np.r_[-np.arange(ext_before, 0, -step), S, S[-1] + np.arange(step, ext_after + 1e-9, step)]
    pts, nrm = [], []
    for p in raw:
        q, _, _ = surf.nearest(p)
        pts.append(q)
        nrm.append(surf.normal(q))
    pts, nrm = np.array(pts), np.array(nrm)
    # smooth the normals along the line
    for _ in range(30):
        nrm[1:-1] = 0.5 * nrm[1:-1] + 0.25 * (nrm[:-2] + nrm[2:])
        nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
    tan = np.gradient(pts, axis=0)
    tan -= nrm * (tan * nrm).sum(1, keepdims=True)
    tan /= np.linalg.norm(tan, axis=1, keepdims=True)
    bin_ = np.cross(nrm, tan)
    return s, pts, tan, nrm, bin_, float(z['s_open'])


# ---------------------------------------------------------------------------------------------- sweeps
def sweep(bm, frames, profile, closed=False, uv_layer=None, v_scale=1.0, lift=0.0, every=1, surf=None, taper=0.0):
    """Sweeps a cross-section (list of (b, n) in metres: across, out of the surface) along the frames.
    With surf, every section point is laid on the surface itself (projected, then raised by n along the
    local normal), so a strip across a crease follows it. UV: u = arc length (m) * v_scale, v = profile
    parameter 0..1 (pattern UVs, not the atlas)."""
    s, pts, tan, nrm, bin_ = frames[:5]
    rows = []
    for i in range(0, len(s), every):
        row = []
        # an open run can run out to nothing at its ends instead of stopping as a cut tube
        f = 1.0 if not taper or closed else max(0.04, min(1.0, (s[i] - s[0]) / taper, (s[-1] - s[i]) / taper))
        for b, n in profile:
            b, n = b * f, n * f
            p = pts[i] + bin_[i] * b
            if surf is not None:
                q, _, _ = surf.nearest(p)
                co = q + surf.normal(q, 0.004) * (n + lift)
            else:
                co = p + nrm[i] * (n + lift)
            row.append(bm.verts.new(co))
        rows.append((i, row))
    plen = np.r_[0, np.cumsum([math.dist(profile[k], profile[k + 1]) for k in range(len(profile) - 1)])]
    plen = plen / max(plen[-1], 1e-9)
    faces = []
    pairs = list(zip(rows[:-1], rows[1:]))
    if closed:
        pairs.append((rows[-1], rows[0]))
    for (i0, r0), (i1, r1) in pairs:
        # a closed loop's last span runs to the loop's full length, not back to zero
        u1 = s[i1] if i1 > i0 else s[i0] + math.dist(pts[i0], pts[i1])
        for k in range(len(profile) - 1):
            f = bm.faces.new((r0[k], r1[k], r1[k + 1], r0[k + 1]))
            faces.append(f)
            if uv_layer is not None:
                for loop, (u, kk) in zip(f.loops, ((s[i0], k), (u1, k), (u1, k + 1), (s[i0], k + 1))):
                    loop[uv_layer].uv = (u * v_scale, plen[kk])
    return faces, rows


def zipper_profile(coil_w=5.5, tape_w=3.0, lip_w=2.6, coil_h=1.05, tape_h=0.35, lip_h=1.2, sink=0.4):
    """Cross-section of the zipper, mm in, metres out: sunk lip edge, rounded lip, flat tape, round coil."""
    h = coil_w / 2
    lip = [(-(h + tape_w + lip_w), -sink), (-(h + tape_w + lip_w * 0.72), lip_h * 0.8),
           (-(h + tape_w + lip_w * 0.3), lip_h), (-(h + tape_w), tape_h + 0.25)]
    tape = [(-(h + 0.2), tape_h)]
    coil = [(-h * 0.85, tape_h + (coil_h - tape_h) * 0.55), (-h * 0.4, coil_h * 0.95), (0.0, coil_h),
            (h * 0.4, coil_h * 0.95), (h * 0.85, tape_h + (coil_h - tape_h) * 0.55)]
    pts = lip + tape + coil + [(-b, n) for b, n in tape[::-1]] + [(-b, n) for b, n in lip[::-1]]
    return [(b * MM, n * MM) for b, n in pts]


def box(bm, center, x, y, z, size, round_=0.0):
    """Axis box in the frame (x, y, z unit vectors) at center, size (sx, sy, sz) metres."""
    sx, sy, sz = (v / 2 for v in size)
    vs = []
    for dz in (-1, 1):
        for dy in (-1, 1):
            for dx in (-1, 1):
                vs.append(bm.verts.new(Vector(center) + Vector(x) * sx * dx + Vector(y) * sy * dy + Vector(z) * sz * dz))
    f = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    faces = [bm.faces.new([vs[i] for i in q]) for q in f]
    if round_ > 0:
        bmesh.ops.bevel(bm, geom=list({e for fc in faces for e in fc.edges}), offset=round_, segments=2,
                        affect='EDGES', profile=0.5)
    return faces


# ---------------------------------------------------------------------------------------------- the pull
def pull_mesh(style):
    """The pull in the zipper bone's X-Ray frame (x across, y thickness, z along the tab; base at origin),
    kept inside the original's envelope: |x| <= 4 + 2 * z / 22.5 mm, |y| <= 1.5 mm, 0 <= z <= 22.5 mm."""
    bm = bmesh.new()
    if style == 'cord':
        # paracord loop through the car, a heat-shrunk tail
        for a in np.linspace(0, math.pi, 1):
            pass
        ring = bmesh.ops.create_circle(bm, cap_ends=False, radius=2.6 * MM, segments=16)['verts']
        bmesh.ops.rotate(bm, verts=ring, cent=(0, 0, 0), matrix=Matrix.Rotation(math.pi / 2, 3, 'Y'))
        bmesh.ops.translate(bm, verts=ring, vec=(0, 0, 2.9 * MM))
        _tube(bm, z0=4.5 * MM, z1=22.0 * MM, r0=1.45 * MM, r1=1.45 * MM, seg=12)
    elif style in ('metal', 'cheap', 'rubber', 'leather'):
        th = {'metal': 1.1, 'cheap': 0.9, 'rubber': 2.6, 'leather': 2.2}[style] * MM
        z1 = {'metal': 21.0, 'cheap': 17.5, 'rubber': 22.2, 'leather': 22.2}[style] * MM
        w0 = {'metal': 7.2, 'cheap': 6.4, 'rubber': 7.6, 'leather': 7.8}[style] * MM
        w1 = {'metal': 10.6, 'cheap': 8.6, 'rubber': 11.4, 'leather': 11.6}[style] * MM
        _plate(bm, z0=1.2 * MM, z1=z1, w0=w0, w1=w1, th=th, slot=(style in ('metal', 'cheap')))
    # the car's hinge barrel at the base
    n0 = len(bm.faces)
    car = bmesh.ops.create_cone(bm, cap_ends=True, segments=12, radius1=1.35 * MM, radius2=1.35 * MM,
                                depth=6.6 * MM)['verts']
    bmesh.ops.rotate(bm, verts=car, cent=(0, 0, 0), matrix=Matrix.Rotation(math.pi / 2, 3, 'Y'))
    bmesh.ops.translate(bm, verts=car, vec=(0, 0, 1.3 * MM))
    global CAR_FACES
    CAR_FACES = len(bm.faces) - n0
    return bm


CAR_FACES = 0


def _tube(bm, z0, z1, r0, r1, seg):
    if seg <= 0:
        return
    v = bmesh.ops.create_cone(bm, cap_ends=True, segments=seg, radius1=r0, radius2=r1, depth=z1 - z0)['verts']
    bmesh.ops.translate(bm, verts=v, vec=(0, 0, (z0 + z1) / 2))


def _plate(bm, z0, z1, w0, w1, th, slot):
    n = 10
    outline = []
    for k in range(n + 1):
        z = z0 + (z1 - z0) * k / n
        outline.append((-(w0 + (w1 - w0) * k / n) / 2, z))
    # rounded far end
    for a in np.linspace(math.pi, 0, 9)[1:-1]:
        outline.append((math.cos(a) * w1 / 2, z1 + math.sin(a) * min(w1 / 2, 0.0) ))
    for k in range(n, -1, -1):
        z = z0 + (z1 - z0) * k / n
        outline.append(((w0 + (w1 - w0) * k / n) / 2, z))
    top = [bm.verts.new((x, th / 2, z)) for x, z in outline]
    bot = [bm.verts.new((x, -th / 2, z)) for x, z in outline]
    bm.faces.new(top)
    bm.faces.new(bot[::-1])
    for i in range(len(outline)):
        j = (i + 1) % len(outline)
        bm.faces.new((top[i], bot[i], bot[j], top[j]))


def line_frames(P, surf, closed=False):
    """Frames along any polyline on the surface: (s, points on the surface, tangent, normal, binormal)."""
    pts, nrm = [], []
    for p in P:
        q, _, _ = surf.nearest(p)
        pts.append(q)
        nrm.append(surf.normal(q))
    pts, nrm = np.array(pts), np.array(nrm)
    for _ in range(20):
        if closed:
            nrm = 0.5 * nrm + 0.25 * (np.roll(nrm, 1, 0) + np.roll(nrm, -1, 0))
        else:
            nrm[1:-1] = 0.5 * nrm[1:-1] + 0.25 * (nrm[:-2] + nrm[2:])
        nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
    if closed:
        tan = np.roll(pts, -1, 0) - np.roll(pts, 1, 0)
    else:
        tan = np.gradient(pts, axis=0)
    tan -= nrm * (tan * nrm).sum(1, keepdims=True)
    tan /= np.linalg.norm(tan, axis=1, keepdims=True)
    bin_ = np.cross(nrm, tan)
    s = np.r_[0, np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))]
    return s, pts, tan, nrm, bin_


def tube_profile(r, n=8, sink=0.25):
    """Round piping lying on the surface: centre raised so the tube shows 1 - sink of its height."""
    c = r * (1 - 2 * sink)
    return [(math.cos(a) * r, c + math.sin(a) * r) for a in np.linspace(-math.pi * 0.15, math.pi * 1.15, n)]


def ribbon_profile(w, lift):
    return [(-w / 2, lift), (0.0, lift), (w / 2, lift)]


def offset_frames(fr, off):
    """The same frames moved `off` metres sideways (along the binormal) - re-projected by sweep(surf=...)."""
    s, pts, tan, nrm, bin_ = fr[:5]
    return (s, pts + bin_ * off, tan, nrm, bin_)
