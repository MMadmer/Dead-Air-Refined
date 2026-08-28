#include "common.h"

uniform float4 		consts; // {1/quant,1/quant,diffusescale,ambient}
// Fade band for the grass shadow: x = start, y = end, metres from the camera.
// STRICTLY ZERO outside the sun shadow pass, and zero means "do nothing".
uniform float4 		grass_sfade;
// World position of the CAMERA, handed over separately: in the sun shadow pass
// m_WV belongs to the SUN, so a view-space distance there measures the wrong thing.
uniform float4 		grass_sfade_eye;
// Terrain-matched hemi variation: x = strength, y = 1/patch size, z = base boost.
uniform float4 		grass_tint;
uniform float4 		wave; 	// cx,cy,cz,tm
uniform float4 		dir2D;
//uniform float4 		array	[200] : register(c12);
//tbuffer DetailsData
//{
	uniform float4 		array[61*4];
//}

v2p_flat 	main (v_detail v, uint instance_id : SV_InstanceID)
{
	v2p_flat 		O;
	// index
	// The instance index comes from the draw call, not from the vertex: the buffer holds
	// one copy of the geometry and DrawIndexedInstanced replicates it (see hw_Load_Geom).
	int 	i 	= int(instance_id) * 4;
	float4  m0 	= array[i+0];
	float4  m1 	= array[i+1];
	float4  m2 	= array[i+2];
	float4  c0 	= array[i+3];

	// Grass shadow fade band: the shadow cull edge reads as a cone travelling with the
	// player, so fade before the cut - by HEIGHT, not opacity: the blade lies down, its
	// shadow shortens and vanishes (opacity would need dithered alpha test, per-pixel work).
	// Height contribution is the y element of each row; distance is taken from the instance
	// ORIGIN so a blade never shrinks unevenly along its own height.
	[branch] if ( grass_sfade.y > 0.001f )
	{
		const float3 wp = float3( m0.w, m1.w, m2.w );
		const float  d  = distance( wp, grass_sfade_eye.xyz );
		const float  k  = saturate( ( grass_sfade.y - d ) /
		                            max( grass_sfade.y - grass_sfade.x, 0.001f ) );
		m0.y *= k; m1.y *= k; m2.y *= k;
	}

	// Transform pos to world coords
	float4 	pos;
 	pos.x 		= dot	(m0, v.pos);
 	pos.y 		= dot	(m1, v.pos);
 	pos.z 		= dot	(m2, v.pos);
	pos.w 		= 1;

	//
	float 	base 	= m1.w;
	float 	dp	= calc_cyclic   (dot(pos,wave));
	float 	H 	= pos.y - base;			// height of vertex (scaled)
	float 	frac 	= v.misc.z*consts.x;		// fractional
	float 	inten 	= H * dp;
	float2 	result	= calc_xz_wave	(dir2D.xz*inten,frac);
	pos		= float4(pos.x+result.x, pos.y, pos.z+result.y, 1);

	// Normal in world coords
	float3 	norm;	//	= float3(0,1,0);
		norm.x 	= pos.x - m0.w	;
		norm.y 	= pos.y - m1.w	+ .75f;	// avoid zero
		norm.z	= pos.z - m2.w	;

	// Final out
	float4	Pp 	= mul		(m_WVP,	pos				);
	O.hpos 		= Pp;
	O.N 		= mul		(m_WV,  normalize(norm)	);
	float3	Pe	= mul		(m_WV,  pos				);
//	O.tcdh 		= float4	((v.misc * consts).xy	);
	O.tcdh 		= float4	((v.misc * consts).xyyy );

# if defined(USE_R2_STATIC_SUN)
	O.tcdh.w	= c0.x;								// (,,,dir-occlusion)
# endif

	// Ground-matched brightness variation. Noise is keyed to the WORLD position of the tuft
	// (m0.w/m2.w), so the pattern stays put as the camera moves, like real soil unevenness -
	// screen-keyed noise would ride along and read as dirt on glass. We vary HEMI, not colour:
	// hemi already flows to the (shared) pixel shader, so the change lives entirely here.
	// Stronger near the BASE (grass_tint.z): the stem takes soil properties at the roots and
	// stays itself at the tip, killing the hard line where grass meets ground.
	float da_hemi = c0.w;
	[branch] if ( grass_tint.x > 0.001f )
	{
		const float2 wxz = float2( m0.w, m2.w ) * grass_tint.y;
		const float2 c   = floor( wxz );
		// frac() is shadowed by the local variable of the same name - take it manually.
		const float2 f   = smoothstep( 0.0f, 1.0f, wxz - c );
		float4 h   = sin( float4(
			dot( c + float2(0,0), float2(127.1f, 311.7f) ),
			dot( c + float2(1,0), float2(127.1f, 311.7f) ),
			dot( c + float2(0,1), float2(127.1f, 311.7f) ),
			dot( c + float2(1,1), float2(127.1f, 311.7f) ) ) ) * 43758.5453f;
		h = h - floor( h );
		const float n = lerp( lerp(h.x,h.y,f.x), lerp(h.z,h.w,f.x), f.y ) * 2.0f - 1.0f;

		const float up = saturate( v.pos.y * 2.0f );
		const float k  = lerp( 1.0f + grass_tint.z, 1.0f, up );
		da_hemi = saturate( da_hemi * ( 1.0f + n * grass_tint.x * k ) );
	}

	O.position	= float4	(Pe, 		da_hemi		);

	return O;
}
FXVS;
