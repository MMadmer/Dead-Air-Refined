// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `mesh` lane (docs/dead-air/dx12-features/mesh.md 9.1). Scenarios() appends this list.
//
// The layer draws quads with an amplification shader that culls a subset and a mesh shader that
// emits the survivors; the Direct3D 11 reference draws the same survivors with DrawIndexedInstanced.
// Colour and depth must match exactly.
#include "harness.h"

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
constexpr u32 kSize = 64;
constexpr u32 kQuads = 48;       // two amplification groups of 32
constexpr u32 kQuadsPerGroup = 4; // mesh groups per amplification group: several

// Shared by both devices (FXC on the reference, DXC on the layer).
const char* const kCommonHlsl = R"(
struct Quad
{
    float4 rect;  // x0, y0, x1, y1 in NDC
    float4 color;
    float4 misc;  // x = depth, y > 0.5 = culled by the amplification shader
};
StructuredBuffer<Quad> quads : register(t0);
cbuffer Params : register(b0)
{
    uint4 params; // x = quad count, y = quads per mesh group
};
struct VsOut
{
    float4 color : COLOR0;
    float4 pos : SV_Position;
};
VsOut Corner(Quad q, uint corner)
{
    VsOut o;
    float2 uv = float2(corner & 1, corner >> 1);
    o.pos = float4(lerp(q.rect.xy, q.rect.zw, uv), q.misc.x, 1);
    o.color = q.color;
    return o;
}
float4 QuadPs(VsOut i) : SV_Target
{
    return i.color;
}
// The alpha-test pattern of the grass in the shadow maps: the pixel shader writes a target the
// pass does not bind.
float4 ClipPs(VsOut i) : SV_Target
{
    clip(i.color.a - 0.5);
    return i.color;
}
)";

// Reference: the survivors (culled on the CPU) as instances of one indexed quad.
const char* const kReferenceHlsl = R"(
VsOut QuadVs(uint v : SV_VertexID, uint i : SV_InstanceID)
{
    return Corner(quads[i], v);
}
Texture2D<float> depthIn : register(t0);
float4 FullVs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float DepthCopyPs(float4 pos : SV_Position) : SV_Target
{
    return depthIn.Load(int3(pos.xy, 0));
}
)";

// Layer: the amplification shader drops culled quads and compacts the rest in order through a
// group bit mask (no wave intrinsics: any wave size), the mesh shader emits params.y quads per group.
const char* const kMeshHlsl = R"(
struct Payload
{
    uint count;
    uint index[32];
};
groupshared Payload P;
groupshared uint gs_mask;
[numthreads(32, 1, 1)]
void QuadAs(uint tid : SV_GroupThreadID, uint gid : SV_GroupID)
{
    uint i = gid * 32 + tid;
    bool keep = i < params.x;
    if (keep)
        keep = quads[i].misc.y < 0.5;
    if (tid == 0)
        gs_mask = 0;
    GroupMemoryBarrierWithGroupSync();
    if (keep)
        InterlockedOr(gs_mask, 1u << tid);
    GroupMemoryBarrierWithGroupSync();
    uint mask = gs_mask;
    if (keep)
        P.index[countbits(mask & ((1u << tid) - 1u))] = i;
    if (tid == 0)
        P.count = countbits(mask);
    GroupMemoryBarrierWithGroupSync();
    uint ipg = params.y;
    DispatchMesh((countbits(mask) + ipg - 1) / ipg, 1, 1, P);
}
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void QuadMs(uint tid : SV_GroupThreadID, uint gid : SV_GroupID, in payload Payload p,
    out vertices VsOut verts[64], out indices uint3 tris[32])
{
    uint ipg = params.y;
    uint first = gid * ipg;
    uint n = min(ipg, p.count - first);
    SetMeshOutputCounts(n * 4, n * 2);
    for (uint v = tid; v < n * 4; v += 32)
        verts[v] = Corner(quads[p.index[first + (v >> 2)]], v & 3);
    for (uint t = tid; t < n * 2; t += 32)
    {
        uint b = (t >> 1) * 4;
        tris[t] = (t & 1) ? uint3(b + 2, b + 1, b + 3) : uint3(b, b + 1, b + 2);
    }
}
)";

