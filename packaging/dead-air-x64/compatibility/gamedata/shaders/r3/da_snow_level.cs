// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The current snow depth of the whole level at 1 m ($user$da_snow_level, da_snow.cpp) over a
// rectangle of texels: the season depth per class from the level fields (warped while the snow is patchy)
// with the coarse ragged margin of thin snow (the fine texture adds the sub-metre one), and the coarse
// trails (L1) resolved into it
// around the camera. The previous frame's depth moves to the high half.

#include "da_snow_trail.h"

RWTexture2D<uint> u_level : register(u0);

uniform float4 da_sn_rect; // xy first level texel, zw size in texels
uniform float4 da_sn_comp; // x residual share open, y residual share road, z 1 = no history, w 1 = L1 holds trails

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (id.x >= uint(da_sn_rect.z) || id.y >= uint(da_sn_rect.w))
		return;
	const int2 t = int2(da_sn_rect.xy) + int2(id.xy);
	const float2 xz = (float2(t) + 0.5 - da_snow_level_xf.zw) / da_snow_level_xf.xy;
	// patchy snow: the season depth at the warped point, as the fine texture takes it (da_sn_warp)
	const float patchy = da_sn_patchy();
	float D = (patchy > 0.0) ? da_sn_depth_bilinear(da_sn_warp(xz, patchy)) : da_sn_texel_depth(t);
	D = da_sn_shape(D, xz, 0.0, patchy) * (1.0 - s_da_sn_shelter.Load(int3(t, 0)));
	[branch] if (da_sn_comp.w > 0.5 && D > 0.0)
	{
		const float share = lerp(da_sn_comp.x, da_sn_comp.y, s_da_sn_weights.Load(int3(t, 0)).z);
		D = da_sn_resolve(D, da_sn_l1_sample(xz), share);
	}
	const uint cur = da_sn_u16(D);
	const uint prev = (da_sn_comp.z > 0.5) ? cur : (u_level[t] & 0xFFFFu);
	u_level[t] = cur | (prev << 16);
}
