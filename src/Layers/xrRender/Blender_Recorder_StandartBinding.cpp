#include "stdafx.h"
#pragma hdrstop

#include "ResourceManager.h"
#include "Blender_Recorder.h"
#include "Blender.h"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrEngine/EnvironmentWeatherState.h"
#include "xrEngine/thunderbolt.h"
#include "xrEngine/da_season.h"
#include "xrEngine/da_snow_field.h"
#include "xrEngine/da_climate.h"
#include "xrEngine/da_ice.h"
#include "xrEngine/da_media.h"
#if RENDER == R_R4
#include "Layers/xrRender/da_wind_history.h"
#include "Layers/xrRender/da_snow.h"
#include "Layers/xrRender/da_grass_blades.h"
#include "Layers/xrRenderPC_R4/da_rt.h"
#endif

// Declared BEFORE the render namespace opens on purpose: an extern inside it would introduce
// its own xray::render::*::psVisDistance that nothing defines, and the build dies at link.
// The variable lives in the engine (Environment.cpp).
extern ENGINE_API float psVisDistance;
// 3D PDA screen state (same rule; both live in xr_ioc_cmd.cpp).
extern ENGINE_API Fvector4 g_pda_screen_affects;
extern ENGINE_API Fvector4 g_pda_screen_rect;
extern ENGINE_API Fvector4 g_pda_taa_bbox;

extern ENGINE_API float psHUD_FOV;

// The water and rain quality ladders (same rule again; defined in xr_ioc_cmd.cpp because the
// solvers that read them live in CEnvironment, while the presets that write them live here).
extern ENGINE_API int ps_r__water_underwater;
extern ENGINE_API int ps_r__water_caustics;
extern ENGINE_API int ps_r__rain_quality;

namespace xray::render::RENDER_NAMESPACE
{
// matrices
#define BIND_DECLARE(xf)\
    class cl_xform_##xf : public R_constant_setup\
    {\
        void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.xforms.set_c_##xf(C); }\
    };\
    static cl_xform_##xf binder_##xf
BIND_DECLARE(w);
BIND_DECLARE(invw);
BIND_DECLARE(v);
BIND_DECLARE(p);
BIND_DECLARE(wv);
BIND_DECLARE(vp);
BIND_DECLARE(wvp);
// Motion vectors (shaders/r3/da_velocity.h): per draw through R_xforms, like m_WVP.
BIND_DECLARE(vp_nojit);
BIND_DECLARE(vp_prev);
BIND_DECLARE(wvp_nojit);
BIND_DECLARE(wvp_prev);
BIND_DECLARE(w_prev);

#define DECLARE_TREE_BIND(c)\
    class cl_tree_##c : public R_constant_setup\
    {\
        void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.tree.set_c_##c(C); }\
    };\
    static cl_tree_##c tree_binder_##c

DECLARE_TREE_BIND(m_xform_v);
DECLARE_TREE_BIND(m_xform);
DECLARE_TREE_BIND(consts);
DECLARE_TREE_BIND(wave);
DECLARE_TREE_BIND(wind);
DECLARE_TREE_BIND(c_scale);
DECLARE_TREE_BIND(c_tree);
DECLARE_TREE_BIND(c_bias);
DECLARE_TREE_BIND(c_sun);
// The previous frame's wind of the tree G-buffer shaders (motion vectors).
DECLARE_TREE_BIND(wave_prev);
DECLARE_TREE_BIND(wind_prev);
DECLARE_TREE_BIND(c_tree_prev);

class cl_hemi_cube_pos_faces : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.hemi.set_c_pos_faces(C); }
};

static cl_hemi_cube_pos_faces binder_hemi_cube_pos_faces;

class cl_hemi_cube_neg_faces : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.hemi.set_c_neg_faces(C); }
};

static cl_hemi_cube_neg_faces binder_hemi_cube_neg_faces;

class cl_material : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.hemi.set_c_material(C); }
};

static cl_material binder_material;

class cl_texgen : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix mTexgen;

#if defined(USE_DX11)
        Fmatrix mTexelAdjust =
        {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f
        };
#elif defined(USE_OGL)
        Fmatrix mTexelAdjust =
        {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f
        };
#else
#    error No graphics API selected or in use!
#endif

        mTexgen.mul(mTexelAdjust, cmd_list.xforms.m_wvp);
        cmd_list.set_c(C, mTexgen);
    }
};
static cl_texgen binder_texgen;

class cl_VPtexgen : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix mTexgen;

#if defined(USE_DX11)
        Fmatrix mTexelAdjust =
        {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f
        };
#elif defined(USE_OGL)
        Fmatrix mTexelAdjust =
        {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f
        };
#else
#    error No graphics API selected or in use!
#endif

        mTexgen.mul(mTexelAdjust, cmd_list.xforms.m_vp);
        cmd_list.set_c(C, mTexgen);
    }
};
static cl_VPtexgen binder_VPtexgen;

// fog
#ifndef _EDITOR
// The haze ramp, in metres. ONE source for everybody.
//
// There are two ways a shader learns how far the fog reaches: fog_params, which the deferred
// combine reads, and fog_plane, which every forward pass reads through calc_fogging - water,
// fire, smoke. They used to compute their bounds differently: fog_params scaled the weather
// distances by the haze knobs, fog_plane took them raw. At stock settings that is a factor of
// 2/3, so the water kept half again as much visible range as the shore around it and stayed
// legible in murk that had already swallowed everything else.
static void fog_bounds(float& n, float& f)
{
    n = g_pGamePersistent->Environment().CurrentEnv.fog_near;
    f = g_pGamePersistent->Environment().CurrentEnv.fog_far;

    // Haze distance (r__fog_dist): 1.0 is exactly the weather values. Both bounds scale
    // together, so the whole start-end shifts while the gradient shape and the differences
    // between weathers survive. With the haze master at zero the weather distances stay
    // untouched. The visibility-distance follow is a RATIO from 1.5 - the position the
    // haze was tuned at - because a difference goes negative at the slider's low end.
    float k = (ps_r__fog_dist > 0.f) ? (1.f + (ps_r__fog_dist - 1.f) * ps_r__fog) : 1.f;
    constexpr float visReference = 1.5f;
    if (ps_r__fog > 0.001f && ps_r__fog_follow_vis > 0.001f && psVisDistance > 0.01f)
    {
        const float ratio = psVisDistance / visReference;
        k *= 1.f + (ratio - 1.f) * ps_r__fog_follow_vis;
    }
    if (_abs(k - 1.f) > 0.001f)
    {
        n *= k;
        f *= k;
    }
}

