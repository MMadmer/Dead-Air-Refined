// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Context, part 1: the D3D11 binding API (Set*/Get*), the D3D11 input/output hazard rules, and the
// translation of the bound state into D3D12 work at every draw and dispatch (root signature, pipeline
// state, root CBVs, descriptor tables, vertex/index buffers, output merger, resource states).

#include "d12_layer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

namespace xrd3d12
{
namespace
{
constexpr u32 kGraphicsStages = u32(Stage::CS); // VS, HS, DS, GS, PS
constexpr u32 kCS = u32(Stage::CS);
constexpr u32 kPS = u32(Stage::PS);

// D3D12 always scissors: a rect this large is D3D11's "scissor test off".
constexpr LONG kNoScissor = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
constexpr UINT kMaxCbvSize = D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16;

static_assert(sizeof(D3D11_VIEWPORT) == sizeof(D3D12_VIEWPORT), "viewports are passed through");
static_assert(sizeof(D3D11_RECT) == sizeof(D3D12_RECT), "scissor rects are passed through");

// Root parameter indices of one stage.
struct StageRoot
{
    u32 cbv0;
    u32 cbvTable;
    u32 srv;
    u32 smp;
    u32 rootCbvs; // slots below this are root CBVs, the rest of the 14 go through the table
};

// Indexed by Stage.
constexpr StageRoot kStageRoot[kStageCount] = {
    {GfxRoot::VS_CBV0, GfxRoot::VS_CBV_TABLE, GfxRoot::VS_SRV, GfxRoot::VS_SMP, kRootCBVsVS},
    {GfxRoot::HS_CBV0, GfxRoot::HS_CBV_TABLE, GfxRoot::HS_SRV, GfxRoot::HS_SMP, kRootCBVsOther},
    {GfxRoot::DS_CBV0, GfxRoot::DS_CBV_TABLE, GfxRoot::DS_SRV, GfxRoot::DS_SMP, kRootCBVsOther},
    {GfxRoot::GS_CBV0, GfxRoot::GS_CBV_TABLE, GfxRoot::GS_SRV, GfxRoot::GS_SMP, kRootCBVsOther},
    {GfxRoot::PS_CBV0, GfxRoot::PS_CBV_TABLE, GfxRoot::PS_SRV, GfxRoot::PS_SMP, kRootCBVsPS},
    {CsRoot::CBV0, CsRoot::CBV_TABLE, CsRoot::SRV, CsRoot::SMP, kRootCBVsCS},
};

// The format string doubles as the once-key.
template <class... Args>
void Once(const char* fmt, Args... args)
{
    LogOnce(fmt, fmt, args...);
}

// Clamps [start, start + num) to the layer's slot limit. False when nothing is left to bind.
bool ClampRange(UINT& start, UINT& num, u32 limit, const char* what)
{
    if (u64(start) + num <= limit)
        return num != 0;
    LogOnce(what, "%s slots %u..%llu are beyond the layer limit %u; clamped", what, start,
        u64(start) + num - 1, limit);
    if (start >= limit)
        return false;
    num = limit - start;
    return true;
}

// D3D11 binding: AddRef the new object, Release the replaced one.
template <class T>
bool Assign(T*& slot, T* value)
{
    if (slot == value)
        return false;
    if (value)
        value->AddRef();
    T* old = slot;
    slot = value;
    if (old)
        old->Release();
    return true;
}

// Every interface the engine binds was created by the layer.
inline View* AsView(ID3D11ShaderResourceView* v) { return static_cast<ShaderResourceView*>(v); }
inline View* AsView(ID3D11RenderTargetView* v) { return static_cast<RenderTargetView*>(v); }
inline View* AsView(ID3D11DepthStencilView* v) { return static_cast<DepthStencilView*>(v); }
inline View* AsView(ID3D11UnorderedAccessView* v) { return static_cast<UnorderedAccessView*>(v); }
inline Resource* AsResource(ID3D11Buffer* b) { return static_cast<Buffer*>(b); }
inline Resource* ActualOf(Resource* r) { return r->IsBackBufferProxy() ? r->Actual() : r; }

inline bool Overlaps(const SubRange& a, const SubRange& b)
{
    return a.firstMip < b.firstMip + b.mipCount && b.firstMip < a.firstMip + a.mipCount &&
        a.firstSlice < b.firstSlice + b.sliceCount && b.firstSlice < a.firstSlice + a.sliceCount &&
        a.firstPlane < b.firstPlane + b.planeCount && b.firstPlane < a.firstPlane + a.planeCount;
}

// `target` must already be resolved through ActualOf.
inline bool Aliases(View* v, Resource* target, const SubRange& range)
{
    return ActualOf(v->resource) == target && Overlaps(v->range, range);
}

// Planes a DSV writes: D3D11's read-only flags take the depth (plane 0) or stencil (plane 1) plane
// out. False when the view writes nothing.
bool DsvWriteRange(const View* v, SubRange& out)
{
    out = v->range;
    u32 first = out.firstPlane;
    u32 end = out.firstPlane + out.planeCount;
    if ((v->dsvFlags & D3D11_DSV_READ_ONLY_DEPTH) && first == 0)
        first = 1;
    if ((v->dsvFlags & D3D11_DSV_READ_ONLY_STENCIL) && end > 1)
        end = 1;
    if (first >= end)
        return false;
    out.firstPlane = first;
    out.planeCount = end - first;
    return true;
}

// Topology-dependent PSO fields.
struct TopologyKey
{
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut;
    bool operator==(const TopologyKey& o) const { return type == o.type && cut == o.cut; }
};

TopologyKey KeyOf(D3D11_PRIMITIVE_TOPOLOGY t, DXGI_FORMAT indexFormat)
{
    TopologyKey key{D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED};
    bool strip = false;
    switch (t)
    {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:
        key.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
        key.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
        key.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        strip = true;
        break;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
        key.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
        key.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        strip = true;
        break;
    default:
        if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
            key.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        break;
    }
    // D3D11 always cuts indexed strips at the all-ones index; D3D12 needs it in the PSO.
    if (strip)
    {
        key.cut = indexFormat == DXGI_FORMAT_R32_UINT ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                      : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    }
    return key;
}

// Output-merger fields of the PSO.
struct OutputSignature
{
    u32 count = 0; // highest bound RTV + 1
    DXGI_FORMAT rt[kMaxRTVs]{};
    DXGI_FORMAT ds = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample{1, 0};

