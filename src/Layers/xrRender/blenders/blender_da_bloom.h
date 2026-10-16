// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The bloom pyramid (r4_rendertarget_phase_da_bloom.cpp): its levels, the horizontal blur's targets and the bright
// pass's history pair.
inline constexpr u32 da_bloom_levels = 6;
inline constexpr pcstr da_bloom_level_rt[da_bloom_levels] = {"$user$da_bloom0", "$user$da_bloom1", "$user$da_bloom2",
    "$user$da_bloom3", "$user$da_bloom4", "$user$da_bloom5"};
inline constexpr pcstr da_bloom_temp_rt[da_bloom_levels] = {"$user$da_bloom_t0", "$user$da_bloom_t1",
    "$user$da_bloom_t2", "$user$da_bloom_t3", "$user$da_bloom_t4", "$user$da_bloom_t5"};
inline constexpr pcstr da_bloom_hist_rt[2] = {"$user$da_bloom_h0", "$user$da_bloom_h1"};

// Elements: the bright pass writing history 0 / 1 (it reads the other one), the downsamples into levels 1..5, the
// horizontal blur of each level, then the vertical blur of each level (with the level below it added, but the last).
inline constexpr u32 da_bloom_e_prefilter = 0;
inline constexpr u32 da_bloom_e_down = 2; // + level - 1
inline constexpr u32 da_bloom_e_blur_h = da_bloom_e_down + da_bloom_levels - 1; // + level
inline constexpr u32 da_bloom_e_blur_v = da_bloom_e_blur_h + da_bloom_levels; // + level
inline constexpr u32 da_bloom_elements = da_bloom_e_blur_v + da_bloom_levels;
// A shader holds six elements: the pyramid's take four, each compiled from its own first element.
inline constexpr u32 da_bloom_shaders = (da_bloom_elements + 5) / 6;

class CBlender_da_bloom final : public IBlender
{
    u32 m_first;

public:
    explicit CBlender_da_bloom(u32 first) : m_first(first) {}
    LPCSTR getComment() override { return "INTERNAL: Dead Air bloom pyramid"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
