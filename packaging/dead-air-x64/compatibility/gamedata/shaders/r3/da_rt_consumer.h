#ifndef	da_rt_consumer_h_included
#define	da_rt_consumer_h_included

// What the engine shaders read of the ray tracing passes (docs/dead-air/dx12-features/dxr.md 3.5-3.7).
// da_rt: x reflections, y sun shadows, z ambient occlusion live this frame (0/1), w quality. It is
// zero on DirectX 11 and whenever an effect is off, and every consumer branches on it at run time:
// no permutation, no restart, and the frame without ray tracing is the frame it always was.
uniform float4		da_rt;

// The traced sun visibility of the render pixel (1 = lit), filtered.
Texture2D<float>	s_rt_sun;
// The mirror requests the water and puddle passes wrote: xy octahedral world direction, z view depth
// of the reflecting surface, w kind (1 puddle, 2 water).
Texture2D			s_rt_refl_req;
// The traced reflections: rgb in the frame copy's LDR space, a confidence (0 = the ray missed).
Texture2D			s_rt_refl;

// Octahedral encoding of a unit vector (the requests carry directions this way).
float2 da_rt_oct_encode( float3 n )
{
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	float2 e = n.xy;
	[flatten] if ( n.z < 0.0f )
		e = ( 1.0f - abs( n.yx ) ) * float2( n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f );
	return e;
}

#endif	//	da_rt_consumer_h_included
