"""Own replacements for the Absolute Nature 4 tree cards, one family at a time.

A family shares one grown structure; its variants (_dry, _red, _broken) differ in how much foliage
they keep (Dead Air's coverage for that variant) and in colour (Dead Air's statistics for that variant).
"""
import hashlib
import json
import math
import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import BORROWED, WORK, install, load_rgba, orig_path, save_png, texconv  # noqa: E402
import cards  # noqa: E402
import sprites  # noqa: E402

OUT = os.path.join(WORK, 'trees')

# Branch architecture presets (per depth: leader, laterals, sub-laterals, twigs)
CROWN = dict(length=[1.4, 0.30, 0.12, 0.05], spacing=[0.03, 0.026, 0.02], angle=[(40, 72), (30, 62), (25, 55)],
             per_node=[1, 1, 1], trop=[(0, 0), (0, -0.012), (0, -0.006), (0, 0)], noise=[0.02, 0.05, 0.08, 0.1],
             clear=0.06, taper=0.6, env=0.12)
COLUMN = dict(CROWN, angle=[(18, 38), (20, 45), (20, 45)], trop=[(0, 0), (0, -0.02), (0, -0.01), (0, 0)])
WEEPING = dict(CROWN, trop=[(0, 0), (0, -0.006), (0, 0.006), (0, 0.02)])
# A sprawling branch: long laterals fanning out, a dense twig network, sparse leaf clusters
SIDE = dict(length=[1.5, 0.55, 0.25, 0.08], spacing=[0.05, 0.04, 0.03], angle=[(25, 65), (25, 65), (20, 60)],
            per_node=[1, 1, 1], trop=[(0, -0.004), (0, -0.006), (0, -0.003), (0, 0)], noise=[0.02, 0.05, 0.08, 0.1],
            clear=0.04, taper=0.5, env=0.05, tip_leaves=5, along_p=0.2, r_tip=0.001)
PINE = dict(length=[1.4, 0.34, 0.10], spacing=[0.045, 0.03], angle=[(50, 82), (35, 70)], per_node=[2, 1],
            trop=[(0, 0), (0, -0.012), (0, -0.006)], noise=[0.015, 0.045, 0.07], clear=0.16, taper=0.55, env=0.14)
SPRUCE = dict(PINE, angle=[(65, 92), (35, 70)], trop=[(0, 0), (0, 0.008), (0, 0.012)])
BOUGH = dict(length=[1.6, 0.45, 0.14], spacing=[0.03, 0.02], angle=[(30, 70), (20, 60)], per_node=[1, 1],
             trop=[(0, 0.006), (0, 0.03), (0, 0.04)], noise=[0.02, 0.05, 0.07], clear=0.04, taper=0.45, env=0.12)
FAKE_PINE = dict(length=[1.4, 0.14, 0.05], spacing=[0.035, 0.02], angle=[(50, 82), (30, 60)], per_node=[2, 1],
                 trop=[(0, 0), (0, -0.008), (0, 0)], noise=[0.01, 0.04, 0.06], clear=0.5, taper=0.5, env=0.12)
WILLOW = dict(length=[1.3, 0.12, 0.04], spacing=[0.05, 0.03], angle=[(10, 30), (20, 45)], per_node=[1, 1],
              trop=[(0, -0.01), (0, -0.01), (0, -0.005)], noise=[0.012, 0.03, 0.06], clear=0.06, taper=0.3, env=0.05,
              shoots=11, shoot_spread=12)

