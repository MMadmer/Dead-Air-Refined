#ifndef	rt_reflect_hlsli_included
#define	rt_reflect_hlsli_included

// What the reflection passes share (rt_reflections.hlsl, rt_gloss.hlsl, rt_gi.hlsl): the mirror ray with its
// alpha-tested candidates, the hit seen from behind, where the frame shows a point and the screen-space march along a
// ray. The includer declares `view` (RtView), `refl` (x TMax, y cone spread per metre, z screen-edge margin in pixels,
// w off-screen sun rays), `refl_mask` (x instance mask, y most screen-space steps) and `rf_depth` (the depth buffer),
// and with RF_CLOUDS `clouds` (RtClouds) and `rf_clouds` (the cloud map): the sun at a hit then passes the deck; with
// RF_LIGHTS `lights` (RtLights) and the world cache of the traced light as the raw output: an off-screen hit then takes
// the lamps' light (one shadow ray) and the light the cache holds around it.

#include "rt_material.hlsli"
#ifdef RF_LIGHTS
#include "rt_cache.hlsli"
#endif

// The world between the hit and the sun (the off-screen hit's shadow), and the clouds' shadow over it.
float rf_sun_visibility( float3 p, float3 n, float cone )
{
	float clouds_t = 1.0f;
#ifdef RF_CLOUDS
	clouds_t = rt_cloud_transmittance( p, view.sun.xyz, clouds, rf_clouds );
#endif
	[branch] if ( refl.w < 0.5f || dot( n, view.sun.xyz ) <= 0.0f )
		return clouds_t;
	RayDesc ray;
	ray.Origin = p + n * 0.03f;
	ray.Direction = view.sun.xyz;
	ray.TMin = 0.02f;
	ray.TMax = 1000.0f;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, RT_RAYS_SHADOW, ray );
	while ( q.Proceed() )
	{
		if ( q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			rt_alpha_solid( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(),
				q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone, view ) )
			q.CommitNonOpaqueTriangleHit();
	}
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : clouds_t;
}

#ifdef RF_LIGHTS
// One shadow ray toward a lamp: 1 where it reaches it (its own fixture is not in the way).
float rf_lamp_visible( float3 p, float3 n, RtLight L, float cone )
{
	const float3 to_lamp = L.pos.xyz - p;
	const float dist = length( to_lamp );
	RayDesc ray;
	ray.Direction = to_lamp / max( dist, 1e-4f );
	ray.Origin = p + n * 0.03f;
	ray.TMin = 0.02f;
	ray.TMax = max( dist - 0.15f, 0.03f );
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, L.color.w >= 1.5f ? RT_RAYS_SHADOW_LAMP_CARRIED : RT_RAYS_SHADOW, ray );
	while ( q.Proceed() )
	{
		if ( q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			rt_alpha_solid( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(),
				q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone, view ) )
			q.CommitNonOpaqueTriangleHit();
	}
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}

// The lamps' light at an off-screen hit: every lamp's unshadowed light, one of them chosen by its share (u in [0, 1))
// for a shadow ray that stands for all - unbiased over the frames the temporal passes average.
float3 rf_hit_lamps( float3 p, float3 n, float cone, float u )
{
	const uint count = min( lights.count.x, 32u );
	[branch] if ( count == 0u )
		return 0.0f;
	float total = 0.0f;
	float3 sum = 0.0f;
	[loop] for ( uint i = 0u; i < count; ++i )
	{
		const float3 c = rt_lamp_light( lights.light[ i ], p, n, lights.falloff );
		sum += c;
		total += dot( c, float3( 0.2126f, 0.7152f, 0.0722f ) );
	}
	[branch] if ( total <= 1e-5f )
		return 0.0f;
	const float pick = u * total;
	float run = 0.0f;
	uint chosen = 0u;
	float3 chosen_c = 0.0f;
	float chosen_w = 0.0f;
	[loop] for ( uint j = 0u; j < count; ++j )
	{
		const float3 c = rt_lamp_light( lights.light[ j ], p, n, lights.falloff );
		const float w = dot( c, float3( 0.2126f, 0.7152f, 0.0722f ) );
		[branch] if ( w > 0.0f )
		{
			chosen = j;
			chosen_c = c;
			chosen_w = w;
			run += w;
			[branch] if ( run >= pick )
				break;
		}
	}
	const float visible = rf_lamp_visible( p, n, lights.light[ chosen ], cone );
	// The chosen lamp's light over its probability (its share of the total).
	return visible * chosen_c * ( total / max( chosen_w, 1e-6f ) );
}

