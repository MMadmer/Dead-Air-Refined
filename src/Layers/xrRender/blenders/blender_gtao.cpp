#include "stdafx.h"
#include "blender_gtao.h"

namespace xray::render::RENDER_NAMESPACE
{
// da_fullscreen is our own fullscreen VS whose input layout matches the FVF::F_TL quad the
// phase draws - the stock stubs silently drop mismatched draws. Texture names resolve lazily
// by name, so binding $user$blue_noise before build_textures() has run is fine.
void CBlender_gtao::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if defined(USE_DX11)
    if (compute)
    {
        switch (C.iElement)
        {
        case 0: // raw GTAO, UAV $user$gtao_0
            C.r_ComputePass("gtao_render");
            C.r_dx11Texture("s_position", r2_RT_P);
            C.r_dx11Texture("s_normal", r2_RT_N);
            C.r_dx11Texture("s_blue_noise", r2_blue_noise);
            C.r_dx11Sampler("smp_nofilter");
            C.r_End();
            break;
        case 1: // guided filter, UAV $user$gtao_cur
            C.r_ComputePass("gtao_filter");
            C.r_dx11Texture("t_gtao_packed", r2_RT_gtao);
            C.r_dx11Sampler("smp_nofilter");
            C.r_End();
            break;
        }
        return;
    }
#endif

    switch (C.iElement)
    {
    case 0: // raw GTAO: view-z + occlusion into $user$gtao_0
        C.r_Pass("da_fullscreen", "gtao_render", FALSE, FALSE, FALSE);
        C.r_CullMode(D3DCULL_NONE);

        C.r_dx11Texture("s_position", r2_RT_P);
        C.r_dx11Texture("s_normal", r2_RT_N);
        C.r_dx11Texture("s_blue_noise", r2_blue_noise);

        C.r_dx11Sampler("smp_nofilter");

        C.r_End();
        break;
    case 1: // guided 8x8 filter into $user$gtao_cur
        C.r_Pass("da_fullscreen", "gtao_filter", FALSE, FALSE, FALSE);
        C.r_CullMode(D3DCULL_NONE);

        C.r_dx11Texture("t_gtao_packed", r2_RT_gtao);

        C.r_dx11Sampler("smp_nofilter");

        C.r_End();
        break;
    case 2: // the temporal pass reading history 0 (into $user$ssao_temp and history 1)
    case 3: // reading history 1 (into history 0)
        C.r_Pass("da_fullscreen", "gtao_temporal", FALSE, FALSE, FALSE);
        C.r_CullMode(D3DCULL_NONE);
        C.r_dx11Texture("s_position", r2_RT_P);
        C.r_dx11Texture("t_gtao_cur", r2_RT_gtao_cur);
        C.r_dx11Texture("t_gtao_hist", C.iElement == 2 ? r2_RT_gtao_hist0 : r2_RT_gtao_hist1);
        C.r_dx11Sampler("smp_nofilter");
        C.r_End();
        break;
    }
}
} // namespace xray::render::RENDER_NAMESPACE
