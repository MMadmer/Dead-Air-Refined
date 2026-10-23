// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Internal declarations of xrD3D12 (see d12_api.h and docs/dead-air/DX12_RENDERER.md).
//
// Ownership of the implementation files:
//   d12_infra.cpp          fences, descriptor heaps and rings, sampler tables, upload pages, buffer
//                          pages, init uploader, PSO cache + pipeline library, root signatures,
//                          null descriptors, query heaps, deferred releaser, formats, hashing, log
//   d12_device.cpp         Device (ID3D11Device), public creation API, frame management
//   d12_objects.cpp        resources, views, state objects, shaders, input layouts, queries,
//                          command lists (the COM objects and their creation)
//   d12_context_state.cpp  Context: binding API, Get* API, hazards, PrepareDraw/Dispatch, draws
//   d12_context_ops.cpp    Context: list lifecycle, state tracking, barriers, Map/Unmap, copies,
//                          clears, resolves, queries, ClearState, Flush, command list execution
//   d12_swapchain.cpp      SwapChain (IDXGISwapChain1), back buffer proxy, present, providers,
//                          native interop API
//   d12_native.cpp         capabilities, native sections v2, bindless range, native pipelines and
//                          buffers, D3D11-path extensions (indirect counts, AS views)
//   d12_async.cpp          compute queue, async jobs, compute recording
//   d12_dxc.cpp            the DXC shader compiler service
//   d12_vrs.cpp            variable rate shading context state (lane `vrs`)
//   d12_mesh.cpp           mesh shader draws (lane `mesh`)
//   d12_texheap.cpp        placed texture heaps (lane `ds`)
// Members and hooks of the feature lanes sit in `[dx12:<lane>]` blocks (PLAN.md 0.2); their bodies
// live in the lane's file.
#pragma once

#include "d12_api.h"
#include "../xrRender/present_pacing.h"

#include <d3dcompiler.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace xrd3d12
{
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

class Device;
class Context;
class SwapChain;
class Resource;
class View;

// ============================================================================================
// Limits and constants
// ============================================================================================

// Frames the CPU may record ahead of the GPU before resources of a frame are recycled.
constexpr u32 kFramesInFlight = 3;

// Binding limits of the emulated pipeline (the engine uses far less: VS 5 CBs / 1 SRV,
// PS 3 CBs / 12 SRVs / 5 samplers, CS 1 CB / 1 SRV / 1 UAV).
constexpr u32 kMaxCBs = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; // 14
constexpr u32 kMaxSRVs = 32;      // D3D11 allows 128; slots above 31 are refused with a log line
constexpr u32 kMaxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; // 16
constexpr u32 kMaxUAVs = 8;
constexpr u32 kMaxRTVs = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; // 8
constexpr u32 kMaxVBs = 16;
constexpr u32 kMaxViewports = 16;

// Constant buffers bound as root CBVs per stage; the rest of the 14 slots go through a table.
constexpr u32 kRootCBVsVS = 5;
constexpr u32 kRootCBVsPS = 5;
constexpr u32 kRootCBVsOther = 1; // GS/HS/DS
constexpr u32 kRootCBVsCS = 4;

// Descriptor ring granularity (shader-visible CBV/SRV/UAV heap).
constexpr u32 kDescriptorRingCapacity = 1000000;
constexpr u32 kDescriptorChunk = 2048;
constexpr u32 kSamplerHeapCapacity = 2048;
// Persistent bindless range at the start of that heap (min(this, heap size / 8)); the ring follows.
constexpr u32 kBindlessCapacity = 65536;

// Upload memory.
constexpr u64 kUploadPageSize = 2ull << 20;       // per-context linear pages (dynamic data)
constexpr u64 kInitUploadPageSize = 32ull << 20;  // creation-time data
constexpr u64 kBufferPageSize = 32ull << 20;      // suballocated static VB/IB/CB pages
constexpr u64 kBufferSuballocLimit = 4ull << 20;  // bigger static buffers are committed alone

enum class Stage : u8
{
    VS = 0,
    HS,
    DS,
    GS,
    PS,
    CS,
    Count
};
constexpr u32 kStageCount = u32(Stage::Count);

// Program kinds of the DXBC version token and the DXIL part header (same numbering).
constexpr u8 kKindPixel = 0, kKindVertex = 1, kKindGeometry = 2, kKindHull = 3, kKindDomain = 4, kKindCompute = 5;
constexpr u8 kKindMesh = 13, kKindAmplification = 14;

// State of the feature lanes, defined in their files (PLAN.md 0.2: one pointer per lane).
struct DeviceVrs;
struct ContextVrs;
struct DeviceMesh;
struct ContextMesh;
class TexturePageAllocator;
struct TexturePlacement;
struct DeviceAsync; // d12_async.cpp

// ============================================================================================
// Utilities (d12_infra.cpp)
// ============================================================================================

// Logging. Lines go to the sink given at creation (the engine log) and to OutputDebugString.
void SetLogSink(LogSink sink);
void Log(const char* fmt, ...);
// Logs once per distinct `key` (format string address is a good key).
void LogOnce(const void* key, const char* fmt, ...);

// 64-bit content hash (xxHash64-style, stable across runs and machines).
u64 Hash64(const void* data, size_t size, u64 seed = 0);
inline u64 HashCombine(u64 a, u64 b) { return Hash64(&b, sizeof(b), a); }

// Formats.
bool IsDepthFormat(DXGI_FORMAT format);          // any depth/stencil format incl. typeless families
u32 PlaneCount(ID3D12Device* device, DXGI_FORMAT format); // D3D12 plane count (D24S8 = 2)
DXGI_FORMAT TypelessOf(DXGI_FORMAT format);      // family typeless format, or the format itself
bool IsBlockCompressed(DXGI_FORMAT format);

// d3d12.dll entry points, resolved at runtime: xrEngine.exe imports every renderer module
// statically and must still start where d3d12.dll is missing (d12_infra.cpp).
struct D3D12Api
{
    PFN_D3D12_CREATE_DEVICE CreateDevice = nullptr;
    PFN_D3D12_GET_DEBUG_INTERFACE GetDebugInterface = nullptr;
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE SerializeVersionedRootSignature = nullptr;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE SerializeRootSignature = nullptr;
    bool Load(); // thread-safe, idempotent; false when d3d12.dll is not available
};
extern D3D12Api g_d3d12;

// Resource states used by the layer for each kind of binding.
constexpr D3D12_RESOURCE_STATES kStateShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// ============================================================================================
// Capabilities (d12_native.cpp) and the shader compiler service (d12_dxc.cpp)
// ============================================================================================

// Every capability of the device (unmasked). `adapter` may be null (vendor and UMA stay 0).
void QueryNativeCaps(ID3D12Device* device, IDXGIAdapter1* adapter, NativeCaps& caps);
// ProbeAdapter; with `caps` also the capabilities of the throw-away device (d12_device.cpp).
bool ProbeAdapterCaps(IDXGIAdapter1* adapter, char* reason, size_t reasonSize, NativeCaps* caps);
// Zeroes what the NativeFeature bits switch off.
void MaskNativeCaps(NativeCaps& caps, UINT disabledFeatures);
// The `* native:` lines of the device log.
void LogNativeCaps(const NativeCaps& caps, UINT disabledFeatures, u32 ringDescriptors);
// Loads the compiler next to the executable and checks a compiled pipeline on `device`; the result
// is sticky for the process. Returns DxcShaderModel() (0 = no compiler).
UINT InitShaderCompiler(ID3D12Device* device);

// ============================================================================================
// Synchronisation (d12_infra.cpp)
// ============================================================================================

class FenceTimeline
{
public:
    bool Init(ID3D12Device* device);
    void Shutdown();
    // Signals the next value on the queue and returns it.
    u64 Signal(ID3D12CommandQueue* queue);
    // The value the next Signal() will use (resources released now are safe after it).
    u64 NextValue() const { return m_next.load(std::memory_order_acquire); }
    u64 Completed();
    bool IsComplete(u64 value) { return value <= Completed(); }
    // Blocks the calling thread until the value completed. Thread-safe.
    void Wait(u64 value);
    ID3D12Fence* Native() const { return m_fence; }

private:
    ID3D12Fence* m_fence = nullptr;
    std::atomic<u64> m_next{1};
    std::atomic<u64> m_completed{0};
    HANDLE m_event = nullptr;
    std::mutex m_waitLock;
    std::mutex m_signalLock;
};

// Objects released by the engine while the GPU may still use them live until the fence passes.
class DeferredReleaser
{
public:
    void Init(FenceTimeline* fence) { m_fence = fence; }
    void Release(IUnknown* object);                 // Release() after the current NextValue()
    void Defer(std::function<void()>&& action);     // runs after the current NextValue()
    void Collect(bool all);                         // runs what the fence allows (all: everything)

private:
    struct Entry
    {
        u64 fence;
        IUnknown* object;
        std::function<void()> action;
    };
    FenceTimeline* m_fence = nullptr;
    std::mutex m_lock;
    std::vector<Entry> m_entries;
};

// ============================================================================================
// Descriptors (d12_infra.cpp)
// ============================================================================================

// Non-shader-visible heap with a free list. Thread-safe. Grows by blocks.
class CpuDescriptorHeap
{
public:
    bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 blockSize);
    void Shutdown();
    D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    u32 Increment() const { return m_increment; }

private:
    ID3D12Device* m_device = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE m_type{};
    u32 m_blockSize = 0;
    u32 m_increment = 0;
    std::mutex m_lock;
    std::vector<ID3D12DescriptorHeap*> m_blocks;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_free;
};

