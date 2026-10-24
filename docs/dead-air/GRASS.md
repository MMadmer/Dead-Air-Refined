# Blade grass (1.6.0)

The grass of the Zone is no longer a field of textured cards. Every detail object whose content is a
grass tuft or a herb is drawn as a stand of real ChEZ plants - 64 species of grasses, sedges, rushes, horsetail
and flowering forbs in 13 plant communities - built of individual procedural leaves, stems, seed heads and flowers,
generated and culled on the GPU every frame after the Ghost of Tsushima pipeline (Wohllaib, GDC 2021):
compute places and thins the blades, raw counters size the indirect draws, the vertex shader builds each
blade from a cubic Bezier with no vertex streams. What is not a plant stand (litter, decals, unknown mod
content) stays a stock detail card, and those cards follow the season on the GPU too. The stock cards write
the lowest foliage code (`DA_STOCK_DETAIL` variant of `deffer_base_aref_flat.ps`): they light as two-sided
foliage like the blades and the snow layer leaves them to their own season (keep, height, burial).
DirectX 11 (R4, FXC SM5) and DirectX 12 (R5) run the same code.

Season data: `configs/dead_air_x64_seasons.ltx` (`SEASON_DATA.md`); the season day `D`: `SEASONS.md`;
snow: `SNOW.md`, `WINTER.md`. Sources: `src/Layers/xrRender/da_grass_blades.{h,cpp}`,
`da_grass_data.h`, `DetailModel.cpp`, `DetailManager*.cpp`; shaders `r3/da_grass_*`, `r3/deffer_detail_*`,
`r3/da_detail_common.h`; the plants: `tools/seasons/data/make_species.py` (species, communities, photo
calibration) and `tools/seasons/gen_season_data.py` (the ltx).

## Which objects become blades

A detail object is bound by its content, never by its level index or texture name (every stock object
uses `build_details`). `CDetail::Load` hashes the model as the file stores it (FNV-1a 64 over the
lower-case shader and texture names, flags, scale range rounded to 1/1000, vertex positions rounded to
1/1000 and uv to 1/16384, the indices, before `Optimize()` permutes them: K1); `CDetailManager::Load`
folds in the mip-0 DXT blocks of the level's base atlas `$level$\build_details.dds` that the model's uv
rectangle touches (KSPR). `[detail_types]` maps the 478 keys of the shipped levels to a class:

