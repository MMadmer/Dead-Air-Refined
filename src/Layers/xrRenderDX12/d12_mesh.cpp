// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Mesh shader draws on the D3D11 path of xrD3D12 (docs/dead-air/dx12-features/mesh.md 3.5; lane
// `mesh`). Reached through hooks in the root signatures, the pipeline cache and the context
// (d12_layer.h, [dx12:mesh]). A mesh shader rides the vertex shader slot, an amplification shader
// the geometry shader slot: bindings, trackers and deferred-context hand-off are the draw path's.
#include "d12_layer.h"

#include <array>
#include <cstring>

namespace xrd3d12
{
namespace
{
// Mesh pipelines never share a library name with a vertex pipeline or a native stream.
constexpr u64 kMeshKeySalt = 0x4d65736850534f32ull;
// DispatchMesh group count limits (D3D12 spec: each dimension, and their product).
constexpr UINT kMaxGroupsPerDimension = 65535;
constexpr u64 kMaxGroups = 1ull << 22;

// ---- root signature ---------------------------------------------------------------------------

// The graphics root signature's parameters (d12_infra.cpp, GfxRoot) with the VS parameters visible
// to the mesh shader and the GS parameters to the amplification shader.
struct MeshRootLayout
{
    std::array<D3D12_DESCRIPTOR_RANGE1, 32> ranges{};
    std::array<D3D12_ROOT_PARAMETER1, 32> params{};
    u32 rangeCount = 0;

    void RootCbv(u32 index, u32 reg, D3D12_SHADER_VISIBILITY visibility)
    {
        D3D12_ROOT_PARAMETER1& p = params[index];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        p.ShaderVisibility = visibility;
    }

    void Table(u32 index, D3D12_DESCRIPTOR_RANGE_TYPE type, u32 base, u32 count, D3D12_SHADER_VISIBILITY visibility)
    {
        D3D12_DESCRIPTOR_RANGE1& r = ranges[rangeCount++];
        r.RangeType = type;
        r.NumDescriptors = count;
        r.BaseShaderRegister = base;
        r.RegisterSpace = 0;
        r.Flags = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
            ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
            : D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        r.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER1& p = params[index];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = &r;
        p.ShaderVisibility = visibility;
    }

    void Stage(u32 cbv0, u32 rootCbvs, u32 cbvTable, u32 srv, u32 smp, D3D12_SHADER_VISIBILITY visibility)
    {
        for (u32 i = 0; i < rootCbvs; ++i)
            RootCbv(cbv0 + i, i, visibility);
        Table(cbvTable, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, rootCbvs, kMaxCBs - rootCbvs, visibility);
        Table(srv, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, kMaxSRVs, visibility);
        Table(smp, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, kMaxSamplers, visibility);
    }
};

ID3D12RootSignature* SerializeMeshRoot(ID3D12Device* device, const MeshRootLayout& layout, bool version11)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = E_NOTIMPL;
    if (version11 && g_d3d12.SerializeVersionedRootSignature)
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = GfxRoot::Count;
        desc.Desc_1_1.pParameters = layout.params.data();
        desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        hr = g_d3d12.SerializeVersionedRootSignature(&desc, &blob, &errors);
    }
    else if (g_d3d12.SerializeRootSignature)
    {
        std::array<D3D12_DESCRIPTOR_RANGE, 32> ranges{};
        std::array<D3D12_ROOT_PARAMETER, 32> params{};
        for (u32 i = 0; i < GfxRoot::Count; ++i)
        {
            const D3D12_ROOT_PARAMETER1& src = layout.params[i];
            D3D12_ROOT_PARAMETER& dst = params[i];
            dst.ParameterType = src.ParameterType;
            dst.ShaderVisibility = src.ShaderVisibility;
            if (src.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            {
                const D3D12_DESCRIPTOR_RANGE1& r = src.DescriptorTable.pDescriptorRanges[0];
                ranges[i] = {r.RangeType, r.NumDescriptors, r.BaseShaderRegister, r.RegisterSpace, r.OffsetInDescriptorsFromTableStart};
                dst.DescriptorTable.NumDescriptorRanges = 1;
                dst.DescriptorTable.pDescriptorRanges = &ranges[i];
            }
            else
            {
                dst.Descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
                dst.Descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
            }
        }
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = GfxRoot::Count;
        desc.pParameters = params.data();
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        hr = g_d3d12.SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors);
    }
    if (errors)
    {
        Log("mesh root signature: %.*s", int(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    if (FAILED(hr) || !blob)
    {
        if (blob)
            blob->Release();
        return nullptr;
    }
    ID3D12RootSignature* signature = nullptr;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature));
    blob->Release();
    if (FAILED(hr))
        return nullptr;
    signature->SetName(L"xrD3D12 mesh root signature");
    return signature;
}

// ---- pipeline state stream --------------------------------------------------------------------

// One subobject of a pipeline state stream: pointer-aligned, type first (the d3dx12 layout).
// Plain data: the stream is zero-filled as a whole, so the padding hashes the same in every
// process (the pipeline library compares it).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T>
struct alignas(void*) Subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;

