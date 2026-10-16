// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Ray tracing passes (docs/dead-air/dx12-features/dxr.md 3.5-3.7): sun shadows (trace, temporal,
// two a-trous passes), ambient occlusion (trace, temporal, depth-aware upsample into the GTAO target,
// the GTAO guided filter), reflections (requests, trace with on-screen reuse), the reflections of glossy surfaces
// (half-resolution trace, temporal upscale; before combine_1, which reads them), the pre-skinning of
// characters and the r__rt_debug views. Every pass is a compute dispatch in a section on the
// immediate context (da_rt_device.h, DirectX 12 and Vulkan); the per-frame latch (da_rt.cpp) decides
// which run.

#include "stdafx.h"

#include "da_rt.h"
#include "da_rt_device.h"
#include "da_dxc.h"

#if defined(DA_RT_LANE)
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRender/da_veg.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if defined(DA_RT_LANE)
// The combine's haze bounds (Blender_Recorder_StandartBinding.cpp).
void da_rt_fog_bounds(float& near_m, float& far_m);
#endif

namespace
{
// Pipelines of each effect's passes; an effect missing one never goes live.
struct DaRtPasses
{
    bool ready[u32(RtEffect::Count)]{};
#if defined(DA_RT_LANE)
    bool made{};
    DaRtPipeline sun{};
    DaRtPipeline sun_direct{};
    DaRtPipeline temporal{};
    DaRtPipeline atrous_first{};
    DaRtPipeline atrous_last{};
    DaRtPipeline ao{};
    DaRtPipeline ao_half{};
    DaRtPipeline ao_upsample{};
    DaRtPipeline reflections{};
    DaRtPipeline gloss{};
    DaRtPipeline gloss_temporal{};
    DaRtPipeline skin{};
    DaRtPipeline debug{};
#endif
};
DaRtPasses g_rt_passes;

#if defined(DA_RT_LANE)
// ---- constants shared with rt/rt_common.hlsli and rt/rt_material.hlsli ---------------------------

constexpr u32 kViewGbufferPacked = 0x1, kViewHistoryValid = 0x2, kViewVelocity = 0x4;
constexpr float kSunDiscTan = 0.0087268f; // tan(0.5 degrees)
constexpr float kTrunkHeight = 1.2f;

struct DaRtView
{
    Fmatrix view, inv_view, vp, vp_prev;
    Fvector4 pos_decompress, proj, size, camera, sun;
    u32 scene[4];
    Fvector4 frame;
    u32 veg[4]; // the vegetation tables: slot, season block, rows | profile rows << 16, snow (float bits)
};
static_assert(sizeof(DaRtView) == 384);

struct DaRtShade
{
    Fvector4 sun_color, env_color, ambient, fog_params, fog_color, fog, fog2, sky_tint, rain2, tonemap, grade;
};
static_assert(sizeof(DaRtShade) == 176);

struct DaRtSunConstants
{
    DaRtView view;
    Fvector4 params;
};
struct DaRtAoConstants
{
    DaRtView view;
    Fvector4 params;
};
struct DaRtTemporalConstants
{
    DaRtView view;
    Fvector4 temporal, temporal2;
};
struct DaRtAtrousConstants
{
    DaRtView view;
    Fvector4 atrous;
};
struct DaRtUpsampleConstants
{
    DaRtView view;
    Fvector4 upsample;
};
struct DaRtReflectionConstants
{
    DaRtView view;
    DaRtShade shade;
    Fvector4 refl;
    u32 mask[4];
};
struct DaRtDebugConstants
{
    DaRtView view;
    u32 debug[4];
};
struct DaRtGlossConstants
{
    DaRtView view;
    DaRtShade shade;
    Fvector4 refl;
    u32 mask[4];
    Fvector4 gloss;
};
struct DaRtGlossTemporalConstants
{
    DaRtView view;
    Fvector4 temporal, temporal2;
};

// The glossy reflections (dxr.md 3.9): the lowest gloss traced, the gloss of the full weight, the spread of a
// rough surface's rays (tangent per roughness squared), the history clip in sigmas, the least fresh share.
constexpr float kGlossMin = 0.03f, kGlossFull = 0.12f, kGlossSpread = 0.35f, kGlossClip = 1.5f, kGlossFresh = 0.1f;
// The glossy rays' reach in metres and their screen-space steps at most.
constexpr float kGlossReach = 250.f;
constexpr u32 kGlossSteps = 16u;

// The tiers of r__rt_quality (dxr.md 10): Low, Medium, High.
constexpr DaRtTier kTiers[3] = {
    {false, false, true, 1.0f, 16.f, 250.f, false, false, 40.f, 20.f, 6u, 4u, 16u, false},
    {true, true, true, 1.5f, 32.f, 400.f, true, true, 60.f, 30.f, 12u, 8u, 24u, true},
    {true, true, false, 2.0f, 32.f, 1000.f, true, true, 60.f, 30.f, 16u, 8u, 32u, true},
};

ID3D11ShaderResourceView* da_rt_srv(const ref_rt& rt) { return rt && rt->pTexture ? rt->pTexture->get_SRView() : nullptr; }

ID3D11ShaderResourceView* da_rt_srv(const ref_texture& t) { return t ? t->get_SRView() : nullptr; }

u32 da_rt_groups(u32 size) { return (size + 7) / 8; }

const ref_rt& da_rt_depth(const CRenderTarget& t) { return t.rt_Render_Depth ? t.rt_Render_Depth : t.rt_MSAADepth; }

DaRtView da_rt_view(u32 flags)
{
    DaRtView v{};
    v.view = Device.mView;
    v.inv_view = Device.mInvView;
    v.vp = da_vp_nojit();
    v.vp_prev = da_vp_prev();
    v.pos_decompress = da_pos_decompress(Device.fFOV);
    const Fmatrix& P = Device.mProject;
    const float far_z = _abs(1.f - P._33) > 1e-6f ? P._43 / (1.f - P._33) : 10000.f;
    v.proj.set(P._33, P._43, tanf(deg2rad(Device.fFOV) * 0.5f), far_z);
    const float w = float(Device.dwRenderWidth), h = float(Device.dwRenderHeight);
    v.size.set(w, h, 1.f / w, 1.f / h);
    v.camera.set(Device.vCameraPosition.x, Device.vCameraPosition.y, Device.vCameraPosition.z, Device.fTimeGlobal);
    Fvector to_sun{0.f, 1.f, 0.f};
    if (const light* sun = static_cast<light*>(RImplementation.Lights.sun._get()))
        to_sun.invert(sun->direction).normalize_safe();
    v.sun.set(to_sun.x, to_sun.y, to_sun.z, kSunDiscTan);
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    const CRenderTarget& t = *RImplementation.Target;
    v.scene[0] = scene.static_instances;
    v.scene[1] = scene.static_texture_base;
    v.scene[2] = flags | (RImplementation.o.gbuffer_opt ? kViewGbufferPacked : 0u) | (t.velocity_ready() ? kViewVelocity : 0u);
    v.scene[3] = Device.dwFrame;
    // The frame's textures are jittered; the alpha test clips where the G-buffer does (da_aref_u).
    const Fvector2 jitter = da_jitter_px();
    v.frame.set(jitter.x, jitter.y, float(ps_r__aref_quality) / 255.f, 0.f);
    // The stand-ins' crowns by their species' day (rt_geometry.hlsli rt_veg_*), the snow on the branches.
    ID3D11Buffer* veg = nullptr;
    u32 prof = 0, seas_first = 0, seas = 0;
    float snow = 0.f;
    if (!da_veg_rt_tables(veg, prof, seas_first, seas, snow))
        snow = 0.f;
    v.veg[0] = scene.veg_tables;
    v.veg[1] = scene.veg_seas;
    v.veg[2] = scene.veg_rows | (scene.veg_prof << 16);
    v.veg[3] = std::bit_cast<u32>(snow);
    return v;
}

// combine_1's lighting and haze constants (phase_combine, the binders of r2.cpp and
// Blender_Recorder_StandartBinding.cpp), for the reflections' off-screen hits.
DaRtShade da_rt_shade()
{
    DaRtShade s{};
    const CEnvDescriptorMixer& env = g_pGamePersistent->Environment().CurrentEnv;
    if (const light* sun = static_cast<light*>(RImplementation.Lights.sun._get()))
        s.sun_color.set(sun->color.r, sun->color.g, sun->color.b, 0.f);
    s.env_color.set(env.env_color.x * 2.f * ps_r2_sun_lumscale_hemi, env.env_color.y * 2.f * ps_r2_sun_lumscale_hemi,
        env.env_color.z * 2.f * ps_r2_sun_lumscale_hemi, env.env_color.w);
    s.ambient.set(std::max(env.ambient.x * 2.f, 0.001f) * ps_r2_sun_lumscale_amb,
        std::max(env.ambient.y * 2.f, 0.001f) * ps_r2_sun_lumscale_amb, std::max(env.ambient.z * 2.f, 0.001f) * ps_r2_sun_lumscale_amb,
        0.f);
    float n = 0.f, f = 1.f;
    da_rt_fog_bounds(n, f);
    const float r = 1.f / std::max(f - n, 0.001f);
    s.fog_params.set(-n * r, n, f, r);
    s.fog_color.set(env.fog_color.x, env.fog_color.y, env.fog_color.z, 0.f);
    s.fog.set(ps_r__fog_sky * ps_r__fog, ps_r__fog_sky_mip, ps_r__fog_height * ps_r__fog, ps_r__fog_height_falloff);
    s.fog2.set(ps_r__fog_max, ps_r__fog_height_base, ps_r__fog_sky_flat, 0.f);
    s.sky_tint.set(env.sky_color.x, env.sky_color.y, env.sky_color.z, 0.f);
    const CEnvironment& environment = g_pGamePersistent->Environment();
    s.rain2.set(environment.rain_rate_mmh, environment.rain_ext_km, 0.f, 0.f);
    s.tonemap.set(0.f, ps_r__tonemap_white, ps_r__tonemap_hue, ps_r__tonemap_desat);
    s.grade.set(ps_r__grade_sat, ps_r__grade_green, ps_r__grade_olive, ps_r__grade_contrast);
    return s;
}

// The GTAO guided filter (s_gtao element 1) from rt_gtao into rt_ssao_temp, as phase_gtao runs it.
void da_rt_gtao_filter(CBackend& cmd_list, CRenderTarget& t)
{
    PIX_EVENT_CTX(cmd_list, rtao_filter);
    u32 offset = 0;
    ref_geom& quad = DaRtAccess::quad(t);
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, quad->vb_stride, offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f);
    pv++;
    RImplementation.Vertex.Unlock(4, quad->vb_stride);
    t.u_setrt(cmd_list, DaRtAccess::ao_target(t), nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_Stencil(FALSE);
    cmd_list.set_Element(DaRtAccess::gtao(t)->E[1]);
    cmd_list.set_Geometry(quad);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
}

void da_rt_debug_pass(CBackend& cmd_list, CRenderTarget& t, DaRtTargets& rt)
{
    if (!ps_r__rt_debug || !g_rt_passes.debug || !da_rt_scene_ready())
        return;
    if (!rt.debug)
        rt.debug.create("$user$rt_debug", rt.width, rt.height, D3DFMT_A16B16G16R16F, 1, {CRT::CreateUAV});
    if (!rt.debug || !rt.debug->pUAView)
        return;
    DaRtDebugConstants c{};
    c.view = da_rt_view(0);
    c.debug[0] = u32(ps_r__rt_debug);
    ID3D11ShaderResourceView* inputs[6] = {da_rt_srv(rt.sun), da_rt_srv(DaRtAccess::ao_target(t)), da_rt_srv(rt.refl),
        da_rt_srv(da_rt_depth(t)), da_rt_srv(t.rt_Color), da_rt_srv(rt.gloss)};
    ID3D11UnorderedAccessView* output = rt.debug->pUAView;
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    DaRtDispatch d;
    d.pipeline = g_rt_passes.debug;
    d.constants = &c;
    d.constants_size = sizeof(c);
    d.scene = scene.tlas;
    d.inputs = inputs;
    d.input_count = 6;
    d.outputs = &output;
    d.output_count = 1;
    for (u32 i = 0; i < da_rt_max_tables; ++i)
        d.tables[i] = scene.tables[i];
    d.groups_x = da_rt_groups(rt.width);
    d.groups_y = da_rt_groups(rt.height);
    bool done;
    {
        DaRtSection section;
        if (section)
            da_rt_require_geometry(section);
        done = section && section.dispatch(d);
    }
    if (done)
        da_rt_debug_blit(cmd_list, t, rt);
}
#endif
} // namespace

