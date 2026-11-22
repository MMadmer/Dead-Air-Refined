// The traced global illumination at full resolution, accumulated over frames (dxr.md 3.11). Every render pixel takes
// this frame's samples around it - the 3x3 traced blocks at half resolution, the 3x3 pixels at full - weighted by
// distance, depth and normal; its history follows the surface (the motion vectors where drawn, else the camera's own
// motion through the depth), tap by tap only where it saw the same surface, and is clipped to the spread of the fresh
// neighbourhood, so a light switched on or a door opened shows within a few frames while the noise of one ray a pixel
// averages away over many. Colours compare in a tonemapped space (x / (1 + max x)).
//
// History: colour (rgb, the sky's share) and aux (frames accumulated, view z).

#include "rt_common.hlsli"

cbuffer rt_gi_temporal_constants : register( b0 )
{
	RtView	view;
	float4	temporal;	// x trace width, y trace height, z half resolution (1) or full (0), w frame cap
	float4	temporal2;	// x least alpha, y clip width (sigmas), z least share of a pixel's own fresh sample
};

Texture2D<float4>	gt_raw			: register( t0 );
Texture2D<float>	gt_raw_z		: register( t1 );
Texture2D<float4>	gt_history		: register( t2 );
Texture2D<float2>	gt_history_aux	: register( t3 );
Texture2D<float>	gt_depth		: register( t4 );
Texture2D<float4>	gt_position		: register( t5 );
Texture2D<float4>	gt_normal		: register( t6 );
Texture2D<float2>	gt_velocity		: register( t7 );

RWTexture2D<float4>	gt_out			: register( u0 );
RWTexture2D<float2>	gt_out_aux		: register( u1 );

float3 gt_tm( float3 c ) { return c / ( 1.0f + max( c.r, max( c.g, c.b ) ) ); }
float3 gt_tm_inv( float3 c ) { return c / max( 1.0f - max( c.r, max( c.g, c.b ) ), 1e-3f ); }

