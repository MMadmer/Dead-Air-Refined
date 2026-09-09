#ifndef DA_WETNESS_H
#define DA_WETNESS_H

// ---- The shared wet-surface model ---------------------------------------------------------
//
// Included by BOTH halves of the puddle system (da_puddles.h, which the G-buffer terrain pass
// and the reflection overlay each run from scratch) and by the fullscreen wet-surface patch
// (rain_patch_normal.ps). It lives in one file on purpose: the two puddle passes are required
// to agree bit for bit, and the last time a piece of this model was transcribed twice the two
// halves of one puddle stopped agreeing about where the water was.
//
// Nothing here samples a texture. The deferred geometry blender is built-in C++ and not
// scriptable, so a puddle cannot acquire a sampler without a C++ change - every pattern below
// is arithmetic for that reason (the same reason recorded at the top of da_puddles.h).

// Wetness, split by WHERE the water is. The three live on completely different clocks and
// one accumulator for all of them is why the world went from bone dry to soaked and back on a
// single ninety-second ramp. Integrated once per frame in r2.cpp (cl_da_wetness).
//   x - surface film, the sheet lying on top      (rises in ~5 s,  gone in ~60 s)
//   y - porous saturation, water taken up INSIDE  (rises in ~90 s, gone in ~6 min)
//   z - standing water, filled at the rain rate   (drains over ~10 min)
//   w - rain falling right now, 0..1
uniform float4 da_wetness;

// x - fill-map placement on/off (r__puddle_fill), y - rain ring layers (r__rain_quality),
// z - ripple amplitude (r__puddles_ripple), w - the ring clock IN RING CYCLES, wrapped on the
// CPU at 2048 (cl_da_wet_params in r2.cpp).
//
// Why w is not just timers.x: fTimeGlobal grows without bound and frac() of it loses a bit of
// resolution on every doubling, so after a few hours the rings would step instead of running.
// It is the same precision trap the periodic hash in da_puddles.h exists to dodge and the
// reason the ripple travel phases are accumulated on the CPU rather than multiplied out of
// absolute time. Everything below therefore ticks off this clock and NOT off timers, and every
// rate applied to it is a whole number of cycles per wrap, so the wrap itself is seamless.
uniform float4 da_wet_params;

// A raindrop ring is a gravity-capillary wave packet: the visible front runs at about
// 0.35 m/s and the packet is dead by 0.25 m, so one ring's whole life is 0.25/0.35 s - which
// is the cycle the clock above counts in. Anything that expands slower, lives longer or
// reaches metres is wind chop, not rain.
#define DA_RING_RADIUS 0.25f

float2 da_wet_hash2(float2 p)
{
	p = frac(p * float2(127.1f, 311.7f) + float2(0.13f, 0.71f));
	p += dot(p, p + 34.23f);
	return frac(float2(p.x * p.y, p.x + p.y));
}

// ---- Porosity ------------------------------------------------------------------------------
//
// Taken from the material id the G-buffer already carries (gbuf_unpack_mtl, 5 bits), which the
// puddle system read exactly nowhere before. That id is the lighting BRDF slice the texture's
// .thm assigns - 0 Oren-Nayar, 1 Blinn, 2 Phong, 3 metal, the four slices built in
// r4_rendertarget_build_textures.cpp - and it is as close to a porosity classification as
// shipped data gets: rough diffuse IS soil and cloth, and metal is metal. The value arrives
// continuous because material_weight blends two slices, so there is nothing to round here.
//
// One class drives both the darkening and, for free, the drying rate: a non-porous surface has
// no porous term to hold, so it keeps only the film and is dry a minute after the rain stops.
float da_porosity(float mtl)
{
	const float m = clamp(mtl * 4.0f - 0.5f, 0.0f, 3.0f);
	const float a = lerp(1.00f, 0.75f, saturate(m));        // soil, cloth -> stone, concrete
	const float b = lerp(0.35f, 0.05f, saturate(m - 2.0f)); // painted, plastic -> metal
	return lerp(a, b, saturate(m - 1.0f));
}

// How wet this pixel reads, given what its material can actually hold. The film sits on
// anything; the porous term only exists where there are pores to fill.
float da_wet_level(float porosity)
{
	return saturate(da_wetness.x * (0.35f + 0.65f * porosity) + da_wetness.y * porosity);
}

