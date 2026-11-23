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

#include "Layers/xrRender/light.h"
#if defined(DA_RT_LANE)
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRender/da_veg.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if defined(DA_RT_LANE)
// The combine's haze bounds and an overcast's share off the sky (Blender_Recorder_StandartBinding.cpp).
void da_rt_fog_bounds(float& near_m, float& far_m);
float da_sky_overcast();
void da_rt_cloud_constants(Fvector4& params, Fvector4& params2, Fvector4& map);
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
    DaRtPipeline gi{};
    DaRtPipeline gi_temporal{};
    DaRtPipeline gi_filter{};
    DaRtPipeline lamps{};
    DaRtPipeline gi_cache{};
    DaRtPipeline skin{};
    DaRtPipeline debug{};
#endif
};
DaRtPasses g_rt_passes;

#if defined(DA_RT_LANE)
// ---- constants shared with rt/rt_common.hlsli and rt/rt_material.hlsli ---------------------------

constexpr u32 kViewGbufferPacked = 0x1, kViewHistoryValid = 0x2, kViewVelocity = 0x4;
constexpr float kSunDiscTan = 0.0046542f; // tan(0.27 degrees): the sun's disc
constexpr float kTrunkHeight = 1.2f;

struct DaRtView
{
    Fmatrix view, inv_view, vp, vp_prev;
    Fvector4 pos_decompress, proj, size, camera, sun;
    u32 scene[4];
    Fvector4 frame;
    u32 veg[4]; // the vegetation tables: slot, season block, rows | profile rows << 16, snow (float bits)
    Fvector4 hud; // the first-person projection's pos_decompress (its own field of view)
};
static_assert(sizeof(DaRtView) == 400);

struct DaRtShade
{
    Fvector4 sun_color, env_color, ambient, fog_params, fog_color, fog, fog2, sky_tint, rain2, tonemap, grade;
};
static_assert(sizeof(DaRtShade) == 176);

// A lamp the rays' hits take (rt/rt_material.hlsli RtLight): world position and 1 / range^2, colour and kind (1 a spot,
// 2 carried by the player), the spot's world direction and the cosine of its half cone.
struct DaRtLight
{
    Fvector4 pos, color, dir;
};
struct DaRtLightBlock
{
    Fvector4 falloff; // da_light_falloff: the inverse-square shape's share, core, gain
    u32 count[4]; // x lamps, y the world cache of the traced light is live (0/1)
    DaRtLight lights[kRtHitLights];
};

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
    Fvector4 clouds, clouds2, cloud_map;
    DaRtLightBlock lights;
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
    Fvector4 clouds, clouds2, cloud_map;
    DaRtLightBlock lights;
};
struct DaRtGlossTemporalConstants
{
    DaRtView view;
    Fvector4 temporal, temporal2;
};
struct DaRtGiConstants
{
    DaRtView view;
    DaRtShade shade;
    Fvector4 refl;
    u32 mask[4];
    Fvector4 gi, gi2;
    Fvector4 clouds, clouds2, cloud_map;
    DaRtLightBlock lights;
};
struct DaRtCacheConstants
{
    u32 cache[4]; // x cells, y clear them all (0/1)
    Fvector4 blend; // x the new frame's share of a written cell, y frames an unwritten cell lives
};
struct DaRtGiTemporalConstants
{
    DaRtView view;
    Fvector4 temporal, temporal2;
};
struct DaRtGiFilterConstants
{
    DaRtView view;
    Fvector4 filter;
};
struct DaRtLampConstants
{
    DaRtView view;
    Fvector4 lamps[kRtLampSlots], info[kRtLampSlots];
    u32 count[4];
};
static_assert(sizeof(DaRtLampConstants) <= da_rt_max_constants && sizeof(DaRtGiConstants) <= da_rt_max_constants &&
    sizeof(DaRtGlossConstants) <= da_rt_max_constants && sizeof(DaRtReflectionConstants) <= da_rt_max_constants);

