#include "stdafx.h"
#pragma hdrstop

#include "xrRender_console.h"
#include "xrCore/xr_token.h"
#include "xrCore/Animation/SkeletonMotions.hpp"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#if RENDER != R_R1
#include "r__pixel_calculator.h"
#endif

#if defined(USE_DX11)
#include "Layers/xrRenderDX11/StateManager/dx11SamplerStateCache.h"
#endif

#if (RENDER == R_R3) || (RENDER == R_R4)
#   ifndef MASTER_GOLD
#   include "Layers/xrRenderDX11/3DFluid/dx113DFluidManager.h"
#   endif // MASTER_GOLD
#endif // (RENDER == R_R3) || (RENDER == R_R4)


namespace xray::render::RENDER_NAMESPACE
{
u32 ps_Preset = 2;
const xr_token qpreset_token[] =
{
    { "Minimum", 0 },
    { "Low", 1 },
    { "Default", 2 },
    { "High", 3 },
    { "Extreme", 4 },
    { nullptr, 0 }
};

u32 ps_r2_smapsize = 2048;
const xr_token qsmapsize_token[] =
{
#if !defined(MASTER_GOLD) || RENDER == R_R1
    { "256", 256 }, // Too bad for R2+
    { "512", 512 }, // But works
#endif
    { "1024", 1024 },
    { "1032", 1032 },
    { "1536", 1536 },
    { "2048", 2048 },
    { "2560", 2560 },
    { "3072", 3072 },
    { "3584", 3584 },
    { "4096", 4096 }, // XXX: runtime check for maximum smap-size on OpenGL
    { "5120", 5120 },
    { "6144", 6144 },
    { "7168", 7168 },
    { "8192", 8192 },
    { "9216", 9216 },
    { "10240", 10240 },
    { "11264", 11264 },
    { "12288", 12288 },
    { "13312", 13312 },
    { "14336", 14336 },
    { "15360", 15360 },
    { "16384", 16384 },
    { nullptr, 0 }
};

u32 ps_r_ssao_mode = ssao_mode_default;
const xr_token qssao_mode_token[] =
{
    { "disabled", ssao_mode_off },
    { "default",  ssao_mode_default },
    { "hdao",     ssao_mode_hdao },
    { "hbao",     ssao_mode_hbao },
    { nullptr,    0 }
};

u32 ps_r_sun_shafts = 2;
const xr_token qsun_shafts_token[] = {{"st_opt_off", 0}, {"st_opt_low", 1}, {"st_opt_medium", 2}, {"st_opt_high", 3}, {nullptr, 0}};
float ps_r2_sun_shafts_value = 0.f;
int ps_r2_sss_enable = 0;
float ps_r2_sss_intensity = 1.f;
float ps_r2_sss_blend = 0.066f;
float ps_r2_sss_phase1 = 0.09f;
float ps_r2_sss_phase2 = 0.03f;
float ps_r2_sss_radius = 1.56f;
int ps_r2_fxaa = 0;

u32 ps_r_ssao = 3;
const xr_token qssao_token[] = {{"st_opt_off", 0}, {"st_opt_low", 1}, {"st_opt_medium", 2}, {"st_opt_high", 3},
    {"st_opt_ultra", 4},
{nullptr, 0}};

u32 ps_r_sun_quality = 1; // = 0;
const xr_token qsun_quality_token[] = {{"st_opt_low", 0}, {"st_opt_medium", 1}, {"st_opt_high", 2},
#if defined(USE_DX11) // TODO: OGL: fix ultra and extreme settings
    {"st_opt_ultra", 3}, {"st_opt_extreme", 4},
#endif // USE_DX11
    {nullptr, 0}};

u32 ps_r_sun_details = detail_shadow_off;
const xr_token qsun_details_token[] =
{
    { "st_opt_off", detail_shadow_off },
    { "st_opt_medium", detail_shadow_medium },
    { "st_opt_high", detail_shadow_high },
    { "off", detail_shadow_off },
    { "on", detail_shadow_high },
    { "0", detail_shadow_off },
    { "1", detail_shadow_high },
    { nullptr, 0 }
};

u32 ps_r_lighting_quality = 4;
const xr_token qlighting_quality_token[] =
{
    { "st_opt_lowest", 0 },
    { "st_opt_low", 1 },
    { "st_opt_medium", 2 },
    { "st_opt_high", 3 },
    { "st_opt_ultra", 4 },
    { nullptr, 0 }
};

u32 ps_r_water_reflection = 3;
const xr_token qwater_reflection_quality_token[] =
{
    { "st_opt_off", 0 },
    { "st_opt_low", 1 },
    { "st_opt_medium", 2 },
    { "st_opt_high", 3 },
    { "st_opt_ultra", 4 },
    { nullptr, -1 }
};

u32 ps_r3_msaa = 0; // = 0;
const xr_token qmsaa_token[] = {{"st_opt_off", 0}, {"2x", 1}, {"4x", 2}, {"8x", 3},
    {nullptr, 0}};

u32 ps_r3_msaa_atest = 0; // = 0;
const xr_token qmsaa__atest_token[] = {
    {"st_opt_off", 0}, {"st_opt_atest_msaa_dx10_0", 1}, {"st_opt_atest_msaa_dx10_1", 2}, {nullptr, 0}};

u32 ps_r3_minmax_sm = 3; // = 0;
const xr_token qminmax_sm_token[] = {{"off", 0}, {"on", 1}, {"auto", 2}, {"autodetect", 3}, {nullptr, 0}};

u32 ps_r_optimize_static = geometry_optimization_medium;
const xr_token q_optimize_static_token[] =
{
    { "st_optimize_off", geometry_optimization_off },
    { "st_optimize_low", geometry_optimization_low },
    { "st_optimize_med", geometry_optimization_medium },
    { "st_optimize_high", geometry_optimization_high },
    { nullptr, 0 }
};

// “Off”
// “DX10.0 style [Standard]”
// “DX10.1 style [Higher quality]”

// Common
extern int psSkeletonUpdate;
extern float r__dtex_range;

Flags32 ps_r__common_flags = { RFLAG_ACTOR_SHADOW }; // All renders

//int ps_r__Supersample = 1;
int ps_r__LightSleepFrames = 10;
// How many shadow-map faces may render per frame; excess faces keep lighting unshadowed.
// Per face, exactly as the reference build ships it (its default is 1 as well): one scene
// pass for the winning face, everything else goes the cheap path. The actor's torch is
// admitted above the budget. 0 = unlimited (one scene pass per source, stock behavior).
int ps_r__light_shadow_budget = 1;
// Middle/far sun cascade reuse TTL in ms, 0 = rebuild every frame (default). The cascade
// volume is fitted to the camera frustum, but cache validity never checks the view direction,
// so any turn or walk applies sun light through a stale volume: the newly revealed part of
// the frame stays black until the next rebuild. Proven by an A/B accumulator probe on the
// light_test save (with 100 ms the frame goes half-dark right after a turn; with 0 it never
// does). The reference project ships 100 ms, but on our content the hole is plainly visible.
int ps_r__sun_cache_ms = 0;

float ps_r__Detail_l_ambient = 0.9f;
float ps_r__Detail_l_aniso = 0.25f;
float ps_r__Detail_density = 0.3f;
float ps_r__Detail_height = 1.f;
float ps_r__Detail_rainbow_hemi = 0.75f;

float ps_r__Tree_SBC = 1.5f; // scale bias correct

float ps_r__WallmarkTTL = 50.f;
float ps_r__WallmarkSHIFT = 0.0001f;
float ps_r__WallmarkSHIFT_V = 0.0001f;

float ps_r__GLOD_ssa_start = 256.f;
float ps_r__GLOD_ssa_end = 64.f;
float ps_r__LOD = 0.75f;
//float ps_r__LOD_Power = 1.5f;
float ps_r__ssaDISCARD = 3.5f; // RO
float ps_r__ssaDONTSORT = 32.f; // RO
float ps_r__ssaHZBvsTEX = 96.f; // RO

int ps_r__tf_Anisotropic = 8;
float ps_r__tf_Mipbias = 0.0f;

int ps_r__clear_models_on_unload = 1; // Alundaio
int ps_r__unload_level_textures = 1;

// R1
float ps_r1_ssaLOD_A = 64.f;
float ps_r1_ssaLOD_B = 48.f;
Flags32 ps_r1_flags = {R1FLAG_DLIGHTS}; // r1-only
float ps_r1_lmodel_lerp = 0.1f;
float ps_r1_dlights_clip = 40.f;
float ps_r1_pps_u = 0.f;
float ps_r1_pps_v = 0.f;
int ps_r1_force_geomx = 0;

// R1-specific
int ps_r1_GlowsPerFrame = 16; // r1-only
float ps_r1_fog_luminance = 1.1f; // r1-only
int ps_r1_dynamic_lights = 1; // Dead Air preset compatibility
int ps_r1_SoftwareSkinning = 0; // r1-only

// R2
bool ps_r2_sun_static = false;
BOOL ps_r2_sun_complex = TRUE;
bool ps_r2_advanced_pp = true; // advanced post process and effects

float ps_r2_ssaLOD_A = 64.f;
float ps_r2_ssaLOD_B = 48.f;

// R2-specific
Flags32 ps_r2_ls_flags = {R2FLAG_SUN
    //| R2FLAG_SUN_IGNORE_PORTALS
    | R2FLAG_EXP_DONT_TEST_UNSHADOWED | R2FLAG_USE_NVSTENCIL | R2FLAG_EXP_SPLIT_SCENE | R2FLAG_EXP_MT_CALC |
    R3FLAG_DYN_WET_SURF | R3FLAG_VOLUMETRIC_SMOKE
    //| R3FLAG_MSAA
    //| R3FLAG_MSAA_OPT
    | R3FLAG_GBUFFER_OPT | R2FLAG_DETAIL_BUMP | R2FLAG_DOF | R2FLAG_SOFT_PARTICLES | R2FLAG_SOFT_WATER |
    R2FLAG_STEEP_PARALLAX | R2FLAG_SUN_FOCUS | R2FLAG_SUN_TSM | R2FLAG_TONEMAP | R2FLAG_VOLUMETRIC_LIGHTS}; // r2-only

Flags32 ps_r2_ls_flags_ext = {
    /*R2FLAGEXT_SSAO_OPT_DATA |*/ R2FLAGEXT_SSAO_HALF_DATA | R2FLAGEXT_ENABLE_TESSELLATION | R3FLAGEXT_SSR_HALF_DEPTH |
    R3FLAGEXT_SSR_JITTER};

float ps_r2_df_parallax_h = 0.02f;
float ps_r2_df_parallax_range = 75.f;
float ps_r2_tonemap_middlegray = 1.f; // r2-only
float ps_r2_tonemap_adaptation = 1.f; // r2-only
float ps_r2_tonemap_low_lum = 0.0001f; // r2-only
float ps_r2_tonemap_amount = 0.7f; // r2-only
float ps_r2_ls_bloom_kernel_g = 3.f; // r2-only
float ps_r2_ls_bloom_kernel_b = .7f; // r2-only
float ps_r2_ls_bloom_speed = 100.f; // r2-only
float ps_r2_ls_bloom_kernel_scale = .7f; // r2-only // gauss
float ps_r2_ls_dsm_kernel = .7f; // r2-only
float ps_r2_ls_psm_kernel = .7f; // r2-only
float ps_r2_ls_ssm_kernel = .7f; // r2-only
float ps_r2_ls_bloom_threshold = .00001f; // r2-only
Fvector ps_r2_aa_barier = {.8f, .1f, 0}; // r2-only
Fvector ps_r2_aa_weight = {.25f, .25f, 0}; // r2-only
float ps_r2_aa_kernel = .5f; // r2-only
float ps_r2_mblur = .0f; // .5f
int ps_r2_GI_depth = 1; // 1..5
int ps_r2_GI_photons = 16; // 8..64
float ps_r2_GI_clip = EPS_L; // EPS
float ps_r2_GI_refl = .9f; // .9f
float ps_r2_ls_depth_scale = 1.00001f; // 1.00001f
float ps_r2_ls_depth_bias = -0.0003f; // -0.0001f
float ps_r2_ls_squality = 1.0f; // 1.00f
float ps_r2_sun_tsm_projection = 0.3f; // 0.18f
float ps_r2_sun_tsm_bias = -0.2f;
float ps_r2_sun_near = 20.f; // 12.0f
float ps_r2_sun_near_border = 0.75f; // 1.0f
float ps_r2_sun_far = 100.f; // 180.f
float ps_r2_sun_depth_far_scale = 1.00000f; // 1.00001f
float ps_r2_sun_depth_far_bias = -0.00002f; // -0.0000f
float ps_r2_sun_depth_near_scale = 1.0000f; // 1.00001f
float ps_r2_sun_depth_near_bias = 0.00001f; // -0.00005f
float ps_r2_sun_lumscale = 1.0f; // 1.0f
float ps_r2_sun_lumscale_hemi = 1.0f; // 1.0f
float ps_r2_sun_lumscale_amb = 1.0f;
float ps_r2_gmaterial = 2.2f; //
float ps_r2_zfill = 0.25f; // .1f

float ps_r2_dhemi_sky_scale = 0.08f; // 1.5f
float ps_r2_dhemi_light_scale = 0.2f;
float ps_r2_dhemi_light_flow = 0.1f;
int ps_r2_dhemi_count = 5; // 5
int ps_r2_wait_sleep = 0;
int ps_r2_wait_timeout = 500;

float ps_r2_lt_smooth = 1.f; // 1.f
float ps_r2_slight_fade = 0.5f; // 1.f

//  x - min (0), y - focus (1.4), z - max (100)
Fvector3 ps_r2_dof = Fvector3().set(-1.25f, 1.4f, 600.f);
float ps_r2_dof_sky = 30; //    distance to sky
float ps_r2_dof_kernel_size = 5.0f; //  7.0f
int ps_r2_dof_pickable = 0;
float ps_r2_dof_time = 0.05f;
int ps_r2_dof_diff_near = -70;
int ps_r2_dof_diff_far = 70;

int ps_r2_technicolor = 0;
int ps_r2_vignette = 0;
BOOL ps_r2_filmgrain = FALSE;
int ps_r2_reflections = 0;
int ps_r2_lensdirt = 0;
int ps_r2_lenswater = 0;
float ps_r2_aberration = 0.f;
float ps_r2_vibrance = 0.f;
float ps_r2_lensdirt_value = 0.f;
float ps_r2_lenswater_value = 0.f;
float ps_r2_lumasharpen = 0.f;
Fvector4 ps_r2_temp{};
float ps_shaders_var_x = 0.f;
float ps_shaders_var_y = 0.f;
float ps_shaders_var_z = 0.f;
float ps_shaders_var_w = 0.f;
float ps_r2_postprocess_var_x = 0.f;
float ps_r2_postprocess_var_y = -1.f;
float ps_r2_postprocess_var_z = -1.f;
float ps_r2_postprocess_var_w = 0.f;
float ps_r2_lens_var_x = 0.f;
float ps_r2_lens_var_y = 0.f;
float ps_r2_lens_var_z = 0.f;
float ps_r2_lens_var_w = 0.f;
BOOL ps_detail_scale_on_fade = FALSE;

float ps_r3_dyn_wet_surf_near = 5.f; // 10.0f
float ps_r3_dyn_wet_surf_far = 20.f; // 30.0f
int ps_r3_dyn_wet_surf_sm_res = 256; // 256

u32 ps_steep_parallax = 0;
int ps_r__detail_radius = 49;

u32 dm_size = 24;
u32 dm_cache1_line = 12; //dm_size*2/dm_cache1_count
u32 dm_cache_line = 49; //dm_size+1+dm_size
u32 dm_cache_size = 2401; //dm_cache_line*dm_cache_line
float dm_fade = 47.5; //float(2*dm_size)-.5f;
u32 dm_current_size = 24;
u32 dm_current_cache1_line = 12; //dm_current_size*2/dm_cache1_count
u32 dm_current_cache_line = 49; //dm_current_size+1+dm_current_size
u32 dm_current_cache_size = 2401; //dm_current_cache_line*dm_current_cache_line
float dm_current_fade = 47.5; //float(2*dm_current_size)-.5f;

float ps_current_detail_density = 0.6f;
float ps_current_detail_height = 1.f;

int ps_r2_mt_calculate = 1;
int ps_r2_mt_render = 1;

xr_token ext_quality_token[] = {{"qt_off", 0}, {"qt_low", 1}, {"qt_medium", 2},
    {"qt_high", 3}, {"qt_extreme", 4}, {nullptr, 0}};
//-AVO

//- Mad Max
float ps_r2_gloss_factor = 4.0f;
//- Mad Max

//AVO: detail draw radius
class CCC_detail_radius : public CCC_Integer
{
public:
    void apply()
    {
        dm_current_size = iFloor((float)ps_r__detail_radius / 4) * 2;
        dm_current_cache1_line = dm_current_size * 2 / 4; // assuming cache1_count = 4
        dm_current_cache_line = dm_current_size + 1 + dm_current_size;
        dm_current_cache_size = dm_current_cache_line * dm_current_cache_line;
        dm_current_fade = float(2 * dm_current_size) - .5f;
    }