const std::string& ReferenceSource()
{
    static const std::string source = std::string(kCommonHlsl) + kReferenceHlsl;
    return source;
}

const std::string& MeshSource()
{
    static const std::string source = std::string(kCommonHlsl) + kMeshHlsl;
    return source;
}

struct Quad
{
    float rect[4];
    float color[4];
    float misc[4];
};

// Overlapping quads with distinct depths (the result does not depend on the draw order); every
// third one is culled; alpha alternates for the alpha-tested pass.
std::vector<Quad> MakeQuads()
{
    std::vector<Quad> quads(kQuads);
    for (u32 i = 0; i < kQuads; ++i)
    {
        Quad& q = quads[i];
        const float x = float((i * 7) % 48), y = float((i * 11) % 48);
        const float w = 10.0f + float(i % 5) * 3.0f, h = 8.0f + float(i % 4) * 4.0f;
        q.rect[0] = x / kSize * 2 - 1;
        q.rect[1] = 1 - y / kSize * 2;
        q.rect[2] = (x + w) / kSize * 2 - 1;
        q.rect[3] = 1 - (y + h) / kSize * 2;
        q.color[0] = float((i * 37) % 256) / 255.0f;
        q.color[1] = float((i * 91) % 256) / 255.0f;
        q.color[2] = float((i * 53) % 256) / 255.0f;
        q.color[3] = (i % 4) == 3 ? 0.25f : 1.0f;
        q.misc[0] = 0.05f + 0.9f * float((i * 29) % kQuads) / float(kQuads);
        q.misc[1] = (i % 3) == 1 ? 1.0f : 0.0f;
    }
    return quads;
}

std::vector<Quad> Survivors(const std::vector<Quad>& quads)
{
    std::vector<Quad> kept;
    for (const Quad& q : quads)
    {
        if (q.misc[1] < 0.5f)
            kept.push_back(q);
    }
    return kept;
}

ComPtr<ID3D11ShaderResourceView> MakeQuadBuffer(Gpu& g, Result& r, const std::vector<Quad>& quads, ComPtr<ID3D11Buffer>& buffer)
{
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = UINT(quads.size() * sizeof(Quad));
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = sizeof(Quad);
    D3D11_SUBRESOURCE_DATA data{quads.data(), 0, 0};
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!r.Ok(g.dev->CreateBuffer(&d, &data, &buffer), "CreateBuffer (structured)"))
        return srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    v.Buffer.FirstElement = 0;
    v.Buffer.NumElements = UINT(quads.size());
    r.Ok(g.dev->CreateShaderResourceView(buffer.Get(), &v, &srv), "CreateShaderResourceView (structured)");
    return srv;
}

// Everything a draw of the quads needs, for either device.
struct QuadDraw
{
    std::vector<Quad> quads;
    ComPtr<ID3D11Buffer> buffer, params, indices;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11VertexShader> vs; // reference: QuadVs; layer: the mesh shader
    ComPtr<ID3D11GeometryShader> as;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11RasterizerState> raster;
    u32 drawn = 0; // quads the reference draws
};

ComPtr<ID3D11VertexShader> MakeMeshShader(Gpu& g, Result& r)
{
    const std::vector<unsigned char>& code = CompileDxil(MeshSource().c_str(), "QuadMs", "ms_6_5");
    ComPtr<ID3D11VertexShader> ms;
    r.Ok(g.dev->CreateVertexShader(code.data(), code.size(), nullptr, &ms), "CreateVertexShader (mesh shader)");
    return ms;
}

