#ifndef DA_CLOUDS_VOL_H
#define DA_CLOUDS_VOL_H

#include "da_clouds.h"

// The volumetric deck: the cloud field as a real volume, in the Nubis/Horizon manner.
//
// The weather map (da_clouds.h, 2D) says WHERE clouds are - coverage cells the wind aloft
// drags across the level, the same field that shades the ground. This header says what a
// cloud IS inside its cell: a Perlin-Worley base shape carved by Worley octaves, a height
// profile from stratus (flat, low) to cumulus (towering, round-topped), and a Worley detail
// texture that erodes the edges - wispy at the base, billowy at the top. Both textures are
// tiling 3D volumes generated once (tools: gen_cloud_noise.py) and shipped in the archive.
//
// Lighting: a short march toward the sun through the cheap (shape-only) density gives the
// optical depth; Beer-Lambert with three multiple-scattering octaves (Wrenninge), a
// dual-lobe Henyey-Greenstein phase (the silver lining and the back glow), a powder term for
// the sun-facing edges, and sky light graded from the dark base to the bright top.
Texture3D s_cloud_shape;   // R = Perlin-Worley, GBA = Worley octaves
Texture3D s_cloud_detail;  // RGB = Worley octaves

// da_cloud_cam_r/u/d (set by the phase): xyz = camera right/up/forward, w = tan(fov/2) x/y.
uniform float4 da_cloud_cam_r;
uniform float4 da_cloud_cam_u;
uniform float4 da_cloud_cam_d;
// Lightning (cl_da_lightning*): the flash inside the deck, its colour, and what it added to
// the fog colour this frame.
uniform float4 da_lightning;
uniform float4 da_lightning2;
uniform float4 da_lightning_fog;

float3 da_cloud_view_ray(float2 uv)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return normalize(da_cloud_cam_d.xyz + da_cloud_cam_r.xyz * (ndc.x * da_cloud_cam_r.w)
        + da_cloud_cam_u.xyz * (ndc.y * da_cloud_cam_u.w));
}

float da_remap(float v, float lo0, float hi0, float lo1, float hi1)
{
    return lo1 + (v - lo0) / max(hi0 - lo0, 0.0001f) * (hi1 - lo1);
}

// Height profile of a cloud of the given type (0 stratus .. 1 cumulus) at hn (0 base, 1 top).
float da_cloud_height_gradient(float hn, float type)
{
    const float stratus = smoothstep(0.0f, 0.10f, hn) * (1.0f - smoothstep(0.25f, 0.45f, hn));
    const float cumulus = smoothstep(0.0f, 0.14f, hn) * (1.0f - smoothstep(0.55f, 0.95f, hn));
    return lerp(stratus, cumulus, type);
}

// Density at a world point. weather = the map's (density, coverage, detail) at p.xz; hn the
// normalised height in the slab. `cheap` skips the detail erosion (the light march).
float da_cloud_density_vol(float3 p, float3 weather, float hn, bool cheap)
{
    // Soft cells: inside a cell the coverage tops out short of 1, so the shape noise still
    // carves separate clouds there instead of filling the cell with one slab.
    const float cov = weather.y * lerp(0.35f, 0.85f, saturate(da_cloud_params.x));
    [branch] if (cov <= 0.002f)
        return 0.0f;
    // Cloud type from the cell's own density: dense cores tower (cumulus), thin rims stay
    // low (stratus) - so the deck's top is bumpy per cell and never a plane seen from afar.
    // A heavy deck (high coverage) flattens toward stratus on its own.
    const float type = saturate(weather.x * 1.4f - 0.1f) * (1.0f - 0.5f * saturate((cov - 0.7f) * 3.0f));
    const float hg = da_cloud_height_gradient(hn, type);
    [branch] if (hg <= 0.001f)
        return 0.0f;

    const float2 drift = da_cloud_params.zw;
    // The shape tiles every 5 km in all three axes: billows the size of a village, as tall
    // as they are wide - the slab is thick enough now for them to stand up in it.
    float3 sp = float3(p.x - drift.x, p.y, p.z - drift.y) * (1.0f / 5000.0f);
    const float4 sh = s_cloud_shape.SampleLevel(smp_linear, sp, cheap ? 1.0f : 0.0f);
    const float fbm = sh.y * 0.625f + sh.z * 0.25f + sh.w * 0.125f;
    float base = saturate(da_remap(sh.x, -(1.0f - fbm), 1.0f, 0.0f, 1.0f)) * hg;
    // Coverage carves the base: inside a dense cell everything above a low threshold is
    // cloud, at a cell's rim only the shape's peaks survive.
    float d = saturate(da_remap(base, 1.0f - cov, 1.0f, 0.0f, 1.0f)) * cov;
    [branch] if (cheap || d <= 0.002f)
        return d;
    float3 dp = float3(p.x - drift.x * 1.25f, p.y, p.z - drift.y * 1.25f) * (1.0f / 420.0f);
    const float3 dn = s_cloud_detail.SampleLevel(smp_linear, dp, 0.0f).xyz;
    const float dfbm = dn.x * 0.625f + dn.y * 0.25f + dn.z * 0.125f;
    // Wispy at the base, billowy at the top.
    const float mod = lerp(dfbm, 1.0f - dfbm, saturate(hn * 6.0f));
    d = saturate(da_remap(d, mod * 0.35f, 1.0f, 0.0f, 1.0f));
    return d;
}


