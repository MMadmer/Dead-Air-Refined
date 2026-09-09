#ifndef DA_WATER_UNDER_H
#define DA_WATER_UNDER_H

//	The world seen from inside the water, and the light that got there.
//
//	The full-screen medium in combine_2: the same Beer-Lambert optics the surface uses
//	(DESIGN.md part 1), run over the whole frame by scene distance, with an in-scatter floor so
//	the far end of a pond goes to the water's own colour instead of to black. And, for the eye
//	that is under the surface, the sun's caustic on the bed - the lens itself is
//	da_water_caustic.h, shared with the surface pass, which also explains why neither lives in
//	the sun accumulator any more.

#include "common.h"
//	The optics are part 1's, not a second set: da_w_transmit and da_w_inscatter carry the same
//	sigma_t and the same interface loss the surface itself uses, so the water cannot end up one
//	colour from above and another from below. It also settles the declaration order - the two
//	headers would otherwise fight over da_water_iop depending on who was included first.
#include "da_water_common.h"
//	The caustics ask the shared field reader where the water is, rather than growing a second
//	answer to the same question - see da_water_field.h's own list of consumers.
#include "da_water_field.h"
#include "da_water_caustic.h"

//	The surface's own detail normal map ("water\water_normal", the s_nmap of the water .s
//	scripts), bound for the combine_2 elements by blender_combine.cpp.
Texture2D s_water_nmap;

//	da_underwater, DA_UW_RAMP and da_uw_amount() live in da_water_common.h above: the surface
//	shader needs the same three and two copies of the ramp is how the two halves of the
//	transition drifted apart in the first place.

//	da_water_qual, the quality ladder, is declared in da_water_common.h: the surface pass reads
//	it too now.

// ---- tunables ---------------------------------------------------------------------------
//	Tier to assume when da_water_qual is not bound (an engine that predates it reads zeros).
//	Default-preset behaviour, so an unbound build looks right rather than looking broken.
#define DA_UW_TIER_FALLBACK	3

//	The image seen through the surface from below is displaced by the surface slope. These are
//	the period and the crest count of the 0.6-1 m gravity waves that carry most of it, not a
//	generic screen wobble: T = sqrt(2*pi*L/g) is 0.8 s at a metre, so omega lands near 8.
#define DA_UW_WARP_TILE		3.5	// crests across the screen
#define DA_UW_WARP_AMP		0.0045	// screen share at full strength
//	Radial chromatic separation and vignette, top tier only.
#define DA_UW_CHROMA		0.0060
#define DA_UW_VIGNETTE		0.55
//	Scales the in-scatter floor against the engine's own sun and hemi. A look control: the
//	engine's light units are not radiometric, so the 0.54 interface loss alone cannot land it.
#define DA_UW_FOG_GAIN		0.85

//	Caustics. Cells are 40-50 cm on a real bed, which is a tile of a bit over two per metre.

// ---- the medium ---------------------------------------------------------------------------

int da_uw_tier()
{
	return (da_water_qual.x > 0.5f) ? (int)da_water_qual.x : DA_UW_TIER_FALLBACK;
}

//	The light that actually gets down to the eye: sun by its own elevation plus the hemisphere,
//	both carried through the column above. Driven by the engine's own sun and hemi, so the murk
//	warms at sunset and goes black at night without a second set of knobs to keep in agreement.
float3 da_uw_irradiance()
{
	const float3 sky = L_sun_color * saturate(-L_sun_dir_w.y) + L_hemi_color.rgb;
	return sky * da_w_transmit(max(da_underwater.x, 0.0f)) * DA_UW_FOG_GAIN;
}

//	Beer-Lambert down the actual eye-to-pixel path, plus the column's own glow as the floor that
//	distance falls to. Red is half gone at 2 m and dead at 8, which is why looking sideways along
//	a pond bed goes green-brown while a pebble at arm's length keeps its colour - and why the far
//	end has to land on the water's colour and not on black.
float3 da_uw_medium(float3 c, float dist, float amt, int tier)
{
	const float3 T = da_w_transmit(dist);
	float3 wet = c * T;
	if (tier >= 2)
		wet += da_w_inscatter(da_uw_irradiance(), T);
	return lerp(c, wet, amt);
}

