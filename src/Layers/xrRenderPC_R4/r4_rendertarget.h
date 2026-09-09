#pragma once

#include "Layers/xrRender/ColorMapManager.h"

namespace xray::render::RENDER_NAMESPACE
{
class light;

//#define DU_SPHERE_NUMVERTEX 92
//#define DU_SPHERE_NUMFACES	180
//#define DU_CONE_NUMVERTEX	18
//#define DU_CONE_NUMFACES	32
//	no less than 2
#define VOLUMETRIC_SLICES 100

class CRenderTarget
{
    u32 dwWidth[R__NUM_CONTEXTS];
    u32 dwHeight[R__NUM_CONTEXTS];
    u32 dwAccumulatorClearMark;

public:
    enum eStencilOptimizeMode
    {
        SO_Light = 0, //	Default
        SO_Combine, //	Default
    };

    u32 dwLightMarkerID;

    IBlender* b_accum_spot{};
    IBlender* b_accum_spot_msaa[8]{};
    IBlender* b_accum_volumetric_msaa[8]{};

#ifdef DEBUG
    struct dbg_line_t
    {
        Fvector P0, P1;
        u32 color;
    };
    xr_vector<std::pair<Fsphere, Fcolor>> dbg_spheres;
    xr_vector<dbg_line_t> dbg_lines;
    xr_vector<Fplane> dbg_planes;
#endif

    // Base targets
    xr_vector<ref_rt> rt_Base;
    ref_rt rt_Base_Depth;

    // MRT-path
    ref_rt rt_Depth; // Z-buffer like - initial depth
    ref_rt rt_MSAADepth; // z-buffer for MSAA deferred shading. If MSAA is disabled, points to rt_Base_Depth so we can reduce branching
    ref_rt rt_Generic_0_r; // MRT generic 0, if MSAA is disabled, just an alias of rt_Generic_0
    ref_rt rt_Generic_1_r; // MRT generic 1, if MSAA is disabled, just an alias of rt_Generic_1
    ref_rt rt_Generic;
    ref_rt rt_Position; // 64bit,	fat	(x,y,z,?)				(eye-space)
    ref_rt rt_Normal; // 64bit,	fat	(x,y,z,hemi)			(eye-space)
    ref_rt rt_Color; // 64/32bit,fat	(r,g,b,specular-gloss)	(or decompressed MET-8-8-8-8)

    //
    ref_rt rt_Accumulator; // 64bit		(r,g,b,specular)
    ref_rt rt_Accumulator_temp; // only for HW which doesn't feature fp16 blend
    ref_rt rt_ui;        // 32bit		(r,g,b,a)				// 3D PDA: UI rasterized to texture ($user$ui)
    ref_rt rt_Generic_0; // 32bit		(r,g,b,a)				// post-process, intermidiate results, etc.
    ref_rt rt_Generic_1; // 32bit		(r,g,b,a)				// post-process, intermidiate results, etc.
    //	Igor: for volumetric lights
    ref_rt rt_Generic_2; // 32bit		(r,g,b,a)				// post-process, intermidiate results, etc.
    ref_rt rt_SSR; // scene grab taken right before the forward pass, sampled by the water SSLR
    // The baked, level-wide water map is deliberately NOT here. Nothing in the frame renders
    // into it, so it is not a render target: it is a CPU bake uploaded as a plain immutable
    // texture under "$user$water_field" by r4_water_field.cpp.
    // Ripple simulation ping-pong ("$user$water_ripple0/1"): R = height now, G = height one
    // step back, over a window of CEnvironment::water_ripple_window metres snapped to whole
    // texels around the camera (da_water_rip). Empty below the tier that runs the field.
    // [band][half]: half 0 is the current step, the name every reader binds; half 1 the step
    // before it, the sim's own input. One band per octave of ring wavelength (see the phase).
    ref_rt rt_WaterRipple[3][2];
    ref_rt rt_SunShaftsMask;
    ref_rt rt_SunShaftsMaskSmoothed;
    ref_rt rt_SunShaftsPass0;
    // SMAA working targets (edge mask / blend weights), always single-sampled
    ref_rt rt_smaa_edges;
    ref_rt rt_smaa_blend;
    // Camera-TAA history: the previous resolved LDR frame, 10-bit. rt_taa_resolve is the
    // second target of the resolve pass (the history cannot be read and written at once)
    // and is copied into rt_taa_history right after it - same format, so CopyResource works.
    ref_rt rt_taa_history;
    ref_rt rt_taa_resolve;
    ref_rt rt_Bloom_1; // 32bit, dim/4	(r,g,b,?)
    ref_rt rt_Bloom_2; // 32bit, dim/4	(r,g,b,?)
    ref_rt rt_LUM_64; // 64bit, 64x64,	log-average in all components
    ref_rt rt_LUM_8; // 64bit, 8x8,		log-average in all components