# mode: leaf | spray | bare. Leaf/spray size is a fraction of the card's longer side.
FAMILIES = {
    'alder': dict(tex=['trees_atp_alder_branch', 'trees_atp_alder_branch_dry', 'trees_atp_alder_branch_red'],
                  mode='leaf', sets=['LeafSet024', 'LeafSet009'], leaf=0.016, arch=CROWN),
    'birch': dict(tex=['trees_atp_birch_branch', 'trees_atp_birch_branch_dry'], mode='leaf', sets=['LeafSet014'],
                  leaf=0.014, arch=WEEPING, birch=True),
    'birch_dark': dict(tex=['trees_atp_birch_dark_branch'], mode='bare', arch=dict(WEEPING, env=0.04)),
    'oak': dict(tex=['trees_atp_oak_branch', 'trees_atp_oak_branch_dry', 'trees_atp_oak_branch_red'], mode='leaf',
                sets=['LeafSet016', 'LeafSet012'], leaf=0.02, arch=CROWN),
    'poplar': dict(tex=['trees_atp_poplar_branch', 'trees_atp_poplar_branch_dry', 'trees_atp_poplar_branch_red'], mode='leaf',
                   sets=['LeafSet004'], leaf=0.016, arch=dict(CROWN, env=0.04)),
    'willow1': dict(tex=['trees_atp_willow_branch1'], mode='leaf', sets=['LeafSet013'], leaf=0.04, arch=WILLOW),
    'pine': dict(tex=['trees_atp_pine_branch', 'trees_atp_pine_branch_broken', 'trees_atp_pine_branch_dry', 'trees_atp_pine_branch_red'],
                 mode='spray', sets=['pine', 'fir'], leaf=0.1, arch=PINE),
    'pine_long': dict(tex=['trees_atp_pine_branch_long', 'trees_atp_pine_branch_long_broken', 'trees_atp_pine_branch_long_dry',
                           'trees_atp_pine_branch_long_red'], mode='spray', sets=['pine', 'fir'], leaf=0.09,
                  arch=dict(PINE, length=[1.4, 0.2, 0.07])),
    'spruce': dict(tex=['trees_atp_spruce_branch', 'trees_atp_spruce_branch_broken', 'trees_atp_spruce_branch_dry',
                        'trees_atp_spruce_branch_red'], mode='spray', sets=['fir'], leaf=0.08, droop=25, arch=SPRUCE),
    'pine_fake': dict(tex=['trees_atp_pine_fake_sux01'], mode='spray', sets=['pine', 'fir'], leaf=0.09, arch=FAKE_PINE),
    'elka': dict(tex=['trees_elka', 'trees_elka_sux'], mode='spray', sets=['fir'], leaf=0.08, droop=55, arch=BOUGH),
    'vetkabig': dict(tex=['trees_vetkabig', 'trees_vetkabig_dry'], mode='leaf', sets=['LeafSet024', 'LeafSet009'], leaf=0.016, arch=SIDE),
    'vetkagreen1': dict(tex=['trees_vetkagreen1', 'trees_vetkagreen1_dry'], mode='leaf', sets=['LeafSet024', 'LeafSet007'], leaf=0.016, arch=SIDE),
    'vetkagreen3': dict(tex=['trees_vetkagreen3', 'trees_vetkagreen3_dry'], mode='leaf', sets=['LeafSet009', 'LeafSet007'], leaf=0.016, arch=SIDE),
    'vetkagreen5': dict(tex=['trees_vetkagreen5', 'trees_vetkagreen5_dry'], mode='leaf', sets=['LeafSet014', 'LeafSet024'], leaf=0.016, arch=SIDE),
    'vetkasux2': dict(tex=['trees_vetkasux2', 'trees_vetkasux2_dry'], mode='leaf', sets=['LeafSet024', 'LeafSet012'], leaf=0.018, arch=SIDE),
    'dub': dict(tex=['trees_dub', 'trees_dub_dry'], mode='leaf', sets=['LeafSet016', 'LeafSet012'], leaf=0.02, arch=SIDE),
    'new_vetka': dict(tex=['trees_new_vetka'], mode='leaf', sets=['LeafSet024', 'LeafSet009'], leaf=0.016, arch=CROWN),
    'new_vetka_green1': dict(tex=['trees_new_vetka_green1', 'trees_new_vetka_green1_dry'], mode='leaf',
                             sets=['LeafSet024', 'LeafSet014'], leaf=0.016, arch=CROWN),
}


def seed_of(name):
    return int(hashlib.md5(name.encode()).hexdigest()[:8], 16)


_SPRITES = {}


def sprite_pool(sets):
    pool = []
    for s in sets:
        if s not in _SPRITES:
            _SPRITES[s] = sprites.needle_sprays(s) if not s.startswith('LeafSet') else sprites.leaf_set(s)
        pool += _SPRITES[s]
    return pool


_BARK = None