//	Refraction of the frame by the surface above. Two crossed trains, and the displacement falls
//	off as the eye sinks because the lens gets further away - at three metres down the ceiling
//	is too far to bend the image much, which is exactly how deep water reads.
float2 da_uw_warp(float2 uv, float amt)
{
	const float t = timers.x;
	const float2 p = uv * DA_UW_WARP_TILE;
	float2 d;
	d.x = sin(p.y * 6.2831853f + t * 7.85f) + 0.55f * sin(p.x * 1.7f - t * 5.60f);
	d.y = cos(p.x * 6.2831853f + t * 6.40f) + 0.55f * cos(p.y * 1.7f + t * 4.90f);
	const float range = lerp(0.35f, 1.0f, saturate(2.0f / (1.0f + da_underwater.x)));
	return d * (DA_UW_WARP_AMP * amt * range);
}

//	Radial chromatic separation, top tier only. Explicit LOD: this is called under a branch and
//	an implicit-gradient Sample there is a compile error waiting for the first person who moves
//	the call. s_image has no mip chain, so level 0 is the same tap either way.
float3 da_uw_chroma(float2 uv, float3 c, float amt)
{
	const float2 shift = (uv - 0.5f) * (DA_UW_CHROMA * amt);
	float3 o = c;
	o.r = s_image.SampleLevel(smp_rtlinear, uv + shift, 0).r;
	o.b = s_image.SampleLevel(smp_rtlinear, uv - shift, 0).b;
	return o;
}

float3 da_uw_vignette(float3 c, float2 uv, float amt)
{
	const float2 d = uv - 0.5f;
	const float r = saturate(dot(d, d) * 4.0f);
	return c * (1.0f - r * r * (DA_UW_VIGNETTE * amt));
}

// ---- caustics, seen from under the surface --------------------------------------------------

//	The net on the bed when the EYE is under water. Above the surface water.ps puts it on the
//	refracted background (da_water_caustic.h says why the sun pass cannot); below it there is
//	no surface pass between the eye and the bed, so the medium pass does the same to the frame
//	itself - the pixel's depth under the surface and the lens above it come from the field.
//	What this pass does not have is the sun's access at the pixel, only the sun above the
//	horizon and its share of the sky, so a bed in a tree's shadow gets a net it should not: the
//	price of a full-screen pass with no lightmap in it.
float3 da_uw_caustics_scene(float3 pos_v)
{
	//	Uniform over the whole pass: off preset, or a level with no baked field. Both of these
	//	are constants, so a dry level pays one compare for the entire screen.
	[branch]
	if (da_water_qual.y < 0.5f || da_water_map2.w < 0.5f)
		return 1.0f;

	//	View space to world, same reconstruction as da_puddles.h.
	const float3x3 V_rot = float3x3(m_V[0].xyz, m_V[1].xyz, m_V[2].xyz);
	const float3 V_ofs = float3(m_V[0].w, m_V[1].w, m_V[2].w);
	const float3 pos_w = mul(transpose(V_rot), pos_v - V_ofs);

	const float4 wf = da_wf_sample(pos_w.xz);
	const float depth = wf.x - pos_w.y;
	[branch]
	if (wf.y < 0.5f || depth <= 0.02f)
		return 1.0f;

	float ms;
	float2 wdir;
	const float mss_total = da_wc_wind(pos_w.xz, ms, wdir);

	//	The direct sun's share of what lights the bed: the rest is sky, and sky throws no net.
	const float3 lum = float3(0.30f, 0.59f, 0.11f);
	const float3 sun = L_sun_color.rgb * saturate(-L_sun_dir_w.y);
	const float sun_share = saturate(dot(sun, lum) / max(dot(sun + L_hemi_color.rgb, lum), 1e-3f));

	const float3 focus = da_wc_focus(s_water_nmap, smp_linear, pos_w, depth, ms, wdir, mss_total);
	return lerp(1.0f, focus, sun_share);
}

#endif // DA_WATER_UNDER_H