#if defined(DA_RT_LANE)
const DaRtTier& da_rt_tier_of(u32 quality) { return kTiers[std::min(quality, 2u)]; }

DaRtPipeline da_rt_skin_pipeline() { return g_rt_passes.skin; }

bool da_rt_gloss_ran()
{
    const DaRtTargets* rt = da_rt_targets();
    return rt && rt->gloss_ran == Device.dwFrame;
}
#endif

void da_rt_passes_create()
{
#if defined(DA_RT_LANE)
    DaRtPasses& p = g_rt_passes;
    if (p.made)
        return;
    p.made = true;
    const DaDxcDefine direct{"RT_SUN_DIRECT", nullptr};
    const DaDxcDefine first{"RT_ATROUS_FIRST", nullptr};
    const DaDxcDefine half{"RT_AO_HALF", nullptr};
    p.sun = da_rt_pipeline("rt/rt_sun_shadow.hlsl", "main");
    p.sun_direct = da_rt_pipeline("rt/rt_sun_shadow.hlsl", "main", &direct, 1);
    p.temporal = da_rt_pipeline("rt/rt_temporal.hlsl", "main");
    p.atrous_first = da_rt_pipeline("rt/rt_atrous.hlsl", "main", &first, 1);
    p.atrous_last = da_rt_pipeline("rt/rt_atrous.hlsl", "main");
    p.ao = da_rt_pipeline("rt/rt_ao.hlsl", "main");
    p.ao_half = da_rt_pipeline("rt/rt_ao.hlsl", "main", &half, 1);
    p.ao_upsample = da_rt_pipeline("rt/rt_ao_upsample.hlsl", "main");
    p.reflections = da_rt_pipeline("rt/rt_reflections.hlsl", "main");
    p.gloss = da_rt_pipeline("rt/rt_gloss.hlsl", "main");
    p.gloss_temporal = da_rt_pipeline("rt/rt_gloss_temporal.hlsl", "main");
    p.skin = da_rt_pipeline("rt/rt_skin.hlsl", "main");
    p.debug = da_rt_pipeline("rt/rt_debug.hlsl", "main");
    p.ready[u32(RtEffect::Shadows)] = p.sun && p.sun_direct && p.temporal && p.atrous_first && p.atrous_last;
    p.ready[u32(RtEffect::AO)] = p.ao && p.ao_half && p.temporal && p.ao_upsample;
    p.ready[u32(RtEffect::Reflections)] = p.reflections;
    Msg("* [rt] passes: reflections %s (glossy surfaces %s), sun shadows %s, occlusion %s, characters %s",
        p.ready[u32(RtEffect::Reflections)] ? "ok" : "failed", p.gloss && p.gloss_temporal ? "ok" : "failed",
        p.ready[u32(RtEffect::Shadows)] ? "ok" : "failed", p.ready[u32(RtEffect::AO)] ? "ok" : "failed", p.skin ? "ok" : "failed");
#endif
}

