#ifndef DA_WATER_CAUSTIC_H
#define DA_WATER_CAUSTIC_H

//	The caustic: what the water surface, as a lens, does to the sun on whatever is under it.
//
//	It is not a pattern of its own. The engine solves the surface's shape every frame - the wave
//	rows, the detail map at the wind's own amplitude, the ripple field the impacts live in - and
//	the bed is lit through the thin-lens relation on exactly those:
//
//	    intensity = 1 / | 1 + (1 - 1/n) * d * laplacian(h) |
//
//	d being how far under the surface the bed is, along the sun. A crest (negative curvature)
//	converges the light and brightens the bed beneath it, a trough spreads it. That buys, for
//	free, the three things a painted pattern can never have: the net moves at the phase speed of
//	the very waves the eye sees, its cells are the detail layer's ten to thirty centimetres, and
//	in a flat calm there is no net at all - as there is none on a real pond bed on a still day. A
//	ring from a footstep or a bullet casts a bright arc that runs out with it.
//
//	WHERE it is applied is not a free choice either. The obvious home is the sun accumulator -
//	modulate the sun's own term and the net vanishes in shadow and warms at sunset for free.
//	That is where the first two versions lived, and neither ever lit a bed: the level compiler
//	bakes the terrain's sun occlusion with the water surface as an occluder, so a bed under
//	water has no dynamic sun in the deferred lighting to modulate. Only the plants standing in
//	the water have, which is exactly what "snowflakes on the plants, not on the bottom" was. So
//	the surface pass applies it to the refracted background itself (water.ps), and the medium
//	pass to the frame when the eye is under the surface (da_water_under.h); this header is the
//	lens both of them share.
//
//	Turbidity does not blur a caustic by wavelength, it takes light OUT of the sharp image: what
//	got down unscattered still draws the line, the rest is the diffuse glow the column already
//	accounts for. That is the exp(-b d) contrast below, with b the scattering share of the
//	profile's extinction - and it is the term whose first version, twenty times too strong,
//	removed the caustic altogether.

#include "da_water_common.h"
#include "da_water_field.h"
//	The detail layer's share of the slope budget and its map gain: the caustic must put the
//	same ripples on the bed that the surface puts in the eye.
#include "settings_da_water.h"

#define DA_WC_LIMIT		0.30f	// the fold: 1/this is the brightest a line can go
//	The detail layer is differenced over this spacing, from the mip whose texel is half of it -
//	a difference over a mip-0 texel would be reading the map's own noise.
#define DA_WC_EPS		0.03f	// metres
#define DA_WC_LOD		3.5f	// ~1 cm texels on the shipped 512 map at 1.35 tiles/m
//	That mip and the difference over three centimetres together read back about a third of the
//	curvature the map really carries (each of them a sinc of the ripple wavelength); this puts
//	it back. Measured on the rig: at 1.5 the net on a 30 cm marsh bed under 5 m/s was a fifth
//	either way of the mean, a real pond's is a half.
#define DA_WC_DETAIL	2.5f

//	Laplacian of the surface height at a world XZ, in 1/m: the wave rows straight out of the
//	table the surface draws (h = A * sin(phase), so d2h/dx2 along the wave is -A k^2 sin), the
//	divergence of the detail layer's slope at the amplitude water.ps gives it, and a five-tap
//	stencil over the ripple field.
float da_wc_curvature(Texture2D nmap, SamplerState smp, float2 wxz, float depth, float ms, float2 wdir, float mss_total)
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

		lap -= amp * W.y * W.y * sin(phase);
	}

	//	The detail layer: the same two lattices at the same amplitude law as the surface
	//	(gain * sqrt(share * mss)), differenced along both axes. Six taps of the detail map, at
	//	an explicit level - this runs under a branch, where a gradient sample is a compile error.
	const float det_amp = WATER_DETAIL_GAIN * sqrt(WATER_DETAIL_SHARE * max(mss_total, 0.0f));
	[branch]
	if (det_amp > 0.001f)
	{
		const float2 s0 = da_w_detail_lod(nmap, smp, wxz, wdir, t, ms, DA_WC_LOD);
		const float2 sx = da_w_detail_lod(nmap, smp, wxz + float2(DA_WC_EPS, 0.0f), wdir, t, ms, DA_WC_LOD);
		const float2 sz = da_w_detail_lod(nmap, smp, wxz + float2(0.0f, DA_WC_EPS), wdir, t, ms, DA_WC_LOD);
		lap += ((sx.x - s0.x) + (sz.y - s0.y)) * (det_amp * DA_WC_DETAIL / DA_WC_EPS);
	}

	//	The ripple field: rings and wakes. Its texel is coarse against the detail layer, so its
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
			const float h0 = s_water_ripple.SampleLevel(smp_rtlinear, uv, 0).r;
			const float hx1 = s_water_ripple.SampleLevel(smp_rtlinear, uv + float2(e, 0.0f), 0).r;
			const float hx0 = s_water_ripple.SampleLevel(smp_rtlinear, uv - float2(e, 0.0f), 0).r;
			const float hz1 = s_water_ripple.SampleLevel(smp_rtlinear, uv + float2(0.0f, e), 0).r;
			const float hz0 = s_water_ripple.SampleLevel(smp_rtlinear, uv - float2(0.0f, e), 0).r;
			const float2 d = min(uv, 1.0f - uv) * da_water_rip.z;
			const float rim = saturate(min(d.x, d.y) * (1.0f / DA_WF_RIM_M));
			lap += (hx1 + hx0 + hz1 + hz0 - 4.0f * h0) / (tm * tm) * rim;
		}
	}
	return lap;
}

