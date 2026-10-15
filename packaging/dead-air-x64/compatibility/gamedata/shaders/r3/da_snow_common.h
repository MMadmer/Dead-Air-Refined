// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Shared helpers of the snow passes (da_snow_*.cs/.vs/.ps, docs/dead-air/SNOW.md): the level fields at
// 1 m, the snow base (the grey-opened topmost surface the snow lies on, the floor under a cover - a roof,
// a deck, a vehicle -, the water surface over water), the shelter of the covers,
// the season depth per class (bilinear, and the smooth cubic B-spline the depth textures carry), the
// ragged margin of thin snow, the trail resolve and the baked water field. Integer and float textures
// are read with Load, so every stage can use them.
#ifndef DA_SNOW_COMMON_H
#define DA_SNOW_COMMON_H

#include "da_snow_sample.h"
#include "da_snow_ice.h"

#define DA_SN_NONE (-10000.0)
#define DA_SN_UNTOUCHED 0xFFFFFFFFu

// Level fields, row z, column x, texel (x, z) centred at origin + (x + 0.5, z + 0.5) * texel.
Texture2D<float4> s_da_sn_weights; // RGBA8: open, under trees, road, drift (sum 1)
Texture2D<float4> s_da_sn_factors; // RGBA8: A / 2, M / 2, n * 0.5 + 0.5, N
Texture2D<float> s_da_sn_base;     // R16_UNORM: snow base of the level (y = da_sn_lvl.x + v * da_sn_lvl.y), 0 = no collision
Texture2D<float> s_da_sn_shelter;  // R8_UNORM: 1 where a cover keeps the sky's snow off the level texel
// Near window of da_ground.cpp, toroidal (world texel modulo da_sn_win2.x): x = the snow base (the grey-opened
// topmost non-passable surface, the floor under a cover; world y, DA_SN_NONE where nothing is; the water
// surface over water), y = cliff taper 0..1 on land, -1 - taper under a water surface, z = the shelter of a
// cover over the texel, w = the opened topmost surface itself.
Texture2D<float4> s_da_ground_base;
// The same window's waterline (da_ground_resolve.cs): x the opened solid surface, the bed under water too
// (DA_SN_NONE where nothing is), y the highest water surface within 8 texels (DA_SN_NONE: none), z how far
// inside that reach the texel lies (1 up to half of it, 0 at its end). Bound by the grid and the layer only.
Texture2D<float4> s_da_ground_shore;
// The baked water field ($user$water_field, da_water_field.h: surface y, coverage, bed y, metres to the
// bank) and running water ($user$water_class); bound by the passes that meet frozen water.
Texture2D<float4> s_water_field;
Texture2D<float> s_water_class;

uniform float4 da_sn_lvl;  // x lowest base y, y base range (m), zw unused
uniform float4 da_sn_win;  // xy window centre (m), z valid half-size (m), w blend band to the level base (m)
uniform float4 da_sn_win2; // x texels per side, y texels per metre, z QA view (r__dbg_snow), w unused
uniform float4 da_water_map;  // xy the level's min xz, zw 1 / extent (zero without a field)
uniform float4 da_water_map2; // x metres per texel, yz the level's y range, w 1 = the field is valid

// The frame's season scalars per class (x open, y under trees, z road, w drift), standard binders.
uniform float4 da_snow_S;
uniform float4 da_snow_Mc;
uniform float4 da_snow_S_prev;
uniform float4 da_snow_Mc_prev;

uint da_sn_u16(float v)
{
	return uint(saturate(v / da_snow_misc.x) * 65535.0 + 0.5);
}

float da_sn_f16(uint v)
{
	return float(v) * (da_snow_misc.x / 65535.0);
}

// World texel modulo the map size; every world texel of a level is above -256 n.
int2 da_sn_wrap(int2 t, int n)
{
	return int2(uint2(t + n * 256) % uint(n));
}

// Integer hash of a cell to [0, 1).
float da_sn_hash2(int2 c, uint salt)
{
	uint v = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ salt * 0x9e3779b9u;
	v ^= v >> 15;
	v *= 0x2c1b3c6du;
	v ^= v >> 12;
	v *= 0x297a2d39u;
	v ^= v >> 15;
	return float(v >> 8) * (1.0 / 16777216.0);
}