void da_rt_passes_destroy() { g_rt_passes = {}; }

bool da_rt_pass_ready(RtEffect effect) { return effect < RtEffect::Count && g_rt_passes.ready[u32(effect)]; }

void da_rt_sun_pass(CBackend& cmd_list)
{
    UNUSED(cmd_list);
#if defined(DA_RT_LANE)
    if (!da_rt_live(RtEffect::Shadows))
        return;
    DaRtTargets* rt = da_rt_targets();
    CRenderTarget& t = *RImplementation.Target;
    const DaRtTier& tier = da_rt_tier();
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    const u32 frame = Device.dwFrame;
    const bool history = tier.sun_filter && rt->sun_frame + 1 == frame && !da_history_reset();
    const u32 cur = rt->sun_current, prev = cur ^ 1u;

    dx11GpuTimerScope gpu(dx11GpuTimers::RtSun);
    DaRtSection section;
    if (!section)
        return;
    da_rt_require_geometry(section);
    ID3D11ShaderResourceView* depth = da_rt_srv(da_rt_depth(t));
    ID3D11ShaderResourceView* position = da_rt_srv(t.rt_Position);
    ID3D11ShaderResourceView* normal = RImplementation.o.gbuffer_opt ? nullptr : da_rt_srv(t.rt_Normal);
    const u32 gx = da_rt_groups(Device.dwRenderWidth), gy = da_rt_groups(Device.dwRenderHeight);
    const auto run = [&](DaRtPipeline pipeline, const void* constants, u32 size, ID3D11ShaderResourceView* const* inputs,
                         u32 input_count, ID3D11UnorderedAccessView* output, bool traces) {
        DaRtDispatch d;
        d.pipeline = pipeline;
        d.constants = constants;
        d.constants_size = size;
        d.inputs = inputs;
        d.input_count = input_count;
        d.outputs = &output;
        d.output_count = 1;
        if (traces)
        {
            d.scene = scene.tlas;
            for (u32 i = 0; i < da_rt_max_tables; ++i)
                d.tables[i] = scene.tables[i];
        }
        d.groups_x = gx;
        d.groups_y = gy;
        return section.dispatch(d);
    };

    DaRtSunConstants sun{};
    sun.view = da_rt_view(history ? kViewHistoryValid : 0u);
    // TMax, jitter inside the disc, normal offset base and per metre of depth.
    sun.params.set(1000.f, tier.sun_jitter ? 1.f : 0.f, 0.01f, 0.0015f);
    if (!tier.sun_jitter)
        sun.view.sun.w = 0.f;
    ID3D11ShaderResourceView* trace_in[3] = {depth, position, normal};
    if (!tier.sun_filter)
    {
        // Low: the hard shadow straight into the mask.
        run(g_rt_passes.sun_direct, &sun, sizeof(sun), trace_in, 3, rt->sun->pUAView, true);
        rt->sun_frame = u32(-2);
        return;
    }
    if (!run(g_rt_passes.sun, &sun, sizeof(sun), trace_in, 3, rt->sun_raw->pUAView, true))
        return;

    DaRtTemporalConstants temporal{};
    temporal.view = sun.view;
    temporal.temporal.set(float(Device.dwRenderWidth), float(Device.dwRenderHeight), 1.f, 32.f);
    temporal.temporal2.set(0.1f, 0.f, 0.25f, 0.f);
    ID3D11ShaderResourceView* temporal_in[4] = {da_rt_srv(rt->sun_raw), da_rt_srv(rt->sun_hist[prev]), da_rt_srv(t.rt_Velocity), depth};
    if (!run(g_rt_passes.temporal, &temporal, sizeof(temporal), temporal_in, 4, rt->sun_hist[cur]->pUAView, false))
        return;

    DaRtAtrousConstants atrous{};
    atrous.view = sun.view;
    // Step, the disc's tangent (the penumbra per metre of occluder distance), plane tolerance, normals.
    atrous.atrous.set(1.f, kSunDiscTan, 0.01f, 16.f);
    ID3D11ShaderResourceView* first_in[4] = {da_rt_srv(rt->sun_hist[cur]), depth, position, normal};
    if (!run(g_rt_passes.atrous_first, &atrous, sizeof(atrous), first_in, 4, rt->sun_raw->pUAView, false))
        return;
    atrous.atrous.x = 2.f;
    ID3D11ShaderResourceView* last_in[4] = {da_rt_srv(rt->sun_raw), depth, position, normal};
    if (!run(g_rt_passes.atrous_last, &atrous, sizeof(atrous), last_in, 4, rt->sun->pUAView, false))
        return;
    rt->sun_frame = frame;
    rt->sun_current = prev;
#endif
}

