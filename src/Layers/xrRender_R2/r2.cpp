#include "stdafx.h"

#include "xrCore/PostProcess/PPInfo.hpp"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/GameFont.h"
#include "xrEngine/PerformanceAlert.hpp"

#include "Layers/xrRender/FBasicVisual.h"
#include "Layers/xrRender/SkeletonCustom.h"
#include "Layers/xrRender/dxWallMarkArray.h"
#include "Layers/xrRender/dxUIShader.h"

#if defined(USE_DX11)
#include "Layers/xrRenderDX11/3DFluid/dx113DFluidManager.h"
#endif

// Declared BEFORE the render namespace opens: a block-scope extern inside it binds to the
// enclosing namespace and dies at link. These live in the engine (xr_ioc_cmd.cpp).
extern ENGINE_API float ps_gamma, ps_brightness, ps_contrast;

namespace xray::render::RENDER_NAMESPACE
{
#if defined(USE_DX11)
void flush_gamesave_screenshots();
#endif

CRender RImplementation;

//////////////////////////////////////////////////////////////////////////
class CGlow : public IRender_Glow
{
public:
    bool bActive;

public:
    CGlow() : bActive(false) {}
    virtual void set_active(bool b) { bActive = b; }
    virtual bool get_active() { return bActive; }
    virtual void set_position(const Fvector& P) {}
    virtual void set_direction(const Fvector& D) {}
    virtual void set_radius(float R) {}
    virtual void set_texture(LPCSTR name) {}
    virtual void set_color(const Fcolor& C) {}
    virtual void set_color(float r, float g, float b) {}
};

float r_dtex_range = 50.f;
//////////////////////////////////////////////////////////////////////////
ShaderElement* CRender::rimp_select_sh_dynamic(dxRender_Visual* pVisual, float cdist_sq, u32 phase)
{
    int id = SE_R2_SHADOW;
    if (CRender::PHASE_NORMAL == phase)
    {
        id = ((_sqrt(cdist_sq) - pVisual->vis.sphere.R) < r_dtex_range) ? SE_R2_NORMAL_HQ : SE_R2_NORMAL_LQ;
    }
    return pVisual->shader->E[id]._get();
}
//////////////////////////////////////////////////////////////////////////
ShaderElement* CRender::rimp_select_sh_static(dxRender_Visual* pVisual, float cdist_sq, u32 phase)
{
    if (!pVisual->shader)
        return nullptr;
    int id = SE_R2_SHADOW;
    if (CRender::PHASE_NORMAL == phase)
    {
        id = ((_sqrt(cdist_sq) - pVisual->vis.sphere.R) < r_dtex_range) ? SE_R2_NORMAL_HQ : SE_R2_NORMAL_LQ;
    }
    return pVisual->shader->E[id]._get();
}
static class cl_parallax : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        float h = ps_r2_df_parallax_h;
        cmd_list.set_c(C, h, -h / 2.f, 1.f / r_dtex_range, 1.f / r_dtex_range);
    }
} binder_parallax;

#if defined(USE_DX11)
static class cl_LOD : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.LOD.set_LOD(C); }
} binder_LOD;
#endif

static class cl_pos_decompress_params : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
#if defined(USE_DX11)
        const float VertTan = -1.0f * tanf(deg2rad(Device.fFOV / 2.0f));
        const float HorzTan = -VertTan / Device.fASPECT;
#elif defined(USE_OGL)
        const float VertTan = tanf(deg2rad(Device.fFOV / 2.0f));
        const float HorzTan = VertTan / Device.fASPECT;
#else
#   error No graphics API selected or enabled!
#endif
        cmd_list.set_c(
            C, HorzTan, VertTan, (2.0f * HorzTan) / (float)Device.dwWidth, (2.0f * VertTan) / (float)Device.dwHeight);
    }
} binder_pos_decompress_params;

static class cl_pos_decompress_params2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, (float)Device.dwWidth, (float)Device.dwHeight, 1.0f / (float)Device.dwWidth,
            1.0f / (float)Device.dwHeight);
    }
} binder_pos_decompress_params2;

static class cl_water_intensity : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment().CurrentEnv;
        const float fValue = env.m_fWaterIntensity;
        cmd_list.set_c(C, fValue, fValue, fValue, 0.f);
    }
} binder_water_intensity;

static class cl_sun_shafts_intensity : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const auto& env = g_pGamePersistent->Environment().CurrentEnv;
        const float fValue = env.m_fSunShaftsIntensity + ps_r2_sun_shafts_value;
        cmd_list.set_c(C, fValue, fValue, fValue, 0.f);
    }
} binder_sun_shafts_intensity;

// Screen-space contact shadows: x = strength (0 = off), y = ray length, z = thickness, w = steps.
// Consumed by da_sss.h from the near sun pass.
static class cl_da_sss : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__sss, ps_r__sss_len, ps_r__sss_thick, ps_r__sss_steps);
    }
} binder_da_sss;

