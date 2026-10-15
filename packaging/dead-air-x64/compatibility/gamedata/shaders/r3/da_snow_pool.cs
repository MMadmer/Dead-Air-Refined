// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The fine trail map (L0) handed down to the coarse one (L1) where deformers worked (da_snow.cpp): one
// group per 64 x 64 L0 tile, one thread per L1 texel (8 x 8 L0 texels). The L1 texel keeps the mean
// carved surface over its block - the volume the prints took out - and the lowest deformer. L0 is the
// truth inside its window, so a block without a print clears the L1 texel.

#include "da_snow_trail.h"

Texture2D<uint> s_da_sn_l0;
StructuredBuffer<uint> s_sn_tiles : register(t8); // L0 tiles (64 texels): x | z << 16, each biased by 32768

RWTexture2D<uint> u_map : register(u0);

uniform float4 da_sn_map; // L0: x texels per metre, y texels per side, zw first world texel of the window

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint tile = s_sn_tiles[gid.x];
	const int2 tc = int2(int(tile & 0xFFFFu) - 32768, int(tile >> 16) - 32768);
	const int2 t1 = tc * 8 + int2(tid.xy);
	const int2 b0 = t1 * 8;
	const int n0 = int(da_sn_map.y);
	const int2 w0 = int2(da_sn_map.zw);
	const int n1 = int(da_sn_map1.y);
	const int2 w1 = int2(da_sn_map1.zw);
	[branch] if (any(b0 < w0) || any(b0 + 8 > w0 + n0) || any(t1 < w1) || any(t1 >= w1 + n1))
		return;

	const float D = da_sn_depth_smooth((float2(t1) + 0.5) / da_sn_map1.x);
	float carve = 0.0;
	uint foot = 0xFFFFu;
	uint touched = 0u;
	[loop] for (int j = 0; j < 8; ++j)
	{
		[unroll] for (int i = 0; i < 8; ++i)
		{
			const uint v = s_da_sn_l0.Load(int3(da_sn_wrap(b0 + int2(i, j), n0), 0));
			float h = D;
			[branch] if (v != DA_SN_UNTOUCHED)
			{
				++touched;
				const uint f = v & 0xFFFFu;
				foot = min(foot, f);
				[branch] if (da_sn_f16(f) < D)
					h = min(da_sn_f16(v >> 16), D);
			}
			carve += h;
		}
	}
	u_map[da_sn_wrap(t1, n1)] = (touched > 0u) ? ((da_sn_u16(carve / 64.0) << 16) | foot) : DA_SN_UNTOUCHED;
}