// Value noise over unit cells with a smooth fade, [0, 1).
float da_sn_vnoise(float2 p, uint salt)
{
	const float2 f = floor(p);
	const int2 i = int2(f);
	float2 t = p - f;
	t = t * t * (3.0 - 2.0 * t);
	const float a = lerp(da_sn_hash2(i, salt), da_sn_hash2(i + int2(1, 0), salt), t.x);
	const float b = lerp(da_sn_hash2(i + int2(0, 1), salt), da_sn_hash2(i + int2(1, 1), salt), t.x);
	return lerp(a, b, t.y);
}

// Depth of each class at one texel: clamp(S_c * A_c - Mc_c * M_c, 0, cap) (da_snow_field.h).
float4 da_sn_class_depth(float4 f, float4 S, float4 Mc)
{
	const float A = f.x * 2.0;
	const float M = f.y * 2.0;
	const float n = f.z * 2.0 - 1.0;
	const float N = f.w;
	const float an = 1.0 + 0.35 * n;
	const float4 Ac = float4(A * an, A * an, 1.0 + 0.15 * n, an * (0.5 + N));
	const float4 Mf = float4(M, M, 1.0, 1.0 - 0.3 * N);
	return clamp(S * Ac - Mc * Mf, 0.0, da_snow_misc.x);
}

// The share of the open snow an exposed roof, a deck or a vehicle keeps (the wind sweeps it).
#define DA_SN_ROOF_SHARE 0.85

// The snow on a cover's face of slope ny at xz now (x) and on the previous frame (y): the open class's
// formula for that slope (all of it to 32 degrees, none past 60) with the level texel's noise and melt.
float2 da_sn_roof_depth(float2 xz, float ny)
{
	const int2 t = int2(floor(xz * da_snow_level_xf.xy + da_snow_level_xf.zw));
	const float4 f = s_da_sn_factors.Load(int3(t, 0));
	const float n = f.z * 2.0 - 1.0;
	const float M = f.y * 2.0;
	const float A = saturate((ny - 0.5) / 0.35) * (1.0 + 0.35 * n);
	const float now = clamp(da_snow_S.x * A - da_snow_Mc.x * M, 0.0, da_snow_misc.x);
	const float prev = clamp(da_snow_S_prev.x * A - da_snow_Mc_prev.x * M, 0.0, da_snow_misc.x);
	return float2(now, prev) * DA_SN_ROOF_SHARE;
}

// Season depth (no trails) of a level texel now.
float da_sn_texel_depth(int2 t)
{
	return dot(s_da_sn_weights.Load(int3(t, 0)), da_sn_class_depth(s_da_sn_factors.Load(int3(t, 0)), da_snow_S, da_snow_Mc));
}

// How patchy the season's snow is, 0..1: the open ground's melt term against its accumulation. The calendar
// winter keeps it near 0 (a continuous cover, the melt a few tenths of the fall at most); the thaw and the
// first snows of November bring it to 1 (patches). Only the frame's season scalars: every pass agrees.
float da_sn_patchy()
{
	const float r = (da_snow_S.x > 1e-3) ? da_snow_Mc.x / da_snow_S.x : 1.0;
	return saturate((r - 0.35) * 2.5);
}

// Where the season depth is read for xz while the snow is patchy: displaced by up to 0.9 m (value noise of
// 2.6 m and 0.8 m lobes), so a patch's edge runs in lobes and bays instead of along the smooth contour of
// the 1 m field (a white decal). Under the continuous winter cover nothing moves.
float2 da_sn_warp(float2 xz, float patchy)
{
	[branch] if (patchy <= 0.0)
		return xz;
	const float2 a = float2(da_sn_vnoise(xz * (1.0 / 2.6), 0x55u), da_sn_vnoise(xz * (1.0 / 2.6), 0x56u)) - 0.5;
	const float2 b = float2(da_sn_vnoise(xz * (1.0 / 0.8), 0x57u), da_sn_vnoise(xz * (1.0 / 0.8), 0x58u)) - 0.5;
	return xz + patchy * (1.3 * a + 0.45 * b);
}

