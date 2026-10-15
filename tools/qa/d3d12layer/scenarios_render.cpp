// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Rendering scenarios: clears, textured draws with dynamic buffers, many constant buffer updates,
// blending, depth/stencil, MRT, scissor, texture arrays, cube and volume textures, MSAA,
// tessellation, compute and deferred contexts.
#include "harness.h"

#include <cstring>
#include <string>
#include <thread>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kRtSrv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

// kQuadHlsl followed by `extra`, kept alive for the shader cache (keyed by the source pointer).
const char* WithQuad(std::string& storage, const char* extra)
{
    if (storage.empty())
        storage = std::string(kQuadHlsl) + extra;
    return storage.c_str();
}

struct QuadPipeline
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
};

// QuadVs + the given pixel shader + a dynamic QuadConstants buffer bound to VS and PS b0.
QuadPipeline MakeQuadPipeline(Gpu& g, Result& r, const char* source, const char* ps)
{
    QuadPipeline p;
    p.vs = MakeVs(g, r, source, "QuadVs");
    p.ps = MakePs(g, r, source, ps);
    p.cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    return p;
}

void BindQuadPipeline(Gpu& g, const QuadPipeline& p)
{
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = p.cb.Get();
    g.ctx->VSSetConstantBuffers(0, 1, &cb);
    g.ctx->PSSetConstantBuffers(0, 1, &cb);
}

void DrawQuad(Gpu& g, const QuadPipeline& p, const QuadConstants& q)
{
    Upload(g, p.cb.Get(), &q, sizeof(q));
    g.ctx->Draw(6, 0);
}

ComPtr<ID3D11BlendState> MakeBlend(Gpu& g, Result& r, const D3D11_BLEND_DESC& desc)
{
    ComPtr<ID3D11BlendState> state;
    r.Ok(g.dev->CreateBlendState(&desc, &state), "CreateBlendState");
    return state;
}

ComPtr<ID3D11DepthStencilState> MakeDepth(Gpu& g, Result& r, const D3D11_DEPTH_STENCIL_DESC& desc)
{
    ComPtr<ID3D11DepthStencilState> state;
    r.Ok(g.dev->CreateDepthStencilState(&desc, &state), "CreateDepthStencilState");
    return state;
}

ComPtr<ID3D11RasterizerState> MakeRaster(Gpu& g, Result& r, D3D11_CULL_MODE cull, BOOL scissor, BOOL multisample = FALSE)
{
    D3D11_RASTERIZER_DESC d{};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = cull;
    d.DepthClipEnable = TRUE;
    d.ScissorEnable = scissor;
    d.MultisampleEnable = multisample;
    ComPtr<ID3D11RasterizerState> state;
    r.Ok(g.dev->CreateRasterizerState(&d, &state), "CreateRasterizerState");
    return state;
}

D3D11_DEPTH_STENCIL_DESC DepthDesc(BOOL depth, D3D11_COMPARISON_FUNC func, D3D11_DEPTH_WRITE_MASK write)
{
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = depth;
    d.DepthFunc = func;
    d.DepthWriteMask = write;
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0xFF;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    d.BackFace = d.FrontFace;
    return d;
}
} // namespace

// ------------------------------------------------------------------------------------------------

void ScenarioClearReadback(Gpu& g, Result& r)
{
    auto rgba = MakeTexture2D(g, r, Desc2D(48, 20, kRgba8, kRtSrv));
    auto r32 = MakeTexture2D(g, r, Desc2D(33, 17, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET));
    auto half = MakeTexture2D(g, r, Desc2D(7, 5, DXGI_FORMAT_R16G16B16A16_FLOAT, kRtSrv));
    REQUIRE_OK();
    auto rgbaRtv = MakeRtv(g, r, rgba.Get());
    auto r32Rtv = MakeRtv(g, r, r32.Get());
    auto halfRtv = MakeRtv(g, r, half.Get());
    REQUIRE_OK();
    const float c0[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const float c1[4] = {0.123f, 0, 0, 0};
    const float c2[4] = {-1.5f, 2.25f, 1000.0f, 0.0f};
    g.ctx->ClearRenderTargetView(rgbaRtv.Get(), c0);
    g.ctx->ClearRenderTargetView(r32Rtv.Get(), c1);
    g.ctx->ClearRenderTargetView(halfRtv.Get(), c2);
    r.images.push_back(Read2D(g, r, rgba.Get(), 0, "rgba8 48x20"));
    r.images.push_back(Read2D(g, r, r32.Get(), 0, "r32f 33x17"));
    r.images.push_back(Read2D(g, r, half.Get(), 0, "rgba16f 7x5"));
    // A second clear of the same target after a readback.
    const float c3[4] = {1.0f, 0.0f, 0.5f, 0.25f};
    g.ctx->ClearRenderTargetView(rgbaRtv.Get(), c3);
    r.images.push_back(Read2D(g, r, rgba.Get(), 0, "rgba8 cleared again"));
}

// ------------------------------------------------------------------------------------------------

namespace
{
const char* const kTexturedHlsl = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer Tint : register(b0)
{
    float4 tint;
};
struct VIn
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
};
struct VOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VOut TexVs(VIn v)
{
    VOut o;
    o.pos = float4(v.pos, 0, 1);
    o.uv = v.uv;
    return o;
}
float4 TexPs(VOut i) : SV_Target
{
    return tex.Sample(smp, i.uv) * tint;
}
)";

struct Vertex
{
    float x, y, u, v;
};

// Two triangles over an NDC rectangle with the given UV extent.
void QuadVertices(Vertex* out, float x0, float y0, float x1, float y1, float uvScale)
{
    const Vertex a{x0, y0, 0, 0}, b{x1, y0, uvScale, 0}, c{x0, y1, 0, uvScale}, d{x1, y1, uvScale, uvScale};
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = b;
    out[4] = d;
    out[5] = c;
}

std::vector<std::vector<u8>> MipPattern()
{
    std::vector<std::vector<u8>> mips;
    const u8 solid[4][3] = {{255, 255, 0}, {0, 255, 255}, {255, 0, 255}, {128, 128, 128}};
    for (u32 m = 0, size = 16; m < 5; ++m, size >>= 1)
    {
        std::vector<u8> texels(size * size * 4);
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                u8* t = &texels[(y * size + x) * 4];
                if (!m)
                {
                    const bool checker = ((x / 4) + (y / 4)) & 1;
                    t[0] = u8(x * 16);
                    t[1] = u8(y * 16);
                    t[2] = checker ? 255 : 0;
                }
                else
                {
                    t[0] = solid[m - 1][0];
                    t[1] = solid[m - 1][1];
                    t[2] = solid[m - 1][2];
                }
                t[3] = 255;
            }
        }
        mips.push_back(std::move(texels));
    }
    return mips;
}
} // namespace

