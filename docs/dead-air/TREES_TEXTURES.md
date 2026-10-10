# Tree textures (1.6.0)

The upgraded trees wear the procedural shrubs' texture sets: their leaf, needle and bark layers are appended to the
same arrays (`textures/da/shrub_leaf_d|n`, `shrub_bark_d|n`), drawn by the same generators from each species' botany
(`tools/shrubs/textures`, no photographs in the layers), so one binding serves both populations and the shrubs' layers
keep their indices (the trees' start after them). The colours come from the season tables as for the shrubs: a leaf
layer carries luminance, veins, light through, relief, gloss and the underside, a bark layer its albedo, cavity,
relief, gloss and where lichen may settle (the channels of `SHRUBS.md`, *Textures*).

Tools: `make_leaf_textures.py` (the trees' leaves after the shrubs', their sizes and sprays in `tree_leaf_botany.py`),
`make_bark_textures.py` (`TREE_TYPES` after `TYPES`), `build_arrays.py` (the arrays, the hulls, `texture_stats.json`
and `out/tree_layers.json`), `review_trees.py` (the review sheets against photo packs, `out/review_trees/`).

    python tools/shrubs/textures/make_bark_textures.py        all bark types (about 4 min; --only a,b for some)
    python tools/shrubs/textures/make_leaf_textures.py        all leaf layers (about 5 min; --fresh-trees while the
                                                              trees' boxes may still shrink)
    python tools/shrubs/textures/build_arrays.py              the arrays, hulls, stats, tree_layers.json (a layer's
                                                              BC7 slice is cached under its PNG's hash: only changed
                                                              layers are encoded; about 3 min from an empty cache)
    python tools/shrubs/textures/review_trees.py --photos <packs> [--research tree_colours.json]   the review sheets

## Leaves

Per broadleaf species an `a` and a `b` leaf (the second its own outline, veins and blemishes; the white poplar's `b`
its short-shoot leaf), a spray `card` (a stretch of shoot with its leaves by the species' phyllotaxis: two-ranked
elms, hornbeam and lime, opposite maples, spiral birches, poplars and oaks; for the far tiers and the shadows) and,
for a compound leaf, its `leaflet` (the clusters' cells). Drawn at the species' real sizes (`tree_leaf_botany.py`),
each from its morphology (`M_TREES`, `LOBES_TREES`, `COMPOUND_TREES`, `PALMATE` in the generator):

| species | leaf | layers |
| --- | --- | --- |
| silver birch | rhombic-deltoid, long-acuminate, doubly serrate, straight craspedodromous veins, long petiole | a, b, card |
| downy birch | ovate, rounded base, coarsely serrate, downy | a, b, card |
| aspen | nearly round, sinuate-crenate, a strong basal vein pair, long flat petiole | a, b, card |
| pedunculate oak | obovate, five pairs of rounded lobes, eared base, short petiole | a, b, card |
| red oak | four pairs of deep lobes with bristle-tipped teeth | a, b, card |
| hornbeam | ovate, doubly serrate, 10-14 parallel vein pairs, pleated between them | a, b, card |
| Norway maple | palmate, five lobes with long sharp teeth, rounded sinuses, long petiole | a, b, card |
| sycamore | five ovate lobes in narrow sinuses, coarsely crenate-serrate, paler beneath | a, b, card |
| small-leaved lime | heart-shaped, oblique, short-acuminate, finely serrate, glaucous beneath | a, b, card |
| European elm, field elm | oblique base, doubly serrate, 9-17 straight vein pairs | a, b, card |
| black alder | obovate with a notched apex, irregularly doubly serrate, glossy | a, b, card |
| white willow, crack willow | lanceolate; silky-silver beneath / glossy, coarsely glandular-serrate | a, b, card |
| black poplar, hybrid poplar | rhombic-deltoid / deltoid with a truncate base, crenate, flat petiole | a, b, card |
| white poplar | palmately three-to-five-lobed (b: the ovate short-shoot leaf), white-felted beneath | a, b, card |
| apple, pear, plum | ovate crenate-serrate downy / roundish glossy, long petiole / obovate rugose | a, b, card |
| ash, green ash | pinnate, 9-11 / 7-9 lanceolate serrate leaflets | a, b, leaflet, card |
| rowan | pinnate, 13-15 sharply serrate leaflets | a, b, leaflet, card |
| black locust | pinnate, 13-15 entire oval leaflets with a mucro | a, b, leaflet, card |
| box elder | 3-5 coarsely toothed leaflets, the laterals of b lobed | a, b, leaflet, card |
| walnut | pinnate, 7-9 entire elliptic leaflets, the terminal largest | a, b, leaflet, card |
| horse chestnut | palmate, 5-7 obovate leaflets fanning from the petiole (`PALMATE`) | a, b, leaflet, card |
| sour cherry (tree) | the shrub *Prunus cerasus*' own layers (one species, one leaf) | a, b, card (shared) |

The cards of the compound species carry whole compound leaves (pairs on ash, box elder and horse chestnut, alternate
on rowan, black locust and walnut), each turned out from the shoot, with the buds (black locust: its paired thorns).

## Needles

A conifer's needle tuft is a stretch of shoot in its needles drawn from a 3D model and projected as seen some
35-40 degrees off the shoot's side, the shoot's tip toward the viewer (the shrubs' juniper sprig extended): the near
tier crosses two such quads about the shoot (`drNeedle2`), the far tiers turn one to the eye, the clusters draw it as a
brush (`SHRUB_LEAF_NEEDLES`). Each needle is a curved, tapered stroke (`render_stroke`) with its section's normal, its
own self-shade (deeper in the brush and behind the shoot darker).

- **Scots pine** (`pin_syl`, an 11 cm tuft of 4-7 cm needles): pairs in grey-brown sheaths every 1.3-2 mm round the
  shoot by the golden angle, the young needles near the tip forward (24 degrees), the older ones spread (58), each
  gently bowed and twisted so its flat inner face with the white stomatal lines turns to the viewer along stretches
  of it; a: a young shoot's end with its resinous bud cluster, b: an older stretch, the needles spread wider and a few
  gone; card: a branch tip - the leader and the whorl of laterals at its foot over last year's stretch.
- **Norway spruce** (`pic_abi`, a 7 cm twig of 1.4-2.2 cm needles): single four-sided needles on pegs all round the
  orange-brown twig, densest and longest on top, a keel along each, faint stomatal lines on every face; a: needles all
  round, b: a shade shoot's needles parted to the sides; card: a flattened branchlet spray, alternate laterals in one
  plane (the comb's hanging curtains).

The tuft's square is fitted to the element as the juniper's (`leaf_fit` from the needle length and the tuft's stretch
of shoot, `out/tree_layers.json`). The bloom shaders draw catkins, cones and fruit procedurally: they need no texture.

## Bark

36 tree types, each a seamless 512 px tile of **6 cm x its scale** of bark (`TREE_TYPES`: the scale is the species'
`bark_scale`, 2-6: an old pine's plates 4-15 cm, a black poplar's furrows 5-8 cm apart), drawn in real millimetres as
a relief with its colour (the normals, the cavity and the lichen mask follow from the relief). An old bark is built
as a relief system, not a pattern of lines, and every old bark carries a crust at the texel scale (`micro`: flakes
about three texels across, each its own brightness, and soft isotropic grit - no drawn lines):

- **fissured barks** (`fissured_bark`, parameters in `TREE_FISSURED`: the willows, black locust, the red oak, ash,
  walnut, lime, box elder, plum, the downy birch's base): plates or ridges between V-shaped
  fissures on the borders of warped Voronoi cells (rows offset: pointed plates in a diamond net; some borders healed,
  so the plates come in many sizes); the fissure's walls keep the bark's colour, lit on one side and shaded on the
  other by the light, darkening toward an AO-dark floor; the ridges domed across their own width, each its own tone,
  their surface layered (`strata`: thin flakes stepping down, each layer its own tone, fresher at its edge) and split
  along the fibres (`splits`), rough and flaky at a few millimetres (`rough_bark`), mottled with lichen and dirt
  flecks; a birch's islands carry its white bark and lenticels between corky black walls;
- **flaking barks** (`flaking_bark`, `TREE_FLAKING`: the pine's mirror bark, spruce, sycamore, apple): flakes lying
  over one another over the under-bark (a dead-leaves model: the fewer over a spot, the fresher it is), each flake's
  rim lifting on one side (a spruce's scales at their lower edge, like roof tiles) with a light cut edge and a thin
  shadow line on the flake below; weathered patches and whole grey flakes; the pine's mirror bark also in patches of
  flakes of one age a few centimetres to a decimetre across (`patches`: rustier, paler and yellower - the mottle that
  still reads from ten metres, where its flakes are under a pixel and it read as a flat salmon tube) and dark brown
  scabs of the old scaly bark left on it (`scabs`, 13 %, a few centimetres to a decimetre), the patches a stem's width;
- **scaly bark** (`scaly_bark`, `TREE_SCALY`: horse chestnut): thin plates between narrow cracks, each lifting on one
  side of its rim over a shadow line, a few fallen to the browner under-bark;
- **the aspen's and white poplar's foot** (`t_aspen_base`): long furrows grown as the pine's, broad rough grey ridges
  between them;
- **furrowed barks** (`furrowed_bark`, parameters in `TREE_FURROWED`: the oak, the black poplar, the alder): long
  furrows grown as the pine's, each running on into an older one it nears or into its own older part round the tile
  (`meet`: a net round long lens-shaped ridges, not strokes ending side by side), the ridges between them with edges
  jagged along the fibres, broken across here and there by a short crack (`cross_breaks`), rough lumpy layered tops,
  the furrows' floors near black (the black poplar's and the alder's regular nets of Voronoi plates read as wallpaper,
  twins and dried mud);
- **the pine's plates** (`t_pine_plates`): long furrows grown along the stem as drying checks (`crack_net`: wandering,
  each running on into an older one it nears, `meet`), domed plates between them broken across by a few short cracks
  across the stem (`cross_breaks`: short checks at a right angle - the zero lines of a noise drew loose arcs that read
  as hairs), their shoulders stepping
  down into the furrow in thin layers, their tops a jigsaw of thin lobed scales over one another (pinkish-buff where
  fresh, grey where weathered, each plate its own cast) with rusty edges, the furrows' walls in reddish-brown strata,
  powdery lichen in the cracks (a Voronoi net of fissures read as a diamond net or as cracks in paint);
- the dead and burnt barks (`t_deadwood_bark`: weathered plates loosening off the silver wood, a third fallen;
  `t_deadwood_bare`: the grain, long drying checks grown along it, weathering streaks; `t_char`), the cherry's glossy
  bark with bands of transverse lenticels and horizontal peeling strips, and the smooth young barks with their
  lenticels and peeling skins. Maple, elm and pear keep the first round's net of lens-shaped furrows (`lens_net`) and
  plates (`plates`).

| type | scale | look | wearers (base / upper) |
| --- | --- | --- | --- |
| pine_plates | 8 | long dark furrows 1-2 cm wide wandering along the stem, merging and parting in a connected net round long plates (grown as drying checks), pinkish-grey domed plates between them broken across by a few short cracks, each plate its own cast, a jigsaw of large thin scales with rusty edges, stepped rusty shoulders, layered reddish-brown furrow walls, powdery lichen | Scots pine (base) |
| pine_mirror | 4 | red-orange papery flakes 2-8 cm, smooth, each its own tone over a red-orange under-bark, curling up at an edge over a thin shadow, rustier and paler patches of them a decimetre across, a third of them pale apricot (peeling), a few soft brown scabs of the old scaly bark (dark ones read as chocolate chips from 20 m) | Scots pine (upper) |
| spruce | 3 | pale grey to grey-brown rounded scales 1.5-4 cm of many sizes overlapping like roof tiles, their lower edges lifting over a dark shadow, the dark reddish-brown under-bark in the gaps, greyer and browner patches a hand across, a little algae | Norway spruce |
| birch_white | 3 | chalk-white papery bark, transverse dark lenticels, peeling strips, a few black marks | both birches (upper) |
| birch_base | 4 | broad black-brown corky ridges, a shallow split along their middle (a deep one left the two edges as a thin black fishnet in the zone), round small tall lens-shaped islands of the white lenticelled bark | silver birch (base) |
| birch_downy_base | 5 | greyer and less furrowed: shallow dark cracks between whitish-grey patches, few lenticels | downy birch (base) |
| aspen_smooth | 3 | smooth green-grey with dark diamond lenticels | aspen (upper) |
| aspen_base | 3 | long near-black furrows 1.5-3 cm wide wandering up the stem, merging and parting (grown as drying checks like the pine's: a Voronoi net of lens cells read as a woven diamond net, its small cracks as ladders of dashes under a high sun), dark rough grey ridges flaky at a few millimetres between them (the photos' foot a third to three fifths of the upper bark's value), a few short cross cracks, algae in the furrows | aspen, white poplar (base) |
| poplar_white | 3 | whitish, black diamond lenticels in rows | white poplar (upper) |
| poplar_black | 8 | deep near-black furrows 1.5-3 cm wide interlacing round thick rough grey ridges 4-7 cm wide, long and leaning each way (an X-net), layered and split along the fibres, algae in the furrows | black and hybrid poplar |
| oak / oak_young | 6 / 2 | long grey ridges 2-4 cm wide, wavy, merging and parting round long near-black furrows 0.8-1.4 cm wide, their edges jagged along the fibres, a short crack across here and there, rough lumpy layered tops / smooth silver-grey 'mirror' | pedunculate oak |
| red_oak | 3 | wide flat-topped light grey ridges ('ski tracks') between shallow darker fissures | red oak |
| hornbeam | 3 | smooth grey, darker wavy streaks over the fluting, mottling | hornbeam |
| maple | 2.5 | grey, dense net of narrow ridges, fine fissures | Norway maple |
| sycamore | 4 | grey to pinkish-grey plates flaking off a buff to pinkish under-bark, shadows under their lifted edges | sycamore |
| box_elder | 2.5 | light grey-brown shallow interlacing ridges | box elder |
| lime | 4 | shallow fibrous grey-brown ridges in a long net | lime |
| ash / ash_young | 3 / 2 | grey diamond net of narrow ridges / smooth olive-grey | ash (both), green ash (net) |
| elm | 3 | corky ridges broken into flaking plates | both elms |
| rowan | 2 | smooth glossy grey, transverse lenticels | rowan |
| alder / alder_young | 4.5 / 2 | olive-grey plates 2-5 cm wide and taller than wide with rough layered tops between dark fissures / smooth olive-brown, pale lenticels | black alder |
| willow_tree | 5 | very coarse thick corky ridges round deep wide furrows, split along the fibres, algae | white and crack willow |
| robinia | 6 | deep interlacing furrows round long rope-like fibrous ridges, orange-brown furrow floors | black locust |
| horse_chestnut | 3 | lighter grey-brown thin irregular plates lifting at an edge, browner under-bark where one fell | horse chestnut |
| apple | 2.5 | grey-brown thin flakes curling over an orange-brown under-bark | apple |
| pear | 2.5 | small square blocks, deep cracks | pear |
| cherry_trunk | 2.5 | an old trunk: dull dark red-brown weathered grey-brown in patches, mottled at three scales, bands of pale transverse lenticels, horizontal peeling strips, shallow cracks across and splits along; little gloss (a flat glossy red-brown read as painted plastic, salmon-pink in the light; the species' tint cools it, `bark_tint` 0.8 0.9 1) | sour cherry (tree) |
| plum | 2.5 | dark grey-brown, fissured and flaking in small plates | plum |
| walnut / walnut_young | 3.5 / 2 | silver-grey flat ridges in a vertical net round deep dark fissures / smooth silver-grey | walnut |
| deadwood_bark | 4 | a dead pine's bleached plates loosening off the silver wood, a third fallen | the dead pines (data) |
| deadwood_bare | 3 | silver-grey weathered wood, fine grain, long checks along it, darker weathering streaks | the dead pines' bare upper trunk (data) |
| char | 4 | charred trunk after a ground fire: black alligator-checked char on the furrows, ash in the checks, glossy burnt resin, a few dull brown unburnt flecks | the burnt variants (data) |

`bark_manifest.json` lists every type with its `scale`, its `species` (the base bark, `bark_tex`) and its `upper`
wearers (`bark_tex_upper`), and maps `species` / `species_upper` (a species with no zone pair wears its bark all the
way up). The dead and burnt types belong to no species: the data's dead and burnt variants take them.

## The binding (`out/tree_layers.json`)

Written by `build_arrays.py` for `tools/trees/make_trees.py`, per tree species (ids of `tree_colours.json`, the sour
cherry `t_pru_cer`): `leaf_a`, `leaf_b`, `card`, `leaflet`, `needles` (-1 none), `bark`, `bark_upper` and their tiles'
scales (`bark_scale`, `bark_upper_scale`), `leaf_hull` (the near leaf's hull, as `leaf_hulls.json`), `leaf_fit`
(the leaf square's length and width over the element's, its start behind the element's base, the clusters' layer's
coverage) and `card_fit` exactly as `make_shrubs.py` writes them, the element sizes they were reckoned for (`element`,
`card_element`: the generator's element must have these sizes for the fits to hold), `leaf_kind`; and the trees'
layers' `leaf_cover` and `bark_mean` (the formats of `texture_stats.json`), `bark_types` (index, scale, wearers),
`leaf_layer_info` (index, kind, box, hull).

## Review against photo packs

`out/review_trees/leaves_NN.png`: per species its layers lit as the engine lights them at their real proportions,
beside photos of its leaves (needles: shoots); `bark_NN.png`: per type the lit tile, a 30 cm trunk lit near, the
same in a low grazing light from the side (the relief in relief) and at about three metres, beside photos of the
species' bark (the close-ups first), with the measures of both (`bark_measures.json`): the bark's median colour, the
fissure contrast (linear luminance p20 / p80), the dark share (under half the median) and the anisotropy (horizontal
over vertical gradients); and the scale measures, at 1 px/mm on both: the ridges' or plates' **spacing** across the
stem (twice the first minimum of the rows' autocorrelation) and the fissures' **width** (the mean dark run across
the stem), of the tile lit flat and in the ambient light against the pack's photos whose trunk is annotated
(`pack/measure.json`: the trunk's edges at a height or two, its diameter, the bark's zone; the trunk unwrapped to a
strip; photos under 0.6 px/mm left out). The packs - 16-24 photos per species
and kind from Wikimedia Commons categories (`Category:<taxon> (leaves)`, `(bark)`, ...; dead and burnt trees from
`Dead Pinus sylvestris`, `Trees damaged by fire`, `Fire scars in trees`, `Wildfires in Ukraine`) - stay outside the
repository; `out/review_trees/sources.json` lists every photo's title and url (the sheets themselves are build
products, ignored by git as `build/` is).

Calibration of the scale (the tile lit flat against the photos at scale; `bark_measures.json` has every value):
within a quarter of the packs' medians in both spacing and fissure width - red oak (-20 %, +11 %), box elder (-9 %,
-5 %), ash (-20 %, +22 %), willow (-17 %, -8 %), black locust (+4 %, -6 %), walnut (0 %, +13 %), sycamore (-24 %,
-21 %), the spruce (+3 %, -9 %), and on one or two photos at scale lime (-21 %, +2 %) and apple (-25 %, -8 %); the
furrowed barks and the pine in spacing - oak -11 %, black poplar -23 %, alder -12 % (+6 % in fissure), pine plates
-28 %, aspen's foot -7 % -, their furrows drawn half again to twice as wide as the packs' mean dark run on purpose (their floors still read at a pixel or two from a few
metres, where the trunks are mostly seen); further off: the birch bases (two photos each; the silver birch's furrows wider by design), the horse chestnut (-57 %:
plates of a few centimetres as described, the three photos at scale are old trunks with long ridges), plum (one
photo), the dead and burnt trees (one each) and the cherry's lenticel bands; the pine's mirror bark has no photo at
scale. The value contrast of the tile in the engine's ambient light (albedo x the cavity's two factors): the
fissured barks at a p20 / p80 of 0.18-0.29 and a dark share of 0.21-0.29 (the research's close-ups 0.12-0.27 and
0.18-0.33) - the fissures are no longer inked into the albedo: their depth darkens the cavity, the colour follows
the relief over the fissures' width (`tint` 0.3) and a crust at the texel scale (`micro`) gives the surface its grain;
the plated and flaking barks at 0.24-0.46 (pine 0.37, spruce 0.31, apple 0.30, horse chestnut 0.35, sycamore 0.46:
light plates as described, their cracks and lifted edges dark); the anisotropy 1.05-1.66 (black locust and lime
1.86: rope-like ridges), the photos 1.0-1.5; the smooth and young barks with their lenticels and mottling stay at
0.66-0.85: their value contrast is the species' `bark_contrast` (a power of the luminance ratio about the tile's
mean in `r3/da_shrub_wood.ps`), as for the shrubs.

The colour against the packs' bark (the masked pixels' median, `tools/trees/data/tree_photo_colours.json`; the
tile's median with its cavity in the ambient light; then the game's frames' red over blue against the packs' under a
neutral grey of the same frame, each frame balanced to its own sky): `TREE_GRADE` scales a type's finished albedo
channel by channel - the pine's plates two fifths too light, red and olive (r/b 1.36 against 1.07, g/b 1.25 against
1.02), the alder's and black poplar's dark bark half the photos' value and red (r/b 1.55 against 1.07, 1.51 against
1.18), the oak's red (1.46 against 1.13), the spruce's (1.40 against 1.19) - round four put the oak's and the black
poplar's tiles at r/b 1.15 and 1.01, the pine's plates pinkish-grey (their tiles' larger: pine plates and black poplar 8,
oak 6, the same millimetres - more of the bark between two repeats) -, the pine's mirror bark a deeper red-orange (in
the game r/g 1.9 and r/b 5.1 against an upper trunk's 3.0 and 4.0 under an overcast sky of the frame's own balance; its
upper chroma 1) -; their r/g and b/g now within 5 % of the packs', the value within 0.65-1.25 of them
(the photos' own exposure). The aspen's foot is drawn grey as its pack (median 0.11 linear, its furrows near black),
the oak's and spruce's tiles redrawn against their packs (long ridges, domed scales). Round nineteen redrew the oak on
long furrows, the spruce's scales small and matte and the pine's furrows into a net: their factors put the new tiles'
median in the ambient light back on the old ones' (`#5d5850`, `#6a635a`), which the frames had been measured with.

## Cost

The leaf arrays grew from 94 to 188 layers (62.7 MB each, BC7 with mips), the bark arrays from 19 to 55 (18.3 MB each).