// Thin snow wears back irregularly: a melting or a first patch does not end on the contour of the 1 m field
// but on a ragged margin that tapers to nothing (value noise of 0.9 m and 0.3 m cells; `detail` 0 keeps only
// the coarse one, for the 1 m level texture), with holes and islands near it. In the calendar winter up to
// 3 cm wear off snow under 8 cm. Patches neither lose nor gain snow on the whole: up to 2 cm come and go by
// the noise under 14 cm, easing out toward 14 cm so the surface has no crease there, and the thinnest snow
// thins a little further, a quarter of the way to u^2 (2 - u) of the 14 cm (same value and slope at 14 cm),
// so its see-through margin is a band, not a line. Snow only moves where some lies: bare ground stays bare.
float da_sn_shape(float D, float2 xz, float detail, float patchy)
{
	const float top = lerp(0.08, 0.14, patchy);
	[branch] if (D <= 0.0 || D >= top)
		return D;
	const float coarse = da_sn_vnoise(xz * (1.0 / 0.9), 0x51u);
	const float fine = lerp(0.5, da_sn_vnoise(xz * (1.0 / 0.3), 0x52u), detail);
	const float n = 0.65 * coarse + 0.35 * fine;
	const float t = 1.0 - D / top;
	float wear = lerp(0.03 * t * n, 0.04 * t * t * (n - 0.45), patchy);
	// what the noise adds fades in over the first 2 cm, so a patch's edge stays where it was, without a step
	wear *= (wear < 0.0) ? saturate(D * 50.0) : 1.0;
	const float r = max(D - wear, 0.0);
	const float u = r / top;
	return lerp(r, top * u * u * (2.0 - u), 0.25 * patchy);
}

// Uniform cubic B-spline weights of the four texels round t in [0, 1).
float4 da_sn_bspline(float t)
{
	const float t2 = t * t;
	const float t3 = t2 * t;
	const float s = 1.0 - t;
	return float4(s * s * s, 3.0 * t3 - 6.0 * t2 + 4.0, -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0, t3) * (1.0 / 6.0);
}

// Level texel coordinate of xz (texel centres at whole numbers).
float2 da_sn_level_coord(float2 xz)
{
	return xz * da_snow_level_xf.xy + da_snow_level_xf.zw - 0.5;
}

// How much a cover (a roof, a deck, a vehicle) keeps the sky's snow off the ground at xz, 0..1: the near
// window's (its last texel fading), blended into the level's over the band at the window's edge.
float da_sn_shelter(float2 xz)
{
	const float2 d = abs(xz - da_sn_win.xy);
	const float lw = saturate((max(d.x, d.y) - (da_sn_win.z - da_sn_win.w)) / max(da_sn_win.w, 1e-3));
	float s = 0.0;
	[branch] if (lw > 0.0)
	{
		const float2 p = da_sn_level_coord(xz);
		const float2 i = floor(p);
		const float2 f = p - i;
		const int2 t = int2(i);
		s = lerp(lerp(s_da_sn_shelter.Load(int3(t, 0)), s_da_sn_shelter.Load(int3(t + int2(1, 0), 0)), f.x),
			lerp(s_da_sn_shelter.Load(int3(t + int2(0, 1), 0)), s_da_sn_shelter.Load(int3(t + int2(1, 1), 0)), f.x), f.y);
	}
	[branch] if (lw < 1.0)
	{
		const int n = int(da_sn_win2.x);
		const float2 p = xz * da_sn_win2.y - 0.5;
		const float2 i = floor(p);
		const float2 f = p - i;
		const int2 t = int2(i);
		const float w00 = s_da_ground_base.Load(int3(da_sn_wrap(t, n), 0)).z;
		const float w10 = s_da_ground_base.Load(int3(da_sn_wrap(t + int2(1, 0), n), 0)).z;
		const float w01 = s_da_ground_base.Load(int3(da_sn_wrap(t + int2(0, 1), n), 0)).z;
		const float w11 = s_da_ground_base.Load(int3(da_sn_wrap(t + int2(1, 1), n), 0)).z;
		s = lerp(lerp(lerp(w00, w10, f.x), lerp(w01, w11, f.x), f.y), s, lw);
	}
	return saturate(s);
}

