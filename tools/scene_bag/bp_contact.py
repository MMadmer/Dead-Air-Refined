"""Where the hands touch the bag over the open and close cycles: for every bag vertex the nearest skinned
hand vertex over all frames (bag frame, root at rest). Saves contact.npz {verts, dmin, frame} and prints the
touched regions.

    blender -b --python bp_contact.py -- [hands]"""
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix
from mathutils.kdtree import KDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv += ['--'] if '--' not in sys.argv else []
args = sys.argv[sys.argv.index('--') + 1:]
hands = args[0] if args else 'wpn_hand_00'
sys.argv = [sys.argv[0], '--', os.path.join(r"D:\Games\Dead-Air-Refined_Assets\source\gamedata-1.6.0",
            r"meshes\dynamics\weapons\wpn_eat\backpack\wpn_backpack_act_hud.ogf"), '_contact', hands]
# reuse the scene builder up to the render loop
src = open(os.path.join(HERE, 'bp_scene.py'), encoding='utf-8').read()
src = src[:src.index("sc = bpy.context.scene\nsc.render.engine")]
exec(compile(src, 'bp_scene_head', 'exec'))

bag_mesh = next(o for o in bpy.data.objects if o.type == 'MESH' and o.parent == b_arm)
hand_meshes = [o for o in bpy.data.objects if o.type == 'MESH' and o.parent == h_arm]
me = bag_mesh.data
grp = {g.index: g.name for g in bag_mesh.vertex_groups}
body_idx = [v.index for v in me.vertices if v.groups and grp[max(v.groups, key=lambda g: g.weight).group] == 'wpn_body']
bag_local = np.array([v.co[:] for v in me.vertices])
dmin = np.full(len(me.vertices), 9.0)
fmin = np.full(len(me.vertices), -1, int)
sc = bpy.context.scene
dg = bpy.context.evaluated_depsgraph_get()
for clip in ('open_b_backpack', 'close_b_backpack'):
    assign(h_arm, h_acts[clip])
    assign(b_arm, b_acts[clip + '_itm'])
    f0, f1 = (int(round(x)) for x in h_acts[clip].frame_range)
    for f in range(f0, f1 + 1):
        sc.frame_set(f)
        bpy.context.view_layer.update()
        b_arm.matrix_world = h_arm.matrix_world @ h_arm.pose.bones['lead_gun'].matrix @ MB.inverted()
        bpy.context.view_layer.update()
        dg = bpy.context.evaluated_depsgraph_get()
        # hand vertices into the bag frame
        inv = b_arm.matrix_world.inverted()
        pts = []
        for o in hand_meshes:
            ev = o.evaluated_get(dg)
            m = inv @ ev.matrix_world
            pts.extend((m @ v.co)[:] for v in ev.data.vertices)
        kd = KDTree(len(pts))
        for i, p in enumerate(pts):
            kd.insert(p, i)
        kd.balance()
        for i in body_idx:
            _, _, d = kd.find(bag_local[i])
            if d < dmin[i]:
                dmin[i] = d
                fmin[i] = (0 if clip.startswith('open') else 1000) + f
np.savez(os.path.join(HERE, f'contact_{hands}.npz'), verts=bag_local, dmin=dmin, frame=fmin,
         body=np.array(body_idx))
near = [i for i in body_idx if dmin[i] < 0.015]
print('CONTACT body verts', len(body_idx), 'within 15 mm', len(near), 'within 5 mm', sum(dmin[i] < 0.005 for i in body_idx))
for i in sorted(near, key=lambda i: dmin[i])[:40]:
    print('  v', i, np.round(bag_local[i], 3), 'd', round(float(dmin[i]), 4), 'frame', fmin[i])
print('CONTACT_DONE')
