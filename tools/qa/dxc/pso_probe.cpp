// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Offline pipeline probe for the DXC audit (docs/dead-air/dx12-features/dxc.md 6.1): creates a D3D12
// pipeline for every stage combination tools\qa\Check-DxcShaders.py --out wrote (pipelines.txt +
// blobs\), with root signatures shaped like the layer's, and reports refusals with the debug layer's
// reason. Lines whose title ends in "(gated)" are pipelines the game builds only behind a legacy switch:
// their refusals are listed but do not fail the run.
//
//   pso_probe.exe <dir> [-adapter N]
//
// Exit code: 0 all created (or only gated ones refused), 1 a refusal outside the gated set, 2 usage,
// 3 no device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
std::vector<unsigned char> ReadAll(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// DXIL containers use the DXBC container header.
const unsigned char* Chunk(const std::vector<unsigned char>& b, const char* fourcc, unsigned& size)
{
    if (b.size() < 32 || memcmp(b.data(), "DXBC", 4))
        return nullptr;
    const unsigned count = *reinterpret_cast<const unsigned*>(b.data() + 28);
    for (unsigned i = 0; i < count; ++i)
    {
        const unsigned off = *reinterpret_cast<const unsigned*>(b.data() + 32 + 4 * i);
        if (!memcmp(b.data() + off, fourcc, 4))
        {
            size = *reinterpret_cast<const unsigned*>(b.data() + off + 4);
            return b.data() + off + 8;
        }
    }
    return nullptr;
}

struct SigElem
{
    std::string name;
    unsigned index, sysval, ctype, reg, mask;
};

std::vector<SigElem> Signature(const std::vector<unsigned char>& b, const char* fourcc)
{
    std::vector<SigElem> out;
    unsigned size = 0;
    const unsigned char* c = Chunk(b, fourcc, size);
    if (!c)
        return out;
    const unsigned n = *reinterpret_cast<const unsigned*>(c);
    for (unsigned i = 0; i < n; ++i)
    {
        // ISG1/OSG1 elements: stream, name offset, index, system value, component type, register, mask.
        const unsigned char* e = c + 8 + i * 32 + 4;
        SigElem s;
        s.name = reinterpret_cast<const char*>(c + *reinterpret_cast<const unsigned*>(e));
        s.index = *reinterpret_cast<const unsigned*>(e + 4);
        s.sysval = *reinterpret_cast<const unsigned*>(e + 8);
        s.ctype = *reinterpret_cast<const unsigned*>(e + 12);
        s.reg = *reinterpret_cast<const unsigned*>(e + 16);
        s.mask = e[20];
        out.push_back(s);
    }
    return out;
}

DXGI_FORMAT FormatFor(unsigned ctype, unsigned mask)
{
    const int comps = (mask & 8) ? 4 : (mask & 4) ? 3 : (mask & 2) ? 2 : 1;
    static const DXGI_FORMAT f[3][4] = {
        {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT},
        {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT},
        {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}};
    const int t = ctype == 1 ? 0 : ctype == 2 ? 1 : 2;
    return f[t][comps - 1];
}

enum class RootKind
{
    graphics,
    compute,
    mesh
};

// The layer's fixed layouts (d12_infra.cpp RootLayout): per stage root CBVs, then CBV/SRV/sampler
// tables; UAVs on the pixel stage (graphics) or all (compute). Mesh = graphics with the VS parameters
// at MESH and the GS parameters at AMPLIFICATION visibility (mesh.md 3.5), no input assembler.
ID3D12RootSignature* RootSig(ID3D12Device* dev, RootKind kind)
{
    std::vector<D3D12_ROOT_PARAMETER> p;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    ranges.reserve(64);
    auto table = [&](D3D12_DESCRIPTOR_RANGE_TYPE t, UINT base, UINT n, D3D12_SHADER_VISIBILITY v) {
        ranges.push_back({t, n, base, 0, 0});
        D3D12_ROOT_PARAMETER r{};
        r.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        r.DescriptorTable = {1, &ranges.back()};
        r.ShaderVisibility = v;
        p.push_back(r);
    };
    auto stage = [&](UINT rootCbvs, D3D12_SHADER_VISIBILITY v) {
        for (UINT i = 0; i < rootCbvs; ++i)
        {
            D3D12_ROOT_PARAMETER r{};
            r.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            r.Descriptor = {i, 0};
            r.ShaderVisibility = v;
            p.push_back(r);
        }
        table(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, rootCbvs, 14 - rootCbvs, v);
        table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 32, v);
        table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 16, v);
    };
    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (kind == RootKind::compute)
    {
        stage(4, D3D12_SHADER_VISIBILITY_ALL);
        table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 8, D3D12_SHADER_VISIBILITY_ALL);
    }
    else
    {
        const bool mesh = kind == RootKind::mesh;
        stage(5, mesh ? D3D12_SHADER_VISIBILITY_MESH : D3D12_SHADER_VISIBILITY_VERTEX);
        stage(5, D3D12_SHADER_VISIBILITY_PIXEL);
        table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 8, D3D12_SHADER_VISIBILITY_PIXEL);
        stage(1, mesh ? D3D12_SHADER_VISIBILITY_AMPLIFICATION : D3D12_SHADER_VISIBILITY_GEOMETRY);
        stage(1, D3D12_SHADER_VISIBILITY_HULL);
        stage(1, D3D12_SHADER_VISIBILITY_DOMAIN);
        if (!mesh)
            flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    }
    const D3D12_ROOT_SIGNATURE_DESC d{UINT(p.size()), p.data(), 0, nullptr, flags};
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err)))
    {
        std::printf("root signature: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    ID3D12RootSignature* rs = nullptr;
    dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    blob->Release();
    return rs;
}

std::string Drain(ID3D12InfoQueue* q)
{
    std::string text;
    if (!q)
        return text;
    for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; ++i)
    {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        q->GetMessage(i, m, &len);
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            text += "    #" + std::to_string(m->ID) + " " +
                std::string(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0) + "\n";
    }
    q->ClearStoredMessages();
    return text;
}