void ScenarioTexturedQuad(Gpu& g, Result& r)
{
    const auto mips = MipPattern();
    D3D11_SUBRESOURCE_DATA init[5];
    for (u32 m = 0; m < 5; ++m)
        init[m] = {mips[m].data(), (16u >> m) * 4, 0};
    D3D11_TEXTURE2D_DESC td = Desc2D(16, 16, kRgba8, D3D11_BIND_SHADER_RESOURCE, 5);
    td.Usage = D3D11_USAGE_IMMUTABLE;
    auto tex = MakeTexture2D(g, r, td, init);
    auto rt = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, kRtSrv));
    REQUIRE_OK();
    auto srv = MakeSrv(g, r, tex.Get());
    auto rtv = MakeRtv(g, r, rt.Get());
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP);
    auto vb = MakeBuffer(g, r, 4096, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER);
    auto cb = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto vs = MakeVs(g, r, kTexturedHlsl, "TexVs");
    auto ps = MakePs(g, r, kTexturedHlsl, "TexPs");
    REQUIRE_OK();
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const ShaderCode code = Code(g, kTexturedHlsl, "TexVs", "vs_5_0");
    ComPtr<ID3D11InputLayout> layout;
    r.Ok(g.dev->CreateInputLayout(elements, 2, code.data, code.size, &layout), "CreateInputLayout");
    REQUIRE_OK();

    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), black);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 64, 64);
    g.ctx->IASetInputLayout(layout.Get());
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(Vertex), offset = 0;
    g.ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());

    const auto mapVertices = [&](D3D11_MAP type, u32 first, float x0, float y0, float x1, float y1, float uv) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (!r.Ok(g.ctx->Map(vb.Get(), 0, type, 0, &m), "Map of the vertex buffer"))
            return;
        QuadVertices(static_cast<Vertex*>(m.pData) + first, x0, y0, x1, y1, uv);
        g.ctx->Unmap(vb.Get(), 0);
    };
    const auto tint = [&](float x, float y, float z, float w) {
        const float v[4] = {x, y, z, w};
        Upload(g, cb.Get(), v, sizeof(v));
    };

    // Magnified (mip 0, bilinear), then two minified quads appended with NO_OVERWRITE while the
    // constant buffer is renamed between draws without being bound again.
    mapVertices(D3D11_MAP_WRITE_DISCARD, 0, -1, 1, 0, -1, 1);
    tint(1, 1, 1, 1);
    g.ctx->Draw(6, 0);
    mapVertices(D3D11_MAP_WRITE_NO_OVERWRITE, 6, 0, 1, 1, 0, 4);
    tint(0.5f, 1, 1, 1);
    g.ctx->Draw(6, 6);
    mapVertices(D3D11_MAP_WRITE_NO_OVERWRITE, 12, 0, 0, 1, -1, 8);
    tint(1, 0.5f, 0.5f, 1);
    g.ctx->Draw(6, 12);
    // The buffer wraps: DISCARD renames it; the draws above keep their vertices.
    mapVertices(D3D11_MAP_WRITE_DISCARD, 0, -0.5f, 0.5f, 0.5f, -0.5f, 2);
    tint(1, 1, 0.25f, 1);
    g.ctx->Draw(6, 0);
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "textured quads", 1));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_manySource;
const char* const kManyExtra = R"(
cbuffer Extra : register(b6)
{
    float4 extraTint;
};
float4 ManyPs(VsOut i) : SV_Target
{
    return color * extraTint;
}
)";
} // namespace

void ScenarioManyDraws(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_manySource, kManyExtra);
    auto rt = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, kRtSrv));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "ManyPs");
    auto extra = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    const float clear[4] = {0, 0, 0, 0};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 64, 64);
    BindQuadPipeline(g, p);
    g.ctx->PSSetConstantBuffers(6, 1, extra.GetAddressOf());
    for (u32 i = 0; i < 1024; ++i)
    {
        if (!(i % 256))
        {
            // b6 is past the root CBVs: it goes through the descriptor table.
            const float t[4] = {1.0f - 0.2f * float(i / 256), 1.0f, 0.5f + 0.125f * float(i / 256), 1.0f};
            Upload(g, extra.Get(), t, sizeof(t));
        }
        const float x = float((i % 32) * 2), y = float((i / 32) * 2);
        DrawQuad(g, p, QuadPx(x, y, x + 2, y + 2, 64, 64, float(i % 7) / 6.0f, float(i % 13) / 12.0f, float(i) / 1023.0f, 1));
    }
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "1024 draws"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_blendSource;
const char* const kBlendExtra = R"(
struct Mrt2
{
    float4 a : SV_Target0;
    float4 b : SV_Target1;
};
Mrt2 Blend2Ps(VsOut i)
{
    Mrt2 o;
    o.a = color;
    o.b = color.bgra;
    return o;
}
)";

D3D11_RENDER_TARGET_BLEND_DESC Rt(BOOL enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, D3D11_BLEND srcA,
    D3D11_BLEND dstA, D3D11_BLEND_OP opA, UINT8 mask = D3D11_COLOR_WRITE_ENABLE_ALL)
{
    return {enable, src, dst, op, srcA, dstA, opA, mask};
}
} // namespace

void ScenarioBlendModes(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_blendSource, kBlendExtra);
    auto rt0 = MakeTexture2D(g, r, Desc2D(48, 16, kRgba8, kRtSrv));
    auto rt1 = MakeTexture2D(g, r, Desc2D(48, 16, kRgba8, kRtSrv));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "Blend2Ps");
    REQUIRE_OK();
    ComPtr<ID3D11RenderTargetView> rtv[2] = {MakeRtv(g, r, rt0.Get()), MakeRtv(g, r, rt1.Get())};
    REQUIRE_OK();

    using B = D3D11_BLEND;
    using O = D3D11_BLEND_OP;
    D3D11_RENDER_TARGET_BLEND_DESC modes[6][2] = {
        {Rt(TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD),
            Rt(TRUE, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD)},
        {Rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD),
            Rt(FALSE, B(0), B(0), O(0), B(0), B(0), O(0), D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN)},
        {Rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN),
            Rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX)},
        {Rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX),
            Rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN)},
        {Rt(TRUE, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD),
            Rt(TRUE, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD)},
        {Rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD),
            Rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD)},
    };
    ComPtr<ID3D11BlendState> states[6];
    for (u32 i = 0; i < 6; ++i)
    {
        D3D11_BLEND_DESC d{};
        d.IndependentBlendEnable = TRUE;
        d.RenderTarget[0] = modes[i][0];
        d.RenderTarget[1] = modes[i][1];
        states[i] = MakeBlend(g, r, d);
    }
    // The engine's shared blend (one description for every target).
    D3D11_BLEND_DESC shared{};
    shared.RenderTarget[0] = Rt(TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE,
        D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL);
    auto sharedState = MakeBlend(g, r, shared);
    REQUIRE_OK();

    const float clear0[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    const float clear1[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    g.ctx->ClearRenderTargetView(rtv[0].Get(), clear0);
    g.ctx->ClearRenderTargetView(rtv[1].Get(), clear1);
    ID3D11RenderTargetView* targets[2] = {rtv[0].Get(), rtv[1].Get()};
    g.ctx->OMSetRenderTargets(2, targets, nullptr);
    SetViewport(g, 0, 0, 48, 16);
    BindQuadPipeline(g, p);
    const float factor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    for (u32 i = 0; i < 6; ++i)
    {
        g.ctx->OMSetBlendState(states[i].Get(), factor, 0xffffffff);
        const float fx = float(i * 8);
        DrawQuad(g, p, QuadPx(fx, 0, fx + 8, 12, 48, 16, 0.8f, 0.2f, 0.4f + 0.1f * float(i), 0.5f));
    }
    g.ctx->OMSetBlendState(sharedState.Get(), nullptr, 0xffffffff);
    DrawQuad(g, p, QuadPx(0, 12, 48, 16, 48, 16, 0.3f, 0.3f, 0.3f, 0.5f));
    r.images.push_back(Read2D(g, r, rt0.Get(), 0, "blend RT0", 1));
    r.images.push_back(Read2D(g, r, rt1.Get(), 0, "blend RT1 (independent)", 1));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_depthReadSource;
const char* const kDepthReadExtra = R"(
Texture2D<float> depthTex : register(t0);
float DepthReadPs(VsOut i) : SV_Target
{
    return depthTex.Load(int3(i.pos.xy, 0));
}
)";
} // namespace

