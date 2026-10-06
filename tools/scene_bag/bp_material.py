"""Procedural looks of the scene bags for the Cycles bake (run inside Blender). Every material carries the
same target image node, so one bake pass fills the whole atlas. Outputs per pass: 'col' (albedo with the
grime), 'gloss', and the shading normal (weave, seams, wrinkles) for the normal bake.

Kinds: fabric (body, flaps, tabs), webbing, tape (zipper tape), coil, metal, plastic, rubber, leather, cord,
patch (velcro / printed patch)."""
import math

import bpy


class NB:
    def __init__(self, mat):
        self.nt = mat.node_tree
        self.x = 0

    def n(self, kind, **kw):
        node = self.nt.nodes.new(kind)
        node.location = (self.x, 0)
        self.x += 180
        for k, v in kw.items():
            if k.startswith('in_'):
                key = k[3:]
                node.inputs[int(key) if key.isdigit() else key].default_value = v
            else:
                setattr(node, k, v)
        return node

    def link(self, a, b):
        self.nt.links.new(a, b)

    def math(self, op, a, b=None, c=None, clamp=False):
        m = self.n('ShaderNodeMath', operation=op, use_clamp=clamp)
        self._in(m.inputs[0], a)
        if b is not None:
            self._in(m.inputs[1], b)
        if c is not None:
            self._in(m.inputs[2], c)
        return m.outputs[0]

    def _in(self, sock, v):
        if isinstance(v, (int, float)):
            sock.default_value = v
        else:
            self.link(v, sock)

    def rgb(self, c):
        n = self.n('ShaderNodeRGB')
        n.outputs[0].default_value = (*c, 1.0)
        return n.outputs[0]

    def mix(self, fac, a, b, blend='MIX'):
        m = self.n('ShaderNodeMix', data_type='RGBA', blend_type=blend)
        self._in(m.inputs['Factor'], fac)
        self._in(m.inputs[6], a) if not isinstance(a, tuple) else setattr(m.inputs[6], 'default_value', (*a, 1))
        self._in(m.inputs[7], b) if not isinstance(b, tuple) else setattr(m.inputs[7], 'default_value', (*b, 1))
        return m.outputs[2]

    def mixf(self, fac, a, b):
        m = self.n('ShaderNodeMix', data_type='FLOAT')
        self._in(m.inputs['Factor'], fac)
        self._in(m.inputs[2], a)
        self._in(m.inputs[3], b)
        return m.outputs[0]


def _srgb(c):
    """Hex or 0-255 sRGB -> linear floats."""
    if isinstance(c, str):
        c = tuple(int(c[i:i + 2], 16) for i in (1, 3, 5))
    out = []
    for v in c:
        v = v / 255.0
        out.append(v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4)
    return tuple(out)


