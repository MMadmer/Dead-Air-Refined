# Ghost of Tsushima as the vegetation reference (1.6.3)

Ghost of Tsushima (Sucker Punch; the Director's Cut on PC) draws dense grass, pampas, flowering meadows and wind-swept
forests at a high frame rate. Besides DOOM Eternal (the GPU-driven geometry, `DX12_RENDERER.md`), it is the mod's
reference for everything the wind and the plants do. This file is what its renderer does with them, read from its data
and its executable, what the mod took in 1.6.3 and what it did not, and why. The plants themselves: `GRASS.md`,
`SHRUBS.md`, `TREES.md`; the wind: `WEATHER.md`.

## How it was read

From its files and, in the 1.6.3 round 4, from the running game (started by its owner; its memory read, no code put
in it).

- **Archives.** `*.dsar` is a container of LZ4 blocks around a PSARC; the PSARC's manifest lists its entries' names,
  matched to its table by the MD5 of each name (the manifest's lines split by position did not line up with the
  table). The packages (`.xpps`, `KCAP` chunks) hold the shaders, the materials and the tuning sets.
- **Shaders.** 20 043 DXIL blobs (10 386 pixel, 7 346 compute, 2 282 vertex, 29 tessellation), shader model 6.6,
  bindless; a draw's or a dispatch's data is a ByteAddressBuffer (`t1 space1`) read at an offset from the root
  constants (`cb14`), so every input is a raw load at a fixed offset and a shader's parameters are read from its
  disassembly (`dxc -dumpbin`). The grass's compute and vertex shaders, the proxy sets' culling and the imposters'
  passes were read through.
- **The executable** (IDA): the tuning tables with their help strings (the wind, the gusts, `GRASS_PARAMETERS`,
  `TREE_WIND_PRESET`), the frame's job names (`JOBAK_*`) and the GPU buffers' debug names.
- **The render parameters, live.** 353 of them (the wind, the gusts, the terrain's grass sheen, the shadows' LOD):
  names, types and defaults in tables of the image, the frame's blended values in the render-parameter manager's
  array of 32-byte entries (value, index, type, blend mode, set flag), read from the process. Units are centimetres.
- **The tuning sets.** A package's data type is indexed by records of `{u64 offset, u64 type hash, u64 field hash}`;
  the field's record (`{u16 type, u16 kind, ...}`) ends with its value, the offset counted from the package's data
  section (the u32 at `0x28` of the `KCAP` header). A field hash is the polynomial `h = h * 101 + c` over the name in
  lower case, from 0 (the executable's field descriptors pair each name with the next field's hash - read them by the
  hash, not by the neighbouring name). The grass kinds are in `core_tsu.sprig.xpps` (45), the tree presets in
  `core_common.sprig.xpps` (37); a set stores only the fields it changes.

## The wind

One wind for the world (`WindDirection`, `WindSpeed`, `WindZVelocity` for set pieces) and two scrolling gust maps over
the landscape:

- the **gust map**: `GustMapScale` (its size, cm), `GustDensity` (how many gusts in it), `GustTravelSpeed` (its scroll,
  a multiple of the wind's speed), `GustAmplitude` (the gusts' peak strength, a multiple of the speed),
  `GustVarianceRate` (how fast the map itself changes);
- the **high-frequency map**: `GustMapScaleHighFreq`, `GustTravelSpeedHighFreq`, `GustAmplitudeHighFreq`,
  `GustMinimumHighFreq`, `GustMapRotationHighFreq` (turned against the big one so the two never line up) and
  `GustScaleRangeHighFreq` / `GustScaleAmountHighFreq`: "the range over which the high frequency gusts increase to give
  a more noticeable look at a distance" and how much - the far field's gusts are scaled up on purpose.

Live, in the open world by day: `WindSpeed` 1, `GustAmplitude` 1, `GustMapScale` 2500 (25 m), `GustVarianceRate` 0.1;
the high-frequency map `GustMapScaleHighFreq` 500-1000 m, `GustMapRotationHighFreq` 260, `GustAmplitudeHighFreq` 3000,
`GustMinimumHighFreq` 0.5, `GustScaleRangeHighFreq` 25-75 m, `GustScaleAmountHighFreq` 5.

Cloth takes it by `ClothWindFactor`; particles by wind spheres (`PARTICLE_WIND_SPHERE`); the GPU keeps wind records
(`Wind Records In/Out/CPU`) for the systems that read it back. Polycuts pin the clocks and offsets
(`WindTimeOverride`, `GustOffsetOverride`), so a cutscene's grass moves the same every time.

The mod's wind is the same idea in its own terms (`WEATHER.md`): one travelling gust field with eddies of 8 and 4 m
ridden downwind (`da_wind_turb`), the stock wave groups, the motors for boots, shots and blasts. 1.6.3 took the
distance boost (below, "Grass").

## Grass

**Data.** `GRASS_PARAMETERS` per grass kind: the procedural placement (`PROC_SCALE_*`, `PROC_ANGLE_*`,
`PROC_JITTER_MAX`, `PROC_ALIGN_TO_TERRAIN`, `PROC_FACE_DOWNHILL`), the blade (`BASE_WIDTH`, `BASE_TILT`, `BASE_BEND`,
`BEND_POINT`, `BEND_INVERSION_FREQ`, `HEIGHT_SCALE/MIN/MAX`, `SIDE_CURVE`, `HEIGHT_CLUMPING`, `HEIGHT_VARIANCE`), the
clumps (`CLUMP_SIZE`, `CLUMP_DENSITY`, `CLUMP_ALT_COLOR_FREQUENCY`, `FACE_CLUMP_DIRECTIONAL`, `FACE_CLUMP_OUTWARD`, and
facing the wind, the slope, a river's current or the waves), the shading (`SPEC_SCALE`, `GLOSS_SCALE`,
`TRANSLUCENCY_SCALE`, `POROSITY`, `AO_SHORT`, `AO_TALL`), the wind (`WIND_SCALE`, `WIND_SCALE_HIGH_FREQ`,
`ANIMATION_SPEED`), the interaction (`DISPLACEMENT_BEND_SCALE`, `DISPLACEMENT_TWIST_SCALE`) and the far blades
(`DISTANT_SIZE`, `DISTANT_FADE_LIMITS`, `DISTANT_HEIGHT_ADJUST`). Clumps are Voronoi cells (`VORONOI_SIZE`, `_EDGE`,
`_MAX_ROTATE`, `_SOFTNESS`).

The shipped kinds (45; the median of those that set the field): `WIND_SCALE` 0.65 (a few stiff kinds 0.1),
`WIND_SCALE_HIGH_FREQ` 1 (0.03 for the stiff ones), `DISPLACEMENT_BEND_SCALE` and `_TWIST_SCALE` 1, `CLUMP_SIZE` 0.6-1.2 m,
`PROC_SCALE` 0.8-1.2, `PROC_ANGLE` +-30 to +-180 degrees, `DISTANT_FADE_LIMITS` 6-11 m for 16 kinds and 120-170 or
150-200 m for six, `DISTANT_HEIGHT_ADJUST` pairs such as 1.375-2.75 and 0-1.5 (by its name a far blade's height, for
the thinned lattice; the mod's pixel floor widens its far blades to the same end, `GRASS.md`).

**Generation.** A compute pass per tile of 256 x 256 lanes writes the blades of the view - tiles four to a batch,
their outputs in ping-pong buffers (eight sets of 65 536 blades) - as structures of arrays: `posGrass_uHeight`,
`sWidth_gTilt_gBend_grassk`, `normalFacing_rSideCurve_uWind`, `uVoronoi_uWindPrev_uClumpColor_uHash`. The previous
frame's wind travels with the blade (`uWindPrev`) for the motion vectors. A pass turns the counts into indirect draws
(`Grass Indirect Args`, `Blade Count`). The tiles are culled on the CPU by the occlusion buffer (`JOBAK_OcclusionBuffer`)
before they are dispatched.

**Level of detail.** A blade is a curve drawn as a strip: 15 vertices within 12.5 m x `LodAdjust` (the grass quality,
0..2), 7 past that, nothing past 210 m x `LodAdjust`, faded out between 190 and 210 m on a random threshold per blade.
The lattice ranks are nested: a far tile's blades are a subset of the near tile's, so the density thins without a
blade jumping. The grass casts into the sun's cascades in passes of its own (a high and a low level of detail,
`INCLUDE_GRASS_IN_SHADOW_MAP`, a `shadow_proxy` set for the far cascades).

**Wind and interaction.** The wind is noise evaluated in the shader per blade (a 3D Perlin of position and time), the
gust maps on top, scaled per kind. Characters, horses, arrows and effects press it through a displacement texture of
256 x 256 texels over 100 m around the camera, sampled bicubically (`PsysGrassDisplacement`, `FX_PARTICLE_DISPLACEMENT`),
which bends and twists the blades by the kind's scales.

**Look.** The blades bend away from the camera a little (`BendAwayFromCamera`: from above, a meadow shows its blades'
sides rather than their cut ends); the ground under the grass has its own specular multipliers
(`TerrainGrassSpecNearValue` below 1 "to create the illusion of shadow under nearby grass", `TerrainGrassSpecFarValue`
"to maintain a grassy appearance when grass geometry is not drawn due to LOD", blended between `FadeStart` and
`FadeEnd`, both scaled by `LodAdjust` with the grass's reach). Live: near 1 (no darkening by the camera), far 0, from
0 to 50 m at `LodAdjust` 1.

**The mod against it.** The mod's blade grass (1.6.0, `GRASS.md`) is built the same way and needed no rebuild: tiles
generated on the GPU into bins drawn indirectly, cubic Bezier blades of 13, 7 and 3 vertices by distance, a quincunx
lattice whose density halves per doubling of the distance (nested), Voronoi clumps with their pull, facing and colour,
the previous frame's force per blade for the motion vectors, the HOM and the frame's depth culling the tiles and lanes
(GoT's grass tiles go by the CPU's occlusion buffer), a push field of 64 m around the camera for the trampling (GoT's
displacement texture), casters of their own per sun cascade. 1.6.3 took:

| GoT | 1.6.3 | Measured |
| --- | --- | --- |
| Per-tile data culled once on the CPU | `TileSummary` (`da_grass_blades.cpp`): what a tile's slots hold - per community the most presence x cover, the height range - worked out the first time the tile is walked and kept by level and place; the day's densities multiplied in each frame | the calc task's walk over every 2 m slot of a 300 m reach: -73 % of its profiler samples; the tiles, levels and lanes the same |
| `GustScaleRangeHighFreq` / `GustScaleAmountHighFreq` (live 25-75 m, 5) | `grass_gust_k` (`da_grass_gen.cs`): from 25 to 75 m the gusts and lulls round the mean lean and a tongue's lean go to 2.5 times theirs (GoT's 5 is on a map faint by the camera; the mod's gusts are whole there) | the field from 25 to 75 m 6-8 % more motion frame to frame (`grass_gust.lua`, the wind reseeded per mode), the near within 4 %; `grass_gen` +0.01-0.02 ms |
| `TerrainGrassSpecNear` / `Far` (live 1 and 0, 0-50 m) | `deffer_impl_flat.ps`: the soil's gloss under the blade cover whole by the camera and gone at `d2` (the preset's 25-45 m: Extreme's `d1`/`d2` of 12/45 m are GoT's 12.5 m blade LOD and 50 m sheen fade) | a far slope against the sun keeps the grass's matte look; an A/B at the village with the wind frozen (`veg_cost.lua`, noon and `qa_veg_hour = 18`): the ground's mean brightness +0.1 of a level near the camera, the rest within the frame-to-frame noise |

`BendAwayFromCamera` was not taken: the mod's blades already turn their flat side toward their lean
(`grass_lean`), and a camera-dependent bend would differ between the view and the casters - the shadow would not follow
the blade (the rule in `TREES.md`: shadows follow the sway).

## Trees and shrubs

**Wind presets.** `TREE_WIND_PRESET` per species: three levels - trunk, branch, subbranch - each with
`STIFFNESS`, `SWAY_DISTANCE`, `SWAY_WAVINESS`, `SWAY_SPEED` and, for the branches, `SWAY_ANGLE_MAXIMUM`; the materials
switch `TreeWind`, `TreeWindTrunk`, `TreeWindBranch`, `TreeWindSubBranch`, `TreeWindHighFreq` (the high-frequency gust
map on the leaves), `FaceWindDirXy`, `InteractiveDeformation`, and for the coast `TreeWindUseOceanWaves` /
`TreeWindClampVertsToOcean`. The vertex shader bends a vertex by the levels its painted weights name: a sway per level
with its own speed and waviness, limited per level. The shipped presets (37, typical values): stiffness 0.8 at every
level; sway distance 1-20 (trunk), 0.5 (branch), 0.5-0.6 (subbranch); waviness 0-0.2, 0.1, 0.1; speed 10 (trunk),
10-25 (branch), 20-40 (subbranch); the angle limit 0.5-1 (branch) and 0.25-1 (subbranch).

How the vertex shader (`vs_SetMaterial_techMoving`, the trees' moving technique) uses them, per level:
`angle = (1 - STIFFNESS) x [min(lean, ANGLE_MAXIMUM x 45 deg) x saturate(|wind| / 66 m/s) + (0.1 + 0.25 x gust) x
SWAY_DISTANCE x 45 deg x sin(SWAY_SPEED x t + the instance's random phase - 0.01 x SWAY_WAVINESS x cm from the pivot)]`
(the subbranches' sine 0.3 + 0.3 x gust, times the vertex's share of its stem); the trunk leans with the wind times
the square of its height share, with a slow ring of a few hundredths of 45 degrees. A preset's speed is varied per
instance by whole steps up to a fifth of it, so neighbours do not swing together. In other words a static lean
capped per level, plus a sine at the level's speed whose amplitude grows with the gust: with a time in seconds the
subbranches ring at 3-6 Hz by up to 16 degrees in a full gust, the branches at 1.6-4 Hz by 2-8 degrees.

**Proxy sets.** The scattered vegetation and props are proxy sets, culled on the GPU: a job culls them on the CPU
first (`JOBAK_ProxySetCull`, `JOBAK_ProxySetPreRender`, `JOBAK_ProxySetFillStreams`), then the GPU against the frame's
depth (`Proxyset Cull Inst Count`, `Proxyset Streams Inst Count`, `Max Depth Cull Scratch`, `Cpu Max Depth Cull 0/1`)
into indirect draws (a command signature of `DrawIndexed`), with fades of their own (`Proxyset Fade`, `Proxyset Temp
Fade`).

**Imposters.** Far trees are imposters (`OBJECT_IMPOSTERS`, `VIEW_IMPOSTER`), baked with a dilation pass
(`FX_DILATE_IMPOSTER`) and their own shadow and occlusion (`FX_COPY_IMPOSTER_SHADOW_AND_AO`); the far shadow cascades
use separate low-detail alpha textures (`*_ll_shadow`).

**Shadows.** Four cascades (`CSM0`-`CSM3`, each with its own polygon and normal offsets); a mesh is drawn into the
shadow map no finer than a level of detail (`LodDistanceMinInShadowMap`) and can be left out of the low-resolution
cascades altogether (`SKIP_DRAW_IN_LOW_RES_SHADOW`); screen-space directional shadows add the contact detail the maps
lack (`DirectionalScreenSpaceShadows`, `ScreenSpaceShadowHardWeight`/`Decay`/`Contrast`).

**The mod against it.** The trees (1.6.0, `TREES.md`) already had the GPU cut with the depth pyramid (DOOM Eternal's
and GoT's way), billboards of eight views baked from the tree itself (the imposters), chain bends per axis cached a
frame in the bend buffer. 1.6.3 took:

| GoT | 1.6.3 | Measured |
| --- | --- | --- |
| Proxy sets culled on the GPU against the frame's depth | The shrubs through the trees' GPU cut, its third phase (`CUT_KEEP`, `da_tree_cut.cs`, `da_veg.cpp` `emit`): an element keeps the CPU's draw and leaves out only what its vertex shaders threw away - out of the view by its own bent place, behind the depth pyramid with every tree in it, the wood under its wire rule. `r__shrubs_cut` (QA) | behind a near poplar on Cordon the view had run 107 000 sprays, 95 000 wood segments and 95 000 fruit groups for a few thousand pixels: `shrubs` 3.68 -> 2.18 ms and 1.94 -> 1.06 ms in the bushy headings, the bend pass 0.44 -> 0.24 ms |
| Coarser, cheaper casters in the far cascades (`LodDistanceMinInShadowMap`, `_ll_shadow`) | A cluster's or a blob's quad whose leaf cells are three shadow texels and more casts round clumps of the same Beer-Lambert cover (`da_shrub_cluster_shadow.ps`): a high crown's sunflecks are the sun's pinhole images. Then the shader itself: the part's constants worked out once in its vertex shader (the rim's phases, the clumps' chance, the needle bristles), a texel past the outline's widest reach dropped before any work, one inside its least taking no outline, the rim's waves as powers of the texel's direction instead of an arc tangent and three sines, the ramps from the quad's derivatives | `sun` 5.63 -> 4.68 ms in that heading (the textured cells were seven million texels of the near map); the shader's rewrite 0.2-0.3 ms more a heading |
| `DISTANT_SIZE` / `DISTANT_FADE_LIMITS` (a far blade drawn by its mean) | A fruit group under four pixels across drawn as its mean cover in the bunch's mean colour (`da_shrub_bloom.ps`), dithered over the band by the pixel's noise | a fruiting stand's far cost was mostly every fruit and stalk tested at each pixel |
| A tree preset's speed varied per instance by up to a fifth (its vertex shader) | `shrub_axis_turn_of` (`da_shrub_common.h`): a plant's limbs ring up to a fifth faster than its species' axis, by its seed (still a whole number of cycles per 64 s, so the clock's wrap stays seamless) | two bushes of a kind side by side no longer drift in and out of step at one frequency; the ring's size is the same |

Not taken:

- **Harmonic sway per level** (`SWAY_SPEED`, `SWAY_WAVINESS`): the mod's limbs bow to the gusts passing their base and
  ring a little at their own frequency (`shrub_axis_turn_of`); a harmonic sway as the whole motion swung every twig
  three to fourteen times a second in any breeze and read as twitching (`SHRUBS.md`). Against GoT's live presets
  (round 4): its limits match the mod's (a branch 22-45 degrees, a subbranch 11-45; the mod's axis and chain 35
  degrees each), its static lean grows with the wind as the mod's mean lean does, its ring grows with the gust as the
  mod's does, but its subbranches ring at 3-6 Hz by up to 16 degrees where the mod's twigs ring by a few degrees
  (0.1 of the push over the root of the axis's frequency) - the motion the mod dropped as twitching. The wave along a
  branch (`WAVINESS` 0.1: a tenth of a radian a metre) would not show on a ring of a few degrees.
- **The distance boost of the gusts for the crowns**: a tree's bend is its own (cached per slot, its shadow from the
  same bend); one walked toward from 120 m would calm down by half as the player nears it. In GoT the boost is the
  grass's and the leaves' high-frequency map, not the trees' presets.
- **Octagons for the clusters and blobs** (tried): the quad less its empty corners left a sixth of the view's and the
  maps' pixels out, but doubled the vertices, and the cascades' parts are a few texels each - the vertex shader (the
  bend, the sway, the cover) costs more there than the corners' texels, dropped at once by the rewritten shader: the sun
  0.1-0.2 ms a heading dearer.
- **The two populations' candidates gathered side by side** (tried, `veg_wait`): no gain to measure, and the waiting
  worker may take up an unrelated long task (the main view's walk) and finish the pick later than alone.
- **The leaves' place a frame back for the motion vectors**: measured at some 0.4 ms a heading (the shrubs' and the
  trees' leaves within 15 m, their flutter, bend and sway once more), kept - a leaf's tip moves a pixel a frame near the
  camera and the temporal resolve smears it without. It is worked out only while something reads the vectors (TAA, an
  upscaler, frame generation, the ray traced histories, the motion blur: `da_motion_vectors_active`); with none the
  leaves, the clusters, the wood, the blades and the bend pass skip it.
- **Screen-space shadows**: a general feature, not the vegetation's; the mod's contact detail on the plants comes from
  their own casters and the crown light.
- **The ocean's waves** in the trees' wind: no sea in the Zone.

## Cost: 1.6.3 against the engine before it

The engine of 040c592b2 (the version bump, before any of the above) against 1.6.3 on the rig, each with its own
shaders, run in turn: `veg_cost.lua` at the save's eye, a July noon, Extreme, 1080p, the RTX 3070 Laptop. GPU ms per
heading (`r__gpu_log`), the frame rate (`r__fps_log`). The final build, Cordon village, the bushes ahead (h0) and
behind the poplar (h90):

| Renderer, heading | frame | shrubs | sun | FPS |
| --- | --- | --- | --- | --- |
| DX11 (bright sun), h0 | 17.45 -> 16.43 | 1.95 -> 1.03 | 4.34 -> 4.19 | 57.0 -> 60.3 (+6 %) |
| DX11 (bright sun), h90 | 23.39 -> 19.90 | 3.76 -> 1.94 | 5.53 -> 4.40 | 42.5 -> 50.0 (+18 %) |
| DX12, h0 | 19.29 -> 17.73 | 1.95 -> 1.07 | 5.37 -> 4.97 | 49.0 -> 55.9 (+14 %) |
| DX12, h90 | 24.70 -> 21.89 | 3.62 -> 2.01 | 6.34 -> 5.21 | 40.3 -> 45.4 (+13 %) |
| Vulkan, h0 | 19.51 -> 18.20 | 1.89 -> 1.07 | 4.20 -> 3.95 | 51.0 -> 52.7 (+3 %) |
| Vulkan, h90 | 22.47 -> 20.12 | 3.53 -> 2.01 | 4.94 -> 3.95 | 44.4 -> 48.6 (+10 %) |

The final build (round 4, 1dd9aa3e6, the live values' gusts and sheen in), the same rig and views, the old engine run
again beside it (`veg_cost.lua`, two headings, one round each), FPS:

| Renderer | h0 | h90 | Both headings | Frame (ms, summed) | Shrubs (ms) | Sun (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| DX11 | 52.2 -> 59.4 | 43.4 -> 49.1 | +14 % | 41.76 -> 36.56 | 5.69 -> 3.00 | 9.96 -> 8.71 |
| DX12 | 52.4 -> 56.3 | 40.3 -> 45.6 | +10 % | 43.32 -> 39.41 | 5.43 -> 3.06 | 11.54 -> 10.12 |
| Vulkan | 48.6 -> 50.9 | 43.1 -> 46.9 | +7 % | 42.37 -> 38.86 | 5.53 -> 3.17 | 9.24 -> 7.74 |

And at four headings a quarter turn apart, DX11, the new build run twice for the noise (round 4 with the limbs'
per-instance ring), FPS summed over the headings, GPU ms summed:

| Place | FPS | Frame | Shrubs | Sun | Grass's look (`Grass-Quality.py`: cover, detail, luma) |
| --- | --- | --- | --- | --- | --- |
| Cordon village | 222.3 -> 232.3 / 225.9 (+2-5 %) | 73.09 -> 69.13 / 70.74 | 7.11 -> 4.09 / 4.11 | 18.58 -> 17.10 / 17.12 | +0.5 %, -1.1 %, -0.6 % |
| The meadow (`qa_meadow`) | 228.0 -> 242.2 (+6 %) | 69.56 -> 65.58 | 6.45 -> 4.47 | 14.20 -> 13.09 | +1.5 %, +3.1 %, +0.5 % |

The village's heading behind the poplar alone: 43.9 -> 49.9 / 48.7 FPS. The new build's two runs differ by 3 % in the
FPS and by 4.9 %, 2.7 % and 1.2 % in the grass's cover, detail and luma: the old engine's grass against the new is
within that.

The shots of both match within the frame-to-frame difference of one build (the wind's sway: the bark's edges on the
swaying trunk, the leaves against the sky, the blades), the crowns' shadows on the ground included. The first two
rounds (the shrubs' cut, the crowns' sunflecks, the grass's tiles, the far gusts, the sheen, the fruit) at four headings
a quarter turn apart, DX11, the old engine's two runs averaged:

| Place, heading | frame | shrubs | shrubs' bend | sun | FPS |
| --- | --- | --- | --- | --- | --- |
| Cordon village, behind a poplar (h90) | 22.81 -> 20.07 | 3.69 -> 1.94 | 0.47 -> 0.34 | 5.44 -> 4.59 | 43.6 -> 49.5 |
| Cordon village, bushes ahead (h0) | 17.36 -> 15.92 | 1.94 -> 1.03 | 0.31 -> 0.31 | 4.34 -> 4.37 | 57.4 -> 62.1 |
| Cordon village, four headings summed | 73.8 -> 68.7 | 7.15 -> 4.05 | 1.10 -> 0.90 | 18.66 -> 17.74 | 220.9 -> 233.8 (+6 %) |
| The meadow (`qa_meadow`), four headings summed | 68.8 -> 66.3 | 6.39 -> 4.50 | 0.63 -> 0.55 | 14.17 -> 13.67 | 230.1 -> 239.2 (+4 %) |

The village's street lamps burn by day: the bushes in their shadow maps cost the old engine's (`lights` 2.8 -> 2.7 ms
summed), the bend pass bends them too. The grass, its generation and the trees stay within a few hundredths (the boosted
gusts and the sheen are free); the shrubs' frame pick on its worker (`shrubs_cpu`) is some 0.05-0.25 ms a heading dearer
for the cut's routes (Cordon summed 2.12 -> 2.49 ms, the meadow 2.58 -> 2.9-3.65, noisy); it runs on a worker, and the
render thread waits for it only when it is late (`veg_wait`, 0.1-1.2 ms a heading in 1.6.3). The shots of both match but
for the wind's sway. The local lights' casters: `tools/qa/water/veg_light.lua` (DX11 and Vulkan).

## Measuring

- `tools/qa/water/veg_cost.lua` (scenario): at the save's eye, headings a quarter turn apart, each mode of a plan in turn
  (an A/B in one run), the `r__gpu_log` lines between its markers, the populations' stats and a shot per mode;
  `tools/qa/Parse-VegCost.py <run>` averages them per heading and mode; `tools/qa/Grass-Quality.py <run A> <run B>`
  compares the grass's cover, detail, luma and chroma in the two runs' shots, view by view. `qa_veg_hour` sets the hour (a low sun for
  the sheen); a mode of `wind_seed 1833.7|wind_freeze 1` holds the wind still for a shot A/B of two shader sets.
- `tools/qa/water/grass_gust.lua`: `r__capture` of the meadow per mode, for the far field's motion frame to frame;
  `wind_seed` before each mode, so both start from the same wind (a capture a few seconds later caught another gust).
  Even so two captures differ by a few per cent (the near band's spread is the noise floor).
- `r__gpu_log` fields: `grass`, `grass_gen`, `shrubs`, `shrubs_bend`, `trees`, `trees_bend`, `trees_bb`, `sun`, and
  the CPU's `trees_cpu`, `shrubs_cpu`, `veg_wait` (the render thread waiting for the vegetation's frame pick).
- QA switches: `r__shrubs_cut 0` (the shrubs as the CPU chose them), `r__dbg_grass 1024` (no distance boost of the
  gusts).
