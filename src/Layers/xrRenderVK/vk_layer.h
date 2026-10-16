// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Internal declarations of xrVulkan (see vk_api.h and docs/dead-air/VULKAN_RENDERER.md). The D3D11 semantics
// (objects, binding state, hazards, dynamic buffer versions, deferred contexts) follow xrD3D12; the backend
// is Vulkan 1.3: timeline semaphores, synchronization2, dynamic rendering, extended dynamic state.
//
// Ownership of the implementation files:
//   vk_loader.cpp          vulkan-1.dll and the function table
//   vk_infra.cpp           log, hashing, formats, memory, upload pages, deferred releaser, descriptor layouts
//                          and pools, null resources, pipeline cache, query pools
//   vk_spirv.cpp           SPIR-V reflection and patching, the shader container, DXC arguments
//   vk_device.cpp          Device (ID3D11Device), instance/device creation, frames, state object caches,
//                          input layouts, shaders, the creation part of vk_api.h
//   vk_objects.cpp         resources, views, queries, command lists
//   vk_context_state.cpp   Context: binding API, Get* API, hazards, draw/dispatch preparation, draws
//   vk_context_ops.cpp     Context: command buffers, barriers and layouts, rendering, Map/Unmap, copies,
//                          clears, resolves, queries, ClearState, Flush, command list execution
//   vk_swapchain.cpp       SwapChain (IDXGISwapChain1) over VkSwapchainKHR, back buffer proxy, present
//   vk_native.cpp          capabilities and the D3D11-path extensions (indirect count, mesh, shading rate)
#pragma once

#include "vk_api.h"
#include "vk_loader.h"
#include "../xrRender/present_pacing.h"

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

namespace xrvk
{
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

class Device;
class Context;
class SwapChain;
class Resource;
class View;
class Shader;

// ============================================================================================
// Limits and constants
// ============================================================================================

// Frames the CPU may record ahead of the GPU before the memory of a frame is recycled.
constexpr u32 kFramesInFlight = 3;

constexpr u32 kMaxCBs = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; // 14
constexpr u32 kMaxSRVs = 32;      // D3D11 allows 128; higher slots are refused with a log line
constexpr u32 kMaxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; // 16
constexpr u32 kMaxUAVs = 8;
constexpr u32 kMaxRTVs = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; // 8
constexpr u32 kMaxVBs = 16;
constexpr u32 kMaxViewports = 16;

// SPIR-V bindings of the register classes (vk_spirv.cpp compiles with these shifts; all in set 0, which the
// layer moves to the stage's set at shader creation).
constexpr u32 kBindingCB = 0;       // b0..b13
constexpr u32 kBindingSRV = 16;     // t0..t31
constexpr u32 kBindingSampler = 48; // s0..s15
constexpr u32 kBindingUAV = 64;     // u0..u7
constexpr u32 kBindingCounter = 72; // UAV counters, assigned by DXC above the UAVs
constexpr u32 kBindingLimit = 96;
// The RT lane (vk_raytracing.cpp): compute layouts of shaders with bindless arrays carry the device's bindless set
// at this index (the shaders' kBindlessSet is moved here).
constexpr u32 kComputeBindlessSet = 1;
// Private MiscFlags of the lane's buffers (CreateRtBuffer, CreateAccel); no D3D11 caller sets these bits.
constexpr UINT kMiscRtBuffer = 0x40000000u;     // device address, build input, storage, 256-byte aligned
constexpr UINT kMiscAccelStorage = 0x80000000u; // acceleration structure storage

// Descriptor sets of the graphics pipeline layout, one per stage; compute uses set 0.
enum class Stage : u8
{
    VS = 0,
    PS,
    GS,
    HS,
    DS,
    CS,
    Count
};
constexpr u32 kStageCount = u32(Stage::Count);
constexpr u32 kGraphicsStages = 5;
inline u32 SetOf(Stage stage) { return stage == Stage::CS ? 0u : u32(stage); }
VkShaderStageFlagBits VkStageOf(Stage stage, bool mesh);

// Memory pages.
constexpr u64 kUploadPageSize = 8ull << 20;      // per-context linear pages (constants, dynamic data, staging)
constexpr u64 kInitUploadPageSize = 32ull << 20; // creation-time data
constexpr u64 kBlockSize = 256ull << 20;         // device memory blocks of the suballocator
constexpr u64 kSmallHeapBlockSize = 32ull << 20; // blocks on heaps under 4 GB (the 256 MB BAR)
constexpr u64 kDedicatedThreshold = 64ull << 20; // bigger images get their own allocation

// ============================================================================================
// Utilities (vk_infra.cpp)
// ============================================================================================

void SetLogSink(LogSink sink);
void Log(const char* fmt, ...);
void LogOnce(const void* key, const char* fmt, ...);
u64 Hash64(const void* data, size_t size, u64 seed = 0);
inline u64 HashCombine(u64 a, u64 b) { return Hash64(&b, sizeof(b), a); }

// Formats.
struct FormatInfo
{
    VkFormat format = VK_FORMAT_UNDEFINED; // the Vulkan format of the DXGI format (typeless: its UNORM/UINT member)
    VkImageAspectFlags aspect = 0;         // COLOR, DEPTH | STENCIL ...
    u32 blockBytes = 0;                    // bytes per texel (per 4x4 block for block-compressed formats)
    u32 blockWidth = 1;                    // 4 for block-compressed formats
    bool depth = false, stencil = false, typeless = false, srgb = false, compressed = false;
};
const FormatInfo& Format(DXGI_FORMAT format);
// The view format of `format` for a view's purpose (a depth texture read as R24X8 / X24G8 ...).
VkFormat ViewFormat(DXGI_FORMAT format, bool readStencil);
DXGI_FORMAT TypelessOf(DXGI_FORMAT format);
bool IsDepthFormat(DXGI_FORMAT format);
// Depth formats this device has (D24S8 is missing on AMD: D32S8 stands in).
void InitFormatSupport(VkPhysicalDevice physical, bool allowD24);

// ============================================================================================
// Synchronisation (vk_infra.cpp)
// ============================================================================================

// A timeline semaphore; values are handed out by Next() and signalled by the submissions that carry them.
class Timeline
{
public:
    bool Init(VkDevice device);
    void Shutdown(VkDevice device);
    VkSemaphore Handle() const { return m_semaphore; }
    // The value the next submission signals (resources released now are safe after it).
    u64 NextValue() const { return m_next.load(std::memory_order_acquire); }
    // Takes the next value for a submission.
    u64 Take() { return m_next.fetch_add(1, std::memory_order_acq_rel); }
    u64 Completed(VkDevice device);
    bool IsComplete(VkDevice device, u64 value) { return value <= Completed(device); }
    void Wait(VkDevice device, u64 value);

private:
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
    std::atomic<u64> m_next{1};
    std::atomic<u64> m_completed{0};
};

// Objects released by the engine while the GPU may still use them live until the timeline passes.
class DeferredReleaser
{
public:
    void Init(Device* device, Timeline* timeline) { m_device = device; m_timeline = timeline; }
    void Defer(std::function<void()>&& action); // runs after the current NextValue()
    void Collect(bool all);                     // runs what the timeline allows (all: everything)

private:
    struct Entry
    {
        u64 value;
        std::function<void()> action;
    };
    Device* m_device = nullptr;
    Timeline* m_timeline = nullptr;
    std::mutex m_lock;
    std::vector<Entry> m_entries;
};

// ============================================================================================
// Memory (vk_infra.cpp)
// ============================================================================================

enum class MemoryClass : u8
{
    Device,   // DEVICE_LOCAL: textures, render targets, static buffers
    Upload,   // HOST_VISIBLE | HOST_COHERENT, video memory where the BAR allows: rings, dynamic data
    Staging,  // HOST_VISIBLE | HOST_COHERENT in system memory: creation-time data
    Readback, // HOST_VISIBLE | HOST_CACHED: staging reads, query results
    Count
};

struct Allocation
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    u8* cpu = nullptr;    // persistently mapped (host-visible classes)
    u32 type = ~0u;       // memory type index
    u32 block = ~0u;      // ~0u = dedicated
    bool Valid() const { return memory != VK_NULL_HANDLE; }
};

