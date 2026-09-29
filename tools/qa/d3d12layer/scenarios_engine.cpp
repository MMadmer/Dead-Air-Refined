// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Scenarios built on engine patterns: indexed and instanced draws from static and dynamic buffers,
// dynamic data that lives across frames, objects released while the GPU still uses them, block
// compressed mip chains, format family copies, a large texture upload.
#include "harness.h"

#include <d3d11_1.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kRtSrv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

const char* const kIndexedHlsl = R"(
cbuffer Instances : register(b0)
{
    float4 offsets[4];
};
struct VIn
{
    float2 pos : POSITION;
    float4 color : COLOR;
};
struct VOut
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};
VOut IdxVs(VIn v, uint instance : SV_InstanceID)
{
    VOut o;
    o.pos = float4(v.pos + offsets[instance].xy, 0, 1);
    o.color = v.color;
    return o;
}
float4 IdxPs(VOut i) : SV_Target
{
    return i.color;
}
)";

struct ColorVertex
{
    float x, y;
    float r, g, b, a;
};

// Four corners of a square (top-left, top-right, bottom-left, bottom-right) in NDC.
void Square(ColorVertex* v, float x0, float y0, float size, float r, float g, float b)
{
    v[0] = {x0, y0, r, g, b, 1};
    v[1] = {x0 + size, y0, r, g * 0.5f, b, 1};
    v[2] = {x0, y0 - size, r * 0.5f, g, b, 1};
    v[3] = {x0 + size, y0 - size, r, g, b * 0.5f, 1};
}

ComPtr<ID3D11InputLayout> ColorLayout(Gpu& g, Result& r, const char* source, const char* entry)
{
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const ShaderCode code = Code(g, source, entry, "vs_5_0");
    ComPtr<ID3D11InputLayout> layout;
    r.Ok(g.dev->CreateInputLayout(elements, 2, code.data, code.size, &layout), "CreateInputLayout");
    return layout;
}
} // namespace

