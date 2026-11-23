// Ray traced sun shadow (dxr.md 3.5, 3.10): rays per render pixel toward the sun against the static world, the
// moving objects and characters near the camera and the player's shadow model (RT_RAYS_SHADOW); vegetation keeps its
// swaying shadow maps. The near and middle cascades leave the traced casters out while this runs; the accumulation
// multiplies the two.
//
// RT_SUN_DIRECT (quality Low): a hard shadow straight into the mask the sun passes read, no jitter, no filter.
// Otherwise (visibility, hit distance) for the temporal pass and the filter; the hit distance is negative where a moving
// object casts, so the temporal pass follows it instead of trailing its shadow.

#include "rt_geometry.hlsli"

cbuffer rt_sun_constants : register( b0 )
{
	RtView	view;
	float4	sun_params;	// x TMax, y rays a pixel (0 = one, unjittered), z normal offset base, w normal offset per metre
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

// One shadow ray: (blocked 0/1, hit distance, a moving object blocked it 0/1).
float3 sun_ray( RayDesc ray, float cone, uint mask )
{
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, mask, ray );
	while ( q.Proceed() )
	{
		if ( q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			rt_alpha_solid( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(),
				q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone, view ) )
			q.CommitNonOpaqueTriangleHit();
	}
	[branch] if ( q.CommittedStatus() != COMMITTED_TRIANGLE_HIT )
		return 0.0f;
	return float3( 1.0f, q.CommittedRayT(), ( q.CommittedInstanceID() & RT_INSTANCE_DYNAMIC ) ? 1.0f : 0.0f );
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
	// The hands are not in the scene: they take the sun where they are in the world (a barrel out of a doorway's shadow
	// is lit). Written as lit, they shone in full sun under any roof, since the cascades leave the static casters to
	// these rays. The rays pass the player's own shadow model, which the hands sit inside.
	[branch] if ( rt_is_hud( depth, view ) )
	{
		float3 n_hud = normalize( rt_dir_to_world( s.n_view, view ) );
		if ( dot( n_hud, view.sun.xyz ) < 0.0f )
			n_hud = -n_hud;
		RayDesc hud_ray;
		hud_ray.Origin = rt_hud_world( float2( id.xy ) + 0.5f, s.z, view ) + n_hud * 0.01f;
		hud_ray.Direction = view.sun.xyz;
		hud_ray.TMin = 0.02f;
		hud_ray.TMax = sun_params.x;
		const float3 h = sun_ray( hud_ray, rt_pixel_size( 1.0f, view ), RT_RAYS_SHADOW_EYE );
		sun_write( id.xy, 1.0f - h.x, 0.0f );
		return;
	}

	const float3 p_world = rt_to_world( rt_view_pos( float2( id.xy ) + 0.5f, z, view ), view );
	float3 n_world = normalize( rt_dir_to_world( s.n_view, view ) );
	// The offset leaves the surface on the sun's side: leaf cards and grass are lit through from
	// behind, and their static shadow still comes from these rays.
	if ( dot( n_world, view.sun.xyz ) < 0.0f )
		n_world = -n_world;
	const float cone = rt_pixel_size( z, view );
	const uint rays = max( uint( sun_params.y + 0.5f ), 1u );
	const float2 noise = rt_noise2( id.xy, view.scene.w );

	float blocked = 0.0f, hit_min = 1e9f, moving = 0.0f;
	[loop] for ( uint i = 0u; i < rays; ++i )
	{
		float3 L = view.sun.xyz;
		// Points of the sun's disc, a golden-ratio step apart from the pixel's noise.
		[branch] if ( sun_params.y > 0.5f )
			L = rt_cone_dir( L, view.sun.w, frac( noise + float2( 0.7548776662f, 0.5698402910f ) * float( i ) ) );
		RayDesc ray;
		ray.Origin = p_world + n_world * ( sun_params.z + sun_params.w * z ) + L * ( 0.0005f * z );
		ray.Direction = L;
		ray.TMin = 0.02f;
		ray.TMax = sun_params.x;
		const float3 h = sun_ray( ray, cone, RT_RAYS_SHADOW );
		blocked += h.x;
		[flatten] if ( h.x > 0.5f )
			hit_min = min( hit_min, h.y );
		moving = max( moving, h.z );
	}
	const float visibility = 1.0f - blocked / float( rays );
	const float hit = blocked > 0.0f ? hit_min : 0.0f;
	sun_write( id.xy, visibility, moving > 0.5f ? -max( hit, 0.001f ) : hit );
}
