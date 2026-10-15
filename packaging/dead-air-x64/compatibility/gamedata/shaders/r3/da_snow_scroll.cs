// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The texels the fine trail map (L0) scrolls into take the coarse map's trails (L1): a path walked an
// hour ago is still there when the camera comes back, softened to the coarse map's resolution.

#include "da_snow_trail.h"

RWTexture2D<uint> u_map : register(u0);

uniform float4 da_sn_rect; // xy first world texel (L0), zw size in texels
uniform float4 da_sn_map;  // L0: x texels per metre, y texels per side, zw first world texel of the window

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (id.x >= uint(da_sn_rect.z) || id.y >= uint(da_sn_rect.w))
		return;
	const int2 t = int2(da_sn_rect.xy) + int2(id.xy);
	const float2 p = (float2(t) + 0.5) / da_sn_map.x;
	u_map[da_sn_wrap(t, int(da_sn_map.y))] = da_sn_l1_sample(p);
}
