// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "blender_da_upscale.h"

namespace xray::render::RENDER_NAMESPACE
{
// da_fullscreen on every pass: its input layout is the FVF::F_TL quad the phases draw, and D3D11
// drops a draw whose layout does not match the vertex shader without a word.
void CBlender_da_upscale::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    switch (C.iElement)
    {
    case 0: // identity: render-size frame -> display-size output
        C.r_Pass("da_fullscreen", "da_upscale_identity", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_image", r2_RT_generic0);
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
        break;
    case 1: // depth upscale: no depth test (ZFUNC ALWAYS), depth written from the shader
        C.r_Pass("da_fullscreen", "da_depth_upscale", FALSE, FALSE, TRUE);
        C.r_dx11Texture("s_da_depth_render", r2_RT_MSAAdepth);
        C.r_End();
        break;
    case 2: // reactive mask: the scene against its opaque-only grab
        C.r_Pass("da_fullscreen", "da_reactive", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_image", r2_RT_generic0);
        C.r_dx11Texture("s_da_opaque", r2_RT_SSR);
        C.r_End();
        break;
    }
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
