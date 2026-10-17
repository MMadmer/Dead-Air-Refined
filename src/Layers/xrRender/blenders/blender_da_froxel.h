// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The froxel volume's passes (r4_rendertarget_phase_froxels.cpp): 0 / 1 the injection writing volume A / B (it reads
// the other one as the history), 2 / 3 the integration of A / B, 4 the apply into the volumetric light target.
inline constexpr pcstr da_froxel_volume_rt[2] = {"$user$da_froxel_a", "$user$da_froxel_b"};
inline constexpr pcstr da_froxel_int_rt = "$user$da_froxel_int";

class CBlender_da_froxel final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air froxel volume"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
