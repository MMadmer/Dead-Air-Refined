"""One scene bag: our hull (shape-gauged on the original), the zipper along the slider's centreline, the
variant's details and pull, skinned on the original skeleton (wpn_body + zipper, X-Ray bind kept, motion
refs to wpn_backpack_act_hud_anm), baked and exported as an OGF.

    blender -b hull.blend --python bp_build.py -- <variant> [--no-bake] [--no-export]"""
import importlib
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bp_parts as BP  # noqa: E402
import bp_variants as VAR  # noqa: E402
importlib.reload(BP)
importlib.reload(VAR)

args = sys.argv[sys.argv.index('--') + 1:]
NAME = args[0]
SPEC = VAR.VARIANTS[NAME]
MM = 0.001
OUT = os.path.join(HERE, 'out', NAME)
os.makedirs(OUT, exist_ok=True)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
arm.data.pose_position = 'REST'
orig = next(o for o in bpy.data.objects if o.type == 'MESH' and o.parent == arm)
ref = bpy.data.objects['hull_ref']
hull = bpy.data.objects['hull']
orig.hide_render = True
ref.hide_render = True
hull.hide_render = True


def new_obj(name, bm):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    return o


# ------------------------------------------------------------------------------------------ body
def make_body():
    bm = bmesh.new()
    bm.from_mesh(hull.data)
    # soften the gauge's facets: relax steps, each vertex kept near where it was - within a millimetre
    # where the hands come (contact map of the original over both cycles), more where they never do
    c = np.load(os.path.join(HERE, 'contact_wpn_hand_00.npz'))
    gv, gd = c['verts'][c['body']], c['dmin'][c['body']]
    base = {v: v.co.copy() for v in bm.verts}
    limit = {}
    for v in bm.verts:
        d = gd[int(np.argmin(np.linalg.norm(gv - np.array(v.co), axis=1)))]
        limit[v] = (0.8 if d < 0.02 else 2.5) * MM
    for _ in range(10):
        new = {}
        for v in bm.verts:
            nb = [e.other_vert(v).co for e in v.link_edges]
            new[v] = v.co.lerp(sum(nb, Vector()) / len(nb), 0.5)
        for v, p in new.items():
            d = p - base[v]
            if d.length > limit[v]:
                p = base[v] + d.normalized() * limit[v]
            v.co = p
    o = new_obj('bag_body', bm)
    # fewer triangles: the cloth is smooth and the bake carries the detail
    dec = o.modifiers.new('dec', 'DECIMATE')
    dec.ratio = SPEC.get('body_ratio', 0.45)
    for ob in bpy.context.view_layer.objects:
        ob.select_set(False)
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.modifier_apply(modifier='dec')
    for p in o.data.polygons:
        p.use_smooth = True
    return o


def soften_crease(body, reach=0.011, limit=1.6 * MM):
    """The zipper runs on the pocket's edge crease: relax the hull near its centreline, moving points only
    inward (no new contact with the hands) and by at most `limit`, so the tape can lie across it."""
    z = np.load(os.path.join(HERE, 'zip_curve.npz'))
    C = z['p']
    bm = bmesh.new()
    bm.from_mesh(body.data)
    bm.normal_update()
    base = {v: (v.co.copy(), v.normal.copy()) for v in bm.verts}
    near = [v for v in bm.verts if np.min(np.linalg.norm(C - np.array(v.co), axis=1)) < reach]
    for _ in range(10):
        new = {}
        for v in near:
            nb = [e.other_vert(v).co for e in v.link_edges]
            new[v] = v.co.lerp(sum(nb, Vector()) / len(nb), 0.6)
        for v, p in new.items():
            b0, n0 = base[v]
            d = p - b0
            inward = min(0.0, d.dot(n0))
            d = d - n0 * d.dot(n0) + n0 * max(inward, -limit)
            v.co = b0 + d
    bm.to_mesh(body.data)
    bm.free()


# ------------------------------------------------------------------------------------------ zipper
def make_tabs(surf, fr):
    """Cloth tabs sewn over both ends of the zipper, clear of the pull: it lies back over [-20, 0] mm at
    rest and forward over about [0, 16] mm past the open end."""
    import bp_details as D
    importlib.reload(D)
    s, pts, tan, nrm, bin_, s_open = fr
    bm = bmesh.new()
    for s_at in (s[0] + 7.5 * MM, s[-1] - 7.5 * MM):
        i = int(np.argmin(np.abs(s - s_at)))
        D.frame_slab(bm, surf, pts[i], tan[i], bin_[i], 17 * MM, 24 * MM, 1.3 * MM, 2.5 * MM, 0)
    o = new_obj('bag_ztabs', bm)
    slot(o, 'trim')
    for p in o.data.polygons:
        p.use_smooth = True
    return o


