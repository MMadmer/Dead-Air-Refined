# Bloom and motion blur

Both follow DOOM Eternal's renderer (from the static breakdown of its shaders) and run on DirectX 11, DirectX 12 and
Vulkan alike. Shaders in `shaders/r3` of the compatibility overlay, passes in `src/Layers/xrRenderPC_R4`.

## 1. Bloom pyramid

The stock bloom drew nothing that could be seen: one 256x256 RGBA8 target of combine_1's opaque light past a
threshold the exposed scene almost never reaches, added after the tone curve (measured: under 1/255 on a sunny
road and next to a bare bulb). Its chain stays, unchanged, as the source of the exposure meter (`phase_luminance`)
and of the distortion's blur; its filter scale is pinned at the stock 0.7, so the bloom slider no longer moves the
exposure.

The pyramid (`r4_rendertarget_phase_da_bloom.cpp`, `da_bloom*.ps`, `blender_da_bloom.h`):

- **Light:** combine_1's second target (the exposed HDR, up to 7) plus what the forward passes added after it -
  fire, muzzle flashes, glowing particles, reflections, volumetric light: the finished scene against the opaque
  grab (`$user$ssr`), both taken back through combine_1's tone curve.
- **Bright pass** at half the render size: DOOM's 13-texel downsample (centre and four taps turned 30 degrees),
  each tap weighed down by its brightness (Karis) so one firefly cannot light a texel, then DOOM's soft knee
  `c * smoothstep(0, 1, (max(c) - threshold) / knee)`.
- **Temporal stabilisation** (DOOM's `computedownsampletaa`): the previous result reprojected by the motion vectors,
  clamped to this frame's five taps, a fifth of this frame at rest, up to 0.6 while the image moves. Only on frames
  with motion vectors of their own; a history reset or a missing frame starts it over.
- **Pyramid:** five more levels down to 1/64 (13-texel downsamples); from the smallest up, each level's separable
  Gaussian (sigma 2.5 texels, 9 bilinear taps) times its weight plus the finished level below it, upsampled. The
  weights `0.35 0.22 0.15 0.12 0.09 0.07` (half size first) sum to one: a tight core and a wide halo, no light added.
- **Composite:** combine_2 adds it before ACES, which rolls the halo off with the light it surrounds instead of
  clipping it; the visor's dirt glows from it too. The underwater medium tints it like the scene.

Knobs: the menu's bloom slider (`r2_ls_bloom_kernel_scale`, 0.5 to 2, default 0.7) scales the share; the look
constants `r__bloom` 0.3 (the share at the default slider), `r__bloom_threshold` 0.5 and `r__bloom_knee` 0.5 (exposed
brightness) are re-applied by the preset sync. `r__bloom 0` turns the pyramid off for a session.

Cost (RTX 3070 Laptop, DirectX 12, 1440p, DLSS Balanced): 0.14 ms over the stock chain.

## 2. Motion blur

An option of its own, Video -> Effects -> Motion blur (`r__mblur`, saved, off by default): a matter of taste, so no
quality preset sets it. The stock one (`mblur.h`, behind the `-mblur` switch) smeared the whole frame by the camera's
motion, weapon included, and the combine pass the game runs never called it; it and `r2_mblur` are gone.

McGuire et al. 2012, as DOOM Eternal runs it (`r4_rendertarget_phase_da_mblur.cpp`, `da_mblur*.ps`, `da_mblur.h`):

- **Motion:** the motion vectors, made on every frame the option is on (`da_temporal.cpp`); without them (MSAA, the
  unpacked G-buffer) the camera's motion through the depth. Half of what moves during the shutter
  (`r__mblur_shutter` 0.5, a 180-degree shutter, a look constant) each way, at most 20 render pixels, so the blur
  follows the frame time like a camera's. The hands and the held item (nearer than 1.1 m) blur a quarter as much.
- **Passes:** the half motion and the depth of every render pixel (one fetch for what follows), the longest motion of
  every 20x20 tile, of every tile and its eight neighbours.
- **Gather** in combine_2, after TAA and the upscalers (neither sees the blur), before the depth of field, the bloom
  and the tone curve: 4 to 12 taps (one per two pixels of the motion) along the neighbourhood's motion, the tile
  motion filtered between tiles (no tile grid), white-noise jitter per pixel; each tap counts by whether it is in
  front of the pixel or behind it, how far it moves and how far it lies (cone and cylinder weights, the depth
  compared over a twentieth of it): a moving edge smears over what is behind it, nothing behind it smears over the
  edge, the weapon stays sharp over a turning world.

Cost (same stand): about 0.6 ms while turning at 90 degrees a second, nothing measurable at rest (a pixel whose
neighbourhood does not move returns at once).
