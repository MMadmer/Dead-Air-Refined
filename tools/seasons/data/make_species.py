"""Write tools/seasons/data/species.json and communities.json: the procedural plants of the ChEZ (64 species) and the
plant communities the stock detail objects grow (docs/dead-air/GRASS.md "Species and communities").

Sources: scratchpad research of 05-06.10.2026 (species.md: Didukh 2023 constancy table, FloraVeg/de.wiki traits,
USGS colour references), the species research of 06.10.2026 (traits.json: morphology and ChEZ phenology,
colours.json: dated iNaturalist photos of Ukraine/Belarus/Poland measured per phenophase) and its round 4 of
07.10.2026 (the 24 forbs of the Didukh 2023 constancy matrix the first 40 lacked, photographed the same way, with a
blue/violet flower class). Heights are the game
convention of the blade grass: about two thirds of the measured stands for the tall species.

    python tools/seasons/data/make_species.py      rewrite species.json and communities.json
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# Defaults per form: 0 graminoid, 1 forb (a leafy stem), 2 rosette forb, 3 rush.
BASE = dict(
    outline=0, leaf_h=[0.2, 0.4], leaf_w=[0.003, 0.006], leaf_tilt=[12, 38], leaf_bend=[0.15, 0.32],
    leaf_droop=[0.05, 0.18], leaf_stiff=0.4, culm_h=[0.45, 0.8], culm_w=[0.002, 0.003], culm_tilt=[2, 9],
    culm_bend=[0.02, 0.07], culm_stiff=0.55, culm_knee=0.0, culm_leaves=0, cauline_h=[0.1, 0.2],
    cauline_w=[0.005, 0.01], cauline_tilt=[40, 70], cauline_span=[0.25, 0.76], cauline_taper=1.0, cauline_arrange=0,
    cauline_bend=0.3, cauline_droop=0.2, basal_h=[0.1, 0.24], basal_w=[0.003, 0.006], basal_tilt=[22, 50],
    basal_bend=[0.15, 0.35], basal_droop=[0.1, 0.28], clump_cell=0.25, clump_pull=0.35, clump_face_away=0.3,
    clump_lean=5, density_leaf=400, density_culm=50, density_basal=1500, density_cal=0.95, cover=1.0,
    shade_density=1.0, shade_height=1.0, head_type=4, head_len=[0.06, 0.12], head_w=[0.03, 0.06], head_branches=4,
    head_open=[45, 25], head_grain=1.0, head_petals=0, head_share=0.35, head_nod=[0, 0], fold_h=0.2, tussock_h=[0, 0],
    tussock_share=0.0, tussock_r=0.0, stem_tint=[1.0, 1.0, 1.0], winter_stand=0.0)
FORB = dict(BASE, leaf_h=[0.08, 0.15], leaf_w=[0.02, 0.04], leaf_tilt=[30, 60], leaf_bend=[0.15, 0.3],
            leaf_droop=[0.05, 0.15], culm_tilt=[1, 6], culm_bend=[0.02, 0.05], culm_stiff=0.75, density_basal=0,
            clump_cell=0.25, clump_pull=0.15, clump_face_away=0.2, clump_lean=3, head_share=0.85, fold_h=0.0,
            outline=1, cauline_arrange=1, cauline_taper=0.6)


def sp(id_, latin, form, template, colours, ramp, phen=None, **kw):
    base = BASE if form in (0, 3) else FORB
    d = dict(base)
    d.update(kw)
    d.update(id=id_, latin=latin, form=form, template=template, colours=colours, seed_ramp=ramp)
    if phen:
        d['phen'] = phen
    return d


def col(live, spring, autumn, yellowing, straw, thatch):
    return dict(live=live, live_spring=spring, live_autumn=autumn, yellowing=yellowing, straw=straw, thatch=thatch)


def ramp(a, b, c, d):
    return {'0.25': a, '0.5': b, '0.75': c, '1.0': d}


def phen(up, full, bud, flower, ripe, end, sen0, sen1, stand, lodge, winter_green=0.0, climate='P2_MEADOW_MIX'):
    """Forb phenology, days of the year (0 = 1 Jan): emergence, full height, buds, anthesis, ripe, heads weathered;
    leaves senesce from sen0 to sen1; stand = share of last year's stems still standing in early spring, lodge = the
    share of them lying; winter_green = green share of the overwintering rosettes; climate = the template whose
    wetness, frost and flood keys the species takes."""
    return dict(up=up, full=full, bud=bud, flower=flower, ripe=ripe, end=end, sen0=sen0, sen1=sen1, stand=stand,
                lodge=lodge, winter_green=winter_green, climate=climate)


S = [
    # ---- open graminoids --------------------------------------------------------------------------------------
    sp('cal_epi', 'Calamagrostis epigejos', 0, 'P1_TALL_CALAMAGROSTIS',
       col('#4F5C36', '#586637', '#535834', '#7A7240', '#9A8659', '#7E7462'),
       ramp('#6C6E58', '#74586A', '#937A5E', '#A39A86'),
       leaf_h=[0.32, 0.62], leaf_w=[0.005, 0.010], leaf_tilt=[10, 32], leaf_bend=[0.12, 0.30], leaf_droop=[0.05, 0.2],
       leaf_stiff=0.55, culm_h=[0.62, 0.98], culm_w=[0.0024, 0.0034], culm_tilt=[2, 8], culm_bend=[0.02, 0.06],
       culm_stiff=0.7, basal_h=[0.12, 0.28], basal_w=[0.004, 0.008], basal_tilt=[22, 48], clump_cell=0.4,
       clump_pull=0.25, clump_face_away=0.25, density_leaf=430, density_culm=55, density_basal=2000, head_type=6,
       head_len=[0.13, 0.22], head_w=[0.022, 0.045], head_branches=6, head_open=[11, 5], head_share=0.32,
       fold_h=0.15, shift=dict(heads=0, green=0)),
    sp('ely_rep', 'Elytrigia repens', 0, 'P2_MEADOW_MIX',
       col('#4E6439', '#577038', '#55603A', '#7D7444', '#A08B5E', '#7A6F5C'),
       ramp('#6D7A5A', '#77805F', '#9C8C66', '#8A806C'),
       leaf_h=[0.22, 0.45], leaf_w=[0.004, 0.009], leaf_tilt=[15, 42], leaf_bend=[0.15, 0.35], leaf_droop=[0.08, 0.22],
       leaf_stiff=0.35, culm_h=[0.5, 0.85], culm_w=[0.0022, 0.003], basal_h=[0.1, 0.24], basal_w=[0.004, 0.007],
       clump_cell=0.3, clump_pull=0.15, clump_face_away=0.2, density_leaf=460, density_culm=60, density_basal=1600,
       head_type=1, head_len=[0.07, 0.14], head_w=[0.006, 0.011], head_branches=0, head_open=[0, 0],
       head_share=0.4, shift=dict(heads=0, green=0)),
    sp('dac_glo', 'Dactylis glomerata', 0, 'P2_MEADOW_MIX',
       col('#4D6040', '#576B40', '#4F5A3A', '#7C7646', '#A28D62', '#7C705C'),
       ramp('#6A7A5E', '#848268', '#9D8A68', '#857A66'),
       leaf_h=[0.25, 0.5], leaf_w=[0.005, 0.011], leaf_tilt=[14, 45], leaf_bend=[0.18, 0.38], leaf_droop=[0.1, 0.28],
       leaf_stiff=0.35, culm_h=[0.55, 0.95], culm_w=[0.0025, 0.0035], basal_h=[0.12, 0.3], basal_w=[0.005, 0.01],
       clump_cell=0.32, clump_pull=0.65, clump_face_away=0.7, density_leaf=400, density_culm=45, density_basal=1800,
       head_type=3, head_len=[0.07, 0.14], head_w=[0.03, 0.06], head_branches=3, head_open=[35, 15],
       head_share=0.45, shift=dict(heads=-8, green=0)),
    sp('poa_pra', 'Poa pratensis', 0, 'P2_MEADOW_MIX',
       col('#4A6634', '#557539', '#4C5F33', '#7E7A45', '#A08C60', '#7A6E5A'),
       ramp('#6A7756', '#776A6E', '#9C8C6B', '#8C826E'),
       leaf_h=[0.18, 0.4], leaf_w=[0.002, 0.004], leaf_tilt=[15, 45], leaf_bend=[0.18, 0.35], leaf_droop=[0.1, 0.25],
       leaf_stiff=0.3, culm_h=[0.35, 0.65], culm_w=[0.0015, 0.0022], basal_h=[0.08, 0.22], basal_w=[0.002, 0.004],
       clump_cell=0.15, clump_pull=0.3, density_leaf=700, density_culm=60, density_basal=1800, head_type=4,
       head_len=[0.05, 0.12], head_w=[0.03, 0.06], head_branches=4, head_open=[50, 30], head_share=0.35,
       fold_h=0.25, shift=dict(heads=-15, green=-5)),
    sp('poa_ang', 'Poa angustifolia', 0, 'P2_MEADOW_MIX',
       col('#536845', '#5D7140', '#56603F', '#807745', '#A89464', '#7F735E'),
       ramp('#6E745C', '#786A6A', '#A08F6C', '#8F8570'),
       leaf_h=[0.18, 0.38], leaf_w=[0.0012, 0.002], leaf_tilt=[10, 35], leaf_bend=[0.15, 0.32], leaf_droop=[0.08, 0.2],
       culm_h=[0.35, 0.6], culm_w=[0.0015, 0.002], basal_h=[0.08, 0.2], basal_w=[0.0012, 0.002], clump_cell=0.15,
       clump_pull=0.35, density_leaf=800, density_culm=55, density_basal=1500, head_type=4, head_len=[0.04, 0.09],
       head_w=[0.015, 0.03], head_branches=4, head_open=[35, 20], head_share=0.35, fold_h=0.25,
       shift=dict(heads=-18, green=-5)),
    sp('fes_rub', 'Festuca rubra', 0, 'P2_MEADOW_MIX',
       col('#44593A', '#4E663A', '#4A5536', '#7C7044', '#A08A60', '#7C6D58'),
       ramp('#6C6450', '#7A5A58', '#998265', '#8A7C6A'),
       leaf_h=[0.16, 0.36], leaf_w=[0.0008, 0.0016], leaf_tilt=[10, 40], leaf_droop=[0.08, 0.2], leaf_stiff=0.35,
       culm_h=[0.35, 0.6], culm_w=[0.0012, 0.0018], basal_h=[0.08, 0.2], basal_w=[0.0008, 0.0015], clump_cell=0.18,
       clump_pull=0.4, density_leaf=900, density_culm=55, density_basal=1400, head_type=4, head_len=[0.05, 0.1],
       head_w=[0.012, 0.025], head_branches=4, head_open=[25, 12], head_share=0.35, fold_h=0.25,
       stem_tint=[1.08, 0.92, 0.9], shift=dict(heads=-10, green=0)),
    sp('fes_ovi', 'Festuca ovina agg.', 0, 'P6_SPARSE_SAND',
       col('#66755C', '#6B7A5A', '#63705A', '#858058', '#A9986E', '#85796A'),
       ramp('#73766A', '#776A70', '#A3976F', '#948B78'),
       outline=3, leaf_h=[0.08, 0.2], leaf_w=[0.0006, 0.001], leaf_tilt=[8, 40], leaf_bend=[0.08, 0.2],
       leaf_droop=[0, 0.08], leaf_stiff=0.7, culm_h=[0.2, 0.42], culm_w=[0.001, 0.0014], basal_h=[0.05, 0.12],
       basal_w=[0.0006, 0.001], clump_cell=0.18, clump_pull=0.85, clump_face_away=0.85, cover=0.75,
       density_leaf=950, density_culm=50, density_basal=500, head_type=4, head_len=[0.03, 0.07],
       head_w=[0.008, 0.016], head_branches=4, head_open=[20, 8], head_share=0.45, fold_h=0.3,
       shift=dict(heads=-20, green=0)),
    sp('agr_cap', 'Agrostis capillaris', 0, 'P2_MEADOW_MIX',
       col('#4B6236', '#567038', '#4D5A34', '#847744', '#A48F62', '#7C6E59'),
       ramp('#6E6F57', '#7E5761', '#93785E', '#857462'),
       leaf_h=[0.12, 0.3], leaf_w=[0.0015, 0.004], leaf_tilt=[15, 45], leaf_bend=[0.18, 0.35], leaf_droop=[0.1, 0.25],
       leaf_stiff=0.3, culm_h=[0.3, 0.55], culm_w=[0.001, 0.0016], basal_h=[0.06, 0.18], basal_w=[0.0015, 0.003],
       clump_cell=0.16, density_leaf=700, density_culm=70, density_basal=1400, head_type=5, head_len=[0.06, 0.14],
       head_w=[0.03, 0.07], head_branches=6, head_open=[45, 25], head_share=0.5, fold_h=0.2,
       shift=dict(heads=15, green=0)),
    sp('agr_gig', 'Agrostis gigantea', 0, 'P2_MEADOW_MIX',
       col('#4C6238', '#577038', '#4F5B37', '#807644', '#A48F62', '#7C6F5A'),
       ramp('#6C7058', '#876B6E', '#8F8270', '#8A806F'),
       leaf_h=[0.2, 0.42], leaf_w=[0.003, 0.007], leaf_tilt=[15, 42], leaf_bend=[0.18, 0.35], leaf_droop=[0.1, 0.25],
       leaf_stiff=0.3, culm_h=[0.45, 0.8], culm_w=[0.0018, 0.0026], basal_h=[0.1, 0.22], basal_w=[0.003, 0.006],
       clump_cell=0.22, clump_pull=0.3, density_leaf=520, density_culm=55, density_basal=1500, head_type=5,
       head_len=[0.08, 0.16], head_w=[0.04, 0.08], head_branches=6, head_open=[45, 25], head_share=0.45,
       shift=dict(heads=12, green=0)),
    sp('phl_pra', 'Phleum pratense', 0, 'P2_MEADOW_MIX',
       col('#4C6040', '#567039', '#505B38', '#827745', '#A68F63', '#7D705B'),
       ramp('#6E7A60', '#7E7A6E', '#A08E68', '#8D826E'),
       leaf_h=[0.2, 0.4], leaf_w=[0.004, 0.008], leaf_tilt=[12, 38], leaf_bend=[0.15, 0.3], culm_h=[0.5, 0.85],
       culm_w=[0.0022, 0.003], culm_knee=0.05, basal_h=[0.1, 0.22], basal_w=[0.004, 0.007], clump_cell=0.22,
       clump_pull=0.45, clump_face_away=0.4, density_leaf=430, density_culm=55, density_basal=1500, head_type=2,
       head_len=[0.05, 0.11], head_w=[0.006, 0.009], head_branches=0, head_open=[0, 0], head_share=0.5,
       shift=dict(heads=8, green=0)),
    sp('alo_pra', 'Alopecurus pratensis', 0, 'P3_WET_MEADOW',
       col('#4A6236', '#557437', '#4E5A35', '#807544', '#A08A5E', '#7A6D59'),
       ramp('#6C7A5E', '#77706E', '#8E8670', '#85796A'),
       leaf_h=[0.2, 0.4], leaf_w=[0.004, 0.008], leaf_tilt=[14, 42], leaf_bend=[0.18, 0.35], leaf_droop=[0.08, 0.22],
       leaf_stiff=0.3, culm_h=[0.4, 0.75], culm_w=[0.002, 0.0028], basal_h=[0.1, 0.24], basal_w=[0.004, 0.007],
       clump_cell=0.22, clump_pull=0.45, density_leaf=460, density_culm=50, density_basal=1700, head_type=2,
       head_len=[0.04, 0.09], head_w=[0.007, 0.01], head_branches=0, head_open=[0, 0], head_grain=1.2,
       head_share=0.5, shift=dict(heads=-30, green=-5)),
    sp('bro_ine', 'Bromopsis inermis', 0, 'P2_MEADOW_MIX',
       col('#4E6446', '#57703F', '#535D3A', '#7F7444', '#A38C60', '#7B6E59'),
       ramp('#6F7A5D', '#7E6A6A', '#A08B68', '#8C7F6A'),
       leaf_h=[0.25, 0.48], leaf_w=[0.006, 0.012], leaf_tilt=[15, 45], leaf_bend=[0.2, 0.38], leaf_droop=[0.1, 0.25],
       leaf_stiff=0.3, culm_h=[0.55, 0.9], culm_w=[0.0025, 0.0035], basal_h=[0.12, 0.26], basal_w=[0.005, 0.009],
       clump_cell=0.35, clump_pull=0.2, density_leaf=420, density_culm=45, density_basal=1500, head_type=4,
       head_len=[0.1, 0.17], head_w=[0.04, 0.08], head_branches=4, head_open=[35, 20], head_nod=[5, 15],
       head_share=0.4, shift=dict(heads=2, green=0)),
    sp('des_ces', 'Deschampsia cespitosa', 0, 'P3_WET_MEADOW',
       col('#3F5733', '#4A6436', '#46553A', '#7E7646', '#AE9866', '#8A7C64'),
       ramp('#66705A', '#7E7078', '#AE9C70', '#A09378'),
       leaf_h=[0.22, 0.42], leaf_w=[0.002, 0.004], leaf_tilt=[15, 50], leaf_bend=[0.2, 0.4], leaf_droop=[0.12, 0.3],
       culm_h=[0.55, 0.95], culm_w=[0.0015, 0.0022], basal_h=[0.1, 0.25], basal_w=[0.002, 0.004], clump_cell=0.45,
       clump_pull=0.75, clump_face_away=0.85, tussock_h=[0.05, 0.14], tussock_share=0.6, tussock_r=0.2,
       density_leaf=650, density_culm=45, density_basal=1800, head_type=5, head_len=[0.15, 0.3],
       head_w=[0.08, 0.15], head_branches=6, head_open=[45, 30], head_share=0.55, shift=dict(heads=8, green=0)),
    sp('cor_can', 'Corynephorus canescens', 0, 'P6_SPARSE_SAND',
       col('#6E7A66', '#707D63', '#6C7763', '#8A8460', '#ADA078', '#8A8070'),
       ramp('#747264', '#7C6476', '#A49A84', '#9A9283'),
       outline=3, leaf_h=[0.06, 0.16], leaf_w=[0.0005, 0.0009], leaf_tilt=[10, 45], leaf_bend=[0.08, 0.2],
       leaf_droop=[0, 0.08], leaf_stiff=0.7, culm_h=[0.12, 0.28], culm_w=[0.0008, 0.0012], culm_tilt=[5, 18],
       culm_knee=0.1, basal_h=[0.04, 0.1], basal_w=[0.0005, 0.0008], clump_cell=0.2, clump_pull=0.9,
       clump_face_away=0.95, cover=0.5, density_leaf=850, density_culm=60, density_basal=300, head_type=2,
       head_len=[0.025, 0.06], head_w=[0.006, 0.012], head_branches=0, head_open=[0, 0], head_share=0.55,
       fold_h=0.35, stem_tint=[1.1, 0.88, 0.9], shift=dict(heads=0, green=0)),
    sp('ani_tec', 'Anisantha tectorum', 0, 'P7_RUDERAL_VERGE',
       col('#5C6E45', '#637845', '#5A6A45', '#8A6E58', '#A99070', '#8A7A66'),
       ramp('#6E7A58', '#7A5E62', '#9C846A', '#A08E78'),
       leaf_h=[0.06, 0.15], leaf_w=[0.002, 0.004], leaf_tilt=[20, 50], leaf_bend=[0.2, 0.35], leaf_droop=[0.1, 0.2],
       leaf_stiff=0.25, culm_h=[0.22, 0.45], culm_w=[0.0012, 0.0018], culm_tilt=[3, 15], culm_knee=0.08,
       basal_h=[0.04, 0.1], basal_w=[0.002, 0.003], clump_cell=0.12, clump_pull=0.3, density_leaf=300,
       density_culm=120, density_basal=500, head_type=8, head_len=[0.06, 0.12], head_w=[0.03, 0.06],
       head_branches=4, head_open=[60, 90], head_nod=[20, 40], head_share=0.8, fold_h=0.2,
       shift=dict(heads=-30, green=-30)),
    # ---- wet ground ----------------------------------------------------------------------------------------
    sp('phr_aus', 'Phragmites australis', 0, 'P5_REED_BED',
       col('#5A6A4E', '#627348', '#6B6A48', '#9A8458', '#B09670', '#A58E6E'),
       ramp('#6E6E5C', '#6A5056', '#8C7A70', '#A89C8C'),
       outline=4, leaf_h=[0.18, 0.6], leaf_w=[0.01, 0.03], leaf_tilt=[30, 70], leaf_bend=[0.4, 0.4],
       leaf_droop=[0.4, 0.4], leaf_stiff=0.25, culm_h=[1.4, 2.3], culm_w=[0.006, 0.01], culm_tilt=[0, 5],
       culm_bend=[0.02, 0.06], culm_stiff=0.8, culm_leaves=6, cauline_h=[0.25, 0.45], cauline_w=[0.012, 0.028],
       cauline_tilt=[45, 75], cauline_span=[0.2, 0.85], cauline_taper=0.75, cauline_arrange=0, cauline_bend=0.4,
       cauline_droop=0.35, clump_cell=0.5, clump_pull=0.1, clump_face_away=0.0, clump_lean=3, density_leaf=0,
       density_culm=70, density_basal=0, head_type=7, head_len=[0.18, 0.32], head_w=[0.05, 0.12], head_branches=6,
       head_open=[30, 20], head_nod=[10, 30], head_share=0.75, fold_h=0.15, winter_stand=1.0,
       shift=dict(heads=0, green=0)),
    sp('pha_aru', 'Phalaris arundinacea', 0, 'P4_SEDGE_MARSH',
       col('#50664A', '#587045', '#5E6440', '#A08A55', '#B49C6E', '#9A8668'),
       ramp('#6E7A60', '#7A6A70', '#B09A68', '#A89878'),
       outline=4, leaf_h=[0.25, 0.45], leaf_w=[0.008, 0.015], leaf_tilt=[25, 55], leaf_bend=[0.3, 0.45],
       leaf_droop=[0.2, 0.35], leaf_stiff=0.3, culm_h=[0.8, 1.35], culm_w=[0.004, 0.006], culm_stiff=0.7,
       culm_leaves=5, cauline_h=[0.15, 0.3], cauline_w=[0.008, 0.016], cauline_tilt=[40, 65],
       cauline_span=[0.2, 0.8], cauline_taper=0.7, cauline_bend=0.35, cauline_droop=0.3, clump_cell=0.4,
       clump_pull=0.15, density_leaf=150, density_culm=60, density_basal=600, head_type=6, head_len=[0.1, 0.2],
       head_w=[0.02, 0.05], head_branches=6, head_open=[25, 8], head_share=0.55, shift=dict(heads=5, green=-5)),
    sp('gly_max', 'Glyceria maxima', 0, 'P4_SEDGE_MARSH',
       col('#4E6E3A', '#577A3A', '#5E6A3E', '#8E7E48', '#A58C60', '#7D6D58'),
       ramp('#6E7A58', '#77706A', '#8E7A5C', '#7D705E'),
       outline=4, leaf_h=[0.35, 0.6], leaf_w=[0.008, 0.016], leaf_tilt=[20, 50], leaf_bend=[0.3, 0.45],
       leaf_droop=[0.2, 0.4], leaf_stiff=0.2, culm_h=[0.8, 1.2], culm_w=[0.005, 0.008], culm_leaves=4,
       cauline_h=[0.2, 0.35], cauline_w=[0.008, 0.014], cauline_tilt=[40, 70], cauline_span=[0.2, 0.7],
       clump_cell=0.4, clump_pull=0.2, density_leaf=300, density_culm=35, density_basal=800, head_type=4,
       head_len=[0.15, 0.28], head_w=[0.08, 0.15], head_branches=6, head_open=[45, 30], head_share=0.4,
       shift=dict(heads=15, green=0)),
    sp('car_acu', 'Carex acuta', 0, 'P4_SEDGE_MARSH',
       col('#4A5C3E', '#55683E', '#6A6440', '#8A7A48', '#9A8458', '#7E6A52'),
       ramp('#4E5040', '#5C4A3C', '#5E4E3C', '#6A5C4A'),
       leaf_h=[0.4, 0.75], leaf_w=[0.004, 0.009], leaf_tilt=[15, 40], leaf_bend=[0.25, 0.45], leaf_droop=[0.2, 0.4],
       leaf_stiff=0.15, culm_h=[0.5, 0.9], culm_w=[0.003, 0.004], culm_tilt=[2, 8], culm_bend=[0.02, 0.08],
       basal_h=[0.16, 0.35], basal_w=[0.004, 0.008], basal_tilt=[25, 52], basal_bend=[0.25, 0.45],
       basal_droop=[0.2, 0.4], clump_cell=0.3, clump_pull=0.3, tussock_h=[0.04, 0.12], tussock_share=0.4,
       tussock_r=0.2, density_leaf=420, density_culm=25, density_basal=1000, head_type=9, head_len=[0.06, 0.13],
       head_w=[0.005, 0.009], head_branches=3, head_open=[20, 40], head_share=0.2, fold_h=0.15,
       shift=dict(heads=-20, green=0)),
    sp('car_hir', 'Carex hirta', 0, 'P3_WET_MEADOW',
       col('#55683F', '#5E7241', '#5E6240', '#857A46', '#9C875A', '#7C6C56'),
       ramp('#5E6046', '#6E5E46', '#74604A', '#726452'),
       leaf_h=[0.15, 0.32], leaf_w=[0.003, 0.006], leaf_tilt=[15, 45], leaf_bend=[0.2, 0.38], leaf_droop=[0.1, 0.25],
       leaf_stiff=0.25, culm_h=[0.25, 0.45], culm_w=[0.0018, 0.0025], basal_h=[0.08, 0.18], basal_w=[0.003, 0.005],
       clump_cell=0.25, clump_pull=0.25, density_leaf=500, density_culm=30, density_basal=900, head_type=9,
       head_len=[0.03, 0.07], head_w=[0.005, 0.008], head_branches=3, head_open=[20, 40], head_share=0.25,
       shift=dict(heads=-25, green=-5)),
    sp('jun_eff', 'Juncus effusus', 3, 'P4_SEDGE_MARSH',
       col('#44622F', '#4C6A33', '#4E5E34', '#7E7848', '#9C8A5C', '#7E7058'),
       ramp('#6A6A48', '#7A6A48', '#6E5A3E', '#5E5040'),
       culm_h=[0.4, 0.8], culm_w=[0.0018, 0.0028], culm_tilt=[4, 28], culm_bend=[0.05, 0.15], culm_stiff=0.6,
       clump_cell=0.35, clump_pull=0.9, clump_face_away=1.0, tussock_h=[0.03, 0.1], tussock_share=0.8,
       tussock_r=0.15, density_leaf=0, density_culm=320, density_basal=0, head_type=3, head_len=[0.02, 0.05],
       head_w=[0.01, 0.025], head_branches=3, head_open=[30, 30], head_grain=0.8, head_share=0.6,
       shift=dict(heads=10, green=10)),
    sp('typ_lat', 'Typha latifolia', 0, 'P5_REED_BED',
       col('#56694A', '#5E7148', '#8A7A50', '#9A8452', '#A88E64', '#957E62'),
       ramp('#5E6A48', '#6A5638', '#4E3A2A', '#8C7E6E'),
       outline=4, leaf_h=[0.9, 1.4], leaf_w=[0.012, 0.022], leaf_tilt=[5, 22], leaf_bend=[0.05, 0.2],
       leaf_droop=[0.05, 0.2], leaf_stiff=0.6, culm_h=[1.0, 1.6], culm_w=[0.006, 0.01], culm_tilt=[0, 4],
       culm_bend=[0.01, 0.03], culm_stiff=0.85, clump_cell=0.5, clump_pull=0.3, clump_face_away=0.6,
       density_leaf=110, density_culm=10, density_basal=0, head_type=2, head_len=[0.12, 0.22],
       head_w=[0.018, 0.028], head_branches=0, head_open=[0, 0], head_grain=1.5, head_share=0.6, winter_stand=1.0,
       shift=dict(heads=10, green=0)),
    # ---- forest floor ----------------------------------------------------------------------------------------
    sp('cal_aru', 'Calamagrostis arundinacea', 0, 'P1_TALL_CALAMAGROSTIS',
       col('#3E5530', '#4A6234', '#5A5A36', '#8A7844', '#A48D5E', '#7A6C56'),
       ramp('#66685A', '#6E5A60', '#8E7C5E', '#84786A'),
       leaf_h=[0.28, 0.5], leaf_w=[0.004, 0.009], leaf_tilt=[15, 45], leaf_bend=[0.2, 0.4], leaf_droop=[0.12, 0.3],
       leaf_stiff=0.35, culm_h=[0.55, 0.9], culm_w=[0.002, 0.003], basal_h=[0.12, 0.28], basal_w=[0.004, 0.008],
       clump_cell=0.3, clump_pull=0.55, clump_face_away=0.6, density_leaf=380, density_culm=40, density_basal=1500,
       shade_height=1.05, head_type=6, head_len=[0.1, 0.18], head_w=[0.02, 0.04], head_branches=6,
       head_open=[25, 10], head_share=0.3, shift=dict(heads=0, green=0)),
    sp('ave_fle', 'Avenella flexuosa', 0, 'P6_SPARSE_SAND',
       col('#4C6B36', '#567A38', '#566436', '#8A8046', '#B09C6A', '#8A7C62'),
       ramp('#6E7058', '#8A7680', '#A89670', '#988C74'),
       outline=3, leaf_h=[0.1, 0.25], leaf_w=[0.0005, 0.0009], leaf_tilt=[15, 50], leaf_bend=[0.2, 0.4],
       leaf_droop=[0.1, 0.3], leaf_stiff=0.35, culm_h=[0.3, 0.55], culm_w=[0.0008, 0.0012], culm_bend=[0.05, 0.12],
       basal_h=[0.06, 0.15], basal_w=[0.0005, 0.0009], clump_cell=0.2, clump_pull=0.75, clump_face_away=0.85,
       cover=0.9, density_leaf=900, density_culm=50, density_basal=900, head_type=5, head_len=[0.06, 0.12],
       head_w=[0.04, 0.08], head_branches=6, head_open=[40, 25], head_share=0.45, fold_h=0.25,
       shift=dict(heads=5, green=0)),
    sp('con_maj', 'Convallaria majalis', 2, None,
       col('#3D5A2E', '#4A6A32', '#8A7A3A', '#9A8A40', '#9C8458', '#6E5E48'),
       ramp('#7E8A6A', '#D6D4C4', '#A84A3A', '#6A4A3A'),
       phen(105, 140, 120, 140, 230, 290, 240, 290, 0.0, 0.9, climate='P1_TALL_CALAMAGROSTIS'),
       outline=2, leaf_h=[0.14, 0.22], leaf_w=[0.035, 0.06], leaf_tilt=[10, 30], leaf_bend=[0.12, 0.25],
       leaf_droop=[0.05, 0.15], leaf_stiff=0.5, culm_h=[0.12, 0.2], culm_w=[0.0018, 0.0025], culm_tilt=[5, 20],
       culm_bend=[0.15, 0.3], culm_stiff=0.5, clump_cell=0.12, clump_pull=0.9, clump_face_away=0.6,
       density_leaf=220, density_culm=60, head_type=14, head_len=[0.04, 0.08], head_w=[0.012, 0.018],
       head_branches=0, head_open=[0, 0], head_grain=0.6, head_share=0.6, shade_density=1.0),
    # ---- forbs -----------------------------------------------------------------------------------------------
    sp('sol_can', 'Solidago canadensis', 1, None,
       col('#4C5F35', '#587037', '#6A6438', '#8A7A42', '#8E7A5E', '#7E7466'),
       ramp('#8A9050', '#D2A830', '#B4A88C', '#8A7E70'),
       phen(100, 200, 205, 235, 275, 320, 260, 305, 0.85, 0.2),
       density_leaf=0, density_culm=150, culm_h=[0.7, 1.15], culm_w=[0.004, 0.006], culm_leaves=12,
       cauline_h=[0.07, 0.12], cauline_w=[0.01, 0.018], cauline_tilt=[45, 70], cauline_span=[0.12, 0.72],
       cauline_taper=0.55, cauline_bend=0.25, cauline_droop=0.25, head_type=11, head_len=[0.15, 0.28],
       head_w=[0.1, 0.2], head_branches=6, head_open=[55, 60], head_nod=[5, 15], stem_tint=[0.95, 1.0, 0.85]),
    sp('art_vul', 'Artemisia vulgaris', 1, None,
       col('#4A5A3C', '#52653E', '#5E5A3E', '#7A6A44', '#6E5848', '#5E5046'),
       ramp('#7A8268', '#8E7E58', '#7E6A50', '#5E5044'),
       phen(95, 210, 200, 220, 260, 310, 260, 300, 0.8, 0.15),
       outline=5, leaf_h=[0.08, 0.14], leaf_w=[0.04, 0.06], density_leaf=25, density_culm=70, culm_h=[0.8, 1.25],
       culm_w=[0.004, 0.007], culm_stiff=0.8, culm_leaves=12, cauline_h=[0.06, 0.11], cauline_w=[0.03, 0.05],
       cauline_tilt=[50, 75], cauline_span=[0.08, 0.65], cauline_taper=0.45, cauline_bend=0.2, cauline_droop=0.3,
       head_type=12, head_len=[0.2, 0.35], head_w=[0.06, 0.12], head_branches=6, head_open=[25, 20],
       stem_tint=[1.12, 0.78, 0.82]),
    sp('art_abs', 'Artemisia absinthium', 1, None,
       col('#8A9282', '#8E9886', '#868C7C', '#8E8A6E', '#8E8270', '#7A7066'),
       ramp('#8C9078', '#A89A58', '#8E8268', '#6E665A'),
       phen(100, 200, 195, 215, 255, 300, 270, 315, 0.75, 0.2, winter_green=0.15),
       outline=6, leaf_h=[0.08, 0.14], leaf_w=[0.03, 0.05], density_leaf=40, density_culm=60, culm_h=[0.55, 0.9],
       culm_w=[0.004, 0.006], culm_leaves=10, cauline_h=[0.04, 0.08], cauline_w=[0.025, 0.04], cauline_tilt=[45, 70],
       cauline_span=[0.1, 0.6], cauline_taper=0.5, cauline_bend=0.2, cauline_droop=0.2, head_type=12,
       head_len=[0.15, 0.28], head_w=[0.06, 0.12], head_branches=6, head_open=[30, 20]),
    sp('art_cam', 'Artemisia campestris', 1, None,
       col('#5E6A48', '#667050', '#6E5A44', '#7E6A48', '#7A5A46', '#6A5246'),
       ramp('#7E7A5A', '#A08A50', '#8A6A4E', '#6A5244'),
       phen(100, 210, 215, 240, 270, 310, 280, 320, 0.8, 0.15, winter_green=0.3, climate='P6_SPARSE_SAND'),
       leaf_h=[0.06, 0.1], leaf_w=[0.015, 0.03], outline=6, density_leaf=60, density_culm=70, culm_h=[0.35, 0.65],
       culm_w=[0.002, 0.003], culm_tilt=[5, 25], culm_bend=[0.05, 0.12], culm_stiff=0.7, culm_leaves=10,
       cauline_h=[0.03, 0.05], cauline_w=[0.008, 0.015], cauline_span=[0.05, 0.6], head_type=12,
       head_len=[0.12, 0.25], head_w=[0.05, 0.1], head_branches=6, head_open=[30, 25], head_grain=0.8,
       stem_tint=[1.2, 0.8, 0.75]),
    sp('tan_vul', 'Tanacetum vulgare', 1, None,
       col('#3E552E', '#4A6633', '#4E5634', '#7A7040', '#6E5C46', '#5E5246'),
       ramp('#7A8A4A', '#D0A425', '#8A6A3A', '#5E4E3A'),
       phen(100, 190, 190, 215, 255, 300, 270, 310, 0.75, 0.2),
       outline=5, leaf_h=[0.12, 0.22], leaf_w=[0.05, 0.08], density_leaf=40, density_culm=80, culm_h=[0.65, 1.05],
       culm_w=[0.004, 0.006], culm_leaves=10, cauline_h=[0.08, 0.15], cauline_w=[0.04, 0.07], cauline_tilt=[45, 70],
       cauline_span=[0.1, 0.7], cauline_bend=0.25, cauline_droop=0.25, head_type=10, head_len=[0.03, 0.06],
       head_w=[0.07, 0.13], head_branches=6, head_open=[40, 40], head_grain=2.0, stem_tint=[1.0, 0.95, 0.9]),
    sp('ach_mil', 'Achillea millefolium', 1, None,
       col('#4A6038', '#557040', '#4E5A38', '#7E7444', '#8A7458', '#6E6252'),
       ramp('#8A9070', '#D2CFC0', '#8E8268', '#6A5E50'),
       phen(90, 175, 165, 195, 250, 290, 265, 310, 0.5, 0.4, winter_green=0.25),
       outline=6, leaf_h=[0.08, 0.16], leaf_w=[0.015, 0.03], leaf_tilt=[25, 55], leaf_bend=[0.2, 0.35],
       density_leaf=160, density_culm=60, culm_h=[0.35, 0.6], culm_w=[0.0022, 0.003], culm_stiff=0.65,
       culm_leaves=6, cauline_h=[0.04, 0.08], cauline_w=[0.012, 0.02], cauline_tilt=[40, 65],
       cauline_span=[0.15, 0.7], cauline_bend=0.2, cauline_droop=0.15, head_type=10, head_len=[0.02, 0.04],
       head_w=[0.05, 0.1], head_branches=6, head_open=[40, 40], head_grain=1.0, head_share=0.75),
    sp('pla_lan', 'Plantago lanceolata', 2, None,
       col('#4A6236', '#556F3A', '#4E5C38', '#7E7444', '#8A7652', '#6E6050'),
       ramp('#6A7050', '#7E7466', '#5E4E3A', '#4E4236'),
       phen(85, 160, 140, 165, 220, 280, 270, 320, 0.3, 0.6, winter_green=0.35),
       leaf_h=[0.1, 0.22], leaf_w=[0.012, 0.025], leaf_tilt=[30, 65], leaf_bend=[0.15, 0.3], leaf_droop=[0.05, 0.15],
       leaf_stiff=0.45, density_leaf=280, density_culm=70, culm_h=[0.2, 0.42], culm_w=[0.0012, 0.0018],
       culm_tilt=[4, 15], culm_bend=[0.03, 0.08], culm_stiff=0.55, clump_cell=0.15, clump_pull=0.9,
       clump_face_away=0.95, head_type=2, head_len=[0.015, 0.035], head_w=[0.007, 0.01], head_branches=0,
       head_open=[0, 0], head_grain=0.8, head_share=0.7),
    sp('pla_maj', 'Plantago major', 2, None,
       col('#3E5A30', '#4A6A34', '#56603A', '#7E7A44', '#7E6E50', '#665A4A'),
       ramp('#6A7650', '#7A7A60', '#6A5C44', '#4E4436'),
       phen(105, 170, 165, 190, 240, 290, 270, 315, 0.2, 0.7, winter_green=0.15),
       outline=2, leaf_h=[0.08, 0.16], leaf_w=[0.04, 0.07], leaf_tilt=[55, 80], leaf_bend=[0.08, 0.18],
       leaf_droop=[0, 0.1], leaf_stiff=0.5, density_leaf=220, density_culm=45, culm_h=[0.15, 0.3],
       culm_w=[0.0015, 0.0022], culm_tilt=[5, 20], clump_cell=0.18, clump_pull=0.9, clump_face_away=1.0,
       head_type=1, head_len=[0.06, 0.15], head_w=[0.005, 0.007], head_branches=0, head_open=[0, 0],
       head_grain=0.8, head_share=0.6),
    sp('urt_dio', 'Urtica dioica', 1, None,
       col('#3C5530', '#46622E', '#4E5A34', '#7A7442', '#7E6C54', '#5E5448'),
       ramp('#6A7A50', '#7A8058', '#7A6E50', '#5E5444'),
       phen(75, 170, 170, 200, 240, 290, 275, 315, 0.5, 0.4),
       density_leaf=0, density_culm=130, culm_h=[0.7, 1.15], culm_w=[0.004, 0.006], culm_stiff=0.7, culm_leaves=12,
       outline=2, cauline_h=[0.07, 0.13], cauline_w=[0.04, 0.07], cauline_tilt=[45, 70], cauline_span=[0.1, 0.92],
       cauline_taper=0.55, cauline_arrange=2, cauline_bend=0.3, cauline_droop=0.4, head_type=12,
       head_len=[0.06, 0.12], head_w=[0.03, 0.06], head_branches=4, head_open=[50, 60], head_grain=0.7,
       head_share=0.6, stem_tint=[0.95, 1.0, 0.95]),
    sp('ant_syl', 'Anthriscus sylvestris', 1, None,
       col('#3E5A2E', '#4A6A30', '#4E6232', '#8A7A44', '#8E7A58', '#6E6050'),
       ramp('#8A9A6E', '#D8D8CC', '#5E5A3A', '#4E4436'),
       phen(70, 140, 120, 140, 175, 220, 180, 220, 0.3, 0.5, winter_green=0.2),
       leaf_h=[0.18, 0.32], leaf_w=[0.06, 0.1], leaf_tilt=[35, 60], leaf_bend=[0.25, 0.4], leaf_droop=[0.15, 0.3],
       outline=5, density_leaf=90, density_culm=40, culm_h=[0.7, 1.1], culm_w=[0.004, 0.007], culm_stiff=0.7,
       culm_leaves=3,
       cauline_h=[0.08, 0.15], cauline_w=[0.04, 0.08], cauline_tilt=[50, 75], cauline_span=[0.2, 0.55],
       cauline_taper=0.5, head_type=13, head_len=[0.04, 0.07], head_w=[0.05, 0.09], head_branches=6,
       head_open=[35, 45], head_share=0.75, stem_tint=[1.05, 0.92, 0.95]),
    sp('eri_can', 'Erigeron canadensis', 1, None,
       col('#56693E', '#5E7240', '#6A6A3E', '#8A7E46', '#9A8662', '#7A6E5C'),
       ramp('#7E8A60', '#C8C4AE', '#B0A890', '#7E7462'),
       phen(120, 220, 200, 225, 255, 290, 265, 300, 0.5, 0.4, winter_green=0.2),
       leaf_h=[0.05, 0.1], leaf_w=[0.008, 0.012], density_leaf=20, density_culm=90, culm_h=[0.5, 0.95],
       culm_w=[0.003, 0.005], culm_stiff=0.7, culm_leaves=12, cauline_h=[0.04, 0.08], cauline_w=[0.004, 0.008],
       cauline_tilt=[35, 60], cauline_span=[0.1, 0.7], cauline_bend=0.2, cauline_droop=0.2, head_type=12,
       head_len=[0.15, 0.3], head_w=[0.06, 0.12], head_branches=6, head_open=[30, 25], head_grain=0.9),
    sp('cha_ang', 'Chamerion angustifolium', 1, None,
       col('#3E5634', '#4A6436', '#7A4A3A', '#8A4E40', '#7E6450', '#6A5A4E'),
       ramp('#7A5A6A', '#B05888', '#C8C0B4', '#8A7E72'),
       phen(110, 185, 180, 205, 235, 290, 250, 290, 0.6, 0.3),
       density_leaf=0, density_culm=90, culm_h=[0.8, 1.3], culm_w=[0.004, 0.007], culm_leaves=12,
       cauline_h=[0.08, 0.14], cauline_w=[0.012, 0.02], cauline_tilt=[40, 65], cauline_span=[0.08, 0.7],
       cauline_bend=0.2, cauline_droop=0.15, head_type=14, head_len=[0.15, 0.3], head_w=[0.04, 0.07],
       head_branches=0, head_open=[0, 0], head_grain=1.8, head_petals=4, head_share=0.8, stem_tint=[1.11, 0.7, 1.27]),
    sp('lyt_sal', 'Lythrum salicaria', 1, None,
       col('#4A5E38', '#556E3A', '#7A4E38', '#8A5A3E', '#7A6450', '#6A5A4C'),
       ramp('#6E5A60', '#A0457A', '#6A4A40', '#5A4A40'),
       phen(110, 185, 180, 210, 245, 290, 260, 300, 0.6, 0.3, climate='P3_WET_MEADOW'),
       density_leaf=0, density_culm=60, culm_h=[0.6, 1.05], culm_w=[0.004, 0.006], culm_leaves=12,
       cauline_h=[0.05, 0.09], cauline_w=[0.01, 0.018], cauline_tilt=[40, 65], cauline_span=[0.08, 0.6],
       cauline_taper=0.65, cauline_arrange=2, cauline_bend=0.15, cauline_droop=0.1, head_type=14,
       head_len=[0.12, 0.25], head_w=[0.025, 0.04], head_branches=0, head_open=[0, 0], head_grain=1.0,
       head_petals=6, head_share=0.8, stem_tint=[1.23, 0.74, 1.2]),
    sp('hyp_per', 'Hypericum perforatum', 1, None,
       col('#4A5E36', '#566E3A', '#6A5A3A', '#7E6A3E', '#6A4E3E', '#5A4A40'),
       ramp('#8A8E50', '#D6B030', '#7A4A34', '#5A3E34'),
       phen(105, 175, 170, 195, 235, 290, 250, 295, 0.7, 0.2),
       density_leaf=0, density_culm=90, culm_h=[0.35, 0.6], culm_w=[0.002, 0.003], culm_stiff=0.7, culm_leaves=12,
       outline=2, cauline_h=[0.015, 0.028], cauline_w=[0.006, 0.01], cauline_tilt=[40, 65], cauline_span=[0.1, 0.75],
       cauline_taper=0.8, cauline_arrange=2, cauline_bend=0.1, cauline_droop=0.05, head_type=10,
       head_len=[0.04, 0.08], head_w=[0.07, 0.13], head_branches=6, head_open=[35, 40], head_grain=4.0,
       head_petals=5, head_share=0.8, stem_tint=[1.08, 0.88, 0.85]),
    sp('gal_ver', 'Galium verum', 1, None,
       col('#3E5A30', '#4A6A33', '#5A5A36', '#7A6E3E', '#5E5040', '#4E4A40'),
       ramp('#8A9050', '#D2B640', '#6E5E40', '#4E443A'),
       phen(110, 170, 165, 190, 230, 280, 255, 295, 0.4, 0.5),
       density_leaf=0, density_culm=160, culm_h=[0.3, 0.6], culm_w=[0.0015, 0.0022], culm_tilt=[5, 25],
       culm_bend=[0.05, 0.12], culm_stiff=0.5, culm_leaves=12, outline=3, cauline_h=[0.015, 0.025],
       cauline_w=[0.001, 0.002], cauline_tilt=[40, 70], cauline_span=[0.05, 0.7], cauline_taper=0.8,
       cauline_arrange=2, cauline_bend=0.05, cauline_droop=0.0, head_type=12, head_len=[0.08, 0.18],
       head_w=[0.03, 0.06], head_branches=6, head_open=[40, 35], head_grain=0.7, head_share=0.8),
    # ---- round 4: the forbs that colour the ChEZ grasslands (Didukh 2023 constancy, clusters 8-20) --------------
    sp('tri_pra', 'Trifolium pratense', 1, None,
       col('#4E6A3A', '#58753E', '#566038', '#7E7444', '#7E6A50', '#665A4C'),
       ramp('#6E7A4E', '#B86A9C', '#5E3E36', '#4A3830'),
       phen(95, 165, 150, 165, 230, 275, 270, 310, 0.1, 0.6, winter_green=0.3),
       outline=2, leaf_h=[0.035, 0.06], leaf_w=[0.022, 0.035], leaf_tilt=[50, 80], leaf_bend=[0.1, 0.25],
       density_leaf=150, density_culm=50, culm_h=[0.2, 0.38], culm_w=[0.002, 0.003], culm_tilt=[8, 25],
       culm_bend=[0.04, 0.1], culm_stiff=0.5, culm_leaves=5, cauline_h=[0.03, 0.05], cauline_w=[0.018, 0.028],
       cauline_tilt=[50, 75], cauline_span=[0.25, 0.85], cauline_taper=0.7, cauline_bend=0.15, cauline_droop=0.1,
       head_type=15, head_len=[0.022, 0.032], head_w=[0.024, 0.032], head_grain=0.9, head_share=0.85,
       stem_tint=[1.05, 0.9, 0.9]),
    sp('vic_cra', 'Vicia cracca', 1, None,
       col('#4A6236', '#536E3A', '#56603A', '#7A7244', '#7A6650', '#62584C'),
       ramp('#5E6A4A', '#8A68B4', '#3E3428', '#352C24'),
       phen(110, 180, 160, 180, 230, 265, 250, 290, 0.2, 0.8),
       outline=5, density_leaf=0, density_culm=70, culm_h=[0.4, 0.75], culm_w=[0.0015, 0.0025], culm_tilt=[10, 30],
       culm_bend=[0.08, 0.2], culm_stiff=0.4, culm_leaves=8, cauline_h=[0.04, 0.08], cauline_w=[0.015, 0.025],
       cauline_tilt=[45, 70], cauline_span=[0.15, 0.85], cauline_taper=0.8, cauline_bend=0.15, cauline_droop=0.25,
       head_type=14, head_len=[0.04, 0.07], head_w=[0.016, 0.024], head_grain=1.0, head_share=0.8,
       head_nod=[0, 15]),
    sp('tri_rep', 'Trifolium repens', 2, None,
       col('#4A6838', '#55763C', '#526036', '#7A7244', '#7A6A50', '#625A4C'),
       ramp('#7A8A60', '#DCD8C8', '#8A6A4A', '#5A4636'),
       phen(90, 150, 135, 155, 220, 280, 280, 320, 0.0, 0.8, winter_green=0.5),
       outline=2, leaf_h=[0.03, 0.05], leaf_w=[0.02, 0.03], leaf_tilt=[55, 85], leaf_bend=[0.1, 0.25],
       leaf_droop=[0.0, 0.1], leaf_stiff=0.5, density_leaf=320, density_culm=70, culm_h=[0.08, 0.18],
       culm_w=[0.0012, 0.0018], culm_tilt=[5, 20], culm_bend=[0.04, 0.1], culm_stiff=0.5, clump_cell=0.12,
       clump_pull=0.6, clump_face_away=0.6, head_type=15, head_len=[0.016, 0.022], head_w=[0.018, 0.025],
       head_grain=0.8, head_share=0.8),
    sp('tar_off', 'Taraxacum officinale agg.', 2, None,
       col('#4A6636', '#55743A', '#566036', '#7E7444', '#806A50', '#665A4C'),
       ramp('#6E7A48', '#E0C030', '#D8D8D0', '#8A7A60'),
       phen(80, 130, 105, 120, 140, 170, 275, 320, 0.0, 0.9, winter_green=0.6),
       outline=5, leaf_h=[0.08, 0.2], leaf_w=[0.025, 0.045], leaf_tilt=[55, 82], leaf_bend=[0.08, 0.2],
       leaf_droop=[0.0, 0.1], leaf_stiff=0.5, density_leaf=110, density_culm=30, culm_h=[0.1, 0.28],
       culm_w=[0.003, 0.0045], culm_tilt=[3, 15], culm_bend=[0.04, 0.12], culm_stiff=0.55, clump_cell=0.15,
       clump_pull=0.9, clump_face_away=1.0, head_type=15, head_len=[0.03, 0.04], head_w=[0.035, 0.045],
       head_grain=0.8, head_share=0.85),
    sp('lat_pra', 'Lathyrus pratensis', 1, None,
       col('#4A6236', '#536E3A', '#56603A', '#7A7244', '#7A6650', '#62584C'),
       ramp('#6A7048', '#D8C030', '#3A3024', '#30281E'),
       phen(110, 180, 165, 185, 235, 265, 255, 295, 0.2, 0.8),
       outline=1, density_leaf=0, density_culm=55, culm_h=[0.3, 0.6], culm_w=[0.0015, 0.0025], culm_tilt=[15, 40],
       culm_bend=[0.1, 0.25], culm_stiff=0.35, culm_leaves=6, cauline_h=[0.025, 0.045], cauline_w=[0.006, 0.01],
       cauline_tilt=[40, 65], cauline_span=[0.2, 0.85], cauline_taper=0.8, cauline_arrange=0, cauline_bend=0.1,
       cauline_droop=0.15, head_type=14, head_len=[0.03, 0.05], head_w=[0.025, 0.035], head_grain=1.4,
       head_share=0.8),
    sp('ran_acr', 'Ranunculus acris', 1, None,
       col('#4A6436', '#55723A', '#56603A', '#7E7444', '#7E6A50', '#665A4C'),
       ramp('#6E7A44', '#E8C828', '#7A6A44', '#5E5040'),
       phen(85, 150, 130, 150, 200, 230, 240, 290, 0.0, 0.8, winter_green=0.3),
       outline=6, leaf_h=[0.06, 0.14], leaf_w=[0.04, 0.07], leaf_tilt=[40, 70], leaf_bend=[0.1, 0.25],
       density_leaf=70, density_culm=45, culm_h=[0.3, 0.55], culm_w=[0.0018, 0.0026], culm_stiff=0.6, culm_leaves=3,
       cauline_h=[0.03, 0.06], cauline_w=[0.02, 0.04], cauline_tilt=[45, 70], cauline_span=[0.3, 0.7],
       cauline_taper=0.6, head_type=10, head_len=[0.04, 0.08], head_w=[0.06, 0.11], head_branches=6,
       head_open=[35, 40], head_grain=4.0, head_petals=5, head_share=0.8),
    sp('sil_lat', 'Silene latifolia', 1, None,
       col('#4E6438', '#58703C', '#5A6038', '#7E7444', '#806A50', '#665A4C'),
       ramp('#7A845E', '#E2E4D6', '#8A7450', '#6A5A44'),
       phen(95, 175, 150, 165, 220, 285, 270, 310, 0.4, 0.4, winter_green=0.2),
       outline=1, leaf_h=[0.06, 0.12], leaf_w=[0.015, 0.03], density_leaf=25, density_culm=45, culm_h=[0.35, 0.65],
       culm_w=[0.003, 0.0045], culm_stiff=0.6, culm_leaves=8, cauline_h=[0.04, 0.08], cauline_w=[0.012, 0.025],
       cauline_tilt=[35, 60], cauline_span=[0.1, 0.7], cauline_taper=0.6, cauline_arrange=2, cauline_bend=0.15,
       cauline_droop=0.15, head_type=10, head_len=[0.04, 0.08], head_w=[0.06, 0.11], head_branches=5,
       head_open=[30, 35], head_grain=5.0, head_petals=5, head_share=0.8),
    sp('sil_flo', 'Silene flos-cuculi', 1, None,
       col('#4A6234', '#546E38', '#56603A', '#7A7244', '#7A6650', '#62584C'),
       ramp('#6E6A50', '#C87AAA', '#6E5A44', '#5A4A3C'),
       phen(90, 150, 130, 145, 190, 220, 230, 280, 0.2, 0.6, winter_green=0.3, climate='P3_WET_MEADOW'),
       outline=1, leaf_h=[0.05, 0.1], leaf_w=[0.008, 0.015], density_leaf=30, density_culm=50, culm_h=[0.3, 0.55],
       culm_w=[0.0018, 0.0026], culm_stiff=0.55, culm_leaves=6, cauline_h=[0.03, 0.06], cauline_w=[0.004, 0.008],
       cauline_tilt=[30, 55], cauline_span=[0.15, 0.7], cauline_taper=0.7, cauline_arrange=2, cauline_bend=0.15,
       head_type=10, head_len=[0.04, 0.07], head_w=[0.06, 0.1], head_branches=5, head_open=[30, 35],
       head_grain=6.0, head_petals=5, head_share=0.8),
    sp('cen_jac', 'Centaurea jacea', 1, None,
       col('#4E6438', '#58703C', '#5A6038', '#7E7444', '#806A50', '#665A4C'),
       ramp('#5E6448', '#A860C0', '#5A4232', '#4A3A2E'),
       phen(100, 190, 175, 195, 240, 290, 270, 310, 0.5, 0.4, winter_green=0.2),
       outline=1, leaf_h=[0.06, 0.12], leaf_w=[0.012, 0.025], density_leaf=30, density_culm=50, culm_h=[0.3, 0.6],
       culm_w=[0.0022, 0.003], culm_stiff=0.6, culm_leaves=8, cauline_h=[0.04, 0.08], cauline_w=[0.008, 0.016],
       cauline_tilt=[35, 60], cauline_span=[0.1, 0.75], cauline_taper=0.55, cauline_bend=0.15,
       head_type=15, head_len=[0.025, 0.035], head_w=[0.03, 0.04], head_grain=1.3, head_share=0.85),
    sp('rum_acl', 'Rumex acetosella', 1, None,
       col('#4E6438', '#58703C', '#6A5A3A', '#7E6A44', '#7A5E48', '#665046'),
       ramp('#6A6048', '#9A4A44', '#6A3A30', '#4A3028'),
       phen(80, 140, 120, 140, 180, 220, 220, 270, 0.3, 0.6, winter_green=0.4, climate='P6_SPARSE_SAND'),
       outline=1, leaf_h=[0.025, 0.05], leaf_w=[0.005, 0.01], leaf_tilt=[30, 60], density_leaf=120,
       density_culm=70, culm_h=[0.15, 0.3], culm_w=[0.0012, 0.0018], culm_tilt=[5, 20], culm_stiff=0.5,
       culm_leaves=3, cauline_h=[0.02, 0.04], cauline_w=[0.003, 0.006], cauline_tilt=[30, 55],
       cauline_span=[0.1, 0.45], head_type=12, head_len=[0.06, 0.12], head_w=[0.025, 0.045], head_branches=5,
       head_open=[30, 25], head_grain=0.6, head_share=0.8, stem_tint=[1.15, 0.8, 0.8]),
    sp('rum_acs', 'Rumex acetosa', 1, None,
       col('#4A6636', '#55723A', '#5E5A38', '#7E6E44', '#7A6248', '#665046'),
       ramp('#6A6A48', '#9A4E46', '#7A5A44', '#5A4636'),
       phen(80, 145, 125, 145, 180, 220, 210, 260, 0.3, 0.6, winter_green=0.3),
       outline=2, leaf_h=[0.06, 0.12], leaf_w=[0.02, 0.035], leaf_tilt=[30, 60], density_leaf=50,
       density_culm=40, culm_h=[0.35, 0.65], culm_w=[0.002, 0.003], culm_stiff=0.6, culm_leaves=4,
       cauline_h=[0.03, 0.06], cauline_w=[0.008, 0.015], cauline_tilt=[30, 55], cauline_span=[0.1, 0.5],
       head_type=12, head_len=[0.1, 0.2], head_w=[0.035, 0.06], head_branches=6, head_open=[25, 20],
       head_grain=0.7, head_share=0.8, stem_tint=[1.12, 0.82, 0.82]),
    sp('oen_bie', 'Oenothera biennis', 1, None,
       col('#4A6236', '#536E3A', '#5A5C38', '#7E7044', '#7A6448', '#665446'),
       ramp('#7A8048', '#D2C030', '#6A6040', '#4A3E30'),
       phen(100, 200, 180, 200, 245, 300, 275, 315, 0.7, 0.2, winter_green=0.4),
       outline=1, leaf_h=[0.08, 0.18], leaf_w=[0.015, 0.03], leaf_tilt=[60, 85], density_leaf=30, density_culm=40,
       culm_h=[0.6, 1.0], culm_w=[0.004, 0.006], culm_stiff=0.8, culm_leaves=12, cauline_h=[0.05, 0.1],
       cauline_w=[0.012, 0.022], cauline_tilt=[35, 55], cauline_span=[0.1, 0.7], cauline_taper=0.55,
       cauline_bend=0.12, head_type=14, head_len=[0.1, 0.22], head_w=[0.05, 0.08], head_grain=2.8,
       head_petals=4, head_share=0.8, stem_tint=[1.1, 0.88, 0.85]),
    sp('cir_arv', 'Cirsium arvense', 1, None,
       col('#4E6440', '#587044', '#5A6040', '#7A7046', '#7A664E', '#625A4E'),
       ramp('#6A6850', '#A07AAE', '#D8D0C8', '#8A7A68'),
       phen(105, 190, 175, 195, 225, 260, 260, 300, 0.5, 0.3),
       outline=5, leaf_h=[0.08, 0.14], leaf_w=[0.025, 0.04], density_leaf=15, density_culm=55, culm_h=[0.5, 0.85],
       culm_w=[0.003, 0.005], culm_stiff=0.75, culm_leaves=10, cauline_h=[0.05, 0.1], cauline_w=[0.015, 0.03],
       cauline_tilt=[40, 65], cauline_span=[0.08, 0.75], cauline_taper=0.55, cauline_bend=0.15,
       cauline_droop=0.15, head_type=10, head_len=[0.04, 0.08], head_w=[0.08, 0.14], head_branches=6,
       head_open=[35, 40], head_grain=3.5, head_share=0.75),
    sp('jas_mon', 'Jasione montana', 1, None,
       col('#4E6438', '#58703C', '#5A6038', '#7E7444', '#806A50', '#665A4C'),
       ramp('#6A7050', '#6E72A8', '#7A6A50', '#5A4A3C'),
       phen(100, 170, 160, 180, 230, 260, 250, 290, 0.3, 0.5, winter_green=0.3, climate='P6_SPARSE_SAND'),
       outline=1, leaf_h=[0.02, 0.04], leaf_w=[0.003, 0.006], density_leaf=60, density_culm=50,
       culm_h=[0.15, 0.3], culm_w=[0.0012, 0.0018], culm_tilt=[5, 20], culm_stiff=0.5, culm_leaves=4,
       cauline_h=[0.015, 0.03], cauline_w=[0.002, 0.004], cauline_tilt=[30, 55], cauline_span=[0.05, 0.4],
       head_type=15, head_len=[0.015, 0.022], head_w=[0.018, 0.026], head_grain=0.6, head_share=0.85),
    sp('hel_are', 'Helichrysum arenarium', 1, None,
       col('#7E8C70', '#86967A', '#7E8468', '#8A8466', '#8A7E68', '#746A5E'),
       ramp('#9A9A70', '#D0B830', '#B8A040', '#8A7A5A'),
       phen(110, 180, 170, 190, 240, 300, 280, 320, 0.6, 0.3, climate='P6_SPARSE_SAND'),
       outline=1, leaf_h=[0.03, 0.06], leaf_w=[0.005, 0.009], leaf_tilt=[50, 80], density_leaf=60,
       density_culm=50, culm_h=[0.12, 0.25], culm_w=[0.0015, 0.0022], culm_stiff=0.6, culm_leaves=6,
       cauline_h=[0.02, 0.04], cauline_w=[0.003, 0.006], cauline_tilt=[30, 50], cauline_span=[0.1, 0.7],
       head_type=10, head_len=[0.02, 0.035], head_w=[0.025, 0.05], head_branches=5, head_open=[35, 35],
       head_grain=1.3, head_share=0.85),
    sp('ber_inc', 'Berteroa incana', 1, None,
       col('#6A7A58', '#74865E', '#6E7258', '#807A5A', '#807058', '#6A6052'),
       ramp('#8A9070', '#D8D8D0', '#8A8668', '#6A6050'),
       phen(105, 170, 150, 165, 220, 290, 280, 320, 0.5, 0.3),
       outline=1, leaf_h=[0.03, 0.06], leaf_w=[0.006, 0.012], density_leaf=20, density_culm=60, culm_h=[0.25, 0.45],
       culm_w=[0.0018, 0.0026], culm_stiff=0.6, culm_leaves=8, cauline_h=[0.025, 0.05], cauline_w=[0.005, 0.01],
       cauline_tilt=[30, 50], cauline_span=[0.1, 0.75], cauline_taper=0.7, head_type=10, head_len=[0.02, 0.04],
       head_w=[0.02, 0.035], head_branches=5, head_open=[30, 30], head_grain=1.0, head_petals=4, head_share=0.85),
    sp('ver_lyc', 'Verbascum lychnitis', 1, None,
       col('#6E8060', '#768A66', '#727860', '#827A5A', '#806E56', '#6A5E50'),
       ramp('#8A8A60', '#C8B048', '#7A6A50', '#4A3E32'),
       phen(100, 190, 170, 185, 235, 280, 260, 300, 0.7, 0.2, winter_green=0.5),
       outline=2, leaf_h=[0.1, 0.2], leaf_w=[0.03, 0.06], leaf_tilt=[60, 85], density_leaf=20, density_culm=25,
       culm_h=[0.6, 1.0], culm_w=[0.004, 0.006], culm_stiff=0.85, culm_leaves=10, cauline_h=[0.05, 0.1],
       cauline_w=[0.015, 0.03], cauline_tilt=[30, 50], cauline_span=[0.1, 0.55], cauline_taper=0.5,
       head_type=12, head_len=[0.2, 0.4], head_w=[0.08, 0.14], head_branches=6, head_open=[25, 20],
       head_grain=2.0, head_petals=5, head_share=0.8),
    sp('sol_vir', 'Solidago virgaurea', 1, None,
       col('#4A6236', '#536E3A', '#56603A', '#7E7044', '#7A6650', '#62584C'),
       ramp('#7A8048', '#C8AC30', '#B0A890', '#7A6A58'),
       phen(105, 200, 190, 215, 260, 300, 270, 310, 0.4, 0.4),
       outline=1, leaf_h=[0.05, 0.1], leaf_w=[0.015, 0.025], density_leaf=20, density_culm=40, culm_h=[0.3, 0.6],
       culm_w=[0.0025, 0.0035], culm_stiff=0.7, culm_leaves=10, cauline_h=[0.04, 0.08], cauline_w=[0.01, 0.02],
       cauline_tilt=[35, 55], cauline_span=[0.1, 0.7], cauline_taper=0.55, head_type=12,
       head_len=[0.08, 0.18], head_w=[0.03, 0.05], head_branches=5, head_open=[20, 20], head_grain=1.6,
       head_share=0.8),
    sp('pru_vul', 'Prunella vulgaris', 1, None,
       col('#4A6436', '#55723A', '#56603A', '#7A7244', '#7A6650', '#62584C'),
       ramp('#6A6A50', '#9A70C0', '#5A4232', '#4A3A2E'),
       phen(105, 180, 165, 190, 230, 270, 270, 310, 0.2, 0.6, winter_green=0.3, climate='P3_WET_MEADOW'),
       outline=2, leaf_h=[0.025, 0.05], leaf_w=[0.012, 0.022], leaf_tilt=[50, 80], density_leaf=60,
       density_culm=70, culm_h=[0.1, 0.22], culm_w=[0.0018, 0.0025], culm_tilt=[5, 20], culm_stiff=0.55,
       culm_leaves=4, cauline_h=[0.02, 0.04], cauline_w=[0.01, 0.018], cauline_tilt=[50, 75],
       cauline_span=[0.2, 0.7], cauline_arrange=2, head_type=2, head_len=[0.025, 0.04], head_w=[0.016, 0.022],
       head_grain=1.3, head_share=0.85),
    sp('dau_car', 'Daucus carota', 1, None,
       col('#4E6238', '#58703C', '#5A6038', '#7E7444', '#806A50', '#665A4C'),
       ramp('#8A9A6E', '#DCDFD6', '#7A6A50', '#5A4A3C'),
       phen(105, 190, 180, 200, 245, 300, 275, 315, 0.6, 0.3, winter_green=0.4),
       outline=6, leaf_h=[0.08, 0.16], leaf_w=[0.04, 0.07], leaf_tilt=[45, 75], density_leaf=35, density_culm=35,
       culm_h=[0.4, 0.75], culm_w=[0.003, 0.0045], culm_stiff=0.7, culm_leaves=4, cauline_h=[0.05, 0.1],
       cauline_w=[0.025, 0.045], cauline_tilt=[40, 65], cauline_span=[0.15, 0.6], cauline_taper=0.5,
       head_type=13, head_len=[0.03, 0.06], head_w=[0.05, 0.09], head_branches=6, head_open=[35, 45],
       head_share=0.8),
    sp('cam_pat', 'Campanula patula', 1, None,
       col('#4E6438', '#58703C', '#5A6038', '#7E7444', '#806A50', '#665A4C'),
       ramp('#6E6A60', '#B07AE0', '#7A6A50', '#6A5A48'),
       phen(110, 170, 155, 170, 210, 240, 240, 280, 0.2, 0.6),
       outline=1, leaf_h=[0.03, 0.06], leaf_w=[0.008, 0.014], density_leaf=25, density_culm=50,
       culm_h=[0.25, 0.45], culm_w=[0.0012, 0.0018], culm_stiff=0.5, culm_leaves=6, cauline_h=[0.02, 0.04],
       cauline_w=[0.004, 0.008], cauline_tilt=[30, 50], cauline_span=[0.1, 0.7], cauline_taper=0.7,
       head_type=10, head_len=[0.04, 0.08], head_w=[0.06, 0.11], head_branches=5, head_open=[35, 40],
       head_grain=4.0, head_petals=5, head_share=0.8),
    sp('equ_arv', 'Equisetum arvense', 1, None,
       col('#5A7A34', '#64863A', '#6A7036', '#887A44', '#7E6A4C', '#665A48'),
       ramp('#6A7044', '#6A7044', '#6A6044', '#5A5040'),
       phen(100, 160, 330, 340, 350, 360, 255, 295, 0.0, 0.9),
       outline=3, density_leaf=0, density_culm=110, culm_h=[0.18, 0.38], culm_w=[0.0018, 0.0026], culm_tilt=[3, 15],
       culm_bend=[0.03, 0.08], culm_stiff=0.6, culm_leaves=12, cauline_h=[0.04, 0.08], cauline_w=[0.0008, 0.0012],
       cauline_tilt=[40, 60], cauline_span=[0.15, 0.85], cauline_taper=0.6, cauline_arrange=2, cauline_bend=0.15,
       cauline_droop=0.25, head_type=2, head_share=0.0),
    sp('lys_vul', 'Lysimachia vulgaris', 1, None,
       col('#4A6236', '#536E3A', '#56603A', '#7E7044', '#7A6650', '#62584C'),
       ramp('#7A8048', '#D0C030', '#7A6A50', '#4A3E32'),
       phen(110, 190, 180, 195, 240, 280, 255, 295, 0.3, 0.5, climate='P3_WET_MEADOW'),
       outline=1, density_leaf=0, density_culm=60, culm_h=[0.5, 0.85], culm_w=[0.003, 0.0045], culm_stiff=0.75,
       culm_leaves=12, cauline_h=[0.05, 0.1], cauline_w=[0.015, 0.028], cauline_tilt=[40, 65],
       cauline_span=[0.1, 0.75], cauline_taper=0.6, cauline_arrange=2, cauline_bend=0.15, head_type=10,
       head_len=[0.05, 0.1], head_w=[0.06, 0.11], head_branches=6, head_open=[30, 35], head_grain=3.5,
       head_petals=5, head_share=0.8),
    sp('che_maj', 'Chelidonium majus', 1, None,
       col('#5A7448', '#62804E', '#5E6A48', '#7E7646', '#7A6A52', '#665C4E'),
       ramp('#7A8A54', '#E0C830', '#7A7A48', '#5E5040'),
       phen(85, 150, 125, 140, 200, 280, 280, 320, 0.2, 0.6, winter_green=0.4),
       outline=5, leaf_h=[0.1, 0.18], leaf_w=[0.04, 0.07], leaf_tilt=[40, 70], density_leaf=25, density_culm=40,
       culm_h=[0.35, 0.6], culm_w=[0.003, 0.0045], culm_stiff=0.6, culm_leaves=5, cauline_h=[0.06, 0.12],
       cauline_w=[0.03, 0.06], cauline_tilt=[40, 65], cauline_span=[0.2, 0.75], cauline_taper=0.6,
       cauline_bend=0.2, cauline_droop=0.2, head_type=10, head_len=[0.03, 0.06], head_w=[0.04, 0.08],
       head_branches=5, head_open=[35, 40], head_grain=4.0, head_petals=4, head_share=0.8),
]

# ---------------------------------------------------------------------------------------------------------------
# The traits research (06.10.2026, traits.json: floras, FloraVeg, the Chornobyl reserve's nature calendars
# 2018-2022, Didukh 2023): leaf postures and the ChEZ dates. Heads of a graminoid shift against its template by
# the difference of the anthesis midpoints, its spring by the green-up; a forb's phenology is its own (days of the
# year, 0 = 1 Jan; 2018-2022 springs are later and autumns earlier than the warm 2019: first frost ~14 Oct).
# ---------------------------------------------------------------------------------------------------------------
TRAITS = {
    'cal_epi': dict(leaf_tilt=[6, 24], leaf_bend=[0.08, 0.22], shift=dict(heads=-10, green=0)),
    'ely_rep': dict(leaf_tilt=[8, 28], shift=dict(heads=22, green=0)),
    'dac_glo': dict(leaf_tilt=[22, 50], shift=dict(heads=4, green=-5)),
    'poa_pra': dict(leaf_tilt=[18, 48], shift=dict(heads=-7, green=-5)),
    'poa_ang': dict(leaf_tilt=[8, 28], leaf_bend=[0.1, 0.25], shift=dict(heads=-14, green=-5)),
    'fes_rub': dict(leaf_tilt=[18, 48], shift=dict(heads=9, green=-8)),
    'fes_ovi': dict(leaf_tilt=[12, 50], shift=dict(heads=-15, green=0)),
    'agr_cap': dict(leaf_tilt=[18, 48], shift=dict(heads=24, green=0)),
    'agr_gig': dict(leaf_tilt=[18, 48], shift=dict(heads=34, green=0)),
    'phl_pra': dict(leaf_tilt=[22, 52], leaf_droop=[0.2, 0.4], leaf_stiff=0.3, shift=dict(heads=24, green=12)),
    'alo_pra': dict(leaf_tilt=[15, 40], shift=dict(heads=-36, green=-15)),
    'bro_ine': dict(leaf_tilt=[15, 35], shift=dict(heads=7, green=0)),
    'des_ces': dict(leaf_tilt=[22, 52], shift=dict(heads=15, green=-8)),
    'cor_can': dict(shift=dict(heads=6, green=0)),
    'ani_tec': dict(shift=dict(heads=-5, green=-35)),
    'phr_aus': dict(cauline_tilt=[35, 65], cauline_droop=0.4, shift=dict(heads=8, green=0)),
    'pha_aru': dict(shift=dict(heads=40, green=-5)),
    'gly_max': dict(shift=dict(heads=45, green=0)),
    'car_acu': dict(leaf_tilt=[5, 35], leaf_droop=[0.25, 0.45], shift=dict(heads=-8, green=0)),
    'car_hir': dict(shift=dict(heads=-43, green=0)),
    'jun_eff': dict(culm_tilt=[2, 22], shift=dict(heads=55, green=0)),
    'typ_lat': dict(shift=dict(heads=-51, green=0)),
    'cal_aru': dict(shift=dict(heads=-5, green=0)),
    'ave_fle': dict(shift=dict(heads=0, green=5)),
    'con_maj': dict(leaf_tilt=[10, 35], phen=phen(105, 135, 125, 137, 235, 275, 240, 287, 0.0, 1.0,
                                                  climate='P1_TALL_CALAMAGROSTIS')),
    'sol_can': dict(cauline_tilt=[35, 60], phen=phen(107, 200, 205, 241, 275, 320, 268, 305, 0.85, 0.15)),
    'art_vul': dict(cauline_tilt=[40, 70], phen=phen(105, 200, 190, 215, 265, 310, 262, 310, 0.8, 0.15)),
    'art_abs': dict(phen=phen(95, 195, 178, 228, 250, 285, 265, 310, 0.75, 0.2, winter_green=0.3)),
    'art_cam': dict(phen=phen(90, 200, 200, 248, 260, 300, 280, 315, 0.8, 0.15, winter_green=0.35,
                              climate='P6_SPARSE_SAND')),
    'tan_vul': dict(phen=phen(105, 190, 175, 215, 255, 300, 265, 310, 0.75, 0.2, winter_green=0.25)),
    'ach_mil': dict(phen=phen(80, 170, 145, 200, 235, 275, 280, 315, 0.4, 0.5, winter_green=0.35)),
    'pla_lan': dict(phen=phen(74, 150, 118, 170, 215, 265, 280, 320, 0.3, 0.6, winter_green=0.4)),
    'pla_maj': dict(leaf_tilt=[60, 85], phen=phen(100, 170, 140, 200, 230, 270, 285, 315, 0.25, 0.7,
                                                   winter_green=0.1)),
    'urt_dio': dict(cauline_tilt=[55, 80], phen=phen(82, 165, 140, 195, 230, 280, 255, 300, 0.5, 0.45)),
    'ant_syl': dict(leaf_h=[0.2, 0.35], leaf_w=[0.06, 0.12], phen=phen(85, 135, 115, 145, 200, 240, 185, 235, 0.25,
                                                                        0.5, winter_green=0.3)),
    'eri_can': dict(phen=phen(100, 215, 185, 228, 255, 300, 255, 300, 0.5, 0.4, winter_green=0.2)),
    'cha_ang': dict(cauline_tilt=[40, 70], phen=phen(112, 185, 165, 197, 230, 265, 250, 285, 0.6, 0.3)),
    'lyt_sal': dict(phen=phen(110, 185, 170, 205, 245, 285, 250, 290, 0.6, 0.3, climate='P3_WET_MEADOW')),
    'hyp_per': dict(phen=phen(102, 170, 150, 180, 230, 270, 250, 290, 0.7, 0.2)),
    'gal_ver': dict(cauline_tilt=[55, 85], phen=phen(100, 165, 148, 195, 240, 270, 250, 290, 0.35, 0.55,
                                                       winter_green=0.2)),
}

# The communities after the traits research's shares (Didukh 2023 constancy; "others not modelled" left out, the
# rest renormalised): the graminoids' cover shares, the forbs' mean shares of the lanes split into the scattered
# plants and their clonal patches (clonal forbs: 85 % in patches taking 70 % of the lanes there; the others half
# in loose patches taking 30-45 %), the patch sizes of the research's patch notes. `cover` thins a community whose
# ground is mostly not grass (a pine forest floor of needles and moss).
CLONAL = {'urt_dio': 0.75, 'con_maj': 0.75, 'sol_can': 0.7, 'tan_vul': 0.5, 'art_vul': 0.5, 'gal_ver': 0.45,
          'lyt_sal': 0.45, 'cha_ang': 0.5, 'cir_arv': 0.6, 'tri_rep': 0.5, 'equ_arv': 0.6, 'rum_acl': 0.5,
          'lys_vul': 0.5, 'che_maj': 0.4, 'pru_vul': 0.4, 'vic_cra': 0.4, 'lat_pra': 0.4}


# Fine-leaved graminoids fold two leaves into one strip (one lane for two leaves): the lattice stays at about 500
# lanes per m2 while their swards keep their leaf counts.
FOLDS = {'poa_pra': 0.6, 'poa_ang': 0.6, 'fes_rub': 0.6, 'fes_ovi': 0.6, 'agr_cap': 0.6, 'cor_can': 0.6,
         'ave_fle': 0.6, 'des_ces': 0.6}
LANES = {'agr_gig': dict(density_leaf=480), 'ely_rep': dict(density_leaf=440), 'car_hir': dict(density_leaf=470)}


def community(name, classes, grams, forbs, others, patch, scattered, forb_patch, litter, litter_h, litter_w,
              presence=0.4, cover=1.0):
    # `others`: the share of the ground the research gives to plants not modelled (mosses, rarer forbs); the shares
    # kept are renormalised among themselves.
    grams = dict(sorted(grams.items(), key=lambda kv: -kv[1])[:8])
    forbs = dict(sorted(forbs.items(), key=lambda kv: -kv[1])[:16])
    keep = sum(grams.values()) + sum(forbs.values())
    g = {k: round(v / sum(grams.values()), 4) for k, v in grams.items()}
    f = {}
    for k, v in forbs.items():
        share = v / keep
        if k in CLONAL:
            dense = CLONAL[k]
            f[k] = [round(0.15 * share, 4), round(min(0.85 * share / dense, 1.0), 4), dense]
        else:
            f[k] = [round(0.5 * share, 4), round(min(0.5 * share / 0.3, 1.0), 4), 0.3]
    return dict(name=name, classes=classes, species=g, forbs=f, forb_patch=forb_patch, patch=patch,
                scattered=scattered, presence=presence, cover=cover, litter=litter, litter_h=litter_h,
                litter_w=litter_w)


C = [
    # Old fields on sand / loamy sand (Didukh cluster 13): Silene latifolia 56, A. absinthium 46, Achillea 40, Galium
    # verum 40, Rumex acetosa 37, Erigeron 34, Tanacetum 30, A. campestris 30, Berteroa 29, Oenothera 26, Hypericum 24,
    # Verbascum 19, Vicia 16, Cirsium 14; Solidago canadensis and fireweed as patchy accents (UNIAN 2026, burns).
    community('old_field', ['PG_BUNCH', 'PG_FINE'],
              dict(cal_epi=0.30, ely_rep=0.13, poa_ang=0.09, car_hir=0.04, dac_glo=0.03, poa_pra=0.03, agr_cap=0.02,
                   fes_rub=0.02),
              dict(sil_lat=0.045, art_abs=0.035, ach_mil=0.032, gal_ver=0.032, rum_acs=0.03, eri_can=0.027,
                   tan_vul=0.024, art_cam=0.024, ber_inc=0.023, oen_bie=0.021, hyp_per=0.019, ver_lyc=0.015,
                   vic_cra=0.013, cir_arv=0.011, sol_can=0.01, cha_ang=0.008), 0.10,
              patch=8.0, scattered=0.15, forb_patch=2.5, litter=1.6, litter_h=[0.12, 0.3], litter_w=0.005),
    # Mesic fallows, abandoned hayfields (cluster 12, with 20): Cirsium arvense 45, Silene 38, Vicia 33, Urtica 29,
    # A. vulgaris 28, Achillea 28, Equisetum 27, Taraxacum 22, Oenothera 23, Hypericum 18, Erigeron 18, Trifolium
    # repens 15, Ranunculus 13, Plantago lanceolata 13, Galium verum 11, Trifolium pratense 9.
    community('mesic_meadow', ['PG_LEAFY', 'PG_PLUME'],
              dict(ely_rep=0.18, dac_glo=0.11, poa_pra=0.10, agr_gig=0.06, alo_pra=0.05, cal_epi=0.04, phl_pra=0.03,
                   bro_ine=0.02),
              dict(cir_arv=0.036, sil_lat=0.03, vic_cra=0.026, urt_dio=0.023, art_vul=0.022, ach_mil=0.022,
                   equ_arv=0.02, tar_off=0.018, oen_bie=0.018, hyp_per=0.014, eri_can=0.014, tri_rep=0.012,
                   ran_acr=0.01, pla_lan=0.01, gal_ver=0.009, tri_pra=0.008), 0.10,
              patch=5.0, scattered=0.3, forb_patch=2.5, litter=1.2, litter_h=[0.1, 0.26], litter_w=0.005),
    # Wet floodplain meadows (clusters 10, 11): Plantago lanceolata 79-86, Trifolium repens 70-100, T. pratense 54-86,
    # Vicia 23-86, Lathyrus 79, Taraxacum 30-79, Silene flos-cuculi 50-73, Prunella 61-64, Ranunculus acris 53-57,
    # Rumex acetosella 60, R. acetosa 47, Centaurea jacea 31-50, Galium verum 34-43, Lythrum 43, Equisetum 29.
    community('wet_meadow', ['PG_TUFT'],
              dict(des_ces=0.12, alo_pra=0.10, agr_gig=0.08, poa_pra=0.07, car_hir=0.07, fes_rub=0.06, car_acu=0.03,
                   jun_eff=0.02),
              dict(pla_lan=0.05, tri_rep=0.05, tri_pra=0.045, ran_acr=0.035, sil_flo=0.03, pru_vul=0.03,
                   tar_off=0.025, vic_cra=0.025, rum_acl=0.02, rum_acs=0.02, cen_jac=0.02, gal_ver=0.02,
                   lat_pra=0.015, lyt_sal=0.012, equ_arv=0.01, ach_mil=0.005), 0.12,
              patch=4.0, scattered=0.3, forb_patch=2.0, litter=1.0, litter_h=[0.1, 0.24], litter_w=0.004),
    # Tall-sedge marsh (cluster 9): Lysimachia vulgaris 27 (64 in the alder swamps), Ranunculus repens 55 (as R. acris),
    # Lythrum, horsetails.
    community('sedge_marsh', ['PG_RUSH'],
              dict(car_acu=0.42, gly_max=0.14, pha_aru=0.08, agr_gig=0.07, phr_aus=0.04, typ_lat=0.02, jun_eff=0.02,
                   des_ces=0.02),
              dict(lys_vul=0.025, lyt_sal=0.02, ran_acr=0.012, equ_arv=0.012), 0.1,
              patch=8.0, scattered=0.15, forb_patch=3.0, litter=0.8, litter_h=[0.14, 0.32], litter_w=0.005),
    community('reed_bed', [],
              dict(phr_aus=0.66, typ_lat=0.08, pha_aru=0.05, car_acu=0.05, gly_max=0.04, cal_epi=0.02, jun_eff=0.01),
              dict(urt_dio=0.02, lys_vul=0.012, lyt_sal=0.01), 0.06,
              patch=10.0, scattered=0.1, forb_patch=3.0, litter=0.5, litter_h=[0.2, 0.4], litter_w=0.008),
    # Corynephorus sands (cluster 14, with 15): Oenothera 63-84, Erigeron 44-84, Jasione 44, Rumex acetosella 44,
    # Helichrysum 41, A. campestris 41, Solidago virgaurea 41, Hypericum 37, Achillea 26, Verbascum 26, Berteroa 22.
    community('sand', [],
              dict(cor_can=0.30, fes_ovi=0.12, agr_cap=0.06, poa_ang=0.06, ely_rep=0.05, cal_epi=0.05, car_hir=0.05,
                   poa_pra=0.02),
              dict(oen_bie=0.035, rum_acl=0.03, jas_mon=0.025, eri_can=0.025, art_cam=0.025, hel_are=0.02,
                   sol_vir=0.018, hyp_per=0.018, ach_mil=0.014, ver_lyc=0.014, ber_inc=0.012, rum_acs=0.01), 0.10,
              patch=4.0, scattered=0.4, forb_patch=2.0, litter=0.3, litter_h=[0.06, 0.16], litter_w=0.003, cover=0.8),
    # Verges (no ChEZ relevé: Litopys and the couch-grass fallow, cluster 19): wormwoods, tansy, Berteroa, Oenothera,
    # Erigeron, Solidago, Cirsium, Daucus, Silene, Achillea, Vicia, Verbascum, Chelidonium near the villages.
    community('ruderal_verge', ['PG_DRY'],
              dict(ely_rep=0.22, cal_epi=0.09, bro_ine=0.07, dac_glo=0.07, poa_ang=0.05, ani_tec=0.05, poa_pra=0.04,
                   agr_gig=0.02),
              dict(art_vul=0.04, art_abs=0.035, tan_vul=0.03, ber_inc=0.025, oen_bie=0.025, eri_can=0.025,
                   sol_can=0.02, cir_arv=0.02, dau_car=0.015, sil_lat=0.015, ach_mil=0.015, vic_cra=0.01,
                   ver_lyc=0.01, che_maj=0.01, urt_dio=0.01, tri_rep=0.01), 0.1,
              patch=4.0, scattered=0.25, forb_patch=2.0, litter=1.0, litter_h=[0.1, 0.24], litter_w=0.005),
    # Trampled ground (cluster 16, with the young fallow 20): Plantago major 50, Trifolium repens 17-38, Taraxacum,
    # Plantago lanceolata, Achillea, Prunella, Ranunculus.
    community('lawn', [],
              dict(poa_pra=0.28, fes_rub=0.08, ely_rep=0.07, agr_cap=0.05, poa_ang=0.05, dac_glo=0.05, agr_gig=0.03,
                   phl_pra=0.02),
              dict(pla_maj=0.06, tri_rep=0.05, tar_off=0.03, pla_lan=0.03, ach_mil=0.02, pru_vul=0.015, ran_acr=0.01,
                   eri_can=0.01), 0.04,
              patch=2.0, scattered=0.4, forb_patch=1.5, litter=0.4, litter_h=[0.05, 0.12], litter_w=0.003),
    # Pine forest floor (clusters 2-5) with its clearings and burns (8): Convallaria 41-47, Solidago virgaurea 85,
    # Hypericum 47-60, fireweed 33 on burns, Rumex acetosella 73 on burns, Jasione 40, Erigeron 80 on burns, Urtica.
    community('forest_floor', ['KS_FERN'],
              dict(fes_rub=0.10, cal_epi=0.07, car_hir=0.06, cal_aru=0.04, fes_ovi=0.04, ave_fle=0.03, poa_ang=0.01,
                   dac_glo=0.01),
              dict(con_maj=0.05, sol_vir=0.03, hyp_per=0.015, cha_ang=0.015, rum_acl=0.01, jas_mon=0.008,
                   eri_can=0.008, urt_dio=0.008), 0.56,
              patch=4.0, scattered=0.3, forb_patch=4.0, litter=0.3, litter_h=[0.08, 0.2], litter_w=0.004,
              presence=0.6, cover=0.55),
    # The stock forb cards: a flowering hay meadow (the floodplain meadow 11 and the young fallow 20): clovers,
    # vetches, Achillea, Galium, Plantago, Ranunculus, knapweed, dandelion, campions, St John's wort, sorrel,
    # bellflower, wild carrot.
    community('forb_meadow', ['KS_FORB'],
              dict(dac_glo=0.05, poa_pra=0.05, ely_rep=0.05, agr_cap=0.04, fes_rub=0.04, phl_pra=0.03, car_hir=0.03,
                   alo_pra=0.02),
              dict(tri_pra=0.06, vic_cra=0.05, ach_mil=0.05, gal_ver=0.05, pla_lan=0.04, lat_pra=0.035,
                   tri_rep=0.035, ran_acr=0.03, cen_jac=0.03, tar_off=0.025, sil_lat=0.025, hyp_per=0.025,
                   rum_acs=0.02, cam_pat=0.015, dau_car=0.015, sil_flo=0.015), 0.12,
              patch=3.0, scattered=0.35, forb_patch=1.5, litter=1.0, litter_h=[0.1, 0.24], litter_w=0.005),
    # The stock herb cards: tall herbs of the yards and village edges (clusters 17, 18): Urtica 71-88, Chelidonium
    # 76, Anthriscus 24-27, A. vulgaris 27, Cirsium 27, Silene 46, Erigeron 31, tansy, goldenrod, wild carrot.
    community('tall_herbs', ['KS_HERB'],
              dict(ely_rep=0.09, cal_epi=0.05, dac_glo=0.04, poa_pra=0.03, bro_ine=0.02, ani_tec=0.01),
              dict(urt_dio=0.25, che_maj=0.05, art_vul=0.05, ant_syl=0.05, cir_arv=0.03, tan_vul=0.02, sil_lat=0.02,
                   sol_can=0.02, eri_can=0.015, dau_car=0.01), 0.06,
              patch=3.0, scattered=0.3, forb_patch=5.0, litter=1.0, litter_h=[0.1, 0.26], litter_w=0.005),
    # The stock horsetail cards: ditch and pond margins: horsetails, yellow and purple loosestrife, nettle,
    # buttercup, ragged robin.
    community('wet_margin', ['KS_HORSETAIL'],
              dict(phr_aus=0.14, typ_lat=0.12, car_acu=0.12, gly_max=0.10, pha_aru=0.10, agr_gig=0.06, jun_eff=0.05,
                   des_ces=0.02),
              dict(equ_arv=0.05, lys_vul=0.04, lyt_sal=0.04, urt_dio=0.02, ran_acr=0.015, sil_flo=0.01), 0.04,
              patch=5.0, scattered=0.2, forb_patch=3.0, litter=0.8, litter_h=[0.12, 0.3], litter_w=0.005),
    # The stock "bare twig bushes and branchy dead stalks" (KS_SHRUB): the couch-grass fallow (cluster 19), whose tall
    # forbs' standing dead stems are what those cards drew: A. absinthium 80, Galium verum 80, Silene 73, Hypericum
    # 67, Rumex acetosa 60, Verbascum 53, Achillea 47, Vicia 47, Berteroa 40, Daucus 40, Campanula patula 40,
    # Oenothera 40, Equisetum 40, Erigeron 33; fireweed and mugwort patches.
    community('forb_fallow', ['KS_SHRUB'],
              dict(cal_epi=0.30, ely_rep=0.14, poa_ang=0.08, dac_glo=0.04, agr_cap=0.03, bro_ine=0.03, car_hir=0.03,
                   poa_pra=0.03),
              dict(art_abs=0.045, gal_ver=0.045, sil_lat=0.04, hyp_per=0.035, rum_acs=0.03, ver_lyc=0.03,
                   ach_mil=0.025, vic_cra=0.025, ber_inc=0.02, dau_car=0.02, cam_pat=0.02, oen_bie=0.02,
                   equ_arv=0.015, eri_can=0.015, art_vul=0.015, cha_ang=0.012), 0.08,
              patch=8.0, scattered=0.15, forb_patch=2.5, litter=1.4, litter_h=[0.12, 0.3], litter_w=0.005),
]

# ---------------------------------------------------------------------------------------------------------------
# Colours from the photos (species_photo_colours.json): the season colours of the species' template stay the game's
# calibrated level (the scene references of the ChEZ, keyframes.json); each species takes the chromaticity of its own
# photos against the median of all 40 and half of its relative darkness, per phenophase. Heads and flowers take the
# photo colours of their stage directly (a little darker: a cluster of grains has shade inside).
# ---------------------------------------------------------------------------------------------------------------
BUCKET = ['winter', 'winter', 'winter', 'spring', 'spring', 'early_summer', 'mid_summer', 'late_summer', 'late_summer',
          'autumn', 'autumn', 'winter']
NEAR = {'winter': ['winter', 'autumn', 'late_summer'], 'spring': ['spring', 'early_summer', 'mid_summer'],
        'early_summer': ['early_summer', 'mid_summer', 'spring'], 'mid_summer': ['mid_summer', 'early_summer', 'late_summer'],
        'late_summer': ['late_summer', 'mid_summer', 'autumn'], 'autumn': ['autumn', 'late_summer', 'winter']}
BUCKETS = list(NEAR)
DARK_HEADS = {'typ_lat', 'car_acu', 'car_hir', 'jun_eff'}
# Species whose photos read glaucous from the sheen of their hairs or wet leaves, not from a bloom: their leaf stays
# the strict leaf mask's colour.
NOT_GLAUCOUS = {'urt_dio', 'sil_flo', 'sol_vir', 'pru_vul'}
# Forb heads by stage: (photo part, buckets in order of preference); 'mix:<part>:<t>' = the summer leaf (game) mixed
# t of the way to that photo part, 'mix2:<a>:<b>:<t>' = photo part a mixed t of the way to part b (a ribwort spike
# in flower: the dark spike in its ring of pale anthers).
FORB_HEADS = {
    'sol_can': {'0.5': ('yellow', ['late_summer', 'autumn']), '0.75': ('pale', ['autumn', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'art_vul': {'0.5': ('mix:tan:0.55', ['mid_summer', 'late_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'art_abs': {'0.5': ('yellow', ['mid_summer', 'late_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'art_cam': {'0.5': ('mix:tan:0.6', ['late_summer', 'mid_summer']), '0.75': ('tan', ['autumn', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'tan_vul': {'0.5': ('yellow', ['mid_summer', 'late_summer']), '0.75': ('tan', ['autumn', 'late_summer']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'ach_mil': {'0.5': ('white', ['mid_summer', 'early_summer', 'late_summer']), '0.75': ('tan', ['autumn', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'pla_lan': {'0.5': ('mix2:dark:pale:0.45', ['early_summer', 'mid_summer']), '0.75': ('dark', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'pla_maj': {'0.5': ('mix:tan:0.45', ['mid_summer', 'late_summer']), '0.75': ('dark', ['autumn', 'late_summer']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'urt_dio': {'0.5': ('mix:tan:0.3', ['mid_summer', 'late_summer']), '0.75': ('tan', ['autumn', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'ant_syl': {'0.5': ('white', ['early_summer', 'spring']), '0.75': ('dark', ['mid_summer', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'eri_can': {'0.5': ('white', ['late_summer', 'mid_summer']), '0.75': ('pale', ['autumn', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'cha_ang': {'0.5': ('pink', ['mid_summer', 'early_summer']), '0.75': ('white', ['late_summer', 'autumn']),
                '1.0': ('pale', ['winter', 'autumn'])},
    'lyt_sal': {'0.5': ('pink', ['mid_summer', 'late_summer']), '0.75': ('tan', ['autumn', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'hyp_per': {'0.5': ('yellow', ['early_summer', 'mid_summer']), '0.75': ('stem', ['autumn', 'late_summer']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'gal_ver': {'0.5': ('yellow', ['mid_summer', 'early_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'con_maj': {'0.5': ('white', ['spring', 'early_summer']), '0.75': ('red', ['autumn', 'late_summer']),
                '1.0': ('red', ['winter', 'autumn'])},
    'tri_pra': {'0.5': ('pink', ['early_summer', 'mid_summer']), '0.75': ('dark', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'vic_cra': {'0.5': ('blue', ['early_summer', 'mid_summer']), '0.75': ('dark', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'tri_rep': {'0.5': ('white', ['early_summer', 'mid_summer', 'spring']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('dark', ['autumn', 'winter'])},
    'tar_off': {'0.5': ('yellow', ['spring', 'early_summer']), '0.75': ('white', ['early_summer', 'spring']),
                '1.0': ('tan', ['late_summer', 'autumn'])},
    'lat_pra': {'0.5': ('yellow', ['early_summer', 'mid_summer']), '0.75': ('dark', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'ran_acr': {'0.5': ('yellow', ['early_summer', 'mid_summer', 'spring']), '0.75': ('mix:tan:0.5', ['mid_summer', 'late_summer']),
                '1.0': ('tan', ['late_summer', 'autumn'])},
    'sil_lat': {'0.5': ('white', ['mid_summer', 'early_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('tan', ['autumn', 'winter'])},
    'sil_flo': {'0.5': ('pink', ['early_summer', 'spring']), '0.75': ('tan', ['mid_summer', 'late_summer']),
                '1.0': ('dark', ['autumn', 'winter'])},
    'cen_jac': {'0.5': ('pink', ['mid_summer', 'late_summer']), '0.75': ('dark', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'rum_acl': {'0.5': ('mix2:purple:red:0.5', ['early_summer', 'spring', 'mid_summer']),
                '0.75': ('stem', ['mid_summer', 'late_summer']), '1.0': ('dark', ['late_summer', 'autumn'])},
    'rum_acs': {'0.5': ('mix2:tan:red:0.45', ['early_summer', 'spring', 'mid_summer']),
                '0.75': ('tan', ['mid_summer', 'late_summer']), '1.0': ('dark', ['late_summer', 'autumn'])},
    'oen_bie': {'0.5': ('yellow', ['mid_summer', 'late_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    # Its photos' pink is three dark close-ups of bracts and florets: the lilac of the heads stays the ramp's.
    'cir_arv': {'0.75': ('white', ['late_summer', 'autumn']),
                '1.0': ('tan', ['autumn', 'winter'])},
    'jas_mon': {'0.5': ('blue', ['early_summer', 'mid_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('dark', ['autumn', 'winter'])},
    'hel_are': {'0.5': ('yellow', ['mid_summer', 'late_summer']), '0.75': ('yellow', ['late_summer', 'autumn']),
                '1.0': ('tan', ['autumn', 'winter'])},
    'ber_inc': {'0.5': ('white', ['mid_summer', 'early_summer']), '0.75': ('mix:tan:0.5', ['late_summer', 'autumn']),
                '1.0': ('tan', ['autumn', 'winter'])},
    'ver_lyc': {'0.5': ('yellow', ['mid_summer', 'early_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'sol_vir': {'0.5': ('yellow', ['late_summer', 'mid_summer']), '0.75': ('pale', ['autumn', 'late_summer']),
                '1.0': ('tan', ['winter', 'autumn'])},
    'pru_vul': {'0.5': ('blue', ['mid_summer', 'late_summer']), '0.75': ('dark', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'dau_car': {'0.5': ('white', ['mid_summer', 'late_summer']), '0.75': ('tan', ['autumn', 'late_summer']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'cam_pat': {'0.5': ('blue', ['early_summer', 'mid_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('tan', ['autumn', 'winter'])},
    'equ_arv': {},
    'lys_vul': {'0.5': ('yellow', ['mid_summer', 'late_summer']), '0.75': ('tan', ['late_summer', 'autumn']),
                '1.0': ('dark', ['winter', 'autumn'])},
    'che_maj': {'0.5': ('yellow', ['early_summer', 'mid_summer']), '0.75': ('mix:tan:0.4', ['mid_summer', 'late_summer']),
                '1.0': ('tan', ['autumn', 'winter'])},
}


def s2l(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def l2s(c):
    c = min(max(c, 0.0), 1.0)
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def hl(h):
    return [s2l(int(h[i:i + 2], 16) / 255.0) for i in (1, 3, 5)]


def lh(c):
    return '#%02X%02X%02X' % tuple(int(round(255 * l2s(x))) for x in c)


def Y(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def mix(a, b, t):
    return [x + (y - x) * t for x, y in zip(a, b)]


def part(pc, kind, buckets, min_n=3):
    for b in buckets:
        row = pc['buckets'].get(b, {})
        if kind == 'flower':
            v = row.get('flower')
        else:
            v = row.get(kind)
        if v and v[1] >= min_n:
            return hl(v[0])
    return None


def leaf_of(pc, b):
    """The live leaf of bucket b (or the nearest one with photos), turned towards the glaucous part of the foliage
    by how glaucous the species is (the strict leaf mask drops the blue-grey of Corynephorus, Elymus, fescues)."""
    leaf = part(pc, 'leaf', NEAR[b])
    if leaf is None:
        return None
    gi = 0.0 if pc.get('id') in NOT_GLAUCOUS else (pc.get('glaucous_index') or 0.0)
    gl = part(pc, 'glauc', NEAR[b], 2)
    if gl is not None and gi > 0.32:
        leaf = mix(leaf, gl, min((gi - 0.32) / 0.6, 0.5))
    # Silvery foliage (wormwoods, grey hair-grass): the strict leaf mask keeps only its greenest pixels; a dull
    # leaf turns towards the silver part of the plant.
    sv = part(pc, 'silver', NEAR[b], 2)
    srgb = [l2s(x) for x in leaf]
    sat = (max(srgb) - min(srgb)) / max(max(srgb), 1e-4)
    if sv is not None and sat < 0.4:
        leaf = mix(leaf, sv, min((0.4 - sat) / 0.2, 0.45))
    return leaf


def transfer(tpl, photo, med, lum=0.5, chroma=0.85):
    """The template colour (game level) with the photo's chromaticity against the median and lum of its darkness."""
    if photo is None:
        return tpl
    ys, ym, yt = max(Y(photo), 1e-4), max(Y(med), 1e-4), max(Y(tpl), 1e-4)
    cr = [min(max(1 + chroma * ((p / ys) / max(m / ym, 1e-4) - 1), 0.5), 1.8) for p, m in zip(photo, med)]
    out = [t * c for t, c in zip(tpl, cr)]
    target = yt * min(max((ys / ym) ** lum, 0.75), 1.3)
    k = target / max(Y(out), 1e-5)
    return [min(x * k, 1.0) for x in out]