// Rain state for surface response: x = rain right now (water ripples scale by it),
// y = accumulated ground wetness, z = puddle share at full wetness, w = debug mode.
//
// The accumulator runs ONCE PER FRAME (frame marker), not per binding: the binder is called
// per pass and per object, and without the marker wetness would grow at a rate depending on
// how much geometry is in frame. Rain strength sets the SPEED of soaking, not its ceiling:
// DA weather rains at 0.1-0.3 most of the time, and an intensity-capped accumulator would
// never form a puddle - in life a drizzle wets the ground SLOWER, not less. The 0.25 floor
// keeps the faintest drizzle from taking days: it fills in four buildup periods.
static class cl_rain_params : public R_constant_setup
{
    u32 marker{};
    float wetness{};
    Fvector4 result{};

    void setup(CBackend& cmd_list, R_constant* C) override
    {
        if (marker != Device.dwFrame)
        {
            marker = Device.dwFrame;

            // x stays the RAW rain density regardless of the puddle master: the water
            // shader scales its rain ripples by it, and the Minimum preset turning
            // puddles off must not also becalm the lakes.
            const float rain = g_pGamePersistent ? g_pGamePersistent->Environment().CurrentEnv.rain_density : 0.f;
            const float dbg = float(ps_r__puddles_debug);
            const float rain_for_wetness = ps_r__puddles ? rain : 0.f;

            if (ps_r__puddles && ps_r__puddles_force > 0.f)
            {
                // Hand-set wetness for checking looks - no minutes of waiting for buildup.
                wetness = ps_r__puddles_force;
            }
            else
            {
                const float dt = Device.fTimeDelta;
                if (rain_for_wetness > 0.02f)
                {
                    const float speed = (0.25f + 0.75f * rain_for_wetness) / _max(ps_r__puddles_buildup, EPS_S);
                    wetness += dt * speed;
                }
                else
                {
                    // Dries the same way, only as many times slower as the knob says.
                    wetness -= dt / _max(ps_r__puddles_buildup * ps_r__puddles_dry, EPS_S);
                }
                clamp(wetness, 0.f, 1.f);
            }
            result.set(rain, wetness, ps_r__puddles_size, dbg);
        }
        cmd_list.set_c(C, result);
    }
} binder_rain_params;

// Puddle look: gloss, darkening factor, wet-ground gloss, ripple strength.
static class cl_da_puddle_look : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__puddles_gloss, ps_r__puddles_dark, ps_r__puddles_damp, ps_r__puddles_ripple);
    }
} binder_da_puddle_look;

// Puddle look 2: draw distance, edge hardness, rim strength, G-buffer half switch.
static class cl_da_puddle_look2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, float(ps_r__puddles_dist), ps_r__puddles_edge, ps_r__puddles_rim,
            float(ps_r__puddles_gbuf));
    }
} binder_da_puddle_look2;

// Puddle look 3: rim width; three slots left for the future.
static class cl_da_puddle_look3 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__puddles_rim_width, 0.f, 0.f, 0.f);
    }
} binder_da_puddle_look3;


// Haze: sky-coloured fog and the height layer, consumed by combine_1.ps. The value goes RAW:
// the shader branches on a 0.001 threshold and applies its own scale inside, so dividing here
// would put the whole working range under the branch's own cutoff. The master multiplies both
// halves; zero means "do nothing", which is the invariant every haze branch is written around -
// gating them with a define would double the shader cache instead.
static class cl_da_fog : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const float k = ps_r__fog;
        cmd_list.set_c(C, ps_r__fog_sky * k, ps_r__fog_sky_mip, ps_r__fog_height * k,
            ps_r__fog_height_falloff);
    }
} binder_da_fog;

// Second haze constant: density ceiling, layer reference altitude, horizon flattening.
static class cl_da_fog2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__fog_max, ps_r__fog_height_base, ps_r__fog_sky_flat, 0.f);
    }
} binder_da_fog2;

// Tonemap tinting: y = white point, z = luminance-tonemap share, w = late-desaturation power.
// Consumed by tonemap() in common_functions.h; a zero constant reproduces stock exactly.
static class cl_da_tonemap_params : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, 0.f, ps_r__tonemap_white, ps_r__tonemap_hue, ps_r__tonemap_desat);
    }
} binder_da_tonemap_params;

// Gamma/brightness/contrast for the final combine, packed the way CGammaControl::GenLUT
// consumes them. w flags "the hardware ramp is not in charge" - anything but exclusive
// fullscreen - which is when the shader has to apply the sliders itself.
static class cl_da_gamma : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        const bool rampDead = psDeviceMode.WindowStyle != rsFullscreen;
        cmd_list.set_c(C, 1.f / _max(::ps_gamma, 0.1f), ::ps_brightness * 0.5f, ::ps_contrast * 0.5f,
            rampDead ? 1.f : 0.f);
    }
} binder_da_gamma;

// Distant-vegetation billboard shading fix (lod.ps): hemi share, saturation, brightness.
static class cl_da_lod_tune : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__lod_hemi, ps_r__lod_sat, ps_r__lod_bright, 0.f);
    }
} binder_da_lod_tune;