// The lamps the rays shade this frame (da_rt_lamps_select): the owners (a point lamp's, for its faces) by mask slot.
struct DaRtLampFrame
{
    u32 frame{u32(-1)};
    u32 count{};
    light* owners[kRtLampSlots]{};
    // The stock lit it without a shadow: its fixture may sink into the ceiling it hangs from.
    bool unshadowed[kRtLampSlots]{};
    bool traced{};
} g_rt_lamps;
// The reach of a lamp's own fixture its shadow rays stop short of, metres; for a lamp the stock lit unshadowed (placed
// knowing nothing would shadow it, often sunk into its ceiling or wall) a share of its range, to a ceiling.
constexpr float kLampFixture = 0.15f, kLampFixtureUnshadowed = 0.06f, kLampFixtureMax = 0.6f;
// The lamps the rays' hits take this frame (da_rt_lamps_select): the frame's most important lit lamps.
struct DaRtHitLights
{
    u32 frame{u32(-1)};
    u32 count{};
    DaRtLight lights[kRtHitLights]{};
    // Each one's lamp and weight: the frame's second light package (the occlusion's) merges into the first's.
    const light* who[kRtHitLights]{};
    float weight[kRtHitLights]{};
} g_rt_hit_lights;
// The world cache of the traced light (dxr.md 3.15): a written cell's new share, the frames an unwritten one lives.
constexpr float kCacheBlend = 0.15f, kCacheLife = 300.f;

// The global illumination (dxr.md 3.11): the glow of an emissive hit, the history clip in sigmas, the least alpha, the least share of a pixel's own fresh sample,
// the filter's plane tolerance per metre, normal power and luminance sigma.
constexpr float kGiGlow = 1.f, kGiClip = 2.5f, kGiAlpha = 0.02f, kGiOwn = 0.35f;
// The light a crown lets through to what lies under it, and the reach of the crowns' test.
constexpr float kGiCanopy = 0.3f, kGiCanopyReach = 40.f;
constexpr float kGiPlane = 0.02f, kGiNormal = 32.f, kGiLuma = 4.f;

// The glossy reflections (dxr.md 3.9): the lowest gloss traced, the gloss of the full weight, the spread of a
// rough surface's rays (tangent per roughness squared), the history clip in sigmas, the least fresh share.
constexpr float kGlossMin = 0.03f, kGlossFull = 0.12f, kGlossSpread = 0.35f, kGlossClip = 1.5f, kGlossFresh = 0.1f;
// The glossy rays' reach in metres and their screen-space steps at most.
constexpr float kGlossReach = 250.f;
constexpr u32 kGlossSteps = 16u;

// The tiers of r__rt_quality (dxr.md 10): Low, Medium, High, Ultra. The sun disc is the real one; a bulb's radius.
constexpr float kSunDisc = kSunDiscTan;
constexpr float kLampBulb = 0.05f;
constexpr DaRtTier kTiers[4] = {
    {false, false, true, 1.0f, 16.f, 250.f, false, false, 40.f, 20.f, 6u, 4u, 16u, false, 1u, 0.f, false, false, true, 1u, 25.f, 24.f, 2u, false, 0u, 1u, 0.f, false},
    {true, true, true, 1.5f, 32.f, 400.f, true, true, 60.f, 30.f, 12u, 8u, 24u, true, 1u, kSunDisc, false, false, true, 1u, 35.f, 32.f, 2u, true, 8u, 1u, kLampBulb, true},
    {true, true, false, 2.0f, 32.f, 1000.f, true, true, 80.f, 40.f, 16u, 12u, 32u, true, 1u, kSunDisc, false, false, true, 1u, 50.f, 32.f, 3u, true, 16u, 1u, kLampBulb, true},
    {true, true, false, 2.5f, 48.f, 1500.f, true, true, 120.f, 60.f, 24u, 16u, 48u, true, 2u, kSunDisc, true, true, false, 1u, 60.f, 48.f, 3u, true, 32u, 2u, kLampBulb, true},
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
    v.hud = da_pos_decompress(psHUD_FOV * Device.fFOV);
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
    // (The raster haze's tint, r2.cpp: an overcast's darkened sky the far hits converge to as the frame's do.)
    const float overcast = da_sky_overcast(), sky_k = 1.f - overcast;
    s.sky_tint.set(env.sky_color.x * sky_k, env.sky_color.y * sky_k, env.sky_color.z * sky_k, overcast);
    const CEnvironment& environment = g_pGamePersistent->Environment();
    s.rain2.set(environment.rain_rate_mmh, environment.rain_ext_km, 0.f, 0.f);
    s.tonemap.set(0.f, ps_r__tonemap_white, ps_r__tonemap_hue, ps_r__tonemap_desat);
    s.grade.set(ps_r__grade_sat, ps_r__grade_green, ps_r__grade_olive, ps_r__grade_contrast);
    return s;
}

