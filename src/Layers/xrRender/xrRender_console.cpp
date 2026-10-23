#include "stdafx.h"
#pragma hdrstop

#include "xrRender_console.h"
#include "xrCore/xr_token.h"
#include "xrCore/Animation/SkeletonMotions.hpp"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"
#if defined(USE_DX11)
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#endif

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

#if RENDER == R_R4
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderPC_R4/da_upscaler.h"
#include "Layers/xrRenderPC_R4/da_native_selftest.h"
#include "Layers/xrRenderPC_R4/da_async.h"
#include "Layers/xrRenderPC_R4/da_vrs.h"
#include "Layers/xrRenderPC_R4/da_grass_mesh.h"
#include "Layers/xrRenderPC_R4/da_dstorage.h"
#include "Layers/xrRender/da_grass_blades.h"
#include "Layers/xrRender/da_shrubs.h"
#include "Layers/xrRender/da_veg.h"
#include "Layers/xrRender/da_trees.h"
#include "Layers/xrRender/da_ground.h"
#include "Layers/xrRender/da_trees_bake.h"
#include "Layers/xrRender/da_snow.h"
#endif

// How many of the eight wave rows the water shader evaluates - a quality tier, set from the
// preset ladder below and overridable for the session from the console. Declared here rather
// than defined with its neighbours (and outside the render namespace, or the extern would name
// a symbol nothing defines): the wave solver that clamps to it lives in CEnvironment, and the
// engine cannot see a variable that belongs to the render DLL. It lives in xr_ioc_cmd.cpp with
// ps_r__WallmarksOnSkeleton, which is shared the same way.
extern ENGINE_API int ps_r__water_waves;
// The rest of the water ladder, shared the same way and for the same reason: the ripple window
// and the rain rate are solved in CEnvironment, the targets and the shaders live here.
extern ENGINE_API int ps_r__water_ripple;
extern ENGINE_API int ps_r__visor_drops;
extern ENGINE_API int ps_r__water_underwater;
extern ENGINE_API int ps_r__water_caustics;
extern ENGINE_API int ps_r__puddle_fill;
extern ENGINE_API int ps_r__rain_quality;
// The rain LOOK knobs, defined with their comments in xr_ioc_cmd.cpp. They are session
// overrides now, but a user.ltx from an earlier build still carries them and still executes
// before the renderer comes up, so the preset sync re-applies the authored values below.
extern ENGINE_API float ps_r__rain_len;
extern ENGINE_API float ps_r__rain_width;
extern ENGINE_API float ps_r__rain_bright;
extern ENGINE_API float ps_r__rain_splash_bright;
extern ENGINE_API float ps_r__rain_radius;
extern ENGINE_API float ps_r__rain_splash;
extern ENGINE_API float ps_r__rain_splash_time;
extern ENGINE_API int ps_r__rain_drops;
extern ENGINE_API int ps_shells_max;

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
// The grass distance selector (Video, below the quality preset), on the preset's tiers with captions of its own
// (st_opt_<token>). Unset (a user.ltx from before 1.6.2, which had the density row in its place) it takes the
// quality preset's tier, and a new quality preset brings its own (CCC_Preset).
constexpr u32 da_grass_distance_unset = u32(-1);
u32 ps_r__grass_distance = da_grass_distance_unset;
const xr_token qgrass_distance_token[] =
{
    { "grass_distance_minimum", 0 },
    { "grass_distance_low", 1 },
    { "grass_distance_medium", 2 },
    { "grass_distance_high", 3 },
    { "grass_distance_maximum", 4 },
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
    { "gtao",     ssao_mode_gtao },
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


//int ps_r__Supersample = 1;
int ps_r__LightSleepFrames = 10;
// How many shadow-map faces may render per frame; excess faces keep lighting unshadowed.
// The unit is a face, not a lamp: light::Export explodes a shadowed point light into six
// OMNIPART faces, so a scene-wide value below 6 cannot shadow even one world lamp completely.
// 0 = shadow every light, exact CoC/1.0/x86 parity, and the default: shipping 1 in 1.3.2
// dropped every lamp shadow but the nearest face (bar arena went shadowless past a few
// meters) and let NPC headlamps shine through their own wearer's skull. The budget stays as
// an opt-in runtime cvar for weak machines that prefer frame rate over lamp shadows.
int ps_r__light_shadow_budget = 0;
// The lamps' static casters kept between frames (da_smap_cache.cpp): no change to the picture, so on everywhere.
int ps_r__light_shadow_cache = 1;
// GTAO's slices (the preset's tier), its temporal pass and that pass's blend toward the frame (gtao_temporal.ps).
int ps_r__gtao_slices = 3;
int ps_r__gtao_temporal = 1;
float ps_r__gtao_blend = 0.12f;
// The local lights' inverse-square falloff (da_light_falloff.h), look constants: its share, core and gain.
float ps_r__light_falloff = 1.f;
// The decals' normal and gloss (da_wmark_gbuf.ps), look constants: on, the bump of their coverage, the gloss of blood and
// of the rest.
int ps_r__wmark_gbuf = 1;
float ps_r__wmark_bump = 3.f;
float ps_r__wmark_gloss_blood = 0.2f;
float ps_r__wmark_gloss = 0.f;
float ps_r__light_falloff_core = 4.f;
float ps_r__light_falloff_gain = 1.7f;
// Detail objects (grass) in local light shadow maps: an upstream extra the reference never
// drew, and on grassy levels the single biggest per-face cost. It follows the quality preset
// (High and Maximum turn it on, see CCC_Preset) and still needs the grass shadow option
// (r2_sun_details high) like every other grass shadow. Opt-in at runtime for the rest.
int ps_r__light_details = 0;
int ps_r__light_dyn_shared = 1;
// Self-shadowing of the first-person hands and item: their own geometry shades them instead
// of the whole model taking one flat sun value. It uses a small dedicated map rather than a
// world cascade, so the weapon never drops a second shadow on the ground next to the one the
// actor's body and the item's world model already cast. Maximum preset only (see CCC_Preset).
int ps_r__hud_shadow = 0;
int ps_r__actor_shadow = 0;
// Cloud deck tier (da_clouds.h): 0/1 flat deck, 2 volumetric 6 steps, 3 volumetric 12 steps.
int ps_r__clouds_quality = 1;
int ps_r__clouds_debug = 0; // 0 off, 1 the deck transmittance, 2 its raw colour, 3 the reprojection offset
float ps_r__clouds_temporal = 0.85f; // share of the previous frame kept by the march (0 = none)
// r__clouds_quality: pin the tier for tuning and QA; -1 follows the preset.
int ps_r__clouds_quality_override = -1;
// r__clouds_cover: pin the deck's coverage (0..1) for tuning and QA; -1 follows the weather.
float ps_r__clouds_cover = -1.f;
// How far the self-shadow lifts a sample off its own surface, in metres, and how much depth
// slope one shadow texel may carry. Both exist for the same reason the sun has its own depth
// bias pair: the deferred position a first-person pixel reconstructs from is not exactly on
// the surface - the model shaders displace it along the normal by the parallax virtual height,
// up to 5 cm and varying per texel - and a surface lit edge-on changes depth across a texel by
// far more than any fixed epsilon. Together they are what keeps direct sunlight from drawing
// stripes along the shadow map's texel grid on hands and blades. Raise them if a machine still
// shows banding, lower them for tighter contact shadows.
float ps_r__hud_shadow_normal_offset = 0.01f;
float ps_r__hud_shadow_slope_bias = 0.003f;
// Screen-space contact shadows for the near sun pass (see da_sss.h in the shader overlay).
// The sun map covers tens of metres at one resolution, so anything thinner than a texel -
// a grass blade at its root - never reaches it and floats above the ground. A short ray
// marched through the depth buffer toward the sun catches exactly that scale. The strength
// follows the quality preset (see xrRender_sync_preset_derived: on from High); the console
// command overrides it for the session only. Ported from the sibling engine, d13e266;
// length/thickness/steps are its in-game tuned values.
float ps_r__sss = 0.f;
float ps_r__sss_len = 0.35f;
float ps_r__sss_thick = 0.5f;
float ps_r__sss_steps = 8.f;
// Haze (ported from the sibling engine, d56f2a2). The stock fog paints one flat colour over
// the whole frame - the "grey curtain" at the map border, where a far hill and the sky above
// it get the same fill. The haze takes its colour from the sky cubemaps along the (horizon-
// flattened) view direction and adds an exponential height layer with an analytic integral,
// so hollows fill with murk while ridgelines stay clear. All values are their in-game tuned
// ones except r__fog_dist, which stays 1.0 here: the weather's own fog distances are part of
// the Dead Air look and are not rescaled by default.
float ps_r__fog = 1.f;               // master, scales both the sky share and the height layer
float ps_r__fog_sky = 0.8f;          // share of sky colour in the haze
float ps_r__fog_sky_mip = 6.f;       // cubemap blur level for the haze sample
float ps_r__fog_sky_flat = 0.8f;     // horizon flattening of the sample direction
float ps_r__fog_height = 0.5f;       // height-fog density (thousandths per metre of path)
float ps_r__fog_height_falloff = 0.02f; // higher = thinner layer hugging the ground
float ps_r__fog_height_base = -15.f; // reference altitude of the layer, world metres
float ps_r__fog_dist = 1.f;          // multiplies the weather fog_near/fog_far together
float ps_r__fog_follow_vis = 1.f;    // haze follows the visibility-distance slider (ref 1.5)
float ps_r__fog_max = 0.95f;         // density ceiling: keeps hill silhouettes at the horizon
// Luminance-preserving tonemap: the per-channel curve pulls channels together and bleaches
// everything bright; computing it once on luminance keeps colour proportions at any
// brightness, and the late desaturation still sends true overexposure to white (sibling
// engine's in-game tuned values). White point 1.7 equals the old hardcoded constant.
float ps_r__tonemap_hue = 1.f;
float ps_r__tonemap_desat = 8.f;
float ps_r__tonemap_white = 1.7f;
// Colour grade after the tonemap (da_grade in common_functions.h). A restrained,
// camera-like grade rather than a look: overall saturation a notch down, greens a notch
// further and pulled toward olive, a touch of contrast around middle grey. Numbers from the
// current photoreal practice - a grade at 20-60 % strength that keeps highlight and texture
// detail, greens being what oversaturates first; the AgX-style "punchy" adjustments raise
// contrast a little after the desaturation. Part of the look: the preset sync re-applies
// them on every start, the console changes them for the session.
float ps_r__grade_sat = 0.9f;
float ps_r__grade_green = 0.8f;
float ps_r__grade_olive = 0.3f;
float ps_r__grade_contrast = 1.05f;
// The bloom pyramid (r4_rendertarget_phase_da_bloom.cpp): its share of the frame at the menu's bloom slider at its
// default (r2_ls_bloom_kernel_scale 0.7, which scales it), the exposed brightness where it starts and the width over
// which it reaches full weight. Look constants, re-applied by the preset sync.
float ps_r__bloom = 0.3f;
float ps_r__bloom_threshold = 0.5f;
float ps_r__bloom_knee = 0.5f;
// Motion blur (r4_rendertarget_phase_da_mblur.cpp): the option, a matter of taste that no preset touches, and the
// share of a frame's motion it spans (a 180-degree shutter), a look constant.
int ps_r__mblur = 0;
float ps_r__mblur_shutter = 0.5f;
// The sunlit air (r4_rendertarget_phase_froxels.cpp): its density against the weather's shaft strength, a look constant.
float ps_r__froxel_strength = 1.f;
// The player's torch beam in the froxel volume, a look constant (0: none).
float ps_r__froxel_torch = 1.f;
// The GGX specular of the stock materials (lmodel.h), look constants: on, f0 per unit of gloss and its ceiling,
// smoothness per unit of gloss.
int ps_r__spec_ggx = 1;
// Lit particles (da_particle_light.cpp), a look constant: 1 on, 0 the stock unlit ones; QA: 2 the light itself, 3 the
// flipbooks' blend.
int ps_r__particle_light = 1;
// The screen-space glossy reflections (r4_rendertarget_phase_ssr.cpp), a preset feature: 0 off, 1 Low (12 steps a
// ray), 2 High (20).
int ps_r__ssr = 2;
// What hides blade grass from the view (da_grass_blades.cpp), bits: 1 the main view's occlusion (HOM) hides whole tiles,
// 2 this frame's depth hides each lane (the walls, fences and barrels HOM has no shape for), 4 it hides a group's whole
// tile before its lanes read anything. A hidden one grows only its shadow casters. A session knob for QA, never saved:
// user.ltx kept the old r__grass_hom 1.
int ps_r__grass_occlusion = 7;
float ps_r__spec_f0 = 0.8f;
float ps_r__spec_f0_max = 0.16f;
float ps_r__spec_smooth = 3.f;
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
// Seconds a visual takes to appear or vanish at its draw distance, dithered (r__dsgraph_build.cpp); 0 = a pop.
float ps_r__fade_time = 0.35f;
// QA: a share forced on every visual of the main view (0 = off), and frames left to log the fade's counters.
float ps_r__fade_qa = 0.f;
int ps_r__fade_stats = 0;
//float ps_r__LOD_Power = 1.5f;
float ps_r__ssaDISCARD = 3.5f; // RO
// Separate discard threshold for vegetation billboards (FLOD). The far background is filled
// by trees and bushes, and the shared r__ssa_discard drags everything with it - crates,
// pipes, debris; measured in the sibling engine, most of the cost of lowering it is NOT
// vegetation. A vegetation billboard is four vertices - pushing just those to the horizon
// is nearly free. Lower = lives farther; below the shared threshold makes sense, above not.
float ps_r__vegDISCARD = 0.5f;
// Where grass starts shrinking with distance, as a share of its draw radius. 0 = stock:
// shrinking starts at ONE metre and runs to the edge, so at half distance grass is at 3/4
// height and past 70% it drops below a pixel and is discarded by area - "no grass far away".
// Those blades are already paid for (cache, visibility, matrices); this only shows them.
// Preset-derived: the extra fill has a real cost (their measure: +69% pixels = -5 FPS).
float ps_r__grass_fade_start = 0.f;
// GPU pass timings to the log every N frames (0 = off). A diagnostic, never persisted.
int ps_r__gpu_log = 0;
// A screenshot of the finished 3D frame every N frames (0 = off): how the QA rig, which runs
// on a hidden desktop nobody can see, hands back pictures. Never persisted.
int ps_r__screenshot_every = 0;
// Share of the grass fade that goes into HEIGHT instead of uniform shrink. Uniform makes the
// blade smaller in every direction until it is discarded by area and the ground bares out;
// height-only lays the tuft flat while its footprint keeps covering the soil - reads as a
// carpet to the very edge and is cheaper (tall stems cost vertical pixels). Preset-derived.
float ps_r__grass_fade_flat = 0.f;
// Grass shadow distance, metres from the camera. Stock fed ALL visible grass (300 m at
// radius 299) into the 20 m near sun cascade and let the GPU cull it after vertex work -
// the sibling measured sun_smap 5.05 ms -> 2.42 ms with near-only grass.
int ps_r__grass_shadow_dist = 40;
// Alpha-test threshold for foliage/lod (da_aref_u uniform), 0..255. 128 = the historical
// baked literal; the preset ladder lowers it on top tiers (denser leaf silhouettes) and
// raises it on low tiers (fewer shaded foliage pixels).
int ps_r__aref_quality = 128;
// SMAA 1x in the FXAA slot of phase_combine (see phase_smaa): morphological AA over the
// tonemapped LDR frame, three fullscreen passes + two RGBA8 targets. Follows r__aa; the
// console command overrides for the session only and supersedes r2_fxaa while on.
int ps_r__smaa = 0;
// Temporal AA on top of SMAA (phase_taa): a jittered projection resolved against the history,
// reprojected through the motion vectors when they exist. Follows r__aa like the one above.
int ps_r__taa = 0;
// QA view of the motion vectors (phase_velocity_debug) over the finished frame; renders them
// even when nothing temporal runs. Session only.
int ps_r__motion_vectors = 0;
// Anti-aliasing is the player's own setting since 1.5.0, no longer a step of the quality preset:
// the persisted choice the three runtime switches above follow (xrRender_apply_aa). An upscaler
// replaces it while one runs. Until a user.ltx carries it, it is what the player's preset gave
// (FXAA on Minimum/Low, SMAA on Default/High, SMAA + TAA on Extreme) - SMAA on a fresh install,
// whose preset is Default.
constexpr u32 da_aa_unset = u32(-1);
u32 ps_r__aa = da_aa_unset;
const xr_token qaa_token[] =
{
    { "st_aa_off", 0 },
    { "st_aa_fxaa", 1 },
    { "st_aa_smaa", 2 },
    { "st_aa_taa", 3 },
    { nullptr, 0 }
};
// The upscalers, one row each and never both: the token value is the quality the backend gets.
// They take effect on the renderer reset (vid_restart), which rebuilds the targets at the new
// render size and the SDK context.
u32 ps_r__fsr = 0;
const xr_token qfsr_token[] =
{
    { "st_fsr_off", 0 },
    { "st_fsr_native", 1 },
    { "st_fsr_quality", 2 },
    { "st_fsr_balanced", 3 },
    { "st_fsr_performance", 4 },
    { "st_fsr_ultra_performance", 5 },
    { nullptr, 0 }
};
u32 ps_r__dlss = 0;
const xr_token qdlss_token[] =
{
    { "st_dlss_off", 0 },
    { "st_dlss_dlaa", 1 },
    { "st_dlss_quality", 2 },
    { "st_dlss_balanced", 3 },
    { "st_dlss_performance", 4 },
    { "st_dlss_ultra_performance", 5 },
    { nullptr, 0 }
};
// What a DLSS row offers on an adapter that cannot run it: nothing but off.
const xr_token qdlss_off_token[] =
{
    { "st_dlss_off", 0 },
    { nullptr, 0 }
};
// Frame generation (DX12). Stored here; the frame generation stage reads it.
u32 ps_r__framegen = 0;
const xr_token qframegen_token[] =
{
    { "st_fg_off", 0 },
    { "st_fg_fsr", 1 },
    { "st_fg_dlss", 2 },
    { "st_fg_dlss_x3", 3 },
    { "st_fg_dlss_x4", 4 },
    { nullptr, 0 }
};
// FSR's RCAS strength, and the post sharpening DLSS gets; 0 = none.
float ps_r__upscale_sharpness = 0.3f;

// DirectX 12 features (docs/dead-air/dx12-features/PLAN.md 2.6).
// [dx12:dxc] begin variables
int ps_r__dxc = 1;
// [dx12:dxc] end variables
// [dx12:async] begin variables
u32 ps_r__async_compute = 1;
const xr_token qasync_compute_token[] =
{
    { "st_async_off", 0 },
    { "st_async_auto", 1 },
    { "st_async_on", 2 },
    { nullptr, 0 }
};
int ps_r__async_path = 0;
// [dx12:async] end variables
// [dx12:vrs] begin variables
int ps_r__vrs = 0;
int ps_r__vrs_override = -1;
float ps_r__vrs_threshold = 1.f;
int ps_r__vrs_force = 0;
int ps_r__vrs_debug = 0;
int ps_r__vrs_log = 0;
// [dx12:vrs] end variables
// [dx12:gd] begin variables
// 0 CPU path, 1 GPU-driven (wind, cull and draw on the GPU), 2 QA: CPU cull, GPU draw (G1/G2 path),
// -1 auto (da_offload.h), the default: the GPU path saves CPU time and costs GPU time, so it runs only
// while the CPU is the frame's bottleneck (gpudriven.md 3.13).
int ps_r__gpu_driven = -1;
// [dx12:gd] end variables
// [dx12:mesh] begin variables
// Mesh-shader grass: 0 the vertex path, 1 mesh, -1 auto (the default, as r__gpu_driven): mesh grass cuts
// the CPU grass work from 8.6 to 1.8 ms a frame on the foliage save and costs 0.4-0.6 ms of GPU (mesh.md).
int ps_r__grass_mesh = -1;
// QA: 0 draws the stock bushes again in place of the procedural shrubs (SHRUBS.md).
int ps_r__shrubs = 1;
// QA: 0 draws the stock reed models again in place of the blades' reed beds (GRASS.md, reed beds).
int ps_r__reeds = 1;
// QA: draws of the shrubs left out (bits: 1 wood, 2 leaves, 4 blooms, 8 clusters, 16 blobs, 32 every shadow map; the
// leaves one by one: 64 the shaped ones, 128 the cut quads, 256 the near sprays).
int ps_r__dbg_shrubs = 0;
float ps_r__shrubs_lod_scale = 1.f;
int ps_r__shrubs_force_tier = -1;
// QA: 0 draws the stock trees again in place of the upgraded trees (TREES.md).
int ps_r__trees = 1;
// QA: draws of the trees left out (bits: 1 the stock tubes, 2 the stock patches, 4 the grafted wood, 8 the leaves, 32 the
// blooms, 64 the clusters, 128 the blobs, 256 the billboards, 512 every shadow map and the rain map; 1024, 2048, 4096,
// 8192 the trees in the leaves', the thinned leaves', the clusters' or the blobs' tier; 16384 the clusters and the blobs
// unthinned past the floor; 32768 the patches without their runs - their bark's seams: the runs' cost; 65536 the
// clusters' and the blobs' pixels out at once - their pixels' cost; 131072 the stock patches untessellated everywhere).
int ps_r__dbg_trees = 0;
// QA: 0 the trees' view drawn as the CPU cut it, without the GPU cut (r3\da_tree_cut.cs) - an A/B.
int ps_r__trees_cut = 1;
// The trees' cut culls by the depth drawn so far (da_veg.cpp cut_dispatch: the opaque world, then the near trees); 0 for QA.
int ps_r__trees_occlusion = 1;
// QA: the vegetation's soft switches (bits, all on by default; off: the hard switch, an A/B) - 1 the thinned prefixes'
// soft ends (da_veg.cpp emit_thinned), 2 the crowns' wood prefixes' (da_veg_internal.h wood_runs), 4 the trees' stock
// wood morphs (the ring levels and the patches' tessellation, da_trees.cpp).
int ps_r__veg_soft = 7;
// QA: the wood's bark in false colour (TREES.md; da_shrub_wood.ps, its main's last block and shrub_dbg_*): 1 what draws
// it, 2 its counts of tiles round the stem, 3 the upper bark's zone, 4 the first mapping's checker, 5 the second's.
int ps_r__dbg_wood = 0;
float ps_r__trees_lod_scale = 1.f;
// QA: the crowns' fine detail - the leaves' floor past the quads and the leafy crowns' wood floors divided by it (1 the
// preset's; the floors never under the pixel floor and the wire rule).
float ps_r__trees_detail = 1.f;
// QA: the thinned leaves' and the clusters' tiers end at their leaf pixels divided by it.
float ps_r__trees_far_detail = 1.f;
// QA: the shrubs' leaves end at their pixel floors over it.
float ps_r__shrubs_far_detail = 1.f;
int ps_r__trees_force_tier = -1;
int ps_r__dbg_grass_ab = 0;
// [dx12:mesh] end variables
// The QA view of the snow passes (da_snow_common.h): 0 off, 1 the paths in colour, 2 also without the grid's cuts.
int ps_r__dbg_snow = 0;
// The snow's glints (da_snow_layer.ps): 1 as built, 0 none.
float ps_r__snow_glint = 1.f;
// QA: the snow shells on roofs and covers, and the snow grid in the near sun cascade (1 as built, 0 off).
int ps_r__snow_shells = 1;
int ps_r__snow_shadow = 1;
int ps_r__dbg_grass = 0;
// [dx12:offload] begin variables
float ps_r__offload_margin = 0.1f;
float ps_r__offload_agree = 3.f;
float ps_r__offload_dwell = 10.f;
float ps_r__offload_recall = 120.f;
float ps_r__offload_smooth = 1.f;
float ps_r__offload_warmup = 30.f;
// [dx12:offload] end variables
// [dx12:ds] begin variables
// [dx12:ds] end variables
// [dx12:rt] begin variables
int ps_r__rt_reflections = 0;
int ps_r__rt_shadows = 0;
int ps_r__rtao = 0;
u32 ps_r__rt_quality = 1;
const xr_token qrt_quality_token[] =
{
    { "st_rt_low", 0 },
    { "st_rt_medium", 1 },
    { "st_rt_high", 2 },
    { nullptr, 0 }
};
int ps_r__rt_debug = 0;
// [dx12:rt] end variables
// Fade band width before that cut-off, metres; 0 = hard edge (stock). Blades lie down over
// the band so their shadows shorten into nothing instead of popping at a moving circle.
int ps_r__grass_shadow_fade = 10;
int ps_r__fire_fluid = 0;
float ps_r__fire_fluid_dist = 60.f;
int ps_r__fire_fluid_grids = 1;
// World-position brightness variation of grass, 0 = off. Distant grass reads as one flat
// fill; noise keyed to the WORLD position of each tuft (stable under camera motion) breaks
// it into a ground-like pattern. Brightness only - true colour matching needs a terrain
// colour map we don't have. Vertex-shader cost is negligible, so on for every preset.
float ps_r__grass_tint = 0.12f;
// Patch size of that variation, metres. Small reads as noise, large as soil unevenness.
float ps_r__grass_tint_scale = 12.f;
// How much stronger the variation is at the BASE of the stem than at the tip. The stem takes
// soil properties at the roots and stays itself at the tip - kills the grass/ground seam.
float ps_r__grass_tint_base = 1.f;
// Rain puddles (G-buffer half in deffer_impl_flat.ps, reflections in da_puddle_refl.ps).
// The wetness accumulator lives in the rain_params binder (r2.cpp): any rain drives
// wetness towards one, strength only sets the SPEED - DA weather rains at 0.1-0.3 most of
// the time and an intensity-capped accumulator would never form a puddle.
int ps_r__puddles = 1;
float ps_r__puddles_buildup = 90.f; // seconds of rain to full wetness
float ps_r__puddles_dry = 4.f; // drying takes this many times longer
// Puddle look. These are look constants, not quality tiers: the preset sync re-applies them
// on every start, so a user.ltx line from an earlier build (size 0.8, dark 1.0, reflection
// 1.5, fresnel floor 0.1, sky 0.15 - a bright overcast noon turned every puddle into a sheet
// of white) cannot pin them; the console changes them for the session.
float ps_r__puddles_size = 0.60f; // surface share under water at full wetness
float ps_r__puddles_force = 0.f; // debug: hand-set wetness, accumulator bypassed
float ps_r__puddles_gloss = 1.00f;
float ps_r__puddles_dark = 0.65f; // soil under water is darker than dry soil; the gloss alone read as snow under a bright sky
float ps_r__puddles_damp = 0.10f; // wet-ground gloss, wider and weaker than puddles
float ps_r__puddles_ripple = 1.f;
int ps_r__puddles_debug = 0;
int ps_r__puddles_dist = 20; // metres, puddles fade out on the last quarter; rides the preset
int ps_r__puddles_gbuf = 0; // 1 = flat water normal in the G-buffer: one sun highlight over a whole puddle at noon; 0 = ground normal, glint in the reflection pass
float ps_r__puddles_edge = 0.3f; // edge hardness, tuned with the straight-up water normal
float ps_r__puddles_rim = 0.72f; // dark soaked-soil rim around water
float ps_r__puddles_rim_width = 0.22f;
int ps_r__puddles_refl = 1; // 0 = none, 1 = sky only (no ray), 2 = world ray-march; rides the preset
float ps_r__puddles_refl_power = 1.0f; // the reflection is what the fresnel says it is
float ps_r__puddles_facing = 0.03f; // Schlick F0 of water is 0.02; the lift keeps a top-down puddle from going black
float ps_r__puddles_sky = 1.0f; // a ray that finds no geometry sees the sky: full share, the fresnel does the rest
// Steep parallax (POM) family, sibling's in-game tuned values. The stock numbers were
// hardcoded in sload.h (25/5 samples, 0.013 depth, 8..12 m fade).
float ps_r__parallax_start = 8.f; // metres: full-strength relief up to here
float ps_r__parallax_stop = 12.f; // metres: gone entirely
float ps_r__parallax_depth = 0.0105f;
// Self-shadow density: a sun-ray march over the same height map. The single largest visual
// gain in the POM shader - without it parallax reads as "the texture swims".
float ps_r__parallax_shadow = 4.f;
int ps_r__parallax_samples = 32;
int ps_r__parallax_samples_min = 2;
int ps_r__parallax_shadow_samples = 8;
// Force POM onto every surface that HAS a height map. Sibling's census over 5985 .thm:
// 112 textures ask for parallax, 2020 carry a height map - the hand-set editor flag never
// reached most of them. Needs a level reload (shader names build once per blender).
int ps_r__parallax_force = 1;
// Debug: 1 = self-shadow, 2 = height at hit, 3 = every pixel the branch reached.
int ps_r__parallax_debug = 0;
// Distance (metres from camera) where the far sun shadow has fully dissolved into light.
// Replaces the stock map-edge fade whose border MOVES with every camera turn (the
// travelling shadow "wedge"). The preset puts it at 7/8 of the far cascade box, before its edge.
float ps_r__sun_shadow_fade = 140.f;
// The middle and far sun cascade boxes, metres; the near one stays at 20. The preset's.
float ps_r__sun_cascade_mid = 40.f;
float ps_r__sun_cascade_far = 160.f;
// The seam between two sun cascades: the outer share of a map's square whose pixels pass by dither to the next map
// (accum_sun_near.ps da_sun_cascade_seam; 0 a hard seam).
float ps_r__sun_cascade_blend = 0.2f;
// Tint sun light per cascade (near=R, middle=G, far=B) to see cascade borders in place.
int ps_r__dbg_sun_cascades = 0;
// Particle effect draw distance, metres from the camera; 0 = no limit (stock). Culled
// before the particle fetch and buffer lock so a skipped effect costs nothing. Sibling's
// Jupiter measure: 585 effects / 0.72 ms unlimited vs 434 / 0.53 ms at 200 m.
int ps_r__particle_dist = 150;
// Terrain far-field family (deffer_impl_flat.ps). Every zero = stock path.
// Far ground brightness variation: mips average the detail into one flat fill; a coarse
// re-read keeps large patches alive to the horizon. Sibling's in-game tuned values.
float ps_r__macro_var = 0.5f;
float ps_r__macro_var_scale = 16.f; // how many times coarser the re-read tiles
float ps_r__macro_var_start = 40.f; // metres: where the variation starts growing
float ps_r__macro_var_end = 250.f; // metres: full strength
// "Fake grass": pull far ground hue towards the (four-layer) detail mix, luminance kept.
float ps_r__macro_tint = 0.6f;
// Far macro relief: coarse detail normal mixed in so distant slopes catch the sun.
float ps_r__macro_relief = 0.1f;
// Height-based layer splatting instead of linear masks; luminance stands in for height.
float ps_r__terrain_blend = 1.f;
// Mask uv jitter against visible low-res mask gradients at distance (mask uv units).
float ps_r__mask_jitter = 0.005f;
// Foliage specular multiplier (deffer_base_aref_bump.ps). The engine's specular hits the
// combine as C.www*L.rgb*5 - additively WHITE - which lays a bleached film on conifer crowns
// under direct sun; no tint fixes added white light. Sibling tuned it to zero in game.
float ps_r__foliage_gloss = 0.f;
// Foliage saturation multiplier (Vibrance() in the shader is a plain lerp from luminance):
// 1 = the texture as is. The sibling's 1.6 was tuned for the old, dull foliage textures;
// the HD set carries its own saturation and 1.6 on top read as plastic greens.
float ps_r__foliage_vibrance = 1.1f;
// Bleached branches: damp bright-AND-colourless in foliage albedo. Found with their light
// probe: the whiteness survives with specular fully off, i.e. it lives in the albedo itself.
float ps_r__foliage_debleach = 0.6f;
// Distant-vegetation billboard shading (lod.ps, da_lod_tune). H.w in the billboard bake is
// the sky-light share captured under an OPEN sky, while a live crown shades itself - so
// untouched impostors read washed-out and pop at the swap line. Saturation and brightness
// recover what the small bake texture ate; 1/1/1 = stock. Sibling's in-game tuned values.
float ps_r__lod_hemi = 2.f;
// Impostor saturation follows the crown: 2.0 matched crowns at vibrance 1.6, 1.4 at 1.1.
float ps_r__lod_sat = 1.4f;
float ps_r__lod_bright = 1.f;
float ps_r__ssaDONTSORT = 32.f; // RO
float ps_r__ssaHZBvsTEX = 96.f; // RO

// 16 rather than the stock 8: anisotropy samples along the long axis of the pixel
// footprint, which is what keeps ground planes at grazing angles sharp instead of mushy.
// The cost on any GPU of the last decade is noise.
int ps_r__tf_Anisotropic = 16;
float ps_r__tf_Mipbias = 0.0f;

int ps_r__clear_models_on_unload = 1; // Alundaio

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
int ps_r2_GI_depth = 1; // 1..5
int ps_r2_GI_photons = 16; // 8..64
float ps_r2_GI_clip = EPS_L; // EPS
float ps_r2_GI_refl = .9f; // .9f
float ps_r2_ls_depth_scale = 1.00001f; // 1.00001f
float ps_r2_ls_depth_bias = -0.0003f; // -0.0001f
float ps_r__smap_slope_bias = 2.0f;
int ps_r__smap_depth_bias = 1;
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

float ps_r2_dhemi_sky_scale = 0.08f; // 1.5f
float ps_r2_dhemi_light_scale = 0.2f;
float ps_r2_dhemi_light_flow = 0.1f;
int ps_r2_dhemi_count = 5; // 5
int ps_r2_wait_sleep = 0;
int ps_r2_wait_timeout = 500;

// Dynamic-object hemi adaptation rate (time-based lerp factor). The reference ships 1.0,
// which settles in about three seconds and reads as an NPC slowly "charring" after stepping
// into shade - reported by players as a defect. 4.0 settles in under a second and stays
// smooth; r2_dhemi_smooth remains tunable, 1.0 restores the reference pace.
float ps_r2_lt_smooth = 4.f; // reference: 1.f
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
// The visor's dirt glowing round bright light. Not an option: it is on whenever a mask is up
// (phase_combine gates it on the glass), and this only turns it off for the session.
int ps_r2_lensdirt = 1;
// Droplets on the visor. It is a user-facing checkbox (configs\ui\ui_mm_opt*.xml,
// check_lenswater), so the preset only moves it when the player applies a preset - but the
// stock default of 0 meant USE_LENS_WATER was not even compiled and every driver of it, the
// actor's surfacing accumulator included, wrote into a dead float. The default is the Default
// preset's rung.
int ps_r2_lenswater = 1;
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

#if defined(USE_RENDERDOC)
#include <renderdoc/renderdoc_app.h>
// One-shot in-app RenderDoc capture of the NEXT frame - catching a specific broken frame
// (stale constants, blacked-out lamps) without alt-tabbing to the RenderDoc UI.
class CCC_RdocCapture final : public IConsole_Command
{
public:
    CCC_RdocCapture(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr /*args*/) override
    {
        extern RENDERDOC_API_1_0_0* g_renderdoc_api;
        if (g_renderdoc_api)
        {
            g_renderdoc_api->TriggerCapture();
            Msg("* [rdoc] capture of the next frame triggered");
        }
        else
            Msg("! [rdoc] renderdoc.dll is not loaded (launch through RenderDoc or drop the dll nearby)");
    }
};
#endif

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

// Same for the look knobs: a session override, never a user.ltx line the look has to fight.
class CCC_RuntimeFloat final : public CCC_Float
{
public:
    CCC_RuntimeFloat(pcstr name, float* target, float minimum, float maximum)
        : CCC_Float(name, target, minimum, maximum)
    {
    }

    void Save(IWriter*) override {}
};

#if RENDER == R_R4
// r__aa: the persisted choice, applied to the passes at once (they switch live).
class CCC_AA final : public CCC_Token
{
public:
    CCC_AA(pcstr name, u32* target, const xr_token* tokens) : CCC_Token(name, target, tokens) {}

    void Execute(pcstr args) override
    {
        const u32 before = *value;
        CCC_Token::Execute(args);
        xrRender_apply_aa();
        if (*value != before)
            da_history_request_reset("anti-aliasing changed");
    }
};

// r__fsr / r__dlss: choosing one switches the other off. The new render size and SDK context
// arrive with the renderer reset (vid_restart).
class CCC_Upscaler final : public CCC_Token
{
    u32* other;
    const bool dlss;

public:
    CCC_Upscaler(pcstr name, u32* target, const xr_token* tokens, u32* other_upscaler, bool is_dlss)
        : CCC_Token(name, target, tokens), other(other_upscaler), dlss(is_dlss) {}

    void Execute(pcstr args) override
    {
        const u32 before = *value;
        CCC_Token::Execute(args);
        if (*value)
            *other = 0;
        if (*value != before)
            da_upscaler_settings_changed();
    }

    const xr_token* GetToken() noexcept override
    {
        // An adapter known not to run DLSS gets a row with off alone, so neither the menu nor a
        // user.ltx line can select it.
        if (dlss && da_upscaler_support(UpscalerKind::DLSS) == 0)
            return qdlss_off_token;
        return tokens;
    }
};

// r__caps: what this adapter and module can do, for the options menu
// (get_console():get_string("r__caps")). Read-only and never saved.
bool da_renderdoc_capture(); // D3DXRenderBase.cpp

class CCC_GpuCapture final : public IConsole_Command
{
public:
    CCC_GpuCapture(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        if (da_renderdoc_capture())
            Msg("* [renderdoc] the next frame is captured");
        else
            Msg("! [renderdoc] RenderDoc is not in the process");
    }
};

class CCC_RenderCaps final : public IConsole_Command
{
public:
    CCC_RenderCaps(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr) override
    {
        TStatus status;
        GetStatus(status);
        Msg("- %s %s", cName, status);
    }

    void GetStatus(TStatus& S) override
    {
        const UpscalerCaps caps = da_upscaler_caps();
        // snprintf: a status that grew past TStatus is cut and reported once, never a crash.
        const int length = std::snprintf(S, sizeof(TStatus),
            "api=%u;dx12=%d;vk=%d;dlss=%d;dlssg=%d;dlssg_max=%u;fsr=%d;fsrfg=%d;hags=%d;rt=%d;rt_tier=%u;vrs_tier=%u;"
            "vrs_tile=%u;vrs=%d;ms=%d;sm=%u;wave=%u;fp16=%d;eb=%d;acq=%d;dxc=%d;gd=%d;dstorage=%d",
            caps.api, caps.dx12 ? 1 : 0, caps.vk ? 1 : 0, caps.dlss ? 1 : 0, caps.dlssg ? 1 : 0, caps.dlssg_max, caps.fsr ? 1 : 0,
            caps.fsrfg ? 1 : 0, caps.hags ? 1 : 0, caps.rt ? 1 : 0, caps.rt_tier, caps.vrs_tier, caps.vrs_tile,
            caps.vrs ? 1 : 0, caps.ms ? 1 : 0, caps.sm, caps.wave, caps.fp16 ? 1 : 0, caps.eb ? 1 : 0,
            caps.acq ? 1 : 0, caps.dxc ? 1 : 0, caps.gd ? 1 : 0, caps.dstorage ? 1 : 0);
        static bool reported = false;
        if (length >= int(sizeof(TStatus)) && !reported)
        {
            reported = true;
            Msg("! r__caps: the status needs %d characters, %u fit", length, u32(sizeof(TStatus) - 1));
        }
    }

    void Save(IWriter*) override {}
};

// A QA command of a DirectX 12 feature: runs the feature's function, has no value, is never saved.
class CCC_DaCall final : public IConsole_Command
{
    void (*m_call)();

public:
    CCC_DaCall(pcstr name, void (*call)()) : IConsole_Command(name), m_call(call) { bEmptyArgsHandled = true; }

    void Execute(pcstr) override { m_call(); }
    void Save(IWriter*) override {}
};

// A QA command with one argument, never saved.
class CCC_DaCallArg final : public IConsole_Command
{
    void (*m_call)(pcstr);

public:
    CCC_DaCallArg(pcstr name, void (*call)(pcstr)) : IConsole_Command(name), m_call(call) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override { m_call(args); }
    void Save(IWriter*) override {}
};
#endif // RENDER == R_R4

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

// Frame-sequence video capture: "r__capture <seconds> [fps]" starts recording JPEG frames to
// $screenshots$\capture_<timestamp>\, "r__capture 0" (or no args) stops early. Dev tool for
// analysing motion; state lives in globals read by CRender::VideoCaptureTick.
// "r__capture <seconds> <fps> cine" is the footage mode: the device timers step 1/fps a frame
// (g_frame_clock_dt) and every frame is saved near-lossless, so motion is smooth at any frame rate.
float ps_r__capture_stop_at = 0.f;
float ps_r__capture_fps = 20.f;
u32 ps_r__capture_cine_frames = 0;
#if defined(USE_DX11)
float video_capture_clock(); // dx11r_screenshot.cpp
#else
static float video_capture_clock() { return Device.fTimeGlobal; }
#endif

class CCC_VideoCapture final : public IConsole_Command
{
public:
    CCC_VideoCapture(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        float seconds = 0.f, fps = 20.f;
        char mode[16]{};
        const int parsed = sscanf(args, "%f %f %15s", &seconds, &fps, mode);
        if (parsed < 1 || seconds <= 0.f)
        {
            if (ps_r__capture_stop_at > 0.f)
            {
                // Force the tick to run its completion branch on the next frame.
                if (ps_r__capture_cine_frames)
                    ps_r__capture_cine_frames = 1;
                ps_r__capture_stop_at = video_capture_clock();
                Msg("* [capture] stop requested");
            }
            else
                Msg("* [capture] usage: r__capture <seconds 1..120> [fps 5..60] [cine]");
            return;
        }
        const bool cine = parsed >= 3 && !xr_stricmp(mode, "cine");
        clamp(seconds, 1.f, cine ? 900.f : 120.f);
        clamp(fps, 5.f, cine ? 120.f : 60.f);
        ps_r__capture_fps = fps;
        ps_r__capture_stop_at = video_capture_clock() + seconds;
        ps_r__capture_cine_frames = cine ? u32(seconds * fps + 0.5f) : 0;
        if (cine)
            g_frame_clock_dt = 1.f / fps;
        Msg("* [capture] recording %.1fs at %.0f fps%s", seconds, fps, cine ? " (cine: frame clock, every frame)" : "");
    }
    void Info(TInfo& info) override { xr_strcpy(info, "record a frame sequence: <seconds> [fps]"); }
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
        case ssao_mode_gtao:
        {
            // GTAO is a separate pre-pass: every inline combine_1 technique goes off.
            // ps_r_ssao stays nonzero - it is also the master AO switch (position-target
            // clear in phase_scene_prepare, SSAO_QUALITY define).
            if (ps_r_ssao == 0)
            {
                ps_r_ssao = 1;
            }
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HBAO, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HDAO, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_OPT_DATA, 0);
            ps_r2_ls_flags_ext.set(R2FLAGEXT_SSAO_HALF_DATA, 0);
            break;
        }
        }
    }
};

