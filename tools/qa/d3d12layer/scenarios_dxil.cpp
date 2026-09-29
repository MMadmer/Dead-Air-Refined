// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `infra` lane for DXIL through the D3D11 path (infra.md 9.1 scenario 11,
// dxc.md 9 test 5: dxil_bindings, dxil_dxbc_mix). Scenarios() appends this list.
#include "harness.h"

#include <d3d12shader.h>

#include <cstring>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kRtSrv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

// DXC with the engine's arguments (dxc.md 3.3), SM 6.0.
std::vector<unsigned char> EngineDxil(Result& r, const char* source, const char* entry, const char* target)
{
    static const wchar_t* const kArgs[] = {L"-Zpr", L"-O3", L"-flegacy-resource-reservation", L"-Wno-conversion"};
    xrd3d12::ShaderCompile request;
    request.source = source;
    request.size = std::strlen(source);
    request.name = entry;
    request.entry = entry;
    request.target = target;
    request.hlsl = 2018;
    request.args = kArgs;
    request.argCount = UINT(std::size(kArgs));
    request.preprocessed = true;
    xrd3d12::ShaderOutput output;
    if (!xrd3d12::CompileShader(request, output))
        r.Fail("%s (%s) does not compile with DXC: %s", entry, target, output.messages.c_str());
    else if (!output.isSigned)
        r.Fail("%s: DXC output is not signed", entry);
    return output.object;
}

// ---- 11. DXIL through the D3D11 path ---------------------------------------------------------

// The reference (D3D11, FXC) and the layer (DXC, SM 6) compute the same: wave intrinsics on the
// layer only check themselves and fall back to the plain result.
const char* const kWaveHlsl = R"(
Texture2D<uint> input : register(t0);
RWTexture2D<uint> output : register(u0);
cbuffer Params : register(b0)
{
    uint4 params;
};
[numthreads(8, 8, 1)]
void WaveCs(uint3 id : SV_DispatchThreadID)
{
    uint value = input[id.xy] * params.x + params.y;
#if defined(__SHADER_TARGET_MAJOR) && __SHADER_TARGET_MAJOR >= 6
    // Lane arithmetic that holds on any wave size.
    uint lanes = WaveActiveSum(1u);
    bool ok = lanes == WaveActiveCountBits(true) && WavePrefixSum(1u) == WaveGetLaneIndex() &&
        WaveActiveMax(value) >= value && lanes == WaveGetLaneCount();
    value = ok ? value : 0xDEADu;
#endif
    output[id.xy] = value;
}
)";

const char* const kPairHlsl = R"(
cbuffer Tint : register(b0)
{
    float4 tint;
    float4 offset;
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VsIn
{
    float2 pos : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};
struct VsOut
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};
VsOut PairVs(VsIn v)
{
    VsOut o;
    o.pos = float4(v.pos + offset.xy, 0.5, 1);
    o.color = v.color;
    o.uv = v.uv;
    return o;
}
float4 PairPs(VsOut i) : SV_Target
{
    return tex.Sample(smp, i.uv) * i.color * tint;
}
)";

struct PairVertex
{
    float pos[2];
    float color[4];
    float uv[2];
};

