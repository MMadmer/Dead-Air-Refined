"""The scene bag as the game has it: bones, mesh split by bone, UV islands, and the bag's own motions
sampled per frame (zipper slider path in the bag frame). Saves bp_orig.blend and bp_orig.json.

    blender -b --factory-startup? no: needs the X-Ray addon -> blender -b --python bp_analyze.py"""
import json
import os

import bpy
from mathutils import Vector

GD = r"D:\Games\Dead-Air-Refined_Assets\source\gamedata-1.6.0"
BP = os.path.join(GD, r"meshes\dynamics\weapons\wpn_eat\backpack")
OUT = os.path.dirname(os.path.abspath(__file__))

prefs = bpy.context.preferences.addons['io_scene_xray'].preferences
prefs.gamedata_folder = GD + '\\'
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)

bpy.ops.xray_import.ogf(directory=BP, files=[{'name': 'wpn_backpack_act_hud.ogf'}], import_motions=False)
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
mesh = next(o for o in bpy.data.objects if o.type == 'MESH')
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
before = set(bpy.data.actions)
bpy.ops.xray_import.omf(directory=BP, files=[{'name': 'wpn_backpack_act_hud_anm.omf'}],
                        import_motions=True, import_bone_parts=True, add_to_motion_list=True)
acts = [a for a in bpy.data.actions if a not in before]
for a in acts:
    a.use_fake_user = True

out = {'bones': {}, 'mesh': {}, 'motions': {}}
for b in arm.data.bones:
    out['bones'][b.name] = {'parent': b.parent.name if b.parent else None,
                            'head': list(b.head_local), 'tail': list(b.tail_local),
                            'matrix_local': [list(r) for r in b.matrix_local]}
me = mesh.data
gname = {g.index: g.name for g in mesh.vertex_groups}
uv = me.uv_layers.active.data
vgrp = []
for v in me.vertices:
    gs = sorted(((g.weight, gname[g.group]) for g in v.groups), reverse=True)
    vgrp.append(gs[0][1] if gs else None)
vert_uv = {}
for poly in me.polygons:
    for li, vi in zip(poly.loop_indices, poly.vertices):
        vert_uv.setdefault(vi, []).append(list(uv[li].uv))
out['mesh'] = {'object': mesh.name, 'matrix_world': [list(r) for r in mesh.matrix_world],
               'verts': [list(v.co) for v in me.vertices], 'group': vgrp,
               'faces': [list(p.vertices) for p in me.polygons],
               'face_uv': [[list(uv[li].uv) for li in p.loop_indices] for p in me.polygons],
               'materials': [m.name for m in me.materials]}
for g in set(vgrp):
    pts = [Vector(me.vertices[i].co) for i, x in enumerate(vgrp) if x == g]
    print('GROUP', g, len(pts), 'min', [round(min(p[k] for p in pts), 4) for k in range(3)],
          'max', [round(max(p[k] for p in pts), 4) for k in range(3)])

arm.animation_data_create()
sc = bpy.context.scene
for a in acts:
    arm.animation_data.action = a
    if hasattr(arm.animation_data, 'action_slot') and a.slots:
        arm.animation_data.action_slot = a.slots[0]
    f0, f1 = (int(round(x)) for x in a.frame_range)
    rows = []
    for f in range(f0, f1 + 1):
        sc.frame_set(f)
        bpy.context.view_layer.update()
        rows.append({bn: [list(r) for r in arm.pose.bones[bn].matrix] for bn in ('wpn_body', 'zipper')})
    out['motions'][a.name] = {'range': [f0, f1], 'fps': sc.render.fps, 'frames': rows}
    zs = [Vector([r[i][3] for i in range(3)]) for r in (x['zipper'] for x in rows)]
    bs = [Vector([r[i][3] for i in range(3)]) for r in (x['wpn_body'] for x in rows)]
    print('MOTION', a.name, f0, f1, 'zip first', [round(c, 4) for c in zs[0]], 'last', [round(c, 4) for c in zs[-1]],
          'body moves', round(max((p - bs[0]).length for p in bs), 4))
json.dump(out, open(os.path.join(OUT, 'bp_orig.json'), 'w'), indent=0)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, 'bp_orig.blend'))
print('DONE')
