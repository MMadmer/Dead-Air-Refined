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
  engine's TAA otherwise.
- Not on Vulkan yet: frame generation, the ray-traced effects of the RTX section and NVIDIA Reflex
  (sections 7 and 8). Their settings are kept for the renderers that have them.

## 2. xrVulkan architecture (details in `vk_layer.h`)

- **Device and queues.** One graphics queue, an async compute queue and a transfer queue where the
  family exists (`-vk_no_async`, `-vk_no_transfer`). Async compute follows `r__async_compute` exactly as
  on DirectX 12 (`da_async.cpp`; Auto keeps it off on NVIDIA). Creation-time uploads go through the
  transfer queue.
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
  whole pipelines) and persist in `appdata/shaders_cache_grizzly/r6/pipelines.bin`.
- **Bindings.** One descriptor set per shader stage, written through an update template; constant
  buffers are dynamic uniform buffers, so a new version of a buffer only moves an offset. Sets with the
  same contents are reused within a frame. Null descriptors (`VK_EXT_robustness2`) stand in for unbound
  slots.
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

Not implemented on Vulkan: `r__framegen` reports "frame generation needs DirectX 12" and stays off.

## 8. Ray tracing and Reflex

The RTX section (`r__rt_reflections`, `r__rt_shadows`, `r__rtao`) runs on DirectX 12 only
(`da_rt_device.cpp`); the device reports ray query support, the passes are not ported. Streamline is
asked for DLSS only on Vulkan, so Reflex (`VK_NV_low_latency2`) is not wired.

## 9. Measured (16.10.2026, RTX 3070 Laptop, 2560x1440 Extreme, DLSS Balanced)

Scene: `qa160 - quicksave7`, July noon, wind frozen, weather pinned to `af3_slight_clear_steady_3`
(`level.set_weather(..., true)` - an unpinned save loads a random weather, and a sunless one takes 4 ms
off the GPU frame), vsync off, 10 s of FPS; noise between runs about 1.5 FPS. All three are GPU-bound
(utilisation about 99 %).

| | DirectX 11 | Vulkan | DirectX 12 (RT off) | DirectX 12 (RT on) |
| --- | ---: | ---: | ---: | ---: |
| FPS | 62.5 | 58-59 | 57.9 | 55.6 |

Where Vulkan's GPU frame differs from DirectX 11's (engine timers, ms): grass generation +0.55 (SPIR-V
code of the compute shader), local lights +0.2 (a rendering ends for every spot light's shadow map and
starts again for its accumulation), VRS rate image +0.14 (DirectX 11 has no VRS); sun -0.2, shrubs -0.19,
post -0.13. The CPU is not the limit in this scene: the game thread spends about 2 ms more in rendering
than on DirectX 11 (the layer's preparation and the driver's recording), hidden behind the GPU.
