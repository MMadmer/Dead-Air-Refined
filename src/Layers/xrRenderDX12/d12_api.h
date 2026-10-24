// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// xrD3D12: a native Direct3D 12 backend behind the Direct3D 11 interfaces the renderer is
// written against. The renderer module built with it (xrRender_R5) runs the same render code
// as the DX11 module; every command becomes D3D12 work - pipeline state objects, descriptor
// tables, explicit barriers, real parallel command lists for the deferred contexts.
//
// The library depends on the Windows SDK and the C++ standard library only, so it can be built
// and tested outside the engine (tools/qa/d3d12layer).
//
// API freeze of the DirectX 12 feature work (docs/dead-air/dx12-features/PLAN.md, milestone I0):
// every declaration the feature lanes build on is here. Later additions go only into the
// `[dx12:<lane>]` blocks at the end.
#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <string>
#include <vector>

namespace xrd3d12
{
// ---- creation ---------------------------------------------------------------------------------

using LogSink = void (*)(const char* line);

// Creates the direct command queue. NVIDIA Streamline must see the queue created through its
// upgraded device (DLSS frame generation); null = ID3D12Device::CreateCommandQueue.
using CreateQueueHook = HRESULT (*)(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, ID3D12CommandQueue** queue);
// Called once the native device exists, before anything is created on it (Streamline's
// slSetD3DDevice). May be null.
using DeviceCreatedHook = void (*)(ID3D12Device* device);

// Capabilities a run switches off (DeviceCreateInfo::disabledFeatures). The matching NativeCaps
// fields then read as absent, so every fallback can be tested on hardware that has everything.
enum NativeFeature : UINT
{
    NativeFeatureRaytracing = 1u << 0,   // -dx12_no_rt: raytracingTier 0
    NativeFeatureVrs = 1u << 1,          // -dx12_no_vrs: vrsTier 0
    NativeFeatureMeshShaders = 1u << 2,  // -dx12_no_ms: meshShaderTier 0
    NativeFeatureAsyncCompute = 1u << 3, // -dx12_no_async: async jobs run in order on the direct queue
    NativeFeatureSm6 = 1u << 4,          // -dx12_no_sm6: no DXC, shader model 5.1 (hence no RT, no mesh shaders)
};

struct DeviceCreateInfo
{
    CreateQueueHook createQueue = nullptr;
    DeviceCreatedHook deviceCreated = nullptr;
    IDXGIAdapter1* adapter = nullptr; // the adapter the engine selected
    bool debugLayer = false;          // -dxdebug: D3D12 debug layer, messages go to the log
    bool gpuValidation = false;       // -dx12gbv: GPU-based validation (slow, debugging only)
    bool dred = false;                // -dx12dred: DRED breadcrumbs + page fault data on removal
    bool stats = false;               // -dxstats: the layer's per-frame work counters in the log
    // Dynamic buffers in CPU-visible video memory (resizable BAR) where the adapter offers GPU upload
    // heaps; -dx12_no_rebar keeps them in system memory.
    bool gpuUploadHeaps = true;
    // Creation-time data (resources created with initial data) goes through a copy queue the direct
    // queue waits for; -dx12_no_copy_queue keeps it on the direct queue.
    bool copyQueueUploads = true;
    const wchar_t* pipelineCachePath = nullptr; // file of the persistent pipeline library, may be null
    LogSink log = nullptr;            // receives complete lines, no trailing newline
    UINT disabledFeatures = 0;        // NativeFeature bits
};

// Creates the D3D12 device and returns it behind ID3D11Device / ID3D11DeviceContext (immediate).
// Feature level is the D3D12 device's (11_0 minimum).
HRESULT CreateDevice(const DeviceCreateInfo& info, ID3D11Device** device, ID3D11DeviceContext** immediate,
    D3D_FEATURE_LEVEL* featureLevel);

// Creates a flip-model swap chain on the device's queue. A legacy (DISCARD/SEQUENTIAL) or
// single-buffer description is mapped to FLIP_DISCARD with two buffers: D3D12 has nothing else.
// GetBuffer(0) of the result always means "the current back buffer", as with D3D11 flip chains.
HRESULT CreateSwapChainForHwnd(ID3D11Device* device, IDXGIFactory2* factory, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGISwapChain1** swapChain);

// Whether the adapter can run the D3D12 renderer: feature level 11_0 and resource binding tier 2.
// Creates and destroys a throw-away device; `reason` gets a short text when it cannot.
bool ProbeAdapter(IDXGIAdapter1* adapter, char* reason, size_t reasonSize);

// True for objects created by this library.
bool IsLayerDevice(ID3D11Device* device);

// Saves the pipeline library now (it is also saved when the device is destroyed).
void SavePipelineCache(ID3D11Device* device);

// Builds in the background the graphics pipelines the journal of earlier sessions and the release's journal
// (`shipped`: shaders_cache_pack/r5/pipelines.journal, may be null) know whose shaders exist now. Returns at once.
void PrewarmPipelines(ID3D11Device* device, const void* shipped, size_t shippedSize);

// ---- capabilities (d12_native.cpp) ------------------------------------------------------------

struct NativeCaps
{
    UINT raytracingTier = 0;        // D3D12_RAYTRACING_TIER: 0, 10, 11, 12
    UINT vrsTier = 0;               // 0, 1, 2
    UINT vrsTileSize = 0;           // tier 2: 8, 16 or 32
    bool vrsAdditionalRates = false, vrsPerPrimitiveWithViewportIndex = false, vrsSumCombiner = false;
    UINT meshShaderTier = 0;        // 0 or 10
    bool meshPerPrimitiveRate = false;
    UINT shaderModel = 0x51;        // device: highest D3D_SHADER_MODEL (0x60..0x69), 0x51 = no SM 6
    UINT dxcShaderModel = 0;        // what the loaded DXC targets (0x69 for 1.9), 0 = no DXC
    bool waveOps = false;
    UINT waveLanesMin = 0, waveLanesMax = 0;
    bool native16Bit = false, int64Ops = false, barycentrics = false;
    bool minPrecision16 = false;    // OPTIONS.MinPrecisionSupport has 16-bit
    bool heapIndexing = false;      // SM 6.6 ResourceDescriptorHeap: shaderModel >= 6.6 and DXC >= 6.6
    bool enhancedBarriers = false;  // reported only
    UINT executeIndirectTier = 0, workGraphsTier = 0, rootSignatureVersion = 0;
    bool asyncCompute = false;      // jobs really run on the compute queue
    bool copyQueueTimestamps = false;
    UINT runtimeSdkVersion = 0;     // ID3D12DeviceConfiguration::GetDesc().SDKVersion, 0 = older runtime
    UINT bindlessCapacity = 0;      // persistent descriptors (AllocateBindless)
    UINT vendorId = 0;              // PCI vendor of the adapter (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel)
    bool uma = false;               // integrated GPU (D3D12_FEATURE_ARCHITECTURE1::UMA)
};

// The device's capabilities, masked by DeviceCreateInfo::disabledFeatures. Null for other devices.
const NativeCaps* GetNativeCaps(ID3D11Device* device);

// ProbeAdapter that also reports the adapter's capabilities, queried on the throw-away device
// (unmasked; dxcShaderModel stays 0: the compiler is not loaded for a probe).
bool ProbeAdapter(IDXGIAdapter1* adapter, char* reason, size_t reasonSize, NativeCaps& caps);

// Variable rate shading: a view of NativeCaps.
struct ShadingRateCaps
{
    UINT tier = 0;
    UINT tileSize = 0;
    bool additionalRates = false;
};
bool GetShadingRateCaps(ID3D11Device* device, ShadingRateCaps* caps);

// ---- native interop ---------------------------------------------------------------------------
// Valid on the immediate context (render thread) and on a deferred context (its recording thread).

ID3D12Device* NativeDevice(ID3D11Device* device);
// Null where the runtime lacks the interface (DXR needs it).
ID3D12Device5* NativeDevice5(ID3D11Device* device);
ID3D12CommandQueue* NativeQueue(ID3D11Device* device);

// The D3D12 resource behind a D3D11 resource (for the back buffer: the current buffer). Null for
// buffers that live in shared pages or upload memory: NativeBufferRange describes those.
ID3D12Resource* NativeResource(ID3D11Resource* resource);

// What native code changed on the list inside a section (EndNative): only that is applied again.
enum NativeTouched : UINT
{
    TouchedHeaps = 1u << 0,          // SetDescriptorHeaps with foreign heaps (SDKs): implies everything
    TouchedGraphicsRoot = 1u << 1,   // SetGraphicsRootSignature / graphics root arguments
    TouchedComputeRoot = 1u << 2,    // SetComputeRootSignature / compute root arguments (DispatchRays too)
    TouchedPipeline = 1u << 3,       // SetPipelineState / SetPipelineState1
    TouchedInputAssembler = 1u << 4,
    TouchedOutputMerger = 1u << 5,   // render targets, blend factor, stencil reference
    TouchedRasterizer = 1u << 6,     // viewports, scissors
    TouchedShadingRate = 1u << 7,    // RSSetShadingRate / RSSetShadingRateImage
    TouchedAll = 0xFFu,
};

struct NativeScope
{
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList4* list4 = nullptr; // DXR; null when the runtime lacks it
    ID3D12GraphicsCommandList5* list5 = nullptr; // VRS
    ID3D12GraphicsCommandList6* list6 = nullptr; // DispatchMesh
    ID3D12DescriptorHeap* heap = nullptr;        // the layer's CBV/SRV/UAV heap, bound on `list`
    D3D12_GPU_DESCRIPTOR_HANDLE bindless{};      // GPU handle of bindless index 0 (AllocateBindless)
};

// Opens a native section and hands out the context's open list with pending barriers flushed, the
// layer's heaps bound and shading rate 1x1 without an image. Between BeginNative and EndNative no
// D3D11 call may be made on that context except the helpers below. False on a removed device.
bool BeginNative(ID3D11DeviceContext* context, NativeScope& scope);
// Closes the section; `touched` (NativeTouched bits) is what the native code changed on the list.
void EndNative(ID3D11DeviceContext* context, UINT touched);
// Puts a tracked resource (all subresources) into `state`; on a deferred context the first
// requirement becomes the list's entry state. Barriers are flushed at once. Untracked buffers
// (pages, upload memory) need none and are ignored.
void Require(ID3D11DeviceContext* context, ID3D11Resource* resource, D3D12_RESOURCE_STATES state);
void RequireMany(ID3D11DeviceContext* context, ID3D11Resource* const* resources, UINT count, D3D12_RESOURCE_STATES state);
// Only the subresources the view covers.
void RequireView(ID3D11DeviceContext* context, ID3D11View* view, D3D12_RESOURCE_STATES state);
// UAV barrier on `resource`; null = global.
void UavBarrier(ID3D11DeviceContext* context, ID3D11Resource* resource);

// Section with the old contract (immediate context, everything re-applied afterwards): the SDK
// integrations (FSR, DLSS, frame generation) use it.
ID3D12GraphicsCommandList* BeginNative(ID3D11DeviceContext* immediate);
void EndNative(ID3D11DeviceContext* immediate);

// CPU descriptor of a view (back buffer RTVs resolve to the current buffer): a CopyDescriptors
// source for ring tables or bindless slots.
D3D12_CPU_DESCRIPTOR_HANDLE NativeDescriptor(ID3D11View* view);

// Where a buffer's bytes are: page ranges, the current version of a dynamic buffer (valid for work
// recorded now; a later DISCARD renames it) and tracked buffers.
struct NativeBuffer
{
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    UINT64 size = 0;
    ID3D12Resource* resource = nullptr; // page, upload page or the buffer's own resource
    UINT64 offset = 0;                  // of `address` inside `resource`
    bool tracked = false;               // own resource with tracked states (Require applies)
};
bool NativeBufferRange(ID3D11Buffer* buffer, NativeBuffer& out);

// Grows whenever texture contents may have changed outside render targets (UpdateSubresource,
// copies, Map into textures without RT/DS/UAV binding) and when textures are created or
// destroyed: descriptor tables built from textures are refreshed when it moved.
UINT64 TextureWriteGeneration(ID3D11Device* device);

// ---- descriptors and memory (d12_native.cpp) -----------------------------------------------------

struct DescriptorSpan
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT index = 0;     // in the shader-visible heap
    UINT count = 0;     // 0 = allocation failed
    UINT increment = 0;
};
struct UploadSpan
{
    void* cpu = nullptr; // null = allocation failed
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    ID3D12Resource* resource = nullptr;
    UINT64 offset = 0;
};

