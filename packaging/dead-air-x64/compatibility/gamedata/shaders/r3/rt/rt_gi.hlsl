// Ray traced diffuse global illumination (dxr.md 3.11). Rays leave every traced pixel's surface cosine-weighted
// about its normal; what they bring back is the light the surface receives from its surroundings:
//  - a ray that reaches the sky brings the sky's light the combine's hemisphere term gives the surface's normal
//    (hmodel.h env_d: the cube is already the light of the open world summed over a hemisphere, the ground below the
//    horizon included, so sampling it along every ray would sum it twice), weighted by the share of the rays that
//    would reach the sky above open flat ground, (1 + n.y) / 2 - in the open the result is the stock sky light, under
//    a roof, a crown or an overhang the share of it that really gets in;
//  - a ray that meets the world takes what lights the point it met: the previous frame's linear colour where the screen
//    shows that point (every lamp, the fire, the torch and their own bounces come with it), else the point's albedo
//    under the sun (its own shadow ray and the clouds' shadow) and the sky over its own hemisphere, plus its glow.
// Half resolution traces one pixel of each 2x2 block a frame, the four in turn (rt_gi_temporal.hlsl rebuilds the
// full resolution); full resolution traces every pixel. Output: the mean radiance (the combine's diffuse ambient) and
// the share of rays that reached the sky (the combine's sky specular takes it in place of the baked hemisphere).

#include "rt_material.hlsli"
#include "da_material_codes.h"

cbuffer rt_gi_constants : register( b0 )
{
	RtView	view;
	RtShade	shade;
	float4	refl;		// x TMax, y cone spread per metre, z screen-edge margin in pixels, w off-screen sun rays (0/1)
	uint4	refl_mask;	// x instance mask of the rays, y rays a traced pixel, zw trace size
	float4	gi;			// x half resolution (1) or full (0), y HDR scale of the frame copy, z the sky's share of a hit's
						// own light (its vertex hemisphere), w the glow of emissive hits
	float4	gi2;		// x the light a crown lets through (1: the crowns are not tested), y their test's reach
	RtClouds clouds;
};

Texture2D<float>	rf_depth	: register( t0 );
Texture2D<float4>	gi_position	: register( t1 );
Texture2D<float4>	gi_normal	: register( t2 );
Texture2D<float4>	gi_scene	: register( t3 );
TextureCube			gi_env0		: register( t4 );
TextureCube			gi_env1		: register( t5 );
Texture2D<float4>	rf_clouds	: register( t6 );
TextureCube			gi_sky0		: register( t7 );
TextureCube			gi_sky1		: register( t8 );

RWTexture2D<float4>	gi_out		: register( u0 );
RWTexture2D<float>	gi_z		: register( u1 );

#define RF_CLOUDS
#include "rt_reflect.hlsli"

// The pixel of a 2x2 block this frame traces (rt_gi_temporal.hlsl takes the same); every pixel at full resolution.
uint2 gi_trace_pixel( uint2 id )
{
	[branch] if ( gi.x < 0.5f )
		return id;
	const uint f = view.scene.w & 3u;
	const uint2 offset = uint2( f & 1u, ( ( f >> 1 ) ^ f ) & 1u );
	return min( id * 2u + offset, uint2( view.size.xy ) - 1u );
}

// The previous frame's light of a world point the current frame shows (weight 0 off its screen or without a copy):
// the copy is fogged from the camera, as the combine leaves it; the haze comes off again (a deep haze leaves too
// little of the point: its weight fades and the hit is shaded instead).
float4 gi_previous( float3 p )
{
	const float3 prev = rt_project( p, view.vp_prev );
	[branch] if ( gi.y <= 0.0f || prev.z <= 0.05f || any( prev.xy <= 0.0f ) || any( prev.xy >= 1.0f ) )
		return 0.0f;
	const float3 c = gi_scene.SampleLevel( rt_linear_clamp, prev.xy, 0 ).rgb * gi.y;
	float3 fog_rgb;
	const float fog = rt_fog_at( p, view.camera.xyz, shade, gi_sky0, gi_sky1, fog_rgb );
	const float3 lit = max( ( c - fog_rgb * fog ) / max( 1.0f - fog, 0.05f ), 0.0f );
	return float4( lit, rf_edge_fade( prev.xy ) * saturate( ( 0.85f - fog ) / 0.2f ) );
}

// The crowns in a ray's way, as whole cards (no alpha test, the first one ends it): the light a canopy lets through.
// The rays proper pass the crowns - their alpha test along every ray cost more than everything else together.
float gi_canopy( float3 origin, float3 dir, float tmax )
{
	[branch] if ( gi2.x >= 0.999f )
		return 1.0f;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.05f;
	ray.TMax = tmax;
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, RT_MASK_FOLIAGE, ray );
	q.Proceed();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? gi2.x : 1.0f;
}

