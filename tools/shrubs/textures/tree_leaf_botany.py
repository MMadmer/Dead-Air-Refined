# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""The trees' leaves and needles as the texture generator draws them (docs/dead-air/TREES_TEXTURES.md): per tree
species its leaf kind, the sizes (m) the layers are drawn at and the element sizes their squares are fitted to (the
same keys the shrubs' species carry in tools/shrubs/make_shrubs.py, so tools/trees/make_trees.py can take them as
they are), and its spray - the stretch of shoot with its leaves the spray card shows for the far tiers.

Ids are tree_colours.json's (the trees' research); one colliding with a shrub id takes a 't_' prefix (t_pru_cer, the
sour cherry grown as a tree: it wears the shrub's own leaf layers - one species, one leaf).

arch keys: leaf_kind simple | lobed | compound | palmate | needles; leaf_len, leaf_wid [min, max] (a simple leaf's
blade, a compound leaf's leaflet); petiole; compound: leaflets, rachis, leaflet_angle (deg); needles: needle_len,
leaf_spacing (the tuft's stretch of shoot); the spray (far tiers, cards): leaflets (leaves on it; a compound
species' spray: card_leaves), leaf_spacing, leaf_per_node, leaf_phyllo (deg), leaf_angle [min, max] (deg from the
shoot), spray_len.
"""


def cm(a, b=None):
    return [round(a / 100.0, 4), round((b if b is not None else a) / 100.0, 4)]


def spray(n, spacing, per=1, phyllo=137.5, angle=(40, 60)):
    """The spray card: n leaves at `spacing` m along a shoot (per: leaves a node), their phyllotaxis and their angle
    from the shoot."""
    return dict(leaflets=n, leaf_spacing=spacing, leaf_per_node=per, leaf_phyllo=phyllo, leaf_angle=list(angle),
                spray_len=round(spacing * n / per, 3))


def sp(latin, kind, leaf_len, leaf_wid, petiole, card=None, **kw):
    a = dict(leaf_kind=kind, leaf_len=leaf_len, leaf_wid=leaf_wid, petiole=petiole)
    a.update(card or {})
    draw = {}
    if 'leaflet_ratio' in kw:
        draw['leaflet_ratio'] = kw.pop('leaflet_ratio')
    a.update(kw)
    return dict(latin=latin, arch=a, draw=draw)


TREES = {
    # ---- Conifers: needle tufts (a stretch of shoot in its needles), the card a branch tip's spray.
    'pin_syl': sp('Pinus sylvestris', 'needles', cm(4, 7), cm(0.15, 0.18), 0.0, needle_len=0.052, leaf_spacing=0.1,
                  card_len=0.32, card_wid=0.26),
    'pic_abi': sp('Picea abies', 'needles', cm(1.5, 2.5), cm(0.1, 0.12), 0.0, needle_len=0.02, leaf_spacing=0.07,
                  card_len=0.3, card_wid=0.2),
    # ---- Native forest trees.
    'bet_pen': sp('Betula pendula', 'simple', cm(4, 6.5), cm(3, 4.5), 0.022,
                  spray(5, 0.018, phyllo=150, angle=(45, 65))),
    'bet_pub': sp('Betula pubescens', 'simple', cm(3.5, 5.5), cm(2.8, 4.2), 0.015, spray(5, 0.018, phyllo=150,
                                                                                         angle=(45, 65))),
    'pop_tre': sp('Populus tremula', 'simple', cm(3.5, 6.5), cm(3.5, 6.5), 0.045, spray(4, 0.012, angle=(50, 75))),
    'que_rob': sp('Quercus robur', 'lobed', cm(7, 12), cm(4, 7), 0.005, spray(6, 0.012, phyllo=144, angle=(40, 70))),
    'car_bet': sp('Carpinus betulus', 'simple', cm(5.5, 9.5), cm(3, 5), 0.01, spray(6, 0.025, phyllo=180,
                                                                                  angle=(50, 70))),
    'ace_pla': sp('Acer platanoides', 'lobed', cm(8, 13), cm(10, 16), 0.08, spray(4, 0.04, per=2, phyllo=90,
                                                                                angle=(45, 65))),
    'til_cor': sp('Tilia cordata', 'simple', cm(4, 7), cm(3.5, 6.5), 0.03, spray(6, 0.025, phyllo=180, angle=(50, 70))),
    'fra_exc': sp('Fraxinus excelsior', 'compound', cm(5, 10), cm(1.5, 3), 0.05, leaflets=11, rachis=0.18,
                  leaflet_angle=60, leaflet_ratio=0.3, card_leaves=4, card_per_node=2, card_spacing=0.05),
    'ulm_lae': sp('Ulmus laevis', 'simple', cm(6, 11), cm(3.5, 6), 0.005, spray(6, 0.022, phyllo=180, angle=(50, 70))),
    'ulm_min': sp('Ulmus minor', 'simple', cm(4, 8), cm(2.2, 4.5), 0.006, spray(7, 0.015, phyllo=180, angle=(50, 70))),
    'sor_auc': sp('Sorbus aucuparia', 'compound', cm(3, 5.5), cm(1, 1.8), 0.03, leaflets=13, rachis=0.12,
                  leaflet_angle=70, leaflet_ratio=0.33, card_leaves=4, card_per_node=1, card_spacing=0.03),
    # ---- Floodplain and wetland trees.
    'aln_glu': sp('Alnus glutinosa', 'simple', cm(4, 9), cm(3.5, 7.5), 0.015,
                  spray(5, 0.02, phyllo=120, angle=(45, 70))),
    'sal_alb': sp('Salix alba', 'simple', cm(6, 10), cm(1, 1.8), 0.006, spray(8, 0.012, angle=(30, 50))),
    'sal_fra': sp('Salix x fragilis', 'simple', cm(9, 15), cm(1.5, 3), 0.01, spray(7, 0.015, angle=(35, 55))),
    'pop_nig': sp('Populus nigra', 'simple', cm(5, 9), cm(4.5, 8.5), 0.04, spray(4, 0.015, angle=(45, 70))),
    'pop_alb': sp('Populus alba', 'lobed', cm(5, 10), cm(4.5, 9), 0.035, spray(4, 0.018, angle=(45, 70))),
    'pop_can': sp('Populus x canadensis', 'simple', cm(7, 10), cm(6.5, 9.5), 0.05, spray(4, 0.018, angle=(45, 70))),
    # ---- Village, roadside and planted trees.
    'rob_pse': sp('Robinia pseudoacacia', 'compound', cm(2.5, 4.5), cm(1.4, 2.5), 0.03, leaflets=15, rachis=0.17,
                  leaflet_angle=75, leaflet_ratio=0.55, card_leaves=4, card_per_node=1, card_spacing=0.035),
    'ace_neg': sp('Acer negundo', 'compound', cm(5.5, 10), cm(3, 6), 0.06, leaflets=5, rachis=0.08, leaflet_angle=55,
                  leaflet_ratio=0.55, card_leaves=4, card_per_node=2, card_spacing=0.06),
    'ace_pse': sp('Acer pseudoplatanus', 'lobed', cm(8, 13), cm(9, 15), 0.07, spray(4, 0.045, per=2, phyllo=90,
                                                                                  angle=(45, 65))),
    'aes_hip': sp('Aesculus hippocastanum', 'palmate', cm(10, 20), cm(3.5, 7), 0.12, leaflets=7, leaflet_angle=84,
                  leaflet_ratio=0.38, card_leaves=4, card_per_node=2, card_spacing=0.06),
    'que_rub': sp('Quercus rubra', 'lobed', cm(12, 20), cm(8, 13), 0.035, spray(5, 0.02, phyllo=144, angle=(40, 70))),
    'fra_pen': sp('Fraxinus pennsylvanica', 'compound', cm(6, 11), cm(2, 4), 0.045, leaflets=7, rachis=0.13,
                  leaflet_angle=55, leaflet_ratio=0.36, card_leaves=4, card_per_node=2, card_spacing=0.05),
    # ---- Orchard relics.
    'mal_dom': sp('Malus domestica', 'simple', cm(4.5, 9), cm(3, 5.5), 0.02,
                  spray(5, 0.008, phyllo=144, angle=(40, 65))),
    'pyr_com': sp('Pyrus communis', 'simple', cm(3.5, 7.5), cm(2.5, 5), 0.03, spray(5, 0.008, phyllo=144,
                                                                                 angle=(40, 65))),
    'pru_dom': sp('Prunus domestica', 'simple', cm(4.5, 9.5), cm(2.5, 5), 0.015, spray(5, 0.012, angle=(40, 65))),
    'jug_reg': sp('Juglans regia', 'compound', cm(7, 14), cm(3.5, 6.5), 0.06, leaflets=7, rachis=0.18, leaflet_angle=65,
                  leaflet_ratio=0.48, card_leaves=3, card_per_node=1, card_spacing=0.05),
}

# Tree species wearing a shrub species' leaf layers (the same species grown as a tree).
SHARED = {'t_pru_cer': 'pru_cer'}

# Every tree species of the research, in its order (tree_colours.json), the shared ones included.
ORDER = ['pin_syl', 'pic_abi', 'bet_pen', 'bet_pub', 'pop_tre', 'que_rob', 'car_bet', 'ace_pla', 'til_cor', 'fra_exc',
         'ulm_lae', 'ulm_min', 'sor_auc', 'aln_glu', 'sal_alb', 'sal_fra', 'pop_nig', 'pop_alb', 'pop_can', 'rob_pse',
         'ace_neg', 'ace_pse', 'aes_hip', 'que_rub', 'fra_pen', 'mal_dom', 'pyr_com', 't_pru_cer', 'pru_dom', 'jug_reg']
