// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// xrVulkan: a native Vulkan backend behind the Direct3D 11 interfaces the renderer is written against
// (docs/dead-air/VULKAN_RENDERER.md). The renderer module built with it (xrRender_R6) runs the same render
// code as the DX11 and DX12 modules; every command becomes Vulkan work - pipelines with the state the
// device lets be dynamic, descriptor sets written per stage, batched synchronization2 barriers, dynamic
// rendering begun lazily with clears folded into load operations, and real parallel command buffers for
// the deferred contexts. Shaders are SPIR-V compiled by DXC from the same HLSL.
//
// The library depends on the Windows SDK, the Vulkan headers and the C++ standard library only.
#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>
#include <vector>

namespace xrvk
{
// ---- creation ---------------------------------------------------------------------------------

using LogSink = void (*)(const char* line);

// Capabilities a run switches off (DeviceCreateInfo::disabledFeatures); every fallback can then be
// tested on hardware that has everything.
enum NativeFeature : UINT
{
    NativeFeatureAsyncCompute = 1u << 0,   // -vk_no_async: one queue for everything
    NativeFeatureTransferQueue = 1u << 1,  // -vk_no_transfer: creation-time uploads on the graphics queue
    NativeFeatureDynamicState3 = 1u << 2,  // -vk_no_eds3: blend and polygon state in the pipeline key
    NativeFeatureVertexInput = 1u << 3,    // -vk_no_vertex_input: input layouts in the pipeline key
    NativeFeatureRebar = 1u << 4,          // -vk_no_rebar: dynamic data in system memory
    NativeFeatureShadingRate = 1u << 5,    // -vk_no_vrs
    NativeFeatureMeshShaders = 1u << 6,    // -vk_no_ms
    NativeFeaturePipelineLibrary = 1u << 7, // -vk_no_gpl: whole pipelines only
    NativeFeatureMultiDraw = 1u << 8,      // -vk_no_multidraw
    NativeFeatureDepth24 = 1u << 9,        // -vk_no_d24: D32S8 for every D24S8 depth
};

// What an SDK sharing the device (NVIDIA Streamline) needs the instance and the device to have. Features are
// member names of VkPhysicalDeviceVulkan12Features / VkPhysicalDeviceVulkan13Features; what the device lacks is
// left out with a log line.
struct ExternalRequirements
{
    std::vector<const char*> instanceExtensions, deviceExtensions, features12, features13;
    UINT graphicsQueues = 0, computeQueues = 0; // extra queues the SDK records on
};

// The native objects of the device, for an SDK that records on them (Vulkan handles as pointers).
struct NativeDeviceInfo
{
    void* instance = nullptr;       // VkInstance
    void* physicalDevice = nullptr; // VkPhysicalDevice
    void* device = nullptr;         // VkDevice
    // The first of the extra queues made for the SDK (past the layer's own in the same family).
    UINT graphicsFamily = 0, graphicsQueueIndex = 0;
    UINT computeFamily = 0, computeQueueIndex = 0;
    // The graphics-family queues kept for an SDK's presenter (DeviceCreateInfo::presentQueues), past the SDK's.
    UINT presentQueueIndex = 0, presentQueueCount = 0;
};

struct DeviceCreateInfo
{
    LUID adapterLuid{};             // the adapter the engine selected through DXGI; zero = the first discrete GPU
    IDXGIAdapter1* adapter = nullptr; // handed back by IDXGIDevice::GetAdapter (ImGui walks it)
    bool validation = false;        // -vkdebug: VK_LAYER_KHRONOS_validation, messages go to the log
    bool syncValidation = false;    // -vksync: synchronization validation as well (slow)
    bool stats = false;             // -vkstats: the layer's per-frame work counters in the log
    bool labels = false;            // debug labels for capture tools (RenderDoc, Nsight); implied by validation
    const wchar_t* pipelineCachePath = nullptr; // file of the persistent pipeline cache, may be null
    LogSink log = nullptr;          // receives complete lines, no trailing newline
    UINT disabledFeatures = 0;      // NativeFeature bits
    // An SDK sharing the device: asked for its requirements before the instance exists, told the native
    // objects right after the device does (null = none).
    void (*externalRequirements)(ExternalRequirements& out) = nullptr;
    void (*deviceCreated)(const NativeDeviceInfo& device) = nullptr;
    // The SDK's own entry points the layer calls instead of the driver's (by name, asked after deviceCreated):
    // the swap chain calls, so it sees every present, and image creation, so it knows the images it is
    // handed. Null result = the driver's.
    void* (*deviceProc)(const char* name) = nullptr;
    // Graphics-family queues nobody else submits to, for a presenter thread (AMD FSR frame generation presents
    // and acquires on its own queues). As many as the family has.
    UINT presentQueues = 0;
};

// Creates the Vulkan device and returns it behind ID3D11Device / ID3D11DeviceContext (immediate). The
// reported feature level is 11_1.
HRESULT CreateDevice(const DeviceCreateInfo& info, ID3D11Device** device, ID3D11DeviceContext** immediate,
    D3D_FEATURE_LEVEL* featureLevel);

// A swap chain of the window behind IDXGISwapChain1. Any description is accepted; GetBuffer(0) always
// means "the current back buffer". `factory` may be null (it is only handed back by GetParent).
HRESULT CreateSwapChainForHwnd(ID3D11Device* device, IDXGIFactory2* factory, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGISwapChain1** swapChain);

// ---- a swap chain an SDK makes (AMD FSR frame generation) -------------------------------------------

// Takes the place of the VkSwapchainKHR calls (handles and structures as pointers). The layer keeps the surface,
// the back buffer and the copy into the acquired image; the provider creates the chain, lists its images,
// acquires and presents. Only one chain of a provider exists at a time: the old one goes before the new one is
// made. Present is called without the layer's queue lock (SubmitToQueue takes it).
class SwapChainProvider
{
public:
    virtual ~SwapChainProvider() = default;
    // vkCreateSwapchainKHR: `createInfo` a VkSwapchainCreateInfoKHR without extensions, `swapchain` a
    // VkSwapchainKHR* to fill. Returns a VkResult.
    virtual int Create(const void* createInfo, void* swapchain) = 0;
    // `last`: no chain is made through this provider after it (another provider, the layer's own, the end).
    virtual void Destroy(void* swapchain, bool last) = 0;
    virtual int GetImages(void* swapchain, UINT* count, void* images) = 0;
    virtual int Acquire(void* swapchain, UINT64 timeout, void* semaphore, UINT* index) = 0;
    virtual int Present(void* queue, const void* presentInfo) = 0;
};

// Makes the chain again, now, through `provider` (null: the layer's own). No buffer of the chain may be held.
// False for a chain that is not this library's or a chain that could not be made (it is then the layer's own).
bool SetSwapChainProvider(IDXGISwapChain* chain, SwapChainProvider* provider);

// The queue an SDK hands its submissions to from any thread (VkSubmitInfo array, VkFence): under the lock the
// layer submits with. Returns a VkResult.
enum class Queue : UINT
{
    Graphics,
    Compute, // the graphics queue where the device has no async compute queue
};
int SubmitToQueue(ID3D11Device* device, Queue queue, UINT count, const void* submits, void* fence);
// The VkQueue behind Queue (the layer's own; null for another device).
void* NativeQueue(ID3D11Device* device, Queue queue);

// Adaptive vsync (rs_vsync): the vsync'd presents of a swap chain of this library go out FIFO_RELAXED, which
// tears a frame that missed its refresh; a change recreates the chain at the next present. False when the
// surface has no such mode (or the chain is not this library's): the caller tears late frames itself.
bool SetAdaptiveVsync(IDXGISwapChain* chain, bool adaptive);

// The id the next present of a swap chain of this library carries (VkPresentIdKHR), when it is above the last one:
// an SDK that pairs its latency markers with the presents (Streamline's Reflex on VK_NV_low_latency2) names the frame.
void SetNextPresentId(IDXGISwapChain* chain, UINT64 id);

// Whether the adapter can run the Vulkan renderer: a Vulkan 1.3 device with the features the layer needs.
// Creates and destroys a throw-away instance and device; `reason` gets a short text when it cannot.
bool ProbeAdapter(const LUID& adapterLuid, char* reason, size_t reasonSize);

// True for objects created by this library.
bool IsLayerDevice(ID3D11Device* device);

// Saves the pipeline cache now (it is also saved when the device is destroyed).
void SavePipelineCache(ID3D11Device* device);

// Builds in the background the graphics pipelines the journal of earlier sessions and the release's journal
// (`shipped`: shaders_cache_pack/r6/pipelines.journal, may be null) know whose shaders exist now. Returns at once.
void PrewarmPipelines(ID3D11Device* device, const void* shipped, size_t shippedSize);

// ---- capabilities -----------------------------------------------------------------------------

struct NativeCaps
{
    UINT apiVersion = 0;              // VK_MAKE_API_VERSION of the device
    UINT driverVersion = 0;
    UINT vendorId = 0;                // PCI vendor (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel)
    UINT deviceId = 0;
    char deviceName[256]{};
    char driverName[256]{};
    char driverInfo[256]{};
    bool asyncCompute = false;        // a compute-only queue family is used
    bool transferQueue = false;       // a transfer-only queue family carries creation-time uploads
    bool rebar = false;               // dynamic data lives in CPU-visible video memory
    UINT64 rebarBytes = 0;            // size of the DEVICE_LOCAL | HOST_VISIBLE heap
    bool dynamicState3 = false;       // blend enable/equation/write mask, polygon mode, sample mask dynamic
    bool vertexInputDynamic = false;  // input layouts out of the pipeline key
    bool pipelineLibrary = false;     // VK_EXT_graphics_pipeline_library with fast linking
    bool multiDraw = false;           // VK_EXT_multi_draw
    bool pushDescriptors = false;     // VK_KHR_push_descriptor
    bool nullDescriptors = false;     // VK_EXT_robustness2 nullDescriptor
    bool shadingRate = false;         // VK_KHR_fragment_shading_rate: pipeline and attachment rates
    UINT shadingRateTexel = 0;        // attachment texel size (8 or 16)
    bool shadingRateLarge = false;    // 4x4 fragments (D3D12's additional rates)
    bool meshShaders = false;         // VK_EXT_mesh_shader
    bool rayQuery = false;            // VK_KHR_ray_query + acceleration structures
    bool presentWait = false;         // VK_KHR_present_wait
    bool memoryBudget = false;        // VK_EXT_memory_budget
    bool fragmentShaderInterlock = false; // rasterizer ordered views
    bool float16 = false, int16 = false, int64Atomics = false;
    UINT subgroupSize = 0;            // the default wave size of compute
    UINT subgroupSizeMin = 0, subgroupSizeMax = 0; // VK_EXT_subgroup_size_control range (Intel 8..32, AMD 32..64)
    bool subgroupArithmetic = false;  // wave reductions and prefix ops in every stage the engine uses
    float timestampPeriod = 0.f;      // nanoseconds per timestamp tick
};

// The device's capabilities, masked by DeviceCreateInfo::disabledFeatures. Null for other devices.
const NativeCaps* GetNativeCaps(ID3D11Device* device);

// ---- D3D11-path extensions (the same calls xrD3D12 offers) ------------------------------------

// vkCmdDraw*IndirectCount over D3D11-created argument buffers with a GPU count (null = maxCount records).
void DrawIndexedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count, UINT countOffset);
void DrawInstancedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count, UINT countOffset);

