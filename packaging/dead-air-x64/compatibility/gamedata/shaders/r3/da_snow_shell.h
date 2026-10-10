// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The snow shells' interpolants (da_snow_shell.vs/.ps): one struct for both stages, so the signatures link the
// same way under FXC and DXC.
#ifndef DA_SNOW_SHELL_H
#define DA_SNOW_SHELL_H

struct da_sn_shell_v2p
{
	float3 Nw : TEXCOORD0;    // world normal of the snow surface
	float3 Pw : TEXCOORD1;    // world position
	float3 Pv : TEXCOORD2;    // view position
	float4 hcur : TEXCOORD3;  // unjittered clip position now
	float4 hprev : TEXCOORD4; // unjittered clip position on the previous frame
	float3 Nf : TEXCOORD5;    // the supporting face's normal
	float2 lift : TEXCOORD6;  // x height over the face (m), y metres to the nearest open edge
	// (centroid and no perspective: the pixel shader writes a conservative depth, the prints on a roof)
	noperspective centroid float4 hpos : SV_Position;
	float clip : SV_ClipDistance0; // < 0: no snow on the face (or past the reach)
};

#endif // DA_SNOW_SHELL_H
