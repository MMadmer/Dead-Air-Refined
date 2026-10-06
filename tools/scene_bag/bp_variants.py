"""Looks of our scene bags: one per backpack the slot can hold (after the item's inventory icon, which the
player has in front of him while the bag is in the hands), plus the cheap default for no backpack, a slot
locked by the suit or taken by something that is not a backpack (the exoskeleton, the air tanks). Each look names its bake materials
(slot name -> (kind, params)) and the details laid on the free lower front (x -0.05..0.08, y -0.165..0.025; left of x -0.05 the left thumb lies 2 mm off the cloth early in the open
cycle, so nothing there stands more than 2 mm). The game shows the lid above the zipper and the band just
under it: tall pieces start at y 0.025, the lid takes flat ones only (the palm sweeps it, 0.6 mm at most).

Detail entries: ('slab', material, ('rrect', cx, cy, w, h, r), thickness mm) - a sewn or moulded piece;
('strap', material, (x0, x1, y, width), thickness mm) - horizontal webbing; ('vstrap', material,
(x, y0, y1, width), thickness mm) - vertical strap; ('cord', material, (x0, x1, y0, y1, n), radius m)."""
import bmesh


def _common(fabric, trim, tape='#34342a', coil='#25251f', metal='#6b6a64', pull=('metal', {}), thread=None,
            **extra):
    looks = {
        'fabric': ('fabric', fabric),
        # stitches ride on ribbons over the same cloth: the fabric's own look plus the thread
        'stitch': ('stitch', dict(fabric, thread=thread or fabric.get('thread', '#33332a'))),
        'trim': ('fabric', trim),
        'tape': ('tape', dict(color=tape, gloss=0.16, dirt=0.6, wear=0.5, bump=0.25)),
        'coil': ('coil', dict(color=coil, gloss=0.3, pitch=1.75, dirt=0.4, wear=0.3, bump=0.5)),
        'metal': ('metal', dict(color=metal, gloss=0.5, dirt=0.35, wear=1.0)),
        'pull': pull,
    }
    looks.update(extra)
    return looks


def _cloth(color, weave=1.4, kind='plain', gloss=0.12, **kw):
    d = dict(color=color, weave=weave, weave_kind=kind, gloss=gloss, mottle=0.55, dirt=1.0, wear=1.6, bump=0.42,
             fade=0.5, wrinkle=0.75, weave_shade=0.12)
    d.update(kw)
    return d


