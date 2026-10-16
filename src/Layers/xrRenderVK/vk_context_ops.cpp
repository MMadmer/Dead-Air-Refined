// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Context, part 2: state tracking, command buffers and submission, dynamic rendering with clears folded
// into load operations, batched barriers, Map/Unmap, copies, clears, resolves, mip generation, queries,
// deferred command lists, debug labels.
//
// A deferred context records its own primary command buffer without knowing what state the resources will
// be in when the list runs: it tracks the first access of every subresource as the list's requirement and
// the state it leaves behind. ExecuteCommandList meets the requirements with one barrier batch at the end
// of the immediate buffer and queues the list's buffer right after it - DOOM Eternal's "pre-barrier"
// scheme, with no secondary command buffers.
#include "vk_layer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <cwchar>
#include <new>
#include <numeric>

namespace xrvk
{
namespace
{
constexpr u64 kEmergencyFrameBytes = 512ull << 20; // frame memory without a Present before a frame is forced

template <class... Args>
void Once(const char* fmt, Args... args)
{
    LogOnce(fmt, fmt, args...);
}

inline View* AsView(ID3D11ShaderResourceView* v) { return static_cast<ShaderResourceView*>(v); }
inline View* AsView(ID3D11RenderTargetView* v) { return static_cast<RenderTargetView*>(v); }
inline View* AsView(ID3D11DepthStencilView* v) { return static_cast<DepthStencilView*>(v); }
inline View* AsView(ID3D11UnorderedAccessView* v) { return static_cast<UnorderedAccessView*>(v); }

IUnknown* ComOf(Resource* r)
{
    switch (r->dimension)
    {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return static_cast<ID3D11Buffer*>(static_cast<Buffer*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return static_cast<ID3D11Texture1D*>(static_cast<Texture1D*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return static_cast<ID3D11Texture2D*>(static_cast<Texture2D*>(r));
    default: return static_cast<ID3D11Texture3D*>(static_cast<Texture3D*>(r));
    }
}

void AddRefResource(Resource* r) { ComOf(r)->AddRef(); }
void ReleaseResource(Resource* r) { ComOf(r)->Release(); }

Query* AsQuery(ID3D11Asynchronous* async)
{
    return static_cast<Query*>(static_cast<ID3D11Query*>(async));
}

bool IsOcclusion(D3D11_QUERY type)
{
    return type == D3D11_QUERY_OCCLUSION || type == D3D11_QUERY_OCCLUSION_PREDICATE;
}

bool IsIntegerFormat(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        return true;
    default:
        return false;
    }
}

bool IsSignedInteger(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        return true;
    default:
        return false;
    }
}

VkClearColorValue ColorValue(VkFormat format, const FLOAT color[4])
{
    VkClearColorValue v{};
    if (IsIntegerFormat(format))
    {
        // D3D11 converts the floats of a colour clear to the integer format.
        for (u32 i = 0; i < 4; ++i)
        {
            if (IsSignedInteger(format))
                v.int32[i] = i32(color[i]);
            else
                v.uint32[i] = color[i] <= 0.f ? 0u : u32(color[i]);
        }
    }
    else
        std::memcpy(v.float32, color, sizeof(float) * 4);
    return v;
}

VkImageSubresourceRange RangeOf(const Resource* r, const SubRange& range, VkImageAspectFlags aspect)
{
    // A 3D image has one layer: a view's W slices are depth, not layers.
    if (r->imageInfo.imageType == VK_IMAGE_TYPE_3D)
        return {aspect ? aspect : r->aspect, range.firstMip, range.mipCount, 0, VK_REMAINING_ARRAY_LAYERS};
    return {aspect ? aspect : r->aspect, range.firstMip, range.mipCount, range.firstLayer, range.layerCount};
}

SubRange WholeRange(const Resource* r)
{
    SubRange s;
    s.firstMip = 0;
    s.mipCount = r->mipLevels;
    s.firstLayer = 0;
    s.layerCount = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : r->arraySize;
    return s;
}

SubRange RangeOfSub(const Resource* r, u32 sub)
{
    SubRange s;
    s.firstMip = sub % r->mipLevels;
    s.mipCount = 1;
    s.firstLayer = sub / r->mipLevels;
    s.layerCount = 1;
    return s;
}

u32 LayerCountOf(const Resource* r)
{
    return r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : r->arraySize;
}

u32 SubCountOf(const Resource* r)
{
    return r->IsBuffer() ? 1 : r->mipLevels * LayerCountOf(r);
}

VkImageMemoryBarrier2 MakeBarrier(Resource* r, u32 firstMip, u32 mips, u32 firstLayer, u32 layers, VkImageLayout from,
    VkImageLayout to, VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStages,
    VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStages ? srcStages : VK_PIPELINE_STAGE_2_NONE;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStages;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = r->image;
    b.subresourceRange = {r->aspect, firstMip, mips, firstLayer, layers};
    // All depth slices of a 3D image (the maintenance9 meaning of a layer count of 1 would be one slice).
    if (r->imageInfo.imageType == VK_IMAGE_TYPE_3D)
    {
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    }
    return b;
}

bool SameState(const ImageState& a, const ImageState& b)
{
    return a.layout == b.layout && a.writeStages == b.writeStages && a.writeAccess == b.writeAccess &&
        a.readStages == b.readStages && a.readAccess == b.readAccess;
}

const char kLostFormat[] = "! the device is lost: %s skipped";
} // namespace

// ================================================================================================
// Annotations (ID3DUserDefinedAnnotation: the engine's PIX scopes become debug labels)
// ================================================================================================

class Context::Annotation final : public ID3DUserDefinedAnnotation
{
public:
    explicit Annotation(Context* owner) : m_owner(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override { return m_owner->QueryInterface(riid, object); }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }
    INT STDMETHODCALLTYPE BeginEvent(LPCWSTR name) override
    {
        char text[256];
        const int n = name ? WideCharToMultiByte(CP_UTF8, 0, name, -1, text, sizeof(text) - 1, nullptr, nullptr) : 0;
        text[n > 0 ? n : 0] = 0;
        try
        {
            m_owner->openLabels.emplace_back(text);
        }
        catch (...)
        {
            return -1;
        }
        if (m_owner->HasOpenCmd())
            Begin(m_owner->Cmd(), text);
        return INT(m_owner->openLabels.size()) - 1;
    }
    INT STDMETHODCALLTYPE EndEvent() override
    {
        if (m_owner->openLabels.empty())
            return -1;
        m_owner->openLabels.pop_back();
        if (m_owner->HasOpenCmd() && Enabled())
            g_vk.vkCmdEndDebugUtilsLabelEXT(m_owner->Cmd());
        return INT(m_owner->openLabels.size());
    }
    void STDMETHODCALLTYPE SetMarker(LPCWSTR name) override
    {
        if (!Enabled() || !m_owner->HasOpenCmd() || !g_vk.vkCmdInsertDebugUtilsLabelEXT)
            return;
        char text[256];
        const int n = name ? WideCharToMultiByte(CP_UTF8, 0, name, -1, text, sizeof(text) - 1, nullptr, nullptr) : 0;
        text[n > 0 ? n : 0] = 0;
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = text;
        g_vk.vkCmdInsertDebugUtilsLabelEXT(m_owner->Cmd(), &label);
    }
    BOOL STDMETHODCALLTYPE GetStatus() override { return Enabled(); }