    CCC_detail_radius(LPCSTR N, int* V, int _min = 0, int _max = 999) : CCC_Integer(N, V, _min, _max) {};

    void Execute(LPCSTR args) override
    {
        CCC_Integer::Execute(args);
        apply();
    }

    void GetStatus(TStatus& S) override
    {
        CCC_Integer::GetStatus(S);
    }
};
//-AVO

class CCC_ClearModelsOnUnload final : public CCC_Integer
{
public:
    CCC_ClearModelsOnUnload(pcstr name, int* target, int minimum, int maximum)
        : CCC_Integer(name, target, minimum, maximum)
    {
    }

    void Execute(pcstr args) override
    {
        CCC_Integer::Execute(args);
        *value = 1;
    }
};

// Runtime-only experiment control: never persisted, so the parity default survives restarts.
class CCC_RuntimeInteger final : public CCC_Integer
{
public:
    CCC_RuntimeInteger(pcstr name, int* target, int minimum, int maximum)
        : CCC_Integer(name, target, minimum, maximum)
    {
    }

    void Save(IWriter*) override {}
};

class CCC_tf_Aniso : public CCC_Integer
{
public:
    void apply()
    {
#if defined(USE_DX11)
        if (nullptr == HW.pDevice)
            return;
#endif
        int val = *value;
        clamp(val, 1, 16);
#if defined(USE_DX11)
        SSManager.SetMaxAnisotropy(val);
#elif defined(USE_OGL)
        // OGL: don't set aniso here because it will be updated after vid restart
#else
#   error No graphics API selected or enabled!
#endif
    }
    CCC_tf_Aniso(LPCSTR N, int* v) : CCC_Integer(N, v, 1, 16){};
    virtual void Execute(LPCSTR args)
    {
        CCC_Integer::Execute(args);
        apply();
    }
    virtual void GetStatus(TStatus& S)
    {
        CCC_Integer::GetStatus(S);
        apply();
    }
};
class CCC_tf_MipBias : public CCC_Float
{
public:
    void apply()
    {
#if defined(USE_DX11)
        if (nullptr == HW.pDevice)
            return;

        SSManager.SetMipLODBias(*value);
#endif
    }

