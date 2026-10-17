#include "common.h"
#ifndef DA_PARTICLE_UNLIT
#include "da_particle_light.h"
#endif

struct vv
{
	float4 P	: POSITION;
	float2 tc	: TEXCOORD0;
	float4 c	: COLOR0;
};

struct v2p
{
	float2 tc	: TEXCOORD0;
	float4 c	: COLOR0;

//	Igor: for additional depth dest
#ifdef	USE_SOFT_PARTICLES
	float4 tctexgen	: TEXCOORD1;
#endif	//	USE_SOFT_PARTICLES

	float4 hpos	: SV_Position;
	float  fog	: FOG;
	// The light the particle receives (da_particle_light.h), last so the pixel shaders that do not read it keep their
	// registers.
	float3 light	: TEXCOORD2;
};

uniform float4x4 	mVPTexgen;

v2p main (vv v)
{
	v2p 		o;

	o.hpos 		= mul	(m_WVP, v.P);		// xform, input in world coords
//	o.hpos 		= mul	(m_VP, v.P);		// xform, input in world coords
	o.tc		= v.tc;				// copy tc
	o.c			= unpack_D3DCOLOR(v.c);				// copy color

//	Igor: for additional depth dest
#ifdef	USE_SOFT_PARTICLES
	o.tctexgen 	= mul( mVPTexgen, v.P);
	o.tctexgen.z	= o.hpos.z;
#endif	//	USE_SOFT_PARTICLES

	o.fog 		= saturate(calc_fogging(v.P));	// skyloader: fog, input in world coords
#ifdef DA_PARTICLE_UNLIT
	o.light		= 1.0f;
#else
	o.light		= da_particle_lighting(v.P.xyz);
#endif

	return o;
}
