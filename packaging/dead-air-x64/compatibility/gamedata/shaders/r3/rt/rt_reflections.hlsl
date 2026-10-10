// Ray traced reflections of water and rain puddles (dxr.md 3.7). The water and puddle passes wrote
// a request per pixel (octahedral world reflection direction, view z of the reflecting surface,
// kind); one mirror ray per request against the whole scene. The frame copy (rt_SSR: lit, fogged,
// tonemapped, every local light included) answers first: a march along the ray in screen space finds
// what the frame shows in the ray's way before the traced hit - the grass, which is not in the scene,
// and crowns and reeds as they sway, which the scene holds in their rest pose. A traced hit the frame
// shows is taken from it too; one it does not show is shaded here into the same LDR space
// (rt_material.hlsli). A miss leaves alpha 0 and the consumers keep their sky. The result is
// premultiplied by its confidence. No denoiser: one deterministic ray per pixel.

#include "rt_material.hlsli"

cbuffer rt_reflection_constants : register( b0 )
{
	RtView	view;
	RtShade	shade;
	float4	refl;		// x TMax, y cone spread per metre, z screen-edge margin in pixels, w off-screen sun rays (0/1)
	uint4	refl_mask;	// x instance mask of the rays, y most screen-space steps of a ray
};

Texture2D<float4>	rf_request	: register( t0 );
Texture2D<float>	rf_depth	: register( t1 );
Texture2D<float4>	rf_scene	: register( t2 );
Texture2D<float>	rf_tonemap	: register( t3 );
TextureCube			rf_env0		: register( t4 );
TextureCube			rf_env1		: register( t5 );
TextureCube			rf_sky0		: register( t6 );
TextureCube			rf_sky1		: register( t7 );

RWTexture2D<float4>	rf_out		: register( u0 );

// The static world between the hit and the sun (the off-screen hit's only shadow).
float rf_sun_visibility( float3 p, float3 n, float cone )
{
	[branch] if ( refl.w < 0.5f || dot( n, view.sun.xyz ) <= 0.0f )
		return 1.0f;
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
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}

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

RfHit rf_trace( RayDesc ray, float cone0 )
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, refl_mask.x, ray );
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

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= view.size.xy ) )
		return;
	const float4 request = rf_request.Load( int3( id.xy, 0 ) );
	[branch] if ( request.w < 0.5f || request.z <= 0.0f )
	{
		rf_out[ id.xy ] = 0.0f;
		return;
	}

	const float3 dir = rt_oct_decode( request.xy );
	const float z = request.z;
	const float3 surface = rt_to_world( rt_view_pos( float2( id.xy ) + 0.5f, z, view ), view );
	// The ray cone starts as wide as a pixel at the surface and spreads by one pixel's angle.
	const float cone0 = rt_pixel_size( z, view );

	const float3 origin = surface + dir * ( 0.02f + 0.001f * z );
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.01f;
	ray.TMax = refl.x;
	RfHit h = rf_trace( ray, cone0 );
	// Where the raster drew the surface lower than the traced mesh (a coarser level of detail, the
	// fp16 depth of the G-buffer) the ray starts inside it and leaves through its back: that is the
	// surface itself, not its reflection. The ray goes on from past it.
	float skipped = 0.0f;
	[branch] if ( h.t >= 0.0f && h.t < 0.5f + 0.05f * z && rf_from_behind( h, dir ) )
	{
		skipped = h.t + 0.02f;
		ray.Origin = origin + dir * skipped;
		ray.TMax = max( refl.x - skipped, 0.02f );
		h = rf_trace( ray, cone0 + refl.y * skipped );
	}
	const bool traced = h.t >= 0.0f;
	// Along the ray from its first origin.
	const float t = traced ? skipped + h.t : refl.x;

	// What the frame shows in the ray's way, up to just short of the traced hit (judged below).
	const float3 seen_at = rf_march( origin, dir, traced ? t - max( 0.05f, 0.01f * t ) : t, id.xy );

	float3 color = 0.0f;
	float alpha = 0.0f;
	[branch] if ( traced && seen_at.z < 0.999f )
	{
		const float3 hit = origin + dir * t;

		// On screen: the frame copy, where the pixel there shows this very point.
		float reuse = 0.0f;
		float3 reused = 0.0f;
		{
			const float3 fp = rf_frame_pos( hit );
			[branch] if ( fp.z > 0.05f && all( fp.xy > 0.0f ) && all( fp.xy < 1.0f ) )
			{
				[branch] if ( abs( rf_frame_z( fp.xy ) - fp.z ) < max( 0.05f, 0.01f * fp.z ) )
				{
					reuse = rf_edge_fade( fp.xy );
					reused = rf_scene.SampleLevel( rt_linear_clamp, fp.xy, 0 ).rgb;
				}
			}
		}

		color = reused;
		[branch] if ( reuse < 0.999f )
		{
			const RtGeometry g = rt_geometry_of( h.instance_id, h.geometry_index );
			const RtInstance inst = rt_instance_of( h.instance_index, view );
			const RtTriangle tri = rt_fetch( g, h.primitive );
			const float cone = cone0 + refl.y * t;
			const RtHitSurface hs = rt_hit_surface( g, inst, tri, h.bary, h.o2w, dir, cone, view );
			const float sun = rf_sun_visibility( hit, hs.normal, cone );
			float3 lit = rt_hit_light( hs, sun, shade, rf_env0, rf_env1, view );
			lit = rt_fog_apply( lit, hit, view.camera.xyz, shade, rf_sky0, rf_sky1 );
			const float exposure = rf_tonemap.Load( int3( 0, 0, 0 ) );
			lit = rt_tonemap( lit, exposure, shade );
			color = lerp( lit, reused, reuse );
		}
		alpha = 1.0f;
	}
	[branch] if ( seen_at.z > 0.0f )
	{
		const float3 seen = rf_scene.SampleLevel( rt_linear_clamp, seen_at.xy, 0 ).rgb;
		color = lerp( color, seen, seen_at.z );
		alpha = lerp( alpha, 1.0f, seen_at.z );
	}
	rf_out[ id.xy ] = float4( color, alpha );
}
