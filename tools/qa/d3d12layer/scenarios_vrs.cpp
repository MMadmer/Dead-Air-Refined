// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `vrs` lane (docs/dead-air/dx12-features/vrs.md 9.2). Scenarios() appends this list.
//
// The layer draws with the real shading rate and counts pixel shader invocations; the Direct3D 11
// reference emulates it: its pixel shader evaluates the same function at the centre of the coarse
// pixel the rate gives (the position a coarse invocation sees) and counts one invocation per coarse
// pixel. Images and counts must match exactly.
#include "harness.h"

#include <cstring>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr u32 kSize = 256;

const char* const kVrsHlsl = R"(
cbuffer Mode : register(b0)
{
    // x: base rate (D3D12_SHADING_RATE), y: image combiner (0 passthrough, 1 override, 2 min),
    // z: image tile size, w: additional rates supported
    uint4 mode;
};
Texture2D<uint> rates : register(t0);
RWTexture2D<uint> counter : register(u1);
struct VsOut
{
    float4 pos : SV_Position;
};
VsOut FullVs(uint id : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 Shade(float2 p)
{
    return float4(frac(p.x / 37.0), frac(p.y / 23.0), frac((p.x + 2.0 * p.y) / 101.0), 1);
}
// Layer: a coarse invocation sees the centre of its coarse pixel.
float4 CountPs(VsOut i) : SV_Target
{
    InterlockedAdd(counter[uint2(0, 0)], 1);
    return Shade(i.pos.xy);
}
float4 NativePs(VsOut i) : SV_Target
{
    return Shade(i.pos.xy);
}
uint2 RateSize(uint rate)
{
    return uint2(1u << ((rate >> 2) & 3), 1u << (rate & 3));
}
// Reference: the coarse pixel of the emulated rate, shaded at its centre, counted once.
float4 EmulatePs(VsOut i) : SV_Target
{
    uint2 px = uint2(i.pos.xy);
    uint2 size = RateSize(mode.x);
    if (mode.y != 0)
    {
        uint2 image = RateSize(rates.Load(int3(px / mode.z, 0)));
        size = mode.y == 1 ? image : min(size, image);
    }
    if (mode.w == 0)
        size = min(size, uint2(2, 2));
    uint2 origin = px / size * size;
    if (all(px == origin))
        InterlockedAdd(counter[uint2(0, 0)], 1);
    return Shade(float2(origin) + float2(size) * 0.5);
}
RWTexture2D<uint> image : register(u0);
[numthreads(8, 8, 1)]
void PatternCs(uint3 id : SV_DispatchThreadID)
{
    // 1x1, 2x2, 4x4 in diagonal stripes of tiles.
    uint k = (id.x + 2 * id.y) % 3;
    image[id.xy] = k == 0 ? 0u : k == 1 ? 5u : 10u;
}
)";

constexpr u8 k1x1 = D3D12_SHADING_RATE_1X1, k2x1 = D3D12_SHADING_RATE_2X1, k2x2 = D3D12_SHADING_RATE_2X2,
             k4x4 = D3D12_SHADING_RATE_4X4;

// What the device offers: without tier 2 SetShadingRate is ignored and the reference shades 1x1.
struct VrsCaps
{
    bool on = false;
    u32 tile = 16;
    bool additional = false;
};

VrsCaps Caps()
{
    VrsCaps c;
    if (xrd3d12::Device* dev = LayerDevice())
    {
        c.on = dev->caps.vrsTier >= 2;
        if (c.on)
            c.tile = dev->caps.vrsTileSize;
        c.additional = dev->caps.vrsAdditionalRates;
    }
    return c;
}

u32 TilesFor(u32 pixels, u32 tile)
{
    return (pixels + tile - 1) / tile;
}

// One rate setting: base, image combiner (0 none, 1 override, 2 min) and the image it uses.
struct Rate
{
    u8 base = k1x1;
    u32 combiner = 0;
    ID3D11Texture2D* image = nullptr;
    ID3D11ShaderResourceView* imageSrv = nullptr;
};

D3D12_SHADING_RATE_COMBINER ImageCombiner(u32 combiner)
{
    return combiner == 1 ? D3D12_SHADING_RATE_COMBINER_OVERRIDE
        : combiner == 2  ? D3D12_SHADING_RATE_COMBINER_MIN
                         : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
}