bool da_rt_ao_pass(CBackend& cmd_list)
{
#if defined(DA_RT_LANE)
    if (!da_rt_live(RtEffect::AO))
        return false;
    DaRtTargets* rt = da_rt_targets();
    CRenderTarget& t = *RImplementation.Target;
    const DaRtTier& tier = da_rt_tier();
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    const u32 frame = Device.dwFrame;
    const bool history = rt->ao_frame + 1 == frame && !da_history_reset();
    const u32 cur = rt->ao_current, prev = cur ^ 1u;
    const u32 tw = rt->ao_width, th = rt->ao_height;
    const float scale = rt->ao_half ? 2.f : 1.f;

    dx11GpuTimerScope gpu(dx11GpuTimers::RtAO);
    bool traced = false;
    {
        DaRtSection section;
        if (!section)
            return false;
        da_rt_require_geometry(section);
        ID3D11ShaderResourceView* depth = da_rt_srv(da_rt_depth(t));
        const auto run = [&](DaRtPipeline pipeline, const void* constants, u32 size, ID3D11ShaderResourceView* const* inputs,
                             u32 input_count, ID3D11UnorderedAccessView* output, u32 w, u32 h, bool traces) {
            DaRtDispatch d;
            d.pipeline = pipeline;
            d.constants = constants;
            d.constants_size = size;
            d.inputs = inputs;
            d.input_count = input_count;
            d.outputs = &output;
            d.output_count = 1;
            if (traces)
            {
                d.scene = scene.tlas;
                for (u32 i = 0; i < da_rt_max_tables; ++i)
                    d.tables[i] = scene.tables[i];
            }
            d.groups_x = da_rt_groups(w);
            d.groups_y = da_rt_groups(h);
            return section.dispatch(d);
        };
        DaRtAoConstants ao{};
        ao.view = da_rt_view(history ? kViewHistoryValid : 0u);
        ao.params.set(tier.ao_tmax, kTrunkHeight, float(tw), float(th));
        ID3D11ShaderResourceView* trace_in[3] = {depth, da_rt_srv(t.rt_Position),
            RImplementation.o.gbuffer_opt ? nullptr : da_rt_srv(t.rt_Normal)};
        DaRtTemporalConstants temporal{};
        temporal.view = ao.view;
        temporal.temporal.set(float(tw), float(th), scale, tier.ao_frames);
        temporal.temporal2.set(1.f / tier.ao_frames, 1.f, 0.25f, 0.f);
        ID3D11ShaderResourceView* temporal_in[4] = {da_rt_srv(rt->ao_raw), da_rt_srv(rt->ao_hist[prev]), da_rt_srv(t.rt_Velocity), depth};
        DaRtUpsampleConstants upsample{};
        upsample.view = ao.view;
        upsample.upsample.set(float(tw), float(th), scale, 0.f);
        ID3D11ShaderResourceView* upsample_in[2] = {da_rt_srv(rt->ao_hist[cur]), depth};
        traced = run(rt->ao_half ? g_rt_passes.ao_half : g_rt_passes.ao, &ao, sizeof(ao), trace_in, 3, rt->ao_raw->pUAView, tw, th, true) &&
            run(g_rt_passes.temporal, &temporal, sizeof(temporal), temporal_in, 4, rt->ao_hist[cur]->pUAView, tw, th, false) &&
            run(g_rt_passes.ao_upsample, &upsample, sizeof(upsample), upsample_in, 2, DaRtAccess::gtao_target(t)->pUAView, Device.dwRenderWidth,
                Device.dwRenderHeight, false);
    }
    if (!traced)
        return false;
    rt->ao_frame = frame;
    rt->ao_current = prev;
    da_rt_gtao_filter(cmd_list, t);
    return true;
#else
    UNUSED(cmd_list);
    return false;
#endif
}

