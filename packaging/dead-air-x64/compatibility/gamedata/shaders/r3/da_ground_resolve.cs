// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The near ground window's products from its raster (da_ground.cpp), one 16 x 16 tile per group:
//   Htop     the topmost non-passable surface (world y)
//   Hg       Htop opened with a 3 x 3 square (erode, then dilate): posts, rails, wires and thin props drop
//            out, so the snow passes under them and they stick out of it. Under a water surface Hg is that
//            surface: the snow grid crosses frozen water on it, and the shore is no step
//   base     the surface the ground's snow lies on. Hg, except where Hg is something standing over the
//            ground - a roof, a deck, a vehicle, a block: more than 0.35 m over the lowest floor of the
//            texel (the low map) and more than 0.6 m over the level's terrain (the bake's ground), and no
//            node of the level's AI walks on it (a bridge's deck, a platform: ground, with its trails). There
//            the base is that floor: the ground's snow runs on under the eaves and round a bus or a wall
//            up to its sides, never up a roof (the snow shells lie on top of those, da_snow_shell.vs)
//   shelter  how much of the sky's snow such a cover keeps off the ground under it, from the share of the
//            5 x 5 texels round it under a cover: 1 deep under a roof (no snow on its floor, not along its
//            walls either), a part under an eave or the edge of a vehicle (the wind blows some under), none a
//            texel out from the footprint: a soft drip line, no moat
//   taper    0 on a cliff texel (a jump of the base or no surface), 1 at half a metre from the nearest
//            one: the snow grid thins to nothing at a bank instead of hanging a curtain. Under a water
//            surface the texel stores -1 - taper (the snow skips the lake bed under it)
//   shore    the continuous quantities of the waterline: the opened solid surface under the water too (the
//            bed), the highest water surface within DA_GR_AP texels and how far inside that reach the texel
//            lies (1 up to half of it, 0 at its end). The snow passes cut a frozen pond's sheet where the
//            bilinear ground crosses that level (da_sn_shore), not along the water texels' squares.
// Texels outside the window are read as "nothing here": the toroidal map holds another place there.

Texture2D<uint> s_gr_solid;
Texture2D<uint> s_gr_water;
Texture2D<uint> s_gr_low;
// The level's terrain at 1 m (da_snow.cpp, R16_UNORM: y = da_gr_lvl.x + v * da_gr_lvl.y, 0 = none).
Texture2D<float> s_gr_ground;
// The highest node of the level's AI in each of those texels, the same encoding (0 = none).
Texture2D<float> s_gr_walk;
// The bake's covers at 1 m (da_snow.cpp, dilated by a texel): 1 an overhang (only those shelter the ground under
// them), 0.5 a solid object, 0 none.
Texture2D<float> s_gr_overhang;

RWTexture2D<float> u_top : register(u0);
RWTexture2D<float4> u_base : register(u1);
RWTexture2D<float4> u_shore : register(u2);

uniform float4 da_gr_rect;  // world texels x0, z0, x1, z1 (exclusive) to write
uniform float4 da_gr_win;   // x texel (m), y texels per metre, z texels per side, w taper radius (texels, 1..2)
uniform float4 da_gr_valid; // world texels x0, z0, x1, z1 (exclusive) the window holds
uniform float4 da_gr_level; // the terrain texture: texel = xz * xy + zw (texel centres at +0.5)
uniform float4 da_gr_lvl;   // x lowest y, y range (m), z 1 = the terrain texture is there, w unused

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
// A cover: this far over the texel's lowest floor and over the terrain (da_snow_field.h, da_snow_cover_over).
#define DA_GR_OVER_FLOOR 0.35
#define DA_GR_OVER_TERRAIN 0.35

groupshared float gs_top[DA_GR_S0 * DA_GR_S0];
groupshared float gs_wat[DA_GR_S0 * DA_GR_S0];
groupshared float gs_ero[DA_GR_S1 * DA_GR_S1];
groupshared float gs_hg[DA_GR_S2 * DA_GR_S2];
groupshared float gs_base[DA_GR_S2 * DA_GR_S2];
groupshared float gs_cover[DA_GR_S2 * DA_GR_S2];
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

// The level's terrain at a window texel, bilinear (DA_GR_NONE: none there, or no terrain texture).
float da_gr_terrain(int2 t)
{
	[branch] if (da_gr_lvl.z < 0.5)
		return DA_GR_NONE;
	const float2 xz = (float2(t) + 0.5) * da_gr_win.x;
	const float2 p = xz * da_gr_level.xy + da_gr_level.zw - 0.5;
	const float2 i = floor(p);
	const float2 f = p - i;
	const int2 q = int2(i);
	const float a = s_gr_ground.Load(int3(q, 0)), b = s_gr_ground.Load(int3(q + int2(1, 0), 0));
	const float c = s_gr_ground.Load(int3(q + int2(0, 1), 0)), d = s_gr_ground.Load(int3(q + int2(1, 1), 0));
	[branch] if (min(min(a, b), min(c, d)) <= 0.0)
		return DA_GR_NONE;
	return da_gr_lvl.x + lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) * da_gr_lvl.y;
}

