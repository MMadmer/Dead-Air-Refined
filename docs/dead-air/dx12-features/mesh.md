# Mesh-shader grass (DirectX 12)

Status: implemented (28.09.2026). M1-M10 in the tree; the functional part of the final test phase
passed (look-neutral against the vertex path, level change under `-dxdebug`) and `r__grass_mesh`
defaulted to 1 after the Perf phase; **since 29.09 the default is -1 (auto, gpudriven.md 3.13)**: mesh
grass saves CPU time but costs 0.4-0.6 ms of GPU, so it runs while the CPU is the frame's bottleneck. After the final one-pass measurement the M8 depth-only shadow
elements are simply how mesh grass draws its shadows (look-neutral by construction, `r__grass_mesh_depth`
dropped) and the M9 G-buffer Hi-Z (`r__grass_mesh_hiz`, `da_grass_hzb.cs`) was removed (13.2); the M10
mesh columns equal the base columns (no radius or density change). Section 13
records what was built, "Final test phase, performance" at the end the numbers. Lane status:
`_work\dx12-features-20260928\lanes\mesh.md`.
DX12 only (`xrRender_R5.dll`); `xrRender_R4.dll` keeps today's
vertex-shader grass. Builds on the native infrastructure in `infra.md` (caps, DXC, stream PSOs,
DXIL shaders on the D3D11 path) and changes nothing in `dxr.md` / `vrs.md` (both keep grass in the
sun cascades 0/1 through `Details->Render`, which is where this path plugs in).

## Integration amendments (PLAN.md)

Implemented by the `mesh` lane (milestones M1-M10 of `PLAN.md` 4.4). These points supersede the text
below where they differ (ids refer to `PLAN.md` section 1):
- **C5, DXIL mode**: no `#dxil` names. Mesh grass requires `dxc=1` (every R5 engine shader is DXIL
  this session, `dxc.md`), so the detail pixel shader is DXIL already. `.ms`/`.as` go through the
  engine compile path with FXC preprocessing and `-HV 2021`; `da_detail_common.h` must compile under
  FXC, HLSL 2018 and 2021. A mesh pass is exempt from `UnifyShaderFormats`: if its PS fell back to
  FXC the element is unavailable, the vertex path runs and the verdict is stored.
  `mesh_ready` = R5, `NativeCaps::meshShaderTier >= 1`, `sm >= 65`, `dxc=1`, no MSAA, the detail VS
  include `da_detail_common.h`, the first mesh PSO accepted, no stored refusal.
- **Who builds what**: the `dxc` lane builds the `#ms/#as` name routing (`ShaderResourceTraits.h`),
  the DXIL `create_shader` for MS/AS (`r4_shaders.cpp`), the `BindPoint` constant-buffer slots
  (C6), `Test-Shaders.ps1` DXC mode and MS->PS pairs in `Check-ShaderLinkage.py`. The `infra` lane
  builds the DXC service, DXIL program-kind detection (kinds 13/14 -> `Shader::kind`), stream PSOs
  (`StreamPipeline`), `CreateNativeBuffer`, `QueryVideoMemory`, native sections v2, structured
  buffer SRVs (C10) and caches `ID3D12GraphicsCommandList6` per list. `core` builds
  `CBackend::RenderMesh`, the `Grass` timer slot, the console commands, the mesh preset columns
  block and the `(mesh)` suffix of the `[preset]` line, and the hook calls in `r2_R_render.cpp` /
  `render_phase_sun.cpp` (`da_grass_mesh_frame_prepare`, `da_grass_mesh_frame` for the
  `parallel_details` skip, `da_grass_ab_*`; `da_grass_hzb` went with the Hi-Z, 3.11). This lane owns `d12_mesh.cpp` (mesh
  root signature, stream-PSO branch, `PrepareMesh`, `DispatchMesh`, draw refusal, stat - reached
  through hooks infra pre-places at I0), `scenarios_mesh.cpp`, `da_grass_mesh.h`,
  `DetailManager*`, `dx11DetailManager_VS.cpp`, the blender files and its shaders.
- **C21**: the refusal verdict is written to its own `render-grass-mesh.cache` next to the caps
  cache by `DetailManager_MS.cpp` (no `r2_test_hw.cpp` change); the preset replay asks
  `da_grass_mesh_expected()`.
- **C20**: layer scenarios in `scenarios_mesh.cpp`.
- **Externals/dxc** is vendored by `core` (K1), not here.
- The only UI edit allowed to this lane: the sentence in `st_da_x64_video_hint_preset` (cp1251) at
  M10 when the measured column differs.

## 1. Goal

1. Draw the detail grass on DX12 with amplification + mesh shaders (tier 1): the amplification
   shader (AS) culls per instance (pass frustum, the grass shadow distance rule), the
   mesh shader (MS) emits the tufts from the same instance rows the engine already builds, in the
   G-buffer pass, the sun cascades 0/1 and the local-light shadow maps.
2. **Identical look**: same rows, same wind maths (one shared header for VS and MS), same fade,
   same pixel shader source, same textures/samplers/stencil/velocity. Proven per frame by an in-frame
   A/B tool, not by eye.
3. Show the saving (CPU submission, GPU vertex ALU, culling) with the stand's interleaved protocol.
4. Spend the saving as policy: a DX12-mesh column of the preset ladder raises grass reach (and, if
   the budget allows, density) exactly as far as the measured equal-cost rule allows.
5. Automatic fallback to today's path wherever mesh shaders, DXC or the pipeline are unavailable.

## 2. Current code

### 2.1 Per-frame data flow
- `CRender::OnCameraUpdated` (`r2.cpp:1088-1106`) -> `CDetailManager::UpdateRenderState`
  (`DetailManager.cpp:551-625`: wind clocks `m_time_pos`, `m_wind_dir1/2` and their `_prev`) and
  `DispatchMTCalc` (`:701-835`): a task runs `cache_Update` (`DetailManager_CACHE.cpp:101-253`, at
  most `dm_max_decompress` = 7 slot decompressions per frame, `:183-230`) and `UpdateVisibleM`.
- `UpdateVisibleM` (`DetailManager.cpp:329-517`): main-view frustum per 8x8 m block and per 2x2 m
  slot, `HOM.visible` per slot (`:416`), and every 15-30 frames per slot ("refresh", `:422-483`) the
  fade (`:343-356`, `:448-454`), the ssa discard and the **packed rows**: `InstanceRows` = 4 x float4
  (3 rows of a 3x4 transform with height scaled separately, then `c_sun,c_sun,c_sun,c_hemi`),
  `DetailManager.h:76-79`, stored per `SlotPart::r_rows[vis_id]` (`:96-106`). Visible parts are
  pushed per wave group (0 still, 1 and 2 wave) and object id into `m_visibles` as
  `VisiblePart{items, rows, slot}` (`:85-94`, `DetailManager.cpp:484-510`).
- Rows depend only on the refresh; between refreshes the same bytes are re-sent every pass.

### 2.2 The draw
- `CDetailManager::Render` (`DetailManager.cpp:627-662`) -> `hw_Render`
  (`dx11DetailManager_VS.cpp:31-81`): three dumps (wave 1, wave 2, still) with `wave`/`dir2D` and
  their previous values. `hw_Render_dump` (`:83-278`), per object and pass: `set_Element(E[lod])`,
  ten `set_c` (`consts, wave, dir2D, xform, grass_sfade, grass_sfade_eye, grass_tint, wave_prev,
  dir2D_prev, da_wm_info_prev`, `:181-196`), then per part: shadow distance test on the slot centre
  (`:221-222`), `IsPartVisible` (slot AABB vs pass frustum, cached per context, `DetailManager.cpp:533-549`),
  `memcpy` of the rows into the `DetailInstances` cbuffer and `DrawIndexedInstanced` per 61
  instances (`:214-275`). Geometry: one copy per model, `vertHW{float3, short u,v,t,mid}`
  (`DetailManager_VS.cpp:21-25`, `:42-118`).
