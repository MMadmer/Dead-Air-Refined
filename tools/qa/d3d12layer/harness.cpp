// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Helpers of the differential tests: shader compilation, resource creation, readback.
#include "harness.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

namespace layertest
{
namespace
{
std::string Format(const char* fmt, va_list args)
{
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    return buffer;
}

u32 TexelSize(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT: return 4;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UINT: return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT: return 1;
    default: return 0;
    }
}
} // namespace

// ---- Result ----------------------------------------------------------------------------------

void Result::Fail(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    failures.push_back(Format(fmt, args));
    va_end(args);
}

bool Result::Check(bool condition, const char* fmt, ...)
{
    if (!condition)
    {
        va_list args;
        va_start(args, fmt);
        failures.push_back(Format(fmt, args));
        va_end(args);
    }
    return condition;
}

bool Result::Ok(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fail("%s failed (0x%08X)", what, unsigned(hr));
    return SUCCEEDED(hr);
}

// ---- shaders ---------------------------------------------------------------------------------

ID3DBlob* Compile(const char* source, const char* entry, const char* profile)
{
    static std::mutex lock;
    static std::map<std::tuple<const char*, std::string, std::string>, ID3DBlob*> cache;
    std::lock_guard guard(lock);
    const auto key = std::make_tuple(source, std::string(entry), std::string(profile));
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), entry, nullptr, nullptr, entry, profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, &code, &errors);
    if (FAILED(hr))
    {
        std::printf("FATAL shader %s (%s) does not compile: %s\n", entry, profile,
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        std::exit(3);
    }
    if (errors)
        errors->Release();
    cache.emplace(key, code);
    return code;
}

bool g_dxil = false;

const std::vector<unsigned char>& CompileDxil(const char* source, const char* entry, const char* profile)
{
    static std::mutex lock;
    static std::map<std::tuple<const char*, std::string, std::string>, std::vector<unsigned char>> cache;
    std::lock_guard guard(lock);
    const auto key = std::make_tuple(source, std::string(entry), std::string(profile));
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    // "vs_5_0" -> "vs_6_0"; SM 6 profiles stay as they are.
    std::string target = profile;
    if (target.size() == 6 && target[3] == '5')
        target = target.substr(0, 3) + "6_0";
    static const wchar_t* const kArgs[] = {L"-Zpr", L"-O3"};
    xrd3d12::ShaderCompile request;
    request.source = source;
    request.size = std::strlen(source);
    request.name = entry;
    request.entry = entry;
    request.target = target.c_str();
    request.hlsl = 2018;
    request.args = kArgs;
    request.argCount = UINT(std::size(kArgs));
    request.preprocessed = true;
    xrd3d12::ShaderOutput output;
    if (!xrd3d12::CompileShader(request, output))
    {
        std::printf("FATAL shader %s (%s) does not compile with DXC: %s\n", entry, target.c_str(), output.messages.c_str());
        std::exit(3);
    }
    return cache.emplace(key, std::move(output.object)).first->second;
}

ShaderCode Code(const Gpu& g, const char* source, const char* entry, const char* profile)
{
    if (g_dxil && g.layer)
    {
        const std::vector<unsigned char>& dxil = CompileDxil(source, entry, profile);
        return {dxil.data(), dxil.size()};
    }
    ID3DBlob* code = Compile(source, entry, profile);
    return {code->GetBufferPointer(), code->GetBufferSize()};
}

