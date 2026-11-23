// Ray traced diffuse global illumination (dxr.md 3.11, 3.15): the light that bounces. The combine keeps the stock
// ambient - the sky over the lightmap's baked hemisphere and the ambient fill, the level's art - and adds what a bake
// cannot know: the light of the sun, the lamps, the fires and the player's torch coming back off what they lit, colour
// and all, and moving with them. Rays leave every traced pixel's surface cosine-weighted about its normal:
//  - a ray that meets the world brings back the light leaving the point it met: its albedo times the direct light on it
//    - this frame's accumulated lights where the screen shows the point (every lamp with its shadow and projector, the
//    sun with its cascades), else the sun by its own shadow ray (and the clouds' shadow) and the frame's lamps (one
//    shadow ray for them all) - plus the bounced light the world cache holds there (the bounces before), plus its glow;
//  - a ray that reaches the sky counts toward the share of the sky the surface sees (the combine's sky specular takes it
//    in place of the baked hemisphere; a crown's card in the way lets 30 % through) and brings no light (the stock
//    ambient has the sky).
// The mean over the rays is the bounced light in the combine's units (D * light, as the accumulator's). The first-person
// hands and what they hold trace from where they are in the world, as the world's pixels do. Half resolution traces one
// pixel of each 2x2 block a frame, the four in turn (rt_gi_temporal.hlsl rebuilds the full resolution); full resolution
// traces every pixel.

#include "rt_material.hlsli"
#include "da_material_codes.h"

cbuffer rt_gi_constants : register( b0 )
{
	RtView	view;
	RtShade	shade;
	float4	refl;		// x TMax, y cone spread per metre, z screen-edge margin in pixels, w off-screen sun rays (0/1)
	uint4	refl_mask;	// x instance mask of the rays, y rays a traced pixel, zw trace size
	float4	gi;			// x half resolution (1) or full (0), y the accumulated lights are bound (0/1), z -, w the glow of
						// emissive hits
	float4	gi2;		// x the light a crown lets through (1: the crowns are not tested), y their test's reach
	RtClouds clouds;
	RtLights lights;
};

Texture2D<float>	rf_depth	: register( t0 );
Texture2D<float4>	gi_position	: register( t1 );
Texture2D<float4>	gi_normal	: register( t2 );
Texture2D<float4>	gi_accum	: register( t3 );
TextureCube			gi_env0		: register( t4 );
TextureCube			gi_env1		: register( t5 );
Texture2D<float4>	rf_clouds	: register( t6 );
TextureCube			gi_sky0		: register( t7 );
TextureCube			gi_sky1		: register( t8 );
Texture2D<float4>	gi_albedo	: register( t9 );

RWTexture2D<float4>	gi_out		: register( u0 );
RWTexture2D<float>	gi_z		: register( u1 );

#define RF_CLOUDS
#define RF_LIGHTS
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

// The crowns in a ray's way, as whole cards (no alpha test, the first one ends it): the sky a canopy lets through.
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

