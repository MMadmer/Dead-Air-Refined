# DXR ray tracing - the "RTX" category (DirectX 12)

Status: implemented (28.09.2026). R0-R6 in the tree; the functional part of the final test phase
passed (9.2: every effect reviewed, sway kept, level change under `-dxdebug`), the Perf phase read the
cost of each effect from one run with everything on (9.3). RT stays outside the presets, in its own
"RTX" category of Options -> Video. Target: `xrRender_R5.dll` only;
`xrRender_R4.dll` (DirectX 11) compiles the same sources and must render bit-identically with every RT
setting, because on it the settings are inert.

## Integration amendments (PLAN.md)

Implemented by the `rt` lane (milestones R0-R6 of `PLAN.md` 4.6; phases 4 and 5 are in scope).
These points supersede the text below where they differ (ids refer to `PLAN.md` section 1):
- **C1**: DXC is **1.9.2607**, one copy in `Externals/dxc` vendored by `core` (K1); not 1.8.2502.
- **C2**: `da_dxc` belongs to the `dxc` lane (`dxc.md`): the loader is the layer service
  `d12_dxc.cpp`, RT shaders compile through `da_dxc_compile_native` (HLSL 2021, FS include handler),
  cached under `shaders_cache_grizzly/r5_dxil/<tag>/native/rt/...`, not `r5/rt/`. This lane owns no
  compiler code.
- **C7**: `r__caps` keys are `rt` (RT usable this session: tier >= 11, DXC service up, RT device init
  ok, no MSAA, no `-dx12_no_rt`; from this lane's `da_rt_available()`) and `rt_tier` (infra; on DX11
  from `render-caps-d3d12.cache`). `rthw` is dropped - the menu tests `rt_tier >= 11`. The switch is
  `-dx12_no_rt` (masks the cap); `-no_rt` is dropped.
- **C8**: no RT descriptor heap, upload ring or own deferred release. Bindless textures and raw
  buffers go into infra's persistent bindless range (`AllocateBindless`, `WriteBindless`,
  `WriteBindlessBuffer`); per-pass tables come from `AllocateDescriptors`; constants and instance
  descs from `AllocateUpload`; lifetimes through `ReleaseNative`; root signatures and PSOs through
  infra's caches. The RT root signature binds its unbounded `Texture2D[]` (space100) and
  `ByteAddressBuffer[]` (space101) tables at `NativeScope::bindless`.
- **C9**: layer API names are infra's: `NativeBufferRange` (for `BufferLocation`), `NativeDescriptor`
  (for `ViewDescriptor`), plus `RequireMany` and `TextureWriteGeneration`, which infra adds at I2;
  the generation also bumps on texture creation/destruction, so material descriptors are refreshed
  after a reload.
- **C13**: `r__gpu_log` fields are `rt_scene`, `rt_sun`, `rt_ao`, `rt_refl` (numeric), through
  `core`'s field registry.
- **C16**: the level walk visits every sector's `root()` **and** its `gd_nodes`
  (`da_gpu_trees_for_each_node`); after classifying visuals it calls `da_gpu_trees_refresh_flags()`
  so GD's shadow-map cull skips `rt_static` instances in the near/middle cascades. `rtx-trees` and
  `rtx-foliage` run with `r__gpu_driven` 1 and 0.
- **C18**: RTAO reuses the GTAO targets; their creation condition
  (`ssao_mode == gtao || da_rt_needs_ao_targets()`) sits in the `async` lane's block of
  `r2_rendertarget.cpp`; async records no GTAO job while RTAO runs.
- **C19**: no rename of the water collector: `phase_water_velocity_collect` becomes idempotent per
  frame and `phase_rt_reflections` calls it early; the existing call stays.
- **C20**: layer scenarios in `scenarios_rt.cpp`.
- **C25**: `Test-Shaders.ps1` skipping `r3/rt/` and new pipelines in `Check-ShaderLinkage.py` go
  through the `dxc` lane's files (this lane's `rt` block).
- **C32**: off-screen reflection hits show crowns in their static pose (reflections do not sway);
  accepted - the sway rule concerns shadows, on-screen hits reuse the swaying frame.
- **Ownership**: `core` registers the commands of 4.1 (both modules, never in a preset), the `rt` /
  `rt_tier` caps keys, timer slots, writes the RTX group of section 7 (rows, nodes, strings, script
  rules), and pre-places every call site of section 4 in `r2_R_render.cpp`, `r2.cpp`,
  `r2_loader.cpp`, `render_phase_sun.cpp`, `da_temporal.cpp`, `r4_rendertarget_phase_combine.cpp`,
  `r2_rendertarget.cpp`, plus `dxRender_Visual::rt_static`, `o.rt_skip_static` and the
  `insert_static` early return. The RT scene build runs before async fork A (`PLAN.md` 2.5).
- **R0 as built** (decisions of the `rt` lane, supersede sections 4.1, 5 and 8 where they differ):
  - The TLAS is bound through a one-descriptor table at `t0 space2` (an acceleration structure view,
    `xrd3d12::CreateAccelerationStructureView`), not a root SRV at `t0`: section 5 had the root SRV and
    the pass-input table both on `t0 space0`, and a root descriptor may never be null, while an
    acceleration structure view of location 0 is a valid empty scene (every ray misses). Passes without
    a scene get the device's empty-scene view. The geometry/instance/material root SRVs (`t0-t2 space3`)
    of a pass that does not use them point at a 256-byte stand-in buffer for the same reason.
  - Constants: every pass declares its own `b0` block (root CBV from `AllocateUpload`, at most 4 KB),
    instead of one constant layout for all passes.
  - Tables per dispatch: one `AllocateDescriptors` span of 25 (scene, 16 inputs, 8 outputs), inputs
    `RequireView(NON_PIXEL_SHADER_RESOURCE)`, outputs `RequireView(UNORDERED_ACCESS)`, both volatile
    ranges; the bindless `Texture2D[]` (`space100`) and `ByteAddressBuffer[]` (`space101`) tables are
    set once per native section at `NativeScope::bindless` (`DaRtSection`, `da_rt_device.h`).
  - `da_rt_available()` = device part ready (not masked by `-dx12_no_rt`, tier >= 1.1, native shader
    model >= 6.5, `ID3D12Device5`, root signature, the probe pipeline `rt/rt_probe.hlsl` and one probe
    dispatch read back correctly at renderer start) and no MSAA. It never looks at the three effect
    settings. A device that cannot run RT switches the saved effect values off with one log line (4.1);
    MSAA keeps them (effects inactive, rows greyed while it is on), like frame generation.
  - The per-frame latch makes an effect live only when RT is available, its setting is on, the scene
    is built and every pipeline of its passes exists; `da_rt` (binder) = (reflections, shadows, AO live,
    quality), zeros on DX11. At R0 no pass exists, so nothing is live and no frame changes by design.
  - `gbuffer_pack.h` holds the `USABLE_BIT_*` defines, `da_gbuf_ground` and the normal/hemi/material
    pack/unpack functions moved verbatim out of `common_functions.h` (which includes it at the same
    place); `rt/rt_common.hlsli` includes it together with the bindings and the instance/ray masks.
