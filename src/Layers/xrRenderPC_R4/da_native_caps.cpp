// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// CHW::native and the DirectX 12 keys of r__caps (da_native_caps.h). One caps hook fills every new
// UpscalerCaps field from the lanes' query functions (PLAN.md C28): no feature registers its own.

#include "stdafx.h"

#include "da_native_caps.h"
#include "da_upscaler.h"
#include "da_dxc.h"
#include "da_vrs.h"
#include "da_gpu_trees.h"
#include "da_dstorage.h"
#include "da_rt.h"

#if defined(XR_D3D12)
#include "Layers/xrRenderDX12/d12_api.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
DaNativeProbeReader g_da_probe_reader = nullptr;

// D3D_SHADER_MODEL (0x60..0x69) as the two digits r__caps prints.
u32 da_shader_model_digits(u32 model) { return model >= 0x60 ? 60 + (model - 0x60) : 50; }

void da_native_caps_hook(UpscalerCaps& caps)
{
    const CHWNativeCaps& n = HW.native;
    const u32 model = n.shaderModel && n.dxcShaderModel ? std::min(n.shaderModel, n.dxcShaderModel) : 0;
    caps.sm = da_shader_model_digits(model);
    caps.rt_tier = n.raytracingTier;
    caps.vrs_tier = n.vrsTier;
    caps.vrs_tile = n.vrsTier >= 2 ? n.vrsTileSize : 0;
    caps.ms = n.meshShaderTier >= 10 && caps.sm >= 65;
    caps.wave = n.waveOps && caps.sm >= 60 ? n.waveLanesMax : 0;
    caps.fp16 = n.native16Bit && caps.sm >= 62;
    caps.eb = n.enhancedBarriers;
    caps.acq = n.asyncCompute;
    caps.rt = da_rt_available();
    caps.vrs = da_vrs_supported();
    caps.dxc = da_dxc_active();
    caps.gd = da_gpu_trees_live();
    caps.dstorage = da_dstorage_available();
}

const struct da_native_caps_registrar
{
    da_native_caps_registrar() { da_upscaler_register_caps_hook(&da_native_caps_hook); }
} s_da_native_caps;

void da_native_apply_masks(CHWNativeCaps& caps)
{
    const u32 disabled = da_native_disabled_features();
    if (disabled & DaNativeDisableRT)
        caps.raytracingTier = 0;
    if (disabled & DaNativeDisableVRS)
    {
        caps.vrsTier = 0;
        caps.vrsTileSize = 0;
        caps.vrsAdditionalRates = false;
    }
    if (disabled & DaNativeDisableMS)
        caps.meshShaderTier = 0;
    if (disabled & DaNativeDisableAsync)
        caps.asyncCompute = false;
    if (disabled & DaNativeDisableSM6)
    {
        caps.shaderModel = 0;
        caps.dxcShaderModel = 0;
    }
}
} // namespace

u32 da_native_disabled_features()
{
    static const u32 disabled = [] {
        u32 mask = 0;
        if (strstr(Core.Params, "-dx12_no_rt"))
            mask |= DaNativeDisableRT;
        if (strstr(Core.Params, "-dx12_no_vrs"))
            mask |= DaNativeDisableVRS;
        if (strstr(Core.Params, "-dx12_no_ms"))
            mask |= DaNativeDisableMS;
        if (strstr(Core.Params, "-dx12_no_async"))
            mask |= DaNativeDisableAsync;
        if (strstr(Core.Params, "-dx12_no_sm6"))
            mask |= DaNativeDisableSM6;
        return mask;
    }();
    return disabled;
}

void da_native_caps_register_probe_reader(DaNativeProbeReader reader) { g_da_probe_reader = reader; }

void da_native_caps_on_device(ID3D11Device* device)
{
    CHWNativeCaps caps{};
#if defined(XR_D3D12)
    if (const xrd3d12::NativeCaps* native = device ? xrd3d12::GetNativeCaps(device) : nullptr)
    {
        caps.raytracingTier = native->raytracingTier;
        caps.vrsTier = native->vrsTier;
        caps.vrsTileSize = native->vrsTileSize;
        caps.vrsAdditionalRates = native->vrsAdditionalRates;
        caps.meshShaderTier = native->meshShaderTier;
        // 0x51 is the layer's "no Shader Model 6".
        caps.shaderModel = native->shaderModel >= 0x60 ? native->shaderModel : 0;
        caps.dxcShaderModel = native->dxcShaderModel;
        caps.waveOps = native->waveOps;
        caps.waveLanesMin = native->waveLanesMin;
        caps.waveLanesMax = native->waveLanesMax;
        caps.native16Bit = native->native16Bit;
        caps.heapIndexing = native->heapIndexing;
        caps.enhancedBarriers = native->enhancedBarriers;
        caps.executeIndirectTier = native->executeIndirectTier;
        caps.asyncCompute = native->asyncCompute;
    }
#else
    UNUSED(device);
    if (g_da_probe_reader)
        g_da_probe_reader(caps);
#endif
    da_native_apply_masks(caps);
    HW.native = caps;
}
} // namespace xray::render::RENDER_NAMESPACE
