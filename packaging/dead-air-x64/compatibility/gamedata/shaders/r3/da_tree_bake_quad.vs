// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The billboards' bake (da_trees_bake.cpp): the quads of its passes over the staging and the atlases, sixty-four an
// instance (SV_VertexID: four a quad), each a 48-byte record in t0 - its destination's edges in clip space, its
// source's (texels, or uv for a filtered read), four words for the pixel shader.

#include "common.h"
#include "da_tree_bake.h"

ByteAddressBuffer	da_tree_bake_quads	: register(t0);
uniform float4		da_tree_bake_q;	// x the draw's quads, y its first record

v2p_tree_bake_quad	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_tree_bake_quad O;
	const uint q = iid * 64u + vid / 4u, corner = vid & 3u;
	if (q >= uint(da_tree_bake_q.x))
	{
		O.src = float2(0.0f, 0.0f);
		O.info = uint4(0u, 0u, 0u, 0u);
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const uint at = (uint(da_tree_bake_q.y) + q) * 48u;
	const float4 dst = asfloat(da_tree_bake_quads.Load4(at));
	const float4 src = asfloat(da_tree_bake_quads.Load4(at + 16u));
	const float2 k = float2(float(corner & 1u), float(corner >> 1));
	O.hpos = float4(lerp(dst.x, dst.z, k.x), lerp(dst.y, dst.w, k.y), 0.0f, 1.0f);
	O.src = float2(lerp(src.x, src.z, k.x), lerp(src.y, src.w, k.y));
	O.info = da_tree_bake_quads.Load4(at + 32u);
	return O;
}
