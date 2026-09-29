// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Ray tracing: availability, the per-frame latch of the settings and what the frame asks of it
// (da_rt.h).

#include "stdafx.h"

#include "da_rt.h"
#include "da_rt_device.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
struct DaRtLatch
{
    bool live[u32(RtEffect::Count)]{};
    u32 quality{1};
};
DaRtLatch g_rt_latch;

#if defined(XR_D3D12)
bool da_rt_setting(RtEffect effect)
{
    switch (effect)
    {
    case RtEffect::Reflections: return ps_r__rt_reflections != 0;
    case RtEffect::Shadows: return ps_r__rt_shadows != 0;
    case RtEffect::AO: return ps_r__rtao != 0;
    default: return false;
    }
}
#endif
} // namespace

bool da_rt_available()
{
#if defined(XR_D3D12)
    // The passes read single-sampled depth and G-buffer.
    return da_rt_device_ready() && !RImplementation.o.msaa;
#else
    return false;
#endif
}

void da_rt_reset_begin() {}

void da_rt_reset_end() {}

void da_rt_frame_begin()
{
    DaRtLatch latch;
#if defined(XR_D3D12)
    latch.quality = std::min(ps_r__rt_quality, 2u);
    const bool traceable = da_rt_available() && da_rt_scene_ready();
    for (u32 i = 0; i < u32(RtEffect::Count); ++i)
    {
        const auto effect = RtEffect(i);
        latch.live[i] = traceable && da_rt_setting(effect) && da_rt_pass_ready(effect) && da_rt_targets_ready(effect);
    }
    // The middle cascade may be reused from an earlier frame (r__sun_cache_ms): a map drawn with the
    // other set of static casters would double or lose their shadows until it expires.
    if (latch.live[u32(RtEffect::Shadows)] != g_rt_latch.live[u32(RtEffect::Shadows)])
    {
        for (auto& cache : RImplementation.r_sun.m_smap_cache)
            cache.valid = false;
    }
#endif
    g_rt_latch = latch;
}

bool da_rt_live(RtEffect effect) { return effect < RtEffect::Count && g_rt_latch.live[u32(effect)]; }

bool da_rt_ao_live() { return da_rt_live(RtEffect::AO); }

u32 da_rt_quality() { return g_rt_latch.quality; }

#if defined(XR_D3D12)
const DaRtTier& da_rt_tier() { return da_rt_tier_of(g_rt_latch.quality); }
#endif

Fvector4 da_rt_constants()
{
#if defined(XR_D3D12)
    const auto flag = [](RtEffect effect) { return da_rt_live(effect) ? 1.f : 0.f; };
    return {flag(RtEffect::Reflections), flag(RtEffect::Shadows), flag(RtEffect::AO), float(g_rt_latch.quality)};
#else
    return {0.f, 0.f, 0.f, 0.f};
#endif
}

bool da_rt_wants_scene_grab() { return da_rt_live(RtEffect::Reflections); }

bool da_rt_skip_static_casters() { return da_rt_live(RtEffect::Shadows); }

// The denoisers reproject with the motion vectors.
bool da_rt_wants_motion() { return da_rt_live(RtEffect::Shadows) || da_rt_live(RtEffect::AO); }

// Asked when the targets are created (renderer start, vid_restart), where the RTAO setting changes.
bool da_rt_needs_ao_targets()
{
#if defined(XR_D3D12)
    return ps_r__rtao && da_rt_available() && da_rt_pass_ready(RtEffect::AO);
#else
    return false;
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
