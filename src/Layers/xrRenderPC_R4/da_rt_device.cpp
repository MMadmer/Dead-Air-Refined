// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Ray tracing device part (docs/dead-air/dx12-features/dxr.md 3.2, PLAN.md C8): DXR availability, the
// RT root signature and pipelines through the layer's caches, the native sections of the passes with
// their per-pass descriptor tables and constants, and the probe that proves the chain at start.

#include "stdafx.h"

#include "da_rt_device.h"
#include "da_dxc.h"
#include "da_native_caps.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
struct DaRtDevice
{
    bool ready{};
    string256 reason{};
#if defined(XR_D3D12)
    ID3D12RootSignature* root{};
    // Bound where a pass has no scene: an empty acceleration structure (every ray misses).
    ID3D11ShaderResourceView* empty_scene{};
    // Stand in for the table SRVs and the raw UAV a pass does not use (root descriptors are never null).
    ID3D12Resource* empty_table{};
    ID3D12Resource* empty_uav{};
    struct Pipeline
    {
        xr_string key;
        ID3D12PipelineState* state{}; // device-owned; null = failed
    };
    xr_vector<Pipeline> pipelines;
#endif
};
DaRtDevice g_rt_device;

void da_rt_refuse(pcstr format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(g_rt_device.reason, sizeof(g_rt_device.reason), format, args);
    va_end(args);
    g_rt_device.ready = false;
}

#if defined(XR_D3D12)
// What the probe writes where its ray misses the empty scene, plus the input texel.
constexpr u32 da_rt_probe_magic = 0x52540000u;
constexpr u32 da_rt_probe_size = 8;