#if RENDER == R_R4
// The same ramp for the ray traced reflections' off-screen hits, which fog themselves the way the
// combine fogs the frame (da_rt_passes.cpp).
void da_rt_fog_bounds(float& near_m, float& far_m) { fog_bounds(near_m, far_m); }
#endif

class cl_fog_plane : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector4 plane;
        const Fmatrix& M = cmd_list.xforms.m_vp;
        plane.x = -(M._14 + M._13);
        plane.y = -(M._24 + M._23);
        plane.z = -(M._34 + M._33);
        plane.w = -(M._44 + M._43);
        const float denom = -1.0f / _sqrt(_sqr(plane.x) + _sqr(plane.y) + _sqr(plane.z));
        plane.mul(denom);

        float A, far_d;
        fog_bounds(A, far_d);
        const float B = 1 / (far_d - A);
        Fvector4 result;
        result.set(-plane.x * B, -plane.y * B, -plane.z * B, 1 - (plane.w - A) * B);
        cmd_list.set_c(C, result);
    }
};
static cl_fog_plane binder_fog_plane;

// fog-params
class cl_fog_params : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        float n, f;
        fog_bounds(n, f);

        const float r = 1 / (f - n);
        Fvector4 result;
        result.set(-n * r, n, f, r);
        cmd_list.set_c(C, result);
    }
};
static cl_fog_params binder_fog_params;

// fog-color
class cl_fog_color : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
        Fvector4 result;
        result.set(desc.fog_color.x, desc.fog_color.y, desc.fog_color.z, 0);
        cmd_list.set_c(C, result);
    }
};
static cl_fog_color binder_fog_color;
#endif

// times
class cl_times : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        float t = Device.fTimeGlobal;
        cmd_list.set_c(C, t, t * 10, t / 10, _sin(t));
    }
};
static cl_times binder_times;

// The interface's own shaders (da_media.h): a menu's or the loading screen's background by a mod.
// xms_ui_time: seconds of the media clock (wrapped each hour), seconds since the menu or the loading
// screen opened, the frame's seconds.
class cl_xms_ui_time : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const u32 now = da_media_time();
        cmd_list.set_c(C, float(now % 3600000) * 0.001f, float(now - g_da_ui_shader.opened) * 0.001f,
            Device.fTimeDeltaReal, 0.f);
    }
};
static cl_xms_ui_time binder_xms_ui_time;

// (xms_ui_rect and xms_ui_uv change with every static: dxUIRender::FlushPrimitive sets them.)

// the cursor in pixels, and over the screen 0..1
class cl_xms_ui_cursor : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const Fvector2& p = g_da_ui_shader.cursor;
        cmd_list.set_c(C, p.x, p.y, p.x / float(Device.dwWidth), p.y / float(Device.dwHeight));
    }
};
static cl_xms_ui_cursor binder_xms_ui_cursor;

class cl_xms_ui_progress : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_da_ui_shader.progress_eased, g_da_ui_shader.progress, 0.f, 0.f);
    }
};
static cl_xms_ui_progress binder_xms_ui_progress;

// eye-params
class cl_eye_P : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector& V = Device.vCameraPosition;
        cmd_list.set_c(C, V.x, V.y, V.z, 1.f);
    }
};
static cl_eye_P binder_eye_P;

// eye-params
class cl_eye_D : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector& V = Device.vCameraDirection;
        cmd_list.set_c(C, V.x, V.y, V.z, 0.f);
    }
};
static cl_eye_D binder_eye_D;

// eye-params
class cl_eye_N : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector& V = Device.vCameraTop;
        cmd_list.set_c(C, V.x, V.y, V.z, 0.f);
    }
};
static cl_eye_N binder_eye_N;

class cl_inv_v : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, cmd_list.xforms.get_inv_V()); }
};
static cl_inv_v binder_inv_v;

#ifndef _EDITOR
// D-Light0
class cl_sun0_color : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
        Fvector4 result;
        result.set(desc.sun_color.x, desc.sun_color.y, desc.sun_color.z, 0);
        cmd_list.set_c(C, result);
    }
};
static cl_sun0_color binder_sun0_color;
class cl_sun0_dir_w : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
        Fvector4 result;
        result.set(desc.sun_dir.x, desc.sun_dir.y, desc.sun_dir.z, 0);
        cmd_list.set_c(C, result);
    }
};
static cl_sun0_dir_w binder_sun0_dir_w;
class cl_sun0_dir_e : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector D;
        const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
        cmd_list.xforms.m_v.transform_dir(D, desc.sun_dir);
        D.normalize();
        Fvector4 result;
        result.set(D.x, D.y, D.z, 0);
        cmd_list.set_c(C, result);
    }
};
static cl_sun0_dir_e binder_sun0_dir_e;

//
class cl_amb_color : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
        Fvector4 result;
        result.set(desc.ambient.x, desc.ambient.y, desc.ambient.z, desc.weight);
        cmd_list.set_c(C, result);
    }
};
static cl_amb_color binder_amb_color;
class cl_hemi_color : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
        Fvector4 result;
        result.set(desc.hemi_color.x, desc.hemi_color.y, desc.hemi_color.z, desc.hemi_color.w);
        cmd_list.set_c(C, result);
    }
};
static cl_hemi_color binder_hemi_color;
#endif

// The size of the frame the current stage draws: the render size inside the 3D scene, the
// display size after the upscale and outside the frame (da_temporal.h). Not the bound target's:
// the stock bloom and luminance passes draw pixel quads into small targets under a frame-size
// viewport and rely on this being the frame.
static class cl_screen_res : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
#if RENDER == R_R4
        const u32 width = da_stage_width(), height = da_stage_height();
#else
        const u32 width = Device.dwWidth, height = Device.dwHeight;
#endif
        cmd_list.set_c(C, (float)width, (float)height, 1.0f / (float)width, 1.0f / (float)height);
    }
} binder_screen_res;

static class cl_various : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, GetCurrentSunReflection(), 0.f, 0.f,
            g_pGamePersistent->Environment().CurrentEnv.wind_velocity);
    }
} binder_various;

// Alpha-ref threshold for every def_aref consumer (foliage clip, lod cards): a preset-laddered
// uniform instead of the old baked 128/255 literal.
static class cl_da_aref : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, float(ps_r__aref_quality) / 255.f, 0.f, 0.f, 0.f);
    }
} binder_da_aref;

