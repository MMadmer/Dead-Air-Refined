// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The near ground window's products from its raster (da_ground.cpp), one 16 x 16 tile per group:
//   Htop  the topmost non-passable surface (world y)
//   Hg    Htop opened with a 3 x 3 square (erode, then dilate): posts, rails and thin props drop out, so
//         the snow passes under them and they stick out of it; roofs, walls and banks stay. Under a water
//         surface Hg is that surface: the snow grid crosses frozen water on it, and the shore is no step
//   taper 0 on a cliff texel (a jump of the surface or no surface), 1 at half a metre from the nearest
//         one: the snow grid thins to nothing at walls instead of hanging a curtain. Under a water
//         surface the texel stores -1 - taper (the snow skips the lake bed under it)
//   shore the continuous quantities of the waterline: the opened solid surface under the water too (the
//         bed), the highest water surface within DA_GR_AP texels and how far inside that reach the texel
//         lies (1 up to half of it, 0 at its end). The snow passes cut a frozen pond's sheet where the
//         bilinear ground crosses that level (da_sn_shore), not along the water texels' squares.
// Texels outside the window are read as "nothing here": the toroidal map holds another place there.

Texture2D<uint> s_gr_solid;
Texture2D<uint> s_gr_water;

RWTexture2D<float> u_top : register(u0);
RWTexture2D<float2> u_base : register(u1);
RWTexture2D<float4> u_shore : register(u2);

uniform float4 da_gr_rect;  // world texels x0, z0, x1, z1 (exclusive) to write
uniform float4 da_gr_win;   // x texel (m), y texels per metre, z texels per side, w taper radius (texels, 1..2)
uniform float4 da_gr_valid; // world texels x0, z0, x1, z1 (exclusive) the window holds

#define DA_GR_NONE (-10000.0)
#define DA_GR_T 16
// The apron, and the reach of the water level (da_ground.cpp k_apron: a raster change reaches this far).
#define DA_GR_AP 8
#define DA_GR_S0 (DA_GR_T + 2 * DA_GR_AP)
#define DA_GR_S1 (DA_GR_S0 - 2)
#define DA_GR_S2 (DA_GR_S1 - 2)
#define DA_GR_S3 (DA_GR_S2 - 2)
// A water surface this far above the opened solid surface makes the texel a water texel.
#define DA_GR_WET 0.05

groupshared float gs_top[DA_GR_S0 * DA_GR_S0];
groupshared float gs_wat[DA_GR_S0 * DA_GR_S0];
groupshared float gs_ero[DA_GR_S1 * DA_GR_S1];
groupshared float gs_hg[DA_GR_S2 * DA_GR_S2];
groupshared float gs_open[DA_GR_S2 * DA_GR_S2];
groupshared float gs_cliff[DA_GR_S3 * DA_GR_S3];
// The water level along each row over the output columns: the highest surface within DA_GR_AP texels, and
// DA_GR_AP minus the distance to the nearest water texel in that row (-1: none).
groupshared float gs_lvl_row[DA_GR_S0 * DA_GR_T];
groupshared float gs_near_row[DA_GR_S0 * DA_GR_T];

float da_ground_unord(uint u)
{
	const float f = asfloat((u & 0x80000000u) ? (u & 0x7FFFFFFFu) : ~u);
	return (u == 0u) ? DA_GR_NONE : f;
}

bool da_gr_inside(int2 t)
{
	return t.x >= int(da_gr_valid.x) && t.y >= int(da_gr_valid.y) && t.x < int(da_gr_valid.z) && t.y < int(da_gr_valid.w);
}

// World texel modulo the window size; every world texel of a level is above -256 n.
int2 da_gr_addr(int2 t)
{
	const int n = int(da_gr_win.z);
	return int2(uint2(t + n * 256) % uint(n));
}

// The surface the snow sees at an opened texel: the water surface where one lies above the opened solid.
float da_gr_surface(float hg, float water)
{
	return (water > DA_GR_NONE * 0.5 && water > hg + DA_GR_WET) ? water : hg;
}

