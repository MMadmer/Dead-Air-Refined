# Snow: the renderer side (1.6.0)

Volumetric seasonal snow on every surface of a level, at every distance, pressed by everything that walks
on it. The season day `D` (`SEASONS.md`) and the world side (`WINTER.md`: the level bake, the snow constants
of every frame, the deformer stamps, the snowfall, the ice) decide how much snow lies where; this page is how
the renderer turns that into a picture. Data: `SEASON_DATA.md` (`configs/dead_air_x64_seasons.ltx`).

Code: `src/Layers/xrRender/da_ground.{h,cpp}` (the collision mesh on the GPU, the near ground window, the snow
shells),
`src/Layers/xrRender/da_snow.{h,cpp}` (everything else), `src/Layers/xrRender/da_snow_gpu.h` (their private
GPU helpers). Shaders (`shaders/r3`): `da_ground_*`, `da_snow_*`; the public headers `da_snow_sample.h` (the
current depth for any shader), `da_snow_light.h` (the lighting of snow pixels) and `da_snow_ice.h` (the ice
of the season, shared with `water.ps`).

## What is on screen

| Piece | Where | What |
|---|---|---|
| snow grid | G-buffer after the grass, and the near sun cascade | real geometry: a camera-centred clipmap of the ground's snow raised by the snow depth, near (to 64-256 m by preset), running on under a roof, a deck or a vehicle at the floor's level; over frozen water a sheet of ice and the snow on it |
| snow shells | G-buffer, after the grid | the snow on roofs, decks, slabs and tanks: generated geometry on the collision's faces that look up, rounded off at their open edges |
| snow layer | full screen, after the rain apply | every opaque pixel at every distance: coverage, the look of the month, thin-snow see-through, a cover on static surfaces past the shells, material code 31 (1 on a glint) |
| trails | compute, every frame | every creature, corpse, prop and blast presses the snow - feet pack it, legs and low bodies plough trails, corpses lie in churned hollows; snowfall fills it back |
| lighting | `lmodel.h` -> `da_snow_light.h` | subsurface wrap for codes 31 and 1, the cool cast of shade, the glints' sun lobe on code 1 alone |

Every pass is skipped while the level-mean depth of the frame is 0 (May to October), the near ground window's
raster included.

## The frame

- **Main thread, before `Calculate()`** (`da_snow_frame_publish`, from `CRender::OnCameraUpdated`): the level
  fields go up, the passes of the frame are decided (on/off, the trail maps created or dropped), the trail
  windows are placed round the camera and every constant the binders hand out is set (`da_snow_fine_xf`,
  `da_snow_level_xf`, `da_snow_misc`). The sun cascades, recorded on worker threads from `Calculate()` on, read
  them; nothing writes them while they record.
- **Immediate context** (`da_snow_frame_prepare`, after the ground window, before the blades): the windows
  scroll to their places, stamps, refill, pool, composite (below).

## The products every shader can read

`r3\da_snow_sample.h`: `da_snow_depth(xz)`, `da_snow_depth_prev(xz)`, `da_snow_depth2(xz)` (both at once) -
the current snow depth in metres at a world position, trails included, and the previous frame's (motion
vectors). Bind `s_da_snow_fine = $user$da_snow_fine` and `s_da_snow_level = $user$da_snow_level` in the
blender, one shader stage per name and pass. Both are `R32_UINT`: the low half is the current depth, the high
half the previous frame's, unorm over [0, cap]; read with `Load`, so the functions work in any stage.

| Texture | Covers | Texel | Holds |
|---|---|---|---|
| `$user$da_snow_fine` | the fine trail window around the camera (48 / 48 / 72 / 96 / 96 m) | 4.7 cm | season depth (a cubic B-spline of the level fields, thin margin ragged), fine trails, rims |
| `$user$da_snow_level` | the whole level | 1 m (the bake's) | season depth (thin margin ragged at 1 m), coarse trails around the camera |

Constants (standard binders): `da_snow_fine_xf` (texels per metre, texels per side, window centre),
`da_snow_level_xf` (texel = xz * xy + zw), `da_snow_misc` (cap, fine half-size, 1 = depth present, blend band).
Inside the fine window the fine texture is read, blended into the level texture over the last tenth of it.

**The season depth is smooth.** The fine texture carries the level fields' depth as a cubic B-spline over the
4 x 4 texels round each point (`da_sn_depth_smooth`; the composite sums the same terms from group-shared
texels). A bilinear read of the 1 m fields has a slope that jumps at every texel border; lit from a low sun
that drew a quilt of light and dark squares over the snow. The B-spline is smooth to the second derivative.

**Thin snow has a ragged margin.** How patchy the day's snow is comes from the frame's season scalars alone
(`da_sn_patchy`: the open class's melt term against its accumulation, `Mc.x / S.x`, 0 under the calendar winter's
continuous cover, 1 in the thaw, the first snows of November and wherever no open snow is left), so every pass
agrees. In the calendar winter (patchy 0) the depth under 8 cm wears back by up to 3 cm of a sub-metre value
noise (0.9 m and 0.3 m cells; the level texture only the coarse one): `D' = max(D - 0.03 n (1 - D / 0.08), 0)`.
While the snow is patchy:
- the season depth is read at a displaced point (`da_sn_warp`: value noise of 2.6 m and 0.8 m lobes, up to
  0.9 m), so a patch's edge runs in lobes and bays instead of along the smooth contour of the 1 m field (the
  "white decal"); the composite's group-shared window is 7 x 7 texels for it, the level texture reads the
  warped point bilinearly;
- snow under 14 cm moves by up to 2 cm either way by the noise (`0.04 t^2 (n - 0.45)`, `t = 1 - D / 0.14`, what
  it adds fading in over the first 2 cm so the edge does not step), holes and islands near the margin, the patch
  neither losing nor gaining on the whole;
- the thinnest snow thins a little further, a quarter of the way to `0.14 u^2 (2 - u)` (`u = D / 0.14`, same
  value and slope at 14 cm), so its see-through, mottled margin is a band and not a line.

Snow only moves where some lies: bare ground stays bare and the winter cover keeps the bake's depth. The stamp
and pool passes read the same shaped depth (`da_sn_depth_smooth`), so a print sinks into the patch as drawn.

## The level fields on the GPU

The first frame after the level's bake (`da_snow_field()`; again whenever its stamp changes) uploads it, so snow
arriving later in the session costs no frame:
the class weights and factors as baked (RGBA8, `$user$da_snow_weights`, `$user$da_snow_factors`), the
level's snow base: the bake's topmost surface opened with a 3 x 3 square, the terrain wherever that surface is a
cover - more than 0.6 m over the bake's terrain (`R16_UNORM`, `$user$da_snow_base`; 0 is reserved for texels with
no collision under them, `SSnowField::solid`: the backdrop beyond the playable area, whose height is only the
hole-filled ground - the grid does not stand there, the layer paints that backdrop with the surface as its own
base), the shelter of the covers (`R8_UNORM`, `$user$da_snow_shelter`: the cover mask eroded by one texel, so
walls, fences and posts drop out and only a building's or a vehicle's inside is sheltered), the terrain and the
topmost surface themselves (`R16_UNORM`, `$user$da_snow_ground`, `$user$da_snow_top`: the ground window's cover
test, the layer's static surfaces past the window), and the road edges (`R8_UNORM`, `$user$da_snow_road`: the exact
Euclidean distance from a road texel to the road's edge, 0..12.75 m, from the road weight; the wheel tracks
run along its iso-lines). The depth of a texel is the bake's formula, evaluated on the GPU at `D`:

