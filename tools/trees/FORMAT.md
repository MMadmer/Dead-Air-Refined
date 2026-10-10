# Tree skeleton file `.dtsk`, version 1

The stock shape of every upgradable tree kind, written by `tools/trees/skeleton/build.py` and read by the engine's
`generate_on_skeleton` (CONTRACT section 4). One file per distinct stock trunk mesh:
`gamedata/meshes/da/trees/<skel>.dtsk`; a kind names its file with `skel=<skel>` in `[tree_kinds]`
(`tools/trees/out/skel_index.json` maps every family key and every exact member key to its skeleton).

## Skeleton id

- A kind with bark: the content key of its `flora\trunk_wave` part as 16 lower-case hex digits (the engine's
  `read_part_keys` key, as the inventory lists it). A kind with several trunk_wave parts (pine bark zones
  `trees_bark_sosna_01` + `_02`, a bark part plus its `trees_spil` / `*_cap` cut-end part) uses the parts' keys
  combined exactly like the engine's kind key: FNV-1a 64 over the part keys sorted ascending, each packed as a
  little-endian u64.
- Kinds that re-dress one trunk mesh with other card/bark textures share the file. When kinds on one trunk mesh
  carry different card layouts (Jaccard of their card triangles below 0.8), the most placed layout keeps the trunk
  key and every other layout gets `combine_keys([trunk key, its first kind key])` (two such variants exist:
  `2dfe1bddc1744448`, `17d339fc737af90b`).
- Procedural kinds (the 48 BUSH_KEPT_STOCK families; any kind without a bark part): `p` + the family key, e.g.
  `pbac0f21610b78cfc.dtsk`.
- `trunk_key` in the header is the u64 whose hex is the id (without the `p`).

## Conventions

Little-endian. Every float is f32, every count/index u32 unless noted. Model space of the stock kind exactly as the
engine reads `level.geom` (X-Ray left-handed, metres, +Y up, origin = the stock pivot); an instance is drawn through
the stock FLOD's own 3x4 transform (world = [p, 1] x M). Cross products below are the plain component formula
(`cross(a, b) = (a.y b.z - a.z b.y, a.z b.x - a.x b.z, a.x b.y - a.y b.x)`) on the stored numbers.

File size = 64 + 32 + 32 n_axes + 76 n_rings + 40 n_patch_verts + 12 n_patch_tris + 48 n_cards + nx ny nz;
a reader should reject any other size.

## Layout

| offset | section | size |
|---|---|---|
| 0 | header | 64 |
| 64 | crown | 32 |
| 96 | axes | 32 x n_axes |
| ... | rings | 76 x n_rings |
| ... | patch vertices | 40 x n_patch_verts |
| ... | patch triangles | 12 x n_patch_tris |
| ... | cards | 48 x n_cards |
| ... | crown grid | nx x ny x nz bytes |

### Header (64 B)

| off | type | field |
|---|---|---|
| 0 | char[4] | magic `DTSK` |
| 4 | u32 | version = 1 |
| 8 | u64 | trunk_key (see Skeleton id) |
| 16 | f32[3] | bmin: box of rings (grown by the largest r0), patch vertices and card corners |
| 28 | f32[3] | bmax |
| 40 | u32 | n_axes |
| 44 | u32 | n_rings |
| 48 | u32 | n_patch_verts |
| 52 | u32 | n_patch_tris |
| 56 | u32 | n_cards |
| 60 | u32 | flags |

Header flags: `1` conifer (every kind of the file is pine/spruce), `2` dead (every kind of the file is DEAD or
BROKEN: the wood is a dead tree's; a kind's own dressing state is in `[tree_kinds]`), `4` broken top (a BROKEN kind,
or a level-0 axis ending in a cut/open end at the top of the wood: 90 % of its height or more; most living pines,
spruces and birches carry it too: their stock trunk ends in a cut-end cap inside the crown cards, so on an ALIVE kind
it means 'the stock leader stops here', not a snag), `8` cut stump base (reserved, not set by the stock data),
`16` procedural kind: synthetic axes only, free to reshape, no collision constraint, no patches.

### Crown (32 B)