// The engine's geometry: static DEFAULT vertex/index buffers created with their data, the 16-bit
// dynamic index stream (DISCARD, then NO_OVERWRITE appends), base vertices, instancing through a
// constant buffer (details), strips with the cut index, lines and points.
void ScenarioIndexedDraws(Gpu& g, Result& r)
{
    ColorVertex vertices[24];
    for (u32 q = 0; q < 6; ++q)
        Square(vertices + q * 4, -0.95f + 0.3f * float(q), 0.95f, 0.25f, 0.2f + 0.15f * float(q), 1.0f - 0.15f * float(q), 0.5f);
    const u16 indices[] = {0, 1, 2, 1, 3, 2, 0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
    auto vb = MakeBuffer(g, r, sizeof(vertices), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, vertices);
    auto ib = MakeBuffer(g, r, sizeof(indices), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, indices);
    auto dynamicIb = MakeBuffer(g, r, 512, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER);
    auto dynamicVb = MakeBuffer(g, r, 4096, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER);
    auto instances = MakeBuffer(g, r, 64, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto rt = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, kIndexedHlsl, "IdxVs");
    auto ps = MakePs(g, r, kIndexedHlsl, "IdxPs");
    auto layout = ColorLayout(g, r, kIndexedHlsl, "IdxVs");
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    const float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 64, 64);
    g.ctx->IASetInputLayout(layout.Get());
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, instances.GetAddressOf());
    const UINT stride = sizeof(ColorVertex), offset = 0;
    g.ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    float offs[16] = {};
    Upload(g, instances.Get(), offs, sizeof(offs));
    // Static index buffer: quad 0, then quad 1 through a base vertex.
    g.ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    g.ctx->DrawIndexed(6, 0, 0);
    g.ctx->DrawIndexed(6, 0, 4);
    // Instanced (details): quad 2 four times, offsets from the constant buffer.
    for (u32 i = 0; i < 4; ++i)
    {
        offs[i * 4 + 0] = 0.0f;
        offs[i * 4 + 1] = -0.3f * float(i + 1);
    }
    Upload(g, instances.Get(), offs, sizeof(offs));
    g.ctx->DrawIndexedInstanced(6, 4, 0, 8, 0);
    std::memset(offs, 0, sizeof(offs));
    Upload(g, instances.Get(), offs, sizeof(offs));
    // Strip with the cut index: two separate squares (quads 3 and 4).
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g.ctx->DrawIndexed(9, 6, 12);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // The dynamic index stream: DISCARD, then an append drawn from its own offset.
    D3D11_MAPPED_SUBRESOURCE m{};
    if (r.Ok(g.ctx->Map(dynamicIb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map of the index stream"))
    {
        const u16 first[6] = {20, 21, 22, 21, 23, 22};
        std::memcpy(m.pData, first, sizeof(first));
        g.ctx->Unmap(dynamicIb.Get(), 0);
    }
    g.ctx->IASetIndexBuffer(dynamicIb.Get(), DXGI_FORMAT_R16_UINT, 0);
    g.ctx->DrawIndexed(6, 0, 0);
    if (r.Ok(g.ctx->Map(dynamicIb.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m), "Map(NO_OVERWRITE) of the index stream"))
    {
        const u16 second[6] = {0, 1, 2, 1, 3, 2};
        std::memcpy(static_cast<u8*>(m.pData) + 12, second, sizeof(second));
        g.ctx->Unmap(dynamicIb.Get(), 0);
    }
    offs[1] = -1.2f;
    Upload(g, instances.Get(), offs, sizeof(offs));
    g.ctx->DrawIndexed(6, 6, 16);
    // Lines and points from the dynamic vertex stream.
    if (r.Ok(g.ctx->Map(dynamicVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map of the vertex stream"))
    {
        ColorVertex* v = static_cast<ColorVertex*>(m.pData);
        for (u32 i = 0; i < 8; ++i)
        {
            v[i * 2] = {-0.9f + 0.2f * float(i), -0.6f, 1, 1, 1, 1};
            v[i * 2 + 1] = {-0.8f + 0.2f * float(i), -0.9f, 1, 0, 1, 1};
        }
        for (u32 i = 0; i < 16; ++i)
            v[16 + i] = {-0.95f + 0.12f * float(i), -0.97f, 0, 1, 1, 1};
        g.ctx->Unmap(dynamicVb.Get(), 0);
    }
    std::memset(offs, 0, sizeof(offs));
    Upload(g, instances.Get(), offs, sizeof(offs));
    g.ctx->IASetVertexBuffers(0, 1, dynamicVb.GetAddressOf(), &stride, &offset);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g.ctx->Draw(16, 0);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.ctx->Draw(16, 16);
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "indexed, instanced, strips, lines, points"));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

namespace
{
const char* const kOffsetHlsl = R"(
cbuffer Offset : register(b0)
{
    float4 offset;
};
struct VIn
{
    float2 pos : POSITION;
    float4 color : COLOR;
};
struct VOut
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};
VOut OffsetVs(VIn v)
{
    VOut o;
    o.pos = float4(v.pos + offset.xy, 0, 1);
    o.color = v.color * offset.z;
    return o;
}
float4 OffsetPs(VOut i) : SV_Target
{
    return i.color;
}
)";
} // namespace

// The dynamic vertex stream appends with NO_OVERWRITE across frames until it wraps; a constant
// buffer is mapped once and read for several frames without being mapped again.
void ScenarioCrossFrameDynamic(Gpu& g, Result& r)
{
    auto stream = MakeBuffer(g, r, 64 * 1024, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER);
    auto cb = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto rt = MakeTexture2D(g, r, Desc2D(64, 32, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, kOffsetHlsl, "OffsetVs");
    auto ps = MakePs(g, r, kOffsetHlsl, "OffsetPs");
    auto layout = ColorLayout(g, r, kOffsetHlsl, "OffsetVs");
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    const float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    const float first[4] = {0, 0, 1, 0};
    Upload(g, cb.Get(), first, sizeof(first));
    const u16 quad[6] = {0, 1, 2, 1, 3, 2};
    for (u32 frame = 0; frame < 6; ++frame)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (!r.Ok(g.ctx->Map(stream.Get(), 0, frame ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m),
                "Map of the vertex stream"))
            return;
        ColorVertex corners[4];
        Square(corners, -0.95f + 0.3f * float(frame), 0.9f, 0.25f, 1.0f - 0.1f * float(frame), 0.2f * float(frame), 0.6f);
        ColorVertex* v = static_cast<ColorVertex*>(m.pData) + frame * 6;
        for (u32 i = 0; i < 6; ++i)
            v[i] = corners[quad[i]];
        g.ctx->Unmap(stream.Get(), 0);

        g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 64, 32);
        g.ctx->IASetInputLayout(layout.Get());
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT stride = sizeof(ColorVertex), offset = 0;
        g.ctx->IASetVertexBuffers(0, 1, stream.GetAddressOf(), &stride, &offset);
        g.ctx->VSSetShader(vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(ps.Get(), nullptr, 0);
        g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        g.ctx->Draw(6, frame * 6);
        if (frame == 5)
        {
            // Vertices written five frames ago, and a new constant buffer version.
            const float moved[4] = {0.0f, -0.9f, 0.5f, 0};
            Upload(g, cb.Get(), moved, sizeof(moved));
            g.ctx->Draw(6, 0);
            g.ctx->Draw(6, 12);
        }
        g.ctx->ClearState();
        EndFrame(g);
    }
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "six frames of appends"));
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_releaseSource;
const char* const kReleaseExtra = R"(
Texture2D tex : register(t0);
SamplerState pointSampler : register(s0);
float4 ReleasePs(VsOut i) : SV_Target
{
    return tex.SampleLevel(pointSampler, i.uv, 0) * color;
}
)";
} // namespace