```
D_c = clamp(S_c A_c - Mc_c M_c, 0, cap)    D = sum_c w_c D_c      (classes open, under trees, road, drift)
```

The level texture's depth is multiplied by one minus the shelter texel, the fine texture's by one minus the
window's shelter (below, blended into the level's at the window's edge): no snow lies under a roof, a deck or a
bus, whatever the 1 m fields say. `da_snow_level.cs` writes it over the whole level when what it reads of the season moved (and once more the
next frame, so the previous half settles), otherwise only around the coarse trail window when its trails
changed. What it reads is compared at the precision the stored depth has: `S` and `Mc` to one u16 depth step at
the largest class factor (`cap / 65535 / 2.7`), the compacted residual shares to 1/256, the cap. The
calendar's day stepping every second and the freshness of new snow ageing every frame move the depth by less
than a step for minutes, so they do not recomposite every frame.

## The near ground window (`da_ground.cpp`)

At level load every collision triangle goes to the GPU as `(i0, i1, i2, flags)` with every vertex (the grass
rasterises its own ground from the same upload, `da_ground_cdb()`); `flags` = GMLib material index, passable,
water (liquid and passable), road (`[snow_bake] road` materials; gravel within `rail_dilation` of a `rail`
material lying just above it is railway ballast, not road). The triangles that can carry snow are indexed by
16 m cells on the CPU.

The window is N x N texels around the camera (512 at 0.5 m on Minimum, 512 at 0.25 m on Low and Default, 1024
at 0.25 m above), toroidal (a texel holds the world texel modulo N), moved in steps of 16 texels. A strip the
camera moves into is cleared and rasterised from the triangles of its cells (`da_ground_raster.cs`, one thread
per triangle and 32 x 32 chunk): coverage is conservative (a texel the triangle touches anywhere) and so is the
height (the plane's highest point over the texel, clamped to the triangle), merged with `InterlockedMax`, so
bilinear reads never dip under the real surface; water goes to its own map. `da_ground_resolve.cs` then
rebuilds the strip and an 8-texel apron (`k_apron`, `DA_GR_AP`: the reach of the water level below):

- `Htop` (`$user$da_ground_top`, R32F): the topmost non-passable surface;
- the low map (raster): the lowest surface facing up (under 72 degrees) at each texel centre, the floor under
  whatever stands over it;
- `Hg`: `Htop` opened with a 3 x 3 square - posts, rails, wires and thin props drop out, so snow passes under
  them and they stick out of it; roofs, walls and banks stay. Where a water surface lies more than 5 cm over it,
  `Hg` is that surface: the grid crosses frozen water on it and the shore is no step;
- a cover: `Hg` more than 0.35 m over the texel's floor and more than 0.6 m over the level's terrain (a roof,
  a bus, a block; the road over a culvert is terrain, no cover; where the collision has no floor under a cover
  more than a metre over the terrain, or a lower one the bake found a cover at - a block or a stump with no floor
  in the collision, whose top as the base ran the grid up on to it in sheets and teeth -, the terrain stands in),
  and no node of the level's AI walks on it: a bridge's
  deck, a platform or a flat roof with a ladder is ground, its snow the grid's with its trails (`s_gr_walk`, the
  bake's highest node per metre in the texel's 3 x 3 metres, within -0.5..+0.3 m of the top). The bake decides
  the same at 1 m (`SSnowField::cover`, `shelter`, `da_snow_cover_at` in `da_snow_field.h`), so the level base,
  the CPU's depth (`da_snow_depth_at`: no snow of the sky under a cover; the stock grass under an eave is no longer
  hidden as if snowed in) and the shells take one rule;
