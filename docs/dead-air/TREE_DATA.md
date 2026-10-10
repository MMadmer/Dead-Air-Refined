# Tree data (generated)

Written by `tools/trees/make_trees.py` into `configs/dead_air_x64_trees.ltx` (docs/dead-air/TREES.md); the census of the stock trees by `tools/trees/census_trees.py` into `tools/trees/data/tree_kinds.json`. Edit the scripts, never the ltx.

## Census

- Kinds: STOCK 24 (30 keys, 515 placements), TREE 313 (335 keys, 19189 placements), YOUNG 48 (54 keys, 773 placements); 419 keys in `[tree_kinds]`, 375 signatures in `[tree_families]`.
- TREE: every standing tree (alive, dead, broken snags, the anomaly-sculpted and the burnt ones) on its stock skeleton; YOUNG: the oversize stock bushes the shrubs keep stock (tops over `[shrub_policy] max_top`), grown whole as young trees (`skel=p<kind>`); STOCK: the lying and dead-wood models - windfalls, logs, stumps, debris - leafy or not (the season looks of their cards stay: see the last table).
- Anomalies (the sculpted byaka and anomaly trunks): their species or community as any other model's (the cards' species, else the habitat's community); dead when their main cards are bare or dead or they have none, alive with leafy cards. The skeletons keep the stock triangles where the rings miss the sculpting, their bark on the stock uv.
- Burnt trees: with foliage (leafy cards) alive and resprouting, `community=<habitat>_brn` of the variants `<id>_brn` - the species with a charred base: `bark_tex` the char layer, `bark_tex_upper` the species' main bark (its upper layer where its own zone ends by 3.5 m), `bark_zone` the scorch line (0.12 of the tree's height, 1-3 m) blended over 0.6 m; bare ones dead and charred all the way up, `species=<id>_char` (the commonest species of their habitat, `bark_tex_upper = -1`). Char: the char layer (tree_layers.json).
- Dead trees (every `dead` kind but the burnt snags: the dead and broken snags, the bare anomalies, the Red Forest's pines): the variants `<id>_dead` of the species their pick can give them (`species=<id>_dead`, `community=<community>_dead` - conifers for a conifer kind, broadleaves for the others, at the community's shares) wear the dead barks: `deadwood_bark` (a dead pine's bleached plates, a third of them fallen) up the trunk, `deadwood_bare` (weathered silver-grey wood) above the species' own zone, 3-6 m at the least. 10 variants, 5 dead communities.
- Keys: da_shrubs.cpp read_part_keys / kind_of, reproduced by tools/trees/census_trees.py: part key = FNV-1a 64 of the lower-case shader string, its zero, the u32 triangle count of the full-detail window and the wrapping sum of the triangles' hashes (vertices as mm positions and uv mod 2048, sorted per triangle); kind key = FNV-1a 64 over the sorted part keys (u64 LE).
- Parity (the census code on the raw levels): 96 of 96 [shrub_kinds] keys computed; 96 of 96 [shrub_families] rows reproduced exactly; 419 of 419 tree-set keys of the inventory computed; 385 of 385 exported copies: the sidecar part keys equal this code's; the engine's own shrub census in a game log: l01_escape: equal (FLODs, bushes, oversize 2444/1267/28).
- Skeleton ids (`skel=`, the file `gamedata/meshes/da/trees/<skel>.dtsk`): the skeleton builder's index `tools/trees/out/skel_index.json` where it lists the kind (it gives the other card layouts of one trunk mesh skeletons of their own: 4 kinds), else the census's: the wood parts (flora\trunk_wave: bark and cut ends): one part's key, several hashed like a kind key; no wood or a young tree: p<kind key>; 232 distinct among the TREE kinds.
- Species of a kind: its main cards (`species=`; a model in two species' cards takes the larger card area): pine `pin_syl`, spruce `pic_abi`, birch `bet_pen` (`bet_pub` where 60 % of its placements stand in the swamp levels), oak `que_rob`, alder `aln_glu`, cherry `t_pru_cer`, walnut `jug_reg`, willow `sal_alb` (tall, 18 m and more) or `sal_fra`, poplar `pop_nig` (a column: span under 0.4 of the height), `pop_tre` (models under 12 m: the aspen groves), `pop_alb` (tall, on wet ground) or `pop_can` (tall rows). The generic broadleaf cards and the bare dead trees take `community=` of the levels most of their placements stand in (LEVEL_COMMUNITY); the engine draws each placement's species from it (the conifer flag picks conifers, the rest broadleaves, at most `kinds_per_level_species` templates of a kind on a level).
- Flags: `dead` - no live foliage, the grafted twigs bare and grey, the dead barks (a dead pine grey, without needles); `red` - the Red Forest's stock: its tall red-carded pines (12 m and more) stand `dead, red`, the pines killed in 1986, and keep their dead needles in the species' `c_dead` (rust-red: the ginger forest) where a plain dead pine has none; the young red pines and spruces are the replanted generation, alive; the generic red cards take the `red_forest` community (the birch, aspen and alder regrowth); `conifer` - from a community the kind takes conifers only, the others broadleaves only.
- Textures: every species on its own layers, tree layers (tools/shrubs/textures/out/tree_layers.json).

## Keys beyond the shrubs' (for the engine)

| key | where | meaning |
|---|---|---|
| `graft_levels` | species | grafted levels on a stock skeleton (l1..l3: branches, twigs, current shoots); the young trees grow `levels` from the root |
| `crown_fill` | species | share of the crown field the grafted shoots fill |
| `stock_card_follow` | species | how closely the grafted level-1 branches follow the stock card branch axes |
| `bark_tex_upper`, `bark_zone`, `bark_zone_mode` | species | the upper bark layer and the heights (m above the root) over which it takes over from `bark_tex` (`height`; `bark_tex_upper = -1`: one bark all the way up) |
| `cone_share`, `cone_size`, `cone_droop` | conifers | cones on the young growth's tips; their look and season are the fruit keys (`fruit_shape = cone`) |
| `autumn_order` | species | the autumn's order leaf by leaf: the weight of the height (the top first) and of the shade (+ the inner leaves first: birch, aspen, poplars, willows, a conifer's old needles; - the outer ones: maples, oaks, lime) |
| `marcescent` | species | the share of its leaves a young tree keeps withered through the winter, its last to fall (pedunculate oak 0.6, red oak 0.5, hornbeam 0.4); the engine scales it by the tree's youth y, `lerp(0.08, 1, y)` |
| `young_h0`, `young_h1`, `young_lead` | `[tree_policy]` | a tree's youth y by its height (1 up to `young_h0` = 4 m, 0 from `young_h1` = 10 m; 1 for a YOUNG kind and a `sapling` kind): its spring up to `young_lead` = 8 days ahead of the old trees', its marcescence, 1 - y of its flowers, fruit and cones |
| `sapling` | `[tree_kinds]` flag | the stock's sapling models (the inventory's flag): young trees whatever their height |

