# Snow: the renderer side (1.6.0)

Volumetric seasonal snow on every surface of a level, at every distance, pressed by everything that walks
on it. The season day `D` (`SEASONS.md`) and the world side (`WINTER.md`: the level bake, the snow constants
of every frame, the deformer stamps, the snowfall, the ice) decide how much snow lies where; this page is how
the renderer turns that into a picture. Data: `SEASON_DATA.md` (`configs/dead_air_x64_seasons.ltx`).

Code: `src/Layers/xrRender/da_ground.{h,cpp}` (the collision mesh on the GPU, the near ground window),
`src/Layers/xrRender/da_snow.{h,cpp}` (everything else), `src/Layers/xrRender/da_snow_gpu.h` (their private
GPU helpers). Shaders (`shaders/r3`): `da_ground_*`, `da_snow_*`; the public headers `da_snow_sample.h` (the
current depth for any shader), `da_snow_light.h` (the lighting of snow pixels) and `da_snow_ice.h` (the ice
of the season, shared with `water.ps`).

## What is on screen

| Piece | Where | What |
|---|---|---|
| snow grid | G-buffer, after the grass | real geometry: a camera-centred clipmap raised by the snow depth, near (to 64-256 m by preset); over frozen water a sheet of ice and the snow on it |
| snow layer | full screen, after the rain apply | every opaque pixel at every distance: coverage, the look of the month, thin-snow see-through, material code 31 |
| trails | compute, every frame | every creature, corpse, prop and blast presses the snow; snowfall fills it back |
| lighting | `lmodel.h` -> `da_snow_light.h` | subsurface wrap for code 31, the cool cast of shade, glints |

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
level's snow base: the bake's topmost surface opened with a 3 x 3 square (`R16_UNORM`, `$user$da_snow_base`; 0
is reserved for texels with no collision under them, `SSnowField::solid`: the backdrop beyond the playable
area, whose height is only the hole-filled ground - the grid does not stand there, the layer paints that
backdrop with the surface as its own base), and the road edges (`R8_UNORM`, `$user$da_snow_road`: the exact
Euclidean distance from a road texel to the road's edge, 0..12.75 m, from the road weight; the wheel tracks
run along its iso-lines). The depth of a texel is the bake's formula, evaluated on the GPU at `D`:

```
D_c = clamp(S_c A_c - Mc_c M_c, 0, cap)    D = sum_c w_c D_c      (classes open, under trees, road, drift)
```

