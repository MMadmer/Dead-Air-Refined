// Pre-skinning of a character for its acceleration structure (dxr.md 3.8): the bind-pose vertices
// (the model's own vertBoned* data, uploaded once per model) through this frame's bone palette,
// exactly as the skinning vertex shaders and the software skinner blend them, into the vertex
// format the scene's own meshes use: float3 position, octahedral snorm16 normal, float2 texcoord.

cbuffer rt_skin_constants : register( b0 )
{
	uint4	skin;		// x vertex count, y bones in the palette
};

#ifdef GRIZZLY_SPIRV
// Vulkan: the source, the palette and the output by device address (DaRtDispatch tables 0-1 and
// raw_output, the push constants of rt_common.hlsli).
struct RtSkinAddresses
{
	uint64_t	table[ 4 ];
	uint64_t	raw_output;
};
[[vk::push_constant]] RtSkinAddresses	skin_addresses;
#define SKIN_SOURCE( type, offset )		vk::RawBufferLoad<type>( skin_addresses.table[ 0 ] + ( offset ) )
#define SKIN_PALETTE( offset )			vk::RawBufferLoad<uint4>( skin_addresses.table[ 1 ] + ( offset ), 16 )
#define SKIN_STORE( type, offset, value )	vk::RawBufferStore<type>( skin_addresses.raw_output + ( offset ), value )
#else
// 64 bytes a vertex: position, normal, texcoord, bones 0-1, bones 2-3 (16 bits each), four weights.
ByteAddressBuffer	skin_source		: register( t0, space3 );
// Three float4 rows a bone, as the engine uploads sbones_array: dot( row, float4( p, 1 ) ).
ByteAddressBuffer	skin_palette	: register( t1, space3 );
RWByteAddressBuffer	skin_out		: register( u0, space3 );
#define SKIN_SOURCE( type, offset )		skin_source.Load<type>( offset )
#define SKIN_PALETTE( offset )			skin_palette.Load4( offset )
#define SKIN_STORE( type, offset, value )	skin_out.Store<type>( offset, value )
#endif

float3x4 skin_bone( uint bone )
{
	const uint o = min( bone, skin.y - 1u ) * 48u;
	return float3x4( asfloat( SKIN_PALETTE( o ) ), asfloat( SKIN_PALETTE( o + 16u ) ), asfloat( SKIN_PALETTE( o + 32u ) ) );
}

uint skin_oct16( float3 n )
{
	n /= abs( n.x ) + abs( n.y ) + abs( n.z ) + 1e-12f;
	float2 e = n.xy;
	if ( n.z < 0.0f )
		e = ( 1.0f - abs( n.yx ) ) * select( n.xy >= 0.0f, float2( 1.0f, 1.0f ), float2( -1.0f, -1.0f ) );
	const int2 q = int2( round( clamp( e, -1.0f, 1.0f ) * 32767.0f ) );
	return ( uint( q.x ) & 0xFFFFu ) | ( uint( q.y ) << 16 );
}

[numthreads( 64, 1, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( id.x >= skin.x )
		return;
	const uint src = id.x * 64u;
	const float3 p = asfloat( SKIN_SOURCE( uint3, src ) );
	const float3 n = asfloat( SKIN_SOURCE( uint3, src + 12u ) );
	const float2 tc = asfloat( SKIN_SOURCE( uint2, src + 24u ) );
	const uint2 bones = SKIN_SOURCE( uint2, src + 32u );
	const float4 w = asfloat( SKIN_SOURCE( uint4, src + 40u ) );

	const uint b[4] = { bones.x & 0xFFFFu, bones.x >> 16, bones.y & 0xFFFFu, bones.y >> 16 };
	float3 sp = 0.0f, sn = 0.0f;
	[unroll] for ( uint i = 0u; i < 4u; ++i )
	{
		[branch] if ( w[ i ] > 0.0f )
		{
			const float3x4 m = skin_bone( b[ i ] );
			sp += mul( m, float4( p, 1.0f ) ) * w[ i ];
			sn += mul( (float3x3)m, n ) * w[ i ];
		}
	}

	const uint dst = id.x * 24u;
	SKIN_STORE( uint3, dst, asuint( sp ) );
	SKIN_STORE( uint, dst + 12u, skin_oct16( normalize( sn ) ) );
	SKIN_STORE( uint2, dst + 16u, asuint( tc ) );
}
