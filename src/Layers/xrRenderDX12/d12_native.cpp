// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Native features of xrD3D12 (docs/dead-air/dx12-features/infra.md): capabilities, native sections
// on any context, the bindless range, native pipelines and buffers, D3D11-path extensions.
#include "d12_layer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace xrd3d12
{
namespace
{
Device* LayerDevice(ID3D11Device* device)
{
    return IsLayerDevice(device) ? Device::From(device) : nullptr;
}

template <class T>
bool QueryFeature(ID3D12Device* device, D3D12_FEATURE feature, T& data)
{
    return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(data)));
}

// Tier values of the form 10, 11, 12 as "1.0", "1.1", "1.2".
const char* TierText(UINT tier, char (&text)[8])
{
    if (!tier)
        return "none";
    std::snprintf(text, sizeof(text), "%u.%u", tier / 10, tier % 10);
    return text;
}
} // namespace

// ============================================================================================
// Capabilities
// ============================================================================================

void QueryNativeCaps(ID3D12Device* device, IDXGIAdapter1* adapter, NativeCaps& caps)
{
    caps = NativeCaps{};
    if (!device)
        return;

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS, options))
        caps.minPrecision16 = (options.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT) != 0;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS1, options1))
    {
        caps.waveOps = options1.WaveOps != FALSE;
        caps.waveLanesMin = options1.WaveOps ? options1.WaveLaneCountMin : 0;
        caps.waveLanesMax = options1.WaveOps ? options1.WaveLaneCountMax : 0;
        caps.int64Ops = options1.Int64ShaderOps != FALSE;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS3, options3))
    {
        caps.barycentrics = options3.BarycentricsSupported != FALSE;
        caps.copyQueueTimestamps = options3.CopyQueueTimestampQueriesSupported != FALSE;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS4, options4))
        caps.native16Bit = options4.Native16BitShaderOpsSupported != FALSE;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS5, options5))
        caps.raytracingTier = UINT(options5.RaytracingTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS6, options6))
    {
        caps.vrsTier = UINT(options6.VariableShadingRateTier);
        caps.vrsTileSize = caps.vrsTier >= 2 ? options6.ShadingRateImageTileSize : 0;
        caps.vrsAdditionalRates = options6.AdditionalShadingRatesSupported != FALSE;
        caps.vrsPerPrimitiveWithViewportIndex = options6.PerPrimitiveShadingRateSupportedWithViewportIndexing != FALSE;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS7, options7))
        caps.meshShaderTier = UINT(options7.MeshShaderTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS10 options10{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS10, options10))
    {
        caps.vrsSumCombiner = options10.VariableRateShadingSumCombinerSupported != FALSE;
        caps.meshPerPrimitiveRate = options10.MeshShaderPerPrimitiveShadingRateSupported != FALSE;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS12, options12))
        caps.enhancedBarriers = options12.EnhancedBarriersSupported != FALSE;
    // Newer than some runtimes: a refused query leaves the tiers 0.
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21{};
    if (QueryFeature(device, D3D12_FEATURE_D3D12_OPTIONS21, options21))
    {
        caps.executeIndirectTier = UINT(options21.ExecuteIndirectTier);
        caps.workGraphsTier = UINT(options21.WorkGraphsTier);
    }
    else
        caps.executeIndirectTier = UINT(D3D12_EXECUTE_INDIRECT_TIER_1_0);

    // A shader model newer than the runtime knows is refused (E_INVALIDARG): walk down.
    caps.shaderModel = 0x51;
    for (UINT model = UINT(D3D_HIGHEST_SHADER_MODEL); model >= UINT(D3D_SHADER_MODEL_6_0); --model)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL(model)};
        if (QueryFeature(device, D3D12_FEATURE_SHADER_MODEL, sm))
        {
            caps.shaderModel = UINT(sm.HighestShaderModel) >= UINT(D3D_SHADER_MODEL_6_0) ? UINT(sm.HighestShaderModel) : 0x51;
            break;
        }
    }
    for (UINT version = UINT(D3D_ROOT_SIGNATURE_VERSION_1_2); version >= UINT(D3D_ROOT_SIGNATURE_VERSION_1_0); --version)
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE root{D3D_ROOT_SIGNATURE_VERSION(version)};
        if (QueryFeature(device, D3D12_FEATURE_ROOT_SIGNATURE, root))
        {
            caps.rootSignatureVersion = UINT(root.HighestVersion);
            break;
        }
    }
    // Final once the compiler is known (MaskNativeCaps): SM 6.6 dynamic resources need binding tier 3.
    caps.heapIndexing = caps.shaderModel >= 0x66 && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;

    ID3D12DeviceConfiguration* configuration = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&configuration))) && configuration)
    {
        caps.runtimeSdkVersion = configuration->GetDesc().SDKVersion;
        configuration->Release();
    }
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
    if (QueryFeature(device, D3D12_FEATURE_ARCHITECTURE1, architecture))
        caps.uma = architecture.UMA != FALSE;
    DXGI_ADAPTER_DESC1 desc{};
    if (adapter && SUCCEEDED(adapter->GetDesc1(&desc)))
        caps.vendorId = desc.VendorId;
}

