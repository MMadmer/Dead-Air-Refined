"""Our hull for the scene bag: the original body is only a shape gauge. A quad box cage (our topology,
about STEP spacing) is pulled onto the gauge, relaxed and pulled again, so the outside matches the gauge
within a millimetre where the hands touch. Saves hull.blend (hull_ref = gauge, hull = ours) and prints the
two-way deviation.

    blender -b bp_orig.blend --python bp_hull.py -- [step_mm]"""
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
STEP = (float(args[0]) if args else 6.0) / 1000.0

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
arm.data.pose_position = 'REST'
src = next(o for o in bpy.data.objects if o.type == 'MESH')

# gauge: the body part only, in rest pose
ref = src.copy()
ref.data = src.data.copy()
ref.name = 'hull_ref'
bpy.context.scene.collection.objects.link(ref)
for m in list(ref.modifiers):
    ref.modifiers.remove(m)
ref.parent = None
grp = {g.index: g.name for g in ref.vertex_groups}
bm = bmesh.new()
bm.from_mesh(ref.data)
dl = bm.verts.layers.deform.active
kill = [v for v in bm.verts if dl and v[dl] and grp[max(v[dl].items(), key=lambda kv: kv[1])[0]] == 'zipper']
bmesh.ops.delete(bm, geom=kill, context='VERTS')
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
print('HULL gauge verts', len(bm.verts), 'faces', len(bm.faces),
      'non-manifold edges', sum(1 for e in bm.edges if not e.is_manifold))
bm.to_mesh(ref.data)
bm.free()
ref.vertex_groups.clear()
co = np.array([v.co[:] for v in ref.data.vertices])
lo, hi = co.min(0), co.max(0)
print('HULL bbox', np.round(lo, 4), np.round(hi, 4))

# box cage, one grid per face, borders welded
bm = bmesh.new()
pad = 0.004
lo2, hi2 = lo - pad, hi + pad
n = np.maximum(2, np.round((hi2 - lo2) / STEP).astype(int))
for axis in range(3):
    u, v = [a for a in range(3) if a != axis]
    for side in (lo2[axis], hi2[axis]):
        grid = []
        for i in range(n[u] + 1):
            row = []
            for j in range(n[v] + 1):
                p = [0.0, 0.0, 0.0]
                p[axis] = side
                p[u] = lo2[u] + (hi2[u] - lo2[u]) * i / n[u]
                p[v] = lo2[v] + (hi2[v] - lo2[v]) * j / n[v]
                row.append(bm.verts.new(p))
            grid.append(row)
        flip = (side == hi2[axis]) == (axis == 1)
        for i in range(n[u]):
            for j in range(n[v]):
                q = [grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]]
                bm.faces.new(q[::-1] if flip else q)
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
me = bpy.data.meshes.new('hull')
bm.to_mesh(me)
bm.free()
hull = bpy.data.objects.new('hull', me)
bpy.context.scene.collection.objects.link(hull)
print('HULL cage faces', len(me.polygons))

dg = bpy.context.evaluated_depsgraph_get()
tr = BVHTree.FromObject(ref, dg)


def project(bm):
    for v in bm.verts:
        hit = tr.find_nearest(v.co)
        if hit[0] is not None:
            v.co = hit[0]


bm = bmesh.new()
bm.from_mesh(me)
project(bm)
for it in range(12):
    # relax toward the neighbours' mean, then back onto the gauge
    new = {}
    for v in bm.verts:
        nb = [e.other_vert(v).co for e in v.link_edges]
        new[v] = sum(nb, Vector()) / len(nb)
    for v, p in new.items():
        v.co = v.co.lerp(p, 0.5)
    project(bm)
bm.to_mesh(me)
bm.free()
for p in me.polygons:
    p.use_smooth = True

dg = bpy.context.evaluated_depsgraph_get()
th = BVHTree.FromObject(hull, dg)
d1 = [tr.find_nearest(v.co)[3] for v in me.vertices]
# gauge -> ours on a dense sampling of the gauge (face centres and edge mids too)
samp = [v.co for v in ref.data.vertices] + [p.center for p in ref.data.polygons]
d2 = [th.find_nearest(p)[3] for p in samp]
print('HULL deviation ours->gauge max %.2f mm | gauge->ours max %.2f mm p99 %.2f mm' % (
    max(d1) * 1e3, max(d2) * 1e3, np.percentile(d2, 99) * 1e3))
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, 'hull.blend'))
print('HULL_DONE')
