// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `infra` lane for async compute (docs/dead-air/dx12-features/infra.md 9.1
// scenarios 5-7, async.md 9.1 scenarios A-E). Scenarios() appends this list.
//
// The layer runs each scenario twice: on the test device (compute queue) and on a second layer device
// created with -dx12_no_async (jobs in order on the direct queue); the D3D11 reference runs it twice
// with the job's work executed where the layer forks. Every run must produce the reference's images.
#include "harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr u32 kSize = 64;
constexpr u32 kGroups = kSize / 8;

const char* const kGfxHlsl = R"(
Texture2D tex : register(t0);
ByteAddressBuffer words : register(t1);
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
float4 CheckerPs(VsOut i) : SV_Target
{
    uint2 p = uint2(i.pos.xy) / 8;
    return ((p.x + p.y) & 1) ? float4(0.9, 0.2, 0.1, 1) : float4(0.1, 0.3, 0.8, 1);
}
float4 SamplePs(VsOut i) : SV_Target
{
    return tex.Sample(smp, i.uv).bgra;
}
float4 UnpackPs(VsOut i) : SV_Target
{
    uint v = words.Load((uint(i.pos.y) * 64 + uint(i.pos.x)) * 4);
    return float4(v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24) / 255.0;
}
)";

const char* const kPackHlsl = R"(
Texture2D<float4> tex : register(t0);
RWByteAddressBuffer words : register(u0);
[numthreads(8, 8, 1)]
void PackCs(uint3 id : SV_DispatchThreadID)
{
    uint4 b = uint4(tex.Load(int3(id.xy, 0)) * 255.0 + 0.5);
    words.Store((id.y * 64 + id.x) * 4, b.r | (b.g << 8) | ((255 - b.b) << 16) | (b.a << 24));
}
)";

const char* const kImageCsHlsl = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
[numthreads(8, 8, 1)]
void InvertCs(uint3 id : SV_DispatchThreadID)
{
    float4 c = src.Load(int3(id.xy, 0));
    dst[id.xy] = float4(1.0 - c.rgb, 1.0);
}
[numthreads(8, 8, 1)]
void HistoryCs(uint3 id : SV_DispatchThreadID)
{
    float4 h = src.Load(int3(id.xy, 0));
    dst[id.xy] = float4(h.rgb * 0.5 + float3(id.x, id.y, 32) / 128.0, 1.0);
}
[numthreads(8, 8, 1)]
void BlurCs(uint3 id : SV_DispatchThreadID)
{
    float4 a = src.Load(int3(id.xy, 0));
    float4 b = src.Load(int3(min(id.xy + 1, 63), 0));
    dst[id.xy] = a * 0.75 + b * 0.25;
}
)";

// Compute results may round differently between FXC and DXC (-dxil): one 8-bit step.
double Tolerance()
{
    return g_dxil ? 1.0 : 0.0;
}

xrd3d12::Device* DeviceOf(Gpu& g)
{
    return g.layer ? xrd3d12::Device::From(g.dev) : nullptr;
}

bool HasComputeQueue(Gpu& g)
{
    const xrd3d12::NativeCaps* caps = g.layer ? xrd3d12::GetNativeCaps(g.dev) : nullptr;
    return caps && caps->asyncCompute;
}

// Counters of -dxstats around a piece of a scenario (layer only).
struct StatWindow
{
    xrd3d12::Device* dev = nullptr;
    bool saved = false;
    u64 base[xrd3d12::Device::StatCount]{};
    explicit StatWindow(Gpu& g) : dev(DeviceOf(g))
    {
        if (!dev)
            return;
        saved = dev->stats;
        dev->stats = true;
        Reset();
    }
    ~StatWindow()
    {
        if (dev)
            dev->stats = saved;
    }
    void Reset()
    {
        if (dev)
            for (u32 i = 0; i < xrd3d12::Device::StatCount; ++i)
                base[i] = dev->statCounters[i].load();
    }
    u64 Delta(xrd3d12::Device::Stat s) const { return dev ? dev->statCounters[s].load() - base[s] : 0; }
};

