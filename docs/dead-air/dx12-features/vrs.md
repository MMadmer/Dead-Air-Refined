# Variable rate shading, tier 2 (DirectX 12)

Status: implemented (28.09.2026). V1-V7 of `PLAN.md` 4.2 reached: the functional part of the final
test phase (V5 quality rows, robustness with everything on) passed after `SkyClouds` was removed (13.7);
the Perf phase decided the ladder: **`vrs_by_preset = {0, 0, 0, 1, 1}`** - conservative on High and
Extreme, off on Minimum, Low and Default (13.8). Measured results and the parts removed with their
numbers: section 13. Module: `xrRender_R5.dll` only; `xrRender_R4.dll` (DirectX 11) compiles the same
sources and keeps every pass exactly as it is today.

## Integration amendments (PLAN.md)

Implemented by the `vrs` lane (milestones V1-V7 of `PLAN.md` 4.2). These points supersede the text
below where they differ (ids refer to `PLAN.md` section 1):
- **C11, layer split**: the `infra` lane queries OPTIONS6/OPTIONS10 once (`NativeCaps`;
  `GetShadingRateCaps` is a view of it) and caches `ID3D12GraphicsCommandList5` per pooled list at
  creation. This lane owns the context state in the new `d12_vrs.cpp` (`SetShadingRate`, apply,
  `SHADING_RATE_SOURCE` requirement incl. the deferred first requirement, safety rules, resets,
  `StatShadingRate`), reached through hook calls infra pre-places at I0 in `ApplyFixedFunction`,
  `EnsureDrawStates`, `ReleaseBindings`/`InvalidateApplied`/`ClearState`, `BeginNative` and
  `Device::Init`. `EndNative` no longer invalidates everything (infra's `EndNative(touched)`):
  `BeginNative` resets a non-default list to 1x1/no image and marks VRS dirty, and
  `EndNative(TouchedShadingRate)` re-applies the context state.
- **C14**: the cloud-march early-out of 3.7 moved to `async.md` (the march's owner): it needs the
  current depth, which the async march at fork A does not have.
- **C24**: the rate histogram goes to an 8x1 R32_UINT texture UAV with staging copies, not a typed
  buffer.
- **C29**: `AoRaw` applies on the Pixel path only (on the async path the GTAO integral is compute).
  Moot since V5: the `AoRaw` scope is removed (13.3).
- **C30**: V7 repeats the quality gate with `r__rt_shadows 1` on `qa_pedro` and `trees` (a coarse
  sun accumulation reads the RT mask of the block's representative pixel).
- **C20**: layer scenarios in `scenarios_vrs.cpp`, not `scenarios_render.cpp`.
- **C25/C26**: `Test-Shaders.ps1` (`.cs`) belongs to the `dxc` lane; `Run-WaterQa.ps1` TGA copying,
  `render_check`'s lossless shot and `tools/qa/Compare-Frames.py` (with the blockiness index and heat
  map specified in 9.3) are built by `core` at K0 for every lane.
- **C27/C28**: `core` registers the `r__vrs*` commands, the `vrs_by_preset` ladder (a `vrs` block in
  `xrRender_sync_preset_derived`), the `vrs=` field of the `[preset]` line, the `vrs` key of
  `r__caps` (from `da_vrs_supported()`), the `VrsRate`/`VrsLuma` timer slots and the `vrs` field of
  `r__gpu_log`, `CBackend::set_ShadingRate`, the VRS scopes in `r4_rendertarget_phase_combine.cpp`
  and `r4_rendertarget_phase_gtao.cpp`, the `da_vrs_*` hook calls in `r2_R_render.cpp`, and the preset
  hint sentence. This lane owns `da_vrs.*`, `r4_rendertarget_phase_vrs.cpp`, the three accum files,
  its shaders and `vrs_check.lua`.
- **C4**: the rate/luma compute shaders are SM 5.0 HLSL compiled in the session's mode (DXIL with DXC
  on R5); no SM 6 feature is required.

## 1. Goal

Shade fewer pixels where nobody can see the difference. Every frame a compute pass builds a
screen-space shading-rate image (SRI) from what the previous frame looked like (luminance
contrast, reprojected), how the frame moves (motion vectors / camera reprojection), where the
geometry has silhouettes (current depth), where the sky and the out-of-focus depth-of-field
regions are, and where the first-person HUD is. The passes that pay per pixel for smooth
content - the sun and local light accumulation, forward water/particles, puddle reflections and the
sun-shaft blurs (combine_1 and the GTAO integral left the table after the calibration, 13.3, the sky
and cloud deck after the final stand, 13.7) - bind it through `RSSetShadingRateImage` with a per-pass cap. The G-buffer, shadow
maps, anti-aliasing, the upscalers, display-size post and the UI never see it.

Look-changing, so it rides the quality presets: off / conservative / aggressive per preset (as
built after the Perf phase: conservative on High and Extreme, off below; 13.8). It is not a ray
tracing feature and is not part of the Video tab "RTX" category.

## 2. Current code

### 2.1 The frame (`src/Layers/xrRender_R2/r2_R_render.cpp`, `CRender::Render`)

| Step | Where | Size | VRS relevance |
|---|---|---|---|
| G-buffer: `phase_scene_begin` (MRT 0 position, 1 albedo, 2 velocity when drawn), level, HUD, lods, grass on 3 parallel deferred contexts (`parallel_details`, lines 33-125) | 371-399 | render | excluded (3.1) |
| wallmarks, occlusion, light visibility | 405-592 | render | - |
| `r_rain.sync()`, water field, ripple (CS), visor drops, cloud map, **clouds march** (half render) | 601-623 | own | excluded; see 3.7 |
| **Sun**: `r_sun.sync()` -> `render_sun::flush()` -> `accumulate_cascade()` records `accum_direct_cascade` on each cascade's *deferred* context and `submit()`s it (`render_phase_sun.cpp` 467-534); then `render_hud_shadow`, `accum_direct_blend` | 625-643 | render | **main target** |
| emissive (stencil) | 645-665 | render | excluded |
| `copy_depth_for_hud`, **local lights** `render_lights` (`r2_R_lights.cpp`: `accum_spot`/`accum_point`/`accum_volumetric` on the immediate context, spot shadow maps rendered in between into the smap atlas) | 679-694 | render | target (light draws only) |
| `phase_combine` (`xrRenderPC_R4/r4_rendertarget_phase_combine.cpp` 131-823) | 709 | | |
| - AO: `phase_gtao` (full-res horizon integral into `rt_gtao`, guided 8x8 filter, `r4_rendertarget_phase_gtao.cpp` 11-56) | 149-173 | render | aggressive only |
| - sky + `phase_clouds_composite` (sky pixels, stencil < 1) | 190-205 | render | target |
| - **combine_1** (albedo x light, fog, AO, tonemap -> `rt_Generic_0`) | 236-361 | render | aggressive only |
| - SSR grab copy, `phase_da_puddle_refl` (full-screen march over puddle pixels) | 380-405 | render | puddles: target |
| - **forward** `render_forward` (water, particles, glass, rain) + `phase_combine_volumetric` | 407-428 | render | target |
| - sky/water velocity, reactive mask, MSAA resolve, bloom, distortion | 430-473 | render | excluded |
| - SMAA/FXAA/TAA | 479-500 | render | excluded |
| - **sun shafts** `phase_sunshafts` (5 full-size passes E[0..4], 47-129) | 505-506 | render | E[0..3] flat 2x2 |
| - `phase_upscale` (FSR/DLSS/identity; native section), depth upscale | 511-512 | display | excluded |
| - combine_2, flares, `phase_pp`, `phase_velocity_debug` | 528-698 | display | excluded |
| `da_framegen_frame()`, UI after `Render()` | 723 | display | excluded |

`scene_stage_guard` (253-270) brackets the render-size stage; every exit of the frame goes
through its destructor. GPU timers: `dx11GpuTimers` slots (`xrRenderDX11/dx11GpuTimers.h` 26-43),
logged by `r__gpu_log N` (r2_R_render.cpp 730-740). Render targets: `r2_rendertarget.cpp` 288-483
(velocity `rt_Velocity` R16G16F only without MSAA and with the packed G-buffer, 463-465).

Light passes and their draws:

- `accum_direct_cascade` (`r4_rendertarget_accum_direct.cpp` 354-766): `phase_accumulator`, the
  **masker** (`s_accum_mask` SE_MASK_DIRECT, stencil write, 391-460; near cascade only), the
  **lighting draw** `s_accum_direct->E[...]` (604-745, stencil LESSEQUAL marker, depth test for
  near/middle), `accum_direct_volumetric` into `rt_Generic_2` (760-764).
- `accum_point` (`r3_rendertarget_accum_point.cpp`): masker volumes 36-74, lighting 91-172.
- `accum_spot` (`r3_rendertarget_accum_spot.cpp`): masker 52-80, lighting 193-298;
  `accum_volumetric` 318+.
- Shaders (`gamedata/shaders/r3/accum_sun_near.ps`, `accum_base.ps`, ...) read the G-buffer with
  `gbuffer_load_data(tc, pos2d)` (`common_functions.h` 340-386: point `Sample` at tc / `Load` at
  SV_Position). Checked over the r3 overlay and the unpacked archive (`_work/da_unpack`): none of
  the VRS'd shaders (accum_*, sky2, da_clouds, da_puddle_refl, water, particle, combine_1,
  gtao_render, sunshaftsgeneration) writes SV_Depth/SV_StencilRef/SV_Coverage; SV_SAMPLEINDEX
  appears only in MSAA variants; the discards are the sun masker `accum_sun_mask.ps` (excluded)
  and alpha-near-zero clips in `particle.ps` / `water.ps` (a coarse discard there removes nothing
  visible).