// Effective wind for the puddle ripple: xy = world-XZ wind direction scaled by the current
// strength, z = accumulated WIND travel of the ripple pattern, w = accumulated RAIN travel
// (both in noise-space units, integrated by the wind service). The shader offsets its noise
// lookup by these, so the ripple pattern SCROLLS continuously - downhill in rain, downwind
// otherwise - instead of crossfading in place.
static class cl_da_puddle_wind : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        const float a = env.eff_wind_dir;
        cmd_list.set_c(C, _sin(a) * env.eff_wind_norm, _cos(a) * env.eff_wind_norm,
            env.eff_water_run_wind, env.eff_water_run_rain);
    }
} binder_da_puddle_wind;

// The travelling gust-field for vegetation vertex shaders: xy = wrapped world-space scroll
// offset of the noise field (accumulated downwind on the CPU), z = the slow strength envelope
// so the field can scale itself, w = gustiness for the lean term.
static class cl_da_wind_field : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        cmd_list.set_c(C, env.eff_wind_field_ofs.x, env.eff_wind_field_ofs.y,
            env.eff_wind_norm, env.eff_wind_gust);
    }
} binder_da_wind_field;

// The wind state for every shader that wants physics rather than a bend: xy = unit heading in
// world XZ, z = speed in m/s at 10 m, w = the service clock in seconds.
static class cl_da_wind_state : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        cmd_list.set_c(C, _sin(env.eff_wind_dir), _cos(env.eff_wind_dir), env.WindSpeedMs(), env.eff_wind_time);
    }
} binder_da_wind_state;

#if RENDER == R_R4
// da_wind_field as the previous frame drew with it (da_wind_history.h): the vegetation
// G-buffer shaders bend a second time with it for the motion vectors.
static class cl_da_wind_field_prev : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_wind_prev().field); }
} binder_da_wind_field_prev;
#endif

// The cloud deck (da_clouds.h): one field for the visible clouds, their shadow and the sun
// shafts. params: coverage from the weather's cloud opacity, the deck's base altitude, and
// the field's world-space drift - the aloft wind integrated by the service, along the aloft
// heading. params2: quality tier, thickness, the service clock, shadow density.
// The weather's clouds_color alpha was the OPACITY of the stock cloud texture, never a sky
// fraction: a "clear" cycle authors ~0.3 and means "faint clouds", not "70% blue". Mapped to
// coverage it spans scattered fair-weather cumulus at the low end and a closed deck at the
// top, so no weather ever shows an empty sky for hours. Rain falls from a closed deck: the stock
// layer drew an overcast cycle's clouds as a black sheet over 86 % of the sky, and the deck of 0.67
// it became left a third of a storm's sky its bright texture - a storm read as a dull grey day.
// The weather's rain closes it (a storm's to 0.93). r__clouds_cover pins the raw value.
static float da_cloud_cover_from_weather(const CEnvDescriptorMixer& e)
{
    if (ps_r__clouds_cover >= 0.f)
        return ps_r__clouds_cover;
    const float alpha = clampr(e.clouds_color.w, 0.f, 1.f);
    const float cover = 0.2f + 0.55f * alpha;
    return clampr(cover + (0.97f - cover) * clampr(e.rain_density * 1.5f, 0.f, 1.f) * alpha, 0.f, 1.f);
}

// How far an overcast darkens the sky where the fog hides the deck (da_clouds.ps): none in fair weather, 70 % under
// a closed storm deck. The haze takes the same share off its sky colour (da_sky_tint, r2.cpp): a fogged tree that
// converged to the undarkened sky stood out white against the storm's.
float da_sky_overcast()
{
    const float cover = da_cloud_cover_from_weather(g_pGamePersistent->Environment().CurrentEnv);
    const float t = clampr((cover - 0.4f) / (0.95f - 0.4f), 0.f, 1.f);
    return 0.7f * t * t * (3.f - 2.f * t);
}

static class cl_da_cloud_params : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        const float run = env.eff_cloud_run;
        const float a = env.eff_wind_dir_aloft;
        // r__clouds_cover pins the coverage for tuning and QA; -1 follows the weather.
        const float cover = da_cloud_cover_from_weather(env.CurrentEnv);
        cmd_list.set_c(C, cover, env.eff_cloud_altitude, _sin(a) * run, _cos(a) * run);
    }
} binder_da_cloud_params;
// Where this frame's cloud map sits: centre XZ, edge length, 1/edge (phase_cloud_map).
extern float g_da_cloud_map_center_x;
extern float g_da_cloud_map_center_z;
extern float g_da_cloud_map_extent;

// First-person pixels under local lights (da_hud_light.h): the HUD field of view for the
// lateral rebuild of their position, in the deferred decompression layout.
static class cl_da_hud_light : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const float hud_fov = psHUD_FOV * Device.fFOV;
#if RENDER == R_R4
        // At render size and with the jitter folded in, like pos_decompression_params.
        cmd_list.set_c(C, da_pos_decompress(hud_fov));
#else
        const float VertTan = -1.0f * tanf(deg2rad(hud_fov / 2.0f));
        const float HorzTan = -VertTan / Device.fASPECT;
        cmd_list.set_c(C, HorzTan, VertTan, (2.0f * HorzTan) / float(Device.dwWidth), (2.0f * VertTan) / float(Device.dwHeight));
#endif
    }
} binder_da_hud_light;

// x = the depth-range slice the HUD is rasterized into, y = on/off (the sun self-shadow
// switch, and never under MSAA where the depth copy does not exist), z = normal offset (m).
static class cl_da_hud_light2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const bool on = ps_r__hud_shadow && !RImplementation.o.msaa;
        cmd_list.set_c(C, r2_hud_depth_limit, on ? 1.f : 0.f, ps_r__hud_shadow_normal_offset, 0.f);
    }
} binder_da_hud_light2;

static class cl_da_cloud_debug : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, float(ps_r__clouds_debug), 0.f, 0.f, 0.f); }
} binder_da_cloud_debug;