// ---- jobs ------------------------------------------------------------------------------------------

// A compute job recorded through the D3D11 API on a deferred context. Layer: compute recording, a job
// forked and joined on the immediate context. Reference: the same list executed where the layer forks.
struct D3D11Job
{
    ComPtr<ID3D11DeviceContext> def;
    xrd3d12::AsyncJob* job = nullptr;
};

bool BeginD3D11Job(Gpu& g, Result& r, D3D11Job& j)
{
    if (!r.Ok(g.dev->CreateDeferredContext(0, &j.def), "CreateDeferredContext"))
        return false;
    return !g.layer || r.Check(xrd3d12::BeginAsyncRecording(j.def.Get()), "BeginAsyncRecording refused");
}

void ForkD3D11Job(Gpu& g, Result& r, D3D11Job& j, const char* name)
{
    ComPtr<ID3D11CommandList> list;
    if (!r.Ok(j.def->FinishCommandList(FALSE, &list), "FinishCommandList"))
        return;
    if (!g.layer)
    {
        g.ctx->ExecuteCommandList(list.Get(), TRUE);
        return;
    }
    j.job = xrd3d12::AsyncJobFromCommandList(g.dev, name, list.Get());
    if (r.Check(j.job, "AsyncJobFromCommandList(%s) failed", name))
        xrd3d12::ForkAsyncCompute(g.ctx, j.job);
}

void JoinD3D11Job(Gpu& g, D3D11Job& j)
{
    if (g.layer && j.job)
        xrd3d12::JoinAsyncCompute(g.ctx, j.job);
    j.job = nullptr;
}

// A layer device created with -dx12_no_async.
struct InOrderDevice
{
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ~InOrderDevice()
    {
        if (ctx)
            ctx->Release();
        if (dev)
            dev->Release();
    }
};

bool MakeInOrderDevice(Result& r, InOrderDevice& out)
{
    xrd3d12::DeviceCreateInfo info = LayerDevice()->createInfo;
    info.adapter = Adapter();
    info.pipelineCachePath = nullptr;
    info.disabledFeatures |= xrd3d12::NativeFeatureAsyncCompute;
    if (!r.Ok(xrd3d12::CreateDevice(info, &out.dev, &out.ctx, nullptr), "CreateDevice(-dx12_no_async)"))
        return false;
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(out.dev);
    return r.Check(caps && !caps->asyncCompute && !xrd3d12::Device::From(out.dev)->computeQueue,
        "-dx12_no_async left a compute queue");
}

// Runs `body` on the device of `g` and again with jobs in order: the layer on a -dx12_no_async
// device, the reference twice (its jobs always run in order).
template <class Body>
void OnBothPaths(Gpu& g, Result& r, Body body)
{
    body(g, r);
    if (!g.layer)
    {
        body(g, r);
        return;
    }
    r.Check(HasComputeQueue(g), "the test device has no compute queue (acq=0)");
    InOrderDevice other;
    if (!MakeInOrderDevice(r, other))
        return;
    Gpu inOrder{"d3d12 layer (no compute queue)", true, other.dev, other.ctx};
    body(inOrder, r);
    EndFrame(inOrder);
    WaitIdle(inOrder);
}

// ---- common resources ------------------------------------------------------------------------------

struct Target
{
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

Target MakeTarget(Gpu& g, Result& r, UINT bind)
{
    Target t;
    t.tex = MakeTexture2D(g, r, Desc2D(kSize, kSize, kRgba8, bind));
    if (!t.tex)
        return t;
    if (bind & D3D11_BIND_RENDER_TARGET)
        t.rtv = MakeRtv(g, r, t.tex.Get());
    if (bind & D3D11_BIND_SHADER_RESOURCE)
        t.srv = MakeSrv(g, r, t.tex.Get());
    if (bind & D3D11_BIND_UNORDERED_ACCESS)
        r.Ok(g.dev->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav), "CreateUnorderedAccessView");
    return t;
}

