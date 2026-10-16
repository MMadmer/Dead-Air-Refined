// Ray traced reflections of glossy surfaces (dxr.md 3.9): DOOM Eternal's coverage (raygenreflections traces every
// glossy pixel the screen space cannot answer) where the engine reflected only the sky's cube. Half resolution: one
// pixel of each 2x2 block per frame, the block's four pixels in turn (DOOM's per-frame sub-pixel offset);
// rt_gloss_temporal.hlsl rebuilds the full resolution. Runs before combine_1, which puts the result in place of the
// cube in its specular term, so everything here is linear: what the screen shows comes from the previous frame's
// HDR colour (combine_1's high target, copied at the scene grab), an off-screen hit is shaded as the water's are,
// without the tonemap. A rough surface spreads its rays by its roughness; the temporal pass averages them.
// Output premultiplied by its confidence (0 = the ray missed: the cube stays).

#include "rt_material.hlsli"
#include "da_material_codes.h"

cbuffer rt_gloss_constants : register( b0 )
{
	RtView	view;
	RtShade	shade;
	float4	refl;		// x TMax, y cone spread per metre, z screen-edge margin in pixels, w off-screen sun rays (0/1)
	uint4	refl_mask;	// x instance mask of the rays, y most screen-space steps of a ray, zw trace size
	float4	gloss;		// x lowest gloss traced, y gloss of the full weight, z roughness spread, w HDR of the frame copy
};

Texture2D<float>	rf_depth	: register( t0 );
Texture2D<float4>	gl_position	: register( t1 );
Texture2D<float4>	gl_normal	: register( t2 );
Texture2D<float4>	gl_albedo	: register( t3 );
Texture2D<float4>	gl_scene	: register( t4 );
TextureCube			gl_env0		: register( t5 );
TextureCube			gl_env1		: register( t6 );
TextureCube			gl_sky0		: register( t7 );
TextureCube			gl_sky1		: register( t8 );

RWTexture2D<float4>	gl_out		: register( u0 );
RWTexture2D<float>	gl_dist		: register( u1 );

#include "rt_reflect.hlsli"

// The pixel of a 2x2 block this frame traces (rt_gloss_temporal.hlsl takes the same).
uint2 gl_block_pixel( uint2 block )
{
	const uint f = view.scene.w & 3u;
	const uint2 offset = uint2( f & 1u, ( ( f >> 1 ) ^ f ) & 1u );
	return min( block * 2u + offset, uint2( view.size.xy ) - 1u );
}

// The G-buffer's material code of a pixel (gbuffer_load_data).
float gl_material( int2 pixel )
{
	const float w = gl_position.Load( int3( pixel, 0 ) ).w;
	return ( view.scene.z & RT_VIEW_GBUFFER_PACKED ) ? gbuf_unpack_mtl( w ) : abs( w );
}