    bool Enabled() const { return m_owner->device()->labels && g_vk.vkCmdBeginDebugUtilsLabelEXT; }
    void Begin(VkCommandBuffer cmd, const char* text)
    {
        if (!Enabled())
            return;
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = text;
        g_vk.vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
    }
    // Labels stay balanced inside each command buffer.
    void Suspend(VkCommandBuffer cmd)
    {
        if (!Enabled())
            return;
        for (size_t i = 0; i < m_owner->openLabels.size(); ++i)
            g_vk.vkCmdEndDebugUtilsLabelEXT(cmd);
    }
    void Resume(VkCommandBuffer cmd)
    {
        for (const std::string& name : m_owner->openLabels)
            Begin(cmd, name.c_str());
    }

private:
    Context* m_owner;
};

HRESULT STDMETHODCALLTYPE Context::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11DeviceContext))
    {
        *object = static_cast<ID3D11DeviceContext*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3DUserDefinedAnnotation) && annotation)
    {
        *object = static_cast<ID3DUserDefinedAnnotation*>(annotation);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

// ================================================================================================
// State tracking
// ================================================================================================

StateTracker::Local& StateTracker::LocalOf(Resource* resource, u32 count)
{
    Local& l = m_local[resource];
    if (l.touched.size() != count)
    {
        l.first.assign(count, ImageState{});
        l.current.assign(count, ImageState{});
        l.touched.assign(count, 0);
    }
    return l;
}

bool StateTracker::Access(Resource* r, u32 sub, u32 count, VkImageLayout layout, VkPipelineStageFlags2 stages,
    VkAccessFlags2 access, VkPipelineStageFlags2& src, VkAccessFlags2& srcAccess, VkImageLayout& oldLayout)
{
    const bool write = IsWriteAccess(access);
    if (!m_deferred)
    {
        ImageState& s = r->IsBuffer() ? r->bufferState : (r->uniform ? r->states[0] : r->states[sub]);
        return Transition(s, layout, stages, access, src, srcAccess, oldLayout);
    }
    Local& l = LocalOf(r, count);
    u8& touched = l.touched[sub];
    ImageState& first = l.first[sub];
    ImageState& cur = l.current[sub];
    if (!touched)
    {
        // The entry requirement: the immediate context puts the subresource there before the list runs.
        first = ImageState{layout, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, stages, access};
        cur = write ? ImageState{layout, stages, access, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE}
                    : ImageState{layout, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, stages, access};
        touched = write ? 2 : 1;
        return false;
    }
    if (touched == 1 && !write && (layout == first.layout || layout == VK_IMAGE_LAYOUT_UNDEFINED))
    {
        // More reads of what the list found: the requirement covers them too.
        first.readStages |= stages;
        first.readAccess |= access;
        cur.readStages |= stages;
        cur.readAccess |= access;
        return false;
    }
    touched = 2;
    return Transition(cur, layout, stages, access, src, srcAccess, oldLayout);
}

void StateTracker::Require(Resource* r, const SubRange& range, VkImageLayout layout, VkPipelineStageFlags2 stages,
    VkAccessFlags2 access, std::vector<VkImageMemoryBarrier2>& barriers)
{
    const u32 layers = LayerCountOf(r);
    const u32 count = r->mipLevels * layers;
    const bool whole = range.firstMip == 0 && range.mipCount >= r->mipLevels && range.firstLayer == 0 &&
        range.layerCount >= layers;
    VkPipelineStageFlags2 src = 0;
    VkAccessFlags2 srcAccess = 0;
    VkImageLayout old = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!m_deferred)
    {
        if (r->states.empty())
            r->states.assign(1, ImageState{});
        if (r->uniform && whole)
        {
            if (Transition(r->states[0], layout, stages, access, src, srcAccess, old))
                barriers.push_back(MakeBarrier(r, 0, r->mipLevels, 0, layers, old, layout, src, srcAccess, stages, access));
            return;
        }
        if (r->uniform)
        {
            const ImageState s = r->states[0];
            r->states.assign(count, s);
            r->uniform = false;
        }
    }
    for (u32 layer = range.firstLayer; layer < range.firstLayer + range.layerCount && layer < layers; ++layer)
    {
        for (u32 mip = range.firstMip; mip < range.firstMip + range.mipCount && mip < r->mipLevels; ++mip)
        {
            const u32 sub = mip + layer * r->mipLevels;
            if (!Access(r, sub, count, layout, stages, access, src, srcAccess, old))
                continue;
            // Merge with the previous barrier when it is the neighbouring mip of the same transition.
            if (!barriers.empty())
            {
                VkImageMemoryBarrier2& last = barriers.back();
                VkImageSubresourceRange& lr = last.subresourceRange;
                if (last.image == r->image && last.oldLayout == old && last.newLayout == layout &&
                    last.srcStageMask == (src ? src : VK_PIPELINE_STAGE_2_NONE) && last.srcAccessMask == srcAccess &&
                    last.dstStageMask == stages && last.dstAccessMask == access && lr.baseArrayLayer == layer &&
                    lr.layerCount == 1 && lr.baseMipLevel + lr.levelCount == mip)
                {
                    ++lr.levelCount;
                    continue;
                }
            }
            barriers.push_back(MakeBarrier(r, mip, 1, layer, 1, old, layout, src, srcAccess, stages, access));
        }
    }
    // Back to one state when the whole resource agrees again.
    if (!m_deferred && !r->uniform && whole)
    {
        bool same = true;
        for (u32 i = 1; i < count && same; ++i)
            same = SameState(r->states[i], r->states[0]);
        if (same)
        {
            r->states.resize(1);
            r->uniform = true;
        }
    }
}

bool StateTracker::RequireBuffer(Resource* r, VkPipelineStageFlags2 stages, VkAccessFlags2 access,
    VkPipelineStageFlags2& src, VkAccessFlags2& srcAccess)
{
    VkImageLayout old;
    return Access(r, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, stages, access, src, srcAccess, old);
}

void StateTracker::Harvest(std::vector<CommandList::Requirement>& out)
{
    for (auto& [r, l] : m_local)
    {
        const u32 count = u32(l.touched.size());
        // One requirement for the whole resource when every subresource agrees.
        bool uniform = count > 0;
        for (u32 i = 0; i < count && uniform; ++i)
            uniform = l.touched[i] == l.touched[0] && SameState(l.first[i], l.first[0]) && SameState(l.current[i], l.current[0]);
        for (u32 i = 0; i < count; ++i)
        {
            if (!l.touched[i])
                continue;
            CommandList::Requirement req{};
            req.resource = r;
            req.subresource = uniform ? ~0u : i;
            req.layout = l.first[i].layout;
            req.stages = l.first[i].readStages;
            req.access = l.first[i].readAccess;
            req.last = l.current[i];
            req.written = l.touched[i] == 2;
            out.push_back(req);
            if (uniform)
                break;
        }
    }
    Reset();
}

void StateTracker::Reset()
{
    m_local.clear();
}

// ================================================================================================
// Lifecycle
// ================================================================================================

Context::Context(Device* device, bool deferred, u32 index) : DeviceChild(device), m_deferred(deferred), m_index(index)
{
    descriptors.Init(device);
    upload.Init(&device->uploadPages);
    tracker.Init(this, deferred);
    dynamicHeap = new (std::nothrow) DynamicHeap(device);
    annotation = new (std::nothrow) Annotation(this);
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = device->graphicsFamily;
    if (g_vk.vkCreateCommandPool(device->vk, &info, nullptr, &m_pool) != VK_SUCCESS)
        m_pool = VK_NULL_HANDLE;
    try
    {
        imageBarriers.reserve(64);
        setCache.reserve(1024);
    }
    catch (...)
    {
    }
}

Context::~Context()
{
    ReleaseBindings();
    for (Query* q : endedQueries)
        q->Release();
    endedQueries.clear();
    for (Query* q : activeQueries)
        q->Release();
    activeQueries.clear();
    for (Resource* r : pendingReadback)
        ReleaseResource(r);
    pendingReadback.clear();
    for (PendingClear& pc : pendingClears)
        pc.iface->Release();
    pendingClears.clear();
    Device* dev = m_device;
    // Queued buffers of other contexts go back to them; ours die with the pool.
    {
        std::lock_guard lock(m_queuedLock);
        for (const Queued& q : m_queued)
        {
            if (q.owner && q.owner != this)
            {
                std::vector<VkCommandBuffer> one{q.buffer};
                q.owner->ReturnBuffers(one, 0);
                q.owner->Release();
            }
        }
        m_queued.clear();
    }
    if (m_cmd)
    {
        g_vk.vkEndCommandBuffer(m_cmd);
        m_cmd = VK_NULL_HANDLE;
    }
    // Buffers may still run on the GPU: the pool goes after it.
    if (m_pool)
    {
        const VkCommandPool pool = m_pool;
        dev->releaser.Defer([dev, pool] { g_vk.vkDestroyCommandPool(dev->vk, pool, nullptr); });
        m_pool = VK_NULL_HANDLE;
    }
    descriptors.Shutdown();
    if (dynamicHeap && dynamicHeap->Orphan())
        delete dynamicHeap;
    dynamicHeap = nullptr;
    delete annotation;
    annotation = nullptr;
}

// ================================================================================================
// Command buffers and submission
// ================================================================================================

void Context::OpenCmd()
{
    if (!m_pool || m_device->lost)
        return;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        std::lock_guard lock(m_poolLock);
        const u64 done = m_device->timeline.Completed(m_device->vk);
        for (size_t i = 0; i < m_free.size(); ++i)
        {
            if (m_free[i].second <= done)
            {
                cmd = m_free[i].first;
                m_free[i] = m_free.back();
                m_free.pop_back();
                break;
            }
        }
    }
    if (!cmd)
    {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = m_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (g_vk.vkAllocateCommandBuffers(m_device->vk, &alloc, &cmd) != VK_SUCCESS)
            return;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_vk.vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
        return;
    m_cmd = cmd;
    InvalidateApplied();
    if (annotation)
        annotation->Resume(cmd);
}

VkCommandBuffer Context::Cmd()
{
    if (!m_cmd)
        OpenCmd();
    return m_cmd;
}