// The shader-visible CBV/SRV/UAV heap. [0, Base()) is the persistent bindless range
// (BindlessAllocator); the rest is used as a ring of chunks. A chunk belongs to one context and one
// frame; chunks of a frame are reusable once that frame's fence completed.
class GpuDescriptorRing
{
public:
    bool Init(ID3D12Device* device, FenceTimeline* fence, u32 capacity);
    void Shutdown();
    ID3D12DescriptorHeap* Heap() const { return m_heap; }
    u32 Base() const { return m_base; }
    u32 Capacity() const { return m_capacity; }
    // Hands out `count` contiguous descriptors (count <= kDescriptorChunk) as a chunk tagged with
    // `frame`. Thread-safe; waits for the GPU when the ring would overtake an unfinished frame.
    u32 AcquireChunk(u32 count, u64 frame);
    // The fence value after which everything tagged with `frame` is free.
    void EndFrame(u64 frame, u64 fenceValue);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(u32 index) const { return {m_cpuBase.ptr + SIZE_T(index) * m_increment}; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(u32 index) const { return {m_gpuBase.ptr + u64(index) * m_increment}; }
    u32 Increment() const { return m_increment; }

private:
    ID3D12DescriptorHeap* m_heap = nullptr;
    FenceTimeline* m_fence = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuBase{};
    u32 m_increment = 0;
    u32 m_capacity = 0;
    u32 m_base = 0; // first ring index (the bindless range lies below it)
    std::mutex m_lock;
    u32 m_head = 0; // next free index
    struct FrameMark
    {
        u64 frame;
        u32 begin;
        u32 end;
        u64 fence; // 0 until the frame ended
    };
    std::vector<FrameMark> m_marks; // oldest first
};

// Per-context sub-allocator over GpuDescriptorRing chunks (not thread-safe, one per context).
class DescriptorAllocator
{
public:
    void Init(GpuDescriptorRing* ring) { m_ring = ring; }
    // Returns the first index of `count` contiguous shader-visible descriptors valid for `frame`.
    u32 Allocate(u32 count, u64 frame);

private:
    GpuDescriptorRing* m_ring = nullptr;
    u64 m_frame = ~0ull;
    u32 m_next = 0;
    u32 m_end = 0;
};

// The shader-visible sampler heap. Tables are keyed by the sampler objects they hold and live
// until the heap fills up (then the cache starts over in a new heap). Thread-safe.
class SamplerTableCache
{
public:
    bool Init(ID3D12Device* device, Device* owner);
    void Shutdown();
    ID3D12DescriptorHeap* Heap() const { return m_heap; }
    // `ids` identify the samplers (SamplerState::Id(), 0 = default sampler), `cpu` their descriptors.
    D3D12_GPU_DESCRIPTOR_HANDLE Get(const u32* ids, const D3D12_CPU_DESCRIPTOR_HANDLE* cpu, u32 count);

    // A full heap is replaced by a new one (open lists keep reading the old one, which is released
    // kFramesInFlight + 1 frames later). The generation counts the replacements: a list that bound
    // the heap of another generation must bind the current one before it uses a table.
    u32 Generation() const { return m_generation.load(std::memory_order_acquire); }
    ID3D12DescriptorHeap* Current(u32& generation);
    // Get() plus the generation of the heap the table lives in.
    D3D12_GPU_DESCRIPTOR_HANDLE Get(const u32* ids, const D3D12_CPU_DESCRIPTOR_HANDLE* cpu, u32 count, u32& generation);
    // Hands replaced heaps no list can reference any more to the device's releaser.
    void EndFrame(u64 frame);

private:
    ID3D12Device* m_device = nullptr;
    Device* m_owner = nullptr;
    ID3D12DescriptorHeap* m_heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuBase{};
    u32 m_increment = 0;
    u32 m_used = 0;
    std::shared_mutex m_lock;
    std::unordered_map<u64, D3D12_GPU_DESCRIPTOR_HANDLE> m_tables;
    std::atomic<u32> m_generation{0};
    std::vector<std::pair<ID3D12DescriptorHeap*, u64>> m_retired; // replaced heap, frame of the replacement
};

// Null descriptors for every view dimension, in the device's CPU heaps (read-only after Init).
struct NullDescriptors
{
    // By D3D12_SRV_DIMENSION; the acceleration structure one (location 0) only on DXR devices.
    D3D12_CPU_DESCRIPTOR_HANDLE srv[D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE + 1]{};
    D3D12_CPU_DESCRIPTOR_HANDLE uav[D3D12_UAV_DIMENSION_TEXTURE3D + 1]{};        // by D3D12_UAV_DIMENSION
    D3D12_CPU_DESCRIPTOR_HANDLE cbv{};
    D3D12_CPU_DESCRIPTOR_HANDLE sampler{}; // D3D11's default sampler state
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};     // null RTV (Texture2D, R8G8B8A8_UNORM)
    // 64 KB of zeros in upload memory: the root CBV of a slot the shader reads but nobody bound
    // (D3D11 returns zeros there; a D3D12 root CBV must be a valid address).
    D3D12_GPU_VIRTUAL_ADDRESS zeroCb = 0;
    bool Init(Device* device);
};

// The persistent SRV range [0, Capacity()) of the shader-visible heap (d12_native.cpp). Null-filled
// at init; contiguous allocations; freed slots return after the current frame's fence. A slot
// written from a View is nulled (after the fence) when the view dies first. Thread-safe.
class BindlessAllocator
{
public:
    bool Init(Device* device, u32 capacity);
    void Shutdown();
    u32 Capacity() const { return m_capacity; }
    u32 Allocate(u32 count); // kBindlessInvalid when no contiguous range is free
    void Free(u32 index, u32 count);
    // Copies the view's descriptor (null view = null SRV) into the slot.
    void Write(u32 index, View* view);
    // Creates a descriptor in the slot (`desc` null = null Texture2D SRV).
    void WriteNative(u32 index, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc);
    // View::~View: slots still holding the view's descriptor become null.
    void ForgetView(View& view);
    // Device::EndFrame: frees and nulls of `frame` wait for `fenceValue`; completed ones are applied.
    void EndFrame(u64 frame, u64 fenceValue);

private:
    bool Valid(u32 index, u32 count) const { return count && index < m_capacity && count <= m_capacity - index; }
    void WriteNull(u32 index); // m_lock held
    void Release(u32 index, u32 count); // m_lock held: back to the free list
    // A free (the range returns) or the null of a dead view's slot (only if not written since).
    struct Pending
    {
        u64 frame = 0;
        u64 fence = 0; // 0 until the frame ended
        u32 index = 0;
        u32 count = 0;
        u32 generation = 0;
        bool free = false;
    };
    Device* m_device = nullptr;
    u32 m_capacity = 0;
    std::mutex m_lock;
    std::vector<std::pair<u32, u32>> m_free; // (index, count), sorted by index, coalesced
    std::vector<u64> m_owner;                // View::id whose descriptor the slot holds, 0 = none
    std::vector<u32> m_generation;           // bumped by every write and free
    std::vector<Pending> m_pending;
};

// ============================================================================================
// Memory (d12_infra.cpp)
// ============================================================================================

struct UploadPage
{
    ID3D12Resource* resource = nullptr;
    u8* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    u64 size = 0;
    u64 frame = 0;      // frame of the dynamic data in it (dynamic pages)
    u64 fence = 0;      // fence after which it is free (init pages)
    bool dedicated = false; // sized for one big request, destroyed instead of pooled
};

struct UploadAlloc
{
    ID3D12Resource* resource = nullptr;
    u64 offset = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    u8* cpu = nullptr;
    u64 size = 0;
};

// Pool of UPLOAD-heap pages for per-frame dynamic data. Thread-safe. Pages of a frame come back
// after that frame's fence (EndFrame); oversized requests get dedicated pages.
class UploadPagePool
{
public:
    bool Init(ID3D12Device* device, FenceTimeline* fence);
    void Shutdown();
    UploadPage* Acquire(u64 minSize, u64 frame);
    void EndFrame(u64 frame, u64 fenceValue);
    void Recycle(); // returns pages whose frames completed
    u64 BytesInFlight() const { return m_inFlight.load(); }

private:
    ID3D12Device* m_device = nullptr;
    FenceTimeline* m_fence = nullptr;
    std::mutex m_lock;
    std::vector<UploadPage*> m_free;
    std::vector<UploadPage*> m_used;                   // tagged with their frame
    std::vector<std::pair<u64, u64>> m_frameFences;    // frame -> fence
    std::atomic<u64> m_inFlight{0};
};

// Per-context linear allocator over UploadPagePool pages (not thread-safe).
class UploadAllocator
{
public:
    void Init(UploadPagePool* pool) { m_pool = pool; }
    UploadAlloc Allocate(u64 size, u64 alignment, u64 frame);

private:
    UploadPagePool* m_pool = nullptr;
    UploadPage* m_page = nullptr;
    u64 m_offset = 0;
    u64 m_frame = ~0ull;
};

// DEFAULT-heap pages that hold static vertex/index/constant buffers (256-byte aligned ranges).
// Thread-safe. Pages are never transitioned explicitly: buffers promote from COMMON implicitly.
class BufferPageAllocator
{
public:
    struct Range
    {
        ID3D12Resource* resource = nullptr; // page or dedicated buffer
        u64 offset = 0;
        u64 size = 0;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        u8* cpu = nullptr; // UPLOAD-heap allocators only (persistently mapped)
        u32 page = ~0u; // ~0u = dedicated
    };
    // DEFAULT heap: static buffers. UPLOAD heap: the first version of dynamic buffers (their
    // initial data or zeros), valid until the buffer is destroyed.
    bool Init(ID3D12Device* device, D3D12_HEAP_TYPE heapType);
    void Shutdown();
    bool Allocate(u64 size, Range& out);
    void Free(const Range& range); // call through DeferredReleaser when the GPU may still read it

private:
    struct Page
    {
        ID3D12Resource* resource = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        u8* cpu = nullptr;
        std::vector<std::pair<u64, u64>> free; // (offset, size), sorted by offset, coalesced
    };
    ID3D12Device* m_device = nullptr;
    D3D12_HEAP_TYPE m_heapType = D3D12_HEAP_TYPE_DEFAULT;
    std::mutex m_lock;
    std::vector<Page> m_pages;
};

// Creation-time data (resources created with initial data, from any thread). Copies are recorded
// on a private command list of `queue` and submitted before the immediate context's next
// submission, or earlier when enough piled up. On a copy queue the direct queue waits for each
// submission right away, so everything the direct queue runs afterwards sees the data. Thread-safe.
class InitUploader
{
public:
    bool Init(Device* device, ID3D12CommandQueue* queue, FenceTimeline* fence, D3D12_COMMAND_LIST_TYPE type);
    void Shutdown();
    bool CopyQueue() const { return m_type == D3D12_COMMAND_LIST_TYPE_COPY; }
    // Buffer: `size` bytes into (dst, dstOffset). The page/dedicated buffer must be in COMMON.
    void UploadBuffer(ID3D12Resource* dst, u64 dstOffset, const void* data, u64 size);
    // Texture subresources from D3D11_SUBRESOURCE_DATA (row/slice pitches of the caller).
    // Direct queue: `resource` is created in COPY_DEST and left in `finalState`. Copy queue: it is
    // created in COMMON and decays back to COMMON after the copy (`finalState` is ignored).
    void UploadTexture(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, u32 firstSubresource,
        u32 subresourceCount, const D3D11_SUBRESOURCE_DATA* data, D3D12_RESOURCE_STATES finalState);
    // Submits pending copies (called by the immediate context before its own submissions).
    void Flush();
    bool Pending();
    // -dxstats: bytes and submissions since the last call.
    void TakeStats(u64& bytes, u64& submissions)
    {
        bytes = m_statBytes.exchange(0, std::memory_order_relaxed);
        submissions = m_statSubmissions.exchange(0, std::memory_order_relaxed);
    }

private:
    UploadAlloc AllocateStaging(u64 size, u64 alignment);
    void EnsureList();
    Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    FenceTimeline* m_fence = nullptr;
    D3D12_COMMAND_LIST_TYPE m_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    std::atomic<u64> m_statBytes{0};
    std::atomic<u64> m_statSubmissions{0};
    std::mutex m_lock;
    ID3D12CommandAllocator* m_allocators[kFramesInFlight + 1]{};
    u64 m_allocatorFence[kFramesInFlight + 1]{};
    u32 m_allocatorIndex = 0;
    ID3D12GraphicsCommandList* m_list = nullptr;
    bool m_open = false;
    u64 m_pendingBytes = 0;
    UploadPage* m_page = nullptr;
    u64 m_pageOffset = 0;
    std::vector<UploadPage*> m_pagesInList;
    std::vector<UploadPage*> m_pagesInFlight;
    std::vector<UploadPage*> m_freePages;
    std::vector<D3D12_RESOURCE_BARRIER> m_finalBarriers;
    // Destinations of the copies hold a reference until their list completed: a resource released
    // right after its creation must outlive the copy into it.
    std::vector<ID3D12Resource*> m_targetsInList;
    std::vector<std::pair<u64, ID3D12Resource*>> m_targetsInFlight; // (fence, resource)
    void KeepTarget(ID3D12Resource* resource); // m_lock held
    void ReleaseCompletedTargets(bool all);    // m_lock held
};