    ref_rt rt_LUM_pool[CHWCaps::MAX_GPUS * 2]; // 1xfp32,1x1,		exp-result -> scaler
    ref_texture t_LUM_src; // source
    ref_texture t_LUM_dest; // destination & usage for current frame

    // smap
    ref_rt rt_smap_surf; // 32bit,		color
    ref_rt rt_smap_depth; // 24(32) bit,	depth
    ref_rt rt_smap_rain;
    ref_rt rt_smap_depth_minmax; //	is used for min/max sm
    ref_rt rt_smap_hud; // first-person hands and item, for self-shadowing only

    //	Igor: for async screenshots
    ID3DTexture2D* t_ss_async; // 32bit		(r,g,b,a) is situated in the system memory

    // Textures
    ref_texture t_material;
    ref_texture t_noise[TEX_jitter_count];
    ref_texture t_noise_mipped;
    // SMAA lookup tables baked into the binary (smaa_luts.h)
    ref_texture t_smaa_area;
    ref_texture t_smaa_search;
    // GTAO blue-noise tile (gtao_noise.h)
    ref_texture t_blue_noise;

private:
    // OCCq
    ref_shader s_occq;

    // Accum
    ref_shader s_accum_mask;
    ref_shader s_accum_mask_msaa[8];
    ref_shader s_accum_direct;
    ref_shader s_accum_direct_msaa[8];
    ref_shader s_accum_direct_volumetric;
    ref_shader s_accum_direct_volumetric_msaa[8];
    ref_shader s_accum_direct_volumetric_minmax;
    ref_shader s_accum_point;
    ref_shader s_accum_point_msaa[8];
    ref_shader s_accum_spot;
    ref_shader s_accum_spot_msaa[8];
    ref_shader s_accum_reflected;
    ref_shader s_accum_reflected_msaa[8];
    ref_shader s_accum_volume;
    ref_shader s_accum_volume_unshadowed;
    ref_shader s_accum_volume_msaa[8];

    //	generate min/max
    ref_shader s_create_minmax_sm;

    //	first-person self-shadow
    ref_shader s_hud_shadow;
    // HUD eye space -> self-shadow texel space, rebuilt every frame the pass runs.
    Fmatrix m_hud_shadow_xform{ Fidentity };
    // Reconstruction parameters of the narrow HUD projection, in the layout the deferred
    // position decompression uses: (HorzTan, VertTan, 2*HorzTan/width, 2*VertTan/height).
    Fvector4 m_hud_pos_decompress{};

    //	DX11 Rain
    ref_shader s_rain;
    ref_shader s_rain_msaa[8]; // up to 8 shaders for DX10.0 support

    // Mark MSAA-edge pixels
    ref_shader s_mark_msaa_edges;

    ref_geom g_accum_point;
    ref_geom g_accum_spot;
    ref_geom g_accum_omnipart;
    ref_geom g_accum_volumetric;

    VertexStagingBuffer g_accum_point_vb;
    IndexStagingBuffer g_accum_point_ib;

    VertexStagingBuffer g_accum_omnip_vb;
    IndexStagingBuffer g_accum_omnip_ib;

    VertexStagingBuffer g_accum_spot_vb;
    IndexStagingBuffer g_accum_spot_ib;

    VertexStagingBuffer g_accum_volumetric_vb;
    IndexStagingBuffer g_accum_volumetric_ib;

    // SSAO
    ref_rt rt_ssao_temp;
    ref_rt rt_half_depth;
    ref_shader s_ssao;
    ref_shader s_ssao_msaa[8];
    ref_shader s_hdao_cs;      // HDAO compute shader
    // GTAO (r2_ssao_mode gtao): raw AO + view-z at full res, guided-filtered into
    // rt_ssao_temp. Created only when the mode is active at renderer start.
    ref_rt rt_gtao;
    ref_shader s_gtao;

