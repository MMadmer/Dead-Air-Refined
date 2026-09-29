// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `infra` lane for the D3D11-path extensions (docs/dead-air/dx12-features/infra.md
// 9.1 scenarios 10 and 12, gpudriven.md 9.1 scenarios 1-5): buffer SRV/UAV views, per-instance streams,
// indirect draws and their count variants, tracked buffers across submissions, acceleration structure
// views. Scenarios() appends this list.
#include "harness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr u32 kTarget = 64;

// x: seed, y: count, z: flags or a byte offset, w: unused; region: NDC origin (xy) and cell size (zw).
#define LAYERTEST_VIEW_PARAMS                                                                                  \
    "cbuffer ViewParams : register(b0)\n"                                                                      \
    "{\n"                                                                                                      \
    "    uint4 params;\n"                                                                                      \
    "    float4 region;\n"                                                                                     \
    "};\n"                                                                                                     \
    "struct ColorOut\n"                                                                                        \
    "{\n"                                                                                                      \
    "    float4 pos : SV_Position;\n"                                                                          \
    "    float4 color : COLOR0;\n"                                                                             \
    "};\n"

struct ViewParams
{
    u32 params[4];
    float region[4];
};

// Buffer views written by a compute shader and read by a vertex shader.
const char* const kViewsHlsl = LAYERTEST_VIEW_PARAMS R"(
struct Item
{
    float2 pos;
    uint color;
    uint extra;
};
RWByteAddressBuffer rawOut : register(u0);
RWStructuredBuffer<Item> itemsOut : register(u1);
RWBuffer<uint> typedOut : register(u2);
[numthreads(16, 1, 1)]
void FillViewsCs(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= params.y)
        return;
    rawOut.Store4(i * 16, uint4(i * 3 + 1, i * 5 + 2 + params.x, i ^ 0x55u, params.x + i));
    Item it;
    it.pos = region.xy + float2(i % 4, i / 4) * region.zw;
    it.color = (i * 37 + params.x) & 0xFFu;
    it.extra = i * 11;
    itemsOut[i] = it;
    typedOut[i] = i * 7 + 2 + params.x;
}

ByteAddressBuffer rawIn : register(t0);
StructuredBuffer<Item> itemsIn : register(t1);
Buffer<uint> typedIn : register(t2);
Buffer<uint> typedWindow : register(t3);
StructuredBuffer<uint> unbound : register(t4);
static const float2 kCorner[6] = { float2(0, 0), float2(0, 1), float2(1, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
ColorOut ViewsVs(uint id : SV_VertexID)
{
    uint q = id / 6;
    Item it = itemsIn[q];
    float2 cell = float2(q % 4, q / 4);
    float2 origin = q < params.y ? it.pos : region.xy + cell * region.zw;
    ColorOut o;
    o.pos = float4(origin + kCorner[id % 6] * region.zw * 0.75, 0, 1);
    uint r = rawIn.Load(q * 16 + 4) & 0xFFu;
    uint g = typedIn[q] & 0xFFu;
    uint b = it.color & 0xFFu;
    uint a = typedWindow[q % 14] & 0xFFu;
    if (params.z & 1)
        b += unbound[q];
    o.color = float4(r, g, b, a) / 255.0;
    return o;
}
float4 ColorPs(ColorOut i) : SV_Target
{
    return i.color;
}
)";

// Instanced quads: slot 0 per-vertex corners, slot 1 one uint per instance.
const char* const kInstanceHlsl = LAYERTEST_VIEW_PARAMS R"(
struct InstIn
{
    float2 corner : CORNER;
    uint data : INSTDATA;
};
ColorOut InstanceVs(InstIn v, uint iid : SV_InstanceID)
{
    float2 cell = float2(iid % 8, iid / 8);
    ColorOut o;
    o.pos = float4(region.xy + (cell + v.corner * 0.75) * region.zw, 0, 1);
    o.color = float4(v.data & 0xFFu, (v.data >> 8) & 0xFFu, (iid * 29) & 0xFFu, 255) / 255.0;
    return o;
}
ColorOut GridVs(InstIn v)
{
    float2 cell = float2(v.data % 16, (v.data / 16) % 4);
    ColorOut o;
    o.pos = float4(region.xy + (cell + v.corner * 0.75) * region.zw, 0, 1);
    o.color = float4((v.data * 13 + params.x) & 0xFFu, (v.data * 7) & 0xFFu, params.x & 0xFFu, 255) / 255.0;
    return o;
}
float4 ColorPs(ColorOut i) : SV_Target
{
    return i.color;
}
)";

// Compute shaders that write buffers the draws then consume.
const char* const kWriteHlsl = LAYERTEST_VIEW_PARAMS R"(
RWByteAddressBuffer out0 : register(u0);
[numthreads(64, 1, 1)]
void WriteInstCs(uint3 id : SV_DispatchThreadID)
{
    out0.Store(id.x * 4, params.x + id.x * 0x0101u);
}
[numthreads(64, 1, 1)]
void TouchCs(uint3 id : SV_DispatchThreadID)
{
    out0.Store(id.x * 4, out0.Load(id.x * 4) * 2 + params.x + (id.x & 3));
}
)";

const char* const kArgsHlsl = LAYERTEST_VIEW_PARAMS R"(
RWByteAddressBuffer argsOut : register(u0);
RWByteAddressBuffer countOut : register(u1);
// Indexed records (20 bytes): every third one draws nothing.
[numthreads(64, 1, 1)]
void WriteArgsCs(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    uint instances = (i % 3 == 0) ? 0 : 1 + ((i + params.x) & 1);
    argsOut.Store4(i * 20, uint4(6, instances, 0, 0));
    argsOut.Store(i * 20 + 16, i * 2);
    if (i == 0)
        countOut.Store(4, params.y);
}
// Non-indexed records (16 bytes).
[numthreads(64, 1, 1)]
void WriteDrawArgsCs(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    uint instances = (i % 4 == 1) ? 0 : 1 + ((i + params.x) & 1);
    argsOut.Store4(i * 16, uint4(6, instances, 0, i * 2));
    if (i == 0)
        countOut.Store(0, params.y);
}
// One dispatch record at byte offset params.z.
[numthreads(1, 1, 1)]
void DispatchArgsCs()
{
    argsOut.Store3(params.z, uint3(params.x, 1, 1));
}
)";

const char* const kFillHlsl = LAYERTEST_VIEW_PARAMS R"(
RWBuffer<uint> filled : register(u0);
[numthreads(8, 1, 1)]
void IndirectFillCs(uint3 id : SV_DispatchThreadID)
{
    filled[id.x] = id.x * 3 + params.x;
}
ByteAddressBuffer src : register(t0);
RWBuffer<uint> dst : register(u1);
[numthreads(64, 1, 1)]
void CopyRawCs(uint3 id : SV_DispatchThreadID)
{
    dst[id.x] = src.Load(id.x * 4) + params.x;
}
)";

