# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Builds the shrubs' and the trees' texture arrays from the generators' layers (docs/dead-air/SHRUBS.md,
docs/dead-air/TREES_TEXTURES.md).

make_leaf_textures.py and make_bark_textures.py write PNG layers and their manifests; this script
- fits each species' near leaf hull (r3/da_shrub_leaf_vs.h: four rows of a pair) to the coverage of its leaf
  layers a and b together and writes leaf_hulls.json (make_shrubs.py puts it into the data),
- compresses each layer to BC7 with a full mip chain (the leaves' coverage kept at the alpha test's 0.5 in every
  mip, the bark's mips filtered as the tile wraps) and assembles the layers in manifest order into texture arrays;
  a layer's compressed slice is cached under the hash of its PNG, the encoder's arguments and texconv.exe
  (build/arrays/cache), so only the changed layers are encoded again, and the shrubs' slices are checked against
  the ones pinned at the first build (build/arrays/shrub_slices.json: the shrub data references them),
- writes da/shrub_leaf_d, shrub_leaf_n, shrub_bark_d and shrub_bark_n.dds into the asset source (Dead-Air-Refined_Assets),
- writes out/tree_layers.json: each tree species' layers, hull and squares over its elements (the keys
  tools/trees/make_trees.py writes into its species, as make_shrubs.py does for the shrubs), the trees' layers'
  coverage and bark means.

