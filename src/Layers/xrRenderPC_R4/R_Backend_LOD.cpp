#include "stdafx.h"
#include "r_backend_lod.h"

namespace xray::render::RENDER_NAMESPACE
{
R_LOD::R_LOD(CBackend& cmd_list_in) : cmd_list(cmd_list_in)
{
    unmap();
}

void R_LOD::set_LOD(float LOD)
{
    if (c_LOD)
    {
        float factor = clampr<float>(ceil(LOD * LOD * LOD * LOD * LOD * 8.0f), 1, 7);
        cmd_list.set_c(c_LOD, factor);
    }
}

void R_LOD::set_fade(float cover)
{
    // y turns the dither's pattern every frame, so TAA blends it into a partial coverage.
    if (c_fade)
        cmd_list.set_c(c_fade, cover, float(Device.dwFrame & 63), 0.f, 0.f);
}
} // namespace xray::render::RENDER_NAMESPACE