// Mesh draw with the bound mesh shader (VS slot), task shader (GS slot) and pixel shader. False when the
// pipeline was refused, so the caller can draw its vertex path instead.
bool DispatchMesh(ID3D11DeviceContext* context, UINT x, UINT y, UINT z);

// Shading rate of the following draws (D3D12_SHADING_RATE values: 0x0 1x1, 0x1 1x2, 0x4 2x1, 0x5 2x2, 0x6 2x4,
// 0x9 4x2, 0xA 4x4) combined with an attachment rate image (R8_UINT, one texel per shadingRateTexel pixels,
// same values) by `imageCombiner` (D3D12_SHADING_RATE_COMBINER: 0 passthrough, 1 override, 2 min, 3 max,
// 4 sum) when `rateImage` is set. Context state like any D3D11 state. Ignored without the feature.
void SetShadingRate(ID3D11DeviceContext* context, UINT baseRate, UINT imageCombiner, ID3D11Resource* rateImage);

// Hint around a run of dispatches/draws whose UAV writes do not overlap (DXVK's BeginUAVOverlap): no barrier
// is inserted between them. Context state; End restores D3D11 serialization.
void BeginUavOverlap(ID3D11DeviceContext* context);
void EndUavOverlap(ID3D11DeviceContext* context);

