# Upgraded level trees (1.6.0)

The trees of the Zone are no longer the stock card models. Every placed tree of a kind the data knows stands where it
stood, its exact stock placement kept - the rotation, the uneven scale, the lean and the root -, but it is rebuilt: its
stock trunk and modelled limbs become tubes on the rings the skeleton builder took from its own mesh (each ring's
cross-section a Fourier profile fitted to the stock wood, the stock triangles kept where no tube fits them: forks,
gnarled bases, cut ends), and new branches, twigs and shoots of a real ChEZ species are grafted on that wood inside the
stock crown's own volume - along the stock card branches where the stock crown had them -, carrying real leaves,
sprays, needles, flowers, catkins, fruit and cones that follow the species' year day by day. Far out a tree is its own
stock billboard, its eight views drawn by the engine from the tree itself for the day (no billboard atlas ships). The
pipeline follows the production vegetation tools and the
shipped open-world games: a stock skeleton with procedural growth grafted on it (the SpeedTree workflow of a modelled
trunk with generated branches), the pipe model's cross-sections down every fork (Shinozaki; Lindenmayer), the
rule-based branching of Weber and Penn, the leafy shoots filling the crown's lit shell, the light inside the crown
baked per element, foliage tiers by the size of the species' own leaf on the screen (leaf hulls, quads, the quads
thinned to a leaf floor, clusters, blobs, then the billboard), crossfaded by element hash, the wood cut on the CPU by
the shaders' own wire rules and the view's wood and near leaves each on the GPU by its own place, depth and distance
(the GPU-driven culling of the shipped engines), and a skeletal wind on the stock tree's own root
oscillator, the same in the view and in every shadow map. DirectX 11 (R4, FXC SM5) and DirectX 12 (R5, DXC SM 6.x) run
the same code.

Data: `configs/dead_air_x64_trees.ltx` (written by `tools/trees/make_trees.py`, the generated tables in
`TREE_DATA.md`); the skeletons `meshes/da/trees/<skel>.dtsk` (written by `tools/trees/skeleton/build.py`, the format
in `tools/trees/FORMAT.md`); the leaf and bark layers in the shrubs' texture arrays (`tools/shrubs/textures`); the
season day `D`: `SEASONS.md`. Sources: `src/Layers/xrRender/da_veg.{h,cpp}` and `da_veg_internal.h` (the vegetation
core both populations share: the frame's buffers, the latch, the culling grid, the chunk emission, the bend pass, the
draws of every pass), `da_trees.{h,cpp}` (the trees' population: the data, the placements, the templates, the tiers,
the billboards), `da_shrub_gen.{h,cpp}` (`read_skeleton`, `generate_on_skeleton`: the generator, no engine types),
`da_trees_bake.{h,cpp}` (the billboards' bake: the trees' atlas and the stock path's `level_lods`); shaders
`r3/da_shrub_*` (shared with the shrubs, the wide element layout by the instance's tree flag), `r3/da_tree_stock*` (the
stock tubes), `r3/da_tree_patch*` (the stock triangles kept), `r3/da_tree_cut*` (the view's GPU cut),
`r3/da_shrub_wood_far.*` and the `_far` vertex shaders (the far wood), `r3/da_tree_bake*` (the bake), `r3/da_tree_bb_copy.*`
(the stock cells' copy where the bake cannot run), the stock `lod.vs` and `r3/da_tree_lod.ps` (`lod.ps` writing the
foliage material: the billboards). Hooks: the shrubs' (`r2_loader.cpp`,
`r2_R_calculate.cpp`, `r2_R_render.cpp`, `render_phase_sun.cpp`, `render_phase_sun_old.cpp`, `r2_R_lights.cpp`,
`r3_R_rain.cpp`, `da_rt_scene.cpp`). Tools: `tools/shrubs/preview/shrub_preview --skel` (one tree of a species on a
skeleton, written as OBJ), `tools/trees/`.

## The vegetation core

The shrubs (`SHRUBS.md`) and the trees are two populations of one module. They share the frame's instance records (96
bytes: the 3x4 transform, the species row, the flags - dead, tree, the Red Forest's red needles -, the phenology offset,
a young tree's spring lead, the template's first axis, hemi and sun, the scale, the wind at the root now and a frame
back, then a tree's trunk push now and a frame back, its height and climbable top, the template's first aux slot and
its youth), the chunks (64 elements of one draw each), the bend records, one table of 128 species rows (the shrubs'
0..47, the trees' 48..127: 19 profile and 16 season rows each), one latch, one grid walk with the frustum and the
occlusion test, one chunk emission and the same draws in every pass. Each population has its own element, axis and
aux buffers and its own tier rules; the frame lays out every population's view chunks first, then each population's
shadow passes in one run, so the bend pass is one dispatch per run with that population's buffers. A tree's elements
take the wide layout (an axis index of 24 bits, the leaves' directions at twelve bits a coordinate); its fixed axes
(the stock wood) end the chain of bends; its chunks of stock wood, patches and blobs are rigid (no bend record: the
trunk's sway alone). The records are capped at 16384 a frame for both populations; a tree past its billboard distance
needs none.

## Which models are replaced

A stock tree is a multiple-use model under an `FLOD` node of a sector hierarchy, its parts `MT_TREE_ST`/`MT_TREE_PM`
visuals - the trunk (`flora\trunk_wave`), the crown cards (`flora\leaf_wave`). The core's census keys every such FLOD
once a level (the shrubs' content key, `SHRUBS.md`); the shrubs take theirs first (their size policy leaves the
oversize ones), the trees the rest that `[tree_kinds]` names `TREE` or `YOUNG` and whose skeleton (`skel=`) reads:

- **TREE**: a standing tree, alive, dead, broken, burnt or anomaly-sculpted, on its stock skeleton. **YOUNG**: an
  oversize stock bush grown whole as a young tree (a procedural skeleton, `skel=p<kind>`). **STOCK** (logs, windfalls,
  stumps, debris) and any model the data does not name - a mod's tree - stay stock. Another compile of a known model
  is found by its signature in `[tree_families]`.
- **Placement**: the stock FLOD's first part's transform as it is, the element sizes scaled by the cube root of its
  determinant; a model under `min_scale` of its size stays stock. Past `max_tilt_graft` degrees of lean the grafted
  growth takes its tilt bucket's vertical (fifteen-degree rings by eight headings: a leaning tree's new shoots still
  grow up toward the light); the stock wood keeps the stock lean.
- **Out of every pass at no cost per frame.** While the trees stand (`r__trees 1`) a replaced tree's FLOD leaves its
  parent hierarchy whole at level load: the views, the cascades, the lights, the rain map, the LOD billboards, the
  GPU-driven registry and the ray tracing scene never meet it. Its parts stay alive: they are the trees' wind roots
  (each tree sways on its stock root's oscillator, `FTreeVisual::wind_state`) and the ray tracing scene's stand-ins
  (`da_veg_replaced_visuals`; `da_veg_stand_in` tells a stand-in's population, species row, template and whether it
  stands dead). `r__trees 0` puts them back live (the registry and the ray tracing scene rebuilt, the temporal history
  reset).
- **The ray tracing stand-ins follow the season** (DirectX 12): a living tree's or bush's alpha-tested stand-in
  instance carries its species' row (`kInstSeason`, the row + 1 in its flags' bits 8..15) and the frame's view the
  vegetation tables' bindless slot, their layout and the snow (`da_veg_rt_tables`). The traced foliage keeps a share
  of its card's texels by a stable hash - the leaves on, not fallen, grown (`rt_veg_leaves`: bare in winter, thin as
  they unfold) - in the species' mean leaf colour now over the card's own mean (`rt_veg_colour`, kept in step with
  `shrub_leaf_colour_mean`), the snow on the tree's tops; a dead one's stand-in stays as it is, the conifers' needles
  all year. A stand-in's trunk occludes the ray traced ambient occlusion from outside only (its front faces,
  `rt_ao.hlsl`): the upgraded tree's own wood lies a few centimetres inside it - the tubes within the skeleton's
  tolerance, the patches' borders laid onto them -, and its rays leave the stand-in through its back faces (they
  blackened the wood under the stock's skin near the root). DirectX 11 has no ray tracing scene and no change.
- **Gameplay is untouched.** The level's collision, the AI graph and the cover points keep the stock tree. The tubes
  are the stock wood within the skeleton builder's tolerance (2 cm or 5 % of the radius; the visible wood beside the
  climbable trunk collision within 3 cm of it, `tools/trees/FORMAT.md`), the patches the stock triangles. The grafted
  wood has no collision: it keeps its radius and 2 cm clear of the stock wood, and wood eight centimetres thick and
  more grows only where a player cannot reach it - not within two metres of the ground, nor within two metres above
  and a metre and a half round a stock limb one can stand on.

## Species, communities, youth