// The lamps the rays' hits take this frame and whether the world cache of the traced light holds anything, for a pass's
// constants; the pass binds the cache (da_rt_cache_address).
void da_rt_light_block(DaRtLightBlock& b, const DaRtTargets* rt)
{
    b.falloff.set(ps_r__light_falloff, ps_r__light_falloff_core, ps_r__light_falloff_gain, 0.f);
    b.count[0] = g_rt_hit_lights.frame == Device.dwFrame ? g_rt_hit_lights.count : 0u;
    // Live while the global illumination keeps it (it ran this frame, before the reflections).
    b.count[1] = rt && rt->gi_cache && !rt->gi_cache_clear && da_rt_tier().gi_cache && rt->gi_ran == Device.dwFrame ? 1u : 0u;
    for (u32 i = 0; i < b.count[0]; ++i)
        b.lights[i] = g_rt_hit_lights.lights[i];
}

DaRtAddress da_rt_cache_address(const DaRtTargets* rt) { return rt && rt->gi_cache ? da_rt_address(rt->gi_cache) : 0; }

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
    ID3D11ShaderResourceView* inputs[10] = {da_rt_srv(rt.sun), da_rt_srv(DaRtAccess::ao_target(t)), da_rt_srv(rt.refl),
        da_rt_srv(da_rt_depth(t)), da_rt_srv(t.rt_Color), da_rt_srv(rt.gloss), da_rt_srv(rt.gi), da_rt_srv(rt.lamps),
        da_rt_srv(t.rt_Position), RImplementation.o.gbuffer_opt ? nullptr : da_rt_srv(t.rt_Normal)};
    ID3D11UnorderedAccessView* output = rt.debug->pUAView;
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    DaRtDispatch d;
    d.pipeline = g_rt_passes.debug;
    d.constants = &c;
    d.constants_size = sizeof(c);
    d.scene = scene.tlas;
    d.inputs = inputs;
    d.input_count = 10;
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
const DaRtTier& da_rt_tier_of(u32 quality) { return kTiers[std::min(quality, 3u)]; }

DaRtPipeline da_rt_skin_pipeline() { return g_rt_passes.skin; }

bool da_rt_gloss_ran()
{
    const DaRtTargets* rt = da_rt_targets();
    return rt && rt->gloss_ran == Device.dwFrame;
}