    void Set(const T& v)
    {
        type = Type;
        value = v;
    }
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
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
};

// D3D11's defaults for a state object left null (the same as the vertex pipelines, d12_infra.cpp).
D3D12_BLEND_DESC DefaultBlend()
{
    D3D12_BLEND_DESC desc{};
    for (D3D12_RENDER_TARGET_BLEND_DESC& rt : desc.RenderTarget)
    {
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = UINT8(D3D12_COLOR_WRITE_ENABLE_ALL);
    }
    return desc;
}

D3D12_RASTERIZER_DESC DefaultRaster()
{
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_BACK;
    desc.DepthClipEnable = TRUE;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

D3D12_DEPTH_STENCIL_DESC DefaultDepth()
{
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.BackFace = desc.FrontFace;
    return desc;
}

bool HasStencil(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        format == DXGI_FORMAT_R24G8_TYPELESS || format == DXGI_FORMAT_R32G8X24_TYPELESS;
}

D3D12_SHADER_BYTECODE Bytecode(const Shader* shader)
{
    if (!shader || shader->bytecode.empty())
        return {};
    return {shader->bytecode.data(), shader->bytecode.size()};
}

std::string NameOf(const Shader* shader)
{
    std::string name = shader ? ShaderName(shader) : std::string();
    return name.empty() ? "-" : name;
}

// The message doubles as the once-key.
void Once(const char* message)
{
    LogOnce(message, message);
}
} // namespace

// ================================================================================================
// Root signature
// ================================================================================================

void RootSignatures::InitMesh(ID3D12Device* device, bool version11)
{
    mesh = nullptr;
    if (!device)
        return;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))) ||
        options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
    {
        return;
    }
    MeshRootLayout layout;
    layout.Stage(GfxRoot::VS_CBV0, kRootCBVsVS, GfxRoot::VS_CBV_TABLE, GfxRoot::VS_SRV, GfxRoot::VS_SMP, D3D12_SHADER_VISIBILITY_MESH);
    layout.Stage(GfxRoot::PS_CBV0, kRootCBVsPS, GfxRoot::PS_CBV_TABLE, GfxRoot::PS_SRV, GfxRoot::PS_SMP, D3D12_SHADER_VISIBILITY_PIXEL);
    layout.Table(GfxRoot::PS_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, kMaxUAVs, D3D12_SHADER_VISIBILITY_PIXEL);
    layout.Stage(GfxRoot::GS_CBV0, kRootCBVsOther, GfxRoot::GS_CBV_TABLE, GfxRoot::GS_SRV, GfxRoot::GS_SMP,
        D3D12_SHADER_VISIBILITY_AMPLIFICATION);
    // Hull and domain keep their parameters so every index stays the graphics one (ApplyStage).
    layout.Stage(GfxRoot::HS_CBV0, kRootCBVsOther, GfxRoot::HS_CBV_TABLE, GfxRoot::HS_SRV, GfxRoot::HS_SMP, D3D12_SHADER_VISIBILITY_HULL);
    layout.Stage(GfxRoot::DS_CBV0, kRootCBVsOther, GfxRoot::DS_CBV_TABLE, GfxRoot::DS_SRV, GfxRoot::DS_SMP, D3D12_SHADER_VISIBILITY_DOMAIN);
    mesh = SerializeMeshRoot(device, layout, version11);
    if (!mesh && version11)
        mesh = SerializeMeshRoot(device, layout, false);
    Log("mesh shaders: tier %u, root signature %s", unsigned(options7.MeshShaderTier), mesh ? "ready" : "refused (mesh draws off)");
}

void RootSignatures::ShutdownMesh()
{
    if (mesh)
        mesh->Release();
    mesh = nullptr;
}

// ================================================================================================
// Pipelines
// ================================================================================================