void ScenarioDxilPath(Gpu& g, Result& r)
{
    const bool dxil = g.layer;
    if (dxil && !xrd3d12::DxcShaderModel())
    {
        r.Fail("the layer device has no shader compiler (DXC next to the executable?)");
        return;
    }

    // Compute: R32_UINT in, R32_UINT UAV out.
    std::vector<u32> seed(16 * 16);
    for (u32 i = 0; i < seed.size(); ++i)
        seed[i] = (i * 2654435761u) >> 20;
    D3D11_SUBRESOURCE_DATA seedData{seed.data(), 16 * 4, 0};
    auto input = MakeTexture2D(g, r, Desc2D(16, 16, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE), &seedData);
    auto output = MakeTexture2D(g, r, Desc2D(16, 16, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS));
    const u32 params[4] = {3, 1, 0, 0};
    auto cb = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    Upload(g, cb.Get(), params, sizeof(params));
    auto inputSrv = MakeSrv(g, r, input.Get());
    ComPtr<ID3D11UnorderedAccessView> outputUav;
    r.Ok(g.dev->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav), "CreateUnorderedAccessView");
    ComPtr<ID3D11ComputeShader> cs;
    if (dxil)
    {
        const std::vector<unsigned char> code = EngineDxil(r, kWaveHlsl, "WaveCs", "cs_6_0");
        REQUIRE_OK();
        r.Ok(g.dev->CreateComputeShader(code.data(), code.size(), nullptr, &cs), "CreateComputeShader (DXIL)");
    }
    else
    {
        ID3DBlob* code = Compile(kWaveHlsl, "WaveCs", "cs_5_0");
        r.Ok(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs), "CreateComputeShader");
    }
    REQUIRE_OK();
    g.ctx->CSSetShader(cs.Get(), nullptr, 0);
    g.ctx->CSSetShaderResources(0, 1, inputSrv.GetAddressOf());
    g.ctx->CSSetUnorderedAccessViews(0, 1, outputUav.GetAddressOf(), nullptr);
    g.ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->Dispatch(2, 2, 1);
    ID3D11UnorderedAccessView* noUav = nullptr;
    g.ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    g.ctx->CSSetShader(nullptr, nullptr, 0);
    r.images.push_back(Read2D(g, r, output.Get(), 0, "wave compute"));

    // Graphics: a VS/PS pair; the layer's input layout comes from the DXIL VS's ISG1 signature.
    const PairVertex quad[6] = {
        {{-0.8f, -0.8f}, {1, 0.5f, 0.25f, 1}, {0, 1}}, {{0.8f, -0.8f}, {0.25f, 1, 0.5f, 1}, {1, 1}},
        {{-0.8f, 0.8f}, {0.5f, 0.25f, 1, 1}, {0, 0}}, {{0.8f, -0.8f}, {0.25f, 1, 0.5f, 1}, {1, 1}},
        {{0.8f, 0.8f}, {1, 1, 1, 1}, {1, 0}}, {{-0.8f, 0.8f}, {0.5f, 0.25f, 1, 1}, {0, 0}},
    };
    auto vb = MakeBuffer(g, r, sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, quad);
    std::vector<u32> texels(8 * 8);
    for (u32 i = 0; i < texels.size(); ++i)
        texels[i] = 0xFF000000u | ((i * 37u) & 0xFF) | (((i * 91u) & 0xFF) << 8) | (((i * 13u) & 0xFF) << 16);
    D3D11_SUBRESOURCE_DATA texData{texels.data(), 8 * 4, 0};
    auto tex = MakeTexture2D(g, r, Desc2D(8, 8, kRgba8, D3D11_BIND_SHADER_RESOURCE), &texData);
    auto target = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, kRtSrv));
    REQUIRE_OK();
    auto texSrv = MakeSrv(g, r, tex.Get());
    auto rtv = MakeRtv(g, r, target.Get());
    auto smp = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);
    const float tint[8] = {1, 0.75f, 0.5f, 1, 0.05f, -0.05f, 0, 0};
    auto tintCb = MakeBuffer(g, r, sizeof(tint), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    REQUIRE_OK();
    Upload(g, tintCb.Get(), tint, sizeof(tint));

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (dxil)
    {
        const std::vector<unsigned char> vsCode = EngineDxil(r, kPairHlsl, "PairVs", "vs_6_0");
        const std::vector<unsigned char> psCode = EngineDxil(r, kPairHlsl, "PairPs", "ps_6_0");
        REQUIRE_OK();
        r.Ok(g.dev->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs), "CreateVertexShader (DXIL)");
        r.Ok(g.dev->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps), "CreatePixelShader (DXIL)");
        // The engine's path: the signature blob of the DXIL VS (a container with ISG1).
        ComPtr<ID3DBlob> signature;
        r.Ok(D3DGetInputSignatureBlob(vsCode.data(), vsCode.size(), &signature), "D3DGetInputSignatureBlob (DXIL)");
        REQUIRE_OK();
        r.Ok(g.dev->CreateInputLayout(elements, 3, signature->GetBufferPointer(), signature->GetBufferSize(), &layout),
            "CreateInputLayout (ISG1)");
        // Reflection of the stripped object through the service.
        ComPtr<ID3D12ShaderReflection> reflection;
        if (r.Ok(xrd3d12::Reflect(psCode.data(), psCode.size(), IID_PPV_ARGS(&reflection)), "Reflect (DXIL)"))
        {
            D3D12_SHADER_DESC desc{};
            reflection->GetDesc(&desc);
            r.Check(desc.BoundResources == 3, "DXIL reflection: %u bound resources (3 expected)", desc.BoundResources);
        }
    }
    else
    {
        vs = MakeVs(g, r, kPairHlsl, "PairVs");
        ps = MakePs(g, r, kPairHlsl, "PairPs");
        ID3DBlob* code = Compile(kPairHlsl, "PairVs", "vs_5_0");
        r.Ok(g.dev->CreateInputLayout(elements, 3, code->GetBufferPointer(), code->GetBufferSize(), &layout), "CreateInputLayout");
    }
    REQUIRE_OK();
    const float black[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), black);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 64, 64);
    const UINT stride = sizeof(PairVertex), offset = 0;
    g.ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    g.ctx->IASetInputLayout(layout.Get());
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, tintCb.GetAddressOf());
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, tintCb.GetAddressOf());
    g.ctx->PSSetShaderResources(0, 1, texSrv.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, smp.GetAddressOf());
    g.ctx->Draw(6, 0);
    g.ctx->ClearState();
    // Colour interpolation and the multiply may round differently between the compilers.
    r.images.push_back(Read2D(g, r, target.Get(), 0, "VS/PS pair", 1.0));
}

