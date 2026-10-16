// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Motion blur, the option r__mblur (shaders/r3/da_mblur.h): McGuire's reconstruction filter as DOOM Eternal runs it.
// Two small passes find the longest motion of every tile and of the tiles round it; combine_2 gathers along it
// after the temporal filter, so neither TAA nor an upscaler sees the blur. A matter of taste: no quality preset
// turns it on or off. The stock motion blur (mblur.h behind -mblur) smeared the whole frame by the camera's motion,
// moving objects and the weapon alike, and the combine pass the game runs never called it.

#include "stdafx.h"

#include "Layers/xrRender/blenders/blender_da_mblur.h"
#include "xrEngine/IGame_Persistent.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// The tile size in render pixels, which is also the longest half motion (the reach of one neighbouring tile).
constexpr u32 kTile = 20;
} // namespace

void CRenderTarget::da_mblur_create()
{
    rt_da_mblur_vz.create(r2_RT_da_mblur_vz, Device.dwRenderWidth, Device.dwRenderHeight, D3DFMT_A16B16G16R16F, 1);
    const u32 w = (Device.dwRenderWidth + kTile - 1) / kTile, h = (Device.dwRenderHeight + kTile - 1) / kTile;
    rt_da_mblur_tile.create(r2_RT_da_mblur_tile, w, h, D3DFMT_G16R16F, 1);
    rt_da_mblur_near.create(r2_RT_da_mblur_near, w, h, D3DFMT_G16R16F, 1);
    CBlender_da_mblur blender;
    s_da_mblur.create(&blender, "r2" DELIMITER "da_mblur");
}

void CRenderTarget::phase_da_mblur(const Fmatrix& previous)
{
    m_da_mblur_ran = false;
    m_da_mblur_prev = previous;
    if (!ps_r__mblur || ps_r__mblur_shutter <= 0.f || RImplementation.o.msaa || !rt_da_mblur_vz ||
        !rt_da_mblur_vz->pRT || !rt_da_mblur_near || !rt_da_mblur_near->pRT ||
        (g_pGamePersistent && g_pGamePersistent->OnRenderPPUI_query()))
        return;
    PIX_EVENT(phase_da_mblur);

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    // The motion and depth of every pixel, the tiles, the tiles' neighbourhoods.
    for (const u32 e : {2u, 0u, 1u})
    {
        const ref_rt& target = e == 2 ? rt_da_mblur_vz : e ? rt_da_mblur_near : rt_da_mblur_tile;
        u_setrt(RCache, target, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
        RCache.SetViewport({0.f, 0.f, float(target->dwWidth), float(target->dwHeight), 0.f, 1.f});
        RCache.set_Element(s_da_mblur->E[e]);
        set_da_mblur_constants(true);
        u32 offset = 0;
        FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, offset);
        pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
        RImplementation.Vertex.Unlock(4, g_combine->vb_stride);
        RCache.set_Geometry(g_combine);
        RCache.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
    }
    // combine_2 draws at the display size next and does not always set the viewport itself.
    RCache.SetViewport({0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight), 0.f, 1.f});
    m_da_mblur_ran = true;
}

// The constants of da_mblur.h for the element just set, the tile passes' and combine_2's alike; on false the blur
// is off for the element (no tiles this frame).
void CRenderTarget::set_da_mblur_constants(bool on)
{
    const float w = float(Device.dwRenderWidth), h = float(Device.dwRenderHeight);
    RCache.set_c("da_mblur", on ? ps_r__mblur_shutter : 0.f, float(kTile), velocity_ready() && t_velocity ? 1.f : 0.f, 0.f);
    RCache.set_c("da_mblur_size", w, h, 1.f / w, 1.f / h);
    RCache.set_c("m_da_mblur_prev", m_da_mblur_prev);
}
} // namespace xray::render::RENDER_NAMESPACE