// Suballocator over big blocks per memory type (free lists of offset/size, coalesced). Thread-safe.
class MemoryAllocator
{
public:
    bool Init(Device* device);
    void Shutdown();
    // The memory type of a class among `typeBits` (~0u when none fits).
    u32 TypeFor(MemoryClass cls, u32 typeBits) const;
    bool Allocate(const VkMemoryRequirements& req, MemoryClass cls, bool dedicated, VkImage image, VkBuffer buffer,
        Allocation& out);
    void Free(const Allocation& allocation); // call through DeferredReleaser when the GPU may still use it
    u64 BytesInUse(u32 heap) const;
    bool IsCoherent(u32 type) const { return type < m_props.memoryTypeCount && (m_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    bool hostVisibleDevice = false; // the Upload class lives in video memory

private:
    struct Block
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
        u8* cpu = nullptr;
        u32 type = 0;
        u32 kind = 0; // bit 0: buffers (else images), bit 1: small allocations
        std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free; // (offset, size) sorted, coalesced
    };
    bool AllocateDedicated(const VkMemoryRequirements& req, u32 type, VkImage image, VkBuffer buffer, Allocation& out);
    Device* m_device = nullptr;
    VkPhysicalDeviceMemoryProperties m_props{};
    u32 m_classType[u32(MemoryClass::Count)]{};
    VkDeviceSize m_bufferImageGranularity = 1;
    mutable std::mutex m_lock;
    std::vector<Block> m_blocks;
    std::atomic<u64> m_heapUse[VK_MAX_MEMORY_HEAPS]{};
};

// A host-visible VkBuffer page: constants, dynamic data and staging copies of one frame.
struct UploadPage
{
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize size = 0;
    u64 frame = 0;           // frame of the data in it
    bool dedicated = false;  // sized for one big request, destroyed instead of pooled
    VkDeviceAddress address = 0;
};

struct UploadAlloc
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    u8* cpu = nullptr;
    VkDeviceSize size = 0;
};

// Pool of upload pages. Pages of a frame come back once that frame's timeline value completed. Thread-safe.
class UploadPagePool
{
public:
    bool Init(Device* device, MemoryClass cls);
    void Shutdown();
    UploadPage* Acquire(VkDeviceSize minSize, u64 frame);
    void EndFrame(u64 frame, u64 timelineValue);
    void Recycle();
    u64 BytesInUse() const { return m_bytesInUse.load(std::memory_order_relaxed); }

private:
    std::atomic<u64> m_bytesInUse{0};
    Device* m_device = nullptr;
    MemoryClass m_class = MemoryClass::Upload;
    std::mutex m_lock;
    std::vector<UploadPage*> m_free;
    std::vector<UploadPage*> m_used;
    std::vector<std::pair<u64, u64>> m_frameValues; // frame -> timeline value
};

// Per-context linear allocator over UploadPagePool pages (not thread-safe).
class UploadAllocator
{
public:
    void Init(UploadPagePool* pool) { m_pool = pool; }
    UploadAlloc Allocate(VkDeviceSize size, VkDeviceSize alignment, u64 frame);

private:
    UploadPagePool* m_pool = nullptr;
    UploadPage* m_page = nullptr;
    VkDeviceSize m_offset = 0;
    u64 m_frame = ~0ull;
};

// Creation-time data from any thread: copies recorded on a private command buffer of the transfer queue (or
// the graphics queue) and submitted before the immediate context's next submission, which waits for them.
// Thread-safe.
class InitUploader
{
public:
    bool Init(Device* device, u32 family, VkQueue queue, std::mutex* queueLock, bool transferQueue);
    void Shutdown();
    void UploadBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);
    // The whole buffer to zero (D3D11 buffers start zeroed) without staging memory.
    void ZeroBuffer(VkBuffer dst);
    // Texture subresources from D3D11_SUBRESOURCE_DATA; the image goes from UNDEFINED to `finalLayout`.
    void UploadTexture(Resource& texture, u32 firstSubresource, u32 count, const D3D11_SUBRESOURCE_DATA* data,
        VkImageLayout finalLayout);
    // Image created without data: UNDEFINED -> `finalLayout` (and zero-filled when `clear`).
    void InitialLayout(Resource& texture, VkImageLayout finalLayout, bool clear);
    // Submits pending copies; returns the timeline value the graphics queue must wait for (0 = none pending).
    u64 Flush();
    Timeline& timeline() { return m_timeline; }
    bool TransferQueue() const { return m_transfer; }
    u64 LastSubmitted() const { return m_lastSubmitted.load(std::memory_order_acquire); }

private:
    UploadAlloc Staging(VkDeviceSize size, VkDeviceSize alignment);
    VkCommandBuffer Recording();
    Device* m_device = nullptr;
    u32 m_family = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::mutex* m_queueLock = nullptr;
    bool m_transfer = false;
    Timeline m_timeline;
    std::mutex m_lock;
    VkCommandPool m_pools[kFramesInFlight + 1]{};
    u64 m_poolValue[kFramesInFlight + 1]{};
    u32 m_poolIndex = 0;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    std::vector<UploadPage*> m_pagesInFlight; // (page->frame holds the timeline value it waits for)
    std::vector<UploadPage*> m_freePages;
    UploadPage* m_page = nullptr;
    VkDeviceSize m_pageOffset = 0;
    VkDeviceSize m_pendingBytes = 0;
    std::atomic<u64> m_lastSubmitted{0};
    std::vector<VkImageMemoryBarrier2> m_released; // ownership releases to the graphics family (transfer queue)
};

// ============================================================================================
// Descriptors (vk_infra.cpp)
// ============================================================================================

// What one descriptor set of a stage holds, from the shader's SPIR-V: binding -> descriptor type.
struct SetBinding
{
    u8 binding = 0;
    u8 type = 0;      // VkDescriptorType (small values only: SAMPLER..STORAGE_BUFFER_DYNAMIC, 1000150000 mapped to 15)
    u8 slot = 0;      // D3D11 slot of its register class (the first one of an array)
    u8 cls = 0;       // 0 CB, 1 SRV, 2 sampler, 3 UAV, 4 UAV counter, 0xFF unknown (null descriptors)
    u8 count = 1;     // array size
    u8 viewType = 0;  // VkImageViewType the shader declares (images), 0xFF = a buffer
    u8 pad[2]{};
};
constexpr u8 kClassCB = 0, kClassSRV = 1, kClassSampler = 2, kClassUAV = 3, kClassCounter = 4, kClassUnknown = 0xFF;
constexpr u8 kTypeAccelerationStructure = 15;
inline VkDescriptorType VkTypeOf(u8 type)
{
    return type == kTypeAccelerationStructure ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VkDescriptorType(type);
}

// A descriptor set layout with its update template; entries are written as one array of DescriptorData.
struct SetLayout
{
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorUpdateTemplate update = VK_NULL_HANDLE; // null for an empty layout
    std::vector<SetBinding> bindings;                    // sorted by binding
    u32 dynamicCount = 0;                                // UNIFORM_BUFFER_DYNAMIC bindings (dynamic offsets, in binding order)
    u64 hash = 0;
};

// The data of one template entry (the largest of the info structs).
union DescriptorData
{
    VkDescriptorImageInfo image;
    VkDescriptorBufferInfo buffer;
    VkBufferView texel;
    VkAccelerationStructureKHR accel;
};

struct PipelineLayout
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const SetLayout* sets[kGraphicsStages]{}; // compute: sets[0]
    u32 setCount = 0;
    u64 hash = 0;
};

