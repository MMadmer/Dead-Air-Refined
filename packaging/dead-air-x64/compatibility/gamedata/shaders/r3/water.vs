// [DA] Water vertex shader. A copy of the stock one with ONE change: the second normal layer is
// decorrelated from the first.
//
// The stock pair samples the SAME 256x256 normal map twice, at scale 1.0 and 1.1, in the same
// orientation, and sums the two. A ten per cent scale ratio with no rotation is a moire
// generator: the two lattices realign every ten tiles, so the pair has a combined period ten
// times longer than either layer, and that long beat is exactly the "the same pattern again over
// there" the eye locks onto across a lake. Neither the wind layer nor the rain layer could hide
// it - both are world-XZ and both are gated off in calm dry weather, while this pair is always
// on and carries the full wave amplitude.
//
// So layer 1 gets a rotation and a scale that is not a near-integer ratio of layer 0. The beat
// period collapses to something no water body in the game is large enough to show. Layer 0 is
// left exactly as authored: the near-field look is what everyone is used to, and it is one
// sample of one texture - by itself it repeats no more visibly than any ground texture does.
//
// Not changed on purpose: watermove_tc stays an orbit rather than becoming a scroll. A scroll
// would unlock the pattern from the world too, but it reads as flow, and a still marsh should
// not flow. Motion is the wind layer's job (water.ps, da_water_wind_waves).

#include "common.h"
#include "shared\waterconfig.h"
#include "shared\watermove.h"

// Decorrelation of the second normal layer. The angle is deliberately off every axis and the
// tile is deliberately not a simple ratio of W_DISTORT_BASE_TILE_0.
#define W_DA_TILE_1  (1.63f)
#define W_DA_ROT_S   (0.6000f)  // sin/cos of 36.87 degrees
#define W_DA_ROT_C   (0.8000f)

struct	v_vert
{
	float4	P		: POSITION;		// (float,float,float,1)
	float4	N		: NORMAL;		// (nx,ny,nz,hemi occlusion)
	float4	T		: TANGENT;
	float4	B		: BINORMAL;
	float4	color	: COLOR0;		// (r,g,b,dir-occlusion)
	int2	uv		: TEXCOORD0;	// (u0,v0)
};

struct   vf
{
	float2	tbase	: TEXCOORD0;	// base
	float4	tnorm0	: TEXCOORD1;	// nm0
	float4	position_w	: TEXCOORD2;	// nm1
	float3	M1		: TEXCOORD3;
	float3	M2		: TEXCOORD4;
	float3	M3		: TEXCOORD5;
	float3	v2point_w	: TEXCOORD6;
#ifdef	USE_SOFT_WATER
#ifdef	NEED_SOFT_WATER
	float4	tctexgen: TEXCOORD7;
#endif	//	USE_SOFT_WATER
#endif	//	NEED_SOFT_WATER
	float4	c0		: COLOR0;
	float	fog		: FOG;
	float4	hpos	: SV_Position;
};

uniform float4x4	m_texgen;

vf main (v_vert v)
{
	v.N		=	unpack_D3DCOLOR(v.N);
	v.T		=	unpack_D3DCOLOR(v.T);
	v.B		=	unpack_D3DCOLOR(v.B);
	v.color	=	unpack_D3DCOLOR(v.color);

	vf		o;

	float4	P	= v.P;					// world
	float3	NN	= unpack_normal	(v.N);
			P	= watermove		(P);

	o.position_w	= float4 (P.xyz, 1.0);
	o.v2point_w		= P-eye_position;
	o.tbase			= unpack_tc_base (v.uv,v.T.w,v.B.w);	// copy tc
	o.tnorm0.xy		= watermove_tc (o.tbase*W_DISTORT_BASE_TILE_0, P.xz, W_DISTORT_AMP_0);
	// The rotated, rescaled twin - see the note at the top of the file.
	const float2 tb1 = float2( o.tbase.x*W_DA_ROT_C - o.tbase.y*W_DA_ROT_S,
	                           o.tbase.x*W_DA_ROT_S + o.tbase.y*W_DA_ROT_C ) * W_DA_TILE_1;
	o.tnorm0.zw		= watermove_tc (tb1, P.xz, W_DISTORT_AMP_1);

	// Calculate the 3x3 transform from tangent space to eye-space
	// TangentToEyeSpace = object2eye * tangent2object
	//                   = object2eye * transpose(object2tangent)
	float3		N	= unpack_bx2(v.N);	// just scale (assume normal in the -.5f, .5f)
	float3		T	= unpack_bx2(v.T);
	float3		B	= unpack_bx2(v.B);
	float3x3 xform	= mul	((float3x3)m_W, float3x3(
								T.x,B.x,N.x,
								T.y,B.y,N.y,
								T.z,B.z,N.z
							));

	// Feed this transform to pixel shader
	o.M1			= xform	[0];
	o.M2			= xform	[1];
	o.M3			= xform	[2];

	float3	L_rgb	= v.color.xyz;					// precalculated RGB lighting
	float3	L_hemi	= v_hemi(N)*v.N.w;				// hemisphere
	float3	L_sun	= v_sun(N)*v.color.w;			// sun
	float3	L_final	= L_rgb + L_hemi + L_sun + L_ambient;

	o.hpos			= mul (m_VP, P);				// xform, input in world coords
	o.fog			= saturate( calc_fogging (v.P) );

	o.c0			= float4 (L_final,1);

//	Igor: for additional depth dest
#ifdef	USE_SOFT_WATER
#ifdef	NEED_SOFT_WATER
	o.tctexgen		= mul( m_texgen, P);
	float3	Pe		= mul (m_V,  P);
	o.tctexgen.z	= Pe.z;
#endif	//	USE_SOFT_WATER
#endif	//	NEED_SOFT_WATER
	return o;
}