    bool operator==(const OutputSignature& o) const
    {
        return count == o.count && ds == o.ds && sample.Count == o.sample.Count && sample.Quality == o.sample.Quality &&
            std::memcmp(rt, o.rt, sizeof(rt)) == 0;
    }
};

u32 BoundRtvCount(const Context& c)
{
    u32 count = 0;
    const u32 n = (std::min)(c.numRTVs, kMaxRTVs);
    for (u32 i = 0; i < n; ++i)
    {
        if (c.rtvs[i])
            count = i + 1;
    }
    return count;
}

OutputSignature SignatureOf(const Context& c)
{
    OutputSignature s;
    Resource* first = nullptr;
    s.count = BoundRtvCount(c);
    for (u32 i = 0; i < s.count; ++i)
    {
        if (!c.rtvs[i])
            continue;
        View* v = AsView(c.rtvs[i]);
        s.rt[i] = v->format;
        if (!first)
            first = ActualOf(v->resource);
    }
    if (c.dsv)
    {
        View* v = AsView(c.dsv);
        s.ds = v->format;
        if (!first)
            first = ActualOf(v->resource);
    }
    if (first)
        s.sample = first->nativeDesc.SampleDesc;
    return s;
}

void FillPsoDesc(const Context& c, GraphicsPsoDesc& d)
{
    d.vs = c.stages[u32(Stage::VS)].shader;
    d.hs = c.stages[u32(Stage::HS)].shader;
    d.ds = c.stages[u32(Stage::DS)].shader;
    d.gs = c.stages[u32(Stage::GS)].shader;
    d.ps = c.stages[kPS].shader;
    d.inputLayout = c.inputLayout ? static_cast<InputLayout*>(c.inputLayout) : nullptr;
    d.blend = c.blendState ? static_cast<BlendState*>(c.blendState) : nullptr;
    d.depth = c.depthState ? static_cast<DepthStencilState*>(c.depthState) : nullptr;
    d.raster = c.rasterState ? static_cast<RasterizerState*>(c.rasterState) : nullptr;
    d.sampleMask = c.sampleMask;
    const TopologyKey key = KeyOf(c.topology, c.indexFormat);
    d.topology = key.type;
    d.stripCut = key.cut;
    const OutputSignature s = SignatureOf(c);
    d.numRTVs = s.count;
    for (u32 i = 0; i < kMaxRTVs; ++i)
        d.rtvFormats[i] = s.rt[i];
    d.dsvFormat = s.ds;
    d.sample = s.sample;
    // A mesh shader in the VS slot makes it a mesh pipeline (no input layout, no topology).
    if (d.vs && d.vs->kind == kKindMesh)
    {
        d.meshKind = kKindMesh;
        d.inputLayout = nullptr;
        d.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    }
}

bool ScissorEnabled(ID3D11RasterizerState* state)
{
    return state && static_cast<RasterizerState*>(state)->scissorEnable;
}

// Descriptor tables depend on the counts and declared dimensions of the shader, not its identity.
bool SameSrvLayout(const Shader* a, const Shader* b)
{
    const u32 na = a ? a->bindings.srvCount : 0;
    const u32 nb = b ? b->bindings.srvCount : 0;
    if (na != nb)
        return false;
    return !na || std::memcmp(a->bindings.srvDims, b->bindings.srvDims, (std::min)(na, kMaxSRVs) * sizeof(D3D12_SRV_DIMENSION)) == 0;
}

bool SameSamplerLayout(const Shader* a, const Shader* b)
{
    return (a ? a->bindings.samplerCount : 0) == (b ? b->bindings.samplerCount : 0);
}

bool SameUavLayout(const Shader* a, const Shader* b)
{
    const u32 na = a ? a->bindings.uavCount : 0;
    const u32 nb = b ? b->bindings.uavCount : 0;
    if (na != nb)
        return false;
    return !na || std::memcmp(a->bindings.uavDims, b->bindings.uavDims, sizeof(a->bindings.uavDims)) == 0;
}

inline u32 NullSrvIndex(D3D12_SRV_DIMENSION dim)
{
    return dim != D3D12_SRV_DIMENSION_UNKNOWN && u32(dim) <= u32(D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
        ? u32(dim)
        : u32(D3D12_SRV_DIMENSION_TEXTURE2D);
}

inline u32 NullUavIndex(D3D12_UAV_DIMENSION dim)
{
    return dim != D3D12_UAV_DIMENSION_UNKNOWN && u32(dim) <= u32(D3D12_UAV_DIMENSION_TEXTURE3D)
        ? u32(dim)
        : u32(D3D12_UAV_DIMENSION_TEXTURE2D);
}

inline D3D12_GPU_VIRTUAL_ADDRESS CbAddress(ID3D11Buffer* b, D3D12_GPU_VIRTUAL_ADDRESS zero)
{
    return b ? AsResource(b)->BufferAddress() : zero;
}

inline void SetRootCbv(ID3D12GraphicsCommandList* list, bool compute, u32 param, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    if (compute)
        list->SetComputeRootConstantBufferView(param, address);
    else
        list->SetGraphicsRootConstantBufferView(param, address);
}

inline void SetRootTable(ID3D12GraphicsCommandList* list, bool compute, u32 param, D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    if (compute)
        list->SetComputeRootDescriptorTable(param, table);
    else
        list->SetGraphicsRootDescriptorTable(param, table);
}

// Root arguments are undefined after a root signature change: everything of the stage goes again.
void ResetStageRoot(StageBindings& sb)
{
    std::memset(sb.cbAppliedAddress, 0, sizeof(sb.cbAppliedAddress));
    std::memset(sb.cbAppliedVersion, 0, sizeof(sb.cbAppliedVersion));
    sb.cbDirty = true;
    sb.srvDirty = true;
    sb.samplerDirty = true;
}

// SetDescriptorHeaps: tables recorded against earlier heaps are gone, and a new heap binding means a
// new list or foreign native code - the whole applied state is re-established.
void ApplyHeaps(Context& c, ID3D12GraphicsCommandList* list)
{
    Context::Applied& a = c.applied;
    Device* dev = c.device();
    // A replaced sampler heap is bound like a new list's heaps.
    if (!a.heapsDirty && c.samplerGeneration == dev->samplerTables.Generation())
        return;
    u32 generation = 0;
    ID3D12DescriptorHeap* heaps[2] = {dev->gpuRing.Heap(), dev->samplerTables.Current(generation)};
    list->SetDescriptorHeaps(2, heaps);
    c.samplerGeneration = generation;
    a.heapsDirty = false;
    a.rootSignatureGraphics = false;
    a.rootSignatureCompute = false;
    a.rootSignatureMesh = false;
    a.pso = nullptr;
    a.psoHash = 0;
    a.psoDirty = true;
    a.vbDirty = a.ibDirty = true;
    a.rtDirty = a.viewportDirty = a.scissorDirty = true;
    a.stencilRefDirty = a.blendFactorDirty = a.topologyDirty = true;
    a.psUavDirty = a.csUavDirty = true;
    for (StageBindings& sb : c.stages)
        ResetStageRoot(sb);
}

// Copies `count` CPU descriptors into fresh ring space; returns the table's GPU handle.
D3D12_GPU_DESCRIPTOR_HANDLE CopyToRing(Context& c, const D3D12_CPU_DESCRIPTOR_HANDLE* src, u32 count)
{
    Device* dev = c.device();
    const u32 base = c.descriptors.Allocate(count, dev->frame.load(std::memory_order_relaxed));
    const D3D12_CPU_DESCRIPTOR_HANDLE dst = dev->gpuRing.Cpu(base);
    const UINT size = count;
    dev->d12->CopyDescriptors(1, &dst, &size, count, src, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return dev->gpuRing.Gpu(base);
}

D3D12_CPU_DESCRIPTOR_HANDLE SrvDescriptor(View* v, D3D12_SRV_DIMENSION declared)
{
    if (declared != D3D12_SRV_DIMENSION_UNKNOWN && declared != v->srvDimension)
    {
        bool ok = false;
        const D3D12_CPU_DESCRIPTOR_HANDLE adapted = v->AdaptedSrv(declared, ok);
        if (ok)
            return adapted;
        Once("an SRV of dimension %d is bound to a slot declared with dimension %d and cannot be adapted",
            int(v->srvDimension), int(declared));
    }
    return v->Descriptor();
}

// The CBV table covers every non-root slot: tier-2 hardware wants all CBVs of a declared range valid.
// Unbound slots read the zero buffer, as unbound D3D11 constant buffers read zeros.
void BuildCbvTable(Context& c, ID3D12GraphicsCommandList* list, StageBindings& sb, const StageRoot& root, bool compute)
{
    Device* dev = c.device();
    const u32 first = root.rootCbvs;
    const u32 size = kMaxCBs - first;
    const u32 base = c.descriptors.Allocate(size, dev->frame.load(std::memory_order_relaxed));
    for (u32 i = 0; i < size; ++i)
    {
        const u32 slot = first + i;
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        if (sb.cb[slot])
        {
            const Resource* r = AsResource(sb.cb[slot]);
            desc.BufferLocation = r->BufferAddress();
            desc.SizeInBytes = UINT((std::min<u64>)((r->byteWidth + 255) & ~u64(255), kMaxCbvSize));
        }
        else
        {
            desc.BufferLocation = dev->nulls.zeroCb;
            desc.SizeInBytes = kMaxCbvSize;
        }
        dev->d12->CreateConstantBufferView(&desc, dev->gpuRing.Cpu(base + i));
        sb.cbAppliedAddress[slot] = desc.BufferLocation;
    }
    SetRootTable(list, compute, root.cbvTable, dev->gpuRing.Gpu(base));
    dev->Count(Device::StatCbvTables);
}

// The UAV table always holds all kMaxUAVs descriptors: tier-2 hardware wants all UAVs of a declared
// range valid, used or not.
D3D12_GPU_DESCRIPTOR_HANDLE BuildUavTable(Context& c, ID3D11UnorderedAccessView* const* uavs, const ShaderBindings& b)
{
    const NullDescriptors& nulls = c.device()->nulls;
    D3D12_CPU_DESCRIPTOR_HANDLE src[kMaxUAVs];
    for (u32 slot = 0; slot < kMaxUAVs; ++slot)
        src[slot] = uavs[slot] ? AsView(uavs[slot])->Descriptor() : nulls.uav[NullUavIndex(b.uavDims[slot])];
    return CopyToRing(c, src, kMaxUAVs);
}

// Root CBVs, CBV table, SRV table and sampler table of one stage that has a shader. False when the
// sampler table came from a heap newer than the bound one (the heap filled up during this call).
bool ApplyStage(Context& c, ID3D12GraphicsCommandList* list, u32 stage, bool compute)
{
    StageBindings& sb = c.stages[stage];
    const ShaderBindings& b = sb.shader->bindings;
    const StageRoot& root = kStageRoot[stage];
    Device* dev = c.device();

    // Constant buffers: DISCARD maps rename dynamic buffers without a rebind, so the address of every
    // used slot is compared at every draw.
    if (b.cbMask)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS zero = dev->nulls.zeroCb;
        const u32 rootMask = u32(b.cbMask) & ((1u << root.rootCbvs) - 1);
        for (u32 m = rootMask; m; m &= m - 1)
        {
            const u32 slot = u32(std::countr_zero(m));
            const D3D12_GPU_VIRTUAL_ADDRESS address = CbAddress(sb.cb[slot], zero);
            if (address != sb.cbAppliedAddress[slot])
            {
                SetRootCbv(list, compute, root.cbv0 + slot, address);
                dev->Count(Device::StatRootCbv);
                sb.cbAppliedAddress[slot] = address;
            }
        }
        const u32 tableMask = u32(b.cbMask) & ~((1u << root.rootCbvs) - 1);
        for (u32 m = tableMask; m; m &= m - 1)
        {
            const u32 slot = u32(std::countr_zero(m));
            if (slot < kMaxCBs && CbAddress(sb.cb[slot], zero) != sb.cbAppliedAddress[slot])
            {
                BuildCbvTable(c, list, sb, root, compute);
                break;
            }
        }
    }
    sb.cbDirty = false;

    if (sb.srvDirty)
    {
        const u32 count = (std::min)(u32(b.srvCount), kMaxSRVs);
        if (count)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE src[kMaxSRVs];
            for (u32 slot = 0; slot < count; ++slot)
            {
                const D3D12_SRV_DIMENSION declared = b.srvDims[slot];
                ID3D11ShaderResourceView* view = sb.srv[slot];
                src[slot] = view ? SrvDescriptor(AsView(view), declared) : dev->nulls.srv[NullSrvIndex(declared)];
            }
            SetRootTable(list, compute, root.srv, CopyToRing(c, src, count));
            dev->Count(Device::StatSrvTables);
            dev->Count(Device::StatSrvDescriptors, count);
        }
        sb.srvDirty = false;
    }

    if (sb.samplerDirty)
    {
        const u32 count = (std::min)(u32(b.samplerCount), kMaxSamplers);
        if (count)
        {
            u32 ids[kMaxSamplers];
            D3D12_CPU_DESCRIPTOR_HANDLE cpu[kMaxSamplers];
            for (u32 slot = 0; slot < count; ++slot)
            {
                if (const SamplerState* s = static_cast<SamplerState*>(sb.samplers[slot]))
                {
                    ids[slot] = s->id;
                    cpu[slot] = s->cpu;
                }
                else
                {
                    ids[slot] = 0;
                    cpu[slot] = dev->nulls.sampler;
                }
            }
            u32 generation = 0;
            const D3D12_GPU_DESCRIPTOR_HANDLE table = dev->samplerTables.Get(ids, cpu, count, generation);
            if (generation != c.samplerGeneration)
            {
                c.applied.heapsDirty = true;
                return false;
            }
            SetRootTable(list, compute, root.smp, table);
            dev->Count(Device::StatSamplerTables);
        }
        sb.samplerDirty = false;
    }
    return true;
}

// Puts a texture range into `state`. The immediate context reads the tracked state directly and
// calls the tracker only when it differs, so the check is cheap enough to run at every draw; that
// also covers every state change made between draws (copies, clears, resolves, native sections,
// Present) without any bookkeeping.
inline void EnsureState(Context& c, Resource* r, const SubRange& range, D3D12_RESOURCE_STATES state)
{
    // A resource lent to an async job goes through the guard.
    if (!c.IsDeferred() && r->states.uniform && r->states.all == state && !r->lentTo)
        return;
    c.Require(r, range, state);
}

// Tracked (GPU-written or oversized) buffers; static and dynamic buffers are never tracked.
inline void EnsureBuffer(Context& c, ID3D11Buffer* b, D3D12_RESOURCE_STATES state)
{
    if (!b)
        return;
    Resource* r = AsResource(b);
    if (r->native)
        c.RequireAll(r, state);
}

// State a shader read of `r` needs; a read-only DSV of the same resource adds DEPTH_READ.
struct ReadOnlyDepth
{
    Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

void EnsureShaderInputs(Context& c, const StageBindings& sb, const ReadOnlyDepth& rod)
{
    const ShaderBindings& b = sb.shader->bindings;
    // A compute recording (async job) reads in the compute state only.
    const D3D12_RESOURCE_STATES read = c.recordCompute ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : kStateShaderRead;
    for (u32 m = b.srvMask; m; m &= m - 1)
    {
        const u32 slot = u32(std::countr_zero(m));
        ID3D11ShaderResourceView* view = sb.srv[slot];
        if (!view)
            continue;
        View* v = AsView(view);
        Resource* r = ActualOf(v->resource);
        if (!r->native)
            continue;
        if (r->IsBuffer())
            c.RequireAll(r, read);
        else
            EnsureState(c, r, v->range, r == rod.resource ? rod.state : read);
    }
    for (u32 m = b.cbMask; m; m &= m - 1)
    {
        const u32 slot = u32(std::countr_zero(m));
        if (slot < kMaxCBs)
            EnsureBuffer(c, sb.cb[slot], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
}

void EnsureUavs(Context& c, ID3D11UnorderedAccessView* const* uavs, u32 mask)
{
    for (u32 m = mask; m; m &= m - 1)
    {
        const u32 slot = u32(std::countr_zero(m));
        if (slot >= kMaxUAVs || !uavs[slot])
            continue;
        View* v = AsView(uavs[slot]);
        Resource* r = ActualOf(v->resource);
        if (!r->native)
            continue;
        if (r->IsBuffer())
            c.RequireAll(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        else
            EnsureState(c, r, v->range, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}

// Resource states of everything the draw reads or writes.
void EnsureDrawStates(Context& c)
{
    // Render targets and depth.
    const u32 rtCount = BoundRtvCount(c);
    for (u32 i = 0; i < rtCount; ++i)
    {
        if (!c.rtvs[i])
            continue;
        View* v = AsView(c.rtvs[i]);
        EnsureState(c, ActualOf(v->resource), v->range, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    ReadOnlyDepth rod;
    if (c.dsv)
    {
        View* v = AsView(c.dsv);
        Resource* r = ActualOf(v->resource);
        const UINT readOnly = v->dsvFlags & (D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL);
        if (!readOnly)
            EnsureState(c, r, v->range, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        else
        {
            // Read-only planes may be sampled at the same time: DEPTH_READ plus shader read.
            rod.resource = r;
            rod.state = D3D12_RESOURCE_STATE_DEPTH_READ;
            if (r->bindFlags & D3D11_BIND_SHADER_RESOURCE)
                rod.state |= kStateShaderRead;
            for (u32 p = v->range.firstPlane; p < v->range.firstPlane + v->range.planeCount; ++p)
            {
                SubRange plane = v->range;
                plane.firstPlane = p;
                plane.planeCount = 1;
                const bool planeReadOnly = (readOnly & (p == 0 ? D3D11_DSV_READ_ONLY_DEPTH : D3D11_DSV_READ_ONLY_STENCIL)) != 0;
                EnsureState(c, r, plane, planeReadOnly ? rod.state : D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
        }
    }

    // Shader inputs of every stage that runs.
    for (u32 s = 0; s < kGraphicsStages; ++s)
    {
        if (c.stages[s].shader)
            EnsureShaderInputs(c, c.stages[s], rod);
    }
    if (const Shader* ps = c.stages[kPS].shader)
        EnsureUavs(c, c.psUavs, ps->bindings.uavMask);

    // Input assembler (only GPU-written buffers are tracked).
    for (const Context::VertexBufferBinding& vb : c.vbs)
        EnsureBuffer(c, vb.buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    EnsureBuffer(c, c.indexBuffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    if (c.vrs)
        c.VrsRequire(); // [dx12:vrs] hook
}

void ApplyVertexBuffers(Context& c, ID3D12GraphicsCommandList* list)
{
    Context::Applied& a = c.applied;
    bool changed = a.vbDirty;
    u32 count = 0;
    for (u32 i = 0; i < kMaxVBs; ++i)
    {
        ID3D11Buffer* b = c.vbs[i].buffer;
        if (!b)
            continue;
        count = i + 1;
        const Resource* r = AsResource(b);
        if (r->usage == D3D11_USAGE_DYNAMIC && r->current.version != a.vbVersions[i])
            changed = true;
    }
    if (!changed)
        return;
    D3D12_VERTEX_BUFFER_VIEW views[kMaxVBs]{};
    for (u32 i = 0; i < count; ++i)
    {
        const Context::VertexBufferBinding& vb = c.vbs[i];
        if (!vb.buffer)
            continue;
        const Resource* r = AsResource(vb.buffer);
        if (vb.offset < r->byteWidth)
        {
            views[i].BufferLocation = r->BufferAddress() + vb.offset;
            views[i].SizeInBytes = UINT(r->byteWidth - vb.offset);
            views[i].StrideInBytes = vb.stride;
        }
        if (r->usage == D3D11_USAGE_DYNAMIC)
            a.vbVersions[i] = r->current.version;
    }
    // Nothing bound: slot 0 gets a null view.
    list->IASetVertexBuffers(0, (std::max)(count, 1u), views);
    a.vbDirty = false;
}

void ApplyIndexBuffer(Context& c, ID3D12GraphicsCommandList* list)
{
    Context::Applied& a = c.applied;
    const Resource* r = c.indexBuffer ? AsResource(c.indexBuffer) : nullptr;
    const bool renamed = r && r->usage == D3D11_USAGE_DYNAMIC && r->current.version != a.ibVersion;
    if (!a.ibDirty && !renamed)
        return;
    if (r)
    {
        D3D12_INDEX_BUFFER_VIEW view{};
        if (c.indexOffset < r->byteWidth)
        {
            view.BufferLocation = r->BufferAddress() + c.indexOffset;
            view.SizeInBytes = UINT(r->byteWidth - c.indexOffset);
        }
        view.Format = c.indexFormat;
        list->IASetIndexBuffer(&view);
        if (r->usage == D3D11_USAGE_DYNAMIC)
            a.ibVersion = r->current.version;
    }
    else
        list->IASetIndexBuffer(nullptr);
    a.ibDirty = false;
}

// Viewports, scissors, render targets, topology, stencil reference, blend factor.
void ApplyFixedFunction(Context& c, ID3D12GraphicsCommandList* list)
{
    Context::Applied& a = c.applied;
    Device* dev = c.device();

    if (a.rtDirty)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handles[kMaxRTVs];
        const u32 count = BoundRtvCount(c);
        for (u32 i = 0; i < count; ++i)
            handles[i] = c.rtvs[i] ? AsView(c.rtvs[i])->Descriptor() : dev->nulls.rtv;
        D3D12_CPU_DESCRIPTOR_HANDLE depth{};
        if (c.dsv)
            depth = AsView(c.dsv)->Descriptor();
        list->OMSetRenderTargets(count, count ? handles : nullptr, FALSE, c.dsv ? &depth : nullptr);
        a.rtDirty = false;
    }

    if (a.viewportDirty)
    {
        if (c.numViewports)
            list->RSSetViewports(c.numViewports, reinterpret_cast<const D3D12_VIEWPORT*>(c.viewports));
        else
        {
            // D3D11 without a viewport rasterizes nothing.
            Once("draw without a viewport");
            const D3D12_VIEWPORT none{};
            list->RSSetViewports(1, &none);
        }
    }
    if (a.viewportDirty || a.scissorDirty)
    {
        D3D12_RECT rects[kMaxViewports];
        const u32 count = (std::max)(c.numViewports, 1u);
        const bool enabled = ScissorEnabled(c.rasterState);
        // With scissoring on, a rect the last RSSetScissorRects did not define is empty (the D3D11
        // specification and NVIDIA's driver; Intel's draws everything). The engine always sets one.
        for (u32 i = 0; i < count; ++i)
        {
            if (!enabled)
                rects[i] = D3D12_RECT{0, 0, kNoScissor, kNoScissor};
            else
                rects[i] = i < c.numScissors ? c.scissors[i] : D3D12_RECT{0, 0, 0, 0};
        }
        list->RSSetScissorRects(count, rects);
        a.scissorDirty = false;
    }
    a.viewportDirty = false;

    if (a.topologyDirty)
    {
        list->IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY(c.topology));
        a.topologyDirty = false;
    }
    if (a.stencilRefDirty)
    {
        list->OMSetStencilRef(c.stencilRef);
        a.stencilRefDirty = false;
    }
    if (a.blendFactorDirty)
    {
        list->OMSetBlendFactor(c.blendFactor);
        a.blendFactorDirty = false;
    }
    if (a.vrsDirty)
        c.VrsApply(list); // [dx12:vrs] hook
}

// D3D11 keeps one output binding per subresource across both pipelines: a new CS UAV unbinds the
// render targets and OM UAVs it aliases, a new render target or OM UAV unbinds aliasing CS UAVs.
void UnbindCsUavsOf(Context& c, Resource* resource, const SubRange& range)
{
    Resource* target = ActualOf(resource);
    for (ID3D11UnorderedAccessView*& slot : c.csUavs)
    {
        if (slot && Aliases(AsView(slot), target, range))
        {
            ID3D11UnorderedAccessView* old = slot;
            slot = nullptr;
            old->Release();
            c.applied.csUavDirty = true;
        }
    }
}

void UnbindGraphicsOutputsOf(Context& c, Resource* resource, const SubRange& range)
{
    Resource* target = ActualOf(resource);
    const OutputSignature before = SignatureOf(c);
    bool changed = false;
    for (ID3D11RenderTargetView*& slot : c.rtvs)
    {
        if (slot && Aliases(AsView(slot), target, range))
        {
            ID3D11RenderTargetView* old = slot;
            slot = nullptr;
            old->Release();
            changed = true;
        }
    }
    for (ID3D11UnorderedAccessView*& slot : c.psUavs)
    {
        if (slot && Aliases(AsView(slot), target, range))
        {
            ID3D11UnorderedAccessView* old = slot;
            slot = nullptr;
            old->Release();
            c.applied.psUavDirty = true;
        }
    }
    if (!changed)
        return;
    c.applied.rtDirty = true;
    if (!(SignatureOf(c) == before))
        c.applied.psoDirty = true;
}

// ---- generic stage setters/getters ------------------------------------------------------------

void SetConstantBuffers(Context& c, Stage stage, UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    if (!ClampRange(start, num, kMaxCBs, "constant buffer"))
        return;
    StageBindings& sb = c.stages[u32(stage)];
    for (UINT i = 0; i < num; ++i)
    {
        if (Assign(sb.cb[start + i], buffers ? buffers[i] : nullptr))
            sb.cbDirty = true;
    }
}

void SetShaderResources(Context& c, Stage stage, UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    if (!ClampRange(start, num, kMaxSRVs, "shader resource"))
        return;
    StageBindings& sb = c.stages[u32(stage)];
    for (UINT i = 0; i < num; ++i)
    {
        ID3D11ShaderResourceView* view = views ? views[i] : nullptr;
        ID3D11ShaderResourceView*& slot = sb.srv[start + i];
        if (view == slot)
            continue;
        // D3D11 binds null instead of an input that aliases a bound output.
        if (view)
        {
            View* v = AsView(view);
            // Outputs of either pipeline count: D3D11 checks CS UAVs against PS inputs and back.
            if (c.IsBoundAsOutput(v->resource, v->range))
                view = nullptr;
        }
        if (Assign(slot, view))
            sb.srvDirty = true;
    }
}

void SetSamplers(Context& c, Stage stage, UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    if (!ClampRange(start, num, kMaxSamplers, "sampler"))
        return;
    StageBindings& sb = c.stages[u32(stage)];
    for (UINT i = 0; i < num; ++i)
    {
        if (Assign(sb.samplers[start + i], samplers ? samplers[i] : nullptr))
            sb.samplerDirty = true;
    }
}

void SetShader(Context& c, Stage stage, ID3D11DeviceChild* shader, UINT numInstances)
{
    if (numInstances)
        Once("shader class instances are not supported; ignored");
    StageBindings& sb = c.stages[u32(stage)];
    if (sb.shaderIface == shader)
        return;
    Shader* next = Shader::From(shader, stage);
    const Shader* prev = sb.shader; // alive: the context still holds its interface
    if (!SameSrvLayout(prev, next))
        sb.srvDirty = true;
    if (!SameSamplerLayout(prev, next))
        sb.samplerDirty = true;
    sb.cbDirty = true;
    if (stage == Stage::CS)
    {
        if (!SameUavLayout(prev, next))
            c.applied.csUavDirty = true;
    }
    else
    {
        c.applied.psoDirty = true;
        if (stage == Stage::PS && !SameUavLayout(prev, next))
            c.applied.psUavDirty = true;
    }
    sb.shader = next;
    Assign(sb.shaderIface, shader);
}

template <class T>
void GetRange(T* const* src, u32 limit, UINT start, UINT num, T** out)
{
    if (!out)
        return;
    for (UINT i = 0; i < num; ++i)
    {
        const u64 slot = u64(start) + i;
        T* p = slot < limit ? src[slot] : nullptr;
        if (p)
            p->AddRef();
        out[i] = p;
    }
}

template <class Iface>
void GetShader(const StageBindings& sb, Iface** shader, UINT* numInstances)
{
    if (shader)
    {
        *shader = static_cast<Iface*>(sb.shaderIface);
        if (*shader)
            (*shader)->AddRef();
    }
    if (numInstances)
        *numInstances = 0;
}

template <class T>
void GetInterface(T* src, T** out)
{
    if (!out)
        return;
    if (src)
        src->AddRef();
    *out = src;
}

void OutOfMemory(Context& c)
{
    Once("out of memory while preparing a draw or dispatch; skipped");
    try
    {
        c.InvalidateApplied();
    }
    catch (...)
    {
    }
}
} // namespace

// ================================================================================================
// Hazards
// ================================================================================================

// Every stage: a new output of either pipeline (RTV, DSV, OM UAV, CS UAV) unbinds all of them.
void Context::UnbindSrvsOf(Resource* resource, const SubRange& range)
{
    Resource* target = ActualOf(resource);
    for (u32 s = 0; s < kStageCount; ++s)
    {
        StageBindings& sb = stages[s];
        for (ID3D11ShaderResourceView*& slot : sb.srv)
        {
            if (slot && Aliases(AsView(slot), target, range))
            {
                ID3D11ShaderResourceView* old = slot;
                slot = nullptr;
                old->Release();
                sb.srvDirty = true;
            }
        }
    }
}

bool Context::IsBoundAsOutput(Resource* resource, const SubRange& range) const
{
    Resource* target = ActualOf(resource);
    const u32 n = (std::min)(numRTVs, kMaxRTVs);
    for (u32 i = 0; i < n; ++i)
    {
        if (rtvs[i] && Aliases(AsView(rtvs[i]), target, range))
            return true;
    }
    if (dsv)
    {
        const View* v = AsView(dsv);
        SubRange written;
        if (DsvWriteRange(v, written) && ActualOf(v->resource) == target && Overlaps(written, range))
            return true;
    }
    for (ID3D11UnorderedAccessView* uav : psUavs)
    {
        if (uav && Aliases(AsView(uav), target, range))
            return true;
    }
    for (ID3D11UnorderedAccessView* uav : csUavs)
    {
        if (uav && Aliases(AsView(uav), target, range))
            return true;
    }
    return false;
}

// ================================================================================================
// Draw and dispatch preparation
// ================================================================================================

namespace
{
enum class ApplyResult
{
    Done,
    Skip,  // no pipeline: the call is dropped
    Retry, // the sampler heap was replaced meanwhile: bind the new one and apply everything again
};

// `mesh`: the mesh root signature (same parameter indices, mesh/amplification visibility).
ApplyResult ApplyGraphics(Context& c, ID3D12GraphicsCommandList* list, bool mesh)
{
    Device* dev = c.device();
    Context::Applied& a = c.applied;
    ApplyHeaps(c, list);

    if (mesh ? !a.rootSignatureMesh : !a.rootSignatureGraphics)
    {
        list->SetGraphicsRootSignature(mesh ? dev->rootSignatures.mesh : dev->rootSignatures.graphics);
        a.rootSignatureGraphics = !mesh;
        a.rootSignatureMesh = mesh;
        a.rootSignatureCompute = false;
        for (u32 s = 0; s < kGraphicsStages; ++s)
            ResetStageRoot(c.stages[s]);
        a.psUavDirty = true;
    }

    if (a.psoDirty)
    {
        GraphicsPsoDesc desc;
        FillPsoDesc(c, desc);
        const u64 hash = desc.Hash();
        a.psoDirty = false;
        // The hash names the PSO; a compute PSO in between resets psoHash.
        if (!a.pso || hash != a.psoHash)
        {
            ID3D12PipelineState* pso = dev->pipelines.Graphics(desc, hash);
            if (pso && pso != a.pso)
            {
                list->SetPipelineState(pso);
                dev->Count(Device::StatPso);
            }
            a.pso = pso;
            a.psoHash = hash;
        }
    }
    if (!a.pso)
    {
        Once("draws skipped: the driver refused a graphics pipeline state");
        return ApplyResult::Skip;
    }

    for (u32 s = 0; s < kGraphicsStages; ++s)
    {
        if (c.stages[s].shader && !ApplyStage(c, list, s, false))
            return ApplyResult::Retry;
    }
    const Shader* ps = c.stages[kPS].shader;
    if (ps && ps->bindings.uavMask && a.psUavDirty)
    {
        list->SetGraphicsRootDescriptorTable(GfxRoot::PS_UAV, BuildUavTable(c, c.psUavs, ps->bindings));
        a.psUavDirty = false;
    }
    return ApplyResult::Done;
}

ApplyResult ApplyCompute(Context& c, ID3D12GraphicsCommandList* list)
{
    Device* dev = c.device();
    Context::Applied& a = c.applied;
    ApplyHeaps(c, list);

    StageBindings& sb = c.stages[kCS];
    Shader* cs = sb.shader;
    if (!a.rootSignatureCompute)
    {
        list->SetComputeRootSignature(dev->rootSignatures.compute);
        a.rootSignatureCompute = true;
        a.rootSignatureGraphics = false;
        a.rootSignatureMesh = false;
        ResetStageRoot(sb);
        a.csUavDirty = true;
    }

    ID3D12PipelineState* pso = dev->pipelines.Compute(cs);
    if (!pso)
    {
        Once("dispatches skipped: the driver refused a compute pipeline state");
        return ApplyResult::Skip;
    }
    if (pso != a.pso)
    {
        list->SetPipelineState(pso);
        a.pso = pso;
        // The graphics PSO has to be set again at the next draw.
        a.psoHash = 0;
        a.psoDirty = true;
    }

    if (!ApplyStage(c, list, kCS, true))
        return ApplyResult::Retry;
    if (cs->bindings.uavMask && a.csUavDirty)
    {
        list->SetComputeRootDescriptorTable(CsRoot::UAV, BuildUavTable(c, c.csUavs, cs->bindings));
        a.csUavDirty = false;
    }
    return ApplyResult::Done;
}

// Runs `apply` until it did not race a sampler heap replacement (one retry is always enough: a single
// call cannot fill a fresh heap).
template <class Apply>
bool ApplyWithRetry(Context& c, ID3D12GraphicsCommandList* list, Apply&& apply)
{
    for (u32 attempt = 0; attempt < 3; ++attempt)
    {
        switch (apply(c, list))
        {
        case ApplyResult::Done: return true;
        case ApplyResult::Skip: return false;
        case ApplyResult::Retry: break;
        }
    }
    Once("a draw or dispatch kept racing sampler heap replacements; skipped");
    return false;
}
} // namespace

bool Context::PrepareDraw()
{
    const Shader* vs = stages[u32(Stage::VS)].shader;
    if (vs && vs->kind == kKindMesh && MeshRefuseDraw()) // [dx12:mesh] hook
        return false;
    return PrepareGraphics(false);
}

bool Context::PrepareGraphics(bool mesh)
{
    if (recordCompute)
    {
        Once("! compute recording (async job): draws dropped");
        return false;
    }
    ID3D12GraphicsCommandList* list = List();
    if (!list)
        return false;
    if (!stages[u32(Stage::VS)].shader)
    {
        Once(mesh ? "mesh draw without a mesh shader skipped" : "draw without a vertex shader skipped");
        return false;
    }
    if (mesh && !m_device->rootSignatures.mesh)
    {
        Once("mesh draw without a mesh root signature skipped");
        return false;
    }
    if (!mesh && topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
    {
        Once("draw without a primitive topology skipped");
        return false;
    }
    if (!ApplyWithRetry(*this, list, [mesh](Context& c, ID3D12GraphicsCommandList* l) { return ApplyGraphics(c, l, mesh); }))
        return false;
    const Shader* ps = stages[kPS].shader;
    const bool usesPsUavs = ps && ps->bindings.uavMask;

    if (!mesh)
    {
        ApplyVertexBuffers(*this, list);
        ApplyIndexBuffer(*this, list);
    }
    ApplyFixedFunction(*this, list);

    EnsureDrawStates(*this);
    // A guard join (async compute) closed the list the state went to: everything again on the new one.
    if (m_list != list)
        return PrepareGraphics(mesh);
    // D3D11 orders UAV accesses of consecutive calls; the flag stays set until the next UAV user.
    if (usesPsUavs)
    {
        if (applied.uavWritten)
            UavBarrier();
        applied.uavWritten = true;
    }
    FlushBarriers();
    m_device->Count(Device::StatDraws);
    return true;
}

bool Context::PrepareDispatch()
{
    ID3D12GraphicsCommandList* list = List();
    if (!list)
        return false;
    StageBindings& sb = stages[kCS];
    const Shader* cs = sb.shader;
    if (!cs)
    {
        Once("dispatch without a compute shader skipped");
        return false;
    }
    if (!ApplyWithRetry(*this, list, ApplyCompute))
        return false;
    const bool uavs = cs->bindings.uavMask != 0;

    EnsureShaderInputs(*this, sb, ReadOnlyDepth{});
    EnsureUavs(*this, csUavs, cs->bindings.uavMask);
    if (m_list != list)
        return PrepareDispatch();
    if (uavs)
    {
        if (applied.uavWritten)
            UavBarrier();
        applied.uavWritten = true;
    }
    FlushBarriers();
    return true;
}

// ================================================================================================
// Draws and dispatches
// ================================================================================================

void STDMETHODCALLTYPE Context::DrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex)
{
    if (!indexCount)
        return;
    try
    {
        if (PrepareDraw())
            m_list->DrawIndexedInstanced(indexCount, 1, startIndex, baseVertex, 0);
    }
    catch (const std::bad_alloc&)
    {
        OutOfMemory(*this);
    }
}

void STDMETHODCALLTYPE Context::Draw(UINT vertexCount, UINT startVertex)
{
    if (!vertexCount)
        return;
    try
    {
        if (PrepareDraw())
            m_list->DrawInstanced(vertexCount, 1, startVertex, 0);
    }
    catch (const std::bad_alloc&)
    {
        OutOfMemory(*this);
    }
}

void STDMETHODCALLTYPE Context::DrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex,
    INT baseVertex, UINT startInstance)
{
    if (!indexCountPerInstance || !instanceCount)
        return;
    try
    {
        if (PrepareDraw())
            m_list->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
    }
    catch (const std::bad_alloc&)
    {
        OutOfMemory(*this);
    }
}

void STDMETHODCALLTYPE Context::DrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex,
    UINT startInstance)
{
    if (!vertexCountPerInstance || !instanceCount)
        return;
    try
    {
        if (PrepareDraw())
            m_list->DrawInstanced(vertexCountPerInstance, instanceCount, startVertex, startInstance);
    }
    catch (const std::bad_alloc&)
    {
        OutOfMemory(*this);
    }
}

void STDMETHODCALLTYPE Context::Dispatch(UINT x, UINT y, UINT z)
{
    if (!x || !y || !z)
        return;
    try
    {
        if (PrepareDispatch())
            m_list->Dispatch(x, y, z);
    }
    catch (const std::bad_alloc&)
    {
        OutOfMemory(*this);
    }
}

void STDMETHODCALLTYPE Context::DrawAuto()
{
    Once("DrawAuto (stream output) is not supported; ignored");
}

void STDMETHODCALLTYPE Context::DrawIndexedInstancedIndirect(ID3D11Buffer* args, UINT offset)
{
    Indirect(IndirectKind::DrawIndexed, args, offset, 1, nullptr, 0);
}

void STDMETHODCALLTYPE Context::DrawInstancedIndirect(ID3D11Buffer* args, UINT offset)
{
    Indirect(IndirectKind::Draw, args, offset, 1, nullptr, 0);
}

void STDMETHODCALLTYPE Context::DispatchIndirect(ID3D11Buffer* args, UINT offset)
{
    Indirect(IndirectKind::Dispatch, args, offset, 1, nullptr, 0);
}

namespace
{
// Where the bytes of a buffer are for ExecuteIndirect; null for staging buffers.
ID3D12Resource* IndirectLocation(Resource* r, u64& offset)
{
    if (r->native)
        return r->native;
    if (r->usage == D3D11_USAGE_DYNAMIC)
    {
        offset += r->current.alloc.offset;
        return r->current.alloc.resource;
    }
    if (r->usage == D3D11_USAGE_STAGING)
        return nullptr;
    offset += r->range.offset;
    return r->range.resource;
}
} // namespace

// ExecuteIndirect with a device-level command signature (no root arguments): the draw or dispatch
// sees exactly the bindings a direct call would. Arguments and count are INDIRECT_ARGUMENT (tracked
// buffers transition, pages promote, upload memory is GENERIC_READ).
void Context::Indirect(IndirectKind kind, ID3D11Buffer* args, UINT offset, UINT maxCount, ID3D11Buffer* count, UINT countOffset)
{
    static constexpr u32 kStride[] = {sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), sizeof(D3D12_DRAW_ARGUMENTS),
        sizeof(D3D12_DISPATCH_ARGUMENTS)};
    if (!args || !maxCount)
        return;
    ID3D12CommandSignature* signature = m_device->indirectSignatures[u32(kind)];
    if (!signature)
    {
        Once("indirect draws skipped: no command signature");
        return;
    }
    if ((offset & 3) || (countOffset & 3))
    {
        Once("indirect call with an argument or count offset not aligned to 4 bytes skipped");
        return;
    }
    Resource* a = AsResource(args);
    Resource* c = count ? AsResource(count) : nullptr;
    // Records beyond the buffer are dropped, as far as they go.
    const u32 stride = kStride[u32(kind)];
    const u64 fit = offset < a->byteWidth ? (a->byteWidth - offset) / stride : 0;
    if (fit < maxCount)
    {
        Once("indirect call reads beyond its argument buffer; clamped");
        maxCount = UINT(fit);
    }
    if (c && u64(countOffset) + 4 > c->byteWidth)
    {
        Once("indirect call with its count beyond the count buffer skipped");
        return;
    }
    if (!maxCount)
        return;
    u64 argsOffset = offset;
    u64 countLocation = c ? countOffset : 0;
    ID3D12Resource* argsResource = IndirectLocation(a, argsOffset);
    ID3D12Resource* countResource = c ? IndirectLocation(c, countLocation) : nullptr;
    if (!argsResource || (c && !countResource))
    {
        Once("indirect call with a staging argument or count buffer skipped");
        return;
    }
    try
    {
        // Arguments first: a guard join (async compute) must not come between the preparation and the call.
        if (a->native)
            RequireAll(a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (c && c->native)
            RequireAll(c, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (!(kind == IndirectKind::Dispatch ? PrepareDispatch() : PrepareDraw()))
            return;
        FlushBarriers();
        m_list->ExecuteIndirect(signature, maxCount, argsResource, argsOffset, countResource, countLocation);
        m_device->Count(Device::StatIndirect);
    }
    catch (const std::bad_alloc&)
    {
        OutOfMemory(*this);
    }
}

// ================================================================================================
// Input assembler
// ================================================================================================

void STDMETHODCALLTYPE Context::IASetInputLayout(ID3D11InputLayout* layout)
{
    if (Assign(inputLayout, layout))
        applied.psoDirty = true;
}

void STDMETHODCALLTYPE Context::IASetVertexBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers, const UINT* strides,
    const UINT* offsets)
{
    if (!ClampRange(start, num, kMaxVBs, "vertex buffer"))
        return;
    for (UINT i = 0; i < num; ++i)
    {
        VertexBufferBinding& vb = vbs[start + i];
        const UINT stride = strides ? strides[i] : 0;
        const UINT offset = offsets ? offsets[i] : 0;
        const bool changed = Assign(vb.buffer, buffers ? buffers[i] : nullptr);
        if (changed || vb.stride != stride || vb.offset != offset)
            applied.vbDirty = true;
        vb.stride = stride;
        vb.offset = offset;
    }
}

void STDMETHODCALLTYPE Context::IASetIndexBuffer(ID3D11Buffer* buffer, DXGI_FORMAT format, UINT offset)
{
    const TopologyKey before = KeyOf(topology, indexFormat);
    bool changed = Assign(indexBuffer, buffer);
    if (format != indexFormat || offset != indexOffset)
        changed = true;
    indexFormat = format;
    indexOffset = offset;
    if (changed)
        applied.ibDirty = true;
    if (!(KeyOf(topology, indexFormat) == before))
        applied.psoDirty = true;
}

void STDMETHODCALLTYPE Context::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY newTopology)
{
    if (newTopology == topology)
        return;
    const TopologyKey before = KeyOf(topology, indexFormat);
    topology = newTopology;
    applied.topologyDirty = true;
    if (!(KeyOf(topology, indexFormat) == before))
        applied.psoDirty = true;
}

void STDMETHODCALLTYPE Context::IAGetInputLayout(ID3D11InputLayout** layout)
{
    GetInterface(inputLayout, layout);
}

void STDMETHODCALLTYPE Context::IAGetVertexBuffers(UINT start, UINT num, ID3D11Buffer** buffers, UINT* strides, UINT* offsets)
{
    for (UINT i = 0; i < num; ++i)
    {
        const u64 slot = u64(start) + i;
        const VertexBufferBinding* vb = slot < kMaxVBs ? &vbs[slot] : nullptr;
        if (buffers)
        {
            buffers[i] = vb ? vb->buffer : nullptr;
            if (buffers[i])
                buffers[i]->AddRef();
        }
        if (strides)
            strides[i] = vb ? vb->stride : 0;
        if (offsets)
            offsets[i] = vb ? vb->offset : 0;
    }
}

void STDMETHODCALLTYPE Context::IAGetIndexBuffer(ID3D11Buffer** buffer, DXGI_FORMAT* format, UINT* offset)
{
    GetInterface(indexBuffer, buffer);
    if (format)
        *format = indexBuffer ? indexFormat : DXGI_FORMAT_UNKNOWN;
    if (offset)
        *offset = indexBuffer ? indexOffset : 0;
}

void STDMETHODCALLTYPE Context::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* outTopology)
{
    if (outTopology)
        *outTopology = topology;
}

// ================================================================================================
// Shader stages
// ================================================================================================

void STDMETHODCALLTYPE Context::VSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    SetConstantBuffers(*this, Stage::VS, start, num, buffers);
}

void STDMETHODCALLTYPE Context::HSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    SetConstantBuffers(*this, Stage::HS, start, num, buffers);
}

void STDMETHODCALLTYPE Context::DSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    SetConstantBuffers(*this, Stage::DS, start, num, buffers);
}

void STDMETHODCALLTYPE Context::GSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    SetConstantBuffers(*this, Stage::GS, start, num, buffers);
}

void STDMETHODCALLTYPE Context::PSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    SetConstantBuffers(*this, Stage::PS, start, num, buffers);
}

void STDMETHODCALLTYPE Context::CSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    SetConstantBuffers(*this, Stage::CS, start, num, buffers);
}

void STDMETHODCALLTYPE Context::VSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    SetShaderResources(*this, Stage::VS, start, num, views);
}

void STDMETHODCALLTYPE Context::HSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    SetShaderResources(*this, Stage::HS, start, num, views);
}

