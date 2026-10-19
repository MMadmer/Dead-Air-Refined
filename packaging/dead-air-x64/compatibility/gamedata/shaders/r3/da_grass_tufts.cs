// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Blade grass, the tufts' shade on the ground (docs/dead-air/GRASS.md): a rectangle of the camera's toroidal
// window, 6.25 cm texels, of how deep the clump the blades grow at each point shades the ground at its foot - the
// community of the point's slot, the patch's species and its Voronoi clump by the generation's own hashes, the stand's
// presence at the clump's centre by its own palette rule (da_grass_gen.cs main, grass_species_pick, grass_evaluate).
// The terrain (deffer_impl_flat.ps) reads it with one tap; this pass runs only over the strips the window moves onto,
// and over all of it when the season's clumps change.

#include "da_grass_common.h"

ByteAddressBuffer	da_grass_slots	: register(t0);	// level.details slots, 16 bytes each
Texture2D<float4>	s_clumps		: register(t2);	// $user$grass_clumps (da_grass_blades.cpp fill_clumps)
RWTexture2D<unorm float>	da_gt_out	: register(u0);

uniform float4	da_gt_rect;		// xy first world texel of the rectangle, zw its size (texels)
uniform float4	da_gt_win;		// x texel (m), y window size (texels, a power of two)
uniform float4	da_grass_slot;	// x offs_x, y offs_z, z size_x, w size_z

// The blade community the slot under xz grows most of (1-based, 0: none) and the share of its lanes alive there (the
// palette's weight against the community's full-presence alpha, times its cover).
uint	tuft_stand	(float2 xz, out float alive)
{
	alive = 0.0f;
	const int2 sxz = int2(floor(xz * 0.5f)) + int2(da_grass_slot.xy);
	if (any(sxz < 0) || sxz.x >= int(da_grass_slot.z) || sxz.y >= int(da_grass_slot.w))
		return 0u;
	const uint4 S = da_grass_slots.Load4((uint(sxz.y) * uint(da_grass_slot.z) + uint(sxz.x)) * 16u);
	const uint ids[4] = { (S.x >> 20) & 63u, (S.x >> 26) & 63u, S.y & 63u, (S.y >> 6) & 63u };
	const float2 f = frac(xz * 0.5f);
	float total = 0.0f, best = 0.0f;
	uint comm = 0u;
	[unroll] for (uint k = 0; k < 4; ++k)
	{
		const uint pal = ((k < 2u) ? S.z : S.w) >> ((k & 1u) * 16u);
		const float4 c4 = float4(pal & 15u, (pal >> 4) & 15u, (pal >> 8) & 15u, (pal >> 12) & 15u) / 15.0f;
		const uint cs = ids[k] != 63u ? grass_id_community(ids[k]) : 0u;
		const float a = cs != 0u ? lerp(lerp(c4.x, c4.y, f.x), lerp(c4.z, c4.w, f.x), f.y) : 0.0f;
		total += a;
		if (a > best)
		{
			best = a;
			comm = cs;
		}
	}
	if (comm != 0u)
		alive = saturate(total / max(grass_comm(comm - 1u, 0).w, 0.05f)) * grass_comm(comm - 1u, 5).y;
	return comm;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= uint(da_gt_rect.z) || id.y >= uint(da_gt_rect.w))
		return;
	const int2 wt = int2(da_gt_rect.xy) + int2(id.xy);
	const float2 xz = (float2(wt) + 0.5f) * da_gt_win.x;
	const uint2 slot = asuint(wt) & (uint(da_gt_win.y) - 1u);

	float tuft = 0.0f, alive;
	const uint comm = tuft_stand(xz, alive);
	const uint gp = comm - 1u;
	const float4 H0 = comm != 0u ? s_clumps.Load(int3(0, gp, 0)) : float4(0.0f, 0.0f, 0.0f, 0.0f);
	[branch] if (H0.z > 0.5f)
	{
		// The patch's species (grass_species_pick's graminoids).
		const float pr = grass_patch(xz, H0.x, 0x7A7Cu + gp * 0x1F3Du);
		const float4 C1 = s_clumps.Load(int3(1, gp, 0)), C2 = s_clumps.Load(int3(2, gp, 0));
		const float cum[8] = { C1.x, C1.y, C1.z, C1.w, C2.x, C2.y, C2.z, C2.w };
		const uint n = clamp(uint(H0.z + 0.5f), 1u, 8u);
		uint k = n - 1u;
		[unroll] for (int j = 7; j >= 0; --j)
		{
			if (uint(j) < n && pr < cum[j])
				k = uint(j);
		}
		const float4 S = s_clumps.Load(int3(3 + 2 * k, gp, 0));
		const uint p = uint(s_clumps.Load(int3(4 + 2 * k, gp, 0)).x + 0.5f);
		float2 centre;
		float cdist;
		uint cid;
		grass_clump(xz, S.x, p, centre, cdist, cid);
		[branch] if (grass_clump_grows(cid, S.y))
		{
			// A thin stand's tuft has fewer blades, not a smaller foot: the shade goes with the root of the share of its
			// lanes alive at its centre (and out under a tenth, where a tuft is a blade or two).
			float alive_c;
			tuft_stand(centre, alive_c);
			const float x = saturate(cdist / max(S.z, 1e-3f));
			// The patch's species takes the lanes its scattered neighbours leave it.
			tuft = S.w * (1.0f - x * x) * (1.0f - x * x) * (1.0f - 0.5f * H0.y) * sqrt(alive_c) * saturate(alive_c * 10.0f);
		}
	}
	da_gt_out[slot] = tuft;
}