    CCC_tf_MipBias(LPCSTR N, float* v) : CCC_Float(N, v, -3.f, +3.f) {}
    virtual void Execute(LPCSTR args)
    {
        CCC_Float::Execute(args);
        apply();
    }
    virtual void GetStatus(TStatus& S)
    {
        CCC_Float::GetStatus(S);
        apply();
    }
};
class CCC_R2GM : public CCC_Float
{
public:
    CCC_R2GM(LPCSTR N, float* v) : CCC_Float(N, v, 0.f, 4.f) { *v = 0; };
    virtual void Execute(LPCSTR args)
    {
        if (0 == xr_strcmp(args, "on"))
        {
            ps_r2_ls_flags.set(R2FLAG_GLOBALMATERIAL, TRUE);
        }
        else if (0 == xr_strcmp(args, "off"))
        {
            ps_r2_ls_flags.set(R2FLAG_GLOBALMATERIAL, FALSE);
        }
        else
        {
            CCC_Float::Execute(args);
            if (ps_r2_ls_flags.test(R2FLAG_GLOBALMATERIAL))
            {
                static LPCSTR name[4] = {"oren", "blin", "phong", "metal"};
                float mid = *value;
                int m0 = iFloor(mid) % 4;
                int m1 = (m0 + 1) % 4;
                float frc = mid - float(iFloor(mid));
                Msg("* material set to [%s]-[%s], with lerp of [%f]", name[m0], name[m1], frc);
            }
        }
    }
};
class CCC_Screenshot : public IConsole_Command
{
public:
    CCC_Screenshot(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (GEnv.isDedicatedServer)
            return;

        string_path name;
        name[0] = 0;
        sscanf(args, "%s", name);
        LPCSTR image = xr_strlen(name) ? name : 0;
        RImplementation.Screenshot(IRender::SM_NORMAL, image);
    }
};

class CCC_ModelPoolStat : public IConsole_Command
{
public:
    CCC_ModelPoolStat(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { RImplementation.Models->dump(); }
};

class CCC_SSAO_Mode : public CCC_Token
{
public:
    CCC_SSAO_Mode(LPCSTR N, u32* V, const xr_token* T) : CCC_Token(N, V, T){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Token::Execute(args);

        switch (*value)
        {
        case ssao_mode_off:
        {
            ps_r_ssao = 0;
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HBAO, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HDAO, 0);
            break;
        }
        case ssao_mode_default:
        {
            if (ps_r_ssao == 0)
            {
                ps_r_ssao = 1;
            }
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HBAO, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HDAO, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HALF_DATA, 0);
            break;
        }
        case ssao_mode_hdao:
        {
            if (ps_r_ssao == 0)
            {
                ps_r_ssao = 1;
            }
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HBAO, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HDAO, 1);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_OPT_DATA, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HALF_DATA, 0);
            break;
        }
        case ssao_mode_hbao:
        {
            if (ps_r_ssao == 0)
            {
                ps_r_ssao = 1;
            }
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HBAO, 1);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HDAO, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_OPT_DATA, 1);
            break;
        }
        }
    }
};