// ============================================================================================
// Pipelines (d12_infra.cpp)
// ============================================================================================

// Root parameter indices of the graphics root signature.
namespace GfxRoot
{
constexpr u32 VS_CBV0 = 0;                           // VS b0..b4 root CBVs: 0..4
constexpr u32 VS_CBV_TABLE = VS_CBV0 + kRootCBVsVS;  // 5: VS b5..b13
constexpr u32 VS_SRV = 6;
constexpr u32 VS_SMP = 7;
constexpr u32 PS_CBV0 = 8;                           // PS b0..b4: 8..12
constexpr u32 PS_CBV_TABLE = PS_CBV0 + kRootCBVsPS;  // 13: PS b5..b13
constexpr u32 PS_SRV = 14;
constexpr u32 PS_SMP = 15;
constexpr u32 PS_UAV = 16;                           // u0..u7 (OMSetRenderTargetsAndUnorderedAccessViews)
constexpr u32 GS_CBV0 = 17, GS_CBV_TABLE = 18, GS_SRV = 19, GS_SMP = 20;
constexpr u32 HS_CBV0 = 21, HS_CBV_TABLE = 22, HS_SRV = 23, HS_SMP = 24;
constexpr u32 DS_CBV0 = 25, DS_CBV_TABLE = 26, DS_SRV = 27, DS_SMP = 28;
constexpr u32 Count = 29;
} // namespace GfxRoot

// Root parameter indices of the compute root signature.
namespace CsRoot
{
constexpr u32 CBV0 = 0;                          // b0..b3: 0..3
constexpr u32 CBV_TABLE = CBV0 + kRootCBVsCS;    // 4: b4..b13
constexpr u32 SRV = 5;
constexpr u32 UAV = 6;
constexpr u32 SMP = 7;
constexpr u32 Count = 8;
} // namespace CsRoot

struct RootSignatures
{
    ID3D12RootSignature* graphics = nullptr;
    ID3D12RootSignature* compute = nullptr;
    bool Init(ID3D12Device* device);
    void Shutdown();
    // [dx12:mesh] begin root
    // The graphics layout with VS parameters at MESH and GS parameters at AMPLIFICATION visibility
    // (same GfxRoot indices), no input assembler. Null where mesh shaders are unavailable.
    ID3D12RootSignature* mesh = nullptr;
    void InitMesh(ID3D12Device* device, bool version11); // d12_mesh.cpp, called by Init
    void ShutdownMesh();                                 // d12_mesh.cpp, called by Shutdown
    // [dx12:mesh] end root
};

class Shader;
class InputLayout;
class BlendState;
class DepthStencilState;
class RasterizerState;

// Everything a graphics PSO depends on. Built by the context when PSO-relevant state changed.
struct GraphicsPsoDesc
{
    Shader* vs = nullptr;
    Shader* hs = nullptr;
    Shader* ds = nullptr;
    Shader* gs = nullptr;
    Shader* ps = nullptr;
    InputLayout* inputLayout = nullptr;
    BlendState* blend = nullptr;           // null = D3D11 default
    DepthStencilState* depth = nullptr;    // null = D3D11 default
    RasterizerState* raster = nullptr;     // null = D3D11 default
    UINT sampleMask = 0xffffffff;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    UINT numRTVs = 0;
    DXGI_FORMAT rtvFormats[kMaxRTVs]{};
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample{1, 0};
    // [dx12:mesh] begin pso
    u8 meshKind = 0; // 0 = vertex pipeline; otherwise a mesh pipeline (PipelineCache::GraphicsMesh)
    // [dx12:mesh] end pso
    u64 Hash() const; // content hash (uses the objects' content hashes, never pointers)
};

// PSO cache: in-memory map by content hash + persistent ID3D12PipelineLibrary. Thread-safe.
class PipelineCache
{
public:
    bool Init(Device* device, const wchar_t* libraryPath);
    void Shutdown(); // saves the library
    void Save();
    // Saves only when pipelines were added since the last save (a level's precache, the shutdown).
    void SaveIfDirty();
    // Returns null (and logs once) when the driver refuses the state.
    ID3D12PipelineState* Graphics(const GraphicsPsoDesc& desc, u64 hash);
    ID3D12PipelineState* Compute(Shader* cs);
    // Pipelines of native passes (d12_native.cpp), named in the library by `key`: the caller's content
    // hash (code, root signature and every state). `persist` false = memory only (foreign root
    // signatures, whose content is unknown).
    ID3D12PipelineState* NativeCompute(u64 key, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, bool persist);
    ID3D12PipelineState* NativeStream(u64 key, const D3D12_PIPELINE_STATE_STREAM_DESC& desc, bool persist);
    u32 Count() const { return m_count.load(); }
    // [dx12:mesh] begin pipelines
    // Stream PSO of a mesh pipeline (desc.meshKind != 0), d12_mesh.cpp; Graphics() hands over.
    ID3D12PipelineState* GraphicsMesh(const GraphicsPsoDesc& desc, u64 hash);
    // [dx12:mesh] end pipelines

private:
    Device* m_device = nullptr;
    ID3D12PipelineLibrary* m_library = nullptr;
    std::vector<u8> m_libraryBlob; // must outlive m_library
    std::wstring m_path;
    bool m_dirty = false;
    std::shared_mutex m_lock;
    std::unordered_map<u64, ID3D12PipelineState*> m_pipelines;
    std::atomic<u32> m_count{0};
    // Caches a pipeline built outside the locks; when another thread was first, `pso` is released
    // and the cached one returned. Only the winner goes into the library (`storeName` non-null).
    ID3D12PipelineState* Insert(u64 key, ID3D12PipelineState* pso, const wchar_t* storeName);
};

// ============================================================================================
// Queries (d12_infra.cpp owns the heaps, d12_objects.cpp the Query objects)
// ============================================================================================

class QueryHeapPool
{
public:
    bool Init(ID3D12Device* device);
    void Shutdown();
    // Slot allocation for occlusion (both D3D11 occlusion types) and timestamp queries.
    // Returns the heap, the index inside it and the readback memory of the 8-byte result.
    struct Slot
    {
        ID3D12QueryHeap* heap = nullptr;
        u32 index = 0;
        ID3D12Resource* readback = nullptr; // READBACK buffer of the heap, 8 bytes per slot
        const u64* result = nullptr;        // persistently mapped result of this slot
    };
    Slot Allocate(D3D12_QUERY_HEAP_TYPE type);
    void Free(D3D12_QUERY_HEAP_TYPE type, const Slot& slot);

private:
    struct Heap
    {
        ID3D12QueryHeap* heap = nullptr;
        ID3D12Resource* readback = nullptr;
        u64* mapped = nullptr;
        std::vector<u32> free;
    };
    ID3D12Device* m_device = nullptr;
    std::mutex m_lock;
    std::vector<Heap> m_occlusion;
    std::vector<Heap> m_timestamp;
};

// ============================================================================================
// COM plumbing (d12_objects.cpp)
// ============================================================================================

// Private data store for SetPrivateData/GetPrivateData (debug names mostly). Thread-safe.
class PrivateData
{
public:
    HRESULT Get(REFGUID guid, UINT* size, void* data);
    HRESULT Set(REFGUID guid, UINT size, const void* data);
    HRESULT SetInterface(REFGUID guid, const IUnknown* data);
    ~PrivateData();

private:
    std::mutex m_lock;
    struct Entry
    {
        GUID guid;
        std::vector<u8> bytes;
        IUnknown* object = nullptr;
    };
    std::vector<Entry> m_entries;
};

// Common base of every ID3D11DeviceChild the layer creates. `Iface` is the most derived D3D11
// interface; QueryInterface answers it and all of its bases.
template <class Iface>
class DeviceChild : public Iface
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override;

    Device* Owner() const { return m_device; }

protected:
    explicit DeviceChild(Device* device) : m_device(device) {}
    virtual ~DeviceChild() = default;
    // Called when the last reference goes. Default: delete this.
    virtual void Destroy() { delete this; }
    // Called with the name set through WKPDID_D3DDebugObjectName.
    virtual void OnDebugName(const char* /*name*/) {}
    // IIDs answered besides IUnknown/ID3D11DeviceChild and the Iface chain.
    virtual bool SupportsExtra(REFIID /*riid*/) const { return false; }

    Device* m_device;
    std::atomic<ULONG> m_refs{1};
    PrivateData m_private;
};

// ============================================================================================
// Resources (d12_objects.cpp)
// ============================================================================================

// Tracked state of the subresources of a texture. `uniform` = every subresource is in `all`.
struct SubresourceStates
{
    bool uniform = true;
    D3D12_RESOURCE_STATES all = D3D12_RESOURCE_STATE_COMMON;
    std::vector<D3D12_RESOURCE_STATES> per; // size = subresource count when !uniform
};

// A range of subresources: mips x array slices x planes (D3D12 subresource indexing).
struct SubRange
{
    u32 firstMip = 0, mipCount = 1;
    u32 firstSlice = 0, sliceCount = 1;
    u32 firstPlane = 0, planeCount = 1;
};