// D3D11 lets the application release anything right after using it: textures, views, constant
// buffers and render targets referenced by recorded, not yet executed work stay valid.
void ScenarioReleaseInFlight(Gpu& g, Result& r)
{
    if (g_releaseSource.empty())
        g_releaseSource = std::string(kQuadHlsl) + kReleaseExtra;
    const char* source = g_releaseSource.c_str();
    auto out = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, source, "QuadVs");
    auto ps = MakePs(g, r, source, "ReleasePs");
    auto colorPs = MakePs(g, r, source, "ColorPs");
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    REQUIRE_OK();
    auto outRtv = MakeRtv(g, r, out.Get());
    REQUIRE_OK();
    const float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(outRtv.Get(), clear);
    for (u32 round = 0; round < 4; ++round)
    {
        std::vector<u32> texels(8 * 8);
        for (u32 i = 0; i < 64; ++i)
            texels[i] = 0xFF000000u | (i * 4 + round * 60) | ((255 - i * 3) << 8) | ((round * 70) << 16);
        const D3D11_SUBRESOURCE_DATA init{texels.data(), 32, 0};
        D3D11_TEXTURE2D_DESC td = Desc2D(8, 8, kRgba8, D3D11_BIND_SHADER_RESOURCE);
        td.Usage = D3D11_USAGE_IMMUTABLE;
        ComPtr<ID3D11Texture2D> tex = MakeTexture2D(g, r, td, &init);
        ComPtr<ID3D11Texture2D> scratch = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
        ComPtr<ID3D11Buffer> cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
        REQUIRE_OK();
        ComPtr<ID3D11ShaderResourceView> srv = MakeSrv(g, r, tex.Get());
        ComPtr<ID3D11RenderTargetView> scratchRtv = MakeRtv(g, r, scratch.Get());
        REQUIRE_OK();
        // Into a scratch target that is copied out and released before anything is submitted.
        const float scratchColor[4] = {0.1f * float(round), 0.5f, 0.25f, 1};
        g.ctx->ClearRenderTargetView(scratchRtv.Get(), scratchColor);
        g.ctx->OMSetRenderTargets(1, scratchRtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 32, 32);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(ps.Get(), nullptr, 0);
        g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
        g.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        const QuadConstants q = QuadPx(4, 4, 28, 28, 32, 32, 1, 1, 1, 1);
        Upload(g, cb.Get(), &q, sizeof(q));
        g.ctx->Draw(6, 0);
        g.ctx->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
        const D3D11_BOX box{0, 0, 0, 16, 16, 1};
        g.ctx->CopySubresourceRegion(out.Get(), 0, (round & 1) * 16, (round / 2) * 16, 0, scratch.Get(), 0, &box);
        // Everything of this round goes away with the work still unsubmitted.
        g.ctx->ClearState();
        srv.Reset();
        tex.Reset();
        scratchRtv.Reset();
        scratch.Reset();
        cb.Reset();
    }
    g.ctx->Flush();
    r.images.push_back(Read2D(g, r, out.Get(), 0, "outputs of released objects"));
}

