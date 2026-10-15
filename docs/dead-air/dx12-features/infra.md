# Native D3D12 infrastructure (xrD3D12 + xrRender_R5)

Status: implemented (28.09.2026). Milestones I0 (API freeze), I1 (capabilities, DXC service, DXIL through
the D3D11 path), I2 (native sections v2), I3 (descriptors, bindless, memory, native pipelines), I4
(D3D11-path extensions: buffer views, indirect draws, acceleration structure SRVs), I5 (async
compute: queue, jobs, compute recording) and I6 (self-test) of `PLAN.md` 3.2 done; the final stand passed
(layer tests 81/0, no D3D12 error with everything on under `-dxdebug`). I7 (copy queue for `InitUploader`) is
**on by default** since the final one-pass measurement (level.geom 256 -> 214 ms, level.geomx 58 -> 47 ms, the
early texture batch's create 611 -> 548 ms, 616 MB of creation-time uploads moved off the direct queue, no error
line); the opt-in `-dx12_copy_queue` is gone, `-dx12_no_copy_queue` keeps the uploads on the direct queue. See
"Implementation record" at the end. Foundation for
the DX12-only features (ray tracing, VRS, mesh shaders, GPU-driven draws, async compute, DirectStorage). Everything here lives in the xrD3D12 layer
(`src/Layers/xrRenderDX12`, linked into `xrRender_R5.dll` only) or behind `#ifdef XR_D3D12`;
`xrRender_R4.dll` gains only read-only `r__caps` keys.

## Integration amendments (PLAN.md)

Implemented by the `infra` lane, milestones I0-I7 of `PLAN.md` 3.2. These points supersede the text
below where they differ (ids refer to `PLAN.md` section 1):
- **C2/C3, DXC**: the compiler service is `dxc.md`'s. `d12_dxc.cpp` (this lane) implements
  `dxc.md` 3.1 in full: absolute-path loader, self-check PSO, SEH-guarded `CompileShader`,
  `Reflect(bytecode, size, REFIID, void**)` returning `ID3D12ShaderReflection` for DXBC and DXIL (no
  `ID3D11ShaderReflection` adapter), `Disassemble`, `IsDxil`. The `//grizzly:dxil` opt-in, the `#dxil`
  variants and `da_shader_dxc.cpp` are dropped: R5 compiles every engine shader with DXC in the
  session's mode and unifies mixed passes to DXBC twins (`dxc.md` 3.6). Native/self-test shaders go
  through `da_dxc_compile_native` (`dxc` lane), cache under `shaders_cache_grizzly/r5_dxil/<tag>/native/`.
  Headers come from the Windows SDK; `Externals/dxc` has no `inc/` (vendored by `core`, C1).
- **I1 order**: caps and the whole DXC service land together, with the layer's DXIL acceptance
  (`Shader::Reflect` via `Reflect`, `ProgramType` for DXIL incl. mesh/amplification kinds,
  `Shader::dxil`, ISG1, the format-mix guard), because `dxc`, `rt` and `mesh` wait for it.
- **C9**: 3.2 gains `RequireMany(context, resources, count, state)` and
  `UINT64 TextureWriteGeneration(ID3D11Device*)` (bumped by `UpdateSubresource`/`Copy*`/`Map` into
  non-RT/DS/UAV textures and by texture creation/destruction) for the RT material tables.
- **C10**: 3.5 gains buffer SRV/UAV views (raw `R32_TYPELESS` + `BUFFEREX_SRV_FLAG_RAW`, structured,
  typed; APPEND/COUNTER refused with a log line), committed tracked SRV/UAV buffers and
  `MISC_DRAWINDIRECT_ARGS`; `gpudriven.md`'s five layer scenarios live in `scenarios_views.cpp` here.
- **C11**: `list4/5/6` are queried once per pooled command list at creation (not per `BeginNative`),
  `ID3D12Device2/5` at `Device::Init`. The VRS context state is the `vrs` lane's `d12_vrs.cpp`,
  called through hooks this lane pre-places at I0; the I0 stub of `VrsNativeReset` already resets a
  non-default list to 1x1 so self-test part (f) works before the VRS lane lands (C31). Mesh
  (`d12_mesh.cpp`) and placed texture heaps (`d12_texheap.cpp`) get pre-placed hooks the same way.
- **C12**: `async.md` 3.3's layer part (compute recording of D3D11 lists, typed allocators/lists,
  `transitioned`, `AsyncJobFromCommandList`, read-shared lending, `StatAutoJoins`) is implemented
  here in I5 with the jobs.
- **C13**: no generic `Async` timer slot; compute-queue job times appear as `cq_march`, `cq_ripple`,
  `cq_gtao` in the `r__gpu_log` field registry (`core`).
- **C7/C28**: `rt` is reported by the `rt` lane's `da_rt_available()`; this lane owns `rt_tier`,
  `vrs_tier`, `vrs_tile`, `ms`, `sm`, `wave`, `fp16`, `eb`, `acq`. All keys are printed by one caps
  hook, `da_native_caps.cpp` (`core`), which also fills `CHW::native`.
- **C20**: scenario files `scenarios_native.cpp`, `scenarios_views.cpp`, `scenarios_async.cpp`,
  `scenarios_dxil.cpp`; every lane has its own file, all registered at I0.
- **C23**: the copy queue (step 9) is measured after DirectStorage landed, on top of it, and is
  merged default-on only if it gains; otherwise it is removed (no `-dx12_copy_queue` flag after the
  decision). Applied: it gained at load and is on by default; `DeviceCreateInfo::copyQueueUploads`
  defaults to true and the mask `-dx12_no_copy_queue` (like `-dx12_no_rebar`) turns it off (13, I7).
- `dx11HW.cpp`, `xrRender_console.cpp`, `da_upscaler.h`, `dx11GpuTimers.*`, `cmake/`, packaging and
  `DEPENDENCIES.md` edits listed below are made by `core` (PLAN 2.2); this lane owns
  `src/Layers/xrRenderDX12`, its CMake file, the layer test harness and `da_native_selftest.*`.

## 1. Goal

1. **Capabilities**: detect DXR tier, VRS tier/tile size/additional rates, mesh shader tier, highest
   shader model, wave ops and lane counts, native 16-bit ops, SM 6.6 heap indexing, enhanced barriers
   (reported only), execute-indirect tier; log them at device creation; export them as `r__caps` keys
   in both modules.
2. **Native recording on any context**: renderer code records `ID3D12GraphicsCommandList4/5/6`
   commands (DispatchRays, BuildRaytracingAccelerationStructure, RSSetShadingRate[Image],
   DispatchMesh, ExecuteIndirect) on the immediate context and on deferred contexts in the middle of
   a frame, with exact resource-state hand-off and cheap re-application of the layer's state.
3. **Second queue**: a layer-owned compute queue (plus the copy queue of creation-time uploads) with cross-queue
   fences, a documented fork/join API and state rules for shared resources.
4. **Helpers for native passes**: root signature and PSO/state-object creation with caching,
   shader-visible transient descriptors from the ring, a persistent bindless range, per-frame upload
   memory, native views of D3D11 objects, a DXC compile path with the engine's include system.

Non-goals: work graphs; enhanced barriers inside the layer (legacy tracking stays the single source
of truth); the D3D12 Agility SDK (not needed, see section 8, Windows 10).

## 2. Current code (what this builds on)

- **Public native API** `d12_api.h:69-87`: `NativeDevice`, `NativeQueue`, `NativeResource`,
  `BeginNative(immediate)`, `Require(immediate, resource, state)`, `EndNative(immediate)`,
  `Flush`. The header says "everything below is for the immediate context on the render thread".
- **Implementation** `d12_swapchain.cpp:793-850`: `BeginNative` = `Context::List()` +
  `FlushBarriers()` + `m_inNative`; `Require` = `RequireAll` on the whole resource + flush, untracked
  resources are ignored with a log line; `EndNative` = `Context::InvalidateApplied()`
  (`d12_context_ops.cpp:1552`): heaps, both root signatures, PSO, IA, OM, viewports, every stage's
  root arguments become dirty. It does **not** set `applied.uavWritten` (`d12_layer.h:1302`), so a
  native UAV write followed by an engine UAV access of the same resource in the same state gets no
  UAV barrier unless the engine's previous call happened to use UAVs.
- `NativeResource` returns `Resource::ActualNative()` (`d12_layer.h:701`), i.e. null for static
  buffers in DEFAULT pages and for dynamic buffers (`Buffer::Create`, `d12_objects.cpp:1343-1386`);
  there is no way to get their GPU address.
- **Users**: FSR 3/4 (`da_upscaler_fsr_dx12.cpp:158-201`), FSR FG (`da_framegen_fsr.cpp:350-394`),
  DLSS (`da_upscaler_dlss.cpp:190-247`), DLSS-G (`da_framegen_dlss.cpp:173-193`). All immediate,
  whole-resource `Require`, full `EndNative`, then `RCache.Invalidate()`
  (`r4_rendertarget_phase_upscale.cpp:138`, needed for the DX11 SDK path, redundant on R5).
- **Lists**: every context records DIRECT lists created as `ID3D12GraphicsCommandList`
  (`Context::OpenList`, `d12_context_ops.cpp:1339`); no QueryInterface to 4/5/6 anywhere.
- **State tracking**: immediate works on `Resource::states`; deferred contexts keep a local tracker
  whose first/last states per subresource are harvested into `CommandList::requirements`
  (`StateTracker::Require/Harvest`, `d12_context_ops.cpp:776-893`) and patched in by
  `ExecuteCommandList` (`:2510`, exact `RequireTracked` + `SetTracked`). So `Context::Require` on a
  deferred context already produces a correct hand-off; only the public API forbids it.
- **Submission**: `Context::Submit` (`:1392`) sends the queued lists (`m_queued`, `d12_layer.h:1363`)
  and the own list in one `ExecuteCommandLists`, one `fence.Signal` (`:1433`); `Present1`
  (`d12_swapchain.cpp:516-566`) submits and calls `Device::EndFrame` (`d12_device.cpp:1725`) which
  retires descriptor chunks, upload pages and allocators by frame fence.
- **Fence discipline**: creation-time uploads signal `Device::initFence` (`d12_layer.h:1446-1448`)
  because a value taken from `Device::fence` in the middle of a frame completes before the submission
  that carries work of objects the `DeferredReleaser` tagged with it. Any new mid-frame signal (the
  cross-queue ones) has the same hazard.
- **Descriptors**: `GpuDescriptorRing` (`d12_infra.cpp:1076-1210`) uses the entire 1M shader-visible
  CBV/SRV/UAV heap (`kDescriptorRingCapacity`, `d12_layer.h:73`) as a ring of per-frame chunks; there
  is no persistent range. `ApplyHeaps` (`d12_context_state.cpp:325`) binds ring + sampler heap.
- **Pipelines**: two fixed root signatures (`RootSignatures::Init`, `d12_infra.cpp:2302`: graphics 29
  parameters, compute 8, v1.1) and `PipelineCache` (graphics/compute PSOs through an
  `ID3D12PipelineLibrary`, keyed by content hashes). No stream PSOs, no state objects.
- **Caps**: `Device::Init` queries `D3D12_OPTIONS` (`d12_device.cpp:1493`) and `OPTIONS16` (`:1506`)
  and logs one device line (`:1515`). `ProbeAdapter` (`:1807`) checks FL 11_0 + binding tier 2; its
  verdict is cached by `xrRender_test_d3d12` (`r2_test_hw.cpp:249-296`), which runs at every start
  from `R5RendererModule::ObtainSupportedModes` (`xrRender_R5.cpp:32`) whatever renderer is chosen.
  `r__caps` = `CCC_RenderCaps::GetStatus` (`xrRender_console.cpp:833-856`) printing `UpscalerCaps`
  (`da_upscaler.h:74-89`) filled by hooks (`da_upscaler.cpp:111-150`); `TStatus` is `char[256]`
  (`xr_ioc_cmd.h:38`).
- **Indirect draws** are logged as unsupported (`d12_context_state.cpp:1276-1289`).
- **Shaders**: the engine compiles FXC SM 5.0 at runtime (`r4_shaders.cpp:653`, `includer` `:151`,
  cache `:600-642` validated by `getFileCrc32` over the source and its includes) and reflects with
  `D3DReflect` (`r4_shaders.cpp:35`); the layer reflects with `D3DReflect` too
  (`Shader::Reflect`, `d12_objects.cpp:1991`) and parses input signatures by hand, ISGN only
  (`d12_device.cpp:180`).
- **GPU timers**: `dx11GpuTimers` (immediate context only, `dx11GpuTimers.h:17-19`).
- **Menu**: `ui_mm_opt_video.script:33-40` parses `r__caps`; frame generation shows the pattern the
  RTX group will follow (row shown, entries disabled, `off_hint` for DX11/unsupported, `:97-127`).

### Probe on this machine

Offline probes written for this design (session scratchpad `native_infra_probe/probe.cpp` and
`mix.cpp`, not in the tree; their checks become layer scenarios 1, 11 and 12).

In-box runtime `D3D12Core.dll 6.2.26100.9278`, device SDK version **616** (same as the SDK header),
RTX 3070 Laptop:

| Cap | Value |
|---|---|
| Binding / heap tier | 3 / 2 |
| Raytracing tier | 1.2 (`12`) |
| VRS | tier 2, tile 16, additional rates, per-primitive with VP index, sum combiner |
| Mesh shaders | tier 1, per-primitive shading rate |
| Highest SM | 6.8 (6.9 query refused) |
| Waves | 32..32 lanes, 5120 total; int64; native 16-bit ops |
| Enhanced barriers | yes; work graphs 1.0; execute indirect 1.1; root signature 1.2 |
| Queues | direct, compute, copy all timestamp at 1 GHz; copy-queue timestamps supported |
| Interfaces | Device2/5/8/10/12, GraphicsCommandList4/5/6/7 |