// Fullscreen draw of `ps` into `target` on `ctx`, reading `srv` (t0) or `words` (t1) when given.
void FullscreenOn(Gpu& g, Result& r, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, const char* ps,
    ID3D11ShaderResourceView* srv = nullptr, ID3D11ShaderResourceView* words = nullptr)
{
    auto vs = MakeVs(g, r, kGfxHlsl, "FullVs");
    auto pixel = MakePs(g, r, kGfxHlsl, ps);
    auto smp = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    if (!vs || !pixel)
        return;
    ctx->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(pixel.Get(), nullptr, 0);
    ID3D11ShaderResourceView* views[2] = {srv, words};
    ctx->PSSetShaderResources(0, 2, views);
    ctx->PSSetSamplers(0, 1, smp.GetAddressOf());
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void Fullscreen(Gpu& g, Result& r, ID3D11RenderTargetView* target, const char* ps, ID3D11ShaderResourceView* srv = nullptr,
    ID3D11ShaderResourceView* words = nullptr)
{
    FullscreenOn(g, r, g.ctx, target, ps, srv, words);
}

// One dispatch of `entry` (kImageCsHlsl) on `ctx`: src (t0) -> dst (u0), bindings cleared after it.
void ImageDispatch(Gpu& g, Result& r, ID3D11DeviceContext* ctx, const char* entry, ID3D11ShaderResourceView* src,
    ID3D11UnorderedAccessView* dst)
{
    auto cs = MakeCs(g, r, kImageCsHlsl, entry);
    if (!cs)
        return;
    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &dst, nullptr);
    ctx->Dispatch(kGroups, kGroups, 1);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &noSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
}

// ---- 5-6: native jobs ------------------------------------------------------------------------------

