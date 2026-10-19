"""Every tree card family of Dead Air in six looks: autumn (the base), summer, winter, early/mid/late spring.

One grown structure per family, so a tree keeps its silhouette through the year. Targets per look:
  autumn  Dead Air's own card (coverage, wood/foliage colour)
  summer  the summer pack's version of the same card (coverage, colour) - statistics only
  winter  deciduous bare, conifers keep needles; snow on upward edges, frost elsewhere
  spring  per-species phenology: catkins, young light leaves, oak's bronze flush, new conifer shoots
Output: work/seasons_out/<look>/<name>.png at the card's final size.
"""
import json
import math
import os
import re
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import BORROWED, WORK, load_rgba, save_png  # noqa: E402
import cards  # noqa: E402
import make_trees as mt  # noqa: E402

STOCK = r"D:/Games/Dead Air/_work/texaudit/stock/textures/trees"
SUMMER = os.path.join(WORK, 'seasons', 'summer_green', 'textures', 'trees')
OUT = os.path.join(WORK, 'seasons_out')
LOOKS = ['autumn', 'summer', 'winter', 'spring_early', 'spring_mid', 'spring_late']

# Families handled elsewhere (GSC sources) or not cards at all
# Card names to render (others of the family are skipped); all when unset
ONLY = set(filter(None, os.environ.get('TREES_ONLY', '').split(',')))

GSC_ROUTE = ('trees_kamish', 'trees_kamysh', 'trees_osoka', 'trees_tina', 'trees_sprouts', 'trees_forestwall', 'trees_a_bush',
             'trees_kamish_sux_fake', 'trees_fake', 'trees_det', 'trees_anomal', 'trees_sosna_red_low', 'trees_byaka')
VARIANT = re.compile(r'(_dry|_red|_broken|_dead|_rotten|_sux|_aurora)+$')


def da_path(name):
    return os.path.join(STOCK, name + '.dds')


def families():
    fam = {}
    for f in sorted(os.listdir(STOCK)):
        if not f.endswith('.dds') or '_bump' in f or 'normal' in f:
            continue
        n = f[:-4]
        if n.startswith(GSC_ROUTE):
            continue
        a = np.asarray(Image.open(da_path(n)).convert('RGBA'))[..., 3]
        if (a > 127).mean() > 0.97:
            continue  # bark, caps: no season change
        root = VARIANT.sub('', n)
        fam.setdefault(root, []).append(n)
    return fam


def species(root):
    for k in ('sosnodub', 'birch', 'alder', 'oak', 'dub', 'poplar', 'willow', 'cherry', 'orex', 'spruce', 'elka', 'pine', 'sosna'):
        if k in root:
            return {'dub': 'oak', 'sosna': 'pine', 'elka': 'spruce', 'sosnodub': 'pine'}.get(k, k)
    return 'generic'


CONIFER = {'pine', 'spruce'}
LEAFSETS = {'alder': ['LeafSet024', 'LeafSet009'], 'birch': ['LeafSet014'], 'oak': ['LeafSet016', 'LeafSet012'],
            'poplar': ['LeafSet004'], 'willow': ['LeafSet013'], 'cherry': ['LeafSet007', 'LeafSet014'],
            'orex': ['LeafSet009', 'LeafSet024'], 'generic': ['LeafSet024', 'LeafSet009', 'LeafSet007']}


def auto_cfg(root, names):
    """Config for a family without a hand-made entry: species from the name, architecture from the card's layout."""
    sp = species(root)
    base = root if root in names else names[0]
    lay = cards.analyse(load_rgba(da_path(base)))
    W = lay['W']
    centred = abs(lay['base'][0] - W / 2) < 0.22 * W
    small = lay['S'] <= 512
    if 'frond' in root:
        arch = dict(mt.SIDE, length=[1.2, 0.25, 0.08], env=0.05)
    elif sp == 'willow':
        arch = mt.WILLOW
    elif lay['edge'] == 'top':
        arch = mt.BOUGH
    elif lay['edge'] == 'bottom' and centred:
        arch = mt.SPRUCE if sp == 'spruce' else mt.PINE if sp == 'pine' else (mt.WEEPING if sp == 'birch' else mt.CROWN)
    else:
        arch = mt.SIDE
    if sp in CONIFER:
        cfg = dict(mode='spray', sets=['fir'] if sp == 'spruce' else ['pine', 'fir'], leaf=0.1 if not small else 0.13, arch=arch,
                   droop=25 if sp == 'spruce' else 0)
    else:
        cfg = dict(mode='leaf', sets=LEAFSETS[sp], leaf=0.016 if not small else 0.024, arch=arch, birch=sp == 'birch')
    if 'dark' in root:
        cfg['mode'] = 'bare'
    cfg['tex'] = sorted(names, key=lambda n: (n != base, n))
    return cfg


