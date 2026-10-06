#include "common.h"
#include "da_wind_field.h"
#include "da_detail_common.h"

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
// wave and dir2D as the previous frame drew with them, for the motion vectors.
uniform float4 		wave_prev;
uniform float4 		dir2D_prev;
// The instance array lives in its OWN constant buffer: it is rewritten per draw call, while
// everything above it changes once per pass, and a shared buffer makes every batch flush
// re-upload both. 512 instances is what the engine's batch ladder probes for first - it is
// eight draw calls' worth of the old 61-instance limit, which came from the DX9 vertex
// register file and has nothing to do with this hardware.
cbuffer DetailInstances
{
	uniform float4 		array[61*4];
};

// The grass maths lives in da_detail_common.h, shared with the DirectX 12 mesh-shader path.
v2p_flat 	main (v_detail v, uint instance_id : SV_InstanceID)
{
	// The instance index comes from the draw call, not from the vertex: the buffer holds
	// one copy of the geometry and DrawIndexedInstanced replicates it (see hw_Load_Geom).
	int 	i 	= int(instance_id) * 4;
	float4  m0 	= array[i+0];
	float4  m1 	= array[i+1];
	float4  m2 	= array[i+2];
	float4  c0 	= array[i+3];

	// Grass shadow fade band: the blade lies down before the shadow cull edge.
	[branch] if ( grass_sfade.y > 0.001f )
	{
		const float k = detail_sfade_k( float3( m0.w, m1.w, m2.w ), grass_sfade, grass_sfade_eye.xyz );
		m0.y *= k; m1.y *= k; m2.y *= k;
	}

	const float3 root	= float3(m0.w, m1.w, m2.w);
	// The season of the class (da_detail_common.h): keep, height, burial; the previous frame's rows for the
	// motion vectors (an item that just appeared has no history and keeps this frame's).
	const float hs	= detail_height_scale(m0, m1, m2);
	const float sk	= stock_season_k(root, hs, stock_season, false);
	float sk_prev	= sk;
	[branch] if ( da_wind_prev_on() )
	{
		sk_prev	= stock_season_k(root, hs, stock_season_prev, true);
		sk_prev	= sk_prev > 0.0f ? sk_prev : sk;
	}
	float4	m0p = m0, m1p = m1, m2p = m2;
	stock_season_rows(m0, m1, m2, sk);
	stock_season_rows(m0p, m1p, m2p, sk_prev);

	const float4 pos_rest = detail_world_pos(m0, m1, m2, v.pos);
	const float H 	= detail_height(m0, m1, m2, v.pos.y);
	const float frac	= v.misc.z*consts.x;		// fractional

	float4	pos	= detail_bent_pos(pos_rest, H, detail_bend(root, c0.w, H, frac, da_wind_field, wave, dir2D, false));
	// The same tuft under the previous frame's wind, only while motion vectors are rendered.
	float4	pos_prev	= pos;
	[branch] if ( da_wind_prev_on() )
	{
		const float4 pos_rest_prev = detail_world_pos(m0p, m1p, m2p, v.pos);
		const float H_prev = detail_height(m0p, m1p, m2p, v.pos.y);
		pos_prev	= detail_bent_pos(pos_rest_prev, H_prev, detail_bend(root, c0.w, H_prev, frac, da_wind_field_prev, wave_prev, dir2D_prev, true));
	}

	v2p_flat	O	= detail_v2p(root, pos, pos_prev, v.misc, consts, c0.x);

	float da_hemi = c0.w;
	[branch] if ( grass_tint.x > 0.001f )
		da_hemi = detail_tint_hemi( da_hemi, detail_tint_n( float2( m0.w, m2.w ), grass_tint.y ), grass_tint, v.pos.y );
	O.position.w	= da_hemi;

	return O;
}
FXVS;