The trees write no `max_leaves`: the generator sizes a tree's leaves by its own crown (the species' LAI over the crown field, on the share `crown_fill` fills). The leaf elements are the texture botany's (`tools/shrubs/textures/tree_leaf_botany.py`: the sizes the layers are drawn at, their squares fitted to them); `bark_contrast` is the bark type's (`TREE_BARK_CONTRAST`: the plated orchard and floodplain barks, the oak, the pine's plates and the black poplar brought to their photos' fissure contrast, the spruce at half it, the rest 1), `bark_scale` the tile scale of the layer on the most wood.

`[tree_kinds]` carries no flags but `dead`, `red`, `conifer` and `sapling`.

## Inputs a re-run picks up

- `tools/shrubs/textures/out/tree_layers.json` (the tree layers in the shrubs' arrays): `species` - per species id the layers `leaf_a`, `leaf_b`, `card`, `leaflet`, `needles`, `bark`, `bark_upper` with `leaf_fit`, `card_fit`, `leaf_hull` (computed from the layers' `box_mm` as the shrubs' are where an entry has none); `bark_types` - the `char` layer of the burnt variants and every bark layer's tile scale (the species' `bark_scale`: its lower bark's, a burnt variant's main bark's, a snag's char's); the far tiers' bark from `texture_stats.json`. A tree species registered in the shrubs' manifests under its own id counts as well; without either a species borrows the nearest shrub layers (`FALLBACK_LEAF`, `FALLBACK_BARK`, the bark tinted toward its photos) and the char is the darkest shrub bark tinted to #1a1714. `bark_types` gives the dead barks too (`deadwood_bark`, `deadwood_bare`: the dead variants).
- `tools/shrubs/textures/tree_leaf_botany.py`: the leaf elements (sizes, sprays, compound leaves, tufts) the species take.
- `tools/trees/out/skel_index.json` (the skeleton builder): kind -> skeleton; `gamedata/meshes/da/trees/*.dtsk`.
- `tools/trees/data/*.json`: re-run `census_trees.py` only when the levels or the inventory change.

## Species, communities, kinds

| # | id | species | height (m) | leaves | graft | flowers | fruit | catkins / cones | leaf out / full | flowers | ripe | autumn peak | bare | autumn order | bark zone (m) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | pin_syl | *Pinus sylvestris L.* | 15-30 | needles narrow | 3 | cone | cone | cones 4.5 cm | evergreen / - | 133-150 | 285 | 272 | 300 | inner needles (top 0.1, shade +0.7) | 3.5-8 |
| 2 | pic_abi | *Picea abies (L.) H.Karst.* | 18-30 | needles narrow | 3 | cone | cone | cones 13 cm | evergreen / - | 128-146 | 285 | 280 | 305 | inner needles (top 0.1, shade +0.6) | 2-8 |
| 3 | bet_pen | *Betula pendula Roth* | 16-25 | spray (5) ovate | 3 | none | cone | hanging | 110 / 135 | 104-118 | 235 | 279 | 305 | top 0.45, shade +0.4 | 0.5-3.5 |
| 4 | bet_pub | *Betula pubescens Ehrh.* | 12-20 | spray (5) ovate | 3 | none | cone | hanging | 118 / 140 | 113-127 | 240 | 288 | 310 | top 0.45, shade +0.4 | 0.3-1.2 |
| 5 | pop_tre | *Populus tremula L.* | 18-25 | spray (4) round | 3 | none | cotton | hanging | 120 / 142 | 85-105 | 140 | 281 | 303 | top 0.45, shade +0.4 | 0.4-1.8 |
| 6 | que_rob | *Quercus robur L.* | 20-28 | spray (6) lobed | 3 | none | nut | hanging | 127 / 150 | 126-140 | 262 | 300 | 320 | top 0.45, shade -0.3, marcescent 0.6 | 4-10 |
| 7 | car_bet | *Carpinus betulus L.* | 15-20 | spray (6) elliptic | 3 | none | pod | hanging | 118 / 140 | 110-124 | 275 | 297 | 318 | top 0.45, shade -0.2, marcescent 0.4 | - |
| 8 | ace_pla | *Acer platanoides L.* | 15-25 | spray (4) lobed | 3 | corymb | pod | none | 115 / 138 | 103-118 | 260 | 290 | 310 | top 0.45, shade -0.35 | 2-6 |
| 9 | til_cor | *Tilia cordata Mill.* | 15-25 | spray (6) round | 3 | corymb | nut | none | 120 / 142 | 168-185 | 240 | 285 | 303 | top 0.45, shade -0.2 | 2-6 |
| 10 | fra_exc | *Fraxinus excelsior L.* | 15-25 | compound (11) lanceolate | 2 | panicle | pod | none | 135 / 158 | 105-120 | 265 | 295 | 310 | top 0.4, shade +0 | 2-6 |
| 11 | ulm_lae | *Ulmus laevis Pall.* | 15-25 | spray (6) elliptic | 3 | umbel | pod | none | 117 / 140 | 82-100 | 135 | 289 | 310 | top 0.45, shade +0 | - |
| 12 | ulm_min | *Ulmus minor Mill.* | 5-15 | spray (7) elliptic | 3 | umbel | pod | none | 120 / 142 | 88-105 | 128 | 298 | 318 | top 0.45, shade +0 | - |
| 13 | sor_auc | *Sorbus aucuparia L.* | 5-12 | compound (13) lanceolate | 3 | corymb | berry | none | 112 / 135 | 135-150 | 225 | 272 | 298 | top 0.4, shade +0.2 | 0.5-1.5 |
| 14 | aln_glu | *Alnus glutinosa (L.) Gaertn.* | 18-25 | spray (5) round | 3 | none | cone | hanging | 115 / 140 | 65-85 | 270 | 292 | 310 | top 0.3, shade +0.2 | 3-8 |
| 15 | sal_alb | *Salix alba L.* | 15-25 | spray (8) lanceolate | 3 | none | cotton | upright | 112 / 135 | 108-122 | 145 | 305 | 325 | top 0.45, shade +0.4 | 4-10 |
| 16 | sal_fra | *Salix x fragilis L.* | 15-22 | spray (7) lanceolate | 3 | none | cotton | upright | 110 / 133 | 105-118 | 150 | 303 | 322 | top 0.45, shade +0.4 | 4-10 |
| 17 | pop_nig | *Populus nigra L. (incl. var. italica)* | 20-30 | spray (4) ovate | 3 | none | cotton | hanging | 120 / 142 | 88-105 | 148 | 298 | 318 | top 0.45, shade +0.4 | 3-9 |
| 18 | pop_alb | *Populus alba L.* | 20-30 | spray (4) lobed | 3 | none | cotton | hanging | 120 / 142 | 85-102 | 140 | 300 | 320 | top 0.45, shade +0.4 | 1-4 |
| 19 | pop_can | *Populus x canadensis Moench* | 25-35 | spray (4) ovate | 3 | none | none | hanging | 128 / 150 | 95-110 | - | 302 | 322 | top 0.45, shade +0.35 | 3-9 |
| 20 | rob_pse | *Robinia pseudoacacia L.* | 10-20 | compound (15) elliptic | 2 | raceme | pod | none | 137 / 160 | 146-163 | 240 | 300 | 315 | top 0.4, shade +0.2 | - |
| 21 | ace_neg | *Acer negundo L.* | 9-17 | compound (5) ovate | 3 | raceme | pod | none | 110 / 135 | 98-112 | 258 | 290 | 308 | top 0.4, shade +0.1 | 1-3 |
| 22 | ace_pse | *Acer pseudoplatanus L.* | 20-30 | spray (4) lobed | 3 | raceme | pod | none | 118 / 140 | 128-145 | 270 | 293 | 312 | top 0.45, shade -0.35 | 2-5 |
| 23 | aes_hip | *Aesculus hippocastanum L.* | 20-25 | compound (7) ovate | 2 | panicle | capsule | none | 108 / 130 | 125-142 | 255 | 290 | 305 | top 0.45, shade -0.2 | 2-5 |
| 24 | que_rub | *Quercus rubra L.* | 20-25 | spray (5) lobed | 3 | none | nut | hanging | 132 / 155 | 120-135 | 275 | 298 | 322 | top 0.45, shade -0.3, marcescent 0.5 | 3-8 |
| 25 | fra_pen | *Fraxinus pennsylvanica Marshall* | 15-20 | compound (7) lanceolate | 2 | panicle | pod | none | 122 / 145 | 100-115 | 255 | 283 | 303 | top 0.4, shade +0.1 | - |
| 26 | mal_dom | *Malus domestica Borkh.* | 5-10 | spray (5) ovate | 3 | corymb | pome | none | 115 / 138 | 118-133 | 238 | 298 | 318 | top 0.4, shade -0.1 | - |
| 27 | pyr_com | *Pyrus communis L. (incl. P. pyraster)* | 10-20 | spray (5) round | 3 | corymb | pome | none | 112 / 135 | 108-122 | 250 | 290 | 312 | top 0.4, shade -0.1 | - |
| 28 | t_pru_cer | *Prunus cerasus L.* | 3-20 | spray (3) elliptic | 3 | umbel | drupe | none | 110 / 132 | 110-125 | 182 | 295 | 310 | top 0.4, shade -0.2 | - |
| 29 | pru_dom | *Prunus domestica L.* | 4-12 | spray (5) elliptic | 3 | single | drupe | none | 110 / 133 | 108-122 | 235 | 295 | 313 | top 0.4, shade -0.1 | 1-3 |
| 30 | jug_reg | *Juglans regia L.* | 15-25 | compound (7) elliptic | 2 | none | nut | hanging | 130 / 152 | 128-142 | 260 | 288 | 305 | top 0.4, shade +0 | 2-6 |
| 31 | aln_glu_brn | *Alnus glutinosa (L.) Gaertn. - burnt: a charred base* | 18-25 | spray (5) round | 3 | none | cone | hanging | 115 / 140 | 65-85 | 270 | 292 | 310 | top 0.3, shade +0.2 | 0.82-1.42 |
| 32 | bet_pen_brn | *Betula pendula Roth - burnt: a charred base* | 16-25 | spray (5) ovate | 3 | none | cone | hanging | 110 / 135 | 104-118 | 235 | 279 | 305 | top 0.45, shade +0.4 | 0.7-1.3 |
| 33 | bet_pen_char | *Betula pendula Roth - burnt: charred all the way up* | 16-25 | spray (5) ovate | 3 | none | cone | hanging | 110 / 135 | 104-118 | 235 | 279 | 305 | top 0.45, shade +0.4 | - |
| 34 | bet_pub_brn | *Betula pubescens Ehrh. - burnt: a charred base* | 12-20 | spray (5) ovate | 3 | none | cone | hanging | 118 / 140 | 113-127 | 240 | 288 | 310 | top 0.45, shade +0.4 | 0.7-1.3 |
| 35 | fra_pen_brn | *Fraxinus pennsylvanica Marshall - burnt: a charred base* | 15-20 | compound (7) lanceolate | 2 | panicle | pod | none | 122 / 145 | 100-115 | 255 | 283 | 303 | top 0.4, shade +0.1 | 0.82-1.42 |
| 36 | pop_alb_brn | *Populus alba L. - burnt: a charred base* | 20-30 | spray (4) lobed | 3 | none | cotton | hanging | 120 / 142 | 85-102 | 140 | 300 | 320 | top 0.45, shade +0.4 | 0.82-1.42 |
| 37 | pop_nig_brn | *Populus nigra L. (incl. var. italica) - burnt: a charred base* | 20-30 | spray (4) ovate | 3 | none | cotton | hanging | 120 / 142 | 88-105 | 148 | 298 | 318 | top 0.45, shade +0.4 | 0.82-1.42 |
| 38 | pop_tre_brn | *Populus tremula L. - burnt: a charred base* | 18-25 | spray (4) round | 3 | none | cotton | hanging | 120 / 142 | 85-105 | 140 | 281 | 303 | top 0.45, shade +0.4 | 0.7-1.3 |
| 39 | que_rob_brn | *Quercus robur L. - burnt: a charred base* | 20-28 | spray (6) lobed | 3 | none | nut | hanging | 127 / 150 | 126-140 | 262 | 300 | 320 | top 0.45, shade -0.3, marcescent 0.6 | 0.82-1.42 |
| 40 | sal_alb_brn | *Salix alba L. - burnt: a charred base* | 15-25 | spray (8) lanceolate | 3 | none | cotton | upright | 112 / 135 | 108-122 | 145 | 305 | 325 | top 0.45, shade +0.4 | 0.82-1.42 |
| 41 | sal_fra_brn | *Salix x fragilis L. - burnt: a charred base* | 15-22 | spray (7) lanceolate | 3 | none | cotton | upright | 110 / 133 | 105-118 | 150 | 303 | 322 | top 0.45, shade +0.4 | 0.7-1.3 |
| 42 | sor_auc_brn | *Sorbus aucuparia L. - burnt: a charred base* | 5-12 | compound (13) lanceolate | 3 | corymb | berry | none | 112 / 135 | 135-150 | 225 | 272 | 298 | top 0.4, shade +0.2 | 0.7-1.3 |
| 43 | aln_glu_dead | *Alnus glutinosa (L.) Gaertn. - dead: the dead barks* | 18-25 | spray (5) round | 3 | none | cone | hanging | 115 / 140 | 65-85 | 270 | 292 | 310 | top 0.3, shade +0.2 | 3-8 |
| 44 | bet_pen_dead | *Betula pendula Roth - dead: the dead barks* | 16-25 | spray (5) ovate | 3 | none | cone | hanging | 110 / 135 | 104-118 | 235 | 279 | 305 | top 0.45, shade +0.4 | 3-6 |
| 45 | bet_pub_dead | *Betula pubescens Ehrh. - dead: the dead barks* | 12-20 | spray (5) ovate | 3 | none | cone | hanging | 118 / 140 | 113-127 | 240 | 288 | 310 | top 0.45, shade +0.4 | 3-6 |
| 46 | fra_exc_dead | *Fraxinus excelsior L. - dead: the dead barks* | 15-25 | compound (11) lanceolate | 2 | panicle | pod | none | 135 / 158 | 105-120 | 265 | 295 | 310 | top 0.4, shade +0 | 3-6 |
| 47 | pic_abi_dead | *Picea abies (L.) H.Karst. - dead: the dead barks* | 18-30 | needles narrow | 3 | cone | cone | cones 13 cm | evergreen / - | 128-146 | 285 | 280 | 305 | inner needles (top 0.1, shade +0.6) | 3-8 |
| 48 | pin_syl_dead | *Pinus sylvestris L. - dead: the dead barks* | 15-30 | needles narrow | 3 | cone | cone | cones 4.5 cm | evergreen / - | 133-150 | 285 | 272 | 300 | inner needles (top 0.1, shade +0.7) | 3.5-8 |
| 49 | pop_tre_dead | *Populus tremula L. - dead: the dead barks* | 18-25 | spray (4) round | 3 | none | cotton | hanging | 120 / 142 | 85-105 | 140 | 281 | 303 | top 0.45, shade +0.4 | 3-6 |
| 50 | que_rob_dead | *Quercus robur L. - dead: the dead barks* | 20-28 | spray (6) lobed | 3 | none | nut | hanging | 127 / 150 | 126-140 | 262 | 300 | 320 | top 0.45, shade -0.3, marcescent 0.6 | 4-10 |
| 51 | sal_fra_dead | *Salix x fragilis L. - dead: the dead barks* | 15-22 | spray (7) lanceolate | 3 | none | cotton | upright | 110 / 133 | 105-118 | 150 | 303 | 322 | top 0.45, shade +0.4 | 4-10 |
| 52 | sor_auc_dead | *Sorbus aucuparia L. - dead: the dead barks* | 5-12 | compound (13) lanceolate | 3 | corymb | berry | none | 112 / 135 | 135-150 | 225 | 272 | 298 | top 0.4, shade +0.2 | 3-6 |

### Communities

| community | species (share %) | levels whose generic trees take it |
|---|---|---|
| pine_forest | pin_syl 86, bet_pen 9, que_rob 2, pop_tre 2, sor_auc 1 | k02_trucks_cemetery, l10_radar |
| pine_birch | pin_syl 68, bet_pen 18, que_rob 6, pop_tre 4, sor_auc 2, pic_abi 2 | k01_darkscape, l04_darkvalley, l13_generators, new_military |
| birch_aspen | bet_pen 66, pop_tre 16, pin_syl 8, bet_pub 6, sor_auc 2, sal_fra 2 | l02_garbage, l06_rostok |
| alder_swamp | aln_glu 80, bet_pub 14, sal_fra 3, fra_exc 3 | k00_marsh, l08_yantar, l08u_brainlab |
| floodplain | sal_alb 24, sal_fra 18, pop_nig 14, pop_alb 12, pop_tre 10, que_rob 10, fra_pen 6, aln_glu 6 | zaton |
| oak_mixed | que_rob 38, car_bet 18, pin_syl 12, til_cor 10, ace_pla 8, fra_exc 6, ulm_lae 4, pop_tre 4 | l03_agroprom, l03u_agr_underground |
| village | ace_neg 25, pru_dom 15, rob_pse 15, bet_pen 6, mal_dom 6, ace_pla 6, pop_alb 5, pop_tre 4, pyr_com 4, t_pru_cer 4, fra_exc 3, ulm_lae 2 | jupiter, l01_escape |
| orchard | mal_dom 35, pru_dom 25, pyr_com 15, t_pru_cer 12, ace_neg 8, jug_reg 5 | - (for other levels) |
| red_forest | pin_syl 45, bet_pen 32, pop_tre 13, aln_glu 10 | l10_red_forest |
| roadside | pop_can 22, pop_nig 14, bet_pen 14, rob_pse 10, ace_neg 10, til_cor 8, ace_pla 7, fra_pen 6, que_rub 5, aes_hip 4, ulm_min 3 | l12_stancia, l12_stancia_2 |
| town | ace_pla 24, bet_pen 14, til_cor 12, pop_tre 10, aes_hip 8, pop_nig 8, ace_neg 8, pop_alb 5, rob_pse 5, ace_pse 3, fra_exc 3 | jupiter_underground, l05_bar, l09_deadcity, l10_limansk, l11_hospital, l11_pripyat, pripyat |
| floodplain_brn | sal_alb_brn 24, sal_fra_brn 18, pop_nig_brn 14, pop_alb_brn 12, pop_tre_brn 10, que_rob_brn 10, fra_pen_brn 6, aln_glu_brn 6 | the burnt trees with foliage whose habitat is floodplain |
| birch_aspen_brn | bet_pen_brn 66, pop_tre_brn 16, bet_pub_brn 6, sor_auc_brn 2, sal_fra_brn 2 | the burnt trees with foliage whose habitat is birch_aspen |
| pine_birch_thick | que_rob 6, pop_tre 4 | the generic trees of pine_birch with a thick trunk |
| birch_aspen_dead | bet_pen_dead 66, pop_tre_dead 16, bet_pub_dead 6, sor_auc_dead 2, sal_fra_dead 2 | the dead trees of birch_aspen |
| pine_forest_dead | bet_pen_dead 9, que_rob_dead 2, pop_tre_dead 2, sor_auc_dead 1 | the dead trees of pine_forest |
| red_forest_dead | bet_pen_dead 32, pop_tre_dead 13, aln_glu_dead 10 | the dead trees of red_forest |
| alder_swamp_dead | aln_glu_dead 80, bet_pub_dead 14, sal_fra_dead 3, fra_exc_dead 3 | the dead trees of alder_swamp |
| pine_birch_dead | bet_pen_dead 18, que_rob_dead 6, pop_tre_dead 4, sor_auc_dead 2 | the dead trees of pine_birch |
| town_thick | pop_tre 10, pop_nig 8, pop_alb 5 | the generic trees of town with a thick trunk |
| pine_forest_thick | que_rob 2, pop_tre 2 | the generic trees of pine_forest with a thick trunk |
| red_forest_thick | pop_tre 13, aln_glu 10 | the generic trees of red_forest with a thick trunk |

### Burnt variants

| variant | species | bark | scorch zone (m) | kinds (their mean height, m) |
|---|---|---|---|---|
| aln_glu_brn | aln_glu | charred base | 0.82-1.42 | 19e3c0cc30795fea (9.32) |
| bet_pen_brn | bet_pen | charred base | 0.7-1.3 | e3e12950c5b30f49 (4.57) |
| bet_pen_char | bet_pen | charred all the way up | - | 6fdf973e3bd9941f, a8fe9a99fa22283c (18.1) |
| bet_pub_brn | bet_pub | charred base | 0.7-1.3 | e3e12950c5b30f49 (4.57) |
| fra_pen_brn | fra_pen | charred base | 0.82-1.42 | 19e3c0cc30795fea (9.32) |
| pop_alb_brn | pop_alb | charred base | 0.82-1.42 | 19e3c0cc30795fea (9.32) |
| pop_nig_brn | pop_nig | charred base | 0.82-1.42 | 19e3c0cc30795fea (9.32) |
| pop_tre_brn | pop_tre | charred base | 0.7-1.3 | 19e3c0cc30795fea, e3e12950c5b30f49 (7.25) |
| que_rob_brn | que_rob | charred base | 0.82-1.42 | 19e3c0cc30795fea (9.32) |
| sal_alb_brn | sal_alb | charred base | 0.82-1.42 | 19e3c0cc30795fea (9.32) |
| sal_fra_brn | sal_fra | charred base | 0.7-1.3 | 19e3c0cc30795fea, e3e12950c5b30f49 (7.25) |
| sor_auc_brn | sor_auc | charred base | 0.7-1.3 | e3e12950c5b30f49 (4.57) |

### Dead variants

| variant | species | deadwood_bark -> deadwood_bare (m) | dead kinds | their placements |
|---|---|---|---|---|
| aln_glu_dead | aln_glu | 3-8 | 10 | 27 |
| bet_pen_dead | bet_pen | 3-6 | 29 | 947 |
| bet_pub_dead | bet_pub | 3-6 | 16 | 390 |
| fra_exc_dead | fra_exc | 3-6 | 7 | 24 |
| pic_abi_dead | pic_abi | 3-8 | 14 | 517 |
| pin_syl_dead | pin_syl | 3.5-8 | 18 | 2305 |
| pop_tre_dead | pop_tre | 3-6 | 27 | 611 |
| que_rob_dead | que_rob | 4-10 | 15 | 242 |
| sal_fra_dead | sal_fra | 4-10 | 16 | 390 |
| sor_auc_dead | sor_auc | 3-6 | 24 | 608 |

### Leaf elements and barks

The generator's elements at the species' mean sizes against the ones the layers' squares were fitted to (`tree_layers.json` `element`, `card_element`; m); `card`: the spray species' sprays. The bark: its layers, the tile scale, the contrast (the upper layer's after a slash: `bark_scale_upper`, `bark_contrast_upper`). Dead wood wears its layers as they are (the dead and charred variants' own weathered barks).

| species | taken from the botany | leaf element | layers' | spray | layers' card | bark (scale, contrast) |
|---|---|---|---|---|---|---|
| pin_syl | leaf_spacing, needle_len | 0.110 x 0.104 | 0.110 x 0.104 | - | 0.320 x 0.260 | pine_plates / pine_mirror (8, 1.2 / 4, 1) |
| pic_abi | leaf_spacing | 0.077 x 0.040 | 0.077 x 0.040 | - | 0.300 x 0.200 | spruce (3, 1.6) |
| bet_pen | leaf_angle, leaf_len, leaf_phyllo, leaf_spacing, leaf_wid, leaflets, spray_len | 0.053 x 0.037 | 0.052 x 0.037 | 0.090 x 0.124 | 0.090 x 0.123 | birch_base / birch_white (4, 1 / 3, 1) |
| bet_pub | leaf_len, leaf_phyllo, leaf_spacing, leaf_wid, leaflets, spray_len | 0.045 x 0.035 | 0.045 x 0.035 | 0.090 x 0.109 | 0.090 x 0.109 | birch_downy_base / birch_white (5, 1 / 3, 1) |
| pop_tre | leaf_angle, leaf_len, leaf_spacing, leaf_wid, leaflets, petiole, spray_len | 0.050 x 0.050 | 0.050 x 0.050 | 0.048 x 0.139 | 0.048 x 0.139 | aspen_base / aspen_smooth (3, 1 / 3, 1) |
| que_rob | leaf_angle, leaf_len, leaf_phyllo, leaf_wid, leaflets, spray_len | 0.095 x 0.055 | 0.095 x 0.055 | 0.072 x 0.211 | 0.072 x 0.211 | oak / oak_young (6, 1.25 / 2, 1) |
| car_bet | leaf_angle, leaf_len | 0.075 x 0.040 | 0.075 x 0.040 | 0.150 x 0.170 | 0.150 x 0.170 | hornbeam (3, 1) |
| ace_pla | leaf_angle, leaf_kind, leaf_len, leaf_spacing, leaf_wid, leaflets, petiole, spray_len | 0.105 x 0.130 | 0.105 x 0.130 | 0.080 x 0.302 | 0.080 x 0.302 | maple (2.5, 1) |
| til_cor | leaf_angle, leaf_len, leaf_spacing, leaf_wid, spray_len | 0.055 x 0.050 | 0.055 x 0.050 | 0.150 x 0.145 | 0.150 x 0.145 | lime (4, 1) |
| fra_exc | leaf_len, leaflet_angle | 0.255 x 0.130 | 0.255 x 0.130 | - | 0.323 x 0.563 | ash / ash_young (3, 1 / 2, 1) |
| ulm_lae | leaf_angle, leaf_len, leaf_spacing, leaf_wid, petiole, spray_len | 0.085 x 0.048 | 0.085 x 0.048 | 0.132 x 0.195 | 0.132 x 0.195 | elm (3, 1) |
| ulm_min | leaf_angle, leaf_spacing, leaf_wid, leaflets, petiole, spray_len | 0.060 x 0.034 | 0.060 x 0.034 | 0.105 x 0.137 | 0.105 x 0.137 | elm (3, 1) |
| sor_auc | leaf_len, leaflet_angle, leaflet_ratio, rachis | 0.162 x 0.080 | 0.163 x 0.080 | - | 0.237 x 0.336 | rowan (2, 1) |
| aln_glu | leaf_angle, leaf_phyllo, leaf_spacing, leaf_wid, petiole, spray_len | 0.065 x 0.055 | 0.065 x 0.055 | 0.100 x 0.165 | 0.100 x 0.165 | alder / alder_young (4.5, 1.25 / 2, 1) |
| sal_alb | leaf_len, leaf_spacing, leaf_wid, leaflets, spray_len | 0.080 x 0.014 | 0.080 x 0.014 | 0.096 x 0.117 | 0.096 x 0.117 | willow_tree (5, 1) |
| sal_fra | leaf_angle, leaf_spacing, spray_len | 0.120 x 0.022 | 0.120 x 0.022 | 0.105 x 0.192 | 0.105 x 0.192 | willow_tree (5, 1) |
| pop_nig | leaf_len, leaf_spacing, leaf_wid, leaflets, spray_len | 0.070 x 0.065 | 0.070 x 0.065 | 0.060 x 0.183 | 0.060 x 0.183 | poplar_black (8, 1) |
| pop_alb | leaf_len, leaf_spacing, leaf_wid, petiole, spray_len | 0.075 x 0.068 | 0.075 x 0.068 | 0.072 x 0.194 | 0.072 x 0.194 | aspen_base / poplar_white (3, 1 / 3, 1) |
| pop_can | leaf_spacing, leaf_wid, spray_len | 0.085 x 0.080 | 0.085 x 0.080 | 0.072 x 0.223 | 0.072 x 0.223 | poplar_black (8, 1) |
| rob_pse | leaf_len, leaf_wid, leaflet_ratio, rachis | 0.205 x 0.068 | 0.205 x 0.068 | - | 0.303 x 0.420 | robinia (6, 1) |
| ace_neg | leaf_len, leaf_wid, leaflet_angle | 0.158 x 0.127 | 0.158 x 0.127 | - | 0.272 x 0.444 | box_elder (2.5, 1) |
| ace_pse | leaf_angle, leaf_kind, leaf_len, leaf_spacing, leaf_wid, leaflets, petiole, spray_len | 0.105 x 0.120 | 0.105 x 0.120 | 0.090 x 0.292 | 0.090 x 0.292 | sycamore (4, 1) |
| aes_hip | leaf_len, leaf_wid, leaflet_angle, leaflet_ratio, rachis | 0.150 x 0.173 | 0.150 x 0.172 | - | 0.330 x 0.483 | horse_chestnut (3, 1.88) |
| que_rub | leaf_angle, leaf_kind, leaf_len, leaf_phyllo, leaf_spacing, leaf_wid, leaflets, petiole, spray_len | 0.160 x 0.105 | 0.160 x 0.105 | 0.100 x 0.367 | 0.100 x 0.367 | red_oak (3, 1) |
| fra_pen | leaf_len, leaflet_angle, leaflet_ratio, petiole | 0.215 x 0.139 | 0.215 x 0.139 | - | 0.257 x 0.444 | ash (3, 1) |
| mal_dom | leaf_angle, leaf_len, leaf_phyllo, leaf_spacing, leaf_wid, spray_len | 0.068 x 0.042 | 0.068 x 0.043 | 0.040 x 0.150 | 0.040 x 0.150 | apple (2.5, 2.71) |
| pyr_com | leaf_angle, leaf_len, leaf_phyllo, leaf_spacing, spray_len | 0.055 x 0.038 | 0.055 x 0.037 | 0.040 x 0.125 | 0.040 x 0.125 | pear (2.5, 3.45) |
| t_pru_cer | leaf_angle, leaf_spacing, leaflets, spray_len | 0.055 x 0.033 | 0.055 x 0.033 | 0.090 x 0.125 | 0.090 x 0.125 | cherry_trunk (2.5, 1) |
| pru_dom | leaf_angle, leaf_len, leaf_spacing, spray_len | 0.070 x 0.038 | 0.070 x 0.037 | 0.060 x 0.149 | 0.060 x 0.149 | plum (2.5, 2.25) |
| jug_reg | leaf_len, leaf_wid, leaflet_angle, leaflet_ratio, petiole, rachis | 0.285 x 0.190 | 0.285 x 0.190 | - | 0.360 x 0.640 | walnut / walnut_young (3.5, 1 / 2, 1) |

### Kinds

| class | species or community | kinds | keys | placements | of them dead kinds |
|---|---|---|---|---|---|
| STOCK | fallen | 15 | 18 | 364 | 0 |
| STOCK | log | 3 | 4 | 65 | 0 |
| STOCK | debris | 4 | 4 | 47 | 4 |
| STOCK | stump | 2 | 4 | 39 | 0 |
| TREE | pin_syl | 29 | 30 | 2394 | 0 |
| TREE | pin_syl_dead | 18 | 20 | 2305 | 18 |
| TREE | pic_abi | 23 | 27 | 1897 | 0 |
| TREE | pop_tre | 13 | 14 | 1752 | 0 |
| TREE | que_rob | 36 | 41 | 1433 | 0 |
| TREE | community birch_aspen | 6 | 6 | 1418 | 0 |
| TREE | community town | 6 | 6 | 1282 | 0 |
| TREE | bet_pen | 19 | 19 | 1221 | 0 |
| TREE | aln_glu | 14 | 17 | 1086 | 0 |
| TREE | community town_thick | 5 | 5 | 596 | 0 |
| TREE | community pine_birch | 8 | 8 | 551 | 0 |
| TREE | community birch_aspen_dead | 9 | 11 | 366 | 9 |
| TREE | bet_pen_dead | 2 | 2 | 336 | 2 |
| TREE | pop_can | 6 | 6 | 334 | 0 |
| TREE | pic_abi_dead | 5 | 5 | 288 | 5 |
| TREE | community pine_birch_thick | 17 | 17 | 271 | 0 |
| TREE | t_pru_cer | 12 | 13 | 245 | 0 |
| TREE | community pine_forest_thick | 4 | 5 | 215 | 0 |
| TREE | pop_nig | 6 | 6 | 179 | 0 |
| TREE | pop_alb | 3 | 3 | 167 | 0 |
| TREE | community pine_forest_dead | 10 | 10 | 142 | 10 |
| TREE | bet_pub | 3 | 3 | 111 | 0 |
| TREE | community pine_birch_dead | 5 | 5 | 100 | 5 |
| TREE | community red_forest_thick | 7 | 7 | 90 | 0 |
| TREE | community red_forest | 11 | 11 | 89 | 0 |
| TREE | jug_reg | 10 | 10 | 79 | 0 |
| TREE | community pine_forest | 4 | 4 | 78 | 0 |
| TREE | sal_alb | 5 | 6 | 44 | 0 |
| TREE | bet_pen_char | 2 | 2 | 35 | 2 |
| TREE | community alder_swamp_dead | 7 | 7 | 24 | 7 |
| TREE | community floodplain_brn | 1 | 1 | 22 | 0 |
| TREE | sal_fra | 3 | 4 | 19 | 0 |
| TREE | community birch_aspen_brn | 1 | 1 | 17 | 0 |
| TREE | community red_forest_dead | 3 | 3 | 3 | 3 |
| YOUNG | pic_abi_dead | 9 | 9 | 229 | 9 |
| YOUNG | pic_abi | 5 | 5 | 116 | 0 |
| YOUNG | community alder_swamp | 5 | 5 | 105 | 0 |
| YOUNG | community town | 5 | 6 | 96 | 0 |
| YOUNG | community birch_aspen | 3 | 5 | 81 | 0 |
| YOUNG | community pine_birch | 2 | 2 | 45 | 0 |
| YOUNG | community pine_forest | 2 | 3 | 36 | 0 |
| YOUNG | community floodplain | 10 | 10 | 26 | 0 |
| YOUNG | community red_forest | 3 | 3 | 23 | 0 |
| YOUNG | que_rob | 2 | 3 | 11 | 0 |
| YOUNG | community oak_mixed | 2 | 3 | 5 | 0 |

### Stock kinds with cards

Their card textures keep their seasonal looks (the textures of every upgraded kind are drawn by the module and need none).

| kind | category | cards (stock look) | placements |
|---|---|---|---|
| 104d869b43ba777d | FALLEN | trees_vetkasux2_red (bare) | 2 |
| 6069f4284bfdcd6c | FALLEN | trees_vetkasux2 (bare) | 1 |
| 66e7075ef2ac4403 | DEBRIS | trees_vetkasux2 (bare) | 17 |
| 7528df6210689758 | DEBRIS | trees_atp_sosnodub_branch (leafy) | 19 |
| 7ed68b59c2c4b605 | FALLEN | trees_vetkasux2 (bare) | 89 |
| 9a6bd71533223b23 | FALLEN | trees_vetkasux2 (bare) | 15 |
| 9dfb417107c603b2 | DEBRIS | trees_vetkasux2 (bare) | 1 |
| b38dd196e40c5e12 | FALLEN | trees_new_vetka (leafy) | 40 |
| b747bfe4f726a273 | FALLEN | trees_vetkasux2 (bare) | 6 |
| be71dbf314002db4 | DEBRIS | trees_atp_sosnodub_branch (leafy) | 10 |
