// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Fresh snowfall fills the trails (da_snow.cpp): every carved surface rises by the snow that fell since
// the last refill; a texel that reaches the cap is untouched again. The roof map (z = 1) holds depressions: they
// shrink by the same steps.

RWTexture2D<uint> u_map : register(u0);

uniform float4 da_sn_refill; // x unorm steps to add, y texels per side, z 1 = the roof map

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (id.x >= uint(da_sn_refill.y) || id.y >= uint(da_sn_refill.y))
		return;
	const uint v = u_map[id.xy];
	[branch] if (da_sn_refill.z > 0.5)
	{
		u_map[id.xy] = (v > uint(da_sn_refill.x)) ? v - uint(da_sn_refill.x) : 0u;
		return;
	}
	[branch] if (v == 0xFFFFFFFFu)
		return;
	const uint h = (v >> 16) + uint(da_sn_refill.x);
	u_map[id.xy] = (h >= 0xFFFFu) ? 0xFFFFFFFFu : ((h << 16) | (v & 0xFFFFu));
}