ID3D12RootSignature* da_rt_create_root_signature()
{
    constexpr D3D12_DESCRIPTOR_RANGE_FLAGS pass_flags =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    const D3D12_DESCRIPTOR_RANGE1 scene{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    const D3D12_DESCRIPTOR_RANGE1 inputs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, da_rt_max_inputs, 0, 0, pass_flags, 0};
    const D3D12_DESCRIPTOR_RANGE1 outputs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, da_rt_max_outputs, 0, 0, pass_flags, 0};
    // Both unbounded tables start at the bindless base (NativeScope::bindless).
    const D3D12_DESCRIPTOR_RANGE1 textures{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 100, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    const D3D12_DESCRIPTOR_RANGE1 buffers{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 101, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};

    D3D12_ROOT_PARAMETER1 params[DaRtRootCount]{};
    const auto table = [&](DaRtRoot index, const D3D12_DESCRIPTOR_RANGE1& range) {
        params[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[index].DescriptorTable = {1, &range};
    };
    const auto root_srv = [&](DaRtRoot index, UINT reg) {
        params[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[index].Descriptor = {reg, 3, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    };
    const auto root_uav = [&](DaRtRoot index, UINT reg) {
        params[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[index].Descriptor = {reg, 3, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    };
    params[DaRtRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[DaRtRootConstants].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    table(DaRtRootScene, scene);
    table(DaRtRootInputs, inputs);
    table(DaRtRootOutputs, outputs);
    table(DaRtRootTextures, textures);
    table(DaRtRootBuffers, buffers);
    root_srv(DaRtRootTable0, 0);
    root_srv(DaRtRootTable1, 1);
    root_srv(DaRtRootTable2, 2);
    root_srv(DaRtRootTable3, 3);
    root_uav(DaRtRootRawOutput, 0);
    for (D3D12_ROOT_PARAMETER1& p : params)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    const auto sampler = [&](UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address) {
        D3D12_STATIC_SAMPLER_DESC& s = samplers[reg];
        s.Filter = filter;
        s.AddressU = s.AddressV = s.AddressW = address;
        s.MaxAnisotropy = 1;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = reg;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    sampler(0, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    samplers[0].MaxAnisotropy = 4;
    sampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler(3, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[3].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = DaRtRootCount;
    desc.Desc_1_1.pParameters = params;
    desc.Desc_1_1.NumStaticSamplers = UINT(std::size(samplers));
    desc.Desc_1_1.pStaticSamplers = samplers;
    return xrd3d12::RootSignature(HW.pDevice, desc, L"rt passes");
}

// The whole chain once at start: the probe traces the empty scene from a native section with every
// kind of binding and adds the input texel to its miss value; the result is read back.
bool da_rt_probe()
{
    ID3D12PipelineState* pipeline = da_rt_pipeline("rt/rt_probe.hlsl", "main");
    if (!pipeline)
    {
        da_rt_refuse("the probe pipeline rt/rt_probe.hlsl could not be built");
        return false;
    }
    constexpr u32 texel_count = da_rt_probe_size * da_rt_probe_size;
    u32 texels[texel_count];
    for (u32 i = 0; i < texel_count; ++i)
        texels[i] = i;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = da_rt_probe_size;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{texels, da_rt_probe_size * 4, 0};
    ID3D11Texture2D* input = nullptr;
    ID3D11Texture2D* output = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11ShaderResourceView* input_view = nullptr;
    ID3D11UnorderedAccessView* output_view = nullptr;
    const auto release = [&] {
        for (IUnknown* object : std::initializer_list<IUnknown*>{input_view, output_view, input, output, staging})
        {
            if (object)
                object->Release();
        }
    };
    bool created = SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, &data, &input)) &&
        SUCCEEDED(HW.pDevice->CreateShaderResourceView(input, nullptr, &input_view));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    created = created && SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, &output)) &&
        SUCCEEDED(HW.pDevice->CreateUnorderedAccessView(output, nullptr, &output_view));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    created = created && SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, &staging));
    if (!created)
    {
        release();
        da_rt_refuse("the probe's textures could not be created");
        return false;
    }

    // x: miss value, y: a bindless index the probe reads only when it is valid (never here).
    const u32 constants[4] = {da_rt_probe_magic, xrd3d12::kBindlessInvalid, 0, 0};
    DaRtDispatch dispatch;
    dispatch.pipeline = pipeline;
    dispatch.constants = constants;
    dispatch.constants_size = sizeof(constants);
    dispatch.inputs = &input_view;
    dispatch.input_count = 1;
    dispatch.outputs = &output_view;
    dispatch.output_count = 1;
    bool dispatched;
    {
        DaRtSection section;
        dispatched = section && section.dispatch(dispatch);
    }
    ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
    u32 wrong = texel_count, first_wrong = 0;
    if (dispatched)
    {
        context->CopyResource(staging, output);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            wrong = 0;
            for (u32 y = 0; y < da_rt_probe_size; ++y)
            {
                const u32* row = reinterpret_cast<const u32*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
                for (u32 x = 0; x < da_rt_probe_size; ++x)
                {
                    const u32 expected = da_rt_probe_magic + texels[y * da_rt_probe_size + x];
                    if (row[x] != expected && !wrong++)
                        first_wrong = row[x];
                }
            }
            context->Unmap(staging, 0);
        }
    }
    release();
    if (!dispatched)
    {
        da_rt_refuse("the probe could not be dispatched");
        return false;
    }
    if (wrong)
    {
        da_rt_refuse("the probe read back %u wrong texels of %u (first 0x%08X)", wrong, texel_count, first_wrong);
        return false;
    }
    return true;
}
#endif
} // namespace

bool da_rt_device_ready() { return g_rt_device.ready; }

pcstr da_rt_device_reason() { return g_rt_device.ready ? "" : g_rt_device.reason; }

#if defined(XR_D3D12)
ID3D12RootSignature* da_rt_root_signature() { return g_rt_device.root; }

ID3D12PipelineState* da_rt_pipeline(pcstr file, pcstr entry, const DaDxcDefine* defines, u32 define_count)
{
    if (!g_rt_device.root)
        return nullptr;
    xr_string key = xr_string(file) + ":" + entry;
    for (u32 i = 0; i < define_count; ++i)
        key += xr_string(" ") + defines[i].name + "=" + (defines[i].value ? defines[i].value : "1");
    for (const DaRtDevice::Pipeline& p : g_rt_device.pipelines)
    {
        if (p.key == key)
            return p.state;
    }
    DaRtDevice::Pipeline& pipeline = g_rt_device.pipelines.emplace_back();
    pipeline.key = key;
    DaDxcNative request;
    request.path = file;
    request.entry = entry;
    request.target = "cs_6_5";
    request.defines = defines;
    request.define_count = define_count;
    xr_vector<u8> dxil;
    if (!da_dxc_compile_native(request, dxil))
    {
        Msg("! [rt] %s does not compile", key.c_str());
        return nullptr;
    }
    wchar_t name[128];
    swprintf_s(name, L"rt %hs:%hs", file, entry);
    pipeline.state = xrd3d12::ComputePipeline(HW.pDevice, g_rt_device.root, dxil.data(), dxil.size(), name);
    if (!pipeline.state)
        Msg("! [rt] the pipeline of %s was refused", key.c_str());
    return pipeline.state;
}

DaRtSection::DaRtSection()
{
    m_context = HW.get_context(CHW::IMM_CTX_ID);
    if (!g_rt_device.root || !m_context || !xrd3d12::BeginNative(m_context, m_scope))
        return;
    m_open = true;
    m_scope.list->SetComputeRootSignature(g_rt_device.root);
    m_scope.list->SetComputeRootDescriptorTable(DaRtRootTextures, m_scope.bindless);
    m_scope.list->SetComputeRootDescriptorTable(DaRtRootBuffers, m_scope.bindless);
}

DaRtSection::~DaRtSection()
{
    if (m_open)
        xrd3d12::EndNative(m_context, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
}

bool DaRtSection::dispatch(const DaRtDispatch& d)
{
    if (!m_open || !d.pipeline || d.input_count > da_rt_max_inputs || d.output_count > da_rt_max_outputs ||
        d.constants_size > da_rt_max_constants)
        return false;
    for (u32 i = 0; i < d.input_count; ++i)
    {
        if (d.inputs[i])
            xrd3d12::RequireView(m_context, d.inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (u32 i = 0; i < d.output_count; ++i)
    {
        if (d.outputs[i])
            xrd3d12::RequireView(m_context, d.outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Scene, inputs and outputs in one span: the tables cover their full ranges, unused slots are
    // never read (volatile descriptors).
    constexpr u32 inputs_at = 1, outputs_at = inputs_at + da_rt_max_inputs, span_size = outputs_at + da_rt_max_outputs;
    const xrd3d12::DescriptorSpan span = xrd3d12::AllocateDescriptors(m_context, span_size);
    const u32 constants_size = (std::max(d.constants_size, 16u) + 255u) & ~255u;
    const xrd3d12::UploadSpan constants =
        xrd3d12::AllocateUpload(m_context, constants_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!span.count || !constants.cpu)
        return false;
    ID3D12Device* device = xrd3d12::NativeDevice(HW.pDevice);
    const auto copy = [&](u32 slot, ID3D11View* view) {
        const D3D12_CPU_DESCRIPTOR_HANDLE target{span.cpu.ptr + SIZE_T(slot) * span.increment};
        device->CopyDescriptorsSimple(1, target, xrd3d12::NativeDescriptor(view), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    };
    const auto table = [&](u32 slot) { return D3D12_GPU_DESCRIPTOR_HANDLE{span.gpu.ptr + UINT64(slot) * span.increment}; };
    copy(0, d.scene ? d.scene : g_rt_device.empty_scene);
    for (u32 i = 0; i < d.input_count; ++i)
    {
        if (d.inputs[i])
            copy(inputs_at + i, d.inputs[i]);
    }
    for (u32 i = 0; i < d.output_count; ++i)
    {
        if (d.outputs[i])
            copy(outputs_at + i, d.outputs[i]);
    }
    std::memset(constants.cpu, 0, constants_size);
    if (d.constants_size)
        std::memcpy(constants.cpu, d.constants, d.constants_size);

    const D3D12_GPU_VIRTUAL_ADDRESS empty = g_rt_device.empty_table->GetGPUVirtualAddress();
    ID3D12GraphicsCommandList* list = m_scope.list;
    list->SetPipelineState(d.pipeline);
    list->SetComputeRootConstantBufferView(DaRtRootConstants, constants.gpu);
    list->SetComputeRootDescriptorTable(DaRtRootScene, table(0));
    list->SetComputeRootDescriptorTable(DaRtRootInputs, table(inputs_at));
    list->SetComputeRootDescriptorTable(DaRtRootOutputs, table(outputs_at));
    for (u32 i = 0; i < da_rt_max_tables; ++i)
        list->SetComputeRootShaderResourceView(DaRtRootTable0 + i, d.tables[i] ? d.tables[i] : empty);
    list->SetComputeRootUnorderedAccessView(
        DaRtRootRawOutput, d.raw_output ? d.raw_output : g_rt_device.empty_uav->GetGPUVirtualAddress());
    list->Dispatch(d.groups_x, d.groups_y, d.groups_z);
    return true;
}

void DaRtSection::uav_barrier(ID3D11Resource* resource)
{
    if (m_open)
        xrd3d12::UavBarrier(m_context, resource);
}

void DaRtSection::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!m_open || !resource || before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    m_scope.list->ResourceBarrier(1, &barrier);
}

void DaRtSection::native_uav_barrier(ID3D12Resource* resource)
{
    if (!m_open)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    m_scope.list->ResourceBarrier(1, &barrier);
}
#endif

void da_rt_device_create()
{
    da_rt_device_destroy();
#if defined(XR_D3D12)
    const CHWNativeCaps& caps = HW.native;
    const u32 model = da_dxc_native_model();
    CTimer timer;
    timer.Start();
    g_rt_device.ready = true;
    if (da_native_disabled_features() & DaNativeDisableRT)
        da_rt_refuse("switched off by -dx12_no_rt");
    else if (caps.raytracingTier < 11)
        da_rt_refuse("the GPU or its driver has no inline ray tracing (DXR tier %u.%u, 1.1 needed)", caps.raytracingTier / 10,
            caps.raytracingTier % 10);
    else if (!model)
        da_rt_refuse("no shader model 6 compiler (DXC)%s", da_native_disabled_features() & DaNativeDisableSM6 ? ": -dx12_no_sm6" : "");
    else if (model < 0x65)
        da_rt_refuse("native passes reach shader model %x.%x only, 6.5 needed", model >> 4, model & 0xF);
    else if (!xrd3d12::NativeDevice5(HW.pDevice))
        da_rt_refuse("the Direct3D 12 runtime has no ID3D12Device5");
    else if (!(g_rt_device.root = da_rt_create_root_signature()))
        da_rt_refuse("the root signature was refused");
    else if (FAILED(xrd3d12::CreateAccelerationStructureView(HW.pDevice, nullptr, &g_rt_device.empty_scene)) ||
        FAILED(xrd3d12::CreateNativeBuffer(HW.pDevice, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COMMON, L"rt empty table", &g_rt_device.empty_table)) ||
        FAILED(xrd3d12::CreateNativeBuffer(HW.pDevice, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, L"rt empty raw output", &g_rt_device.empty_uav)))
        da_rt_refuse("the empty scene could not be created");
    // The pass pipelines come with the targets of the first effect switched on (da_rt_targets_create).
    else
        da_rt_probe();

    if (g_rt_device.ready)
    {
        Msg("* [rt] ready: DXR tier %u.%u, passes at cs_6_5 through DXC, probe ok (%.1f ms)", caps.raytracingTier / 10,
            caps.raytracingTier % 10, timer.GetElapsed_sec() * 1000.f);
        if (RImplementation.o.msaa)
            Msg("* [rt] inactive with MSAA: the passes read single-sampled depth");
    }
    else
    {
        // Capability gaps are no errors; a chain that should have worked is.
        const bool expected = caps.raytracingTier < 11 || model < 0x65 || (da_native_disabled_features() & DaNativeDisableRT);
        Msg("%c [rt] unavailable: %s", expected ? '*' : '!', g_rt_device.reason);
        if (ps_r__rt_reflections || ps_r__rt_shadows || ps_r__rtao)
        {
            Msg("! [rt] r__rt_reflections, r__rt_shadows and r__rtao are switched off");
            ps_r__rt_reflections = ps_r__rt_shadows = ps_r__rtao = 0;
        }
    }
#else
    da_rt_refuse("ray tracing needs DirectX 12");
    static bool reported = false;
    if (!reported && (ps_r__rt_reflections || ps_r__rt_shadows || ps_r__rtao))
    {
        Msg("~ [rt] ray tracing needs DirectX 12: the RTX settings are kept for it");
        reported = true;
    }
#endif
}

void da_rt_device_destroy()
{
    da_rt_scene_release();
    da_rt_passes_destroy();
#if defined(XR_D3D12)
    if (g_rt_device.empty_scene)
        g_rt_device.empty_scene->Release();
    if (g_rt_device.empty_table)
        xrd3d12::ReleaseNative(HW.pDevice, g_rt_device.empty_table);
    if (g_rt_device.empty_uav)
        xrd3d12::ReleaseNative(HW.pDevice, g_rt_device.empty_uav);
#endif
    g_rt_device = {};
}
} // namespace xray::render::RENDER_NAMESPACE