// The season depth (no trails) at xz the fine depth texture carries: a cubic B-spline over the 4 x 4 level
// texels round the warped point (da_sn_warp), smooth to the second derivative, so the snow's slope - and its
// light - has no crease at the 1 m texel borders a bilinear read leaves; thin snow shaped by da_sn_shape.
// da_snow_composite.cs sums the same terms in the same order from group-shared texels.
float da_sn_depth_smooth(float2 xz)
{
	const float patchy = da_sn_patchy();
	const float2 p = da_sn_level_coord(da_sn_warp(xz, patchy));
	const float2 i = floor(p);
	const float4 wx = da_sn_bspline(p.x - i.x);
	const float4 wz = da_sn_bspline(p.y - i.y);
	const int2 t0 = int2(i) - 1;
	float D = 0.0;
	[unroll] for (int j = 0; j < 4; ++j)
	{
		float row = 0.0;
		[unroll] for (int k = 0; k < 4; ++k)
			row += wx[k] * da_sn_texel_depth(t0 + int2(k, j));
		D += wz[j] * row;
	}
	return da_sn_shape(max(D, 0.0), xz, 1.0, patchy) * (1.0 - da_sn_shelter(xz));
}

// The season depth (no trails) at xz, bilinear over the level texels: the 1 m level texture's read of a
// warped point while the snow is patchy (da_snow_level.cs).
float da_sn_depth_bilinear(float2 xz)
{
	const float2 p = da_sn_level_coord(xz);
	const float2 i = floor(p);
	const float2 f = p - i;
	const int2 t = int2(i);
	return lerp(lerp(da_sn_texel_depth(t), da_sn_texel_depth(t + int2(1, 0)), f.x),
		lerp(da_sn_texel_depth(t + int2(0, 1)), da_sn_texel_depth(t + int2(1, 1)), f.x), f.y);
}

struct da_sn_field
{
	float2 D;  // season depth now, previous frame (bilinear, unshaped)
	float4 w;  // class weights
	float4 f;  // factors
};

