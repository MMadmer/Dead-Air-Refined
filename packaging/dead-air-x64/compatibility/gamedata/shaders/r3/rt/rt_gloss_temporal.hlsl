// The glossy reflections at full resolution (dxr.md 3.9), after DOOM Eternal's computereflectionstemporalupscale:
// every render pixel takes the 3x3 blocks of this frame's half-resolution trace around it, weighted by distance
// (Gaussian), depth and normal; mean and spread of that neighbourhood clip the history, which follows the reflected
// point (the mirror image behind a coplanar surface moves with the hit's parallax, a curved one with the surface).
// A pixel that holds this frame's sample takes most of it; the others live on the history. Colours blend in a
// tonemapped space (x / (1 + max x)), so a bright highlight cannot outweigh its neighbourhood. The result goes to
// the history and to the target combine_1 reads, premultiplied by its confidence.

#include "rt_common.hlsli"
#include "da_material_codes.h"

cbuffer rt_gloss_temporal_constants : register( b0 )
{
	RtView	view;
	float4	temporal;	// x trace width, y trace height, z lowest gloss traced, w clip width (sigmas)
	float4	temporal2;	// x least history weight left to a pixel without a fresh sample
};

Texture2D<float4>	gt_raw		: register( t0 );
Texture2D<float>	gt_dist		: register( t1 );
Texture2D<float4>	gt_history	: register( t2 );
Texture2D<float>	gt_depth	: register( t3 );
Texture2D<float4>	gt_position	: register( t4 );
Texture2D<float4>	gt_normal	: register( t5 );
Texture2D<float4>	gt_albedo	: register( t6 );

RWTexture2D<float4>	gt_out		: register( u0 );
RWTexture2D<float4>	gt_result	: register( u1 );

float3 gt_tm( float3 c ) { return c / ( 1.0f + max( c.r, max( c.g, c.b ) ) ); }
float3 gt_tm_inv( float3 c ) { return c / max( 1.0f - max( c.r, max( c.g, c.b ) ), 1e-3f ); }

void gt_write( uint2 pixel, float4 v )
{
	gt_out[ pixel ] = v;
	gt_result[ pixel ] = v;
}

// The pixel of a block this frame's trace took (rt_gloss.hlsl gl_block_pixel).
uint2 gt_block_pixel( uint2 block )
{
	const uint f = view.scene.w & 3u;
	const uint2 offset = uint2( f & 1u, ( ( f >> 1 ) ^ f ) & 1u );
	return min( block * 2u + offset, uint2( view.size.xy ) - 1u );
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	const float depth = gt_depth.Load( int3( id.xy, 0 ) );
	const float g = gt_albedo.Load( int3( id.xy, 0 ) ).w;
	const float pw = gt_position.Load( int3( id.xy, 0 ) ).w;
	const float mtl = ( view.scene.z & RT_VIEW_GBUFFER_PACKED ) ? gbuf_unpack_mtl( pw ) : abs( pw );
	[branch] if ( rt_is_sky( depth ) || g < temporal.z || da_is_foliage( mtl ) || da_is_snowy( mtl ) )
	{
		gt_write( id.xy, 0.0f );
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( gt_position, gt_normal, int2( id.xy ), view );

	// The neighbourhood of this frame's samples.
	const int2 block = int2( id.xy / 2u );
	const int2 blocks = int2( temporal.xy ) - 1;
	float3 sum = 0.0f, sum2 = 0.0f;
	float conf = 0.0f, conf_lo = 1.0f, conf_hi = 0.0f;
	float weight = 0.0f, peak = 0.0f, dist = 0.0f, dist_w = 0.0f;
	bool coplanar = true;
	[unroll] for ( int dy = -1; dy <= 1; ++dy )
	{
		[unroll] for ( int dx = -1; dx <= 1; ++dx )
		{
			const int2 b = clamp( block + int2( dx, dy ), int2( 0, 0 ), blocks );
			const uint2 sp = gt_block_pixel( uint2( b ) );
			const float2 d = float2( sp ) - float2( id.xy );
			float w = exp( -0.5f * dot( d, d ) );
			const float ds = gt_depth.Load( int3( sp, 0 ) );
			const float zs = rt_is_sky( ds ) ? 1e6f : rt_linear_z( ds, view );
			const RtSurface ss = rt_surface( gt_position, gt_normal, int2( sp ), view );
			const float facing = saturate( dot( ss.n_view, s.n_view ) );
			coplanar = coplanar && facing > 0.98f;
			w *= saturate( 1.0f - abs( zs - z ) / ( 0.02f * z + 0.05f ) ) * pow( facing, 16.0f );
			const float4 r = gt_raw.Load( int3( b, 0 ) );
			const float3 c = gt_tm( r.rgb );
			sum += c * w;
			sum2 += c * c * w;
			conf += r.a * w;
			[branch] if ( w > 0.01f )
			{
				conf_lo = min( conf_lo, r.a );
				conf_hi = max( conf_hi, r.a );
			}
			const float t = gt_dist.Load( int3( b, 0 ) );
			[flatten] if ( t > 0.0f )
			{
				dist += t * w;
				dist_w += w;
			}
			weight += w;
			peak = max( peak, w );
		}
	}

	const float3 p_world = rt_to_world( rt_view_pos( float2( id.xy ) + 0.5f, z, view ), view );
	const bool fresh = weight > 1e-3f;
	float3 mean = fresh ? sum / weight : 0.0f;
	const float3 sigma = fresh ? sqrt( max( sum2 / weight - mean * mean, 0.0f ) ) : 1.0f;
	const float mean_conf = fresh ? conf / weight : 0.0f;

	// The history at the reflected point's previous place: on a plane the mirror image lies on the eye's ray at the
	// reflector's distance plus the hit's; elsewhere the surface itself.
	float4 history = 0.0f;
	bool valid = false;
	[branch] if ( view.scene.z & RT_VIEW_HISTORY_VALID )
	{
		const float3 eye = view.camera.xyz;
		const float hit = dist_w > 1e-3f ? dist / dist_w : 0.0f;
		const float3 follow = ( coplanar && hit > 0.0f ) ? eye + normalize( p_world - eye ) * ( length( p_world - eye ) + hit ) : p_world;
		const float3 prev = rt_project( follow, view.vp_prev );
		[branch] if ( prev.z > 0.05f && all( prev.xy > 0.0f ) && all( prev.xy < 1.0f ) )
		{
			history = gt_history.SampleLevel( rt_linear_clamp, prev.xy, 0 );
			valid = true;
		}
	}

	float4 result;
	[branch] if ( !valid )
		result = float4( gt_tm_inv( mean ), mean_conf );
	else [branch] if ( !fresh )
		result = history;
	else
	{
		// The history clipped to what this frame's neighbourhood allows.
		float3 h = gt_tm( history.rgb );
		h = clamp( h, mean - temporal.w * sigma, mean + temporal.w * sigma );
		const float hc = clamp( history.a, conf_lo, conf_hi );
		// A pixel next to a fresh sample takes it; one between samples keeps more history.
		const float alpha = saturate( max( temporal2.x, 0.85f * peak ) );
		result = float4( gt_tm_inv( lerp( h, mean, alpha ) ), lerp( hc, mean_conf, alpha ) );
	}
	gt_write( id.xy, result );
}