// Layout objects by content (they live as long as the device). Thread-safe.
class LayoutCache
{
public:
    bool Init(Device* device);
    void Shutdown();
    const SetLayout* Set(Stage stage, const std::vector<SetBinding>& bindings);
    const SetLayout* Empty() const { return m_empty; }
    // partial: a pipeline library's layout (absent stages are null, independent sets).
    const PipelineLayout* Graphics(const SetLayout* const (&sets)[kGraphicsStages], bool partial = false);
    // bindless: the device's bindless set at kComputeBindlessSet; pushBytes: a push-constant range from 0.
    const PipelineLayout* Compute(const SetLayout* set, bool bindless = false, u32 pushBytes = 0);

private:
    Device* m_device = nullptr;
    std::mutex m_lock;
    std::unordered_map<u64, SetLayout*> m_sets;
    std::unordered_map<u64, PipelineLayout*> m_layouts;
    const SetLayout* m_empty = nullptr;
};

// Per-context descriptor pools, one chain per frame slot; reset when the slot comes round again.
class DescriptorAllocator
{
public:
    void Init(Device* device);
    void Shutdown();
    VkDescriptorSet Allocate(const SetLayout* layout, u64 frame);

private:
    VkDescriptorPool NewPool();
    Device* m_device = nullptr;
    struct Slot
    {
        std::vector<VkDescriptorPool> pools;
        u32 current = 0;
        u64 frame = ~0ull;
    };
    Slot m_slots[kFramesInFlight + 1];
};

// Null resources for slots a shader reads that nothing is bound to (D3D11 returns zeros there), where the
// device lacks robustness2 null descriptors.
struct NullResources
{
    VkImage image1d = VK_NULL_HANDLE, image2d = VK_NULL_HANDLE, image3d = VK_NULL_HANDLE; // 2D: 6 layers, cube-compatible
    Allocation memory1d, memory2d, memory3d;
    VkImageView view1d = VK_NULL_HANDLE, view1dArray = VK_NULL_HANDLE, view2d = VK_NULL_HANDLE,
                view2dArray = VK_NULL_HANDLE, view3d = VK_NULL_HANDLE, viewCube = VK_NULL_HANDLE,
                viewCubeArray = VK_NULL_HANDLE, storage2d = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE; // 64 KB of zeros: CBs, storage and texel buffers
    Allocation bufferMemory;
    VkBufferView texel = VK_NULL_HANDLE, storageTexel = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE; // D3D11's default sampler state
    bool Init(Device* device);
    void Shutdown(Device* device);
    VkImageView ImageFor(VkImageViewType type, bool storage) const;
};

// ============================================================================================
// Queries (vk_infra.cpp)
// ============================================================================================

class QueryPoolSet
{
public:
    bool Init(Device* device);
    void Shutdown();
    struct Slot
    {
        VkQueryPool pool = VK_NULL_HANDLE;
        u32 index = 0;
    };
    Slot Allocate(VkQueryType type);       // host-reset before it is handed out
    void Free(VkQueryType type, const Slot& slot);

private:
    struct Pool
    {
        VkQueryPool pool = VK_NULL_HANDLE;
        std::vector<u32> free;
    };
    Device* m_device = nullptr;
    std::mutex m_lock;
    std::vector<Pool> m_occlusion;
    std::vector<Pool> m_timestamp;
};

// ============================================================================================
// COM plumbing (vk_objects.cpp)
// ============================================================================================

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
    virtual void Destroy() { delete this; }
    virtual void OnDebugName(const char* /*name*/) {}
    virtual bool SupportsExtra(REFIID /*riid*/) const { return false; }

    Device* m_device;
    std::atomic<ULONG> m_refs{1};
    PrivateData m_private;
};

// ============================================================================================
// Resources (vk_objects.cpp)
// ============================================================================================

// Tracked state of a subresource (or of a GPU-written buffer): its layout, the last write and the stages
// that were made to see it (reads after reads need no barrier, a write waits for the reads before it).
struct ImageState
{
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 writeStages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 writeAccess = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 readStages = VK_PIPELINE_STAGE_2_NONE; // stages ordered after the last write
    VkAccessFlags2 readAccess = VK_ACCESS_2_NONE;                // accesses the last write was made visible to
};

// The barrier (if any) that orders an access after `state`, and the state after it. `attachmentOrder`:
// consecutive writes of the same attachment kind keep rasterization order and need none.
bool Transition(ImageState& state, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access,
    VkPipelineStageFlags2& srcStages, VkAccessFlags2& srcAccess, VkImageLayout& oldLayout);
bool IsWriteAccess(VkAccessFlags2 access);

// Subresources of a range: mips x array layers (the aspect follows the format).
struct SubRange
{
    u32 firstMip = 0, mipCount = 1;
    u32 firstLayer = 0, layerCount = 1;
};

// Persistent upload memory for the versions of dynamic buffers: a version stays valid until the buffer is
// mapped with DISCARD again (a buffer filled once is drawn for many frames), then it is retired and reused
// kFramesInFlight + 1 frames later. Size classes of 256 B << n inside 8 MB pages; bigger versions get
// their own VkBuffer. One heap per context; a version may be retired by another context (locked).
class DynamicHeap
{
public:
    explicit DynamicHeap(Device* device) : m_device(device) {}
    ~DynamicHeap();
    // A new version; `retire` (the version it replaces, when it came from this heap) goes in the same lock.
    bool Allocate(VkDeviceSize size, UploadAlloc& out, const UploadAlloc* retire);
    // True when the heap was orphaned and this was its last outstanding version (the caller deletes it).
    bool Retire(const UploadAlloc& alloc);
    // The owning context is gone: memory is released after the GPU; true when nothing is outstanding.
    bool Orphan();

private:
    static constexpr u32 kClassCount = 15; // 256 B .. 4 MB
    struct Retired
    {
        UploadAlloc alloc;
        u64 frame = 0;
    };
    struct Block
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        Allocation memory;
        VkDeviceSize size = 0;
    };
    static u32 ClassOf(VkDeviceSize size);
    bool CreateBlock(VkDeviceSize size, Block& out);
    void DropBlock(VkBuffer buffer);
    void PushRetired(const UploadAlloc& alloc);
    void Reclaim(u64 frame);
    Device* m_device;
    std::mutex m_lock;
    std::vector<Block> m_blocks;
    std::vector<UploadAlloc> m_free[kClassCount];
    std::vector<UploadAlloc> m_freeLarge;
    std::vector<Retired> m_retired; // ring
    size_t m_retiredHead = 0, m_retiredCount = 0;
    Block m_page;
    VkDeviceSize m_pageUsed = 0;
    u64 m_bytes = 0;
    u32 m_outstanding = 0;
    bool m_orphaned = false;
};

// The current CPU-written version of a dynamic buffer (upload memory). Replaced by every DISCARD map.
struct DynamicVersion
{
    UploadAlloc alloc;
    DynamicHeap* heap = nullptr; // null: frame memory (fallback) or nothing yet
    u32 version = 0;
};
void RetireDynamicVersion(Resource* resource);

class Resource
{
public:
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    UINT bindFlags = 0;
    UINT cpuAccess = 0;
    UINT miscFlags = 0;