// The light a ray brings back: (bounced light, reached the sky). A blade, a leaf or a twig sits inside its own plant's
// stand-in (the scene holds the stock model where the procedural one is drawn): its rays pass the stand-ins' trunks and
// crowns.
float4 gi_ray( float3 origin, float3 dir, float cone0, bool foliage, float u )
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.01f;
	ray.TMax = refl.x;
	const RfHit h = rf_trace_masked( ray, cone0, foliage ? ( refl_mask.x & ~RT_MASK_TRUNK ) : refl_mask.x );
	[branch] if ( h.t < 0.0f )
	{
		// Below the horizon a miss is the far ground under a drop: no sky.
		[branch] if ( dir.y <= 0.0f )
			return 0.0f;
		return float4( 0.0f, 0.0f, 0.0f, foliage ? 1.0f : gi_canopy( origin, dir, gi2.y ) );
	}

	const float3 hit = origin + dir * h.t;
	const RtGeometry geo = rt_geometry_of( h.instance_id, h.geometry_index );
	// A wall's inside seen through its back: it sends nothing toward the receiver.
	[branch] if ( !( geo.flags & RT_GEOM_ALPHA ) && rf_from_behind( h, dir ) )
		return 0.0f;
	const RtInstance inst = rt_instance_of( h.instance_index, view );
	const RtTriangle tri = rt_fetch( geo, h.primitive );
	const float cone = cone0 + refl.y * h.t;
	const RtHitSurface hs = rt_hit_surface( geo, inst, tri, h.bary, h.o2w, dir, cone, view );
	const float4 around = rf_hit_around( hit, hs.normal );

	// Where the screen shows the point this frame: its albedo and the lights the accumulator gathered on it.
	[branch] if ( gi.y > 0.5f )
	{
		const float3 fp = rf_frame_pos( hit );
		[branch] if ( fp.z > 0.05f && all( fp.xy > 0.0f ) && all( fp.xy < 1.0f ) &&
			abs( rf_frame_z( fp.xy ) - fp.z ) < max( 0.05f, 0.01f * fp.z ) )
		{
			const int3 px = int3( fp.xy * view.size.xy, 0 );
			const float3 albedo = gi_albedo.Load( px ).rgb;
			const float3 direct = max( gi_accum.Load( px ).rgb, 0.0f );
			return float4( albedo * ( direct + around.rgb ), 0.0f );
		}
	}

	// Off the screen: the sun by its own ray (as the sun's accumulation lights: the cosine, LIGHT_MUL1) and the lamps.
	float sun = 0.0f;
	[branch] if ( dot( hs.normal, view.sun.xyz ) > 0.0f )
	{
		sun = rf_sun_visibility( hit, hs.normal, cone );
		[branch] if ( sun > 0.0f )
			sun *= gi_canopy( hit + hs.normal * 0.03f, view.sun.xyz, gi2.y );
	}
	const float3 direct = shade.sun_color.rgb * ( 0.7f * saturate( dot( hs.normal, view.sun.xyz ) ) * sun ) +
		rf_hit_lamps( hit, hs.normal, cone, u );
	float3 lit = hs.albedo * ( direct + around.rgb );
	[flatten] if ( hs.emissive )
		lit += hs.albedo * gi.w;
	return float4( lit, 0.0f );
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
	// The first-person hands are not where the main projection puts them: their rays leave where they are in the world.
	const bool hud = rt_is_hud( z, s );

	const float w_mtl = gi_position.Load( int3( pixel, 0 ) ).w;
	const bool foliage = !hud && da_is_foliage( ( view.scene.z & RT_VIEW_GBUFFER_PACKED ) ? gbuf_unpack_mtl( w_mtl ) : abs( w_mtl ) );
	const float3 surface = hud ? rt_hud_world( float2( pixel ) + 0.5f, s.z, view ) :
		rt_to_world( rt_view_pos( float2( pixel ) + 0.5f, z, view ), view );
	const float3 n = normalize( rt_dir_to_world( s.n_view, view ) );
	const float z_ray = hud ? s.z : z;
	const float3 origin = surface + n * ( 0.02f + 0.001f * z_ray );
	const float cone0 = rt_pixel_size( z_ray, view );
	const uint rays = max( refl_mask.y, 1u );
	const float2 noise = rt_noise2( pixel, view.scene.w );
	float4 sum = 0.0f;
	[loop] for ( uint i = 0u; i < rays; ++i )
	{
		const float2 u = frac( noise + float2( 0.7548776662f, 0.5698402910f ) * float( i ) );
		sum += gi_ray( origin, rt_cosine_dir( n, u ), cone0, foliage, frac( u.x * 7.31f + u.y * 3.17f ) );
	}
	sum /= float( rays );
	// The sky seen, as the baked hemisphere means it: 1 in the open (where a wall sees half the sky).
	sum.w = min( sum.w / max( 0.5f + 0.5f * n.y, 0.2f ), 1.0f );
	gi_out[ id.xy ] = sum;
}
