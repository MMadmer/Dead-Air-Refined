// Edge-aware a-trous filter of the traced sun shadow (dxr.md 3.5): a 5x5 B3 kernel with holes of
// `step` pixels, weighted by the distance to the centre's surface plane and the normals, and
// limited to the penumbra the sun disc throws from the occluder's distance (hard contact shadows
// stay hard). Two passes: RT_ATROUS_FIRST reads the temporal history (x value, w hit distance) and
// writes (value, hit distance); the second writes the mask the sun passes read.

#include "rt_common.hlsli"

cbuffer rt_atrous_constants : register( b0 )
{
	RtView	view;
	float4	atrous;		// x step in pixels, y tan of the sun disc's radius, z plane distance per metre of depth, w normal power
};

#ifdef RT_ATROUS_FIRST
Texture2D<float4>	at_source	: register( t0 );
#else
Texture2D<float2>	at_source	: register( t0 );
#endif
Texture2D<float>	at_depth	: register( t1 );
Texture2D<float4>	at_position	: register( t2 );
Texture2D<float4>	at_normal	: register( t3 );

#ifdef RT_ATROUS_FIRST
RWTexture2D<float2>			at_out	: register( u0 );
#else
RWTexture2D<unorm float>	at_out	: register( u0 );
#endif

float2 at_load( int2 p )
{
#ifdef RT_ATROUS_FIRST
	const float4 h = at_source.Load( int3( p, 0 ) );
	return float2( h.x, h.w );
#else
	return at_source.Load( int3( p, 0 ) );
#endif
}

void at_write( uint2 p, float value, float hit )
{
#ifdef RT_ATROUS_FIRST
	at_out[ p ] = float2( value, hit );
#else
	at_out[ p ] = saturate( value );
#endif
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	const int2 size = int2( view.size.xy );
	const float2 centre = at_load( int2( id.xy ) );

	const float depth = at_depth.Load( int3( id.xy, 0 ) );
	[branch] if ( rt_is_sky( depth ) )
	{
		at_write( id.xy, centre.x, centre.y );
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( at_position, at_normal, int2( id.xy ), view );
	const float3 p_view = rt_view_pos( float2( id.xy ) + 0.5f, z, view );

	const int step = int( atrous.x );
	const float kernel[3] = { 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f };

	// The widest penumbra around: the hit distance of the occluders in reach times the disc.
	float lo = centre.x, hi = centre.x, hit = centre.y;
	[unroll] for ( int j = -2; j <= 2; ++j )
	{
		[unroll] for ( int i = -2; i <= 2; ++i )
		{
			const int2 p = clamp( int2( id.xy ) + int2( i, j ) * step, int2( 0, 0 ), size - 1 );
			const float2 v = at_load( p );
			lo = min( lo, v.x );
			hi = max( hi, v.x );
			hit = max( hit, v.y );
		}
	}
	const float penumbra_px = hit * atrous.y / max( rt_pixel_size( z, view ), 1e-5f );
	// Uniform around (fully lit or fully shadowed), or a hard edge: nothing to filter.
	[branch] if ( hi - lo < 0.004f || penumbra_px < 0.75f )
	{
		at_write( id.xy, centre.x, centre.y );
		return;
	}

	const float sigma_plane = atrous.z * z + 0.02f;
	const float inv_pen2 = 1.0f / ( 2.0f * penumbra_px * penumbra_px );
	float sum = 0.0f, weight = 0.0f;
	[unroll] for ( int y = -2; y <= 2; ++y )
	{
		[unroll] for ( int x = -2; x <= 2; ++x )
		{
			const int2 p = int2( id.xy ) + int2( x, y ) * step;
			if ( any( p < 0 ) || any( p >= size ) )
				continue;
			const float d = at_depth.Load( int3( p, 0 ) );
			if ( rt_is_sky( d ) )
				continue;
			const RtSurface sp = rt_surface( at_position, at_normal, p, view );
			const float3 q = rt_view_pos( float2( p ) + 0.5f, rt_linear_z( d, view ), view );
			const float plane = abs( dot( s.n_view, q - p_view ) );
			const float r2 = float( x * x + y * y ) * float( step * step );
			const float w = kernel[ abs( x ) ] * kernel[ abs( y ) ] * exp( -plane / sigma_plane ) *
				pow( saturate( dot( s.n_view, sp.n_view ) ), atrous.w ) * exp( -r2 * inv_pen2 );
			sum += at_load( p ).x * w;
			weight += w;
		}
	}
	at_write( id.xy, weight > 1e-5f ? sum / weight : centre.x, centre.y );
}