// Steep parallax: fade start/end, depth, self-shadow strength.
static class cl_da_parallax : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__parallax_start, ps_r__parallax_stop, ps_r__parallax_depth,
            ps_r__parallax_shadow);
    }
} binder_da_parallax;

// Steep parallax: search steps max/min, sun-ray steps, debug mode.
static class cl_da_parallax2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, float(ps_r__parallax_samples), float(ps_r__parallax_samples_min),
            float(ps_r__parallax_shadow_samples), float(ps_r__parallax_debug));
    }
} binder_da_parallax2;

// Far ground variation: strength, 1/coarse step, fade start, fade end (end kept above start).
static class cl_da_macro_var : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__macro_var, 1.f / _max(ps_r__macro_var_scale, 1.f),
            ps_r__macro_var_start, _max(ps_r__macro_var_end, ps_r__macro_var_start + 1.f));
    }
} binder_da_macro_var;

// Far hue tint, macro relief, height splatting, mask jitter.
static class cl_da_macro_var2 : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__macro_tint, ps_r__macro_relief, ps_r__terrain_blend, ps_r__mask_jitter);
    }
} binder_da_macro_var2;

// Foliage albedo/gloss knobs for deffer_base_aref_*. Gloss travels SHIFTED BY ONE: the
// "zero constant = stock picture" invariant (archive shader against a new DLL) would
// otherwise eat the meaningful zero of the knob - so 0 = unbound, 1 = knob at zero.
// .y is reserved (their translucency bend needs the TAA pipeline we do not ship).
static class cl_da_foliage : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        cmd_list.set_c(C, ps_r__foliage_gloss + 1.f, 0.f, ps_r__foliage_vibrance, ps_r__foliage_debleach);
    }
} binder_da_foliage;

static class cl_alpha_ref : public R_constant_setup
{
    void setup(CBackend& cmd_list, R_constant* C) override
    {
        // TODO: OGL: Implement AlphaRef.
#   if defined(USE_DX11)
        cmd_list.StateManager.BindAlphaRef(C);
#   endif
    }
} binder_alpha_ref;

// Defined in ResourceManager.cpp
IReader* open_shader(pcstr shader);

// Check shadow cascades type (old SOC/CS or new COP)
static bool must_enable_old_cascades()
{
    bool oldCascades = false;
#if RENDER != R_R1
    {
        IReader* accumSunNear = open_shader("accum_sun_near.ps");
        R_ASSERT3(accumSunNear, "Can't open shader", "accum_sun_near.ps");
        do
        {
            xr_string str(static_cast<cpcstr>(accumSunNear->pointer()), accumSunNear->length());

            pcstr begin = strstr(str.c_str(), "float4");
            if (!begin)
                break;

            begin = strstr(begin, "main");
            if (!begin)
                break;

            cpcstr end = strstr(begin, "SV_Target");
            if (!end)
                break;

            str.assign(begin, end);
            cpcstr ptr = str.data();

            if (strstr(ptr, "v2p_TL2uv"))
            {
                oldCascades = true;
            }
            else if (strstr(ptr, "v2p_volume"))
            {
                oldCascades = false;
            }
        } while (false);
        FS.r_close(accumSunNear);
    }
#endif
    return oldCascades;
}

// Returns true if compute shaders for HDAO Ultra exist
[[maybe_unused]] static bool ssao_hdao_cs_shaders_exist()
{
    IReader* hdao_cs      = open_shader("ssao_hdao.cs");
    IReader* hdao_cs_msaa = open_shader("ssao_hdao_msaa.cs");

    const bool exist      = hdao_cs && hdao_cs_msaa;

    FS.r_close(hdao_cs);
    FS.r_close(hdao_cs_msaa);

    return exist;
}