//-----------------------------------------------------------------------
class CCC_Preset : public CCC_Token
{
public:
    CCC_Preset(LPCSTR N, u32* V, const xr_token* T) : CCC_Token(N, V, T){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Token::Execute(args);
        string_path _cfg;
        string_path cmd;

        switch (*value)
        {
        case 0: xr_strcpy(_cfg, "rspec_minimum.ltx"); break;
        case 1: xr_strcpy(_cfg, "rspec_low.ltx"); break;
        case 2: xr_strcpy(_cfg, "rspec_default.ltx"); break;
        case 3: xr_strcpy(_cfg, "rspec_high.ltx"); break;
        case 4: xr_strcpy(_cfg, "rspec_extreme.ltx"); break;
        }
        FS.update_path(_cfg, "$game_config$", _cfg);
        strconcat(sizeof(cmd), cmd, "cfg_load", " ", _cfg);
        Console->Execute(cmd);
    }
};

class CCC_memory_stats : public IConsole_Command
{
public:
    CCC_memory_stats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/)
    {
        // TODO: OGL: Implement memory usage statistics.
#if defined(USE_DX11)
        u32 m_base = 0;
        u32 c_base = 0;
        u32 m_lmaps = 0;
        u32 c_lmaps = 0;

        RImplementation.ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

        Msg("memory usage  mb \t \t video    \t managed      \t system \n");

        const float MiB = 1024*1024; // XXX: use it as common enum value (like in X-Ray 2.0)
        const u32* mem_usage = HW.stats_manager.memory_usage_summary[enum_stats_buffer_type_vertex];

        float vb_video = mem_usage[D3DPOOL_DEFAULT] / MiB;
        float vb_managed = mem_usage[D3DPOOL_MANAGED] / MiB;
        float vb_system = mem_usage[D3DPOOL_SYSTEMMEM] / MiB;
        Msg("vertex buffer      \t \t %f \t %f \t %f ", vb_video, vb_managed, vb_system);

        float ib_video = mem_usage[D3DPOOL_DEFAULT] / MiB;
        float ib_managed = mem_usage[D3DPOOL_MANAGED] / MiB;
        float ib_system = mem_usage[D3DPOOL_SYSTEMMEM] / MiB;
        Msg("index buffer      \t \t %f \t %f \t %f ", ib_video, ib_managed, ib_system);

        float textures_video = (m_base+m_lmaps)/MiB;
        Msg("textures          \t \t %f \t %f \t %f ", textures_video, 0.f, 0.f);

        mem_usage = HW.stats_manager.memory_usage_summary[enum_stats_buffer_type_rtarget];
        float rt_video = mem_usage[D3DPOOL_DEFAULT] / MiB;
        float rt_managed = mem_usage[D3DPOOL_MANAGED] / MiB;
        float rt_system = mem_usage[D3DPOOL_SYSTEMMEM] / MiB;
        Msg("R-Targets         \t \t %f \t %f \t %f ", rt_video, rt_managed, rt_system);

        Msg("\nTotal             \t \t %f \t %f \t %f ", vb_video + ib_video + textures_video + rt_video,
            vb_managed + ib_managed + rt_managed, vb_system + ib_system + rt_system);
#endif // !USE_OGL
    }
};

class CCC_DumpResources final : public IConsole_Command
{
public:
    CCC_DumpResources(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr /*args*/) override
    {
        RImplementation.Models->dump();
        RImplementation.Resources->Dump(false);
    }
};

class CCC_MotionsStat final : public IConsole_Command
{
public:
    CCC_MotionsStat(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr /*args*/) override
    {
        g_pMotionsContainer->dump();
    }
};

class CCC_TexturesStat final : public IConsole_Command
{
public:
    CCC_TexturesStat(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr /*args*/) override
    {
        RImplementation.Resources->_DumpMemoryUsage();
    }
};

#if RENDER != R_R1
class CCC_BuildSSA : public IConsole_Command
{
public:
    CCC_BuildSSA(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/)
    {
        r_pixel_calculator c;
        c.run();
    }
};
#endif

class CCC_DofFar : public CCC_Float
{
public:
    CCC_DofFar(LPCSTR N, float* V, float _min = 0.0f, float _max = 10000.0f) : CCC_Float(N, V, _min, _max) {}
    virtual void Execute(LPCSTR args)
    {
        float v = float(atof(args));

        if (v < ps_r2_dof.y + 0.1f)
        {
            char pBuf[256];
            _snprintf(pBuf, sizeof(pBuf) / sizeof(pBuf[0]), "float value greater or equal to r2_dof_focus+0.1");
            Msg("~ Invalid syntax in call to '%s'", cName);
            Msg("~ Valid arguments: %s", pBuf);
            Console->Execute("r2_dof_focus");
        }
        else
        {
            CCC_Float::Execute(args);
            if (g_pGamePersistent)
                g_pGamePersistent->SetBaseDof(ps_r2_dof);
        }
    }

    //  CCC_Dof should save all data as well as load from config
    virtual void Save(IWriter* /*F*/) { ; }
};

class CCC_DofNear : public CCC_Float
{
public:
    CCC_DofNear(LPCSTR N, float* V, float _min = 0.0f, float _max = 10000.0f) : CCC_Float(N, V, _min, _max) {}
    virtual void Execute(LPCSTR args)
    {
        float v = float(atof(args));

        if (v > ps_r2_dof.y - 0.1f)
        {
            char pBuf[256];
            _snprintf(pBuf, sizeof(pBuf) / sizeof(pBuf[0]), "float value less or equal to r2_dof_focus-0.1");
            Msg("~ Invalid syntax in call to '%s'", cName);
            Msg("~ Valid arguments: %s", pBuf);
            Console->Execute("r2_dof_focus");
        }
        else
        {
            CCC_Float::Execute(args);
            if (g_pGamePersistent)
                g_pGamePersistent->SetBaseDof(ps_r2_dof);
        }
    }

    // CCC_Dof should save all data as well as load from config
    virtual void Save(IWriter* /*F*/) { ; }
};

class CCC_DofFocus : public CCC_Float
{
public:
    CCC_DofFocus(LPCSTR N, float* V, float _min = 0.0f, float _max = 10000.0f) : CCC_Float(N, V, _min, _max) {}
    virtual void Execute(LPCSTR args)
    {
        float v = float(atof(args));

        if (v > ps_r2_dof.z - 0.1f)
        {
            char pBuf[256];
            _snprintf(pBuf, sizeof(pBuf) / sizeof(pBuf[0]), "float value less or equal to r2_dof_far-0.1");
            Msg("~ Invalid syntax in call to '%s'", cName);
            Msg("~ Valid arguments: %s", pBuf);
            Console->Execute("r2_dof_far");
        }
        else if (v < ps_r2_dof.x + 0.1f)
        {
            char pBuf[256];
            _snprintf(pBuf, sizeof(pBuf) / sizeof(pBuf[0]), "float value greater or equal to r2_dof_far-0.1");
            Msg("~ Invalid syntax in call to '%s'", cName);
            Msg("~ Valid arguments: %s", pBuf);
            Console->Execute("r2_dof_near");
        }
        else
        {
            CCC_Float::Execute(args);
            if (g_pGamePersistent)
                g_pGamePersistent->SetBaseDof(ps_r2_dof);
        }
    }

    //  CCC_Dof should save all data as well as load from config
    virtual void Save(IWriter* /*F*/) { ; }
};