    // Images.
    VkImage image = VK_NULL_HANDLE;
    VkImageCreateInfo imageInfo{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    u32 width = 1, height = 1, depth = 1;
    u32 mipLevels = 1;
    u32 arraySize = 1;
    u32 samples = 1;
    u32 SubresourceCount() const { return mipLevels * arraySize; }
    u32 Subresource(u32 mip, u32 layer) const { return mip + layer * mipLevels; }
    // Tracked per subresource (owned by the immediate context's tracking). `uniform` = all in states[0].
    bool uniform = true;
    std::vector<ImageState> states;

    // Buffers.
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize bufferOffset = 0; // of the resource's bytes inside `buffer`
    u64 byteWidth = 0;
    VkDeviceAddress address = 0;
    // A buffer the GPU writes (UAV, stream out, copy destination): global memory barriers order its uses
    // (the immediate context tracks bufferState; deferred contexts track their own).
    bool gpuWritten = false;
    ImageState bufferState;
    // Constant buffers the engine updates with UpdateSubresource live in upload memory like dynamic ones
    // (no copy and no split of the rendering); `shadow` keeps the contents for partial updates.
    bool renamed = false;
    std::vector<u8> shadow;

    Allocation memory;

    // Dynamic buffers (USAGE_DYNAMIC): current version; `current.version` changes on every DISCARD.
    DynamicVersion current;

    // Staging textures (USAGE_STAGING) and dynamic textures: CPU-visible linear memory per subresource.
    VkBuffer linear = VK_NULL_HANDLE;
    Allocation linearMemory;
    struct Footprint
    {
        VkDeviceSize offset = 0;
        u32 rowPitch = 0, depthPitch = 0;
        u32 width = 0, height = 0, depth = 0;
        u32 rows = 0;
    };
    std::vector<Footprint> footprints;
    u64 lastGpuWrite = 0;          // timeline value of the last copy into `linear` (readback)
    bool gpuWritePending = false;  // a copy was recorded into `linear` but not submitted

    // The engine's shading rate image as a rate attachment (made on first use).
    VkImageView rateView = VK_NULL_HANDLE;
    // Mip 0, layer 0 for an SDK's native work (PrepareNativeImage, made on first use).
    VkImageView nativeView = VK_NULL_HANDLE;

    bool IsBuffer() const { return dimension == D3D11_RESOURCE_DIMENSION_BUFFER; }
    bool IsImage() const { return !IsBuffer(); }
    // Dynamic and renamed buffers: their bytes are the current upload version.
    bool Versioned() const { return usage == D3D11_USAGE_DYNAMIC || renamed; }
    VkBuffer BufferHandle() const { return Versioned() ? current.alloc.buffer : buffer; }
    VkDeviceSize BufferBase() const { return Versioned() ? current.alloc.offset : bufferOffset; }

    u64 id = 0; // unique, never reused (keys descriptor caches)
    virtual ~Resource();
    static Resource* From(ID3D11Resource* resource);
};

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
    // Wraps a swap chain image (image set) or makes the back buffer proxy (image null).
    static Texture2D* CreateForSwapChain(Device* device, VkImage image, const D3D11_TEXTURE2D_DESC& desc);
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
    bool m_swapChainImage = false; // the image belongs to the swap chain
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

// Creates the image of a texture (all three dimensions) and fills it (vk_objects.cpp).
HRESULT InitTexture(Device* device, Resource& r, D3D11_RESOURCE_DIMENSION dim, u32 width, u32 height, u32 depth,
    u32 mips, u32 arraySize, DXGI_FORMAT format, u32 samples, D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc,
    const D3D11_SUBRESOURCE_DATA* data);
void ReleaseResourceStorage(Device* device, Resource& r);

// ============================================================================================
// Views (vk_objects.cpp)
// ============================================================================================

enum class ViewKind : u8
{
    SRV,
    RTV,
    DSV,
    UAV
};

class View
{
public:
    ViewKind kind = ViewKind::SRV;
    ID3D11Resource* resourceIface = nullptr; // AddRef'd
    Resource* resource = nullptr;
    SubRange range;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // Images: the view.
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo viewInfo{};  // what imageView was made from (adapted views reuse it)
    // The view as the shader declares it (2D <-> 2D array, cube <-> 2D array, 1D <-> 1D array over the
    // same range); D3D11 drivers tolerate such mismatches, Vulkan does not. Null when impossible.
    VkImageView ImageViewFor(VkImageViewType declared);
    struct Adapted
    {
        VkImageViewType type;
        VkImageView view;
    };
    std::vector<Adapted> adapted;
    std::mutex adaptLock;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // DSV read-only flags (D3D11_DSV_READ_ONLY_DEPTH/STENCIL).
    UINT dsvFlags = 0;
    // Buffers: typed views (texel buffer views; a dynamic buffer's view follows its version), raw and
    // structured views (a byte range of the buffer).
    VkBufferView texelView = VK_NULL_HANDLE;
    VkAccelerationStructureKHR accel = VK_NULL_HANDLE; // an acceleration structure SRV (vk_native.cpp)
    VkDeviceSize bufferOffset = 0; // of the view inside the resource's bytes
    VkDeviceSize bufferRange = 0;
    bool structured = false;       // raw or structured (storage buffer) rather than typed
    // UAV counter (append/consume/counter structured buffers): its own small device buffer.
    VkBuffer counter = VK_NULL_HANDLE;
    Allocation counterMemory;
    u64 id = 0;
    Device* owner = nullptr;

    virtual ~View();
    // Defers the destruction of the Vulkan views and drops the resource reference.
    void ReleaseViews(Device* device);
    Device* Owner() const;
    static View* From(ID3D11ShaderResourceView* view);
    static View* From(ID3D11RenderTargetView* view);
    static View* From(ID3D11DepthStencilView* view);
    static View* From(ID3D11UnorderedAccessView* view);
};

class ShaderResourceView final : public DeviceChild<ID3D11ShaderResourceView>, public View
{
public:
    static HRESULT Create(Device* device, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ShaderResourceView** out);
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
    UINT uavFlags = 0;

private:
    explicit UnorderedAccessView(Device* device) : DeviceChild(device) {}
    void Destroy() override;
    D3D11_UNORDERED_ACCESS_VIEW_DESC m_desc{};
};

// ============================================================================================
// State objects, shaders, input layouts (vk_device.cpp). Identical descriptions return the same object.
// ============================================================================================

class BlendState final : public DeviceChild<ID3D11BlendState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC* desc) override { *desc = desc11; }
    D3D11_BLEND_DESC desc11{};
    VkPipelineColorBlendAttachmentState attachments[kMaxRTVs]{};
    bool alphaToCoverage = false;
    u64 hash = 0;
    explicit BlendState(Device* device) : DeviceChild(device) {}
};

class DepthStencilState final : public DeviceChild<ID3D11DepthStencilState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC* desc) override { *desc = desc11; }
    D3D11_DEPTH_STENCIL_DESC desc11{};
    bool depthTest = false, depthWrite = false, stencilTest = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
    VkStencilOpState front{}, back{};
    u64 hash = 0;
    explicit DepthStencilState(Device* device) : DeviceChild(device) {}
};

class RasterizerState final : public DeviceChild<ID3D11RasterizerState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC* desc) override { *desc = desc11; }
    D3D11_RASTERIZER_DESC desc11{};
    VkPolygonMode polygon = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front = VK_FRONT_FACE_CLOCKWISE;
    bool depthBias = false, depthClamp = false, scissor = false;
    u64 hash = 0;
    explicit RasterizerState(Device* device) : DeviceChild(device) {}
};

class SamplerState final : public DeviceChild<ID3D11SamplerState>
{
public:
    void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC* desc) override { *desc = desc11; }
    D3D11_SAMPLER_DESC desc11{};
    VkSampler sampler = VK_NULL_HANDLE;
    u32 id = 0;
    explicit SamplerState(Device* device) : DeviceChild(device) {}

protected:
    void Destroy() override;
};

// What a shader binds (from its SPIR-V) and how its interface is laid out.
struct ShaderInterface
{
    std::vector<SetBinding> bindings; // sorted by binding
    u16 cbMask = 0;
    u32 srvMask = 0;
    u16 samplerMask = 0;
    u8 uavMask = 0;
    // A stage input or output with a Location (DXC names them in.var.<SEMANTIC> / out.var.<SEMANTIC>).
    struct Var
    {
        std::string semantic;   // upper-case, without the index
        u32 index = 0;          // semantic index
        u32 location = 0;
        u32 locations = 1;      // matrices and arrays of the per-vertex element take several
        u32 components = 4;
        bool integer = false;
        bool patch = false;     // per-patch (tessellation)
        u32 locationWord = 0;   // word offset of the Location literal in Shader::spirv (for relinking)
    };
    std::vector<Var> inputs;    // VS: vertex attributes
    std::vector<Var> outputs;   // PS: render targets
    u32 localSize[3]{1, 1, 1};  // compute / mesh / task workgroup size
    bool sampleShading = false; // reads SV_SampleIndex or interpolates at sample
    bool bindless = false;      // compute: declares the device's bindless arrays (kBindlessSet)
    u32 pushConstants = 0;      // compute: bytes of its push-constant block (kRtPushConstantBytes at most)
};