//////////////////////////////////////////////////////////////////////////
// Just two static storage
void CRender::create()
{
    ZoneScoped;

    Device.seqFrame.Add(this, REG_PRIORITY_HIGH + 0x12345678);

    // The preset-derived switches never persist in user.ltx; re-derive them from the
    // replayed preset so a value lost mid-session cannot survive into the next one.
    xrRender_sync_preset_derived();

    m_skinning = -1;
    m_MSAASample = -1;

    // hardware
    o.mrt = (HW.Caps.raster.dwMRT_count >= 3);
    o.mrtmixdepth = (HW.Caps.raster.b_MRT_mixdepth);

    // Check for NULL render target support
    o.nullrt = false;

    /*
    if (o.nullrt)		{
    Msg				("* NULLRT supported and used");
    };
    */
    if (o.nullrt)
    {
        Msg("* NULLRT supported");

        //.	    _tzset			();
        //.		??? _strdate	( date, 128 );	???
        //.		??? if (date < 22-march-07)
        if (0)
        {
            u32 device_id = HW.Caps.id_device;
            bool disable_nullrt = false;
            switch (device_id)
            {
            case 0x190:
            case 0x191:
            case 0x192:
            case 0x193:
            case 0x194:
            case 0x197:
            case 0x19D:
            case 0x19E:
            {
                disable_nullrt = true; // G80
                break;
            }
            case 0x400:
            case 0x401:
            case 0x402:
            case 0x403:
            case 0x404:
            case 0x405:
            case 0x40E:
            case 0x40F:
            {
                disable_nullrt = true; // G84
                break;
            }
            case 0x420:
            case 0x421:
            case 0x422:
            case 0x423:
            case 0x424:
            case 0x42D:
            case 0x42E:
            case 0x42F:
            {
                disable_nullrt = true; // G86
                break;
            }
            }
            if (disable_nullrt)
                o.nullrt = false;
        }
        if (o.nullrt)
            Msg("* ...and used");
    }

    // SMAP / DST
    o.HW_smap_FETCH4 = FALSE;
    o.HW_smap = true;
    o.HW_smap_PCF = o.HW_smap;

    if (o.HW_smap)
    {
#if defined(USE_DX11)
        //	For ATI it's much faster on DX11 to use D32F format
        if (HW.Caps.id_vendor == 0x1002)
            o.HW_smap_FORMAT = D3DFMT_D32F_LOCKABLE;
        else
#endif
        {
            o.HW_smap_FORMAT = D3DFMT_D24X8;
        }
        Msg("* HWDST/PCF supported and used");
    }

    o.fp16_filter = true;
    o.fp16_blend = true;

    // emulate ATI-R4xx series
    if (strstr(Core.Params, "-r4xx"))
    {
        o.mrtmixdepth = FALSE;
        o.HW_smap = FALSE;
        o.HW_smap_PCF = FALSE;
        o.fp16_filter = FALSE;
        o.fp16_blend = FALSE;
    }

    VERIFY2(o.mrt && (HW.Caps.raster.dwInstructions >= 256), "Hardware doesn't meet minimum feature-level");
    if (o.mrtmixdepth)
        o.albedo_wo = FALSE;
    else if (o.fp16_blend)
        o.albedo_wo = FALSE;
    else
        o.albedo_wo = TRUE;

    // nvstencil on NV40 and up
    // nvstencil should be enabled only for GF 6xxx and GF 7xxx
    // if hardware support early stencil (>= GF 8xxx) stencil reset trick only
    // slows down.
    o.nvstencil = FALSE;
    if (strstr(Core.Params, "-nonvs"))
        o.nvstencil = FALSE;

    // nv-dbt
    o.nvdbt = false;

    if (o.nvdbt)
        Msg("* NV-DBT supported and used");

    o.ffp = false;

    // options (smap-pool-size)
    if (strstr(Core.Params, "-smap1024"))
        o.smapsize = 1024;
    else if (strstr(Core.Params, "-smap1536"))
        o.smapsize = 1536;
    else if (strstr(Core.Params, "-smap2048"))
        o.smapsize = 2048;
    else if (strstr(Core.Params, "-smap2560"))
        o.smapsize = 2560;
    else if (strstr(Core.Params, "-smap3072"))
        o.smapsize = 3072;
    else if (strstr(Core.Params, "-smap4096"))
        o.smapsize = 4096;
    else if (strstr(Core.Params, "-smap8192"))
        o.smapsize = 8192;
    else
        o.smapsize = ps_r2_smapsize;

    // gloss
    cpcstr g = strstr(Core.Params, "-gloss ");
    o.forcegloss = g ? TRUE : FALSE;
    if (g)
    {
        o.forcegloss_v = float(atoi(g + xr_strlen("-gloss "))) / 255.f;
    }

    // options
    o.bug = (strstr(Core.Params, "-bug")) ? TRUE : FALSE;
    o.sunfilter = (strstr(Core.Params, "-sunfilter")) ? TRUE : FALSE;
    //.	o.sunstatic			= (strstr(Core.Params,"-sunstatic"))?	TRUE	:FALSE	;
    o.sunstatic = ps_r2_sun_static;
    o.advancedpp = ps_r2_advanced_pp;
#if defined(USE_DX11)
    o.volumetricfog = ps_r2_ls_flags.test(R3FLAG_VOLUMETRIC_SMOKE);
#elif defined(USE_OGL)
    // TODO: OGL: temporary disabled, need to fix it
    o.volumetricfog = false;
#endif
    o.sjitter = (strstr(Core.Params, "-sjitter")) ? TRUE : FALSE;
    o.depth16 = (strstr(Core.Params, "-depth16")) ? TRUE : FALSE;
    o.noshadows = (strstr(Core.Params, "-noshadows")) ? TRUE : FALSE;
    o.Tshadows = (strstr(Core.Params, "-tsh")) ? TRUE : FALSE;
    o.oldshadowcascades = must_enable_old_cascades() || ps_r2_ls_flags_ext.test(R2FLAGEXT_SUN_OLD);
    o.mblur = (strstr(Core.Params, "-mblur")) ? TRUE : FALSE;
    o.distortion_enabled = (strstr(Core.Params, "-nodistort")) ? FALSE : TRUE;
    o.distortion = o.distortion_enabled;
    o.disasm = (strstr(Core.Params, "-disasm")) ? TRUE : FALSE;
    o.forceskinw = (strstr(Core.Params, "-skinw")) ? TRUE : FALSE;

    o.ssao_blur_on = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_BLUR) && (ps_r_ssao != 0);
    o.ssao_opt_data = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_OPT_DATA) && (ps_r_ssao != 0);
    o.ssao_half_data = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HALF_DATA) && o.ssao_opt_data && (ps_r_ssao != 0);