// The lamps' light at an off-screen hit for a pass with no temporal accumulation (the water's mirror): the first
// `traced` lamps that light it (the list is the brightest first) each by its own shadow ray, the rest unshadowed - no
// frame-to-frame noise.
float3 rf_hit_lamps_each( float3 p, float3 n, float cone, uint traced )
{
	const uint count = min( lights.count.x, 32u );
	float3 sum = 0.0f;
	uint rays = 0u;
	[loop] for ( uint i = 0u; i < count; ++i )
	{
		float3 c = rt_lamp_light( lights.light[ i ], p, n, lights.falloff );
		[branch] if ( any( c > 0.0f ) && rays < traced )
		{
			c *= rf_lamp_visible( p, n, lights.light[ i ], cone );
			++rays;
		}
		sum += c;
	}
	return sum;
}

// The light around an off-screen hit the world cache holds (a 0: none, or the cache is not live).
float4 rf_hit_around( float3 p, float3 n )
{
	[branch] if ( lights.count.y == 0u )
		return 0.0f;
	return rt_cache_read( p, n, view.camera.xyz );
}
#endif

// ---- the traced ray ----------------------------------------------------------------------------

// What the shading needs of a committed hit once its query is gone.
struct RfHit
{
	float		t;				// < 0: the ray missed
	uint		instance_id;
	uint		geometry_index;
	uint		primitive;
	uint		instance_index;
	float2		bary;
	float3x4	o2w;
};

RfHit rf_trace_masked( RayDesc ray, float cone0, uint mask )
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, mask, ray );
	while ( q.Proceed() )
	{
		if ( q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE )
			continue;
		const float cone = cone0 + refl.y * q.CandidateTriangleRayT();
		if ( rt_alpha_solid( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(),
			q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone, view ) )
			q.CommitNonOpaqueTriangleHit();
	}
	RfHit h = (RfHit)0;
	h.t = -1.0f;
	[branch] if ( q.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
	{
		h.t = q.CommittedRayT();
		h.instance_id = q.CommittedInstanceID();
		h.geometry_index = q.CommittedGeometryIndex();
		h.primitive = q.CommittedPrimitiveIndex();
		h.instance_index = q.CommittedInstanceIndex();
		h.bary = q.CommittedTriangleBarycentrics();
		h.o2w = q.CommittedObjectToWorld3x4();
	}
	return h;
}

RfHit rf_trace( RayDesc ray, float cone0 ) { return rf_trace_masked( ray, cone0, refl_mask.x ); }

// A hit on the back of a solid surface (the ray runs the way its vertex normal faces). Cut-out cards
// are two-sided and never count.
bool rf_from_behind( RfHit h, float3 dir )
{
	const RtGeometry g = rt_geometry_of( h.instance_id, h.geometry_index );
	[branch] if ( g.flags & RT_GEOM_ALPHA )
		return false;
	const RtTriangle tri = rt_fetch( g, h.primitive );
	const float3 w = rt_bary3( h.bary );
	const float3 n = mul( (float3x3)h.o2w, tri.v0.normal * w.x + tri.v1.normal * w.y + tri.v2.normal * w.z );
	return dot( n, dir ) > 0.0f;
}

// ---- the frame --------------------------------------------------------------------------------

// Where this frame's textures show a world point (uv; they were drawn with the jittered projection)
// and its view z.
float3 rf_frame_pos( float3 p )
{
	const float3 s = rt_project( p, view.vp );
	return float3( s.xy + view.frame.xy * view.size.zw, s.z );
}