class Shader
{
public:
    Stage stage = Stage::VS;
    bool mesh = false;      // a mesh shader in the VS slot, a task shader in the GS slot
    std::vector<u32> spirv; // patched: the stage's descriptor set, semantic locations, no reflection decorations
    std::vector<u8> dxil;   // the DXIL twin (input signature for CreateInputLayout)
    ShaderInterface iface;
    const SetLayout* setLayout = nullptr;
    VkShaderModule module = VK_NULL_HANDLE;
    std::string entry = "main";
    u64 hash = 0;
    // Compute: the pipeline is per shader (created on first dispatch).
    VkPipeline computePipeline = VK_NULL_HANDLE;
    const PipelineLayout* computeLayout = nullptr;
    std::string name;
    virtual ~Shader();
    void ReleaseObjects(Device* device);
    static Shader* From(ID3D11DeviceChild* shader, Stage stage);
};

template <class Iface, Stage S>
class ShaderObject final : public DeviceChild<Iface>, public Shader
{
public:
    explicit ShaderObject(Device* device) : DeviceChild<Iface>(device) { stage = S; }

protected:
    void OnDebugName(const char* debugName) override
    {
        name = debugName;
        // Capture tools (RenderDoc, Nsight) show the engine's shader names.
        if (module)
            this->Owner()->Name(VK_OBJECT_TYPE_SHADER_MODULE, u64(module), debugName);
    }
    void Destroy() override;
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
    struct Element
    {
        std::string semantic; // upper-case
        u32 index = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        u32 slot = 0;
        u32 offset = 0;       // resolved APPEND_ALIGNED
        bool instance = false;
        u32 stepRate = 1;
    };
    std::vector<Element> elements;
    u32 slotMask = 0;
    bool slotInstance[kMaxVBs]{};
    u32 slotStepRate[kMaxVBs]{};
    u64 hash = 0;
    explicit InputLayout(Device* device) : DeviceChild(device) {}
};

// ============================================================================================
// SPIR-V (vk_spirv.cpp)
// ============================================================================================

// Reads the bindings, interface and workgroup size of a module DXC compiled with SpirvCompileArgs, moves its
// descriptor set to the stage's, assigns stage I/O locations by semantic (a process-wide table, so a VS and
// the PS it feeds agree whatever order each declared them in) and strips the reflection decorations.
bool PrepareSpirv(const u32* words, size_t count, Stage stage, const ShaderBinding* table, size_t tableCount,
    std::vector<u32>& out, ShaderInterface& iface, std::string& entry, bool& mesh, std::string& error);
// A copy of `consumer`'s SPIR-V whose inputs sit at the locations `producer` writes the same semantics to;
// false (and `out` untouched) when they already agree.
bool RelinkInputs(const Shader& producer, const Shader& consumer, std::vector<u32>& out);
// Capabilities the module declares that the device lacks (empty = fine).
std::string MissingCapabilities(Device* device, const std::vector<u32>& spirv);

// ============================================================================================
// Pipelines (vk_infra.cpp)
// ============================================================================================

// Everything a graphics pipeline depends on that the device does not let be dynamic.
struct GraphicsPipelineDesc
{
    Shader* vs = nullptr;
    Shader* hs = nullptr;
    Shader* ds = nullptr;
    Shader* gs = nullptr;
    Shader* ps = nullptr;
    InputLayout* inputLayout = nullptr;   // null with dynamic vertex input
    u32 vbStrides[kMaxVBs]{};             // with a static input layout: the bound strides
    BlendState* blend = nullptr;          // null with EDS3 colour blend state (or D3D11 default)
    RasterizerState* raster = nullptr;    // polygon mode, depth clamp (unless EDS3)
    u32 sampleMask = 0xffffffff;          // unless EDS3
    VkPrimitiveTopology topologyClass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // the class; the exact topology is dynamic
    u32 patchPoints = 0;
    u32 numRTVs = 0;
    VkFormat rtvFormats[kMaxRTVs]{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    u32 samples = 1;
    bool alphaToCoverage = false;         // unless EDS3
    bool shadingRate = false;             // attachment shading rate used
    u64 Hash() const;
};

// Pipelines by content + the persistent VkPipelineCache. Thread-safe.
class PipelineCache
{
public:
    bool Init(Device* device, const wchar_t* path);
    void Shutdown(); // saves
    void Save();
    // Returns null (and logs once) when the driver refuses the state.
    VkPipeline Graphics(const GraphicsPipelineDesc& desc, u64 hash, const PipelineLayout* layout);
    VkPipeline Compute(Shader* cs, const PipelineLayout* layout);
    // A shader object goes: pipelines keyed by its pointer are no longer looked up (their content keys stay).
    void Forget(Shader* shader);
    VkPipelineCache Handle() const { return m_cache; }
    u32 Count() const { return m_count.load(); }

    // Grows whenever an optimized pipeline replaced a fast-linked one: contexts look their pipeline up again.
    u64 Generation() const { return m_generation.load(std::memory_order_acquire); }

private:
    struct Worker;
    VkPipeline CreateGraphics(const GraphicsPipelineDesc& desc, const PipelineLayout* layout);
    VkPipeline LinkGraphics(const GraphicsPipelineDesc& desc, u64 hash, const PipelineLayout* layout);
    VkPipeline Library(std::unordered_map<u64, VkPipeline>& map, u64 key, const std::function<VkPipeline()>& create);
    const PipelineLayout* PartialLayout(const PipelineLayout* full, bool preRaster);
    void WorkerLoop();
    Device* m_device = nullptr;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    std::wstring m_path;
    std::shared_mutex m_lock;
    std::unordered_map<u64, VkPipeline> m_pipelines;
    std::unordered_map<u64, VkPipeline> m_vertexInput, m_preRaster, m_fragment, m_output; // libraries
    std::vector<VkPipeline> m_retired; // fast-linked pipelines an optimized one replaced
    Worker* m_worker = nullptr;
    std::atomic<u64> m_generation{0};
    std::atomic<u32> m_count{0};
    std::atomic<u64> m_compileNs{0};
    size_t m_loadedBytes = 0;
};

// ============================================================================================
// Queries and command lists (vk_objects.cpp)
// ============================================================================================

class Query final : public DeviceChild<ID3D11Query>
{
public:
    static HRESULT Create(Device* device, const D3D11_QUERY_DESC& desc, Query** out);
    UINT STDMETHODCALLTYPE GetDataSize() override;
    void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC* desc) override { *desc = m_desc; }

    D3D11_QUERY_DESC m_desc{};
    QueryPoolSet::Slot slot;      // occlusion / timestamp
    // Timeline value of the submission that carries the End(); 0 = ended in a buffer not yet submitted,
    // ~0ull = never ended.
    u64 value = ~0ull;
    bool active = false;
    bool begun = false;    // the Vulkan query is open in the current rendering instance
    bool slotUsed = false; // slot holds a result (it was begun and ended)
    // An occlusion query the immediate context had to end across a rendering instance or a submission
    // continues in more slots; the result is the sum.
    std::vector<QueryPoolSet::Slot> parts;
    void FreeSlots(); // after the GPU

private:
    explicit Query(Device* device) : DeviceChild(device) {}
    void Destroy() override;
};