//-----------------------------------------------------------------------
// The preset-derived switches. None of them serialize into user.ltx on purpose -
// the preset is their single source of truth - so besides CCC_Preset they are re-applied
// whenever the renderer comes up: any way a value gets lost or overwritten mid-session
// (a script, a console line, a stale options control) heals on the next start instead
// of surviving as "weapon shadows stopped working" with a Maximum preset on screen.
//
// A few of them used to be held back on renderer start (shadow map size, AO technique, grass
// density and radius, visor droplets) because the options menu also showed them, and following
// the preset would have undone a choice the player had just made. The advanced video page does
// not show anything the preset owns any more, so there is no such choice to protect and no
// reason to treat those five differently from the rest.
void xrRender_sync_preset_derived()
{
    // The shadow-map budget follows the preset: it caps how many local light faces
    // keep their shadows in one frame, the rest light unshadowed. Maximum keeps the
    // reference unlimited behavior.
    static constexpr int budget_by_preset[] = {1, 16, 32, 48, 0};
    // Grass in local light shadow maps is a High/Maximum feature: it was the costliest
    // part of a shadowed lamp on grassy levels, so the lower presets keep the reference
    // behaviour (sun cascades only).
    static constexpr int light_details_by_preset[] = {0, 0, 0, 1, 1};
    // Weapon self-shadowing is a Maximum-only feature: every frame the sun is up it draws
    // the first-person hands and item once more into their own depth map, which the lower
    // presets should not pay for.
    static constexpr int hud_shadow_by_preset[] = {0, 0, 0, 0, 1};
    // The player's own world shadow starts at Medium: it is one more full-body caster in
    // every shadow map, which the two lowest presets should not pay for.
    static constexpr int actor_shadow_by_preset[] = {0, 0, 1, 1, 1};
    // Spent cartridge cases lying about at once: past the count the farthest resting one goes.
    static constexpr int shells_by_preset[] = {32, 64, 128, 192, 256};
    ps_shells_max = shells_by_preset[ps_Preset];
    // The fluid campfire: the two top presets simulate the nearest fires on the 3D grid.
    static constexpr int fire_fluid_by_preset[] = {0, 0, 0, 1, 1};
    ps_r__fire_fluid = fire_fluid_by_preset[ps_Preset];
    // How far and how many: a campfire is some fifteen pixels tall at 60 m in 1440p, past that the marched flame
    // stands in. One grid simulates (0.8 ms a step), the nearest fire's; every other campfire in range draws a copy of
    // its volume (a few hundredths of a millisecond each).
    static constexpr float fire_fluid_dist_by_preset[] = {25.f, 25.f, 25.f, 60.f, 60.f};
    static constexpr int fire_fluid_grids_by_preset[] = {1, 1, 1, 1, 1};
    ps_r__fire_fluid_dist = fire_fluid_dist_by_preset[ps_Preset];
    ps_r__fire_fluid_grids = fire_fluid_grids_by_preset[ps_Preset];
    // Screen-space contact shadows join at High: an 8-step depth ray per lit pixel of the
    // near sun pass. Slightly stronger on Maximum; below High the two lowest-cost presets
    // keep the reference look. Not full strength on purpose - the technique's stepping
    // noise shows at 1.0, and 0.6-0.7 reads as shadow, not as dirt.
    static constexpr float sss_by_preset[] = {0.f, 0.f, 0.f, 0.6f, 0.7f};
    // Step count of that ray: the band where neighbouring pixels disagree on a hit is one
    // step wide, so at 8 steps over 35 cm it is 4 cm of rough edge - Maximum halves it.
    static constexpr float sss_steps_by_preset[] = {8.f, 8.f, 8.f, 8.f, 16.f};
    // Water screen-space reflections ladder. The tier IS the reflection distance: REFL_RANGE in
    // ogse_reflections.h cuts the reflection off past 150/250/350/500 metres of eye depth for
    // tiers 1..4 (100/140/180/220 before), and the sample count along the ray follows it
    // (20/32/40/64) so the far end does not thin out. Reported from the field as reflections ending too close: the fourth
    // tier exists, the video menu offers it by name, and no preset ever selected it - Maximum
    // sat on the same 180 m as High. It has it now; Minimum stays on the plain cubemap.
    // A shader-options change, so it applies on renderer (re)start like the token itself.
    static constexpr u32 water_refl_by_preset[] = {0, 1, 2, 3, 4};
    // Wave rows the water surface evaluates, of the eight the engine solves. Each row is a sin,
    // a cos and a tanh per water pixel, and the tail rows are the short steep ones - dropping
    // them costs texture, not silhouette, because none of this displaces geometry anyway.
    static constexpr int water_waves_by_preset[] = {2, 4, 6, 8, 8};
    // The ripple field: a spectral (FFT) water solver over a 32 m window around the camera, on
    // every tier - a ring is part of the water, not a feature to buy - with the preset only
    // deciding how fine the grid is: 12.5 cm texels on the two bottom tiers, 6 in the middle,
    // 3 on the top. Powers of two, the transform needs them. Takes effect on renderer restart -
    // the targets are created at the size named.
    static constexpr int water_ripple_by_preset[] = {256, 256, 512, 512, 1024};
    // What the camera below a water surface gets: 1 tint, 2 +fog, 3 +warp, 4 full. Even the
    // cheapest tier has to have SOMETHING - the stock behaviour is a hole in the world.
    static constexpr int water_underwater_by_preset[] = {1, 2, 3, 3, 4};
    // Sun caustics under the field, from Default: the surface's own three layers re-read per
    // lit pixel of the sun pass under water, which is exactly the kind of per-pixel cost the
    // two bottom tiers must not pay.
    static constexpr int water_caustics_by_preset[] = {0, 0, 1, 1, 1};
    // Puddle placement: 0 = the value-noise mask, 1 = the fill map the rain occlusion pass
    // produces. The fill costs a few dozen iterations on an existing tile, once per rain tick,
    // so it starts at Default rather than at the top.
    static constexpr int puddle_fill_by_preset[] = {0, 0, 1, 1, 1};
    // Rain: 1 base, 2 oriented splashes, 3 +streak lighting, 4 full. Everything that costs
    // nothing (the cover test, the splash lifetime, the drop size distribution) is unconditional
    // and is not on this ladder at all.
    static constexpr int rain_quality_by_preset[] = {1, 1, 2, 3, 4};
    // Visor droplets: a post effect in the combine, so it joins at Default with the rest of the
    // post stack. It is a shader option (USE_LENS_WATER), so it lands on the vid restart.
    static constexpr int lenswater_by_preset[] = {0, 0, 1, 1, 1};
    // Screen-space sun shafts (r2_sss_enable - not the contact shadows above): five full-screen
    // passes. Measured on the rig at 1080p, Extreme: +0.63 ms median in the combine, which a
    // weak card pays several times over, so they join at Default with the rest of the post
    // stack. A shader option (USE_SSHAFTS), so it lands on the renderer (re)start.
    static constexpr int sun_shafts_ss_by_preset[] = {0, 0, 1, 1, 1};
    // How wide the visor's drop field is. The height follows the screen's aspect, so this is the
    // resolution of the glass ITSELF, and it is what decides whether a drop can be a drop: across
    // the 12 cm of visor the screen spans a cell is 0.23 mm at 512 and 0.12 at 1024, so the
    // one-millimetre beads that cover a rainy window are four cells wide on the first and eight
    // on the second. Two cells is a blob with no rim and no roundness, and no shading recovers it.
    //
    // The step follows the grid rather than the ladder: the transport is a flux bound by the
    // Courant condition, so a finer grid needs a proportionally shorter step to carry the same
    // drop at the same speed. phase_visor_drops derives it, and the water runs at the same
    // millimetres a second on every tier. Create-time.
    static constexpr int visor_drops_by_preset[] = {256, 384, 512, 768, 1024};
    // Grass distance-fade rework: the extra far-grass fill has a measured frame cost
    // (+69% grass pixels at 0.95 in the sibling engine), so the start point climbs with
    // the preset. Minimum keeps the stock fade-from-one-metre.
    static constexpr float grass_fade_by_preset[] = {0.f, 0.5f, 0.7f, 0.95f, 0.95f};
    // With a late fade start the remaining band is short, so on the top presets the fade
    // spends half its shrink on height alone - the footprint keeps covering the soil.
    static constexpr float grass_flat_by_preset[] = {0.f, 0.f, 0.f, 0.5f, 0.5f};
    // Rain puddles ladder: 1 = G-buffer puddles (noise + normal/gloss on terrain pixels within
    // the distance below - arithmetic only, so every tier has them), 2 = plus a sky reflection
    // (the reflection pass without the depth march), 3 = the world ray-march while wet.
    static constexpr u32 puddles_by_preset[] = {1, 1, 2, 3, 3};
    // Puddle draw distance ladder, metres, from the 20 every preset had: the mask and the
    // reflection cost per covered pixel, and the far pixels are the cheap ones.
    static constexpr int puddles_dist_by_preset[] = {20, 40, 60, 90, 120};
    // Grass density, one for every tier (lower = denser: the stock cell grid is iCeil(2/density)+1 squared, the
    // blades scale as 1/density). It is the Default tier's, and the grass distance row trades what a denser field
    // cost for reach: the density ladder of 1.6.1 (0.6 / 0.6 / 0.5 / 0.35 / 0.25) drew a blade count that a
    // longer radius at this density draws now.
    constexpr float detail_density = 0.5f;
    // Grass draw radius by the grass distance row, metres, from the stock 49 every preset had. The blades reach
    // half again as far (da_grass_blades.cpp, kReach), their cover easing down to 0.85 at the radius. Each tier draws
    // the records its density of 1.6.1 drew at the Extreme preset's 160 m (0.6 / 0.5 / 0.35 / 0.25), the open view
    // over the l01_escape meadow: Default 105 m, High 133 m, Maximum 200 m (a 300 m reach). Minimum and Low stay
    // below their 0.6's records, for the machines that pick them. The blades follow at once, the stock detail cards
    // at the next details reload (CCC recomputes dm_*).
    static constexpr int detail_radius_by_tier[] = {60, 90, 105, 133, 200};
    // The static geometry's distance cut (r__dsgraph_build.cpp): Minimum keeps the old default, which dropped a
    // crate or a barrel at 50 m; from Default on none, as DOOM Eternal culls by screen size alone. Over the
    // rookie village the cut saved nothing measurable.
    static constexpr u32 optimize_static_by_preset[] = {
        geometry_optimization_medium, geometry_optimization_low, geometry_optimization_off,
        geometry_optimization_off, geometry_optimization_off};
    ps_r_optimize_static = optimize_static_by_preset[ps_Preset];
    // Wet-surface radius ladder, metres (r3_dynamic_wet_surfaces_far semantics). The pair of
    // console knobs existed but the shader hardcoded 5/20 - now that they are live, the far edge
    // rides the preset. Near stays at its 5 m default.
    static constexpr float wet_far_by_preset[] = {20.f, 30.f, 45.f, 65.f, 90.f};
    // The rain occlusion map covers that radius: its resolution rides along so a roof edge
    // stays a roof edge at the far end. Applied at start (the surface is created then).
    static constexpr int wet_sm_res_by_preset[] = {256, 512, 1024, 1024, 2048};
    // Alpha-ref ladder (donor rspec values with Default pinned to our historical 128): lower =
    // denser foliage silhouettes = more shaded pixels; Minimum/Low trim exactly there.
    static constexpr int aref_by_preset[] = {180, 160, 128, 110, 100};
    // Every distance below follows one rule: Minimum sits exactly where the distance was before it
    // had a ladder - the value every preset used to share - and each preset above goes further,
    // up to what current games ship: sun shadows to 200 m of cascades in Unreal's default, 500 m in
    // Unity HDRP's, the horizon with virtual shadow maps; grass to 100-200 m in open worlds; wet
    // ground, puddles and water reflections as far as the eye goes.
    //
    // How far the sun casts. The game runs the cascaded path (the shaders are cascade shaders -
    // must_enable_old_cascades), where the reach is the far cascade's box and the distance fade
    // inside it, and those were a fixed 160 m and 140 m on every preset; r2_sun_far only moves
    // the old single-map path. The fade ends at 7/8 of the far box, where the tuned 140/160 pair
    // had it. Measured at 1080p, Extreme: the longer boxes cost +0.1 ms in the sun pass, a 4096
    // map on top of them +2.9 ms - so the maps stay as they were.
    //
    // The middle box is exactly as far as grass casts (grass_shadow_by_preset below), because grass
    // shadows are drawn into the near and middle maps only. In the far map a texel is a third of a
    // metre, a blade a few centimetres, and the wind moves it by less than a texel: the shadow
    // there is a block that switches on and off, and a patch of ground at middle distance
    // flickered in sun and wind - from 1.4.2, which sent grass shadows past the middle box.
    static constexpr float sun_cascade_mid_by_preset[] = {40.f, 50.f, 60.f, 75.f, 95.f};
    static constexpr float sun_cascade_far_by_preset[] = {160.f, 200.f, 240.f, 300.f, 360.f};
    static constexpr u32 smapsize_by_preset[] = {1024, 1536, 2048, 2048, 2048};
    // The old single-map path (r2_shadow_cascede_old): its one far map, pinned at 100 before.
    static constexpr float sun_far_by_preset[] = {100.f, 150.f, 200.f, 250.f, 350.f};
    // How far grass casts into those cascades, metres. Pinned at 40 before. Grass is fed per blade
    // into the near pass, so this is the one genuinely expensive shadow distance (the sibling
    // measured 5.05 ms -> 2.42 ms cutting it back). Equal to the middle box above, rung for rung.
    static constexpr int grass_shadow_by_preset[] = {40, 50, 60, 75, 95};
    // AO technique ladder. GTAO (ported from IX-Ray: the horizon integral plus a guided filter, gathered
    // over the frames by its temporal pass) from Low up, its slices by the preset - Low the cheap tier of
    // one, which the temporal pass turns into the donor's three; Minimum keeps AO off as before. Applied
    // through the console command so the R2FLAGEXT_SSAO_* side effects stay in CCC_SSAO_Mode.
    static constexpr pcstr ssao_mode_by_preset[] = {"disabled", "gtao", "gtao", "gtao", "gtao"};
    static constexpr int gtao_slices_by_preset[] = {1, 1, 2, 3, 3};
    ps_r__gtao_slices = gtao_slices_by_preset[ps_Preset];
    ps_r__gtao_temporal = 1;
    ps_r__gtao_blend = 0.12f;
    // [dx12:vrs] begin preset
    // Variable rate shading on DirectX 12, conservative on High and Extreme only: it pays where the
    // frame is pixel-bound (Extreme 1440p +3.9 %) and costs its rate image on the light presets
    // (Low aggressive -1.9 %, Default conservative -1 %; vrs.md, final test phase).
    static constexpr int vrs_by_preset[] = {0, 0, 0, 1, 1};
    // [dx12:vrs] end preset
    // [dx12:mesh] begin preset
    // The grass radius when mesh grass runs this session (mesh.md 3.9): what the same frame cost buys through mesh
    // shaders, set by the mesh lane's ladder sweep (M10), at the one density above.
    static constexpr int detail_radius_mesh_by_tier[] = {60, 90, 105, 133, 200};
    // [dx12:mesh] end preset

    if (ps_Preset >= std::size(budget_by_preset))
        return;

#if RENDER == R_R4
    const bool mesh_grass = da_grass_mesh_expected();
#else
    constexpr bool mesh_grass = false;
#endif
    // The radius is the grass distance row's, not the quality preset's.
    if (ps_r__grass_distance > 4)
        ps_r__grass_distance = ps_Preset;
    const int detail_radius =
        mesh_grass ? detail_radius_mesh_by_tier[ps_r__grass_distance] : detail_radius_by_tier[ps_r__grass_distance];
    // [dx12:vrs] begin apply
    ps_r__vrs = vrs_by_preset[ps_Preset];
    // [dx12:vrs] end apply

    ps_r__light_shadow_budget = budget_by_preset[ps_Preset];
    ps_r__light_shadow_cache = 1;
    ps_r__light_falloff = 1.f;
    ps_r__wmark_gbuf = 1;
    ps_r__wmark_bump = 3.f;
    ps_r__wmark_gloss_blood = 0.2f;
    ps_r__wmark_gloss = 0.f;
    ps_r__light_falloff_core = 4.f;
    ps_r__light_falloff_gain = 1.7f;
    ps_r__light_details = light_details_by_preset[ps_Preset];
    ps_r__hud_shadow = hud_shadow_by_preset[ps_Preset];
    ps_r__actor_shadow = actor_shadow_by_preset[ps_Preset];
    // Cloud deck: the flat deck everywhere (it is a few noise reads per sky pixel), the
    // volumetric slab on the two top presets - 6 steps on High, 12 on Extreme.
    static constexpr int clouds_by_preset[] = {0, 1, 1, 2, 3};
    ps_r__clouds_quality = clouds_by_preset[ps_Preset];
    ps_r__sss = sss_by_preset[ps_Preset];
    ps_r__sss_steps = sss_steps_by_preset[ps_Preset];
    ps_r_water_reflection = water_refl_by_preset[ps_Preset];
    ps_r__water_waves = water_waves_by_preset[ps_Preset];
    ps_r__water_ripple = water_ripple_by_preset[ps_Preset];
    ps_r__visor_drops = visor_drops_by_preset[ps_Preset];
    ps_r__water_underwater = water_underwater_by_preset[ps_Preset];
    ps_r__water_caustics = water_caustics_by_preset[ps_Preset];
    ps_r__puddle_fill = puddle_fill_by_preset[ps_Preset];
    ps_r__rain_quality = rain_quality_by_preset[ps_Preset];
    // The look is not a quality tier: the same grade and foliage saturation on every preset,
    // re-applied here so a user.ltx line from an earlier build (the sibling's 1.6 / 2.0) or a
    // session experiment never outlives the start.
    ps_r__foliage_vibrance = 1.1f;
    ps_r__lod_sat = 1.4f;
    ps_r__grade_sat = 0.9f;
    ps_r__grade_green = 0.8f;
    ps_r__grade_olive = 0.3f;
    ps_r__grade_contrast = 1.05f;
    ps_r__bloom = 0.3f;
    ps_r__bloom_threshold = 0.5f;
    ps_r__bloom_knee = 0.5f;
    ps_r__mblur_shutter = 0.5f;
    ps_r__froxel_strength = 1.f;
    ps_r__froxel_torch = 1.f;
    ps_r__spec_ggx = 1;
    ps_r__particle_light = 1;
    ps_r__spec_f0 = 0.8f;
    ps_r__spec_f0_max = 0.16f;
    ps_r__spec_smooth = 3.f;
    ps_r__fade_time = 0.35f;
    ps_r__grass_fade_start = grass_fade_by_preset[ps_Preset];
    ps_r__grass_fade_flat = grass_flat_by_preset[ps_Preset];
    ps_r__puddles = puddles_by_preset[ps_Preset] > 0;
    ps_r__puddles_refl = int(puddles_by_preset[ps_Preset]) - 1;
    ps_r__puddles_dist = puddles_dist_by_preset[ps_Preset];
    // Look constants, see their definitions: re-applied so no earlier user.ltx pins them.
    ps_r__puddles_size = 0.60f;
    ps_r__puddles_dark = 0.65f;
    ps_r__puddles_refl_power = 1.0f;
    ps_r__puddles_facing = 0.03f;
    ps_r__puddles_sky = 1.0f;
    ps_r__puddles_gbuf = 0;
    // Rain look constants, defined in xr_ioc_cmd.cpp - the same treatment as the grade and
    // puddle look above. They are not a tier (a preset must not decide what rain looks like,
    // only how much of it there is, and that column is r__rain_quality), but an old user.ltx
    // still carries the pre-Runtime lines and executes before the renderer starts.
    ps_r__rain_len = 2.0f;
    ps_r__rain_width = 0.20f;
    ps_r__rain_bright = 2.2f;
    ps_r__rain_splash_bright = 0.9f;
    ps_r__rain_radius = 14.0f;
    ps_r__rain_splash = 1.0f;
    ps_r__rain_splash_time = 0.10f;
    // The drop BUDGET is a quality column, but it rides the ladder exactly once, downstream:
    // dxRainRender scales this base by the r__rain_quality tier factor. A second table here
    // would apply the ladder twice, so what this heals is only a stale pinned count.
    ps_r__rain_drops = 6000;
    ps_current_detail_density = detail_density;
    ps_r3_dyn_wet_surf_far = wet_far_by_preset[ps_Preset];
    ps_r3_dyn_wet_surf_sm_res = wet_sm_res_by_preset[ps_Preset];
    ps_r__aref_quality = aref_by_preset[ps_Preset];
    // Anti-aliasing is not the preset's: the replay only puts the passes back on the player's
    // r__aa after a console session override.
    xrRender_apply_aa();
    ps_r2_lenswater = lenswater_by_preset[ps_Preset];
    ps_r2_sss_enable = sun_shafts_ss_by_preset[ps_Preset];
    // The visor's dirt is a mask's, not a setting: a user.ltx line from when it was a checkbox
    // must not keep it off.
    ps_r2_lensdirt = 1;
    ps_r2_smapsize = smapsize_by_preset[ps_Preset];
    ps_r2_sun_far = sun_far_by_preset[ps_Preset];
    ps_r__sun_cascade_mid = sun_cascade_mid_by_preset[ps_Preset];
    ps_r__sun_cascade_far = sun_cascade_far_by_preset[ps_Preset];
    ps_r__sun_shadow_fade = ps_r__sun_cascade_far * 0.875f;
    ps_r__grass_shadow_dist = grass_shadow_by_preset[ps_Preset];
#if RENDER == R_R4
    // The blade grass and the snow keep their preset columns in their own modules (GRASS.md, SNOW.md);
    // both apply at the next frame.
    da_grass_apply_preset(ps_Preset);
    da_grass_apply_distance(ps_r__grass_distance);
    da_snow_apply_preset(ps_Preset);
    da_veg_apply_preset(ps_Preset);
#endif

    // The distances the preset just decided, in the log. Every report about something ending
    // too close - shadows, reflections, grass - is answered by these five numbers, and until
    // now answering it meant reading this function.
#if RENDER == R_R4
    const int vrs_mode = da_vrs_preset_mode();
#else
    constexpr int vrs_mode = 0;
#endif
    Msg("* [preset] %u: sun shadows=%.0f m (cascades 20/%.0f/%.0f, map %u), grass_shadow=%d m, grass=%d m%s, "
        "grass distance=%u, refl_tier=%u, puddles=%d m, wet=%.0f m, vrs=%d",
        ps_Preset, ps_r__sun_shadow_fade, ps_r__sun_cascade_mid, ps_r__sun_cascade_far, ps_r2_smapsize,
        ps_r__grass_shadow_dist, detail_radius, mesh_grass ? " (mesh)" : "", ps_r__grass_distance, ps_r_water_reflection,
        ps_r__puddles_dist, ps_r3_dyn_wet_surf_far, vrs_mode);
    string_path ssao_cmd;
    strconcat(sizeof(ssao_cmd), ssao_cmd, "r2_ssao_mode ", ssao_mode_by_preset[ps_Preset]);
    Console->Execute(ssao_cmd);
    // Radius goes through the console command so dm_current_size/dm_fade recompute exactly
    // the way a manual r__detail_radius change does.
    string32 radius_cmd;
    xr_sprintf(radius_cmd, "r__detail_radius %d", detail_radius);
    Console->Execute(radius_cmd);

    // QA hook: an optional appdata\qa_autoexec.ltx executes AFTER the derived switches.
    // The rig runs headless and user.ltx executes BEFORE renderer create, so any
    // preset-derived value it sets is stomped by the tables above - this is the only
    // per-feature override a measurement run has. Absent file = zero cost.
    {
        string_path qa_cfg;
        FS.update_path(qa_cfg, "$app_data_root$", "qa_autoexec.ltx");
        if (FS.exist(qa_cfg))
        {
            string_path cmd;
            strconcat(sizeof(cmd), cmd, "cfg_load ", qa_cfg);
            Console->Execute(cmd);
            Msg("* [QA] qa_autoexec.ltx applied after preset sync");
        }
    }
}