- the snow base (`$user$da_ground_base`, RGBA32F, x): `Hg`, the floor under a cover - never lower than 0.3 m
  under the terrain (a cellar's floor ran the grid down along the walls, a moat round the house) nor higher than
  10 cm over it (a plinth's teeth in the doorway). The ground's snow runs on under the eaves and round a bus or a
  wall up to its sides; it never climbs a roof (the old grid did, and hung curtains, torn shards and a ribbon
  along a wire from every eave); the cover's own snow is a shell;
- the cover's floor: beside a small cover (up to some five texels across) the ground round it as the eroded top
  reaches it, ground-like (within half a metre of the terrain: by a house's wall it was a sill or an eave, and the
  grid hung from it in curtains), never more than 5 cm under it (a crate's collision sunk into the ground faced up
  there, and the visible ground showed through the snow in a bare wedge); else the terrain as above. A low block or a
  stump with no floor in the collision is a cover where the bake found one (`bake > 0.25`);
- the shelter (z): from the share of the 5 x 5 texels round it under an **overhang** (`smoothstep(0.35, 0.9,
  share)`): 1 deep under a roof (no snow on its floor, not along its walls either), a part under an eave (the wind
  blows some under), none a texel out from the footprint - a soft drip line, no moat. Only an overhang shelters
  (`s_gr_overhang`, the bake's cover kinds dilated by a metre: 1 an overhang, 0.5 a solid object; an overhang only
  where the cover itself stands 1.5 m over the terrain - crates a metre from a house's eave took the eave's): under a **solid object** - a concrete block, a
  cart, a car, a stump - the ground's snow runs on at its full depth up to its sides and on under it, hidden
  (sheltered like a roof, every block and stump stood in a ragged moat a texel or two wide, the 1 m squares of
  the bake drawn into the snow);
- the opened top itself (w): what the layer's exposure looks up at;
- the cliff taper (y): 0 on a texel next to a jump of the base (more than max(0.35 m, 50 degrees over a
  texel)) or next to no surface - never a jump into a cover's footprint (a raised floor behind a wall is no bank) unless the texel stands over 1.2 m above that cover's base (a walked railway deck over the ground: the grid hung from it in curtains)
  -, 1 half a metre away; under a water surface the texel stores `-1 - taper`, so the snow knows the lake bed
  under it and the taper runs on across the waterline;
- the shore (`$user$da_ground_shore`, RGBA32F): x the opened solid surface under the water too (the bed), y the
  highest water surface within 8 texels (a separable square max over the raw water map), z how far inside that
  reach the texel lies (1 up to half of it, 0 at its end, from the Chebyshev distance to the nearest water
  texel). These are the continuous quantities of the waterline: `da_sn_shore(xz)` takes the bilinear ground
  against the level, so a frozen pond's edge follows the ground's own contour, never the water texels' squares.

`da_sn_base(xz)` reads it bilinear: x the height, y the taper, z the share of the four texels under water;
`da_sn_shelter(xz)` the shelter, the level's past the window.
Beyond the window (and in its last 4 m, blended) the level base stands in; it knows no water. The snow is the
window's only reader: while it draws nothing (May to October) nothing is rasterised, and the first snowy frame
rebuilds the whole window before the snow reads it.

## Trails

| Map | Format | Window | Texel | Holds |
|---|---|---|---|---|
| L0 | R32_UINT, toroidal | 48 / 48 / 72 / 96 / 96 m | 4.7 cm | carved surface (high half), lowest deformer (low half), heights above the snow base, unorm over the cap |
| L1 | R32_UINT, toroidal | 192 / 192 / 384 / 384 / 384 m | 37.5 cm | the same, coarse |
| trod | R8_UNORM, toroidal | L0's | 4.7 cm | how much of the snow prints compacted (the layer's trodden look) |

Every frame, in this order:

1. **Scroll.** L1 clears what it moves into; L0 takes what it moves into from L1 (`da_snow_scroll.cs`), so a
   path walked an hour ago is there again when the camera returns, softened to L1's resolution.
2. **Stamps** (`da_snow_stamp.cs`, one group per stamp, three float4 each): the deformers of `da_snow_stamps()`
   reaching into L0 go into L0, the others inside L1's window into L1. A stamp is the **capsule** its deformer
   swept since the last frame (a disc when it stood still), so a fast leg or body leaves no gaps. Its floor
   (WINTER.md, deformers): a planted foot or a lying body **packs** the loose snow under it to its density and
   sinks until the packed column carries it (`D * rho / pack` over the ground: most of fresh powder, under half of
   January's settled snow for a man, hardly any for a tushkan); a swinging leg or a low body **ploughs** a furrow
   at its own height, `fall` of the snow above it falling back; a crater carves to the ground; never deeper than
   the creature's reach (`sink`). The stamped surface rises from the floor to the snow surface at the radius - a
   print's flat sole and steep walls (`dist^4`), a body's or a furrow's bowl (`dist^2`) -, merged with
   `InterlockedMin` (RotTR packing: the deepest deformer wins, and its sole height is kept for the resolve);
   walls that stopped short of the surface at the radius stood as a step there, a staircase of texels along
   every trail. In deep snow the legs never lift out of it, so the prints join into a trench with the furrows of
   the swinging legs between them. **Partial carving**: under feet and bodies (stamped every frame) a texel sinks
   at most 10 m/s from where it stands; a resting object or a crater (stamped once) carves at once. A crater's
   floor is bare ground, and so is a loose object's: its physics lies on the ground under the snow, so it lies on
   the flat floor of its own hollow (discs of at least 0.3 m radius, dense enough that their floors join - `dist^8`
   walls: sparse, a barrel rolled away left a clover of four bare holes -, the low half 0: no compacted residual) - packed by weight, a rifle went under a drift whole, and a bowl narrower than the grid's
   cells closed over it again; a body packs its hollow but at most 0.4 of each element's height goes under. A
   crater's and a bullet's walls are a print's (`dist^4`: a blast is a hole, not a dimple); a campfire's thaw
   (`sink` > 0 on a crater) is a soft bowl. A deformer more than half a metre under the snow base is under a roof
   or a bridge and stamps nothing; nothing is stamped over water. **On a cover** (its foot at the top of a shell's
   cover in the level's 1 m fields, within 0.8 m under it: a roof, a trailer) a deformer presses the shells' snow
   instead: the **roof map** (`$user$da_snow_roof`, R32_UINT the size of L0, cleared with L0's fresh strips)
   holds how far under the shell's surface each texel is pressed, unorm over the cap, merged with
   `InterlockedMax` - a planted foot three quarters of the roof's snow, a body or a plough half, anything resting
   or a crater nearly all of it, with a print's walls. The level fields decide (the window's raw top read in the
   compute shader was not reliable enough on a roof's slope). Any cover 0.6 m up and more takes them (a trailer, a
   car, a crate pile, a wall top), the foot in the cover's upper half (a boot beside a low cover in the same metre
   still prints the ground); where no shell lies the layer's coat thins over a print and bares the cover under a press
   most of the way down (`1 - smoothstep(0.2, 0.7, depth share)`): bullets and boots mark a wall's or a car's snow too.
3. **Refill**: the snow that falls and the snow a blizzard carries (`SSnowFrame::fall.y`, metres per real second;
   WEATHER.md) raises every carved surface,
   in whole unorm steps; a texel back at the cap is untouched again. The roof map's depressions shrink by the
   same steps.
4. **Pool**: L0 tiles that were stamped hand their mean carved surface and lowest deformer down to L1.
5. **Composite** (`da_snow_composite.cs`, 64 x 64 tiles: the stamped ones of this and the last frame, the
   strips L0 scrolled into, all of them when what it reads of the season moved, as for the level texture
   above): the smooth season depth, the L0 texel resolved against it - carved only where the deformer got
   below the surface, never below the compacted residual `max(2 cm, D * rho_open / 550)` (roads: their
   compaction) - and the rim the displaced snow builds beside a print or a furrow (`0.25 *` the mean depression
   around it, at most a quarter of the depth and 4 cm). On a print's walls the depression is half smoothed over
   the 3 x 3 (1 2 1: the overlapping bowls left a staircase of texels and terraces down a trench's walls); at its
   floor (no deeper neighbour) it keeps its depth, so a rat's or a tushkan's print a texel or two wide does not
   lose a third of it. The share of the snow a print compacted (`depression / 3 cm`) goes to the trod map. The
   season depth is multiplied by one minus the shelter first.

When the season melts the snow the trail maps go; they start empty when it comes back. The stamps (step 2)
and the pool (step 4) read the same smooth season depth as the composite, so a print's depth is the
creature's sink exactly.

## The snow grid

A VS-only clipmap in the G-buffer, drawn after the grass. Rings of 32 x 32 cell tiles, each ring twice the
spacing of the one inside it and with a hole where that one lies; odd vertices slide onto even ones over the
outer band of a ring (geomorph), so ring borders meet without T-junctions. Tiles outside the view, beyond the
reach or inside the finer ring are not drawn (CPU), cells inside the finer ring are culled (`SV_CullDistance`).

| Preset | Cells per ring side | Rings: spacing to the ring's half-size | Reach |
|---|---|---|---|
| Minimum | - | - | no grid: the layer alone |
| Low | 128 | 0.25 m to 16 m, 0.5 m to 32 m, 1 m to 64 m | 64 m |
| Default | 256 | 0.25 m to 32 m, 0.5 m to 64 m, 1 m to 128 m | 128 m |
| High | 192 | 0.125 m to 12 m, 0.25 m to 24 m, 0.5 m to 48 m, 1 m to 96 m, 2 m to 192 m | 192 m |
| Extreme | 256 | 0.125 m to 16 m, 0.25 m to 32 m, 0.5 m to 64 m, 1 m to 128 m, 2 m to 256 m | 256 m |

Against the design (`RESEARCH_BRIEF.md` 0.2, after `snow_tech.md`: the L0 rings from 0.25 m to 24 m / 64 m on
Low / Default, from 0.125 m to 64 m / 128 m on High / Extreme, coarse 1-2 m rings out to the reach): the brief's
"reach (fine rings)" is the reach of the whole doubling ring set from its finest spacing, about 128 cells per
ring side. These rings are larger (128-256 cells), so at every distance the spacing is the design's or finer.
Sub-metre cells out to 128 m on Extreme would take 512 cells per ring, 2.5-3x the triangles of the whole grid
now, and break the 1.7 ms snow budget the grid is part of.

A vertex stands on the snow base raised by the current depth, and only where geometry can show it:

- **Depth precision** (D24S8, near plane 0.05 m): one depth step at view depth z is `z^2 / (0.05 * 2^24)`. The
  grid draws only where the snow is at least three such steps and 4 cm thick (`dmin`). The raise ramps from
  nothing at `dmin` to the whole depth at `2 dmin` and runs on, negative, below `dmin`, where `SV_ClipDistance`
  cuts the triangle: so the clip line falls where the raise is zero, and a patch's edge meets the ground
  without a lip (the first in-game round showed a 1-3 cm vertical cut round every thaw patch). The layer draws
  the thinner snow.
- **No curtains**: the base never climbs a cover (the floor under it stands in), so a house, a bus or a block
  is no cliff; the snow runs to its walls and on under it, where the shelter takes it to nothing. A vertex across
  a jump of the base that remains - a bank, a retaining wall - (more than max(0.35 m, 50 degrees over the spacing),
  slopes steeper than 60 degrees, no surface) is a cliff vertex; every triangle touching one is clipped
  away (`SV_ClipDistance` -1000 at that corner leaves a sliver), cells with three cliff corners are culled
  (`SV_CullDistance`); the height tapers to nothing within half a metre of a cliff. Inside the window a step cuts
  only where the resolve sees a cliff within half a metre (taper under 1): the base steps across a cover's footprint by
  design, and cut there a crate's or a stump's edge tore the grid into a ragged hole. Tree trunks and branches
  (`[snow_bake] trunk`) and the `skip` materials are no snow base: the snow lies on the ground round a trunk,
  never on a sheet stretched over its collision.
- **Frozen water**: where water whose ice (`da_snow_ice.h`, the rule `water.ps` and `CEnvironment::ice_at`
  share) is past half its cover lies under a vertex, the grid is a sheet on the water surface raised by the
  snow on the ice, or by `0.04 m + 3 depth steps` over bare ice (the water mesh bobs by 1/30 m), and the
  deferred light shades the ice like the ground. Across the shore nothing switches per vertex: the base and
  the depth both blend by the pond's water share, so the bank's snow and the sheet are one surface along the
  waterline, and a bank is no cliff toward the water below it (up to a metre). **The waterline is continuous**
  (`da_sn_pond`): within 8 window texels of the collision's liquid faces the share is the bilinear ground (the
  bed under the water too) against the level of that water (`da_sn_shore`), 0.5 where the ground lies 3 cm
  under it, ramping over 4 cm of height, plus a micro-relief noise of +-1.2 cm (lobes of 1.3 m and 0.4 m) that
  lets the line meander on a flat shore and keeps it nearly straight along a steep bank; beyond that reach, or
  where the window holds no liquid face at all, the baked field's pond takes over (`da_sn_flood`: ground below
  a frozen pond's surface within two field texels of it, one field texel deciding first - and no more than 0.2 m
  under it: lower ground lies past a wall or a rim, water standing in a house; the path outside it froze into a raised
  sheet that took no prints), the share blending
  from one to the other by the window's reach, so they meet without a seam. The window's own water share - the
  bilinear of its 0.25 m water texels - drew the edge of the ice as a staircase of 1 m runs and 0.25 m risers,
  seen at a grazing angle as rectangular notches (the March church pond, three rounds); it now decides only
  over open water, whose line the water mesh draws anyway. The neighbours of a bank vertex count at least the
  pond's level, so the ice meets the bank where the bank rises out of it. In the ice months the bank beside
  frozen water keeps its triangles (no per-vertex clip,
  its snow never raised below the ground) and a vertex at the water's edge never sinks under the top of the
  pond's ice (`da_sn_ice_top`: `level + 0.04 m + 3 depth steps`): clipped per vertex, the sheet ended on
  straight cuts through the grid's cells, a row of teeth along the shore, and sunken bank vertices pulled it
  under the water mesh's ice in a dark band. **The flat shore** just above the waterline (no more than 0.2 m under the top of
  the ice - lower ground lies past a wall or a rim; by a house whose floor carries a liquid face the snow was lifted to
  that level in a flat sheet torn at its edges, and the window's waterline gives no bank there either) keeps its
  triangles and
  the same floor: dry ground that may lie under the top of the ice - the bank's snow base (a conservative raster,
  half a window texel's rise over the surface on a slope) up to `da_sn_bank_slack` (that rise plus 2 cm for the
  render mesh) over it. The water mesh bobs by 1/30 m and draws its ice over anything lower; in the thaw, with a
  few centimetres of snow on the bank, it drew a stepped grey band along the grid's triangles under the edge of
  the bank's snow strip, seen from standing height and changing with the bob (the March church pond). The pixel
  shader cuts per pixel instead: the ice front (open water beyond, `water.ps`); over the bank, where the bank's
  own snow is under 3 cm (the sheet ends at the waterline, the layer draws the bank's cover) and where a
  triangle bridging from the ice to the bank floats more than 8 cm over the bank's snow or the ice's (it hid the
  bank behind its straight edges like a lid); never on that flat shore (cut there, the water mesh's own ice
  showed through over the ground in grey slivers and bands). The pixel shader's water test is the same
  continuous share. Deep snow on the ice runs on into the bank's. Open water is thin snow (clipped), the grid
  sloping down into it. Within the ground window only (64-128 m round the camera by preset); beyond it
  `water.ps` draws the ice under the same rule.
- **Depth bias**: the vertex is pushed 8 depth units toward the camera, so it wins over the ground it meets
  at the fade.
- **Motion vectors**: the previous position uses the previous frame's depth and the previous camera's fade.

The grid writes position, the snow base's slope over 2 m as its normal (taps at +-1 m on rings finer than a
metre, its own cells beyond; one-sided at a roof edge as the layer takes it), its own sky term (lower under the
crowns) and material code 31; the albedo
target is masked off, so the albedo of what lies under it stays for the layer's see-through. Where the raised
surface covers the sky at a crest, a pre-pass has put snow albedo under the sky pixels first. **The grid casts
into the two near sun cascades** (`da_snow_grid_shadow.vs`, the same rings within 28 m round the camera, depth
only, recorded on the cascades' worker threads, the tiles culled by each cascade's box): the walls of a trail, a
crater's rim and a drift's lee cast their shadows - without it a trench in the sun read flat, lit as the open
snow. The second cascade takes the tiles past 12 m only (nearer, every pixel is the first's) in cells of every
other vertex: the band where the two blend reads both, where the trails' shadows broke into noise from 15 m with
the first alone. Every pass with per-target write masks (the grid, the sky pre-fill, the layer's
write-back) sets independent blend: without it D3D takes target 0's mask for all targets, the grid wrote its
grey into the albedo and the write-back kept the ground's blue under white snow (yellow snow edges, roofs and
far slopes in the first in-game round). The grid's vertex and pixel shaders read the same fields (the window,
the level base, the water field): `CBlender_Compile::r_dx11Texture` binds a texture at the slot of every stage
that declares it. The merged constant table keeps one slot per name, the last merged stage's (the vertex
shader's), so before that the pixel shader read zeros - its shore and ice-front tests saw no water anywhere.

