#include "common.h"

// Rain streaks: camera-facing quads built per drop in dxRainRender, in world space (the draw
// sets an identity world transform), carrying the drop colour and its own alpha in COLOR and
// the streak texture's corner in TEXCOORD0.
//
// Everything else this hands on is what the pixel shader needs to fog the streak, light it and
// soften it against the scene - none of which the stock stub_default pair carried.

struct v_rain
{
	float4 P     : POSITION;
	float2 Tex0  : TEXCOORD0;
	float4 Color : COLOR;
};

struct v2p_rain
{
	float2 Tex0     : TEXCOORD0;
	float4 Color    : COLOR;
	float3 wpos     : TEXCOORD1;
	float4 tctexgen : TEXCOORD2;
	float4 HPos     : SV_Position;
	float  fog      : FOG;
};

uniform float4x4 mVPTexgen;

v2p_rain main (v_rain I)
{
	v2p_rain O;

	O.HPos       = mul(m_WVP, I.P);
	O.Tex0       = I.Tex0;
	O.Color      = I.Color.bgra; // swizzle vertex colour
	O.wpos       = I.P.xyz;
	O.tctexgen   = mul(mVPTexgen, I.P);
	O.tctexgen.z = O.HPos.z;
	O.fog        = saturate(calc_fogging(I.P));

	return O;
}