// Shader-visible descriptors and upload memory of the context, valid until the frame ends.
DescriptorSpan AllocateDescriptors(ID3D11DeviceContext* context, UINT count);
UploadSpan AllocateUpload(ID3D11DeviceContext* context, UINT64 size, UINT64 alignment);

// The persistent bindless range at the start of the shader-visible heap: SRVs only, null-filled,
// addressed by index through an unbounded table bound at NativeScope::bindless (or
// ResourceDescriptorHeap[] with heapIndexing). Thread-safe. A slot the GPU may read this frame is
// never rewritten: allocate a new one and free the old.
constexpr UINT kBindlessInvalid = ~0u;
UINT AllocateBindless(ID3D11Device* device, UINT count); // contiguous; kBindlessInvalid when full
void WriteBindless(ID3D11Device* device, UINT index, ID3D11ShaderResourceView* view); // null = null SRV
void WriteBindlessBuffer(ID3D11Device* device, UINT index, ID3D11Buffer* buffer);     // raw SRV over its range
void WriteBindlessNative(ID3D11Device* device, UINT index, ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& desc);
void FreeBindless(ID3D11Device* device, UINT index, UINT count); // reusable after the current frame's fence

// Committed resources for native passes; released through ReleaseNative.
HRESULT CreateNativeBuffer(ID3D11Device* device, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial, const wchar_t* name, ID3D12Resource** out);
// DEFAULT heap, UAV flag, created in RAYTRACING_ACCELERATION_STRUCTURE (never transitioned).
HRESULT CreateAccelerationStructureBuffer(ID3D11Device* device, UINT64 size, const wchar_t* name, ID3D12Resource** out);
// Releases the reference once the GPU finished the work recorded so far.
void ReleaseNative(ID3D11Device* device, IUnknown* object);
// Local video memory of the adapter (IDXGIAdapter3::QueryVideoMemoryInfo).
bool QueryVideoMemory(ID3D11Device* device, UINT64& budget, UINT64& usage);