// The light a ray brings back: (radiance, reached the sky). sky: the open sky's light for the receiver's normal. A
// blade, a leaf or a twig sits inside its own plant's stand-in (the scene holds the stock model where the procedural
// one is drawn): its rays pass the stand-ins' trunks and crowns, its own plant's shade is its baked hemisphere.
float4 gi_ray( float3 origin, float3 dir, float cone0, float3 sky, bool foliage )
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.01f;
	ray.TMax = refl.x;
	const RfHit h = rf_trace_masked( ray, cone0, foliage ? ( refl_mask.x & ~RT_MASK_TRUNK ) : refl_mask.x );
	[branch] if ( h.t < 0.0f )
	{
		// Below the horizon a miss is the far ground under a drop (a high wall, an eave over a street, a cliff): the
		// cube's own lower half, no sky of the open-ground share.
		[branch] if ( dir.y <= 0.0f )
			return float4( rt_sky_diffuse( dir, shade, gi_env0, gi_env1 ), 0.0f );
		const float canopy = foliage ? 1.0f : gi_canopy( origin, dir, gi2.y );
		return float4( sky * canopy, canopy );
	}

	const float3 hit = origin + dir * h.t;
	// Where the screen shows the point: what lights it there, everything included.
	float4 reused = 0.0f;
	{
		const float3 fp = rf_frame_pos( hit );
		[branch] if ( fp.z > 0.05f && all( fp.xy > 0.0f ) && all( fp.xy < 1.0f ) &&
			abs( rf_frame_z( fp.xy ) - fp.z ) < max( 0.05f, 0.01f * fp.z ) )
		{
			reused = gi_previous( hit );
			reused.w *= rf_edge_fade( fp.xy );
		}
	}
	[branch] if ( reused.w >= 0.999f )
		return float4( reused.rgb, 0.0f );

	const RtGeometry geo = rt_geometry_of( h.instance_id, h.geometry_index );
	const RtInstance inst = rt_instance_of( h.instance_index, view );
	const RtTriangle tri = rt_fetch( geo, h.primitive );
	const float cone = cone0 + refl.y * h.t;
	const RtHitSurface hs = rt_hit_surface( geo, inst, tri, h.bary, h.o2w, dir, cone, view );
	// A wall's inside seen through its back: it lights nothing toward the receiver.
	[branch] if ( !( geo.flags & RT_GEOM_ALPHA ) && rf_from_behind( h, dir ) )
		return float4( lerp( 0.0f, reused.rgb, reused.w ), 0.0f );
	float sun = 0.0f;
	[branch] if ( dot( hs.normal, view.sun.xyz ) > 0.0f )
	{
		sun = rf_sun_visibility( hit, hs.normal, cone );
		[branch] if ( sun > 0.0f )
			sun *= gi_canopy( hit + hs.normal * 0.03f, view.sun.xyz, gi2.y );
	}
	RtHitSurface lit_surface = hs;
	lit_surface.hemi *= gi.z;
	float3 lit = rt_hit_light( lit_surface, sun, shade, gi_env0, gi_env1, view );
	[flatten] if ( hs.emissive )
		lit += hs.albedo * ( gi.w - 1.0f );
	return float4( lerp( lit, reused.rgb, reused.w ), 0.0f );
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( id.xy >= refl_mask.zw ) )
		return;
	const uint2 pixel = gi_trace_pixel( id.xy );
	const float depth = rf_depth.Load( int3( pixel, 0 ) );
	[branch] if ( rt_is_sky( depth ) )
	{
		gi_out[ id.xy ] = float4( 0.0f, 0.0f, 0.0f, 1.0f );
		gi_z[ id.xy ] = view.proj.w;
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( gi_position, gi_normal, int2( pixel ), view );
	gi_z[ id.xy ] = z;
	// The first-person hands are not where the main projection puts them: the combine keeps their stock ambient.
	[branch] if ( rt_is_hud( z, s ) )
	{
		gi_out[ id.xy ] = float4( 0.0f, 0.0f, 0.0f, -1.0f );
		return;
	}

	const float w_mtl = gi_position.Load( int3( pixel, 0 ) ).w;
	const bool foliage = da_is_foliage( ( view.scene.z & RT_VIEW_GBUFFER_PACKED ) ? gbuf_unpack_mtl( w_mtl ) : abs( w_mtl ) );
	const float3 surface = rt_to_world( rt_view_pos( float2( pixel ) + 0.5f, z, view ), view );
	const float3 n = normalize( rt_dir_to_world( s.n_view, view ) );
	const float3 origin = surface + n * ( 0.02f + 0.001f * z );
	const float cone0 = rt_pixel_size( z, view );
	const uint rays = max( refl_mask.y, 1u );
	const float2 noise = rt_noise2( pixel, view.scene.w );
	// A ray that reaches the sky stands for the open world's share of it (a wall sees half the sky in a field).
	const float open = max( 0.5f + 0.5f * n.y, 0.2f );
	const float3 sky = rt_sky_diffuse( n, shade, gi_env0, gi_env1 ) / open;
	float4 sum = 0.0f;
	[loop] for ( uint i = 0u; i < rays; ++i )
	{
		const float2 u = frac( noise + float2( 0.7548776662f, 0.5698402910f ) * float( i ) );
		sum += gi_ray( origin, rt_cosine_dir( n, u ), cone0, sky, foliage );
	}
	sum /= float( rays );
	// The sky seen, as the baked hemisphere means it: 1 in the open.
	sum.w = min( sum.w / open, 1.0f );
	gi_out[ id.xy ] = sum;
}