def _weave(b, co, period, kind):
    """Height 0..1 of a weave in the plane coordinates (u, v): plain (canvas), cordura (fine, irregular),
    ripstop (fine + grid). Real cloth threads are half a millimetre: at arm's length the weave melts into
    fine grain, and the cloth reads through its slubs and fibre streaks, so the look's weave figure is
    scaled down to that and the streaks carry the character."""
    period = period * 0.45
    # threads wander a little: a low noise bends the grid
    wob = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=60.0, in_Detail=2.0)
    b.link(co, wob.inputs['Vector'])
    off = b.n('ShaderNodeVectorMath', operation='SCALE')
    sub =b.n('ShaderNodeVectorMath', operation='SUBTRACT')
    b.link(wob.outputs['Color'], sub.inputs[0])
    sub.inputs[1].default_value = (0.5, 0.5, 0.5)
    b.link(sub.outputs[0], off.inputs[0])
    off.inputs['Scale'].default_value = period * 0.6
    co2 = b.n('ShaderNodeVectorMath', operation='ADD')
    b.link(co, co2.inputs[0])
    b.link(off.outputs[0], co2.inputs[1])
    sep = b.n('ShaderNodeSeparateXYZ')
    b.link(co2.outputs[0], sep.inputs[0])
    pu = b.math('DIVIDE', sep.outputs[0], period)
    pv = b.math('DIVIDE', sep.outputs[1], period)
    # thread profiles across each set: warp runs along u (rows in v), weft along v
    warp = b.math('POWER', b.math('ABSOLUTE', b.math('SINE', b.math('MULTIPLY', pv, math.pi))), 0.6)
    weft = b.math('POWER', b.math('ABSOLUTE', b.math('SINE', b.math('MULTIPLY', pu, math.pi))), 0.6)

    # slubs: every thread thickens and thins along its length (noise stretched along the thread, one row
    # of it per thread) - no two cells alike, so the cloth reads as cloth and not as a grid
    def slub(along, across):
        c = b.n('ShaderNodeCombineXYZ')
        b.link(b.math('MULTIPLY', along, 0.12), c.inputs[0])
        b.link(b.math('FLOOR', across), c.inputs[1])
        nz = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=1.0, in_Detail=2.0)
        b.link(c.outputs[0], nz.inputs['Vector'])
        return b.math('MULTIPLY_ADD', nz.outputs['Fac'], 1.2, 0.35)

    h = b.math('MAXIMUM', b.math('MULTIPLY', warp, slub(pu, pv)), b.math('MULTIPLY', weft, slub(pv, pu)))
    h = b.math('MULTIPLY', h, 0.45)

    # fibre streaks: thin, a few millimetres long, along both thread sets
    def streak(sx, sy):
        c = b.n('ShaderNodeCombineXYZ')
        b.link(b.math('MULTIPLY', sep.outputs[0], sx), c.inputs[0])
        b.link(b.math('MULTIPLY', sep.outputs[1], sy), c.inputs[1])
        nz = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=1.0, in_Detail=3.0, in_Roughness=0.6)
        b.link(c.outputs[0], nz.inputs['Vector'])
        return nz.outputs['Fac']

    h = b.math('ADD', h, b.math('MULTIPLY', b.math('ADD', streak(260.0, 1400.0), streak(1400.0, 260.0)), 0.32))
    if kind == 'ripstop':
        g = 7.0
        gu = b.math('PINGPONG', b.math('DIVIDE', sep.outputs[0], period * g), 0.5)
        gv = b.math('PINGPONG', b.math('DIVIDE', sep.outputs[1], period * g), 0.5)
        line = b.math('MAXIMUM', b.math('GREATER_THAN', gu, 0.46), b.math('GREATER_THAN', gv, 0.46))
        h = b.math('ADD', b.math('MULTIPLY', h, 0.6), b.math('MULTIPLY', line, 0.5))
    return h


def _triplanar(b, fn):
    """fn(coord_socket) on the three object-space planes, blended by the normal."""
    tc = b.n('ShaderNodeTexCoord')
    geo = b.n('ShaderNodeNewGeometry')
    obj = tc.outputs['Object']
    sep = b.n('ShaderNodeSeparateXYZ')
    b.link(obj, sep.inputs[0])
    nsep = b.n('ShaderNodeSeparateXYZ')
    b.link(geo.outputs['Normal'], nsep.inputs[0])
    ws = [b.math('POWER', b.math('ABSOLUTE', nsep.outputs[i]), 4.0) for i in range(3)]
    tot = b.math('ADD', b.math('ADD', ws[0], ws[1]), ws[2])
    planes = [(1, 2), (0, 2), (0, 1)]
    acc = None
    for i, (a, c) in enumerate(planes):
        comb = b.n('ShaderNodeCombineXYZ')
        b.link(sep.outputs[a], comb.inputs[0])
        b.link(sep.outputs[c], comb.inputs[1])
        val = fn(comb.outputs[0])
        term = b.math('MULTIPLY', val, b.math('DIVIDE', ws[i], tot))
        acc = term if acc is None else b.math('ADD', acc, term)
    return acc


