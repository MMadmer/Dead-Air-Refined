// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `infra` lane (docs/dead-air/dx12-features/infra.md 9.1). Scenarios() appends
// this list.
#include "harness.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

template <class T>
bool Feature(ID3D12Device* device, D3D12_FEATURE feature, T& data)
{
    return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(data)));
}

// 1. GetNativeCaps equals direct CheckFeatureSupport; every mask bit zeroes its fields only.
void ScenarioNativeCaps(Gpu& g, Result& r)
{
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(g.dev);
    ID3D12Device* device = xrd3d12::NativeDevice(g.dev);
    r.Check(caps && device, "GetNativeCaps/NativeDevice answered null for the layer device");
    REQUIRE_OK();

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    Feature(device, D3D12_FEATURE_D3D12_OPTIONS1, o1);
    Feature(device, D3D12_FEATURE_D3D12_OPTIONS4, o4);
    Feature(device, D3D12_FEATURE_D3D12_OPTIONS5, o5);
    Feature(device, D3D12_FEATURE_D3D12_OPTIONS6, o6);
    Feature(device, D3D12_FEATURE_D3D12_OPTIONS7, o7);
    UINT model = 0x51;
    for (UINT m = UINT(D3D_HIGHEST_SHADER_MODEL); m >= UINT(D3D_SHADER_MODEL_6_0); --m)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL(m)};
        if (Feature(device, D3D12_FEATURE_SHADER_MODEL, sm))
        {
            model = UINT(sm.HighestShaderModel) >= 0x60 ? UINT(sm.HighestShaderModel) : 0x51;
            break;
        }
    }
    r.Check(caps->raytracingTier == UINT(o5.RaytracingTier), "raytracing tier %u, device %u", caps->raytracingTier,
        unsigned(o5.RaytracingTier));
    r.Check(caps->vrsTier == UINT(o6.VariableShadingRateTier), "VRS tier %u, device %u", caps->vrsTier,
        unsigned(o6.VariableShadingRateTier));
    r.Check(caps->vrsTileSize == (o6.VariableShadingRateTier >= 2 ? o6.ShadingRateImageTileSize : 0u), "VRS tile %u",
        caps->vrsTileSize);
    r.Check(caps->meshShaderTier == UINT(o7.MeshShaderTier), "mesh shader tier %u, device %u", caps->meshShaderTier,
        unsigned(o7.MeshShaderTier));
    r.Check(caps->shaderModel == model, "shader model 0x%X, device 0x%X", caps->shaderModel, model);
    r.Check(caps->waveOps == (o1.WaveOps != FALSE) && (!o1.WaveOps || caps->waveLanesMax == o1.WaveLaneCountMax),
        "wave ops %d/%u, device %d/%u", int(caps->waveOps), caps->waveLanesMax, int(o1.WaveOps), o1.WaveLaneCountMax);
    r.Check(caps->native16Bit == (o4.Native16BitShaderOpsSupported != FALSE), "native 16-bit ops %d", int(caps->native16Bit));
    r.Check(caps->dxcShaderModel == xrd3d12::DxcShaderModel(), "DXC shader model 0x%X, service 0x%X", caps->dxcShaderModel,
        xrd3d12::DxcShaderModel());

    xrd3d12::ShadingRateCaps vrs;
    r.Check(xrd3d12::GetShadingRateCaps(g.dev, &vrs) && vrs.tier == caps->vrsTier && vrs.tileSize == caps->vrsTileSize,
        "GetShadingRateCaps differs from NativeCaps");

    // The probe sees the same adapter through a throw-away device.
    xrd3d12::NativeCaps probe;
    char reason[128]{};
    r.Check(xrd3d12::ProbeAdapter(Adapter(), reason, sizeof(reason), probe), "ProbeAdapter refused: %s", reason);
    r.Check(probe.raytracingTier == caps->raytracingTier && probe.vrsTier == caps->vrsTier &&
            probe.meshShaderTier == caps->meshShaderTier && probe.shaderModel == caps->shaderModel && !probe.dxcShaderModel,
        "the probe's capabilities differ from the device's");

    // Each switch zeroes its fields and leaves the rest alone.
    struct Mask
    {
        UINT bit;
        const char* name;
        bool (*zeroed)(const xrd3d12::NativeCaps&);
        bool (*kept)(const xrd3d12::NativeCaps& masked, const xrd3d12::NativeCaps& full);
    };
    const Mask masks[] = {
        {xrd3d12::NativeFeatureRaytracing, "rt", [](const xrd3d12::NativeCaps& c) { return c.raytracingTier == 0; },
            [](const xrd3d12::NativeCaps& m, const xrd3d12::NativeCaps& f) { return m.vrsTier == f.vrsTier && m.shaderModel == f.shaderModel; }},
        {xrd3d12::NativeFeatureVrs, "vrs",
            [](const xrd3d12::NativeCaps& c) { return c.vrsTier == 0 && c.vrsTileSize == 0 && !c.vrsAdditionalRates; },
            [](const xrd3d12::NativeCaps& m, const xrd3d12::NativeCaps& f) { return m.raytracingTier == f.raytracingTier && m.meshShaderTier == f.meshShaderTier; }},
        {xrd3d12::NativeFeatureMeshShaders, "ms", [](const xrd3d12::NativeCaps& c) { return c.meshShaderTier == 0; },
            [](const xrd3d12::NativeCaps& m, const xrd3d12::NativeCaps& f) { return m.vrsTier == f.vrsTier && m.raytracingTier == f.raytracingTier; }},
        {xrd3d12::NativeFeatureAsyncCompute, "async", [](const xrd3d12::NativeCaps& c) { return !c.asyncCompute; },
            [](const xrd3d12::NativeCaps& m, const xrd3d12::NativeCaps& f) { return m.shaderModel == f.shaderModel && m.vrsTier == f.vrsTier; }},
        {xrd3d12::NativeFeatureSm6, "sm6",
            [](const xrd3d12::NativeCaps& c) { return c.shaderModel == 0x51 && c.dxcShaderModel == 0 && !c.waveOps && !c.heapIndexing; },
            [](const xrd3d12::NativeCaps& m, const xrd3d12::NativeCaps& f) { return m.raytracingTier == f.raytracingTier && m.vrsTier == f.vrsTier; }},
    };
    for (const Mask& mask : masks)
    {
        xrd3d12::NativeCaps masked = *caps;
        xrd3d12::MaskNativeCaps(masked, mask.bit);
        r.Check(mask.zeroed(masked), "mask %s left its capability set", mask.name);
        r.Check(mask.kept(masked, *caps), "mask %s changed another capability", mask.name);
    }
    r.Value("raytracing tier", caps->raytracingTier);
    r.Value("shader model", caps->shaderModel);
    r.Value("dxc shader model", caps->dxcShaderModel);
}

