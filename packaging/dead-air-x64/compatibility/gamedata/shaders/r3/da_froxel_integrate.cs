#include "common.h"
#define DA_FROXEL_NO_SAMPLE
#include "da_froxel.h"

// Front to back along every column of the froxel grid (da_froxel.h): what the air has scattered toward the eye up to
// the far edge of each slice and how much of what lies behind it still gets through. Each slice's in-scatter is
// integrated against its own extinction (Hillaire 2015): the air scatters what it takes out of the light, so a slice
// adds the lit share of 1 - its transmittance and no more. The light is the sun's where the cascades and the clouds
// let it through, and the player's torch inside its cone.

Texture3D s_froxel_cur;
RWTexture3D<float4> u_froxel_int : register(u0);

// rgb: the colour of the light the air scatters, times the weather's strength (isotropic: the shafts must not crowd
// the sun, see the history of accum_volumetric_sun.ps); w: unused.
uniform float4 da_froxel_sun;
// The player's torch in view space (r4_rendertarget_phase_froxels.cpp): xyz its position, w 1 / range^2 (0: none);
// xyz its direction, w the cosine of its half cone; rgb its light at the beam's strength, w the cosine where the
// cone's edge has faded in. From the eye it needs no shadow: what hides the beam lies behind the hider on screen too.
uniform float4 da_froxel_torch_pos;
uniform float4 da_froxel_torch_dir;
uniform float4 da_froxel_torch_col;

// The torch's light at view point P: the lamps' quadratic fall-off, the cone's soft edge.
float3 da_froxel_torch(float3 P)
{
	const float3 d = P - da_froxel_torch_pos.xyz;
	const float r2 = dot(d, d);
	float att = saturate(1.0f - r2 * da_froxel_torch_pos.w);
	att *= att;
	// The beam fades in from the cone's edge most of the way to its axis, squared: a torch's hot centre, no rim.
	const float edge = smoothstep(da_froxel_torch_dir.w, da_froxel_torch_col.w, dot(d * rsqrt(max(r2, 1e-4f)), da_froxel_torch_dir.xyz));
	att *= edge * edge;
	return da_froxel_torch_col.rgb * att;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (any(id.xy >= uint2(da_froxel_grid.xy)))
		return;
	const float2 uv = (float2(id.xy) + 0.5f) / da_froxel_grid.xy;
	const bool torch = da_froxel_torch_pos.w > 0.0f;
	float3 light = 0.0f;
	float T = 1.0f;
	const uint slices = uint(da_froxel_grid.z);
	[loop]
	for (uint i = 0; i < slices; ++i)
	{
		const float2 v = s_froxel_cur.Load(int4(id.xy, i, 0)).xy;
		const float dz = da_froxel_depth(float(i + 1)) - da_froxel_depth(float(i));
		const float Ts = exp(-v.x * dz);
		float3 lit = da_froxel_sun.rgb * v.y;
		[branch] if (torch)
			lit += da_froxel_torch(da_froxel_view(uv, da_froxel_depth(float(i) + 0.5f)));
		light += T * lit * (1.0f - Ts);
		T *= Ts;
		u_froxel_int[uint3(id.xy, i)] = float4(light, T);
	}
}
