// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Blade grass (docs/dead-air/GRASS.md): the hashes, the clonal patches and the clumps the generation places its lanes
// by. Bound to no resource, so the tufts' window of the terrain (da_grass_tufts.cs) finds the same tufts the blades
// grow in.
#ifndef DA_GRASS_HASH_H
#define DA_GRASS_HASH_H

uint	grass_hash	(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
float	grass_u01	(uint x)	{ return float(x >> 8) * (1.0f / 16777216.0f); }
uint	grass_seed	(int2 g, uint salt)	{ return grass_hash(asuint(g.x) * 0x9E3779B1u ^ grass_hash(asuint(g.y) + salt)); }

// A patch of clonal growth at xz: the cell of a grid one patch size apart under a warp of two incommensurate sine
// waves (most of a cell), so the cells are organic blobs, and the cell's own uniform value (so the shares of the
// patches are exact).
float	grass_patch	(float2 xz, float size, uint salt)
{
	const float2 q = xz / max(size, 0.25f);
	const float ph = float(salt & 1023u) * 0.0061359f;
	const float2 w = float2(sin(q.y * 1.37f + ph) + 0.5f * sin(q.x * 2.71f - ph), sin(q.x * 1.19f + 2.0f * ph) + 0.5f * sin(q.y * 2.33f + ph));
	return grass_u01(grass_hash(grass_seed(int2(floor(q + w * 0.4f)), salt) ^ 0xA7C3u));
}

// The clump of species p at xz (GoT: a Voronoi cell of 3x3 jittered points, cell metres apart): its centre, the
// distance to it and its id.
void	grass_clump	(float2 xz, float cell, uint p, out float2 centre, out float cdist, out uint cid)
{
	cell = max(cell, 0.02f);
	const float2 cq = xz / cell, b = floor(cq);
	float best = 1e9f;
	float2 best_cell = b, best_pt = b;
	[unroll] for (int j = -1; j <= 1; ++j)
	{
		[unroll] for (int i = -1; i <= 1; ++i)
		{
			const float2 cc = b + float2(i, j);
			const uint hc2 = grass_seed(int2(cc), 0x2545F491u + p * 0x3B9Du);
			const float2 pt = cc + float2(grass_u01(hc2), grass_u01(grass_hash(hc2)));
			const float d2 = dot(cq - pt, cq - pt);
			if (d2 < best)
			{
				best = d2;
				best_cell = cc;
				best_pt = pt;
			}
		}
	}
	centre = best_pt * cell;
	cdist = sqrt(best) * cell;
	cid = grass_seed(int2(best_cell), 0x7F4A7C15u + p * 0x3B9Du);
}

// Whether clump cid of a species with this cover grows at all (bare gaps between tufts: sand, cover < 1).
bool	grass_clump_grows	(uint cid, float cover)	{ return grass_u01(grass_hash(cid ^ 0xC0C0u)) < cover; }

#endif // DA_GRASS_HASH_H