// ---- native passes on the layer ----------------------------------------------------------------

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;

const char* const kNativeHlsl = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VsOut FullVs(uint id : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 UvPs(VsOut i) : SV_Target
{
    return float4(i.uv, 0.5, 1);
}
float4 MipsPs(VsOut i) : SV_Target
{
    return tex.SampleLevel(smp, i.uv, i.uv.x < 0.5 ? 0.0 : 1.0);
}
float4 RedPs(VsOut i) : SV_Target
{
    return float4(1, 0, 0, 1);
}

RWTexture2D<float4> dst : register(u0);
[numthreads(8, 8, 1)]
void MipCs(uint3 id : SV_DispatchThreadID)
{
    dst[id.xy] = float4(id.x * (8.0 / 255.0), id.y * (8.0 / 255.0), 0.25, 1);
}
)";

const char* const kIdsHlsl = R"(
RWTexture2D<uint> ids : register(u0);
[numthreads(8, 8, 1)]
void IdCs(uint3 id : SV_DispatchThreadID)
{
    ids[id.xy] = id.x + id.y * 16 + 7;
}
[numthreads(8, 8, 1)]
void DoubleCs(uint3 id : SV_DispatchThreadID)
{
    ids[id.xy] = ids[id.xy] * 2 + 1;
}
)";

// SM 5.1: an unbounded table in space100 (the bindless range), a root UAV, root constants.
const char* const kBindlessHlsl = R"(
cbuffer Range : register(b0)
{
    uint first;
    uint count;
};
Texture2D<uint> slots[] : register(t0, space100);
RWByteAddressBuffer sum : register(u0);
[numthreads(1, 1, 1)]
void SumCs()
{
    uint total = 0;
    for (uint i = 0; i < count; ++i)
        total += slots[first + i].Load(int3(0, 0, 0));
    sum.Store(0, total);
}
RWByteAddressBuffer values : register(u1);
[numthreads(64, 1, 1)]
void FillCs(uint3 id : SV_DispatchThreadID)
{
    values.Store(id.x * 4, id.x * 5 + 3);
}
)";

D3D12_ROOT_PARAMETER1 UavTableParam(const D3D12_DESCRIPTOR_RANGE1* range)
{
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1;
    p.DescriptorTable.pDescriptorRanges = range;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return p;
}

ID3D12RootSignature* NativeRoot(Gpu& g, Result& r, const D3D12_ROOT_PARAMETER1* params, UINT count, const wchar_t* name,
    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE)
{
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{};
    d.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    d.Desc_1_1.NumParameters = count;
    d.Desc_1_1.pParameters = params;
    d.Desc_1_1.Flags = flags;
    ID3D12RootSignature* root = xrd3d12::RootSignature(g.dev, d, name);
    r.Check(root, "RootSignature(%ls) failed", name);
    return root;
}

// FXC cs_5_1 with unbounded descriptor tables allowed (kept for the process, like Compile()).
ID3DBlob* CompileNativeCs(const char* source, const char* entry)
{
    static std::map<std::string, ID3DBlob*> cache;
    const std::string key = entry;
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), entry, nullptr, nullptr, entry, "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code, &errors);
    if (FAILED(hr))
    {
        std::printf("FATAL shader %s (cs_5_1) does not compile: %s\n", entry,
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        std::exit(3);
    }
    if (errors)
        errors->Release();
    cache.emplace(key, code);
    return code;
}

ID3D12PipelineState* NativeCompute(Gpu& g, Result& r, ID3D12RootSignature* root, const char* source, const char* entry)
{
    ID3DBlob* code = CompileNativeCs(source, entry);
    ID3D12PipelineState* pso = xrd3d12::ComputePipeline(g.dev, root, code->GetBufferPointer(), code->GetBufferSize(), L"layertest native");
    r.Check(pso, "ComputePipeline(%s) failed", entry);
    return pso;
}