class CCC_Dof : public CCC_Vector3
{
public:
    CCC_Dof(LPCSTR N, Fvector* V, const Fvector _min, const Fvector _max) : CCC_Vector3(N, V, _min, _max) { ; }
    virtual void Execute(LPCSTR args)
    {
        Fvector v;
        if (3 != sscanf(args, "%f,%f,%f", &v.x, &v.y, &v.z))
            InvalidSyntax();
        else if ((v.x > v.y - 0.1f) || (v.z < v.y + 0.1f))
        {
            InvalidSyntax();
            Msg("x <= y - 0.1");
            Msg("y <= z - 0.1");
        }
        else
        {
            CCC_Vector3::Execute(args);
            if (g_pGamePersistent)
                g_pGamePersistent->SetBaseDof(ps_r2_dof);
        }
    }
    virtual void GetStatus(TStatus& S) { xr_sprintf(S, "%f,%f,%f", value->x, value->y, value->z); }
    virtual void Info(TInfo& I)
    {
        xr_sprintf(I, "vector3 in range [%f,%f,%f]-[%f,%f,%f]", min.x, min.y, min.z, max.x, max.y, max.z);
    }
};

//  Allow real-time fog config reload
#if (RENDER == R_R3) || (RENDER == R_R4)
#   ifndef MASTER_GOLD
class CCC_Fog_Reload : public IConsole_Command
{
public:
    CCC_Fog_Reload(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR /*args*/) { FluidManager.UpdateProfiles(); }
};
#   endif // MASTER_GOLD
#endif // (RENDER == R_R3) || (RENDER == R_R4)

#ifdef USE_DX11
// Diagnostics: queue N RenderDoc frame captures when running under the RenderDoc injector,
// so QA scripts can capture the exact defective frames from lua.
class CCC_RdcCapture final : public IConsole_Command
{
public:
    explicit CCC_RdcCapture(LPCSTR name) : IConsole_Command(name) { bEmptyArgsHandled = TRUE; }

    void Execute(LPCSTR args) override
    {
        extern void dx11_rdc_trigger(u32 frames);
        const int frames = atoi(args);
        dx11_rdc_trigger(u32(frames > 0 ? frames : 1));
    }
};
#endif

