"""The backpack scene as the engine builds it, from the HUD camera: hands (stock model) playing the
zzzz_ea_backpack cycle, the bag on the hands' lead_gun with its root held at rest and its own cycle on
the zipper. Renders chosen frames of the open and close cycles.

    blender -b --python bp_scene.py -- <bag .ogf path | blend:<file>:<object prefix>> <out tag> [hands]
"""
import math
import os
import sys

import bpy
from mathutils import Matrix

HERE = os.path.dirname(os.path.abspath(__file__))
GD = r"D:\Games\Dead-Air-Refined_Assets\source\gamedata"
STOCK = r"D:\Games\Dead Air\_work\texaudit\stock"
BP = os.path.join(GD, r"meshes\dynamics\weapons\wpn_eat\backpack")
HANDS_DIR = os.path.join(STOCK, r"meshes\dynamics\weapons\wpn_hand")
ANIM = os.path.join(GD, r"meshes\anomaly_weapons\hud_hands_animation")
MB = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))

args = sys.argv[sys.argv.index('--') + 1:]
bag_src, tag = args[0], args[1]
hands = args[2] if len(args) > 2 else 'wpn_hand_00'
FRAMES = {'open_b_backpack': [0, 10, 18, 21, 23, 25, 27, 29, 31, 34, 44],
          'close_b_backpack': [0, 8, 10, 12, 14, 16, 18, 20, 30]}

prefs = bpy.context.preferences.addons['io_scene_xray'].preferences
prefs.gamedata_folder = STOCK + '\\'
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)


def imp_ogf(directory, name):
    before = set(bpy.data.objects)
    bpy.ops.xray_import.ogf(directory=directory, files=[{'name': name}], import_motions=False)
    new = [o for o in bpy.data.objects if o not in before]
    return next(o for o in new if o.type == 'ARMATURE'), [o for o in new if o.type == 'MESH']


def imp_omf(arm, directory, name):
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    before = set(bpy.data.actions)
    bpy.ops.xray_import.omf(directory=directory, files=[{'name': name}], import_motions=True,
                            import_bone_parts=False, add_to_motion_list=False)
    acts = {a.name: a for a in bpy.data.actions if a not in before}
    for a in acts.values():
        a.use_fake_user = True
    return acts


def assign(obj, act):
    obj.animation_data_create()
    obj.animation_data.action = act
    if hasattr(obj.animation_data, 'action_slot') and act.slots:
        obj.animation_data.action_slot = act.slots[0]


h_arm, h_meshes = imp_ogf(HANDS_DIR, hands + '.ogf')
h_acts = imp_omf(h_arm, ANIM, 'zzzz_ea_backpack.omf')

if bag_src.startswith('blend:'):
    _, path, prefix = bag_src.split(':', 2)
    with bpy.data.libraries.load(path.replace('|', ':')) as (src, dst):
        dst.objects = [n for n in src.objects if n.startswith(prefix)]
    for o in dst.objects:
        bpy.context.scene.collection.objects.link(o)
    b_arm = next(o for o in dst.objects if o.type == 'ARMATURE')
else:
    # our bags carry their textures in out/<v>/gd until installed
    gd = os.path.join(os.path.dirname(os.path.dirname(bag_src)), 'gd')
    prefs.gamedata_folder = (gd if os.path.isdir(gd) else GD) + '\\'
    b_arm, _ = imp_ogf(os.path.dirname(bag_src), os.path.basename(bag_src))
b_acts = imp_omf(b_arm, BP, 'wpn_backpack_act_hud_anm.omf')
# the engine overwrites the item root with identity: drop the root's curves
for a in b_acts.values():
    fcs = a.fcurves if hasattr(a, 'fcurves') else None
    try:
        from io_scene_xray.utils.version import fcurves as _fc
        fcs = _fc(a)
    except Exception:
        pass
    for fc in list(fcs):
        if 'wpn_body' in fc.data_path:
            fc.mute = True
b_arm.pose.bones['wpn_body'].location = (0, 0, 0)
b_arm.pose.bones['wpn_body'].rotation_euler = (0, 0, 0)
b_arm.pose.bones['wpn_body'].rotation_quaternion = (1, 0, 0, 0)

sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.color_type = 'TEXTURE'
sc.render.resolution_x, sc.render.resolution_y = 1280, 720
cam = bpy.data.objects.new('hudcam', bpy.data.cameras.new('hudcam'))
sc.collection.objects.link(cam)
cam.rotation_euler = (math.radians(90), 0, 0)
cam.data.sensor_fit = 'VERTICAL'
cam.data.angle = math.radians(33.75)
cam.data.clip_start = 0.01
sc.camera = cam

out_dir = os.path.join(HERE, 'scene', tag)
os.makedirs(out_dir, exist_ok=True)
for clip, frames in FRAMES.items():
    assign(h_arm, h_acts[clip])
    assign(b_arm, b_acts[clip + '_itm'])
    for f in frames:
        sc.frame_set(f)
        bpy.context.view_layer.update()
        # item = hands * lead_gun (X-Ray), i.e. the lead_gun pose without the bone-axis correction
        b_arm.matrix_world = h_arm.matrix_world @ h_arm.pose.bones['lead_gun'].matrix @ MB.inverted()
        bpy.context.view_layer.update()
        sc.render.filepath = os.path.join(out_dir, f'{clip}_{f:03d}.png')
        bpy.ops.render.render(write_still=True)
print('SCENE_DONE', out_dir)
