# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""Write the upgraded level trees' data (docs/dead-air/TREES.md): configs/dead_air_x64_trees.ltx and
docs/dead-air/TREE_DATA.md (and the data section of TREES.md when that document exists).

Sources (tools/trees/data, written by census_trees.py):
  tree_kinds.json          the census of the stock trees: every kind's family keys, class, look flags, species hints
                           from its cards, skeleton id, signature rows and placements per level
  tree_photo_colours.json  the 30 ChEZ tree species' measured colours per calendar month and role (the species
                           research of 08.10.2026, METHOD_TREES.md)
Texture layers: tools/shrubs/textures/out/tree_layers.json (the tree layers in the shrubs' arrays: the species' leaf,
card and bark layers with their fits; bark_types: the char layer, the tiles' scales) with the shrubs' manifests and
texture_stats.json; while it does not exist each species borrows the nearest shrub layers (FALLBACK_LEAF,
FALLBACK_BARK).
Skeletons: tools/trees/out/skel_index.json (kind -> skeleton id); a kind it does not list takes the first of its
skeleton ids with a file in packaging/.../gamedata/meshes/da/trees/*.dtsk.

Each species gets the architecture of its young tree (grown whole for the oversize bushes) and of the levels grafted
on a stock skeleton, its drawing keys, its bark zones, its season keys from the zone's phenology and its colours from
its photos (the shrubs' calibration); communities give the generic broadleaf models their species by habitat.

    python tools/trees/make_trees.py
"""
import colorsys
import json
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
DATA = os.path.join(HERE, 'data')
GAMEDATA = os.path.join(REPO, 'packaging', 'dead-air-x64', 'compatibility', 'gamedata')
OUT_LTX = os.path.join(GAMEDATA, 'configs', 'dead_air_x64_trees.ltx')
OUT_MD = os.path.join(REPO, 'docs', 'dead-air', 'TREE_DATA.md')
TREES_MD = os.path.join(REPO, 'docs', 'dead-air', 'TREES.md')
SKEL_DIR = os.path.join(GAMEDATA, 'meshes', 'da', 'trees')
TEX_DIR = os.path.join(REPO, 'tools', 'shrubs', 'textures')
TREE_LAYERS = os.path.join(TEX_DIR, 'out', 'tree_layers.json')

KINDS = json.load(open(os.path.join(DATA, 'tree_kinds.json'), encoding='utf-8'))
PHOTOS = json.load(open(os.path.join(DATA, 'tree_photo_colours.json'), encoding='utf-8'))['species']

MONTHS = ['jan', 'feb', 'mar', 'apr', 'may', 'jun', 'jul', 'aug', 'sep', 'oct', 'nov', 'dec']
MONTH_DAY = dict(zip(MONTHS, [15.5, 45.0, 74.5, 105.0, 135.5, 166.0, 196.5, 227.5, 258.0, 288.5, 319.0, 349.5]))

# ======================================================================================================
# Architecture: archetypes and the species' own keys
# ======================================================================================================
# The keys are da_shrub::Arch's (da_shrub_gen.h). A tree's levels serve twice: the young trees the oversize stock
# bushes become are grown whole from the root (stems or a leader, levels l1..l3 below them), and on a stock skeleton
# the stock trunk, limbs and card branches are level 0 (fixed) and l1..l3 are the grafted levels (graft_levels of them:
# branches, twigs, the current shoots). Lengths are shares of the parent, so one set fits both.


def level(density, start, end, angle, phyllo, len_, up, outward, wobble, radius, seg, min_len, per_node=1, jitter=0.3):
    return dict(density=density, start=start, end=end, angle=angle, phyllo=phyllo, per_node=per_node, len=len_,
                len_jitter=jitter, up=up, outward=outward, wobble=wobble, radius=radius, seg_len=seg, min_len=min_len)


def cm(a, b=None):
    return [round(a / 100.0, 4), round((b if b is not None else a) / 100.0, 4)]


def spray(n, spacing, per=1):
    """Leaves drawn as sprays of n at the species' node spacing (a mature crown's 10^5 leaves within the budget)."""
    return dict(leaf_kind='spray', leaflets=n, leaf_spacing=spacing, leaf_per_node=per, spray_len=round(spacing * n / per, 3))


# A broadleaf tree of one stem: an ovoid crown over a clear bole, three levels.
BROADLEAF = dict(href=10.0, envelope='ellipsoid', env_base=0.15, stems=[1, 1], root_r=0.06, stem_spread=[0, 6],
                 stem_len=[1.0, 1.0], stem_up=0.5, stem_arch=0.0, stem_wobble=0.08, stem_radius=0.09, stem_seg=0.3,
                 leader=0, whorl_step=0.0, levels=3, graft_levels=3,
                 l1=level(2.4, 0.25, 0.95, [55, 42], 137.5, [0.45, 0.2], 0.15, 0.25, 0.15, 0.5, 0.2, 0.18),
                 l2=level(4.0, 0.12, 1.0, [55, 45], 137.5, [0.45, 0.25], 0.1, 0.1, 0.18, 0.55, 0.09, 0.05),
                 l3=level(6.0, 0.2, 1.0, [50, 40], 137.5, [0.35, 0.2], 0.15, 0.0, 0.18, 0.6, 0.05, 0.03),
                 shoot_len=0.3, lai=5.0, fill_len=[0.15, 0.5], fill_shell=0.5, fill_clump=0.45, fill_up=0.2,
                 fill_from=0.25, spur_share=0.15, spur_leaves=[2, 4], leaf_spacing=0.03, leaf_per_node=1,
                 leaf_phyllo=137.5, leaf_angle=[50, 70], leaf_flat=0.6, leaf_droop=0.1, petiole=0.02, tip_shrink=0.6,
                 sag=0.6, zig=0.18)
# A young broadleaf with a persistent leader (birch, alder, aspen, the poplars, the young maples and ash).
EXCURRENT = dict(BROADLEAF, envelope='cone', env_base=0.2, leader=1, stem_up=0.7, stem_wobble=0.05)
# A conifer: one leader, a whorl of branches every year's growth.
CONIFER = dict(BROADLEAF, envelope='cone', env_base=0.3, leader=1, stem_up=0.85, stem_wobble=0.04, stem_radius=0.12,
               stem_seg=0.45, whorl_step=0.5, sag=0.5, zig=0.05, leaf_kind='needles', leaf_angle=[35, 55], leaf_flat=0.0,
               leaf_droop=0.04, tip_shrink=0.85, spur_share=0.0, spur_leaves=[0, 0])


def arch(base, **kw):
    a = dict(base)
    for lv in ('l1', 'l2', 'l3'):
        a[lv] = dict(base[lv]) if lv in base else None
    for k, v in kw.items():
        if k in ('l1', 'l2', 'l3') and isinstance(v, dict):
            a[k] = dict(a[k] or {}, **v)
        else:
            a[k] = v
    return a


SP = {}


def species(id_, latin, h, w, a, draw, **extra):
    """id: the data's species id (t_ where a shrub has it), latin, h: the real tree's height range (m), w: crown width
    as a share of the height, a: architecture, draw: drawing keys, extra: crown, bark, season and colour details
    (marcescent: the share of its leaves a young tree keeps withered through the winter - its last to fall - until
    bud burst; the engine scales it by the tree's youth, lerp(0.08, 1, y): an old tree keeps a twelfth of it)."""
    SP[id_] = dict(id=id_, latin=latin, h=h, w=w, arch=a, draw=draw, extra=extra)


# ---- Conifers ---------------------------------------------------------------------------------------------------
# The leaf area (lai over the crown's projection) of the pine and the birches sets their crowns' gap fraction at 25 m
# - the sky seen through the crown inside its silhouette - on the look-dev packs' medians (WP2 phase 3: pine 0.17,
# birch 0.19; the spruce's 0.07 at its lai already under its pack's 0.13).
# Scots pine: whorls of 4-6 branches at 60-90 deg (upper ascending, lower level with upturned tips), forest-grown a
# clear bole for two thirds of its height; needles in pairs, 4-7 cm, in tufts at the shoot ends - clumped, cloudy
# masses with sky between them (SPECIES.md pin_syl).
species('pin_syl', 'Pinus sylvestris L.', [15, 30], [0.25, 0.5],
        arch(CONIFER, href=15.0, env_base=0.3, stem_radius=0.14, whorl_step=0.55,
             l1=level(1.2, 0.35, 0.97, [80, 55], 137.5, [0.32, 0.12], 0.1, 0.1, 0.12, 0.4, 0.3, 0.3, per_node=5),
             l2=level(2.6, 0.35, 0.97, [55, 45], 180, [0.5, 0.25], 0.25, 0.0, 0.15, 0.55, 0.12, 0.1, per_node=2),
             l3=level(6.0, 0.5, 1.0, [45, 35], 137.5, [0.35, 0.2], 0.35, 0.0, 0.15, 0.6, 0.06, 0.04),
             shoot_len=0.4, lai=3.5, fill_len=[0.12, 0.35], fill_shell=0.4, fill_clump=0.85, fill_up=0.35, fill_from=0.3,
             leaf_spacing=0.12, leaf_per_node=2, needle_len=0.05, bloom_place='tip', bloom_share=0.25, bloom_size=0.03,
             bloom_droop=-0.6, fruit_size=0.045),
        dict(outline='narrow', gloss=0.15, translucency=0.1, flutter=0.04, fold=0.0, underside=[1.2, 0.15],
             jitter=[0.1, 0.03], bloom_shape='cone', petals=0, fruit_shape='cone', fruits=1, needle_density=1.0),
        evergreen=True, cone=(0.12, 0.045, 0.6), inner_needles=0.18, autumn_order=(0.1, 0.7), graft_levels=3,
        crown_fill=0.55, follow=0.8, dead=0.04, bark_zone=(3.5, 8.0), bark=[0.85, 0.1, 0.0], bark_scale=3.5,
        bark_contrast=1.3, lichen=0.25, twig_r=0.01,
        bark_tint=[0.92, 0.96, 1.0], bark_chroma_upper=1.0)
# Norway spruce: a narrow spire at every age, main whorls and inter-whorl branches, upper ascending, middle level,
# lower drooping with curtains of hanging second-order branchlets (comb type); needles single on pegs, 1.5-2.5 cm,
# all round the shoot; cones 10-16 cm hanging (SPECIES.md pic_abi).
species('pic_abi', 'Picea abies (L.) H.Karst.', [18, 30], [0.3, 0.4],
        arch(CONIFER, href=12.0, env_base=0.42, stem_radius=0.12, whorl_step=0.45,
             l1=level(2.5, 0.05, 0.98, [95, 60], 137.5, [0.35, 0.08], -0.1, 0.05, 0.08, 0.35, 0.25, 0.2, per_node=5),
             l2=level(5.0, 0.1, 0.95, [85, 75], 180, [0.5, 0.3], -3.0, 0.0, 0.1, 0.5, 0.1, 0.08),
             l3=level(7.0, 0.1, 1.0, [50, 40], 137.5, [0.4, 0.25], -0.8, 0.0, 0.12, 0.6, 0.05, 0.03),
             shoot_len=0.6, lai=6.0, fill_len=[0.1, 0.3], fill_shell=0.55, fill_clump=0.35, fill_up=0.1, fill_from=0.05,
             leaf_spacing=0.12, leaf_per_node=1, leaf_angle=[55, 75], needle_len=0.02, bloom_place='tip',
             bloom_share=0.15, bloom_size=0.02, bloom_droop=-0.8, fruit_size=0.13),
        dict(outline='narrow', gloss=0.35, translucency=0.08, flutter=0.03, fold=0.0, underside=[1.15, 0.05],
             jitter=[0.1, 0.03], bloom_shape='cone', petals=0, fruit_shape='cone', fruits=1, needle_density=1.2),
        evergreen=True, cone=(0.06, 0.13, 1.0), inner_needles=0.08, autumn_order=(0.1, 0.6), graft_levels=3,
        crown_fill=0.65, follow=0.8, dead=0.06, bark_zone=(2.0, 8.0), bark=[0.5, 0.15, 0.0], bark_scale=2.0,
        bark_contrast=1.2, lichen=0.4, twig_r=0.008, apex_cone=0.33, apex_leader=1.25)

# ---- Native forest trees ------------------------------------------------------------------------------------------
# Silver birch: acute branch angles (30-50 deg), a fine twig network carrying the long hanging shoots in weeping
# strands; rhombic leaves 3.5-7 cm on long petioles (they flicker); male catkins closed over the winter, open in April.
species('bet_pen', 'Betula pendula Roth', [16, 25], [0.35, 0.55],
        arch(EXCURRENT, href=10.0, stems=[1, 2], stem_radius=0.08, stem_up=0.6, stem_wobble=0.06,
             l1=level(3.5, 0.25, 0.97, [45, 32], 137.5, [0.35, 0.15], 0.35, 0.15, 0.15, 0.45, 0.25, 0.2),
             l2=level(5.0, 0.15, 1.0, [50, 40], 137.5, [0.5, 0.3], -0.9, 0.05, 0.2, 0.5, 0.1, 0.06),
             l3=level(8.0, 0.2, 1.0, [40, 30], 137.5, [0.5, 0.35], -3.5, 0.0, 0.2, 0.6, 0.06, 0.04),
             sag=0.8, **spray(6, 0.022), leaf_len=cm(3.5, 7), leaf_wid=cm(2.5, 5), petiole=0.022, leaf_angle=[40, 60],
             leaf_flat=0.45, lai=6.5, shoot_len=0.4, fill_len=[0.15, 0.5], fill_shell=0.5, fill_clump=0.45, fill_up=0.1,
             fill_from=0.3, spur_share=0.3, spur_leaves=[2, 3], catkin_share=0.3, catkin_len=0.06, catkin_droop=1.0,
             bloom_place='axil', bloom_share=0.08, bloom_size=0.02, fruit_size=0.03),
        dict(outline='ovate', gloss=0.2, translucency=0.6, flutter=0.25, fold=0.1, underside=[1.25, 0.05],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cone', fruits=1, catkin_shape='hanging'),
        catkin_winter=True, autumn_order=(0.45, 0.4), crown_fill=0.6, follow=0.7, dead=0.03, bark_zone=(0.5, 3.5),
        bark=[0.3, 0.2, 0.9], bark_scale=2.5, bark_contrast=1.2, lichen=0.2, twig_r=0.012,
        browning=0.1)
# Downy birch: rounder and denser than silver birch, the twigs not weeping, often several crooked stems on bogs; the
# base greyer, shallowly cracked (no thick black rhytidome); a week later.
species('bet_pub', 'Betula pubescens Ehrh.', [12, 20], [0.45, 0.7],
        arch(EXCURRENT, href=9.0, stems=[1, 3], stem_spread=[2, 15], leader=0, envelope='ellipsoid', stem_radius=0.08,
             stem_up=0.5, stem_wobble=0.1,
             l1=level(3.5, 0.2, 0.97, [50, 38], 137.5, [0.38, 0.16], 0.25, 0.2, 0.18, 0.45, 0.22, 0.18),
             l2=level(5.5, 0.12, 1.0, [50, 40], 137.5, [0.45, 0.25], 0.05, 0.08, 0.2, 0.5, 0.09, 0.05),
             l3=level(8.0, 0.2, 1.0, [45, 35], 137.5, [0.35, 0.2], 0.1, 0.0, 0.2, 0.6, 0.05, 0.03),
             **spray(6, 0.02), leaf_len=cm(3, 6), leaf_wid=cm(2.5, 4.5), petiole=0.015, leaf_angle=[45, 65],
             leaf_flat=0.55, lai=7.2, shoot_len=0.3, fill_shell=0.55, fill_clump=0.4, spur_share=0.3, spur_leaves=[2, 3],
             catkin_share=0.3, catkin_len=0.05, catkin_droop=1.0, bloom_place='axil', bloom_share=0.08, bloom_size=0.02,
             fruit_size=0.028),
        dict(outline='ovate', gloss=0.08, translucency=0.55, flutter=0.2, fold=0.1, underside=[1.3, 0.06],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cone', fruits=1, catkin_shape='hanging'),
        catkin_winter=True, autumn_order=(0.45, 0.4), crown_fill=0.7, follow=0.6, dead=0.03, bark_zone=(0.3, 1.2),
        bark=[0.3, 0.15, 0.8], bark_scale=2.5, bark_contrast=1.15, lichen=0.3, twig_r=0.01,
        browning=0.1)
# Aspen: a long clear bole, few thick ascending limbs, a sparse twig network, leaves clustered on short shoots on
# flattened petioles (the whole crown trembles); grey silky catkins in March, seed cotton in May; copper flush.
species('pop_tre', 'Populus tremula L.', [18, 25], [0.3, 0.45],
        arch(EXCURRENT, href=12.0, envelope='ellipsoid', env_base=0.15, stem_radius=0.09,
             l1=level(1.8, 0.4, 0.97, [45, 35], 137.5, [0.35, 0.15], 0.35, 0.2, 0.15, 0.5, 0.25, 0.25),
             l2=level(3.0, 0.15, 1.0, [50, 40], 137.5, [0.45, 0.25], 0.15, 0.05, 0.18, 0.55, 0.12, 0.08),
             l3=level(6.0, 0.2, 1.0, [45, 35], 137.5, [0.3, 0.15], 0.2, 0.0, 0.15, 0.6, 0.05, 0.03),
             **spray(5, 0.02), leaf_len=cm(3.5, 7), leaf_wid=cm(3.5, 7), petiole=0.05, leaf_angle=[45, 70],
             leaf_flat=0.4, leaf_droop=0.15, lai=3.8, shoot_len=0.25, fill_len=[0.12, 0.4], fill_clump=0.5,
             fill_up=0.25, fill_from=0.35, spur_share=0.35, spur_leaves=[3, 5], catkin_share=0.25, catkin_len=0.07,
             catkin_droop=1.0, bloom_place='axil', bloom_share=0.06, bloom_size=0.03, fruit_size=0.06),
        dict(outline='round', gloss=0.08, translucency=0.55, flutter=0.5, fold=0.06, underside=[1.3, 0.1],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='hanging'),
        young_copper=0.45, autumn_order=(0.45, 0.4), crown_fill=0.6, follow=0.6, dead=0.03, bark_zone=(0.4, 1.8),
        bark_zone_order='ridges',
        bark=[0.2, 0.3, 0.6], bark_scale=2.0, bark_contrast=1.15, lichen=0.35, browning=0.1)
# Pedunculate oak: wide branch angles, tortuous, nearly horizontal limbs, zigzag twigs branching in tufts; obovate
# lobed leaves 7-14 cm clustered at the twig ends; catkins with the leaf-out, acorns on long stalks; young oaks keep
# their dry brown leaves all winter (marcescence).
species('que_rob', 'Quercus robur L.', [20, 28], [0.55, 0.9],
        arch(BROADLEAF, href=10.0, stem_wobble=0.15, stem_radius=0.1, stem_up=0.3,
             l1=level(1.8, 0.25, 0.95, [65, 50], 137.5, [0.45, 0.2], 0.05, 0.3, 0.3, 0.5, 0.2, 0.2),
             l2=level(4.0, 0.1, 1.0, [60, 50], 137.5, [0.45, 0.25], 0.1, 0.15, 0.32, 0.55, 0.1, 0.06),
             l3=level(6.0, 0.4, 1.0, [55, 45], 137.5, [0.35, 0.2], 0.2, 0.05, 0.3, 0.6, 0.05, 0.03),
             zig=0.3, **spray(5, 0.012), leaf_len=cm(7, 14), leaf_wid=cm(4, 8), petiole=0.005, leaf_angle=[50, 75],
             leaf_flat=0.6, leaf_droop=0.08, lai=5.5, shoot_len=0.25, fill_len=[0.12, 0.4], fill_clump=0.55,
             fill_from=0.2, spur_share=0.25, spur_leaves=[4, 6], tip_shrink=0.7, catkin_share=0.2, catkin_len=0.05,
             catkin_droop=1.0, bloom_place='axil', bloom_share=0.06, bloom_size=0.01, bloom_droop=0.6,
             fruit_size=0.025),
        dict(outline='lobed', lobes=7, gloss=0.15, translucency=0.45, flutter=0.1, fold=0.12, underside=[1.25, 0.08],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='nut', fruits=2, catkin_shape='hanging'),
        young_copper=0.25, marcescent=0.6, autumn_order=(0.45, -0.3), crown_fill=0.75, follow=0.7, dead=0.02,
        bark_zone=(4.0, 10.0), bark=[0.9, 0.15, 0.1], bark_scale=3.0, bark_contrast=1.35, lichen=0.3,
        bark_tint=[1.04, 1.0, 1.0])
# Hornbeam: a dense ovoid crown of many fine two-ranked zigzag twigs, coppice regrowth multi-stemmed; ovate pleated
# leaves 5-10 cm; catkins with the leaves, hanging clusters of three-lobed bracts; smooth fluted grey bark all life.
species('car_bet', 'Carpinus betulus L.', [15, 20], [0.6, 0.8],
        arch(BROADLEAF, href=9.0, envelope='ellipsoid', env_base=0.2, stems=[1, 3], stem_spread=[2, 15], stem_radius=0.08,
             stem_up=0.4, stem_wobble=0.1,
             l1=level(3.0, 0.2, 0.95, [50, 38], 180, [0.42, 0.18], 0.2, 0.25, 0.15, 0.5, 0.2, 0.15),
             l2=level(6.0, 0.1, 1.0, [50, 40], 180, [0.45, 0.25], 0.05, 0.1, 0.2, 0.55, 0.08, 0.05),
             l3=level(9.0, 0.2, 1.0, [45, 35], 180, [0.35, 0.2], 0.1, 0.0, 0.2, 0.6, 0.04, 0.03),
             zig=0.25, **spray(6, 0.025), leaf_phyllo=180, leaf_len=cm(5, 10), leaf_wid=cm(3, 5), petiole=0.01,
             leaf_angle=[55, 75], leaf_flat=0.7, lai=6.0, fill_shell=0.55, fill_clump=0.35, spur_share=0.2,
             catkin_share=0.15, catkin_len=0.04, catkin_droop=1.0, bloom_place='tip', bloom_share=0.12, bloom_size=0.02,
             bloom_droop=0.9, fruit_size=0.1),
        dict(outline='elliptic', gloss=0.1, translucency=0.6, flutter=0.1, fold=0.18, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='pod', fruits=8, catkin_shape='hanging'),
        marcescent=0.4, autumn_order=(0.45, -0.2), crown_fill=0.85, follow=0.55, dead=0.02, bark_zone=None,
        bark=[0.15, 0.2, 0.2], bark_scale=2.0, bark_contrast=1.15, lichen=0.35)
# Norway maple: opposite, stout twigs, branches ascending then spreading into a dense rounded dome; palmate leaves
# 10-18 cm on long petioles; lime-green upright corymbs before the leaves, wide-angled samaras.
species('ace_pla', 'Acer platanoides L.', [15, 25], [0.6, 0.85],
        arch(EXCURRENT, href=10.0, envelope='ellipsoid', env_base=0.15, stem_radius=0.09, stem_up=0.6,
             l1=level(2.5, 0.25, 0.95, [55, 40], 90, [0.45, 0.2], 0.25, 0.25, 0.12, 0.5, 0.2, 0.2, per_node=2),
             l2=level(4.0, 0.1, 1.0, [55, 45], 90, [0.45, 0.25], 0.1, 0.1, 0.15, 0.55, 0.1, 0.06, per_node=2),
             l3=level(6.0, 0.2, 1.0, [50, 40], 90, [0.35, 0.2], 0.15, 0.0, 0.15, 0.6, 0.05, 0.03, per_node=2),
             leaf_kind='simple', leaf_len=cm(10, 18), leaf_wid=cm(12, 20), petiole=0.09, leaf_per_node=2,
             leaf_phyllo=90, leaf_spacing=0.06, leaf_angle=[55, 75], leaf_flat=0.75, leaf_droop=0.08, lai=5.8,
             shoot_len=0.3, fill_clump=0.35, spur_share=0.15, spur_leaves=[2, 2], bloom_place='tip', bloom_share=0.45,
             bloom_size=0.07, bloom_droop=-0.3, fruit_size=0.08),
        dict(outline='lobed', lobes=5, gloss=0.2, translucency=0.6, flutter=0.1, fold=0.08, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='corymb', petals=5, fruit_shape='pod', fruits=6),
        autumn_order=(0.45, -0.35), crown_fill=0.85, follow=0.55, dead=0.01, bark_zone=(2.0, 6.0),
        bark=[0.6, 0.15, 0.05], bark_scale=2.5, bark_contrast=1.2, lichen=0.25)
# Small-leaved lime: a dense ovoid crown, two-ranked zigzag twigs, the lower branches arching down, sprouts at the
# base; heart-shaped leaves 3-8 cm, bluish-white below; scented pale yellow flowers in late June, nutlets on bracts.
species('til_cor', 'Tilia cordata Mill.', [15, 25], [0.55, 0.75],
        arch(BROADLEAF, href=10.0, env_base=0.2, stems=[1, 3], stem_spread=[2, 12], stem_radius=0.09,
             l1=level(3.0, 0.2, 0.96, [55, 40], 180, [0.42, 0.18], 0.05, 0.25, 0.15, 0.5, 0.2, 0.15),
             l2=level(6.0, 0.1, 1.0, [50, 40], 180, [0.45, 0.25], -0.1, 0.1, 0.2, 0.55, 0.08, 0.05),
             l3=level(8.0, 0.2, 1.0, [45, 35], 180, [0.35, 0.2], 0.0, 0.0, 0.2, 0.6, 0.04, 0.03),
             zig=0.25, **spray(6, 0.03), leaf_phyllo=180, leaf_len=cm(3, 8), leaf_wid=cm(3, 7), petiole=0.03,
             leaf_angle=[55, 75], leaf_flat=0.7, lai=6.2, fill_shell=0.55, fill_clump=0.3, bloom_place='axil',
             bloom_share=0.35, bloom_size=0.06, bloom_droop=0.8, fruit_size=0.06),
        dict(outline='round', gloss=0.1, translucency=0.55, flutter=0.12, fold=0.08, underside=[1.5, 0.25],
             jitter=[0.15, 0.06], bloom_shape='corymb', petals=5, fruit_shape='nut', fruits=4),
        autumn_order=(0.45, -0.2), crown_fill=0.85, follow=0.55, dead=0.01, bark_zone=(2.0, 6.0),
        bark=[0.5, 0.2, 0.1], bark_scale=2.5, bark_contrast=1.15, lichen=0.3, fruit_winter=True)
# European ash: a high open crown on a long clean bole, few stout opposite branches upturned at the ends
# (candelabra), thick grey-green twigs with black buds; pinnate leaves 20-35 cm of 9-13 leaflets, out late (mid May),
# falling green or pale yellow; purple-black flower tufts before the leaves, samara bunches hanging all winter.
species('fra_exc', 'Fraxinus excelsior L.', [15, 25], [0.45, 0.65],
        arch(EXCURRENT, href=10.0, envelope='ellipsoid', env_base=0.15, stem_radius=0.09, stem_up=0.6,
             l1=level(1.6, 0.35, 0.95, [50, 38], 90, [0.42, 0.18], 0.35, 0.2, 0.12, 0.55, 0.25, 0.2, per_node=2),
             l2=level(2.5, 0.12, 1.0, [55, 45], 90, [0.4, 0.25], 0.45, 0.05, 0.12, 0.6, 0.12, 0.08, per_node=2),
             l3=level(3.0, 0.2, 1.0, [45, 35], 90, [0.3, 0.2], 0.4, 0.0, 0.12, 0.65, 0.06, 0.04, per_node=2),
             leaf_kind='compound', leaflets=11, rachis=0.18, leaflet_angle=70, leaf_len=cm(5, 11), leaf_wid=cm(1.5, 3),
             petiole=0.05, leaf_per_node=2, leaf_phyllo=90, leaf_spacing=0.07, leaf_angle=[45, 65], leaf_flat=0.55,
             leaf_droop=0.15, lai=4.0, fill_shell=0.45, fill_clump=0.5, spur_share=0.0, bloom_place='axil',
             bloom_share=0.25, bloom_size=0.05, bloom_droop=0.2, fruit_size=0.12),
        dict(outline='lanceolate', gloss=0.3, translucency=0.55, flutter=0.15, fold=0.25, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='panicle', petals=0, fruit_shape='pod', fruits=12, leaflet_ratio=0.3),
        autumn_green=0.45, fruit_winter=True, autumn_order=(0.4, 0.0), graft_levels=2, crown_fill=0.55, follow=0.55,
        dead=0.08, bark_zone=(2.0, 6.0), bark=[0.55, 0.25, 0.05], bark_scale=2.5, bark_contrast=1.2, lichen=0.3)
# European white elm: a broad rounded open crown, spreading branches, fine dense two-ranked zigzag twigs; elliptic
# leaves 6-12 cm with an oblique base; long-stalked hanging flower tassels in March, green samaras haze the bare
# crown in late April and fall in May.
species('ulm_lae', 'Ulmus laevis Pall.', [15, 25], [0.6, 0.9],
        arch(BROADLEAF, href=10.0, stem_radius=0.09, stem_up=0.4, stem_wobble=0.12,
             l1=level(2.5, 0.25, 0.95, [55, 45], 180, [0.45, 0.2], 0.05, 0.3, 0.15, 0.5, 0.2, 0.15),
             l2=level(5.5, 0.1, 1.0, [50, 40], 180, [0.45, 0.25], -0.1, 0.1, 0.2, 0.55, 0.08, 0.05),
             l3=level(8.0, 0.2, 1.0, [45, 35], 180, [0.35, 0.2], 0.0, 0.0, 0.2, 0.6, 0.04, 0.03),
             zig=0.3, **spray(6, 0.025), leaf_phyllo=180, leaf_len=cm(6, 12), leaf_wid=cm(3, 6), petiole=0.006,
             leaf_angle=[55, 75], leaf_flat=0.7, lai=5.0, bloom_place='old_wood', bloom_share=0.3, bloom_size=0.04,
             bloom_droop=0.9, fruit_size=0.05),
        dict(outline='elliptic', gloss=0.08, translucency=0.5, flutter=0.1, fold=0.12, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='umbel', petals=0, fruit_shape='pod', fruits=12),
        autumn_order=(0.45, 0.0), crown_fill=0.75, follow=0.55, dead=0.08, bark_zone=None, bark=[0.6, 0.1, 0.05],
        bark_scale=2.5, bark_contrast=1.2, lichen=0.3)
# Field elm: mostly suckering stems of 5-15 m (Dutch elm disease), dying stems with dead twigs common; small glossy
# leaves 4-8 cm, corky twigs.
species('ulm_min', 'Ulmus minor Mill.', [5, 15], [0.5, 0.8],
        arch(BROADLEAF, href=8.0, stems=[1, 4], stem_spread=[3, 20], stem_radius=0.07, stem_up=0.5, stem_wobble=0.1,
             l1=level(2.8, 0.2, 0.95, [55, 45], 180, [0.42, 0.18], 0.1, 0.25, 0.15, 0.5, 0.18, 0.15),
             l2=level(5.5, 0.1, 1.0, [50, 40], 180, [0.45, 0.25], 0.0, 0.1, 0.2, 0.55, 0.08, 0.05),
             l3=level(8.0, 0.2, 1.0, [45, 35], 180, [0.35, 0.2], 0.05, 0.0, 0.2, 0.6, 0.04, 0.03),
             zig=0.3, **spray(6, 0.02), leaf_phyllo=180, leaf_len=cm(4, 8), leaf_wid=cm(2, 4), petiole=0.008,
             leaf_angle=[55, 75], leaf_flat=0.7, lai=4.5, bloom_place='old_wood', bloom_share=0.3, bloom_size=0.03,
             bloom_droop=0.5, fruit_size=0.04),
        dict(outline='elliptic', gloss=0.3, translucency=0.5, flutter=0.1, fold=0.12, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='umbel', petals=0, fruit_shape='pod', fruits=10),
        autumn_order=(0.45, 0.0), crown_fill=0.7, follow=0.55, dead=0.15, bark_zone=None, bark=[0.65, 0.1, 0.0],
        bark_scale=2.0, bark_contrast=1.2, lichen=0.3)
# Rowan (tree form): a loose airy ovoid crown, ascending branches, often several stems; pinnate leaves of 9-19
# leaflets; white corymbs in mid May, berry clusters orange then red, hanging into the winter.
species('sor_auc', 'Sorbus aucuparia L.', [5, 12], [0.5, 0.7],
        arch(BROADLEAF, href=7.0, stems=[1, 3], stem_spread=[2, 15], stem_radius=0.06,
             l1=level(2.2, 0.25, 0.95, [45, 35], 137.5, [0.42, 0.18], 0.3, 0.2, 0.15, 0.5, 0.18, 0.15),
             l2=level(3.5, 0.12, 1.0, [45, 38], 137.5, [0.45, 0.25], 0.2, 0.05, 0.18, 0.55, 0.08, 0.05),
             l3=level(4.0, 0.2, 1.0, [40, 30], 137.5, [0.35, 0.2], 0.25, 0.0, 0.15, 0.6, 0.04, 0.03),
             leaf_kind='compound', leaflets=13, rachis=0.13, leaflet_angle=72, leaf_len=cm(3, 6), leaf_wid=cm(1, 1.8),
             petiole=0.03, leaf_spacing=0.04, leaf_angle=[40, 60], leaf_flat=0.55, lai=3.6, fill_clump=0.45,
             spur_share=0.0, bloom_place='tip', bloom_share=0.3, bloom_size=0.1, bloom_droop=-0.3, fruit_size=0.1),
        dict(outline='lanceolate', gloss=0.06, translucency=0.55, flutter=0.12, fold=0.25, underside=[1.25, 0.1],
             jitter=[0.15, 0.06], bloom_shape='corymb', petals=5, fruit_shape='berry', fruits=30, leaflet_ratio=0.3),
        fruit_winter=True, autumn_order=(0.4, 0.2), crown_fill=0.6, follow=0.55, dead=0.03, bark_zone=(0.5, 1.5),
        bark=[0.15, 0.4, 0.6], bark_scale=2.0, bark_contrast=1.1, lichen=0.3)

# ---- Floodplain and wetland trees ---------------------------------------------------------------------------------
# Black alder: a narrow cone with a persistent leader when young, branches near-perpendicular in regular tiers;
# rounded-obovate notched leaves 4-9 cm, dark and glossy, falling green or brownish; male catkins purple-brown all
# winter, open in March; small woody cones, green then black, on the tree all year.
species('aln_glu', 'Alnus glutinosa (L.) Gaertn.', [18, 25], [0.4, 0.55],
        arch(EXCURRENT, href=10.0, stem_radius=0.08, stem_up=0.7,
             l1=level(3.0, 0.2, 0.97, [80, 60], 137.5, [0.32, 0.12], 0.0, 0.1, 0.1, 0.45, 0.2, 0.15),
             l2=level(4.5, 0.12, 1.0, [55, 45], 137.5, [0.45, 0.25], 0.05, 0.05, 0.15, 0.55, 0.08, 0.05),
             l3=level(7.0, 0.2, 1.0, [45, 35], 137.5, [0.35, 0.2], 0.1, 0.0, 0.15, 0.6, 0.04, 0.03),
             **spray(5, 0.025), leaf_len=cm(4, 9), leaf_wid=cm(4, 8), petiole=0.02, leaf_angle=[50, 70], leaf_flat=0.6,
             lai=5.2, fill_shell=0.55, fill_clump=0.35, spur_share=0.2, catkin_share=0.3, catkin_len=0.08,
             catkin_droop=1.0, bloom_place='axil', bloom_share=0.1, bloom_size=0.01, fruit_size=0.018),
        dict(outline='round', gloss=0.35, translucency=0.45, flutter=0.1, fold=0.06, underside=[1.2, 0.04],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cone', fruits=4, catkin_shape='hanging'),
        catkin_winter=True, fruit_all_year=True, autumn_green=0.6, autumn_order=(0.3, 0.2), crown_fill=0.8, follow=0.75,
        dead=0.03, bark_zone=(3.0, 8.0), bark=[0.6, 0.35, 0.5], bark_scale=2.5, bark_contrast=1.2, lichen=0.35)
# White willow: a broad rounded to irregular crown, often leaning or several stems, ascending branches, long flexible
# twigs with drooping tips; lanceolate leaves 5-10 cm, silky white below (the crown flashes silver); catkins with the
# leaves, seed cotton in late May.
species('sal_alb', 'Salix alba L.', [15, 25], [0.7, 1.0],
        arch(BROADLEAF, href=10.0, env_base=0.3, stems=[1, 3], stem_spread=[5, 25], stem_radius=0.12, stem_up=0.2,
             stem_wobble=0.15,
             l1=level(2.0, 0.25, 0.95, [45, 35], 137.5, [0.5, 0.25], 0.3, 0.3, 0.15, 0.5, 0.25, 0.2),
             l2=level(4.0, 0.12, 1.0, [40, 30], 137.5, [0.5, 0.3], 0.1, 0.1, 0.15, 0.5, 0.12, 0.08),
             l3=level(6.0, 0.2, 1.0, [35, 25], 137.5, [0.5, 0.35], -0.2, 0.0, 0.15, 0.55, 0.06, 0.05),
             sag=0.7, **spray(7, 0.015), leaf_len=cm(5, 10), leaf_wid=cm(1, 2), petiole=0.006, leaf_angle=[30, 50],
             leaf_flat=0.35, leaf_droop=0.15, lai=4.2, catkin_share=0.2, catkin_len=0.05, catkin_droop=-0.3,
             bloom_place='axil', bloom_share=0.05, bloom_size=0.03, fruit_size=0.04),
        dict(outline='lanceolate', gloss=0.1, translucency=0.55, flutter=0.22, fold=0.15, underside=[2.0, 0.18],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'),
        autumn_order=(0.45, 0.4), crown_fill=0.75, follow=0.7, dead=0.05, bark_zone=(4.0, 10.0),
        bark=[0.85, 0.3, 0.0], bark_scale=3.0, bark_contrast=1.3, lichen=0.4)
# Crack willow: broad irregular ragged crowns along rivers, ditches and village roads, often leaning, broken and
# resprouting; glossy olive-brown brittle twigs; lanceolate leaves 9-15 cm, glossy green, little silver.
species('sal_fra', 'Salix x fragilis L.', [15, 22], [0.7, 1.0],
        arch(BROADLEAF, href=10.0, env_base=0.3, stems=[1, 3], stem_spread=[5, 30], stem_radius=0.12, stem_up=0.2,
             stem_wobble=0.18,
             l1=level(2.0, 0.25, 0.95, [50, 40], 137.5, [0.5, 0.25], 0.25, 0.3, 0.2, 0.5, 0.25, 0.2),
             l2=level(4.0, 0.12, 1.0, [45, 35], 137.5, [0.5, 0.3], 0.15, 0.1, 0.2, 0.5, 0.12, 0.08),
             l3=level(6.0, 0.2, 1.0, [40, 30], 137.5, [0.45, 0.3], 0.0, 0.0, 0.18, 0.55, 0.06, 0.05),
             sag=0.6, **spray(7, 0.018), leaf_len=cm(9, 15), leaf_wid=cm(1.5, 3), petiole=0.01, leaf_angle=[30, 50],
             leaf_flat=0.4, leaf_droop=0.12, lai=4.5, catkin_share=0.2, catkin_len=0.05, catkin_droop=-0.3,
             bloom_place='axil', bloom_share=0.05, bloom_size=0.03, fruit_size=0.04),
        dict(outline='lanceolate', gloss=0.35, translucency=0.5, flutter=0.18, fold=0.12, underside=[1.25, 0.08],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'),
        autumn_order=(0.45, 0.4), crown_fill=0.75, follow=0.7, dead=0.06, bark_zone=(4.0, 10.0),
        bark=[0.85, 0.35, 0.0], bark_scale=3.0, bark_contrast=1.3, lichen=0.4)
# Black poplar with the Lombardy poplar (var. italica): the stock poplar cards are fastigiate, so the Lombardy form:
# branches pressed to the stem, a narrow dark column; triangular-rhombic leaves 5-10 cm on flat petioles (trembling);
# red male catkins in late March, poplar fluff in late May.
species('pop_nig', 'Populus nigra L. (incl. var. italica)', [20, 30], [0.2, 0.5],
        arch(EXCURRENT, href=12.0, envelope='column', env_base=0.3, stem_radius=0.1, stem_up=0.8,
             l1=level(3.5, 0.1, 0.97, [25, 15], 137.5, [0.3, 0.12], 0.6, 0.0, 0.08, 0.45, 0.25, 0.2),
             l2=level(4.5, 0.12, 1.0, [30, 22], 137.5, [0.45, 0.25], 0.5, 0.0, 0.1, 0.5, 0.1, 0.06),
             l3=level(6.0, 0.2, 1.0, [35, 25], 137.5, [0.3, 0.2], 0.4, 0.0, 0.12, 0.6, 0.05, 0.03),
             **spray(5, 0.025), leaf_len=cm(5, 10), leaf_wid=cm(4, 8), petiole=0.04, leaf_angle=[45, 70],
             leaf_flat=0.45, leaf_droop=0.15, lai=4.5, spur_share=0.3, spur_leaves=[3, 5], fill_up=0.4,
             catkin_share=0.25, catkin_len=0.07, catkin_droop=1.0, bloom_place='axil', bloom_share=0.05,
             bloom_size=0.03, fruit_size=0.05),
        dict(outline='ovate', gloss=0.3, translucency=0.5, flutter=0.35, fold=0.06, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='hanging'),
        autumn_order=(0.45, 0.4), crown_fill=0.7, follow=0.75, dead=0.03, bark_zone=(3.0, 9.0),
        bark=[0.85, 0.2, 0.1], bark_scale=3.0, bark_contrast=1.3, lichen=0.25)
# White poplar: a broad open irregular crown, white-felted young twigs, lobed leaves white-felted below (the crown
# flashes white in the wind); bark whitish with black diamond lenticels above a dark fissured base.
species('pop_alb', 'Populus alba L.', [20, 30], [0.6, 0.9],
        arch(BROADLEAF, href=12.0, stems=[1, 2], stem_spread=[2, 12], stem_radius=0.1, stem_up=0.4,
             l1=level(2.0, 0.3, 0.95, [50, 40], 137.5, [0.45, 0.2], 0.2, 0.3, 0.18, 0.5, 0.25, 0.2),
             l2=level(3.5, 0.12, 1.0, [50, 40], 137.5, [0.45, 0.25], 0.15, 0.1, 0.2, 0.55, 0.1, 0.06),
             l3=level(5.0, 0.2, 1.0, [45, 35], 137.5, [0.3, 0.2], 0.2, 0.0, 0.18, 0.6, 0.05, 0.03),
             **spray(4, 0.03), leaf_len=cm(6, 12), leaf_wid=cm(5, 10), petiole=0.04, leaf_angle=[45, 70],
             leaf_flat=0.5, lai=4.2, spur_share=0.25, catkin_share=0.2, catkin_len=0.07, catkin_droop=1.0,
             bloom_place='axil', bloom_share=0.05, bloom_size=0.03, fruit_size=0.05),
        # (the felt under the leaf sends most of the light back: translucency under an aspen's 0.55; its white is a
        # close leaf's - the undersides a crown shows from below lie in its shade, at 2.6 every white poplar read
        # beige-white against the sky)
        dict(outline='lobed', lobes=5, gloss=0.15, translucency=0.3, flutter=0.3, fold=0.08, underside=[1.6, 0.08],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='hanging'),
        autumn_order=(0.45, 0.4), crown_fill=0.65, follow=0.6, dead=0.03, bark_zone=(1.0, 4.0),
        bark_zone_order='ridges',
        bark=[0.25, 0.3, 0.7], bark_scale=2.5, bark_contrast=1.2, lichen=0.25)
# Hybrid black poplar: tall fan-shaped crowns of straight ascending branches, wider at the top, planted in rows along
# roads and farm yards; stout angled twigs; triangular leaves 7-10 cm, out late with a bronze flush; red male catkins
# (the planted clones are male: no fluff).
species('pop_can', 'Populus x canadensis Moench', [25, 35], [0.5, 0.8],
        arch(BROADLEAF, href=14.0, envelope='vase', env_base=0.35, stem_radius=0.12, stem_up=0.6, stem_wobble=0.05,
             l1=level(1.8, 0.3, 0.95, [40, 30], 137.5, [0.5, 0.25], 0.4, 0.25, 0.08, 0.5, 0.3, 0.25),
             l2=level(3.0, 0.12, 1.0, [45, 35], 137.5, [0.45, 0.25], 0.3, 0.05, 0.1, 0.55, 0.12, 0.08),
             l3=level(5.0, 0.2, 1.0, [40, 30], 137.5, [0.22, 0.14], 0.3, 0.0, 0.1, 0.6, 0.05, 0.03),
             zig=0.2, **spray(4, 0.03), leaf_len=cm(7, 10), leaf_wid=cm(6, 9), petiole=0.05, leaf_angle=[45, 70],
             leaf_flat=0.45, leaf_droop=0.12, lai=5.5, spur_share=0.25, catkin_share=0.25, catkin_len=0.08,
             catkin_droop=1.0),
        dict(outline='ovate', gloss=0.3, translucency=0.5, flutter=0.35, fold=0.05, underside=[1.15, 0.03],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='none', fruits=1, catkin_shape='hanging'),
        young_copper=0.4, autumn_order=(0.45, 0.35), crown_fill=0.75, follow=0.55, dead=0.04, bark_zone=(3.0, 9.0),
        bark=[0.8, 0.2, 0.1], bark_scale=3.0, bark_contrast=1.25, lichen=0.2)

# ---- Village, roadside and planted trees ---------------------------------------------------------------------------
# Black locust: an open airy irregular crown in tiers, twisted leaning often multi-stemmed trunks, zigzag twigs with
# paired stipular thorns; pinnate leaves 15-30 cm of 9-23 oval leaflets (a see-through crown); white fragrant hanging
# racemes in late May, flat dark pods all winter; rope-furrowed bark from an early age.
species('rob_pse', 'Robinia pseudoacacia L.', [10, 20], [0.5, 0.8],
        arch(BROADLEAF, href=10.0, stems=[1, 3], stem_spread=[3, 20], stem_radius=0.08, stem_up=0.3, stem_wobble=0.2,
             l1=level(1.8, 0.3, 0.95, [55, 45], 137.5, [0.45, 0.2], 0.15, 0.3, 0.3, 0.5, 0.2, 0.2),
             l2=level(3.0, 0.12, 1.0, [55, 45], 137.5, [0.45, 0.25], 0.1, 0.1, 0.3, 0.55, 0.08, 0.06),
             l3=level(4.0, 0.2, 1.0, [50, 40], 137.5, [0.3, 0.2], 0.15, 0.0, 0.25, 0.6, 0.05, 0.03),
             zig=0.35, prickle_spacing=0.06, prickle_len=0.012, prickle_base=0.3, prickle_young=1.5,
             leaf_kind='compound', leaflets=15, rachis=0.18, leaflet_angle=75, leaf_len=cm(2.5, 5), leaf_wid=cm(1.5, 2.5),
             petiole=0.03, leaf_spacing=0.05, leaf_angle=[40, 65], leaf_flat=0.55, leaf_droop=0.15, lai=3.6,
             fill_shell=0.45, fill_clump=0.5, spur_share=0.0, bloom_place='axil', bloom_share=0.4, bloom_size=0.15,
             bloom_droop=1.0, fruit_size=0.08),
        dict(outline='elliptic', gloss=0.06, translucency=0.55, flutter=0.15, fold=0.25, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='raceme', petals=5, fruit_shape='pod', fruits=3, leaflet_ratio=0.6),
        fruit_winter=True, autumn_green=0.4, autumn_order=(0.4, 0.2), graft_levels=2, crown_fill=0.5, follow=0.55,
        dead=0.03, bark_zone=None, bark=[0.9, 0.1, 0.0], bark_scale=3.0, bark_contrast=1.35, lichen=0.15)
# Box elder: an irregular sprawling crown, a short trunk splitting into curved limbs, stems leaning and with age
# almost lying, dense water sprouts; opposite green violet-bloomed twigs; pinnate leaves of 3-5 leaflets; yellow-green
# tassels before the leaves, samara bunches hanging all winter.
species('ace_neg', 'Acer negundo L.', [9, 17], [0.7, 1.1],
        arch(BROADLEAF, href=9.0, env_base=0.3, stems=[1, 3], stem_spread=[10, 40], stem_radius=0.08, stem_up=0.15,
             stem_arch=0.2, stem_wobble=0.18,
             l1=level(2.2, 0.15, 0.95, [50, 40], 90, [0.45, 0.22], 0.1, 0.3, 0.25, 0.5, 0.2, 0.15, per_node=2),
             l2=level(4.0, 0.1, 1.0, [50, 40], 90, [0.45, 0.25], 0.2, 0.1, 0.2, 0.55, 0.08, 0.05, per_node=2),
             l3=level(5.0, 0.2, 1.0, [45, 35], 90, [0.3, 0.2], 0.2, 0.0, 0.2, 0.6, 0.05, 0.03, per_node=2),
             leaf_kind='compound', leaflets=5, rachis=0.08, leaflet_angle=60, leaf_len=cm(5, 10), leaf_wid=cm(3, 5),
             petiole=0.06, leaf_per_node=2, leaf_phyllo=90, leaf_spacing=0.07, leaf_angle=[45, 65], leaf_flat=0.6,
             lai=4.5, spur_share=0.0, bloom_place='axil', bloom_share=0.35, bloom_size=0.1, bloom_droop=1.0,
             fruit_size=0.1),
        dict(outline='ovate', gloss=0.06, translucency=0.55, flutter=0.12, fold=0.25, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='raceme', petals=0, fruit_shape='pod', fruits=10, leaflet_ratio=0.55),
        fruit_winter=True, autumn_order=(0.4, 0.1), crown_fill=0.7, follow=0.55, dead=0.03, bark_zone=(1.0, 3.0),
        bark=[0.45, 0.3, 0.1], bark_scale=2.0, bark_contrast=1.15, lichen=0.15)
# Sycamore maple: a broad dense dome; opposite stout twigs with green buds; dark coarse-toothed five-lobed leaves
# 10-16 cm, glaucous below; hanging yellow-green racemes in May, samaras at an acute angle; grey bark flaking in
# irregular plates on old trees.
species('ace_pse', 'Acer pseudoplatanus L.', [20, 30], [0.6, 0.85],
        arch(EXCURRENT, href=10.0, envelope='ellipsoid', env_base=0.15, stem_radius=0.1, stem_up=0.55,
             l1=level(2.3, 0.25, 0.95, [55, 42], 90, [0.45, 0.2], 0.2, 0.3, 0.12, 0.5, 0.2, 0.2, per_node=2),
             l2=level(4.0, 0.1, 1.0, [55, 45], 90, [0.45, 0.25], 0.1, 0.1, 0.15, 0.55, 0.1, 0.06, per_node=2),
             l3=level(5.5, 0.2, 1.0, [50, 40], 90, [0.35, 0.2], 0.15, 0.0, 0.15, 0.6, 0.05, 0.03, per_node=2),
             leaf_kind='simple', leaf_len=cm(10, 16), leaf_wid=cm(10, 18), petiole=0.1, leaf_per_node=2,
             leaf_phyllo=90, leaf_spacing=0.07, leaf_angle=[55, 75], leaf_flat=0.75, leaf_droop=0.08, lai=5.8,
             fill_clump=0.35, spur_share=0.12, spur_leaves=[2, 2], bloom_place='tip', bloom_share=0.35,
             bloom_size=0.1, bloom_droop=0.8, fruit_size=0.07),
        dict(outline='lobed', lobes=5, gloss=0.12, translucency=0.5, flutter=0.1, fold=0.08, underside=[1.4, 0.2],
             jitter=[0.15, 0.06], bloom_shape='raceme', petals=5, fruit_shape='pod', fruits=8),
        autumn_order=(0.45, -0.35), crown_fill=0.85, follow=0.55, dead=0.02, bark_zone=(2.0, 5.0),
        bark=[0.55, 0.15, 0.05], bark_scale=2.5, bark_contrast=1.2, lichen=0.25)
# Horse chestnut: a broad dense ovoid dome, upswept branches with hanging branchlets turning up at their tips,
# opposite stout twigs with big sticky buds; palmate leaves of 5-7 leaflets 10-25 cm, browned by the leaf miner from
# July; white candles in May, spiny capsules and conkers in September.
species('aes_hip', 'Aesculus hippocastanum L.', [20, 25], [0.6, 0.85],
        arch(BROADLEAF, href=10.0, stem_radius=0.1, stem_up=0.5,
             l1=level(1.8, 0.25, 0.95, [50, 40], 90, [0.45, 0.2], 0.3, 0.25, 0.12, 0.55, 0.2, 0.2, per_node=2),
             l2=level(2.5, 0.12, 1.0, [60, 50], 90, [0.45, 0.25], -0.3, 0.05, 0.12, 0.6, 0.1, 0.08, per_node=2),
             l3=level(3.0, 0.2, 1.0, [45, 35], 90, [0.3, 0.2], 0.3, 0.0, 0.12, 0.65, 0.05, 0.04, per_node=2),
             leaf_kind='compound', leaflets=7, rachis=0.01, leaflet_angle=85, leaf_len=cm(10, 22), leaf_wid=cm(4, 8),
             petiole=0.12, leaf_per_node=2, leaf_phyllo=90, leaf_spacing=0.08, leaf_angle=[50, 70], leaf_flat=0.7,
             lai=6.5, spur_share=0.0, bloom_place='tip', bloom_share=0.5, bloom_size=0.22, bloom_droop=-1.0,
             fruit_size=0.05),
        dict(outline='ovate', gloss=0.1, translucency=0.5, flutter=0.08, fold=0.2, underside=[1.2, 0.04],
             jitter=[0.15, 0.06], bloom_shape='panicle', petals=5, fruit_shape='capsule', fruits=2, leaflet_ratio=0.4),
        autumn_order=(0.45, -0.2), graft_levels=2, crown_fill=0.9, follow=0.55, dead=0.03, bark_zone=(2.0, 5.0),
        bark=[0.6, 0.3, 0.05], bark_scale=2.5, bark_contrast=1.2, lichen=0.2)
# Red oak: a broad, rounded, regular crown, straighter and less tortuous than the pedunculate oak; large leaves with
# bristle-tipped lobes, red to brown in October; acorns ripening in the second year; smooth grey bark for long.
species('que_rub', 'Quercus rubra L.', [20, 25], [0.7, 0.9],
        arch(BROADLEAF, href=10.0, stem_radius=0.1, stem_up=0.5, stem_wobble=0.08,
             l1=level(2.0, 0.25, 0.95, [55, 45], 137.5, [0.45, 0.2], 0.15, 0.3, 0.15, 0.5, 0.2, 0.2),
             l2=level(4.0, 0.1, 1.0, [55, 45], 137.5, [0.45, 0.25], 0.1, 0.1, 0.18, 0.55, 0.1, 0.06),
             l3=level(6.0, 0.2, 1.0, [50, 40], 137.5, [0.35, 0.2], 0.15, 0.0, 0.18, 0.6, 0.05, 0.03),
             leaf_kind='simple', leaf_len=cm(12, 22), leaf_wid=cm(8, 14), petiole=0.03, leaf_spacing=0.03,
             leaf_angle=[50, 75], leaf_flat=0.65, lai=5.5, spur_share=0.25, spur_leaves=[3, 5], catkin_share=0.2,
             catkin_len=0.06, catkin_droop=1.0, bloom_place='axil', bloom_share=0.05, bloom_size=0.01,
             fruit_size=0.022),
        dict(outline='lobed', lobes=7, gloss=0.2, translucency=0.5, flutter=0.1, fold=0.1, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='nut', fruits=1, catkin_shape='hanging'),
        marcescent=0.5, autumn_order=(0.45, -0.3), crown_fill=0.8, follow=0.55, dead=0.01, bark_zone=(3.0, 8.0),
        bark=[0.45, 0.2, 0.05], bark_scale=2.5, bark_contrast=1.15, lichen=0.2)
# Green ash: like a small, messy ash, often multi-stemmed; brown buds; pinnate leaves of 7-9 leaflets, yellow in
# autumn; samaras persisting in the winter; grey-brown diamond-furrowed bark.
species('fra_pen', 'Fraxinus pennsylvanica Marshall', [15, 20], [0.5, 0.75],
        arch(BROADLEAF, href=9.0, stems=[1, 3], stem_spread=[3, 18], stem_radius=0.08, stem_up=0.45,
             l1=level(1.8, 0.3, 0.95, [50, 40], 90, [0.42, 0.2], 0.3, 0.25, 0.15, 0.55, 0.2, 0.2, per_node=2),
             l2=level(2.8, 0.12, 1.0, [55, 45], 90, [0.4, 0.25], 0.35, 0.05, 0.15, 0.6, 0.1, 0.08, per_node=2),
             l3=level(3.5, 0.2, 1.0, [45, 35], 90, [0.3, 0.2], 0.3, 0.0, 0.15, 0.65, 0.06, 0.04, per_node=2),
             leaf_kind='compound', leaflets=7, rachis=0.13, leaflet_angle=65, leaf_len=cm(5, 10), leaf_wid=cm(2, 4),
             petiole=0.05, leaf_per_node=2, leaf_phyllo=90, leaf_spacing=0.07, leaf_angle=[45, 65], leaf_flat=0.55,
             leaf_droop=0.12, lai=4.2, spur_share=0.0, bloom_place='axil', bloom_share=0.2, bloom_size=0.04,
             bloom_droop=0.2, fruit_size=0.1),
        dict(outline='lanceolate', gloss=0.2, translucency=0.55, flutter=0.15, fold=0.25, underside=[1.2, 0.05],
             jitter=[0.15, 0.06], bloom_shape='panicle', petals=0, fruit_shape='pod', fruits=10, leaflet_ratio=0.4),
        fruit_winter=True, autumn_order=(0.4, 0.1), graft_levels=2, crown_fill=0.6, follow=0.55, dead=0.04,
        bark_zone=None, bark=[0.7, 0.15, 0.0], bark_scale=2.5, bark_contrast=1.25, lichen=0.25)

# ---- Orchard relics -------------------------------------------------------------------------------------------------
# Apple (feral and relic): a broad, rounded, low, gnarled crown on crooked limbs, spur shoots, water sprouts and dead
# limbs on the abandoned trees; ovate leaves 4-9 cm, downy below; white-pink blossom in early May, apples from August.
species('mal_dom', 'Malus domestica Borkh.', [5, 10], [0.9, 1.3],
        arch(BROADLEAF, href=6.0, env_base=0.2, stem_radius=0.08, stem_up=0.3, stem_wobble=0.18,
             l1=level(2.2, 0.3, 0.95, [60, 45], 137.5, [0.5, 0.25], 0.15, 0.35, 0.3, 0.5, 0.15, 0.15),
             l2=level(4.0, 0.12, 1.0, [55, 45], 137.5, [0.45, 0.25], 0.15, 0.1, 0.3, 0.55, 0.08, 0.05),
             l3=level(5.0, 0.2, 1.0, [50, 40], 137.5, [0.3, 0.2], 0.2, 0.0, 0.25, 0.6, 0.04, 0.03),
             **spray(5, 0.015), leaf_len=cm(4, 9), leaf_wid=cm(3, 5), petiole=0.02, leaf_angle=[45, 65], leaf_flat=0.55,
             lai=4.0, spur_share=0.45, spur_leaves=[4, 6], bloom_place='old_wood', bloom_share=0.45, bloom_size=0.06,
             bloom_droop=0.1, fruit_size=0.06),
        dict(outline='ovate', gloss=0.1, translucency=0.45, flutter=0.08, fold=0.1, underside=[1.35, 0.1],
             jitter=[0.15, 0.06], bloom_shape='corymb', petals=5, fruit_shape='pome', fruits=1),
        autumn_order=(0.4, -0.1), crown_fill=0.75, follow=0.55, dead=0.1, bark_zone=None, bark=[0.7, 0.15, 0.05],
        bark_scale=2.0, bark_contrast=1.2, lichen=0.4)
# Pear and wild pear: a narrow, steep, pyramidal-ovoid crown, branches ascending at acute angles, spur shoots and
# thorny spurs on the wild trees; rounded glossy leathery leaves on long petioles; white blossom in late April, small
# hard pears in September; dark grey bark cracked into small square blocks.
species('pyr_com', 'Pyrus communis L. (incl. P. pyraster)', [10, 20], [0.45, 0.65],
        arch(BROADLEAF, href=9.0, stem_radius=0.09, stem_up=0.6,
             l1=level(2.2, 0.25, 0.95, [40, 30], 137.5, [0.42, 0.18], 0.35, 0.15, 0.15, 0.5, 0.18, 0.15),
             l2=level(3.5, 0.12, 1.0, [45, 35], 137.5, [0.45, 0.25], 0.25, 0.05, 0.18, 0.55, 0.08, 0.05),
             l3=level(5.0, 0.2, 1.0, [45, 35], 137.5, [0.3, 0.2], 0.2, 0.0, 0.2, 0.6, 0.04, 0.03),
             prickle_spacing=0.12, prickle_len=0.02, prickle_base=0.15, prickle_young=1.0,
             **spray(5, 0.015), leaf_len=cm(3, 7), leaf_wid=cm(2.5, 5), petiole=0.03, leaf_angle=[45, 65], leaf_flat=0.6,
             lai=4.0, spur_share=0.45, spur_leaves=[3, 5], bloom_place='old_wood', bloom_share=0.45, bloom_size=0.06,
             fruit_size=0.04),
        dict(outline='round', gloss=0.4, translucency=0.4, flutter=0.15, fold=0.08, underside=[1.15, 0.03],
             jitter=[0.15, 0.06], bloom_shape='corymb', petals=5, fruit_shape='pome', fruits=2),
        autumn_order=(0.4, -0.1), crown_fill=0.7, follow=0.55, dead=0.05, bark_zone=None, bark=[0.8, 0.1, 0.0],
        bark_scale=2.0, bark_contrast=1.3, lichen=0.35)
# Sour cherry (the shrubs' pru_cer grown as a tree): a rounded crown with drooping thin branches; glossy dark green
# elliptic leaves; white blossom in late April, red cherries in late June and July; glossy red-brown bark with
# horizontal lenticels, peeling in rings. (The stock cherry trees reach 16-24 m: wild cherry's size, sour cherry's
# look; the height range runs to the old village cherries' 20 m.)
species('t_pru_cer', 'Prunus cerasus L.', [3, 20], [0.85, 1.15],
        arch(BROADLEAF, href=5.0, stems=[1, 3], stem_spread=[3, 20], stem_radius=0.06, stem_up=0.4,
             l1=level(2.4, 0.25, 0.95, [55, 42], 137.5, [0.45, 0.2], 0.0, 0.3, 0.15, 0.5, 0.15, 0.12),
             l2=level(4.0, 0.12, 1.0, [55, 45], 137.5, [0.48, 0.26], -0.25, 0.1, 0.2, 0.55, 0.07, 0.05),
             l3=level(5.0, 0.2, 1.0, [50, 40], 137.5, [0.35, 0.2], -0.2, 0.0, 0.2, 0.6, 0.04, 0.03),
             **spray(4, 0.025), leaf_len=cm(4, 7), leaf_wid=cm(2.5, 4), petiole=0.012, leaf_angle=[45, 65],
             leaf_flat=0.55, lai=4.0, spur_share=0.35, spur_leaves=[3, 5], bloom_place='old_wood', bloom_share=0.35,
             bloom_size=0.03, bloom_droop=0.3, fruit_size=0.02),
        dict(outline='elliptic', gloss=0.35, translucency=0.45, flutter=0.1, fold=0.12, underside=[1.2, 0.03],
             jitter=[0.15, 0.06], bloom_shape='umbel', petals=5, fruit_shape='drupe', fruits=2),
        photos='pru_cer', autumn_order=(0.4, -0.2), crown_fill=0.7, follow=0.7, dead=0.08, bark_zone=None,
        # (cooled and with its relief brought out: its red-brown read salmon-pink and flat, painted plastic among the
        # grey barks round it)
        bark=[0.15, 0.4, 0.7], bark_scale=2.0, bark_contrast=1.35, bark_tint=[0.8, 0.9, 1.0], lichen=0.25)
# Plum: dense low sparry crowns, root-sucker thickets filling the former gardens, sometimes thorny; dull green
# oblong-elliptic leaves; white blossom in late April, blue-purple or yellow plums in August and September.
species('pru_dom', 'Prunus domestica L.', [4, 12], [0.7, 1.0],
        arch(BROADLEAF, href=6.0, stems=[1, 3], stem_spread=[3, 18], stem_radius=0.06, stem_up=0.45,
             l1=level(2.6, 0.2, 0.95, [45, 35], 137.5, [0.45, 0.2], 0.25, 0.2, 0.12, 0.5, 0.15, 0.12),
             l2=level(4.5, 0.12, 1.0, [50, 40], 137.5, [0.45, 0.25], 0.15, 0.05, 0.15, 0.55, 0.07, 0.05),
             l3=level(6.0, 0.2, 1.0, [45, 35], 137.5, [0.3, 0.2], 0.15, 0.0, 0.15, 0.6, 0.04, 0.03),
             prickle_spacing=0.15, prickle_len=0.012, prickle_base=0.15, prickle_young=0.8,
             **spray(5, 0.015), leaf_len=cm(4, 10), leaf_wid=cm(2.5, 5), petiole=0.015, leaf_angle=[45, 65],
             leaf_flat=0.55, lai=4.5, spur_share=0.4, spur_leaves=[3, 5], bloom_place='old_wood', bloom_share=0.4,
             bloom_size=0.022, fruit_size=0.035),
        dict(outline='elliptic', gloss=0.08, translucency=0.45, flutter=0.08, fold=0.12, underside=[1.25, 0.05],
             jitter=[0.15, 0.06], bloom_shape='single', petals=5, fruit_shape='drupe', fruits=1),
        autumn_order=(0.4, -0.1), crown_fill=0.8, follow=0.55, dead=0.08, bark_zone=(1.0, 3.0), bark=[0.45, 0.2, 0.3],
        bark_scale=2.0, bark_contrast=1.15, lichen=0.35)
# Walnut: a broad, rounded, open crown on thick spreading limbs, stout twigs, few of them; pinnate leaves of 5-9 large
# aromatic leaflets with a bronze-purple flush in May, falling early after the first frosts; green husked nuts
# splitting black; smooth silver-grey bark, dark and deeply fissured on old trunks.
species('jug_reg', 'Juglans regia L.', [15, 25], [0.8, 1.1],
        arch(BROADLEAF, href=10.0, stem_radius=0.11, stem_up=0.35,
             l1=level(1.4, 0.3, 0.95, [60, 45], 137.5, [0.5, 0.25], 0.15, 0.35, 0.15, 0.55, 0.25, 0.25),
             l2=level(2.2, 0.12, 1.0, [55, 45], 137.5, [0.45, 0.25], 0.2, 0.1, 0.15, 0.6, 0.1, 0.08),
             l3=level(2.5, 0.2, 1.0, [45, 35], 137.5, [0.3, 0.2], 0.25, 0.0, 0.15, 0.65, 0.05, 0.04),
             leaf_kind='compound', leaflets=7, rachis=0.2, leaflet_angle=70, leaf_len=cm(6, 15), leaf_wid=cm(3, 7),
             petiole=0.07, leaf_spacing=0.06, leaf_angle=[45, 65], leaf_flat=0.6, leaf_droop=0.12, lai=4.2,
             spur_share=0.0, catkin_share=0.2, catkin_len=0.1, catkin_droop=1.0, bloom_place='tip', bloom_share=0.15,
             bloom_size=0.01, fruit_size=0.045),
        dict(outline='elliptic', gloss=0.08, translucency=0.5, flutter=0.1, fold=0.25, underside=[1.15, 0.03],
             jitter=[0.15, 0.06], bloom_shape='none', fruit_shape='nut', fruits=2, catkin_shape='hanging',
             leaflet_ratio=0.5),
        young_copper=0.55, autumn_order=(0.4, 0.0), graft_levels=2, crown_fill=0.65, follow=0.7, dead=0.03,
        bark_zone=(2.0, 6.0), bark=[0.7, 0.2, 0.05], bark_scale=3.0, bark_contrast=1.25, lichen=0.2)


def mean(v):
    return 0.5 * (v[0] + v[1])


# (No max_leaves for the trees: the generator sizes a tree's leaf elements by its own crown - the species' LAI over
# the crown field's projection, on the share crown_fill fills - within the level's budget, da_shrub_gen.cpp
# generate_on_skeleton.)


# ======================================================================================================
# The leaf elements: the texture layers' own (tools/shrubs/textures/tree_leaf_botany.py)
# ======================================================================================================
# The leaf, needle and spray-card layers are drawn at the sizes tree_leaf_botany.py gives them, and their squares
# (leaf_fit, card_fit) are fitted to those elements: the generator's elements need the same sizes for the fits to hold
# (a compound leaf's or a tuft's other proportions stretch the drawing). The botany's sizes are the drawn ones and lie
# inside the species' ranges of the floras, so every species takes them: its leaf (length, width, petiole), a compound
# leaf's leaflets, rachis and leaflet angle (its leaflets' width over their length), a palmate leaf's fan, a conifer's
# tuft (the needles' length, the stretch of shoot), a simple leaf's spray - the leaves on it, their spacing, pairs,
# phyllotaxis and angle, as its card shows them (the big-leaved maples and the red oak too: they grow in sprays like
# the rest). The branching and a compound species' leaf spacing stay its own (where its leaves stand on the shoots).
SPRAY_KEYS = ('leaflets', 'leaf_spacing', 'leaf_per_node', 'leaf_phyllo', 'leaf_angle', 'spray_len')
# (The palmate fan: the generator's compound element is 2 * leaflet * sin(leaflet_angle) wide; the botany's is 1.15
# leaflets - make_leaf_textures.py PALMATE - with no rachis.)
PALMATE_ANGLE = round(math.degrees(math.asin(1.15 / 2.0)), 1)
BOTANY = {}


def _module(path, name):
    """A tool script of the repository as a module (None where it does not exist)."""
    import importlib.util
    if not os.path.exists(path):
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def align_to_botany():
    """Every species' leaf elements to the texture botany's; BOTANY: the keys that changed per species."""
    tb = _module(os.path.join(TEX_DIR, 'tree_leaf_botany.py'), 'tree_leaf_botany')
    if tb is None:
        return
    shrubs = None
    for sid, s in SP.items():
        src = tb.SHARED.get(sid, sid)
        if src in tb.TREES:
            b, bd = tb.TREES[src]['arch'], tb.TREES[src].get('draw', {})
        else:
            # (A tree wearing a shrub's layers - the sour cherry - has the shrub's elements.)
            if shrubs is None:
                shrubs = _module(os.path.join(REPO, 'tools', 'shrubs', 'make_shrubs.py'), 'make_shrubs')
            if shrubs is None or src not in shrubs.SP:
                continue
            b, bd = shrubs.SP[src]['arch'], shrubs.SP[src].get('draw', {})
        a, kind = s['arch'], b.get('leaf_kind', 'simple')
        new = {}
        if kind == 'needles':
            new.update(needle_len=b['needle_len'], leaf_spacing=b['leaf_spacing'])
        else:
            new.update(leaf_len=list(b['leaf_len']), leaf_wid=list(b['leaf_wid']), petiole=b.get('petiole', 0.01))
            if kind == 'compound':
                new.update(leaflets=b['leaflets'], rachis=b['rachis'], leaflet_angle=b['leaflet_angle'])
            elif kind == 'palmate':
                new.update(leaflets=b['leaflets'], rachis=0.0, leaflet_angle=PALMATE_ANGLE)
            elif b.get('spray_len'):
                # (A simple leaf with a spray card grows in sprays: the generator's spray element is the card's own
                # stretch of shoot - a single leaf's far card would be the generator's eight-leaf stretch instead,
                # tree_cards, several times the drawn card's length.)
                new.update({k: (list(b[k]) if isinstance(b[k], (list, tuple)) else b[k]) for k in SPRAY_KEYS if k in b})
                new['leaf_kind'] = 'spray'
        diff = {k: (a.get(k), v) for k, v in new.items() if a.get(k) != v}
        a.update(new)
        if kind in ('compound', 'palmate') and 'leaflet_ratio' in bd:
            if s['draw'].get('leaflet_ratio') != bd['leaflet_ratio']:
                diff['leaflet_ratio'] = (s['draw'].get('leaflet_ratio'), bd['leaflet_ratio'])
            s['draw']['leaflet_ratio'] = bd['leaflet_ratio']
        BOTANY[sid] = diff


# ======================================================================================================
# Phenology (day of year) for the Kyiv Polesie, SPECIES.md section 4: the reserve's nature calendars [L19, L22],
# the PSRER seasons [Y25], iNaturalist p50 dates of the Polesie box (tree_photo_colours.json phenology_inat).
# bud burst, leaf unfold, leaf full; flowers (or catkins) start and end; fruit ripe; autumn start and peak; leaf fall
# start and end. Evergreens: the inner needles' yellowing and fall (autumn start, peak; fall start, end).
# ======================================================================================================
PHENO = {
    # pollen mid-late May [L19 13-15 May 2019, L22 27 May 2022]; cones ripe in October of their 2nd year; the 3rd-4th
    # year inner needles yellow in September and fall by the end of October
    'pin_syl': dict(flower_start=133, flower_end=150, fruit_ripe=285, autumn_start=250, autumn_peak=272,
                    leaf_fall_start=268, leaf_fall_end=300, new_needles=172),
    # red male strobili in May, new shoots bright light green in late May-June, cones ripe brown in October
    'pic_abi': dict(flower_start=128, flower_end=146, fruit_ripe=285, autumn_start=255, autumn_peak=280,
                    leaf_fall_start=272, leaf_fall_end=305, new_needles=165),
    # bud burst mid April [L19 16 Apr, Y25 ~18 Apr], catkins ~19 Apr (iNat); yellow strands ~11 Sep, golden ~6 Oct
    # [Y25], half fallen ~10 Oct [L19], bare by ~30 Oct-early Nov
    'bet_pen': dict(bud_burst=104, leaf_unfold=110, leaf_full=135, flower_start=104, flower_end=118, fruit_ripe=235,
                    autumn_start=252, autumn_peak=279, leaf_fall_start=276, leaf_fall_end=305),
    'bet_pub': dict(bud_burst=112, leaf_unfold=118, leaf_full=140, flower_start=113, flower_end=127, fruit_ripe=240,
                    autumn_start=262, autumn_peak=288, leaf_fall_start=283, leaf_fall_end=310),
    # catkins late March (iNat ~5 Apr), leaves from late April, seed cotton flying [L19 23 May]; peak early Oct
    # [L19 2 Oct: almost fully red-yellow]
    'pop_tre': dict(bud_burst=113, leaf_unfold=120, leaf_full=142, flower_start=85, flower_end=105, fruit_ripe=140,
                    autumn_start=262, autumn_peak=281, leaf_fall_start=280, leaf_fall_end=303),
    # leaf-out early-mid May [L19 2 May], catkins ~10 May (iNat); acorns ripe in September; colour late October; all
    # down by mid November [L19 14 Nov]
    'que_rob': dict(bud_burst=120, leaf_unfold=127, leaf_full=150, flower_start=126, flower_end=140, fruit_ripe=262,
                    autumn_start=285, autumn_peak=300, leaf_fall_start=300, leaf_fall_end=320),
    'car_bet': dict(bud_burst=112, leaf_unfold=118, leaf_full=140, flower_start=110, flower_end=124, fruit_ripe=275,
                    autumn_start=280, autumn_peak=297, leaf_fall_start=298, leaf_fall_end=318),
    # flowers before the leaves mid-late April (iNat 19 Apr); peak yellow-orange mid-late October
    'ace_pla': dict(bud_burst=110, leaf_unfold=115, leaf_full=138, flower_start=103, flower_end=118, fruit_ripe=260,
                    autumn_start=268, autumn_peak=290, leaf_fall_start=290, leaf_fall_end=310),
    # flowering late June [L19 12-20 Jun, Y25 ~23 Jun, iNat 21 Jun]
    'til_cor': dict(bud_burst=113, leaf_unfold=120, leaf_full=142, flower_start=168, flower_end=185, fruit_ripe=240,
                    autumn_start=265, autumn_peak=285, leaf_fall_start=283, leaf_fall_end=303),
    # flowers before the leaves, leaves unfold mid May (late), fall green or pale yellow after the first frosts
    'fra_exc': dict(bud_burst=128, leaf_unfold=135, leaf_full=158, flower_start=105, flower_end=120, fruit_ripe=265,
                    autumn_start=282, autumn_peak=295, leaf_fall_start=290, leaf_fall_end=310),
    # flowers late March, samaras ripe and falling mid-late May (iNat 17 May)
    'ulm_lae': dict(bud_burst=110, leaf_unfold=117, leaf_full=140, flower_start=82, flower_end=100, fruit_ripe=135,
                    autumn_start=268, autumn_peak=289, leaf_fall_start=288, leaf_fall_end=310),
    'ulm_min': dict(bud_burst=112, leaf_unfold=120, leaf_full=142, flower_start=88, flower_end=105, fruit_ripe=128,
                    autumn_start=275, autumn_peak=298, leaf_fall_start=295, leaf_fall_end=318),
    # white corymbs mid May [Y25 ~19 May, iNat 17 May], berries orange in July, red in August
    'sor_auc': dict(bud_burst=105, leaf_unfold=112, leaf_full=135, flower_start=135, flower_end=150, fruit_ripe=225,
                    autumn_start=250, autumn_peak=272, leaf_fall_start=275, leaf_fall_end=298),
    # catkins burst 27 Feb, pollen 8 Mar [L19], iNat 22 Mar; leaves mid-late April; leaf fall in progress 2 Oct
    'aln_glu': dict(bud_burst=108, leaf_unfold=115, leaf_full=140, flower_start=65, flower_end=85, fruit_ripe=270,
                    autumn_start=278, autumn_peak=292, leaf_fall_start=280, leaf_fall_end=310),
    'sal_alb': dict(bud_burst=105, leaf_unfold=112, leaf_full=135, flower_start=108, flower_end=122, fruit_ripe=145,
                    autumn_start=285, autumn_peak=305, leaf_fall_start=300, leaf_fall_end=325),
    # leaf appearance 18 Apr [L19]
    'sal_fra': dict(bud_burst=104, leaf_unfold=110, leaf_full=133, flower_start=105, flower_end=118, fruit_ripe=150,
                    autumn_start=283, autumn_peak=303, leaf_fall_start=298, leaf_fall_end=322),
    'pop_nig': dict(bud_burst=113, leaf_unfold=120, leaf_full=142, flower_start=88, flower_end=105, fruit_ripe=148,
                    autumn_start=280, autumn_peak=298, leaf_fall_start=296, leaf_fall_end=318),
    'pop_alb': dict(bud_burst=113, leaf_unfold=120, leaf_full=142, flower_start=85, flower_end=102, fruit_ripe=140,
                    autumn_start=282, autumn_peak=300, leaf_fall_start=298, leaf_fall_end=320),
    'pop_can': dict(bud_burst=120, leaf_unfold=128, leaf_full=150, flower_start=95, flower_end=110,
                    autumn_start=285, autumn_peak=302, leaf_fall_start=300, leaf_fall_end=322),
    # leaves mid May; racemes [L19 21 May 2019, L22 8 Jun 2022, iNat 31 May]; often green at the first frost
    'rob_pse': dict(bud_burst=130, leaf_unfold=137, leaf_full=160, flower_start=146, flower_end=163, fruit_ripe=240,
                    autumn_start=285, autumn_peak=300, leaf_fall_start=295, leaf_fall_end=315),
    # tassels before the leaves mid April (iNat 12 Apr)
    'ace_neg': dict(bud_burst=103, leaf_unfold=110, leaf_full=135, flower_start=98, flower_end=112, fruit_ripe=258,
                    autumn_start=270, autumn_peak=290, leaf_fall_start=288, leaf_fall_end=308),
    # racemes in May (iNat 17 May)
    'ace_pse': dict(bud_burst=112, leaf_unfold=118, leaf_full=140, flower_start=128, flower_end=145, fruit_ripe=270,
                    autumn_start=275, autumn_peak=293, leaf_fall_start=292, leaf_fall_end=312),
    # candles early-mid May (iNat 17 May); the leaf miner browns the crowns from July
    'aes_hip': dict(bud_burst=102, leaf_unfold=108, leaf_full=130, flower_start=125, flower_end=142, fruit_ripe=255,
                    autumn_start=200, autumn_peak=290, leaf_fall_start=280, leaf_fall_end=305),
    'que_rub': dict(bud_burst=125, leaf_unfold=132, leaf_full=155, flower_start=120, flower_end=135, fruit_ripe=275,
                    autumn_start=278, autumn_peak=298, leaf_fall_start=300, leaf_fall_end=322),
    'fra_pen': dict(bud_burst=115, leaf_unfold=122, leaf_full=145, flower_start=100, flower_end=115, fruit_ripe=255,
                    autumn_start=265, autumn_peak=283, leaf_fall_start=283, leaf_fall_end=303),
    # blossom [L19 2 May 2019, iNat 3 May], apples ripe from late August [L22 31 Aug 2022]
    'mal_dom': dict(bud_burst=108, leaf_unfold=115, leaf_full=138, flower_start=118, flower_end=133, fruit_ripe=238,
                    autumn_start=280, autumn_peak=298, leaf_fall_start=296, leaf_fall_end=318),
    # blossom ended 2 May 2019 [L19], iNat 26 Apr
    'pyr_com': dict(bud_burst=105, leaf_unfold=112, leaf_full=135, flower_start=108, flower_end=122, fruit_ripe=250,
                    autumn_start=272, autumn_peak=290, leaf_fall_start=290, leaf_fall_end=312),
    # blossom 23 Apr 2019 [L19], cherries red late June, ripe in July (the shrubs' pru_cer)
    't_pru_cer': dict(bud_burst=102, leaf_unfold=110, leaf_full=132, flower_start=110, flower_end=125, fruit_ripe=182,
                      autumn_start=275, autumn_peak=295, leaf_fall_start=292, leaf_fall_end=310),
    'pru_dom': dict(bud_burst=103, leaf_unfold=110, leaf_full=133, flower_start=108, flower_end=122, fruit_ripe=235,
                    autumn_start=275, autumn_peak=295, leaf_fall_start=293, leaf_fall_end=313),
    # bronze-purple leaves early-mid May, catkins (iNat 17 May), husks split black in September-October
    'jug_reg': dict(bud_burst=122, leaf_unfold=130, leaf_full=152, flower_start=128, flower_end=142, fruit_ripe=260,
                    autumn_start=272, autumn_peak=288, leaf_fall_start=285, leaf_fall_end=305),
}

# ======================================================================================================
# Colours from the photos (the shrubs' calibration, make_shrubs.py, on the trees' monthly medians)
# ======================================================================================================


def hex_rgb(h):
    h = h.lstrip('#')
    return [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]


def rgb_hex(c):
    return '#%02X%02X%02X' % tuple(max(0, min(255, int(round(x * 255.0)))) for x in c)


def to_lin(c):
    return [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]


def to_srgb(c):
    return [x * 12.92 if x <= 0.0031308 else 1.055 * max(x, 0.0) ** (1 / 2.4) - 0.055 for x in c]


def lum(lin):
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def scale_lum(h, y_min=None, y_max=None):
    lin = to_lin(hex_rgb(h))
    y = lum(lin)
    k = 1.0
    if y_min is not None and y < y_min:
        k = y_min / max(y, 1e-4)
    if y_max is not None and y > y_max:
        k = y_max / max(y, 1e-4)
    return rgb_hex(to_srgb([min(x * k, 1.0) for x in lin]))


def mix(a, b, t):
    la, lb = to_lin(hex_rgb(a)), to_lin(hex_rgb(b))
    return rgb_hex(to_srgb([x + (y - x) * t for x, y in zip(la, lb)]))


def photo_y(h):
    return lum(to_lin(hex_rgb(h)))


def flower_floor(h):
    """The luminance a petal of the photo's colour class reflects at least (GRASS.md, the forbs' rule)."""
    r, g, b = hex_rgb(h)
    hh, ll, ss = colorsys.rgb_to_hls(r, g, b)
    if ss < 0.18 or ll > 0.78:
        return scale_lum(h, 0.55)
    deg = hh * 360.0
    if 38 <= deg < 75:
        return scale_lum(h, 0.42)
    if 200 <= deg < 300:
        return scale_lum(h, 0.14)
    if deg >= 300 or deg < 12:
        return scale_lum(h, 0.22 if ll > 0.55 else 0.09)
    return scale_lum(h, 0.09)


def src_of(sid):
    return SP[sid]['extra'].get('photos', sid)


def photo(sid, month, role, min_n=2):
    m = PHOTOS.get(src_of(sid), {}).get('months', {}).get(month, {})
    r = m.get(role)
    return r['hex'] if r and r.get('n', 0) >= min_n else None


# The shrubs' levels (make_shrubs.py REF_Y, SCALE_Y): a leaf of the game's crowns a fifth under the meadow's live
# colour of July, the young flush lighter; the brighter subjects scaled by the exposure their kind was given.
REF_Y = {'summer': 0.072, 'young': 0.1}
SCALE_Y = {'autumn': (0.6, 0.04, 0.26), 'dead': (0.7, 0.05, 0.18), 'fruit': (0.7, 0.012, 0.3), 'unripe': (0.4, 0.03, 0.12),
           'twig': (0.75, 0.04, 0.12)}
_MEDIANS = {}


def median_y(role, month):
    """The median luminance of a colour role in a month over the tree species (3 photos or more)."""
    key = (role, month)
    if key not in _MEDIANS:
        ys = sorted(photo_y(p['months'][month][role]['hex']) for p in PHOTOS.values()
                    if role in p['months'].get(month, {}) and p['months'][month][role].get('n', 0) >= 3)
        _MEDIANS[key] = ys[len(ys) // 2] if ys else 0.2
    return _MEDIANS[key]


def to_level(h, ref, med):
    y = max(photo_y(h), 1e-4)
    t = ref * (y / med) ** 0.5
    return scale_lum(h, t, t)


def scaled(h, role):
    k, lo, hi = SCALE_Y[role]
    t = min(max(photo_y(h) * k, lo), hi)
    return scale_lum(h, t, t)


# A green leaf's blue and red against its green (make_shrubs.py LEAF_TINT): the photos' warm light leaves too little
# blue; the fresh and young leaves yellower.
LEAF_TINT = {'summer': (0.27, 0.42, 0.48, 0.80), 'fresh': (0.25, 0.40, 0.50, 0.84), 'young': (0.23, 0.38, 0.52, 0.86)}
# May's and June's foliage is lighter and yellower than July's (luminance factor, tint).
FRESH = {'may': (1.3, 'fresh'), 'jun': (1.12, 'summer')}


def leaf_tint(h, kind):
    b_floor, b_cap, r_lo, r_hi = LEAF_TINT[kind]
    r, g, b = to_lin(hex_rgb(h))
    y = lum([r, g, b])
    g = max(g, 1e-4)
    bg = min(b_floor + 0.5 * max(b / g - 0.12, 0.0), b_cap)
    rg = min(max(r / g, r_lo), r_hi)
    c = [rg * g, g, bg * g]
    k = y / max(lum(c), 1e-6)
    return rgb_hex(to_srgb([min(x * k, 1.0) for x in c]))


# Glaucous and silvery foliage (its share of a blue-grey green at the leaf's own luminance): the pine's stomatal lines,
# white willow's silky leaves, white poplar's felt.
GLAUCOUS = {'pin_syl': 0.25, 'sal_alb': 0.25, 'pop_alb': 0.15}
GLAUCOUS_HEX = '#7A8A84'
# Light and dark greens of the species cards (the leaf's luminance factor): the palest birch flush and the light
# robinia, box elder and ash against the darkest broadleaf green of alder and the near-black spruce spires.
LIGHTER = {'bet_pen': 1.1, 'aln_glu': 0.85, 'que_rob': 0.92, 'fra_exc': 1.1, 'rob_pse': 1.15, 'ace_neg': 1.15,
           'sal_alb': 1.1, 'pic_abi': 0.8, 'pin_syl': 0.95, 'aes_hip': 0.9, 'ace_pse': 0.9, 'til_cor': 0.95,
           'pop_nig': 0.95, 'bet_pub': 0.95, 'mal_dom': 0.95, 'pyr_com': 0.95, 'pru_dom': 0.95}
# Undersides the upper sides' photos do not tell (tint, linear luminance): white poplar's white felt, white willow's
# silky silver, lime's bluish white.
UNDER = {'pop_alb': ('#87917F', 0.269), 'sal_alb': ('#B8C0B4', 0.2), 'til_cor': ('#A8B4AC', 0.12)}
# Twigs of the winter crown where the photos have none (the species cards).
TWIG = {'pin_syl': '#8C735B', 'pic_abi': '#795F3A', 'bet_pen': '#5E4A42', 'bet_pub': '#4E4744', 'pop_tre': '#736C5E',
        'que_rob': '#735E4C', 'car_bet': '#756450', 'ace_pla': '#7D5D4A', 'til_cor': '#7A4A3A', 'fra_exc': '#7A7A6A',
        'ulm_lae': '#6F6053', 'ulm_min': '#594D43', 'sor_auc': '#59493F', 'aln_glu': '#5E4E48', 'sal_alb': '#8A7A4A',
        'sal_fra': '#7A6A42', 'pop_nig': '#655747', 'pop_alb': '#8A8A80', 'pop_can': '#5A4A40', 'rob_pse': '#7B6E50',
        'ace_neg': '#5E6A50', 'ace_pse': '#7A6A50', 'aes_hip': '#6A4A38', 'que_rub': '#806848', 'fra_pen': '#796A57',
        'mal_dom': '#645043', 'pyr_com': '#5D4C39', 't_pru_cer': '#6E3E32', 'pru_dom': '#583E35', 'jug_reg': '#846C59'}
# Petals and centres where the photo colour misleads the petal floor (the dark flowers of ash and elm, the conifers'
# strobili, the catkin-only species have none).
PETAL = {'fra_exc': '#2E2226', 'fra_pen': '#4A2A2A', 'ulm_lae': '#6A3030', 'ulm_min': '#6A3238', 'pin_syl': '#C8B860',
         'pic_abi': '#A84A40', 'ace_pla': '#A8B848', 'ace_pse': '#98B040', 'ace_neg': '#A89870', 'til_cor': '#D8D08A',
         'aes_hip': '#ECE8E0', 'rob_pse': '#ECECE4', 'mal_dom': '#ECDCDC', 'sor_auc': '#ECECE0'}
CENTRE = {'mal_dom': '#D8B040', 'pyr_com': '#8A3040', 't_pru_cer': '#D8B040', 'pru_dom': '#D8B040', 'aes_hip': '#D8A040',
          'rob_pse': '#E0D060', 'sor_auc': '#D0C080', 'til_cor': '#C8B050', 'pin_syl': '#C8A040', 'pic_abi': '#C8A040'}
# Catkins whose photos are weak or missing (the poplars' red male catkins, alder's purple-brown winter catkins).
CATKIN = {'pop_nig': '#7A2E2A', 'pop_can': '#7A2E2E', 'aln_glu': '#7A5A48', 'pop_alb': '#B3AE83', 'jug_reg': '#7A7A50'}
# Fruit: unripe colours by the cards, ripe ones where the photos have none.
UNRIPE = {'pin_syl': '#6E7A4A', 'pic_abi': '#7A5A3A', 'bet_pen': '#6A7A40', 'bet_pub': '#6A7A40', 'aln_glu': '#5E6E3A',
          'que_rob': '#7A8A40', 'que_rub': '#6A7A40', 'sor_auc': '#8A9A40', 'mal_dom': '#8A9A40', 'pyr_com': '#7A8A40',
          't_pru_cer': '#8A9A4A', 'pru_dom': '#7A8A40', 'jug_reg': '#5E7A3A', 'aes_hip': '#6E8A3A', 'ace_pla': '#8A9A50',
          'ace_pse': '#8A9A50', 'ace_neg': '#8A9A50', 'fra_exc': '#7A8A4A', 'fra_pen': '#7A8A4A', 'rob_pse': '#6A7A40',
          'til_cor': '#9AA06A', 'car_bet': '#8A9A50', 'ulm_lae': '#9AAA6A', 'ulm_min': '#9AAA6A', 'pop_tre': '#8A9A60',
          'pop_nig': '#8A9A60', 'pop_alb': '#8A9A60', 'sal_alb': '#8A9A60', 'sal_fra': '#8A9A60'}
RIPE = {'pin_syl': '#7E6E50', 'pic_abi': '#877059', 'aln_glu': '#322520', 't_pru_cer': '#8A1E1E', 'jug_reg': '#2A2A22',
        'ulm_min': '#A0966C', 'pop_tre': '#E0E0D6', 'pop_nig': '#E0E0D6', 'pop_alb': '#E0E0D6', 'sal_alb': '#E0E0D6',
        'sal_fra': '#E0E0D6'}
# Fruits given as the game's final colours (unscaled): the seed cotton's white.
FRUIT_FINAL = {sid: {'c_fruit_ripe': '#E0E2DA'} for sid in ('pop_tre', 'pop_nig', 'pop_alb', 'sal_alb', 'sal_fra')}
# Dead foliage: the needles of a dead conifer (the Red Forest's rust-red, a spruce's red-brown) and the oaks' and the
# hornbeam's dry winter leaves come from the photos.
DEAD_NEEDLES = {'pin_syl': '#8E4E2C', 'pic_abi': '#7A4E36'}


# The needles as the look-dev's Commons packs show them (scratchpad lookdev packstats.py: each crown's foliage pixels,
# the median against the frame's white - a cloud or overcast sky divides the channels; WP2 phase 3): the frames' linear
# luminance relative to the birch pack's summer crowns (0.172 of the white) - the pine 0.61, the spruce 0.26 - taken to
# the power 0.85 over the birch's game level (0.075); the pine slightly blue-green (its June frame under a blue sky
# r/g 0.78, b/g 0.53; the late-summer frames' warm light gave the cloud-white ones 1.0 and 0.3), the spruce dark green
# (0.71, 0.24). Winter: the pine's a touch yellower-olive and a seventh darker (its winter frames r/g 0.93-1.0), the
# spruce's as dark as in summer. (Y linear, r/g, b/g; the underside a twelfth lighter and a little bluer.)
PACK_NEEDLES = {'pin_syl': dict(summer=(0.049, 0.70, 0.42), winter=(0.042, 0.82, 0.34)),
                'pic_abi': dict(summer=(0.024, 0.65, 0.30), winter=(0.023, 0.68, 0.29))}
# The October gold of the birches and the aspen from the packs' October frames (the saturated half of the yellow
# leaves against a cloud white: birch r/g 1.54-1.58, b/g 0.20-0.22; the aspens of Ohed 1.6-2.1, 0.02-0.05), at the
# autumn level (Y linear): A the gold, B the orange; bright until late October, browning only as the last leaves fall
# (the curve's last key, half the dead colour, three days past the leaf fall's end).
PACK_AUTUMN = {'bet_pen': dict(a=(0.20, 1.50, 0.12), b=(0.16, 1.90, 0.06), b_share=0.25),
               'bet_pub': dict(a=(0.20, 1.50, 0.12), b=(0.16, 1.90, 0.06), b_share=0.25),
               'pop_tre': dict(a=(0.19, 1.65, 0.08), b=(0.13, 2.40, 0.05), b_share=0.35)}


def chroma_hex(y, rg, bg):
    """A colour of linear luminance y with r/g and b/g as given."""
    g = y / (0.2126 * rg + 0.7152 + 0.0722 * bg)
    return rgb_hex(to_srgb([min(rg * g, 1.0), min(g, 1.0), min(bg * g, 1.0)]))


def bark_colour(sid):
    """The far tiers' bark: the bark texture's own mean in the species' tint where the layers have one (the far tiers
    draw the bark the near tubes show), else the trunks and branches of the whole-tree photos at a wood's albedo."""
    tex = TEXTURES[sid]
    if tex.get('bark_mean'):
        return [(15.5, rgb_hex(to_srgb([x * k for x, k in zip(to_lin(hex_rgb(tex['bark_mean'])), tex['tint'])])))]
    wf = (PHOTOS[src_of(sid)].get('wood_far') or {}).get('hex') or '#5E5046'
    return [(15.5, scale_lum(wf, 0.04, 0.13))]


def leafy_months(sid):
    """The months whose leaf photos give the summer curve: every month for an evergreen, else the months between the
    leaf unfolding and the leaf fall's end."""
    if SP[sid]['extra'].get('evergreen'):
        return MONTHS
    ph = PHENO[sid]
    return [m for m in MONTHS if ph['leaf_unfold'] + 10 <= MONTH_DAY[m] <= ph['leaf_fall_end'] - 5]


def colour_keys(sid):
    ex = SP[sid]['extra']
    ev = ex.get('evergreen')
    ph = PHENO[sid]
    keys = {}
    summer = []
    for m in leafy_months(sid):
        role = 'needle' if ev and photo(sid, m, 'needle', 3) else 'leaf'
        h = photo(sid, m, role, 3)
        if not h:
            continue
        k, kind = FRESH.get(m, (1.0, 'summer'))
        c = to_level(h, REF_Y['summer'] * k, median_y(role, m))
        # (The autumn months and an evergreen's winter keep the photos' chroma: the yellowing and the bronzing.)
        tint = m in ('may', 'jun', 'jul', 'aug', 'sep')
        summer.append((MONTH_DAY[m], leaf_tint(c, kind) if tint else c))
    if not summer:
        summer = [(196.5, leaf_tint(scale_lum('#5E7E3A', REF_Y['summer'], REF_Y['summer']), 'summer'))]
    glauc = GLAUCOUS.get(sid, 0.0)
    if glauc > 0.0:
        summer = [(d, scale_lum(mix(h, GLAUCOUS_HEX, glauc), photo_y(h), photo_y(h))) for d, h in summer]
    light = LIGHTER.get(sid, 1.0)
    summer = [(d, scale_lum(h, photo_y(h) * light, photo_y(h) * light)) for d, h in summer]
    pack = PACK_NEEDLES.get(SP[sid].get('base', sid))
    if pack:
        # (Winter December to February, summer April to October, half way in March and November.)
        su, wi = chroma_hex(*pack['summer']), chroma_hex(*pack['winter'])
        summer = [(15.5, wi), (45.0, wi), (74.5, mix(wi, su, 0.5)), (105.0, su), (196.5, su), (288.5, su),
                  (319.0, mix(su, wi, 0.5)), (349.5, wi)]
    keys['c_summer'] = summer
    # The young flush: the month of the leaf unfolding (an evergreen's new needles in June).
    um = 'jun' if ev else min(MONTHS, key=lambda m: abs(MONTH_DAY[m] - (ph['leaf_unfold'] + 12)))
    young = None
    for m in (um, MONTHS[(MONTHS.index(um) + 1) % 12]):
        h = photo(sid, m, 'young', 2)
        if h:
            young = to_level(h, REF_Y['young'], median_y('young', m))
            break
    if not young:
        young = scale_lum(mix(summer[0][1], '#B8D070', 0.4), REF_Y['young'], REF_Y['young'])
    young = leaf_tint(young, 'young')
    if ex.get('young_copper'):
        young = mix(young, scale_lum('#8A5A36', REF_Y['young'], REF_Y['young']), ex['young_copper'])
    if glauc > 0.0:
        young = scale_lum(mix(young, GLAUCOUS_HEX, 0.6 * glauc), photo_y(young), photo_y(young))
    young = scale_lum(young, photo_y(young) * light, photo_y(young) * light)
    keys['c_young'] = [(ph.get('new_needles', ph.get('leaf_unfold', 120)), young)]
    # Autumn: the photos' palette, A the kind found in most autumn close-ups, B the next.
    ap = PHOTOS[src_of(sid)].get('autumn') or {}
    counts = ap.get('counts') or {}
    a_hex = ap.get('A_hex') or '#A89838'
    b_hex = ap.get('B_hex') or mix(a_hex, '#8A3A20', 0.5)
    na, nb = counts.get(ap.get('A'), 0), counts.get(ap.get('B'), 0)
    b_share = nb / float(na + nb) if na + nb else 0.25
    if ev:
        # (The inner needles yellow before they fall.)
        a_hex, b_hex, b_share = '#A89848', '#8A6A3A', 0.3
    if sid == 'aes_hip':
        # The leaf miner's rusty brown first (July-September), the yellow of October last.
        a_hex = photo(sid, 'sep', 'autumn', 3) or '#89725A'
        b_hex = photo(sid, 'nov', 'autumn', 3) or '#CBAF3F'
        b_share = 0.45
    a_hex, b_hex = scaled(a_hex, 'autumn'), scaled(b_hex, 'autumn')
    green = ex.get('autumn_green', 0.0)
    if green > 0.0:
        # Falling green or pale yellow-green (alder, ash, robinia): the late summer leaf mixed in.
        late = summer[-1][1]
        a_hex = scale_lum(mix(a_hex, late, green), photo_y(a_hex), photo_y(a_hex))
        b_hex = scale_lum(mix(b_hex, late, green * 0.6), photo_y(b_hex), photo_y(b_hex))
    keys['c_autumn_a'] = [(ph['autumn_peak'], a_hex)]
    keys['c_autumn_b'] = [(ph['autumn_peak'], b_hex)]
    keys['b_share'] = round(min(max(b_share, 0.05), 0.6), 3)
    gold = PACK_AUTUMN.get(SP[sid].get('base', sid))
    if gold:
        a_hex, b_hex = chroma_hex(*gold['a']), chroma_hex(*gold['b'])
        keys['b_share'] = gold['b_share']
        keys['_late'] = (ph['leaf_fall_end'] - 8, ph['leaf_fall_end'] + 3)
        keys['c_autumn_a'] = [(ph['autumn_peak'], a_hex), (ph['leaf_fall_end'] - 8, a_hex)]
        keys['c_autumn_b'] = [(ph['autumn_peak'], b_hex), (ph['leaf_fall_end'] - 8, b_hex)]
    # The dead leaf: a marcescent species' dry winter leaves (the photos of December-March), else the tan and dead
    # kinds of November-December; a conifer's dead needles.
    dead = None
    if ev:
        dead = scaled(DEAD_NEEDLES.get(sid, '#7A5A3A'), 'dead')
    else:
        for m in ('jan', 'feb', 'dec', 'nov'):
            r = PHOTOS[src_of(sid)]['months'][m].get('autumn')
            if r and r.get('n', 0) >= 3 and r.get('label') == 'brown':
                dead = scaled(r['hex'], 'dead')
                break
        if not dead:
            for m in ('nov', 'dec'):
                kinds = PHOTOS[src_of(sid)]['months'][m].get('kinds', {})
                for k in ('tan', 'dead'):
                    if kinds.get(k, {}).get('n', 0) >= 2:
                        dead = scaled(kinds[k]['hex'], 'dead')
                        break
                if dead:
                    break
        dead = dead or scaled('#6E5A40', 'dead')
    keys['c_dead'] = [(ph['leaf_fall_end'], dead)]
    if keys.get('_late'):
        # (The gold browns only as the last leaves fall.)
        late = keys.pop('_late')[1]
        keys['c_autumn_a'].append((late, mix(keys['c_autumn_a'][0][1], dead, 0.55)))
        keys['c_autumn_b'].append((late, mix(keys['c_autumn_b'][0][1], dead, 0.55)))
    # The underside: the summer leaf greyer by the glaucous index, about the upper side's level (it carries its own
    # shade); the felted and silky ones by their cards.
    gi = PHOTOS[src_of(sid)].get('glaucous_index') or 0.25
    top = summer[min(2, len(summer) - 1)][1]
    under_y = photo_y(top) * (0.95 + 0.3 * min(gi, 0.5))
    keys['c_under'] = [(196.5, scale_lum(mix(top, '#B4BCAA', min(0.05 + 0.35 * gi, 0.35)), under_y, under_y))]
    if pack:
        y, rg, bg = pack['summer']
        keys['c_under'] = [(196.5, chroma_hex(y * 1.08, rg, bg + 0.04))]
    if sid in UNDER:
        tint, y = UNDER[sid]
        keys['c_under'] = [(196.5, scale_lum(tint, y, y))]
    keys['c_bark'] = bark_colour(sid)
    tw = None
    for m in ('jan', 'feb', 'dec', 'mar'):
        tw = photo(sid, m, 'twig', 3)
        if tw:
            break
    keys['c_twig'] = [(15.5, scaled(tw or TWIG.get(sid, '#6E5A48'), 'twig'))]
    # Flowers and catkins: the photos of the flowering month raised to the petal's floor.
    fs = ph.get('flower_start', 150)
    fm = min(MONTHS, key=lambda m: abs(MONTH_DAY[m] - (fs + 7)))
    fl = None
    for m in (fm, MONTHS[(MONTHS.index(fm) + 1) % 12], MONTHS[(MONTHS.index(fm) - 1) % 12]):
        fl = photo(sid, m, 'flower', 2)
        if fl:
            break
    keys['c_petal'] = [(fs, PETAL.get(sid) or (flower_floor(fl) if fl else '#E8E6DA'))]
    keys['c_centre'] = [(fs, CENTRE.get(sid, '#D8B040'))]
    keys['c_catkin'] = [(fs, CATKIN.get(sid) or (flower_floor(fl) if fl else '#C8B860'))]
    rd = ph.get('fruit_ripe') or 230
    rm = min(MONTHS, key=lambda m: abs(MONTH_DAY[m] - rd))
    fr = None
    for m in (rm, MONTHS[(MONTHS.index(rm) + 1) % 12]):
        fr = photo(sid, m, 'fruit', 2)
        if fr:
            break
    ripe = RIPE.get(sid) or fr or '#6E2A2A'
    keys['c_fruit_ripe'] = [(rd, ripe if sid in RIPE else scaled(ripe, 'fruit'))]
    keys['c_fruit_unripe'] = [(rd, scaled(UNRIPE.get(sid, '#7A9A4A'), 'unripe'))]
    for key, value in FRUIT_FINAL.get(sid, {}).items():
        keys[key] = [(rd, value)]
    return keys


# ======================================================================================================
# Texture layers (tools/shrubs/textures: one binding for both populations)
# ======================================================================================================
def _json(path):
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as f:
        return json.load(f)


LEAF_TEX = _json(os.path.join(TEX_DIR, 'leaf_manifest.json'))
LEAF_HULLS = _json(os.path.join(TEX_DIR, 'leaf_hulls.json'))
TEX_STATS = _json(os.path.join(TEX_DIR, 'texture_stats.json'))
BARK_TEX = _json(os.path.join(TEX_DIR, 'bark_manifest.json'))


def _tree_layers():
    """tree_layers.json per species id: {"species": {id: entry}}, {id: entry} or a list of entries with an id."""
    d = _json(TREE_LAYERS)
    if isinstance(d, list):
        return {e.get('id') or e.get('species'): e for e in d if isinstance(e, dict)}
    if isinstance(d.get('species'), dict):
        return d['species']
    if isinstance(d.get('species'), list):
        return {e.get('id') or e.get('species'): e for e in d['species'] if isinstance(e, dict)}
    return {k: v for k, v in d.items() if isinstance(v, dict)}


TREE_TEX = _tree_layers()
# The bark layers' tile scales (tree_layers.json bark_types: a tile is 6 cm times the scale, the engine's bark_scale).
BARK_TYPES = (lambda d: d.get('bark_types') if isinstance(d, dict) and isinstance(d.get('bark_types'), dict) else {})(
    _json(TREE_LAYERS))


def layer_scale(i):
    """The bark layer's tile scale where tree_layers.json gives one, else None."""
    return next((float(v['scale']) for v in BARK_TYPES.values()
                 if isinstance(v, dict) and v.get('index') == i and v.get('scale')), None)


# The bark types' contrast as the shrubs' BARK_CONTRAST (r3/da_shrub_wood.ps: a power of the luminance ratio about the
# tile's mean, the hue kept), by the layer a species wears on the most wood. The texture review measured every tree
# type's tile in the engine's ambient light against photo packs (tools/shrubs/textures/out/review_trees/
# bark_measures.json: the fissure contrast, linear luminance p20 / p80): the fissured and furrowed ones at the photos'
# at 1, the plated and scaly orchard and floodplain barks flatter (0.32-0.41 against 0.10-0.23). Theirs is the power
# that puts the rendered ratio at the middle (geometric) of the photos' range - the pack's median and the research's
# close-ups - on the tile with its cavity (pear 0.40 -> 0.16 of 0.11-0.23, apple 0.38 -> 0.13 of 0.10-0.18, plum
# 0.33 -> 0.125 of 0.12-0.13, horse chestnut 0.29 -> 0.13 of 0.11-0.16); their median value rises 1.1-1.4 times with it,
# toward the photos'. The smooth and young barks have no photo target and stay at 1 (the engine's contrast is the
# species': the alder's young upper bark takes the alder's).
# (The oak's and the pine plates' furrows to their photo packs too, as their tiles are now - the oak's long ridges
# round narrow furrows, the pine's plates in their furrows' net -: oak 0.11 -> 0.06 (past the pack's 0.10: in the
# frames the oak's dark share stayed under the photos'), pine plates 0.26 -> 0.20, alder 0.29 -> 0.21; the black
# poplar's deep furrows (0.11) are past their pack's by themselves. The spruce's scales in its crown's deep shade, where
# their relief does not show, at half the pack's contrast: 1.6.)
TREE_BARK_CONTRAST = {'pear': 3.45, 'apple': 2.71, 'plum': 2.25, 'horse_chestnut': 1.88, 'oak': 1.25, 'pine_plates': 1.2,
                      'alder': 1.25, 'spruce': 1.6}


def upper_bark_keys(sid, upper):
    """The upper bark's own tile scale and contrast (the engine's bark_scale_upper, bark_contrast_upper; the lower
    bark's where these are not given): its layer's (tree_layers.json bark_types), the smooth young barks - a pine's
    mirror bark, the young oak, ash, alder and walnut - at a contrast of 1. None for a borrowed shrub bark."""
    names = bark_names()
    if TEXTURES[sid]['source'] != 'tree' or not 0 <= upper < len(names) or layer_scale(upper) is None:
        return None
    return SP[sid]['extra'].get('bark_scale_upper') or layer_scale(upper), TREE_BARK_CONTRAST.get(names[upper], 1.0)


def bark_contrast(sid):
    """The species' bark_contrast: its main layer's type's (the tree layers), the borrowed shrub bark's own else."""
    t = TEXTURES[sid]
    if t['source'] != 'tree':
        return SP[sid]['extra'].get('bark_contrast', 1.0)
    names = bark_names()
    i = t.get('scale_layer', t['bark'])
    return TREE_BARK_CONTRAST.get(names[i], 1.0) if 0 <= i < len(names) else 1.0
# The nearest shrub layers while the tree layers do not exist: a leaf of the same kind and outline (sprays from the
# species with a spray card, compound leaves from the compound ones, needles from the juniper's sprig), bark of the
# same build (base, upper).
FALLBACK_LEAF = {'pin_syl': 'jun_com', 'pic_abi': 'jun_com', 'bet_pen': 'pru_cer', 'bet_pub': 'pru_cer',
                 'pop_tre': 'sal_cap', 'que_rob': 'cra_mon', 'car_bet': 'pad_avi', 'ace_pla': 'rib_nig',
                 'til_cor': 'ame_spi', 'fra_exc': 'sam_nig', 'ulm_lae': 'pad_avi', 'ulm_min': 'pad_avi',
                 'sor_auc': 'amo_fru', 'aln_glu': 'sal_cap', 'sal_alb': 'sal_tri', 'sal_fra': 'sal_pen',
                 'pop_nig': 'sal_cap', 'pop_alb': 'cra_mon', 'pop_can': 'sal_cap', 'rob_pse': 'amo_fru',
                 'ace_neg': 'sam_nig', 'ace_pse': 'rib_nig', 'aes_hip': 'sam_nig', 'que_rub': 'cra_mon',
                 'fra_pen': 'sam_nig', 'mal_dom': 'pad_avi', 'pyr_com': 'pru_cer', 't_pru_cer': 'pru_cer',
                 'pru_dom': 'pru_cer', 'jug_reg': 'sam_nig'}
FALLBACK_BARK = {'pin_syl': ('hawthorn', 'cherry'), 'pic_abi': ('shreddy', 'cherry'), 'bet_pen': ('hawthorn', 'willow_goat'),
                 'bet_pub': ('willow_grey', 'willow_goat'), 'pop_tre': ('willow_grey', 'willow_goat'),
                 'que_rob': ('hawthorn', 'hazel'), 'car_bet': ('hazel', 'hazel'), 'ace_pla': ('spindle', 'hazel'),
                 'til_cor': ('spindle', 'hazel'), 'fra_exc': ('willow_grey', 'willow_goat'), 'ulm_lae': ('spindle', 'spindle'),
                 'ulm_min': ('spindle', 'spindle'), 'sor_auc': ('bird_cherry', 'bird_cherry'),
                 'aln_glu': ('willow_grey', 'alder_buckthorn'), 'sal_alb': ('willow_grey', 'willow_smooth'),
                 'sal_fra': ('willow_grey', 'willow_smooth'), 'pop_nig': ('willow_grey', 'willow_goat'),
                 'pop_alb': ('willow_grey', 'willow_goat'), 'pop_can': ('willow_grey', 'willow_goat'),
                 'rob_pse': ('elder', 'elder'), 'ace_neg': ('spindle', 'hazel'), 'ace_pse': ('shreddy', 'hazel'),
                 'aes_hip': ('shreddy', 'hazel'), 'que_rub': ('willow_grey', 'hazel'), 'fra_pen': ('willow_grey', 'willow_grey'),
                 'mal_dom': ('shreddy', 'shreddy'), 'pyr_com': ('hawthorn', 'hawthorn'), 't_pru_cer': ('cherry', 'cherry'),
                 'pru_dom': ('spindle', 'cherry'), 'jug_reg': ('hawthorn', 'willow_goat')}


def bark_names():
    return [t['name'] for t in BARK_TEX.get('types', [])]


def _flat(e, prefix=''):
    """An entry's values with nested groups flattened (leaf: {a: ..} -> leaf_a; bark: {upper: ..} -> bark_upper)."""
    out = {}
    for k, v in e.items():
        if isinstance(v, dict):
            out.update(_flat(v, prefix + k + '_'))
            if prefix == '' and 'base' in v:
                out.setdefault(k, v['base'])
        else:
            out[prefix + k] = v
    return out


def _layer(v, names):
    """A layer index from an index or a layer name (-1 none)."""
    if isinstance(v, bool) or v is None:
        return -1
    if isinstance(v, (int, float)):
        return int(v) if v >= 0 else -1
    if isinstance(v, str):
        if v in names:
            return names.index(v)
        if v.lstrip('-').isdigit():
            return int(v)
    if isinstance(v, (list, tuple)) and v:
        return _layer(v[0], names)
    return -1


def _pick(e, names, *keys):
    for k in keys:
        if k in e:
            i = _layer(e[k], names)
            if i >= 0:
                return i
    return -1


def tree_entry(sid):
    """The species' tree layers, normalised: tree_layers.json (WP3), else the species registered in the shrubs'
    manifests under its own id; None while neither has it."""
    leaf_names = [l['name'] for l in LEAF_TEX.get('layers', [])]
    names = bark_names()
    e = None
    for k in (sid, sid[2:] if sid.startswith('t_') else None, 't_' + sid):
        if k and k in TREE_TEX:
            e = _flat(TREE_TEX[k])
            break
    src = 'tree layers (tools/shrubs/textures/out/tree_layers.json)'
    if e is None:
        # (The plain id only where no shrub has it: the shrubs' pru_cer is the shrub's own.)
        le = LEAF_TEX.get('species', {}).get(sid)
        if le is None or sid not in BARK_TEX.get('species', {}):
            return None
        e = dict(_flat(le), bark=BARK_TEX['species'][sid])
        e['bark_upper'] = BARK_TEX.get('species_upper', {}).get(sid, -1)
        src = 'tree layers (the shrubs\' manifests)'
    needles = SP[sid]['arch'].get('leaf_kind') == 'needles'
    a = _pick(e, leaf_names, *(('needles', 'needles_a', 'needle', 'sprig', 'leaf_a', 'a', 'leaf') if needles else
                               ('leaf_a', 'a', 'leaf', 'leaf_leaf_a', 'needles')))
    b = _pick(e, leaf_names, 'needles_b', 'leaf_b', 'b', 'leaf_leaf_b')
    out = {'source': 'tree', 'note': src,
           'tex': [a, b if b >= 0 else a, _pick(e, leaf_names, 'card', 'spray', 'spray_card', 'leaf_card'),
                   _pick(e, leaf_names, 'leaflet', 'leaf_leaflet')],
           'bark': _pick(e, names, 'bark', 'bark_base', 'base', 'bark_lower'),
           'bark_upper': _pick(e, names, 'bark_upper', 'upper', 'bark_top'),
           'fit': e.get('leaf_fit') or e.get('fit_leaf'), 'card_fit': e.get('card_fit') or e.get('fit_card'),
           'hull': e.get('leaf_hull') or e.get('hull'),
           'cover': e.get('cover', e.get('coverage', e.get('leaf_cover')))}
    if isinstance(out['cover'], dict):
        out['cover'] = None
    if out['bark'] < 0:
        out['bark'] = 0
    if out['bark_upper'] < 0:
        out['bark_upper'] = out['bark']
    out['hull_key'] = sid if sid in LEAF_HULLS else (sid[2:] if sid.startswith('t_') and sid[2:] in LEAF_HULLS else None)
    return out


def textures(sid):
    """The species' layers: leaf a, b, card, leaflet, the hull, the bark base and upper, the stats for the fits."""
    names = bark_names()
    out = tree_entry(sid)
    if out is None:
        out = {'source': 'shrub', 'note': 'fallback', 'fit': None, 'card_fit': None, 'hull': None, 'cover': None}
        src = FALLBACK_LEAF[sid]
        le = LEAF_TEX.get('species', {}).get(src, {})
        a = le.get('a', 0)
        out['source'] = 'shrub:' + src
        out['tex'] = [a, le.get('b', a), le.get('card', -1), le.get('leaflet', -1)]
        base, upper = FALLBACK_BARK[sid]
        out['bark'] = names.index(base) if base in names else 0
        out['bark_upper'] = names.index(upper) if upper in names else out['bark']
        out['hull_key'] = src
    # The tint: a borrowed bark toward the species' photographed trunk (the photos show the lower trunk: the base
    # layer's mean; a hue correction, its value moved a third at most); a species' own bark layer wears its colour.
    # The far tiers' bark is the upper layer's where a zone has one (the birch's white, the pine's orange limbs).
    out['tint'] = [1.0, 1.0, 1.0]
    bm = TEX_STATS.get('bark_mean', {})
    base_name = names[out['bark']] if 0 <= out['bark'] < len(names) else None
    upper_name = names[out['bark_upper']] if 0 <= out['bark_upper'] < len(names) else None
    mean_name = upper_name if SP[sid]['extra'].get('bark_zone') else base_name
    out['bark_mean'] = bm.get(mean_name) if mean_name else None
    base_mean = bm.get(base_name) if base_name else None
    if out['source'] != 'tree' and base_mean:
        bk = PHOTOS[src_of(sid)].get('bark', {}).get('all', {}).get('hex') or \
            (PHOTOS[src_of(sid)].get('wood_far') or {}).get('hex')
        if bk:
            t, m = to_lin(hex_rgb(bk)), to_lin(hex_rgb(base_mean))
            k = [x / max(y, 1e-4) for x, y in zip(t, m)]
            ky = lum(t) / max(lum(m), 1e-4)
            # (Half the photo's chromaticity - a few trunk photos per species - and the value moved a third at most.)
            norm = min(max(ky, 0.75), 1.35) / max(ky, 1e-4)
            out['tint'] = [round(min(max((x / ky) ** 0.5 * ky * norm, 0.7), 1.45), 3) for x in k]
    # (A species' own tint where its layer's colour reads off its photos in the game's light.)
    if SP[sid]['extra'].get('bark_tint'):
        out['tint'] = [float(x) for x in SP[sid]['extra']['bark_tint']]
    return out


TEXTURES = {}


def texture_source():
    own = sorted(s for s, t in TEXTURES.items() if t['source'] == 'tree')
    if len(own) == len(TEXTURES):
        return 'every species on its own layers, %s' % TEXTURES[own[0]]['note']
    if not own:
        return 'every species on the nearest shrub layers (FALLBACK_LEAF, FALLBACK_BARK) until the tree layers exist'
    return '%d species on their own layers, %s on the nearest shrub layers' % (
        len(own), ', '.join(s for s in TEXTURES if s not in own))


def texture_lines(sid):
    t = TEXTURES[sid]
    a_ = SP[sid]['arch']
    layers = LEAF_TEX.get('layers', [])
    tex = list(t['tex'])
    a = tex[0]
    # (Leaf b is drawn in a's square: a b of another size goes, as the shrubs'.)
    if 0 <= tex[1] < len(layers) and tex[1] != a and 0 <= a < len(layers):
        ba, bb = layers[a]['box_mm'], layers[tex[1]]['box_mm']
        if any(abs(bb[i] - ba[i]) > 0.12 * ba[i] for i in range(2)):
            tex[1] = a
    kind = a_.get('leaf_kind', 'simple')
    hull = t['hull'] or LEAF_HULLS.get(t.get('hull_key') or '', [1.0, 0.3333, 1.0, 0.6667, 1.0, 1.0])
    box = lambda i: [x / 1000.0 for x in layers[i]['box_mm']] if 0 <= i < len(layers) else None
    pet = a_.get('petiole', 0.01)
    ll, lw = mean(a_['leaf_len']) if a_.get('leaf_len') else 0.05, mean(a_['leaf_wid']) if a_.get('leaf_wid') else 0.03
    fit = [1.0, 1.0, 0.0]
    if t['fit']:
        fit = list(t['fit'])[:3]
    elif box(a):
        bl, bw = box(a)
        if kind == 'compound':
            n = max(round(a_.get('leaflets', 1)), 1)
            le = a_.get('rachis', 0.05) + ll * (1.0 if n % 2 else 0.5)
            we = max(2.0 * ll * math.sin(math.radians(a_.get('leaflet_angle', 60))), lw)
            fit = [bl / le, bw / we, (0.006 * bl + pet) / le]
        elif kind == 'needles':
            le, we = a_.get('leaf_spacing', 0.05) * 1.1, 2.0 * a_.get('needle_len', 0.014)
            fit = [bl / le, bw / we, 0.006 * bl / le]
        else:
            fit = [bl / ll, bw / lw, ((0.016 if layers[a]['kind'] == 'simple' else 0.006) * bl + pet) / ll]
    cl = tex[3] if tex[3] >= 0 else a
    cover = t['cover'] if t['cover'] is not None else \
        (TEX_STATS.get('leaf_cover', {}).get(layers[cl]['name'], 0.0) if 0 <= cl < len(layers) else 0.0)
    if t['fit'] and len(t['fit']) > 3 and t['cover'] is None:
        cover = t['fit'][3]
    # (A spray species without a spray card keeps the card fit at one: it draws its leaves alone at every tier.)
    card = [1.0, 1.0, 0.0]
    if t['card_fit']:
        card = list(t['card_fit'])[:3]
    elif kind == 'spray' and box(tex[2]):
        bl, bw = box(tex[2])
        splay = math.radians(mean(a_.get('leaf_angle', [45, 60])))
        le, we = max(a_.get('spray_len', 0.1), 0.03), 2.0 * ll * math.sin(splay) + lw
        card = [bl / le, bw / we, 0.006 * bl / le]
    ex = SP[sid]['extra']
    zone = ex.get('bark_zone')
    # (One bark all the way up: no upper layer, -1.)
    upper = t['bark_upper'] if zone and t['bark_upper'] >= 0 else -1
    src = 'the tree layers' if t['source'] == 'tree' else \
        'borrowed from the shrubs\' %s until the tree layers exist' % t['source'][6:]
    # (A species' own zone by its share of the tree's height - its metres are a tree of the species' mean height's -;
    # a burnt or dead variant's in metres above the ground: a scorch line, the bark lost from a dead trunk's top.)
    share = bool(zone) and not ex.get('base')
    zone_out = [round(float(x) / mean(SP[sid]['h']), 4) for x in zone] if share else [float(x) for x in (zone or (0.0, 0.0))]
    lines = ['; textures (tools/shrubs/textures; %s):' % src,
             '; leaf layers a, b, card, leaflet; the near leaf hull; the bark of the lower trunk and the upper bark taking',
             '; over between the zone\'s heights (shares of the tree\'s height above the ground, or m: its mode); lichen;',
             '; the squares over the elements; the bark as the species wears it'] + [
            '; ' + x for x in t.get('bark_note', [])] + [
            'leaf_tex = %s' % fmt([float(x) for x in tex]), 'leaf_hull = %s' % fmt([float(x) for x in hull]),
            'bark_tex = %s' % fmt(float(t['bark'])), 'bark_tex_upper = %s' % fmt(float(upper)),
            'bark_zone = %s' % fmt(zone_out), 'bark_zone_mode = %s' % ('share' if share else 'height'),
            'bark_zone_order = %s' % ex.get('bark_zone_order', 'fissures'),
            'lichen = %s' % fmt(round(ex.get('lichen', 0.25), 3)),
            'leaf_fit = %s' % fmt([round(x, 4) for x in fit + [cover]]), 'card_fit = %s' % fmt([round(x, 4) for x in card]),
            'young_relief = %s' % fmt(ex.get('young_relief', 0.25)), 'bark_tint = %s' % fmt(t['tint']),
            'bark_contrast = %s' % fmt(bark_contrast(sid)),
            'bark_scale = %s' % fmt(layer_scale(t.get('scale_layer', t['bark'])) or ex.get('bark_scale', 2.5))]
    up = upper_bark_keys(sid, upper) if upper != t['bark'] else None
    if up:
        lines += ['; the upper bark\'s own tile scale and contrast (a smooth young bark at 1)',
                  'bark_scale_upper = %s' % fmt(up[0]), 'bark_contrast_upper = %s' % fmt(up[1])]
    return lines


# ======================================================================================================
# Season keys from the phenology
# ======================================================================================================
def wrap(d):
    return round(d % 365.0, 1)


def season_keys(sid):
    """The engine's season scalars of a species (the shrubs' rows 0..2) as day:value keys."""
    ph = PHENO[sid]
    ex = SP[sid]['extra']
    draw = SP[sid]['draw']
    k = {}
    if ex.get('evergreen'):
        # Every needle on all year; the inner, oldest needles yellow in September and fall by November, their share
        # of the crown gone until the new needles of June replace them.
        n = ph['new_needles']
        share = ex.get('inner_needles', 0.1)
        k['leaf_out'] = [(1, 1.0)]
        k['leaf_grow'] = [(1, 1.0)]
        k['autumn'] = [(n - 8, share), (n, 0.0), (ph['autumn_start'], 0.0), (ph['autumn_peak'], share * 0.8),
                       (ph['leaf_fall_end'], share)]
        k['fallen'] = [(n - 8, share), (n, 0.0), (ph['leaf_fall_start'], 0.0), (ph['leaf_fall_end'], share)]
    else:
        b, u, f = ph['bud_burst'], ph['leaf_unfold'], ph['leaf_full']
        a0, ap, l0, l1 = ph['autumn_start'], ph['autumn_peak'], ph['leaf_fall_start'], ph['leaf_fall_end']
        k['leaf_out'] = [(b - 2, 1.0), (b - 1, 0.0), (u, 0.45), (f, 1.0)]
        k['leaf_grow'] = [(b - 2, 1.0), (b - 1, 0.3), (f, 0.85), (f + 20, 1.0)]
        k['autumn'] = [(b - 2, 1.0), (b - 1, 0.0), (a0, 0.0), (ap, 0.65), (l1, 1.0)]
        k['fallen'] = [(b - 2, 1.0), (b - 1, 0.0), (l0, 0.0), (l1, 1.0)]
    fs, fe = ph.get('flower_start'), ph.get('flower_end')
    if fs and fe and draw.get('bloom_shape', 'none') != 'none':
        k['bloom'] = [(fs - 12, 0.0), (fs + 3, 1.0), (fe - 3, 1.0), (fe + 8, 0.0)]
        k['bloom_stage'] = [(fs - 12, 0.0), (fs + 2, 1.0), (fe - 2, 1.0), (fe + 6, 2.0)]
    else:
        k['bloom'] = [(1, 0.0)]
        k['bloom_stage'] = [(1, 0.0)]
    r = ph.get('fruit_ripe')
    shape = draw.get('fruit_shape', 'none')
    if shape == 'cone' and ex.get('evergreen'):
        if sid == 'pin_syl':
            # Cones of three years on the tree at once: the 2nd-year cones green in summer, brown when ripe in October,
            # open and grey from February.
            k['fruit'] = [(1, 1.0)]
            k['ripe'] = [(45, 1.25), (120, 1.3), (165, 0.75), (230, 0.8), (wrap(r), 1.0), (335, 1.1)]
        else:
            # Spruce: the cones set in June hang green, ripen brown in October, shed their seed in late winter and
            # fall whole in the spring.
            k['fruit'] = [(150, 0.0), (172, 1.0), (60, 1.0), (115, 0.0)]
            k['ripe'] = [(150, 0.0), (172, 0.45), (240, 0.7), (wrap(r), 1.0), (60, 1.3), (115, 1.4)]
    elif ex.get('fruit_all_year') and r:
        # Alder's woody cones: last year's black ones on the tree all year, the new ones green from May, ripe and
        # brown in October, black over the winter.
        k['fruit'] = [(1, 1.0)]
        k['ripe'] = [(130, 0.5), (230, 0.75), (wrap(r), 1.0), (340, 1.25), (60, 1.35)]
    elif shape != 'none' and fe and r:
        fset = min(fe, r - 20)
        set_ = fset + 10
        turn = wrap(max(fset + 5, r - max(14, 0.3 * (r - fset))))
        b = ph.get('bud_burst') or 100
        if ex.get('fruit_winter'):
            # Last year's fruit hangs through the winter and is gone by the bud burst, or by the flowering where the
            # flowers come first (ash, the maples): the new crop never overlaps the old one's ripe value.
            old = min(b, fset) - 2
            k['fruit'] = [(fset, 0.0), (set_, 1.0), (wrap(r + 60), 1.0), (wrap(old - 18), 0.35), (wrap(old), 0.0)]
            k['ripe'] = [(fset, 0.0), (turn, 0.7), (r, 1.0), (wrap(r + 90), 1.4), (wrap(old - 5), 1.8), (wrap(old), 0.0)]
        elif shape == 'cotton':
            k['fruit'] = [(fset, 0.0), (set_, 1.0), (r + 12, 1.0), (r + 25, 0.0)]
            k['ripe'] = [(fset, 0.0), (turn, 0.7), (r, 1.0), (r + 25, 1.0), (r + 30, 0.0)]
        elif sid in ('ulm_lae', 'ulm_min'):
            # Elm samaras: green on the bare crown, ripe and gone within weeks.
            k['fruit'] = [(fset, 0.0), (set_, 1.0), (r + 5, 1.0), (r + 18, 0.0)]
            k['ripe'] = [(fset, 0.0), (turn, 0.7), (r, 1.0), (r + 18, 1.0)]
        else:
            k['fruit'] = [(fset, 0.0), (set_, 1.0), (r + 25, 1.0), (r + 50, 0.0)]
            k['ripe'] = [(fset, 0.0), (turn, 0.7), (r, 1.0), (r + 30, 1.25), (r + 50, 1.4)]
    else:
        k['fruit'] = [(1, 0.0)]
        k['ripe'] = [(1, 0.0)]
    if draw.get('catkin_shape', 'none') != 'none' and fs and fe:
        if ex.get('catkin_winter'):
            # Formed in late summer, closed through the winter, long and open at flowering, then shed.
            k['catkin'] = [(200, 0.0), (230, 1.0), (wrap(fs - 8), 1.0), (wrap(fs + 2), 2.0), (wrap(fe), 2.0),
                           (wrap(fe + 8), 3.0), (wrap(fe + 14), 0.0)]
            k['catkin_grow'] = [(230, 0.35), (wrap(fs - 8), 0.45), (wrap(fs + 4), 1.0), (wrap(fe + 8), 1.0)]
        else:
            k['catkin'] = [(wrap(fs - 12), 0.0), (wrap(fs - 4), 1.0), (wrap(fs + 2), 2.0), (wrap(fe), 2.0),
                           (wrap(fe + 8), 3.0), (wrap(fe + 14), 0.0)]
            k['catkin_grow'] = [(wrap(fs - 12), 0.4), (wrap(fs + 4), 1.0)]
    # (The top-down and inside/outside order spans about two weeks of the crown's colouring: more of the wave than a
    # bush's.)
    k['autumn_spread'] = [(1, 0.2 if ex.get('evergreen') else 0.5)]
    # (A tree's fruit groups keep every fruit they set: shrub_fruit_keep.)
    k['fruit_keep'] = [(1, 1.0)]
    return k


def fmt_keys(keys, colour=False):
    pts = {}
    for d, v in keys:
        pts[wrap(d)] = v
    items = sorted(pts.items())
    if colour:
        return ', '.join('%g:%s' % (d, v) for d, v in items)
    return ', '.join('%g:%g' % (d, round(v, 3)) for d, v in items)


# ======================================================================================================
# Communities (SPECIES.md 1.2, 1.3; shares in per cent, at most 12 species each)
# ======================================================================================================
COMMUNITIES = {
    # planted pine 40-80 yr: pine 85-100 %, birch 0-15 %, single oak and aspen; rowan in the bilberry pine
    'pine_forest': {'pin_syl': 86, 'bet_pen': 9, 'que_rob': 2, 'pop_tre': 2, 'sor_auc': 1},
    # 7-9 tenths pine with birch, the oak-pine subir with oak as a second layer
    'pine_birch': {'pin_syl': 68, 'bet_pen': 18, 'que_rob': 6, 'pop_tre': 4, 'sor_auc': 2, 'pic_abi': 2},
    # birch forest on old fields, burns and the cooling pond: birch 70-90 %, aspen 5-20 %, young pine, downy birch wet
    'birch_aspen': {'bet_pen': 66, 'pop_tre': 16, 'pin_syl': 8, 'bet_pub': 6, 'sor_auc': 2, 'sal_fra': 2},
    # alder swamp: black alder 80-100 %, downy birch
    'alder_swamp': {'aln_glu': 80, 'bet_pub': 14, 'sal_fra': 3, 'fra_exc': 3},
    # the Pripyat and Uzh floodplains: white and crack willow, black and white poplar, aspen, oak on the levees, green
    # ash on the islands
    'floodplain': {'sal_alb': 24, 'sal_fra': 18, 'pop_nig': 14, 'pop_alb': 12, 'pop_tre': 10, 'que_rob': 10,
                   'fra_pen': 6, 'aln_glu': 6},
    # oak-pine and hornbeam-oak forests of the richer soils (the Tilio-Carpinetum cluster)
    'oak_mixed': {'que_rob': 38, 'car_bet': 18, 'pin_syl': 12, 'til_cor': 10, 'ace_pla': 8, 'fra_exc': 6, 'ulm_lae': 4,
                  'pop_tre': 4},
    # an abandoned village: box elder 25, fruit trees 29, robinia 15, birch and aspen 10, maples 6, poplars 5, ash 3,
    # elm 2 (the lime, oak, walnut, willows and horse chestnut of 1 % and less left out)
    'village': {'ace_neg': 25, 'pru_dom': 15, 'rob_pse': 15, 'bet_pen': 6, 'mal_dom': 6, 'ace_pla': 6, 'pop_alb': 5,
                'pop_tre': 4, 'pyr_com': 4, 't_pru_cer': 4, 'fra_exc': 3, 'ulm_lae': 2},
    # orchard relics: apple stands, plum groves, pear and cherry, box elder shading them out, walnut by the houses
    'orchard': {'mal_dom': 35, 'pru_dom': 25, 'pyr_com': 15, 't_pru_cer': 12, 'ace_neg': 8, 'jug_reg': 5},
    # the Red Forest: the pines killed in 1986 (their kinds stand dead) and the birch, aspen and alder regrowth
    'red_forest': {'pin_syl': 45, 'bet_pen': 32, 'pop_tre': 13, 'aln_glu': 10},
    # Soviet-era rows along the roads and shelterbelts
    'roadside': {'pop_can': 22, 'pop_nig': 14, 'bet_pen': 14, 'rob_pse': 10, 'ace_neg': 10, 'til_cor': 8, 'ace_pla': 7,
                 'fra_pen': 6, 'que_rub': 5, 'aes_hip': 4, 'ulm_min': 3},
    # Pripyat's, Chornobyl's and the towns' streets: Norway maple dominating the regeneration, lime, horse chestnut and
    # Lombardy poplar planted, sparse aspen and birch canopies over the centre, box elder and robinia
    'town': {'ace_pla': 24, 'bet_pen': 14, 'til_cor': 12, 'pop_tre': 10, 'aes_hip': 8, 'pop_nig': 8, 'ace_neg': 8,
             'pop_alb': 5, 'rob_pse': 5, 'ace_pse': 3, 'fra_exc': 3},
}
# The habitat of a level's trees (where its generic broadleaf models stand), by what the level is.
LEVEL_COMMUNITY = {
    'jupiter': 'village', 'jupiter_underground': 'town', 'k00_marsh': 'alder_swamp', 'k01_darkscape': 'pine_birch',
    'k02_trucks_cemetery': 'pine_forest', 'l01_escape': 'village', 'l02_garbage': 'birch_aspen',
    'l03_agroprom': 'oak_mixed', 'l03u_agr_underground': 'oak_mixed', 'l04_darkvalley': 'pine_birch', 'l05_bar': 'town',
    'l06_rostok': 'birch_aspen', 'l08_yantar': 'alder_swamp', 'l08u_brainlab': 'alder_swamp', 'l09_deadcity': 'town',
    'l10_limansk': 'town', 'l10_radar': 'pine_forest', 'l10_red_forest': 'red_forest', 'l11_hospital': 'town',
    'l11_pripyat': 'town', 'l12_stancia': 'roadside', 'l12_stancia_2': 'roadside', 'l13_generators': 'pine_birch',
    'new_military': 'pine_birch', 'pripyat': 'town', 'zaton': 'floodplain'}
WET = ('floodplain', 'alder_swamp')
BOG = ('alder_swamp',)

# ======================================================================================================
# The kinds: species or community, flags, skeleton
# ======================================================================================================
HINT_SPECIES = {'pine': 'pin_syl', 'spruce': 'pic_abi', 'oak': 'que_rob', 'alder': 'aln_glu', 'cherry': 't_pru_cer',
                'walnut': 'jug_reg'}


def habitat(k):
    """The kind's habitat: the community of the levels that hold most of its placements; a model spread over several
    habitats with none holding half of it takes the zone's commonest broadleaf succession (birch and aspen grow in its
    towns, forests and fields alike)."""
    tally = {}
    for lv, n in k['levels'].items():
        c = LEVEL_COMMUNITY.get(lv)
        if c:
            tally[c] = tally.get(c, 0) + n
    if not tally:
        return 'birch_aspen'
    best = max(sorted(tally), key=lambda c: tally[c])
    return best if tally[best] >= 0.5 * sum(tally.values()) else 'birch_aspen'


def share_in(k, communities):
    tot = sum(k['levels'].values()) or 1
    return sum(n for lv, n in k['levels'].items() if LEVEL_COMMUNITY.get(lv) in communities) / tot


# A generic broadleaf kind's species by its trunk: a living one whose stock trunk is thick for its height - 0.6 m and
# more at breast height in the game and over 0.045 of its height (the stock giants stand 1.2-3.3 m thick; a birch's
# trunk rarely passes a fortieth of its height) - takes its community's thick-trunked species (the oaks, the poplars,
# the willows, the alder) at their shares: the one of them alone where it holds four fifths, else <community>_thick.
THICK = ('que_rob', 'que_rub', 'pop_tre', 'pop_nig', 'pop_alb', 'pop_can', 'sal_alb', 'sal_fra', 'aln_glu')
THICK_DBH, THICK_RATIO = 0.6, 0.045
_TRUNK = {}


def trunk_of(k):
    """The kind's stock trunk in the game's scale: its diameter 1.3 m above the root and the skeleton's height (None
    without a skeleton or a stock trunk)."""
    sk = skel_of(k)
    if sk not in _TRUNK:
        _TRUNK[sk] = None
        p = os.path.join(SKEL_DIR, sk + '.dtsk')
        if os.path.exists(p):
            import sys as _sys
            _sys.path.insert(0, os.path.join(HERE, 'skeleton'))
            import dtsk
            s = dtsk.unpack(open(p, 'rb').read())
            ax = s.axes[0] if s.axes else None
            if ax is not None and ax['n_rings'] and not (ax['flags'] & dtsk.A_FROM_CARDS):
                r = s.rings[ax['first_ring']:ax['first_ring'] + ax['n_rings']]
                y0 = float(s.rings['c'][:, 1].min())
                i = int(abs(r['c'][:, 1] - (y0 + 1.3)).argmin())
                _TRUNK[sk] = (2.0 * float(r['r0'][i]), float(s.bmax[1]) - y0)
    t = _TRUNK[sk]
    if t is None:
        return None
    sc = k.get('scale_p50') or 1.0
    return t[0] * sc, t[1] * sc


def thick_pick(k, comm, why):
    """A living generic broadleaf kind's pick: its community, or the community's thick-trunked species for a thick
    trunk."""
    if k['dead'] or k['conifer']:
        return None, comm, why
    t = trunk_of(k)
    if not t or t[0] < THICK_DBH or t[0] < THICK_RATIO * t[1]:
        return None, comm, why
    thick = {s: v for s, v in COMMUNITIES.get(comm, {}).items() if s in THICK}
    if not thick:
        return None, comm, why
    why += '; a thick trunk (%.1f m at breast height, %.3f of its height)' % (t[0], t[0] / max(t[1], 0.1))
    best = max(sorted(thick), key=lambda s: thick[s])
    if thick[best] >= 0.8 * sum(thick.values()):
        return best, None, why + ': ' + best
    name = comm + '_thick'
    COMMUNITIES.setdefault(name, dict(thick))
    return None, name, why + ': ' + name


def species_of_kind(k):
    """species id or None, community or None, and the rule that decided (TREE_DATA.md)."""
    hints = k['species_hint'] or []
    hint = max(k['card_area'], key=k['card_area'].get) if k['card_area'] else (hints[0] if hints else None)
    hab = habitat(k)
    m = k['model']
    slender = (m.get('span') or 0) / max(m.get('height') or 1.0, 0.1)
    top = k['top']['p50'] or m.get('height') or 10.0
    if k['red'] and not k['conifer'] and hint in (None, 'deciduous (generic cards)', 'unknown (generic bark)'):
        return thick_pick(k, 'red_forest', 'generic cards of the Red Forest')
    if hint in HINT_SPECIES:
        return HINT_SPECIES[hint], None, 'cards: %s' % hint
    if hint == 'birch':
        if share_in(k, BOG) >= 0.6:
            return 'bet_pub', None, 'cards: birch, mostly in the swamps'
        return 'bet_pen', None, 'cards: birch'
    if hint == 'willow':
        return ('sal_alb', None, 'cards: willow, tall') if top >= 18.0 else ('sal_fra', None, 'cards: willow')
    if hint == 'poplar':
        if slender <= 0.4:
            return 'pop_nig', None, 'cards: poplar, a column (Lombardy)'
        if (m.get('height') or 0) < 12.0:
            return 'pop_tre', None, 'cards: poplar, a young grove (aspen)'
        if hab in WET:
            return 'pop_alb', None, 'cards: poplar, tall on wet ground'
        return 'pop_can', None, 'cards: poplar, tall rows'
    return thick_pick(k, hab, 'generic: %s' % hab)


def kind_value(k):
    """[tree_kinds] value of a kind, and the decision for the docs."""
    cls = k['class']
    flags = []
    if cls == 'STOCK':
        return ', '.join(['STOCK'] + (['dead'] if k['dead'] else []) + (['red'] if k['red'] else []) +
                         (['conifer'] if k['conifer'] else [])), None, None, k['stock_reason']
    sp, comm, why, dead = pick_of(k)
    if k.get('anomalous'):
        why += '; an anomaly, %s' % ('dead: bare or no cards' if dead else 'alive: leafy cards')
    if k['key'] in DEAD:
        sp, comm = DEAD[k['key']]
        why += '; the dead barks'
    if dead:
        flags.append('dead')
    if k['red']:
        flags.append('red')
    if k['conifer']:
        flags.append('conifer')
    # The stock's sapling models are young trees whatever their height (the engine's youth 1).
    if 'sapling' in (k.get('flags') or []):
        flags.append('sapling')
    pick = 'species=%s' % sp if sp else 'community=%s' % comm
    return ', '.join([cls] + flags + [pick, 'skel=%s' % skel_of(k)]), sp, comm, why


_DTSK = set()
if os.path.isdir(SKEL_DIR):
    _DTSK = {os.path.splitext(f)[0].lower() for f in os.listdir(SKEL_DIR) if f.lower().endswith('.dtsk')}
# The skeleton builder's index (tools/trees/skeleton/plan.py: kind -> skeleton; it gives a trunk mesh's other card
# layouts skeletons of their own).
SKEL_INDEX = _json(os.path.join(HERE, 'out', 'skel_index.json')).get('kinds', {})


def skel_of(k):
    """The kind's skeleton: the builder's index, else the census's id (or the first of its other readings that has a
    file once skeletons exist)."""
    if k['key'] in SKEL_INDEX:
        return SKEL_INDEX[k['key']]
    if _DTSK and k['skel'].lower() not in _DTSK:
        for c in k.get('skel_candidates', []):
            if c.lower() in _DTSK:
                return c
    return k['skel']


# ======================================================================================================
# Burnt trees: variants of the species they can take
# ======================================================================================================
# A standing burnt tree that still carries foliage survived and resprouts: the variant <id>_brn of each species its
# habitat's community can give it, with a charred base - the char layer up to the scorch line (an eighth of the
# tree's height, 1-3 m) blended over 0.6 m into the species' main bark (its upper layer where the species' own zone
# ends by 3.5 m: a birch's white, an aspen's grey-green; else its lower bark); <habitat>_brn lists them at the
# community's shares. A bare burnt snag (no foliage cards) is dead and charred all the way up: <id>_char of the
# commonest species of its habitat (its crown field is empty, the species shows in nothing but the char). The char
# layer is tree_layers.json's `char`; until it exists the darkest shrub bark tinted to charcoal.
CHAR_HEX = '#1A1714'
SCORCH = dict(share=0.12, low=1.0, high=3.0, blend=0.6)
BURNT = {}
BURNT_NOTES = {}


def char_layer():
    """The char layer's index, the tint it is worn in (None: its own colours) and where it came from."""
    names = bark_names()
    raw = _json(TREE_LAYERS)
    cands = []
    if isinstance(raw, dict):
        cands.append(raw.get('char'))
        if isinstance(raw.get('species'), dict):
            cands.append(raw['species'].get('char'))
    cands += [TREE_TEX.get('char'), BARK_TYPES.get('char')]
    for c in cands:
        if isinstance(c, dict):
            c = next((c[k] for k in ('bark', 'index', 'layer', 'name') if k in c), None)
        i = _layer(c, names)
        if i >= 0:
            return i, None, 'the char layer (tree_layers.json)'
    if 'char' in names:
        return names.index('char'), None, 'the char layer (bark_manifest.json)'
    # (The darkest shrub bark, tinted to charcoal: the tint darkens the species' bark above the scorch line too.)
    bm = TEX_STATS.get('bark_mean', {})
    dark = min((n for n in names if n in bm), key=lambda n: lum(to_lin(hex_rgb(bm[n]))), default=None)
    if dark is None:
        return 0, None, 'no char layer'
    tint = [round(min(max(t / max(m, 1e-4), 0.02), 2.0), 3) for t, m in zip(to_lin(hex_rgb(CHAR_HEX)), to_lin(hex_rgb(bm[dark])))]
    return names.index(dark), tint, 'the darkest shrub bark (%s) tinted to %s until the char layer exists' % (dark, CHAR_HEX)


def make_variant(base, vid, zone, note):
    """A copy of a species under a new id: its architecture, drawing, seasons and colours, its own bark (its zone in
    metres, its layers taking over fissures first - a char's ridges and a dead trunk's plates go last)."""
    s = SP[base]
    a = {k: (dict(v) if isinstance(v, dict) else list(v) if isinstance(v, list) else v) for k, v in s['arch'].items()}
    ex = dict(s['extra'], bark_zone=zone, base=base, photos=src_of(base))
    ex.pop('bark_zone_order', None)
    SP[vid] = dict(id=vid, latin='%s - %s' % (s['latin'], note), h=list(s['h']), w=list(s['w']), arch=a,
                   draw=dict(s['draw']), extra=ex, base=base)
    PHENO[vid] = PHENO[base]


def burnt_variants():
    """The burnt kinds' picks (BURNT), their variant species and communities."""
    ci, ctint, csrc = char_layer()
    reach = {}
    for key, k in sorted(KINDS['kinds'].items()):
        if k['class'] != 'TREE' or not k.get('burnt'):
            continue
        hab = habitat(k)
        comm = COMMUNITIES[hab]
        fits = [s for s in comm if bool(SP[s]['extra'].get('evergreen')) == bool(k['conifer'])] or list(comm)
        if k['dead']:
            sp = max(fits, key=lambda s: comm[s])
            vid = sp + '_char'
            reach.setdefault(vid, (sp, False, []))[2].append(k)
            BURNT[key] = (vid, None, 'burnt and bare: dead, charred all the way up (the commonest of %s)' % hab)
        else:
            name = hab + '_brn'
            bc = COMMUNITIES.setdefault(name, {})
            for s in fits:
                bc[s + '_brn'] = comm[s]
                reach.setdefault(s + '_brn', (s, True, []))[2].append(k)
            BURNT[key] = (None, name, 'burnt with foliage: alive, a charred base (%s)' % hab)
    names = bark_names()
    bm = TEX_STATS.get('bark_mean', {})
    for vid, (base, scorch, ks) in sorted(reach.items()):
        n = sum(k['placements'] for k in ks) or 1
        top = sum((k['top']['p50'] or 0.0) * k['placements'] for k in ks) / n
        line = min(max(SCORCH['share'] * top, SCORCH['low']), SCORCH['high'])
        zone = (round(line - 0.5 * SCORCH['blend'], 2), round(line + 0.5 * SCORCH['blend'], 2)) if scorch else None
        make_variant(base, vid, zone, 'burnt: a charred base' if scorch else 'burnt: charred all the way up')
        t = dict(TEXTURES[base])
        bz = SP[base]['extra'].get('bark_zone')
        main = t['bark_upper'] if bz and bz[1] <= 3.5 and t['bark_upper'] >= 0 else t['bark']
        t['bark'], t['bark_upper'] = ci, (main if scorch else -1)
        if ctint:
            t['tint'] = ctint
        # (The far tiers go with the bark that covers the most wood: the main bark above a scorch line, the char on a
        # snag. The tile scale and contrast are the char's, the main bark's its own: bark_scale_upper.)
        shown = main if scorch else ci
        t['scale_layer'] = ci
        t['bark_mean'] = bm.get(names[shown]) if 0 <= shown < len(names) else None
        t['bark_note'] = ['the bark: %s,' % csrc, 'the species\' main bark above the scorch line' if scorch else
                          'all the way up']
        TEXTURES[vid] = t
        BURNT_NOTES[vid] = dict(base=base, zone=zone, kinds=[k['key'] for k in ks], top=round(top, 2), char=csrc)
    return csrc


# ======================================================================================================
# Dead trees: variants of the species they can take
# ======================================================================================================
# A dead tree - a `dead` kind: the census's dead and broken snags, the bare anomalies, the Red Forest's pines killed in
# 1986 - wears the dead barks: the variant <id>_dead of each species its pick can give it (its own species, else its
# community's, conifers for a conifer kind and broadleaves for the others, <community>_dead listing them at the
# community's shares), deadwood_bark - a dead pine's bleached plates, a third of them fallen, beetle galleries - up
# its trunk and deadwood_bare - weathered silver-grey wood - above: the species' own zone where it has one (its thin
# upper bark goes first: a pine's mirror bark), 3 to 6 m at the least. The bare burnt snags keep their char (<id>_char).
DEAD_ZONE = (3.0, 6.0)
DEAD = {}
DEAD_NOTES = {}


def bark_layer(name):
    """A bark type's layer by its name (tree_layers.json bark_types, else the bark manifest), -1 where there is none."""
    v = BARK_TYPES.get(name)
    if isinstance(v, dict) and isinstance(v.get('index'), int):
        return v['index']
    names = bark_names()
    return names.index(name) if name in names else -1


def pick_of(k):
    """A standing kind's species or community, the rule that decided, and whether it stands dead."""
    sp, comm, why = BURNT[k['key']] if k['key'] in BURNT else species_of_kind(k)
    dead = k['dead']
    # The Red Forest's tall pines are the ones killed in 1986 (the young ones are the replanted generation).
    if k['red'] and k['conifer'] and sp == 'pin_syl' and (k['top']['p50'] or 0) >= 12.0:
        dead = True
        why += '; the Red Forest\'s dead pines'
    return sp, comm, why, dead


def dead_variants():
    """The dead kinds' picks (DEAD), their variant species and communities."""
    lo, hi = bark_layer('deadwood_bark'), bark_layer('deadwood_bare')
    if lo < 0:
        return 'no dead bark layers: the dead kinds wear their species\' bark, weathered'
    reach = {}
    for key, k in sorted(KINDS['kinds'].items()):
        if k['class'] == 'STOCK' or key in BURNT:
            continue
        sp, comm, why, dead = pick_of(k)
        if not dead:
            continue
        if sp:
            reach.setdefault(sp, []).append(k)
            DEAD[key] = (sp + '_dead', None)
            continue
        src = COMMUNITIES[comm]
        fits = [s for s in src if bool(SP[s]['extra'].get('evergreen')) == bool(k['conifer'])] or list(src)
        dc = COMMUNITIES.setdefault(comm + '_dead', {})
        for s in fits:
            dc[s + '_dead'] = src[s]
            reach.setdefault(s, []).append(k)
        DEAD[key] = (None, comm + '_dead')
    names = bark_names()
    bm = TEX_STATS.get('bark_mean', {})
    for base, ks in sorted(reach.items()):
        vid = base + '_dead'
        own = SP[base]['extra'].get('bark_zone') or DEAD_ZONE
        zone = (max(own[0], DEAD_ZONE[0]), max(own[1], DEAD_ZONE[1]))
        make_variant(base, vid, zone, 'dead: the dead barks')
        t = dict(TEXTURES[base], bark=lo, bark_upper=hi, tint=[1.0, 1.0, 1.0], scale_layer=lo)
        # (The far tiers' bark is the upper layer's, as a living species' with a zone.)
        t['bark_mean'] = bm.get(names[hi if hi >= 0 else lo])
        t['bark_note'] = ['the bark: deadwood_bark up the trunk, deadwood_bare above the zone']
        TEXTURES[vid] = t
        DEAD_NOTES[vid] = dict(base=base, zone=zone, kinds=sorted({k['key'] for k in ks}),
                               placements=sum(k['placements'] for k in {k['key']: k for k in ks}.values()))
    return 'deadwood_bark %d, deadwood_bare %d' % (lo, hi)


def kind_lines():
    kinds, fams, decisions = [], {}, {}
    for key, k in sorted(KINDS['kinds'].items()):
        value, sp, comm, why = kind_value(k)
        decisions[key] = (value, sp, comm, why)
        for member in k['members']:
            kinds.append('%s = %s' % (member, value))
        if k['signature']:
            for member, row in sorted(k['family_rows'].items()):
                fams.setdefault(k['signature'], []).append(row)
    kinds = sorted(set(kinds))
    fam_lines = ['%s = %s' % (s, ', '.join(sorted(v))) for s, v in sorted(fams.items())]
    return kinds, fam_lines, decisions


# ======================================================================================================
# The file
# ======================================================================================================
ARCH_ORDER = ['href', 'envelope', 'env_base', 'stems', 'root_r', 'stem_spread', 'stem_len', 'stem_up', 'stem_arch',
              'stem_wobble', 'stem_radius', 'stem_seg', 'leader', 'whorl_step', 'levels', 'sag', 'prickle_spacing',
              'prickle_len', 'prickle_hook', 'prickle_base', 'prickle_young', 'zig']
LEVEL_ORDER = ['density', 'start', 'end', 'angle', 'phyllo', 'per_node', 'len', 'len_jitter', 'up', 'outward', 'wobble',
               'radius', 'seg_len', 'min_len']
LEAF_ORDER = ['leaf_kind', 'shoot_len', 'lai', 'fill_len', 'fill_shell', 'fill_clump', 'fill_up', 'fill_from',
              'spur_share', 'spur_leaves', 'spray_len', 'leaf_spacing', 'leaf_per_node', 'leaf_phyllo', 'leaf_angle',
              'leaf_flat', 'leaf_droop', 'leaf_len', 'leaf_wid', 'petiole', 'tip_shrink', 'leaflets', 'rachis',
              'leaflet_angle', 'needle_len', 'bloom_place', 'bloom_share', 'bloom_size', 'bloom_droop', 'fruit_size',
              'catkin_share', 'catkin_len', 'catkin_droop']
DRAW_ORDER = ['leaf_outline', 'leaflet_ratio', 'translucency', 'gloss', 'flutter', 'fold', 'underside', 'jitter',
              'bloom_shape', 'petals', 'fruit_shape', 'fruits', 'catkin_shape', 'bark', 'needle_density', 'evergreen',
              'lobes', 'autumn_mix']
SEASON_ORDER = ['leaf_out', 'leaf_grow', 'autumn', 'fallen', 'bloom', 'bloom_stage', 'fruit', 'ripe', 'catkin',
                'catkin_grow', 'autumn_spread', 'fruit_keep']
COLOUR_ORDER = ['c_young', 'c_summer', 'c_autumn_a', 'c_autumn_b', 'c_dead', 'c_under', 'c_bark', 'c_twig', 'c_petal',
                'c_centre', 'c_fruit_unripe', 'c_fruit_ripe', 'c_catkin']
# The policy: grafted growth on a stock skeleton tilted more than max_tilt_graft (deg) bends by gravity in model space
# per tilt bucket; placements scaled under min_scale stay stock; at most kinds_per_level_species species templates of
# one kind on one level (a community's draw). A tree's youth y: 1 for a YOUNG kind and a sapling kind, else by its
# height between young_h0 (a sapling up to it, m) and young_h1 (grown from it); a young tree's spring leads the old
# trees' by up to young_lead days (AAA_TREES.md 7: sub-canopy juveniles about 8 days ahead), its marcescence is its
# species' share scaled by its youth, and it bears 1 - y of its flowers, fruit and cones (the engine's); red_community:
# the community of the Red Forest's pines (a red kind draws from it; the engine knows no name of its own).
POLICY = [('max_tilt_graft', 15), ('min_scale', 0.05), ('kinds_per_level_species', 3), ('young_h0', 4), ('young_h1', 10),
          ('young_lead', 8), ('red_community', 'red_forest')]


def fmt(v):
    if isinstance(v, (list, tuple)):
        return ', '.join(fmt(x) for x in v)
    if isinstance(v, float):
        return ('%.4f' % v).rstrip('0').rstrip('.') if abs(v) < 100 else '%g' % v
    return str(v)


# The wood under this radius (m) wears the young twig's skin where a species gives none of its own: the fine twigs of
# a tree are smooth and twig-coloured before the bark forms (r3/da_shrub_wood_vs.h).
TWIG_R = 0.006


# The wood a cut end shows (r3\da_shrub_wood.ps shrub_end_grain): the fresh sapwood's colour, the heartwood's and its
# share of the radius (None: the wood is alike throughout - birch, spruce, maple, lime, alder, horse chestnut), a year's
# ring width (mm: the fast poplars and willows wide, the oaks, the pine and the fruit trees narrow). The cut surfaces'
# colours as the wood atlases and the sawn-log photos show them; the Zone's cuts are old - the shader weathers them.
WOOD = {
    'pin_syl': ('#E9D7A9', '#C78E5B', 0.5, 2.5), 'pic_abi': ('#EBDCB9', None, 0.0, 3.0),
    'bet_pen': ('#E8D9BD', None, 0.0, 3.0), 'bet_pub': ('#E6D6B8', None, 0.0, 2.5),
    'pop_tre': ('#E9DFC6', '#B8A88E', 0.3, 3.5), 'que_rob': ('#DCC6A0', '#A9784A', 0.7, 2.5),
    'car_bet': ('#E0D6C2', None, 0.0, 2.0), 'ace_pla': ('#E6D8BC', None, 0.0, 3.0),
    'til_cor': ('#E8DABB', None, 0.0, 3.0), 'fra_exc': ('#E4D3B0', '#A88E68', 0.35, 3.0),
    'ulm_lae': ('#DCCAA5', '#A88560', 0.6, 3.0), 'ulm_min': ('#DCC8A0', '#9A6E48', 0.65, 3.0),
    'sor_auc': ('#E2D0B0', '#A97A58', 0.4, 2.5), 'aln_glu': ('#C99A72', None, 0.0, 4.0),
    'sal_alb': ('#E5D7BD', '#BD957A', 0.55, 5.0), 'sal_fra': ('#E2D2B6', '#B88A6C', 0.5, 5.0),
    'pop_nig': ('#E3D5BA', '#B39B7E', 0.55, 6.0), 'pop_alb': ('#E6DAC2', '#BCA587', 0.5, 5.0),
    'pop_can': ('#E4D6BB', '#B9A083', 0.55, 7.0), 'rob_pse': ('#E0D2A0', '#B7974D', 0.8, 4.0),
    'ace_neg': ('#E8DCC4', None, 0.0, 5.0), 'ace_pse': ('#EDE2CB', None, 0.0, 3.0),
    'aes_hip': ('#EBDFC6', None, 0.0, 4.0), 'que_rub': ('#DCC6A3', '#B07F5C', 0.7, 4.0),
    'fra_pen': ('#E2D1AD', '#AE9068', 0.45, 4.0), 'mal_dom': ('#DEC6A6', '#A87158', 0.6, 2.5),
    'pyr_com': ('#D3AF91', '#B98D6F', 0.5, 2.5), 't_pru_cer': ('#DCC3A0', '#A4694A', 0.55, 3.0),
    'pru_dom': ('#DCC2A1', '#96604A', 0.6, 3.0), 'jug_reg': ('#D8CAB0', '#6E5040', 0.7, 4.0),
}


def wood_lines(sid):
    """The wood keys of a species (its base's for a burnt, charred or dead variant)."""
    base = SP[sid].get('base', sid)
    for cut in ('_brn', '_char', '_dead'):
        if base.endswith(cut):
            base = base[:-len(cut)]
    sap, heart, share, ring_mm = WOOD.get(base, ('#DEC8A0', None, 0.0, 3.0))
    lines = ['; the wood a cut end shows: the sapwood (linear rgb), the heartwood and its share of the radius, a',
             "; year's ring width (m)", 'wood_sap = %s' % fmt([round(x, 4) for x in to_lin(hex_rgb(sap))])]
    if heart:
        lines.append('wood_heart = %s' % fmt([round(x, 4) for x in to_lin(hex_rgb(heart))] + [float(share)]))
    lines.append('ring_width = %s' % fmt(round(ring_mm / 1000.0, 5)))
    return lines


def species_section(n, sid):
    s = SP[sid]
    ex = s['extra']
    a = dict(s['arch'])
    lines = ['[tree_species_%d]' % n, 'name = %s' % sid, '; %s' % s['latin']]
    lines.append('height = %s' % fmt([float(x) for x in s['h']]))
    lines.append('width = %s' % fmt([float(x) for x in s['w']]))
    lines.append('classes = TREE, YOUNG')
    # A living stock tree stays alive: the levels' own dead models carry the Zone's standing dead wood (a random share
    # turned a landmark tree the player knows into a grey snag). The species' research share stays in its entry.
    lines.append('dead = 0')
    lines.append('spread_days = 5')
    lines.append('; architecture: the young tree grown whole (YOUNG) and the levels grafted on a stock skeleton (the')
    lines.append('; stock trunk, limbs and card branches are level 0; da_shrub_gen.h)')
    for k in ARCH_ORDER:
        if k in a and a[k] is not None:
            lines.append('%s = %s' % (k, fmt(a[k])))
    lines.append('graft_levels = %d' % ex.get('graft_levels', a.get('graft_levels', 3)))
    for i, lv in enumerate(('l1', 'l2', 'l3')):
        if i >= a.get('levels', 3) or not a.get(lv):
            continue
        for k in LEVEL_ORDER:
            lines.append('%s_%s = %s' % (lv, k, fmt(a[lv][k])))
    for k in LEAF_ORDER:
        if k in a:
            lines.append('%s = %s' % (k, fmt(a[k])))
    if ex.get('cone'):
        share, length, droop = ex['cone']
        lines.append('; cones at the shoot tips (share, size m, droop: 1 hanging; the bloom keys where they are given); their')
        lines.append('; look and season are the fruit keys')
        lines += ['cone_share = %s' % fmt(share), 'cone_size = %s' % fmt(length), 'cone_droop = %s' % fmt(droop)]
    d = dict(s['draw'])
    # (A variant wears its base species' colours and year; only its bark is its own.)
    base = s.get('base', sid)
    ck = colour_keys(base)
    ck['c_bark'] = bark_colour(sid)
    # Autumn: the share of colour B, its pull to the lit leaves, the margin first, browning (the shrubs' keys).
    autumn_mix = [ck['b_share'], 0.5, 0.3, ex.get('browning', 0.35)]
    ratio = d.get('leaflet_ratio', 0.55)
    if a.get('leaf_kind', 'simple') in ('spray', 'simple'):
        # (A spray's near leaves are drawn at this width over their length - the maples' wider than long: 1.24, 1.14.)
        ratio = round(min(max(mean(a['leaf_wid']) / mean(a['leaf_len']), 0.08), 1.6), 3)
    layers = LEAF_TEX.get('layers', [])
    sprig = TEXTURES[sid]['tex'][0]
    if a.get('leaf_kind') == 'needles' and 0 <= sprig < len(layers):
        bl, bw = layers[sprig]['box_mm']
        ratio = round(min(max(bw / bl, 0.08), 0.95), 3)
    fold = d['fold']
    if a.get('leaf_kind') == 'compound':
        fold = max(fold, 0.25)
    elif d.get('lobes', 0) > 0:
        fold = max(fold, 0.2)
    vals = {'leaf_outline': d['outline'], 'leaflet_ratio': ratio, 'translucency': d['translucency'], 'gloss': d['gloss'],
            'flutter': d['flutter'], 'fold': fold, 'underside': d['underside'],
            'jitter': [max(d['jitter'][0], 0.15), max(d['jitter'][1], 0.06)],
            'bloom_shape': d.get('bloom_shape', 'none'), 'petals': d.get('petals', 5),
            'fruit_shape': d.get('fruit_shape', 'none'), 'fruits': d.get('fruits', 1),
            'catkin_shape': d.get('catkin_shape', 'none'), 'bark': ex.get('bark', [0.6, 0.1, 0.0]),
            'needle_density': d.get('needle_density', 1.0), 'evergreen': 1 if ex.get('evergreen') else 0,
            'lobes': d.get('lobes', 0), 'autumn_mix': autumn_mix}
    lines.append('; drawing (r3\\da_shrub_common.h shrub_prof)')
    for k in DRAW_ORDER:
        lines.append('%s = %s' % (k, fmt(vals[k])))
    lines.append('; the crown: the share of the crown field the grafted shoots fill, how closely the grafted branches')
    lines.append('; follow the stock card branches; the autumn\'s order leaf by leaf: the weight of the height (the top')
    lines.append('; first) and of the shade (+: the inner leaves first - birch, aspen, poplars, willows, a conifer\'s old')
    lines.append('; needles; -: the outer ones - maples, oaks, lime; Kitao et al. 2022)')
    lines.append('crown_fill = %s' % fmt(ex.get('crown_fill', 0.7)))
    lines.append('stock_card_follow = %s' % fmt(ex.get('follow', 0.6)))
    if ex.get('apex_cone'):
        lines.append('; apical dominance: the crown\'s top share narrowed to a cone from the apex (the branches shortening')
        lines.append('; toward the top), its top metres the leader and its short whorls')
        lines += ['apex_cone = %s' % fmt(ex['apex_cone']), 'apex_leader = %s' % fmt(ex.get('apex_leader', 1.25))]
    lines.append('autumn_order = %s' % fmt([float(x) for x in ex.get('autumn_order', (0.4, 0.0))]))
    # (A dead or charred tree's wood is all old: no young twig's skin on it.)
    twig_r = 0.0 if sid.endswith(('_dead', '_char')) else ex.get('twig_r', TWIG_R)
    lines.append('; the radius (m) under which its wood wears the young twig\'s skin, not its bark (a birch\'s white')
    lines.append('; bark on its limbs, its twigs dark)')
    lines.append('twig_r = %s' % fmt(float(twig_r)))
    # The calibrations against the photos: the bark tiles' stretch along the stem, the upper bark's value and chroma.
    for k in ('bark_stretch', 'bark_value_upper', 'bark_chroma_upper'):
        if k in ex:
            lines.append('%s = %s' % (k, fmt(float(ex[k]))))
    if ex.get('marcescent'):
        lines.append('; marcescence: the share of its leaves a young tree keeps withered through the winter, its last to')
        lines.append('; fall (an old tree a twelfth of it: the engine scales it by the tree\'s youth)')
        lines.append('marcescent = %s' % fmt(ex['marcescent']))
    lines += wood_lines(sid)
    lines += texture_lines(sid)
    sk = season_keys(base)
    lines.append('; season keys (day of year: value; colours sRGB)')
    for k in SEASON_ORDER:
        if k in sk:
            lines.append('%s = %s' % (k, fmt_keys(sk[k])))
    for k in COLOUR_ORDER:
        lines.append('%s = %s' % (k, fmt_keys(ck[k], colour=True)))
    return lines


def write_bytes(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def elements(sid):
    """The generator's leaf element (da_shrub_gen.cpp leaf_at, tufts_on) and spray element (sprays_on) of a species at
    its mean sizes, (length, width) m; the spray's None where the species has no sprays (a simple or compound species'
    far cards are the generator's own stretches of its shoots, tree_cards)."""
    a = SP[sid]['arch']
    kind = a.get('leaf_kind', 'simple')
    ll = mean(a['leaf_len']) if a.get('leaf_len') else 0.05
    lw = mean(a['leaf_wid']) if a.get('leaf_wid') else 0.03
    if kind == 'needles':
        return (a.get('leaf_spacing', 0.05) * 1.1, 2.0 * a.get('needle_len', 0.014)), None
    if kind == 'compound':
        n = max(round(a.get('leaflets', 1)), 1)
        leaf = (a.get('rachis', 0.05) + ll * (1.0 if n % 2 else 0.5),
                max(2.0 * ll * math.sin(math.radians(a.get('leaflet_angle', 60))), lw))
    else:
        leaf = (ll, lw)
    if kind != 'spray':
        return leaf, None
    splay = math.radians(mean(a.get('leaf_angle', [45, 60])))
    return leaf, (max(a.get('spray_len', 0.03), 0.03), 2.0 * ll * math.sin(splay) + lw)


FIT_CHECK = {}


def check_fits():
    """Each species' elements against the ones its layers' squares were fitted to (tree_layers.json element,
    card_element); where a leaf element differs (over 1 %) its fit is reckoned again from the layers' boxes."""
    for sid in SP:
        e = TREE_TEX.get(sid)
        if not isinstance(e, dict) or 'element' not in e or TEXTURES[sid]['source'] != 'tree':
            continue
        leaf, spray = elements(sid)
        el, ce = e.get('element') or {}, e.get('card_element') or {}
        off = lambda m, w: max(abs(m[0] / w['length'] - 1.0), abs(m[1] / w['width'] - 1.0)) if w else 0.0
        r = dict(leaf=leaf, leaf_wp3=(el.get('length'), el.get('width')), leaf_off=off(leaf, el),
                 card=spray, card_wp3=(ce.get('length'), ce.get('width')),
                 card_off=off(spray, ce) if spray else None)
        if r['leaf_off'] > 0.01:
            TEXTURES[sid]['fit'] = None
        if spray and r['card_off'] > 0.01:
            TEXTURES[sid]['card_fit'] = None
        FIT_CHECK[sid] = r


# (The engine's tables: da_veg_internal.h kTreeSpecies, a community's species[16].)
TREE_SPECIES_MAX = 80
COMMUNITY_SPECIES_MAX = 16


def main():
    align_to_botany()
    for sid in list(SP.keys()):
        TEXTURES[sid] = textures(sid)
    check_fits()
    burnt_variants()
    dead_variants()
    ids = list(SP.keys())
    assert len(ids) <= TREE_SPECIES_MAX, len(ids)
    for name, shares in COMMUNITIES.items():
        assert len(shares) <= COMMUNITY_SPECIES_MAX, name
        for s in shares:
            assert s in SP, (name, s)
    out = ['; Dead Air: Refined 1.6.0 - the upgraded level trees (docs/dead-air/TREES.md, TREE_DATA.md).',
           '; Written by tools/trees/make_trees.py from the ChEZ tree research of 08.10.2026 and the census of the stock',
           '; trees (tools/trees/data/tree_kinds.json); edit the script, not this file.', '']
    for n, sid in enumerate(ids, 1):
        out += species_section(n, sid) + ['']
    for n, (name, shares) in enumerate(COMMUNITIES.items(), 1):
        sp = ['%s:%g' % (k, v) for k, v in sorted(shares.items(), key=lambda kv: -kv[1]) if v > 0]
        out += ['[tree_community_%d]' % n, 'name = %s' % name, 'species = %s' % ', '.join(sp), '']
    out += ['[tree_policy]'] + ['%s = %s' % (k, fmt(v)) for k, v in POLICY] + ['']
    kinds, fams, decisions = kind_lines()
    out += ['[tree_kinds]'] + kinds + ['', '[tree_families]'] + fams + ['']
    text = '\r\n'.join(out)
    os.makedirs(os.path.dirname(OUT_LTX), exist_ok=True)
    write_bytes(OUT_LTX, text.encode('cp1251'))
    write_docs(ids, decisions, kinds, fams)
    print('species %d, communities %d, kinds %d (keys), families %d -> %s' % (len(ids), len(COMMUNITIES), len(kinds),
                                                                                len(fams), OUT_LTX))
    print('textures: %s; skeletons: %s' % (texture_source(), '%d .dtsk files' % len(_DTSK) if _DTSK else 'none yet'))


# ======================================================================================================
# The documents
# ======================================================================================================
def doc_tables(ids, decisions):
    """The data section: species, communities, kinds by class and species."""
    L = ['| # | id | species | height (m) | leaves | graft | flowers | fruit | catkins / cones | leaf out / full | '
         'flowers | ripe | autumn peak | bare | autumn order | bark zone (m) |',
         '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for n, sid in enumerate(ids, 1):
        s = SP[sid]
        a, d, ex, ph = s['arch'], s['draw'], s['extra'], PHENO[sid]
        leaf = a.get('leaf_kind', 'simple')
        if leaf in ('compound', 'spray'):
            leaf += ' (%d)' % a.get('leaflets', 0)
        cat = d.get('catkin_shape', 'none')
        if ex.get('cone'):
            cat = 'cones %g cm' % (ex['cone'][1] * 100)
        order = ex.get('autumn_order', (0.4, 0.0))
        order_t = 'top %g, shade %+g' % tuple(order)
        if ex.get('evergreen'):
            order_t = 'inner needles (%s)' % order_t
        zone = ex.get('bark_zone')
        L.append('| %d | %s | *%s* | %s | %s %s | %d | %s | %s | %s | %s / %s | %s-%s | %s | %s | %s | %s | %s |' % (
            n, sid, s['latin'], '-'.join('%g' % x for x in s['h']), leaf, d.get('outline', ''),
            ex.get('graft_levels', a.get('graft_levels', 3)), d.get('bloom_shape', 'none'), d.get('fruit_shape', 'none'),
            cat, ph.get('leaf_unfold', 'evergreen'), ph.get('leaf_full', '-'), ph.get('flower_start', '-'),
            ph.get('flower_end', '-'), ph.get('fruit_ripe', '-'), ph.get('autumn_peak', '-'), ph.get('leaf_fall_end', '-'),
            order_t + (', marcescent %g' % ex['marcescent'] if ex.get('marcescent') else ''),
            '%g-%g' % zone if zone else '-'))
    L += ['', '### Communities', '', '| community | species (share %) | levels whose generic trees take it |', '|---|---|---|']
    for name, shares in COMMUNITIES.items():
        lv = ', '.join(sorted(k for k, v in LEVEL_COMMUNITY.items() if v == name))
        if name.endswith('_brn'):
            lv = 'the burnt trees with foliage whose habitat is %s' % name[:-4]
        if name.endswith('_dead'):
            lv = 'the dead trees of %s' % name[:-5]
        if name.endswith('_thick'):
            lv = 'the generic trees of %s with a thick trunk' % name[:-6]
        L.append('| %s | %s | %s |' % (name, ', '.join('%s %g' % (k, v) for k, v in sorted(shares.items(), key=lambda kv: -kv[1])),
                                       lv or '- (for other levels)'))
    if BURNT_NOTES:
        L += ['', '### Burnt variants', '', '| variant | species | bark | scorch zone (m) | kinds (their mean height, m) |',
              '|---|---|---|---|---|']
        for vid, b in BURNT_NOTES.items():
            L.append('| %s | %s | %s | %s | %s (%g) |' % (
                vid, b['base'], 'charred base' if b['zone'] else 'charred all the way up',
                '%g-%g' % b['zone'] if b['zone'] else '-', ', '.join(b['kinds']), b['top']))
    if DEAD_NOTES:
        L += ['', '### Dead variants', '', '| variant | species | deadwood_bark -> deadwood_bare (m) | dead kinds | '
              'their placements |', '|---|---|---|---|---|']
        for vid, b in sorted(DEAD_NOTES.items()):
            L.append('| %s | %s | %g-%g | %d | %d |' % (vid, b['base'], b['zone'][0], b['zone'][1], len(b['kinds']),
                                                       b['placements']))
    # The leaf elements against the layers' (tree_leaf_botany.py) and the barks.
    L += ['', '### Leaf elements and barks', '',
          'The generator\'s elements at the species\' mean sizes against the ones the layers\' squares were fitted to '
          '(`tree_layers.json` `element`, `card_element`; m); `card`: the spray species\' sprays. The bark: its layers, '
          'the tile scale, the contrast (the upper layer\'s after a slash: '
          '`bark_scale_upper`, `bark_contrast_upper`). Dead wood wears its layers as they are (the dead and charred '
          'variants\' own weathered barks).', '',
          '| species | taken from the botany | leaf element | layers\' | spray | layers\' card | bark (scale, contrast) |',
          '|---|---|---|---|---|---|---|']
    names = bark_names()
    nm = lambda i: names[i] if 0 <= i < len(names) else '-'
    for sid in ids:
        if SP[sid].get('base'):
            continue
        f = FIT_CHECK.get(sid)
        t = TEXTURES[sid]
        took = ', '.join(sorted(BOTANY.get(sid, {}))) or '-'
        el = lambda v: '%.3f x %.3f' % tuple(v) if v and None not in v else '-'
        zone = SP[sid]['extra'].get('bark_zone')
        bark = nm(t['bark']) + (' / ' + nm(t['bark_upper']) if zone and t['bark_upper'] >= 0 and
                                t['bark_upper'] != t['bark'] else '')
        up = upper_bark_keys(sid, t['bark_upper']) if zone and t['bark_upper'] != t['bark'] else None
        L.append('| %s | %s | %s | %s | %s | %s | %s (%g, %g%s) |' % (
            sid, took, el(f['leaf']) if f else '-', el(f['leaf_wp3']) if f else '-',
            el(f['card']) if f and f['card'] else '-',
            el(f['card_wp3']) if f else '-', bark, layer_scale(t.get('scale_layer', t['bark'])) or
            SP[sid]['extra'].get('bark_scale', 2.5), bark_contrast(sid), ' / %g, %g' % up if up else ''))
    # Kinds by class and pick.
    by = {}
    for key, (value, sp, comm, why) in decisions.items():
        k = KINDS['kinds'][key]
        cls = k['class']
        pick = sp or ('community ' + comm if comm else k['stock_reason'] or 'stock')
        e = by.setdefault((cls, pick), [0, 0, 0, 0])
        e[0] += 1
        e[1] += len(k['members'])
        e[2] += k['placements']
        e[3] += 1 if 'dead' in value.split(', ') else 0
    L += ['', '### Kinds', '', '| class | species or community | kinds | keys | placements | of them dead kinds |',
          '|---|---|---|---|---|---|']
    for (cls, pick), (nk, nm, npl, nd) in sorted(by.items(), key=lambda kv: (kv[0][0], -kv[1][2])):
        L.append('| %s | %s | %d | %d | %d | %d |' % (cls, pick, nk, nm, npl, nd))
    # The kinds that stay stock with cards: the season looks of exactly these card textures stay in the game.
    L += ['', '### Stock kinds with cards', '', 'Their card textures keep their seasonal looks (the textures of every upgraded '
          'kind are drawn by the module and need none).', '',
          '| kind | category | cards (stock look) | placements |', '|---|---|---|---|']
    for key, k in sorted(KINDS['kinds'].items()):
        if k['class'] == 'STOCK' and k['cards']:
            L.append('| %s | %s | %s | %d |' % (key, k['category'], ', '.join(
                '%s (%s)' % (c, v) for c, v in sorted(k['cards'].items())), k['placements']))
    return L


def write_docs(ids, decisions, kinds, fams):
    meta = KINDS['meta']
    cnt = {}
    for key, k in KINDS['kinds'].items():
        e = cnt.setdefault(k['class'], [0, 0, 0])
        e[0] += 1
        e[1] += len(k['members'])
        e[2] += k['placements']
    L = ['# Tree data (generated)', '',
         'Written by `tools/trees/make_trees.py` into `configs/dead_air_x64_trees.ltx` (docs/dead-air/TREES.md); the '
         'census of the stock trees by `tools/trees/census_trees.py` into `tools/trees/data/tree_kinds.json`. Edit the '
         'scripts, never the ltx.', '',
         '## Census', '',
         '- Kinds: %s; %d keys in `[tree_kinds]`, %d signatures in `[tree_families]`.' % (
             ', '.join('%s %d (%d keys, %d placements)' % (c, v[0], v[1], v[2]) for c, v in sorted(cnt.items())),
             len(kinds), len(fams)),
         '- TREE: every standing tree (alive, dead, broken snags, the anomaly-sculpted and the burnt ones) on its stock '
         'skeleton; YOUNG: the oversize stock bushes the shrubs keep stock (tops over `[shrub_policy] max_top`), grown '
         'whole as young trees (`skel=p<kind>`); STOCK: the lying and dead-wood models - windfalls, logs, stumps, debris '
         '- leafy or not (the season looks of their cards stay: see the last table).',
         '- Anomalies (the sculpted byaka and anomaly trunks): their species or community as any other model\'s (the '
         'cards\' species, else the habitat\'s community); dead when their main cards are bare or dead or they have none, '
         'alive with leafy cards. The skeletons keep the stock triangles where the rings miss the sculpting, their bark '
         'on the stock uv.',
         '- Burnt trees: with foliage (leafy cards) alive and resprouting, `community=<habitat>_brn` of the variants '
         '`<id>_brn` - the species with a charred base: `bark_tex` the char layer, `bark_tex_upper` the species\' main '
         'bark (its upper layer where its own zone ends by 3.5 m), `bark_zone` the scorch line (0.12 of the tree\'s '
         'height, 1-3 m) blended over 0.6 m; bare ones dead and charred all the way up, `species=<id>_char` (the '
         'commonest species of their habitat, `bark_tex_upper = -1`). Char: %s.' % (
             char_layer()[2]),
         '- Dead trees (every `dead` kind but the burnt snags: the dead and broken snags, the bare anomalies, the Red '
         'Forest\'s pines): the variants `<id>_dead` of the species their pick can give them (`species=<id>_dead`, '
         '`community=<community>_dead` - conifers for a conifer kind, broadleaves for the others, at the community\'s '
         'shares) wear the dead barks: `deadwood_bark` (a dead pine\'s bleached plates, a third of them fallen) up the '
         'trunk, `deadwood_bare` (weathered silver-grey wood) above the species\' own zone, 3-6 m at the least. %s.' % (
             '%d variants, %d dead communities' % (len(DEAD_NOTES), sum(1 for c in COMMUNITIES if c.endswith('_dead')))),
         '- Keys: %s' % meta['key_definition'] + '.',
         '- Parity (the census code on the raw levels): %s; %s; %s; %s; the engine\'s own shrub census in a game log: '
         '%s.' % (meta['parity']['shrub_kinds'], meta['parity']['shrub_families'], meta['parity']['tree_keys'],
                  meta['parity']['sidecar_parts'], meta['parity'].get('live_log', 'not checked')),
         '- Skeleton ids (`skel=`, the file `gamedata/meshes/da/trees/<skel>.dtsk`): the skeleton builder\'s index '
         '`tools/trees/out/skel_index.json` where it lists the kind (it gives the other card layouts of one trunk mesh '
         'skeletons of their own: %d kinds), else the census\'s: %s; %d distinct among the TREE kinds.' % (
             sum(1 for k in KINDS['kinds'].values() if k['class'] != 'STOCK' and k['key'] in SKEL_INDEX and
                 SKEL_INDEX[k['key']] != k['skel']),
             meta['skeleton_rule'], len({skel_of(k) for k in KINDS['kinds'].values() if k['class'] == 'TREE'})),
         '- Species of a kind: its main cards (`species=`; a model in two species\' cards takes the larger card area): '
         'pine `pin_syl`, spruce `pic_abi`, birch `bet_pen` (`bet_pub` where 60 % of its placements stand in the swamp '
         'levels), oak `que_rob`, alder `aln_glu`, cherry `t_pru_cer`, walnut `jug_reg`, willow `sal_alb` (tall, 18 m '
         'and more) or `sal_fra`, poplar `pop_nig` (a column: span under 0.4 of the height), `pop_tre` (models under '
         '12 m: the aspen groves), `pop_alb` (tall, on wet ground) or `pop_can` (tall rows). The generic broadleaf '
         'cards and the bare dead trees take `community=` of the levels most of their placements stand in '
         '(LEVEL_COMMUNITY); the engine draws each placement\'s species from it (the conifer flag picks conifers, '
         'the rest broadleaves, at most `kinds_per_level_species` templates of a kind on a level).',
         '- Flags: `dead` - no live foliage, the grafted twigs bare and grey, the dead barks (a dead pine grey, '
         'without needles); `red` - the Red Forest\'s stock: its tall red-carded pines (12 m and more) stand `dead, '
         'red`, the pines killed in 1986, and keep their dead needles in the species\' `c_dead` (rust-red: the ginger '
         'forest) where a plain dead pine has none; the young red pines and spruces are the replanted generation, alive; '
         'the generic red cards take the `red_forest` community (the birch, aspen and alder regrowth); `conifer` - from '
         'a community the kind takes conifers only, the others broadleaves only.',
         '- Textures: %s.' % texture_source(), '',
         '## Keys beyond the shrubs\' (for the engine)', '',
         '| key | where | meaning |', '|---|---|---|',
         '| `graft_levels` | species | grafted levels on a stock skeleton (l1..l3: branches, twigs, current shoots); the '
         'young trees grow `levels` from the root |',
         '| `crown_fill` | species | share of the crown field the grafted shoots fill |',
         '| `stock_card_follow` | species | how closely the grafted level-1 branches follow the stock card branch axes |',
         '| `bark_tex_upper`, `bark_zone`, `bark_zone_mode` | species | the upper bark layer and the heights (m above the '
         'root) over which it takes over from `bark_tex` (`height`; `bark_tex_upper = -1`: one bark all the way up) |',
         '| `cone_share`, `cone_size`, `cone_droop` | conifers | cones on the young growth\'s tips; their look and season '
         'are the fruit keys (`fruit_shape = cone`) |',
         '| `autumn_order` | species | the autumn\'s order leaf by leaf: the weight of the height (the top first) and '
         'of the shade (+ the inner leaves first: birch, aspen, poplars, willows, a conifer\'s old needles; - the outer '
         'ones: maples, oaks, lime) |',
         '| `marcescent` | species | the share of its leaves a young tree keeps withered through the winter, its last to '
         'fall (pedunculate oak 0.6, red oak 0.5, hornbeam 0.4); the engine scales it by the tree\'s youth y, '
         '`lerp(0.08, 1, y)` |',
         '| `young_h0`, `young_h1`, `young_lead` | `[tree_policy]` | a tree\'s youth y by its height (1 up to `young_h0` '
         '= 4 m, 0 from `young_h1` = 10 m; 1 for a YOUNG kind and a `sapling` kind): its spring up to `young_lead` = '
         '8 days ahead of the old trees\', its marcescence, 1 - y of its flowers, fruit and cones |',
         '| `sapling` | `[tree_kinds]` flag | the stock\'s sapling models (the inventory\'s flag): young trees whatever '
         'their height |',
         '', 'The trees write no `max_leaves`: the generator sizes a tree\'s leaves by its own crown (the species\' LAI '
         'over the crown field, on the share `crown_fill` fills). The leaf elements are the texture botany\'s '
         '(`tools/shrubs/textures/tree_leaf_botany.py`: the sizes the layers are drawn at, their squares fitted to '
         'them); `bark_contrast` is the bark type\'s (`TREE_BARK_CONTRAST`: the plated orchard and floodplain barks, '
         'the oak, the pine\'s plates and the black poplar brought to their photos\' fissure contrast, the spruce at '
         'half it, the rest 1), `bark_scale` the tile scale of the layer on the '
         'most wood.', '',
         '`[tree_kinds]` carries no flags but `dead`, `red`, `conifer` and `sapling`.', '',
         '## Inputs a re-run picks up', '',
         '- `tools/shrubs/textures/out/tree_layers.json` (the tree layers in the shrubs\' arrays): `species` - per '
         'species id the layers `leaf_a`, `leaf_b`, `card`, `leaflet`, `needles`, `bark`, `bark_upper` with '
         '`leaf_fit`, `card_fit`, `leaf_hull` (computed from the layers\' `box_mm` as the shrubs\' are where an entry '
         'has none); `bark_types` - the `char` layer of the burnt variants and every bark layer\'s tile scale (the '
         'species\' `bark_scale`: its lower bark\'s, a burnt variant\'s main bark\'s, a snag\'s char\'s); the far '
         'tiers\' bark from `texture_stats.json`. A tree species registered in the shrubs\' manifests under its own id '
         'counts as well; without either a species borrows the nearest shrub layers (`FALLBACK_LEAF`, '
         '`FALLBACK_BARK`, the bark tinted toward its photos) and the char is the darkest shrub bark tinted to '
         '#1a1714. `bark_types` gives the dead barks too (`deadwood_bark`, `deadwood_bare`: the dead variants).',
         '- `tools/shrubs/textures/tree_leaf_botany.py`: the leaf elements (sizes, sprays, compound leaves, tufts) the '
         'species take.',
         '- `tools/trees/out/skel_index.json` (the skeleton builder): kind -> skeleton; `gamedata/meshes/da/trees/*.dtsk`.',
         '- `tools/trees/data/*.json`: re-run `census_trees.py` only when the levels or the inventory change.', '',
         '## Species, communities, kinds', '']
    L += doc_tables(ids, decisions)
    text = '\r\n'.join(L) + '\r\n'
    write_bytes(OUT_MD, text.encode('utf-8'))
    update_trees_md(ids, decisions)


BEGIN, END = '<!-- tree data: tools/trees/make_trees.py -->', '<!-- end of tree data -->'


def update_trees_md(ids, decisions):
    """The short data section of TREES.md between its markers (appended the first time), when that document exists."""
    if not os.path.exists(TREES_MD):
        return
    with open(TREES_MD, 'rb') as f:
        raw = f.read()
    text = raw.decode('utf-8')
    nl = '\r\n' if '\r\n' in text else '\n'
    cnt = {}
    for key, k in KINDS['kinds'].items():
        e = cnt.setdefault(k['class'], [0, 0])
        e[0] += 1
        e[1] += k['placements']
    sec = [BEGIN, '## Data', '',
           '`configs/dead_air_x64_trees.ltx` is written by `tools/trees/make_trees.py` (never edit the ltx) from the census '
           '`tools/trees/data/tree_kinds.json` (`tools/trees/census_trees.py`: the engine\'s keys on the raw levels, '
           'parity with every `[shrub_kinds]` key and `[shrub_families]` row) and the species research '
           '(`tools/trees/data/tree_photo_colours.json`), the leaf elements of the texture botany '
           '(`tools/shrubs/textures/tree_leaf_botany.py`) and the texture layers (`tools/shrubs/textures/out/'
           'tree_layers.json`). %d species (`[tree_species_N]`: %d of the research, %d burnt variants `_brn`/`_char`, '
           '%d dead variants `_dead`), %d communities, kinds: %s. Tables, rules and the keys beyond the shrubs\': '
           '[TREE_DATA.md](TREE_DATA.md).' % (
               len(ids), sum(1 for s in ids if not SP[s].get('base')), len(BURNT_NOTES), len(DEAD_NOTES),
               len(COMMUNITIES), ', '.join('%s %d (%d placements)' % (c, v[0], v[1]) for c, v in sorted(cnt.items()))),
           END]
    block = nl.join(sec)
    if BEGIN in text and END in text:
        text = text[:text.index(BEGIN)] + block + text[text.index(END) + len(END):]
    else:
        text = text.rstrip() + nl + nl + block + nl
    write_bytes(TREES_MD, text.encode('utf-8'))


if __name__ == '__main__':
    main()
