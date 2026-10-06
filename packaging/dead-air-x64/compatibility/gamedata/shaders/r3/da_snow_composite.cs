// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The current snow depth of the fine window ($user$da_snow_fine, da_snow.cpp) over a list of 64 x 64
// tiles: the season depth from the level fields as a cubic B-spline (da_sn_depth_smooth: no slope crease
// at the 1 m texel borders) with the ragged margin of thin snow, the fine trail map resolved against it
// (RotTR: carve only where the deformer reached below the surface; never below the compacted residual),
// and the slight rim the displaced snow builds beside a print. The previous frame's depth moves to the
// high half, so the motion vectors of every consumer see what the surface did. How much of the snow a
// print compacted goes to $user$da_snow_trod (the snow layer's trodden look).

#include "da_snow_common.h"

Texture2D<uint> s_da_sn_l0;
StructuredBuffer<uint> s_sn_tiles : register(t8); // L0 tiles (64 texels): x | z << 16, each biased by 32768

RWTexture2D<uint> u_fine : register(u0);
RWTexture2D<unorm float> u_trod : register(u1);

uniform float4 da_sn_map;    // L0: x texels per metre, y texels per side, zw first world texel of the window
uniform float4 da_sn_comp;   // x residual share open, y residual share road, z 1 = no history at all, w unused
uniform float4 da_sn_fresh0; // world texels x0, z0, x1, z1 (exclusive) without history (scrolled in)
uniform float4 da_sn_fresh1;

#define DA_SN_AP 2
#define DA_SN_GS (8 + 2 * DA_SN_AP)
// Level texels one group's B-spline reads: 8 fine texels span 0.375 m, the patchy season's warp
// (da_sn_warp) moves a read point by under a texel either way, so four whole texels plus the spline's
// support.
#define DA_SN_LV 7

groupshared uint gs_l0[DA_SN_GS * DA_SN_GS];
groupshared float gs_lv[DA_SN_LV * DA_SN_LV];
groupshared float gs_road[DA_SN_LV * DA_SN_LV];

bool da_sn_in_rect(int2 t, float4 r)
{
	return t.x >= int(r.x) && t.y >= int(r.y) && t.x < int(r.z) && t.y < int(r.w);
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	const uint tile = s_sn_tiles[gid.z];
	const int2 tc = int2(int(tile & 0xFFFFu) - 32768, int(tile >> 16) - 32768);
	const int2 o = tc * 64 + int2(gid.xy) * 8;
	const int n = int(da_sn_map.y);
	const int2 w0 = int2(da_sn_map.zw);
	// the first level texel of this group's spline support, warp included
	const int2 lv0 = int2(floor(da_sn_level_coord((float2(o) + 0.5) / da_sn_map.x))) - 2;

	[loop] for (uint k = gi; k < uint(DA_SN_GS * DA_SN_GS); k += 64u)
	{
		const int2 t = o - DA_SN_AP + int2(k % DA_SN_GS, k / DA_SN_GS);
		const bool inside = all(t >= w0) && all(t < w0 + n);
		gs_l0[k] = inside ? s_da_sn_l0.Load(int3(da_sn_wrap(t, n), 0)) : DA_SN_UNTOUCHED;
	}
	[branch] if (gi < uint(DA_SN_LV * DA_SN_LV))
	{
		const int2 t = lv0 + int2(gi % DA_SN_LV, gi / DA_SN_LV);
		gs_lv[gi] = da_sn_texel_depth(t);
		gs_road[gi] = s_da_sn_weights.Load(int3(t, 0)).z;
	}
	GroupMemoryBarrierWithGroupSync();

	const int2 t = o + int2(tid.xy);
	[branch] if (all(t >= w0) && all(t < w0 + n))
	{
		const float2 xz = (float2(t) + 0.5) / da_sn_map.x;
		const float patchy = da_sn_patchy();
		const float2 p = da_sn_level_coord(da_sn_warp(xz, patchy));
		const float2 pi = floor(p);
		const float4 wx = da_sn_bspline(p.x - pi.x);
		const float4 wz = da_sn_bspline(p.y - pi.y);
		const int2 s0 = clamp(int2(pi) - 1 - lv0, int2(0, 0), int2(DA_SN_LV - 4, DA_SN_LV - 4));
		float D = 0.0;
		[unroll] for (int j = 0; j < 4; ++j)
		{
			float row = 0.0;
			[unroll] for (int i = 0; i < 4; ++i)
				row += wx[i] * gs_lv[(s0.y + j) * DA_SN_LV + s0.x + i];
			D += wz[j] * row;
		}
		D = da_sn_shape(max(D, 0.0), xz, 1.0, patchy);

		// the road's share of the texel at the true point, bilinear (the compacted residual of a print there)
		const float2 pr = da_sn_level_coord(xz);
		const float2 pri = floor(pr);
		const float2 fr = pr - pri;
		const int2 r0 = int2(pri) - lv0;
		const float road = lerp(lerp(gs_road[r0.y * DA_SN_LV + r0.x], gs_road[r0.y * DA_SN_LV + r0.x + 1], fr.x),
			lerp(gs_road[(r0.y + 1) * DA_SN_LV + r0.x], gs_road[(r0.y + 1) * DA_SN_LV + r0.x + 1], fr.x), fr.y);
		const float share = lerp(da_sn_comp.x, da_sn_comp.y, road);
		const int2 c = int2(tid.xy) + DA_SN_AP;
		float surf = da_sn_resolve(D, gs_l0[c.y * DA_SN_GS + c.x], share);

		// Rim: the little snow a print pushes aside (most of it is compacted, not displaced), from the
		// mean depression of the 5 x 5 around; a few centimetres at most.
		const float dep = D - surf;
		float sum = 0.0;
		[unroll] for (int jj = -DA_SN_AP; jj <= DA_SN_AP; ++jj)
			[unroll] for (int ii = -DA_SN_AP; ii <= DA_SN_AP; ++ii)
				sum += D - da_sn_resolve(D, gs_l0[(c.y + jj) * DA_SN_GS + c.x + ii], share);
		const float rim = min(min(0.2 * max(sum * (1.0 / 25.0) - dep, 0.0), 0.25 * D), 0.03);
		surf = min(surf + rim, da_snow_misc.x);

		const int2 addr = da_sn_wrap(t, n);
		const bool fresh = da_sn_comp.z > 0.5 || da_sn_in_rect(t, da_sn_fresh0) || da_sn_in_rect(t, da_sn_fresh1);
		const uint cur = da_sn_u16(surf);
		const uint prev = fresh ? cur : (u_fine[addr] & 0xFFFFu);
		u_fine[addr] = cur | (prev << 16);
		// a print of 3 cm and more reads as fully compacted
		u_trod[addr] = saturate(dep * (1.0 / 0.03));
	}
}