// ------------------------------------------------------------------------------------------------

namespace
{
std::string g_bcSource;
const char* const kBcExtra = R"(
Texture2D bc : register(t0);
SamplerState pointClamp : register(s0);
float4 MipsPs(VsOut i) : SV_Target
{
    float level = floor(i.pos.x / 8);
    float2 uv = float2(frac(i.pos.x / 8), i.pos.y / 8);
    return bc.SampleLevel(pointClamp, uv, level);
}
)";

u16 Rgb565(u32 r, u32 g, u32 b)
{
    return u16(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// BC1 (8 bytes a block) or BC3 (16) data of a 32x16 texture with its full mip chain.
std::vector<std::vector<u8>> BlockMips(bool bc3, u32 width, u32 height, u32 mips)
{
    std::vector<std::vector<u8>> out;
    const u32 blockBytes = bc3 ? 16 : 8;
    for (u32 m = 0; m < mips; ++m)
    {
        const u32 w = (std::max)(1u, width >> m), h = (std::max)(1u, height >> m);
        const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
        std::vector<u8> data(size_t(bw) * bh * blockBytes);
        for (u32 b = 0; b < bw * bh; ++b)
        {
            u8* block = data.data() + size_t(b) * blockBytes;
            if (bc3)
            {
                block[0] = u8(255 - m * 30);
                block[1] = u8(m * 20);
                const u64 indices = 0x249249249249ull * ((b + m) % 7 + 1) & 0xFFFFFFFFFFFFull;
                std::memcpy(block + 2, &indices, 6);
                block += 8;
            }
            const u16 c0 = Rgb565(255 - m * 40, (b * 37 + m * 50) & 255, m * 40);
            const u16 c1 = Rgb565(m * 30, 20, 255 - (b * 13 & 255));
            const u16 hi = (std::max)(c0, c1), lo = (std::min)(c0, c1);
            std::memcpy(block, &hi, 2);
            std::memcpy(block + 2, &lo, 2);
            const u32 indices = 0x1B1B1B1Bu ^ (b * 0x9E3779B9u) ^ (m << 5);
            std::memcpy(block + 4, &indices, 4);
        }
        out.push_back(std::move(data));
    }
    return out;
}
} // namespace

// DDS textures: IMMUTABLE BC1/BC3 with full mip chains down to 1x1 (mips smaller than a block).
void ScenarioBlockCompressed(Gpu& g, Result& r)
{
    if (g_bcSource.empty())
        g_bcSource = std::string(kQuadHlsl) + kBcExtra;
    const char* source = g_bcSource.c_str();
    auto vs = MakeVs(g, r, source, "QuadVs");
    auto ps = MakePs(g, r, source, "MipsPs");
    auto cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto sampler = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    REQUIRE_OK();
    for (u32 variant = 0; variant < 2; ++variant)
    {
        const bool bc3 = variant == 1;
        const auto mips = BlockMips(bc3, 32, 16, 6);
        D3D11_SUBRESOURCE_DATA init[6];
        for (u32 m = 0; m < 6; ++m)
        {
            const u32 bw = ((std::max)(1u, 32u >> m) + 3) / 4;
            init[m] = {mips[m].data(), bw * (bc3 ? 16u : 8u), 0};
        }
        D3D11_TEXTURE2D_DESC td = Desc2D(32, 16, bc3 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_BC1_UNORM, D3D11_BIND_SHADER_RESOURCE, 6);
        td.Usage = D3D11_USAGE_IMMUTABLE;
        auto tex = MakeTexture2D(g, r, td, init);
        auto rt = MakeTexture2D(g, r, Desc2D(48, 8, kRgba8, kRtSrv));
        REQUIRE_OK();
        auto srv = MakeSrv(g, r, tex.Get());
        auto rtv = MakeRtv(g, r, rt.Get());
        REQUIRE_OK();
        g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 48, 8);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(ps.Get(), nullptr, 0);
        g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
        g.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        const QuadConstants q = QuadPx(0, 0, 48, 8, 48, 8, 0, 0, 0, 0);
        Upload(g, cb.Get(), &q, sizeof(q));
        g.ctx->Draw(6, 0);
        r.images.push_back(Read2D(g, r, rt.Get(), 0, bc3 ? "BC3 mips 32x16..1x1" : "BC1 mips 32x16..1x1"));
        g.ctx->ClearState();
    }
}

