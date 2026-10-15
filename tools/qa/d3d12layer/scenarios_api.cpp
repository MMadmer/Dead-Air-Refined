// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// API scenarios: queries, staging maps, copies, dynamic and updated textures, the D3D11 hazard
// rules, input layout validation, ClearState, the swap chain; the scenario table and the benchmark.
#include "harness.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace layertest
{
void ScenarioClearReadback(Gpu& g, Result& r);
void ScenarioTexturedQuad(Gpu& g, Result& r);
void ScenarioManyDraws(Gpu& g, Result& r);
void ScenarioBlendModes(Gpu& g, Result& r);
void ScenarioDepthStencil(Gpu& g, Result& r);
void ScenarioMrt3(Gpu& g, Result& r);
void ScenarioScissor(Gpu& g, Result& r);
void ScenarioDepthArray(Gpu& g, Result& r);
void ScenarioCubeSampling(Gpu& g, Result& r);
void ScenarioVolumeRender(Gpu& g, Result& r);
void ScenarioMsaaResolve(Gpu& g, Result& r);
void ScenarioTessellation(Gpu& g, Result& r);
void ScenarioTexture1D3D(Gpu& g, Result& r);
void ScenarioComputePingPong(Gpu& g, Result& r);
void ScenarioDeferredContexts(Gpu& g, Result& r);
void ScenarioSamplerHeapChurn(Gpu& g, Result& r);
void ScenarioIndexedDraws(Gpu& g, Result& r);
void ScenarioCrossFrameDynamic(Gpu& g, Result& r);
void ScenarioReleaseInFlight(Gpu& g, Result& r);
void ScenarioBlockCompressed(Gpu& g, Result& r);
void ScenarioFormatFamilyCopy(Gpu& g, Result& r);
void ScenarioLargeTexture(Gpu& g, Result& r);
void ScenarioAnnotations(Gpu& g, Result& r);
void ScenarioNativeInterop(Gpu& g, Result& r);

namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kRtSrv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

ComPtr<ID3D11Query> MakeQuery(Gpu& g, Result& r, D3D11_QUERY type)
{
    D3D11_QUERY_DESC d{type, 0};
    ComPtr<ID3D11Query> q;
    r.Ok(g.dev->CreateQuery(&d, &q), "CreateQuery");
    return q;
}

// GetData with flags 0 until the data is there (at most two seconds).
template <class T>
bool WaitData(Gpu& g, ID3D11Asynchronous* query, T& data, UINT flags = 0)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;)
    {
        const HRESULT hr = g.ctx->GetData(query, &data, sizeof(data), flags);
        if (hr == S_OK)
            return true;
        if (FAILED(hr) || std::chrono::steady_clock::now() > deadline)
            return false;
        if (flags & D3D11_ASYNC_GETDATA_DONOTFLUSH)
            g.ctx->Flush();
        SwitchToThread();
    }
}

struct Pipeline
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
};

Pipeline MakePipeline(Gpu& g, Result& r, const char* source, const char* ps)
{
    Pipeline p;
    p.vs = MakeVs(g, r, source, "QuadVs");
    p.ps = MakePs(g, r, source, ps);
    p.cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    return p;
}

void Bind(Gpu& g, const Pipeline& p)
{
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, p.cb.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, p.cb.GetAddressOf());
}

void Draw(Gpu& g, const Pipeline& p, const QuadConstants& q)
{
    Upload(g, p.cb.Get(), &q, sizeof(q));
    g.ctx->Draw(6, 0);
}

std::string g_sampleSource;
const char* const kSampleExtra = R"(
Texture2D tex : register(t0);
SamplerState pointSampler : register(s0);
float4 SamplePs(VsOut i) : SV_Target
{
    return tex.SampleLevel(pointSampler, i.uv, 0);
}
Texture2D<float> depthCopy : register(t0);
float DepthCopyPs(VsOut i) : SV_Target
{
    return depthCopy.Load(int3(i.pos.xy, 0));
}
float4 LoadPs(VsOut i) : SV_Target
{
    return tex.Load(int3(i.pos.xy, 0));
}
)";

const char* SampleSource()
{
    if (g_sampleSource.empty())
        g_sampleSource = std::string(kQuadHlsl) + kSampleExtra;
    return g_sampleSource.c_str();
}
} // namespace

// ------------------------------------------------------------------------------------------------

