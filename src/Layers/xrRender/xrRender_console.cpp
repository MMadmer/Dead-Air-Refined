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
// tonemapped LDR frame, three fullscreen passes + two RGBA8 targets. Preset-derived; the
// console command overrides for the session only and supersedes r2_fxaa while on.
int ps_r__smaa = 0;
// Camera-reprojection TAA on top of SMAA (phase_taa): stabilises grass/foliage shimmer the
// spatial pass cannot see. One fullscreen resolve + one RGBA8 history target.
int ps_r__taa = 0;
// Fade band width before that cut-off, metres; 0 = hard edge (stock). Blades lie down over
// the band so their shadows shorten into nothing instead of popping at a moving circle.
int ps_r__grass_shadow_fade = 10;
int ps_r__fire_fluid = 0;
float ps_r__fire_fluid_dist = 25.f;
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
// travelling shadow "wedge"). 140 completes before the 160 m far cascade edge.
float ps_r__sun_shadow_fade = 140.f;
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
int ps_r2_lensdirt = 0;
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
float ps_r__capture_stop_at = 0.f;
float ps_r__capture_fps = 20.f;

class CCC_VideoCapture final : public IConsole_Command
{
public:
    CCC_VideoCapture(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        float seconds = 0.f, fps = 20.f;
        const int parsed = sscanf(args, "%f %f", &seconds, &fps);
        if (parsed < 1 || seconds <= 0.f)
        {
            if (ps_r__capture_stop_at > 0.f)
            {
                // Force the tick to run its completion branch on the next frame.
                ps_r__capture_stop_at = Device.fTimeGlobal;
                Msg("* [capture] stop requested");
            }
            else
                Msg("* [capture] usage: r__capture <seconds 1..120> [fps 5..60]");
            return;
        }
        clamp(seconds, 1.f, 120.f);
        clamp(fps, 5.f, 60.f);
        ps_r__capture_fps = fps;
        ps_r__capture_stop_at = Device.fTimeGlobal + seconds;
        Msg("* [capture] recording %.1fs at %.0f fps", seconds, fps);
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
// user_facing: the switches that also live in the options menu and user.ltx (shadow map size,
// AO technique, grass density and radius, visor droplets). They follow the preset only when the
// player applies a preset; on renderer start the player's own user.ltx values stay, or the
// options would show a choice the next start silently undid.
void xrRender_sync_preset_derived(bool user_facing)
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
    // The fluid campfire: the two top presets simulate the nearest fires on the 3D grid.
    static constexpr int fire_fluid_by_preset[] = {0, 0, 0, 1, 1};
    ps_r__fire_fluid = fire_fluid_by_preset[ps_Preset];
    // Screen-space contact shadows join at High: an 8-step depth ray per lit pixel of the
    // near sun pass. Slightly stronger on Maximum; below High the two lowest-cost presets
    // keep the reference look. Not full strength on purpose - the technique's stepping
    // noise shows at 1.0, and 0.6-0.7 reads as shadow, not as dirt.
    static constexpr float sss_by_preset[] = {0.f, 0.f, 0.f, 0.6f, 0.7f};
    // Step count of that ray: the band where neighbouring pixels disagree on a hit is one
    // step wide, so at 8 steps over 35 cm it is 4 cm of rough edge - Maximum halves it.
    static constexpr float sss_steps_by_preset[] = {8.f, 8.f, 8.f, 8.f, 16.f};
    // Water screen-space reflections ladder (r3_water_refl semantics: 0 off, the march length
    // scales 64/110/160 with the tier). The two top presets take the sibling engine's tuned
    // default (high); Minimum stays on the plain cubemap. A shader-options change, so it
    // applies on renderer (re)start like the token itself.
    static constexpr u32 water_refl_by_preset[] = {0, 1, 2, 3, 3};
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
    // post stack. User-facing (there is a checkbox for it), hence the user_facing block below.
    // It is also a shader option (USE_LENS_WATER), so like the checkbox it lands on vid restart.
    static constexpr int lenswater_by_preset[] = {0, 0, 1, 1, 1};
    // How wide the visor's drop field is. The height follows the screen's aspect, so this is
    // the resolution of the glass itself: at 512 across a 22 cm visor a texel is 0.4 mm and the
    // 2 mm drops that pin to it are five texels across, which is the smallest a drop can be and
    // still read as round. One small pass at 30 Hz, so the ladder is short. Create-time.
    static constexpr int visor_drops_by_preset[] = {256, 256, 512, 512, 1024};
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
    // Puddle draw distance ladder, metres: the mask and the reflection cost per covered pixel,
    // and the far pixels are the cheap ones, so the top tiers see water to the tree line.
    static constexpr int puddles_dist_by_preset[] = {15, 20, 30, 45, 60};
    // Grass density ladder (lower = denser: cell grid is iCeil(2/density)+1 squared). The flat
    // 0.6 left even Extreme with a 25-cell grid; IX-Ray runs 121 cells there. Default keeps a
    // near-stock look, the top presets grow the field. Applies on level (re)load (cache_Alloc).
    static constexpr float detail_density_by_preset[] = {0.6f, 0.6f, 0.5f, 0.35f, 0.25f};
    // Grass draw radius ladder, metres. Was pinned at the stock 49 on every preset; the ceiling
    // stays modest because slot count grows quadratically. Applies live (CCC recomputes dm_*).
    static constexpr int detail_radius_by_preset[] = {49, 49, 60, 80, 100};
    // Wet-surface radius ladder, metres (r3_dynamic_wet_surfaces_far semantics). The pair of
    // console knobs existed but the shader hardcoded 5/20 - now that they are live, the far edge
    // rides the preset. Near stays at its 5 m default.
    static constexpr float wet_far_by_preset[] = {20.f, 20.f, 25.f, 35.f, 50.f};
    // The rain occlusion map covers that radius: its resolution rides along so a roof edge
    // stays a roof edge at fifty metres. Applied at start (the surface is created then).
    static constexpr int wet_sm_res_by_preset[] = {256, 256, 512, 512, 1024};
    // Alpha-ref ladder (donor rspec values with Default pinned to our historical 128): lower =
    // denser foliage silhouettes = more shaded pixels; Minimum/Low trim exactly there.
    static constexpr int aref_by_preset[] = {180, 160, 128, 110, 100};
    // SMAA 1x from the Default preset up: better gradients than FXAA for ~3 cheap fullscreen
    // passes. The two lowest presets keep the reference pipeline (r2_fxaa keeps working there).
    static constexpr int smaa_by_preset[] = {0, 0, 1, 1, 1};
    // Camera TAA only on the top preset: it trades a touch of sharpness under motion for a
    // stable field, which is an Extreme-tier call.
    static constexpr int taa_by_preset[] = {0, 0, 0, 0, 1};
    // Sun shadow-map size ladder - the single most expensive shadow knob was pinned at 2048
    // on every preset ("presets or nothing" gap). Applies on renderer (re)start, since the
    // smap targets are created once. Default keeps the historical 2048 exactly.
    // 4096 cascades cost ~3.5 ms more than 2048 in the sun pass at 1440p (rig, 3 cascades);
    // 2048 on the top presets too: the clouds are where those milliseconds go now.
    static constexpr u32 smapsize_by_preset[] = {1024, 1536, 2048, 2048, 2048};
    // AO technique ladder. GTAO (ported from IX-Ray: 3-slice horizon integral plus a guided
    // filter) replaces the inline HDAO/HBAO on the two top presets; Default keeps the reference
    // inline SSAO, the two lowest presets keep AO off. Applied through the console command so
    // the R2FLAGEXT_SSAO_* side effects stay in CCC_SSAO_Mode.
    static constexpr pcstr ssao_mode_by_preset[] = {"disabled", "disabled", "default", "gtao", "gtao"};

    if (ps_Preset >= std::size(budget_by_preset))
        return;

    ps_r__light_shadow_budget = budget_by_preset[ps_Preset];
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
    if (user_facing)
        ps_current_detail_density = detail_density_by_preset[ps_Preset];
    ps_r3_dyn_wet_surf_far = wet_far_by_preset[ps_Preset];
    ps_r3_dyn_wet_surf_sm_res = wet_sm_res_by_preset[ps_Preset];
    ps_r__aref_quality = aref_by_preset[ps_Preset];
    ps_r__smaa = smaa_by_preset[ps_Preset];
    ps_r__taa = taa_by_preset[ps_Preset];
    if (user_facing)
    {
        ps_r2_lenswater = lenswater_by_preset[ps_Preset];
        ps_r2_smapsize = smapsize_by_preset[ps_Preset];
        string_path ssao_cmd;
        strconcat(sizeof(ssao_cmd), ssao_cmd, "r2_ssao_mode ", ssao_mode_by_preset[ps_Preset]);
        Console->Execute(ssao_cmd);
        // Radius goes through the console command so dm_current_size/dm_fade recompute exactly
        // the way a manual r__detail_radius change does.
        string32 radius_cmd;
        xr_sprintf(radius_cmd, "r__detail_radius %d", detail_radius_by_preset[ps_Preset]);
        Console->Execute(radius_cmd);
    }

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

        // Applied after the preset file so the derived switches stay in charge
        // regardless of what the file carries.
        xrRender_sync_preset_derived(true);
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
    CMD4(CCC_Integer, "r__unload_level_textures", &ps_r__unload_level_textures, 0, 1);
    CMD4(CCC_RuntimeInteger, "r__light_shadow_budget", &ps_r__light_shadow_budget, 0, 64);
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
#if defined(USE_RENDERDOC)
    CMD1(CCC_RdocCapture, "rdoc_capture");
#endif
    CMD4(CCC_Integer, "r__grass_shadow_fade", &ps_r__grass_shadow_fade, 0, 50);
    CMD4(CCC_Integer, "r__fire_fluid", &ps_r__fire_fluid, 0, 1);
    CMD4(CCC_Float, "r__fire_fluid_dist", &ps_r__fire_fluid_dist, 5.f, 100.f);
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
    CMD4(CCC_Float, "r__sun_shadow_fade", &ps_r__sun_shadow_fade, 20.f, 500.f);
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
    CMD1(CCC_gpu_stats, "r__gpu_stats");
    CMD1(CCC_CloudMapDump, "r__cloud_map_dump");
    CMD1(CCC_WaterRippleStats, "r__water_ripple_stats");
    CMD1(CCC_VisorDropsStats, "r__visor_drops_stats");
    CMD4(CCC_Integer, "r__clouds_debug", &ps_r__clouds_debug, 0, 3);
    CMD4(CCC_Float, "r__clouds_temporal", &ps_r__clouds_temporal, 0.f, 0.95f);
    CMD4(CCC_Integer, "r__gpu_log", &ps_r__gpu_log, 0, 100000);
    CMD4(CCC_Integer, "r__screenshot_every", &ps_r__screenshot_every, 0, 100000);

    //CMD3(CCC_Mask, "r2_sun_ignore_portals", &ps_r2_ls_flags, R2FLAG_SUN_IGNORE_PORTALS);

    CMD4(CCC_Integer, "r2_mt_calculate",    &ps_r2_mt_calculate, 0, 1);
#if RENDER == R_R4
    CMD4(CCC_Integer, "r2_mt_render",       &ps_r2_mt_render,    0, 1);
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
