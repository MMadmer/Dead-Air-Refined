// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// r__native_selftest (da_native_selftest.h, docs/dead-air/dx12-features/infra.md 3.7). The command asks
// for the test; it runs at the start of the next rendered frame on the render thread, before the level
// renders, with embedded shaders (DXC through da_dxc_compile_native) and small resources of its own. It
// logs one line with every part ok, failed or skipped, and a line with the reason for every failure.
#include "stdafx.h"

#include "da_native_selftest.h"

#if defined(XR_D3D12)
#include "da_dxc.h"
#include "Layers/xrRenderDX12/d12_api.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if defined(XR_D3D12)
namespace
{
constexpr u32 kSize = 64;    // test targets are kSize x kSize
constexpr u32 kWords = 1024; // raw buffers of the async part
constexpr u32 kSlots = 1024; // bindless slots summed

constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// Owns one COM reference.
template <class T>
class Ref
{
public:
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { reset(); }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p; }
    T** put()
    {
        reset();
        return &m_p;
    }
    T* const* addr() const { return &m_p; }
    void reset()
    {
        if (m_p)
            m_p->Release();
        m_p = nullptr;
    }

private:
    T* m_p = nullptr;
};

// ---- shaders ----------------------------------------------------------------------------------------

const char kWaveHlsl[] = R"(
cbuffer SelfTest : register(b0)
{
    uint4 params;
};
RWTexture2D<float4> image : register(u0);
// Per texel: the wave's active lane count, the texel's coordinates, the part's tag.
[numthreads(8, 8, 1)]
void WaveCs(uint3 id : SV_DispatchThreadID)
{
    image[id.xy] = float4(float(WaveActiveSum(1u)), float(id.x), float(id.y), float(params.x));
}
)";

const char kDrawHlsl[] = R"(
Texture2D<float4> source : register(t0);
float4 FullVs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 CopyPs(float4 pos : SV_Position) : SV_Target
{
    return source.Load(int3(pos.xy, 0));
}
float4 PositionPs(float4 pos : SV_Position) : SV_Target
{
    return float4(pos.xy, 0, 1);
}
struct StripOut
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
};
static const float2 kCorners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
// SV_VertexID is the index value (BaseVertexLocation is not in it): draw k starts at index 6k and paints
// the 16 texels wide strip k.
StripOut StripVs(uint id : SV_VertexID)
{
    uint strip = id / 6;
    float2 c = kCorners[id % 6];
    float2 texel = float2((strip + c.x) * 16.0, c.y * 64.0);
    StripOut o;
    o.pos = float4(texel.x / 32.0 - 1.0, 1.0 - texel.y / 32.0, 0, 1);
    o.color = float4((strip + 1) * 0.25, 0, 0, 1);
    return o;
}
float4 StripPs(StripOut i) : SV_Target
{
    return i.color;
}
)";

const char kFillHlsl[] = R"(
cbuffer SelfTest : register(b0)
{
    uint4 params;
};
RWByteAddressBuffer words : register(u0);
[numthreads(64, 1, 1)]
void FillCs(uint3 id : SV_DispatchThreadID)
{
    words.Store(id.x * 4, id.x * 3 + 7 + params.x);
}
)";

const char kBindlessHlsl[] = R"(
cbuffer SelfTest : register(b0)
{
    uint4 params; // x first slot, y slot count
};
Texture2D<uint> slots[] : register(t0, space100);
RWByteAddressBuffer total : register(u0);
[numthreads(1, 1, 1)]
void SumCs()
{
    uint sum = 0;
    for (uint i = 0; i < params.y; ++i)
        sum += slots[params.x + i].Load(int3(0, 0, 0));
    total.Store(0, sum);
}
)";

const char kTraceHlsl[] = R"(
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer hits : register(u0);
// One ray per texel of a 64 x 64 grid over [-1, 1]^2 towards +z: 1 + 100 t on a hit, 0 on a miss.
[numthreads(8, 8, 1)]
void TraceCs(uint3 id : SV_DispatchThreadID)
{
    RayDesc ray;
    ray.Origin = float3((float2(id.xy) + 0.5) / 32.0 - 1.0, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0;
    ray.TMax = 10;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    uint value = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1 + uint(q.CommittedRayT() * 100.0 + 0.5) : 0;
    hits.Store((id.y * 64 + id.x) * 4, value);
}
)";

// Inside the triangle (-0.5,-0.5) (0.5,-0.5) (0,0.5) with a margin: 1 inside, 0 outside, -1 on the edge.
int InsideTriangle(float x, float y)
{
    const auto edge = [](float x0, float y0, float x1, float y1, float px, float py) {
        return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    };
    const float e0 = edge(-0.5f, -0.5f, 0.5f, -0.5f, x, y), e1 = edge(0.5f, -0.5f, 0.0f, 0.5f, x, y),
                e2 = edge(0.0f, 0.5f, -0.5f, -0.5f, x, y);
    constexpr float kMargin = 0.05f;
    if (e0 > kMargin && e1 > kMargin && e2 > kMargin)
        return 1;
    if (e0 < -kMargin || e1 < -kMargin || e2 < -kMargin)
        return 0;
    return -1;
}