[numthreads(DA_GR_T, DA_GR_T, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	const int2 out0 = int2(da_gr_rect.xy) + int2(gid.xy) * DA_GR_T;
	const int2 org = out0 - DA_GR_AP;

	[loop] for (uint k = gi; k < uint(DA_GR_S0 * DA_GR_S0); k += DA_GR_T * DA_GR_T)
	{
		const int2 t = org + int2(k % DA_GR_S0, k / DA_GR_S0);
		const bool inside = da_gr_inside(t);
		gs_top[k] = inside ? da_ground_unord(s_gr_solid.Load(int3(da_gr_addr(t), 0))) : DA_GR_NONE;
		gs_wat[k] = inside ? da_ground_unord(s_gr_water.Load(int3(da_gr_addr(t), 0))) : DA_GR_NONE;
	}
	GroupMemoryBarrierWithGroupSync();

	// erosion: the lowest of the 3 x 3
	[loop] for (uint k1 = gi; k1 < uint(DA_GR_S1 * DA_GR_S1); k1 += DA_GR_T * DA_GR_T)
	{
		const int2 c = int2(k1 % DA_GR_S1, k1 / DA_GR_S1) + 1;
		float m = gs_top[c.y * DA_GR_S0 + c.x];
		[unroll] for (int j = -1; j <= 1; ++j)
			[unroll] for (int i = -1; i <= 1; ++i)
				m = min(m, gs_top[(c.y + j) * DA_GR_S0 + c.x + i]);
		gs_ero[k1] = m;
	}
	// the water level along the rows (separable square window: rows here, columns at the output)
	[loop] for (uint kr = gi; kr < uint(DA_GR_S0 * DA_GR_T); kr += DA_GR_T * DA_GR_T)
	{
		const int row = int(kr / DA_GR_T);
		const int col = int(kr % DA_GR_T) + DA_GR_AP;
		float lvl = DA_GR_NONE;
		float near = -1.0;
		[loop] for (int i = -DA_GR_AP; i <= DA_GR_AP; ++i)
		{
			const float w = gs_wat[row * DA_GR_S0 + col + i];
			[flatten] if (w > DA_GR_NONE * 0.5)
			{
				lvl = max(lvl, w);
				near = max(near, float(DA_GR_AP - abs(i)));
			}
		}
		gs_lvl_row[kr] = lvl;
		gs_near_row[kr] = near;
	}
	GroupMemoryBarrierWithGroupSync();

	// dilation: the highest eroded of the 3 x 3, then the water surface over it
	[loop] for (uint k2 = gi; k2 < uint(DA_GR_S2 * DA_GR_S2); k2 += DA_GR_T * DA_GR_T)
	{
		const int2 c = int2(k2 % DA_GR_S2, k2 / DA_GR_S2) + 1;
		float m = DA_GR_NONE;
		[unroll] for (int j = -1; j <= 1; ++j)
			[unroll] for (int i = -1; i <= 1; ++i)
				m = max(m, gs_ero[(c.y + j) * DA_GR_S1 + c.x + i]);
		gs_open[k2] = m;
		gs_hg[k2] = da_gr_surface(m, gs_wat[(c.y + 1) * DA_GR_S0 + c.x + 1]);
	}
	GroupMemoryBarrierWithGroupSync();

	// cliffs: no surface, or a step to a neighbour above what a 50 degree slope gives over one texel
	const float jump = max(0.35, 1.2 * da_gr_win.x);
	[loop] for (uint k3 = gi; k3 < uint(DA_GR_S3 * DA_GR_S3); k3 += DA_GR_T * DA_GR_T)
	{
		const int2 c = int2(k3 % DA_GR_S3, k3 / DA_GR_S3) + 1;
		const float h = gs_hg[c.y * DA_GR_S2 + c.x];
		const float hl = gs_hg[c.y * DA_GR_S2 + c.x - 1];
		const float hr = gs_hg[c.y * DA_GR_S2 + c.x + 1];
		const float hd = gs_hg[(c.y - 1) * DA_GR_S2 + c.x];
		const float hu = gs_hg[(c.y + 1) * DA_GR_S2 + c.x];
		const float lo = min(min(hl, hr), min(hd, hu));
		const bool none = min(h, lo) < DA_GR_NONE * 0.5;
		const float rise = max(max(abs(hl - h), abs(hr - h)), max(abs(hd - h), abs(hu - h)));
		gs_cliff[k3] = (none || rise > jump) ? 1.0 : 0.0;
	}
	GroupMemoryBarrierWithGroupSync();

	const int2 t = out0 + int2(tid.xy);
	const bool alive = t.x < int(da_gr_rect.z) && t.y < int(da_gr_rect.w) && da_gr_inside(t);
	[branch] if (alive)
	{
		const int2 c3 = int2(tid.xy) + (DA_GR_AP - 3);
		const int radius = int(da_gr_win.w);
		float dist = 1e6;
		[loop] for (int j = -radius; j <= radius; ++j)
		{
			[loop] for (int i = -radius; i <= radius; ++i)
			{
				[branch] if (gs_cliff[(c3.y + j) * DA_GR_S3 + c3.x + i] > 0.5)
					dist = min(dist, length(float2(i, j)) * da_gr_win.x);
			}
		}
		const int2 c0 = int2(tid.xy) + DA_GR_AP;
		const int c2 = (int(tid.y) + DA_GR_AP - 2) * DA_GR_S2 + int(tid.x) + DA_GR_AP - 2;
		const float surface = gs_hg[c2];
		const float top = gs_top[c0.y * DA_GR_S0 + c0.x];
		const float water = gs_wat[c0.y * DA_GR_S0 + c0.x];
		const float taper = saturate(dist / 0.5);
		// a water texel: its surface stands above the solid one (da_gr_surface took it)
		const bool wet = water > DA_GR_NONE * 0.5 && surface == water;
		// the water level within the square of DA_GR_AP texels round the texel, and the Chebyshev distance to
		// the nearest water texel in it: max over the rows of min(row's DA_GR_AP - |dx|, DA_GR_AP - |dz|)
		float lvl = DA_GR_NONE;
		float near = -1.0;
		[loop] for (int dz = -DA_GR_AP; dz <= DA_GR_AP; ++dz)
		{
			const int kr = (c0.y + dz) * DA_GR_T + int(tid.x);
			lvl = max(lvl, gs_lvl_row[kr]);
			near = max(near, min(gs_near_row[kr], float(DA_GR_AP - abs(dz))));
		}
		const float reach = (lvl > DA_GR_NONE * 0.5) ? saturate(near * (2.0 / DA_GR_AP)) : 0.0;
		const int2 addr = da_gr_addr(t);
		u_top[addr] = top;
		u_base[addr] = float2(surface, wet ? -1.0 - taper : taper);
		u_shore[addr] = float4(gs_open[c2], lvl, reach, 0.0);
	}
}