void ScenarioDepthStencil(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_depthReadSource, kDepthReadExtra);
    auto rt = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    auto depthRt = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET));
    auto depth = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE));
    QuadPipeline color = MakeQuadPipeline(g, r, source, "ColorPs");
    auto readPs = MakePs(g, r, source, "DepthReadPs");
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    auto depthRtv = MakeRtv(g, r, depthRt.Get());
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    auto dsv = MakeDsv(g, r, depth.Get(), &dd);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    auto depthSrv = MakeSrv(g, r, depth.Get(), &sd);

    D3D11_DEPTH_STENCIL_DESC write = DepthDesc(TRUE, D3D11_COMPARISON_LESS, D3D11_DEPTH_WRITE_MASK_ALL);
    write.StencilEnable = TRUE;
    write.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
    write.BackFace = write.FrontFace;
    D3D11_DEPTH_STENCIL_DESC equal = DepthDesc(FALSE, D3D11_COMPARISON_LESS, D3D11_DEPTH_WRITE_MASK_ZERO);
    equal.StencilEnable = TRUE;
    equal.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
    equal.BackFace = equal.FrontFace;
    D3D11_DEPTH_STENCIL_DESC equalDepth = equal;
    equalDepth.DepthEnable = TRUE;
    equalDepth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    D3D11_DEPTH_STENCIL_DESC back = DepthDesc(FALSE, D3D11_COMPARISON_LESS, D3D11_DEPTH_WRITE_MASK_ZERO);
    back.StencilEnable = TRUE;
    back.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    back.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INVERT, D3D11_COMPARISON_ALWAYS};
    auto dssWrite = MakeDepth(g, r, write);
    auto dssEqual = MakeDepth(g, r, equal);
    auto dssEqualDepth = MakeDepth(g, r, equalDepth);
    auto dssBack = MakeDepth(g, r, back);
    auto noCull = MakeRaster(g, r, D3D11_CULL_NONE, FALSE);
    REQUIRE_OK();

    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), black);
    g.ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    SetViewport(g, 0, 0, 32, 32);
    BindQuadPipeline(g, color);

    g.ctx->OMSetDepthStencilState(dssWrite.Get(), 1);
    DrawQuad(g, color, QuadPx(0, 0, 16, 32, 32, 32, 1, 0, 0, 1, 0.5f));
    // Only the reference value changes: the same state object with ref 2.
    g.ctx->OMSetDepthStencilState(dssWrite.Get(), 2);
    DrawQuad(g, color, QuadPx(0, 0, 32, 16, 32, 32, 0, 0.5f, 0, 1, 0.7f));
    g.ctx->OMSetDepthStencilState(dssEqual.Get(), 2);
    DrawQuad(g, color, QuadPx(0, 0, 32, 32, 32, 32, 0, 0, 1, 1, 0.3f));
    g.ctx->OMSetDepthStencilState(dssEqualDepth.Get(), 1);
    DrawQuad(g, color, QuadPx(0, 0, 32, 32, 32, 32, 1, 1, 1, 1, 0.5f));
    // A back face (mirrored quad) inverts the stencil of the right bottom quarter: 0 -> 255.
    g.ctx->RSSetState(noCull.Get());
    g.ctx->OMSetDepthStencilState(dssBack.Get(), 0);
    QuadConstants mirrored = QuadPx(16, 16, 32, 32, 32, 32, 0, 0, 0, 1, 0.1f);
    mirrored.misc[1] = 1.0f;
    DrawQuad(g, color, mirrored);
    g.ctx->OMSetDepthStencilState(dssEqual.Get(), 255);
    DrawQuad(g, color, QuadPx(0, 0, 32, 32, 32, 32, 1, 1, 0, 1, 0.1f));
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "depth/stencil colour"));

    // The depth buffer through its R24_UNORM_X8 view (the depth-stencil view unbound first).
    g.ctx->OMSetRenderTargets(1, depthRtv.GetAddressOf(), nullptr);
    g.ctx->OMSetDepthStencilState(nullptr, 0);
    g.ctx->RSSetState(nullptr);
    g.ctx->PSSetShader(readPs.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, depthSrv.GetAddressOf());
    DrawQuad(g, color, QuadPx(0, 0, 32, 32, 32, 32, 0, 0, 0, 0, 0));
    r.images.push_back(Read2D(g, r, depthRt.Get(), 0, "depth values"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_mrtSource;
const char* const kMrtExtra = R"(
struct Mrt3
{
    float4 c0 : SV_Target0;
    float c1 : SV_Target1;
    float2 c2 : SV_Target2;
};
Mrt3 Mrt3Ps(VsOut i)
{
    Mrt3 o;
    o.c0 = float4(i.uv, 0.25, 1);
    o.c1 = i.uv.x * 10 + i.uv.y;
    o.c2 = float2(i.uv.y, -i.uv.x);
    return o;
}
)";
} // namespace

