#include "common.h"
#define DA_FROXEL_NO_SAMPLE
#include "da_froxel.h"

// Front to back along every column of the froxel grid (da_froxel.h): what the air has scattered toward the eye up to
// the far edge of each slice and how much of what lies behind it still gets through. Each slice's in-scatter is
// integrated against its own extinction (Hillaire 2015), so a thick cell does not add more light than it can.

Texture3D s_froxel_cur;
RWTexture3D<float4> u_froxel_int : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (any(id.xy >= uint2(da_froxel_grid.xy)))
		return;
	float3 light = 0.0f;
	float T = 1.0f;
	const uint slices = uint(da_froxel_grid.z);
	[loop]
	for (uint i = 0; i < slices; ++i)
	{
		const float4 v = s_froxel_cur.Load(int4(id.xy, i, 0));
		const float dz = da_froxel_depth(float(i + 1)) - da_froxel_depth(float(i));
		const float sigma = max(v.a, 1e-6f);
		const float Ts = exp(-sigma * dz);
		light += T * v.rgb * ((1.0f - Ts) / sigma);
		T *= Ts;
		u_froxel_int[uint3(id.xy, i)] = float4(light, T);
	}
}