def make_zipper(surf):
    fr = BP.zip_frames(surf, ext_before=SPEC.get('zip_before', 32) * MM, ext_after=SPEC.get('zip_after', 32) * MM)
    s, pts, tan, nrm, bin_, s_open = fr
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new('pattern')
    prof = BP.zipper_profile(**SPEC.get('zip_profile', {}))
    faces, _ = BP.sweep(bm, fr, prof, uv_layer=uvl, every=3, surf=surf)
    SWEEP_FACES[0] = len(faces)
    # end stops just past both ends of the travel
    for s_at in (-2.2 * MM, s_open + 2.4 * MM):
        i = int(np.argmin(np.abs(s - s_at)))
        BP.box(bm, pts[i] + nrm[i] * 0.9 * MM, tan[i], bin_[i], nrm[i], (2.2 * MM, 6.4 * MM, 1.5 * MM), round_=0.4 * MM)
    o = new_obj('bag_zipper', bm)
    for p in o.data.polygons:
        p.use_smooth = True
    return o, fr


# ------------------------------------------------------------------------------------------ seams
def make_seams(surf, zfr):
    """Piping round the front and back panels with a stitch row on each side, and stitch rows along
    both lips of the zipper. Piping stays under a millimetre proud: the hands brush the rims."""
    import bp_seamlines as SL
    importlib.reload(SL)
    lines = SL.seam_lines(hull, zfr)
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new('pattern')
    rib = BP.ribbon_profile(1.3 * MM, 0.12 * MM)
    zpts = zfr[1]
    for key, every, seg in (('rim_front', 1, 8), ('rim_back', 2, 6)):
        if key not in lines:
            continue
        loop = lines[key]
        # where the zipper runs along the panel's edge it is the seam: the piping stops there
        near = np.array([np.min(np.linalg.norm(zpts - p, axis=1)) < 10.5 * MM for p in loop])
        if near.all():
            continue
        runs = []
        if not near.any():
            runs.append((loop, True))
        else:
            k0 = int(np.argmax(near))
            ring = np.roll(loop, -k0, axis=0)
            nr = np.roll(near, -k0)
            cur = []
            for p, n_ in zip(ring, nr):
                if n_:
                    if len(cur) > 3:
                        runs.append((np.array(cur), False))
                    cur = []
                else:
                    cur.append(p)
            if len(cur) > 3:
                runs.append((np.array(cur), False))
        for pts_, closed in runs:
            fr = BP.line_frames(pts_, surf, closed=closed)
            f, _ = BP.sweep(bm, fr, BP.tube_profile(SPEC.get('piping', 0.85) * MM, seg), closed=closed,
                            uv_layer=uvl, every=every, surf=surf, taper=4 * MM)
            for x in f:
                x.material_index = 0
            for off in (3.2, -3.2):
                f, _ = BP.sweep(bm, BP.offset_frames(fr, off * MM), rib, closed=closed, uv_layer=uvl, every=every,
                                surf=surf, taper=3 * MM)
                for x in f:
                    x.material_index = 1
    pz = BP.zipper_profile(**SPEC.get('zip_profile', {}))
    half = max(abs(b) for b, _ in pz) / MM + 2.0
    for sgn in (1, -1):
        f, _ = BP.sweep(bm, BP.offset_frames(zfr, sgn * half * MM), rib, uv_layer=uvl, every=3, surf=surf)
        for x in f:
            x.material_index = 1
    o = new_obj('bag_seams', bm)
    slot(o, 'trim')
    slot(o, 'stitch')
    for p in o.data.polygons:
        p.use_smooth = True
    return o


# ------------------------------------------------------------------------------------------ pull
def make_pull():
    bm = BP.pull_mesh(SPEC.get('pull', 'metal'))
    # the car is the last part pull_mesh adds: its faces close the list
    PULL_FACES[0] = len(bm.faces) - BP.CAR_FACES
    o = new_obj('bag_pull', bm)
    # bone frame -> armature space: the zipper bone's bind as Blender holds it (the frame the original
    # tab was measured in: x across, y thickness, z along the tab)
    o.data.transform(arm.data.bones['zipper'].matrix_local)
    for p in o.data.polygons:
        p.use_smooth = True
    return o


# ------------------------------------------------------------------------------------------ materials
def slot(o, name):
    """Index of the bake material `name` on o, adding the slot (placeholder material) when missing."""
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    for i, ms in enumerate(o.data.materials):
        if ms == m:
            return i
    o.data.materials.append(m)
    return len(o.data.materials) - 1


