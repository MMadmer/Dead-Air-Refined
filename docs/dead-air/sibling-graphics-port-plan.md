# Sibling graphics port — working checklist

Source: DeadAir-Engine-x64-OpenSource 239a785..7a8af8d, full graphics audit
(16 agents, 8 themes, independently verified). Scope fixed by the user:
every portable shader/computation feature; no DX12, no upscalers and their
velocity/reactive/TAA plumbing, no new assets, no animation/gameplay/UI.

HARD RULE (user, 2026-08-28): a graphics feature either ships through the
base quality presets (visible by default at its tier) or it does not ship at
all. Console commands are never a delivery mechanism — they remain only as
parameters of live features and as QA diagnostics. Everything that was
default-off/console-only was REMOVED in the follow-up commit.

Method: batches of related features, each batch = implement -> build ->
rig smoke (shader compile is fatal-checked by the engine itself; visuals by
captures) -> commit.

## Batch 1 — water (forward pass, one file zone) — DONE, smoke passed
- [x] water.ps into our overlay (seeded from the pack our players run)
- [x] Schlick Fresnel (F0=0.02, coupled: alpha/refl_amount/base fade/glint)
- [x] depth-based coloured absorption + softer water fog (exp -4 -> -1.3)
- [x] procedural shoreline foam (noise from already-sampled normals)
- [x] sun glint (daytime specular track)
- [x] sky cubemap unsquash fix (reflection vector)
- [x] distance wave damping + reflection firefly clamp
- [x] surface debris after fog + honest depth test
- [x] green standing-water profile (water_green split)
- [x] rain ripples as extra normal layers
- [x] screen-space refraction (difference-add)
- [x] settings_da_water.h (pruned of upscaler defines)
- [x] water SSR wired on DX11: rt_SSR scene grab + s_image + USE_REFLECTIONS
      (preset ladder r3_water_refl {0,1,2,3,3})
- [x] ogse_reflections.h: [loop] guard, ray-start jitter, quality ladder,
      SSR distance cutoffs, ripple damping for reflection stability
- [x] EXCLUDED inside these files: da_water_velocity/depth, TAA jitter,
      reactive stencil marking — stripped, not ported

## Batch 2 — fog and frame-wide output — DONE, smoke passed
- [x] combine_1.ps into overlay: mip fog (sky-coloured haze) + horizon
      flattening + height fog (analytic integral) + density ceiling
- [x] fog master + follow-visibility-distance (engine, cl_fog_params);
      r__fog_dist kept 1.0 (their 0.5 pulls haze closer — their taste)
- [x] water fog seam: our new water.ps takes the same sky-fog colour
- [x] output dithering (+-1/2 LSB triangular, both combine_2 variants)
- [x] gamma/brightness/contrast in shader (hardware ramp dead in windowed)
- [x] luminance-preserving tonemap + late desaturation (our common_functions.h)

## Batch 3 — vegetation and terrain — DONE, smoke passed
- [x] lod.ps impostor shading fix (r__lod_hemi/sat/bright, tuned 2/2/1)
- [x] grass distance-fade rework: fade_start preset ladder {0,.5,.7,.95,.95},
      fade_flat (height-only fade) {0,0,0,.5,.5}
- [x] grass world-position brightness variation (r__grass_tint 0.12 on for
      all presets, scale 12, base boost 1)
- [x] grass shadow distance 40 m + fade band 10 m (near sun cascade relief;
      their measure 5.05 -> 2.42 ms sun_smap) — CPU cull per part + VS fade
- [x] grass instance constant caching (cached_out; their ~47k x 12 mults/frame)
- [x] foliage debleach 0.6 + vibrance 1.6 + gloss kill (r__foliage_*)
- [x] terrain macro variation 0.5 + fake-grass tint 0.6 (from dt blend, not
      the red layer — their brown-distance trap) + macro relief 0.1 + mask
      jitter 0.005 + height splatting 1.0 (deffer_impl_flat.ps overlay)
- [x] vegetation impostor separate discard (r__veg_discard 0.5 tuned)
- [x] particle draw distance (r__particle_dist 150, their Jupiter measure)

## Batch 4 — lighting and shadows — DONE, smoke passed
- [x] NPC torch shadow off (r__npc_torch_shadow 0; the crawling black wedge —
      NPC lamp sits inside the chest bone) — actor/dropped torches unchanged
