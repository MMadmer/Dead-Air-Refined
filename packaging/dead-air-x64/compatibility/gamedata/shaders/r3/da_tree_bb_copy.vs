// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Trees, the billboard atlas's fill (docs/dead-air/TREES.md, da_trees.cpp pre_render): a template's eight cells copied
// from its stock kind's cells in the level's own billboard atlas (level_lods and its _nm), until the bake renders them
// from the tree itself. One quad a cell (64 a draw instance): its destination's edges in clip space and its source's
// uv edges, eight floats in t0.

#include "common.h"

ByteAddressBuffer	da_tree_bb_cells	: register(t0);
uniform float4		da_tree_bb0;	// x the cells

struct v2p_bb_copy
{
	float2	uv		: TEXCOORD0;
	float4	hpos	: SV_Position;
};

v2p_bb_copy	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_bb_copy O;
	const uint cell = iid * 64u + vid / 4u, corner = vid & 3u;
	if (cell >= uint(da_tree_bb0.x))
	{
		O.uv = float2(0.0f, 0.0f);
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const float4 dst = asfloat(da_tree_bb_cells.Load4(cell * 32u));
	const float4 src = asfloat(da_tree_bb_cells.Load4(cell * 32u + 16u));
	const float2 k = float2(float(corner & 1u), float(corner >> 1));
	O.hpos = float4(lerp(dst.x, dst.z, k.x), lerp(dst.y, dst.w, k.y), 0.0f, 1.0f);
	O.uv = float2(lerp(src.x, src.z, k.x), lerp(src.y, src.w, k.y));
	return O;
}