void STDMETHODCALLTYPE Context::DSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    SetShaderResources(*this, Stage::DS, start, num, views);
}

void STDMETHODCALLTYPE Context::GSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    SetShaderResources(*this, Stage::GS, start, num, views);
}

void STDMETHODCALLTYPE Context::PSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    SetShaderResources(*this, Stage::PS, start, num, views);
}

void STDMETHODCALLTYPE Context::CSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    SetShaderResources(*this, Stage::CS, start, num, views);
}

void STDMETHODCALLTYPE Context::VSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    SetSamplers(*this, Stage::VS, start, num, samplers);
}

void STDMETHODCALLTYPE Context::HSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    SetSamplers(*this, Stage::HS, start, num, samplers);
}

void STDMETHODCALLTYPE Context::DSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    SetSamplers(*this, Stage::DS, start, num, samplers);
}

void STDMETHODCALLTYPE Context::GSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    SetSamplers(*this, Stage::GS, start, num, samplers);
}

void STDMETHODCALLTYPE Context::PSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    SetSamplers(*this, Stage::PS, start, num, samplers);
}

void STDMETHODCALLTYPE Context::CSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    SetSamplers(*this, Stage::CS, start, num, samplers);
}

void STDMETHODCALLTYPE Context::VSSetShader(ID3D11VertexShader* shader, ID3D11ClassInstance* const* /*inst*/, UINT numInst)
{
    SetShader(*this, Stage::VS, shader, numInst);
}

