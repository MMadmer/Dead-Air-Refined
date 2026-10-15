# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Write the procedural shrubs' data (docs/dead-air/SHRUBS.md): configs/dead_air_x64_shrubs.ltx and
docs/dead-air/SHRUB_DATA.md.

Sources (tools/shrubs/data, the research of 07.10.2026):
  shrubs_flora.json        48 ChEZ shrub species: traits, phenology (day of year), habitats with Didukh 2023 constancy,
                           the ten shrub communities of the zone (SHRUBS.md of the research)
  shrub_photo_colours.json dated iNaturalist photos of Ukraine/Belarus/Poland measured per month bucket and colour role
  shrub_kinds.json         the stock bush models of the 22 levels: content keys, class, look, clumps, signatures

Each species gets its branching architecture (da_shrub_gen.h keys, written here from its traits), its drawing keys,
its season keys built from its phenology and its colours from its photos. Every stock bush model of the replaced
classes is listed by its content key, every compile of it by its signature.

    python tools/shrubs/make_shrubs.py
"""
import colorsys
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
DATA = os.path.join(HERE, 'data')
OUT_LTX = os.path.join(REPO, 'packaging', 'dead-air-x64', 'compatibility', 'gamedata', 'configs', 'dead_air_x64_shrubs.ltx')
OUT_MD = os.path.join(REPO, 'docs', 'dead-air', 'SHRUB_DATA.md')

FLORA = json.load(open(os.path.join(DATA, 'shrubs_flora.json'), encoding='utf-8'))
PHOTOS = json.load(open(os.path.join(DATA, 'shrub_photo_colours.json'), encoding='utf-8'))
KINDS = json.load(open(os.path.join(DATA, 'shrub_kinds.json'), encoding='utf-8'))
FLORA_BY_ID = {s['id']: s for s in FLORA['species']}
# Fly honeysuckle (community C of the research) has no card: its phenology after the Chernihiv shrub calendars.
FLORA_BY_ID.setdefault('lon_xyl', {'latin': 'Lonicera xylosteum L.', 'phenology': dict(
    bud_burst=95, leaf_unfold=102, leaf_full=125, flower_start=130, flower_end=150, fruit_ripe=200, autumn_start=265,
    autumn_peak=285, leaf_fall_start=280, leaf_fall_end=300)})

# The engine's species table (r3\da_shrub_common.h SHRUB_SPECIES).
MAX_SPECIES = 48

# ======================================================================================================
# Architecture: archetypes and the species' own keys
# ======================================================================================================


def level(density, start, end, angle, phyllo, len_, up, outward, wobble, radius, seg, min_len, per_node=1, jitter=0.3):
    return dict(density=density, start=start, end=end, angle=angle, phyllo=phyllo, per_node=per_node, len=len_,
                len_jitter=jitter, up=up, outward=outward, wobble=wobble, radius=radius, seg_len=seg, min_len=min_len)


# The woody framework is the archetype's rules (stems, two or three levels of branches); the leafy shoots that fill
# the crown's lit shell grow from it toward the light (fill_*: their length, the shell's depth, the clumps, their
# turn upward, the stems' bare foot) - the framework only has to reach within a shoot's length of the shell.
#
# Basitonic multi-stem shrubs: stems from the root crown fanning into a vase, laterals on their upper part, the
# leafy shoots filling a broad rounded crown (hazel, alder buckthorn, guelder rose, elders, lilac).
VASE = dict(envelope='vase', env_base=0.32, stems=[5, 12], root_r=0.25, stem_spread=[6, 38], stem_len=[0.8, 1.0],
            stem_up=0.05, stem_arch=0.15, stem_wobble=0.12, stem_radius=0.028, stem_seg=0.16, leader=0, whorl_step=0,
            levels=2,
            l1=level(2.2, 0.12, 0.97, [55, 40], 137.5, [0.42, 0.18], 0.05, 0.45, 0.2, 0.5, 0.15, 0.12),
            l2=level(4.0, 0.15, 1.0, [55, 45], 137.5, [0.48, 0.25], -0.04, 0.25, 0.25, 0.6, 0.08, 0.05),
            shoot_len=0.35, lai=3.6, fill_len=[0.12, 0.45], fill_shell=0.6, fill_clump=0.55, fill_up=0.25, fill_from=0.1,
            spur_share=0.0, spur_leaves=[2, 4], leaf_spacing=0.045, leaf_per_node=1, leaf_phyllo=137.5,
            leaf_angle=[45, 70], leaf_flat=0.5, leaf_droop=0.12, petiole=0.01, tip_shrink=0.5)
# A dense dome of many spreading stems, foliage to the ground (grey and eared willows).
DOME = dict(VASE, envelope='dome', env_base=0.6, stems=[12, 26], root_r=0.45, stem_spread=[10, 45],
            stem_len=[0.75, 1.0], stem_up=0.35, stem_arch=0.35, stem_radius=0.025,
            l1=level(2.6, 0.25, 0.97, [50, 40], 137.5, [0.4, 0.16], 0.12, 0.4, 0.22, 0.5, 0.14, 0.1),
            l2=level(5.0, 0.15, 1.0, [50, 40], 137.5, [0.5, 0.28], 0.08, 0.2, 0.25, 0.6, 0.07, 0.04),
            lai=4.0, fill_len=[0.15, 0.45], fill_shell=0.65, fill_clump=0.45, fill_up=0.35, fill_from=0.12)
# Long straight rods fanning from the base, leafy along their length, few short laterals (osier, sharp-leaved and
# almond willows, serviceberry, cinnamon rose).
RODS = dict(VASE, envelope='vase', env_base=0.42, stems=[14, 30], root_r=0.3, stem_spread=[5, 32],
            stem_len=[0.72, 1.0], stem_up=0.12, stem_arch=0.12, stem_wobble=0.08, stem_radius=0.016, stem_seg=0.2, levels=2,
            l1=level(1.4, 0.2, 0.95, [32, 24], 137.5, [0.3, 0.12], 0.3, 0.2, 0.15, 0.55, 0.15, 0.1),
            l2=level(2.5, 0.3, 1.0, [35, 25], 137.5, [0.4, 0.2], 0.2, 0.05, 0.2, 0.6, 0.08, 0.05),
            shoot_len=2.0, lai=3.0, fill_len=[0.12, 0.4], fill_shell=0.8, fill_clump=0.4, fill_up=0.6, fill_from=0.18,
            leaf_flat=0.35, leaf_angle=[30, 50])
# Arching canes: long stems climbing then bending over under their own weight (dog rose, raspberry).
CANES = dict(VASE, envelope='fountain', env_base=0.3, stems=[5, 12], root_r=0.22, stem_spread=[10, 42],
             stem_len=[0.95, 1.15], stem_up=0.32, stem_arch=1.15, stem_wobble=0.1, stem_radius=0.011, stem_seg=0.14,
             levels=2,
             l1=level(4.5, 0.35, 1.0, [52, 45], 140, [0.24, 0.14], 0.3, 0.25, 0.2, 0.6, 0.08, 0.05),
             l2=level(6.0, 0.2, 1.0, [50, 40], 140, [0.4, 0.25], 0.2, 0.1, 0.3, 0.7, 0.05, 0.03),
             shoot_len=0.35, lai=3.2, spur_leaves=[2, 3], fill_len=[0.08, 0.3], fill_shell=0.7, fill_clump=0.5,
             fill_up=0.3, fill_from=0.2)
# A small tree on one to three stems: a clear foot, an oval crown (rowan, bird cherry, apple, pear, box elder).
TREELET = dict(VASE, envelope='ellipsoid', env_base=0.12, stems=[1, 3], root_r=0.08, stem_spread=[2, 14],
               stem_len=[1.0, 1.0], stem_up=0.3, stem_arch=0.0, stem_wobble=0.08, stem_radius=0.05, stem_seg=0.2,
               levels=3,
               l1=level(2.4, 0.22, 0.95, [52, 38], 137.5, [0.45, 0.16], 0.12, 0.3, 0.18, 0.55, 0.18, 0.15),
               l2=level(3.6, 0.15, 1.0, [50, 40], 137.5, [0.48, 0.26], 0.0, 0.15, 0.24, 0.6, 0.09, 0.05),
               l3=level(4.0, 0.25, 1.0, [48, 40], 137.5, [0.4, 0.25], 0.0, 0.05, 0.3, 0.6, 0.06, 0.05),
               shoot_len=0.3, lai=3.4, fill_len=[0.12, 0.5], fill_shell=0.6, fill_clump=0.55, fill_up=0.3, fill_from=0.25)
# Green switches: a broom of thin green twigs with small leaves (Scotch and Russian broom).
BROOM = dict(RODS, envelope='vase', env_base=0.35, stems=[10, 24], stem_spread=[6, 40], stem_up=0.2, stem_arch=0.25,
             stem_radius=0.006, stem_seg=0.12, levels=2,
             l1=level(5.0, 0.25, 0.95, [35, 26], 137.5, [0.38, 0.16], 0.3, 0.2, 0.2, 0.7, 0.07, 0.04),
             l2=level(6.0, 0.2, 1.0, [32, 26], 137.5, [0.42, 0.22], 0.25, 0.1, 0.25, 0.7, 0.05, 0.03),
             shoot_len=0.6, lai=1.8, spur_leaves=[2, 3], fill_len=[0.08, 0.3], fill_shell=0.8, fill_clump=0.35,
             fill_up=0.7, fill_from=0.15)
# Juniper: a narrow column of many short upright shoots in needles.
JUNIPER = dict(VASE, envelope='column', env_base=0.6, stems=[1, 4], root_r=0.1, stem_spread=[2, 12],
               stem_len=[0.85, 1.0], stem_up=0.4, stem_arch=0.0, stem_radius=0.03, levels=2,
               l1=level(6.0, 0.06, 0.97, [35, 25], 137.5, [0.32, 0.12], 0.35, 0.1, 0.15, 0.5, 0.08, 0.05),
               l2=level(8.0, 0.1, 1.0, [35, 28], 137.5, [0.45, 0.3], 0.3, 0.05, 0.2, 0.6, 0.05, 0.03),
               leaf_kind='needles', shoot_len=0.45, lai=3.4, spur_leaves=[0, 0], leaf_spacing=0.05, needle_len=0.014,
               tip_shrink=0.8, fill_len=[0.05, 0.2], fill_shell=0.75, fill_clump=0.3, fill_up=0.6, fill_from=0.0)
# A low tangle of trailing canes (dewberry).
LOW = dict(CANES, envelope='cushion', env_base=0.6, stems=[6, 14], root_r=0.3, stem_spread=[35, 75], stem_len=[1.1, 1.5],
           stem_up=0.15, stem_arch=1.6, stem_radius=0.006, levels=2, lai=2.6, fill_len=[0.06, 0.25], fill_shell=0.9,
           fill_clump=0.4, fill_up=0.4, fill_from=0.1)


def arch(base, **kw):
    """An architecture: the archetype with the species' own keys (l1/l2/l3 replace their level or patch it)."""
    a = dict(base)
    for lv in ('l1', 'l2', 'l3'):
        a[lv] = dict(base[lv]) if lv in base else None
    for k, v in kw.items():
        if k in ('l1', 'l2', 'l3') and isinstance(v, dict):
            a[k] = dict(a[k] or {}, **v)
        else:
            a[k] = v
    return a


def cm(a, b=None):
    return [round(a / 100.0, 4), round((b if b is not None else a) / 100.0, 4)]


def spray(n, spacing, per=1):
    """Leaves drawn as sprays of n at the species' own node spacing (per: leaves per node)."""
    return dict(leaf_kind='spray', leaflets=n, leaf_spacing=spacing, leaf_per_node=per,
                spray_len=round(spacing * n / per, 3))


# The species. h: the real plant's height range (m), w: crown width as a share of the height, classes: the stock
# classes it stands in for, draw: drawing keys (outline, gloss, translucency, flutter, fold, underside light and
# blue shift, value and hue jitter, bloom/fruit/catkin shapes, a leaflet's width over its length), extra: season
# details (marcescent, evergreen, persistence).
SP = {}


