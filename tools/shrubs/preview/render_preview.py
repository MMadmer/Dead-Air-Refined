# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Renders shrub_preview.exe's OBJ in Blender (headless) to see a species' architecture.

    blender -b --factory-startup --python render_preview.py -- <in.obj> <out.png> [--leaf r,g,b] [--bloom r,g,b]
        [--wood r,g,b] [--side] [--top] [--res WxH]

Leaves, wood and blooms get flat materials of the given linear colours; a sun from the front left, a sky, a
ground plane. The camera frames the whole row from the front (or the side, or above). render_gallery.py calls
render_scene for a whole list of bushes in one session.
"""
import math
import sys

import bpy
from mathutils import Vector


def material(name, rgb, rough=0.6, trans=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes.get('Principled BSDF')
    p.inputs['Base Color'].default_value = (*rgb, 1.0)
    p.inputs['Roughness'].default_value = rough
    if trans > 0.0 and 'Subsurface Weight' in p.inputs:
        p.inputs['Subsurface Weight'].default_value = trans
    return m


def render_scene(src, out, leaf=(0.06, 0.16, 0.035), bloom=(0.8, 0.8, 0.75), wood=(0.11, 0.09, 0.07), res=(1800, 800),
                 view='front'):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.obj_import(filepath=src, forward_axis='NEGATIVE_Z', up_axis='Y')
    mats = {'wood': material('wood', wood, 0.8), 'leaves': material('leaves', leaf, 0.5, 0.15),
            'blooms': material('blooms', bloom, 0.6)}
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for ob in list(bpy.context.scene.objects):
        if ob.type != 'MESH':
            continue
        kind = ob.name.split('_')[0]
        ob.data.materials.clear()
        ob.data.materials.append(mats.get(kind, mats['wood']))
        for v in ob.data.vertices:
            w = ob.matrix_world @ v.co
            lo = Vector((min(lo.x, w.x), min(lo.y, w.y), min(lo.z, w.z)))
            hi = Vector((max(hi.x, w.x), max(hi.y, w.y), max(hi.z, w.z)))
    centre = (lo + hi) * 0.5
    size = hi - lo
    bpy.ops.mesh.primitive_plane_add(size=max(size.x, size.y) * 3.0 + 10.0, location=(centre.x, centre.y, 0.0))
    bpy.context.object.data.materials.append(material('ground', (0.09, 0.075, 0.05), 0.95))
    sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
    sun.data.energy = 4.0
    sun.data.angle = math.radians(1.0)
    sun.rotation_euler = (math.radians(50.0), 0.0, math.radians(-35.0))
    bpy.context.scene.collection.objects.link(sun)
    world = bpy.data.worlds.new('sky')
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.42, 0.55, 0.75, 1.0)
    world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.6
    bpy.context.scene.world = world
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
    bpy.context.scene.collection.objects.link(cam)
    bpy.context.scene.camera = cam
    aspect = res[0] / res[1]
    cam.data.lens = 50.0
    fov = 2.0 * math.atan(36.0 / 2.0 / cam.data.lens)
    if view == 'top':
        extent = max(size.x / aspect, size.y) * 0.55
        cam.location = (centre.x, centre.y, hi.z + extent / math.tan(fov / 2.0) / aspect + 1.0)
        cam.rotation_euler = (0.0, 0.0, 0.0)
    else:
        across = size.x if view == 'front' else size.y
        extent = max(across / aspect, size.z) * 0.62
        dist = extent / math.tan(fov / 2.0 / aspect) + max(size.x, size.y) * 0.5
        elev = math.radians(8.0)
        target = Vector((centre.x, centre.y, size.z * 0.45))
        d = Vector((0.0, -1.0, 0.0)) if view == 'front' else Vector((1.0, 0.0, 0.0))
        cam.location = target - d * dist * math.cos(elev) + Vector((0.0, 0.0, dist * math.sin(elev)))
        cam.rotation_euler = (target - cam.location).to_track_quat('-Z', 'Y').to_euler()
    scene = bpy.context.scene
    engines = [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items]
    scene.render.engine = 'BLENDER_EEVEE_NEXT' if 'BLENDER_EEVEE_NEXT' in engines else 'BLENDER_EEVEE'
    scene.render.resolution_x, scene.render.resolution_y = res
    scene.render.film_transparent = False
    transforms = [v.identifier for v in scene.view_settings.bl_rna.properties['view_transform'].enum_items]
    scene.view_settings.view_transform = 'AgX' if 'AgX' in transforms else 'Filmic'
    try:
        scene.eevee.taa_render_samples = 16
    except AttributeError:
        pass
    scene.render.filepath = out
    bpy.ops.render.render(write_still=True)
    print('rendered', out)


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    src, out = argv[0], argv[1]
    opts = {'leaf': (0.06, 0.16, 0.035), 'bloom': (0.8, 0.8, 0.75), 'wood': (0.11, 0.09, 0.07)}
    res, view = (1800, 800), 'front'
    i = 2
    while i < len(argv):
        a = argv[i]
        if a in ('--leaf', '--bloom', '--wood'):
            opts[a[2:]] = tuple(float(x) for x in argv[i + 1].split(','))
            i += 2
        elif a == '--res':
            res = tuple(int(x) for x in argv[i + 1].split('x'))
            i += 2
        elif a in ('--side', '--top'):
            view = a[2:]
            i += 1
        else:
            i += 1
    render_scene(src, out, opts['leaf'], opts['bloom'], opts['wood'], res, view)


if __name__ == '__main__':
    main()
