// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The billboards' runtime bake (docs/dead-air/TREES.md, da_trees_bake.cpp): what its passes share - the targets it
// renders (the albedo and its coverage; the normal in the bake camera's view space and the hemisphere light, what the
// stock lod.ps reads from level_lods and level_lods_nm; the foliage, what the trees' own billboard shader da_tree_lod.ps
// writes the material from) and the quads of its passes over the staging and the atlases.
#ifndef DA_TREE_BAKE_H
#define DA_TREE_BAKE_H

struct f_tree_bake
{
	float4	albedo	: SV_Target0;	// rgb the albedo as the G-buffer holds it, a the coverage
	float4	normal	: SV_Target1;	// rgb the normal (x 0.5 + 0.5), a the hemisphere light
	float4	mtl		: SV_Target2;	// r the share of the covered samples that are foliage, g their translucency (below)
};
// (The atlas pair's top level alone: the foliage's atlas is half their size, its levels theirs from the second down.)
struct f_tree_bake_pair
{
	float4	albedo	: SV_Target0;
	float4	normal	: SV_Target1;
};

// A pass's quad (da_tree_bake_quad.vs): where it reads, interpolated over the quad (texels, or uv for a filtered read),
// and its record's four words.
struct v2p_tree_bake_quad
{
	float2	src		: TEXCOORD0;
	nointerpolation uint4	info	: TEXCOORD1;
	float4	hpos	: SV_Position;
};

// A normal stored in a target (x 0.5 + 0.5) and back.
float3	tree_bake_n_in	(float3 c)	{ return c * 2.0f - 1.0f; }
float3	tree_bake_n_out	(float3 n)	{ return n * 0.5f + 0.5f; }

// A sample's foliage from the material its G-buffer shader wrote (m) and a leaf's translucency (tr, the near tiers' own
// looking into the sun): foliage or not, and the translucency over the codes' least, of their span (da_material_codes.h).
float4	tree_bake_mtl_out	(float m, float tr)
{
	const float leaf = da_is_foliage(m) ? 1.0f : 0.0f;
	return float4(leaf, leaf * saturate((tr - DA_FOLIAGE_TR_MIN) / (3.0f * DA_FOLIAGE_TR_STEP)), 0.0f, 0.0f);
}

// sRGB-encoded colours to linear and back (the stock path's gains are linear, as the offline bake's).
float3	tree_bake_lin	(float3 c)
{
	const float3 c0 = max(c, 0.0f);
	return lerp(pow((c0 + 0.055f) / 1.055f, 2.4f), c0 / 12.92f, step(c0, 0.04045f));
}
float3	tree_bake_srgb	(float3 c)
{
	const float3 c0 = saturate(c);
	return lerp(1.055f * pow(c0, 1.0f / 2.4f) - 0.055f, c0 * 12.92f, step(c0, 0.0031308f));
}
#endif // DA_TREE_BAKE_H