#define LAYERTEST_SHADER_MAKER(Name, Iface, Create, Profile)                                             \
    ComPtr<Iface> Name(Gpu& g, Result& r, const char* source, const char* entry)                     \
    {                                                                                                  \
        const ShaderCode code = Code(g, source, entry, Profile);                                       \
        ComPtr<Iface> shader;                                                                          \
        r.Ok(g.dev->Create(code.data, code.size, nullptr, &shader), #Create);                          \
        return shader;                                                                                 \
    }

LAYERTEST_SHADER_MAKER(MakeVs, ID3D11VertexShader, CreateVertexShader, "vs_5_0")
LAYERTEST_SHADER_MAKER(MakePs, ID3D11PixelShader, CreatePixelShader, "ps_5_0")
LAYERTEST_SHADER_MAKER(MakeGs, ID3D11GeometryShader, CreateGeometryShader, "gs_5_0")
LAYERTEST_SHADER_MAKER(MakeHs, ID3D11HullShader, CreateHullShader, "hs_5_0")
LAYERTEST_SHADER_MAKER(MakeDs, ID3D11DomainShader, CreateDomainShader, "ds_5_0")
LAYERTEST_SHADER_MAKER(MakeCs, ID3D11ComputeShader, CreateComputeShader, "cs_5_0")
#undef LAYERTEST_SHADER_MAKER

// ---- resources -------------------------------------------------------------------------------

D3D11_TEXTURE2D_DESC Desc2D(u32 width, u32 height, DXGI_FORMAT format, UINT bind, u32 mips, u32 slices)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = mips;
    d.ArraySize = slices;
    d.Format = format;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    return d;
}

ComPtr<ID3D11Texture2D> MakeTexture2D(Gpu& g, Result& r, const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data)
{
    ComPtr<ID3D11Texture2D> texture;
    r.Ok(g.dev->CreateTexture2D(&desc, data, &texture), "CreateTexture2D");
    return texture;
}

ComPtr<ID3D11Buffer> MakeBuffer(Gpu& g, Result& r, UINT size, D3D11_USAGE usage, UINT bind, const void* data)
{
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    r.Ok(g.dev->CreateBuffer(&d, data ? &init : nullptr, &buffer), "CreateBuffer");
    return buffer;
}

ComPtr<ID3D11RenderTargetView> MakeRtv(Gpu& g, Result& r, ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc)
{
    ComPtr<ID3D11RenderTargetView> view;
    r.Ok(g.dev->CreateRenderTargetView(resource, desc, &view), "CreateRenderTargetView");
    return view;
}

ComPtr<ID3D11ShaderResourceView> MakeSrv(Gpu& g, Result& r, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc)
{
    ComPtr<ID3D11ShaderResourceView> view;
    r.Ok(g.dev->CreateShaderResourceView(resource, desc, &view), "CreateShaderResourceView");
    return view;
}

ComPtr<ID3D11DepthStencilView> MakeDsv(Gpu& g, Result& r, ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc)
{
    ComPtr<ID3D11DepthStencilView> view;
    r.Ok(g.dev->CreateDepthStencilView(resource, desc, &view), "CreateDepthStencilView");
    return view;
}

ComPtr<ID3D11SamplerState> MakeSampler(Gpu& g, Result& r, D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address)
{
    D3D11_SAMPLER_DESC d{};
    d.Filter = filter;
    d.AddressU = d.AddressV = d.AddressW = address;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MinLOD = 0;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    r.Ok(g.dev->CreateSamplerState(&d, &sampler), "CreateSamplerState");
    return sampler;
}

void Upload(Gpu& g, ID3D11Resource* buffer, const void* data, size_t size)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g.ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, data, size);
        g.ctx->Unmap(buffer, 0);
    }
}

void SetViewport(Gpu& g, float x, float y, float width, float height, float minDepth, float maxDepth)
{
    const D3D11_VIEWPORT vp{x, y, width, height, minDepth, maxDepth};
    g.ctx->RSSetViewports(1, &vp);
}

// ---- readback --------------------------------------------------------------------------------