void MaskNativeCaps(NativeCaps& caps, UINT disabledFeatures)
{
    if (disabledFeatures & NativeFeatureRaytracing)
        caps.raytracingTier = 0;
    if (disabledFeatures & NativeFeatureVrs)
    {
        caps.vrsTier = 0;
        caps.vrsTileSize = 0;
        caps.vrsAdditionalRates = caps.vrsPerPrimitiveWithViewportIndex = caps.vrsSumCombiner = false;
        caps.meshPerPrimitiveRate = false;
    }
    if (disabledFeatures & NativeFeatureMeshShaders)
    {
        caps.meshShaderTier = 0;
        caps.meshPerPrimitiveRate = false;
    }
    if (disabledFeatures & NativeFeatureAsyncCompute)
        caps.asyncCompute = false;
    // Without shader model 6 the SM 6 features of the device are out of reach too.
    if (disabledFeatures & NativeFeatureSm6)
    {
        caps.shaderModel = 0x51;
        caps.dxcShaderModel = 0;
        caps.waveOps = false;
        caps.waveLanesMin = caps.waveLanesMax = 0;
        caps.native16Bit = false;
    }
    caps.heapIndexing = caps.heapIndexing && caps.shaderModel >= 0x66 && caps.dxcShaderModel >= 0x66;
}

void LogNativeCaps(const NativeCaps& caps, UINT disabledFeatures, u32 ringDescriptors)
{
    char rt[8], ei[8], wg[8], rs[8];
    std::string vrs = "none";
    if (caps.vrsTier)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%u", caps.vrsTier);
        vrs = text;
        if (caps.vrsTier >= 2)
        {
            std::snprintf(text, sizeof(text), " (tile %u%s)", caps.vrsTileSize, caps.vrsAdditionalRates ? ", extra rates" : "");
            vrs += text;
        }
    }
    char dxc[16] = "no DXC";
    if (caps.dxcShaderModel)
        std::snprintf(dxc, sizeof(dxc), "DXC %X.%X", caps.dxcShaderModel >> 4, caps.dxcShaderModel & 0xF);
    char waves[24] = "no waves";
    if (caps.waveOps)
        std::snprintf(waves, sizeof(waves), "waves %u-%u", caps.waveLanesMin, caps.waveLanesMax);
    const UINT rootTier = caps.rootSignatureVersion == UINT(D3D_ROOT_SIGNATURE_VERSION_1_2) ? 12 :
                          caps.rootSignatureVersion == UINT(D3D_ROOT_SIGNATURE_VERSION_1_1) ? 11 :
                          caps.rootSignatureVersion ? 10 : 0;
    Log("* native: raytracing %s, VRS %s, mesh shaders %s, SM %X.%X (%s), %s%s%s%s, execute indirect %s, work graphs %s "
        "(unused), root signature %s, runtime SDK %u",
        TierText(caps.raytracingTier, rt), vrs.c_str(), caps.meshShaderTier ? "1" : "none", caps.shaderModel >> 4,
        caps.shaderModel & 0xF, dxc, waves, caps.native16Bit ? ", 16-bit ops" : "", caps.int64Ops ? ", int64" : "",
        caps.enhancedBarriers ? ", enhanced barriers (unused)" : "", TierText(caps.executeIndirectTier, ei),
        TierText(caps.workGraphsTier, wg), TierText(rootTier, rs), caps.runtimeSdkVersion);
    std::string off;
    const struct
    {
        UINT bit;
        const char* name;
    } switches[] = {{NativeFeatureRaytracing, " -dx12_no_rt"}, {NativeFeatureVrs, " -dx12_no_vrs"},
        {NativeFeatureMeshShaders, " -dx12_no_ms"}, {NativeFeatureAsyncCompute, " -dx12_no_async"},
        {NativeFeatureSm6, " -dx12_no_sm6"}};
    for (const auto& s : switches)
    {
        if (disabledFeatures & s.bit)
            off += s.name;
    }
    Log("* native: compute queue %s, copy queue timestamps %s, bindless %u + ring %u descriptors, %s GPU (vendor 0x%04X)%s%s",
        caps.asyncCompute ? "on (async jobs, made by the first)" : "off (jobs in order on the direct queue)",
        caps.copyQueueTimestamps ? "yes" : "no",
        caps.bindlessCapacity, ringDescriptors, caps.uma ? "integrated" : "discrete", caps.vendorId,
        off.empty() ? "" : ", switched off:", off.c_str());
}

const NativeCaps* GetNativeCaps(ID3D11Device* device)
{
    Device* dev = LayerDevice(device);
    return dev ? &dev->caps : nullptr;
}

bool ProbeAdapter(IDXGIAdapter1* adapter, char* reason, size_t reasonSize, NativeCaps& caps)
{
    return ProbeAdapterCaps(adapter, reason, reasonSize, &caps);
}

bool GetShadingRateCaps(ID3D11Device* device, ShadingRateCaps* caps)
{
    const NativeCaps* native = GetNativeCaps(device);
    if (!native || !caps)
        return false;
    caps->tier = native->vrsTier;
    caps->tileSize = native->vrsTileSize;
    caps->additionalRates = native->vrsAdditionalRates;
    return true;
}

ID3D12Device5* NativeDevice5(ID3D11Device* device)
{
    Device* dev = LayerDevice(device);
    return dev ? dev->d12_5 : nullptr;
}

// ============================================================================================
// Native sections
// ============================================================================================