// ------------------------------------------------------------------------------------------------

// Level and cube map shots: the UNORM back buffer copied into an SNORM staging texture of the same
// typeless family (t_ss_async).
void ScenarioFormatFamilyCopy(Gpu& g, Result& r)
{
    auto rt = MakeTexture2D(g, r, Desc2D(20, 10, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, kQuadHlsl, "QuadVs");
    auto ps = MakePs(g, r, kQuadHlsl, "UvPs");
    auto cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    D3D11_TEXTURE2D_DESC sd = Desc2D(20, 10, DXGI_FORMAT_R8G8B8A8_SNORM, 0);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto staging = MakeTexture2D(g, r, sd);
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 20, 10);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    const QuadConstants q = QuadPx(0, 0, 20, 10, 20, 10, 0, 0, 0, 0);
    Upload(g, cb.Get(), &q, sizeof(q));
    g.ctx->Draw(6, 0);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.ctx->CopyResource(staging.Get(), rt.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (!r.Ok(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map of the SNORM staging copy"))
        return;
    Image image;
    image.label = "UNORM target through an SNORM staging copy";
    image.format = DXGI_FORMAT_R8G8B8A8_SNORM;
    image.width = 20;
    image.height = 10;
    image.texelSize = 4;
    image.texels.resize(20 * 10 * 4);
    for (u32 y = 0; y < 10; ++y)
        std::memcpy(image.texels.data() + y * 80, static_cast<const u8*>(m.pData) + size_t(y) * m.RowPitch, 80);
    g.ctx->Unmap(staging.Get(), 0);
    r.images.push_back(std::move(image));
    g.ctx->ClearState();
}

// ------------------------------------------------------------------------------------------------

// A 4096x4096 RGBA8 texture (64 MB of initial data: larger than a creation-time upload page).
void ScenarioLargeTexture(Gpu& g, Result& r)
{
    constexpr u32 kSize = 4096;
    std::vector<u32> texels(size_t(kSize) * kSize);
    for (u32 y = 0; y < kSize; ++y)
    {
        for (u32 x = 0; x < kSize; ++x)
            texels[size_t(y) * kSize + x] = 0xFF000000u | ((x * 7 + y) & 255) | (((x ^ y) & 255) << 8) | (((y * 3) & 255) << 16);
    }
    const D3D11_SUBRESOURCE_DATA init{texels.data(), kSize * 4, 0};
    D3D11_TEXTURE2D_DESC td = Desc2D(kSize, kSize, kRgba8, D3D11_BIND_SHADER_RESOURCE);
    td.Usage = D3D11_USAGE_IMMUTABLE;
    auto big = MakeTexture2D(g, r, td, &init);
    auto window = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_SHADER_RESOURCE));
    REQUIRE_OK();
    const D3D11_BOX box{1000, 3000, 0, 1064, 3064, 1};
    g.ctx->CopySubresourceRegion(window.Get(), 0, 0, 0, 0, big.Get(), 0, &box);
    r.images.push_back(Read2D(g, r, window.Get(), 0, "64x64 window of the 4096x4096 texture"));
    const D3D11_BOX corner{kSize - 64, kSize - 64, 0, kSize, kSize, 1};
    g.ctx->CopySubresourceRegion(window.Get(), 0, 0, 0, 0, big.Get(), 0, &corner);
    r.images.push_back(Read2D(g, r, window.Get(), 0, "last 64x64 corner"));
}
// ------------------------------------------------------------------------------------------------

// The engine's PIX_EVENT scopes: every CBackend queries ID3DUserDefinedAnnotation from its context
// and calls it without a null check. Scopes span submissions and command list execution.
void ScenarioAnnotations(Gpu& g, Result& r)
{
    ComPtr<ID3DUserDefinedAnnotation> immediate;
    r.Value("immediate QueryInterface", g.ctx->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), reinterpret_cast<void**>(immediate.GetAddressOf())));
    ComPtr<ID3D11DeviceContext> deferred;
    r.Ok(g.dev->CreateDeferredContext(0, &deferred), "CreateDeferredContext");
    REQUIRE_OK();
    ComPtr<ID3DUserDefinedAnnotation> recorded;
    r.Value("deferred QueryInterface", deferred->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), reinterpret_cast<void**>(recorded.GetAddressOf())));
    if (!immediate || !recorded)
        return;
    // COM identity: IUnknown of the annotation is the context's.
    ComPtr<IUnknown> a, b;
    immediate->QueryInterface(IID_PPV_ARGS(&a));
    g.ctx->QueryInterface(IID_PPV_ARGS(&b));
    r.Value("same IUnknown", a.Get() == b.Get());

    auto rt = MakeTexture2D(g, r, Desc2D(16, 16, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, kQuadHlsl, "QuadVs");
    auto ps = MakePs(g, r, kQuadHlsl, "ColorPs");
    auto cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto deferredCb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    r.Ok(g.dev->CreateQuery(&qd, &query), "CreateQuery");
    REQUIRE_OK();
    auto rtv = MakeRtv(g, r, rt.Get());
    REQUIRE_OK();
    const auto draw = [&](ID3D11DeviceContext* dc, ID3D11Buffer* buffer, const QuadConstants& q) {
        dc->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0, 0, 16, 16, 0, 1};
        dc->RSSetViewports(1, &vp);
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->VSSetShader(vs.Get(), nullptr, 0);
        dc->PSSetShader(ps.Get(), nullptr, 0);
        dc->VSSetConstantBuffers(0, 1, &buffer);
        dc->PSSetConstantBuffers(0, 1, &buffer);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(dc->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, &q, sizeof(q));
            dc->Unmap(buffer, 0);
        }
        dc->Draw(6, 0);
    };
    const float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);

    r.Value("status", immediate->GetStatus());
    r.Value("begin frame", immediate->BeginEvent(L"frame"));
    r.Value("begin pass", immediate->BeginEvent(L"a pass with a rather long name that is cut to fit the copy"));
    draw(g.ctx, cb.Get(), QuadPx(0, 0, 8, 8, 16, 16, 1, 0, 0, 1));
    immediate->SetMarker(L"marker");
    // A forced submission inside the scopes.
    g.ctx->End(query.Get());
    BOOL done = FALSE;
    while (g.ctx->GetData(query.Get(), &done, sizeof(done), 0) == S_FALSE)
        SwitchToThread();
    draw(g.ctx, cb.Get(), QuadPx(8, 0, 16, 8, 16, 16, 0, 1, 0, 1));
    r.Value("end pass", immediate->EndEvent());

    // A deferred list with scopes of its own, executed inside an immediate scope.
    r.Value("deferred begin", recorded->BeginEvent(L"deferred"));
    draw(deferred.Get(), deferredCb.Get(), QuadPx(0, 8, 8, 16, 16, 16, 0, 0, 1, 1));
    r.Value("deferred end", recorded->EndEvent());
    r.Value("deferred begin, left open", recorded->BeginEvent(L"left open"));
    ComPtr<ID3D11CommandList> list;
    r.Ok(deferred->FinishCommandList(FALSE, &list), "FinishCommandList");
    if (list)
        g.ctx->ExecuteCommandList(list.Get(), FALSE);
    r.Value("deferred end after the list", recorded->EndEvent());
    draw(g.ctx, cb.Get(), QuadPx(8, 8, 16, 16, 16, 16, 1, 1, 0, 1));
    r.Value("end frame", immediate->EndEvent());
    r.Value("unbalanced end", immediate->EndEvent());
    r.images.push_back(Read2D(g, r, rt.Get(), 0, "draws inside the scopes"));
    g.ctx->ClearState();
}
// ------------------------------------------------------------------------------------------------