### 2.2 The D3D12 layer (`src/Layers/xrRenderDX12`)

- Lists are plain `ID3D12GraphicsCommandList` (`d12_layer.h` 1349; `Context::OpenList`,
  `d12_context_ops.cpp` 1339-1383). No VRS anywhere; `Device::Init` queries only OPTIONS and
  OPTIONS16 (`d12_device.cpp` 1493-1507).
- Per-context D3D11-level state + `Context::Applied` dirty flags (`d12_layer.h` 1251-1303);
  `ApplyFixedFunction` (`d12_context_state.cpp` 698-762) applies RTs/viewports/scissors/topology;
  `EnsureDrawStates` (582-632) requires resource states per draw; `PrepareDraw` (1125-1160).
- Deferred contexts keep a local tracker; `ExecuteCommandList` patches their first requirements
  into the immediate list (`d12_context_ops.cpp` 2510-2574). `ExecuteCommandList(FALSE)` and
  `FinishCommandList(FALSE)` call `ClearState` (= `ReleaseBindings` + `InvalidateApplied`, 2635).
- Native sections for SDKs: `BeginNative`/`Require`/`EndNative` (`d12_swapchain.cpp` 801-850).

### 2.3 Presets and console (`src/Layers/xrRender/xrRender_console.cpp`)

- `xrRender_sync_preset_derived()` (1086-1340): per-preset ladders indexed by `ps_Preset`
  (0 Minimum, 1 Low, 2 Default, 3 High, 4 Extreme), replayed at every renderer start; logged in
  the `[preset]` line; `qa_autoexec.ltx` applies after it.
- Session overrides: `CCC_RuntimeInteger`/`CCC_RuntimeFloat` (762-783, never saved); the
  override-variable pattern of `r__clouds_quality` -> `ps_r__clouds_quality_override` (-1 =
  preset; 1736, read in `r4_rendertarget_phase_clouds.cpp` 17-20) survives the preset replay.
- `r__caps` (`CCC_RenderCaps`, 835-856): `api=..;dx12=..;dlss=..;...` for the options menu.

### 2.4 What the hardware and the frame measure

Probe (throw-away D3D12 program, scratchpad; reproduced later by the layer test `vrs_rates`),
RTX 3070 Laptop, driver current, 2560x1440 full-screen triangle:

| Setup | PS invocations | Heavy PS (64 iters) | Light PS (16 iters) |
|---|---|---|---|
| 1x1 | 1.000 | 0.611 ms | 0.122 ms |
| per-draw 2x2 / image 2x2 (OVERRIDE) | 0.250 | 0.155 ms (3.95x) | 0.031 ms (3.94x) |
| image 2x1 | 0.500 | 0.309 ms (1.98x) | 0.062 ms |
| image 4x4 | 0.0625 | 0.078 ms (7.9x) | 0.028 ms (4.4x: fill floor) |
| base 2x2 **MIN** image 4x4 | 0.250 | = 2x2 | cap works |
| base 4x4 MIN image 1x1 | 1.000 | = 1x1 | |
| image covering only the left half | 0.625 | | outside the image = 1x1 |

Caps: `VariableShadingRateTier 2`, `ShadingRateImageTileSize 16`, `AdditionalShadingRatesSupported 1`,
`VariableRateShadingSumCombinerSupported 1`. In a 2x2 coarse pixel SV_Position is the coarse
centre (1.0, 1.0): `Load(int2(pos))` and a point `Sample` at the interpolated tc both read the
**bottom-right fine pixel** of the block; its result is replicated to all four.

Pixel-proportional pass cost (stand, qa_pedro, R5, Extreme, TAA, 2560x1440; render 2560x1440 vs
1280x720 through FSR performance, two interleaved rounds, `r__gpu_log 300`):