// ---- pipelines (d12_native.cpp) ---------------------------------------------------------------
// Device-owned caches, thread-safe; the objects live as long as the device (never Release them).

// Serialized as version 1.1 (1.0 where the runtime lacks it) and cached by the blob's hash.
ID3D12RootSignature* RootSignature(ID3D11Device* device, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, const wchar_t* name);
// Compute and stream pipelines go through the persistent pipeline library.
ID3D12PipelineState* ComputePipeline(ID3D11Device* device, ID3D12RootSignature* root, const void* dxil, size_t size,
    const wchar_t* name);
// `stream` is a D3D12_PIPELINE_STATE_STREAM_DESC payload; `contentHash` names it in the library.
ID3D12PipelineState* StreamPipeline(ID3D11Device* device, const void* stream, size_t size, UINT64 contentHash,
    const wchar_t* name);
ID3D12CommandSignature* CommandSignature(ID3D11Device* device, const D3D12_COMMAND_SIGNATURE_DESC& desc,
    ID3D12RootSignature* root);
// State objects are not cached: `out` gets a reference the caller owns (ReleaseNative). 30-60 ms each:
// create at renderer init, never on a frame.
HRESULT RaytracingPipeline(ID3D11Device* device, const D3D12_STATE_OBJECT_DESC& desc, ID3D12StateObject** out);