    // Bloom
    ref_geom g_bloom_build;
    ref_geom g_bloom_filter;
    ref_shader s_bloom_dbg_1;
    ref_shader s_bloom_dbg_2;
    ref_shader s_bloom;
    ref_shader s_bloom_msaa; // if MSAA is disabled, just an alias of s_bloom
    float f_bloom_factor;

    // Luminance
    ref_shader s_luminance;
    float f_luminance_adapt;

    // Combine
    ref_geom g_combine;
    ref_geom g_combine_VP; // xy=p,zw=tc
    ref_geom g_combine_2UV;
    ref_geom g_combine_cuboid;
    ref_geom g_aa_blur;
    ref_geom g_aa_AA;
    ref_shader s_combine_dbg_0;
    ref_shader s_combine_dbg_1;
    ref_shader s_combine_dbg_Accumulator;
    ref_shader s_combine;
    ref_shader s_combine_msaa[8];
    ref_shader s_combine_volumetric;
    ref_shader s_fxaa;
    ref_shader s_smaa;
    ref_shader s_taa;
    ref_shader s_sunshafts;
    ref_shader s_puddle_refl; // world reflections in rain puddles, fullscreen pass
    ref_shader s_water_ripple[3]; // one step of one band of the ripple field; created on first use by its phase
    // The cloud deck field, rendered once per frame over a square of the deck plane around the
    // camera; the sun passes, the shafts and the visible deck all read it instead of evaluating
    // the field's noise per pixel.
    ref_rt rt_cloud_map;
    ref_rt rt_clouds[2]; // the volumetric march at half resolution: current, history
    ref_shader s_clouds_march;
    ref_shader s_clouds_composite;
    ref_rt rt_depth_copy; // depth as the g-buffer left it, readable during light accumulation
    ID3DTexture2D* cloud_readback[2]{}; // one texel of the cloud map, the sun's column, two frames deep
    ref_shader s_cloud_map;
    ref_geom g_fxaa;

public:
    ref_shader s_postprocess;
    ref_shader s_postprocess_msaa; // if MSAA is disabled, just an alias of s_bloom
    ref_geom g_postprocess;
    ref_shader s_menu;
    ref_geom g_menu;
#if 0 // kept for historical reasons
    ref_shader s_flip;
    ref_geom g_flip;
#endif
private:
    float im_noise_time;
    u32 im_noise_shift_w;
    u32 im_noise_shift_h;

    float param_blur;
    float param_gray;
    float param_duality_h;
    float param_duality_v;
    float param_noise;
    float param_noise_scale;
    float param_noise_fps;
    u32 param_color_base;
    u32 param_color_gray;
    Fvector param_color_add;

    //	Color mapping
    float param_color_map_influence;
    float param_color_map_interpolate;
    ColorMapManager color_map_manager;

    //	Igor: used for volumetric lights
    bool m_bHasActiveVolumetric;
    Fmatrix m_mblur_viewproj{};
    u32 m_mblur_frame{};
    bool m_mblur_history_valid{};

public:
    CRenderTarget();
    ~CRenderTarget();

    void build_textures();

    void accum_point_geom_create();
    void accum_point_geom_destroy();
    void accum_omnip_geom_create();
    void accum_omnip_geom_destroy();
    void accum_spot_geom_create();
    void accum_spot_geom_destroy();
    //	Igor: used for volumetric lights
    void accum_volumetric_geom_create();
    void accum_volumetric_geom_destroy();

    ID3DRenderTargetView* get_base_rt() { return rt_Base[HW.CurrentBackBuffer]->pRT; }
    ID3DDepthStencilView* get_base_zb() { return rt_Base_Depth->pZRT[CHW::IMM_CTX_ID]; }

