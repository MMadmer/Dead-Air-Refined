# Screen-space reflections

Glossy opaque surfaces (wet roads and ground, car bodies, polished floors) mirror what the screen shows of the
world, after DOOM Eternal's `computessrtrace` and its temporal upscale, wherever the ray traced glossy reflections
(`dx12-features/dxr.md` 3.9) do not run: DirectX 11, and DirectX 12 or Vulkan with ray traced reflections off.
Shaders `da_ssr.h`, `da_ssr_trace.ps`, `da_ssr_temporal.ps` and `combine_1.ps` in the compatibility overlay; engine
`src/Layers/xrRenderPC_R4/r4_rendertarget_phase_ssr.cpp`.

## 1. What was there

Without ray tracing a glossy surface reflected only the sky's cube: a wet road under a bus showed the overcast where
the bus should be, and the water and puddle passes were the only ones that marched the screen.

## 2. The passes

Before `combine_1`, after the ambient occlusion:

- **Trace**, half resolution: one pixel of each 2x2 block a frame, the four in turn (DOOM's per-frame sub-pixel
  offset). Glossy pixels (gloss from 0.03, the full weight at 0.12; not foliage, snow or the hands) march their
  mirror ray, spread by roughness and jittered every frame, against this frame's depth: uniform screen steps with
  1/z linear along them, a bisection at the crossing, a thickness test. Low 12 steps a ray, High 20, up to 250 m.
- **The colour of a hit** is what the previous frame showed there: `combine_1` writes its linear colour (before the
  tone map, after the fog) into a third target, and the trace reprojects the hit into it. So the reflection of the
  opaque world lands in the specular term at the scene's own brightness, as DOOM reprojects its last frame.
- **Temporal**, full resolution: the 3x3 blocks of this frame's trace around a pixel, weighted by distance, depth
  and normal; their mean and spread clip the history, which follows the reflected point (the mirror image of a plane
  moves with the hit's parallax). The same filter as the traced reflections' `rt_gloss_temporal.hlsl`.
- **Combine**: the result, premultiplied by its confidence, takes the cube's place in the sky specular term, as the
  traced one does. Where a ray finds nothing on screen the cube stays.

The thresholds and the roughness spread are the traced reflections', so the two look alike: in a rain on the bus by
the road the screen-space frame against the traced one differs by 0.5 to 0.6 of 255.

## 3. Cost

RTX 3070 Laptop, 1440p, DLSS Balanced (render 1484x834), a steady rain, High:

| | DirectX 12 | DirectX 11 | Vulkan |
|---|---|---|---|
| The `ssr` GPU slot | 0.34 ms | 0.33 ms | 0.40 ms |
| Frame, against off | -0.67 of 255: the wet ground stops mirroring the sky where the world stands | -0.60 | -0.74 |

Dry ground (gloss under 0.03) leaves both passes at their first test. Targets: the linear colour, the history pair
and the result at the render size (RGBA16F), the trace and its hit distance at half; made on first use, so a preset
without the reflections spends no memory.

## 4. Knobs

`r__ssr`, set by the presets: 0 off (Minimum, Low), 1 Low (Default), 2 High (High, Extreme). Ray traced reflections
take over while they run (`r__rt_reflections` with the Medium or High tier).