End-to-end checks, all passing with both DXC 1.8.2502 (Windows SDK) and DXC 1.9.2607
(`_work/sdk-downloads/dxc-1.9.2607`): `cs_6_6` with `ResourceDescriptorHeap[0]` and
`WaveGetLaneCount()` dispatched on a **compute queue** between two direct-queue fence points
(result 32 lanes); one-triangle BLAS/TLAS from an UPLOAD vertex buffer + inline `RayQuery` `cs_6_5`
(hit t = 1.000, miss = sentinel); `lib_6_3` DXR state object (30-60 ms to create); `ms_6_5`+`ps_6_5`
stream PSO. Also measured: a PSO that mixes FXC DXBC and DXC DXIL stages is **refused**
("mixes shader model 6.0 or above with 5.1 or below"); `D3DReflect` on DXIL fails
(`E_INVALIDARG`); `IDxcUtils::CreateReflection` returns `ID3D12ShaderReflection` with register
spaces. `ID3D12SDKConfiguration1::CreateDeviceFactory` exists on this OS.

## 3. Design

### 3.1 Capabilities

`d12_api.h` gets a plain struct; `d12_infra.cpp` gets `QueryNativeCaps(ID3D12Device*, NativeCaps&)`
used by both `Device::Init` and `ProbeAdapter`:

```cpp
struct NativeCaps
{
    UINT raytracingTier = 0;        // D3D12_RAYTRACING_TIER: 0, 10, 11, 12
    UINT vrsTier = 0;               // 0, 1, 2
    UINT vrsTileSize = 0;           // tier 2: 8, 16 or 32
    bool vrsAdditionalRates = false, vrsPerPrimitiveWithViewportIndex = false, vrsSumCombiner = false;
    UINT meshShaderTier = 0;        // 0 or 10
    bool meshPerPrimitiveRate = false;
    UINT shaderModel = 0x51;        // device: highest D3D_SHADER_MODEL (0x60..0x69), 0x51 = none of SM 6
    UINT dxcShaderModel = 0;        // what the loaded DXC targets (0x69 for 1.9), 0 = no DXC
    bool waveOps = false;
    UINT waveLanesMin = 0, waveLanesMax = 0;
    bool native16Bit = false, int64Ops = false, barycentrics = false;
    bool heapIndexing = false;      // SM 6.6 ResourceDescriptorHeap: shaderModel >= 6.6 and DXC >= 6.6
    bool enhancedBarriers = false;  // reported only
    UINT executeIndirectTier = 0, workGraphsTier = 0, rootSignatureVersion = 0;
    bool asyncCompute = false;      // jobs really run on the compute queue
    bool copyQueueTimestamps = false;
    UINT runtimeSdkVersion = 0;     // ID3D12DeviceConfiguration::GetDesc().SDKVersion, 0 = older runtime
    UINT bindlessCapacity = 0;      // persistent descriptors (3.4)
};
const NativeCaps* GetNativeCaps(ID3D11Device* device); // null for non-layer devices
```

- Queries: `OPTIONS1` (waves, int64), `OPTIONS3` (barycentrics, copy timestamps), `OPTIONS4`
  (16-bit), `OPTIONS5` (raytracing), `OPTIONS6` (VRS), `OPTIONS7` (mesh), `OPTIONS10` (sum combiner,
  per-primitive MS rate), `OPTIONS12` (enhanced barriers), `OPTIONS21` (execute indirect / work
  graphs, may fail on older runtimes -> 0), `SHADER_MODEL` walked down from `D3D_HIGHEST_SHADER_MODEL`
  (a too-new request returns `E_INVALIDARG`), `ROOT_SIGNATURE` from 1.2 down.
- `DeviceCreateInfo::disabledFeatures` (bit mask, set in `dx11HW.cpp:210-226` from `-dx12_no_rt`,
  `-dx12_no_vrs`, `-dx12_no_ms`, `-dx12_no_async`, `-dx12_no_sm6`) zeroes the matching caps after the
  query, so fallbacks are testable on this GPU. The mask also applies to `rt_tier` (a masked GPU looks
  like one without DXR), and the R4 reader of the probe cache honours the same switches.
- Log at device creation, after the existing device line:
  `* native: raytracing 1.2, VRS 2 (tile 16, extra rates), mesh shaders 1, SM 6.8 (DXC 6.9), waves 32-32, 16-bit ops, int64, enhanced barriers (unused), execute indirect 1.1, work graphs 1.0 (unused), root signature 1.2, runtime SDK 616`
  and `* native: compute queue on (async jobs, made by the first), copy queue timestamps yes, bindless 65536 + ring 934464 descriptors`
  (the queue itself logs `* async compute: compute queue made for the first job` when a job makes it).
- **Probe cache for DX11**: `ProbeAdapter` gets an overload filling `NativeCaps` from its throw-away
  device. `xrRender_test_d3d12` writes a second file `render-caps-d3d12.cache` (magic, version,
  signature from `GetD3D12Cache`, `raytracingTier`, `vrsTier`, `meshShaderTier`, `shaderModel`) whenever
  it probes. A reader compiled into **both** modules (`r2_test_hw.cpp`, outside `XR_D3D12`; the
  signature needs `ProductVersion.h` there too) gives R4 the adapter's tiers, so the menu can say
  "switch to DirectX 12" instead of "not supported".
- **r__caps** (existing plumbing): `UpscalerCaps` (`da_upscaler.h:74`) gets the fields below, filled
  by a new hook in `da_native_caps.cpp` (both modules) and printed by `CCC_RenderCaps::GetStatus`.
  The full string is about 155 characters, under `TStatus` (256); `GetStatus` logs once if
  `xr_sprintf` would truncate.

| Key | R5 | R4 |
|---|---|---|
| `rt` | RT usable this session (tier >= 11, DXC service up, RT device init ok, no MSAA, no `-dx12_no_rt`), reported by the `rt` lane's `da_rt_available()` (PLAN C7) | 0 |
| `rt_tier` | device tier (0/10/11/12) | adapter tier from `render-caps-d3d12.cache`, 0 unknown |
| `vrs_tier` | VRS tier (0/1/2) | 0 |
| `vrs_tile` | tier-2 tile size, else 0 | 0 |
| `vrs` | not set here: the VRS feature's hook (`vrs.md` 3.4) sets 1 when tier 2 and its shaders are present | 0 |
| `ms` | 1 when mesh shader tier 1 and DXC present | 0 |
| `sm` | `min(shaderModel, dxcShaderModel)` as 60..69; 50 without DXC | 50 |
| `wave` | `waveLanesMax` when wave ops and `sm >= 60`, else 0 | 0 |
| `fp16` | native 16-bit ops and `sm >= 62` | 0 |
| `eb` | enhanced barriers (information) | 0 |
| `acq` | async compute jobs run on the compute queue | 0 |
| `dstorage` | left 0 here; the DirectStorage feature's caps hook sets it | same |

- Engine-side copy: `CHW` (`dx11HW.h`) gets `CHWNativeCaps native{}` (zero on R4), filled after
  `xrd3d12::CreateDevice` under `XR_D3D12`, so feature code reads `HW.native.raytracingTier` without
  module checks. Feature code that *calls* the layer stays in `#ifdef XR_D3D12` blocks (the symbols
  exist only in R5), as `da_upscaler_fsr_dx12.cpp` does.

### 3.2 Native recording on any context

The existing three functions keep their signatures (FSR/DLSS code unchanged; they become wrappers).
New API, valid on the immediate context (render thread) and on a deferred context (its recording
thread). Between `BeginNative` and `EndNative` no D3D11 call may be made on that context, except the
helpers listed here.

```cpp
enum NativeTouched : UINT   // what the native code changed on the list
{
    TouchedHeaps = 1 << 0,          // SetDescriptorHeaps with foreign heaps (SDKs)
    TouchedGraphicsRoot = 1 << 1,   // SetGraphicsRootSignature / graphics root arguments
    TouchedComputeRoot = 1 << 2,    // SetComputeRootSignature / compute root arguments (DispatchRays too)
    TouchedPipeline = 1 << 3,       // SetPipelineState / SetPipelineState1
    TouchedInputAssembler = 1 << 4,
    TouchedOutputMerger = 1 << 5,   // render targets, blend factor, stencil ref
    TouchedRasterizer = 1 << 6,     // viewports, scissors
    TouchedShadingRate = 1 << 7,    // RSSetShadingRate / RSSetShadingRateImage
    TouchedAll = 0xFF,
};
struct NativeScope
{
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList4* list4 = nullptr; // DXR; null when the runtime lacks it
    ID3D12GraphicsCommandList5* list5 = nullptr; // VRS
    ID3D12GraphicsCommandList6* list6 = nullptr; // DispatchMesh
    ID3D12DescriptorHeap* heap = nullptr;        // the layer's CBV/SRV/UAV heap, bound on `list`
    D3D12_GPU_DESCRIPTOR_HANDLE bindless{};      // GPU handle of bindless index 0 (3.4)
};
bool BeginNative(ID3D11DeviceContext* context, NativeScope& scope);
void EndNative(ID3D11DeviceContext* context, UINT touched);
void Require(ID3D11DeviceContext* context, ID3D11Resource* resource, D3D12_RESOURCE_STATES state);
void RequireView(ID3D11DeviceContext* context, ID3D11View* view, D3D12_RESOURCE_STATES state); // the view's subresources only
void UavBarrier(ID3D11DeviceContext* context, ID3D11Resource* resource); // null = global
```

- `BeginNative`: `List()`, `ApplyHeaps` (binds ring + sampler heap if the list has not got them;
  cost 0 when bound), `FlushBarriers()`, QueryInterface of 4/5/6 (released in `EndNative`; about
  100 ns, no caching across pooled lists), `m_inNative = true`. Returns false on a removed device.
- `Require`/`RequireView` go through `Context::Require` (`d12_context_ops.cpp:1497`): on the immediate
  context they transition `Resource::states`; on a deferred context the first requirement becomes the
  list's entry state and `ExecuteCommandList` patches it in - no new mechanism. Barriers are flushed
  immediately (native code records right after).
- Untracked buffers (static pages, dynamic versions) need no `Require`: pages rely on COMMON
  promotion (`d12_layer.h:364-365`), upload memory is always GENERIC_READ. Both are valid inputs of
  `BuildRaytracingAccelerationStructure` (NON_PIXEL_SHADER_RESOURCE by promotion; the probe built a
  BLAS from an UPLOAD buffer). The existing log line stays for textures only.
- `EndNative(touched)` maps flags onto `Context::Applied` instead of the blanket `InvalidateApplied`:
  Heaps -> `heapsDirty` (implies everything, as today); GraphicsRoot -> `rootSignatureGraphics=false`
  + `ResetStageRoot` of VS..PS + `psUavDirty`; ComputeRoot -> `rootSignatureCompute=false` +
  `ResetStageRoot(CS)` + `csUavDirty`; Pipeline -> `pso=nullptr, psoHash=0, psoDirty`; IA ->
  `vbDirty/ibDirty/topologyDirty`; OM -> `rtDirty/stencilRefDirty/blendFactorDirty`; Rasterizer ->
  `viewportDirty/scissorDirty`; ShadingRate -> the VRS dirty flag (3.5, `vrs.md` 3.4). It **always** sets
  `applied.uavWritten = true` (fixes the missing UAV barrier noted in section 2). A DXR dispatch in
  the combine then re-applies one compute root + PSO instead of heaps + both roots + every table.
- On R5 the engine's `RCache` needs no `Invalidate()` after a native section: native code never
  changes D3D11-level state, the layer re-applies its own. (Upscaler code keeps it for the DX11 path.)
- **Views of D3D11 objects**:
  - `D3D12_CPU_DESCRIPTOR_HANDLE NativeDescriptor(ID3D11View*)` - the view's CPU descriptor
    (`View::Descriptor()`, back-buffer RTVs resolve to the current buffer), for `CopyDescriptors`
    into ring tables or bindless slots.
  - `bool NativeBufferRange(ID3D11Buffer*, NativeBuffer&)` - `{address, size, resource, offset,
    tracked}` for page ranges, the *current* dynamic version (valid for work recorded now; a later
    DISCARD renames, the old version lives until the GPU is done) and tracked buffers. This is how
    native passes bind the engine's constant buffers (wind, `m_WVP`) as root CBVs and read engine
    vertex/index buffers for BLAS builds.

### 3.3 Second queue: async compute (and copy)

**Objects** (`d12_async.cpp`, new): `Device::computeQueue` (`D3D12_COMMAND_LIST_TYPE_COMPUTE`,
created with the plain device - Streamline only needs the present queue, which stays the hooked
direct queue), `Device::crossFence` (direct -> compute points) and `Device::computeFence`
(compute -> direct points), both separate `FenceTimeline`s: a signal on `Device::fence` in the middle
of a submission would hand the `DeferredReleaser` a value that completes before later work of the
same frame (the `initFence` lesson, `d12_layer.h:1446`).

**API**

```cpp
class AsyncJob;
// Render thread, like every job call: begin, record, fork and join within one frame (a job forked in a
// later frame is dropped, logged once). scope.list is a COMPUTE list (a DIRECT one when async is off)
// with the layer heap bound.
AsyncJob* BeginAsyncCompute(ID3D11Device* device, const char* name, NativeScope& scope);
// Tracked resources the job touches: the state it needs on entry (compute-legal) and leaves them in.
// entry == exit in a read state = read-only: no barrier on it and no write inside the job.
void AsyncUse(AsyncJob* job, ID3D11Resource* resource, D3D12_RESOURCE_STATES entry, D3D12_RESOURCE_STATES exit);
DescriptorSpan AsyncDescriptors(AsyncJob* job, UINT count);
UploadSpan AsyncUpload(AsyncJob* job, UINT64 size, UINT64 alignment);
void EndAsyncCompute(AsyncJob* job);                                   // closes the list
// Immediate context, render thread. Fork: the job starts after everything recorded so far.
void ForkAsyncCompute(ID3D11DeviceContext* immediate, AsyncJob* job);
// Join: work recorded from here on waits for the job; its resources take their exit states.
void JoinAsyncCompute(ID3D11DeviceContext* immediate, AsyncJob* job); // the job is recycled after it
float AsyncComputeMs(ID3D11Device* device, const char* name);         // last resolved GPU time
```

