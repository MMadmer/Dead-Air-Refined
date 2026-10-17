#pragma once

extern ENGINE_API int ps_r__Supersample;

namespace xray::render::RENDER_NAMESPACE
{
// Common
extern ECORE_API u32 ps_r_sun_shafts; //=	0;
extern ECORE_API const xr_token qsun_shafts_token[];
extern ECORE_API float ps_r2_sun_shafts_value;
extern ECORE_API int ps_r2_sss_enable;
extern ECORE_API float ps_r2_sss_intensity;
extern ECORE_API float ps_r2_sss_blend;
extern ECORE_API float ps_r2_sss_phase1;
extern ECORE_API float ps_r2_sss_phase2;
extern ECORE_API float ps_r2_sss_radius;
extern ECORE_API int ps_r2_fxaa;

extern ECORE_API u32 ps_r2_smapsize;
extern ECORE_API const xr_token qsmapsize_token[];

extern ECORE_API u32 ps_r_ssao; //	=	0;
extern ECORE_API const xr_token qssao_token[];

extern ECORE_API u32 ps_r_ssao_mode;
extern ECORE_API const xr_token qssao_mode_token[];

enum
{
    ssao_mode_off,
    ssao_mode_default,
    ssao_mode_hdao,
    ssao_mode_hbao,
    ssao_mode_gtao,
};

extern ECORE_API u32 ps_r_sun_quality; //	=	0;
extern ECORE_API u32 ps_r_water_reflection; //	=	0;
extern ECORE_API const xr_token qsun_quality_token[];
extern ECORE_API u32 ps_r_sun_details;
extern ECORE_API const xr_token qsun_details_token[];
extern ECORE_API u32 ps_r_lighting_quality;
extern ECORE_API const xr_token qlighting_quality_token[];
extern ECORE_API const xr_token qwater_reflection_quality_token[];

enum DetailShadowQuality : u32
{
    detail_shadow_off,
    detail_shadow_medium,
    detail_shadow_high,
};

extern ECORE_API u32 ps_r3_msaa; //	=	0;
extern ECORE_API const xr_token qmsaa_token[];

extern ECORE_API u32 ps_r3_msaa_atest; //=	0;
extern ECORE_API const xr_token qmsaa__atest_token[];

extern ECORE_API u32 ps_r3_minmax_sm; //	=	0;
extern ECORE_API const xr_token qminmax_sm_token[];

enum GeometryOptimizationQuality : u32
{
    geometry_optimization_off,
    geometry_optimization_low,
    geometry_optimization_medium,
    geometry_optimization_high,
};

extern ECORE_API u32 ps_r_optimize_static;

extern ECORE_API int ps_r__LightSleepFrames;
extern ECORE_API int ps_r__light_shadow_budget;
extern ECORE_API int ps_r__light_details;
extern ECORE_API int ps_r__light_dyn_shared;
extern ECORE_API int ps_r__hud_shadow;
extern ECORE_API int ps_r__actor_shadow;
extern ECORE_API float ps_r__hud_shadow_normal_offset;
extern ECORE_API float ps_r__hud_shadow_slope_bias;
extern ECORE_API float ps_r__sss;
extern ECORE_API float ps_r__sss_len;
extern ECORE_API float ps_r__sss_thick;
extern ECORE_API float ps_r__sss_steps;
extern ECORE_API float ps_r__fog;
extern ECORE_API float ps_r__fog_sky;
extern ECORE_API float ps_r__fog_sky_mip;
extern ECORE_API float ps_r__fog_sky_flat;
extern ECORE_API float ps_r__fog_height;
extern ECORE_API float ps_r__fog_height_falloff;
extern ECORE_API float ps_r__fog_height_base;
extern ECORE_API float ps_r__fog_dist;
extern ECORE_API float ps_r__fog_follow_vis;
extern ECORE_API float ps_r__fog_max;
extern ECORE_API float ps_r__tonemap_hue;
extern ECORE_API float ps_r__tonemap_desat;
extern ECORE_API float ps_r__tonemap_white;
extern ECORE_API float ps_r__grade_sat;
extern ECORE_API float ps_r__grade_green;
extern ECORE_API float ps_r__grade_olive;
extern ECORE_API float ps_r__grade_contrast;
extern ECORE_API float ps_r__bloom;
extern ECORE_API float ps_r__bloom_threshold;
extern ECORE_API float ps_r__bloom_knee;
extern ECORE_API int ps_r__mblur;
extern ECORE_API float ps_r__mblur_shutter;
extern ECORE_API float ps_r__froxel_strength;
extern ECORE_API int ps_r__spec_ggx;
extern ECORE_API int ps_r__particle_light;
extern ECORE_API int ps_r__ssr;
extern ECORE_API float ps_r__spec_f0;
extern ECORE_API float ps_r__spec_f0_max;
extern ECORE_API float ps_r__spec_smooth;
extern ECORE_API int ps_r__sun_cache_ms;

extern ECORE_API float ps_r__Detail_l_ambient;
extern ECORE_API float ps_r__Detail_l_aniso;
extern ECORE_API float ps_r__Detail_density;
extern ECORE_API float ps_r__Detail_height;

extern ECORE_API float ps_r__Tree_SBC; // scale bias correct

extern ECORE_API float ps_r__WallmarkTTL;
extern ECORE_API float ps_r__WallmarkSHIFT;
extern ECORE_API float ps_r__WallmarkSHIFT_V;

extern ECORE_API float ps_r__GLOD_ssa_start;
extern ECORE_API float ps_r__GLOD_ssa_end;
extern ECORE_API float ps_r__LOD;
//.extern ECORE_API	float		ps_r__LOD_Power		;
extern ECORE_API float ps_r__ssaDISCARD;
// Separate discard threshold for vegetation billboards (FLOD); see xrRender_console.cpp.
extern ECORE_API float ps_r__vegDISCARD;
extern ECORE_API float ps_r__grass_fade_start;
extern ECORE_API int ps_r__gpu_log;
extern ECORE_API int ps_r__screenshot_every;
extern ECORE_API int ps_r__clouds_quality;
extern ECORE_API int ps_r__clouds_debug;
extern ECORE_API float ps_r__clouds_temporal;
extern ECORE_API int ps_r__clouds_quality_override;
extern ECORE_API float ps_r__clouds_cover;
extern ECORE_API float ps_r__grass_fade_flat;
extern ECORE_API int ps_r__grass_shadow_dist;
extern ECORE_API int ps_r__aref_quality;
extern ECORE_API int ps_r__smaa;
extern ECORE_API int ps_r__taa;
extern ECORE_API int ps_r__motion_vectors; // QA view of the motion vectors (phase_velocity_debug)
// Anti-aliasing (r__aa), the upscalers (r__fsr / r__dlss, token value = quality, 0 = off), frame
// generation (r__framegen) and the upscaler's sharpening - docs/dead-air/DX12_RENDERER.md, 2.
extern ECORE_API u32 ps_r__aa;
extern ECORE_API u32 ps_r__fsr;
extern ECORE_API u32 ps_r__dlss;
extern ECORE_API u32 ps_r__framegen;
extern ECORE_API float ps_r__upscale_sharpness;

// DirectX 12 features (docs/dead-air/dx12-features/PLAN.md 2.6), registered by both modules; each
// lane changes only its own block.
// [dx12:dxc] begin variables
extern ECORE_API int ps_r__dxc; // r__dxc: engine shaders through DXC, next start
// [dx12:dxc] end variables
// [dx12:async] begin variables
extern ECORE_API u32 ps_r__async_compute; // r__async_compute: 0 off, 1 auto, 2 on
extern ECORE_API const xr_token qasync_compute_token[];
extern ECORE_API int ps_r__async_path; // r__async_path: 0 the setting, 1 pixel, 2 inline, 3 async
// [dx12:async] end variables
// [dx12:vrs] begin variables
extern ECORE_API int ps_r__vrs; // the preset's mode: 0 off, 1 conservative, 2 aggressive
extern ECORE_API int ps_r__vrs_override; // r__vrs: -1 the preset, else the mode for the session
extern ECORE_API float ps_r__vrs_threshold;
extern ECORE_API int ps_r__vrs_force;
extern ECORE_API int ps_r__vrs_debug;
extern ECORE_API int ps_r__vrs_log;
// [dx12:vrs] end variables
// [dx12:gd] begin variables
extern ECORE_API int ps_r__gpu_driven; // r__gpu_driven: the GPU-driven trees (-1 auto, 0 off, 1 on, 2 CPU cull)
// [dx12:gd] end variables
// [dx12:mesh] begin variables
extern ECORE_API int ps_r__grass_mesh; // r__grass_mesh: mesh grass (-1 auto, 0 off, 1 on)
extern ECORE_API int ps_r__shrubs; // r__shrubs (QA): 0 = the stock bushes in place of the procedural shrubs
extern ECORE_API int ps_r__dbg_shrubs; // r__dbg_shrubs (QA): draws of the shrubs left out (da_shrubs.cpp draw_range)
extern ECORE_API float ps_r__shrubs_lod_scale; // r__shrubs_lod_scale (QA): the shrubs' tier distances scaled (da_shrubs.cpp view_bush)
extern ECORE_API int ps_r__shrubs_force_tier; // r__shrubs_force_tier (QA): every bush in one tier, -1 off (da_shrubs.cpp view_bush)
extern ECORE_API int ps_r__trees; // r__trees (QA): 0 = the stock trees in place of the upgraded trees
extern ECORE_API int ps_r__dbg_trees; // r__dbg_trees (QA): draws of the trees left out (da_trees.cpp skip_draw)
extern ECORE_API int ps_r__trees_cut; // r__trees_cut (QA): 0 = the trees' view without the GPU cut (da_veg.cpp cut)
extern ECORE_API int ps_r__veg_soft; // r__veg_soft (QA): the vegetation's soft switches, bits (1 thinned, 2 wood, 4 morphs)
extern ECORE_API int ps_r__dbg_wood; // r__dbg_wood (QA): the wood's bark in false colour (da_shrub_wood.ps shrub_dbg_*)
extern ECORE_API float ps_r__trees_lod_scale; // r__trees_lod_scale (QA): the trees' tier distances scaled (da_trees.cpp tiers)
extern ECORE_API float ps_r__shrubs_far_detail; // r__shrubs_far_detail (QA): the shrubs' leaf tier longer
extern ECORE_API float ps_r__trees_far_detail; // r__trees_far_detail (QA): the thinned leaves' and clusters' tiers longer
extern ECORE_API float ps_r__trees_detail; // r__trees_detail (QA): the crowns' leaf and wood floors divided (da_trees.cpp frame_begin)
extern ECORE_API int ps_r__trees_force_tier; // r__trees_force_tier (QA): every tree in one tier, -1 off (da_trees.cpp view)
extern ECORE_API int ps_r__dbg_grass_ab; // r__dbg_grass_ab: one A/B of the grass paths next frame
// [dx12:mesh] end variables
extern ECORE_API int ps_r__dbg_snow; // r__dbg_snow: the QA view of the snow passes (0 off, 1 paths, 2 no grid cuts)
extern ECORE_API float ps_r__snow_glint; // r__snow_glint: the glints' strength (QA, 1 as built)
extern ECORE_API int ps_r__snow_shells; // r__snow_shells: the snow shells (QA, 1 as built)
extern ECORE_API int ps_r__snow_shadow; // r__snow_shadow: the snow grid in the near sun cascade (QA, 1 as built)
// r__dbg_grass (QA, cost experiments of the blade generation): 1 no basal pairs, 2 no litter, 4 the community's first
// graminoid only, 8 no early out, 16 no stem leaves, 32 the palette's presence near the camera too, 64 every lane
// ends after the early out, 128 no wind forces.
extern ECORE_API int ps_r__dbg_grass;
// [dx12:offload] begin variables
// The auto value of r__gpu_driven and r__grass_mesh (da_offload.h): its thresholds.
extern ECORE_API float ps_r__offload_margin; // how much slower the bottleneck side is, 0.1 = 10 %
extern ECORE_API float ps_r__offload_agree; // seconds the verdict holds before a switch
extern ECORE_API float ps_r__offload_dwell; // seconds a switch holds at least
extern ECORE_API float ps_r__offload_recall; // seconds a path left stays remembered
extern ECORE_API float ps_r__offload_smooth; // seconds of the timings' smoothing
extern ECORE_API float ps_r__offload_warmup; // seconds of a level (or of a return to auto) not measured
// [dx12:offload] end variables
// [dx12:ds] begin variables
extern ECORE_API int ps_r__dstorage; // r__dstorage: DirectStorage texture loading
// [dx12:ds] end variables
// [dx12:rt] begin variables
// The RTX category of the Video tab: never written by a preset.
extern ECORE_API int ps_r__rt_reflections;
extern ECORE_API int ps_r__rt_shadows;
extern ECORE_API int ps_r__rtao;
extern ECORE_API u32 ps_r__rt_quality; // 0 low, 1 medium, 2 high
extern ECORE_API const xr_token qrt_quality_token[];
extern ECORE_API int ps_r__rt_debug;
// [dx12:rt] end variables

extern ECORE_API int ps_r__grass_shadow_fade;
// The fluid campfire (CDaFireEffect over the 3D fluid grid): on/off, and the distance within
// which a fire is simulated rather than drawn as the procedural flame.
extern ECORE_API int ps_r__fire_fluid;
extern ECORE_API float ps_r__fire_fluid_dist;
extern ECORE_API int ps_r__fire_fluid_grids;
extern ECORE_API float ps_r__grass_tint;
extern ECORE_API float ps_r__grass_tint_scale;
extern ECORE_API float ps_r__grass_tint_base;
extern ECORE_API float ps_r__lod_hemi;
extern ECORE_API float ps_r__lod_sat;
extern ECORE_API float ps_r__lod_bright;
extern ECORE_API float ps_r__foliage_gloss;
extern ECORE_API float ps_r__foliage_vibrance;
extern ECORE_API float ps_r__foliage_debleach;
extern ECORE_API int ps_r__puddles;
extern ECORE_API float ps_r__puddles_buildup;
extern ECORE_API float ps_r__puddles_dry;
extern ECORE_API float ps_r__puddles_size;
extern ECORE_API float ps_r__puddles_force;
extern ECORE_API float ps_r__puddles_gloss;
extern ECORE_API float ps_r__puddles_dark;
extern ECORE_API float ps_r__puddles_damp;
extern ECORE_API float ps_r__puddles_ripple;
extern ECORE_API int ps_r__puddles_debug;
extern ECORE_API int ps_r__puddles_dist;
extern ECORE_API int ps_r__puddles_gbuf;
extern ECORE_API float ps_r__puddles_edge;
extern ECORE_API float ps_r__puddles_rim;
extern ECORE_API float ps_r__puddles_rim_width;
extern ECORE_API int ps_r__puddles_refl;
extern ECORE_API float ps_r__puddles_refl_power;
extern ECORE_API float ps_r__puddles_facing;
extern ECORE_API float ps_r__puddles_sky;
extern ECORE_API float ps_r__parallax_start;
extern ECORE_API float ps_r__parallax_stop;
extern ECORE_API float ps_r__parallax_depth;
extern ECORE_API float ps_r__parallax_shadow;
extern ECORE_API int ps_r__parallax_samples;
extern ECORE_API int ps_r__parallax_samples_min;
extern ECORE_API int ps_r__parallax_shadow_samples;
extern ECORE_API int ps_r__parallax_force;
extern ECORE_API int ps_r__parallax_debug;
extern ECORE_API float ps_r__sun_shadow_fade;
extern ECORE_API float ps_r__sun_cascade_mid;
extern ECORE_API float ps_r__sun_cascade_far;
extern ECORE_API float ps_r__sun_cascade_blend;
extern ECORE_API int ps_r__dbg_sun_cascades;
extern ECORE_API int ps_r__particle_dist;
extern ECORE_API float ps_r__macro_var;
extern ECORE_API float ps_r__macro_var_scale;
extern ECORE_API float ps_r__macro_var_start;
extern ECORE_API float ps_r__macro_var_end;
extern ECORE_API float ps_r__macro_tint;
extern ECORE_API float ps_r__macro_relief;
extern ECORE_API float ps_r__terrain_blend;
extern ECORE_API float ps_r__mask_jitter;
extern ECORE_API float ps_r__ssaDONTSORT;
extern ECORE_API float ps_r__ssaHZBvsTEX;
extern ECORE_API int ps_r__tf_Anisotropic;
extern ECORE_API float ps_r__tf_Mipbias;

extern ECORE_API int ps_r__clear_models_on_unload;
extern ECORE_API int ps_r__unload_level_textures;

// R1
extern ECORE_API float ps_r1_ssaLOD_A;
extern ECORE_API float ps_r1_ssaLOD_B;
extern ECORE_API float ps_r1_lmodel_lerp;
extern ECORE_API float ps_r1_dlights_clip;
extern ECORE_API float ps_r1_pps_u;
extern ECORE_API float ps_r1_pps_v;
extern ECORE_API int ps_r1_force_geomx;

// R1-specific
extern ECORE_API int ps_r1_GlowsPerFrame; // r1-only
extern ECORE_API Flags32 ps_r1_flags; // r1-only

extern ECORE_API float ps_r1_fog_luminance; // 1.f r1-only
extern ECORE_API int ps_r1_SoftwareSkinning; // r1-only

enum
{
    R1FLAG_DLIGHTS = (1 << 0),
    R1FLAG_FFP = (1 << 1), // don't use shaders, only fixed-function pipeline or software processing
    R1FLAG_FFP_LIGHTMAPS = (1 << 2),
};

// R2
extern ECORE_API bool ps_r2_sun_static;
extern ECORE_API BOOL ps_r2_sun_complex;
extern ECORE_API bool ps_r2_advanced_pp; // advanced post process and effects

extern ECORE_API float ps_r2_ssaLOD_A;
extern ECORE_API float ps_r2_ssaLOD_B;

// R2-specific
extern ECORE_API Flags32 ps_r2_ls_flags; // r2-only
extern ECORE_API Flags32 ps_r2_ls_flags_ext;
extern ECORE_API float ps_r2_df_parallax_h; // r2-only
extern ECORE_API float ps_r2_df_parallax_range; // r2-only
extern ECORE_API float ps_r2_gmaterial; // r2-only
extern ECORE_API float ps_r2_tonemap_middlegray; // r2-only
extern ECORE_API float ps_r2_tonemap_adaptation; // r2-only
extern ECORE_API float ps_r2_tonemap_low_lum; // r2-only
extern ECORE_API float ps_r2_tonemap_amount; // r2-only
extern ECORE_API float ps_r2_ls_bloom_kernel_scale; // r2-only	// gauss
extern ECORE_API float ps_r2_ls_bloom_kernel_g; // r2-only	// gauss
extern ECORE_API float ps_r2_ls_bloom_kernel_b; // r2-only	// bilinear
extern ECORE_API float ps_r2_ls_bloom_threshold; // r2-only
extern ECORE_API float ps_r2_ls_bloom_speed; // r2-only
extern ECORE_API float ps_r2_ls_dsm_kernel; // r2-only
extern ECORE_API float ps_r2_ls_psm_kernel; // r2-only
extern ECORE_API float ps_r2_ls_ssm_kernel; // r2-only
extern ECORE_API Fvector ps_r2_aa_barier; // r2-only
extern ECORE_API Fvector ps_r2_aa_weight; // r2-only
extern ECORE_API float ps_r2_aa_kernel; // r2-only
extern ECORE_API int ps_r2_GI_depth; // 1..5
extern ECORE_API int ps_r2_GI_photons; // 8..256
extern ECORE_API float ps_r2_GI_clip; // EPS
extern ECORE_API float ps_r2_GI_refl; // .9f
extern ECORE_API float ps_r2_ls_depth_scale; // 1.0f
extern ECORE_API float ps_r2_ls_depth_bias; // -0.0001f
// Hardware depth bias of the shadow casters (slope-scaled: grazing surfaces get more, contact keeps tight).
extern ECORE_API float ps_r__smap_slope_bias;
extern ECORE_API int ps_r__smap_depth_bias;
extern ECORE_API float ps_r2_ls_squality; // 1.0f
extern ECORE_API float ps_r2_sun_near; // 10.0f
extern ECORE_API float ps_r2_sun_near_border; // 1.0f
extern ECORE_API float ps_r2_sun_far;
extern ECORE_API float ps_r2_sun_tsm_projection; // 0.2f
extern ECORE_API float ps_r2_sun_tsm_bias; // 0.0001f
extern ECORE_API float ps_r2_sun_depth_far_scale; // 1.00001f
extern ECORE_API float ps_r2_sun_depth_far_bias; // -0.0001f
extern ECORE_API float ps_r2_sun_depth_near_scale; // 1.00001f
extern ECORE_API float ps_r2_sun_depth_near_bias; // -0.0001f
extern ECORE_API float ps_r2_sun_lumscale; // 0.5f
extern ECORE_API float ps_r2_sun_lumscale_hemi; // 1.0f
extern ECORE_API float ps_r2_sun_lumscale_amb; // 1.0f
extern ECORE_API float ps_r2_zfill; // .1f

extern ECORE_API float ps_r2_dhemi_sky_scale; // 1.5f
extern ECORE_API float ps_r2_dhemi_light_scale; // 1.f
extern ECORE_API float ps_r2_dhemi_light_flow; // .1f
extern ECORE_API int ps_r2_dhemi_count; // 5
extern ECORE_API float ps_r2_slight_fade; // 1.f
extern ECORE_API int ps_r2_wait_sleep;
extern ECORE_API int ps_r2_wait_timeout;

//	x - min (0), y - focus (1.4), z - max (100)
extern ECORE_API Fvector3 ps_r2_dof;
extern ECORE_API float ps_r2_dof_sky; //	distance to sky
extern ECORE_API float ps_r2_dof_kernel_size; //	7.0f
extern ECORE_API int ps_r2_dof_pickable;
extern ECORE_API float ps_r2_dof_time;
extern ECORE_API int ps_r2_dof_diff_near;
extern ECORE_API int ps_r2_dof_diff_far;

extern ECORE_API int ps_r2_technicolor;
extern ECORE_API int ps_r2_vignette;
extern ECORE_API BOOL ps_r2_filmgrain;
extern ECORE_API int ps_r2_reflections;
extern ECORE_API int ps_r2_lensdirt;
extern ECORE_API int ps_r2_lenswater;
extern ECORE_API float ps_r2_aberration;
extern ECORE_API float ps_r2_vibrance;
extern ECORE_API float ps_r2_lensdirt_value;
extern ECORE_API float ps_r2_lenswater_value;
extern ECORE_API float ps_r2_lumasharpen;
extern ECORE_API Fvector4 ps_r2_temp;
extern ECORE_API float ps_shaders_var_x;
extern ECORE_API float ps_shaders_var_y;
extern ECORE_API float ps_shaders_var_z;
extern ECORE_API float ps_shaders_var_w;
extern ECORE_API float ps_r2_postprocess_var_x;
extern ECORE_API float ps_r2_postprocess_var_y;
extern ECORE_API float ps_r2_postprocess_var_z;
extern ECORE_API float ps_r2_postprocess_var_w;
extern ECORE_API float ps_r2_lens_var_x;
extern ECORE_API float ps_r2_lens_var_y;
extern ECORE_API float ps_r2_lens_var_z;
extern ECORE_API float ps_r2_lens_var_w;
extern ECORE_API BOOL ps_detail_scale_on_fade;

extern ECORE_API float ps_r3_dyn_wet_surf_near; // 10.0f
extern ECORE_API float ps_r3_dyn_wet_surf_far; // 30.0f
extern ECORE_API int ps_r3_dyn_wet_surf_sm_res; // 256

enum
{
    R2FLAG_SUN = (1 << 0),
    R2FLAG_SUN_FOCUS = (1 << 1),
    R2FLAG_SUN_TSM = (1 << 2),
    R2FLAG_SUN_DETAILS = (1 << 3),
    R2FLAG_TONEMAP = (1 << 4),
    R2FLAG_AA = (1 << 5),
    R2FLAG_GI = (1 << 6),
    R2FLAG_FASTBLOOM = (1 << 7),
    R2FLAG_GLOBALMATERIAL = (1 << 8),
    R2FLAG_ZFILL = (1 << 9),
    R2FLAG_R1LIGHTS = (1 << 10),
    R2FLAG_SUN_IGNORE_PORTALS = (1 << 11),

