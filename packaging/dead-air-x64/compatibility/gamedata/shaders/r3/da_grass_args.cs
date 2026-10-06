// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Blade grass, the indirect draw arguments (docs/dead-air/GRASS.md; the GoT "compute 2"): one thread per
// bin turns the generation's record count into DrawIndexedInstanced arguments - 32 blades per instance
// (merge-instancing; the index buffer holds 32 blades of each vertex class) - and keeps the count it
// clamped to the bin's window for the vertex shaders, which collapse the leftovers of the last instance.

#include "da_grass_common.h"

RWByteAddressBuffer	da_grass_counts	: register(u0);	// [0, NBINS) records written, [NBINS, 2 NBINS) drawn
RWByteAddressBuffer	da_grass_args	: register(u1);	// 20 bytes per bin

uniform float4	da_grass_bin_cap[4];	// records each bin holds
uniform float4	da_grass_args_n[4];		// indices per instance of each bin's vertex class
uniform float4	da_grass_args_i[4];		// first index of that class

float	grass_c	(float4 v, uint i)	{ return i == 0u ? v.x : (i == 1u ? v.y : (i == 2u ? v.z : v.w)); }

[numthreads(GRASS_NBINS, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const uint b = id.x;
	const uint cap = uint(grass_c(da_grass_bin_cap[b >> 2], b & 3u));
	const uint n = min(da_grass_counts.Load(b * 4u), cap);
	da_grass_counts.Store((GRASS_NBINS + b) * 4u, n);
	const uint per_instance = uint(grass_c(da_grass_args_n[b >> 2], b & 3u));
	da_grass_args.Store4(b * 20u, uint4(per_instance, per_instance != 0u ? (n + 31u) / 32u : 0u,
		uint(grass_c(da_grass_args_i[b >> 2], b & 3u)), 0u));
	da_grass_args.Store(b * 20u + 16u, 0u);
}