def out_size(name):
    p = os.path.join(BORROWED, f'textures/trees/{name}.dds')
    if os.path.isfile(p):
        return Image.open(p).size
    w, h = Image.open(da_path(name)).size
    k = min(2.0, 2048 / max(w, h))
    return int(w * k), int(h * k)


# Spring per species: (leaf share of summer, leaf scale) for early/mid/late, extras
# A tree is dozens of overlapping cards, so a share that looks sparse on one card reads as a crown on the tree:
# mid-spring leaves stay a haze the branches show through
PHENO = {
    'alder': [(0.0, 0.0), (0.2, 0.55), (0.85, 0.9)], 'birch': [(0.0, 0.0), (0.25, 0.5), (0.9, 0.9)],
    'willow': [(0.0, 0.0), (0.3, 0.6), (0.9, 0.95)], 'poplar': [(0.0, 0.0), (0.18, 0.5), (0.8, 0.85)],
    'oak': [(0.0, 0.0), (0.03, 0.3), (0.55, 0.7)], 'cherry': [(0.0, 0.0), (0.15, 0.45), (0.85, 0.9)],
    'orex': [(0.0, 0.0), (0.18, 0.5), (0.85, 0.9)], 'generic': [(0.0, 0.0), (0.18, 0.55), (0.82, 0.9)],
}
# Catkins at their real size on a ~5 m branch card (5-7 cm by ~1 cm), on a tenth of the twig tips
CATKINS = {'alder': ((0.012, 0.0018), (78, 58, 46), True), 'birch': ((0.011, 0.0017), (118, 104, 62), True),
           'poplar': ((0.014, 0.0022), (98, 76, 68), True), 'orex': ((0.012, 0.0018), (168, 152, 88), True),
           'willow': ((0.005, 0.0035), (214, 212, 200), False)}
CATKIN_TIPS = 0.12


def lab_stats(rgba, green=False):
    _, st = cards.classes(rgba, prefer_green=green)
    return st


def shift_stats(st, dL=0, da=0, db=0, chroma=1.0, spread=1.0):
    out = {}
    for k, v in st.items():
        if k == 'foliage_frac':
            out[k] = v
            continue
        m, s = v
        m = m.copy()
        m[0] += dL
        m[1] = 128 + (m[1] - 128) * chroma + da
        m[2] = 128 + (m[2] - 128) * chroma + db
        out[k] = (m, s * spread)
    return out


GENERIC_SUMMER = None


def summer_target(name, base_name, da_rgba, green=False):
    """(coverage, stats) of the summer pack's card; similar cards' numbers when the pack has none."""
    p = os.path.join(SUMMER, name + '.dds')
    ratio = 1.0
    if not os.path.isfile(p):
        p = os.path.join(SUMMER, base_name + '.dds')
        # Borrowed from the family's base card: keep this card's own density, scaled like the base's
        if os.path.isfile(p) and os.path.isfile(da_path(base_name)):
            db = np.asarray(Image.open(da_path(base_name)).convert('RGBA'))[..., 3]
            sb = np.asarray(Image.open(p).convert('RGBA'))[..., 3]
            ratio = float((sb > 127).mean()) / max(float((db > 127).mean()), 1e-3)
            s = np.asarray(Image.open(p).convert('RGBA'))
            st = lab_stats(s, green)
            if 'foliage' not in st:
                st['foliage'] = st['wood']
            return float((da_rgba[..., 3] > 127).mean()) * ratio, st
    if os.path.isfile(p):
        s = np.asarray(Image.open(p).convert('RGBA'))
        st = lab_stats(s, green)
        if 'foliage' not in st:
            st['foliage'] = st['wood']
        return float((s[..., 3] > 127).mean()), st
    st = lab_stats(da_rgba)
    st = shift_stats(st, dL=-6, da=-14, db=6)
    return float((da_rgba[..., 3] > 127).mean()) * 1.2, st