namespace
{
Context* ContextOf(ID3D11DeviceContext* context)
{
    return static_cast<Context*>(context);
}

// The layer object behind a view of any kind (QueryInterface answers only the view's own kind).
template <class Iface>
bool ViewAs(ID3D11View* view, Iface*& out)
{
    out = nullptr;
    if (FAILED(view->QueryInterface(__uuidof(Iface), reinterpret_cast<void**>(&out))) || !out)
        return false;
    // The caller's reference keeps the view alive.
    out->Release();
    return true;
}

View* ViewOf(ID3D11View* view)
{
    if (!view)
        return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    if (ViewAs(view, srv))
        return View::From(srv);
    if (ViewAs(view, uav))
        return View::From(uav);
    if (ViewAs(view, rtv))
        return View::From(rtv);
    if (ViewAs(view, dsv))
        return View::From(dsv);
    return nullptr;
}

// Binds the ring and the sampler heap when the list has not got them (a new heap binding
// re-establishes the whole applied state, as the draw path does).
void BindLayerHeaps(Context& c, ID3D12GraphicsCommandList* list)
{
    Device* dev = c.device();
    if (!c.applied.heapsDirty && c.samplerGeneration == dev->samplerTables.Generation())
        return;
    u32 generation = 0;
    ID3D12DescriptorHeap* heaps[2] = {dev->gpuRing.Heap(), dev->samplerTables.Current(generation)};
    list->SetDescriptorHeaps(heaps[1] ? 2 : 1, heaps);
    c.InvalidateApplied();
    c.samplerGeneration = generation;
    c.applied.heapsDirty = false;
}

// Root arguments of a stage are undefined after its root signature changed.
void ResetStageRoot(StageBindings& sb)
{
    std::memset(sb.cbAppliedAddress, 0, sizeof(sb.cbAppliedAddress));
    std::memset(sb.cbAppliedVersion, 0, sizeof(sb.cbAppliedVersion));
    sb.cbDirty = sb.srvDirty = sb.samplerDirty = true;
}

void UntrackedTexture()
{
    static const char message[] = "native section: Require() on an untracked texture (staging) ignored";
    LogOnce(message, message);
}
} // namespace

bool BeginNative(ID3D11DeviceContext* context, NativeScope& scope)
{
    scope = NativeScope{};
    if (!context)
        return false;
    Context* c = ContextOf(context);
    Device* dev = c->device();
    if (dev->removed)
        return false;
    try
    {
        ID3D12GraphicsCommandList* list = c->List();
        if (!list)
            return false;
        BindLayerHeaps(*c, list);
        c->FlushBarriers();
        c->VrsNativeReset(); // [dx12:vrs] hook
        if (c->m_inNative)
        {
            static const char message[] = "native section opened inside another one";
            LogOnce(message, message);
        }
        c->m_inNative = true;
        dev->Count(Device::StatNativeSections);
        scope.list = list;
        scope.list4 = c->List4();
        scope.list5 = c->List5();
        scope.list6 = c->List6();
        scope.heap = dev->gpuRing.Heap();
        scope.bindless = dev->gpuRing.Gpu(0);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        Log("native section: out of memory");
        return false;
    }
}

void EndNative(ID3D11DeviceContext* context, UINT touched)
{
    if (!context)
        return;
    Context* c = ContextOf(context);
    Context::Applied& a = c->applied;
    if (touched & TouchedHeaps)
        c->InvalidateApplied();
    else
    {
        constexpr u32 kGraphicsStages = u32(Stage::CS);
        if (touched & TouchedGraphicsRoot)
        {
            // The mesh root signature is a graphics one too.
            a.rootSignatureGraphics = false;
            a.rootSignatureMesh = false;
            for (u32 s = 0; s < kGraphicsStages; ++s)
                ResetStageRoot(c->stages[s]);
            a.psUavDirty = true;
        }
        if (touched & TouchedComputeRoot)
        {
            a.rootSignatureCompute = false;
            ResetStageRoot(c->stages[u32(Stage::CS)]);
            a.csUavDirty = true;
        }
        if (touched & TouchedPipeline)
        {
            a.pso = nullptr;
            a.psoHash = 0;
            a.psoDirty = true;
        }
        if (touched & TouchedInputAssembler)
            a.vbDirty = a.ibDirty = a.topologyDirty = true;
        if (touched & TouchedOutputMerger)
            a.rtDirty = a.stencilRefDirty = a.blendFactorDirty = true;
        if (touched & TouchedRasterizer)
            a.viewportDirty = a.scissorDirty = true;
    }
    if (touched & TouchedShadingRate)
    {
        a.vrsDirty = true;
        a.vrsListNonDefault = true;
    }
    // Native code may have written UAVs: the next engine UAV access is ordered after it.
    a.uavWritten = true;
    c->m_inNative = false;
}

void Require(ID3D11DeviceContext* context, ID3D11Resource* resource, D3D12_RESOURCE_STATES state)
{
    RequireMany(context, &resource, 1, state);
}

void RequireMany(ID3D11DeviceContext* context, ID3D11Resource* const* resources, UINT count, D3D12_RESOURCE_STATES state)
{
    if (!context || !resources)
        return;
    Context* c = ContextOf(context);
    bool any = false;
    for (UINT i = 0; i < count; ++i)
    {
        Resource* r = Resource::From(resources[i]);
        if (!r)
            continue;
        r = r->Actual();
        // Pages and upload memory need no transitions (COMMON promotion, GENERIC_READ).
        if (!r->native)
        {
            if (!r->IsBuffer())
                UntrackedTexture();
            continue;
        }
        c->RequireAll(r, state);
        any = true;
    }
    if (!any)
        return;
    try
    {
        c->FlushBarriers();
    }
    catch (const std::bad_alloc&)
    {
        Log("native section: out of memory");
    }
}