// Lightning as the clouds see it. The channel inside the deck is a bent line: A where the
// bolt's direction meets the slab, B a few kilometres along a rolled heading, M the midpoint
// pushed sideways by the bend. da_lightning_a/b/m: xyz = the points, w = intensity (in a).
// da_lightning2: rgb = the flash's colour. da_lightning_fog / da_lightning_sun: what the
// flash added to the fog and sun colours this frame, so the deck lights locally instead of
// everywhere. da_sun_dir_real: the sun's own direction while the bolt borrows sun_dir.
static bool da_lightning_points(Fvector& a, Fvector& m, Fvector& b, float& intensity)
{
    const auto& env = g_pGamePersistent->Environment();
    const auto* bolt = env.eff_Thunderbolt;
    if (!bolt || !bolt->lightning_active())
        return false;
    // The channel starts where the line of sight through the bolt enters the deck: just
    // inside the base for a visible bolt (the glow sits on the channel's top), in the body
    // of the slab for sheet lightning. The direction is eye-to-bolt: the effect's own
    // current_direction is inverted after the roll to stand in for the sun, and reading
    // that put the glow twelve kilometres away on the far side of the sky.
    const Fvector& dir = bolt->lightning_direction();
    const Fvector& eye = Device.vCameraPosition;
    const float y = env.eff_cloud_altitude + env.eff_cloud_thickness * (bolt->bolt_is_hidden() ? 0.4f : 0.12f);
    const float t = std::min((y - eye.y) / std::max(dir.y, 0.08f), 12000.f);
    a.set(eye.x + dir.x * t, y, eye.z + dir.z * t);
    const float h = bolt->channel_heading_rad();
    const float len = bolt->channel_length_m();
    const Fvector along{_sin(h), 0.f, _cos(h)};
    const Fvector side{_cos(h), 0.f, -_sin(h)};
    b.mad(a, along, len);
    m.mad(a, along, len * 0.5f);
    m.mad(side, len * bolt->channel_bend_k());
    m.y += env.eff_cloud_thickness * 0.12f;
    intensity = bolt->lightning_intensity();
    return true;
}

static class cl_da_lightning_a : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector a, m, b; float i;
        if (da_lightning_points(a, m, b, i)) cmd_list.set_c(C, a.x, a.y, a.z, i);
        else cmd_list.set_c(C, 0.f, 0.f, 0.f, 0.f);
    }
} binder_da_lightning_a;

static class cl_da_lightning_m : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector a, m, b; float i;
        if (da_lightning_points(a, m, b, i)) cmd_list.set_c(C, m.x, m.y, m.z, 0.f);
        else cmd_list.set_c(C, 0.f, 0.f, 0.f, 0.f);
    }
} binder_da_lightning_m;

static class cl_da_lightning_b : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector a, m, b; float i;
        if (da_lightning_points(a, m, b, i)) cmd_list.set_c(C, b.x, b.y, b.z, 0.f);
        else cmd_list.set_c(C, 0.f, 0.f, 0.f, 0.f);
    }
} binder_da_lightning_b;

static class cl_da_lightning2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto* bolt = g_pGamePersistent->Environment().eff_Thunderbolt;
        const Fvector c = bolt && bolt->lightning_active() ? bolt->lightning_colour() : Fvector{0.f, 0.f, 0.f};
        cmd_list.set_c(C, c.x, c.y, c.z, 0.f);
    }
} binder_da_lightning2;

static class cl_da_lightning_fog : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto* bolt = g_pGamePersistent->Environment().eff_Thunderbolt;
        const Fvector f = bolt ? bolt->lightning_fog_added() : Fvector{0.f, 0.f, 0.f};
        cmd_list.set_c(C, f.x, f.y, f.z, 0.f);
    }
} binder_da_lightning_fog;

static class cl_da_lightning_sun : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto* bolt = g_pGamePersistent->Environment().eff_Thunderbolt;
        const Fvector f = bolt ? bolt->lightning_sun_added() : Fvector{0.f, 0.f, 0.f};
        cmd_list.set_c(C, f.x, f.y, f.z, 0.f);
    }
} binder_da_lightning_sun;

static class cl_da_lightning_sky : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto* bolt = g_pGamePersistent->Environment().eff_Thunderbolt;
        const Fvector f = bolt ? bolt->lightning_sky_added() : Fvector{0.f, 0.f, 0.f};
        cmd_list.set_c(C, f.x, f.y, f.z, 0.f);
    }
} binder_da_lightning_sky;

static class cl_da_sun_dir_real : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        const auto* bolt = env.eff_Thunderbolt;
        const Fvector d = bolt && bolt->lightning_active() ? bolt->sun_direction_real() : env.CurrentEnv.sun_dir;
        cmd_list.set_c(C, d.x, d.y, d.z, 0.f);
    }
} binder_da_sun_dir_real;

static class cl_da_cloud_map : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_da_cloud_map_center_x, g_da_cloud_map_center_z, g_da_cloud_map_extent,
            1.f / g_da_cloud_map_extent);
    }
} binder_da_cloud_map;

static class cl_da_cloud_params2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        // Thin cirrus barely dims the sun, a heavy deck cuts more than half of it. Overcast
        // weathers author their own dim sun on top.
        const float cover = da_cloud_cover_from_weather(env.CurrentEnv);
        // The map's alpha holds the column's transmittance through the volume; this is only
        // the switch (0 = no shadow) and the strength the deck casts with.
        const float density = cover > 0.02f ? 1.f : 0.f;
        const int quality = ps_r__clouds_quality_override >= 0 ? ps_r__clouds_quality_override : ps_r__clouds_quality;
        cmd_list.set_c(C, float(quality), env.eff_cloud_thickness, env.eff_wind_time, density);
    }
} binder_da_cloud_params2;

