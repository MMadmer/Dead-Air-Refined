"""The hands over both scene cycles in the bag's frame (the bag body never moves in it: the root is held):
every skinned hand vertex per frame -> hands_<hands>.npz {open: (F,N,3), close: (F,N,3)}.
    blender -b --python bp_handdump.py -- [hands]"""
import os
import sys

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
a = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
hands = a[0] if a else 'wpn_hand_00'
sys.argv = [sys.argv[0], '--', os.path.join(r"D:\Games\Dead-Air-Refined_Assets\source\gamedata-1.6.0",
            r"meshes\dynamics\weapons\wpn_eat\backpack\wpn_backpack_act_hud_anm.omf").replace('_anm.omf', '.ogf'),
            '_dump', hands]
src = open(os.path.join(HERE, 'bp_scene.py'), encoding='utf-8').read()
src = src[:src.index("sc = bpy.context.scene\nsc.render.engine")]
exec(compile(src, 'bp_scene_head', 'exec'))
hand_meshes = [o for o in bpy.data.objects if o.type == 'MESH' and o.parent == h_arm]
sc = bpy.context.scene
out = {}
for clip in ('open_b_backpack', 'close_b_backpack'):
    assign(h_arm, h_acts[clip])
    f0, f1 = (int(round(x)) for x in h_acts[clip].frame_range)
    seq = []
    for f in range(f0, f1 + 1):
        sc.frame_set(f)
        bpy.context.view_layer.update()
        bag_m = h_arm.matrix_world @ h_arm.pose.bones['lead_gun'].matrix @ MB.inverted()
        inv = bag_m.inverted()
        dg = bpy.context.evaluated_depsgraph_get()
        pts = []
        for o in hand_meshes:
            ev = o.evaluated_get(dg)
            m = inv @ ev.matrix_world
            pts.extend((m @ v.co)[:] for v in ev.data.vertices)
        seq.append(pts)
    out[clip.split('_')[0]] = np.array(seq, np.float32)
    print('DUMP', clip, out[clip.split('_')[0]].shape)
np.savez_compressed(os.path.join(HERE, f'hands_{hands}.npz'), **out)
print('DUMP_DONE')