// Recorded work of a deferred context (FinishCommandList).
class CommandList final : public DeviceChild<ID3D11CommandList>
{
public:
    UINT STDMETHODCALLTYPE GetContextFlags() override { return 0; }
    std::vector<VkCommandBuffer> buffers;  // closed primaries, in order
    Context* recordedBy = nullptr;         // AddRef'd; gets the buffers back once they were submitted
    std::vector<Query*> endedQueries;      // AddRef'd: queries ended in the list (their value is set at execution)
    // The first access of each tracked subresource (what the immediate context makes ready before the
    // list runs) and the state the list leaves it in.
    struct Requirement
    {
        Resource* resource;
        u32 subresource;               // ~0u = all of them
        VkImageLayout layout;          // UNDEFINED for buffers
        VkPipelineStageFlags2 stages;
        VkAccessFlags2 access;
        ImageState last;
        bool written = false;          // the list transitioned or wrote it: `last` replaces the tracked state
    };
    std::vector<Requirement> requirements;
    std::vector<Resource*> pendingReadback; // staging copies recorded in the list
    // Buffers the list wrote (the immediate context orders their next use after the list).
    std::vector<Resource*> writtenBuffers;
    explicit CommandList(Device* device) : DeviceChild(device) {}

protected:
    void Destroy() override;
};

void ReleaseStagingReferences(std::vector<Resource*>& resources);

// ============================================================================================
// Context (vk_context_state.cpp + vk_context_ops.cpp)
// ============================================================================================

struct StageBindings
{
    Shader* shader = nullptr;
    ID3D11DeviceChild* shaderIface = nullptr;         // AddRef'd
    ID3D11Buffer* cb[kMaxCBs]{};                      // AddRef'd
    ID3D11ShaderResourceView* srv[kMaxSRVs]{};        // AddRef'd
    ID3D11SamplerState* samplers[kMaxSamplers]{};     // AddRef'd
    bool dirty = true;                                // the set must be written again
    bool offsetsDirty = true;                         // only the dynamic offsets changed
    VkDescriptorSet set = VK_NULL_HANDLE;             // the last written set (valid while !dirty)
    VkBuffer cbAppliedBuffer[kMaxCBs]{};              // what the set holds for each CB
    VkDeviceSize cbAppliedRange[kMaxCBs]{};
    u32 dynamicOffsets[kMaxCBs]{};                    // in the order of the set's dynamic bindings
    u32 dynamicCount = 0;
};

// Per-context state tracking: the immediate context works on Resource::states; a deferred context keeps its
// own map and records the first requirement of every subresource.
class StateTracker
{
public:
    void Init(Context* context, bool deferred) { m_context = context; m_deferred = deferred; }
    // Brings the subresources to `layout` for an access at `stages`/`access`; appends image barriers.
    void Require(Resource* resource, const SubRange& range, VkImageLayout layout, VkPipelineStageFlags2 stages,
        VkAccessFlags2 access, std::vector<VkImageMemoryBarrier2>& barriers);
    // A GPU-written buffer: the source scope of the memory barrier it needs (false: none).
    bool RequireBuffer(Resource* resource, VkPipelineStageFlags2 stages, VkAccessFlags2 access,
        VkPipelineStageFlags2& srcStages, VkAccessFlags2& srcAccess);
    void Harvest(std::vector<CommandList::Requirement>& out);
    void Reset();

private:
    struct Local
    {
        std::vector<ImageState> first;   // the first access of each subresource (the list's entry requirement)
        std::vector<ImageState> current;
        std::vector<u8> touched;         // 0 untouched, 1 only read in the entry layout, 2 transitioned or written
    };
    Local& LocalOf(Resource* resource, u32 count);
    // One subresource: the immediate's own state or the list's.
    bool Access(Resource* resource, u32 sub, u32 count, VkImageLayout layout, VkPipelineStageFlags2 stages,
        VkAccessFlags2 access, VkPipelineStageFlags2& srcStages, VkAccessFlags2& srcAccess, VkImageLayout& oldLayout);
    Context* m_context = nullptr;
    bool m_deferred = false;
    std::unordered_map<Resource*, Local> m_local;
};

class Context final : public DeviceChild<ID3D11DeviceContext>
{
public:
    Context(Device* device, bool deferred, u32 index);
    ~Context() override;

    // ---- ID3D11DeviceContext ----
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
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;

    // ---- internal ----
    bool IsDeferred() const { return m_deferred; }
    u32 Index() const { return m_index; }
    Device* device() const { return m_device; }

    // vk_context_ops.cpp: command buffers
    VkCommandBuffer Cmd(); // the open command buffer (opens one: everything applied is invalid then)
    bool HasOpenCmd() const { return m_cmd != VK_NULL_HANDLE; }
    // Immediate only: submits the recorded work and the queued deferred buffers in one vkQueueSubmit2;
    // returns its timeline value. The swap chain passes its binary semaphores for the frame's last one.
    u64 Submit(VkSemaphore waitAcquire = VK_NULL_HANDLE, VkSemaphore signalPresent = VK_NULL_HANDLE);
    // Buffers of this context come back once the submission with `value` completed (0 = never submitted).
    void ReturnBuffers(std::vector<VkCommandBuffer>& buffers, u64 value);
    // Rendering (dynamic rendering, begun lazily at a draw with the bound targets).
    void EndRendering();
    bool BeginRendering();
    bool InRendering() const { return m_rendering; }
    void MarkTargetsDirty() { m_targetsDirty = true; }
    // Barriers: tracked images to a layout, GPU-written buffers by one merged memory barrier; all
    // collected and recorded in one vkCmdPipelineBarrier2 by FlushBarriers (DOOM's batching).
    void RequireImage(Resource* resource, const SubRange& range, VkImageLayout layout, VkPipelineStageFlags2 stages,
        VkAccessFlags2 access);
    void RequireImageAll(Resource* resource, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access);
    void RequireBuffer(Resource* resource, VkPipelineStageFlags2 stages, VkAccessFlags2 access);
    bool BarriersPending() const { return !imageBarriers.empty() || memoryBarrierPending; }
    void FlushBarriers();
    void InvalidateApplied();
    // The layouts the layer keeps a resource in for each use (storage-capable images stay in GENERAL).
    static VkImageLayout ReadLayout(const Resource* r);
    static VkImageLayout TransferLayout(const Resource* r, bool dst);

    // vk_context_state.cpp: draw preparation
    bool PrepareDraw(bool indexed, bool mesh = false);
    bool PrepareDispatch();
    void UnbindSrvsOf(Resource* resource, const SubRange& range);
    bool IsBoundAsOutput(Resource* resource, const SubRange& range) const;
    void IndirectCount(bool indexed, ID3D11Buffer* args, UINT offset, UINT maxCount, ID3D11Buffer* count, UINT countOffset);
    void ReleaseBindings();

    // ---- D3D11-level state ----
    StageBindings stages[kStageCount];
    ID3D11InputLayout* inputLayout = nullptr; // AddRef'd
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
    ID3D11BlendState* blendState = nullptr;        // AddRef'd
    FLOAT blendFactor[4]{1, 1, 1, 1};
    UINT sampleMask = 0xffffffff;
    ID3D11DepthStencilState* depthState = nullptr; // AddRef'd
    UINT stencilRef = 0;
    ID3D11RasterizerState* rasterState = nullptr;  // AddRef'd
    D3D11_VIEWPORT viewports[kMaxViewports]{};
    UINT numViewports = 0;
    D3D11_RECT scissors[kMaxViewports]{};
    UINT numScissors = 0;
    ID3D11RenderTargetView* rtvs[kMaxRTVs]{};       // AddRef'd
    UINT numRTVs = 0;
    ID3D11DepthStencilView* dsv = nullptr;          // AddRef'd
    ID3D11UnorderedAccessView* csUavs[kMaxUAVs]{};  // AddRef'd
    ID3D11UnorderedAccessView* psUavs[kMaxUAVs]{};  // AddRef'd (OM UAVs, by register)
    // vk_api.h state.
    UINT shadingRate = 0;                           // D3D12_SHADING_RATE value
    bool shadingRateImage = false;
    UINT rateCombiner = 0;                          // D3D12_SHADING_RATE_COMBINER of the image
    ID3D11Resource* rateImage = nullptr;            // AddRef'd
    u32 uavOverlap = 0;                             // BeginUavOverlap depth
    u8 computePush[kRtPushConstantBytes]{};         // SetComputePushConstants
    u32 computePushSize = 0;

