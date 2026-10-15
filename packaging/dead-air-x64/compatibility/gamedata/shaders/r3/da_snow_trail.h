// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The trail maps (da_snow.cpp): R32_UINT, toroidal (world texel modulo the map size), high 16 bits the
// carved snow surface, low 16 bits the lowest deformer, both heights above the snow base, unorm over
// [0, cap]; 0xFFFFFFFF = untouched. L0 is the fine map around the camera (4.7 cm), L1 the coarse one
// (37.5 cm) that keeps the trails the fine window scrolls off, and gives them back when it returns.
#ifndef DA_SNOW_TRAIL_H
#define DA_SNOW_TRAIL_H

#include "da_snow_common.h"

Texture2D<uint> s_da_sn_l1;

uniform float4 da_sn_map1; // L1: x texels per metre, y texels per side, zw first world texel of the window

// L1 at a world position, bilinear: carved surfaces blend (an untouched texel counts as the cap), the
// lowest deformer of the touched texels is kept. Untouched when no touched L1 texel is around.
uint da_sn_l1_sample(float2 xz)
{
	const int n = int(da_sn_map1.y);
	const int2 w0 = int2(da_sn_map1.zw);
	const float2 q = xz * da_sn_map1.x - 0.5;
	const float2 i = floor(q);
	const float2 fr = q - i;
	const int2 t = int2(i);
	const float4 bw = float4((1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y, fr.x * fr.y);
	float carve = 0.0;
	uint foot = 0xFFFFu;
	bool touched = false;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		const int2 tk = t + int2(k & 1, k >> 1);
		uint v = DA_SN_UNTOUCHED;
		[branch] if (all(tk >= w0) && all(tk < w0 + n))
			v = s_da_sn_l1.Load(int3(da_sn_wrap(tk, n), 0));
		[branch] if (v != DA_SN_UNTOUCHED)
		{
			touched = true;
			foot = min(foot, v & 0xFFFFu);
		}
		carve += float(v >> 16) * bw[k];
	}
	return touched ? ((min(uint(carve + 0.5), 0xFFFFu) << 16) | foot) : DA_SN_UNTOUCHED;
}

#endif // DA_SNOW_TRAIL_H