**Mechanics** (keeps one queue submission per frame):
- A job owns a recorder from a pool: its allocator ring by frame slot (like `Context::m_allocators`),
  a `DescriptorAllocator` on the ring, an `UploadAllocator`, two timestamp slots in a compute-queue
  query heap resolved at the end of its list.
- `Fork`: `Require(entry)` for every `AsyncUse` resource on the immediate list, `FlushBarriers`,
  `CloseOwn` into `m_queued`, then a **Fork marker** (`QueuedList` gets `kind` + `job`). The
  resources are marked `lentTo = job` (immediate-owned field on `Resource`). `EndAsyncCompute` must
  have run (else: log once and execute the job in order on the direct queue).
- `Join`: `CloseOwn`, **Join marker**, `SetTracked(exit)` for the lent resources, `lentTo = null`.
- `Submit` walks `m_queued`: lists accumulate into a batch; at a Fork marker the batch executes,
  `d = crossFence.Signal(queue)`, `computeQueue->Wait(crossFence, d)`, the job's list executes on the
  compute queue, `job->done = computeFence.Signal(computeQueue)`; at a Join marker the batch executes
  and `queue->Wait(computeFence, job->done)`. Then the own list, `extra`, and the frame signal on
  `Device::fence` as today.
- **Invariant: every fork is joined before the frame's final signal.** `Present1` joins any open job
  first (log once "async job <name> not joined, joined at Present"). By transitivity the frame fence
  then covers the compute work, so descriptor chunks, upload pages, allocators and the
  `DeferredReleaser` need no knowledge of the second queue. `Device::WaitIdle` also waits for
  `computeFence`. The emergency frame end of `Submit` (upload memory past its budget without a Present)
  waits while a job is recording, unforked or forked and unjoined (`Device::AsyncJobsPending`): that
  submission's fence would not cover the job's memory.
- **One frame, render thread**: a job's descriptors and upload memory are tagged with the frame it was begun
  in. Jobs are begun, recorded, forked and joined on the render thread within one frame; a job forked in a
  later frame is dropped (logged once) and counts as joined, so the caller's join or the frame boundary
  recycles it.
- **Guard**: `RequireTracked` on a lent resource (immediate draw, `ExecuteCommandList` requirements)
  inserts the join right there (`CloseOwn`, Join marker) and logs once - a misuse costs overlap,
  never correctness. `ExecuteCommandList` checks every requirement's loan before any entry transition: a
  list that transitions a read-shared resource joins (its barriers would change it under the job), and the
  entry transitions follow the last join, so buffers decayed there get theirs in the same
  `ExecuteCommandLists` as the list. A compute recording handed to `ExecuteCommandList` runs as a job joined
  at once (logged once); a fork or join inside a native section is ignored with a log line (it would close
  the list native code records into).
- **Fallback** (`-dx12_no_async`, compute queue creation failed): `BeginAsyncCompute` hands out a
  DIRECT list; Fork queues it in order like a deferred list; Join is a no-op except the state
  bookkeeping. Same results, no overlap, `acq=0`.

**State rules for resources shared across queues**

| Resource | Rule |
|---|---|
| Tracked textures | Hand-off only through `AsyncUse`: entry/exit must be compute-legal (`NON_PIXEL_SHADER_RESOURCE`, `UNORDERED_ACCESS`, `COPY_SOURCE/DEST`, `COMMON`). The direct queue transitions into `entry` before the fork, the job's own barriers leave it in `exit`, the direct queue adopts `exit` at the join. Depth read by compute: entry `NON_PIXEL_SHADER_RESOURCE`; the direct queue binds it as a DSV again only after the join. |
| Read-only sharing | A texture both queues only read stays in a read state that contains `NON_PIXEL_SHADER_RESOURCE` (declare entry = exit = that state; the layer lends it read-shared in `entry | PIXEL | NON_PIXEL`). The job then records no barrier on it and never writes it. A job that writes it, even one that restores the read state, declares the write state it leaves it in as `exit` and records no barrier back. |
| Tracked buffers (UAV buffers) | Buffers decay to COMMON at the end of every `ExecuteCommandLists` on either queue; declare entry = exit = `COMMON` and let each queue promote. UAV ordering inside a queue still needs UAV barriers. |
| Page / dynamic buffers | Read-only on both queues (COMMON promotion / GENERIC_READ). Never written. |
| Acceleration structures | Created in `RAYTRACING_ACCELERATION_STRUCTURE`, never transitioned; UAV barriers only. BLAS/TLAS builds and refits may run as async jobs. |
| Graphics-only states | `PIXEL_SHADER_RESOURCE`, `RENDER_TARGET`, `DEPTH_*`, `SHADING_RATE_SOURCE` never appear in a job's barriers; the direct queue converts before the fork. |