// Orders every shader and transfer write recorded so far before what follows (D3D12's global UAV barrier;
// `resource` is accepted for the same signature). The layer orders D3D11 accesses by itself: only a run
// inside BeginUavOverlap needs it.
void UavBarrier(ID3D11DeviceContext* context, ID3D11Resource* resource);

// Local video memory of the adapter (VK_EXT_memory_budget; the heap sizes without it).
bool QueryVideoMemory(ID3D11Device* device, UINT64& budget, UINT64& usage);

// The persistently mapped bytes of a STAGING buffer with CPU_ACCESS_WRITE (an upload ring the caller fills
// without Map): writes are not synchronized, the caller never touches bytes the GPU may still copy from.
void* PersistentData(ID3D11Buffer* buffer);

// ---- ray tracing: the renderer's RT lane (da_rt_*.cpp, docs/dead-air/VULKAN_RENDERER.md) --------------------
// The lane's compute shaders bind like any other through the D3D11 calls, plus what D3D11 has no word for: the
// bindless arrays in descriptor set kBindlessSet ([[vk::binding(0, 7)]] 2D textures, [[vk::binding(1, 7)]] raw
// buffers, one index space for both), a push-constant block of at most kRtPushConstantBytes and the scene as an SRV
// of an acceleration structure. Everything here needs GetNativeCaps()->rayQuery.