// FXC cs_5_1 of a native job (kept for the process, like Compile()).
ID3DBlob* CompileNative(const char* source, const char* entry)
{
    static std::map<std::string, ID3DBlob*> cache;
    if (const auto it = cache.find(entry); it != cache.end())
        return it->second;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(source, std::strlen(source), entry, nullptr, nullptr, entry, "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &code, &errors)))
    {
        std::printf("FATAL shader %s (cs_5_1) does not compile: %s\n", entry,
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        std::exit(3);
    }
    if (errors)
        errors->Release();
    cache.emplace(entry, code);
    return code;
}

// The hand-off of infra.md 9.1 scenario 5: a PS writes T; a native job reads T (entry
// NON_PIXEL_SHADER_RESOURCE, read-shared) and writes the raw buffer B; a PS reads T while the job runs;
// after the join a PS reads B. `join` false: no join, the frame boundary joins (scenario 6).
void NativeHandOff(Gpu& g, Result& r, bool join)
{
    Target t = MakeTarget(g, r, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    Target side = MakeTarget(g, r, D3D11_BIND_RENDER_TARGET);
    Target result = MakeTarget(g, r, D3D11_BIND_RENDER_TARGET);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kSize * kSize * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> words;
    r.Ok(g.dev->CreateBuffer(&bd, nullptr, &words), "CreateBuffer(words)");
    REQUIRE_OK();
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kSize * kSize;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> wordsUav;
    r.Ok(g.dev->CreateUnorderedAccessView(words.Get(), &ud, &wordsUav), "CreateUnorderedAccessView(raw)");
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = kSize * kSize;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ComPtr<ID3D11ShaderResourceView> wordsSrv;
    r.Ok(g.dev->CreateShaderResourceView(words.Get(), &sd, &wordsSrv), "CreateShaderResourceView(raw)");
    auto packCs = MakeCs(g, r, kPackHlsl, "PackCs");
    REQUIRE_OK();

    Fullscreen(g, r, t.rtv.Get(), "UvPs");
    xrd3d12::AsyncJob* job = nullptr;
    if (g.layer)
    {
        const D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 1}};
        D3D12_ROOT_PARAMETER1 param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{};
        rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rd.Desc_1_1.NumParameters = 1;
        rd.Desc_1_1.pParameters = &param;
        ID3D12RootSignature* root = xrd3d12::RootSignature(g.dev, rd, L"layertest async pack");
        ID3DBlob* code = CompileNative(kPackHlsl, "PackCs");
        ID3D12PipelineState* pso =
            root ? xrd3d12::ComputePipeline(g.dev, root, code->GetBufferPointer(), code->GetBufferSize(), L"layertest async pack") : nullptr;
        if (!r.Check(root && pso, "native pack pipeline refused"))
            return;
        xrd3d12::NativeScope scope;
        job = xrd3d12::BeginAsyncCompute(g.dev, "layertest_pack", scope);
        if (!r.Check(job && scope.list && scope.heap, "BeginAsyncCompute refused"))
            return;
        r.Check(!scope.list5 && !scope.list6, "a job list hands out VRS / mesh interfaces");
        xrd3d12::AsyncUse(job, t.tex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        xrd3d12::AsyncUse(job, words.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        const xrd3d12::DescriptorSpan span = xrd3d12::AsyncDescriptors(job, 2);
        if (!r.Check(span.count == 2, "AsyncDescriptors failed"))
            return;
        ID3D12Device* device = xrd3d12::NativeDevice(g.dev);
        device->CopyDescriptorsSimple(1, span.cpu, xrd3d12::NativeDescriptor(t.srv.Get()), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CopyDescriptorsSimple(1, {span.cpu.ptr + span.increment}, xrd3d12::NativeDescriptor(wordsUav.Get()),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        scope.list->SetComputeRootSignature(root);
        scope.list->SetPipelineState(pso);
        scope.list->SetComputeRootDescriptorTable(0, span.gpu);
        scope.list->Dispatch(kGroups, kGroups, 1);
        xrd3d12::EndAsyncCompute(job);
        xrd3d12::ForkAsyncCompute(g.ctx, job);
    }
    else
    {
        g.ctx->CSSetShader(packCs.Get(), nullptr, 0);
        g.ctx->CSSetShaderResources(0, 1, t.srv.GetAddressOf());
        g.ctx->CSSetUnorderedAccessViews(0, 1, wordsUav.GetAddressOf(), nullptr);
        g.ctx->Dispatch(kGroups, kGroups, 1);
        ID3D11ShaderResourceView* noSrv = nullptr;
        ID3D11UnorderedAccessView* noUav = nullptr;
        g.ctx->CSSetShaderResources(0, 1, &noSrv);
        g.ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }

    // T is read-shared: the direct queue samples it while the job runs, without a join.
    StatWindow stats(g);
    Fullscreen(g, r, side.rtv.Get(), "SamplePs", t.srv.Get());
    r.Check(!stats.Delta(xrd3d12::Device::StatAutoJoins), "a read of the read-shared texture joined the job");
    if (join)
    {
        if (g.layer)
            xrd3d12::JoinAsyncCompute(g.ctx, job);
    }
    else
    {
        const int before = g.layer ? LayerLogCount("not joined, joined at Present") : 0;
        EndFrame(g);
        if (HasComputeQueue(g))
            r.Check(LayerLogCount("not joined, joined at Present") == 1 && before <= 1,
                "the frame boundary did not log the forgotten join once");
    }
    Fullscreen(g, r, result.rtv.Get(), "UnpackPs", nullptr, wordsSrv.Get());
    r.images.push_back(Read2D(g, r, result.tex.Get(), 0, "buffer written by the job", Tolerance()));
    r.images.push_back(Read2D(g, r, side.tex.Get(), 0, "texture read while the job ran"));
}

// 5. PS -> native job -> PS, with a read-shared texture and a COMMON buffer.
void ScenarioAsyncHandOff(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) { NativeHandOff(gpu, res, true); });
}

// 6. The same fork without a join: the frame boundary joins it (one log line), the result is valid.
void ScenarioAsyncNotJoined(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) { NativeHandOff(gpu, res, false); });
}

// ---- 7, A-E: jobs recorded through D3D11 -----------------------------------------------------------

// 7. The direct queue draws from a texture the job writes (lent exclusively) between the fork and the
// join: the guard joins right there (counted, logged once) and the draw sees the job's result.
void ScenarioAsyncGuard(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) {
        Target s = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Target t = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target out = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        if (!res.failures.empty())
            return;
        Fullscreen(gpu, res, s.rtv.Get(), "CheckerPs");
        D3D11Job job;
        if (!BeginD3D11Job(gpu, res, job))
            return;
        ImageDispatch(gpu, res, job.def.Get(), "InvertCs", s.srv.Get(), t.uav.Get());
        ForkD3D11Job(gpu, res, job, "layertest_guard");
        StatWindow stats(gpu);
        Fullscreen(gpu, res, out.rtv.Get(), "SamplePs", t.srv.Get());
        if (HasComputeQueue(gpu))
        {
            res.Check(stats.Delta(xrd3d12::Device::StatAutoJoins) == 1, "the guard joined %llu times (1 expected)",
                static_cast<unsigned long long>(stats.Delta(xrd3d12::Device::StatAutoJoins)));
            res.Check(LayerLogCount("joined early: the direct queue needs") == 1, "the guard join was not logged once");
        }
        JoinD3D11Job(gpu, job);
        res.images.push_back(Read2D(gpu, res, out.tex.Get(), 0, "read of the lent texture", Tolerance()));
    });
}