// ---- the test ---------------------------------------------------------------------------------------

struct Outcome
{
    enum Kind : u8
    {
        Ok,
        Failed,
        Skipped,
    };
    Kind kind = Ok;
    xr_string note; // failure reason, skip reason or a remark on success
};

Outcome Fail(const char* format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return {Outcome::Failed, text};
}

Outcome Skip(const char* why) { return {Outcome::Skipped, why}; }

class SelfTest
{
public:
    SelfTest(ID3D11Device* device, ID3D11DeviceContext* immediate)
        : m_dev(device), m_imm(immediate), m_caps(xrd3d12::GetNativeCaps(device)), m_native(xrd3d12::NativeDevice(device))
    {
    }

    void Run()
    {
        const auto start = std::chrono::steady_clock::now();
        struct Entry
        {
            const char* name;
            Outcome outcome;
        } parts[] = {{"immediate", {}}, {"deferred", {}}, {"async", {}}, {"bindless", {}}, {"raytracing", {}}, {"vrs", {}},
            {"indirect", {}}};
        if (!m_caps || !m_native)
        {
            Msg("! [qa] native selftest: not a DirectX 12 layer device");
            return;
        }
        const bool dxc = da_dxc_native_model() >= 0x60;
        if (!dxc)
        {
            for (Entry& e : parts)
                e.outcome = Skip("no DXC");
        }
        else if (!Prepare(parts[0].outcome))
        {
            for (Entry& e : parts)
            {
                if (&e != parts)
                    e.outcome = Skip("the shared shaders failed");
            }
        }
        else
        {
            parts[0].outcome = Wave(false);
            parts[1].outcome = Wave(true);
            parts[2].outcome = Async();
            parts[3].outcome = Bindless();
            parts[4].outcome = Raytracing();
            parts[5].outcome = Vrs();
            parts[6].outcome = Indirect();
        }
        Unbind();
        RCache.Invalidate();

        xr_string line;
        bool failed = false;
        for (const Entry& e : parts)
        {
            line += line.empty() ? "" : ", ";
            line += e.name;
            switch (e.outcome.kind)
            {
            case Outcome::Ok: line += " ok"; break;
            case Outcome::Failed:
                line += " FAILED";
                failed = true;
                break;
            case Outcome::Skipped: line += " skipped"; break;
            }
            if (!e.outcome.note.empty() && e.outcome.kind != Outcome::Failed)
            {
                line += " (";
                line += e.outcome.note;
                line += ")";
            }
        }
        const char* version = xrd3d12::DxcVersion();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Msg("%s [qa] native selftest: %s, dxc %s (%.0f ms)", failed ? "!" : "*", line.c_str(), *version ? version : "none", ms);
        for (const Entry& e : parts)
        {
            if (e.outcome.kind == Outcome::Failed)
                Msg("! [qa] native selftest: %s: %s", e.name, e.outcome.note.c_str());
        }
    }

private:
    ID3D11Device* m_dev;
    ID3D11DeviceContext* m_imm;
    const xrd3d12::NativeCaps* m_caps;
    ID3D12Device* m_native;

    Ref<ID3D11RasterizerState> m_raster;
    Ref<ID3D11DepthStencilState> m_depth;
    Ref<ID3D11VertexShader> m_fullVs, m_stripVs;
    Ref<ID3D11PixelShader> m_copyPs, m_positionPs, m_stripPs;
    Ref<ID3D11ComputeShader> m_fillCs;
    xr_vector<u8> m_fullVsCode, m_positionPsCode;
    ID3D12RootSignature* m_rootTable = nullptr; // UAV table u0 + 4 constants b0 (device-owned)
    ID3D12PipelineState* m_wavePso = nullptr;
    ID3D12PipelineState* m_fillPso = nullptr;

    static bool Compile(const char* path, const char* source, const char* entry, const char* target, xr_vector<u8>& code)
    {
        DaDxcNative request;
        request.path = path;
        request.source = source;
        request.entry = entry;
        request.target = target;
        return da_dxc_compile_native(request, code);
    }