VARIANTS = {
    # no backpack, the suit locks the slot, or the slot holds a tank / rebreather / belt: cheap swampy canvas
    'default': dict(
        pull='cheap',
        looks=_common(
            fabric=_cloth('#3a3d2e', 1.5, gloss=0.06, mottle=0.65, dirt=1.4, dirt_color='#2b281e', wear=2.0,
                          bump=0.45, fade=0.6),
            trim=_cloth('#35382a', 1.5, gloss=0.06, mottle=0.6, dirt=1.3, dirt_color='#2b281e', wear=0.6),
            metal='#6c6150',
            pull=('metal', dict(color='#4f4537', gloss=0.25, dirt=1.0, dirt_color='#5a3a22', wear=0.8)),
            patch=('fabric', _cloth('#323728', 1.7, gloss=0.05, mottle=0.65, dirt=1.4, dirt_color='#2b281e',
                                    wear=1.0, bump=0.6)),
            patch2=('fabric', _cloth('#3f3d2f', 1.8, gloss=0.05, mottle=0.65, dirt=1.5, dirt_color='#2b281e',
                                     wear=1.0, bump=0.6)),
        ),
        details=[('slab', 'patch', ('rrect', -0.004, -0.006, 0.070, 0.044, 0.005), 0.8),
                 ('slab', 'patch2', ('rrect', 0.036, 0.142, 0.046, 0.032, 0.006), 0.5)],
    ),
    # kit_hunt - the hunter's rucksack: dark grey-green canvas, two leather straps with buckles
    'kit_hunt': dict(
        pull='leather',
        looks=_common(
            fabric=_cloth('#383d34', 1.4, gloss=0.08, dirt_color='#2c291f'),
            trim=_cloth('#33372f', 1.3, gloss=0.08, dirt_color='#2c291f'),
            metal='#55534c',
            thread='#2b2e24',
            pull=('leather', dict(color='#2c2620', gloss=0.28, dirt=0.6, wear=1.0)),
            leather=('leather', dict(color='#2c2620', gloss=0.3, dirt=0.7, wear=1.2, bump=0.5)),
        ),
        details=[('vstrap', 'leather', (-0.038, -0.163, 0.026, 0.020), 1.8),
                 ('vstrap', 'leather', (0.050, -0.163, 0.026, 0.020), 1.8),
                 ('slab', 'metal', ('rrect', -0.038, -0.004, 0.026, 0.022, 0.003), 1.0),
                 ('slab', 'metal', ('rrect', 0.050, -0.004, 0.026, 0.022, 0.003), 1.0),
                 ('slab', 'leather', ('rrect', 0.0, 0.146, 0.052, 0.022, 0.004), 0.6)],
    ),
    # backpack_light - the hiking daypack: pale sage ripstop, a bungee cord laced over the front
    'backpack_light': dict(
        pull='cord',
        looks=_common(
            fabric=_cloth('#5f645a', 1.0, 'ripstop', gloss=0.16, mottle=0.4, dirt=0.9, dirt_color='#3d3c32',
                          fade=0.4),
            trim=_cloth('#565b51', 0.9, gloss=0.16, mottle=0.4, dirt=0.8, dirt_color='#3d3c32'),
            tape='#3a3f33',
            thread='#5b6150',
            pull=('cord', dict(color='#1d1e1b', gloss=0.25, dirt=0.25, dirt_color='#2a2a26', wear=0.3)),
            cord=('cord', dict(color='#1d1e1b', gloss=0.25, dirt=0.25, dirt_color='#2a2a26', wear=0.3)),
        ),
        # the cord, then a cloth loop sewn over every turn of it
        details=[('cord', 'cord', (-0.046, 0.066, -0.085, 0.020, 6), 1.5 * 0.001)]
        + [('slab', 'trim', ('rrect', -0.046 + 0.112 * k / 6, 0.020 if k % 2 == 0 else -0.085, 0.010, 0.008, 0.002), 1.2)
           for k in range(7)]
        + [('slab', 'trim', ('rrect', 0.0, 0.150, 0.084, 0.012, 0.003), 0.5)],
    ),
    # backpack_heavy - the tactical pack: near-black green cordura, dark mottled camouflage, MOLLE rows
    'backpack_heavy': dict(
        pull='rubber',
        looks=_common(
            fabric=_cloth('#30342a', 1.1, gloss=0.14, mottle=0.45, dirt=0.9, wear=1.4,
                          camo=['#282b23', '#393e2f'], camo_scale=19.0, camo_pos=[0.45, 0.6]),
            trim=_cloth('#2c3027', 1.0, gloss=0.14, mottle=0.4, dirt=0.8),
            thread='#24271f',
            pull=('rubber', dict(color='#232426', gloss=0.25, dirt=0.4, wear=0.5)),
            webbing=('webbing', dict(color='#2f3428', gloss=0.2, dirt=0.6, wear=1.0)),
            velcro=('patch', dict(color='#2b2f25', gloss=0.05, dirt=0.5, wear=0.6, bump=0.9)),
        ),
        details=[('strap', 'webbing', (-0.074, 0.074, 0.008, 0.025), 1.4),
                 ('strap', 'webbing', (-0.074, 0.074, -0.032, 0.025), 1.4),
                 ('strap', 'webbing', (-0.074, 0.074, -0.072, 0.025), 1.4),
                 ('strap', 'webbing', (-0.074, 0.074, -0.112, 0.025), 1.4),
                 ('slab', 'velcro', ('rrect', 0.0, 0.145, 0.076, 0.044, 0.004), 0.6)],
    ),
    # backpack_travel - the "Yermak" tourist pack: khaki canvas, a sewn-on front pocket under two
    # compression straps with plastic buckles
    'backpack_travel': dict(
        pull='metal',
        looks=_common(
            fabric=_cloth('#54513b', 1.4, gloss=0.08, dirt_color='#3a3424'),
            trim=_cloth('#4c4936', 1.3, gloss=0.08, dirt_color='#3a3424'),
            metal='#77756c',
            thread='#4a4527',
            pull=('metal', dict(color='#6f6d64', gloss=0.45, dirt=0.6, wear=0.8)),
            pocket=('fabric', _cloth('#4f4c38', 1.4, gloss=0.08, dirt_color='#3a3424')),
            webbing=('webbing', dict(color='#3d3a27', gloss=0.2, dirt=0.7, wear=1.0)),
            plastic=('plastic', dict(color='#262622', gloss=0.3, dirt=0.5, wear=0.6)),
            label=('leather', dict(color='#2e2721', gloss=0.26, dirt=0.8, wear=1.0, bump=0.5)),
        ),
        details=[('slab', 'pocket', ('rrect', 0.009, -0.060, 0.112, 0.150, 0.010), 3.2),
                 ('strap', 'webbing', (-0.076, 0.076, -0.005, 0.020), 1.4),
                 ('strap', 'webbing', (-0.076, 0.076, -0.075, 0.020), 1.4),
                 ('slab', 'plastic', ('rrect', 0.040, -0.005, 0.022, 0.026, 0.004), 3.0),
                 ('slab', 'plastic', ('rrect', 0.040, -0.075, 0.022, 0.026, 0.004), 3.0),
                 ('slab', 'label', ('rrect', 0.0, 0.146, 0.044, 0.018, 0.003), 0.6)],
    ),
    # backpack_stalker - the stalker's pack: faded grey-green, a big sewn front pocket and a patch
    'backpack_stalker': dict(
        pull='metal',
        looks=_common(
            fabric=_cloth('#474c42', 1.3, gloss=0.08, dirt=1.1, dirt_color='#33312a', fade=0.55),
            trim=_cloth('#41463d', 1.2, gloss=0.08, dirt=1.0, dirt_color='#33312a'),
            metal='#6e6d66',
            thread='#3d4236',
            pull=('metal', dict(color='#5d5c56', gloss=0.4, dirt=0.7, wear=0.8)),
            pocket=('fabric', _cloth('#42473d', 1.3, gloss=0.08, dirt=1.2, dirt_color='#33312a')),
            patch=('fabric', _cloth('#3e4237', 0.6, gloss=0.05, mottle=0.2, dirt=0.6, bump=0.9)),
            patch2=('fabric', _cloth('#38362d', 0.7, gloss=0.05, mottle=0.35, dirt=0.9, bump=0.7)),
        ),
        details=[('slab', 'pocket', ('rrect', 0.009, -0.060, 0.116, 0.155, 0.012), 3.4),
                 ('slab', 'patch', ('rrect', 0.009, -0.018, 0.060, 0.030, 0.004), 0.8),
                 ('slab', 'patch2', ('rrect', 0.0, 0.146, 0.042, 0.042, 0.018), 0.6)],
    ),
}