usage: build_arrays.py [--tools <DirectXTex bin>] [--src <dir with the manifests and build/>] [--repin-shrubs]
"""
import argparse
import hashlib
import json
import math
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
# Textures are content: they go to the asset source (Dead-Air-Refined_Assets), not into the repository.
ASSETS = os.environ.get('DAR_ASSETS_GAMEDATA', os.path.join(REPO, '..', '..', 'Dead-Air-Refined_Assets', 'source', 'gamedata'))
OUT = os.path.join(ASSETS, 'textures', 'da')
WORK = os.path.join(HERE, 'build', 'arrays')
CACHE = os.path.join(WORK, 'cache')
PINS = os.path.join(WORK, 'shrub_slices.json')
TOOLS = os.path.join(os.environ.get('DAR_ASSET_TOOLS', r'D:\Tools\asset-pipeline'), 'dxtex-build', 'bin')


SRC = HERE


def load(name):
    return load_json(os.path.join(SRC, name))


def load_json(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def write_text(path, text):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(text)
    os.replace(tmp, path)


def half_widths(alpha):
    """Per row (v), the half width of the covered texels about the midrib, in texture-width units (0 = none)."""
    h, w = alpha.shape
    cols = (np.arange(w) + 0.5) / w - 0.5
    covered = alpha > 1
    hw = np.where(covered, np.abs(cols)[None, :] + 0.5 / w, 0.0).max(axis=1)
    return hw


def fit_hull(hw):
    """The smallest hull of four rows (r3/da_shrub_leaf_vs.h: v 0, v1, v2, 1, each a pair) that holds every covered row.

    The hull's half width is piecewise linear through (0, a0), (v1, a1), (v2, a2) and (1, a3); returns the full
    widths and the places: [w0, v1, w1, v2, w2, w3].
    """
    n = len(hw)
    v = (np.arange(n) + 0.5) / n
    hs = np.where(hw > 0.0, hw + 2.0 / n, 0.0)
    grid = np.arange(2, 31) / 32.0
    best = None
    for i1, v1 in enumerate(grid):
        for i2 in range(i1 + 2, len(grid)):
            v2 = grid[i2]
            knots = np.array([0.0, v1, v2, 1.0])
            a = np.array([hs[v <= 0.02].max(initial=0.0), 0.0, 0.0, hs[v >= 0.98].max(initial=0.0)])
            # The smallest heights: raise a segment's two ends by its worst deficit until every row clears.
            for _ in range(12):
                ok = True
                for s in range(3):
                    m = (v >= knots[s]) & (v <= knots[s + 1])
                    t = (v[m] - knots[s]) / (knots[s + 1] - knots[s])
                    deficit = (hs[m] - (a[s] * (1.0 - t) + a[s + 1] * t)).max(initial=0.0)
                    if deficit > 1e-6:
                        ok = False
                        a[s] += deficit
                        a[s + 1] += deficit
                if ok:
                    break
            area = np.trapezoid(a, knots)
            # (A width over the square's would be clamped and cut the leaf's edge: such a candidate is out.)
            if ok and a.max() <= 0.5 + 1e-6 and (best is None or area < best[0]):
                best = (area, [2 * a[0], v1, 2 * a[1], v2, 2 * a[2], 2 * a[3]])
    if best is None:
        return [1.0, 0.3333, 1.0, 0.6667, 1.0, 1.0]
    return [round(min(x, 1.0), 4) if k in (0, 2, 4, 5) else round(x, 4) for k, x in enumerate(best[1])]


def hulls(manifest):
    out = {}
    layers = manifest['layers']
    for sid, e in sorted(manifest['species'].items()):
        idx = sorted({e['a'], e.get('b', e['a'])})
        if any(layers[i].get('kind') != 'simple' for i in idx):
            continue
        hw = None
        for i in idx:
            a = np.asarray(Image.open(os.path.join(SRC, 'build', 'leaf', layers[i]['name'] + '_d.png')))[..., 3]
            h = half_widths(a)
            hw = h if hw is None else np.maximum(hw, h)
        out[sid] = fit_hull(hw)
    return out


def mean(v):
    return 0.5 * (v[0] + v[1])


def tree_layers(leaf, bark, hull, cover, bark_mean):
    """The trees' texture binding for tools/trees/make_trees.py (texture_lines of make_shrubs.py for the trees): per
    species its leaf layers (a, b, the spray card, a compound leaf's leaflet, a conifer's needle tuft), its bark and
    upper bark with their tiles' scales, the near leaf's hull, the textures' squares over its elements (leaf_fit:
    length, width, start behind the element's base, the clusters' layer's coverage; card_fit the same for the spray
    card), the element sizes these are reckoned for; the trees' layers' coverage and bark means."""
    sys.path.insert(0, HERE)
    import tree_leaf_botany as TB
    layers = leaf['layers']
    types = bark['types']
    box = lambda i: [x / 1000.0 for x in layers[i]['box_mm']] if 0 <= i < len(layers) else None
    out_sp = {}
    for sid in TB.ORDER:
        src = TB.SHARED.get(sid, sid)
        e = leaf['species'].get(sid) or leaf['species'].get(src)
        if not e:
            continue
        a_ = (TB.TREES.get(src) or {}).get('arch')
        if a_ is None:
            sys.path.insert(0, os.path.dirname(HERE))
            import make_shrubs as MS
            a_ = MS.SP[src]['arch']
        kind = a_.get('leaf_kind', 'simple')
        a, b, card, leaflet = e['a'], e.get('b', e['a']), e.get('card', -1), e.get('leaflet', -1)
        pet = a_.get('petiole', 0.01)
        ll = mean(a_['leaf_len']) if a_.get('leaf_len') else 0.05
        lw = mean(a_['leaf_wid']) if a_.get('leaf_wid') else 0.03
        fit, elem = [1.0, 1.0, 0.0], {}
        if box(a):
            bl, bw = box(a)
            if kind == 'compound':
                n = max(round(a_.get('leaflets', 1)), 1)
                le = a_.get('rachis', 0.05) + ll * (1.0 if n % 2 else 0.5)
                we = max(2.0 * ll * math.sin(math.radians(a_.get('leaflet_angle', 60))), lw)
                fit = [bl / le, bw / we, (0.006 * bl + pet) / le]
            elif kind == 'palmate':
                # (The leaflets fan from the petiole's tip: the element is the middle leaflet's length and the fan's
                # breadth, about 1.15 of it for the horse chestnut's seven - make_leaf_textures.py PALMATE.)
                le = ll
                we = max(1.15 * ll, lw)
                fit = [bl / le, bw / we, (0.006 * bl + pet) / le]
            elif kind == 'needles':
                le, we = a_.get('leaf_spacing', 0.05) * 1.1, 2.0 * a_.get('needle_len', 0.014)
                fit = [bl / le, bw / we, 0.006 * bl / le]
            else:
                le, we = ll, lw
                fit = [bl / ll, bw / lw, ((0.016 if layers[a]['kind'] == 'simple' else 0.006) * bl + pet) / ll]
            elem = dict(length=round(le, 4), width=round(we, 4))
        cl = leaflet if leaflet >= 0 else a
        cov = cover.get(layers[cl]['name'], 0.0) if 0 <= cl < len(layers) else 0.0
        card_fit, card_elem = [1.0, 1.0, 0.0], {}
        if box(card):
            cb, cw = box(card)
            if a_.get('spray_len'):
                splay = math.radians(mean(a_.get('leaf_angle', [45, 60])))
                le, we = max(a_['spray_len'], 0.03), 2.0 * ll * math.sin(splay) + lw
            else:
                # A compound leaf's, a palmate's or a conifer's spray: the element is the spray drawn.
                le, we = a_.get('card_len', cb * 0.984), a_.get('card_wid', cw * 0.94)
            card_fit = [cb / le, cw / we, 0.006 * cb / le]
            card_elem = dict(length=round(le, 4), width=round(we, 4))
        bi = bark['species'].get(sid, -1)
        bu = bark.get('species_upper', {}).get(sid, bi)
        out_sp[sid] = dict(
            leaf_a=a, leaf_b=b, card=card, leaflet=leaflet, needles=e.get('needles', -1), bark=bi, bark_upper=bu,
            bark_scale=types[bi].get('scale', 1.0) if bi >= 0 else 1.0,
            bark_upper_scale=types[bu].get('scale', 1.0) if bu >= 0 else 1.0,
            leaf_kind=kind, shared_with=(src if src != sid else None),
            leaf_hull=hull.get(sid) or hull.get(src) or [1.0, 0.3333, 1.0, 0.6667, 1.0, 1.0],
            leaf_fit=[round(x, 4) for x in fit + [cov]], card_fit=[round(x, 4) for x in card_fit],
            element=elem, card_element=card_elem)
    tree_leaf = [l for l in layers if l['species'] in TB.TREES]
    tree_types = [t for t in types if t.get('tree')]
    first_leaf = layers.index(tree_leaf[0]) if tree_leaf else len(layers)
    first_bark = types.index(tree_types[0]) if tree_types else len(types)
    return dict(
        note='tools/shrubs/textures/build_arrays.py: the trees\' layers in textures/da/shrub_leaf_d|n and '
             'shrub_bark_d|n (appended after the shrubs\' layers); leaf_fit, card_fit and leaf_hull as '
             'make_shrubs.py writes them; bark tiles are 6 cm x the type\'s scale (bark_scale)',
        leaf_layers=len(layers), bark_layers=len(types), tree_leaf_first=first_leaf, tree_bark_first=first_bark,
        species=out_sp,
        bark_types={t['name']: dict(index=types.index(t), scale=t.get('scale', 1.0), species=t.get('species', []),
                                    upper=t.get('upper', [])) for t in tree_types},
        # (The dead and burnt trees' barks belong to no species: the data's dead and burnt variants take them.)
        special_bark={t['name']: types.index(t) for t in tree_types if not t.get('species') and not t.get('upper')},
        leaf_layer_info={l['name']: dict(index=layers.index(l), kind=l['kind'], box_mm=l['box_mm'], hull=l['hull'])
                         for l in tree_leaf},
        leaf_cover={l['name']: cover[l['name']] for l in tree_leaf if l['name'] in cover},
        bark_mean={t['name']: bark_mean[t['name']] for t in tree_types if t['name'] in bark_mean})


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('failed: ' + ' '.join(cmd))


def tool_id(tools):
    """The encoder's identity in the slices' keys: texconv.exe's bytes (another build encodes everything anew)."""
    with open(os.path.join(tools, 'texconv.exe'), 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def slice_key(png, args, tid):
    h = hashlib.sha256()
    with open(png, 'rb') as f:
        h.update(f.read())
    h.update('\0'.join(args).encode('utf-8'))
    h.update(tid.encode('ascii'))
    return h.hexdigest()


def encode(tools, missing, args):
    """Compresses the layers missing from the cache (a batch of files per texconv call): each one's single-image DDS
    goes into the cache under its key."""
    tmp = os.path.join(WORK, 'enc')
    os.makedirs(tmp, exist_ok=True)
    for k in range(0, len(missing), 48):
        batch = missing[k:k + 48]
        run([os.path.join(tools, 'texconv.exe'), '-y'] + args + ['-o', tmp] + [p for p, _ in batch])
        for p, key in batch:
            os.replace(os.path.join(tmp, os.path.splitext(os.path.basename(p))[0] + '.dds'),
                       os.path.join(CACHE, key + '.dds'))


def array(tools, tid, files, out_name, pins, shrubs, coverage=False, wrap=False, repin=False):
    """The array of the layers' cached slices (BC7, full mips), the missing ones encoded first. A texconv output's
    DX10 header is the array's but for its size (and the slices the same: each layer is compressed on its own either
    way). The first `shrubs` slices must be the pinned ones. Returns the array's path and the number encoded."""
    # The alpha is data (coverage, cavity, the lichen mask, the underside's luminance), never a transparency: its mips
    # are filtered apart from the colour's (without -sepalpha texconv weighs the colour by the alpha, and every mip
    # past the first went black where the bark's lichen mask is zero and half dark under a leaf's underside term).
    args = ['-f', 'BC7_UNORM', '-m', '0', '--ignore-srgb', '-sepalpha']
    if coverage:
        args += ['--keep-coverage', '0.5']
    if wrap:
        args += ['-wrap']
    keys = [slice_key(f, args, tid) for f in files]
    pinned = pins.get(out_name)
    if pinned is None or repin:
        pins[out_name] = keys[:shrubs]
    elif keys[:shrubs] != pinned:
        bad = [os.path.basename(files[i]) for i in range(shrubs) if i >= len(pinned) or keys[i] != pinned[i]]
        raise SystemExit('%s: shrub slices changed (%s); the shrub data references them (--repin-shrubs to accept)'
                         % (out_name, ', '.join(bad[:8])))
    os.makedirs(CACHE, exist_ok=True)
    missing = list({k: (f, k) for f, k in zip(files, keys) if not os.path.exists(os.path.join(CACHE, k + '.dds'))}
                   .values())
    if missing:
        encode(tools, missing, args)
    hdr, parts = None, []
    for f, k in zip(files, keys):
        with open(os.path.join(CACHE, k + '.dds'), 'rb') as fh:
            b = fh.read()
        if hdr is None:
            hdr = bytearray(b[:148])
        elif b[:140] != hdr[:140] or b[144:148] != hdr[144:148] or len(b) != len(parts[0]) + 148:
            raise SystemExit('%s: %s does not match the array\'s format' % (out_name, os.path.basename(f)))
        parts.append(b[148:])
    struct.pack_into('<I', hdr, 140, len(parts))
    os.makedirs(OUT, exist_ok=True)
    dst = os.path.join(OUT, out_name + '.dds')
    with open(dst + '.tmp', 'wb') as fh:
        fh.write(bytes(hdr))
        for p in parts:
            fh.write(p)
    os.replace(dst + '.tmp', dst)
    return dst, len(missing)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tools', default=TOOLS)
    ap.add_argument('--src', default=HERE)
    ap.add_argument('--repin-shrubs', action='store_true', help='accept changed shrub slices as the new pins')
    args = ap.parse_args()
    global SRC
    SRC = os.path.abspath(args.src)
    leaf, bark = load('leaf_manifest.json'), load('bark_manifest.json')
    hull = hulls(leaf)
    write_text(os.path.join(SRC, 'leaf_hulls.json'), json.dumps(hull, indent=1, sort_keys=True))
    print('hulls: %d species' % len(hull))
    # Each leaf layer's coverage of its square (the clusters' cells: a leaf's area in its box) and each bark type's
    # mean colour (sRGB: the far tiers' and the bare cells' bark).
    cover = {}
    for l in leaf['layers']:
        a = np.asarray(Image.open(os.path.join(SRC, 'build', 'leaf', l['name'] + '_d.png')))[..., 3]
        cover[l['name']] = round(float(a.mean()) / 255.0, 4)
    bark_mean = {}
    for t in bark['types']:
        c = np.asarray(Image.open(os.path.join(SRC, 'build', 'bark', t['name'] + '_d.png')).convert('RGB'), dtype=np.float64)
        bark_mean[t['name']] = '#%02X%02X%02X' % tuple(int(round(x)) for x in c.reshape(-1, 3).mean(axis=0))
    write_text(os.path.join(SRC, 'texture_stats.json'), json.dumps({'leaf_cover': cover, 'bark_mean': bark_mean}, indent=1,
        sort_keys=True))
    os.makedirs(os.path.join(SRC, 'out'), exist_ok=True)
    tl = tree_layers(leaf, bark, hull, cover, bark_mean)
    write_text(os.path.join(SRC, 'out', 'tree_layers.json'), json.dumps(tl, indent=1))
    print('tree_layers: %d species, leaf layers %d.., bark layers %d..' % (len(tl['species']), tl['tree_leaf_first'],
                                                                          tl['tree_bark_first']))
    lp = [os.path.join(SRC, 'build', 'leaf', l['name']) for l in leaf['layers']]
    bt = [os.path.join(SRC, 'build', 'bark', t['name']) for t in bark['types']]
    for f in lp + bt:
        for suffix in ('_d.png', '_n.png'):
            if not os.path.exists(f + suffix):
                raise SystemExit('missing layer ' + f + suffix)
    tid = tool_id(args.tools)
    pins = load_json(PINS) if os.path.exists(PINS) else {}
    nl, nb = tl['tree_leaf_first'], tl['tree_bark_first']
    for files, name, shr, kw in (([f + '_d.png' for f in lp], 'shrub_leaf_d', nl, dict(coverage=True)),
                                 ([f + '_n.png' for f in lp], 'shrub_leaf_n', nl, {}),
                                 ([f + '_d.png' for f in bt], 'shrub_bark_d', nb, dict(wrap=True)),
                                 ([f + '_n.png' for f in bt], 'shrub_bark_n', nb, dict(wrap=True))):
        dst, enc = array(args.tools, tid, files, name, pins, shr, repin=args.repin_shrubs, **kw)
        print('%s %.1f MB: %d slices, %d encoded' % (os.path.relpath(dst, REPO), os.path.getsize(dst) / 1048576.0,
                                                    len(files), enc))
    os.makedirs(WORK, exist_ok=True)
    write_text(PINS, json.dumps(pins, indent=1))


if __name__ == '__main__':
    main()
