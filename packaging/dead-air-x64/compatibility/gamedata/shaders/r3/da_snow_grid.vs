// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The near snow grid (da_snow.cpp, docs/dead-air/SNOW.md): a camera-centred clipmap, vertex shader only.
// Each instance is a 32 x 32 cell tile of one ring; ring k has twice the spacing of ring k-1 and a hole
// where ring k-1 lies. Odd vertices slide onto even ones over the outer band of a ring (geomorph), so a
// ring's border matches the next ring's vertices exactly.
// A vertex sits on the snow base (the grey-opened topmost surface, the floor under a roof, a deck or a
// vehicle - their own snow is a shell, da_snow_shell.vs; the water surface over water) raised by the
// current snow depth:
//   - only where the snow is at least three depth-buffer steps thick at that distance (D24S8, near
//     0.05 m: dz = z^2 / (0.05 * 2^24)) and at least the thinnest snow drawn as geometry. Over the next
//     such thickness the raise ramps up from nothing; below it the ramp runs on under the base, so the
//     clip line (SV_ClipDistance, the full-screen snow layer draws the rest) falls exactly where the
//     raise is zero and a patch's edge meets the ground without a lip;
//   - tapered to nothing within half a metre of a cliff, and culled across one (SV_CullDistance), so
//     no curtain hangs from a wall, a roof edge or a bank;
//   - over frozen water (da_snow_ice.h, the rule water.ps draws by) the grid is a sheet: the snow on the
//     ice, or the bare ice a few centimetres over the water mesh's bob, and the snow layer shades the ice
//     like the ground. Across the shore the depth blends by the pond's water share, continuous over the
//     bilinear ground against the pond's level (da_sn_pond), so the bank's snow and the sheet are one surface
//     and a shore hollow below the pond's surface is the pond's ice too; the flat shore just above the
//     waterline never lies under the top of that ice (the water mesh's bob); the pixel shader cuts the ice
//     front; open water is thin snow (clipped);
//   - pushed a few depth units toward the camera, so it wins over the ground it meets at the fade;
//   - its normal is the snow base's slope over 2 m: the snow layer adds the depth's own slope.
// The previous position uses the previous depth and the previous camera's fade: exact motion vectors.
// DA_SN_SHADOW (da_snow_grid_shadow.vs): the same surface in the near sun cascade, so a trail's walls and a
// drift's lee cast their shadows.

#include "common.h"
#include "da_velocity.h"
#include "da_snow_common.h"
#include "da_snow_grid.h"

#define DA_SN_MAX_TILES 320
#define DA_SN_MAX_RINGS 6

uniform float4 da_sn_tiles[DA_SN_MAX_TILES]; // xy world position of the tile's first vertex, z spacing (m), w ring
uniform float4 da_sn_rings[DA_SN_MAX_RINGS]; // xy ring centre, z morph start (m, Chebyshev from the centre), w 1 / morph band
uniform float4 da_sn_holes[DA_SN_MAX_RINGS]; // xy hole centre (the finer ring), z hole half-size (0 = no hole)
uniform float4 da_sn_grid;                   // x reach (m), y reach fade (m), z thinnest snow drawn (m), w depth bias (NDC z)

// The raise of the snow surface over the base for depth D: none at dmin, the whole depth from 2 dmin,
// linear in between and continued below dmin (negative), where the vertex is clipped anyway.
float da_sn_raise(float D, float dmin)
{
	return (D < 2.0 * dmin) ? 2.0 * (D - dmin) : D;
}

#ifdef DA_SN_SHADOW
struct da_sn_grid_v2s
{
	float4 hpos : SV_Position;
	float clip : SV_ClipDistance0;
	float2 cull : SV_CullDistance0;
};
#define DA_SN_GRID_OUT da_sn_grid_v2s
#else
#define DA_SN_GRID_OUT da_sn_grid_v2p
#endif

