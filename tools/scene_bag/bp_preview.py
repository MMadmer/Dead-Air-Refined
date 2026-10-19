"""Cycles look of a finished bag: the exported OGF with its baked albedo, tangent normal and gloss, lit by a
sky and a key light; four views (HUD-like, 3/4, zipper close-up, pull close-up) tiled into one PNG.

    blender -b --python bp_preview.py -- <variant> [frame]"""
import math
import os
import subprocess
import sys

import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
a = sys.argv[sys.argv.index('--') + 1:]
NAME = a[0]
FRAME = int(a[1]) if len(a) > 1 else 0
OUT = os.path.join(HERE, 'out', NAME)
GD = r"D:\Games\Dead-Air-Refined_Assets\source\gamedata"
BPDIR = os.path.join(GD, r"meshes\dynamics\weapons\wpn_eat\backpack")

prefs = bpy.context.preferences.addons['io_scene_xray'].preferences
ORIG = NAME == 'orig'
prefs.gamedata_folder = (GD if ORIG else os.path.join(OUT, 'gd')) + '\\'
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
if ORIG:
    bpy.ops.xray_import.ogf(directory=BPDIR, files=[{'name': 'wpn_backpack_act_hud.ogf'}], import_motions=False)
else:
    bpy.ops.xray_import.ogf(directory=os.path.join(OUT, 'game'), files=[{'name': f'dar_bag_{NAME}_hud.ogf'}],
                            import_motions=False)
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
bag = next(o for o in bpy.data.objects if o.type == 'MESH')
# the zipper at a frame of the open cycle (root held at rest)
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
bpy.ops.xray_import.omf(directory=BPDIR, files=[{'name': 'wpn_backpack_act_hud_anm.omf'}], import_motions=True,
                        import_bone_parts=False, add_to_motion_list=False)
act = bpy.data.actions.get('open_b_backpack_itm')
arm.animation_data_create()
arm.animation_data.action = act
if act.slots:
    arm.animation_data.action_slot = act.slots[0]
from io_scene_xray.utils.version import fcurves  # noqa: E402
for fc in fcurves(act):
    if 'wpn_body' in fc.data_path:
        fc.mute = True
pb = arm.pose.bones['wpn_body']
pb.location = (0, 0, 0)
pb.rotation_euler = (0, 0, 0)
pb.rotation_quaternion = (1, 0, 0, 0)
sc = bpy.context.scene
sc.frame_set(FRAME)

# material from the bakes (the original keeps the importer's own, its albedo)
m = bpy.data.materials.new('prev')
if ORIG:
    src = bag.data.materials[0]
    img = next(n.image for n in src.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image)
    os.makedirs(OUT, exist_ok=True)
m.use_nodes = True
nt = m.node_tree
bsdf = nt.nodes['Principled BSDF']
col = nt.nodes.new('ShaderNodeTexImage')
col.image = img if ORIG else bpy.data.images.load(os.path.join(OUT, 'bake_col.png'))
nrm = nt.nodes.new('ShaderNodeTexImage')
gl = nt.nodes.new('ShaderNodeTexImage')
if not ORIG:
    nrm.image = bpy.data.images.load(os.path.join(OUT, 'bake_normal.exr'))
    nrm.image.colorspace_settings.name = 'Non-Color'
    gl.image = bpy.data.images.load(os.path.join(OUT, 'bake_gloss.png'))
    gl.image.colorspace_settings.name = 'Non-Color'
nm = nt.nodes.new('ShaderNodeNormalMap')
inv = nt.nodes.new('ShaderNodeMath')
inv.operation = 'SUBTRACT'
inv.inputs[0].default_value = 1.0
nt.links.new(col.outputs[0], bsdf.inputs['Base Color'])
if not ORIG:
    nt.links.new(nrm.outputs[0], nm.inputs['Color'])
    nt.links.new(nm.outputs[0], bsdf.inputs['Normal'])
    nt.links.new(gl.outputs[0], inv.inputs[1])
    nt.links.new(inv.outputs[0], bsdf.inputs['Roughness'])
else:
    bsdf.inputs['Roughness'].default_value = 0.8
bsdf.inputs['Specular IOR Level'].default_value = 0.35
bag.data.materials.clear()
bag.data.materials.append(m)

sc.render.engine = 'CYCLES'
cp = bpy.context.preferences.addons['cycles'].preferences
for kind in ('OPTIX', 'CUDA'):
    try:
        cp.compute_device_type = kind
        cp.get_devices()
        if any(d.type == kind for d in cp.devices):
            for d in cp.devices:
                d.use = d.type == kind
            sc.cycles.device = 'GPU'
            break
    except Exception:
        pass
sc.cycles.samples = 64
sc.cycles.use_denoising = True
sc.render.resolution_x, sc.render.resolution_y = 960, 720
world = bpy.data.worlds.new('w')
sc.world = world
world.use_nodes = True
sky = world.node_tree.nodes.new('ShaderNodeTexSky')
world.node_tree.links.new(sky.outputs[0], world.node_tree.nodes['Background'].inputs[0])
world.node_tree.nodes['Background'].inputs[1].default_value = 0.12
sc.view_settings.view_transform = 'Standard'
sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
sun.data.energy = 1.6
sun.rotation_euler = (math.radians(35), math.radians(-25), math.radians(20))
sc.collection.objects.link(sun)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.clip_start = 0.005
views = [(Vector((0.0, -0.30, 0.42)), Vector((0, 0.0, 0.03)), 40),
         (Vector((0.42, -0.36, 0.30)), Vector((0, 0.0, 0.0)), 40),
         (Vector((-0.24, 0.02, 0.16)), Vector((-0.11, 0.055, 0.065)), 26),
         (Vector((0.24, -0.02, 0.16)), Vector((0.115, 0.035, 0.065)), 26)]
tiles = []
for i, (loc, tgt, fov) in enumerate(views):
    cam.location = loc
    cam.rotation_euler = (tgt - loc).to_track_quat('-Z', 'Y').to_euler()
    cam.data.angle = math.radians(fov)
    p = os.path.join(OUT, f'_prev{i}.png')
    sc.render.filepath = p
    bpy.ops.render.render(write_still=True)
    tiles.append(p)
out = os.path.join(HERE, f'preview_{NAME}_{FRAME:03d}.png')
subprocess.run(['python', '-c', 'import sys;from PIL import Image;t=sys.argv[2:];ims=[Image.open(p).convert("RGB") for p in t];W=Image.new("RGB",(ims[0].width*2,ims[0].height*2));[W.paste(im,((k%2)*im.width,(k//2)*im.height)) for k,im in enumerate(ims)];W.resize((W.width*3//4,W.height*3//4)).save(sys.argv[1])', out] + tiles, check=True)
print('PREVIEW_DONE', out)
