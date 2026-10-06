// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Fresh snowfall fills the trails (da_snow.cpp): every carved surface rises by the snow that fell since
// the last refill; a texel that reaches the cap is untouched again.

RWTexture2D<uint> u_map : register(u0);

uniform float4 da_sn_refill; // x unorm steps to add, y texels per side

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (id.x >= uint(da_sn_refill.y) || id.y >= uint(da_sn_refill.y))
		return;
	const uint v = u_map[id.xy];
	[branch] if (v == 0xFFFFFFFFu)
		return;
	const uint h = (v >> 16) + uint(da_sn_refill.x);
	u_map[id.xy] = (h >= 0xFFFFu) ? 0xFFFFFFFFu : ((h << 16) | (v & 0xFFFFu));
}