void ScenarioMrt3(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_mrtSource, kMrtExtra);
    auto t0 = MakeTexture2D(g, r, Desc2D(24, 16, kRgba8, kRtSrv));
    auto t1 = MakeTexture2D(g, r, Desc2D(24, 16, DXGI_FORMAT_R32_FLOAT, kRtSrv));
    auto t2 = MakeTexture2D(g, r, Desc2D(24, 16, DXGI_FORMAT_R16G16_FLOAT, kRtSrv));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "Mrt3Ps");
    REQUIRE_OK();
    ComPtr<ID3D11RenderTargetView> rtv[3] = {MakeRtv(g, r, t0.Get()), MakeRtv(g, r, t1.Get()), MakeRtv(g, r, t2.Get())};
    REQUIRE_OK();
    const float clear[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    for (auto& v : rtv)
        g.ctx->ClearRenderTargetView(v.Get(), clear);
    // Four slots like the engine's pRT[4]: the last one null.
    ID3D11RenderTargetView* targets[4] = {rtv[0].Get(), rtv[1].Get(), rtv[2].Get(), nullptr};
    g.ctx->OMSetRenderTargets(4, targets, nullptr);
    SetViewport(g, 0, 0, 24, 16);
    BindQuadPipeline(g, p);
    DrawQuad(g, p, QuadPx(2, 1, 22, 15, 24, 16, 0, 0, 0, 0));
    r.images.push_back(Read2D(g, r, t0.Get(), 0, "MRT0 rgba8"));
    r.images.push_back(Read2D(g, r, t1.Get(), 0, "MRT1 r32f"));
    r.images.push_back(Read2D(g, r, t2.Get(), 0, "MRT2 rg16f"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_scissorSource;
const char* const kScissorExtra = R"(
float4 RightHalfPs(VsOut i) : SV_Target
{
    if (i.pos.x < 16)
        discard;
    return color;
}
)";
} // namespace

void ScenarioScissor(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_scissorSource, kScissorExtra);
    auto rt = MakeTexture2D(g, r, Desc2D(32, 24, kRgba8, kRtSrv));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "ColorPs");
    auto rightPs = MakePs(g, r, source, "RightHalfPs");
    auto on = MakeRaster(g, r, D3D11_CULL_BACK, TRUE);
    auto off = MakeRaster(g, r, D3D11_CULL_BACK, FALSE);
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), black);

    const auto setup = [&] {
        g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 32, 24);
        BindQuadPipeline(g, p);
    };
    // Scissoring on without a rect is left out: drivers disagree (NVIDIA clips everything, as the
    // specification says, Intel clips nothing) and the engine always sets a rect with it.
    g.ctx->ClearState();
    setup();
    // With a rect.
    const D3D11_RECT rect{8, 8, 24, 20};
    g.ctx->RSSetScissorRects(1, &rect);
    g.ctx->RSSetState(on.Get());
    DrawQuad(g, p, QuadPx(0, 0, 32, 24, 32, 24, 1, 0, 0, 1));
    // Off again: the rect stays set but no longer clips.
    g.ctx->RSSetState(off.Get());
    g.ctx->PSSetShader(rightPs.Get(), nullptr, 0);
    DrawQuad(g, p, QuadPx(0, 0, 32, 6, 32, 24, 0, 0, 1, 1));
    // On with a second rect.
    g.ctx->RSSetState(on.Get());
    const D3D11_RECT rect2{0, 20, 10, 24};
    g.ctx->RSSetScissorRects(1, &rect2);
    g.ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    DrawQuad(g, p, QuadPx(0, 0, 32, 24, 32, 24, 0, 1, 0, 1));
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "scissor"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_arraySource;
const char* const kArrayExtra = R"(
Texture2DArray<float> depthArray : register(t0);
Texture2D<float> depthPlain : register(t0);
SamplerComparisonState cmp : register(s0);
float ReadArrayPs(VsOut i) : SV_Target
{
    return depthArray.Load(int4(i.pos.xy, 0, 0));
}
float CompareArrayPs(VsOut i) : SV_Target
{
    return depthArray.SampleCmpLevelZero(cmp, float3(i.uv, 0), 0.6);
}
float ReadPlainPs(VsOut i) : SV_Target
{
    return depthPlain.Load(int3(i.pos.xy, 0));
}
)";
} // namespace

// The sun cascade pattern: a 3-slice depth array written per slice, read per slice.
void ScenarioDepthArray(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_arraySource, kArrayExtra);
    auto depth = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 1, 3));
    ComPtr<ID3D11Texture2D> out[5];
    for (auto& t : out)
        t = MakeTexture2D(g, r, Desc2D(32, 32, DXGI_FORMAT_R32_FLOAT, kRtSrv));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "ColorPs");
    auto readArray = MakePs(g, r, source, "ReadArrayPs");
    auto compareArray = MakePs(g, r, source, "CompareArrayPs");
    auto readPlain = MakePs(g, r, source, "ReadPlainPs");
    auto always = MakeDepth(g, r, DepthDesc(TRUE, D3D11_COMPARISON_ALWAYS, D3D11_DEPTH_WRITE_MASK_ALL));
    D3D11_SAMPLER_DESC cd{};
    cd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    cd.AddressU = cd.AddressV = cd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    cd.BorderColor[0] = cd.BorderColor[1] = cd.BorderColor[2] = cd.BorderColor[3] = 1.0f;
    cd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    cd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> cmp;
    r.Ok(g.dev->CreateSamplerState(&cd, &cmp), "CreateSamplerState (comparison)");
    REQUIRE_OK();

    ComPtr<ID3D11DepthStencilView> dsv[3];
    ComPtr<ID3D11ShaderResourceView> srv[3];
    for (u32 i = 0; i < 3; ++i)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray.FirstArraySlice = i;
        dd.Texture2DArray.ArraySize = 1;
        dsv[i] = MakeDsv(g, r, depth.Get(), &dd);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.FirstArraySlice = i;
        sd.Texture2DArray.ArraySize = 1;
        srv[i] = MakeSrv(g, r, depth.Get(), &sd);
    }
    ComPtr<ID3D11RenderTargetView> outRtv[5];
    for (u32 i = 0; i < 5; ++i)
        outRtv[i] = MakeRtv(g, r, out[i].Get());
    REQUIRE_OK();

    // Depth only, with the engine's four null colour slots.
    BindQuadPipeline(g, p);
    g.ctx->PSSetShader(nullptr, nullptr, 0);
    g.ctx->OMSetDepthStencilState(always.Get(), 0);
    SetViewport(g, 0, 0, 32, 32);
    ID3D11RenderTargetView* none[4] = {};
    for (u32 i = 0; i < 3; ++i)
    {
        g.ctx->ClearDepthStencilView(dsv[i].Get(), D3D11_CLEAR_DEPTH, 0.2f + 0.3f * float(i), 0);
        g.ctx->OMSetRenderTargets(4, none, dsv[i].Get());
        const float o = float(4 * i);
        DrawQuad(g, p, QuadPx(4 + o, 4, 20 + o, 28, 32, 32, 0, 0, 0, 0, 0.9f - 0.2f * float(i)));
    }

    // Per-slice views through a Texture2DArray declaration, then a comparison sample of slice 1.
    g.ctx->OMSetDepthStencilState(nullptr, 0);
    for (u32 i = 0; i < 3; ++i)
    {
        g.ctx->OMSetRenderTargets(1, outRtv[i].GetAddressOf(), nullptr);
        g.ctx->PSSetShader(readArray.Get(), nullptr, 0);
        g.ctx->PSSetShaderResources(0, 1, srv[i].GetAddressOf());
        DrawQuad(g, p, QuadPx(0, 0, 32, 32, 32, 32, 0, 0, 0, 0));
    }
    g.ctx->OMSetRenderTargets(1, outRtv[3].GetAddressOf(), nullptr);
    g.ctx->PSSetShader(compareArray.Get(), nullptr, 0);
    g.ctx->PSSetSamplers(0, 1, cmp.GetAddressOf());
    g.ctx->PSSetShaderResources(0, 1, srv[1].GetAddressOf());
    DrawQuad(g, p, QuadPx(0, 0, 32, 32, 32, 32, 0, 0, 0, 0));
    // The slice-0 array view through a Texture2D declaration (D3D11 tolerates the mismatch).
    g.ctx->OMSetRenderTargets(1, outRtv[4].GetAddressOf(), nullptr);
    g.ctx->PSSetShader(readPlain.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, srv[0].GetAddressOf());
    DrawQuad(g, p, QuadPx(0, 0, 32, 32, 32, 32, 0, 0, 0, 0));

    static const char* const labels[5] = {"slice 0 (array view)", "slice 1 (array view)", "slice 2 (array view)",
        "slice 1 comparison", "slice 0 (Texture2D declaration)"};
    for (u32 i = 0; i < 5; ++i)
        r.images.push_back(Read2D(g, r, out[i].Get(), 0, labels[i]));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_cubeSource;
