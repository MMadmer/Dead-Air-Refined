#include "common.h"

// The shader fire's rasterization vehicle (CDaFireEffect::render_flame): a camera-facing quad
// over the flame's bounding sphere. The pixel shader marches the flame volume along the view
// ray from the eye; the quad's own depth means nothing, the scene depth comes from the G-buffer.

struct vv
{
	float4 P	: POSITION;
	float4 c	: COLOR0;
	float2 tc	: TEXCOORD0;
};

struct v2p
{
	float3 wpos		: TEXCOORD0;
	float4 tctexgen	: TEXCOORD1;
	float4 hpos		: SV_Position;
};

uniform float4x4 mVPTexgen;

v2p main (vv v)
{
	v2p o;
	o.hpos		= mul(m_WVP, v.P);
	o.wpos		= v.P.xyz;
	o.tctexgen	= mul(mVPTexgen, v.P);
	o.tctexgen.z = o.hpos.z;
	return o;
}