def bark_grain():
    """Bark grain multiplier around 1.0 from a CC0 bark scan's luminance."""
    global _BARK
    if _BARK is None:
        p = os.path.join(sprites.ACG, 'Bark001', 'Bark001_2K-JPG_Color.jpg')
        L = np.asarray(Image.open(p).convert('L'), np.float32)
        L = cv2.resize(L, (512, 512), interpolation=cv2.INTER_AREA)
        hp = L - cv2.GaussianBlur(L, (0, 0), 8)
        _BARK = 1.0 + 0.11 * hp / max(float(hp.std()), 1e-3)
    return _BARK


def angle_of(d):
    return math.degrees(math.atan2(d[0], -d[1]))


def node_depth(parent):
    """Branch order per node: +1 whenever a node is not its parent's first child."""
    n = len(parent)
    first = {}
    dep = np.zeros(n, int)
    for i in range(1, n):
        p = parent[i]
        if p in first:
            dep[i] = dep[p] + 1
        else:
            first[p] = i
            dep[i] = dep[p]
    return dep


def foliage_plan(P, parent, D, r, cfg, n_sprites, rng):
    """Every possible leaf/spray as (node, angle, size, tint, back, sprite). Shuffled so prefixes are even."""
    n = len(P)
    nchild = np.bincount(parent[parent >= 0], minlength=n)
    dep = node_depth(parent)
    r_tip = r.min()
    out = []
    tips = []
    droop = cfg.get('droop', 0)
    for i in range(1, n):
        tip = nchild[i] == 0
        if cfg['mode'] == 'leaf':
            if not (tip or (dep[i] >= 2 and r[i] <= r_tip * 2.5)):
                continue
            k = cfg['arch'].get('tip_leaves', 9) if tip else (2 if rng.random() < cfg['arch'].get('along_p', 0.55) else 0)
            spread = (20, 95)
        else:
            if dep[i] == 0 and not tip:
                continue
            # Needles sit in tufts at the ends of the boughs; along a bough only now and then, so the wood
            # inside the crown shows between the tufts
            k = 8 if tip else (1 if rng.random() < 0.06 else 0)
            spread = (5, 80)
        for j in range(k):
            side = 1 if (i + j) % 2 else -1
            ang = angle_of(D[i]) + side * rng.uniform(*spread)
            if droop:
                diff = ((180 - ang + 180) % 360) - 180
                ang += np.clip(diff, -droop, droop) * rng.uniform(0.4, 1.0)
            node_tint = 0.72 + 0.36 * ((i * 2654435761) % 1000) / 1000.0
            out.append((i, ang, cfg['leaf'] * rng.uniform(0.72, 1.25), node_tint * rng.uniform(0.9, 1.06), rng.random() < 0.4,
                        int(rng.integers(0, n_sprites))))
            tips.append(tip)
    order = rng.permutation(len(out))
    if cfg['mode'] == 'spray' and out:
        # Needles grow in clumps: a conifer reads by its separate boughs with sky between them, not as an even mass.
        # Every first-order bough gets a weight, and the plan is ranked by weight times chance, tips first, so the
        # sprays the coverage asks for (a prefix of the plan) pile onto some boughs and leave others nearly bare.
        seg = np.arange(n)
        cluster = np.zeros(n, int)
        for i in range(1, n):
            p = parent[i]
            seg[i] = i if dep[i] != dep[p] else seg[p]
            cluster[i] = seg[i] if dep[i] <= 1 else cluster[p]
        weight = {c: rng.uniform(0.12, 1.0) ** 1.6 for c in np.unique(cluster)}
        key = np.array([rng.random() * weight[cluster[e[0]]] * (1.7 if t else 1.0) for e, t in zip(out, tips)])
        order = np.argsort(-key, kind='stable')
    return [out[k] for k in order]


def render(P, parent, r, plan, n_fol, pool, W, H, scale, rng_seed, wood_col=np.array([0.38, 0.33, 0.29]), birch=False):
    rng = np.random.default_rng(rng_seed)
    cv = cards.Canvas(W, H)
    use = plan[:n_fol]
    for back in (True, False):
        if not back:
            if birch:
                cards.draw_wood(cv, P, parent, r, scale, bark_grain(), np.array([0.30, 0.22, 0.19]), rng,
                                white_from=r.min() * 3.0, white_col=np.array([0.80, 0.79, 0.75]))
            else:
                cards.draw_wood(cv, P, parent, r, scale, bark_grain(), wood_col, rng)
        for (i, ang, size, tint, is_back, si) in use:
            if is_back != back:
                continue
            x, y = P[i] * scale
            cards.place(cv, pool[si], x, y, ang, size * scale, tint=tint * (0.68 if back else 1.0), sat=0.85 if back else 1.0)
    return cv