// Wind motors for the vegetation shaders: 8 point sources packed as two 4x4 matrices each
// (row per motor). pos rows = (xyz, radius), par rows = (bend amp, ring radius, ring width, 0);
// info.x = number of live motors so the shader loop is free when the world is quiet.
// ⚠️ dx11ConstantBuffer::set(Fmatrix) TRANSPOSES on write (column-major cbuffer layout for
// the mul(v,M) convention). Row-per-motor packing therefore needs a PRE-transpose, or the
// shader's da_wm_pos0[i] reads a column - the x-coordinates of four different motors instead
// of motor i. That scramble is exactly why trampling and blast rings never showed while the
// (untransposed float4) motor counter probed fine.
static class cl_da_wm_pos0 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().wind_motor_pos[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_pos0;
static class cl_da_wm_pos1 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().wind_motor_pos[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_pos1;
static class cl_da_wm_par0 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().wind_motor_par[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_par0;
static class cl_da_wm_par1 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().wind_motor_par[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_par1;
static class cl_da_wm_info : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_pGamePersistent->Environment().wind_motor_active, 0.f, 0.f, 0.f);
    }
} binder_da_wm_info;

#if RENDER == R_R4
// The same motor rows as the previous frame drew with them (same pre-transpose), for the
// motion vectors. da_wm_info_prev.y switches the second evaluation in the vegetation shaders:
// it costs a whole bend per vertex and nothing reads the vectors unless TAA or an upscaler runs.
static class cl_da_wm_pos0_prev : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(da_wind_prev().wm_pos[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_pos0_prev;
static class cl_da_wm_pos1_prev : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(da_wind_prev().wm_pos[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_pos1_prev;
static class cl_da_wm_par0_prev : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(da_wind_prev().wm_par[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_par0_prev;
static class cl_da_wm_par1_prev : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(da_wind_prev().wm_par[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_wm_par1_prev;
static class cl_da_wm_info_prev : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, da_wind_prev().wm_active, da_motion_vectors_active() || ps_r__motion_vectors ? 1.f : 0.f, 0.f, 0.f);
    }
} binder_da_wm_info_prev;
#endif

// Water impact spots for the puddle shader - same packing and the same PRE-transpose
// requirement as the wind motors above.
static class cl_da_wh_pos0 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_hit_pos[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_wh_pos0;
static class cl_da_wh_pos1 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_hit_pos[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_wh_pos1;
static class cl_da_wh_par0 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_hit_par[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_wh_par0;
static class cl_da_wh_par1 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_hit_par[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_wh_par1;
static class cl_da_wh_info : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_pGamePersistent->Environment().water_hit_active, 0.f, 0.f, 0.f);
    }
} binder_da_wh_info;

// The water surface (da_water_common.h). sea = the state the wind and the level's fetch solve
// to each frame: significant wave height, peak wavelength, Cox-Munk mean square slope and the
// filtered 10 m wind - gusts must not resize a sea, so w is not the raw WindSpeedMs(). body =
// the water body itself: mean depth, the fetch that state was solved with, and the live wave
// row count the shader loops to.
static class cl_da_water_sea : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        cmd_list.set_c(C, env.water_sea.x, env.water_sea.y, env.water_sea.z, env.water_sea.w);
    }
} binder_da_water_sea;
static class cl_da_water_body : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        cmd_list.set_c(C, env.water_body.x, env.water_body.y, env.water_body.z, env.water_body.w);
    }
} binder_da_water_body;

// Two optical profiles live at once because a level may carry a clear stream and a green pool
// side by side; a surface picks its slot at compile time in its own .s script, so both are
// always bound. iop = extinction sigma_t per channel in 1/m plus the profile's wave damping in w,
// iop2 = the irradiance reflectance of the water column plus the floating scum coverage.
// The 'b' pair is slot B.
static class cl_da_water_iop : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& p = g_pGamePersistent->Environment().water_profile[0];
        cmd_list.set_c(C, p.sigma_t.x, p.sigma_t.y, p.sigma_t.z, p.wave_damp);
    }
} binder_da_water_iop;
static class cl_da_water_iop2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& p = g_pGamePersistent->Environment().water_profile[0];
        cmd_list.set_c(C, p.body_r.x, p.body_r.y, p.body_r.z, p.scum);
    }
} binder_da_water_iop2;
static class cl_da_water_iopb : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& p = g_pGamePersistent->Environment().water_profile[1];
        cmd_list.set_c(C, p.sigma_t.x, p.sigma_t.y, p.sigma_t.z, p.wave_damp);
    }
} binder_da_water_iopb;
static class cl_da_water_iop2b : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& p = g_pGamePersistent->Environment().water_profile[1];
        cmd_list.set_c(C, p.body_r.x, p.body_r.y, p.body_r.z, p.scum);
    }
} binder_da_water_iop2b;

// Eight waves as two matrices, one wave per ROW - (theta, k, A, phi). Same PRE-transpose as
// the wind motors and the impact slots above, or the shader reads a column and gets the
// headings of four different waves instead of one whole wave.
static class cl_da_water_wave0 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_wave[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_water_wave0;
static class cl_da_water_wave1 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_wave[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_water_wave1;

// The baked water field (DESIGN2.md section 1). map carries the XZ mapping a shader needs to
// address it - uv = (wp.xz - map.xy) * map.zw - and map2 the scale and the "this level has a
// field at all" flag. An invalid field publishes zeros rather than the sentinel corners of an
// invalidated box, so a shader that forgets to test map2.w reads the origin and not 1e30.
static class cl_da_water_map : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        if (!env.water_field_valid())
        {
            cmd_list.set_c(C, 0.f, 0.f, 0.f, 0.f);
            return;
        }
        cmd_list.set_c(C, env.water_field_bounds.vMin.x, env.water_field_bounds.vMin.z,
            env.water_field_inv_extent.x, env.water_field_inv_extent.y);
    }
} binder_da_water_map;
static class cl_da_water_map2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        if (!env.water_field_valid())
        {
            cmd_list.set_c(C, 0.f, 0.f, 0.f, 0.f);
            return;
        }
        cmd_list.set_c(C, env.water_field_texel, env.water_field_bounds.vMin.y,
            env.water_field_bounds.vMax.y, 1.f);
    }
} binder_da_water_map2;

// The camera below a water surface: how deep, the surface it is below, and the flag the
// combine and the surface shader branch on.
static class cl_da_underwater : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        cmd_list.set_c(C, env.eye_under_depth, env.eye_under_surface, 0.f,
            env.eye_under_depth > 0.f ? 1.f : 0.f);
    }
} binder_da_underwater;

// The water quality ladder as a CONSTANT and not a shader option: r__water_underwater and
// r__water_caustics are CCC_RuntimeInteger and change live, and a shader-cache key would not
// follow them. x = the underwater tier 1..4, y = 1 while caustics are on for this preset.
static class cl_da_water_qual : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, float(ps_r__water_underwater), ps_r__water_caustics ? 1.f : 0.f, 0.f, 0.f);
    }
} binder_da_water_qual;

// The ripple simulation window, solved in CEnvironment so the sim pass and every CPU consumer
// land on the same texel grid: (centre x, centre z, window metres, 1/texels).
static class cl_da_water_rip : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& w = g_pGamePersistent->Environment().water_ripple_win;
        cmd_list.set_c(C, w.x, w.y, w.z, w.w);
    }
} binder_da_water_rip;

// Wading wakes, the continuous sources of the ripple field. Same packing and the same PRE-
// transpose requirement as the wind motors and the impact slots.
static class cl_da_water_wake0 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_wake_pos[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_water_wake0;
static class cl_da_water_wake1 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_wake_pos[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_water_wake1;
static class cl_da_water_wakep0 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_wake_par[0]);
        cmd_list.set_c(C, t);
    }
} binder_da_water_wakep0;
static class cl_da_water_wakep1 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fmatrix t;
        t.transpose(g_pGamePersistent->Environment().water_wake_par[1]);
        cmd_list.set_c(C, t);
    }
} binder_da_water_wakep1;
static class cl_da_water_wakei : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_pGamePersistent->Environment().water_wake_active, 0.f, 0.f, 0.f);
    }
} binder_da_water_wakei;