**Copy queue** (section 12, step 9): `Device::copyQueue` + `copyFence` behind a second `InitUploader`
(`Device::initCopy`, COPY allocators and list). Textures uploaded there are created in `COMMON` (the copy
queue accepts no other state, debug layer #1425) and decay back to it after the copy, so they are tracked as
COMMON; the first `Require` issues COMMON -> shader read. Depth-stencil textures with initial data stay on
the direct uploader. Each copy submission is followed, still under the uploader's lock, by
`queue->Wait(copyFence, value)` on the direct queue: every direct submission after it (the context's next
`Submit` flushes first and blocks on that lock) sees the data, and the frame fence covers the copies, so
releases and placed-heap reuse stay safe. Merged default-on after the final one-pass measurement with
DirectStorage on (13, I7; PLAN C23); `-dx12_no_copy_queue` keeps every upload on the direct uploader.

### 3.4 Root signatures, pipelines, descriptors, memory

**Descriptor heap split.** The one shader-visible CBV/SRV/UAV heap (only one can be bound) is split:
`[0, kBindless)` persistent, `[kBindless, capacity)` the ring. `kBindless = 65536`
(`min(65536, capacity / 8)` when a driver forces a smaller heap, `GpuDescriptorRing::Init`
`d12_infra.cpp:1085-1097`). `GpuDescriptorRing` gets a base index: `AcquireChunk` wraps to the base, not 0;
`Cpu()/Gpu()` stay absolute. The ring loses 6.5% of 1M; a heavy frame uses tens of thousands.

**Bindless** (`BindlessAllocator` in `d12_infra.cpp`, thread-safe):

```cpp
UINT AllocateBindless(ID3D11Device* device, UINT count);            // contiguous, ~0u when full
void WriteBindless(ID3D11Device* device, UINT index, ID3D11ShaderResourceView* view); // null = null SRV
void WriteBindlessBuffer(ID3D11Device* device, UINT index, ID3D11Buffer* buffer);     // raw SRV over its range
void WriteBindlessNative(ID3D11Device* device, UINT index, ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& desc);
void FreeBindless(ID3D11Device* device, UINT index, UINT count);    // reusable after the current frame's fence
```

- The range is filled with null `Texture2D` SRVs at init (about 3 ms): tier-2 hardware then accepts an
  **unbounded SRV table** `DescriptorTable(SRV(t0, space100, unbounded, DESCRIPTORS_VOLATILE))` bound
  to `scope.bindless` (= heap start), which works from SM 5.1/6.0 up and does not need SM 6.6. Where
  `heapIndexing` is set, `ResourceDescriptorHeap[i]` addresses the same index (root signature flag
  `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`), because the range starts at heap index 0.
- SRV-only on purpose: tier 2 bounds UAV and CBV tables. TLASes are bound as root SRVs by address.
- `WriteBindlessBuffer` makes a raw SRV over a page range (`FirstElement = offset / 4`), so hit
  shaders read engine vertex/index data without copies; dynamic buffers are refused (they rename).
- Slots are written only when free (never rewrite a slot the GPU may read this frame: allocate a new
  one, free the old). A `View` remembers the slots written from it; its destructor writes null
  descriptors there (deferred to the fence) and logs once - a dangling slot never faults.

**Transient descriptors and upload memory** (context-local allocators, per frame):

```cpp
DescriptorSpan AllocateDescriptors(ID3D11DeviceContext* context, UINT count); // {cpu, gpu, index, count, increment}
UploadSpan AllocateUpload(ID3D11DeviceContext* context, UINT64 size, UINT64 alignment); // {cpu, gpu, resource, offset}
```
Ring chunks and upload pages of the context (`Context::descriptors/upload`), valid until the frame
ends - for per-dispatch tables, shader binding tables (64-byte aligned), per-frame instance
descriptors, constants.

**Root signatures and pipelines** (`d12_native.cpp`, new; device-owned caches, thread-safe):

```cpp
ID3D12RootSignature* RootSignature(ID3D11Device*, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, const wchar_t* name);
ID3D12PipelineState* ComputePipeline(ID3D11Device*, ID3D12RootSignature*, const void* dxil, size_t size, const wchar_t* name);
ID3D12PipelineState* StreamPipeline(ID3D11Device*, const void* stream, size_t size, UINT64 contentHash, const wchar_t* name);
HRESULT RaytracingPipeline(ID3D11Device*, const D3D12_STATE_OBJECT_DESC&, ID3D12StateObject** out);
ID3D12CommandSignature* CommandSignature(ID3D11Device*, const D3D12_COMMAND_SIGNATURE_DESC&, ID3D12RootSignature*);
```
- Root signatures are serialized with `g_d3d12.SerializeVersionedRootSignature` (1.1, fallback 1.0 as
  `RootSignatures::Init` does) and cached by the hash of the blob. Recommended static samplers
  (native passes do not use the layer's sampler tables and their generation dance).
- Compute and stream PSOs go through the existing `PipelineCache` library (stream PSOs via
  `ID3D12PipelineLibrary1::LoadPipeline`, stored with `StorePipeline`), names from content hashes,
  so mesh-shader pipelines persist like the rest. State objects are not library-storable; the
  driver's shader cache covers them. All creation calls are allowed off the render thread; features
  create at renderer init / `vid_restart` on a `TaskScheduler` task (the probe measured 30-60 ms per
  DXR pipeline - never on a frame).
- Memory: `CreateNativeBuffer(device, size, heap, flags, initial, name, out)` (committed),
  `CreateAccelerationStructureBuffer(device, size, name, out)` (DEFAULT, UAV flag, created in
  `RAYTRACING_ACCELERATION_STRUCTURE`), `ReleaseNative(device, object)` (through the
  `DeferredReleaser`), `QueryVideoMemory(device, budget, usage)` (`IDXGIAdapter3` of
  `Device::adapter`, local segment) so RT features can size their pools on an 8 GB card.

### 3.5 D3D11-path extensions (engine draws keep using the D3D11 API)

These let features change how the engine's existing draws execute without native sections.

- **VRS state per context** is specified by the VRS feature (`vrs.md` 3.4: `SetShadingRate(context,
  base, perPrimitive, image, rateImage)`, D3D11-like context state, re-applied on every new list,
  cleared by `ClearState`). Infra supplies what it builds on and other features share: the
  `ID3D12GraphicsCommandList5` pointer queried once in `OpenList` (`Context::m_list5`, also handed out
  as `NativeScope::list5`), the VRS fields of `NativeCaps` (its `GetShadingRateCaps` is a view of
  them), and the native-section contract: `BeginNative` hands out a list at 1x1 without an image
  (a non-default list is reset and marked dirty, as `vrs.md` specifies), `TouchedShadingRate` is for
  native code that sets rates itself.
- **Indirect draws**: `DrawIndexedInstancedIndirect`, `DrawInstancedIndirect`, `DispatchIndirect`
  become `ExecuteIndirect` with three device-level command signatures (no root arguments, so no root
  signature), args `Require`d as `INDIRECT_ARGUMENT` (page buffers promote). Plus
  `DrawIndexedIndirectCount(context, args, offset, maxCount, count, countOffset)` and
  `DispatchIndirectCount(...)` with a count buffer (tier 1.0 is enough) for GPU-driven culling with
  the engine's normal bindings.
- **Buffer views** (PLAN C10): D3D11 buffer SRVs (`BUFFER`/`BUFFEREX`) and UAVs, raw (`R32_TYPELESS`
  + `RAW`, the buffer created with `ALLOW_RAW_VIEWS`), structured (`BUFFER_STRUCTURED`, stride a
  multiple of 4 up to 2048, format `UNKNOWN`) and typed, with D3D11's creation rules. A buffer with
  SRV or UAV binding gets its own committed, tracked resource (views address it from offset 0; a
  structured view of a page range would need the page offset to be a multiple of the stride).
  Dynamic buffers get no views (every DISCARD renames them), append/counter UAVs are refused, both
  with a log line. `ClearUnorderedAccessViewUint` of a structured view goes through a raw alias of
  the same bytes (D3D12 clears no structured UAV); float clears of raw/structured views are dropped
  as D3D11 drops them.
- **Acceleration structure as a D3D11 SRV**: `CreateAccelerationStructureView(device, tlas,
  ID3D11ShaderResourceView** out)` - a layer `View` whose descriptor is a
  `RAYTRACING_ACCELERATION_STRUCTURE` SRV of an untracked resource (never transitioned; a null AS uses
  location 0). Bound with `PSSetShaderResources/CSSetShaderResources`, it lets DXIL pixel/compute
  shaders of existing passes (deferred sun, lights, AO) use inline `RayQuery`. `ShaderBindings`
  learns `D3D_SIT_RTACCELERATIONSTRUCTURE` and the null-descriptor table gets that dimension.

### 3.6 Shader compilation (DXC)

- **Runtime**: `Externals/dxc/` vendors DXC 1.9.2607 (`bin/x64/dxcompiler.dll`, `bin/x64/dxil.dll`,
  `LICENSE-MS.txt`, `LICENSE-LLVM.txt`, `LICENCE-MIT.txt`, `README.md`; headers `dxcapi.h` and
  `d3d12shader.h` come from the Windows SDK 10.0.26100) from `_work/sdk-downloads/dxc-1.9.2607`,
  vendored by `core` (PLAN C1). Both DLLs are staged by
  `xray_stage_windows_runtime` (`cmake/XRay.Dependencies.cmake`, `XRAY_DXC_RUNTIME_FILES`) and listed
  in `packaging/dead-air-x64/installer/runtime-files.txt`. `dxil.dll` signs the DXIL; without it the
  runtime refuses the shaders.
- **Layer** (`d12_dxc.cpp`, new): loads `dxcompiler.dll` from the executable's directory on first use
  (`LoadLibraryExW` with the full path; `dxil.dll` resolves next to it), `DxcShaderModel()`,

  ```cpp
  struct ShaderCompile
  {
      const char* source; size_t size; const char* name; const char* entry; const char* target; // "cs_6_6", "lib_6_5"
      const std::pair<const char*, const char*>* defines; UINT defineCount;
      UINT flags;                                        // CompileDebugInfo, Compile16BitTypes, CompileKeepReflection
      bool (*include)(void* user, const char* file, std::string& text); void* user;
  };
  bool CompileShader(const ShaderCompile& request, std::vector<unsigned char>& dxil, std::string& messages);
  HRESULT Reflect(const void* bytecode, size_t size, ID3D11ShaderReflection** out);
  ```
  Superseded by `dxc.md` 3.1/3.4 (PLAN C2/C3): `Reflect(bytecode, size, REFIID, void**)` returns
  `ID3D12ShaderReflection` for both formats (DXBC: `D3DReflect` with the D3D12 IID; DXIL:
  `IDxcUtils::CreateReflection`), no adapter object. `Shader::Reflect` and the input-signature parser
  (`d12_device.cpp:125-190`, ISG1 with stride 32) use it.
- **Native pass shaders** live in `gamedata/shaders/r3/native/*.hlsl` (RT: `r3/rt/`; overlay,
  moddable like the rest), compiled through `da_dxc_compile_native` of `da_dxc.cpp` (`dxc` lane, PLAN
  C2): FS include handler (relative to the includer first, then the r3 root), cache
  `appdata/shaders_cache_grizzly/r5_dxil/<tag>/native/<path>.<entry>.<target>/<defines hash>`
  with header {magic, DXC version, `getFileCrc32` of source + includes, DXIL crc}. Always defined:
  `GRIZZLY_DXC=1`, `GRIZZLY_SM=<sm>`, `GRIZZLY_WAVE_LANES=<lanes>`, `SMAP_ARRAY=1`. Flags: `-HV 2021 -O3`,
  `-enable-16bit-types` only on request and with `fp16=1`, reflection stripped for native passes.
  Shared headers (`da_wind_*.h`, `tree_instance.h`) must compile under both FXC and DXC, so an RT or
  compute pre-transform pass animates vegetation with the very same wind code as the shadow and
  G-buffer vertex shaders (plant shadows keep swaying in every technique).
- **SM 6 for existing D3D11-path shaders**: superseded by `dxc.md` (PLAN C2). There is no opt-in
  marker: R5 compiles every engine shader with DXC in the session's mode, guards SM 6 code with
  `#if GRIZZLY_DXC`, and a pass whose stages ended up in different formats is unified to DXBC twins
  (`dxc.md` 3.6); the layer's mix guard logs any mixed PSO by name.

### 3.7 Self-test

`r__native_selftest` (registered in `xrRender_console.cpp` for both modules; R4 prints "DirectX 12
only") runs once in-game on the render thread with embedded HLSL (no gamedata dependency): (a) a
native `cs_6_0` wave-sum dispatch on the immediate context writing a D3D11-created UAV texture that a
D3D11 draw then samples; (b) the same on a deferred context executed with `ExecuteCommandList`;
(c) an async job writing a buffer between a fork and a join, read back through a staging copy;
(d) 1024 bindless slots written and summed through the unbounded table; (e) a one-triangle
BLAS/TLAS + `RayQuery` (when `rt=1`), bound once natively and once through
`CreateAccelerationStructureView`; (f) a native section with `RSSetShadingRate(2x2)` on a 64x64 target through `list5`, checked for
2x2 uniform blocks and for the engine draw after `EndNative(TouchedShadingRate)` shading at 1x1
(when `vrs_tier=2`); (g) `DrawIndexedIndirectCount` of 3 draws. Prints
`[qa] native selftest: immediate ok, deferred ok, async ok, bindless ok, raytracing ok, vrs ok, indirect ok, dxc 1.9.0.5402`
or the first failure.

## 4. Integration points

| File | Change |
|---|---|
| `src/Layers/xrRenderDX12/d12_api.h` | `NativeCaps`, `DeviceCreateInfo::disabledFeatures`, native scope API, helpers, async jobs, DXC, indirect-count entry points; the "immediate only" comment goes |
| `d12_layer.h` | `Device`: caps, `computeQueue`, `crossFence`, `computeFence`, `copyQueue`, bindless allocator, command signatures, native root-signature cache; `Context`: `m_list5`, `QueuedList::kind/job`; `Resource::lentTo`; `View` bindless slots |
| `d12_device.cpp` | `Init` (`:1491-1520`): caps query + log + mask, queues/fences, bindless init; `WaitIdle` waits for compute; `ProbeAdapter` overload |
| `d12_infra.cpp` | `QueryNativeCaps`, ring base (`GpuDescriptorRing`), `BindlessAllocator`, stream-PSO library entries |
| `d12_context_ops.cpp` | `Submit` markers, `OpenList` QI of list5, lent-resource guard in `RequireTracked`, indirect args states |
| `d12_context_state.cpp` | indirect draws via `ExecuteIndirect`, AS SRV binding kind (VRS application: `vrs.md`) |
| `d12_objects.cpp` | `Shader::Reflect` through `Reflect`, AS view object, view bindless bookkeeping |
| `d12_swapchain.cpp` | old native API as wrappers; `Present1` joins open jobs before `Submit` |
| `d12_native.cpp`, `d12_async.cpp`, `d12_dxc.cpp` (new) | sections 3.2/3.4, 3.3, 3.6 |
| `src/Layers/xrRenderDX12/CMakeLists.txt` | new sources; `Externals/dxc/inc` include dir |
| `src/Layers/xrRenderDX11/dx11HW.cpp/.h` | disable flags into `DeviceCreateInfo`; `CHW::native` |
| `src/Layers/xrRenderDX11/dx11GpuTimers.*` | no `Async` slot (PLAN C13): job times go to the `cq_*` fields of the `core` field registry |
| `src/Layers/xrRender/xrRender_console.cpp` | `r__caps` keys (`:849`), `r__native_selftest`; nothing in `xrRender_sync_preset_derived` (`:1086`) |
| `src/Layers/xrRenderPC_R4/da_upscaler.h` | `UpscalerCaps` fields |
| `r2_test_hw.cpp` | caps cache write (R5) / read (both) |
| `r4_shaders.cpp` | nothing here: the DXC branch is `dxc.md`'s (`dxc` lane) |
| `da_native_caps.cpp` (`core`), `da_native_selftest.cpp` (this lane), `sources.cmake` (`core`) | caps hook, self-test; native shader compile/cache is `da_dxc.cpp` (`dxc.md`) |
| `cmake/XRay.Dependencies.cmake`, `runtime-files.txt`, `docs/dead-air/DEPENDENCIES.md` | DXC runtime + licence |
| `docs/dead-air/DX12_RENDERER.md` | section 2 `r__caps` row, section 4 pointer to this file |
| `tools/qa/d3d12layer/scenarios_native.cpp` (new), `scenarios_api.cpp` list | scenarios (9.1) |
| `tools/qa/water/native_check.lua` (new) | stand scenario (9.3) |

New engine files start with the Grizzly/MIT header; `Externals/dxc` keeps Microsoft's files as
they are.

## 5. Data and resources

- Descriptors: 65536 persistent (about 2 MB of heap), ring 934464. Null-filled at init.
- Queues: one compute queue, one copy queue (creation-time uploads, I7; none with `-dx12_no_copy_queue`), three fences (`crossFence`,
  `computeFence`, `copyFence`); an async timestamp heap of 128 slots + readback.
- Command signatures: 3 at init (draw, draw indexed, dispatch), count variants share them.
- Caches: native root signatures (tens), stream PSOs in `pipelines.bin`, DXIL files under
  `shaders_cache_grizzly/r5/native` and the `dxil` permutations of engine shaders.
- Files: `render-caps-d3d12.cache` next to `render-hardware-d3d12.cache`.
- Binaries: `dxcompiler.dll` (28 MB) + `dxil.dll` (3.8 MB) of DXC 1.9.2607 in the game's bin folder.

## 6. Shaders

Infra itself adds only the embedded self-test HLSL and no shader under `gamedata`. It defines the
conventions features follow: native shaders in `shaders/r3/native/`, DXC targets up to `sm` from
`r__caps`, bindless through `space100` unbounded SRV tables, static samplers, `#if GRIZZLY_DXC` guards in
headers shared with FXC, the DXIL/DXBC pairing rule (3.6).

## 7. UI and presets

Infra adds **no menu rows and nothing to the quality presets**. It provides the keys the Video tab
consumes:
- The **RTX** category (owned by the ray tracing features) is its own group in
  `dead_air_x64_video_options.ltx`, never in `_preset` ladders or `xrRender_sync_preset_derived`.
  Rows are enabled when `rt=1`; otherwise shown disabled with an `off_hint`: `api ~= 12` and
  `rt_tier >= 11` -> "needs DirectX 12", else "not supported by this GPU" - the frame-generation
  pattern (`ui_mm_opt_video.script:97-127`).
- VRS / mesh-shader / GPU-driven features that change the look go through the presets as usual and
  read `vrs` (set by the VRS feature), `ms`, `sm` for availability.
- Diagnostics are command-line only (`-dx12_no_rt`, `-dx12_no_vrs`, `-dx12_no_ms`, `-dx12_no_async`,
  `-dx12_no_sm6`) plus `r__native_selftest`; none is saved.

## 8. Fallback

- **DX11 (R4)**: untouched rendering; `r__caps` gains keys (0 except `rt_tier`/`sm=50`).
- **DX12 without a capability**: the key is 0 and features stay off (rows disabled). No DXR 1.1 ->
  `rt=0`; VRS tier 1 -> `vrs_tier=1` (per-draw rate only; the VRS feature needs 2); no mesh shaders
  -> `ms=0`.
- **No DXC** (DLL missing or load failure): `sm=50`, `rt=0`, `ms=0`; `//grizzly:dxil` shaders compile
  with FXC; one log line.
- **No compute queue / `-dx12_no_async`**: jobs run in order on the direct queue (`acq=0`).
- **Bindless full**: `AllocateBindless` returns `~0u`, log once; the feature degrades.
- **Device removed**: fences report `UINT64_MAX`, waits return (`FenceTimeline::Wait`), native helpers
  return failure.
- **Windows 10**: the design needs only what the in-box runtime of Windows 10 2004+ has (DXR 1.1, VRS,
  mesh shaders, SM 6.5, legacy barriers, unbounded SRV tables). SM 6.6 heap indexing is optional
  (`heapIndexing`). The Agility SDK is therefore not shipped; if a feature ever needs SM 6.6+ on
  Windows 10, `ID3D12SDKConfiguration1::CreateDeviceFactory` (present here, no exe exports) is the
  route.

## 9. Test plan

### 9.1 Layer tests (`tools\qa\d3d12layer\Run-LayerTests.ps1 -HighPerformance`, then `-Gbv`)

Pass: exit code 0, all 36 existing scenarios still pass, debug layer and GBV silent. New scenarios
in `scenarios_native.cpp` (layer-only ones compare against values computed on the CPU or against
the equivalent D3D11 result on the reference device):
1. `native caps` - `GetNativeCaps` equals direct `CheckFeatureSupport`; masks zero the right fields.
2. `native section on deferred context` - deferred: D3D11 draw into T, native CS writes T's second
   mip as UAV (`RequireView`), `FinishCommandList` + `ExecuteCommandList`; immediate samples both
   mips; reference renders the same with D3D11 compute. Pixel-exact.
3. `EndNative touched subsets` - draw A, native dispatch with `TouchedComputeRoot|TouchedPipeline`,
   draw B with unchanged bindings; equals reference; `-dxstats` counters show no heap re-bind.
4. `native UAV then engine UAV` - native CS and D3D11 CS write the same buffer in sequence; result
   exact (the `uavWritten` fix).
5. `async compute hand-off` - PS writes T; job reads T (entry NSR) and writes buffer B (COMMON);
   join; PS reads B. Runs with async on and off: identical, GBV silent.
6. `async job not joined` - fork without join, Present: auto-joined, one log line, result valid.
7. `async lent guard` - direct draw reads a lent texture between fork and join: join inserted, one log line.
8. `bindless` - 1000 slots written from SRVs, CS sums via unbounded table; free + reuse after 4 frames.
9. `VRS through native sections` - native `RSSetShadingRate(2x2)` then `EndNative(TouchedShadingRate)`:
   the next D3D11 draw shades at 1x1; `BeginNative` on a list the VRS context state left non-default
   hands it out at 1x1 (skipped when `vrs_tier<2`; the context-state scenarios are `vrs.md`'s).
10. `indirect draws` - `DrawIndexedInstancedIndirect`/`DrawInstancedIndirect`/`DispatchIndirect`
    vs D3D11 (differential); `DrawIndexedIndirectCount` vs individual draws.
11. `DXIL through the D3D11 path` - CS 6.0 `WaveActiveSum`, VS/PS 6.0 pair reflected through
    `Reflect` (`ID3D12ShaderReflection`) with the input layout from an ISG1 signature; a mixed
    DXBC/DXIL pair logs the refusal once (together with `dxc.md`'s `dxil_bindings` and
    `dxil_dxbc_mix`, in `scenarios_dxil.cpp`).
12. `acceleration structure SRV` - PS 6.5 `RayQuery` against the one-triangle TLAS bound through
    `CreateAccelerationStructureView` (skipped when `rt=0`).
13. `creation-time uploads from 3 loader threads while frames are submitted` - IMMUTABLE/DEFAULT textures
    (single mip and 16 -> 4 chains, some released at once) and buffers (static pages, view-carrying committed)
    created on three threads while the immediate context submits frames; atlas and buffer words read back
    against D3D11; the layer checks which queue carried the bytes. Runs in both modes: default (copy queue)
    and `Run-LayerTests.ps1 -NoCopyQueue` (harness `-nocopyqueue`: the whole suite on the direct queue, I7).

### 9.2 Build

`Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'`: no errors, no new warnings in touched files.

### 9.3 Stand (`Invoke-Stand`, `D:\Games\Dead Air\_qa\lightdiag`, save `qa_pedro`)

| Run | Command | Pass |
|---|---|---|
| R5 caps | `-Scenario render_check -ExtraArgs '-r5 -always_active'` | log has the two `* native:` lines; `[qa] caps` contains `api=12` and `rt=1;rt_tier=12;vrs_tier=2;vrs_tile=16;ms=1;sm=68;wave=32;fp16=1;eb=1;acq=1` (`sm` = min(device 6.8, DXC 6.9)); no `FATAL`, `device lost`, `native section`, `refused`; frames equal the pre-change baseline with SMAA (per-channel diff <= 2) |
| R4 caps | `-Scenario render_check -ExtraArgs '-always_active'` | `[qa] caps` has `api=11` and `rt=0;rt_tier=12;vrs_tier=0;...;sm=50`; frames equal the R4 baseline |
| R5 masked | `-Scenario render_check -ExtraArgs '-r5 -always_active -dx12_no_rt -dx12_no_vrs -dx12_no_ms -dx12_no_async -dx12_no_sm6'` | caps `rt=0;rt_tier=0;vrs_tier=0;ms=0;sm=50;acq=0`; frames equal baseline |
| Self-test | `-Scenario native_check -ExtraArgs '-r5 -always_active'` (runs `r__native_selftest` after load, prints `DA_WATER_PROBE_DONE`) | `[qa] native selftest: ... ok` for every part, nothing else logged with `!` by the layer |
| Self-test, validation | same with `-r5 -always_active -dxdebug` | as above and no D3D12 debug-layer error/corruption lines |
| Self-test, fallback | same with `-dx12_no_async -dx12_no_rt` | async/raytracing parts report `skipped`/in-order, the rest ok |
| Upscalers | `-Scenario upscale_check -ExtraArgs '-r5 -always_active'` | every step shot and FPS line present; FSR/DLSS/FG frames equal the pre-change baseline (native interop wrappers) |
| Motion | `-Scenario motion_check -ExtraArgs '-r5 -always_active' -Globals "qa_render_cmds = { 'r__aa st_aa_taa', 'r__motion_vectors 1' }"` | recording equal to baseline |
| Copy queue (I7) | `-Scenario texture_load` (dstorage.md 9, DirectStorage on), interleaved pairs of `-r5 -always_active -dx12_no_copy_queue` / `-r5 -always_active` on `qa_pedro` | log `* native: creation-time uploads on a copy queue` (`... on the direct queue (-dx12_no_copy_queue)` in the A runs); load wall (the phases up to `Final texture upload` / `net_start_client5`, and the `[texload]` batch wall) per run; no `!` layer line; frames of the B runs equal the A runs |

### 9.4 Performance (ninja.lock held, no compile sharing the CPU)

`ab_run.ps1 -Runs r5a,r5b,r5c,r5d` with the new DLL for b/d and the baseline for a/c (`Invoke-Stand
-Dll`), 2560x1440 Extreme TAA, `r__gpu_log 300`, saves `qa_pedro` and `foliage`, `analyze.ps1`.
Pass: FPS within +-1% at equal clocks, every `r__gpu_log` pass within +-0.05 ms, `-dxstats`
submissions per frame unchanged (2). The infra must cost nothing until a feature uses it.

## 10. Performance expectation

- Idle: 0 per frame. Caps query < 1 ms at start; ring split, bindless init 3 ms once.
- `EndNative(touched)`: a mid-frame DXR/compute native section re-applies one root + PSO instead of
  the full state (tens of microseconds of render thread per section saved).
- Indirect: same CPU cost as a direct draw.
- Async compute: markers are free on the CPU; each fork adds one `ExecuteCommandLists` + two fence
  operations (about 20-40 us). GPU gain is the feature's: on Ampere, compute passes overlapped with
  geometry-bound work (sun cascades about 2 ms at 1440p) typically hide 30-70% of their cost; nothing
  is gained against bandwidth-bound passes.
- DXC: first compile 20-200 ms per shader, cached afterwards; DXR pipelines 30-60 ms each at load.

## 11. Risks

- **Cross-queue lifetime**: relies on the join-before-Present invariant; enforced in `Present1` and
  by the lent-resource guard; GBV scenario 5-7 cover it.
- **Fence-value hijack**: cross-queue signals must never use `Device::fence` (section 2); separate
  timelines by design.
- **Buffer decay** across queues: rules in 3.3; tracked buffers declared COMMON at hand-off.
- **VRS state leaking into native sections** (or lost after them): `BeginNative` resets to 1x1 and
  marks dirty, scenario 9; list-split behaviour is covered by `vrs.md`.
- **DXBC/DXIL mixing** refused by the runtime: pairing rule + linkage checker + the layer's refusal log.
- **Reflection adapter** struct mismatch (`Space`/`uID`): translated, scenario 11.
- **Tier-2 bindless**: whole range null-initialized; SRV-only.
- **DXC size and licence**: 32 MB more in the release; Microsoft redistributable + LLVM/MIT licences
  recorded in `DEPENDENCIES.md`.
- **`r__caps` length**: about 155 of 255 chars; truncation logged.
- **Streamline / FFX** own queues and heaps: untouched; their native sections keep `TouchedAll`.
- **Copy-queue textures in COMMON**: tracker starts them COMMON; gated on the load-time A/B.
- **DX11 intact**: R4 changes are read-only caps plumbing; R4 stand run compares frames.

## 12. Implementation order

1. **Caps**: vendor `Externals/dxc` (runtime staging, `runtime-files.txt`, `DEPENDENCIES.md`) and
   the DXC loader part of `d12_dxc.cpp` (`DxcShaderModel`, version string) - `rt`, `ms` and `sm`
   depend on it; `NativeCaps`, `QueryNativeCaps`, log, disable flags (`dx11HW.cpp`), `GetNativeCaps`,
   probe overload + `render-caps-d3d12.cache`, `UpscalerCaps` fields, `da_native_caps.cpp`,
   `r__caps` print; DX12_RENDERER.md row. Layer scenario 1; stand R5/R4/masked caps runs.
2. **Native sections v2**: `NativeScope`, `BeginNative/EndNative(touched)` on any context,
   `RequireView`, `UavBarrier`, `uavWritten` fix, old API as wrappers, `NativeDescriptor`,
   `NativeBufferRange`. Scenarios 2-4; `upscale_check`.
3. **Descriptors and memory**: ring base + `BindlessAllocator` (null init, deferred free, view
   bookkeeping), `AllocateDescriptors`, `AllocateUpload`, `CreateNativeBuffer`,
   `CreateAccelerationStructureBuffer`, `ReleaseNative`, `QueryVideoMemory`. Scenario 8.
4. **Pipelines**: `RootSignature` cache, `ComputePipeline`, `StreamPipeline` (library),
   `RaytracingPipeline`, `CommandSignature`.
5. **DXC compile**: `d12_dxc.cpp` (`CompileShader`, `Reflect` per `dxc.md` 3.1, ISG1); the engine
   side (includes, cache) is `da_dxc.cpp` of the `dxc` lane. Scenario 11 (compute part). In PLAN
   order this lands with step 1 (milestone I1).
6. **D3D11-path extensions**: indirect draws + count variants, AS SRV, the VRS contract of native
   sections (the context VRS state itself is `vrs.md`'s step). Scenarios 9, 10, 12.
7. **SM 6 for engine shaders**: replaced by `dxc.md` (global DXC mode + DXBC twins, PLAN C2); the
   layer part (DXIL `Shader::Reflect`, `ProgramType`, `Shader::dxil`, mix guard) lands in I1.
   Scenario 11 (graphics part).
8. **Async compute**: queue, fences, recorder pool, jobs, `Submit` markers, Present join, guard,
   fallback, timestamps + `dx11GpuTimers` `Async` slot. Scenarios 5-7.
9. **Copy queue** for `InitUploader`: after DirectStorage has landed (PLAN C23), implement, then A/B
   the `qa_pedro` load time with DirectStorage on (5 interleaved loads each); merged default-on only
   if it gains, otherwise removed (no `-dx12_copy_queue` flag). Done: default-on (I7).
10. **Self-test** `r__native_selftest` + `tools/qa/water/native_check.lua`; full 9.3 matrix; 9.4 perf
    A/B; update DX12_RENDERER.md section 4.

## 13. Implementation record

### I0 - API freeze (28.09.2026 05:46)
- `d12_api.h` declares everything of 3.1-3.5, `dxc.md` 3.1/3.4, `async.md` 3.3, `vrs.md` 3.4, `mesh.md` 3.5
  and PLAN C9; later additions only inside the `[dx12:<lane>] begin/end api` blocks at its end. Beyond the
  designs: `NativeDevice5`, `DrawInstancedIndirectCount`, `DiscardAsyncCompute`, `NativeCaps::vendorId/uma`
  (the async auto rule), `ShaderDefine`, `ShaderOutput::isSigned`.
- `d12_layer.h`: per-lane blocks and hooks (`Device::vrs/mesh/texheap`, `Context::vrs/mesh`, the VRS dirty
  flags in `Context::Applied`, `Resource::placement`, `RootSignatures::mesh`, `GraphicsPsoDesc::meshKind`,
  `PipelineCache::GraphicsMesh`), `Shader::kind/dxil`, `Resource::lentTo/lentShared`,
  `CommandList::type`, `Requirement::transitioned`, feature counters of `-dxstats`. Hook calls pre-placed
  in `d12_context_state.cpp`, `d12_context_ops.cpp`, `d12_device.cpp`, `d12_infra.cpp`, `d12_objects.cpp`,
  `d12_swapchain.cpp`; lane stubs in `d12_vrs.cpp`, `d12_mesh.cpp`, `d12_texheap.cpp` keep today's
  behaviour (the VRS stub already resets a list left non-default by native code, C31).
  `Context::PrepareGraphics(bool mesh)` is the draw preparation shared with the mesh path.
- `ID3D12GraphicsCommandList4/5/6` are queried once per pooled list at creation (weak pointers),
  `ID3D12Device2/5` at `Device::Init`.
- Layer tests: one scenario file per lane, registered by `Scenarios()`; `layertest.lock` around build and
  run (several agents share `build\qa\d3d12layer`).
- Gate: build EXIT 0; `Run-LayerTests.ps1 -HighPerformance` 36/36; R5 `render_check` qa_pedro with the
  Gate-0 overlay (`infra-i0-render-pedro-g0ovl`) equal to same-period runs of the Gate-0 binaries (within
  99.52 % against 98.25 % of the references, 4 clusters > 16 px, largest 50 px, PSNR 63.4 dB). The 04:xx
  Gate-0 references themselves no longer hold under load: the Gate-0 binaries run at 05:44
  (`infra-i0-control-pedro`) reach only 91.4 % against them (later game time at the shot).

### I1 - capabilities, DXC service, DXIL through the D3D11 path (28.09.2026 06:02)
- Capabilities (`d12_native.cpp`): `QueryNativeCaps` (OPTIONS 1/3/4/5/6/7/10/12/21, shader model walked
  down from 6.9, root signature from 1.2, runtime SDK version, UMA, vendor), `MaskNativeCaps`
  (`-dx12_no_rt/_vrs/_ms/_async/_sm6` through `DeviceCreateInfo::disabledFeatures`), the two `* native:`
  log lines, `GetNativeCaps`, `GetShadingRateCaps` (a view), `ProbeAdapter(..., NativeCaps&)` on the
  probe's throw-away device. `render-caps-d3d12.cache` (magic, version, the verdict's signature, RT/VRS/mesh
  tiers, shader model) is written by the R5 probe and read by both modules through
  `da_native_caps_register_probe_reader`; R4 reports only `rt_tier` from it (PLAN C7).
- DXC service (`d12_dxc.cpp`, dxc.md 3.1): `dxil.dll` + `dxcompiler.dll` by absolute path from the exe
  folder, `DxcCreateInstance` through `GetProcAddress`, version >= 1.8, version string `1.9.0.5402` from
  `IDxcVersionInfo2`, one-time DXIL pipeline self-check on the device (off for the session if refused),
  per-thread `IDxcCompiler3`/`IDxcUtils`, SEH-guarded `Compile`, include handler through the request's
  callback (paths normalized, non-UTF-8 text made ASCII like the engine's sources), `Reflect` (DXBC ->
  `D3DReflect`, DXIL or a DXC reflection blob -> `IDxcUtils::CreateReflection`), `Disassemble`, `IsDxil`.
  The layer loads it at `Device::Init` unless `-dx12_no_sm6`; `dxcShaderModel` is 0 when either step fails.
- DXIL acceptance: `Shader::Reflect` on `ID3D12ShaderReflection` for both formats (bindings outside space 0
  are ignored with a log line), `ProgramType` reads the DXIL part (mesh 13 via `CreateVertexShader`,
  amplification 14 via `CreateGeometryShader`, `Shader::kind`), `Shader::dxil`, VS input semantics of DXIL
  from its ISG1 part (stripped reflection has no names), mixed DXIL/DXBC pipelines refused by name before
  the runtime sees them (`! graphics pipeline ... mixes DXIL (ps 'a') and DXBC (vs 'b') stages: refused`,
  `-dxstats` counts them).
- Harness: `-dxil` (`Run-LayerTests.ps1 -Dxil`) builds the layer device's scenario shaders with DXC SM 6.0;
  float images then compare with a 1e-5 relative tolerance (DXC contracts `a * b + c` into mad). New
  scenarios: `native caps` (scenario 1), `DXIL through the D3D11 path` (scenario 11: wave compute and a
  VS/PS pair with an ISG1 input layout against D3D11/FXC), `dxil_bindings` (FXC and DXC builds of one draw
  with a reserved register: identical), `dxil_dxbc_mix`.
- Gate: layer tests 40/40 with `-HighPerformance`, `-Dxil`, `-Gbv`, `-Gbv -Dxil` (`qa\infra-i1-layertests*`).
  Stand (qa_pedro, parity mode): R5 `infra-i1-caps-r5` `rt=0;rt_tier=12;vrs_tier=2;vrs_tile=16;ms=1;sm=68;
  wave=32;fp16=1;eb=1;acq=0` (`rt` is the rt lane's, `acq` arrives with I5), both `* native:` lines,
  `* shader compiler: dxcompiler 1.9.0.5402, shader model 6.9`, no FATAL/refused/device-lost line; R4
  `infra-i1-caps-r4` `api=11;rt=0;rt_tier=12;vrs_tier=0;...;sm=50`; masked `infra-i1-caps-masked`
  (`-dx12_no_rt -dx12_no_vrs -dx12_no_ms -dx12_no_async -dx12_no_sm6`) `rt=0;rt_tier=0;vrs_tier=0;ms=0;sm=50;
  acq=0` and `switched off: ...` in the log; its frame against the unmasked R5 run: 0.30 % moving pixels,
  PSNR 56.3 dB (the Gate-0 references' own spread: 0.36 %, 56.0 dB).

### I2 - native sections v2 (28.09.2026 06:48)
- `d12_native.cpp`: `BeginNative(context, scope)` works on the immediate context and on deferred contexts (their
  recording thread): the open list, the layer heaps bound only when the list lacks them, pending barriers
  flushed, the VRS hook hands the list out at 1x1, `NativeScope` with the cached `list4/5/6`, the heap and
  `bindless` (= heap start). `EndNative(context, touched)` marks only what was touched: heaps -> everything;
  graphics root -> graphics and mesh root flags, VS..PS root arguments, PS UAV table; compute root -> CS root
  arguments and UAV table; pipeline; input assembler; output merger; rasterizer; shading rate -> VRS dirty +
  list non-default. It always sets `uavWritten`, so the next engine UAV access gets its UAV barrier (the gap of
  section 2). `Require`/`RequireMany`/`RequireView` go through the context's tracker (deferred: the first
  requirement becomes the list's entry state, `ExecuteCommandList` patches it in) and flush at once; untracked
  buffers need nothing and are skipped silently, untracked (staging) textures log once. `UavBarrier(resource)`
  (global for resources without UAV access), `NativeDescriptor` for all four view kinds (back buffer RTVs resolve
  to the current buffer), `NativeBufferRange` (page range, current dynamic version, tracked buffer, staging
  memory). The old three calls are wrappers (`BeginNative(immediate)` = a scope's list, `EndNative(immediate)` =
  `TouchedAll`, `Require` shared), so FSR/DLSS/frame generation code is unchanged.
- `TextureWriteGeneration` (C9) also moves on `UpdateSubresource`, `CopyResource`, `CopySubresourceRegion` and the
  `Unmap` of a dynamic texture into textures without RT/DS/UAV binding (creation/destruction since I0).
- Scenarios (`scenarios_native.cpp`): 2 `native section on a deferred context` (D3D11 draw into mip 0, native
  DXBC 5.1 compute writes mip 1 through `RequireView` + `NativeDescriptor` + `AllocateDescriptors`, executed and
  sampled on the immediate context: equal to D3D11 compute), 3 `EndNative touched subsets` (draw, native dispatch
  with compute root | pipeline, draw: equal to D3D11; after `EndNative` heaps and graphics root stay applied, and
  draw B re-sets exactly 1 PSO, 0 root CBVs, 0 CBV/sampler tables), 4 `native UAV then engine UAV` on a fresh
  deferred context (exact values; `uavWritten` set).
- Gate: layer tests 45/45 in all four modes (`-HighPerformance`, `-Dxil`, `-Gbv`, `-Gbv -Dxil`: `qa\infra-i2i3-layertests`,
  `-dxil`, `-gbv`, `-gbv-dxil`; floor now 45).
  Stand, parity recipe of PLAN 0.6 with a control:
  `upscale_check` R5 `infra-i2-upscale-g0ovl` (tree binaries, Gate-0 overlay; all 12 steps shot, FSR, FSR frame
  generation, DLSS, DLAA and DLSS + FSR frame generation through the native wrappers, no `!` layer line) against
  `core-g0-upscale-{a,b,c}` + the same-period Gate-0 control `infra-i2-upscale-ctl`: calibrated "equal" PASS in
  every step (within 98.6-99.96 %, PSNR 54.3-58.0 dB against ref a; step 2 is the `vectors` QA overlay, which the
  references show in some runs only).

### I3 - descriptors, memory, pipelines (28.09.2026 06:56)
- Heap split: `[0, 65536)` persistent bindless range (`min(65536, heap / 8)`), the ring from there (934464 of the
  1M heap); `GpuDescriptorRing::Base()`, the ring wraps to its base, and its failure paths return a ring slot,
  never a bindless one.
- `BindlessAllocator` (`d12_native.cpp`, device-owned, thread-safe): the range is null-filled at init with
  Texture2D null SRVs (64 `CopyDescriptors` of 1024 each), first-fit contiguous allocations (`kBindlessInvalid`
  + one log line when full), `FreeBindless` returns the range, nulled, once the fence of the frame it was freed in
  passed (`Device::EndFrame`, the ring's pattern - so deferred lists executed later in that frame are covered).
  `WriteBindless` copies an SRV's descriptor and remembers slot -> view id; a view released while a slot still
  holds it logs once and the slot is nulled after that frame, unless it was rewritten or freed meanwhile
  (per-slot generation). `WriteBindlessBuffer` writes a raw SRV over a page range or a tracked buffer (dynamic and
  staging buffers refused with a log line: they rename), `WriteBindlessNative` any descriptor.
- `AllocateDescriptors` (ring space of the context, at most a quarter of the ring per request), `AllocateUpload`
  (the context's upload pages, any alignment), `CreateNativeBuffer` (committed), `CreateAccelerationStructureBuffer`
  (size rounded to 256, UAV flag, created in `RAYTRACING_ACCELERATION_STRUCTURE`; `DXGI_ERROR_UNSUPPORTED` without
  DXR or with `-dx12_no_rt`), `ReleaseNative`, `QueryVideoMemory` (`IDXGIAdapter3` of the device's adapter, local
  segment).
- `RootSignature` (serialized as given; a 1.1 description is converted to 1.0 where the runtime lacks 1.1; cached by
  the blob's hash, lives as long as the device), `ComputePipeline` (DXBC or DXIL; key = code hash + root signature
  content hash, persisted in the pipeline library; a root signature not made by `RootSignature` is kept alive and
  its pipelines stay in memory only), `StreamPipeline` (`ID3D12Device2::CreatePipelineState`, persisted through
  `ID3D12PipelineLibrary1::LoadPipeline/StorePipeline`; `contentHash` must cover everything the stream holds),
  `CommandSignature` (cached by argument content + root signature), `RaytracingPipeline`
  (`ID3D12Device5::CreateStateObject`, not cached: 30-60 ms, create at init). The library entries are
  `PipelineCache::NativeCompute/NativeStream` (key salted apart from engine pipelines); the mesh lane can put its
  mesh pipelines through `NativeStream`.
- Bindless tables: `DescriptorTable(SRV(t0, space100, caps.bindlessCapacity))` bound at `NativeScope::bindless`
  (whole range null-initialised, valid on binding tier 2); an unbounded range works on tier 3. FXC needs
  `D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES` for `Texture2D x[]` in SM 5.1; DXC does not.
- Scenarios: 8 `bindless` (1000 slots written from SRVs and summed by a native CS through the unbounded table:
  exact; 16 unwritten slots read 0; a released view's slot logs once and reads 0 after the frame; freed slots are
  not reused before the frame's fence and are reused, null, after it), `native pipelines` (root signature and
  compute caches by content, `ExecuteIndirect` of a dispatch through a cached command signature with arguments in
  `AllocateUpload` memory, a stream PSO drawing natively into a D3D11 RTV followed by a D3D11 draw into the same
  target (touched graphics root, pipeline, IA, OM, RS re-applied), a `lib_6_3` DXR state object with shader
  identifiers, an acceleration structure buffer, `QueryVideoMemory`, `NativeBufferRange` of the three buffer kinds
  incl. a DISCARD rename and a native read of a page range, `TextureWriteGeneration` moving on create/update/copy
  and not on an RT clear).
- Gate: layer tests 45/45 in all four modes (as I2). Stand: R5 `render_check` qa_pedro `infra-i3-render-pedro-g0ovl`
  (tree binaries, Gate-0 overlay, SMAA + `wind_freeze 1`, `-Tga`) against `core-g0-render-pedro-{a,b,c}` + the
  control `infra-i3-ctl-pedro`: PASS (within 98.64 %, references 97.44 %; 90 clusters, references 239; largest
  414 px, references 3105 px); against the control alone 99.92 % within 2/255, largest cluster 10 px, PSNR 59.0 dB.
  The control itself fails against the quiet-machine references (96.9 %), as at I0.
- Build note: five layer objects built at 05:24 had no header dependencies recorded in `build\ninja-x64`
  (`ninja -t deps` `#deps 0`; the layer has no PCH), so the I2/I3 change of `d12_layer.h` did not rebuild them and
  R5 staged between 06:14 and 06:35 crashed in `ApplyHeaps` (core's report). Those objects were rebuilt under
  `ninja.lock`; all twelve layer objects now carry their dependencies.

### I4 - D3D11-path extensions (28.09.2026 13:30)
- Buffer views (PLAN C10, `d12_objects.cpp`): D3D11 buffer SRVs (`BUFFER`/`BUFFEREX`) and UAVs, raw / structured / typed,
  D3D11's creation rules (raw needs `ALLOW_RAW_VIEWS` and `R32_TYPELESS`; structured needs format `UNKNOWN`, stride a
  multiple of 4 up to 2048, excludes raw views and indirect arguments; the range must fit; a null description only for
  structured buffers). Buffers with SRV or UAV binding are committed and tracked (views address them from offset 0);
  dynamic buffers get no views and append/counter UAVs are refused, each with one `!` log line.
  `ClearUnorderedAccessViewUint` of a structured view goes through a raw alias of the same bytes (the D3D12 debug layer:
  "ClearUnorderedAccessView* methods are not compatible with Structured Buffers"); raw/structured views clear with the
  first value (D3D11), float clears of them are dropped (D3D11). Every UAV clear is now ordered after earlier UAV work
  (UAV barrier when `uavWritten`), as D3D11 orders it.
- Indirect draws (`d12_context_state.cpp` `Context::Indirect`): `DrawIndexedInstancedIndirect`, `DrawInstancedIndirect`,
  `DispatchIndirect` and the count variants of `d12_api.h` (`DrawIndexedIndirectCount`, `DrawInstancedIndirectCount`,
  `DispatchIndirectCount`) are one `ExecuteIndirect` each with three device-level command signatures (no root arguments;
  created at `Device::Init`), after the normal draw/dispatch preparation, so the call sees exactly the bindings a
  direct call would. Arguments and count are `INDIRECT_ARGUMENT` (tracked buffers transition, pages promote, dynamic
  buffers are upload memory); records beyond the buffer are clamped away, unaligned offsets and staging buffers are
  skipped with a log line; `-dxstats` counts the calls (`indirect draws`).
- Acceleration structure SRVs: `CreateAccelerationStructureView(device, tlas | null)` makes a layer SRV whose
  descriptor is a `RAYTRACING_ACCELERATION_STRUCTURE` view (location 0 for null); its D3D11 resource is a 256-byte
  untracked page buffer, so the view is never transitioned; the view holds a reference on the TLAS (released through the
  deferred releaser). Reflection maps `D3D_SIT_RTACCELERATIONSTRUCTURE` to that dimension and an unbound slot reads a
  null acceleration structure (miss everywhere; created on DXR devices only).
- Tracked buffers across submissions: buffers decay to COMMON at the end of every `ExecuteCommandLists`, which the
  immediate context's tracking did not model. `Context::Submit` now executes all queued lists in one
  `ExecuteCommandLists` (before: chunks of 32, with a decay between the chunks) and then resets every tracked buffer to
  COMMON (`Device::DecayTrackedBuffers`, a registry kept by `Buffer::Create/Destroy`). The debug layer and GBV accepted
  the stale before-states as well (switching the reset off still passed the scenario), so this follows the
  specification rather than a validation message. **For I5**: a fork/join marker splits a submission into several
  `ExecuteCommandLists`; the reset must then also run where the immediate context closes its list at a marker.
- Scenario 9 (VRS through native sections) is the `vrs` lane's `vrs_native` (passes).
- Scenarios (`scenarios_views.cpp`, against D3D11 unless layer-only): `buffer views` (raw, structured and typed UAVs
  cleared and written by CS, read as VS SRVs incl. an element-offset window, a whole-buffer null-description view and a
  declared-but-unbound structured slot), `buffer view rules` (layer-only: D3D11 refusals, dynamic/counter refusals and
  their log lines, SRV buffers committed, VB-only buffers in pages), `per-instance stream with StartInstanceLocation`
  (gpudriven 2), `UAV -> vertex buffer -> UAV` on the immediate context and in a deferred list (gpudriven 3),
  `indirect draws` (infra 10: arguments from UpdateSubresource, an immutable page and a compute shader; DispatchIndirect
  and DispatchIndirectCount), `multi-draw` (gpudriven 4: 64 records incl. zero-instance ones through both count variants
  against D3D11 drawing them one by one, with a GPU count and with maxCount only), `deferred cull + draw` (gpudriven 5:
  two deferred lists on the same buffers in one frame), `tracked buffers across submissions` (UAV, SRV and indirect
  arguments across frame boundaries, then 40 deferred lists alternating writes and reads in one submission),
  `acceleration structure SRV` (infra 12, layer-only: PS 6.5 `RayQuery` against a one-triangle TLAS built in a native
  section: hits inside with t = 1, misses outside; the empty view and an unbound slot miss everywhere).
- Gate (PLAN 3.2: layer tests + `-Gbv`): **60 passed, 0 failed** with `-HighPerformance`, `-Gbv`, `-Dxil` and `-Gbv -Dxil`
  (`qa\infra-i4-layertests`, `-gbv`, `-dxil`, `-gbv-dxil`; floor now 60; the new `Reflect` scenario of
  `scenarios_dxil.cpp` covers dxc's bare-STAT note). Build EXIT 0 (13:19, 13:23; R5 of 13:26 carries the final code).
- Stand (beyond the gate; R5 `render_check` qa_pedro, tree binaries, Gate-0 overlay, SMAA + `wind_freeze 1`, `-Tga`):
  every run clean in the log (no `!` layer line, no FATAL/refused). Everything-off
  (`-dx12_no_rt -dx12_no_vrs -dx12_no_ms -dx12_no_async -dx12_no_sm6 -no_dstorage`, PLAN 6.5)
  `infra-i4-render-pedro-alloff-g0ovl` against `core-g0-render-pedro-{a,b,c}` + the same-period Gate-0 control
  `infra-i4-ctl-pedro`: within 99.39 % (calibrated threshold 99.63 %), 2 clusters > 16 px (allowed), largest 22 px,
  PSNR 54.6 dB, p99.9 7/255 - calibrated "equal" misses on `within` only. The control itself reaches 99.48 % against
  the three quiet-machine references (it fails too, as at I0/I3), and both heat maps
  (`infra-i4-render-pedro-alloff-g0ovl\heat.png`, `infra-i4-ctl-pedro\heat.png`) show the same frame-wide single-pixel
  speckle and no structure; the tree binaries also carry the other lanes' switch-less engine changes. Default runs:
  `infra-i4-render-pedro-g0ovl` (DXC + VRS on: `vrs=1` since the vrs lane's preset landed) and
  `infra-i4-render-pedro-nosm6-g0ovl` differ from Gate 0 by those features, not by the layer (the pre-I4 tree with
  DXC, `dxc-d3b-render-pedro`, scores lower still: 99.10 %, 25 clusters).
### I5 - async compute (28.09.2026 14:26)
- `d12_async.cpp`: the compute queue (created on the plain device by the first job - a session that never takes the job
  path has none; Streamline hooks only the present queue), `crossFence`
  (direct -> compute) and `computeFence` (compute -> direct) as timelines of their own, a recorder pool (allocator + COMPUTE
  list, reused once the compute fence of its last run passed), jobs with timestamp pairs in a compute-queue query heap
  (`AsyncComputeMs` reads the last resolved time per job name with the compute queue's own frequency). `ForkAsyncCompute`
  transitions the job's textures into their entry states on the direct list, closes it and queues a fork marker;
  `JoinAsyncCompute` queues a join marker and adopts the exit states of this job and of every job forked before it (one
  compute queue runs them in order). `Context::Submit` splits the one submission at the markers: the direct batch so far,
  cross-fence signal (shared by forks with no direct work between them), compute-queue wait, the job, compute-fence
  signal; a join makes the direct queue wait for that value. `Present1` / `ForceFrameBoundary` join a forgotten job first
  (`! async job <name> not joined, joined at Present`, once), so the frame fence covers the compute work; `WaitIdle` waits
  for the compute queue too; a job ended but never forked is dropped at the frame boundary with a log line.
- Buffers decay to COMMON at every `ExecuteCommandLists` boundary: the reset of the tracked buffers (I4) also runs where the
  immediate context closes its list at a marker, and buffer uses are not transitioned at the fork (both queues promote).
- Lending: `Resource::lentTo/lentShared`. A resource a job writes is lent exclusively: any direct-queue access joins right
  there (`GuardLent` in `Require`/`RequireAll`/`RequireExact` and in `ExecuteCommandList`'s requirement patch, logged once
  with the debug name, `StatAutoJoins`). A texture the job only reads is lent read-shared in a widened read state
  (`entry | PIXEL | NON_PIXEL`, native `AsyncUse` with entry == exit read-only, and `AsyncJobFromCommandList` for a
  requirement the list never transitioned): direct reads that need no transition keep the overlap, a write or another
  state joins. A native section that touches a lent resource logs and does not join (it must join before the section).
- Compute recording (async.md 3.3): `BeginAsyncRecording(deferred)` switches a deferred context to a COMPUTE list (DIRECT
  without a compute queue) until `FinishCommandList`; allocators and spare lists are typed; draws, RTV/DSV clears and
  resolves are dropped with one `! compute recording (async job): ...` line each (queries on deferred contexts were
  ignored already); CS inputs are required as `NON_PIXEL_SHADER_RESOURCE`, and the tracker skips (logs once) a transition
  that would involve a graphics state; `StateTracker::Local::transitioned` travels into `CommandList::Requirement`.
  `AsyncJobFromCommandList` turns the finished list into a job (requirements -> uses; the list returns to its deferred
  context's pool once executed, or to a graveyard released by the compute fence when the context died).
- Fallback (`-dx12_no_async`, no compute queue): recorders and compute recordings are DIRECT lists, a fork executes the job
  in order in an `ExecuteCommandLists` call of its own, the join only adopts states. Same results, `acq=0`.
- Fix at I5's gate: a native `AsyncUse` with entry == exit == `NON_PIXEL_SHADER_RESOURCE` had put the texture into exactly
  that state, so the direct queue's next pixel-shader read (it needs `PIXEL`) joined the job; read-only uses are now widened
  like the D3D11-recorded ones (scenarios 5 and 6 failed on exactly that before).
- Gate (PLAN 3.2: scenarios 5-7 and async A-E with and without GBV; `-dxstats` unchanged with no job): layer tests
  **68 passed, 0 failed, 0 debug layer warnings** with `-HighPerformance`, `-Gbv`, `-Dxil` and `-Gbv -Dxil`
  (`qa\infra-i5-layertests`, `-gbv`, `-dxil`, `-gbv-dxil`; floor now 68). Each async scenario runs on the test device
  (compute queue) and again on a second layer device created with `-dx12_no_async`, both against D3D11: 5 `async compute
  hand-off` (PS -> native job reading a read-shared texture and writing a COMMON raw buffer -> PS; the direct read during the
  job does not join), 6 `async job not joined` (the frame boundary joins, one log line), 7 `async lent guard` (one guard join,
  logged once), A `D3D11 compute recording`, B `refusals` (draw and RTV clear dropped, one line each), C `read-shared lending`
  (read: no join; render into it: one join), D `compute-list hand-off over two frames` (UAV -> SRV -> COPY_SOURCE and
  SRV -> COPY_DEST, the march's history), E `no compute queue`. Stand `infra-i5-dxstats-pedro` (R5 `render_check`
  qa_pedro, `-dxstats`): `acq=1` in `r__caps`, the device line `* native: compute queue on (async jobs)`, `2 submissions`
  per frame (as before; no job exists in the engine until the async lane's A5), no feature-counter line (no fork, join or
  guard join), no `!` layer line.

### I6 - self-test (28.09.2026 15:30; the 9.4 A/B has no verdict yet, see the end)
- `r__native_selftest` (`xrRenderPC_R4/da_native_selftest.cpp`, both modules; R4 prints `r__native_selftest: DirectX 12
  only`). The command only asks: the test runs at the start of the next rendered frame, from a one-shot `Device.seqRender`
  entry at high priority, i.e. on the render thread before the level renders (a command arrives from a script update or
  the console at any point of the frame; the test needs the immediate context between the frame's own work). Embedded
  HLSL (five small sources `native/infra_selftest_*.hlsl`) through `da_dxc_compile_native` (cached like every native
  shader), its own 64x64 targets and buffers; afterwards it unbinds what it bound and `RCache.Invalidate()`s, so the frame
  and the following ones render as before. One line: `[qa] native selftest: immediate ok (32 lanes), deferred ok, async
  ok, bindless ok, raytracing ok, vrs ok, indirect ok, dxc 1.9.0.5402 (<ms>)` (`!` and a reason line per failed part).
- Parts: (a) immediate - a native `cs_6_0` `WaveActiveSum` dispatch into a D3D11 UAV texture (`Require`,
  `AllocateDescriptors`, `NativeDescriptor`, `EndNative(compute root | pipeline)`), then a D3D11 draw reads it: lane count
  inside the device's range, coordinates and tag exact; (b) deferred - the same section on a deferred context,
  `FinishCommandList` + `ExecuteCommandList`; (c) async - a native job (`BeginAsyncCompute`, `AsyncUse` COMMON,
  `AsyncDescriptors`) and a D3D11-recorded job (`BeginAsyncRecording` + `AsyncJobFromCommandList`, the async lane's path),
  each forked and joined, both buffers read back (`in order` without a compute queue); (d) bindless - 1024 1x1 textures
  written into 1024 slots from D3D11 SRVs (`WriteBindless`) and summed through the space100 table at
  `NativeScope::bindless`, the slots freed before the views go; (e) raytracing - a one-triangle BLAS/TLAS built in a
  native section, a `cs_6_5` RayQuery once with the TLAS as a root SRV and once through `CreateAccelerationStructureView`
  on the D3D11 path: hits (t = 1) and misses exact both ways; (f) vrs - a native graphics PSO drawn with
  `RSSetShadingRate(2x2)` through `list5` (every aligned 2x2 block uniform, blocks differ), then an engine draw after
  `EndNative(... | TouchedShadingRate)` at 1x1 (every texel its own centre); the context's VRS state is pinned to 1x1
  before the test's draws (the check is about the section's re-apply, not about the vrs lane's scopes); (g) indirect -
  `DrawIndexedIndirectCount` of four records with a count buffer of three: three strips drawn, the fourth not.
- Skips: raytracing without DXR 1.1 or without DXC SM 6.5 - decided on the device's (masked) caps, not on the rt lane's
  `rt` key (C7): the part checks the infrastructure RT builds on; vrs below tier 2; every part without DXC
  (`-dx12_no_sm6`: all test shaders are DXIL - wave ops, RayQuery).
- Found on the way: `SV_VertexID` of an indexed draw is the index value without `BaseVertexLocation`, on D3D11 and on the
  layer alike (measured; new layer scenario `indexed indirect count: records told apart by SV_VertexID`, which also covers
  a count buffer filled with initial data). The first version of part (g) aimed its records by base vertex and drew one
  strip of three (`infra-i6-selftest`, `-dxdebug`); it aims them by `StartIndexLocation` now. gd told.
- Gate (PLAN 3.2: every part ok, also with `-dxdebug`; fallback parts skipped; FPS +-1 %, passes +-0.05 ms), stand runs
  (test budget 0.1a: 4), native_check on qa_pedro, tree binaries:
  - `infra-i6-selftest-b` (`-dxdebug -dxstats`): **all seven parts ok** (327 ms incl. the first DXC compiles of the
    changed source), no debug-layer error or corruption line (the only warnings are the engine's two known sampler
    #1361), `r__caps ...rt=1;rt_tier=12;vrs_tier=2;vrs_tile=16;vrs=1;ms=1;sm=68;wave=32;fp16=1;eb=1;acq=1;dxc=1`.
  - `infra-i6-selftest-masked` (`-dx12_no_rt -dx12_no_vrs -dx12_no_ms -dx12_no_async`): `rt=0;rt_tier=0;vrs_tier=0;
    vrs_tile=0;vrs=0;ms=0;...;acq=0`, `* native: compute queue off (jobs in order on the direct queue) ... switched off:
    ...`; self-test `immediate ok, deferred ok, async ok (in order), bindless ok, raytracing skipped (no DXR), vrs skipped
    (no VRS), indirect ok`; no `!` layer line.
  - `infra-i6-selftest` / `infra-i6-selftest-dxdebug` (first version): six parts ok, indirect failed on the test's own
    expectation (above), no debug-layer error.
  - The rest of the 9.3 matrix under the budget: R4 caps `infra-i1-caps-r4` (R4 gained only the `DirectX 12 only` line
    since), `-dx12_no_sm6` masking `infra-i1-caps-masked`, upscalers `infra-i2-upscale-g0ovl` (the native wrappers are
    unchanged since I2), motion with TAA + vectors at the final gate (PLAN 6.6).
  - Layer tests after the new scenario: **77 passed, 0 failed** in all four modes (`qa\infra-i6-layertests`, `-gbv`,
    `-dxil`, `-gbv-dxil`; the other lanes' scenarios included).
- 9.4 zero-cost A/B (one DLL; A = `-dx12_no_async -dx12_no_rt`, B = everything on, both `r__async_compute st_async_off`;
  VRS, mesh and DXC identical in both): **no verdict**. Two `Invoke-Dx12Perf` batches (`infra-i6-perf-pedro`, the first
  under `qa\infra-i6-perf-pedro-try1`, broken by a rig DLL still locked by the previous run) gave 0 valid pairs: with the
  other lanes' compiles, audits and tests on the CPU the engine ran CPU-bound (GPU utilisation 38-86 %, P8 clocks in
  7 of 12 runs, no foreign GPU load), FPS 24-53 within one variant. The busy windows' timed GPU passes x window clock:
  A 20.1 / 17.0 / 22.8, B 18.5 / 21.1 / 20.9 (mean +0.9 % for B, spread +-15 % within a variant) - no difference beyond
  the noise, but no +-1 % proof either. By construction nothing runs without a job: the compute queue gets no work and no
  fence operation, the frame boundary walks an empty job list, `-dxstats` shows 2 submissions per frame as before.

### I7 - copy queue for `InitUploader` (28.09.2026 16:12: implemented; on by default since the Perf phase)
- Written under PLAN 0.1b (code first, every test in the final phase). Opt-in until the A/B decides: `-dx12_copy_queue`
  (read in the `[dx12:infra] create-info` block of `dx11HW.cpp`) sets `DeviceCreateInfo::copyQueueUploads`;
  `Device::CopyQueueInit` (Init, right after the direct uploader) creates a COPY queue, `copyFence` and a second
  `InitUploader` (`initCopy`: COPY allocators and list) and logs `* native: creation-time uploads on a copy queue`
  (the switch and its log text were changed by the decision below); a failure logs `* native: copy queue failed (...), creation-time uploads stay on the direct queue`
  and the session runs as without the switch.
- `InitUploader` is generic over (queue, fence timeline, list type): `Init(device, queue, fence, type)`; staging pages,
  allocators and kept targets follow its own fence (`initFence` for the direct one, `copyFence` for the copy one). A copy
  list records no barriers. After a copy submission the direct queue gets `Wait(copyFence, value)` inside the uploader's
  lock (the context's `Submit` flushes both uploaders first - `Device::FlushUploads` / `UploadsPending` - and blocks on
  that lock), so everything the direct queue runs afterwards, including the frame signal the releaser tags with, is
  behind the copies; compute-queue jobs follow by transitivity (their fork signal is a direct-queue signal).
- Routing (`Device::Uploader(depthStencil)`): buffers (static pages and committed view buffers) and every texture with
  initial data go to the copy queue, except textures with `ALLOW_DEPTH_STENCIL` (copy engines need not write depth
  layouts). Copy-queue textures are created in COMMON (debug layer #1425 wants COMMON on a copy queue), decay to COMMON
  after the copy and are tracked as COMMON. The placed-heap hook (`TexturePlace`, `ds`) keys "has initial data" on
  COPY_DEST, so a copy-queue texture is created under `SetStreamedTextures(true)` (restored right after): its copy
  writes every subresource before the first use, exactly the streamed case - placement stays as without the switch.
- `-dxstats` prints `* stats per 300 frames: creation-time uploads <MB> in <n> submissions on the direct queue,
  <MB> in <n> on the copy queue` whenever something was uploaded in the interval (both modes).
- Harness: `-copyqueue` (`Run-LayerTests.ps1 -CopyQueue`) runs the whole suite and the benchmark process with the copy
  queue; new scenario 13 `creation-time uploads from 3 loader threads while frames are submitted` (9.1) in both modes.
- Build: `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` EXIT 0 (16:12; the touched objects recompiled under ninja.lock
  without a warning), `Build-LayerTests.ps1` built without a diagnostic. Nothing ran (0.1b).
- Final test phase (budget 0.1a): (1) layer tests `-HighPerformance -NoBench` in the default mode and with `-CopyQueue`
  (plus `-Gbv -CopyQueue`): every scenario passes in both, scenario 13 included, 0 debug-layer errors (watch #1425 and
  barrier-type messages); (2) the load A/B of 9.3 (5 interleaved `texture_load` pairs on `qa_pedro`, DirectStorage on,
  A = `-r5 -always_active`, B = A + `-dx12_copy_queue`). **Rule**: keep only if B's median load wall (the loading phases
  up to `Final texture upload`) is lower than A's and B is faster in at least 4 of the 5 pairs; then the copy queue
  becomes unconditional (`CopyQueueInit` without the switch, the `copyQueueUploads` field, the `dx11HW.cpp` line and the
  harness `-copyqueue` go; the direct uploader stays for depth-stencil data), else remove it (`initCopy`, `copyQueue`,
  `copyFence`, `Uploader`, `CopyQueueInit/Shutdown`, the field, the switch, the harness mode and scenario 13's copy
  branch; `InitUploader` keeps its generic `Init`). Either way no `-dx12_copy_queue` remains (C23); numbers into this
  record and PLAN 8.
- **Decision (Perf phase, one pass instead of the 5 pairs)**: `final-onepass-foliage` (defaults, direct queue)
  against `final-onepass-variants-foliage` (the same with `-dx12_copy_queue` and the GD/grass variants of
  gpudriven.md and mesh.md; 2560x1440 Extreme TAA, DirectStorage on): level.geom 256 -> 214 ms, level.geomx
  58 -> 47 ms, the early texture batch's create 611 -> 548 ms, 616 MB of creation-time uploads moved off the direct
  queue, no error line; the `[texload]` batch's wall 678 -> 675 ms. **The copy queue is the default**:
  `DeviceCreateInfo::copyQueueUploads` defaults to true, `dx11HW.cpp` reads the mask `-dx12_no_copy_queue` (the `-dx12_no_rebar` pattern) instead of the
  opt-in, the device logs `* native: creation-time uploads on a copy queue` or `... on the direct queue
  (-dx12_no_copy_queue)`; the harness runs the copy queue by default and `-nocopyqueue` (`Run-LayerTests.ps1
  -NoCopyQueue`) runs the direct mode. The sync was reviewed unchanged: every copy submission is followed under the
  uploader's lock by the direct queue's `Wait(copyFence)`, `Context::Submit` flushes both uploaders before its first
  `ExecuteCommandLists`, and a compute-queue fork waits on a direct-queue signal made after that wait (each `Submit`
  starts with a stale cross value). The mask stays for fallback runs, like the other `-dx12_no_*` masks.

### Review fixes of the final test phase (28.09.2026 18:25; code + build, untested)
Every finding was checked against the code first; all eight are real (one claim of L-ASYNC-2 was not).
- L-ASYNC-1 (`ExecuteCommandList` guards): two passes now. Pass 1 checks the loans only: a requirement the list
  transitions joins its job (`JoinLent`), the others keep the exact read-shared check (`GuardLent`). Pass 2 then
  requires every entry state and flushes, so each entry transition sits after the last join marker and buffers decayed
  by a join get their COMMON -> first barrier in the same `ExecuteCommandLists` as the list. New layer scenario
  `async F` (deferred list samples a read-shared texture, then renders into it: one auto join).
- L-ASYNC-2 (emergency frame end): `Submit` skips its budget-driven `EndFrame` while `Device::AsyncJobsPending()` (a
  job recording, closed or forked and unjoined); the next submission without one, or Present, ends the frame.
  `ForceFrameBoundary` is not dead: the layer test harness ends its frames with it (`main.cpp` `EndFrame`); it stays,
  with its comment corrected (frame boundary without a Present, joins every job). Joining at the emergency point was
  rejected: `AsyncFrameBoundary` also expires the caller's job pointers, which is wrong in the middle of a frame.
- L-ASYNC-3 (threads, frames): the contract is narrowed (`d12_api.h`): every job call on the render thread, a job begun,
  recorded, forked and joined within one frame (the engine's callers, `da_async.cpp` and `da_native_selftest.cpp`,
  already are). `AsyncJob::frame` is stored at `BeginAsyncCompute` / `AsyncJobFromCommandList`; a job forked in a later
  frame is dropped with one `!` line instead of reading recycled descriptors and upload pages, and counts as joined (the
  caller's join or Present recycles it, so the pointer is not reused under the caller).
- L-NATIVE-4: `ExecuteCommandList` of a COMPUTE list (a compute recording) runs it as a job forked and joined at once
  (logged once) instead of queueing it on the direct queue; new layer scenario `async G`. `ForkJob` / `JoinJob` inside a
  native section log once and do nothing (a forgotten join is then made at Present).
- L-ASYNC-5: documentation only (`d12_api.h`, 3.3 table): entry == exit in a read state declares a read-only use (no
  barrier, no write inside the job); a job that writes declares the write state it leaves the resource in as `exit`.
  An explicit read-only flag was rejected: it changes the frozen API (I0) for no engine caller.
- L-RULES-6: the four pointer-to-`nullptr` comparisons use the implicit bool conversion (`IsBackBufferProxy`,
  `IsTracked`, `HasOpenList`, `ProgramType`).
- L-CAPS-7: the caps query, the DXC load with its pipeline self-check and the mask now run right after
  `DeviceCreatedHook` (Streamline's `slSetD3DDevice` sees the device before anything is created on it).
- RT-5: `WriteBindlessBuffer`'s raw view covers `(size + 3) / 4` words (every allocation is 256-byte aligned), so the
  last index of a 16-bit index buffer with an odd count is readable.
- Build: `Invoke-Ninja xrD3D12` EXIT 0 (all twelve layer objects recompiled on purpose, no warning), `xrRender_R5.dll`
  relinked with that library (18:27), `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` EXIT 0 (no work left);
  `Build-LayerTests.ps1` built without a diagnostic. Nothing ran. The layer-test floor gains `async F` and `async G`.

### Final stand run (28.09.2026 18:38-20:03; `_work\dx12-features-20260928\final\stand.md`, `stand-fixes.md`)
- Layer tests `-HighPerformance`: 80/0 on the tree as the fix phase left it, 81/0 after the fixes below (0 debug-layer
  warnings; the two D3D11-reference input-layout errors #160/#163 are the negative scenario's own), `-Gbv -Filter
  async` 11/0.
- **S1, read-only requirements of a deferred list** (`d12_context_ops.cpp` `ExecuteCommandList`): the march job
  lends `$user$cloud_map` read-shared in NON_PIXEL | PIXEL; the sun cascade's deferred list samples it as PIXEL and
  the exact entry state made the layer transition it under the job and join first (`joined early`, 47-150 of 300
  frames in the storm, every frame in clear weather). A requirement the list did not transition and that is
  read-only now guards its loan and requires its state with `Satisfies` (a state holding its read bits), and the
  tracked state is not overwritten by the list's equal last state; transitioned requirements keep the exact entry
  and the join. New scenario `async H` (a deferred list that only samples a read-shared texture joins nothing, a
  later render into it joins once). Re-check `final-async-verify-b`: 0 guard joins, verify port/schedule 0.
- Diagnostics: `DebugName` names an unnamed resource by its size, format and bind flags (how the cloud map was
  found); `BeginAsyncRecording` tells "still recording a job" from "has recorded work".
- Two engine-backend defects the layer surfaced (core code, `XR_D3D12` only, stand-fixes S2/S4): stale shader stages
  after `CBackend::Invalidate` gave D3D12 unlinkable pipelines (refused, draws skipped); GPU markers of pooled
  backends drawing on the immediate context went into a deferred list nobody executes (with `-dxdebug` the layer
  records them: the list grew and blocked the async jobs).

### Final test phase, performance (Perf phase, 28.09.2026)
At the user's request the Perf phase ran one measurement pass instead of on/off batches (PLAN 8.4).
- 9.4 zero-cost (a masked device against every capability present and every feature off): read from the masked
  variants of the async and VRS batches (qa_pedro, 2560x1440 Extreme TAA; A = `-dx12_no_async -dx12_no_rt` with
  `r__async_compute st_async_off`, B = the same without the masks). Every timed pass is equal within +-0.05 ms (the
  I6 criterion); FPS reads +2.7 % for the masked device in `perf-async-pedro` (2 valid pairs, `frame` span -0.54 ms
  outside the timed passes, CPU main -0.12 ms) and +1.2 % in `perf-vrs-pedro` (1 valid pair, `frame` -0.08 ms). The
  gap lies in the frame span, not in GPU work, on a CPU-bound stand frame; it was not investigated further.
  Follow-up audit (not re-measured): the two variants' D3D12 command streams are equal (draws, PSO sets, root CBVs,
  tables, barriers, 2 submissions a frame, within run-to-run noise), no per-frame code of the RT or async lane does
  more than an early return while its feature is off, and the pairs overlap (`off-2` 55.8 against `masked-2` 55.7).
  What availability did cost is now paid only on use: the compute queue and its fences are made by the first job,
  the RT pass pipelines with the targets of the first effect switched on (the RT device part took 319-368 ms of every
  start, 11 of its 12 pipelines are the passes; "Renderer resources created" 1140-1217 ms against 770-781 ms masked),
  the level walk (14 ms, ~11 k items) at load only
  with an effect on, and the `join` timestamp pair only on the job path.
- I7: creation-time uploads on the copy queue by default (level.geom 256 -> 214 ms, level.geomx 58 -> 47 ms, the
  early texture batch's create 611 -> 548 ms, 616 MB of creation-time uploads moved off the direct queue, no error
  line); `-dx12_no_copy_queue` keeps them on the direct queue.
- One pass with everything on (`final-onepass-foliage`, 2560x1440 Extreme TAA): 49 FPS, GPU frame 20.2 ms,
  `-dxstats` 182 forks and 182 joins per 300 frames (0 by the guard), no D3D12 error or corruption line; the
  device line reports `root signature 1.2, runtime SDK 616` (no Agility SDK shipped).