| off | type | field |
|---|---|---|
| 0 | u32[3] | nx, ny, nz (0 when the kind has no opaque card texel) |
| 12 | f32[3] | origin (corner of cell 0) |
| 24 | f32 | cell (m, 0.25-0.5) |
| 28 | u32 | **reserved, used**: number of bark patch triangles. Patch triangles `[0, reserved)` are stock bark; `[reserved, n_patch_tris)` are the stock cut-end caps (end grain: `trees_spil`, `trees_atp_*_cap`) |

### Axes (32 B each)

| off | type | field |
|---|---|---|
| 0 | i32 | parent axis (-1: root); a parent always precedes its children |
| 4 | u32 | level: 0 trunk, 1 modelled stock limb (any depth), 2 card branch |
| 8 | f32 | attach: arclength on the parent's spine where this axis leaves it (m, parent's `s` scale) |
| 12 | u32 | first_ring |
| 16 | u32 | n_rings (>= 2) |
| 20 | f32 | climbable: share of the axis' rings whose stock wood lies within 1 cm of `materials\tree_trunk` collision (majority of the sampled placements) |
| 24 | u32 | flags: `1` from cards (no stock wood), `2` cut/broken end (the stock tube ends open), `4` cap (a stock cut-end cap sits at the end), `8` synthetic (every ring has stock_sides = 0: card branches and the axes of procedural kinds) |
| 28 | u32 | **reserved, used**: level 2: index of its card; otherwise 0xFFFFFFFF |

Order: stock (or synthetic) axes first, parents before children, then the level-2 card axes.

### Rings (76 B each)

CONTRACT section 4 lists these fields and says 72 B; the listed fields add up to 76 B and the file keeps every one of
them in the listed order.

