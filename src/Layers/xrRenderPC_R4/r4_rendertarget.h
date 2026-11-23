#pragma once

#include "Layers/xrRender/ColorMapManager.h"

namespace xray::render::RENDER_NAMESPACE
{
class light;
// The state of a DirectX 12 feature lane behind one pointer (dx12-features/PLAN.md 0.2), defined in
// the lane's own file so that this header does not change with the lane's work.
struct DaAsyncTargets;
struct DaVrsTargets;
struct DaRtTargets;

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
    // The ripple field's state, (height, velocity) - the name every reader binds - and the two
    // complex scratch buffers its spectral solver ping-pongs through (see the phase).
    ref_rt rt_WaterRipple;
    ref_rt rt_WaterRippleFFT[2];
    // How much of the visor the screen spans, metres. Not the visor's width: the eye is a few
    // centimetres behind the glass and the game's field of view cuts a window out of it, and that
    // window is what the screen shows edge to edge. Twelve centimetres puts a millimetre of glass
    // at sixteen pixels of a 1080p screen, which is where a two-millimetre bead stops being a dot
    // and starts having an inside - the scale every photograph of rain on a window is taken at.
    // Shared by the step (cell size, and from it the step length) and the optics (slope per mm).
    static constexpr float visor_width_m = 0.12f;
    ref_rt rt_VisorDrops; // the water on the visor: the finished step, under the name readers bind
    ref_rt rt_VisorDropsTmp; // what a step is written into before it is copied to the one above
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
    // Upscaling (da_upscaler.h). rt_Render_Depth is the single-sampled scene depth at render size:
    // the base depth itself unless an upscaler runs, then rt_MSAADepth, which is separate from
    // the display-size base depth the back buffer, the flares, the tracers and the UI use.
    ref_rt rt_Render_Depth;
    ref_rt rt_Reactive; // R8, render size: how much the transparent passes changed each pixel
    ref_rt rt_Upscaled; // RGBA16F + UAV, display size: the upscaler's output
    ref_rt rt_PostDisplay; // RGBA8, display size: combine_2 and the flares, phase_pp's input
    ref_texture t_velocity; // "$user$velocity": the motion vectors, when the velocity pass made them
    // The motion vectors (shaders/r3/da_velocity.h): NDC(current) - NDC(previous), unjittered,
    // y up, RG16F at render size. MRT 2 of the packed G-buffer on the frames that want them;
    // exists only with the packed layout and without MSAA.
    ref_rt rt_Velocity;
    ref_rt rt_Bloom_1; // 32bit, dim/4	(r,g,b,?)
    ref_rt rt_Bloom_2; // 32bit, dim/4	(r,g,b,?)
    ref_rt rt_LUM_64; // 64bit, 64x64,	log-average in all components
    ref_rt rt_LUM_8; // 64bit, 8x8,		log-average in all components
    // The bloom pyramid (r4_rendertarget_phase_da_bloom.cpp, blender_da_bloom.h): RGBA16F levels from half the
    // render size down to 1/64, the horizontal blur's targets, the bright pass's history pair. rt_Bloom_1/2 stay
    // the exposure meter's source.
    ref_rt rt_da_bloom[6];
    ref_rt rt_da_bloom_temp[6];
    ref_rt rt_da_bloom_hist[2];
    // Motion blur (r4_rendertarget_phase_da_mblur.cpp): the longest motion of each tile, and of it and its neighbours.
    ref_rt rt_da_mblur_vz;
    ref_rt rt_da_mblur_tile;
    ref_rt rt_da_mblur_near;

    ref_rt rt_LUM_pool[CHWCaps::MAX_GPUS * 2]; // 1xfp32,1x1,		exp-result -> scaler
    ref_texture t_LUM_src; // source
    ref_texture t_LUM_dest; // destination & usage for current frame