//	The lens over one bed point: bed_w its world position, depth the water standing over it.
//	Returns the multiplier on the light reaching it, per channel, with the turbidity contrast
//	already in. The wind arguments are the surface's own - water.ps has them in hand, the
//	medium pass rebuilds them with da_wc_wind.
float3 da_wc_focus(Texture2D nmap, SamplerState smp, float3 bed_w, float depth, float ms, float2 wdir, float mss_total)
{
	//	The lens the light went through is the surface above the SUN ray, not above the bed:
	//	trace the bed point back up to the surface plane along the light and read it there.
	//	That is the right parameterisation and it is also what keeps the pattern from smearing
	//	straight down every vertical face, which a flat XZ projection does.
	const float3 Lup = -L_sun_dir_w;
	const float cosl = max(Lup.y, 0.25f);
	const float2 hit = bed_w.xz + Lup.xz * (depth / cosl);
	const float lap = da_wc_curvature(nmap, smp, hit, depth, ms, wdir, mss_total);

	//	The path through the water is longer than the depth when the sun is low.
	const float d = depth / cosl;

	//	Thin lens per channel. The three refractive indices of water at the red, green and blue
	//	primaries differ in the third decimal, and that difference is the coloured fringe every
	//	real caustic line carries - here it costs three divides instead of one.
	const float3 bend = float3(0.2487f, 0.2498f, 0.2554f) * d * lap;
	const float3 focus = 1.0f / max(abs(1.0f + bend), DA_WC_LIMIT);

	//	Contrast: the unscattered share, see the note at the top.
	const float3 lum = float3(0.30f, 0.59f, 0.11f);
	const float b = dot(DA_W_IOP.xyz, lum) * saturate(dot(DA_W_IOP2.xyz, lum) * 10.0f);

	//	A caustic moves light, it does not add it: the mean of the focus over the bed is one, so
	//	switching it on neither darkens nor brightens the water as a whole.
	return lerp(1.0f, focus, exp(-b * d));
}

//	The wind at a point of the surface, the way water.ps sees it: the gust tongue, the local
//	heading, the fetch shelter one tap upwind, the scum and the profile's damping. For the
//	pass that does not have the surface's own values in hand. Returns the slope budget.
float da_wc_wind(float2 wxz, out float ms, out float2 wdir)
{
	const float3 gustf = da_wind_field_eval(wxz);
	ms = da_wind_state.z;
	wdir = da_wind_local_dir(da_wind_state.xy, gustf.z);
	float shelter = 1.0f;
	{
		const float2 Q = wxz - wdir * WATER_FETCH_LEN;
		const float qs = da_wf_shore(Q);
		const float qsig = (da_wf_depth(Q) > 0.001f) ? qs : -qs;
		shelter = saturate((WATER_FETCH_LEN + qsig) / WATER_FETCH_LEN);
	}
	const float scum = saturate(DA_W_IOP2.w);
	return da_w_mss(ms * shelter, scum, gustf.x) * da_w_damp(DA_W_IOP.w);
}

#endif	// DA_WATER_CAUSTIC_H