| off | type | field |
|---|---|---|
| 0 | f32[3] | c: centroid (stock ring: mean of the ring's stock vertices; any other station: area centroid of the stock section there) |
| 12 | f32[3] | t: unit tangent of the spine at c |
| 24 | f32[3] | n: unit reference normal (angle 0 of the profile), normal to t, parallel-transported from ring 0 (ring 0: model +X projected, or +Z when the axis runs along X) |
| 36 | f32 | s: arclength from the axis base (m, along the Catmull-Rom spine, 32 steps per segment) |
| 40 | f32 | r0: mean radius |
| 44 | f32[6] | a1 b1 a2 b2 a3 b3 |
| 68 | u32 | stock_sides: vertices of the stock ring at this station; for any other station the facets its stock section crosses; 0 = synthetic ring |
| 72 | f32 | fit_err: largest distance between the tube and the stock surface near this ring, both ways (m; stock wood further than the ring's radius + 5 cm from the tube - another limb left without a tube - is not this ring's error) |

Cross-section in the ring plane: `r(th) = r0 (1 + a1 cos th + b1 sin th + a2 cos 2th + b2 sin 2th + a3 cos 3th + b3 sin 3th)`
along `cos th * n + sin th * cross(t, n)`.

How the profile is placed (AAA_TREES 6.3): minimax (Chebyshev, Lawson IRLS) against the stock section - halfway
between the stock faces and vertices, half the worst case of a ring through the vertices. Harmonics k < sides/2 by
default (the profile does not copy the n-gon's own facets); coarse rings of thick wood where that round profile
misses by more than 0.7 of the tolerance take k = 3. Rings with 8 or more stock sides go through the stock vertices
(least squares) where that costs at most half the tolerance. Tolerance = max(2 cm, 5 % r0). Rings whose tube still
lay outside the stock surface by more than the tolerance were pulled in by the excess, never below 60 % of the radius
their section gave (the stock patch covers them). A tube is measured against the stock surface of its own wood
pieces (and the cut-end caps): the end of a separate limb pushed into the trunk is no trunk surface.

`fit_err > max(0.02, 0.05 r0)` marks rings where the shape is carried by the patches (forks, crotches, gnarled or
lobed sections, flared bases, cut ends): the tube there lies under the stock patch (at most the tolerance outside
it); a renderer may skip the tube between two such rings and draw only the patch.

### Tube reconstruction (the engine must build exactly this)

Per axis with rings i = 0..m-1 and a segment parameter u in [0, 1] between ring i and i+1:

1. Spine `C(u)`: centripetal Catmull-Rom (alpha 0.5, Barry-Goldman) through the centroids; the end segments use
   phantom points `2 c0 - c1` and `2 c(m-1) - c(m-2)`.
2. Frame: `T(u) = normalize(lerp(t_i, t_i+1, u))`; `N(u) = normalize(lerp(n_i, n_i+1, u) - T dot(..., T))`;
   `B(u) = cross(T(u), N(u))`.
3. Radius: `r0(u)` = cubic Hermite between `r0_i` and `r0_i+1` with Fritsch-Carlson monotone slopes over the knots
   `(s_i, r0_i)` (end slopes = the end secants; a slope is 0 where the secants change sign), `h = s_i+1 - s_i`.
4. Profile coefficients lerped: `f(u) = lerp(f_i, f_i+1, u)`.
5. `P(u, th) = C(u) + r0(u) * prof(f(u), th) * (cos th N(u) + sin th B(u))`.

The rings were fitted and measured against the stock mesh through this reconstruction (`tools/trees/skeleton/tube.py`);
the engine's sides/segment counts are free (screen error), the surface is not.

Where the stations come from: one per level-set node of the stock wood (hop distance from the piece's root ring;
a clean node is a stock ring and is its own section), fork level sets left out; stations in the middle of a segment
where the tube missed the stock (the stock section there, `stock_sides` = its facet count); a trunk whose stock
piece is closed or ragged at the bottom gets its first ring's section carried down to the piece's lowest point; a
piece standing on several root ends (a flared base) starts its trunk where the root fronts merge, the root fingers
stay stock (patches). A piece the level sets leave more than half patched is read again: from the geodesic
contours of the piece (its triangles cut k x k, the shortest-path distance from its root over the refined edges,
iso-contours every 2-25 cm - loops around the wood -, the loops' Reeb graph: a loop's parent is the loop below it on
the same band of the surface; stations on the loop centroids about every 0.6 r, a branch tip at the farthest point
of the band where a branch ends; on root fingers the trunk starts where they merge), and, where under 70 % of its
vertices lie on clean stock rings, from a voxel skeleton (solid proxy, geodesic slabs). Both are fitted on the stock
sections like the ring stations; the reading with the lowest score (patch share + share of the stock surface further
than max(35 % r, 3 cm) from every tube + 2 x share of the visible tube surface that far out in the air) wins, a
second reading by at least 0.05. Axes without a radius, or whose tube runs mostly through the air (a level-set chain
that jumped a crotch), are dropped; their wood stays as patches. A kind left without any readable axis gets one
synthetic trunk line (flags 8) and keeps all its wood as patches.

A level-1 axis that branches off a welded fork starts with a join ring on its parent's last clean stock ring below
the fork (inside the parent, `stock_sides` 0, round, the branch's first radius). A level-1 axis that is a separate
stock tube starts at its own stock root ring (inside the parent's wood). An axis whose stock end is closed by a
cut-end cap stops at its last bark ring (flags 2 | 4): the cap patch closes it.

### Patch vertices (40 B each) and triangles (12 B each)

The original stock triangles kept where no tube reproduces them within the tolerance: every stock triangle with a
sample (vertices, centroid, 6 cm fill) further than max(2 cm, 5 % r) from every tube (stock wood hidden inside
another tube ignored), every stock triangle under a visible tube sample that misses the stock by more than the
tolerance, and every stock cut-end cap triangle. When the CFORM gate (below) fails, the tolerance on climbable wood
drops to 2 cm, then 1.5, 1, 0.5 and 0.25 cm, until it passes; at 0 every climbable face stays stock.

| off | type | field |
|---|---|---|
| 0 | f32[3] | p: stock position (model space, exact stock bytes) |
| 12 | f32[3] | n: stock normal (decoded from the D3DCOLOR, unit) |
| 24 | f32[2] | uv: stock texture uv (tc / 2048: bark uv for bark triangles, end-grain uv for caps) |
| 32 | u32 | axis: the axis whose spine is nearest (skinning for wind) |
| 36 | f32 | s: arclength of that nearest spine point on the axis |

Triangles: `u32 i[3]` into the patch vertices, stock winding (`cross(p1 - p0, p2 - p0)` along the stock normal).
Bark triangles first, then caps (count in crown.reserved). The tubes run on under the patches; the patch borders lie
on good tube (within the tolerance), so no seam opens.

### Cards (48 B each)

One record per stock crown card (`flora\leaf_wave` part pieces; two-sided copies and the same card of another dressing
counted once).

| off | type | field |
|---|---|---|
| 0 | f32[3] | base: where the painted branch enters the card: of the opaque runs on the card's texture border, the one nearest to the stock wood (within 0.75 m), else the one nearest to the trunk spine, else (procedural kinds) the lowest, else the thickest |
| 12 | f32[3] | dir: unit direction from the base towards the painted branch's far end |
| 24 | f32[3] | up: unit, in the card plane, normal to dir, towards model +Y (card normal = cross(dir, up)) |
| 36 | f32 | len: extent of the card's opaque texels along dir from the base (m) |
| 40 | f32 | width: extent of the opaque texels along up (m) |
| 44 | u32 | axis: its level-2 axis, or 0xFFFFFFFF |

Level-2 axis of a card: parent = the stock (or synthetic) axis whose spine is nearest to the base, attach = that
spine point's arclength; ring 0 on the parent's spine at attach (left out when the base lies within 1 cm of it), then
the painted main branch (cheapest path from the base through the opaque texels, cost 1 / (1 + distance to the
transparent), to the farthest texel) simplified to at most 4 rings (Douglas-Peucker, 4 % of its length);
radius = the painted half-width, at most 0.7 of the
parent's radius at the attachment and 6 cm, at least 3 mm, never growing towards the tip; round profiles,
`stock_sides` 0, flags 1 | 8.

### Crown grid (nx x ny x nz bytes)

Cell (ix, iy, iz) is byte `ix + nx * (iz + nz * iy)` (x fastest, then z, then y); centre `origin + (i + 0.5) * cell`.
Occupancy 0..255:

1. Every opaque texel (alpha >= 0.5, 64 x 64 mask per card) of the stock cards splatted with its model-space area;
   per cell `occ = 1 - exp(-opaque card area / cell^2)`.
2. Closed by a 3x3x3 dilation + erosion (cells the closing adds: at least 64); empty cells enclosed by the crown: at
   least 40 (room for branches, not for leaves).
3. 3x3x3 box blur, keeping at least half of the unblurred value.
4. Clipped by the passable crown hull: the `materials\bush` / `bush_sux` CFORM triangles of the sampled placements
   that belong to this tree (pieces whose box lies mostly in the card box), mapped into model space, solidified
   by their winding number (|w| > 0.5, robust to the holes of the simplified hulls) and grown by 0.75 m (the hull
   is a coarse copy); cells outside it are 0. Kinds without a usable hull keep the card field unclipped.

## CFORM gate (how the patches were validated)

Per skeleton up to 12 placements (stock-kept first, spread over the levels): their `materials\tree_trunk` CFORM
triangles are mapped into model space through the inverse placement transform. Climbable wood = stock surface within
1 cm of that collision. The visible refit wood (tube samples outside other tubes and off patches, plus the patches)
next to climbable wood must lie within 3 cm of the collision at the 99th percentile, in model space and in world
units (distance times the placement's scale; where the stock wood itself is further than 3 cm from its collision in
world units - placements scaled up about three times -, the stock's own p99 is the bar). Tube samples over a patched
face or over a cap count as patch (the stock triangles are drawn there). `axis.climbable` is the share of the axis'
rings on climbable wood.

## Procedural kinds

Synthetic level-0 axes: one per ground clump of card bases (single linkage in x/z within 1.5 m; clumps under 3 % of
the cards dropped when several exist), vertical from the lowest card point up to the clump's opaque-area centroid,
snapped to a stock stem position within 2 m when the inventory lists stems; 5 rings, radius 1.2 % of the height
+ 1.2 cm (1.5-15 cm) tapering to 45 %; flags 8, `stock_sides` 0, `fit_err` 0, `climbable` 0; header flag 16; no
patches. Cards, card axes and the crown field as above.

## Reading

`tools/trees/skeleton/dtsk.py` has the reference reader (`unpack`) and the validator (`validate`: parents first,
ring ranges, unit and orthogonal t/n, increasing s from 0, attach inside the parent, patch indices, card axes, grid
size).
