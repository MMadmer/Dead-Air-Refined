// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "blender_da_ssr.h"

namespace xray::render::RENDER_NAMESPACE
{
void CBlender_da_ssr::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    if (C.iElement > 2)
        return;
    C.r_Pass("da_fullscreen", C.iElement ? "da_ssr_temporal" : "da_ssr_trace", FALSE, FALSE, FALSE);
    C.r_dx11Texture("s_position", r2_RT_P);
    C.r_dx11Texture("s_normal", r2_RT_N);
    C.r_dx11Texture("s_diffuse", r2_RT_albedo);
    if (!C.iElement)
        C.r_dx11Texture("s_ssr_scene", da_ssr_scene_rt);
    else
    {
        C.r_dx11Texture("s_ssr_raw", da_ssr_raw_rt);
        C.r_dx11Texture("s_ssr_dist", da_ssr_dist_rt);
        C.r_dx11Texture("s_ssr_hist", da_ssr_hist_rt[2 - C.iElement]);
    }
    C.r_dx11Sampler("smp_nofilter");
    C.r_dx11Sampler("smp_rtlinear");
    C.r_End();
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