void STDMETHODCALLTYPE Context::HSSetShader(ID3D11HullShader* shader, ID3D11ClassInstance* const* /*inst*/, UINT numInst)
{
    SetShader(*this, Stage::HS, shader, numInst);
}

void STDMETHODCALLTYPE Context::DSSetShader(ID3D11DomainShader* shader, ID3D11ClassInstance* const* /*inst*/, UINT numInst)
{
    SetShader(*this, Stage::DS, shader, numInst);
}

void STDMETHODCALLTYPE Context::GSSetShader(ID3D11GeometryShader* shader, ID3D11ClassInstance* const* /*inst*/, UINT numInst)
{
    SetShader(*this, Stage::GS, shader, numInst);
}

void STDMETHODCALLTYPE Context::PSSetShader(ID3D11PixelShader* shader, ID3D11ClassInstance* const* /*inst*/, UINT numInst)
{
    SetShader(*this, Stage::PS, shader, numInst);
}

void STDMETHODCALLTYPE Context::CSSetShader(ID3D11ComputeShader* shader, ID3D11ClassInstance* const* /*inst*/, UINT numInst)
{
    SetShader(*this, Stage::CS, shader, numInst);
}

void STDMETHODCALLTYPE Context::CSSetUnorderedAccessViews(UINT start, UINT num, ID3D11UnorderedAccessView* const* uavs,
    const UINT* /*counts*/)
{
    // Initial counts only matter for append/consume buffers, which the layer does not create.
    if (!ClampRange(start, num, kMaxUAVs, "CS unordered access"))
        return;
    for (UINT i = 0; i < num; ++i)
    {
        ID3D11UnorderedAccessView* uav = uavs ? uavs[i] : nullptr;
        if (!Assign(csUavs[start + i], uav))
            continue;
        applied.csUavDirty = true;
        if (uav)
        {
            View* v = AsView(uav);
            UnbindSrvsOf(v->resource, v->range);
            UnbindGraphicsOutputsOf(*this, v->resource, v->range);
        }
    }
}