// Current CPU-written version of a dynamic buffer (upload memory). Replaced by every DISCARD map.
struct DynamicVersion
{
    UploadAlloc alloc;
    u32 version = 0;
    // d12_context_ops.cpp: the persistent heap `alloc` came from (null: the first version or frame
    // memory). A version outlives its frame; it returns to the heap when the buffer is renamed again.
    class DynamicHeap* heap = nullptr;
};

// Data shared by all resource kinds. Textures are tracked (explicit barriers); static buffers live
// in DEFAULT buffer pages (implicit promotion, never tracked); dynamic buffers live in upload
// memory (GENERIC_READ, never tracked); staging textures live in READBACK/UPLOAD buffers laid out by
// placed footprints.
class Resource
{
public:
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    UINT bindFlags = 0;
    UINT cpuAccess = 0;
    UINT miscFlags = 0;

    // Textures (and buffers too big for a page): the DEFAULT-heap resource.
    ID3D12Resource* native = nullptr;
    D3D12_RESOURCE_DESC nativeDesc{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 mipLevels = 1;
    u32 arraySize = 1;     // for 3D textures: 1
    u32 planeCount = 1;
    u32 SubresourceCount() const { return mipLevels * arraySize * planeCount; }
    u32 Subresource(u32 mip, u32 slice, u32 plane) const { return mip + slice * mipLevels + plane * mipLevels * arraySize; }
    SubresourceStates states; // textures only; owned by the immediate context's tracking

    // Static buffers.
    BufferPageAllocator::Range range;
    u64 byteWidth = 0;

    // Dynamic buffers (USAGE_DYNAMIC): current version; `current.version` changes on every DISCARD.
    DynamicVersion current;

    // Staging textures (USAGE_STAGING) and dynamic textures: CPU-visible linear memory.
    ID3D12Resource* linear = nullptr;         // READBACK (read) or UPLOAD (write) buffer
    u8* linearCpu = nullptr;                   // persistently mapped
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<u32> rowCounts;
    std::vector<u64> rowSizes;
    u64 lastGpuWrite = 0;                      // fence value of the last copy into `linear` (readback)
    bool gpuWritePending = false;              // a copy was recorded into `linear` but not submitted

    // Async compute: the job the resource is lent to between its fork and join (immediate-owned);
    // a read-shared loan lets the direct queue keep reading it.
    AsyncJob* lentTo = nullptr;
    bool lentShared = false;

    // Tracked buffers decay to COMMON at the end of every ExecuteCommandLists: index in
    // Device::trackedBuffers (~0u = not registered).
    u32 trackedBufferSlot = ~0u;

    // [dx12:ds] begin resource
    TexturePlacement* placement = nullptr; // placed in a texture heap (Device::TexturePlace)
    // [dx12:ds] end resource

    // Back buffer proxy: the swap chain resolves the current buffer.
    SwapChain* swapChain = nullptr;
    bool IsBackBufferProxy() const { return swapChain; }
    // The resource commands must act on (back buffer proxy: the current buffer's Resource).
    Resource* Actual();
    ID3D12Resource* ActualNative() { return Actual()->native; }

    bool IsBuffer() const { return dimension == D3D11_RESOURCE_DIMENSION_BUFFER; }
    // Tracked = has its own DEFAULT-heap resource: every texture except staging ones, and buffers
    // that are GPU-written (DEFAULT without initial data, UAV), have views (SRV bind) or are too big
    // for a page. Static buffers with initial data live in untracked pages (copies into them are
    // refused with a log line).
    bool IsTracked() { return Actual()->native; }
    // GPU address of a buffer: static range or the current dynamic version.
    D3D12_GPU_VIRTUAL_ADDRESS BufferAddress() const
    {
        return usage == D3D11_USAGE_DYNAMIC ? current.alloc.gpu : (native ? native->GetGPUVirtualAddress() : range.gpu);
    }
    u32 DynamicVersionId() const { return current.version; }

    virtual ~Resource();
    // From any ID3D11Resource the layer created.
    static Resource* From(ID3D11Resource* resource);
};

// d12_context_ops.cpp: returns the current version of a dynamic buffer to its heap (it is reused once
// the GPU is done with it). Buffer::Destroy calls it; without the call that version's block leaks.
void RetireDynamicVersion(Resource* resource);

class Buffer final : public DeviceChild<ID3D11Buffer>, public Resource
{
public:
    static HRESULT Create(Device* device, const D3D11_BUFFER_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Buffer** out);
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dim) override { *dim = D3D11_RESOURCE_DIMENSION_BUFFER; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* desc) override { *desc = m_desc; }
    const D3D11_BUFFER_DESC& Desc() const { return m_desc; }

private:
    explicit Buffer(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    void OnDebugName(const char* name) override;
    D3D11_BUFFER_DESC m_desc{};
};

class Texture1D final : public DeviceChild<ID3D11Texture1D>, public Resource
{
public:
    static HRESULT Create(Device* device, const D3D11_TEXTURE1D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture1D** out);
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dim) override { *dim = D3D11_RESOURCE_DIMENSION_TEXTURE1D; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE1D_DESC* desc) override { *desc = m_desc; }

private:
    explicit Texture1D(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    void OnDebugName(const char* name) override;
    D3D11_TEXTURE1D_DESC m_desc{};
};

class Texture2D final : public DeviceChild<ID3D11Texture2D>, public Resource
{
public:
    static HRESULT Create(Device* device, const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture2D** out);
    // Wraps a swap chain buffer (index >= 0) or makes the back buffer proxy (index < 0).
    static Texture2D* CreateForSwapChain(Device* device, SwapChain* chain, ID3D12Resource* buffer, const D3D11_TEXTURE2D_DESC& desc);
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dim) override { *dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) override { *desc = m_desc; }
    const D3D11_TEXTURE2D_DESC& Desc() const { return m_desc; }

private:
    explicit Texture2D(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    void OnDebugName(const char* name) override;
    D3D11_TEXTURE2D_DESC m_desc{};
};

class Texture3D final : public DeviceChild<ID3D11Texture3D>, public Resource
{
public:
    static HRESULT Create(Device* device, const D3D11_TEXTURE3D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture3D** out);
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dim) override { *dim = D3D11_RESOURCE_DIMENSION_TEXTURE3D; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE3D_DESC* desc) override { *desc = m_desc; }

private:
    explicit Texture3D(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    void OnDebugName(const char* name) override;
    D3D11_TEXTURE3D_DESC m_desc{};
};

// ============================================================================================
// Views (d12_objects.cpp)
// ============================================================================================

enum class ViewKind : u8
{
    SRV,
    RTV,
    DSV,
    UAV
};

// Data shared by all view kinds. A view holds a reference on its resource (D3D11 semantics).
class View
{
public:
    ViewKind kind = ViewKind::SRV;
    ID3D11Resource* resourceIface = nullptr; // AddRef'd
    Resource* resource = nullptr;
    SubRange range;                          // subresources the view covers
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};       // CPU descriptor (RTV/DSV/SRV/UAV heap)
    // Back buffer proxy RTVs: one descriptor per swap chain buffer, resolved at bind time.
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> perBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE Descriptor(); // cpu, or perBuffer[current back buffer]
    // SRVs: the D3D12 dimension of `cpu`.
    D3D12_SRV_DIMENSION srvDimension = D3D12_SRV_DIMENSION_UNKNOWN;
    // DSV read-only flags (D3D11_DSV_READ_ONLY_DEPTH/STENCIL) - decide the tracked state.
    UINT dsvFlags = 0;
    // SRV dimension adapters (a Texture2D declaration bound to a one-slice 2D-array view and the
    // reverse), created on demand by the context and cached here. Thread-safe.
    D3D12_CPU_DESCRIPTOR_HANDLE AdaptedSrv(D3D12_SRV_DIMENSION wanted, bool& ok);
    u64 id = 0; // unique, never reused (keys descriptor caches)
    // Bindless slots written from this view (guarded by the device's BindlessAllocator).
    std::vector<u32> bindlessSlots;
    // Acceleration structure SRVs (CreateAccelerationStructureView): the TLAS, referenced; `resource`
    // is then a small untracked carrier buffer, so the view is never transitioned.
    ID3D12Resource* accelerationStructure = nullptr;

    virtual ~View();
    static View* From(ID3D11ShaderResourceView* view);
    static View* From(ID3D11RenderTargetView* view);
    static View* From(ID3D11DepthStencilView* view);
    static View* From(ID3D11UnorderedAccessView* view);

protected:
    std::mutex m_adaptLock;
    std::vector<std::pair<D3D12_SRV_DIMENSION, D3D12_CPU_DESCRIPTOR_HANDLE>> m_adapted;
};

class ShaderResourceView final : public DeviceChild<ID3D11ShaderResourceView>, public View
{
public:
    static HRESULT Create(Device* device, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ShaderResourceView** out);
    // RAYTRACING_ACCELERATION_STRUCTURE SRV of `tlas` (null = an empty one, location 0).
    static HRESULT CreateAccelerationStructure(Device* device, ID3D12Resource* tlas, ShaderResourceView** out);
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** resource) override;
    void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* desc) override { *desc = m_desc; }

private:
    explicit ShaderResourceView(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    D3D11_SHADER_RESOURCE_VIEW_DESC m_desc{};
};

class RenderTargetView final : public DeviceChild<ID3D11RenderTargetView>, public View
{
public:
    static HRESULT Create(Device* device, ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc, RenderTargetView** out);
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** resource) override;
    void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* desc) override { *desc = m_desc; }

private:
    explicit RenderTargetView(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    D3D11_RENDER_TARGET_VIEW_DESC m_desc{};
};

class DepthStencilView final : public DeviceChild<ID3D11DepthStencilView>, public View
{
public:
    static HRESULT Create(Device* device, ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, DepthStencilView** out);
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** resource) override;
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* desc) override { *desc = m_desc; }

private:
    explicit DepthStencilView(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    D3D11_DEPTH_STENCIL_VIEW_DESC m_desc{};
};

class UnorderedAccessView final : public DeviceChild<ID3D11UnorderedAccessView>, public View
{
public:
    static HRESULT Create(Device* device, ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, UnorderedAccessView** out);
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** resource) override;
    void STDMETHODCALLTYPE GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC* desc) override { *desc = m_desc; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC nativeDesc{};
    // Structured buffer views: a raw view of the same bytes, which ClearUnorderedAccessViewUint uses
    // (D3D12 clears no structured UAV).
    D3D12_CPU_DESCRIPTOR_HANDLE rawAlias{};

private:
    explicit UnorderedAccessView(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    D3D11_UNORDERED_ACCESS_VIEW_DESC m_desc{};
};

// ============================================================================================
// State objects, shaders, input layouts (d12_objects.cpp). Identical descriptions return the same
// object (D3D11 semantics); the device keeps the caches.
// ============================================================================================

class BlendState final : public DeviceChild<ID3D11BlendState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC* desc) override { *desc = desc11; }
    D3D11_BLEND_DESC desc11{};
    D3D12_BLEND_DESC desc12{};
    u64 hash = 0;
    explicit BlendState(Device* device) : DeviceChild(device) {}
};

