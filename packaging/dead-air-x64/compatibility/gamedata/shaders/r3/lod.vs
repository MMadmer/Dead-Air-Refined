#include "common.h"
#include "da_wind_field.h"

// Far vegetation impostors. Stock: two crossfaded billboards that never moved, so at the
// LOD switch a swaying crown became a stiff card - the ONE moment the eye is guaranteed to
// be looking at a tree. The card now sways with the same field, the same heading and the
// same waveform as the model it stands in for; the vertical texture coordinate tells the
// vertex how high on the tree it is.
uniform float4 wind, wave;

struct vv
{
	float3 pos0	: POSITION0	;
	float3 pos1	: POSITION1	;
	float3 n0	: NORMAL0	;
	float3 n1	: NORMAL1	;
	float2 tc0	: TEXCOORD0	;
	float2 tc1	: TEXCOORD1	;
	float4 rgbh0	: TEXCOORD2;	// rgb.h
	float4 rgbh1	: TEXCOORD3;	// rgb.h
	float4 sun_af	: COLOR0;	// x=sun_0, y=sun_1, z=alpha, w=factor
};
struct vf
{
	float3	Pe	: TEXCOORD0	;
 	float2 	tc0	: TEXCOORD1	;	// base0
 	float2 	tc1	: TEXCOORD2	;	// base1
	float4 	af	: COLOR1	;	// alpha&factor
	float4 	hpos: SV_Position;
};

#define L_SCALE (2.0h*1.55h)
vf 	main	( vv I )
{
	vf 		o;

	I.sun_af.xyz	= I.sun_af.zyx;
	I.rgbh0.xyz		= I.rgbh0.zyx;
	I.rgbh1.xyz		= I.rgbh1.zyx;

	// lerp pos
	float 	factor 	= I.sun_af.w	;
	float3	p		= lerp(I.pos0,I.pos1,factor);

	// Wind. The card's root is where the two billboards meet the ground; the top row of the
	// texture (tc.y = 0) is the crown, the bottom (1) the root, so the vertical coordinate is
	// the flexibility the models carry per vertex. Impostors are ~12 m trees, hence the
	// height scale; the same field/heading/waveform as deffer_tree_flat.vs so a tree does not
	// change its mind at the LOD switch.
	const float top = saturate(1.0f - I.tc0.y);
	[branch] if (top > 0.01f && dot(wind.xz, wind.xz) > 0.0f)
	{
		float3 flow = da_wind_field_eval(p.xz);
		const float2 wdir = da_wind_local_dir(wind.xz, flow.z);
		const float freq_k = 0.82f + 0.42f * da_wf_hash(p.xz * 0.37f);
		const float wind_k = saturate(da_wind_field.z);
		const float sway_mean = 0.45f + 0.35f * wind_k;
		const float dp = sway_mean + (1.0f - sway_mean) * da_sway(wave.w * freq_k + dot(p, (float3)wave));
		// The trunk profile of da_tree_bend.h (top^1.5, three quarters at the crown) and its cap.
		float2 bend = wdir * (12.0f * top * sqrt(top) * 0.75f * (dp * flow.x + flow.y * 0.5f));
		const float bend_len = length(bend);
		const float bend_max = 12.0f * top * 0.50f;
		[branch] if (bend_len > 0.001f)
			bend *= bend_max * tanh(bend_len / bend_max) / bend_len;
		p.xz += bend;
	}
	float4 	pos 	= float4	(p,1);

	float 	h 	= lerp		(I.rgbh0.w,I.rgbh1.w,factor)		*L_SCALE;

	o.hpos 		= mul		(m_VP, 	pos);				// xform, input in world coords
	o.Pe		= mul		(m_V,	pos);

	// replicate TCs
	o.tc0		= I.tc0;
	o.tc1		= I.tc1;

	// calc normal & lighting
	o.af		= float4	(h,h,I.sun_af.z,factor);
	return o	;
}
FXVS;
