// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The bloom pyramid, after DOOM Eternal's (shaders/r3/da_bloom.h): the light the lens and the eye scatter round
// what is bright, a tight core and a wide halo, added before the tone curve in combine_2. The stock bloom left
// nothing on the screen: one 256x256 RGBA8 target of combine_1's opaque light past a threshold the exposed scene
// rarely reaches, added after the tone curve. It stays the exposure meter's source (phase_luminance).

#include "stdafx.h"

#include "Layers/xrRender/blenders/blender_da_bloom.h"
#include "Layers/xrRender/da_temporal.h"
#include "xrEngine/IGame_Persistent.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// The levels' shares of the halo, half the render size first: a tight core and a long tail. They sum to one, so the
// pyramid spreads the light past the knee without adding to it.
constexpr float kLevelWeight[da_bloom_levels] = {0.35f, 0.22f, 0.15f, 0.12f, 0.09f, 0.07f};

u32 level_size(u32 size, u32 level) { return std::max(1u, (size + (2u << level) - 1) / (2u << level)); }
} // namespace

void CRenderTarget::da_bloom_create()
{
    static_assert(std::extent_v<decltype(rt_da_bloom)> == da_bloom_levels);
    static_assert(std::extent_v<decltype(s_da_bloom)> == da_bloom_shaders);
    const u32 w = Device.dwRenderWidth, h = Device.dwRenderHeight;
    for (u32 i = 0; i < da_bloom_levels; ++i)
    {
        rt_da_bloom[i].create(da_bloom_level_rt[i], level_size(w, i), level_size(h, i), D3DFMT_A16B16G16R16F, 1);
        rt_da_bloom_temp[i].create(da_bloom_temp_rt[i], level_size(w, i), level_size(h, i), D3DFMT_A16B16G16R16F, 1);
    }
    for (u32 i = 0; i < 2; ++i)
        rt_da_bloom_hist[i].create(da_bloom_hist_rt[i], level_size(w, 0), level_size(h, 0), D3DFMT_A16B16G16R16F, 1);
    for (u32 i = 0; i < da_bloom_shaders; ++i)
    {
        CBlender_da_bloom blender(i * 6);
        string32 name;
        xr_sprintf(name, "r2" DELIMITER "da_bloom%u", i);
        s_da_bloom[i].create(&blender, name);
    }
    m_da_bloom_frame = 0;
}

bool CRenderTarget::da_bloom_wanted() const
{
    return ps_r__bloom > 0.f && rt_da_bloom[0] && rt_da_bloom[0]->pRT && rt_da_bloom_hist[1] &&
        rt_da_bloom_hist[1]->pRT && s_da_bloom[0] && !(g_pGamePersistent && g_pGamePersistent->OnRenderPPUI_query());
}

void CRenderTarget::phase_da_bloom(bool opaque_grab)
{
    m_da_bloom_ran = false;
    if (!da_bloom_wanted())
        return;
    PIX_EVENT(phase_da_bloom);

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    const auto pass = [&](const ref_rt& target, const ref_rt& second, u32 element, const auto& constants)
    {
        u_setrt(RCache, target, second, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
        RCache.SetViewport({0.f, 0.f, float(target->dwWidth), float(target->dwHeight), 0.f, 1.f});
        RCache.set_Element(&*s_da_bloom[element / 6]->E[element % 6]);
        constants();
        u32 offset = 0;
        FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, offset);
        pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
        RImplementation.Vertex.Unlock(4, g_combine->vb_stride);
        RCache.set_Geometry(g_combine);
        RCache.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
    };

    // The bright pass, its history only when the previous frame made it and the motion vectors are this frame's.
    const bool history = m_da_bloom_frame && m_da_bloom_frame + 1 == Device.dwFrame && !da_history_reset() &&
        velocity_ready() && t_velocity;
    m_da_bloom_hist ^= 1;
    const float src_w = float(Device.dwRenderWidth), src_h = float(Device.dwRenderHeight);
    pass(rt_da_bloom[0], rt_da_bloom_hist[m_da_bloom_hist], da_bloom_e_prefilter + m_da_bloom_hist, [&]
    {
        RCache.set_c("da_bloom_src", 1.f / src_w, 1.f / src_h, opaque_grab ? 1.f : 0.f,
            ps_r__tonemap_white > 0.01f ? ps_r__tonemap_white : 1.7f);
        RCache.set_c("da_bloom_knee", ps_r__bloom_threshold, ps_r__bloom_knee, history ? 1.f : 0.f, src_w);
    });

    for (u32 i = 1; i < da_bloom_levels; ++i)
    {
        const ref_rt& src = rt_da_bloom[i - 1];
        pass(rt_da_bloom[i], nullptr, da_bloom_e_down + i - 1, [&]
        {
            RCache.set_c("da_bloom_step", 1.f / float(src->dwWidth), 1.f / float(src->dwHeight), 0.f, 0.f);
        });
    }

    // From the smallest level up: each level's Gaussian at its weight, plus the finished level below it.
    for (u32 i = da_bloom_levels; i-- > 0;)
    {
        const ref_rt& level = rt_da_bloom[i];
        pass(rt_da_bloom_temp[i], nullptr, da_bloom_e_blur_h + i, [&]
        {
            RCache.set_c("da_bloom_step", 1.f / float(level->dwWidth), 0.f, 1.f, 0.f);
        });
        pass(level, nullptr, da_bloom_e_blur_v + i, [&]
        {
            RCache.set_c("da_bloom_step", 0.f, 1.f / float(level->dwHeight), kLevelWeight[i], 0.f);
        });
    }

    // The passes after this one draw at the render size and do not set the viewport themselves.
    RCache.SetViewport({0.f, 0.f, src_w, src_h, 0.f, 1.f});
    m_da_bloom_frame = Device.dwFrame;
    m_da_bloom_ran = true;
}
} // namespace xray::render::RENDER_NAMESPACE