    // ---- applied Vulkan state (what the open command buffer has) ----
    struct Applied
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        const PipelineLayout* layout = nullptr;     // of the bound graphics pipeline
        bool pipelineDirty = true;                  // pipeline-relevant D3D11 state changed since the lookup
        u64 pipelineHash = 0;
        u64 pipelineGeneration = 0;
        bool meshPipeline = false;
        VkPipeline computePipeline = VK_NULL_HANDLE;
        const PipelineLayout* computeLayout = nullptr;
        VkDescriptorSet sets[kGraphicsStages]{};    // bound graphics sets
        bool setsBound = false;                     // the sets above are bound with `layout`
        VkDescriptorSet computeSet = VK_NULL_HANDLE;
        bool computeSetBound = false;
        bool computeBindlessBound = false;          // the device's bindless set with computeLayout
        bool computePushDirty = true;               // computePush not yet pushed with computeLayout
        bool vbDirty = true;
        VkBuffer vbBuffers[kMaxVBs]{};
        VkDeviceSize vbOffsets[kMaxVBs]{};
        bool ibDirty = true;
        VkBuffer ibBuffer = VK_NULL_HANDLE;
        VkDeviceSize ibOffset = 0;
        // Dynamic state groups.
        bool viewportDirty = true;
        bool rasterDirty = true;
        bool depthDirty = true;
        bool stencilRefDirty = true;
        bool blendFactorDirty = true;
        bool blendDirty = true;
        bool multisampleDirty = true;
        bool topologyDirty = true;
        bool vertexInputDirty = true;
        bool rateDirty = true;
        u64 vertexInputKey = 0;
        VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        bool restart = false;
        u32 patchPoints = 0;
        // Hazards: bindings or GPU writes changed since the last draw's requirements were met.
        bool hazardsDirty = true;
        bool computeHazardsDirty = true;
        bool uavWritten = false; // the last draw or dispatch wrote UAVs (D3D11 orders the next after it)
    } applied;

    // ---- recording ----
    DescriptorAllocator descriptors;
    UploadAllocator upload;
    DynamicHeap* dynamicHeap = nullptr; // orphaned (not deleted) with the context while versions remain
    StateTracker tracker;
    std::vector<VkImageMemoryBarrier2> imageBarriers;
    VkMemoryBarrier2 memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    bool memoryBarrierPending = false;
    std::vector<Query*> endedQueries;           // immediate: queries ended in buffers not yet submitted (AddRef'd)
    std::vector<Query*> activeQueries;          // immediate: occlusion queries between Begin and End (AddRef'd)
    std::vector<Resource*> pendingReadback;     // staging copies recorded and not submitted (AddRef'd)
    std::vector<Resource*> writtenBuffers;      // deferred: GPU-written buffers of the list
    struct TextureMap
    {
        Resource* resource = nullptr;
        u32 subresource = 0;
        UploadAlloc alloc;
        u32 rowPitch = 0, depthPitch = 0;
    };
    std::vector<TextureMap> textureMaps;
    // Clears that wait for the rendering of their attachment to fold into its load operation.
    struct PendingClear
    {
        View* view = nullptr;    // kept alive by `iface`
        IUnknown* iface = nullptr; // AddRef'd
        bool color = true;
        VkClearValue value{};
        VkImageAspectFlags aspects = 0;
    };
    std::vector<PendingClear> pendingClears;
    void FlushPendingClears(Resource* only = nullptr); // records them now (outside rendering)
    // Descriptor sets written this frame by content: the same bindings reuse the set (no write at all).
    struct SetCacheEntry
    {
        u64 key;
        VkDescriptorSet set;
    };
    std::vector<SetCacheEntry> setCache;
    u64 setCacheFrame = ~0ull;
    // Debug labels (ID3DUserDefinedAnnotation), kept balanced per command buffer.
    class Annotation;
    Annotation* annotation = nullptr;
    std::vector<std::string> openLabels;
    void Stat(u32 stat, u64 amount = 1);
    struct Queued
    {
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        Context* owner = nullptr; // AddRef'd unless it is this context: gets the buffer back
    };

private:
    bool m_deferred;
    u32 m_index;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    bool m_rendering = false;
    struct RenderingKey
    {
        VkImageView color[kMaxRTVs]{};
        VkImageView depth = VK_NULL_HANDLE;
        VkImageView rate = VK_NULL_HANDLE;
        u32 colorCount = 0;
        u32 width = 0, height = 0, layers = 1;
        UINT dsvFlags = 0;
        bool operator==(const RenderingKey& o) const;
    } m_renderingKey;
    bool m_targetsDirty = true; // RTVs/DSV changed since the rendering began
    RenderingKey CurrentTargets();
    VkImageView RateView(Resource* r);
    u64 m_initWaited = 0, m_initGraphicsWaited = 0; // creation-time submissions the queue already waits for
    // Command buffers: one pool per context; a buffer is reused once the submission that carried it completed.
    VkCommandPool m_pool = VK_NULL_HANDLE;
    std::mutex m_poolLock; // m_free only: ReturnBuffers comes from the immediate context's thread
    std::vector<std::pair<VkCommandBuffer, u64>> m_free;
    std::vector<Queued> m_queued; // immediate: buffers waiting for the next submission, in order
    std::mutex m_queuedLock;
    VkCommandBuffer CloseOwn();
    void OpenCmd();
    // Occlusion queries open across a rendering instance: ended in it, continued in the next (parts).
    void SuspendQueries();
    void ResumeQueries();
    bool m_queriesSuspended = false;
    friend class Device;
    friend class SwapChain;
    friend struct ContextAccess;
};

// ============================================================================================
// Device (vk_device.cpp)
// ============================================================================================