const char* const kCubeExtra = R"(
TextureCube cube : register(t0);
SamplerState pointSampler : register(s0);
float4 CubePs(VsOut i) : SV_Target
{
    static const float3 dirs[8] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1),
        float3(0, 0, -1), float3(0.3, 0.9, -0.2), float3(-0.5, -0.2, 0.7) };
    uint column = uint(i.pos.x) / 4;
    float3 d = dirs[column % 8];
    d.xy += (float2(uint2(i.pos.xy) % 4) - 1.5) * 0.2;
    return cube.SampleLevel(pointSampler, d, 0);
}
)";
} // namespace

void ScenarioCubeSampling(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_cubeSource, kCubeExtra);
    std::vector<u8> faces[6];
    D3D11_SUBRESOURCE_DATA init[6];
    for (u32 f = 0; f < 6; ++f)
    {
        faces[f].resize(4 * 4 * 4);
        for (u32 y = 0; y < 4; ++y)
        {
            for (u32 x = 0; x < 4; ++x)
            {
                u8* t = &faces[f][(y * 4 + x) * 4];
                t[0] = u8(40 * f);
                t[1] = u8(60 * x);
                t[2] = u8(60 * y);
                t[3] = 255;
            }
        }
        init[f] = {faces[f].data(), 16, 0};
    }
    D3D11_TEXTURE2D_DESC cd = Desc2D(4, 4, kRgba8, D3D11_BIND_SHADER_RESOURCE, 1, 6);
    cd.Usage = D3D11_USAGE_IMMUTABLE;
    cd.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    auto cube = MakeTexture2D(g, r, cd, init);
    auto rt = MakeTexture2D(g, r, Desc2D(32, 4, kRgba8, kRtSrv));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "CubePs");
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    REQUIRE_OK();
    auto srv = MakeSrv(g, r, cube.Get());
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 32, 4);
    BindQuadPipeline(g, p);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    DrawQuad(g, p, QuadPx(0, 0, 32, 4, 32, 4, 0, 0, 0, 0));
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "cube faces"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_volumeSource;
const char* const kVolumeExtra = R"(
struct GsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    uint slice : SV_RenderTargetArrayIndex;
};
[maxvertexcount(12)]
void VolumeGs(triangle VsOut input[3], inout TriangleStream<GsOut> stream)
{
    for (uint s = 0; s < 4; ++s)
    {
        for (uint v = 0; v < 3; ++v)
        {
            GsOut o;
            o.pos = input[v].pos;
            o.uv = input[v].uv;
            o.slice = s;
            stream.Append(o);
        }
        stream.RestartStrip();
    }
}
float4 VolumePs(GsOut i) : SV_Target
{
    return float4(i.uv * 0.5, i.slice / 4.0, 1);
}
)";
} // namespace

