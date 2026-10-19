"""Depth of the hands in a bag's body (the pull left out), per hand vertex and frame, against the dumped
hands (bp_handdump.py). The original's depths are the baseline: what counts is how much deeper a hand goes
into ours than into theirs at the same vertex and frame.
    blender -b --python bp_depth.py -- <tag> <bag .ogf> [hands]"""
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
a = sys.argv[sys.argv.index('--') + 1:]
tag, ogf = a[0], a[1]
hands = a[2] if len(a) > 2 else 'wpn_hand_00'
gd = os.path.join(os.path.dirname(os.path.dirname(ogf)), 'gd')
prefs = bpy.context.preferences.addons['io_scene_xray'].preferences
prefs.gamedata_folder = (gd if os.path.isdir(gd) else r"D:\Games\Dead-Air-Refined_Assets\source\gamedata") + '\\'
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.xray_import.ogf(directory=os.path.dirname(ogf), files=[{'name': os.path.basename(ogf)}], import_motions=False)
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
arm.data.pose_position = 'REST'
bm = bmesh.new()
for o in bpy.data.objects:
    if o.type != 'MESH':
        continue
    tmp = bmesh.new()
    tmp.from_mesh(o.data)
    gi = {g.index: g.name for g in o.vertex_groups}
    dl = tmp.verts.layers.deform.active
    pull = {v for v in tmp.verts if dl and v[dl] and gi[max(v[dl].items(), key=lambda kv: kv[1])[0]] == 'zipper'}
    bmesh.ops.delete(tmp, geom=[f for f in tmp.faces if any(v in pull for v in f.verts)], context='FACES')
    tmp.transform(o.matrix_world)
    me = bpy.data.meshes.new('t')
    tmp.to_mesh(me)
    bm.from_mesh(me)
    tmp.free()
tree = BVHTree.FromBMesh(bm)
H = np.load(os.path.join(HERE, f'hands_{hands}.npz'))
out = {}
for clip in ('open', 'close'):
    P = H[clip]
    D = np.zeros(P.shape[:2], np.float32)
    U = np.full(P.shape[:2], 0.03, np.float32)
    for fi in range(P.shape[0]):
        for vi in range(P.shape[1]):
            p = Vector(P[fi, vi])
            loc, nrm, _, d = tree.find_nearest(p, 0.03)
            if loc is not None:
                U[fi, vi] = d
                if (p - loc).dot(nrm) < 0:
                    D[fi, vi] = d
    out[clip] = D
    out[clip + '_u'] = U
np.savez_compressed(os.path.join(HERE, f'depth_{tag}.npz'), **out)
for clip in ('open', 'close'):
    D = out[clip]
    print('DEPTH %s %s max %.1f mm, verts deeper than 1 mm per frame (median) %d' % (
        tag, clip, D.max() * 1e3, int(np.median((D > 0.001).sum(1)))))
print('DEPTH_DONE', tag)
