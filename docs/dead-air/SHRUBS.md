# Procedural shrubs (1.6.0)

The bushes of the Zone are no longer the stock card models. Every placed bush of the four bush classes (single
bushes, bush groups, thickets, conifer bushes) is a bush of a real ChEZ species - 33 shrubs of the zone's ten shrub
communities - grown at level load from its species' branching rules, with its own wood, leaves, flowers, fruits and
catkins following the species' phenology day by day, its every axis bending under the wind service's field and
motors. Bushes only: a stock model scaled into a tree stays stock, and no tree species stands in for a bush (the
young pines, birches, aspens, oaks, alders and fruit trees of the first version went; the trees get their own
replacement). The pipeline follows the production vegetation tools and the shipped open-world games: a rule-based
branching framework (Weber and Penn 1995, the SpeedTree generators), the leafy shoots filling the crown's lit shell
by one pass of space colonisation (Runions, Lane and Prusinkiewicz 2007), the light inside the crown baked per leaf
from the crown's own leaf density (the production foliage tools' occlusion), small leaves grouped as sprays and
drawn as their real leaves, three distance tiers crossfaded by element hash (leaves, then leaf clusters, then crown
blobs), a pixel floor that thins the elements under a pixel and grows the survivors to keep the area, a skeletal
wind of rigid axis rotations evaluated in the vertex shader (Pivot Painter's scheme) and the leaves' flutter in
eddies carried downwind, the same in the view and in the shadow maps. DirectX 11 (R4, FXC SM5) and DirectX 12 (R5,
DXC SM 6.x) run the same code.

Data: `configs/dead_air_x64_shrubs.ltx` (written by `tools/shrubs/make_shrubs.py`, the generated tables in
`SHRUB_DATA.md`); the season day `D`: `SEASONS.md`. Sources: `src/Layers/xrRender/da_shrub_gen.{h,cpp}` (the
generator, no engine types), `da_shrubs.{h,cpp}` (the shrubs' population: the stock bushes, the species, the
templates, the tiers), `da_veg.{h,cpp}` and `da_veg_internal.h` (the vegetation core the shrubs share with the
trees, `TREES.md`: the frame's records and chunks, the culling grid, the bend pass, the draws of every pass); shaders
`r3/da_shrub_*`; hooks in `r2_loader.cpp`, `r2_R_render.cpp`, `render_phase_sun.cpp`, `render_phase_sun_old.cpp`,
`r2_R_lights.cpp`, `r3_R_rain.cpp`, `r2_R_calculate.cpp`. Tools: `tools/shrubs/` (the data script, the research
tables in `data/`, the preview generator and its Blender gallery in `preview/`, the leaf and bark texture
generators and the array builder in `textures/` - `make_leaf_textures.py`, `make_bark_textures.py`,
`build_arrays.py`, their manifests and READMEs; the arrays `textures/da/shrub_leaf_d|n`, `shrub_bark_d|n`).

## Which models are replaced

A stock bush is a multiple-use model compiled into the level: tree-format parts (`MT_TREE_PM` cards with
`flora\leaf_wave`, usually an `MT_TREE_ST` stem part) under one `FLOD` (`MT_LOD`) node, which is the placed
instance and its far billboard. The 22 outdoor levels place 17,173 of them in the four classes (BUSH 11,472,
BUSH_GROUP 3,693, THICKET 1,085, CONIFER_BUSH 1,375 - pairs, quads and 21 m thickets of a handful of 2-4 m
meshes re-dressed with the bush card families).

- **Content key.** xrLC writes the same model with permuted vertices and triangles, other UV-seam duplicates and
  up to 15 um of float noise from level to level, so the key is order-independent: every vertex as its position
  in millimetres and its uv wrapped into one tile, every triangle by its three vertex hashes sorted, a part by its
  shader string, triangle count and the wrapping sum of its triangle hashes (FNV-1a 64), the bush by its parts'
  keys sorted. The parts are read back from `level.geom` (the GPU buffers keep no CPU copy; `FTreeVisual` keeps
  its buffer ids, `dxRender_Visual::level_shader` its shader string). `[shrub_kinds]` lists the 96 keys of the
  shipped levels with their class, look (`dry` cards, the Red Forest's `red` ones) and, for groups and thickets,
  their clumps; another compile of a known model is found by its signature in `[shrub_families]` (its parts'
  shaders and triangle counts, the local box within 2.5 mm, the area within 0.2 %). A model not listed - a mod's
  bush, a tree, a log - stays stock.
- **Size policy** (`[shrub_policy]`): a bush scaled into a tree crown stays stock - its top above 8.5 m, a
  thicket's above 10.5 m, a conifer bush's above 4.7 m (past that the stock conifer bush is a young pine, a tree) -
  and so does one under 5 % of its model.
- **Gameplay is untouched.** The level's collision keeps each bush's passable `materials\bush` hull (about 0.7
  of the visual's height and width; bullets pass, sight is cut to 0.3 per crossing) and the AI graph and cover
  points were compiled around it: the procedural bush keeps the stock bush's footprint and height (a single
  bush stands on the stock root, 0.96 of the stock top and 0.86 of its box), and a group's or a thicket's clumps
  are filled with bushes about `spacing` (2.6 m) apart, each on the ground under it.
- **Out of every pass at no cost per frame.** While the shrubs stand (`r__shrubs 1`), a replaced bush's FLOD is
  taken out of its parent hierarchy at level load (`da_veg_level_load`, before the GPU-driven registry): every
  consumer - the views, the sun cascades, the local lights' faces, the rain map, the LOD billboards, the
  GPU-driven registry, the ray tracing scene - walks the sector hierarchies, so it is gone from all of them.
  `r__shrubs 0` puts them back live (the registry and the ray tracing scene rebuilt, the temporal history reset).

## Species and communities

**Species** (`[shrub_species_N]`, up to 48): the shrubs of the ChEZ forests, edges, wetlands, villages and old
fields after the zone's floras and Didukh's vegetation surveys, with their phenology from the Chornobyl reserve's
nature calendars and the Ukrainian and Belarusian shrub calendars:

- willows: *Salix cinerea*, *S. aurita*, *S. pentandra*, *S. caprea*, *S. acutifolia*, *S. triandra*,
  *S. viminalis*, *S. rosmarinifolia*;
- forest and edge shrubs: *Frangula alnus*, *Corylus avellana*, *Euonymus europaeus*, *E. verrucosus*,
  *Viburnum opulus*, *Lonicera xylosteum*, *Padus avium*, *Amelanchier spicata*, *Rubus idaeus*, *R. caesius*,
  *Rosa canina*, *R. majalis*, *Crataegus monogyna*, *Sambucus nigra*, *S. racemosa*, *Ribes nigrum*,
  *R. uva-crispa*;
- village and road shrubs: *Syringa vulgaris*, *Philadelphus coronarius*, *Amorpha fruticosa*, *Prunus
  cerasifera*, *Cytisus scoparius*, *Chamaecytisus ruthenicus*, *Hippophae rhamnoides*;
- the pine forest's: *Juniperus communis*.

The large shrubs that grow into small trees (goat and bay willow, bird cherry, hawthorn, cherry plum) grow as
multi-stemmed bushes within their shrub heights; the trees proper (pine, birch, aspen, oak, alder, rowan, the
fruit trees, box elder, black locust) are not grown as bushes at all.

Their colours are measured: 4,334 dated iNaturalist photos of Ukraine and Belarus north of 48 N, nine a month
bucket per species, measured per colour role (summer leaf, the spring flush, autumn kinds, the dead leaf, bark,
twigs, flowers, fruit, catkins); a flower's luminance is raised to its colour class's floor (the photo median is
shaded), the autumn colours are the photos' two dominant kinds with their share.

A photo is exposed by its camera, not measured: a green bush that fills the frame is pushed to mid grey. The
leaves therefore keep half of their darkness relative to the species' median at the game's level - the blade
grass's July meadow is a linear luminance of 0.080, the shrubs' summer leaves a fifth under it at 0.060 (the
dicots' darker sun leaves; at the meadow's level a bush read pale against it), the leaves of May 1.3 times that and
of June 1.12 (the fresh leaves, thinner and with less chlorophyll, darken over the first summer weeks), the spring
flush 0.09; silvery leaves reflect more (sea buckthorn's scales 1.7 times, the rosemary willow's silky hairs 1.3:
`SILVERY`); the brighter subjects (autumn and dead leaves, fruit, twigs) keep their measured colour scaled by the
exposure their kind was given. The photos' chromaticity is not taken as it is: their warm light, white balance and
the cameras' saturation leave a leaf's blue at a sixth to a tenth of its green, where the leaf spectra over the sRGB
primaries give a third or more (the ColorChecker's foliage patch, linear 0.64 : 1 : 0.37) - a bush of those leaves
read acid yellow-green against the meadow. `leaf_tint` raises the blue by a soft floor (0.27 of the green plus half
the photo's excess over 0.12, at most 0.42; May's 0.25-0.40, the young flush's 0.23-0.38, yellower) and keeps the red
within 0.48-0.80 of the green (May 0.50-0.84, the flush 0.52-0.86), at the colour's own luminance, the photos' order
between species kept (at the ColorChecker's level itself a crown's sunlit top read cool and frosted); the yellowing
leaves of autumn and an evergreen's winter bronze keep the photos' chroma. Glaucous foliage mixes a share of a
blue-grey green at the leaf's own luminance (sea buckthorn, juniper, the rosemary willow: `GLAUCOUS`); the grey and
the eared willows' downy leaves a little of a neutral grey-green (`GREY`; a fifth of the blue-grey made them frosted
blue domes on the wet meadows they fill). The species' lightness follows the flora's descriptions on top of the
photos (`LIGHTER`: hazel's soft matte leaves 1.35, mock orange 1.3, guelder rose 1.25, cherry plum 1.15; the dark
glossy elder and alder buckthorn 0.9, hawthorn and bird cherry 0.92): half of the photos' own darkness alone left
every broadleaf species on one dark green, and a fifth either way was lost in the crown's shade. The underside
is the summer leaf a little greyer by the photos' glaucous index, at about the upper side's level: its tissue
reflects more, but it faces down into the leaf's own shade, which the two-sided shading does not give it - lighter,
the turned-away half of a crown seen from the side read frosted.

**Checked against a pack of real photos.** 27 summer photos (June to August, 36 bushes) next to their meadows -
Volhynian Polesie, the Chornobyl zone in 2019, the Polesie and Narew national parks, hedgerows and willow carr -
measured alike to the game's frames (the bush's pixels against a sunlit stretch of its meadow, sky and cloud
pixels out; in the game the shrubs' pixels by a frame without them, their shadows kept off the meadow sample): the
bush's median luminance 0.40 of the meadow's (quartiles 0.25-0.60), its lit tenth 1.6 of it, its median a quarter of
its lit tenth and its darkest tenth a twentieth, its hue 8 degrees greener than the meadow's, its saturation 0.91 of
the meadow's, its linear blue 0.38 of its green. The game's frames of July noon (five views, 8 to 95 m; two runs,
the views' light changing with the clouds) give 0.26-0.47, 1.1-1.3, 0.28-0.30, a ninth, +10 to +13 degrees,
0.91-0.98 and 0.36-0.38; before this round 0.61, 1.5, 0.45, a fifth, +14, 1.28 and 0.24 - the bushes stood out
light, saturated and flat. The deepest shade stays lighter than the photos' (the engine's flat
ambient lifts every shadow, the meadow's too), and the game's hue is some 15 degrees yellower than the photos'
everywhere, the grass's too: the colour grade (`da_grade`). In the game the lit side of everything, the meadow and
the stock trees too, is bluer and greyer than its shade.

**Communities** (`[shrub_community_N]`, up to 24 species each with a share): the zone's ten shrub communities -
moist and dry pine forest, oak-hazel, the wet and the riverside willows, the alder swamp, the village, the old
field, the burn regrowth and the hedgerow. A bush takes the community of its spot: the Red Forest's red stock
the burn's, else the grass community under it (`[shrub_grass_community]`, the blade grass's community map at
the bush), else its class's (`[shrub_class_community]`, closed forest has no grass). Among the community's species
that stand in for the stock class, a bush of its height is picked by share (within the species' height range and
a quarter beyond first, then any height, then the class's own community); a few stand dead (`dead`, and
`dry_dead` of the dry stock): no leaves, grey wood.

## The generator (`da_shrub_gen.cpp`)

A bush is generated in template space (y up, the root at the origin) for its height and width; lengths are the
species' for its reference height `href`, the woody structure scales to the bush, the leaves, flowers and fruits
keep their real sizes. Deterministic for a species, size and seed.

1. **Stems** from the root crown (`stems`, `root_r`, `stem_spread` inner to outer, `stem_len`, `stem_up`
   tropism, `stem_arch` for canes bending over) or one leader with whorls (`leader`, `whorl_step`).
2. **Branching levels** `l1`..`l3`: children along the parent by `density`, `phyllo` and `per_node` (2 =
   opposite pairs), `angle` from the parent (base to tip), `len` as a share of the parent, `up` tropism,
   `outward` light seeking, `wobble`; every child is cut by the crown's **envelope** (ellipsoid, vase, cone,
   column, dome, fountain, cushion), its radius swelling and denting by a fifth over a value noise of half the
   crown's radius (a lumpy outline, each stem's foliage its own cloud, not a clipped hedge), and dropped under
   `min_len`. Every axis follows its heading under the tropism and the arching, with a lean off it that wanders
   (an Ornstein-Uhlenbeck process, a third of a metre's correlation, its spread the level's `wobble`, at most some
   thirty degrees) and always returns: stems bow in long curves; a fresh random turn per segment had left them
   straight sticks fanning from the root, an unbounded turn rate curled them into hoops. The twigs and the shoots
   (level 2 up) kink a little at every node, side to side (sympodial growth). A long thin axis sags under its own
   weight (a cantilever's tip angle, its length cubed over its radius squared: a 1.5 m wand of 8 mm some
   fifteen degrees, the species' `sag` scaling it), and an axis ends where it leaves the crown, tapering to its tip over what it grew (its
   course wanders and sags past the straight free length it was promised: bare rods stuck out of the crowns).
3. **Young growth**: the last `shoot_len` of every axis carries the current year's leaves at `leaf_spacing`
   (`leaf_per_node`, `leaf_phyllo`, `leaf_angle`, turned to the horizontal by `leaf_flat`, rolled at random so
   the leaf angles spread), the first ones small (0.55), the largest past the middle, shrinking to `tip_shrink` at
   the tip; axil flowers, catkins near the tips.
4. **Filling shoots** (space colonisation, one pass): points of the crown's lit shell (`fill_shell` of the radius
   at the side, the same depth under the top), accepted by their light (the outer shell and the top) and a value
   noise of a third of the crown (`fill_clump`: clumps and holes), each grown to from the nearest usable wood
   within its reach (a thick stem counts farther than a twig; not the stems' bare foot `fill_from`), a
   `fill_len` shoot (32 cm at most: a current year's shoot; longer ones crossed the empty air to the crown's
   surface as straight wires) of a few segments turning `fill_up` toward the light, node by node, carrying leaves
   like the young growth - until the leaf
   area reaches the species' **LAI** over the crown's larger projection (from above or from the side). Points no
   wood reaches stay holes. One pass would hang every shoot whose point lies past a twig's end from that tip - a
   burst of shoots like a dandelion clock: a quarter of a wood segment costs more by the shoots already leaving it
   (0.3 of the reach each), and a point past the end is reached from the twig's last half.
5. **Dead stubs**: an old stool (three stems and more) has one to five stems broken off low among the living ones,
   8-35 cm, clear of the living stems, grey weathered wood (the wood record's level byte carries the flag), with no
   branches, leaves or shoots. **Prickles and thorns** (`prickle_spacing`, `prickle_len`, `prickle_hook`,
   `prickle_base` their foot's radius over their length): a rose's and a gooseberry's along the stems and branches, a
   hawthorn's and a sea buckthorn's on the young growth alone (`prickle_young`: a branch's last metres, every twig;
   their old trunks carry none; the filling shoots get theirs after they grow) - short spikes tapering to a point (a
   sixth of their foot, however thin), leaning back toward the base, a rose's broad-footed and hooked over; rigid,
   leafless, riding their wood in the wind, in their own colour (`prickle_colour`, the wood record's level byte flags
   them: in the young twig's green a dog rose's red-brown hooks hid on its green canes). The young shoots kink side to
   side at every node by the species' `zig` (a hawthorn's zigzag twigs; straight ones read as pick-up sticks). A long
   thin axis sags by the species' `sag` (an osier's wands
   and a hazel's rods stand, a rose's canes bow), a stem that does not arch a third as much.
6. **Spurs** (`spur_share` of the leaf area: plums, currants, hawthorn, sea buckthorn): rosettes on short shoots
   along the older wood by light, the old wood's flowers on them; for every broadleaf they also carry what the
   filling shoots could not reach of the LAI (in a real crown most of the inner leaf area sits on short shoots; a
   conifer's needle tufts sticking out of its old wood read as starbursts).
7. **Leaf kinds**: a simple blade; a compound leaf as one element (rachis and `leaflets`, drawn by its texture,
   `leaflet_ratio` their width over length); a needle tuft per `leaf_spacing` of shoot (juniper); a
   **spray** - a `spray_len` stretch of a small-leaved shoot with its `leaflets` leaves as one element (the
   willows, the plums, broom, sea buckthorn): four to six times the leaf area per element, drawn as its leaves.
8. **Flowers and fruits** at the shoot tips, in the axils or on the old wood (`bloom_place`, `bloom_share`,
   `bloom_size`, `fruit_size`, `bloom_droop`), **catkins** near the young tips (`catkin_*`: hanging hazel catkins;
   upright willow ones).
9. **Cap**: `max_leaves` elements (the data script sets it from the species' leaf area, 6,000-22,000, a conifer's
   needle tufts 40,000); over it the
   least lit go (by a guess from their place in the crown), the spurs keep their share.
10. **The light inside the crown**: the sky each leaf, flower and wood segment sees past the rest of the crown. The
   leaf area is splatted over a grid of a sixteenth of the bush, each cell's view of the upper hemisphere marched
   through it in twelve cosine-weighted directions - a step of length l through a leaf area density d lets
   exp(-0.5 d l) through (a spherical leaf angle distribution, Ross and Nilson), the cell's own leaves half - and
   every element takes the cells round it trilinearly. The outer top sees most of the sky, the underside and the
   inside a twentieth of it; the leaves' `light` is this from here on (the shaders' occlusion, the flutter's
   amplitude, the autumn colour B's share). A leaf's **exposure** adds the sunlit ground's light from below (six more
   directions, a third of the sky's for a meadow's albedo): the pixel floor thins the crown by it, the least exposed
   first - by the sky alone the crown's lower side, open to the meadow, ranked with its inside and thinned with it
   at distance, a see-through skirt. A few milliseconds a template.
11. **Mechanics**: each axis's bend under the reference wind (a cantilever's tip slope, its length over its radius)
    and its natural frequency (by level and length).
12. **Clusters**: the leaves in cells of a hand's span or two (0.25-0.55 m by the crown's size), one element each
    for the middle distance, bending with the shoot that holds most of its leaf area, its light its leaves' mean
    by area (its exposure too, for the pixel floor), its flowers and fruits counted by their area in its mean leaf's
    (rounded by the cell's own hash, so the fractions add up over the bush); **blobs**: k-means of the clusters by
    leaf area (6-64, 0.15 square metres of leaves each: a dozen large ones read as stacked discs, forty over a large
    bush reached past its shoots - a dog rose's into the sky over its arching canes), the far
    crown, its light its clusters' weighted by their own (the outer ones are the ones seen); each blob two fifths
    wider than its clusters' spread (`kBlobGrow`, the shaders' `SHRUB_BLOB_GROW`: neighbours overlap into one mass,
    where blobs that only met left the meadow showing between them) and drawn in by its growth on the crown's outside
    (the outline stays where the clusters end), its leaf area restated so the wider disc's cover is the share the
    spread covered (the same leaves over twice the disc overlapped less and grew the far crowns by two fifths); the
    shadow maps take the spread, its cover and the place it was drawn in from (the record carries the pull).

The preview (`tools/shrubs/preview`) builds the engine's generator standalone: `shrub_preview.exe <ltx> <species>
<out.obj> h,w ... [--lod 0|1|2]` prints each bush's axes, segments, leaves, blooms, clusters, blobs, its leaf area
and its share of the LAI; `gallery.py <ltx> <out> [species ...]` renders every species twice in one headless
Blender session into a contact sheet.

## Templates and instances

Per species three heights (the middles of the thirds of its range) and three variants, only those the level's
placements use, generated on the task pool at level load and packed into one element buffer (32 bytes an element:
the wood ordered by level so a far tier draws a prefix of it, the leaves and clusters by importance - lit and
outer first - so the pixel floor draws a prefix and the interior goes first, the blooms from the top, the blobs by
area; a wood segment's, a flower's and a leaf's light in its spare byte) and one axis buffer (base, direction,
parent, attachment, flex, frequency). A placement is an instance of the nearest template, scaled to the
placement's height, its width within a fifth, every other one mirrored, with its own phenology offset
(`spread_days`), its seed and the stock bush's baked hemisphere and sun. The frame's instance record (80 bytes)
carries its transform, species, phenology, template, hemisphere and the wind at its root now and a frame back.

## Drawing

Per frame the CPU walks a 32 m grid of the instances against the view frustum and the occlusion map (HOM), emits
chunks of 64 elements into one list per pass and draw, uploads them, and every draw is one instanced call with no
vertex streams (the vertex shaders read the records by `SV_VertexID`). The frame is built in `Calculate`, after the
occlusion task is joined and before the sun pass starts: with the multithreaded draw (`r2_mt_render`) its task
draws the cascades on a worker while the main thread is still on its way to `Render`, and a frame built there came
after it now and then - the bushes left out of that frame's cascades, the whole shadow of a bush blinking off for
single frames. The rain's occlusion map takes the crowns on the main thread when it is flushed, and only in a frame
that redrew the map (a map kept from an earlier frame has an older transform). The view's bushes are emitted near to
far, so within each draw the near crowns' leaves fill the depth buffer first and the alpha-tested leaves behind them
fail the early depth test instead of being shaded; the bushes in view take their instance records before the ones
only the cascades see (at the record cap a shadow far out gives way to a bush in view). A compute pass
(`da_shrub_bend.cs`, one group a chunk, on the immediate context in `Render` ahead of every list that draws the
shrubs) writes every element's bend under the wind and the motors - the turn of its own axis kept apart (each vertex
turns by its angle at its share of the axis) and the turns of the axes it hangs from composed into one rotation and
a translation - and for the view's elements the same a frame back (the motion vectors): the vertex shaders read the
record instead of walking the chain of axes for every vertex (a near willow's sprays walked it for each of their
forty-eight vertices, twice: a millisecond of vertex work). The buffer (36 bytes a slot) is sized in `Calculate`
with a quarter's headroom for the local lights' chunks appended later and never reallocated later in the frame;
slots past it (a frame of more chunks than it grew to) walk the chain themselves. The main pass's vertex shaders
drop an element out of the view with all its reach (its size, the pixel floor's growth and the most the wind can
carry it; the side planes' distances by the projection's own scales, so a scope's narrow view culls nothing it
shows) before any of that. **Ray tracing** (DirectX 12): the shrubs live in the frame's buffers and in no
acceleration structure; the stock bushes they replace stand in for them in the ray tracing scene
(`da_shrubs_replaced_visuals`, `da_rt_scene.cpp`) - their swaying parts alone, as foliage and trunks - so the water
and the puddles reflect a bush where one stands (in its stock look), the occlusion and shadow rays skipping the
crowns as they skip every swaying crown (the shrubs' shadows come from their own cascade passes).

**The GPU cut** (1.6.3, the trees' cut's last phase: `r3/da_tree_cut.cs` `CUT_KEEP`, `da_veg.cpp` `emit`): every chunk the
view's walk emits is a candidate of the cut instead of a draw of its own, its route the draw the CPU chose for it (its
tier by the bush's distance, nothing about the look changes). After the trees are drawn - their trunks and crowns in the
depth - one lane an element leaves out what its vertex shaders would have thrown away: out of the view by its own bent
place and reach (the leaf's, the cluster's, a fruit group's three sizes), behind the depth pyramid (`da_hzb.h`: the
opaque world, the LODs, the first-person view and every tree), the wood under its wire rule (the ratio of its width to
the floor, its axis's hash, exactly as `da_shrub_wood_vs.h` keeps it); the survivors fill the draws' lists and the
draws are indirect. Ghost of Tsushima culls its proxy sets' instances on the GPU against the frame's depth
(`Proxyset Cull Inst Count`, `Max Depth Cull`) the same way. Behind a near poplar the view drew half a million shrub
elements - 107 000 sprays as 640 000 leaf quads, 95 000 wood segments, 95 000 fruit groups - for a few thousand pixels:
the vertex shaders ran every one of them and collapsed most. The cut keeps 30 % of the sprays, 9 % of the fruit groups
and 5 % of the wood segments there (Cordon village, Extreme, DX11 1080p): `shrubs` 3.68 -> 2.18 ms and 1.94 -> 1.06 ms
in the two bushy headings of `tools/qa/water/veg_cost.lua`, the bend pass 0.44 -> 0.24 ms; a heading with a bush or two
in view pays the cut's 0.1 ms. The cut's buffers have room for the lists of both populations; past it (or with the cut
off) the shrubs draw as the CPU chose.

| Tier | Up to | Draws |
| --- | --- | --- |
| near | `d0` | wood (all levels; the stems and branches eight-sided tubes, the twigs five-sided), each leaf its species' texture cut out by its coverage (from a mip of sixteen texels a side at the coarsest, by the array's loaded size: a leaf of a few pixels cut by a coarser mip's blot was a square, a front-lit crown at a dozen metres a brick mosaic; not a needle sprig, whose needles under a pixel scattered into a haze; under eight pixels, while a temporal resolve runs - TAA, DLSS, FSR -, the cut-out is hashed: its threshold an interleaved gradient noise of the pixel and the frame and its alpha the mip its pixels read, so the resolve sums a soft leaf of its outline and area - the hard cut drew a crown at 20-40 m as a mosaic of squares, an upscaler's sharpening squared them more; the crown's cover the same, 0.79 of a box over a poplar either way) from an eight-vertex hull fitted to it (folded, drooping, edge-on ones turned a little to the camera; compound and lobed leaves folded along their rachis or midrib too) while a leaf is two dozen pixels long, from the texture's quad past that (four vertices cost less than the fragments the hull saves there); a spray as its six leaves by the species' phyllotaxis (two-ranked hazel sprays lie flat, opposite pairs cross); needle sprigs as two crossed textured quads about their shoot, lit by the billboard's normal (one quad turned to the eye showed a flat frond: a fern); blooms |
| leaves | `d1` | the wood without the finest level, the leaves and a spray's leaves as textured quads, a needle sprig as one quad turned to the eye |
| middle | `d2` | the main wood, the clusters: camera-facing quads whose leaves are drawn cell by cell (`da_shrub_mass.h`, each cell the species' leaf texture - a compound leaf's leaflet - turned and mirrored its own way, with its own rank, fall order and facing, the cover by Beer-Lambert of the cluster's leaf area over the texture's coverage, the leaves clumping over a few cells so the holes and the dark inside behind them come in pockets; bare, the twigs as short random strokes). Leaves under three pixels gather into clumps of four, sixteen or sixty-four (fewer, larger cells at the same cover; the vertex shader's continuous level, its switches dithered pixel by pixel over a band of it and moved by each part's hash, the temporal resolve blending the two grids - a part switching whole popped as one walked by), a narrow leaf's clump and a needle tuft at every level an elliptic blot as wide as its cell (a needle's sliver left the far junipers bare), a broad leaf's clump its leaf's outline; each leaf within a clump its own facing and colour step (a clump of one read as a sequin): a far part keeps a ragged leafy outline and its holes - its mean blot (a part under nine pixels) lobed by a second octave of clumps; a smooth disc of the mean cover per part read as stacked coins, bonsai pads. A needle brush's part reaches a tenth past its tufts' bases and projects two thirds of its area (the near tufts turned to the eye; with all of it the far junipers read a third denser than their near tier), its clusters a quarter lighter than a broad leaf's and its blobs at 0.88 against 0.95, neither taking the far self-shade (the near tier shows its outer, lit tufts; at four fifths of a broad leaf's, under the self-shade, its far crown read near black); its parts' rims bristle with two dozen tufts' tips over a shallow dome (its thickly overlapping small parts, each with its own lit top and shaded foot, read as fish scales, the crown as topiary), each tuft's blot a brush of eight tips streaked with its needles over the darker inside (a smooth blot read as cotton wool) |
| far | `R` | the stems, the crown blobs the same way, their cells the species' mean leaf (a spray's leaf, a compound leaf's leaflet: one leaf texture a cell, as the clusters' cells their own leaves) (a dozen across, a blob showed leaves several times their size: flakes, coins and confetti on the crowns' edges), thinning and shrinking toward a blob's rim (a finer speckle where neighbours overlap) |

Each tier crossfades into the next over the last 15 % of its distance by the elements' hashes, the elements
crossing an end shrinking away (exact motion vectors, no dither); every switch inside a tier does the same - a
wood level dissolving out where its tier ends (the finest twigs past `d0`, the twigs past `d1`, a bare bush's a
tier later, the branches past `d2`, the stems at a half beyond it), the blades giving way to diamonds. The
distances scale with the bush's size. The pixel floor keeps the share `(size x px / floor)^2` of an element kind
(at least 3 %) and grows the survivors by `1 / sqrt(keep)` - the prefix ending softly (`da_veg.cpp` `emit_thinned`,
the trees' too: its last 30 % shrinks away by rank in groups of its own, each group's shrink in its chunks' keep
word, the rest grown a little more to hold the area; with a hard end each element at the prefix's end popped in
and out, grown, as the distance moved it - a near crown's leaves and a far one's clusters blinked as one walked,
in the view and in the shadows alike); a wood segment thinner than the floor is kept with the
probability of its width over the floor (by its hash) and drawn at the floor - a phone wire's antialiasing, a
twig never a crawling half-pixel. The leaves end earlier than `d1` where the template's mean leaf (a spray's leaf,
a compound leaf, a needle tuft) falls under the preset's pixel floors on the screen (2 on Minimum and Low, 1.6 on
Default, 1.3 on High, 1 on Extreme; `r__shrubs_far_detail` divides them, QA - at two on every preset a small-leaved
bush stood in clusters from ten metres on, their cells a mosaic at 25 m; 0.1-0.25 ms); its clusters draw it from there. A
spray drawn as one quad turned to the eye (the first version) showed all its leaves face on where the near ones
turn every way, and a bush read twice as dense and in flakes until one walked up to it.

**Textures** (`tools/shrubs/textures`: our own procedural sets, no photographs). **Leaves**: 89 layers of 512 px
(BC7, two arrays), one per leaf of a species - an `a` and often a `b` leaf, a spray's card (its stretch of shoot
with its leaves round it, for the far billboards and the shadows), a compound leaf's leaflet (the clusters' cells),
a juniper's sprig - drawn from the species' botany: outline and margin (teeth, double serration, lobes, cordate
bases), venation by its architecture (craspedodromous hazel, the parallel arches of alder buckthorn, the willows'
many fine loops, palmate lobed leaves, the acrodromous mock orange), the petiole, mottling, a few blemishes, bites
and browned margins. `shrub_leaf_d`: the upper side's luminance factor (the impressed veins of the rugose species
slightly darker, the smooth glossy ones' midrib a little lighter), the vein mask, the light through (the lamina's,
not the veins'), the coverage; `shrub_leaf_n`: the upper side's relief (the veins' grooves, a hazel's quilted
fields, rolled margins), the gloss, the underside's luminance (its raised paler veins, hairs, scales). The square
holds the petiole and a margin the generator's element leaves out: per species its length, width and start behind
the element's base (`leaf_fit`, `card_fit`) put it on the element. The near leaf's hull (four rows of a pair) is
fitted to the coverage of the species' a and b leaves together (`build_arrays.py`), so the alpha test cuts little
and the outline is the texture's, not the polygon's; the mips keep the coverage at the test's half (texconv
`--keep-coverage`), so a far leaf neither thins nor swells, and the tests are hard (no dithered edge for the
temporal resolve to haze). A leaf picks a or b by its hash and is mirrored by it; a few in ten have one or two
scalloped notches eaten from the margin by the pixel shader, each its own (the textures carry no holes: one baked
hole on every leaf of a species read as polka dots). **Bark**: 19 types of a 6 cm
tile (lenticels of hazel, alder buckthorn and the cherries, the willows' fissures, elder's corky furrows, the warty
spindle, hawthorn's scales, the shreddy honeysuckle and juniper, the canes' prickles and bloom): the albedo with a
cavity term, the relief, the gloss and where lichen may settle. An axis's tile is the species' tile size on every
axis a tile round and more - `bark_scale`, a hawthorn's plates two and a half times the textures' 6 cm, which at 6
cm read as stucco; one scale over a tree's trunk and its limbs: each axis's own, a whole number round its base, met a
limb's bark with its trunk's at a fifth to half again the size -, a thinner one's base girth (its single tile shrinks
both ways), its real size along the axis, and a whole number of them round every ring of the tapering stem: the pixel
shader takes the turn round
the stem times the count of a segment's first ring and of its last and blends the two along it where the count steps
(the count held from the base squeezed a tapering stem's bark into slivers, a hawthorn's plates into smooth elephant
skin; one count interpolated between the rings seamed and twisted it; the trees' stock patches and junctions take the
very counts of their tube's rings); each axis's pattern turned and shifted its own
way, the tile without its repeat (hex tiling: the plane of tiles cut into equilateral triangles a tile on a side, one
side along the turn, each corner reading the tile at its own offset, the three blended by their weights lifted a
little by the bark's height; half of them mirrored round the stem, the relief's turn with them - a pine's
plates and a black poplar's forks read twice a metre), a slow swell of its value over the stem (a noise of the rest
position, a fifth of a metre
to a metre: one over every joint and seam). The species wears its bark texture in its
own tint (`bark_tint`, linear: a hawthorn's old stems a neutral grey - Commons photos of Crataegus trunks, mean r/g
1.05, b/g 0.84, the texture a warm taupe -, a dog rose's canes green going olive, a cinnamon rose's red-brown) and
contrast (`bark_contrast`: a power of the luminance ratio about the tile's mean - its last mip - the hue kept; the
textures' plates and fissures differ more in hue than in value, and from a metre and a half a hawthorn's trunk read
smooth, a hazel's showed no lenticels: fissured and scaly barks 1.5-2, the smooth ones with lenticels 1.3-1.6, the
canes 1.3-1.4, the ones already strong in value as they are). The current year's growth - the last ten to forty-five
centimetres of an axis, a filling shoot all of it - fades to the young twig's colour and a smoother skin (by level,
a twig was young end to end: a metre of red wire), keeping the species' share of the bark's relief and detail
(`young_relief`, a quarter; a warty spindle's corky warts stand on its green twigs), flushed on its upper, sunlit side
where the species reddens there (`twig_blush`: a dog rose's canes; uniform green read as a garden hose). Dead wood
wears its bark as the layer paints it (nothing bleaches it). Crustose
lichen in soft grey-green blotches of one to four centimetres over a tenth of an old stem three centimetres thick
and more, most near the foot (by a noise of the rest position), half covering the bark and a fifth lighter than it at
most, and a green film of algae
on the foot's side away from the sun (the species' `lichen`; the bark's own lenticels painted pale as lichen had
stitched the stems white). Where
a segment meets the next one of its axis the ring is square to their mean direction and takes the next one's own
base and arclength, so the two tubes share it - no wedge open on the outer side of a bend, no step in the bark -
within `d0`, past it on the stems and branches (their joints' wedges opened pale rings onto the tubes' insides at
thirty metres) and on the twigs within a dozen metres (their joints a bent wire's kinks at arm's length; farther a
twig's is under a pixel; the shadow maps' thinner tubes need none).
The far tiers' bark colour (`c_bark`) is the bark texture's mean in the species' tint.
The trees' leaf, needle and bark layers follow the shrubs' in the same arrays (the shrubs' indices never move):
`TREES_TEXTURES.md`.

**Occlusion.** The procedural foliage bakes its own occlusion into the G-buffer's hemi term (the light inside the
crown below) and the ray traced occlusion of DirectX 12 has no copy of it. A leaf's or a cluster cell's face turned
down sees the ground under the crown, in the crown's own shade, not the sky: its hemi falls to three tenths of an
upward face's (`shrub_face_hemi`; the combine's sky term does not look at the normal) - lit by the whole sky, a
white poplar's felted undersides, all a tall crown shows from below, read white over every tree of it, and the
players took them for birches' white leaves; screen-space occlusion (GTAO, HBAO, the
stock SSAO) takes three tenths of its share on the foliage material codes in the combine (`combine_1.ps`) - with
all of it a crown's inside read up to a third darker on DirectX 11 than on DirectX 12, twice occluded. The blade
grass shares the codes and the rule.

**Shading.** Every tier lights a bush as one volume: the leaf's, the cluster cell's and the blob's shading normal
is partly the crown's (the gradient of the ellipsoid of the bush's bounds, carried in the record) - a third near,
where each leaf's own facing and relief read, half from 15 m - with the leaf texture's relief on top in the leaf's
own frame (a mirrored bush's frame turned back), kept well toward the eye; a leaf shows its upper or its under side by the way it faces the eye, turning over
smoothly as it passes edge-on, a cluster's cells each a leaf of its own facing round the leaves' mean. A far cell of
a few pixels keeps two fifths of its own normal's share, the rest growing in from two pixels to eight (its own
facing's light, a step from leaf to leaf, read as a mosaic of flat facets against a low sun; with none of it a crown's
top went smooth as cotton wool), the part's share's gain on the sunlit side taken back (a unit normal where the leaves'
mean is shorter: lit tops a fifth lighter); the near tier keeps every leaf's own light (there it flattened a willow's
sunlit side at 7 m into a pale pastel mass). A part's cells lie on a grid turned its own way and pick their clump
level pixel by pixel, their edges' footprints from the part's own gradients (a grid square to the screen, its level
chosen per 2x2 quad, read as square bricks). **The light
inside the crown** (`shrub_crown_light`): the G-buffer's occlusion is the sky the element sees past the leaves
(step 10), to the power of the share of the leaf area on now - the leaves out, grown and not fallen (a crown's
optical depth goes with its leaf area: a thinning autumn crown and a spring one open, a bare bush's wood sees the
whole sky) - and 1.2 more (the twigs and branches and the clumping round the shoots, which the leaves alone leave
out; calibrated against the photo pack), over a floor for the light the leaves scatter onto each other: 0.13 for a
mid-green leaf (0.08 left the textured crowns' inside near black, the crown's median 0.27 of the meadow against the
pack's 0.40), going with the leaves' albedo (a pale, silvery or yellow crown is lighter inside, up to twice).
That light went through and off leaves and is their colour: its share of an element's light takes half of the leaf
colour's chroma once more at the same luminance, so a crown's inside is a deep green and not the sky's grey-blue
dimmed; the wood and the fruit inside take the crown's mean leaf colour of the day. The clusters and blobs keep
their leaves' look (calibrated at 40 m over eight species against the near tier forced in the same view: 0.91 of
their leaves' colour for the dark inside between the cells, a juniper's clusters 0.6 - its cells face the eye; each
within 0.82-1.04 of the near luminance, its coverage 0.75-1.13). A part's shading normal is the crown's at its centre
turned out toward its rim as a dome on the crown's surface (production foliage's clump normals): each part lit on its
sun side and shaded on the other as the near leaves' mass is - the same top-lit gradient on every part stacked them
as coins, a whole sphere's turned every part's middle to the eye and dimmed the sunlit tops at noon. Its cells'
leaves lean round the leaves' mean up and out of the crown (the outer leaves turn to the light: a bush seen from the
side shows their upper sides; straight up showed half the cells' undersides, a frosted mint crown), a clump of leaves
less (their mean), the willows' silvery leaves more. A part's light (the sky its leaves see) is pulled toward the
crown's middle (a blob half way, a cluster three tenths: one value per part read as a patchwork of flat discs, the
low ones near black), its innermost occlusion at least a third of the lit ones' (a blob's nearly half), its leaves a
tenth (a blob a fifth) less saturated than the near ones. Their outline swells and dents smoothly at three scales.
**Past the near cascade** (14-22 m) every element darkens by its own shade inside
the crown (`shrub_far_shade`: its sky visibility, which goes with the sun it gets, to half at the darkest): the far
shadow maps' casters are clusters and blobs and lose most of a crown's self-shadow - the same bushes read up to two
fifths lighter at 40 m than at 10 m, pale sage florets among the darker trees; with it, a tenth. The live
greens take 1.08 times their chroma where the blade grass takes 1.3 (1.15 with the textured leaves put the crowns'
saturation over the photos' meadow ratio), without the grass's push toward blue-green (on
a solid crown, over the photos' own chroma, all of it read acid, then the push cool and frosted on the sunlit top).
The leaves write the translucent foliage material codes 27-30 (`da_material_codes.h`).

**Gloss.** The deferred combine scales the sun's specular five times over (`combine_1.ps`), so the G-buffer gloss
of the foliage is a few hundredths: the species' cuticle gloss (0 matte .. 0.4 a cherry's) maps to 0.01-0.04,
halved toward 40 m. A berry's skin 0.025, a hip's 0.03 (satin: at 0.07 its highlight went near white, a plastic
olive), a plum under its waxy bloom 0.015, a raspberry's drupelets 0.02 and matte between them, capsules, nuts, husks
and pods about a hundredth, the stalks and sepals 0.006 (at 0.07-0.09 on every berry, each berry of a bunch had a hot
white highlight and a sunlit bunch read as a white puffball).

**Fruit in the crown's light.** A fruit sits in the crown's light as the leaves round it do: the same crown light
(`shrub_crown_light` with its site's sky visibility) in the G-buffer's hemi, with only a crease where the berries of
a bunch overlap, its colour as the data has it, and the translucent foliage codes - a juicy berry, drupe, pome or
raspberry 0.52, a hip and the dry capsules, nuts, pods and cones 0.30 -, so the combine takes under half of its
screen-space occlusion over the crown's own, the sky light through it and the sun through it from behind (a back-lit
berry glows) as through a blade, and the sun's shadow is looked up as the leaves' is. On the stock material, with its
occlusion taken twice and cut again by its colour and an underside term, a fruit beside lit leaves went black: an
elder's green bunch at a tenth of its leaves, a raspberry at a quarter of its colour.

| Preset | `d0` | `d1` | `d2` | `R` | pixel floor | sun cascades |
| --- | --- | --- | --- | --- | --- | --- |
| Minimum | 4 | 14 | 40 | 150 | 3 px | 1 |
| Low | 5 | 17 | 50 | 180 | 2.5 px | 1 |
| Default | 6 | 21 | 60 | 210 | 2 px | 2 |
| High | 8 | 26 | 75 | 250 | 1.75 px | 2 |
| Extreme | 10 | 32 | 90 | 300 | 1.5 px | 3 |

**Shadows.** Sun cascade 0 takes the wood and the leaves (at its texel), cascade 1 the main wood and the clusters,
cascade 2 the blobs (the quads turned to the sun; the old cascades take cascade 0 near and the farthest one far).
The shadowed local lights within 40 m of the camera (flashlights, lamps, fires; the lights' frusta cull the
bushes) take their bushes' main wood and clusters, as the stock bushes cast into them. The rain's occlusion map
takes the crowns' blobs from above (the ground under a leafy bush stays drier; a bare one shelters little).
**Ray tracing** (DirectX 12, the RTX options) sees no procedural vegetation, the same as the blade grass: the
reflections' screen-first march takes the shrubs the frame shows as they sway, the ray tracing scene has no
copy of them; RT shadows leave foliage to the cascades anyway.

**Wind.** Per bush, on the CPU, the wind at its root - the gust tongue's strength and its local turn, the pressure
and the lean (`da_wind_core.h`, the shaders' own maths) - now and a frame back, into the bush's instance record
(every vertex evaluated the gust field's noise twice before). Every vertex bends with its chain of axes from the
stem out (up to four): each axis turns rigidly about its base by its own angle - its flex times the wind's
pressure at the bush, a downwind lean, the **gust passing its base now** (`da_wind_turb`, `da_wind_core.h`: eddies of
8 and 4 m ridden downwind on the gust field's scroll, so a stem and its shoots bow together and a gust runs through
the crown) and a small ring at the axis's own frequency while the gust shakes it (0.1 / sqrt(freq) of the push: a
stem's a tenth, a twig's a few hundredths), the motors (bodies, shots, blasts) pushing it from its base with its
length as the lever - a cantilever's deflection at a share of its
length is its tip angle times the square root of the share. (Swung by `da_sway` at their own frequencies as the
whole motion, the twigs went back and forth three to fourteen times a second in any breeze - 40 % of a birch crown's
motion over 4-8 Hz at 7 m - and the crowns read as twitching; the grafted wood of the trees shares this bend.)
Leaves flutter about their petioles on top (the
species' flutter, the sheltered inner leaves less) in **eddies carried downwind** (frozen
turbulence, at 1.5-5.5 m/s by the wind's strength): two analytic waves give each place its eddy's strength and a
phase field coherent over a hand's span, so neighbouring leaves move together, a gust's front runs through the
bush, and the shadows the leaves cast move with them - the shadow passes run the same flutter (a random phase per
leaf made every bush's shadow shimmer on the ground). The clock is a double wrapped every 64 s, every oscillator's
frequency a whole number of cycles in it (a float count of seconds lost its milliseconds over a long session and
the leaves jittered). The previous frame's wind gives the motion vectors.

**Seasons** (on the GPU, per element): each leaf has its own rank (when it unfolds) and fall order (lit outer
leaves first); the species' keys at the day (`leaf_out`, `leaf_grow`, `autumn`, `fallen`) decide every leaf -
unfolded once the leaf-out passes its rank and grown over its first weeks, coloured by its own place in the
autumn wave (colour A or B by its hash and light, the margin first for hazel, browning before the fall), gone once
the fall passes its order (shrinking over a sliver of the order, never popping). The evergreens keep their leaves,
lilac falls green. Flowers go bud, open and over; fruits set, ripen and shrivel or stay into the winter
(`fruit_winter`); catkins form in late summer, wait closed through the winter, open and are shed
(`catkin_winter`). Fresh snow settles on the branches and on the upward faces of the leaves still on; on the
branches a line stays all winter, thinner as the snow ages (`L.snow` = cover x (fresh + 0.75 settled + 0.45 old):
at a fifth for old snow the crowns stood bare through most of January). The wood's pixel shader draws it as a crisp
line on each stem's own top - the shading normal within some 55 degrees of the highest a normal round the stem
points (by its axis), on stems up to some 65 degrees steep; against the world's up alone only the near-level twigs
took any, and a stock patch's interpolated normal faces in (its light comes from its projections'), so the line is
the shading normal's. The pixel shaders reach the world through `da_shrub_up` (the world's up in view space, set
per draw): `m_V` reads zero in these passes, and the snow line and the leaves' downward hemi below drew nothing.

**Fruits.** Ripeness runs 0 .. 0.7 while a fruit grows (full size by 0.6) and 0.7 .. 1 while it colours: a fruit
stays green until its last weeks (a third of its time, two weeks at least - the `ripe` key's 0.7 point), and the
berries of a bunch colour one by one. A site flowers or fruits only while its species' share is above zero (an
early bush's lowest-ranked sites stand below a zero share and flowered out of season). A bloom element is the larger
of the species' flower and fruit (`bloom_size`, `fruit_size`); each draws at its own size, `fruit_size` being the
fruit group's own outline without its stalks: a single fruit's length, a pair's or a corymb's breadth, a raceme's
or a panicle's length (drawn at the flower's, a dog rose's hip was three times a leaflet).

**How many fruits.** `fruits` is a group's layout - the most it holds -, not what it carries: the season key
`fruit_keep` is the share of a group's fruits there today (the fruits its flowers set, then what the birds leave
through the winter), `crop_spread` how far one bush's crop falls short of the best one's (a hash of the bush picks
1 .. 1 - spread), and `shrub_fruit_keep` multiplies the two. A group's pixel shader drops each fruit of its layout
whose hash is past that share - its place in the group left empty, the others' sizes kept -; a site of one fruit (a
hip, a drupe, a pod) loses it whole, its share of the sites dropping instead; the clusters' and blobs' fruit cells
take the same share. Snow lies on a flower's or a catkin's upper side, never on a fruit group or a pod (their quad
turns to the eye: from above every one of them read as snowed over, a hawthorn's haws pink-white). Without the key a
species keeps all it sets; only the common hawthorn has its own (Commons pack of 16.10.2026, 36 winter, 46 autumn and
40 summer photos of *Crataegus monogyna*): a corymb ripens half of its eight haws, a quarter of the corymbs none
(`fruit` 0.75), one bush laden and the next with a few clusters (`crop_spread` 0.7), and the thrushes leave 70 % by
mid-December, 45 % by late January, a fifth by March, the frosts darkening them maroon by January (`ripe` 1.85; with
every one of its corymbs full, every twig of a January hawthorn was red).

A **fruit group** is laid out in three dimensions by the inflorescence it came from (`bloom_shape`) and drawn whole
by the pixel shader, a layout of `fruits` of them (up to 64) and the share it carries (below), facing the camera
with its depth toward the eye: a
corymb's flat-topped disc on Vogel's spiral (guelder rose, hawthorn, alder buckthorn); an elder's cyme, five rays
each ending in a small dome of berries, turning over as they ripen (`umbel`); a raceme's or a berry panicle's
fruits round their axis by the golden angle, the biggest at the base (bird cherry, serviceberry, currants, red
elder) and a false indigo's dense spike of small pods (8 mm, a twelfth of the spike's length) standing; a lilac's
pyramid of beaked capsules standing out and up; a spindle's one to a few capsules (`fruits` at most, five at most;
most cymes one to three) on short stalks forking apart from the end of a long thin stalk hanging from the twig, 1.3
times the group's breadth; one hip, plum, raspberry or gooseberry on its stalk; a fly honeysuckle's pair joined at the base; a
hazel's one to four nuts (`fruits` at most) on a short common stalk, pointing out and pressed together; a sea
buckthorn's spur crowded with stalkless berries. Groups hang lower as they fill and ripen; a single fruit or pair
(and a broom's pod) ripens on its own day round its bush's, so a raspberry cane in July has green, turning and ripe
berries. Every fruit is a lit ellipsoid of its own size (a fifth either way) on its own thin stalk from the group's
peduncle, rachis or rays; the one nearest the eye covers the ones behind, which take a soft shade near the outlines
of those in front (in their creases only: the crown's light is the leaves'). A fruit's diameter follows
its group: a corymb's about 0.6 x `fruit_size` / sqrt(`fruits`), an elder's 0.5 x, a raceme's 0.8 x `fruit_size`
/ `fruits`, a berry panicle's 0.7 x `fruit_size` / sqrt(`fruits`), a spindle's capsule 0.4 x `fruit_size`, a
pair's berries half of it, a hazelnut 0.45 x `fruit_size` long. The shapes: a hip a satin ovoid, the dark scar of
its disc at its tip inside its five broad lanceolate sepals, bent back over its shoulders (out over the rim of the
tip, then back along its sides) and green while it grows, withering to short brown stubs as it ripens; a raspberry's
or a dewberry's drupelets (`aggregate`), the colour's mean kept; a spindle's four-lobed capsule, split once ripe to
show its seeds' orange arils; a lilac's oblong beaked capsule splitting in two, a mock orange's in four; a hazelnut
in its husk, a loose leafy cup wider than the nut and as long, its rim cut into eight deep ragged teeth that stand out
past the nut's sides, the nut showing above it and between them (the husk the crown's green, browning after the nut
ripens; the nut a glossy brown then); a berry with its calyx's dark speck. A fruit under a pixel grows to one, its cover cut by the area it gained, so a group
keeps its own outline at any distance (a group's mean cover over a disc in its place read as a dotted slab). Every
part's rim - a fruit's, a pod's, a flower's, a catkin's - is cut at half its cover once the part is about a pixel and
a half across, a hard edge the jittered frames antialias, and against interleaved gradient noise only under that; a
stalk or a sepal is cut at its true width, kept where it crosses the pixel's centre (the jittered frames make a thin
one a faint line). Its ramp is the pixel's footprint across the rim (the length of the gradient of the distance to
it). fwidth's sum made the ramps about two pixels wide, and the dithered fringe on parts of several pixels was spread
into hair by the upscalers: the spindles' capsules read as moss balls, the drupes as pompoms, an elder's bunch of
two-to-three-pixel berries had a dithered rim. A fruit's surface detail
(a capsule's lobes and arils, the drupelets, a pod's seeds) fades to its mean under about eight pixels across. The
pixel shader tests every fruit and stalk of a group at each pixel inside the group's hull, so a group's cost goes
with its count: on a GeForce RTX 3070 Laptop an elder's cyme of 40 costs about ten times the old lattice's, a
guelder rose's corymb of 18 about four times, a false indigo's spike of 40 pods about as much as the elder, a hip
with its sepals about twice the old single fruit. A broom's **pod** stands out from its site, one to a site, on a
short stalk, as long as `fruit_size`: flat, six times as long as wide, a little curved, matte green while it grows, a
dull shine once ripe and brown-black (wider pods in pairs with pale hairy margins read as moths); a **cone** an egg of scales in crossing spirals, matte; a willow's **cotton** a soft ragged tuft that lets
the light through as petals do. A guelder rose's flowers are a **lacecap** (`bloom_shape = lacecap`): a flat disc
of small cream fertile florets ringed by large flat white sterile flowers.

## Console

- `r__shrubs 0|1` (default 1): the procedural shrubs or the stock bushes, live.
- `r__shrubs_stats`: the preset's distances, the bushes, templates and memory, this frame's tiers, elements and
  chunks per draw and pass (view, cascades, rain), the shadowed local lights that cast shrubs (of those offered by
  both of the frame's light packages, the normal and the occlusion-pending one, and those within 40 m), the sun
  cascades that asked for the bushes before their frame was built (always 0; anything else is a broken order), the
  bushes per species.
- `r__shrubs_species_pin <name|off>` (QA): every bush grows that species (the templates rebuilt).
- `r__shrubs_probe` (QA): the ten bushes nearest the camera - species, distance, position, height, template (its
  height, class, variant), tier, dead; `r__shrubs_probe ray` the ten the view's ray passes through, nearest first.
- `r__shrubs_force_tier -1..3` (QA): every bush in one tier (0 near, 1 leaves, 2 clusters, 3 blobs), its pixel
  floor still its distance's; -1 as drawn.
- `r__shrubs_lod_scale <0.05..20>` (QA, default 1): scales the blade distance (the tiers' calibration).
- `r__shrubs_cut 0|1` (QA, default 1): 0 draws the shrubs' view as the CPU chose it, without the GPU cut - an A/B.
- `r__dbg_shrubs <mask>` (QA): hides draws - 1 wood, 2 every leaf draw, 4 blooms, 8 clusters, 16 blobs, 32 the
  shadow and rain passes, 64 the blades, 128 the cut quads, 256 the sprays.

The level log prints `* [shrubs] N bushes of M placements, T templates, E elements (K KB), ms`; `r__gpu_log` adds
`shrubs` (the G-buffer draws and their cut, GPU), `shrubs_cpu` (the frame's bush choice, chunk builds and uploads, CPU)
and `veg_wait` (the time the render thread waits for the vegetation's frame pick, both populations: 0.65-1.4 ms on
Cordon at Extreme; `da_veg.cpp da_veg_frame_prepare`).

## Cost

Measured on the QA install (Cordon, July noon, a light breeze; an RTX 3070 Laptop GPU, the player's settings:
Extreme, DLSS Balanced, 1484x834 to 2560x1440; 3 s windows, twice in turn; the procedural shrubs against the stock
bushes and against none):

| View | `shrubs` G-buffer, DirectX 12 | DirectX 11 | GPU frame against no shrubs, DirectX 12 / 11 |
| --- | --- | --- | --- |
| a bush 4 m away | 1.23 ms | 1.24-1.26 ms | +2.7-2.8 / +1.9-2.4 ms |
| a bush group from 23 m | 0.73 ms | 0.76 ms | +1.6-1.7 / +1.2-1.4 ms |
| the western stand from 30 m | 1.14-1.15 ms | 1.11-1.14 ms | +2.8-2.9 / +1.8-2.1 ms |
| the stand from 95 m, a willow at arm's length | 2.59-2.61 ms | 2.48-2.52 ms | +4.4-4.5 / +3.3-3.5 ms |

The bend pass (`shrubs_bend` of `r__gpu_log`) takes 0.09-0.10 ms with a bush near and 0.16-0.17 ms over the stands.
By kind (DirectX 12, each left out in turn): a bush 4 m away - the leaves 0.52 ms, the wood 0.28-0.33, the fruit
0.20-0.25 (an elder's or a guelder rose's every berry shaded as its own lit ellipsoid with its stalk); the stand from
30 m - the wood 0.30-0.36, the leaves 0.28, the clusters 0.13-0.29, the fruit 0.10-0.18; from 95 m the near willow's
wood 0.6 ms, the clusters and the blobs 0.07-0.08 each. 08.10 (the bark's tile per ring, its tint and contrast, the
far tiers' dithered clumps and dome normals, the wider blobs, the crossed needle quads, the hazel husks) the views
cost 5-15 % more than the figures before them (the GPU clocks of that boot ran the whole frame about 5 % slower too);
the blobs at the species' leaf size and up to 64 a bush, the leaves cut out of a mip of sixteen texels at the coarsest
and the far cells' turned grids, another 0.05-0.15 ms.

The textures (08.10: the leaves' and the bark's sets, the hulls, the clusters' cells as leaves, the eight-sided near
stems) first cost half again as much; drawing near to far, the bend pass, the culling of the elements out of the
view and the hulls' earlier switch to quads won it back to within a few tenths of the untextured shrubs (before: 1.00-1.06,
0.31, 0.59-0.60 and 1.57-1.65 ms on DirectX 12). The frame against no shrubs includes the shrubs' sun cascades.
Earlier figures (the untextured shrubs, 07.10):
At 30 m the clusters take 0.24 ms, the wood 0.11, the leaves 0.08 (DirectX 11, each draw left out in turn); the
light inside the crown and the leaves' clumping added 0.05-0.1 ms. `shrubs_cpu` 0.30-0.40 ms on Extreme (0.13 ms
Minimum). On the DirectX 12 rig the frame rate stays within the runs' noise (the frame is the CPU's); on DirectX 11
the views lose 2-5 frames of 60-70. By preset (DirectX 12, the stand from 30 m, before the crown light): `shrubs`
0.31 ms Minimum, 0.38 Low, 0.41 Default, 0.53 High, 0.55 Extreme. A view's own timer swings by a millisecond between
runs where the auto offloads switch the grass and the trees' paths (the 30 m stand read 0.6 and 1.6-1.9 ms), so
every figure here is two windows of a run, repeated. Cordon grows 2,078 bushes on 121 templates: 1.8 million
elements, 64 MB of element buffers, built in about 1.1 s on the task pool at level load (the light inside the
crowns included); with the textured generation (lumpier crowns, the spurs topping up every broadleaf, the dead
stubs) 2.9 million elements, 103 MB, built in 1.6 s.

## The data file

`[shrub_species_N]`: `name`, `height` and `width` (share of the height) ranges, `classes`, `dead`, `spread_days`;
the architecture keys of `da_shrub_gen.h` (`envelope`, `stems`, `l1_density`, ..., `fill_*`, `leaf_kind`,
`leaflets`, `spray_len`, `max_leaves`); the drawing keys (`leaf_outline`, `leaflet_ratio`, `translucency`,
`gloss`, `flutter`, `fold`, `underside`, `jitter`, `bloom_shape` (none, single, corymb, umbel, raceme, panicle,
pea, cone, lacecap), `petals`, `fruit_shape` (none, berry, hip, drupe, pome, nut, capsule, cone, pod, cotton,
aggregate), `fruits`,
`catkin_shape`, `bark`, `needle_density`, `evergreen`, `lobes`, `autumn_mix`); the texture keys (`leaf_tex`: the
leaf layers a, b, card and leaflet; `leaf_hull`: the near leaf's hull; `bark_tex`; `lichen`; `leaf_fit` and
`card_fit`: the squares over the elements and the clusters' leaf coverage - all written from the texture
manifests, `tools/shrubs/textures`; `young_relief`, `bark_tint`, `bark_contrast`, `bark_scale`: the bark as the
species wears it; `prickle_colour`: linear rgb; `twig_blush`: linear rgb and its share; `far_value`, `far_chroma`: the far
parts' value and chroma on their open, light top against the near leaves, 1 unless measured off - a dog rose's 0.8
and 0.7; `crop_spread`: how far one bush's crop of fruit falls short of the best one's); the season keys as
`day:value` lists (`leaf_out`, `leaf_grow`, `autumn`, `fallen`, `bloom`, `bloom_stage`, `fruit`, `ripe`,
`catkin`, `catkin_grow`, `autumn_spread`, `fruit_keep`) and the colours as `day:#rrggbb` lists (`c_young`,
`c_summer`, `c_autumn_a`, `c_autumn_b`, `c_dead`, `c_under`, `c_bark`, `c_twig`, `c_petal`, `c_centre`,
`c_fruit_unripe`, `c_fruit_ripe`, `c_catkin`). `[shrub_community_N]` (`name`, `species = id:share, ...`),
`[shrub_grass_community]`, `[shrub_class_community]`, `[shrub_policy]` (`max_top = CLASS:m, ...`, `min_scale`,
`spacing`, `dry_dead`, `red_community`), `[shrub_kinds]` (`key = CLASS[, dry][, red][, x:z:r:top ...]`),
`[shrub_families]` (`signature = key:x0:y0:z0:x1:y1:z1:area, ...`, the box in mm). A mod adds a species, binds its
own bush models to a class or moves a community by data alone; the mechanism names no model. Edit
`tools/shrubs/make_shrubs.py`, not the ltx.