void STDMETHODCALLTYPE Context::VSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers)
{
    GetRange(stages[u32(Stage::VS)].cb, kMaxCBs, start, num, buffers);
}

void STDMETHODCALLTYPE Context::HSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers)
{
    GetRange(stages[u32(Stage::HS)].cb, kMaxCBs, start, num, buffers);
}

void STDMETHODCALLTYPE Context::DSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers)
{
    GetRange(stages[u32(Stage::DS)].cb, kMaxCBs, start, num, buffers);
}

void STDMETHODCALLTYPE Context::GSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers)
{
    GetRange(stages[u32(Stage::GS)].cb, kMaxCBs, start, num, buffers);
}

void STDMETHODCALLTYPE Context::PSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers)
{
    GetRange(stages[u32(Stage::PS)].cb, kMaxCBs, start, num, buffers);
}

void STDMETHODCALLTYPE Context::CSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers)
{
    GetRange(stages[u32(Stage::CS)].cb, kMaxCBs, start, num, buffers);
}

void STDMETHODCALLTYPE Context::VSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views)
{
    GetRange(stages[u32(Stage::VS)].srv, kMaxSRVs, start, num, views);
}

void STDMETHODCALLTYPE Context::HSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views)
{
    GetRange(stages[u32(Stage::HS)].srv, kMaxSRVs, start, num, views);
}

