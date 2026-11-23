// The world cache of the traced light (rt_cache.hlsli, dxr.md 3.15): every cell's sample of the frame into its resolved
// light, or every cell cleared (a new cache, a new scene). A cell written this frame blends its sample in (the first
// one, or a key new to the cell, takes it whole); an unwritten one ages and is let go after its life.

#include "rt_cache.hlsli"

cbuffer rt_cache_constants : register( b0 )
{
	uint4	cache;		// x cells, y clear them all (0/1)
	float4	blend;		// x a written cell's new share, y frames an unwritten cell lives
};

[numthreads( 64, 1, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if ( id.x >= cache.x )
		return;
	const uint offset = id.x * RT_CACHE_STRIDE;
	[branch] if ( cache.y != 0u )
	{
		rt_cache_store( offset, 0u );
		rt_cache_store( offset + 16u, 0u );
		return;
	}
	const uint4 sample = rt_cache_load( offset );
	uint4 r = rt_cache_load( offset + 16u );
	[branch] if ( sample.z & RT_CACHE_WRITTEN )
	{
		const float3 c = float3( f16tof32( sample.y ), f16tof32( sample.y >> 16 ), f16tof32( sample.z ) );
		float3 res = c;
		[branch] if ( r.z == sample.x && ( r.y & RT_CACHE_VALID ) )
			res = lerp( float3( f16tof32( r.x ), f16tof32( r.x >> 16 ), f16tof32( r.y ) ), c, blend.x );
		r = uint4( f32tof16( res.r ) | ( f32tof16( res.g ) << 16 ), f32tof16( res.b ) | RT_CACHE_VALID, sample.x, 0u );
		rt_cache_store( offset, uint4( sample.x, sample.y, sample.z & ~RT_CACHE_WRITTEN, 0u ) );
		rt_cache_store( offset + 16u, r );
		return;
	}
	[branch] if ( r.y & RT_CACHE_VALID )
	{
		const uint age = ( r.y >> 17 ) + 1u;
		r.y = age > uint( blend.y ) ? 0u : ( ( r.y & 0x1FFFFu ) | ( min( age, 0x7FFFu ) << 17 ) );
		if ( !( r.y & RT_CACHE_VALID ) )
			r.z = 0u;
		rt_cache_store( offset + 16u, r );
	}
}
