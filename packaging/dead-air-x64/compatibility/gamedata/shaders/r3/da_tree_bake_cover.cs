// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The billboards' bake (da_trees_bake.cpp): each cell's alpha factor of each level, so the billboards' alpha test at
// the preset's reference (lod.ps: clip(alpha - def_aref)) keeps the cell's coverage at every level of its mips
// (Castano's coverage-preserving alpha, as the offline bake's coverage-keeping mips): the share of the level's texels
// that must pass is the cell's - a tree's mean coverage, a stock-path cell's texels passing the reference at level 0
// (its level 0 as calibrated) -, the factor the reference over the alpha of the texel at that rank (a histogram of the
// level's alphas). One group a cell and level.

#include "common.h"

Texture2D<float4>	da_tree_bake_stage	: register(t0);	// the staging's albedo and coverage, every level
ByteAddressBuffer	da_tree_bake_cells	: register(t1);	// a cell: x, y, width, height (the staging's texels at level 0)
RWByteAddressBuffer	da_tree_bake_k		: register(u0);	// the factors, a cell's levels one after another
uniform float4		da_tree_bake_c;	// x the alpha reference, y the levels a cell has, z mode (0 a tree, 1 a stock-path cell)

groupshared uint	g_hist[256];
groupshared uint	g_kept;

[numthreads(256, 1, 1)]
void	main	(uint3 gid : SV_GroupID, uint ti : SV_GroupIndex)
{
	const uint level = gid.x, cell = gid.y, levels = uint(da_tree_bake_c.y + 0.5f);
	const uint4 r = da_tree_bake_cells.Load4(cell * 16u);
	const float aref = da_tree_bake_c.x;
	const bool stock = da_tree_bake_c.z > 0.5f;
	g_hist[ti] = 0u;
	if (ti == 0u)
		g_kept = 0u;
	GroupMemoryBarrierWithGroupSync();
	// The coverage to keep (level 0): a tree's mean (in 1/1024 a texel), a stock-path cell's texels passing.
	const uint n0 = r.z * r.w;
	uint acc = 0u;
	for (uint i = ti; i < n0; i += 256u)
	{
		const float a = da_tree_bake_stage.Load(int3(int(r.x + i % r.z), int(r.y + i / r.z), 0)).a;
		acc += stock ? (a >= aref ? 1u : 0u) : uint(a * 1024.0f + 0.5f);
	}
	InterlockedAdd(g_kept, acc);
	// This level's alphas (the texels the cell touches).
	const uint x = r.x >> level, y = r.y >> level;
	const uint w = max((r.z + (1u << level) - 1u) >> level, 1u), h = max((r.w + (1u << level) - 1u) >> level, 1u);
	for (uint j = ti; j < w * h; j += 256u)
	{
		const float a = da_tree_bake_stage.Load(int3(int(x + j % w), int(y + j / w), int(level))).a;
		InterlockedAdd(g_hist[min(uint(a * 255.0f + 0.5f), 255u)], 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	if (ti != 0u)
		return;
	const float share = float(g_kept) / ((stock ? 1.0f : 1024.0f) * float(max(n0, 1u)));
	const uint target = uint(share * float(w * h) + 0.5f);
	float k = 0.0f;
	if (stock && level == 0u)
		k = 1.0f;
	else if (target > 0u)
	{
		// The lowest bin, from the top, that holds the target: every texel from it up passes once scaled.
		uint sum = 0u, b = 255u;
		[loop] for (; b > 0u; --b)
		{
			sum += g_hist[b];
			if (sum >= target)
				break;
		}
		k = min(aref / max((float(b) - 0.5f) / 255.0f, 0.5f / 255.0f), 255.0f);
	}
	da_tree_bake_k.Store((cell * levels + level) * 4u, asuint(k));
}