// ---- dxil_bindings: the same draw built by FXC and by DXC on the layer --------------------------

// `s_unused` binds t0 explicitly and is never read: FXC reserves t0, so `s_color` lands on t1.
// -flegacy-resource-reservation makes DXC do the same (dxc.md 3.3).
const char* const kBindingsHlsl = R"(
cbuffer A : register(b0) { float4 a; };
cbuffer B : register(b1) { float4 b; };
Texture2D s_unused : register(t0);
Texture2D s_color;
Texture2D s_detail : register(t2);
SamplerState s_point : register(s1);
float4 BindVs(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 BindPs(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy / 32;
    return s_color.Sample(s_point, uv) * a + s_detail.Sample(s_point, uv) * b;
}
)";

void ScenarioDxilBindings(Gpu& g, Result& r)
{
    if (!xrd3d12::DxcShaderModel())
    {
        r.Fail("the layer device has no shader compiler");
        return;
    }
    auto target = MakeTexture2D(g, r, Desc2D(32, 32, kRgba8, kRtSrv));
    const u32 red = 0xFF0000FFu, green = 0xFF00FF00u, blue = 0xFFFF0000u;
    std::vector<u32> texels(4 * 4);
    ComPtr<ID3D11Texture2D> textures[3];
    ComPtr<ID3D11ShaderResourceView> views[3];
    const u32 colors[3] = {red, green, blue};
    for (u32 t = 0; t < 3; ++t)
    {
        for (u32 i = 0; i < texels.size(); ++i)
            texels[i] = (i & 1) ? colors[t] : (colors[t] & 0xFF7F7F7Fu);
        D3D11_SUBRESOURCE_DATA data{texels.data(), 16, 0};
        textures[t] = MakeTexture2D(g, r, Desc2D(4, 4, kRgba8, D3D11_BIND_SHADER_RESOURCE), &data);
        REQUIRE_OK();
        views[t] = MakeSrv(g, r, textures[t].Get());
    }
    const float ab[8] = {1, 1, 1, 1, 0.25f, 0.25f, 0.25f, 0};
    auto cbA = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto cbB = MakeBuffer(g, r, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto smp = MakeSampler(g, r, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP);
    auto rtv = MakeRtv(g, r, target.Get());
    REQUIRE_OK();
    Upload(g, cbA.Get(), ab, 16);
    Upload(g, cbB.Get(), ab + 4, 16);

    ID3DBlob* fxcVs = Compile(kBindingsHlsl, "BindVs", "vs_5_0");
    ID3DBlob* fxcPs = Compile(kBindingsHlsl, "BindPs", "ps_5_0");
    const std::vector<unsigned char> dxcVs = EngineDxil(r, kBindingsHlsl, "BindVs", "vs_6_0");
    const std::vector<unsigned char> dxcPs = EngineDxil(r, kBindingsHlsl, "BindPs", "ps_6_0");
    REQUIRE_OK();
    ComPtr<ID3D11VertexShader> vs[2];
    ComPtr<ID3D11PixelShader> ps[2];
    r.Ok(g.dev->CreateVertexShader(fxcVs->GetBufferPointer(), fxcVs->GetBufferSize(), nullptr, &vs[0]), "CreateVertexShader (DXBC)");
    r.Ok(g.dev->CreatePixelShader(fxcPs->GetBufferPointer(), fxcPs->GetBufferSize(), nullptr, &ps[0]), "CreatePixelShader (DXBC)");
    r.Ok(g.dev->CreateVertexShader(dxcVs.data(), dxcVs.size(), nullptr, &vs[1]), "CreateVertexShader (DXIL)");
    r.Ok(g.dev->CreatePixelShader(dxcPs.data(), dxcPs.size(), nullptr, &ps[1]), "CreatePixelShader (DXIL)");
    REQUIRE_OK();

    Image images[2];
    for (u32 pass = 0; pass < 2; ++pass)
    {
        const float black[4] = {0, 0, 0, 0};
        g.ctx->ClearRenderTargetView(rtv.Get(), black);
        g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        SetViewport(g, 0, 0, 32, 32);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(vs[pass].Get(), nullptr, 0);
        g.ctx->PSSetShader(ps[pass].Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = {cbA.Get(), cbB.Get()};
        g.ctx->PSSetConstantBuffers(0, 2, cbs);
        ID3D11ShaderResourceView* srvs[3] = {views[0].Get(), views[1].Get(), views[2].Get()};
        g.ctx->PSSetShaderResources(0, 3, srvs);
        g.ctx->PSSetSamplers(1, 1, smp.GetAddressOf());
        g.ctx->Draw(3, 0);
        g.ctx->ClearState();
        images[pass] = Read2D(g, r, target.Get(), 0, pass ? "bindings DXIL" : "bindings DXBC");
    }
    REQUIRE_OK();
    size_t differ = 0;
    for (size_t i = 0; i < images[0].texels.size(); ++i)
    {
        const int d = int(images[0].texels[i]) - int(images[1].texels[i]);
        differ += d > 1 || d < -1;
    }
    r.Check(differ == 0, "the DXIL build binds differently: %zu bytes differ from the DXBC build", differ);
    // s_color on t1 (green), not t0 (red): the reserved register holds.
    r.Check(images[1].texels.size() >= 4 && images[1].texels[1] > images[1].texels[0], "the DXIL build read the wrong texture");
    r.images.push_back(images[1]);
}

// ---- dxil_dxbc_mix: a pipeline of both formats is refused by name, never reaches the runtime ----

void ScenarioDxilDxbcMix(Gpu& g, Result& r)
{
    if (!xrd3d12::DxcShaderModel())
    {
        r.Fail("the layer device has no shader compiler");
        return;
    }
    auto target = MakeTexture2D(g, r, Desc2D(16, 16, kRgba8, kRtSrv));
    auto rtv = MakeRtv(g, r, target.Get());
    REQUIRE_OK();
    ID3DBlob* fxcVs = Compile(kBindingsHlsl, "BindVs", "vs_5_0");
    const char* const psText = "float4 MixPs() : SV_Target { return float4(1, 0, 1, 1); }";
    const std::vector<unsigned char> dxcPs = EngineDxil(r, psText, "MixPs", "ps_6_0");
    REQUIRE_OK();
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    r.Ok(g.dev->CreateVertexShader(fxcVs->GetBufferPointer(), fxcVs->GetBufferSize(), nullptr, &vs), "CreateVertexShader (DXBC)");
    r.Ok(g.dev->CreatePixelShader(dxcPs.data(), dxcPs.size(), nullptr, &ps), "CreatePixelShader (DXIL)");
    REQUIRE_OK();
    const int before = LayerLogCount("mixes DXIL");
    const float clear[4] = {0, 0.5f, 0, 1};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, 16, 16);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(ps.Get(), nullptr, 0);
    g.ctx->Draw(3, 0);
    g.ctx->Draw(3, 0);
    g.ctx->ClearState();
    const Image image = Read2D(g, r, target.Get(), 0, "mixed pipeline");
    REQUIRE_OK();
    // The pixel shader writes magenta; the clear left red and blue at 0.
    bool untouched = !image.texels.empty();
    for (size_t i = 0; i + 3 < image.texels.size(); i += 4)
        untouched = untouched && image.texels[i] == 0 && image.texels[i + 2] == 0;
    r.Check(untouched, "the mixed pipeline drew");
    r.Check(LayerLogCount("mixes DXIL") == before + 1, "the refusal was logged %d times (once expected)",
        LayerLogCount("mixes DXIL") - before);
}

// ---- Reflect on every form CompileShader hands out -------------------------------------------------

// The object with its reflection kept, the stripped object, and DXC_OUT_REFLECTION: a bare STAT part
// (no container), which only DXC reads.
void ScenarioDxcReflectionBlob(Gpu&, Result& r)
{
    if (!xrd3d12::DxcShaderModel())
    {
        r.Fail("the layer device has no shader compiler");
        return;
    }
    xrd3d12::ShaderCompile request;
    request.source = kWaveHlsl;
    request.size = std::strlen(kWaveHlsl);
    request.name = "WaveCs";
    request.entry = "WaveCs";
    request.target = "cs_6_0";
    request.preprocessed = true;
    xrd3d12::ShaderOutput stripped, kept;
    if (!r.Check(xrd3d12::CompileShader(request, stripped), "WaveCs does not compile: %s", stripped.messages.c_str()))
        return;
    request.flags = xrd3d12::CompileKeepReflection;
    if (!r.Check(xrd3d12::CompileShader(request, kept), "WaveCs (kept reflection) does not compile: %s", kept.messages.c_str()))
        return;
    const auto bindings = [&](const std::vector<unsigned char>& blob, const char* what) -> UINT {
        ID3D12ShaderReflection* reflection = nullptr;
        const HRESULT hr = xrd3d12::Reflect(blob.data(), blob.size(), __uuidof(ID3D12ShaderReflection), reinterpret_cast<void**>(&reflection));
        if (!r.Check(SUCCEEDED(hr) && reflection, "Reflect(%s) failed (0x%08X)", what, unsigned(hr)))
            return 0;
        D3D12_SHADER_DESC desc{};
        reflection->GetDesc(&desc);
        reflection->Release();
        return desc.BoundResources;
    };
    const bool bare = stripped.reflection.size() >= 8 && !std::memcmp(stripped.reflection.data(), "STAT", 4);
    r.Check(bare, "DXC_OUT_REFLECTION is no longer a bare STAT part: the check below tests nothing new");
    // input, output, Params.
    r.Check(bindings(stripped.reflection, "DXC_OUT_REFLECTION") == 3, "the reflection blob does not report 3 bindings");
    r.Check(bindings(kept.object, "object with reflection") == 3, "the object with reflection does not report 3 bindings");
}
} // namespace

const std::vector<Scenario>& DxilScenarios()
{
    static const std::vector<Scenario> list = {
        {"DXIL through the D3D11 path: wave compute, VS/PS pair with an ISG1 input layout", ScenarioDxilPath, false},
        {"dxil_bindings: one draw built by FXC and by DXC on the layer, same bindings", ScenarioDxilBindings, true},
        {"dxil_dxbc_mix: a DXIL/DXBC pipeline refused by name", ScenarioDxilDxbcMix, true},
        {"Reflect: DXC reflection blob (bare STAT part) and object with reflection", ScenarioDxcReflectionBlob, true},
    };
    return list;
}
} // namespace layertest