// Clockwise in NDC (front faces under the default rasterizer state).
static const float kCorners[12] = {0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
static const u16 kQuadIndices[6] = {0, 1, 2, 3, 4, 5};

ComPtr<ID3D11Buffer> MakeBufferEx(Gpu& g, Result& r, UINT size, D3D11_USAGE usage, UINT bind, UINT misc, UINT stride = 0,
    const void* data = nullptr)
{
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    d.MiscFlags = misc;
    d.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    r.Ok(g.dev->CreateBuffer(&d, data ? &init : nullptr, &buffer), "CreateBuffer");
    return buffer;
}

ComPtr<ID3D11ShaderResourceView> BufferSrv(Gpu& g, Result& r, ID3D11Buffer* buffer, DXGI_FORMAT format, UINT first, UINT count,
    bool raw)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    d.BufferEx.FirstElement = first;
    d.BufferEx.NumElements = count;
    d.BufferEx.Flags = raw ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0;
    ComPtr<ID3D11ShaderResourceView> view;
    r.Ok(g.dev->CreateShaderResourceView(buffer, &d, &view), "CreateShaderResourceView(buffer)");
    return view;
}

ComPtr<ID3D11UnorderedAccessView> BufferUav(Gpu& g, Result& r, ID3D11Buffer* buffer, DXGI_FORMAT format, UINT first, UINT count,
    bool raw)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    d.Buffer.FirstElement = first;
    d.Buffer.NumElements = count;
    d.Buffer.Flags = raw ? D3D11_BUFFER_UAV_FLAG_RAW : 0;
    ComPtr<ID3D11UnorderedAccessView> view;
    r.Ok(g.dev->CreateUnorderedAccessView(buffer, &d, &view), "CreateUnorderedAccessView(buffer)");
    return view;
}