- Shadow passes are the same dump with a frustum: `grass_sfade` band (`:104-114`) and the distance
  cut `ps_r__grass_shadow_dist` (`:130`).

### 2.3 Callers
- G-buffer: `r2_R_render.cpp` `render_details` (`:360-369`), no-split part 0 (`:371-385`) or split
  part 1 (`:532-555`). On DX12 the three dumps are recorded on three deferred contexts in parallel
  (`parallel_details`, `:33-127`) and executed where the grass always drew.
- Sun: `render_phase_sun.cpp:414-431`, cascades 0 and 1 only (`cascade_ind + 1 < R__NUM_SUN_CASCADES`),
  recorded on deferred contexts inside the `r_sun` task during `CRender::Calculate`, executed at
  `r_sun.sync()` (`r2_R_render.cpp:633`).
- Local lights: `r2_R_lights.cpp:178-193` (`ps_r__light_details`, lights within 40 m; the comment
  there measured this pass at "about a quarter of the whole frame" on grassy levels).

### 2.4 Shaders and material
- `deffer_detail_w_flat.vs` (overlay `shaders/r3`): `detail_bend` (`:32-88`), arc-length drop
  (`:93-97`), sfade (`:116-123`), bend twice when motion vectors are drawn (`:139-143`), tint noise
  (`:171-189`). `deffer_detail_s_flat.vs`: motors only (`:21-35`). Wind: `da_wind_field.h`,
  `da_wind_motors.h` (8 motors + previous rows).
- Blender `CBlender_Detail_Still::Compile` (`Blender_detail_still_deferred.cpp:48-100`): only
  elements 0 (wave) and 1 (still), via `uber_deffer(C,false,"detail_w"/"detail_s","base",true,0,true)`
  -> `deffer_detail_*_flat.vs` + `deffer_base_aref_flat.ps` (`uber_deffer.cpp:33-81`, `:177`),
  stencil ref 0x01, cull none. Elements 2..5 of every detail `Shader` are compiled empty.
- The PS reads `$Globals`, `dynamic_transforms`, `s_base`, `smp_base` and writes the packed G-buffer
  + velocity. In the sun pass no colour target is bound (`r3_rendertarget_phase_smap_D.cpp:15-21`,
  `rt_smap_surf` is null on DX10+): the outputs are dropped, only `clip(D.w - def_aref)` matters.

### 2.5 Engine shader/constant machinery
- `CResourceManager::CreateShader` (`ShaderResourceTraits.h:616-700`) strips `(...)` from names
  (`:637-645`) and compiles through `CRender::shader_compile` (`r4_shaders.cpp:250`, FXC at `:653`,
  `SMAP_ARRAY` on R5 at `:574`, cache `shaders_cache_xfr/r5/<name>.<ext>/<options>`).
- `R_constant_table::parse` (`dx11r_constants.cpp:288-330`) uses the reflection **index** of a
  cbuffer as its slot (`:316-325`) and accepts FLOAT/BOOL/INT variables only (`:43-49`);
  structured buffers are skipped (`:196`).
- `CBackend::Compute`/`RenderInstanced` (`dx11R_Backend_Runtime.h:324-337`, `:389-419`) show the
  flush order (SRVs, RT/ZB, state manager, constants, draw).

### 2.6 Presets
`xrRender_sync_preset_derived` (`xrRender_console.cpp:1086`): `grass_fade_by_preset` (`:1169`),
`detail_density_by_preset {0.6,0.6,0.5,0.35,0.25}` (`:1183`, applies on level load),
`detail_radius_by_preset {49,70,100,130,160}` (`:1188`, through `r__detail_radius`, `:1321-1323`),
`grass_shadow_by_preset {40,50,60,75,95}` (`:1225`, tied to the middle cascade box). Measured before
(distance-ladder work): 100 -> 160 m costs +2 ms at 1080p Extreme, the whole price of the ladders.

### 2.7 Defect found while reading (fixed first, both APIs)
`cache_Task` (`DetailManager_CACHE.cpp:48-80`) re-targets a slot that scrolled out of the cache but
leaves `SlotPart::r_items` (pointers to the `SlotItem`s it just destroyed) and `r_rows` (rows of the
old location) in place and does not reset `Slot::frame`. Until the slot's next refresh (up to 30
frames) `UpdateVisibleM` pushes the old rows for drawing whenever the new slot box is visible:
wasted vertex work on grass at the old, far edge; the soft path would dereference freed items. Fix:
in `cache_Task` clear `r_items[0..2]`, `r_rows[0..2]`, set `D->frame = 0`, bump the part generation
(3.2). No visual change is expected beyond the absence of those rows.

