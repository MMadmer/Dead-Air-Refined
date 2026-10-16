#include "stdafx.h"
#pragma hdrstop

#include "blender_smaa.h"

namespace xray::render::RENDER_NAMESPACE
{
// SMAA 1x (github.com/iryoku/smaa), ported from IX-Ray 1.6 stcop. Three fullscreen
// passes over the tonemapped LDR frame: edge detect -> blend weights -> neighbour blend.
// da_fullscreen VS on every pass: the stock stubs declare vertex layouts that do not match
// g_combine, and D3D11 drops mismatched draws silently.
void CBlender_smaa::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    switch (C.iElement)
    {
    case 0: // edge detection: frame -> RG edge mask
        C.r_Pass("da_fullscreen", "smaa_edge_detect", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_image", r2_RT_generic0);
        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
        break;
    case 1: // blending weights: edge mask + baked LUTs -> per-pixel weights
        C.r_Pass("da_fullscreen", "smaa_bweight_calc", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_edgetex", r2_RT_smaa_edges);
        C.r_dx11Texture("s_areatex", r2_smaa_area);
        C.r_dx11Texture("s_searchtex", r2_smaa_search);
        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
        break;
    case 2: // neighbourhood blend: weights applied to the frame
        C.r_Pass("da_fullscreen", "smaa_neighbour_blend", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_image", r2_RT_generic0);
        C.r_dx11Texture("s_blendtex", r2_RT_smaa_blend);
        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
        break;
    }
#endif
}

void CBlender_taa::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    // The resolve: current frame + reprojected history. Element = the frame (+2: SMAA's output in
    // rt_Generic instead of generic0) + where the history is (+1: taa_resolve instead of
    // taa_history): the two 10-bit targets take turns, the resolve writes the one not read.
    if (C.iElement < 4)
    {
        C.r_Pass("da_fullscreen", "da_taa", FALSE, FALSE, FALSE);
        C.r_dx11Texture("s_image", (C.iElement & 2) ? r2_RT_generic : r2_RT_generic0);
        C.r_dx11Texture("s_taa_history", (C.iElement & 1) ? r2_RT_taa_resolve : r2_RT_taa_history);
        C.r_dx11Texture("s_position", r2_RT_P);
        // The motion vectors of the velocity pass (read only when they were made this frame).
        C.r_dx11Texture("s_da_taa_velocity", "$user$velocity");
        // The pixels the forward passes painted over the opaque scene (phase_reactive).
        C.r_dx11Texture("s_reactive", r2_RT_reactive);
        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
    }
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
