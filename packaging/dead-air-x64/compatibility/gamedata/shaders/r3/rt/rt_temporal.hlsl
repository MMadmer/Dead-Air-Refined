// Temporal accumulation of a traced scalar (dxr.md 3.5, 3.6), shared by the sun shadow and the
// occlusion: the previous history is reprojected (motion vectors when drawn, else the camera's
// own motion through the depth), taken bilaterally by depth so a disocclusion starts over, clipped
// to the current 3x3 neighbourhood and blended with alpha = max( 1 / n, min_alpha ).
//
// History texel: x value, y frames accumulated, z view z it was written for, w the auxiliary
// channel (the shadow's hit distance; unused by the occlusion).

#include "rt_common.hlsli"

cbuffer rt_temporal_constants : register( b0 )
{
	RtView	view;
	float4	temporal;	// x trace width, y trace height, z render pixels per trace texel, w frame cap
	float4	temporal2;	// x minimum alpha, y raw.y is the view z (1) or an auxiliary value (0), z clip widening
};

Texture2D<float2>	tmp_raw			: register( t0 );
Texture2D<float4>	tmp_history		: register( t1 );
Texture2D<float2>	tmp_velocity	: register( t2 );
Texture2D<float>	tmp_depth		: register( t3 );

RWTexture2D<float4>	tmp_out			: register( u0 );

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	const float2 trace_size = temporal.xy;
	if ( any( float2( id.xy ) >= trace_size ) )
		return;

	const float2 cur = tmp_raw.Load( int3( id.xy, 0 ) );
	const float scale = temporal.z;
	const float2 pixel_center = ( float2( id.xy ) + 0.5f ) * scale;
	const uint2 render_pixel = min( uint2( pixel_center ), uint2( view.size.xy ) - 1u );
	float z;
	[branch] if ( temporal2.y > 0.5f )
		z = cur.y;
	else
	{
		const float d = tmp_depth.Load( int3( render_pixel, 0 ) );
		z = rt_is_sky( d ) ? view.proj.w : rt_linear_z( d, view );
	}
	const float aux = temporal2.y > 0.5f ? 0.0f : cur.y;

	// The neighbourhood of this frame's raw signal.
	float lo = cur.x, hi = cur.x;
	[unroll] for ( int y = -1; y <= 1; ++y )
	{
		[unroll] for ( int x = -1; x <= 1; ++x )
		{
			const int2 p = clamp( int2( id.xy ) + int2( x, y ), int2( 0, 0 ), int2( trace_size ) - 1 );
			const float v = tmp_raw.Load( int3( p, 0 ) ).x;
			lo = min( lo, v );
			hi = max( hi, v );
		}
	}
	const float widen = temporal2.z * ( hi - lo ) + 0.02f;
	lo -= widen;
	hi += widen;

	float4 history = float4( cur.x, 1.0f, z, aux );
	[branch] if ( ( view.scene.z & RT_VIEW_HISTORY_VALID ) && z < view.proj.w * 0.999f )
	{
		const float3 p_world = rt_to_world( rt_view_pos( pixel_center, z, view ), view );
		const float3 prev = rt_project( p_world, view.vp_prev );
		float2 prev_uv = prev.xy;
		[branch] if ( view.scene.z & RT_VIEW_VELOCITY )
		{
			const float2 v = tmp_velocity.Load( int3( render_pixel, 0 ) );
			prev_uv = pixel_center * view.size.zw - float2( v.x, -v.y ) * 0.5f;
		}
		const float expected_z = prev.z;

		// Bilinear taps of the history, each kept only where it saw the same surface.
		const float2 t = prev_uv * trace_size - 0.5f;
		const int2 base = int2( floor( t ) );
		const float2 f = t - float2( base );
		float4 sum = 0.0f;
		float weight = 0.0f;
		[unroll] for ( int k = 0; k < 4; ++k )
		{
			const int2 o = int2( k & 1, k >> 1 );
			const int2 p = base + o;
			if ( any( p < 0 ) || any( p >= int2( trace_size ) ) )
				continue;
			const float4 h = tmp_history.Load( int3( p, 0 ) );
			const float wb = ( o.x ? f.x : 1.0f - f.x ) * ( o.y ? f.y : 1.0f - f.y );
			const float wz = abs( h.z - expected_z ) < 0.05f + 0.03f * expected_z ? 1.0f : 0.0f;
			const float w = wb * wz;
			sum += h * w;
			weight += w;
		}
		[branch] if ( weight > 0.05f )
		{
			const float4 h = sum / weight;
			const float n = min( h.y + 1.0f, temporal.w );
			const float alpha = max( 1.0f / n, temporal2.x );
			history.x = lerp( clamp( h.x, lo, hi ), cur.x, alpha );
			history.y = n;
			history.w = lerp( h.w, aux, alpha );
		}
	}
	tmp_out[ id.xy ] = history;
}
