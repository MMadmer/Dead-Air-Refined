// [DA] Water vertex shader.
//
// The vertex INPUT is a fixed contract - the declaration is baked into the shipped level.geom
// and cannot change. The output no longer is: the surface shader builds its normals in world
// space from world XZ, so the tangent frame that used to eat three interpolators (M1/M2/M3) is
// gone, and with it the bug where the wind direction on water depended on the level author's UV
// layout. Four interpolators instead of eight.
//
// tctexgen is emitted UNCONDITIONALLY now. The stock file put it behind USE_SOFT_WATER while the
// pixel shader declared it always, so with r2_soft_water off - which two shipped presets do -
// the VS and PS signatures disagreed. Depth is cheap and everything downstream wants it; the
// preset gate moved into the pixel shader, where it decides how much work to do, not whether the
// shaders link.
//
// watermove() stays: the mesh keeps its stock bob. Nothing here displaces geometry, because at
// this fetch and this wind the waves are 0.5-6 cm high - smaller than a vertex.

#include "common.h"
#include "shared\waterconfig.h"
#include "shared\watermove.h"

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
	float2	tbase		: TEXCOORD0;	// base / film / debris UV, as authored
	float4	position_w	: TEXCOORD1;	// xyz world position, w = sun access
	float4	v2point_w	: TEXCOORD2;	// xyz eye->point in world, w = sky openness
	float4	tctexgen	: TEXCOORD3;	// screen texgen, z = view-space depth of the surface
	float4	c0			: COLOR0;		// the light actually reaching this patch of water
	float	fog			: FOG;
	float4	hpos		: SV_Position;
};

uniform float4x4	m_texgen;

vf main (v_vert v)
{
	v.N		=	unpack_D3DCOLOR(v.N);
	v.T		=	unpack_D3DCOLOR(v.T);
	v.B		=	unpack_D3DCOLOR(v.B);
	v.color	=	unpack_D3DCOLOR(v.color);

	vf		o;

	float4	P	= v.P;
			P	= watermove		(P);
	float3	N	= unpack_bx2	(v.N);

	o.position_w	= float4 (P.xyz, v.color.w);
	o.v2point_w		= float4 (P.xyz - eye_position, v.N.w);
	o.tbase			= unpack_tc_base (v.uv, v.T.w, v.B.w);

	float3	L_rgb	= v.color.xyz;					// precalculated RGB lighting
	float3	L_hemi	= v_hemi(N)*v.N.w;				// hemisphere
	float3	L_sun	= v_sun(N)*v.color.w;			// sun
	o.c0			= float4 (L_rgb + L_hemi + L_sun + L_ambient, 1);

	o.hpos			= mul (m_VP, P);
	o.fog			= saturate( calc_fogging (v.P) );

	o.tctexgen		= mul( m_texgen, P);
	float3	Pe		= mul (m_V,  P);
	o.tctexgen.z	= Pe.z;

	return o;
}
