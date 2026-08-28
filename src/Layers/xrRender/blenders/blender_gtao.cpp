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
    case 1: // guided 8x8 filter into $user$ssao_temp
        C.r_Pass("da_fullscreen", "gtao_filter", FALSE, FALSE, FALSE);
        C.r_CullMode(D3DCULL_NONE);

        C.r_dx11Texture("t_gtao_packed", r2_RT_gtao);

        C.r_dx11Sampler("smp_nofilter");

        C.r_End();
        break;
    }
}
} // namespace xray::render::RENDER_NAMESPACE