class DepthStencilState final : public DeviceChild<ID3D11DepthStencilState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC* desc) override { *desc = desc11; }
    D3D11_DEPTH_STENCIL_DESC desc11{};
    D3D12_DEPTH_STENCIL_DESC desc12{};
    u64 hash = 0;
    explicit DepthStencilState(Device* device) : DeviceChild(device) {}
};

class RasterizerState final : public DeviceChild<ID3D11RasterizerState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC* desc) override { *desc = desc11; }
    D3D11_RASTERIZER_DESC desc11{};
    D3D12_RASTERIZER_DESC desc12{};
    bool scissorEnable = false; // D3D12 always scissors: off = scissor rect of the viewport
    u64 hash = 0;
    explicit RasterizerState(Device* device) : DeviceChild(device) {}
};

class SamplerState final : public DeviceChild<ID3D11SamplerState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC* desc) override { *desc = desc11; }
    D3D11_SAMPLER_DESC desc11{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{}; // in the device's CPU sampler heap
    u32 id = 0;                        // unique per state (0 = the default sampler)
    explicit SamplerState(Device* device) : DeviceChild(device) {}

protected:
    void Destroy() override;
};

// What a shader binds, from reflection. Slots are D3D11 register numbers.
struct ShaderBindings
{
    u16 cbMask = 0;            // bit per constant buffer slot
    u8 cbCount = 0;            // highest used slot + 1
    u8 srvCount = 0;           // highest used SRV slot + 1
    u32 srvMask = 0;
    D3D12_SRV_DIMENSION srvDims[kMaxSRVs]{}; // declared dimension per slot (UNKNOWN = unused)
    u8 samplerCount = 0;       // highest used sampler slot + 1
    u16 samplerMask = 0;
    u8 uavCount = 0;           // highest used UAV slot + 1
    u8 uavMask = 0;
    D3D12_UAV_DIMENSION uavDims[kMaxUAVs]{};
};

class Shader
{
public:
    Stage stage = Stage::VS;
    std::vector<u8> bytecode;
    ShaderBindings bindings;
    u64 hash = 0;
    u8 kind = 0;       // program kind (kKind*): a mesh shader sits in the VS slot, amplification in GS
    bool dxil = false; // DXC bytecode (a pipeline never mixes DXIL and DXBC stages)
    // VS: input signature elements (semantic name upper-case + index) for input layout validation.
    std::vector<std::pair<std::string, u32>> inputs;
    // Compute: the PSO is per shader (created on first dispatch by the pipeline cache).
    ID3D12PipelineState* computePso = nullptr;
    // WKPDID_D3DDebugObjectName (the engine's file name), shown when a pipeline is refused.
    std::string name;
    bool Reflect(); // fills bindings, hash, inputs from bytecode
    virtual ~Shader() = default;
    static Shader* From(ID3D11DeviceChild* shader, Stage stage);
};

template <class Iface, Stage S>
class ShaderObject final : public DeviceChild<Iface>, public Shader
{
public:
    explicit ShaderObject(Device* device) : DeviceChild<Iface>(device) { stage = S; }

protected:
    void OnDebugName(const char* debugName) override { name = debugName; }
};
using VertexShader = ShaderObject<ID3D11VertexShader, Stage::VS>;
using HullShader = ShaderObject<ID3D11HullShader, Stage::HS>;
using DomainShader = ShaderObject<ID3D11DomainShader, Stage::DS>;
using GeometryShader = ShaderObject<ID3D11GeometryShader, Stage::GS>;
using PixelShader = ShaderObject<ID3D11PixelShader, Stage::PS>;
using ComputeShader = ShaderObject<ID3D11ComputeShader, Stage::CS>;

class InputLayout final : public DeviceChild<ID3D11InputLayout>
{
public:
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements; // SemanticName points into `names`
    std::vector<std::string> names;
    u64 hash = 0;
    explicit InputLayout(Device* device) : DeviceChild(device) {}
};

// ============================================================================================
// Queries and command lists (d12_objects.cpp; the context drives them)
// ============================================================================================

class Query final : public DeviceChild<ID3D11Query>
{
public:
    static HRESULT Create(Device* device, const D3D11_QUERY_DESC& desc, Query** out);
    UINT STDMETHODCALLTYPE GetDataSize() override;
    void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC* desc) override { *desc = m_desc; }

    D3D11_QUERY_DESC m_desc{};
    QueryHeapPool::Slot slot;     // occlusion / timestamp
    // Fence value of the submission that carries the End(); 0 = ended in a list not yet submitted,
    // ~0ull = never ended.
    u64 fence = ~0ull;
    bool active = false;          // between Begin and End
    // Occlusion: D3D12 queries cannot span command lists. A submission while the query is open ends
    // the D3D12 query in the old list (its slot becomes a part) and begins it again in a new slot;
    // the result is the sum of the parts and the slot.
    std::vector<QueryHeapPool::Slot> parts;

private:
    explicit Query(Device* device) : DeviceChild(device) {}
    void Destroy() override;
};

// Recorded work of a deferred context (FinishCommandList).
class CommandList final : public DeviceChild<ID3D11CommandList>
{
public:
    UINT STDMETHODCALLTYPE GetContextFlags() override { return 0; }
    ID3D12GraphicsCommandList* list = nullptr;   // closed
    ID3D12CommandAllocator* allocator = nullptr; // owned by the deferred context's pool
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT; // COMPUTE: an async recording
    // First state each tracked subresource must be in when the list starts (the immediate context
    // inserts the transitions) and the state it leaves it in.
    struct Requirement
    {
        Resource* resource;
        u32 subresource; // ~0u = all
        D3D12_RESOURCE_STATES first;
        D3D12_RESOURCE_STATES last;
        bool transitioned = false; // the list itself changed its state (else it only read it)
    };
    std::vector<Requirement> requirements;
    std::vector<Resource*> pendingReadback;      // staging copies recorded in the list
    Context* recordedBy = nullptr;               // gets `list` back once it was submitted
    u32 asyncStamps = ~0u;                       // compute recording: its timestamp pair (d12_async.cpp)
    explicit CommandList(Device* device) : DeviceChild(device) {}

protected:
    void Destroy() override;
};

// Drops the references a list of staging copies holds (d12_context_ops.cpp).
void ReleaseStagingReferences(std::vector<Resource*>& resources);

// ============================================================================================
// Context (d12_context_state.cpp + d12_context_ops.cpp)
// ============================================================================================

// D3D11-level binding state of one stage.
struct StageBindings
{
    Shader* shader = nullptr;
    ID3D11DeviceChild* shaderIface = nullptr; // AddRef'd
    ID3D11Buffer* cb[kMaxCBs]{};              // AddRef'd
    u32 cbAppliedVersion[kMaxCBs]{};          // dynamic version last put into the root/table
    D3D12_GPU_VIRTUAL_ADDRESS cbAppliedAddress[kMaxCBs]{};
    ID3D11ShaderResourceView* srv[kMaxSRVs]{}; // AddRef'd
    ID3D11SamplerState* samplers[kMaxSamplers]{}; // AddRef'd
    bool cbDirty = true;
    bool srvDirty = true;
    bool samplerDirty = true;
};

// Per-context state tracking: the immediate context works on Resource::states; a deferred context
// keeps its own map and records the first requirement of every subresource.
class StateTracker
{
public:
    void Init(Context* context, bool deferred) { m_context = context; m_deferred = deferred; }
    // Ensures the subresources are in `state`; appends transitions to `barriers`.
    void Require(Resource* resource, const SubRange& range, D3D12_RESOURCE_STATES state,
        std::vector<D3D12_RESOURCE_BARRIER>& barriers);
    void RequireAll(Resource* resource, D3D12_RESOURCE_STATES state, std::vector<D3D12_RESOURCE_BARRIER>& barriers);
    // Deferred: moves the collected first/last states into the list and resets.
    void Harvest(std::vector<CommandList::Requirement>& out);
    void Reset();

private:
    struct Local
    {
        std::vector<D3D12_RESOURCE_STATES> first; // per subresource, 0xFFFFFFFF = untouched
        std::vector<D3D12_RESOURCE_STATES> current;
        bool transitioned = false;                // the list recorded a barrier for it
    };
    Context* m_context = nullptr;
    bool m_deferred = false;
    std::unordered_map<Resource*, Local> m_local;
};

class Context final : public DeviceChild<ID3D11DeviceContext>
{
public:
    Context(Device* device, bool deferred, u32 index);
    ~Context() override;