// The upscaler/frame generation contract (d12_api.h): native D3D12 work inside a native section,
// resources put into states with Require(), foreign heaps and root signature left behind (as SDKs
// do). The app binds nothing again afterwards. On D3D11 the same steps are D3D11 calls.
void ScenarioNativeInterop(Gpu& g, Result& r)
{
    auto source = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    auto copy = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    auto vs = MakeVs(g, r, kQuadHlsl, "QuadVs");
    auto ps = MakePs(g, r, kQuadHlsl, "ColorPs");
    auto cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    auto sourceRtv = MakeRtv(g, r, source.Get());
    auto copyRtv = MakeRtv(g, r, copy.Get());
    REQUIRE_OK();
    const float clear[4] = {0.2f, 0.2f, 0.2f, 1};
    g.ctx->ClearRenderTargetView(sourceRtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, sourceRtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 32, 32);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    QuadConstants q = QuadPx(2, 2, 14, 14, 32, 32, 1, 0, 0, 1);
    Upload(g, cb.Get(), &q, sizeof(q));
    g.ctx->Draw(6, 0);

    const float native[4] = {0, 0.5f, 1, 1};
    if (g.layer)
    {
        ID3D12GraphicsCommandList* list = xrd3d12::BeginNative(g.ctx);
        ID3D12Device* device = xrd3d12::NativeDevice(g.dev);
        ID3D12Resource* src = xrd3d12::NativeResource(source.Get());
        ID3D12Resource* dst = xrd3d12::NativeResource(copy.Get());
        if (!r.Check(list && device && src && dst, "native interop handles missing"))
            return;
        // Foreign state, as an SDK leaves it: its own descriptor heap and root signature.
        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        r.Ok(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_DESCRIPTOR_HEAP_DESC rd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        r.Ok(device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap (RTV)");
        REQUIRE_OK();
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        // Native: copy source -> copy, then clear a corner of the copy through a native RTV.
        xrd3d12::Require(g.ctx, source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        xrd3d12::Require(g.ctx, copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(dst, src);
        xrd3d12::Require(g.ctx, copy.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(dst, nullptr, handle);
        const D3D12_RECT corner{24, 24, 32, 32};
        list->ClearRenderTargetView(handle, native, 1, &corner);
        const D3D12_VIEWPORT tiny{0, 0, 1, 1, 0, 1};
        list->RSSetViewports(1, &tiny);
        xrd3d12::EndNative(g.ctx);
        // Kept alive until the GPU is done with the list that used them.
        xrd3d12::Flush(g.ctx, true);
    }
    else
    {
        g.ctx->CopyResource(copy.Get(), source.Get());
        ComPtr<ID3D11DeviceContext1> context1;
        if (SUCCEEDED(g.ctx->QueryInterface(IID_PPV_ARGS(&context1))))
        {
            const D3D11_RECT corner{24, 24, 32, 32};
            context1->ClearView(copyRtv.Get(), native, &corner, 1);
        }
        else
            r.Fail("ID3D11DeviceContext1 unavailable");
    }

    // The same bindings as before the native section: a second quad into the source.
    q = QuadPx(16, 16, 30, 30, 32, 32, 0, 1, 0, 1);
    Upload(g, cb.Get(), &q, sizeof(q));
    g.ctx->Draw(6, 0);
    // And into the copy (its state was left as RENDER_TARGET by native code).
    g.ctx->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
    q = QuadPx(0, 20, 10, 30, 32, 32, 1, 1, 0, 1);
    Upload(g, cb.Get(), &q, sizeof(q));
    g.ctx->Draw(6, 0);
    r.images.push_back(Read2D(g, r, source.Get(), 0, "source after the native section"));
    r.images.push_back(Read2D(g, r, copy.Get(), 0, "native copy + clear + draw"));
    g.ctx->ClearState();
}
} // namespace layertest