void STDMETHODCALLTYPE Context::DSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views)
{
    GetRange(stages[u32(Stage::DS)].srv, kMaxSRVs, start, num, views);
}

void STDMETHODCALLTYPE Context::GSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views)
{
    GetRange(stages[u32(Stage::GS)].srv, kMaxSRVs, start, num, views);
}

void STDMETHODCALLTYPE Context::PSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views)
{
    GetRange(stages[u32(Stage::PS)].srv, kMaxSRVs, start, num, views);
}

void STDMETHODCALLTYPE Context::CSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views)
{
    GetRange(stages[u32(Stage::CS)].srv, kMaxSRVs, start, num, views);
}

void STDMETHODCALLTYPE Context::VSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers)
{
    GetRange(stages[u32(Stage::VS)].samplers, kMaxSamplers, start, num, samplers);
}

void STDMETHODCALLTYPE Context::HSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers)
{
    GetRange(stages[u32(Stage::HS)].samplers, kMaxSamplers, start, num, samplers);
}

void STDMETHODCALLTYPE Context::DSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers)
{
    GetRange(stages[u32(Stage::DS)].samplers, kMaxSamplers, start, num, samplers);
}

void STDMETHODCALLTYPE Context::GSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers)
{
    GetRange(stages[u32(Stage::GS)].samplers, kMaxSamplers, start, num, samplers);
}

