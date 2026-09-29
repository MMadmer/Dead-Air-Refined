#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
void CRenderTarget::phase_occq()
{
    // No colour is written: the back buffer is only a partner for the depth, and cannot be one
    // for the render-size depth while an upscaler runs.
    if (!RImplementation.o.msaa && !da_upscaler_active())
        u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), 0, 0, rt_MSAADepth);
    else
        u_setrt(RCache, Device.dwRenderWidth, Device.dwRenderHeight, 0, 0, 0, rt_MSAADepth);
    RCache.set_Shader(s_occq);
    RCache.set_CullMode(CULL_CCW);
    RCache.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00);
    RCache.set_ColorWriteEnable(FALSE);
}
} // namespace xray::render::RENDER_NAMESPACE
