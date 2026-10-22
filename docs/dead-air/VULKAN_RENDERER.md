# Vulkan renderer (R6, 1.6.0)

Status: shipped in the 1.6.0 tree (16.10.2026). The third renderer module next to DirectX 11 (R4) and
DirectX 12 (R5, `DX12_RENDERER.md`); same render code, same shaders, same look.

## 1. Decisions

- **One render code, three modules.** `xrRender_R6.dll` compiles `xrRenderPC_R4/sources.cmake` like R4
  and R5, with `XR_VULKAN` and `XR_NATIVE`. It links `xrVulkan` (`src/Layers/xrRenderVK`), a Vulkan 1.3
  backend that implements the D3D11 interfaces the renderer uses: devices, contexts (immediate and
  deferred), resources and views, state objects, queries, the swap chain. Every effect that exists on
  DirectX 11 exists on Vulkan by construction. `vulkan-1.dll` is loaded at runtime, so the module loads
  where Vulkan is absent.
- **Menu choice "Рендер: DirectX 11 / DirectX 12 / Vulkan"** (`renderer renderer_r6`, `-r6`), applied at
  the next start. `xrvk::ProbeAdapter` decides whether the adapter can run it (Vulkan 1.3 with dynamic
  rendering, synchronization2, timeline semaphores, format-less storage images and the D3D11 feature
  level 11_1 core features). A device that fails at start falls back to DirectX 11 for the next start
  (`xrRender_vulkan_failed`).
- **Upscalers** as on the other renderers: NVIDIA DLSS / DLAA through Streamline's Vulkan path, AMD FSR
  3.1 through the signed Vulkan runtime of FidelityFX SDK 1.1.4 (`Externals/fidelityfx-vk`), the
  engine's TAA otherwise. **AMD FSR frame generation** through the same runtime (section 7).
- **Ray tracing** (the RTX section: reflections, sun shadows, occlusion) as on DirectX 12, from the same scene
  and pass code (section 8).
- **NVIDIA Reflex** and its PC latency stats through Streamline's Vulkan path (`VK_NV_low_latency2`, section 9).
- Not on Vulkan: NVIDIA DLSS frame generation and async compute (section 9). Their settings are kept for the
  renderers that have them.

## 2. xrVulkan architecture (details in `vk_layer.h`)

- **Video memory full.** A device resource the driver refuses (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) goes to a system-memory
  type its requirements allow (`MemoryAllocator::SystemTypeFor`), read by the GPU over the bus, as a D3D11 driver pages
  out by itself; the heap is marked full until memory of it goes back, so the driver is not asked again for every
  allocation (1.6.2: a 2 GB card failed the level load, then the render targets).

- **Device and queues.** One graphics queue, an async compute queue and a transfer queue where the
  family exists (`-vk_no_async`, `-vk_no_transfer`), and two more graphics-family queues for FSR frame
  generation's presenter. Creation-time uploads go through the transfer queue; the engine's async compute
  passes (`da_async.cpp`) run on DirectX 12 only so far.
- **Frames.** One timeline semaphore; three frames in flight (`kFramesInFlight`). Pools, upload pages,
  dynamic buffer versions and deferred releases are tagged with the frame and recycled when its value
  completed.
- **Command recording.** The immediate context records one command buffer, submitted at the engine's
  event query (its throttle point, so the GPU gets the frame's start early) and at Present. Deferred
  contexts record their own command buffers on the renderer's worker threads (sun cascades, rain, HUD
  shadow); `ExecuteCommandList` stitches them in with the layout requirements they collected.
