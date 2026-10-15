// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Context, second half: command list lifecycle, resource state tracking and barriers, dynamic buffer
// versions, Map/Unmap, copies, clears, resolves, queries, ClearState/Flush and the command lists of
// deferred contexts. The binding/draw half lives in d12_context_state.cpp.
#include "d12_layer.h"

#include <d3d11_1.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <new>

namespace xrd3d12
{
// ================================================================================================
// Annotations
// ================================================================================================

// The context's ID3DUserDefinedAnnotation; its reference count is the context's. Events become PIX
// markers of the D3D12 lists. D3D11 scopes may span submissions, so the open ones are ended before a
// list closes and begun again on the next list: every list carries balanced markers.
class Context::Annotation final : public ID3DUserDefinedAnnotation
{
public:
    explicit Annotation(Context* owner) : m_owner(owner), m_on(owner->device()->markers) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_POINTER;
        if (riid == __uuidof(ID3DUserDefinedAnnotation))
        {
            *object = static_cast<ID3DUserDefinedAnnotation*>(this);
            AddRef();
            return S_OK;
        }
        return m_owner->QueryInterface(riid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }

    INT STDMETHODCALLTYPE BeginEvent(LPCWSTR name) override
    {
        if (m_on && m_depth < kMaxDepth)
        {
            // Opening a list begins the scopes open so far; this one follows.
            ID3D12GraphicsCommandList* list = m_owner->List();
            Store(m_depth, name);
            if (list)
                Begin(list, m_names[m_depth]);
        }
        ++m_depth;
        // D3D11 answers -1 unless a profiling tool captures (GetStatus); the engine ignores it.
        return -1;
    }
    INT STDMETHODCALLTYPE EndEvent() override
    {
        if (!m_depth)
            return -1;
        --m_depth;
        if (m_on && m_depth < kMaxDepth && m_owner->m_list)
            m_owner->m_list->EndEvent();
        return -1;
    }
    void STDMETHODCALLTYPE SetMarker(LPCWSTR name) override
    {
        if (!m_on)
            return;
        wchar_t text[kMaxName];
        Copy(text, name);
        if (ID3D12GraphicsCommandList* list = m_owner->List())
            list->SetMarker(0, text, UINT((std::wcslen(text) + 1) * sizeof(wchar_t)));
    }
    BOOL STDMETHODCALLTYPE GetStatus() override { return FALSE; }

    // `list` is about to close: its open scopes end with it.
    void Suspend(ID3D12GraphicsCommandList* list)
    {
        for (u32 i = m_on ? (std::min)(m_depth, kMaxDepth) : 0; i-- > 0;)
            list->EndEvent();
    }
    // A new list opened: the scopes still open continue in it.
    void Resume(ID3D12GraphicsCommandList* list)
    {
        for (u32 i = 0; m_on && i < (std::min)(m_depth, kMaxDepth); ++i)
            Begin(list, m_names[i]);
    }

private:
    static constexpr u32 kMaxDepth = 16;
    static constexpr u32 kMaxName = 64;

    static void Copy(wchar_t (&out)[kMaxName], LPCWSTR name)
    {
        size_t length = 0;
        if (name)
        {
            while (length + 1 < kMaxName && name[length])
            {
                out[length] = name[length];
                ++length;
            }
        }
        out[length] = 0;
    }
    void Store(u32 depth, LPCWSTR name) { Copy(m_names[depth], name); }
    static void Begin(ID3D12GraphicsCommandList* list, const wchar_t* name)
    {
        // Metadata 0: a UTF-16 string, the format PIX, RenderDoc and Nsight read.
        list->BeginEvent(0, name, UINT((std::wcslen(name) + 1) * sizeof(wchar_t)));
    }

    Context* m_owner;
    bool m_on;
    u32 m_depth = 0;
    wchar_t m_names[kMaxDepth][kMaxName]{};
};

HRESULT STDMETHODCALLTYPE Context::QueryInterface(REFIID riid, void** object)
{
    if (object && annotation && riid == __uuidof(ID3DUserDefinedAnnotation))
    {
        *object = static_cast<ID3DUserDefinedAnnotation*>(annotation);
        AddRef();
        return S_OK;
    }
    return DeviceChild::QueryInterface(riid, object);
}

// ================================================================================================
// Dynamic buffer versions
// ================================================================================================

// Persistent upload memory for the versions of dynamic buffers, one heap per context. A version has
// to outlive its frame: constant buffers are mapped again only when their contents change, and the
// dynamic vertex/index streams append with NO_OVERWRITE across frames until they wrap. A block
// therefore lives until its buffer is renamed (or destroyed) and is reused kFramesInFlight + 1 frames
// after that, when every list that could read it has completed (the Device::EndFrame guarantee).
class DynamicHeap
{
public:
    explicit DynamicHeap(Device* device) : m_device(device) {}
    ~DynamicHeap();
    // Owner context's thread. False when no memory could be had. `retire` (a version of this heap)
    // is retired under the same lock when the allocation succeeded.
    bool Allocate(u64 size, UploadAlloc& out, const UploadAlloc* retire = nullptr);
    // Any thread. True when the heap is orphaned and now empty: the caller deletes it.
    bool Retire(const UploadAlloc& alloc);
    // The owning context is gone: its memory is released, the object lives on until the last version
    // a buffer holds is retired. True = nothing is held, delete it now.
    bool Orphan();
    // Retired versions still waiting for their frames to complete.
    u64 PendingBytes();

private:
    static constexpr u32 kClassCount = 9; // 256 B .. 64 KB; larger versions get buffers of their own
    static constexpr u64 kLargeGranularity = 64ull << 10;
    static constexpr size_t kMaxFreeLarge = 8;
    static constexpr u64 kGrowthWarning = 512ull << 20;
    struct Retired
    {
        UploadAlloc alloc;
        u64 frame;
    };
    static u32 ClassOf(u64 size);
    bool CreateBuffer(u64 size, ID3D12Resource*& resource, u8*& cpu, D3D12_GPU_VIRTUAL_ADDRESS& gpu);
    void Reclaim(u64 frame);
    void PushRetired(const UploadAlloc& alloc); // m_lock held
    void DropBuffer(ID3D12Resource* resource);

