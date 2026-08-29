#include "common.h"
#include "da_wind_motors.h"

uniform float4 		consts; // {1/quant,1/quant,diffusescale,ambient}
// See deffer_detail_w_flat.vs for the notes on all three.
uniform float4 		grass_sfade;
uniform float4 		grass_sfade_eye;
uniform float4 		grass_tint;
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

	// Grass shadow fade band - zero outside the sun shadow pass, see deffer_detail_w_flat.vs.
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

	// Wind motors: still details do not wave, but a boot or a blast still bends them.
	{
		const float H_s = v.pos.y * length(float3(m0.y, m1.y, m2.y));
		float press_w;
		float2 mb = da_wind_motors_bend(float3(m0.w, m1.w, m2.w), H_s, press_w);
		// Cap at "lying flat" - excess bend would pure-stretch the blade (see w_flat).
		mb *= min(1.0f, H_s / max(length(mb), 0.001f));
		// Same arc-length correction as the waving grass: a full-strength press LAYS the
		// tuft down instead of stretching it sideways.
		const float drop_s = H_s - sqrt(max(H_s * H_s - dot(mb, mb), 0.0f));
		pos.x += mb.x;
		pos.y -= drop_s;
		pos.z += mb.y;
	}

	// Normal in world coords
	float3 	norm;
		norm.x 	= pos.x - m0.w	;
		norm.y 	= pos.y - m1.w	+ .75f;	// avoid zero
		norm.z	= pos.z - m2.w	;

	// Final out
	float4	Pp 	= mul		(m_WVP,	pos				);
	O.hpos 		= Pp;

	O.N 		= mul		(m_WV,  normalize(norm)	);
	float3	Pe	= mul		(m_WV,  pos				);
	O.tcdh 		= float4	((v.misc * consts).xyyy	);

# if defined(USE_R2_STATIC_SUN)
	O.tcdh.w	= c0.x;								// (,,,dir-occlusion)
# endif

	// Ground-matched hemi variation - see deffer_detail_w_flat.vs for the notes.
	float da_hemi = c0.w;
	[branch] if ( grass_tint.x > 0.001f )
	{
		const float2 wxz = float2( m0.w, m2.w ) * grass_tint.y;
		const float2 c   = floor( wxz );
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