    void u_setrt(CBackend& cmd_list, const ref_rt& _1, const ref_rt& _2, const ref_rt& _3, ID3DDepthStencilView* zb);
    void u_setrt(CBackend& cmd_list, const ref_rt& _1, const ref_rt& _2, const ref_rt& _3, const ref_rt& _zb)
    {
        u_setrt(cmd_list, _1, _2, _3, _zb ? _zb->pZRT[cmd_list.context_id] : nullptr);
    }
    void u_setrt(CBackend& cmd_list, const ref_rt& _1, const ref_rt& _2, ID3DDepthStencilView* zb);
    void u_setrt(CBackend& cmd_list, const ref_rt& _1, const ref_rt& _2, const ref_rt& _zb)
    {
        u_setrt(cmd_list, _1, _2, _zb ? _zb->pZRT[cmd_list.context_id] : nullptr);
    }
    void u_setrt(CBackend& cmd_list, u32 W, u32 H, ID3DRenderTargetView* _1, ID3DRenderTargetView* _2, ID3DRenderTargetView* _3,
        ID3DDepthStencilView* zb);
    void u_setrt(CBackend& cmd_list, u32 W, u32 H, ID3DRenderTargetView* _1, ID3DRenderTargetView* _2, ID3DRenderTargetView* _3,
        const ref_rt& _zb)
    {
        u_setrt(cmd_list, W, H, _1, _2, _3, _zb ? _zb->pZRT[cmd_list.context_id] : nullptr);
    }

    void u_stencil_optimize(CBackend& cmd_list, eStencilOptimizeMode eSOM = SO_Light);
    void u_compute_texgen_screen(CBackend& cmd_list, Fmatrix& dest);
    void u_compute_texgen_jitter(CBackend& cmd_list, Fmatrix& dest);
    void u_calc_tc_noise(Fvector2& p0, Fvector2& p1);
    void u_calc_tc_duality_ss(Fvector2& r0, Fvector2& r1, Fvector2& l0, Fvector2& l1);
    bool u_need_PP();
    bool u_need_CM();
    bool u_DBT_enable(float zMin, float zMax);
    void u_DBT_disable();

    void phase_scene_prepare();
    void phase_scene_begin();
    void phase_scene_end();
    void phase_occq();
    void phase_ssao();
    void phase_gtao(CBackend& cmd_list);
    void phase_hdao();
    void phase_downsamp();
    void phase_wallmarks();

    void phase_smap_direct(CBackend& cmd_list, light *L, u32 sub_phase);
    void phase_smap_direct_tsh(CBackend& cmd_list, light *L, u32 sub_phase);
    void phase_smap_spot_clear(CBackend& cmd_list);
    void phase_smap_spot(CBackend& cmd_list, light* L);
    void phase_smap_spot_tsh(CBackend& cmd_list, light* L);
    void phase_accumulator(CBackend& cmd_list);
    void phase_vol_accumulator(CBackend& cmd_list);

    //	Generates min/max sm
    void create_minmax_SM(CBackend& cmd_list);

    //	First-person self-shadow: the caster map, and its application to the HUD pixels
    void phase_smap_hud(CBackend& cmd_list);
    void phase_hud_shadow(CBackend& cmd_list);
    bool hud_shadow_available() const { return !!s_hud_shadow && !!rt_smap_hud; }
    u32 hud_smap_size() const { return rt_smap_hud ? rt_smap_hud->dwWidth : 0; }
    void set_hud_shadow_params(const Fmatrix& xform, const Fvector4& decompress)
    {
        m_hud_shadow_xform = xform;
        m_hud_pos_decompress = decompress;
    }

    void phase_rain(CBackend& cmd_list);
    void draw_rain(CBackend& cmd_list, light& RainSetup);

    void mark_msaa_edges();

    bool need_to_render_sunshafts();
    bool use_minmax_sm_this_frame();

    bool enable_scissor(light* L); // true if intersects near plane
    void enable_dbt_bounds(light* L);

    void disable_aniso();

    void draw_volume(CBackend& cmd_list, light* L);
    void accum_direct(CBackend& cmd_list, u32 sub_phase);
    void accum_direct_cascade(CBackend& cmd_list, u32 sub_phase, Fmatrix& xform, Fmatrix& xform_prev, float fBias);
    void accum_direct_f(CBackend& cmd_list, u32 sub_phase);
    void accum_direct_lum(CBackend& cmd_list);
    void accum_direct_blend(CBackend& cmd_list);
    void accum_direct_volumetric(CBackend& cmd_list, u32 sub_phase, const u32 Offset, const Fmatrix& mShadow, float zMin, float zMax);
    void accum_point(CBackend& cmd_list, light* L);
    void accum_spot(CBackend& cmd_list, light* L);
    void accum_reflected(CBackend& cmd_list, light* L);
    //	Igor: for volumetric lights
    void accum_volumetric(CBackend& cmd_list, light* L);