void STDMETHODCALLTYPE Context::PSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers)
{
    GetRange(stages[u32(Stage::PS)].samplers, kMaxSamplers, start, num, samplers);
}

void STDMETHODCALLTYPE Context::CSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers)
{
    GetRange(stages[u32(Stage::CS)].samplers, kMaxSamplers, start, num, samplers);
}

void STDMETHODCALLTYPE Context::VSGetShader(ID3D11VertexShader** shader, ID3D11ClassInstance** /*inst*/, UINT* numInst)
{
    GetShader(stages[u32(Stage::VS)], shader, numInst);
}

void STDMETHODCALLTYPE Context::HSGetShader(ID3D11HullShader** shader, ID3D11ClassInstance** /*inst*/, UINT* numInst)
{
    GetShader(stages[u32(Stage::HS)], shader, numInst);
}

void STDMETHODCALLTYPE Context::DSGetShader(ID3D11DomainShader** shader, ID3D11ClassInstance** /*inst*/, UINT* numInst)
{
    GetShader(stages[u32(Stage::DS)], shader, numInst);
}

void STDMETHODCALLTYPE Context::GSGetShader(ID3D11GeometryShader** shader, ID3D11ClassInstance** /*inst*/, UINT* numInst)
{
    GetShader(stages[u32(Stage::GS)], shader, numInst);
}

void STDMETHODCALLTYPE Context::PSGetShader(ID3D11PixelShader** shader, ID3D11ClassInstance** /*inst*/, UINT* numInst)
{
    GetShader(stages[u32(Stage::PS)], shader, numInst);
}

void STDMETHODCALLTYPE Context::CSGetShader(ID3D11ComputeShader** shader, ID3D11ClassInstance** /*inst*/, UINT* numInst)
{
    GetShader(stages[u32(Stage::CS)], shader, numInst);
}

void STDMETHODCALLTYPE Context::CSGetUnorderedAccessViews(UINT start, UINT num, ID3D11UnorderedAccessView** uavs)
{
    GetRange(csUavs, kMaxUAVs, start, num, uavs);
}

// ================================================================================================
// Rasterizer
// ================================================================================================

void STDMETHODCALLTYPE Context::RSSetState(ID3D11RasterizerState* state)
{
    if (state == rasterState)
        return;
    const bool scissorBefore = ScissorEnabled(rasterState);
    Assign(rasterState, state);
    applied.psoDirty = true;
    if (ScissorEnabled(rasterState) != scissorBefore)
        applied.scissorDirty = true;
}