ID3D12PipelineState* PipelineCache::GraphicsMesh(const GraphicsPsoDesc& desc, u64 hash)
{
    ID3D12RootSignature* root = m_device->rootSignatures.mesh;
    const Shader* ms = desc.vs;
    const Shader* as = desc.gs;
    // Only mesh + optional amplification + pixel: tessellation and a geometry shader do not exist here.
    if (!root || !ms || ms->kind != kKindMesh || (as && as->kind != kKindAmplification) || desc.hs || desc.ds)
    {
        LogOnce(reinterpret_cast<const void*>(static_cast<uintptr_t>(hash ^ kMeshKeySalt)),
            "! mesh pipeline %016llx refused: %s (ms '%s', as '%s')", static_cast<unsigned long long>(hash),
            !root ? "no mesh root signature" : (as && as->kind != kKindAmplification) ? "the geometry slot holds no amplification shader"
                                                                                       : "tessellation stages bound",
            NameOf(ms).c_str(), NameOf(as).c_str());
        return Insert(hash, nullptr, nullptr);
    }

    MeshStream s;
    std::memset(&s, 0, sizeof(s));
    s.root.Set(root);
    s.as.Set(Bytecode(as));
    s.ms.Set(Bytecode(ms));
    s.ps.Set(Bytecode(desc.ps));
    s.blend.Set(desc.blend ? desc.blend->desc12 : DefaultBlend());
    s.sampleMask.Set(desc.sampleMask);
    s.raster.Set(desc.raster ? desc.raster->desc12 : DefaultRaster());
    D3D12_DEPTH_STENCIL_DESC depth = desc.depth ? desc.depth->desc12 : DefaultDepth();
    // The rules of the vertex pipelines: no depth test without a depth buffer, no stencil without a plane.
    if (desc.dsvFormat == DXGI_FORMAT_UNKNOWN)
        depth.DepthEnable = FALSE;
    if (!HasStencil(desc.dsvFormat))
        depth.StencilEnable = FALSE;
    s.depth.Set(depth);
    // Formats past NumRenderTargets must stay UNKNOWN (E_INVALIDARG otherwise).
    D3D12_RT_FORMAT_ARRAY rtvs{};
    rtvs.NumRenderTargets = (std::min)(desc.numRTVs, kMaxRTVs);
    for (u32 i = 0; i < rtvs.NumRenderTargets; ++i)
        rtvs.RTFormats[i] = desc.rtvFormats[i];
    s.rtvs.Set(rtvs);
    s.dsv.Set(desc.dsvFormat);
    s.sample.Set(desc.sample);

    // Memory only: the pipeline library refuses these streams on the next start (debug layer #972
    // "desc does not match" for every AS+MS+PS pipeline stored the run before, whatever is zeroed),
    // and the driver's own shader cache makes the re-creation cheap.
    D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(s), &s};
    ID3D12PipelineState* pso = NativeStream(HashCombine(hash, kMeshKeySalt), stream, false);
    if (!pso)
    {
        LogOnce(reinterpret_cast<const void*>(static_cast<uintptr_t>(hash ^ kMeshKeySalt ^ 1)),
            "! mesh pipeline %016llx refused by the driver: ms '%s', as '%s', ps '%s', %u RTVs, DSV %d",
            static_cast<unsigned long long>(hash), NameOf(ms).c_str(), NameOf(as).c_str(), NameOf(desc.ps).c_str(), desc.numRTVs,
            int(desc.dsvFormat));
        return Insert(hash, nullptr, nullptr);
    }
    // NativeStream keeps its own reference under its key; the engine key gets one of its own.
    pso->AddRef();
    return Insert(hash, pso, nullptr);
}

// ================================================================================================
// Draws
// ================================================================================================

void Device::MeshShutdown()
{
}

bool Context::MeshRefuseDraw()
{
    Once("a draw with a mesh shader in the vertex shader slot skipped (DispatchMesh draws it)");
    return true;
}

bool Context::PrepareMesh()
{
    const NativeCaps& caps = m_device->caps;
    if (!caps.meshShaderTier || !m_device->rootSignatures.mesh)
    {
        Once("DispatchMesh without mesh shader support skipped");
        return false;
    }
    const Shader* ms = stages[u32(Stage::VS)].shader;
    const Shader* as = stages[u32(Stage::GS)].shader;
    if (!ms || ms->kind != kKindMesh)
    {
        Once("DispatchMesh without a mesh shader in the vertex shader slot skipped");
        return false;
    }
    if ((as && as->kind != kKindAmplification) || stages[u32(Stage::HS)].shader || stages[u32(Stage::DS)].shader)
    {
        Once("DispatchMesh with a geometry or tessellation shader bound skipped");
        return false;
    }
    return PrepareGraphics(true);
}

void Context::MeshDestroy()
{
}

bool DispatchMesh(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    if (!context || !x || !y || !z)
        return false;
    auto* c = static_cast<Context*>(context);
    if (c->m_inNative)
    {
        Once("DispatchMesh inside a native section skipped");
        return false;
    }
    if (x > kMaxGroupsPerDimension || y > kMaxGroupsPerDimension || z > kMaxGroupsPerDimension || u64(x) * y * z > kMaxGroups)
    {
        Once("DispatchMesh beyond the group count limits skipped");
        return false;
    }
    try
    {
        if (!c->PrepareMesh())
            return false;
        // PrepareGraphics may have moved to a new list (async guard join): take the current one.
        ID3D12GraphicsCommandList6* list = c->List6();
        if (!list)
        {
            Once("DispatchMesh without ID3D12GraphicsCommandList6 skipped");
            return false;
        }
        list->DispatchMesh(x, y, z);
        c->device()->Count(Device::StatMeshDispatches);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        Once("DispatchMesh skipped: out of memory");
        return false;
    }
}
} // namespace xrd3d12
