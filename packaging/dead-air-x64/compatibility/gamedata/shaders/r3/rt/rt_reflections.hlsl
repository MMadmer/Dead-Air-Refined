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

#include "rt_reflect.hlsli"

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
