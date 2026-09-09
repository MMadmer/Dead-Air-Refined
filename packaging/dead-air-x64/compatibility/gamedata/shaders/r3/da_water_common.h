#ifndef DA_WATER_COMMON_H
#define DA_WATER_COMMON_H

//	The water model: sea state, waves, surface statistics and optics.
//
//	Everything here is driven by physical quantities the engine solves on the CPU (wind speed,
//	fetch, water-body depth, extinction coefficients) rather than by hand-tuned amplitudes. The
//	scale is set once and everything follows from it: on a Zone map the fetch is 5-400 m and the
//	wind 0-12 m/s, which the fetch-limited relations turn into waves 0.5-6 cm high, 7-60 cm long
//	with a 0.2-0.6 s period. That is below a vertex and below a texel, so none of this displaces
//	geometry - it is all slope and roughness.
//
//	Slopes are accumulated in WORLD XZ and turned into a world normal at the end. The old shader
//	added world-space gradients into a tangent-space normal, which made the wind direction on
//	water depend on how the level author laid out the UVs.

#include "da_wind_field.h"

//	x = significant wave height Hs (m), y = peak wavelength (m),
//	z = Cox-Munk total mean square slope, w = the 10 m wind in m/s, time-filtered.
uniform float4 da_water_sea;
//	x = mean depth of this level's water body (m), y = the fetch it was solved with (m),
//	z = reserved, w = how many wave rows carry an amplitude.
uniform float4 da_water_body;
//	Two optical profiles, because a level may carry two water materials at once (a clear stream
//	and a green standing pool). Which one a surface reads is decided at compile time by its own
//	.s script - that is the only per-material channel the lua shader API gives us.
//	xyz = extinction sigma_t per channel (1/m), w = the profile's wave damping 0..1: a factor on
//	the slope variance of everything the wind raises, waves, detail and roughness alike.
uniform float4 da_water_iop;
uniform float4 da_water_iopb;
//	xyz = irradiance reflectance of the water column - what the body glows,
//	w = surface scum / duckweed coverage 0..1.
uniform float4 da_water_iop2;
uniform float4 da_water_iop2b;

//	Where the EYE is: x = metres it is below the surface, y = that surface's world Y, z reserved,
//	w = 1 while it is submerged. All four are zero above water. Bound by cl_da_underwater.
//
//	It describes the camera rather than the water, so it does not obviously belong here - but it
//	has to have exactly one owner. The surface (water.ps) and the full-screen medium
//	(da_water_under.h) both decide which side of the interface they are on from it, and while
//	they each kept a private copy they were free to decide it differently. They did: one flipped
//	on a hard boolean at first contact and the other ramped over the first 20 cm, so for that
//	whole band the two halves of the transition disagreed about which side you were on.
uniform float4 da_underwater;

//	The water quality ladder, published as a constant rather than a compile-time option
//	because r__water_underwater and r__water_caustics are CCC_RuntimeInteger - they change
//	live, and a shader-cache key would not follow them.
//	x = underwater tier 1..4 (1 tint, 2 +fog, 3 +warp, 4 full)
//	y = 1 when caustics are on for this preset, z/w reserved.
uniform float4 da_water_qual;

//	Metres of submersion the whole underwater stack ramps in over. Breaking the surface has to be
//	a transition and not a cut: there is a TAA pass downstream and a hard switch ghosts across it.
#define DA_UW_RAMP	0.20

//	0 above the surface, 1 once the eye is DA_UW_RAMP under it. The full-screen effects multiply
//	by it; the surface, which cannot blend two structurally different shading paths, flips at its
//	MIDPOINT - so both halves change sides at the same depth instead of 20 cm apart.
float da_uw_amount()
{
	return da_underwater.w * saturate(da_underwater.x * (1.0f / DA_UW_RAMP));
}

#ifdef DA_WATER_SLOT_B
#	define DA_W_IOP	da_water_iopb
#	define DA_W_IOP2	da_water_iop2b
#else
#	define DA_W_IOP	da_water_iop
#	define DA_W_IOP2	da_water_iop2
#endif
//	Eight waves, pre-transposed on the CPU like the wind motors and the impact slots.
//	Row = (theta, k, A, phi): heading in world XZ radians, wavenumber 2pi/lambda, amplitude in
//	metres, phase offset. Dispersion and the depth cut are evaluated here, per pixel.
uniform float4x4 da_water_wave0;
uniform float4x4 da_water_wave1;

#define DA_W_G 9.81f

//	How much of the wave spectrum survives at this depth. Deep water leaves it alone; the last
//	metre before a bank takes the long waves out first, which is why real shallows go glassy
//	while the middle of the pond is still ruffled.
float da_w_shoal(float k, float h)
{
	return tanh(k * clamp(h, 0.02f, 8.0f));
}