// ---- D3D11-path extensions --------------------------------------------------------------------
// Engine draws keep using the D3D11 API; these change how they execute.

// ExecuteIndirect over D3D11-created argument buffers (MISC_DRAWINDIRECT_ARGS) with a GPU count
// (null = maxCount records).
void DrawIndexedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count, UINT countOffset);
void DrawInstancedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count, UINT countOffset);
void DispatchIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count, UINT countOffset);

// A D3D11 SRV of a top-level acceleration structure (null = an empty one), bindable with
// PS/CSSetShaderResources for inline RayQuery in DXIL shaders.
HRESULT CreateAccelerationStructureView(ID3D11Device* device, ID3D12Resource* tlas, ID3D11ShaderResourceView** out);

// Context state like any D3D11 state (immediate or deferred): kept until changed, cleared by
// ClearState, ExecuteCommandList(FALSE) and FinishCommandList(FALSE); never seen by native sections.
// Ignored below VRS tier 2.
void SetShadingRate(ID3D11DeviceContext* context, D3D12_SHADING_RATE base, D3D12_SHADING_RATE_COMBINER perPrimitive,
    D3D12_SHADING_RATE_COMBINER image, ID3D11Resource* rateImage);

// Mesh draw with the bound mesh shader (VS slot), amplification shader (GS slot) and pixel shader.
// False when the pipeline was refused, so the caller can draw its vertex path instead.
bool DispatchMesh(ID3D11DeviceContext* context, UINT x, UINT y, UINT z);