// The preset file plus the derived switches: everything the quality tier decides, in the order
// that leaves the switches in charge of whatever the file carries.
void xrRender_apply_preset()
{
    string_path cfg;

    switch (ps_Preset)
    {
    case 0: xr_strcpy(cfg, "rspec_minimum.ltx"); break;
    case 1: xr_strcpy(cfg, "rspec_low.ltx"); break;
    case 2: xr_strcpy(cfg, "rspec_default.ltx"); break;
    case 3: xr_strcpy(cfg, "rspec_high.ltx"); break;
    case 4: xr_strcpy(cfg, "rspec_extreme.ltx"); break;
    default: return;
    }
    FS.update_path(cfg, "$game_config$", cfg);

    string_path cmd;
    strconcat(sizeof(cmd), cmd, "cfg_load", " ", cfg);
    Console->Execute(cmd);

    xrRender_sync_preset_derived();
}

// r__aa onto the passes: TAA is SMAA plus the temporal resolve, as the old Extreme preset had it.
void xrRender_apply_aa()
{
    if (ps_r__aa > 3)
    {
        static constexpr u32 aa_by_preset[] = {1, 1, 2, 2, 3};
        ps_r__aa = aa_by_preset[std::min<u32>(ps_Preset, 4)];
    }
    ps_r2_fxaa = ps_r__aa == 1 ? 1 : 0;
    ps_r__smaa = ps_r__aa >= 2 ? 1 : 0;
    ps_r__taa = ps_r__aa == 3 ? 1 : 0;
}