// A. A compute recording reads S (which the direct queue samples meanwhile) and writes T; the join;
// a PS reads T.
void AsyncRecordingBody(Gpu& g, Result& r)
{
    Target s = MakeTarget(g, r, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    Target t = MakeTarget(g, r, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    Target during = MakeTarget(g, r, D3D11_BIND_RENDER_TARGET);
    Target after = MakeTarget(g, r, D3D11_BIND_RENDER_TARGET);
    REQUIRE_OK();
    Fullscreen(g, r, s.rtv.Get(), "UvPs");
    D3D11Job job;
    if (!BeginD3D11Job(g, r, job))
        return;
    ImageDispatch(g, r, job.def.Get(), "InvertCs", s.srv.Get(), t.uav.Get());
    ForkD3D11Job(g, r, job, "layertest_recording");
    StatWindow stats(g);
    Fullscreen(g, r, during.rtv.Get(), "SamplePs", s.srv.Get());
    r.Check(!stats.Delta(xrd3d12::Device::StatAutoJoins), "sampling the read-shared source joined the job");
    JoinD3D11Job(g, job);
    Fullscreen(g, r, after.rtv.Get(), "SamplePs", t.srv.Get());
    r.images.push_back(Read2D(g, r, during.tex.Get(), 0, "source sampled during the job"));
    r.images.push_back(Read2D(g, r, after.tex.Get(), 0, "job result sampled after the join", Tolerance()));
}

void ScenarioAsyncRecording(Gpu& g, Result& r)
{
    OnBothPaths(g, r, AsyncRecordingBody);
}

// B. A draw and an RTV clear on a compute recording are dropped (one log line each); the rest of the
// list runs. The reference records neither.
void ScenarioAsyncRefusals(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) {
        Target s = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Target t = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target x = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        auto vs = MakeVs(gpu, res, kGfxHlsl, "FullVs");
        auto ps = MakePs(gpu, res, kGfxHlsl, "UvPs");
        if (!res.failures.empty())
            return;
        Fullscreen(gpu, res, s.rtv.Get(), "CheckerPs");
        Fullscreen(gpu, res, x.rtv.Get(), "UvPs");
        D3D11Job job;
        if (!BeginD3D11Job(gpu, res, job))
            return;
        ID3D11DeviceContext* def = job.def.Get();
        if (gpu.layer)
        {
            const float red[4] = {1, 0, 0, 1};
            def->ClearRenderTargetView(x.rtv.Get(), red);
            def->OMSetRenderTargets(1, x.rtv.GetAddressOf(), nullptr);
            const D3D11_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
            def->RSSetViewports(1, &vp);
            def->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            def->VSSetShader(vs.Get(), nullptr, 0);
            def->PSSetShader(ps.Get(), nullptr, 0);
            def->Draw(3, 0);
            def->OMSetRenderTargets(0, nullptr, nullptr);
        }
        ImageDispatch(gpu, res, def, "InvertCs", s.srv.Get(), t.uav.Get());
        ForkD3D11Job(gpu, res, job, "layertest_refusals");
        JoinD3D11Job(gpu, job);
        if (gpu.layer)
        {
            res.Check(LayerLogCount("compute recording (async job): ClearRenderTargetView dropped") == 1,
                "the dropped clear was not logged once");
            res.Check(LayerLogCount("compute recording (async job): draws dropped") == 1, "the dropped draw was not logged once");
        }
        res.images.push_back(Read2D(gpu, res, x.tex.Get(), 0, "target of the dropped clear and draw"));
        res.images.push_back(Read2D(gpu, res, t.tex.Get(), 0, "dispatch of the same list", Tolerance()));
    });
}