constexpr UINT kBindlessSet = 7;
constexpr UINT kBindlessInvalid = ~0u;
constexpr UINT kRtPushConstantBytes = 64;

// The device's bindless range (as xrD3D12's): contiguous; kBindlessInvalid when full. Thread-safe. A slot the GPU
// may read this frame is never rewritten: allocate a new one and free the old.
UINT AllocateBindless(ID3D11Device* device, UINT count);
void WriteBindless(ID3D11Device* device, UINT index, ID3D11ShaderResourceView* view); // a 2D texture; null = none
void WriteBindlessBuffer(ID3D11Device* device, UINT index, ID3D11Buffer* buffer);     // its bytes as a raw buffer
void FreeBindless(ID3D11Device* device, UINT index, UINT count); // reusable after the frames in flight

// The push constants of the following dispatches (context state, up to kRtPushConstantBytes).
void SetComputePushConstants(ID3D11DeviceContext* context, const void* data, UINT size);

// A device-local buffer of the lane: device address, build input, 256-byte aligned, readable and writable by
// shaders through its address or a bindless slot; `data` fills it before the next submission (null: zeros).
HRESULT CreateRtBuffer(ID3D11Device* device, UINT64 size, const void* data, const char* name, ID3D11Buffer** out);
// The device address of a buffer's bytes; 0 for dynamic buffers (their bytes move) and without ray queries.
UINT64 BufferAddress(ID3D11Buffer* buffer);
// Upload memory of the context's frame with its device address (shader reads, build inputs), valid until the
// frame's work completed.
struct UploadSpan
{
    void* cpu = nullptr;
    UINT64 address = 0;
};
UploadSpan AllocateUpload(ID3D11DeviceContext* context, UINT64 size, UINT64 alignment);
// Textures (and GPU-written buffers) into the state compute shaders read them in, for reads the layer cannot see
// (bindless slots).
void RequireRead(ID3D11DeviceContext* context, ID3D11Resource* const* resources, UINT count);
// Grows whenever texture contents may have changed outside render targets (copies, updates, mips) and when
// textures are created or destroyed: bindless tables built from textures are refreshed when it moved.
UINT64 TextureWriteGeneration(ID3D11Device* device);