// The 3D fluid pattern: a TEXTURE3D render target view, slices chosen by the geometry shader.
void ScenarioVolumeRender(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_volumeSource, kVolumeExtra);
    D3D11_TEXTURE3D_DESC vd{};
    vd.Width = 8;
    vd.Height = 8;
    vd.Depth = 4;
    vd.MipLevels = 1;
    vd.Format = kRgba8;
    vd.Usage = D3D11_USAGE_DEFAULT;
    vd.BindFlags = kRtSrv;
    ComPtr<ID3D11Texture3D> volume;
    r.Ok(g.dev->CreateTexture3D(&vd, nullptr, &volume), "CreateTexture3D");
    auto vs = MakeVs(g, r, source, "FullscreenVs");
    auto gs = MakeGs(g, r, source, "VolumeGs");
    auto ps = MakePs(g, r, source, "VolumePs");
    REQUIRE_OK();
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = kRgba8;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
    rd.Texture3D.MipSlice = 0;
    rd.Texture3D.FirstWSlice = 0;
    rd.Texture3D.WSize = 4;
    auto rtv = MakeRtv(g, r, volume.Get(), &rd);
    REQUIRE_OK();
    const float clear[4] = {0.1f, 0.1f, 0.1f, 0.1f};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 8, 8);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->GSSetShader(gs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->Draw(3, 0);
    g.ctx->GSSetShader(nullptr, nullptr, 0);
    r.images.push_back(Read3D(g, r, volume.Get(), "volume 8x8x4"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
const char* const kTriangleHlsl = R"(
cbuffer Tri : register(b0)
{
    float4 p0;
    float4 p1;
    float4 p2;
    float4 triColor;
};
float4 TriVs(uint id : SV_VertexID) : SV_Position
{
    return id == 0 ? p0 : (id == 1 ? p1 : p2);
}
float4 TriPs() : SV_Target
{
    return triColor;
}
Texture2DMS<float4, 4> msTex : register(t0);
float4 SamplesPs(float4 pos : SV_Position) : SV_Target
{
    return msTex.Load(int2(uint(pos.x) % 32, pos.y), uint(pos.x) / 32);
}
)";

struct TriangleConstants
{
    float p[3][4];
    float color[4];
};
} // namespace

void ScenarioMsaaResolve(Gpu& g, Result& r)
{
    UINT levels = 0;
    r.Ok(g.dev->CheckMultisampleQualityLevels(kRgba8, 4, &levels), "CheckMultisampleQualityLevels");
    r.Value("4x quality levels > 0", levels > 0);
    D3D11_TEXTURE2D_DESC md = Desc2D(32, 32, kRgba8, kRtSrv);
    md.SampleDesc = {4, UINT(D3D11_STANDARD_MULTISAMPLE_PATTERN)};
    auto ms = MakeTexture2D(g, r, md);
    auto resolved = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    auto samples = MakeTexture2D(g, r, Desc2D(128, 32, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, kTriangleHlsl, "TriVs");
    auto ps = MakePs(g, r, kTriangleHlsl, "TriPs");
    auto samplesPs = MakePs(g, r, kTriangleHlsl, "SamplesPs");
    auto cb = MakeBuffer(g, r, sizeof(TriangleConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto raster = MakeRaster(g, r, D3D11_CULL_NONE, FALSE, TRUE);
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, ms.Get());
    auto srv = MakeSrv(g, r, ms.Get());
    auto samplesRtv = MakeRtv(g, r, samples.Get());
    REQUIRE_OK();
    const float clear[4] = {0.0f, 0.0f, 0.2f, 1.0f};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 32, 32);
    g.ctx->RSSetState(raster.Get());
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    const TriangleConstants t0{{{-0.9f, -0.7f, 0, 1}, {0.13f, 0.93f, 0, 1}, {0.87f, -0.31f, 0, 1}}, {1, 0.5f, 0, 1}};
    Upload(g, cb.Get(), &t0, sizeof(t0));
    g.ctx->Draw(3, 0);
    // The engine's per-sample passes: only sample 0 of this triangle is written.
    const TriangleConstants t1{{{-0.95f, 0.8f, 0, 1}, {0.9f, 0.6f, 0, 1}, {-0.2f, -0.95f, 0, 1}}, {0, 1, 0.5f, 1}};
    Upload(g, cb.Get(), &t1, sizeof(t1));
    g.ctx->OMSetBlendState(nullptr, nullptr, 0x1);
    g.ctx->Draw(3, 0);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g.ctx->ResolveSubresource(resolved.Get(), 0, ms.Get(), 0, kRgba8);
    r.images.push_back(Read2D(g, r, resolved.Get(), 0, "4x resolve", 1));

    // Every sample through a Texture2DMS declaration.
    g.ctx->RSSetState(nullptr);
    g.ctx->OMSetRenderTargets(1, samplesRtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 128, 32);
    const TriangleConstants full{{{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}}, {0, 0, 0, 0}};
    Upload(g, cb.Get(), &full, sizeof(full));
    g.ctx->PSSetShader(samplesPs.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->Draw(3, 0);
    r.images.push_back(Read2D(g, r, samples.Get(), 0, "4 samples"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
const char* const kTessHlsl = R"(
struct HsIn
{
    float4 pos : CPPOS;
};
HsIn TessVs(uint id : SV_VertexID)
{
    static const float2 p[3] = { float2(-0.9, -0.9), float2(0.0, 0.9), float2(0.9, -0.9) };
    HsIn o;
    o.pos = float4(p[id], 0, 1);
    return o;
}
struct HsConst
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};
HsConst TessConst(InputPatch<HsIn, 3> patch)
{
    HsConst c;
    c.edges[0] = c.edges[1] = c.edges[2] = 3;
    c.inside = 3;
    return c;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("TessConst")]
HsIn TessHs(InputPatch<HsIn, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}
cbuffer TessParams : register(b0)
{
    float4 wobble;
};
struct DsOut
{
    float4 pos : SV_Position;
    float3 bary : TEXCOORD0;
};
[domain("tri")]
DsOut TessDs(HsConst c, float3 bary : SV_DomainLocation, const OutputPatch<HsIn, 3> patch)
{
    DsOut o;
    o.pos = patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;
    o.pos.x += sin(bary.y * 6.2831) * wobble.x;
    o.bary = bary;
    return o;
}
float4 TessPs(DsOut i) : SV_Target
{
    return float4(i.bary, 1);
}
)";
} // namespace

void ScenarioTessellation(Gpu& g, Result& r)
{
    auto rt = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, kTessHlsl, "TessVs");
    auto hs = MakeHs(g, r, kTessHlsl, "TessHs");
    auto ds = MakeDs(g, r, kTessHlsl, "TessDs");
    auto ps = MakePs(g, r, kTessHlsl, "TessPs");
    const float wobble[4] = {0.1f, 0, 0, 0};
    auto cb = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto raster = MakeRaster(g, r, D3D11_CULL_NONE, FALSE);
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    Upload(g, cb.Get(), wobble, sizeof(wobble));
    const float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 32, 32);
    g.ctx->RSSetState(raster.Get());
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->HSSetShader(hs.Get(), nullptr, 0);
    g.ctx->DSSetShader(ds.Get(), nullptr, 0);
    g.ctx->DSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->Draw(3, 0);
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "tessellated triangle", 1));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_lutSource;
const char* const kLutExtra = R"(
Texture1D<float4> lut1 : register(t0);
Texture3D<float2> lut3 : register(t1);
SamplerState pointClamp : register(s0);
float4 LutPs(VsOut i) : SV_Target
{
    float4 a = lut1.SampleLevel(pointClamp, i.uv.x, 0);
    float2 b = lut3.SampleLevel(pointClamp, float3(i.uv, frac(i.uv.x * 3)), 0);
    return float4(a.rg, b);
}
)";
} // namespace

void ScenarioTexture1D3D(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_lutSource, kLutExtra);
    u8 line[8 * 4];
    for (u32 i = 0; i < 8; ++i)
    {
        line[i * 4 + 0] = u8(i * 30);
        line[i * 4 + 1] = u8(255 - i * 30);
        line[i * 4 + 2] = 0;
        line[i * 4 + 3] = 255;
    }
    D3D11_TEXTURE1D_DESC d1{8, 1, 1, kRgba8, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA i1{line, 0, 0};
    ComPtr<ID3D11Texture1D> t1;
    r.Ok(g.dev->CreateTexture1D(&d1, &i1, &t1), "CreateTexture1D");
    u8 volume[4 * 4 * 4 * 2];
    for (u32 i = 0; i < 64; ++i)
    {
        volume[i * 2 + 0] = u8(i * 4);
        volume[i * 2 + 1] = u8(255 - i * 3);
    }
    D3D11_TEXTURE3D_DESC d3{4, 4, 4, 1, DXGI_FORMAT_R8G8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA i3{volume, 8, 32};
    ComPtr<ID3D11Texture3D> t3;
    r.Ok(g.dev->CreateTexture3D(&d3, &i3, &t3), "CreateTexture3D");
    auto rt = MakeTexture2D(g, r, Desc2D(24, 24, kRgba8, kRtSrv));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "LutPs");
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    REQUIRE_OK();
    ID3D11ShaderResourceView* views[2] = {};
    auto s1 = MakeSrv(g, r, t1.Get());
    auto s3 = MakeSrv(g, r, t3.Get());
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    views[0] = s1.Get();
    views[1] = s3.Get();
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 24, 24);
    BindQuadPipeline(g, p);
    g.ctx->PSSetShaderResources(0, 2, views);
    g.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    DrawQuad(g, p, QuadPx(0, 0, 24, 24, 24, 24, 0, 0, 0, 0));
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "1D + 3D lookups"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
const char* const kPingPongHlsl = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
cbuffer Step : register(b0)
{
    float4 step;
};
[numthreads(8, 8, 1)]
void PingPongCs(uint3 id : SV_DispatchThreadID)
{
    float2 a = src[id.xy];
    float2 b = src[uint2((id.x + 1) & 15, id.y)];
    dst[id.xy] = a * 0.5 + b * 0.25 + float2(step.x, id.x * 0.01);
}
)";
} // namespace