#if defined(DA_RT_LANE)
namespace
{
// The glossy reflections run this frame: Medium and High, the renderer started with their targets.
bool da_rt_gloss_wanted(const DaRtTargets* rt)
{
    return rt && rt->gloss && rt->gloss_scene && da_rt_live(RtEffect::Reflections) && da_rt_tier().gloss &&
        g_rt_passes.gloss && g_rt_passes.gloss_temporal;
}
} // namespace
#endif

const ref_rt* da_rt_gloss_scene_target()
{
#if defined(DA_RT_LANE)
    DaRtTargets* rt = da_rt_targets();
    if (!da_rt_gloss_wanted(rt))
        return nullptr;
    rt->gloss_scene_frame = Device.dwFrame;
    return &rt->gloss_scene;
#else
    return nullptr;
#endif
}

void da_rt_gloss_pass(CBackend& cmd_list)
{
    UNUSED(cmd_list);
#if defined(DA_RT_LANE)
    DaRtTargets* rt = da_rt_targets();
    const u32 frame = Device.dwFrame;
    // The previous frame's linear colour is what the screen answers with: without it the frame keeps the cube.
    if (!da_rt_gloss_wanted(rt) || rt->gloss_scene_frame + 1 != frame)
        return;
    CRenderTarget& t = *RImplementation.Target;
    const DaRtTier& tier = da_rt_tier();
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    const bool history = rt->gloss_frame + 1 == frame && !da_history_reset();
    const u32 cur = rt->gloss_current, prev = cur ^ 1u;

    DaRtGlossConstants c{};
    c.view = da_rt_view(0);
    c.shade = da_rt_shade();
    const float pixel_angle = 2.f * c.view.proj.z * c.view.size.w;
    // A glossy surface's image is blurred by its roughness: a shorter reach, no alpha-tested crowns in the rays (the
    // screen-space march still finds those on screen) and a shorter march than the water's mirror needs.
    c.refl.set(std::min({tier.refl_tmax, kGlossReach, std::max(c.shade.fog_params.z, 50.f)}), pixel_angle, 8.f,
        tier.refl_sun_rays ? 1.f : 0.f);
    c.mask[0] = 0xFBu;
    c.mask[1] = std::min(tier.refl_steps, kGlossSteps);
    c.mask[2] = rt->gloss_width;
    c.mask[3] = rt->gloss_height;
    c.gloss.set(kGlossMin, kGlossFull, kGlossSpread, 1.f);
    ID3D11ShaderResourceView* normal = RImplementation.o.gbuffer_opt ? nullptr : da_rt_srv(t.rt_Normal);
    ID3D11ShaderResourceView* trace_in[9] = {da_rt_srv(da_rt_depth(t)), da_rt_srv(t.rt_Position), normal, da_rt_srv(t.rt_Color),
        da_rt_srv(rt->gloss_scene), da_rt_srv(rt->t_env0), da_rt_srv(rt->t_env1), da_rt_srv(rt->t_sky0), da_rt_srv(rt->t_sky1)};
    ID3D11UnorderedAccessView* trace_out[2] = {rt->gloss_raw->pUAView, rt->gloss_dist->pUAView};

    DaRtGlossTemporalConstants tc{};
    tc.view = da_rt_view(history ? kViewHistoryValid : 0u);
    tc.temporal.set(float(rt->gloss_width), float(rt->gloss_height), kGlossMin, kGlossClip);
    tc.temporal2.set(kGlossFresh, 0.f, 0.f, 0.f);
    ID3D11ShaderResourceView* temporal_in[7] = {da_rt_srv(rt->gloss_raw), da_rt_srv(rt->gloss_dist), da_rt_srv(rt->gloss_hist[prev]),
        da_rt_srv(da_rt_depth(t)), da_rt_srv(t.rt_Position), normal, da_rt_srv(t.rt_Color)};
    ID3D11UnorderedAccessView* temporal_out[2] = {rt->gloss_hist[cur]->pUAView, rt->gloss->pUAView};

    bool done;
    {
        dx11GpuTimerScope gpu(dx11GpuTimers::RtGloss);
        DaRtSection section;
        if (!section)
            return;
        da_rt_require_geometry(section);
        DaRtDispatch d;
        d.pipeline = g_rt_passes.gloss;
        d.constants = &c;
        d.constants_size = sizeof(c);
        d.scene = scene.tlas;
        d.inputs = trace_in;
        d.input_count = 9;
        d.outputs = trace_out;
        d.output_count = 2;
        for (u32 i = 0; i < da_rt_max_tables; ++i)
            d.tables[i] = scene.tables[i];
        d.groups_x = da_rt_groups(rt->gloss_width);
        d.groups_y = da_rt_groups(rt->gloss_height);
        done = section.dispatch(d);
        DaRtDispatch u;
        u.pipeline = g_rt_passes.gloss_temporal;
        u.constants = &tc;
        u.constants_size = sizeof(tc);
        u.inputs = temporal_in;
        u.input_count = 7;
        u.outputs = temporal_out;
        u.output_count = 2;
        u.groups_x = da_rt_groups(Device.dwRenderWidth);
        u.groups_y = da_rt_groups(Device.dwRenderHeight);
        done = done && section.dispatch(u);
    }
    if (!done)
        return;
    rt->gloss_frame = frame;
    rt->gloss_current = prev;
    rt->gloss_ran = frame;
#endif
}

