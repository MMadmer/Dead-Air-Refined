#ifndef DA_WATER_UNDER_H
#define DA_WATER_UNDER_H

//	The world seen from inside the water, and the light that got there.
//
//	Two halves that share one set of constants. The first is the full-screen medium in
//	combine_2: the same Beer-Lambert optics the surface uses (DESIGN.md part 1), run over the
//	whole frame by scene distance, with an in-scatter floor so the far end of a pond goes to the
//	water's own colour instead of to black. The second is the caustic net, which lives in the
//	SUN accumulator rather than in a decal - that is the whole point of putting it there: it
//	vanishes in shadow and warms at sunset because the term it multiplies does.
//
//	The caustic PATTERN is procedural on purpose: the accumulators bind their samplers from C++
//	blenders, and a scrolling cell pattern is not worth a new texture stage there. The one map
//	the caustics do read is the baked water field, which they need anyway to know where the
//	water is and how deep the pixel sits under it.

#include "common.h"
//	The optics are part 1's, not a second set: da_w_transmit and da_w_inscatter carry the same
//	sigma_t and the same interface loss the surface itself uses, so the water cannot end up one
//	colour from above and another from below. It also settles the declaration order - the two
//	headers would otherwise fight over da_water_iop depending on who was included first.
#include "da_water_common.h"
//	The caustics ask the shared field reader where the water is, rather than growing a second
//	answer to the same question - see da_water_field.h's own list of consumers.
#include "da_water_field.h"

//	da_underwater, DA_UW_RAMP and da_uw_amount() live in da_water_common.h above: the surface
//	shader needs the same three and two copies of the ramp is how the two halves of the
//	transition drifted apart in the first place.

//	The water quality ladder, published as a constant rather than a compile-time option
//	because r__water_underwater and r__water_caustics are CCC_RuntimeInteger - they change
//	live, and a shader-cache key would not follow them.
//	x = underwater tier 1..4 (1 tint, 2 +fog, 3 +warp, 4 full)
//	y = 1 when caustics are on for this preset, z/w reserved.
uniform float4 da_water_qual;

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
#define DA_UW_CAUSTIC_TILE	2.2
#define DA_UW_CAUSTIC_RATIO	1.31	// second layer's relative scale - the standard pairing
#define DA_UW_CAUSTIC_FOCUS	3.0	// metres below the surface where the net has washed out
#define DA_UW_CAUSTIC_AMP	0.85	// peak +-modulation of the diffuse sun term
#define DA_UW_CAUSTIC_MED	0.55	// the pattern's median: where the net's edge sits
#define DA_UW_CAUSTIC_DISP	0.012	// per-channel offset in cell units - the dispersion

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

// ---- caustics -------------------------------------------------------------------------------

//	Three plane waves 120 degrees apart. A caustic is the FOLD of a wavy lens, so the net is the
//	ridge line of each family - the max of the three - and not their peaks: peaks would give
//	isolated dots on a hex lattice, ridges give the triangular mesh of lines a pool bottom
//	actually carries. Three sines, no hashes, and it tiles for free.
float da_uw_cell(float2 q)
{
	const float w0 = sin(6.2831853f * q.x);
	const float w1 = sin(6.2831853f * (-0.5f * q.x + 0.8660254f * q.y));
	const float w2 = sin(6.2831853f * (-0.5f * q.x - 0.8660254f * q.y));
	return max(max(1.0f - abs(w0), 1.0f - abs(w1)), 1.0f - abs(w2));
}

//	Two of them at 1.3x relative scale drifting apart, multiplied: the beat between the two
//	lattices is what stops the pattern reading as wallpaper.
float da_uw_net(float2 q, float2 dr0, float2 dr1)
{
	return da_uw_cell(q + dr0) * da_uw_cell(q * DA_UW_CAUSTIC_RATIO + dr1);
}

//	The multiplier for the sun's diffuse term at a pixel under the water. Takes VIEW space,
//	because that is what the accumulators have and because the early-outs below are then paid
//	before the world reconstruction is.
float3 da_uw_caustics(float3 pos_v)
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

	//	One field fetch answers both questions - is there water over this pixel, and how far
	//	under its surface the pixel sits. Per pixel and not per camera, so a bed lit through
	//	knee-deep water gets its net while the eye is still in the air.
	const float4 wf = da_wf_sample(pos_w.xz);
	const float depth = wf.x - pos_w.y;
	[branch]
	if (wf.y < 0.5f || depth <= 0.0f)
		return 1.0f;

	//	The net is carried by the light, so it is constant ALONG the sun ray: trace the pixel
	//	back up to the surface plane and use where it crossed. That is the physically right
	//	parameterisation and it is also what keeps the pattern from smearing straight down every
	//	vertical face, which a flat XZ projection does.
	const float3 Lup = -L_sun_dir_w;
	const float2 hit = pos_w.xz + Lup.xz * (depth / max(Lup.y, 0.25f));
	const float2 q = hit * DA_UW_CAUSTIC_TILE;

	//	Sharp just under the surface, washed out by a couple of metres: the wave lens has a focal
	//	length. The amplitude carries the water's own extinction on top, per channel - so a deep
	//	bed loses the red of the net first and then the net itself, because no light gets there.
	const float focus = saturate(1.0f - depth * (1.0f / DA_UW_CAUSTIC_FOCUS));
	const float3 amp = (DA_UW_CAUSTIC_AMP * focus) * da_w_transmit(depth);

	//	Drift DOWNWIND, on the wind service's own clock - the same clock the waves upstairs run
	//	on, so the net travels with the swell that casts it instead of on a private diagonal.
	//	The two layers differ slightly in rate and heading; that mismatch is what makes the
	//	pattern evolve rather than slide.
	const float t = da_wind_state.w;
	const float2 wd = da_wind_state.xy;
	const float2 wa = float2(-wd.y, wd.x);
	const float2 dr0 = -(wd * 0.77f) * t;
	const float2 dr1 = -(wd * 0.55f + wa * 0.18f) * t;

	//	Per-channel offset along the refraction azimuth. The cheapest realism in the whole water
	//	rework: one mad per channel buys the coloured fringe every real caustic has.
	float2 disp = Lup.xz;
	const float dlen = length(disp);
	disp = (dlen > 1e-4f) ? disp * (DA_UW_CAUSTIC_DISP / dlen) : float2(DA_UW_CAUSTIC_DISP, 0.0f);

	float3 net;
	net.r = da_uw_net(q + disp, dr0, dr1);
	net.g = da_uw_net(q, dr0, dr1);
	net.b = da_uw_net(q - disp, dr0, dr1);

	//	Focus rides the WIDTH of the threshold rather than a pow: a narrow band is a thin bright
	//	net, a wide one is soft blotches, and both stay centred on the pattern's median so the
	//	modulation averages to about one. A caustic moves light, it does not add it - miss that
	//	and switching caustics on visibly darkens or brightens the whole bed.
	const float w = lerp(0.34f, 0.09f, focus);
	const float3 c = smoothstep(DA_UW_CAUSTIC_MED - w, DA_UW_CAUSTIC_MED + w, net);
	return 1.0f + (c * 2.0f - 1.0f) * amp;
}

#endif // DA_WATER_UNDER_H