#if defined(USE_DX11)
    o.ssao_hdao = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HDAO) && (ps_r_ssao != 0);
    // The ultra path dispatches a compute shader that writes rt_ssao_temp through a UAV, and the
    // UAV bind flag is only granted at feature level 11.0 (dx11SH_RT). A 10.x device still reports
    // ComputeShadersSupported for CS 4.x, so without this guard phase_hdao would bind a null UAV
    // and dispatch against it - undefined for the driver, and the device is lost mid-frame.
    o.ssao_ultra = HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0 && HW.ComputeShadersSupported &&
        ssao_hdao_cs_shaders_exist();
    o.ssao_hbao = !o.ssao_hdao && ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HBAO) && (ps_r_ssao != 0);
#elif defined(USE_OGL)
    // TODO: OGL: temporary disabled HBAO/HDAO, need to fix it
    o.ssao_hbao = false;
    o.ssao_hdao = false;
#else
#   error No graphics API selected or enabled!
#endif

    //	TODO: fix hbao shader to allow to perform per-subsample effect!
    if (o.ssao_hbao && HW.Caps.id_vendor == 0x1002)
        o.hbao_vectorized = true;
    else
        o.hbao_vectorized = false;

#if defined(USE_DX11)
    o.dx11_sm4_1 = ps_r2_ls_flags.test((u32)R3FLAG_USE_DX10_1);
    o.dx11_sm4_1 = o.dx11_sm4_1 && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1);
#elif defined(USE_OGL)
    o.dx11_sm4_1 = true;
#else
#   error No graphics API selected or enabled!
#endif

    //	MSAA option dependencies
#if defined(USE_DX11)
    o.msaa = !!ps_r3_msaa;
    o.msaa_samples = (1 << ps_r3_msaa);

    o.msaa_opt = ps_r2_ls_flags.test(R3FLAG_MSAA_OPT);
    o.msaa_opt = o.msaa_opt && o.msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1) ||
        o.msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0);

    // o.msaa_hybrid	= ps_r2_ls_flags.test(R3FLAG_MSAA_HYBRID);
    o.msaa_hybrid = ps_r2_ls_flags.test((u32)R3FLAG_USE_DX10_1);
    o.msaa_hybrid &= !o.msaa_opt && o.msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1);
#elif defined(USE_OGL)
    // TODO: OGL: temporary disabled, need to fix it
    o.msaa = false;
    o.msaa_samples = 0;
    o.msaa_opt = o.msaa;
    o.msaa_hybrid = false;
#else
#   error No graphics API selected or enabled!
#endif
    //	Allow alpha test MSAA for DX10.0

    // o.msaa_alphatest= ps_r2_ls_flags.test((u32)R3FLAG_MSAA_ALPHATEST);
    // o.msaa_alphatest= o.msaa_alphatest && o.msaa;

    // o.msaa_alphatest_atoc= (o.msaa_alphatest && !o.msaa_opt && !o.msaa_hybrid);

    o.msaa_alphatest = 0;
    if (o.msaa)
    {
        if (o.msaa_opt || o.msaa_hybrid)
        {
            if (ps_r3_msaa_atest == 1)
                o.msaa_alphatest = MSAA_ATEST_DX10_1_ATOC;
            else if (ps_r3_msaa_atest == 2)
                o.msaa_alphatest = MSAA_ATEST_DX10_1_NATIVE;
        }
        else
        {
            if (ps_r3_msaa_atest)
                o.msaa_alphatest = MSAA_ATEST_DX10_0_ATOC;
        }
    }

    o.gbuffer_opt = ps_r2_ls_flags.test(R3FLAG_GBUFFER_OPT);

    o.minmax_sm = ps_r3_minmax_sm;
    o.minmax_sm_screenarea_threshold = 1600 * 1200;

#if defined(USE_DX11)
    o.tessellation =
        HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0 && ps_r2_ls_flags_ext.test(R2FLAGEXT_ENABLE_TESSELLATION);
    o.support_rt_arrays = true;
#else
    o.support_rt_arrays = false;