// The render pixel a trace texel took this frame (rt_gi.hlsl gi_trace_pixel).
uint2 gt_trace_pixel( uint2 t )
{
	[branch] if ( temporal.z < 0.5f )
		return t;
	const uint f = view.scene.w & 3u;
	const uint2 offset = uint2( f & 1u, ( ( f >> 1 ) ^ f ) & 1u );
	return min( t * 2u + offset, uint2( view.size.xy ) - 1u );
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	const float depth = gt_depth.Load( int3( id.xy, 0 ) );
	[branch] if ( rt_is_sky( depth ) )
	{
		gt_out[ id.xy ] = float4( 0.0f, 0.0f, 0.0f, 1.0f );
		gt_out_aux[ id.xy ] = float2( 0.0f, view.proj.w );
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( gt_position, gt_normal, int2( id.xy ), view );
	[branch] if ( rt_is_hud( z, s ) )
	{
		gt_out[ id.xy ] = float4( 0.0f, 0.0f, 0.0f, -1.0f );
		gt_out_aux[ id.xy ] = float2( 0.0f, z );
		return;
	}

	// This frame's samples around the pixel.
	const bool half_res = temporal.z > 0.5f;
	const int2 centre = half_res ? int2( id.xy / 2u ) : int2( id.xy );
	const int2 last = int2( temporal.xy ) - 1;
	// Two sets of weights: with the normals, and without them for the leaves, blades and twigs, whose normals turn
	// from pixel to pixel - with the normals alone they found no sample and went black.
	float3 sum = 0.0f, sum2 = 0.0f, lsum = 0.0f, lsum2 = 0.0f;
	float sky = 0.0f, weight = 0.0f, peak = 0.0f, lsky = 0.0f, lweight = 0.0f, lpeak = 0.0f;
	[unroll] for ( int dy = -1; dy <= 1; ++dy )
	{
		[unroll] for ( int dx = -1; dx <= 1; ++dx )
		{
			const int2 t = clamp( centre + int2( dx, dy ), int2( 0, 0 ), last );
			const uint2 sp = gt_trace_pixel( uint2( t ) );
			const float2 d = float2( sp ) - float2( id.xy );
			const float zs = gt_raw_z.Load( int3( t, 0 ) );
			const RtSurface ss = rt_surface( gt_position, gt_normal, int2( sp ), view );
			const float facing = saturate( dot( ss.n_view, s.n_view ) );
			const float4 r = gt_raw.Load( int3( t, 0 ) );
			float wl = exp( -0.35f * dot( d, d ) ) * saturate( 1.0f - abs( zs - z ) / ( 0.03f * z + 0.05f ) );
			[flatten] if ( r.a < 0.0f )
				wl = 0.0f;
			const float w = wl * pow( facing, 8.0f );
			const float3 c = gt_tm( r.rgb );
			sum += c * w;
			sum2 += c * c * w;
			sky += r.a * w;
			weight += w;
			peak = max( peak, w );
			lsum += c * wl;
			lsum2 += c * c * wl;
			lsky += r.a * wl;
			lweight += wl;
			lpeak = max( lpeak, wl );
		}
	}
	[branch] if ( weight <= 1e-3f )
	{
		sum = lsum;
		sum2 = lsum2;
		sky = lsky;
		weight = lweight;
		peak = lpeak;
	}
	const bool fresh = weight > 1e-3f;
	const float3 mean = fresh ? sum / weight : 0.0f;
	const float3 sigma = fresh ? sqrt( max( sum2 / weight - mean * mean, 0.0f ) ) : 1.0f;
	const float mean_sky = fresh ? sky / weight : 0.0f;

	// The history where the surface was.
	float4 history = 0.0f;
	float frames = 0.0f;
	[branch] if ( view.scene.z & RT_VIEW_HISTORY_VALID )
	{
		const float2 pixel_center = float2( id.xy ) + 0.5f;
		const float3 p_world = rt_to_world( rt_view_pos( pixel_center, z, view ), view );
		const float3 prev = rt_project( p_world, view.vp_prev );
		float2 prev_uv = prev.xy;
		[branch] if ( view.scene.z & RT_VIEW_VELOCITY )
		{
			const float2 v = gt_velocity.Load( int3( id.xy, 0 ) );
			prev_uv = pixel_center * view.size.zw - float2( v.x, -v.y ) * 0.5f;
		}
		const float2 tp = prev_uv * view.size.xy - 0.5f;
		const int2 base = int2( floor( tp ) );
		const float2 f = tp - float2( base );
		float4 hsum = 0.0f;
		float nsum = 0.0f, hw = 0.0f;
		[unroll] for ( int k = 0; k < 4; ++k )
		{
			const int2 o = int2( k & 1, k >> 1 );
			const int2 p = base + o;
			if ( any( p < 0 ) || any( p >= int2( view.size.xy ) ) )
				continue;
			const float2 aux = gt_history_aux.Load( int3( p, 0 ) );
			const float wb = ( o.x ? f.x : 1.0f - f.x ) * ( o.y ? f.y : 1.0f - f.y );
			const float wz = abs( aux.y - prev.z ) < 0.05f + 0.03f * prev.z ? 1.0f : 0.0f;
			const float4 h = gt_history.Load( int3( p, 0 ) );
			const float w = wb * wz * ( h.a < 0.0f ? 0.0f : 1.0f );
			hsum += h * w;
			nsum += aux.x * w;
			hw += w;
		}
		[branch] if ( hw > 0.05f )
		{
			history = hsum / hw;
			frames = nsum / hw;
		}
	}

	float4 result;
	float n;
	[branch] if ( frames < 0.5f )
	{
		// A new surface: what the neighbourhood traced; nothing at all yet (a twig against the far background), and the
		// combine keeps its stock ambient there (-1, as for the HUD) rather than none.
		result = fresh ? float4( gt_tm_inv( mean ), mean_sky ) : float4( 0.0f, 0.0f, 0.0f, -1.0f );
		n = fresh ? 1.0f : 0.0f;
	}
	else [branch] if ( !fresh )
	{
		result = history;
		n = frames;
	}
	else
	{
		n = min( frames + 1.0f, temporal.w );
		// The history clipped to what the fresh neighbourhood allows: a change of light gets through.
		float3 h = gt_tm( history.rgb );
		h = clamp( h, mean - temporal2.y * sigma, mean + temporal2.y * sigma );
		const float alpha = saturate( max( 1.0f / n, temporal2.x ) * max( peak, temporal2.z ) );
		result = float4( gt_tm_inv( lerp( h, mean, alpha ) ), lerp( history.a, mean_sky, alpha ) );
	}
	gt_out[ id.xy ] = result;
	gt_out_aux[ id.xy ] = float2( n, z );
}
