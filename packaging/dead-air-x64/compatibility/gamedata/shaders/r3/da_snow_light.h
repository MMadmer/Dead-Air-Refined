// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Lighting of snow pixels (material code 31, docs/dead-air/SNOW.md). lmodel.h calls it for every
// da_is_snow(m) pixel with the LUT result of slice 0 (rgb = diffuse, w = specular) and returns what it
// gives back:
//   float4 da_snow_light(float4 lut_light, float3 N, float3 V, float3 L, bool glint)
// Diffuse: a share of the light goes through UE's SubsurfaceBxDF (wrapped diffuse, back scatter and the
// forward in-scatter lobe) tinted toward blue, which scatters furthest in snow; the rest stays on the
// stock lobe. The sun's regular shadow applies to both (no separate transmission shadow).
// Colour: the snow layer gives the snow's albedo a slight cool cast (DA_SNOW_TINT), so what the sky's
// hemisphere lights - shade, overcast - stays blue-grey as it does on real snow even under the warm
// ambient of the autumn weathers; the sun and the lights are divided by it here, so sunlit snow keeps
// its neutral white.
// Specular: the snow layer turns the normal of a glinting cell exactly toward the sun's half vector and marks
// the pixel with the glint code (da_material_codes.h), and a very narrow extra lobe lights those pixels alone
// (world-anchored sparkle; no time term); their gloss carries the flash's brightness, DA_SNOW_GLINT_SCALE times
// the snow's diffuse light at gloss 1. On plain snow that lobe drew the sun's mirror image as a glossy
// blotch round its reflection point wherever the surface ran smooth.
#ifndef DA_SNOW_LIGHT_H
#define DA_SNOW_LIGHT_H

// Luminance 1 (Rec. 709 weights).
#define DA_SNOW_TINT float3(0.952, 1.0044, 1.098)
// The sky's light on snow after the white balance (combine_1.ps), luminance 1: the open sky lights snow from
// a blue hemisphere, so shade and overcast read cool grey while the sun keeps sunlit snow neutral.
#define DA_SNOW_SKY_TINT float3(0.93, 1.0087, 1.12)
#define DA_SNOW_SSS_OPACITY 0.85
#define DA_SNOW_SSS_SHARE 0.45
#define DA_SNOW_GLINT 16.0
// About how many times the snow's diffuse light a glint pixel of gloss 1 shines (combine_1.ps: five times the
// gloss times the light's specular over the light).
#define DA_SNOW_GLINT_SCALE 100.0
#define DA_SNOW_GLINT_PEAK 10.0

float4 da_snow_light(float4 lut_light, float3 N, float3 V, float3 L, bool glint)
{
	const float NoL = dot(N, L);
	// UE: WrappedDiffuse = pow(saturate(N.L / 1.5 + 0.5 / 1.5), 1.5) * (2.5 / 1.5)
	const float wrapped = pow(saturate(NoL * (1.0 / 1.5) + (0.5 / 1.5)), 1.5) * (2.5 / 1.5);
	const float normal_contribution = lerp(1.0, wrapped, DA_SNOW_SSS_OPACITY);
	// BackScatter = NormalContribution / (2 pi), in units of the Lambert lobe (1 / pi).
	const float back_scatter = normal_contribution * 0.5;
	const float in_scatter = pow(saturate(dot(L, -V)), 12.0) * lerp(3.0, 0.1, DA_SNOW_SSS_OPACITY);
	const float3 transmission = lerp(back_scatter.xxx, float3(1.0, 1.0, 1.0), in_scatter) * float3(0.80, 0.90, 1.00);

	float4 light = lut_light;
	light.rgb = (lut_light.rgb * (1.0 - DA_SNOW_SSS_SHARE) + transmission * DA_SNOW_SSS_SHARE) / DA_SNOW_TINT;

	[branch] if (glint)
	{
		const float NoH = saturate(dot(N, normalize(L + V)));
		// (held under some sixty times the snow's light: brighter, a flash under the temporal resolve and bloom
		// flickered from frame to frame)
		light.w += min(DA_SNOW_GLINT * pow(NoH, 8192.0) * saturate(NoL * 4.0), DA_SNOW_GLINT_PEAK);
	}
	return light;
}

// White balance of the light on snow (combine_1.ps). The eye takes snow for white under any daylight, while
// the autumn weathers' sun and sky are warm enough to turn it cream: a snow pixel's sun and local lights are
// balanced by the sun's colour, its sky term by its own, so sunlit snow comes out near neutral and the
// albedo's cool cast (DA_SNOW_TINT) reads in the shade and under overcast. Partial and clamped, so a low
// red sun still warms the snow; faded out at night, when the moonlight's blue is the look. The weight
// follows the pixel's whiteness (G-buffer albedo as stored), so ground showing through thin snow keeps its
// own colours.
float3 da_snow_wb_gain(float3 c)
{
	const float y = dot(c, float3(0.2126, 0.7152, 0.0722));
	return clamp(y / max(c, 1e-4), 0.7, 1.45);
}

float da_snow_wb_weight(float3 albedo_stored, float3 sun)
{
	const float day = smoothstep(0.05, 0.3, dot(sun, float3(0.2126, 0.7152, 0.0722)));
	return 0.85 * day * smoothstep(0.55, 0.8, dot(albedo_stored, float3(0.2126, 0.7152, 0.0722)));
}

#endif // DA_SNOW_LIGHT_H