// ---- Darkening -----------------------------------------------------------------------------
//
// Saunderson/Angstrom. Light that diffused back out of the substrate meets the water-air
// interface from inside, and past the critical angle (48.75 deg at n = 1.33) it is sent down
// for another round of absorption. Hemispherically integrated Fresnel gives r_e = 0.0659 and
// r_i = 0.4719, so R_wet = (1-r_e)(1-r_i) R / (1 - r_i R) = 0.4972 R / (1 - 0.4719 R).
//
// The point of shipping a curve rather than a scalar: it halves a dark albedo and takes only a
// fifth off a bright one. That is why wet asphalt goes black while wet chalk barely moves, and
// why one global multiplier darkened painted metal exactly like soil.
float3 da_saunderson(float3 albedo)
{
	return (0.4972f * albedo) / max(1.0f - 0.4719f * albedo, 1e-3f);
}

// The single multiplier a pipeline that can only carry one number needs. Evaluated at this
// pixel's own luminance, so the per-albedo trend survives even though the chroma shift does
// not: the defect being fixed is "everything darkens by the same factor", not the chroma.
float da_wet_darken_k(float3 albedo, float wet)
{
	const float rd = max(dot(albedo, LUMINANCE_VECTOR), 1e-3f);
	const float k = 0.4972f / max(1.0f - 0.4719f * rd, 1e-3f);
	return lerp(1.0f, saturate(k), saturate(wet));
}

// ---- Rain rings ----------------------------------------------------------------------------
//
// Lagarde's ComputeRipple without the atlas. The atlas stored an inverted normalised distance,
// a perturbation direction and a per-circle time offset; all three are cheaper to compute than
// to fetch, and a fetch would need a sampler binding this shader cannot have.
//
// One ring per lattice cell per cycle. The centre is jittered only as far as the cell can take
// it without the ring crossing into a neighbour - a ring cut off at a cell seam draws a
// straight line across the water, which is exactly what the lattice is there to avoid.
float2 da_rain_ring_layer(float2 wp, float t, float cell, float seed)
{
	const float2 g = floor(wp / cell);
	const float2 h = da_wet_hash2(g + seed);

	// Per-cell time offset: without it every cell in the world pulses on the same beat and the
	// rain reads as a strobe rather than as drops.
	const float ph = frac(t + h.x);
	const float2 c = (g + 0.5f) * cell + (h - 0.5f) * max(cell - 2.0f * DA_RING_RADIUS, 0.0f);
	const float2 d = wp - c;
	const float r = length(d);
	const float inv = saturate(1.0f - r / DA_RING_RADIUS);
	[branch] if (inv <= 0.0f)
		return float2(0.0f, 0.0f);

	// The crest reaches a radius when the cycle has run far enough to get there, makes one and
	// a half oscillations and stops. The clamp is what ends the ring - at exactly zero, which
	// an exponential tail never does, and for one instruction.
	const float tf = ph - 1.0f + inv;
	const float a = inv * sin(clamp(tf * 9.0f, 0.0f, 3.0f) * 3.14159265f);
	return (d / max(r, 1e-4f)) * a;
}

// The XZ slope the rain adds to a flat water surface; the caller's normal is (x, 1, y), the
// same convention the rest of the puddle code uses.
//
// Rain intensity buys LAYERS, not bigger rings - a heavier shower puts more drops on the same
// square metre. That is Remember Me's rule (one layer per 0.25 of intensity) and it is also
// what the physics says, since ring size is set by surface tension and not by the weather.
float2 da_rain_rings(float2 wp, float rain, float layers)
{
	[branch] if (rain <= 0.002f || layers < 0.5f)
		return float2(0.0f, 0.0f);

	const float t = da_wet_params.w;
	const float n = min(layers, ceil(rain * 4.0f));

	float2 ripple = da_rain_ring_layer(wp, t, 0.70f, 0.0f);
	// Every layer runs on the SAME clock and differs only by cell size, lattice offset and
	// seed. Per-layer rates were the obvious way to decorrelate them, but the per-cell time
	// offset inside a layer already does that completely, and a rate that is not a whole
	// number of cycles per wrap would jump the phase every time the clock came round.
	[branch] if (n > 1.5f) ripple += da_rain_ring_layer(wp + 3.70f, t, 0.85f, 17.0f);
	[branch] if (n > 2.5f) ripple += da_rain_ring_layer(wp - 5.10f, t, 0.58f, 41.0f);
	[branch] if (n > 3.5f) ripple += da_rain_ring_layer(wp + 8.30f, t, 1.00f, 73.0f);
	return ripple * (0.05f * max(da_wet_params.z, 0.0f));
}

#endif // DA_WETNESS_H
