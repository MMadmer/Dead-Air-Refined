// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Clears a rectangle of a toroidal R32_UINT map (world texel modulo its size): the strips the near
// ground window and the trail maps scroll into (da_ground.cpp, da_snow.cpp).

RWTexture2D<uint> u_map : register(u0);

uniform float4 da_sn_rect;  // xy first world texel, zw size in texels
uniform float4 da_sn_clear; // x texels per side, y 1 = untouched (0xFFFFFFFF), 0 = zero

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (id.x >= uint(da_sn_rect.z) || id.y >= uint(da_sn_rect.w))
		return;
	const int n = int(da_sn_clear.x);
	const int2 t = int2(da_sn_rect.xy) + int2(id.xy);
	// world texel modulo the map size; every world texel of a level is above -256 n
	u_map[int2(uint2(t + n * 256) % uint(n))] = (da_sn_clear.y > 0.5) ? 0xFFFFFFFFu : 0u;
}
