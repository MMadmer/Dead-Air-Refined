// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "blender_da_smap_cache.h"

namespace xray::render::RENDER_NAMESPACE
{
// da_fullscreen: the FVF::F_TL quad of the viewport. No depth test, the depth written from the shader.
void CBlender_da_smap_cache::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

#if RENDER == R_R4
    C.r_Pass("da_fullscreen", "da_smap_cache", FALSE, FALSE, TRUE);
    C.r_dx11Texture("s_smap_cache", r2_RT_smap_cache);
    C.r_End();
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