// The view z the frame shows at uv; the sky is infinitely far.
float rf_frame_z( float2 uv )
{
	const float d = rf_depth.SampleLevel( rt_point_clamp, uv, 0 );
	return rt_is_sky( d ) ? 1e6f : rt_linear_z( d, view );
}

// 1 inside the frame, falling to 0 over the band at its border (past the border nothing is known).
float rf_edge_fade( float2 uv )
{
	const float2 px = uv * view.size.xy;
	const float2 edge = min( px - refl.z, view.size.xy - refl.z - px ) / ( 4.0f * refl.z );
	return saturate( min( edge.x, edge.y ) );
}

// Where p + d * s leaves [0, 1] on one axis.
float rf_exit( float p, float d )
{
	return d > 1e-7f ? ( 1.0f - p ) / d : ( d < -1e-7f ? -p / d : 1e30f );
}

// The first surface the frame shows in the way of the ray from `origin` to `t_end`: its uv (xy) and
// weight (z, 0 = none). Uniform steps on the screen with 1/z linear along them (perspective-correct);
// a step that takes the ray behind the frame's depth is bisected, and the crossing counts where the
// ray meets that surface within its thickness: a ray passing well behind a blade or a post goes on.
float3 rf_march( float3 origin, float3 dir, float t_end, uint2 pixel )
{
	const float3 a = rf_frame_pos( origin );
	// A ray coming toward the camera stops short of it: behind the camera the projection flips.
	const float dz = rt_dir_to_view( dir, view ).z;
	[flatten] if ( dz < 0.0f )
		t_end = min( t_end, ( min( 0.2f, 0.5f * a.z ) - a.z ) / dz );
	[branch] if ( t_end <= 0.0f )
		return 0.0f;
	const float3 b = rf_frame_pos( origin + dir * t_end );
	const float3 p0 = float3( a.xy, 1.0f / a.z );
	const float3 dp = float3( b.xy, 1.0f / b.z ) - p0;
	const float s_end = min( 1.0f, min( rf_exit( p0.x, dp.x ), rf_exit( p0.y, dp.y ) ) );
	const float pixels = length( dp.xy * view.size.xy ) * s_end;
	[branch] if ( s_end <= 0.0f || pixels < 2.0f )
		return 0.0f;
	const uint steps = clamp( uint( pixels * 0.25f ), 4u, max( refl_mask.y, 4u ) );
	const float ds = s_end / float( steps );
	// Clear of the origin's own pixel; a per-pixel offset of the strides breaks their banding.
	const float s_first = 1.5f * s_end / pixels;
	const float offset = rt_ign( float2( pixel ) );
	float s_prev = 0.0f;
	bool front = true;
	[loop] for ( uint i = 0u; i < steps; ++i )
	{
		const float s = min( s_first + ds * ( float( i ) + offset ), s_end );
		const float3 p = p0 + dp * s;
		const bool behind = rf_frame_z( p.xy ) < 1.0f / p.z;
		[branch] if ( behind && front )
		{
			float lo = s_prev, hi = s;
			[unroll] for ( uint k = 0u; k < 5u; ++k )
			{
				const float m = 0.5f * ( lo + hi );
				const float3 pm = p0 + dp * m;
				[flatten] if ( rf_frame_z( pm.xy ) < 1.0f / pm.z )
					hi = m;
				else
					lo = m;
			}
			const float3 ph = p0 + dp * hi;
			const float zr = 1.0f / ph.z;
			const float zs = rf_frame_z( ph.xy );
			// The first-person HUD, far nearer than the reflecting surface, is never in a ray's way.
			[branch] if ( zr - zs < 0.05f + 0.01f * zr && zs > 0.5f * a.z )
				return float3( ph.xy, rf_edge_fade( ph.xy ) );
		}
		front = !behind;
		s_prev = s;
	}
	return 0.0f;
}

#endif	//	rt_reflect_hlsli_included
