#include "stdafx.h"
#include "blender_hud_shadow.h"

namespace xray::render::RENDER_NAMESPACE
{
void CBlender_hud_shadow::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

    switch (C.iElement)
    {
    case 0:
        // Modulate: the accumulator holds the sun term alone at this point, so multiplying by
        // the shadow factor darkens exactly the sun and nothing else. Z-test stays on - the
        // quad is issued at the HUD depth limit with an inverted compare, which is what
        // restricts the pass to first-person pixels.
        C.r_Pass("stub_notransform_t", "hud_shadow", false, TRUE, FALSE, TRUE, D3DBLEND_ZERO, D3DBLEND_SRCCOLOR);
        C.r_CullMode(D3DCULL_NONE);
        C.PassSET_ZB(TRUE, FALSE);

        C.r_dx11Texture("s_position", r2_RT_P);
        C.r_dx11Texture("s_smap_hud", r2_RT_smap_hud);

        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_smap");

        C.r_End();
        break;
    }
}
} // namespace xray::render::RENDER_NAMESPACE
