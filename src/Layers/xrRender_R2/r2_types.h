#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// Base targets
#define     r2_RT_base          "$user$base_"
#define     r2_RT_base_depth    "$user$base_depth"

// r3xx code-path (MRT)
#define     r2_RT_depth         "$user$depth"       // MRT
#define     r2_RT_MSAAdepth     "$user$msaadepth"   // MRT
#define     r2_RT_P             "$user$position"    // MRT
#define     r2_RT_N             "$user$normal"      // MRT
#define     r2_RT_albedo        "$user$albedo"      // MRT

// other
#define     r2_RT_SSR           "$user$ssr"         // --- scene grab for water SSLR (R4)
#define     r2_RT_accum         "$user$accum"       // --- 16 bit fp or 16 bit fx
#define     r2_RT_accum_temp    "$user$accum_temp"  // --- 16 bit fp - only for HW which doesn't feature fp16 blend

#define     r2_T_envs0          "$user$env_s0"
#define     r2_T_envs1          "$user$env_s1"

#define     r2_T_sky0           "$user$sky0"
#define     r2_T_sky1           "$user$sky1"

// 3D PDA: the whole PDA dialog rasterized once per frame; the pda screen material samples it.
#define     r2_RT_ui            "$user$ui"
#define     r2_RT_generic0      "$user$generic0"
#define     r2_RT_generic0_r    "$user$generic0_r"

#define     r2_RT_generic1      "$user$generic1"
#define     r2_RT_generic1_r    "$user$generic1_r"

#define     r2_RT_generic2      "$user$generic2"    // --- // Igor: for volumetric lights
#define     r2_RT_generic       "$user$generic"     // --- actually generic3

#define     r2_RT_SunShaftsMask "$user$SunShaftsMask"
#define     r2_RT_SunShaftsMaskSmoothed "$user$SunShaftsMaskSmoothed"
#define     r2_RT_SunShaftsPass0 "$user$SunShaftsPass0"
#define     r2_RT_smaa_edges    "$user$smaa_edges"   // --- SMAA pass 1 output (RG edge mask)
#define     r2_RT_smaa_blend    "$user$smaa_blend"   // --- SMAA pass 2 output (blend weights)
#define     r2_RT_gtao          "$user$gtao_0"      // GTAO: view-z + raw AO for the guided filter
#define     r2_RT_taa_history   "$user$taa_history"  // camera-TAA history (previous resolved frame, 10-bit)
#define     r2_RT_taa_resolve   "$user$taa_resolve"  // camera-TAA resolve MRT1: this frame's history before the copy

#define     r2_RT_ssao_temp     "$user$ssao_temp"   // temporary rt for ssao calculation
#define     r2_RT_half_depth    "$user$half_depth"  // temporary rt for ssao/hbao calculation

#define     r2_RT_bloom1        "$user$bloom1"
#define     r2_RT_bloom2        "$user$bloom2"

#define     r2_RT_luminance_t64 "$user$lum_t64"     // --- temp
#define     r2_RT_luminance_t8  "$user$lum_t8"      // --- temp

#define     r2_RT_luminance_src "$user$tonemap_src" // --- prev-frame-result
#define     r2_RT_luminance_cur "$user$tonemap"     // --- result
#define     r2_RT_luminance_pool "$user$luminance"  // --- pool

#define     r2_RT_smap_surf     "$user$smap_surf"   // --- directional
#define     r2_RT_smap_depth    "$user$smap_depth"  // --- directional
#define     r2_RT_smap_rain     "$user$smap_rain"
#define     r2_RT_smap_depth_minmax "$user$smap_depth_minmax"
#define     r2_RT_smap_hud      "$user$smap_hud"    // --- first-person self-shadow

// Depth slice rmNear() squeezes the first-person HUD into, so it can never intersect the
// world. It doubles as the marker of a HUD pixel: nothing else in the frame reaches it,
// because at a 0.05 m near plane this depth is a distance of about 5 cm from the eye.
static constexpr float r2_hud_depth_limit = 0.02f;

#define     r2_async_ss         "$user$async_ss"

#define     r2_material         "$user$material"
#define     r2_ds2_fade         "$user$ds2_fade"

