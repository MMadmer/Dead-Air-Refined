#include "common.h"
#include "skin.h"
#include "da_particle_light.h"

// Glass and the other blended env-mapped models (models_xwindows.s, the EbB blender), forward over the tone-mapped
// frame (model_env_lq.ps): the world position and normal for the sky's mirror image, and the light a white surface
// there takes through the scene's exposure (da_particle_light.h) for the dirt on the glass. The stock pair lit it with
// the R1 hemisphere and mirrored a fixed daylight cube.

struct 	vf
{
	float2 tc0	: TEXCOORD0;	// base
	float3 pos	: TEXCOORD1;	// world position
	float3 nrm	: TEXCOORD2;	// world normal
	float3 c0	: COLOR0;		// the light, exposed
	float  fog	: FOG;
	float4 hpos	: SV_Position;
};

vf 	_main (v_model v)
{
	vf 		o;

	float4 	pos 	= v.P;
	float3  pos_w 	= mul			(m_W, pos);
	float3 	norm_w 	= normalize 		(mul(m_W,v.N));

	o.hpos 		= mul			(m_WVP, pos);		// xform, input in world coords
	o.tc0		= v.tc.xy;					// copy tc
	o.pos		= pos_w;
	o.nrm		= norm_w;
	o.c0 		= da_particle_light.x > 0.5f ? da_particle_exposed(da_particle_irradiance(pos_w, 0.0f, 0.0f)) : calc_model_lq_lighting(norm_w);
	o.fog 		= saturate(calc_fogging 		(float4(pos_w,1)) );	// fog, input in world coords

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