    void phase_bloom();
    void phase_luminance();
    void phase_fxaa();
    void phase_smaa();
    void phase_taa(const Fmatrix& reproject);
    void phase_sunshafts();
    void phase_da_puddle_refl(); // world reflections in rain puddles
    void phase_water_ripple(); // fixed steps of the interaction ripple field
    void phase_cloud_map();
    void dump_cloud_map();
    void phase_clouds_march();
    void phase_clouds_composite();
    void copy_depth_for_hud(); // the cloud deck field for this frame (before the sun)
    void phase_combine();
    void phase_combine_volumetric();
    void phase_pp();
#if 0 // kept for historical reasons
    void phase_flip();
#endif

    u32 get_width(CBackend& cmd_list) { return dwWidth[cmd_list.context_id]; }
    u32 get_height(CBackend& cmd_list) { return dwHeight[cmd_list.context_id]; }

    void set_blur(float f) { param_blur = f; }
    void set_gray(float f) { param_gray = f; }
    void set_duality_h(float f) { param_duality_h = _abs(f); }
    void set_duality_v(float f) { param_duality_v = _abs(f); }
    void set_noise(float f) { param_noise = f; }
    void set_noise_scale(float f) { param_noise_scale = f; }
    void set_noise_fps(float f) { param_noise_fps = _abs(f) + EPS_S; }
    void set_color_base(u32 f) { param_color_base = f; }
    void set_color_gray(u32 f) { param_color_gray = f; }
    void set_color_add(const Fvector& f) { param_color_add = f; }
    void set_cm_imfluence(float f) { param_color_map_influence = f; }
    void set_cm_interpolate(float f) { param_color_map_interpolate = f; }
    void set_cm_textures(const shared_str& tex0, const shared_str& tex1)
    {
        color_map_manager.SetTextures(tex0, tex1);
    }

    //	Need to reset stencil only when marker overflows.
    //	Don't clear when render for the first time
    void reset_light_marker(CBackend& cmd_list, bool bResetStencil = false);
    void increment_light_marker(CBackend& cmd_list);

#ifdef DEBUG
    void dbg_addline(const Fvector& P0, const Fvector& P1, u32 c)
    {
        dbg_lines.emplace_back(dbg_line_t{ P0, P1, c });
    }

    void dbg_addbox(const Fbox& box, const u32& color)
    {
        Fvector c, r;
        box.getcenter(c);
        box.getradius(r);
        dbg_addbox(c, r.x, r.y, r.z, color);
    }

    void dbg_addbox(const Fvector& c, float rx, float ry, float rz, u32 color)
    {
        Fvector p1, p2, p3, p4, p5, p6, p7, p8;

        p1.set(c.x + rx, c.y + ry, c.z + rz);
        p2.set(c.x + rx, c.y - ry, c.z + rz);
        p3.set(c.x - rx, c.y - ry, c.z + rz);
        p4.set(c.x - rx, c.y + ry, c.z + rz);

        p5.set(c.x + rx, c.y + ry, c.z - rz);
        p6.set(c.x + rx, c.y - ry, c.z - rz);
        p7.set(c.x - rx, c.y - ry, c.z - rz);
        p8.set(c.x - rx, c.y + ry, c.z - rz);

        dbg_addline(p1, p2, color);
        dbg_addline(p2, p3, color);
        dbg_addline(p3, p4, color);
        dbg_addline(p4, p1, color);

        dbg_addline(p5, p6, color);
        dbg_addline(p6, p7, color);
        dbg_addline(p7, p8, color);
        dbg_addline(p8, p5, color);

        dbg_addline(p1, p5, color);
        dbg_addline(p2, p6, color);
        dbg_addline(p3, p7, color);
        dbg_addline(p4, p8, color);
    }
    void dbg_addplane(Fplane& P0, u32 /*c*/) { dbg_planes.emplace_back(P0); }
#else
    void dbg_addline(Fvector& /*P0*/, Fvector& /*P1*/, u32 /*c*/) {}
    void dbg_addplane(Fplane& /*P0*/, u32 /*c*/) {}
#endif
};
} // namespace xray::render::RENDER_NAMESPACE
