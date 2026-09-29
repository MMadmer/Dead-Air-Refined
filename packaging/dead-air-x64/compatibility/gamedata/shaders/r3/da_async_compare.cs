#include "common.h"

//	r__async_verify (da_async.cpp): the same pass run twice - pixel and compute, or inline and on
//	the compute queue - is compared texel by texel on the GPU. Every texel whose bits differ is
//	counted, and the largest difference over its channels is kept; both land in a 2x1 R32_UINT
//	target the CPU reads back: x = 0 the count, x = 1 the largest difference as float bits (a
//	non-negative float orders like its bit pattern, so InterlockedMax on the bits is a max).
Texture2D<float4> s_cmp_a;
Texture2D<float4> s_cmp_b;
RWTexture2D<uint> u_cmp : register(u0);

//	xy = the compared size in texels.
uniform float4 da_cmp_size;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (any(id.xy >= uint2(da_cmp_size.xy)))
		return;
	const float4 a = s_cmp_a.Load(int3(id.xy, 0));
	const float4 b = s_cmp_b.Load(int3(id.xy, 0));
	[branch] if (any(asuint(a) != asuint(b)))
	{
		InterlockedAdd(u_cmp[uint2(0, 0)], 1u);
		const float4 d = abs(a - b);
		InterlockedMax(u_cmp[uint2(1, 0)], asuint(max(max(d.x, d.y), max(d.z, d.w))));
	}
}