// Rain as a physical rate: mm/h and the extinction it adds to the fog, 1/km. Both solved in
// water_tick from the weather's rain_density - a 0..1 dial says nothing about drop counts.
// z carries the rain tier so the streak shader can ride the ladder the C++ half already reads;
// a constant and not a shader option, because r__rain_quality is CCC_RuntimeInteger.
static class cl_da_rain2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        cmd_list.set_c(C, env.rain_rate_mmh, env.rain_ext_km, float(ps_r__rain_quality), 0.f);
    }
} binder_da_rain2;

// 3D PDA screen state (model_pda_screen.ps): published by the game once per frame.
static class cl_pda_affects : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_pda_screen_affects.x, g_pda_screen_affects.y, g_pda_screen_affects.z,
            g_pda_screen_affects.w);
    }
} binder_pda_affects;
static class cl_pda_screen_rect : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_pda_screen_rect.x, g_pda_screen_rect.y, g_pda_screen_rect.z,
            g_pda_screen_rect.w);
    }
} binder_pda_screen_rect;
static class cl_pda_taa_bbox : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, g_pda_taa_bbox.x, g_pda_taa_bbox.y, g_pda_taa_bbox.z, g_pda_taa_bbox.w);
    }
} binder_pda_taa_bbox;

static class cl_various_rain : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& environment = g_pGamePersistent->Environment();
        cmd_list.set_c(C, environment.CurrentEnv.rain_density, GetCurrentWetness(),
            GetCurrentSnowFactor(), GetCurrentSeason());
    }
} binder_various_rain;

// The season day (da_season.h): x D/365, y last frame's D/365, z 1 while a QA command drives D,
// w hour of the game day (0..24).
static class cl_da_season_day : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const float hour = g_pGamePersistent ? g_pGamePersistent->Environment().GetGameTime() / 3600.f : 12.f;
        cmd_list.set_c(C, da_season_day() / 365.f, da_season_day_prev() / 365.f, da_season_qa() ? 1.f : 0.f, hour);
    }
} binder_da_season_day;

// What the weather does to vegetation now (da_precip()): x hoarfrost, y dew, z rain wetness (liquid
// share only), w snowfall.
static class cl_da_season_env : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const SPrecipState& p = da_precip();
        cmd_list.set_c(C, p.frost, p.dew, GetCurrentWetness(), p.snow_rate);
    }
} binder_da_season_env;

// The snow constants of this frame (da_snow_frame()), one binder per vector.
template <Fvector4 SSnowFrame::*field>
class cl_da_snow_frame final : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_snow_frame().*field); }
};
static cl_da_snow_frame<&SSnowFrame::S> binder_da_snow_S;
static cl_da_snow_frame<&SSnowFrame::Mc> binder_da_snow_Mc;
static cl_da_snow_frame<&SSnowFrame::S_prev> binder_da_snow_S_prev;
static cl_da_snow_frame<&SSnowFrame::Mc_prev> binder_da_snow_Mc_prev;
static cl_da_snow_frame<&SSnowFrame::cover> binder_da_snow_cover;
static cl_da_snow_frame<&SSnowFrame::state> binder_da_snow_state;
static cl_da_snow_frame<&SSnowFrame::surf> binder_da_snow_surf;
static cl_da_snow_frame<&SSnowFrame::dirt> binder_da_snow_dirt;
static cl_da_snow_frame<&SSnowFrame::fall> binder_da_snow_fall;
static cl_da_snow_frame<&SSnowFrame::ice> binder_da_snow_ice;
static cl_da_snow_frame<&SSnowFrame::albedo_open> binder_da_snow_albedo_open;
static cl_da_snow_frame<&SSnowFrame::albedo_road> binder_da_snow_albedo_road;
static cl_da_snow_frame<&SSnowFrame::albedo_trees> binder_da_snow_albedo_trees;

// The climate's ice (xrEngine da_ice.h, docs/dead-air/WEATHER.md): x holes bound, y standing water's ice (m), z running
// water's (m), w the rot of the standing water's spring ice; and the holes nearest the camera.
static Fvector4 s_ice_holes[da_ice_holes_gpu];
static u32 s_ice_holes_n = 0, s_ice_holes_frame = u32(-1);
static void ice_holes_frame()
{
    if (s_ice_holes_frame == Device.dwFrame)
        return;
    s_ice_holes_frame = Device.dwFrame;
    s_ice_holes_n = da_ice_holes_near(Device.vCameraPosition, s_ice_holes);
}
static class cl_da_ice_state final : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        ice_holes_frame();
        const SDaClimateState& st = da_climate_state();
        const bool on = da_climate_enabled();
        cmd_list.set_c(C, float(s_ice_holes_n), on ? st.ice[0] : 0.f, on ? st.ice[1] : 0.f,
            on ? clampr(st.ice_rot[0] / 15.f, 0.f, 1.f) : 0.f);
    }
} binder_da_ice_state;
static class cl_da_ice_holes final : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        ice_holes_frame();
        for (u32 i = 0; i < da_ice_holes_gpu; ++i)
            cmd_list.set_ca(C, i, i < s_ice_holes_n ? s_ice_holes[i] : Fvector4{});
    }
} binder_da_ice_holes;

#if RENDER == R_R4
// r3\da_snow_sample.h: the fine trail window and the level-wide depth texture, and the whole misc
// vector (x cap, y fine half-size, z depth present, w fine-to-level blend band).
static class cl_da_snow_fine_xf : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_snow_fine_xf()); }
} binder_da_snow_fine_xf;
static class cl_da_snow_level_xf : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_snow_level_xf()); }
} binder_da_snow_level_xf;
static class cl_da_snow_misc : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_snow_misc()); }
} binder_da_snow_misc;

// Blade grass ground tint of the terrain (deffer_impl_flat.ps): cover map mapping, strength and fade,
// the canopy colours of the 16 communities at the day and at the latched folder's reference day. The
// values are a copy of what the main thread latched, safe from any context.
struct da_grass_tint_values
{
    Fvector4 xf{}, tint{}, tint2{}, canopy[16]{}, canopy_ref[16]{};
    da_grass_tint_values() { da_grass_ground_tint(xf, tint, tint2, canopy, canopy_ref); }
};
static class cl_da_gcover_xf : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_grass_tint_values().xf); }
} binder_da_gcover_xf;
static class cl_da_gtint : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_grass_tint_values().tint); }
} binder_da_gtint;
static class cl_da_gtint2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_grass_tint_values().tint2); }
} binder_da_gtint2;
static class cl_da_gtuft : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_grass_tuft_window()); }
} binder_da_gtuft;
static class cl_da_gcanopy : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const da_grass_tint_values v;
        for (u32 i = 0; i < 16; ++i)
            cmd_list.set_ca(C, i, v.canopy[i]);
    }
} binder_da_gcanopy;
static class cl_da_gcanopy_ref : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const da_grass_tint_values v;
        for (u32 i = 0; i < 16; ++i)
            cmd_list.set_ca(C, i, v.canopy_ref[i]);
    }
} binder_da_gcanopy_ref;
#endif

