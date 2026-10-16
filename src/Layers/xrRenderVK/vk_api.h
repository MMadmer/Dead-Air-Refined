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

// Whether the adapter can run the Vulkan renderer: a Vulkan 1.3 device with the features the layer needs.
// Creates and destroys a throw-away instance and device; `reason` gets a short text when it cannot.
bool ProbeAdapter(const LUID& adapterLuid, char* reason, size_t reasonSize);

// True for objects created by this library.
bool IsLayerDevice(ID3D11Device* device);

// Saves the pipeline cache now (it is also saved when the device is destroyed).
void SavePipelineCache(ID3D11Device* device);

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
