// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Variable rate shading: mode, per-pass table and scopes (da_vrs.h). The rate image itself and its
// inputs are r4_rendertarget_phase_vrs.cpp.

#include "stdafx.h"

#include "da_vrs.h"

#if defined(XR_NATIVE)
#include "Layers/xrRenderDX11/dx11Native.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
int ps_r__vrs_passes = (1 << int(VrsPass::Count)) - 1;
float ps_r__vrs_edge = 1.f;

namespace
{
#if defined(XR_NATIVE)
// The cap of a pass per mode (conservative, aggressive): the image rate is limited to it through
// the MIN combiner. Without an image the pass shades flat at the cap (vrs.md 3.3). combine_1 is not a
// VRS pass: it failed the aggressive quality gate (vrs.md 9.3: blockiness +92 %, limit 15 %); nor are
// the sky and the cloud deck: they failed the turn gate (vrs.md 13.7).
struct VrsPassRule
{
    u8 cap[2];
    bool image;
};

constexpr VrsPassRule vrs_rules[] = {
    /* SunLight */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_4X4}, true},
    /* SunVolumetric */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_4X4}, true},
    /* Lights */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_4X4}, true},
    /* LightsVolumetric */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_4X4}, true},
    /* PuddleRefl */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X2}, true},
    /* Forward */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_4X4}, true},
    /* VolumetricCombine */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X2}, true},
    /* SunshaftsBlur */ {{D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X2}, false},
};
static_assert(std::size(vrs_rules) == size_t(VrsPass::Count));

bool da_vrs_apply(CBackend& cmd_list, VrsPass pass)
{
    if (!(ps_r__vrs_passes & (1 << int(pass))))
        return false;
    const int mode = da_vrs_frame_mode();
    if (mode <= 0)
        return false;
    const VrsPassRule& rule = vrs_rules[size_t(pass)];
    const u8 cap = rule.cap[mode - 1];
    if (!rule.image)
    {
        cmd_list.set_ShadingRate(cap, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, nullptr);
        return true;
    }
    ID3DTexture2D* image = da_vrs_frame_image();
    if (!image)
        return false;
    cmd_list.set_ShadingRate(cap, D3D12_SHADING_RATE_COMBINER_MIN, image);
    return true;
}
#endif
} // namespace

int da_vrs_preset_mode()
{
#if defined(XR_NATIVE)
    return ps_r__vrs;
#else
    return 0;
#endif
}

int da_vrs_requested_mode() { return ps_r__vrs_override >= 0 ? ps_r__vrs_override : ps_r__vrs; }

void da_vrs_reset(CBackend& cmd_list)
{
#if defined(XR_NATIVE)
    if (da_vrs_supported())
        cmd_list.set_ShadingRate(D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, nullptr);
#else
    (void)cmd_list;
#endif
}

da_vrs_scope::da_vrs_scope(CBackend& cmd_list, VrsPass pass) : m_cmd_list(cmd_list)
{
#if defined(XR_NATIVE)
    m_active = da_vrs_apply(cmd_list, pass);
#else
    (void)pass;
#endif
}

da_vrs_scope::~da_vrs_scope()
{
    if (m_active)
        da_vrs_reset(m_cmd_list);
}
} // namespace xray::render::RENDER_NAMESPACE
