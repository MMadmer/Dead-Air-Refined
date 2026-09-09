#include "common.h"

// Splash crowns: the rain.dm detail model, one instance per landed drop, transformed on the
// CPU into world space and batched (dxRainRender). Same vertex layout as the streaks, but the
// crown wants no screen-space texgen - it stands ON the surface it hit, and a soft depth fade
// would eat the base it stands on.

struct v_rain
{
	float4 P     : POSITION;
	float2 Tex0  : TEXCOORD0;
	float4 Color : COLOR;
};

struct v2p_splash
{
	float2 Tex0  : TEXCOORD0;
	float4 Color : COLOR;
	float3 wpos  : TEXCOORD1;
	float4 HPos  : SV_Position;
	float  fog   : FOG;
};

v2p_splash main (v_rain I)
{
	v2p_splash O;

	O.HPos  = mul(m_WVP, I.P);
	O.Tex0  = I.Tex0;
	O.Color = I.Color.bgra; // swizzle vertex colour
	O.wpos  = I.P.xyz;
	O.fog   = saturate(calc_fogging(I.P));

	return O;
}
