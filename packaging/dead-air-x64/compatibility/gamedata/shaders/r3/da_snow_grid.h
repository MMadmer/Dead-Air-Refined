// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The snow grid's interpolants (da_snow_grid.vs/.ps). One struct for both stages, so the signatures link
// the same way under FXC and DXC.
#ifndef DA_SNOW_GRID_H
#define DA_SNOW_GRID_H

// The water mesh bobs by 1/30 m (watermove.h): bare ice stands above that.
#define DA_SN_ICE_LIFT 0.04

// Three depth-buffer steps at view depth z (D24S8, near plane 0.05 m): the thinnest separation that wins.
float da_sn_dz(float view_z)
{
	return view_z * view_z * (3.0 / (0.05 * 16777216.0));
}

// The lowest the snow surface may lie beside a frozen pond of this level, seen at view depth z: over the
// water mesh's bob by the depth steps it needs to win. Anything lower (the sheet, the bank's foot and the
// flat shore just above the waterline) is drawn over by the water mesh's own ice as it bobs.
float da_sn_ice_top(float level, float view_z)
{
	return level + DA_SN_ICE_LIFT + da_sn_dz(max(view_z, 0.0));
}

// How far over the top of the ice a bank's snow base may stand while the ground itself is still under it: the
// base is a conservative raster (each window texel holds the highest point of the collision over it, half a
// texel's rise over the surface on a slope of gradient g), and the terrain's render mesh lies a little off its
// collision. The grid keeps the shore up to there.
float da_sn_bank_slack(float2 g)
{
	return 0.5 / max(da_sn_win2.y, 1.0) * (abs(g.x) + abs(g.y)) + 0.02;
}

struct da_sn_grid_v2p
{
	float3 Nw : TEXCOORD0;    // world normal of the snow surface
	float3 Pw : TEXCOORD1;    // world position
	float3 Pv : TEXCOORD2;    // view position
	float wet : TEXCOORD3;    // water share at the vertex, 1 on a bank beside frozen water (da_snow_grid.ps cuts per pixel there)
	float4 hcur : TEXCOORD4;  // unjittered clip position now
	float4 hprev : TEXCOORD5; // unjittered clip position on the previous frame
	float4 hpos : SV_Position;
	float clip : SV_ClipDistance0;  // < 0: the snow is too thin for geometry here (the layer draws it)
	float2 cull : SV_CullDistance0; // x < 0 at all three corners: a cliff; y: inside the finer level
};

#endif // DA_SNOW_GRID_H