def species(id_, h, w, classes, a, draw, **extra):
    SP[id_] = dict(id=id_, h=h, w=w, classes=classes, arch=a, draw=draw, extra=extra)


B, G, T, C = 'BUSH', 'BUSH_GROUP', 'THICKET', 'CONIFER_BUSH'
SHRUB = [B, G, T]

# ---- Willows -------------------------------------------------------------------------------------------------
# The willows' many small and narrow leaves crowd their shoots: sprays (leaflets = leaves per spray_len of shoot).
species('sal_cin', [2, 5], [1.1, 1.5], SHRUB,
        arch(DOME, root_r=0.65, **spray(4, 0.025), leaf_len=cm(4, 9), leaf_wid=cm(1.5, 3), petiole=0.006,
             leaf_angle=[35, 55], leaf_flat=0.35, catkin_share=0.2, catkin_len=0.03,
             catkin_droop=-0.7, bloom_place='axil', bloom_share=0.05, bloom_size=0.04, fruit_size=0.04),
        dict(outline='lanceolate', gloss=0.05, translucency=0.55, flutter=0.16, fold=0.18, underside=[1.45, 0.22],
             jitter=[0.08, 0.03], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'),
        catkin_winter=True)
species('sal_aur', [0.8, 2.5], [1.0, 1.4], SHRUB,
        arch(DOME, stems=[10, 22], root_r=0.3, **spray(5, 0.018), leaf_len=cm(2, 5),
             leaf_wid=cm(1, 3), petiole=0.006, leaf_angle=[40, 60], leaf_flat=0.4,
             catkin_share=0.2, catkin_len=0.025, catkin_droop=-0.7, bloom_place='axil', bloom_share=0.04,
             bloom_size=0.03, fruit_size=0.03),
        dict(outline='ovate', gloss=0.04, translucency=0.5, flutter=0.15, fold=0.2, underside=[1.4, 0.15],
             jitter=[0.08, 0.03], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'),
        catkin_winter=True)
species('sal_pen', [2.5, 6], [0.6, 0.9], SHRUB,
        arch(TREELET, stems=[3, 6], root_r=0.2, stem_spread=[6, 28], **spray(3, 0.035),
             leaf_len=cm(5, 12), leaf_wid=cm(2, 4), petiole=0.008, leaf_flat=0.55,
             catkin_share=0.15, catkin_len=0.04, catkin_droop=-0.3, bloom_place='axil', bloom_share=0.04,
             bloom_size=0.05, fruit_size=0.05),
        dict(outline='lanceolate', gloss=0.35, translucency=0.5, flutter=0.12, fold=0.15, underside=[1.25, 0.1],
             jitter=[0.06, 0.02], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'))
species('sal_cap', [2.5, 6], [0.7, 1.1], SHRUB,
        arch(TREELET, stems=[3, 6], root_r=0.2, stem_spread=[6, 30], l1=dict(density=2.2, angle=[50, 38], outward=0.35),
             **spray(3, 0.035), leaf_len=cm(5, 11), leaf_wid=cm(2.5, 6), petiole=0.012, leaf_flat=0.55, lai=3.4,
             catkin_share=0.22, catkin_len=0.03, catkin_droop=-0.8, bloom_place='axil', bloom_share=0.05,
             bloom_size=0.06, fruit_size=0.06),
        dict(outline='round', gloss=0.12, translucency=0.5, flutter=0.14, fold=0.12, underside=[1.55, 0.2],
             jitter=[0.07, 0.03], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'),
        catkin_winter=True)
species('sal_acu', [2, 6], [0.7, 1.0], SHRUB,
        arch(RODS, **spray(5, 0.02), leaf_len=cm(6, 13), leaf_wid=cm(0.7, 1.2),
             petiole=0.01, lai=2.8, catkin_share=0.2, catkin_len=0.03, catkin_droop=-0.8,
             bloom_place='axil', bloom_share=0.04, bloom_size=0.03, fruit_size=0.03),
        dict(outline='narrow', gloss=0.3, translucency=0.55, flutter=0.2, fold=0.1, underside=[1.35, 0.3],
             jitter=[0.06, 0.02], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'),
        catkin_winter=True)
species('sal_tri', [2, 6], [0.8, 1.1], SHRUB,
        arch(RODS, stems=[10, 22], stem_spread=[6, 40], stem_arch=0.2, l1=dict(density=2.2, len=[0.35, 0.15]),
             **spray(4, 0.025), leaf_len=cm(4, 10), leaf_wid=cm(1, 2.5), petiole=0.012,
             leaf_angle=[35, 55], catkin_share=0.15, catkin_len=0.05, catkin_droop=-0.3,
             bloom_place='axil', bloom_share=0.04, bloom_size=0.03, fruit_size=0.03),
        dict(outline='lanceolate', gloss=0.08, translucency=0.55, flutter=0.18, fold=0.12, underside=[1.25, 0.12],
             jitter=[0.07, 0.03], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'))
species('sal_vim', [3, 6], [0.6, 0.9], SHRUB,
        arch(RODS, stems=[14, 30], sag=0.3, **spray(4, 0.03), leaf_len=cm(10, 18),
             leaf_wid=cm(0.6, 1.4), petiole=0.006, leaf_droop=0.4, lai=2.2, catkin_share=0.2,
             catkin_len=0.03, catkin_droop=-0.8, bloom_place='axil', bloom_share=0.04, bloom_size=0.04,
             fruit_size=0.04),
        dict(outline='narrow', gloss=0.05, translucency=0.5, flutter=0.24, fold=0.15, underside=[2.2, 0.18],
             jitter=[0.06, 0.02], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'),
        catkin_winter=True)
species('sal_ros', [0.4, 1.0], [1.0, 1.6], SHRUB,
        arch(RODS, stems=[16, 32], root_r=0.35, stem_spread=[8, 45], stem_radius=0.005, stem_arch=0.3,
             **spray(5, 0.014), leaf_len=cm(2, 5), leaf_wid=cm(0.3, 0.8), petiole=0.002,
             lai=2.4, fill_len=[0.08, 0.25], catkin_share=0.2, catkin_len=0.017, catkin_droop=-0.8,
             bloom_place='axil', bloom_share=0.04, bloom_size=0.02, fruit_size=0.02),
        dict(outline='narrow', gloss=0.04, translucency=0.5, flutter=0.15, fold=0.15, underside=[1.5, 0.2],
             jitter=[0.06, 0.02], bloom_shape='none', fruit_shape='cotton', fruits=1, catkin_shape='upright'))

# ---- Forest and edge shrubs ----------------------------------------------------------------------------------
species('fra_aln', [1.5, 4], [0.8, 1.1], SHRUB,
        arch(VASE, stems=[3, 9], stem_spread=[6, 36], stem_arch=0.25, l1=dict(phyllo=160, angle=[62, 50], density=1.9),
             **spray(3, 0.035), leaf_len=cm(4, 7), leaf_wid=cm(2.5, 4.5), petiole=0.01, lai=3.0, fill_clump=0.65,
             fill_up=0.2, bloom_place='axil', bloom_share=0.15, bloom_size=0.012, fruit_size=0.025),
        dict(outline='elliptic', gloss=0.3, translucency=0.55, flutter=0.1, fold=0.12, underside=[1.2, 0.04],
             jitter=[0.07, 0.03], bloom_shape='corymb', petals=5, fruit_shape='berry', fruits=4))
species('cor_ave', [2, 6], [0.85, 1.15], SHRUB,
        arch(VASE, stems=[7, 16], stem_spread=[5, 38], stem_arch=0.22, stem_up=0.03, env_base=0.36, sag=0.4,
             l1=dict(phyllo=180, angle=[60, 45], outward=0.5, density=2.0, up=0.0), l2=dict(phyllo=180, up=-0.1),
             leaf_len=cm(7, 12), leaf_wid=cm(6, 10), petiole=0.012, leaf_spacing=0.05, leaf_phyllo=180, lai=4.0,
             leaf_flat=0.62, tip_shrink=0.45, fill_len=[0.15, 0.5], fill_up=0.15, fill_from=0.2, catkin_share=0.28,
             catkin_len=0.055, catkin_droop=1.0, bloom_place='tip', bloom_share=0.22, bloom_size=0.008,
             fruit_size=0.035),
        dict(outline='round', gloss=0.05, translucency=0.6, flutter=0.1, fold=0.1, underside=[1.2, 0.03],
             jitter=[0.07, 0.03], bloom_shape='none', fruit_shape='nut', fruits=4, catkin_shape='hanging'),
        catkin_winter=True, autumn_margin=True)
species('euo_eur', [1.5, 4], [0.7, 0.95], SHRUB,
        arch(VASE, stems=[4, 12], stem_spread=[3, 30], stem_up=0.15, env_base=0.3, fill_from=0.08, sag=0.5,
             l1=dict(per_node=2, phyllo=90, angle=[45, 36], density=1.5, start=0.1), l2=dict(per_node=2, phyllo=90, density=3.0),
             **spray(4, 0.045, 2), leaf_len=cm(3, 10), leaf_wid=cm(1.2, 3.8), petiole=0.008, leaf_phyllo=90,
             lai=3.2, fill_up=0.3, bloom_place='axil', bloom_share=0.12, bloom_size=0.02,
             fruit_size=0.03),
        dict(outline='elliptic', gloss=0.06, translucency=0.55, flutter=0.1, fold=0.1, underside=[1.15, 0.02],
             jitter=[0.07, 0.03], bloom_shape='corymb', petals=4, fruit_shape='capsule', fruits=4))
species('euo_ver', [1, 2.5], [0.7, 0.95], SHRUB,
        arch(VASE, stems=[3, 8], stem_spread=[3, 30], stem_up=0.2, stem_radius=0.012, env_base=0.3, fill_from=0.08, sag=0.5,
             l1=dict(per_node=2, phyllo=90, angle=[48, 38], density=1.8, start=0.1), l2=dict(per_node=2, phyllo=90, density=3.5),
             **spray(4, 0.03, 2), leaf_len=cm(2, 6), leaf_wid=cm(1, 3.5), petiole=0.004, leaf_phyllo=90,
             lai=3.0, fill_clump=0.65, fill_len=[0.1, 0.35], bloom_place='axil', bloom_share=0.12,
             bloom_size=0.015, fruit_size=0.025),
        dict(outline='ovate', gloss=0.06, translucency=0.55, flutter=0.1, fold=0.1, underside=[1.15, 0.02],
             jitter=[0.07, 0.03], bloom_shape='corymb', petals=4, fruit_shape='capsule', fruits=3))
species('vib_opu', [1.5, 4], [0.85, 1.05], SHRUB,
        arch(VASE, stems=[6, 14], root_r=0.4, stem_arch=0.22, l1=dict(per_node=2, phyllo=90), l2=dict(per_node=2, phyllo=90),
             leaf_len=cm(6, 11), leaf_wid=cm(5, 10), petiole=0.025, leaf_per_node=2, leaf_phyllo=90,
             leaf_spacing=0.055, lai=3.4, leaf_flat=0.6, bloom_place='tip', bloom_share=0.3, bloom_size=0.08,
             fruit_size=0.07),
        dict(outline='lobed', lobes=3, gloss=0.06, translucency=0.6, flutter=0.1, fold=0.08, underside=[1.2, 0.03],
             jitter=[0.07, 0.03], bloom_shape='lacecap', petals=5, fruit_shape='berry', fruits=18),
        fruit_winter=True)
species('lon_xyl', [1, 2.5], [0.95, 1.25], SHRUB,
        arch(VASE, stems=[6, 14], stem_spread=[4, 44], stem_arch=0.2, l1=dict(per_node=2, phyllo=90),
             l2=dict(per_node=2, phyllo=90), **spray(4, 0.035, 2), leaf_len=cm(3, 6), leaf_wid=cm(2, 4), petiole=0.006,
             leaf_phyllo=90, lai=3.2, fill_len=[0.1, 0.35], fill_from=0.15, bloom_place='axil',
             bloom_share=0.15, bloom_size=0.02, fruit_size=0.014),
        dict(outline='elliptic', gloss=0.04, translucency=0.55, flutter=0.1, fold=0.1, underside=[1.3, 0.05],
             jitter=[0.07, 0.03], bloom_shape='single', petals=5, fruit_shape='berry', fruits=2))
species('pad_avi', [2.5, 5.5], [0.65, 0.95], SHRUB,
        arch(TREELET, stems=[3, 7], root_r=0.2, stem_spread=[6, 28], stem_arch=0.25, fill_from=0.3,
             l1=dict(up=-0.05, angle=[55, 45]), l2=dict(up=-0.15),
             **spray(3, 0.045), leaf_len=cm(6, 11), leaf_wid=cm(3, 5.5), petiole=0.015, lai=3.4,
             bloom_place='tip', bloom_share=0.35, bloom_size=0.12, bloom_droop=0.7, fruit_size=0.1),
        dict(outline='ovate', gloss=0.06, translucency=0.55, flutter=0.12, fold=0.1, underside=[1.3, 0.05],
             jitter=[0.07, 0.03], bloom_shape='raceme', petals=5, fruit_shape='berry', fruits=12))
species('ame_spi', [2, 5], [0.5, 0.75], SHRUB,
        arch(RODS, stems=[10, 24], stem_spread=[4, 28], **spray(3, 0.03), leaf_len=cm(3, 5.5), leaf_wid=cm(2.5, 4.5),
             petiole=0.01, leaf_flat=0.55, lai=3.0, l1=dict(density=2.0, len=[0.32, 0.14]), fill_len=[0.08, 0.3],
             bloom_place='tip', bloom_share=0.3, bloom_size=0.05, fruit_size=0.05),
        dict(outline='round', gloss=0.05, translucency=0.55, flutter=0.1, fold=0.08, underside=[1.2, 0.04],
             jitter=[0.07, 0.03], bloom_shape='raceme', petals=5, fruit_shape='berry', fruits=6))
species('rub_ida', [0.8, 2], [1.2, 2.0], SHRUB,
        # A thicket of upright, barely branched canes, leafy all along, their tips nodding (not a dome of shoots).
        arch(CANES, envelope='vase', env_base=0.55, stems=[12, 24], root_r=0.8, stem_spread=[3, 22], stem_arch=0.9,
             stem_up=0.5, levels=1, l1=dict(density=4, len=[0.18, 0.1], start=0.15), shoot_len=2.0, fill_from=0.1,
             leaf_kind='compound', leaflets=3, rachis=0.05, leaflet_angle=55, leaf_len=cm(5, 9), leaf_wid=cm(3, 6),
             petiole=0.04, leaf_spacing=0.06, lai=3.2, fill_len=[0.05, 0.15], fill_shell=0.35, bloom_place='tip',
             bloom_share=0.3, bloom_size=0.015, fruit_size=0.017),
        dict(gloss=0.05, translucency=0.5, flutter=0.12, fold=0.0, underside=[1.45, 0.12], jitter=[0.07, 0.03],
             bloom_shape='single', petals=5, fruit_shape='aggregate', fruits=1, leaflet_ratio=0.62))
species('rub_cae', [0.3, 1.0], [2.0, 3.5], SHRUB,
        arch(LOW, leaf_kind='compound', leaflets=3, rachis=0.035, leaflet_angle=55, leaf_len=cm(4, 7),
             leaf_wid=cm(3, 5), petiole=0.04, leaf_spacing=0.06, lai=2.6, bloom_place='axil', bloom_share=0.1,
             bloom_size=0.022, fruit_size=0.014),
        dict(gloss=0.06, translucency=0.5, flutter=0.1, fold=0.0, underside=[1.2, 0.06], jitter=[0.07, 0.03],
             bloom_shape='single', petals=5, fruit_shape='aggregate', fruits=1, leaflet_ratio=0.68))
species('ros_can', [1.5, 3], [1.5, 2.0], SHRUB,
        # Long canes climbing, then bowing over into a mound under their own weight, leafy along their length.
        # (Canes 1-3 cm across at the base, in short segments: an arching cane of 14 cm segments bent in elbows.)
        arch(CANES, stems=[8, 16], stem_len=[1.1, 1.3], stem_up=0.35, stem_arch=2.4, shoot_len=0.9, fill_from=0.05,
             stem_radius=0.0075, stem_seg=0.09,
             # (Hooked prickles on a broad flat foot, five to ten millimetres: thinner ones read as pimples.)
             prickle_spacing=0.05, prickle_len=0.008, prickle_hook=1, prickle_base=0.45,
             # (Laterals leave the arching canes at thirty to fifty degrees and curve up gently: a steep turn up read
             # as a hose's elbow.)
             l1=dict(start=0.08, up=0.25, angle=[48, 34]),
             leaf_kind='compound', leaflets=7, rachis=0.06, leaflet_angle=65, leaf_len=cm(2, 2.8),
             leaf_wid=cm(1.1, 1.6), petiole=0.012, leaf_spacing=0.035, bloom_place='tip', bloom_share=0.3,
             bloom_size=0.045, fruit_size=0.018),
        dict(gloss=0.18, translucency=0.5, flutter=0.1, fold=0.0, underside=[1.2, 0.04], jitter=[0.07, 0.03],
             bloom_shape='single', petals=5, fruit_shape='hip', fruits=1, leaflet_ratio=0.58),
        fruit_winter=True)
species('ros_maj', [0.8, 1.5], [1.0, 1.6], SHRUB,
        arch(RODS, stems=[14, 30], root_r=0.4, stem_spread=[5, 34], stem_radius=0.006, stem_up=0.25, prickle_spacing=0.04,
             prickle_len=0.004,
             leaf_kind='compound', leaflets=7, rachis=0.05, leaflet_angle=60, leaf_len=cm(2, 3), leaf_wid=cm(1, 1.5),
             petiole=0.01, leaf_spacing=0.03, lai=2.6, fill_len=[0.06, 0.2], bloom_place='tip', bloom_share=0.25,
             bloom_size=0.05, fruit_size=0.014),
        dict(gloss=0.05, translucency=0.5, flutter=0.1, fold=0.0, underside=[1.3, 0.08], jitter=[0.07, 0.03],
             bloom_shape='single', petals=5, fruit_shape='hip', fruits=1, leaflet_ratio=0.5),
        fruit_winter=True)
species('cra_mon', [1.5, 5], [0.85, 1.05], SHRUB,
        # Straighter stems (sinuous ones read as noodles), branched and leafy low (bare poles read as dead); zigzag
        # twigs (straight ones read as pick-up sticks) clothed in spur shoots, the thorns on the young wood (the old
        # trunks carry none: toothpicks stuck in them); the branches kinked every nine centimetres (eighteen read as
        # straight dowels at arm's length).
        arch(TREELET, stems=[3, 6], root_r=0.18, stem_spread=[10, 36], stem_wobble=0.08,
             l1=dict(density=2.8, wobble=0.2, start=0.12, seg_len=0.09),
             prickle_spacing=0.07, prickle_len=0.015, prickle_base=0.12, prickle_young=0.6, zig=0.36,
             l2=dict(density=4.5, wobble=0.28), **spray(4, 0.03), leaf_len=cm(3, 5.5),
             leaf_wid=cm(2.5, 4.5), petiole=0.015, lai=3.6, spur_share=0.45, spur_leaves=[3, 5], fill_len=[0.08, 0.25],
             fill_from=0.12, bloom_place='tip', bloom_share=0.35, bloom_size=0.05, fruit_size=0.04),
        dict(outline='lobed', lobes=5, gloss=0.25, translucency=0.5, flutter=0.08, fold=0.08, underside=[1.25, 0.03],
             jitter=[0.06, 0.03], bloom_shape='corymb', petals=5, fruit_shape='berry', fruits=8),
        fruit_winter=True)
species('sam_nig', [2, 6], [0.95, 1.25], SHRUB,
        arch(VASE, stems=[3, 9], stem_radius=0.035, stem_arch=0.3, l1=dict(per_node=2, phyllo=90, up=-0.05),
             l2=dict(per_node=2, phyllo=90, up=-0.15),
             leaf_kind='compound', leaflets=5, rachis=0.14, leaflet_angle=62, leaf_len=cm(7, 11), leaf_wid=cm(3, 4.5),
             petiole=0.05, leaf_per_node=2, leaf_phyllo=90, leaf_spacing=0.08, lai=3.2, bloom_place='tip',
             bloom_share=0.4, bloom_size=0.15, bloom_droop=-0.4, fruit_size=0.10),
        dict(gloss=0.04, translucency=0.5, flutter=0.1, fold=0.0, underside=[1.3, 0.08], jitter=[0.07, 0.03],
             bloom_shape='umbel', petals=5, fruit_shape='berry', fruits=48, leaflet_ratio=0.42))
species('sam_rac', [1, 3.5], [0.85, 1.15], SHRUB,
        arch(VASE, stems=[3, 9], stem_radius=0.025, l1=dict(per_node=2, phyllo=90), l2=dict(per_node=2, phyllo=90),
             leaf_kind='compound', leaflets=5, rachis=0.11, leaflet_angle=62, leaf_len=cm(6, 9), leaf_wid=cm(2.2, 3.5),
             petiole=0.04, leaf_per_node=2, leaf_phyllo=90, leaf_spacing=0.07, lai=3.0, bloom_place='tip',
             bloom_share=0.35, bloom_size=0.06, bloom_droop=-0.7, fruit_size=0.05),
        dict(gloss=0.04, translucency=0.5, flutter=0.1, fold=0.0, underside=[1.25, 0.06], jitter=[0.07, 0.03],
             bloom_shape='panicle', petals=5, fruit_shape='berry', fruits=48, leaflet_ratio=0.36))
species('syr_vul', [2, 6], [0.75, 1.0], SHRUB,
        arch(VASE, stems=[10, 24], root_r=0.4, stem_spread=[3, 26], stem_up=0.15, stem_arch=0.05, env_base=0.4,
             l1=dict(per_node=2, phyllo=90, angle=[38, 30], up=0.2, density=1.6), l2=dict(per_node=2, phyllo=90, up=0.1,
                                                                                          density=2.8),
             leaf_len=cm(5, 11), leaf_wid=cm(4, 8), petiole=0.02, leaf_per_node=2, leaf_phyllo=90, leaf_spacing=0.05,
             lai=4.6, fill_up=0.4, bloom_place='tip', bloom_share=0.3, bloom_size=0.15, bloom_droop=-0.6,
             fruit_size=0.1),
        dict(outline='ovate', gloss=0.12, translucency=0.45, flutter=0.08, fold=0.08, underside=[1.15, 0.03],
             jitter=[0.06, 0.03], bloom_shape='panicle', petals=4, fruit_shape='capsule', fruits=24),
        fruit_winter=True, leaves_fall_green=True)
species('phi_cor', [2, 3.5], [0.85, 1.05], SHRUB,
        arch(VASE, stems=[6, 16], stem_arch=0.3, l1=dict(per_node=2, phyllo=90), l2=dict(per_node=2, phyllo=90),
             leaf_len=cm(4, 9), leaf_wid=cm(2, 4.5), petiole=0.007, leaf_per_node=2, leaf_phyllo=90,
             leaf_spacing=0.045, lai=3.4, bloom_place='tip', bloom_share=0.3, bloom_size=0.06, fruit_size=0.05),
        dict(outline='ovate', gloss=0.04, translucency=0.55, flutter=0.1, fold=0.08, underside=[1.2, 0.03],
             jitter=[0.07, 0.03], bloom_shape='raceme', petals=4, fruit_shape='capsule', fruits=6),
        fruit_winter=True)
species('rib_nig', [1, 2], [1.0, 1.3], SHRUB,
        arch(VASE, stems=[6, 14], stem_spread=[8, 42], stem_radius=0.012, stem_arch=0.2, leaf_len=cm(4, 8),
             leaf_wid=cm(4, 8), petiole=0.035, leaf_spacing=0.05, lai=3.2, fill_len=[0.1, 0.35], fill_from=0.12,
             bloom_place='axil', bloom_share=0.15, bloom_size=0.04, bloom_droop=0.8, fruit_size=0.06),
        dict(outline='lobed', lobes=5, gloss=0.05, translucency=0.55, flutter=0.08, fold=0.08, underside=[1.2, 0.03],
             jitter=[0.07, 0.03], bloom_shape='raceme', petals=5, fruit_shape='berry', fruits=6))
species('rib_uva', [0.6, 1.2], [1.1, 1.5], SHRUB,
        arch(VASE, stems=[6, 14], stem_spread=[15, 58], stem_arch=0.55, stem_radius=0.008, leaf_len=cm(2, 4.5),
             prickle_spacing=0.04, prickle_len=0.009, prickle_base=0.2,
             leaf_wid=cm(2, 4), petiole=0.015, leaf_spacing=0.03, lai=2.8, spur_share=0.4, spur_leaves=[3, 5],
             fill_len=[0.06, 0.22], fill_from=0.1, bloom_place='old_wood', bloom_share=0.12, bloom_size=0.01,
             bloom_droop=0.8, fruit_size=0.014),
        dict(outline='lobed', lobes=3, gloss=0.04, translucency=0.55, flutter=0.08, fold=0.08, underside=[1.2, 0.03],
             jitter=[0.07, 0.03], bloom_shape='single', petals=5, fruit_shape='berry', fruits=1))

# ---- Village and road shrubs ---------------------------------------------------------------------------------
species('amo_fru', [1, 3], [0.8, 1.1], SHRUB,
        arch(VASE, stems=[6, 16], stem_up=0.15, stem_spread=[4, 30], leaf_kind='compound', leaflets=17, rachis=0.16,
             leaflet_angle=72, leaf_len=cm(2, 3.5), leaf_wid=cm(0.8, 1.4), petiole=0.02, leaf_spacing=0.06, lai=3.0,
             fill_up=0.4, bloom_place='tip', bloom_share=0.35, bloom_size=0.12, bloom_droop=-0.8, fruit_size=0.1),
        dict(gloss=0.05, translucency=0.55, flutter=0.12, fold=0.0, underside=[1.2, 0.04], jitter=[0.06, 0.03],
             bloom_shape='raceme', petals=5, fruit_shape='pod', fruits=30, leaflet_ratio=0.42),
        fruit_winter=True)
species('pru_cer', [2, 5], [0.85, 1.15], SHRUB,
        arch(TREELET, stems=[3, 7], root_r=0.2, stem_spread=[8, 34], l1=dict(up=-0.05), l2=dict(up=-0.2), spur_share=0.35,
             spur_leaves=[3, 5], **spray(3, 0.03), leaf_len=cm(4, 7), leaf_wid=cm(2.5, 4), petiole=0.012, lai=3.6,
             fill_from=0.25, bloom_place='old_wood', bloom_share=0.3, bloom_size=0.022, bloom_droop=0.3,
             fruit_size=0.025),
        dict(outline='elliptic', gloss=0.3, translucency=0.45, flutter=0.1, fold=0.12, underside=[1.2, 0.03],
             jitter=[0.06, 0.03], bloom_shape='single', petals=5, fruit_shape='drupe', fruits=1))
species('cyt_sco', [1, 2], [0.85, 1.15], SHRUB,
        arch(BROOM, **spray(6, 0.013), leaf_len=cm(1, 2), leaf_wid=cm(0.4, 0.8),
             petiole=0.003, leaf_angle=[15, 30], lai=1.1, bloom_place='axil', bloom_share=0.35,
             bloom_size=0.022, fruit_size=0.03),
        dict(outline='elliptic', gloss=0.05, translucency=0.15, flutter=0.06, fold=0.05, underside=[1.1, 0.02],
             jitter=[0.06, 0.03], bloom_shape='pea', petals=5, fruit_shape='pod', fruits=1),
        green_twigs=True)
species('cha_rut', [0.6, 1.8], [0.8, 1.2], SHRUB,
        arch(BROOM, stems=[6, 16], **spray(5, 0.016), leaf_len=cm(1.4, 2.4),
             leaf_wid=cm(0.6, 1.0), petiole=0.008, leaf_angle=[40, 60], bloom_place='axil',
             bloom_share=0.3, bloom_size=0.025, fruit_size=0.025),
        dict(outline='elliptic', gloss=0.03, translucency=0.45, flutter=0.06, fold=0.05, underside=[1.35, 0.12],
             jitter=[0.06, 0.03], bloom_shape='pea', petals=5, fruit_shape='pod', fruits=1))
species('hip_rha', [2, 3.5], [1.0, 1.4], SHRUB,
        arch(VASE, stems=[6, 14], stem_spread=[10, 50], stem_wobble=0.2, l1=dict(wobble=0.35), l2=dict(wobble=0.35),
             prickle_spacing=0.07, prickle_len=0.015, prickle_base=0.12, prickle_young=0.5, zig=0.25,
             **spray(6, 0.015), leaf_len=cm(4, 6.5), leaf_wid=cm(0.4, 0.8), petiole=0.002,
             leaf_angle=[25, 45], lai=4.2, fill_len=[0.1, 0.35], fill_from=0.1, spur_share=0.25,
             bloom_place='old_wood', bloom_share=0.45, bloom_size=0.004, fruit_size=0.03),
        dict(outline='narrow', gloss=0.08, translucency=0.4, flutter=0.12, fold=0.12, underside=[1.9, 0.15],
             jitter=[0.06, 0.03], bloom_shape='none', fruit_shape='berry', fruits=8),
        fruit_winter=True)

# ---- Pine-forest floor shrubs ---------------------------------------------------------------------------------
species('jun_com', [1, 3], [0.35, 0.55], [B, G, C],
        arch(JUNIPER, stems=[5, 14], root_r=0.25, leaf_spacing=0.028, lai=6.0, shoot_len=1.0, fill_shell=0.9, sag=0.3,
             l2=dict(density=12), bloom_place='axil', bloom_share=0.06, bloom_size=0.004, fruit_size=0.008),
        dict(outline='narrow', gloss=0.15, translucency=0.08, flutter=0.04, fold=0.0, underside=[1.4, 0.15],
             jitter=[0.05, 0.02], bloom_shape='none', fruit_shape='berry', fruits=1, needle_density=1.0,
             leaflet_ratio=0.043),
        evergreen=True, fruit_winter=True)



def mean(v):
    return 0.5 * (v[0] + v[1])


def leaf_budget(sid):
    """The leaves (elements) the species' tallest bush needs for its LAI, between 6000 and 22000 (a conifer's tufts
    40000): the generator stops at the LAI, the cap only binds the big crowns of small leaves."""
    s = SP[sid]
    a = s['arch']
    h = s['h'][1]
    w = h * mean(s['w'])
    # The generator's target (da_shrub_gen.cpp leaf_target): the LAI over the crown's larger projection.
    target = a['lai'] * math.pi * 0.5 * w * max(0.5 * w, 0.5 * h)
    kind = a.get('leaf_kind', 'simple')
    if kind == 'needles':
        per = a['leaf_spacing'] * 2.0 * a['needle_len'] * 0.5
    else:
        per = mean(a['leaf_len']) * mean(a['leaf_wid']) * 0.67 * (a.get('leaflets', 1) if kind in ('compound', 'spray') else 1)
    per *= 0.85
    # (A needle tuft counts its needles' small area: a juniper's dense column needs about twice the elements.)
    return int(min(max(target / max(per, 1e-5) * 1.1, 6000), 40000 if kind == 'needles' else 22000))


# The colours of a species missing from the photos: a related one's.
COLOUR_STANDIN = {'pad_avi': 'pru_pad'}

# ======================================================================================================
# Colours from the photos
# ======================================================================================================
BUCKET_DAY = {'early_spring': 95.5, 'late_spring': 134.5, 'early_summer': 165.5, 'mid_summer': 195.5,
              'late_summer': 226.5, 'early_autumn': 257.5, 'mid_autumn': 287.5, 'late_autumn': 318.5, 'winter': 15.5}
LEAFY_BUCKETS = ['late_spring', 'early_summer', 'mid_summer', 'late_summer', 'early_autumn', 'mid_autumn']


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
    """A colour's hue kept, its linear luminance raised to y_min or lowered to y_max."""
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


def flower_floor(h):
    """The luminance a petal of the photo's colour class reflects at least (the photo median is shaded): white
    0.55, yellow 0.42, pink 0.22, blue/violet 0.14, red 0.09 (GRASS.md, the forbs' rule)."""
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


def photo(sp, bucket, role, min_n=2):
    p = PHOTOS.get(sp)
    if not p:
        return None
    b = p['buckets'].get(bucket)
    if not b or role not in b:
        return None
    r = b[role]
    return r['hex'] if r['n'] >= min_n else None


def photo_kinds(sp, buckets, names):
    """The photos' autumn kinds over the buckets: [(name, hex, n)] most photographed first."""
    p = PHOTOS.get(sp)
    out = {}
    if not p:
        return []
    for bk in buckets:
        b = p['buckets'].get(bk, {})
        for k in names:
            kv = b.get('kinds', {}).get(k)
            if kv and kv.get('n', 0) >= 1:
                o = out.setdefault(k, [kv['hex'], 0])
                o[1] += kv['n']
    return sorted(((k, v[0], v[1]) for k, v in out.items()), key=lambda x: -x[2])


# Twig colours of the young growth (the flora's winter twig descriptions; GSC's cards had none).
# (A juniper's current shoots are green under their needle bases, dark in the brush's shade: brown twig lines ran
# through its brushes, pale green ones stood out of them as sticks. A warty spindle's and a dog rose's young shoots
# are green: grey-brown hid the spindle's warts, tan made the rose's canes rattan.)
TWIG = {'sal_cin': '#5E5248', 'sal_aur': '#7A4A36', 'sal_pen': '#8A6A3A', 'sal_cap': '#6E6450', 'sal_acu': '#7A3A52',
        'sal_tri': '#7E7A48', 'sal_vim': '#7A7E5A', 'sal_ros': '#6A4E3A', 'fra_aln': '#5E463E', 'cor_ave': '#7A6656',
        'euo_eur': '#5E7040', 'euo_ver': '#5A6A3C', 'vib_opu': '#7E6446', 'lon_xyl': '#6E6252', 'pad_avi': '#6E3E36',
        'ame_spi': '#5E4038', 'rub_ida': '#7E5A44', 'rub_cae': '#5E6E6A', 'ros_can': '#56663A', 'ros_maj': '#7E3E2E',
        'cra_mon': '#6A5A4C', 'sam_nig': '#8A8270', 'sam_rac': '#6E5E50', 'syr_vul': '#6E6658', 'phi_cor': '#8E6E4E',
        'rib_nig': '#6E5A48', 'rib_uva': '#5E5244', 'amo_fru': '#5E5446', 'pru_cer': '#6E3E32', 'cyt_sco': '#4E7036',
        'cha_rut': '#7A7A6A', 'hip_rha': '#8E8E86', 'jun_com': '#4A5236'}
# Glaucous and silvery foliage (the flora's notes: sea buckthorn's scaled leaves, juniper's stomatal bands, the
# rosemary willow): its share of a blue-grey green at the leaf's own luminance. The photos' warm light and white
# balance read these bushes olive. The grey and the eared willows' downy leaves are grey-green, not glaucous: a
# share of a neutral grey-green (a fifth of the blue-grey made them frosted blue domes on the meadows they fill).
GLAUCOUS = {'hip_rha': 0.6, 'jun_com': 0.45, 'sal_ros': 0.3}
GLAUCOUS_HEX = '#7A8A84'
GREY = {'sal_cin': 0.07, 'sal_aur': 0.05}
GREY_HEX = '#878F7E'
# Bark: furrow depth (0 smooth .. 1 deeply furrowed), the young twigs' gloss, lenticels (the flora's bark notes).
BARK = {'pru_cer': [0.15, 0.3, 0.6], 'fra_aln': [0.25, 0.15, 0.6], 'sam_nig': [0.8, 0.05, 0.8],
        'cor_ave': [0.3, 0.15, 0.5], 'pad_avi': [0.25, 0.15, 0.5], 'sal_pen': [0.4, 0.4, 0], 'syr_vul': [0.6, 0.1, 0],
        'jun_com': [0.85, 0.05, 0], 'phi_cor': [0.7, 0.05, 0]}
# A leaf's own value and hue step at the least (the species' jitter): photos of a crown show leaves a sixth lighter
# or darker than their neighbours and a little yellower or bluer; less read as one sheet of olive paper.
JITTER_MIN = [0.15, 0.06]
# Cuticle gloss where the flora's notes say glossier than the default (hawthorn, lilac and the spindles' shiny
# leaves, guelder rose and elder a little).
GLOSS = {'cra_mon': 0.2, 'syr_vul': 0.25, 'euo_eur': 0.15, 'euo_ver': 0.15, 'vib_opu': 0.12, 'sam_nig': 0.1, 'sam_rac': 0.1}
# Crustose lichen on the old stems (0..1): the humid Polesie's willows, hazel, alder buckthorn and bird cherry carry
# the most; the biennial canes (raspberry, dewberry) and the green-stemmed brooms none.
LICHEN = {'sal_cin': 0.45, 'sal_aur': 0.4, 'sal_pen': 0.3, 'sal_cap': 0.4, 'sal_acu': 0.2, 'sal_tri': 0.2, 'sal_vim': 0.15,
          'sal_ros': 0.1, 'fra_aln': 0.4, 'cor_ave': 0.45, 'euo_eur': 0.3, 'euo_ver': 0.25, 'vib_opu': 0.3, 'lon_xyl': 0.25,
          'pad_avi': 0.35, 'ame_spi': 0.3, 'rub_ida': 0.0, 'rub_cae': 0.0, 'ros_can': 0.05, 'ros_maj': 0.05, 'cra_mon': 0.35,
          'sam_nig': 0.35, 'sam_rac': 0.25, 'syr_vul': 0.3, 'phi_cor': 0.2, 'rib_nig': 0.1, 'rib_uva': 0.1, 'amo_fru': 0.15,
          'pru_cer': 0.25, 'cyt_sco': 0.05, 'cha_rut': 0.05, 'hip_rha': 0.2, 'jun_com': 0.15}
# The texture sets' layers (tools/shrubs/textures: make_leaf_textures.py and make_bark_textures.py write them).
TEX_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'textures')
# (A guelder rose's bark is finely fissured grey-brown: the bird cherry's pale lenticels read as sprinkles on it.)
BARK_OVERRIDE = {'vib_opu': 'spindle'}
# The bark textures' contrast about their tile's mean (r3/da_shrub_wood.ps: a power of the luminance ratio, the hue
# kept): their plates and fissures, lenticels and warts differ more in hue than in value, and on the tubes a hawthorn's
# trunk read as smooth elephant skin, a hazel's showed no lenticels. Fissured and scaly barks most, the smooth ones
# with lenticels less, the canes least; the textures already strong in value (elder's cork, juniper's and the
# honeysuckles' shreds, alder buckthorn's lenticels, cherry's bands) keep theirs.
BARK_CONTRAST = {'hazel': 1.6, 'bird_cherry': 1.3, 'willow_grey': 1.5, 'willow_goat': 1.6, 'willow_smooth': 1.45,
                 'spindle': 1.45, 'warty_spindle': 1.35, 'hawthorn': 2.6, 'currant': 1.4, 'gooseberry': 1.3, 'rose': 1.15,
                 'raspberry': 1.3, 'dewberry': 1.4}
# A species' bark tile over the textures' 6 cm (the plates of an old stem grow with it): a hawthorn's, a lilac's and
# a sea buckthorn's fissured plates a few centimetres across on a trunk - at 6 cm a tile's plates were 5-9 mm, a
# stucco from a metre and a half (a review against Commons photos of Crataegus trunks: 4-16 times contrast, clearly
# vertical fissures).
BARK_SCALE = {'cra_mon': 2.5, 'syr_vul': 1.8, 'hip_rha': 1.8}
# A species' tint of its bark texture (linear): a hawthorn's old stems a neutral grey, a tenth darker (Commons photos
# of Crataegus trunks, five old ones measured: mean r/g 0.99-1.17, b/g 0.79-1.05, medians 1.05 and 0.84; the
# texture's mean is a warm taupe, 1.27 and 0.76), a lilac's grey-brown, a sea buckthorn's dark grey-brown; a dog
# rose's old canes olive grey-brown (beige rattan on the meadow; their young growth is the twig green), a cinnamon
# rose's red-brown.
BARK_TINT = {'cra_mon': [0.76, 0.92, 1.0], 'syr_vul': [0.85, 0.95, 1.0], 'hip_rha': [0.84, 0.86, 0.88],
             'ros_can': [0.85, 1.0, 0.75], 'ros_maj': [1.1, 0.88, 0.85]}
# The share of the bark's relief and detail the young growth keeps (the default a quarter): a warty spindle's corky
# warts stand on its green twigs, where they show most.
YOUNG_RELIEF = {'euo_ver': 0.85}
# Prickles and thorns in their own colour (the flora's notes; in the young twig colour a rose's hooks hid on its green
# canes): a dog rose's red-brown, a cinnamon rose's reddish, hawthorn's glossy dark brown, sea buckthorn's grey-brown,
# gooseberry's pale straw spines.
PRICKLE = {'ros_can': '#6E3C2A', 'ros_maj': '#6A3424', 'cra_mon': '#5A4234', 'hip_rha': '#6A6058', 'rib_uva': '#9A8A68'}
# The young growth's flush on its upper, sunlit side and its share (a dog rose's green canes redden where the sun
# falls on them; uniform green read as a garden hose).
BLUSH = {'ros_can': ('#7A4A34', 0.55), 'ros_maj': ('#7A3428', 0.4)}
# The far parts' value and chroma on their open, light top against the species' near leaves where its own calibration
# is off (the default one): a dog rose's young leaflets on the tips of its open arching canes gather into far tops a
# quarter lighter and two fifths more saturated than its near ones at 40 m (a trim of the whole colour left them pale).
FAR_VALUE = {'ros_can': 0.8}
FAR_CHROMA = {'ros_can': 0.7}


def _manifest(name):
    path = os.path.join(TEX_DIR, name)
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as f:
        return json.load(f)


LEAF_TEX = _manifest('leaf_manifest.json')
LEAF_HULLS = _manifest('leaf_hulls.json')
TEX_STATS = _manifest('texture_stats.json')
BARK_TEX = _manifest('bark_manifest.json')


def bark_layer(sid):
    """The species' bark layer in the bark array (the manifest's, or its override's)."""
    bark = BARK_TEX.get('species', {}).get(sid, 0)
    names = [t['name'] for t in BARK_TEX.get('types', [])]
    if sid in BARK_OVERRIDE and BARK_OVERRIDE[sid] in names:
        bark = names.index(BARK_OVERRIDE[sid])
    return bark


def texture_lines(sid):
    """The species' texture layers, its near leaf's hull and its lichen (r3/da_shrub_common.h rows 8..10)."""
    e = LEAF_TEX.get('species', {}).get(sid, {})
    a = e.get('a', 0)
    tex = [a, e.get('b', a), e.get('card', -1), e.get('leaflet', -1)]
    layers = LEAF_TEX.get('layers', [])
    # Leaf b is drawn in a's square (one fit per species): a b of another size would be squeezed, so it goes.
    if 0 <= tex[1] < len(layers) and tex[1] != a and 0 <= a < len(layers):
        ba, bb = layers[a]['box_mm'], layers[tex[1]]['box_mm']
        if any(abs(bb[i] - ba[i]) > 0.12 * ba[i] for i in range(2)):
            tex[1] = a
    # The near leaf's hull holds both leaves (build_arrays.py fits it to their coverage); quads ignore it.
    hull = LEAF_HULLS.get(sid, [1.0, 0.3333, 1.0, 0.6667, 1.0, 1.0])
    # The textures' squares against the generator's elements (da_shrub_gen.cpp leaf_at, sprays_on, tufts_on): a
    # leaf element starts at the blade (past its petiole) and is the blade's length and width, a compound one the
    # rachis and the terminal leaflet as long and the splayed leaflets as wide, a spray its stretch of shoot, a tuft
    # its leaf spacing; a texture's square holds the petiole too and a margin round the object (box_mm). Per draw:
    # the square's length and width over the element's, and how far behind the element's base it starts (a share
    # of the element's length).
    a_ = SP[sid]['arch']
    kind = a_.get('leaf_kind', 'simple')
    box = lambda i: [x / 1000.0 for x in layers[i]['box_mm']] if 0 <= i < len(layers) else None
    pet = a_.get('petiole', 0.01)
    ll, lw = mean(a_['leaf_len']) if a_.get('leaf_len') else 0.05, mean(a_['leaf_wid']) if a_.get('leaf_wid') else 0.03
    fit = [1.0, 1.0, 0.0]
    if box(a):
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
    # The clusters' cells are the leaflet (a compound leaf's) or leaf a: their coverage of the square.
    cl = tex[3] if tex[3] >= 0 else a
    cover = TEX_STATS.get('leaf_cover', {}).get(layers[cl]['name'], 0.0) if 0 <= cl < len(layers) else 0.0
    card = [1.0, 1.0, 0.0]
    if kind == 'spray' and box(tex[2]):
        bl, bw = box(tex[2])
        splay = math.radians(mean(a_.get('leaf_angle', [45, 60])))
        le, we = max(a_.get('spray_len', 0.1), 0.03), 2.0 * ll * math.sin(splay) + lw
        card = [bl / le, bw / we, 0.006 * bl / le]
    bark = bark_layer(sid)
    names = [t['name'] for t in BARK_TEX.get('types', [])]
    contrast = BARK_CONTRAST.get(names[bark], 1.0) if 0 <= bark < len(names) else 1.0
    return ['; textures (tools/shrubs/textures: leaf layers a, b, card, leaflet; the near leaf hull; bark; lichen; the',
            '; squares over the elements: leaf length, width, start behind the base; the card the same; the bark as the',
            '; species wears it: the young growth\'s share of its detail, its tint and contrast)',
            'leaf_tex = %s' % fmt([float(x) for x in tex]), 'leaf_hull = %s' % fmt([float(x) for x in hull]),
            'bark_tex = %s' % fmt(float(bark)),
            # (Soft blotches over a tenth of an old stem at most, half covering its bark, r3/da_shrub_wood.ps.)
            'lichen = %s' % fmt(round(LICHEN.get(sid, 0.2), 3)),
            'leaf_fit = %s' % fmt([round(x, 4) for x in fit + [cover]]), 'card_fit = %s' % fmt([round(x, 4) for x in card]),
            'young_relief = %s' % fmt(YOUNG_RELIEF.get(sid, 0.25)),
            'bark_tint = %s' % fmt(BARK_TINT.get(sid, [1.0, 1.0, 1.0])), 'bark_contrast = %s' % fmt(contrast),
            'bark_scale = %s' % fmt(BARK_SCALE.get(sid, 1.0))] + (
            ['prickle_colour = %s' % fmt([round(x, 4) for x in to_lin(hex_rgb(PRICKLE[sid]))])] if sid in PRICKLE else []) + (
            ['twig_blush = %s' % fmt([round(x, 4) for x in to_lin(hex_rgb(BLUSH[sid][0]))] + [BLUSH[sid][1]])]
            if sid in BLUSH else []) + (['far_value = %s' % fmt(FAR_VALUE[sid])] if sid in FAR_VALUE else []) + (
            ['far_chroma = %s' % fmt(FAR_CHROMA[sid])] if sid in FAR_CHROMA else []) + (
            ['crop_spread = %s' % fmt(FRUIT_CROP[sid]['crop_spread'])] if sid in FRUIT_CROP else [])


# The fruit a species' groups carry (season key fruit_keep: the share of a group's fruits that set, then what the
# birds leave of it through the winter), the share of its flowering sites that set any, and how far one bush's crop
# falls short of the best one's (crop_spread). Every other species keeps all it sets.
# Common hawthorn (Commons pack 16.10.2026: 36 winter, 46 autumn, 40 summer photos of Crataegus monogyna): a corymb of
# five to twenty-five flowers ripens one to eight haws, four or so in a good year, and a quarter of the corymbs set
# none; one bush stands laden in October, the next carries a few clusters; thrushes and waxwings strip the haws through
# the winter - the January photos keep a quarter of the autumn's crop or less, dark and shrivelled, by March a few.
# winter: (day of the year, share of the set left), 'b' the bud burst.
FRUIT_CROP = {'cra_mon': dict(set=0.5, sites=0.75, crop_spread=0.7,
                              winter=[(300, 1.0), (349, 0.7), (20, 0.45), (60, 0.2), ('b', 0.1)])}

# Phenology the flora's cards have wrong for the zone: the brooms' pods blacken by mid-July (their 'ripe' was the
# seed's shedding in late summer).
PHENO_FIX = {'cyt_sco': {'fruit_ripe': 205}, 'cha_rut': {'fruit_ripe': 205}}
# Fruit colours given as the game's final values (unscaled): the brooms' hairy green pods turning brown-black.
FRUIT_FINAL = {'cyt_sco': {'c_fruit_unripe': '#4E6438', 'c_fruit_ripe': '#2A2622'},
               'cha_rut': {'c_fruit_unripe': '#5A6A48', 'c_fruit_ripe': '#33291F'}}
# Fruits that stay green until their last week or so (an elder's berries until mid-August, then colour fast).
COLOUR_DAYS = {'sam_nig': 8}
# Undersides by the flora (white-felted raspberry, silky silver osier): their tint at a linear luminance of their own,
# some three times the upper side's (at the photos' own brightness, five to eight times, the undersides read frosted).
UNDER = {'rub_ida': ('#9AA594', 0.24), 'sal_vim': ('#B0B8AC', 0.2)}
# Unripe fruit by the flora's notes (green unless it reddens first).
UNRIPE = {'fra_aln': '#9A3A2A', 'pad_avi': '#8A3A2A', 'sam_nig': '#5E7A3A', 'ame_spi': '#8A3A3A', 'rub_ida': '#C08A8A',
          'cor_ave': '#9AA46A',
          'rub_cae': '#8A2A2A'}
# Ripe fruit where the photos have none (the flora's colour).
RIPE = {'ame_spi': '#2E2A44', 'sal_aur': '#E0E0D6', 'sal_acu': '#E0E0D6', 'hip_rha': '#E07A1E', 'amo_fru': '#6E5040',
        'cyt_sco': '#2A2622', 'lon_xyl': '#A01E24', 'jun_com': '#2E2A3E'}
PETAL = {'fra_aln': '#C8CCAA', 'euo_eur': '#B8BC86', 'euo_ver': '#8A6A4A', 'amo_fru': '#4A2A6A', 'cyt_sco': '#E8C030',
         'lon_xyl': '#E8E0B8', 'ame_spi': '#F0F0EA', 'rib_nig': '#8A9A6A', 'rib_uva': '#9AA070'}
CENTRE = {'ros_can': '#E0B830', 'ros_maj': '#E0B830', 'cra_mon': '#A03050', 'amo_fru': '#E0A020', 'rub_ida': '#D8D0A0'}


# A photo is exposed by its camera, not measured: a green bush fills the frame and is pushed to mid grey, a yellow
# one less so. The leaves' absolute level is the game's (the blade grass's meadow live colour of July, keyframes.json,
# is a linear luminance of 0.080; the shrubs' leaves a fifth under it - the dicots' darker sun leaves, and a bush at
# the meadow's level read pale against it -, the young flush lighter); each species keeps the chromaticity of its
# photos (leaf_tint) and half of its darkness relative to the median of the species, per bucket (the blade grass's
# rule, tools/seasons/data/make_species.py). The brighter subjects - autumn leaves, dead leaves, fruit - were pushed
# less: they keep their measured colour scaled by the exposure their kind was given.
# (07.10, the textured leaves: their lit tenth read 0.94 of the meadow against the photo pack's 1.59 [0.90-1.97], the
# crown's median 0.27 against 0.40: a fifth more.)
REF_Y = {'summer': 0.072, 'young': 0.1}
SCALE_Y = {'autumn': (0.6, 0.04, 0.26), 'dead': (0.7, 0.05, 0.18), 'fruit': (0.7, 0.012, 0.3), 'unripe': (0.4, 0.03, 0.12),
           'twig': (0.75, 0.04, 0.12)}
_MEDIANS = {}


def photo_y(h):
    return lum(to_lin(hex_rgb(h)))


def median_y(role, bucket):
    """The median luminance of a colour role in a bucket over the species (3 photos or more)."""
    key = (role, bucket)
    if key not in _MEDIANS:
        ys = sorted(photo_y(p['buckets'][bucket][role]['hex']) for p in PHOTOS.values()
                    if bucket in p['buckets'] and role in p['buckets'][bucket] and p['buckets'][bucket][role]['n'] >= 3)
        _MEDIANS[key] = ys[len(ys) // 2] if ys else 0.2
    return _MEDIANS[key]


def to_level(h, ref, med):
    """h at the reference level, half of its darkness relative to the median kept."""
    y = max(photo_y(h), 1e-4)
    t = ref * (y / med) ** 0.5
    return scale_lum(h, t, t)


def scaled(h, role):
    k, lo, hi = SCALE_Y[role]
    t = min(max(photo_y(h) * k, lo), hi)
    return scale_lum(h, t, t)


# A green leaf reflects a third or more of its green in the blue and about three fifths in the red (the leaf spectra
# over the sRGB primaries; the ColorChecker's foliage patch, linear 0.64 : 1 : 0.37). The photos' warm light, white
# balance and the cameras' saturation leave a sixth to a tenth in the blue: a bush of those leaves reads acid
# yellow-green against the meadow. The blue is raised into a leaf's range by a soft floor (the photos' order kept,
# their spread halved; at the ColorChecker's level a crown's sunlit top read cool and frosted against the photos,
# whose lit side is a bush's yellowest), the red kept within it, the luminance the colour's own; the fresh and young
# leaves are yellower.
LEAF_TINT = {'summer': (0.27, 0.42, 0.48, 0.80), 'fresh': (0.25, 0.40, 0.50, 0.84), 'young': (0.23, 0.38, 0.52, 0.86)}
# The foliage of May and June is not July's: the fresh leaves, thinner and with less chlorophyll, are lighter and
# yellower, and darken over the first summer weeks (the bucket's luminance factor and tint).
FRESH = {'late_spring': (1.3, 'fresh'), 'early_summer': (1.12, 'summer')}
# Silvery leaves reflect more: sea buckthorn's scales, the rosemary willow's silky hairs (the leaf's luminance factor;
# at the green leaves' level a sea buckthorn read near black inside its own dense crown).
SILVERY = {'hip_rha': 1.7, 'sal_ros': 1.3}
# The light mid-green leaves of the flora's descriptions (hazel's soft matte ones, guelder rose, mock orange, cherry
# plum) against the dark glossy ones of alder buckthorn, hawthorn, elder and bird cherry: half of the photos' own
# darkness alone left every broadleaf species on one dark green, and a fifth either way was lost in the crown's
# shade (the leaf's luminance factor).
LIGHTER = {'cor_ave': 1.35, 'vib_opu': 1.25, 'phi_cor': 1.3, 'pru_cer': 1.15, 'sam_nig': 1.25, 'syr_vul': 1.15,
           'fra_aln': 0.9, 'cra_mon': 0.92, 'pad_avi': 0.92}


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


def colour_keys(sid):
    """The season colours of a species: young and summer leaves through the year, autumn A and B, the dead leaf, the
    underside, bark, young twig, petals and centre, unripe and ripe fruit, catkins."""
    src = sid if sid in PHOTOS else COLOUR_STANDIN.get(sid, sid)
    fl = FLORA_BY_ID.get(sid, {})
    ph = fl.get('phenology', {})
    keys = {}
    # Summer leaf by bucket (the leafy months), the young flush in May.
    # (The yellowing leaves of autumn and an evergreen's winter bronze keep the photos' chroma.)
    summer = []
    for bk in LEAFY_BUCKETS + ['late_autumn']:
        h = photo(src, bk, 'leaf', 3)
        if h:
            k, kind = FRESH.get(bk, (1.0, 'summer'))
            c = to_level(h, REF_Y['summer'] * k, median_y('leaf', bk))
            summer.append((BUCKET_DAY[bk], c if bk in ('mid_autumn', 'late_autumn') else leaf_tint(c, kind)))
    if not summer:
        summer = [(195.5, leaf_tint(scale_lum('#5E7E3A', REF_Y['summer'], REF_Y['summer']), 'summer'))]
    winter_leaf = photo(src, 'winter', 'leaf', 3) if SP[sid]['extra'].get('evergreen') else None
    if winter_leaf:
        summer.append((15.5, to_level(winter_leaf, REF_Y['summer'], median_y('leaf', 'mid_summer'))))
    glauc = GLAUCOUS.get(sid, 0.0)
    if glauc > 0.0:
        summer = [(d, scale_lum(mix(h, GLAUCOUS_HEX, glauc), photo_y(h), photo_y(h))) for d, h in summer]
    grey = GREY.get(sid, 0.0)
    if grey > 0.0:
        summer = [(d, scale_lum(mix(h, GREY_HEX, grey), photo_y(h), photo_y(h))) for d, h in summer]
    silver = SILVERY.get(sid, 1.0) * LIGHTER.get(sid, 1.0)
    summer = [(d, scale_lum(h, photo_y(h) * silver, photo_y(h) * silver)) for d, h in summer]
    keys['c_summer'] = summer
    young_b = 'late_spring' if photo(src, 'late_spring', 'young', 2) else 'early_summer'
    young = photo(src, young_b, 'young', 2)
    young = to_level(young, REF_Y['young'], median_y('young', young_b)) if young else \
        scale_lum(mix(summer[0][1], '#B8D070', 0.4), REF_Y['young'], REF_Y['young'])
    young = leaf_tint(young, 'young')
    if SP[sid]['extra'].get('young_copper'):
        young = mix(young, scale_lum('#8A5A36', REF_Y['young'], REF_Y['young']), 0.45)
    if glauc > 0.0:
        young = scale_lum(mix(young, GLAUCOUS_HEX, 0.6 * glauc), photo_y(young), photo_y(young))
    if grey > 0.0:
        young = scale_lum(mix(young, GREY_HEX, 0.6 * grey), photo_y(young), photo_y(young))
    young = scale_lum(young, photo_y(young) * silver, photo_y(young) * silver)
    keys['c_young'] = [(100.5, young)]
    # Autumn: the photos' dominant kind and the next; the flora's colour where the photos have none.
    kinds = photo_kinds(src, ['early_autumn', 'mid_autumn', 'late_autumn'], ['yellow', 'orange', 'red', 'burgundy'])
    autumn = photo(src, 'mid_autumn', 'autumn', 2) or photo(src, 'early_autumn', 'autumn', 2) or photo(src, 'late_autumn', 'autumn', 2)
    a_hex = kinds[0][1] if kinds else (autumn or '#A89838')
    b_hex = kinds[1][1] if len(kinds) > 1 else mix(a_hex, '#8A3A20', 0.5)
    b_share = (kinds[1][2] / float(kinds[0][2] + kinds[1][2])) if len(kinds) > 1 else 0.15
    if SP[sid]['extra'].get('leaves_fall_green'):
        a_hex, b_hex, b_share = mix(summer[-1][1], '#7A7030', 0.35), '#5E5A36', 0.3
    if SP[sid]['extra'].get('autumn_red'):
        b_share = max(b_share, 0.45)
    if not SP[sid]['extra'].get('leaves_fall_green'):
        a_hex, b_hex = scaled(a_hex, 'autumn'), scaled(b_hex, 'autumn')
    keys['c_autumn_a'] = [(288.5, a_hex)]
    keys['c_autumn_b'] = [(288.5, b_hex)]
    keys['b_share'] = round(min(max(b_share, 0.05), 0.6), 3)
    dead = photo_kinds(src, ['late_autumn', 'winter'], ['tan', 'dead'])
    keys['c_dead'] = [(318.5, scaled(dead[0][1] if dead else '#6E5A40', 'dead'))]
    # The underside: the summer leaf greyer by the glaucous index. Its tissue reflects more than the upper side, but
    # it faces down, into the leaf's own shade, which the two-sided shading (the normal turned to the eye, half the
    # crown's) does not give it: at its albedo, a fifth to two fifths lighter, the turned-away half of a crown seen
    # from the side lit as the upper one and read frosted, pale and bluish against the photos of the bushes (whose
    # lit side is their yellowest). So it carries its shade: about the upper side's level, a silvery one a little more.
    gi = (PHOTOS.get(src) or {}).get('glaucous_index') or 0.25
    top = summer[min(2, len(summer) - 1)][1]
    under_y = photo_y(top) * (0.95 + 0.3 * min(gi, 0.5))
    under = scale_lum(mix(top, '#B4BCAA', min(0.05 + 0.35 * gi, 0.35)), under_y, under_y)
    keys['c_under'] = [(195.5, under)]
    # The felted and silky undersides the photos' upper sides do not tell: a raspberry's chalk white, an osier's silver.
    if sid in UNDER:
        tint, y = UNDER[sid]
        keys['c_under'] = [(195.5, scale_lum(tint, y, y))]
    bark = photo(src, 'winter', 'twig', 2) or photo(src, 'late_autumn', 'twig', 1) or photo(src, 'early_spring', 'twig', 1)
    # Bark and twigs at a wood's albedo (a tenth or so; the photos' trunks were pushed bright like the leaves).
    keys['c_bark'] = [(15.5, scale_lum(bark or '#5E5046', 0.04, 0.13))]
    # The bark texture's own mean where the species has one: the old wood the far tiers and the bare cells draw is
    # the bark the near tubes show.
    bark_type = bark_layer(sid) if BARK_TEX.get('species', {}).get(sid) is not None else None
    if bark_type is not None and 0 <= bark_type < len(BARK_TEX.get('types', [])):
        mean_hex = TEX_STATS.get('bark_mean', {}).get(BARK_TEX['types'][bark_type]['name'])
        if mean_hex:
            # (In the species' tint, as the near tubes wear it.)
            tint = BARK_TINT.get(sid, [1.0, 1.0, 1.0])
            keys['c_bark'] = [(15.5, rgb_hex(to_srgb([x * k for x, k in zip(to_lin(hex_rgb(mean_hex)), tint)])))]
    twig = scaled(TWIG.get(sid, bark or '#6E5A48'), 'twig')
    keys['c_twig'] = [(15.5, twig)]
    if SP[sid]['extra'].get('green_twigs'):
        keys['c_twig'] = [(15.5, twig), (165.5, mix(twig, summer[0][1], 0.4))]
    # Flowers: the photos' flowering bucket, raised to the petal's floor; catkins the same from their bucket.
    fs = ph.get('flower_start') or 150
    fb = min(BUCKET_DAY, key=lambda b: abs(((BUCKET_DAY[b] - fs + 182) % 365) - 182))
    flower = photo(src, fb, 'flower', 2) or photo(src, 'late_spring', 'flower', 2) or photo(src, 'early_summer', 'flower', 2)
    petal = PETAL.get(sid) or (flower_floor(flower) if flower else '#E8E6DA')
    keys['c_petal'] = [(fs, petal)]
    keys['c_centre'] = [(fs, CENTRE.get(sid, '#D8B040'))]
    keys['c_catkin'] = [(fs, flower_floor(flower) if flower else '#C8B860')]
    rd = ph.get('fruit_ripe') or 230
    rb = min(BUCKET_DAY, key=lambda b: abs(((BUCKET_DAY[b] - rd + 182) % 365) - 182))
    fruit = RIPE.get(sid) or photo(src, rb, 'fruit', 2) or photo(src, 'early_autumn', 'fruit', 2) or '#6E2A2A'
    keys['c_fruit_ripe'] = [(rd, fruit if sid in RIPE else scaled(fruit, 'fruit'))]
    keys['c_fruit_unripe'] = [(rd, scaled(UNRIPE.get(sid, '#7A9A4A'), 'unripe'))]
    # The pods' own colours, as the game shows them (the bloom shaders' calibration: no exposure scaling).
    for key, value in FRUIT_FINAL.get(sid, {}).items():
        keys[key] = [(rd, value)]
    return keys


# ======================================================================================================
# Season keys from the phenology
# ======================================================================================================
def wrap(d):
    return round(d % 365.0, 1)


def season_keys(sid):
    """The engine's season scalars of a species (da_shrub_common.h shrub_seas rows 0..2) as day:value keys."""
    fl = FLORA_BY_ID.get(sid)
    ph = dict(fl['phenology']) if fl else {}
    ph.update(PHENO_FIX.get(sid, {}))
    ex = SP[sid]['extra']
    k = {}
    if ex.get('evergreen'):
        k['leaf_out'] = [(1, 1.0)]
        k['leaf_grow'] = [(1, 1.0)]
        k['autumn'] = [(1, 0.0)]
        k['fallen'] = [(1, 0.0)]
    else:
        b, u, f = ph['bud_burst'], ph['leaf_unfold'], ph['leaf_full']
        a0, ap, l0, l1 = ph['autumn_start'], ph['autumn_peak'], ph['leaf_fall_start'], ph['leaf_fall_end']
        # The year turns at bud burst: last year's leaves (fallen, or the dead ones an oak keeps) give way to the
        # unfolding ones, which grow to full size over their first weeks.
        k['leaf_out'] = [(b - 2, 1.0), (b - 1, 0.0), (u, 0.45), (f, 1.0)]
        k['leaf_grow'] = [(b - 2, 1.0), (b - 1, 0.3), (f, 0.85), (f + 20, 1.0)]
        k['autumn'] = [(b - 2, 1.0), (b - 1, 0.0), (a0, 0.0), (ap, 0.65), (l1, 1.0)]
        keep = 0.12 if ex.get('marcescent') else 1.0
        k['fallen'] = [(b - 2, keep), (b - 1, 0.0), (l0, 0.0), (l1, keep)]
    fs, fe = ph.get('flower_start'), ph.get('flower_end')
    draw = SP[sid]['draw']
    if fs and fe and draw.get('bloom_shape', 'none') != 'none':
        k['bloom'] = [(fs - 12, 0.0), (fs + 3, 1.0), (fe - 3, 1.0), (fe + 8, 0.0)]
        k['bloom_stage'] = [(fs - 12, 0.0), (fs + 2, 1.0), (fe - 2, 1.0), (fe + 6, 2.0)]
    else:
        k['bloom'] = [(1, 0.0)]
        k['bloom_stage'] = [(1, 0.0)]
    r = ph.get('fruit_ripe')
    if draw.get('fruit_shape', 'none') != 'none' and fe and r:
        # A long flowering (dewberry's runs on past its first ripe fruit) sets its fruit from its early flowers.
        fset = min(fe, r - 20)
        set_ = fset + 10
        # Ripeness 0 .. 0.7 while the fruit grows, 0.7 .. 1 while it colours: a fruit stays green until its last
        # weeks (a third of its time, two weeks at least); the bloom shaders read the two stretches apart.
        turn = wrap(max(fset + 5, r - COLOUR_DAYS.get(sid, max(14, 0.3 * (r - fset)))))
        fc = FRUIT_CROP.get(sid)
        if fc and ex.get('fruit_winter'):
            # Its sites hold their groups to the bud burst; the birds take the fruits one by one (fruit_keep).
            b = ph.get('bud_burst') or 100
            k['fruit'] = [(fset, 0.0), (set_, fc['sites']), (wrap(b - 8), fc['sites']), (wrap(b), 0.0)]
            # (The frosts darken what is left: a hawthorn's haws maroon by January.)
            k['ripe'] = [(fset, 0.0), (turn, 0.7), (r, 1.0), (wrap(r + 60), 1.3), (20, 1.85), (wrap(b - 5), 1.9), (wrap(b), 0.0)]
            k['fruit_keep'] = [(wrap(fset), fc['set'])] + [(wrap(b if d == 'b' else d), fc['set'] * v) for d, v in fc['winter']]
        elif ex.get('fruit_winter'):
            b = ph.get('bud_burst') or 100
            k['fruit'] = [(fset, 0.0), (set_, 1.0), (wrap(r + 60), 1.0), (wrap(b - 20), 0.35), (wrap(b), 0.0)]
            k['ripe'] = [(fset, 0.0), (turn, 0.7), (r, 1.0), (wrap(r + 90), 1.4), (wrap(b - 5), 1.8), (wrap(b), 0.0)]
        elif draw.get('fruit_shape') == 'cotton':
            k['fruit'] = [(fset, 0.0), (set_, 1.0), (r + 12, 1.0), (r + 25, 0.0)]
            k['ripe'] = [(fset, 0.0), (turn, 0.7), (r, 1.0), (r + 25, 1.0), (r + 30, 0.0)]
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
    k['autumn_spread'] = [(1, 0.45 if ex.get('autumn_margin') else 0.35)]
    k.setdefault('fruit_keep', [(1, 1.0)])
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
# Communities
# ======================================================================================================
# The shrub communities of the zone (the flora research's ten, SHRUBS.md 2; shares in per cent), with the species
# of the cards; a share of a group spread over its members.
COMMUNITIES = {
    'pine_moist': {'fra_aln': 50, 'rub_ida': 15, 'sal_aur': 10, 'cor_ave': 8, 'jun_com': 6, 'euo_ver': 6, 'ame_spi': 3,
                   'lon_xyl': 2},
    'pine_dry': {'cha_rut': 32, 'jun_com': 14, 'cyt_sco': 14, 'fra_aln': 12, 'euo_ver': 10, 'ame_spi': 8, 'amo_fru': 5,
                 'ros_can': 5},
    'oak_hazel': {'cor_ave': 42, 'euo_ver': 15, 'euo_eur': 12, 'fra_aln': 10, 'vib_opu': 7, 'lon_xyl': 6, 'pad_avi': 5,
                  'rub_cae': 3},
    'willow_wet': {'sal_cin': 55, 'fra_aln': 20, 'sal_aur': 10, 'sal_pen': 6, 'vib_opu': 5, 'rib_nig': 4},
    'willow_river': {'sal_acu': 30, 'sal_tri': 24, 'sal_vim': 16, 'sal_ros': 14, 'hip_rha': 10, 'sal_cap': 6},
    'alder_swamp': {'rub_cae': 30, 'fra_aln': 28, 'rib_nig': 12, 'vib_opu': 10, 'pad_avi': 10, 'sal_cin': 10},
    'village': {'syr_vul': 22, 'sam_nig': 20, 'pru_cer': 15, 'rib_nig': 8, 'rib_uva': 6, 'phi_cor': 6, 'ros_can': 6,
                'ame_spi': 5, 'ros_maj': 4, 'euo_eur': 4, 'rub_ida': 4},
    'old_field': {'ros_can': 20, 'cra_mon': 15, 'sal_cap': 12, 'sal_cin': 10, 'rub_cae': 10, 'fra_aln': 10, 'sam_nig': 8,
                  'pru_cer': 7, 'cyt_sco': 4, 'amo_fru': 4},
    'burn': {'rub_ida': 32, 'sam_rac': 18, 'sal_cap': 15, 'fra_aln': 15, 'rub_cae': 10, 'sal_cin': 10},
    'hedgerow': {'ros_can': 22, 'cra_mon': 16, 'sam_nig': 15, 'pru_cer': 12, 'amo_fru': 10, 'ame_spi': 8, 'rub_ida': 6,
                 'cyt_sco': 6, 'ros_maj': 5},
}
# The grass community under a bush (dead_air_x64_seasons.ltx) -> the shrub community there.
GRASS_TO_SHRUB = {'old_field': 'old_field', 'mesic_meadow': 'hedgerow', 'wet_meadow': 'willow_wet',
                  'sedge_marsh': 'willow_wet', 'reed_bed': 'willow_wet', 'sand': 'willow_river',
                  'ruderal_verge': 'hedgerow', 'lawn': 'village', 'forest_floor': 'pine_moist',
                  'forb_meadow': 'hedgerow', 'tall_herbs': 'village', 'wet_margin': 'willow_river',
                  'forb_fallow': 'old_field'}
# Where no grass grows under a bush (closed forest): its class decides; the Red Forest's red stock is burn regrowth.
CLASS_TO_SHRUB = {'BUSH': 'pine_moist', 'BUSH_GROUP': 'oak_hazel', 'THICKET': 'willow_wet', 'CONIFER_BUSH': 'pine_dry'}
RED_COMMUNITY = 'burn'


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
              'catkin_share', 'catkin_len', 'catkin_droop', 'max_leaves']
DRAW_ORDER = ['leaf_outline', 'leaflet_ratio', 'translucency', 'gloss', 'flutter', 'fold', 'underside', 'jitter',
              'bloom_shape', 'petals', 'fruit_shape', 'fruits', 'catkin_shape', 'bark', 'needle_density', 'evergreen',
              'lobes', 'autumn_mix']


def fmt(v):
    if isinstance(v, (list, tuple)):
        return ', '.join(fmt(x) for x in v)
    if isinstance(v, float):
        return ('%.4f' % v).rstrip('0').rstrip('.') if abs(v) < 100 else '%g' % v
    return str(v)


def species_section(n, sid):
    s = SP[sid]
    fl = FLORA_BY_ID.get(sid, {})
    a = dict(s['arch'])
    a['href'] = round(sum(s['h']) / 2.0, 2)
    a['max_leaves'] = leaf_budget(sid)
    lines = ['[shrub_species_%d]' % n, 'name = %s' % sid]
    latin = fl.get('latin') or {'lon_xyl': 'Lonicera xylosteum L.'}.get(sid, sid)
    lines.append('; %s' % latin.replace('\u0142', 'l').encode('ascii', 'replace').decode('ascii'))
    lines.append('height = %s' % fmt([float(x) for x in s['h']]))
    lines.append('width = %s' % fmt([float(x) for x in s['w']]))
    lines.append('classes = %s' % ', '.join(s['classes']))
    lines.append('dead = 0.04')
    lines.append('spread_days = 6')
    lines.append('; architecture (da_shrub_gen.h)')
    for k in ARCH_ORDER:
        if k in a and a[k] is not None:
            lines.append('%s = %s' % (k, fmt(a[k])))
    for i, lv in enumerate(('l1', 'l2', 'l3')):
        if i >= a.get('levels', 2) or not a.get(lv):
            continue
        for k in LEVEL_ORDER:
            lines.append('%s_%s = %s' % (lv, k, fmt(a[lv][k])))
    for k in LEAF_ORDER:
        if k in a:
            lines.append('%s = %s' % (k, fmt(a[k])))
    d = dict(s['draw'])
    d.setdefault('outline', 'ovate')
    lines.append('; drawing (r3\\da_shrub_common.h shrub_prof)')
    ex = s['extra']
    ck = colour_keys(sid)
    # Autumn: the share of colour B (the photos' second kind), its pull to the lit leaves, the margin first, browning.
    autumn_mix = [ck['b_share'], 0.5, 0.5 if ex.get('autumn_margin') else 0.3, 0.35]
    # A spray leaf's width over its length from its sizes, a compound leaf's leaflet from the flora.
    # (A simple leaf's own, which the clusters draw their leaves by.)
    ratio = d.get('leaflet_ratio', 0.55)
    if a.get('leaf_kind', 'simple') in ('spray', 'simple'):
        ratio = round(min(max(mean(a['leaf_wid']) / mean(a['leaf_len']), 0.08), 0.95), 3)
    # A needle species' clusters draw its sprig texture: the sprig's own proportions, not a needle's.
    sprig = LEAF_TEX.get('species', {}).get(sid, {}).get('a', -1)
    if a.get('leaf_kind') == 'needles' and 0 <= sprig < len(LEAF_TEX.get('layers', [])):
        bl, bw = LEAF_TEX['layers'][sprig]['box_mm']
        ratio = round(min(max(bw / bl, 0.08), 0.95), 3)
    # A compound leaf folds along its rachis and a lobed one along its midrib as a blade does (they are drawn folded
    # near): at least a seventh of a right angle, a compound one a quarter.
    fold = d['fold']
    if a.get('leaf_kind') == 'compound':
        fold = max(fold, 0.25)
    elif d.get('lobes', 0) > 0:
        fold = max(fold, 0.2)
    vals = {'leaf_outline': d['outline'], 'leaflet_ratio': ratio, 'translucency': d['translucency'],
            'gloss': GLOSS.get(sid, d['gloss']), 'flutter': d['flutter'],
            'fold': fold, 'underside': d['underside'],
            'jitter': [max(d['jitter'][0], JITTER_MIN[0]), max(d['jitter'][1], JITTER_MIN[1])],
            'bloom_shape': d.get('bloom_shape', 'none'), 'petals': d.get('petals', 5),
            'fruit_shape': d.get('fruit_shape', 'none'), 'fruits': d.get('fruits', 1),
            'catkin_shape': d.get('catkin_shape', 'none'),
            'bark': BARK.get(sid, [0.6, 0.1, 0.0]), 'needle_density': d.get('needle_density', 1.0),
            'evergreen': 1 if ex.get('evergreen') else 0, 'lobes': d.get('lobes', 0), 'autumn_mix': autumn_mix}
    for k in DRAW_ORDER:
        lines.append('%s = %s' % (k, fmt(vals[k])))
    lines += texture_lines(sid)
    sk = season_keys(sid)
    lines.append('; season keys (day of year: value; colours sRGB)')
    for k in ('leaf_out', 'leaf_grow', 'autumn', 'fallen', 'bloom', 'bloom_stage', 'fruit', 'ripe', 'catkin', 'catkin_grow',
              'autumn_spread', 'fruit_keep'):
        if k in sk:
            lines.append('%s = %s' % (k, fmt_keys(sk[k])))
    for k in ('c_young', 'c_summer', 'c_autumn_a', 'c_autumn_b', 'c_dead', 'c_under', 'c_bark', 'c_twig', 'c_petal',
              'c_centre', 'c_fruit_unripe', 'c_fruit_ripe', 'c_catkin'):
        lines.append('%s = %s' % (k, fmt_keys(ck[k], colour=True)))
    return lines


def kind_lines():
    """[shrub_kinds]: every key of every family of the replaced classes; [shrub_families]: the signatures."""
    kinds, fams = [], {}
    for key, v in sorted(KINDS['kinds'].items()):
        clumps = ['%g:%g:%g:%g' % tuple(c[:4]) for c in v['clumps_local']] if v['class'] in ('BUSH_GROUP', 'THICKET') else []
        flags = (['dry'] if v['dry'] else []) + (['red'] if v['red'] else [])
        for member in sorted(set(v['family_members']) | {key}):
            kinds.append('%s = %s' % (member, ', '.join([v['class']] + flags + clumps)))
        # The signature: the parts sorted by shader (case-insensitive), FNV-1a 64 over (shader lower-case, 0, u32
        # triangle count); the box of all parts in mm and their area.
        parts = sorted(v['signature'], key=lambda p: p['shader'].lower())
        h = 0xcbf29ce484222325
        for p in parts:
            for byte in p['shader'].lower().encode('cp1251') + b'\0' + int(p['tris']).to_bytes(4, 'little'):
                h = ((h ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        bmin = [min(p['box_min_mm'][i] for p in parts) for i in range(3)]
        bmax = [max(p['box_max_mm'][i] for p in parts) for i in range(3)]
        area = sum(p['area_m2'] for p in parts)
        fams.setdefault('%016x' % h, []).append('%s:%d:%d:%d:%d:%d:%d:%.4f' % (key, *bmin, *bmax, area))
    kinds = sorted(set(kinds))
    fam_lines = ['%s = %s' % (k, ', '.join(sorted(v))) for k, v in sorted(fams.items())]
    return kinds, fam_lines


def main():
    ids = list(SP.keys())
    assert len(ids) <= MAX_SPECIES, len(ids)
    out = ['; Dead Air: Refined 1.6.0 - the procedural shrubs (docs/dead-air/SHRUBS.md, SHRUB_DATA.md).',
           '; Written by tools/shrubs/make_shrubs.py from the ChEZ research of 07.10.2026; edit the script, not this file.',
           '']
    for n, sid in enumerate(ids, 1):
        out += species_section(n, sid) + ['']
    for n, (name, shares) in enumerate(COMMUNITIES.items(), 1):
        sp = ['%s:%g' % (k, v) for k, v in sorted(shares.items(), key=lambda kv: -kv[1]) if v > 0 and k in SP]
        out += ['[shrub_community_%d]' % n, 'name = %s' % name, 'species = %s' % ', '.join(sp), '']
    out += ['[shrub_grass_community]'] + ['%s = %s' % kv for kv in GRASS_TO_SHRUB.items()] + ['']
    out += ['[shrub_class_community]'] + ['%s = %s' % kv for kv in CLASS_TO_SHRUB.items()] + ['']
    # A conifer bush taller than a juniper grows (3 m and a quarter) is a young pine or spruce: it stays stock until
    # the trees are replaced.
    out += ['[shrub_policy]', 'max_top = BUSH:8.5, BUSH_GROUP:8.5, CONIFER_BUSH:4.7, THICKET:10.5', 'min_scale = 0.05',
            'spacing = 2.6', 'dry_dead = 0.12', 'red_community = %s' % RED_COMMUNITY, '']
    kinds, fams = kind_lines()
    out += ['[shrub_kinds]'] + kinds + ['', '[shrub_families]'] + fams + ['']
    text = '\r\n'.join(out)
    tmp = OUT_LTX + '.tmp'
    open(tmp, 'wb').write(text.encode('cp1251'))
    os.replace(tmp, OUT_LTX)
    write_doc(ids)
    print('species %d, communities %d, kinds %d, families %d -> %s' % (len(ids), len(COMMUNITIES), len(kinds), len(fams), OUT_LTX))


def write_doc(ids):
    lines = ['# Shrub data (generated)', '',
             'Written by `tools/shrubs/make_shrubs.py` into `configs/dead_air_x64_shrubs.ltx` (docs/dead-air/SHRUBS.md).', '',
             '| # | id | species | height (m) | classes | leaves | flowers | fruit | catkins | leaf out / full | flowers | ripe | autumn | bare |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for n, sid in enumerate(ids, 1):
        s = SP[sid]
        fl = FLORA_BY_ID.get(sid, {})
        ph = fl.get('phenology', {})
        a = s['arch']
        leaf = a.get('leaf_kind', 'simple')
        if leaf == 'compound':
            leaf += ' (%d)' % a.get('leaflets', 0)
        d = s['draw']
        lines.append('| %d | %s | *%s* | %s | %s | %s %s | %s | %s | %s | %s / %s | %s-%s | %s | %s | %s |' % (
            n, sid, (fl.get('latin') or 'Lonicera xylosteum').split(' (')[0], '-'.join('%g' % x for x in s['h']),
            ', '.join(s['classes']), leaf, d.get('outline', ''), d.get('bloom_shape', ''), d.get('fruit_shape', ''),
            d.get('catkin_shape', ''), ph.get('leaf_unfold', '-'), ph.get('leaf_full', '-'), ph.get('flower_start', '-'),
            ph.get('flower_end', '-'), ph.get('fruit_ripe', '-'), ph.get('autumn_peak', '-'), ph.get('leaf_fall_end', '-')))
    lines += ['', '## Communities', '', '| community | species (share %) | grows where the grass is |', '|---|---|---|']
    for name, shares in COMMUNITIES.items():
        g = ', '.join(k for k, v in GRASS_TO_SHRUB.items() if v == name)
        c = ', '.join(k for k, v in CLASS_TO_SHRUB.items() if v == name)
        lines.append('| %s | %s | %s%s |' % (name, ', '.join('%s %g' % (k, v) for k, v in sorted(shares.items(), key=lambda kv: -kv[1]) if v > 0),
                                          g, ('; default of ' + c) if c else ''))
    tmp = OUT_MD + '.tmp'
    open(tmp, 'wb').write(('\r\n'.join(lines) + '\r\n').encode('utf-8'))
    os.replace(tmp, OUT_MD)


if __name__ == '__main__':
    main()