# zipper profile segments: lips, tape, coil (zipper_profile has 15 points)
ZIP_SEG = {**{k: 'trim' for k in (0, 1, 2, 11, 12, 13)}, **{k: 'tape' for k in (3, 4, 9, 10)},
           **{k: 'coil' for k in (5, 6, 7, 8)}}
SWEEP_FACES = [0]
PULL_FACES = [0]


def paint_zipper(o, nseg):
    idx = {k: slot(o, k) for k in ('trim', 'tape', 'coil', 'metal')}
    for f in o.data.polygons:
        if f.index < SWEEP_FACES[0]:
            f.material_index = idx[ZIP_SEG[f.index % nseg]]
        else:
            f.material_index = idx['metal']


# ------------------------------------------------------------------------------------------ skin + export
def skin(objs_body, obj_pull):
    for o in objs_body + [obj_pull]:
        o.parent = arm
        o.matrix_parent_inverse = Matrix.Identity(4)
        o.vertex_groups.clear()
        g = o.vertex_groups.new(name='zipper' if o is obj_pull else 'wpn_body')
        g.add(range(len(o.data.vertices)), 1.0, 'REPLACE')


def join(parts):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    bag = parts[0]
    bag.name = 'bag'
    bag.data.name = 'bag'
    return bag


def unwrap(bag):
    me = bag.data
    if 'pattern' not in me.uv_layers:
        me.uv_layers.new(name='pattern')
    atlas = me.uv_layers.new(name='atlas')
    me.uv_layers.active = atlas
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    bag.select_set(True)
    bpy.context.view_layer.objects.active = bag
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.004, area_weight=0.0,
                             correct_aspect=True, scale_to_bounds=False)
    # the game shows the lid, the zipper and the band under it: their islands get twice the texels
    bm = bmesh.from_edit_mesh(me)
    uvl = bm.loops.layers.uv['atlas']
    seen = set()
    for f0 in bm.faces:
        if f0 in seen:
            continue
        island, stack = [], [f0]
        seen.add(f0)
        while stack:
            f = stack.pop()
            island.append(f)
            for lp in f.loops:
                for other in lp.edge.link_faces:
                    if other in seen:
                        continue
                    # neighbours across an edge whose two ends keep their UVs belong to the same island
                    ol = [l for l in other.loops if l.edge == lp.edge][0]
                    a0, a1 = lp[uvl].uv, lp.link_loop_next[uvl].uv
                    b0, b1 = ol[uvl].uv, ol.link_loop_next[uvl].uv
                    if ((a0 - b1).length < 1e-6 and (a1 - b0).length < 1e-6) or \
                            ((a0 - b0).length < 1e-6 and (a1 - b1).length < 1e-6):
                        seen.add(other)
                        stack.append(other)
        area = sum(f.calc_area() for f in island)
        vis = sum(f.calc_area() for f in island
                  if f.calc_center_median().y > -0.06 and f.normal.z > 0.25)
        if area > 0 and vis / area > 0.5:
            uvs = [lp[uvl] for f in island for lp in f.loops]
            c = sum((u.uv for u in uvs), Vector((0.0, 0.0))) / len(uvs)
            for u in uvs:
                u.uv = c + (u.uv - c) * SPEC.get('visible_texels', 2.0)
    bmesh.update_edit_mesh(me)
    bpy.ops.uv.select_all(action='SELECT')
    bpy.ops.uv.pack_islands(margin=0.003, rotate=True)
    bpy.ops.object.mode_set(mode='OBJECT')
    # the atlas renders and exports; pattern feeds the procedural lookups only
    atlas.active_render = True


def setup_cycles():
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    prefs = bpy.context.preferences.addons['cycles'].preferences
    for kind in ('OPTIX', 'CUDA'):
        try:
            prefs.compute_device_type = kind
            prefs.get_devices()
            gpus = [d for d in prefs.devices if d.type == kind]
        except Exception:
            gpus = []
        if gpus:
            for d in prefs.devices:
                d.use = d.type == kind
            sc.cycles.device = 'GPU'
            print('BAKE device', kind)
            break