- **State.** Dynamic rendering (a target change ends the rendering, clears fold into load operations),
  extended dynamic state 1-3 and dynamic vertex input where present (`-vk_no_eds3`,
  `-vk_no_vertex_input`), so the pipeline key is the shaders, the attachment formats and the topology
  class. Pipelines are built through graphics pipeline libraries with fast linking (`-vk_no_gpl` for
  whole pipelines) and persist in `appdata/shaders_cache_grizzly/r6/pipelines.bin`. A context looks its
  layouts and pipelines up in small direct-mapped tables of its own (the device's caches take a lock), and a
  description equal to the bound pipeline's - shaders, state objects and their contents - needs no lookup.
  A rate image going away keeps the rendering (it stays attached, the combiner ignores it): a lamp's
  stencil mask and its light pass used to end and begin one twice per lamp.
- **Bindings.** One descriptor set per shader stage, written through an update template; constant
  buffers are dynamic uniform buffers, so a new version of a buffer only moves an offset. Sets with the
  same contents are reused across frames: a context keeps them by content until a view, sampler, buffer or
  acceleration structure is released (`Device::descriptorEpoch`) or 16384 are kept; their pools then wait for
  the frames that bound them and are reset for the next sets. Null descriptors (`VK_EXT_robustness2`) stand
  in for unbound slots.
- **Hazards.** Per-subresource layout and access tracking (synchronization2). Each barrier carries the
  last writer's stages as its source and the consumer's as its destination; buffer hazards of a batch
  merge into one memory barrier. A barrier inside a rendering ends it (LOAD keeps the contents).
- **Queries.** Occlusion and timestamp results are read as soon as the GPU wrote them (availability),
  not when their submission ends; an event query completes with its own submission.
- **Memory.** Own allocator with dedicated allocations for render targets; dynamic data in resizable
  BAR memory where available (`-vk_no_rebar`).
- **Native API (`vk_api.h`).** Indirect draws with a GPU count, mesh shaders, variable rate shading,
  the UAV overlap hint, persistent staging pointers, video memory budget, and the image/command buffer
  handoff an SDK records on (`PrepareNativeImage`, `BeginNative`, `EndNative`, `GetNativeDevice`).

## 3. Shaders

- **One source, two binaries.** Every shader compiles through DXC twice from the same text and defines:
  DXIL (its reflection gives the signature, the constant layout and the registers) and SPIR-V
  (`-spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout`, D3D's base vertex/instance and
  `SV_Position.w`, register shifts per class). They travel together in one container; the layer matches
  SPIR-V variables to registers by name (`vk_spirv.cpp`) and patches the bindings.
- **`GRIZZLY_SPIRV`** is defined for the SPIR-V compile only (the DXIL twin keeps the reflection without
  SPIR-V-only declarations); `GRIZZLY_VULKAN` and `GRIZZLY_DXC` for both.
- **What SPIR-V does worse than DXIL, and how the shaders avoid it** (measured on the pinned scene of
  section 9):
  - A byte address buffer's `Load4` / `Store4` becomes four 32-bit accesses. `da_raw_buffer.h` gives the
    same storage buffer a `uint4` view (`<name>_v4`, bound to `<name>`'s register by the layer) for the
    16-byte records: the grass generation 2.25 -> 2.13 ms.
  - DXC's SPIR-V path does not unroll a loop with constant bounds on its own (FXC and DXIL do). Hot loops
    carry `[unroll]`: the extreme shadow filter summed its 11x11 weight table per pixel in a loop
    (sun 3.16 -> 2.90 ms).
- RWTexture formats: storage images take the view's format (`-fspv-use-unknown-image-format`); a shader
  with image atomics compiles again with its HLSL types' formats.

## 4. Presentation

- The frame renders into a layer texture behind `GetBuffer(0)`; Present submits the frame, acquires a
  swap chain image and copies into it, so only that copy waits for the display.
- Three images. Vsync (`rs_vsync`): FIFO for On, FIFO_RELAXED for Adaptive where the surface has it,
  IMMEDIATE (else MAILBOX) for Off; `VK_EXT_swapchain_maintenance1` switches between FIFO and IMMEDIATE
  per present without a new chain (FIFO_RELAXED switches with nothing, so a change recreates it).
- Pacing (`src/Layers/xrRender/present_pacing.h`, shared with DXGI): with `VK_KHR_present_wait` a vsync'd
  present waits for the previous present to reach the display while frames keep up with the refresh, for
  the one before it while they come slower (`DX12_RENDERER.md`, vsync pacing).

## 5. Upscalers

- **DLSS** (`da_upscaler_dlss.cpp`, `da_streamline.cpp`): Streamline's Vulkan integration by manual
  hooking. Before the instance exists `slGetFeatureRequirements` gives the extensions, the 1.2/1.3
  features by name and the extra queues (`xrvk::ExternalRequirements`); after the device
  `slSetVulkanInfo`; the swap chain calls and image creation go through `sl.interposer.dll`'s exports.
  Resources are tagged as image, memory, view and layout. The reactive mask is R16F on Vulkan
  (Streamline's Vulkan format table has no R8_UNORM).
- **FSR 3.1** (`da_upscaler_fsr_vk.cpp`): `amd_fidelityfx_vk.dll` by absolute path, ffx-api upscale
  context on the layer's device, dispatched on the immediate command buffer between `BeginNative` and
  `EndNative`. ffx-api resolves extension aliases of core entry points (`vkGetBufferMemoryRequirements2KHR`)
  that a 1.3 device without those extensions answers null for, so it gets a resolver that falls back to
  the core names and stubs the debug labels.

## 6. Console and launch switches

| Switch | Effect |
| --- | --- |
| `-r6` | start on Vulkan (the menu's choice is saved as `renderer renderer_r6`) |
| `-vkdebug` | validation layer (Vulkan SDK), debug names, ffx-api debug checking |
| `-vksync` | synchronization validation as well |
| `-vkstats` | the layer's work counters per frame and per engine scope, target creation flags |
| `-vklabels` | debug labels for capture tools |
| `-vk_no_async`, `-vk_no_transfer`, `-vk_no_eds3`, `-vk_no_vertex_input`, `-vk_no_rebar`, `-vk_no_vrs`, `-vk_no_ms`, `-vk_no_gpl`, `-vk_no_multidraw`, `-vk_no_d24` | switch a capability off to test its fallback |
| `-vsync_strict` | every vsync'd present waits for the previous one (all APIs; for pacing comparisons) |
| `r__gpu_capture` | a RenderDoc frame capture on any API |

## 7. Frame generation

AMD FSR frame generation (`r__framegen st_fg_fsr`, `da_framegen_fsr_vk.cpp`), the manager shared with DirectX 12
(`da_framegen.cpp`):
- **The swap chain.** FFX's frame interpolation swap chain takes the place of the layer's VkSwapchainKHR through
  `xrvk::SwapChainProvider`: the layer keeps the surface, the back buffer and its copy into the acquired image;
  FFX creates the chain, lists its images, acquires and presents, a generated frame between every two rendered
  ones, from its presenter thread on two graphics-family queues the device keeps for it
  (`DeviceCreateInfo::presentQueues`). FFX submits to the layer's graphics and compute queues under the layer's
  locks (`xrvk::SubmitToQueue`). The UI composition runs on the present queue.
- **One context, several chains.** A resize or a vsync change destroys the chain and makes the next one under
  the same swap chain context (FFX holds one chain at a time); only the chain that ends frame generation takes
  the context with it.
- **Streamline.** The runtime makes and presents its real chain through its own imports of `vulkan-1.dll`; where
  Streamline runs they are pointed at its interposer (`da_ffx_vk_route_swapchain`), so its per-frame bookkeeping
  sees every present and DLSS upscaling runs under the generation (the DirectX 12 twin makes FFX's chain through
  Streamline's factory).
- **Per frame:** configure, then prepare on the immediate command buffer (depth, motion vectors, the camera); the
  generation dispatch runs inside the present, through the swap chain's callback. The UI is told apart from the
  scene by the hudless copy. SDK 1.1.4 takes a reset as a frame id that does not step by one.
- Measured (RTX 3070 Laptop, DLSS Balanced, vsync): 1213 presents for the first 600 generating frames; 52-53
  rendered frames per second with the generation against 61.6 without it, so about 105 on the screen.
- **The presenter spins a core.** SDK 1.1.4's Vulkan presenter waits out its pacing on the performance counter
  (62-72 % of a core on the stand), and its pacing tuning has no hybrid wait: the key-value configure even
  reports an error (its switch breaks before `return FFX_OK`). The DirectX 12 runtime (FSR SDK 2.x) takes the
  hybrid wait and the fence waits (`da_framegen_fsr.cpp`): its presenter and interpolation threads went from
  2.46 to 1.50 cores busy.

## 8. Ray tracing

The RTX section of DirectX 12 (`docs/dead-air/dx12-features/dxr.md`): sun shadows, ambient occlusion, reflections
on water and puddles and of glossy surfaces (dxr.md 3.9, after DOOM Eternal), moving objects and pre-skinned characters, traced by inline ray queries in compute passes
(`VK_KHR_ray_query`, `VK_KHR_acceleration_structure`).
- **One scene and pass code for both APIs.** `da_rt_scene.cpp` and `da_rt_passes.cpp` work through the lane's
  objects (`da_rt_native.cpp`, `da_rt_device.h`): native buffers, acceleration structures, builds with their
  compacted sizes, compacting copies, bindless slots, upload memory by address. DirectX 12 runs them on
  xrD3D12's native resources, Vulkan on xrVulkan's (`vk_raytracing.cpp`).
- **Binding model.** The passes are compute dispatches of the D3D11 path, so the layer orders their inputs and
  outputs like any other dispatch: inputs t0-t15, the scene at t16 (an SRV over the top level's storage),
  outputs u0-u7, constants b0, the root signature's static samplers as sampler states s0-s3. What D3D11 has no
  word for: the device's bindless set (descriptor indexing with updates after bind, 2D textures and raw buffers
  in one index space), which the shaders declare at `[[vk::binding(n, 7)]]` and the layer moves to set 1 of the
  compute layout; the geometry and instance tables and the skinning source, palette and output, read and
  written by device address from a push-constant block (`GRIZZLY_SPIRV` in `rt/rt_common.hlsli`,
  `vk::RawBufferLoad` / `RawBufferStore`).
- **Synchronization.** What the passes read by address or through the bindless set the layer cannot see:
  builds, compacting copies and the pre-skinning write are ordered by full barriers (`xrvk::AccelBarrier`), the
  scene's textures go to their read layout when they changed (`xrvk::RequireRead`, `TextureWriteGeneration`).
  A section takes the frame's render targets off while it runs and puts them back: the passes read the depth the
  frame still has bound as its depth target, and D3D11's rules read an input bound for output as null (the
  occlusion then took every pixel for sky and traced nothing; the native DirectX 12 section never sees the rule).
- **Objects.** Every acceleration structure in a buffer of its own (256-byte aligned, as the scratch), compacted
  sizes through a query pool, the empty scene a built top level without instances, frame data (instances,
  per-frame tables, bone palettes) in the context's upload pages by address. Vertex and index buffers carry the
  build-input and storage usages, so the level's own buffers feed the builds and the bindless slots as they are.
- **Requirements:** ray query with acceleration structures, buffer device address, 64-bit integers, descriptor
  indexing with updates after bind (partially bound, non-uniform indexing, 4096 slots at least); the bindless
  set holds up to 65536 of each kind. `-dx12_no_rt` switches ray tracing off on both APIs.
- **Parity with DirectX 12** (`r__rt_debug` views of the same frame, mean difference): sun visibility 0.9/255,
  occlusion 2/255 (1.5/255 with a character in front of the camera, its self-occlusion included), reflections on
  the marsh 7/255 (the ripples' timing). Validation (`-vkdebug -vksync`) reports nothing of the lane.

## 9. Reflex, and what Vulkan does not have

- **Reflex.** Streamline loads `sl.reflex` and `sl.pcl` on Vulkan too, and the device asks for what they require
  (`VK_NV_low_latency2`, merged with DLSS's requirements). The layer's swap chain calls go through Streamline's
  interposer (the `deviceProc` of the device), so the Reflex plugin puts the latency mode on every chain
  (`VkSwapchainLatencyCreateInfoNV`) and sleeps the frame on it; the presents carry the frame token's id
  (`xrvk::SetNextPresentId`), so the latency markers name the frames they belong to. The low latency mode is
  what DLSS frame generation turns on on DirectX 12; on Vulkan the sleep and the stats run with it off.
- **DLSS frame generation** is not wired on Vulkan: the stand's GPU (RTX 3070, Ampere) cannot run it, so no
  Vulkan path for it could be verified. AMD FSR frame generation runs on every GPU (section 7).
- **Async compute** (`r__async_compute`) stays on DirectX 12 (`da_async.cpp`): its auto rule picks the pixel
  passes on NVIDIA (a loss on Ampere, `dx12-features/async.md`), and the AMD and Intel GPUs it helps are not on
  the stand to verify a Vulkan path across two queues.

## 10. Measured (16.10.2026, RTX 3070 Laptop, 2560x1440 Extreme, DLSS Balanced)

Scene: `qa160 - quicksave7`, July noon, wind frozen, weather pinned to `af3_slight_clear_steady_3`
(`level.set_weather(..., true)` - an unpinned save loads a random weather, and a sunless one takes 4 ms
off the GPU frame), vsync off, 10 s of FPS; noise between runs about 1.5 FPS. All three are GPU-bound
(utilisation about 99 %).

| | DirectX 11 | Vulkan | DirectX 12 (RT off) | DirectX 12 (RT on) |
| --- | ---: | ---: | ---: | ---: |
| FPS | 62.5 | 58-59 | 57.9 | 55.6 |

Ray tracing on Vulkan (17.10.2026, the same scene with every RTX effect on at High; the laptop ran in another
power state that day, every API about a fifth slower than above): Vulkan 44.7 FPS with ray tracing, 46.4 without
it, DirectX 12 44.4 with it. On the marsh (`bolota`, reflections on the water): Vulkan 33.5, DirectX 12 32.3. GPU
passes on Vulkan: scene 0.28 ms, sun 0.6, occlusion 0.75, reflections 1.6-2.1 (DirectX 12: 0.42, 0.9, 0.86, 2.2
on the marsh).

Where Vulkan's GPU frame differs from DirectX 11's (engine timers, ms): grass generation +0.55 (SPIR-V
code of the compute shader), local lights +0.2 (a rendering ends for every spot light's shadow map and
starts again for its accumulation), VRS rate image +0.14 (DirectX 11 has no VRS); sun -0.2, shrubs -0.19,
post -0.13. The CPU is not the limit in this scene: the game thread spends about 2 ms more in rendering
than on DirectX 11 (the layer's preparation and the driver's recording), hidden behind the GPU.

Where the CPU is the limit (18.10.2026, `light_test`: the Jupiter hall at night, a hundred lamps, frame
generation off, 20 s after the load so the spawning settled, each pair in one sitting):

| | DirectX 11 | Vulkan |
| --- | ---: | ---: |
| before | 46.6 | 35.8 |
| after | 49.6 | 38.3 |

The game thread's rendering on Vulkan costs about 5 ms a frame more than on DirectX 11, the layer's own work
and the NVIDIA driver's in equal parts; what stays is the driver's share of about 1000 pipeline binds, 90
renderings and 2100 draws a frame. Both APIs gained from the environment's sky and env cubes, which made 28
shader resource views a frame (`CTexture::surface_set` of the same surface), and the exposure pair's views
made every frame (they now swap with their surfaces). With frame generation under vsync the stand rendered 31
frames a second on Vulkan against 37.6 without it (measured before these changes, right after the load).