VkCommandBuffer Context::CloseOwn()
{
    if (!m_cmd)
        return VK_NULL_HANDLE;
    EndRendering();
    FlushBarriers();
    if (annotation)
        annotation->Suspend(m_cmd);
    const VkCommandBuffer cmd = m_cmd;
    m_cmd = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkEndCommandBuffer(cmd);
    if (result != VK_SUCCESS)
    {
        m_device->ReportLost(result);
        std::vector<VkCommandBuffer> one{cmd};
        ReturnBuffers(one, 0);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

void Context::ReturnBuffers(std::vector<VkCommandBuffer>& buffers, u64 value)
{
    std::lock_guard lock(m_poolLock);
    for (VkCommandBuffer b : buffers)
    {
        if (b)
            m_free.emplace_back(b, value);
    }
    buffers.clear();
}

void Context::InvalidateApplied()
{
    applied = Applied{};
    for (StageBindings& sb : stages)
    {
        sb.dirty = true;
        sb.offsetsDirty = true;
    }
    m_rendering = false;
    m_targetsDirty = true;
}

u64 Context::Submit(VkSemaphore waitAcquire, VkSemaphore signalPresent)
{
    Device* dev = m_device;
    Timeline& timeline = dev->timeline;
    if (m_deferred)
    {
        Once("Submit on a deferred context ignored");
        return timeline.NextValue() - 1;
    }
    if (m_cmd)
        FlushPendingClears(nullptr);
    const u64 initValue = dev->init.Flush();
    const u64 initGraphicsValue = dev->transferQueue ? dev->initGraphics.Flush() : 0;
    std::unique_lock queuedLock(m_queuedLock);
    if (!m_cmd && m_queued.empty() && endedQueries.empty() && pendingReadback.empty() && !initValue &&
        !initGraphicsValue && !waitAcquire && !signalPresent)
        return timeline.NextValue() - 1;

    const VkCommandBuffer own = CloseOwn();
    std::vector<VkCommandBufferSubmitInfo> buffers;
    buffers.reserve(m_queued.size() + 1);
    for (const Queued& q : m_queued)
    {
        VkCommandBufferSubmitInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        info.commandBuffer = q.buffer;
        buffers.push_back(info);
    }
    if (own)
    {
        VkCommandBufferSubmitInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        info.commandBuffer = own;
        buffers.push_back(info);
    }

    VkSemaphoreSubmitInfo waits[3]{};
    u32 waitCount = 0;
    // Creation-time copies first: everything after them may read what they wrote.
    const u64 initDone = dev->init.LastSubmitted();
    if (initDone > m_initWaited)
    {
        Stat(Device::StatInitWaits);
        waits[waitCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[waitCount].semaphore = dev->init.timeline().Handle();
        waits[waitCount].value = initDone;
        waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++waitCount;
        m_initWaited = initDone;
    }
    if (dev->transferQueue)
    {
        const u64 graphicsDone = dev->initGraphics.LastSubmitted();
        if (graphicsDone > m_initGraphicsWaited)
        {
            waits[waitCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            waits[waitCount].semaphore = dev->initGraphics.timeline().Handle();
            waits[waitCount].value = graphicsDone;
            waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ++waitCount;
            m_initGraphicsWaited = graphicsDone;
        }
    }
    if (waitAcquire)
    {
        waits[waitCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[waitCount].semaphore = waitAcquire;
        waits[waitCount].stageMask = SwapChain::kAcquireStages;
        ++waitCount;
    }
    const u64 value = timeline.Take();
    VkSemaphoreSubmitInfo signals[2]{};
    u32 signalCount = 0;
    signals[signalCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signals[signalCount].semaphore = timeline.Handle();
    signals[signalCount].value = value;
    signals[signalCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ++signalCount;
    if (signalPresent)
    {
        signals[signalCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[signalCount].semaphore = signalPresent;
        signals[signalCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++signalCount;
    }
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waits;
    submit.commandBufferInfoCount = u32(buffers.size());
    submit.pCommandBufferInfos = buffers.data();
    submit.signalSemaphoreInfoCount = signalCount;
    submit.pSignalSemaphoreInfos = signals;
    VkResult result;
    {
        std::lock_guard lock(dev->graphicsQueueLock);
        result = g_vk.vkQueueSubmit2(dev->graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    }
    if (result != VK_SUCCESS)
    {
        dev->ReportLost(result);
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = timeline.Handle();
        info.value = value;
        g_vk.vkSignalSemaphore(dev->vk, &info);
    }
    Stat(Device::StatSubmits);

    for (const Queued& q : m_queued)
    {
        std::vector<VkCommandBuffer> one{q.buffer};
        if (q.owner && q.owner != this)
        {
            q.owner->ReturnBuffers(one, value);
            q.owner->Release();
        }
        else
            ReturnBuffers(one, value);
    }
    m_queued.clear();
    queuedLock.unlock();
    if (own)
    {
        std::vector<VkCommandBuffer> one{own};
        ReturnBuffers(one, value);
    }
    for (Query* q : endedQueries)
    {
        // A query begun again since its End finishes with a later submission.
        if (!q->active)
            q->value = value;
        q->Release();
    }
    endedQueries.clear();
    for (Resource* r : pendingReadback)
    {
        r->lastGpuWrite = value;
        r->gpuWritePending = false;
        ReleaseResource(r);
    }
    pendingReadback.clear();

    // Frames end at Present; work that never presents (long loads) would keep its frame memory forever.
    if (!signalPresent && dev->uploadPages.BytesInUse() > kEmergencyFrameBytes)
    {
        Once("~ no Present for a long time: frame memory is recycled at a submission");
        dev->EndFrame(value);
    }
    return value;
}

// ================================================================================================
// Rendering
// ================================================================================================

bool Context::RenderingKey::operator==(const RenderingKey& o) const
{
    return colorCount == o.colorCount && depth == o.depth && rate == o.rate && width == o.width && height == o.height &&
        layers == o.layers && dsvFlags == o.dsvFlags && !std::memcmp(color, o.color, sizeof(VkImageView) * colorCount);
}

Context::RenderingKey Context::CurrentTargets()
{
    RenderingKey key;
    key.colorCount = numRTVs;
    u32 width = ~0u, height = ~0u, layers = ~0u;
    auto fit = [&](View* v) {
        Resource* r = v->resource;
        width = (std::min)(width, (std::max)(1u, r->width >> v->range.firstMip));
        height = (std::min)(height, (std::max)(1u, r->height >> v->range.firstMip));
        layers = (std::min)(layers, v->range.layerCount);
    };
    for (u32 i = 0; i < numRTVs; ++i)
    {
        if (!rtvs[i])
            continue;
        View* v = AsView(rtvs[i]);
        key.color[i] = v->imageView;
        fit(v);
    }
    if (dsv)
    {
        View* v = AsView(dsv);
        key.depth = v->imageView;
        key.dsvFlags = v->dsvFlags;
        fit(v);
    }
    if (width == ~0u)
    {
        // No attachments (UAV-only or empty rendering): the viewport's extent.
        const D3D11_VIEWPORT& vp = viewports[0];
        width = u32((std::max)(1.f, vp.TopLeftX + vp.Width));
        height = u32((std::max)(1.f, vp.TopLeftY + vp.Height));
        layers = 1;
    }
    key.width = width;
    key.height = height;
    key.layers = (std::max)(1u, layers);
    if (shadingRateImage && rateImage && m_device->caps.shadingRate)
    {
        Resource* r = Resource::From(rateImage);
        if (r && r->IsImage() && (r->imageInfo.usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR))
            key.rate = RateView(r);
    }
    return key;
}

VkImageView Context::RateView(Resource* r)
{
    if (r->rateView)
        return r->rateView;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = r->image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_R8_UINT;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    info.pNext = &usage;
    if (g_vk.vkCreateImageView(m_device->vk, &info, nullptr, &r->rateView) != VK_SUCCESS)
        r->rateView = VK_NULL_HANDLE;
    return r->rateView;
}

bool Context::BeginRendering()
{
    if (m_rendering)
        return true;
    if (!Cmd())
        return false;
    const RenderingKey key = CurrentTargets();
    VkRenderingAttachmentInfo colors[kMaxRTVs]{};
    for (u32 i = 0; i < numRTVs; ++i)
    {
        VkRenderingAttachmentInfo& a = colors[i];
        a = VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!rtvs[i])
            continue;
        View* v = AsView(rtvs[i]);
        Resource* r = v->resource;
        a.imageView = key.color[i];
        a.imageLayout = (r->bindFlags & D3D11_BIND_UNORDERED_ACCESS) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // A pending clear of this view is the load operation (no separate clear pass).
        for (size_t c = 0; c < pendingClears.size(); ++c)
        {
            if (pendingClears[c].view == v && pendingClears[c].color)
            {
                a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                a.clearValue = pendingClears[c].value;
                pendingClears[c].iface->Release();
                pendingClears.erase(pendingClears.begin() + c);
                break;
            }
        }
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool hasDepth = false, hasStencil = false;
    if (dsv)
    {
        View* v = AsView(dsv);
        Resource* r = v->resource;
        hasDepth = (r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        hasStencil = (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        const bool depthRO = (v->dsvFlags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
        const bool stencilRO = (v->dsvFlags & D3D11_DSV_READ_ONLY_STENCIL) != 0 || !hasStencil;
        if (r->bindFlags & D3D11_BIND_UNORDERED_ACCESS)
            layout = VK_IMAGE_LAYOUT_GENERAL;
        else if (depthRO && stencilRO)
            layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        else if (depthRO)
            layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        else if (stencilRO && hasStencil)
            layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
        for (VkRenderingAttachmentInfo* a : {&depth, &stencil})
        {
            a->imageView = key.depth;
            a->imageLayout = layout;
            a->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        // A read-only plane is not written back (no needless decompression or store traffic).
        if (depthRO)
            depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        if (stencilRO)
            stencil.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        for (size_t c = 0; c < pendingClears.size(); ++c)
        {
            PendingClear& pc = pendingClears[c];
            if (pc.view != v || pc.color)
                continue;
            if ((pc.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && hasDepth)
            {
                depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depth.clearValue = pc.value;
                depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            if ((pc.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) && hasStencil)
            {
                stencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                stencil.clearValue = pc.value;
                stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            pc.iface->Release();
            pendingClears.erase(pendingClears.begin() + c);
            break;
        }
    }
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {key.width, key.height}};
    info.layerCount = key.layers;
    info.colorAttachmentCount = numRTVs;
    info.pColorAttachments = colors;
    info.pDepthAttachment = hasDepth ? &depth : nullptr;
    info.pStencilAttachment = hasStencil ? &stencil : nullptr;
    VkRenderingFragmentShadingRateAttachmentInfoKHR rate{VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
    if (key.rate)
    {
        rate.imageView = key.rate;
        rate.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        const u32 texel = m_device->caps.shadingRateTexel ? m_device->caps.shadingRateTexel : 16;
        rate.shadingRateAttachmentTexelSize = {texel, texel};
        info.pNext = &rate;
    }
    g_vk.vkCmdBeginRendering(m_cmd, &info);
    m_rendering = true;
    if (!(m_renderingKey.rate == key.rate))
    {
        applied.pipelineDirty = true;
        applied.rateDirty = true;
    }
    m_renderingKey = key;
    m_targetsDirty = false;
    Stat(Device::StatRenderings);
    ResumeQueries();
    return true;
}

void Context::EndRendering()
{
    if (!m_rendering)
        return;
    SuspendQueries();
    g_vk.vkCmdEndRendering(m_cmd);
    m_rendering = false;
}

void Context::SuspendQueries()
{
    Device* dev = m_device;
    for (Query* q : activeQueries)
    {
        if (!q->begun)
            continue;
        g_vk.vkCmdEndQuery(m_cmd, q->slot.pool, q->slot.index);
        q->begun = false;
        // The part ended here counts; the query continues in a new slot at the next rendering.
        try
        {
            q->parts.push_back(q->slot);
        }
        catch (...)
        {
            Once("~ an occlusion query split by a rendering boundary lost a part (out of memory)");
        }
        q->slot = dev->queryPools.Allocate(VK_QUERY_TYPE_OCCLUSION);
        q->slotUsed = false;
    }
}

void Context::ResumeQueries()
{
    for (Query* q : activeQueries)
    {
        if (q->begun || !q->slot.pool)
            continue;
        const VkQueryControlFlags precise = q->m_desc.Query == D3D11_QUERY_OCCLUSION && m_device->preciseOcclusion ?
            VK_QUERY_CONTROL_PRECISE_BIT : 0;
        g_vk.vkCmdBeginQuery(m_cmd, q->slot.pool, q->slot.index, precise);
        q->begun = true;
        q->slotUsed = true;
    }
}

void Context::FlushPendingClears(Resource* only)
{
    if (pendingClears.empty() || !Cmd())
        return;
    // Inside a rendering its own attachments clear in place.
    if (m_rendering)
    {
        VkClearAttachment attachments[kMaxRTVs + 1];
        u32 count = 0;
        for (size_t c = 0; c < pendingClears.size();)
        {
            PendingClear& pc = pendingClears[c];
            int slot = -1;
            if (pc.color)
            {
                for (u32 i = 0; i < numRTVs; ++i)
                {
                    if (rtvs[i] && AsView(rtvs[i]) == pc.view)
                        slot = int(i);
                }
            }
            const bool isDepth = !pc.color && dsv && AsView(dsv) == pc.view;
            if (slot < 0 && !isDepth)
            {
                ++c;
                continue;
            }
            VkClearAttachment& a = attachments[count++];
            a.aspectMask = pc.color ? VK_IMAGE_ASPECT_COLOR_BIT : pc.aspects;
            a.colorAttachment = slot < 0 ? 0 : u32(slot);
            a.clearValue = pc.value;
            pc.iface->Release();
            pendingClears.erase(pendingClears.begin() + c);
        }
        if (count)
        {
            const VkClearRect rect{{{0, 0}, {m_renderingKey.width, m_renderingKey.height}}, 0, m_renderingKey.layers};
            g_vk.vkCmdClearAttachments(m_cmd, count, attachments, 1, &rect);
        }
        if (pendingClears.empty())
            return;
        EndRendering();
    }
    std::vector<PendingClear> clears;
    clears.swap(pendingClears);
    for (PendingClear& pc : clears)
    {
        Resource* r = pc.view->resource;
        if (only && r != only)
        {
            pendingClears.push_back(pc);
            continue;
        }
        RequireImage(r, pc.view->range, TransferLayout(r, true), VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    FlushBarriers();
    for (PendingClear& pc : clears)
    {
        Resource* r = pc.view->resource;
        if (only && r != only)
            continue;
        const VkImageSubresourceRange range = RangeOf(r, pc.view->range, pc.color ? VK_IMAGE_ASPECT_COLOR_BIT : pc.aspects);
        const VkImageLayout layout = TransferLayout(r, true);
        if (pc.color)
            g_vk.vkCmdClearColorImage(m_cmd, r->image, layout, &pc.value.color, 1, &range);
        else
            g_vk.vkCmdClearDepthStencilImage(m_cmd, r->image, layout, &pc.value.depthStencil, 1, &range);
        pc.iface->Release();
    }
    applied.hazardsDirty = true;
    applied.computeHazardsDirty = true;
}

// ================================================================================================
// Barriers
// ================================================================================================

void Context::RequireImage(Resource* resource, const SubRange& range, VkImageLayout layout, VkPipelineStageFlags2 stages,
    VkAccessFlags2 access)
{
    Resource* r = resource;
    if (!r->image)
        return;
    tracker.Require(r, range, layout, stages, access, imageBarriers);
}

void Context::RequireImageAll(Resource* resource, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access)
{
    Resource* r = resource;
    if (!r->image)
        return;
    tracker.Require(r, WholeRange(r), layout, stages, access, imageBarriers);
}

void Context::RequireBuffer(Resource* resource, VkPipelineStageFlags2 stages, VkAccessFlags2 access)
{
    if (!resource || !resource->IsBuffer() || resource->Versioned() || !resource->buffer)
        return;
    VkPipelineStageFlags2 src = 0;
    VkAccessFlags2 srcAccess = 0;
    if (!tracker.RequireBuffer(resource, stages, access, src, srcAccess))
        return;
    // One memory barrier for every buffer hazard of the batch.
    memoryBarrier.srcStageMask |= src;
    memoryBarrier.srcAccessMask |= srcAccess;
    memoryBarrier.dstStageMask |= stages;
    memoryBarrier.dstAccessMask |= access;
    memoryBarrierPending = true;
    if (m_deferred && IsWriteAccess(access))
    {
        if (std::find(writtenBuffers.begin(), writtenBuffers.end(), resource) == writtenBuffers.end())
            writtenBuffers.push_back(resource);
    }
}

void Context::Stat(u32 stat, u64 amount)
{
    if (!m_device->stats)
        return;
    m_device->Count(Device::Stat(stat), amount);
    m_device->CountScope(openLabels, Device::Stat(stat), amount);
}

void Context::FlushBarriers()
{
    if (!BarriersPending() || !Cmd())
        return;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    if (memoryBarrierPending)
    {
        if (!memoryBarrier.srcStageMask)
            memoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &memoryBarrier;
    }
    dep.imageMemoryBarrierCount = u32(imageBarriers.size());
    dep.pImageMemoryBarriers = imageBarriers.data();
    g_vk.vkCmdPipelineBarrier2(m_cmd, &dep);
    Stat(Device::StatBarriers, imageBarriers.size() + (memoryBarrierPending ? 1 : 0));
    Stat(Device::StatBarrierBatches);
    if (m_device->stats)
    {
        for (const VkImageMemoryBarrier2& b : imageBarriers)
        {
            if (b.oldLayout == b.newLayout)
                continue;
            Stat(Device::StatLayoutChanges);
            if (b.subresourceRange.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                Stat(Device::StatDepthLayoutChanges);
        }
    }
    imageBarriers.clear();
    memoryBarrier = VkMemoryBarrier2{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memoryBarrierPending = false;
}

// ================================================================================================
// Map / Unmap
// ================================================================================================

namespace
{
void FillMapped(D3D11_MAPPED_SUBRESOURCE* mapped, void* data, u64 rowPitch, u64 depthPitch)
{
    mapped->pData = data;
    mapped->RowPitch = UINT(rowPitch);
    mapped->DepthPitch = UINT(depthPitch);
}

// A new version of a dynamic or renamed buffer from the context's heap (frame memory as a fallback).
bool NewVersion(Context& c, Resource* r, UploadAlloc& out)
{
    const VkDeviceSize size = r->byteWidth;
    DynamicHeap* heap = c.dynamicHeap;
    const DynamicVersion old = r->current;
    const bool retireHere = heap && old.heap == heap;
    if (!heap || !heap->Allocate(size, out, retireHere ? &old.alloc : nullptr))
    {
        heap = nullptr;
        Once("~ dynamic buffer versions fall back to frame memory (out of persistent memory)");
        out = c.upload.Allocate(size, 256, c.device()->frame.load(std::memory_order_acquire));
        if (!out.cpu)
            return false;
    }
    const bool retired = heap && retireHere;
    r->current.alloc = out;
    r->current.heap = heap;
    ++r->current.version;
    if (!retired && old.heap && old.heap->Retire(old.alloc))
        delete old.heap;
    return true;
}

HRESULT MapDynamicBuffer(Context& c, Resource* r, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (sub)
        return E_INVALIDARG;
    const u64 size = r->byteWidth;
    if (type == D3D11_MAP_WRITE_NO_OVERWRITE && r->current.alloc.cpu)
    {
        FillMapped(mapped, r->current.alloc.cpu, size, size);
        return S_OK;
    }
    if (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE)
    {
        Once("Map of a dynamic buffer with type %u refused (D3D11 allows DISCARD/NO_OVERWRITE)", unsigned(type));
        return E_INVALIDARG;
    }
    // DISCARD renames: draws recorded so far keep the old version, later ones read the new one.
    UploadAlloc alloc{};
    if (!NewVersion(c, r, alloc))
        return E_OUTOFMEMORY;
    FillMapped(mapped, alloc.cpu, size, size);
    return S_OK;
}

HRESULT MapDynamicTexture(Context& c, Resource* r, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (type != D3D11_MAP_WRITE_DISCARD)
    {
        Once("Map of a dynamic texture with type %u refused (D3D11 allows WRITE_DISCARD)", unsigned(type));
        return E_INVALIDARG;
    }
    if (!r->image || sub >= r->footprints.size())
        return E_INVALIDARG;
    const Resource::Footprint& f = r->footprints[sub];
    const VkDeviceSize total = VkDeviceSize(f.depthPitch) * f.depth;
    const UploadAlloc alloc = c.upload.Allocate(total, 16, c.device()->frame.load(std::memory_order_acquire));
    if (!alloc.cpu)
        return E_OUTOFMEMORY;
    Context::TextureMap* entry = nullptr;
    for (Context::TextureMap& m : c.textureMaps)
    {
        if (m.resource == r && m.subresource == sub)
        {
            entry = &m;
            break;
        }
    }
    if (!entry)
        entry = &c.textureMaps.emplace_back();
    entry->resource = r;
    entry->subresource = sub;
    entry->alloc = alloc;
    entry->rowPitch = f.rowPitch;
    entry->depthPitch = f.depthPitch;
    FillMapped(mapped, alloc.cpu, f.rowPitch, f.depthPitch);
    return S_OK;
}

HRESULT MapStaging(Context& c, Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (c.IsDeferred())
    {
        Once("Map of a staging resource on a deferred context refused");
        return E_INVALIDARG;
    }
    const bool read = type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE;
    const bool write = type == D3D11_MAP_WRITE || type == D3D11_MAP_READ_WRITE;
    if ((!read && !write) || (read && !(r->cpuAccess & D3D11_CPU_ACCESS_READ)) ||
        (write && !(r->cpuAccess & D3D11_CPU_ACCESS_WRITE)))
    {
        Once("Map of a staging resource with type %u refused (CPU access 0x%X)", unsigned(type), r->cpuAccess);
        return E_INVALIDARG;
    }
    Device* dev = c.device();
    // A copy into (or out of) the memory recorded but not submitted reaches the GPU first.
    if (r->gpuWritePending)
    {
        c.Stat(Device::StatMapSubmits);
        c.Submit();
    }
    if (r->lastGpuWrite && !dev->timeline.IsComplete(dev->vk, r->lastGpuWrite))
    {
        if (flags & D3D11_MAP_FLAG_DO_NOT_WAIT)
            return DXGI_ERROR_WAS_STILL_DRAWING;
        c.Stat(Device::StatMapWaits);
        dev->timeline.Wait(dev->vk, r->lastGpuWrite);
    }
    const Allocation& memory = r->IsBuffer() ? r->memory : r->linearMemory;
    if (!memory.cpu)
        return E_INVALIDARG;
    if (read && !dev->memory.IsCoherent(memory.type))
    {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory.memory;
        range.offset = memory.offset & ~VkDeviceSize(dev->limits.nonCoherentAtomSize - 1);
        range.size = VK_WHOLE_SIZE;
        g_vk.vkInvalidateMappedMemoryRanges(dev->vk, 1, &range);
    }
    if (r->IsBuffer())
    {
        FillMapped(mapped, memory.cpu, r->byteWidth, r->byteWidth);
        return S_OK;
    }
    if (sub >= r->footprints.size())
        return E_INVALIDARG;
    const Resource::Footprint& f = r->footprints[sub];
    FillMapped(mapped, memory.cpu + f.offset, f.rowPitch, f.depthPitch);
    return S_OK;
}
} // namespace

HRESULT STDMETHODCALLTYPE Context::Map(ID3D11Resource* resource, UINT subresource, D3D11_MAP type, UINT flags,
    D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (mapped)
        *mapped = {};
    if (!resource || !mapped)
        return E_INVALIDARG;
    Stat(Device::StatMaps);
    try
    {
        Resource* r = Resource::From(resource);
        if (!r)
            return E_INVALIDARG;
        if (r->usage == D3D11_USAGE_DYNAMIC)
        {
            if (m_deferred && type != D3D11_MAP_WRITE_DISCARD && !(r->IsBuffer() && type == D3D11_MAP_WRITE_NO_OVERWRITE))
                return E_INVALIDARG;
            return r->IsBuffer() ? MapDynamicBuffer(*this, r, subresource, type, mapped)
                                 : MapDynamicTexture(*this, r, subresource, type, mapped);
        }
        if (r->usage == D3D11_USAGE_STAGING)
            return MapStaging(*this, r, subresource, type, flags, mapped);
        Once("Map of a resource with usage %u refused (not DYNAMIC or STAGING)", unsigned(r->usage));
        return E_INVALIDARG;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

void STDMETHODCALLTYPE Context::Unmap(ID3D11Resource* resource, UINT subresource)
{
    try
    {
        Resource* r = Resource::From(resource);
        if (!r || r->usage != D3D11_USAGE_DYNAMIC || r->IsBuffer())
            return;
        for (size_t i = 0; i < textureMaps.size(); ++i)
        {
            TextureMap& m = textureMaps[i];
            if (m.resource != r || m.subresource != subresource)
                continue;
            const TextureMap entry = m;
            textureMaps[i] = textureMaps.back();
            textureMaps.pop_back();
            if (!Cmd())
                return;
            EndRendering();
            FlushPendingClears(nullptr);
            const SubRange range = RangeOfSub(r, entry.subresource);
            RequireImage(r, range, TransferLayout(r, true), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            FlushBarriers();
            const Resource::Footprint& f = r->footprints[entry.subresource];
            const FormatInfo& fmt = Format(r->format);
            VkBufferImageCopy region{};
            region.bufferOffset = entry.alloc.offset;
            region.bufferRowLength = f.rowPitch / (fmt.blockBytes ? fmt.blockBytes : 4) * fmt.blockWidth;
            region.bufferImageHeight = f.rows * fmt.blockWidth;
            region.imageSubresource = {r->aspect, range.firstMip,
                r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 0u : range.firstLayer, 1};
            region.imageExtent = {f.width, f.height, f.depth};
            g_vk.vkCmdCopyBufferToImage(m_cmd, entry.alloc.buffer, r->image, TransferLayout(r, true), 1, &region);
            applied.hazardsDirty = true;
            applied.computeHazardsDirty = true;
            return;
        }
    }
    catch (const std::bad_alloc&)
    {
        Once("out of memory in Unmap");
    }
}

// ================================================================================================
// Copies
// ================================================================================================

namespace
{
// Where a buffer's bytes are for a copy.
struct BufferBytes
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

BufferBytes BytesOf(Resource* r)
{
    return {r->BufferHandle(), r->BufferBase()};
}

// Copies go outside a rendering, after the clears they may depend on.
bool PrepareTransfer(Context& c)
{
    if (c.device()->lost || !c.Cmd())
        return false;
    c.EndRendering();
    c.FlushPendingClears(nullptr);
    return true;
}

void NoteWrite(Context& c)
{
    c.applied.hazardsDirty = true;
    c.applied.computeHazardsDirty = true;
}

void CopyBufferBytes(Context& c, Resource* d, VkDeviceSize dstOffset, Resource* s, VkDeviceSize srcOffset, VkDeviceSize size)
{
    if (!size)
        return;
    if (d->Versioned())
    {
        // A copy into a dynamic buffer is a GPU write D3D11 forbids; renamed constant buffers take it on the CPU.
        Once("~ a GPU copy into a dynamic or constant buffer was skipped");
        return;
    }
    const BufferBytes src = BytesOf(s);
    const BufferBytes dst = BytesOf(d);
    if (!src.buffer || !dst.buffer)
        return;
    c.RequireBuffer(s, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    d->gpuWritten = true;
    c.RequireBuffer(d, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    c.FlushBarriers();
    const VkBufferCopy region{src.offset + srcOffset, dst.offset + dstOffset, size};
    g_vk.vkCmdCopyBuffer(c.Cmd(), src.buffer, dst.buffer, 1, &region);
    if (d->usage == D3D11_USAGE_STAGING && !d->gpuWritePending)
    {
        d->gpuWritePending = true;
        AddRefResource(d);
        c.pendingReadback.push_back(d);
    }
    NoteWrite(c);
}

// Extent of a mip, in texels.
VkExtent3D MipExtent(const Resource* r, u32 mip)
{
    return {(std::max)(1u, r->width >> mip), (std::max)(1u, r->height >> mip), (std::max)(1u, r->depth >> mip)};
}

// One subresource region between images, staging footprints or both.
void CopyRegion(Context& c, Resource* d, u32 dstSub, u32 dx, u32 dy, u32 dz, Resource* s, u32 srcSub, const D3D11_BOX* box)
{
    const u32 srcMip = srcSub % s->mipLevels, srcLayer = srcSub / s->mipLevels;
    const u32 dstMip = dstSub % d->mipLevels, dstLayer = dstSub / d->mipLevels;
    const VkExtent3D srcExtent = MipExtent(s, srcMip);
    VkOffset3D srcOffset{0, 0, 0};
    VkExtent3D extent = srcExtent;
    if (box)
    {
        if (box->right <= box->left || box->bottom <= box->top || box->back <= box->front)
            return;
        srcOffset = {i32(box->left), i32(box->top), i32(box->front)};
        extent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
    }
    const VkExtent3D dstExtent = MipExtent(d, dstMip);
    // D3D11 clips nothing: a region outside either subresource is invalid and skipped.
    if (u64(srcOffset.x) + extent.width > srcExtent.width || u64(srcOffset.y) + extent.height > srcExtent.height ||
        u64(srcOffset.z) + extent.depth > srcExtent.depth || u64(dx) + extent.width > dstExtent.width + 3 ||
        u64(dy) + extent.height > dstExtent.height + 3)
    {
        Once("~ a copy region outside its subresource was skipped");
        return;
    }
    // Block-compressed regions may end at the mip's edge with a partial block.
    extent.width = (std::min)(extent.width, dstExtent.width - (std::min)(dx, dstExtent.width));
    extent.height = (std::min)(extent.height, dstExtent.height - (std::min)(dy, dstExtent.height));
    const bool src3D = s->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    const bool dst3D = d->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    const VkCommandBuffer cmd = c.Cmd();

    const bool srcLinear = !s->image && s->linear;
    const bool dstLinear = !d->image && d->linear;
    if (srcLinear && dstLinear)
    {
        Once("~ a copy between two staging textures was skipped");
        return;
    }
    const SubRange srcRange{srcMip, 1, src3D ? 0 : srcLayer, 1};
    const SubRange dstRange{dstMip, 1, dst3D ? 0 : dstLayer, 1};
    if (!srcLinear)
        c.RequireImage(s, srcRange, Context::TransferLayout(s, false), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (!dstLinear)
        c.RequireImage(d, dstRange, Context::TransferLayout(d, true), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    c.FlushBarriers();

    auto footprintRegion = [&](Resource* staged, u32 sub, VkImageAspectFlags aspect, u32 mip, u32 layer, bool is3D,
                               VkOffset3D imageOffset, VkOffset3D bufferTexel) {
        const Resource::Footprint& f = staged->footprints[sub];
        const FormatInfo& fmt = Format(staged->format);
        const u32 blockBytes = fmt.blockBytes ? fmt.blockBytes : 4;
        VkBufferImageCopy region{};
        region.bufferOffset = f.offset + VkDeviceSize(bufferTexel.z) * f.depthPitch +
            VkDeviceSize(bufferTexel.y / fmt.blockWidth) * f.rowPitch + VkDeviceSize(bufferTexel.x / fmt.blockWidth) * blockBytes;
        region.bufferRowLength = f.rowPitch / blockBytes * fmt.blockWidth;
        region.bufferImageHeight = f.rows * fmt.blockWidth;
        region.imageSubresource = {aspect, mip, is3D ? 0u : layer, 1};
        region.imageOffset = imageOffset;
        region.imageExtent = extent;
        return region;
    };
    if (dstLinear)
    {
        // Image -> staging footprint (readback).
        VkImageAspectFlags aspect = s->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : s->aspect;
        const VkBufferImageCopy region = footprintRegion(d, dstSub, aspect, srcMip, srcLayer, src3D, srcOffset,
            {i32(dx), i32(dy), i32(dz)});
        g_vk.vkCmdCopyImageToBuffer(cmd, s->image, Context::TransferLayout(s, false), d->linear, 1, &region);
        if (!d->gpuWritePending)
        {
            d->gpuWritePending = true;
            AddRefResource(d);
            c.pendingReadback.push_back(d);
        }
    }
    else if (srcLinear)
    {
        VkImageAspectFlags aspect = d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : d->aspect;
        const VkBufferImageCopy region = footprintRegion(s, srcSub, aspect, dstMip, dstLayer, dst3D,
            {i32(dx), i32(dy), i32(dz)}, srcOffset);
        g_vk.vkCmdCopyBufferToImage(cmd, s->linear, d->image, Context::TransferLayout(d, true), 1, &region);
    }
    else if ((s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) != (d->aspect & VK_IMAGE_ASPECT_COLOR_BIT))
    {
        // Depth <-> colour: Vulkan copies images of one kind only; through a buffer then.
        const FormatInfo& fmt = Format(s->format);
        const u32 texel = fmt.blockBytes ? fmt.blockBytes : 4;
        const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * extent.depth * texel;
        const UploadAlloc scratch = c.upload.Allocate(bytes, 16, c.device()->frame.load(std::memory_order_acquire));
        if (!scratch.buffer)
            return;
        VkBufferImageCopy region{};
        region.bufferOffset = scratch.offset;
        region.imageSubresource = {s->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : s->aspect,
            srcMip, src3D ? 0u : srcLayer, 1};
        region.imageOffset = srcOffset;
        region.imageExtent = extent;
        g_vk.vkCmdCopyImageToBuffer(cmd, s->image, Context::TransferLayout(s, false), scratch.buffer, 1, &region);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        g_vk.vkCmdPipelineBarrier2(cmd, &dep);
        region.imageSubresource = {d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : d->aspect,
            dstMip, dst3D ? 0u : dstLayer, 1};
        region.imageOffset = {i32(dx), i32(dy), i32(dz)};
        g_vk.vkCmdCopyBufferToImage(cmd, scratch.buffer, d->image, Context::TransferLayout(d, true), 1, &region);
    }
    else
    {
        VkImageCopy region{};
        region.srcSubresource = {s->aspect, srcMip, src3D ? 0u : srcLayer, 1};
        region.srcOffset = srcOffset;
        region.dstSubresource = {d->aspect, dstMip, dst3D ? 0u : dstLayer, 1};
        region.dstOffset = {i32(dx), i32(dy), i32(dz)};
        region.extent = extent;
        // A 2D slice of a 3D image is one depth layer of the other.
        if (src3D != dst3D)
            region.extent.depth = 1;
        g_vk.vkCmdCopyImage(cmd, s->image, Context::TransferLayout(s, false), d->image, Context::TransferLayout(d, true), 1,
            &region);
    }
    NoteWrite(c);
}
} // namespace

void STDMETHODCALLTYPE Context::CopyResource(ID3D11Resource* dst, ID3D11Resource* src)
{
    if (!dst || !src || dst == src)
        return;
    Resource* d = Resource::From(dst);
    Resource* s = Resource::From(src);
    if (!d || !s || d->dimension != s->dimension || !PrepareTransfer(*this))
        return;
    if (d->IsBuffer())
    {
        CopyBufferBytes(*this, d, 0, s, 0, (std::min)(d->byteWidth, s->byteWidth));
        return;
    }
    if (d->mipLevels != s->mipLevels || d->arraySize != s->arraySize || d->samples != s->samples)
    {
        Once("~ CopyResource between textures of different shape was skipped");
        return;
    }
    for (u32 sub = 0; sub < SubCountOf(d); ++sub)
        CopyRegion(*this, d, sub, 0, 0, 0, s, sub, nullptr);
}

void STDMETHODCALLTYPE Context::CopySubresourceRegion(ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
    ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
{
    if (!dst || !src)
        return;
    Resource* d = Resource::From(dst);
    Resource* s = Resource::From(src);
    if (!d || !s || d->IsBuffer() != s->IsBuffer() || !PrepareTransfer(*this))
        return;
    if (d->IsBuffer())
    {
        const UINT from = box ? box->left : 0;
        const UINT to = box ? box->right : UINT(s->byteWidth);
        if (to <= from || to > s->byteWidth || u64(x) + (to - from) > d->byteWidth)
            return;
        CopyBufferBytes(*this, d, x, s, from, to - from);
        return;
    }
    if (dstSub >= SubCountOf(d) || srcSub >= SubCountOf(s))
        return;
    CopyRegion(*this, d, dstSub, x, y, z, s, srcSub, box);
}

void STDMETHODCALLTYPE Context::UpdateSubresource(ID3D11Resource* dst, UINT dstSub, const D3D11_BOX* box, const void* data,
    UINT rowPitch, UINT depthPitch)
{
    if (!dst || !data)
        return;
    Resource* d = Resource::From(dst);
    if (!d || d->usage == D3D11_USAGE_IMMUTABLE)
        return;
    try
    {
        if (d->IsBuffer())
        {
            const UINT from = box ? box->left : 0;
            const UINT to = box ? box->right : UINT(d->byteWidth);
            if (to <= from || to > d->byteWidth)
                return;
            if (d->renamed)
            {
                // A new version with the shadow updated: no GPU copy, no split rendering.
                std::memcpy(d->shadow.data() + from, data, to - from);
                UploadAlloc alloc{};
                if (!NewVersion(*this, d, alloc))
                    return;
                std::memcpy(alloc.cpu, d->shadow.data(), d->byteWidth);
                return;
            }
            if (d->usage == D3D11_USAGE_DYNAMIC || !PrepareTransfer(*this))
                return;
            d->gpuWritten = true;
            RequireBuffer(d, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            FlushBarriers();
            const BufferBytes bytes = BytesOf(d);
            const VkDeviceSize size = to - from;
            if (size <= 65536 && !(from & 3) && !(size & 3))
                g_vk.vkCmdUpdateBuffer(m_cmd, bytes.buffer, bytes.offset + from, size, data);
            else
            {
                const UploadAlloc staging = upload.Allocate(size, 16, m_device->frame.load(std::memory_order_acquire));
                if (!staging.cpu)
                    return;
                std::memcpy(staging.cpu, data, size_t(size));
                const VkBufferCopy region{staging.offset, bytes.offset + from, size};
                g_vk.vkCmdCopyBuffer(m_cmd, staging.buffer, bytes.buffer, 1, &region);
            }
            NoteWrite(*this);
            return;
        }
        if (!d->image || dstSub >= SubCountOf(d) || !PrepareTransfer(*this))
            return;
        const u32 mip = dstSub % d->mipLevels, layer = dstSub / d->mipLevels;
        const VkExtent3D mipExtent = MipExtent(d, mip);
        VkOffset3D offset{0, 0, 0};
        VkExtent3D extent = mipExtent;
        if (box)
        {
            if (box->right <= box->left || box->bottom <= box->top || box->back <= box->front)
                return;
            offset = {i32(box->left), i32(box->top), i32(box->front)};
            extent = {box->right - box->left, box->bottom - box->top, box->back - box->front};
        }
        const FormatInfo& fmt = Format(d->format);
        const u32 blockBytes = fmt.blockBytes ? fmt.blockBytes : 4;
        const u32 blocksX = (extent.width + fmt.blockWidth - 1) / fmt.blockWidth;
        const u32 blocksY = (extent.height + fmt.blockWidth - 1) / fmt.blockWidth;
        const VkDeviceSize row = VkDeviceSize(blocksX) * blockBytes;
        const VkDeviceSize slice = row * blocksY;
        const UploadAlloc staging = upload.Allocate(slice * extent.depth, std::lcm(VkDeviceSize(blockBytes), VkDeviceSize(16)),
            m_device->frame.load(std::memory_order_acquire));
        if (!staging.cpu)
            return;
        const u8* src = static_cast<const u8*>(data);
        for (u32 zz = 0; zz < extent.depth; ++zz)
        {
            for (u32 yy = 0; yy < blocksY; ++yy)
                std::memcpy(staging.cpu + zz * slice + yy * row, src + VkDeviceSize(zz) * depthPitch + VkDeviceSize(yy) * rowPitch,
                    size_t(row));
        }
        const bool is3D = d->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
        RequireImage(d, SubRange{mip, 1, is3D ? 0u : layer, 1}, TransferLayout(d, true), VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        FlushBarriers();
        VkBufferImageCopy region{};
        region.bufferOffset = staging.offset;
        region.imageSubresource = {d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : d->aspect,
            mip, is3D ? 0u : layer, 1};
        region.imageOffset = offset;
        region.imageExtent = {(std::min)(extent.width, mipExtent.width - u32(offset.x)),
            (std::min)(extent.height, mipExtent.height - u32(offset.y)), extent.depth};
        g_vk.vkCmdCopyBufferToImage(m_cmd, staging.buffer, d->image, TransferLayout(d, true), 1, &region);
        NoteWrite(*this);
    }
    catch (const std::bad_alloc&)
    {
        Once("out of memory in UpdateSubresource");
    }
}

void STDMETHODCALLTYPE Context::CopyStructureCount(ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*)
{
    Once("CopyStructureCount is not supported (append/counter UAVs are not either)");
}

// ================================================================================================
// Clears
// ================================================================================================

void STDMETHODCALLTYPE Context::ClearRenderTargetView(ID3D11RenderTargetView* rtv, const FLOAT color[4])
{
    if (!rtv || !color || !Cmd())
        return;
    View* v = AsView(rtv);
    Resource* r = v->resource;
    const VkFormat format = v->viewInfo.format ? v->viewInfo.format : r->imageInfo.format;
    const VkClearValue value{.color = ColorValue(format, color)};
    // Inside a rendering of this view: cleared in place.
    if (m_rendering)
    {
        for (u32 i = 0; i < numRTVs; ++i)
        {
            if (rtvs[i] != rtv)
                continue;
            VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, i, value};
            const VkClearRect rect{{{0, 0}, {m_renderingKey.width, m_renderingKey.height}}, 0, m_renderingKey.layers};
            g_vk.vkCmdClearAttachments(m_cmd, 1, &a, 1, &rect);
            return;
        }
    }
    // Otherwise it waits to become the load operation of the next rendering of the view.
    for (PendingClear& pc : pendingClears)
    {
        if (pc.view == v && pc.color)
        {
            pc.value = value;
            return;
        }
    }
    try
    {
        PendingClear pc;
        pc.view = v;
        pc.iface = rtv;
        pc.color = true;
        pc.value = value;
        pc.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        rtv->AddRef();
        pendingClears.push_back(pc);
    }
    catch (...)
    {
        rtv->Release();
    }
}

void STDMETHODCALLTYPE Context::ClearDepthStencilView(ID3D11DepthStencilView* dsvIn, UINT flags, FLOAT depthValue, UINT8 stencil)
{
    if (!dsvIn || !Cmd())
        return;
    View* v = AsView(dsvIn);
    Resource* r = v->resource;
    VkImageAspectFlags aspects = 0;
    if ((flags & D3D11_CLEAR_DEPTH) && (r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & D3D11_CLEAR_STENCIL) && (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspects)
        return;
    VkClearValue value{};
    value.depthStencil = {std::clamp(depthValue, 0.f, 1.f), stencil};
    if (m_rendering && dsv == dsvIn)
    {
        VkClearAttachment a{aspects, 0, value};
        const VkClearRect rect{{{0, 0}, {m_renderingKey.width, m_renderingKey.height}}, 0, m_renderingKey.layers};
        g_vk.vkCmdClearAttachments(m_cmd, 1, &a, 1, &rect);
        return;
    }
    for (PendingClear& pc : pendingClears)
    {
        if (pc.view != v || pc.color)
            continue;
        // A second clear of the same view merges (depth of one, stencil of the other).
        if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
            pc.value.depthStencil.depth = value.depthStencil.depth;
        if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
            pc.value.depthStencil.stencil = value.depthStencil.stencil;
        pc.aspects |= aspects;
        return;
    }
    try
    {
        PendingClear pc;
        pc.view = v;
        pc.iface = dsvIn;
        pc.color = false;
        pc.value = value;
        pc.aspects = aspects;
        dsvIn->AddRef();
        pendingClears.push_back(pc);
    }
    catch (...)
    {
        dsvIn->Release();
    }
}

namespace
{
void ClearUav(Context& c, ID3D11UnorderedAccessView* uav, const UINT* uints, const FLOAT* floats)
{
    if (!uav || !PrepareTransfer(c))
        return;
    View* v = AsView(uav);
    Resource* r = v->resource;
    const VkCommandBuffer cmd = c.Cmd();
    if (r->IsBuffer())
    {
        // Buffers fill with a 32-bit pattern: the first value (raw, structured and R32 views).
        const u32 bytes = v->structured ? 4 : Format(v->format).blockBytes;
        if (bytes != 4)
        {
            Once("~ a UAV clear of a non-32-bit buffer view was skipped");
            return;
        }
        r->gpuWritten = true;
        c.RequireBuffer(r, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        c.FlushBarriers();
        const u32 pattern = uints ? uints[0] : std::bit_cast<u32>(floats[0]);
        g_vk.vkCmdFillBuffer(cmd, r->BufferHandle(), r->BufferBase() + v->bufferOffset, v->bufferRange, pattern);
        NoteWrite(c);
        return;
    }
    c.RequireImage(r, v->range, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    c.FlushBarriers();
    const VkFormat format = v->viewInfo.format;
    VkClearColorValue value{};
    if (uints)
    {
        if (IsIntegerFormat(format))
            std::memcpy(value.uint32, uints, sizeof(u32) * 4);
        else
        {
            // D3D11 writes the low bits of each value: for normalized formats the largest code is 1.0.
            for (u32 i = 0; i < 4; ++i)
                value.float32[i] = float(uints[i] & 0xFF) / 255.f;
        }
    }
    else
        std::memcpy(value.float32, floats, sizeof(float) * 4);
    const VkImageSubresourceRange range = RangeOf(r, v->range, VK_IMAGE_ASPECT_COLOR_BIT);
    g_vk.vkCmdClearColorImage(cmd, r->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    NoteWrite(c);
}
} // namespace

void STDMETHODCALLTYPE Context::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* uav, const UINT values[4])
{
    if (values)
        ClearUav(*this, uav, values, nullptr);
}

void STDMETHODCALLTYPE Context::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* uav, const FLOAT values[4])
{
    if (values)
        ClearUav(*this, uav, nullptr, values);
}

// ================================================================================================
// Mips, resolves
// ================================================================================================

void STDMETHODCALLTYPE Context::GenerateMips(ID3D11ShaderResourceView* srv)
{
    if (!srv || !PrepareTransfer(*this))
        return;
    View* v = AsView(srv);
    Resource* r = v->resource;
    if (!r->image || v->range.mipCount < 2 || !(r->miscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS))
        return;
    for (u32 mip = v->range.firstMip + 1; mip < v->range.firstMip + v->range.mipCount; ++mip)
    {
        const SubRange srcRange{mip - 1, 1, v->range.firstLayer, v->range.layerCount};
        const SubRange dstRange{mip, 1, v->range.firstLayer, v->range.layerCount};
        RequireImage(r, srcRange, TransferLayout(r, false), VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        RequireImage(r, dstRange, TransferLayout(r, true), VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        FlushBarriers();
        const VkExtent3D from = MipExtent(r, mip - 1), to = MipExtent(r, mip);
        VkImageBlit blit{};
        blit.srcSubresource = {r->aspect, mip - 1, v->range.firstLayer, v->range.layerCount};
        blit.srcOffsets[1] = {i32(from.width), i32(from.height), i32(from.depth)};
        blit.dstSubresource = {r->aspect, mip, v->range.firstLayer, v->range.layerCount};
        blit.dstOffsets[1] = {i32(to.width), i32(to.height), i32(to.depth)};
        g_vk.vkCmdBlitImage(m_cmd, r->image, TransferLayout(r, false), r->image, TransferLayout(r, true), 1, &blit,
            VK_FILTER_LINEAR);
    }
    NoteWrite(*this);
}

void STDMETHODCALLTYPE Context::ResolveSubresource(ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT)
{
    if (!dst || !src || !PrepareTransfer(*this))
        return;
    Resource* d = Resource::From(dst);
    Resource* s = Resource::From(src);
    if (!d || !s || !d->image || !s->image || s->samples < 2 || d->samples != 1)
        return;
    if (d->imageInfo.format != s->imageInfo.format)
    {
        Once("~ a resolve between images of different formats was skipped");
        return;
    }
    const SubRange srcRange = RangeOfSub(s, srcSub);
    const SubRange dstRange = RangeOfSub(d, dstSub);
    RequireImage(s, srcRange, TransferLayout(s, false), VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    RequireImage(d, dstRange, TransferLayout(d, true), VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    FlushBarriers();
    VkImageResolve region{};
    region.srcSubresource = {s->aspect, srcRange.firstMip, srcRange.firstLayer, 1};
    region.dstSubresource = {d->aspect, dstRange.firstMip, dstRange.firstLayer, 1};
    region.extent = MipExtent(d, dstRange.firstMip);
    g_vk.vkCmdResolveImage(m_cmd, s->image, TransferLayout(s, false), d->image, TransferLayout(d, true), 1, &region);
    NoteWrite(*this);
}

// ================================================================================================
// Queries
// ================================================================================================

void STDMETHODCALLTYPE Context::Begin(ID3D11Asynchronous* async)
{
    if (!async)
        return;
    if (m_deferred)
    {
        Once("queries on deferred contexts are not supported; Begin ignored");
        return;
    }
    Query* q = AsQuery(async);
    const D3D11_QUERY type = q->m_desc.Query;
    if (IsOcclusion(type))
    {
        if (q->active)
            return;
        // The previous run's slots may still be written by the GPU: fresh ones.
        q->FreeSlots();
        q->slot = m_device->queryPools.Allocate(VK_QUERY_TYPE_OCCLUSION);
        if (!q->slot.pool)
            return;
        try
        {
            activeQueries.push_back(q);
        }
        catch (...)
        {
            return;
        }
        q->AddRef();
        q->active = true;
        q->begun = false;
        q->slotUsed = false;
        q->value = ~0ull;
        // Begun in Vulkan inside a rendering (a query must end in the rendering it began in).
        if (m_rendering)
            ResumeQueries();
    }
    else if (type == D3D11_QUERY_TIMESTAMP_DISJOINT)
    {
        q->active = true;
        q->value = ~0ull;
    }
}

void STDMETHODCALLTYPE Context::End(ID3D11Asynchronous* async)
{
    if (!async)
        return;
    if (m_deferred)
    {
        Once("queries on deferred contexts are not supported; End ignored");
        return;
    }
    Query* q = AsQuery(async);
    const D3D11_QUERY type = q->m_desc.Query;
    if (IsOcclusion(type))
    {
        if (!q->active)
            return;
        if (q->begun && Cmd())
        {
            g_vk.vkCmdEndQuery(m_cmd, q->slot.pool, q->slot.index);
            q->begun = false;
        }
        const auto it = std::find(activeQueries.begin(), activeQueries.end(), q);
        if (it != activeQueries.end())
        {
            activeQueries.erase(it);
            q->Release();
        }
    }
    else if (type == D3D11_QUERY_TIMESTAMP)
    {
        if (!Cmd())
            return;
        q->FreeSlots();
        q->slot = m_device->queryPools.Allocate(VK_QUERY_TYPE_TIMESTAMP);
        if (!q->slot.pool)
            return;
        g_vk.vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->slot.pool, q->slot.index);
        q->slotUsed = true;
    }
    else if (type != D3D11_QUERY_EVENT && type != D3D11_QUERY_TIMESTAMP_DISJOINT)
        Once("query type %u records nothing; its data reads as zeros", unsigned(type));
    q->active = false;
    try
    {
        endedQueries.push_back(q);
        q->AddRef();
        q->value = 0;
    }
    catch (...)
    {
        q->value = m_device->timeline.NextValue();
    }
    // An event marks this point of the frame: the engine's CPU/GPU throttle waits for the one it ended a
    // frame earlier. Its own submission keeps a frame of overlap (as on DX12).
    if (type == D3D11_QUERY_EVENT)
        Submit();
}

HRESULT STDMETHODCALLTYPE Context::GetData(ID3D11Asynchronous* async, void* data, UINT size, UINT flags)
{
    if (!async)
        return E_INVALIDARG;
    if (m_deferred)
        return DXGI_ERROR_INVALID_CALL;
    Query* q = AsQuery(async);
    const UINT expected = q->GetDataSize();
    if (data ? size != expected : size != 0)
        return E_INVALIDARG;
    if (m_device->lost)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (q->value == ~0ull || q->active)
        return DXGI_ERROR_INVALID_CALL;
    if (!q->value)
    {
        if (flags & D3D11_ASYNC_GETDATA_DONOTFLUSH)
            return S_FALSE;
        Stat(Device::StatQuerySubmits);
        Submit();
        if (!q->value || q->value == ~0ull)
        {
            Stat(Device::StatQueryNotReady);
            return S_FALSE;
        }
    }
    // Occlusion and timestamp results are ready once the GPU has passed the query itself (as D3D11 drivers report
    // them), not when its whole submission ends: the engine polls the last frame's light queries early in the next
    // frame, and a wait for that frame's end would run the CPU and the GPU in turn.
    auto written = [&] {
        const D3D11_QUERY type = q->m_desc.Query;
        if (!IsOcclusion(type) && type != D3D11_QUERY_TIMESTAMP)
            return false;
        auto done = [&](const QueryPoolSet::Slot& slot) {
            u64 result[2] = {};
            g_vk.vkGetQueryPoolResults(m_device->vk, slot.pool, slot.index, 1, sizeof(result), result, sizeof(result),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            return result[1] != 0;
        };
        if (q->slotUsed && q->slot.pool && !done(q->slot))
            return false;
        for (const QueryPoolSet::Slot& part : q->parts)
        {
            if (part.pool && !done(part))
                return false;
        }
        return true;
    };
    if (!m_device->timeline.IsComplete(m_device->vk, q->value) && !written())
    {
        Stat(Device::StatQueryNotReady);
        return S_FALSE;
    }
    if (!data)
        return S_OK;

    u64 result = 0;
    auto read = [&](const QueryPoolSet::Slot& slot) {
        u64 value = 0;
        if (slot.pool &&
            g_vk.vkGetQueryPoolResults(m_device->vk, slot.pool, slot.index, 1, sizeof(value), &value, sizeof(value),
                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            result += value;
    };
    if (q->slotUsed)
        read(q->slot);
    for (const QueryPoolSet::Slot& part : q->parts)
        read(part);
    switch (q->m_desc.Query)
    {
    case D3D11_QUERY_OCCLUSION: std::memcpy(data, &result, sizeof(result)); break;
    case D3D11_QUERY_TIMESTAMP:
    {
        // Ticks of timestampPeriod ns; the disjoint query reports the matching frequency.
        std::memcpy(data, &result, sizeof(result));
        break;
    }
    case D3D11_QUERY_OCCLUSION_PREDICATE:
    {
        const BOOL visible = result != 0;
        std::memcpy(data, &visible, sizeof(visible));
        break;
    }
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        const double period = m_device->limits.timestampPeriod > 0.f ? double(m_device->limits.timestampPeriod) : 1.0;
        disjoint.Frequency = u64(1e9 / period);
        disjoint.Disjoint = FALSE;
        std::memcpy(data, &disjoint, sizeof(disjoint));
        break;
    }
    case D3D11_QUERY_EVENT:
    {
        const BOOL done = TRUE;
        std::memcpy(data, &done, sizeof(done));
        break;
    }
    default: std::memset(data, 0, size); break;
    }
    return S_OK;
}

// ================================================================================================
// Command lists, ClearState, Flush
// ================================================================================================

void STDMETHODCALLTYPE Context::ExecuteCommandList(ID3D11CommandList* list, BOOL restoreState)
{
    if (!list)
        return;
    if (m_deferred)
    {
        Once("ExecuteCommandList on a deferred context ignored");
        return;
    }
    try
    {
        auto* cl = static_cast<CommandList*>(list);
        if (cl->buffers.empty())
            Once("~ a command list was executed twice; the repeat is skipped");
        else if (Cmd())
        {
            EndRendering();
            FlushPendingClears(nullptr);
            // The list's entry requirements: one barrier batch at the end of the immediate buffer.
            for (const CommandList::Requirement& req : cl->requirements)
            {
                Resource* r = req.resource;
                if (r->IsBuffer())
                    RequireBuffer(r, req.stages, req.access);
                else if (req.subresource == ~0u)
                    RequireImageAll(r, req.layout, req.stages, req.access);
                else
                    RequireImage(r, RangeOfSub(r, req.subresource), req.layout, req.stages, req.access);
            }
            FlushBarriers();
            {
                std::lock_guard lock(m_queuedLock);
                if (const VkCommandBuffer own = CloseOwn())
                    m_queued.push_back({own, this});
                for (VkCommandBuffer b : cl->buffers)
                {
                    m_queued.push_back({b, cl->recordedBy});
                    if (cl->recordedBy)
                        cl->recordedBy->AddRef();
                }
            }
            cl->buffers.clear();
            // What the list left behind is what the immediate context tracks from now on.
            for (const CommandList::Requirement& req : cl->requirements)
            {
                if (!req.written)
                    continue;
                Resource* r = req.resource;
                if (r->IsBuffer())
                    r->bufferState = req.last;
                else if (req.subresource == ~0u)
                {
                    r->states.assign(1, req.last);
                    r->uniform = true;
                }
                else
                {
                    const u32 count = SubCountOf(r);
                    if (r->uniform)
                    {
                        const ImageState s = r->states.empty() ? ImageState{} : r->states[0];
                        r->states.assign(count, s);
                        r->uniform = false;
                    }
                    if (req.subresource < r->states.size())
                        r->states[req.subresource] = req.last;
                }
            }
            for (Resource* r : cl->writtenBuffers)
                r->gpuWritten = true;
            for (Resource* r : cl->pendingReadback)
                r->gpuWritePending = true;
            pendingReadback.insert(pendingReadback.end(), cl->pendingReadback.begin(), cl->pendingReadback.end());
            cl->pendingReadback.clear();
            applied.hazardsDirty = true;
            applied.computeHazardsDirty = true;
        }
    }
    catch (const std::bad_alloc&)
    {
        Once("out of memory in ExecuteCommandList");
    }
    if (!restoreState)
        ClearState();
}

HRESULT STDMETHODCALLTYPE Context::FinishCommandList(BOOL restoreState, ID3D11CommandList** out)
{
    if (!out)
        return E_INVALIDARG;
    *out = nullptr;
    if (!m_deferred)
        return DXGI_ERROR_INVALID_CALL;
    try
    {
        if (!Cmd())
            return m_device->lost ? DXGI_ERROR_DEVICE_REMOVED : E_OUTOFMEMORY;
        FlushPendingClears(nullptr);
        auto* cl = new CommandList(m_device);
        const VkCommandBuffer cmd = CloseOwn();
        if (!cmd)
        {
            cl->Release();
            if (!restoreState)
                ClearState();
            return m_device->lost ? DXGI_ERROR_DEVICE_REMOVED : E_FAIL;
        }
        cl->buffers.push_back(cmd);
        tracker.Harvest(cl->requirements);
        cl->pendingReadback.swap(pendingReadback);
        cl->writtenBuffers.swap(writtenBuffers);
        cl->recordedBy = this;
        AddRef();
        if (!restoreState)
            ClearState();
        *out = cl;
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

void STDMETHODCALLTYPE Context::ClearState()
{
    ReleaseBindings();
    // The open command buffer keeps what it has bound; the next draw applies everything again.
    const bool rendering = m_rendering;
    const RenderingKey key = m_renderingKey;
    InvalidateApplied();
    m_rendering = rendering;
    m_renderingKey = key;
    m_targetsDirty = true;
}

void STDMETHODCALLTYPE Context::Flush()
{
    if (!m_deferred)
        Submit();
}
} // namespace xrvk