#endif

    if (o.minmax_sm == MMSM_AUTODETECT)
    {
        o.minmax_sm = MMSM_OFF;

        //	AMD device
        if (HW.Caps.id_vendor == 0x1002)
        {
            if (ps_r_sun_quality >= 3)
                o.minmax_sm = MMSM_AUTO;
            else if (ps_r_sun_shafts >= 2)
            {
                o.minmax_sm = MMSM_AUTODETECT;
                //	Check resolution in runtime in use_minmax_sm_this_frame
                o.minmax_sm_screenarea_threshold = 1600 * 1200;
            }
        }

        //	NVidia boards
        if (HW.Caps.id_vendor == 0x10DE)
        {
            if (ps_r_sun_shafts >= 2)
            {
                o.minmax_sm = MMSM_AUTODETECT;
                //	Check resolution in runtime in use_minmax_sm_this_frame
                o.minmax_sm_screenarea_threshold = 1280 * 1024;
            }
        }
    }

    // constants
    Resources->RegisterConstantSetup("parallax", &binder_parallax);
    Resources->RegisterConstantSetup("water_intensity", &binder_water_intensity);
    Resources->RegisterConstantSetup("sun_shafts_intensity", &binder_sun_shafts_intensity);
    Resources->RegisterConstantSetup("da_sss", &binder_da_sss);
    Resources->RegisterConstantSetup("rain_params", &binder_rain_params);
    Resources->RegisterConstantSetup("da_fog", &binder_da_fog);
    Resources->RegisterConstantSetup("da_fog2", &binder_da_fog2);
    Resources->RegisterConstantSetup("da_tonemap_params", &binder_da_tonemap_params);
    Resources->RegisterConstantSetup("da_gamma", &binder_da_gamma);
    Resources->RegisterConstantSetup("da_lod_tune", &binder_da_lod_tune);
    Resources->RegisterConstantSetup("da_foliage", &binder_da_foliage);
    Resources->RegisterConstantSetup("da_macro_var", &binder_da_macro_var);
    Resources->RegisterConstantSetup("da_macro_var2", &binder_da_macro_var2);
    Resources->RegisterConstantSetup("da_parallax", &binder_da_parallax);
    Resources->RegisterConstantSetup("da_parallax2", &binder_da_parallax2);
    Resources->RegisterConstantSetup("da_puddle_look", &binder_da_puddle_look);
    Resources->RegisterConstantSetup("da_puddle_look2", &binder_da_puddle_look2);
    Resources->RegisterConstantSetup("da_puddle_look3", &binder_da_puddle_look3);
    Resources->RegisterConstantSetup("pos_decompression_params", &binder_pos_decompress_params);
    Resources->RegisterConstantSetup("pos_decompression_params2", &binder_pos_decompress_params2);
    Resources->RegisterConstantSetup("m_AlphaRef", &binder_alpha_ref);
#if defined(USE_DX11)
    Resources->RegisterConstantSetup("triLOD", &binder_LOD);
#endif

    Target = xr_new<CRenderTarget>(); // Main target

    Models = xr_new<CModelPool>();
    PSLibrary.OnCreate();
    HWOCC.occq_create(occq_size);

    rmNormal(RCache);
    q_sync_point.Create();

    //	TODO: OGL: Implement FluidManager.
#if defined(USE_DX11)
    FluidManager.Initialize(70, 70, 70);
    //	FluidManager.Initialize( 100, 100, 100 );
    FluidManager.SetScreenSize(Device.dwWidth, Device.dwHeight);
#endif
}

void CRender::destroy()
{
#if defined(USE_DX11)
    flush_gamesave_screenshots();
    FluidManager.Destroy();
#endif
    q_sync_point.Destroy();
    HWOCC.occq_destroy();
    xr_delete(Models);
    xr_delete(Target);
    PSLibrary.OnDestroy();
    Device.seqFrame.Remove(this);
}

void CRender::reset_begin()
{
    ZoneScoped;
    // Wait for tasks to be done
    r_main.sync();
    r_sun.sync();
    r_sun_old.sync();
#if RENDER != R_R2
    r_rain.sync();
#endif

    Resources->reset_begin();

    // Update incremental shadowmap-visibility solver
    // BUG-ID: 10646
    {
        u32 it = 0;
        for (it = 0; it < Lights_LastFrame.size(); it++)
        {
            if (0 == Lights_LastFrame[it])
                continue;
            try
            {
                for (int id = 0; id < R__NUM_PARALLEL_CONTEXTS; ++id)
                    Lights_LastFrame[it]->svis[id].resetoccq();
            }
            catch (...)
            {
                Msg("! Failed to flush-OCCq on light [%d] %X", it, *(u32*)(&Lights_LastFrame[it]));
            }
        }
        Lights_LastFrame.clear();
    }

    //AVO: let's reload details while changed details options on vid_restart
    if (b_loaded && (dm_current_size != dm_size ||
        !fsimilar(ps_r__Detail_density, ps_current_detail_density) ||
        !fsimilar(ps_r__Detail_height, ps_current_detail_height)))
    {
        if (Details)
            Details->Unload();
        xr_delete(Details);
    }
    //-AVO

    xr_delete(Target);
    HWOCC.occq_destroy();
    q_sync_point.Destroy();
}

void CRender::reset_end()
{
    ZoneScoped;
    q_sync_point.Create();
    HWOCC.occq_create(occq_size);

    Target = xr_new<CRenderTarget>();

    //AVO: let's reload details while changed details options on vid_restart
    if (b_loaded && (dm_current_size != dm_size ||
        !fsimilar(ps_r__Detail_density, ps_current_detail_density) ||
        !fsimilar(ps_r__Detail_height, ps_current_detail_height)))
    {
        Details = xr_new<CDetailManager>();
        Details->Load();
    }
    //-AVO

#if defined(USE_DX11)
    FluidManager.SetScreenSize(Device.dwWidth, Device.dwHeight);
#endif

    cleanup_contexts();

    // Set this flag true to skip the first render frame,
    // that some data is not ready in the first frame (for example device camera position)
    m_bFirstFrameAfterReset = true;
}