// Shared objects of a scenario.
struct Rig
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> countPs, emulatePs;
    ComPtr<ID3D11Buffer> mode;
    ComPtr<ID3D11Texture2D> counter;
    ComPtr<ID3D11UnorderedAccessView> counterUav;
    VrsCaps caps;

    bool Init(Gpu& g, Result& r)
    {
        caps = Caps();
        vs = MakeVs(g, r, kVrsHlsl, "FullVs");
        countPs = MakePs(g, r, kVrsHlsl, "CountPs");
        emulatePs = MakePs(g, r, kVrsHlsl, "EmulatePs");
        mode = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
        counter = MakeTexture2D(g, r, Desc2D(1, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS));
        if (counter)
            r.Ok(g.dev->CreateUnorderedAccessView(counter.Get(), nullptr, &counterUav), "CreateUnorderedAccessView(counter)");
        return r.failures.empty();
    }

    // The draw's state on `ctx`: target, counter, shaders; the layer gets the rate, the reference the
    // emulation constants and the image as an SRV. `emulateOnLayer` draws the reference shader on the
    // layer too (with 1x1 constants), which also binds the image as an input.
    void Bind(Gpu& g, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, u32 width, u32 height, const Rate& rate,
        const Rate& emulated, bool emulateOnLayer = false)
    {
        const UINT zero[4]{};
        ctx->ClearUnorderedAccessViewUint(counterUav.Get(), zero);
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 1, counterUav.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        const bool emulate = !g.layer || emulateOnLayer;
        ctx->PSSetShader(emulate ? emulatePs.Get() : countPs.Get(), nullptr, 0);
        const Rate& e = caps.on ? emulated : Rate{};
        const UINT constants[4] = {e.base, e.combiner, caps.tile, caps.additional ? 1u : 0u};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(mode.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, constants, sizeof(constants));
            ctx->Unmap(mode.Get(), 0);
        }
        ctx->PSSetConstantBuffers(0, 1, mode.GetAddressOf());
        ID3D11ShaderResourceView* srv = emulate ? (emulateOnLayer ? rate.imageSrv : e.imageSrv) : nullptr;
        ctx->PSSetShaderResources(0, 1, &srv);
        if (g.layer)
            xrd3d12::SetShadingRate(ctx, D3D12_SHADING_RATE(rate.base), D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                ImageCombiner(rate.combiner), rate.image);
    }

    void Unbind(Gpu& g, ID3D11DeviceContext* ctx)
    {
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
        if (g.layer)
            xrd3d12::SetShadingRate(ctx, D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, nullptr);
    }

    // The counter as a value named `name`.
    void Count(Gpu& g, Result& r, const char* name)
    {
        Image counted = Read2D(g, r, counter.Get(), 0, name);
        u32 value = 0;
        if (counted.texels.size() == 4)
            std::memcpy(&value, counted.texels.data(), 4);
        r.Value(name, value);
    }
};

struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    u32 width = 0, height = 0;
};