// Bilinear over the four texel centres around xz: the class weights and factors (the look of the snow),
// with the season depth alongside.
da_sn_field da_sn_season(float2 xz)
{
	const float2 p = da_sn_level_coord(xz);
	const float2 i = floor(p);
	const float2 fr = p - i;
	const int2 t = int2(i);
	const float4 bw = float4((1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y, fr.x * fr.y);
	da_sn_field r;
	r.D = float2(0.0, 0.0);
	r.w = float4(0.0, 0.0, 0.0, 0.0);
	r.f = float4(0.0, 0.0, 0.0, 0.0);
	[unroll] for (int k = 0; k < 4; ++k)
	{
		const int2 tk = t + int2(k & 1, k >> 1);
		const float4 w = s_da_sn_weights.Load(int3(tk, 0));
		const float4 f = s_da_sn_factors.Load(int3(tk, 0));
		const float2 D = float2(dot(w, da_sn_class_depth(f, da_snow_S, da_snow_Mc)), dot(w, da_sn_class_depth(f, da_snow_S_prev, da_snow_Mc_prev)));
		r.D += D * bw[k];
		r.w += w * bw[k];
		r.f += f * bw[k];
	}
	return r;
}

float da_sn_level_base(float2 xz)
{
	const float2 p = da_sn_level_coord(xz);
	const float2 i = floor(p);
	const float2 fr = p - i;
	const int2 t = int2(i);
	const float b00 = s_da_sn_base.Load(int3(t, 0));
	const float b10 = s_da_sn_base.Load(int3(t + int2(1, 0), 0));
	const float b01 = s_da_sn_base.Load(int3(t + int2(0, 1), 0));
	const float b11 = s_da_sn_base.Load(int3(t + int2(1, 1), 0));
	return da_sn_lvl.x + lerp(lerp(b00, b10, fr.x), lerp(b01, b11, fr.x), fr.y) * da_sn_lvl.y;
}

// Collision under all four texels around xz (the level base holds 0 over the backdrop beyond the playable
// area: its height there is the hole-filled ground, nothing the grid may stand on).
bool da_sn_level_solid(float2 xz)
{
	const int2 t = int2(floor(da_sn_level_coord(xz)));
	const float b = min(min(s_da_sn_base.Load(int3(t, 0)), s_da_sn_base.Load(int3(t + int2(1, 0), 0))),
		min(s_da_sn_base.Load(int3(t + int2(0, 1), 0)), s_da_sn_base.Load(int3(t + int2(1, 1), 0))));
	return b > 0.0;
}

// (Hg, taper, water share) from the near window, bilinear; a texel without a surface leaves the others
// and takes the taper to 0 (a cliff). The share is that of the four texels under a water surface; the
// taper runs on across the waterline (the shore is no cliff).
float3 da_sn_window_base(float2 xz)
{
	const int n = int(da_sn_win2.x);
	const float2 p = xz * da_sn_win2.y - 0.5;
	const float2 i = floor(p);
	const float2 fr = p - i;
	const int2 t = int2(i);
	const float2 v00 = s_da_ground_base.Load(int3(da_sn_wrap(t, n), 0)).xy;
	const float2 v10 = s_da_ground_base.Load(int3(da_sn_wrap(t + int2(1, 0), n), 0)).xy;
	const float2 v01 = s_da_ground_base.Load(int3(da_sn_wrap(t + int2(0, 1), n), 0)).xy;
	const float2 v11 = s_da_ground_base.Load(int3(da_sn_wrap(t + int2(1, 1), n), 0)).xy;
	const float4 h = float4(v00.x, v10.x, v01.x, v11.x);
	const float4 g = float4(v00.y, v10.y, v01.y, v11.y);
	const float4 bw = float4((1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y, fr.x * fr.y);
	const float4 ok = step(DA_SN_NONE * 0.5, h);
	const float4 wet = step(g, -0.5);
	const float4 tp = saturate(lerp(g, -g - 1.0, wet));
	float3 r = float3(max(max(h.x, h.y), max(h.z, h.w)), 0.0, dot(wet, bw));
	[branch] if (dot(ok, float4(1.0, 1.0, 1.0, 1.0)) > 3.5)
		r.xy = float2(dot(h, bw), dot(tp, bw));
	return r;
}

// The snow base at a world position: x height, y cliff taper, z water share. The near window, blended
// into the level base over the band at the window's edge (the two rasters differ by centimetres; the band
// hides the step); the level base knows no water. DA_SN_NONE where no collision lies under xz.
float3 da_sn_base(float2 xz)
{
	const float2 d = abs(xz - da_sn_win.xy);
	const float r = max(d.x, d.y);
	const float lw = saturate((r - (da_sn_win.z - da_sn_win.w)) / max(da_sn_win.w, 1e-3));
	float3 b = float3(0.0, 1.0, 0.0);
	bool level_ok = true;
	[branch] if (lw > 0.0)
	{
		level_ok = da_sn_level_solid(xz);
		b.x = level_ok ? da_sn_level_base(xz) : DA_SN_NONE;
	}
	[branch] if (lw < 1.0)
	{
		const float3 wb = da_sn_window_base(xz);
		if (wb.x > DA_SN_NONE * 0.5)
			b = level_ok ? lerp(wb, b, lw) : wb;
		else if (lw <= 0.0)
			b = wb;
	}
	return b;
}

// Slope over +-e from three heights: central, or the gentler side where the two disagree by more than
// 40 degrees (a roof edge, a wall top: the snow there keeps its own plane).
float da_sn_edge_slope(float h_minus, float h0, float h_plus, float e)
{
	const float sp = (h_plus - h0) / e;
	const float sm = (h0 - h_minus) / e;
	return (abs(sp - sm) > 0.85) ? ((abs(sp) < abs(sm)) ? sp : sm) : 0.5 * (sp + sm);
}

// The QA view of the snow passes (r__dbg_snow): 0 off, 1 the layer paints each path in its own colour,
// 2 the same with the grid's per-pixel cuts off.
// r__dbg_snow: 1 the passes' false colours, 2 without the wet tint, 3 the window's taper, shelter and covers; 10 + n
// a QA cut of the layer (da_sn_cut).
int da_sn_debug()
{
	const int v = int(da_sn_win2.z + 0.5);
	return v >= 10 ? 0 : v;
}

// QA (r__dbg_snow 11..16): the snow layer cut short after its stage n, for the stages' costs.
int da_sn_cut()
{
	const int v = int(da_sn_win2.z + 0.5);
	return v >= 10 ? v - 10 : 0;
}

// The baked water field at xz, read the way the water shaders sample it (da_water_field.h): x surface y
// and y coverage of the nearest texel, z bed y, w metres to the nearest bank, or on dry texels to the
// water (bilinear); zero off the field. `flowing` = running water there ($user$water_class), `inside` =
// the field is valid and covers xz.
float4 da_sn_water(float2 xz, out bool flowing, out bool inside)
{
	flowing = false;
	inside = false;
	[branch] if (da_water_map2.w < 0.5)
		return float4(0.0, 0.0, 0.0, 0.0);
	const float2 uv = (xz - da_water_map.xy) * da_water_map.zw;
	[branch] if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
		return float4(0.0, 0.0, 0.0, 0.0);
	inside = true;
	uint w, h;
	s_water_field.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const int2 t = min(int2(uv * float2(w, h)), last);
	float4 r = s_water_field.Load(int3(t, 0));
	// (classes: standing 0, indoors 0.5, running 1)
	flowing = r.y > 0.5 && s_water_class.Load(int3(t, 0)) > 0.75;
	const float2 q = uv * float2(w, h) - 0.5;
	const float2 qi = floor(q);
	const float2 qf = q - qi;
	const int2 b0 = clamp(int2(qi), int2(0, 0), last);
	const int2 b1 = min(b0 + 1, last);
	const float s00 = s_water_field.Load(int3(b0, 0)).w;
	const float s10 = s_water_field.Load(int3(b1.x, b0.y, 0)).w;
	const float s01 = s_water_field.Load(int3(b0.x, b1.y, 0)).w;
	const float s11 = s_water_field.Load(int3(b1, 0)).w;
	r.w = lerp(lerp(s00, s10, qf.x), lerp(s01, s11, qf.x), qf.y);
	return r;
}

// Ground below a pond's surface that the ground window does not see under water: the window's water comes
// from the collision's liquid faces, which often stop short of the bank or are missing, while the pond
// (the baked field, water.ps) reaches on; and hollows beside a pond lying below its surface where the water
// mesh itself stops short of the bank (a level's ponds are coarse polygons). Both are part of the pond's
// ice in winter. The water surface of the baked field at xz, or the highest within two texels of it (in the
// ice months): x that level (DA_SN_NONE: none), y how much of xz lies under it - 1 from 5 cm below the
// level, faded out over the last half texel of the reach - so the ice runs on until the bank rises out of
// it, along the bank's own contour instead of the field's metre squares.
float2 da_sn_flood(float2 xz, float hg)
{
	[branch] if (da_snow_ice.x < 0.02 || da_water_map2.w < 0.5 || hg < DA_SN_NONE * 0.5)
		return float2(DA_SN_NONE, 0.0);
	const float2 uv = (xz - da_water_map.xy) * da_water_map.zw;
	[branch] if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
		return float2(DA_SN_NONE, 0.0);
	// one texel decides first: most of a level is far from any water
	uint w, h;
	s_water_field.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const int2 c = min(int2(uv * float2(w, h)), last);
	const float texel = da_water_map2.x;
	const float4 n = s_water_field.Load(int3(c, 0));
	[branch] if (n.y < 0.5 && n.w > 3.0 * texel)
		return float2(DA_SN_NONE, 0.0);
	bool flowing, inside;
	const float4 wf = da_sn_water(xz, flowing, inside);
	[branch] if (wf.y < 0.5 && wf.w > 2.5 * texel)
		return float2(DA_SN_NONE, 0.0);
	// on the field: its own surface; beside it: the highest of the water texels around
	float level = (wf.y > 0.5) ? wf.x : DA_SN_NONE;
	[branch] if (wf.y < 0.5)
	{
		[unroll] for (int j = -2; j <= 2; ++j)
		{
			[unroll] for (int i = -2; i <= 2; ++i)
			{
				const float4 f = s_water_field.Load(int3(clamp(c + int2(i, j), int2(0, 0), last), 0));
				level = (f.y > 0.5) ? max(level, f.x) : level;
			}
		}
	}
	[branch] if (level < DA_SN_NONE * 0.5)
		return float2(DA_SN_NONE, 0.0);
	const float reach = (wf.y > 0.5) ? 1.0 : saturate((2.5 * texel - wf.w) / (0.5 * texel));
	// Beside the field, a shore the collision's liquid faces miss lies centimetres under the water's surface; ground
	// lower than that lies past a wall or a rim - water standing in a house or a tank, the path outside no pond's bank
	// (it froze into a raised sheet that took no prints).
	const float bank = (wf.y > 0.5) ? 1.0 : saturate((0.2 - (level - hg)) / 0.05);
	return float2(level, saturate((level - hg) / 0.05) * reach * bank);
}

// Ice and the snow on it at xz, where the ground window has a water surface: x ice cover, y snow depth on
// the ice (m) (da_snow_ice.h, the rule water.ps draws by).
// Whether the field's water at xz is indoors (class 0.5: a flooded cellar, a factory floor), which never freezes.
bool da_sn_water_indoor(float2 xz)
{
	uint w, h;
	s_water_class.GetDimensions(w, h);
	const float2 uv = (xz - da_water_map.xy) * da_water_map.zw;
	const int2 t = clamp(int2(uv * float2(w, h)), int2(0, 0), int2(w, h) - 1);
	return abs(s_water_class.Load(int3(t, 0)) - 0.5) < 0.2;
}

float2 da_sn_ice_at(float2 xz)
{
	[branch] if (max(da_snow_ice.x, da_snow_ice.y) < 0.02)
		return float2(0.0, 0.0);
	bool flowing, inside;
	const float4 wf = da_sn_water(xz, flowing, inside);
	[branch] if (inside && da_sn_water_indoor(xz))
		return float2(0.0, 0.0);
	// Inside the baked field the front keeps its distance from the bank even on the waterline's texels the
	// field's nearest-texel mask leaves dry (the distance runs on through them, near nothing): the shore of
	// the ice follows the water's own edge instead of the field's metre squares.
	const float ice = da_ice_cover(xz, inside, flowing, wf.w);
	return float2(ice, da_ice_snow(xz, ice));
}

// The ice cover at xz before the holes broken into it (the snow grid's vertices: a hole is cut per pixel).
float da_sn_ice_whole(float2 xz)
{
	[branch] if (max(da_snow_ice.x, da_snow_ice.y) < 0.02)
		return 0.0;
	bool flowing, inside;
	const float4 wf = da_sn_water(xz, flowing, inside);
	[branch] if (inside && da_sn_water_indoor(xz))
		return 0.0;
	return da_ice_cover_whole(xz, inside, flowing, wf.w);
}

// The waterline of the window's own water at xz, from continuous quantities only: the bilinear ground (the bed
// under the water) against the level of the water within reach, so the line follows the ground's own contour at
// any distance instead of the water texels' 0.25 m squares (the stair-stepped shore of a frozen pond). The
// collision mesh knows the ground only to centimetres: a micro-relief noise (lobes of 1.3 m and 0.4 m, +-1.2 cm)
// lets the line meander on a flat shore and keeps it nearly straight along a steep bank, as a real waterline.
// x the level (DA_SN_NONE: no water within reach, or outside the window), y the share of xz under it (0.5 on
// the line, 0 at 1 cm and 1 at 5 cm under the level), z how much the window's water decides there: its reach
// (1 near the water, 0 at its end) faded out over the band at the window's edge.
float3 da_sn_shore(float2 xz, float wet)
{
	const float2 d = abs(xz - da_sn_win.xy);
	const float lw = saturate((max(d.x, d.y) - (da_sn_win.z - da_sn_win.w)) / max(da_sn_win.w, 1e-3));
	[branch] if (lw >= 1.0)
		return float3(DA_SN_NONE, 0.0, 0.0);
	const int n = int(da_sn_win2.x);
	const float2 p = xz * da_sn_win2.y - 0.5;
	const float2 i = floor(p);
	const float2 fr = p - i;
	const int2 t = int2(i);
	const float4 v00 = s_da_ground_shore.Load(int3(da_sn_wrap(t, n), 0));
	const float4 v10 = s_da_ground_shore.Load(int3(da_sn_wrap(t + int2(1, 0), n), 0));
	const float4 v01 = s_da_ground_shore.Load(int3(da_sn_wrap(t + int2(0, 1), n), 0));
	const float4 v11 = s_da_ground_shore.Load(int3(da_sn_wrap(t + int2(1, 1), n), 0));
	const float lvl = max(max(v00.y, v10.y), max(v01.y, v11.y));
	[branch] if (lvl < DA_SN_NONE * 0.5)
		return float3(DA_SN_NONE, 0.0, 0.0);
	const float4 bw = float4((1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y, fr.x * fr.y);
	// how far each texel's ground lies under that level; no solid surface under the water at all is deep
	const float4 g = float4(v00.x, v10.x, v01.x, v11.x);
	const float4 under = (g > DA_SN_NONE * 0.5) ? clamp(lvl - g, -2.0, 2.0) : 2.0;
	const float relief = 0.016 * da_sn_vnoise(xz * (1.0 / 1.3), 0x93u) + 0.008 * da_sn_vnoise(xz * (1.0 / 0.4), 0x94u) - 0.012;
	const float u = dot(under, bw) + relief;
	const float reach = dot(float4(v00.z, v10.z, v01.z, v11.z), bw) * (1.0 - lw);
	// Off the water (wet: the share of xz the window's water lies over), a bank lies centimetres under the water's
	// surface; ground lower than that lies past a wall or a rim (a liquid face on a house's floor made the path by
	// the house a pond's bank: a sheet of ice and snow raised over it, torn at its edges)
	const float bank = lerp(saturate((0.2 - u) / 0.05), 1.0, saturate(wet * 2.0));
	return float3(lvl, saturate((u - 0.03) * 25.0 + 0.5) * bank, reach);
}

// A frozen pond's waterline at xz for the passes that draw its ice (the snow grid and the layer): the window's
// own water (da_sn_shore) and, where its reach ends or the window does, ground below the surface of the baked
// field's pond (da_sn_flood); the share blends from one to the other by the window's reach, so they meet without
// a seam. b = da_sn_base(xz). x the level (DA_SN_NONE: none), y the share of xz under it, z the window's weight.
float3 da_sn_pond(float2 xz, float3 b)
{
	const float3 s = da_sn_shore(xz, b.z);
	[branch] if (s.z >= 0.999)
		return float3(s.x, s.y, 1.0);
	const float2 f = da_sn_flood(xz, b.x);
	return float3((s.x > DA_SN_NONE * 0.5) ? s.x : f.x, lerp(f.y, s.y, s.z), s.z);
}

// The snow surface of a trail texel (heights above the base): untouched texels and deformers that
// never got below the surface keep the season depth; a print never goes below the compacted residual;
// a crater's floor (the word 0) is bare until snowfall refills it, and so is the hollow of a loose object
// lying on the ground (no foot: the low half 0) - a pistol on 3 cm of packed snow was out of sight.
float da_sn_resolve(float D, uint v, float residual_ratio)
{
	const float carve = da_sn_f16(v >> 16);
	const float foot = da_sn_f16(v & 0xFFFFu);
	const float residual = ((v & 0xFFFFu) == 0u) ? 0.0 : min(D, max(0.02, D * residual_ratio));
	float surf = (foot < D) ? clamp(carve, residual, D) : D;
	surf = (v == DA_SN_UNTOUCHED) ? D : surf;
	return (v == 0u) ? 0.0 : surf;
}

// The roof map: how far under the shells' (or a cover's coat's) surface the near window's deformers pressed the snow
// (da_snow_stamp.cs).
Texture2D<uint> s_da_sn_roof;

// How far a roof's snow is pressed at xz (m), bilinear; 0 outside the near trail window.
float da_sn_roof_dep(float2 xz)
{
	const float2 dd = abs(xz - da_snow_fine_xf.zw);
	[branch] if (da_snow_misc.z < 0.5 || max(dd.x, dd.y) > da_snow_misc.y - 0.5)
		return 0.0;
	const float n = da_snow_fine_xf.y;
	const float2 p = xz * da_snow_fine_xf.x - 0.5;
	const float2 i = floor(p);
	const float2 f = p - i;
	const float2 a = i - n * floor(i / n);
	const float2 b = a + 1.0 - n * step(n, a + 1.0);
	const float k = da_snow_misc.x / 65535.0;
	const float v00 = float(s_da_sn_roof.Load(int3(int2(a), 0)) & 0xFFFFu), v10 = float(s_da_sn_roof.Load(int3(int(b.x), int(a.y), 0)) & 0xFFFFu);
	const float v01 = float(s_da_sn_roof.Load(int3(int(a.x), int(b.y), 0)) & 0xFFFFu), v11 = float(s_da_sn_roof.Load(int3(int2(b), 0)) & 0xFFFFu);
	return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y) * k;
}

#endif // DA_SNOW_COMMON_H
