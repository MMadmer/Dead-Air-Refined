#include "common.h"

// The stock sky dome, with the flash of a lightning bolt handled per pixel: the effect adds
// a uniform share of the flash to the sky colour every frame (thunderbolt.ltx sky_color),
// which brightened the whole dome at once. That share comes back out here and sky2.ps puts
// a glow around the bolt instead.
uniform float4 da_lightning_sky; // what the flash added to the sky colour this frame

struct vi
{
	float4	p		: POSITION;
	float4	c		: COLOR0;
	float3	tc0		: TEXCOORD0;
	float3	tc1		: TEXCOORD1;
};

struct v2p
{
	float4	c		: COLOR0;
	float3	tc0		: TEXCOORD0;
	float3	tc1		: TEXCOORD1;
	float3	dir		: TEXCOORD2;	// world direction of the dome vertex
	float	scale	: TEXCOORD3;	// the tonemap prescale the colour carries
	float4	hpos	: SV_Position;
};

v2p main (vi v)
{
	v2p		o;

	float4	tpos	= float4(1000 * v.p.x, 500 * v.p.y, 1000 * v.p.z, 1000 * v.p.w);
	o.hpos			= mul(m_WVP, tpos);
	o.hpos.z		= o.hpos.w;
	o.tc0			= v.tc0;
	o.tc1			= v.tc1;
	float	scale	= s_tonemap.Load(int3(0, 0, 0)).x;
	float3	base	= max(v.c.rgb - da_lightning_sky.rgb, 0.0f);
	o.c				= float4(base * (scale * 2.0f), v.c.a);
	o.dir			= mul(m_W, float4(v.p.xyz, 0.0f));
	o.scale			= scale * 2.0f;

	return	o;
}
