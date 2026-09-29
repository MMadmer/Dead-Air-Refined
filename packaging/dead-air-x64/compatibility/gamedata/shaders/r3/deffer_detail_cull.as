#include "common.h"
#include "da_detail_common.h"
#include "da_detail_mesh.h"

// The grass amplification shader of the DirectX 12 mesh path: one group per 32 consecutive entries
// of the frame's row list (grass_index: pool rows of the visible parts, part after part). The engine
// dispatches only the parts of the slots the vertex path would draw in this pass (its frustum and
// shadow-distance tests, per slot); a row is dropped here only when it provably draws nothing: its
// whole bent tuft lies outside the volume the pass rasterizes (grass_planes: the planes of its WVP),
// or its footprint was faded to zero. The survivors keep their order.

StructuredBuffer<detail_rows>	grass_rows		: register(t2);
StructuredBuffer<uint>			grass_index		: register(t3);

groupshared grass_payload	P;
groupshared uint	gs_mask;

bool	grass_sphere_visible	(float3 c, float r)
{
	[unroll] for (uint i = 0; i < GRASS_MAX_PLANES; ++i)
	{
		if ( dot( grass_planes[i].xyz, c ) + grass_planes[i].w > r )
			return false;
	}
	return true;
}

[numthreads(GRASS_AS_THREADS, 1, 1)]
void main (uint tid : SV_GroupThreadID, uint gid : SV_GroupID)
{
	const uint entry = gid * GRASS_AS_THREADS + tid;
	bool keep = entry < uint( grass_dispatch.y );
	uint row = 0;

	[branch] if ( keep )
	{
		row = grass_index[uint( grass_dispatch.x ) + entry];
		const detail_rows d = detail_rows_faded( grass_rows[row] );
		const float3 root = float3( d.m0.w, d.m1.w, d.m2.w );
		const float sx = length( float3( d.m0.x, d.m1.x, d.m2.x ) );
		const float sy = length( float3( d.m0.y, d.m1.y, d.m2.y ) );
		const float sz = length( float3( d.m0.z, d.m1.z, d.m2.z ) );
		// No footprint left: nothing rasterizes. A flat row (height zero) still does and stays.
		keep = max( sx, sz ) > 0.0f;
		// The rigid model sphere, grown by the farthest the bend can carry a vertex (1.103 of its
		// height above the root: the 0.92 cap plus the arc-length drop).
		const float3 c = root + float3( dot( d.m0.xyz, grass_bound.xyz ), dot( d.m1.xyz, grass_bound.xyz ),
			dot( d.m2.xyz, grass_bound.xyz ) );
		const float r = grass_bound.w * max( max( sx, sy ), sz ) + grass_dispatch.w * sy;
		keep = keep && grass_sphere_visible( c, r );
	}

	// Compaction through a group bit mask rather than wave intrinsics: the rows keep their order and
	// nothing depends on the wave size (16 on some Intel parts, 64 on some AMD ones).
	if ( tid == 0 )
		gs_mask = 0;
	GroupMemoryBarrierWithGroupSync();
	if ( keep )
		InterlockedOr( gs_mask, 1u << tid );
	GroupMemoryBarrierWithGroupSync();
	const uint mask = gs_mask;
	const uint total = countbits( mask );
	if ( keep )
		P.rows[countbits( mask & ( ( 1u << tid ) - 1u ) )] = row;
	if ( tid == 0 )
		P.count = total;
	GroupMemoryBarrierWithGroupSync();

	const uint ipg = grass_ipg();
	DispatchMesh( ( total + ipg - 1 ) / ipg, 1, 1, P );
}
