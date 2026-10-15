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
//   da_ice_mark da_ice_marks(xz, footprint)            what the holes and the bullets did to it
#ifndef DA_SNOW_ICE_H
#define DA_SNOW_ICE_H

// x ice on standing water, y ice on running water, z snow cover on the ice, w snow depth on it (m): the
// season's ice keys at this day (da_snow_frame().ice), all zero outside the ice months.
uniform float4 da_snow_ice;
// The climate's ice (docs/dead-air/WEATHER.md): x the number of holes in da_ice_holes, y standing water's ice (m),
// z running water's (m), w how rotten the standing water's spring ice is (0..1).
uniform float4 da_ice_state;
// Holes broken into the ice near the camera (xrEngine da_ice.h): x, z, radius, kind * 1000 + seed (kind 0 a hole of
// open water, 1 a bullet's star of cracks in ice that held).
uniform float4 da_ice_holes[16];

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

// The edge of a broken hole at angle ang: a polygon of n straight fractures between corners at radius r x 0.72..1.22
// (a sheet breaks along straight lines between the points where its cracks met, not in a round wobble).
float da_ice_rim(float ang, float r, float seed, float n)
{
	const float step = 6.2831853 / n;
	const float a0 = frac(seed * 0.618) * step;
	const float t = (ang - a0) / step;
	const float i = floor(t);
	const int ii = (int(i) % int(n) + int(n)) % int(n);
	const int jj = (ii + 1) % int(n);
	const float ai = a0 + i * step, aj = ai + step;
	const float ri = r * (0.72 + 0.5 * da_ice_hash(int2(ii, int(seed)))), rj = r * (0.72 + 0.5 * da_ice_hash(int2(jj, int(seed))));
	const float2 pi = ri * float2(cos(ai), sin(ai)), pj = rj * float2(cos(aj), sin(aj));
	const float2 u = float2(cos(ang), sin(ang));
	const float2 e = pj - pi;
	return (pi.x * pj.y - pi.y * pj.x) / max(u.x * e.y - u.y * e.x, 1e-4);
}

// What the holes near the camera did to the ice at xz (xrEngine da_ice.h, da_ice_holes: x, z, radius, kind * 1000 +
// seed; kind 0 a hole broken through, 1 a bullet the ice stopped). A hole is a polygon of fractures, open water in it,
// the water it pushed up soaking the ice and the snow round it, and a few cracks running out of it: jagged lines of
// water, darker than the ice, never white. (Flat plates floating in it read as cut-outs: a plate tilts and shows its
// thickness, which a surface cannot.) A bullet the ice
// stopped crushed a spot of it white, a centimetre or two, cracked it with a few short fine fractures that show as
// faint pale lines in clear ice, and blew the snow out round it. Fine lines fade out below the pixel footprint (m).
// The physics takes a hole's mean circle (da_ice_in_hole).
struct da_ice_mark
{
	float open;  // open water in a hole
	float crack; // a crack of water in the ice
	float wet;   // the water pushed up round a hole
	float white; // crushed ice: a bullet's spot, a fresh fracture face
	float pit;   // the snow blown out round a bullet's spot
	float fine;  // a bullet's fine fractures in clear ice
};

da_ice_mark da_ice_marks(float2 xz, float footprint)
{
	da_ice_mark m = (da_ice_mark)0;
	const float fp = max(footprint, 1e-3);
	const int n = (int)da_ice_state.x;
	[loop] for (int i = 0; i < n; ++i)
	{
		const float4 h = da_ice_holes[i];
		const float2 d = xz - h.xy;
		const float r = h.z;
		const float d2 = dot(d, d);
		[branch] if (d2 > r * r * 12.0 + 0.04)
			continue;
		const float kind = floor(h.w * 0.001);
		const float seed = h.w - kind * 1000.0;
		const float dist = sqrt(d2);
		const float ang = atan2(d.y, d.x);
		[branch] if (kind < 0.5)
		{
			// The hole: its fractures, open water in it; small chips along its edge (a broken edge is not a clean cut).
			const float rr = da_ice_rim(ang, r, seed, 9.0) * (1.0 + 0.05 * (da_ice_noise(xz * 90.0 + seed) - 0.5));
			const float fw = max(0.5 * fp, 0.004);
			const float inside = 1.0 - smoothstep(rr - fw, rr + fw, dist);
			m.open = max(m.open, inside);
			// the sheet's fresh fracture face along the edge
			const float face = (1.0 - smoothstep(0.0, 0.012 + fp, dist - rr)) * step(rr, dist);
			m.white = max(m.white, face * 0.3 * saturate((0.08 - fp) / 0.05));
			// the water it pushed up
			const float halo = 1.0 - smoothstep(rr, rr * 1.35 + 0.1, dist);
			m.wet = max(m.wet, (1.0 - inside) * halo * (0.55 + 0.45 * da_ice_noise(xz * 14.0 + seed)));
			// cracks out of it, four to six, each its own length, tapering to the tip: straight runs with a kink every
			// 10-25 cm (a crack runs straight until the sheet turns it), never a wave
			const float K = 4.0 + floor(3.0 * da_ice_hash(int2(int(seed), 3)));
			const float stepk = 6.2831853 / K;
			const float t0 = (ang - seed * 0.37) / stepk;
			const float kk = floor(t0 + 0.5);
			const float seg_len = 0.1 + 0.15 * da_ice_hash(int2(int(kk) + 41, int(seed)));
			const float sg = dist / seg_len;
			const float si = floor(sg);
			const float k0 = da_ice_hash(int2(int(si) + int(kk) * 37, int(seed) + 11)) - 0.5;
			const float k1 = da_ice_hash(int2(int(si) + 1 + int(kk) * 37, int(seed) + 11)) - 0.5;
			const float side = lerp(k0, k1, sg - si) * 0.09 * saturate((dist - rr) / 0.15);
			const float off = abs((t0 - kk) * stepk * dist - side);
			const float len = rr * (0.5 + 1.3 * da_ice_hash(int2(int(kk) + 17, int(seed))));
			const float along = saturate((dist - rr) / max(len, 1e-3));
			const float w = 0.0045 * (1.0 - 0.7 * along);
			const float lit = (1.0 - smoothstep(w, w + fp, off)) * step(rr, dist) * (1.0 - smoothstep(0.8, 1.0, along));
			m.crack = max(m.crack, lit * 0.7 * saturate((0.12 - fp) / 0.08));
		}
		else
		{
			// The bullet's crushed spot, the snow blown out round it, its fine fractures.
			const float rw = r * (0.22 + 0.12 * da_ice_hash(int2(int(seed), 5)));
			m.white = max(m.white, (1.0 - smoothstep(rw * 0.55, rw, dist)) * 0.85);
			m.pit = max(m.pit, 1.0 - smoothstep(r * 0.4, r * 0.75, dist));
			const float K = 5.0 + floor(3.0 * da_ice_hash(int2(int(seed), 9)));
			const float stepk = 6.2831853 / K;
			const float t = (ang - seed * 0.53) / stepk;
			const float kk = floor(t + 0.5);
			const float off = abs(t - kk) * stepk * dist;
			const float len = r * (0.7 + 1.5 * da_ice_hash(int2(int(kk) + 29, int(seed))));
			const float along = saturate(dist / max(len, 1e-3));
			const float w = 0.0012 + 0.001 * (1.0 - along);
			const float lit = (1.0 - smoothstep(w, w + fp, off)) * (1.0 - smoothstep(0.7, 1.0, along)) * step(rw * 0.8, dist);
			m.fine = max(m.fine, lit * 0.3 * saturate((0.06 - fp) / 0.04));
		}
	}
	return m;
}