// The deck's light in the sky's units. The skybox is an LDR texture pre-exposed by the tonemap
// scale, while L_sun_color is scene HDR - lit with that, a cloud blew out to a pale wash after
// the tonemap. So: the sun as a unit-luminance tint (a sunlit top is white, 1.0), the sky as
// the horizon's fog colour (what the sky texture is made of), both a little lifted.
void da_cloud_lights(float sun_up, out float3 sun, out float3 sky)
{
    const float3 lum = float3(0.299f, 0.587f, 0.114f);
    const float3 sun_t = L_sun_color.rgb / max(dot(L_sun_color.rgb, lum), 0.05f);
    sun = sun_t * 0.95f * sun_up;
    // The flash's share of the fog colour comes back out: a bolt lights the clouds around it
    // (da_cloud_lightning), not every cloud in the sky.
    sky = saturate((fog_color.rgb - da_lightning_fog.rgb) * 1.05f + 0.06f);
}

// The glow of a discharge inside the deck: a point of light in the slab, falling off over a
// kilometre or so, scattered by the cloud around it. Dense cloud near the bolt goes white,
// the far deck stays as it was.
float3 da_cloud_lightning(float3 p, float d)
{
    [branch] if (da_lightning.w <= 0.001f)
        return 0;
    const float3 dv = da_lightning.xyz - p;
    const float r2 = dot(dv, dv);
    const float att = da_lightning.w * 4.0f / (1.0f + r2 * (1.0f / (1200.0f * 1200.0f)));
    return da_lightning2.rgb * att * (0.5f + 0.5f * saturate(d * 3.0f));
}

float da_cloud_hg(float cos_t, float g)
{
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(1.0f + g2 - 2.0f * g * cos_t, 0.0001f), 1.5f));
}

// Dual-lobe phase, normalised so a side-lit cloud sits near 1.
float da_cloud_phase(float cos_t, float g_scale)
{
    return min(12.566f * (0.7f * da_cloud_hg(cos_t, 0.62f * g_scale) + 0.3f * da_cloud_hg(cos_t, -0.28f * g_scale)), 2.5f);
}

// Optical depth toward the sun from p: four taps through the cheap density, spaced so the
// near ones catch a cloud's own edge and the far one the deck above.
float da_cloud_sun_depth(Texture2D map, float3 p, float3 to_sun, float base_alt, float thickness, float ext)
{
    const float taps[3] = { 16.0f, 48.0f, 140.0f };
    float tau = 0.0f;
    float prev = 0.0f;
    [unroll]
    for (int k = 0; k < 3; ++k)
    {
        const float3 lp = p + to_sun * taps[k];
        const float hn = (lp.y - base_alt) / thickness;
        [branch] if (hn > 1.0f || hn < 0.0f)
            break;
        const float3 w = da_cloud_field(map, lp.xz);
        tau += da_cloud_density_vol(lp, w, hn, true) * ext * (taps[k] - prev);
        prev = taps[k];
    }
    return tau;
}

// Sun light reaching a sample: Beer-Lambert with three multiple-scattering octaves, the
// phase softening with each octave, and a powder term that darkens the sun-facing edge of a
// cloud (light has not had the chance to scatter back yet).
float3 da_cloud_sun_scatter(float tau, float cos_t, float d_local)
{
    float3 acc = 0;
    float a = 1.0f, b = 1.0f, c = 1.0f;
    [unroll]
    for (int o = 0; o < 3; ++o)
    {
        acc += a * exp(-tau * b) * da_cloud_phase(cos_t, c);
        a *= 0.5f; b *= 0.5f; c *= 0.5f;
    }
    // A wide fourth term without phase: light that has scattered many times and lights the
    // base and the shaded side from within - what keeps a cloud's underside grey, not black.
    acc += 0.14f * exp(-tau * 0.12f);
    const float powder = 1.0f - 0.55f * exp(-d_local * 6.0f) * saturate(cos_t * 0.5f + 0.5f);
    return acc * powder;
}

#endif // DA_CLOUDS_VOL_H
