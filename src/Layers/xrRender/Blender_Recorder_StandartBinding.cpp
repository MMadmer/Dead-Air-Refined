#include "stdafx.h"
#pragma hdrstop

#include "ResourceManager.h"
#include "Blender_Recorder.h"
#include "Blender.h"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrEngine/EnvironmentWeatherState.h"

// Declared BEFORE the render namespace opens on purpose: an extern inside it would introduce
// its own xray::render::*::psVisDistance that nothing defines, and the build dies at link.
// The variable lives in the engine (Environment.cpp).
extern ENGINE_API float psVisDistance;
// 3D PDA screen state (same rule; both live in xr_ioc_cmd.cpp).
extern ENGINE_API Fvector4 g_pda_screen_affects;
extern ENGINE_API Fvector4 g_pda_screen_rect;
extern ENGINE_API Fvector4 g_pda_taa_bbox;

extern ENGINE_API float psHUD_FOV;

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

        const float A = g_pGamePersistent->Environment().CurrentEnv.fog_near;
        const float B = 1 / (g_pGamePersistent->Environment().CurrentEnv.fog_far - A);
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
        float n = g_pGamePersistent->Environment().CurrentEnv.fog_near;
        float f = g_pGamePersistent->Environment().CurrentEnv.fog_far;

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

static class cl_screen_res : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, (float)Device.dwWidth, (float)Device.dwHeight, 1.0f / (float)Device.dwWidth,
            1.0f / (float)Device.dwHeight);
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

// The cloud deck (da_clouds.h): one field for the visible clouds, their shadow and the sun
// shafts. params: coverage from the weather's cloud opacity, the deck's base altitude, and
// the field's world-space drift - the aloft wind integrated by the service, along the aloft
// heading. params2: quality tier, thickness, the service clock, shadow density.
// The weather's clouds_color alpha was the OPACITY of the stock cloud texture, never a sky
// fraction: a "clear" cycle authors ~0.3 and means "faint clouds", not "70% blue". Mapped to
// coverage it spans scattered fair-weather cumulus at the low end and a closed deck at the
// top, so no weather ever shows an empty sky for hours. r__clouds_cover pins the raw value.
static float da_cloud_cover_from_weather(float alpha)
{
    if (ps_r__clouds_cover >= 0.f)
        return ps_r__clouds_cover;
    return clampr(0.2f + 0.55f * clampr(alpha, 0.f, 1.f), 0.f, 1.f);
}

static class cl_da_cloud_params : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment();
        const float run = env.eff_cloud_run;
        const float a = env.eff_wind_dir_aloft;
        // r__clouds_cover pins the coverage for tuning and QA; -1 follows the weather.
        const float cover = da_cloud_cover_from_weather(env.CurrentEnv.clouds_color.w);
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
        const float VertTan = -1.0f * tanf(deg2rad(hud_fov / 2.0f));
        const float HorzTan = -VertTan / Device.fASPECT;
        cmd_list.set_c(C, HorzTan, VertTan, (2.0f * HorzTan) / float(Device.dwWidth), (2.0f * VertTan) / float(Device.dwHeight));
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
        const float cover = da_cloud_cover_from_weather(env.CurrentEnv.clouds_color.w);
        const float density = clampr((cover - 0.05f) * 1.6f, 0.f, 1.f) * 0.55f;
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

    r_Constant("m_xform_v", &tree_binder_m_xform_v);
    r_Constant("m_xform", &tree_binder_m_xform);
    r_Constant("consts", &tree_binder_consts);
    r_Constant("wave", &tree_binder_wave);
    r_Constant("wind", &tree_binder_wind);
    r_Constant("c_scale", &tree_binder_c_scale);
    r_Constant("c_bias", &tree_binder_c_bias);
    r_Constant("c_sun", &tree_binder_c_sun);
    r_Constant("c_tree", &tree_binder_c_tree);

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
    r_Constant("da_cloud_debug", &binder_da_cloud_debug);
    r_Constant("da_hud_light", &binder_da_hud_light);
    r_Constant("da_hud_light2", &binder_da_hud_light2);
    r_Constant("da_wm_pos0", &binder_da_wm_pos0);
    r_Constant("da_wm_pos1", &binder_da_wm_pos1);
    r_Constant("da_wm_par0", &binder_da_wm_par0);
    r_Constant("da_wm_par1", &binder_da_wm_par1);
    r_Constant("da_wm_info", &binder_da_wm_info);
    r_Constant("da_wh_pos0", &binder_da_wh_pos0);
    r_Constant("da_wh_pos1", &binder_da_wh_pos1);
    r_Constant("da_wh_par0", &binder_da_wh_par0);
    r_Constant("da_wh_par1", &binder_da_wh_par1);
    r_Constant("da_wh_info", &binder_da_wh_info);
    r_Constant("m_affects", &binder_pda_affects);
    r_Constant("pda_screen_rect", &binder_pda_screen_rect);
    r_Constant("pda_taa_bbox", &binder_pda_taa_bbox);
    r_Constant("various", &binder_various);
    r_Constant("various_rain", &binder_various_rain);
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
