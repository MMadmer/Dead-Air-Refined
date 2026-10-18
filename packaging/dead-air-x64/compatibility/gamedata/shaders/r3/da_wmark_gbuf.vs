#include "common.h"

// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The decals' G-buffer pass (da_wmark_gbuf.ps): the wallmark geometry as the albedo pass draws it (FVF::LIT in world
// space, the engine's shifted view), with the view-space position the pixel shader builds its frame from.
struct vi
{
	float4 P	: POSITION;
	float4 c	: COLOR0;
	float2 tc	: TEXCOORD0;
};

struct vo
{
	float2 tc	: TEXCOORD0;
	float3 pv	: TEXCOORD1;
	float4 hpos	: SV_Position;
};

vo main( vi v )
{
	vo o;
	o.hpos = mul( m_VP, float4( v.P.xyz, 1.0f ) );
	o.pv = mul( m_V, float4( v.P.xyz, 1.0f ) ).xyz;
	o.tc = v.tc;
	return o;
}
