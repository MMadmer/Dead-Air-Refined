// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The occlusion test against the G-buffer stage's depth pyramid (da_hzb.cs, da_hzb.cpp da_hzb_bind; docs/dead-air/GRASS.md,
// "Occlusion"): a bounding sphere or box lies behind what is drawn when its nearest depth is farther than the farthest
// depth of the pyramid over its rectangle on the screen and a pixel more, read at the level where the rectangle spans two
// texels at most. Never hidden while it reaches before the near plane, off the screen, or with the test off. Define
// DA_HZB_SLOT (the pyramid's texture register, t6 by default) before including it.
#ifndef DA_HZB_H
#define DA_HZB_H

#ifndef DA_HZB_SLOT
#define DA_HZB_SLOT t6
#endif

Texture2D<float>	da_hzb_tex		: register(DA_HZB_SLOT);
uniform float4	da_hzb;			// x 1 = the test is on, y the pyramid's levels, zw the depth's size (pixels)
uniform float4	da_hzb_lv[14];	// each level: xy its first texel, zw its size
uniform float4	da_hzb_view[3];	// world -> view: the rows of x, y and z (dot with the point and 1)
uniform float4	da_hzb_proj;	// the projection's _11, _22, _33, _43
uniform float4	da_hzb_proj2;	// its _31, _32 (the jitter), the near plane, -

bool	hzb_on	()	{ return da_hzb.x > 0.5f; }

float3	hzb_view_pos	(float3 c)
{
	const float4 p = float4(c, 1.0f);
	return float3(dot(p, da_hzb_view[0]), dot(p, da_hzb_view[1]), dot(p, da_hzb_view[2]));
}

// The screen rectangle [nlo, nhi] (normalized device x and y) at the nearest view depth zn.
bool	hzb_rect_hidden	(float2 nlo, float2 nhi, float zn)
{
	const float2 size = da_hzb.zw;
	const int2 p0 = max(int2(floor(float2(nlo.x * 0.5f + 0.5f, 0.5f - nhi.y * 0.5f) * size)) - 1, 0);
	const int2 p1 = min(int2(floor(float2(nhi.x * 0.5f + 0.5f, 0.5f - nlo.y * 0.5f) * size)) + 1, int2(size) - 1);
	if (any(p1 < p0))
		return false;
	const uint span = uint(max(p1.x - p0.x, p1.y - p0.y));
	const uint k = min(span > 0u ? firstbithigh(span) : 0u, uint(da_hzb.y) - 1u);
	const float4 lv = da_hzb_lv[k];
	const int2 o = int2(lv.xy), last = int2(lv.zw) - 1;
	const int2 t0 = min(p0 >> int(k + 1u), last), t1 = min(p1 >> int(k + 1u), last);
	const float deepest = max(max(da_hzb_tex.Load(int3(o + t0, 0)), da_hzb_tex.Load(int3(o + int2(t1.x, t0.y), 0))),
		max(da_hzb_tex.Load(int3(o + int2(t0.x, t1.y), 0)), da_hzb_tex.Load(int3(o + t1, 0))));
	return da_hzb_proj.z + da_hzb_proj.w / zn > deepest;
}

// A sphere (world centre c, radius r): its rectangle its exact perspective bounds (the tangents from the eye, Mara and
// McGuire 2013).
bool	hzb_sphere_hidden	(float3 c, float r)
{
	if (!hzb_on())
		return false;
	const float3 v = hzb_view_pos(c);
	const float zn = v.z - r;
	if (zn <= da_hzb_proj2.z)
		return false;
	const float den = v.z * v.z - r * r;
	const float2 ux = (v.x * v.z + float2(-r, r) * sqrt(v.x * v.x + den)) / den;
	const float2 uy = (v.y * v.z + float2(-r, r) * sqrt(v.y * v.y + den)) / den;
	const float2 nx = ux * da_hzb_proj.x + da_hzb_proj2.x;
	const float2 ny = uy * da_hzb_proj.y + da_hzb_proj2.y;
	return hzb_rect_hidden(float2(nx.x, ny.x), float2(nx.y, ny.y), zn);
}

// A world box [lo, hi]: the bounds of its eight corners.
bool	hzb_box_hidden	(float3 lo, float3 hi)
{
	if (!hzb_on())
		return false;
	float2 nlo = 1e30f, nhi = -1e30f;
	float zn = 1e30f;
	[unroll] for (uint i = 0; i < 8u; ++i)
	{
		const float3 v = hzb_view_pos(float3((i & 1u) ? hi.x : lo.x, (i & 2u) ? hi.y : lo.y, (i & 4u) ? hi.z : lo.z));
		if (v.z <= da_hzb_proj2.z)
			return false;
		const float2 n = v.xy / v.z * da_hzb_proj.xy + da_hzb_proj2.xy;
		nlo = min(nlo, n);
		nhi = max(nhi, n);
		zn = min(zn, v.z);
	}
	return hzb_rect_hidden(nlo, nhi, zn);
}

#endif
