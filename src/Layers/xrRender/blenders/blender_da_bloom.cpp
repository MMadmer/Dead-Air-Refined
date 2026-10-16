// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "blender_da_bloom.h"

namespace xray::render::RENDER_NAMESPACE
{
// da_fullscreen on every pass: its input layout is the FVF::F_TL quad the phase draws.
void CBlender_da_bloom::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    const u32 e = m_first + C.iElement;
    if (e >= da_bloom_elements)
        return;
    if (e < da_bloom_e_down)
    {
        C.r_Pass("da_fullscreen", "da_bloom_prefilter", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_image", r2_RT_generic1);
        C.r_dx11Texture("s_scene", r2_RT_generic0);
        C.r_dx11Texture("s_da_opaque", r2_RT_SSR);
        C.r_dx11Texture("s_bloom_hist", da_bloom_hist_rt[e ^ 1]);
        C.r_dx11Texture("s_da_taa_velocity", r2_RT_velocity);
    }
    else if (e < da_bloom_e_blur_h)
    {
        C.r_Pass("da_fullscreen", "da_bloom_down", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_bloom_src", da_bloom_level_rt[e - da_bloom_e_down]);
    }
    else if (e < da_bloom_e_blur_v)
    {
        C.r_Pass("da_fullscreen", "da_bloom_blur", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_bloom_src", da_bloom_level_rt[e - da_bloom_e_blur_h]);
    }
    else
    {
        const u32 level = e - da_bloom_e_blur_v;
        const bool up = level + 1 < da_bloom_levels;
        C.r_Pass("da_fullscreen", up ? "da_bloom_blur_up" : "da_bloom_blur", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_bloom_src", da_bloom_temp_rt[level]);
        if (up)
            C.r_dx11Texture("s_bloom_coarse", da_bloom_level_rt[level + 1]);
    }
    C.r_dx11Sampler("smp_rtlinear");
    C.r_dx11Sampler("smp_nofilter");
    C.r_End();
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
