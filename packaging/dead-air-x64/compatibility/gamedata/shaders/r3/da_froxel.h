#ifndef DA_FROXEL_H
#define DA_FROXEL_H

// The froxel volume (r4_rendertarget_phase_froxels.cpp), after DOOM Eternal's light scattering: a grid over the view
// frustum, its slices spread quadratically in view depth, the sunlit air of every cell injected with the cascades'
// shadows and the clouds', filtered over frames, then integrated front to back. A pixel reads what the air scattered
// toward the eye up to its depth.

uniform float4 da_froxel_grid;	// xyz: the grid's size, w: unused
uniform float4 da_froxel_range;	// x: near plane (m), y: far - near, z: 1 / (far - near), w: far plane
uniform float4 da_froxel_ray;	// xy: the view-space ray per unit of NDC (1 / P11, 1 / P22), zw: unused

// The view depth at continuous slice position s (0 at the near plane, grid.z at the far one), and its inverse.
float da_froxel_depth(float s)
{
	const float n = s / da_froxel_grid.z;
	return da_froxel_range.x + n * n * da_froxel_range.y;
}

float da_froxel_slice(float z)
{
	return sqrt(saturate((z - da_froxel_range.x) * da_froxel_range.z)) * da_froxel_grid.z;
}

// The view-space point at screen uv and view depth z.
float3 da_froxel_view(float2 uv, float z)
{
	return float3((uv.x * 2.0f - 1.0f) * da_froxel_ray.x, (1.0f - uv.y * 2.0f) * da_froxel_ray.y, 1.0f) * z;
}

#ifndef DA_FROXEL_NO_SAMPLE
// What the air scattered toward the eye along the view ray at uv up to view depth z (0: the sky, the whole grid):
// rgb the light, a the transmittance of the air between.
Texture3D s_froxel_int;

float4 da_froxel_sample(float2 uv, float z)
{
	const float s = z > 0.001f ? da_froxel_slice(z) : da_froxel_grid.z;
	// Texel i holds the integral to the far edge of slice i; inside the first slice it grows from nothing.
	float4 f = s_froxel_int.SampleLevel(smp_rtlinear, float3(uv, (s - 0.5f) / da_froxel_grid.z), 0);
	const float near = saturate(s);
	f.rgb *= near;
	f.a = lerp(1.0f, f.a, near);
	return f;
}
#endif

#endif