bool da_rt_gi_ran()
{
    const DaRtTargets* rt = da_rt_targets();
    return rt && rt->gi_ran == Device.dwFrame;
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
    p.gi = da_rt_pipeline("rt/rt_gi.hlsl", "main");
    p.gi_temporal = da_rt_pipeline("rt/rt_gi_temporal.hlsl", "main");
    p.gi_filter = da_rt_pipeline("rt/rt_gi_filter.hlsl", "main");
    p.lamps = da_rt_pipeline("rt/rt_light_shadow.hlsl", "main");
    p.gi_cache = da_rt_pipeline("rt/rt_gi_cache.hlsl", "main");
    p.skin = da_rt_pipeline("rt/rt_skin.hlsl", "main");
    p.debug = da_rt_pipeline("rt/rt_debug.hlsl", "main");
    p.ready[u32(RtEffect::Shadows)] = p.sun && p.sun_direct && p.temporal && p.atrous_first && p.atrous_last;
    p.ready[u32(RtEffect::AO)] = p.ao && p.ao_half && p.temporal && p.ao_upsample;
    p.ready[u32(RtEffect::Reflections)] = p.reflections;
    p.ready[u32(RtEffect::GI)] = p.gi && p.gi_temporal && p.gi_filter;
    Msg("* [rt] passes: reflections %s (glossy surfaces %s), sun shadows %s, occlusion %s, global illumination %s, characters %s",
        p.ready[u32(RtEffect::Reflections)] ? "ok" : "failed", p.gloss && p.gloss_temporal ? "ok" : "failed",
        p.ready[u32(RtEffect::Shadows)] ? "ok" : "failed", p.ready[u32(RtEffect::AO)] ? "ok" : "failed",
        p.ready[u32(RtEffect::GI)] ? "ok" : "failed", p.skin ? "ok" : "failed");
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
    // TMax, rays a pixel inside the disc (0: one, unjittered), normal offset base and per metre of depth.
    sun.params.set(1000.f, tier.sun_jitter ? float(tier.sun_rays) : 0.f, 0.01f, 0.0015f);
    sun.view.sun.w = tier.sun_jitter ? tier.sun_disc : 0.f;
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
    // Where a moving object casts, half of each frame is new: its shadow follows it.
    temporal.temporal2.set(0.1f, 0.f, 0.25f, 0.5f);
    ID3D11ShaderResourceView* temporal_in[4] = {da_rt_srv(rt->sun_raw), da_rt_srv(rt->sun_hist[prev]), da_rt_srv(t.rt_Velocity), depth};
    if (!run(g_rt_passes.temporal, &temporal, sizeof(temporal), temporal_in, 4, rt->sun_hist[cur]->pUAView, false))
        return;

    DaRtAtrousConstants atrous{};
    atrous.view = sun.view;
    // Step, the disc's tangent (the penumbra per metre of occluder distance), plane tolerance, normals.
    atrous.atrous.set(1.f, tier.sun_disc, 0.01f, 16.f);
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
// The glossy reflections run this frame: Medium and up, the renderer started with their targets.
bool da_rt_gloss_wanted(const DaRtTargets* rt)
{
    return rt && rt->gloss && rt->gloss_scene && da_rt_live(RtEffect::Reflections) && da_rt_tier().gloss &&
        g_rt_passes.gloss && g_rt_passes.gloss_temporal;
}

// The global illumination runs this frame.
bool da_rt_gi_wanted(const DaRtTargets* rt) { return rt && rt->gi && da_rt_live(RtEffect::GI); }
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
    // Ultra's rays see the crowns too.
    c.mask[0] = tier.gloss_foliage ? 0xFFu : 0xFBu;
    c.mask[1] = std::min(tier.refl_steps, kGlossSteps);
    c.mask[2] = rt->gloss_width;
    c.mask[3] = rt->gloss_height;
    c.gloss.set(kGlossMin, kGlossFull, kGlossSpread, 1.f);
    da_rt_cloud_constants(c.clouds, c.clouds2, c.cloud_map);
    da_rt_light_block(c.lights, rt);
    ID3D11ShaderResourceView* normal = RImplementation.o.gbuffer_opt ? nullptr : da_rt_srv(t.rt_Normal);
    ID3D11ShaderResourceView* trace_in[10] = {da_rt_srv(da_rt_depth(t)), da_rt_srv(t.rt_Position), normal, da_rt_srv(t.rt_Color),
        da_rt_srv(rt->gloss_scene), da_rt_srv(rt->t_env0), da_rt_srv(rt->t_env1), da_rt_srv(rt->t_sky0), da_rt_srv(rt->t_sky1),
        da_rt_srv(rt->t_clouds)};
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
        d.input_count = 10;
        d.outputs = trace_out;
        d.output_count = 2;
        for (u32 i = 0; i < da_rt_max_tables; ++i)
            d.tables[i] = scene.tables[i];
        d.raw_output = da_rt_cache_address(rt);
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

void da_rt_gi_pass(CBackend& cmd_list)
{
    UNUSED(cmd_list);
#if defined(DA_RT_LANE)
    DaRtTargets* rt = da_rt_targets();
    const u32 frame = Device.dwFrame;
    if (!da_rt_gi_wanted(rt))
        return;
    CRenderTarget& t = *RImplementation.Target;
    const DaRtTier& tier = da_rt_tier();
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    const bool history = rt->gi_frame + 1 == frame && !da_history_reset();
    const u32 cur = rt->gi_current, prev = cur ^ 1u;
    // The lights this frame gathered (the sun's and the lamps' diffuse, before the combine) and the albedo: what leaves a
    // hit the screen shows.
    const bool accumulated = t.rt_Accumulator && t.rt_Color;

    DaRtGiConstants c{};
    c.view = da_rt_view(0);
    c.shade = da_rt_shade();
    const float pixel_angle = 2.f * c.view.proj.z * c.view.size.w;
    // Reach, cone spread per metre, the screen's edge band, the hits' own sun rays.
    c.refl.set(tier.gi_tmax, pixel_angle, 8.f, 1.f);
    // The world but the crowns and the player's own shadow model (the rays leave the ground around him); the tier's
    // canopy test lets the crowns' cards dim what passes them.
    c.mask[0] = 0x7Bu;
    c.mask[1] = tier.gi_rays;
    c.mask[2] = rt->gi_width;
    c.mask[3] = rt->gi_height;
    c.gi.set(rt->gi_half ? 1.f : 0.f, accumulated ? 1.f : 0.f, 0.f, kGiGlow);
    c.gi2.set(tier.gi_foliage ? kGiCanopy : 1.f, kGiCanopyReach, 0.f, 0.f);
    da_rt_cloud_constants(c.clouds, c.clouds2, c.cloud_map);
    // The world cache of the traced light (dxr.md 3.15): cleared when made and for every new scene, else written by
    // this pass's pixels and resolved after it.
    const bool cache = rt->gi_cache && tier.gi_cache && g_rt_passes.gi_cache;
    const bool cache_clear = cache && (rt->gi_cache_clear || rt->gi_cache_epoch != da_rt_scene_epoch());
    if (cache_clear)
        rt->gi_cache_clear = true;
    da_rt_light_block(c.lights, rt);
    c.lights.count[1] = cache && !cache_clear ? 1u : 0u;
    // z: the pass writes the cache.
    c.gi2.z = cache ? 1.f : 0.f;
    ID3D11ShaderResourceView* depth = da_rt_srv(da_rt_depth(t));
    ID3D11ShaderResourceView* position = da_rt_srv(t.rt_Position);
    ID3D11ShaderResourceView* normal = RImplementation.o.gbuffer_opt ? nullptr : da_rt_srv(t.rt_Normal);
    ID3D11ShaderResourceView* trace_in[10] = {depth, position, normal, accumulated ? da_rt_srv(t.rt_Accumulator) : nullptr,
        da_rt_srv(rt->t_env0), da_rt_srv(rt->t_env1), da_rt_srv(rt->t_clouds), da_rt_srv(rt->t_sky0), da_rt_srv(rt->t_sky1),
        accumulated ? da_rt_srv(t.rt_Color) : nullptr};
    ID3D11UnorderedAccessView* trace_out[2] = {rt->gi_raw->pUAView, rt->gi_z->pUAView};

    DaRtGiTemporalConstants tc{};
    tc.view = da_rt_view(history ? kViewHistoryValid : 0u);
    tc.temporal.set(float(rt->gi_width), float(rt->gi_height), rt->gi_half ? 1.f : 0.f, tier.gi_frames);
    // w: the filtered light of the pixels goes to the world cache of the traced light.
    tc.temporal2.set(kGiAlpha, kGiClip, kGiOwn, cache && !cache_clear ? 1.f : 0.f);
    ID3D11ShaderResourceView* temporal_in[8] = {da_rt_srv(rt->gi_raw), da_rt_srv(rt->gi_z), da_rt_srv(rt->gi_hist[prev]),
        da_rt_srv(rt->gi_aux[prev]), depth, position, normal, da_rt_srv(t.rt_Velocity)};
    ID3D11UnorderedAccessView* temporal_out[2] = {rt->gi_hist[cur]->pUAView, rt->gi_aux[cur]->pUAView};

    bool done;
    {
        dx11GpuTimerScope gpu(dx11GpuTimers::RtGI);
        DaRtSection section;
        if (!section)
            return;
        da_rt_require_geometry(section);
        DaRtCacheConstants cc{};
        cc.cache[0] = kRtGiCacheCells;
        cc.blend.set(kCacheBlend, kCacheLife, 0.f, 0.f);
        const auto cache_pass = [&](bool clear) {
            cc.cache[1] = clear ? 1u : 0u;
            DaRtDispatch k;
            k.pipeline = g_rt_passes.gi_cache;
            k.constants = &cc;
            k.constants_size = sizeof(cc);
            k.raw_output = da_rt_cache_address(rt);
            k.groups_x = (kRtGiCacheCells + 63) / 64;
            const bool ok = section.dispatch(k);
            section.uav_barrier();
            return ok;
        };
        if (cache_clear && cache_pass(true))
        {
            rt->gi_cache_clear = false;
            rt->gi_cache_epoch = da_rt_scene_epoch();
        }
        DaRtDispatch d;
        d.pipeline = g_rt_passes.gi;
        d.constants = &c;
        d.constants_size = sizeof(c);
        d.scene = scene.tlas;
        d.inputs = trace_in;
        d.input_count = 10;
        d.outputs = trace_out;
        d.output_count = 2;
        for (u32 i = 0; i < da_rt_max_tables; ++i)
            d.tables[i] = scene.tables[i];
        d.raw_output = cache ? da_rt_cache_address(rt) : 0;
        d.groups_x = da_rt_groups(rt->gi_width);
        d.groups_y = da_rt_groups(rt->gi_height);
        done = section.dispatch(d);
        // The trace read the cache; the temporal pass writes the frame's samples into it.
        if (cache)
            section.uav_barrier();
        DaRtDispatch u;
        u.pipeline = g_rt_passes.gi_temporal;
        u.constants = &tc;
        u.constants_size = sizeof(tc);
        u.inputs = temporal_in;
        u.input_count = 8;
        u.outputs = temporal_out;
        u.output_count = 2;
        u.raw_output = cache && !rt->gi_cache_clear ? da_rt_cache_address(rt) : 0;
        u.groups_x = da_rt_groups(Device.dwRenderWidth);
        u.groups_y = da_rt_groups(Device.dwRenderHeight);
        done = done && section.dispatch(u);
        // The frame's samples into the cache's light, before the reflections and the next frame read it.
        if (done && cache && !rt->gi_cache_clear)
        {
            section.uav_barrier();
            cache_pass(false);
        }
        // The a-trous passes, steps 1, 2, 4 (the tier's count): the history into the result, through the spare.
        const u32 passes = std::max(tier.gi_filter, 1u);
        ID3D11ShaderResourceView* source = da_rt_srv(rt->gi_hist[cur]);
        for (u32 i = 0; i < passes && done; ++i)
        {
            // The last pass writes the result; before it, the result and the spare take turns.
            const bool last = i + 1 == passes;
            const ref_rt& target = last ? rt->gi : ((passes - i) % 2 ? rt->gi : rt->gi_tmp);
            DaRtGiFilterConstants fc{};
            fc.view = tc.view;
            fc.filter.set(float(1u << i), kGiPlane, kGiNormal, kGiLuma);
            ID3D11ShaderResourceView* filter_in[5] = {source, da_rt_srv(rt->gi_aux[cur]), depth, position, normal};
            ID3D11UnorderedAccessView* filter_out = target->pUAView;
            DaRtDispatch f;
            f.pipeline = g_rt_passes.gi_filter;
            f.constants = &fc;
            f.constants_size = sizeof(fc);
            f.inputs = filter_in;
            f.input_count = 5;
            f.outputs = &filter_out;
            f.output_count = 1;
            f.groups_x = da_rt_groups(Device.dwRenderWidth);
            f.groups_y = da_rt_groups(Device.dwRenderHeight);
            done = section.dispatch(f);
            source = da_rt_srv(target);
        }
    }
    if (!done)
        return;
    rt->gi_frame = frame;
    rt->gi_current = prev;
    rt->gi_ran = frame;
#endif
}

void da_rt_lamps_select(xr_vector<light*>& shadowed, const xr_vector<light*>& unshadowed, xr_vector<light*>& traced, bool volumetric)
{
    traced.clear();
#if defined(DA_RT_LANE)
    g_rt_lamps.count = 0;
    g_rt_lamps.traced = false;
    g_rt_lamps.frame = Device.dwFrame;
    if (g_rt_hit_lights.frame != Device.dwFrame)
    {
        g_rt_hit_lights.count = 0;
        g_rt_hit_lights.frame = Device.dwFrame;
    }
    if (!da_rt_scene_ready())
        return;
    // The owners, each once (a point lamp's faces go together), shadowed or not, with what keeps a shadowed one on its
    // map: the procedural shrubs cast into it and are no part of the scene; a sphere reaching past the characters the
    // scene traces round the camera (they would cast into it no more). An unshadowed lamp had no shadow at all: the
    // rays only add one.
    struct Owner
    {
        light* L;
        float score;
        bool keep;
        bool unshadowed;
    };
    static xr_vector<Owner> owners;
    owners.clear();
    const Fvector& eye = Device.vCameraPosition;
    const float reach = da_rt_tier().skinned_radius;
    const auto owner_of = [](light* f) { return f->omnipart_owner ? f->omnipart_owner : f; };
    const auto entry = [&](light* L, bool shadowed_lamp) -> Owner& {
        auto it = std::find_if(owners.begin(), owners.end(), [L](const Owner& o) { return o.L == L; });
        if (it != owners.end())
            return *it;
        const float d = L->position.distance_to(eye);
        owners.push_back({L, std::max(d - L->range, 0.f), shadowed_lamp && d + L->range > reach, !shadowed_lamp});
        return owners.back();
    };
    for (light* f : shadowed)
    {
        Owner& o = entry(owner_of(f), true);
        if (da_veg_light_casts(f))
            o.keep = true;
    }
    for (light* f : unshadowed)
        entry(owner_of(f), false);

    // The lights the rays' hits take: every lamp lit this frame by its light at the camera's surroundings (its
    // brightness over the distance to its sphere), the brightest kRtHitLights of both light packages.
    {
        static xr_vector<std::pair<float, const light*>> by_weight;
        by_weight.clear();
        for (u32 i = 0; i < g_rt_hit_lights.count; ++i)
            by_weight.push_back({g_rt_hit_lights.weight[i], g_rt_hit_lights.who[i]});
        for (const Owner& o : owners)
        {
            const light* L = o.L;
            const float lum = 0.2126f * L->color.r + 0.7152f * L->color.g + 0.0722f * L->color.b;
            const float d = std::max(o.score, 0.5f * L->range);
            const bool known = std::any_of(by_weight.begin(), by_weight.end(), [L](const auto& w) { return w.second == L; });
            if (!known && lum > 1e-3f && L->range > 0.05f)
                by_weight.push_back({lum * L->range * L->range / (d * d), L});
        }
        std::sort(by_weight.begin(), by_weight.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : std::less<const light*>()(a.second, b.second);
        });
        const u32 n = std::min(u32(by_weight.size()), kRtHitLights);
        for (u32 i = 0; i < n; ++i)
        {
            const light* L = by_weight[i].second;
            g_rt_hit_lights.who[i] = L;
            g_rt_hit_lights.weight[i] = by_weight[i].first;
            const float R = std::max(L->range * .95f, 0.01f);
            DaRtLight& out = g_rt_hit_lights.lights[i];
            out.pos.set(L->position.x, L->position.y, L->position.z, 1.f / (R * R));
            // A spot by its cone (the faces of a point lamp light every way); a spot fades out as its volume does.
            const bool spot = L->flags.type == IRender_Light::SPOT;
            const float lod = spot ? L->get_LOD() : 1.f;
            out.color.set(L->color.r * lod, L->color.g * lod, L->color.b * lod,
                (spot ? 1.f : 0.f) + (L->flags.bNeverDemote ? 2.f : 0.f));
            Fvector d = L->direction;
            d.normalize_safe();
            out.dir.set(d.x, d.y, d.z, spot ? _cos(L->cone * .5f) : -2.f);
        }
        g_rt_hit_lights.count = n;
    }

    const DaRtTargets* rt = da_rt_targets();
    if (!rt || !rt->lamps || !g_rt_passes.lamps || !da_rt_live(RtEffect::Shadows))
        return;
    const u32 wanted = std::min(da_rt_tier().lamps_max, kRtLampSlots);
    if (!wanted)
        return;
    owners.erase(std::remove_if(owners.begin(), owners.end(), [](const Owner& o) { return o.keep; }), owners.end());
    std::sort(owners.begin(), owners.end(), [](const Owner& a, const Owner& b) {
        return a.score != b.score ? a.score < b.score : std::less<light*>()(a.L, b.L);
    });
    if (owners.size() > wanted)
        owners.resize(wanted);
    if (owners.empty())
        return;
    for (const Owner& o : owners)
    {
        g_rt_lamps.unshadowed[g_rt_lamps.count] = o.unshadowed;
        g_rt_lamps.owners[g_rt_lamps.count++] = o.L;
    }
    // The chosen shadowed faces out of the atlas's list (its order kept: the batches read it back to front). A face with
    // a volumetric beam keeps its map for the beam's march; its surfaces still take the rays' shadow (da_lv_draw).
    const auto chosen = [volumetric](const light* f) {
        const light* L = f->omnipart_owner ? f->omnipart_owner : f;
        return da_rt_lamp_slot(f) >= 0 && !(volumetric && (f->flags.bVolumetric || L->flags.bVolumetric));
    };
    for (light* f : shadowed)
    {
        if (chosen(f))
            traced.push_back(f);
    }
    shadowed.erase(std::remove_if(shadowed.begin(), shadowed.end(), chosen), shadowed.end());
#else
    UNUSED(shadowed);
    UNUSED(unshadowed);
    UNUSED(volumetric);
#endif
}