| Class | Drawn as | Community |
| --- | --- | --- |
| `PG_BUNCH`, `PG_FINE` (and the empty Stancia sprites) | blades | old field (*Calamagrostis epigejos* fallow) |
| `PG_LEAFY`, `PG_PLUME` | blades | mesic meadow |
| `PG_TUFT` | blades | wet meadow (tussocks) |
| `PG_RUSH` | blades | sedge marsh |
| `PG_DRY` | blades | ruderal verge |
| `KS_FORB` | blades | forb meadow |
| `KS_HERB` | blades | tall herbs (nettle, mugwort, cow parsley) |
| `KS_FERN` | blades | forest floor (no fern: the pine wood's sparse grasses, lily of the valley) |
| `KS_HORSETAIL` | blades | wet margin |
| `KS_SHRUB` (bare twig bushes, branchy dead stalks) | blades | forb fallow (old field with mugwort, tansy, wormwood) |
| `KS_LITTER`, `KS_DECAL` | stock card | season keys of the class |
| any key not listed (a mod's model, a re-encoded atlas) | stock card | generic grass season (`stock_keys_grass_legacy`) |

`[grass_class_community]` maps a blade class to a community by name; the reed bed, sand and lawn
communities are complete ones a mod can map its own classes to. The stock decompress
(`DetailManager_Decompress.cpp`) leaves the blade classes out while the blades run, so the two never double
up. The key is verified byte for byte against the reference generator (`tools/seasons/gen_season_data.py`,
478 keys over 494 objects).

### Reed beds

The levels' reeds and sedge are not details but stock multiple-use models (`trees_kamish_big_01`, `_big_02`,
`trees_kamish_sux`, `trees_kamysh`, `trees_osoka`, `trees_osoka_sux`: GSC's crossed cards). Where one stands the
blades grow a reed bed instead (`src/Layers/xrRender/da_reeds.{h,cpp}`, `dead_air_x64_reeds.ltx`):

- `[reed_cards]` names a stock card (the level shader's base texture) and the community a model of it stands for:
  the `kamish_big` cards draw cattail stands (strap leaves, brown sausages) - `cattail_bed` (*Typha latifolia* 71 %,
  a tenth of reed, *Carex acuta*, *Glyceria maxima*, *Phalaris arundinacea*); the `kamysh` card a reed - `reed_bed`
  (*Phragmites australis* 73 %, the same company - the Pripyat floodplain's beds); the sedge cards `sedge_marsh`. (Grown
  as a reed bed, the cattail stands read as anything but cattail: the user's "rogoz".) A mod's reed model is added
  here by its card.
- At level load (after the vegetation core's census, before the bake and the GPU-driven registry) every such model
  is a bed: its footprint (`footprint` x its mean half width), the bed under it (the static ground past the water),
  the water over that bed. The models leave their sector hierarchies (every view, shadow, the GPU registry and the ray
  tracing scene forget them; the ray tracing scene keeps them as stand-ins), the blades are set up again with the beds.
- The blades' copy of the slots carries them: each community takes a free object id at the top of the table, a slot
  under a bed gets that id with the bed's cover at its four palette corners (full within three quarters of the radius,
  none three quarters of a metre past it), the meadow's own objects giving way by as much, its box down to the bed (the
  ground window finds the bed under the passable water), an open sky term where the level left the slot bare. Its
  colour bits, unused by the blades, carry the water: red is how far the culms rise for it (0.1 m units, the deepest
  bed's, x `emerge`), green a mask of the slot's bed objects, blue how far the box went down (0.2 m units: the
  meadow's objects of the slot root no lower than its stock box - rooted on the bed they stood up out of the water).
- **The water over each root** (the level's baked water field, `da_water_field.h`, in the generation): a lane deeper
  than its species stands in (`water_depth`, profile row 27 y: reed 1.5 m, cattail 1.2, reed sweet-grass 0.8, tufted
  sedge 0.4, reed canary grass and purple loosestrife 0.3, a puddle's 0.05 for the rest, nettle and wood small-reed
  none; a hand either way at the margin) is not grown, and a blade rises by the water over its own root x `emerge`
  (at most the deepest bed's): a reed in a metre of water stands its own height over the surface, a reed on the
  shore its own height over the shore (it rose by the slot's deepest water). Without the field the slot's red bits
  hold.
- Past the blades' radius the stock model's billboard stands for the bed, fading in over the radius' last 15 % as the
  blades fade out (`da_reeds_frame`: the replaced FLODs into the main view's LOD list after its graph is built; the
  batch's alpha comes from the encoded size measure); its cells are drawn for the day by the bake like every stock-path
  FLOD (`TREES.md`).
- The year is the species' own: green culms and leaves from May, the plumes in August and September, straw in
  November, and through the winter last year's culms stand pale over the snow and the ice with their plumes
  (`winter_stand = 1` on *Phragmites* and *Typha*: the snow's grass visibility, which leaves the meadows bare from
  December to February, does not thin them; the snow over a blade still buries it).
- A dead reed bed is not one straw colour: `dead_vary` (1 *Phragmites*, 0.8 *Typha*, 0.6 the marsh sedges and grasses
  *Carex acuta*, *Glyceria maxima*, *Phalaris arundinacea*) spreads its dead culms, leaves and ripe plumes blade by
  blade and a little clump by clump from tawny to grey-buff and from pale to dark round the season's colour
  (`grass_dead_vary`). Measured on 40 Commons photos of dry reed beds, autumn to winter (pixels of hue 18-58 deg):
  hue 24-41 deg p10-p90, saturation 0.18-0.55, r/g x1.6 and b/g x2 across a bed, neighbouring culms 30 % apart in
  value.
- **Cattail** (`head_velvet = 1`, profile row 19 w): its head a smooth velvet sausage over the lower 58 % of the head,
  blunt at its ends, the male spike right above it a third as thick (yellow with pollen while it flowers, a thin
  grey-brown spike once withered), its vertices placed at those joins (spread evenly the sausage read as a spindle);
  no lobes, fluff or air between grains (as the cylinder spike type it read as a granular dock). Leaves 1.3-2.2 m,
  1.4-2.4 cm wide, stiff with arching tips; a fifth of the shoots fertile stems as tall; heads from June, brown by July.
- **Stem leaves** (a reed's) are rooted on their stem's curve at their node (`grass_ctrl`, shared with the vertex
  shaders), and a stem that carries leaves has no bob of its own (on the straight chord, a bent reed's leaves stood a
  hand off it and hung in the air); their occlusion counts from the upper canopy, not from their node as the ground
  (their bases sat in the dark of the stand's foot: near black).
- **The dead season** checked against a Commons pack of 714 dated photos of *Typha* and *Phragmites* by month (pixels
  of the plants, hue and saturation medians): green June to September (hue 75-88 deg), mixed in May and October,
  straw November to April at hue 28-37 deg and saturation 0.30-0.47. The marsh plants' dead tissue keeps its chroma
  (no fifth off the straw, no quarter off a dead culm - a reed's culm is glossy straw, the palest of the bed), and the
  hoarfrost is a third of a white over the upper blade, not 0.7 of a blue-grey (the bed's straw went grey and lavender
  from October to February: saturation 0.15-0.2 and 28 % of the pixels lavender in November).
- `r__reeds 0` (QA) puts the stock models back. Log: `* [reeds] N stock reed models stand as reed beds of the blades
  (M in water)`, `* [grass] N reed beds grow in M slot touches (deepest emergence X m)`.

## Species and communities

**Species** (`[grass_species_N]`, up to 64 - the six species bits of a record; `make_species.py`): 64 plants of
the ChEZ meadows, fallows, verges, wetlands and pine woods after Didukh 2023 (the constancy table of the zone's
grasslands: every forb that reaches a constancy of about 25 % in one of its open-ground clusters), the floras
and FloraVeg traits, and the Chornobyl reserve's nature calendars 2018-2022:

- graminoids: *Calamagrostis epigejos*, *C. arundinacea*, *Elytrigia repens*, *Dactylis glomerata*, *Poa
  pratensis*, *P. angustifolia*, *Festuca rubra*, *F. ovina* agg., *Agrostis capillaris*, *A. gigantea*,
  *Phleum pratense*, *Alopecurus pratensis*, *Bromopsis inermis*, *Deschampsia cespitosa*, *Corynephorus
  canescens*, *Anisantha tectorum*, *Avenella flexuosa*, *Phragmites australis*, *Phalaris arundinacea*,
  *Glyceria maxima*, *Typha latifolia*, *Carex acuta*, *C. hirta*, *Juncus effusus*;
- forbs: *Solidago canadensis*, *S. virgaurea*, *Artemisia vulgaris*, *A. absinthium*, *A. campestris*, *Tanacetum
  vulgare*, *Achillea millefolium*, *Urtica dioica*, *Anthriscus sylvestris*, *Daucus carota*, *Erigeron
  canadensis*, *Chamerion angustifolium*, *Lythrum salicaria*, *Lysimachia vulgaris*, *Hypericum perforatum*,
  *Galium verum*, *Trifolium pratense*, *Vicia cracca*, *Lathyrus pratensis*, *Ranunculus acris*, *Silene
  latifolia*, *S. flos-cuculi*, *Centaurea jacea*, *Rumex acetosella*, *R. acetosa*, *Oenothera biennis*,
  *Cirsium arvense*, *Jasione montana*, *Helichrysum arenarium*, *Berteroa incana*, *Verbascum lychnitis*,
  *Prunella vulgaris*, *Campanula patula*, *Chelidonium majus*, *Equisetum arvense*; rosettes: *Plantago
  lanceolata*, *P. major*, *Trifolium repens*, *Taraxacum officinale*, *Convallaria majalis*.

Every species flowers, seeds and dies back by its own phenology, so each month has its own flowers: dandelion,
ragged robin and the sorrels in May; buttercup, the clovers, campions and bellflower in June; vetch, vetchling,
knapweed, thistle, St John's wort, fireweed, loosestrife, yarrow, wild carrot and evening primrose in July; tansy,
both goldenrods and mugwort in August.

A species is a form (0 graminoid: leaves, culms, basal pairs; 1 forb: a leafy stem with up to 12 cauline
leaves - two-ranked, spiral or opposite - and a terminal inflorescence; 2 rosette: basal leaves and leafless
scapes; 3 rush: leafless green stems), a leaf outline (grass, lanceolate, ovate, bristle, strap, pinnate,
dissected), its geometry (heights a game convention of about two thirds of the measured stands), its head
(type, size, branches, opening, grain scale, flower petals), its stem tint and its season: a graminoid's keys
are its template's (P1-P8, `SEASON_DATA.md`) shifted by its own green-up and anthesis; a forb has its own
phenology (emergence, full height, buds, flowers, ripe, weathered heads, senescence, last year's stems).

**Colours from photos**: dated iNaturalist research-grade photos of Ukraine and Belarus (2595 photos, 11 per
species and phenophase, centre-weighted HSV masks per part: live, glaucous and dead leaf, heads, flowers,
stems; `species_photo_colours.json`). `make_species.py` gives every species the meadow template's live
colour of the month times its photo chromaticity against the cross-species median and the square root of its
relative luminance (glaucous and silvery species turned towards their glaucous or silver part; nettle's hairs
read glaucous in the photos and are excluded), its dead colours from its template, its heads and flowers by
stage from the photo parts (tan, purple, dark, pale, yellow, white, pink, red, blue), the stems of fireweed and
purple loosestrife from their photo stems. 4179 photos for the 64 species. A flower's photo colour is a median
over the flower in the frame, its shaded petals included, so it comes out darker than the petal: a flower keeps
its photo hue and rises to the luminance a petal of its colour reflects at least (linear: white 0.55, yellow
0.42, pink 0.22, blue and violet 0.14, dark red 0.09), and the head pixel shader varies it round that colour
instead of darkening it like a grass head's glumes.

**Communities** (`[grass_community_N]`, up to 15): up to eight graminoids by cover share, in clonal patches
of `patch` metres (sine-warped cells) with a `scattered` share mixed in; up to sixteen forbs (the
constancy-matrix forbs of the community's clusters, a forb's cover share about its constancy x 0.08 %), each with
the share of the lanes it takes scattered everywhere, the share of the area its clonal patches cover and its share
of the lanes inside them (one shared patch field per community; the generation finds a lane's forb in the
cumulative shares with four vector compares); `presence` (the summed palette alpha from which the stand is
complete), `cover` (a pine floor of needles and moss is not all grass), the litter.

**Presence**: near the camera a stand grows at full density wherever its slot's summed palette alpha reaches
the community's `presence` (the stock rule thinned the blades by the palette like its cards: a meadow painted
at alpha 0.4 grew 40 % of its grass), and at any distance (1.6.2). 1.6.1 returned to the palette's weight from 0.75
`d1` to `d2`: a meadow went half bare at 30-40 m and stayed so to the reach, which its denser tiers hid (at the
Default tier on the l01_escape meadow the full stand draws 1.8 times the far field's records from 35 m up, 1.4
times at the eye; `r__dbg_grass 512` brings the old rule back).

## Pipeline

Level load (`da_grass_level_load`, after the snow): the slots of `level.details` as a raw buffer, the
object id -> community table, the collision triangles that can be grass ground (up-facing, not passable)
bucketed into 4 m cells over the slots that grow blade grass, the push field, the record buffers, the
index buffer, and the grass cover map of the ground tint (`$user$grass_cover`). The collision mesh comes
from `da_ground_cdb()` (one upload for snow and grass); without it an upload of its own in the same format.

Per frame:

1. **Latch** (main thread, `CDetailManager::UpdateRenderState`): every species' profile (28 float4) and
   season keys at `D` and at the previous frame's `D` (`[grass_keys_N]`, one periodic monotone cubic per
   key, colours in linear rgb), the communities (9 float4), the camera, the preset columns, the settings, the
   bins' windows, the record budget's pixel floor.
2. **Tiles** (the detail calc task): a quadtree of world-aligned tiles over the draw radius (and the sun
   cascades' reach). The candidate lattice is quincunx: its density halves per doubling of the distance
   beyond `R0` (`R0` holds a 6 mm leaf above the pixel floor), each tile is enumerated at its ring's level
   plus the levels its own grass density spares (a sparse or out-of-season tile runs fewer lanes), tiles
   outside the view that can cast into it run at their shadow level, tiles without blade grass run none.
   64 lanes per compute group, the nearest tiles first (over the group budget, and in a bin that overflows,
   the far ones lose). The groups are laid out on the main thread once the main view's occlusion buffer (HOM)
   is drawn, after DOOM Eternal's culling of its GPU-driven geometry by its occlusion buffer: a tile in view
   that the HOM hides grows only the shadow it casts into the view, at the coarser of its own and the shadow's
   level, or nothing (`r__grass_occlusion` bit 1). Among the rookie village's fences and houses 780 of 1915
   tiles hide: 22014 -> 15270 groups, the blades' compute 2.4 -> 1.5 ms and their G-buffer 0.93 -> 0.8 ms on the
   RTX 3070 Laptop at Extreme; in the open 15 tiles hide; the HOM tests cost the main thread about 0.1 ms.
   `r__grass_blades_stats` reports the hidden tiles. What a tile's slots hold is the same in every frame: per community
   the most presence x cover of its slots and the slots' height range are worked out the first time the tile is walked
   and kept by its level and place (1.6.3, `TileSummary`; a new lattice spacing - the height row - starts over), the
   frame multiplying the communities' density of the day in. Worked out over every slot of every tile each frame (a
   300 m reach is some 70 000 slots) the walk was the calc task's heaviest work: a fifth of a core on Cordon at the
   Maximum distance, a quarter of that now, the tiles, levels and lanes the same.
3. **Ground window** (compute): a toroidal 1024², 2048² or 4096² R32F window of 0.25 m texels around the camera
   (the reach of the grass distance row; at 0.5 m texels a 27-degree slope read as a kerb and lost its grass),
   exactly the stock decompress rule - the highest up-facing non-passable surface under the slot's top,
   nothing below its base. Only the strips the window moved onto (8 m steps) are rebaked.
4. **Push field** (compute): every creature, corpse and settled object stamped this frame
   (`da_snow_stamps()`, the same deformer list as the snow) pushes the grass in a 64 m toroidal field
   (512², 12.5 cm) outwards and along its motion; the field springs back over a few seconds. A stamp pulls a texel
   toward its push over a tenth of a second (0.06 s), never overwrites it: the frame a standing foot's outward press
   outgrew the fading trail under it, the texel jumped from the trail's heading to the foot's and its blades spun.
5. **Occlusion** (1.6.2, compute, in the G-buffer stage): the HOM is the level's coarse occluder mesh - the
   terrain and the big buildings; walls, fences, barrels and the like are not in it. So the blades grow there, once
   the opaque world, the LODs, the trees, the shrubs and the first-person view are in the depth and before the
   blades draw (`r2_R_render.cpp` render_details), against that depth: it is copied (`CRenderTarget::copy_depth`),
   and the stage's depth pyramid (`da_hzb.cpp`, `r3/da_hzb.cs`, the test in `r3/da_hzb.h`; the trees' cut builds it
   too, TREES.md "Occlusion") holds its farthest depths (level 0 at half the render size, 2 x 2 per level to one
   texel, the levels side by side in one R32F texture). A group's tile whose box lies behind it keeps only its shadow,
   at the coarser lattice level the shadow's keep needs (the group's first lane tests the box for all of them; the
   record carries the tile's heights, its shadow-only level and flags - `expand_tiles` grows a HOM-hidden tile so),
   the lanes off that level dying before a slot is read; then a lane whose bounding sphere's nearest depth lies past
   the farthest depth over its rectangle on the screen (the sphere's exact perspective bounds, a pixel more, at the
   level where they span two texels) grows only its shadow casters. It is this frame's depth, so nothing lags a
   frame behind a moving camera or a door. `r__grass_occlusion` (a session knob, 7): bit 1 the HOM's tiles, bit 2
   the depth's lanes, 4 its groups too. Measured A/B at the eye, four headings a quarter turn apart, Extreme, DX11
   1080p on the rig (records of the view; GPU `grass` + `grass_gen`, summed over the headings):

   | Place | Records, HOM only -> + depth | GPU ms, HOM only -> + depth |
   | --- | --- | --- |
   | Cordon, inside a house | 830 k -> 176 k (-79 %) | 6.45 -> 3.99 |
   | Swamp, a tower over the reeds | 853 k -> 726 k (-15 %) | 7.39 -> 7.17 |
   | Cordon, the open meadow | 1285 k -> 1164 k (-9 %) | 9.91 -> 9.65 |

   Facing a house wall 219 k -> 2 k; in the open the pyramid costs some 0.05-0.1 ms the culled lanes do not pay
   back. DirectX 12 and Vulkan cull the same (in the house, two headings: 399 k -> 110 k records, 3.36 -> 2.36 and
   2.93 -> 1.98 ms). The groups' test (bit 4, 3 -> 7) hides up to 93 % of a house's groups; `grass_gen` 3.52 -> 3.10 ms
   there, the meadow's 6.18 -> 6.11 (the timer holds the ground window, the push field and the tufts as well).
   `tools/qa/water/grass_hiz.lua` shoots the modes: the shots match but for the wind's sway.
6. **Generation** (`da_grass_gen.cs`): per lane the community of the slot's objects (picked by their
   bilinear palette weights; presence and cover above), the species (forb patches and scattered forbs, then
   the graminoids' clonal patches or the scattered share), an early out on the species' widest lane against
   the pixel rule before its tables are read, the clump (a Voronoi cell of 3x3 jittered points:
   pull to the centre, shared and away-from-centre facing, height and colour variation, bare gaps of sparse
   sand, tussock bases), the ground, the season state of the blade (cohort: this year's or last year's
   thatch, senescence, growth height, lodging, seed heads and their stage), the snow gate, the keep, the
   wind and push, then a 64-byte record into the window of its bin. A grass leaf lane also writes the folded
   basal pairs of its tuft and the stand's litter (flat dead pairs, inside `d1`, fewer under the summer
   canopy) right after its own record (near the camera, see "The look"), a stem its cauline leaves up to `d2`
   (past `d1` a forb's dozen leaves are four larger ones of the same area: a leafy column still, at a third of the
   records; folding them into the stem's far triangle there read as a field of green paddles). Shadow casters of
   the near and middle sun cascades get records of their own (their own keep per shadow texel: two texels of
   the near cascade, three of the middle one; the basal pairs cast none, a forb's leaves only up to halfway to
   `d1`, its stem's triangle carries them beyond).
7. **Arguments** (`da_grass_args.cs`): 32 blades per instance, the record count clamped to the bin's window.
   A lane needing several records (a culm and its cauline leaves) that runs past its bin's window fills the
   slots it reserved below the cap with void records (zero shape), and a head whose culm found no room with no
   culm; the vertex shaders collapse both, so the drawn prefix never holds a stale record.
8. **Draws**: the view bins near to far (early-Z on deep fields), no alpha, no discard, except two: the
   short bin's pixel shader (`da_grass_blade_cut.ps`) cuts the lobes of pinnate and dissected leaves, and the
   seed heads (a few thousand small ribbons) cut their fluffy rim and their grains in their own pixel shader.

Bins: 13-vertex blades in 2 distance bins, 7-vertex in 3, 3-vertex in 4, the near and middle sun cascades,
43-vertex near seed heads, 13-vertex middle heads in 2 bins, the near cascade's heads, and the 7-vertex
short bin: the short leaves of the lanes inside `d1` (basal pairs, litter, a stem's leaves, the pinnate and
dissected leaves of forbs), drawn between the near and the middle bins (thirteen vertices drew a 15 cm basal
leaf as finely as a 60 cm one). Their windows split the preset's record budget; a readback resizes them to
what they needed (at once on an overflow, every 240 frames otherwise). It is a ring of five staging copies:
every frame every finished copy is read, oldest first, and the counts go to the next free one, so it keeps
up however many frames the GPU runs behind.

**Over the budget** (a dense fireweed or goldenrod stand fills the view): the same readback drives the
record budget's pixel floor, a factor of `k_px` and of the casters' texel widths that follows the blades'
share of their buffer towards 85 % (up at once, at most x1.25 a readback, down 1.5 % a readback, 1..3): the
whole view grows fewer and wider blades, the near detail last, instead of a bin dropping its last records in a
ring; the nearest tiles go first, so a head or a record that finds no room is a far one. In the near sun
cascade a forb's head that finds no room is carried by its stem's triangle (the culm's head flag tells the
vertex shader). The natural meadows of the shipped levels stay at x1 on every preset.

### Keep: the pixel rule with coverage preserved

A blade narrower than `k_px` render pixels survives with probability `width / (k_px x pixel at its
depth)` and is widened to that floor, so the field's expected coverage equals the real one at every
distance; density (season, settings, presence) thins without widening. Survivors are chosen by a nested
rank of the lattice point, with an 8 % band relative to the keep in which the blade narrows to zero: no
blade ever pops, and the coarser points live longest. Where the lattice level supplies fewer lanes than
the keep asks (wide reed culms near a ring edge) the survivors widen further. Without TAA or an upscaler (SMAA, FXAA,
no anti-aliasing) the floor is 1.75 times wider (1.6.2): nothing resolves a blade of one or two pixels across frames,
and the far field shimmered with every sway. Folded short grass (a species'
leaf height x `r__detail_height` below its fold height) draws two leaves per strip from one shared base pair.

### The blade

A cubic Bezier: the root, the tip from tilt and facing, the midpoint lifted to the inner, upper side of the
chord (a blade leaves its root steeper than the chord and arches outward; the outer side drew every blade
flat at the root and turning up at the end, a J), arching leaves
drooping at the tip, a sine bob phased by hash and wind strength. The vertex shader evaluates it for this
and the previous frame (shape, wind, motors, push and the season's growth all included) and applies the
same screen-facing offset to both, so motion vectors are exact for TAA, FSR and DLSS. Leaves are drawn
at their projected width (edge-on leaves keep a quarter), culms as screen-facing cylinders emitted top-down
(the far LOD is an inverted triangle whose top carries the folded seed head's area and colour). Morphs in
the last fifth before `d1` and `d2` land exactly on the next LOD's polyline. A geniculate culm (`culm_knee`)
leaves its root upright for that share of its height. The heads are a second draw of procedural ribbons on
the headed culms: a core continuing the culm and up to six branches by head type (spike, cylinder,
one-sided, pyramid, cloud, dense lobed panicle, plume, nodding awned, sedge spikes), opening with the seed
stage, fluffing when ripe, thinning when shattered; their vertex shader evaluates the culm's top for both
frames, so head and culm never part. The head width of the data is the spread of the whole head. The spike
types (1, 2) are the core alone, the sedge spikes (9) a core with short side spikes; every panicle is a
thin rachis (12 % of the spread, at least 2.5 mm) whose branches carry the spread: the dense *Calamagrostis*
panicle (6) is a lobed spindle of six short branches pressed to the axis (11 degrees at anthesis, 5 ripe,
0.42 of the spread wide each), the one-sided cluster (3) three wide clusters, the open types (4, 5, 7, 8)
thin drooping branches. Beyond `d1` the dense and one-sided panicles become a spindle ribbon as wide as
their branches spread (the near core widens into it while the branches fade, so the switch does not pop).
The forbs' inflorescences: a flat corymb (10: yarrow, tansy, St John's wort) and a compound umbel (13: cow
parsley) are a level disc of florets on thin rays (the disc's diameter runs across the view, its chord level,
its florets' depth upright and never under the pixel floor, so a disc seen edge-on stays a line; an umbel's
a little domed, its florets gathered in umbellets a fifth of its width across); the disc opens from a knot
of buds to its full width by anthesis. A golden panicle (11, goldenrod) has arching one-sided branches, a
panicle of tiny heads (12: mugwort, wormwood, fleabane, lady's bedstraw, nettle) a spindle of short branches,
a raceme of large flowers (14: fireweed, purple loosestrife, lily of the valley, vetches, evening primrose) is a
cone whose upper third is buds - smaller, closed and darker; a globe (15: clovers, dandelion and its clock,
knapweed, sheep's-bit) a round head of florets. Flowers with petals (`head_petals`: fireweed and evening
primrose 4, St John's wort, buttercup, campions and bellflower 5, loosestrife 6) are round grains cut into their
petals, each turned its own way, with a darker eye; they stand apart with air between them. A forb's flowers
keep their rim nearly whole and are never cut by the middle distance's see-through dither (that is for the airy
grass panicles).
`da_grass_head.ps` builds every ribbon out of spikelets: ellipses of the type's grain (*Calamagrostis*
5 x 2.2 mm, the spikes 3 x 2 mm, the awned type 12 x 4 mm) on a jittered grid in metres along and across
the ribbon (the vertex shader passes both and a seed per ribbon), present by a cover that is dense on the
axis, thins towards a fluffy rim and drops between the clusters (lobes) along the head; each grain has its
own tone, a lighter tip and a shaded margin, and the air between the grains is cut. Once a grain spans less
than about two pixels the grains fade into their mean cover, cut stochastically (interleaved gradient
noise; a new pattern every frame while the motion vectors run, so TAA resolves it into a soft, see-through
head; a fixed pattern otherwise); a ribbon under two pixels wide is a stroke of colour and stays solid. Reed
culms carry four cauline leaves.

Forb stems carry up to 12 cauline leaves (`culm_leaves`) from their first to their last node (shares of the
stem), two-ranked, in a 137.5 degree spiral or in opposite pairs turning a quarter per node, the upper ones
smaller (`cauline_taper`), curling as they die; a rosette's leaves are its lane's own leaves, its scapes
leafless. Leaf outlines: lanceolate, ovate (broadest below the middle), bristle, strap; a pinnate leaf
(tansy, mugwort, cow parsley) is an oblong blade from a short stalk with eight pairs of lobes cut out of it,
alternate and leaning to the tip round a whole rachis, a dissected one (yarrow, wormwoods) a narrow plume of
fifteen thinner pairs. The cut runs only in the short bin (inside `d1`; the leaf's lane takes the short bin
there); where a lobe spans under two pixels the leaf narrows to its mean cover (60 %) instead of dithering,
and beyond `d1` and in the shadows it is drawn that narrow. Past `d1` a forb's stem carries four leaves of
its dozen's area; in the near sun cascade it turns into the far triangle with its leaves folded in from 0.4
to 0.5 `d1` (its leaves fade out over the same band, so nothing pops).

### The look

What makes the meadow read lush and natural (Ghost of Tsushima's shipped look), all data-driven per
species (`SEASON_DATA.md`):

- **Arching leaves, straight culms**: leaves lean 10-45 degrees and bend 0.12-0.45 of their height on the
  inner side, so they rise and arch outward with drooping tips; culms lean 0-12 degrees and bend at most
  0.02-0.10, nearly straight, nodding only under a ripe head. Heights are a game convention about two
  thirds of the measured ChEZ stands (*Calamagrostis* leaves 0.30-0.60 m, culms 0.60-0.95 m).
- **A dense base**: a leaf lane carries the folded basal pairs of its tuft (`density_basal` leaves per m2,
  up to three pairs per leaf, `floor(pairs per leaf + hash)`), short (`basal_h`, 0.1-0.45 m), arching
  (`basal_tilt` 15-52 degrees, `basal_bend`, `basal_droop`), fanned wider than a folded leaf (1.7x the fold
  yaw), beside the leaf's root and pulled into the clump, facing away from its centre. They are the
  youngest leaves: their own cohort by hash, curing last, so autumn tussocks keep a green base. They ride
  on the leaf lanes (the lattice is not denser), keep the pixel rule, and fade out from `d1` to halfway to
  `d2` (Extreme 12 -> 28.5 m), so they cost nothing in the far field. As they fade, the leaves take their
  share of the stand's cover over in the view (`da_grass_gen.cs`: the lane's keep times `1 + share x fade`,
  `share` = the basal pairs' leaf area over the leaf's, at most 2): a gap between the taller blades looks
  into the stand's lower leaves, not onto the ground. Only as far as the ground faces the camera (full from
  14 degrees between the view and the ground on): at a grazing view the taller layer covers the base anyway
  and nothing is added; the shadows keep the stand's own leaves (the basal pairs cast none). Summer meadows
  read lush and full; autumn ones as dense dry tussocks.
- **Clumps that lean together**: every clump has a lean direction and amount up to `clump_lean` (P1/P2/P7
  9-10 degrees), its culms take all of it, its leaves half.
- **Lodged mats lie combed**: wind, rain and the snow's load lay a stand down in swathes a few metres across,
  each combed one way. A lodged blade (and a lodged basal leaf) turns 0.8 of the way to its swathe's direction
  (`grass_comb`: the angle turns smoothly with a value noise of 4 m cells, partly down the slope) and lies
  flat along the ground (bend toward 0.04, no droop) instead of arching: lodged at their own random facings
  and bends, the March and April straw read as a scatter of loops and crossed sticks over bare soil.
- **Culms**: 2-3 mm, leaning 5-20 degrees in a gentle arc (`culm_bend` 0.08-0.2 on P1/P2/P3/P7: the
  0.2-0.45 of round 1 bent mid-field culms into hooks, and their low LOD polyline into V-shaped kinks; a
  *Calamagrostis* culm is stiff), and a nod under the head as it ripens (stage 0.3 -> 0.8, by the head's
  length: the top leans up to ~7-10 degrees further and arches a little more). They sway with the wind like the leaves (the GoT bob on top of
  the stock tuft's wind and motors). A dry culm is buff-grey: three quarters of the straw's chroma at 0.88 of
  its value, so a stand of dead culms reads as stems and not as yellow sticks. A culm much thinner than the
  pixel floor stands for a stem whose pixel mostly holds the stand round it: its colour and its sky ease up
  to halfway to the leaves' round it (their colour and occlusion halfway up the canopy) as the stem's share of
  its drawn width falls (`0.5 smoothstep(0.5, 1, 1 - width / floor)`); drawn in their own open, pale tone the
  culms of the middle distance read as thin pale strokes over the canopy (October hill at 10-40 m).
- **Seed heads** that never read as cards: thin rachis and branches built of spikelets with air between them
  (above), translucent only where back-lit (0.55 flowering, 0.7 ripe, 0.45 weathered), and darker than a
  single glume of the ramp (x0.8: a head is a cluster of grains with shade between them; the far culm's
  top, which carries the folded head, takes the same colour and is never lighter than the culm or its head).
- **Fresh greens**: the live colours of May-July carry more chroma (P1 #4F5E32 / #4A5A2F, P2 #4E6230 /
  #45572A) at the same brightness; the canopy colours of the ground tint follow (`ground_tint.json`).

### Shading (G-buffer)

- Colour along the blade from the season keys (live, yellowing, straw, thatch; tips cure first; a dark
  base, x0.62 at the root to x1.04 at the tip), dead tissue at 0.82 of its measured chroma (the warm light
  and the straw's yellow transmission make the photo straw read yellow on screen), a value step per clump
  (+-12 %) and per blade (+-14 %),
  a few live blades a step towards the yellowing colour, hoarfrost, dew and damp (porous straw darkens
  more), silt after floods, falling snow on the upper blade. Heads take the species' seed colour ramp by
  stage (`seed_ramp`: *Calamagrostis* grey-green heading, purplish-brown at anthesis, bronze-tawny ripe,
  grey-tan weathered).
- Translucent two-sided foliage in the free material codes 27-30 (`da_material_codes.h`): four
  translucency steps, dithered so their mean is exact, resolved by TAA. `lmodel.h` adds the sun's
  transmission through the blade (UE TwoSidedFoliage: wrapped back light, a fixed GGX lobe around the
  light direction, a warm tint), `hmodel.h` some sky through it; both sample the Oren-Nayar slice of the
  material LUT for codes 27-31 (`da_lut_z`), and code 31 (snow) gets `da_snow_light`. A leaf's cuticle reflects
  by Fresnel (`da_foliage_fresnel`, Schlick-like: 0.15 of the stock lobe face-on, all of it along the leaf
  into the light): with the sun behind the viewer every blade the camera sees sat at the stock lobe's peak and
  the front-lit field washed out white, while the sheen into the sun stays. The blades write
  their translucency only where the camera looks towards the sun (the lowest step elsewhere, so the sky
  term does not brighten a front-lit field). The sun passes move foliage receivers 3 cm toward the sun
  instead of along the normal, so a back-lit blade does not shadow its own transmission.
- Normals rounded across the width (30 degrees near, 10 at `d2`; culms and heads 60 -> 20), turned to
  the viewer per pixel, blended with distance towards the canopy normal as the camera sees the field
  (upright blades face the viewer, lodged ones the sky: an up-facing canopy normal lit the middle
  distance like a lawn seen from above, a cream band brighter than the near blades). The canopy normal
  stands for the mean of the faces the camera sees, which is shorter than a unit normal: a face turned
  from the view by phi shows cos(phi) of its area and catches a sun behind the viewer by cos(phi), so
  the lit faces average pi/4 of the full cosine; the canopy normal leans away from the sun until its cosine
  is that mean (a unit one shone pale at 20-40 m in a front-lit field). Gloss low (0.02 live, 0.012 dead),
  filtered by the normal's screen variance (Tokuyoshi & Kaplanyan 2021) and reduced with distance.
- The live greens get GoT's push towards blue-green with 30 % more chroma at the same luminance (the
  weathers' warm light and the colour grade take a quarter of a green's saturation); a green that is already
  cool (blue over green above half: the glaucous fescues, grey hair-grass, wormwoods) keeps its photo hue and
  takes only the chroma, so a front-lit stand of them does not turn cyan-white.
- Sky occlusion: the slot's baked hemi x 1.6 (open ground 1, sheltered slots graded; the stock grass's
  x 3.5 made it binary) times a base ambient occlusion by the point's height above the ground in canopy
  heights (the species' leaves at the season's growth, at least 0.12 m): 0.3 at the ground (0.5 at the
  draw radius, 0.1 more from `d1` to `d2`, where the basal pairs fade out and a blade stands for a stand
  whose lower leaves hide one another's dark bases), 1 at the canopy's top, darker in clump centres. Lodged
  mats and the basal leaves lie in the dark base along their whole length; culms and heads above the canopy
  are open. The far field keeps the base occlusion, so its mean matches the near field.
- Between the near blades lies the stand's own litter (`deffer_impl_flat.ps`), not bare soil: the ground
  under the blade grass turns toward the canopy colour of the day shifted a third of the way to a grey-brown
  thatch, at 40-65 % of its value (the ground's own light and dark kept as the litter's grain), by 0.65 of the
  blade cover, and is darkened up to 25 % by the shade of the sward of the day (its standing growth or lodged
  thatch against 0.45 m, times its cover). Both ease out between 0.35 and 0.7 of the draw radius, where the
  blades hide the ground. The round-1 ground (soil darkened 45 %) showed near-black gaps between pale straws
  looking down at an October field; March's lodged mat lay on dark wet soil. Past the basal pairs (`d1` to
  halfway to `d2`, `da_gtint2`) a gap looks into the stand's lower leaves rather than onto its litter: the
  ground there is the stand's under-layer, the canopy colour with less thatch (0.15 of the way) at 60-75 % of
  its value by 0.85 of the cover - about the mean of the blades with their base occlusion, so the gaps never
  glow - and only 10 % of shade on top.
- The tufts' shade on the ground (1.6.1): the litter's shade and the ground's sky light follow the very tufts
  the blades grow in, not the slot's mean. `da_grass_tufts.cs` fills a toroidal window around the camera
  (`$user$grass_tufts`, R8, 2048 texels of 6.25 cm, 128 m) with how deep a tuft shades its foot at each point:
  the slot's dominant community, the patch's species (`grass_species_pick`'s graminoids) and its Voronoi clump
  by the generation's own hashes (`da_grass_hash.h`, shared with `da_grass_gen.cs`), `(1 - (d / reach)^2)^2`
  around the clump's centre (reach: 0.6 of the cell less the clump pull, plus a third of the sward's height),
  times the sward's depth (as the litter's shade) and the root of the share of its lanes alive at the centre (a
  thin stand's tuft has fewer blades, not a smaller foot; out under a tenth). The window moves like the ground
  window, baking only the 32-texel strips it moves onto, and is baked whole when the season's clumps change
  (`fill_clumps`, a level, a step of the day: `$user$grass_clumps`, a row per community). The terrain reads
  it with one tap inside `d2` (easing into the stand's mean from 0.7 of it): the litter's shade gathers under
  the tufts and out of the gaps between them (twice its mean at most), at least 60 % of the tuft's depth at
  its foot (a uniform shade lit a lone tuft at a road's edge like bare soil), and the hemi term loses 75 % of
  the occlusion in any weather (the sun's shade is the casters'), easing out with the blades' thinning. Cost
  at 1080p: 0.01-0.02 ms in the scene (the shade computed in the terrain's pixel shader cost 0.12 ms looking
  down into a meadow). QA: `tools/qa/water/ground_ao.lua`.
- Wet blades: the rain apply pass darkens and glosses them like any surface (foliage porosity 0.35);
  fill-map puddles never stand on foliage. The snow layer skips foliage (the blades carry their own snow).

### Shadows

Blades cast into the near sun cascade (7 vertices, at least one shadow texel wide - the cascade's box over the
map size, 20 m and `r__sun_cascade_mid` - facing the light, with
their heads) and on presets Default and above into the middle cascade (3 vertices, culms with their folded
heads), each cascade with its own keep per texel and a fixed LOD, so no shadow pops when a blade crosses
`d1` or `d2`. The far cascade gets no grass (a texel there is a third of a metre and flickered in wind);
contact shadows and the base occlusion carry it. Local lights take no grass casters, nor the cards left beside the
blades (litter and decals on the ground; `r__light_details` keeps deciding on a level without blades). The blades' shadows
follow the preset alone: `r2_sun_details` keeps deciding for the detail cards that stay (forbs, ferns,
litter), because grass without its own shadow floats over the ground.

### Wind, trampling, shots, explosions

The blades bend with exactly the stock tufts' wind (the travelling gust field, the two wave groups, local
heading, sheltering by the baked hemi) and the 8 wind motors (boots, bullets, blasts, scripts), plus the
push field of every creature and settled object, plus a lean downhill. Shots and explosions keep working
through the motors; trails of NPCs, mutants and the player press the grass through the push field. An anomaly whose
section blows air (`blowout_wind`: the springboard, the gravitational ones) rings out from its discharge like a blast,
at three times its radius (3..12 m) and twice its `blowout_wind_power` (1.6.2, `CCustomZone::UpdateBlowout`): stock
only nudged the global wind factor, which the wind field overwrites every frame.

The wave is the gusts and lulls of the moment, not a harmonic: the tuft's lean is its mean (0.42 + 0.38 x the wind)
plus the rest of the way times the turbulence at its root (`da_wind_turb`: eddies of 8 and 4 m ridden downwind on
the gust field's scroll, the small ones boiling on the wave's clock), so a gust bows the grass in a patch that runs
across the meadow and a lull lets it stand. (`da_sway` of the wave's phase rocked every blade from almost upright to
its full lean about once a second in any wind, tonal at 1-3 Hz: twitching.) A blade turns its flat side toward its
lean by as much as it leans (`grass_lean`: its own facing within a twentieth of its length of upright, the lean's at
a third): near upright the tip's heading swings round at the least push, and a blade pushed back past upright - a
trampled one standing up again, an upwind-facing one in a gust - spun about its length in a frame.

The wave, the gust lean and the bob scale with the real wind (`field.z`, the effective wind 0..1.25,
`smoothstep(0.03, 0.45)`): the gust field's amplitude never drops below 0.6, and without that factor a dead
calm still rocked every blade. The motors are not scaled: a boot or a blast moves grass in any weather.
Tall thin blades carry the stock tufts' wave at 0.65 and a light bob (0.05 per unit of wind at 3.2 rad/s).
Measured (escape, DX11, frames 1.2 s apart): calm 2.33 mean difference against 2.15 for a frozen wind
clock (the TAA floor), wind 0.9 9.6.

## Seasons

The blade state comes from its species' `[grass_keys_N]` at `D` every frame (no rebuild, no reload): new growth height,
green share, cohort green, thatch share and lodging, straw lodging, seed heads and their stage, tip
dryness, damp, frost, density, flood; colours live / yellowing / straw / thatch / seed. Last season's thatch
rolls over to the new cohort per blade by hash inside the winter window, so it never shows. `grass_visible`
of the snow frame (`saturate((0.9 - open cover) / 0.6)`) thins every blade: 0 from 1 December to 28
February - not a single blade. In November and March the snow gate lets through what tops the local snow
depth (trails included) by 3 cm, so tall culms, reeds and sedge tussocks poke through first; wet early snow
lays the leaves down. Under more than 1-4 cm of snow (by hash, so a patch's margin thins out) leaves, basal
pairs and a lodged mat lie pressed under the load: only the stiff standing culms pierce a patch, upright.

**Stock cards** (`[stock_keys_*]`): an item is drawn iff the stable hash of its root is below its class's
keep, its height scales by the class's height, and it is buried where the snow depth reaches
`max(hide_abs, hide_rel x top - hide_margin)`; the previous frame's rows drive the motion vectors. The
vertex path, the DirectX 12 mesh path (the amplification shader drops what is not kept, the mesh shader
scales and buries) and every shadow pass apply it. A class whose keep stays zero for the next days is not
even packed: the look-ahead covers 32 frames of D's motion (at least 4 days, along the direction D moves), as a
slot repacks every 15-30 frames, and a class that comes back after all (a pin jumping D) repacks every cached
slot at once. Unknown content follows the generic grass curve times `grass_visible`. The CPU rebuild of the
detail manager on a season change and the `[season_grass]` / `[season_shrubs]` shares are gone; a mod that
replaces the detail vertex shaders draws its details without the seasonal keep.

**Ground tint** (`deffer_impl_flat.ps`): the terrain sets hardly change brightness between seasons, so the
ground under the blade grass takes the canopy colour of the day (each community's, mixed from its species
by their shares: live, thatch and dead): lightly between
the near blades, fully where they have faded out, the hue only, scaled by how much brighter the canopy is
now than on the reference day of the latched terrain folder; under the near blades the litter of the stand
(Shading, above). Nothing is tinted where no blade grass grows or where `grass_visible` is 0.

## Settings and presets

- `r__grass_distance` (the player's row "Grass draw distance" under the quality preset, 1.6.2; the preset's five
  tiers as tokens of its own, `grass_distance_minimum` ... `grass_distance_maximum`, captioned `st_opt_<token>`):
  writes `r__detail_radius` 60 / 90 / 105 / 133 / 200 m (the blades reach 1.5 times as far: 90 / 135 / 157 / 200 /
  300 m). Unset (a user.ltx from before 1.6.2) it takes the quality preset's tier, and a new quality preset brings
  its own (`CCC_Preset`; the menu moves the row when the preset row is picked), which the player may change after.
  The record budget is the larger tier's of the two. The blades follow at once (`blade_reach` reads the console's
  `dm_current_fade`), the stock detail cards at the next detail reload.
- `r__detail_density` is one for every tier, 0.5 (a spacing): density scale `S = 0.25 / spacing = 0.5`, times each
  species' calibration (`density_cal`). It replaced the density row of 1.6.1 (0.6 / 0.6 / 0.5 / 0.35 / 0.25 by the
  same tiers, with the radius the preset's 49 / 70 / 100 / 130 / 160 m). The records go linearly with the density
  everywhere (the keep is `dens / lattice density` and the lattice itself scales with `S`), but the far field is
  cheap: the pixel floor widens its blades and the main view's occlusion takes what lies behind a hill, so past
  some 250 m a longer radius adds few records. Every tier keeps the same thinning, too slow to be seen (the cover
  eases from 1 at `d2` to 0.85 at the detail radius and 0.7 at the reach, `kCoverAtRadius`), and differs by its
  reach alone: 60 / 90 / 105 / 133 / 200 m (reach 90 / 135 / 157 / 200 / 300 m), each drawing about the records of
  its 1.6.1 density at the Extreme preset's 160 m (where any density row reached 240 m, the cover 0.38 at 160 m
  and 0.24 at 240 m). The first round of 1.6.2 had solved the thinning per tier for those records instead and kept
  the palette's weight out there: the Default tier kept 0.38 of the cover at its radius, a meadow at 60 m half
  bare.

  Measured on the l01_escape meadow at the Extreme preset (`tools/qa/water/grass_distance.lua`, July noon, DX11
  1080p on the rig; records of the view at the eye / 35 m up, the GPU frame at the eye):

  | Tier | 1.6.1 (density row at 160 m) | 1.6.2 (distance row) |
  | --- | --- | --- |
  | Low | about 205 k / 93 k | 282 k / 134 k, 17.3 ms |
  | Default | about 247 k / 111 k | 308 k / 169 k, 17.6 ms |
  | High | about 352 k / 159 k | 362 k / 245 k, 17.7 ms |
  | Maximum | 493 k / 222 k, 20.2 ms | 446 k / 359 k, 19.1 ms |

  (1.6.1's lower densities scaled from its Maximum: the records go with `1 / spacing`.) The far field's full stand
  costs the open view more than 1.6.1 did; at the eye the near field had held the records, and Maximum is cheaper
  there. The shadow casters follow the density: 133 k -> 93 k at Maximum.
- `r__detail_height` (the player's row): scales leaf, culm and head lengths and the fold test, live.
- `r__detail_radius`: the LOD distances and the stock cards keep it; the blades reach 1.5 times as far (below),
  the last 15 % fades their height, the ground tint takes over.

**Reach** (1.6.1): at full cover to the detail radius, the far edge of the grass read as a line from any hill.
The blades now reach `kReach` = 1.5 times the detail radius (1.6.1 Extreme 240 m, 1.6.2 Maximum 300 m): past `d2` the
cover thins as `1 / sqrt(1 + x^2)`, `x = (d - d2) / scale` (`grass_taper`, density thinning without widening:
the view's lattice level, early out and keep; the casters keep the stand's full density, so a field's shadow is as
deep as before). `blade_reach` sets the scale so that the cover is `kCoverAtRadius` (0.85) at the detail radius;
1.6.1 solved it for the records of the stock reach at full cover (Extreme 0.38 of the cover at 160 m, 0.24 at
240 m). The slope is at most 0.2 % a metre and nothing changes inside `d2`: running across a meadow the thinning
cannot be seen. The ground tint takes the opening gaps over (`da_gtint2.zw`: the
near-to-far strength follows `1 - cover` too), and the reed billboards hand over at the new reach. QA:
`r__dbg_grass 256` is the stock reach at full cover; `tools/qa/water/grass_reach.lua` shoots both, at the eye
and from above, with the record counts.

| Column | Minimum | Low | Default | High | Extreme |
| --- | --- | --- | --- | --- | --- |
| Pixel floor `k_px` (render px) | 1.5 | 1.5 | 1.25 | 1.0 | 1.0 |
| LOD `d1` / `d2` (m) | 8 / 25 | 9 / 30 | 10 / 35 | 11 / 40 | 12 / 45 |
| Sun cascades | near | near | near + middle | near + middle | near + middle |
| Blade records | 192 k | 256 k | 400 k | 640 k | 896 k |
| Head records | 16 k | 16 k | 24 k | 32 k | 48 k |

The record budgets hold a full stand (presence no longer thins it by the palette), its basal pairs and litter
and the forbs' leaves. Memory at Extreme: 56 MB of blade records, 0.75 MB of heads, the ground window 4, 16 or
64 MB (the last at the Maximum distance), the push field 3 MB, the slots up to 10 MB, the triangle cells a few MB.

Measured with the species (l01_escape meadow at A, June, DX12, DLSS Balanced at 2560x1440, RTX 3070 Laptop,
GPU timers `grass` + `grass_gen`): Minimum and Low 0.60 + 0.54 ms, Default 0.82 + 0.63 ms, High 1.34 + 0.86 ms
(40 species); Extreme with the 64 species and 16 forbs a community 1.98-2.18 + 1.40-1.64 ms (527 k records,
no bin over its window, the budget floor at x1; the forbs' dozen leaves past `d1` were 666 k). The species and
community tables (64 x 46 + 16 x 19 float4) are filled and uploaded only when the day, the height setting or
the snow's grass visibility change (a calendar day moves once a second). The earlier blade-grass figures below are from before the species (one profile per
class, presence thinned by the palette).

Measured on the stand before the species (Extreme, l01_escape field, GPU timers of the blade and card draws
`grass` + `grass_gen`), final look:

| Renderer, resolution | Calendar (October) | January |
| --- | --- | --- |
| DX12, native 2560x1440 | 0.99-1.08 + 0.34-0.38 = 1.36-1.44 ms | 0.45-0.48 + 0.03 ms (stock cards only) |
| DX12, DLSS Balanced (1484x834) | 0.72 + 0.31 = 1.03 ms | 0.39 + 0.03 ms |
| DX11, DLSS Balanced | 0.52 + 0.41 = 0.93 ms | - |

The cover handoff of the basal pairs (final fixes) costs about 0.04 ms of generation on the October view
(DX11, DLSS Balanced: 0.51-0.52 + 0.43-0.46 ms); the draw did not change measurably.

Before the look pass (static / turning camera) the same views cost 1.11 / 1.10 ms native on DX12, 0.88 / 1.11
ms with DLSS Balanced on DX12 and 0.84 / 1.05 ms on DX11: the look adds about 0.3 ms at native 1440p, inside
the 2.0 ms target. The stock cards cost 0.81-0.88 ms (DX12) and 1.36-1.79 ms (DX11) there, plus 9-10 ms of CPU
for their decompress; with the blades that CPU share is 1.2-1.5 ms. Frame rate against the stock build (before
the look pass): from 1 % lower (DX12, turning) to 23 % higher (DX11).

## Compatibility and fallbacks

- Levels and saves are untouched: everything is computed from `level.details`, the collision mesh and
  the season data at load.
- Missing compute shaders (an older content install), a shader that does not compile, a level without
  blade grass or without a collision mesh: the blades stay off on that level and the detail cards draw
  every class (`! [grass] the blade grass is off on this level (...)` in the log).
- Unknown detail content stays stock. A mod that changes a sprite, re-encodes the atlas or adds models
  falls back to stock cards for exactly those objects.

## QA

- `r__grass_blades_stats`: preset, radius, rings, lattice, groups and lanes of the frame, records per bin
  against the bin windows, overflowing frames, the budget floor, memory.
- `r__grass_species_pin <name|off>`: every lane grows that species (its id from `[grass_species_N] name`,
  e.g. `cha_ang`), the species gallery's switch; `off` hands back to the communities.
- `r__dbg_grass <bits>` (runtime): 1 no basal pairs, 2 no litter, 4 each community's first graminoid only,
  8 no early out, 16 no stem leaves, 32 presence by the palette near the camera too, 64 end every lane after
  the early out, 128 no wind, 256 the stock reach (no thinning, the detail radius), 512 presence back to the
  palette's weight from `d1` to `d2` (1.6.1).
- `qa_season_pin <month>` / `qa_season_year 60` (`SEASONS.md`) drive `D`; grass, stock cards, the ground
  tint and the snow follow it continuously.
- Log: `* [details] N models: K known by content key, B grown as blades`, `* [grass] season data: N detail
  keys, S species in C communities`, `* [grass] ground bake: ...`, `* [grass] blades on: ...`.
