#include "common.h"
#define DA_FROXEL_NO_SAMPLE
#include "da_froxel.h"

// Front to back along every column of the froxel grid (da_froxel.h): what the air has scattered toward the eye up to
// the far edge of each slice and how much of what lies behind it still gets through. Each slice's in-scatter is
// integrated against its own extinction (Hillaire 2015): the air scatters what it takes out of the sun's light, so a
// slice adds the sunlit share of 1 - its transmittance and no more.

Texture3D s_froxel_cur;
RWTexture3D<float4> u_froxel_int : register(u0);

// rgb: the colour of the light the air scatters, times the weather's strength (isotropic: the shafts must not crowd
// the sun, see the history of accum_volumetric_sun.ps); w: unused.
uniform float4 da_froxel_sun;

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
		const float2 v = s_froxel_cur.Load(int4(id.xy, i, 0)).xy;
		const float dz = da_froxel_depth(float(i + 1)) - da_froxel_depth(float(i));
		const float Ts = exp(-v.x * dz);
		light += T * da_froxel_sun.rgb * (v.y * (1.0f - Ts));
		T *= Ts;
		u_froxel_int[uint3(id.xy, i)] = float4(light, T);
	}
}