// The first `words` 32-bit words of a buffer as a one-row R32_UINT image.
Image ReadWords(Gpu& g, Result& r, ID3D11Buffer* buffer, u32 words, const char* label)
{
    Image image;
    image.label = label;
    image.format = DXGI_FORMAT_R32_UINT;
    image.width = words;
    image.height = 1;
    image.texelSize = 4;
    if (!buffer)
    {
        r.Fail("%s: no buffer to read", label);
        return image;
    }
    D3D11_BUFFER_DESC d{};
    buffer->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    d.StructureByteStride = 0;
    ComPtr<ID3D11Buffer> staging;
    if (!r.Ok(g.dev->CreateBuffer(&d, nullptr, &staging), "staging CreateBuffer"))
        return image;
    g.ctx->CopyResource(staging.Get(), buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!r.Ok(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map of a staging buffer"))
        return image;
    image.texels.resize(size_t(words) * 4);
    std::memcpy(image.texels.data(), mapped.pData, (std::min)(size_t(words) * 4, size_t(d.ByteWidth)));
    g.ctx->Unmap(staging.Get(), 0);
    return image;
}

// The constants of one call: params x/y/z, a grid origin and cell size in NDC (y up).
ViewParams Params(u32 x, u32 y, u32 z, float originX, float originY, float cellX, float cellY)
{
    ViewParams p{};
    p.params[0] = x;
    p.params[1] = y;
    p.params[2] = z;
    p.region[0] = originX;
    p.region[1] = originY;
    p.region[2] = cellX;
    p.region[3] = cellY;
    return p;
}

// A 64x64 RGBA8 render target, cleared to opaque black on `ctx` (BindTarget binds it).
struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
};

Target MakeTarget(Gpu& g, Result& r, ID3D11DeviceContext* ctx)
{
    Target t;
    t.texture = MakeTexture2D(g, r, Desc2D(kTarget, kTarget, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    if (t.texture)
        t.rtv = MakeRtv(g, r, t.texture.Get());
    if (t.rtv)
    {
        const float black[4] = {0, 0, 0, 1};
        ctx->ClearRenderTargetView(t.rtv.Get(), black);
    }
    return t;
}

void BindTarget(ID3D11DeviceContext* ctx, const Target& t)
{
    ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT vp{0, 0, float(kTarget), float(kTarget), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

void UploadOn(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data, size_t size)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, data, size);
        ctx->Unmap(buffer, 0);
    }
}

// The instanced-quad pipeline of kInstanceHlsl: corners in slot 0, `instances` in slot 1.
struct QuadPipeline
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> corners;
    ComPtr<ID3D11Buffer> indices;
    ComPtr<ID3D11Buffer> cb;
};

QuadPipeline MakeQuadPipeline(Gpu& g, Result& r, const char* vsEntry)
{
    QuadPipeline p;
    p.vs = MakeVs(g, r, kInstanceHlsl, vsEntry);
    p.ps = MakePs(g, r, kInstanceHlsl, "ColorPs");
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"CORNER", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTDATA", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    const ShaderCode code = Code(g, kInstanceHlsl, vsEntry, "vs_5_0");
    r.Ok(g.dev->CreateInputLayout(elements, UINT(std::size(elements)), code.data, code.size, &p.layout), "CreateInputLayout");
    p.corners = MakeBufferEx(g, r, sizeof(kCorners), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, kCorners);
    p.indices = MakeBufferEx(g, r, sizeof(kQuadIndices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, kQuadIndices);
    p.cb = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    return p;
}

void BindQuadPipeline(ID3D11DeviceContext* ctx, const QuadPipeline& p, ID3D11Buffer* instances)
{
    ctx->IASetInputLayout(p.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* vbs[2] = {p.corners.Get(), instances};
    const UINT strides[2] = {8, 4};
    const UINT offsets[2] = {0, 0};
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->IASetIndexBuffer(p.indices.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, p.cb.GetAddressOf());
}

void UnbindCs(ID3D11DeviceContext* ctx)
{
    ID3D11UnorderedAccessView* none[3] = {};
    ctx->CSSetUnorderedAccessViews(0, 3, none, nullptr);
    ID3D11ShaderResourceView* noSrv[1] = {};
    ctx->CSSetShaderResources(0, 1, noSrv);
}

void Dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11Buffer* cb, ID3D11UnorderedAccessView* const* uavs, UINT uavCount,
    UINT groups)
{
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetUnorderedAccessViews(0, uavCount, uavs, nullptr);
    ctx->Dispatch(groups, 1, 1);
    UnbindCs(ctx);
}

// ---- gpudriven 1: buffer views ------------------------------------------------------------------

// Raw, structured and typed UAVs cleared and written by a compute shader, then read by a vertex
// shader as SRVs (one typed view with an element offset, one slot left unbound).
void ScenarioBufferViews(Gpu& g, Result& r)
{
    constexpr UINT kItems = 16;
    auto raw = MakeBufferEx(g, r, kItems * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
    auto items = MakeBufferEx(g, r, kItems * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
    auto typed = MakeBufferEx(g, r, kItems * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0);
    auto cb = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    auto rawUav = BufferUav(g, r, raw.Get(), DXGI_FORMAT_R32_TYPELESS, 0, kItems * 4, true);
    auto itemsUav = BufferUav(g, r, items.Get(), DXGI_FORMAT_UNKNOWN, 0, kItems, false);
    auto typedUav = BufferUav(g, r, typed.Get(), DXGI_FORMAT_R32_UINT, 0, kItems, false);
    auto rawSrv = BufferSrv(g, r, raw.Get(), DXGI_FORMAT_R32_TYPELESS, 0, kItems * 4, true);
    auto itemsSrv = BufferSrv(g, r, items.Get(), DXGI_FORMAT_UNKNOWN, 0, kItems, false);
    auto typedSrv = BufferSrv(g, r, typed.Get(), DXGI_FORMAT_R32_UINT, 0, kItems, false);
    auto windowSrv = BufferSrv(g, r, typed.Get(), DXGI_FORMAT_R32_UINT, 2, kItems - 2, false);
    // A structured view from a null description covers the whole buffer.
    ComPtr<ID3D11ShaderResourceView> wholeSrv;
    r.Ok(g.dev->CreateShaderResourceView(items.Get(), nullptr, &wholeSrv), "CreateShaderResourceView(structured, null desc)");
    auto fill = MakeCs(g, r, kViewsHlsl, "FillViewsCs");
    auto vs = MakeVs(g, r, kViewsHlsl, "ViewsVs");
    auto ps = MakePs(g, r, kViewsHlsl, "ColorPs");
    Target target = MakeTarget(g, r, g.ctx);
    REQUIRE_OK();

    // Clears: raw and structured views take the first value, typed ones theirs.
    const UINT rawClear[4] = {0xABCDu, 1, 2, 3};
    const UINT itemsClear[4] = {9, 1, 2, 3};
    const UINT typedClear[4] = {5, 1, 2, 3};
    g.ctx->ClearUnorderedAccessViewUint(rawUav.Get(), rawClear);
    g.ctx->ClearUnorderedAccessViewUint(itemsUav.Get(), itemsClear);
    g.ctx->ClearUnorderedAccessViewUint(typedUav.Get(), typedClear);

    const ViewParams p = Params(0x30, 12, 0, -1.0f, -1.0f, 0.5f, 0.5f);
    Upload(g, cb.Get(), &p, sizeof(p));
    ID3D11UnorderedAccessView* uavs[3] = {rawUav.Get(), itemsUav.Get(), typedUav.Get()};
    Dispatch(g.ctx, fill.Get(), cb.Get(), uavs, 3, 1);

    BindTarget(g.ctx, target);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ID3D11ShaderResourceView* srvs[4] = {rawSrv.Get(), itemsSrv.Get(), typedSrv.Get(), windowSrv.Get()};
    g.ctx->VSSetShaderResources(0, 4, srvs);
    g.ctx->Draw(kItems * 6, 0);
    // The whole-buffer view in slot 1 draws the same quads again.
    g.ctx->VSSetShaderResources(1, 1, wholeSrv.GetAddressOf());
    g.ctx->Draw(kItems * 6, 0);
    ID3D11ShaderResourceView* none[4] = {};
    g.ctx->VSSetShaderResources(0, 4, none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    r.images.push_back(Read2D(g, r, target.texture.Get(), 0, "quads from raw/structured/typed buffer SRVs"));
    r.images.push_back(ReadWords(g, r, raw.Get(), kItems * 4, "raw buffer (cleared + written)"));
    r.images.push_back(ReadWords(g, r, items.Get(), kItems * 4, "structured buffer (cleared + written)"));
    r.images.push_back(ReadWords(g, r, typed.Get(), kItems, "typed buffer (cleared + written)"));
}

// Creation rules the layer shares with D3D11 and the refusals of its own (dynamic buffers rename,
// append/counter UAVs are not supported).
void ScenarioBufferViewRules(Gpu& g, Result& r)
{
    D3D11_BUFFER_DESC bad{};
    bad.ByteWidth = 256;
    bad.Usage = D3D11_USAGE_DEFAULT;
    bad.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bad.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    bad.StructureByteStride = 16;
    ComPtr<ID3D11Buffer> refused;
    r.Check(FAILED(g.dev->CreateBuffer(&bad, nullptr, &refused)), "structured + raw buffer accepted");
    bad.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bad.StructureByteStride = 6;
    r.Check(FAILED(g.dev->CreateBuffer(&bad, nullptr, &refused)), "structured buffer with a stride of 6 accepted");

    auto structured = MakeBufferEx(g, r, 256, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
    auto plain = MakeBufferEx(g, r, 256, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0);
    auto dynamic = MakeBufferEx(g, r, 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, 0);
    REQUIRE_OK();
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = DXGI_FORMAT_R32_TYPELESS;
    s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    s.BufferEx.NumElements = 64;
    s.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    r.Check(FAILED(g.dev->CreateShaderResourceView(plain.Get(), &s, &srv)), "raw view of a buffer without raw views accepted");
    s.Format = DXGI_FORMAT_R32_UINT;
    s.BufferEx.Flags = 0;
    s.BufferEx.NumElements = 65;
    r.Check(FAILED(g.dev->CreateShaderResourceView(plain.Get(), &s, &srv)), "view beyond the buffer accepted");
    r.Check(FAILED(g.dev->CreateShaderResourceView(plain.Get(), nullptr, &srv)), "null description of a typed buffer accepted");
    s.BufferEx.NumElements = 16;
    s.Format = DXGI_FORMAT_R32_UINT;
    r.Check(FAILED(g.dev->CreateShaderResourceView(structured.Get(), &s, &srv)), "typed view of a structured buffer accepted");
    s.Format = DXGI_FORMAT_R32_UINT;
    s.BufferEx.NumElements = 64;
    r.Check(FAILED(g.dev->CreateShaderResourceView(dynamic.Get(), &s, &srv)), "view of a dynamic buffer accepted");
    r.Check(LayerLogCount("of a dynamic buffer refused") >= 1, "no log line for the dynamic buffer view");

    ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_UNKNOWN;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = 16;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
    r.Check(FAILED(g.dev->CreateUnorderedAccessView(structured.Get(), &u, &uav)), "counter UAV accepted");
    r.Check(LayerLogCount("append/counter buffer UAVs are not supported") >= 1, "no log line for the counter UAV");
    u.Buffer.Flags = 0;
    r.Check(SUCCEEDED(g.dev->CreateUnorderedAccessView(structured.Get(), &u, &uav)), "structured UAV refused");

    // Buffers with views have their own tracked resource; buffers without stay in pages.
    const u32 words[64] = {};
    auto immutableSrv = MakeBufferEx(g, r, sizeof(words), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, words);
    auto immutableVb = MakeBufferEx(g, r, sizeof(words), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, words);
    REQUIRE_OK();
    xrd3d12::NativeBuffer a, b;
    r.Check(xrd3d12::NativeBufferRange(immutableSrv.Get(), a) && a.tracked && !a.offset, "an immutable SRV buffer is not committed");
    r.Check(xrd3d12::NativeBufferRange(immutableVb.Get(), b) && !b.tracked, "an immutable vertex buffer left the pages");
}

// ---- gpudriven 2: per-instance stream with StartInstanceLocation ------------------------------

void ScenarioInstanceStream(Gpu& g, Result& r)
{
    u32 data[64];
    for (u32 i = 0; i < 64; ++i)
        data[i] = i * 0x0103u + 0x10u;
    auto instances = MakeBufferEx(g, r, sizeof(data), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, data);
    QuadPipeline pipe = MakeQuadPipeline(g, r, "InstanceVs");
    Target target = MakeTarget(g, r, g.ctx);
    REQUIRE_OK();
    BindTarget(g.ctx, target);
    BindQuadPipeline(g.ctx, pipe, instances.Get());
    const ViewParams p = Params(0, 0, 0, -1.0f, -1.0f, 0.25f, 0.25f);
    Upload(g, pipe.cb.Get(), &p, sizeof(p));
    g.ctx->DrawIndexedInstanced(6, 8, 0, 0, 37);
    const ViewParams p2 = Params(0, 0, 0, -1.0f, 0.5f, 0.25f, 0.25f);
    Upload(g, pipe.cb.Get(), &p2, sizeof(p2));
    g.ctx->DrawInstanced(6, 8, 0, 5);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.images.push_back(Read2D(g, r, target.texture.Get(), 0, "instanced quads, start instance 37 and 5"));
}

// ---- gpudriven 3: UAV -> vertex buffer -> UAV --------------------------------------------------

// One buffer written by a compute shader, drawn from as the per-instance stream, written again and
// drawn again: on the immediate context and in a deferred command list.
void UavVertexSteps(ID3D11DeviceContext* ctx, const QuadPipeline& pipe, ID3D11ComputeShader* write, ID3D11Buffer* cb,
    ID3D11Buffer* stream, ID3D11UnorderedAccessView* uav, const Target& target, u32 first)
{
    for (u32 step = 0; step < 2; ++step)
    {
        const u32 row = first + step;
        const ViewParams p = Params(0x20u + row * 0x30u, 0, 0, -1.0f, -1.0f + float(row) * 0.5f, 0.25f, 0.5f);
        UploadOn(ctx, cb, &p, sizeof(p));
        // D3D11 forces an input that is still bound as an output to null: unbind first.
        ID3D11Buffer* noVb = nullptr;
        const UINT zero = 0;
        ctx->IASetVertexBuffers(1, 1, &noVb, &zero, &zero);
        Dispatch(ctx, write, cb, &uav, 1, 1);
        BindTarget(ctx, target);
        BindQuadPipeline(ctx, pipe, stream);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->DrawIndexedInstanced(6, 8, 0, 0, 0);
    }
    ID3D11Buffer* noVb = nullptr;
    const UINT zero = 0;
    ctx->IASetVertexBuffers(1, 1, &noVb, &zero, &zero);
}

void ScenarioUavVertexUav(Gpu& g, Result& r)
{
    auto stream = MakeBufferEx(g, r, 256, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
    auto cb = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto cbDeferred = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    QuadPipeline pipe = MakeQuadPipeline(g, r, "InstanceVs");
    auto write = MakeCs(g, r, kWriteHlsl, "WriteInstCs");
    Target target = MakeTarget(g, r, g.ctx);
    REQUIRE_OK();
    auto uav = BufferUav(g, r, stream.Get(), DXGI_FORMAT_R32_TYPELESS, 0, 64, true);
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();

    UavVertexSteps(g.ctx, pipe, write.Get(), cb.Get(), stream.Get(), uav.Get(), target, 0);
    UavVertexSteps(def.Get(), pipe, write.Get(), cbDeferred.Get(), stream.Get(), uav.Get(), target, 2);
    ComPtr<ID3D11CommandList> list;
    r.Ok(def->FinishCommandList(FALSE, &list), "FinishCommandList");
    REQUIRE_OK();
    g.ctx->ExecuteCommandList(list.Get(), FALSE);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.images.push_back(Read2D(g, r, target.texture.Get(), 0, "UAV-written instance stream, drawn four times"));
    r.images.push_back(ReadWords(g, r, stream.Get(), 64, "instance stream buffer"));
}

// ---- infra 10 and gpudriven 4/5: indirect draws ------------------------------------------------

u32 g_instanceWords[128];

ComPtr<ID3D11Buffer> MakeInstanceIds(Gpu& g, Result& r)
{
    for (u32 i = 0; i < 128; ++i)
        g_instanceWords[i] = i;
    return MakeBufferEx(g, r, sizeof(g_instanceWords), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, g_instanceWords);
}

// Records drawn with the count variant on the layer and one by one on D3D11 (the reference). `count`
// is the value the compute shader stored.
void MultiDraw(Gpu& g, ID3D11DeviceContext* ctx, bool indexed, ID3D11Buffer* args, ID3D11Buffer* countBuffer, UINT countOffset,
    UINT maxCount, UINT count)
{
    if (g.layer)
    {
        if (indexed)
            xrd3d12::DrawIndexedIndirectCount(ctx, args, 0, maxCount, countBuffer, countOffset);
        else
            xrd3d12::DrawInstancedIndirectCount(ctx, args, 0, maxCount, countBuffer, countOffset);
        return;
    }
    const UINT n = countBuffer ? (std::min)(count, maxCount) : maxCount;
    for (UINT i = 0; i < n; ++i)
    {
        if (indexed)
            ctx->DrawIndexedInstancedIndirect(args, i * 20);
        else
            ctx->DrawInstancedIndirect(args, i * 16);
    }
}

struct IndirectBuffers
{
    ComPtr<ID3D11Buffer> args;       // 64 indexed records
    ComPtr<ID3D11Buffer> drawArgs;   // 64 non-indexed records
    ComPtr<ID3D11Buffer> count;      // two counts: [0] non-indexed, [1] indexed
    ComPtr<ID3D11UnorderedAccessView> argsUav, drawArgsUav, countUav;
};

IndirectBuffers MakeIndirectBuffers(Gpu& g, Result& r)
{
    IndirectBuffers b;
    constexpr UINT kMisc = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    b.args = MakeBufferEx(g, r, 64 * 20, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, kMisc);
    b.drawArgs = MakeBufferEx(g, r, 64 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, kMisc);
    b.count = MakeBufferEx(g, r, 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, kMisc);
    if (!r.failures.empty())
        return b;
    b.argsUav = BufferUav(g, r, b.args.Get(), DXGI_FORMAT_R32_TYPELESS, 0, 64 * 5, true);
    b.drawArgsUav = BufferUav(g, r, b.drawArgs.Get(), DXGI_FORMAT_R32_TYPELESS, 0, 64 * 4, true);
    b.countUav = BufferUav(g, r, b.count.Get(), DXGI_FORMAT_R32_TYPELESS, 0, 4, true);
    return b;
}

// Compute writes both record sets and their counts, then both are drawn with the count variants: the
// indexed set into the top half, the non-indexed one into the bottom half.
void CullAndDraw(Gpu& g, ID3D11DeviceContext* ctx, const IndirectBuffers& b, const QuadPipeline& pipe, ID3D11ComputeShader* writeArgs,
    ID3D11ComputeShader* writeDrawArgs, ID3D11Buffer* cb, ID3D11Buffer* ids, const Target& target, u32 seed, UINT indexedCount,
    UINT drawCount, bool withCount)
{
    const ViewParams top = Params(seed, indexedCount, 0, -1.0f, 0.0f, 0.125f, 0.25f);
    UploadOn(ctx, cb, &top, sizeof(top));
    ID3D11UnorderedAccessView* uavs[2] = {b.argsUav.Get(), b.countUav.Get()};
    Dispatch(ctx, writeArgs, cb, uavs, 2, 1);
    BindTarget(ctx, target);
    BindQuadPipeline(ctx, pipe, ids);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    MultiDraw(g, ctx, true, b.args.Get(), withCount ? b.count.Get() : nullptr, 4, 64, indexedCount);

    const ViewParams bottom = Params(seed + 0x40, drawCount, 0, -1.0f, -1.0f, 0.125f, 0.25f);
    UploadOn(ctx, cb, &bottom, sizeof(bottom));
    ID3D11UnorderedAccessView* drawUavs[2] = {b.drawArgsUav.Get(), b.countUav.Get()};
    Dispatch(ctx, writeDrawArgs, cb, drawUavs, 2, 1);
    BindTarget(ctx, target);
    BindQuadPipeline(ctx, pipe, ids);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    MultiDraw(g, ctx, false, b.drawArgs.Get(), withCount ? b.count.Get() : nullptr, 0, 64, drawCount);
}

// gpudriven 4: 64 records incl. zero-instance ones through the count variants against D3D11 drawing
// them one by one; once with a GPU count, once with maxCount only.
void ScenarioMultiDraw(Gpu& g, Result& r)
{
    IndirectBuffers b = MakeIndirectBuffers(g, r);
    auto ids = MakeInstanceIds(g, r);
    auto cb = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    QuadPipeline pipe = MakeQuadPipeline(g, r, "GridVs");
    auto writeArgs = MakeCs(g, r, kArgsHlsl, "WriteArgsCs");
    auto writeDrawArgs = MakeCs(g, r, kArgsHlsl, "WriteDrawArgsCs");
    Target counted = MakeTarget(g, r, g.ctx);
    Target all = MakeTarget(g, r, g.ctx);
    REQUIRE_OK();
    CullAndDraw(g, g.ctx, b, pipe, writeArgs.Get(), writeDrawArgs.Get(), cb.Get(), ids.Get(), counted, 0x11, 50, 37, true);
    CullAndDraw(g, g.ctx, b, pipe, writeArgs.Get(), writeDrawArgs.Get(), cb.Get(), ids.Get(), all, 0x22, 50, 37, false);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    r.images.push_back(Read2D(g, r, counted.texture.Get(), 0, "multi-draw with GPU counts 50 / 37"));
    r.images.push_back(Read2D(g, r, all.texture.Get(), 0, "multi-draw of all 64 records"));
    r.images.push_back(ReadWords(g, r, b.args.Get(), 64 * 5, "indexed records"));
    r.images.push_back(ReadWords(g, r, b.count.Get(), 4, "counts"));
}

// gpudriven 5: the same, recorded in two deferred command lists that reuse the buffers, both executed
// in one frame.
void ScenarioDeferredCullDraw(Gpu& g, Result& r)
{
    IndirectBuffers b = MakeIndirectBuffers(g, r);
    auto ids = MakeInstanceIds(g, r);
    auto cb = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    QuadPipeline pipe = MakeQuadPipeline(g, r, "GridVs");
    auto writeArgs = MakeCs(g, r, kArgsHlsl, "WriteArgsCs");
    auto writeDrawArgs = MakeCs(g, r, kArgsHlsl, "WriteDrawArgsCs");
    Target first = MakeTarget(g, r, g.ctx);
    Target second = MakeTarget(g, r, g.ctx);
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();
    ComPtr<ID3D11CommandList> lists[2];
    const struct
    {
        const Target* target;
        u32 seed;
        UINT indexed;
        UINT draws;
    } runs[2] = {{&first, 0x31, 40, 60}, {&second, 0x52, 20, 9}};
    for (u32 i = 0; i < 2; ++i)
    {
        CullAndDraw(g, def.Get(), b, pipe, writeArgs.Get(), writeDrawArgs.Get(), cb.Get(), ids.Get(), *runs[i].target, runs[i].seed,
            runs[i].indexed, runs[i].draws, true);
        def->OMSetRenderTargets(0, nullptr, nullptr);
        r.Ok(def->FinishCommandList(FALSE, &lists[i]), "FinishCommandList");
    }
    REQUIRE_OK();
    g.ctx->ExecuteCommandList(lists[0].Get(), FALSE);
    g.ctx->ExecuteCommandList(lists[1].Get(), FALSE);
    r.images.push_back(Read2D(g, r, first.texture.Get(), 0, "deferred cull + draw, first list"));
    r.images.push_back(Read2D(g, r, second.texture.Get(), 0, "deferred cull + draw, second list"));
    r.images.push_back(ReadWords(g, r, b.count.Get(), 4, "counts after both lists"));
}

// infra 10: the three D3D11 indirect calls with arguments in a buffer filled by UpdateSubresource
// (tracked), in an immutable page (promoted) and written by a compute shader.
void ScenarioIndirectCalls(Gpu& g, Result& r)
{
    constexpr UINT kMisc = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    auto updated = MakeBufferEx(g, r, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kMisc);
    const u32 drawRecords[8] = {6, 3, 0, 20, /**/ 6, 5, 0, 60};
    auto page = MakeBufferEx(g, r, sizeof(drawRecords), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS,
        0, drawRecords);
    auto dispatchArgs = MakeBufferEx(g, r, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, kMisc);
    auto filled = MakeBufferEx(g, r, 64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0);
    auto ids = MakeInstanceIds(g, r);
    auto cb = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    QuadPipeline pipe = MakeQuadPipeline(g, r, "GridVs");
    auto argsCs = MakeCs(g, r, kArgsHlsl, "DispatchArgsCs");
    auto fillCs = MakeCs(g, r, kFillHlsl, "IndirectFillCs");
    Target target = MakeTarget(g, r, g.ctx);
    REQUIRE_OK();
    auto argsUav = BufferUav(g, r, dispatchArgs.Get(), DXGI_FORMAT_R32_TYPELESS, 0, 16, true);
    auto filledUav = BufferUav(g, r, filled.Get(), DXGI_FORMAT_R32_UINT, 0, 64, false);
    REQUIRE_OK();

    // Indexed: the second of two records (offset 20); the rest of the 64 bytes stays zero.
    const u32 indexedRecords[16] = {6, 2, 0, 0, 0, /**/ 6, 4, 0, 0, 40};
    g.ctx->UpdateSubresource(updated.Get(), 0, nullptr, indexedRecords, 0, 0);
    const ViewParams p = Params(0x44, 0, 0, -1.0f, 0.0f, 0.125f, 0.25f);
    Upload(g, cb.Get(), &p, sizeof(p));
    BindTarget(g.ctx, target);
    BindQuadPipeline(g.ctx, pipe, ids.Get());
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->DrawIndexedInstancedIndirect(updated.Get(), 20);
    // Non-indexed from the immutable page: both records.
    const ViewParams p2 = Params(0x88, 0, 0, -1.0f, -1.0f, 0.125f, 0.25f);
    Upload(g, cb.Get(), &p2, sizeof(p2));
    g.ctx->DrawInstancedIndirect(page.Get(), 0);
    g.ctx->DrawInstancedIndirect(page.Get(), 16);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Dispatch: the arguments (3, 1, 1) at offset 12 come from a compute shader.
    const ViewParams p3 = Params(3, 0, 12, 0, 0, 0, 0);
    Upload(g, cb.Get(), &p3, sizeof(p3));
    ID3D11UnorderedAccessView* argsUavs[1] = {argsUav.Get()};
    Dispatch(g.ctx, argsCs.Get(), cb.Get(), argsUavs, 1, 1);
    const ViewParams p4 = Params(100, 0, 0, 0, 0, 0, 0);
    Upload(g, cb.Get(), &p4, sizeof(p4));
    const UINT zeros[4] = {};
    g.ctx->ClearUnorderedAccessViewUint(filledUav.Get(), zeros);
    g.ctx->CSSetShader(fillCs.Get(), nullptr, 0);
    g.ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->CSSetUnorderedAccessViews(0, 1, filledUav.GetAddressOf(), nullptr);
    g.ctx->DispatchIndirect(dispatchArgs.Get(), 12);
    UnbindCs(g.ctx);

    // DispatchIndirectCount: two records from offset 12, a count of 1 set by UpdateSubresource; D3D11
    // dispatches the first record directly.
    const u32 one[4] = {1, 0, 0, 0};
    auto count = MakeBufferEx(g, r, 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kMisc);
    auto filled2 = MakeBufferEx(g, r, 64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0);
    REQUIRE_OK();
    auto filled2Uav = BufferUav(g, r, filled2.Get(), DXGI_FORMAT_R32_UINT, 0, 64, false);
    REQUIRE_OK();
    g.ctx->UpdateSubresource(count.Get(), 0, nullptr, one, 0, 0);
    g.ctx->ClearUnorderedAccessViewUint(filled2Uav.Get(), zeros);
    const ViewParams p5 = Params(200, 0, 0, 0, 0, 0, 0);
    Upload(g, cb.Get(), &p5, sizeof(p5));
    g.ctx->CSSetShader(fillCs.Get(), nullptr, 0);
    g.ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->CSSetUnorderedAccessViews(0, 1, filled2Uav.GetAddressOf(), nullptr);
    if (g.layer)
        xrd3d12::DispatchIndirectCount(g.ctx, dispatchArgs.Get(), 12, 2, count.Get(), 0);
    else
        g.ctx->DispatchIndirect(dispatchArgs.Get(), 12);
    UnbindCs(g.ctx);

    r.images.push_back(Read2D(g, r, target.texture.Get(), 0, "DrawIndexedInstancedIndirect + DrawInstancedIndirect"));
    r.images.push_back(ReadWords(g, r, filled.Get(), 64, "DispatchIndirect (3, 1, 1) output"));
    r.images.push_back(ReadWords(g, r, dispatchArgs.Get(), 16, "dispatch arguments written by compute"));
    r.images.push_back(ReadWords(g, r, filled2.Get(), 64, "DispatchIndirectCount (count 1 of 2) output"));
}

// ---- tracked buffers across submissions --------------------------------------------------------

// A tracked buffer used as UAV, SRV and indirect arguments with a frame boundary between the uses
// (buffers decay to COMMON at every ExecuteCommandLists), then 40 deferred lists that alternate
// writing and reading it, all in one submission.
void ScenarioTrackedBufferDecay(Gpu& g, Result& r)
{
    constexpr UINT kMisc = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    const u32 zeros[64] = {};
    auto shared = MakeBufferEx(g, r, sizeof(zeros), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, kMisc,
        0, zeros);
    auto out1 = MakeBufferEx(g, r, 64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0);
    auto out2 = MakeBufferEx(g, r, 64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0);
    auto filled = MakeBufferEx(g, r, 64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0);
    auto cb = MakeBuffer(g, r, sizeof(ViewParams), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto touch = MakeCs(g, r, kWriteHlsl, "TouchCs");
    auto copy = MakeCs(g, r, kFillHlsl, "CopyRawCs");
    auto fill = MakeCs(g, r, kFillHlsl, "IndirectFillCs");
    REQUIRE_OK();
    auto sharedUav = BufferUav(g, r, shared.Get(), DXGI_FORMAT_R32_TYPELESS, 0, 64, true);
    auto sharedSrv = BufferSrv(g, r, shared.Get(), DXGI_FORMAT_R32_TYPELESS, 0, 64, true);
    auto out1Uav = BufferUav(g, r, out1.Get(), DXGI_FORMAT_R32_UINT, 0, 64, false);
    auto out2Uav = BufferUav(g, r, out2.Get(), DXGI_FORMAT_R32_UINT, 0, 64, false);
    auto filledUav = BufferUav(g, r, filled.Get(), DXGI_FORMAT_R32_UINT, 0, 64, false);
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();

    const auto touchOn = [&](ID3D11DeviceContext* ctx, u32 add) {
        const ViewParams p = Params(add, 0, 0, 0, 0, 0, 0);
        UploadOn(ctx, cb.Get(), &p, sizeof(p));
        ID3D11UnorderedAccessView* uavs[1] = {sharedUav.Get()};
        Dispatch(ctx, touch.Get(), cb.Get(), uavs, 1, 1);
    };
    const auto copyOn = [&](ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* dst, u32 add) {
        const ViewParams p = Params(add, 0, 0, 0, 0, 0, 0);
        UploadOn(ctx, cb.Get(), &p, sizeof(p));
        ctx->CSSetShader(copy.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->CSSetShaderResources(0, 1, sharedSrv.GetAddressOf());
        ID3D11UnorderedAccessView* uavs[2] = {nullptr, dst};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch(1, 1, 1);
        UnbindCs(ctx);
    };

    touchOn(g.ctx, 1); // words 1, 2, 3, 4, ...: also a valid dispatch record (1, 2, 3)
    EndFrame(g);
    copyOn(g.ctx, out1Uav.Get(), 10);
    EndFrame(g);
    const ViewParams p = Params(7, 0, 0, 0, 0, 0, 0);
    Upload(g, cb.Get(), &p, sizeof(p));
    g.ctx->CSSetShader(fill.Get(), nullptr, 0);
    g.ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->CSSetUnorderedAccessViews(0, 1, filledUav.GetAddressOf(), nullptr);
    g.ctx->DispatchIndirect(shared.Get(), 0);
    UnbindCs(g.ctx);
    EndFrame(g);
    touchOn(g.ctx, 3);

    // 40 deferred lists: writes and reads alternate; the immediate context patches each entry state.
    for (u32 i = 0; i < 40; ++i)
    {
        if (i % 2 == 0)
            touchOn(def.Get(), i);
        else
            copyOn(def.Get(), out2Uav.Get(), i);
        ComPtr<ID3D11CommandList> list;
        if (!r.Ok(def->FinishCommandList(FALSE, &list), "FinishCommandList"))
            return;
        g.ctx->ExecuteCommandList(list.Get(), FALSE);
    }
    EndFrame(g);
    r.images.push_back(ReadWords(g, r, shared.Get(), 64, "shared buffer"));
    r.images.push_back(ReadWords(g, r, out1.Get(), 64, "SRV read after a frame boundary"));
    r.images.push_back(ReadWords(g, r, out2.Get(), 64, "SRV read in the last deferred list"));
    r.images.push_back(ReadWords(g, r, filled.Get(), 64, "DispatchIndirect from the shared buffer"));
}

// ---- infra 12: acceleration structure SRV ------------------------------------------------------

const char kTraceHlsl[] = R"(
RaytracingAccelerationStructure scene : register(t0);
struct VsOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};
VsOut FullVs(uint id : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.ndc = o.pos.xy;
    return o;
}
float4 TracePs(VsOut i) : SV_Target
{
    RayDesc ray;
    ray.Origin = float3(i.ndc, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0;
    ray.TMax = 10;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        return float4(0, 1, q.CommittedRayT() * 0.5, 1);
    return float4(1, 0, 0, 1);
}
)";

bool CompileSm65(Result& r, const char* entry, const char* target, std::vector<unsigned char>& out)
{
    xrd3d12::ShaderCompile request;
    request.source = kTraceHlsl;
    request.size = sizeof(kTraceHlsl) - 1;
    request.name = "layertest_trace.hlsl";
    request.entry = entry;
    request.target = target;
    request.preprocessed = true;
    xrd3d12::ShaderOutput output;
    if (!r.Check(xrd3d12::CompileShader(request, output), "%s (%s) does not compile: %s", entry, target, output.messages.c_str()))
        return false;
    out = std::move(output.object);
    return true;
}

// Inside the triangle (-0.5,-0.5) (0.5,-0.5) (0,0.5) with a margin: 1 inside, 0 outside, -1 on the edge.
int InsideTriangle(float x, float y)
{
    const float ax = -0.5f, ay = -0.5f, bx = 0.5f, by = -0.5f, cx = 0.0f, cy = 0.5f;
    const auto edge = [](float x0, float y0, float x1, float y1, float px, float py) {
        return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    };
    const float e0 = edge(ax, ay, bx, by, x, y), e1 = edge(bx, by, cx, cy, x, y), e2 = edge(cx, cy, ax, ay, x, y);
    constexpr float kMargin = 0.05f;
    if (e0 > kMargin && e1 > kMargin && e2 > kMargin)
        return 1;
    if (e0 < -kMargin || e1 < -kMargin || e2 < -kMargin)
        return 0;
    return -1;
}

// PS 6.5 RayQuery against a one-triangle TLAS bound through CreateAccelerationStructureView; the
// empty view and an unbound slot miss everywhere. Skipped without inline ray tracing.
void ScenarioAccelerationStructureView(Gpu& g, Result& r)
{
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(g.dev);
    ID3D12Device5* dev5 = xrd3d12::NativeDevice5(g.dev);
    if (!caps || caps->raytracingTier < 11 || xrd3d12::DxcShaderModel() < 0x65 || !dev5)
    {
        std::printf("      skipped: no inline ray tracing (tier %u, DXC 0x%X)\n", caps ? caps->raytracingTier : 0u,
            xrd3d12::DxcShaderModel());
        return;
    }
    std::vector<unsigned char> vsCode, psCode;
    if (!CompileSm65(r, "FullVs", "vs_6_5", vsCode) || !CompileSm65(r, "TracePs", "ps_6_5", psCode))
        return;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    r.Ok(g.dev->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs), "CreateVertexShader(vs_6_5)");
    r.Ok(g.dev->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps), "CreatePixelShader(ps_6_5)");
    REQUIRE_OK();

    // Geometry in upload memory, acceleration structures and scratch.
    ID3D12Resource* vertices = nullptr;
    const float triangle[9] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
    if (!r.Ok(xrd3d12::CreateNativeBuffer(g.dev, sizeof(triangle), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                  D3D12_RESOURCE_STATE_GENERIC_READ, L"layertest triangle", &vertices),
            "CreateNativeBuffer(triangle)"))
        return;
    void* mapped = nullptr;
    if (SUCCEEDED(vertices->Map(0, nullptr, &mapped)) && mapped)
    {
        std::memcpy(mapped, triangle, sizeof(triangle));
        vertices->Unmap(0, nullptr);
    }
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
    ID3D12Resource* blas = nullptr;
    ID3D12Resource* tlas = nullptr;
    ID3D12Resource* scratch = nullptr;
    r.Ok(xrd3d12::CreateAccelerationStructureBuffer(g.dev, bottomInfo.ResultDataMaxSizeInBytes, L"layertest blas", &blas), "BLAS buffer");
    r.Ok(xrd3d12::CreateAccelerationStructureBuffer(g.dev, topInfo.ResultDataMaxSizeInBytes, L"layertest tlas", &tlas), "TLAS buffer");
    r.Ok(xrd3d12::CreateNativeBuffer(g.dev, (std::max)(bottomInfo.ScratchDataSizeInBytes, topInfo.ScratchDataSizeInBytes) + 256,
             D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             L"layertest scratch", &scratch),
        "scratch buffer");
    const auto releaseAll = [&] {
        for (ID3D12Resource* object : {vertices, blas, tlas, scratch})
        {
            if (object)
                xrd3d12::ReleaseNative(g.dev, object);
        }
    };
    if (!r.failures.empty())
    {
        releaseAll();
        return;
    }

    // Builds on the immediate context's list: the instance places the triangle at z = 1.
    {
        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope) && scope.list4, "BeginNative refused or no ID3D12GraphicsCommandList4"))
        {
            const xrd3d12::UploadSpan instance = xrd3d12::AllocateUpload(g.ctx, sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
            if (r.Check(instance.cpu, "AllocateUpload(instance) failed"))
            {
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
                D3D12_RESOURCE_BARRIER uav{};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                scope.list->ResourceBarrier(1, &uav);
                build.Inputs = top;
                build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
                scope.list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                scope.list->ResourceBarrier(1, &uav);
            }
            xrd3d12::EndNative(g.ctx, 0);
        }
    }
    ComPtr<ID3D11ShaderResourceView> sceneSrv, emptySrv;
    r.Ok(xrd3d12::CreateAccelerationStructureView(g.dev, tlas, &sceneSrv), "CreateAccelerationStructureView(tlas)");
    r.Ok(xrd3d12::CreateAccelerationStructureView(g.dev, nullptr, &emptySrv), "CreateAccelerationStructureView(null)");
    Target hit = MakeTarget(g, r, g.ctx);
    Target empty = MakeTarget(g, r, g.ctx);
    Target unbound = MakeTarget(g, r, g.ctx);
    if (!r.failures.empty())
    {
        releaseAll();
        return;
    }

    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    const struct
    {
        const Target* target;
        ID3D11ShaderResourceView* view;
    } passes[3] = {{&hit, sceneSrv.Get()}, {&empty, emptySrv.Get()}, {&unbound, nullptr}};
    for (const auto& pass : passes)
    {
        BindTarget(g.ctx, *pass.target);
        g.ctx->PSSetShaderResources(0, 1, &pass.view);
        g.ctx->Draw(3, 0);
    }
    ID3D11ShaderResourceView* none = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    const Image hitImage = Read2D(g, r, hit.texture.Get(), 0, "ray query hits");
    const Image emptyImage = Read2D(g, r, empty.texture.Get(), 0, "empty acceleration structure");
    const Image unboundImage = Read2D(g, r, unbound.texture.Get(), 0, "unbound acceleration structure slot");
    constexpr u32 kRed = 0xFF0000FFu; // RGBA8 in memory order, little endian
    // A hit writes (0, 1, t / 2): t = 1 at the triangle's plane.
    const auto isHit = [](const u8* t) { return t[0] == 0 && t[1] == 255 && t[2] >= 126 && t[2] <= 130 && t[3] == 255; };
    u32 wrong = 0, inside = 0, missesWrong = 0;
    for (u32 y = 0; y < kTarget && hitImage.texels.size() == kTarget * kTarget * 4; ++y)
    {
        for (u32 x = 0; x < kTarget; ++x)
        {
            const float nx = (float(x) + 0.5f) / float(kTarget) * 2.0f - 1.0f;
            const float ny = 1.0f - (float(y) + 0.5f) / float(kTarget) * 2.0f;
            const int expected = InsideTriangle(nx, ny);
            const size_t at = (size_t(y) * kTarget + x) * 4;
            u32 texel = 0, emptyTexel = 0, unboundTexel = 0;
            std::memcpy(&texel, hitImage.texels.data() + at, 4);
            if (emptyImage.texels.size() == hitImage.texels.size())
                std::memcpy(&emptyTexel, emptyImage.texels.data() + at, 4);
            if (unboundImage.texels.size() == hitImage.texels.size())
                std::memcpy(&unboundTexel, unboundImage.texels.data() + at, 4);
            missesWrong += (emptyTexel != kRed) + (unboundTexel != kRed);
            if (expected < 0)
                continue;
            inside += expected;
            if (expected ? !isHit(hitImage.texels.data() + at) : texel != kRed)
                ++wrong;
        }
    }
    r.Check(inside > 200 && !wrong, "ray query: %u of the pixels wrong (%u inside)", wrong, inside);
    r.Check(!missesWrong, "the empty/unbound acceleration structure hit something (%u pixels)", missesWrong);
    releaseAll();
}
// ---- indexed indirect records told apart by SV_VertexID (r__native_selftest part g) --------------------

const char kStripHlsl[] = R"(
struct StripOut
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
};
static const float2 kStripCorners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
StripOut StripVs(uint id : SV_VertexID)
{
    uint strip = id / 6;
    float2 c = kStripCorners[id % 6];
    float2 texel = float2((strip + c.x) * 16.0, c.y * 64.0);
    StripOut o;
    o.pos = float4(texel.x / 32.0 - 1.0, 1.0 - texel.y / 32.0, 0, 1);
    o.color = float4((strip + 1) * 0.25, id % 6 == 5 ? 1.0 : 0.0, 0, 1);
    return o;
}
float4 StripPs(StripOut i) : SV_Target
{
    return i.color;
}
)";

// Four indexed records, the count buffer (initial data) says three: record k aims at strip k once
// through BaseVertexLocation and once through StartIndexLocation. SV_VertexID is the index value
// without BaseVertexLocation on D3D11 and on the layer alike (one strip, measured), so only the
// StartIndexLocation records paint three strips; both images must equal D3D11's.
void ScenarioIndirectVertexId(Gpu& g, Result& r)
{
    u16 indices[24];
    for (u16 i = 0; i < 24; ++i)
        indices[i] = i;
    u32 base[4][5]{}, start[4][5]{};
    for (u32 k = 0; k < 4; ++k)
    {
        base[k][0] = start[k][0] = 6;
        base[k][1] = start[k][1] = 1;
        base[k][3] = 6 * k;
        start[k][2] = 6 * k;
    }
    const u32 count[4] = {3, 0, 0, 0};
    constexpr UINT kMisc = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    auto ib = MakeBufferEx(g, r, sizeof(indices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, indices);
    auto baseArgs = MakeBufferEx(g, r, sizeof(base), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kMisc, 0, base);
    auto startArgs = MakeBufferEx(g, r, sizeof(start), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kMisc, 0, start);
    auto counter = MakeBufferEx(g, r, sizeof(count), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kMisc, 0, count);
    auto vs = MakeVs(g, r, kStripHlsl, "StripVs");
    auto ps = MakePs(g, r, kStripHlsl, "StripPs");
    Target viaBase = MakeTarget(g, r, g.ctx);
    Target viaStart = MakeTarget(g, r, g.ctx);
    REQUIRE_OK();
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    const struct
    {
        const Target* target;
        ID3D11Buffer* args;
    } passes[2] = {{&viaBase, baseArgs.Get()}, {&viaStart, startArgs.Get()}};
    for (const auto& pass : passes)
    {
        BindTarget(g.ctx, *pass.target);
        MultiDraw(g, g.ctx, true, pass.args, counter.Get(), 0, 4, 3);
    }
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    const Image images[2] = {Read2D(g, r, viaBase.texture.Get(), 0, "records through BaseVertexLocation"),
        Read2D(g, r, viaStart.texture.Get(), 0, "records through StartIndexLocation")};
    // Strips painted in each image (row 32, the red channel of each strip's centre texel).
    u32 painted[2] = {};
    for (u32 i = 0; i < 2; ++i)
    {
        for (u32 strip = 0; strip < 4 && images[i].texels.size() == kTarget * kTarget * 4; ++strip)
            painted[i] += images[i].texels[(32 * kTarget + strip * 16 + 8) * 4] != 0;
        r.Value(images[i].label.c_str(), painted[i]);
    }
    std::printf("      %s: strips painted through BaseVertexLocation %u, through StartIndexLocation %u\n", g.name, painted[0],
        painted[1]);
    r.images.push_back(images[0]);
    r.images.push_back(images[1]);
}
} // namespace

const std::vector<Scenario>& ViewScenarios()
{
    static const std::vector<Scenario> list = {
        {"buffer views: raw, structured and typed UAVs cleared and written by CS, read as VS SRVs", ScenarioBufferViews, false},
        {"buffer view rules: D3D11 validation, dynamic and counter refusals, committed SRV buffers", ScenarioBufferViewRules, true},
        {"per-instance stream with StartInstanceLocation", ScenarioInstanceStream, false},
        {"UAV -> vertex buffer -> UAV in one list and across a deferred list", ScenarioUavVertexUav, false},
        {"indirect draws: DrawIndexedInstancedIndirect, DrawInstancedIndirect, DispatchIndirect(Count)", ScenarioIndirectCalls, false},
        {"multi-draw: indirect count variants over 64 records vs single indirect draws", ScenarioMultiDraw, false},
        {"deferred cull + draw: two lists on the same buffers in one frame", ScenarioDeferredCullDraw, false},
        {"tracked buffers across submissions: decay to COMMON, 40 deferred lists in one submission", ScenarioTrackedBufferDecay, false},
        {"acceleration structure SRV: PS 6.5 RayQuery through CreateAccelerationStructureView", ScenarioAccelerationStructureView, true},
        {"indexed indirect count: records told apart by SV_VertexID (base vertex, start index), count from initial data",
            ScenarioIndirectVertexId, false},
    };
    return list;
}
} // namespace layertest