static class cl_temp : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, ps_r2_temp); }
} binder_temp;

static class cl_scripted : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_shaders_var_x, ps_shaders_var_y, ps_shaders_var_z, ps_shaders_var_w);
    }
} binder_scripted;

static class cl_postprocess_var : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r2_postprocess_var_x, ps_r2_postprocess_var_y,
            ps_r2_postprocess_var_z, ps_r2_postprocess_var_w);
    }
} binder_postprocess_var;

static class cl_lens_var : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r2_lens_var_x, ps_r2_lens_var_y, ps_r2_lens_var_z, ps_r2_lens_var_w);
    }
} binder_lens_var;

// SM_TODO: cmd_list.hemi заменить на более "логичное" место
static class cl_hud_params : public R_constant_setup //--#SM+#--
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, g_pGamePersistent->m_pGShaderConstants->hud_params); }
} binder_hud_params;

static class cl_script_params : public R_constant_setup //--#SM+#--
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, g_pGamePersistent->m_pGShaderConstants->m_script_params); }
} binder_script_params;

static class cl_blend_mode : public R_constant_setup //--#SM+#--
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        Fvector4 blenderMode = g_pGamePersistent->m_pGShaderConstants->m_blender_mode;
        if (cmd_list.detailRendering)
            blenderMode.w = 1.f;
        cmd_list.set_c(C, blenderMode);
    }
} binder_blend_mode;

class cl_camo_data : public R_constant_setup //--#SM+#--
{
    void setup(CBackend& cmd_list, R_constant* C) override  { cmd_list.hemi.c_camo_data = C; }
};
static cl_camo_data binder_camo_data;

class cl_custom_data : public R_constant_setup //--#SM+#--
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.hemi.c_custom_data = C; }
};
static cl_custom_data binder_custom_data;

class cl_entity_data : public R_constant_setup //--#SM+#--
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.hemi.c_entity_data = C; }
};
static cl_entity_data binder_entity_data;

#if RENDER == R_R4
// Ray tracing effects live this frame (da_rt.h): x reflections, y sun shadows, z AO, w quality.
static class cl_da_rt : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, da_rt_constants()); }
} binder_da_rt;
#endif

