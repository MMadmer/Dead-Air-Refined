// The device part's probe (da_rt_device.cpp), dispatched once at renderer start: a ray against the
// empty scene misses, so every texel of the output is the miss value plus its input texel. The other
// bindings of the root signature are read only for a valid bindless index, which the probe never
// passes, so the pipeline still proves they match.

#include "rt_common.hlsli"

cbuffer rt_probe_constants : register( b0 )
{
	uint4	probe;	// x miss value, y bindless index (0xFFFFFFFF = none)
};

Texture2D<uint>		probe_in	: register( t0 );
RWTexture2D<uint>	probe_out	: register( u0 );
#ifdef GRIZZLY_SPIRV
// Vulkan: the tables by device address (rt_common.hlsli).
#define PROBE_TABLE( slot )	vk::RawBufferLoad<uint>( rt_addresses.table[ slot ] )
#else
ByteAddressBuffer	probe_geometry	: register( t0, space3 );
ByteAddressBuffer	probe_instances	: register( t1, space3 );
ByteAddressBuffer	probe_materials	: register( t2, space3 );
#define PROBE_TABLE( slot )	( slot == 0 ? probe_geometry.Load( 0 ) : slot == 1 ? probe_instances.Load( 0 ) : probe_materials.Load( 0 ) )
#endif

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	RayDesc ray;
	ray.Origin = float3( id.xy, -1.0 );
	ray.Direction = float3( 0.0, 0.0, 1.0 );
	ray.TMin = 0.0;
	ray.TMax = 10.0;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline( rt_scene, RAY_FLAG_NONE, RT_RAYS_REFLECTION, ray );
	while ( query.Proceed() )
		query.CommitNonOpaqueTriangleHit();
	uint value = query.CommittedStatus() == COMMITTED_NOTHING ? probe.x : 0u;
	value += probe_in.Load( int3( id.xy, 0 ) );

	if ( probe.y != 0xFFFFFFFFu )
	{
		const uint index = probe.y;
		const float2 uv = ( float2( id.xy ) + 0.5 ) / 8.0;
		float4 texel = rt_textures[ NonUniformResourceIndex( index ) ].SampleLevel( rt_linear_wrap, uv, 0.0 );
		texel += rt_textures[ NonUniformResourceIndex( index ) ].SampleLevel( rt_point_clamp, uv, 0.0 );
		texel += rt_textures[ NonUniformResourceIndex( index ) ].SampleLevel( rt_linear_clamp, uv, 0.0 );
		texel.x += rt_textures[ NonUniformResourceIndex( index ) ].SampleCmpLevelZero( rt_compare, uv, 0.5 );
		value += asuint( texel.x + texel.y + texel.z + texel.w );
		value += rt_buffers[ NonUniformResourceIndex( index ) ].Load( 0 );
		value += PROBE_TABLE( 0 ) + PROBE_TABLE( 1 ) + PROBE_TABLE( 2 );
		value += asuint( gbuf_unpack_hemi( asfloat( value ) ) );
	}
	probe_out[ id.xy ] = value;
}
