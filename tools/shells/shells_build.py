"""Spent cartridge cases -> OGF, one per calibre (tools/shells/shells_data.py), on the shared atlas
wpn\\dar_shells. Run headless:

    blender -b --python shells_build.py -- <out dir>

A case is a revolved profile: every profile segment is a band of its own vertices, so the rim, the groove,
the shoulder and the mouth keep hard edges while the round stays smooth. The head carries the primer with
the firing pin's dimple; the mouth is open down to a dark interior. Blender +Y is the case axis (the
exporter turns it into X-Ray +Z), the origin is the middle of the case, metres.
"""
import math
import os
import sys

import bmesh
import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shells_data import ATLAS, BAND_PX, CASES, FINISHES  # noqa: E402

OUT = sys.argv[sys.argv.index('--') + 1] if '--' in sys.argv else os.path.join(os.path.dirname(__file__), 'out')
TEXTURE = 'wpn\\dar_shells'
MM = 0.001


def band_uv(finish, u, t):
    """Side of a case: u around (0..1), t along it (0 head .. 1 mouth)."""
    row = FINISHES.index(finish)
    py = row * BAND_PX + 2 + (1.0 - t) * (BAND_PX - 4)
    return (u, 1.0 - py / ATLAS)


def tile_uv(tile_row, tile_col, x, y):
    """A 64 px tile: x, y in -1..1."""
    cx = tile_col * BAND_PX + BAND_PX / 2
    cy = tile_row * BAND_PX + BAND_PX / 2
    px = cx + x * (BAND_PX / 2 - 3)
    py = cy + y * (BAND_PX / 2 - 3)
    return (px / ATLAS, 1.0 - py / ATLAS)


HEAD_ROW, INNER_ROW = 5, 6
PRIMER_COL = 6


def profile(c):
    """(z, r, finish) from the head to the mouth, mm. Consecutive points are one band."""
    L = c['length']
    big = c['base'] > 5.5
    rim_h = 1.4 if big else 0.9
    groove_w = 1.6 if big else 1.0
    f = c['finish']
    pts = []
    kind = c['head']
    if kind == 'hull':
        brass = 'brass'
        hh = c['head_height']
        pts += [(0.0, c['rim'] - 0.5, brass), (0.5, c['rim'], brass), (1.5, c['rim'], brass), (1.9, c['base'] + 0.15, brass),
                (hh, c['base'] + 0.15, brass), (hh, c['base'], f), (L - 2.0, c['mouth'], f), (L, c['mouth'] + 0.35, f)]
        return pts
    pts += [(0.0, c['rim'] - 0.35, f), (0.35, c['rim'], f), (rim_h, c['rim'], f)]
    z = rim_h
    if kind == 'rimmed':
        pts += [(z + 0.5, c['base'], f)]
        z += 0.5
    else:
        g = c['groove']
        pts += [(z + 0.35, g, f), (z + groove_w - 0.35, g, f), (z + groove_w, c['base'], f)]
        z += groove_w
        if kind == 'belted':
            pts += [(5.3, c['rim'], f), (5.6, c['base'], f)]
    if c['shoulder']:
        sz, sr = c['shoulder']
        nz, nr = c['neck']
        pts += [(sz, sr, f), (nz, nr, f), (L, c['mouth'], f)]
    else:
        pts += [(L, c['mouth'], f)]
    return pts