A placement's species is its kind's own (`species=`), else one of its community's by share - the kind's
`community=`, the Red Forest's (`red_community`, default `red_forest`) for a red kind, the grass community under the
root (`[tree_grass_community]`), the class's (`[tree_class_community]`) -, a `conifer` kind among the community's
conifers and any other among its broadleaves, by the root's hash. At most `kinds_per_level_species` species of one
kind stand on a level (the rest take the kind's most frequent of the same look). A share `dead` of each species
stands dead. A living generic broadleaf kind whose stock trunk is thick for its height (0.6 m and more at breast
height in the game and over 0.045 of its height: the stock giants stand 1.2-3.3 m thick, a birch's trunk rarely
passes a fortieth of its height) takes its community's thick-trunked species instead - the oaks, the poplars, the
willows, the alder: the one of them alone where it holds four fifths of them, else `<community>_thick`
(`tools/trees/make_trees.py thick_pick`).

- **Dead** (`dead`): bare, the grafted twigs grey and weathered, no leaves, flowers or fruit. **The Red Forest**
  (`dead, red`): the pines killed in 1986 keep their dead needles all year in the species' `c_dead` (rust-red), no
  flush, no fall, the snow on them in winter.
- **Youth** `y` (0 a grown tree .. 1 a sapling): 1 for a `YOUNG` kind or a `sapling` kind, else
  `saturate((young_h1 - H) / (young_h1 - young_h0))` of the tree's height H above its root (`[tree_policy]`
  `young_h0 = 4`, `young_h1 = 10` m). It does three things:
  - **Phenological escape**: a young tree's buds break and its leaves grow ahead of the old trees' - up to
    `young_lead` days (`[tree_policy]`, 8), each tree its own 0.6 .. 1 of `young_lead * y` by its hash (no stand
    steps at once). The season table carries the leaf out and the leaf growth `young_lead` days on (season rows 3 w
    and 4 w); a tree's lead blends them in for its spring only - its autumn keeps the old trees' dates.
  - **Marcescence** (`marcescent`, the species' share: a young oak 0.6, red oak 0.5, hornbeam 0.4): a tree keeps
    `marcescent * lerp(0.08, 1, y)` of its leaves withered through the winter - its last to fall by its autumn order
    (on an old tree the inner, lower crown, where that order leaves them) -, from their fall to its next bud break,
    in the dead colour; the far tiers keep the same share.
  - **The juvenile phase**: a tree bears `1 - y` of its flowers, fruit and cones (a sapling none).

## The skeleton and the generator

`read_skeleton` reads `tools/trees/FORMAT.md` version 1: the header and the crown block (the grid, its origin and
cell, the count of the stock bark's patch triangles before the cut ends' caps), the axes, the rings (76 bytes; the
contract's 72-byte record, without `fit_err`, is read too), the patch vertices and triangles, the cards and the crown
grid; any other size is refused (the kind stands stock, with a log line). `generate_on_skeleton` grows one tree:

- **The stock wood**: every axis but the card branches' fixed where the stock has it, its rings subdivided where the
  spine turns more than six degrees or runs over a metre (eight at most) by the file's own reconstruction - the
  centripetal Catmull-Rom spine through the centroids, the tangent and the reference normal lerped, the radius the
  monotone cubic Hermite over the arclengths, the profiles lerped -, three levels of detail of its rings (every ring;
  every 12 degrees, 3 m or a quarter of the radius; every 25 degrees, 8 m or the same), the stock triangles kept and
  skinned to their nearest axis (their turn about it and their distance from it), the top of the climbable wood (an
  axis a quarter on the trunk collision and more).
- **The crown field**: the skeleton's occupancy grid (an opaque card's cell `1 - exp(-area / cell^2)`, the closing's
  cells at least 64/255, the enclosed ones 40/255 - room for branches); the crown is its cells from 0.12 up, their
  chamfer depth from the nearest empty cell, the projections from above and the side; a kind without cards (a bare
  dead tree) takes an ellipsoid in the upper four fifths of its box. Its islands go (the 6-connected parts under 2 %
  of the crown's cells, or under 15 % and more than 1.5 m from the rest): a stray card's lone cell grew a shoot at
  the end of a long bare wire. Apical dominance (`apex_cone`, `apex_leader`): the field's top share narrowed to a cone
  from the apex - its radius linear from the crown's own where the cone starts to the leader's at the top - and its
  top metres to the leader's short whorls (15 cm round it, 15 more a metre down), so the branches growing in it shorten
  toward the top: a spruce's spire (`apex_cone = 0.33`, `apex_leader = 1.25`), not the stock cards' rounded head.
- **The grafted levels** (`graft_levels`, the species' `l1_..l3_` keys): the first along the stock axes and along the
  stock card branches' axes (`stock_card_follow`: how closely), the next on them; every child's cross-section from
  the pipe model (exponent 2.15, from the tip down, the stock children's first), the wood kept inside the crown field
  and off the stock wood, thin where a player could reach it (above). A last-level shoot is its leafy growth and a
  short bare run at most (`shoot_len + max(8 leaf_spacing, 0.25 m)`); once the leaves are on, a shoot whose first
  leaf stands past `max(8 leaf_spacing, 0.25 m, 0.7 of it)` and a twig whose first child stands past
  `max(3 / l3_density, 0.6 m, 0.8 of it)` go with what they carry (long thin wires to a lone tuft).
- **Bare stock limbs**: a stock limb whose leaves end a metre and a quarter of its length short of its tip (or that
  carries none) - a conifer's self-pruned limbs below its crown - is dead from its last leaf on (grey, the stock
  element's level bit `0x80` and its patches' weathered bit), with short dead stubs every 0.2-0.45 m; a broadleaf's
  whose tip comes within 1.5 m of the crown field grows short leafy twigs on its outer third instead.
- **The leaves** by the species' LAI over the crown's larger projection (from above, or three fifths of the side's:
  a narrow crown is seen from the side), all of it - `crown_fill` places the shoots (clumps and gaps), it does not
  take leaves off -, the fill shoots in the field's lit shell, the spurs along the older wood; their count by the
  level's budget (below). Then the flowers, catkins, fruit and cones (`cone_share`, `cone_len` or `cone_size`,
  `cone_droop`, where the bloom keys give no flowers), the light inside the crown on a grid of about half a metre (64
  cells a side at most), the mechanics (the stock axes no bend of their own), the clusters (a metre at most) and the
  blobs (64 to 512 by the leaf area). The autumn's order: the height (the top first) and the shade
  (`autumn_order`'s two weights; `autumn_inside` 1 the inner leaves first, 0 the outer); the leaf-out and the fall
  orders are then each leaf's quantile by leaf area, so `leaf_out` and `fallen` are the shares of the leaves the crown
  shows out and down (a birch's raw fall order spanned 0.30 to 0.86: a third down showed three per cent gone).
- **The stock patches tessellated**: the stock's coarse facets (a hexagonal trunk thicker than ~0.45 m fits no
  k <= 3 profile within the tolerance and stays stock triangles: 68 % of the bark's area) are rounded by Phong
  tessellation from the stock normals (Boubekeur and Alexa 2008): every patch triangle split into 4^L by halving its
  edges, L 2..4 by the bark's edge length (its 90th percentile over 15 cm), the template's triangles 64k at the most
  and one L for all; each new point lifted toward its places' tangent planes by alpha - a bark triangle's own keeps
  its bulge within 3 cm on climbable wood and min(5 % r, 4 cm) elsewhere, a place takes the least of its triangles', a
  cut-end cap's none (the caps stay flat, the rim on them) -, the stock uvs and normals interpolated, the new vertices
  skinned to their axes again. The places are welded to a millimetre: a stock soup's copies of one place (its uv
  seams, a crease's two sides) stand as one and lift along their normals' mean, so an edge two triangles share by place
  alone splits alike on both and no crack opens along it. The tessellated triangles stand where the camera is within
  3.5 m of the trunk's lower three fifths (its flare and forks; `near_patches`: at touching distance its 4 cm of bulge
  are ten pixels of a silhouette), the stock's own past it (`Template::patch_far`; the white poplar's fork and foot read
  the same from 2 m, `r__dbg_trees 131072`): by the hull tier a big crown drew its tessellated patches with its trunk
  9 m off - tens of thousands of triangles a tree, each corner a vertex shader of its own (a patch's corners share
  nothing), a millisecond a tree. The lift eases in from 3.5 to 1.75 m (`kTessFar`, `kTessNear`): each tessellated
  vertex keeps its stock facet (the corners it rose from and their weights), the pack its lift over the facet's plane
  and the facet's normal there (the third sidecar), and the chunk's keep word's upper half the share eased out - at
  3.5 m the near set lies on the stock's facets, where the far set takes over unseen (switched there at once, an
  angular trunk turned smooth a step from it).
- **The patches' borders laid onto the tubes** (`tree_patch_feather`): a patch keeps whole each stock triangle the
  refit tube misses anywhere by more than the tolerance, so where its border meets the tube the stock stood off it by
  up to that (a step, a ledge under a fork's limb) and a long sliver of a trunk's mesh stood out along it (a fin).
  Each place of the bark patches goes along its ray from the nearest tube's axis toward 4 mm under that tube's profile
  by 1 - m: m 0 on an open border of the patches over a tube (off it by twice the tolerance and a centimetre at the
  most), easing to 1 a blend's length in along the edges (60 % of the ring's radius, 4 .. 25 cm), and 0 wherever the
  stock keeps within half the tolerance of the tube, easing to 1 at one and a half; a border's pull fades on places off
  the tube by more than its reach (stock wood inside another limb, a crotch's far side), the weights eased over the
  edges (three rounds). The nearest tube: the feet on the stock stretches round the place (across each stretch's chord
  - a kinked stock ring's tangent may lean far off it -, past its ends no further than a bend's outer wedge), the least
  gap; a place about as near another axis's (within the tolerance) is a crotch's and keeps its stock but by a border;
  a place a cap shares stays. A place laid on a tube takes its skinning, its normals leaning to the profile's. Offline
  (`tree_probe`, eight species' skeletons): the border places standing out of the profile by more than a centimetre
  44-85 % -> 0-14 % (the rest stock pieces with no tube near: a root's underground edge, a broken limb's), the moves'
  99th percentile 3-6 cm.
- **The patches' bark by two tubes** (`tree_patch_pairs`): the stock uv laid the bark at its own scale and turned its
  own way on each island - a decal round a fork, a mitre where two islands met, streaks where it stretched -, so each
  bark vertex takes the two nearest stock axes' mappings instead (the turn round each and its arclength, as their tubes
  lay them) and the second's weight: a half where the two tubes' surfaces stand as near, none a blend's width (a third
  of the thinner radius, 2 .. 15 cm) nearer the first. The pair is in the axes' order (the lower first), so a
  triangle's corners on one pair interpolate alike; a triangle whose corners keep different pairs gets copies of them
  on the two axes its corners weigh most (offline: 15-25 % more patch vertices; a few dozen of 26 000 triangles keep two
  pairs, all at a second weight near 0). A
  place's foot on an axis lies between the two rings' planes (the tube's own cross-sections, so the patch lays the bark
  as the tube does, one foot from both sides of a joint - by the chord across it a bend's outer wedge took the joint's
  arclength whole and smeared a texel over the triangle). Each mapping's fit on a vertex's triangles (the singular
  values of the map to the turn's arc and the arclength: under a quarter a texel smeared, past four the bark squeezed;
  by the near list's triangles - a stock corner both lists share took the far list's coarse triangles' poorer fit near
  by) moves the weight to the one that lays the bark there, and where the better still smears (a saddle looking along
  both limbs: on a pedunculate oak 2.6 m2 of 91; from a fit of 0.65 down - a crotch's face half smeared along a limb drew
  pale ribs) the world's projection takes over by a third weight - the bark from the
  rest position (each instance's offset under 64 m: the coordinates a few hundred tiles, the fp32 texel exact) along the
  world's three axes at the species' tile, by the interpolated normal's facing in the world (no step at a facet's
  edge), the three met by their heights about their weights and the whole met with the tubes' bark the same way (cross-
  faded, two and three barks averaged into a flat brown smear round an oak's joins), the grain upright on the sides;
  the tubes' mappings are not sampled under it whole. Both weights are eased over the patch - three rounds of each
  place's half toward its neighbours' mean along the triangles' edges of its pair, the vertices welded by their places
  (the stock splits every vertex at its uv seams), a place on an open edge held (a vertex's fit is its worst
  triangle's: the second mapping came in blocks a triangle wide) -, then bound by the fits (a mapping keeps no more
  weight than its fit there allows, the other the rest, eased once more within the bounds: the easing carried a
  limb's weight half over a crotch's faces the trunk's mapping smears into stripes, a pale zebra wedge where a limb
  leaves its trunk), the projection's weight steepened after (none under a third, all past four fifths: eased out, it
  covered good bark round it). Before the easing, on a pair of a
  parent and its child the parent's mapping takes the place against the parent's tube - all of it on the profile,
  none 1.3 of the child's radius out (5 .. 35 cm; at half of it the grain turned along a straight line round a limb's
  foot) as far as its fit lays the bark there (on a limb's own face by the crotch the trunk's mapping smears into
  stripes; forced there, it drew the seam along the crease), the projection as the fit put it (upright on the trunk's
  sides as the trunk's grain): a limb's patch runs into
  its trunk's tube along their seam, and the trunk's patch round a limb's foot meets the limb's tube, whose junction
  record turns it to the trunk's bark; and where a place's copies keep different pairs (the triangles round it settled
  on different axes, each showing its own first axis's bark: a straight seam in one patch) every copy takes the axis
  the pairs share, else the world's projection, and one projection's weight, the most of its copies' - both sides of
  the edge alike, in both lists -, held while the easing carries it in.
  The pixel shader wobbles the second weight where it blends by a noise of the rest position six centimetres to a
  decimetre and a half across (interpolated over the patch's triangles it met the two barks along straight lines).
- **The stock patches over the tubes**: each stock ring keeps the sixteenths of its turn the stock patches lie over,
  seen from its axis - the ray from the ring's centre out through the tube's point there meets an outward stock
  triangle first, 15 cm past the point at the most (any axis's: a fork's patch hangs on the parent), and so do the
  rays half way to the neighbouring sixteenths and rings (a patch's border meets good tube: no dent past it); the own
  ray alone where the point stands out past that triangle by more than the tolerance `max(2 cm, 5 % of the ring)` or
  the ring's fit error is over it (the patches carry its shape, `tools/trees/FORMAT.md`), an axis's end rings only
  where they stand out so. Each covered sixteenth steps back on its own (four bits of the ring's record a sixteenth):
  as far as it and the middles of the quads round it stand out along their rays and a centimetre - level L the
  tolerance and ((L - 1) / 14)^2 of the rest of the profile's radius there, 15 % of the radius left at the least.
  Then the bark patches' points the tube as drawn still buries by more than the tolerance (a fork's crotch, where the
  rays run on inside the other limb) take in the two sixteenths round them and the two beside those on both rings of
  their stretch, where that stays out of sight (the corner under a patch or inside another limb's tube; a tube of
  sixteen sides or fewer steps a vertex back by the least level of the three sixteenths it spans, and the pair alone
  stepped none). A stretch between two rings covered all
  round is not drawn at all; so the patch shows and the two never fight for the depth (offline, a pine, a birch and
  three oak skeletons, `shrub_preview --overlap`: the drawn tube's samples under a patch standing out past it by more
  than the tolerance 0-0.3 % instead of 3.2-5.4 % as the tubes were built; the bark patches' points buried deeper than
  the tolerance 0.2-1.7 % instead of 3.0-6.9 %, within 2 mm of the tube 0.4-3.2 % instead of 1.5-13.8 %; 4-400
  tube samples stepped back by more than the tolerance in sight, 2-4 % of those stepped back so).
- **Limits**: 2^24 axes and aux slots a template, a bend chain of seven axes (a fixed axis ends it), 80 tree species,
  arclengths in f32, 16384 records a frame for both populations.

## Templates and the disk cache

A template is a kind, a species and a tilt bucket: every placement of them draws it on its own transform. The level's
templates are grown on the task pool, their leaf elements capped by the level's budget (9 million elements shared
out on Extreme, 0.85 / 0.7 / 0.5 / 0.35 of it on High / Default / Low / Minimum from the next level load, 160000 to
20000 a template in steps of two), packed into the trees' element, axis and aux buffers and kept on disk. A template's
aux, in 32-byte slots from its first: a header (the patch vertices' first slot and count, their sidecar's slot, the
rings' first slot; the junction table's slot and length, the junction records' slot and count), the grafted axes'
junction table (a u16 an axis: its record + 1), the junction records (two slots each: the parent's axis point and
tangent at the attach, the child's way out across it, the parent's mean radius and its profile's toward the child, its
arclength there, the way out's turn in the parent's bark, its sky and level byte; the parent's segment there - its
rings' radii and arclengths), the rings' morph table (a word a ring: how many rings back and on levels 1 and 2 keep
round it, 0 back where it is kept itself), the stock rings (two slots), the
patch vertices (one: the first axis and its arclength, the turn round the second axis, the first axis's run - the
stretch its arclength falls in: the template's ring before it, 16 bits, 0xFFFF none, and its share of the way to the
next, 16 bits), the caps' copies (one), the patch vertices' sidecar (a word each: the turn round the first axis, the
second mapping's weight) and second sidecar (two words each: the second axis - 24 bits, all ones none - and the world
projection's weight, the second axis's run) and third sidecar (two words each: a tessellated vertex's lift over its
stock facet, three halves, and the facet's normal, eight bits a coordinate). A stock tube's segment
names its
junction record in its last word's upper bits, a grafted segment flags it in its axis word's top byte (the table
names it). On disk:
`$app_data_root$\cache\trees\<kind>_<species>_<hash>_<genver>.bin`, the hash over the species section's text, the
skeleton's bytes, the kind, the bucket, the cap and the generator's version (`kGenVer` in `da_trees.cpp`: raise it
when the generator changes). A torn or stale file is grown again.

## Drawing

Per frame a tree in the view takes its tiers by the size of its species' leaf on the screen (render pixels at the
render height, the upscaler's input): the leaf hulls from `hull_px` up, the quads down to `quad_px`, then the quads
thinned to the leaf floor down to `leaf_px` (a crown's leaves stand at the share that keeps each survivor `leaf_kpx`
pixels, grown to hold the crown's cover: it keeps its leaves' look on to the clusters - the spray cards that stood
there read as fern fronds), the clusters to `clu_px` - and on to the billboard for a tree that has one: it is baked
from them (below) -, the blobs under it, then the billboard from `R` - where a 20 m tree stands `bb_px` render pixels
tall (the atlas's cells are 128 px; a blob tier on to 360 m drew a hundred trees'
trunks, patches and blobs for a few pixels) -, each tier crossfading into the next over the last 15 % of its distance
by the elements' hashes; a bare tree keeps its finer tiers a third longer (its twigs). Under the pixel floor (`kpx`)
the hulls, the clusters and the blobs draw a prefix of their elements, the lit outer ones first, the survivors grown
to keep the area; the flowers and fruit at twice it (a cherry's fruit were tens of thousands of elements a tree).
Every such prefix - the view's and the shadows' - ends softly (`emit_thinned`, `SHRUBS.md`): its last 30 % shrinks
away by rank. With a hard end each leaf at the end of a thinned crown's prefix popped in and out, grown, as the
distance moved it - thousands a second as one walked toward a tree, its crown's inside blinking. The leaves' floor
(`leaf_kpx`) holds the thinned tier's survivors to about 2.2 times their size at its end (at 5 px on Extreme a third
of an oak's leaves stood at 25 m grown to 5 px: big flat confetti floating round a bare crown, its twigs under a
pixel). The crowns' fine detail (`crown`): the leaves' floor past the quads and a leafy crown's wood floors (its
limbs, branches and twigs, cut at multiples of the pixel floor under its leaves) divided by it, never under the pixel
floor and the wire rule; from 2 a leafy crown keeps its twigs and shoots a tier longer, as a bare one does. Measured on
the QA rig at Extreme (Cordon's oak stand, 3 s windows interleaved): the crown detail 0.1-0.35 ms of the GPU and the
longer thinned leaves and clusters (`leaf_px`, `clu_px` halved on Extreme) 0.3-0.45 ms - the thinning had saved next
to nothing and drew a near crown's leaves big and few, its twigs gone, and a poplar's crown in clusters (a grainy dot
pattern) from 30 m on.

| preset | hull_px | quad_px | leaf_px | clu_px | bb_px | kpx | leaf_kpx | shadow_kpx | cascades | crown |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Minimum | 40 | 14 | 4 | 2 | 110 | 3 | 8 | 7 | 2 | 1 |
| Low | 32 | 11 | 3 | 1.5 | 100 | 2.5 | 6.6 | 8 | 3 | 1.5 |
| Default | 24 | 8 | 2 | 0.8 | 90 | 2 | 5.5 | 9 | 3 | 2 |
| High | 20 | 7 | 1.33 | 0.53 | 80 | 1.75 | 4.4 | 10.5 | 3 | 3 |
| Extreme | 16 | 6 | 0.75 | 0.3 | 72 | 1.5 | 3.4 | 12 | 3 | 4 |

The crowns' density from tier to tier (1.6.2, `tools/qa/water/tree_density.lua`: the camera still, `r__trees_lod_scale`
walking the tiers' distances, so each crown runs its tiers through at one size on the screen; the share of the sky a
box over the crown covers, by colour against the sky): the Cordon village's poplars and birches hold 0.79-0.84 from
the hulls to the blobs, within 0.03. Seen from 35 m up over the meadow, the groves at 150-300 m keep their cover into
their billboards within 0.02-0.05 at the switch's own size (a billboard drawn three or four times its 128-pixel cell -
`r__trees_lod_scale` 0.1-0.2 over a near crown - shows its cell's solid inside and reads a fifth denser: not a sight
of the game). The leaves of two or three render pixels were the mid distance's squares (a poplar's at 20-40 m: its
own leaves, over the floor, by the quads; an upscaler's input at DLSS Balanced makes them twice that on the screen,
its sharpening crisper): their cut-out is hashed now under a temporal resolve (SHRUBS.md, the near tier's leaves), the
same cover, soft. A lower leaf floor changed nothing there (they stand over it) for +0.2 ms.

- **The wood's cut** (`WoodCut`, `da_veg_internal.h`): the template's wood sets ordered by their radius falling, a
  table of their radii at every 32nd - the grafted wood's three groups (levels 0-1, 2, 3) axis by axis by an axis's
  thickest mean radius (the vertex shader shares a joint's ring with the next segment beside it), the stock tubes' three
  levels by their thicker ring's, the patches near and far by their limb's. The shaders' wire rules by the radius alone
  (a tree's segment under a quarter of the pixel at its own depth, or of the texel, stands nowhere; over it it grows to
  half a pixel, its pixels kept at the ratio - by the hash the CPU could not split a crown's thin twigs off its thick
  ones), so a view or a cascade draws the prefix over its floor - a quarter of its pixel at the tree's nearest depth,
  or of its texel, over the tree's scale -: every element the shaders would stand and a few more, none they throw away
  (sub-pixel wood was most of the trees' cost: its vertices ran to be culled); and a split of the prefix by radius gives
  each part the sides its silhouette needs (by the table's shares of the whole set: read against the prefix's count
  the splits fell short). Where a leafy crown's groups are cut at multiples of the floor (under its leaves) the
  prefix ends softly (`wood_runs`, `da_veg_internal.h`): the axes whose key passes the cut by half again stand whole,
  the ones down to it shrink away by their key read off the table, in runs of their own - with a hard end whole
  limbs a pixel wide blinked in and out at it, a 32nd of the group at once (the table's step).
- **The GPU cut** (`r3/da_tree_cut.cs`, `da_tree_cut_args.cs`; `da_veg.cpp` `cut`): the view's stock tubes and patches,
  grafted wood and near leaves (a crown thinned alike for its hulls and its quads) go past the CPU's prefixes to the GPU
  as candidate chunks, each with its route - its class, the four draws it may take, its parameters. After the frame's
  bends one lane an element decides alone: out of the view by its own bent and swayed place and reach (a near crown
  above the screen ran the vertex shaders for nothing: the per-vertex test held the wind's reach of a shrub, 0.9 of the
  height); under the wire rule at its own depth; the wood's sides by its own radius on the screen at its own depth (32
  from 26 px, 16 from 6.6, 8 from 2.6, five under it - a group's cap kept: the twigs eight at the most, the shoots
  five -; the far wood under one and a half, or where the bark's tile is under four pixels); a
  leaf's tier by its own distance (the hulls', the quads' and the far quad's windows of the hashes by its own distance,
  the clusters' share the tree's); and joins its draw's list, a word an element (the chunk, its element, its crossfade's
  shrink) past the chunk records in the chunk buffer (a vertex shader has no resource slot to spare). A group counts its
  draws in shared memory, one atomic a draw a group; the lists' instances of 64 go to indirect draws (`da_tree_cut_args.cs`
  marks their tails). The tree's own choice drew a near crown's far side at its near side's detail: the two trees at
  the user's fork took 3 ms of the GPU's frame, 0.2 ms after. `r__trees_cut 0` (QA) draws the CPU's cut alone. The
  clusters and the blobs go through it too (`ccMass`: one draw, the thinned set's runs - `thinned_runs` - as candidates).
- **The shrubs' phase** (1.6.3): the cut's third phase is the shrubs' view (SHRUBS.md, "The GPU cut"), after both of
  the trees' phases, against the pyramid with every tree drawn in.
- **Occlusion** (1.6.2): the cut runs in the G-buffer stage (`da_veg_render_gbuffer`, not with the bends), in two
  phases. The trees nearer than `kCutNearPhase` (40 m) first, each element also against the depth pyramid of what is
  drawn so far (`da_hzb.cpp`, `r3/da_hzb.h`: the opaque world, the LODs and the first-person view - the test of the
  grass's generation, GRASS.md "Occlusion"), drawn with every chunk the CPU chose; then the pyramid again with them in it
  and the far trees against it: behind a wall, a hill or the first rows of a stand an element joins no list (its
  bounding sphere's nearest depth past the farthest one over its rectangle). The phases keep lists, counts and draw
  arguments of their own (`da_tree_cut1`: the phase's first count). The grass's own pyramid after them holds the trees
  and the shrubs too. `r__trees_occlusion 0` (QA) culls by the frustum and the wire rule alone. Measured at the eye in
  the Cordon village (`trees` save, four headings, Extreme, DX11 1080p; `tools/qa/water/tree_hiz.lua`): the cut's
  elements drawn 1465 k -> 1115 k, facing the houses 272 k -> 98 k and the trees' GPU 2.24 -> 1.61 ms; where the near
  trees fill the view and nothing stands before them, the same (the two pyramids and the second cut some 0.05-0.1 ms).

- **The stock wood**: tubes of 32, 16 or 8 sides by the thickest ring's silhouette at the distance (half a pixel of
  error: `n = ceil(pi / acos(1 - 0.5 p / r))`), each ring collapsing its class's vertices to its own need - the need by
  the ring's own radius and depth, so both segments sharing a ring build it alike (a need per segment opened gaps
  where it flipped); a segment in view or culled, under the wire rule or not, as one (the wire rule by its radius: a
  limb thins from its tip, whole); the rings of the level of detail of the tier - every ring to the quads' end, the
  middle level to the clusters', the far level past them -, a level's dropped rings moving onto the next level's
  tube over the last 30 % of its range (`stock_level`; the chunk's keep word's upper half the share of the way, the
  rings' morph table - a word a ring past the junction records: how many rings back and on the next level keeps
  round it - names the strip of the coarser tube the vertex goes to, at its arclength and the same vertex of its
  ring), so the switch moves nothing: a bent pine's trunk turned from a curve into kinks at once at eight metres; the
  profile
  `r(th) = r0 (1 + sum ak cos k th + bk sin k th)` about the ring's reference normal, the angle toward `cross(t, n)`,
  its covered sixteenths stepped back - a vertex by the least level of the sixteenths its two edges run over (one on
  a covered sixteenth's border stepped back by the side its rounding fell on, the shader compilers apart, and opened a
  dark crack under the patch's border laid onto the tube); the normals through the transform's cofactors (the stock
  scale is uneven). The patches: the stock triangles (a triangle culled as one; never thinned by the wire rule - not
  widened to the pixel floor, and its dither cut holes in a thin fork's patches far off), the species' bark by their
  two tubes' mappings - the first axis's turn and arclength, the tube's own whole counts of tiles round the rings the triangle's
  runs span stepped where the tube steps them (nointerpolation: the count round the first ring of the span and the
  steps after it - up to six, each its stretch from that ring and its change, or where more steps of one count each
  run over up to 36 stretches two bits a stretch; past both the rest's change on the sixth, a root's flare over many
  stretches seamed there -, and each corner's ring coordinate from that ring, every count by one expression of the
  ring's radius - two rounded apart by an ulp about a half: the patch and the tube meet without a seam; a count per ring
  of three
  rings from the triangle's lowest took the axis's base count over wider triangles - a fifth of an oak's patches, up to
  28 stretches - and their arclength nought, the second mapping in streaks along its axis; the counts go to the pixel
  shader nointerpolation, so the triangle's first vertex alone runs their loop), each turn unwrapped to the
  triangle's first vertex's, the second axis's the same by its weight -, the pixel shader's frame from the mapping's
  derivatives (a count blended about each half of the girth's own, tried for one way to step everywhere,
  double-exposed every stem whose girth asked about a half); the two met by their heights about the weight
  (one bark's ridges over the other's fissures: a fork's limbs' barks interlocking where they meet, not cross-faded),
  the plain weight at its ends. A limb or a thick grafted branch near its parent wears the parent's bark
  toward the parent's surface - per pixel, from the child's place against the junction's frame (interpolated, the
  turn and the weight went wrong across the child's first segment, which crosses the parent's surface far along it):
  the turn round the parent from the child's way out, the parent's arclength and its segment's counts of tiles
  stepping along it as its tube's do, the weight 1 on the parent's surface to 0 a blend's length out (80 % of the
  child's radius, 1.2 .. 25 cm) and nothing past the junction's reach - and the parent's sky, level and zone (per
  vertex, over a wider blend), and only as far as the parent's mapping lies on the child as the child's own does (the
  singular values of the one's screen derivatives over the other's: past a stretch of 1.6 it fades out by 2.5 - a
  co-dominant leader running off along its parent drew the parent's turn along it in streaks); a limb whose foot the
  patches wrap has its bark from the patches' pair, and a child leaving its parent where a patch covers the parent
  toward it has none. The cut ends' caps - the stock `trees_spil` caps and,
  across an open cut end the stock left hollow, a fan the generator lays over its last ring - show their end grain
  (`shrub_end_grain`, `da_shrub_wood.ps`): never tessellated, each cap's vertices copied with its cross-section's
  coordinates (square to its axis: an oblique cut's rings are ellipses), its rim gauge (1 on its open border, 0 at
  its fan's centre - the stock caps are fans and pyramids round their inner vertices, so the border lies at the
  centre's distance over the gauge along the fan's rays, its straight edges too) and its place along its own axis,
  the rings nested
  about an off-centre pith, a third along the cut's outline near the rim only (the rim's radius is interpolated over the
  cap's triangles: rings sized by it inside kinked into chevrons along their edges), at the
  species' year width (`ring_width`), the latewood darker and
  raised, the years in groups of a few lighter or darker (still read from metres), the heartwood's core (`wood_heart`)
  in the sapwood (`wood_sap`), five to eight open checks from the rim inward, nearly straight with ragged edges and
  widening to the bark (a smooth wavy check read as a pen stroke), the dark pith, the bark round the rim over its inner bark, a living tree's callus rolled over the edge; the
  Zone's thirty-year-old cuts weathered most of the way to a dark silver-grey (a dead tree's more), a rotten browner
  core on some; a cap standing off its own plane by more than a fifth of its radius (a gable, a cone: a break more than
  a saw's cut) its own stock normals and no rings, checks, pith or heartwood - its wood torn through its fibres
  instead: bundles a centimetre across, raised splinters and recesses between them, and the fibres a few millimetres
  across on them, both drawn out along the cap's own axis (value noise of the cross-section's coordinates and, ten
  times slower, of the place along the axis, the patch shader's ring slot), their relief bent into the normal (marked
  by 4 added to its rim gauge: its faces fold along their triangles' edges, which the smooth normals hide, and the
  rings, cylinders round the pith, kinked into nested chevrons at every fold; without them the face read as putty,
  splinters by the cross-section alone as a paper fan and the zero lines of a noise as a maze of worms); a flat cap's
  rings stretched into
  ellipses on an oblique cut, fading to the weathered grain only where its face stands nearly along the axis (by a
  pixel's area in the cross-section over its area on the face); the bark round the rim and the callus by the rim
  gauge (a cap with no inner vertex by its mean radius); any flat cap one normal (its
  triangles' by their areas, outward: a stock cap of two faces
  read as two lids along their crease; a cap's triangles one cap by their places to a millimetre); the wood weathered
  to a quarter of its value (at nearly half the cut stood twice as light as the bark round it, a plaster cast), the
  years' groups as low ridges (the earlywood weathered out between them), the face's grain of pits and fibres a few
  millimetres and blotches a few centimetres across; the tube's end ring
  stepped back wherever its cap crosses it past the patch's 4 mm out along the face's normal (a gable's low face poked
  a bark scrap through the cut); every copy of a cap its voted axis (its rings' seed: a cap skinned to two axes drew
  its rings round two piths);
  rings, checks and pith fading to their
  mean as they near a pixel; the rim's bark as the species wears it. A thin dead stub's bark at half its value, easing
  back by 4 cm across by its true radius - the same at every distance - (its tile shrunk to a thin twig's girth reads
  its mean, without its fissures' shade a pale dowel; a dead limb keeps its own). The bark: the
  species' layer without its tile's repeat (hex tiling: the plane of tiles cut into equilateral
  triangles a tile on a side, one side along the turn - a whole number of them round the stem -, each corner reading
  the tile at its own offset, half of them mirrored round the stem and each at its own scale (0.9 .. 1.1 each way) and
  turn (3.5 degrees at the most) about its corner, its relief's turn with it (mirrored only, a pine's plates and a black
  poplar's forks came back twice a metre, flipped; the grain's up and down kept), the three blended by their weights lifted a
  little by the bark's height), its upper
  layer (`bark_tex_upper`) over its zone (`bark_zone`: two shares of the tree's height above the ground at the trunk
  with `bark_zone_mode = share` - a young tree's rough foot as low as its height asks; two heights above it in metres
  with `height` - the ground the mean of the level's round the trunk, a tree on a slope or sunk into it by the level
  stands its root a metre and more under its foot -, or two radii with `age`) at its own tile size and contrast
  (`bark_scale_upper`, `bark_contrast_upper`: the smooth young upper barks at 1), taking over in patches: the zone's
  height share shifted by the boundary's run (a noise of the rest position stretched along the stem, at the species'
  tile - an aspen's tongues a hand wide, a pine's a forearm: at one size for all a thin aspen's foot rose in a rounded
  sock -, three times as tall: at seven a pine's ran down its trunk as a ruler-straight stripe -: tongues of the lower
  bark up and of the upper down, most in the zone's middle, none at its ends), against
  each place's own level
  (the lower bark's height - its fissures fill first, its plates stand longest; its ridges first with
  `bark_zone_order = ridges`, the fissures left as dark lenses, a place's level then more by the lenses than by the
  height: by the height alike the last of a poplar's foot stood as thin ink-black lines in the white - and lenses of a
  few centimetres three times as tall: a
  birch's dark lenses left in the white, a pine's grey plates as islands in the orange), the threshold eased over what
  a pixel spans of it (the lower bark's relief over the tiles a pixel covers, the lenses over its metres: a hard edge
  sparkled and crawled in a spray of single pixels from a few metres off; far off the two barks blend by the zone's
  share); the slow swell of
  the bark's value and the lichen's blotches by noises of the rest position too (one over every seam); the wood under
  the species'
  `twig_r` in the young twig's skin (a birch's twigs dark under its white limbs); dead wood as its layers paint it
  (nothing bleaches it: a dead tree wears its own weathered layers, a living one's dead stretch its bark). The tiles'
  stretch along the stem (`bark_stretch`, 1 by default), the upper layer's value and chroma against its tile
  (`bark_value_upper`, `bark_chroma_upper`). The crown's tint (`shrub_crown_light`: its leaves' chroma in its shade)
  on a tree's old wood a third of the shrubs' (a big spruce's trunk inside its crown took its needles' green into its
  bark, an olive-brown felt; the light round a trunk is the stand's gaps' and the ground's as much as the crown's).
- **The far level**: in the blob tier, the middle and the far cascades and the local lights the stock wood is its far
  level's tubes alone, whole along every axis and round it (`kChunkFar`: no step back under a patch), without the
  patches (a far trunk's were a thousand triangles a tree for a few pixels).
- **The far wood** (`r3/da_shrub_wood_far.ps`, `da_shrub_wood_far.vs`, `da_tree_stock_far.vs`, `da_tree_patch_far.vs`):
  the bark as its mean worked out per vertex - its tile's last mip in the species' tint, the cavity's, the lichen's and
  the algae's means, the snow, the crown's light - and a zoned tree's two barks both, their hem the pixel shader's: the
  zone's share at the vertex shifted by the near bark's own boundary run (`shrub_zone_run`, the same noises of the same
  rest position: the tongues where the near bark has them - by the vertex alone a white poplar's dark foot was a smooth
  gradient between two rings at 25 m); seven interpolants for the near wood's seventeen to twenty (a far tree's wood paid
  the near bark's attributes for a bark under a pixel); the patches without their runs and their second mapping. Where
  the bark's tile is under four pixels or the wood under one and a half across (the GPU cut's, per element; the CPU's cut
  a whole tree by its nearest depth); the near bark there blends its two barks by the same run, its lenses under a pixel.
- **The grafted wood** by group: the branches past one and a half cluster distances, a leafy crown's twigs past the
  quads and its shoots past the hulls (a bare tree's a tier later); sixteen, eight or five sides by the thickest
  branch's silhouette (half a pixel of error); every group's prefix over the floor - a leafy crown's branches at twice
  it among its leaves and four times under its clusters (eased over their band: under the foliage only the thick ones
  show), its twigs at two and a half, a leafy crown's stock limbs at up to twice; a bare crown's at the wire rule's
  own.
- **The foliage**: the leaves' hulls, their quads (a lobed or compound leaf's cut quads, a spray's leaves, a needle
  sprig's crossed quads near), the quads thinned to the leaf floor (a spray's far quad its whole spray), the blooms,
  the clusters, the blobs; each tier dissolving into the next, the whole tree into its billboard over the last 15 %
  before `R`.
- **The billboards**: past `R` a tree is its stock FLOD's two facets nearest the view, blended as the stock billboards
  are (shifted half a radius toward the eye, the alpha by the band and the far plane's last 12 %), drawn with the stock
  `lod` shaders and states on the module's atlas pair (below), `lod.ps` writing the near tiers' foliage material
  (`da_tree_lod.ps`), near to far after the trees' G-buffer draws. A tree standing dead among its template's living
  trees takes its template's second cell set, baked in the dead look (with no room left for it in the atlas it keeps its
  blobs out to the far plane); a template whose every tree stands dead bakes the dead look into its own cells.
- **The order**: the trees, their billboards and the shrubs draw into the G-buffer before the grass, the trees before the
  shrubs (`r2_R_render.cpp`, `da_veg_render_gbuffer`): their trunks and crowns hide the grass and the bushes behind them
  from their pixel shaders, as the stock trees drawn with the level did (drawn after them, the trees cost the grass and
  the shrubs half a millisecond more).
- **Shadows**: every wood's prefix over the texel's floor. The near cascade: the stock wood at the view's level and
  patches (a coarser caster stood in front of the view's surface: a dark band down a pine's trunk), the branches (a leafy
  crown's at three times the floor: inside it the leaves' shadow covers the thin ones) - a bare crown's twigs and shoots
  too, its shadow's lace -, the leaves within 22 m (and the view's leaf tiers) thinned to
  `shadow_kpx` texels, the clusters past them, crossfading over the outer half of those 22 m (over the stock band's
  3 m a tree's shadow on the ground at one's feet turned from blots to leaves at once as one walked toward it); no
  caster switches by the camera's distance near the eye (the twigs at the hulls' end and the cards at the quads'
  stepped the shadows round the player as one walked). The seams between the cascades are bands, not lines
  (`MODDING.md`, `r__sun_cascade_blend`): each map holds its own casters. The middle one: the
  far level's tubes (no patches: tens of thousands of triangles in the middle map for a texel or two of difference), a
  bare crown's branches and the clusters, their leaves gathered into round clumps three texels across where a leaf is
  under three (`da_shrub_mass.h` `shrub_mass_dapple`: the near cascade's leaves cast a dapple, a solid blot stepped at
  the seam). A cluster's or a blob's quad whose leaf cells stand three texels and more casts round clumps (`shrub_mass_dapple`,
  a clump a cell, Beer-Lambert's cover of the part; `da_shrub_cluster_shadow.ps`, 1.6.3): a part casts from past the near
  leaves - a crown over 22 m away whose shadow falls into the near map, or into a farther map -, high over what it shades,
  where its sunflecks are the sun's pinhole images and its leaves' outlines lie inside a penumbra of some nine centimetres
  a ten metres of height. Their textured outlines, every cell turned its own way, and the twigs were most of the trees'
  shadow maps: seven million texels a frame in the near map (Cordon village, the white poplar's heading: the sun 5.63 ->
  4.68 ms; Ghost of Tsushima casts its far trees from cheap shadow proxies of their own, the `*_ll_shadow` alpha maps
  beside their imposters). The far one: the trunk's far level and the blobs. The rain map: the crowns' blobs (a dead tree shelters
  little). The shadowed local lights within 40 m: the far level's tubes, a bare crown's branches, a leafy crown's
  clusters (a village's lamps drew three hundred thousand of the forks' patches). The clusters a shadow pass draws
  past the pixel floor are thinned - the middle cascade's to two fifths, the near one's (a far crown's past the leaves'
  22 m) and the lights' to two thirds, the far cascade's blobs to a half -, their cover made up (below): a crown's clusters stood ten deep along the sun,
  each of the middle map's texels shaded as often; the share of the leaves on is the vertex shader's there (the same
  over the quad, the shadow pixel shader ran it every texel).
- **The parts' thinning**: past the pixel floor a chunk keeps a prefix of its clusters or blobs - they go by importance,
  the outer, lit ones first - at the share its flags' low six bits carry (`thin_flags`, `1 - k/63`), and the vertex
  shader makes the survivors' cover up (`da_shrub_cluster_vs.h`: `1 - (1 - c)^(1 / share)`, Beer-Lambert over the
  stacked layers: the light through the crown keeps its share). The view's by their size on the screen (a cluster all
  from 12 px, three fifths at 4 px and under; a blob from 18 and 6 px: a far crown's parts stood three and four deep,
  every pixel shaded as often), the shadows' by the pass (above). The mass pixel shader leaves a pixel out of its part's
  ragged outline - the quad's corners, a fifth of it - before any leaf is worked out (`shrub_mass_at`); its cells' hash
  is an integer one (the sine hash took a quarter-rate sine a call).
- **Wind**: the trunk sways on its stock root's oscillator (the root's state, frequency factor, gust and deviation,
  the frame's wave and wind: `da_tree_bend_ex` on the CPU, the downwind push per metre of height now and a frame back
  in the record): `d = push h saturate(h / H)`, capped by a tanh at `da_trunk_bend_share` of the height (0.27 of it up
  to 6 m tall, 0.18 from 20 m: the top's asymptote about 16 and 10 degrees; at half the height every tree leaned 30 in a
  storm, 1.6.2), the length kept by the drop; the stock trees, their impostors and the rain map take the same; while
  the player may be in the tree (the camera within 4-8 m of the trunk and 2.2-3 m up over its root) the climbable wood
  moves a few centimetres at most (3 cm times 1 + 4 wind^2) up to its top and the rest on from there - held always, a
  tree whose stock wood is climbable most of its height stood still in a gale;
  the tree motors as the stock trees'; the grafted axes bend on top through their chain - each by its soft limit of
  0.6 rad times the sine between it and the wind and never past the wind's heading, the chain's turns adding up to
  `SHRUB_CHAIN_TURN` (0.6 rad) at most (1.6.2: an axis turned its full 50 degrees whatever its heading, six levels of
  them on top of one another). The same in the view, every
  shadow map and the motion vectors - their wind a frame back only within 15 m of view depth (`SHRUB_MV_NEAR`: past it
  a leaf's or a twig's sway is under a tenth of a pixel a frame, and the second bend and flutter were a third of the
  vertex shaders' work), the camera's everywhere.
- **Seasons**: the species' rows of the shared tables (`SHRUBS.md`), the young trees' spring lead, marcescence, the
  juvenile phase, the Red Forest's needles, the dead trees, the snow on the wood and the leaves still on. The spring
  lead: the table carries the leaf out, the growth and the fallen share `young_lead` days on while that day has more
  leaves on than today (rows 3 w, 4 w, 5 w), and a young tree blends its whole season state toward them by its lead
  (`shrub_seas_lead`; the data keeps `leaf_out` at 1 through the winter, so the lead moves the state, not each
  leaf's rank). The ray tracing stand-ins (above) follow the same rows.

## The billboards

No billboard atlas ships: the engine draws every far picture of the vegetation itself, for the season's day, and
`level_lods` wears no season look (`da_trees_bake.cpp`). Two atlases:

- **The trees' atlas pair**, made per level by the population: `$user$da_tree_bb` (albedo and alpha) and
  `$user$da_tree_bb_nm` (the normal in rgb, the hemisphere light in alpha: what the stock `lod.ps` reads from
  `level_lods` and `level_lods_nm`), A8R8G8B8, 4096 wide, a cell set's eight views in one row of 128-pixel cells, each
  facet's uv inset by a gutter of a sixteenth of the cell (8 pixels) that holds the tree beyond the facet's edges, a
  chain of levels down to a cell's texel (8). Each facet's uv is mapped from its own stock cell onto its set's. The cell
  sets: every template's own, then a second set in the dead look for each template some of whose trees stand dead
  among living ones (the Red Forest's red pines keep their needles: not dead); 64-pixel cells for every row when they do
  not fit 4096 lines; past the room a template keeps its blobs out to the far plane, and the dead looks are the first
  to go. Beside the pair the bake's foliage atlas `$user$da_tree_bb_mtl` (R8G8, half their size, its levels theirs from
  the second down): the share of a texel's covered samples that are leaves, and their translucency.
- **The stock path's `level_lods`**: a copy of the level's atlas whose vegetation cells are drawn again (below).

**A template's views.** Eight orthographic views, each along its facet's normal from the facet's near side, framing the
facet's quad in the template's model space (its corners on their texels by a least-squares affine map, the gutter round
them), the template drawn on itself as the G-buffer draws it at the switch distance `R`: its stock wood's coarsest rings
on eight sides and its patches, the clusters the pixel floor keeps there (the preset's `kpx`), unthinned - the view's
clusters run on to `R` and dissolve into the billboard, so it shows what they show (baked from the blobs, a poplar's
billboard stood as rows of flat daubs with the sky between them) -, the wood's sizes on the screen and its wire rule a
runtime pixel's at `R`, the clusters' pixels sixteen times finer (`kBakeLeafZoom`: their leaves resolved into cells -
a part of a few pixels is its mean blot, a disc as wide as the root of its cover, and a thin crown's discs stood
apart as dots and dashes - and clipped against the dither, the bake's samples averaging both into the crown's true
cover); the day's species tables, the snow on the
branches, the Red Forest's needles and the dead look (bare: the twigs, the dead bark rule, the snow - every tree of
the template dead, or its second set), its trees' mean youth (the spring lead, marcescence), no wind. The far tiers'
own pixel shaders run whole (`da_tree_bake_mass.ps` and `da_tree_bake_wood.ps` include `da_shrub_cluster.ps` and
`da_shrub_wood.ps`); what they write goes to the bake's three targets instead of the G-buffer: the albedo, the normal in
the bake camera's view space (the G-buffer's own: the runtime camera faces the facet it draws), the hemisphere light,
and whether the sample is a leaf with its translucency (the material code taken before it is dithered, the camera
looking into the sun: the leaf's full value). One billboard stands for trees turned every way, so the
parts' darkening of their sunlit side (`SHRUB_SUNLIT`) is left out of the bake and taken by `da_tree_lod.ps` under the
frame's sun on the texel's share of leaves (a mean over the sun's headings left the billboards' lit side a fifth
lighter and warmer than the blobs' at a low sun). The patches baked are the far list (the stock's own triangles, as
the G-buffer draws them at `R`); a batch takes a template's chunks whole or leaves it for the next batch. Four by
four samples a texel, resolved into a staging chain - the coverage the samples' share, the rest their mean over the
covered ones -, its empty texels filled from the levels above (the offline bake's push-pull fill: the filtering meets
the leaves' colours, not black). Each level's alpha is scaled so the billboards' alpha test at the preset's reference
(`r__aref_quality`) keeps the cell's coverage (Castano's coverage-preserving alpha, `da_tree_bake_cover.cs`: the share
of the level's texels that must pass is the cell's mean coverage). What `lod.vs` and `lod.ps` do to a texel is taken
back so the G-buffer gets what the blobs gave it: the albedo's saturation and brightness (`r__lod_sat`,
`r__lod_bright`), and the hemisphere light over the facets' vertex light (lod.vs: x 3.1, then `r__lod_hemi`) by the
template's mean of its trees' record light over their facets' vertex light. The trees' billboards draw with
`da_tree_lod.ps`, `lod.ps` with one change, the material: the near tiers' foliage codes (`da_pack_foliage_mtl`) on the
share of the pixels the foliage atlas gives (dithered a pixel at a time, turning with the frame when motion vectors are
on), their translucency turned by the frame's sun as the near tiers turn it - the least away from the sun, the leaves'
own looking into it (`shrub_translucency`) -, else `lod.ps`'s; the backlit glow of a forest goes on past `R`, bare and
dead trees have none. `lod.ps` itself and the stock path's billboards are untouched.

**When.** Every cell set at the level's first frame (the level log's `[trees] billboards baked` line, its CPU time),
after a device reset (the pair made again), on `r__trees_bb_rebake` and on a jump of the season past two days (a save
of another day, a time skip, the QA pins: every set at once - within the progressive budget a January forest stood
seconds in July's billboards); then a set at a time, a few views a frame within about 0.3 ms of the GPU (the cost of a
view measured by `r__gpu_log`'s `trees_bake`), whenever its species' season rows changed and the season's day moved
two days since its bake, or the look of all of them moved: the snow (a tenth), the presets' alpha reference, the lod
tuning, the billboards' pixels a metre at `R` (the render height and the field of view setting: not a scope's, nor the
health's narrowing), the pixel floor - each a step once it moved three quarters of one past where it stands, so a value
at a step's edge does not bake back and forth. The sun is no part of the look: the billboard shader takes it. Camera cuts, the menu and the other temporal resets keep the bake.
While a cell set has no picture yet its trees keep their blobs.

**The stock path's `level_lods`.** The stock path still draws thousands of vegetation FLODs (reeds, sedge, duckweed, the
stock fallen trees and debris, a mod's vegetation: the census's candidates neither population took whose parts wear a
`trees\` texture; the shrubs' and the trees' own models too while their stock models stand - `r__shrubs 0`, `r__trees
0`, a population that failed to load). At the level's first frame the level's atlas is copied level for level into a
texture of the engine's (Dead Air's bake: the level's own file; no season look of `level_lods` ships), and each of those
models' eight cells (one set a model, its first placement standing for the rest, as the compiled atlas shares them) is
drawn again along its FLOD's own facets (the facets' quads and uv rects of OGF_LODDEF2): the stock parts with their own
geometry and transform, cut out at the alpha reference where their shader cuts (foliage, not a trunk), in the albedo of
their base texture - the latched season card -, unlit, four by four samples a texel. Calibrated to Dead Air's bake as
the offline bake was (`pipeline\seasons_lods.py`), once a level: the same cells drawn with the base cards against Dead
Air's own pixels give the alpha's quantile map from the coverage (101 points) and the colour's linear gains over the
texels both cover past 0.6 (each channel within 0.25 .. 2); every look is drawn through them, the cells' levels keeping
their coverage at the alpha reference. The copy is bound as the `level_lods` texture's surface
(`CTexture::surface_set`): `details\lod` and `render_lods` draw it unchanged, on DirectX 11 and 12; `level_lods_nm`
stays the base. Cars, ruins, rocks and every other cell keep Dead Air's pictures. It is drawn again whenever the texture
latch moves or the texture reloads (`SyncTextureSeason`), and when a population's stock models come back.

**DirectX 12.** The atlases are bound through their textures' views at each draw (the trees' pair and foliage atlas by
their billboard element, the stock FLOD's `level_lods` by its `details\lod` element in `render_lods`; the GPU-driven
registry hands an FLOD's billboard to `render_lods` too, `gd_node_main`), and the layer copies a view's descriptor at
the draw: a picture baked again or a surface bound anew is what the next draw samples. The ray tracing scene never
reads them.

**Without the bake** (its shaders missing or failed): the pair has one level and a template's cells are a GPU copy of
its stock kind's cells from the level's own atlas (`da_tree_bb_copy`), the dead looks stay empty (those trees keep
their blobs), no foliage atlas (`da_tree_lod.ps` writes `lod.ps`'s material); `level_lods` is the file.

The bake's entry points in `da_trees.h`: `da_trees_bb_atlas` (the pair's names and the foliage atlas's, size, cell,
gutter, levels, the templates and the cell sets, this frame's `R` and pixel floor), `da_trees_bb_template` (a cell set:
its template, kind, species row, the placement it was taken from, its eight views - the facet's rect, its quad in the
template's model space in the stock vertex order, each corner's uv, its normal -, its dirty and filled flags, its look:
youth, dead, red, the hemisphere ratio) and `da_trees_bb_mark` (cleared: its cells hold its picture).

## Console

- `r__trees 0|1` (default 1): the upgraded trees or the stock trees, live.
- `r__trees_stats`: the preset's tiers, the trees, templates and memory, this frame's tiers, elements and chunks per
  draw and pass, the GPU cut's candidates and - read back, logged a frame or two later - the elements each of its draws
  took, the billboards drawn, the trees per species and kind, the dead, red, sapling and growing trees.
- `r__trees_species_pin <name|off>` (QA): every tree grows that species, from the next level load.
- `r__trees_kind_pin <kind key|off>` (QA): only that stock kind is upgraded, from the next level load.
- `r__trees_probe` (QA): the ten trees nearest the camera - kind, species, distance, position, height, youth,
  template (its bucket and leaves), tier, dead, red, and its trunk's wind (the stock root's oscillator, the wind's
  strength, the push per metre of height, how high its climbable wood goes); `r__trees_probe ray` the ten the view's
  ray passes through, nearest first, with their templates' clusters and blobs (their radii), leaf area, scale and
  tier distances; `r__trees_probe templates` every template's species, height, width, leaf area, parts and how many trees
  wear it (one's position).
- `r__trees_force_tier -1..5` (QA): every tree in one tier (0 hulls, 1 quads, 2 thinned quads, 3 clusters, 4 blobs, 5 the
  billboard), its pixel floor still its distance's; -1 as drawn.
- `r__trees_lod_scale <0.05..20>` (QA, default 1): scales the tiers' distances and `R`.
- `r__trees_detail <0.25..8>` (QA, default 1): multiplies the preset's crown detail (the leaves' floor and a leafy
  crown's wood floors divided by it); `r__trees_far_detail <0.25..4>` (QA, default 1): the thinned leaves' and the
  clusters' tiers end at their leaf pixels divided by it (0.5: the tiers before 1.6.0's fix round).
- `r__veg_soft <0..7>` (QA, default 7): the vegetation's soft switches, bits - 1 the thinned prefixes' soft ends, 2 the
  crowns' wood prefixes', 4 the stock wood's morphs (the ring levels and the patches' tessellation); off, the hard
  switch (an A/B).
- `r__dbg_trees <mask>` (QA): hides draws - 1 the stock tubes, 2 the patches, 4 the grafted wood, 8 the leaves, 32 the
  blooms, 64 the clusters, 128 the blobs, 256 the billboards, 512 every shadow map and the rain map; 1024, 2048, 4096,
  8192 the trees in the leaves', the thinned leaves', the clusters' or the blobs' tier (the view); 16384 the clusters and
  the blobs unthinned past the floor; 32768 the patches without their runs (their bark's seams: the runs' cost); 65536
  the clusters' and the blobs' pixels out at once (their pixels' cost); 131072 the stock patches untessellated
  everywhere.
- `r__trees_cut 0|1` (QA, default 1): 0 draws the view as the CPU cut it, without the GPU cut - an A/B.
- `r__dbg_wood 0..6` (QA): the wood's bark in false colour (`da_shrub_wood.ps`, its main's last block and
  `shrub_dbg_*`) - 1 what draws it (a
  tube or grafted wood green, a stock patch red, a cut end white; the second mapping's weight blue over it, a patch's
  world projection yellow), 2 its counts of tiles round the stem (a colour each whole count), 3 the upper bark's zone
  (red its share there, green the upper bark's), 4 and 5 the first and the second mapping in a checker of half tiles
  (the turn's wrap red), 6 the snow line (red the snow, green the stem's top - the highest its normal points -, blue
  the shading normal's up; SHRUBS.md, phenology); 0 off.
- `r__trees_bb_dump` (QA): the next frame writes the trees' atlas pair, their foliage atlas and the level's
  `level_lods` copy, every level, to `$screenshots$` (`trees_bb_<level>`, `trees_bb_nm_<level>`, `trees_bb_mtl_<level>`
  - red the leaves' share, green their translucency -, `level_lods_<level>`: DDS, and level 0 as PNG).
- `r__trees_bb_rebake` (QA): every billboard drawn again next frame, the stock path's calibration too.

The generator's version is `kGenVer` (`da_trees.cpp`, now 35: the rings' morph table and the patches' third sidecar,
the tessellation's lift; the patch cover seen from the axis with a step back per
sixteenth and the hidden stretches, the patches' tessellation, the leaf orders' quantiles, the whole LAI, the
islands, the wires, the bare limbs, the apical dominance, the misfit rings held, the patches' uv in metres, the
caps' end grain and the open cut ends' fans, nothing grafted on a cut end's face nor on a conifer's limb without a
card branch, a conifer's bare grafted rods dropped, the stock's flat strips of bark grown as thin dead branches, a
conifer's blobs smaller and up to 2048, the far parts at the whole leaf area where the budget thinned the leaves; the
aux header, the junctions and the sidecars; the strips by their places welded; the patches tessellated by places, their
borders laid onto the tubes, their bark by two tubes - their feet between the rings' planes, weighted by their fit, the world's projection
where both smear, eased over the patch, a parent's against its tube, a place's pairs alike -, the caps' one normal, a
cap off its plane's own
normals and one axis and its mark, the caps' rim gauge and place along their own axis, its tube's end ring under it,
a stock parent's junction
radii in full, the patches' runs; the wood's cut tables and orders (by radius), the far level's tubes all along, the
spray cards gone; raise
it whenever
the generator's or the pack's output changes - the cache of its other versions is deleted at the level's load. The level's leaf budget is shared by what each template asks
(its crown field's leaf area over its species' element, `tree_leaf_need`): the small ones take theirs, the large ones
share the rest alike, a dead tree's species 16k at the most.
The disk cache's key takes the header's size too, and a cache whose ranges or measures do not hold is generated again.

`read_skeleton` takes no file whose numbers are not finite, whose points lie outside its box and a quarter of it, whose
box is past 200 m or whose crown grid's cell is not in (0, 2] m, whose axis comes before its parent or whose patch
vertex names no axis; it holds the misfits: a ring profile's harmonics past 0.6 of its radius (a thin twig made round),
an arclength running back, a ring past its axis's bound (a limb's two and a half times its parent's widest ring, the
trunk's half the box across: the median of its axis's rings within the bound) - the level log's `misfit rings held`
line. Before that check (1.6.2): a stock ring of fewer than 8 sides drops its third harmonic (fitted on 7 vertices it
is a three-lobed zigzag, blended toward the next ring's lobes the limb read as twisted into itself), the first harmonic
moves into the ring's centre, and along each axis the second and third harmonics are evened out 1-2-1, so no lobe
turns by half its width between two rings (the misfits went down four to seven times on new_military's trees). The templates' check after the level's build rejects a template with a bad reference: its trees stay stock.

The level log prints `* [trees] data: ...`, the census line, `* [trees] T templates (C from the disk cache), ...` and
`* [trees] N trees of M placements, T templates, E elements (K KB), billboard atlas WxH (P px cells), ms`;
`* [trees] billboards baked (reason): S cell sets (T templates, D in the dead look of their dead trees), V views in X ms
on the CPU, WxH atlas of P px cells, the foliage atlas half that, day D`,
`* [lods] level_lods: N stock-path vegetation placements of M models to draw again`, `* [lods] level_lods calibrated
against Dead Air's bake: gains r g b, ...` and `* [lods] level_lods composed: C vegetation cells ... ms`;
`r__trees_stats` ends with the bake's figures. `r__gpu_log` adds `trees` (the G-buffer draws), `trees_bend` (their
bends and the GPU cut), `trees_bb` (the billboards), `trees_bake` (the bakes, both atlases) and `trees_cpu` (the frame's choice, chunk
builds and uploads).

## Cost

The QA rig at the player's settings (Extreme, DLSS Balanced to 2560x1440, the ray-traced sun and AO on DirectX 12, an
RTX 3070 Laptop GPU), July noon, a light wind: the user's own view on `qa_trees` (the white poplar's fork at arm's
length, a stand behind it) and the script camera on the same white poplar stand from 6, 20 and 45 m; 2 s windows of
`r__gpu_log`, the upgraded trees and the stock ones (`r__trees 0`) twice each, interleaved (the laptop's clocks drift by
a millisecond over a run), the GPU's milliseconds:

| view | DirectX 12: frame upgraded / stock | `trees` | DirectX 11: frame upgraded / stock | `trees` |
| --- | --- | --- | --- | --- |
| the fork (the user's view) | 18.9 / 15.4 (+3.5) | 2.6 | 18.1 / 15.8 (+2.2) | 2.5 |
| the stand from 6 m | 16.4 / 12.8 (+3.5) | 3.5 | 16.1 / 12.7 (+3.45) | 3.3 |
| the stand from 20 m | 20.2 / 16.5 (+3.7) | 2.8 | 19.4 / 16.1 (+3.3) | 2.7 |
| the stand from 45 m | 22.2 / 18.6 (+3.6) | 3.6 | 20.6 / 18.0 (+2.55) | 3.2 |

`trees_bend` (the bends and the GPU cut) 0.25-0.45 ms, `trees_cpu` 0.6-0.8 ms; the stock trees' own share of the frame
(their part of the `scene` pass and of the sun's) is about 2 ms, which the upgraded trees take over. Their shadow maps
are 0.3 ms over the stock trees' now, the local lights 0.25. Before this round the same views cost 10-12.6 ms over the
stock trees (the fork: `trees` 10.3 ms): the GPU cut (`trees` at the fork 6.2 -> 2.9 ms - the two near trees 3 ms ->
0.2), the trees drawn before the grass and the shrubs (0.6 ms of theirs), the lights' far casters (0.6 ms), the
shadows' thinned clusters (0.9 ms), the far wood's hem (the near patches of a zoned stand, 1.5 ms at 45 m), the
patches tessellated only at arm's length (1 ms a tree in the near tier), the mass pixel shader's four cells (a third of
the clusters' and the blobs' pixels: they were half the trees' pass). By the `r__dbg_trees` masks the rest at the fork:
the clusters' and the blobs' pixels about 1.1 ms (their geometry 0.1), the near trunk's patches 0.6, the thinned crowns'
wood 0.6, their leaves 0.2. The billboards' bake (`trees_bake`) runs only on a month's or a level's change.

## The data file

`[tree_species_N]` (N up to 80): every key of a shrub species (`SHRUBS.md`) and the trees' own - `graft_levels`, the
`l1_..l3_` level keys, `crown_fill`, `stock_card_follow`, `autumn_order` (the height's and the shade's weights) or
`autumn_inside` (0/1), `cone_share`, `cone_len` (or `cone_size`), `cone_droop`, `bark_tex_upper`, `bark_zone`,
`bark_zone_mode` (`share`, `height` or `age`), `bark_zone_order` (`fissures` by default; `ridges`: the upper bark takes the
lower's ridges first, a poplar's smooth bark over its rough foot), `bark_scale_upper` and `bark_contrast_upper` (the upper layer's tile scale and
contrast, the lower's by default), `bark_value_upper` and `bark_chroma_upper` (1 by default), `bark_stretch` (the bark
tiles' stretch along the stem, default 1), `marcescent`, `wood_sap`, `wood_heart` (linear rgb and the heartwood's share of the radius) and `ring_width` (m: a cut end's wood),
`twig_r` (the radius in metres under which the wood wears the young twig's skin, default 0), `apex_cone` and
`apex_leader` (the apical dominance: the share of the crown's height under the apex shaped to a cone, the leader's
top metres; 0 and 1.25 by default); no `max_leaves` (a tree's leaves go with its crown).
`[tree_community_N]` (`name`, `species = id:share, ...`), `[tree_grass_community]` (grass community = tree
community), `[tree_class_community]` (class = community), `[tree_policy]` (`max_tilt_graft`, `min_scale`,
`kinds_per_level_species`, `young_lead`, `young_h0`, `young_h1`, `red_community`), `[tree_kinds]`
(`key = TREE|YOUNG|STOCK[, dead][, red][, conifer][, sapling][, species=<id>|community=<name>][, skel=<file>]`),
`[tree_families]` (`signature = key:x0:y0:z0:x1:y1:z1:area, ...`, the box in mm). Edit `tools/trees/make_trees.py`,
not the ltx.

<!-- tree data: tools/trees/make_trees.py -->
## Data

`configs/dead_air_x64_trees.ltx` is written by `tools/trees/make_trees.py` (never edit the ltx) from the census `tools/trees/data/tree_kinds.json` (`tools/trees/census_trees.py`: the engine's keys on the raw levels, parity with every `[shrub_kinds]` key and `[shrub_families]` row) and the species research (`tools/trees/data/tree_photo_colours.json`), the leaf elements of the texture botany (`tools/shrubs/textures/tree_leaf_botany.py`) and the texture layers (`tools/shrubs/textures/out/tree_layers.json`). 52 species (`[tree_species_N]`: 30 of the research, 12 burnt variants `_brn`/`_char`, 10 dead variants `_dead`), 22 communities, kinds: STOCK 24 (515 placements), TREE 313 (19189 placements), YOUNG 48 (773 placements). Tables, rules and the keys beyond the shrubs': [TREE_DATA.md](TREE_DATA.md).
<!-- end of tree data -->