    // ---- ID3D11DeviceContext (all methods; see the .cpp files for which are functional) ----
    // d12_context_state.cpp
    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) override;
    void STDMETHODCALLTYPE PSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) override;
    void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* shader, ID3D11ClassInstance* const* inst, UINT numInst) override;
    void STDMETHODCALLTYPE PSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) override;
    void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* shader, ID3D11ClassInstance* const* inst, UINT numInst) override;
    void STDMETHODCALLTYPE DrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex) override;
    void STDMETHODCALLTYPE Draw(UINT vertexCount, UINT startVertex) override;
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) override;
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* layout) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* buffer, DXGI_FORMAT format, UINT offset) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance) override;
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) override;
    void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader* shader, ID3D11ClassInstance* const* inst, UINT numInst) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology) override;
    void STDMETHODCALLTYPE VSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) override;
    void STDMETHODCALLTYPE VSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) override;
    void STDMETHODCALLTYPE SetPredication(ID3D11Predicate* predicate, BOOL value) override;
    void STDMETHODCALLTYPE GSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) override;
    void STDMETHODCALLTYPE GSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT num, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) override;
    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT numRTVs, ID3D11RenderTargetView* const* rtvs,
        ID3D11DepthStencilView* dsv, UINT uavStart, UINT numUAVs, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) override;
    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* state, const FLOAT blendFactor[4], UINT sampleMask) override;
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* state, UINT stencilRef) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT num, ID3D11Buffer* const* targets, const UINT* offsets) override;
    void STDMETHODCALLTYPE DrawAuto() override;
    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer* args, UINT offset) override;
    void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer* args, UINT offset) override;
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override;
    void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer* args, UINT offset) override;
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* state) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT num, const D3D11_VIEWPORT* viewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT num, const D3D11_RECT* rects) override;
    void STDMETHODCALLTYPE HSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) override;
    void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader* shader, ID3D11ClassInstance* const* inst, UINT numInst) override;
    void STDMETHODCALLTYPE HSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) override;
    void STDMETHODCALLTYPE HSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) override;
    void STDMETHODCALLTYPE DSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) override;
    void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader* shader, ID3D11ClassInstance* const* inst, UINT numInst) override;
    void STDMETHODCALLTYPE DSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) override;
    void STDMETHODCALLTYPE DSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) override;
    void STDMETHODCALLTYPE CSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) override;
    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT start, UINT num, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) override;
    void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader* shader, ID3D11ClassInstance* const* inst, UINT numInst) override;
    void STDMETHODCALLTYPE CSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) override;
    void STDMETHODCALLTYPE CSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) override;
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) override;
    void STDMETHODCALLTYPE PSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) override;
    void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader** shader, ID3D11ClassInstance** inst, UINT* numInst) override;
    void STDMETHODCALLTYPE PSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) override;
    void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader** shader, ID3D11ClassInstance** inst, UINT* numInst) override;
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) override;
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** layout) override;
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT start, UINT num, ID3D11Buffer** buffers, UINT* strides, UINT* offsets) override;
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** buffer, DXGI_FORMAT* format, UINT* offset) override;
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) override;
    void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader** shader, ID3D11ClassInstance** inst, UINT* numInst) override;
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* topology) override;
    void STDMETHODCALLTYPE VSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) override;
    void STDMETHODCALLTYPE VSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) override;
    void STDMETHODCALLTYPE GetPredication(ID3D11Predicate** predicate, BOOL* value) override;
    void STDMETHODCALLTYPE GSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) override;
    void STDMETHODCALLTYPE GSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) override;
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT num, ID3D11RenderTargetView** rtvs, ID3D11DepthStencilView** dsv) override;
    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(UINT numRTVs, ID3D11RenderTargetView** rtvs,
        ID3D11DepthStencilView** dsv, UINT uavStart, UINT numUAVs, ID3D11UnorderedAccessView** uavs) override;
    void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState** state, FLOAT blendFactor[4], UINT* sampleMask) override;
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState** state, UINT* stencilRef) override;
    void STDMETHODCALLTYPE SOGetTargets(UINT num, ID3D11Buffer** targets) override;
    void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState** state) override;
    void STDMETHODCALLTYPE RSGetViewports(UINT* num, D3D11_VIEWPORT* viewports) override;
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* num, D3D11_RECT* rects) override;
    void STDMETHODCALLTYPE HSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) override;
    void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader** shader, ID3D11ClassInstance** inst, UINT* numInst) override;
    void STDMETHODCALLTYPE HSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) override;
    void STDMETHODCALLTYPE HSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) override;
    void STDMETHODCALLTYPE DSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) override;
    void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader** shader, ID3D11ClassInstance** inst, UINT* numInst) override;
    void STDMETHODCALLTYPE DSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) override;
    void STDMETHODCALLTYPE DSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) override;
    void STDMETHODCALLTYPE CSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) override;
    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT start, UINT num, ID3D11UnorderedAccessView** uavs) override;
    void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader** shader, ID3D11ClassInstance** inst, UINT* numInst) override;
    void STDMETHODCALLTYPE CSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) override;
    void STDMETHODCALLTYPE CSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) override;
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override;
    UINT STDMETHODCALLTYPE GetContextFlags() override { return 0; }

    // d12_context_ops.cpp
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* resource, UINT subresource, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) override;
    void STDMETHODCALLTYPE Unmap(ID3D11Resource* resource, UINT subresource) override;
    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* async) override;
    void STDMETHODCALLTYPE End(ID3D11Asynchronous* async) override;
    HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* async, void* data, UINT size, UINT flags) override;
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
        ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box) override;
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource* dst, ID3D11Resource* src) override;
    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* dst, UINT dstSub, const D3D11_BOX* box,
        const void* data, UINT rowPitch, UINT depthPitch) override;
    void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer* dst, UINT offset, ID3D11UnorderedAccessView* src) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* rtv, const FLOAT color[4]) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* uav, const UINT values[4]) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* uav, const FLOAT values[4]) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil) override;
    void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* srv) override;
    void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource* resource, FLOAT minLod) override;
    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(ID3D11Resource* resource) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format) override;
    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList* list, BOOL restoreState) override;
    void STDMETHODCALLTYPE ClearState() override;
    void STDMETHODCALLTYPE Flush() override;
    HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL restoreState, ID3D11CommandList** list) override;

    // ---- internal API ----
    bool IsDeferred() const { return m_deferred; }
    u32 Index() const { return m_index; }

    // d12_context_ops.cpp: list lifecycle
    // The open command list (opens one when needed: descriptor heaps set, everything dirty).
    ID3D12GraphicsCommandList* List();
    // Immediate only: closes and submits the recorded work (plus pending init copies and the lists
    // queued by ExecuteCommandList); returns the fence value signalled after it. `extra` lists are
    // executed right after it, in order.
    u64 Submit(ID3D12CommandList* const* extra = nullptr, u32 extraCount = 0);
    // Tracking + barriers.
    void Require(Resource* resource, const SubRange& range, D3D12_RESOURCE_STATES state);
    void RequireAll(Resource* resource, D3D12_RESOURCE_STATES state);
    void FlushBarriers();
    void UavBarrier(); // global UAV barrier (D3D11 orders UAV work between calls)
    // Makes every applied D3D12 binding dirty (after a list reset or a native section).
    void InvalidateApplied();
    // A closed D3D12 list of this context was submitted: it can be Reset again. Thread-safe.
    void ReturnList(ID3D12GraphicsCommandList* list);

    // Interfaces of the open list (queried once per pooled list; null where the runtime lacks them).
    ID3D12GraphicsCommandList4* List4() const { return m_list4; }
    ID3D12GraphicsCommandList5* List5() const { return m_list5; }
    ID3D12GraphicsCommandList6* List6() const { return m_list6; }

    // d12_context_state.cpp: draw preparation
    bool PrepareDraw();      // false = skip the draw (no PSO), logged once
    // PrepareDraw's work; `mesh` = a mesh pipeline (mesh root signature, no input assembler).
    bool PrepareGraphics(bool mesh);
    bool PrepareDispatch();
    // Indirect draws and dispatches (D3D11 *Indirect and the count variants of d12_api.h): up to
    // `maxCount` tightly packed records, `count` (optional) holds the GPU count.
    enum class IndirectKind : u8
    {
        DrawIndexed,
        Draw,
        Dispatch,
        Count
    };
    void Indirect(IndirectKind kind, ID3D11Buffer* args, UINT offset, UINT maxCount, ID3D11Buffer* count, UINT countOffset);
    // Hazards: D3D11 unbinds inputs that alias outputs.
    void UnbindSrvsOf(Resource* resource, const SubRange& range);
    bool IsBoundAsOutput(Resource* resource, const SubRange& range) const;

    // Native interop (d12_swapchain.cpp uses these).
    bool m_inNative = false;

    // ---- D3D11-level state (public for the two implementation files) ----
    StageBindings stages[kStageCount];
    ID3D11InputLayout* inputLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    struct VertexBufferBinding
    {
        ID3D11Buffer* buffer = nullptr; // AddRef'd
        UINT stride = 0;
        UINT offset = 0;
    } vbs[kMaxVBs];
    ID3D11Buffer* indexBuffer = nullptr; // AddRef'd
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
    UINT indexOffset = 0;
    ID3D11BlendState* blendState = nullptr;       // AddRef'd
    FLOAT blendFactor[4]{1, 1, 1, 1};
    UINT sampleMask = 0xffffffff;
    ID3D11DepthStencilState* depthState = nullptr; // AddRef'd
    UINT stencilRef = 0;
    ID3D11RasterizerState* rasterState = nullptr;  // AddRef'd
    D3D11_VIEWPORT viewports[kMaxViewports]{};
    UINT numViewports = 0;
    D3D11_RECT scissors[kMaxViewports]{};
    UINT numScissors = 0;
    ID3D11RenderTargetView* rtvs[kMaxRTVs]{};      // AddRef'd
    UINT numRTVs = 0;
    ID3D11DepthStencilView* dsv = nullptr;         // AddRef'd
    ID3D11UnorderedAccessView* csUavs[kMaxUAVs]{}; // AddRef'd
    ID3D11UnorderedAccessView* psUavs[kMaxUAVs]{}; // AddRef'd (OM UAVs)
    UINT psUavStart = 0;

    // ---- applied D3D12 state (what the open list has) ----
    struct Applied
    {
        bool heapsDirty = true;        // SetDescriptorHeaps needed (new list / after native code)
        bool rootSignatureGraphics = false;
        bool rootSignatureCompute = false;
        bool rootSignatureMesh = false;
        ID3D12PipelineState* pso = nullptr;
        bool psoDirty = true;          // PSO-relevant D3D11 state changed since the last lookup
        u64 psoHash = 0;
        bool vbDirty = true;
        bool ibDirty = true;
        u32 vbVersions[kMaxVBs]{};     // dynamic versions last applied
        u32 ibVersion = 0;
        bool rtDirty = true;
        bool viewportDirty = true;
        bool scissorDirty = true;
        bool stencilRefDirty = true;
        bool blendFactorDirty = true;
        bool topologyDirty = true;
        bool psUavDirty = true;
        bool csUavDirty = true;
        bool uavWritten = false;       // the last draw/dispatch had UAVs bound
        // [dx12:vrs] begin applied
        bool vrsDirty = true;           // the shading rate state must be looked at before the next draw
        bool vrsListNonDefault = false; // the open list may shade at another rate than 1x1 (native code)
        // [dx12:vrs] end applied
    } applied;

    // ---- recording ----
    Device* device() const { return m_device; }
    DescriptorAllocator descriptors;
    UploadAllocator upload;
    StateTracker tracker;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    std::vector<Query*> endedQueries;              // immediate: queries ended in the open list
    std::vector<Resource*> pendingReadback;        // staging copies recorded in the open list
    // Dynamic textures mapped with WRITE_DISCARD: the upload memory handed out until Unmap.
    struct TextureMap
    {
        Resource* resource = nullptr;
        u32 subresource = 0;
        UploadAlloc alloc;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    };
    std::vector<TextureMap> textureMaps;
    // Persistent upload memory of the DISCARD versions of dynamic buffers (d12_context_ops.cpp).
    DynamicHeap* dynamicHeap = nullptr;
    // Generation of the sampler heap the open list has bound (SamplerTableCache::Generation).
    u32 samplerGeneration = 0;

    // ID3DUserDefinedAnnotation (the engine's PIX_EVENT scopes): markers on the D3D12 lists, kept
    // balanced per list (d12_context_ops.cpp). QueryInterface answers it besides the context chain.
    class Annotation;
    Annotation* annotation = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    // Immediate: occlusion queries between Begin and End (AddRef'd), split at submissions.
    std::vector<Query*> activeQueries;
    // Immediate: results of queries ended in the open list, copied to readback memory when it
    // closes, a run of consecutive slots per ResolveQueryData. A resolve makes the GPU wait for
    // everything before it: one per query put a pipeline drain at every timestamp and occlusion test.
    struct PendingResolve
    {
        ID3D12QueryHeap* heap = nullptr;
        ID3D12Resource* readback = nullptr;
        D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_OCCLUSION;
        u32 index = 0;
    };
    std::vector<PendingResolve> pendingResolves;

    // Deferred: records a COMPUTE list for an async job until FinishCommandList (d12_async.cpp).
    bool recordCompute = false;
    u32 asyncStamps = ~0u; // timestamp pair of that recording
    // FinishCommandList of a compute recording, before the list closes: the end timestamp goes in and
    // the pair moves to `out` (null: the recording is dropped).
    void AsyncRecordingClosing(ID3D12GraphicsCommandList* list, CommandList* out);

    // Async compute on the immediate context (d12_async.cpp). A fork queues the job after everything
    // recorded so far; a join makes what follows wait for it (and for every job forked before it).
    void ForkJob(AsyncJob* job);
    void JoinJob(AsyncJob* job, bool guard);
    // The direct queue needs a resource lent to a job: joins that job (logged once).
    void JoinLent(Resource* resource);
    // Immediate: joins first when `resource` is lent and the access conflicts with the loan
    // (d12_context_ops.cpp).
    void GuardLent(Resource* resource, const SubRange* range, D3D12_RESOURCE_STATES state, bool exact);
    // d12_context_ops.cpp, for the jobs (immediate): a tracked subresource (~0u = all) into `state`
    // (exact, or merged with a read state it is in), guarded; the state a job left it in; a job entry
    // after the recorded work (the own list is closed first).
    void RequireExact(Resource* resource, u32 subresource, D3D12_RESOURCE_STATES state, bool exact);
    void AdoptState(Resource* resource, u32 subresource, D3D12_RESOURCE_STATES state);
    void QueueJobEntry(AsyncJob* job, bool join, ID3D12GraphicsCommandList* list, Context* owner);
    bool HasOpenList() const { return m_list; }

    // [dx12:vrs] begin context
    ContextVrs* vrs = nullptr;           // SetShadingRate state; null = 1x1, no image
    void VrsApply(ID3D12GraphicsCommandList* list); // ApplyFixedFunction, when applied.vrsDirty
    void VrsRequire();                   // EnsureDrawStates, when vrs is set: rate image state
    void VrsReset(bool keepState);       // ReleaseBindings (false) and InvalidateApplied (true), when vrs is set
    void VrsNativeReset();               // BeginNative: hands the list out at 1x1 without an image
    void VrsDestroy();                   // the context's destructor, when vrs is set
    // [dx12:vrs] end context

    // [dx12:mesh] begin context
    ContextMesh* mesh = nullptr;
    bool MeshRefuseDraw();               // PrepareDraw with a mesh-kind shader in the VS slot: true = skip
    bool PrepareMesh();                  // DispatchMesh's preparation (uses PrepareGraphics(true))
    void MeshDestroy();                  // the context's destructor, when mesh is set
    // [dx12:mesh] end context