def structure(fam, cfg):
    rng = np.random.default_rng(seed_of(fam))
    da = load_rgba(orig_path(f'textures/trees/{cfg["tex"][0]}.dds'))
    lay = cards.analyse(da)
    P, parent, D = cards.grow_tree(lay, rng, cfg['arch'])
    P, parent, D = cards.add_twigs(P, parent, D, rng, n_per_tip=(0, 2), length=(0.006, 0.02), lateral=0.04)
    P = cards.smooth(P, parent)
    r_tip = cfg['arch'].get('r_tip', 0.0007)
    r = cards.radii(parent, r_tip, max(lay['trunk_r'] * 0.9, r_tip * 3))
    return rng, lay, P, parent, D, r


def build_family(fam, cfg, only=None):
    rng, lay, P, parent, D, r = structure(fam, cfg)
    pool = sprite_pool(cfg['sets']) if cfg['mode'] != 'bare' else []
    plan = foliage_plan(P, parent, D, r, cfg, len(pool), rng) if pool else []
    results = {}
    curve = None
    for name in cfg['tex']:
        if only and name not in only:
            continue
        rel = f'textures/trees/{name}.dds'
        dav = load_rgba(orig_path(rel))
        out_w, out_h = Image.open(os.path.join(BORROWED, rel)).size
        _, stats = cards.classes(dav)
        target = float((dav[..., 3] > 127).mean())
        n = 0
        if plan:
            if curve is None:
                cw, ch = max(64, out_w // 2), max(64, out_h // 2)
                curve = cards.coverage_curve(P, parent, r, [(i, a, s, t, b) for (i, a, s, t, b, si) in plan],
                                             [pool[si] for (*_, si) in plan], cw, ch, max(cw, ch), batch=25)
            # Foliage only when Dead Air's card is denser than our bare wood
            if target > curve[0] + 0.01:
                hit = np.flatnonzero(curve >= target)
                n = int(min(len(plan), (hit[0] if len(hit) else len(curve) - 1) * 25))
        ss = 2 if max(out_w, out_h) <= 2048 else 1
        cv = render(P, parent, r, plan, n, pool, out_w * ss, out_h * ss, max(out_w, out_h) * ss, seed_of(name), birch=cfg.get('birch', False))
        cards.occlude(cv)
        rgb, a, fol = cards.finalize(cv, out_w, out_h)
        rgb8 = (rgb * 255 + 0.5).astype(np.uint8)
        rgb8 = cards.transfer(rgb8, a, fol, stats, keep_wood=cfg.get('birch', False))
        a8 = (a * 255 + 0.5).astype(np.uint8)
        os.makedirs(os.path.join(OUT, 'png'), exist_ok=True)
        save_png(os.path.join(OUT, 'png', name + '.png'), np.dstack([rgb8, a8]))
        results[name] = {'size': [out_w, out_h], 'leaves': n, 'of': len(plan), 'cov': round(float((a > 0.5).mean()), 3),
                         'da_cov': round(target, 3), 'wood_cov': round(float(curve[0]) if curve is not None else -1, 3),
                         'foliage_da': round(stats.get('foliage_frac', 0.0), 3)}
        print(fam, name, results[name], flush=True)
    return results


def encode_install(name):
    png = os.path.join(OUT, 'png', name + '.png')
    enc = os.path.join(OUT, 'dds')
    os.makedirs(enc, exist_ok=True)
    dds = texconv(png, enc, 'BC1_UNORM', 0, alpha_cutout=True)
    install(dds, f'textures/trees/{name}.dds')


if __name__ == '__main__':
    fams = sys.argv[1:] or list(FAMILIES)
    allres = {}
    for f in fams:
        allres.update(build_family(f, FAMILIES[f]))
    json.dump(allres, open(os.path.join(OUT, 'report_' + '_'.join(fams)[:60] + '.json'), 'w'), indent=1)