Implemented (M1): `cache_Task` clears the render lists and sets `frame = 0`; `cache_Decompress` also
sets `frame = 0`, because a re-targeted slot that is visible while still pending is refreshed with
no items and scheduled 15-30 frames ahead - its grass appeared up to 30 frames after the
decompression (the stock code lagged the same way through the old place's schedule). The part
generation is added with the row pool (M6), where it has a reader. Gate: `render_check foliage`
R5 and R4 equal Gate 0 (runs `mesh-m1-foliage`, `mesh-m1-foliage-r4`).

### 2.8 Probes on this machine (offline, session scratchpad `meshprobe/`)
- RTX 3070 Laptop: mesh shader tier 1, highest SM 6.8, GPU upload heap supported, waves 32..32
  (matches `infra.md` 2).
- The real headers compile with DXC 1.8.2502 (SDK) and 1.9.2607 (`_work/sdk-downloads`), `-Zpr -O3`:
  `deffer_detail_mesh.ms` (wave and still), `deffer_detail_cull.as`, `deffer_base_aref_flat.ps` as
  `ps_6_5` (one truncation warning), a depth-only PS. DXC strips unused resources: the MS binds only
  `$Globals` b0, `dynamic_transforms` b1, `da_motion_xform` b2, t0..t2.
- MS output signature = `v2p_flat` registers 0..4 + `SV_Position` 5; the DXIL PS input is registers
  0..4: links. Stream PSOs AS+MS+PS created for the packed G-buffer (3 RTs, D24S8) and depth-only
  (0 RTs, depth bias) with both the G-buffer PS and the depth PS. Trap found: formats past
  `NumRenderTargets` must be `UNKNOWN`, otherwise `E_INVALIDARG`.
- FXC instruction slots: `deffer_detail_w_flat.vs` 639; the same shader with the per-instance terms
  supplied precomputed (what the MS computes once per tuft) 362 - **43% of the grass vertex work is
  per instance** (field eval x2, sway x2, local heading x2, tint hash). Still VS: 357.

## 3. Design

### 3.1 Overview
```
calc task (existing, per frame)          render thread, top of CRender::Render      every grass pass
UpdateVisibleM -> rows per part  ---->   frame prep: copy new pages (native      ->  per object and wave group:
+ page allocation (pool, LRU)            section, CopyBufferRegion) + chunk list      set_Element(E[2|3]), constants,
+ rows of new/refreshed parts            into DEFAULT buffers                         RenderMesh(chunks) -> AS (cull,
  into the upload ring                                                                compact) -> MS (tufts) -> PS
+ chunk list, dispatch ranges
```
Rows stay GPU-resident; a pass costs one `DispatchMesh` per object and wave group instead of
`instances/61` draws with a `memcpy` each.

### 3.2 GPU-resident rows
- **Pool**: one structured buffer of `InstanceRows` (64 B), created through D3D11 as DEFAULT
  without initial data (the layer makes it a tracked committed resource, `d12_objects.cpp:1386-1395`),
  split into **pages of 32 rows** (2 KB). Capacity from `QueryVideoMemory` (infra 3.4): 24 MB on
  <= 4 GB cards, 48 MB <= 8 GB, 96 MB above (12k/24k/48k pages).
- **Ownership**: `SlotPart` gets `u32 gen` (bumped at every refresh of its rows and in `cache_Task`),
  and per wave group `GpuRows{u32 gen; u32 lastFrame; small_vector<u32,4> pages}`.
- **Allocation** (calc task, single-threaded there): after `UpdateVisibleM`, for every visible part
  whose `GpuRows::gen != SlotPart::gen`: free its old pages, allocate `ceil(n/32)` pages, append an
  upload record. Pages freed in frame N can be reused by frame N's own uploads: every list that read
  them was executed before frame N's prep on the same queue, and no list of frame N references them.
- **Eviction**: when the free list is empty, pages of parts not visible this frame go first,
  oldest `lastFrame` first. If the visible set itself does not fit, the remaining parts are drawn by
  the vertex path this frame (**hybrid**, 3.4) and `r__grass_stats` counts them.
- **Upload ring**: `CreateNativeBuffer` (infra 3.4) UPLOAD heap, persistently mapped, 4 slots x
  8 MB, written by the calc task (rows of new pages, then the frame's chunk list). A slot is reused
  after its batch was consumed and 4 frames passed; a frame whose Render is skipped leaves its batch
  for the next prep (FIFO). More than 8 MB new rows in one frame (first frame after a load, a
  teleport): the rest of the new parts go hybrid this frame and upload next frame.

### 3.3 Frame data and frame prep
- Calc task output `MeshFrame`: chunk list `grass_chunk{u32 base; u32 count; float3 centre}` (one per
  page; `centre` = slot sphere centre, for the exact shadow-distance rule), per (wave group, object)
  `{firstChunk, chunkCount}`, per object `overflow` part list, upload batch.
- `CDetailManager::MeshFramePrepare(CBackend& imm)` at the top of `CRender::Render` (DX12 only, after
  the sync point, before any grass list executes - the sun lists recorded earlier execute later at
  `r_sun.sync`, so queue order is guaranteed): one native section (`BeginNative`, `Require(pool,
  COPY_DEST)`, one `CopyBufferRegion` per contiguous page run and one for the chunk list, `Require`
  both to `NON_PIXEL_SHADER_RESOURCE`, `EndNative(0)`). No `memcpy` on the render thread.
- `UpdateRenderState` latches `m_mesh_frame = mesh_ready && ps_r__grass_mesh` once per frame; the
  calc task, the prep and all passes of the frame use that value.

### 3.4 Per-pass draw (`CDetailManager::ms_Render`, new `DetailManager_MS.cpp`)
- Replaces `hw_Render` when `m_mesh_frame`; same three dumps and `dumps` mask (so every caller
  works unchanged), same per-pass constants as `hw_Render_dump` (`:181-196`) plus:
  `grass_planes[12]` (the pass's raster volume: the six planes of the pass's current WVP, i.e. the
  jittered main view, the cascade's `combine`, the light face; unused planes = never cull; see 13.1
  "AS planes"), `grass_dispatch` (x first chunk, y count, z shadow distance squared or 0,
  w bend margin), `grass_obj` (first vertex, first index, `nv | nt<<16`, instances per MS group),
  `grass_bound` (model `bv_sphere`). All float4 (the constant table rejects uint, `:43-49`).
- Per object: `set_Element(E[2]` wave / `E[3]` still`)`, `SRVSManager.SetVSResource(0..2)`
  (vertices, indices, pool) and `SetGSResource(2..3)` (pool, chunks), `cmd_list.RenderMesh(chunks)`;
  split at 65535 groups. Then the object's `overflow` parts through the existing
  `hw_Render_dump` loop (hybrid). `parallel_details::start` does nothing when `m_mesh_frame`: tens
  of dispatches need no parallel recording, the grass draws inline on the immediate context.
- **Culling rules (exactness)**: AS drops a row only when it provably draws nothing different:
  (a) shadow passes: the chunk's slot centre farther than the grass shadow distance - the CPU rule of
  `:221` verbatim; (b) conservative sphere outside the pass's raster volume (not its culling
  frustum, which the per-slot tests keep using): centre = root + rows x model
  sphere centre, radius = model radius x max row scale + 1.11 x (model top y x height scale) (the
  bend cap 0.92 H plus the arc drop bound the displacement by 1.103 H); (c) scale zero (faded rows
  rasterize nothing). Flat (height 0) rows are **kept**: they still rasterize a footprint today.
  G-buffer: CPU slot tests already ran; the AS adds the per-instance frustum.
- MS limits per object: `ipg = min(32, 64 / nv, 126 / nt)` (NVIDIA's recommended 64 vertices /
  126 primitives, 64 threads); a model that does not fit even once goes to the vertex path (logged
  per level). Level models seen on the stand: 609-679 vertices for all objects together.

### 3.5 xrD3D12: mesh draws on the D3D11 path
A D3D11-path extension like `vrs.md` 3.4, on top of infra step 7 (DXIL shaders through the D3D11
API, `Reflect` adapter):
- **Stage kinds**: a DXIL container whose program kind is mesh (13) is accepted by
  `CreateVertexShader`, amplification (14) by `CreateGeometryShader`; `Shader::kind` records it. They
  bind to the VS and GS slots with the normal D3D11 calls (constant buffers, SRVs), so the engine's
  constant table, binders and SRV cache work unchanged. `Draw*` with a mesh-kind VS bound is refused
  (log once).
- **Root signature**: `RootSignatures::mesh` (`d12_infra.cpp:2302`) = the graphics layout with the
  VS parameters at MESH visibility and the GS parameters at AMPLIFICATION visibility (same `GfxRoot`
  indices, so `ApplyStage`, `d12_context_state.cpp:418`, is reused as is), no IA flag.
- **PSO**: `FillPsoDesc` (`:226`) carries `meshKind`; `PipelineCache::Graphics` (`d12_infra.cpp:2507`)
  builds a stream (root signature, AS, MS, PS, blend, raster, depth-stencil, sample mask, RT formats
  with the unused ones `UNKNOWN`, DSV format, sample desc) through `ID3D12Device2`, stored in the
  pipeline library via infra's `StreamPipeline`; the hash includes the kinds.
- **Draw**: `bool xrd3d12::DispatchMesh(ID3D11DeviceContext*, UINT x, UINT y, UINT z)` on any
  context -> `Context::PrepareMesh` = `ApplyGraphics` with the mesh root signature and PSO,
  `EnsureDrawStates`, `FlushBarriers` -> `m_list6->DispatchMesh` (`ID3D12GraphicsCommandList6`
  queried in `OpenList`, `d12_context_ops.cpp:1339`, next to infra's `m_list5`). Returns false when
  the pipeline was refused, so the caller can fall back. `-dxstats` gets a mesh dispatch counter.
- Deferred contexts need nothing new: bindings, trackers and `ExecuteCommandList` hand-off are the
  draw path's.

### 3.6 Engine side
- **Names**: `name#ms` / `name#as` (PLAN C5: no `#dxil`; the PS is DXIL because the session is in
  DXC mode). `CreateShader` strips the `#...` suffix for the file like `(...)`, picks `.ms`/`.as` and
  targets `ms_6_<m>`/`as_6_<m>` (engine SM, >= 6.5); `shader_compile` compiles them through the
  engine DXC path of `dxc.md` 3.3 (FXC preprocessing with the engine includer, `-Zpr -O3 -HV 2021`,
  the same option macros and `SMAP_ARRAY`, `XFR_DXC=1`), cache `r5_dxil/<tag>/...` like every DXIL
  engine shader, reflection blob kept.
- **create_shader** (`r4_shaders.cpp:15-138`): DXIL VS/GS/PS skip `D3DGetInputSignatureBlob` and
  reflect through `xrd3d12::Reflect`. `R_constant_table::parse` takes the cbuffer slot from the
  binding's `BindPoint` for DXIL (DXC orders cbuffers by first use; the DXBC path keeps the index,
  unchanged). Mesh constants get `RC_dest_vertex`, AS `RC_dest_geometry` - the stages they ride.
- **CBackend::RenderMesh(x,y,z)** (`R_Backend.h`, `dx11R_Backend_Runtime.h`, R5 only): the flush order
  of `Compute` plus `ApplyRTandZB`, then `xrd3d12::DispatchMesh`; stats.
- **Blender**: on R5 with mesh grass available, `CBlender_Detail_Still::Compile` fills elements
  2 (wave) and 3 (still) through `uber_deffer(..., mesh = true)`: same texture/sampler/stencil/cull
  setup, pass `C.r_Pass("deffer_detail_mesh#ms(DETAIL_WAVE)" | "deffer_detail_mesh#ms",
  "deffer_detail_cull#as", "<the PS uber_deffer names>", ...)` through the existing 3-shader
  `r_Pass` overload. Elements 4/5: the depth-only shadow variants (3.10, kept as the shadow path).

### 3.7 Availability and switches
- `mesh_ready` = R5, `HW.native.meshShaderTier >= 1` and `sm >= 65` (infra caps; `-dx12_no_ms`
  masks it), MSAA off, the detail shaders compiled, the first mesh PSO accepted, and the level's
  `deffer_detail_w_flat.vs` / `deffer_detail_s_flat.vs` include `da_detail_common.h` (a mod that
  replaced them with its own wind keeps its look on the vertex path - a mechanism, no mod names).
- A refused mesh pipeline at run time: vertex path for the session, one log line, and the verdict is
  stored in `render-grass-mesh.cache` next to infra's `render-caps-d3d12.cache` (the same signature
  scheme minus the per-boot `AdapterLuid`: vendor/device/subsystem/revision ids, adapter name,
  dedicated video memory, UMD version, `d3d12.dll` size and time, game version; written by
  `DetailManager_MS.cpp`, PLAN C21) so the next start picks the base preset column (3.9), after a
  reboot too.
