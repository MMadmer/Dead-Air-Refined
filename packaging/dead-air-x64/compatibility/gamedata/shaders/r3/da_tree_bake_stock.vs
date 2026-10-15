// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The billboards' bake, the stock path's vegetation (da_trees_bake.cpp): a stock tree-format part (FTreeVisual: the
// level's vertices, its own world transform) as it stands, without its sway, for a view along one of its FLOD's facets;
// the texture coordinates of the tree format (uv x the quantum).

#include "common.h"

uniform float4	da_tree_bake_uv;	// x the tree format's texture coordinate quantum (1 / FTreeVisual_quant)

struct v_tree_bake
{
	float4	P		: POSITION;
	float4	Nh		: NORMAL;
	float3	T		: TANGENT;
	float3	B		: BINORMAL;
	int4	tc		: TEXCOORD0;
};

struct v2p_tree_bake_stock
{
	float2	tc		: TEXCOORD0;
	float4	hpos	: SV_Position;
};

v2p_tree_bake_stock	main	(v_tree_bake I)
{
	v2p_tree_bake_stock O;
	O.hpos = mul(m_WVP, float4(I.P.xyz, 1.0f));
	O.tc = float2(I.tc.xy) * da_tree_bake_uv.x;
	return O;
}