// Acceleration structures (VK_KHR_acceleration_structure), each in a buffer of its own.
struct AccelTriangles
{
    UINT64 vertexAddress = 0; // float3 positions
    UINT vertexStride = 0, vertexCount = 0;
    UINT64 indexAddress = 0;  // 16-bit indices
    UINT indexCount = 0;
    bool opaque = true;       // false: any-hit candidates reach the shader's query loop, each once
};
// D3D12's build flag values, which Vulkan shares.
enum AccelFlags : UINT
{
    AccelAllowUpdate = 0x1,
    AccelCompaction = 0x2,
    AccelFastTrace = 0x4,
    AccelFastBuild = 0x8,
};
struct AccelSizes
{
    UINT64 result = 0, build = 0, update = 0;
};
// A bottom level over `count` meshes, or (geometry null) a top level of `count` instances.
bool AccelBuildSizes(ID3D11Device* device, const AccelTriangles* geometry, UINT count, UINT flags, AccelSizes& out);
class Accel;
Accel* CreateAccel(ID3D11Device* device, bool top, UINT64 size, const char* name);
void ReleaseAccel(Accel* accel); // once the GPU finished the work recorded so far
UINT64 AccelAddress(const Accel* accel);
// The scene as a pass binds it (an SRV slot). The view holds the structure's storage, not the structure: release the
// view before the structure.
HRESULT CreateAccelView(Accel* accel, ID3D11ShaderResourceView** view);
// Records one build on the immediate context (no rendering open); nothing orders it after earlier work or before
// later work but AccelBarrier. `instances`: VkAccelerationStructureInstanceKHR records (D3D12_RAYTRACING_INSTANCE_DESC's
// layout), 16-byte aligned.
struct AccelBuild
{
    Accel* dst = nullptr;
    Accel* src = nullptr; // the structure an update refits (`update`)
    const AccelTriangles* geometry = nullptr;
    UINT count = 0;       // meshes, or instances of a top level
    UINT64 instances = 0;
    UINT flags = 0;
    bool update = false;
    UINT64 scratch = 0;   // aligned to 256
};
void BuildAccel(ID3D11DeviceContext* context, const AccelBuild& build);
void CopyAccelCompact(ID3D11DeviceContext* context, Accel* dst, Accel* src);
// Every write so far (shaders, copies, builds) before every access that follows, builds included.
void AccelBarrier(ID3D11DeviceContext* context);
// Compacted sizes of built structures (AccelCompaction), recorded after an AccelBarrier and read once the work
// completed (Flush with wait).
class AccelSizeQuery;
AccelSizeQuery* CreateAccelSizeQuery(ID3D11Device* device, UINT count);
void WriteCompactedSizes(ID3D11DeviceContext* context, AccelSizeQuery* query, Accel* const* accels, UINT first, UINT count);
bool ReadCompactedSizes(AccelSizeQuery* query, UINT64* sizes, UINT count);
void ReleaseAccelSizeQuery(AccelSizeQuery* query);

// ---- native work of an SDK on the immediate context (DLSS, FSR) ------------------------------------------

