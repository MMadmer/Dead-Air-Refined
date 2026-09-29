#include "common.h"
#include "da_vrs.h"

//	phase_vrs_luma (r4_rendertarget_phase_vrs.cpp): the finished render-size frame - after the
//	anti-aliasing and the sun shafts - as one texel per 8x8 pixel block: x = RMS of the horizontal
//	luma step, y = RMS of the vertical one, z = mean luma, w = 1 (written). The next frame's rate pass
//	reads it at the reprojected position. A group covers 32x32 pixels (sixteen blocks), a thread 4x4
//	of them. The frame is sampled at the render pixels' centres: exactly its texels at render size,
//	the resolved image resampled to render size when it is an upscaler's display-size output.
Texture2D s_vrs_frame;
RWTexture2D<float4> u_vrs_contrast : register(u0);

//	xy = the render size in pixels.
uniform float4 da_vrs_size;

//	The region and a one-pixel apron to the right and below, clamped to the frame.
groupshared float g_luma[33 * 33];
//	Per thread, over its 4x4 pixels: the squared horizontal steps, the squared vertical ones, the luma.
groupshared float3 g_part[64];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex)
{
	const int2 size = int2(da_vrs_size.xy);
	const int2 origin = int2(group.xy) * 32;
	const float2 texel = 1.0 / float2(size);

	for (uint i = index; i < 33u * 33u; i += 64u)
	{
		const int2 p = min(origin + int2(i % 33u, i / 33u), size - 1);
		g_luma[i] = da_vrs_luma(s_vrs_frame.SampleLevel(smp_rtlinear, (float2(p) + 0.5) * texel, 0).rgb);
	}
	GroupMemoryBarrierWithGroupSync();

	float3 sum = float3(0.0, 0.0, 0.0);
	const uint2 block = local.xy * 4u;
	[unroll]
	for (uint k = 0u; k < 16u; ++k)
	{
		const uint2 lp = block + uint2(k & 3u, k >> 2);
		const uint at = lp.y * 33u + lp.x;
		const float l = g_luma[at];
		const float dx = g_luma[at + 1u] - l;
		const float dy = g_luma[at + 33u] - l;
		const float inside = all(origin + int2(lp) < size) ? 1.0 : 0.0;
		sum += float3(dx * dx, dy * dy, l) * inside;
	}
	g_part[index] = sum;
	GroupMemoryBarrierWithGroupSync();

	//	Block b is the 2x2 threads from thread (2 b.x, 2 b.y) on.
	[branch] if (index < 16u)
	{
		const int2 b = int2(index & 3u, index >> 2);
		const int2 covered = min(size - (origin + b * 8), int2(8, 8));
		[branch] if (all(covered > 0))
		{
			const uint first = uint(b.y) * 16u + uint(b.x) * 2u;
			const float3 total = g_part[first] + g_part[first + 1u] + g_part[first + 8u] + g_part[first + 9u];
			const float3 mean = total / float(covered.x * covered.y);
			u_vrs_contrast[int2(group.xy) * 4 + b] = float4(sqrt(mean.x), sqrt(mean.y), mean.z, 1.0);
		}
	}
}
