# DirectX 12 renderer, temporal upscalers and frame generation (1.5.0)

Status: implementation spec for 1.5.0. It supersedes the draft `TEMPORAL_UPSCALER_RFC.md` (its
invariants still hold: the native path stays equivalent to today's renderer, lifecycle lives in the
reset flow, no Lua/save dependency, an identity pass validates the plumbing before any SDK).
Sections 6-11 describe the DirectX 12-only features added on top (implemented and through their final
test phase on 28.09.2026; designs and measurements in `dx12-features/`).

## 1. Decisions

- **Two renderer modules, one render code.** `xrRender_R4.dll` (DirectX 11) and the new
  `xrRender_R5.dll` (DirectX 12) compile the same sources (`xrRenderPC_R4/sources.cmake`). R5 adds the
  `xrD3D12` library (`src/Layers/xrRenderDX12`), a native D3D12 backend that implements exactly the
  D3D11 interfaces the renderer uses: pipeline state objects with a persistent pipeline library,
  root signatures, descriptor rings, explicit barriers with per-subresource tracking, ring-buffered
  upload memory, and real parallel command lists for the renderer's deferred contexts (sun cascades,
  rain, HUD shadow). Every effect that exists on DX11 therefore exists on DX12 by construction.
- **Menu choice "Рендер: DirectX 11 / DirectX 12"** (`renderer renderer_r4|renderer_r5`), applied at
  the next start. DirectX 11 stays the default. The saved choice is read before the renderer module is
  selected (pre-read of the `renderer` line of user.ltx, as before 1.4.2's pinning); an adapter that
  fails `xrd3d12::ProbeAdapter` (feature level 11_0 + resource binding tier 2) never offers DX12 and a
  saved DX12 falls back to DX11 with a log line.
- **Anti-aliasing leaves the quality presets.** `r__aa` (Выкл / FXAA / SMAA / TAA) is saved and never
  written by `_preset`. Upscalers replace it while active.
- **Upscalers**: AMD FSR and NVIDIA DLSS, each its own row, mutually exclusive.
  - DX11: FSR 3.1.2 upscaler through the MSVC-built `optiscaler/FidelityFX-SDK-DX11` port
    (static libs); DLSS Super Resolution / DLAA through NVIDIA Streamline (`sl.dlss`, D3D11).
  - DX12: FSR 3.1.5 (FSR 4 on RX 7000/9000, chosen by the AMD loader) through AMD FSR SDK 2.3.0
    signed DLLs (ffx-api); DLSS through Streamline (`sl.dlss`, D3D12).
  - The DLSS row is hidden when the adapter does not support DLSS (Streamline
    `slIsFeatureSupported`), and a saved DLSS choice is switched off at start.
  - Streamline starts with the renderer on every NVIDIA adapter, so DLSS can be switched on without
    a restart; the first NGX query costs about two seconds of the start (`-no_streamline` skips it).
    Plugins downloaded into ProgramData are not loaded (ours are newer; mapping both cost two more
    seconds).
  - A frame without motion vectors (the save screenshot under the pause menu) is stretched and the
    DLSS history starts over; without the packed G-buffer (`r3_gbuffer_opt 0`) DLSS does not start.
- **Frame generation** (DX12 only): Выкл / AMD FSR / NVIDIA DLSS (2x, and 3x/4x where
  `numFramesToGenerateMax` allows). FSR FG = FFX frame generation + its swap chain; DLSS FG =
  Streamline `sl.dlss_g` with Reflex (`sl.reflex` + `sl.pcl`, required by DLSS-G). The row is shown
  on DX11 but disabled with a hint; unsupported entries are disabled/hidden per GPU.
- **FOV is vertical.** `Device.fFOV` is the vertical angle (`build_projection_HAT`, `_22 = cot(fov/2)`,
  aspect = H/W). Pass `deg2rad(Device.fFOV)` to FSR `cameraFovAngleVertical` and Streamline
  `cameraFOV` unchanged (the sibling project's H/W conversion is wrong for this engine).
- **The upscaler input is LDR.** The scene is tonemapped in combine_1 into RGBA8 `generic0`. No HDR
  flags, exposure 1, pre-exposure 1. DLSS also gets a tagged 1x1 R32F exposure texture holding 1.0:
  without one it runs its own auto-exposure. On D3D11 Streamline gets frame tags that are only valid
  now (it works on its own copies): DLSS read nothing out of the engine's targets directly there -
  a black frame without a single error.
- **Where DX12 does better, it does it that way**: the G-buffer grass (a fifth of the render thread on
  grassy levels) is recorded on three deferred contexts in parallel with the level (`parallel_details`
  in `r2_R_render.cpp`, `r2_mt_render 1`), and deferred command lists are submitted with the frame
  instead of one queue submission each. Measured on the stand (RTX 3070 Laptop, 1920x1080, TAA,
  `-always_active`, interleaved runs): grassy `foliage` save 76.0 FPS on DX12 against 73.8 on DX11,
  indoor `qa_pedro` 76.8 against 74.5.

## 2. Console contract

All of them are registered by `xrRender_console.cpp`, i.e. identically by R4 and R5 (a combo bound to
a command the running module lacks asserts). Token **names** are what user.ltx stores.

| Command | Type | Values | Saved | Notes |
|---|---|---|---|---|
| `renderer` | engine token | `renderer_r4` (id 5, "DirectX 11"), `renderer_r5` (id 7, "DirectX 12") | yes | id 6 stays OpenGL for old scripts |
| `r__aa` | token | `st_aa_off` 0, `st_aa_fxaa` 1, `st_aa_smaa` 2, `st_aa_taa` 3 | yes | default `st_aa_smaa`; drives `r2_fxaa`/`r__smaa`/`r__taa` (runtime) |
| `r__fsr` | token | `st_fsr_off` 0, `st_fsr_native` 1, `st_fsr_quality` 2, `st_fsr_balanced` 3, `st_fsr_performance` 4, `st_fsr_ultra_performance` 5 | yes | non-off switches `r__dlss` off |
| `r__dlss` | token | `st_dlss_off` 0, `st_dlss_dlaa` 1, `st_dlss_quality` 2, `st_dlss_balanced` 3, `st_dlss_performance` 4, `st_dlss_ultra_performance` 5 | yes | non-off switches `r__fsr` off; forced off at start when unsupported |
| `r__framegen` | token | `st_fg_off` 0, `st_fg_fsr` 1, `st_fg_dlss` 2, `st_fg_dlss_x3` 3, `st_fg_dlss_x4` 4 | yes | DX12 only; unsupported values fall back to off at start |
| `r__upscale_sharpness` | float 0..1 | default 0.3 | yes | FSR RCAS; post sharpen for DLSS |
| `r__caps` | read-only string | e.g. `api=11;dx12=1;dlss=1;dlssg=0;dlssg_max=0;fsr=1;fsrfg=1;hags=1` | no | for the menu (`get_console():get_string("r__caps")`); on DX12 `fsr` = the FFX loader (`da_ffx_loader.h`) has an upscaling provider for the adapter; the DirectX 12 feature keys that follow are in section 8 |

Changing `r__aa`, `r__fsr`, `r__dlss`, `r__framegen` or `r__upscale_sharpness` applies without a game
restart (`vid_restart` recreates targets, upscaler contexts and the swap chain for FG). Shader
permutations do not depend on them (velocity output and jitter are unconditional in the shaders).

## 3. Render resolution, jitter, motion vectors

### 3.1 Sizes
- `Device.dwWidth/dwHeight` = display (swap chain). New `Device.dwRenderWidth/dwRenderHeight` =
  render size, from the active upscaler's quality ratio (FSR ratios 1.0/1.5/1.7/2.0/3.0; DLSS from
  `slDLSSGetOptimalSettings`), even-rounded, min 64. Equal to display when no upscaler is active.
  The renderer owns it: `CRenderDevice::SelectResolution` resets it to the display size only when the
  display size changes (it used to do so in the `UpdateWindowProps` that follows every `Reset`, which
  after any `vid_restart` left render-size targets addressed at display size).
- Render-size targets: G-buffer (`rt_Position`, `rt_Color`, `rt_Accumulator`, `rt_Generic_2`),
  scene depth (`rt_MSAADepth`, split from `rt_Base_Depth` when the sizes differ), `rt_Generic_0/1`,
  `rt_Generic`, `rt_SSR`, AO targets, clouds (half render), sun shafts, SMAA/TAA targets, velocity,
  reactive, `rt_depth_copy`.
- Display-size targets: `rt_Base` (back buffer), `rt_Base_Depth`, `rt_Upscaled` (RGBA16F, UAV), the
  combine_2 output `rt_PostDisplay` when upscaling, `rt_ui` (PDA canvas), screenshots.

### 3.2 The upscale slot
Frame order with an upscaler active (without one, the order and every pass are exactly today's):
G-buffer → lights → combine_1 → SSR grab → puddles → forward (water, particles, glass, rain) →
volumetric combine → MSAA resolve → bloom build (render res) → distortion mask (render res) →
sun shafts (render res) → **reactive mask + upscale (generic0 → rt_Upscaled)** → **depth upscale**
(render depth → `rt_Base_Depth` by nearest sample, for lens flares, tracers, UI) → combine_2 at
display size (reads `rt_Upscaled`; G-buffer, distortion mask and bloom by UV) → lens flares →
`phase_pp` (reads `rt_PostDisplay`) → UI.
Screen-space overlays (visor drops and water, lens dirt, grain, chromatic aberration, dither, gamma)
are therefore never fed into an upscaler's history.
Binders: `pos_decompression_params2` = render size (G-buffer addressing, `Load(int3(tc*xy))`),
`screen_res` = the bound target. Post shaders that address the G-buffer by pixel position convert
through `tc * pos_decompression_params2.xy`.

### 3.3 Jitter
- Halton(2,3), phase count `ceil(8 * (display/render)^2)` (DLSS minimum 8), `index = frame % phases`,
  offset in render pixels in [-0.5, 0.5]. Active when TAA or an upscaler is active and no menu PP is
  up; otherwise 0 (then every matrix is bit-identical to today).
- Applied to the main-view projection handed to the contexts (`D3DXRenderBase::SetCacheXform`), the
  HUD projection (`hud_transform_helper`) and the Z-prefill projection; never to `Device.mProject`
  (it drives cascade fitting, culling, gameplay). Light-volume passes that re-set `Device.mProject`
  use the jittered main projection. Shadow maps, rain map, HUD shadow keep their own unjittered
  projections.
- Position reconstruction: `pos_decompression_params.xy += jitter_px * params.zw` (the binder folds the
  offset in; no shader change).

### 3.4 Motion vectors
- `rt_Velocity` R16G16_FLOAT, render size: `NDC(current, unjittered) - NDC(previous, unjittered)`,
  NDC y up. 0 = static relative to the camera motion already contained in the vector (camera motion is
  included).
- G-buffer MRT slot 2 (packed G-buffer, `GBUFFER_OPTIMIZATION`); every `deffer_*` pixel shader writes
  it; velocity is always compiled in, the target is bound only when TAA or an upscaler is active.
- Names (`shaders/r3/da_velocity.h`): the VS fills `hpos_cur`/`hpos_prev` (TEXCOORD7/8 of the
  `v2p_`/`p_` structs), the PS writes `O.velocity = da_velocity(I.hpos_cur, I.hpos_prev)` (`f_deffer`,
  `pack_gbuffer` defaults it to 0). Constants, per draw through `R_xforms`: `m_VP_nojit`, `m_VP_prev`
  (the HUD's pair inside `hud_transform_helper`), `m_WVP_nojit`, `m_WVP_prev`, `m_W_prev`; skinned:
  `sbones_array_prev` and `skinning_prev_0..4` (`skin.h`). C++: `da_vp_nojit/da_vp_prev/da_hud_vp`
  (da_temporal.h), `R_xforms::set_W_prev` for movers, `CRenderTarget::velocity_ready()` for readers.
  QA view: `r__motion_vectors 1` (runtime; renders the vectors even without TAA).
- Per-object previous transforms: static (identity world, previous VP), dynamic models
  (`m_W_prev` per draw through `R_xforms`), skinned models (previous bone palette uploaded as a second
  array, reseeded after visibility gaps), trees and grass (previous values of every wind uniform of
  our wind system, evaluated twice in the VS), HUD (previous HUD VP), water (our displaced surface,
  separate velocity pass), sky (analytic reprojection at far distance), particles/transparent (none:
  they go into the reactive mask).
- `da_velocity` clamps to +-16 NDC units: a previous w just above zero would overflow the 16-bit
  target to infinity, which the temporal resolvers keep in their history.
- Everything that feeds the vectors keys on the vectors being drawn (`velocity_ready()`,
  `da_motion_vectors_active() || r__motion_vectors`), not on TAA or an upscaler running: frame
  generation alone reads them too (sky and water passes, the previous wind bend of grass and
  trees). The sky pass finds the sky by position z == 0, so `rt_Position` is cleared on every
  preset while the vectors are drawn.
- Scale for SDKs: FSR `motionVectorScale = (-0.5*renderW, +0.5*renderH)`; Streamline
  `mvecScale = (-0.5, +0.5)`, `cameraMotionIncluded = true`, `depthInverted = false`.
- History reset: level load, teleports (camera jump > 25 m), menu → game, resolution/upscaler change.

### 3.5 Reactive mask
`rt_Reactive` R8_UNORM render size, from the opaque-only copy (`rt_SSR` grab before forward) versus the
final pre-upscale colour: transparents, particles, rain, fire, glass and water get high reactivity.
FSR `reactive`, DLSS `pInBiasCurrentColorMask`-equivalent through Streamline tag
(`kBufferTypeBiasCurrentColorHint`).

### 3.6 Mip bias
While an upscaler is active the scene samplers get `log2(render/display) - 1` added to
`r2_tf_mipbias`; UI/HUD draws after `Render()` restore the plain bias.

### 3.7 Frame generation
- **Manager** (`xrRenderPC_R4/da_framegen.cpp`, both modules compile it; DX11 answers "unavailable"
  and keeps the saved `r__framegen` for DX12). Backends register through `da_framegen_registrar`
  (FSR: `da_framegen_fsr.cpp`, token 1; DLSS: tokens 2..4). A saved value the adapter cannot run
  (`supported()`, `maxGenerated()`) is switched off at start with one log line; MSAA disables it; a
  backend that fails holds off until `r__framegen` changes.
- **Lifecycle**: `CRender::create` → `da_framegen_switch()` before the main target (no back buffer
  view exists: the swap chain is rebuilt through the backend's `SwapChainProvider`), then
  `da_framegen_start()` after the targets (`Init` at the display size, `$user$hudless` created).
  `vid_restart`: `da_framegen_stop()` before `Resources->reset_begin()`, `da_framegen_switch()` after
  the target is deleted (before `HW.Reset` resizes the chain), `da_framegen_start()` in `reset_end`.
  Providers are static objects: the swap chain keeps its provider until the device is destroyed.
  Without a backend the chain goes through Streamline's provider where it runs, else plain DXGI.
- **Per frame**: end of `CRender::Render()` (after `phase_pp`, before tracers/UI) →
  `da_framegen_frame()`: back buffer → `$user$hudless` (CopyResource, back buffer unbound), then
  `SetActive(true)` + `Prepare` (render-size scene depth, `$user$velocity`, hudless, jitter,
  unjittered current/previous matrices, camera vectors, frame index +1 per prepared frame,
  `reset` = history reset or first frame back on). FSR: `ffxConfigure` → `PrepareV2` on the native
  list; the generation runs inside `Present` through the swap chain's callback, the UI is separated
  by comparing the back buffer with the hudless copy. `D3DXRenderBase::End` → `da_framegen_present()`
  switches a frame nothing prepared off before `Present`.
- **Off** (`SetActive(false)`): main menu or no level, menu PP-UI, loading screen, `Device.Paused()`
  (pause, change-level window, tutorials), console, precache, Ansel, 4 presents after a start, no
  motion vectors this frame. Motion vectors are rendered while a backend runs
  (`da_motion_vectors_active()`), without jitter unless TAA or an upscaler runs.
- **FSR frame generation with DLSS upscaling**: the FSR swap chain is made through Streamline's
  factory proxy where Streamline runs (`da_streamline_factory`), so sl.common keeps seeing every
  present; the plain factory is the fallback. Checked on the stand: DLSS quality under FSR frame
  generation runs clean.
- **Frame limit**: `vid_fps_limit` keeps meaning shown frames: the renderer sets
  `Device.dwFramesPerRender` (generated + 1 while generating) and the CPU limiter paces rendered
  frames at `limit / dwFramesPerRender`. Where the renderer owns the limit (Reflex under DLSS-G,
  `IRender::OwnFrameLimit`) the CPU limiter stands aside. VSync: the FSR swap chain takes the sync
  interval from `Present` itself, and every present it makes, rendered or generated, goes through
  our present callback (the copy FFX's default does without a UI surface), which paces it on the
  waitable object of FFX's real swap chain (see the swap chain entry in 4; the real chain is kept
  by a forwarding factory FFX creates it through). With generation and vsync on, frames render at
  half the refresh and every refresh shows a rendered or a generated one - FFX's documented
  behaviour; without the pacing an Optimus laptop rendered at the refresh and dropped half.
- `r__caps` `fsrfg` = the FSR backend is registered and the loader has a frame generation provider
  for the adapter (`ffxQuery` GET_VERSIONS).

## 4. xrD3D12 architecture (details in `src/Layers/xrRenderDX12/d12_layer.h`)

- Frames: `Device::frame` advances at every Present; upload pages, descriptor chunks and command
  allocators are tagged with it and reused once `frame - 3` completed.
- Textures are tracked per subresource (immediate context: `Resource::states`; deferred contexts: a
  local tracker whose first requirements are patched in by `ExecuteCommandList`). Static buffers live
  in untracked DEFAULT pages (implicit promotion); dynamic buffers are renamed into upload memory on
  every DISCARD and re-read at every draw; staging textures are READBACK/UPLOAD buffers with placed
  footprints (Map returns the footprint row pitch).
- D3D11 hazards are reproduced: binding an output unbinds aliasing SRVs; binding an SRV of a bound
  output binds null.
- Graphics root signature: VS/PS b0..b4 root CBVs, CBV tables for the other slots, SRV and sampler
  tables per stage, PS UAV table; compute: b0..b3 root CBVs, tables. Descriptor tables are copied into
  a 1M-descriptor ring per draw when bindings changed; sampler tables are cached by content, and a
  full sampler heap is replaced by a new one (the old one is released after 4 frames; a draw retries
  once when the heap changed under it).
- Dynamic buffers keep their DISCARD versions in a persistent per-context heap, not in per-frame
  pages: the engine re-maps a constant buffer only when its contents change, so a version can stay
  bound for many frames, and the 4 MB dynamic vertex stream appends with NO_OVERWRITE across frames.
  The heap is `D3D12_HEAP_TYPE_GPU_UPLOAD` (CPU-visible video memory, resizable BAR) where the adapter
  reports `GPUUploadHeapSupported`, system memory otherwise (`-dx12_no_rebar` forces that); the
  device line of the log says which. On the RTX 3070 Laptop the difference is within the noise.
- Render targets and depth buffers are created with an optimized clear value (black; depth 1,
  stencil 0), which is what nearly every clear of the engine uses.
- Tests: `tools/qa/d3d12layer` runs 36 scenarios on real Direct3D 11 and on the layer and compares
  the results (debug layer and GPU-based validation silent); `-HighPerformance` picks the discrete
  GPU. Draw submission benchmark on an RTX 3070 Laptop: 0.30 us per draw on the layer against 0.73 us
  on the D3D11 driver.
- PSOs are keyed by content hashes of shaders and states (stable across runs) and stored in an
  `ID3D12PipelineLibrary` at `appdata/shaders_cache_grizzly/r5/pipelines.bin`.
- Queries: occlusion and timestamp heaps resolved into readback memory; event queries are fence
  values; `GetData` flushes like D3D11 unless `DONOTFLUSH`. Results are resolved when the list
  closes, one `ResolveQueryData` per run of consecutive slots (`Context::pendingResolves`): a resolve
  makes the GPU wait for the work before it, and one per query put that wait after every GPU timer
  stamp and every occlusion test.
- Swap chain: flip model always, three buffers at least; the engine's `GetBuffer(0)` is a proxy that
  resolves to the current buffer; frame generation replaces the native swap chain through
  `SwapChainProvider`. Three because on a D3D12 queue DXGI holds everything submitted after a
  Present until the next back buffer leaves the screen - the whole next frame, where a D3D11 driver
  waits only before the pass that writes the back buffer. With two buffers under vsync every
  frame was rounded up to whole refresh intervals: at 165 Hz, 12.5 ms of GPU work came out every
  18.2 ms, and DirectX 12 ran at two thirds of DirectX 11's frame rate on the player's machine
  while the stand, which presents without a display, never showed it.
- Vsync pacing (`d12_pacing.h`): the native chain is created frame-latency-waitable, and a vsync'd
  Present waits until every earlier present has left the queue (the object is released exactly
  once per present, also across `ResizeBuffers` and while minimized); without vsync it only counts,
  so tearing stays uncapped. DXGI itself bounds a vsync'd flip chain only by its buffers: N buffers
  accept N - 1 queued presents. Behind the NVIDIA Optimus copy to the integrated GPU (hybrid
  laptops: `xrEngine.exe` exports `NvOptimusEnablement`, and the driver profile the exe name
  matches enables it too; DXGI then reports the panel on the NVIDIA adapter) a buffer is free once
  it has been copied, so N - 1 presents leave the queue every refresh: with three buffers vsync
  let the game run at twice the refresh (330 fps at 165 Hz, half of it never shown), while DirectX
  11 with two buffers was capped. Measured on the player's RTX 3070 Laptop with a probe under the
  exe name: 330 -> 165 presents/s plain and through the FSR swap chain, FSR generating 165 -> 82.5
  rendered/s; GPU-bound rates unchanged (136 and 109 fps at 7.3 and 9.2 ms a frame). The FSR
  provider paces FFX's real chain from its present callback (3.7) and strips the flag from its own;
  DLSS-G's chain is left to Streamline.
- Submission: `ExecuteCommandList` does not submit. The immediate list so far and the deferred list
  are queued (`Context::m_queued`) and go to the queue in one `ExecuteCommandLists` at the next real
  submission (Present, `Flush`, an event query, a staging `Map` that has to wait, `GetData` without
  `DONOTFLUSH`); staging copies recorded in a deferred list are marked pending there and get that
  submission's fence.
- Creation-time uploads signal their own fence (`Device::initFence`). On the shared one they used
  up the value the deferred releaser had given to objects released since the last submission, so
  those could be freed while the submission that still read them ran.
- Annotation events (`PIX_EVENT`) reach the lists only with `-dxdebug` or when PIX, RenderDoc or
  Nsight is loaded into the process: D3D11 drops them otherwise, and recording every one cost about
  4% of the render thread.
- Shader linkage: D3D12 validates a pipeline as a whole, so a pixel shader must read exactly what its
  vertex shader writes (semantic, register, mask); D3D11 linked by register and tolerated mismatches.
  `python tools/qa/Check-ShaderLinkage.py` checks every pass pair of the game (C++ `r_Pass`, `.s`
  scripts, `uber_deffer`) in every permutation for both APIs. A refused pipeline logs the shader file
  names (`r4_shaders.cpp` names every shader on R5). The old sun cascades (`r2_shadow_cascede_old`)
  do not link and are switched off on DX12.
- Diagnostics: `-dxdebug` (debug layer to the log), `-dx12gbv` (GPU-based validation), `-dx12dred`,
  `-dxstats` (per-frame draws, PSO sets, root CBVs, descriptor tables, maps, barriers and submissions,
  averaged into the log every 300 frames; a grassy scene is about 4400 draws, 300 PSO sets, 4000 root
  CBVs, 330 SRV tables, 2 submissions), `-dx12_no_rebar` (dynamic buffers in system memory).
- GPU cost against DirectX 11 (the stand, RTX 3070 Laptop, 1440p Extreme with TAA, interleaved runs
  at equal clocks, `r__gpu_log`, no compile sharing the CPU): level, 15.8 ms a frame on both; per
  pass the cloud march is 0.15-0.25 ms faster and the ambient occlusion up to 0.15 ms slower, the
  rest within the noise. The CPU side of the layer is well under the D3D11 driver's.
- The sun shadow cascades are one texture array read through per-slice views; on R5 the shaders are
  compiled with `SMAP_ARRAY` and declare `Texture2DArray s_smap` (D3D12 cannot express a Texture2D
  view of slice > 0).

## 5. Packaging

New runtime files (top level, `runtime-files.txt`): `xrRender_R5.dll`, `sl.interposer.dll`,
`sl.common.dll`, `sl.pcl.dll`, `sl.reflex.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `nvngx_dlss.dll`,
`nvngx_dlssg.dll`, `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`,
`amd_fidelityfx_framegeneration_dx12.dll`, and for the DirectX 12-only features (section 6)
`dxcompiler.dll`, `dxil.dll` (Microsoft DirectX Shader Compiler 1.9.2607) and `dstorage.dll`,
`dstoragecore.dll` (Microsoft DirectStorage 1.3.0). SDKs are vendored under `Externals/` (headers, import
libs, the FSR DX11 static libs) with their licences; third-party notices in
`docs/dead-air/DEPENDENCIES.md`. No DirectX Agility SDK ships: everything runs on the in-box D3D12 runtime
(SM 6.6 where the driver offers it, RT at SM 6.5). The new shaders (`.cs`, `.ms`, `.as`, `r3/rt/*.hlsl(i)`, their
headers) travel in the `shaders/r3` overlay like every other shader.

## 6. DirectX 12-only features

Everything in this section lives in `xrRender_R5.dll` (the xrD3D12 layer or code under `XR_D3D12`).
`xrRender_R4.dll` compiles the same sources, registers the same console commands (a combo bound to a
missing command asserts) and renders exactly as before: its frames were checked equal to the Gate-0
references and its imports unchanged in the final test phase. Each feature reports availability through
`r__caps` (section 8) and has a command-line switch that takes it out for troubleshooting (section 10).
Designs, contradictions and every measurement: `docs/dead-air/dx12-features/` (`PLAN.md` and one file per
feature).

| Feature | Needs | Default | Set by | Doc |
|---|---|---|---|---|
| Native infrastructure | feature level 11_0, binding tier 2 (what R5 already requires) | always | - | `infra.md` |
| DXC / Shader Model 6.6 engine shaders | SM 6.0+ driver, `dxcompiler.dll` + `dxil.dll` | on | `r__dxc` (saved, next start) | `dxc.md` |
| Async compute (GTAO, water ripple) | a compute queue | auto: pixel passes on NVIDIA, compute queue on AMD and Intel discrete | Video -> Screen "Async compute" (`r__async_compute`) | `async.md` |
| Variable rate shading | VRS tier 2 | conservative on High and Extreme, off below | the quality preset (`vrs_by_preset`) | `vrs.md` |
| GPU-driven vegetation | nothing beyond R5 (ExecuteIndirect is core D3D12) | auto: while the CPU is the bottleneck | `r__gpu_driven` (runtime; -1 auto) | `gpudriven.md` |
| Mesh-shader grass | mesh shader tier 1, SM 6.5, DXC, no MSAA | auto: while the CPU is the bottleneck | `r__grass_mesh` (runtime; -1 auto) | `mesh.md` |
| DirectStorage loading | `dstorage.dll` + `dstoragecore.dll` | on | Video -> Quality "DirectStorage texture loading" (`r__dstorage`) | `dstorage.md` |
| Ray tracing ("RTX") | DXR 1.1 (tier >= 1.1), DXC, no MSAA | off | Video -> RTX, never a preset | `dxr.md` |

Look-neutral features (async, GPU-driven vegetation, mesh grass, DirectStorage) were checked frame-equal to
the same tree without them; VRS changes the look within measured quality rows and therefore rides the
presets; ray tracing changes the look on purpose and stays out of every preset, `rspec_*.ltx` and
`xrRender_sync_preset_derived`.

### 6.1 Native infrastructure (`infra.md`)

The xrD3D12 layer that emulates D3D11 for the engine also lets feature code record native D3D12 on any
context: `NativeCaps` (one query of every capability at device creation, masked by the `-dx12_no_*`
switches), native sections on the immediate and deferred contexts (`BeginNative`/`EndNative` with explicit
state hand-off), a persistent bindless range of 65536 descriptors next to the per-draw ring, root signature
and pipeline caches for native passes, buffer SRV/UAV views (raw, structured, typed), indirect draws with a
count buffer, acceleration-structure SRVs, and async compute jobs: a second queue with its own fence
(made by the first job, so a session on the pixel passes has none), jobs forked and joined inside one frame on the render thread, a lent-resource guard that joins a job
early when the direct queue touches what the job uses, and an in-order fallback on the direct queue
when no compute queue exists. `r__native_selftest` runs seven parts (immediate, deferred, async,
bindless, ray tracing, VRS, indirect) on the next frame and prints one `[qa] native selftest:` line.
Creation-time uploads (resources created with initial data) go through a copy queue the direct queue
waits for; `-dx12_no_copy_queue` keeps them on the direct queue (section 11 has the load numbers). Layer
tests: `tools\qa\d3d12layer`, 81 scenarios, debug layer and GPU-based validation silent.

### 6.2 Shader compiler: DXC and Shader Model 6.6 (`dxc.md`)

On R5 every engine shader is compiled by DXC 1.9.2607 into DXIL at `min(device, DXC, 6.6)`: FXC's
preprocessor with the engine's includer and macros, then HLSL 2018, `-Zpr -O3
-flegacy-resource-reservation`, the three legacy warning classes silenced, macros `GRIZZLY_DXC`, `GRIZZLY_SM`,
`GRIZZLY_WAVE_LANES` (wave ops), `GRIZZLY_MIN16` (16-bit min precision). Native passes (RT, self-test) compile
at HLSL 2021 through `da_dxc_compile_native`; mesh and amplification shaders go through the engine path
at HLSL 2021. Cache `appdata/shaders_cache_grizzly/r5_dxil/<tag>/` next to the FXC cache `r5/`. A shader
DXC refuses falls back to FXC and its pass to DXBC twins (a pipeline is all DXIL or all DXBC; the layer
refuses a mix with a named log line). Mesh grass and ray tracing need DXIL, so FXC (`r__dxc 0`,
`-dx12_no_sm6`) switches them off. Measured: no pass slower than under FXC; cold start with an empty
cache 61 s against 108 s on `qa_pedro` Extreme; the TAA pass 3-5 % faster with quad-shared moments
(`-dxc_plain` compiles without the wave/quad/min16 paths). Log: `* [dxc] dxcompiler 1.9.0.5402, engine
shaders SM 6.6 (device 6.8), waves 32-32, min16 yes` and `* [dxc] shaders: N compiled, M cached, 0 on
FXC, 0 twins`.

### 6.3 Async compute (`async.md`)

Two passes have compute versions with results identical to their pixel shaders (checked on the GPU by
`r__async_verify`: 0 differing texels): GTAO (render + filter) and the water ripple solver. On the async
path they are recorded on pooled contexts as jobs: the ripple at fork A (frame start, overlapping the
shadows and the G-buffer), GTAO at fork B (after the G-buffer, overlapping the sun); one join at the
combine head. The cloud map stays on the direct queue, and the cloud march is a pixel pass after the
G-buffer with a depth early-out (texels hidden by geometry are not marched) on every DirectX 12 path;
DirectX 11 marches every texel as before. Under RTAO there is no GTAO job. `r__async_compute`
(`st_async_off` / `st_async_auto` / `st_async_on`) is the player's row; auto is decided once per renderer
start: `* [async] auto: pixel passes (NVIDIA: no measured gain)`, `compute queue (AMD)`, `compute queue
(Intel discrete)`, pixel passes for integrated GPUs, unknown vendors and without a compute queue. On the
RTX 3070 Laptop every "on" variant lost frame rate (off 54.3 FPS, on 50.0 at 1440p Extreme): the extra
work in flight lowers the clock of the power-limited GPU more than the overlap saves. The compute cloud
march, `r__async_march`, `r__async_join` and the split joins were removed for that reason.

### 6.4 Variable rate shading (`vrs.md`)

Tier 2 image-based VRS. A compute pass builds the shading-rate image each frame from the previous
frame's luminance contrast (reprojected), the current depth (silhouettes force 1x1), the sky and the HUD
(always 1x1); `* [vrs] tier 2, tile 16, additional rates 1: mode M (preset P, override O)` at start.
Eight passes bind it with a per-pass cap: sun lighting, sun volumetric, local lights, light volumetrics,
puddle reflections, forward, volumetric combine, sun-shaft blurs. The G-buffer, shadow maps, anti-aliasing,
upscalers, display-size post and the UI never see it; the sky/cloud pass was removed from the set (its 4x4
blocks showed in the sky under camera turns). Modes: 1 conservative, 2 aggressive (aggressive runs as
conservative under any upscaler). The ladder `vrs_by_preset = {0, 0, 0, 1, 1}` (Minimum, Low, Default off;
High, Extreme conservative) comes from the Perf phase: Extreme conservative +3.9 % FPS at 1440p
(aggressive +4.8 %), Low aggressive -1.9 %, Default conservative -1.0 %: the rate image costs 0.17-0.19 ms
and pays only where the frame is pixel-bound. Quality on `qa_pedro` conservative: 53.6 dB against VRS off,
blockiness +0.6 %, 43 % coarse tiles.

### 6.5 GPU-driven vegetation (`gpudriven.md`)

Trees (`MT_TREE_ST`/`MT_TREE_PM`) are registered at level load when every pass of their HQ, LQ and shadow
elements has a `(DA_TREE_GD,)` vertex shader variant with the `GDREF` instance input; a crown and its
trunk never split. Their wind runs in a compute pass over every root, each view (main, sun cascades, rain,
Z-prefill) culls them in compute (`da_gd_tree_cull/args/scatter.cs`) and draws them with one
`ExecuteIndirect` per group of cells instead of a CPU draw per instance. The main view keeps the CPU node
decisions (HOM, value filter, LOD switch). Local-light faces cull the trees on the CPU (a face sees a few;
a GPU view per face cost 1.56 ms for a campfire's six faces) and draw them through the same GD passes and
GPU wind. `* [gd] trees: N instances, M roots, ...` at load; `[gd] instances main=.. shadow=.. draws=..`
with `r__gpu_log`. `r__gpu_driven` -1 auto (default, section 6.9), 1 GPU-driven, 0 the CPU path, 2 CPU cull
+ GPU draw (QA). Phase 2 (Hi-Z of the main view) and phase 3 (the main view's nodes and
impostors on the GPU) were measured and removed: they cost more than they save (section 11).

### 6.6 Mesh-shader grass (`mesh.md`)

Grass rows live in a GPU-resident pool (48 MB); each frame the calc task uploads a row list, an
amplification shader culls 32 rows per group against the exact raster volume of the pass, and a mesh
shader emits the tufts with the same maths as the vertex shaders (`da_detail_common.h`). Used in the
G-buffer, the sun cascades 0/1 and the local-light maps. Runs when `r__caps` `ms=1`, DXC is on, MSAA is
off, the first mesh pipeline is accepted and the level's detail vertex shaders include
`da_detail_common.h`; otherwise, and for any refused pipeline (verdict stored in
`render-grass-mesh.cache`), the vertex path. `* [grass-mesh] on: N objects ...`; `r__grass_stats` prints the
counters and a GPU row check; `r__dbg_grass_ab` compares both paths in one frame. Render-thread grass time on
foliage 8.6 ms (vertex path) -> 1.8 ms. The shadow passes draw depth-only elements (the same tufts, sway and
alpha test as the colour elements, no G-buffer outputs); the G-buffer Hi-Z variant was measured and removed
(section 11). The preset grass radius and density do
not change with mesh grass. `r__grass_mesh` -1 (default) runs it while the CPU is the bottleneck (6.9).

### 6.7 DirectStorage (`dstorage.md`)

Textures inside the game's archives (raw and LZO blocks) and the level geometry (`level.geom`,
`level.geomx`) are read by DirectStorage straight into placed resources of pooled texture heaps; LZO is
decoded on 8 codec threads. Loose files, XMS overlays, video textures and anything else the path cannot
take load the usual way, per texture. `r__dstorage` (saved, default 1) applies at the next load;
`r__texload_stats` prints the state, the heaps and the last batches; `-dstorage_verify N` compares N
textures of each batch (and every geometry buffer) with the old path. The loader logs `* [texload] batch
early: T textures queued, L loads (ds raw R, ds lzo Z, cpu C, failed F), MB, wall W ms`. A 30 s timeout turns
DirectStorage off for the session and parks what is in flight. Verified in the final stand: 1086 textures
and 64 geometry buffers, 0 mismatches, frames unchanged.

### 6.8 Ray tracing: the RTX category (`dxr.md`)

Inline ray queries (DXR 1.1) in compute passes at `cs_6_5`, bindless through the persistent range, own
temporal and a-trous filters (no third-party denoiser). The acceleration structure holds what never moves:
the level's static geometry in BLASes per 256 m cell (opaque and alpha-tested apart) and multiple-use models
as TLAS instances with masks (rigid props cast; crowns and trunks only in reflections, trunks near the
root also in AO), built and compacted on the level's first frame with an effect on (`* [rt] scene: N BLAS,
T tris, MB compacted, TLAS I instances, ... ms total`). With every effect off the level is not walked, no
structure exists and the pass pipelines are not compiled. Effects:
- **Sun shadows** (`r__rt_shadows`): static casters are traced (soft edges from a jittered sun cone,
  denoised; hard on Low) and leave the near and middle cascades; vegetation, characters and moving
  objects keep their shadow maps, so plant shadows still sway; the far cascade stays complete.
- **Ambient occlusion** (`r__rtao`): one cosine ray per pixel (half resolution on Low/Medium) with a
  temporal pass, replacing GTAO/SSAO in the AO slot.
- **Reflections** (`r__rt_reflections`): water and rain puddles trace one mirror ray per requesting pixel;
  on-screen hits reuse the lit frame, off-screen hits are shaded from albedo, sun and hemisphere; no
  pixel requests a ray where there is no water or puddle.
- **Quality** (`r__rt_quality` Low/Medium/High): resolution, ray reach (reflections 250 / 400 / 1000 m) and
  filtering.

Options -> Video -> RTX holds the four rows (`depend="vid"`: a toggle recreates the targets through
`vid_restart`). The rows are always shown and greyed with the reason: on DirectX 11 "needs DirectX 12" (or
"needs DirectX 12 and a GPU with hardware ray tracing" when the adapter's probed tier is below 1.1), on
DirectX 12 without `rt=1` "no hardware ray tracing (DXR 1.1): GeForce RTX, Radeon RX 6000 and newer, Intel
Arc", and the quality row while no effect is ticked. Saved values are kept for DirectX 12 while DX11 runs and
switched off at start on a DX12 adapter without RT. `r__rt_debug 1..4` shows the sun mask, AO, reflections,
or the traced albedo against the G-buffer.

### 6.9 The offloads' auto choice (`gpudriven.md` 3.13)

GPU-driven trees and mesh grass move CPU work to the GPU. On a player's save (campfire at night,
k00_marsh, 2560x1440 Extreme TAA, RTX 3070 Laptop) the frame is GPU-bound and both lose: 43.9 FPS with both
on (the 28.09 defaults) against 52.0 with both off; DirectX 11 49.7. So `r__gpu_driven` and `r__grass_mesh`
default to -1: `da_offload.cpp` compares the main thread's work of the frame (from `SimulationStart` to
`OwnFrameLimit`, minus the CPU/GPU sync point's wait and `Present`) with the GPU timers' `Frame` slot, both
smoothed; CPU-bound by `r__offload_margin` for `r__offload_agree` s -> the GPU paths, GPU-bound -> the CPU
paths, at most once per `r__offload_dwell` s. A switch must shorten the frame within `r__offload_dwell` s or
it is undone ("no gain"); a path left less than `r__offload_recall` s ago comes back only if it was faster,
and once after that time it is tried again ("recheck"). `r__offload_warmup` s after a level load are not
measured (the post-load frames are CPU-bound for a while). Without GPU timers: the CPU paths. Each switch
logs `* [offload] auto: <reason> -> trees cpu|gpu, grass cpu|gpu (cpu .. ms, gpu .. ms, frame .. ms)`;
`r__gpu_log` adds `* [offload] path= cpu= gpu= frame=`. Local-light faces cull their trees on the CPU in every
mode (6.5). DirectX 11: auto = 0.

## 7. DirectX 12 feature console contract

Registered by `xrRender_console.cpp` in both modules; inert on R4. "Saved" commands go to user.ltx; runtime
commands (`CCC_RuntimeInteger/Float`) and calls are never saved. Nothing here is written by `_preset` except
the internal VRS mode of the ladder (`ps_r__vrs`, not the `r__vrs` override).

| Command | Type | Values | Default | Saved | Notes |
|---|---|---|---|---|---|
| `r__dxc` | integer | 0..1 | 1 | yes | engine shaders through DXC; next start |
| `r__native_selftest` | call | - | - | no | infrastructure self-test on the next frame (R4: "DirectX 12 only") |
| `r__async_compute` | token | `st_async_off` 0, `st_async_auto` 1, `st_async_on` 2 | auto | yes | Video row; applies without a restart |
| `r__async_path` | runtime integer | 0 the setting, 1 pixel, 2 compute inline, 3 compute async | 0 | no | QA |
| `r__async_verify` | call | - | - | no | pixel vs compute comparison of every async pass (port, then schedule stage) |
| `r__vrs` | runtime integer | -1 the preset, 0 off, 1 conservative, 2 aggressive | -1 | no | session override |
| `r__vrs_threshold` | runtime float | 0.25..4 | 1 | no | contrast threshold scale (calibration) |
| `r__vrs_edge` | runtime float | 0.25..4 | 1 | no | QA: silhouette depth-ratio scale |
| `r__vrs_force` | runtime integer | 0 off, 1 every tile 2x2, 2 every tile 4x4 | 0 | no | QA plumbing check |
| `r__vrs_debug` | runtime integer | 0..1 | 0 | no | rate overlay |
| `r__vrs_log` | runtime integer | 0..100000 | 0 | no | every N frames a `[vrs] mode= tiles= ...` histogram |
| `r__vrs_passes` | runtime integer | 0..255 (bit n = VRS pass n) | 255 | no | QA per-pass mask |
| `r__gpu_driven` | runtime integer | -1 auto, 0 CPU path, 1 GPU-driven, 2 CPU cull + GPU draw | -1 | no | auto: section 6.9 |
| `r__grass_mesh` | runtime integer | -1 auto, 0 vertex path, 1 mesh | -1 | no | auto: section 6.9; the preset replay takes it into account |
| `r__offload_margin` | runtime float | 0..1 | 0.1 | no | auto: how much longer the bottleneck side is |
| `r__offload_agree` | runtime float | 0..60 s | 3 | no | auto: how long a verdict holds before a switch |
| `r__offload_dwell` | runtime float | 0..600 s | 10 | no | auto: the least time between two switches |
| `r__offload_recall` | runtime float | 0..3600 s | 120 | no | auto: how long a path left stays remembered |
| `r__offload_smooth` | runtime float | 0.05..10 s | 1 | no | auto: smoothing of the CPU and GPU times |
| `r__offload_warmup` | runtime float | 0..120 s | 30 | no | auto: not measured after a level load or a return to -1 |
| `r__grass_stats` | call | - | - | no | mesh-grass counters and a GPU row check |
| `r__dbg_grass_ab` | runtime integer | 0..1 | 0 | no | one A/B of both grass paths on the next frame, resets itself |
| `r__dstorage` | integer | 0..1 | 1 | yes | Video row; from the next texture load |
| `r__texload_stats` | call | - | - | no | DirectStorage state, texture heaps, last batches, the level's sync loads |
| `r__rt_reflections` | integer | 0..1 | 0 | yes | RTX row, `vid_restart` |
| `r__rt_shadows` | integer | 0..1 | 0 | yes | RTX row, `vid_restart` |
| `r__rtao` | integer | 0..1 | 0 | yes | RTX row, `vid_restart` |
| `r__rt_quality` | token | `st_rt_low` 0, `st_rt_medium` 1, `st_rt_high` 2 | medium | yes | RTX row |
| `r__rt_debug` | runtime integer | 0 off, 1 sun mask, 2 AO, 3 reflections, 4 traced albedo vs G-buffer | 0 | no | QA view |
| `r__gpu_log` | integer | 0..100000 | 0 | yes | every N frames one `[gpu]` line (section 9) |

## 8. `r__caps` keys

`get_console():get_string("r__caps")`, read-only, never saved. Key order:
`api;dx12;dlss;dlssg;dlssg_max;fsr;fsrfg;hags;rt;rt_tier;vrs_tier;vrs_tile;vrs;ms;sm;wave;fp16;eb;acq;dxc;gd;dstorage`
(the stand's RTX 3070 Laptop on R5: `rt=1;rt_tier=12;vrs_tier=2;vrs_tile=16;vrs=1;ms=1;sm=68;wave=32;fp16=1;eb=1;acq=1;dxc=1`).

| Key | R5 | R4 |
|---|---|---|
| `api`, `dx12`, `dlss`, `dlssg`, `dlssg_max`, `fsr`, `fsrfg`, `hags` | section 2 | section 2 |
| `rt` | RT usable this session: tier >= 1.1, DXC up, RT device ready, no MSAA, no `-dx12_no_rt` | 0 |
| `rt_tier` | device DXR tier (0/10/11/12) | the adapter's tier from the D3D12 probe cache (`render-caps-d3d12.cache`), 0 unknown |
| `vrs_tier`, `vrs_tile` | VRS tier (0/1/2); tile size at tier 2, else 0 | 0 |
| `vrs` | VRS runs: tier 2, shaders present, targets created | 0 |
| `ms` | mesh shader tier 1 and `sm >= 65` | 0 |
| `sm` | `min(device, DXC)` shader model as 60..69; 50 without DXC | 50 |
| `wave` | wave lanes when wave ops are on and `sm >= 60`, else 0 | 0 |
| `fp16` | native 16-bit ops and `sm >= 62` | 0 |
| `eb` | enhanced barriers (information; the layer does not use them) | 0 |
| `acq` | a separate compute queue is available (made by the first job) | 0 |
| `dxc` | engine shaders are DXIL this session | 0 |
| `gd` | the GPU-driven registry is live | 0 |
| `dstorage` | DirectStorage runtime and queues are up (independent of the `r__dstorage` tick; 0 after a timeout) | 0 |

## 9. `r__gpu_log` keys

`r__gpu_log N` prints every N frames one line with the mean of every frame resolved since the previous one:
`* [gpu] frame= scene= sun= lights= clouds= march= combine= ms (ao= forward= bloom= aa= post=) ripple= join= vrs= gd=
grass= rt_scene= rt_sun= rt_ao= rt_refl=`, then the registered fields, then `frames=`.
`tools\qa\dx12\Invoke-Dx12Perf.ps1` parses every `key=<number>`.

| Key | Times |
|---|---|
| `frame` | `CRender::Render` from the CPU/GPU sync point on (GPU; since 29.09 the first timestamp rides with the frame's own work, not with the sync point's submission, so a CPU-bound frame no longer counts the GPU idling until Present) |
| `scene`, `sun`, `lights` | G-buffer; sun accumulation (cascades included); local lights |
| `clouds`, `march` | cloud cache retrace + sky/cloud composite; volumetric cloud march (half resolution) |
| `combine` (`ao`, `forward`, `bloom`, `aa`, `post`) | combine with AO (GTAO/HDAO/SSAO or RTAO), forward (water, particles, sorted), bloom, SMAA/FXAA/TAA, post (shafts, upscale, final combine) |
| `ripple`, `join` | ripple steps on the direct queue; the direct queue's wait at the async join |
| `vrs` | rate image + luma contrast |
| `gd` | GPU-driven main-view cull, args and scatter |
| `grass` | G-buffer grass (vertex or mesh path) |
| `rt_scene`, `rt_sun`, `rt_ao`, `rt_refl` | acceleration-structure builds and updates; RT sun shadows; RTAO; RT reflections |
| `async` (R5 field) | the frame's path: 0 pixel, 1 compute inline, 2 compute async |
| `cq_ripple`, `cq_gtao` (R5 fields) | compute-queue job times, 0 when not forked this frame |
| `grass_cpu`, `grass_mesh` | CPU grass time per frame over all contexts (ms); 1 when the mesh path drew |

On the same cadence GPU-driven trees print `* [gd] instances main=.. shadow=.. draws=.. records=.. views=..`
(local-light faces' CPU-culled instances included) and the offloads' auto choice
`* [offload] path= cpu= gpu= frame= ms warmup= s` (6.9).

## 10. Command-line switches

| Switch | Effect |
|---|---|
| `-r5` | start on DirectX 12 (falls back to DirectX 11 when the adapter fails the probe) |
| `-dx12_no_rt` | masks DXR (`rt_tier=0`, `rt=0`); saved RT rows are switched off for the session |
| `-dx12_no_vrs` | masks VRS (`vrs_tier=0`, `* [vrs] off: tier 0`) |
| `-dx12_no_ms` | masks mesh shaders (`ms=0`, vertex-path grass) |
| `-dx12_no_async` | no compute queue (`acq=0`): pixel passes; jobs forced by `r__async_path 3` run in order on the direct queue |
| `-dx12_no_sm6` | no Shader Model 6: engine shaders on FXC (`dxc=0`, `sm=50`), hence no RT and no mesh grass |
| `-dx12_no_rebar` | dynamic buffers in system memory instead of GPU upload heaps |
| `-dx12_no_copy_queue` | creation-time uploads on the direct queue instead of the copy queue |
| `-no_dstorage` | DirectStorage never loads (`dstorage=0`) |
| `-dstorage_verify N` | compare N textures of each load batch plus every geometry buffer with the old path (`verify: N textures, M mismatches`) |
| `-dstorage_buffered` | buffered DirectStorage reads (taken automatically on drives with a seek penalty) |
| `-dxc_plain` | DXC without the wave, quad and 16-bit paths (own cache folder) |
| `-dxc_all_bound` | DXC with `-all-resources-bound` (QA, own cache folder, unmeasured) |
| `-dxc_debug` | DXIL with embedded debug info for PIX/Nsight |
| `-dxdebug`, `-dx12gbv`, `-dx12dred`, `-dxstats` | debug layer; GPU-based validation; DRED; per-300-frame layer counters (draws, PSO sets, forks/joins and guard joins, mixed PSOs, indirect draws, shading-rate sets, mesh dispatches) |

## 11. Measured cost (final test phase, 28.09.2026)

At the user's request the Perf phase ended with one measurement pass instead of on/off pairs, so most numbers
below are per-feature GPU timers of one run, not A/B deltas. `final-onepass-foliage`: 2560x1440 Extreme, TAA, RTX
3070 Laptop, everything on (async forced on, `r__vrs 1`, GD 1, mesh grass 1, RT shadows + RTAO + reflections at
High, DirectStorage): **49 FPS, GPU frame 20.2 ms**.

| Pass (ms a frame) | Cost |
|---|---|
| `scene` / `sun` / `lights` / `march` | 2.06 / 5.9 / 1.40 / 1.44 |
| `combine` (`ao`) | 4.13 (1.82: RTAO in the AO slot, replacing GTAO) |
| `rt_scene` (TLAS refit) / `rt_sun` / `rt_ao` / `rt_refl` | 0.38 / 1.75 / 1.83 / 0 (no water in view; 0.75-0.78 on the fogqa ponds, `final-rtx-fogqa`) |
| `vrs` (rate image + luma) | 0.18 |
| `gd` (GPU-driven cull) | 0.12 |
| `grass` (mesh grass, G-buffer) | 1.86 GPU, 2.5 ms render thread |
| `cq_ripple` (compute queue, overlapped) / `join` | 1.04 / 0.035 |

RT scene of l01_escape: 417 BLAS, 1.57 M triangles, 42.7 MB compacted (104.3 before), TLAS 7897 instances, built in
659 ms at load. No shader, DXC or D3D12 error line in the log.

A/B numbers that do exist:
- Async, `perf-async-pedro` (qa_pedro 1440p Extreme TAA): off 54.3 FPS, on 50.0, on with the pixel march 52.1 (-3.9 %
  against off over 2 valid pairs), on with split joins 51.6 -> auto = pixel passes on NVIDIA, compute march and
  split joins removed (6.3).
- VRS, `perf-vrs-pedro` / `perf-vrsladder-pedro`: Extreme conservative +3.9 % (aggressive +4.8 %), Low aggressive
  -1.9 %, Default conservative -1.0 % -> `{0, 0, 0, 1, 1}` (6.4).
- Look-neutral set, stand logs at 1920x1080 Extreme SMAA: foliage 68.9 FPS with the DXC-only tree
  (`final-dxconly-foliage`) against 81.4 with GD, mesh grass, DirectStorage and async on (`final-on-foliage`;
  render-thread grass 8.6 -> 1.8 ms); qa_pedro 64.5 -> 65.1 (`final-dxconly-pedro` -> `final-on-nods-pedro`).
- DXC against FXC (D4, before the final tree): no pass slower, GPU work 5-9 % lower at equal clocks.
- Optional variants, one pass: `final-onepass-variants-foliage` = the run above plus `r__gpu_driven_hiz 1`,
  `r__gpu_driven_main 1`, `r__grass_mesh_hiz 1`, `r__grass_mesh_depth 1` and `-dx12_copy_queue`. Last `[gpu]`
  lines: frame 20.30 -> 20.41 ms, `scene` 2.07 -> 2.26, `gd` 0.12 -> 0.34, `grass` 1.89 -> 1.94, `sun` 5.95 ->
  5.77, FPS 49.0 -> 47.8; load: `level.geom` 256 -> 214 ms, `level.geomx` 58 -> 47 ms, texture batch create
  611 -> 548 ms, 616 MB of creation-time uploads off the direct queue, no error line. Decisions: the GD phases 2
  and 3 and the grass Hi-Z removed (`gd`, `scene` and `grass` up, frame slower); the depth-only grass shadows
  kept without a switch (look-neutral by construction, `sun` down); the copy queue on by default
  (`-dx12_no_copy_queue` masks it).

Not measured: DirectStorage load time, RT Low/Medium and the VRAM peak
with frame generation, other GPU vendors.