//	The wave field at a point: the world-XZ slope of the sum, and the slope variance it carries
//	(so the microfacet lobe below can subtract what is already an explicit wave and not count
//	the same roughness twice).
float2 da_w_waves(float2 wxz, float h, float t, out float mss_explicit)
{
	float2 slope = 0.0f;
	float  var = 0.0f;
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

		float s, c;
		sincos(W.x, s, c);
		const float2 dir = float2(c, s);

		const float shoal = da_w_shoal(W.y, h);
		const float amp = W.z * shoal;
		//	Depth-limited dispersion: omega^2 = g*k*tanh(kh). Shallow water slows the long
		//	waves down, so the whole train visibly drags as it runs into the bank.
		const float omega = sqrt(DA_W_G * W.y * shoal);
		const float phase = W.y * dot(wxz, dir) - omega * t + W.w;

		slope += dir * (amp * W.y * cos(phase));
		var += 0.5f * (amp * W.y) * (amp * W.y);
	}

	mss_explicit = var;
	return slope;
}

//	Sub-wave detail. Two layers of the animated normal map in world space, rotated against each
//	other so the two lattices never beat (the stock pair sampled the same map at 1.0 and 1.1 in
//	the same orientation, which is a moire generator). Drifts downwind at the phase speed of a
//	wave its own size, so it reads as part of the same sea rather than a sliding texture.
//
//	The lattices and the mix are one piece of code with two ways of sampling it: the surface
//	takes the gradient-selected mip, the caustic under it asks for an explicit level (a Sample
//	inside the caustic's branch is a compile error, and the level is chosen to match the
//	finite-difference spacing there). Both see the SAME ripples, which is the whole point.
void da_w_detail_uv(float2 wxz, float2 wdir, float t, float ms, out float2 uv0, out float2 uv1, out float2 wd1)
{
	const float2 across = float2(-wdir.y, wdir.x);
	const float2 f0 = float2(dot(wxz, wdir), dot(wxz, across));
	//	~27 degrees off the wind for the second lattice.
	wd1 = float2(wdir.x * 0.8763f - wdir.y * 0.4818f, wdir.x * 0.4818f + wdir.y * 0.8763f);
	const float2 f1 = float2(dot(wxz, wd1), dot(wxz, float2(-wd1.y, wd1.x)));

	//	Phase speed of a 12 cm gravity-capillary wave, nudged by the wind.
	const float drift = t * (0.30f + 0.030f * ms);
	uv0 = f0 * float2(1.35f, 1.90f) - float2(drift, 0.0f);
	uv1 = f1 * float2(2.60f, 3.30f) - float2(drift * 1.7f, drift * 0.31f) + 0.37f;
}

float2 da_w_detail_mix(float2 r0, float2 r1, float2 wdir, float2 wd1)
{
	const float2 s0 = float2(r0.x * wdir.x - r0.y * wdir.y, r0.x * wdir.y + r0.y * wdir.x);
	const float2 s1 = float2(r1.x * wd1.x - r1.y * wd1.y, r1.x * wd1.y + r1.y * wd1.x);
	return s0 + s1 * 0.62f;
}

float2 da_w_detail(Texture2D nmap, SamplerState smp, float2 wxz, float2 wdir, float t, float ms)
{
	float2 uv0, uv1, wd1;
	da_w_detail_uv(wxz, wdir, t, ms, uv0, uv1, wd1);
	const float2 r0 = nmap.Sample(smp, uv0).xy - 0.5f;
	const float2 r1 = nmap.Sample(smp, uv1).xy - 0.5f;
	return da_w_detail_mix(r0, r1, wdir, wd1);
}

float2 da_w_detail_lod(Texture2D nmap, SamplerState smp, float2 wxz, float2 wdir, float t, float ms, float lod)
{
	float2 uv0, uv1, wd1;
	da_w_detail_uv(wxz, wdir, t, ms, uv0, uv1, wd1);
	const float2 r0 = nmap.SampleLevel(smp, uv0, lod).xy - 0.5f;
	const float2 r1 = nmap.SampleLevel(smp, uv1, lod).xy - 0.5f;
	return da_w_detail_mix(r0, r1, wdir, wd1);
}

//	The profile's damping as bound, or one: a build that predates the constant reads a zero
//	there, and a zero would be a dead calm on every water in the game.
float da_w_damp(float bound)
{
	return (bound > 0.001f) ? bound : 1.0f;
}