// The device's native objects (queue indices 0: the layer's own queues) and the vkGetDeviceProcAddr it
// resolved its calls through. False for another device.
bool GetNativeDevice(ID3D11Device* device, NativeDeviceInfo& out, void** getDeviceProcAddr);

// A texture as an SDK takes it: Vulkan handles as pointers, the rest as their Vulkan values.
struct NativeImage
{
    void* image = nullptr;  // VkImage
    void* memory = nullptr; // VkDeviceMemory
    void* view = nullptr;   // VkImageView of mip 0, layer 0 (the depth aspect of a depth texture)
    UINT layout = 0;        // VkImageLayout it is in for the SDK's access
    UINT format = 0;        // VkFormat
    UINT width = 0, height = 0, mipLevels = 0, arrayLayers = 0;
    UINT createFlags = 0;   // VkImageCreateFlags
    UINT usage = 0;         // VkImageUsageFlags
};
// Puts a texture of the immediate context into the layout of a compute access (`write`: GENERAL storage,
// else SHADER_READ_ONLY_OPTIMAL) and describes it. Call before BeginNative.
bool PrepareNativeImage(ID3D11DeviceContext* context, ID3D11Resource* resource, bool write, NativeImage& out);
// The immediate context's command buffer (VkCommandBuffer) with no rendering open and every pending barrier
// recorded; null on another device or a lost one.
void* BeginNative(ID3D11DeviceContext* context);
// After the SDK recorded: the layer binds its own state again for the next draw or dispatch.
void EndNative(ID3D11DeviceContext* context);

// ---- shaders --------------------------------------------------------------------------------------

// The container the engine hands to Create*Shader on this renderer: SPIR-V for the device, the DXIL the
// same source compiled to (its signature and reflection, which the engine's constant tables read) and the
// registers the DXIL reflection gave each resource. The layer binds every SPIR-V resource at the register
// of the same name, so the engine's D3D11 slots mean the same on both.
//   u32 magic 'GRZV', u32 version, u32 dxilSize, u32 spirvSize, u32 bindingCount,
//   dxil bytes (padded to 4), spirv words, ShaderBinding[bindingCount]
constexpr UINT kShaderContainerMagic = 0x565A5247; // "GRZV"
constexpr UINT kShaderContainerVersion = 1;
enum ShaderBindingClass : unsigned char
{
    ShaderBindingCB = 0,
    ShaderBindingSRV = 1,
    ShaderBindingSampler = 2,
    ShaderBindingUAV = 3,
};
struct ShaderBinding
{
    char name[58]{};          // the HLSL name (reflection Name), zero-terminated
    unsigned char cls = 0;    // ShaderBindingClass
    unsigned char count = 1;  // array size
    unsigned short reg = 0;   // register (BindPoint)
    unsigned short pad = 0;
};
static_assert(sizeof(ShaderBinding) == 64);
struct ShaderContainerView
{
    const void* dxil = nullptr;
    size_t dxilSize = 0;
    const uint32_t* spirv = nullptr;
    size_t spirvWords = 0;
    const ShaderBinding* bindings = nullptr;
    size_t bindingCount = 0;
};
bool ParseShaderContainer(const void* blob, size_t size, ShaderContainerView& out);
void BuildShaderContainer(const void* dxil, size_t dxilSize, const void* spirv, size_t spirvSize,
    const ShaderBinding* bindings, size_t bindingCount, std::vector<unsigned char>& out);

// The DXC arguments a SPIR-V compile for this layer needs (register shifts, layout, I/O semantics, Vulkan
// conventions); the engine adds -E/-T/defines.
const wchar_t* const* SpirvCompileArgs(UINT& count);

// ---- submission ---------------------------------------------------------------------------------

// Submits the immediate context's recorded work; with `wait` also waits until the GPU is idle.
void Flush(ID3D11DeviceContext* immediate, bool wait);
} // namespace xrvk