//-----------------------------------------------------------------------
void xrRender_initconsole()
{
    ZoneScoped;

    CMD3(CCC_Preset, "_preset", &ps_Preset, qpreset_token);

#ifdef USE_DX11
    CMD1(CCC_RdcCapture, "rdc_capture");
#endif

    CMD4(CCC_Integer, "rs_skeleton_update", &psSkeletonUpdate, 2, 128);
#ifndef MASTER_GOLD
    CMD1(CCC_DumpResources, "dump_resources");
    CMD1(CCC_MotionsStat, "stat_motions");
    CMD1(CCC_TexturesStat, "stat_textures");
#endif

    CMD4(CCC_Float, "r__dtex_range", &r__dtex_range, 5, 175);

    // Common
    CMD1(CCC_Screenshot, "screenshot");

#ifdef DEBUG
#if RENDER != R_R1
    CMD1(CCC_BuildSSA, "build_ssa");
#endif
    CMD4(CCC_Integer, "r__lsleep_frames", &ps_r__LightSleepFrames, 4, 30);
    CMD4(CCC_Float, "r__ssa_glod_start", &ps_r__GLOD_ssa_start, 128, 512);
    CMD4(CCC_Float, "r__ssa_glod_end", &ps_r__GLOD_ssa_end, 16, 96);
    CMD4(CCC_Float, "r__wallmark_shift_pp", &ps_r__WallmarkSHIFT, 0.0f, 1.f);
    CMD4(CCC_Float, "r__wallmark_shift_v", &ps_r__WallmarkSHIFT_V, 0.0f, 1.f);
    CMD1(CCC_ModelPoolStat, "stat_models");
#endif // DEBUG
    CMD4(CCC_Float, "r__wallmark_ttl", &ps_r__WallmarkTTL, 1.0f, 10.f * 60.f);

    CMD4(CCC_Integer, "r__supersample", &ps_r__Supersample, 1, 8);

    CMD4(CCC_Float, "r__geometry_lod", &ps_r__LOD, 0.1f, 2.f);
    CMD3(CCC_Token, "r__optimize_static_geom", &ps_r_optimize_static, q_optimize_static_token);
    //CMD4(CCC_Float, "r__geometry_lod_pow", &ps_r__LOD_Power, 0, 2);

    CMD4(CCC_Float, "r__detail_density", &ps_current_detail_density/*&ps_r__Detail_density*/, 0.1f, 1.f);
    CMD4(CCC_detail_radius, "r__detail_radius", &ps_r__detail_radius, 49, 300);
    CMD4(CCC_Float, "r__detail_height", &ps_r__Detail_height, 0.1f, 2.f);

#ifdef DEBUG
    CMD4(CCC_Float, "r__detail_l_ambient", &ps_r__Detail_l_ambient, .5f, .95f);
    CMD4(CCC_Float, "r__detail_l_aniso", &ps_r__Detail_l_aniso, .1f, .5f);
#endif // DEBUG

    CMD3(CCC_Mask, "r__actor_shadow", &ps_r__common_flags, RFLAG_ACTOR_SHADOW);
    CMD3(CCC_Mask, "r__actor_body", &ps_r__common_flags, RFLAG_ACTOR_BODY);

    CMD2(CCC_tf_Aniso, "r__tf_aniso", &ps_r__tf_Anisotropic); // {1..16}
    CMD2(CCC_tf_MipBias, "r1_tf_mipbias", &ps_r__tf_Mipbias); // {-3 +3}
    CMD2(CCC_tf_MipBias, "r2_tf_mipbias", &ps_r__tf_Mipbias); // {-3 +3}

    CMD4(CCC_ClearModelsOnUnload, "r__clear_models_on_unload", &ps_r__clear_models_on_unload, 0, 1);
    CMD4(CCC_Integer, "r__unload_level_textures", &ps_r__unload_level_textures, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__light_shadow_budget", &ps_r__light_shadow_budget, 0, 64);
    CMD4(CCC_RuntimeInteger, "r__sun_cache_ms", &ps_r__sun_cache_ms, 0, 1000);

    // R1
    CMD4(CCC_Float, "r1_ssa_lod_a", &ps_r1_ssaLOD_A, 16, 96);
    CMD4(CCC_Float, "r1_ssa_lod_b", &ps_r1_ssaLOD_B, 16, 64);
    CMD4(CCC_Float, "r1_lmodel_lerp", &ps_r1_lmodel_lerp, 0, 0.333f);
    CMD3(CCC_Mask, "r1_dlights", &ps_r1_flags, R1FLAG_DLIGHTS);
    CMD4(CCC_Float, "r1_dlights_clip", &ps_r1_dlights_clip, 10.f, 150.f);
    CMD4(CCC_Float, "r1_pps_u", &ps_r1_pps_u, -1.f, +1.f);
    CMD4(CCC_Float, "r1_pps_v", &ps_r1_pps_v, -1.f, +1.f);
    CMD4(CCC_Integer, "r1_force_geomx", &ps_r1_force_geomx, 0, 1);

    // R1-specific
    CMD4(CCC_Integer, "r1_glows_per_frame", &ps_r1_GlowsPerFrame, 2, 32);
    CMD3(CCC_Mask, "r1_detail_textures", &ps_r2_ls_flags, R1FLAG_DETAIL_TEXTURES);

    CMD4(CCC_Float, "r1_fog_luminance", &ps_r1_fog_luminance, 0.2f, 5.f);
    CMD4(CCC_Integer, "r1_dynamic_lights", &ps_r1_dynamic_lights, 0, 1);

    // Software Skinning
    // 0 - disabled (renderer can override)
    // 1 - enabled
    // 2 - forced hardware skinning (renderer can not override)
    CMD4(CCC_Integer, "r1_software_skinning", &ps_r1_SoftwareSkinning, 0, 2);

    CMD3(CCC_Mask, "r1_ffp", &ps_r1_flags, R1FLAG_FFP);
    CMD3(CCC_Mask, "r1_ffp_lightmaps", &ps_r1_flags, R1FLAG_FFP_LIGHTMAPS);

    // R2
    CMD4(CCC_Float, "r2_ssa_lod_a", &ps_r2_ssaLOD_A, 16, 96);
    CMD4(CCC_Float, "r2_ssa_lod_b", &ps_r2_ssaLOD_B, 32, 64);

    // R2-specific
    CMD2(CCC_R2GM, "r2em", &ps_r2_gmaterial);
    CMD3(CCC_Mask, "r2_tonemap", &ps_r2_ls_flags, R2FLAG_TONEMAP);
    CMD4(CCC_Float, "r2_tonemap_middlegray", &ps_r2_tonemap_middlegray, 0.0f, 2.0f);
    CMD4(CCC_Float, "r2_tonemap_adaptation", &ps_r2_tonemap_adaptation, 0.01f, 10.0f);
    CMD4(CCC_Float, "r2_tonemap_lowlum", &ps_r2_tonemap_low_lum, 0.0001f, 1.0f);
    CMD4(CCC_Float, "r2_tonemap_amount", &ps_r2_tonemap_amount, 0.0000f, 1.0f);
    CMD4(CCC_Float, "r2_ls_bloom_kernel_scale", &ps_r2_ls_bloom_kernel_scale, 0.5f, 2.f);
    CMD4(CCC_Float, "r2_ls_bloom_kernel_g", &ps_r2_ls_bloom_kernel_g, 1.f, 20.f);
    CMD4(CCC_Float, "r2_ls_bloom_kernel_b", &ps_r2_ls_bloom_kernel_b, 0.01f, 1.f);
    CMD4(CCC_Float, "r2_ls_bloom_threshold", &ps_r2_ls_bloom_threshold, 0.f, 1.f);
    CMD4(CCC_Float, "r2_ls_bloom_speed", &ps_r2_ls_bloom_speed, 0.f, 100.f);
    CMD3(CCC_Mask, "r2_ls_bloom_fast", &ps_r2_ls_flags, R2FLAG_FASTBLOOM);
    CMD4(CCC_Float, "r2_ls_dsm_kernel", &ps_r2_ls_dsm_kernel, .1f, 3.f);
    CMD4(CCC_Float, "r2_ls_psm_kernel", &ps_r2_ls_psm_kernel, .1f, 3.f);
    CMD4(CCC_Float, "r2_ls_ssm_kernel", &ps_r2_ls_ssm_kernel, .1f, 3.f);
    CMD4(CCC_Float, "r2_ls_squality", &ps_r2_ls_squality, .5f, 1.f);

    CMD3(CCC_Mask, "r2_zfill", &ps_r2_ls_flags, R2FLAG_ZFILL);
    CMD4(CCC_Float, "r2_zfill_depth", &ps_r2_zfill, .001f, .5f);
    CMD3(CCC_Mask, "r2_allow_r1_lights", &ps_r2_ls_flags, R2FLAG_R1LIGHTS);

    //- Mad Max
    CMD4(CCC_Float, "r2_gloss_factor", &ps_r2_gloss_factor, .0f, 10.f);
//- Mad Max

#ifdef DEBUG
    CMD3(CCC_Mask, "r2_use_nvdbt", &ps_r2_ls_flags, R2FLAG_USE_NVDBT);
    CMD3(CCC_Mask, "r2_mt", &ps_r2_ls_flags, R2FLAG_EXP_MT_CALC);
#endif // DEBUG

    CMD3(CCC_Mask, "r2_sun", &ps_r2_ls_flags, R2FLAG_SUN);
    CMD4(CCC_Integer, "r2_sun_complex", &ps_r2_sun_complex, 0, 1);
    CMD3(CCC_Token, "r2_sun_details", &ps_r_sun_details, qsun_details_token);
    CMD3(CCC_Mask, "r2_sun_focus", &ps_r2_ls_flags, R2FLAG_SUN_FOCUS);
    //CMD3(CCC_Mask, "r2_sun_static", &ps_r2_ls_flags, R2FLAG_SUN_STATIC);
    //CMD3(CCC_Mask, "r2_exp_splitscene", &ps_r2_ls_flags, R2FLAG_EXP_SPLIT_SCENE);
    //CMD3(CCC_Mask, "r2_exp_donttest_uns", &ps_r2_ls_flags, R2FLAG_EXP_DONT_TEST_UNSHADOWED);
    CMD3(CCC_Mask, "r2_exp_donttest_shad", &ps_r2_ls_flags, R2FLAG_EXP_DONT_TEST_SHADOWED);

    CMD3(CCC_Mask, "r2_sun_tsm", &ps_r2_ls_flags, R2FLAG_SUN_TSM);
    CMD4(CCC_Float, "r2_sun_tsm_proj", &ps_r2_sun_tsm_projection, .001f, 0.8f);
    CMD4(CCC_Float, "r2_sun_tsm_bias", &ps_r2_sun_tsm_bias, -0.5, +0.5);
    CMD4(CCC_Float, "r2_sun_near", &ps_r2_sun_near, 1.f, 150.f); //AVO: extended from 50.f to 150.f
#if RENDER != R_R1
    CMD4(CCC_Float, "r2_sun_far", &ps_r2_sun_far, 51.f, 180.f);
#endif
    CMD4(CCC_Float, "r2_sun_near_border", &ps_r2_sun_near_border, .5f, 1.0f);
    CMD4(CCC_Float, "r2_sun_depth_far_scale", &ps_r2_sun_depth_far_scale, 0.5, 1.5);
    CMD4(CCC_Float, "r2_sun_depth_far_bias", &ps_r2_sun_depth_far_bias, -0.5, +0.5);
    CMD4(CCC_Float, "r2_sun_depth_near_scale", &ps_r2_sun_depth_near_scale, 0.5, 1.5);
    CMD4(CCC_Float, "r2_sun_depth_near_bias", &ps_r2_sun_depth_near_bias, -0.5, +0.5);
    CMD4(CCC_Float, "r2_sun_lumscale", &ps_r2_sun_lumscale, -1.0, +3.0);
    CMD4(CCC_Float, "r2_sun_lumscale_hemi", &ps_r2_sun_lumscale_hemi, 0.0, +3.0);
    CMD4(CCC_Float, "r2_sun_lumscale_amb", &ps_r2_sun_lumscale_amb, 0.0, +3.0);

    CMD3(CCC_Mask, "r2_aa", &ps_r2_ls_flags, R2FLAG_AA);
    CMD4(CCC_Float, "r2_aa_kernel", &ps_r2_aa_kernel, 0.3f, 0.7f);
    CMD4(CCC_Float, "r2_mblur", &ps_r2_mblur, 0.0f, 1.0f);

    CMD3(CCC_Mask, "r2_gi", &ps_r2_ls_flags, R2FLAG_GI);
    CMD4(CCC_Float, "r2_gi_clip", &ps_r2_GI_clip, EPS, 0.1f);
    CMD4(CCC_Integer, "r2_gi_depth", &ps_r2_GI_depth, 1, 5);
    CMD4(CCC_Integer, "r2_gi_photons", &ps_r2_GI_photons, 8, 256);
    CMD4(CCC_Float, "r2_gi_refl", &ps_r2_GI_refl, EPS_L, 0.99f);

    CMD4(CCC_Integer, "r2_wait_sleep", &ps_r2_wait_sleep, 0, 1);
    CMD4(CCC_Integer, "r2_wait_timeout", &ps_r2_wait_timeout, 100, 1000);

#ifndef MASTER_GOLD
    CMD4(CCC_Integer, "r2_dhemi_count", &ps_r2_dhemi_count, 4, 25);
    CMD4(CCC_Float, "r2_dhemi_sky_scale", &ps_r2_dhemi_sky_scale, 0.0f, 100.f);
    CMD4(CCC_Float, "r2_dhemi_light_scale", &ps_r2_dhemi_light_scale, 0, 100.f);
    CMD4(CCC_Float, "r2_dhemi_light_flow", &ps_r2_dhemi_light_flow, 0, 1.f);
    CMD4(CCC_Float, "r2_dhemi_smooth", &ps_r2_lt_smooth, 0.f, 10.f);
    CMD3(CCC_Mask, "rs_hom_depth_draw", &ps_r2_ls_flags_ext, R_FLAGEXT_HOM_DEPTH_DRAW);
    CMD3(CCC_Mask, "r2_shadow_cascede_zcul", &ps_r2_ls_flags_ext, R2FLAGEXT_SUN_ZCULLING);
    CMD3(CCC_Mask, "r2_shadow_cascede_old", &ps_r2_ls_flags_ext, R2FLAGEXT_SUN_OLD);

#endif // DEBUG

    CMD4(CCC_Float, "r2_ls_depth_scale", &ps_r2_ls_depth_scale, 0.5, 1.5);
    CMD4(CCC_Float, "r2_ls_depth_bias", &ps_r2_ls_depth_bias, -0.5, +0.5);

    CMD4(CCC_Float, "r2_parallax_h", &ps_r2_df_parallax_h, .0f, .5f);
    //  CMD4(CCC_Float,     "r2_parallax_range",    &ps_r2_df_parallax_range,   5.0f,   175.0f  );

    CMD4(CCC_Float, "r2_slight_fade", &ps_r2_slight_fade, .2f, 1.f);
    CMD3(CCC_Token, "r2_smap_size", &ps_r2_smapsize, qsmapsize_token);
    CMD3(CCC_Token, "r2_shadow_map_size", &ps_r2_smapsize, qsmapsize_token);

    Fvector tw_min, tw_max;
    tw_min.set(0, 0, 0);
    tw_max.set(1, 1, 1);
    CMD4(CCC_Vector3, "r2_aa_break", &ps_r2_aa_barier, tw_min, tw_max);

    tw_min.set(0, 0, 0);
    tw_max.set(1, 1, 1);
    CMD4(CCC_Vector3, "r2_aa_weight", &ps_r2_aa_weight, tw_min, tw_max);

    // Igor: Depth of field
    tw_min.set(-10000, -10000, 0);
    tw_max.set(10000, 10000, 10000);
    CMD4(CCC_Dof, "r2_dof", &ps_r2_dof, tw_min, tw_max);
    CMD4(CCC_DofNear, "r2_dof_near", &ps_r2_dof.x, tw_min.x, tw_max.x);
    CMD4(CCC_DofFocus, "r2_dof_focus", &ps_r2_dof.y, tw_min.y, tw_max.y);
    CMD4(CCC_DofFar, "r2_dof_far", &ps_r2_dof.z, tw_min.z, tw_max.z);

    CMD4(CCC_Float, "r2_dof_kernel", &ps_r2_dof_kernel_size, .0f, 10.f);
    CMD4(CCC_Float, "r2_dof_sky", &ps_r2_dof_sky, -10000.f, 10000.f);
    CMD3(CCC_Mask, "r2_dof_enable", &ps_r2_ls_flags, R2FLAG_DOF);
    CMD4(CCC_Integer, "r2_dof_pickable", &ps_r2_dof_pickable, 0, 1);
    CMD4(CCC_Float, "r2_dof_time", &ps_r2_dof_time, 0.f, 10.f);
    CMD4(CCC_Integer, "r2_dof_diff_near", &ps_r2_dof_diff_near, -10000, 10000);
    CMD4(CCC_Integer, "r2_dof_diff_far", &ps_r2_dof_diff_far, -10000, 10000);

    CMD4(CCC_Integer, "r2_technicolor", &ps_r2_technicolor, 0, 1);
    CMD4(CCC_Integer, "r2_vignette", &ps_r2_vignette, 0, 1);
    CMD4(CCC_Integer, "r2_filmgrain", &ps_r2_filmgrain, 0, 1);
    CMD4(CCC_Integer, "r2_reflections", &ps_r2_reflections, 0, 2);
    CMD4(CCC_Integer, "r2_lensdirt", &ps_r2_lensdirt, 0, 1);
    CMD4(CCC_Integer, "r2_lenswater", &ps_r2_lenswater, 0, 1);
    CMD4(CCC_Float, "r2_aberration_val", &ps_r2_aberration, 0.f, 1.f);
    CMD4(CCC_Float, "r2_aberration", &ps_r2_aberration, 0.f, 1.f);
    CMD4(CCC_Float, "r2_vibrance_val", &ps_r2_vibrance, -1.f, 1.f);
    CMD4(CCC_Float, "r2_lensdirt_val", &ps_r2_lensdirt_value, 0.f, 1.f);
    CMD4(CCC_Float, "r2_lenswater_val", &ps_r2_lenswater_value, 0.f, 1.f);
    CMD4(CCC_Float, "r2_lumasharpen", &ps_r2_lumasharpen, 0.f, 1.f);
    CMD4(CCC_Float, "r2_tmp_x", &ps_r2_temp.x, -1.f, 1.f);
    CMD4(CCC_Float, "r2_tmp_y", &ps_r2_temp.y, -1.f, 1.f);
    CMD4(CCC_Float, "r2_tmp_z", &ps_r2_temp.z, -1.f, 1.f);
    CMD4(CCC_Float, "r2_tmp_w", &ps_r2_temp.w, -1.f, 1.f);
    CMD4(CCC_Float, "shaders_var_x", &ps_shaders_var_x, -1.f, 1.f);
    CMD4(CCC_Float, "shaders_var_y", &ps_shaders_var_y, -1.f, 1.f);
    CMD4(CCC_Float, "shaders_var_z", &ps_shaders_var_z, -1.f, 1.f);
    CMD4(CCC_Float, "shaders_var_w", &ps_shaders_var_w, -1.f, 1.f);
    CMD4(CCC_Float, "r2_postprocess_var_x", &ps_r2_postprocess_var_x, -1.f, 1.f);
    CMD4(CCC_Float, "r2_postprocess_var_y", &ps_r2_postprocess_var_y, -1.f, 1.f);
    CMD4(CCC_Float, "r2_postprocess_var_z", &ps_r2_postprocess_var_z, -1.f, 1.f);
    CMD4(CCC_Float, "r2_postprocess_var_w", &ps_r2_postprocess_var_w, -1.f, 1.f);
    CMD4(CCC_Float, "r2_lens_var_x", &ps_r2_lens_var_x, -1.f, 1.f);
    CMD4(CCC_Float, "r2_lens_var_y", &ps_r2_lens_var_y, -1.f, 1.f);
    CMD4(CCC_Float, "r2_lens_var_z", &ps_r2_lens_var_z, -1.f, 1.f);
    CMD4(CCC_Float, "r2_lens_var_w", &ps_r2_lens_var_w, -1.f, 1.f);
    CMD4(CCC_Integer, "r__detail_scale_on_fade", &ps_detail_scale_on_fade, 0, 1);
    CMD4(CCC_Float, "r2_mblur_value", &ps_r2_mblur, 0.f, 1.f);

    //float ps_r2_dof_near = 0.f; // 0.f
    //float ps_r2_dof_focus = 1.4f; // 1.4f

    CMD3(CCC_Mask, "r2_volumetric_lights", &ps_r2_ls_flags, R2FLAG_VOLUMETRIC_LIGHTS);
    //CMD3(CCC_Mask, "r2_sun_shafts", &ps_r2_ls_flags, R2FLAG_SUN_SHAFTS);
    CMD3(CCC_Token, "r2_sun_shafts", &ps_r_sun_shafts, qsun_shafts_token);
    CMD4(CCC_Float, "r2_sun_shafts_value", &ps_r2_sun_shafts_value, 0.f, 0.4f);
    CMD4(CCC_Integer, "r2_sss_enable", &ps_r2_sss_enable, 0, 1);
    CMD4(CCC_Float, "r2_sss_intensity", &ps_r2_sss_intensity, 0.f, 2.f);
    CMD4(CCC_Float, "r2_sss_blend", &ps_r2_sss_blend, 0.01f, 1.f);
    CMD4(CCC_Float, "r2_sss_phase0", &ps_r2_sss_phase1, 0.01f, 0.2f);
    CMD4(CCC_Float, "r2_sss_phase1", &ps_r2_sss_phase1, 0.01f, 0.2f);
    CMD4(CCC_Float, "r2_sss_phase2", &ps_r2_sss_phase2, 0.01f, 0.2f);
    CMD4(CCC_Float, "r2_sss_radius", &ps_r2_sss_radius, 0.5f, 2.f);
    CMD4(CCC_Integer, "r2_fxaa", &ps_r2_fxaa, 0, 1);
    CMD3(CCC_SSAO_Mode, "r2_ssao_mode", &ps_r_ssao_mode, qssao_mode_token);
    CMD3(CCC_Token, "r2_ssao", &ps_r_ssao, qssao_token);
    CMD3(CCC_Mask, "r2_ssao_blur", &ps_r2_ls_flags_ext, R2FLAGEXT_SSAO_BLUR); // Need restart
    CMD3(CCC_Mask, "r2_ssao_opt_data", &ps_r2_ls_flags_ext, R2FLAGEXT_SSAO_OPT_DATA); // Need restart
    CMD3(CCC_Mask, "r2_ssao_half_data", &ps_r2_ls_flags_ext, R2FLAGEXT_SSAO_HALF_DATA); // Need restart
    CMD3(CCC_Mask, "r2_ssao_hbao", &ps_r2_ls_flags_ext, R2FLAGEXT_SSAO_HBAO); // Need restart
    CMD3(CCC_Mask, "r2_ssao_hdao", &ps_r2_ls_flags_ext, R2FLAGEXT_SSAO_HDAO); // Need restart
    CMD3(CCC_Mask, "r4_enable_tessellation", &ps_r2_ls_flags_ext, R2FLAGEXT_ENABLE_TESSELLATION); // Need restart
    CMD3(CCC_Mask, "r4_wireframe", &ps_r2_ls_flags_ext, R2FLAGEXT_WIREFRAME); // Need restart
    CMD3(CCC_Mask, "r2_steep_parallax", &ps_r2_ls_flags, R2FLAG_STEEP_PARALLAX);
    CMD3(CCC_Mask, "r2_detail_bump", &ps_r2_ls_flags, R2FLAG_DETAIL_BUMP);

    CMD3(CCC_Token, "r2_sun_quality", &ps_r_sun_quality, qsun_quality_token);
    CMD3(CCC_Token, "r2_lighting_quality", &ps_r_lighting_quality, qlighting_quality_token);

    //Igor: need restart
    CMD3(CCC_Mask, "r2_soft_water", &ps_r2_ls_flags, R2FLAG_SOFT_WATER);
    CMD3(CCC_Mask, "r2_soft_particles", &ps_r2_ls_flags, R2FLAG_SOFT_PARTICLES);

    CMD3(CCC_Token, "r3_water_refl", &ps_r_water_reflection, qwater_reflection_quality_token);
    CMD3(CCC_Mask, "r3_water_refl_half_depth", &ps_r2_ls_flags_ext, R3FLAGEXT_SSR_HALF_DEPTH);
    CMD3(CCC_Mask, "r3_water_refl_jitter", &ps_r2_ls_flags_ext, R3FLAGEXT_SSR_JITTER);

    //CMD3(CCC_Mask, "r3_msaa", &ps_r2_ls_flags, R3FLAG_MSAA);
    CMD3(CCC_Token, "r3_msaa", &ps_r3_msaa, qmsaa_token);
    //CMD3(CCC_Mask, "r3_msaa_hybrid", &ps_r2_ls_flags, R3FLAG_MSAA_HYBRID);
    //CMD3(CCC_Mask, "r3_msaa_opt", &ps_r2_ls_flags, R3FLAG_MSAA_OPT);
    CMD3(CCC_Mask, "r3_gbuffer_opt", &ps_r2_ls_flags, R3FLAG_GBUFFER_OPT);
    CMD3(CCC_Mask, "r3_use_dx10_1", &ps_r2_ls_flags, (u32)R3FLAG_USE_DX10_1);
    //CMD3(CCC_Mask, "r3_msaa_alphatest", &ps_r2_ls_flags, (u32)R3FLAG_MSAA_ALPHATEST);
    CMD3(CCC_Token, "r3_msaa_alphatest", &ps_r3_msaa_atest, qmsaa__atest_token);
    CMD3(CCC_Token, "r3_minmax_sm", &ps_r3_minmax_sm, qminmax_sm_token);

//  Allow real-time fog config reload
#if (RENDER == R_R3) || (RENDER == R_R4)
#   ifndef MASTER_GOLD
    CMD1(CCC_Fog_Reload, "r3_fog_reload");
#   endif
#endif // (RENDER == R_R3) || (RENDER == R_R4)

    CMD3(CCC_Mask, "r3_dynamic_wet_surfaces", &ps_r2_ls_flags, R3FLAG_DYN_WET_SURF);
    CMD4(CCC_Float, "r3_dynamic_wet_surfaces_near", &ps_r3_dyn_wet_surf_near, 5, 70);
    CMD4(CCC_Float, "r3_dynamic_wet_surfaces_far", &ps_r3_dyn_wet_surf_far, 20, 100);
    CMD4(CCC_Integer, "r3_dynamic_wet_surfaces_sm_res", &ps_r3_dyn_wet_surf_sm_res, 64, 2048);
    CMD3(CCC_Mask, "r2_dynamic_wet_surfaces", &ps_r2_ls_flags, R3FLAG_DYN_WET_SURF);
    CMD4(CCC_Float, "r2_dynamic_wet_surfaces_near", &ps_r3_dyn_wet_surf_near, 5, 70);
    CMD4(CCC_Float, "r2_dynamic_wet_surfaces_far", &ps_r3_dyn_wet_surf_far, 20, 100);
    CMD4(CCC_Integer, "r2_dynamic_wet_surfaces_sm_res", &ps_r3_dyn_wet_surf_sm_res, 64, 2048);

    CMD3(CCC_Mask, "r3_volumetric_smoke", &ps_r2_ls_flags, R3FLAG_VOLUMETRIC_SMOKE);
    CMD1(CCC_memory_stats, "render_memory_stats");

    //CMD3(CCC_Mask, "r2_sun_ignore_portals", &ps_r2_ls_flags, R2FLAG_SUN_IGNORE_PORTALS);

    CMD4(CCC_Integer, "r2_mt_calculate",    &ps_r2_mt_calculate, 0, 1);
#if RENDER == R_R4
    CMD4(CCC_Integer, "r2_mt_render",       &ps_r2_mt_render,    0, 1);
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
