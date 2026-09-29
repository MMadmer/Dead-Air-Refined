// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRender/da_wind_history.h"
#include "Layers/xrRender/FBasicVisual.h"
#include "da_rt.h"

// Motion vectors the G-buffer cannot carry (docs/dead-air/DX12_RENDERER.md, 3.4), drawn into
// rt_Velocity after the forward pass and before the reactive mask and the upscale:
//  * the sky - no geometry writes it, and a zero there would say "moves with the camera";
//  * the forward water - the G-buffer under it holds the bed, while the eye and the history see
//    the surface.
// Both whenever the G-buffer drew the vectors this frame (TAA, an upscaler, frame generation or the
// r__motion_vectors view).
namespace xray::render::RENDER_NAMESPACE
{
extern float r_ssaGLOD_start, r_ssaGLOD_end;

namespace
{
// The water visuals the forward pass is about to draw, with their screen-space area (for the
// LOD the forward pass picks). Collected before it, because drawing consumes the lists; once per
// frame for everybody who redraws them (the motion vectors, the ray traced reflections' requests).
xr_vector<R_dsgraph::_NormalItem> g_da_water_velocity_items;
u32 g_da_water_collect_frame = u32(-1);

// The water surface shaders: water.vs through water_soft (effects_water.s and its two
// siblings). The distortion element (waterd*) never reaches the normal lists.
bool da_is_water_pass(const SPass* pass)
{
    if (!pass || !pass->vs)
        return false;
    pcstr name = pass->vs->cName.c_str();
    return name && !strncmp(name, "water", 5) && strncmp(name, "waterd", 6);
}

// The same selection the forward pass makes (r__dsgraph_render.cpp, calcLOD).
float da_water_lod(float ssa)
{
    return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

// The da_fullscreen.vs quad: xy is the clip-space position, zw the texture coordinate.
void da_velocity_quad(CBackend& cmd_list, ref_geom& geom)
{
    u32 offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, geom->vb_stride, offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, geom->vb_stride);
    cmd_list.set_Geometry(geom);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
}
} // namespace

// The sky pixels: the pixel's direction through the unjittered view-projections of both frames,
// translation left out (da_sky_velocity.ps). The cloud deck is on those pixels too.
void CRenderTarget::phase_sky_velocity()
{
    if (!velocity_ready() || !rt_Velocity->pRT || RImplementation.o.msaa)
        return;
    if (!s_sky_velocity)
        s_sky_velocity.create("da_sky_velocity");
    if (!s_sky_velocity)
        return;

    PIX_EVENT(DA_phase_sky_velocity);

    u_setrt(RCache, rt_Velocity, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
    RCache.SetViewport({ 0.f, 0.f, float(rt_Velocity->dwWidth), float(rt_Velocity->dwHeight), 0.f, 1.f });
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    RCache.set_Z(FALSE);
    RCache.set_ColorWriteEnable();
    RCache.set_Element(s_sky_velocity->E[0]);
    da_velocity_quad(RCache, g_combine);
}

void CRenderTarget::phase_water_velocity_collect()
{
    // Idempotent per frame (PLAN.md C19): the reflections collect early, the forward pass again.
    if (g_da_water_collect_frame == Device.dwFrame)
        return;
    g_da_water_collect_frame = Device.dwFrame;
    g_da_water_velocity_items.clear();
    const bool velocity = velocity_ready() && rt_Velocity->pRT && !RImplementation.o.msaa;
    if (!velocity && !da_rt_live(RtEffect::Reflections))
        return;

    // Water is static level geometry of the forward priority (sorting 2): the normal lists of
    // priority 1, every pass.
    auto& dsgraph = RImplementation.get_imm_context();
    for (u32 pass = 0; pass < SHADER_PASSES_MAX; ++pass)
    {
        auto& map = dsgraph.mapNormalPasses[1][pass];
        for (auto* node = map.begin(); node != map.end(); ++node)
        {
            if (da_is_water_pass(node->first))
                g_da_water_velocity_items.insert(g_da_water_velocity_items.end(), node->second.begin(), node->second.end());
        }
    }
}

// The water surface: the forward water meshes again with da_water_velocity (water.vs's bob, now
// and one frame back), depth-tested less-equal against the scene without writing it - the water
// writes no depth, so the test keeps exactly the pixels it covers. Over the sky's vectors.
void CRenderTarget::phase_water_velocity()
{
    if (g_da_water_velocity_items.empty())
        return;
    if (!velocity_ready() || !rt_Velocity->pRT || RImplementation.o.msaa)
    {
        g_da_water_velocity_items.clear();
        return;
    }
    if (!s_water_velocity)
        s_water_velocity.create("da_water_velocity");
    if (!s_water_velocity)
    {
        g_da_water_velocity_items.clear();
        return;
    }

    PIX_EVENT(DA_phase_water_velocity);

    u_setrt(RCache, rt_Velocity, nullptr, nullptr, rt_MSAADepth);
    RCache.SetViewport({ 0.f, 0.f, float(rt_Velocity->dwWidth), float(rt_Velocity->dwHeight), 0.f, 1.f });
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    RCache.set_ColorWriteEnable();
    RCache.set_xform_world(Fidentity);
    RCache.set_Element(s_water_velocity->E[0]);
    // The bob's clock as the previous frame drew the water.
    RCache.set_c("da_water_velocity", da_wind_prev().time, 0.f, 0.f, 0.f);
    for (const R_dsgraph::_NormalItem& item : g_da_water_velocity_items)
        item.pVisual->Render(RCache, da_water_lod(item.ssa), false);
    g_da_water_velocity_items.clear();
}

bool da_water_forward_items_empty() { return g_da_water_velocity_items.empty(); }

// The water surfaces again with a shader of the ray tracing lane: `element` for the first water
// material, `element_b` for the second (water_green, profile slot B), picked by the pixel shader the
// surface is drawn with.
void da_water_forward_draw(CBackend& cmd_list, ShaderElement* element, ShaderElement* element_b)
{
    ShaderElement* current = nullptr;
    for (const R_dsgraph::_NormalItem& item : g_da_water_velocity_items)
    {
        ShaderElement* wanted = element;
        const Shader* shader = item.pVisual->shader._get();
        if (element_b && shader && shader->E[0] && !shader->E[0]->passes.empty() && shader->E[0]->passes[0]->ps)
        {
            pcstr ps = shader->E[0]->passes[0]->ps->cName.c_str();
            if (ps && strstr(ps, "water_green"))
                wanted = element_b;
        }
        if (!wanted)
            continue;
        if (wanted != current)
        {
            cmd_list.set_Element(wanted);
            current = wanted;
        }
        item.pVisual->Render(cmd_list, da_water_lod(item.ssa), false);
    }
}
} // namespace xray::render::RENDER_NAMESPACE