`da_snow_level.cs` writes it over the whole level when what it reads of the season moved (and once more the
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
- `Hg` (`$user$da_ground_base`, x): `Htop` opened with a 3 x 3 square - posts, rails and thin props drop
  out, so snow passes under them and they stick out of it; roofs, walls and banks stay. Where a water
  surface lies more than 5 cm over it, `Hg` is that surface: the grid crosses frozen water on it and the
  shore is no step. The snow lies on `Hg`;
- the cliff taper (y): 0 on a texel next to a jump of the surface (more than max(0.35 m, 50 degrees over a
  texel)) or next to no surface, 1 half a metre away; under a water surface the texel stores `-1 - taper`, so
  the snow knows the lake bed under it and the taper runs on across the waterline;
- the shore (`$user$da_ground_shore`, RGBA32F): x the opened solid surface under the water too (the bed), y the
  highest water surface within 8 texels (a separable square max over the raw water map), z how far inside that
  reach the texel lies (1 up to half of it, 0 at its end, from the Chebyshev distance to the nearest water
  texel). These are the continuous quantities of the waterline: `da_sn_shore(xz)` takes the bilinear ground
  against the level, so a frozen pond's edge follows the ground's own contour, never the water texels' squares.

`da_sn_base(xz)` reads it bilinear: x the height, y the taper, z the share of the four texels under water.
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
2. **Stamps** (`da_snow_stamp.cs`, one group per stamp): the deformers of `da_snow_stamps()` reaching into L0
   go into L0, the others inside L1's window into L1. The stamped surface is the deformer's bottom plus
   `(dist / radius)^2 * sharp`, merged with `InterlockedMin` (RotTR packing: the deepest deformer wins, and
   its sole height is kept for the resolve). **A foot or a body sinks no deeper than its creature's sink**
   (`SSnowStamp::sink`, WINTER.md: a rat 1.2 cm, a dog 4-5 cm, a man 6.5 cm, a boar 8.5 cm, a pseudogiant
   15 cm) under the snow surface, whatever depth its sole reaches: the snow carries the rest. **Partial
   carving**: under feet and bodies (stamped every frame) a texel sinks at most 3 m/s from where it stands, so
   nothing cuts through deep snow in one frame; a resting object or a crater (stamped once) carves at once. A
   crater's floor is bare ground. A deformer more than half a metre under the snow base is under a roof or a
   bridge and stamps nothing; nothing is stamped over water.
3. **Refill**: the snow that falls (`SSnowFrame::fall.y`, metres per real second) raises every carved surface,
   in whole unorm steps; a texel back at the cap is untouched again.
4. **Pool**: L0 tiles that were stamped hand their mean carved surface and lowest deformer down to L1.
5. **Composite** (`da_snow_composite.cs`, 64 x 64 tiles: the stamped ones of this and the last frame, the
   strips L0 scrolled into, all of them when what it reads of the season moved, as for the level texture
   above): the smooth season depth, the L0 texel resolved against it - carved only where the deformer got
   below the surface, never below the compacted residual `max(2 cm, D * rho_open / 550)` (roads: their
   compaction) - and the slight rim the displaced snow builds beside a print (most of a print is compacted,
   not displaced: `0.2 *` the mean depression around it, at most a quarter of the depth and 3 cm). The share
   of the snow a print compacted (`depression / 3 cm`) goes to the trod map.

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
- **No curtains**: a vertex across a jump of the base (more than max(0.35 m, 50 degrees over the spacing),
  slopes steeper than 60 degrees, no surface) is a cliff vertex; every triangle touching one is clipped
  away (`SV_ClipDistance` -1000 at that corner leaves a sliver), cells with three cliff corners are culled
  (`SV_CullDistance`); the height tapers to nothing within half a metre of a cliff. Tree trunks and branches
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
  a frozen pond's surface within two field texels of it, one field texel deciding first), the share blending
  from one to the other by the window's reach, so they meet without a seam. The window's own water share - the
  bilinear of its 0.25 m water texels - drew the edge of the ice as a staircase of 1 m runs and 0.25 m risers,
  seen at a grazing angle as rectangular notches (the March church pond, three rounds); it now decides only
  over open water, whose line the water mesh draws anyway. The neighbours of a bank vertex count at least the
  pond's level, so the ice meets the bank where the bank rises out of it. In the ice months the bank beside
  frozen water keeps its triangles (no per-vertex clip,
  its snow never raised below the ground) and a vertex at the water's edge never sinks under the top of the
  pond's ice (`da_sn_ice_top`: `level + 0.04 m + 3 depth steps`): clipped per vertex, the sheet ended on
  straight cuts through the grid's cells, a row of teeth along the shore, and sunken bank vertices pulled it
  under the water mesh's ice in a dark band. **The flat shore** just above the waterline keeps its triangles and
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
surface covers the sky at a crest, a pre-pass has put snow albedo under the sky pixels first. The grid is not
drawn into the shadow cascades. Every pass with per-target write masks (the grid, the sky pre-fill, the layer's
write-back) sets independent blend: without it D3D takes target 0's mask for all targets, the grid wrote its
grey into the albedo and the write-back kept the ground's blue under white snow (yellow snow edges, roofs and
far slopes in the first in-game round). The grid's vertex and pixel shaders read the same fields (the window,
the level base, the water field): `CBlender_Compile::r_dx11Texture` binds a texture at the slot of every stage
that declares it. The merged constant table keeps one slot per name, the last merged stage's (the vertex
shader's), so before that the pixel shader read zeros - its shore and ice-front tests saw no water anywhere.

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
- **Exposure**: only what lies above the fall line shelters a pixel - a roof, an eave, a bridge deck; a bank,
  a kerb or a wall foot beside it does not (the first round counted a bank as cover, which bared the inner
  side of every road cut and hillside path: the brown streaks of January). The fall line climbs upwind by the
  flakes' slant (at most 27 degrees); five taps of the snow base: straight over the pixel at 0.3 m (0.4 of the
  weight) and a cross of four at 1.2 m, 0.35 m out (the soft edge of an eave). Only taken where snow can lie
  at all.