void da_rt_reflections_pass(CBackend& cmd_list)
{
#if defined(DA_RT_LANE)
    DaRtTargets* rt = da_rt_targets();
    if (!rt || !RImplementation.Target)
        return;
    CRenderTarget& t = *RImplementation.Target;
    if (da_rt_live(RtEffect::Reflections))
    {
        // The water surfaces the forward pass will draw, collected now (PLAN.md C19).
        t.phase_water_velocity_collect();
        if (da_rt_reflection_requests(cmd_list, t, *rt))
        {
            const DaRtTier& tier = da_rt_tier();
            const DaRtSceneBinding& scene = da_rt_scene_binding();
            DaRtReflectionConstants c{};
            c.view = da_rt_view(0);
            c.shade = da_rt_shade();
            const float pixel_angle = 2.f * c.view.proj.z * c.view.size.w;
            c.refl.set(std::min(tier.refl_tmax, std::max(c.shade.fog_params.z, 50.f)), pixel_angle, 8.f,
                tier.refl_sun_rays ? 1.f : 0.f);
            c.mask[0] = tier.refl_foliage ? 0xFFu : 0xFBu;
            c.mask[1] = tier.refl_steps;
            const u32 gpu_id = Device.dwFrame % HW.Caps.iGPUNum;
            ID3D11ShaderResourceView* inputs[8] = {da_rt_srv(rt->refl_req), da_rt_srv(da_rt_depth(t)), da_rt_srv(t.rt_SSR),
                da_rt_srv(t.rt_LUM_pool[gpu_id * 2 + 1]), da_rt_srv(rt->t_env0), da_rt_srv(rt->t_env1), da_rt_srv(rt->t_sky0),
                da_rt_srv(rt->t_sky1)};
            ID3D11UnorderedAccessView* output = rt->refl->pUAView;
            DaRtDispatch d;
            d.pipeline = g_rt_passes.reflections;
            d.constants = &c;
            d.constants_size = sizeof(c);
            d.scene = scene.tlas;
            d.inputs = inputs;
            d.input_count = 8;
            d.outputs = &output;
            d.output_count = 1;
            for (u32 i = 0; i < da_rt_max_tables; ++i)
                d.tables[i] = scene.tables[i];
            d.groups_x = da_rt_groups(Device.dwRenderWidth);
            d.groups_y = da_rt_groups(Device.dwRenderHeight);
            bool traced;
            {
                dx11GpuTimerScope gpu(dx11GpuTimers::RtRefl);
                DaRtSection section;
                if (section)
                    da_rt_require_geometry(section);
                traced = section && section.dispatch(d);
            }
            // The consumers never read a stale mirror: nothing reflected rather than last frame's.
            if (!traced)
            {
                const float zero[4] = {0.f, 0.f, 0.f, 0.f};
                HW.get_context(CHW::IMM_CTX_ID)->ClearUnorderedAccessViewFloat(rt->refl->pUAView, zero);
            }
        }
    }
    da_rt_debug_pass(cmd_list, t, *rt);
#else
    UNUSED(cmd_list);
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
