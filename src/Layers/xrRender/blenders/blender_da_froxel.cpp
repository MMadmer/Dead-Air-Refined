// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "blender_da_froxel.h"

namespace xray::render::RENDER_NAMESPACE
{
void CBlender_da_froxel::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if defined(USE_DX11)
    switch (C.iElement)
    {
    case 0:
        C.r_ComputePass("da_froxel_inject");
        C.r_dx11Texture("s_smap_array", r2_RT_smap_depth);
        C.r_dx11Texture("s_cloud_map", r2_RT_cloud_map);
        C.r_dx11Texture("s_blue_noise", r2_blue_noise);
        C.r_dx11Texture("s_froxel_hist", da_froxel_hist_rt);
        C.r_dx11Sampler("smp_smap");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
        break;
    case 1:
        C.r_ComputePass("da_froxel_integrate");
        C.r_dx11Texture("s_froxel_cur", da_froxel_cur_rt);
        C.r_End();
        break;
    case 2:
        C.r_Pass("da_fullscreen", "da_froxel_apply", FALSE, FALSE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_ONE);
        C.r_dx11Texture("s_position", r2_RT_P);
        C.r_dx11Texture("s_froxel_int", da_froxel_int_rt);
        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
        break;
    }
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