// Does the level's AI walk on a surface at about y round a window texel (its own 1 m texel and the eight round it:
// a deck's nodes stop short of its edge), a node over its metre's ground (da_snow_walkable, da_snow_deck_node)?
bool da_gr_walked(int2 t, float y)
{
	[branch] if (da_gr_lvl.z < 0.5)
		return false;
	const int2 q = int2(floor(((float2(t) + 0.5) * da_gr_win.x) * da_gr_level.xy + da_gr_level.zw));
	bool walked = false;
	[unroll] for (int j = -1; j <= 1; ++j)
		[unroll] for (int i = -1; i <= 1; ++i)
		{
			const float v = s_gr_walk.Load(int3(q + int2(i, j), 0));
			const float w = da_gr_lvl.x + v * da_gr_lvl.y;
			const float gv = s_gr_ground.Load(int3(q + int2(i, j), 0));
			const float g = da_gr_lvl.x + gv * da_gr_lvl.y;
			walked = walked || (v > 0.0 && w > y - 0.25 && w < y + 0.3 && (gv <= 0.0 || w > g + 0.25));
		}
	return walked;
}

// The bake's kind of cover over a window texel: 1 an overhang (a roof, an eave: it shelters the ground under it), 0.5
// a solid object (a block, a car, a stump: the ground's snow runs on at its full depth up to its sides), 0 none.
float da_gr_bake_cover(int2 t)
{
	[branch] if (da_gr_lvl.z < 0.5)
		return 1.0;
	const int2 q = int2(floor(((float2(t) + 0.5) * da_gr_win.x) * da_gr_level.xy + da_gr_level.zw));
	return s_gr_overhang.Load(int3(q, 0));
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

	// dilation: the highest eroded of the 3 x 3, then the water surface over it; a cover over the floor and
	// the terrain hands the base to the floor
	[loop] for (uint k2 = gi; k2 < uint(DA_GR_S2 * DA_GR_S2); k2 += DA_GR_T * DA_GR_T)
	{
		const int2 c = int2(k2 % DA_GR_S2, k2 / DA_GR_S2) + 1;
		float m = DA_GR_NONE, lo_e = -DA_GR_NONE;
		[unroll] for (int j = -1; j <= 1; ++j)
			[unroll] for (int i = -1; i <= 1; ++i)
			{
				const float e = gs_ero[(c.y + j) * DA_GR_S1 + c.x + i];
				m = max(m, e);
				lo_e = min(lo_e, e);
			}
		const float hg = da_gr_surface(m, gs_wat[(c.y + 1) * DA_GR_S0 + c.x + 1]);
		gs_hg[k2] = hg;
		const int2 t = org + c + 1;
		const uint lw = da_gr_inside(t) ? s_gr_low.Load(int3(da_gr_addr(t), 0)) : 0u;
		const float low = (lw == 0u) ? DA_GR_NONE : da_ground_unord(~lw);
		const bool has_floor = low > DA_GR_NONE * 0.5 && m - low > DA_GR_OVER_FLOOR;
		bool cover = false;
		float under = hg;
		const float bake = da_gr_bake_cover(t);
		float terrain_t = DA_GR_NONE;
		[branch] if (hg == m && m > DA_GR_NONE * 0.5)
		{
			const float terrain = da_gr_terrain(t);
			terrain_t = terrain;
			[branch] if (terrain > DA_GR_NONE * 0.5)
			{
				// over the terrain: the floor under it, or the terrain where the collision has no floor there; never a
				// raised floor (a house's plinth): the ground's snow runs on under it at the terrain's level, hidden,
				// instead of ramping up into the doorway in white teeth; nor a cellar's (the grid ran down into it
				// along the walls, a moat round the house)
				// (a low block or a stump with no floor in the collision under it is a cover where the bake found one: as
				// the base its top ran the grid up on to it, torn into sheets and teeth at its edges)
				cover = m - terrain > DA_GR_OVER_TERRAIN && (has_floor || m - terrain > 1.0 || bake > 0.25) &&
					!da_gr_walked(t, m);
				// The ground the cover stands on: beside a small one (a stump, a crate, up to some five texels across) the
				// ground round it, as the eroded top reaches it - the bake's metre-wide terrain lay a few decimetres
				// under it there and the snow's surface sank under the visible ground in a bare wedge -, else the
				// terrain.
				// (and a floor known so never more than 5 cm under it: a crate's collision sunk into the ground faced up
				// there, the base followed it down and the visible ground showed through the snow)
				// (ground-like only: by a house's wall the lowest eroded top round a texel was a sill or an eave, and the
				// grid hung from it down the wall in white curtains)
				const bool ring = lo_e > DA_GR_NONE * 0.5 && lo_e < m - DA_GR_OVER_TERRAIN && lo_e < terrain + 0.5;
				const float ref = ring ? max(terrain, lo_e) : terrain;
				under = has_floor ? clamp(low, ref - (ring ? 0.05 : 0.3), ref + 0.1) : ref;
				// A floor level with the ground round the cover is a passage's - under a bridge's arch -, not a cellar's: the
				// floor itself (held to the terrain, the snow's base hung over it and crossed the bridge's face in a band)
				[branch] if (has_floor && lo_e > DA_GR_NONE * 0.5 && abs(low - lo_e) < 0.2)
					under = low;
			}
			else
			{
				cover = has_floor && da_gr_lvl.z < 0.5;
				under = low;
			}
		}
		// 1 an overhang (it shelters), 0.5 a solid object (it only takes the ground's snow under it, hidden). The bake's
		// kind is a metre wide and dilated by one: only a cover standing an overhang's clearance (1.5 m, da_snow_bake.cpp)
		// over the terrain takes it - crates a metre from a house's eave took the eave's and stood in a bare patch.
		// (negative: a cover whose base the terrain clamp holds well over its own floor - a bridge's side, ramping from
		// the ground to the deck: its steps are no cover's footprint the cliff test forgives, or the grid hung a curtain
		// under the arch; nor does it shelter)
		const float kind = (bake > 0.75 && m - terrain_t > 1.5) ? 1.0 : 0.5;
		const bool hanging = cover && has_floor && under > low + DA_GR_OVER_TERRAIN;
		gs_cover[k2] = cover ? (hanging ? -kind : kind) : 0.0;
		gs_base[k2] = cover ? under : hg;
	}
	GroupMemoryBarrierWithGroupSync();

	// cliffs: no surface, or a step to a neighbour above what a 50 degree slope gives over one texel; never
	// into a cover's footprint (a raised floor behind a wall is no bank: the snow outside runs to the wall)
	const float jump = max(0.35, 1.2 * da_gr_win.x);
	[loop] for (uint k3 = gi; k3 < uint(DA_GR_S3 * DA_GR_S3); k3 += DA_GR_T * DA_GR_T)
	{
		const int2 c = int2(k3 % DA_GR_S3, k3 / DA_GR_S3) + 1;
		const int ci = c.y * DA_GR_S2 + c.x;
		const float h = gs_base[ci];
		const bool cov = gs_cover[ci] > 0.25;
		const float hl = (cov || gs_cover[ci - 1] > 0.25) ? h : gs_base[ci - 1];
		const float hr = (cov || gs_cover[ci + 1] > 0.25) ? h : gs_base[ci + 1];
		const float hd = (cov || gs_cover[ci - DA_GR_S2] > 0.25) ? h : gs_base[ci - DA_GR_S2];
		const float hu = (cov || gs_cover[ci + DA_GR_S2] > 0.25) ? h : gs_base[ci + DA_GR_S2];
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
		float shelter = 0.0;
		[unroll] for (int sj = -2; sj <= 2; ++sj)
			[unroll] for (int si = -2; si <= 2; ++si)
				shelter += step(0.75, gs_cover[c2 + sj * DA_GR_S2 + si]);
		shelter = smoothstep(0.35, 0.9, shelter * (1.0 / 25.0));
		// the opened solid surface (the dilation of the erosion, as above)
		float open = DA_GR_NONE;
		[unroll] for (int oj = -1; oj <= 1; ++oj)
			[unroll] for (int oi = -1; oi <= 1; ++oi)
				open = max(open, gs_ero[(c0.y - 1 + oj) * DA_GR_S1 + c0.x - 1 + oi]);
		const float hg = gs_hg[c2];
		const float surface = gs_base[c2];
		const float top = gs_top[c0.y * DA_GR_S0 + c0.x];
		const float water = gs_wat[c0.y * DA_GR_S0 + c0.x];
		const float taper = saturate(dist / 0.5);
		// a water texel: its surface stands above the solid one (da_gr_surface took it)
		const bool wet = water > DA_GR_NONE * 0.5 && hg == water;
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
		u_base[addr] = float4(surface, wet ? -1.0 - taper : taper, shelter, hg);
		u_shore[addr] = float4(open, lvl, reach, 0.0);
	}
}