def bake(bag, res):
    import bp_material as MAT
    importlib.reload(MAT)
    setup_cycles()
    sc = bpy.context.scene
    imgs = {}
    for p in ('col', 'gloss', 'normal'):
        im = bpy.data.images.new(f'bake_{p}', res, res, alpha=False, float_buffer=(p == 'normal'))
        if p != 'col':
            im.colorspace_settings.name = 'Non-Color'
        imgs[p] = im
    # one procedural material per slot, by the slot's name
    looks = dict(SPEC['looks'])
    mats = []
    for i, ms in enumerate(bag.data.materials):
        if ms.name not in looks and ms.name.startswith('stitch_'):
            k0, p0 = looks[ms.name[len('stitch_'):]]
            looks[ms.name] = ('stitch', dict(p0, thread=p0.get('thread', looks['stitch'][1]['thread'])))
        kind, params = looks[ms.name]
        m = MAT.make('bake_' + ms.name, kind, params, imgs['col'])
        bag.data.materials[i] = m
        mats.append(m)
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    bag.select_set(True)
    bpy.context.view_layer.objects.active = bag
    out = {}
    for p in ('col', 'gloss', 'normal'):
        for m in mats:
            next(n for n in m.node_tree.nodes if n.label == 'bake_target').image = imgs[p]
            MAT.route(m, p)
        sc.cycles.samples = {'col': 48, 'gloss': 4, 'normal': 8}[p]
        if p == 'normal':
            bpy.ops.object.bake(type='NORMAL', normal_space='TANGENT', margin=16, margin_type='EXTEND',
                                uv_layer='atlas')
        else:
            bpy.ops.object.bake(type='EMIT', margin=16, margin_type='EXTEND', uv_layer='atlas')
        path = os.path.join(OUT, f'bake_{p}.' + ('exr' if p == 'normal' else 'png'))
        imgs[p].filepath_raw = path
        imgs[p].file_format = 'OPEN_EXR' if p == 'normal' else 'PNG'
        imgs[p].save()
        out[p] = path
        print('BAKE', p, path)
    return out


def export(bag):
    tex = SPEC.get('texture', 'act\\dar_bag_' + NAME)
    m = bpy.data.materials.new('dar_bag_' + NAME)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    o_ = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    t = nt.nodes.new('ShaderNodeTexImage')
    # texture_name_from_image_path off: the exporter takes the texture name from the node's name
    t.name = tex
    t.label = tex
    t.image = bpy.data.images.get('bake_col') or bpy.data.images.new('dar_bag_dummy', 4, 4)
    nt.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
    nt.links.new(bsdf.outputs[0], o_.inputs['Surface'])
    nt.nodes.active = t
    me = bag.data
    me.materials.clear()
    me.materials.append(m)
    for f in me.polygons:
        f.material_index = 0
    if 'pattern' in me.uv_layers:
        me.uv_layers.remove(me.uv_layers['pattern'])
    me.uv_layers.active = me.uv_layers['atlas']
    mod = bag.modifiers.new('arm', 'ARMATURE')
    mod.object = arm
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    # the original mesh must not ride along
    bpy.data.objects.remove(orig, do_unlink=True)
    bpy.context.view_layer.update()
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    # the addon's initializer resets the shader on first use; set it last
    m.xray.eshader = 'models\\model'
    m.xray.cshader = 'default'
    m.xray.gamemtl = 'default'
    path = os.path.join(OUT, f'dar_bag_{NAME}_hud.ogf')
    res = bpy.ops.xray_export.ogf_file(filepath=path, fmt_version='cscop', export_motions=False, hq_export=True,
                                       use_export_paths=False, texture_name_from_image_path=False)
    print('EXPORT', res, path, os.path.getsize(path) if os.path.exists(path) else None)
    return path


def main():
    body = make_body()
    soften_crease(body)
    surf = BP.Surface(body)
    for p in body.data.polygons:
        p.material_index = slot(body, 'fabric')
    zipper, fr = make_zipper(surf)
    paint_zipper(zipper, len(BP.zipper_profile(**SPEC.get('zip_profile', {}))) - 1)
    seams = make_seams(surf, fr)
    tabs = make_tabs(surf, fr)
    details = VAR.build_details(NAME, SPEC, BP, surf, fr, new_obj, slot)
    pull = make_pull()
    for p in pull.data.polygons:
        p.material_index = slot(pull, 'pull' if p.index < PULL_FACES[0] else 'metal')
    parts = [body, zipper, seams, tabs] + details
    print('BUILD parts', [(o.name, len(o.data.polygons)) for o in parts + [pull]])
    skin(parts, pull)
    bag = join(parts + [pull])
    unwrap(bag)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, f'{NAME}.blend'))
    if '--no-bake' not in args:
        bake(bag, SPEC.get('res', 2048))
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, f'{NAME}.blend'))
    if '--no-export' not in args:
        export(bag)
    print('BUILD_DONE', NAME)


main()