void ScenarioQueries(Gpu& g, Result& r)
{
    const char* source = SampleSource();
    auto rt = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    auto depth = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL));
    Pipeline p = MakePipeline(g, r, source, "ColorPs");
    auto visible = MakeQuery(g, r, D3D11_QUERY_OCCLUSION);
    auto occluded = MakeQuery(g, r, D3D11_QUERY_OCCLUSION);
    auto predicate = MakeQuery(g, r, D3D11_QUERY_OCCLUSION_PREDICATE);
    auto event = MakeQuery(g, r, D3D11_QUERY_EVENT);
    auto disjoint = MakeQuery(g, r, D3D11_QUERY_TIMESTAMP_DISJOINT);
    auto ts0 = MakeQuery(g, r, D3D11_QUERY_TIMESTAMP);
    auto ts1 = MakeQuery(g, r, D3D11_QUERY_TIMESTAMP);
    ComPtr<ID3D11Query> many[48];
    for (auto& q : many)
        q = MakeQuery(g, r, D3D11_QUERY_OCCLUSION);
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    auto dsv = MakeDsv(g, r, depth.Get());
    REQUIRE_OK();

    g.ctx->Begin(disjoint.Get());
    g.ctx->End(ts0.Get());
    const float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    SetViewport(g, 0, 0, 32, 32);
    Bind(g, p);
    g.ctx->Begin(visible.Get());
    Draw(g, p, QuadPx(4, 4, 20, 20, 32, 32, 1, 0, 0, 1, 0.2f));
    g.ctx->End(visible.Get());
    g.ctx->Begin(occluded.Get());
    Draw(g, p, QuadPx(0, 0, 32, 32, 32, 32, 0, 1, 0, 1, 0.9f));
    g.ctx->End(occluded.Get());
    g.ctx->Begin(predicate.Get());
    Draw(g, p, QuadPx(10, 10, 11, 12, 32, 32, 0, 0, 1, 1, 0.1f));
    g.ctx->End(predicate.Get());
    // The light-volume pattern: many small queries in one frame.
    for (u32 i = 0; i < 48; ++i)
    {
        g.ctx->Begin(many[i].Get());
        const float x = float(i % 8) * 4, y = float(i / 8) * 4;
        Draw(g, p, QuadPx(x, y, x + 1 + float(i % 3), y + 1 + float(i % 4), 32, 32, 0.5f, 0.5f, 0.5f, 1, (i & 1) ? 0.7f : 0.3f));
        g.ctx->End(many[i].Get());
    }
    // An occlusion query open across submissions: an event End and a flushing GetData in between.
    auto spanning = MakeQuery(g, r, D3D11_QUERY_OCCLUSION);
    auto midEvent = MakeQuery(g, r, D3D11_QUERY_EVENT);
    REQUIRE_OK();
    g.ctx->Begin(spanning.Get());
    Draw(g, p, QuadPx(0, 24, 8, 32, 32, 32, 1, 1, 0, 1, 0.1f));
    g.ctx->End(midEvent.Get());
    BOOL midDone = FALSE;
    r.Check(WaitData(g, midEvent.Get(), midDone), "event inside an open occlusion query never completed");
    Draw(g, p, QuadPx(8, 24, 20, 32, 32, 32, 1, 0, 1, 1, 0.1f));
    g.ctx->End(midEvent.Get());
    Draw(g, p, QuadPx(20, 24, 21, 25, 32, 32, 0, 1, 1, 1, 0.1f));
    g.ctx->End(spanning.Get());

    g.ctx->End(ts1.Get());
    g.ctx->End(disjoint.Get());
    g.ctx->End(event.Get());

    // Non-blocking read first: it must not produce data out of nothing.
    UINT64 early = 0;
    const HRESULT earlyHr = g.ctx->GetData(visible.Get(), &early, sizeof(early), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    r.Check(earlyHr == S_OK || earlyHr == S_FALSE, "GetData(DONOTFLUSH) returned 0x%08X", unsigned(earlyHr));

    UINT64 count = 0;
    r.Check(WaitData(g, visible.Get(), count), "visible occlusion query never completed");
    r.Value("visible samples", long long(count));
    r.Check(WaitData(g, occluded.Get(), count), "occluded occlusion query never completed");
    r.Value("occluded samples", long long(count));
    r.Check(WaitData(g, spanning.Get(), count), "spanning occlusion query never completed");
    r.Value("samples of a query spanning submissions", long long(count));
    BOOL any = FALSE;
    r.Check(WaitData(g, predicate.Get(), any), "occlusion predicate never completed");
    r.Value("predicate", any);
    for (u32 i = 0; i < 48; ++i)
    {
        r.Check(WaitData(g, many[i].Get(), count, i % 2 ? D3D11_ASYNC_GETDATA_DONOTFLUSH : 0), "occlusion query %u never completed", i);
        r.Value(("query " + std::to_string(i)).c_str(), long long(count));
    }
    BOOL done = FALSE;
    r.Check(WaitData(g, event.Get(), done), "event query never completed");
    r.Value("event", done);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    r.Check(WaitData(g, disjoint.Get(), dj), "disjoint query never completed");
    UINT64 t0 = 0, t1 = 0;
    r.Check(WaitData(g, ts0.Get(), t0) && WaitData(g, ts1.Get(), t1), "timestamps never completed");
    // Device-specific values: checked locally, not compared.
    r.Check(!dj.Disjoint && dj.Frequency > 0, "disjoint query: disjoint %d, frequency %llu", int(dj.Disjoint),
        static_cast<unsigned long long>(dj.Frequency));
    r.Check(t1 >= t0 && t0 != 0, "timestamps out of order (%llu, %llu)", static_cast<unsigned long long>(t0),
        static_cast<unsigned long long>(t1));
    // Ended twice: the second End restarts nothing, the data stays readable.
    g.ctx->End(event.Get());
    r.Check(WaitData(g, event.Get(), done), "re-ended event query never completed");
    g.ctx->ClearState();
}

// Queries that were never issued, one still between Begin and End, a wrong data size: the
// engine's spin loops depend on the exact results (D3D11 reports these calls as errors).
void ScenarioQueryMisuse(Gpu& g, Result& r)
{
    auto freshEvent = MakeQuery(g, r, D3D11_QUERY_EVENT);
    auto freshOcclusion = MakeQuery(g, r, D3D11_QUERY_OCCLUSION);
    auto freshTimestamp = MakeQuery(g, r, D3D11_QUERY_TIMESTAMP);
    auto open = MakeQuery(g, r, D3D11_QUERY_OCCLUSION);
    auto ended = MakeQuery(g, r, D3D11_QUERY_OCCLUSION);
    REQUIRE_OK();
    BOOL done = FALSE;
    UINT64 count = 0, time = 0;
    r.Value("GetData of an event never ended", g.ctx->GetData(freshEvent.Get(), &done, sizeof(done), 0));
    r.Value("GetData of an occlusion query never begun", g.ctx->GetData(freshOcclusion.Get(), &count, sizeof(count), 0));
    r.Value("GetData of a timestamp never ended", g.ctx->GetData(freshTimestamp.Get(), &time, sizeof(time), 0));
    g.ctx->Begin(open.Get());
    r.Value("GetData of an occlusion query not ended", g.ctx->GetData(open.Get(), &count, sizeof(count), 0));
    g.ctx->End(open.Get());
    r.Check(WaitData(g, open.Get(), count), "late-ended occlusion query never completed");
    r.Value("late-ended occlusion samples", long long(count));
    g.ctx->Begin(ended.Get());
    g.ctx->End(ended.Get());
    r.Value("GetData with a wrong size", g.ctx->GetData(ended.Get(), &done, sizeof(done), 0));
    r.Value("GetData with a null buffer and size 0", SUCCEEDED(g.ctx->GetData(ended.Get(), nullptr, 0, 0)));
    r.Check(WaitData(g, ended.Get(), count), "empty occlusion query never completed");
    r.Value("empty occlusion samples", long long(count));
}

// ------------------------------------------------------------------------------------------------