# The flowers' photo colours are medians over the flower in the frame, its shaded petals included, so they come out
# darker than the petals are. A petal reflects about this much light at least (luminance, linear): white petals
# 0.6-0.8, yellow ones 0.5-0.6 above 520 nm, pink 0.25-0.4, blue and violet 0.15-0.25, dark red 0.1-0.15; a photo
# colour below that keeps its hue and rises to it.
FLOWER_Y = {'white': 0.55, 'yellow': 0.42, 'pink': 0.22, 'blue': 0.14, 'red': 0.09}


def petal(c, kind):
    if c is None:
        return None
    y = Y(c)
    k = FLOWER_Y[kind] / y if 0.0 < y < FLOWER_Y[kind] else 1.0
    return [min(x * k, 1.0) for x in c]


def calibrate(species, photo, kf):
    # Cross-species medians of the live and dead leaf per bucket (species with 3 photos or more).
    med = {}
    for kind in ('leaf', 'dead'):
        for b in BUCKETS:
            vals = [hl(p['buckets'][b][kind][0]) for p in photo.values()
                    if b in p['buckets'] and kind in p['buckets'][b] and p['buckets'][b][kind][1] >= 3]
            med[(kind, b)] = [sorted(v[ch] for v in vals)[len(vals) // 2] for ch in range(3)]
    report = []
    for s in species:
        pc = dict(photo[s['id']], id=s['id'])
        tpl_id = s['template'] or s['phen']['climate']
        rows = [m['grass'][tpl_id] for m in kf['months']]
        # The live greens of every species start from the meadow's (the photos carry the species' own: a sand or
        # reed template's grey-green would count twice); dead tissue from the species' template.
        meadow = [m['grass']['P2_MEADOW_MIX'] for m in kf['months']]
        live, yellowing, straw, thatch = [], [], [], []
        for m in range(12):
            b = BUCKET[m]
            live.append(lh(transfer(hl(meadow[m]['live_color']), leaf_of(pc, b), med[('leaf', b)])))
            dead = part(pc, 'dead', NEAR[b])
            straw.append(lh(transfer(hl(rows[m]['straw_color']), dead, med[('dead', b)])))
            yellowing.append(lh(transfer(hl(rows[m]['yellowing_color']), part(pc, 'dead', NEAR['autumn']),
                                         med[('dead', 'autumn')])))
            thatch.append(lh(transfer(hl(rows[m]['thatch_color']), part(pc, 'dead', NEAR['winter']),
                                      med[('dead', 'winter')])))
        s['colours'] = dict(live=live, yellowing=yellowing, straw=straw, thatch=thatch)
        # Heads and flowers by stage.
        summer = hl(live[6])
        ramp = dict(s['seed_ramp'])
        head = lambda kind, bs, k=0.85: (lambda c: [x * k for x in c] if c is not None else None)(part(pc, kind, bs, 2))
        if s['form'] in (1, 2):
            plan = FORB_HEADS[s['id']]
            for st, (kind, bs) in plan.items():
                if kind.startswith('mix:'):
                    _, pk, t = kind.split(':')
                    c = head(pk, bs)
                    c = mix(summer, c, float(t)) if c is not None else None
                elif kind.startswith('mix2:'):
                    _, pa, pb, t = kind.split(':')
                    a, b = head(pa, bs), head(pb, bs)
                    c = mix(a, b, float(t)) if a is not None and b is not None else (a if a is not None else b)
                elif kind in FLOWER_Y:
                    c = petal(head(kind, bs, 1.0), kind)
                else:
                    c = head(kind, bs, 0.85)
                if c is not None:
                    ramp[st] = lh(c)
            ramp['0.25'] = lh(mix(summer, hl(ramp['0.5']), 0.2))
        else:
            dark = s['id'] in DARK_HEADS
            ripe = head('dark' if dark else 'tan', ['late_summer', 'autumn', 'mid_summer'], 1.0 if dark else 0.85)
            old = head('dark' if dark else 'tan', ['winter', 'autumn'], 1.0 if dark else 0.85)
            tan_mid = head('tan', ['mid_summer', 'early_summer', 'late_summer'])
            purple = head('purple', ['early_summer', 'mid_summer'])
            n_purple = max((pc['buckets'].get(b, {}).get('purple', ['', 0])[1] for b in ('early_summer', 'mid_summer')),
                           default=0)
            if dark:
                anth = head('dark', ['mid_summer', 'early_summer', 'late_summer'], 1.0)
            elif purple is not None and n_purple >= 4 and tan_mid is not None:
                anth = mix(tan_mid, purple, 0.6)
            elif tan_mid is not None:
                anth = mix(tan_mid, summer, 0.3)
            else:
                anth = None
            if anth is not None:
                ramp['0.5'] = lh(anth)
            if ripe is not None:
                ramp['0.75'] = lh(ripe)
            if old is not None:
                ramp['1.0'] = lh(old)
            ramp['0.25'] = lh(mix(summer, hl(ramp['0.5']), 0.35))
        s['seed_ramp'] = ramp
        report.append('%-8s live Jul %s  straw Sep %s  thatch %s  heads %s' % (
            s['id'], live[6], straw[8], thatch[0], ' '.join(ramp[k] for k in ('0.25', '0.5', '0.75', '1.0'))))
    return report


def main():
    ids = [s['id'] for s in S]
    assert len(ids) == len(set(ids)) and len(ids) <= 64
    for s in S:
        s.update(TRAITS.get(s['id'], {}))
        s.update(LANES.get(s['id'], {}))
        if s['id'] in FOLDS:
            s['fold_h'] = FOLDS[s['id']]
    photo = json.load(open(os.path.join(HERE, 'species_photo_colours.json'), encoding='utf-8'))['species']
    kf = json.load(open(os.path.join(HERE, 'keyframes.json'), encoding='utf-8'))
    for line in calibrate(S, photo, kf):
        print(line)
    for c in C:
        for k in list(c['species']) + list(c['forbs']):
            assert k in ids, (c['name'], k)
        for k in c['species']:
            assert next(s for s in S if s['id'] == k)['form'] in (0, 3), (c['name'], k)
        for k in c['forbs']:
            assert next(s for s in S if s['id'] == k)['form'] in (1, 2), (c['name'], k)
        assert len(c['species']) <= 8 and len(c['forbs']) <= 16, c['name']
    meta = dict(what='Species and communities of the procedural grass (dead_air_x64_seasons.ltx [grass_species_N], '
                     '[grass_keys_N], [grass_community_N], [grass_class_community])',
                generator='tools/seasons/data/make_species.py')
    with open(os.path.join(HERE, 'species.json'), 'w', encoding='utf-8', newline='\r\n') as f:
        json.dump(dict(meta=meta, species=S), f, indent=1)
        f.write('\n')
    with open(os.path.join(HERE, 'communities.json'), 'w', encoding='utf-8', newline='\r\n') as f:
        json.dump(dict(meta=meta, communities=C), f, indent=1)
        f.write('\n')
    print('%d species, %d communities' % (len(S), len(C)))


if __name__ == '__main__':
    main()