    // smap
    ref_rt rt_smap_surf; // 32bit,		color
    ref_rt rt_smap_depth; // 24(32) bit,	depth
    ref_rt rt_smap_cache; // the lamps' static casters kept between frames (da_smap_cache.cpp)
    ref_shader s_smap_cache;
    // The cache's quad, its own vertices: the lamps' faces record it on worker threads (r2_R_lights.cpp), where the
    // shared dynamic stream cannot be locked.
    VertexStagingBuffer g_smap_cache_vb;
    ref_geom g_smap_cache_quad;
    // The cache's quad over the viewport: xy the slot's corner, z the face size, w 1 = clear (da_smap_cache.cpp).
    void phase_smap_cache_quad(CBackend& cmd_list, float x, float y, float size, float clear);
    // The decals' G-buffer pass (WallmarksEngine.cpp): the albedo's gloss and the normal channels as targets.
    void phase_wallmarks_gbuf();
    void phase_wallmarks_gbuf_masks();
    bool wallmarks_gbuf_ready() const { return m_wmark_gbuf_ok; }
    bool m_wmark_gbuf_ok = false;
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
    // The temporal pass (gtao_temporal.ps): the filtered AO of the frame, the history pair, which one was written last,
    // and the view-projection it was written under.
    ref_rt rt_gtao_cur;
    ref_rt rt_gtao_hist[2];
    u32 m_gtao_hist = 0;
    u32 m_gtao_frame = 0;
    Fmatrix m_gtao_prev_vp;
    void phase_gtao_temporal(CBackend& cmd_list);

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
    ref_shader s_da_bloom[4]; // six elements each (blender_da_bloom.h)
    ref_shader s_da_mblur; // the motion blur's tile passes
    ref_shader s_sunshafts;
    ref_shader s_puddle_refl; // world reflections in rain puddles, fullscreen pass
    ref_shader s_water_ripple; // the ripple field's real-space step; created on first use by its phase
    ref_shader s_water_ripple_fft; // its FFT pass (element 0 reads scratch a, 1 reads b)
    ref_shader s_water_ripple_prop; // its Fourier-space step
    ref_shader s_visor_drops; // the visor drop field's step; created on first use by its phase
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
    // The upscale slot: 0 the identity stretch, 1 the depth upscale, 2 the reactive mask.
    ref_shader s_upscale;
    ref_shader s_combine_display; // combine_2 reading rt_Upscaled instead of generic0
    // Motion vectors the G-buffer cannot carry: the sky's and the forward water surface's.
    // Created on first use by their phases.
    ref_shader s_sky_velocity;
    ref_shader s_water_velocity;

public:
    ref_shader s_postprocess;
    ref_shader s_postprocess_msaa; // if MSAA is disabled, just an alias of s_bloom
    ref_shader s_postprocess_display; // phase_pp reading rt_PostDisplay (upscaling)
    ref_geom g_postprocess;
    ref_shader s_menu;
    ref_shader s_menu_display; // the menu over the display-size targets (upscaling)
    ref_geom g_menu;
    // The unit volumes of the lamps (du_sphere, du_cone, du_sphere_part), for the instanced light volumes.
    const ref_geom& volume_point() const { return g_accum_point; }
    const ref_geom& volume_spot() const { return g_accum_spot; }
    const ref_geom& volume_omnipart() const { return g_accum_omnipart; }
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
    u32 m_da_bloom_frame{}; // the frame the bloom pyramid last ran (its history is the previous frame's)
    u32 m_da_bloom_hist{}; // the history target the last bright pass wrote
    bool m_da_bloom_ran{}; // the pyramid is this frame's: combine_2 adds it
    bool m_da_mblur_ran{}; // the motion blur's tiles are this frame's: combine_2 gathers along them
    Fmatrix m_da_mblur_prev{}; // this frame's view space to the previous frame's clip space

    // The frame the motion vectors were cleared and bound on (phase_scene_begin runs twice in a
    // split scene: only the first call clears).
    u32 m_velocity_frame{ u32(-1) };
    // r__motion_vectors: made on first use, only when the shader ships with the content.
    ref_shader s_velocity_debug;
    bool m_velocity_debug_tried{};
    // The TAA history targets take turns: true = the last resolve wrote rt_taa_resolve, which is
    // this frame's history, and this frame writes rt_taa_history.
    bool m_taa_history_in_resolve{};

public:
    CRenderTarget();
    ~CRenderTarget();

    // The motion vectors were rendered this frame; rt_Velocity holds them.
    bool velocity_ready() const { return rt_Velocity && m_velocity_frame == Device.dwFrame; }
    void phase_velocity_debug(); // r__motion_vectors: the vectors over the finished frame

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
    // The same G-buffer targets and states on a parallel context that draws part of the scene
    // (its list executes inside phase_scene_begin/phase_scene_end of the immediate context).
    void phase_scene_begin_parallel(CBackend& cmd_list, bool velocity);
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
    // The froxel volume of the sunlit air (r4_rendertarget_phase_froxels.cpp): each cascade's matrix as the sun
    // accumulates it, then the injection, the integration and the apply into the volumetric light target.
    void froxels_create();
    void froxels_destroy();
    void froxels_note_cascade(u32 cascade, const Fmatrix& m_shadow);
    bool phase_froxels(CBackend& cmd_list);
    bool froxels_ran() const; // the volume is this frame's
    struct DaFroxels* m_froxels{};
    // The screen-space glossy reflections (r4_rendertarget_phase_ssr.cpp) where the traced ones do not run: the trace
    // and its temporal pass before combine_1, which writes the linear colour the next frame's trace reads.
    void ssr_create();
    void ssr_destroy();
    void phase_ssr(CBackend& cmd_list);
    const ref_rt* ssr_scene_target(); // combine_1's third target this frame, or null
    bool ssr_ran() const; // combine_1 reads them this frame
    struct DaSsr* m_ssr{};
    void accum_reflected(CBackend& cmd_list, light* L);
    //	Igor: for volumetric lights
    void accum_volumetric(CBackend& cmd_list, light* L);