Target MakeTarget(Gpu& g, Result& r, u32 width, u32 height)
{
    Target t;
    t.width = width;
    t.height = height;
    t.texture = MakeTexture2D(g, r, Desc2D(width, height, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    if (t.texture)
        t.rtv = MakeRtv(g, r, t.texture.Get());
    return t;
}

// A rate image for a width x height target, texel (x, y) = fill(x, y).
template <class Fill>
ComPtr<ID3D11Texture2D> MakeRateImage(Gpu& g, Result& r, const VrsCaps& caps, u32 width, u32 height, Fill fill,
    ComPtr<ID3D11ShaderResourceView>* srv = nullptr, ComPtr<ID3D11UnorderedAccessView>* uav = nullptr)
{
    const u32 w = TilesFor(width, caps.tile), h = TilesFor(height, caps.tile);
    std::vector<u8> texels(size_t(w) * h);
    for (u32 y = 0; y < h; ++y)
    {
        for (u32 x = 0; x < w; ++x)
            texels[size_t(y) * w + x] = fill(x, y, w, h);
    }
    const D3D11_SUBRESOURCE_DATA data{texels.data(), w, 0};
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
    if (uav)
        bind |= D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> image = MakeTexture2D(g, r, Desc2D(w, h, DXGI_FORMAT_R8_UINT, bind), &data);
    if (image && srv)
        *srv = MakeSrv(g, r, image.Get());
    if (image && uav)
        r.Ok(g.dev->CreateUnorderedAccessView(image.Get(), nullptr, &(*uav)), "CreateUnorderedAccessView(rate image)");
    return image;
}

void Draw(Gpu& g, Result& r, Rig& rig, const Target& t, const Rate& rate, const Rate& emulated, const char* label,
    bool emulateOnLayer = false)
{
    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(t.rtv.Get(), black);
    rig.Bind(g, g.ctx, t.rtv.Get(), t.width, t.height, rate, emulated, emulateOnLayer);
    g.ctx->Draw(3, 0);
    rig.Unbind(g, g.ctx);
    r.images.push_back(Read2D(g, r, t.texture.Get(), 0, label));
    rig.Count(g, r, label);
}

// 1. vrs_rates: per-draw and image rates, the MIN cap, a mixed image.
void ScenarioRates(Gpu& g, Result& r)
{
    Rig rig;
    if (!rig.Init(g, r))
        return;
    Target t = MakeTarget(g, r, kSize, kSize);
    const auto uniform = [](u8 value) { return [value](u32, u32, u32, u32) { return value; }; };
    ComPtr<ID3D11ShaderResourceView> s2x2, s2x1, s4x4, sMixed;
    auto i2x2 = MakeRateImage(g, r, rig.caps, kSize, kSize, uniform(k2x2), &s2x2);
    auto i2x1 = MakeRateImage(g, r, rig.caps, kSize, kSize, uniform(k2x1), &s2x1);
    auto i4x4 = MakeRateImage(g, r, rig.caps, kSize, kSize, uniform(k4x4), &s4x4);
    // Left half of the tiles 2x2, right half 1x1: 5/8 of the invocations.
    auto iMixed = MakeRateImage(g, r, rig.caps, kSize, kSize, [](u32 x, u32, u32 w, u32) { return x < w / 2 ? k2x2 : k1x1; }, &sMixed);
    REQUIRE_OK();

    const auto image = [](u8 base, u32 combiner, ID3D11Texture2D* tex, ID3D11ShaderResourceView* srv) {
        Rate rate;
        rate.base = base;
        rate.combiner = combiner;
        rate.image = tex;
        rate.imageSrv = srv;
        return rate;
    };
    Draw(g, r, rig, t, Rate{}, Rate{}, "1x1");
    Draw(g, r, rig, t, image(k2x2, 0, nullptr, nullptr), image(k2x2, 0, nullptr, nullptr), "per-draw 2x2");
    const Rate r2x2 = image(k1x1, 1, i2x2.Get(), s2x2.Get());
    Draw(g, r, rig, t, r2x2, r2x2, "image 2x2 override");
    const Rate r2x1 = image(k1x1, 1, i2x1.Get(), s2x1.Get());
    Draw(g, r, rig, t, r2x1, r2x1, "image 2x1 override");
    const Rate r4x4 = image(k1x1, 1, i4x4.Get(), s4x4.Get());
    Draw(g, r, rig, t, r4x4, r4x4, "image 4x4 override");
    const Rate capped = image(k2x2, 2, i4x4.Get(), s4x4.Get());
    Draw(g, r, rig, t, capped, capped, "base 2x2 min image 4x4");
    const Rate mixed = image(k1x1, 1, iMixed.Get(), sMixed.Get());
    Draw(g, r, rig, t, mixed, mixed, "mixed image (left 2x2, right 1x1)");
}

// 2. vrs_deferred: the image is set on a deferred context; a compute shader on the immediate context
// writes it after the list was recorded and before it runs (UAV -> SHADING_RATE_SOURCE through the
// list's first requirement).
void ScenarioDeferred(Gpu& g, Result& r)
{
    Rig rig;
    if (!rig.Init(g, r))
        return;
    Target t = MakeTarget(g, r, kSize, kSize);
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    auto image = MakeRateImage(g, r, rig.caps, kSize, kSize, [](u32, u32, u32, u32) { return k1x1; }, &srv, &uav);
    auto cs = MakeCs(g, r, kVrsHlsl, "PatternCs");
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();

    Rate rate;
    rate.combiner = 1;
    rate.image = image.Get();
    rate.imageSrv = srv.Get();
    const float black[4] = {0, 0, 0, 1};
    def->ClearRenderTargetView(t.rtv.Get(), black);
    rig.Bind(g, def.Get(), t.rtv.Get(), kSize, kSize, rate, rate);
    def->Draw(3, 0);
    rig.Unbind(g, def.Get());
    ComPtr<ID3D11CommandList> list;
    r.Ok(def->FinishCommandList(FALSE, &list), "FinishCommandList");
    REQUIRE_OK();

    g.ctx->CSSetShader(cs.Get(), nullptr, 0);
    g.ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    const u32 tiles = TilesFor(kSize, rig.caps.tile);
    g.ctx->Dispatch((tiles + 7) / 8, (tiles + 7) / 8, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    g.ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    g.ctx->CSSetShader(nullptr, nullptr, 0);
    g.ctx->ExecuteCommandList(list.Get(), FALSE);

    r.images.push_back(Read2D(g, r, t.texture.Get(), 0, "deferred draw with a compute-written image"));
    rig.Count(g, r, "deferred invocations");
}

// 3. vrs_clear: the rate is gone after ClearState, ExecuteCommandList(FALSE) and FinishCommandList(FALSE).
void ScenarioClear(Gpu& g, Result& r)
{
    Rig rig;
    if (!rig.Init(g, r))
        return;
    Target t = MakeTarget(g, r, kSize, kSize);
    ComPtr<ID3D11DeviceContext> def, empty;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    r.Ok(g.dev->CreateDeferredContext(0, &empty), "CreateDeferredContext");
    REQUIRE_OK();
    Rate coarse;
    coarse.base = k2x2;
    const Rate full;
    const float black[4] = {0, 0, 0, 1};

    // Control: the rate is set and drawn with.
    Draw(g, r, rig, t, coarse, coarse, "control 2x2");

    // Bind() would set 1x1 on the layer itself: the rate goes in through SetShadingRate only here.
    const auto setCoarse = [&](ID3D11DeviceContext* ctx) {
        if (g.layer)
            xrd3d12::SetShadingRate(ctx, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, nullptr);
    };
    const auto bindNoRate = [&](ID3D11DeviceContext* ctx) {
        // Everything of Bind() except SetShadingRate.
        const UINT zero[4]{};
        ctx->ClearUnorderedAccessViewUint(rig.counterUav.Get(), zero);
        ID3D11RenderTargetView* rtv = t.rtv.Get();
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 1, rig.counterUav.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(rig.vs.Get(), nullptr, 0);
        ctx->PSSetShader(g.layer ? rig.countPs.Get() : rig.emulatePs.Get(), nullptr, 0);
        const UINT constants[4] = {k1x1, 0, rig.caps.tile, 1};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(rig.mode.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, constants, sizeof(constants));
            ctx->Unmap(rig.mode.Get(), 0);
        }
        ctx->PSSetConstantBuffers(0, 1, rig.mode.GetAddressOf());
    };
    const auto drawNoRate = [&](const char* label) {
        g.ctx->ClearRenderTargetView(t.rtv.Get(), black);
        bindNoRate(g.ctx);
        g.ctx->Draw(3, 0);
        g.ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
        r.images.push_back(Read2D(g, r, t.texture.Get(), 0, label));
        rig.Count(g, r, label);
    };
    setCoarse(g.ctx);
    g.ctx->ClearState();
    drawNoRate("after ClearState");

    ComPtr<ID3D11CommandList> nothing;
    r.Ok(empty->FinishCommandList(FALSE, &nothing), "FinishCommandList(empty)");
    REQUIRE_OK();
    setCoarse(g.ctx);
    g.ctx->ExecuteCommandList(nothing.Get(), FALSE);
    drawNoRate("after ExecuteCommandList(FALSE)");

    // Deferred: list 1 ends with the rate set, list 2 draws without setting it.
    setCoarse(def.Get());
    ComPtr<ID3D11CommandList> first, second;
    r.Ok(def->FinishCommandList(FALSE, &first), "FinishCommandList(1)");
    def->ClearRenderTargetView(t.rtv.Get(), black);
    bindNoRate(def.Get());
    def->Draw(3, 0);
    def->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
    r.Ok(def->FinishCommandList(FALSE, &second), "FinishCommandList(2)");
    REQUIRE_OK();
    g.ctx->ExecuteCommandList(first.Get(), FALSE);
    g.ctx->ExecuteCommandList(second.Get(), FALSE);
    r.images.push_back(Read2D(g, r, t.texture.Get(), 0, "after FinishCommandList(FALSE)"));
    rig.Count(g, r, "after FinishCommandList(FALSE)");
}

// 4. vrs_native: BeginNative hands the list out at 1x1 (a native draw shades every pixel); the
// context's rate returns after EndNative; a rate native code left behind is not inherited.
void ScenarioNative(Gpu& g, Result& r)
{
    Rig rig;
    if (!rig.Init(g, r))
        return;
    Target a = MakeTarget(g, r, kSize, kSize);
    Target b = MakeTarget(g, r, kSize, kSize);
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();

    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    if (g.layer && rig.caps.on)
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{};
        d.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        root = xrd3d12::RootSignature(g.dev, d, L"layertest vrs empty");
        ID3DBlob* vsCode = Compile(kVrsHlsl, "FullVs", "vs_5_0");
        ID3DBlob* psCode = Compile(kVrsHlsl, "NativePs", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = root;
        p.VS = {vsCode->GetBufferPointer(), vsCode->GetBufferSize()};
        p.PS = {psCode->GetBufferPointer(), psCode->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask = 0xFFFFFFFFu;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = kRgba8;
        p.SampleDesc = {1, 0};
        if (!root || FAILED(xrd3d12::NativeDevice(g.dev)->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso))))
            r.Fail("native pipeline for the 1x1 check not created");
        REQUIRE_OK();
    }

    // Native work on `ctx`: optionally one draw into `target`, then 4x4 left on the list when `leave4x4`.
    const auto native = [&](ID3D11DeviceContext* ctx, const Target* target, bool leave4x4) {
        xrd3d12::NativeScope scope;
        if (!r.Check(xrd3d12::BeginNative(ctx, scope), "BeginNative refused"))
            return;
        UINT touched = 0;
        if (target)
        {
            xrd3d12::RequireView(ctx, target->rtv.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            const D3D12_CPU_DESCRIPTOR_HANDLE handle = xrd3d12::NativeDescriptor(target->rtv.Get());
            scope.list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            const D3D12_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
            scope.list->RSSetViewports(1, &vp);
            scope.list->RSSetScissorRects(1, &scissor);
            scope.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            scope.list->SetGraphicsRootSignature(root);
            scope.list->SetPipelineState(pso);
            scope.list->DrawInstanced(3, 1, 0, 0);
            touched |= xrd3d12::TouchedGraphicsRoot | xrd3d12::TouchedPipeline | xrd3d12::TouchedInputAssembler |
                xrd3d12::TouchedOutputMerger | xrd3d12::TouchedRasterizer;
        }
        if (leave4x4 && scope.list5)
        {
            scope.list5->RSSetShadingRate(D3D12_SHADING_RATE_4X4, nullptr);
            touched |= xrd3d12::TouchedShadingRate;
        }
        xrd3d12::EndNative(ctx, touched);
    };
    const float black[4] = {0, 0, 0, 1};
    Rate coarse;
    coarse.base = k2x2;
    const Rate full;

    // A (context 2x2), B (a native draw in between shades 1x1 and leaves 4x4 on the list), C (the
    // same bindings drawn again: the context's 2x2 is back). The reference draws B first: its
    // bindings would otherwise replace A's.
    g.ctx->ClearRenderTargetView(a.rtv.Get(), black);
    g.ctx->ClearRenderTargetView(b.rtv.Get(), black);
    const bool nativeOn = g.layer && rig.caps.on;
    if (!nativeOn)
    {
        rig.Bind(g, g.ctx, b.rtv.Get(), kSize, kSize, full, full);
        g.ctx->Draw(3, 0);
    }
    rig.Bind(g, g.ctx, a.rtv.Get(), kSize, kSize, coarse, coarse);
    g.ctx->Draw(3, 0);
    if (nativeOn)
        native(g.ctx, &b, true);
    g.ctx->Draw(3, 0);
    rig.Unbind(g, g.ctx);
    rig.Count(g, r, "A + C: context 2x2 around a native section");
    r.images.push_back(Read2D(g, r, a.texture.Get(), 0, "A + C"));
    r.images.push_back(Read2D(g, r, b.texture.Get(), 0, "B: native draw"));

    // D: the context at 1x1, native code leaves 4x4, the same bindings drawn again.
    g.ctx->ClearRenderTargetView(a.rtv.Get(), black);
    rig.Bind(g, g.ctx, a.rtv.Get(), kSize, kSize, full, full);
    if (nativeOn)
        native(g.ctx, nullptr, true);
    g.ctx->Draw(3, 0);
    rig.Unbind(g, g.ctx);
    rig.Count(g, r, "D: context 1x1 after a native 4x4");
    r.images.push_back(Read2D(g, r, a.texture.Get(), 0, "D"));

    // E: a deferred context that never had a rate: native 4x4 is not inherited by its next draw.
    if (nativeOn)
        native(def.Get(), nullptr, true);
    def->ClearRenderTargetView(a.rtv.Get(), black);
    rig.Bind(g, def.Get(), a.rtv.Get(), kSize, kSize, full, full);
    def->Draw(3, 0);
    rig.Unbind(g, def.Get());
    ComPtr<ID3D11CommandList> list;
    r.Ok(def->FinishCommandList(FALSE, &list), "FinishCommandList");
    REQUIRE_OK();
    g.ctx->ExecuteCommandList(list.Get(), FALSE);
    r.images.push_back(Read2D(g, r, a.texture.Get(), 0, "E: deferred draw after a native 4x4"));
    rig.Count(g, r, "E: deferred context after a native 4x4");

    WaitIdle(g);
    if (pso)
        pso->Release();
}

// 5. vrs_hazard: the image bound as a compute UAV, read by the draw, or on a target it does not
// cover: ignored for that draw, which shades 1x1.
void ScenarioHazard(Gpu& g, Result& r)
{
    Rig rig;
    if (!rig.Init(g, r))
        return;
    Target t = MakeTarget(g, r, kSize, kSize);
    Target larger = MakeTarget(g, r, kSize * 2, kSize * 2);
    Target smaller = MakeTarget(g, r, kSize / 2, kSize / 2);
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    auto image = MakeRateImage(g, r, rig.caps, kSize, kSize, [](u32, u32, u32, u32) { return k2x2; }, &srv, &uav);
    REQUIRE_OK();
    Rate rate;
    rate.combiner = 1;
    rate.image = image.Get();
    rate.imageSrv = srv.Get();
    const Rate full;

    // Control: the image works on its own target.
    Draw(g, r, rig, t, rate, rate, "control: image 2x2");

    g.ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    Draw(g, r, rig, t, rate, full, "image bound as a compute UAV");
    ID3D11UnorderedAccessView* none = nullptr;
    g.ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);

    // The layer draws the reference shader with 1x1 constants: it reads the image as an SRV.
    Draw(g, r, rig, t, rate, full, "image read by the draw", true);
    Draw(g, r, rig, larger, rate, full, "larger target");
    Draw(g, r, rig, smaller, rate, full, "smaller target");

    if (g.layer && rig.caps.on)
    {
        r.Check(LayerLogCount("shading rate image bound as an output or read by the draw") > 0,
            "no log line for the image bound as an output");
        r.Check(LayerLogCount("shading rate image does not cover the render target") > 0,
            "no log line for the image on a foreign-size target");
    }
}
} // namespace

const std::vector<Scenario>& VrsScenarios()
{
    static const std::vector<Scenario> list = {
        {"vrs_rates: per-draw, image override, min cap, mixed image", ScenarioRates, false},
        {"vrs_deferred: image on a deferred context written by compute before the list runs", ScenarioDeferred, false},
        {"vrs_clear: ClearState, ExecuteCommandList(FALSE), FinishCommandList(FALSE) reset the rate", ScenarioClear, false},
        {"vrs_native: native sections shade 1x1, the context rate returns, native rates not inherited", ScenarioNative, false},
        {"vrs_hazard: image as an output, as an input, on foreign-size targets is ignored", ScenarioHazard, false},
    };
    return list;
}
} // namespace layertest
