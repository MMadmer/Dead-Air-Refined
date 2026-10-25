#include "common.h"
#include "da_fog_common.h"
#include "da_particle_light.h"

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
	float3 light    : TEXCOORD3;	// the light the drop takes (da_particle_light.h)
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
	// Lit like the particles; a drop and a flake are lenses, with a forward lobe on the High rung of the rain's ladder.
	O.light      = 1.0f;
	[branch] if (da_particle_light.x > 0.5f)
		O.light = da_particle_drop_light(I.P.xyz, normalize(I.P.xyz - eye_position), da_rain2.z >= 2.5f ? 3.0f : 0.0f);

	return O;
}