class Device final : public ID3D11Device
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

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
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return D3D_FEATURE_LEVEL_11_1; }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return 0; }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** context) override;
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT) override { return S_OK; }
    UINT STDMETHODCALLTYPE GetExceptionMode() override { return 0; }

    // ---- internal ----
    static Device* From(ID3D11Device* device) { return static_cast<Device*>(device); }
    HRESULT Init(const DeviceCreateInfo& info);
    void Shutdown();

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice vk = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceLimits limits{};
    NativeCaps caps;
    UINT disabledFeatures = 0;
    // Queues: graphics (also presents), async compute, transfer. Families may coincide.
    u32 graphicsFamily = 0, computeFamily = ~0u, transferFamily = ~0u;
    VkQueue graphicsQueue = VK_NULL_HANDLE, computeQueue = VK_NULL_HANDLE, transferQueue = VK_NULL_HANDLE;
    u32 presentQueueIndex = 0, presentQueueCount = 0; // graphics-family queues for an SDK's presenter
    std::mutex graphicsQueueLock; // the immediate context, the init uploader (no transfer queue) and present
    std::mutex transferQueueLock;
    std::mutex computeQueueLock;
    bool surfaceMaintenance = false, swapchainMaintenance = false, mutableSwapchain = false;
    VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties{};
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR shadingRateProperties{};
    Timeline timeline;            // the graphics queue's submissions
    DeferredReleaser releaser;
    MemoryAllocator memory;
    UploadPagePool uploadPages;    // Upload class: per-context frame data
    UploadPagePool readbackPages;  // Readback class
    InitUploader init;             // creation-time data: the transfer queue where there is one
    InitUploader initGraphics;     // what a transfer queue cannot do (clears, depth uploads); unused without one
    InitUploader& GraphicsInit() { return transferQueue ? initGraphics : init; }
    DynamicHeap* creationHeap = nullptr; // first versions of dynamic and renamed buffers created with data
    LayoutCache layouts;
    NullResources nulls;
    PipelineCache pipelines;
    QueryPoolSet queryPools;
    // The RT lane (vk_raytracing.cpp): the bindless set (one index space for its texture and buffer arrays) and its
    // free ranges; a freed range comes back after the frames in flight.
    struct Bindless
    {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        u32 capacity = 0;
        std::mutex lock;
        std::vector<std::pair<u32, u32>> free; // (first, count) sorted by first, coalesced
    } bindless;
    bool InitBindless();
    void ShutdownBindless();
    VkDeviceSize scratchAlignment = 256; // minAccelerationStructureScratchOffsetAlignment, at least 256
    // TextureWriteGeneration (vk_api.h).
    std::atomic<u64> textureWrites{1};
    void TextureWritten() { textureWrites.fetch_add(1, std::memory_order_relaxed); }
    // Device features enabled (decide paths).
    bool eds3Blend = false, eds3Polygon = false, eds3SampleMask = false, eds3AlphaToCoverage = false,
         eds3DepthClamp = false, eds3Samples = false, eds3DepthClip = false;
    bool vertexInputDynamic = false;
    bool nullDescriptor = false;
    bool pushDescriptors = false;
    bool depthClipEnable = false; // VK_EXT_depth_clip_enable (pipeline-static path)
    bool debugUtils = false;
    bool labels = false;          // debug labels of the engine's annotation scopes (-vkdebug / capture tools)
    bool customBorderColor = false;
    // Shader capabilities the device enabled (MissingCapabilities refuses modules that need more).
    struct ShaderFeatures
    {
        bool geometry = false, tessellation = false, float16 = false, float64 = false, int64 = false,
             int64Atomics = false, int16 = false, int8 = false, clipDistance = false, cullDistance = false,
             cubeArray = false, sampleRate = false, minLod = false, storageExtended = false, geometryStreams = false,
             readWithoutFormat = false, writeWithoutFormat = false, multiViewport = false, outputLayer = false,
             outputViewport = false, drawParameters = false, rayQuery = false, stencilExport = false,
             viewportLayer = false, fullyCovered = false, mesh = false, interlock = false;
    } shaderFeatures;
    bool memoryPriority = false;  // VK_EXT_memory_priority (+ pageable device local memory)
    bool cubeArray = false;
    bool depthBiasClamp = false;
    bool preciseOcclusion = false;
    VkDescriptorPool emptyPool = VK_NULL_HANDLE;
    VkDescriptorSet emptySet = VK_NULL_HANDLE; // fills the set slots of stages without a shader
    bool patchControlPointsDynamic = false;
    bool topologyUnrestricted = false; // EDS3: the exact topology class may change without a new pipeline
    // -vkstats counters.
    enum Stat : u32
    {
        StatDraws, StatDispatches, StatPipelines, StatPipelineBinds, StatSets, StatOffsetsOnly, StatBarriers,
        StatBarrierBatches, StatRenderings, StatSubmits, StatMaps, StatPools, StatLayoutChanges, StatDepthLayoutChanges,
        StatInitFlushes, StatInitKB, StatInitWaits, StatQueryNotReady, StatQuerySubmits, StatMapSubmits, StatMapWaits,
        StatFrameWaitUs,
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
    // -vkstats: the same counters per innermost engine scope (PIX event) they were counted in.
    std::mutex scopeLock;
    std::unordered_map<std::string, std::array<u64, StatCount>> scopeStats;
    void CountScope(const std::vector<std::string>& labels, Stat stat, u64 amount)
    {
        std::lock_guard lock(scopeLock);
        scopeStats[labels.empty() ? std::string("(none)") : labels.back()][stat] += amount;
    }
    Context* immediate = nullptr;

    // State object caches.
    std::mutex stateLock;
    std::unordered_map<u64, BlendState*> blendStates;
    std::unordered_map<u64, DepthStencilState*> depthStates;
    std::unordered_map<u64, RasterizerState*> rasterStates;
    std::unordered_map<u64, SamplerState*> samplerStates;
    std::atomic<u32> nextSamplerId{1};
    std::atomic<u64> nextObjectId{1};

    // Frames: `frame` increments at every Present (EndFrame). Pages and pools are tagged with it; EndFrame
    // hands the timeline value of the frame's last submission to the pools and waits until frame -
    // kFramesInFlight completed.
    std::atomic<u64> frame{1};
    u64 frameValues[kFramesInFlight]{};
    void EndFrame(u64 lastValue);
    void WaitIdle();
    void ReportLost(VkResult result);
    bool lost = false;

    DeviceCreateInfo createInfo;
    IDXGIAdapter1* adapter = nullptr; // AddRef'd
    VkSurfaceKHR TakeSurface(HWND hwnd);

    // Debug names (VK_EXT_debug_utils, only with -vkdebug).
    void Name(VkObjectType type, u64 handle, const char* name);

private:
    std::atomic<ULONG> m_refs{1};
    PrivateData m_private;
    class DxgiDevice;
    DxgiDevice* m_dxgiDevice = nullptr;
};

// ============================================================================================
// Swap chain (vk_swapchain.cpp)
// ============================================================================================

class SwapChain final : public IDXGISwapChain1
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override;
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
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** unk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override;

    // The only stage that waits for an acquired image: Present's copy of the back buffer into it.
    static constexpr VkPipelineStageFlags2 kAcquireStages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    static HRESULT Create(Device* device, IDXGIFactory2* factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, SwapChain** out);

private:
    SwapChain() = default;
    HRESULT CreateNative(UINT syncInterval);
    friend bool SetAdaptiveVsync(IDXGISwapChain* chain, bool adaptive);
    friend bool SetSwapChainProvider(IDXGISwapChain* chain, SwapChainProvider* provider);
    // `last`: the chain's provider makes none after it.
    void DestroyNative(bool last = false);
    HRESULT CreateBackBuffer();
    bool Acquire();
    std::atomic<ULONG> m_refs{1};
    PrivateData m_private;
    Device* m_device = nullptr;        // AddRef'd
    IDXGIFactory2* m_factory = nullptr; // AddRef'd
    HWND m_hwnd = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_vkFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D m_extent{};
    VkPresentModeKHR m_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR m_vsyncMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync on: FIFO, FIFO_RELAXED when adaptive
    VkPresentModeKHR m_tearingMode = VK_PRESENT_MODE_FIFO_KHR; // vsync off
    bool m_adaptive = false;
    bool m_relaxedSupported = false;
    SwapChainProvider* m_provider = nullptr;      // what the next chain is made through (null: the driver)
    SwapChainProvider* m_chainProvider = nullptr; // what made the current one
    bool m_switchModes = false;        // VK_EXT_swapchain_maintenance1: the mode changes per present
    bool m_mutable = false;
    bool m_minimized = false;
    UINT m_syncInterval = 1;
    DXGI_SWAP_CHAIN_DESC1 m_desc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC m_fullscreenDesc{};
    bool m_hasFullscreenDesc = false;
    bool m_fullscreen = false;
    std::vector<Texture2D*> m_buffers;  // the swap chain's images (state tracking of the copy into them)
    Texture2D* m_backBuffer = nullptr;   // what GetBuffer(0) hands out: the frame renders into it
    u32 m_current = 0;
    UINT m_presentCount = 0;
    u64 m_presentId = 0;
    u64 m_chainFirstId = 1; // the first present id of the current VkSwapchainKHR
    xrpacing::PresentClock m_clock;
    // Acquire semaphores rotate per frame; each image has the semaphore its present waits for.
    VkSemaphore m_acquire[kFramesInFlight + 1]{};
    u32 m_acquireIndex = 0;
    VkSemaphore m_pendingAcquire = VK_NULL_HANDLE;
    std::vector<VkSemaphore> m_renderDone;
    bool m_acquired = false;
    bool m_outOfDate = false;
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
} // namespace xrvk