    //	R2FLAG_SUN_STATIC			= (1<<12),

    R2FLAG_EXP_SPLIT_SCENE = (1 << 13),
    R2FLAG_EXP_DONT_TEST_UNSHADOWED = (1 << 14),
    R2FLAG_EXP_DONT_TEST_SHADOWED = (1 << 15),

    R2FLAG_USE_NVDBT = (1 << 16),
    R2FLAG_USE_NVSTENCIL = (1 << 17),

    R2FLAG_EXP_MT_CALC = (1 << 18),

    R2FLAG_SOFT_WATER = (1 << 19), //	Igor: need restart
    R2FLAG_SOFT_PARTICLES = (1 << 20), //	Igor: need restart
    R2FLAG_VOLUMETRIC_LIGHTS = (1 << 21),
    R2FLAG_STEEP_PARALLAX = (1 << 22),
    R2FLAG_DOF = (1 << 23),

    R1FLAG_DETAIL_TEXTURES = (1 << 24),

    R2FLAG_DETAIL_BUMP = (1 << 25),

    R3FLAG_DYN_WET_SURF = (1 << 26),
    R3FLAG_VOLUMETRIC_SMOKE = (1 << 27),

    // R3FLAG_MSAA					= (1<<28),
    R3FLAG_MSAA_HYBRID = (1 << 28),
    R3FLAG_MSAA_OPT = (1 << 29),
    R3FLAG_GBUFFER_OPT = (1 << 30),
    R3FLAG_USE_DX10_1 = (1 << 31),
    // R3FLAG_MSAA_ALPHATEST		= (1<<31),
};

enum
{
    R2FLAGEXT_SSAO_BLUR = (1 << 0),
    R2FLAGEXT_SSAO_OPT_DATA = (1 << 1),
    R2FLAGEXT_SSAO_HALF_DATA = (1 << 2),
    R2FLAGEXT_SSAO_HBAO = (1 << 3),
    R2FLAGEXT_SSAO_HDAO = (1 << 4),
    R2FLAGEXT_ENABLE_TESSELLATION = (1 << 5),
    R2FLAGEXT_WIREFRAME = (1 << 6),
    R_FLAGEXT_HOM_DEPTH_DRAW = (1 << 7),
    R2FLAGEXT_SUN_ZCULLING = (1 << 8),
    R2FLAGEXT_SUN_OLD = (1 << 9),
    R3FLAGEXT_SSR_HALF_DEPTH = (1 << 10),
    R3FLAGEXT_SSR_JITTER = (1 << 11),
};

extern void xrRender_initconsole();
// Re-derive the preset-driven switches (shadow budget, light details, hud/actor shadow)
// from ps_Preset, for values that never serialize into user.ltx.
extern void xrRender_sync_preset_derived();
// Replay the whole quality preset: its rspec file, then the switches above. The advanced video
// page shows nothing the preset owns, so nothing else can be the source of those values - and a
// user.ltx line left over from a build whose menu did show them has to lose to the preset.
extern void xrRender_apply_preset();
// r__aa -> the runtime switches of the passes (r2_fxaa, r__smaa, r__taa).
extern void xrRender_apply_aa();
extern BOOL xrRender_test_hw();
} // namespace xray::render::RENDER_NAMESPACE