DA_SN_GRID_OUT main(float2 ij : POSITION, uint iid : SV_InstanceID)
{
	const float4 tile = da_sn_tiles[iid];
	const uint ring = uint(tile.w);
	const float s = tile.z;
	float2 x = tile.xy + ij * s;

	const float4 hole = da_sn_holes[ring];
	const float2 dh = abs(x - hole.xy);
	// the hole's border counts as inside (a quarter cell of slack for rounding)
	const float in_hole = (hole.z > 0.0 && max(dh.x, dh.y) <= hole.z + 0.25 * s) ? -1.0 : 1.0;

	const float4 rg = da_sn_rings[ring];
	const float2 dr = abs(x - rg.xy);
	const float morph = saturate((max(dr.x, dr.y) - rg.z) * rg.w + 1e-3);
	x -= (frac(ij * 0.5) * 2.0) * s * morph;

	float3 b0 = da_sn_base(x);
	const float3 bx0 = da_sn_base(x - float2(s, 0.0));
	const float3 bx1 = da_sn_base(x + float2(s, 0.0));
	const float3 bz0 = da_sn_base(x - float2(0.0, s));
	const float3 bz1 = da_sn_base(x + float2(0.0, s));
	const float hx0 = bx0.x, hx1 = bx1.x, hz0 = bz0.x, hz1 = bz1.x;
	// No surface here (outside the collision mesh): the vertex borrows a neighbour's height and is
	// clipped, so the triangles around it end at the valid side instead of reaching down to nowhere.
	const bool none = b0.x < DA_SN_NONE * 0.5;
	[branch] if (none)
	{
		const float hn = max(max(hx0, hx1), max(hz0, hz1));
		b0 = float3((hn > DA_SN_NONE * 0.5) ? hn : da_sn_level_base(x), 0.0, 0.0);
	}
	// The share of water under the vertex. A frozen pond's share is continuous (da_sn_pond: the bilinear ground
	// against the pond's level, the baked field's pond where the window sees no water), so the sheet's edge
	// follows the ground's contour and not the water texels' squares; open water keeps the window's own share
	// (the water mesh draws its line). Where there is any water or a pond's level is in reach: the ice and the
	// snow on it.
	float3 pond = float3(DA_SN_NONE, 0.0, 0.0);
	[branch] if (!none && da_snow_ice.x > 0.02)
		pond = da_sn_pond(x, b0);
	float2 ice = float2(0.0, 0.0);
	[branch] if (!none && (max(b0.z, pond.y) > 0.0 || pond.x > DA_SN_NONE * 0.5))
		ice = da_sn_ice_at(x);
	const bool frozen = ice.x > 0.5;
	const float psh = frozen ? pond.y : 0.0;
	const float wsh = none ? 0.0 : (frozen ? psh : saturate(b0.z));
	const float hb = lerp(b0.x, max(b0.x, pond.x), psh);
	// The neighbours as the snow lies on them: never under the level of the frozen pond beside them.
	const float lvl = (da_snow_ice.x > 0.5) ? pond.x : DA_SN_NONE;
	const float4 hr = float4(hx0, hx1, hz0, hz1);
	const float4 hn = (hr > DA_SN_NONE * 0.5) ? max(hr, lvl) : hr;
	const float2 slope = float2(hn.y - hn.x, hn.w - hn.z) / (2.0 * s);
	// Mostly water: flat, never a cliff. A bank is no cliff toward the water below it either, up to a metre:
	// its snow runs down the bank to the ice instead of the sheet ending on a row of grid cells.
	const float4 wn = float4(bx0.z, bx1.z, bz0.z, bz1.z);
	const float4 jn = abs(hn - hb);
	const float4 jl = (wn > 0.5 && jn < 1.0) ? 0.0 : jn;
	const bool shore = max(max(wn.x, wn.y), max(wn.z, wn.w)) > 0.5 || lvl > DA_SN_NONE * 0.5;
	// Inside the window a step cuts only where its resolve sees a cliff within half a metre (taper under 1): the base
	// steps across a cover's footprint by design, and cut there a crate's or a stump's edge tore the grid's triangles
	// into a ragged hole beside it. (The taper alone thins the snow, not the base: trusted alone, a wall's raised
	// texels hung the grid from the eaves in curtains.) Past the window the level base knows no taper.
	const float2 wd = abs(x - da_sn_win.xy);
	const bool in_window = max(wd.x, wd.y) < da_sn_win.z - da_sn_win.w;
	const bool steep = max(max(jl.x, jl.y), max(jl.z, jl.w)) > max(0.35, 1.2 * s) || (!shore && dot(slope, slope) > 3.0);
	const bool cliff = none || (wsh < 0.5 && steep && (!in_window || b0.y < 0.999));

	const float2 D = da_snow_depth2(x);

	const float3 P0 = float3(x.x, hb, x.y);
#ifdef DA_SN_SHADOW
	// the main view's depth steps at the vertex: the cascade keeps exactly what the G-buffer draws
	const float4 h0 = float4(0.0, 0.0, 0.0, length(P0 - eye_position));
	const float4 h0p = h0;
#else
	const float4 h0 = mul(m_VP, float4(P0, 1.0));
	const float4 h0p = mul(m_VP_prev, float4(P0, 1.0));
#endif
	const float dz = da_sn_dz(max(h0.w, 0.0));
	const float dmin = max(da_sn_grid.z, dz);
	const float dmin_prev = max(da_sn_grid.z, da_sn_dz(max(h0p.w, 0.0)));
	const float reach = saturate((da_sn_grid.x - length(x - eye_position.xz)) / max(da_sn_grid.y, 1e-3));
	// The top of the ice of the frozen pond beside the vertex, and dry ground that may lie lower than it: the flat
	// shore just above the waterline. The water mesh's ice bobs over a snow surface or a ground lying there; in
	// the thaw, with a few centimetres of snow on the bank, it drew a stepped grey band along the grid's triangles
	// under the edge of the bank's snow. The grid keeps that shore (no clip, no per-pixel cut) at least at the
	// top of the ice.
	const float ice_top = (frozen && pond.x > DA_SN_NONE * 0.5) ? da_sn_ice_top(pond.x, h0.w) : DA_SN_NONE;
	const bool low_bank = !none && wsh <= 0.0 && hb < ice_top + da_sn_bank_slack(slope);

	// On land: the season's snow over the base. A cliff vertex (and one with no surface) clips every
	// triangle that touches it down to a sliver: the cull below only drops triangles with three cliff
	// corners, and one with a single corner on a roof, a wall top or the edge of the collision would hang
	// a spike or a sheet from it.
	const float taper = b0.y;
	const float dl = da_sn_raise(D.x, dmin) * reach * taper;
	const float dl_prev = da_sn_raise(D.y, dmin_prev) * reach * taper;
	const float clip_l = min(D.x - dmin, reach - 0.001);
	// On the water: frozen, the snow on the ice, never under the bob of the water mesh and the depth steps
	// it needs to win; open, under the surface and clipped, so the bank's snow runs down into the water.
	const float dw = frozen ? max(ice.y, DA_SN_ICE_LIFT + dz) : -2.0 * dmin;
	const float clip_w = frozen ? reach - 0.001 : -0.01;
	// One surface across the shore: base and depth blend by the water share, so the bank's snow meets the
	// sheet on the ice along the waterline without a step, whatever the bank does at the grid's cells.
	// Beside frozen water the bank keeps its triangles, its snow never below the ground (no raise under dmin):
	// the pixel shader ends the sheet along the bank's own contour where the bank's snow is thin. Clipped per
	// vertex, the sheet ended on straight cuts through the grid's cells, a row of teeth along the shore, and the
	// bank's sunken vertices pulled the sheet under the water mesh's ice in a dark band.
	const bool ice_shore = !none && !cliff && da_snow_ice.x > 0.02 && (shore || wsh > 0.0 || low_bank);
	const float d = lerp(ice_shore ? max(dl, 0.0) : dl, dw, wsh);
	const float d_prev = lerp(ice_shore ? max(dl_prev, 0.0) : dl_prev, dw, wsh);
	float clip = cliff ? -1000.0 : lerp(clip_l, clip_w, wsh);
	[flatten] if (ice_shore && (frozen || wsh <= 0.0))
		clip = reach - 0.001;
	// A vertex at the water's edge or on the flat shore beside it never sinks under the ice of the pond.
	const float y_floor = (wsh > 0.0 || low_bank) ? ice_top : DA_SN_NONE;
	// The normal the grid writes is the snow base's slope over 2 m, as the snow layer takes it for any
	// pixel (the collision's facets show no bands of light); the layer adds the depth's own slope at 6 cm
	// and reads this one back instead of four base lookups per pixel. Finer rings take the slope at +-1 m,
	// coarser ones over their own spacing; a tap off the collision keeps the cells' slope.
	float2 gb = slope;
	[branch] if (s < 1.0 && !none)
	{
		const float bx0f = da_sn_base(x - float2(1.0, 0.0)).x, bx1f = da_sn_base(x + float2(1.0, 0.0)).x;
		const float bz0f = da_sn_base(x - float2(0.0, 1.0)).x, bz1f = da_sn_base(x + float2(0.0, 1.0)).x;
		[branch] if (min(min(bx0f, bx1f), min(bz0f, bz1f)) > DA_SN_NONE * 0.5)
			gb = float2(da_sn_edge_slope(max(bx0f, lvl), hb, max(bx1f, lvl), 1.0), da_sn_edge_slope(max(bz0f, lvl), hb, max(bz1f, lvl), 1.0));
	}
	const float3 Nw = normalize(lerp(float3(-gb.x, 1.0, -gb.y), float3(0.0, 1.0, 0.0), wsh * wsh));

	const float3 P = float3(x.x, max(hb + d, y_floor), x.y);
#ifdef DA_SN_SHADOW
	da_sn_grid_v2s so;
	so.hpos = mul(m_VP, float4(P, 1.0));
	so.clip = clip;
	so.cull = float2(cliff ? -1.0 : 1.0, in_hole);
	return so;
#else
	da_sn_grid_v2p o;
	o.Nw = Nw;
	o.Pw = P;
	o.Pv = mul(m_V, float4(P, 1.0));
	// a bank vertex beside frozen water asks the pixel shader for its per-pixel cut, like a wet one
	o.wet = ice_shore ? 1.0 : wsh;
	o.hpos = mul(m_VP, float4(P, 1.0));
	o.hpos.z -= da_sn_grid.w * o.hpos.w;
	o.hcur = mul(m_VP_nojit, float4(P, 1.0));
	o.hprev = mul(m_VP_prev, float4(x.x, max(hb + d_prev, y_floor), x.y, 1.0));
	o.clip = clip;
	o.cull = float2(cliff ? -1.0 : 1.0, in_hole);
	return o;
#endif
}