def render_card(P, parent, r, plan, n, pool, out_w, out_h, seed, birch, size_mul=1.0, spray=False):
    # Needle sprays are photo sprites already: no supersampling for them, it only costs time
    ss = 1 if spray or max(out_w, out_h) > 2048 else 2
    use = plan if size_mul == 1.0 else [(i, a, s * size_mul, t, b, si) for (i, a, s, t, b, si) in plan]
    cv = mt.render(P, parent, r, use, n, pool, out_w * ss, out_h * ss, max(out_w, out_h) * ss, seed, birch=birch)
    cards.occlude(cv)
    rgb, a, fol = cards.finalize(cv, out_w, out_h)
    return (rgb * 255 + 0.5).astype(np.uint8), a, fol


# Margin a grown tree keeps from the card's border, and the reach around the attachment point that may cross it
FIT_MARGIN = 0.012
FIT_FREE = 0.12


def fit_to_card(lay, P, r, plan, pool):
    """Keep wood and foliage inside the card.

    Growth stops at Dead Air's envelope, but a spray or a leaf at a tip reaches a sprite's length further, and a
    card whose foliage runs into its border shows the card in the game: a plane cut off by a straight line.
    The tree shrinks towards its attachment point - as far as its wood needs, and by at most a quarter for its
    foliage; a spray that still sticks out is shortened to fit, or dropped when too little would be left.
    Only the attachment (FIT_FREE around the base point, and the borders it lies on) may cross the border, as on
    Dead Air's cards. Returns P, r, plan and the scale k <= 1.
    """
    B = np.asarray(lay['base'], np.float64)
    W, H = lay['W'], lay['H']
    m = FIT_MARGIN
    near = 0.05
    # A border the attachment lies on is not a limit (the entry edge, and both edges of a corner)
    limits = [(axis, lo, hi, B[axis] > near, B[axis] < size - near)
              for axis, lo, hi, size in ((0, m, W - m, W), (1, m, H - m, H))]

    def scale_for(Q, R):
        """Largest k <= 1 that keeps the discs (Q, R) scaled about B inside the limits."""
        far = np.linalg.norm(Q - B, axis=1) > FIT_FREE
        Q, R = Q[far], R[far]
        k = 1.0
        if not len(Q):
            return k
        d = Q - B
        for axis, lo, hi, use_lo, use_hi in limits:
            ext_hi = d[:, axis] + R
            ext_lo = d[:, axis] - R
            if use_hi and (ext_hi > 1e-9).any():
                k = min(k, float(((hi - B[axis]) / ext_hi[ext_hi > 1e-9]).min()))
            if use_lo and (ext_lo < -1e-9).any():
                k = min(k, float(((B[axis] - lo) / -ext_lo[ext_lo < -1e-9]).min()))
        return k

    reach = [math.hypot(0.5 * pool[si].rgba.shape[1] / pool[si].rgba.shape[0], 1.0) for (*_, si) in plan]
    k_wood = scale_for(P, r)
    if plan:
        k_all = scale_for(np.concatenate([P, P[[e[0] for e in plan]]]), np.concatenate([r, [e[2] * c for e, c in zip(plan, reach)]]))
    else:
        k_all = k_wood
    k = min(1.0, max(k_all, min(k_wood, 0.75)))
    k = max(k, 0.3)
    if k < 0.999:
        P = B + (P - B) * k
        r = r * k
        plan = [(i, ang, size * k, tint, back, si) for (i, ang, size, tint, back, si) in plan]
    else:
        k = 1.0
    # Sprays still past the border: shortened to fit, dropped below 40 % of their length
    kept = []
    for (i, ang, size, tint, back, si), c in zip(plan, reach):
        q = P[i]
        room = size * c
        if np.linalg.norm(q - B) > FIT_FREE:
            for axis, lo, hi, use_lo, use_hi in limits:
                if use_hi:
                    room = min(room, hi - q[axis])
                if use_lo:
                    room = min(room, q[axis] - lo)
        fit_size = max(room, 0.0) / c
        if fit_size < 0.4 * size:
            continue
        kept.append((i, ang, min(size, fit_size), tint, back, si))
    return P, r, kept, k


