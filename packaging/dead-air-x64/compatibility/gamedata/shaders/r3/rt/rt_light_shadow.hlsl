// Ray traced shadows of the local lamps (dxr.md 3.12, 3.15): every lamp the tier traces in one pass, four lamps a slice of
// one array, which the lamps' volumes read in place of their shadow maps - or of no shadow at all, for the lamps the stock
// lit unshadowed (da_light_volume.ps). A ray leaves every render pixel's surface toward a point of the lamp's bulb (its
// radius jittered frame by frame, so the temporal antialiasing softens the edge the way the bulb's size does) and stops
// short of the lamp's own fixture. The scene's static world, moving objects and characters cast; a lamp the player
// carries (the torch above all) is not shadowed by his own body, as its map was not. The first-person hands and what they
// hold trace from where they are in the world. A surface facing away from a lamp is dark to it already and traces nothing
// (leaf cards and blades lit through from behind, and snow, whose light wraps past the terminator, do).

#include "rt_geometry.hlsli"
#include "da_material_codes.h"

cbuffer rt_light_constants : register( b0 )
{
	RtView	view;
	float4	lamps[ 32 ];		// xyz world position, w 1 / range^2
	float4	lamp_info[ 32 ];	// x the bulb's radius, y the fixture's reach the ray stops short of, z carried by the player (0/1)
	uint4	lamp_count;			// x lamps, y rays a lamp, z slices
};

Texture2D<float>	ls_depth	: register( t0 );
Texture2D<float4>	ls_position	: register( t1 );
Texture2D<float4>	ls_normal	: register( t2 );

RWTexture2DArray<unorm float4>	ls_out	: register( u0 );

// One shadow ray: 1 where it reaches the lamp.
float ls_ray( float3 origin, float3 to_lamp, float tmax, uint mask, float cone )
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = to_lamp;
	ray.TMin = 0.01f;
	ray.TMax = tmax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, mask, ray );
	while ( q.Proceed() )
	{
		if ( q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			rt_alpha_solid( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(),
				q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone, view ) )
			q.CommitNonOpaqueTriangleHit();
	}
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}

void ls_write_all( uint2 pixel, float4 v )
{
	[loop] for ( uint m = 0u; m < lamp_count.z; ++m )
		ls_out[ uint3( pixel, m ) ] = v;
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	const float depth = ls_depth.Load( int3( id.xy, 0 ) );
	[branch] if ( rt_is_sky( depth ) )
	{
		ls_write_all( id.xy, 1.0f );
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( ls_position, ls_normal, int2( id.xy ), view );
	const bool hud = rt_is_hud( depth, view );
	const float3 p = hud ? rt_hud_world( float2( id.xy ) + 0.5f, s.z, view ) :
		rt_to_world( rt_view_pos( float2( id.xy ) + 0.5f, z, view ), view );
	const float z_ray = hud ? s.z : z;
	const float3 n = normalize( rt_dir_to_world( s.n_view, view ) );
	const float w_mtl = ls_position.Load( int3( id.xy, 0 ) ).w;
	const float mtl = ( view.scene.z & RT_VIEW_GBUFFER_PACKED ) ? gbuf_unpack_mtl( w_mtl ) : abs( w_mtl );
	// Leaves and blades are lit through from behind, snow wraps its light past the terminator (da_snow_light).
	const bool through = !hud && ( da_is_foliage( mtl ) || da_is_snowy( mtl ) );
	const float cone = rt_pixel_size( z_ray, view );
	const uint rays = max( lamp_count.y, 1u );
	const uint count = min( lamp_count.x, 32u );
	const float2 noise = rt_noise2( id.xy, view.scene.w );

	[loop] for ( uint m = 0u; m < lamp_count.z; ++m )
	{
		float4 visibility = 1.0f;
		[loop] for ( uint k = 0u; k < 4u; ++k )
		{
			const uint i = m * 4u + k;
			[branch] if ( i >= count )
				break;
			const float3 to_lamp = lamps[ i ].xyz - p;
			const float d2 = dot( to_lamp, to_lamp );
			[branch] if ( d2 * lamps[ i ].w >= 1.0f )
				continue;
			const float dist = sqrt( d2 );
			const float3 L = to_lamp / max( dist, 1e-4f );
			const float facing = dot( n, L );
			[branch] if ( facing < -0.05f && !through )
			{
				visibility[ k ] = 0.0f;
				continue;
			}
			// Off the surface on the lamp's side; leaf cards lit from behind keep their shadow.
			const float3 side = facing >= 0.0f ? n : -n;
			const float3 origin = p + side * ( ( hud ? 0.004f : 0.01f ) + 0.0015f * z_ray );
			// The hands sit inside the player's own shadow model, which shadows no lamp they are lit by.
			const uint mask = ( hud || lamp_info[ i ].z > 0.5f ) ? RT_RAYS_SHADOW_LAMP_CARRIED : RT_RAYS_SHADOW;
			const float tmax = max( dist - lamp_info[ i ].y, 0.02f );
			float lit = 0.0f;
			[loop] for ( uint r = 0u; r < rays; ++r )
			{
				float3 dir = L;
				[branch] if ( lamp_info[ i ].x > 0.0f )
				{
					const float2 u = frac( noise + float2( 0.7548776662f, 0.5698402910f ) * float( r + i * 3u ) );
					dir = rt_cone_dir( L, lamp_info[ i ].x / max( dist, 0.05f ), u );
				}
				lit += ls_ray( origin, dir, tmax, mask, cone );
			}
			visibility[ k ] = lit / float( rays );
		}
		ls_out[ uint3( id.xy, m ) ] = visibility;
	}
}