class CCC_Preset : public CCC_Token
{
public:
    CCC_Preset(LPCSTR N, u32* V, const xr_token* T) : CCC_Token(N, V, T){};

    virtual void Execute(LPCSTR args)
    {
        const u32 before = *value;
        CCC_Token::Execute(args);
        // A new quality preset brings its own grass distance; the selector may move it after.
        if (*value != before)
            ps_r__grass_distance = *value;
        xrRender_apply_preset();
    }
};

// r__grass_distance: the derived switches again, which take the detail radius from it (the blades follow at once,
// the stock detail cards at the next detail reload).
class CCC_GrassDistance final : public CCC_Token
{
public:
    CCC_GrassDistance(pcstr name, u32* target, const xr_token* tokens) : CCC_Token(name, target, tokens) {}

    void Execute(pcstr args) override
    {
        CCC_Token::Execute(args);
        // The sync replays qa_autoexec.ltx, which may set the density itself.
        static bool syncing = false;
        if (*value <= 4 && !syncing)
        {
            syncing = true;
            xrRender_sync_preset_derived();
            syncing = false;
        }
    }
};

// Logs the last resolved GPU pass timings. The stats HUD shows the same numbers; this one
// exists so a QA probe can read them back from the log.
void da_dump_cloud_map();
class CCC_CloudMapDump final : public IConsole_Command
{
public:
    CCC_CloudMapDump(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override { da_dump_cloud_map(); }
};

// What the ripple field holds, read back and logged (r4_rendertarget_phase_water_ripple.cpp):
// the QA rig's way to tell an empty field from a faint one.
void da_water_ripple_stats();
class CCC_WaterRippleStats final : public IConsole_Command
{
public:
    CCC_WaterRippleStats(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override { da_water_ripple_stats(); }
};

// The same for the water on the visor (r4_rendertarget_phase_visor_drops.cpp), plus the
// thickness dumped as a picture: this effect has twice been shipped by people who could not
// tell an empty field from one too faint to see.
void da_visor_drops_stats();
class CCC_VisorDropsStats final : public IConsole_Command
{
public:
    CCC_VisorDropsStats(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override { da_visor_drops_stats(); }
};

class CCC_gpu_stats : public IConsole_Command
{
public:
    CCC_gpu_stats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/)
    {
#if defined(USE_DX11)
        if (!GpuTimers.enabled())
        {
            Msg("* [gpu] timestamp queries unavailable");
            return;
        }
        if (!GpuTimers.valid())
        {
            Msg("* [gpu] no resolved frame yet");
            return;
        }
        Msg("* [gpu] frame=%.3f scene=%.3f sun=%.3f lights=%.3f clouds=%.3f combine=%.3f ms",
            GpuTimers.ms(dx11GpuTimers::Frame), GpuTimers.ms(dx11GpuTimers::Scene),
            GpuTimers.ms(dx11GpuTimers::Sun), GpuTimers.ms(dx11GpuTimers::Lights),
            GpuTimers.ms(dx11GpuTimers::Clouds), GpuTimers.ms(dx11GpuTimers::Combine));
#else
        Msg("* [gpu] timing is a DX11 feature");
#endif
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

//-----------------------------------------------------------------------
void xrRender_initconsole()
{
    ZoneScoped;

    CMD3(CCC_Preset, "_preset", &ps_Preset, qpreset_token);
    CMD3(CCC_GrassDistance, "r__grass_distance", &ps_r__grass_distance, qgrass_distance_token);

    CMD4(CCC_Integer, "rs_skeleton_update", &psSkeletonUpdate, 2, 128);
#ifndef MASTER_GOLD
    CMD1(CCC_DumpResources, "dump_resources");
    CMD1(CCC_MotionsStat, "stat_motions");
    CMD1(CCC_TexturesStat, "stat_textures");
#endif

    CMD4(CCC_Float, "r__dtex_range", &r__dtex_range, 5, 175);

    // Common
    CMD1(CCC_Screenshot, "screenshot");
#if defined(USE_DX11)
    CMD1(CCC_VideoCapture, "r__capture");
#endif

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
    CMD4(CCC_RuntimeFloat, "r__fade_time", &ps_r__fade_time, 0.f, 2.f);
    CMD4(CCC_RuntimeFloat, "r__fade_qa", &ps_r__fade_qa, 0.f, 1.f);
    CMD4(CCC_RuntimeInteger, "r__fade_stats", &ps_r__fade_stats, 0, 600);
    CMD3(CCC_Token, "r__optimize_static_geom", &ps_r_optimize_static, q_optimize_static_token);
    //CMD4(CCC_Float, "r__geometry_lod_pow", &ps_r__LOD_Power, 0, 2);

    CMD4(CCC_Float, "r__detail_density", &ps_current_detail_density/*&ps_r__Detail_density*/, 0.1f, 1.f);
    CMD4(CCC_detail_radius, "r__detail_radius", &ps_r__detail_radius, 49, 300);
    CMD4(CCC_Float, "r__detail_height", &ps_r__Detail_height, 0.1f, 2.f);

#ifdef DEBUG
    CMD4(CCC_Float, "r__detail_l_ambient", &ps_r__Detail_l_ambient, .5f, .95f);
    CMD4(CCC_Float, "r__detail_l_aniso", &ps_r__Detail_l_aniso, .1f, .5f);
#endif // DEBUG


    CMD2(CCC_tf_Aniso, "r__tf_aniso", &ps_r__tf_Anisotropic); // {1..16}
    CMD2(CCC_tf_MipBias, "r1_tf_mipbias", &ps_r__tf_Mipbias); // {-3 +3}
    CMD2(CCC_tf_MipBias, "r2_tf_mipbias", &ps_r__tf_Mipbias); // {-3 +3}

    CMD4(CCC_ClearModelsOnUnload, "r__clear_models_on_unload", &ps_r__clear_models_on_unload, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__light_shadow_budget", &ps_r__light_shadow_budget, 0, 64);
    CMD4(CCC_RuntimeInteger, "r__light_shadow_cache", &ps_r__light_shadow_cache, 0, 2); // QA: 2 logs the faces' modes
    CMD4(CCC_RuntimeInteger, "r__gtao_slices", &ps_r__gtao_slices, 1, 4);
    CMD4(CCC_RuntimeInteger, "r__gtao_temporal", &ps_r__gtao_temporal, 0, 1);
    CMD4(CCC_RuntimeFloat, "r__gtao_blend", &ps_r__gtao_blend, 0.02f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__light_falloff", &ps_r__light_falloff, 0.f, 1.f);
    CMD4(CCC_RuntimeInteger, "r__wmark_gbuf", &ps_r__wmark_gbuf, 0, 2); // QA: 2 the marks' quads whole
    CMD4(CCC_RuntimeFloat, "r__wmark_bump", &ps_r__wmark_bump, 0.f, 16.f);
    CMD4(CCC_RuntimeFloat, "r__wmark_gloss_blood", &ps_r__wmark_gloss_blood, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__wmark_gloss", &ps_r__wmark_gloss, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__light_falloff_core", &ps_r__light_falloff_core, 0.f, 64.f);
    CMD4(CCC_RuntimeFloat, "r__light_falloff_gain", &ps_r__light_falloff_gain, 0.1f, 10.f);
    CMD4(CCC_RuntimeInteger, "r__light_details", &ps_r__light_details, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__light_dyn_shared", &ps_r__light_dyn_shared, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__hud_shadow", &ps_r__hud_shadow, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__actor_shadow", &ps_r__actor_shadow, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__clouds_quality", &ps_r__clouds_quality_override, -1, 3);
    CMD4(CCC_Float, "r__clouds_cover", &ps_r__clouds_cover, -1.f, 1.f);
    CMD4(CCC_Float, "r__hud_shadow_normal_offset", &ps_r__hud_shadow_normal_offset, 0.f, 0.3f);
    CMD4(CCC_Float, "r__hud_shadow_slope_bias", &ps_r__hud_shadow_slope_bias, 0.f, 0.05f);
    CMD4(CCC_Float, "r__sss", &ps_r__sss, 0.f, 1.f);
    CMD4(CCC_Float, "r__sss_len", &ps_r__sss_len, 0.05f, 2.f);
    CMD4(CCC_Float, "r__sss_thick", &ps_r__sss_thick, 0.05f, 3.f);
    CMD4(CCC_Float, "r__sss_steps", &ps_r__sss_steps, 2.f, 32.f);
    CMD4(CCC_Float, "r__fog", &ps_r__fog, 0.f, 1.f);
    CMD4(CCC_Float, "r__fog_sky", &ps_r__fog_sky, 0.f, 1.f);
    CMD4(CCC_Float, "r__fog_sky_mip", &ps_r__fog_sky_mip, 0.f, 10.f);
    CMD4(CCC_Float, "r__fog_sky_flat", &ps_r__fog_sky_flat, 0.f, 1.f);
    CMD4(CCC_Float, "r__fog_height", &ps_r__fog_height, 0.f, 4.f);
    CMD4(CCC_Float, "r__fog_height_falloff", &ps_r__fog_height_falloff, 0.001f, 0.5f);
    CMD4(CCC_Float, "r__fog_height_base", &ps_r__fog_height_base, -500.f, 500.f);
    CMD4(CCC_Float, "r__fog_dist", &ps_r__fog_dist, 0.1f, 8.f);
    CMD4(CCC_Float, "r__fog_follow_vis", &ps_r__fog_follow_vis, 0.f, 2.f);
    CMD4(CCC_Float, "r__fog_max", &ps_r__fog_max, 0.f, 1.f);
    CMD4(CCC_Float, "r__tonemap_hue", &ps_r__tonemap_hue, 0.f, 1.f);
    CMD4(CCC_Float, "r__tonemap_desat", &ps_r__tonemap_desat, 1.f, 32.f);
    CMD4(CCC_Float, "r__tonemap_white", &ps_r__tonemap_white, 0.f, 8.f);
    CMD4(CCC_RuntimeFloat, "r__grade_sat", &ps_r__grade_sat, 0.f, 2.f);
    CMD4(CCC_RuntimeFloat, "r__grade_green", &ps_r__grade_green, 0.f, 2.f);
    CMD4(CCC_RuntimeFloat, "r__grade_olive", &ps_r__grade_olive, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__grade_contrast", &ps_r__grade_contrast, 0.5f, 2.f);
    CMD4(CCC_RuntimeFloat, "r__bloom", &ps_r__bloom, 0.f, 4.f);
    CMD4(CCC_RuntimeFloat, "r__bloom_threshold", &ps_r__bloom_threshold, 0.f, 8.f);
    CMD4(CCC_RuntimeFloat, "r__bloom_knee", &ps_r__bloom_knee, 0.05f, 8.f);
    CMD4(CCC_Integer, "r__mblur", &ps_r__mblur, 0, 1);
    CMD4(CCC_RuntimeFloat, "r__mblur_shutter", &ps_r__mblur_shutter, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__froxel_strength", &ps_r__froxel_strength, 0.f, 100.f);
    CMD4(CCC_RuntimeFloat, "r__froxel_torch", &ps_r__froxel_torch, 0.f, 100.f);
    CMD4(CCC_RuntimeInteger, "r__spec_ggx", &ps_r__spec_ggx, 0, 3); // QA: 2 the gloss as grey, 3 the lights' and the sky's specular
    CMD4(CCC_RuntimeFloat, "r__spec_f0", &ps_r__spec_f0, 0.f, 4.f);
    CMD4(CCC_RuntimeInteger, "r__particle_light", &ps_r__particle_light, 0, 3);
    CMD4(CCC_Integer, "r__ssr", &ps_r__ssr, 0, 2);
    CMD4(CCC_RuntimeInteger, "r__grass_occlusion", &ps_r__grass_occlusion, 0, 7);
    CMD4(CCC_RuntimeFloat, "r__spec_f0_max", &ps_r__spec_f0_max, 0.02f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__spec_smooth", &ps_r__spec_smooth, 0.f, 8.f);
#if defined(USE_DX11)
    {
        // kill switch for batched tree rendering - it reorders and consumes the draw list
        extern int ps_r__tree_batch;
        CMD4(CCC_RuntimeInteger, "r__tree_batch", &ps_r__tree_batch, 0, 1);
    }
#endif
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
    CMD4(CCC_Float, "r2_sun_far", &ps_r2_sun_far, 51.f, 500.f);
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
    CMD4(CCC_Float, "r__smap_slope_bias", &ps_r__smap_slope_bias, 0.f, 8.f);
    CMD4(CCC_Integer, "r__smap_depth_bias", &ps_r__smap_depth_bias, 0, 256);

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
    CMD4(CCC_RuntimeInteger, "r2_lensdirt", &ps_r2_lensdirt, 0, 1);
    CMD4(CCC_Integer, "r2_lenswater", &ps_r2_lenswater, 0, 1);
    CMD4(CCC_Float, "r2_aberration_val", &ps_r2_aberration, 0.f, 1.f);
    CMD4(CCC_Float, "r2_aberration", &ps_r2_aberration, 0.f, 1.f);
    CMD4(CCC_Float, "r2_vibrance_val", &ps_r2_vibrance, -1.f, 1.f);
    CMD4(CCC_Float, "r2_lensdirt_val", &ps_r2_lensdirt_value, 0.f, 1.f);
    // Session override: the droplet AMOUNT is pushed every tick by the engine and by the HUD
    // script, so a config save must never catch a mid-surface value and pin it into user.ltx.
    // The master r2_lenswater above stays persisted - it is the options checkbox.
    CMD4(CCC_RuntimeFloat, "r2_lenswater_val", &ps_r2_lenswater_value, 0.f, 1.f);
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
    CMD4(CCC_Float, "r__veg_discard", &ps_r__vegDISCARD, 0.2f, 16.f);
    CMD4(CCC_Float, "r__grass_fade_start", &ps_r__grass_fade_start, 0.f, 0.95f);
    CMD4(CCC_Float, "r__grass_fade_flat", &ps_r__grass_fade_flat, 0.f, 1.f);
    CMD4(CCC_Integer, "r__grass_shadow_dist", &ps_r__grass_shadow_dist, 8, 150);
    CMD4(CCC_Integer, "r__aref_quality", &ps_r__aref_quality, 64, 255);
    CMD4(CCC_RuntimeInteger, "r__smaa", &ps_r__smaa, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__taa", &ps_r__taa, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__motion_vectors", &ps_r__motion_vectors, 0, 1);
#if RENDER == R_R4
    // The same commands in both renderer modules: a combo bound to a command the running module
    // lacks asserts, and user.ltx would drop the line at the next save.
    CMD3(CCC_AA, "r__aa", &ps_r__aa, qaa_token);
    {
        static CCC_Upscaler fsr("r__fsr", &ps_r__fsr, qfsr_token, &ps_r__dlss, false);
        Console->AddCommand(&fsr);
        static CCC_Upscaler dlss("r__dlss", &ps_r__dlss, qdlss_token, &ps_r__fsr, true);
        Console->AddCommand(&dlss);
    }
    CMD3(CCC_Token, "r__framegen", &ps_r__framegen, qframegen_token);
    CMD4(CCC_Float, "r__upscale_sharpness", &ps_r__upscale_sharpness, 0.f, 1.f);
    CMD1(CCC_RenderCaps, "r__caps");
    CMD1(CCC_GpuCapture, "r__gpu_capture");

    // DirectX 12 features (docs/dead-air/dx12-features/PLAN.md 2.6): the same commands in both
    // modules, so user.ltx keeps the DirectX 12 choices across a DirectX 11 session. Nothing here is
    // written by a preset except r__vrs's ladder (xrRender_sync_preset_derived).
    // [dx12:infra] begin commands
    CMD2(CCC_DaCall, "r__native_selftest", &da_native_selftest_run);
    // [dx12:infra] end commands
    // [dx12:dxc] begin commands
    CMD4(CCC_Integer, "r__dxc", &ps_r__dxc, 0, 1);
    // [dx12:dxc] end commands
    // [dx12:async] begin commands
    CMD3(CCC_Token, "r__async_compute", &ps_r__async_compute, qasync_compute_token);
    CMD4(CCC_RuntimeInteger, "r__async_path", &ps_r__async_path, 0, 3);
    CMD2(CCC_DaCall, "r__async_verify", &da_async_verify_request);
    // [dx12:async] end commands
    // [dx12:vrs] begin commands
    CMD4(CCC_RuntimeInteger, "r__vrs", &ps_r__vrs_override, -1, 2);
    CMD4(CCC_RuntimeFloat, "r__vrs_threshold", &ps_r__vrs_threshold, 0.25f, 4.f);
    CMD4(CCC_RuntimeInteger, "r__vrs_force", &ps_r__vrs_force, 0, 2);
    CMD4(CCC_RuntimeInteger, "r__vrs_debug", &ps_r__vrs_debug, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__vrs_log", &ps_r__vrs_log, 0, 100000);
    // QA: which passes may shade at a reduced rate, bit n = VrsPass n (da_vrs.h).
    CMD4(CCC_RuntimeInteger, "r__vrs_passes", &ps_r__vrs_passes, 0, (1 << int(VrsPass::Count)) - 1);
    // QA: scales the depth ratio a silhouette needs (k_jump, calibration).
    CMD4(CCC_RuntimeFloat, "r__vrs_edge", &ps_r__vrs_edge, 0.25f, 4.f);
    // [dx12:vrs] end commands
    // [dx12:gd] begin commands
    CMD4(CCC_RuntimeInteger, "r__gpu_driven", &ps_r__gpu_driven, -1, 2);
    // [dx12:gd] end commands
    // [dx12:mesh] begin commands
    CMD4(CCC_RuntimeInteger, "r__grass_mesh", &ps_r__grass_mesh, -1, 1);
    CMD2(CCC_DaCall, "r__grass_stats", &da_grass_mesh_stats);
    CMD4(CCC_RuntimeInteger, "r__dbg_grass_ab", &ps_r__dbg_grass_ab, 0, 1);
    // [dx12:mesh] end commands
    // QA: the state of the blade grass and of the snow passes in the log (GRASS.md, SNOW.md).
    CMD2(CCC_DaCall, "r__grass_blades_stats", &da_grass_stats);
    CMD2(CCC_DaCallArg, "r__grass_species_pin", &da_grass_pin_species);
    CMD2(CCC_DaCall, "r__snow_stats", &da_snow_stats);
    // QA: the procedural shrubs (SHRUBS.md) - the stock bushes back for an A/B, the frame's numbers, one species everywhere.
    CMD4(CCC_RuntimeInteger, "r__shrubs", &ps_r__shrubs, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__reeds", &ps_r__reeds, 0, 1);
    CMD2(CCC_DaCall, "r__shrubs_stats", &da_shrubs_stats);
    CMD2(CCC_DaCallArg, "r__shrubs_species_pin", &da_shrubs_pin_species);
    CMD2(CCC_DaCallArg, "r__shrubs_probe", &da_shrubs_probe);
    // (10 + n: QA, the snow layer cut short after its stage n, for the stages' costs - SNOW.md, costs)
    CMD4(CCC_RuntimeInteger, "r__dbg_snow", &ps_r__dbg_snow, 0, 19);
    CMD4(CCC_RuntimeFloat, "r__snow_glint", &ps_r__snow_glint, 0.f, 4.f);
    CMD4(CCC_RuntimeInteger, "r__snow_shells", &ps_r__snow_shells, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__snow_shadow", &ps_r__snow_shadow, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__dbg_grass", &ps_r__dbg_grass, 0, 1023);
    CMD4(CCC_RuntimeInteger, "r__dbg_shrubs", &ps_r__dbg_shrubs, 0, 511);
    CMD4(CCC_RuntimeFloat, "r__shrubs_lod_scale", &ps_r__shrubs_lod_scale, 0.05f, 20.f);
    CMD4(CCC_RuntimeInteger, "r__shrubs_force_tier", &ps_r__shrubs_force_tier, -1, 3);
    // QA: the upgraded trees (TREES.md) - the stock trees back for an A/B, the frame's numbers, one kind or one species, the
    // nearest trees; draws left out, the tiers' distances scaled, every tree in one tier (0 hulls .. 4 blobs, 5 billboards).
    CMD4(CCC_RuntimeInteger, "r__trees", &ps_r__trees, 0, 1);
    CMD2(CCC_DaCall, "r__trees_stats", &da_trees_stats);
    CMD2(CCC_DaCallArg, "r__trees_species_pin", &da_trees_pin_species);
    CMD2(CCC_DaCallArg, "r__trees_kind_pin", &da_trees_pin_kind);
    CMD2(CCC_DaCallArg, "r__trees_probe", &da_trees_probe);
    CMD2(CCC_DaCallArg, "r__snow_window_probe", &da_ground_window_probe);
    CMD4(CCC_RuntimeInteger, "r__dbg_trees", &ps_r__dbg_trees, 0, 262143);
    CMD4(CCC_RuntimeInteger, "r__trees_cut", &ps_r__trees_cut, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__trees_occlusion", &ps_r__trees_occlusion, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__veg_soft", &ps_r__veg_soft, 0, 7);
    CMD4(CCC_RuntimeInteger, "r__dbg_wood", &ps_r__dbg_wood, 0, 6);
    CMD4(CCC_RuntimeFloat, "r__trees_lod_scale", &ps_r__trees_lod_scale, 0.05f, 20.f);
    CMD4(CCC_RuntimeFloat, "r__trees_detail", &ps_r__trees_detail, 0.25f, 8.f);
    CMD4(CCC_RuntimeFloat, "r__trees_far_detail", &ps_r__trees_far_detail, 0.25f, 4.f);
    CMD4(CCC_RuntimeFloat, "r__shrubs_far_detail", &ps_r__shrubs_far_detail, 0.25f, 4.f);
    CMD4(CCC_RuntimeInteger, "r__trees_force_tier", &ps_r__trees_force_tier, -1, 5);
    // QA: the billboards' runtime bake (TREES.md) - both atlases and the stock path's level_lods copy into $screenshots$,
    // every billboard baked again.
    CMD2(CCC_DaCall, "r__trees_bb_dump", &da_trees_bake_dump);
    CMD2(CCC_DaCall, "r__trees_bb_rebake", &da_trees_bake_rebake);
    // [dx12:offload] begin commands
    CMD4(CCC_RuntimeFloat, "r__offload_margin", &ps_r__offload_margin, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__offload_agree", &ps_r__offload_agree, 0.f, 60.f);
    CMD4(CCC_RuntimeFloat, "r__offload_dwell", &ps_r__offload_dwell, 0.f, 600.f);
    CMD4(CCC_RuntimeFloat, "r__offload_recall", &ps_r__offload_recall, 0.f, 3600.f);
    CMD4(CCC_RuntimeFloat, "r__offload_smooth", &ps_r__offload_smooth, 0.05f, 10.f);
    CMD4(CCC_RuntimeFloat, "r__offload_warmup", &ps_r__offload_warmup, 0.f, 120.f);
    // [dx12:offload] end commands
    // [dx12:ds] begin commands
    CMD2(CCC_DaCall, "r__texload_stats", &da_dstorage_log_stats);
    // [dx12:ds] end commands
    // [dx12:rt] begin commands
    CMD4(CCC_Integer, "r__rt_reflections", &ps_r__rt_reflections, 0, 1);
    CMD4(CCC_Integer, "r__rt_shadows", &ps_r__rt_shadows, 0, 1);
    CMD4(CCC_Integer, "r__rtao", &ps_r__rtao, 0, 1);
    CMD3(CCC_Token, "r__rt_quality", &ps_r__rt_quality, qrt_quality_token);
    CMD4(CCC_RuntimeInteger, "r__rt_debug", &ps_r__rt_debug, 0, 5);
    // [dx12:rt] end commands
#endif
#if defined(USE_RENDERDOC)
    CMD1(CCC_RdocCapture, "rdoc_capture");
#endif
    CMD4(CCC_Integer, "r__grass_shadow_fade", &ps_r__grass_shadow_fade, 0, 50);
    CMD4(CCC_Integer, "r__fire_fluid", &ps_r__fire_fluid, 0, 1);
    CMD4(CCC_Float, "r__fire_fluid_dist", &ps_r__fire_fluid_dist, 5.f, 100.f);
    CMD4(CCC_Integer, "r__fire_fluid_grids", &ps_r__fire_fluid_grids, 1, 6);
    CMD4(CCC_Float, "r__grass_tint", &ps_r__grass_tint, 0.f, 1.f);
    CMD4(CCC_Float, "r__grass_tint_scale", &ps_r__grass_tint_scale, 1.f, 64.f);
    CMD4(CCC_Float, "r__grass_tint_base", &ps_r__grass_tint_base, 0.f, 4.f);
    CMD4(CCC_Float, "r__lod_hemi", &ps_r__lod_hemi, 0.f, 2.f);
    CMD4(CCC_RuntimeFloat, "r__lod_sat", &ps_r__lod_sat, 0.f, 3.f);
    CMD4(CCC_Float, "r__lod_bright", &ps_r__lod_bright, 0.2f, 2.f);
    CMD4(CCC_Float, "r__foliage_gloss", &ps_r__foliage_gloss, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__foliage_vibrance", &ps_r__foliage_vibrance, 0.f, 3.f);
    CMD4(CCC_Float, "r__foliage_debleach", &ps_r__foliage_debleach, 0.f, 1.f);
    // Session overrides: the preset decides what runs and the look constants are re-applied
    // at start, so none of these is written to user.ltx (an older line there still parses).
    CMD4(CCC_RuntimeInteger, "r__water_waves", &ps_r__water_waves, 0, 8);
    // Session overrides of the water ladder. r__water_ripple only takes effect on the next
    // renderer start (the target pair is created at that size); the rest are live.
    CMD4(CCC_RuntimeInteger, "r__water_ripple", &ps_r__water_ripple, 0, 1024);
    CMD4(CCC_RuntimeInteger, "r__visor_drops", &ps_r__visor_drops, 0, 1024);
    CMD4(CCC_RuntimeInteger, "r__water_underwater", &ps_r__water_underwater, 1, 4);
    CMD4(CCC_RuntimeInteger, "r__water_caustics", &ps_r__water_caustics, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__puddle_fill", &ps_r__puddle_fill, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__rain_quality", &ps_r__rain_quality, 1, 4);
    CMD4(CCC_RuntimeInteger, "r__puddles", &ps_r__puddles, 0, 1);
    CMD4(CCC_RuntimeFloat, "r__puddles_buildup", &ps_r__puddles_buildup, 5.f, 600.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_dry", &ps_r__puddles_dry, 0.5f, 20.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_size", &ps_r__puddles_size, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_force", &ps_r__puddles_force, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_gloss", &ps_r__puddles_gloss, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_dark", &ps_r__puddles_dark, 0.2f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_damp", &ps_r__puddles_damp, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_ripple", &ps_r__puddles_ripple, 0.f, 4.f);
    CMD4(CCC_RuntimeInteger, "r__puddles_debug", &ps_r__puddles_debug, 0, 3);
    CMD4(CCC_RuntimeInteger, "r__puddles_dist", &ps_r__puddles_dist, 5, 200);
    CMD4(CCC_RuntimeInteger, "r__puddles_gbuf", &ps_r__puddles_gbuf, 0, 1);
    CMD4(CCC_RuntimeFloat, "r__puddles_edge", &ps_r__puddles_edge, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_rim", &ps_r__puddles_rim, 0.f, 1.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_rim_width", &ps_r__puddles_rim_width, 0.01f, 1.f);
    CMD4(CCC_RuntimeInteger, "r__puddles_refl", &ps_r__puddles_refl, 0, 2);
    CMD4(CCC_RuntimeFloat, "r__puddles_refl_power", &ps_r__puddles_refl_power, 0.f, 4.f);
    CMD4(CCC_RuntimeFloat, "r__puddles_facing", &ps_r__puddles_facing, 0.f, 1.f);
    CMD4(CCC_Float, "r__puddles_sky", &ps_r__puddles_sky, 0.f, 1.f);
    CMD4(CCC_Float, "r__parallax_start", &ps_r__parallax_start, 0.f, 300.f);
    CMD4(CCC_Float, "r__parallax_stop", &ps_r__parallax_stop, 0.f, 300.f);
    CMD4(CCC_Float, "r__parallax_depth", &ps_r__parallax_depth, 0.f, 0.2f);
    CMD4(CCC_Float, "r__parallax_shadow", &ps_r__parallax_shadow, 0.f, 16.f);
    CMD4(CCC_Integer, "r__parallax_samples", &ps_r__parallax_samples, 4, 64);
    CMD4(CCC_Integer, "r__parallax_samples_min", &ps_r__parallax_samples_min, 1, 16);
    CMD4(CCC_Integer, "r__parallax_shadow_samples", &ps_r__parallax_shadow_samples, 2, 32);
    CMD4(CCC_Integer, "r__parallax_force", &ps_r__parallax_force, 0, 1);
    CMD4(CCC_Integer, "r__parallax_debug", &ps_r__parallax_debug, 0, 3);
    CMD4(CCC_RuntimeFloat, "r__sun_shadow_fade", &ps_r__sun_shadow_fade, 20.f, 800.f);
    CMD4(CCC_RuntimeFloat, "r__sun_cascade_mid", &ps_r__sun_cascade_mid, 20.f, 200.f);
    CMD4(CCC_RuntimeFloat, "r__sun_cascade_far", &ps_r__sun_cascade_far, 60.f, 800.f);
    CMD4(CCC_RuntimeFloat, "r__sun_cascade_blend", &ps_r__sun_cascade_blend, 0.f, 0.5f);
    CMD4(CCC_Integer, "r__dbg_sun_cascades", &ps_r__dbg_sun_cascades, 0, 1);
    CMD4(CCC_Integer, "r__particle_dist", &ps_r__particle_dist, 0, 1000);
    CMD4(CCC_Float, "r__macro_var", &ps_r__macro_var, 0.f, 1.f);
    CMD4(CCC_Float, "r__macro_var_scale", &ps_r__macro_var_scale, 2.f, 64.f);
    CMD4(CCC_Float, "r__macro_var_start", &ps_r__macro_var_start, 0.f, 300.f);
    CMD4(CCC_Float, "r__macro_var_end", &ps_r__macro_var_end, 10.f, 1000.f);
    CMD4(CCC_Float, "r__macro_tint", &ps_r__macro_tint, 0.f, 1.f);
    CMD4(CCC_Float, "r__macro_relief", &ps_r__macro_relief, 0.f, 2.f);
    CMD4(CCC_Float, "r__terrain_blend", &ps_r__terrain_blend, 0.f, 1.f);
    CMD4(CCC_Float, "r__mask_jitter", &ps_r__mask_jitter, 0.f, 0.05f);

    //float ps_r2_dof_near = 0.f; // 0.f
    //float ps_r2_dof_focus = 1.4f; // 1.4f

    CMD3(CCC_Mask, "r2_volumetric_lights", &ps_r2_ls_flags, R2FLAG_VOLUMETRIC_LIGHTS);
    //CMD3(CCC_Mask, "r2_sun_shafts", &ps_r2_ls_flags, R2FLAG_SUN_SHAFTS);
    CMD3(CCC_Token, "r2_sun_shafts", &ps_r_sun_shafts, qsun_shafts_token);
    CMD4(CCC_Float, "r2_sun_shafts_value", &ps_r2_sun_shafts_value, 0.f, 0.4f);
    CMD4(CCC_RuntimeInteger, "r2_sss_enable", &ps_r2_sss_enable, 0, 1);
    CMD4(CCC_Float, "r2_sss_intensity", &ps_r2_sss_intensity, 0.f, 2.f);
    CMD4(CCC_Float, "r2_sss_blend", &ps_r2_sss_blend, 0.01f, 1.f);
    CMD4(CCC_Float, "r2_sss_phase0", &ps_r2_sss_phase1, 0.01f, 0.2f);
    CMD4(CCC_Float, "r2_sss_phase1", &ps_r2_sss_phase1, 0.01f, 0.2f);
    CMD4(CCC_Float, "r2_sss_phase2", &ps_r2_sss_phase2, 0.01f, 0.2f);
    CMD4(CCC_Float, "r2_sss_radius", &ps_r2_sss_radius, 0.5f, 2.f);
    // Runtime, like r__smaa: the saved choice is r__aa, which these follow at every start.
    CMD4(CCC_RuntimeInteger, "r2_fxaa", &ps_r2_fxaa, 0, 1);
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
    CMD1(CCC_gpu_stats, "r__gpu_stats");
    CMD1(CCC_CloudMapDump, "r__cloud_map_dump");
    CMD1(CCC_WaterRippleStats, "r__water_ripple_stats");
    CMD1(CCC_VisorDropsStats, "r__visor_drops_stats");
    CMD4(CCC_Integer, "r__clouds_debug", &ps_r__clouds_debug, 0, 3);
    CMD4(CCC_Float, "r__clouds_temporal", &ps_r__clouds_temporal, 0.f, 0.95f);
    CMD4(CCC_RuntimeInteger, "r__gpu_log", &ps_r__gpu_log, 0, 100000);
    CMD4(CCC_Integer, "r__screenshot_every", &ps_r__screenshot_every, 0, 100000);

    //CMD3(CCC_Mask, "r2_sun_ignore_portals", &ps_r2_ls_flags, R2FLAG_SUN_IGNORE_PORTALS);

    CMD4(CCC_Integer, "r2_mt_calculate",    &ps_r2_mt_calculate, 0, 1);
#if RENDER == R_R4
    CMD4(CCC_Integer, "r2_mt_render",       &ps_r2_mt_render,    0, 1);
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