def build(name, c):
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new('uv')
    n = c['segments']
    L = c['length']
    pts = profile(c)

    def v(z, r, a):
        return bm.verts.new(((r * math.cos(a)) * MM, (z - L / 2) * MM, (r * math.sin(a)) * MM))

    def quad(ring0, ring1, uv0, uv1):
        for i in range(n):
            j = i + 1
            f = bm.faces.new((ring0[i], ring0[j], ring1[j], ring1[i]))
            f.smooth = True
            for loop, (a, b) in zip(f.loops, ((uv0, i), (uv0, j), (uv1, j), (uv1, i))):
                loop[uv].uv = a[b]

    angles = [2 * math.pi * i / n for i in range(n + 1)]
    # the side, band by band
    for k in range(len(pts) - 1):
        z0, r0, f0 = pts[k]
        z1, r1, _ = pts[k + 1]
        if abs(z1 - z0) < 1e-6 and abs(r1 - r0) < 1e-6:
            continue
        ring0 = [v(z0, r0, a) for a in angles]
        ring1 = [v(z1, r1, a) for a in angles]
        uv0 = [band_uv(f0, i / n, z0 / L) for i in range(n + 1)]
        uv1 = [band_uv(f0, i / n, z1 / L) for i in range(n + 1)]
        quad(ring0, ring1, uv0, uv1)

    head_col = FINISHES.index('brass' if c['head'] == 'hull' else c['finish'])
    # the head: a flat ring out to the primer, the primer a touch below it, the pin's dimple in its middle
    r_edge = pts[0][1]
    pr = c['primer']
    outer = [v(0.0, r_edge, a) for a in angles]
    inner = [v(0.0, pr + 0.15, a) for a in angles]
    uv_o = [tile_uv(HEAD_ROW, head_col, math.cos(a) * r_edge / c['rim'], math.sin(a) * r_edge / c['rim']) for a in angles]
    uv_i = [tile_uv(HEAD_ROW, head_col, math.cos(a) * (pr + 0.15) / c['rim'], math.sin(a) * (pr + 0.15) / c['rim'])
            for a in angles]
    quad(inner, outer, uv_i, uv_o)
    # the primer has a tile of its own, its edge on the tile's edge whatever the calibre
    cup = [v(0.12, pr, a) for a in angles]
    uv_c = [tile_uv(HEAD_ROW, PRIMER_COL, math.cos(a) * 0.82, math.sin(a) * 0.82) for a in angles]
    uv_pe = [tile_uv(HEAD_ROW, PRIMER_COL, math.cos(a), math.sin(a)) for a in angles]
    ring_in = [v(0.0, pr + 0.15, a) for a in angles]
    quad(cup, ring_in, uv_c, uv_pe)
    centre = bm.verts.new((0.0, (0.55 - L / 2) * MM, 0.0))
    for i in range(n):
        f = bm.faces.new((cup[i + 1], cup[i], centre))
        f.smooth = False
        for loop, t in zip(f.loops, (uv_c[i + 1], uv_c[i], tile_uv(HEAD_ROW, PRIMER_COL, 0, 0))):
            loop[uv].uv = t

    # the mouth: the lip, then the inside going down into the dark
    wall = 0.5 if c['head'] == 'hull' else 0.3
    zt, rt, _ = pts[-1]
    lip_o = [v(zt, rt, a) for a in angles]
    lip_i = [v(zt, rt - wall, a) for a in angles]
    side_row = FINISHES.index(c['finish'])
    uv_lo = [band_uv(c['finish'], i / n, 0.995) for i in range(n + 1)]
    uv_li = [band_uv(c['finish'], i / n, 0.98) for i in range(n + 1)]
    quad(lip_i, lip_o, uv_li, uv_lo)
    depth = min(6.0, L * 0.3)
    in_top = [v(zt, rt - wall, a) for a in angles]
    in_bot = [v(zt - depth, rt - wall, a) for a in angles]
    uv_it = [tile_uv(INNER_ROW, 0, -1 + 2 * i / n, -0.9) for i in range(n + 1)]
    uv_ib = [tile_uv(INNER_ROW, 0, -1 + 2 * i / n, 0.9) for i in range(n + 1)]
    quad(in_bot, in_top, uv_ib, uv_it)
    bottom = bm.verts.new((0.0, (zt - depth - L / 2) * MM, 0.0))
    for i in range(n):
        f = bm.faces.new((in_bot[i], in_bot[i + 1], bottom))
        for loop in f.loops:
            loop[uv].uv = tile_uv(INNER_ROW, 1, 0, 0)
    del side_row

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new('dar_shell_' + name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new('dar_shell_' + name, me)
    bpy.context.collection.objects.link(ob)
    return ob


def material():
    m = bpy.data.materials.new('dar_shells')
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    o_ = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    t = nt.nodes.new('ShaderNodeTexImage')
    # the exporter takes the texture name from the node's name
    t.name = TEXTURE
    t.label = TEXTURE
    t.image = bpy.data.images.new('dar_shells', 4, 4)
    nt.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
    nt.links.new(bsdf.outputs[0], o_.inputs['Surface'])
    nt.nodes.active = t
    return m


def main():
    os.makedirs(OUT, exist_ok=True)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    mat = material()
    only = sys.argv[sys.argv.index('--') + 2:] if '--' in sys.argv else []
    for name, c in CASES.items():
        if only and name not in only:
            continue
        ob = build(name, c)
        ob.data.materials.append(mat)
        mat.xray.eshader = 'models\\model'
        mat.xray.cshader = 'default'
        mat.xray.gamemtl = 'default'
        # A rigid skeleton of one bone, like every item model of the game: the renderer draws nothing of a plain
        # static visual handed to it by a renderable.
        arm_data = bpy.data.armatures.new('dar_shell_' + name + '_skel')
        arm = bpy.data.objects.new('dar_shell_' + name + '_root', arm_data)
        bpy.context.collection.objects.link(arm)
        arm.xray.isroot = True
        bpy.context.view_layer.objects.active = arm
        bpy.ops.object.mode_set(mode='EDIT')
        bone = arm_data.edit_bones.new('link')
        bone.head = (0.0, 0.0, 0.0)
        bone.tail = (0.0, 0.01, 0.0)
        bpy.ops.object.mode_set(mode='OBJECT')
        ob.parent = arm
        group = ob.vertex_groups.new(name='link')
        group.add([v.index for v in ob.data.vertices], 1.0, 'REPLACE')
        mod = ob.modifiers.new('arm', 'ARMATURE')
        mod.object = arm
        for o in bpy.data.objects:
            if o is not None:
                o.select_set(False)
        arm.select_set(True)
        bpy.context.view_layer.objects.active = arm
        path = os.path.join(OUT, f'dar_shell_{name}.ogf')
        res = bpy.ops.xray_export.ogf_file(filepath=path, fmt_version='cscop', export_motions=False, hq_export=True,
                                           use_export_paths=False, texture_name_from_image_path=False)
        print('SHELL', name, res, len(ob.data.polygons), 'faces',
              sum(len(p.vertices) - 2 for p in ob.data.polygons), 'tris', os.path.getsize(path) if os.path.exists(path) else None)
        bpy.data.objects.remove(ob, do_unlink=True)
        bpy.data.objects.remove(arm, do_unlink=True)


main()