// A pipeline state stream subobject (d3dx12's layout: pointer-aligned type tag, then the value); the
// padding the alignment adds is intended.
#pragma warning(push)
#pragma warning(disable : 4324)
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Value>
struct alignas(void*) Subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Value value{};
};

struct MeshStream
{
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sampleMask;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtFormats;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsvFormat;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
};
#pragma warning(pop)

struct FixedState
{
    D3D12_BLEND_DESC blend{};
    D3D12_RASTERIZER_DESC raster{};
    D3D12_DEPTH_STENCIL_DESC depth{};

    FixedState()
    {
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (int i = 1; i < 8; ++i)
            blend.RenderTarget[i] = blend.RenderTarget[0];
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.DepthClipEnable = TRUE;
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    }
};

// Render targets from the pixel shader's SV_Target outputs.
UINT RenderTargets(const std::vector<unsigned char>& ps)
{
    UINT rts = 0;
    for (const SigElem& e : Signature(ps, "OSG1"))
    {
        if (e.sysval == 64 || e.name == "SV_Target" || e.name == "SV_TARGET")
            rts = std::max(rts, e.index + 1);
    }
    return rts;
}

bool EndsWith(const std::string& s, const char* tail)
{
    const size_t n = strlen(tail);
    return s.size() >= n && !s.compare(s.size() - n, n, tail);
}
} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2)
    {
        std::printf("usage: pso_probe <dir with pipelines.txt and blobs\\> [-adapter N]\n");
        return 2;
    }
    const std::string dir = argv[1];
    UINT adapterIndex = 0;
    for (int i = 2; i + 1 < argc; ++i)
    {
        if (!strcmp(argv[i], "-adapter"))
            adapterIndex = UINT(atoi(argv[i + 1]));
    }

    ID3D12Debug* dbg = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        dbg->EnableDebugLayer();
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return 3;
    IDXGIAdapter1* adapter = nullptr;
    factory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    ID3D12Device2* dev = nullptr;
    if (!adapter || FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
    {
        std::printf("no D3D12 device\n");
        return 3;
    }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    std::printf("adapter: %ls\n", ad.Description);

    ID3D12InfoQueue* q = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&q));
    if (q)
        q->SetMuteDebugOutput(TRUE);
    ID3D12RootSignature* gfx = RootSig(dev, RootKind::graphics);
    ID3D12RootSignature* cs = RootSig(dev, RootKind::compute);
    ID3D12RootSignature* meshRoot = RootSig(dev, RootKind::mesh);
    Drain(q);
    const FixedState fixed;

    std::ifstream list(dir + "\\pipelines.txt");
    if (!list)
    {
        std::printf("no %s\\pipelines.txt\n", dir.c_str());
        return 2;
    }
    std::string line;
    int total = 0, refused = 0, refusedGated = 0;
    std::map<std::string, std::vector<unsigned char>> cache;
    auto blob = [&](const std::string& name) -> std::vector<unsigned char>& {
        auto it = cache.find(name);
        if (it == cache.end())
            it = cache.emplace(name, ReadAll(dir + "\\blobs\\" + name)).first;
        return it->second;
    };
    while (std::getline(list, line))
    {
        if (line.empty())
            continue;
        const size_t hash = line.find(" # ");
        const std::string stages = line.substr(0, hash);
        const std::string title = hash != std::string::npos ? line.substr(hash + 3) : "";
        std::istringstream ss(stages);
        std::string tok;
        std::map<std::string, std::string> st;
        while (ss >> tok)
            st[tok.substr(0, 2)] = tok.substr(3);
        ++total;
        auto bc = [&](const char* s) -> D3D12_SHADER_BYTECODE {
            if (!st.count(s))
                return {};
            auto& b = blob(st[s]);
            return {b.data(), b.size()};
        };
        HRESULT hr;
        ID3D12PipelineState* pso = nullptr;
        if (st.count("cs"))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = cs;
            d.CS = bc("cs");
            hr = dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
        }
        else if (st.count("ms"))
        {
            MeshStream s;
            s.root.value = meshRoot;
            s.as.value = bc("as");
            s.ms.value = bc("ms");
            s.ps.value = bc("ps");
            s.blend.value = fixed.blend;
            s.sampleMask.value = ~0u;
            s.raster.value = fixed.raster;
            s.depth.value = fixed.depth;
            s.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            s.rtFormats.value.NumRenderTargets = st.count("ps") ? RenderTargets(blob(st["ps"])) : 0;
            for (UINT i = 0; i < s.rtFormats.value.NumRenderTargets; ++i)
                s.rtFormats.value.RTFormats[i] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            s.dsvFormat.value = DXGI_FORMAT_D24_UNORM_S8_UINT;
            s.sample.value = {1, 0};
            const D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(s), &s};
            hr = dev->CreatePipelineState(&sd, IID_PPV_ARGS(&pso));
        }
        else
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = gfx;
            d.VS = bc("vs");
            d.PS = bc("ps");
            d.GS = bc("gs");
            d.HS = bc("hs");
            d.DS = bc("ds");
            // Every non-system VS input from its own slot, in the signature's own format.
            const std::vector<SigElem> in = Signature(blob(st["vs"]), "ISG1");
            std::vector<D3D12_INPUT_ELEMENT_DESC> il;
            UINT slot = 0;
            for (const SigElem& e : in)
            {
                if (e.sysval)
                    continue;
                il.push_back({e.name.c_str(), e.index, FormatFor(e.ctype, e.mask), slot++ % 16, 0,
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
            }
            d.InputLayout = {il.data(), UINT(il.size())};
            d.NumRenderTargets = st.count("ps") ? RenderTargets(blob(st["ps"])) : 0;
            for (UINT i = 0; i < d.NumRenderTargets; ++i)
                d.RTVFormats[i] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            d.BlendState = fixed.blend;
            d.SampleMask = ~0u;
            d.RasterizerState = fixed.raster;
            d.DepthStencilState = fixed.depth;
            d.PrimitiveTopologyType = st.count("hs") ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.SampleDesc.Count = 1;
            hr = dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
        }
        if (pso)
            pso->Release();
        const std::string why = Drain(q);
        if (FAILED(hr))
        {
            const bool gated = EndsWith(title, "(gated)");
            ++(gated ? refusedGated : refused);
            std::printf("REFUSED%s 0x%08x %s\n%s", gated ? " (gated)" : "", unsigned(hr), title.c_str(), why.c_str());
        }
    }
    std::printf("%d pipelines, %d refused, %d refused behind a legacy switch (gated)\n", total, refused, refusedGated);
    return refused ? 1 : 0;
}