void CRender::OnCameraUpdated()
{
    ZoneScoped;

    // Frustum
    ViewBase.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);

    if (g_pGamePersistent->MainMenuActiveOrLevelNotExist())
        return;

    ProcessHOMTask = &HOM.DispatchMTRender();
    if (Details)
    {
        Details->UpdateRenderState();
        Details->DispatchMTCalc();
    }
}

void CRender::OnFrame()
{
    ZoneScoped;

#if defined(USE_DX11)
    ProcessGamesaveScreenshots();
#endif

    Models->DeleteQueue();

    if (g_pGamePersistent->MainMenuActiveOrLevelNotExist())
        return;
}

#ifdef USE_OGL
IRender::RenderContext CRender::GetCurrentContext() const
{
    return HW.GetCurrentContext();
}

void CRender::MakeContextCurrent(RenderContext context)
{
    R_ASSERT3(HW.MakeContextCurrent(context) == 0,
        "Failed to switch OpenGL context", SDL_GetError());
}
#endif

// Implementation
IRender_ObjectSpecific* CRender::ros_create(IRenderable* parent) { return xr_new<CROS_impl>(); }
void CRender::ros_destroy(IRender_ObjectSpecific*& p) { xr_delete(p); }
IRenderVisual* CRender::model_Create(LPCSTR name, IReader* data) { return Models->Create(name, data); }
IRenderVisual* CRender::model_CreateChild(LPCSTR name, IReader* data) { return Models->CreateChild(name, data); }
IRenderVisual* CRender::model_Duplicate(IRenderVisual* V) { return Models->Instance_Duplicate((dxRender_Visual*)V); }

void CRender::model_Delete(IRenderVisual*& V, bool bDiscard)
{
    dxRender_Visual* pVisual = (dxRender_Visual*)V;
    Models->Delete(pVisual, bDiscard);
    V = nullptr;
}

IRender_DetailModel* CRender::model_CreateDM(IReader* F)
{
    CDetail* D = xr_new<CDetail>();
    D->Load(F);
    return D;
}

void CRender::model_Delete(IRender_DetailModel*& F)
{
    if (F)
    {
        CDetail* D = (CDetail*)F;
        D->Unload();
        xr_delete(D);
        F = nullptr;
    }
}

IRenderVisual* CRender::model_CreatePE(LPCSTR name)
{
    PS::CPEDef* SE = PSLibrary.FindPED(name);
    R_ASSERT3(SE, "Particle effect doesn't exist", name);
    return Models->CreatePE(SE);
}

IRenderVisual* CRender::model_CreateParticles(LPCSTR name)
{
    PS::CPEDef* SE = PSLibrary.FindPED(name);
    if (SE)
        return Models->CreatePE(SE);

    PS::CPGDef* SG = PSLibrary.FindPGD(name);
    R_ASSERT3(SG, "Particle effect or group doesn't exist", name);
    return Models->CreatePG(SG);
}
void CRender::models_Prefetch() { Models->Prefetch(); }
void CRender::models_Clear(bool b_complete) { Models->ClearPool(b_complete); }
ref_shader CRender::getShader(int id)
{
    VERIFY(id < int(Shaders.size()));
    return Shaders[id];
}
IRenderVisual* CRender::getVisual(int id)
{
    VERIFY(id < int(Visuals.size()));
    return Visuals[id];
}

VertexElement* CRender::getVB_Format(int id, bool alternative)
{
    if (alternative)
    {
        VERIFY(id < int(xDC.size()));
        return xDC[id].begin();
    }
    VERIFY(id < int(nDC.size()));
    return nDC[id].begin();
}

VertexStagingBuffer* CRender::getVB(int id, bool alternative)
{
    if (alternative)
    {
        VERIFY(id<int(xVB.size()));
        return &xVB[id];
    }
    VERIFY(id < int(nVB.size()));
    return &nVB[id];
}

IndexStagingBuffer* CRender::getIB(int id, bool alternative)
{
    if (alternative)
    {
        VERIFY(id < int(xIB.size()));
        return &xIB[id];
    }
    VERIFY(id < int(nIB.size()));
    return &nIB[id];
}

FSlideWindowItem* CRender::getSWI(int id)
{
    VERIFY(id < int(SWIs.size()));
    return &SWIs[id];
}

IRender_Light* CRender::light_create() { return Lights.Create(); }
IRender_Glow* CRender::glow_create() { return xr_new<CGlow>(); }
bool CRender::occ_visible(vis_data& P) { return HOM.visible(P); }
bool CRender::occ_visible(sPoly& P) { return HOM.visible(P); }
bool CRender::occ_visible(Fbox& P) { return HOM.visible(P); }
void CRender::add_Visual(u32 context_id, IRenderable* root, IRenderVisual* V, Fmatrix& m)
{
    // TODO: this whole function should be replaced by a list of renderables+xforms returned from `renderable_Render` call
    auto& dsgraph = get_context(context_id);
    dsgraph.add_leafs_dynamic(root, (dxRender_Visual*)V, m);
}
void CRender::add_StaticWallmark(ref_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* verts)
{
    VERIFY2(T, "Invalid static wallmark triangle");
    if (T->suppress_wm)
        return;
    VERIFY2(_valid(P) && _valid(s) && verts && (s > EPS_L), "Invalid static wallmark params");
    if (!Wallmarks)
        return;
    Wallmarks->AddStaticWallmark(T, verts, P, &*S, s);
}