// Ice cover 0..1. It grows out from the banks: the front stands key * reach metres from the nearest bank,
// ragged by the noise; both close over completely at key 1, running water's key lags (a lane over the current
// while its ice is young). Water the baked field does not carry takes the standing key, patchy while partial.
float da_ice_cover_whole(float2 xz, bool on_field, bool flowing, float shore);
float da_ice_cover(float2 xz, bool on_field, bool flowing, float shore)
{
	// A hole broken into it is open water.
	return da_ice_cover_whole(xz, on_field, flowing, shore) * (1.0 - da_ice_marks(xz, 0.02).open);
}

// The cover before the holes.
float da_ice_cover_whole(float2 xz, bool on_field, bool flowing, float shore)
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
	const float reach = key * (flowing ? 40.0 : 38.0);
	return saturate((reach - shore + 3.0 * (n01 * 2.0 - 1.0)) * 0.5);
}

// Snow depth on the ice (m): the season's depth where its cover key reaches, by a noise of 3 m lobes with
// a 1 m one on top, so a partial cover is ragged patches whose depth tapers to nothing at their margin;
// none on young ice. The full key leaves only a few wind-swept spots.
float da_ice_snow(float2 xz, float ice)
{
	[branch] if (da_snow_ice.w <= 0.0 || da_snow_ice.z <= 0.0 || ice <= 0.5)
		return 0.0;
	const float n = 0.75 * da_ice_noise(xz * 6.0) + 0.25 * da_ice_noise(xz * 20.0 + 31.0);
	const float c = saturate((da_snow_ice.z * 1.2 - n) * 4.0) * saturate((ice - 0.5) * 4.0);
	const float drift = 0.8 + 0.4 * da_ice_noise(xz * 9.0 + 57.0);
	// blown out round a bullet's spot, soaked away round a hole, a broken plate keeping some of it
	const da_ice_mark mk = da_ice_marks(xz, 0.05);
	const float keep = (1.0 - mk.pit) * (1.0 - 0.85 * mk.wet);
	return da_snow_ice.w * drift * c * c * (3.0 - 2.0 * c) * keep;
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
	// What the holes and the bullets did: water in the cracks (darker than the ice), the faint pale fractures of a
	// bullet in clear ice, the white of crushed ice, the soaked ring round a hole, its wet grey plates.
	const da_ice_mark mk = da_ice_marks(xz, fp);
	a = lerp(a, a * 0.3, mk.crack);
	a = lerp(a, float3(0.34, 0.37, 0.38), mk.fine);
	a = lerp(a, float3(0.6, 0.64, 0.66), mk.white);
	a = lerp(a, a * 0.6, mk.wet);
	// Rotten spring ice: candled, grey-white and porous, wet on top.
	a = lerp(a, float3(0.2, 0.22, 0.22), 0.7 * saturate(da_ice_state.w));

	// Snow dust in broken streaks along the winter wind, over a part of the ice: the rest stays dark.
	const float streak = da_ice_noise(float2(xz.x * 1.5, xz.y * 16.0) + 41.0) * lerp(0.6, 1.0, da_ice_noise(xz * 12.0 + 29.0));
	const float dust = smoothstep(0.52, 0.85, streak) * saturate(0.25 + 1.5 * da_snow_ice.z) * 0.45;
	a = lerp(a, float3(0.6, 0.62, 0.65), dust);
	float gloss = lerp(DA_ICE_GLOSS_MAX, 0.03, max(dust, 0.6 * cloudy));
	// water on it is a mirror; crushed ice scatters
	gloss = lerp(gloss, DA_ICE_GLOSS_MAX, mk.wet);
	gloss = lerp(gloss, 0.02, mk.white);
	return float4(a, gloss);
}

#endif // DA_SNOW_ICE_H