// ---- async compute (d12_async.cpp) ------------------------------------------------------------
// A job is compute work on the compute queue between a fork and a join of the immediate context.
// Invariant: every fork is joined before the frame's Present (Present joins a forgotten one and logs).
// With no compute queue (or -dx12_no_async) jobs run in order on the direct queue: same results.

class AsyncJob;
// Render thread (the immediate context's), like every job call below: a job is begun, recorded, forked
// and joined within one frame. Its descriptors and upload memory belong to the frame it was begun in, so
// a job forked in a later frame is dropped (logged once); one ended and never forked is dropped at
// Present. scope.list is a COMPUTE list (a DIRECT one without a compute queue) with the layer heap
// bound.
AsyncJob* BeginAsyncCompute(ID3D11Device* device, const char* name, NativeScope& scope);
// A tracked resource the job touches: the compute-legal state it needs on entry and leaves it in.
// entry == exit in a read state declares a read-only use: the job records no barrier on it and never
// writes it; it is lent read-shared in entry | PIXEL | NON_PIXEL_SHADER_RESOURCE, so the direct queue
// keeps reading it while the job runs. A job that writes it (even one that restores the read state)
// declares the write state it leaves it in as `exit` and records no barrier back.
void AsyncUse(AsyncJob* job, ID3D11Resource* resource, D3D12_RESOURCE_STATES entry, D3D12_RESOURCE_STATES exit);
DescriptorSpan AsyncDescriptors(AsyncJob* job, UINT count);
UploadSpan AsyncUpload(AsyncJob* job, UINT64 size, UINT64 alignment);
void EndAsyncCompute(AsyncJob* job); // closes the list
// A job that will not be forked: its work is dropped and the job recycled.
void DiscardAsyncCompute(AsyncJob* job);
// Immediate context, render thread. Fork: the job starts after everything recorded so far.
void ForkAsyncCompute(ID3D11DeviceContext* immediate, AsyncJob* job);
// Join: work recorded from here on waits for the job; its resources take their exit states. The job
// is recycled after it.
void JoinAsyncCompute(ID3D11DeviceContext* immediate, AsyncJob* job);
// GPU milliseconds of the last resolved job of that name (0 when none).
float AsyncComputeMs(ID3D11Device* device, const char* name);

// Switches a deferred context to compute recording until its next FinishCommandList: dispatches,
// copies, UAV clears and every state setter are recorded; draws, RTV/DSV clears, resolves and
// queries are dropped with a log line. False while a list is open on it.
bool BeginAsyncRecording(ID3D11DeviceContext* deferred);
// Wraps a list finished by such a context into a job: every tracked resource the list touched
// becomes an AsyncUse (first state -> last state; one it only reads is lent read-shared). Render
// thread; fork it in the frame it was made in (the recording itself may run on any thread).
AsyncJob* AsyncJobFromCommandList(ID3D11Device* device, const char* name, ID3D11CommandList* list);

// ---- shader compiler (DXC, d12_dxc.cpp) -------------------------------------------------------

// Loads dxil.dll and dxcompiler.dll from `dir` (absolute; null = the executable's folder) and checks
// that a compiled pipeline is accepted. The first call decides for the process; later calls return
// the same verdict. `reason` gets a short text when it fails.
bool LoadShaderCompiler(const wchar_t* dir, char* reason, size_t reasonSize);
// "1.9.0.5402"-style version of the loaded compiler, "" when none is loaded.
const char* DxcVersion();
// Highest shader model the loaded compiler targets (D3D_SHADER_MODEL value, e.g. 0x69); 0 without it.
UINT DxcShaderModel();

