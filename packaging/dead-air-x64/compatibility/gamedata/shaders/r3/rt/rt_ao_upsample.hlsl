// The accumulated occlusion into the GTAO target (dxr.md 3.6): x = linear view z (the guide the
// existing guided filter reads), y = visibility. From half resolution the four nearest traced
// texels are weighted bilinearly and by how close their depth is to this pixel's, so occlusion
// does not bleed across silhouettes; at full resolution it is a copy.

#include "rt_common.hlsli"

cbuffer rt_upsample_constants : register( b0 )
{
	RtView	view;
	float4	upsample;	// x trace width, y trace height, z render pixels per trace texel
};

Texture2D<float4>	up_history	: register( t0 );
Texture2D<float>	up_depth	: register( t1 );

RWTexture2D<float2>	up_out		: register( u0 );

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	const float depth = up_depth.Load( int3( id.xy, 0 ) );
	[branch] if ( rt_is_sky( depth ) )
	{
		up_out[ id.xy ] = float2( view.proj.w, 1.0f );
		return;
	}
	const float z = rt_linear_z( depth, view );

	[branch] if ( upsample.z < 1.5f )
	{
		up_out[ id.xy ] = float2( z, up_history.Load( int3( id.xy, 0 ) ).x );
		return;
	}

	const float2 t = ( float2( id.xy ) + 0.5f ) / upsample.z - 0.5f;
	const int2 base = int2( floor( t ) );
	const float2 f = t - float2( base );
	const int2 limit = int2( upsample.xy ) - 1;
	float sum = 0.0f, weight = 0.0f, nearest = 1.0f, nearest_dz = 1e30f;
	[unroll] for ( int k = 0; k < 4; ++k )
	{
		const int2 o = int2( k & 1, k >> 1 );
		const float4 h = up_history.Load( int3( clamp( base + o, int2( 0, 0 ), limit ), 0 ) );
		const float dz = abs( h.z - z );
		const float wb = ( o.x ? f.x : 1.0f - f.x ) * ( o.y ? f.y : 1.0f - f.y );
		const float w = wb * exp( -dz / ( 0.02f + 0.05f * z ) ) + 1e-4f * wb;
		sum += h.x * w;
		weight += w;
		if ( dz < nearest_dz )
		{
			nearest_dz = dz;
			nearest = h.x;
		}
	}
	const float ao = weight > 1e-3f ? sum / weight : nearest;
	up_out[ id.xy ] = float2( z, ao );
}