    void phase_bloom();
    void da_bloom_create();
    bool da_bloom_wanted() const; // the pyramid runs this frame (it wants the opaque scene grab)
    void phase_da_bloom(bool opaque_grab);
    void da_mblur_create();
    // previous: this frame's view space to the previous frame's clip space (the camera's motion without vectors).
    void phase_da_mblur(const Fmatrix& previous);
    void set_da_mblur_constants(bool on);
    void phase_luminance();
    void phase_fxaa();
    // copy_back: the result into generic0; false leaves it in rt_Generic for the TAA after it.
    void phase_smaa(bool copy_back = true);
    // smaa_in_generic: the frame to resolve is SMAA's output in rt_Generic, and the resolve goes
    // straight into generic0.
    void phase_taa(const Fmatrix& reproject, bool smaa_in_generic);
    void phase_sunshafts();
    void phase_da_puddle_refl(); // world reflections in rain puddles
    void phase_water_ripple(); // fixed steps of the interaction ripple field
    void phase_visor_drops(); // fixed steps of the water standing on the actor's visor
    void phase_cloud_map();
    void dump_cloud_map();
    void phase_clouds_march();
    void phase_clouds_composite();
    void copy_depth_for_hud(); // the depth for the local lights' first-person test, with the HUD self-shadow
    // The depth the G-buffer left, copied into $user$depth_copy; false under MSAA (no copy target).
    bool copy_depth(CBackend& cmd_list);
    void phase_combine();
    void phase_combine_volumetric();
    void phase_pp();
#if 0 // kept for historical reasons
    void phase_flip();
#endif
    // The upscale slot (r4_rendertarget_phase_upscale.cpp).
    bool upscale_ready() const; // the targets and shaders it needs exist
    void phase_reactive(); // rt_Reactive from the opaque scene grab against the finished scene
    bool m_taa_reactive = false; // this frame's reactive mask is there for native TAA
    void phase_upscale(); // generic0 -> rt_Upscaled through the backend, then the depth upscale
    void phase_upscale_identity(); // the identity backend: a bilinear stretch into rt_Upscaled
    void phase_depth_upscale(); // the render-size scene depth into the display-size base depth
    // Motion vectors after the forward pass (r4_rendertarget_phase_sky_water_velocity.cpp): the
    // sky pixels, then the water surface over them. The forward pass consumes its draw lists,
    // so the water is collected before it.
    void phase_sky_velocity();
    void phase_water_velocity_collect();
    void phase_water_velocity();
    // What the passes inside the scene leave bound for whatever renders next: the back buffer and
    // its depth - or the scene depth alone while an upscaler runs (the sizes differ then).
    void u_setrt_scene_base(CBackend& cmd_list);

    // [dx12:async] begin members
    // The compute versions of GTAO and the ripple step (async.md 3.4). false (the ripple: -1, else
    // its step count) = the compute pass is not available here, the caller runs the pixel pass.
    // verify = a r__async_verify stage runs: first the pixel pass (port) or this compute pass inline
    // on the immediate context (schedule) from the same inputs, and the two results are compared
    // (schedule: after the join).
    bool phase_gtao_cs(CBackend& cmd_list, bool verify);
    int phase_water_ripple_cs(CBackend& cmd_list, bool verify);
    void phase_water_ripple_publish();
    // GTAO is the AO technique of the frame (the test of phase_combine's AO slot).
    bool gtao_is_frame_ao() const;
    // One ripple step: the real-space pass as a pixel (compute = false) or a compute pass, then the
    // spectral dispatches. A pixel step copies its result into the state as it always did; a
    // compute step leaves it in scratch b for the next step and phase_water_ripple_publish.
    void phase_water_ripple_step(CBackend& cmd_list, const struct DaRippleStep& step, int index, bool compute);
    ref_shader s_gtao_cs;
    ref_shader s_water_ripple_cs;
    ref_shader s_async_compare;
    bool m_ripple_publish{};
    DaAsyncTargets* m_da_async{};
    // [dx12:async] end members
    // [dx12:vrs] begin members
    DaVrsTargets* m_da_vrs{};
    // [dx12:vrs] end members
    // [dx12:rt] begin members
    DaRtTargets* m_da_rt{};
    // The combine's own resources the ray tracing passes draw with (the AO slot's GTAO targets and
    // filter, the fullscreen quad, the puddle pass): r4_rendertarget_phase_rt.cpp.
    friend struct DaRtAccess;
    // [dx12:rt] end members

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