// C. Read-shared lending: sampling S during the job inserts no join; rendering into S does.
void ScenarioAsyncSharedLending(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) {
        Target s = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Target t = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target read = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        Target after = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        if (!res.failures.empty())
            return;
        Fullscreen(gpu, res, s.rtv.Get(), "UvPs");
        D3D11Job job;
        if (!BeginD3D11Job(gpu, res, job))
            return;
        ImageDispatch(gpu, res, job.def.Get(), "InvertCs", s.srv.Get(), t.uav.Get());
        ForkD3D11Job(gpu, res, job, "layertest_shared");
        const bool async = HasComputeQueue(gpu);
        StatWindow stats(gpu);
        Fullscreen(gpu, res, read.rtv.Get(), "SamplePs", s.srv.Get());
        if (async)
            res.Check(!stats.Delta(xrd3d12::Device::StatAutoJoins), "a read of the shared texture joined");
        // A write needs a transition: the job (still reading S) is joined first.
        Fullscreen(gpu, res, s.rtv.Get(), "CheckerPs");
        if (async)
            res.Check(stats.Delta(xrd3d12::Device::StatAutoJoins) == 1, "rendering into the shared texture joined %llu times (1 expected)",
                static_cast<unsigned long long>(stats.Delta(xrd3d12::Device::StatAutoJoins)));
        JoinD3D11Job(gpu, job);
        Fullscreen(gpu, res, after.rtv.Get(), "SamplePs", t.srv.Get());
        res.images.push_back(Read2D(gpu, res, read.tex.Get(), 0, "shared read during the job"));
        res.images.push_back(Read2D(gpu, res, s.tex.Get(), 0, "shared texture rendered after the guard"));
        res.images.push_back(Read2D(gpu, res, after.tex.Get(), 0, "job result (read the old contents)", Tolerance()));
    });
}

// D. The march's history pattern inside one job, two frames: T0 UAV -> SRV -> COPY_SOURCE, T1 SRV ->
// COPY_DEST; the direct queue adopts the exit states and hands them in again the next frame.
void ScenarioAsyncHistory(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) {
        Target t0 = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target t1 = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Target t2 = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target frames[2] = {MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET), MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET)};
        if (!res.failures.empty())
            return;
        Fullscreen(gpu, res, t1.rtv.Get(), "CheckerPs");
        for (u32 f = 0; f < 2; ++f)
        {
            D3D11Job job;
            if (!BeginD3D11Job(gpu, res, job))
                return;
            ImageDispatch(gpu, res, job.def.Get(), "HistoryCs", t1.srv.Get(), t0.uav.Get());
            ImageDispatch(gpu, res, job.def.Get(), "BlurCs", t0.srv.Get(), t2.uav.Get());
            job.def->CopyResource(t1.tex.Get(), t0.tex.Get());
            ForkD3D11Job(gpu, res, job, "layertest_history");
            JoinD3D11Job(gpu, job);
            Fullscreen(gpu, res, frames[f].rtv.Get(), "SamplePs", t2.srv.Get());
            EndFrame(gpu);
        }
        res.images.push_back(Read2D(gpu, res, frames[0].tex.Get(), 0, "frame 1", Tolerance()));
        res.images.push_back(Read2D(gpu, res, frames[1].tex.Get(), 0, "frame 2 (history of frame 1)", Tolerance()));
        res.images.push_back(Read2D(gpu, res, t1.tex.Get(), 0, "history after two frames", Tolerance()));
    });
}