- [x] distance-based far sun shadow fade (r__sun_shadow_fade 140 m, replaces
      the map-edge fade whose border travels with the camera; falls back to
      the stock edge fade when the constant is unbound)
- [x] ultra shadow path retired on extreme sun quality (it bypassed shadow_hw;
      extreme now takes the same road as high — their instrumented conclusion)
- [x] forced early-Z on light accumulation ([earlydepthstencil], accum_base)
- [x] POM overhaul: distance-scaled step count, SampleGrad (divergent-flow
      mips), [loop] guard, 5-step binary refinement, sun-march self-shadow
      into albedo+gloss, r__parallax_* tuned (8/12/0.0105/shadow 4/32/2/8)
- [x] POM force onto every surface with a height map (r__parallax_force 1;
      their census: 112 hand-marked vs 2020 carrying height maps)
- [x] anisotropic filtering default 16 (was 8)
- [x] sun cascade tint diagnostic (r__dbg_sun_cascades — QA tooling)
- [x] torch brightness multipliers — NOT ported: our torch colours are
      script-owned (Lua setters); an engine multiplier would be a second
      master over the mod's own settings

## Batch 5 — rain surface response — DONE, smoke passed (puddles verified
   with forced wetness)
- [x] rain wetness accumulator (rain_params binder: frame marker, buildup
      90 s / dry-out x4, rain speed drives soaking not its ceiling; raw rain
      density stays live for water ripples on every preset)
- [x] procedural puddles in G-buffer (da_puddles.h: world-keyed valley noise,
      sky/slope gates, lightmap sun access indoors, straight-up water normal,
      soft-mask gloss, dark rim; terrain shader applies colour/gloss inline —
      their inout-loss lesson)
- [x] puddle world reflections (phase_da_puddle_refl: reads the batch-1
      rt_SSR grab, premultiplied over the scene before water; fresnel floor,
      sky fallback; skipped under MSAA like their build)
- [x] puddle preset ladder {off, gbuf, gbuf, +refl, +refl}
- [x] rain overhaul: near-vertical fast drops (3 deg/40-80 m/s), drop length
      from exposure (r__rain_len 2, width 0.2), brightness split drop 2.2 /
      splash 0.9, drop count 6000/radius 14 knobs, splash share 1.0 (stock
      refused every second hit), splash pool 4000/cache 1500

## Deliberately NOT ported / REMOVED (recorded verdicts)
Removed in the presets-or-nothing follow-up (were coded default-off, i.e.
console-only — violates the delivery rule; several also rejected or unproven
by the sibling's own in-game tuning):
- coarse four-layer far terrain detail (r__macro_detail — their tuning
  rejected the look outright)
- Toksvig specular AA (unproven visually even in their build)
- rotated PCF + per-cascade kernel floor + footprint-scaled far kernel
  (the rotation needs a temporal filter we do not have; the kernel widening
  ships off even in their build)
- hex-grid repeat breaking incl. the material allowlist and per-material
  constant plumbing (their own default off, look never validated)
- terrain detail mip bias + detail-normal distance fade (their tuned bias is
  a temporal-upscaler companion; both default 0 even there)
- sun shafts master/boost/floor, under-roof gate, horizon-sky shaft tint
  (all default-neutral; our UI has no checkbox these serve)
- shadow-pass wind freeze + wind scale (freeze ships off in their build too)
- specular floor r2_gloss_min (default 0 in both builds)
- runtime SSAO strength (neutral no-op knob)

Excluded by scope from the start:
- upscalers/TAA/velocity/reactive/hashed-aref/soft-edges dither, bicubic+RCAS
- DX12 (does not exist in their tree outside XeSS binaries)
- seasonal vegetation archive, duckweed/silt (asset rebinds)
- strict sRGB EOTF, ACES/Hable selector, linear-light workflow (their own
  non-defaults/experiments), grading-in-one-point (incompatible with our pack)
- SSR half-depth and standalone fullscreen SSR (rejected by their measures)
- stock PCSS disable markers (we keep our shadow.h behaviour)
- light probe instrument, frame-diff/graph profilers (their tooling)
- grass hardware instancing rework (ours already runs SV_InstanceID batching)
- translucency bend + foliage soft edges (deffer_base.ps — dead outside
  their TAA pipeline, gated on r__taa)
- shaft gain/norm knobs (their comments point at accum_volumetric_sun.ps,
  which carries no change in their tree — unverifiable, skipped)
- torch brightness multipliers (see batch 4)