// The cloud map pattern: a staging copy polled with DO_NOT_WAIT every frame.
void ScenarioStagingDoNotWait(Gpu& g, Result& r)
{
    const char* source = SampleSource();
    auto rt = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, kRtSrv));
    D3D11_TEXTURE2D_DESC sd = Desc2D(64, 64, kRgba8, 0);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto staging = MakeTexture2D(g, r, sd);
    Pipeline p = MakePipeline(g, r, source, "UvPs");
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    for (u32 round = 0; round < 3; ++round)
    {
        const float clear[4] = {0.1f * float(round), 0.2f, 0.3f, 1};
        g.ctx->ClearRenderTargetView(rtv.Get(), clear);
        g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 64, 64);
        Bind(g, p);
        Draw(g, p, QuadPx(8 + float(round) * 8, 8, 40, 56, 64, 64, 0, 0, 0, 0));
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g.ctx->CopyResource(staging.Get(), rt.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = S_OK;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        u32 polls = 0;
        while ((hr = g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            if (std::chrono::steady_clock::now() > deadline)
                break;
            ++polls;
            g.ctx->Flush();
            SwitchToThread();
        }
        if (!r.Ok(hr, "Map(DO_NOT_WAIT) of the staging copy"))
            return;
        r.Check(m.RowPitch >= 64 * 4, "RowPitch %u too small", m.RowPitch);
        Image image;
        image.label = "staging round " + std::to_string(round);
        image.format = kRgba8;
        image.width = 64;
        image.height = 64;
        image.texelSize = 4;
        image.texels.resize(64 * 64 * 4);
        for (u32 y = 0; y < 64; ++y)
            std::memcpy(image.texels.data() + y * 256, static_cast<const u8*>(m.pData) + size_t(y) * m.RowPitch, 256);
        g.ctx->Unmap(staging.Get(), 0);
        r.images.push_back(std::move(image));
    }
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

void ScenarioCopySubresourceRegion(Gpu& g, Result& r)
{
    const char* source = SampleSource();
    auto rt = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    D3D11_TEXTURE2D_DESC one = Desc2D(1, 1, kRgba8, 0);
    one.Usage = D3D11_USAGE_STAGING;
    one.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto texel = MakeTexture2D(g, r, one);
    auto target = MakeTexture2D(g, r, Desc2D(16, 16, kRgba8, kRtSrv, 2, 2));
    D3D11_TEXTURE2D_DESC arr = Desc2D(8, 8, kRgba8, 0, 1, 2);
    arr.Usage = D3D11_USAGE_STAGING;
    arr.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto stagingArray = MakeTexture2D(g, r, arr);
    Pipeline p = MakePipeline(g, r, source, "UvPs");
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    auto targetRtv = MakeRtv(g, r, target.Get());
    REQUIRE_OK();
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1};
    g.ctx->ClearRenderTargetView(targetRtv.Get(), grey);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 32, 32);
    Bind(g, p);
    Draw(g, p, QuadPx(0, 0, 32, 32, 32, 32, 0, 0, 0, 0));
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // The cloud map read: one texel into a 1x1 staging texture.
    const D3D11_BOX box{13, 7, 0, 14, 8, 1};
    g.ctx->CopySubresourceRegion(texel.Get(), 0, 0, 0, 0, rt.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (r.Ok(g.ctx->Map(texel.Get(), 0, D3D11_MAP_READ, 0, &m), "Map of the 1x1 staging texture"))
    {
        u32 value = 0;
        std::memcpy(&value, m.pData, 4);
        r.Value("texel (13,7)", value);
        g.ctx->Unmap(texel.Get(), 0);
    }
    // A region into mip 0 of slice 1 of a DEFAULT texture, and one texel into slice 1 of staging memory.
    const D3D11_BOX region{4, 4, 0, 12, 10, 1};
    g.ctx->CopySubresourceRegion(target.Get(), D3D11CalcSubresource(0, 1, 2), 2, 3, 0, rt.Get(), 0, &region);
    const D3D11_BOX single{20, 21, 0, 21, 22, 1};
    g.ctx->CopySubresourceRegion(stagingArray.Get(), 1, 5, 6, 0, rt.Get(), 0, &single);
    r.images.push_back(Read2D(g, r, target.Get(), D3D11CalcSubresource(0, 1, 2), "region into slice 1"));
    r.images.push_back(Read2D(g, r, target.Get(), D3D11CalcSubresource(0, 0, 2), "slice 0 untouched"));
    if (r.Ok(g.ctx->Map(stagingArray.Get(), 1, D3D11_MAP_READ, 0, &m), "Map of the staging array"))
    {
        u32 value = 0;
        std::memcpy(&value, static_cast<const u8*>(m.pData) + 6 * m.RowPitch + 5 * 4, 4);
        r.Value("staging slice 1 texel (5,6)", value);
        g.ctx->Unmap(stagingArray.Get(), 1);
    }
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

// The HUD shadow pattern: CopyResource of a D24S8 depth buffer that is still bound for testing,
// then the copy read through its R24_UNORM_X8 view.
void ScenarioDepthCopy(Gpu& g, Result& r)
{
    const char* source = SampleSource();
    const UINT bind = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    auto depth = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_R24G8_TYPELESS, bind));
    auto copy = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_R24G8_TYPELESS, bind));
    auto out = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_R32_FLOAT, kRtSrv));
    Pipeline p = MakePipeline(g, r, source, "ColorPs");
    auto readPs = MakePs(g, r, source, "DepthCopyPs");
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> always;
    r.Ok(g.dev->CreateDepthStencilState(&dd, &always), "CreateDepthStencilState");
    dd.DepthEnable = FALSE;
    ComPtr<ID3D11DepthStencilState> off;
    r.Ok(g.dev->CreateDepthStencilState(&dd, &off), "CreateDepthStencilState");
    REQUIRE_OK();
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    auto dsv = MakeDsv(g, r, depth.Get(), &dvd);
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    auto copySrv = MakeSrv(g, r, copy.Get(), &svd);
    auto outRtv = MakeRtv(g, r, out.Get());
    REQUIRE_OK();
    g.ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 7);
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), dsv.Get());
    SetViewport(g, 0, 0, 32, 32);
    Bind(g, p);
    g.ctx->OMSetDepthStencilState(always.Get(), 0);
    Draw(g, p, QuadPx(6, 3, 27, 22, 32, 32, 0, 0, 0, 0, 0.75f));
    g.ctx->CopyResource(copy.Get(), depth.Get());
    // Still bound as the depth buffer, depth testing off; the copy is sampled.
    g.ctx->OMSetDepthStencilState(off.Get(), 0);
    g.ctx->PSSetShader(readPs.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, copySrv.GetAddressOf());
    Draw(g, p, QuadPx(0, 0, 32, 32, 32, 32, 0, 0, 0, 0));
    r.images.push_back(Read2D(g, r, out.Get(), 0, "copied depth"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

// The video texture pattern: a DYNAMIC texture uploaded with Map(WRITE_DISCARD) twice per frame.
void ScenarioDynamicTexture(Gpu& g, Result& r)
{
    const char* source = SampleSource();
    D3D11_TEXTURE2D_DESC td = Desc2D(20, 12, kRgba8, D3D11_BIND_SHADER_RESOURCE);
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    auto video = MakeTexture2D(g, r, td);
    auto rt = MakeTexture2D(g, r, Desc2D(40, 12, kRgba8, kRtSrv));
    Pipeline p = MakePipeline(g, r, source, "SamplePs");
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    REQUIRE_OK();
    auto srv = MakeSrv(g, r, video.Get());
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 40, 12);
    Bind(g, p);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    for (u32 frame = 0; frame < 2; ++frame)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (!r.Ok(g.ctx->Map(video.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map(WRITE_DISCARD) of a dynamic texture"))
            return;
        r.Check(m.RowPitch >= 20 * 4, "RowPitch %u", m.RowPitch);
        for (u32 y = 0; y < 12; ++y)
        {
            u8* row = static_cast<u8*>(m.pData) + size_t(y) * m.RowPitch;
            for (u32 x = 0; x < 20; ++x)
            {
                row[x * 4 + 0] = u8(x * 12 + frame * 100);
                row[x * 4 + 1] = u8(y * 20);
                row[x * 4 + 2] = u8(frame ? 200 : 50);
                row[x * 4 + 3] = 255;
            }
        }
        g.ctx->Unmap(video.Get(), 0);
        const float x0 = float(frame * 20);
        Draw(g, p, QuadPx(x0, 0, x0 + 20, 12, 40, 12, 0, 0, 0, 0));
    }
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "two uploads"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

// The ImGui font pattern: UpdateSubresource on a DEFAULT texture, whole and with a box.
void ScenarioUpdateSubresource(Gpu& g, Result& r)
{
    auto tex = MakeTexture2D(g, r, Desc2D(16, 16, kRgba8, D3D11_BIND_SHADER_RESOURCE, 2));
    REQUIRE_OK();
    std::vector<u8> full(16 * 16 * 4), boxData(64 * 6), mip(8 * 8 * 4);
    for (u32 i = 0; i < 16 * 16; ++i)
    {
        full[i * 4 + 0] = u8(i);
        full[i * 4 + 1] = u8(255 - i);
        full[i * 4 + 2] = u8(i * 3);
        full[i * 4 + 3] = 255;
    }
    // A pitch of 64 bytes for a 6-texel-wide box.
    for (u32 y = 0; y < 6; ++y)
    {
        for (u32 x = 0; x < 6; ++x)
        {
            u8* t = &boxData[y * 64 + x * 4];
            t[0] = 250;
            t[1] = u8(x * 40);
            t[2] = u8(y * 40);
            t[3] = 128;
        }
    }
    for (u32 i = 0; i < 64; ++i)
    {
        mip[i * 4 + 0] = 10;
        mip[i * 4 + 1] = u8(i * 4);
        mip[i * 4 + 2] = 90;
        mip[i * 4 + 3] = 255;
    }
    g.ctx->UpdateSubresource(tex.Get(), 0, nullptr, full.data(), 64, 0);
    const D3D11_BOX box{4, 3, 0, 10, 9, 1};
    g.ctx->UpdateSubresource(tex.Get(), 0, &box, boxData.data(), 64, 0);
    g.ctx->UpdateSubresource(tex.Get(), 1, nullptr, mip.data(), 32, 0);
    r.images.push_back(Read2D(g, r, tex.Get(), 0, "mip 0"));
    r.images.push_back(Read2D(g, r, tex.Get(), 1, "mip 1"));
}

// ------------------------------------------------------------------------------------------------

// D3D11 resolves read/write hazards by unbinding: binding an output unbinds its inputs, binding an
// input of a bound output binds null.
void ScenarioHazards(Gpu& g, Result& r)
{
    const char* source = SampleSource();
    auto tex = MakeTexture2D(g, r, Desc2D(16, 16, kRgba8, kRtSrv | D3D11_BIND_UNORDERED_ACCESS));
    auto out = MakeTexture2D(g, r, Desc2D(16, 16, kRgba8, kRtSrv));
    const UINT depthBind = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    auto depth = MakeTexture2D(g, r, Desc2D(16, 16, DXGI_FORMAT_R24G8_TYPELESS, depthBind));
    Pipeline p = MakePipeline(g, r, source, "LoadPs");
    REQUIRE_OK();
    auto srv = MakeSrv(g, r, tex.Get());
    auto rtv = MakeRtv(g, r, tex.Get());
    auto outRtv = MakeRtv(g, r, out.Get());
    ComPtr<ID3D11UnorderedAccessView> uav;
    r.Ok(g.dev->CreateUnorderedAccessView(tex.Get(), nullptr, &uav), "CreateUnorderedAccessView");
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    auto dsv = MakeDsv(g, r, depth.Get(), &dvd);
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    auto depthSrv = MakeSrv(g, r, depth.Get(), &svd);
    REQUIRE_OK();
    const auto bound = [&](const char* name, auto get) {
        ComPtr<ID3D11ShaderResourceView> view;
        get(view.GetAddressOf());
        r.Value(name, view != nullptr);
    };
    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), red);
    g.ctx->ClearRenderTargetView(outRtv.Get(), blue);
    SetViewport(g, 0, 0, 16, 16);
    Bind(g, p);

    // 1. Bound as PS input, then bound as the render target: the input goes.
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    bound("PS SRV bound before", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(0, 1, v); });
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    bound("PS SRV after RTV bind", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(0, 1, v); });
    // Drawing into the other target now reads slot 0: null, so zeros.
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
    Draw(g, p, QuadPx(0, 0, 8, 16, 16, 16, 0, 0, 0, 0));

    // 2. The input of a bound output binds null.
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    bound("PS SRV of the bound RTV", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(0, 1, v); });
    g.ctx->VSSetShaderResources(0, 1, srv.GetAddressOf());
    bound("VS SRV of the bound RTV", [&](ID3D11ShaderResourceView** v) { g.ctx->VSGetShaderResources(0, 1, v); });

    // 3. Depth: the view of a bound depth buffer binds null; unbinding the DSV lets it through.
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), dsv.Get());
    g.ctx->PSSetShaderResources(1, 1, depthSrv.GetAddressOf());
    bound("depth SRV while the DSV is bound", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(1, 1, v); });
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
    g.ctx->PSSetShaderResources(1, 1, depthSrv.GetAddressOf());
    bound("depth SRV after the DSV went", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(1, 1, v); });
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), dsv.Get());
    bound("depth SRV after the DSV came back", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(1, 1, v); });

    // 4. Compute: the SRV of a bound CS UAV binds null; binding the UAV unbinds CS inputs.
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
    g.ctx->CSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    bound("CS SRV after the UAV bind", [&](ID3D11ShaderResourceView** v) { g.ctx->CSGetShaderResources(0, 1, v); });
    g.ctx->CSSetShaderResources(0, 1, srv.GetAddressOf());
    bound("CS SRV of the bound UAV", [&](ID3D11ShaderResourceView** v) { g.ctx->CSGetShaderResources(0, 1, v); });

    // 5. A PS input while the same texture is a CS UAV, and the reverse.
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    bound("PS SRV of a bound CS UAV", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(0, 1, v); });
    ID3D11UnorderedAccessView* nullUav = nullptr;
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    bound("PS SRV after a CS UAV bind", [&](ID3D11ShaderResourceView** v) { g.ctx->PSGetShaderResources(0, 1, v); });
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

    // 6. Compute inputs against graphics outputs.
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.ctx->CSSetShaderResources(0, 1, srv.GetAddressOf());
    bound("CS SRV of the bound RTV", [&](ID3D11ShaderResourceView** v) { g.ctx->CSGetShaderResources(0, 1, v); });
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
    g.ctx->CSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    bound("CS SRV after the RTV bind", [&](ID3D11ShaderResourceView** v) { g.ctx->CSGetShaderResources(0, 1, v); });
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g.ctx->CSSetShaderResources(0, 1, &nullSrv);

    // 7. Outputs against outputs: an RTV bind while the texture is a CS UAV, and the reverse.
    const auto outputs = [&](const char* name) {
        ComPtr<ID3D11UnorderedAccessView> u;
        g.ctx->CSGetUnorderedAccessViews(0, 1, &u);
        ComPtr<ID3D11RenderTargetView> t;
        g.ctx->OMGetRenderTargets(1, &t, nullptr);
        r.Value((std::string(name) + ": CS UAV bound").c_str(), u != nullptr);
        r.Value((std::string(name) + ": RTV bound").c_str(), t != nullptr);
    };
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
    g.ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    outputs("RTV bound after the CS UAV");
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    outputs("CS UAV bound after the RTV");
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);

    // 8. With every conflict gone the texture reads back its red.
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    Draw(g, p, QuadPx(8, 0, 16, 16, 16, 16, 0, 0, 0, 0));
    r.images.push_back(Read2D(g, r, out.Get(), 0, "reads after the hazards"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
const char* const kLayoutHlsl = R"(
struct VIn
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 unused : COLOR;
};
float4 LayoutVs(VIn v) : SV_Position
{
    return float4(v.pos + v.uv.xyx, 1);
}
)";
} // namespace