private:
    bool m_deferred;
    u32 m_index;
    ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12GraphicsCommandList4* m_list4 = nullptr;
    ID3D12GraphicsCommandList5* m_list5 = nullptr;
    ID3D12GraphicsCommandList6* m_list6 = nullptr;
    // Newer interfaces of every list the context created (weak pointers: the list's own reference
    // keeps them valid). Looked up when a pooled list is opened again.
    struct ListInterfaces
    {
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12GraphicsCommandList4* list4 = nullptr;
        ID3D12GraphicsCommandList5* list5 = nullptr;
        ID3D12GraphicsCommandList6* list6 = nullptr;
    };
    std::vector<ListInterfaces> m_listInterfaces; // guarded by m_spareLock
    void SetOpenList(ID3D12GraphicsCommandList* list, bool created);
    // Allocators rotate per frame: slot = frame % (kFramesInFlight + 1). Device::EndFrame waits
    // for frame - kFramesInFlight, so a slot last used kFramesInFlight + 1 frames ago is idle and
    // is Reset at its first use in the new frame.
    struct AllocatorSlot
    {
        ID3D12CommandAllocator* allocator = nullptr;
        u64 frame = 0;
    };
    // By list type: [0] DIRECT, [1] COMPUTE (compute recordings of async jobs).
    AllocatorSlot m_allocators[2][kFramesInFlight + 1];
    u32 m_listType = 0; // of the open list
    // Immediate: closed lists waiting for the next submission, in execution order - the parts of
    // the own list cut at ExecuteCommandList and the deferred lists between them. One queue
    // submission a frame instead of one per executed deferred list (each costs a kernel call and
    // a fence signal). `owner` gets the list back once it went to the queue. With `job` set the
    // entry belongs to an async job: a fork (no list) or join marker, or the job's list run in order
    // on the direct queue (no compute queue).
    struct QueuedList
    {
        ID3D12GraphicsCommandList* list = nullptr;
        Context* owner = nullptr;
        AsyncJob* job = nullptr;
        bool join = false;
    };
    std::vector<QueuedList> m_queued;
    std::mutex m_queuedLock; // a deferred context's destructor may run on another thread
    std::vector<ID3D12CommandList*> m_submitBatch; // Submit: more lists than its stack array holds
    // Closes the open list for submission (queries split, annotation scopes ended); null = none.
    ID3D12GraphicsCommandList* CloseOwn();
    // Closed lists that were submitted (a list may be Reset right after ExecuteCommandLists), by type.
    std::mutex m_spareLock;
    std::vector<ID3D12GraphicsCommandList*> m_spareLists[2];
    friend class Device;
    friend class SwapChain;
    void OpenList();
    ID3D12CommandAllocator* AcquireAllocator(u32 type);
};

// ============================================================================================
// Device (d12_device.cpp)
// ============================================================================================