// A one-descriptor table of a D3D11 view in the context's ring space.
D3D12_GPU_DESCRIPTOR_HANDLE ViewTable(Gpu& g, ID3D11DeviceContext* ctx, ID3D11View* view)
{
    const xrd3d12::DescriptorSpan span = xrd3d12::AllocateDescriptors(ctx, 1);
    xrd3d12::NativeDevice(g.dev)->CopyDescriptorsSimple(1, span.cpu, xrd3d12::NativeDescriptor(view),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return span.gpu;
}

// UAV root table + the pipeline of `entry`: one native dispatch writing `uav` on `ctx`.
void NativeUavDispatch(Gpu& g, Result& r, ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* uav, const char* source,
    const char* entry, UINT groupsX, UINT groupsY)
{
    const D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    const D3D12_ROOT_PARAMETER1 param = UavTableParam(&range);
    ID3D12RootSignature* root = NativeRoot(g, r, &param, 1, L"layertest uav table");
    ID3D12PipelineState* pso = root ? NativeCompute(g, r, root, source, entry) : nullptr;
    if (!pso)
        return;
    xrd3d12::NativeScope scope;
    if (!r.Check(xrd3d12::BeginNative(ctx, scope), "BeginNative refused"))
        return;
    r.Check(scope.list && scope.heap == LayerDevice()->gpuRing.Heap(), "the scope has no list or not the layer's heap");
    xrd3d12::RequireView(ctx, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = ViewTable(g, ctx, uav);
    scope.list->SetComputeRootSignature(root);
    scope.list->SetPipelineState(pso);
    scope.list->SetComputeRootDescriptorTable(0, table);
    scope.list->Dispatch(groupsX, groupsY, 1);
    xrd3d12::EndNative(ctx, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
}

// Native buffer contents through a readback copy (layer only).
std::vector<u32> ReadNativeBuffer(Gpu& g, Result& r, ID3D12Resource* buffer, D3D12_RESOURCE_STATES state, UINT words)
{
    std::vector<u32> out;
    ID3D12Resource* readback = nullptr;
    if (!r.Ok(xrd3d12::CreateNativeBuffer(g.dev, words * 4ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                  D3D12_RESOURCE_STATE_COPY_DEST, L"layertest readback", &readback),
            "CreateNativeBuffer(readback)"))
        return out;
    xrd3d12::NativeScope scope;
    if (r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buffer;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            scope.list->ResourceBarrier(1, &b);
        scope.list->CopyBufferRegion(readback, 0, buffer, 0, words * 4ull);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            scope.list->ResourceBarrier(1, &b);
        xrd3d12::EndNative(g.ctx, 0);
    }
    xrd3d12::Flush(g.ctx, true);
    void* mapped = nullptr;
    const D3D12_RANGE whole{0, words * 4ull};
    if (r.Ok(readback->Map(0, &whole, &mapped), "Map(readback)"))
    {
        out.assign(static_cast<const u32*>(mapped), static_cast<const u32*>(mapped) + words);
        const D3D12_RANGE nothing{0, 0};
        readback->Unmap(0, &nothing);
    }
    xrd3d12::ReleaseNative(g.dev, readback);
    return out;
}

// 2. Deferred context: D3D11 draw into mip 0, a native CS writes mip 1 through RequireView; the list
// is executed and the immediate context samples both mips. Reference: the same with D3D11 compute.
void ScenarioNativeDeferred(Gpu& g, Result& r)
{
    auto tex = MakeTexture2D(g, r,
        Desc2D(64, 64, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 2));
    auto target = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    REQUIRE_OK();
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = kRgba8;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    auto mip0 = MakeRtv(g, r, tex.Get(), &rtvDesc);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = kRgba8;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 1;
    ComPtr<ID3D11UnorderedAccessView> mip1;
    r.Ok(g.dev->CreateUnorderedAccessView(tex.Get(), &uavDesc, &mip1), "CreateUnorderedAccessView(mip 1)");
    auto texSrv = MakeSrv(g, r, tex.Get());
    auto targetRtv = MakeRtv(g, r, target.Get());
    auto smp = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    auto vs = MakeVs(g, r, kNativeHlsl, "FullVs");
    auto uvPs = MakePs(g, r, kNativeHlsl, "UvPs");
    auto mipsPs = MakePs(g, r, kNativeHlsl, "MipsPs");
    auto cs = MakeCs(g, r, kNativeHlsl, "MipCs");
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();

    def->OMSetRenderTargets(1, mip0.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
    def->RSSetViewports(1, &vp);
    def->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    def->VSSetShader(vs.Get(), nullptr, 0);
    def->PSSetShader(uvPs.Get(), nullptr, 0);
    def->Draw(3, 0);
    def->OMSetRenderTargets(0, nullptr, nullptr);
    if (g.layer)
        NativeUavDispatch(g, r, def.Get(), mip1.Get(), kNativeHlsl, "MipCs", 4, 4);
    else
    {
        def->CSSetShader(cs.Get(), nullptr, 0);
        def->CSSetUnorderedAccessViews(0, 1, mip1.GetAddressOf(), nullptr);
        def->Dispatch(4, 4, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        def->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    }
    ComPtr<ID3D11CommandList> list;
    r.Ok(def->FinishCommandList(FALSE, &list), "FinishCommandList");
    REQUIRE_OK();
    g.ctx->ExecuteCommandList(list.Get(), FALSE);

    g.ctx->OMSetRenderTargets(1, targetRtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 64, 64);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(mipsPs.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, texSrv.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, smp.GetAddressOf());
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* noSrv = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &noSrv);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.images.push_back(Read2D(g, r, target.Get(), 0, "both mips sampled"));
    r.images.push_back(Read2D(g, r, tex.Get(), 0, "mip 0 (D3D11 draw)"));
    r.images.push_back(Read2D(g, r, tex.Get(), 1, "mip 1 (compute)"));
}

// 3. Draw A, a native dispatch that touched only the compute root and the pipeline, draw B with the
// same bindings: equal to D3D11, and draw B re-sets only the pipeline (no heaps, no root CBVs).
void ScenarioNativeTouched(Gpu& g, Result& r)
{
    auto target = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    auto ids = MakeTexture2D(g, r, Desc2D(16, 16, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS));
    auto cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, target.Get());
    ComPtr<ID3D11UnorderedAccessView> idsUav;
    r.Ok(g.dev->CreateUnorderedAccessView(ids.Get(), nullptr, &idsUav), "CreateUnorderedAccessView");
    auto vs = MakeVs(g, r, kQuadHlsl, "QuadVs");
    auto ps = MakePs(g, r, kQuadHlsl, "ColorPs");
    auto cs = MakeCs(g, r, kIdsHlsl, "IdCs");
    REQUIRE_OK();
    const QuadConstants q = QuadPx(0, 0, 64, 64, 64, 64, 0.25f, 0.5f, 1.0f, 1.0f);
    Upload(g, cb.Get(), &q, sizeof(q));
    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), black);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    SetViewport(g, 0, 0, 32, 64);
    g.ctx->Draw(6, 0);

    xrd3d12::Device* dev = g.layer ? LayerDevice() : nullptr;
    u64 before[xrd3d12::Device::StatCount]{};
    if (g.layer)
    {
        NativeUavDispatch(g, r, g.ctx, idsUav.Get(), kIdsHlsl, "IdCs", 2, 2);
        const xrd3d12::Context::Applied& a = static_cast<xrd3d12::Context*>(g.ctx)->applied;
        r.Check(!a.heapsDirty && a.rootSignatureGraphics && !a.pso && a.uavWritten,
            "EndNative(compute root | pipeline) invalidated too much or too little (heaps %d, graphics root %d, pso %d, uav %d)",
            int(a.heapsDirty), int(a.rootSignatureGraphics), int(bool(a.pso)), int(a.uavWritten));
        dev->stats = true;
        for (u32 i = 0; i < xrd3d12::Device::StatCount; ++i)
            before[i] = dev->statCounters[i].load();
    }
    else
    {
        g.ctx->CSSetShader(cs.Get(), nullptr, 0);
        g.ctx->CSSetUnorderedAccessViews(0, 1, idsUav.GetAddressOf(), nullptr);
        g.ctx->Dispatch(2, 2, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        g.ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        g.ctx->CSSetShader(nullptr, nullptr, 0);
    }
    // One triangle of the quad: the halves differ.
    SetViewport(g, 32, 0, 32, 64);
    g.ctx->Draw(3, 0);
    if (g.layer)
    {
        const auto delta = [&](xrd3d12::Device::Stat s) { return dev->statCounters[s].load() - before[s]; };
        r.Check(delta(xrd3d12::Device::StatPso) == 1 && !delta(xrd3d12::Device::StatRootCbv) &&
                !delta(xrd3d12::Device::StatCbvTables) && !delta(xrd3d12::Device::StatSamplerTables),
            "draw B after the native section: %llu PSO sets, %llu root CBVs, %llu CBV tables, %llu sampler tables (1, 0, 0, 0 expected)",
            static_cast<unsigned long long>(delta(xrd3d12::Device::StatPso)),
            static_cast<unsigned long long>(delta(xrd3d12::Device::StatRootCbv)),
            static_cast<unsigned long long>(delta(xrd3d12::Device::StatCbvTables)),
            static_cast<unsigned long long>(delta(xrd3d12::Device::StatSamplerTables)));
        dev->stats = false;
    }
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.images.push_back(Read2D(g, r, target.Get(), 0, "draw A | draw B"));
    r.images.push_back(Read2D(g, r, ids.Get(), 0, "compute between the draws"));
}

// 4. A fresh deferred context: native CS writes U, then a D3D11 CS reads and rewrites U. The engine
// dispatch needs the UAV barrier EndNative arms (uavWritten).
void ScenarioNativeUavThenEngine(Gpu& g, Result& r)
{
    auto ids = MakeTexture2D(g, r, Desc2D(16, 16, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS));
    REQUIRE_OK();
    ComPtr<ID3D11UnorderedAccessView> uav;
    r.Ok(g.dev->CreateUnorderedAccessView(ids.Get(), nullptr, &uav), "CreateUnorderedAccessView");
    auto idCs = MakeCs(g, r, kIdsHlsl, "IdCs");
    auto doubleCs = MakeCs(g, r, kIdsHlsl, "DoubleCs");
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();
    if (g.layer)
    {
        NativeUavDispatch(g, r, def.Get(), uav.Get(), kIdsHlsl, "IdCs", 2, 2);
        r.Check(static_cast<xrd3d12::Context*>(def.Get())->applied.uavWritten, "EndNative left uavWritten unset");
    }
    else
    {
        def->CSSetShader(idCs.Get(), nullptr, 0);
        def->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        def->Dispatch(2, 2, 1);
    }
    def->CSSetShader(doubleCs.Get(), nullptr, 0);
    def->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    def->Dispatch(2, 2, 1);
    ComPtr<ID3D11CommandList> list;
    r.Ok(def->FinishCommandList(FALSE, &list), "FinishCommandList");
    REQUIRE_OK();
    g.ctx->ExecuteCommandList(list.Get(), FALSE);
    Image image = Read2D(g, r, ids.Get(), 0, "native then engine UAV");
    // (x + 16 y + 7) * 2 + 1 everywhere.
    bool exact = image.texels.size() == 16 * 16 * 4;
    for (u32 i = 0; exact && i < 256; ++i)
    {
        u32 value = 0;
        std::memcpy(&value, image.texels.data() + i * 4, 4);
        exact = value == ((i % 16) + (i / 16) * 16 + 7) * 2 + 1;
    }
    r.Check(exact, "the second dispatch did not see the first one's writes");
    r.images.push_back(std::move(image));
}

// 8. Bindless: 1000 slots written from SRVs and summed through the unbounded table; unwritten slots
// read null; a slot whose view died reads null after the frame; freed slots are reused.
void ScenarioBindless(Gpu& g, Result& r)
{
    xrd3d12::Device* dev = LayerDevice();
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(g.dev);
    r.Check(caps && caps->bindlessCapacity == dev->bindless.Capacity() && dev->bindless.Capacity() == dev->gpuRing.Base() &&
            dev->bindless.Capacity() >= 1024,
        "bindless capacity %u, ring base %u", caps ? caps->bindlessCapacity : 0u, dev->gpuRing.Base());
    REQUIRE_OK();

    D3D12_ROOT_PARAMETER1 params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    const D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 100,
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    params[2] = UavTableParam(&range);
    for (D3D12_ROOT_PARAMETER1& p : params)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ID3D12RootSignature* root = NativeRoot(g, r, params, 3, L"layertest bindless");
    ID3D12PipelineState* pso = root ? NativeCompute(g, r, root, kBindlessHlsl, "SumCs") : nullptr;
    ID3D12Resource* sum = nullptr;
    r.Ok(xrd3d12::CreateNativeBuffer(g.dev, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"layertest bindless sum", &sum),
        "CreateNativeBuffer(sum)");
    REQUIRE_OK();
    const auto sumOf = [&](u32 first, u32 count) -> u32 {
        xrd3d12::NativeScope scope;
        if (!r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
            return ~0u;
        r.Check(scope.bindless.ptr == dev->gpuRing.Gpu(0).ptr, "NativeScope::bindless is not the heap start");
        scope.list->SetComputeRootSignature(root);
        scope.list->SetPipelineState(pso);
        const u32 constants[2] = {first, count};
        scope.list->SetComputeRoot32BitConstants(0, 2, constants, 0);
        scope.list->SetComputeRootUnorderedAccessView(1, sum->GetGPUVirtualAddress());
        scope.list->SetComputeRootDescriptorTable(2, scope.bindless);
        scope.list->Dispatch(1, 1, 1);
        xrd3d12::EndNative(g.ctx, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
        const std::vector<u32> words = ReadNativeBuffer(g, r, sum, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 1);
        return words.empty() ? ~0u : words[0];
    };
    const auto endFrames = [&](u32 frames) {
        for (u32 i = 0; i < frames; ++i)
        {
            EndFrame(g);
            WaitIdle(g);
        }
    };

    constexpr u32 kSlots = 1000;
    std::vector<ComPtr<ID3D11Texture2D>> textures(kSlots);
    std::vector<ComPtr<ID3D11ShaderResourceView>> views(kSlots);
    u32 expected = 0;
    for (u32 i = 0; i < kSlots; ++i)
    {
        const u32 value = i * 3 + 1;
        expected += value;
        const D3D11_SUBRESOURCE_DATA data{&value, 4, 0};
        D3D11_TEXTURE2D_DESC d = Desc2D(1, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE);
        d.Usage = D3D11_USAGE_IMMUTABLE;
        textures[i] = MakeTexture2D(g, r, d, &data);
        if (textures[i])
            views[i] = MakeSrv(g, r, textures[i].Get());
    }
    REQUIRE_OK();
    const u32 base = xrd3d12::AllocateBindless(g.dev, kSlots);
    const u32 unwritten = xrd3d12::AllocateBindless(g.dev, 16);
    r.Check(base != xrd3d12::kBindlessInvalid && unwritten != xrd3d12::kBindlessInvalid, "AllocateBindless failed");
    REQUIRE_OK();
    for (u32 i = 0; i < kSlots; ++i)
        xrd3d12::WriteBindless(g.dev, base + i, views[i].Get());
    const u32 total = sumOf(base, kSlots);
    r.Check(total == expected, "sum over %u bindless slots: %u, expected %u", kSlots, total, expected);
    r.Check(sumOf(unwritten, 16) == 0, "unwritten slots do not read null");

    // A slot whose view is released first.
    const u32 lone = xrd3d12::AllocateBindless(g.dev, 1);
    {
        const u32 value = 5000;
        const D3D11_SUBRESOURCE_DATA data{&value, 4, 0};
        auto texture = MakeTexture2D(g, r, Desc2D(1, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE), &data);
        auto view = texture ? MakeSrv(g, r, texture.Get()) : nullptr;
        REQUIRE_OK();
        xrd3d12::WriteBindless(g.dev, lone, view.Get());
        r.Check(sumOf(lone, 1) == 5000, "a single written slot does not read its texture");
    }
    const int dangling = LayerLogCount("slots still held it");
    endFrames(4);
    r.Check(sumOf(lone, 1) == 0, "the slot of a released view still reads it after the frame");
    r.Check(dangling >= 1, "the released view with a bindless slot was not logged");

    // Freed slots come back after the frame's fence, null.
    xrd3d12::FreeBindless(g.dev, base, kSlots);
    const u32 early = xrd3d12::AllocateBindless(g.dev, kSlots);
    r.Check(early != base, "freed slots were reused before the frame ended");
    endFrames(4);
    const u32 again = xrd3d12::AllocateBindless(g.dev, kSlots);
    r.Check(again == base, "freed slots were not reused (%u, expected %u)", again, base);
    r.Check(sumOf(again, kSlots) == 0, "reused slots do not read null");
    xrd3d12::FreeBindless(g.dev, again, kSlots);
    xrd3d12::FreeBindless(g.dev, early, kSlots);
    xrd3d12::FreeBindless(g.dev, unwritten, 16);
    xrd3d12::FreeBindless(g.dev, lone, 1);
    xrd3d12::ReleaseNative(g.dev, sum);
    endFrames(4);
    r.Value("bindless capacity", caps->bindlessCapacity);
}

// Pipeline stream subobject (the layout CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT has).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T>
struct alignas(void*) StreamSub
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};

// Native pipelines and memory: root signature and pipeline caches, a stream PSO drawing into a D3D11
// render target, ExecuteIndirect through a cached command signature, a DXR state object, native and
// acceleration structure buffers, video memory, NativeBufferRange, TextureWriteGeneration.
void ScenarioNativePipelines(Gpu& g, Result& r)
{
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(g.dev);

    // Root signature cache: equal descriptions give the same object.
    D3D12_ROOT_PARAMETER1 uavParam{};
    uavParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    uavParam.Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    uavParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ID3D12RootSignature* fillRoot = NativeRoot(g, r, &uavParam, 1, L"layertest fill");
    ID3D12RootSignature* fillRoot2 = NativeRoot(g, r, &uavParam, 1, L"layertest fill again");
    ID3D12RootSignature* emptyRoot = NativeRoot(g, r, nullptr, 0, L"layertest empty",
        D3D12_ROOT_SIGNATURE_FLAG_NONE);
    r.Check(fillRoot == fillRoot2 && fillRoot != emptyRoot, "the root signature cache does not key by content");
    ID3D12PipelineState* fill = fillRoot ? NativeCompute(g, r, fillRoot, kBindlessHlsl, "FillCs") : nullptr;
    r.Check(!fill || fill == NativeCompute(g, r, fillRoot, kBindlessHlsl, "FillCs"), "the compute pipeline cache does not key by content");
    REQUIRE_OK();

    // ExecuteIndirect of a dispatch (2, 1, 1) from upload memory.
    D3D12_INDIRECT_ARGUMENT_DESC dispatchArg{};
    dispatchArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
    sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &dispatchArg;
    ID3D12CommandSignature* signature = xrd3d12::CommandSignature(g.dev, sigDesc, nullptr);
    r.Check(signature && signature == xrd3d12::CommandSignature(g.dev, sigDesc, nullptr), "CommandSignature failed or is not cached");
    ID3D12Resource* values = nullptr;
    r.Ok(xrd3d12::CreateNativeBuffer(g.dev, 512, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"layertest values", &values),
        "CreateNativeBuffer(values)");
    REQUIRE_OK();
    {
        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
        {
            const xrd3d12::UploadSpan args = xrd3d12::AllocateUpload(g.ctx, sizeof(D3D12_DISPATCH_ARGUMENTS), 256);
            r.Check(args.cpu && args.resource && !(args.gpu % 256) && args.resource->GetGPUVirtualAddress() + args.offset == args.gpu,
                "AllocateUpload: bad span");
            if (args.cpu)
            {
                const D3D12_DISPATCH_ARGUMENTS dispatch{2, 1, 1};
                std::memcpy(args.cpu, &dispatch, sizeof(dispatch));
                scope.list->SetComputeRootSignature(fillRoot);
                scope.list->SetPipelineState(fill);
                scope.list->SetComputeRootUnorderedAccessView(0, values->GetGPUVirtualAddress());
                scope.list->ExecuteIndirect(signature, 1, args.resource, args.offset, nullptr, 0);
            }
            xrd3d12::EndNative(g.ctx, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
        }
        const std::vector<u32> words = ReadNativeBuffer(g, r, values, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 128);
        bool exact = words.size() == 128;
        for (u32 i = 0; exact && i < 128; ++i)
            exact = words[i] == i * 5 + 3;
        r.Check(exact, "ExecuteIndirect of the fill dispatch wrote wrong values");
    }
    xrd3d12::ReleaseNative(g.dev, values);

    // A stream pipeline (VS/PS) draws natively into a D3D11 render target; then a D3D11 draw into
    // the same target re-applies its own root, pipeline, targets and viewport.
    auto target = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    auto rtv = target ? MakeRtv(g, r, target.Get()) : nullptr;
    auto cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto quadVs = MakeVs(g, r, kQuadHlsl, "QuadVs");
    auto colorPs = MakePs(g, r, kQuadHlsl, "ColorPs");
    REQUIRE_OK();
    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), black);
    const QuadConstants q = QuadPx(32, 0, 64, 64, 64, 64, 0, 1, 0, 1);
    Upload(g, cb.Get(), &q, sizeof(q));
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 64, 64);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(quadVs.Get(), nullptr, 0);
    g.ctx->PSSetShader(colorPs.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());

    ID3DBlob* vsCode = Compile(kNativeHlsl, "FullVs", "vs_5_1");
    ID3DBlob* psCode = Compile(kNativeHlsl, "RedPs", "ps_5_1");
    struct Stream
    {
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sampleMask;
        StreamSub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    } stream;
    stream.root.value = emptyRoot;
    stream.vs.value = {vsCode->GetBufferPointer(), vsCode->GetBufferSize()};
    stream.ps.value = {psCode->GetBufferPointer(), psCode->GetBufferSize()};
    stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
    stream.raster.value.CullMode = D3D12_CULL_MODE_NONE;
    stream.raster.value.DepthClipEnable = TRUE;
    stream.depth.value.DepthEnable = FALSE;
    stream.depth.value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    stream.depth.value.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    stream.depth.value.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    stream.depth.value.BackFace = stream.depth.value.FrontFace;
    D3D12_RENDER_TARGET_BLEND_DESC& blend = stream.blend.value.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.sampleMask.value = 0xFFFFFFFFu;
    stream.sample.value = {1, 0};
    stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.formats.value.NumRenderTargets = 1;
    stream.formats.value.RTFormats[0] = kRgba8;
    const u64 streamHash = xrd3d12::HashCombine(xrd3d12::Hash64(vsCode->GetBufferPointer(), vsCode->GetBufferSize()),
        xrd3d12::Hash64(psCode->GetBufferPointer(), psCode->GetBufferSize()));
    ID3D12PipelineState* red = xrd3d12::StreamPipeline(g.dev, &stream, sizeof(stream), streamHash, L"layertest stream");
    r.Check(red && red == xrd3d12::StreamPipeline(g.dev, &stream, sizeof(stream), streamHash, nullptr),
        "StreamPipeline failed or is not cached");
    REQUIRE_OK();
    {
        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
        {
            xrd3d12::RequireView(g.ctx, rtv.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            const D3D12_CPU_DESCRIPTOR_HANDLE handle = xrd3d12::NativeDescriptor(rtv.Get());
            r.Check(handle.ptr, "NativeDescriptor of an RTV is null");
            scope.list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            const D3D12_VIEWPORT left{0, 0, 32, 64, 0, 1};
            const D3D12_RECT scissor{0, 0, 32, 64};
            scope.list->RSSetViewports(1, &left);
            scope.list->RSSetScissorRects(1, &scissor);
            scope.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            scope.list->SetGraphicsRootSignature(emptyRoot);
            scope.list->SetPipelineState(red);
            scope.list->DrawInstanced(3, 1, 0, 0);
            xrd3d12::EndNative(g.ctx, xrd3d12::TouchedGraphicsRoot | xrd3d12::TouchedPipeline | xrd3d12::TouchedInputAssembler |
                    xrd3d12::TouchedOutputMerger | xrd3d12::TouchedRasterizer);
        }
    }
    g.ctx->Draw(6, 0);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    Image image = Read2D(g, r, target.Get(), 0, "native stream PSO | D3D11 quad");
    const auto pixel = [&](u32 x, u32 y) {
        u32 value = 0;
        if (image.texels.size() == 64 * 64 * 4)
            std::memcpy(&value, image.texels.data() + (y * 64 + x) * 4, 4);
        return value;
    };
    r.Check(pixel(16, 32) == 0xFF0000FFu && pixel(48, 32) == 0xFF00FF00u, "left half 0x%08X (red expected), right half 0x%08X (green expected)",
        pixel(16, 32), pixel(48, 32));

    // A DXR state object: a ray generation and a miss shader from DXC.
    if (caps && caps->raytracingTier >= 10 && xrd3d12::DxcShaderModel() >= 0x63)
    {
        static const char kLibrary[] = R"(
RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> output : register(u0);
struct Payload
{
    float4 color;
};
[shader("raygeneration")]
void RayGen()
{
    Payload p;
    p.color = 0;
    RayDesc ray;
    ray.Origin = float3(0, 0, -1);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0;
    ray.TMax = 10;
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, p);
    output[DispatchRaysIndex().xy] = p.color;
}
[shader("miss")]
void Miss(inout Payload p)
{
    p.color = float4(0, 0, 1, 1);
}
)";
        xrd3d12::ShaderCompile request;
        request.source = kLibrary;
        request.size = sizeof(kLibrary) - 1;
        request.name = "layertest.lib";
        request.target = "lib_6_3";
        request.preprocessed = true;
        xrd3d12::ShaderOutput library;
        if (r.Check(xrd3d12::CompileShader(request, library), "lib_6_3 does not compile: %s", library.messages.c_str()))
        {
            D3D12_ROOT_PARAMETER1 rtParams[2]{};
            rtParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            rtParams[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
            const D3D12_DESCRIPTOR_RANGE1 outRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
            rtParams[1] = UavTableParam(&outRange);
            rtParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            ID3D12RootSignature* global = NativeRoot(g, r, rtParams, 2, L"layertest rt global");
            D3D12_DXIL_LIBRARY_DESC lib{};
            lib.DXILLibrary = {library.object.data(), library.object.size()};
            D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{16, 8};
            D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
            D3D12_GLOBAL_ROOT_SIGNATURE globalRoot{global};
            const D3D12_STATE_SUBOBJECT subobjects[] = {
                {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
                {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
                {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot},
            };
            D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(std::size(subobjects)), subobjects};
            ID3D12StateObject* state = nullptr;
            if (global && r.Ok(xrd3d12::RaytracingPipeline(g.dev, desc, &state), "RaytracingPipeline"))
            {
                ID3D12StateObjectProperties* properties = nullptr;
                if (r.Ok(state->QueryInterface(IID_PPV_ARGS(&properties)), "ID3D12StateObjectProperties"))
                {
                    r.Check(properties->GetShaderIdentifier(L"RayGen") && properties->GetShaderIdentifier(L"Miss"),
                        "the state object has no shader identifiers");
                    properties->Release();
                }
                xrd3d12::ReleaseNative(g.dev, state);
            }
        }
        ID3D12Resource* as = nullptr;
        if (r.Ok(xrd3d12::CreateAccelerationStructureBuffer(g.dev, 1000, L"layertest as", &as), "CreateAccelerationStructureBuffer"))
        {
            r.Check(as->GetDesc().Width == 1024 && (as->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                "acceleration structure buffer: width %llu, flags 0x%X", static_cast<unsigned long long>(as->GetDesc().Width),
                unsigned(as->GetDesc().Flags));
            xrd3d12::ReleaseNative(g.dev, as);
        }
    }

    // Video memory of the adapter.
    UINT64 budget = 0, usage = 0;
    r.Check(xrd3d12::QueryVideoMemory(g.dev, budget, usage) && budget > usage && usage, "QueryVideoMemory: budget %llu, usage %llu",
        static_cast<unsigned long long>(budget), static_cast<unsigned long long>(usage));

    // NativeBufferRange of the three buffer kinds; a page range is read natively.
    u32 words[64];
    for (u32 i = 0; i < 64; ++i)
        words[i] = i * 7 + 11;
    auto page = MakeBuffer(g, r, sizeof(words), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, words);
    auto dynamic = MakeBuffer(g, r, 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto tracked = MakeBuffer(g, r, 256, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER);
    REQUIRE_OK();
    xrd3d12::NativeBuffer pageRange, dynamicRange, dynamicRange2, trackedRange;
    r.Check(xrd3d12::NativeBufferRange(page.Get(), pageRange) && !pageRange.tracked && pageRange.size == sizeof(words) &&
            pageRange.address == pageRange.resource->GetGPUVirtualAddress() + pageRange.offset,
        "NativeBufferRange of a static buffer");
    Upload(g, dynamic.Get(), words, 256);
    r.Check(xrd3d12::NativeBufferRange(dynamic.Get(), dynamicRange) && !dynamicRange.tracked, "NativeBufferRange of a dynamic buffer");
    Upload(g, dynamic.Get(), words, 256);
    r.Check(xrd3d12::NativeBufferRange(dynamic.Get(), dynamicRange2) && dynamicRange2.address != dynamicRange.address,
        "NativeBufferRange does not follow a DISCARD rename");
    r.Check(xrd3d12::NativeBufferRange(tracked.Get(), trackedRange) && trackedRange.tracked && !trackedRange.offset,
        "NativeBufferRange of a GPU-written buffer");
    ID3D12Resource* copy = nullptr;
    if (pageRange.resource && r.Ok(xrd3d12::CreateNativeBuffer(g.dev, sizeof(words), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_COPY_DEST, L"layertest page copy", &copy),
                                   "CreateNativeBuffer(copy)"))
    {
        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
        {
            scope.list->CopyBufferRegion(copy, 0, pageRange.resource, pageRange.offset, sizeof(words));
            xrd3d12::EndNative(g.ctx, 0);
        }
        const std::vector<u32> read = ReadNativeBuffer(g, r, copy, D3D12_RESOURCE_STATE_COPY_DEST, 64);
        r.Check(read.size() == 64 && !std::memcmp(read.data(), words, sizeof(words)), "the page range read natively differs");
        xrd3d12::ReleaseNative(g.dev, copy);
    }

    // TextureWriteGeneration: copies into sampled textures move it, render target draws do not.
    const UINT64 g0 = xrd3d12::TextureWriteGeneration(g.dev);
    auto sampled = MakeTexture2D(g, r, Desc2D(4, 4, kRgba8, D3D11_BIND_SHADER_RESOURCE));
    REQUIRE_OK();
    const UINT64 g1 = xrd3d12::TextureWriteGeneration(g.dev);
    u32 texels[16]{};
    g.ctx->UpdateSubresource(sampled.Get(), 0, nullptr, texels, 16, 0);
    const UINT64 g2 = xrd3d12::TextureWriteGeneration(g.dev);
    g.ctx->ClearRenderTargetView(rtv.Get(), black);
    const UINT64 g3 = xrd3d12::TextureWriteGeneration(g.dev);
    const D3D11_BOX corner{0, 0, 0, 4, 4, 1};
    g.ctx->CopySubresourceRegion(sampled.Get(), 0, 0, 0, 0, target.Get(), 0, &corner);
    const UINT64 g4 = xrd3d12::TextureWriteGeneration(g.dev);
    r.Check(g1 > g0 && g2 > g1 && g3 == g2 && g4 > g3, "TextureWriteGeneration %llu -> %llu (create) -> %llu (update) -> %llu (RT clear) -> %llu (copy)",
        static_cast<unsigned long long>(g0), static_cast<unsigned long long>(g1), static_cast<unsigned long long>(g2),
        static_cast<unsigned long long>(g3), static_cast<unsigned long long>(g4));
}

// 13. Creation-time uploads from three loader threads while the immediate context submits frames:
// textures (single mip and 16 -> 4 chains, IMMUTABLE and DEFAULT, some released at once) and buffers
// (static pages and view-carrying committed ones) read back their data. The uploads go through the copy
// queue and the direct queue waits for them (PLAN C23), or through the direct queue with -nocopyqueue;
// the layer checks the queue.
void ScenarioCreationUploads(Gpu& g, Result& r)
{
    constexpr u32 kThreads = 3, kPerThread = 8, kCount = kThreads * kPerThread, kSize = 16, kWords = 64;
    const auto texel = [](u32 index, u32 mip, u32 x, u32 y) {
        return 0xFF000000u | ((index * 37 + mip * 90) & 255) | (((x * 13 + index * 5) & 255) << 8) |
            (((y * 17 + mip * 60) & 255) << 16);
    };
    const auto word = [](u32 index, u32 i) { return index * 0x01000193u ^ (i * 0x9E3779B9u); };

    xrd3d12::Device* device = g.layer ? xrd3d12::Device::From(g.dev) : nullptr;
    u64 directBytes = 0, directSubmissions = 0, copyBytes = 0, copySubmissions = 0;
    if (device)
    {
        // Earlier scenarios' uploads leave the counters here.
        EndFrame(g);
        device->init.TakeStats(directBytes, directSubmissions);
        device->initCopy.TakeStats(copyBytes, copySubmissions);
    }

    auto busy = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    REQUIRE_OK();
    auto busyRtv = MakeRtv(g, r, busy.Get());
    REQUIRE_OK();

    struct Loaded
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = S_OK;
        const char* what = "";
    };
    std::vector<Loaded> loaded(kCount);
    std::atomic<u32> finished{0};
    std::thread threads[kThreads];
    for (u32 t = 0; t < kThreads; ++t)
    {
        threads[t] = std::thread([&, t] {
            for (u32 k = 0; k < kPerThread; ++k)
            {
                const u32 index = t * kPerThread + k;
                Loaded& l = loaded[index];
                const u32 mips = index & 1 ? 3 : 1;
                std::vector<u32> texels[3];
                D3D11_SUBRESOURCE_DATA init[3]{};
                for (u32 m = 0; m < mips; ++m)
                {
                    const u32 w = kSize >> m;
                    texels[m].resize(size_t(w) * w);
                    for (u32 y = 0; y < w; ++y)
                    {
                        for (u32 x = 0; x < w; ++x)
                            texels[m][size_t(y) * w + x] = texel(index, m, x, y);
                    }
                    init[m] = {texels[m].data(), w * 4, 0};
                }
                D3D11_TEXTURE2D_DESC td = Desc2D(kSize, kSize, kRgba8, D3D11_BIND_SHADER_RESOURCE, mips);
                td.Usage = index % 3 ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
                l.what = "CreateTexture2D";
                l.hr = g.dev->CreateTexture2D(&td, init, &l.texture);
                if (FAILED(l.hr))
                    break;
                // Released before any submission: the pending copy must keep its target alive.
                ComPtr<ID3D11Texture2D> dropped;
                l.what = "CreateTexture2D (dropped)";
                l.hr = g.dev->CreateTexture2D(&td, init, &dropped);
                if (FAILED(l.hr))
                    break;
                dropped.Reset();

                u32 words[kWords];
                for (u32 i = 0; i < kWords; ++i)
                    words[i] = word(index, i);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = sizeof(words);
                if (index & 1)
                {
                    bd.Usage = D3D11_USAGE_DEFAULT;
                    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                }
                else
                {
                    bd.Usage = D3D11_USAGE_IMMUTABLE;
                    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                }
                const D3D11_SUBRESOURCE_DATA data{words, 0, 0};
                l.what = "CreateBuffer";
                l.hr = g.dev->CreateBuffer(&bd, &data, &l.buffer);
                if (FAILED(l.hr))
                    break;
            }
            finished.fetch_add(1);
        });
    }
    // The immediate context keeps submitting frames while the loaders upload.
    for (u32 frame = 0; frame < 6 || finished.load() < kThreads; ++frame)
    {
        const float color[4] = {float(frame % 7) / 7.0f, 0.25f, 0.5f, 1.0f};
        g.ctx->ClearRenderTargetView(busyRtv.Get(), color);
        EndFrame(g);
    }
    for (std::thread& thread : threads)
        thread.join();
    for (u32 i = 0; i < kCount; ++i)
    {
        if (!r.Ok(loaded[i].hr, loaded[i].what))
            return;
    }

    // Atlas: mip 0 of every texture (8 a row), mips 1 and 2 of the chained ones below.
    auto atlas = MakeTexture2D(g, r, Desc2D(128, 64, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = kCount * kWords * 4;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    r.Ok(g.dev->CreateBuffer(&sd, nullptr, &staging), "staging CreateBuffer");
    REQUIRE_OK();
    auto atlasRtv = MakeRtv(g, r, atlas.Get());
    REQUIRE_OK();
    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(atlasRtv.Get(), black);
    for (u32 i = 0; i < kCount; ++i)
    {
        g.ctx->CopySubresourceRegion(atlas.Get(), 0, (i % 8) * kSize, (i / 8) * kSize, 0, loaded[i].texture.Get(), 0, nullptr);
        if (i & 1)
        {
            g.ctx->CopySubresourceRegion(atlas.Get(), 0, (i / 2) * 8, 48, 0, loaded[i].texture.Get(), 1, nullptr);
            g.ctx->CopySubresourceRegion(atlas.Get(), 0, (i / 2) * 4, 56, 0, loaded[i].texture.Get(), 2, nullptr);
        }
        g.ctx->CopySubresourceRegion(staging.Get(), 0, i * kWords * 4, 0, 0, loaded[i].buffer.Get(), 0, nullptr);
    }
    r.images.push_back(Read2D(g, r, atlas.Get(), 0, "textures uploaded by loader threads"));
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (r.Ok(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map of the staging buffer"))
    {
        const u32* read = static_cast<const u32*>(mapped.pData);
        u32 wrong = 0;
        for (u32 i = 0; i < kCount; ++i)
        {
            for (u32 w = 0; w < kWords; ++w)
                wrong += read[i * kWords + w] != word(i, w);
        }
        g.ctx->Unmap(staging.Get(), 0);
        r.Check(!wrong, "%u buffer words differ from their initial data", wrong);
        r.Value("buffer words differing", wrong);
    }

    // The uploads went to the queue the device was created for (depth-stencil textures excepted).
    if (device)
    {
        EndFrame(g);
        device->init.TakeStats(directBytes, directSubmissions);
        device->initCopy.TakeStats(copyBytes, copySubmissions);
        const bool copyMode = device->createInfo.copyQueueUploads;
        r.Check(!copyMode || device->copyQueue, "the device was created for a copy queue and has none");
        if (device->copyQueue)
            r.Check(copyBytes && copySubmissions && !directBytes, "copy queue mode: %llu bytes on the copy queue, %llu on the direct queue",
                static_cast<unsigned long long>(copyBytes), static_cast<unsigned long long>(directBytes));
        else
            r.Check(directBytes && directSubmissions && !copyBytes, "direct mode: %llu bytes on the direct queue, %llu on the copy queue",
                static_cast<unsigned long long>(directBytes), static_cast<unsigned long long>(copyBytes));
    }
}
} // namespace

const std::vector<Scenario>& NativeScenarios()
{
    static const std::vector<Scenario> list = {
        {"native caps: GetNativeCaps, masks, probe", ScenarioNativeCaps, true},
        {"native section on a deferred context: RequireView, mip written by native compute", ScenarioNativeDeferred, false},
        {"EndNative touched subsets: draw, native dispatch, draw", ScenarioNativeTouched, false},
        {"native UAV then engine UAV on a fresh deferred context", ScenarioNativeUavThenEngine, false},
        {"bindless: 1000 slots, null init, released views, free and reuse", ScenarioBindless, true},
        {"native pipelines: root signature, compute, stream, command signature, DXR, buffers", ScenarioNativePipelines, true},
        {"creation-time uploads from 3 loader threads while frames are submitted", ScenarioCreationUploads, false},
    };
    return list;
}
} // namespace layertest