// Standart constant-binding
void CBlender_Compile::SetMapping()
{
    // misc
    r_Constant("m_hud_params", &binder_hud_params); //--#SM+#--
    r_Constant("m_script_params", &binder_script_params); //--#SM+#--
    r_Constant("m_blender_mode", &binder_blend_mode); //--#SM+#--

    // objects data
    r_Constant("m_obj_camo_data", &binder_camo_data); //--#SM+#--
    r_Constant("m_obj_custom_data", &binder_custom_data); //--#SM+#--
    r_Constant("m_obj_entity_data", &binder_entity_data); //--#SM+#--

    // matrices
    r_Constant("m_W", &binder_w);
    r_Constant("m_invW", &binder_invw);
    r_Constant("m_V", &binder_v);
    r_Constant("m_P", &binder_p);
    r_Constant("m_WV", &binder_wv);
    r_Constant("m_VP", &binder_vp);
    r_Constant("m_WVP", &binder_wvp);
    r_Constant("m_VP_nojit", &binder_vp_nojit);
    r_Constant("m_VP_prev", &binder_vp_prev);
    r_Constant("m_WVP_nojit", &binder_wvp_nojit);
    r_Constant("m_WVP_prev", &binder_wvp_prev);
    r_Constant("m_W_prev", &binder_w_prev);

    r_Constant("m_xform_v", &tree_binder_m_xform_v);
    r_Constant("m_xform", &tree_binder_m_xform);
    r_Constant("consts", &tree_binder_consts);
    r_Constant("wave", &tree_binder_wave);
    r_Constant("wind", &tree_binder_wind);
    r_Constant("c_scale", &tree_binder_c_scale);
    r_Constant("c_bias", &tree_binder_c_bias);
    r_Constant("c_sun", &tree_binder_c_sun);
    r_Constant("c_tree", &tree_binder_c_tree);
    r_Constant("wave_prev", &tree_binder_wave_prev);
    r_Constant("wind_prev", &tree_binder_wind_prev);
    r_Constant("c_tree_prev", &tree_binder_c_tree_prev);

    // hemi cube
    r_Constant("L_material", &binder_material);
    r_Constant("hemi_cube_pos_faces", &binder_hemi_cube_pos_faces);
    r_Constant("hemi_cube_neg_faces", &binder_hemi_cube_neg_faces);

    // Igor temp solution for the texgen functionality in the shader
    r_Constant("m_texgen", &binder_texgen);
    r_Constant("mVPTexgen", &binder_VPtexgen);

#ifndef _EDITOR
    // fog-params
    r_Constant("fog_plane", &binder_fog_plane);
    r_Constant("fog_params", &binder_fog_params);
    r_Constant("fog_color", &binder_fog_color);
#endif
    // time
    r_Constant("timers", &binder_times);
    r_Constant("xms_ui_time", &binder_xms_ui_time);
    r_Constant("xms_ui_cursor", &binder_xms_ui_cursor);
    r_Constant("xms_ui_progress", &binder_xms_ui_progress);

    // eye-params
    r_Constant("eye_position", &binder_eye_P);
    r_Constant("eye_direction", &binder_eye_D);
    r_Constant("eye_normal", &binder_eye_N);
    r_Constant("m_v2w", &binder_inv_v);

#ifndef _EDITOR
    // global-lighting (env params)
    r_Constant("L_sun_color", &binder_sun0_color);
    r_Constant("L_sun_dir_w", &binder_sun0_dir_w);
    r_Constant("L_sun_dir_e", &binder_sun0_dir_e);
    //r_Constant("L_lmap_color", &binder_lm_color);
    r_Constant("L_hemi_color", &binder_hemi_color);
    r_Constant("L_ambient", &binder_amb_color);
#endif
    r_Constant("screen_res", &binder_screen_res);
    r_Constant("da_aref_u", &binder_da_aref);
    r_Constant("da_puddle_wind", &binder_da_puddle_wind);
    r_Constant("da_wind_field", &binder_da_wind_field);
    r_Constant("da_wind_state", &binder_da_wind_state);
    r_Constant("da_cloud_params", &binder_da_cloud_params);
    r_Constant("da_cloud_params2", &binder_da_cloud_params2);
    r_Constant("da_cloud_map", &binder_da_cloud_map);
    r_Constant("da_lightning_a", &binder_da_lightning_a);
    r_Constant("da_lightning_m", &binder_da_lightning_m);
    r_Constant("da_lightning_b", &binder_da_lightning_b);
    r_Constant("da_lightning2", &binder_da_lightning2);
    r_Constant("da_lightning_fog", &binder_da_lightning_fog);
    r_Constant("da_lightning_sun", &binder_da_lightning_sun);
    r_Constant("da_lightning_sky", &binder_da_lightning_sky);
    r_Constant("da_sun_dir_real", &binder_da_sun_dir_real);
    r_Constant("da_cloud_debug", &binder_da_cloud_debug);
    r_Constant("da_hud_light", &binder_da_hud_light);
    r_Constant("da_hud_light2", &binder_da_hud_light2);
    r_Constant("da_wm_pos0", &binder_da_wm_pos0);
    r_Constant("da_wm_pos1", &binder_da_wm_pos1);
    r_Constant("da_wm_par0", &binder_da_wm_par0);
    r_Constant("da_wm_par1", &binder_da_wm_par1);
    r_Constant("da_wm_info", &binder_da_wm_info);
#if RENDER == R_R4
    r_Constant("da_wind_field_prev", &binder_da_wind_field_prev);
    r_Constant("da_wm_pos0_prev", &binder_da_wm_pos0_prev);
    r_Constant("da_wm_pos1_prev", &binder_da_wm_pos1_prev);
    r_Constant("da_wm_par0_prev", &binder_da_wm_par0_prev);
    r_Constant("da_wm_par1_prev", &binder_da_wm_par1_prev);
    r_Constant("da_wm_info_prev", &binder_da_wm_info_prev);
    r_Constant("da_rt", &binder_da_rt);
#endif
    r_Constant("da_wh_pos0", &binder_da_wh_pos0);
    r_Constant("da_wh_pos1", &binder_da_wh_pos1);
    r_Constant("da_wh_par0", &binder_da_wh_par0);
    r_Constant("da_wh_par1", &binder_da_wh_par1);
    r_Constant("da_wh_info", &binder_da_wh_info);
    r_Constant("da_water_sea", &binder_da_water_sea);
    r_Constant("da_water_body", &binder_da_water_body);
    r_Constant("da_water_iop", &binder_da_water_iop);
    r_Constant("da_water_iop2", &binder_da_water_iop2);
    r_Constant("da_water_iopb", &binder_da_water_iopb);
    r_Constant("da_water_iop2b", &binder_da_water_iop2b);
    r_Constant("da_water_wave0", &binder_da_water_wave0);
    r_Constant("da_water_wave1", &binder_da_water_wave1);
    r_Constant("da_water_map", &binder_da_water_map);
    r_Constant("da_water_map2", &binder_da_water_map2);
    r_Constant("da_underwater", &binder_da_underwater);
    r_Constant("da_water_qual", &binder_da_water_qual);
    r_Constant("da_water_rip", &binder_da_water_rip);
    r_Constant("da_water_wake0", &binder_da_water_wake0);
    r_Constant("da_water_wake1", &binder_da_water_wake1);
    r_Constant("da_water_wakep0", &binder_da_water_wakep0);
    r_Constant("da_water_wakep1", &binder_da_water_wakep1);
    r_Constant("da_water_wakei", &binder_da_water_wakei);
    r_Constant("da_rain2", &binder_da_rain2);
    r_Constant("m_affects", &binder_pda_affects);
    r_Constant("pda_screen_rect", &binder_pda_screen_rect);
    r_Constant("pda_taa_bbox", &binder_pda_taa_bbox);
    r_Constant("various", &binder_various);
    r_Constant("various_rain", &binder_various_rain);
    // Seasons and snow (da_season.h, da_snow_field.h, docs/dead-air/SNOW.md, GRASS.md).
    r_Constant("da_season_day", &binder_da_season_day);
    r_Constant("da_season_env", &binder_da_season_env);
    r_Constant("da_snow_S", &binder_da_snow_S);
    r_Constant("da_snow_Mc", &binder_da_snow_Mc);
    r_Constant("da_snow_S_prev", &binder_da_snow_S_prev);
    r_Constant("da_snow_Mc_prev", &binder_da_snow_Mc_prev);
    r_Constant("da_snow_cover", &binder_da_snow_cover);
    r_Constant("da_snow_state", &binder_da_snow_state);
    r_Constant("da_snow_surf", &binder_da_snow_surf);
    r_Constant("da_snow_dirt", &binder_da_snow_dirt);
    r_Constant("da_snow_fall", &binder_da_snow_fall);
    r_Constant("da_snow_ice", &binder_da_snow_ice);
    r_Constant("da_ice_state", &binder_da_ice_state);
    r_Constant("da_ice_holes", &binder_da_ice_holes);
    r_Constant("da_snow_albedo_open", &binder_da_snow_albedo_open);
    r_Constant("da_snow_albedo_road", &binder_da_snow_albedo_road);
    r_Constant("da_snow_albedo_trees", &binder_da_snow_albedo_trees);
#if RENDER == R_R4
    r_Constant("da_snow_fine_xf", &binder_da_snow_fine_xf);
    r_Constant("da_snow_level_xf", &binder_da_snow_level_xf);
    r_Constant("da_snow_misc", &binder_da_snow_misc);
    r_Constant("da_gcover_xf", &binder_da_gcover_xf);
    r_Constant("da_gtint", &binder_da_gtint);
    r_Constant("da_gtint2", &binder_da_gtint2);
    r_Constant("da_gtuft", &binder_da_gtuft);
    r_Constant("da_gcanopy", &binder_da_gcanopy);
    r_Constant("da_gcanopy_ref", &binder_da_gcanopy_ref);
#endif
    r_Constant("temp", &binder_temp);
    r_Constant("scripted", &binder_scripted);
    r_Constant("postprocess_var", &binder_postprocess_var);
    r_Constant("lens_var", &binder_lens_var);

    // detail
    // if (bDetail  && detail_scaler)
    // Igor: bDetail can be overridden by no_detail_texture option.
    // But shader can be deatiled implicitly, so try to set this parameter
    // anyway.
    if (detail_scaler)
        r_Constant("dt_params", detail_scaler);

    // other common
    for (u32 it = 0; it < RImplementation.Resources->v_constant_setup.size(); it++)
    {
        std::pair<shared_str, R_constant_setup*> cs = RImplementation.Resources->v_constant_setup[it];
        r_Constant(cs.first.c_str(), cs.second);
    }
}
} // namespace xray::render::RENDER_NAMESPACE