- **Coverage**: `smoothstep(0.5 cm, 3 cm, D) * exposure * slope` (full to 50 degrees, nothing past 70; a pixel
  on the snow base takes the slope of the ground over 2 m where its normal map is steeper, so the bumps of a
  ground texture strip no snow); thin snow goes first on dark ground (down to 0.65 of the cover under 4 cm on
  black asphalt); on onset and melt days the class cover keys over the same noise the depth uses, on a ragged
  0.6 m margin; roads covered kerb to kerb from December to February (`cover.z` = 1, decision D5). Anything
  standing above the snow surface - props, corpses, fences, tree crowns - gets at most a light up-facing
  dusting (decision D8; winter tree cards carry their own snow).
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
  dark ring of ground (x0.72) round melting patches, only where the snow itself has melted (not where a slope
  or a shelter keeps the layer off deep snow); it keeps the ground's own gloss and halves its normal map's
  relief (more gloss lit every ridge and pebble of asphalt and gravel white). Gloss from the state roughness
  (fresh 0.8, settled 0.7, old 0.55, wet 0.45, slush 0.4 - wet corn snow and slush are coarse and matte, not a
  plastic sheen; compacted 0.5, ruts 0.3, only the meltwater in a hollow mirrors), times the sheen left on
  melting snow (`da_sn_sheen`: 1 down to 0.5 over the wet and slush states; the meltwater pool keeps all of it):
  the light model's sun specular is a broad lobe added on top of the albedo, and on the thaw's grey snow
  (0.6-0.75) it lifted the result back to a fresh snow's white.
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
- **Glints**: rare (about one pixel in 300 toward the sun's mirror direction, one in 3000 elsewhere), about a
  pixel wide at every distance (Bowles & Wang: discrete 1.3x cell steps, blended across a step), only while
  the sun shines through (`da_sn_sun.w`, the sun colour's luminance: none under an overcast sky), none on wet,
  trodden or dirty snow, no time term: a glint cell turns its normal to the sun's half vector and
  `da_snow_light.h` lights it; the light pass shadows it. Its gloss stays the snow's (a glint with full gloss
  mirrored the sky into the shade).
- **Material**: code 31 where the snow dominates (`k > 0.5`); the puddle flag cleared under snow (the puddle
  passes leave it alone). The packed material word is copied bit for bit, never re-packed from a float.

**The ice sheet.** A grid pixel over frozen water: bare ice (`da_ice_albedo`: black ice dark grey-green over the
water column, linear 0.03-0.08, with a 0.3 m mottle, greyer blotches where air and snow froze into it, pale
crack lines of a coarse 3 m net along a few long runs and a fine 0.9 m one in rare patches (most borders stay
sound: no net of tiles), fading below the pixel footprint, streaks of wind-blown snow dust along the winter wind
over a part of it; gloss at most 0.06, so it reads as dark ice and not as a pale mirror), under
the season's snow on the ice (`da_ice_snow`: its depth key where the cover key reaches, patchy while partial,
tapering at a patch's margin) with the same see-through as on the ground and the pack's own texture. Bare ice
is lit as a stock glossy material, the snow on it as code 31.

## Lighting

`da_snow_light.h`, called by `lmodel.h` for code-31 pixels with the slice-0 LUT result: 35 % of the diffuse
light goes through UE's SubsurfaceBxDF (wrapped diffuse, back scatter, forward in-scatter; opacity 0.85,
tinted (0.80, 0.90, 1.00)), the rest stays on the stock lobe; the sun's regular shadow covers both. The result
is divided by `DA_SNOW_TINT`, so the cool cast of the albedo stays only in what the sky's hemisphere lights. A
very narrow specular lobe (`N.H` within half a degree, 4x) lights the glint cells. `combine_1.ps` white-balances
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

## Known limits

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
  the last frame's stamps near/far, composite tiles, level texels, grid tiles) and the frame's snow scalars.
- `r__dbg_snow` (not saved): 1 paints every path of the layer in its own colour - the grid green (yellow-green
  where it runs on as the ice's snow over a bank), the ice sheet blue (cyan where the baked field's pond decides
  it beyond the window's water), the
  layer's snow yellow, its thin snow and wet rings orange, the lake bed magenta; whatever stays in its own
  colours is the ground the layer left alone or the water pass's ice. 2 does the same with the grid's
  per-pixel cuts off, so a pair of frames shows what the cuts remove.
- `qa_snow_state` (WINTER.md): the frame and the precipitation; `qa_season_pin january`, `qa_season_year 60`
  (SEASONS.md) sweep the snow through the year.

To look at: January on escape at Extreme - the road covered kerb to kerb in white-grey compacted snow with
faint wheel tracks, no bare streak on any hillside path or road cut, a grid edge at a wall without a curtain,
posts and rails sticking out of the snow, no snow under the bus stop roof, no stripy bands on a slope under a
low sun, only rare pixel glints in the sun and none in the shade; a dog running through a drift (a row of 4-5 cm
prints, not a trench), a stalker walking (6-7 cm prints one by one), a tushkan (barely a dent); a grenade
crater (bare floor, a slight rim); a frozen pond in January (snow over the whole ice, the bank running on to it
without a step) and in March (dark grey-green ice with cracks and dust, snow patches, the bank's snow meeting the
ice along a smooth meandering line at any distance, no notch); the snow's own texture - a gentle undulation in
the sun, a granular surface up close, the thaw's snow greyer with a little dirt in its hollows; March and November
(ragged, see-through patch edges with no lip, a faint wet ring, no speckle on the asphalt); TAA/DLSS on and a
fast camera turn over the grid (no smearing); MSAA 4x and the plain G-buffer (`r3_gbuffer_opt 0`) for the layer.
