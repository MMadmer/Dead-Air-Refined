// Ray traced sun shadow (dxr.md 3.5): one ray per render pixel toward the sun against the static
// world only (RT_RAYS_SHADOW: vegetation and moving objects keep their swaying shadow maps). The
// near and middle cascades leave the static casters out while this runs; the accumulation
// multiplies the two.
//
// RT_SUN_DIRECT (quality Low): a hard shadow straight into the mask the sun passes read, no
// jitter, no filter. Otherwise (visibility, hit distance) for the temporal pass and the filter.

#include "rt_geometry.hlsli"

cbuffer rt_sun_constants : register( b0 )
{
	RtView	view;
	float4	sun_params;	// x TMax, y jitter inside the sun disc (0/1), z normal offset base, w normal offset per metre
};

Texture2D<float>	sun_depth		: register( t0 );
Texture2D<float4>	sun_position	: register( t1 );
Texture2D<float4>	sun_normal		: register( t2 );

#ifdef RT_SUN_DIRECT
RWTexture2D<unorm float>	sun_out	: register( u0 );
#else
RWTexture2D<float2>			sun_out	: register( u0 );
#endif

void sun_write( uint2 pixel, float visibility, float hit_distance )
{
#ifdef RT_SUN_DIRECT
	sun_out[ pixel ] = visibility;
#else
	sun_out[ pixel ] = float2( visibility, hit_distance );
#endif
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;

	const float depth = sun_depth.Load( int3( id.xy, 0 ) );
	[branch] if ( rt_is_sky( depth ) )
	{
		sun_write( id.xy, 1.0f, 0.0f );
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( sun_position, sun_normal, int2( id.xy ), view );
	// The hands are not in the scene: they take the sun the eye sees. Written as lit, they shone in full sun under
	// any roof, since the cascades leave the static casters to these rays.
	[branch] if ( rt_is_hud( z, s ) )
	{
		RayDesc eye_ray;
		eye_ray.Origin = view.camera.xyz;
		eye_ray.Direction = view.sun.xyz;
		eye_ray.TMin = 0.05f;
		eye_ray.TMax = sun_params.x;
		RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> eye;
		eye.TraceRayInline( rt_scene, RAY_FLAG_NONE, RT_RAYS_SHADOW, eye_ray );
		while ( eye.Proceed() )
		{
			if ( eye.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
				rt_alpha_solid( eye.CandidateInstanceID(), eye.CandidateInstanceIndex(), eye.CandidateGeometryIndex(),
					eye.CandidatePrimitiveIndex(), eye.CandidateTriangleBarycentrics(), eye.CandidateObjectToWorld3x4(),
					rt_pixel_size( 1.0f, view ), view ) )
				eye.CommitNonOpaqueTriangleHit();
		}
		sun_write( id.xy, eye.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f, 0.0f );
		return;
	}

	const float3 p_world = rt_to_world( rt_view_pos( float2( id.xy ) + 0.5f, z, view ), view );
	float3 n_world = normalize( rt_dir_to_world( s.n_view, view ) );
	float3 L = view.sun.xyz;
	[branch] if ( sun_params.y > 0.5f )
		L = rt_cone_dir( L, view.sun.w, rt_noise2( id.xy, view.scene.w ) );
	// The offset leaves the surface on the sun's side: leaf cards and grass are lit through from
	// behind, and their static shadow still comes from these rays.
	if ( dot( n_world, view.sun.xyz ) < 0.0f )
		n_world = -n_world;

	RayDesc ray;
	ray.Origin = p_world + n_world * ( sun_params.z + sun_params.w * z ) + L * ( 0.0005f * z );
	ray.Direction = L;
	ray.TMin = 0.02f;
	ray.TMax = sun_params.x;

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, RT_RAYS_SHADOW, ray );
	const float cone = rt_pixel_size( z, view );
	while ( q.Proceed() )
	{
		if ( q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			rt_alpha_solid( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(),
				q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone, view ) )
			q.CommitNonOpaqueTriangleHit();
	}
	const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
	sun_write( id.xy, hit ? 0.0f : 1.0f, hit ? q.CommittedRayT() : 0.0f );
}
