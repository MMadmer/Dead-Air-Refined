// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The season's ice on standing and running water (docs/dead-air/WINTER.md, SNOW.md), shared by water.ps
// (the surface beyond the snow grid) and the snow passes (the snow grid crosses frozen water near the
// camera, da_snow_grid.vs / da_snow_layer.ps), so both draw the same ice front, the same snow on it and
// the same bare ice. The cover rule is CEnvironment::ice_at's: the picture and gameplay agree.
//   float  da_ice_cover(xz, on_field, flowing, shore)  ice 0..1 here
//   float  da_ice_snow(xz, ice)                        snow depth on that ice (m)
//   float4 da_ice_albedo(xz, footprint)                bare ice: linear albedo, gloss
#ifndef DA_SNOW_ICE_H
#define DA_SNOW_ICE_H

// x ice on standing water, y ice on running water, z snow cover on the ice, w snow depth on it (m): the
// season's ice keys at this day (da_snow_frame().ice), all zero outside the ice months.
uniform float4 da_snow_ice;

// The integer hash and fade of CEnvironment::ice_at (Environment.cpp).
float da_ice_hash(int2 c)
{
	uint v = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u;
	v ^= v >> 15;
	v *= 0x2c1b3c6du;
	v ^= v >> 12;
	v *= 0x297a2d39u;
	v ^= v >> 15;
	return float(v >> 8) * (1.0 / 16777216.0);
}

// Value noise with lobes of 20 m at scale 1.
float da_ice_noise(float2 xz)
{
	const float2 p = xz * 0.05;
	const float2 f = floor(p);
	const int2 i = int2(f);
	float2 t = p - f;
	t = t * t * (3.0 - 2.0 * t);
	const float a = lerp(da_ice_hash(i), da_ice_hash(i + int2(1, 0)), t.x);
	const float b = lerp(da_ice_hash(i + int2(0, 1)), da_ice_hash(i + int2(1, 1)), t.x);
	return lerp(a, b, t.y);
}

// Ice cover 0..1. It grows out from the banks: the front stands key * reach metres from the nearest bank,
// ragged by the noise; standing water closes over completely at key 1, running water keeps a lane wider
// than twice its reach. Water the baked field does not carry takes the standing key, patchy while partial.
float da_ice_cover(float2 xz, bool on_field, bool flowing, float shore)
{
	const float4 k = da_snow_ice;
	[branch] if (max(k.x, k.y) < 0.02)
		return 0.0;
	const float n01 = da_ice_noise(xz);
	[branch] if (!on_field)
		return (k.x < 0.02) ? 0.0 : saturate((k.x * 1.2 - n01) * 5.0);
	const float key = flowing ? k.y : k.x;
	[branch] if (key < 0.02)
		return 0.0;
	const float reach = key * (flowing ? 32.0 : 38.0);
	return saturate((reach - shore + 3.0 * (n01 * 2.0 - 1.0)) * 0.5);
}

// Snow depth on the ice (m): the season's depth where its cover key reaches, by a noise of 3 m lobes with
// a 1 m one on top, so a partial cover is ragged patches whose depth tapers to nothing at their margin;
// none on young ice. The full key leaves only a few wind-swept spots.
float da_ice_snow(float2 xz, float ice)
{
	[branch] if (da_snow_ice.w <= 0.0 || da_snow_ice.z <= 0.0 || ice <= 0.25)
		return 0.0;
	const float n = 0.75 * da_ice_noise(xz * 6.0) + 0.25 * da_ice_noise(xz * 20.0 + 31.0);
	const float c = saturate((da_snow_ice.z * 1.2 - n) * 4.0) * saturate(ice * 2.0 - 0.5);
	const float drift = 0.8 + 0.4 * da_ice_noise(xz * 9.0 + 57.0);
	return da_snow_ice.w * drift * c * c * (3.0 - 2.0 * c);
}

// Distance (in cells) from p to the nearest border of a jittered-grid Voronoi network: half the gap
// between the nearest and the second nearest site, along the line joining them.
float da_ice_crack(float2 p, int salt)
{
	const float2 c = floor(p);
	const float2 f = p - c;
	float2 n0 = float2(0.0, 0.0), n1 = float2(0.0, 0.0);
	float d0 = 8.0, d1 = 8.0;
	[unroll] for (int j = -1; j <= 1; ++j)
	{
		[unroll] for (int i = -1; i <= 1; ++i)
		{
			const int2 cell = int2(c) + int2(i, j);
			const float2 site = float2(i, j) + 0.1 + 0.8 * float2(da_ice_hash(cell + salt), da_ice_hash(cell.yx - salt));
			const float d = dot(site - f, site - f);
			[flatten] if (d < d0)
			{
				d1 = d0;
				n1 = n0;
				d0 = d;
				n0 = site;
			}
			else if (d < d1)
			{
				d1 = d;
				n1 = site;
			}
		}
	}
	const float2 m = 0.5 * (n0 + n1);
	const float2 dir = n1 - n0;
	return abs(dot(f - m, dir)) * rsqrt(max(dot(dir, dir), 1e-6));
}