Image Read2D(Gpu& g, Result& r, ID3D11Texture2D* texture, u32 sub, const char* label, double tolerance)
{
    Image image;
    image.label = label;
    image.tolerance = tolerance;
    if (!texture)
    {
        r.Fail("%s: no texture to read", label);
        return image;
    }
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE;
    ComPtr<ID3D11Texture2D> staging;
    if (!r.Ok(g.dev->CreateTexture2D(&s, nullptr, &staging), "staging CreateTexture2D"))
        return image;
    g.ctx->CopyResource(staging.Get(), texture);
    const u32 mip = sub % d.MipLevels;
    image.format = d.Format;
    image.width = (std::max)(1u, d.Width >> mip);
    image.height = (std::max)(1u, d.Height >> mip);
    image.texelSize = TexelSize(d.Format);
    if (!image.texelSize)
    {
        r.Fail("%s: format %u cannot be compared", label, unsigned(d.Format));
        return image;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!r.Ok(g.ctx->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped), "Map of a staging copy"))
        return image;
    const size_t row = size_t(image.width) * image.texelSize;
    image.texels.resize(row * image.height);
    for (u32 y = 0; y < image.height; ++y)
        std::memcpy(image.texels.data() + y * row, static_cast<const u8*>(mapped.pData) + size_t(y) * mapped.RowPitch, row);
    g.ctx->Unmap(staging.Get(), sub);
    return image;
}

Image Read3D(Gpu& g, Result& r, ID3D11Texture3D* texture, const char* label, double tolerance)
{
    Image image;
    image.label = label;
    image.tolerance = tolerance;
    D3D11_TEXTURE3D_DESC d{};
    texture->GetDesc(&d);
    D3D11_TEXTURE3D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = 0;
    ComPtr<ID3D11Texture3D> staging;
    if (!r.Ok(g.dev->CreateTexture3D(&s, nullptr, &staging), "staging CreateTexture3D"))
        return image;
    g.ctx->CopyResource(staging.Get(), texture);
    image.format = d.Format;
    image.width = d.Width;
    image.height = d.Height;
    image.depth = d.Depth;
    image.texelSize = TexelSize(d.Format);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!r.Ok(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map of a staging 3D copy"))
        return image;
    const size_t row = size_t(image.width) * image.texelSize;
    image.texels.resize(row * image.height * image.depth);
    for (u32 z = 0; z < image.depth; ++z)
    {
        for (u32 y = 0; y < image.height; ++y)
        {
            std::memcpy(image.texels.data() + (size_t(z) * image.height + y) * row,
                static_cast<const u8*>(mapped.pData) + size_t(z) * mapped.DepthPitch + size_t(y) * mapped.RowPitch, row);
        }
    }
    g.ctx->Unmap(staging.Get(), 0);
    return image;
}

// ---- common shaders --------------------------------------------------------------------------

const char* const kQuadHlsl = R"(
cbuffer QuadConstants : register(b0)
{
    float4 rect;
    float4 color;
    float4 misc;
};

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 kCorners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };

VsOut QuadVs(uint id : SV_VertexID)
{
    float2 t = kCorners[id % 6];
    if (misc.y > 0.5)
        t.x = 1 - t.x;
    VsOut o;
    o.pos = float4(lerp(rect.xy, rect.zw, t), misc.x, 1);
    o.uv = t;
    return o;
}

VsOut FullscreenVs(uint id : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), misc.x, 1);
    return o;
}

float4 ColorPs(VsOut i) : SV_Target
{
    return color;
}

float4 UvPs(VsOut i) : SV_Target
{
    return float4(i.uv, 0.5, 1);
}
)";

QuadConstants QuadPx(float x0, float y0, float x1, float y1, u32 width, u32 height, float r, float g, float b, float a, float depth)
{
    QuadConstants q{};
    q.rect[0] = x0 / float(width) * 2.0f - 1.0f;
    q.rect[1] = 1.0f - y0 / float(height) * 2.0f;
    q.rect[2] = x1 / float(width) * 2.0f - 1.0f;
    q.rect[3] = 1.0f - y1 / float(height) * 2.0f;
    q.color[0] = r;
    q.color[1] = g;
    q.color[2] = b;
    q.color[3] = a;
    q.misc[0] = depth;
    return q;
}
} // namespace layertest