// CBackend::ApplyVertexLayout retries or drops draws on E_INVALIDARG: the layer must refuse what
// D3D11 refuses and accept what it accepts.
void ScenarioInputLayout(Gpu& g, Result& r)
{
    const ShaderCode vs = Code(g, kLayoutHlsl, "LayoutVs", "vs_5_0");
    ComPtr<ID3DBlob> signature;
    r.Ok(D3DGetInputSignatureBlob(vs.data, vs.size, &signature), "D3DGetInputSignatureBlob");
    REQUIRE_OK();
    const D3D11_INPUT_ELEMENT_DESC pos{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const D3D11_INPUT_ELEMENT_DESC uv{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const D3D11_INPUT_ELEMENT_DESC uv1{"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const D3D11_INPUT_ELEMENT_DESC color{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const D3D11_INPUT_ELEMENT_DESC colorLower{"color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const D3D11_INPUT_ELEMENT_DESC normal{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    struct Case
    {
        const char* name;
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    };
    const Case cases[] = {
        {"complete", {pos, uv, color}},
        {"missing TEXCOORD0", {pos, color}},
        {"missing COLOR (declared, unused)", {pos, uv}},
        {"TEXCOORD1 instead of 0", {pos, uv1, color}},
        {"extra NORMAL", {pos, uv, color, normal}},
        {"lower-case semantic", {pos, uv, colorLower}},
        {"duplicate", {pos, uv, color, color}},
    };
    for (const Case& c : cases)
    {
        ComPtr<ID3D11InputLayout> layout;
        const HRESULT full = g.dev->CreateInputLayout(c.elements.data(), UINT(c.elements.size()), vs.data, vs.size, &layout);
        ComPtr<ID3D11InputLayout> fromSignature;
        const HRESULT sig = g.dev->CreateInputLayout(c.elements.data(), UINT(c.elements.size()), signature->GetBufferPointer(),
            signature->GetBufferSize(), &fromSignature);
        r.Value((std::string(c.name) + " (shader blob)").c_str(), full);
        r.Value((std::string(c.name) + " (signature blob)").c_str(), sig);
    }
}

// ------------------------------------------------------------------------------------------------

void ScenarioClearState(Gpu& g, Result& r)
{
    const char* source = SampleSource();
    auto rt = MakeTexture2D(g, r, Desc2D(8, 8, kRgba8, kRtSrv));
    auto other = MakeTexture2D(g, r, Desc2D(8, 8, kRgba8, kRtSrv));
    Pipeline p = MakePipeline(g, r, source, "SamplePs");
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP);
    auto vb = MakeBuffer(g, r, 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER);
    auto ib = MakeBuffer(g, r, 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    r.Ok(g.dev->CreateBlendState(&bd, &blend), "CreateBlendState");
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    auto srv = MakeSrv(g, r, other.Get());
    REQUIRE_OK();
    Bind(g, p);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.ctx->PSSetShaderResources(3, 1, srv.GetAddressOf());
    g.ctx->PSSetSamplers(2, 1, sampler.GetAddressOf());
    const UINT stride = 16, offset = 0;
    g.ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    g.ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R32_UINT, 4);
    const float factor[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    g.ctx->OMSetBlendState(blend.Get(), factor, 0x3);
    SetViewport(g, 1, 2, 3, 4);
    const D3D11_RECT rect{1, 1, 2, 2};
    g.ctx->RSSetScissorRects(1, &rect);

    g.ctx->ClearState();

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    g.ctx->VSGetShader(&vs, nullptr, nullptr);
    g.ctx->PSGetShader(&ps, nullptr, nullptr);
    r.Value("VS", vs != nullptr);
    r.Value("PS", ps != nullptr);
    ComPtr<ID3D11Buffer> cb;
    g.ctx->VSGetConstantBuffers(0, 1, &cb);
    r.Value("VS CB0", cb != nullptr);
    ComPtr<ID3D11ShaderResourceView> view;
    g.ctx->PSGetShaderResources(3, 1, &view);
    r.Value("PS SRV3", view != nullptr);
    ComPtr<ID3D11SamplerState> smp;
    g.ctx->PSGetSamplers(2, 1, &smp);
    r.Value("PS sampler 2", smp != nullptr);
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    g.ctx->OMGetRenderTargets(1, &target, &depth);
    r.Value("RTV", target != nullptr);
    r.Value("DSV", depth != nullptr);
    ComPtr<ID3D11Buffer> buffer;
    UINT gotStride = 99, gotOffset = 99;
    g.ctx->IAGetVertexBuffers(0, 1, &buffer, &gotStride, &gotOffset);
    r.Value("VB", buffer != nullptr);
    r.Value("VB stride", gotStride);
    ComPtr<ID3D11Buffer> index;
    DXGI_FORMAT format = DXGI_FORMAT_R8_UNORM;
    UINT indexOffset = 99;
    g.ctx->IAGetIndexBuffer(&index, &format, &indexOffset);
    r.Value("IB", index != nullptr);
    r.Value("IB format", format);
    r.Value("IB offset", indexOffset);
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    g.ctx->IAGetPrimitiveTopology(&topology);
    r.Value("topology", topology);
    ComPtr<ID3D11BlendState> blendAfter;
    float factorAfter[4] = {};
    UINT mask = 0;
    g.ctx->OMGetBlendState(&blendAfter, factorAfter, &mask);
    r.Value("blend state", blendAfter != nullptr);
    r.Value("blend factor x1000", long long(factorAfter[0] * 1000 + factorAfter[3] * 10));
    r.Value("sample mask", mask);
    ComPtr<ID3D11DepthStencilState> dss;
    UINT ref = 99;
    g.ctx->OMGetDepthStencilState(&dss, &ref);
    r.Value("depth state", dss != nullptr);
    r.Value("stencil ref", ref);
    UINT count = 0;
    g.ctx->RSGetViewports(&count, nullptr);
    r.Value("viewports", count);
    count = 0;
    g.ctx->RSGetScissorRects(&count, nullptr);
    r.Value("scissor rects", count);
    ComPtr<ID3D11RasterizerState> raster;
    g.ctx->RSGetState(&raster);
    r.Value("rasterizer state", raster != nullptr);
    ComPtr<ID3D11InputLayout> layout;
    g.ctx->IAGetInputLayout(&layout);
    r.Value("input layout", layout != nullptr);
}

// ------------------------------------------------------------------------------------------------

// The ImGui DX11 backend: device -> IDXGIDevice -> adapter -> factory at init, and a full state
// backup/restore through the Get* calls (with its fixed-size arrays) around every UI draw.
void ScenarioImGuiContract(Gpu& g, Result& r)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    r.Value("QueryInterface(IDXGIDevice)", g.dev->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));
    if (dxgiDevice)
    {
        ComPtr<IDXGIAdapter> adapter;
        r.Value("IDXGIDevice::GetParent(IDXGIAdapter)", dxgiDevice->GetParent(IID_PPV_ARGS(&adapter)));
        ComPtr<IDXGIAdapter> direct;
        r.Value("IDXGIDevice::GetAdapter", dxgiDevice->GetAdapter(&direct));
        if (adapter)
        {
            DXGI_ADAPTER_DESC mine{}, expected{};
            adapter->GetDesc(&mine);
            Adapter()->GetDesc(&expected);
            r.Value("same adapter", std::memcmp(&mine.AdapterLuid, &expected.AdapterLuid, sizeof(LUID)) == 0);
            ComPtr<IDXGIFactory> factory;
            r.Value("IDXGIAdapter::GetParent(IDXGIFactory)", adapter->GetParent(IID_PPV_ARGS(&factory)));
        }
        // The chain leads back to the device.
        ComPtr<ID3D11Device> back;
        r.Value("IDXGIDevice -> ID3D11Device", dxgiDevice->QueryInterface(IID_PPV_ARGS(&back)));
        r.Value("same device", back.Get() == g.dev);
    }

    const D3D11_VIEWPORT viewports[2] = {{0, 0, 64, 32, 0, 1}, {1, 2, 3, 4, 0, 1}};
    g.ctx->RSSetViewports(2, viewports);
    const D3D11_RECT rects[3] = {{0, 0, 5, 5}, {1, 1, 2, 2}, {3, 3, 4, 4}};
    g.ctx->RSSetScissorRects(3, rects);
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT gotViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    g.ctx->RSGetViewports(&count, gotViewports);
    r.Value("RSGetViewports count (array of 16)", count);
    r.Value("viewport 1 height", long long(gotViewports[1].Height));
    count = 1;
    g.ctx->RSGetViewports(&count, gotViewports);
    r.Value("RSGetViewports count (array of 1)", count);
    count = 0;
    g.ctx->RSGetViewports(&count, nullptr);
    r.Value("RSGetViewports count (no array)", count);
    count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT gotRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    g.ctx->RSGetScissorRects(&count, gotRects);
    r.Value("RSGetScissorRects count (array of 16)", count);
    r.Value("rect 2 right", gotRects[2].right);
    count = 2;
    g.ctx->RSGetScissorRects(&count, gotRects);
    r.Value("RSGetScissorRects count (array of 2)", count);

    auto vs = MakeVs(g, r, kQuadHlsl, "QuadVs");
    auto ps = MakePs(g, r, kQuadHlsl, "ColorPs");
    REQUIRE_OK();
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    ComPtr<ID3D11PixelShader> gotPs;
    ID3D11ClassInstance* instances[256] = {};
    UINT instanceCount = 256;
    g.ctx->PSGetShader(&gotPs, instances, &instanceCount);
    r.Value("PSGetShader instance count", instanceCount);
    r.Value("PSGetShader returns the shader", gotPs.Get() == ps.Get());
    ComPtr<ID3D11VertexShader> gotVs;
    instanceCount = 256;
    g.ctx->VSGetShader(&gotVs, instances, &instanceCount);
    r.Value("VSGetShader instance count", instanceCount);
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g.ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    long long anyTarget = dsv != nullptr;
    for (ID3D11RenderTargetView* v : rtvs)
        anyTarget += v != nullptr;
    r.Value("OMGetRenderTargets of 8 with nothing bound", anyTarget);
    // Restoring the backup: the arrays go back in.
    g.ctx->RSSetScissorRects(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, gotRects);
    count = 0;
    g.ctx->RSGetScissorRects(&count, nullptr);
    r.Value("scissor rects after restoring 16", count);
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND MakeHiddenWindow(u32 width, u32 height)
{
    static ATOM atom = 0;
    if (!atom)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"xrD3D12LayerTest";
        atom = RegisterClassW(&wc);
    }
    return CreateWindowExW(0, L"xrD3D12LayerTest", L"xrD3D12 layer test", WS_OVERLAPPEDWINDOW, 0, 0, int(width), int(height),
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
} // namespace

// CreateSwapChainForHwnd, 10 presents through the back buffer proxy (draws and a readback of it),
// ResizeBuffers, one more present. Layer only: the D3D11 flip model is not the subject here.
void ScenarioSwapChain(Gpu& g, Result& r)
{
    HWND hwnd = MakeHiddenWindow(256, 160);
    if (!r.Check(hwnd != nullptr, "CreateWindow failed"))
        return;
    ComPtr<IDXGIFactory2> factory;
    r.Ok(Adapter()->GetParent(IID_PPV_ARGS(&factory)), "IDXGIAdapter::GetParent");
    REQUIRE_OK();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 256;
    desc.Height = 160;
    desc.Format = kRgba8;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> chain;
    r.Ok(xrd3d12::CreateSwapChainForHwnd(g.dev, factory.Get(), hwnd, &desc, nullptr, &chain), "CreateSwapChainForHwnd");
    const char* source = SampleSource();
    Pipeline p = MakePipeline(g, r, source, "UvPs");
    D3D11_TEXTURE2D_DESC sd = Desc2D(256, 160, kRgba8, 0);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto staging = MakeTexture2D(g, r, sd);
    if (!r.failures.empty())
    {
        DestroyWindow(hwnd);
        return;
    }

    // The engine's rt_Base: one back buffer texture and one view, held across presents.
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> backRtv;
    const auto acquire = [&]() -> bool {
        if (!r.Ok(chain->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer(0)"))
            return false;
        backRtv = MakeRtv(g, r, back.Get());
        return backRtv != nullptr;
    };
    const auto frame = [&](u32 index, u32 width, u32 height) -> bool {
        const float clear[4] = {float(index % 4) / 3.0f, 0.25f, 0.5f, 1};
        g.ctx->ClearRenderTargetView(backRtv.Get(), clear);
        g.ctx->OMSetRenderTargets(1, backRtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, float(width), float(height));
        Bind(g, p);
        Draw(g, p, QuadPx(10, 10, 60 + float(index), 50, width, height, 0, 0, 0, 0));
        if (index % 3 == 0 && width == 256)
        {
            // The iconic preview pattern: the current back buffer copied to staging memory.
            g.ctx->CopyResource(staging.Get(), back.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (r.Ok(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map of the back buffer copy"))
            {
                const u32 expected = 0xFF804000u | u32(clear[0] * 255.0f + 0.5f);
                u32 corner = 0, inside = 0;
                std::memcpy(&corner, m.pData, 4);
                std::memcpy(&inside, static_cast<const u8*>(m.pData) + 20 * m.RowPitch + 20 * 4, 4);
                // Drivers round 0.5 to 127 or 128: one step of tolerance per channel.
                const auto within1 = [](u32 a, u32 b) {
                    for (u32 shift = 0; shift < 32; shift += 8)
                        if (std::abs(int(a >> shift & 0xFF) - int(b >> shift & 0xFF)) > 1)
                            return false;
                    return true;
                };
                r.Check(within1(corner, expected), "frame %u: back buffer corner 0x%08X, expected 0x%08X", index, corner, expected);
                r.Check(std::abs(int(inside >> 16 & 0xFF) - 0x80) <= 1 && (inside & 0xFFFF) != (corner & 0xFFFF),
                    "frame %u: back buffer inside 0x%08X shows no draw", index, inside);
                g.ctx->Unmap(staging.Get(), 0);
            }
        }
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        const HRESULT hr = chain->Present(0, (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        return r.Check(hr == S_OK || hr == DXGI_STATUS_OCCLUDED, "Present returned 0x%08X", unsigned(hr));
    };
    if (acquire())
    {
        for (u32 i = 0; i < 10 && frame(i, 256, 160); ++i)
        {
        }
    }
    DXGI_SWAP_CHAIN_DESC1 got{};
    chain->GetDesc1(&got);
    r.Check(got.Width == 256 && got.Height == 160 && got.BufferCount >= 2, "GetDesc1 %ux%u x%u", got.Width, got.Height, got.BufferCount);
    g.ctx->ClearState();
    // ResizeBuffers needs every reference to the back buffer gone.
    backRtv.Reset();
    back.Reset();
    r.Ok(chain->ResizeBuffers(0, 320, 200, DXGI_FORMAT_UNKNOWN, desc.Flags), "ResizeBuffers");
    chain->GetDesc1(&got);
    r.Check(got.Width == 320 && got.Height == 200, "after ResizeBuffers %ux%u", got.Width, got.Height);
    if (acquire())
    {
        D3D11_TEXTURE2D_DESC bd{};
        back->GetDesc(&bd);
        r.Check(bd.Width == 320 && bd.Height == 200, "back buffer desc %ux%u after resize", bd.Width, bd.Height);
        for (u32 i = 10; i < 13 && frame(i, 320, 200); ++i)
        {
        }
    }
    backRtv.Reset();
    back.Reset();
    g.ctx->ClearState();
    chain.Reset();
    DestroyWindow(hwnd);
}

// ------------------------------------------------------------------------------------------------

const std::vector<Scenario>& Scenarios()
{
    static const std::vector<Scenario> list = [] {
    std::vector<Scenario> base = {
        {"clear + readback", ScenarioClearReadback, false},
        {"textured quad (immutable mips, dynamic VB DISCARD/NO_OVERWRITE, CB renames)", ScenarioTexturedQuad, false},
        {"1024 draws with changing constant buffers", ScenarioManyDraws, false},
        {"blend modes (alpha, additive, min/max, factor, subtract, independent MRT1)", ScenarioBlendModes, false},
        {"depth test + stencil ops, dynamic stencil ref, back faces", ScenarioDepthStencil, false},
        {"MRT with 3 targets", ScenarioMrt3, false},
        {"scissor on/off", ScenarioScissor, false},
        {"Texture2DArray depth: per-slice DSV writes, per-slice reads, comparison, Texture2D declaration", ScenarioDepthArray, false},
        {"cube texture sampling", ScenarioCubeSampling, false},
        {"3D render target via GS SV_RenderTargetArrayIndex", ScenarioVolumeRender, false},
        {"MSAA 4x render, sample mask, ResolveSubresource, Texture2DMS reads", ScenarioMsaaResolve, false},
        {"tessellation (HS/DS, patch list)", ScenarioTessellation, false},
        {"Texture1D and Texture3D sampling", ScenarioTexture1D3D, false},
        {"compute RWTexture2D<float2> ping-pong", ScenarioComputePingPong, false},
        {"deferred contexts on 3 threads + ExecuteCommandList", ScenarioDeferredContexts, false},
        {"sampler heap churn (1200 tables in one list)", ScenarioSamplerHeapChurn, false},
        {"indexed draws: static VB/IB, dynamic IB, base vertex, instancing, strip cut, lines, points", ScenarioIndexedDraws, false},
        {"dynamic data across frames (NO_OVERWRITE stream, constant buffer not remapped)", ScenarioCrossFrameDynamic, false},
        {"objects released while their work is not submitted", ScenarioReleaseInFlight, false},
        {"block compressed mip chains down to 1x1 (BC1, BC3)", ScenarioBlockCompressed, false},
        {"UNORM texture copied into an SNORM staging texture", ScenarioFormatFamilyCopy, false},
        {"64 MB texture upload (4096x4096)", ScenarioLargeTexture, false},
        {"ID3DUserDefinedAnnotation scopes across submissions and command lists", ScenarioAnnotations, false},
        {"native interop: BeginNative/Require/EndNative with foreign heaps", ScenarioNativeInterop, false},
        {"queries: occlusion, predicate, event, timestamp + disjoint", ScenarioQueries, false},
        {"queries used wrongly (never ended, open, wrong size)", ScenarioQueryMisuse, false, true},
        {"staging readback polled with DO_NOT_WAIT", ScenarioStagingDoNotWait, false},
        {"CopySubresourceRegion (one texel, regions, array slices)", ScenarioCopySubresourceRegion, false},
        {"CopyResource of a bound D24S8 depth buffer, read via R24_UNORM_X8", ScenarioDepthCopy, false},
        {"DYNAMIC texture Map(DISCARD) uploads", ScenarioDynamicTexture, false},
        {"UpdateSubresource on a DEFAULT texture", ScenarioUpdateSubresource, false},
        {"D3D11 hazard rules", ScenarioHazards, false},
        {"CreateInputLayout validation", ScenarioInputLayout, false, true},
        {"ClearState then Get*", ScenarioClearState, false},
        {"ImGui contract: DXGI device chain, Get* arrays", ScenarioImGuiContract, false},
        {"swap chain: presents, back buffer proxy, ResizeBuffers", ScenarioSwapChain, true},
    };
    // The DirectX 12 feature lanes' files (PLAN.md C20).
    for (const std::vector<Scenario>* lane : {&NativeScenarios(), &ViewScenarios(), &AsyncScenarios(), &DxilScenarios(),
             &VrsScenarios(), &MeshScenarios(), &TexheapScenarios(), &RtScenarios()})
        base.insert(base.end(), lane->begin(), lane->end());
    return base;
    }();
    return list;
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_benchSource;
const char* const kBenchExtra = R"(
Texture2D a : register(t0);
Texture2D b : register(t1);
SamplerState s : register(s0);
float4 BenchPs(VsOut i) : SV_Target
{
    return a.Sample(s, i.uv) * b.Sample(s, i.uv) * color;
}
)";
} // namespace

double BenchmarkFrameMs(Gpu& g, u32 frames, u32 drawsPerFrame)
{
    if (g_benchSource.empty())
        g_benchSource = std::string(kQuadHlsl) + kBenchExtra;
    Result r;
    auto rt = MakeTexture2D(g, r, Desc2D(256, 256, kRgba8, kRtSrv));
    ComPtr<ID3D11Texture2D> textures[4];
    ComPtr<ID3D11ShaderResourceView> views[4];
    std::vector<u32> texels(64 * 64);
    for (u32 i = 0; i < 4; ++i)
    {
        for (u32 t = 0; t < 64 * 64; ++t)
            texels[t] = 0xFF000000u | (t * (i + 1) * 2654435761u >> 8);
        const D3D11_SUBRESOURCE_DATA init{texels.data(), 256, 0};
        textures[i] = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_SHADER_RESOURCE), &init);
        views[i] = MakeSrv(g, r, textures[i].Get());
    }
    Pipeline p = MakePipeline(g, r, g_benchSource.c_str(), "BenchPs");
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP);
    auto rtv = MakeRtv(g, r, rt.Get());
    if (!r.failures.empty())
    {
        std::printf("benchmark setup failed on %s: %s\n", g.name, r.failures[0].c_str());
        return 0;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    std::vector<double> times;
    const bool profile = g_profileBenchmark && g.layer;
    for (u32 frame = 0; frame < frames + 3; ++frame)
    {
        if (profile && frame == 3)
            ProfilerStart();
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 256, 256);
        Bind(g, p);
        g.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        for (u32 i = 0; i < drawsPerFrame; ++i)
        {
            ID3D11ShaderResourceView* pair[2] = {views[i & 3].Get(), views[(i + 1) & 3].Get()};
            g.ctx->PSSetShaderResources(0, 2, pair);
            const float x = float((i * 7) % 250), y = float((i * 13) % 250);
            const QuadConstants q = QuadPx(x, y, x + 6, y + 6, 256, 256, 1, float(i & 255) / 255.0f, 0.5f, 1);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g.ctx->Map(p.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                std::memcpy(m.pData, &q, sizeof(q));
                g.ctx->Unmap(p.cb.Get(), 0);
            }
            g.ctx->Draw(6, 0);
        }
        g.ctx->Flush();
        QueryPerformanceCounter(&end);
        if (frame >= 3)
            times.push_back(double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart));
        g.ctx->ClearState();
        WaitIdle(g);
        EndFrame(g);
    }
    if (profile)
        ProfilerStop(g.name, 40);
    std::sort(times.begin(), times.end());
    return times.empty() ? 0 : times[times.size() / 2];
}
} // namespace layertest
