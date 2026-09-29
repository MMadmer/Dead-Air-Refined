#include "common.h"
#include "skin.h"

// [DA_PORT] The layout of particle.vs, which particle_distort.ps and particle_hard.ps read: they
// take the soft-particle TEXCOORD1 and the fog term too. D3D11 handed them unwritten registers
// (the fog multiplies the alpha); DirectX 12 refuses the pipeline and the effect never drew.
struct vf
{
	float2	tc0	: TEXCOORD0;	// base
	float4	c0	: COLOR0;		// color
#ifdef	USE_SOFT_PARTICLES
	float4	tctexgen	: TEXCOORD1;
#endif
	float4	hpos: SV_Position;
	float	fog	: FOG;
};

uniform float4x4 	mVPTexgen;

vf 	_main (v_model v)
{
	vf 		o;

	o.hpos 		= mul			(m_WVP, v.P);		// xform, input in world coords
	o.tc0		= v.tc.xy;					// copy tc

	// calculate fade
	float3  dir_v   = normalize    (mul(m_WV,v.P));
	float3  norm_v  = normalize    (mul(m_WV,v.N));
	float 	fade 	= abs			(dot(dir_v,norm_v));
	o.c0		= fade;

	// [DA_PORT] Written the way particle.vs writes them, from the world position.
	float3	pos_w	= mul			(m_W, v.P);
#ifdef	USE_SOFT_PARTICLES
	o.tctexgen	= mul			(mVPTexgen, float4(pos_w, 1));
	o.tctexgen.z	= o.hpos.z;
#endif
	o.fog		= saturate(calc_fogging(float4(pos_w, 1)));

	return o;
}

/////////////////////////////////////////////////////////////////////////
#ifdef 	SKIN_NONE
vf	main(v_model v) 		{ return _main(v); 		}
#endif

#ifdef 	SKIN_0
vf	main(v_model_skinned_0 v) 	{ return _main(skinning_0(v)); }
#endif

#ifdef	SKIN_1
vf	main(v_model_skinned_1 v) 	{ return _main(skinning_1(v)); }
#endif

#ifdef	SKIN_2
vf	main(v_model_skinned_2 v) 	{ return _main(skinning_2(v)); }
#endif

#ifdef	SKIN_3
vf	main(v_model_skinned_3 v) 	{ return _main(skinning_3(v)); }
#endif

#ifdef	SKIN_4
vf	main(v_model_skinned_4 v) 	{ return _main(skinning_4(v)); }
#endif