def n_for(curve, target, plan_len):
    if curve is None or target <= curve[0] + 0.01:
        return 0
    hit = np.flatnonzero(curve >= target)
    return int(min(plan_len, (hit[0] if len(hit) else len(curve) - 1) * 25))


def build(root, cfg, looks):
    sp = species(root)
    names = cfg['tex']
    fam_cfg = dict(cfg, tex=names)
    rng, lay, P, parent, D, r = mt.structure(root, fam_cfg)
    if cfg['mode'] == 'leaf':
        # Leaf cards need room for leaves: thin the branching until the bare wood covers at most ~62% of the card's coverage
        base_cov = float((load_rgba(da_path(names[0]))[..., 3] > 127).mean())
        arch = dict(cfg['arch'])
        for _ in range(3):
            q = 256
            wood = cards.coverage_curve(P, parent, r, [], [], q, q, q)[0]
            if wood <= 0.62 * base_cov:
                break
            arch = dict(arch, spacing=[x * 1.35 for x in arch['spacing']])
            fam_cfg = dict(fam_cfg, arch=arch)
            cfg = dict(cfg, arch=arch)
            rng, lay, P, parent, D, r = mt.structure(root, fam_cfg)
    pool = mt.sprite_pool(cfg['sets']) if cfg['mode'] != 'bare' else []
    plan = mt.foliage_plan(P, parent, D, r, cfg, len(pool), rng) if pool else []
    n_plan = len(plan)
    P, r, plan, k = fit_to_card(lay, P, r, plan, pool)
    if k < 1.0 or len(plan) < n_plan:
        print(f'{root}: fitted into the card, scale {k:.3f}, {n_plan - len(plan)} of {n_plan} sprays dropped', flush=True)
    nchild = np.bincount(parent[parent >= 0], minlength=len(P))
    tips = P[nchild == 0]
    base_name = names[0]
    report = {}
    curve = None
    for name in names:
        da = load_rgba(da_path(name))
        out_w, out_h = out_size(name)
        if plan and curve is None:
            cw, ch = max(64, out_w // 2), max(64, out_h // 2)
            curve = cards.coverage_curve(P, parent, r, [(i, a, s, t, b) for (i, a, s, t, b, si) in plan],
                                         [pool[si] for (*_, si) in plan], cw, ch, max(cw, ch), batch=25)
        green = sp in CONIFER
        st_aut = lab_stats(da, green)
        cov_aut = float((da[..., 3] > 127).mean())
        cov_sum, st_sum = summer_target(name, base_name, da, green)
        # Dead wood does not follow the year: a dry spruce keeps its dead needles, a rotten branch stays bare
        dead = bool(re.search(r'_(dead|rotten)', name)) or (sp in CONIFER and 'sux' in name)
        n_aut = n_for(curve, cov_aut, len(plan))
        n_sum = 0 if dead else n_for(curve, cov_sum, len(plan))
        if not dead and '_dry' in name and sp not in CONIFER:
            n_sum = int(n_sum * 0.8)
        scale = max(out_w, out_h)
        if ONLY and name not in ONLY:
            continue
        for look in looks:
            seed = mt.seed_of(name + look)
            st, n, mul, extra = st_aut, n_aut, 1.0, None
            wood = {'wood': st_aut['wood']} if 'wood' in st_aut else {}
            if dead:
                if look == 'winter':
                    st = shift_stats(st_aut, dL=-10, chroma=0.6)
            elif look == 'summer':
                st, n = dict(st_sum, **wood), n_sum
            elif look == 'winter':
                n = n_aut if sp in CONIFER and not dead else 0
                st = shift_stats(st_sum, dL=-8, chroma=0.8) if sp in CONIFER else shift_stats(st_aut, dL=-10, chroma=0.6)
            elif look.startswith('spring'):
                stage = ['spring_early', 'spring_mid', 'spring_late'].index(look)
                fresh = shift_stats(st_sum, dL=14, da=-4, db=12)
                if sp in CONIFER:
                    n = 0 if dead else n_sum
                    st = dict(shift_stats(st_sum, dL=-6 if stage == 0 else 0, chroma=0.85 if stage == 0 else 1.0), **wood)
                    if stage == 2:
                        extra = 'candles'
                else:
                    share, mul = PHENO.get(sp, PHENO['generic'])[stage]
                    n = 0 if dead else int(n_sum * share)
                    st = dict(fresh, **wood)
                    if sp == 'oak' and stage >= 1:
                        st = dict(shift_stats(fresh, da=10, db=6), **wood)
                    if stage == 0 and sp in CATKINS and not dead:
                        extra = 'catkins'
                    if stage == 1 and sp == 'cherry' and not dead:
                        extra = 'blossom'
                    mul = mul or 1.0
            rgb8, a, fol = render_card(P, parent, r, plan, n, pool, out_w, out_h, seed, cfg.get('birch', False), mul,
                                       spray=cfg['mode'] == 'spray')
            rgb8 = cards.transfer(rgb8, a, fol, st, keep_wood=cfg.get('birch', False))
            a8 = (a * 255 + 0.5).astype(np.uint8)
            if look == 'winter':
                if sp in CONIFER:
                    rgb8 = cards.snow(rgb8, a8, amount=2.6, frost=0.08, depth_rel=0.034, seed=seed)
                else:
                    rgb8 = cards.snow(rgb8, a8, amount=1.2, frost=0.34, seed=seed)
            if extra == 'catkins':
                (Lr, Wr), col, hang = CATKINS[sp]
                rg = np.random.default_rng(seed)
                pts = tips[rg.random(len(tips)) < CATKIN_TIPS]
                rgb8, a8 = cards.dangles(rgb8, a8, pts, scale, Lr, Wr, col, rg, hang=hang, count=(1, 2) if hang else (1, 3))
            elif extra == 'blossom':
                rg = np.random.default_rng(seed)
                pts = tips[rg.random(len(tips)) < 0.6]
                rgb8, a8 = cards.dangles(rgb8, a8, pts, scale, 0.006, 0.006, (236, 228, 226), rg, hang=False, count=(2, 5), jitter=90)
            elif extra == 'candles':
                rg = np.random.default_rng(seed)
                pts = tips[rg.random(len(tips)) < 0.7]
                rgb8 = cards.tint_near(rgb8, a8, pts, scale, 0.0045, (150, 178, 92), 0.55, fol=fol)
            os.makedirs(os.path.join(OUT, look), exist_ok=True)
            save_png(os.path.join(OUT, look, name + '.png'), np.dstack([rgb8, a8]))
            report[f'{look}/{name}'] = {'n': n, 'of': len(plan), 'cov': round(float((a8 > 127).mean()), 3)}
    return report


def all_configs():
    fam = families()
    cfgs = {}
    for root, names in fam.items():
        hand = next((v for k, v in mt.FAMILIES.items() if v['tex'][0] == root or root in v['tex']), None)
        if hand:
            # Hand-tuned family; add the variants it did not list
            cfgs[root] = dict(hand, tex=hand['tex'] + [n for n in sorted(names) if n not in hand['tex']])
        else:
            cfgs[root] = auto_cfg(root, names)
    return cfgs


if __name__ == '__main__':
    cfgs = all_configs()
    if sys.argv[1:2] == ['--list']:
        for k, v in cfgs.items():
            print(k, v['mode'], v.get('sets'), len(v['tex']), v['tex'])
        sys.exit(0)
    looks = LOOKS
    args = sys.argv[1:]
    if args[:1] == ['--looks']:
        looks, args = args[1].split(','), args[2:]
    roots = args or list(cfgs)
    rep = {}
    for root in roots:
        rep.update(build(root, cfgs[root], looks))
    os.makedirs(os.path.join(OUT, 'reports'), exist_ok=True)
    json.dump(rep, open(os.path.join(OUT, 'reports', '_'.join(roots)[:80] + '.json'), 'w'), indent=0)
