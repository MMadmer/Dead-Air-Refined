// r__rt_debug views (dxr.md 4.1): 1 the sun mask, 2 the occlusion, 3 the reflections, 4 the albedo a
// primary ray finds against the G-buffer's own (left half traced, right half G-buffer), which is
// what proves the vertex and texture decode of the hit shading, 5 the glossy surfaces' reflections
// (tonemapped; where the sky's cube stays, dark blue).

#include "rt_geometry.hlsli"

cbuffer rt_debug_constants : register( b0 )
{
	RtView	view;
	uint4	debug;		// x mode
};

Texture2D<float>	dbg_sun		: register( t0 );
Texture2D<float>	dbg_ao		: register( t1 );
Texture2D<float4>	dbg_refl	: register( t2 );
Texture2D<float>	dbg_depth	: register( t3 );
Texture2D<float4>	dbg_albedo	: register( t4 );
Texture2D<float4>	dbg_gloss	: register( t5 );

RWTexture2D<float4>	dbg_out		: register( u0 );

float3 dbg_primary_albedo( uint2 pixel )
{
	const float3 p_view = rt_view_pos( float2( pixel ) + 0.5f, 1.0f, view );
	RayDesc ray;
	ray.Origin = view.camera.xyz;
	ray.Direction = normalize( rt_dir_to_world( p_view, view ) );
	ray.TMin = 0.05f;
	ray.TMax = 2000.0f;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, RT_RAYS_REFLECTION, ray );
	const float cone = view.size.w * 2.0f * view.proj.z;
	while ( q.Proceed() )
	{
		if ( q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			rt_alpha_solid( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(),
				q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone * q.CandidateTriangleRayT(), view ) )
			q.CommitNonOpaqueTriangleHit();
	}
	[branch] if ( q.CommittedStatus() != COMMITTED_TRIANGLE_HIT )
		return float3( 0.0f, 0.0f, 0.25f );
	const RtGeometry g = rt_geometry_of( q.CommittedInstanceID(), q.CommittedGeometryIndex() );
	const uint tex = rt_texture_index( g, view );
	[branch] if ( tex == 0xFFFFFFFFu )
		return float3( 1.0f, 0.0f, 1.0f );
	const RtTriangle t = rt_fetch( g, q.CommittedPrimitiveIndex() );
	const float3 w = rt_bary3( q.CommittedTriangleBarycentrics() );
	const float2 tc = t.v0.tc * w.x + t.v1.tc * w.y + t.v2.tc * w.z;
	uint tw, th, levels;
	rt_textures[ NonUniformResourceIndex( tex ) ].GetDimensions( 0, tw, th, levels );
	const float lod = min( rt_cone_lod( t, q.CommittedObjectToWorld3x4(), cone * q.CommittedRayT(), uint2( tw, th ) ),
		float( levels ) - 1.0f );
	const float3 albedo = rt_textures[ NonUniformResourceIndex( tex ) ].SampleLevel( rt_linear_wrap, tc, lod ).rgb;
	// (A stand-in crown in its species' colour of the day, as the reflections draw it.)
	const RtInstance inst = rt_instance_of( q.CommittedInstanceIndex(), view );
	[branch] if ( rt_veg_on( inst, view ) )
	{
		const float3 mean = rt_textures[ NonUniformResourceIndex( tex ) ].SampleLevel( rt_linear_wrap, tc, float( levels ) - 1.0f ).rgb;
		return rt_veg_colour( inst, view ) * albedo / max( mean, 0.02f );
	}
	return albedo;
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	float3 c = 0.0f;
	switch ( debug.x )
	{
	case 1u:
		c = dbg_sun.Load( int3( id.xy, 0 ) ).xxx;
		break;
	case 2u:
		c = dbg_ao.Load( int3( id.xy, 0 ) ).xxx;
		break;
	case 3u:
	{
		const float4 r = dbg_refl.Load( int3( id.xy, 0 ) );
		c = r.rgb * r.a;
		break;
	}
	case 5u:
	{
		const float4 r = dbg_gloss.Load( int3( id.xy, 0 ) );
		c = r.rgb / ( 1.0f + r.rgb ) + ( 1.0f - r.a ) * float3( 0.0f, 0.0f, 0.15f );
		break;
	}
	default:
		c = float( id.x ) < view.size.x * 0.5f ? dbg_primary_albedo( id.xy ) : dbg_albedo.Load( int3( id.xy, 0 ) ).rgb;
		break;
	}
	dbg_out[ id.xy ] = float4( c, 1.0f );
}