// E. Scenario A on a device without the compute queue only (the reference once).
void ScenarioAsyncNoQueue(Gpu& g, Result& r)
{
    if (!g.layer)
    {
        AsyncRecordingBody(g, r);
        return;
    }
    InOrderDevice other;
    if (!MakeInOrderDevice(r, other))
        return;
    Gpu inOrder{"d3d12 layer (no compute queue)", true, other.dev, other.ctx};
    AsyncRecordingBody(inOrder, r);
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(other.dev);
    r.Check(caps && !caps->asyncCompute, "acq=1 without a compute queue");
    EndFrame(inOrder);
    WaitIdle(inOrder);
}

// F. A deferred graphics list samples S (read-shared with a running job), then renders into it: its
// transition would change S under the job, so executing the list joins the job first.
void ScenarioAsyncDeferredWrite(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) {
        Target s = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Target t = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target read = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        Target after = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11DeviceContext> gfx;
        res.Ok(gpu.dev->CreateDeferredContext(0, &gfx), "CreateDeferredContext(graphics)");
        if (!res.failures.empty())
            return;
        Fullscreen(gpu, res, s.rtv.Get(), "UvPs");
        D3D11Job job;
        if (!BeginD3D11Job(gpu, res, job))
            return;
        ImageDispatch(gpu, res, job.def.Get(), "InvertCs", s.srv.Get(), t.uav.Get());
        ForkD3D11Job(gpu, res, job, "layertest_deferred_write");
        FullscreenOn(gpu, res, gfx.Get(), read.rtv.Get(), "SamplePs", s.srv.Get());
        FullscreenOn(gpu, res, gfx.Get(), s.rtv.Get(), "CheckerPs");
        ComPtr<ID3D11CommandList> list;
        if (!res.Ok(gfx->FinishCommandList(FALSE, &list), "FinishCommandList(graphics)"))
            return;
        StatWindow stats(gpu);
        gpu.ctx->ExecuteCommandList(list.Get(), FALSE);
        if (HasComputeQueue(gpu))
            res.Check(stats.Delta(xrd3d12::Device::StatAutoJoins) == 1, "the list rendering into the shared texture joined %llu times (1 expected)",
                static_cast<unsigned long long>(stats.Delta(xrd3d12::Device::StatAutoJoins)));
        JoinD3D11Job(gpu, job);
        Fullscreen(gpu, res, after.rtv.Get(), "SamplePs", t.srv.Get());
        res.images.push_back(Read2D(gpu, res, read.tex.Get(), 0, "shared texture sampled by the deferred list"));
        res.images.push_back(Read2D(gpu, res, s.tex.Get(), 0, "shared texture rendered by the deferred list"));
        res.images.push_back(Read2D(gpu, res, after.tex.Get(), 0, "job result (read the old contents)", Tolerance()));
    });
}

