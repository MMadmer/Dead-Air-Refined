// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "blender_da_mblur.h"

namespace xray::render::RENDER_NAMESPACE
{
void CBlender_da_mblur::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    switch (C.iElement)
    {
    case 0: // the longest motion of each tile
        C.r_Pass("da_fullscreen", "da_mblur_tile", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_da_mblur_vz", r2_RT_da_mblur_vz);
        C.r_End();
        break;
    case 1: // and of its neighbours
        C.r_Pass("da_fullscreen", "da_mblur_near", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_da_mblur_tile", r2_RT_da_mblur_tile);
        C.r_End();
        break;
    case 2: // the motion and the depth of every render pixel
        C.r_Pass("da_fullscreen", "da_mblur_prep", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_position", r2_RT_P);
        C.r_dx11Texture("s_da_taa_velocity", r2_RT_velocity);
        C.r_dx11Sampler("smp_nofilter");
        C.r_End();
        break;
    }
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
