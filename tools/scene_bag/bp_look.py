"""Quick Workbench look at named objects: 4 views side by side.  blender -b f.blend --python bp_look.py -- out.png obj1 [obj2 ...]"""
import os, sys, math
import bpy
from mathutils import Vector
a = sys.argv[sys.argv.index('--') + 1:]
out, names = a[0], a[1:]
sc = bpy.context.scene
for o in bpy.data.objects:
    if o.type == 'ARMATURE':
        o.data.pose_position = 'REST'
    o.hide_render = o.name not in names and o.type in ('MESH', 'CURVE')
sc_mat = None
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.color_type = 'TEXTURE' if '--tex' in a else ('MATERIAL' if '--mat' in a else 'SINGLE')
sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 700, 600
cam = bpy.data.objects.new('lcam', bpy.data.cameras.new('lcam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = 0.44
tiles = []
for i, d in enumerate([Vector((0, 0, 1)), Vector((0.7, -0.5, 0.5)).normalized(), Vector((-0.7, 0.4, 0.6)).normalized(), Vector((1, 0, 0.05)).normalized()]):
    cam.location = d
    cam.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
    p = os.path.join(os.path.dirname(out), f'_look{i}.png'); sc.render.filepath = p
    bpy.ops.render.render(write_still=True); tiles.append(p)
import subprocess
subprocess.run(['python', '-c', 'import sys;from PIL import Image;t=sys.argv[2:];ims=[Image.open(p).convert("RGB") for p in t];W=Image.new("RGB",(ims[0].width*2,ims[0].height*2));[W.paste(im,((k%2)*im.width,(k//2)*im.height)) for k,im in enumerate(ims)];W.save(sys.argv[1])', out] + tiles, check=True)
print('LOOK_DONE')
