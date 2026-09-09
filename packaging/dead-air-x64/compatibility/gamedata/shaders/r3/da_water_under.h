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

//	A caustic is not a pattern, it is what a wavy lens does to the light going through it. The
//	water surface IS that lens, and the engine already solves its shape every frame - the wave
//	rows the surface draws and the ripple field the impacts live in. So the caustic on the bed
//	is derived from exactly those, through the thin-lens relation, and nothing else:
//
//	    intensity = 1 / | 1 + (1 - 1/n) * d * laplacian(h) |
//
//	where d is how far under the surface the bed is. A crest (negative curvature) converges the
//	light and brightens the bed beneath it, a trough spreads it. That buys, for free, the three
//	things a painted pattern can never have: the net moves at the phase speed of the very waves
//	the eye sees on the surface, its cell size is the wave size, and in a flat calm there is no
//	net at all - just as there is none on a real pond bed on a still day. A ring from a footstep
//	or a bullet casts a bright arc that runs out with it.
//
//	The first version here was three sines at 120 degrees - a hexagonal lattice, drifting along
//	the wind at close to a metre a second and swinging with its heading. Snowflakes on the move.

#define DA_UW_CAUSTIC_AMP	1.0f	// trim on the modulation; 1 is the thin-lens value
#define DA_UW_CAUSTIC_LIMIT	0.30f	// the fold: 1/this is the brightest a line can go

//	Laplacian of the surface height at a world XZ, in 1/m. The waves come straight out of the
//	rows the surface shader draws, with the same shoaling and the same clock, so the two agree
//	to the texel; the height convention there is h = A * sin(phase), so d2h/dx2 along the wave
//	is -A k^2 sin(phase). The ripple field is added as a five-tap stencil.
//
//	Turbidity blurs a caustic - scattering spreads the focused bundle before it reaches the
//	bed - and it takes the short waves first, so every term is damped by exp(-k * d * turb):
//	silt water keeps only the long slow swell of the net, peat water keeps nearly all of it.
float da_uw_curvature(float2 wxz, float depth, float turb)
{
	float lap = 0.0f;

	const float t = da_wind_state.w;
	const int n = (int)da_water_body.w;
	[loop]
	for (int i = 0; i < n; ++i)
	{
		const int lo = min(i, 3);
		const int hi = max(i - 4, 0);
		const float4 W = (i < 4) ? da_water_wave0[lo] : da_water_wave1[hi];
		[branch]
		if (W.z <= 0.00001f)
			continue;

		float sn, cs;
		sincos(W.x, sn, cs);
		const float2 dir = float2(cs, sn);
		const float shoal = da_w_shoal(W.y, depth);
		const float amp = W.z * shoal;
		const float omega = sqrt(DA_W_G * W.y * shoal);
		const float phase = W.y * dot(wxz, dir) - omega * t + W.w;

		lap -= amp * W.y * W.y * sin(phase) * exp(-W.y * depth * turb);
	}

	//	The ripple field: rings and wakes. Its texel is coarse against the wave rows, so its
	//	curvature is the honest finite difference and nothing sharper.
	[branch]
	if (da_water_rip.z > 0.0f)
	{
		const float2 uv = da_wf_ripple_uv(wxz);
		[branch]
		if (uv.x > 0.0f && uv.x < 1.0f && uv.y > 0.0f && uv.y < 1.0f)
		{
			const float e = da_water_rip.w;
			const float tm = da_water_rip.z * e;
			const float h0 = s_water_ripple.SampleLevel(smp_linear, uv, 0).r;
			const float hx1 = s_water_ripple.SampleLevel(smp_linear, uv + float2(e, 0.0f), 0).r;
			const float hx0 = s_water_ripple.SampleLevel(smp_linear, uv - float2(e, 0.0f), 0).r;
			const float hz1 = s_water_ripple.SampleLevel(smp_linear, uv + float2(0.0f, e), 0).r;
			const float hz0 = s_water_ripple.SampleLevel(smp_linear, uv - float2(0.0f, e), 0).r;
			const float2 d = min(uv, 1.0f - uv);
			const float rim = saturate(min(d.x, d.y) * 6.0f);
			//	The field's own wavelength is about a quarter metre, k ~ 25.
			lap += (hx1 + hx0 + hz1 + hz0 - 4.0f * h0) / (tm * tm) * rim * exp(-25.0f * depth * turb);
		}
	}
	return lap;
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

	//	The lens the light went through is the surface above the SUN ray, not above the pixel:
	//	trace the pixel back up to the surface plane along the light and read the surface there.
	//	That is the physically right parameterisation and it is also what keeps the pattern from
	//	smearing straight down every vertical face, which a flat XZ projection does.
	const float3 Lup = -L_sun_dir_w;
	const float cosl = max(Lup.y, 0.25f);
	const float2 hit = pos_w.xz + Lup.xz * (depth / cosl);

	//	How much the water diffuses the bundle on the way down. The column's reflectance is the
	//	one number in the profile that says how much it SCATTERS rather than absorbs: silt water
	//	glows and blurs, peat water swallows and keeps the lines crisp.
	const float turb = saturate(dot(DA_W_IOP2.xyz, float3(0.30f, 0.59f, 0.11f)) * 12.0f) * 1.5f;

	const float lap = da_uw_curvature(hit, depth, turb);

	//	The path through the water is longer than the depth when the sun is low.
	const float d = depth / cosl;

	//	Thin lens per channel. The three refractive indices of water at the red, green and blue
	//	primaries differ in the third decimal, and that difference is the coloured fringe every
	//	real caustic line carries - here it costs three divides instead of one.
	const float3 bend = float3(0.2487f, 0.2498f, 0.2554f) * d * lap;
	const float3 focus = 1.0f / max(abs(1.0f + bend), DA_UW_CAUSTIC_LIMIT);

	//	A caustic moves light, it does not add it: the mean of this over the bed is one, so
	//	switching it on neither darkens nor brightens the water as a whole.
	return lerp(1.0f, focus, DA_UW_CAUSTIC_AMP);
}

#endif // DA_WATER_UNDER_H