#define     r2_jitter           "$user$jitter_"     // --- dither
#define     r2_jitter_mipped    "$user$jitter_mipped" // --- dither
#define     r2_smaa_area        "$user$smaa_area"    // --- baked LUT, 160x560 R8G8
#define     r2_smaa_search      "$user$smaa_search"  // --- baked LUT, 64x16 R8
#define     r2_blue_noise       "$user$blue_noise"  // --- 128x128 R8G8 blue-noise tile (GTAO jitter)
#define     r2_sunmask          "sunmask"
// The cloud deck field rendered once per frame (phase_cloud_map); the sun passes read it
// through s_lmap, the visible deck through s_cloud_map.
// The baked, level-wide water map (R = surface world Y, G = coverage, B = bed world Y,
// A = metres to the nearest bank) and the ripple simulation's ping-pong pair (R = height now,
// G = height one step back), a window of CEnvironment::water_ripple_window metres around the
// camera. Mapped by da_water_map / da_water_map2 and da_water_rip.
// Only the ripple pair is a render target. The field is a CPU bake uploaded once per level as
// a plain immutable texture (r4_water_field.cpp), so it lives under the same $user$ namespace
// but never appears in the render target list.
// The pair is named like rt_Base: the index is appended, so the ping-pong halves are
// "$user$water_ripple0" and "$user$water_ripple1" and a pass can bind either by name.
#define r2_RT_water_field "$user$water_field"
#define r2_RT_water_ripple "$user$water_ripple"
// The puddle fill map: how deep rain would stand at this texel, R16F, 0..1 of
// CEnvironment::puddle_fill_depth. Baked in the same sweep as the field above and uploaded
// beside it (r4_water_field.cpp), so it is no more a render target than the field is. Mapped by
// da_water_map, read by da_puddles.h in both halves of a puddle.
#define r2_RT_puddle_fill "$user$puddle_fill"
#define r2_RT_cloud_map "$user$cloud_map"
#define r2_RT_depth_copy "$user$depth_copy"
#define r2_RT_clouds0 "$user$clouds0"
#define r2_RT_clouds1 "$user$clouds1"

#define     r2_base             "$user$base"

static constexpr auto c_lmaterial = "L_material";
static constexpr auto c_sbase = "s_base";
static constexpr auto c_snoise = "s_noise";
static constexpr auto c_ssky0 = "s_sky0";
static constexpr auto c_ssky1 = "s_sky1";
static constexpr auto c_sclouds0 = "s_clouds0";
static constexpr auto c_sclouds1 = "s_clouds1";

#define JITTER(a) r2_jitter #a

const float SMAP_near_plane = .1f;

const u32 SMAP_adapt_min = 32;
const u32 SMAP_adapt_optimal = 768;
const u32 SMAP_adapt_max = 1536;

const u32 TEX_material_LdotN = 128; // diffuse, X, almost linear = small res
const u32 TEX_material_LdotH = 256; // specular, Y
const u32 TEX_material_Count = 4; // Number of materials, Z
const u32 TEX_jitter = 64;
const u32 TEX_jitter_count = 5; // for HBAO

const u32 BLOOM_size_X = 256;
const u32 BLOOM_size_Y = 256;
const u32 LUMINANCE_size = 16;

// deffer
#define SE_R2_NORMAL_HQ     0 // high quality/detail
#define SE_R2_NORMAL_LQ     1 // low quality
#define SE_R2_SHADOW        2 // shadow generation

// spot
#define SE_L_FILL           0
#define SE_L_UNSHADOWED     1
#define SE_L_NORMAL         2 // typical, scaled
#define SE_L_FULLSIZE       3 // full texture coverage
#define SE_L_TRANSLUENT     4 // with opacity/color mask

// mask
#define SE_MASK_SPOT        0
#define SE_MASK_POINT       1
#define SE_MASK_DIRECT      2
#define SE_MASK_ACCUM_VOL   3
#define SE_MASK_ACCUM_2D    4
#define SE_MASK_ALBEDO      5

// sun
#define SE_SUN_NEAR         0
#define SE_SUN_MIDDLE       1
#define SE_SUN_FAR          2
#define SE_SUN_LUMINANCE    3
#define SE_SUN_NEAR_MINMAX  4
// For rain R3 rendering
#define SE_SUN_RAIN_SMAP    5

extern float ps_r2_gloss_factor;
IC float u_diffuse2s(float x, float y, float z)
{
    float v = (x + y + z) / 3.f;
    return ps_r2_gloss_factor * ((v < 1) ? powf(v, 2.f / 3.f) : v);
}

IC float u_diffuse2s(Fvector3& c)
{
    return u_diffuse2s(c.x, c.y, c.z);
}
} // namespace xray::render::RENDER_NAMESPACE