QuadDraw MakeQuadDraw(Gpu& g, Result& r, const char* ps, int depthBias)
{
    QuadDraw d;
    const std::vector<Quad> all = MakeQuads();
    d.quads = g.layer ? all : Survivors(all);
    d.drawn = u32(Survivors(all).size());
    d.srv = MakeQuadBuffer(g, r, d.quads, d.buffer);
    const u32 params[4] = {u32(d.quads.size()), kQuadsPerGroup, 0, 0};
    d.params = MakeBuffer(g, r, 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, params);
    if (g.layer)
    {
        if (!xrd3d12::DxcShaderModel())
        {
            r.Fail("the layer device has no shader compiler (DXC next to the executable?)");
            return d;
        }
        d.vs = MakeMeshShader(g, r);
        const std::vector<unsigned char>& as = CompileDxil(MeshSource().c_str(), "QuadAs", "as_6_5");
        r.Ok(g.dev->CreateGeometryShader(as.data(), as.size(), nullptr, &d.as), "CreateGeometryShader (amplification shader)");
        const std::vector<unsigned char>& code = CompileDxil(MeshSource().c_str(), ps, "ps_6_5");
        r.Ok(g.dev->CreatePixelShader(code.data(), code.size(), nullptr, &d.ps), "CreatePixelShader (DXIL)");
    }
    else
    {
        const u16 index[6] = {0, 1, 2, 2, 1, 3};
        d.indices = MakeBuffer(g, r, sizeof(index), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, index);
        ID3DBlob* vs = Compile(ReferenceSource().c_str(), "QuadVs", "vs_5_0");
        r.Ok(g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d.vs), "CreateVertexShader");
        ID3DBlob* code = Compile(ReferenceSource().c_str(), ps, "ps_5_0");
        r.Ok(g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.ps), "CreatePixelShader");
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.DepthBias = depthBias;
    rd.SlopeScaledDepthBias = depthBias ? 1.5f : 0.0f;
    r.Ok(g.dev->CreateRasterizerState(&rd, &d.raster), "CreateRasterizerState");
    return d;
}

