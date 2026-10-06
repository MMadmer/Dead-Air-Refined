// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Blade grass, the ground (docs/dead-air/GRASS.md): a rectangle of the camera's toroidal window of grass
// ground, 0.25 m texels, rasterised from the level's collision mesh with exactly the stock decompress
// rule - the highest up-facing, non-passable surface under the slot's top (y_base + y_height), nothing
// below its base, nothing where the slot holds no blade grass. One thread per texel, the triangles of
// the texel's 4 m cell only (the cells are bucketed on the CPU at level load).

#include "da_grass_common.h"

ByteAddressBuffer	da_grass_slots	: register(t0);	// level.details slots, 16 bytes each
ByteAddressBuffer	da_gg_verts		: register(t2);	// collision vertices, float3
ByteAddressBuffer	da_gg_tris		: register(t3);	// collision triangles: i0, i1, i2, flags (bit 16 passable)
ByteAddressBuffer	da_gg_cells		: register(t4);	// per cell: first reference, count
ByteAddressBuffer	da_gg_refs		: register(t5);	// triangle index per reference
RWTexture2D<float>	da_gg_out		: register(u0);

uniform float4	da_gg_rect;		// xy first world texel of the rectangle, zw its size (texels)
uniform float4	da_gg_win;		// x texel (m), y window size (texels), z cell size (m)
uniform float4	da_gg_grid;		// xy origin of the cell grid (m), zw cells along x, z
uniform float4	da_grass_slot;	// x offs_x, y offs_z, z size_x, w size_z

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= uint(da_gg_rect.z) || id.y >= uint(da_gg_rect.w))
		return;
	const int2 wt = int2(da_gg_rect.xy) + int2(id.xy);
	const float2 p = (float2(wt) + 0.5f) * da_gg_win.x;
	float result = -1e30f;

	const int2 sxz = int2(floor(p * 0.5f)) + int2(da_grass_slot.xy);
	[branch] if (all(sxz >= 0) && sxz.x < int(da_grass_slot.z) && sxz.y < int(da_grass_slot.w))
	{
		const uint4 S = da_grass_slots.Load4((uint(sxz.y) * uint(da_grass_slot.z) + uint(sxz.x)) * 16u);
		const bool blades = grass_id_community((S.x >> 20) & 63u) != 0u || grass_id_community((S.x >> 26) & 63u) != 0u
			|| grass_id_community(S.y & 63u) != 0u || grass_id_community((S.y >> 6) & 63u) != 0u;
		const int2 c = int2(floor((p - da_gg_grid.xy) / da_gg_win.z));
		[branch] if (blades && all(c >= 0) && c.x < int(da_gg_grid.z) && c.y < int(da_gg_grid.w))
		{
			// The slot box of cache_Task, grown by EPS_L like it.
			const float base = float(S.x & 0xFFFu) * 0.2f - 200.0f;
			const float top = base + float((S.x >> 12) & 0xFFu) * 0.1f + 0.001f;
			const uint2 cell = da_gg_cells.Load2((uint(c.y) * uint(da_gg_grid.z) + uint(c.x)) * 8u);
			float best = -1e30f;
			for (uint k = 0; k < cell.y; ++k)
			{
				const uint tri = da_gg_refs.Load((cell.x + k) * 4u);
				const uint4 T = da_gg_tris.Load4(tri * 16u);
				if ((T.w & (1u << 16)) != 0u)
					continue;
				const float3 v0 = asfloat(da_gg_verts.Load3(T.x * 12u));
				const float3 e1 = asfloat(da_gg_verts.Load3(T.y * 12u)) - v0;
				const float3 e2 = asfloat(da_gg_verts.Load3(T.z * 12u)) - v0;
				// The culled ray test of the stock rule: only triangles facing up are hit.
				const float ny = e1.z * e2.x - e1.x * e2.z;
				if (ny <= 1e-5f)
					continue;
				const float2 dp = p - v0.xz;
				const float u = (e2.x * dp.y - dp.x * e2.z) / ny;
				const float v = (e1.z * dp.x - e1.x * dp.y) / ny;
				if (u < 0.0f || v < 0.0f || u + v > 1.0f)
					continue;
				const float y = v0.y + u * e1.y + v * e2.y;
				if (y <= top && y > best)
					best = y;
			}
			if (best >= base - 0.001f)
				result = best;
		}
	}
	const int size = int(da_gg_win.y);
	da_gg_out[uint2((wt % size + size) % size)] = result;
}