def make(name, kind, p, image):
    """Material `name` of `kind` with parameters p; image = the bake target. Returns the material; its node
    tree has custom outputs reachable by label: 'out_col', 'out_gloss', 'out_height'."""
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    b = NB(mat)
    tc = b.n('ShaderNodeTexCoord')
    geo = b.n('ShaderNodeNewGeometry')
    uvp = b.n('ShaderNodeUVMap', uv_map='pattern')
    base = _srgb(p.get('color', '#4a4a3a'))
    col = b.rgb(base)
    # the game's own fabric reads matte (gloss about 8/255 in the bump): scale the look's gloss per kind
    gloss = p.get('gloss', 0.18) * {'fabric': 0.4, 'stitch': 0.4, 'tape': 0.4, 'webbing': 0.4, 'patch': 0.4,
                                   'leather': 0.5, 'rubber': 0.45, 'plastic': 0.6, 'cord': 0.5, 'coil': 0.8}.get(kind, 1.0)
    height = None

    if kind in ('fabric', 'tape', 'webbing', 'stitch'):
        period = p.get('weave', 1.4) * 0.001
        wk = p.get('weave_kind', 'plain')
        if kind in ('fabric', 'stitch'):
            height = _triplanar(b, lambda co: _weave(b, co, period, wk))
        elif kind == 'tape':
            height = _weave(b, uvp.outputs[0], 0.0009, 'plain')
        else:
            # webbing: twill ribs across the strap (pattern v is across)
            sep = b.n('ShaderNodeSeparateXYZ')
            b.link(uvp.outputs[0], sep.inputs[0])
            rib = b.math('SINE', b.math('MULTIPLY', b.math('ADD', sep.outputs[0], b.math('MULTIPLY', sep.outputs[1], 0.004)), 2 * math.pi / 0.0011))
            height = b.math('MULTIPLY_ADD', rib, 0.5, 0.5)
            # bar tacks across the strap every MOLLE pitch: a sunken stitched band
            tack = b.math('LESS_THAN', b.math('FRACT', b.math('DIVIDE', sep.outputs[0], p.get('tack', 0.038))), 0.06)
            height = b.math('SUBTRACT', height, b.math('MULTIPLY', tack, 0.9))
            col = b.mix(b.math('MULTIPLY', tack, 0.7), col, tuple(v * 0.45 for v in base))
        # camouflage: thresholded object-space noise, 2-3 extra colours
        camo = p.get('camo')
        if camo and kind in ('fabric', 'stitch'):
            noise = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=p.get('camo_scale', 9.0),
                        in_Detail=3.0, in_Roughness=0.55)
            b.link(tc.outputs['Object'], noise.inputs['Vector'])
            ramp = b.n('ShaderNodeValToRGB')
            cr = ramp.color_ramp
            cr.interpolation = 'CONSTANT'
            stops = [base] + [_srgb(c) for c in camo]
            cr.elements[0].position = 0.0
            cr.elements[0].color = (*stops[0], 1)
            cr.elements[1].position = 0.999
            cr.elements[1].color = (*stops[-1], 1)
            pos = p.get('camo_pos', [0.48, 0.56, 0.63][:len(stops) - 1])
            for k, c in enumerate(stops[1:]):
                e = cr.elements.new(pos[k])
                e.color = (*c, 1)
            b.link(noise.outputs['Fac'], ramp.inputs[0])
            col = ramp.outputs[0]
        # tonal mottling and the weave's own shading
        mott = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=38.0, in_Detail=6.0, in_Roughness=0.6)
        b.link(tc.outputs['Object'], mott.inputs['Vector'])
        m1 = b.math('MULTIPLY_ADD', mott.outputs['Fac'], p.get('mottle', 0.35), 1.0 - p.get('mottle', 0.35) * 0.5)
        hsv = b.n('ShaderNodeHueSaturation', in_Hue=0.5, in_Saturation=1.0)
        b.link(m1, hsv.inputs['Value'])
        b.link(col, hsv.inputs['Color'])
        col = hsv.outputs[0]
        # sun-faded and wet-darkened patches over the whole bag
        fade = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=5.0, in_Detail=2.0)
        b.link(tc.outputs['Object'], fade.inputs['Vector'])
        fv = b.math('MULTIPLY_ADD', fade.outputs['Fac'], p.get('fade', 0.3), 1.0 - p.get('fade', 0.3) * 0.5)
        fs = b.math('MULTIPLY_ADD', fade.outputs['Fac'], -p.get('fade', 0.3), 1.0 + p.get('fade', 0.3) * 0.4)
        fh = b.n('ShaderNodeHueSaturation', in_Hue=0.5)
        b.link(fv, fh.inputs['Value'])
        b.link(fs, fh.inputs['Saturation'])
        b.link(col, fh.inputs['Color'])
        col = fh.outputs[0]
        wshade = b.math('MULTIPLY_ADD', height, p.get('weave_shade', 0.07), 1.0 - p.get('weave_shade', 0.07) * 0.6)
        cv = b.n('ShaderNodeHueSaturation', in_Hue=0.5, in_Saturation=1.0)
        b.link(wshade, cv.inputs['Value'])
        b.link(col, cv.inputs['Color'])
        col = cv.outputs[0]
        if kind == 'stitch':
            # thread dashes down the ribbon (pattern u along, v across)
            sep = b.n('ShaderNodeSeparateXYZ')
            b.link(uvp.outputs[0], sep.inputs[0])
            dash = b.math('LESS_THAN', b.math('FRACT', b.math('DIVIDE', sep.outputs[0], p.get('stitch_pitch', 3.4) * 0.001)), 0.62)
            across = b.math('LESS_THAN', b.math('ABSOLUTE', b.math('SUBTRACT', sep.outputs[1], 0.5)), 0.34)
            thread = b.math('MULTIPLY', dash, across)
            col = b.mix(thread, col, _srgb(p.get('thread', '#3a3a2c')))
            height = b.math('MAXIMUM', b.math('MULTIPLY', height, 0.5), b.math('MULTIPLY', thread, 1.2))
    elif kind == 'coil':
        sep = b.n('ShaderNodeSeparateXYZ')
        b.link(uvp.outputs[0], sep.inputs[0])
        teeth = b.math('PINGPONG', b.math('DIVIDE', sep.outputs[0], p.get('pitch', 1.8) * 0.001), 0.5)
        height = b.math('MULTIPLY', teeth, 2.0)
        col = b.mix(b.math('MULTIPLY', teeth, 1.2), col, tuple(v * 0.55 for v in base))
    elif kind in ('metal', 'plastic', 'rubber', 'leather', 'cord'):
        noise = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale={'metal': 900.0, 'plastic': 1400.0,
                    'rubber': 1800.0, 'leather': 260.0, 'cord': 600.0}[kind], in_Detail=4.0)
        b.link(tc.outputs['Object'], noise.inputs['Vector'])
        height = noise.outputs['Fac']
        if kind == 'leather':
            vor = b.n('ShaderNodeTexVoronoi', in_Scale=900.0)
            b.link(tc.outputs['Object'], vor.inputs['Vector'])
            height = b.math('ADD', b.math('MULTIPLY', vor.outputs['Distance'], 0.8), b.math('MULTIPLY', height, 0.3))
        if kind == 'cord':
            sep = b.n('ShaderNodeSeparateXYZ')
            b.link(tc.outputs['Object'], sep.inputs[0])
            spiral = b.math('SINE', b.math('MULTIPLY', b.math('ADD', sep.outputs[2], b.math('MULTIPLY', sep.outputs[0], 0.6)), 2 * math.pi / 0.0012))
            height = b.math('MULTIPLY_ADD', spiral, 0.5, 0.5)
    elif kind == 'patch':
        noise = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=2600.0, in_Detail=2.0)
        b.link(tc.outputs['Object'], noise.inputs['Vector'])
        height = noise.outputs['Fac']

    # grime: occlusion and low noise darken, worn edges (pointiness) lighten and desaturate
    ao = b.n('ShaderNodeAmbientOcclusion', samples=16, in_Distance=0.012)
    occl = b.math('POWER', ao.outputs['AO'], p.get('ao_pow', 1.4))
    dirt = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=14.0, in_Detail=4.0, in_Roughness=0.65)
    b.link(tc.outputs['Object'], dirt.inputs['Vector'])
    dmask = b.math('MULTIPLY', b.math('SUBTRACT', dirt.outputs['Fac'], 0.45, clamp=True), p.get('dirt', 0.9), clamp=True)
    dirty = b.mix(dmask, col, _srgb(p.get('dirt_color', '#3b3426')))
    worn = b.math('MULTIPLY', b.math('SUBTRACT', geo.outputs['Pointiness'], 0.52, clamp=True), p.get('wear', 3.0), clamp=True)
    edge = b.mix(worn, dirty, b.mix(0.3, dirty, (0.62, 0.6, 0.55)))
    final = b.mix(1.0, edge, b.math('MULTIPLY_ADD', occl, 0.75, 0.25), 'MULTIPLY')

    out_col = b.n('ShaderNodeEmission')
    out_col.label = 'out_col'
    b.link(final, out_col.inputs['Color'])
    g = b.n('ShaderNodeValue')
    g.outputs[0].default_value = gloss
    gm = b.math('MULTIPLY', g.outputs[0], b.math('SUBTRACT', 1.0, b.math('MULTIPLY', worn, 0.3)))
    out_gloss = b.n('ShaderNodeEmission')
    out_gloss.label = 'out_gloss'
    b.link(gm, out_gloss.inputs['Color'])
    # cloth relief as strong as the game's own bags (their normals tilt about 40 degrees on average)
    cloth = kind in ('fabric', 'stitch', 'tape', 'webbing', 'patch')
    bump = b.n('ShaderNodeBump', in_Strength=min(1.0, p.get('bump', 0.3) * (2.4 if cloth else 1.5)),
               in_Distance=p.get('bump_dist', 0.0013 if cloth else 0.0006))
    if height is not None:
        b.link(height, bump.inputs['Height'])
    if kind in ('fabric', 'stitch') and p.get('wrinkle', 0.45) > 0:
        # cloth over a stuffed bag: soft dents and creases, centimetres wide, under the weave
        wn = b.n('ShaderNodeTexNoise', noise_dimensions='3D', in_Scale=32.0, in_Detail=3.0, in_Roughness=0.5,
                 in_Distortion=0.6)
        b.link(tc.outputs['Object'], wn.inputs['Vector'])
        wv = b.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='DIAGONAL', in_Scale=9.0,
                 in_Distortion=14.0, in_Detail=2.0)
        b.link(tc.outputs['Object'], wv.inputs['Vector'])
        wr = b.math('ADD', wn.outputs['Fac'], b.math('MULTIPLY', wv.outputs['Fac'], 0.08))
        wb = b.n('ShaderNodeBump', in_Strength=min(1.0, p.get('wrinkle', 0.45) * 1.3), in_Distance=0.004)
        b.link(wr, wb.inputs['Height'])
        b.link(wb.outputs['Normal'], bump.inputs['Normal'])
    diff = b.n('ShaderNodeBsdfDiffuse')
    diff.label = 'out_normal'
    b.link(bump.outputs['Normal'], diff.inputs['Normal'])
    out = b.n('ShaderNodeOutputMaterial', target='CYCLES')
    out.label = 'out'
    tex = b.n('ShaderNodeTexImage')
    tex.label = 'bake_target'
    tex.image = image
    nt.nodes.active = tex
    return mat


def route(mat, what):
    """Points the material output at one bake pass: 'col', 'gloss' (emission) or 'normal' (diffuse)."""
    nt = mat.node_tree
    out = next(n for n in nt.nodes if n.label == 'out')
    for lk in list(out.inputs['Surface'].links):
        nt.links.remove(lk)
    src = next(n for n in nt.nodes if n.label == {'col': 'out_col', 'gloss': 'out_gloss', 'normal': 'out_normal'}[what])
    nt.links.new(src.outputs[0], out.inputs['Surface'])
    nt.nodes.active = next(n for n in nt.nodes if n.label == 'bake_target')
