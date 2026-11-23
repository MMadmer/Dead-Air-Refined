#ifndef	rt_cache_hlsli_included
#define	rt_cache_hlsli_included

// The world cache of the traced light (dxr.md 3.15): the bounced light the global illumination found at the surfaces the
// screen showed, kept by world position and facing, for the rays' hits - a hit adds the light the cache holds there to
// what lights it, and the light bounces again, off screen and behind the camera too. A hashed grid of cells whose size
// grows with the distance to the camera; a key has two cells (its own and the one beside it), a cell holds the last
// sample a pixel wrote this frame and the light resolved over the frames (rt_gi_cache.hlsl). No atomics: pixels of one
// key write in turn, the last one wins, the resolve averages over the frames. A key keeps its cell while it is written:
// another key takes only a free cell or one unwritten for RT_CACHE_STALE frames, else writes nothing (the reader checks
// the key).
//
// The includer binds the cache as the pass's raw output (u0 space3; on Vulkan its address in the push constants).

#include "rt_common.hlsli"

#ifdef GRIZZLY_SPIRV
uint4 rt_cache_load( uint offset ) { return vk::RawBufferLoad<uint4>( rt_addresses.raw_output + offset, 16 ); }
void rt_cache_store( uint offset, uint4 v ) { vk::RawBufferStore<uint4>( rt_addresses.raw_output + offset, v, 16 ); }
#else
RWByteAddressBuffer	rt_cache_cells	: register( u0, space3 );
uint4 rt_cache_load( uint offset ) { return rt_cache_cells.Load4( offset ); }
void rt_cache_store( uint offset, uint4 v ) { rt_cache_cells.Store4( offset, v ); }
#endif

// Cells (a power of two; kRtGiCacheCells), bytes a cell: the last sample (its key, rg, b | written), then the resolved
// light (rg, b | valid | age << 17, its key).
#define RT_CACHE_CELLS		( 1u << 19 )
#define RT_CACHE_STRIDE		32u
// The last sample was written this frame; the resolved light holds a value (rt_gi_cache.hlsl).
#define RT_CACHE_WRITTEN	0x10000u
#define RT_CACHE_VALID		0x10000u
// Frames a cell stays its key's without a sample before another key may take it.
#define RT_CACHE_STALE		60u

uint rt_cache_hash( uint x )
{
	// PCG's output permutation (O'Neill 2014).
	const uint s = x * 747796405u + 2891336453u;
	const uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

// The key of the cell a surface point falls in, never 0 (an empty cell): its position on a grid of the distance's
// level (25 cm within 16 m of the camera, doubling with every doubling of the distance), the axis its normal faces most.
uint rt_cache_key( float3 p, float3 n, float3 camera )
{
	const float d = max( length( p - camera ), 1.0f );
	const uint level = min( uint( max( floor( log2( d / 16.0f ) ) + 1.0f, 0.0f ) ), 7u );
	const float size = 0.25f * float( 1u << level );
	const float3 a = abs( n );
	const uint facing = a.x >= a.y && a.x >= a.z ? ( n.x >= 0.0f ? 0u : 1u ) :
		( a.y >= a.z ? ( n.y >= 0.0f ? 2u : 3u ) : ( n.z >= 0.0f ? 4u : 5u ) );
	// Off the surface by a share of the cell, so the two faces of a thin wall fall in cells of their own.
	const int3 c = int3( floor( ( p + n * ( 0.25f * size ) ) / size ) );
	uint h = rt_cache_hash( asuint( c.x ) + level * 0x9E3779B9u );
	h = rt_cache_hash( h ^ asuint( c.y ) );
	h = rt_cache_hash( h ^ asuint( c.z ) );
	h = rt_cache_hash( h ^ ( facing + 7u ) );
	return max( h, 1u );
}

uint rt_cache_offset( uint key ) { return ( key & ( RT_CACHE_CELLS - 1u ) ) * RT_CACHE_STRIDE; }

bool rt_cache_holds( uint4 resolved, uint key ) { return resolved.z == key && ( resolved.y & RT_CACHE_VALID ); }

// The light the cache holds at a point (rgb; a 0 when none).
float4 rt_cache_read( float3 p, float3 n, float3 camera )
{
	const uint key = rt_cache_key( p, n, camera );
	const uint offset = rt_cache_offset( key );
	uint4 r = rt_cache_load( offset + 16u );
	[branch] if ( !rt_cache_holds( r, key ) )
	{
		r = rt_cache_load( ( offset ^ RT_CACHE_STRIDE ) + 16u );
		[branch] if ( !rt_cache_holds( r, key ) )
			return 0.0f;
	}
	return float4( f16tof32( r.x ), f16tof32( r.x >> 16 ), f16tof32( r.y ), 1.0f );
}

// A cell another key may take: free, or unwritten for RT_CACHE_STALE frames.
bool rt_cache_free( uint4 resolved ) { return !( resolved.y & RT_CACHE_VALID ) || ( resolved.y >> 17 ) > RT_CACHE_STALE; }

// This pixel's light as its key's sample of the frame: in the key's own cell, else a free one of its two, else nowhere.
void rt_cache_write( float3 p, float3 n, float3 camera, float3 light )
{
	const uint key = rt_cache_key( p, n, camera );
	const uint o0 = rt_cache_offset( key ), o1 = o0 ^ RT_CACHE_STRIDE;
	const uint4 r0 = rt_cache_load( o0 + 16u ), r1 = rt_cache_load( o1 + 16u );
	uint offset = 0xFFFFFFFFu;
	[flatten] if ( r0.z == key )
		offset = o0;
	else if ( r1.z == key )
		offset = o1;
	else if ( rt_cache_free( r0 ) )
		offset = o0;
	else if ( rt_cache_free( r1 ) )
		offset = o1;
	[branch] if ( offset == 0xFFFFFFFFu )
		return;
	const float3 c = min( max( light, 0.0f ), 60000.0f );
	rt_cache_store( offset, uint4( key, f32tof16( c.r ) | ( f32tof16( c.g ) << 16 ), f32tof16( c.b ) | RT_CACHE_WRITTEN, 0u ) );
}

#endif	//	rt_cache_hlsli_included