// The water ripple pattern: RWTexture2D<float2> ping-pong through UAV/SRV swaps.
void ScenarioComputePingPong(Gpu& g, Result& r)
{
    std::vector<float> initial(16 * 16 * 2);
    for (size_t i = 0; i < initial.size(); ++i)
        initial[i] = float(i % 17) * 0.125f;
    const D3D11_SUBRESOURCE_DATA init{initial.data(), 16 * 8, 0};
    const D3D11_TEXTURE2D_DESC td = Desc2D(16, 16, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11Texture2D> tex[4];
    for (auto& t : tex)
        t = MakeTexture2D(g, r, td, &init);
    auto cs = MakeCs(g, r, kPingPongHlsl, "PingPongCs");
    auto cb = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    ComPtr<ID3D11ShaderResourceView> srv[4];
    ComPtr<ID3D11UnorderedAccessView> uav[4];
    for (u32 i = 0; i < 4; ++i)
    {
        srv[i] = MakeSrv(g, r, tex[i].Get());
        r.Ok(g.dev->CreateUnorderedAccessView(tex[i].Get(), nullptr, &uav[i]), "CreateUnorderedAccessView");
    }
    REQUIRE_OK();
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    g.ctx->CSSetShader(cs.Get(), nullptr, 0);
    g.ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    // The engine's order: bind, dispatch, unbind.
    for (u32 k = 0; k < 5; ++k)
    {
        const float step[4] = {float(k), 0, 0, 0};
        Upload(g, cb.Get(), step, sizeof(step));
        g.ctx->CSSetShaderResources(0, 1, srv[k & 1].GetAddressOf());
        g.ctx->CSSetUnorderedAccessViews(0, 1, uav[(k + 1) & 1].GetAddressOf(), nullptr);
        g.ctx->Dispatch(2, 2, 1);
        g.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        g.ctx->CSSetShaderResources(0, 1, &nullSrv);
    }
    // Swaps without unbinding: binding the SRV of the bound UAV binds null (D3D11 hazard rule).
    for (u32 k = 0; k < 5; ++k)
    {
        const float step[4] = {float(k) + 0.5f, 0, 0, 0};
        Upload(g, cb.Get(), step, sizeof(step));
        g.ctx->CSSetShaderResources(0, 1, srv[2 + (k & 1)].GetAddressOf());
        g.ctx->CSSetUnorderedAccessViews(0, 1, uav[2 + ((k + 1) & 1)].GetAddressOf(), nullptr);
        ComPtr<ID3D11ShaderResourceView> bound;
        g.ctx->CSGetShaderResources(0, 1, &bound);
        r.Value(("naive swap " + std::to_string(k) + ": SRV bound").c_str(), bound != nullptr);
        g.ctx->Dispatch(2, 2, 1);
    }
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    g.ctx->CSSetShader(nullptr, nullptr, 0);
    r.images.push_back(Read2D(g, r, tex[1].Get(), 0, "ping-pong (engine order)"));
    r.images.push_back(Read2D(g, r, tex[0].Get(), 0, "ping-pong source"));
    r.images.push_back(Read2D(g, r, tex[3].Get(), 0, "ping-pong (naive swaps)"));
    r.images.push_back(Read2D(g, r, tex[2].Get(), 0, "ping-pong (naive swaps, other)"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_deferredSource;
const char* const kDeferredExtra = R"(
Texture2DArray<float4> slices : register(t0);
float4 GatherSlicesPs(VsOut i) : SV_Target
{
    uint x = uint(i.pos.x);
    return slices.Load(int4(x % 64, i.pos.y, x / 64, 0));
}
struct VIn
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
};
VsOut StreamVs(VIn v)
{
    VsOut o;
    o.pos = float4(v.pos, 0, 1);
    o.uv = v.uv;
    return o;
}
)";
} // namespace

// The sun cascade pattern: three deferred contexts record on three threads into their own slice
// (constant buffers mapped on the deferred contexts, a stream mapped on the immediate one), the
// immediate context executes the lists in order and samples the result.
void ScenarioDeferredContexts(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_deferredSource, kDeferredExtra);
    auto array = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, kRtSrv, 1, 3));
    auto out = MakeTexture2D(g, r, Desc2D(192, 64, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, source, "QuadVs");
    auto colorPs = MakePs(g, r, source, "ColorPs");
    auto uvPs = MakePs(g, r, source, "UvPs");
    auto streamVs = MakeVs(g, r, source, "StreamVs");
    auto gatherPs = MakePs(g, r, source, "GatherSlicesPs");
    auto stream = MakeBuffer(g, r, 4096, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER);
    auto gatherCb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const ShaderCode code = Code(g, source, "StreamVs", "vs_5_0");
    ComPtr<ID3D11InputLayout> layout;
    r.Ok(g.dev->CreateInputLayout(elements, 2, code.data, code.size, &layout), "CreateInputLayout");
    ComPtr<ID3D11RenderTargetView> sliceRtv[3];
    for (u32 i = 0; i < 3; ++i)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = kRgba8;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.FirstArraySlice = i;
        rd.Texture2DArray.ArraySize = 1;
        sliceRtv[i] = MakeRtv(g, r, array.Get(), &rd);
    }
    auto arraySrv = MakeSrv(g, r, array.Get());
    auto outRtv = MakeRtv(g, r, out.Get());
    ComPtr<ID3D11DeviceContext> deferred[3];
    ComPtr<ID3D11Buffer> cbs[3];
    for (u32 i = 0; i < 3; ++i)
    {
        r.Ok(g.dev->CreateDeferredContext(0, &deferred[i]), "CreateDeferredContext");
        cbs[i] = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    }
    REQUIRE_OK();

    for (u32 frame = 0; frame < 6; ++frame)
    {
        // The stream is mapped on the immediate context before the recording starts.
        D3D11_MAPPED_SUBRESOURCE m{};
        if (!r.Ok(g.ctx->Map(stream.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map of the stream"))
            return;
        Vertex* v = static_cast<Vertex*>(m.pData);
        for (u32 i = 0; i < 3; ++i)
        {
            const float x0 = -0.9f + 0.1f * float(i) + 0.05f * float(frame);
            QuadVertices(v + 6 * i, x0, -0.2f, x0 + 0.5f, -0.9f, 1);
        }
        g.ctx->Unmap(stream.Get(), 0);

        ComPtr<ID3D11CommandList> lists[3];
        HRESULT results[3] = {};
        std::thread threads[3];
        for (u32 i = 0; i < 3; ++i)
        {
            threads[i] = std::thread([&, i] {
                ID3D11DeviceContext* dc = deferred[i].Get();
                const float clear[4] = {0.1f * float(i + 1), 0.2f * float(frame), 0.3f, 1.0f};
                dc->ClearRenderTargetView(sliceRtv[i].Get(), clear);
                dc->OMSetRenderTargets(1, sliceRtv[i].GetAddressOf(), nullptr);
                const D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
                dc->RSSetViewports(1, &vp);
                dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                dc->VSSetShader(vs.Get(), nullptr, 0);
                dc->PSSetShader(colorPs.Get(), nullptr, 0);
                dc->VSSetConstantBuffers(0, 1, cbs[i].GetAddressOf());
                dc->PSSetConstantBuffers(0, 1, cbs[i].GetAddressOf());
                for (u32 k = 0; k < 48; ++k)
                {
                    const float x = float((k % 8) * 8), y = float((k / 8) * 8);
                    const QuadConstants q = QuadPx(x + 1, y + 1, x + 6 + float(i), y + 7, 64, 64, float(k % 5) / 4.0f,
                        float(i) / 2.0f, float(k) / 47.0f, 1);
                    D3D11_MAPPED_SUBRESOURCE cm{};
                    if (SUCCEEDED(dc->Map(cbs[i].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cm)))
                    {
                        std::memcpy(cm.pData, &q, sizeof(q));
                        dc->Unmap(cbs[i].Get(), 0);
                    }
                    dc->Draw(6, 0);
                }
                // A draw from the immediate context's stream.
                const UINT stride = sizeof(Vertex), offset = 0;
                dc->IASetInputLayout(layout.Get());
                dc->IASetVertexBuffers(0, 1, stream.GetAddressOf(), &stride, &offset);
                dc->VSSetShader(streamVs.Get(), nullptr, 0);
                dc->PSSetShader(uvPs.Get(), nullptr, 0);
                dc->Draw(6, 6 * i);
                results[i] = dc->FinishCommandList(FALSE, &lists[i]);
            });
        }
        for (auto& t : threads)
            t.join();
        for (u32 i = 0; i < 3; ++i)
            r.Ok(results[i], "FinishCommandList");
        REQUIRE_OK();
        for (u32 i = 0; i < 3; ++i)
            g.ctx->ExecuteCommandList(lists[i].Get(), FALSE);
        // ExecuteCommandList(FALSE) leaves the immediate context in its default state.
        ComPtr<ID3D11VertexShader> vsAfter;
        g.ctx->VSGetShader(&vsAfter, nullptr, nullptr);
        ComPtr<ID3D11RenderTargetView> rtvAfter;
        g.ctx->OMGetRenderTargets(1, &rtvAfter, nullptr);
        r.Value("state cleared by ExecuteCommandList", !vsAfter && !rtvAfter);

        g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 192, 64);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(gatherPs.Get(), nullptr, 0);
        g.ctx->VSSetConstantBuffers(0, 1, gatherCb.GetAddressOf());
        g.ctx->PSSetShaderResources(0, 1, arraySrv.GetAddressOf());
        const QuadConstants q = QuadPx(0, 0, 192, 64, 192, 64, 0, 0, 0, 0);
        Upload(g, gatherCb.Get(), &q, sizeof(q));
        g.ctx->Draw(6, 0);
        if (frame == 0 || frame == 5)
        {
            const std::string label = "slices sampled, frame " + std::to_string(frame);
            r.images.push_back(Read2D(g, r, out.Get(), 0, label.c_str()));
        }
        g.ctx->ClearState();
        EndFrame(g);
    }
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_churnSource;
const char* const kChurnExtra = R"(
Texture2D tex : register(t0);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
float4 ChurnPs(VsOut i) : SV_Target
{
    float2 outside = float2(2.5, 2.5);
    return 0.5 * (tex.SampleLevel(s0, outside, 0) + tex.SampleLevel(s1, -outside, 0));
}
)";
} // namespace