def build_details(name, spec, BP, surf, fr, new_obj, slot):
    import importlib

    import bpy

    import bp_details as D
    importlib.reload(D)
    items = spec.get('details', [])
    if not items:
        return []
    body = bpy.data.objects['bag_body']
    bvh = D.Stack(D.surface_bvh(body))
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new('pattern')
    order = []

    def mi(mat):
        if mat not in order:
            order.append(mat)
        return order.index(mat)

    # pieces stack: each one is dropped onto what lies under it (the bag, or a pocket under a strap)
    for kind, mat, shape, thick in items:
        if kind == 'slab':
            _, cx, cy, w, h, r = shape
            D.slab(bm, surf, bvh, D.rounded_rect(cx, cy, w, h, r), thick * 0.001, mat=mi(mat), uvl=uvl)
            # cloth and leather pieces are sewn on: a stitch row 2.8 mm in from the edge
            if mat not in ('metal', 'plastic') and min(w, h) > 0.012:
                ins = 0.0028
                D.stitch_ring_front(bm, BP, surf, bvh,
                                    D.rounded_rect(cx, cy, w - 2 * ins, h - 2 * ins, max(r - ins, 0.001)),
                                    thick * 0.001 + 0.00012, mi('stitch_' + mat), uvl)
        elif kind == 'strap':
            x0, x1, y, w = shape
            D.strap(bm, surf, bvh, x0, x1, y, w, thick * 0.001, mi(mat), uvl=uvl)
        elif kind == 'vstrap':
            x, y0, y1, w = shape
            D.slab(bm, surf, bvh, D.rounded_rect(x, (y0 + y1) / 2, w, abs(y1 - y0), 0.0015), thick * 0.001,
                   res=0.0025, mat=mi(mat), uvl=uvl, edge_round=0.4 * 0.001)
        elif kind == 'cord':
            x0, x1, y0, y1, n = shape
            D.cord_zigzag(bm, surf, bvh, x0, x1, y0, y1, n, thick, mi(mat))
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bvh.add(bm)
    o = new_obj('bag_details', bm)
    for m in order:
        slot(o, m)
    for p in o.data.polygons:
        p.use_smooth = True
    return [o]
