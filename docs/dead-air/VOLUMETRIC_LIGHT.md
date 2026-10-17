# Volumetric sunlight

The sun's light in the air is a froxel volume, after DOOM Eternal's light scattering (from the static breakdown of
its `computelightscattering` and `computelightscatteringintegral` programs). It runs on DirectX 11, DirectX 12 and
Vulkan alike: compute shaders on 3D textures. Passes in `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_froxels.cpp`,
shaders `da_froxel*.cs/.ps/.h` in the compatibility overlay.

## 1. What it replaced

The stock shafts (`accum_volumetric_sun.ps`) marched every render pixel's ray with 20, 40 or 80 shadow taps of the far
cascade alone, at full resolution, with no temporal filter. Every ray read the same strength whatever its length, so a
wall at arm's length was as hazy as the horizon: at noon they lifted the whole frame by about 8 of 255. They cost
1.0 ms at High (RTX 3070 Laptop, 1440p, DLSS Balanced, DirectX 12). The pass, its shaders, its blenders and the far
cascade's forced chain reset (it made the far map start at the camera, so the shafts could read it near the eye)
are gone; the far cascade now covers only what lies past the middle one, at a better resolution.

## 2. The volume

- **Grid** over the view frustum, set by the `r2_sun_shafts` tier the presets already carry: Low (Default) 16 render
  pixels a froxel and 64 slices, Medium (High) 12 and 64, High (Extreme) 8 and 96. Slices are spread quadratically
  in view depth from 0.1 m to 200 m (DOOM's 100 m is an arena's scale). The injection writes an RG16F volume (the
  air's density and the sun it sees), copied each frame into a second one for the next frame's history; the integral
  is RGBA16F. The lit particles read the sun term of the current one (`LIT_PARTICLES.md`), which keeps the injection
  running without the glow while they are about.
- **Injection**, once a frame after the sun has accumulated every cascade (in the far cascade's accumulation, while
  the three slices of the shadow map array are still the sun's):
  - the cell's point, jittered in depth only inside its slice (DOOM: a 16-phase bit-reversed sequence plus a
    blue-noise offset per column);
  - the air's density: an e-fold of light every 40 m at the eye's height, thinning by e every 60 m above it, with
    wisps 14 m across (two octaves of value noise on the integer lattice hash, `da_lattice.h`) moving the density by
    up to 60 % and drifting with the wind at 60 % of its speed;
  - the sun it sees: the finest cascade that holds the point (one hardware PCF tap; the temporal filter averages the
    jitter), times the cloud deck's shadow (`da_cloud_transmittance`, the field the ground's cloud shadow reads);
  - the previous frame's value of the cell's centre, reprojected and blended in at 0.9 while it was on screen and in
    the grid (DOOM keeps 0.95; a swaying crown's shadow must not trail). A missed frame or a history reset starts over.
- **Integration** front to back along every column, each slice's in-scatter integrated against its own extinction
  (Hillaire 2015): what has reached the eye up to the far edge of each slice, and the transmittance. The light is the
  sun's hue, isotropic on purpose (the shafts must not crowd the sun - see the history of the stock shader), at four
  times the weather's shaft strength (`sun_shafts_intensity` + `r2_sun_shafts_value`, times the cloud cover's sun
  visibility).
- **Apply**: every pixel reads the integral at its depth (the sky at the far plane) into the volumetric light target,
  which `combine_volumetric` adds to the scene through the tone map, as it did the shafts. The light levels off at
  four times the strength by some 80 m of lit air: near things stay clear, the distance gets the sunlit haze, and the
  shadow of a crown or a house lies in the air as a darker column. At noon it keeps the frame's brightness of the
  stock shafts (7.1 against 8.1 of 255), spread by depth instead of flat.

## 3. Cost

Measured on the same stand: the `sun` slot of `r__gpu_log` with the volume on against off (it records inside the
sun's accumulation, on the cascades' lists):

| | Stock shafts (High) | Froxels (High, 186x105x96) |
|---|---|---|
| DirectX 12 | +1.0 ms | +0.35 ms |
| DirectX 11 | - | +0.15 ms |
| Vulkan | - | +0.4 ms |

## 4. Knobs

- `r2_sun_shafts` off / low / medium / high: the tier (the presets set it: off on Minimum and Low).
- `r__froxel_strength` (default 1): a look constant over the level above, re-applied by the preset sync.
- The weather's `sun_shafts_intensity` per hour stays the authored strength.

## 5. Not yet

The local volumetric lights (the flashlight's beam and the lamps authored volumetric) keep the stock slice renderer
(`accum_volumetric`), additive into the same target; water, glass and particles take the glow at the depth of the
opaque surface behind them, as they took the shafts before.
