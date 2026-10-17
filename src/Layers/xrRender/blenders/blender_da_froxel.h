// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The froxel volume's passes (r4_rendertarget_phase_froxels.cpp): 0 the injection (the air's density and the sun's
// visibility, the history reprojected in), 1 the integration, 2 the apply into the volumetric light target.
inline constexpr pcstr da_froxel_cur_rt = "$user$da_froxel_cur"; // RG16F: density, sun visibility (the lit particles too)
inline constexpr pcstr da_froxel_hist_rt = "$user$da_froxel_hist"; // the previous frame's copy of it
inline constexpr pcstr da_froxel_int_rt = "$user$da_froxel_int"; // RGBA16F: the glow up to each slice, transmittance

class CBlender_da_froxel final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air froxel volume"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
