// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Trees, the arguments of the GPU cut's draws (da_tree_cut.cs): a group a draw turns the words its list took into
// DrawIndexedInstanced arguments - 64 elements an instance, the vertex class's indices - and marks the rest of its last
// instance as the list's tail (all ones: shrub_vertex draws nothing there).

#include "common.h"
#include "da_shrub_common.h"

RWByteAddressBuffer	da_tree_cut_chunks	: register(u0);	// the chunk buffer, the lists past SHRUB_CUT_WORDS
RWByteAddressBuffer	da_tree_cut_counts	: register(u1);
RWByteAddressBuffer	da_tree_cut_args	: register(u2);	// 20 bytes a draw

uniform float4	da_tree_cut_reg[SHRUB_DRAWS];	// x its list's first word, y its room (whole instances), z indices an
												// instance, w the class's first index

[numthreads(SHRUB_CHUNK, 1, 1)]
void	main	(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint d = gid.x;
	const float4 reg = da_tree_cut_reg[d];
	const uint n = min(da_tree_cut_counts.Load(d * 4u), uint(reg.y));
	const uint inst = (n + SHRUB_CHUNK - 1u) / SHRUB_CHUNK;
	if (n + tid.x < inst * SHRUB_CHUNK)
		da_tree_cut_chunks.Store(SHRUB_CUT_WORDS + (uint(reg.x) + n + tid.x) * 4u, 0xFFFFFFFFu);
	if (tid.x == 0u)
	{
		da_tree_cut_args.Store4(d * 20u, uint4(uint(reg.z), inst, uint(reg.w), 0u));
		da_tree_cut_args.Store(d * 20u + 16u, 0u);
	}
}