// More distinct sampler tables than the shader-visible sampler heap holds, in one list: the layer
// continues in a new heap while earlier draws of the open list still reference the old one.
void ScenarioSamplerHeapChurn(Gpu& g, Result& r)
{
    const char* source = WithQuad(g_churnSource, kChurnExtra);
    constexpr u32 kSamplers = 1200;
    auto rt = MakeTexture2D(g, r, Desc2D(40, 30, kRgba8, kRtSrv));
    auto tex = MakeTexture2D(g, r, Desc2D(4, 4, kRgba8, D3D11_BIND_SHADER_RESOURCE));
    QuadPipeline p = MakeQuadPipeline(g, r, source, "ChurnPs");
    REQUIRE_OK();
    auto srv = MakeSrv(g, r, tex.Get());
    auto rtv = MakeRtv(g, r, rt.Get());
    std::vector<ComPtr<ID3D11SamplerState>> samplers(kSamplers);
    for (u32 i = 0; i < kSamplers; ++i)
    {
        D3D11_SAMPLER_DESC d{};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        d.BorderColor[0] = float(i % 256) / 255.0f;
        d.BorderColor[1] = float(i / 256) / 8.0f;
        d.BorderColor[2] = float(i % 3) / 2.0f;
        d.BorderColor[3] = 1.0f;
        d.MaxAnisotropy = 1;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        r.Ok(g.dev->CreateSamplerState(&d, &samplers[i]), "CreateSamplerState");
    }
    REQUIRE_OK();
    const float clear[4] = {0, 0, 0, 0};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 40, 30);
    BindQuadPipeline(g, p);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    for (u32 i = 0; i < kSamplers; ++i)
    {
        ID3D11SamplerState* pair[2] = {samplers[i].Get(), samplers[(i * 7 + 3) % kSamplers].Get()};
        g.ctx->PSSetSamplers(0, 2, pair);
        const float x = float(i % 40), y = float(i / 40);
        DrawQuad(g, p, QuadPx(x, y, x + 1, y + 1, 40, 30, 0, 0, 0, 0));
    }
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "1200 sampler tables", 1));
    g.ctx->ClearState();
}
} // namespace layertest