    ID3D12RootSignature* Root(const D3D12_ROOT_PARAMETER1* params, u32 count, const wchar_t* name) const
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = count;
        desc.Desc_1_1.pParameters = params;
        return xrd3d12::RootSignature(m_dev, desc, name);
    }

    // Shaders and states every part shares.
    bool Prepare(Outcome& error)
    {
        xr_vector<u8> wave, fill, copyPs, stripVs, stripPs;
        if (!Compile("native/infra_selftest_wave.hlsl", kWaveHlsl, "WaveCs", "cs_6_0", wave) ||
            !Compile("native/infra_selftest_fill.hlsl", kFillHlsl, "FillCs", "cs_6_0", fill) ||
            !Compile("native/infra_selftest_draw.hlsl", kDrawHlsl, "FullVs", "vs_6_0", m_fullVsCode) ||
            !Compile("native/infra_selftest_draw.hlsl", kDrawHlsl, "CopyPs", "ps_6_0", copyPs) ||
            !Compile("native/infra_selftest_draw.hlsl", kDrawHlsl, "PositionPs", "ps_6_0", m_positionPsCode) ||
            !Compile("native/infra_selftest_draw.hlsl", kDrawHlsl, "StripVs", "vs_6_0", stripVs) ||
            !Compile("native/infra_selftest_draw.hlsl", kDrawHlsl, "StripPs", "ps_6_0", stripPs))
        {
            error = Fail("a test shader does not compile (see the [dxc] line above)");
            return false;
        }
        if (FAILED(m_dev->CreateVertexShader(m_fullVsCode.data(), m_fullVsCode.size(), nullptr, m_fullVs.put())) ||
            FAILED(m_dev->CreateVertexShader(stripVs.data(), stripVs.size(), nullptr, m_stripVs.put())) ||
            FAILED(m_dev->CreatePixelShader(copyPs.data(), copyPs.size(), nullptr, m_copyPs.put())) ||
            FAILED(m_dev->CreatePixelShader(m_positionPsCode.data(), m_positionPsCode.size(), nullptr, m_positionPs.put())) ||
            FAILED(m_dev->CreatePixelShader(stripPs.data(), stripPs.size(), nullptr, m_stripPs.put())) ||
            FAILED(m_dev->CreateComputeShader(fill.data(), fill.size(), nullptr, m_fillCs.put())))
        {
            error = Fail("the D3D11 path refused a DXIL test shader");
            return false;
        }
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(m_dev->CreateRasterizerState(&raster, m_raster.put())) || FAILED(m_dev->CreateDepthStencilState(&depth, m_depth.put())))
        {
            error = Fail("no rasterizer or depth-stencil state");
            return false;
        }

        const D3D12_DESCRIPTOR_RANGE1 uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
        D3D12_ROOT_PARAMETER1 params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &uav};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, 4};
        m_rootTable = Root(params, 2, L"selftest table");
        m_wavePso = m_rootTable ? xrd3d12::ComputePipeline(m_dev, m_rootTable, wave.data(), wave.size(), L"selftest wave") : nullptr;
        m_fillPso = m_rootTable ? xrd3d12::ComputePipeline(m_dev, m_rootTable, fill.data(), fill.size(), L"selftest fill") : nullptr;
        if (!m_wavePso || !m_fillPso)
        {
            error = Fail("native compute pipelines refused");
            return false;
        }
        if (m_caps->vrsTier >= 2)
            xrd3d12::SetShadingRate(m_imm, D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, nullptr);
        return true;
    }

    void Unbind() const
    {
        ID3D11ShaderResourceView* noSrv = nullptr;
        ID3D11UnorderedAccessView* noUav = nullptr;
        ID3D11Buffer* noCb = nullptr;
        m_imm->PSSetShaderResources(0, 1, &noSrv);
        m_imm->CSSetShaderResources(0, 1, &noSrv);
        m_imm->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        m_imm->CSSetConstantBuffers(0, 1, &noCb);
        m_imm->OMSetRenderTargets(0, nullptr, nullptr);
        m_imm->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        m_imm->VSSetShader(nullptr, nullptr, 0);
        m_imm->PSSetShader(nullptr, nullptr, 0);
        m_imm->CSSetShader(nullptr, nullptr, 0);
    }

    // ---- resources and read-back ----

    bool Texture(DXGI_FORMAT format, UINT bind, Ref<ID3D11Texture2D>& out, const void* data = nullptr, UINT pitch = 0) const
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = data ? 1 : kSize;
        d.Height = data ? 1 : kSize;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        const D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
        return SUCCEEDED(m_dev->CreateTexture2D(&d, data ? &init : nullptr, out.put()));
    }

    bool Buffer(UINT bytes, UINT bind, UINT misc, const void* data, Ref<ID3D11Buffer>& out, D3D11_USAGE usage = D3D11_USAGE_DEFAULT) const
    {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = usage;
        d.BindFlags = bind;
        d.MiscFlags = misc;
        const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
        return SUCCEEDED(m_dev->CreateBuffer(&d, data ? &init : nullptr, out.put()));
    }

    // A raw UAV buffer of `words` 32-bit words, zero-filled, and its view.
    bool RawBuffer(u32 words, Ref<ID3D11Buffer>& buffer, Ref<ID3D11UnorderedAccessView>& view) const
    {
        const xr_vector<u32> zeros(words, 0u);
        if (!Buffer(words * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, zeros.data(), buffer))
            return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = words;
        d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        return SUCCEEDED(m_dev->CreateUnorderedAccessView(buffer.get(), &d, view.put()));
    }

    bool ReadWords(ID3D11Buffer* buffer, u32 words, xr_vector<u32>& out) const
    {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = words * 4;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Ref<ID3D11Buffer> staging;
        if (FAILED(m_dev->CreateBuffer(&d, nullptr, staging.put())))
            return false;
        m_imm->CopyResource(staging.get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_imm->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
            return false;
        out.resize(words);
        std::memcpy(out.data(), mapped.pData, words * 4);
        m_imm->Unmap(staging.get(), 0);
        return true;
    }

    // An RGBA32F kSize x kSize texture, 4 floats per texel.
    bool ReadImage(ID3D11Texture2D* texture, xr_vector<float>& out) const
    {
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        Ref<ID3D11Texture2D> staging;
        if (FAILED(m_dev->CreateTexture2D(&d, nullptr, staging.put())))
            return false;
        m_imm->CopyResource(staging.get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_imm->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
            return false;
        out.resize(kSize * kSize * 4);
        for (u32 y = 0; y < kSize; ++y)
            std::memcpy(out.data() + y * kSize * 4, static_cast<const u8*>(mapped.pData) + size_t(y) * mapped.RowPitch, kSize * 16);
        m_imm->Unmap(staging.get(), 0);
        return true;
    }

    // An RGBA32F target with its RTV.
    bool Target(Ref<ID3D11Texture2D>& texture, Ref<ID3D11RenderTargetView>& rtv) const
    {
        return Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET, texture) &&
            SUCCEEDED(m_dev->CreateRenderTargetView(texture.get(), nullptr, rtv.put()));
    }

    // D3D11 draw state of the test's fullscreen draws into `rtv`.
    void DrawInto(ID3D11RenderTargetView* rtv, ID3D11VertexShader* vs, ID3D11PixelShader* ps) const
    {
        m_imm->OMSetRenderTargets(1, &rtv, nullptr);
        m_imm->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        m_imm->OMSetDepthStencilState(m_depth.get(), 0);
        const D3D11_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
        m_imm->RSSetViewports(1, &viewport);
        m_imm->RSSetState(m_raster.get());
        m_imm->IASetInputLayout(nullptr);
        m_imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_imm->HSSetShader(nullptr, nullptr, 0);
        m_imm->DSSetShader(nullptr, nullptr, 0);
        m_imm->GSSetShader(nullptr, nullptr, 0);
        m_imm->VSSetShader(vs, nullptr, 0);
        m_imm->PSSetShader(ps, nullptr, 0);
    }

    // ---- (a) immediate, (b) deferred: a native wave-op dispatch, then a D3D11 draw reads its result ----

    Outcome Wave(bool deferred)
    {
        Ref<ID3D11Texture2D> image, copy;
        Ref<ID3D11UnorderedAccessView> uav;
        Ref<ID3D11ShaderResourceView> srv;
        Ref<ID3D11RenderTargetView> rtv;
        if (!Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, image) ||
            FAILED(m_dev->CreateUnorderedAccessView(image.get(), nullptr, uav.put())) ||
            FAILED(m_dev->CreateShaderResourceView(image.get(), nullptr, srv.put())) || !Target(copy, rtv))
            return Fail("resources");
        Ref<ID3D11DeviceContext> def;
        if (deferred && FAILED(m_dev->CreateDeferredContext(0, def.put())))
            return Fail("CreateDeferredContext");
        ID3D11DeviceContext* context = deferred ? def.get() : m_imm;
        const u32 tag = deferred ? 2 : 1;

        xrd3d12::NativeScope scope;
        if (!xrd3d12::BeginNative(context, scope))
            return Fail("BeginNative refused");
        xrd3d12::Require(context, image.get(), kUav);
        const xrd3d12::DescriptorSpan span = xrd3d12::AllocateDescriptors(context, 1);
        if (span.count)
        {
            m_native->CopyDescriptorsSimple(1, span.cpu, xrd3d12::NativeDescriptor(uav.get()), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            scope.list->SetComputeRootSignature(m_rootTable);
            scope.list->SetPipelineState(m_wavePso);
            scope.list->SetComputeRootDescriptorTable(0, span.gpu);
            const u32 constants[4] = {tag, 0, 0, 0};
            scope.list->SetComputeRoot32BitConstants(1, 4, constants, 0);
            scope.list->Dispatch(kSize / 8, kSize / 8, 1);
        }
        xrd3d12::EndNative(context, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
        if (!span.count)
            return Fail("AllocateDescriptors failed");
        if (deferred)
        {
            Ref<ID3D11CommandList> list;
            if (FAILED(def->FinishCommandList(FALSE, list.put())))
                return Fail("FinishCommandList");
            m_imm->ExecuteCommandList(list.get(), FALSE);
        }

        DrawInto(rtv.get(), m_fullVs.get(), m_copyPs.get());
        m_imm->PSSetShaderResources(0, 1, srv.addr());
        m_imm->Draw(3, 0);
        Unbind();
        xr_vector<float> texels;
        if (!ReadImage(copy.get(), texels))
            return Fail("read-back");
        const float lanes = texels[0];
        if (lanes < float(m_caps->waveLanesMin) || lanes > float(m_caps->waveLanesMax) || lanes < 4.0f)
            return Fail("wave of %.0f lanes (the device reports %u-%u)", lanes, m_caps->waveLanesMin, m_caps->waveLanesMax);
        u32 wrong = 0;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const float* t = texels.data() + (y * kSize + x) * 4;
                wrong += t[0] != lanes || t[1] != float(x) || t[2] != float(y) || t[3] != float(tag);
            }
        }
        if (wrong)
            return Fail("%u of %u texels wrong", wrong, kSize * kSize);
        char note[32];
        std::snprintf(note, sizeof(note), "%.0f lanes", lanes);
        return {Outcome::Ok, deferred ? "" : note};
    }

    // ---- (c) async: a native job and a D3D11-recorded job write buffers between a fork and a join ----

    Outcome Async()
    {
        Ref<ID3D11Buffer> nativeWords, recordedWords, constants;
        Ref<ID3D11UnorderedAccessView> nativeUav, recordedUav;
        const u32 params[4] = {100, 0, 0, 0};
        if (!RawBuffer(kWords, nativeWords, nativeUav) || !RawBuffer(kWords, recordedWords, recordedUav) ||
            !Buffer(sizeof(params), D3D11_BIND_CONSTANT_BUFFER, 0, params, constants, D3D11_USAGE_IMMUTABLE))
            return Fail("resources");

        xrd3d12::NativeScope scope;
        xrd3d12::AsyncJob* job = xrd3d12::BeginAsyncCompute(m_dev, "selftest_native", scope);
        if (!job || !scope.list)
            return Fail("BeginAsyncCompute refused");
        xrd3d12::AsyncUse(job, nativeWords.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        const xrd3d12::DescriptorSpan span = xrd3d12::AsyncDescriptors(job, 1);
        if (!span.count)
        {
            xrd3d12::DiscardAsyncCompute(job);
            return Fail("AsyncDescriptors failed");
        }
        m_native->CopyDescriptorsSimple(1, span.cpu, xrd3d12::NativeDescriptor(nativeUav.get()), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        scope.list->SetComputeRootSignature(m_rootTable);
        scope.list->SetPipelineState(m_fillPso);
        scope.list->SetComputeRootDescriptorTable(0, span.gpu);
        const u32 zero[4] = {};
        scope.list->SetComputeRoot32BitConstants(1, 4, zero, 0);
        scope.list->Dispatch(kWords / 64, 1, 1);
        xrd3d12::EndAsyncCompute(job);
        xrd3d12::ForkAsyncCompute(m_imm, job);
        xrd3d12::JoinAsyncCompute(m_imm, job);

        // The engine's way: D3D11 calls on a deferred context in compute recording.
        {
            Ref<ID3D11DeviceContext> def;
            if (FAILED(m_dev->CreateDeferredContext(0, def.put())))
                return Fail("CreateDeferredContext");
            if (!xrd3d12::BeginAsyncRecording(def.get()))
                return Fail("BeginAsyncRecording refused");
            def->CSSetShader(m_fillCs.get(), nullptr, 0);
            def->CSSetConstantBuffers(0, 1, constants.addr());
            def->CSSetUnorderedAccessViews(0, 1, recordedUav.addr(), nullptr);
            def->Dispatch(kWords / 64, 1, 1);
            Ref<ID3D11CommandList> list;
            if (FAILED(def->FinishCommandList(FALSE, list.put())))
                return Fail("FinishCommandList");
            xrd3d12::AsyncJob* recorded = xrd3d12::AsyncJobFromCommandList(m_dev, "selftest_recorded", list.get());
            if (!recorded)
                return Fail("AsyncJobFromCommandList refused");
            xrd3d12::ForkAsyncCompute(m_imm, recorded);
            xrd3d12::JoinAsyncCompute(m_imm, recorded);
        }

        xr_vector<u32> a, b;
        if (!ReadWords(nativeWords.get(), kWords, a) || !ReadWords(recordedWords.get(), kWords, b))
            return Fail("read-back");
        u32 wrongNative = 0, wrongRecorded = 0;
        for (u32 i = 0; i < kWords; ++i)
        {
            wrongNative += a[i] != i * 3 + 7;
            wrongRecorded += b[i] != i * 3 + 7 + params[0];
        }
        if (wrongNative || wrongRecorded)
            return Fail("%u words of the native job and %u of the recorded one wrong", wrongNative, wrongRecorded);
        return {Outcome::Ok, m_caps->asyncCompute ? "" : "in order"};
    }

    // ---- (d) bindless: 1024 slots written from D3D11 views and summed through the table ----

    Outcome Bindless()
    {
        if (m_caps->bindlessCapacity < kSlots)
            return Skip("no bindless range");
        const D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 100,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
        D3D12_ROOT_PARAMETER1 params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 4};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &range};
        ID3D12RootSignature* root = Root(params, 3, L"selftest bindless");
        xr_vector<u8> code;
        if (!Compile("native/infra_selftest_bindless.hlsl", kBindlessHlsl, "SumCs", "cs_6_0", code))
            return Fail("shader");
        ID3D12PipelineState* pso = root ? xrd3d12::ComputePipeline(m_dev, root, code.data(), code.size(), L"selftest bindless") : nullptr;
        Ref<ID3D11Buffer> total;
        Ref<ID3D11UnorderedAccessView> totalUav;
        xrd3d12::NativeBuffer address;
        if (!pso || !RawBuffer(4, total, totalUav) || !xrd3d12::NativeBufferRange(total.get(), address))
            return Fail("pipeline or resources");

        xr_vector<Ref<ID3D11Texture2D>> textures(kSlots);
        xr_vector<Ref<ID3D11ShaderResourceView>> views(kSlots);
        u32 expected = 0;
        for (u32 i = 0; i < kSlots; ++i)
        {
            const u32 value = i * 3 + 1;
            expected += value;
            if (!Texture(DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE, textures[i], &value, 4) ||
                FAILED(m_dev->CreateShaderResourceView(textures[i].get(), nullptr, views[i].put())))
                return Fail("slot texture %u", i);
        }
        const u32 base = xrd3d12::AllocateBindless(m_dev, kSlots);
        if (base == xrd3d12::kBindlessInvalid)
            return Fail("AllocateBindless(%u) failed", kSlots);
        for (u32 i = 0; i < kSlots; ++i)
            xrd3d12::WriteBindless(m_dev, base + i, views[i].get());

        xrd3d12::NativeScope scope;
        const bool open = xrd3d12::BeginNative(m_imm, scope);
        if (open)
        {
            xrd3d12::Require(m_imm, total.get(), kUav);
            scope.list->SetComputeRootSignature(root);
            scope.list->SetPipelineState(pso);
            const u32 constants[4] = {base, kSlots, 0, 0};
            scope.list->SetComputeRoot32BitConstants(0, 4, constants, 0);
            scope.list->SetComputeRootUnorderedAccessView(1, address.address);
            scope.list->SetComputeRootDescriptorTable(2, scope.bindless);
            scope.list->Dispatch(1, 1, 1);
            xrd3d12::EndNative(m_imm, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
        }
        xr_vector<u32> sum;
        const bool read = open && ReadWords(total.get(), 1, sum);
        // Freed before the views go: a slot must not outlive its view.
        xrd3d12::FreeBindless(m_dev, base, kSlots);
        if (!open)
            return Fail("BeginNative refused");
        if (!read)
            return Fail("read-back");
        if (sum[0] != expected)
            return Fail("sum over %u slots %u, expected %u", kSlots, sum[0], expected);
        return {};
    }

    // ---- (e) raytracing: one-triangle BLAS/TLAS, RayQuery bound natively and through a D3D11 view ----

    Outcome Raytracing()
    {
        ID3D12Device5* dev5 = xrd3d12::NativeDevice5(m_dev);
        if (m_caps->raytracingTier < 11)
            return Skip(m_caps->raytracingTier ? "DXR 1.0 only" : "no DXR");
        if (da_dxc_native_model() < 0x65 || !dev5)
            return Skip("no shader model 6.5");
        xr_vector<u8> code;
        if (!Compile("native/infra_selftest_trace.hlsl", kTraceHlsl, "TraceCs", "cs_6_5", code))
            return Fail("shader");
        D3D12_ROOT_PARAMETER1 params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
        ID3D12RootSignature* root = Root(params, 2, L"selftest trace");
        ID3D12PipelineState* pso = root ? xrd3d12::ComputePipeline(m_dev, root, code.data(), code.size(), L"selftest trace") : nullptr;
        Ref<ID3D11ComputeShader> cs;
        Ref<ID3D11Buffer> nativeHits, viewHits;
        Ref<ID3D11UnorderedAccessView> nativeUav, viewUav;
        xrd3d12::NativeBuffer nativeAddress;
        if (!pso || FAILED(m_dev->CreateComputeShader(code.data(), code.size(), nullptr, cs.put())) ||
            !RawBuffer(kSize * kSize, nativeHits, nativeUav) || !RawBuffer(kSize * kSize, viewHits, viewUav) ||
            !xrd3d12::NativeBufferRange(nativeHits.get(), nativeAddress))
            return Fail("pipeline or resources");

        // Geometry in upload memory; acceleration structures and scratch are native buffers.
        ID3D12Resource* vertices = nullptr;
        ID3D12Resource* blas = nullptr;
        ID3D12Resource* tlas = nullptr;
        ID3D12Resource* scratch = nullptr;
        const auto release = [&] {
            for (ID3D12Resource* object : {vertices, blas, tlas, scratch})
            {
                if (object)
                    xrd3d12::ReleaseNative(m_dev, object);
            }
        };
        const float triangle[9] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
        if (FAILED(xrd3d12::CreateNativeBuffer(m_dev, sizeof(triangle), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ, L"selftest triangle", &vertices)))
            return Fail("vertex buffer");
        void* mapped = nullptr;
        if (FAILED(vertices->Map(0, nullptr, &mapped)) || !mapped)
        {
            release();
            return Fail("vertex buffer map");
        }
        std::memcpy(mapped, triangle, sizeof(triangle));
        vertices->Unmap(0, nullptr);
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexBuffer = {vertices->GetGPUVirtualAddress(), 12};
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = 3;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
        bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bottom.NumDescs = 1;
        bottom.pGeometryDescs = &geometry;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
        top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.NumDescs = 1;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomInfo{}, topInfo{};
        dev5->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bottomInfo);
        dev5->GetRaytracingAccelerationStructurePrebuildInfo(&top, &topInfo);
        if (FAILED(xrd3d12::CreateAccelerationStructureBuffer(m_dev, bottomInfo.ResultDataMaxSizeInBytes, L"selftest blas", &blas)) ||
            FAILED(xrd3d12::CreateAccelerationStructureBuffer(m_dev, topInfo.ResultDataMaxSizeInBytes, L"selftest tlas", &tlas)) ||
            FAILED(xrd3d12::CreateNativeBuffer(m_dev, (std::max)(bottomInfo.ScratchDataSizeInBytes, topInfo.ScratchDataSizeInBytes) + 256,
                D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kUav, L"selftest scratch", &scratch)))
        {
            release();
            return Fail("acceleration structure buffers");
        }

        // Build, then trace with the TLAS as a root SRV: one native section on the immediate context.
        xrd3d12::NativeScope scope;
        if (!xrd3d12::BeginNative(m_imm, scope) || !scope.list4)
        {
            if (scope.list)
                xrd3d12::EndNative(m_imm, 0);
            release();
            return Fail("BeginNative refused or no ID3D12GraphicsCommandList4");
        }
        const xrd3d12::UploadSpan instance =
            xrd3d12::AllocateUpload(m_imm, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
        if (instance.cpu)
        {
            // The instance puts the triangle at z = 1.
            D3D12_RAYTRACING_INSTANCE_DESC desc{};
            desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1.0f;
            desc.Transform[2][3] = 1.0f;
            desc.InstanceMask = 0xFF;
            desc.AccelerationStructure = blas->GetGPUVirtualAddress();
            std::memcpy(instance.cpu, &desc, sizeof(desc));
            top.InstanceDescs = instance.gpu;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = bottom;
            build.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
            build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            scope.list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            scope.list->ResourceBarrier(1, &barrier);
            build.Inputs = top;
            build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
            scope.list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            scope.list->ResourceBarrier(1, &barrier);
            xrd3d12::Require(m_imm, nativeHits.get(), kUav);
            scope.list->SetComputeRootSignature(root);
            scope.list->SetPipelineState(pso);
            scope.list->SetComputeRootShaderResourceView(0, tlas->GetGPUVirtualAddress());
            scope.list->SetComputeRootUnorderedAccessView(1, nativeAddress.address);
            scope.list->Dispatch(kSize / 8, kSize / 8, 1);
        }
        xrd3d12::EndNative(m_imm, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
        if (!instance.cpu)
        {
            release();
            return Fail("AllocateUpload failed");
        }

        // The same shader through the D3D11 path, the TLAS bound as a D3D11 SRV.
        Ref<ID3D11ShaderResourceView> view;
        if (FAILED(xrd3d12::CreateAccelerationStructureView(m_dev, tlas, view.put())))
        {
            release();
            return Fail("CreateAccelerationStructureView");
        }
        m_imm->CSSetShader(cs.get(), nullptr, 0);
        m_imm->CSSetShaderResources(0, 1, view.addr());
        m_imm->CSSetUnorderedAccessViews(0, 1, viewUav.addr(), nullptr);
        m_imm->Dispatch(kSize / 8, kSize / 8, 1);
        Unbind();

        xr_vector<u32> a, b;
        const bool read = ReadWords(nativeHits.get(), kSize * kSize, a) && ReadWords(viewHits.get(), kSize * kSize, b);
        release();
        if (!read)
            return Fail("read-back");
        u32 wrongNative = 0, wrongView = 0, inside = 0;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const int expected = InsideTriangle((float(x) + 0.5f) / 32.0f - 1.0f, (float(y) + 0.5f) / 32.0f - 1.0f);
                if (expected < 0)
                    continue;
                inside += expected;
                const u32 value = expected ? 101 : 0;
                wrongNative += a[y * kSize + x] != value;
                wrongView += b[y * kSize + x] != value;
            }
        }
        if (inside < 200 || wrongNative || wrongView)
            return Fail("%u texels wrong through the root SRV, %u through the D3D11 view (%u inside)", wrongNative, wrongView, inside);
        return {};
    }

    // ---- (f) vrs: a native 2x2 draw, then an engine draw after EndNative(TouchedShadingRate) at 1x1 ----

    Outcome Vrs()
    {
        if (m_caps->vrsTier < 2)
            return Skip(m_caps->vrsTier ? "VRS tier 1" : "no VRS");
        ID3D12RootSignature* root = Root(nullptr, 0, L"selftest empty");
        if (!root)
            return Fail("root signature");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root;
        d.VS = {m_fullVsCode.data(), m_fullVsCode.size()};
        d.PS = {m_positionPsCode.data(), m_positionPsCode.size()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(m_native->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
            return Fail("graphics pipeline");
        Ref<ID3D11Texture2D> coarse, fine;
        Ref<ID3D11RenderTargetView> coarseRtv, fineRtv;
        if (!Target(coarse, coarseRtv) || !Target(fine, fineRtv))
        {
            xrd3d12::ReleaseNative(m_dev, pso);
            return Fail("resources");
        }

        xrd3d12::NativeScope scope;
        const bool open = xrd3d12::BeginNative(m_imm, scope);
        if (open && scope.list5)
        {
            xrd3d12::Require(m_imm, coarse.get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = xrd3d12::NativeDescriptor(coarseRtv.get());
            scope.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
            scope.list->RSSetViewports(1, &viewport);
            scope.list->RSSetScissorRects(1, &scissor);
            scope.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            scope.list->SetGraphicsRootSignature(root);
            scope.list->SetPipelineState(pso);
            scope.list5->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
            scope.list->DrawInstanced(3, 1, 0, 0);
        }
        if (open)
            xrd3d12::EndNative(m_imm, xrd3d12::TouchedGraphicsRoot | xrd3d12::TouchedPipeline | xrd3d12::TouchedInputAssembler |
                    xrd3d12::TouchedOutputMerger | xrd3d12::TouchedRasterizer | xrd3d12::TouchedShadingRate);
        xrd3d12::ReleaseNative(m_dev, pso);
        if (!open || !scope.list5)
            return Fail("BeginNative refused or no ID3D12GraphicsCommandList5");
        DrawInto(fineRtv.get(), m_fullVs.get(), m_positionPs.get());
        m_imm->Draw(3, 0);
        Unbind();

        xr_vector<float> a, b;
        if (!ReadImage(coarse.get(), a) || !ReadImage(fine.get(), b))
            return Fail("read-back");
        // Coarse: every aligned 2x2 block holds one value, and the blocks differ. Fine: the texel's centre.
        u32 mixedBlocks = 0, fineWrong = 0;
        for (u32 y = 0; y < kSize; y += 2)
        {
            for (u32 x = 0; x < kSize; x += 2)
            {
                const float* t = a.data() + (y * kSize + x) * 4;
                for (const u32 offset : {1u, kSize, kSize + 1})
                    mixedBlocks += t[offset * 4] != t[0] || t[offset * 4 + 1] != t[1];
            }
        }
        const bool blocksDiffer = a[0] != a[(kSize / 2 * kSize + kSize / 2) * 4];
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const float* t = b.data() + (y * kSize + x) * 4;
                fineWrong += t[0] != float(x) + 0.5f || t[1] != float(y) + 0.5f;
            }
        }
        if (mixedBlocks || !blocksDiffer)
            return Fail("the native 2x2 draw is not coarse (%u texels differ inside their block)", mixedBlocks);
        if (fineWrong)
            return Fail("the engine draw after the section does not shade at 1x1 (%u texels)", fineWrong);
        return {};
    }

    // ---- (g) indirect: DrawIndexedIndirectCount of 3 of 4 records ----

    Outcome Indirect()
    {
        u16 indices[24];
        for (u16 i = 0; i < 24; ++i)
            indices[i] = i;
        u32 records[4][5]{};
        for (u32 k = 0; k < 4; ++k)
        {
            records[k][0] = 6;     // IndexCountPerInstance
            records[k][1] = 1;     // InstanceCount
            records[k][2] = 6 * k; // StartIndexLocation: strip k
        }
        const u32 count[4] = {3, 0, 0, 0};
        Ref<ID3D11Buffer> ib, args, counter;
        Ref<ID3D11Texture2D> target;
        Ref<ID3D11RenderTargetView> rtv;
        if (!Buffer(sizeof(indices), D3D11_BIND_INDEX_BUFFER, 0, indices, ib, D3D11_USAGE_IMMUTABLE) ||
            !Buffer(sizeof(records), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, records, args) ||
            !Buffer(sizeof(count), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, count, counter) ||
            !Target(target, rtv))
            return Fail("resources");
        const float clear[4] = {};
        m_imm->ClearRenderTargetView(rtv.get(), clear);
        DrawInto(rtv.get(), m_stripVs.get(), m_stripPs.get());
        m_imm->IASetIndexBuffer(ib.get(), DXGI_FORMAT_R16_UINT, 0);
        xrd3d12::DrawIndexedIndirectCount(m_imm, args.get(), 0, 4, counter.get(), 0);
        Unbind();
        xr_vector<float> texels;
        if (!ReadImage(target.get(), texels))
            return Fail("read-back");
        u32 wrong = 0, strips = 0;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const u32 strip = x / 16;
                const float expected = strip < 3 ? float(strip + 1) * 0.25f : 0.0f;
                if (texels[(y * kSize + x) * 4] != expected)
                {
                    ++wrong;
                    strips |= 1u << strip;
                }
            }
        }
        if (wrong)
            return Fail("%u of %u texels wrong (strips 0x%X; 0-2 drawn, 3 beyond the count)", wrong, kSize * kSize, strips);
        return {};
    }
};

// Runs the test at the start of the next rendered frame, before the level renders.
struct SelfTestRequest final : public pureRender
{
    bool pending = false;
    void OnRender() override
    {
        Device.seqRender.Remove(this);
        pending = false;
        if (ID3D11DeviceContext* immediate = HW.get_context(CHW::IMM_CTX_ID); immediate && HW.pDevice)
        {
            SelfTest test(HW.pDevice, immediate);
            test.Run();
        }
    }
};
SelfTestRequest g_request;
} // namespace
#endif

void da_native_selftest_run()
{
#if defined(XR_D3D12)
    if (g_request.pending)
        return;
    g_request.pending = true;
    Device.seqRender.Add(&g_request, REG_PRIORITY_HIGH);
#else
    Msg("! r__native_selftest: DirectX 12 only");
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
