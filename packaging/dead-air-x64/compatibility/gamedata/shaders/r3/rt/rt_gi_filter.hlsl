// Edge-aware a-trous filter of the accumulated global illumination (dxr.md 3.11): a 5x5 B3 kernel with holes of
// `step` pixels, weighted by the distance to the centre's surface plane, the normals and the luminance - the latter
// loosened where the history is young (few frames: noisier) and tightened where it is long, so a settled image keeps
// its contact shading while a fresh one loses its noise. Several passes with growing steps.

#include "rt_common.hlsli"

cbuffer rt_gi_filter_constants : register( b0 )
{
	RtView	view;
	float4	filter;		// x step in pixels, y plane distance per metre of depth, z normal power, w luminance sigma
};

Texture2D<float4>	gf_source	: register( t0 );
Texture2D<float2>	gf_aux		: register( t1 );
Texture2D<float>	gf_depth	: register( t2 );
Texture2D<float4>	gf_position	: register( t3 );
Texture2D<float4>	gf_normal	: register( t4 );

RWTexture2D<float4>	gf_out		: register( u0 );

float gf_luma( float3 c ) { return dot( c, float3( 0.2126f, 0.7152f, 0.0722f ) ); }

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	const float4 centre = gf_source.Load( int3( id.xy, 0 ) );
	const float depth = gf_depth.Load( int3( id.xy, 0 ) );
	[branch] if ( rt_is_sky( depth ) || centre.a < 0.0f )
	{
		gf_out[ id.xy ] = centre;
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( gf_position, gf_normal, int2( id.xy ), view );
	const float3 p = rt_view_pos( float2( id.xy ) + 0.5f, z, view );
	const float frames = gf_aux.Load( int3( id.xy, 0 ) ).x;
	const float l = gf_luma( centre.rgb );
	// The noise of a mean of n samples falls as 1/sqrt(n).
	const float sigma_l = filter.w * ( l + 0.02f ) * rsqrt( max( frames, 1.0f ) ) + 1e-4f;
	const float plane_tol = filter.y * z + 0.02f;

	static const float kB3[3] = { 0.375f, 0.25f, 0.0625f };
	const int step = int( filter.x );
	const int2 size = int2( view.size.xy );
	float4 sum = 0.0f;
	float weight = 0.0f;
	[unroll] for ( int y = -2; y <= 2; ++y )
	{
		[unroll] for ( int x = -2; x <= 2; ++x )
		{
			const int2 q = clamp( int2( id.xy ) + int2( x, y ) * step, int2( 0, 0 ), size - 1 );
			const float4 v = gf_source.Load( int3( q, 0 ) );
			const float dq = gf_depth.Load( int3( q, 0 ) );
			[branch] if ( rt_is_sky( dq ) || v.a < 0.0f )
				continue;
			const float zq = rt_linear_z( dq, view );
			const RtSurface sq = rt_surface( gf_position, gf_normal, q, view );
			const float3 pq = rt_view_pos( float2( q ) + 0.5f, zq, view );
			const float wp = saturate( 1.0f - abs( dot( pq - p, s.n_view ) ) / plane_tol );
			const float wn = pow( saturate( dot( sq.n_view, s.n_view ) ), filter.z );
			const float wl = exp( -abs( gf_luma( v.rgb ) - l ) / sigma_l );
			const float w = kB3[ abs( x ) ] * kB3[ abs( y ) ] * wp * wn * wl;
			sum += v * w;
			weight += w;
		}
	}
	gf_out[ id.xy ] = weight > 1e-5f ? sum / weight : centre;
}