## Snow shells

The snow on everything standing over the terrain is generated geometry (`da_ground.cpp`, once per bake, after
the level fields' upload), not the height field: a heightfield cannot end at an eave, and the old grid hung
curtains, torn shards and stacked sheets there.

- **Faces**: the collision's solid faces looking up (under 60 degrees), their middle 1.8 m over the bake's terrain
  (a roof, a trailer, a bus; a block, a cart or a stump lower than that is a solid object, its snow the layer's
  cover over the grid's running up to its sides - a shell on it tore into sheets over the grid's snow) and every
  corner 0.3 m over it, not under another surface (the 1 m top field first, a ray up
  through the collision to be sure: a floor under a roof); not a deck the AI walks (the bake's nodes, as the
  window's cover), not a block under a metre with no floor under it (a ray down: the window keeps its top as the
  base, and a shell there stood on the grid's own snow); welded at 1 cm, a face welded into a sliver dropped (its
  zero-length edge counted as open, a pit in the middle of a roof).
- **Islands**: the faces joined by their edges; an island carries a shell when it is broad and smooth - at least
  1.5 m2, its area at least 0.15 m per metre of open edge, at most one metre of fold (over 20 degrees) per square
  metre. Small, narrow and crumpled islands (sills, fence tops, crates, a wreck's roof panels) keep the layer's
  cover on their own drawn surface: shells there showed the collision's simplifications - stacked sheets on a
  bus, a triangle of snow in a fence's gap.
- **Open edges**: edges of one face only - but a T-junction (a face's edge along two of its neighbour's, the
  panels' seams the collision leaves unwelded: its middle on another open edge running the same way) is none; left
  open, the snow sank to the roof along every seam. Hashed by 1 m cells sampled along each segment (a long
  diagonal edge's bounding box held tens of thousands).
- **Mesh**: faces near an open edge (within 0.6 m) are cut into triangles of at most 0.3 m (a lattice of up to 24
  per side; a 20 m eave of 12 drew its rounding as a wedge two metres wide); an edge's points depend on the edge
  alone (its count from its length and its distance to an open edge, its points from its two welded corners in
  the edge's own order, to the last bit) and a row of the face's lattice snaps onto them, so faces sharing an edge
  share its vertices - no cracks (degenerate faces are dropped, the panels' T-junction seams closed, the corners
  and edges canonical - the jagged cracks in the shells of concrete blocks). A face far from every open edge stays whole. Chunks of up to 65536 vertices by
  32 m cells, 16-bit indices from each chunk's first vertex; past 2 M vertices a face keeps the layer's cover (the
  log says how many).
- **Per vertex** (in parallel): the normal - a corner's or an edge point's summed over every face round it,
  whichever chunk drew it (a chunk's seam kept two heights on a ridge) -, the distance to the nearest open edge
  (capped at 0.6 m) and the way away from it; a vertex under another surface (an eave over a lower roof) counts as
  an edge with no way away.
- **Drawn** (`da_snow_shell.vs/.ps`, after the grid, chunks in the view within 120 / 160 / 200 / 260 / 320 m by
  preset): a vertex stands on its face raised by the roof's snow - the open class's formula for the face's own
  slope (all of it to 32 degrees, none past 60) with the level texel's noise and melt, 0.85 of it (the wind
  sweeps an exposed roof) - rounded off toward the open edges by a quarter circle of about the depth's radius,
  never steeper than 40 degrees from the edge (no fin of snow on a narrow support); the normal follows the
  rounding. A centimetre over the face and pushed 8 depth units toward the camera, so it wins over the roof at its
  edge. **Prints on a roof**: the pixel shader reads the roof map (trails, above) and marches the view ray on into
  the shell's snow to the pressed surface (three steps against the map), taking its position, the print's slope
  and its depth - only ever farther (`SV_DepthGreaterEqual`, the position `noperspective centroid`), so the early
  depth test stays. Written like the grid (code 31, albedo kept) with a hemi of 1, the shell's mark (the grid's is 0.95 at
  most); the layer shades a shell pixel as deep snow. A pixel whose view ray runs on well past its supporting face
  before it meets the scene (the depth copied before the shells) has nothing under it and is dropped: the snow
  lies on a ruin's rafters, not on a slab over them. Built again on the next frame if the bake came before the
  collision mesh.

Escape: 6437 of 95130 faces in 537 of 14698 islands, 370 k vertices, 703 k triangles, 15.3 MB, built in 0.4 s;
the minimum preset draws no grid and no shells.
Past the reach a roof keeps the layer's cover (a couple of pixels thinner).

## The snow layer

Two full-screen passes at the rain apply's slot (stencil: geometry only): `da_snow_layer.ps` shades into the
scratch targets (accumulator, generic 1), `da_snow_apply.ps` writes back the pixels it changed (a G-buffer
target cannot be read while written). Per pixel:

- **Skipped**: foliage codes 27-30 (blades carry their own snow), the first-person view, the lake bed (a pixel
  more than 3 cm under the water surface of the window). The first-person view is told apart by the hardware
  depth, as the local lights do (`da_hud_light.h`): the HUD is rasterised into the top slice of the depth range
  (`r2_hud_depth_limit`), and the layer reads a copy of the depth (`$user$depth_copy`, taken right before it,
  since the depth stays bound for the stencil test). Under MSAA there is no single-sampled copy: everything
  nearer than 1 m (the longest first-person item) is skipped.
- **The ice sheet**: a grid pixel where the pond's continuous share (`da_sn_pond`, the grid's own) passes one
  half is bare ice under the season's snow on it (below), flat as the ice whatever the triangle did. The bank's
  own snow runs on over the ice and thins out by three quarters of the share, wet as the bank's (snow on
  melting ice is soaked and greyer), so the snow's edge at the waterline is the line's meander with a thin
  see-through rim, not a cut and no tone step. A grid pixel standing above the bank's own snow (the snow on the
  ice running on over the bank, or the ice's edge over a flat shore) is as deep as the snow on the ice there,
  eased in over 3 cm of that rise, and beside a frozen pond what its thin snow lets through is that ice: the
  albedo the G-buffer holds under the grid is whatever lies behind it along the view. The grid is never
  "something above the snow" that gets only a dusting: it is the snow surface.
- **A shell pixel** (code 31 with the shell's mark, a hemi of 1; told by its height over the ground's snow, a
  coarse ring's chord over a hollow and the snow on a pond's ice running onto the bank lost their trails and ice)
  is deep snow: no trails, no pond, no ruts; its normal is the shell's plus the pack's own.
- **Exposure**: only what lies above the fall line shelters a pixel - an eave, a bridge deck; a bank, a kerb or a
  wall foot beside it does not (the first round counted a bank as cover, which bared the inner side of every road
  cut and hillside path: the brown streaks of January). The fall line climbs upwind by the flakes' slant (at most
  27 degrees); five taps of the window's opened topmost surface (wires and posts removed): straight over the pixel
  at 0.3 m (0.4 of the weight) and a cross of four at 1.2 m, 0.35 m out (the soft edge of an eave). Past the window
  the depth's shelter does it. Only taken where snow can lie at all.
- **Coverage**: `smoothstep(0.5 cm, 3 cm, D) * exposure * slope` (full to 50 degrees, nothing past 70; a pixel
  on the snow base takes the slope of the ground over 2 m where its normal map is steeper, so the bumps of a
  ground texture strip no snow); thin snow goes first on dark ground (down to 0.65 of the cover under 4 cm on
  black asphalt); on onset and melt days the class cover keys over the same noise the depth uses, on a ragged
  0.6 m margin; roads covered kerb to kerb from December to February (`cover.z` = 1, decision D5). A static surface
  standing above the snow surface and open to the sky - a roof past the shells' reach, a crate's or a slab's top,
  a sill, a wreck's roof - carries the snow a roof keeps (the same formula as the shells) as a cover of its own:
  the pixel itself stands a cover's height over the snow (eased in from 15 to 30 cm: not the ground a body or a
  boot lies on), lies open to the sky (at most 6 cm over the raster's highest point, under it by no more than a
  rim's or a rail's 60 cm - a truck's bed lies that far under its collision box -, 15 cm over it, and the slope's run
  over a texel) and faces up, by its own normal as well. (Gated by its
  texel's top over the texel's base instead, a stump's roots, a crate's slats and a trailer's bed beside its rails
  turned white in the window's 0.25 m squares: sheets with toothed edges.) Anything else above the snow
  surface - corpses, creatures, tree crowns - gets at most a light up-facing dusting (decision D8; winter trees
  carry their own snow). The step from what lies under the snow to what stands in it is 4 cm (over 15, a rifle in
  its hollow took the snow's cover). What stands over the snow facing sideways or down - a wall, a trunk, a fence,
  a body's flank - leaves the pass before the season's, the slope's and the exposure's lookups (a good share of
  the screen), and an up-facing top over the snow skips the slope's and the exposure's (neither changes it).
- **See-through**: thin snow lets the ground through by the two-way diffuse transmission of a scattering slab,
  `T = 1 / (1 + 0.135 d_we / r_e)`, opacity `1 - T^2` (water equivalent `d_we` in mm, grain radius `r_e` in mm
  by age and wetness): a centimetre of old snow is about half see-through, 4 cm hide the ground, 17 cm of
  January snow are opaque (the first round's `d_we / (d_we + 10 r_e)` still showed 12 % of the ground through
  it: blotchy plaster). A thinning margin breaks into clumps before the ground shows through whole (noise of
  25 cm and 8 cm, each faded out near the pixel footprint; not by the pixel's albedo, and no finer grain - the
  17 cm / 5 cm mottle of round 1 read as a white frost speckle on gravel and asphalt). Up close (fully within
  a few metres, gone by about 15 m: `1 - smoothstep(3 mm, 12 mm, footprint)`) the cover itself - the opacity
  times the lay, which both ramp over the last centimetres - is cut on the same noise, stretched over the whole
  range of the cover, with a transition of about a centimetre and 15 % of the veil kept as the clumps'
  see-through fringe: a patch's rim is a lace of granular snow and bare ground with crisp edges, its mean cover
  the veil's, instead of a milky veil fading over a hand's width (round 2 at B up close); its course stays the
  noise's, so no decal contour returns. The grid's own snow is deep; the thinner snow it keeps at a pond's shore
  breaks up the same way, eased in from 5 cm to 3 cm (switched at 4 cm, the see-through of the deep side and
  the opaque clumps of the thin side drew tone steps along that depth, at a pond's shore and where the grid
  meets the layer's thin snow). Roads `R = 1`.
- **Look**: the class colour of the month (linear keys, `albedo_open/road/trees`; texels the bake left without
  a class - its water - give their share to the open class: normalised to nothing there, the snow of a pond's
  bank over those texels had no colour and drew a black notch, the December limit of earlier rounds), wetter
  and darker on south
  faces in the thaw, water-logged in hollows late in the thaw, roadside dirt and litter under trees, fresh snow
  brighter while it falls; **prints** at 0.8 of the snow's albedo (the trod map: compacted, never dark grey -
  the floor of a print is never thinner than the compacted residual); **roads**: compacted white-grey
  snow with the wheel tracks of a lane each way, 1.0 m and 2.6 m in from either edge (`$user$da_snow_road`),
  broken along the road, 8 % greyer, a little dirtier and glossier, 2 cm grooves. The snow's albedo carries a
  slight cool cast (`DA_SNOW_TINT`, luminance 1) that `da_snow_light.h` takes back off the sun and the lights:
  what only the sky lights - shade, overcast - stays blue-grey, sunlit snow stays neutral white. A subtle wet
  dark ring of ground (x0.72) round melting patches, only on ground facing up and only where the snow itself has
  melted (not where a slope or a shelter keeps the layer off deep snow); it keeps the ground's own gloss and halves its normal map's
  relief (more gloss lit every ridge and pebble of asphalt and gravel white). Gloss from the state roughness
  (fresh 0.8, settled 0.7, old 0.55, wet 0.45, slush 0.4; compacted 0.5) times 0.012 (`DA_SN_GLOSS`) and the
  sheen left on melting snow (`da_sn_sheen`), only the meltwater in a hollow and the wet ruts mirroring (times
  0.25). The light model's sun specular is a broad lobe, five times the gloss times the light twice over
  (`combine_1.ps`): under a bright low sun the old 0.05-0.11 drew a smooth glossy streak along every swell toward
  the sun and a white hot spot round its mirror point - the "plastic" snow. Snow's own specular is weak and rough;
  what flashes on it is single grains (the glints).
- **The pack's own texture** (`da_sn_pack_detail`, also on the snow on the ice): world-anchored value noise,
  each octave on its own turned lattice, no time term, every scale faded out between nine and three pixels
  per cell (`da_sn_fade`: no shimmer under TAA, no glitter), every octave skipped where it adds nothing.
  Relief: the settled, wind-pressed undulation of the pack (lobes of 3.4 m and 1.15 m, about 3 degrees; fresh
  snow drapes it at 0.7, wind 0.3 more, roads and prints at 0.3), the ablation hollows of melting snow (0.45 m)
  and, close up, the granular surface (2.2 cm to about 9 m, 0.75 cm to about 3 m at 1440p: fine on fresh snow,
  coarse corn snow in the thaw). Albedo: +-4 % following the relief (the hollows darker), and on old and wet
  snow (`0.5 old + 0.8 wet + slush`, from 0.15) the dust and litter it gathers: a thin grey-brown film (about
  5 % in March), more in the hollows and in sub-metre blotches of irregular outline (up to about 11 %),
  greyer meltwater in the ablation hollows, the coarse grains +-4 % close up.
- **Normal**: the snow surface's own: the slope of the snow base over 2 m (one-sided where the two sides
  disagree by more than 40 degrees, at a roof edge or a wall top) - the collision mesh is flat per triangle,
  and its facets taken at 6 cm drew the stripy light and dark bands of the first round; a grid pixel reads it
  back from the grid's normal instead of four base lookups - plus the depth's own slope (on the fine texture a
  forward difference over one 4.7 cm texel, eight loads for the depth and its slope together: its smooth
  season and the prints; on the level texture a central difference over one texel either way, twelve loads),
  plus subtle wind ripples on loose snow (12 cm, fading below the pixel footprint, and 1.4 m, both a few
  degrees), crust pits on old snow close up and the grooves of the wheel tracks. Even a thin film fills the
  ground's millimetre relief: the ground's normal map is gone once a quarter of the cover lies (it caught the
  snow's white as a crinkled web over asphalt).
- **Glints** (`da_sn_glint_cell`): the sun mirrored by single grains. A cell of about one and a half pixels at
  any distance (Bowles & Wang: discrete 1.3x cell steps from 0.6 mm, two levels blended across a step) carries
  the facet of its best placed grain, tilted at random about the snow's normal (a normal slope spread of 0.35);
  it flashes while the sun's half vector falls within the cell's tolerance of it. With M grains in the cell (grain
  0.4 mm on fresh snow to 1.3 mm on old) the chance grows M times and the grain's share of the cell shrinks M
  times - capped at two cells in a hundred: grown on, the far cells flashed nearly all at once, faintly, and TAA
  smoothed them into a glossy streak. So near, few bright sparks; far, as few, each as bright as its share of
  the sun allows; most faint, a few bright (a heavy-tailed brightness). World-anchored, no time term: as the eye
  moves other grains light up and the snow twinkles; standing still nothing changes. Only while the sun shines
  through (`da_sn_sun.w`), on clean dry loose snow (none on wet, trodden or dirty snow), to 150 m. A flashing pixel
  turns its normal to the sun's half vector, takes the glint code (1, `DA_MTL_GLINT`) and the flash as its gloss
  (`DA_SNOW_GLINT_SCALE` = 100 times the snow's diffuse light at gloss 1); `r__snow_glint` scales it (QA).
- **Material**: code 31 where the snow dominates (`k > 0.5`), 1 on a glint; the puddle flag cleared under snow
  (the puddle passes leave it alone). The packed material word is copied bit for bit, never re-packed from a float.

**The ice sheet.** A grid pixel over frozen water: bare ice (`da_ice_albedo`: black ice dark grey-green over the
water column, linear 0.03-0.08, with a 0.3 m mottle, greyer blotches where air and snow froze into it, pale
crack lines of a coarse 3 m net along a few long runs and a fine 0.9 m one in rare patches (most borders stay
sound: no net of tiles), fading below the pixel footprint, streaks of wind-blown snow dust along the winter wind
over a part of it; gloss at most 0.06, so it reads as dark ice and not as a pale mirror), under
the season's snow on the ice (`da_ice_snow`: its depth key where the cover key reaches, patchy while partial,
tapering at a patch's margin) with the same see-through as on the ground and the pack's own texture. Bare ice
is lit as a stock glossy material, the snow on it as code 31.

## Yellow snow

Where a man or a dog relieved himself: rarely, and never amid the houses. At level load (`stains_build`, da_snow.cpp)
one tree or stump in twenty in the open (`Environment::wind_veg_trees`, fewer than 60 overhang texels in the 41 x 41 m
round it) gets a stain 0.35-0.9 m off its root; one in a hundred of the 4 m cells along a static object's foot and one
in two hundred along a building's (on the outskirts: fewer than 250 overhang texels round it) get one 0.25-0.65 m off
it; stains stand at least 6 m apart (escape: about 150). They go to the GPU by 8 m cells (`$user$da_snow_stain_cells`,
first << 12 | count; `$user$da_snow_stains`, x z radius seed) and the layer paints them within 80 m
(`da_sn_stain_at`), after the reference photos: a core some 20-60 cm, 1.3-2.6 times longer than wide, its edge ragged by
three octaves of noise down to a couple of centimetres, ochre and darker toward where the stream struck; a pale lemon
halo round it; a scatter of 2 cm drops to one side; a small brown melt hole in about half of them. Fresh snowfall
covers them (`da_sn_layer2.x`), no glint on them. `r__snow_stats` lists the eight nearest.

## Lighting

`da_snow_light.h`, called by `lmodel.h` for code-31 and code-1 pixels (`da_is_snowy`) with the slice-0 LUT result: 35 % of the diffuse
light goes through UE's SubsurfaceBxDF (wrapped diffuse, back scatter, forward in-scatter; opacity 0.85,
tinted (0.80, 0.90, 1.00)), the rest stays on the stock lobe; the sun's regular shadow covers both. The result
is divided by `DA_SNOW_TINT`, so the cool cast of the albedo stays only in what the sky's hemisphere lights. A
very narrow specular lobe (`N.H` within half a degree, 16x) lights the glint pixels (code 1) alone: on plain snow
it drew the sun's mirror image as a glossy blotch wherever the surface ran smooth. The sky's mirror image
(`hmodel`'s specular) keeps on snow only as much as its gloss over 0.15 says - the meltwater in a hollow keeps it,
plain snow hardly any (the sun in the sky's cube drew a glossy streak toward a low sun), a glint 4 %. `combine_1.ps` white-balances
a snow pixel (the sun and the lights by the sun's colour, the sky term by its own) and then gives the sky term
a blue hemisphere's cast (`DA_SNOW_SKY_TINT`, luminance 1): overcast January snow lit at about #C8C9CB, shade
bluish, sunlit snow neutral. Its specular takes the light without the albedo's division.

## Presets

`xrRender_sync_preset_derived` -> `da_snow_apply_preset(i)`, applied at the next frame:

| | Minimum | Low | Default | High | Extreme |
|---|---|---|---|---|---|
| ground window | 512 @ 0.5 m | 512 @ 0.25 m | 512 @ 0.25 m | 1024 @ 0.25 m | 1024 @ 0.25 m |
| L0 | 1024 (48 m) | 1024 (48 m) | 1536 (72 m) | 2048 (96 m) | 2048 (96 m) |
| L1 | 512 (192 m) | 512 (192 m) | 1024 (384 m) | 1024 (384 m) | 1024 (384 m) |
| grid | - | 0.25-1 m, 64 m | 0.25-1 m, 128 m | 0.125-2 m, 192 m | 0.125-2 m, 256 m |
| shells' reach | 120 m | 160 m | 200 m | 260 m | 320 m |
| grid in the near sun cascade | - | 28 m | 28 m | 28 m | 28 m |

GPU memory: ground window 9 / 9 / 9 / 36 / 36 MB (the shore texture 4 / 16 MB of it), trails (L0 + fine +
trod + L1) 10 / 10 / 25 / 40 / 40 MB, the level fields 15 B per m^2 of the level box (39 MB on a 1.6 km level),
the collision mesh 16 B per triangle and 12 B per vertex (about 50 MB on the largest level).

Measured on the stand in the first round (Extreme, January on l01_escape, GPU timers `snow_prep` +
`snow_grid` + `snow_layer`, static / turning camera): DirectX 12 at native 2560x1440 1.46 / 1.62 ms, within
the 1.7 ms target; with DLSS Balanced (1484x834) 0.57 / 0.63 ms on DirectX 12 and 0.67 / 0.73 ms on DirectX
11. The polish keeps the work per pixel where it was (the exposure is five base lookups instead of five to ten;
the compaction and road reads are four loads each, the road's on road pixels only) and makes the fine
composite cheaper (49 texel evaluations per 8 x 8 group instead of eight per texel). From May to October the
passes cost nothing (skipped). Round 1 of the polish stood at its limit (1.64-1.70 ms native, the layer
1.33-1.40 ms); round 2 takes the grid pixels' base slope from the grid's normal and the depth's slope from
shared texels: 0.07 + 0.24-0.26 + 0.89-0.95 = 1.20-1.27 ms native on DirectX 12 (layer -0.45 ms, grid +0.02
ms), 0.62 ms on DirectX 11 with DLSS Balanced. The pack's texture and the continuous waterline (the same view,
January at A looking down, native 2560x1440): DirectX 12 0.07 + 0.26 + 1.19-1.23 = 1.52-1.56 ms with the first,
ungated variant of the texture; the final one on DirectX 11 0.08 + 0.25-0.26 + 0.93-0.95 = 1.27-1.29 ms.

The 1.6.0 snow round (shells, covers, trails, glints; Extreme, January, the rookie village at noon, DirectX 12
with DLSS on the QA rig): `snow_prep` 0.08 + `snow_grid` 0.20-0.22 (the shells 0.01-0.02 of it) + `snow_layer`
0.52-0.56 ms, the grid in the near sun cascade 0.1-0.15 ms of `sun` (`r__snow_shadow 0` against 1). After the
code review's round (the shells' lattice to 24, the second cascade at full detail): 0.07 + 0.25 (the shells 0.05,
their depth copy included) + 0.44-0.51 ms, the grid's shadows 0.24-0.28 ms of `sun` - about 1.1 ms of snow.
The optimisation round, the layer's stages measured by cutting it short (`r__dbg_snow 11..16`, the same views):
the bare pass 0.15 (its two full-screen passes, the reads and the write-back), the water's gates +0.05, the
season and the coverage +0.12, the pack's albedo +0.09, the normal's detail +0.05, the glints +0.03; the walls'
early out takes the layer to 0.36-0.40 ms, the shadows' cascade boxes and the second cascade's ring to within the
noise of `sun`: 0.07 + 0.22-0.25 + 0.36-0.40 = some 0.7 ms. The same frames without snow (July) cost 2.7 ms more
(the grass 0.83 ms alone): a winter frame is cheaper than a summer one.

## Known limits

- Shells come from the collision mesh: where it simplifies a roof (a single plane over corrugated sheets) the
  shell follows the plane; crumpled or small covers keep only the layer's cover (no thickness).
- The prints on a roof live in the near trail window alone: the roof map is not handed down to L1, so they go
  when the camera moves 48-96 m away (L0's window by preset) and do not come back.
- The shells are built on the render thread when the bake is uploaded (some 0.4 s at the level's first frame).

- The deferred ice sheet reaches as far as the ground window (64-128 m by preset); beyond it `water.ps` draws
  the same ice, front, snow patches and cracks, but lit by the water's forward light: a slight change of
  brightness can show where a large frozen lake crosses the window's edge.
- The snow on the ice takes no prints (the ice is visual only, decision D7: a creature wading through a frozen
  pond goes through it).
- At the k00_marsh church pond's flat shore the last run of the shore pass still showed faint (2-3 %, an
  albedo difference) tone steps along a few lines of the bank's snow in March, up close. The grid's thin/deep
  snow switch at 4 cm, eased over 3-5 cm after that run (see-through above), is their likely cause; not yet
  seen in game.
- The flat shore the grid keeps beside frozen water is lit as snow (code 31) where little or none lies on it: a
  bare strip of gravel there loses its normal map's relief.
- No snow footstep sounds, no walkable ice, no winter sun path (decision D14).

## QA

- `r__snow_stats`: the state of the passes (layer and depth on/off, the level fields, both trail windows,
  the last frame's stamps near/far, composite tiles, level texels, grid tiles), the frame's snow scalars and
  state (fresh, settled, old, wet), and the 16 stamps nearest the camera (`stamp <i>: x z y radius kind`).
- `r__snow_glint <0..4>` (QA, not saved): the glints' strength, 1 as built (a glint's lobe held at 10, some sixty
  times the snow's light: brighter flickered under the temporal resolve and bloom); `r__snow_shells 0|1`,
  `r__snow_shadow 0|1` (QA): the shells and the grid in the near sun cascades (for an A/B).
- `r__dbg_snow` (not saved): 1 paints every path of the layer in its own colour - the grid green (yellow-green
  where it runs on as the ice's snow over a bank), the shells violet, the ice sheet blue (cyan where the baked field's pond decides
  it beyond the window's water), the
  layer's snow yellow, its thin snow and wet rings orange, the lake bed magenta; whatever stays in its own
  colours is the ground the layer left alone or the water pass's ice; in every mode over 0 the prints the roof
  map holds are red, on a shell or not. 4 paints the snow base's height in bands of 5 cm, 5 the snow's depth.
- `r__snow_window_probe x z dx dz count` (QA): the ground window's texels along a line read back from the GPU next
  frame - the raw top, the snow base, its taper, shelter, opened top and the fine depth there. 3 paints the window's texels over everything: red where the snow tapers to
  a cliff, green a cover (its base dropped under its opened top), blue a texel standing over its lowest neighbour
  by a cliff's step - where a ragged hole beside an object comes from. 2 does the same with the grid's
  per-pixel cuts off, so a pair of frames shows what the cuts remove. 11..16 cut the layer short after its stage
  (the reads, the base and depth, the water, the season and coverage, the albedo, the normal's detail), for their
  costs in `r__gpu_log`.
- `qa_snow_state`, `qa_snow_shot`, `qa_snow_fire_melt`, `qa_snow_probe` (WINTER.md): the frame and the
  precipitation, bullets, campfires' thaw, the contact gate; `qa_season_pin january`, `qa_season_year 60`
  (SEASONS.md) sweep the snow through the year.

To look at: January on escape at Extreme - the road covered kerb to kerb in white-grey compacted snow with
faint wheel tracks, no bare streak on any hillside path or road cut, a grid edge at a wall without a curtain,
posts and rails sticking out of the snow, no snow under the bus stop roof, no stripy bands on a slope under a
low sun, only rare pixel glints in the sun and none in the shade; a dog running through a drift (a row of 4-5 cm
prints, not a trench), a stalker walking (6-7 cm prints one by one), a tushkan (barely a dent); a grenade
crater (bare floor in steep walls, lobed); bullet channels and a load of shot; a campfire's pit dusted, then a
bare pit in a smooth thaw bowl when lit; prints on a roof's shell; a stump, a cart and concrete blocks standing
in full snow with no ragged holes, their own snow on top; a frozen pond in January (snow over the whole ice, the bank running on to it
without a step) and in March (dark grey-green ice with cracks and dust, snow patches, the bank's snow meeting the
ice along a smooth meandering line at any distance, no notch); the snow's own texture - a gentle undulation in
the sun, a granular surface up close, the thaw's snow greyer with a little dirt in its hollows; March and November
(ragged, see-through patch edges with no lip, a faint wet ring, no speckle on the asphalt); TAA/DLSS on and a
fast camera turn over the grid (no smearing); MSAA 4x and the plain G-buffer (`r3_gbuffer_opt 0`) for the layer.