// Records the quads on `ctx` (immediate or deferred): the mesh dispatch on the layer, the
// instanced draw on the reference.
void DrawQuads(Gpu& g, Result& r, ID3D11DeviceContext* ctx, const QuadDraw& d)
{
    ctx->RSSetState(d.raster.Get());
    ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    ctx->PSSetShader(d.ps.Get(), nullptr, 0);
    ctx->VSSetShaderResources(0, 1, d.srv.GetAddressOf());
    ctx->VSSetConstantBuffers(0, 1, d.params.GetAddressOf());
    if (g.layer)
    {
        ctx->GSSetShader(d.as.Get(), nullptr, 0);
        ctx->GSSetShaderResources(0, 1, d.srv.GetAddressOf());
        ctx->GSSetConstantBuffers(0, 1, d.params.GetAddressOf());
        r.Check(xrd3d12::DispatchMesh(ctx, (kQuads + 31) / 32, 1, 1), "DispatchMesh refused the pipeline");
        ctx->GSSetShader(nullptr, nullptr, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->GSSetShaderResources(0, 1, &none);
    }
    else
    {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetIndexBuffer(d.indices.Get(), DXGI_FORMAT_R16_UINT, 0);
        ctx->DrawIndexedInstanced(6, d.drawn, 0, 0, 0);
    }
    ID3D11ShaderResourceView* none = nullptr;
    ctx->VSSetShaderResources(0, 1, &none);
}

// Depth as an R32_FLOAT image (depth formats have no staging comparison): drawn through an SRV.
Image ReadDepth(Gpu& g, Result& r, ID3D11Texture2D* depth, const char* label)
{
    Image image;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    auto srv = MakeSrv(g, r, depth, &sd);
    auto copy = MakeTexture2D(g, r, Desc2D(kSize, kSize, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET));
    if (!r.failures.empty())
        return image;
    auto rtv = MakeRtv(g, r, copy.Get());
    ComPtr<ID3D11VertexShader> vs = MakeVs(g, r, ReferenceSource().c_str(), "FullVs");
    ComPtr<ID3D11PixelShader> ps = MakePs(g, r, ReferenceSource().c_str(), "DepthCopyPs");
    if (!r.failures.empty())
        return image;
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, kSize, kSize);
    g.ctx->RSSetState(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->GSSetShader(nullptr, nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &none);
    return Read2D(g, r, copy.Get(), 0, label);
}

struct Targets
{
    ComPtr<ID3D11Texture2D> color, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
};

Targets MakeTargets(Gpu& g, Result& r, bool color)
{
    Targets t;
    if (color)
    {
        t.color = MakeTexture2D(g, r, Desc2D(kSize, kSize, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
        if (t.color)
            t.rtv = MakeRtv(g, r, t.color.Get());
    }
    t.depth = MakeTexture2D(g, r, Desc2D(kSize, kSize, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE));
    if (t.depth)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        t.dsv = MakeDsv(g, r, t.depth.Get(), &dd);
    }
    return t;
}

void Clear(ID3D11DeviceContext* ctx, const Targets& t)
{
    const float clear[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    if (t.rtv)
        ctx->ClearRenderTargetView(t.rtv.Get(), clear);
    ctx->ClearDepthStencilView(t.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
}

void Bind(ID3D11DeviceContext* ctx, const Targets& t)
{
    ctx->OMSetRenderTargets(t.rtv ? 1 : 0, t.rtv.GetAddressOf(), t.dsv.Get());
    D3D11_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

bool MeshReady(Gpu& g, Result& r)
{
    if (!g.layer)
        return true;
    const xrd3d12::Device* dev = LayerDevice();
    if (!dev || !dev->caps.meshShaderTier || !dev->rootSignatures.mesh)
    {
        r.Fail("the layer device reports no mesh shader support");
        return false;
    }
    return true;
}

// ---- 1. mesh dispatch ----------------------------------------------------------------------------

void ScenarioDispatch(Gpu& g, Result& r)
{
    if (!MeshReady(g, r))
        return;
    QuadDraw d = MakeQuadDraw(g, r, "QuadPs", 0);
    Targets t = MakeTargets(g, r, true);
    REQUIRE_OK();
    Clear(g.ctx, t);
    Bind(g.ctx, t);
    DrawQuads(g, r, g.ctx, d);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.Value("quads drawn", d.drawn);
    r.images.push_back(Read2D(g, r, t.color.Get(), 0, "colour"));
    r.images.push_back(ReadDepth(g, r, t.depth.Get(), "depth"));
    EndFrame(g);
}

// ---- 2. mesh on a deferred context ---------------------------------------------------------------

void ScenarioDeferred(Gpu& g, Result& r)
{
    if (!MeshReady(g, r))
        return;
    QuadDraw d = MakeQuadDraw(g, r, "QuadPs", 0);
    Targets t = MakeTargets(g, r, true);
    ComPtr<ID3D11DeviceContext> deferred;
    r.Ok(g.dev->CreateDeferredContext(0, &deferred), "CreateDeferredContext");
    REQUIRE_OK();
    // The immediate context clears; the deferred list only draws (its targets enter as it finds them).
    Clear(g.ctx, t);
    Bind(deferred.Get(), t);
    DrawQuads(g, r, deferred.Get(), d);
    ComPtr<ID3D11CommandList> list;
    if (!r.Ok(deferred->FinishCommandList(FALSE, &list), "FinishCommandList"))
        return;
    g.ctx->ExecuteCommandList(list.Get(), FALSE);
    r.Value("quads drawn", d.drawn);
    r.images.push_back(Read2D(g, r, t.color.Get(), 0, "colour"));
    r.images.push_back(ReadDepth(g, r, t.depth.Get(), "depth"));
    EndFrame(g);
}

// ---- 3. mesh in a depth-only pass ----------------------------------------------------------------

// The shadow-map pattern: no render target, depth bias, the alpha-tested pixel shader still writes
// its colour output.
void ScenarioDepthOnly(Gpu& g, Result& r)
{
    if (!MeshReady(g, r))
        return;
    QuadDraw d = MakeQuadDraw(g, r, "ClipPs", 150);
    Targets t = MakeTargets(g, r, false);
    REQUIRE_OK();
    Clear(g.ctx, t);
    Bind(g.ctx, t);
    DrawQuads(g, r, g.ctx, d);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.images.push_back(ReadDepth(g, r, t.depth.Get(), "depth (biased, alpha-tested)"));
    EndFrame(g);
}

// ---- 4. refusals -------------------------------------------------------------------------------

// A plain draw with a mesh shader in the vertex slot draws nothing and logs once; a mesh pipeline
// with a DXBC pixel shader is refused and DispatchMesh says so (the engine then draws its vertex path).
void ScenarioRefused(Gpu& g, Result& r)
{
    if (!MeshReady(g, r))
        return;
    QuadDraw d = MakeQuadDraw(g, r, "QuadPs", 0);
    Targets t = MakeTargets(g, r, true);
    ID3DBlob* fxcPs = Compile(ReferenceSource().c_str(), "QuadPs", "ps_5_0");
    ComPtr<ID3D11PixelShader> dxbcPs;
    r.Ok(g.dev->CreatePixelShader(fxcPs->GetBufferPointer(), fxcPs->GetBufferSize(), nullptr, &dxbcPs), "CreatePixelShader (DXBC)");
    REQUIRE_OK();
    Clear(g.ctx, t);
    Bind(g.ctx, t);
    g.ctx->RSSetState(d.raster.Get());
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(d.ps.Get(), nullptr, 0);
    g.ctx->VSSetShaderResources(0, 1, d.srv.GetAddressOf());
    g.ctx->VSSetConstantBuffers(0, 1, d.params.GetAddressOf());
    g.ctx->Draw(6, 0);
    r.Check(LayerLogCount("mesh shader in the vertex shader slot skipped") >= 1, "the refused draw was not logged");

    // Mixed DXIL mesh + DXBC pixel shader: refused by name, DispatchMesh returns false.
    g.ctx->GSSetShader(d.as.Get(), nullptr, 0);
    g.ctx->GSSetShaderResources(0, 1, d.srv.GetAddressOf());
    g.ctx->GSSetConstantBuffers(0, 1, d.params.GetAddressOf());
    g.ctx->PSSetShader(dxbcPs.Get(), nullptr, 0);
    r.Check(!xrd3d12::DispatchMesh(g.ctx, (kQuads + 31) / 32, 1, 1), "DispatchMesh accepted a DXIL/DXBC mix");
    r.Check(LayerLogCount("mixes DXIL") >= 1, "the mixed mesh pipeline was not logged");
    // Out-of-range group counts are refused before anything is recorded.
    g.ctx->PSSetShader(d.ps.Get(), nullptr, 0);
    r.Check(!xrd3d12::DispatchMesh(g.ctx, 70000, 1, 1), "DispatchMesh accepted 70000 groups in x");
    g.ctx->GSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g.ctx->GSSetShaderResources(0, 1, &none);
    g.ctx->VSSetShaderResources(0, 1, &none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Nothing was drawn: the target keeps its clear colour.
    Image color = Read2D(g, r, t.color.Get(), 0, "colour after refused draws");
    const u8 expected[4] = {26, 51, 77, 255};
    bool untouched = color.texels.size() == size_t(kSize) * kSize * 4;
    for (size_t i = 0; untouched && i < color.texels.size(); i += 4)
    {
        for (u32 c = 0; c < 4; ++c)
            untouched = untouched && color.texels[i + c] >= expected[c] - 1 && color.texels[i + c] <= expected[c] + 1;
    }
    r.Check(untouched, "a refused mesh draw reached the target");
    EndFrame(g);
}
} // namespace

const std::vector<Scenario>& MeshScenarios()
{
    static const std::vector<Scenario> list = {
        {"mesh dispatch: amplification culls a subset, mesh shader emits the rest (colour and depth exact)", ScenarioDispatch, false},
        {"mesh on deferred context: recorded, executed, same image", ScenarioDeferred, false},
        {"mesh in depth-only pass: no RTV, depth bias, alpha test (depth exact)", ScenarioDepthOnly, false},
        {"mesh refused: draw with a mesh VS, DXIL/DXBC mix, group limits", ScenarioRefused, true},
    };
    return list;
}
} // namespace layertest
