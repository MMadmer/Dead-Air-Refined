// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The screen-space glossy reflections (r4_rendertarget_phase_ssr.cpp, shaders/r3/da_ssr.h): the previous frame's
// linear colour, the half-resolution trace and its hit distance, the history pair and the result combine_1 reads.
inline constexpr pcstr da_ssr_scene_rt = "$user$da_ssr_scene";
inline constexpr pcstr da_ssr_raw_rt = "$user$da_ssr_raw";
inline constexpr pcstr da_ssr_dist_rt = "$user$da_ssr_dist";
inline constexpr pcstr da_ssr_hist_rt[2] = {"$user$da_ssr_h0", "$user$da_ssr_h1"};
inline constexpr pcstr da_ssr_rt = "$user$da_ssr";

// Elements: the trace, then the temporal pass writing history 0 / 1 (it reads the other one).
class CBlender_da_ssr final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air screen-space glossy reflections"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
