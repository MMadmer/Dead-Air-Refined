// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Screen-space reflections of glossy surfaces (r4_rendertarget_phase_ssr.cpp), after DOOM Eternal's computessrtrace:
// what the ray traced glossy reflections (rt/rt_gloss.hlsl) answer from the screen, without ray tracing. Glossy
// pixels march their mirror ray, spread by roughness, against this frame's depth at half resolution and take the
// colour the previous frame showed at the hit (combine_1's linear third target, so the reflection of the opaque
// world lands in the specular term before the tone map, as DOOM reprojects last frame). da_ssr_temporal.ps rebuilds
// the full resolution; combine_1 puts it in place of the sky's cube, premultiplied by its confidence.
#ifndef DA_SSR_H
#define DA_SSR_H

#include "da_material_codes.h"

// x the frame (the block's pixel and the noise), y the history is valid (0/1), zw the trace size.
uniform float4 da_ssr_frame;
// x lowest gloss traced, y gloss of the full weight, z roughness spread, w most steps of a ray.
uniform float4 da_ssr_gloss;
// This frame's view space -> the previous frame's clip space, unjittered.
uniform float4x4 m_ssr_reproj;

// The pixel of a 2x2 block this frame traces: the block's four in turn (DOOM's per-frame sub-pixel offset).
uint2 da_ssr_block_pixel(uint2 block)
{
	const uint f = uint(da_ssr_frame.x) & 3u;
	const uint2 offset = uint2(f & 1u, ((f >> 1) ^ f) & 1u);
	return min(block * 2u + offset, uint2(pos_decompression_params2.xy) - 1u);
}

// A render pixel's view position and normal from the G-buffer's position target alone (sky: z 0).
void da_ssr_surface(uint2 pixel, out float3 P, out float3 N)
{
	const float4 p = s_position.Load(int3(pixel, 0));
#ifdef GBUFFER_OPTIMIZATION
	P = float3(p.z * ((float2(pixel) + 0.5f) * pos_decompression_params.zw - pos_decompression_params.xy), p.z);
	N = gbuf_unpack_normal(p.xy);
#else
	P = p.xyz;
	N = s_normal.Load(int3(pixel, 0)).xyz;
#endif
}

// Where the frame shows a view point: its uv, by the G-buffer's own reconstruction (the jitter folded in).
float2 da_ssr_uv(float3 Pv)
{
	return (Pv.xy / Pv.z + pos_decompression_params.xy) / pos_decompression_params.zw * pos_decompression_params2.zw;
}

// The view position the frame shows at uv with view depth z.
float3 da_ssr_view(float2 uv, float z)
{
	return float3(z * (uv * pos_decompression_params2.xy * pos_decompression_params.zw - pos_decompression_params.xy), z);
}

// Leaves, blades, snow and the hands keep the cube: a sheen is not a mirror, and the HUD moves with the eye.
bool da_ssr_skips(float z, float gloss, float mtl)
{
	return z < 1.1f || gloss < da_ssr_gloss.x || da_is_foliage(mtl) || da_is_snowy(mtl);
}

#endif // DA_SSR_H