//	Cox & Munk 1954: the mean square slope of a wind-roughened surface, which is the capillary
//	ripple nobody can render as a normal. Clean water is the standard fit; a sheltered marsh
//	with scum on it damps like an oil slick, which is exactly the "slick" branch of the same
//	paper. Modulated per pixel by the gust field, so a squall's cat's paws stretch the glitter
//	into tongues for the price of one lerp - the single most recognisable wind-on-water cue.
//
//	Below their own data the fits are wrong for a pond: the 0.003 intercept (0.008 on a slick) is
//	the ocean's residual swell, and a pond has none. Under about half a metre a second the wind
//	raises no capillary at all and the water is a mirror, so the threshold comes off the wind and
//	the law fades in over the next metre a second. The gust field goes in before the threshold,
//	which is what makes cat's paws: in a light air the lulls go glassy and the tongues ripple.
//	CEnvironment's water_mss is the same law for the wave budget and must stay so.
float da_w_mss(float ms, float scum, float gust)
{
	const float u = max(ms * gust - 0.5f, 0.0f);
	const float clean = 0.003f + 5.12e-3f * u;
	const float slick = 0.0024f + 1.56e-3f * u;
	return 1e-4f + lerp(clean, slick, saturate(scum)) * min(u, 1.0f);
}

//	Schlick against water's IOR 1.333. Worst absolute error against the exact unpolarised
//	Fresnel is 0.058 near 85 degrees, which nothing in this game will notice.
float da_w_fresnel_up(float ndv)
{
	const float f = 1.0f - saturate(ndv);
	const float f2 = f * f;
	return 0.02037f + 0.97963f * (f2 * f2 * f);
}

//	Bruneton's mean Fresnel over a rough surface. Without it a choppy far shore goes full mirror,
//	because the flat-surface Fresnel at grazing incidence knows nothing about the slopes it is
//	averaging over.
float da_w_fresnel_rough(float ndv, float mss)
{
	const float sig = sqrt(max(mss, 1e-6f));
	const float e = 5.0f * exp(-2.69f * sig);
	return saturate(pow(1.0f - saturate(ndv), e) / (1.0f + 22.7f * pow(sig, 1.5f)));
}

//	Looking UP from inside the water. Schlick is wrong by 37x at the critical angle and can never
//	produce Snell's window, so this one is the real thing: total internal reflection past
//	48.607 degrees, an exact unpolarised average below it.
float da_w_fresnel_down(float ndv)
{
	const float ci = saturate(ndv);
	const float si = sqrt(max(0.0f, 1.0f - ci * ci)) * 1.333f;
	[branch]
	if (si >= 1.0f)
		return 1.0f;
	const float ct = sqrt(max(0.0f, 1.0f - si * si));
	const float rs = (1.333f * ci - ct) / (1.333f * ci + ct);
	const float rp = (1.333f * ct - ci) / (1.333f * ct + ci);
	return saturate(0.5f * (rs * rs + rp * rp));
}

//	GGX with Smith height-correlated masking, driven straight by the Cox-Munk slope variance.
//	This replaces pow(dot(N,H), 280): that exponent is a permanent 0.8 m/s breeze, which is why
//	the sun on the water never changed with the weather.
//
//	The lobe is normalised to a peak of one rather than left energy-conserving. The frame this
//	writes into is 8-bit, and a physically normalised GGX at calm-water roughness peaks around
//	1/(pi*a^2) ~ 100, which would land as a hard-edged white disc with everything around it
//	clipped. Normalised, the SHAPE stays exactly right - a tight bright spot in a calm, a broad
//	dim glitter path in a blow - which is the whole point of driving it from the wind.
float da_w_specular(float3 N, float3 V, float3 L, float mss)
{
	const float a = clamp(sqrt(mss), 0.02f, 0.6f);
	const float3 H = normalize(V + L);
	const float ndh = saturate(dot(N, H));
	const float ndv = saturate(dot(N, V));
	const float ndl = saturate(dot(N, L));

	const float a2 = a * a;
	const float d = ndh * ndh * (a2 - 1.0f) + 1.0f;
	const float D = a2 / max(3.14159265f * d * d, 1e-7f);
	const float lv = ndl * sqrt(ndv * ndv * (1.0f - a2) + a2);
	const float ll = ndv * sqrt(ndl * ndl * (1.0f - a2) + a2);
	const float Vis = 0.5f / max(lv + ll, 1e-7f);
	//	Below the horizon of either the eye or the light there is no lobe at all.
	const float on = step(0.0001f, ndl) * step(0.0001f, ndv);
	return D * (3.14159265f * a2) * Vis * ndl * on;
}

//	Beer-Lambert down the actual path the light took through the water. Not a lerp between two
//	authored tints: red is half gone at 2 m and dead at 8, which is the entire reason clear water
//	is blue and peat water is brown, and it is why the colour has to SHIFT with depth rather than
//	just darken.
float3 da_w_transmit(float path)
{
	return exp(-DA_W_IOP.xyz * max(path, 0.0f));
}

//	What the water column itself glows: its irradiance reflectance times the light falling on it,
//	times the interface loss on the way back out. Radiance is divided by n^2 = 1.777 crossing
//	back into air, times about 0.98 of transmission - so 0.54. Missing that factor is why hand
//	tuned water usually ends up about twice too bright.
float3 da_w_inscatter(float3 irradiance, float3 T)
{
	return DA_W_IOP2.xyz * irradiance * 0.54f * (1.0f - T);
}

#endif // DA_WATER_COMMON_H
