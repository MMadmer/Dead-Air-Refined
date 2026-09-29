// Ray traced ambient occlusion (dxr.md 3.6): one cosine-weighted ray around the G-buffer normal,
// against the static world, tree trunks near their roots and the moving objects the scene tracks.
// Grass and crowns are receivers only (they sway). Output (visibility, view z of the traced pixel)
// at the trace resolution; RT_AO_HALF traces per 2x2 block the pixel nearest the camera.

#include "rt_geometry.hlsli"

cbuffer rt_ao_constants : register( b0 )
{
	RtView	view;
	float4	ao_params;	// x TMax, y trunk height above the root, z trace width, w trace height
};

Texture2D<float>	ao_depth	: register( t0 );
Texture2D<float4>	ao_position	: register( t1 );
Texture2D<float4>	ao_normal	: register( t2 );

RWTexture2D<float2>	ao_out		: register( u0 );

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( any( float2( id.xy ) >= ao_params.zw ) )
		return;

#ifdef RT_AO_HALF
	// The block's pixel nearest the camera: silhouettes keep the foreground's occlusion.
	uint2 pixel = min( id.xy * 2u, uint2( view.size.xy ) - 1u );
	float depth = ao_depth.Load( int3( pixel, 0 ) );
	[unroll] for ( uint k = 1u; k < 4u; ++k )
	{
		const uint2 p = min( id.xy * 2u + uint2( k & 1u, k >> 1 ), uint2( view.size.xy ) - 1u );
		const float d = ao_depth.Load( int3( p, 0 ) );
		if ( d < depth )
		{
			depth = d;
			pixel = p;
		}
	}
#else
	const uint2 pixel = id.xy;
	const float depth = ao_depth.Load( int3( pixel, 0 ) );
#endif

	[branch] if ( rt_is_sky( depth ) )
	{
		ao_out[ id.xy ] = float2( 1.0f, view.proj.w );
		return;
	}
	const float z = rt_linear_z( depth, view );
	const RtSurface s = rt_surface( ao_position, ao_normal, int2( pixel ), view );
	[branch] if ( rt_is_hud( z, s ) )
	{
		ao_out[ id.xy ] = float2( 1.0f, z );
		return;
	}

	const float3 p_world = rt_to_world( rt_view_pos( float2( pixel ) + 0.5f, z, view ), view );
	const float3 n_world = normalize( rt_dir_to_world( s.n_view, view ) );
	const float3 dir = rt_cosine_dir( n_world, rt_noise2( pixel, view.scene.w ) );

	RayDesc ray;
	ray.Origin = p_world + n_world * ( 0.01f + 0.001f * z );
	ray.Direction = dir;
	ray.TMin = 0.01f;
	ray.TMax = ao_params.x;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline( rt_scene, RAY_FLAG_NONE, RT_RAYS_AO, ray );
	const float cone = rt_pixel_size( z, view );
	while ( q.Proceed() )
	{
		if ( q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE )
			continue;
		const RtInstance inst = rt_instance_of( q.CandidateInstanceIndex(), view );
		bool solid;
		[branch] if ( inst.flags & RT_INST_TRUNK )
		{
			// A trunk occludes only near its root, where the wind does not move it.
			const float y = ray.Origin.y + ray.Direction.y * q.CandidateTriangleRayT();
			solid = y < inst.root_y + ao_params.y;
		}
		else
			solid = rt_alpha_solid( q.CandidateInstanceID(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(),
				q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4(), cone, view );
		if ( solid )
			q.CommitNonOpaqueTriangleHit();
	}
	float visibility = 1.0f;
	[branch] if ( q.CommittedStatus() == COMMITTED_TRIANGLE_HIT )
	{
		const float k = saturate( 1.0f - q.CommittedRayT() / ao_params.x );
		visibility = 1.0f - k * k;
	}
	ao_out[ id.xy ] = float2( visibility, z );
}