class Device final : public ID3D11Device
{
public:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D11Device
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** buffer) override;
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture1D** texture) override;
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** texture) override;
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture3D** texture) override;
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** view) override;
    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, ID3D11UnorderedAccessView** view) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** view) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, ID3D11DepthStencilView** view) override;
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void* bytecode, SIZE_T size, ID3D11InputLayout** layout) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader) override;
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11GeometryShader** shader) override;
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void* bytecode, SIZE_T size, const D3D11_SO_DECLARATION_ENTRY* decl,
        UINT numEntries, const UINT* strides, UINT numStrides, UINT rasterizedStream, ID3D11ClassLinkage* linkage, ID3D11GeometryShader** shader) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) override;
    HRESULT STDMETHODCALLTYPE CreateHullShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11HullShader** shader) override;
    HRESULT STDMETHODCALLTYPE CreateDomainShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11DomainShader** shader) override;
    HRESULT STDMETHODCALLTYPE CreateComputeShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11ComputeShader** shader) override;
    HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage** linkage) override;
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* desc, ID3D11BlendState** state) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* desc, ID3D11DepthStencilState** state) override;
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* desc, ID3D11RasterizerState** state) override;
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** state) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* desc, ID3D11Query** query) override;
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC* desc, ID3D11Predicate** predicate) override;
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC* desc, ID3D11Counter** counter) override;
    HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT flags, ID3D11DeviceContext** context) override;
    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE resource, REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT format, UINT* support) override;
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels) override;
    void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO* info) override;
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC* desc, D3D11_COUNTER_TYPE* type, UINT* activeCounters,
        LPSTR name, UINT* nameLength, LPSTR units, UINT* unitsLength, LPSTR description, UINT* descriptionLength) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE feature, void* data, UINT size) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override;
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return m_featureLevel; }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return 0; }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** context) override;
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT flags) override { return S_OK; }
    UINT STDMETHODCALLTYPE GetExceptionMode() override { return 0; }

    // ---- internal ----
    static Device* From(ID3D11Device* device) { return static_cast<Device*>(device); }
    HRESULT Init(const DeviceCreateInfo& info);
    void Shutdown();

    ID3D12Device* d12 = nullptr;
    ID3D12Device1* d12_1 = nullptr;          // pipeline libraries (may be null)
    ID3D12Device2* d12_2 = nullptr;          // pipeline state streams (mesh pipelines; may be null)
    ID3D12Device5* d12_5 = nullptr;          // raytracing (may be null)
    NativeCaps caps;                         // masked by disabledFeatures (d12_native.cpp)
    UINT disabledFeatures = 0;               // NativeFeature bits of the creation
    ID3D12CommandQueue* queue = nullptr;
    FenceTimeline fence;
    // Creation-time uploads signal their own timeline: a value of `fence` taken by one of them
    // would complete before the submission that carries work the releaser tagged with it.
    FenceTimeline initFence;
    DeferredReleaser releaser;
    CpuDescriptorHeap cpuSrv;                // CBV/SRV/UAV
    CpuDescriptorHeap cpuRtv;
    CpuDescriptorHeap cpuDsv;
    CpuDescriptorHeap cpuSampler;
    GpuDescriptorRing gpuRing;
    SamplerTableCache samplerTables;
    NullDescriptors nulls;
    UploadPagePool uploadPages;
    BufferPageAllocator bufferPages;         // DEFAULT heap
    BufferPageAllocator dynamicFirstVersions; // dynamicHeapType
    InitUploader init;
    // DeviceCreateInfo::copyQueueUploads (-dx12_no_copy_queue turns it off): creation-time data on a copy
    // queue with its own timeline; null when off or its creation failed. Depth-stencil textures stay on `init`.
    ID3D12CommandQueue* copyQueue = nullptr;
    FenceTimeline copyFence;
    InitUploader initCopy;
    InitUploader& Uploader(bool depthStencil) { return copyQueue && !depthStencil ? initCopy : init; }
    void FlushUploads();   // both uploaders (Submit, WaitIdle)
    bool UploadsPending();
    void CopyQueueInit();  // Init, after `init`
    void CopyQueueShutdown();
    RootSignatures rootSignatures;
    // Where dynamic buffers live: every draw reads its constants and streamed geometry from there.
    // GPU_UPLOAD (CPU-visible video memory) where the adapter offers it, UPLOAD (system memory, read
    // across the bus) otherwise.
    D3D12_HEAP_TYPE dynamicHeapType = D3D12_HEAP_TYPE_UPLOAD;
    PipelineCache pipelines;
    QueryHeapPool queryHeaps;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    u64 timestampFrequency = 0;
    // Annotation events reach the lists only with the debug layer or a capture tool in the process:
    // D3D11 drops them otherwise, and D3D12 records every one (several percent of the render thread).
    bool markers = false;
    // -dxstats: what the layer did per frame, averaged into the log every kStatsFrames frames.
    enum Stat : u32
    {
        StatDraws, StatPso, StatRootCbv, StatSrvTables, StatSrvDescriptors, StatCbvTables, StatSamplerTables,
        StatMaps, StatBarriers, StatSubmits,
        StatNativeSections, StatForks, StatJoins, StatAutoJoins, StatMixedPso, StatIndirect, // infra
        StatShadingRate,     // [dx12:vrs] RSSetShadingRate calls
        StatMeshDispatches,  // [dx12:mesh] DispatchMesh calls
        StatCount
    };
    static constexpr u32 kStatsFrames = 300;
    bool stats = false;
    std::atomic<u64> statCounters[StatCount]{};
    void Count(Stat stat, u64 amount = 1)
    {
        if (stats)
            statCounters[stat].fetch_add(amount, std::memory_order_relaxed);
    }
    Context* immediate = nullptr;

    // State object caches (identical descriptions -> same object; objects hold one cache reference).
    std::mutex stateLock;
    std::unordered_map<u64, BlendState*> blendStates;
    std::unordered_map<u64, DepthStencilState*> depthStates;
    std::unordered_map<u64, RasterizerState*> rasterStates;
    std::unordered_map<u64, SamplerState*> samplerStates;
    std::atomic<u32> nextSamplerId{1};
    std::atomic<u64> nextViewId{1};

    // Frames. `frame` increments at every Present (EndFrame). Upload pages and descriptor chunks are
    // tagged with it; EndFrame hands the fence value of the frame's last submission to the pools and
    // waits until frame - kFramesInFlight completed.
    std::atomic<u64> frame{1};
    u64 frameFences[kFramesInFlight]{};
    void EndFrame(u64 lastFenceValue);
    // A frame boundary without a Present (the layer test harness): joins every async job, submits the
    // immediate context and advances the frame.
    void ForceFrameBoundary();
    void WaitIdle();

    // Device removal: logs the reason (and DRED data when enabled) once.
    void ReportRemoval(HRESULT hr);
    bool removed = false;
    void DrainDebugMessages(); // -dxdebug: info queue -> log

    SwapChain* swapChain = nullptr; // the one created through CreateSwapChainForHwnd
    DeviceCreateInfo createInfo;
    IDXGIAdapter1* adapter = nullptr; // AddRef'd

    // TextureWriteGeneration (d12_api.h).
    std::atomic<u64> textureWrites{1};
    void TextureWritten() { textureWrites.fetch_add(1, std::memory_order_relaxed); }

    // Native objects of the feature work (d12_native.cpp): the bindless range, root and command
    // signatures by content (they live as long as the device), the adapter's memory interface.
    BindlessAllocator bindless;
    std::mutex nativeLock;
    std::unordered_map<u64, ID3D12RootSignature*> nativeRootSignatures; // blob hash -> object
    std::unordered_map<ID3D12RootSignature*, u64> nativeRootHashes;     // object -> blob hash
    std::unordered_map<u64, ID3D12CommandSignature*> commandSignatures;
    std::vector<ID3D12RootSignature*> foreignRoots; // referenced by command signatures, kept alive
    IDXGIAdapter3* adapter3 = nullptr;              // null where DXGI 1.4 is missing
    // Command signatures of the indirect draws (by Context::IndirectKind; no root arguments).
    ID3D12CommandSignature* indirectSignatures[u32(Context::IndirectKind::Count)]{};
    void NativeInit();                              // Init, after the null descriptors
    void NativeShutdown();                          // Shutdown, after the releaser drained
    // Buffers with their own resource and tracked states. They decay to COMMON at the end of every
    // ExecuteCommandLists, so the immediate context's tracking follows (DecayTrackedBuffers).
    std::mutex trackedBuffersLock;
    std::vector<Resource*> trackedBuffers;
    void TrackBuffer(Resource* buffer);
    void UntrackBuffer(Resource* buffer);
    void DecayTrackedBuffers(); // immediate context, right after its ExecuteCommandLists

    // Async compute (d12_async.cpp): the compute queue (null: jobs run in order on the direct queue),
    // its fences, job and recorder pools, job timestamps.
    DeviceAsync* async = nullptr;
    ID3D12CommandQueue* computeQueue = nullptr;
    void AsyncInit();          // Init, after the query heaps; sets caps.asyncCompute
    void AsyncShutdown();      // Shutdown, after the immediate context went, before the releaser drains
    void AsyncWaitIdle();      // WaitIdle: the compute queue as well
    void AsyncFrameBoundary(); // Present / ForceFrameBoundary, before the submission: every job joined
    void AsyncEndFrame(u64 frame, u64 next); // EndFrame: compute work of old frames done, timings read
    // A job is recording, waits for its fork or runs unjoined: the frame fence does not cover its
    // memory yet (Submit's emergency frame end waits).
    bool AsyncJobsPending();
    void AsyncForgetContext(Context* context); // a deferred context goes: its lists held by jobs lose the owner
    void AsyncDropStamps(u32 pair);            // a compute recording that never became a job
    // Context::Submit at the markers of m_queued; `crossStale`: direct work ran since the last cross signal.
    void AsyncSubmitFork(AsyncJob* job, bool& crossStale);
    void AsyncSubmitJoin(AsyncJob* job);
    // After the submission's signal: a job list the direct queue ran in order goes back to its owner.
    void AsyncSubmitInOrder(AsyncJob* job, ID3D12GraphicsCommandList* list, Context* owner, u64 fenceValue);

    // [dx12:vrs] begin device
    DeviceVrs* vrs = nullptr;
    bool VrsInit();      // Init, after the capabilities (d12_vrs.cpp)
    void VrsShutdown();  // Shutdown
    // [dx12:vrs] end device

    // [dx12:mesh] begin device
    DeviceMesh* mesh = nullptr;
    void MeshShutdown(); // Shutdown (d12_mesh.cpp)
    // [dx12:mesh] end device

    // [dx12:ds] begin device
    TexturePageAllocator* texheap = nullptr;
    // InitTexture, before it creates a committed resource: true = `out` was placed in a texture
    // heap (r.placement set), false = committed as before (d12_texheap.cpp).
    bool TexturePlace(Resource& r, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial,
        const D3D12_CLEAR_VALUE* clear, ID3D12Resource** out);
    void TextureRelease(Resource& r); // the storage of a placed texture goes (r.placement set)
    void TexheapShutdown();           // Shutdown, after every resource was released
    // [dx12:ds] end device

private:
    std::atomic<ULONG> m_refs{1};
    PrivateData m_private;
    ID3D12InfoQueue* m_infoQueue = nullptr;
    // Answers QueryInterface(IDXGIDevice): the ImGui DX11 backend walks device -> IDXGIDevice ->
    // adapter -> factory. Only GetAdapter/GetParent are functional.
    class DxgiDevice;
    DxgiDevice* m_dxgiDevice = nullptr;
};

// ============================================================================================
// Swap chain (d12_swapchain.cpp)
// ============================================================================================

class SwapChain final : public IDXGISwapChain1
{
public:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override;
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* params) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** unk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override;

    // ---- internal ----
    static HRESULT Create(Device* device, IDXGIFactory2* factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, SwapChain** out);
    HRESULT Recreate(SwapChainProvider* provider);
    IDXGISwapChain4* Native() const { return m_native; }
    // Current back buffer (for the proxy).
    u32 CurrentIndex() const { return m_current; }
    Texture2D* CurrentBuffer() const { return m_buffers[m_current]; }
    u32 BufferCount() const { return u32(m_buffers.size()); }

private:
    SwapChain() = default;
    HRESULT CreateNative();
    void AttachPacer();
    void AcquireBuffers();
    void ReleaseBuffers();
    std::atomic<ULONG> m_refs{1};
    PrivateData m_private;
    Device* m_device = nullptr;
    IDXGIFactory2* m_factory = nullptr;
    IDXGISwapChain4* m_native = nullptr;
    SwapChainProvider* m_provider = nullptr;
    HWND m_hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 m_desc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC m_fullscreenDesc{};
    bool m_hasFullscreenDesc = false;
    std::vector<Texture2D*> m_buffers; // the real buffers (layer textures, tracked states)
    Texture2D* m_proxy = nullptr;      // what GetBuffer(0) returns; resolves to m_buffers[m_current]
    u32 m_current = 0;
    UINT m_presentCount = 0;
    // Vsync pacing on the native chain's waitable object; a provider that paces itself strips the
    // flag from its chain (present_pacing.h).
    xrpacing::PresentPacer m_pacer;
};

// ============================================================================================
// DeviceChild (inline: every implementation file instantiates it)
// ============================================================================================

template <class Iface>
HRESULT STDMETHODCALLTYPE DeviceChild<Iface>::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    // D3D11 interfaces form single-inheritance chains, so every base is the same pointer.
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(Iface) ||
        (std::is_base_of_v<ID3D11Resource, Iface> && riid == __uuidof(ID3D11Resource)) ||
        (std::is_base_of_v<ID3D11View, Iface> && riid == __uuidof(ID3D11View)) ||
        (std::is_base_of_v<ID3D11Asynchronous, Iface> && riid == __uuidof(ID3D11Asynchronous)) ||
        SupportsExtra(riid))
    {
        *object = static_cast<Iface*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

template <class Iface>
ULONG STDMETHODCALLTYPE DeviceChild<Iface>::AddRef()
{
    return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <class Iface>
ULONG STDMETHODCALLTYPE DeviceChild<Iface>::Release()
{
    const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0)
        Destroy();
    return refs;
}

template <class Iface>
void STDMETHODCALLTYPE DeviceChild<Iface>::GetDevice(ID3D11Device** device)
{
    *device = m_device;
    m_device->AddRef();
}

template <class Iface>
HRESULT STDMETHODCALLTYPE DeviceChild<Iface>::GetPrivateData(REFGUID guid, UINT* size, void* data)
{
    return m_private.Get(guid, size, data);
}

template <class Iface>
HRESULT STDMETHODCALLTYPE DeviceChild<Iface>::SetPrivateData(REFGUID guid, UINT size, const void* data)
{
    const HRESULT hr = m_private.Set(guid, size, data);
    if (SUCCEEDED(hr) && guid == WKPDID_D3DDebugObjectName && data && size)
    {
        std::string name(static_cast<const char*>(data), size);
        OnDebugName(name.c_str());
    }
    return hr;
}

template <class Iface>
HRESULT STDMETHODCALLTYPE DeviceChild<Iface>::SetPrivateDataInterface(REFGUID guid, const IUnknown* data)
{
    return m_private.SetInterface(guid, data);
}
} // namespace xrd3d12