void RequireView(ID3D11DeviceContext* context, ID3D11View* view, D3D12_RESOURCE_STATES state)
{
    if (!context)
        return;
    View* v = ViewOf(view);
    if (!v || !v->resource)
        return;
    Resource* r = v->resource->Actual();
    if (!r->native)
    {
        if (!r->IsBuffer())
            UntrackedTexture();
        return;
    }
    Context* c = ContextOf(context);
    c->Require(r, v->range, state);
    try
    {
        c->FlushBarriers();
    }
    catch (const std::bad_alloc&)
    {
        Log("native section: out of memory");
    }
}

void UavBarrier(ID3D11DeviceContext* context, ID3D11Resource* resource)
{
    if (!context)
        return;
    Context* c = ContextOf(context);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    // Resources without UAV access (pages, upload memory) get the global barrier.
    if (Resource* r = Resource::From(resource))
    {
        r = r->Actual();
        if (r->native && (r->nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            b.UAV.pResource = r->native;
    }
    try
    {
        c->FlushBarriers();
        if (ID3D12GraphicsCommandList* list = c->List())
        {
            list->ResourceBarrier(1, &b);
            c->device()->Count(Device::StatBarriers);
        }
    }
    catch (const std::bad_alloc&)
    {
        Log("native section: out of memory");
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE NativeDescriptor(ID3D11View* view)
{
    View* v = ViewOf(view);
    return v ? v->Descriptor() : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

bool NativeBufferRange(ID3D11Buffer* buffer, NativeBuffer& out)
{
    out = NativeBuffer{};
    Resource* r = Resource::From(buffer);
    if (!r || !r->IsBuffer())
        return false;
    out.size = r->byteWidth;
    if (r->native)
    {
        out.resource = r->native;
        out.tracked = true;
    }
    else if (r->usage == D3D11_USAGE_DYNAMIC)
    {
        out.resource = r->current.alloc.resource;
        out.offset = r->current.alloc.offset;
    }
    else if (r->usage == D3D11_USAGE_STAGING)
        out.resource = r->linear;
    else
    {
        out.resource = r->range.resource;
        out.offset = r->range.offset;
    }
    if (!out.resource)
    {
        out = NativeBuffer{};
        return false;
    }
    out.address = out.resource->GetGPUVirtualAddress() + out.offset;
    return true;
}

UINT64 TextureWriteGeneration(ID3D11Device* device)
{
    Device* dev = LayerDevice(device);
    return dev ? dev->textureWrites.load(std::memory_order_relaxed) : 0;
}

// ============================================================================================
// Descriptors and memory
// ============================================================================================

DescriptorSpan AllocateDescriptors(ID3D11DeviceContext* context, UINT count)
{
    if (!context || !count)
        return {};
    Context* c = ContextOf(context);
    Device* dev = c->device();
    // A quarter of the ring at most: one request must never stall on the ring itself.
    const u32 limit = (dev->gpuRing.Capacity() - dev->gpuRing.Base()) / 4;
    if (count > limit)
    {
        static const char message[] = "! AllocateDescriptors: %u descriptors exceed the per-request limit (%u)";
        LogOnce(message, message, count, limit);
        return {};
    }
    const u32 index = c->descriptors.Allocate(count, dev->frame.load(std::memory_order_relaxed));
    DescriptorSpan span;
    span.cpu = dev->gpuRing.Cpu(index);
    span.gpu = dev->gpuRing.Gpu(index);
    span.index = index;
    span.count = count;
    span.increment = dev->gpuRing.Increment();
    return span;
}

UploadSpan AllocateUpload(ID3D11DeviceContext* context, UINT64 size, UINT64 alignment)
{
    if (!context || !size)
        return {};
    Context* c = ContextOf(context);
    Device* dev = c->device();
    const UploadAlloc alloc = c->upload.Allocate(size, alignment ? alignment : 1, dev->frame.load(std::memory_order_acquire));
    if (!alloc.cpu)
        return {};
    UploadSpan span;
    span.cpu = alloc.cpu;
    span.gpu = alloc.gpu;
    span.resource = alloc.resource;
    span.offset = alloc.offset;
    return span;
}

// ---- bindless range ---------------------------------------------------------------------------

bool BindlessAllocator::Init(Device* device, u32 capacity)
{
    Shutdown();
    if (!device || !capacity)
        return false;
    try
    {
        m_owner.assign(capacity, 0);
        m_generation.assign(capacity, 0);
        m_free.assign(1, {0u, capacity});
        m_pending.reserve(64);
    }
    catch (const std::bad_alloc&)
    {
        Shutdown();
        Log("! bindless range: out of memory, native passes get no bindless tables");
        return false;
    }
    m_device = device;
    m_capacity = capacity;
    // Every slot starts as a null Texture2D SRV, so an unbounded table over the range is valid.
    constexpr u32 kBatch = 1024;
    D3D12_CPU_DESCRIPTOR_HANDLE sources[kBatch];
    UINT sizes[kBatch];
    for (u32 i = 0; i < kBatch; ++i)
    {
        sources[i] = device->nulls.srv[D3D12_SRV_DIMENSION_TEXTURE2D];
        sizes[i] = 1;
    }
    for (u32 first = 0; first < capacity; first += kBatch)
    {
        const UINT count = (std::min)(kBatch, capacity - first);
        const D3D12_CPU_DESCRIPTOR_HANDLE destination = device->gpuRing.Cpu(first);
        device->d12->CopyDescriptors(1, &destination, &count, count, sources, sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    return true;
}

void BindlessAllocator::Shutdown()
{
    std::lock_guard lock(m_lock);
    m_device = nullptr;
    m_capacity = 0;
    m_free.clear();
    m_owner.clear();
    m_generation.clear();
    m_pending.clear();
}

void BindlessAllocator::WriteNull(u32 index)
{
    m_device->d12->CopyDescriptorsSimple(1, m_device->gpuRing.Cpu(index), m_device->nulls.srv[D3D12_SRV_DIMENSION_TEXTURE2D],
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

u32 BindlessAllocator::Allocate(u32 count)
{
    if (!count)
        return kBindlessInvalid;
    std::lock_guard lock(m_lock);
    for (auto it = m_free.begin(); it != m_free.end(); ++it)
    {
        if (it->second < count)
            continue;
        const u32 index = it->first;
        it->first += count;
        it->second -= count;
        if (!it->second)
            m_free.erase(it);
        return index;
    }
    static const char message[] = "! bindless range full: %u slots requested (%u in all)";
    LogOnce(message, message, count, m_capacity);
    return kBindlessInvalid;
}

void BindlessAllocator::Release(u32 index, u32 count)
{
    // Sorted insert, merged with both neighbours.
    auto it = std::lower_bound(m_free.begin(), m_free.end(), std::make_pair(index, 0u));
    if (it != m_free.begin() && std::prev(it)->first + std::prev(it)->second == index)
    {
        --it;
        it->second += count;
    }
    else
        it = m_free.insert(it, {index, count});
    const auto next = std::next(it);
    if (next != m_free.end() && it->first + it->second == next->first)
    {
        it->second += next->second;
        m_free.erase(next);
    }
}

void BindlessAllocator::Free(u32 index, u32 count)
{
    std::lock_guard lock(m_lock);
    if (!Valid(index, count))
    {
        static const char message[] = "! FreeBindless(%u, %u) outside the range ignored";
        LogOnce(message, message, index, count);
        return;
    }
    for (u32 i = index; i < index + count; ++i)
    {
        m_owner[i] = 0;
        ++m_generation[i];
    }
    Pending p;
    p.frame = m_device->frame.load(std::memory_order_acquire);
    p.index = index;
    p.count = count;
    p.free = true;
    try
    {
        m_pending.push_back(p);
    }
    catch (const std::bad_alloc&)
    {
        // Lost rather than reused while the GPU may still read it.
        static const char message[] = "! bindless: out of memory, freed slots are lost";
        LogOnce(message, message);
    }
}

void BindlessAllocator::Write(u32 index, View* view)
{
    std::lock_guard lock(m_lock);
    if (!Valid(index, 1))
    {
        static const char message[] = "! WriteBindless(%u) outside the range ignored";
        LogOnce(message, message, index);
        return;
    }
    ++m_generation[index];
    // Back buffer views change their resource every frame: never bindless.
    if (!view || view->kind != ViewKind::SRV || !view->perBuffer.empty())
    {
        if (view)
        {
            static const char message[] = "! WriteBindless: only shader resource views of textures and buffers, slot left null";
            LogOnce(message, message);
        }
        WriteNull(index);
        m_owner[index] = 0;
        return;
    }
    m_device->d12->CopyDescriptorsSimple(1, m_device->gpuRing.Cpu(index), view->Descriptor(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (m_owner[index] == view->id)
        return;
    m_owner[index] = view->id;
    try
    {
        view->bindlessSlots.push_back(index);
    }
    catch (const std::bad_alloc&)
    {
        // Untracked: the slot keeps the descriptor after the view dies (the caller frees it first).
        m_owner[index] = 0;
    }
}

void BindlessAllocator::WriteNative(u32 index, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc)
{
    std::lock_guard lock(m_lock);
    if (!Valid(index, 1))
    {
        static const char message[] = "! WriteBindlessNative(%u) outside the range ignored";
        LogOnce(message, message, index);
        return;
    }
    ++m_generation[index];
    m_owner[index] = 0;
    if (!desc)
        WriteNull(index);
    else
        m_device->d12->CreateShaderResourceView(resource, desc, m_device->gpuRing.Cpu(index));
}

void BindlessAllocator::ForgetView(View& view)
{
    std::lock_guard lock(m_lock);
    bool dangling = false;
    for (const u32 slot : view.bindlessSlots)
    {
        if (slot >= m_capacity || m_owner[slot] != view.id)
            continue;
        m_owner[slot] = 0;
        dangling = true;
        Pending p;
        p.frame = m_device->frame.load(std::memory_order_acquire);
        p.index = slot;
        p.count = 1;
        p.generation = m_generation[slot];
        try
        {
            m_pending.push_back(p);
        }
        catch (const std::bad_alloc&)
        {
            // No room to wait: nulled at once (a freed view is worse than one early null).
            WriteNull(slot);
        }
    }
    view.bindlessSlots.clear();
    if (dangling)
    {
        static const char message[] = "! bindless: a view was released while slots still held it; they read null from the next frame";
        LogOnce(message, message);
    }
}

void BindlessAllocator::EndFrame(u64 frame, u64 fenceValue)
{
    std::lock_guard lock(m_lock);
    if (m_pending.empty() || !m_device)
        return;
    const u64 done = m_device->fence.Completed();
    size_t keep = 0;
    for (size_t i = 0; i < m_pending.size(); ++i)
    {
        Pending& p = m_pending[i];
        if (!p.fence && p.frame <= frame)
            p.fence = fenceValue;
        if (!p.fence || p.fence > done)
        {
            m_pending[keep++] = p;
            continue;
        }
        if (p.free)
        {
            for (u32 slot = p.index; slot < p.index + p.count; ++slot)
                WriteNull(slot);
            try
            {
                Release(p.index, p.count);
            }
            catch (const std::bad_alloc&)
            {
                static const char message[] = "! bindless: out of memory, freed slots are lost";
                LogOnce(message, message);
            }
        }
        // A dead view's slot: nulled unless it was written or freed since.
        else if (p.generation == m_generation[p.index])
            WriteNull(p.index);
    }
    m_pending.resize(keep);
}

UINT AllocateBindless(ID3D11Device* device, UINT count)
{
    Device* dev = LayerDevice(device);
    return dev ? dev->bindless.Allocate(count) : kBindlessInvalid;
}

void WriteBindless(ID3D11Device* device, UINT index, ID3D11ShaderResourceView* view)
{
    if (Device* dev = LayerDevice(device))
        dev->bindless.Write(index, View::From(view));
}

void WriteBindlessBuffer(ID3D11Device* device, UINT index, ID3D11Buffer* buffer)
{
    Device* dev = LayerDevice(device);
    if (!dev)
        return;
    NativeBuffer range;
    const Resource* r = Resource::From(buffer);
    // A dynamic buffer renames at every DISCARD: a slot would keep an old version.
    if (!r || r->usage == D3D11_USAGE_DYNAMIC || r->usage == D3D11_USAGE_STAGING || !NativeBufferRange(buffer, range))
    {
        static const char message[] = "! WriteBindlessBuffer: only static and GPU-written buffers, slot left null";
        LogOnce(message, message);
        dev->bindless.WriteNative(index, nullptr, nullptr);
        return;
    }
    // Raw views address 4-byte words; page ranges are 256-byte aligned. The last word is rounded up (an
    // odd count of 16-bit indices), which stays inside the 256-byte aligned allocation.
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = range.offset / 4;
    desc.Buffer.NumElements = UINT((range.size + 3) / 4);
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    dev->bindless.WriteNative(index, range.resource, desc.Buffer.NumElements ? &desc : nullptr);
}

void WriteBindlessNative(ID3D11Device* device, UINT index, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc)
{
    if (Device* dev = LayerDevice(device))
        dev->bindless.WriteNative(index, resource, &desc);
}

void FreeBindless(ID3D11Device* device, UINT index, UINT count)
{
    if (Device* dev = LayerDevice(device))
        dev->bindless.Free(index, count);
}

// ---- resources --------------------------------------------------------------------------------

namespace
{
HRESULT CreateBuffer(Device* dev, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial,
    const wchar_t* name, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    const HRESULT hr = dev->d12->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr))
    {
        *out = nullptr;
        Log("! native buffer '%ls' (%llu bytes, heap %d) failed (0x%08X)", name ? name : L"", static_cast<unsigned long long>(size),
            int(heap), unsigned(hr));
        if (hr == DXGI_ERROR_DEVICE_REMOVED)
            dev->ReportRemoval(hr);
        return hr;
    }
    if (name)
        (*out)->SetName(name);
    return S_OK;
}
} // namespace

HRESULT CreateNativeBuffer(ID3D11Device* device, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial, const wchar_t* name, ID3D12Resource** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    Device* dev = LayerDevice(device);
    if (!dev || !size)
        return E_INVALIDARG;
    return CreateBuffer(dev, size, heap, flags, initial, name, out);
}

HRESULT CreateAccelerationStructureBuffer(ID3D11Device* device, UINT64 size, const wchar_t* name, ID3D12Resource** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    Device* dev = LayerDevice(device);
    if (!dev || !size)
        return E_INVALIDARG;
    if (!dev->caps.raytracingTier)
        return DXGI_ERROR_UNSUPPORTED;
    constexpr UINT64 kAlign = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    return CreateBuffer(dev, (size + kAlign - 1) & ~(kAlign - 1), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, name, out);
}

void ReleaseNative(ID3D11Device* device, IUnknown* object)
{
    if (!object)
        return;
    if (Device* dev = LayerDevice(device))
        dev->releaser.Release(object);
    else
        object->Release();
}

bool QueryVideoMemory(ID3D11Device* device, UINT64& budget, UINT64& usage)
{
    budget = usage = 0;
    Device* dev = LayerDevice(device);
    if (!dev || !dev->adapter3)
        return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(dev->adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;
    budget = info.Budget;
    usage = info.CurrentUsage;
    return true;
}

// ============================================================================================
// Pipelines
// ============================================================================================

namespace
{
// Version 1.1 -> 1.0 for runtimes without 1.1 (flags dropped: 1.0 treats everything as volatile).
HRESULT SerializeAs10(const D3D12_ROOT_SIGNATURE_DESC1& src, ID3DBlob** blob, ID3DBlob** errors)
{
    if (!g_d3d12.SerializeRootSignature)
        return E_NOTIMPL;
    std::vector<D3D12_ROOT_PARAMETER> params(src.NumParameters);
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> ranges(src.NumParameters);
    for (UINT i = 0; i < src.NumParameters; ++i)
    {
        const D3D12_ROOT_PARAMETER1& p = src.pParameters[i];
        D3D12_ROOT_PARAMETER& d = params[i];
        d.ParameterType = p.ParameterType;
        d.ShaderVisibility = p.ShaderVisibility;
        switch (p.ParameterType)
        {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r)
            {
                const D3D12_DESCRIPTOR_RANGE1& range = p.DescriptorTable.pDescriptorRanges[r];
                ranges[i].push_back({range.RangeType, range.NumDescriptors, range.BaseShaderRegister, range.RegisterSpace,
                    range.OffsetInDescriptorsFromTableStart});
            }
            d.DescriptorTable.NumDescriptorRanges = UINT(ranges[i].size());
            d.DescriptorTable.pDescriptorRanges = ranges[i].data();
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: d.Constants = p.Constants; break;
        default:
            d.Descriptor.ShaderRegister = p.Descriptor.ShaderRegister;
            d.Descriptor.RegisterSpace = p.Descriptor.RegisterSpace;
            break;
        }
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = src.NumParameters;
    desc.pParameters = params.data();
    desc.NumStaticSamplers = src.NumStaticSamplers;
    desc.pStaticSamplers = src.pStaticSamplers;
    desc.Flags = src.Flags;
    return g_d3d12.SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, errors);
}

// Content hash of a root signature the cache made; foreign ones are kept alive (so their address is
// never reused under a cached pipeline) and keyed by address.
u64 RootKey(Device* dev, ID3D12RootSignature* root, bool& known)
{
    known = true;
    if (!root)
        return 0;
    std::lock_guard lock(dev->nativeLock);
    if (const auto it = dev->nativeRootHashes.find(root); it != dev->nativeRootHashes.end())
        return it->second;
    known = false;
    if (std::find(dev->foreignRoots.begin(), dev->foreignRoots.end(), root) == dev->foreignRoots.end())
    {
        dev->foreignRoots.push_back(root);
        root->AddRef();
    }
    return u64(reinterpret_cast<uintptr_t>(root));
}
} // namespace

ID3D12RootSignature* RootSignature(ID3D11Device* device, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, const wchar_t* name)
{
    Device* dev = LayerDevice(device);
    if (!dev || !g_d3d12.SerializeVersionedRootSignature)
        return nullptr;
    try
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = E_FAIL;
        if (desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && dev->caps.rootSignatureVersion < UINT(D3D_ROOT_SIGNATURE_VERSION_1_1))
            hr = SerializeAs10(desc.Desc_1_1, &blob, &errors);
        else
            hr = g_d3d12.SerializeVersionedRootSignature(&desc, &blob, &errors);
        if (FAILED(hr) || !blob)
        {
            Log("! native root signature '%ls' serialization failed (0x%08X): %.*s", name ? name : L"", unsigned(hr),
                errors ? int(errors->GetBufferSize()) : 0, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            if (errors)
                errors->Release();
            if (blob)
                blob->Release();
            return nullptr;
        }
        if (errors)
            errors->Release();
        const u64 hash = Hash64(blob->GetBufferPointer(), blob->GetBufferSize());
        {
            std::lock_guard lock(dev->nativeLock);
            if (const auto it = dev->nativeRootSignatures.find(hash); it != dev->nativeRootSignatures.end())
            {
                blob->Release();
                return it->second;
            }
        }
        ID3D12RootSignature* root = nullptr;
        hr = dev->d12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        blob->Release();
        if (FAILED(hr))
        {
            Log("! native root signature '%ls' refused (0x%08X)", name ? name : L"", unsigned(hr));
            return nullptr;
        }
        if (name)
            root->SetName(name);
        std::lock_guard lock(dev->nativeLock);
        // Another thread may have created the same one meanwhile.
        const auto [it, inserted] = dev->nativeRootSignatures.try_emplace(hash, root);
        if (!inserted)
        {
            root->Release();
            return it->second;
        }
        dev->nativeRootHashes.emplace(root, hash);
        return root;
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}

ID3D12PipelineState* ComputePipeline(ID3D11Device* device, ID3D12RootSignature* root, const void* dxil, size_t size,
    const wchar_t* name)
{
    Device* dev = LayerDevice(device);
    if (!dev || !dxil || !size)
        return nullptr;
    try
    {
        bool known = true;
        const u64 key = HashCombine(Hash64(dxil, size), RootKey(dev, root, known));
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.CS = {dxil, size};
        ID3D12PipelineState* pso = dev->pipelines.NativeCompute(key, desc, known);
        if (pso && name)
            pso->SetName(name);
        return pso;
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}

ID3D12PipelineState* StreamPipeline(ID3D11Device* device, const void* stream, size_t size, UINT64 contentHash, const wchar_t* name)
{
    Device* dev = LayerDevice(device);
    if (!dev || !stream || !size)
        return nullptr;
    D3D12_PIPELINE_STATE_STREAM_DESC desc{};
    desc.SizeInBytes = size;
    desc.pPipelineStateSubobjectStream = const_cast<void*>(stream);
    ID3D12PipelineState* pso = dev->pipelines.NativeStream(contentHash, desc, true);
    if (pso && name)
        pso->SetName(name);
    return pso;
}

ID3D12CommandSignature* CommandSignature(ID3D11Device* device, const D3D12_COMMAND_SIGNATURE_DESC& desc, ID3D12RootSignature* root)
{
    Device* dev = LayerDevice(device);
    if (!dev || !desc.NumArgumentDescs || !desc.pArgumentDescs)
        return nullptr;
    try
    {
        bool known = true;
        u64 key = HashCombine(RootKey(dev, root, known), (u64(desc.ByteStride) << 32) | desc.NodeMask);
        key = Hash64(desc.pArgumentDescs, sizeof(D3D12_INDIRECT_ARGUMENT_DESC) * desc.NumArgumentDescs, key);
        {
            std::lock_guard lock(dev->nativeLock);
            if (const auto it = dev->commandSignatures.find(key); it != dev->commandSignatures.end())
                return it->second;
        }
        ID3D12CommandSignature* signature = nullptr;
        const HRESULT hr = dev->d12->CreateCommandSignature(&desc, root, IID_PPV_ARGS(&signature));
        if (FAILED(hr))
        {
            Log("! command signature (stride %u, %u arguments) refused (0x%08X)", desc.ByteStride, desc.NumArgumentDescs, unsigned(hr));
            return nullptr;
        }
        std::lock_guard lock(dev->nativeLock);
        const auto [it, inserted] = dev->commandSignatures.try_emplace(key, signature);
        if (!inserted)
            signature->Release();
        return it->second;
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}

HRESULT RaytracingPipeline(ID3D11Device* device, const D3D12_STATE_OBJECT_DESC& desc, ID3D12StateObject** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    Device* dev = LayerDevice(device);
    if (!dev)
        return E_INVALIDARG;
    if (!dev->d12_5)
        return E_NOINTERFACE;
    if (!dev->caps.raytracingTier)
        return DXGI_ERROR_UNSUPPORTED;
    const HRESULT hr = dev->d12_5->CreateStateObject(&desc, IID_PPV_ARGS(out));
    if (FAILED(hr))
    {
        *out = nullptr;
        Log("! raytracing pipeline (%u subobjects) refused (0x%08X)", desc.NumSubobjects, unsigned(hr));
    }
    return hr;
}

void Device::NativeShutdown()
{
    bindless.Shutdown();
    for (ID3D12CommandSignature*& signature : indirectSignatures)
    {
        if (signature)
            signature->Release();
        signature = nullptr;
    }
    std::lock_guard lock(nativeLock);
    for (auto& [key, signature] : commandSignatures)
        signature->Release();
    commandSignatures.clear();
    for (auto& [hash, root] : nativeRootSignatures)
        root->Release();
    nativeRootSignatures.clear();
    nativeRootHashes.clear();
    for (ID3D12RootSignature* root : foreignRoots)
        root->Release();
    foreignRoots.clear();
    if (adapter3)
    {
        adapter3->Release();
        adapter3 = nullptr;
    }
}

// ============================================================================================
// D3D11-path extensions
// ============================================================================================

void Device::NativeInit()
{
    // One argument each, no root arguments: no root signature, any bound pipeline.
    static constexpr D3D12_INDIRECT_ARGUMENT_TYPE kTypes[] = {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
        D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    static constexpr UINT kStrides[] = {sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), sizeof(D3D12_DRAW_ARGUMENTS),
        sizeof(D3D12_DISPATCH_ARGUMENTS)};
    static_assert(std::size(kTypes) == std::size(indirectSignatures));
    for (u32 i = 0; i < std::size(kTypes); ++i)
    {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = kTypes[i];
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = kStrides[i];
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &argument;
        const HRESULT hr = d12->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&indirectSignatures[i]));
        if (FAILED(hr))
        {
            indirectSignatures[i] = nullptr;
            Log("! indirect command signature %u refused (0x%08X): those indirect calls are skipped", i, unsigned(hr));
        }
    }
}

// ============================================================================================
// Tracked buffers
// ============================================================================================

void Device::TrackBuffer(Resource* buffer)
{
    try
    {
        std::lock_guard lock(trackedBuffersLock);
        buffer->trackedBufferSlot = u32(trackedBuffers.size());
        trackedBuffers.push_back(buffer);
    }
    catch (const std::bad_alloc&)
    {
        buffer->trackedBufferSlot = ~0u;
        static const char message[] = "! out of memory while registering a tracked buffer: its states may go stale";
        LogOnce(message, message);
    }
}

void Device::UntrackBuffer(Resource* buffer)
{
    std::lock_guard lock(trackedBuffersLock);
    const u32 slot = buffer->trackedBufferSlot;
    if (slot >= trackedBuffers.size())
        return;
    Resource* last = trackedBuffers.back();
    trackedBuffers[slot] = last;
    last->trackedBufferSlot = slot;
    trackedBuffers.pop_back();
    buffer->trackedBufferSlot = ~0u;
}

// Buffers decay to COMMON when an ExecuteCommandLists completes; work recorded from here on starts
// from COMMON, so explicit barriers name the right before-state. Resource::states belongs to the
// immediate context: only it calls this.
void Device::DecayTrackedBuffers()
{
    std::lock_guard lock(trackedBuffersLock);
    for (Resource* r : trackedBuffers)
    {
        r->states.uniform = true;
        r->states.all = D3D12_RESOURCE_STATE_COMMON;
    }
}

// ============================================================================================
// D3D11-path extensions
// ============================================================================================

void DrawIndexedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount, ID3D11Buffer* count,
    UINT countOffset)
{
    if (context)
        ContextOf(context)->Indirect(Context::IndirectKind::DrawIndexed, args, offset, maxCount, count, countOffset);
}

void DrawInstancedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount, ID3D11Buffer* count,
    UINT countOffset)
{
    if (context)
        ContextOf(context)->Indirect(Context::IndirectKind::Draw, args, offset, maxCount, count, countOffset);
}

void DispatchIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount, ID3D11Buffer* count,
    UINT countOffset)
{
    if (context)
        ContextOf(context)->Indirect(Context::IndirectKind::Dispatch, args, offset, maxCount, count, countOffset);
}

HRESULT CreateAccelerationStructureView(ID3D11Device* device, ID3D12Resource* tlas, ID3D11ShaderResourceView** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    Device* dev = LayerDevice(device);
    if (!dev)
        return E_INVALIDARG;
    ShaderResourceView* view = nullptr;
    const HRESULT hr = ShaderResourceView::CreateAccelerationStructure(dev, tlas, &view);
    if (SUCCEEDED(hr))
        *out = view;
    return hr;
}
} // namespace xrd3d12