- `r__caps` `ms` (infra) says whether the hardware can; the grass log line says whether it runs:
  `* [grass-mesh] on: N objects, ipg 2..6, pool 48 MB (24576 pages), K models via vertex path`.
- Console (not saved): `r__grass_mesh -1|0|1` (-1 auto, the default: the offloads' decision of the
  previous frame's end, latched in `ms_Latch` with the rest of the frame's state; 0/1 force a path for
  runtime A/B), `r__grass_stats` (instances,
  chunks, pages used/free, uploads, hybrid parts, dispatches per pass, decompress backlog),
  `r__dbg_grass_ab` (3.8).

### 3.8 Parity tool `r__dbg_grass_ab`
Next frame, inline (no parallel contexts): before the G-buffer grass copy the G-buffer targets and
depth-stencil to scratch; draw grass through the vertex path; copy the result; restore the scratch;
draw through the mesh path; copy. Same for the cascade 0/1 slices inside `render_cascade` (depth
slice copy on the deferred context). Results are read back a few frames later through staging
copies and compared on the CPU: per target the number of differing pixels, max and mean absolute
difference, the grass-pixel count from stencil 0x01. Logged as
`[grass-ab] gbuffer color: px 0.012% max 1/255 ...`. Same uniforms, same frame - the only variable is
the path.