// The previous frame's linear colour of a world point the current frame shows (weight 0 off its screen).
float4 gl_previous( float3 p )
{
	const float3 prev = rt_project( p, view.vp_prev );
	[branch] if ( prev.z <= 0.05f || any( prev.xy <= 0.0f ) || any( prev.xy >= 1.0f ) )
		return 0.0f;
	const float3 c = gl_scene.SampleLevel( rt_linear_clamp, prev.xy, 0 ).rgb * gloss.w;
	return float4( c, rf_edge_fade( prev.xy ) );
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( id.xy >= refl_mask.zw ) )
		return;
	gl_out[ id.xy ] = 0.0f;
	gl_dist[ id.xy ] = 0.0f;
	const uint2 pixel = gl_block_pixel( id.xy );
	const float depth = rf_depth.Load( int3( pixel, 0 ) );
	[branch] if ( rt_is_sky( depth ) )
		return;
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( gl_position, gl_normal, int2( pixel ), view );
	const float g = gl_albedo.Load( int3( pixel, 0 ) ).w;
	const float mtl = gl_material( int2( pixel ) );
	// Leaves, blades and snow keep the cube: their gloss is a sheen, not a mirror.
	[branch] if ( rt_is_hud( z, s ) || g < gloss.x || da_is_foliage( mtl ) || da_is_snowy( mtl ) )
		return;

	const float3 surface = rt_to_world( rt_view_pos( float2( pixel ) + 0.5f, z, view ), view );
	const float3 n = normalize( rt_dir_to_world( s.n_view, view ) );
	const float3 v = normalize( surface - view.camera.xyz );
	float3 dir = reflect( v, n );
	// A rough surface spreads its mirror image; a ray that would leave below the surface grazes it instead.
	const float rough = 1.0f - g;
	dir = rt_cone_dir( dir, gloss.z * rough * rough, rt_noise2( pixel, view.scene.w ) );
	const float rise = dot( dir, n );
	[flatten] if ( rise < 0.02f )
		dir = normalize( dir + n * ( 0.02f - rise ) );

	const float cone0 = rt_pixel_size( z, view );
	const float3 origin = surface + n * ( 0.02f + 0.001f * z );
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.01f;
	ray.TMax = refl.x;
	RfHit h = rf_trace( ray, cone0 );
	// The raster's surface lower than the traced mesh: the ray left it through its back (rt_reflections.hlsl).
	float skipped = 0.0f;
	[branch] if ( h.t >= 0.0f && h.t < 0.5f + 0.05f * z && rf_from_behind( h, dir ) )
	{
		skipped = h.t + 0.02f;
		ray.Origin = origin + dir * skipped;
		ray.TMax = max( refl.x - skipped, 0.02f );
		h = rf_trace( ray, cone0 + refl.y * skipped );
	}
	const bool traced = h.t >= 0.0f;
	const float t = traced ? skipped + h.t : refl.x;

	// What this frame shows in the ray's way short of the traced hit: grass and swaying crowns, which the scene
	// lacks or holds at rest; their colour is the previous frame's at the same point.
	const float3 seen_at = rf_march( origin, dir, traced ? t - max( 0.05f, 0.01f * t ) : t, pixel );

	float3 color = 0.0f;
	float alpha = 0.0f;
	[branch] if ( traced && seen_at.z < 0.999f )
	{
		const float3 hit = origin + dir * t;
		float4 reused = 0.0f;
		{
			const float3 fp = rf_frame_pos( hit );
			[branch] if ( fp.z > 0.05f && all( fp.xy > 0.0f ) && all( fp.xy < 1.0f ) &&
				abs( rf_frame_z( fp.xy ) - fp.z ) < max( 0.05f, 0.01f * fp.z ) )
			{
				reused = gl_previous( hit );
				reused.w *= rf_edge_fade( fp.xy );
			}
		}
		color = reused.rgb;
		[branch] if ( reused.w < 0.999f )
		{
			const RtGeometry geo = rt_geometry_of( h.instance_id, h.geometry_index );
			const RtInstance inst = rt_instance_of( h.instance_index, view );
			const RtTriangle tri = rt_fetch( geo, h.primitive );
			const float cone = cone0 + refl.y * t;
			const RtHitSurface hs = rt_hit_surface( geo, inst, tri, h.bary, h.o2w, dir, cone, view );
			const float sun = rf_sun_visibility( hit, hs.normal, cone );
			float3 lit = rt_hit_light( hs, sun, shade, gl_env0, gl_env1, view );
			lit = rt_fog_apply( lit, hit, view.camera.xyz, shade, gl_sky0, gl_sky1 );
			color = lerp( lit, reused.rgb, reused.w );
		}
		alpha = 1.0f;
	}
	[branch] if ( seen_at.z > 0.0f )
	{
		const float2 uv = seen_at.xy;
		const float zs = rf_frame_z( uv );
		const float3 ps = rt_to_world( rt_view_pos( uv * view.size.xy, zs, view ), view );
		const float4 seen = gl_previous( ps );
		const float w = seen_at.z * seen.w;
		color = lerp( color, seen.rgb, w );
		alpha = lerp( alpha, 1.0f, w );
	}
	// The weight ramps in above the lowest gloss traced, so the threshold draws no edge.
	alpha *= saturate( ( g - gloss.x ) / max( gloss.y - gloss.x, 1e-3f ) );
	gl_out[ id.xy ] = float4( color * alpha, alpha );
	gl_dist[ id.xy ] = traced ? t : 0.0f;
}