    Device* m_device;
    std::mutex m_lock;
    // FIFO ring (frames non-decreasing): no allocation once it reached the steady-state size.
    std::vector<Retired> m_retired;
    size_t m_retiredHead = 0;
    size_t m_retiredCount = 0;
    std::vector<UploadAlloc> m_free[kClassCount];
    std::vector<UploadAlloc> m_freeLarge; // `size` = capacity
    std::vector<ID3D12Resource*> m_buffers;
    ID3D12Resource* m_page = nullptr;
    u8* m_pageCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_pageGpu = 0;
    u64 m_pageUsed = 0;
    u64 m_bytes = 0;
    u64 m_pending = 0;     // bytes of m_retired not reclaimed yet
    u64 m_outstanding = 0; // blocks held by buffers
    bool m_orphaned = false;
};

namespace
{
constexpr u64 AlignUp(u64 value, u64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

D3D12_RESOURCE_DESC LinearDesc(u64 size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

bool IsRemovalCode(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// A failed D3D12 call: device removal goes to the device's report, anything else is logged once.
void ReportFailure(Device* device, HRESULT hr, const char* what)
{
    const HRESULT reason = device->d12 ? device->d12->GetDeviceRemovedReason() : S_OK;
    if (FAILED(reason))
        device->ReportRemoval(reason);
    else if (IsRemovalCode(hr))
        device->ReportRemoval(hr);
    else
        LogOnce(what, "%s failed (0x%08X)", what, unsigned(hr));
}

void ReleaseLater(Device* device, IUnknown* object)
{
    if (!object)
        return;
    try
    {
        device->releaser.Release(object);
    }
    catch (...)
    {
        // Releasing now could free memory the GPU still reads: leaking is the safe failure.
        static constexpr char kFormat[] = "context teardown: out of memory in the deferred releaser, an object leaks";
        LogOnce(kFormat, kFormat);
    }
}
} // namespace

DynamicHeap::~DynamicHeap()
{
    for (ID3D12Resource* buffer : m_buffers)
        ReleaseLater(m_device, buffer);
}

u32 DynamicHeap::ClassOf(u64 size)
{
    u32 cls = 0;
    while (cls < kClassCount && (256ull << cls) < size)
        ++cls;
    return cls;
}

bool DynamicHeap::CreateBuffer(u64 size, ID3D12Resource*& resource, u8*& cpu, D3D12_GPU_VIRTUAL_ADDRESS& gpu)
{
    const D3D12_HEAP_PROPERTIES props = HeapProperties(m_device->dynamicHeapType);
    const D3D12_RESOURCE_DESC desc = LinearDesc(size);
    ID3D12Resource* created = nullptr;
    HRESULT hr = m_device->d12->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&created));
    if (FAILED(hr))
    {
        ReportFailure(m_device, hr, "dynamic buffer memory: CreateCommittedResource");
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE nothing{0, 0};
    hr = created->Map(0, &nothing, &mapped);
    if (FAILED(hr) || !mapped)
    {
        created->Release();
        ReportFailure(m_device, FAILED(hr) ? hr : E_FAIL, "dynamic buffer memory: Map");
        return false;
    }
    try
    {
        m_buffers.push_back(created);
    }
    catch (const std::bad_alloc&)
    {
        created->Release();
        return false;
    }
    resource = created;
    cpu = static_cast<u8*>(mapped);
    gpu = created->GetGPUVirtualAddress();
    m_bytes += size;
    if (m_bytes > kGrowthWarning)
    {
        static constexpr char kFormat[] = "dynamic buffer versions of one context exceed %llu MB: frames are not ending";
        LogOnce(kFormat, kFormat, static_cast<unsigned long long>(m_bytes >> 20));
    }
    return true;
}

void DynamicHeap::DropBuffer(ID3D12Resource* resource)
{
    const auto it = std::find(m_buffers.begin(), m_buffers.end(), resource);
    if (it == m_buffers.end())
        return;
    m_bytes -= (*it)->GetDesc().Width;
    m_buffers.erase(it);
    // Only reusable (GPU-idle) blocks get here.
    resource->Release();
}

void DynamicHeap::PushRetired(const UploadAlloc& alloc)
{
    try
    {
        if (m_retiredCount == m_retired.size())
        {
            std::vector<Retired> grown((std::max)(m_retired.size() * 2, size_t(1024)));
            for (size_t i = 0; i < m_retiredCount; ++i)
                grown[i] = m_retired[(m_retiredHead + i) % m_retired.size()];
            m_retired.swap(grown);
            m_retiredHead = 0;
        }
        m_retired[(m_retiredHead + m_retiredCount) % m_retired.size()] = {alloc, m_device->frame.load(std::memory_order_acquire)};
        ++m_retiredCount;
        m_pending += alloc.size;
    }
    catch (const std::bad_alloc&)
    {
        // The block is lost; nothing else is affected.
    }
}

void DynamicHeap::Reclaim(u64 frame)
{
    while (m_retiredCount)
    {
        const Retired& entry = m_retired[m_retiredHead];
        if (entry.frame + kFramesInFlight + 1 > frame)
            break;
        m_pending -= (std::min)(m_pending, entry.alloc.size);
        const u32 cls = ClassOf(entry.alloc.size);
        if (cls < kClassCount)
            m_free[cls].push_back(entry.alloc);
        else
        {
            UploadAlloc block = entry.alloc;
            block.size = AlignUp(block.size, kLargeGranularity);
            if (m_freeLarge.size() >= kMaxFreeLarge)
            {
                DropBuffer(m_freeLarge.front().resource);
                m_freeLarge.erase(m_freeLarge.begin());
            }
            m_freeLarge.push_back(block);
        }
        m_retiredHead = (m_retiredHead + 1) % m_retired.size();
        --m_retiredCount;
    }
}

bool DynamicHeap::Allocate(u64 size, UploadAlloc& out, const UploadAlloc* retire)
{
    try
    {
        std::lock_guard lock(m_lock);
        if (m_orphaned)
            return false;
        Reclaim(m_device->frame.load(std::memory_order_acquire));
        const u64 aligned = AlignUp(size ? size : 1, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        const u32 cls = ClassOf(aligned);
        UploadAlloc block{};
        if (cls < kClassCount)
        {
            std::vector<UploadAlloc>& list = m_free[cls];
            if (!list.empty())
            {
                block = list.back();
                list.pop_back();
            }
            else
            {
                const u64 blockSize = 256ull << cls;
                if (!m_page || m_pageUsed + blockSize > kUploadPageSize)
                {
                    ID3D12Resource* page = nullptr;
                    u8* cpu = nullptr;
                    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
                    if (!CreateBuffer(kUploadPageSize, page, cpu, gpu))
                        return false;
                    m_page = page;
                    m_pageCpu = cpu;
                    m_pageGpu = gpu;
                    m_pageUsed = 0;
                }
                block.resource = m_page;
                block.offset = m_pageUsed;
                block.gpu = m_pageGpu + m_pageUsed;
                block.cpu = m_pageCpu + m_pageUsed;
                m_pageUsed += blockSize;
            }
        }
        else
        {
            const u64 capacity = AlignUp(aligned, kLargeGranularity);
            const auto it = std::find_if(
                m_freeLarge.begin(), m_freeLarge.end(), [capacity](const UploadAlloc& a) { return a.size == capacity; });
            if (it != m_freeLarge.end())
            {
                block = *it;
                m_freeLarge.erase(it);
            }
            else
            {
                ID3D12Resource* buffer = nullptr;
                u8* cpu = nullptr;
                D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
                if (!CreateBuffer(capacity, buffer, cpu, gpu))
                    return false;
                block.resource = buffer;
                block.offset = 0;
                block.gpu = gpu;
                block.cpu = cpu;
            }
        }
        block.size = aligned;
        out = block;
        ++m_outstanding;
        if (retire)
        {
            --m_outstanding;
            PushRetired(*retire);
        }
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}

bool DynamicHeap::Retire(const UploadAlloc& alloc)
{
    std::lock_guard lock(m_lock);
    if (m_outstanding)
        --m_outstanding;
    if (m_orphaned)
        return !m_outstanding;
    PushRetired(alloc);
    return false;
}

u64 DynamicHeap::PendingBytes()
{
    std::lock_guard lock(m_lock);
    return m_pending;
}

bool DynamicHeap::Orphan()
{
    std::lock_guard lock(m_lock);
    m_orphaned = true;
    // The memory goes now (after the GPU): only buffers of the dead context still name its blocks, and
    // nothing draws with them any more. The object itself stays until their versions are retired.
    for (ID3D12Resource* buffer : m_buffers)
        ReleaseLater(m_device, buffer);
    m_buffers.clear();
    for (std::vector<UploadAlloc>& list : m_free)
        list.clear();
    m_freeLarge.clear();
    m_retired.clear();
    m_retiredHead = 0;
    m_retiredCount = 0;
    m_pending = 0;
    m_page = nullptr;
    m_pageCpu = nullptr;
    m_pageUsed = 0;
    m_bytes = 0;
    return !m_outstanding;
}

void RetireDynamicVersion(Resource* resource)
{
    if (!resource)
        return;
    DynamicHeap* heap = resource->current.heap;
    if (!heap)
        return;
    resource->current.heap = nullptr;
    if (heap->Retire(resource->current.alloc))
        delete heap;
}

// ================================================================================================
// State tracking
// ================================================================================================

namespace
{
constexpr D3D12_RESOURCE_STATES kUntouched = static_cast<D3D12_RESOURCE_STATES>(0xFFFFFFFFu);
constexpr u32 kAllSubresources = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
// Deferred trackers keep their per-resource entries between lists (no allocation in the steady
// state) and start over when they collected more than this.
constexpr size_t kMaxLocalEntries = 8192;

constexpr D3D12_RESOURCE_STATES kReadStates = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
    D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE);

bool IsReadOnly(D3D12_RESOURCE_STATES s)
{
    return s != D3D12_RESOURCE_STATE_COMMON && !(u32(s) & ~u32(kReadStates));
}

// Read-only states combine: a subresource in a state holding every requested read bit is ready.
bool Satisfies(D3D12_RESOURCE_STATES current, D3D12_RESOURCE_STATES wanted)
{
    return current == wanted || (IsReadOnly(current) && IsReadOnly(wanted) && (u32(current) & u32(wanted)) == u32(wanted));
}

D3D12_RESOURCE_STATES Merge(D3D12_RESOURCE_STATES current, D3D12_RESOURCE_STATES wanted)
{
    return IsReadOnly(current) && IsReadOnly(wanted) ? static_cast<D3D12_RESOURCE_STATES>(u32(current) | u32(wanted)) : wanted;
}

u32 MipsOf(const Resource* r)
{
    return r->mipLevels ? r->mipLevels : 1;
}

u32 SlicesOf(const Resource* r)
{
    return r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D || !r->arraySize ? 1 : r->arraySize;
}

u32 PlanesOf(const Resource* r)
{
    return r->planeCount ? r->planeCount : 1;
}

u32 SubCountOf(const Resource* r)
{
    return r->IsBuffer() ? 1 : MipsOf(r) * SlicesOf(r) * PlanesOf(r);
}

// Clamped subresource box of a tracked resource.
struct Span
{
    u32 m0 = 0, m1 = 1;
    u32 s0 = 0, s1 = 1;
    u32 p0 = 0, p1 = 1;
    bool whole = true;
    bool Empty() const { return m0 >= m1 || s0 >= s1 || p0 >= p1; }
};

Span WholeSpan(const Resource* r)
{
    Span s;
    if (r->IsBuffer())
        return s;
    s.m1 = MipsOf(r);
    s.s1 = SlicesOf(r);
    s.p1 = PlanesOf(r);
    return s;
}

Span ClampSpan(const Resource* r, const SubRange& range)
{
    if (r->IsBuffer())
        return Span{};
    const auto clamp = [](u32 first, u32 count, u32 total, u32& begin, u32& end) {
        begin = (std::min)(first, total);
        end = count >= total - begin ? total : begin + count;
    };
    const u32 mips = MipsOf(r), slices = SlicesOf(r), planes = PlanesOf(r);
    Span s;
    clamp(range.firstMip, range.mipCount, mips, s.m0, s.m1);
    if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        // A 3D texture has one subresource per mip; W slices are not subresources.
        s.s0 = 0;
        s.s1 = 1;
    }
    else
        clamp(range.firstSlice, range.sliceCount, slices, s.s0, s.s1);
    clamp(range.firstPlane, range.planeCount, planes, s.p0, s.p1);
    s.whole = s.m0 == 0 && s.m1 == mips && s.s0 == 0 && s.s1 == slices && s.p0 == 0 && s.p1 == planes;
    return s;
}

u32 SubIndex(const Resource* r, u32 mip, u32 slice, u32 plane)
{
    const u32 mips = MipsOf(r);
    return mip + slice * mips + plane * mips * SlicesOf(r);
}

// A D3D12 subresource index as a one-subresource range.
SubRange RangeOfSub(const Resource* r, u32 sub)
{
    const u32 mips = MipsOf(r), slices = SlicesOf(r);
    SubRange range;
    range.firstMip = sub % mips;
    range.firstSlice = (sub / mips) % slices;
    range.firstPlane = sub / (mips * slices);
    return range;
}

// Every plane of a D3D11 subresource (depth-stencil copies and clears act on all of them).
SubRange PlanesOfSub(const Resource* r, u32 sub11)
{
    SubRange range = RangeOfSub(r, sub11);
    range.firstPlane = 0;
    range.planeCount = PlanesOf(r);
    return range;
}

// Appends a transition. A pending transition of the same subresource that ends where this one starts
// is folded into it (and dropped when the pair cancels out: no work was recorded in between).
void PushTransition(std::vector<D3D12_RESOURCE_BARRIER>& barriers, ID3D12Resource* resource, u32 sub,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    for (size_t i = barriers.size(); i-- > 0;)
    {
        D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            break;
        if (b.Transition.pResource != resource)
            continue;
        if (b.Transition.Subresource != sub || b.Transition.StateAfter != before)
            break;
        if (b.Transition.StateBefore == after)
            barriers.erase(barriers.begin() + ptrdiff_t(i));
        else
            b.Transition.StateAfter = after;
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = resource;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    barriers.push_back(b);
}

void Collapse(SubresourceStates& st)
{
    if (st.uniform || st.per.empty())
        return;
    const D3D12_RESOURCE_STATES first = st.per.front();
    for (const D3D12_RESOURCE_STATES s : st.per)
    {
        if (s != first)
            return;
    }
    st.uniform = true;
    st.all = first;
}

// Immediate tracking on Resource::states. `exact`: the subresources must end in exactly `state`
// (the first states of an executed command list); otherwise read states combine.
void RequireTracked(Resource* resource, const SubRange* range, D3D12_RESOURCE_STATES state, bool exact,
    std::vector<D3D12_RESOURCE_BARRIER>& barriers)
{
    Resource* r = resource ? resource->Actual() : nullptr;
    if (!r || !r->native)
        return;
    const Span span = range ? ClampSpan(r, *range) : WholeSpan(r);
    if (span.Empty())
        return;
    SubresourceStates& st = r->states;
    const u32 count = SubCountOf(r);
    if (!st.uniform && st.per.size() != count)
    {
        st.uniform = true;
        st.all = st.per.empty() ? D3D12_RESOURCE_STATE_COMMON : st.per.front();
    }
    const auto ready = [&](D3D12_RESOURCE_STATES current) { return exact ? current == state : Satisfies(current, state); };
    const auto target = [&](D3D12_RESOURCE_STATES current) { return exact ? state : Merge(current, state); };

    if (st.uniform)
    {
        if (ready(st.all))
            return;
        if (span.whole)
        {
            const D3D12_RESOURCE_STATES next = target(st.all);
            PushTransition(barriers, r->native, kAllSubresources, st.all, next);
            st.all = next;
            return;
        }
        // First partial transition: the resource switches to per-subresource states.
        st.per.assign(count, st.all);
        st.uniform = false;
    }

    if (span.whole)
    {
        // Every subresource goes to exactly `state`, so the resource is uniform again.
        for (u32 i = 0; i < count; ++i)
        {
            if (st.per[i] == state)
                continue;
            PushTransition(barriers, r->native, i, st.per[i], state);
            st.per[i] = state;
        }
        st.uniform = true;
        st.all = state;
        return;
    }

    for (u32 p = span.p0; p < span.p1; ++p)
    {
        for (u32 s = span.s0; s < span.s1; ++s)
        {
            for (u32 m = span.m0; m < span.m1; ++m)
            {
                const u32 sub = SubIndex(r, m, s, p);
                const D3D12_RESOURCE_STATES current = st.per[sub];
                if (ready(current))
                    continue;
                const D3D12_RESOURCE_STATES next = target(current);
                PushTransition(barriers, r->native, sub, current, next);
                st.per[sub] = next;
            }
        }
    }
    Collapse(st);
}

// States a COMPUTE list may transition between (async jobs record nothing else).
constexpr D3D12_RESOURCE_STATES kComputeStates = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_DEST |
    D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

bool ComputeLegal(D3D12_RESOURCE_STATES s)
{
    return !(u32(s) & ~u32(kComputeStates));
}

// Whether the tracked states of the range already allow `state` (RequireTracked would record nothing).
bool TrackedReady(Resource* r, const SubRange* range, D3D12_RESOURCE_STATES state, bool exact)
{
    if (!r->native)
        return false;
    const auto ready = [&](D3D12_RESOURCE_STATES current) { return exact ? current == state : Satisfies(current, state); };
    const SubresourceStates& st = r->states;
    if (st.uniform || st.per.size() != SubCountOf(r))
        return ready(st.uniform ? st.all : st.per.empty() ? D3D12_RESOURCE_STATE_COMMON : st.per.front());
    const Span span = range ? ClampSpan(r, *range) : WholeSpan(r);
    for (u32 p = span.p0; p < span.p1; ++p)
    {
        for (u32 s = span.s0; s < span.s1; ++s)
        {
            for (u32 m = span.m0; m < span.m1; ++m)
            {
                if (!ready(st.per[SubIndex(r, m, s, p)]))
                    return false;
            }
        }
    }
    return true;
}

// The immediate context takes over the state a command list left a subresource in.
void SetTracked(Resource* r, u32 sub, D3D12_RESOURCE_STATES state)
{
    SubresourceStates& st = r->states;
    const u32 count = SubCountOf(r);
    if (sub == kAllSubresources || count == 1)
    {
        st.uniform = true;
        st.all = state;
        return;
    }
    if (sub >= count)
        return;
    if (!st.uniform && st.per.size() != count)
    {
        st.uniform = true;
        st.all = st.per.empty() ? D3D12_RESOURCE_STATE_COMMON : st.per.front();
    }
    if (st.uniform)
    {
        if (st.all == state)
            return;
        st.per.assign(count, st.all);
        st.uniform = false;
    }
    st.per[sub] = state;
}
} // namespace

void StateTracker::Require(Resource* resource, const SubRange& range, D3D12_RESOURCE_STATES state,
    std::vector<D3D12_RESOURCE_BARRIER>& barriers)
{
    if (!m_deferred)
    {
        RequireTracked(resource, &range, state, false, barriers);
        return;
    }

    Resource* r = resource ? resource->Actual() : nullptr;
    if (!r || !r->native)
        return;
    const Span span = ClampSpan(r, range);
    if (span.Empty())
        return;
    const u32 count = SubCountOf(r);
    auto it = m_local.find(r);
    if (it == m_local.end())
        it = m_local.try_emplace(r).first;
    Local& local = it->second;
    if (local.first.size() != count)
    {
        local.first.assign(count, kUntouched);
        local.current.assign(count, kUntouched);
    }
    // A compute recording (async job) never transitions from or into a graphics state.
    const bool compute = m_context && m_context->recordCompute;
    const auto legal = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (!compute || (ComputeLegal(from) && ComputeLegal(to)))
            return true;
        static constexpr char kFormat[] = "! compute recording: a transition 0x%X -> 0x%X involves a graphics state; skipped";
        LogOnce(kFormat, kFormat, unsigned(from), unsigned(to));
        return false;
    };

    // Whole resource already known in one state: one barrier for all subresources.
    if (span.whole && count > 1)
    {
        const D3D12_RESOURCE_STATES current = local.current[0];
        bool same = local.first[0] != kUntouched;
        for (u32 i = 1; same && i < count; ++i)
            same = local.first[i] != kUntouched && local.current[i] == current;
        if (same)
        {
            if (Satisfies(current, state))
                return;
            const D3D12_RESOURCE_STATES next = Merge(current, state);
            if (!legal(current, next))
                return;
            PushTransition(barriers, r->native, kAllSubresources, current, next);
            local.transitioned = true;
            std::fill(local.current.begin(), local.current.end(), next);
            return;
        }
    }

    for (u32 p = span.p0; p < span.p1; ++p)
    {
        for (u32 s = span.s0; s < span.s1; ++s)
        {
            for (u32 m = span.m0; m < span.m1; ++m)
            {
                const u32 sub = SubIndex(r, m, s, p);
                if (local.first[sub] == kUntouched)
                {
                    // The first requirement becomes the list's entry state; the immediate context
                    // inserts that transition when it executes the list.
                    local.first[sub] = state;
                    local.current[sub] = state;
                    continue;
                }
                const D3D12_RESOURCE_STATES current = local.current[sub];
                if (Satisfies(current, state))
                    continue;
                const D3D12_RESOURCE_STATES next = Merge(current, state);
                if (!legal(current, next))
                    continue;
                PushTransition(barriers, r->native, sub, current, next);
                local.transitioned = true;
                local.current[sub] = next;
            }
        }
    }
}

void StateTracker::RequireAll(Resource* resource, D3D12_RESOURCE_STATES state, std::vector<D3D12_RESOURCE_BARRIER>& barriers)
{
    Resource* r = resource ? resource->Actual() : nullptr;
    if (!r || !r->native)
        return;
    SubRange all;
    all.mipCount = MipsOf(r);
    all.sliceCount = SlicesOf(r);
    all.planeCount = PlanesOf(r);
    Require(r, all, state, barriers);
}

void StateTracker::Harvest(std::vector<CommandList::Requirement>& out)
{
    for (auto& [resource, local] : m_local)
    {
        const size_t count = local.first.size();
        bool any = false;
        bool uniform = true;
        for (size_t i = 0; i < count; ++i)
        {
            if (local.first[i] == kUntouched)
            {
                uniform = false;
                continue;
            }
            any = true;
            if (local.first[i] != local.first[0] || local.current[i] != local.current[0])
                uniform = false;
        }
        if (!any)
            continue;
        if (uniform)
            out.push_back({resource, kAllSubresources, local.first[0], local.current[0], local.transitioned});
        else
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (local.first[i] != kUntouched)
                    out.push_back({resource, u32(i), local.first[i], local.current[i], local.transitioned});
            }
        }
        std::fill(local.first.begin(), local.first.end(), kUntouched);
        std::fill(local.current.begin(), local.current.end(), kUntouched);
        local.transitioned = false;
    }
    if (m_local.size() > kMaxLocalEntries)
        m_local.clear();
}

void StateTracker::Reset()
{
    m_local.clear();
}

// ================================================================================================
// Helpers of the operations
// ================================================================================================

namespace
{
// The COM object behind a Resource (pending readbacks and queries hold references like D3D11 does).
IUnknown* ComOf(Resource* r)
{
    switch (r->dimension)
    {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return static_cast<ID3D11Buffer*>(static_cast<Buffer*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return static_cast<ID3D11Texture1D*>(static_cast<Texture1D*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return static_cast<ID3D11Texture2D*>(static_cast<Texture2D*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return static_cast<ID3D11Texture3D*>(static_cast<Texture3D*>(r));
    default: return nullptr;
    }
}

void AddRefResource(Resource* r)
{
    if (IUnknown* object = ComOf(r))
        object->AddRef();
}

void ReleaseResource(Resource* r)
{
    if (IUnknown* object = ComOf(r))
        object->Release();
}

} // namespace

void ReleaseStagingReferences(std::vector<Resource*>& resources)
{
    for (Resource* r : resources)
        ReleaseResource(r);
    resources.clear();
}

namespace
{
Query* AsQuery(ID3D11Asynchronous* async)
{
    return static_cast<Query*>(static_cast<ID3D11Query*>(async));
}

Resource* ActualOf(ID3D11Resource* resource)
{
    Resource* r = Resource::From(resource);
    return r ? r->Actual() : nullptr;
}

bool IsStaging(const Resource* r)
{
    return r->usage == D3D11_USAGE_STAGING && r->linear;
}

// READBACK memory: a copy destination only. UPLOAD memory: a copy source only.
bool IsReadback(const Resource* r)
{
    return (r->cpuAccess & D3D11_CPU_ACCESS_READ) != 0;
}

// Staging memory an async job copies into is lent to it: the direct queue joins before touching it.
void GuardStaging(Context& c, Resource* r)
{
    if (r && r->lentTo)
        c.GuardLent(r, nullptr, D3D12_RESOURCE_STATE_COPY_DEST, true);
}

// A texture's contents changed outside render target/depth/UAV output (TextureWriteGeneration).
void NoteTextureWrite(Context& c, const Resource* r)
{
    constexpr UINT kOutputs = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
    if (!r->IsBuffer() && !(r->bindFlags & kOutputs))
        c.device()->TextureWritten();
}

D3D12_TEXTURE_COPY_LOCATION SubresourceLocation(ID3D12Resource* resource, u32 sub)
{
    D3D12_TEXTURE_COPY_LOCATION loc{};
    loc.pResource = resource;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    loc.SubresourceIndex = sub;
    return loc;
}

D3D12_TEXTURE_COPY_LOCATION FootprintLocation(ID3D12Resource* resource, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
{
    D3D12_TEXTURE_COPY_LOCATION loc{};
    loc.pResource = resource;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    loc.PlacedFootprint = footprint;
    return loc;
}

D3D12_BOX ToBox(const D3D11_BOX& box)
{
    return D3D12_BOX{box.left, box.top, box.front, box.right, box.bottom, box.back};
}

bool IsEmptyBox(const D3D11_BOX* box)
{
    return box && (box->left >= box->right || box->top >= box->bottom || box->front >= box->back);
}

// The staging memory of `r` is written (READBACK) or read (UPLOAD) by work recorded now: Map must wait
// for it. Deferred contexts leave the Resource fields to the immediate context's thread.
void NoteLinearAccess(Context& c, Resource* r)
{
    try
    {
        c.pendingReadback.push_back(r);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory while tracking a staging copy; a Map may read early";
        LogOnce(kFormat, kFormat);
        return;
    }
    AddRefResource(r);
    if (!c.IsDeferred())
        r->gpuWritePending = true;
}

// Sets the descriptor heaps when the open list has none yet (a new heap binding re-establishes the
// whole applied state, as d12_context_state.cpp does).
void EnsureHeaps(Context& c, ID3D12GraphicsCommandList* list)
{
    if (!c.applied.heapsDirty)
        return;
    Device* dev = c.device();
    u32 generation = 0;
    ID3D12DescriptorHeap* heaps[2] = {dev->gpuRing.Heap(), dev->samplerTables.Current(generation)};
    list->SetDescriptorHeaps(heaps[1] ? 2 : 1, heaps);
    c.InvalidateApplied();
    c.samplerGeneration = generation;
    c.applied.heapsDirty = false;
}

template <class T>
void ReleaseSlot(T*& slot)
{
    if (slot)
    {
        slot->Release();
        slot = nullptr;
    }
}

// Releases every D3D11 binding and restores the D3D11 defaults (applied state untouched).
void ReleaseBindings(Context& c)
{
    for (StageBindings& sb : c.stages)
    {
        ReleaseSlot(sb.shaderIface);
        sb.shader = nullptr;
        for (ID3D11Buffer*& cb : sb.cb)
            ReleaseSlot(cb);
        for (ID3D11ShaderResourceView*& srv : sb.srv)
            ReleaseSlot(srv);
        for (ID3D11SamplerState*& sampler : sb.samplers)
            ReleaseSlot(sampler);
        std::memset(sb.cbAppliedVersion, 0, sizeof(sb.cbAppliedVersion));
        std::memset(sb.cbAppliedAddress, 0, sizeof(sb.cbAppliedAddress));
        sb.cbDirty = sb.srvDirty = sb.samplerDirty = true;
    }
    ReleaseSlot(c.inputLayout);
    c.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    for (Context::VertexBufferBinding& vb : c.vbs)
    {
        ReleaseSlot(vb.buffer);
        vb.stride = 0;
        vb.offset = 0;
    }
    ReleaseSlot(c.indexBuffer);
    c.indexFormat = DXGI_FORMAT_R16_UINT;
    c.indexOffset = 0;
    ReleaseSlot(c.blendState);
    std::fill(std::begin(c.blendFactor), std::end(c.blendFactor), 1.0f);
    c.sampleMask = 0xffffffff;
    ReleaseSlot(c.depthState);
    c.stencilRef = 0;
    ReleaseSlot(c.rasterState);
    std::memset(c.viewports, 0, sizeof(c.viewports));
    c.numViewports = 0;
    std::memset(c.scissors, 0, sizeof(c.scissors));
    c.numScissors = 0;
    for (ID3D11RenderTargetView*& rtv : c.rtvs)
        ReleaseSlot(rtv);
    c.numRTVs = 0;
    ReleaseSlot(c.dsv);
    for (ID3D11UnorderedAccessView*& uav : c.csUavs)
        ReleaseSlot(uav);
    for (ID3D11UnorderedAccessView*& uav : c.psUavs)
        ReleaseSlot(uav);
    c.psUavStart = 0;
    if (c.vrs)
        c.VrsReset(false); // [dx12:vrs] hook
}

// The allocator slot of the open list is the one opened last (highest frame). When the frame advanced
// while the list was open, the slot is re-tagged so its reuse waits for the work (see AcquireAllocator).
template <class Slots>
void RetagOpenSlot(Slots& slots, u64 frame)
{
    u64 newest = 0;
    for (const auto& slot : slots)
        newest = (std::max)(newest, slot.frame);
    if (newest >= frame)
        return;
    for (auto& slot : slots)
    {
        if (slot.allocator && slot.frame == newest)
            slot.frame = frame;
    }
}

template <class Slots>
ID3D12CommandAllocator* OpenSlotAllocator(const Slots& slots)
{
    ID3D12CommandAllocator* allocator = nullptr;
    u64 newest = 0;
    for (const auto& slot : slots)
    {
        if (slot.allocator && (!allocator || slot.frame > newest))
        {
            allocator = slot.allocator;
            newest = slot.frame;
        }
    }
    return allocator;
}
} // namespace

namespace
{
D3D12_QUERY_TYPE NativeQueryType(D3D11_QUERY type);

// Hands query slots to the pool once the GPU is done with them.
void FreeSlotsLater(Device* device, std::vector<QueryHeapPool::Slot>& slots)
{
    for (const QueryHeapPool::Slot& slot : slots)
    {
        const QueryHeapPool::Slot freed = slot;
        try
        {
            device->releaser.Defer([device, freed] { device->queryHeaps.Free(D3D12_QUERY_HEAP_TYPE_OCCLUSION, freed); });
        }
        catch (...)
        {
        }
    }
    slots.clear();
}

// The result of a query ended in `list` goes to readback memory when the list closes.
void QueueResolve(Context& c, ID3D12GraphicsCommandList* list, const QueryHeapPool::Slot& slot, D3D12_QUERY_TYPE type)
{
    try
    {
        c.pendingResolves.push_back({slot.heap, slot.readback, type, slot.index});
    }
    catch (const std::bad_alloc&)
    {
        list->ResolveQueryData(slot.heap, type, slot.index, 1, slot.readback, u64(slot.index) * sizeof(u64));
    }
}

// Records the queued resolves: one per run of consecutive slots of a heap and type.
void ResolvePendingQueries(Context& c, ID3D12GraphicsCommandList* list)
{
    std::vector<Context::PendingResolve>& pending = c.pendingResolves;
    if (pending.empty())
        return;
    std::sort(pending.begin(), pending.end(), [](const Context::PendingResolve& a, const Context::PendingResolve& b) {
        if (a.heap != b.heap)
            return a.heap < b.heap;
        return a.type != b.type ? a.type < b.type : a.index < b.index;
    });
    for (size_t i = 0; i < pending.size();)
    {
        const Context::PendingResolve& first = pending[i];
        u32 count = 1;
        size_t next = i + 1;
        for (; next < pending.size() && pending[next].heap == first.heap && pending[next].type == first.type; ++next)
        {
            // A slot ended twice in one list resolves once, to its last value.
            if (pending[next].index == first.index + count - 1)
                continue;
            if (pending[next].index != first.index + count)
                break;
            ++count;
        }
        list->ResolveQueryData(first.heap, first.type, first.index, count, first.readback, u64(first.index) * sizeof(u64));
        i = next;
    }
    pending.clear();
}

// `list` is about to close with occlusion queries open: they end in it and continue in new slots.
void SplitActiveQueries(Context& c, ID3D12GraphicsCommandList* list)
{
    Device* dev = c.device();
    for (Query* q : c.activeQueries)
    {
        if (!q->slot.heap)
            continue;
        const D3D12_QUERY_TYPE native = NativeQueryType(q->m_desc.Query);
        list->EndQuery(q->slot.heap, native, q->slot.index);
        QueueResolve(c, list, q->slot, native);
        try
        {
            q->parts.push_back(q->slot);
        }
        catch (...)
        {
            static constexpr char kFormat[] = "an occlusion query split by a submission lost a part (out of memory)";
            LogOnce(kFormat, kFormat);
        }
        q->slot = dev->queryHeaps.Allocate(D3D12_QUERY_HEAP_TYPE_OCCLUSION);
        if (!q->slot.heap)
        {
            static constexpr char kFormat[] = "no query slot to continue an occlusion query after a submission";
            LogOnce(kFormat, kFormat);
        }
    }
}
} // namespace

// ================================================================================================
// Lifecycle
// ================================================================================================

Context::Context(Device* device, bool deferred, u32 index) : DeviceChild(device), m_deferred(deferred), m_index(index)
{
    descriptors.Init(&device->gpuRing);
    upload.Init(&device->uploadPages);
    tracker.Init(this, deferred);
    dynamicHeap = new (std::nothrow) DynamicHeap(device);
    annotation = new (std::nothrow) Annotation(this);
    try
    {
        barriers.reserve(64);
        if (!deferred)
        {
            endedQueries.reserve(256);
            activeQueries.reserve(64);
        }
    }
    catch (const std::bad_alloc&)
    {
    }
}

Context::~Context()
{
    ReleaseBindings(*this);
    if (vrs)
        VrsDestroy(); // [dx12:vrs] hook
    if (mesh)
        MeshDestroy(); // [dx12:mesh] hook
    for (Query* q : endedQueries)
        q->Release();
    endedQueries.clear();
    for (Query* q : activeQueries)
        q->Release();
    activeQueries.clear();
    for (Resource* r : pendingReadback)
        ReleaseResource(r);
    pendingReadback.clear();
    textureMaps.clear();
    if (m_list)
    {
        // Recorded but never submitted: nothing on the GPU refers to it.
        m_list->Close();
        m_list->Release();
        SetOpenList(nullptr, false);
    }
    // Queued but never submitted: the same. A deferred context's lists still queued on the
    // immediate one lose their owner and are released there. Async markers hold no list; a job's
    // recorder keeps its own list.
    {
        std::lock_guard queuedLock(m_queuedLock);
        for (const QueuedList& queued : m_queued)
        {
            // Job lists belong to the job (Device::AsyncShutdown).
            if (!queued.list || queued.job)
                continue;
            if (queued.owner && queued.owner != this)
                queued.owner->ReturnList(queued.list);
            else
                queued.list->Release();
        }
        m_queued.clear();
    }
    if (m_deferred && m_device->immediate && m_device->immediate != this)
    {
        Context* immediate = m_device->immediate;
        std::lock_guard queuedLock(immediate->m_queuedLock);
        for (QueuedList& queued : immediate->m_queued)
        {
            if (queued.owner == this)
                queued.owner = nullptr;
        }
    }
    if (m_deferred)
        m_device->AsyncForgetContext(this);
    {
        std::lock_guard lock(m_spareLock);
        for (auto& spares : m_spareLists)
        {
            for (ID3D12GraphicsCommandList* list : spares)
                ReleaseLater(m_device, list);
            spares.clear();
        }
    }
    for (auto& slots : m_allocators)
    {
        for (AllocatorSlot& slot : slots)
        {
            ReleaseLater(m_device, slot.allocator);
            slot.allocator = nullptr;
        }
    }
    if (dynamicHeap && dynamicHeap->Orphan())
        delete dynamicHeap;
    dynamicHeap = nullptr;
    delete annotation;
    annotation = nullptr;
}

ID3D12CommandAllocator* Context::AcquireAllocator(u32 type)
{
    const u64 frame = m_device->frame.load(std::memory_order_acquire);
    AllocatorSlot& slot = m_allocators[type][frame % (kFramesInFlight + 1)];
    if (!slot.allocator)
    {
        const HRESULT hr = m_device->d12->CreateCommandAllocator(
            type ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
        if (FAILED(hr))
        {
            slot.allocator = nullptr;
            ReportFailure(m_device, hr, "CreateCommandAllocator");
            return nullptr;
        }
        slot.frame = frame;
        return slot.allocator;
    }
    if (slot.frame != frame)
    {
        // Device::EndFrame guarantees the frame this slot had kFramesInFlight + 1 frames ago. A slot
        // re-tagged by a list that stayed open across a Present is newer than that: wait for the GPU.
        if (slot.frame + kFramesInFlight + 1 > frame)
        {
            static constexpr char kFormat[] = "a command list stayed open across a Present; its allocator waits for the GPU";
            LogOnce(kFormat, kFormat);
            FenceTimeline& fence = m_device->fence;
            if (fence.NextValue() > 1)
                fence.Wait(fence.NextValue() - 1);
        }
        const HRESULT hr = slot.allocator->Reset();
        if (FAILED(hr))
        {
            ReportFailure(m_device, hr, "ID3D12CommandAllocator::Reset");
            return nullptr;
        }
        slot.frame = frame;
    }
    return slot.allocator;
}

void Context::OpenList()
{
    // A compute recording (async job) records a COMPUTE list where the device has a compute queue.
    const u32 type = recordCompute && m_device->computeQueue ? 1 : 0;
    ID3D12CommandAllocator* allocator = AcquireAllocator(type);
    if (!allocator)
        return;
    ID3D12GraphicsCommandList* list = nullptr;
    {
        std::lock_guard lock(m_spareLock);
        if (!m_spareLists[type].empty())
        {
            list = m_spareLists[type].back();
            m_spareLists[type].pop_back();
        }
    }
    if (list)
    {
        const HRESULT hr = list->Reset(allocator, nullptr);
        if (FAILED(hr))
        {
            ReportFailure(m_device, hr, "ID3D12GraphicsCommandList::Reset");
            ReleaseLater(m_device, list);
            list = nullptr;
        }
    }
    bool created = false;
    if (!list)
    {
        const HRESULT hr = m_device->d12->CreateCommandList(0,
            type ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr))
        {
            ReportFailure(m_device, hr, "CreateCommandList");
            return;
        }
        created = true;
    }
    m_listType = type;
    SetOpenList(list, created);
    InvalidateApplied();
    // A fresh list shades at 1x1 without a rate image.
    applied.vrsListNonDefault = false;
    if (annotation)
        annotation->Resume(list);
    // Occlusion queries open across the last submission continue here.
    for (Query* q : activeQueries)
    {
        if (q->slot.heap)
            list->BeginQuery(q->slot.heap, NativeQueryType(q->m_desc.Query), q->slot.index);
    }
}

void Context::SetOpenList(ID3D12GraphicsCommandList* list, bool created)
{
    m_list = list;
    m_list4 = nullptr;
    m_list5 = nullptr;
    m_list6 = nullptr;
    if (!list)
        return;
    std::lock_guard lock(m_spareLock);
    auto it = std::find_if(m_listInterfaces.begin(), m_listInterfaces.end(),
        [list](const ListInterfaces& entry) { return entry.list == list; });
    // A new list may reuse the address of a released one: its entry is queried again.
    if (it == m_listInterfaces.end() || created)
    {
        ListInterfaces entry;
        entry.list = list;
        // Weak pointers: the list's own reference keeps the object alive.
        const auto query = [list](auto*& out) {
            if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&out))) && out)
                out->Release();
            else
                out = nullptr;
        };
        query(entry.list4);
        query(entry.list5);
        query(entry.list6);
        try
        {
            if (it == m_listInterfaces.end())
                it = m_listInterfaces.insert(m_listInterfaces.end(), entry);
            else
                *it = entry;
        }
        catch (const std::bad_alloc&)
        {
            m_list4 = entry.list4;
            m_list5 = entry.list5;
            m_list6 = entry.list6;
            return;
        }
    }
    m_list4 = it->list4;
    // Shading rates and mesh draws do not exist on COMPUTE lists.
    m_list5 = m_listType ? nullptr : it->list5;
    m_list6 = m_listType ? nullptr : it->list6;
}

ID3D12GraphicsCommandList* Context::List()
{
    if (!m_list)
        OpenList();
    return m_list;
}

u64 Context::Submit(ID3D12CommandList* const* extra, u32 extraCount)
{
    FenceTimeline& fence = m_device->fence;
    if (m_deferred)
    {
        static constexpr char kFormat[] = "Submit on a deferred context ignored";
        LogOnce(kFormat, kFormat);
        return fence.NextValue() - 1;
    }
    if (m_list)
        FlushBarriers();
    if (!m_list && m_queued.empty() && !extraCount && endedQueries.empty() && pendingReadback.empty() &&
        !m_device->UploadsPending())
        return fence.NextValue() - 1;

    // Creation-time uploads run before any work that may read what they write.
    m_device->FlushUploads();

    ID3D12GraphicsCommandList* own = CloseOwn();
    std::unique_lock queuedLock(m_queuedLock);

    // Queued lists first, then the own list, then `extra`: the order they were recorded in. One
    // ExecuteCommandLists for all of them: tracked buffers decay to COMMON between two calls, which
    // the barriers recorded in the lists do not expect.
    ID3D12CommandList* local[64];
    ID3D12CommandList** batch = local;
    size_t capacity = std::size(local);
    const size_t total = m_queued.size() + 1 + extraCount;
    if (total > capacity)
    {
        try
        {
            m_submitBatch.resize(total);
            batch = m_submitBatch.data();
            capacity = total;
        }
        catch (const std::bad_alloc&)
        {
            static constexpr char kFormat[] = "out of memory while submitting: the lists go in several calls";
            LogOnce(kFormat, kFormat);
        }
    }
    size_t n = 0;
    // The compute queue must wait for the direct work before a fork (a cross-fence signal); forks
    // with no direct work in between share one signal.
    bool crossStale = true;
    const auto execute = [&] {
        if (!n)
            return;
        m_device->queue->ExecuteCommandLists(UINT(n), batch);
        n = 0;
        crossStale = true;
    };
    const auto add = [&](ID3D12CommandList* list) {
        if (!list)
            return;
        if (n == capacity)
            execute();
        batch[n++] = list;
    };
    for (const QueuedList& queued : m_queued)
    {
        if (!queued.job)
            add(queued.list);
        else if (queued.list)
        {
            // A job without a compute queue runs in order, in a call of its own: tracked buffers decay
            // at its boundaries as they do around a job on the compute queue.
            execute();
            add(queued.list);
            execute();
        }
        else
        {
            // Async markers split the submission: the direct work so far goes first.
            execute();
            if (queued.join)
                m_device->AsyncSubmitJoin(queued.job);
            else
                m_device->AsyncSubmitFork(queued.job, crossStale);
        }
    }
    add(own);
    for (u32 i = 0; i < extraCount; ++i)
        add(extra[i]);
    execute();
    m_device->DecayTrackedBuffers();
    const u64 value = fence.Signal(m_device->queue);
    m_device->Count(Device::StatSubmits);
    for (const QueuedList& queued : m_queued)
    {
        if (queued.job)
        {
            if (queued.list)
                m_device->AsyncSubmitInOrder(queued.job, queued.list, queued.owner, value);
        }
        else if (queued.owner)
            queued.owner->ReturnList(queued.list);
        else
            ReleaseLater(m_device, queued.list);
    }
    m_queued.clear();
    queuedLock.unlock();

    for (Query* q : endedQueries)
    {
        // A query begun again since its End is not finished; its next End is in a later list.
        if (!q->active)
            q->fence = value;
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
    if (own)
        ReturnList(own);

    // Frames end at Present. Work that never presents (long loads, captures) would keep its frame
    // memory forever: past a budget the submission ends a frame by itself. Not while an async job holds
    // frame memory this fence does not cover (a later submission or Present ends the frame then).
    constexpr u64 kEmergencyBytes = 256ull << 20;
    if ((m_device->uploadPages.BytesInFlight() > kEmergencyBytes || (dynamicHeap && dynamicHeap->PendingBytes() > kEmergencyBytes / 2)) &&
        !m_device->AsyncJobsPending())
    {
        static constexpr char kFormat[] = "no Present for a long time: frame memory is recycled at a submission";
        LogOnce(kFormat, kFormat);
        m_device->EndFrame(value);
    }
    return value;
}

ID3D12GraphicsCommandList* Context::CloseOwn()
{
    ID3D12GraphicsCommandList* own = m_list;
    SetOpenList(nullptr, false);
    if (!own)
        return nullptr;
    RetagOpenSlot(m_allocators[m_listType], m_device->frame.load(std::memory_order_acquire));
    if (!activeQueries.empty())
        SplitActiveQueries(*this, own);
    ResolvePendingQueries(*this, own);
    if (annotation)
        annotation->Suspend(own);
    const HRESULT hr = own->Close();
    if (FAILED(hr))
    {
        ReportFailure(m_device, hr, "ID3D12GraphicsCommandList::Close");
        ReturnList(own);
        return nullptr;
    }
    return own;
}

void Context::GuardLent(Resource* resource, const SubRange* range, D3D12_RESOURCE_STATES state, bool exact)
{
    Resource* r = resource ? resource->Actual() : nullptr;
    if (m_deferred || !r || !r->lentTo)
        return;
    // A read-shared loan survives accesses that need no transition.
    if (r->lentShared && TrackedReady(r, range, state, exact))
        return;
    JoinLent(r);
}

void Context::RequireExact(Resource* resource, u32 subresource, D3D12_RESOURCE_STATES state, bool exact)
{
    Resource* r = resource ? resource->Actual() : nullptr;
    if (!r || !r->native)
        return;
    if (subresource == kAllSubresources)
    {
        GuardLent(r, nullptr, state, exact);
        RequireTracked(r, nullptr, state, exact, barriers);
        return;
    }
    const SubRange range = RangeOfSub(r, subresource);
    GuardLent(r, &range, state, exact);
    RequireTracked(r, &range, state, exact, barriers);
}

void Context::AdoptState(Resource* resource, u32 subresource, D3D12_RESOURCE_STATES state)
{
    Resource* r = resource ? resource->Actual() : nullptr;
    if (!r || !r->native)
        return;
    SetTracked(r, subresource, state);
    Collapse(r->states);
}

void Context::QueueJobEntry(AsyncJob* job, bool join, ID3D12GraphicsCommandList* list, Context* owner)
{
    FlushBarriers();
    std::lock_guard queuedLock(m_queuedLock);
    m_queued.reserve(m_queued.size() + 2);
    if (ID3D12GraphicsCommandList* own = CloseOwn())
        m_queued.push_back({own, this});
    m_queued.push_back({list, owner, job, join});
}

void Context::Require(Resource* resource, const SubRange& range, D3D12_RESOURCE_STATES state)
{
    try
    {
        GuardLent(resource, &range, state, false);
        tracker.Require(resource, range, state, barriers);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory while tracking resource states; a barrier is missing";
        LogOnce(kFormat, kFormat);
    }
}

void Context::RequireAll(Resource* resource, D3D12_RESOURCE_STATES state)
{
    try
    {
        GuardLent(resource, nullptr, state, false);
        tracker.RequireAll(resource, state, barriers);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory while tracking resource states; a barrier is missing";
        LogOnce(kFormat, kFormat);
    }
}

void Context::FlushBarriers()
{
    if (barriers.empty())
        return;
    if (ID3D12GraphicsCommandList* list = List())
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    m_device->Count(Device::StatBarriers, barriers.size());
    barriers.clear();
}

void Context::UavBarrier()
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = nullptr;
    try
    {
        barriers.push_back(b);
    }
    catch (const std::bad_alloc&)
    {
        // No room to queue it: issue it on its own.
        FlushBarriers();
        if (ID3D12GraphicsCommandList* list = List())
            list->ResourceBarrier(1, &b);
    }
}

void Context::InvalidateApplied()
{
    applied.heapsDirty = true;
    applied.rootSignatureGraphics = false;
    applied.rootSignatureCompute = false;
    applied.pso = nullptr;
    applied.psoHash = 0;
    applied.psoDirty = true;
    applied.vbDirty = true;
    applied.ibDirty = true;
    applied.rtDirty = true;
    applied.viewportDirty = true;
    applied.scissorDirty = true;
    applied.stencilRefDirty = true;
    applied.blendFactorDirty = true;
    applied.topologyDirty = true;
    applied.psUavDirty = true;
    applied.csUavDirty = true;
    applied.rootSignatureMesh = false;
    for (StageBindings& sb : stages)
    {
        std::memset(sb.cbAppliedAddress, 0, sizeof(sb.cbAppliedAddress));
        std::memset(sb.cbAppliedVersion, 0, sizeof(sb.cbAppliedVersion));
        sb.cbDirty = true;
        sb.srvDirty = true;
        sb.samplerDirty = true;
    }
    // The list keeps whatever rate it has (OpenList resets the flag for fresh lists).
    applied.vrsDirty = true;
    if (vrs)
        VrsReset(true); // [dx12:vrs] hook
}

void Context::ReturnList(ID3D12GraphicsCommandList* list)
{
    if (!list)
        return;
    try
    {
        std::lock_guard lock(m_spareLock);
        m_spareLists[list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : 0].push_back(list);
    }
    catch (...)
    {
        ReleaseLater(m_device, list);
    }
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

HRESULT MapDynamicBuffer(Context& c, Resource* r, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (sub)
        return E_INVALIDARG;
    const u64 size = r->byteWidth ? r->byteWidth : static_cast<Buffer*>(r)->Desc().ByteWidth;
    if (type == D3D11_MAP_WRITE_NO_OVERWRITE && r->current.alloc.cpu)
    {
        FillMapped(mapped, r->current.alloc.cpu, size, size);
        return S_OK;
    }
    if (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE)
    {
        static constexpr char kFormat[] = "Map of a dynamic buffer with type %u refused (D3D11 allows DISCARD/NO_OVERWRITE)";
        LogOnce(kFormat, kFormat, unsigned(type));
        return E_INVALIDARG;
    }

    // DISCARD renames the buffer: draws recorded so far keep the old version, later ones read the new.
    Device* dev = c.device();
    UploadAlloc alloc{};
    DynamicHeap* heap = c.dynamicHeap;
    const DynamicVersion old = r->current;
    // The usual case: the version being replaced came from this context's heap (one lock for both).
    const bool retireHere = heap && old.heap == heap;
    if (!heap || !heap->Allocate(size, alloc, retireHere ? &old.alloc : nullptr))
    {
        heap = nullptr;
        static constexpr char kFormat[] = "dynamic buffer versions fall back to frame memory (out of persistent memory)";
        LogOnce(kFormat, kFormat);
        alloc = c.upload.Allocate(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, dev->frame.load(std::memory_order_acquire));
        if (!alloc.cpu)
            return E_OUTOFMEMORY;
    }
    // A heap still set here means Allocate succeeded (and retired `old` when it was its own).
    const bool retired = heap && retireHere;
    r->current.alloc = alloc;
    r->current.heap = heap;
    ++r->current.version;
    if (!retired && old.heap && old.heap->Retire(old.alloc))
        delete old.heap;
    FillMapped(mapped, alloc.cpu, size, size);
    return S_OK;
}

HRESULT MapDynamicTexture(Context& c, Resource* r, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (type != D3D11_MAP_WRITE_DISCARD)
    {
        static constexpr char kFormat[] = "Map of a dynamic texture with type %u refused (D3D11 allows WRITE_DISCARD)";
        LogOnce(kFormat, kFormat, unsigned(type));
        return E_INVALIDARG;
    }
    if (!r->native || sub >= MipsOf(r) * SlicesOf(r))
        return E_INVALIDARG;
    Device* dev = c.device();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 total = 0;
    D3D12_RESOURCE_DESC desc = r->nativeDesc;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    dev->d12->GetCopyableFootprints(&desc, sub, 1, 0, &footprint, &rows, &rowSize, &total);
    if (!total || total == ~0ull)
        return E_INVALIDARG;
    const UploadAlloc alloc =
        c.upload.Allocate(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, dev->frame.load(std::memory_order_acquire));
    if (!alloc.cpu)
        return E_OUTOFMEMORY;
    footprint.Offset = alloc.offset;

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
    entry->footprint = footprint;
    FillMapped(mapped, alloc.cpu, footprint.Footprint.RowPitch, u64(footprint.Footprint.RowPitch) * rows);
    return S_OK;
}

HRESULT MapStaging(Context& c, Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (c.IsDeferred())
    {
        static constexpr char kFormat[] = "Map of a staging resource on a deferred context refused";
        LogOnce(kFormat, kFormat);
        return E_INVALIDARG;
    }
    const bool read = type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE;
    const bool write = type == D3D11_MAP_WRITE || type == D3D11_MAP_READ_WRITE;
    if ((!read && !write) || (read && !(r->cpuAccess & D3D11_CPU_ACCESS_READ)) ||
        (write && !(r->cpuAccess & D3D11_CPU_ACCESS_WRITE)) || !r->linearCpu)
    {
        static constexpr char kFormat[] = "Map of a staging resource with type %u refused (CPU access 0x%X)";
        LogOnce(kFormat, kFormat, unsigned(type), r->cpuAccess);
        return E_INVALIDARG;
    }
    const bool buffer = r->IsBuffer() || r->footprints.empty();
    if (!buffer && sub >= r->footprints.size())
        return E_INVALIDARG;

    GuardStaging(c, r);
    // A copy into (or out of) the memory recorded in the open list has to reach the GPU first.
    if (r->gpuWritePending)
        c.Submit();
    FenceTimeline& fence = c.device()->fence;
    if (r->lastGpuWrite && !fence.IsComplete(r->lastGpuWrite))
    {
        if (flags & D3D11_MAP_FLAG_DO_NOT_WAIT)
            return DXGI_ERROR_WAS_STILL_DRAWING;
        fence.Wait(r->lastGpuWrite);
    }

    if (buffer)
    {
        FillMapped(mapped, r->linearCpu, r->byteWidth, r->byteWidth);
        return S_OK;
    }
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = r->footprints[sub];
    const u32 rows = sub < r->rowCounts.size() ? r->rowCounts[sub] : fp.Footprint.Height;
    FillMapped(mapped, r->linearCpu + fp.Offset, fp.Footprint.RowPitch, u64(fp.Footprint.RowPitch) * rows);
    return S_OK;
}
} // namespace

HRESULT STDMETHODCALLTYPE Context::Map(ID3D11Resource* resource, UINT subresource, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if (mapped)
        *mapped = {};
    if (!resource || !mapped)
        return E_INVALIDARG;
    m_device->Count(Device::StatMaps);
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
        static constexpr char kFormat[] = "Map of a resource with usage %u refused (not DYNAMIC or STAGING)";
        LogOnce(kFormat, kFormat, unsigned(r->usage));
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
            if (!r->native)
                return;
            Require(r, RangeOfSub(r, entry.subresource), D3D12_RESOURCE_STATE_COPY_DEST);
            FlushBarriers();
            if (ID3D12GraphicsCommandList* list = List())
            {
                const D3D12_TEXTURE_COPY_LOCATION dst = SubresourceLocation(r->native, entry.subresource);
                const D3D12_TEXTURE_COPY_LOCATION src = FootprintLocation(entry.alloc.resource, entry.footprint);
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            NoteTextureWrite(*this, r);
            return;
        }
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in Unmap";
        LogOnce(kFormat, kFormat);
    }
}

