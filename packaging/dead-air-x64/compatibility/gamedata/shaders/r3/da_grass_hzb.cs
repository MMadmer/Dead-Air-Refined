// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The depth pyramid of the blades' occlusion test (docs/dead-air/GRASS.md, "Occlusion"; da_grass_blades.cpp
// build_hzb): level 0 holds the farthest depth of every 2 x 2 pixels of the depth the opaque world, the LODs and the
// vegetation left this frame, each level after it the farthest of 2 x 2 texels of the one before. All levels lie in
// one texture: level 0 at the origin, the rest stacked down a column to its right. A texel x of level k covers the
// pixels [x 2^(k+1), (x + 1) 2^(k+1)), the last one of an odd row whatever is left.

Texture2D<float>	s_depth_copy;
RWTexture2D<float>	da_hzb		: register(u0);

uniform float4	da_hzb_src;	// xy first texel of the level read (x < 0: the depth itself), zw its size
uniform float4	da_hzb_dst;	// xy first texel of the level written, zw its size

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= uint2(da_hzb_dst.zw)))
		return;
	const int2 s0 = int2(id.xy) * 2;
	const int2 last = int2(da_hzb_src.zw) - 1;
	const bool depth = da_hzb_src.x < 0.0f;
	float d = 0.0f;
	[unroll] for (int y = 0; y < 2; ++y)
	{
		[unroll] for (int x = 0; x < 2; ++x)
		{
			const int2 p = min(s0 + int2(x, y), last);
			d = max(d, depth ? s_depth_copy.Load(int3(p, 0)) : da_hzb[int2(da_hzb_src.xy) + p]);
		}
	}
	da_hzb[int2(da_hzb_dst.xy) + int2(id.xy)] = d;
}