int da_rt_lamp_slot(const light* L)
{
#if defined(DA_RT_LANE)
    if (!L || g_rt_lamps.frame != Device.dwFrame)
        return -1;
    const light* owner = L->omnipart_owner ? L->omnipart_owner : L;
    for (u32 i = 0; i < g_rt_lamps.count; ++i)
    {
        if (g_rt_lamps.owners[i] == owner)
            return int(i);
    }
#else
    UNUSED(L);
#endif
    return -1;
}

ID3D11ShaderResourceView* da_rt_lamp_view()
{
#if defined(DA_RT_LANE)
    const DaRtTargets* rt = da_rt_targets();
    if (rt && g_rt_lamps.traced && g_rt_lamps.frame == Device.dwFrame)
        return da_rt_srv(rt->lamps);
#endif
    return nullptr;
}

void da_rt_lamps_pass(CBackend& cmd_list)
{
    UNUSED(cmd_list);
#if defined(DA_RT_LANE)
    DaRtTargets* rt = da_rt_targets();
    if (!rt || !rt->lamps || !g_rt_lamps.count || g_rt_lamps.frame != Device.dwFrame)
        return;
    CRenderTarget& t = *RImplementation.Target;
    const DaRtTier& tier = da_rt_tier();
    const DaRtSceneBinding& scene = da_rt_scene_binding();
    ID3D11ShaderResourceView* depth = da_rt_srv(da_rt_depth(t));
    ID3D11ShaderResourceView* position = da_rt_srv(t.rt_Position);
    ID3D11ShaderResourceView* normal = RImplementation.o.gbuffer_opt ? nullptr : da_rt_srv(t.rt_Normal);
    ID3D11ShaderResourceView* inputs[3] = {depth, position, normal};
    // Every lamp in one dispatch: a pixel loops over the lamps whose reach it is in, the slices of the lamps beyond the
    // count left lit.
    DaRtLampConstants c{};
    c.view = da_rt_view(0);
    for (u32 i = 0; i < g_rt_lamps.count; ++i)
    {
        const light* L = g_rt_lamps.owners[i];
        const float R = std::max(L->range * .95f, 0.01f);
        c.lamps[i].set(L->position.x, L->position.y, L->position.z, 1.f / (R * R));
        const float fixture = g_rt_lamps.unshadowed[i] ? std::clamp(kLampFixtureUnshadowed * L->range, kLampFixture, kLampFixtureMax) : kLampFixture;
        c.info[i].set(tier.lamp_bulb, fixture, L->flags.bNeverDemote ? 1.f : 0.f, 0.f);
    }
    c.count[0] = g_rt_lamps.count;
    c.count[1] = tier.lamp_rays;
    c.count[2] = kRtLampMasks;
    bool done = false;
    {
        dx11GpuTimerScope gpu(dx11GpuTimers::RtLamps);
        DaRtSection section;
        if (section)
        {
            da_rt_require_geometry(section);
            ID3D11UnorderedAccessView* output = rt->lamps->pUAView;
            DaRtDispatch d;
            d.pipeline = g_rt_passes.lamps;
            d.constants = &c;
            d.constants_size = sizeof(c);
            d.scene = scene.tlas;
            d.inputs = inputs;
            d.input_count = 3;
            d.outputs = &output;
            d.output_count = 1;
            for (u32 i = 0; i < da_rt_max_tables; ++i)
                d.tables[i] = scene.tables[i];
            d.groups_x = da_rt_groups(Device.dwRenderWidth);
            d.groups_y = da_rt_groups(Device.dwRenderHeight);
            done = section.dispatch(d);
        }
    }
    g_rt_lamps.traced = true;
    // Not traced: the lamps light unshadowed rather than through a stale mask.
    if (!done && rt->lamps->pUAView)
    {
        const float lit[4] = {1.f, 1.f, 1.f, 1.f};
        HW.get_context(CHW::IMM_CTX_ID)->ClearUnorderedAccessViewFloat(rt->lamps->pUAView, lit);
    }
    rt->lamps_frame = Device.dwFrame;
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
            da_rt_cloud_constants(c.clouds, c.clouds2, c.cloud_map);
            da_rt_light_block(c.lights, rt);
            const u32 gpu_id = Device.dwFrame % HW.Caps.iGPUNum;
            ID3D11ShaderResourceView* inputs[9] = {da_rt_srv(rt->refl_req), da_rt_srv(da_rt_depth(t)), da_rt_srv(t.rt_SSR),
                da_rt_srv(t.rt_LUM_pool[gpu_id * 2 + 1]), da_rt_srv(rt->t_env0), da_rt_srv(rt->t_env1), da_rt_srv(rt->t_sky0),
                da_rt_srv(rt->t_sky1), da_rt_srv(rt->t_clouds)};
            ID3D11UnorderedAccessView* output = rt->refl->pUAView;
            DaRtDispatch d;
            d.pipeline = g_rt_passes.reflections;
            d.constants = &c;
            d.constants_size = sizeof(c);
            d.scene = scene.tlas;
            d.inputs = inputs;
            d.input_count = 9;
            d.outputs = &output;
            d.output_count = 1;
            for (u32 i = 0; i < da_rt_max_tables; ++i)
                d.tables[i] = scene.tables[i];
            d.raw_output = da_rt_cache_address(rt);
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
