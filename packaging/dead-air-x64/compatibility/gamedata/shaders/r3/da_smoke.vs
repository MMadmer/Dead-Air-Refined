#include "common.h"

// The shader fire's smoke puffs (CDaFireEffect::render_smoke): camera-facing quads carrying
// the puff's noise seed, age, the fire's light on it and its radius in the second texcoord.

struct vv
{
	float4 P	: POSITION;
	float4 c	: COLOR0;
	float2 tc	: TEXCOORD0;
	float4 e	: TEXCOORD1;
};

struct v2p
{
	float2 tc		: TEXCOORD0;
	float4 c		: COLOR0;
	float4 e		: TEXCOORD1;
	float3 wpos		: TEXCOORD2;
	float4 tctexgen	: TEXCOORD3;
	float4 hpos		: SV_Position;
	float  fog		: FOG;
};

uniform float4x4 mVPTexgen;

v2p main (vv v)
{
	v2p o;
	o.hpos		= mul(m_WVP, v.P);
	o.tc		= v.tc;
	o.c			= unpack_D3DCOLOR(v.c);
	o.e			= v.e;
	o.wpos		= v.P.xyz;
	o.tctexgen	= mul(mVPTexgen, v.P);
	o.tctexgen.z = o.hpos.z;
	o.fog		= saturate(calc_fogging(v.P));
	return o;
}