// ================================================================================================
// Copies
// ================================================================================================

namespace
{
// Where the bytes of a buffer are, and whether the copy may write/read them.
struct BufferMemory
{
    ID3D12Resource* resource = nullptr;
    u64 offset = 0;
    u64 size = 0;
    bool tracked = false;
    bool staging = false;
};

BufferMemory BufferMemoryOf(Resource* r)
{
    BufferMemory m;
    m.size = r->byteWidth;
    if (r->native)
    {
        m.resource = r->native;
        m.tracked = true;
    }
    else if (r->usage == D3D11_USAGE_DYNAMIC)
    {
        m.resource = r->current.alloc.resource;
        m.offset = r->current.alloc.offset;
    }
    else if (r->usage == D3D11_USAGE_STAGING)
    {
        m.resource = r->linear;
        m.staging = true;
    }
    else
    {
        m.resource = r->range.resource;
        m.offset = r->range.offset;
    }
    return m;
}

void CopyBuffers(Context& c, Resource* d, u64 dstOffset, Resource* s, u64 srcOffset, u64 size)
{
    const BufferMemory dst = BufferMemoryOf(d);
    const BufferMemory src = BufferMemoryOf(s);
    if (!dst.resource || !src.resource || dstOffset >= dst.size || srcOffset >= src.size)
        return;
    if (!dst.tracked && !(dst.staging && IsReadback(d)))
    {
        static constexpr char kFormat[] = "copy into a %s buffer refused (only GPU-written, oversized or read-back buffers take copies)";
        LogOnce(kFormat, kFormat, d->usage == D3D11_USAGE_DYNAMIC ? "dynamic" : d->usage == D3D11_USAGE_STAGING ? "upload staging" : "static");
        return;
    }
    if (src.staging && IsReadback(s))
    {
        static constexpr char kFormat[] = "copy out of a read-back staging buffer refused (READBACK memory is not a copy source)";
        LogOnce(kFormat, kFormat);
        return;
    }
    size = (std::min)(size, (std::min)(dst.size - dstOffset, src.size - srcOffset));
    if (!size)
        return;
    if (dst.staging)
        GuardStaging(c, d);
    if (src.tracked)
        c.RequireAll(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (dst.tracked)
        c.RequireAll(d, D3D12_RESOURCE_STATE_COPY_DEST);
    c.FlushBarriers();
    ID3D12GraphicsCommandList* list = c.List();
    if (!list)
        return;
    list->CopyBufferRegion(dst.resource, dst.offset + dstOffset, src.resource, src.offset + srcOffset, size);
    if (dst.staging)
        NoteLinearAccess(c, d);
    if (src.staging)
        NoteLinearAccess(c, s);
}

// Refuses copies D3D12 cannot express; logs once per reason.
bool CheckTextureCopy(Resource* d, Resource* s)
{
    const bool dStaging = IsStaging(d);
    const bool sStaging = IsStaging(s);
    if (dStaging && sStaging)
    {
        static constexpr char kFormat[] = "staging-to-staging texture copy is not implemented";
        LogOnce(kFormat, kFormat);
        return false;
    }
    if ((!dStaging && !d->native) || (!sStaging && !s->native))
        return false;
    if (dStaging && !IsReadback(d))
    {
        static constexpr char kFormat[] = "copy into a write-only staging texture refused (UPLOAD memory is not a copy destination)";
        LogOnce(kFormat, kFormat);
        return false;
    }
    if (sStaging && IsReadback(s))
    {
        static constexpr char kFormat[] = "copy out of a read-back staging texture refused (READBACK memory is not a copy source)";
        LogOnce(kFormat, kFormat);
        return false;
    }
    if ((dStaging && s->nativeDesc.SampleDesc.Count > 1) || (sStaging && d->nativeDesc.SampleDesc.Count > 1))
    {
        static constexpr char kFormat[] = "copy between a multisampled texture and staging memory refused (resolve first)";
        LogOnce(kFormat, kFormat);
        return false;
    }
    return true;
}
} // namespace

void STDMETHODCALLTYPE Context::CopyResource(ID3D11Resource* dst, ID3D11Resource* src)
{
    try
    {
        Resource* d = ActualOf(dst);
        Resource* s = ActualOf(src);
        if (!d || !s || d == s)
            return;
        if (d->IsBuffer() != s->IsBuffer())
            return;
        if (d->IsBuffer())
        {
            CopyBuffers(*this, d, 0, s, 0, (std::min)(d->byteWidth, s->byteWidth));
            return;
        }
        if (!CheckTextureCopy(d, s))
            return;

        const bool dStaging = IsStaging(d);
        const bool sStaging = IsStaging(s);
        if (dStaging)
            GuardStaging(*this, d);
        if (!dStaging && !sStaging)
        {
            // Every plane of a depth-stencil texture goes with it.
            RequireAll(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
            RequireAll(d, D3D12_RESOURCE_STATE_COPY_DEST);
            FlushBarriers();
            if (ID3D12GraphicsCommandList* list = List())
                list->CopyResource(d->native, s->native);
            NoteTextureWrite(*this, d);
            return;
        }

        if (dStaging)
        {
            // Texture -> read-back memory: every subresource into its placed footprint.
            const u32 count = (std::min)(SubCountOf(s), u32(d->footprints.size()));
            RequireAll(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            ID3D12GraphicsCommandList* list = List();
            if (!list)
                return;
            for (u32 i = 0; i < count; ++i)
            {
                const D3D12_TEXTURE_COPY_LOCATION to = FootprintLocation(d->linear, d->footprints[i]);
                const D3D12_TEXTURE_COPY_LOCATION from = SubresourceLocation(s->native, i);
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            NoteLinearAccess(*this, d);
            return;
        }

        // Upload memory -> texture.
        const u32 count = (std::min)(SubCountOf(d), u32(s->footprints.size()));
        RequireAll(d, D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        ID3D12GraphicsCommandList* list = List();
        if (!list)
            return;
        for (u32 i = 0; i < count; ++i)
        {
            const D3D12_TEXTURE_COPY_LOCATION to = SubresourceLocation(d->native, i);
            const D3D12_TEXTURE_COPY_LOCATION from = FootprintLocation(s->linear, s->footprints[i]);
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        NoteLinearAccess(*this, s);
        NoteTextureWrite(*this, d);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in CopyResource";
        LogOnce(kFormat, kFormat);
    }
}

void STDMETHODCALLTYPE Context::CopySubresourceRegion(ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
    ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
{
    try
    {
        Resource* d = ActualOf(dst);
        Resource* s = ActualOf(src);
        if (!d || !s || d->IsBuffer() != s->IsBuffer() || IsEmptyBox(box))
            return;
        if (d->IsBuffer())
        {
            if (dstSub || srcSub)
                return;
            const u64 offset = box ? box->left : 0;
            const u64 size = box ? u64(box->right) - box->left : s->byteWidth;
            CopyBuffers(*this, d, x, s, offset, size);
            return;
        }
        if (!CheckTextureCopy(d, s))
            return;
        if (dstSub >= MipsOf(d) * SlicesOf(d) || srcSub >= MipsOf(s) * SlicesOf(s))
            return;

        const bool dStaging = IsStaging(d);
        const bool sStaging = IsStaging(s);
        // D3D11 subresources span every plane; D3D12 copies one plane per call.
        const u32 planes = dStaging ? PlanesOf(s) : PlanesOf(d);
        const u32 dPlaneStride = MipsOf(d) * SlicesOf(d);
        const u32 sPlaneStride = MipsOf(s) * SlicesOf(s);
        if (dStaging)
            GuardStaging(*this, d);
        if (!sStaging)
            Require(s, PlanesOfSub(s, srcSub), D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!dStaging)
            Require(d, PlanesOfSub(d, dstSub), D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        ID3D12GraphicsCommandList* list = List();
        if (!list)
            return;
        D3D12_BOX region{};
        if (box)
            region = ToBox(*box);
        for (u32 p = 0; p < planes; ++p)
        {
            const u32 dIndex = dstSub + p * dPlaneStride;
            const u32 sIndex = srcSub + p * sPlaneStride;
            if ((dStaging && dIndex >= d->footprints.size()) || (sStaging && sIndex >= s->footprints.size()))
                break;
            const D3D12_TEXTURE_COPY_LOCATION to =
                dStaging ? FootprintLocation(d->linear, d->footprints[dIndex]) : SubresourceLocation(d->native, dIndex);
            const D3D12_TEXTURE_COPY_LOCATION from =
                sStaging ? FootprintLocation(s->linear, s->footprints[sIndex]) : SubresourceLocation(s->native, sIndex);
            list->CopyTextureRegion(&to, x, y, z, &from, box ? &region : nullptr);
        }
        if (dStaging)
            NoteLinearAccess(*this, d);
        else
            NoteTextureWrite(*this, d);
        if (sStaging)
            NoteLinearAccess(*this, s);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in CopySubresourceRegion";
        LogOnce(kFormat, kFormat);
    }
}

void STDMETHODCALLTYPE Context::UpdateSubresource(ID3D11Resource* dst, UINT dstSub, const D3D11_BOX* box,
    const void* data, UINT rowPitch, UINT depthPitch)
{
    try
    {
        Resource* r = ActualOf(dst);
        if (!r || !data || IsEmptyBox(box))
            return;
        Device* dev = device();
        const u64 frame = dev->frame.load(std::memory_order_acquire);

        if (r->IsBuffer())
        {
            if (!r->native || r->usage != D3D11_USAGE_DEFAULT)
            {
                static constexpr char kFormat[] = "UpdateSubresource of a buffer without its own GPU memory refused (usage %u)";
                LogOnce(kFormat, kFormat, unsigned(r->usage));
                return;
            }
            if (dstSub || (box && box->left >= r->byteWidth))
                return;
            const u64 offset = box ? box->left : 0;
            const u64 size = (std::min)(box ? u64(box->right) - box->left : r->byteWidth, r->byteWidth - offset);
            const UploadAlloc alloc = upload.Allocate(size, 16, frame);
            if (!alloc.cpu)
                return;
            std::memcpy(alloc.cpu, data, size_t(size));
            RequireAll(r, D3D12_RESOURCE_STATE_COPY_DEST);
            FlushBarriers();
            if (ID3D12GraphicsCommandList* list = List())
                list->CopyBufferRegion(r->native, offset, alloc.resource, alloc.offset, size);
            return;
        }

        if (!r->native || r->usage != D3D11_USAGE_DEFAULT || PlanesOf(r) > 1 || IsDepthFormat(r->format) ||
            r->nativeDesc.SampleDesc.Count > 1)
        {
            static constexpr char kFormat[] = "UpdateSubresource of a texture refused (usage %u, format %u, samples %u)";
            LogOnce(kFormat, kFormat, unsigned(r->usage), unsigned(r->format), unsigned(r->nativeDesc.SampleDesc.Count));
            return;
        }
        const u32 mips = MipsOf(r);
        if (dstSub >= mips * SlicesOf(r))
            return;
        const u32 mip = dstSub % mips;
        const D3D12_RESOURCE_DESC& nd = r->nativeDesc;
        const bool volume = nd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const u32 width = u32((std::max)(nd.Width >> mip, u64(1)));
        const u32 height = (std::max)(nd.Height >> mip, 1u);
        const u32 depth = volume ? (std::max)(u32(nd.DepthOrArraySize) >> mip, 1u) : 1u;
        D3D12_BOX region{0, 0, 0, width, height, depth};
        if (box)
        {
            region = ToBox(*box);
            region.right = (std::min)(region.right, width);
            region.bottom = (std::min)(region.bottom, height);
            region.back = (std::min)(region.back, depth);
            if (region.left >= region.right || region.top >= region.bottom || region.front >= region.back)
                return;
        }
        const u32 w = region.right - region.left;
        const u32 h = region.bottom - region.top;
        const u32 dd = region.back - region.front;

        // The upload layout of the region: a one-mip texture of its size (whole blocks for BC).
        const bool blocks = IsBlockCompressed(nd.Format);
        D3D12_RESOURCE_DESC desc = nd;
        desc.Width = blocks ? AlignUp(w, 4) : w;
        desc.Height = blocks ? u32(AlignUp(h, 4)) : h;
        desc.DepthOrArraySize = UINT16(volume ? dd : 1);
        desc.MipLevels = 1;
        desc.Alignment = 0;
        desc.SampleDesc = {1, 0};
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 total = 0;
        dev->d12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
        if (!total || total == ~0ull)
            return;
        const UploadAlloc alloc = upload.Allocate(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, frame);
        if (!alloc.cpu)
            return;
        footprint.Offset = alloc.offset;
        const u8* source = static_cast<const u8*>(data);
        for (u32 slice = 0; slice < dd; ++slice)
        {
            for (u32 row = 0; row < rows; ++row)
            {
                std::memcpy(alloc.cpu + (u64(slice) * rows + row) * footprint.Footprint.RowPitch,
                    source + u64(slice) * depthPitch + u64(row) * rowPitch, size_t(rowSize));
            }
        }
        Require(r, RangeOfSub(r, dstSub), D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        if (ID3D12GraphicsCommandList* list = List())
        {
            const D3D12_TEXTURE_COPY_LOCATION to = SubresourceLocation(r->native, dstSub);
            const D3D12_TEXTURE_COPY_LOCATION from = FootprintLocation(alloc.resource, footprint);
            list->CopyTextureRegion(&to, region.left, region.top, region.front, &from, nullptr);
        }
        NoteTextureWrite(*this, r);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in UpdateSubresource";
        LogOnce(kFormat, kFormat);
    }
}

void STDMETHODCALLTYPE Context::CopyStructureCount(ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*)
{
    static constexpr char kFormat[] = "CopyStructureCount is not implemented (no append/counter UAVs in the renderer)";
    LogOnce(kFormat, kFormat);
}

// ================================================================================================
// Clears and resolves
// ================================================================================================

void STDMETHODCALLTYPE Context::ClearRenderTargetView(ID3D11RenderTargetView* rtv, const FLOAT color[4])
{
    if (recordCompute)
    {
        static constexpr char kFormat[] = "! compute recording (async job): ClearRenderTargetView dropped";
        LogOnce(kFormat, kFormat);
        return;
    }
    try
    {
        View* v = View::From(rtv);
        if (!v || !color)
            return;
        Require(v->resource, v->range, D3D12_RESOURCE_STATE_RENDER_TARGET);
        FlushBarriers();
        if (ID3D12GraphicsCommandList* list = List())
            list->ClearRenderTargetView(v->Descriptor(), color, 0, nullptr);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in ClearRenderTargetView";
        LogOnce(kFormat, kFormat);
    }
}

void STDMETHODCALLTYPE Context::ClearDepthStencilView(ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (recordCompute)
    {
        static constexpr char kFormat[] = "! compute recording (async job): ClearDepthStencilView dropped";
        LogOnce(kFormat, kFormat);
        return;
    }
    try
    {
        View* v = View::From(dsv);
        Resource* r = v ? v->resource->Actual() : nullptr;
        if (!r || !r->native)
            return;
        u32 clear = 0;
        if (flags & D3D11_CLEAR_DEPTH)
            clear |= D3D12_CLEAR_FLAG_DEPTH;
        // Formats without a stencil plane ignore a stencil clear, as in D3D11.
        if ((flags & D3D11_CLEAR_STENCIL) && PlanesOf(r) > 1)
            clear |= D3D12_CLEAR_FLAG_STENCIL;
        if (!clear)
            return;
        // A DSV writes depth and stencil: every plane of its subresources.
        SubRange range = v->range;
        range.firstPlane = 0;
        range.planeCount = PlanesOf(r);
        Require(r, range, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        FlushBarriers();
        if (ID3D12GraphicsCommandList* list = List())
            list->ClearDepthStencilView(v->Descriptor(), static_cast<D3D12_CLEAR_FLAGS>(clear), depth, stencil, 0, nullptr);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in ClearDepthStencilView";
        LogOnce(kFormat, kFormat);
    }
}

namespace
{
template <class T>
void ClearUav(Context& c, ID3D11UnorderedAccessView* uav, const T values[4])
{
    View* v = View::From(uav);
    Resource* r = v ? v->resource->Actual() : nullptr;
    if (!r || !r->native || !values)
        return;
    const UnorderedAccessView* view = static_cast<UnorderedAccessView*>(uav);
    const D3D12_UNORDERED_ACCESS_VIEW_DESC& nd = view->nativeDesc;
    const bool untypedBuffer = nd.ViewDimension == D3D12_UAV_DIMENSION_BUFFER &&
        (nd.Format == DXGI_FORMAT_UNKNOWN || (nd.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW));
    if constexpr (!std::is_same_v<T, UINT>)
    {
        // D3D11 drops float clears of raw and structured views.
        if (untypedBuffer)
        {
            static constexpr char kFormat[] = "ClearUnorderedAccessViewFloat of a raw or structured buffer view ignored (D3D11 rule)";
            LogOnce(kFormat, kFormat);
            return;
        }
    }
    // Structured views clear through their raw alias.
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu = view->rawAlias.ptr ? view->rawAlias : v->cpu;
    c.Require(r, v->range, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // D3D11 orders the clear after earlier UAV work too.
    if (c.applied.uavWritten)
        c.UavBarrier();
    c.FlushBarriers();
    ID3D12GraphicsCommandList* list = c.List();
    if (!list)
        return;
    // The clear needs the view both in the shader-visible heap and in a CPU-only heap.
    Device* dev = c.device();
    const u32 index = c.descriptors.Allocate(1, dev->frame.load(std::memory_order_acquire));
    dev->d12->CopyDescriptorsSimple(1, dev->gpuRing.Cpu(index), cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EnsureHeaps(c, list);
    if constexpr (std::is_same_v<T, UINT>)
    {
        // Raw and structured views take the first value only (D3D11).
        const UINT first[4] = {values[0], values[0], values[0], values[0]};
        list->ClearUnorderedAccessViewUint(dev->gpuRing.Gpu(index), cpu, r->native, untypedBuffer ? first : values, 0, nullptr);
    }
    else
        list->ClearUnorderedAccessViewFloat(dev->gpuRing.Gpu(index), cpu, r->native, values, 0, nullptr);
    // D3D11 orders the clear before later UAV work on the view.
    c.UavBarrier();
}
} // namespace

void STDMETHODCALLTYPE Context::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* uav, const UINT values[4])
{
    try
    {
        ClearUav(*this, uav, values);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in ClearUnorderedAccessViewUint";
        LogOnce(kFormat, kFormat);
    }
}

void STDMETHODCALLTYPE Context::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* uav, const FLOAT values[4])
{
    try
    {
        ClearUav(*this, uav, values);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in ClearUnorderedAccessViewFloat";
        LogOnce(kFormat, kFormat);
    }
}

void STDMETHODCALLTYPE Context::GenerateMips(ID3D11ShaderResourceView*)
{
    static constexpr char kFormat[] = "GenerateMips is not implemented (the renderer builds mips on the CPU)";
    LogOnce(kFormat, kFormat);
}

void STDMETHODCALLTYPE Context::SetResourceMinLOD(ID3D11Resource*, FLOAT)
{
}

FLOAT STDMETHODCALLTYPE Context::GetResourceMinLOD(ID3D11Resource*)
{
    return 0.0f;
}

void STDMETHODCALLTYPE Context::ResolveSubresource(ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format)
{
    if (recordCompute)
    {
        static constexpr char kFormat[] = "! compute recording (async job): ResolveSubresource dropped";
        LogOnce(kFormat, kFormat);
        return;
    }
    try
    {
        Resource* d = ActualOf(dst);
        Resource* s = ActualOf(src);
        if (!d || !s || !d->native || !s->native || d->IsBuffer() || s->IsBuffer())
            return;
        if (dstSub >= MipsOf(d) * SlicesOf(d) || srcSub >= MipsOf(s) * SlicesOf(s))
            return;
        Require(s, RangeOfSub(s, srcSub), D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        Require(d, RangeOfSub(d, dstSub), D3D12_RESOURCE_STATE_RESOLVE_DEST);
        FlushBarriers();
        if (ID3D12GraphicsCommandList* list = List())
            list->ResolveSubresource(d->native, dstSub, s->native, srcSub, format);
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in ResolveSubresource";
        LogOnce(kFormat, kFormat);
    }
}

// ================================================================================================
// Queries
// ================================================================================================

namespace
{
bool IsOcclusion(D3D11_QUERY type)
{
    return type == D3D11_QUERY_OCCLUSION || type == D3D11_QUERY_OCCLUSION_PREDICATE;
}

D3D12_QUERY_TYPE NativeQueryType(D3D11_QUERY type)
{
    switch (type)
    {
    case D3D11_QUERY_OCCLUSION_PREDICATE: return D3D12_QUERY_TYPE_BINARY_OCCLUSION;
    case D3D11_QUERY_TIMESTAMP: return D3D12_QUERY_TYPE_TIMESTAMP;
    default: return D3D12_QUERY_TYPE_OCCLUSION;
    }
}
} // namespace

void STDMETHODCALLTYPE Context::Begin(ID3D11Asynchronous* async)
{
    if (!async)
        return;
    if (m_deferred)
    {
        static constexpr char kFormat[] = "queries on deferred contexts are not supported; Begin ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    Query* q = AsQuery(async);
    const D3D11_QUERY type = q->m_desc.Query;
    if (IsOcclusion(type))
    {
        if (q->active || !q->slot.heap)
            return;
        ID3D12GraphicsCommandList* list = List();
        if (!list)
            return;
        // Parts of the previous run: the GPU may still resolve into them.
        if (!q->parts.empty())
            FreeSlotsLater(m_device, q->parts);
        try
        {
            activeQueries.push_back(q);
        }
        catch (const std::bad_alloc&)
        {
            static constexpr char kFormat[] = "out of memory tracking an occlusion query; Begin ignored";
            LogOnce(kFormat, kFormat);
            return;
        }
        q->AddRef();
        list->BeginQuery(q->slot.heap, NativeQueryType(type), q->slot.index);
        q->active = true;
        q->fence = ~0ull;
    }
    else if (type == D3D11_QUERY_TIMESTAMP_DISJOINT)
    {
        q->active = true;
        q->fence = ~0ull;
    }
}

void STDMETHODCALLTYPE Context::End(ID3D11Asynchronous* async)
{
    if (!async)
        return;
    if (m_deferred)
    {
        static constexpr char kFormat[] = "queries on deferred contexts are not supported; End ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    Query* q = AsQuery(async);
    const D3D11_QUERY type = q->m_desc.Query;
    if (IsOcclusion(type) || type == D3D11_QUERY_TIMESTAMP)
    {
        const bool timestamp = type == D3D11_QUERY_TIMESTAMP;
        // An occlusion query ended without Begin records nothing (D3D11 treats it as an error).
        if (q->slot.heap && (timestamp || q->active))
        {
            if (ID3D12GraphicsCommandList* list = List())
            {
                const D3D12_QUERY_TYPE native = NativeQueryType(type);
                list->EndQuery(q->slot.heap, native, q->slot.index);
                QueueResolve(*this, list, q->slot, native);
            }
        }
        if (!timestamp && q->active)
        {
            const auto it = std::find(activeQueries.begin(), activeQueries.end(), q);
            if (it != activeQueries.end())
            {
                activeQueries.erase(it);
                q->Release();
            }
        }
    }
    else if (type != D3D11_QUERY_EVENT && type != D3D11_QUERY_TIMESTAMP_DISJOINT)
    {
        static constexpr char kFormat[] = "query type %u records nothing; its data reads as zeros";
        LogOnce(kFormat, kFormat, unsigned(type));
    }
    q->active = false;
    // The fence of the submission carrying the End is assigned by Submit.
    try
    {
        endedQueries.push_back(q);
        q->AddRef();
        q->fence = 0;
    }
    catch (const std::bad_alloc&)
    {
        // Untracked: the next submission's fence is conservative enough for any reader.
        q->fence = m_device->fence.NextValue();
    }
    // An event marks this point of the frame: the engine's CPU/GPU throttle waits for the event it
    // ended one frame earlier. Carried by the frame's single submission at Present, it would make
    // every frame wait for the whole previous one; its own submission keeps a frame of overlap.
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
    if (m_device->removed)
        return DXGI_ERROR_DEVICE_REMOVED;
    // D3D11 refuses a query that was never ended or is between Begin and End.
    if (q->fence == ~0ull || q->active)
        return DXGI_ERROR_INVALID_CALL;
    if (!q->fence)
    {
        if (flags & D3D11_ASYNC_GETDATA_DONOTFLUSH)
            return S_FALSE;
        Submit();
        if (!q->fence || q->fence == ~0ull)
            return S_FALSE;
    }
    if (!m_device->fence.IsComplete(q->fence))
        return S_FALSE;
    if (!data)
        return S_OK;

    u64 result = q->slot.result ? *q->slot.result : 0;
    for (const QueryHeapPool::Slot& part : q->parts)
        result += part.result ? *part.result : 0;
    switch (q->m_desc.Query)
    {
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_TIMESTAMP: std::memcpy(data, &result, sizeof(result)); break;
    case D3D11_QUERY_OCCLUSION_PREDICATE:
    {
        const BOOL visible = result != 0;
        std::memcpy(data, &visible, sizeof(visible));
        break;
    }
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        disjoint.Frequency = m_device->timestampFrequency;
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
        static constexpr char kFormat[] = "ExecuteCommandList on a deferred context ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    try
    {
        auto* cl = static_cast<CommandList*>(list);
        if (!cl->list)
        {
            static constexpr char kFormat[] = "a command list was executed twice; the repeat is skipped";
            LogOnce(kFormat, kFormat);
        }
        else if (cl->type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
        {
            // A compute recording cannot run on the direct queue: it runs as a job joined at once.
            static constexpr char kFormat[] = "! ExecuteCommandList of a compute recording: it runs as an async job joined at once";
            LogOnce(kFormat, kFormat);
            AsyncJob* job = AsyncJobFromCommandList(m_device, "ExecuteCommandList", list);
            ForkJob(job);
            JoinJob(job, false);
        }
        else
        {
            // A list that only read a resource recorded no barrier for it: any read state holding its
            // bits will do (a read-shared loan's widened state included), and the tracked state stays.
            const auto onlyRead = [](const CommandList::Requirement& req) { return !req.transitioned && IsReadOnly(req.first); };
            // Loans first. A list that transitions a read-shared resource would change it while the job
            // reads it. A join closes the own list at its marker and decays the buffers there, so the
            // entry transitions below all go after the last join.
            for (const CommandList::Requirement& req : cl->requirements)
            {
                if (req.transitioned)
                    JoinLent(req.resource);
                else if (req.subresource == kAllSubresources)
                    GuardLent(req.resource, nullptr, req.first, !onlyRead(req));
                else
                {
                    const SubRange range = RangeOfSub(req.resource, req.subresource);
                    GuardLent(req.resource, &range, req.first, !onlyRead(req));
                }
            }
            // Entry states of the list: exact where its first barriers start from them. The transitions
            // go to the end of the immediate list, which executes right before it.
            for (const CommandList::Requirement& req : cl->requirements)
            {
                if (req.subresource == kAllSubresources)
                    RequireTracked(req.resource, nullptr, req.first, !onlyRead(req), barriers);
                else
                {
                    const SubRange range = RangeOfSub(req.resource, req.subresource);
                    RequireTracked(req.resource, &range, req.first, !onlyRead(req), barriers);
                }
            }
            FlushBarriers();
            // The own list so far, then the deferred one, wait for the next submission together.
            {
                std::lock_guard queuedLock(m_queuedLock);
                m_queued.reserve(m_queued.size() + 2);
                pendingReadback.reserve(pendingReadback.size() + cl->pendingReadback.size());
                if (ID3D12GraphicsCommandList* own = CloseOwn())
                    m_queued.push_back({own, this});
                m_queued.push_back({cl->list, cl->recordedBy ? cl->recordedBy : this});
            }

            for (const CommandList::Requirement& req : cl->requirements)
            {
                if (!onlyRead(req))
                    SetTracked(req.resource, req.subresource, req.last);
            }
            for (const CommandList::Requirement& req : cl->requirements)
                Collapse(req.resource->states);
            // Its staging copies get the fence of that submission, like the own list's; until then a
            // Map of them has to submit first.
            for (Resource* r : cl->pendingReadback)
                r->gpuWritePending = true;
            pendingReadback.insert(pendingReadback.end(), cl->pendingReadback.begin(), cl->pendingReadback.end());
            cl->pendingReadback.clear();
            cl->list = nullptr;
        }
    }
    catch (const std::bad_alloc&)
    {
        static constexpr char kFormat[] = "out of memory in ExecuteCommandList";
        LogOnce(kFormat, kFormat);
    }
    // D3D11: ExecuteCommandList(FALSE) leaves the immediate context in its default state.
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
        ID3D12GraphicsCommandList* list = List();
        if (!list)
            return m_device->removed ? DXGI_ERROR_DEVICE_REMOVED : E_OUTOFMEMORY;
        FlushBarriers();

        auto* cl = new CommandList(m_device);
        try
        {
            tracker.Harvest(cl->requirements);
            cl->pendingReadback.assign(pendingReadback.begin(), pendingReadback.end());
        }
        catch (const std::bad_alloc&)
        {
            cl->Release();
            throw;
        }
        pendingReadback.clear();

        RetagOpenSlot(m_allocators[m_listType], m_device->frame.load(std::memory_order_acquire));
        ID3D12CommandAllocator* allocator = OpenSlotAllocator(m_allocators[m_listType]);
        if (annotation)
            annotation->Suspend(list);
        if (recordCompute)
            AsyncRecordingClosing(list, cl);
        cl->type = list->GetType();
        SetOpenList(nullptr, false);
        const HRESULT hr = list->Close();
        if (FAILED(hr))
        {
            ReportFailure(m_device, hr, "ID3D12GraphicsCommandList::Close (deferred)");
            for (Resource* r : cl->pendingReadback)
                ReleaseResource(r);
            cl->pendingReadback.clear();
            cl->Release();
            ReturnList(list);
            if (!restoreState)
                ClearState();
            return hr;
        }
        cl->list = list;
        cl->allocator = allocator;
        cl->recordedBy = this;
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
    ReleaseBindings(*this);
    InvalidateApplied();
}

void STDMETHODCALLTYPE Context::Flush()
{
    if (!m_deferred)
        Submit();
}
} // namespace xrd3d12