| Pass (`[gpu]` field) | full ms | quarter ms | pixel part at 1440p | fixed part |
|---|---|---|---|---|
| sun (cascades + accumulation) | 4.74 | 2.08 | **3.55** | 1.19 (shadow maps) |
| lights | 0.47 | 0.22 | 0.33 | 0.14 |
| ao (GTAO) | 1.62 | 0.30 | ~1.6 | ~0 |
| forward | 1.75 | 0.74 | 1.35 | 0.40 |
| combine rest (combine_1, sky, clouds, grab, puddles) | 0.64 | 0.19 | 0.60 | 0.04 |
| scene (G-buffer, not VRS'd) | 3.05 | 1.41 | 2.19 | 0.86 |
| clouds march (half res, not VRS'd) | 1.95 | 0.81 | 1.52 | 0.43 |
| frame | ~17 | | | |

So about 5.8 ms of a 17 ms Extreme frame is pixel work VRS can reach (conservative set ~5.3 ms).

## 3. Design

### 3.1 Scope decisions

- **DX12 + tier 2 only.** Needs `RSSetShadingRateImage` + combiners. Tier 1 (per-draw only) and
  NVAPI VRS on DX11 are not used: DX11 stays exactly as it is.
- **G-buffer: no.** Coarse G-buffer replicates one albedo/normal/gloss per block (texture blur
  that TAA cannot undo), alpha-tested foliage `clip()` would kill whole blocks (blocky
  silhouettes), MRT writes stay full rate so the saving is small, and the grass is recorded on
  parallel deferred contexts. Lighting is where the per-pixel ALU is; albedo stays full rate and
  multiplies the coarse light in combine_1, so texture detail survives.
- **Shadow maps, rain map, HUD shadow: no** (depth-only, alpha-tested casters, other sizes).
- **Stencil maskers: never.** `s_accum_mask` passes write stencil through PS discard/alpha
  tests; coarse would make the light masks blocky.
- **MSAA: VRS off** (`o.msaa`): per-sample shaders ignore coarse rates and the SRI inputs assume
  one sample.
- **Display-size passes: never** (combine_2, pp, flares, UI, velocity/reactive, AA, upscale).
  The SRI is render-size; the probe shows an image smaller than the target shades the rest at
  1x1, so a mistake is harmless but still wrong, and the layer refuses it (3.4).
- **HUD (first-person) pixels: always 1x1** (depth < `r2_hud_depth_limit` 0.02, `r2_types.h` 73).

### 3.2 Building the rate image

Two small compute passes, SM 5.0 HLSL (`cs_5_0`; compiled in the session's mode like every engine
shader, DXIL with DXC on R5 - PLAN C4), both at render size.

**A. `phase_vrs_luma` - end of frame N** (phase_combine, after `phase_sunshafts`, before
`phase_upscale`): reads `rt_Generic_0` (the finished render-size LDR frame, tonemapped, after
AA and shafts; also what the upscaler reads) and writes `rt_vrs_contrast`, one texel per 8x8
pixel block (RGBA16F, ceil(W/8) x ceil(H/8)):

- `L = dot(rgb, (0.299, 0.587, 0.114))` (gamma-encoded LDR: roughly perceptual);
- `x = Ex = sqrt(mean over the block of (L(x+1,y) - L(x,y))^2)`, `y = Ey` likewise vertically,
  `z = mean L`, `w = 1` (valid). Group 8x8 threads with a 9x9 groupshared luma tile.

**B. `phase_vrs_rate` - frame N+1, before the sun** (after `phase_cloud_map`/clouds march,
immediately before the `DEFER_SUN` block): one thread group per 16x16 pixels (tile T from caps:
T=8 -> the group writes 2x2 texels, T=16 -> 1, T=32 -> groups of 32x32 pixels, 2x2 pixels per
thread). Inputs: hardware depth of `rt_Render_Depth` (SRV of its CRT texture; the DSV is unbound
first), `rt_vrs_contrast`, `rt_Velocity` when `velocity_ready()`, the previous SRI (ping-pong).
Per pixel (groupshared depth tile with a 1-pixel apron):

- `sky = d >= 1` (depth is cleared to 1 in `phase_scene_prepare`; the sky is drawn later),
  `hud = d < r2_hud_depth_limit`.
- **Silhouette**: view depths `z = P43 / (d - P33)`; edge when `max(z)/min(z)` over the pixel and
  its 4 neighbours exceeds `1 + k_jump` (0.08 conservative, 0.20 aggressive) or the neighbourhood
  mixes sky/non-sky or HUD/world. Grass blades over the ground right behind them are not edges;
  a trunk against a hill 100 m away is. (Creases are left to the contrast term: a crease with a
  visible lighting step is a luminance edge.)
- **Reprojection**: previous uv = `uv - velocity * (0.5, -0.5)` when the vectors are drawn, else
  through `m_vrs_reproj = VP_prev * inverse(VP_nojit)` (da_temporal.h `da_vp_prev`/`da_vp_nojit`)
  on the reconstructed position; `invalid` when the previous uv leaves the screen.
- Contrast `(Ex, Ey, Lmean)` = point sample of `rt_vrs_contrast` at the previous uv; motion
  `m` = length of the reprojection offset in render pixels.

Per tile (groupshared reduction): `Ex = max`, `Ey = max`, `L = mean`, `m = min`, `edge/hud/invalid =
any`, `sky = all`, `farDof = all pixels beyond the DoF far plane with >= 2 px blur` (from
`GetCurrentDof` + `r2_dof_enable`, the constants combine_2 uses). Then per axis:

```
tau      = s_mode * (L + 0.08) * r__vrs_threshold * (renderW / displayW)
           s_mode: 0.06 conservative, 0.15 aggressive (starting values, calibrated in 9.3)
Ex_eff   = Ex / (1 + k_m * m)            k_m 0.05 cons / 0.10 aggr per px/frame, at most /2,
                                          only while TAA, an upscaler or r2_mblur runs
Ex_eff  /= prev_rate_x == 2 ? sqrt(2) : prev_rate_x == 4 ? 2 : 1
           (a block shaded at 2x shows zero inside, 2x the step at its borders: RMS grows by sqrt 2)
half_ok  = Ex_eff        < tau * (prev_rate_x >= 2 ? 1.0 : 0.8)     (hysteresis 20 %)
quart_ok = 2.13 * Ex_eff < tau * (prev_rate_x >= 4 ? 1.0 : 0.8)     (quarter-rate error ratio)
rx       = quart_ok && (aggressive || sky) ? 4 : half_ok ? 2 : 1;   ry likewise
```

Overrides, in order: `edge | hud | invalid | history not valid` -> 1x1; `sky` -> 4x4;
`farDof` -> one step coarser on both axes; conservative clamps non-sky tiles to 2x2; no
additional rates -> clamp to 2; 1x4 -> 1x2, 4x1 -> 2x1. Written as `D3D12_SHADING_RATE`
(`log2(rx) << 2 | log2(ry)`) into `vrs_image[frame & 1]`; with `r__vrs_log` a 7-bin histogram
goes to a counter buffer (one `InterlockedAdd` per tile).

History validity: the rate pass runs only when `phase_vrs_luma` ran in frame N-1
(`m_vrs_luma_frame == Device.dwFrame - 1`), `da_history_reset()` is false (level load, teleport,
size/upscaler change, menu -> game) and the targets were not recreated; otherwise the frame has
no image and every pass shades at 1x1 (the luma pass still runs to seed the next frame).
`r__vrs_force 1|2` writes 2x2|4x4 everywhere (QA plumbing and the upper bound of the gain).

### 3.3 Applying it: one image, per-pass caps

Combiners `{PASSTHROUGH, MIN}` with the pass cap as the base rate: `final = min(cap, image)` per
axis (verified by the probe). No per-primitive rates (SV_ShadingRate needs SM 6.4; not used).

| Pass (`VrsPass`) | Scope placed around | Context | Conservative | Aggressive |
|---|---|---|---|---|
| `SunLight` | the lighting draw block of `accum_direct_cascade` (and `accum_direct_f`) | deferred (per cascade) | image, cap 2x2 | image, cap 4x4 |
| `SunVolumetric` | `accum_direct_volumetric` | deferred | image, cap 2x2 | image, cap 4x4 |
| `Lights` | the `shader->E[_id]` draws of `accum_point` / `accum_spot` | immediate | image, cap 2x2 | image, cap 4x4 |
| `LightsVolumetric` | the draws of `accum_volumetric` | immediate | image, cap 2x2 | image, cap 4x4 |
| ~~`SkyClouds`~~ | ~~`RenderSky()` + `phase_clouds_composite()`~~ | removed (13.7): failed the turn gate | - | - |
| `PuddleRefl` | `phase_da_puddle_refl` | immediate | image, cap 2x2 | image, cap 2x2 |
| `Forward` | `render_forward()` in phase_combine | immediate | image, cap 2x2 | image, cap 4x4 |
| `VolumetricCombine` | `phase_combine_volumetric` | immediate | image, cap 2x2 | image, cap 2x2 |
| `SunshaftsBlur` | `phase_sunshafts` E[0..3] (not E[4], which mixes the scene) | immediate | flat 2x2, no image | flat 2x2 |

The design's `Combine1` (combine_1, aggressive, cap 2x2) and `AoRaw` (the `gtao_render` draw) failed
the aggressive quality gate and are removed with their scopes and enum values (13.3); `SkyClouds` failed the
turn gate of the final phase and is removed the same way (13.7).

`da_vrs_scope scope(cmd_list, VrsPass::X);` sets the state on entry and resets 1x1/no image on
exit, so no VRS state ever crosses a shadow-map render (local lights render their atlas between
light draws), a target switch, a `submit()` or the end of the scene stage.

### 3.4 Layer extension (`xrD3D12`)

Public API (`d12_api.h`):

```cpp
struct ShadingRateCaps { u32 tier; u32 tileSize; bool additionalRates; };
bool GetShadingRateCaps(ID3D11Device* device, ShadingRateCaps* caps);
// Context state like any D3D11 state (immediate or deferred): kept until changed, cleared by
// ClearState, ExecuteCommandList(FALSE) and FinishCommandList(FALSE); never seen by native sections.
void SetShadingRate(ID3D11DeviceContext* context, D3D12_SHADING_RATE base,
    D3D12_SHADING_RATE_COMBINER perPrimitive, D3D12_SHADING_RATE_COMBINER image, ID3D11Resource* rateImage);
```

Implementation:

- `Device::Init`: query `D3D12_FEATURE_D3D12_OPTIONS6` (+ OPTIONS10 for the log), store it, log
  `* VRS: tier %d, tile %u, additional rates %d`.
- `Context`: `struct { D3D12_SHADING_RATE base; D3D12_SHADING_RATE_COMBINER comb[2]; ID3D11Resource* image; } vrs`
  (image AddRef'd) and `applied.vrsDirty`, `applied.vrsListNonDefault`. `m_list5`
  (`ID3D12GraphicsCommandList5`) cached by QueryInterface in `OpenList` when tier >= 2.
- `ApplyFixedFunction`: when `vrsDirty` and (state non-default or the list is non-default):
  `RSSetShadingRate(base, comb)` + `RSSetShadingRateImage(native or null)`.
- `EnsureDrawStates`: `EnsureState(image, all, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE)`; the
  deferred tracker records it as a first requirement, `ExecuteCommandList` patches the transition
  in (the cascades' lists are exactly this case).
- Safety rules (logged once each): the image is ignored for a draw when it is bound as an output
  (RTV/UAV) or when the bound RT0/DSV size is not covered by `image * tile` within one tile
  (display-size or shadow-map targets); `SetShadingRate` below tier 2 is ignored.
- `ReleaseBindings` releases the image and resets to 1x1/PASSTHROUGH; `InvalidateApplied` sets
  `vrsDirty` and `vrsListNonDefault = false` (a fresh list starts at 1x1).
- `BeginNative`: if the open list is non-default, reset it to 1x1/no image before handing it
  out and mark VRS dirty; `EndNative(TouchedShadingRate)` re-applies the context state (infra's
  `EndNative(touched)` no longer invalidates everything, PLAN C11).
- `-dxstats`: `StatShadingRate` (RSSetShadingRate calls per frame).

### 3.5 Interactions

- **TAA / DLSS / FSR, render vs display size**: the SRI, its inputs and all consumers are
  render-size scene-stage passes (`da_stage_scene(true)`); `da_vrs_reset(RCache)` runs right
  after `phase_vrs_luma`, before `phase_upscale`, and in `scene_stage_guard`'s destructor. The
  threshold scales with `renderW/displayW` (DLSS performance halves it: the upscaler magnifies
  block errors); with a ratio >= 2.0 an aggressive mode runs as conservative.
- **Jitter**: the SRI is jitter-agnostic at tile scale; the coarse sample sits at the jittered
  coarse centre, so upscalers still receive jittered (sparser) samples in coarse tiles. The G-buffer,
  depth, velocity and reactive mask stay full rate - nothing the SDKs read changes.
- **Frame generation**: reads depth, velocity and the hudless copy after `phase_pp` - unaffected;
  its native work runs with the list reset by `BeginNative`.
- **MSAA**: off (3.1). **UI / menu / PDA canvas**: reset at stage end; `RenderMenu` and
  `BeforeWorldRender` never set it. **Pause menu PP** (`OnRenderPPUI_query`): no rate pass.
- **Screenshots, save thumbnails, `r__screenshot_every`**: read the back buffer - unaffected.
- **Plant shadows**: untouched (shadow maps never get VRS; wind stays in the vertex shaders).

### 3.6 Engine-side module

`xrRenderPC_R4/da_vrs.h/.cpp` (both modules compile it; without `XR_D3D12` everything answers "off"):

```cpp
enum class VrsPass : u8 { SunLight, SunVolumetric, Lights, LightsVolumetric, PuddleRefl,
    Forward, VolumetricCombine, SunshaftsBlur, Count };   // Combine1, AoRaw (13.3), SkyClouds (13.7) removed
bool da_vrs_supported();            // R5, tier 2, shaders present, targets created
int  da_vrs_mode();                 // this frame: 0 off, 1 conservative, 2 aggressive
bool da_vrs_ready();                // the rate image of this frame exists
void da_vrs_apply(CBackend& cmd, VrsPass pass);  // cap table 3.3; no-op when off / not in the mode
void da_vrs_reset(CBackend& cmd);
struct da_vrs_scope { da_vrs_scope(CBackend&, VrsPass); ~da_vrs_scope(); };
```

`CBackend::set_ShadingRate(u8 base, u8 combiner, ID3D11Texture2D* image)` (`R_Backend.h`, only
under `XR_D3D12`) caches the last state per context and calls `xrd3d12::SetShadingRate`;
`CBackend::Invalidate()` (`R_Backend_Runtime.cpp` 48) forgets the cache.

### 3.7 Findings outside VRS (fixed with this work)

- The clouds march (1.5 ms pixel part at 1440p High/Extreme) marches every half-res texel,
  including those behind geometry that the composite never shows (`da_clouds_march.ps`: "the sky
  mask is applied at composite"). A depth early-out (all full-res depths of the texel's 2x2
  footprint plus a 1-texel border < 1 -> write a sentinel `a = -1`, and the history blend
  treats a sentinel as "no history") saves more than VRS could and works on DX11 as well.
  **Moved to `async.md`** (PLAN C14): implemented by the `async` lane on the Pixel path.
- `tools/qa/Test-Shaders.ps1` compiles only `.ps`/`.vs` (lines 66, 86): `.cs` (cs_5_0) is added,
  which also covers the existing ripple compute shaders.
- `tools/qa/Run-WaterQa.ps1` copies only `*.jpg` (line 62): it also copies the lossless `-ss_tga`
  shots (`dx11r_screenshot.cpp` 355-368) as `NN.tga`, which the quality diff needs.

## 4. Integration points

| File | Function / place | Change |
|---|---|---|
| `xrRenderDX12/d12_api.h` | namespace `xrd3d12` | `ShadingRateCaps`, `GetShadingRateCaps`, `SetShadingRate` |
| `xrRenderDX12/d12_layer.h` | `Context` (1097-1379), `Device` (1468) | `vrs` state, `applied.vrs*`, `m_list5`, `options6`, stat |
| `xrRenderDX12/d12_device.cpp` | `Device::Init` (1493-1519) | OPTIONS6/10 query, log line |
| `xrRenderDX12/d12_context_state.cpp` | `ApplyFixedFunction` 698, `EnsureDrawStates` 582 | apply + SHADING_RATE_SOURCE + safety rules; `SetShadingRate` entry |
| `xrRenderDX12/d12_context_ops.cpp` | `OpenList` 1339, `InvalidateApplied` 1552, `ReleaseBindings` | list5 cache, dirty/reset |
| `xrRenderDX12/d12_swapchain.cpp` | `BeginNative` 801 | reset a non-default list |
| `xrRender/R_Backend.h`, `R_Backend_Runtime.cpp` | `CBackend` | `set_ShadingRate` + cache reset in `Invalidate` |
| `xrRender_R2/r2_R_render.cpp` | `Render()` before `DEFER_SUN` (625); `scene_stage_guard` dtor (263); `[gpu]` Msg (732) | `Target->phase_vrs_rate()`; `da_vrs_reset(RCache)`; `vrs=` field |
| `xrRenderPC_R4/r4_rendertarget_accum_direct.cpp` | `accum_direct_cascade` 604-745, 760-764; `accum_direct_f` | `SunLight` / `SunVolumetric` scopes |
| `xrRender_R2/r3_rendertarget_accum_point.cpp` | 91-172 | `Lights` scope |
| `xrRender_R2/r3_rendertarget_accum_spot.cpp` | 193-298, `accum_volumetric` | `Lights`, `LightsVolumetric` scopes |
| `xrRenderPC_R4/r4_rendertarget_phase_combine.cpp` | 190-205, 236-361, 405, 407-428, `phase_sunshafts` 123-126, before 511, after 698 | scopes; `phase_vrs_luma()` + reset before upscale; `phase_vrs_debug()` |
| `xrRenderPC_R4/r4_rendertarget_phase_gtao.cpp` | - | none (the `AoRaw` scope is removed, 13.3) |
| `xrRenderPC_R4/r4_rendertarget.h` | `CRenderTarget` | members, `phase_vrs_rate/luma/debug`, `vrs_create/destroy` |
| `xrRender_R2/r2_rendertarget.cpp` | ctor after 465; dtor | `vrs_create()` under R_R4 && XR_D3D12, not with MSAA |
| `xrRenderDX11/dx11GpuTimers.h/.cpp` | `Slot` | `VrsRate`, `VrsLuma` (+ names) |
| `xrRender/xrRender_console.cpp/.h` | `xrRender_sync_preset_derived` 1086; registration 1956-1969; `CCC_RenderCaps` 847 | ladder + `[preset]` log, `r__vrs*`, `vrs=` cap |
| `xrRenderPC_R4/sources.cmake` | R4 source list | `da_vrs.cpp`, `r4_rendertarget_phase_vrs.cpp` |
| `xrRenderPC_R4/r4_rendertarget_phase_clouds.cpp` + `da_clouds_march.ps`/`da_clouds.ps` | march/composite | 3.7 early-out + sentinel history |

## 5. Data and resources

| Resource | Format | Size (1440p, T=16) | Created | Notes |
|---|---|---|---|---|
| `vrs_image[2]` | R8_UINT, SRV + UAV | 160x90 (ceil(W/T) x ceil(H/T)) | `HW.pDevice->CreateTexture2D` in `vrs_create` | ping-pong: write N, read N-1 as `$user$vrs_prev` (`surface_set`) |
| `rt_vrs_contrast` | RGBA16F CRT, `CRT::CreateUAV` | 320x180 (ceil/8) | `vrs_create` | `$user$vrs_contrast` |
| `vrs_stats` + 3 staging copies | R32_UINT texture 8x1, UAV (PLAN C24) | 32 B | `vrs_create` | only read with `r__vrs_log`: 3 frames late, the slot is mapped (`DO_NOT_WAIT`) before the frame's copy replaces it, when the layer (3 frames in flight) has already waited for it |

Total below 0.5 MB. Recreated with the render targets (vid_restart, upscaler change, size change);
recreation invalidates the history. Depth read: SRV of `rt_Render_Depth`'s CRT texture (the view
`s_da_depth_render` uses); velocity: `t_velocity` (`$user$velocity`). Constants: `da_vrs_params`
(tile, mode, s_mode * r__vrs_threshold * render/display, k_jump), `da_vrs_params2` (P33, P43,
hud limit, k_m), `da_vrs_reproj` (float4x4), `dof_params`/`dof_kernel` as combine_2 sets them.

## 6. Shaders (`packaging/dead-air-x64/compatibility/gamedata/shaders/r3`)

| File | Kind | Content |
|---|---|---|
| `da_vrs.h` | include | rate encoding, luma, constants |
| `da_vrs_luma.cs` | cs_5_0 | 8x8 groups, 9x9 groupshared luma, writes `u_vrs_contrast` |
| `da_vrs_rate.cs` | cs_5_0 | 16x16 groups, 18x18 groupshared depth, reprojection, reduction, writes `u_vrs_rate` (+ `u_vrs_stats`) |
| `da_vrs_debug.ps` + `.s` | fullscreen (`da_fullscreen.vs`) | `r__vrs_debug 1` tint by rate over the finished frame (1x1 none, 2x1/1x2 blue, 2x2 green, 2x4/4x2 yellow, 4x4 red, 35 %) |

Blenders `CBlender_da_vrs_luma` / `CBlender_da_vrs_rate` live file-local in
`r4_rendertarget_phase_vrs.cpp` (`C.r_ComputePass`, like `CBlender_da_ripple_prop`), created on
first use; `open_shader` checks the files exist (a binary update without content leaves VRS off,
like `hud_shadow.ps`). UAVs are bound by hand as in `da_ripple_compute`
(`r4_rendertarget_phase_water_ripple.cpp` 84-99). No existing shader changes for VRS itself; the
march early-out of 3.7 changes `da_clouds_march.ps` and `da_clouds.ps`.

## 7. UI, presets, console

- Ladder in `xrRender_sync_preset_derived`: `static constexpr int vrs_by_preset[] = {0, 0, 0, 1, 1};`
  -> `ps_r__vrs` (Minimum/Low/Default off, High/Extreme conservative; the Perf phase's decision, 13.8 -
  the design's `{2, 2, 1, 1, 0}` lost frame rate on Low and Default). The `[preset]` line gains `vrs=%d`.
- Console (all `CCC_Runtime*`, never saved, registered in both modules):

| Command | Range | Meaning |
|---|---|---|
| `r__vrs` | -1..2, default -1 | session override: -1 preset, 0 off, 1 conservative, 2 aggressive (`ps_r__vrs_override`) |
| `r__vrs_threshold` | 0.25..4, default 1 | scales `s_mode` (calibration) |
| `r__vrs_force` | 0..2 | 1 every tile 2x2, 2 every tile 4x4 (plumbing, upper bound) |
| `r__vrs_debug` | 0..1 | rate overlay |
| `r__vrs_log` | 0..100000 | every N frames: `[vrs] mode=1 tiles=14400 1x1=58% 1x2=4% 2x1=5% 2x2=31% 2x4=0% 4x2=0% 4x4=2%` |
| `r__vrs_passes` | 0..255 (`(1 << VrsPass::Count) - 1`) | QA: bit n lets `VrsPass` n shade coarse (the per-pass A/B of V4) |
| `r__vrs_edge` | 0.25..4, default 1 | QA: scales `k_jump`, the depth ratio of a silhouette (calibration) |

- `r__caps` gains `;vrs=1` (DX12 and tier 2, shaders present) / `;vrs=0`.
- Video tab: no row - the preset owns it, and nothing preset-owned is on the page. The preset
  hint (`st_da_x64_video_hint_preset`, eng + rus cp1251 in `text/*/dead_air_x64_video.xml`) gets
  one sentence: lower presets on DirectX 12 shade flat and dark areas at a reduced rate (no
  "fast-moving": motion relief is gone in both modes, 13.3; rewritten with the final ladder after
  V7). `tools/qa/Check-OptionHints.py` must stay clean. The RTX category is not touched.
  Open after the Perf phase: the shipped sentence still reads "every preset but the highest" (the
  design ladder); with `{0, 0, 0, 1, 1}` it has to say "High and Extreme" (13.8).
- Log at renderer start: `[vrs] tier 2, tile 16, additional rates 1: mode 1 (preset 1, override -1)`
  or `[vrs] off: <no DX12 | tier N | MSAA | shaders missing | target creation failed>`.

## 8. Fallback

| Condition | Behaviour |
|---|---|
| DX11 (`xrRender_R4`) | module stubs; `vrs=0`; commands accepted, no effect; `[gpu] vrs=0.000` |
| DX12, tier 0/1 (Pascal, RDNA1, Xe-LP) | off, one log line; no targets created |
| no additional rates / tile 8 or 32 | CS clamps to 2x2 / adapts group layout (3.2) |
| MSAA on | off for the session (targets not created) |
| shader files missing / compile failure / texture creation failure | off, log line |
| history invalid (reset, first frame, mode switched on, recreated targets) | no image this frame -> 1x1 |
| pause menu PP, main menu, no level | no rate pass, nothing applied |
| a bad scope (image on a foreign-size target) | layer ignores the image for that draw, logs once |

## 9. Test plan

All stand work through `locked.ps1` (`Invoke-Ninja`, `Invoke-Stand` = `tools/qa/Run-WaterQa.ps1`
under `stand.lock`); runs are short (60-90 s), shader cache kept unless a shader changed.

### 9.1 Build and static checks
- `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` clean.
- `tools/qa/Test-Shaders.ps1 -Shader da_vrs_rate.cs`, `-Shader da_vrs_luma.cs`,
  `-Shader da_vrs_debug.ps`, plus the whole set (with the `.cs` extension added): 0 FAIL.
- `python tools/qa/Check-ShaderLinkage.py`: 0 mismatches (new pair `da_fullscreen.vs`/`da_vrs_debug.ps`).
- `python tools/qa/Check-OptionHints.py`: clean.

### 9.2 Layer tests (`tools/qa/d3d12layer`, `Run-LayerTests.ps1 -HighPerformance`, then `-Gbv -Filter vrs`)
New scenarios in `scenarios_vrs.cpp` (PLAN C20); the D3D11 run produces the reference by emulation
(a PS that evaluates its function at `floor(pos / r) * r + r/2`, and analytic counts as Values):
- `vrs_rates`: 256x256 target, PS counts invocations through a UAV `InterlockedAdd` and writes
  f(SV_Position); cases 1x1, per-draw 2x2, image 2x2/2x1/4x4 OVERRIDE, base 2x2 MIN image 4x4,
  half-width image. Pass: counts = 1, 1/4, 1/4, 1/2, 1/16 (1/4 without additional rates), 1/4,
  5/8 of the pixels; images equal the emulation exactly.
- `vrs_deferred`: image set on a deferred context, list executed on the immediate one after a
  CS wrote the image (UAV -> SHADING_RATE_SOURCE via the list's first requirement).
- `vrs_clear`: state gone after `ClearState`, `ExecuteCommandList(FALSE)`, `FinishCommandList(FALSE)`.
- `vrs_native`: `BeginNative` hands out a list at 1x1 (a native draw counts full invocations);
  the state returns after `EndNative`.
- `vrs_hazard`: image bound as CS UAV / on a larger target -> ignored, draw at 1x1.
Pass: the 36 existing scenarios + 5 new pass, debug layer and GBV silent.

### 9.3 Quality (stand, lossless shots)
New scenario `tools/qa/water/vrs_check.lua` (steps like `upscale_check.lua`): after 150 frames,
`wind_force 0`, then per step: set the console values, settle 1500 ms (TAA converged), one
frame of `r__screenshot_every 1`, 5 s measurement with `r__gpu_log 30` and `r__vrs_log 30`,
printing `[qa] vrs step <name> mode=<m> FPS <f>`. Steps: `ref0` (`r__vrs 0`), `ref1` (again, 700 ms
later: the noise floor), `cons` (`r__vrs 1`), `aggr` (`r__vrs 2`), `force2` (`r__vrs_force 1`),
`dbg` (`r__vrs 1`, `r__vrs_debug 1`), `back` (all 0). Runs (`-ExtraArgs '-r5 -always_active -ss_tga'`,
`-PreCommands 'vid_mode 2560x1440 (60Hz)','r__aa st_aa_taa','_preset Extreme'`):

| Save | Why |
|---|---|
| `qa_pedro` | perf baseline scene, walls and ground |
| `foliage` | worst case: grass, silhouettes everywhere |
| `trees` | canopy against sky (edge rule, sky tiles) |
| `fogqa` | forward water, fog |
| `fire` | particles, fluid fire |
| `rainhouse` (`-Weather af3_dark_rain`) | rain, puddle reflections, interior lamps (`Lights`) |
| `water-night` | dark scene (aggressive dark tiles), local lights |

Plus `qa_pedro` and `foliage` again with `r__dlss st_dlss_quality` and with
`r__fsr st_fsr_performance`, and `motion_check` with
`-Globals "qa_render_cmds = { 'r__aa st_aa_taa', 'r__vrs 1' }"` against the same with `r__vrs 0`
(flicker/ghosting of rate changes under a 30 deg/s turn, judged on the frame sequence).

New tool `tools/qa/Compare-Frames.py` (numpy + PIL, both present): for a pair of TGA shots it
masks pixels that moved between `ref0` and `ref1` (|diff| > 2/255 dilated by 2 px), then reports
PSNR, the 99.9th percentile error, the worst 16x16 tile mean error, a 2x2 blockiness index
(gradient energy on even pixel borders over odd ones, relative to `ref1`), and writes a
heat-map PNG. Pass criteria per save, against `ref1`, on the static mask:

| Metric | conservative (`cons`) | aggressive (`aggr`) |
|---|---|---|
| PSNR | >= 40 dB | >= 34 dB |
| p99.9 abs error | <= 12/255 | <= 24/255 |
| worst 16x16 tile mean error | <= 4/255 | <= 8/255 |
| blockiness increase | <= 5 % | <= 15 % |
| weapon rectangle (x > 0.55 W, y > 0.55 H), HUD pixels | PSNR >= 48 dB | >= 48 dB |
| coarse tiles (`[vrs]` log, non-1x1) | qa_pedro >= 25 %, foliage >= 8 % | qa_pedro >= 45 %, foliage >= 15 % |

Calibration: sweep `r__vrs_threshold` 0.5/0.75/1/1.5/2 on `qa_pedro` + `foliage` (same scenario,
`-Globals` switching the value) and fix `s_mode` at the largest value that meets every quality row;
if conservative then cannot reach 25 % coarse on `qa_pedro`, the conservative pass set loses its
weakest-gain pass instead of loosening quality. Visual checks on the `dbg` shots: no coarse
tiles on the weapon, on skyline edges, on shorelines; `r__dbg_sun_cascades 1` shot shows no
band along cascade splits.

Robustness: `render_check` with `-r5 -dxdebug` and a 60 s `-dx12gbv` run with `r__vrs 2`: no
debug-layer or GBV message; `upscale_check` with `r__vrs 1` in the header: every step runs, no
FATAL, shots at every step; `resize_check`: VRS comes back after a size change (log `mode 1`
after the restart, coarse tiles > 0).

### 9.4 Performance (holding `ninja.lock`, interleaved, 2560x1440, TAA, `r__gpu_log 300`)
`ab_run.ps1`-style interleaved pairs a/b/a/b on `qa_pedro` and `foliage`, Extreme content, with
Pre `r__vrs 0` vs `r__vrs 1` vs `r__vrs 2`; `analyze.ps1` means; only pairs whose busy GPU clocks
agree within 3 %. Pass:

| Check | Criterion |
|---|---|
| `r__vrs 0` vs the pre-change build (R5 and R4) | frame within +-1 %, `vrs=0.000` |
| overhead (`vrs=` field) | <= 0.10 ms at 1440p |
| `r__vrs 1`, qa_pedro | frame >= 5 % lower, `sun` >= 15 % lower, no other field higher by > 0.05 ms |
| `r__vrs 1`, foliage | frame >= 2 % lower |
| `r__vrs 2`, qa_pedro | frame >= 10 % lower |
| `_preset High` / `_preset Low` runs | log `mode 1` / `mode 2`, gains of the same sign |
| `r__vrs_force 1` | `sun` lighting part close to 1/3 of 1x1 (plumbing: every lighting draw reached) |

Decision rule: if conservative meets quality but misses the 5 % gate after calibration, High
drops to off (`{2, 2, 1, 0, 0}`); if aggressive misses 10 %, Low/Minimum keep it only if their own
`_preset Low` run gains >= 5 %. Applied in 13.8 by sign rather than by these thresholds (a preset keeps
the mode that gained on it): `{0, 0, 0, 1, 1}`.

## 10. Performance expectation

Pixel parts from 2.4 times the coarse fraction times the probe's cost ratio (2x2 = 0.25-0.33 of
1x1 once full-rate ROP writes are counted, 4x4 ~0.12-0.2):

| Pass | 1440p pixel ms | conservative (40 % coarse) | aggressive (60 %) |
|---|---|---|---|
| sun lighting | ~3.2 | -0.85 | -1.45 |
| forward | 1.35 | -0.25 | -0.45 |
| lights | 0.33 | -0.09 | -0.15 |
| sun-shaft blurs (all tiles) | ~0.5 | -0.35 | -0.35 |
| sky/clouds | 0.12 | -0.05 | -0.05 |
| combine_1 | ~0.5 | - | -0.18 |
| GTAO integral | ~1.2 | - | -0.36 |
| overhead (two CS) | | +0.06 | +0.06 |
| **total** | | **~-1.5 ms (~9 % of 17 ms)** | **~-2.9 ms (~17 %)** |

Grass-heavy views sit at the low end (silhouettes force 1x1): 2-4 %. Scaled to 1080p the absolute
numbers are ~0.56x. With DLSS/FSR the render size is smaller and the threshold stricter: roughly
half the gain. The clouds-march early-out (3.7) adds up to ~1 ms on High/Extreme in built-up
views, independent of VRS and of the API.

## 11. Risks

- **Representative pixel**: a coarse lighting invocation shades the block with the G-buffer of
  its bottom-right pixel; across a silhouette that is another surface (halo). Mitigated by the
  current-frame depth-jump rule; verified on `trees`/`foliage` shots.
- **Rate oscillation** (coarse shading changes the contrast it is measured from): the
  rate-aware correction and the 20 % hysteresis; judged on `motion_check` sequences.
- **Stale contrast after disocclusion / fast motion**: invalid reprojection -> 1x1; one frame of
  lag elsewhere, hidden by TAA; the motion term only relaxes, never tightens, the threshold.
- **Upscalers magnify blocks**: threshold scaled by render/display, aggressive capped at ratio >= 2.
- **State leaking** into shadow maps, native SDK work or the UI: scopes reset on exit, layer
  resets on ClearState/ExecuteCommandList(FALSE)/FinishCommandList(FALSE)/BeginNative, the
  foreign-size rule, and the `vrs_clear`/`vrs_native`/`vrs_hazard` layer tests.
- **Sun cascade splits**: a block straddling a split is shaded by the near draw with a pixel just
  beyond it; the cascades overlap, checked with `r__dbg_sun_cascades`.
- **GTAO noise** at 2x2 (aggressive only): the guided filter sees fewer distinct samples; the
  quality gate on `qa_pedro` decides, `AoRaw` leaves the table if it fails.
- **Other vendors** (tile 8, no additional rates) are not on the stand: the code paths are
  driven by caps and covered by the layer test at the reported tile size; the start log line
  names what the adapter reported.
- **Derivatives**: ddx/ddy span coarse quads, so mip selection in coarse passes is one level
  blurrier (particles, water normals, light cookies) - part of the quality budget above.

## 12. Implementation order

1. Layer: API, caps, state, apply, SRI requirement, safety rules, BeginNative reset, `-dxstats`
   counter; layer scenarios `vrs_*`; `Run-LayerTests.ps1 -HighPerformance` and `-Gbv -Filter vrs`.
2. `CBackend::set_ShadingRate` + cache reset; `da_vrs.h/.cpp` (mode, caps, table, scope) with DX11
   stubs; console commands, ladder, `[preset]` field, `r__caps vrs=`; GPU timer slots and the
   `vrs=` log field. Build both modules.
3. Resources (`vrs_create/destroy`), shaders `da_vrs.h`, `da_vrs_luma.cs`, `da_vrs_rate.cs`,
   `da_vrs_debug.ps/.s`, `phase_vrs_luma/rate/debug`; `Test-Shaders.ps1` learns `.cs`,
   `Run-WaterQa.ps1` copies TGA shots. Stand: `r__vrs_force 1` + `r__vrs_debug 1` on
   `render_check` - overlay shows the grid, nothing consumes it yet.
4. Scopes pass by pass in this order, each checked with `r__vrs_force 1` shots and the `[gpu]`
   field of that pass: `SunLight`/`SunVolumetric` (deferred contexts), `Lights`/`LightsVolumetric`,
   `SunshaftsBlur`, `SkyClouds`, `PuddleRefl`, `Forward`, `VolumetricCombine`, then `Combine1`, `AoRaw`
   (both removed at V5, 13.3).
5. Content adaptivity on (`r__vrs 1/2`), `vrs_check.lua`, `Compare-Frames.py`; calibration sweep
   (9.3); fix `s_mode`, `k_jump`, `k_m`.
6. Upscaler/FG/MSAA/menu/resize robustness runs (9.3), `-dxdebug` and `-dx12gbv`.
7. Performance gates (9.4) and the ladder decision rule; DX11 parity run.
8. Clouds-march early-out (3.7): moved to `async.md` (PLAN C14).
9. Docs: `DX12_RENDERER.md` (new 3.8 "Variable rate shading", console contract rows, `r__caps` key),
   preset hint strings (eng/rus), this file's status line.

## 13. Implementation record

Stand: RTX 3070 Laptop, VRS tier 2, tile 16, additional rates; 1920x1080 (the rig's size), Extreme,
TAA, `qa_tga` parity mode (game time and weather clock pinned, wind frozen). Eight other lanes shared
the machine, so single `[gpu]` fields vary by 10-30 % between steps; quality is judged on lossless
shots against two references of the same run (noise floor), performance only at V7.

### 13.1 What is in the tree

- Layer (`d12_vrs.cpp`, V1): `SetShadingRate` as context state; applied lazily per draw from
  `EnsureDrawStates` (a context that never set a rate costs one pointer test); the image requires
  `SHADING_RATE_SOURCE` (deferred contexts record it as a first requirement); the image is dropped for a
  draw (combiner PASSTHROUGH) when it is bound as an output or read by the draw, or when RT0 (or the DSV
  without RTs) is not covered by `image * tile` within one tile - each case logs once; below tier 2 or
  inside a native section the call is ignored; 2x4/4x2/4x4 fall back to 2x2 without additional rates;
  resets on `ClearState`, `ExecuteCommandList(FALSE)`, `FinishCommandList(FALSE)`, `BeginNative`;
  `-dxstats` counts shading-rate sets. Layer scenarios `vrs_rates`, `vrs_deferred`, `vrs_clear`,
  `vrs_native`, `vrs_hazard` (`scenarios_vrs.cpp`).
- Engine (`da_vrs.h/.cpp`, `r4_rendertarget_phase_vrs.cpp`, V2-V4): mode from `r__vrs` or the preset,
  the per-pass cap table, `da_vrs_scope`, targets behind `CRenderTarget::m_da_vrs`, the start line
  `* [vrs] tier 2, tile 16, additional rates 1: mode N (preset P, override O)` (R4:
  `* [vrs] off: no DirectX 12`), `* [vrs] mode ...` on every change, `r__vrs_log` histogram lines,
  `r__vrs_passes` (QA: bit n lets `VrsPass` n shade coarse). Scopes in the three accum files
  (SunLight on `accum_direct`, `accum_direct_cascade` and `accum_direct_f`; SunVolumetric; Lights on
  point and spot; LightsVolumetric) and, placed by core, SkyClouds (removed in the final phase, 13.7), PuddleRefl, Forward,
  VolumetricCombine, SunshaftsBlur (E0-E3).
- Rate image (`da_vrs_rate.cs`, `da_vrs_luma.cs`, V3/V5/V7): as 3.2, with these changes found by the
  calibration: the sky is no unconditional 4x4 - it may reach a quarter in either mode but only as far
  as its contrast allows; no motion relief in either mode (13.3); the sky reprojects by the camera's
  motion (its velocity texels are still zero when the rate pass runs); with an upscaler the contrast is
  measured after the upscale on `rt_Upscaled` (`da_vrs_luma_upscaled_pass`, one hook line in
  `phase_combine`), sampled at the render pixels' centres; an aggressive mode runs as conservative
  under any upscale.
- Group layout after the V7 overhead rework (13.5): the rate CS covers two by two tiles per group
  (tile 8: 16x16 px, tile 16: 32x32 px; tile 32: one tile), a thread 2x2 or 4x4 px; the depth apron
  comes in by `GatherRed` (a quarter of the loads); the motion vector and the previous rate are loaded
  before the first barrier, so a group waits on memory twice (depth, contrast); per-tile reduction by
  wave ops under GRIZZLY_WAVE_LANES (a device with wave ops, not under `-dxc_plain`; one lane per tile and
  wave contributes), groupshared atomics otherwise. The
  luma CS covers 32x32 px per group, a thread 4x4 px, and sums its 8x8 blocks from a per-thread
  groupshared array instead of atomics.
- Frame state: the rate pass publishes the frame's mode and image with a release store of the frame
  number, the scopes read it with an acquire load (the sun cascades may record on other threads); a
  render size the targets have not followed yet turns the frame off (`~ [vrs] render size ...` once)
  instead of handing a foreign-size image to a pass, `r__vrs_force` included.

### 13.2 Every scope reaches its pass (V4)

`r__vrs_force 1` (2x2 everywhere) with one pass at a time (`r__vrs_passes`), runs
`vrs-v4-passes-{trees,qa_pedro,fogqa,rainhouse-b}`. "Own change" = pixels > 4/255 where the step
differs from both neighbouring steps while those agree (the scene's drift cancels out):

| Pass | own change | `[gpu]` field, off -> on |
|---|---|---|
| SunLight | trees 2.3 %, qa_pedro 0.7 % (interior) | sun 3.29 -> 3.04 ms (trees) |
| SunVolumetric | trees 1.4 % | sun -1.11 ms (qa_pedro), -0.81 ms (trees) |
| Lights | qa_pedro 8.5 %, fogqa 2.8 % | lights 0.304 -> 0.276 ms |
| LightsVolumetric | trees 2.1 %, fogqa 0.5 % | below the noise |
| SkyClouds | trees 9.9 % | clouds 0.095 -> 0.065, 0.076 -> 0.059 ms |
| PuddleRefl | fogqa 0.9 % | below the noise |
| Forward | fogqa 0.9 % (the water) | forward 1.25-1.36 -> 1.12 ms |
| VolumetricCombine | trees 1.6 %, fogqa 1.3 % | below the noise |
| SunshaftsBlur | 0.1-0.7 % | post 0.618 -> 0.398, 0.613 -> 0.433 ms |
| Combine1 (removed, 13.3) | 17.7 % | below the noise |
| AoRaw (removed, 13.3) | 8-10 % | ao 0.712 -> 0.412, 0.596 -> 0.404 ms |
### 13.3 Calibration (vrs.md 9.3, `r__vrs_threshold` sweep)

`vrs-v5-pedro` (qa_pedro, the actor has empty hands: the "weapon rectangle" holds world pixels) and
`vrs-v5-foliage-b` (HUD hidden for the comparisons, shown for the overlay):

| Step | coarse | PSNR | p99.9 | worst tile | blockiness | rect | verdict |
|---|---|---|---|---|---|---|---|
| qa_pedro cons t=1 | 38.4 % | 52.9 dB | 5 | 3.68 | +0.45 % | 49.6 dB | PASS |
| qa_pedro cons t=1.5 | 58.6 % | 51.3 dB | 7 | 3.05 | +2.88 % | 47.8 dB | fail (rect) |
| qa_pedro cons t=2 | 69.8 % | 50.0 dB | 9 | 3.36 | +6.26 % | 46.3 dB | fail |
| qa_pedro aggr t=1, all passes | 82.6 % | 42.5 dB | 18 | 7.87 | +92 % | 39.6 dB | fail |
| qa_pedro aggr t=1 without Combine1 | 84.1 % | 45.3 dB | 14 | 5.78 | +29 % | 41.5 dB | fail |
| qa_pedro aggr t=1 without Combine1, AoRaw | 84.2 % | 46.7 dB | 14 | 5.28 | +8.3 % | 42.1 dB | PASS (rect: see below) |
| qa_pedro aggr t=0.75 / 0.5, all passes | 84.0 / 75.7 % | 42.7 / 43.6 dB | 17 / 14 | 7.8 / 7.2 | +88 / +63 % | | fail |
| qa_pedro turn 30 deg/s, cons | 59.0 % | 50.3 dB | 5 | 2.10 | +2.2 % | 49.2 dB | PASS |
| foliage cons t=1 (sky 4x4 always) | 27.9 % | 49.4 dB | 6 | 2.37 | +7.4 % | 48.2 dB | fail (sky) |
| foliage aggr t=1 / 1.5 | 36.7 / 37.4 % | 47.4 / 47.6 dB | 11 / 13 | 4.05 / 3.85 | +10.6 / +14.8 % | 45.6 / 47.6 | PASS / borderline |
| foliage DLSS quality cons / aggr (fixed luma) | 32.1 / 39.2 % | 51.7 / 51.6 dB | 3 / 5 | 2.26 / 3.11 | +0.6 / +1.0 % | 51.8 / 52.8 | PASS |

Decisions: `s_mode` stays 0.06 (conservative) and 0.15 (aggressive) - the largest tested values that
meet every row; **Combine1 and AoRaw are removed** (aggressive only: albedo and the GTAO integral at 2x2
are blocky at every threshold, +92 % and +29 % against 15 %; the scopes left `phase_combine` and
`phase_gtao`); the sky follows its contrast (the foliage failure was entirely in the top 40 % of the
frame: +4.2 % there, -0.02 % below); `k_m` aggressive 0 (at 30 deg/s aggressive went from 84 % to 91 %
coarse and the benches of qa_pedro showed 4x4 blocks), and after `vrs-v5-foliage-c` (turn steps: the
relief coarsened the sky and the canopy edges while the view turned, where TAA does not hide blocks
fixed to the screen) conservative lost it too - no motion relief in either mode. The same run's DLSS
quality steps moved aggressive under any upscale to conservative (the upscaler magnifies block errors;
the design only demoted at a ratio >= 2).
The rectangle row is meant for HUD pixels: HUD tiles are 1x1 by construction (any pixel nearer than
`r2_hud_depth_limit` forces 1x1) and the overlay shows the weapon untinted; where the rectangle holds
world pixels, aggressive cannot reach 48 dB by design (it is allowed PSNR >= 34 on the frame).
Upscalers: before the fix the contrast came from the upscaler's jittered, aliased input - 0.4 %
coarse tiles under DLSS quality on qa_pedro (38 % native), nothing but sky under FSR performance.

**Silhouette fix after the calibration (V7 code pass).** `vrs_w(nd)` passed the float4 of the four
neighbour depths to a float function (FXC warning X3206, implicit truncation): the depth-ratio
silhouette compared a pixel with its left neighbour only (sky/HUD neighbours were checked on all four).
Every number of the table above was measured with it. The float4 overload makes it the rule of 3.2 -
more 1x1 tiles along horizontal depth edges (roof lines, terrain against the far hills, branch
undersides) - so the coarse fractions above are an upper bound and the quality columns a lower bound.
`r__vrs_edge` (QA, scales `k_jump`) lets the final phase re-calibrate within one run.

### 13.4 Robustness (V6)

Code: no image without the previous frame's contrast (first frame, `da_history_reset()`, a luma gap),
the size guard (13.1), none in the pause-menu PP or outside a level, off with one log line for tier < 2,
an unknown tile size, MSAA, missing shaders or a failed target; the layer's safety rules (V1). Runs
taken before PLAN 0.1b (on the shaders before the V7 rework and the 15:49 sky/relief change):
`vrs-v6-dxdebug` (foliage, `-dxdebug`): turn, cons, aggr, force, dbg, 1080p -> 720p -> 1080p through
`vid_restart` - after each restart the start line, `mode 1` and coarse tiles (34.0 % at 720p, 35.8 %
back); the debug layer reported only two #1361 sampler warnings of device creation. Its `msaa` step
did not turn MSAA on (`o.msaa` is read when the renderer is created), so the MSAA fallback is left to
a start with `r3_msaa` set. `vrs-v5-foliage-c`: DLSS quality and FSR performance restarts brought VRS
back (3600 / 2040 tiles, 32 % / 29 % coarse).

### 13.5 Overhead (V7)

Before the rework: rate 0.09-0.12 ms + luma 0.04-0.05 ms at 1080p (force mode: rate 0.03 ms), about
0.25 ms at 1440p against the 0.10 ms row of 9.4. The rate CS waited on memory four times per group
(depth, velocity, contrast, previous rate) in 8160 groups of 16x16 px, with contended groupshared
atomics for tiles of 8; the luma CS reduced with 3 contended atomics per thread in 16x16-px groups. The
rework (13.1) leaves two waits per group, a quarter of the groups and of the depth loads at tile 16,
and no atomics in the luma pass. What is left is close to the bandwidth floor: both passes read one
render-size surface each (the RGBA8 frame, the 32-bit depth; 14.7 MB apiece at 1440p) plus an eighth
of the velocity - about 0.08-0.09 ms at 1440p on this GPU. Rule for the final phase: `vrs` <= 0.10 ms
passes the row; above it the passes stay only when the frame gain of `r__vrs 1` (which contains the
overhead) meets its own row, and the overhead row is recorded as the floor.

### 13.6 Final test phase (PLAN 0.1b, budget 0.1a)

`vrs_check.lua` carries the step sets (`qa_vrs_set`): `quality` (ref0, ref1, cons, aggr, force2, four
30 deg/s turn steps, dbg with the HUD shown, back), `upscale` (DLSS quality and FSR performance, each
with two references, cons and aggr), `robust` (cons, aggr, force 4x4, 720p and back, FSR quality with
aggr, native, dbg), `preset` (High, Low, Extreme through `r__vrs -1`). Every step prints `[qa] vrs step
... edge= vid_mode= FPS`, the `[vrs] mode=` histograms and `[gpu]` with `vrs=` (rate + luma, the mean
over the `r__gpu_log` window like every slot field; the single-frame `vrs_rate`/`vrs_luma` fields are
gone).
- V5, 4 runs (1920x1080, `-Tga`, `-ExtraArgs '-r5 -always_active -ss_tga'`, `-Globals 'qa_render_cmds =
  { "r__aa st_aa_taa" } ; qa_hide_hud = true'`): `quality` on qa_pedro and foliage, `upscale` on both.
  Compare-Frames on the static mask against the two references of the step group, rows of 9.3; coarse
  from the `[vrs]` lines. A failing quality row: sweep `r__vrs_threshold` (0.75, 0.5) in `qa_vrs_steps`;
  coarse below its row with every quality row met: sweep `r__vrs_edge` (1.5, 2), keep the largest value
  meeting every quality row and write it into `vrs_k_jump`; still short: the conservative pass set
  loses its weakest pass (9.3). dbg: weapon untinted, no coarse tile on skylines or shorelines.
- V6, up to 4 short runs: `robust` on foliage with `-dxdebug` (no message beyond the two #1361, after
  every restart the start line + `mode` + coarse > 0, `fsrq2` logs `mode 1`, force4 100 % 4x4);
  60 s render_check with `-dx12gbv` and `qa_render_cmds = { "r__aa st_aa_taa", "r__vrs 2" }` (GBV
  silent); render_check with `-PreCommands 'r3_msaa 2x'` (`* [vrs] off: MSAA`, `r__caps ...;vrs=0`);
  render_check with `-dx12_no_vrs` (`* [vrs] off: tier 0`). Layer tests: `Run-LayerTests.ps1
  -HighPerformance` (floor 77) and `-Gbv -Filter vrs`.
- V7: one `Invoke-Dx12Perf` batch on qa_pedro, `-Pairs 3`, A `r__vrs 0` / B `r__vrs 1` (protocol 0.5):
  frame >= 5 % lower, `sun` >= 15 % lower, no other field higher by > 0.05 ms, `vrs` per 13.5 (the
  windowed `vrs=` field of B). The `preset` set appended to the V6 `robust` run shows the ladder's modes. One R4 `render_check` on
  qa_pedro against `core-g0-render-pedro-{a,b,c}-r4` (the three accum files and the console block are
  shared engine code). Ladder rule (9.4): conservative short of 5 % -> High off (`{2, 2, 1, 0, 0}`);
  aggressive (Low/Minimum) kept only where a `_preset Low` run gains >= 5 %. The preset hint sentence
  (core) follows the final ladder. Once `rt` R2 is in: the `quality` set with `r__rt_shadows 1` on
  qa_pedro (C30).

Fix phase (review findings, code + build only, no runs):
- DXC-R1: the rate CS's wave reduction is guarded by `GRIZZLY_WAVE_LANES`, not `GRIZZLY_DXC` (dxc.md 3.3 step
  5): a device without wave ops or `-dxc_plain` gets the groupshared-atomics path, whose DXIL needs no
  wave feature. This machine defines `GRIZZLY_WAVE_LANES`, so its DXIL is unchanged.
- VRS-1: `r__vrs_log` maps the staging slot it is about to overwrite (copied three frames back, which
  the layer's EndFrame has already waited for) before the frame's copy, so the line is never dropped by
  `DO_NOT_WAIT`; it prints the mode of the frame the histogram belongs to.
- VRS-2: the single-frame `vrs_rate=`/`vrs_luma=` fields are gone; the overhead row reads the windowed
  `vrs=` slot field (PLAN 2.6).
- VRS-4: `Combine1`/`AoRaw` struck from 3.3/3.6, `r__vrs_passes` 0..511, PLAN 2.5's order line
  amended.

### 13.7 Final test phase, functional part (28.09.2026; `_work\dx12-features-20260928\final\stand.md`)

Runs `final-vrs-pedro`, `final-vrs-foliage`, `final-vrs-foliage-b`, `final-vrs-foliage-c` (1920x1080 Extreme, TAA,
`-Tga`, HUD hidden, `qa_vrs_measure_ms = 1300`), Compare-Frames on the static mask against `ref1` (static steps) and
`turn0b` (turn steps), rows of 9.3:

| Step | qa_pedro | foliage (all passes) | foliage, `SkyClouds` removed (final) |
|---|---|---|---|
| cons | 53.6 dB, p99.9 4, tile 3.4, +0.6 %, coarse 43 % - PASS | 48.3 dB, 7, tile 7.1 (sun glare), +3.0 %, coarse 37.8 % | 50.4 dB, 7, tile 7.5 (sun glare), +0.01 % - PASS |
| aggr | 48.4 dB, 12, 6.2, +7.9 %, coarse 84 % - PASS | 46.6 dB, 12, 7.7, +7.2 %, coarse 44.5 % - PASS | 49.6 dB, 8, 7.6, +1.98 % - PASS |
| turn1 (cons) | 51.6 dB, 4, 1.8, +0.6 % - PASS | 50.5 dB, 4, 2.3, **+24.5 %** - FAIL | 49.4 dB, 2, tile 20.8 (sun), +0.17 % - PASS |
| turn2 (aggr) | 48.5 dB, 8, 3.2, +5.7 % - PASS | 50.4 dB, 4, 2.3, **+32.9 %** - FAIL | 53.1 dB, 3, 1.6, +0.42 % - PASS |

- The worst tiles on foliage sit on the sun's glare and flare, which changes between any two shots with VRS off as
  well (`back` against `ref1`: the same 7.5 tile; `turn0` against `turn0b`: max 94/255 there): not a VRS error.
- The turn blockiness was the sky band above the horizon. Pass masks in one session (`final-vrs-foliage-b`): static
  cons +0.86 % with every pass, +0.42 % without `SkyClouds`, +0.47 % without `SunshaftsBlur`, -0.03 % without both;
  the turns pass without both. With `SkyClouds` alone removed (`-c`) every blockiness row is met, so by the rule of
  13.6 (the conservative set loses its weakest pass: `SkyClouds` saved 0.03 ms at force 1, 13.2) **`SkyClouds` is
  removed** in both modes - enum value, rule and the scope around `RenderSky()` + `phase_clouds_composite()` - and
  `SunshaftsBlur` (post 0.2 ms at force 1) stays. `r__vrs_passes` is 0..255.
- Ladder in the same session (`preset` steps on qa_pedro): High -> `mode 1`, Low -> `mode 2`, Extreme -> `mode 0`
  (`{2, 2, 1, 1, 0}` as built then; replaced by `{0, 0, 0, 1, 1}` in the Perf phase, 13.8).
- Robustness with everything on (`final-robust-pedro-b`, `-dxdebug`, 1280x720, `r__vrs 1`, RT, GD, mesh grass):
  no D3D12 error line, 12-13k shading rate sets per 300 frames, the mode and coarse tiles back after `vid_restart`.
- C30 (the quality set with `r__rt_shadows 1`) was not measured: the RT rows need a renderer restart that the
  quality set does not make, so its `rt0`/`rt1` steps ran without RT; VRS and RT shadows ran together without an
  error in the robustness runs.
- Found on the way (engine backend, `final\stand-fixes.md` S2): the first turn on qa_pedro hit a refused
  tree-shadow pipeline (stale pixel shader of a pooled backend on the immediate context) - not a VRS defect.

### 13.8 Final test phase, performance (Perf phase, 28.09.2026; V7 and the ladder)

Batches of `tools\qa\dx12\Invoke-Dx12Perf.ps1` on qa_pedro, 2560x1440, TAA, `r__gpu_log 300`, 2 pairs after the
warm-up run (the Perf-phase cap of PLAN 0.1a); summaries `qa\perf-vrs-pedro.summary.txt`,
`qa\perf-vrsladder-pedro.summary.txt`:

| Preset, mode | FPS off -> on | Verdict |
|---|---|---|
| Extreme, conservative (`perf-vrs-pedro`, vrs0 / vrs1) | 57.9 -> 60.2, **+3.9 %** over 2 valid pairs: frame -0.81 ms (`sun` -0.94, `combine` -0.58, `post` -0.38, `forward` -0.14), the rate + luma passes +0.19 ms | gain |
| Extreme, aggressive (vrs0 / vrs2) | 57.9 -> 60.7, +4.8 % (no pair within the 3 % clock rule) | gain, not chosen: no pair within the clock rule, and its quality rows are the looser ones (13.7) |
| Low, aggressive (`perf-vrsladder-pedro`, low0 / low2) | 95.0 -> 93.2, **-1.9 %** (-0.75 % over the 1 valid pair: frame +0.25 ms, `vrs` +0.17 ms) | loss |
| Default, conservative (def0 / def1) | 88.6 -> 87.7, **-1.0 %** (no valid pair) | loss |

On Low and Default the stand's GPU is hardly busy (every ladder run is flagged so): the frame is not paid by
per-pixel shading, and the fixed cost of the two compute passes is not earned back. V7's thresholds (5 % for
conservative, 10 % for aggressive) were not reached anywhere; the ladder follows the sign instead - VRS stays where
it gains and nowhere else:

- **`vrs_by_preset = {0, 0, 0, 1, 1}`** (`src\Layers\xrRender\xrRender_console.cpp`): Minimum, Low, Default off;
  High and Extreme conservative. High was not measured on its own; the decision puts it with Extreme. Aggressive
  stays reachable through `r__vrs 2` (session override, not saved).
- The sky/cloud-deck pass is out of the pass set (13.7, stand fix S3); `SunshaftsBlur` stays.
- One-pass cost with everything on (`final-onepass-foliage`, 1440p Extreme, `r__vrs 1`): `vrs` (rate image + luma)
  0.18 ms a frame.
- Not measured: C30 (the quality set with RT shadows), VRS on other vendors (tile 8/32, no additional rates).