- **R1-R6 as built** (28.09.2026, code complete, untested until the final test phase of `PLAN.md`
  0.1b; decisions of the `rt` lane, they supersede sections 3-6 where they differ):
  - **Root signature** (`da_rt_device.cpp`): the three table SRVs became four root SRVs `t0-t3 space3`
    (static geometry records, static instance records, this frame's geometry records, this frame's
    instance records) plus a root UAV `u0 space3` (raw output of the pre-skinning pass); unused ones
    point at 256-byte stand-ins. There is no separate material table: the albedo index and the
    material flags live in the 32-byte geometry record (`vb, vb_offset, ib, ib_offset, layout, layout2,
    albedo, flags`), the layout word carries the vertex decode (stride, normal offset/kind, texcoord
    offset/kind, tangent/binormal offsets for the lightmap texcoord's fraction bits), found from the
    engine's own vertex declaration (`dcl_code`), not from a list of known formats.
  - **Masks**: `0x40 NOSHADOW` added - static geometry whose shader has no shadow element; the rays
    of the cascades never saw it cast, so shadow rays (`0x03`) do not either; occlusion rays are
    `STATIC|STATIC_ALPHA|TRUNK|DYNAMIC|SKINNED|NOSHADOW`, reflection rays `0xFF` (Low: without
    `FOLIAGE`). `rt_static` is set only on what enters the shadow mask (static meshes and rigid
    multiple-use models with a shadow element); `da_gpu_trees_refresh_flags()` right after the walk.
  - **Scene** (`da_rt_scene.cpp`): walk of every sector root and `da_gpu_trees_for_each_node`
    (visited set) at the end of `level_Load` when an effect is wanted then, else on the first frame
    that wants one (a level played with every effect off is never walked); static meshes grouped by 256 m cell x class into
    `PREFER_FAST_TRACE|ALLOW_COMPACTION` structures, multiple-use models one structure per unique
    geometry (window 0) instanced with `FTreeVisual::world()`; builds on the first frame that has an
    effect switched on whose targets exist, in scratch batches of <= 64 MB, compacted-size query, one
    `Flush(wait)`, compaction copies, then the top level (capacity: static + 512 moving instances).
    Budget: prebuild sizes + scratch + tables against `QueryVideoMemory`; under 512 MB left, the
    crowns and trunks are dropped (their structures are then not built; the saving counts each
    structure that only crowns and trunks name, once, however many trees share it); still under, the scene is
    refused for the level with one `!` line. Freed with the collection when every effect is off (walked
    again when one comes back); dropped at `level_Unload`. "Wanted" = setting on, pass pipelines and
    targets there. The pass pipelines are made with the targets of the first effect switched on on
    the device (renderer start or the restart an RT row asks for): a device that never traces pays
    only the root signature and the probe at start. Textures: one bindless range for every static albedo
    (records index it relative to `RtView.scene.y`); on a texture-write-generation change the views
    are compared and, when one changed, written into a fresh range (the old one freed after the
    fence); every change requires the textures in `NON_PIXEL_SHADER_RESOURCE` again (`RequireMany`).
    The season's texture reload (`CResourceManager::SyncTextureSeason`) calls `da_rt_textures_replaced`
    after each chunk's new views exist and before it releases the old ones: the tables move to the new
    views at once (the read state follows on the next frame's refresh), so no slot holds a released view
    (the layer's `! [d3d12] bindless: a view was released...` line).
  - **Moving objects** (R4, R5): while occlusion or reflections run, `SpatialSpace.q_sphere` around
    the camera (Low 40 m, Medium/High 60 m), skipping the view entity, HUD and invisible renderables.
    Plain meshes use the engine buffers; model parts riding one bone (`RM_SINGLE`) get their
    bind-pose vertices (the part's shared `vertBoned*` data) uploaded once in the scene's own 24-byte
    format and are instanced with `world * LL_GetTransform_R(bone)`; skinned parts of the nearest
    6/12/16 characters within 20/30/30 m (every part of each: the parts sort by distance, then model,
    and the cut falls between two models) get the pre-skinning source (64 bytes a vertex) uploaded once per model,
    `rt/rt_skin.hlsl` with this frame's palette (as `sbones_array` is uploaded), a
    `PREFER_FAST_BUILD|ALLOW_UPDATE` structure refitted every frame and rebuilt every 30. The top level
    is then rebuilt every frame (`PREFER_FAST_BUILD`); with shadows alone it keeps the static one (a
    top level that still holds moving objects is rebuilt static once). Unused structures go after
    300 frames. Hemisphere of moving objects = 0.75 x their ROS hemi (as `apply_object`).
    Lifetimes (final phase, RT-2/RT-6): a model can be freed during the level (breakables), so the
    engine buffers the moving objects register get a reference (`AddRef`) and their textures a
    `ref_texture`, both looked up every frame they are used and let go with the other entries after
    300 unused frames - a freed model's address never names another buffer or texture while a cache
    holds it. A model part's uploaded vertices keep the shared data's checksum and length: a reused
    address with other data is uploaded again under a new serial, which is part of the structure's
    key; a character part whose source, index buffer or window changed gets a structure sized and
    built anew (never a refit). Records whose buffers have no bindless slot are left out; the
    per-frame tables are written before the top level is built, which takes the static instances
    alone when they could not be; a top level that could not be rebuilt while it held moving objects
    leaves the passes on the empty scene until a build succeeds.
  - **Targets** (`r4_rendertarget_phase_rt.cpp`, created with the render targets only for the effects
    switched on): `rt_sun_raw` RG16F, `rt_sun_hist0/1` RGBA16F (value, frames, view z, hit distance),
    `rt_sun` R8 (the mask), `rt_ao_half` RG16F (visibility, view z; half or full size by the quality
    the renderer started with), `rt_ao_hist0/1` RGBA16F, `rt_refl_req` and `rt_refl` RGBA16F,
    `$user$rt_debug` RGBA16F on first use of `r__rt_debug`. A console toggle without `vid_restart`
    finds no targets and the effect stays off (the rows restart the renderer).
  - **Sun** (`da_rt_sun_pass`): trace -> temporal (velocity when drawn, else the depth through the
    previous view-projection; 3x3 clip; alpha max(1/n, 0.1), 32 frames) -> a-trous step 1 into
    `rt_sun_raw` -> step 2 into `rt_sun`; Low traces a hard shadow straight into `rt_sun`. Pixels facing
    away from the sun still trace (the offset flips to the sun's side): leaf cards lit from behind
    keep their static shadow. `accum_sun_near.ps` multiplies, `accum_sun_far.ps` takes the minimum,
    both behind `da_rt.y` (`da_rt_consumer.h`); `s_rt_sun` bound in the three non-MSAA sun elements.
    A change of the shadow latch invalidates the cached middle/far maps (`r__sun_cache_ms`).
  - **Occlusion** (`da_rt_ao_pass`): trace (half: the nearest pixel of each 2x2) -> temporal (Low 16,
    Medium/High 32 frames) -> depth-aware upsample into `rt_gtao` -> the GTAO guided filter
    (`s_gtao` element 1, pixel pass) into `rt_ssao_temp`; `combine_1.ps` takes `s_occ` behind `da_rt.z`
    and skips the preset's own technique then.
  - **Reflections** (`da_rt_reflections_pass`): the water collector (`phase_water_velocity_collect`,
    idempotent per frame) -> requests: `da_puddle_refl_req.s` (fullscreen, same gates as the puddle
    pass) then the water items with `da_water_rtreq.s` / `da_water_green_rtreq.s` (by the surface's
    pixel shader; `water_rtreq.ps` = `water.ps` with `DA_WATER_RT_REQUEST`, returning after `vreflect`;
    separate `.s` scripts because a script's `normal` function is elements 0 and 1 alike) -> trace
    with on-screen reuse (unjittered projection, 8 px margin with a 32 px fade) and off-screen shading
    (`rt/rt_material.hlsli`: albedo at the ray-cone mip, vertex normal and hemisphere, static sun
    shadow ray, the combine's `hmodel` terms without the material table, the combine's haze incl.
    height/rain/sky colour, the combine's tonemap and grade with this frame's exposure). Off-screen
    hits get no vegetation shadows (no cascade lookup: slices 1-2 hold static casters or not by the
    shadow latch); on-screen hits carry them. `water.ps` uses the result where the request at the
    pixel is this surface's (depth within 2 cm / 0.2 %), blurred along the slope on rough water; the
    other water body keeps SSR. `da_puddle_refl.ps` takes it (sky where the ray missed) where the
    request at the pixel is the puddle's own (kind 1, same depth test); where water drawn in front of
    the ground asked for its ray there, the puddle keeps its sky/march path. Bindings in
    `effects_water.s`, `effects_waterryaska.s`, `effects_waterstuden.s` (unowned files: two lines
    each) and `da_puddle_refl.s`.
    Amended 29.09.2026 (marsh report: flickering grey/dark squares in the water under reeds and grass,
    Extreme preset, DLSS, all three effects on Medium):
    - Both consumers widen the request depth match by `0.75 * fwidth(depth)`: under variable rate
      shading (on in the High and Extreme presets) the water and puddle passes interpolate at the
      coarse pixel's centre, half a pixel from the fine pixel whose request they load, and on water
      seen at a grazing angle past a few metres the depth moves more than the 0.2 % tolerance in half
      a pixel. Every coarse tile (16x16) then fell back to SSR/sky, and as the tile's rate follows the
      previous frame's contrast, which the fallback itself changes, it flickered.
    - Screen first: a march along the mirror ray over this frame's depth (uniform screen steps, 1/z
      linear, the crossing bisected five times and kept within 5 cm + 1 % of the depth; Low/Medium/High
      at most 16/24/32 steps) up to just short of the traced hit takes what the frame shows in the
      ray's way: grass and mesh grass (never in the scene) and crowns and reeds as they sway (the scene
      holds their rest pose). A traced hit the frame shows is still reused, one it does not show is
      still shaded. The output is premultiplied by its confidence (the frame's edge fade); the puddle
      consumer blends the sky by it.
    - The frame's textures are read at the jittered projection of a point (`RtView.frame.xy`, the
      jitter in render pixels): the unjittered one sampled them up to half a pixel off, so the reuse
      test flipped on thin reeds from frame to frame under TAA/DLSS.
    - A traced hit within `0.5 m + 5 %` of the view depth on the back of a solid surface (the ray runs
      the way its vertex normal faces) is the reflecting ground itself where the raster drew it lower
      than the traced mesh (a coarser progressive window, the fp16 G-buffer depth): the ray goes on
      from past it. Before, wet mud reflected its own texture instead of the sky and the reeds.
    - The alpha test clips at the G-buffer's reference (`da_aref_u`, `RtView.frame.z`), not at a
      fixed 128/255 (Extreme clips at 100/255: traced vegetation was thinner than the drawn one); a
      cut-out whose mask texture is unknown is not solid (it was a whole grey card), and the scene
      walk leaves such meshes to the raster, their cascade shadow included.
  - **Tiers** (`kTiers`, `da_rt_passes.cpp`): Low = hard shadow without filter, half-res occlusion
    1.0 m / 16 frames, reflections 250 m without crowns and without off-screen sun rays, 6 characters;
    Medium = soft filtered shadow, half-res 1.5 m / 32, 400 m, 12 characters; High = full-res 2.0 m,
    1000 m, 16 characters. Reflection reach is also capped at the haze's far end. Screen steps of a
    reflection ray: 16 / 24 / 32 at most.
  - **Debug** (`r__rt_debug` 1-4): `rt/rt_debug.hlsl` into `$user$rt_debug`, blitted over the frame
    after the reflections (`da_rt_debug.s`); 4 = primary-ray albedo (left half) against the G-buffer
    albedo (right half).
  - **QA**: `tools/qa/water/rtx_check.lua` (steps of section 9, `qa_sway` two shots 0.5 s apart,
    `qa_debug` the four views).
## 1. Goal

Three hardware ray traced effects on DirectX 12, each switchable on its own in a new **RTX**
subsection of Options - Video, never touched by the quality presets:

| Effect | Replaces / augments | Traced against |
|---|---|---|
| RT reflections | water and rain puddle SSR (`ogse_reflections.h`), which stops at 45 m on water and at the screen edge | whole level |
| RT sun shadows | the static part of the near and middle sun cascades | static world only - everything that moves keeps its shadow maps, so plant shadows keep swaying |
| RTAO | GTAO / HDAO / SSAO in the combine AO slot | static world, rigid props, tree trunks near the ground |

Plus a quality row (Low / Medium / High). Requirements: DX12 renderer, DXR 1.1 (inline ray
queries, Windows 10 2004+), shader model 6.5, the DirectX Shader Compiler DLLs shipped with the
game. Everything else shows the rows greyed with the reason, like frame generation.

Hard rules this design keeps: plant shadows sway in every shadow technique (no vegetation in any
shadow ray's instance mask); RT is not in `_preset`/`rspec_*.ltx`; DX11 unchanged; no names of other
mods anywhere (material classes come from shader flags).

## 2. Current code (what RT plugs into)

Measured on this machine by a probe (D3D12 device on the RTX 3070 Laptop, driver of 28.09.2026):
raytracing tier 0xC (1.2), resource binding tier 3, highest shader model 6.8, mesh shader tier 1;
an SM 6.6 `ResourceDescriptorHeap` PSO is accepted without the Agility SDK on Windows 11 26200.
Synthetic 2.08 M-triangle heightfield (16 geometries, 16-bit indices, stride 32 like `level.geom`):
BLAS 70 B/tri before and 26 B/tri after compaction, scratch 114 MB, build 26.8 ms (78 Mtri/s),
compaction copy 0.46 ms; a 10 k-tri fast-build/update BLAS is 0.61 MB; a 30 k-instance TLAS is
8.6 MB + 3.1 MB scratch. DXC compiles a cs_6_5 RayQuery shader in 25 ms. Inline ray query cost
(1 ray/pixel, render 1707x960 = 1440p DLSS Quality): coherent sun shadow 0.63 ms, AO 1.5 m 0.58 ms,
incoherent closest-hit 0.96 ms, alpha-test candidate loop 0.75 ms; half resolution 0.23-0.35 ms;
native 1440p 1.1-1.6 ms. Real level content (instances, overlap, alpha) is budgeted at 2x these.

### 2.1 DX12 layer (`src/Layers/xrRenderDX12`)
- `d12_api.h:68-87` native interop: `NativeDevice`, `NativeResource`, `BeginNative` (returns the
  immediate context's open `ID3D12GraphicsCommandList`, barriers flushed), `Require` (layer-tracked
  transitions), `EndNative` (all bindings re-applied, descriptor heaps re-set), `Flush`. Used by
  `da_upscaler_fsr_dx12.cpp:139-201` and `da_upscaler_dlss.cpp:190-247` - the pattern RT follows.
- `d12_swapchain.cpp:793` `NativeResource` returns `Resource::ActualNative()`, which is **null for
  static buffers**: they live suballocated in 32 MB DEFAULT pages (`d12_layer.h:80-81`,
  `BufferPageAllocator` `:366-397`, untracked, implicit promotion from COMMON); only buffers above
  4 MB are dedicated. `Resource::BufferAddress()` (`d12_layer.h:710`) has the GPU VA.
- `Require` ignores untracked resources (`d12_swapchain.cpp:825`); textures with initial data rest
  in `kStateShaderRead` = PIXEL|NON_PIXEL_SHADER_RESOURCE (`d12_objects.cpp:403-428`,
  `d12_layer.h:128`), and read states merge (`d12_context_ops.cpp:519-534`).
- Device: `ID3D12Device` + `ID3D12Device1` only (`d12_device.cpp:1483-1502`); lists are created as
  `ID3D12GraphicsCommandList` (`d12_context_ops.cpp:1366`) - DXR needs `ID3D12Device5` and
  `ID3D12GraphicsCommandList4` by `QueryInterface`. d3d12.dll is loaded dynamically, no Agility SDK.
- One shader-visible CBV/SRV/UAV ring of 1 M descriptors (`d12_layer.h:73`); a native section may
  bind foreign heaps (the harness scenario "native interop: ... with foreign heaps").

### 2.2 Frame (`xrRender_R2/r2_R_render.cpp`, `CRender::Render` `:217`)
Z-prefill → `r_main.sync` `:321` → G-buffer (parallel grass contexts on DX12, `parallel_details`
`:33`) → occlusion → wallmarks → `da_water_field_update`/ripple/visor `:608-612` → cloud map and
march `:618-622` → **DEFER_SUN** `:626-643` (`r_sun.sync` executes the cascade lists recorded on
deferred contexts, `render_hud_shadow`, `accum_direct_blend`) → emissive → lights `:676-694` →
`phase_combine` `:709` → `da_framegen_frame` → `r__gpu_log` line `:730-740`.
`OnCameraUpdated` (`r2.cpp:1089`) runs `da_temporal_frame_begin` before `Calculate`
(`r2_R_calculate.cpp:73`, `r_sun.run` `:199`).

### 2.3 Sun cascades (`xrRender_R2/render_phase_sun.cpp`)
3 cascades (near 20 m, middle/far from the preset). `calculate()` `:79-386` builds each cascade's
caster graph on its own context (`process_cascade` `:352-376`, `dsgraph.o.phase = PHASE_SMAP`);
`render()` `:388-465` draws `render_graph(0)` plus grass (`Details->Render`, never into the far map)
and the translucent pass; `accumulate_cascade` `:489-534` runs `accum_direct_cascade` per cascade.
Middle/far maps may be cached (`r__sun_cache_ms`, `:57-77`); slice 0 is reused by local lights. The
far cascade is also what the volumetric sun (`need_to_render_sunshafts`) marches.
Caster insertion: `R_dsgraph_structure::insert_static` (`xrRender/r__dsgraph_build.cpp:218`, shader
select `:253-258`), fed by `add_static` `:632` / `add_leafs_static` `:404`; trees are
`MT_TREE_ST/PM` → `insert_static` (`:508-514`, `:759-765`); dynamic objects come through
`add_leafs_Dynamic`. The Z-prefill also uses `PHASE_SMAP` (`r2_R_render.cpp:292`), so a filter must
not key on the phase alone.
Accumulation shaders: `accum_sun_near.ps` (`s = sunmask * shadow(PS)`, then SSS contact shadows),
`accum_sun_far.ps` (distance fade `da_sun_far_fade`); bound by `blender_light_direct.cpp:29-196`.

### 2.4 Level geometry (`xrRender_R2/r2_loader.cpp`)
`level_Load` `:27`: `LoadBuffers` `:333` fills `nVB` (DX9 declarators) and `nIB` (**16-bit**
indices, `:401`), `level.geomX` into `xVB/xIB` (position-only `x_decl_vert`); `LoadVisuals` `:410`;
`LoadSectors` `:439` (sector roots are `FHierrarhyVisual`); XMS overlays attach to / hide from
sector roots (`AttachOverlayVisual` `:640`, `HideOverlayGeometry` `:855`), so the **sector roots,
not `Visuals`, are the truth**. Vertex formats (`Common/OGF_GContainer_Vertices.hpp:46-110`): every
level format starts with `FLOAT3` position at offset 0; `r1_decl_lmap`/`r1_decl_vert`/
`mu_model_decl` are 32 bytes (normal as D3DCOLOR at 12, quantized SHORT2/SHORT4 texcoords at
24/28), `x_decl_vert` is 12 bytes.
`Fvisual::Load` (`FVisual.cpp:21-95`): `vBase/vCount/iBase/iCount` into `getVB(ID)/getIB(ID)`,
optional fast mesh. `FTreeVisual::Load` (`FTreeVisual.cpp:124-219`): every multiple-use model
(trees, bushes, MU props) is a tree visual with its own `xform` (`:158`) on shared geometry;
foliage = the pass PS is `*_aref` (`:175-181`); `rigid()` (`:539`) = a root without foliage (stumps,
logs, props) that the wind never moves. Stand log for `l01_escape`: 32 normal VBs, 1.99 M vertices,
0.7 M fast vertices, 20.8 M indices in 21 IBs (progressive windows included), 7848 tree-visual
positions, 93 rigid.

### 2.5 Reflections, AO, combine (`xrRenderPC_R4/r4_rendertarget_phase_combine.cpp`)
`phase_combine` `:131`: AO slot `:149-173` (`phase_gtao` → `rt_gtao` G16R16F (view z, raw AO) →
guided filter `s_gtao->E[1]` → `rt_ssao_temp`, `r4_rendertarget_phase_gtao.cpp:11-54`; created only
when `r2_ssao_mode gtao` at renderer start, `r2_rendertarget.cpp:805-825`); `combine_1` `:236-361`
samples `s_occ` only under `USE_GTAO` (`combine_1.ps:148`, define in `r4_shaders.cpp:381-383`,
texture always bound, `blender_combine.cpp:232`); **scene grab** into `rt_SSR` `:376-400` (LDR
tonemapped frame, gated on water/SSR/puddles/upscaling); `phase_da_puddle_refl` `:405`
(`r4_rendertarget_phase_da_puddle_refl.cpp:22-56`, fullscreen, `da_puddle_refl.ps`: puddle mask
re-derived from the G-buffer, ripple normal, `da_puddle_march` = `ssr_march_core`, sky fallback);
forward pass `:408-428` draws water (`water.ps:269-333`: `calc_reflections` only under
`USE_REFLECTIONS` (preset-owned `r3_water_refl`) and only to `WATER_SSR_END` = 45 m,
`settings_da_water.h:54-55`; env cube beyond). Water surfaces can be re-drawn with another shader:
`phase_water_velocity_collect` (`r4_rendertarget_phase_sky_water_velocity.cpp:81`) copies the water
items out of `mapNormalPasses[1]` before the forward pass consumes them, `phase_water_velocity`
(`:104`) redraws them.
G-buffer decode: `common_functions.h:226-310` (pure pack/unpack functions), `gbuffer_load_data`
`:340-405`; `rt_Position` is RGBA16F (`r2_rendertarget.cpp:320`) - view z in fp16 is 0.25 m coarse at
500 m, so RT ray origins use the hardware depth (`rt_MSAADepth`, typeless with SRV,
`dx11SH_RT.cpp:51-69`).

### 2.6 Temporal, settings, menu
`da_temporal.cpp:120`: motion vectors are drawn when TAA/upscaler/frame generation run -
denoisers need them too. `r__caps` (`xrRender_console.cpp:833-855`, fields in `da_upscaler.h:74-86`
`UpscalerCaps`); console registration for both modules `:1959-1968`; presets
(`xrRender_apply_preset` `:1344`, `xrRender_sync_preset_derived` `:1086`) only execute
`rspec_*.ltx` and set their own ladders. Menu: `dead_air_x64_video_options.ltx` ([rows], [groups],
[caps]), `ui_mm_opt_video.script` (`InitStates`/`Refresh`: frame generation is the "shown but
greyed" pattern), nodes `ui_mm_opt.xml:267-439` / `ui_mm_opt_16.xml`, strings
`text/{rus,eng}/dead_air_x64_video.xml` (cp1251). GPU timers `xrRenderDX11/dx11GpuTimers.h` slots.

## 3. Design

### 3.1 Principles
- **Inline ray queries (DXR 1.1) in compute shaders**, no state objects/SBT: every effect is a
  full-screen compute pass in a native section on the immediate context, like the upscalers.
- **Shader model 6.5**, bindless through unbounded descriptor tables over infra's persistent
  bindless range (`Texture2D t[] : register(t0, space100)` + `NonUniformResourceIndex`, PLAN C8) -
  same indexing power as SM 6.6
  `ResourceDescriptorHeap`, but it runs on the in-box runtime of Windows 10 2004+, which SM 6.6 does
  not without the Agility SDK (and the Agility SDK needs `D3D12SDKVersion` exports in the exe).
- **Split by motion, not by effect**: the acceleration structure holds what never moves (level
  mesh, MU props, stumps), plus - with masks - static-pose trees for reflections only. Shadow rays
  see only the static world; everything that moves keeps its shadow maps. A visual either casts
  through RT or through the near/middle cascades, never both, decided by one flag.
- **Settings latched once per frame** in `da_rt_frame_begin()` (before `da_temporal_frame_begin`,
  which asks `da_rt_wants_motion`); cascade culling, dispatches, shader constants and the motion
  vectors read the latch, so a toggle or a failed build never leaves a frame with shadows from
  both or neither.
- Own denoisers (temporal accumulation + edge-aware a-trous), no third-party code: NVIDIA NRD is
  under the NVIDIA RTX SDK licence (proprietary terms, attribution duties, NRI/ShaderMake
  integration) - out of proportion for two scalar signals; reflections on water and puddles are
  mirror-like and deterministic (one ray along the per-pixel reflection vector) and need no
  denoiser at all.
- No opacity micromaps: they need DXR 1.2 through the Agility SDK; foliage never enters shadow/AO
  rays, so alpha testing is paid only by reflection rays through crowns and by level-mesh fences.

### 3.2 Modules (new, `src/Layers/xrRenderPC_R4`, compiled by both modules; bodies under `XR_D3D12`)
- `da_rt.h/.cpp` - public surface: settings, `da_rt_caps()`, frame latch (`da_rt_frame_begin`,
  `da_rt_live(RtEffect)`), lifecycle hooks, `da_rt` constant values. DX11: stubs returning "off".
- `da_dxc.h/.cpp` - owned by `dxc.md` (PLAN C2): RT compiles through `da_dxc_compile_native`
  (include handler through `FS`, so the overlay and mods work; cache
  `appdata/shaders_cache_grizzly/r5_dxil/<tag>/native/rt/...`).
- `da_rt_device.cpp` - DXR availability, RT root signature and PSOs through infra's caches, bindless
  slots, per-pass descriptor/upload spans and lifetimes through infra's helpers (PLAN C8).
- `da_rt_scene.cpp` - level walk, BLAS/TLAS build and compaction, geometry/material/instance tables.
- `da_rt_passes.cpp` - sun shadow, AO and reflection dispatches and their filters; the request passes
  are CRenderTarget phases in `r4_rendertarget_phase_rt.cpp`.

### 3.3 Acceleration structures
**Static world BLAS** (built once per level). Walk every sector root recursively
(`FHierrarhyVisual` children, `FLOD` children, skip `MT_3DFLUIDVOLUME`, LOD billboards and
particles; a visited set by pointer, since a visual can hang under a LOD node and a root). A leaf `Fvisual`/`FProgressive` qualifies when its shader element 0 has
`iPriority/2 == 0`, is not `bStrictB2F` (no glass, water, decals) and not distortion-only. Geometry
desc per visual: `VertexBuffer.StartAddress = VA(nVB[id]) + vBase*32`, stride 32 (12 for the fast
mesh), `R32G32B32_FLOAT`; `IndexBuffer = VA(nIB[id]) + iBase*2`, `R16_UINT`, `iCount`; progressive
visuals use slide window 0 (full detail). Group into BLASes by 256 m world grid cell x {opaque,
alpha-tested}: opaque geometry gets `GEOMETRY_FLAG_OPAQUE`, alpha-tested (pass PS `*_aref`) is
non-opaque and resolved in the candidate loop. Flags `PREFER_FAST_TRACE | ALLOW_COMPACTION`;
instance transform identity. Each visual gets `rt_static = true` (new `bool` in `dxRender_Visual`,
`FBasicVisual.h:65-67`).
**MU/tree BLAS**: one per unique `(VB id, vBase, vCount, IB id, iBase, iCount)` of `FTreeVisual`
(PM: window 0), instanced in the TLAS with `FTreeVisual::xform` (3x4). Classification per root:
`rigid()` → mask `STATIC` and `rt_static = true`; foliage tree: crown visuals (`_aref`) → mask
`FOLIAGE`, trunk visuals → mask `TRUNK` (instance flag `FORCE_NON_OPAQUE`), `rt_static` stays false
(cascades keep them).
**Instance masks**: `0x01 STATIC` (world opaque), `0x02 STATIC_ALPHA`, `0x04 FOLIAGE`,
`0x08 TRUNK`, `0x10 DYNAMIC` (phase 4), `0x20 SKINNED` (phase 5).
Ray masks: shadow `0x03`; AO `0x03 | 0x08` with trunk candidates accepted only below 1.2 m above
the tree root (instance table holds the root height; the wind bend `H = max(pos.y - base, 0)` is
negligible there, so the static pose equals the drawn one) and only on their front faces (an upgraded
tree's wood lies inside its stock stand-in, its own rays leave through the back faces); reflections `0x3F`.
**TLAS**: static instances + MU instances, built once (8-10 k instances on Cordon ≈ 3 MB); rebuilt
per frame only from phase 4 on (dynamic instances, `PREFER_FAST_BUILD`, ~0.1 ms).
**Build scheduling**: `level_Load` end → `da_rt_level_load()` walks the visuals (CPU only) when an
effect is on, else the first frame that wants one does.
The first `CRender::Render` of the level (render thread, owner of the immediate context) records
the builds in a native section, batching geometry so scratch stays ≤ 64 MB, with post-build
compacted-size queries → `Flush(wait)` → compaction copies into exact-size buffers → release of
uncompacted storage after the fence. Material table is built in the same step (textures are
complete: `CLevel::net_start_client5` runs `ResourcesDeferredUpload`). Log:
`* [rt] scene: <n> BLAS, <tris> tris, <MB> MB compacted (<MB> before), TLAS <k> instances,
<ms> ms GPU, <ms> ms total`.
**Memory budget (8 GB)**: estimate before building from prebuild info; if
`QueryVideoMemoryInfo` budget - usage - estimate < 512 MB, drop FOLIAGE+TRUNK instances first
(reflections lose trees), then refuse the scene (effects off for the level, one log line).
Expected on Cordon: static 1.5-3 M tris ≈ 50-130 MB compacted (26 B/tri measured on a
heightfield, 30-45 B/tri assumed for real meshes), MU ≈ 1 M tris ≈ 35 MB, TLAS ≈ 3-9 MB,
transient build peak ≈ 250 MB, RT targets ≤ 150 MB at native 1440p (≈ 70 MB at DLSS Quality).

### 3.4 Geometry and material tables (hit shading, alpha test)
`StructuredBuffer<RtGeometry>` indexed by `InstanceID() + GeometryIndex()` (`InstanceID` = base of
the BLAS's geometry records, shared by all instances of one MU BLAS):
`{ vbSrv, vbOffset, ibSrv, ibOffset, stride|layout, albedoSrv, alphaRef, flags }`; layout = lmap /
vert / mu / fast. `vbSrv/ibSrv` index raw `ByteAddressBuffer` SRVs over the layer's page or
dedicated buffer (one SRV per page, from the new `BufferLocation` API). Texcoord decode follows the
vertex shaders (`unpack_tc_base`: SHORT2 x 32/32768 plus the tangent/binormal alpha bits for lmap,
SHORT4 x 16/32768 for mu), ported to `rt_geometry.hlsli`.
`StructuredBuffer<RtInstance>` by `InstanceIndex()`: tree root height, baked `c_scale/c_bias`
hemi/sun of MU visuals, flags.
Albedo texture = the texture bound to `s_base` in pass 0 of element 0: stage from the pass's
constant table (`s_base` → register), texture from `SPass::T`; fallback: the first texture. Its
layer SRV is copied (`ViewDescriptor` API) into the RT heap; the table holds a reference to the
`CTexture` for the level. Material textures get one `RequireMany(..., NON_PIXEL_SHADER_RESOURCE)`
batch at build and again whenever the layer's texture-write generation changed.

### 3.5 RT sun shadows (hybrid)
- **Cascade side**: `R_dsgraph_structure::o` gets `bool rt_skip_static`; `render_sun::calculate`
  sets it for cascades 0 and 1 when `da_rt_live(Shadows)`; `insert_static` returns early for
  `pVisual->rt_static` when set (after the marker, before shader selection). Contexts reset it on
  `alloc_context`. Near and middle maps then hold only trees with foliage, grass
  (`Details->Render`, unchanged), dynamic and skinned objects, the actor shadow and translucent
  casters. The **far cascade stays complete**: the volumetric sun marches it, it is cached, and
  it covers the fade band.
- **Trace** (`rt_sun_shadow.hlsl`, render size, after the G-buffer, before `r_sun.sync` in
  DEFER_SUN): origin from hardware depth + inverse (jittered) projection + view inverse, offset
  `N_geo*(0.01 + 0.0015 z) + L*(0.0005 z)`, TMin 0.02, TMax 1000; direction = sun direction
  jittered inside the sun cone (angular radius 0.5 deg, R2 sequence x interleaved gradient noise per
  frame); `RayQuery<ACCEPT_FIRST_HIT_AND_END_SEARCH>`, mask 0x03, alpha-test loop for non-opaque
  candidates. Sky pixels (depth 1) skip. Output `rt_rt_sun_raw` R16G16F (visibility, hit distance).
- **Denoise**: temporal (`rt_temporal.hlsl`, shared with AO): reproject with `rt_Velocity`,
  bilateral history fetch with previous depth, 3x3 min/max clip, alpha = max(1/(n+1), 0.1), history
  length in the second channel, reset on `da_history_reset()`; spatial: 2 a-trous 5x5 passes
  (steps 1, 2), weights on depth plane distance and normal, radius from the penumbra estimate
  (hit distance x tan(cone) in pixels), early out where the 5x5 is uniform. Output `rt_rt_sun`
  R8_UNORM. Quality Low: no jitter, no denoise (raw hard shadows, AA/DLSS resolve the edge).
- **Combine**: `accum_sun_near.ps` (the near AND middle elements, `blender_light_direct.cpp:27-29`,
  `:92-95`, `:192-200`; the `_nomsaa_*minmax` names are its variants): `s = sunmask * shadow(PS)` (vegetation
  and dynamic casters) `* rt` where `rt = da_rt.y > 0.5 ? s_rt_sun.Load(pixel) : 1`, before the SSS
  contact shadows (kept). `accum_sun_far.ps`: the far map still has the static casters, so after the
  existing distance fade `s = min(s, rt)` (no double penumbra) - and past the fade RT keeps static
  shadows at every distance. `s_rt_sun` = `$user$rt_sun`, bound in every sun element of
  `blender_light_direct.cpp`; on DX11 the name has no surface and the branch is off.
- Local lights, HUD self-shadow, actor shadow, rain map: unchanged.

### 3.6 RTAO
- AO slot of `phase_combine`: when `da_rt_live(AO)`, `phase_rtao()` replaces GTAO/HDAO/SSAO.
- Trace (`rt_ao.hlsl`): half render size on Low/Medium (per 2x2 the closest-depth pixel), full on
  High; 1 cosine-weighted ray around the G-buffer normal, TMax 1.0 / 1.5 / 2.0 m,
  `ACCEPT_FIRST_HIT_AND_END_SEARCH`, mask `0x03|0x08` (trunk rule above), occlusion
  `1 - saturate(1 - t/TMax)^2`. Temporal pass (shared) up to 32 frames, depth-aware upsample into
  `rt_gtao` (x = view z, y = AO) → the existing guided filter `s_gtao->E[1]` → `rt_ssao_temp`.
  `rt_gtao`, `rt_ssao_temp` and `s_gtao` are created also when RTAO is on.
- `combine_1.ps`: outside `USE_GTAO`, `occ = da_rt.z > 0.5 ? s_occ.Sample(...).x : <inline AO>`
  (a uniform branch; `s_occ` is always bound) - no permutation, so RTAO does not need a game restart
  and presets keep their `r2_ssao_mode`.
- Grass is an AO receiver, not an occluder (not in the TLAS); crowns neither (they sway).

### 3.7 RT reflections (water and puddles)
Order inside `phase_combine`: combine_1 → scene grab (condition extended by
`da_rt_live(Reflections)`) → **`phase_rt_reflections()`** → `phase_da_puddle_refl` → forward.
1. **Requests** into `rt_rt_refl_req` RGBA16F (cleared to 0): xy = octahedral world reflection
   direction, z = view depth of the reflecting surface, w = kind (1 puddle, 2 water) .
   - Puddles: `da_puddle_refl.s` element 1, `da_puddle_refl_req.ps` = the existing mask, ripple
     normal and `vreflect` of `da_puddle_refl.ps` without the march; only where `gbd.ground` and
     mask > 0.004; skipped when dry (same gates as `phase_da_puddle_refl`).
   - Water: `phase_water_velocity_collect` keeps its name and becomes idempotent per frame (PLAN
     C19; runs when the velocity or the reflections need it), called early from
     `phase_rt_reflections` before the requests; the items are drawn with
     `da_water_rtreq.s` (vs `water`, ps `water_rtreq` = `#define DA_WATER_RT_REQUEST` +
     `#include "water.ps"`, returning right after `vreflect` is known, so VS→PS linkage is
     identical). Depth-tested less-equal, no depth write; underwater (`under`) writes nothing.
2. **Trace** (`rt_reflections.hlsl`, render size, pixels without a request exit at once): origin =
   pixel ray at request depth, offset along the reflection; TMax = min(fog end, 400 m Medium /
   1000 m High, 250 m Low); full mask 0x3F; candidate loop alpha-tests `FOLIAGE`/`STATIC_ALPHA`
   against the albedo alpha (mip from a ray cone).
   Hit resolve, in order:
   - **On-screen reuse**: project the hit with the unjittered VP; inside the screen (8-pixel edge
     margin) and |depth(pixel) - depth(hit)| < max(0.05, 0.01 z) → colour from `rt_SSR` (the lit,
     fogged, tonemapped frame - identical to what SSR returns, with every local light).
   - **Off-screen shading**: albedo (bindless `s_base`), vertex normal, sun = `L_sun_color * N.L *`
     static shadow ray (mask 0x03) `*` vegetation lookup in cascade slices 1-2 (High; slice 0 is
     overwritten by local lights), ambient = hemi (lmap/vertex/`c_scale/c_bias` as the G-buffer VS
     gets it) x `L_hemi_color` + `L_ambient`, emissive materials added; then the combine_1 fog
     (camera-to-hit distance, same as the grabbed frame carries) and the combine_1 tonemap with
     the current `s_tonemap` exposure - so both paths are in the same LDR space.
   - Miss: alpha 0.
   Output `rt_rt_refl` RGBA16F (rgb, confidence: 1 hit, edge-faded for reuse, 0 miss).
3. **Consumers** (runtime branch on `da_rt.x`, no permutation, not under `USE_REFLECTIONS`, so RT
   reflections work on every preset):
   - `water.ps`: when `da_rt.x` and the request depth at `pos2d` matches this fragment's depth
     (two water bodies behind each other: the far one keeps SSR), `reflection =
     lerp(env, rt.rgb, rt.a)` with the RT reach instead of `WATER_SSR_END`; the SSR branch
     (`calc_reflections`) is not executed then. Rough water: env mip as today; the RT colour gets
     the same `mss_total`-driven 1-texel blur (3 taps along the slope).
   - `da_puddle_refl.ps`: `refl = rt.a > 0 ? float4(rt.rgb, 1) : da_puddle_sky(...)` instead of
     `da_puddle_march` when `da_rt.x`.
No denoiser; TAA/DLSS resolve aliasing; water is already high in the reactive mask.

### 3.8 Dynamic and skinned objects (phases 4-5)
- Rigid dynamic renderables (`MT_NORMAL`/`MT_HIERRARHY`/`MT_SKELETON_RIGID` children) within 60 m:
  BLAS per model mesh (built on first use, cached in the model pool entry), instanced per frame
  with the renderable's world (bone world for rigid bone parts), mask `DYNAMIC` - reflections and
  AO only; their shadows stay in the cascades (so no per-object handover between the two shadow
  systems).
- Skinned (`RM_SKINNING_*B`): the nearest 12 within 30 m: compute pre-skin (`rt_skin.hlsl`,
  palette = `LL_GetTransform_R` as `SkeletonX.cpp:93-102` uploads it, the vertex decode of
  `FSkinnedTypes.h`) into a per-instance position buffer, BLAS `PREFER_FAST_BUILD|ALLOW_UPDATE`,
  refit every frame, rebuild every 30 frames; mask `SKINNED`, reflections and AO only.

## 4. Integration points (edits in existing files)

| File | Change |
|---|---|
| `xrRenderDX12/d12_api.h`, `d12_swapchain.cpp` (built by `infra`, PLAN C9) | `NativeBufferRange` (was `BufferLocation`), `NativeDescriptor` (was `ViewDescriptor`, layer CPU descriptor, copy source), `RequireMany(ctx, ID3D11Resource* const*, UINT, D3D12_RESOURCE_STATES)`, `UINT64 TextureWriteGeneration(ID3D11Device*)` |
| `xrRenderDX12/d12_context_ops.cpp`, `d12_context_state.cpp` (built by `infra`) | generation counter bumped by UpdateSubresource/Copy*/Map into non-RT/DS/UAV textures and by texture creation/destruction |
| `xrRenderDX12/d12_device.cpp` (built by `infra`) | `ProbeAdapter` also reports `RaytracingTier` (for `rt_tier` on DX11); log line `raytracing tier` |
| `xrRender/FBasicVisual.h:65-67` | `bool rt_static{}` |
| `xrRender/r__dsgraph_structure.h`, `r__dsgraph_build.cpp:218` | `o.rt_skip_static`, early return in `insert_static`; reset in `alloc_context` |
| `xrRender_R2/render_phase_sun.cpp:365-373` | `dsgraph.o.rt_skip_static = cascade_ind < 2 && da_rt_live(Shadows)` |
| `xrRender_R2/r2_R_render.cpp` | after `r_main.sync` `:321`: `da_rt_update_scene()`; in DEFER_SUN before `r_sun.sync` `:633`: `Target->phase_rt_sun()`; timers |
| `xrRender_R2/r2.cpp` | `da_rt_device_create/destroy` in `create`/`destroy`; `da_rt_reset_begin/end` in `reset_begin`/`reset_end` (targets only; scene kept, freed if all effects went off); `da_rt_frame_begin()` before `da_temporal_frame_begin()` in `OnCameraUpdated` |
| `xrRender_R2/r2_loader.cpp` | `da_rt_level_load()` at the end of `level_Load` (walks only with an effect on); `da_rt_level_unload()` first in `level_Unload` (GPU wait, free) |
| `xrRender_R2/da_temporal.cpp:120` | `motion |= !menu && da_rt_wants_motion()` |
| `xrRender_R2/r2_rendertarget.cpp` | RT targets (R5, when any effect is on, render size, `CRT::CreateUAV`): `rt_rt_sun_raw` RG16F, `rt_rt_sun_hist[2]` RG16F, `rt_rt_sun` R8, `rt_rt_ao_half` R16F, `rt_rt_ao_hist[2]` RG16F, `rt_rt_refl_req` RGBA16F, `rt_rt_refl` RGBA16F; `rt_gtao`/`rt_ssao_temp`/`s_gtao` also when RTAO |
| `xrRender_R2/r2_types.h` | names `$user$rt_sun`, `$user$rt_refl`, `$user$rt_refl_req`, ... |
| `xrRenderPC_R4/r4_rendertarget.h`, `r4_rendertarget_phase_combine.cpp` | AO slot `:149` → `phase_rtao()`; grab condition `:380`; `phase_rt_reflections()` before `:405`; water collect moved |
| `xrRenderPC_R4/r4_rendertarget_phase_sky_water_velocity.cpp:81-141` | collector shared with RT (`phase_water_velocity_collect`, idempotent per frame, PLAN C19) |
| `xrRenderPC_R4/sources.cmake`, `xrRenderPC_R5` CMake | new files; R5 adds the DXC include (Windows SDK `dxcapi.h`, no import lib) |
| `xrRender/blenders/blender_light_direct.cpp` | `C.r_dx11Texture("s_rt_sun", r2_RT_rt_sun)` in every sun element |
| `xrRender/Blender_Recorder_StandartBinding.cpp` | binder `da_rt` = (reflections, shadows, AO, quality) live this frame; zeros on DX11 |
| `xrRender/xrRender_console.cpp` (`core`) | commands (4.1), `r__caps` keys `rt`, `rt_tier` (PLAN C7); `UpscalerCaps` + `rt`, `rt_tier` |
| `xrRenderDX11/dx11GpuTimers.h/.cpp` (`core`) | slots `RtScene`, `RtSun`, `RtAO`, `RtRefl`; `[gpu]` line gets `rt_scene=`, `rt_sun=`, `rt_ao=`, `rt_refl=` (PLAN C13) |
| shaders `r3/common_functions.h` | pure pack/unpack (`:226-310`) moved to `gbuffer_pack.h`, included back |
| shaders `accum_sun_near.ps`, `accum_sun_far.ps` (`accum_sun.ps` is the luminance pass, untouched), `combine_1.ps`, `water.ps`, `da_puddle_refl.ps`, `da_puddle_refl.s` | as in 3.5-3.7 |
| `configs/dead_air_x64_video_options.ltx`, `ui/ui_mm_opt.xml`, `ui/ui_mm_opt_16.xml`, `scripts/ui_mm_opt_video.script`, `text/{rus,eng}/dead_air_x64_video.xml` | section 6 |
| `installer/runtime-files.txt`, `docs/dead-air/DEPENDENCIES.md`, `docs/dead-air/DX12_RENDERER.md` | DXC DLLs, notices, an RT section |

### 4.1 Console contract (registered by both modules, saved, never written by `_preset`)
| Command | Type | Values | Notes |
|---|---|---|---|
| `r__rt_reflections` | integer 0/1 | default 0 | `depend="vid"` |
| `r__rt_shadows` | integer 0/1 | default 0 | `depend="vid"` |
| `r__rtao` | integer 0/1 | default 0 | `depend="vid"` |
| `r__rt_quality` | token | `st_rt_low` 0, `st_rt_medium` 1, `st_rt_high` 2; default medium | `depend="vid"` |
| `r__rt_debug` | runtime integer 0..4 | 0 off, 1 sun mask, 2 AO, 3 reflections, 4 primary-ray albedo vs G-buffer | QA diagnostic, not saved |
| `r__caps` | + `rt=` (RT usable this session), `rt_tier=` (device/adapter DXR tier, PLAN C7) | | |
`-dx12_no_rt` on the command line masks the capability and skips RT initialisation (troubleshooting;
`-no_rt` is dropped, PLAN C7).
Effects switch on/off through `vid_restart`: targets are recreated; the scene is built on the next
frame when missing and freed when every effect is off. DX11 keeps saved values for DX12; on DX12
with `rt=0` they are switched off at start with one log line (frame generation's rule).

## 5. Data and resources

| Resource | Format / size | Lifetime |
|---|---|---|
| static + MU BLASes | compacted, ~90-170 MB on Cordon | level |
| TLAS + instance descs | ~3-9 MB | level (per frame from phase 4) |
| build scratch / uncompacted | ≤ 64 MB scratch per batch, peak ≈ 250 MB | first frame of the level |
| geometry / instance / material tables | StructuredBuffers, < 4 MB | level |
| descriptors (PLAN C8) | infra's persistent bindless range (textures, raw buffers) + `AllocateDescriptors` spans for pass tables; no own heap | level / frame |
| constants / instance descs (PLAN C8) | infra's `AllocateUpload` (frame-scoped upload memory) | frame |
| `rt_rt_sun_raw`, `rt_rt_sun_hist[2]`, `rt_rt_sun` | RG16F, RG16F x2, R8; render size | renderer |
| `rt_rt_ao_half`, `rt_rt_ao_hist[2]`, `rt_gtao`, `rt_ssao_temp` | R16F half/full, RG16F x2, existing | renderer |
| `rt_rt_refl_req`, `rt_rt_refl` | RGBA16F x2, render size | renderer |
Root signature (one for all RT passes): b0 root CBV (per-pass constants: matrices, sun, fog,
tonemap, frame, quality), table t0 space2 (TLAS view; see "R0 as built"), table t0-t15 space0 (pass
inputs: depth, G-buffer, velocity, histories, `rt_SSR`, cascade array), table u0-u7 (pass outputs), unbounded table space100
(`Texture2D`) and unbounded table space101 (`ByteAddressBuffer`), both at infra's bindless base
(PLAN C8), t0-t2 space3 (geometry, instance,
material tables); static samplers s0 linear wrap (anisotropic 4), s1 point clamp, s2 linear clamp,
s3 comparison (cascade lookup). Engine textures enter a pass via `Require` (inputs
NON_PIXEL_SHADER_RESOURCE, outputs UNORDERED_ACCESS) and `ViewDescriptor` copies into the ring.

## 6. Shaders

DXC (cs_6_5, `-HV 2021`), in `gamedata/shaders/r3/rt/`: `rt_common.hlsli` (constants, bindings,
reconstruction, octahedral, noise), `gbuffer_pack.h` (shared with FXC), `rt_geometry.hlsli`
(vertex/texcoord decode, alpha test), `rt_material.hlsli` (albedo, hemi, off-screen lighting, fog,
tonemap copies of combine_1), `rt_sun_shadow.hlsl`, `rt_temporal.hlsl`, `rt_atrous.hlsl`,
`rt_ao.hlsl`, `rt_ao_upsample.hlsl`, `rt_reflections.hlsl`, `rt_skin.hlsl` (phase 5),
`rt_debug.hlsl`. FXC (existing tree): `water_rtreq.ps`, `da_water_rtreq.s`,
`da_puddle_refl_req.ps`, edits listed in section 4. `Check-ShaderLinkage.py` covers the new FXC
pass pairs; `Test-Shaders.ps1` skips `r3/rt/`. New C++ files carry the Grizzly MIT header;
shader files follow the shader tree's convention.

## 7. UI (not in presets)

`dead_air_x64_video_options.ltx`: rows `rt_reflections = check, r__rt_reflections`,
`rt_shadows = check, r__rt_shadows`, `rtao = check, r__rtao`, `rt_quality = list, r__rt_quality`;
`[groups] order = screen, quality, aa, rtx, image, effects, color`; `[group_rtx] rows =
rt_reflections, rt_shadows, rtao, rt_quality`; captions `ui_mm_da_rt_*`. No `[caps]` line: the rows
are always shown. Nodes `check_rt_reflections`, `check_rt_shadows`, `check_rtao`
(`x="355" ... ui_inGame2_checkbox`), `list_rt_quality` (`list_length="3"`), all
`group="mm_opt_video" depend="vid"`, in both xml files.
`ui_mm_opt_video.script`: `InitStates` sets `off_hint` for the four rows: `caps.api ~= 12` →
`..._rt_dx11` (or `..._rt_dx11_nohw` when `caps.rt_tier < 11`, PLAN C7); `caps.rt ~= 1` → `..._rt_unsupported`;
`Refresh`: greyed also while the renderer combo shows DirectX 11 (frame generation's rule), and
`rt_quality` greyed with `..._rt_quality_off` while none of the three is ticked.
Strings (eng / rus, rus written as cp1251):
- `ui_mm_grp_rtx`: "RTX" / "RTX"
- `ui_mm_da_rt_reflections`: "Ray-traced reflections" / "Отражения (трассировка лучей)"
- `ui_mm_da_rt_shadows`: "Ray-traced sun shadows" / "Тени от солнца (трассировка лучей)"
- `ui_mm_da_rtao`: "Ray-traced ambient occlusion" / "Затенение окружения (трассировка лучей)"
- `ui_mm_da_rt_quality`: "Ray tracing quality" / "Качество трассировки" (shortened by `core` at K3:
  the longer caption ran into the combo on the 16:9 layout)
- `st_opt_st_rt_low|medium|high`: "Low|Medium|High" / "Низкое|Среднее|Высокое"
- hint `rt_reflections`: "Water and rain puddles reflect the world traced by the GPU's ray tracing
  cores: also what is off screen or hidden, out to the far shore." / "Вода и лужи отражают мир,
  просчитанный аппаратной трассировкой лучей: в том числе то, что за краем экрана или скрыто, до
  дальнего берега."
- hint `rt_shadows`: "Sun shadows of buildings, terrain and other static objects are traced: exact
  at any distance, with soft edges. Trees, grass, characters and moving objects keep their shadow
  maps, so their shadows still sway in the wind." / "Тени от зданий, рельефа и других неподвижных
  объектов считаются трассировкой лучей: точные на любом расстоянии, с мягкими краями. Деревья,
  трава, персонажи и движущиеся предметы сохраняют карты теней, поэтому их тени по-прежнему
  качаются на ветру."
- hint `rtao`: "Ambient occlusion from ray tracing instead of the screen-space one: contact shading
  also from what the screen does not show." / "Затенение окружения трассировкой лучей вместо
  экранного: контактные тени и от того, чего нет на экране."
- hint `rt_quality`: "Resolution and reach of the ray traced effects. Low: hard shadows, half-
  resolution occlusion, shorter reflections. High: full resolution and the farthest reflections." /
  "Разрешение и дальность эффектов трассировки. Низкое: жёсткие тени, затенение в половинном
  разрешении, короткие отражения. Высокое: полное разрешение и самые дальние отражения."
- `..._rt_dx11`: "Ray tracing needs DirectX 12: choose it under Renderer and restart the game." /
  "Трассировке лучей нужен DirectX 12: выберите его в строке «Рендер» и перезапустите игру."
- `..._rt_dx11_nohw`: "Ray tracing needs DirectX 12 and a GPU with hardware ray tracing." /
  "Трассировке лучей нужны DirectX 12 и видеокарта с аппаратной трассировкой лучей."
- `..._rt_unsupported`: "The GPU or its driver has no hardware ray tracing (DXR 1.1): GeForce RTX,
  Radeon RX 6000 and newer, Intel Arc." / "Видеокарта или драйвер не поддерживают аппаратную
  трассировку лучей (DXR 1.1): нужны GeForce RTX, Radeon RX 6000 и новее, Intel Arc."
- `..._rt_quality_off`: "Quality of the ray traced effects: it matters only while one of them is
  on." / "Качество эффектов трассировки: действует, только когда включён хотя бы один из них."
Presets: `rspec_*.ltx` and `xrRender_sync_preset_derived` never name `r__rt_*`; the preset
combo's hint gains nothing (RT is not part of quality).

## 8. Fallback

| Situation | Behaviour |
|---|---|
| DX11 module | commands registered and saved, all RT code compiled out, `da_rt` = 0, `rt=0`, `rt_tier` from `ProbeAdapter`'s cache; rows greyed with the DX12 hint; images bit-identical to today |
| DX12, `RaytracingTier < 1_1`, no `dxcompiler.dll`/`dxil.dll`, compile or PSO failure, `-dx12_no_rt` | `rt=0`, `* [rt] unavailable: <reason>`, saved values switched off at start, rows greyed |
| MSAA on | effects inactive (the passes read single-sampled depth/G-buffer), hint text = unsupported |
| Scene over the VRAM budget / build failure | foliage dropped first, else effects inactive for the level, one log line; cascades, SSR and the preset AO take over the same frame through the latch |
| Menu, loading, PDA/UI-only frames | no dispatch (Render returns early as today) |
| A frame without motion vectors (menu PP) | histories reset, raw signals used |
| Device removed | the layer's DRED/log path; RT resources die with the device |

## 9. Test plan

Builds/tools (all must pass):
1. `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` clean; `dumpbin /imports xrRender_R4.dll` shows no
   new imports.
2. `tools\qa\d3d12layer\Run-LayerTests.ps1 -HighPerformance`: the 36 scenarios plus new
   "native raytracing" (engine VB/IB → `BufferLocation` → BLAS/TLAS, inline query compute into a
   layer UAV texture, compared with a CPU-computed occluder mask) and "view descriptor copy" -
   PASS, debug layer and GBV silent.
3. `python tools/qa/Check-ShaderLinkage.py` and `python tools/qa/Check-OptionHints.py` pass.
4. `git grep -n "r__rt" -- "*rspec_*.ltx"` is empty.

Stand (`Invoke-Stand`, `-ExtraArgs '-r5 -always_active'` unless noted). New scenario
`tools/qa/water/rtx_check.lua` (built like `upscale_check.lua`): steps `rt_off`, `rtao`,
`shadows`, `refl`, `all`, `all_low`, `all_high`, `all_dlss_q` (need `dlss`), `back`; each step sets
its commands, `vid_restart` when needed, settles 3 s, `r__capture 1 2`, `r__gpu_log 60`, measures
FPS 8 s, logs `[qa] step <name> fps <x> caps <r__caps>`; skips RT steps where `rt ~= 1`.
| Run | Save | Pass criteria |
|---|---|---|
| `rtx-pedro` | `qa_pedro` | `[rt] scene` line; no FATAL / device removed / `! [rt]`; per-step captures without black or NaN pixels (numpy: no channel = 0 over > 1% of a non-sky region that is lit at `rt_off`) |
| `rtx-foliage` | `foliage` | sway: two captures 0.5 s apart with `r__rt_shadows 1` - crown shadow region differs (mean abs diff > 2/255, same method as the wind QA), a static wall/building shadow region stays within 1/255; the same numbers with `rt_off` |
| `rtx-trees` | `trees` | tree shadows present with RT shadows (crown shadow region darker than lit ground by ≥ 20% at `shadows` and `rt_off` alike); no shadow doubling on MU props (prop shadow luminance within 10% of `rt_off`) |
| `rtx-water` | `fogqa` (`surface` weather) | with `refl` the reflection region past 45 m differs from the env-only frame (far shore present); on-screen reuse seam: gradient across the SSR/RT boundary below 10/255 |
| `rtx-puddles` | `puddle`, `-Scenario puddles -PreCommands 'r__rt_reflections 1'` | puddle captures match SSR ones where SSR hit (PSNR > 30 dB on on-screen hits), sky fallback where missed |
| `rtx-dx11` | `qa_pedro`, `-ExtraArgs '-always_active'` | `caps` has `rt=0`; `rt_off` vs `all` captures identical (RT inert on DX11) and identical to a capture of the pre-RT build (PSNR > 45 dB) |
| `rtx-off-parity` | `qa_pedro`, R5 | `rt_off` capture of the new build vs the pre-RT build: PSNR > 45 dB (RT off changes nothing) |
| `rtx-debug` | `qa_pedro`, `-ExtraArgs '-r5 -always_active -dxdebug'` | no `D3D12 ERROR`/`CORRUPTION` lines; `r__rt_debug 4` albedo view matches the G-buffer albedo (PSNR > 25 dB on primary hits) |
| `rtx-restart` | `qa_pedro` | each row toggled 3x through `vid_restart`: no crash, VRAM (nvidia-smi sampler) back within 50 MB of the start |
| `rtx-levelchange` | `qa_quest` via `quest_teleport` | scene rebuilt on the new level, the old one freed (log sizes), no leak over two changes |
| `options` | `options_tab.lua` | the RTX header and four rows visible; on DX11 greyed with the DX12 hint; hints shown |
Perf (`ab_run.ps1` + `analyze.ps1`, 2560x1440 Extreme, DLSS Quality, interleaved, holding
`ninja.lock`): per-effect GPU timers within the budget table (10), all-on Medium ≤ +3.0 ms per frame
over `rt_off`, Low ≤ +1.5 ms; level load +≤ 1.5 s; VRAM +≤ 600 MB.

### 9.1 R0 results (28.09.2026, runs under `_work\dx12-features-20260928\qa`)

Under the test budget (`PLAN.md` 0.1a) `rtx-dx11` and `rtx-off-parity` ran as `render_check` parity
runs against the Gate-0 references (`rtx_check.lua` comes with R1, whose steps need passes).
- Build: both modules, no warning from a touched file. `xrRender_R4.dll` imports no new DLL (five new
  functions against Gate 0, none from RT code: `std::chrono` and file-attribute calls of other lanes).
- Layer tests, every scenario: 77 passed / 0 failed in `-HighPerformance`, `-Gbv` and `-Dxil`, debug layer
  silent; "native raytracing" matches the CPU model on every texel off the triangle edges, the compacted
  BLAS is smaller than the built one; "view descriptor copy" equals the D3D11 reference through ring
  tables and through bindless slots.
- `rtx-off-parity` = `rt-r0-render-pedro` (R5, today's binaries, Gate-0 overlay plus the rt shader files):
  PSNR 50.5 dB against `core-g0-render-pedro-a/b/c` (> 45 dB). Against the same configuration's DXC run
  of 13:23 (`infra-i4-render-pedro-g0ovl`) 99.81 % of static pixels within 2/255, no cluster over 16 px,
  57.1 dB: what differs from the FXC references is DXC's float rounding, which every DXC run shows.
  `* [rt] ready: DXR tier 1.2, passes at cs_6_5 through DXC, probe ok (60.4 ms)`, `r__caps` `rt=1;rt_tier=12`.
- `rtx-dx11` = `rt-r0-render-pedro-r4` (R4, `r__rt_reflections/_shadows/_rtao 1`, quality high):
  `rt=0;rt_tier=12`, `~ [rt] ray tracing needs DirectX 12: the RTX settings are kept for it`; calibrated
  "equal" to the references plus the same-period snapshot control (`vrs-v2-ctl-pedro-r4`: PASS,
  99.70 % within; `ds-s3-ctl-pedro-r4`: PASS), 53.8 dB against the three references alone.
- Availability both ways, `options_tab` 50/50: `rt-r0-options-dxdebug` (`-dxdebug`: RT device part and
  probe without a debug layer error, RTX rows enabled, quality greyed until an effect is ticked) and
  `rt-r0-options-nort` (`-dx12_no_rt` with two effects saved on: `rt=0;rt_tier=0`, rows greyed
  "unsupported", `* [rt] unavailable: switched off by -dx12_no_rt`, the saved values switched off).

### 9.2 Final test phase, functional part (28.09.2026; `_work\dx12-features-20260928\final\stand.md`)

- `final-rtx-pedro` (`qa_only = rt_off,rtao,shadows,refl,all,debug_albedo`, `-Tga`): `* [rt] passes: reflections ok,
  sun shadows ok, occlusion ok, characters ok`, `* [rt] scene: 417 BLAS, 1573602 tris, 42.7 MB compacted (104.3
  before), TLAS 7897 instances, 262 textures, 302 ms total`; every shot reviewed: no black, NaN or garbage; RTAO
  darkens contacts, RT sun shadows keep the bench and door shadows (no loss, no doubling); `rt_refl` 0 ms in the
  interior (no pixel requests a ray) - reflections are judged on water. `r__rt_debug 4`: the traced half matches the
  G-buffer half without a seam; 94 magenta pixels through slat gaps are hits on meshes without a bindless albedo
  (the view's own marker; the hit shading has its fallback).
- `final-rtx-fogqa` (`rt_off,rtao,shadows,refl`): `371 BLAS ... 280 ms`, `rt_ao` 0.56-0.59 ms, `rt_sun` 1.14-1.23 ms,
  `rt_refl` 0.75-0.78 ms; the marsh ponds reflect the far reeds and banks, open sky where the ray misses; the wet mud
  shows the ground's reflection; no seam seen.
- Sway (the hard gate): `final-rtx-foliage`'s pair collided in one screenshot file name (0.5 s apart; `rtx_check.lua`
  now waits 1.2 s); `final-rtx-trees` (GD 1, mesh grass 1, `qa_sway`): the crown shadows on the roof move between the
  two shots with RT shadows on as they do off (11.2 % / 8.3 % of the roof region > 2/255), the sky 0 %: vegetation
  stays in the cascades and keeps swaying - PASS.
- Level change with all three effects, GD, mesh grass, VRS on and `-dxdebug` (`final-robust-level-c`): the l01 scene
  (417 BLAS) released at the level unload and the k00 scene (371 BLAS, 367 ms) built on its first frame, 0 D3D12
  error lines. `options_tab` R5 50/50 with
  the RTX rows; R4 `caps ... rt=0`.
- Not covered here: `rtx-puddles` (a puddle save needs 40 s of rain first; fogqa's wet mud stood in), the budget
  table and TLAS rebuild cost (Perf phase), the RT-1 VRAM fallback (needs a VRAM-starved card).

### 9.3 Final test phase, performance (Perf phase, 28.09.2026)

At the user's request the Perf phase ran one measurement pass instead of on/off pairs (PLAN 8.4), so these are the
per-effect GPU timers of `r__gpu_log`, not A/B deltas: the price a player pays per effect on top of what it replaces.
- `final-onepass-foliage` (2560x1440 Extreme, TAA, RTX 3070 Laptop; every DX12 feature on, RT shadows + RTAO +
  reflections at High): 49 FPS, GPU frame 20.2 ms. `rt_scene` (TLAS refit) 0.38 ms, `rt_sun` 1.75 ms, `rt_ao`
  1.83 ms (the AO slot reads 1.82 ms: RTAO replaced GTAO there), `rt_refl` 0 (no water in view); `sun` 5.9 ms with
  RT shadows on (the far cascade and every swaying or dynamic caster stay in the maps, no off-pair to compare).
- Reflections: 0.75-0.78 ms on the fogqa ponds (`final-rtx-fogqa`, 9.2); nothing where no pixel requests a ray.
- Scene at load (l01_escape): 417 BLAS, 1.57 M triangles, 42.7 MB compacted, TLAS 7897 instances, built in 659 ms
  in that run (302 ms in `final-rtx-pedro`).
- Not measured: Low and Medium tier costs, VRAM peak with frame generation, an RT on/off FPS pair. The 10 table stays
  the expectation for them.
- Decision: no change. RT stays out of every preset, `rspec_*.ltx` and `xrRender_sync_preset_derived`; the RTX
  rows are the player's choice at the price above.

### 9.4 Marsh reflection fix (29.09.2026)

- `rt-refl-fix` (`fogqa`, `rt_off,refl,all,debug_refl`, `-Tga`, TAA, `r__vrs_force 1` = every forward
  pixel coarse 2x2, the worst case of the VRS depth match): the ponds keep the traced reflection
  (reeds and banks, the content of `final-rtx-fogqa`'s VRS-off shot) instead of the SSR/sky look of
  `rt_off`; `rt_refl` 0.62-0.70 ms with the screen march (0.75-0.78 ms before, VRS off); no `! [rt]`
  or DXC line. The back-side skip for the wet mud was added after this run (compiled, not run).

## 10. Performance expectation (RTX 3070 Laptop, 1440p, DLSS Quality = render 1707x960)

Probe numbers x2 for real content:
| Pass | Low | Medium | High |
|---|---|---|---|
| sun shadows trace (+ alpha loop) | 1.3 ms hard, no filter | 1.3 + 0.35 filter | same as Medium |
| near/middle cascades without static casters | -0.8 .. -1.5 ms (sun slot is 4.4-5.0 ms at 1440p Extreme) | same | same |
| RTAO (trace + temporal + guided filter) | 0.7 (half res) | 0.8 (half res, 1.5 m) | 1.5 (full) |
| GTAO removed | -0.6 .. -0.7 (1.4-1.7 ms native, scaled) | same | same |
| reflections (30% water/puddle coverage, requests included) | 0.6 | 0.8 | 1.1 |
| SSR march removed on those pixels | -0.2 .. -0.4 | same | same |
| TLAS update | 0 (static) / 0.1-0.2 with phase 4-5 | | |
Net, all three on: Low ≈ +0.5..+1.2 ms, Medium ≈ +1.0..+2.0 ms, High ≈ +1.8..+3.0 ms on a
≈ 11-12 ms DLSS-Quality frame (-5..-20% FPS); dry land without water pays nothing for
reflections. Load: +0.3-1.0 s (BLAS build 27 ms per 2 M triangles plus compaction wait). VRAM:
+250-450 MB. AMD RDNA2/3 inline ray queries run about 2x slower: Low is the recommended tier there
(no vendor code paths).

## 11. Risks

1. **Shadow handover correctness** - one flag (`rt_static`) decides RT vs cascade for every static
   visual; the far cascade keeps everything and combines with `min`. A misclassified visual shows
   as a missing or doubled shadow; `rtx-trees`/`rtx-foliage` catch it. Swaying geometry is never
   `rt_static`, so the sway rule cannot break through this path.
2. **Texcoord/format decode** in hit shading (quantized SHORT2/SHORT4, lmap secondary bits) -
   `r__rt_debug 4` compares against the G-buffer.
3. **LDR seam** between on-screen reuse and off-screen shading - same fog and tonemap code, edge
   fade, measured in `rtx-water`.
4. **fp16 G-buffer depth** - origins come from the hardware depth; bias scales with distance.
5. **Layer ordering** - the sun dispatch is recorded on the immediate context before the cascade
   lists are executed by `r_sun.sync`; the layer patches the deferred lists' first states
   (`d12_context_ops.cpp:2532-2555`), so `rt_rt_sun` is read-ready. Covered by `-dxdebug`.
6. **Texture state** - material textures read bindless without per-frame barriers rely on the
   write generation; a texture reloaded in place would otherwise be read in COPY_DEST.
7. **VRAM on 6-8 GB cards with DLSS FG + 1440p** - budget check degrades before failing.
8. **DXC redistribution** - `dxcompiler.dll` (LLVM/NCSA + MIT) and `dxil.dll` (Microsoft
   redistributable licence) are Microsoft-signed release binaries; about 20 MB more in the package.
9. **Win10 players** - SM 6.5 + DXR 1.1 run in-box from 2004; nothing needs the Agility SDK.
10. **Level load hitch** on the first frame (scene build on the render thread) - bounded by the
   batch size, logged.
11. **Temporal ghosting** of shadow/AO under fast motion - variance clip, history reset on
   disocclusion and `da_history_reset`.

## 12. Implementation order (each phase ships working on its own)

0. **Foundation** (no visible change; PLAN R0): layer APIs + generation counter come from `infra`
   (I2/I3), `da_dxc` and DXC 1.9.2607 from `dxc`/`core` (C1/C2), console commands, `r__caps rt/rt_tier`,
   timer slots and the RTX UI from `core`; this lane: `da_rt` device part over infra's helpers (root
   signature, PSOs, bindless, upload), `da_rt` binder, `gbuffer_pack.h` split, `scenarios_rt.cpp`.
   Check: layer tests, both modules build, `rtx-dx11`, `rtx-off-parity`.
1. **Scene + RTAO**: level walk, static and MU BLAS/TLAS with compaction and budget, material table
   (albedo alpha for the alpha test), `rt_static` flags (unused yet), `r__rt_debug`; RTAO trace,
   temporal, upsample into `rt_gtao`, guided filter, `combine_1` branch; UI group with `rtao` and
   `rt_quality` rows and strings. Check: `rtx-pedro` (rtao steps), `rtx-debug`, `rtx-restart`,
   `rtx-levelchange`, perf row.
2. **Sun shadows**: cascade filter (`rt_skip_static`), trace, temporal + a-trous, accum shader
   changes, `s_rt_sun` binding; `rt_shadows` row. Check: `rtx-foliage`, `rtx-trees`, `rtx-pedro`
   shadows steps, perf.
3. **Reflections**: full hit shading (UVs, hemi, lighting, fog/tonemap), request passes (puddles,
   water, shared water collector), trace with on-screen reuse, `water.ps`/`da_puddle_refl.ps`
   branches, grab condition; `rt_reflections` row. Check: `rtx-water`, `rtx-puddles`, perf.
4. **Dynamic rigid objects** in the TLAS (per-frame TLAS rebuild) for reflections and AO.
5. **Skinned characters**: compute pre-skin, refit, nearest 12 within 30 m.
6. **Tuning and docs**: budgets per tier against the perf table, `DX12_RENDERER.md` RT section,
   final `rtx_check` on `qa_pedro`, `foliage`, `fogqa`, `puddle`, DX11 parity run.