// Gloss of clear bare ice (the G-buffer's specular intensity): the sun's specular is five times the gloss
// (combine_1.ps), so more burns the ice white under the sun. water.ps scales its mirror by the same value.
#define DA_ICE_GLOSS_MAX 0.06

// Bare ice: black ice over the water column (dark grey-green, Warren: ice absorbs red first) with a fine
// mottle, greyer where air and snow froze into it (blotches of 5-10 m and 1-2 m), thin pale cracks - a
// coarse (3 m) net whose borders show along long runs and a fine (0.9 m) one in patches, so the
// ice never reads as paving nor as open water - and wind-blown snow dust in streaks along the winter wind
// (west to east). The cracks fade out once a line is thinner than the pixel footprint (m), so a far pond
// does not shimmer.
// Returns linear albedo (rgb) and the G-buffer gloss (w).
float4 da_ice_albedo(float2 xz, float footprint)
{
	const float big = da_ice_noise(xz * 3.0 + 11.0);
	const float mid = da_ice_noise(xz * 14.0 + 23.0);
	float3 a = lerp(float3(0.026, 0.038, 0.035), float3(0.065, 0.08, 0.075), saturate(big * 1.4 - 0.2));
	a *= lerp(0.8, 1.2, da_ice_noise(xz * 70.0 + 3.0));
	const float cloudy = smoothstep(0.6, 0.88, 0.6 * mid + 0.4 * big);
	a = lerp(a, float3(0.15, 0.17, 0.17), 0.4 * cloudy);

	const float fp = max(footprint, 1e-3);
	float crack = 0.0;
	[branch] if (fp < 0.15)
	{
		const float w0 = 0.005, w1 = 0.0025;
		// The cells are warped (noise of 2 m and 0.7 m lobes), so a crack meanders instead of running as
		// the straight border of a tile.
		const float2 warp = float2(da_ice_noise(xz * 10.0 + 5.0), da_ice_noise(xz * 10.0 + 97.0)) - 0.5
			+ 0.35 * (float2(da_ice_noise(xz * 28.0 + 13.0), da_ice_noise(xz * 28.0 + 71.0)) - 0.5);
		const float2 q = xz + warp * 1.4;
		const float e0 = da_ice_crack(q * (1.0 / 3.0), 7) * 3.0;
		const float e1 = da_ice_crack(q * (1.0 / 0.9), 19) * 0.9;
		const float l0 = 1.0 - smoothstep(w0, w0 + fp, e0);
		const float l1 = 1.0 - smoothstep(w1, w1 + fp, e1);
		// which stretches of the borders are cracked: a few long runs (noise of 2.5 m) and, for the fine
		// net, rare 6 m patches; most borders stay sound, so no net of tiles shows
		const float m0 = smoothstep(0.64, 0.82, da_ice_noise(xz * 8.0 + 61.0));
		const float m1 = smoothstep(0.76, 0.9, da_ice_noise(xz * 3.3 + 83.0));
		crack = max(l0 * m0 * 0.5, l1 * m1 * 0.3) * saturate((0.15 - fp) / 0.1);
	}
	a = lerp(a, float3(0.24, 0.27, 0.27), crack);

	// Snow dust in broken streaks along the winter wind, over a part of the ice: the rest stays dark.
	const float streak = da_ice_noise(float2(xz.x * 1.5, xz.y * 16.0) + 41.0) * lerp(0.6, 1.0, da_ice_noise(xz * 12.0 + 29.0));
	const float dust = smoothstep(0.52, 0.85, streak) * saturate(0.25 + 1.5 * da_snow_ice.z) * 0.45;
	a = lerp(a, float3(0.6, 0.62, 0.65), dust);
	const float gloss = lerp(DA_ICE_GLOSS_MAX, 0.03, max(dust, 0.6 * cloudy));
	return float4(a, gloss);
}

#endif // DA_SNOW_ICE_H