// G. A compute recording handed to ExecuteCommandList instead of AsyncJobFromCommandList: the layer runs
// it as a job joined at once (logged once), never on the direct queue.
void ScenarioAsyncExecuteRecording(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) {
        Target s = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Target t = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target after = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        if (!res.failures.empty())
            return;
        Fullscreen(gpu, res, s.rtv.Get(), "UvPs");
        D3D11Job job;
        if (!BeginD3D11Job(gpu, res, job))
            return;
        ImageDispatch(gpu, res, job.def.Get(), "InvertCs", s.srv.Get(), t.uav.Get());
        ComPtr<ID3D11CommandList> list;
        if (!res.Ok(job.def->FinishCommandList(FALSE, &list), "FinishCommandList"))
            return;
        StatWindow stats(gpu);
        gpu.ctx->ExecuteCommandList(list.Get(), TRUE);
        if (HasComputeQueue(gpu))
        {
            res.Check(stats.Delta(xrd3d12::Device::StatForks) == 1 && stats.Delta(xrd3d12::Device::StatJoins) == 1,
                "the executed compute recording did not run as one job joined at once");
            res.Check(LayerLogCount("ExecuteCommandList of a compute recording") == 1, "the executed compute recording was not logged once");
        }
        Fullscreen(gpu, res, after.rtv.Get(), "SamplePs", t.srv.Get());
        res.images.push_back(Read2D(gpu, res, after.tex.Get(), 0, "result of the executed compute recording", Tolerance()));
    });
}
// H. A deferred graphics list that only samples S (read-shared with a running job, in the widened read
// state) records no barrier for it: executing the list joins nothing, and S keeps its state for the job.
void ScenarioAsyncDeferredRead(Gpu& g, Result& r)
{
    OnBothPaths(g, r, [](Gpu& gpu, Result& res) {
        Target s = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Target t = MakeTarget(gpu, res, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Target read = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        Target after = MakeTarget(gpu, res, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11DeviceContext> gfx;
        res.Ok(gpu.dev->CreateDeferredContext(0, &gfx), "CreateDeferredContext(graphics)");
        if (!res.failures.empty())
            return;
        Fullscreen(gpu, res, s.rtv.Get(), "UvPs");
        D3D11Job job;
        if (!BeginD3D11Job(gpu, res, job))
            return;
        ImageDispatch(gpu, res, job.def.Get(), "InvertCs", s.srv.Get(), t.uav.Get());
        ForkD3D11Job(gpu, res, job, "layertest_deferred_read");
        FullscreenOn(gpu, res, gfx.Get(), read.rtv.Get(), "SamplePs", s.srv.Get());
        ComPtr<ID3D11CommandList> list;
        if (!res.Ok(gfx->FinishCommandList(FALSE, &list), "FinishCommandList(graphics)"))
            return;
        StatWindow stats(gpu);
        gpu.ctx->ExecuteCommandList(list.Get(), FALSE);
        const bool async = HasComputeQueue(gpu);
        if (async)
            res.Check(!stats.Delta(xrd3d12::Device::StatAutoJoins), "a deferred list that only samples the shared texture joined");
        // The tracked state is still the loan's: a write needs a transition, so the job is joined first.
        Fullscreen(gpu, res, s.rtv.Get(), "CheckerPs");
        if (async)
            res.Check(stats.Delta(xrd3d12::Device::StatAutoJoins) == 1, "rendering into the shared texture joined %llu times (1 expected)",
                static_cast<unsigned long long>(stats.Delta(xrd3d12::Device::StatAutoJoins)));
        JoinD3D11Job(gpu, job);
        Fullscreen(gpu, res, after.rtv.Get(), "SamplePs", t.srv.Get());
        res.images.push_back(Read2D(gpu, res, read.tex.Get(), 0, "shared texture sampled by the deferred list"));
        res.images.push_back(Read2D(gpu, res, s.tex.Get(), 0, "shared texture rendered after the guard"));
        res.images.push_back(Read2D(gpu, res, after.tex.Get(), 0, "job result (read the old contents)", Tolerance()));
    });
}
} // namespace

const std::vector<Scenario>& AsyncScenarios()
{
    static const std::vector<Scenario> list = {
        {"async compute hand-off: PS -> native job (shared texture, COMMON buffer) -> PS, with and without a compute queue",
            ScenarioAsyncHandOff, false},
        {"async job not joined: the frame boundary joins it", ScenarioAsyncNotJoined, false},
        {"async lent guard: a draw from a lent texture joins the job", ScenarioAsyncGuard, false},
        {"async A: D3D11 compute recording, read-shared source, join", ScenarioAsyncRecording, false},
        {"async B: draws and RTV clears on a compute recording are dropped", ScenarioAsyncRefusals, false},
        {"async C: read-shared lending, a write joins", ScenarioAsyncSharedLending, false},
        {"async D: compute-list hand-off over two frames (history copy)", ScenarioAsyncHistory, false},
        {"async E: compute recording without a compute queue", ScenarioAsyncNoQueue, false},
        {"async F: a deferred list that samples then renders into a read-shared texture joins", ScenarioAsyncDeferredWrite, false},
        {"async G: ExecuteCommandList of a compute recording runs as a job joined at once", ScenarioAsyncExecuteRecording, false},
        {"async H: a deferred list that only samples a read-shared texture joins nothing", ScenarioAsyncDeferredRead, false},
    };
    return list;
}
} // namespace layertest