void CRender::add_StaticWallmark(IWallMarkArray* pArray, const Fvector& P, float s, CDB::TRI* T, Fvector* V)
{
    dxWallMarkArray* pWMA = (dxWallMarkArray*)pArray;
    ref_shader* pShader = pWMA->dxGenerateWallmark();
    if (pShader)
        add_StaticWallmark(*pShader, P, s, T, V);
}

void CRender::add_StaticWallmark(const wm_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* V)
{
    dxUIShader* pShader = (dxUIShader*)&*S;
    add_StaticWallmark(pShader->hShader, P, s, T, V);
}

void CRender::clear_static_wallmarks()
{
    if (Wallmarks)
        Wallmarks->clear();
}
void CRender::add_SkeletonWallmark(intrusive_ptr<CSkeletonWallmark> wm)
{
    if (Wallmarks)
        Wallmarks->AddSkeletonWallmark(wm);
}
void CRender::add_SkeletonWallmark(
    const Fmatrix* xf, CKinematics* obj, ref_shader& sh, const Fvector& start, const Fvector& dir, float size)
{
    if (!Wallmarks)
        return;
    Wallmarks->AddSkeletonWallmark(xf, obj, sh, start, dir, size);
}
void CRender::add_SkeletonWallmark(
    const Fmatrix* xf, IKinematics* obj, IWallMarkArray* pArray, const Fvector& start, const Fvector& dir, float size)
{
    dxWallMarkArray* pWMA = (dxWallMarkArray*)pArray;
    ref_shader* pShader = pWMA->dxGenerateWallmark();
    if (pShader)
        add_SkeletonWallmark(xf, (CKinematics*)obj, *pShader, start, dir, size);
}

void CRender::rmNear(CBackend& cmd_list)
{
    const D3D_VIEWPORT viewport = {
        0, 0, Target->get_width(cmd_list), Target->get_height(cmd_list), 0.f, r2_hud_depth_limit };
    cmd_list.SetViewport(viewport);
}

void CRender::rmFar(CBackend& cmd_list)
{
    const D3D_VIEWPORT viewport = { 0, 0, Target->get_width(cmd_list), Target->get_height(cmd_list), 0.99999f, 1.f };
    cmd_list.SetViewport(viewport);
}

void CRender::rmNormal(CBackend& cmd_list)
{
    const D3D_VIEWPORT viewport = { 0, 0, Target->get_width(cmd_list), Target->get_height(cmd_list), 0.f, 1.f };
    cmd_list.SetViewport(viewport);
}

void CRender::SetPostProcessParams(const SPPInfo& ppi)
{
    Target->set_blur(ppi.blur);
    Target->set_gray(ppi.gray);

    Target->set_duality_h(ppi.duality.h);
    Target->set_duality_v(ppi.duality.v);

    Target->set_noise(ppi.noise.intensity);
    Target->set_noise_scale(ppi.noise.grain);
    Target->set_noise_fps(ppi.noise.fps);

    Target->set_color_base(ppi.color_base);
    Target->set_color_gray(ppi.color_gray);
    Target->set_color_add(ppi.color_add);

    Target->set_cm_imfluence(ppi.cm_influence);
    Target->set_cm_interpolate(ppi.cm_interpolate);
    Target->set_cm_textures(ppi.cm_tex1, ppi.cm_tex2);
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
CRender::CRender()
    : Sectors_xrc("render")
{
}

CRender::~CRender() {}

void CRender::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    D3DXRenderBase::DumpStatistics(font, alert);
    Stats.FrameEnd();
    font.OutNext("Lights:");
    font.OutNext("- total:      %u", Stats.l_total);
    font.OutNext("- visible:    %u", Stats.l_visible);
    font.OutNext("- shadowed:   %u", Stats.l_shadowed);
    font.OutNext("- unshadowed: %u", Stats.l_unshadowed);
    font.OutNext("Shadow maps:");
    font.OutNext("- used:       %d", Stats.s_used);
    font.OutNext("- merged:     %d", Stats.s_merged - Stats.s_used);
    font.OutNext("- finalclip:  %d", Stats.s_finalclip);
    u32 ict = Stats.ic_total + Stats.ic_culled;
    font.OutNext("ICULL:        %03.1f", 100.f * f32(Stats.ic_culled) / f32(ict ? ict : 1));
    font.OutNext("- visible:    %u", Stats.ic_total);
    font.OutNext("- culled:     %u", Stats.ic_culled);
    Stats.FrameStart();
    HOM.DumpStatistics(font, alert);
    Sectors_xrc.DumpStatistics(font, alert);
}
} // namespace xray::render::RENDER_NAMESPACE
