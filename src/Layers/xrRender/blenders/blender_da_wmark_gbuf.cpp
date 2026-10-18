// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "blender_da_wmark_gbuf.h"

namespace xray::render::RENDER_NAMESPACE
{
// Depth tested, not written (the marks lie on the surface, as the albedo pass draws them). No blending: a packed normal
// does not blend and the gloss is its target's alpha itself, so the coverage is a dither TAA resolves.
void CBlender_da_wmark_gbuf::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    C.r_Pass("da_wmark_gbuf", "da_wmark_gbuf", false, TRUE, FALSE);
    C.r_dx11Texture("s_base", C.L_textures[0]);
    C.r_dx11Sampler("smp_rtlinear");
    C.r_End();
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