enum ShaderCompileFlags : UINT
{
    CompileDebugInfo = 1u << 0,        // -Zi -Qembed_debug
    Compile16BitTypes = 1u << 1,       // -enable-16bit-types
    CompileKeepReflection = 1u << 2,   // reflection stays in the object (it is also in ShaderOutput::reflection)
    CompileWarningsAsErrors = 1u << 3, // -WX
    CompileSkipOptimization = 1u << 4, // -Od
    CompileSpirv = 1u << 5,            // a -spirv compile (xrVulkan): none of the DXIL container switches
};
struct ShaderDefine
{
    const char* name = nullptr;
    const char* value = nullptr; // null = "1"
};
struct ShaderCompile
{
    const char* source = nullptr;
    size_t size = 0;
    const char* name = nullptr;   // file name for messages; includes resolve relative to it first
    const char* entry = nullptr;  // null for libraries (lib_6_x)
    const char* target = nullptr; // "vs_6_6", "cs_6_5", "lib_6_5", "ms_6_5"
    const ShaderDefine* defines = nullptr;
    UINT defineCount = 0;
    UINT flags = 0;               // ShaderCompileFlags
    UINT hlsl = 2021;             // language version: 2018 or 2021
    const wchar_t* const* args = nullptr; // further DXC arguments, passed as they are
    UINT argCount = 0;
    bool preprocessed = false;    // self-contained source: no include handler
    // #include handler: fills `text` with the file's contents, false = not found. DXC offers the
    // path relative to the including file first, then the include directories.
    bool (*include)(void* user, const char* file, std::string& text) = nullptr;
    void* user = nullptr;
};
struct ShaderOutput
{
    std::vector<unsigned char> object;     // DXIL container, reflection stripped unless kept
    std::vector<unsigned char> reflection; // DXC_OUT_REFLECTION (for Reflect)
    std::string messages;                  // warnings and errors
    bool isSigned = false;                 // validated and signed by dxil.dll
};
// Thread-safe. An access violation inside DXC becomes a failed compile.
bool CompileShader(const ShaderCompile& request, ShaderOutput& output);
// ID3D12ShaderReflection (or ID3D12LibraryReflection for lib_*) for DXBC and DXIL bytecode or a
// DXIL reflection blob.
HRESULT Reflect(const void* bytecode, size_t size, REFIID riid, void** reflection);
HRESULT Disassemble(const void* bytecode, size_t size, std::string& text);
// Has a DXIL part (DXC output), as opposed to FXC's DXBC.
bool IsDxil(const void* bytecode, size_t size);

// ---- submission, swap chain -------------------------------------------------------------------

// Submits the immediate context's recorded work; with `wait` also waits until the GPU is idle.
void Flush(ID3D11DeviceContext* immediate, bool wait);

// Swap chain providers: frame generation replaces the DXGI swap chain with its own (AMD FSR frame
// generation creates one, NVIDIA Streamline upgrades the factory). The wrapper the engine holds
// stays the same object; only what it presents through changes.
class SwapChainProvider
{
public:
    virtual ~SwapChainProvider() = default;
    // Creates the swap chain. `desc` is always flip-model.
    virtual HRESULT Create(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1& desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
        IDXGISwapChain4** swapChain) = 0;
    // Called after the GPU went idle, before the swap chain is released.
    virtual void Destroy(IDXGISwapChain4* swapChain) = 0;
    // Called on the render thread right before every Present of the native swap chain.
    virtual void BeforePresent(IDXGISwapChain4* /*swapChain*/) {}
};

// Rebuilds the native swap chain of `swapChain` (a wrapper from CreateSwapChainForHwnd) through
// `provider`, or back to a plain DXGI one when `provider` is null. Waits for the GPU; the engine
// must not hold back buffer views across the call (same rule as ResizeBuffers).
HRESULT RecreateSwapChain(IDXGISwapChain* swapChain, SwapChainProvider* provider);

// The native swap chain currently behind the wrapper.
IDXGISwapChain4* NativeSwapChain(IDXGISwapChain* swapChain);

// ---- later additions, one block per lane ------------------------------------------------------
// [dx12:infra] begin api
// [dx12:infra] end api
// [dx12:vrs] begin api
// [dx12:vrs] end api
// [dx12:mesh] begin api
// [dx12:mesh] end api
// [dx12:ds] begin api
// Placed texture heaps (d12_texheap.cpp). SRV-only textures created with initial data live in shared
// texture heaps. A texture without initial data is placed only while its creating thread has set
// SetStreamedTextures(true): an outside copy (DirectStorage) writes every subresource before its first
// use. Returns the previous setting of the calling thread.
bool SetStreamedTextures(bool on);
struct TextureHeapStats
{
    UINT heaps = 0;
    UINT64 heapBytes = 0;
    UINT placed = 0;          // live placed textures
    UINT64 placedBytes = 0;
    UINT64 placedTotal = 0;   // placed since the device was created
    UINT64 committedTotal = 0; // eligible, but committed (too big, or no heap memory)
};
void GetTextureHeapStats(ID3D11Device* device, TextureHeapStats& stats);
// [dx12:ds] end api
// [dx12:rt] begin api
// [dx12:rt] end api
// [dx12:async] begin api
// [dx12:async] end api
// [dx12:dxc] begin api
// [dx12:dxc] end api
} // namespace xrd3d12