void STDMETHODCALLTYPE Context::RSSetViewports(UINT num, const D3D11_VIEWPORT* newViewports)
{
    if (!newViewports)
        num = 0;
    if (num > kMaxViewports)
    {
        Once("more viewports than the layer supports; clamped");
        num = kMaxViewports;
    }
    if (num == numViewports && (!num || std::memcmp(viewports, newViewports, num * sizeof(D3D11_VIEWPORT)) == 0))
        return;
    // Without scissoring there is one full rect per viewport.
    if (num != numViewports)
        applied.scissorDirty = true;
    if (num)
        std::memcpy(viewports, newViewports, num * sizeof(D3D11_VIEWPORT));
    numViewports = num;
    applied.viewportDirty = true;
}

void STDMETHODCALLTYPE Context::RSSetScissorRects(UINT num, const D3D11_RECT* rects)
{
    if (!rects)
        num = 0;
    if (num > kMaxViewports)
    {
        Once("more scissor rects than the layer supports; clamped");
        num = kMaxViewports;
    }
    if (num == numScissors && (!num || std::memcmp(scissors, rects, num * sizeof(D3D11_RECT)) == 0))
        return;
    if (num)
        std::memcpy(scissors, rects, num * sizeof(D3D11_RECT));
    numScissors = num;
    applied.scissorDirty = true;
}

void STDMETHODCALLTYPE Context::RSGetState(ID3D11RasterizerState** state)
{
    GetInterface(rasterState, state);
}

// Like the D3D11 runtime (checked by tools/qa/d3d12layer): without an array the count of bound
// viewports; with one, the array is filled (unbound entries zeroed) and the count becomes the
// smaller of the two.
void STDMETHODCALLTYPE Context::RSGetViewports(UINT* num, D3D11_VIEWPORT* outViewports)
{
    if (!num)
        return;
    if (outViewports)
    {
        for (UINT i = 0; i < *num; ++i)
            outViewports[i] = i < numViewports ? viewports[i] : D3D11_VIEWPORT{};
        *num = (std::min)(*num, numViewports);
    }
    else
        *num = numViewports;
}

void STDMETHODCALLTYPE Context::RSGetScissorRects(UINT* num, D3D11_RECT* rects)
{
    if (!num)
        return;
    if (rects)
    {
        for (UINT i = 0; i < *num; ++i)
            rects[i] = i < numScissors ? scissors[i] : D3D11_RECT{};
        *num = (std::min)(*num, numScissors);
    }
    else
        *num = numScissors;
}

// ================================================================================================
// Output merger
// ================================================================================================

namespace
{
void BindTargets(Context& c, UINT num, ID3D11RenderTargetView* const* newRtvs, ID3D11DepthStencilView* newDsv)
{
    if (num > kMaxRTVs)
    {
        Once("more render targets than D3D11 allows; clamped");
        num = kMaxRTVs;
    }
    const OutputSignature before = SignatureOf(c);
    bool changed = false;
    // Bound outputs unbind every input that aliases them (D3D11 hazard rule); outputs that stayed
    // bound had their aliasing inputs removed already.
    for (u32 i = 0; i < kMaxRTVs; ++i)
    {
        ID3D11RenderTargetView* rtv = i < num && newRtvs ? newRtvs[i] : nullptr;
        if (!Assign(c.rtvs[i], rtv))
            continue;
        changed = true;
        if (rtv)
        {
            View* v = AsView(rtv);
            c.UnbindSrvsOf(v->resource, v->range);
            UnbindCsUavsOf(c, v->resource, v->range);
        }
    }
    if (Assign(c.dsv, newDsv))
    {
        changed = true;
        if (newDsv)
        {
            View* v = AsView(newDsv);
            SubRange written;
            if (DsvWriteRange(v, written))
                c.UnbindSrvsOf(v->resource, written);
        }
    }
    c.numRTVs = num;
    if (!changed)
        return;
    c.applied.rtDirty = true;
    if (!(SignatureOf(c) == before))
        c.applied.psoDirty = true;
}
} // namespace

void STDMETHODCALLTYPE Context::OMSetRenderTargets(UINT num, ID3D11RenderTargetView* const* newRtvs,
    ID3D11DepthStencilView* newDsv)
{
    BindTargets(*this, num, newRtvs, newDsv);
}

void STDMETHODCALLTYPE Context::OMSetRenderTargetsAndUnorderedAccessViews(UINT newNumRTVs, ID3D11RenderTargetView* const* newRtvs,
    ID3D11DepthStencilView* newDsv, UINT uavStart, UINT numUAVs, ID3D11UnorderedAccessView* const* uavs, const UINT* /*counts*/)
{
    if (newNumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        BindTargets(*this, newNumRTVs, newRtvs, newDsv);
    if (numUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
        return;

    // The call replaces the whole UAV set: slots outside [uavStart, uavStart + numUAVs) are unbound.
    ID3D11UnorderedAccessView* next[kMaxUAVs]{};
    UINT first = uavStart;
    UINT count = numUAVs;
    if (ClampRange(first, count, kMaxUAVs, "OM unordered access"))
    {
        for (UINT i = 0; i < count; ++i)
            next[first + i] = uavs ? uavs[i] : nullptr;
    }
    for (u32 slot = 0; slot < kMaxUAVs; ++slot)
    {
        if (!Assign(psUavs[slot], next[slot]))
            continue;
        applied.psUavDirty = true;
        if (next[slot])
        {
            View* v = AsView(next[slot]);
            UnbindSrvsOf(v->resource, v->range);
            UnbindCsUavsOf(*this, v->resource, v->range);
        }
    }
    psUavStart = (std::min)(uavStart, kMaxUAVs);
}

void STDMETHODCALLTYPE Context::OMSetBlendState(ID3D11BlendState* state, const FLOAT factor[4], UINT mask)
{
    if (Assign(blendState, state))
        applied.psoDirty = true;
    static const FLOAT kDefaultFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const FLOAT* f = factor ? factor : kDefaultFactor;
    if (std::memcmp(blendFactor, f, sizeof(blendFactor)) != 0)
    {
        std::memcpy(blendFactor, f, sizeof(blendFactor));
        applied.blendFactorDirty = true;
    }
    // The sample mask is PSO state in D3D12.
    if (mask != sampleMask)
    {
        sampleMask = mask;
        applied.psoDirty = true;
    }
}

void STDMETHODCALLTYPE Context::OMSetDepthStencilState(ID3D11DepthStencilState* state, UINT ref)
{
    if (Assign(depthState, state))
        applied.psoDirty = true;
    if (ref != stencilRef)
    {
        stencilRef = ref;
        applied.stencilRefDirty = true;
    }
}

void STDMETHODCALLTYPE Context::OMGetRenderTargets(UINT num, ID3D11RenderTargetView** outRtvs, ID3D11DepthStencilView** outDsv)
{
    GetRange(rtvs, kMaxRTVs, 0, num, outRtvs);
    GetInterface(dsv, outDsv);
}

void STDMETHODCALLTYPE Context::OMGetRenderTargetsAndUnorderedAccessViews(UINT num, ID3D11RenderTargetView** outRtvs,
    ID3D11DepthStencilView** outDsv, UINT uavStart, UINT numUAVs, ID3D11UnorderedAccessView** uavs)
{
    GetRange(rtvs, kMaxRTVs, 0, num, outRtvs);
    GetInterface(dsv, outDsv);
    GetRange(psUavs, kMaxUAVs, uavStart, numUAVs, uavs);
}

void STDMETHODCALLTYPE Context::OMGetBlendState(ID3D11BlendState** state, FLOAT factor[4], UINT* mask)
{
    GetInterface(blendState, state);
    if (factor)
        std::memcpy(factor, blendFactor, sizeof(blendFactor));
    if (mask)
        *mask = sampleMask;
}

void STDMETHODCALLTYPE Context::OMGetDepthStencilState(ID3D11DepthStencilState** state, UINT* ref)
{
    GetInterface(depthState, state);
    if (ref)
        *ref = stencilRef;
}

// ================================================================================================
// Stream output, predication, context type
// ================================================================================================

void STDMETHODCALLTYPE Context::SOSetTargets(UINT num, ID3D11Buffer* const* targets, const UINT* /*offsets*/)
{
    if (!targets)
        return;
    for (UINT i = 0; i < num; ++i)
    {
        if (targets[i])
        {
            Once("stream output is not supported; SOSetTargets ignored");
            return;
        }
    }
}

void STDMETHODCALLTYPE Context::SOGetTargets(UINT num, ID3D11Buffer** targets)
{
    if (!targets)
        return;
    for (UINT i = 0; i < num; ++i)
        targets[i] = nullptr;
}

void STDMETHODCALLTYPE Context::SetPredication(ID3D11Predicate* predicate, BOOL /*value*/)
{
    if (predicate)
        Once("predication is not supported; SetPredication ignored");
}

void STDMETHODCALLTYPE Context::GetPredication(ID3D11Predicate** predicate, BOOL* value)
{
    if (predicate)
        *predicate = nullptr;
    if (value)
        *value = FALSE;
}

D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE Context::GetType()
{
    return m_deferred ? D3D11_DEVICE_CONTEXT_DEFERRED : D3D11_DEVICE_CONTEXT_IMMEDIATE;
}
} // namespace xrd3d12