### 3.9 Presets (policy)
`xrRender_sync_preset_derived` gets two columns used when mesh grass will run as the preset replays
(renderer start, `r2.cpp:601`, and every preset change): `r__grass_mesh` on and the path available
(availability is known from caps and the stored verdict; `da_grass_mesh_expected()`):
```cpp
static constexpr int detail_radius_mesh_by_preset[] = {49, R1, R2, R3, R4};      // measured, section 12 step 11
static constexpr float detail_density_mesh_by_preset[] = {0.6f, 0.6f, 0.5f, 0.35f, D4}; // measured
```
Rule: Minimum stays 49 m (distance-ladder rule). For every other rung, the largest radius (steps of
5 m, cap 200 m) whose frame time with mesh grass is not above the vertex path's at the base rung
(DX12, same save, interleaved, within the 1.3% noise band), with no decompress backlog left 2 s
after a sprint (9.3 "Sprint", section 12 step 11). Density moves only on Extreme and only if radius reached the cap. Grass shadow
distance and the cascade boxes do not change. The preset log line appends `(mesh)`. `r__grass_mesh`
switched at runtime keeps the rung the replay chose (that is how equal-setting A/B is measured: the QA
scenarios toggle it in the level); a `r__grass_mesh 0` in force at the replay (a `-PreCommands` line)
gives the base rung, and so does the switch's default while it is 0. Under auto (-1) the replay takes
the auto decision of that moment (the CPU path at the renderer's start); the columns are equal anyway.

### 3.10 Tuning variants (kept only if faster and A/B-identical)
MS output 64/126 (64 threads) vs 128/256 (128 threads; likely better on AMD/Intel, chosen by
vendor); per-instance terms in the AS payload vs MS groupshared; a depth-only PS
(`deffer_detail_mesh_depth.ps`, the alpha test only) for shadow passes as elements 4/5. Only the
depth-only elements were built; they are the shadow path of mesh grass without a switch (13.2).

### 3.11 Hi-Z (phase 2): removed
G-buffer only: `da_grass_hzb.cs` (FXC) reduced the render-size depth to a max-depth pyramid right
before the grass and the AS tested each tuft's sphere against it. Gate: keep only if it saves
>= 0.15 ms of `grass` on `foliage` or `fogqa` at 1440p Extreme. The final one-pass run (13.2) gave
`grass` 1.89 -> 1.94 ms with it on - no saving - so the switch, the pyramid, `da_grass_hzb.cs` and the
AS test were removed.

## 4. Integration points

| File | Change |
|---|---|
| `src/Layers/xrRender/DetailManager_CACHE.cpp` | `cache_Task`: clear `r_items/r_rows`, `frame = 0`, bump gen (2.7) |
| `src/Layers/xrRender/DetailManager.h/.cpp` | `SlotPart::gen`, `GpuRows`; `MeshFrame`; latch in `UpdateRenderState`; mesh step after `UpdateVisibleM` in the task; `Render` dispatches to `ms_Render`; `MeshActive()`; `Load/Unload` create/free mesh resources |
| `src/Layers/xrRender/DetailManager_MS.cpp` (new) | pool, pages, LRU, upload ring, chunk list, `MeshFramePrepare`, `ms_Render`, hybrid, stats, A/B tool; body `#if defined(XR_D3D12)` |
| `src/Layers/xrRenderDX11/dx11DetailManager_VS.cpp` | `hw_Render_dump` over an explicit part list (hybrid) |
| `src/Layers/xrRender/blenders/Blender_detail_still_deferred.cpp`, `uber_deffer.cpp/.h` | mesh elements 2/3 (4/5) |
| `src/Layers/xrRender/ShaderResourceTraits.h` | `#ms/#as/#dxil` names -> extension and target |
| `src/Layers/xrRenderPC_R4/r4_shaders.cpp` | DXC route (infra), DXIL `create_shader` variant |
| `src/Layers/xrRenderDX11/dx11r_constants.cpp`, `xrRender/r_constants.h` | DXIL cbuffer slot from `BindPoint` |
| `src/Layers/xrRender/R_Backend.h`, `xrRenderDX11/dx11R_Backend_Runtime.h` | `RenderMesh` (R5) |
| `src/Layers/xrRender_R2/r2_R_render.cpp` | `MeshFramePrepare` at the top of `Render`; `parallel_details::start` skip; A/B hooks |
| `src/Layers/xrRender_R2/render_phase_sun.cpp` | A/B hook around `Details->Render` (QA only) |
| `src/Layers/xrRender/xrRender_console.cpp/.h` | mesh ladder columns, `(mesh)` log, `r__grass_mesh`, `r__grass_stats`, `r__dbg_grass_ab` |
| `src/Layers/xrRenderPC_R4/r2_test_hw.cpp` | nothing (PLAN C21: the verdict file is written by `DetailManager_MS.cpp`) |
| `src/Layers/xrRenderDX11/dx11GpuTimers.h/.cpp` | slot `Grass` around `render_details` (immediate context; covers the executed parallel lists too) |
| `src/Layers/xrRenderDX12/d12_api.h`, `d12_layer.h` | `DispatchMesh`, `Shader::kind`, `RootSignatures::mesh`, `Context::m_list6`, mesh stat |
| `src/Layers/xrRenderDX12/d12_mesh.cpp` (new) | DXIL kind detection, mesh RS, stream PSO build, `PrepareMesh`, `DispatchMesh` |
| `d12_infra.cpp`, `d12_context_state.cpp`, `d12_context_ops.cpp`, `d12_objects.cpp`, `d12_device.cpp` | RS init, PSO cache branch, draw refusal, list6 QI, kind in `Shader::Reflect`, `ID3D12Device2` |
| `src/Layers/xrRenderDX12/CMakeLists.txt`, `xrRenderPC_R4/sources.cmake` | new files |
| `packaging/.../shaders/r3/*` | shaders (section 6) |
| `tools/qa/Test-Shaders.ps1`, `tools/qa/Check-ShaderLinkage.py` | DXC permutations, MS->PS pairs |
| `tools/qa/d3d12layer/scenarios_mesh.cpp` | mesh scenarios (9.1, PLAN C20) |
| `tools/qa/water/grass_ab.lua`, `grass_run.lua` (new) | stand scenarios |
| `docs/dead-air/DX12_RENDERER.md`, `TEST_MATRIX.md` | section and rows |

## 5. Data and resources

| Resource | Format | Size | Lifetime / writer |
|---|---|---|---|
| Model vertices | `StructuredBuffer<detail_vtx>` {float3, 2 x packed s16 pairs}, 20 B | ~14 KB/level | immutable, `Load` (same `QC` quantization as `hw_Load_Geom`) |
| Model indices | `ByteAddressBuffer`, u16 packed | ~2 KB/level | immutable, `Load` |
| Row pool | `StructuredBuffer<InstanceRows>` 64 B, pages of 32 | 24/48/96 MB | DEFAULT tracked, per level; CopyBufferRegion in frame prep |
| Chunk list | `StructuredBuffer<grass_chunk>` 20 B | up to 131072 chunks, 2.5 MB | DEFAULT tracked, rewritten every frame |
| Upload ring | native UPLOAD, persistently mapped | 4 x 8 MB | calc task writes, frame prep copies |
| CPU | `SlotPart::gen` + 3 x `GpuRows`; `MeshFrame` vectors | ~40 B per part + chunk vector | calc task |

Constants (engine constant table): `grass_obj`, `grass_dispatch`, `grass_bound`, `grass_planes[12]`
plus every constant the vertex path sets. Pool at Extreme 160 m on a dense meadow: ~100-250k visible
rows (6-16 MB) + retained parts; 48 MB leaves room, the stats command verifies.

## 6. Shaders (overlay `packaging/dead-air-x64/compatibility/gamedata/shaders/r3`)

- `da_detail_common.h` (new, M2): the grass maths as pure functions (every constant a parameter,
  so the includer keeps its constant layout): `detail_sfade_k` (shadow-band factor; the caller
  scales `m*.y`), `detail_world_pos`, `detail_height_scale`/`detail_height`, `detail_shelter`,
  `detail_sway`, `detail_wind_at` (per tuft: sway value, shelter, field eval, local heading),
  `detail_bend_v` (per vertex from those terms), `detail_bend_tail` (motors, wave, gust lean, cap -
  shared by both), `detail_bend` (the same bend in the vertex shader's order), `detail_bent_pos`,
  `detail_motor_pos`, `detail_tint_n` (per tuft), `detail_tint_hemi` (per vertex), `detail_v2p`
  (interpolants; the caller writes `position.w`). The waving part needs `da_wind_field.h` included
  first; the still VS does not include it, because FXC keeps unused `$Globals` and three more
  constants would move its layout.
- `deffer_detail_w_flat.vs`, `deffer_detail_s_flat.vs`: rewritten to call the header - same maths.
  FXC schedules in source order and copies `inout`/struct arguments differently, so the vertex
  shaders compose the pieces in their original statement order and keep the shadow-band mutation
  in `main`; result (M2 gate, `_work\dx12-features-20260928\mesh\detail_dxbc_identity.py`): 40
  permutations (engine options R4/R5 +- static sun, the eight `Test-Shaders` permutations +- static
  sun) **byte-identical** DXBC, the harness reproduces the engine's cached bytecode byte for byte,
  and the engine's recompiled cache after the change equals the old one. The header also compiles
  under DXC 1.9 (`vs_6_5`, HLSL 2018 and 2021; only the pre-existing `mul(m_WV, float3)`
  truncation warnings). Frame check: the Gate-0 binaries with the Gate-0 overlay plus only these
  three files give an R4 `foliage` parity frame equal to the Gate-0 references (run
  `mesh-m2-iso-foliage-r4`, calibrated equal, 99.71 % within).
- `da_detail_mesh.h` (new): `detail_vtx`, payload {base, count, 8 x packed u8 offsets}, grass
  constants, `detail_index`.
- `deffer_detail_mesh.ms` (new): 64 threads; threads `< n` load their row (sfade applied) and compute
  the per-instance terms (current and, when `da_wind_prev_on()`, previous) into groupshared; then
  vertices and triangles of `n = min(ipg, count - g*ipg)` tufts, output `v2p_flat` exactly as the VS
  (hpos, hpos_cur, hpos_prev, N, position.w hemi with tint, tcdh incl. `USE_R2_STATIC_SUN`).
  `DETAIL_WAVE` selects wave vs still.
- `deffer_detail_cull.as` (new): 32 threads = one chunk; culling 3.4; compaction through a group bit
  mask + `countbits` (keeps row order, independent of wave size: 16 on some Intel, 64 on some AMD);
  `DispatchMesh(ceil(total / ipg))`.
- `deffer_base_aref_flat.ps`: unchanged source; DXIL because the session runs in DXC mode (PLAN C5).
- `deffer_detail_mesh_depth.ps` (new): the depth-only PS of the shadow elements 4/5 (3.10).
- Probe versions of the new files: session scratchpad `meshprobe/r3/`.

## 7. UI and presets

- No menu row: mesh grass is how DX12 draws grass where available and the CPU is the bottleneck (auto); the RTX group and
  the frame-generation pattern are not involved. `r__caps` key `ms` comes from infra.
- Presets: the mesh columns of 3.9. If the measured column differs from the base one, the preset
  hint (`st_da_x64_video_hint_preset`, rus/eng, cp1251) gains one sentence ("На DirectX 12 трава видна
  дальше при той же цене кадра." / "On DirectX 12 grass reaches further at the same frame cost.").

## 8. Fallback

| Condition | Result |
|---|---|
| DX11 (R4) | today's path; the only R4 changes are the 2.7 fix and the refactored VS sources (identical DXBC) |
| No mesh tier 1 / SM < 6.5 / no DXC / `-dx12_no_ms` | vertex path, base preset column, `ms=0` |
| MSAA on | vertex path (the ATOC elements exist only there) |
| Detail VS replaced by a mod | vertex path for the session, log line |
| Mesh PSO refused / `DispatchMesh` false | vertex path for the session, verdict stored, base column next start |
| Model over the MS limits | that model on the vertex path |
| Pool or upload ring full | those parts on the vertex path this frame (hybrid) |
| `r__grass_mesh -1` (default) | mesh while the CPU is the bottleneck, vertex path while the GPU is or no GPU timers exist (gpudriven.md 3.13) |
| `r__grass_mesh 0` at runtime | vertex path at the rung the last preset replay chose - QA only |
| `r__grass_mesh 0` at a preset replay | vertex path, base preset column, no `(mesh)` |

## 9. Test plan

Short runs (60-90 s), shader cache kept unless shaders changed; stand = `Invoke-Stand` from
`locked.ps1` on `D:\Games\Dead Air\_qa\lightdiag`.

### 9.1 Layer (`tools\qa\d3d12layer\Run-LayerTests.ps1 -HighPerformance`, then `-Gbv`)
Existing 36 + infra's pass; new: `mesh dispatch` (AS+MS+PS instanced quads with a culled subset vs
the same quads by `DrawIndexedInstanced` on the D3D11 reference: pixel-exact colour and depth),
`mesh on deferred context` (recorded on a deferred context, executed, same image), `mesh in depth-only
pass` (0 RTVs, depth bias: depth equal to the reference), `draw with mesh VS refused` (one log line,
no draw). Pass: all green, debug layer and GBV silent.

### 9.2 Offline
- `python tools\qa\Check-ShaderLinkage.py`: MS->PS pairs of elements 2..5 in every permutation. Pass: 0 failures.
- `tools\qa\Test-Shaders.ps1` (with DXC for `.ms/.as/#dxil`): 0 errors.
- DXBC identity: `fxc /Fc` of `deffer_detail_w_flat.vs` and `deffer_detail_s_flat.vs` before/after
  the refactor, all Test-Shaders permutations: instruction streams identical.

### 9.3 Stand: parity and visuals
| Run | Command | Pass |
|---|---|---|
| A/B, no AA | `-Name grass-ab -Scenario grass_ab -Save foliage -ExtraArgs '-r5 -always_active' -PreCommands @('r__aa st_aa_off')` (3 yaws 0/120/240, `r__dbg_grass_ab 1` each) | per frame: G-buffer pixels differing in any target <= 0.10%, elsewhere max diff <= 2/255 (UNORM) / 1e-3 rel (FP16), depth and stencil identical outside those pixels, grass-pixel count equal within 0.1%; cascade 0/1 texels with depth diff > 1e-6 <= 0.05% |
| A/B, TAA + vectors | same with `@('r__aa st_aa_taa','r__motion_vectors 1')` | as above; velocity max diff <= 0.05 px |
| A/B, open level | same on `-Save fogqa` | as above |
| Visual | `-Scenario render_check -Save foliage -Globals "qa_render_cmds = { 'r__grass_mesh 1' }"` and `{ 'r__grass_mesh 0' }` | side-by-side captures: no missing patches, fade band and near/middle shadows present |
| Motion | `-Scenario motion_check -Save foliage -Globals "qa_render_cmds = { 'r__aa st_aa_taa', 'r__motion_vectors 1', 'r__grass_mesh 1' }"` vs `0` | vector views equal; sway continuous |
| Sprint | `-Scenario grass_run -Save foliage` (actor moved 7 m/s for 10 s, `r__grass_stats` each second) | backlog 0 within 2 s after stop; 0 hybrid parts after the first 2 s; no holes in captures |
| Validation | `render_check` with `-r5 -always_active -dxdebug`, then `-dx12gbv` | no D3D12 errors, no `refused` |
| Fallback | `render_check` with `-r5 -always_active -dx12_no_ms`; `render_check` DX11 on `foliage` and `qa_pedro` | vertex path, base column in the preset log; DX11 frames/FPS equal the pre-change baseline |

### 9.4 Performance (ninja.lock held; `ab_run.ps1` + `analyze.ps1`)
2560x1440 Extreme TAA, `r__gpu_log 300`, `-dxstats`, saves `foliage`, `fogqa`, `qa_pedro` (control),
runs interleaved `m1a, m0a, m1b, m0b, m1c, m0c` (`r__grass_mesh 1/0` via `-PreCommands`, same DLL,
first pair warm-up; a `-PreCommands` switch is in force at the preset replay, so `m0*` runs take the
base rung - equal settings while the mesh columns equal the base ones; once M10 changes them, the
equal-setting A/B toggles the switch in the level instead, `qa_grass_gpu`), nvidia-smi clocks. Engine profiler on the render thread for
`CDetailManager::*`. Pass: FPS(mesh) >= FPS(vertex) - 0.5% on every save; `Grass` GPU slot and
`Shadows` not higher; grass render-thread time down >= 70%; `qa_pedro` within noise.

## 10. Performance expectation (to be confirmed by 9.4)
- CPU: today ~`instances/61` draws per pass with a 3.9 KB `memcpy` each (a grassy G-buffer ~1-2k
  draws plus the cascades) and "a fifth of the render thread"; mesh: 30-100 dispatches per frame
  plus a ~0.1 ms prep. Expected 1.5-3 ms of CPU thread time per frame on grassy levels.
- GPU: -40% vertex ALU per grass vertex (2.8), per-instance frustum culling in cascades and spot
  lights (the spot pass is the big one on grassy stations), no change to pixel work. Expected grass
  GPU time -15..30%, frame +2..6% on `foliage` at 1440p Extreme; indoor unchanged.
- Ladder: +15..25% reach at equal cost (Extreme 160 -> ~185-190 m) unless pixel cost or the
  decompress budget stops it earlier; the measurement decides.

## 11. Risks
- **Parity**: FXC vs DXC rounding can flip the alpha test on a few edge pixels; bounded by 9.3
  thresholds; if exceeded, compare DXC `-Gis`/precise variants before touching maths.
- **cbuffer slot order** under DXIL reflection (2.5): handled by `BindPoint`; an unhandled case shows
  as black/garbage grass in the A/B at once.
- **Recording before upload**: sun lists are recorded before `Render`; correctness relies on queue
  order (prep recorded before any grass list executes). A/B cascade checks cover it.
- **Pool/ring exhaustion**: hybrid path keeps grass on screen; stats and the sprint run size it.
- **Decompress budget** (7 slots/frame) limits reach when moving fast, independent of mesh shaders;
  the ladder rule includes it; raising the budget in proportion to `dm_cache_line` is the lever.
- **CPU memory** grows with radius: items 88 B + rows 64 B per tuft (~+60 MB at 190 m Extreme).
- **Driver differences** (MS output sizes, payload): variants in 3.10; fallback per object.
- **Modded grass shaders**: detected (3.7), vertex path keeps the mod's look.
- **Depends on infra** steps 1, 4, 5, 7; if one slips, its part is done here in the same shape.

## 12. Implementation order
1. 2.7 fix (`cache_Task`); stand `render_check` on `foliage`, R4 and R5.
2. Infra prerequisites if not landed: DXC vendoring + caps (`ms`, `-dx12_no_ms`), `Reflect` adapter +
   engine DXC route, stream PSOs in the library, DXIL shaders through the D3D11 path.
3. Layer: mesh/amplification kinds, mesh root signature, stream PSO branch, list6, `DispatchMesh`,
   draw refusal, stat; layer scenarios 9.1.
4. Engine: `#ms/#as/#dxil` names, DXIL `create_shader`, `BindPoint` slots, `CBackend::RenderMesh`.
5. Shaders: `da_detail_common.h` + VS refactor (DXBC identity 9.2), `da_detail_mesh.h`, MS, AS;
   Test-Shaders and linkage checker with DXC.
6. Blender elements 2/3; availability checks; `* [grass-mesh]` log.
7. Detail manager: models to SRV buffers, pool/pages/LRU/generations, upload ring, calc-task step,
   `MeshFramePrepare`, `ms_Render`, hybrid, `parallel_details` skip, `r__grass_mesh`, `r__grass_stats`,
   verdict cache, `Grass` GPU timer.
8. `r__dbg_grass_ab` + `grass_ab.lua`; iterate until 9.3 parity passes on all three runs.
9. Perf A/B 9.4; tuning variants 3.10 (keep only faster and A/B-identical ones).
10. Hi-Z phase 3.11 (merge only if it meets its gate) - removed after the measurement.
11. Ladder: `grass_run.lua`, radius (then density) sweep per 3.9 on `foliage` and `fogqa` at 1440p
    Extreme and 1080p High, short runs; fill the mesh columns, `(mesh)` log, hint strings if changed;
    rerun 9.3 and 9.4 at the final rungs, fallback run with `-dx12_no_ms`.
12. Docs: DX12_RENDERER.md section (mesh grass, numbers), TEST_MATRIX.md rows.

## 13. As built (28.09.2026) and what the final test phase decides

Code written under PLAN 0.1b (no stand runs, no layer tests in this phase); the gates of 9.x run once
in the final test phase.

### 13.1 Engine path
- **Row list instead of per-part chunks.** The first build dispatched one amplification group per
  part-chunk (`grass_chunk {base, count}`, up to 32 rows of one part). On `foliage` a part holds 3.8
  rows on average (25405 parts, 96272 rows, 25411 chunks), so the amplification groups ran 1/8 full,
  and that build drew visibly sparse grass (run `mesh-m6-ab-foliage`, shot `on1`: the low heather
  carpet mostly gone; cascade A/B 11-37 % of the texels different) and cost more GPU time than the
  vertex path (`grass` 1.3-1.6 ms vs 0.9 ms). The cause was not found by reading (shaders, constant
  plumbing, layer bindings and the CPU bookkeeping all check out; the DXIL of every stage was
  inspected). The path is now built around a per-frame **row list**: the calc task writes, part after
  part in draw order, the pool row of every resident tuft (`u32`, `grass_index`, t3 of the AS) and
  uploads it with the changed rows; a dispatch covers a run of list entries (`grass_dispatch.x` first
  entry, `.y` count), an amplification group takes 32 consecutive entries whatever part they belong
  to, and the payload carries the surviving pool rows (`grass_payload {count, rows[32]}`) instead of
  a base and packed offsets. Full groups, no chunk records, no per-part base.
- **Stray amplification shader (gd's 15:50 report).** Several backends share the immediate context
  (the local lights' `alloc_context(false)` contexts, RCache): a backend starting from a null cache
  never unbinds a geometry shader, so the AS the grass left in the GS slot rode along with every
  later vertex pipeline and the driver refused them (57 refused pipelines, draws skipped). Each mesh
  dump now ends by binding the vertex element of its last object (`set_Element(E[lod])`), which
  clears the GS slot through the backend's own cache (`set_GS` is private).
- **AS planes = the raster volume (final-phase review mesh-cr-1).** The AS used to cull each tuft
  against the pass's culling frustum (`ViewBase` in the G-buffer, `dsgraph.o.view_frustum` in the
  passes that hand one in). In the sun cascades that is the caster volume of
  `compute_caster_model_fixed`, clearly narrower than the square ortho map drawn with
  `X.D[cascade].combine`, while the vertex path tests it only per 2x2 m slot and so draws every tuft
  of a straddling slot into the map: the AS dropped tufts the vertex path still rasterized (a band up
  to a slot wide along the volume's edge, far above 9.3's 0.05 % cascade budget). In the G-buffer
  `ViewBase` is unjittered while the grass is drawn with `da_jitter_project` (half-pixel edges under
  TAA). The planes now come from `CFrustum::CreateFromMatrix(cmd_list.xforms.m_wvp, FRUSTUM_P_ALL)`
  (world is identity in `CDetailManager::Render`; the mesh shader writes `mul(m_WVP, pos)`; depth
  clip is always on; the matrix's GL-style near plane is looser than D3D's, so it only keeps more),
  exact for every pass. The per-slot `IsPartVisible` test stays on the pass frustum, which both paths
  share. A cascade A/B failure in 13.3 step 3 is therefore no longer explained by the plane set.
- **Smaller review fixes (mesh-cr-2..4).** The verdict signature hashes only stable adapter fields
  (3.7; `AdapterLuid` changed it on every boot, so a stored refusal was ignored after a reboot);
  `ms_Stats` (`r__grass_stats`) waits for the calc task before reading its counters, pool and
  backlog; the one raw-pointer test against `nullptr` in `RenderMeshDump` uses the bool conversion.
- **`r__grass_mesh` default 0** until the final phase passes 9.3 and 9.4, then 1 (the same rule gd
  uses for `r__gpu_driven`). Flipped to 1 by the Perf phase on the look-neutral verdict. The preset replay picks the mesh columns only when mesh grass will run:
  `da_grass_mesh_expected()` = `r__grass_mesh` on and `Available()` (final-phase fix F2: it ignored the
  switch, so a default-0 session drew the vertex path at the mesh rung and logged `(mesh)`); the
  blender keeps compiling the mesh elements on `Available()` alone, for a runtime switch.
- **Row check** (`r__grass_stats`): besides the counters, the next frame's pool and row list are
  copied to staging after the frame's copies, together with the rows the calc meant to upload, and
  compared three frames later: `* [grass-mesh] row check (frame N): P parts, R rows compared, L list
  entries wrong, W pool rows wrong -> PASS|FAIL`. It separates "the GPU holds the wrong rows" from
  "the shaders draw the right rows wrong" in one run.
- **A/B tool**: a target the tool cannot copy is now named with the failing step and HRESULT, and the
  job goes on with the other targets (the first run lost the whole G-buffer comparison to one target:
  `gbuffer: its targets could not be captured`).

### 13.2 Variants: decided by the final one-pass run
Measured together with the GD phases 2/3 and the copy queue (2560x1440 Extreme TAA, `foliage`:
`final-onepass-foliage` with the defaults against `final-onepass-variants-foliage` with
`r__grass_mesh_hiz 1`, `r__grass_mesh_depth 1`, `r__gpu_driven_hiz 1`, `r__gpu_driven_main 1` and
`-dx12_copy_queue`; last `[gpu]` lines): frame 20.30 -> 20.41 ms, `scene` 2.07 -> 2.26, `gd` 0.12 -> 0.34,
`grass` 1.89 -> 1.94, `sun` 5.95 -> 5.77, FPS 49.0 -> 47.8; no error line.
- `r__grass_mesh_hiz` - **Hi-Z (3.11): removed.** `da_grass_hzb` built a max-depth pyramid of the depth
  drawn so far before the G-buffer grass and the amplification shader dropped rows hidden behind it
  (exact with respect to LESSEQUAL). `grass` (the pyramid inside the `Grass` timer, so net) went up by
  0.05 ms instead of down by the gate's 0.15 ms: the switch, the pyramid, `da_grass_hzb.cs`, the AS test
  and its constants are gone.
- `r__grass_mesh_depth` - **depth-only shadow elements (3.10): the shadow path, switch dropped.** Detail
  elements 4/5 (`deffer_detail_mesh.ms` under `DETAIL_DEPTH` + `deffer_detail_mesh_depth.ps`) draw the
  grass in every shadow pass (sun cascades, local-light maps) wherever they compiled; elements 2/3 stay
  the fallback. Look-neutral by construction, from the code: the same amplification shader and rows
  (same tufts dropped), the same mesh shader maths for the position - the wave elements keep
  `detail_wind_at` and `detail_bent_pos`, the still ones `detail_motor_pos`, so the shadows keep swaying
  (the project rule for plant shadows) - the same `m_WVP`, texture coordinates, `s_base`/`smp_base` and
  `clip(D.w - def_aref)` as `deffer_base_aref_flat.ps` (MSAA, whose alpha-to-coverage passes differ, has
  no mesh grass), and the same depth, stencil and cull state. Dropped are only the outputs no shadow pass
  binds: normal, view position, hemi tint and the previous-frame position of the motion vectors. `sun`
  5.95 -> 5.77 ms in the same run.
- Not built, with reasons: the 128-thread mesh group (3.10) is meant for AMD/Intel and only an NVIDIA
  GPU is available, so its "faster" gate cannot be met here; per-tuft terms in the amplification
  payload (3.10) remove no work - the mesh shader already computes each tuft's terms once (threads
  `< n`), the variant would only move them.
- Not built, with reasons: the 128-thread mesh group (3.10) is meant for AMD/Intel and only an NVIDIA
  GPU is available, so its "faster" gate cannot be met here; per-tuft terms in the amplification
  payload (3.10) remove no work - the mesh shader already computes each tuft's terms once (threads
  `< n`), the variant would only move them.

### 13.3 What the final test phase runs and decides (budget of PLAN 0.1a)
1. Offline (already green in this phase): `Test-Shaders.ps1 -Dxc` (and `-Sm 6_5`) on
   `deffer_detail_mesh.ms`, `deffer_detail_cull.as`, `deffer_detail_mesh_depth.ps`;
   FXC on the `.ps`; `Check-ShaderLinkage.py --compiler dxc --filter deffer_detail`: 12 pipelines,
   0 refused (also `--sm 6_5`).
2. Layer tests at the floor (M3 scenarios unchanged).
3. R5 `grass_ab` on `foliage` (`-PreCommands r__aa st_aa_off`, `qa_grass_shots`, `qa_grass_ab`): the row
   check PASS, shots `on1` vs `off1..off3` equal (Compare-Frames, noise floor), `[grass-ab]` G-buffer
   and cascades within 9.3. If the row check fails, the data path is wrong; if it passes and the
   frames differ, the shaders/constants are. Since the variants were decided (13.2) the cascade A/B
   runs the depth-only shadow elements by default.
4. Perf 9.4 (`qa_grass_gpu`): mesh must be >= vertex - 0.5 % frame time, else the mesh path is
   removed (PLAN 7); `r__grass_mesh` becomes 1 when 3 and 4 pass. The variants were measured in one
   pass instead (13.2).
5. M10: `grass_run.lua` sprint (backlog 0 within 2 s, no vertex-path parts after 2 s) and the radius
   sweep of 3.9 for the mesh columns (`[dx12:mesh] preset` block in `xrRender_console.cpp`); the hint
   sentence of 7 only if a column changes.
6. Fallbacks: `-dx12_no_ms` and `r__dxc 0` give `* [grass-mesh] off: ...` and the base columns; a
   second level loaded in-game with mesh grass on (gd saw the engine exit after `load fogqa` in a run
   that also carried the stray AS; not reproduced here).

## Final test phase, functional part (28.09.2026; `_work\dx12-features-20260928\final\stand.md`)

- `r__grass_mesh 1` with GD, async, DirectStorage and DXC (`final-on-foliage`, `final-on-pedro`): `[grass-mesh] on: 32
  objects (0 by the vertex path, 32 with shadow variants) ... hi-z ready`, 150-170k mesh dispatches per 300 frames,
  no `refused`; foliage against the same tree's DXC-only control (vertex grass): 99.91 % within, 0 clusters > 16 px,
  56.4 dB, full frame 0.84 % > 2/255 - inside the Gate-0 references' own spread (0.76-1.54 %). Mesh grass is
  look-neutral.
- Level change with mesh grass on and `-dxdebug` (`final-robust-level-c`): `[grass-mesh] on: 37 objects` on
  k00_marsh, no D3D12 error line. Sway with RT shadows on: dxr.md 9.2.
- The default stayed 0 until the Perf phase (flipped to 1 there, below).

## Final test phase, performance (Perf phase, 28.09.2026)

At the user's request the Perf phase ran one measurement pass instead of the 9.4 batch (PLAN 8.4); the M10
radius sweep was not run, the Hi-Z and the depth variant were measured in one pass against the defaults (13.2).
- `final-onepass-foliage` (2560x1440 Extreme, TAA, every feature on): G-buffer mesh grass `grass` **1.86 ms** GPU,
  `grass_cpu` 2.5 ms (render thread), `grass_mesh=1`; `[grass-mesh] on: 32 objects (0 by the vertex path, 32 with
  shadow variants) ... pool 48 MB`.
- Render-thread grass time on the stand (1920x1080 Extreme, foliage, `r__gpu_log 100`): 8.6 ms with the vertex path
  (`final-dxconly-foliage`) against 1.8 ms with mesh grass (`final-on-foliage`); the same two runs gave 68.9 -> 81.4
  FPS with GD, DirectStorage and async also on (not mesh grass alone).
- Decision: **`r__grass_mesh` defaults to 1** (look-neutral PASS of the final stand); the mesh preset columns
  equal the base columns, so the preset hint gains no sentence.
- 29.09, a player's save (campfire at night on k00_marsh, 2560x1440 Extreme TAA, RTX 3070 Laptop; GPU-bound):
  GD off and mesh grass on 49.1 FPS, both off 52.0 FPS (`grass` 1.03 -> 0.64 ms, `grass_cpu` 3.7-4.1 -> 7.0-7.4
  ms): mesh grass saves CPU time at a GPU price, and it draws the G-buffer grass on the render thread where the
  vertex path uses a worker. **`r__grass_mesh` defaults to -1 (auto) since then**, one decision with
  `r__gpu_driven` (gpudriven.md 3.13).
- Variants (13.2): the G-buffer Hi-Z **removed** (`grass` 1.89 -> 1.94 ms with it, no saving); the depth-only
  shadow elements **kept as the only mesh shadow path**, `r__grass_mesh_depth` dropped (same tufts, sway and
  alpha test as the colour elements by construction; `sun` 5.95 -> 5.77 ms in that run).
