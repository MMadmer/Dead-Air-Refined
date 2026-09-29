#include "common.h"
// The engine's shader cache follows "#include" lines, whatever the defines.
#ifdef DETAIL_WAVE
#include "da_wind_field.h"
#endif
#include "da_detail_common.h"
#include "da_detail_mesh.h"

// The grass mesh shader of the DirectX 12 mesh path: the tufts the amplification shader kept, up to
// grass_obj.w per group, with exactly the maths of deffer_detail_w_flat.vs (DETAIL_WAVE) or
// deffer_detail_s_flat.vs - only the terms that belong to a whole tuft (its sway, shelter, gust
// field, local heading, tint noise) are computed once per tuft instead of once per vertex.
// DETAIL_DEPTH: the shadow-pass variant (detail elements 4, 5) - the same positions, only what the
// alpha test and the rasterizer read.

#ifdef DETAIL_WAVE
uniform float4 		wave; 	// cx,cy,cz,tm
uniform float4 		dir2D;
// wave and dir2D as the previous frame drew with them, for the motion vectors.
uniform float4 		wave_prev;
uniform float4 		dir2D_prev;
#endif

#ifdef DETAIL_DEPTH
struct v2p_grass_depth
{
	float2	tcdh	: TEXCOORD0;
	float4	hpos	: SV_Position;
};
#define GRASS_VERTEX	v2p_grass_depth
#else
#define GRASS_VERTEX	v2p_flat
#endif

StructuredBuffer<detail_vtx>	grass_vertices	: register(t0);
ByteAddressBuffer				grass_indices	: register(t1);
StructuredBuffer<detail_rows>	grass_rows		: register(t2);

groupshared detail_rows	gs_rows[GRASS_MAX_IPG];
#ifndef DETAIL_DEPTH
groupshared float		gs_tint[GRASS_MAX_IPG];
#endif
#ifdef DETAIL_WAVE
groupshared detail_wind	gs_wind[GRASS_MAX_IPG];
#ifndef DETAIL_DEPTH
groupshared detail_wind	gs_wind_prev[GRASS_MAX_IPG];
#endif
#endif

[outputtopology("triangle")]
[numthreads(GRASS_MS_THREADS, 1, 1)]
void main (uint tid : SV_GroupThreadID, uint gid : SV_GroupID, in payload grass_payload P,
	out vertices GRASS_VERTEX verts[GRASS_MS_MAX_VERTS], out indices uint3 tris[GRASS_MS_MAX_PRIMS])
{
	const uint ipg = grass_ipg();
	const uint nv = grass_nv();
	const uint nt = grass_nt();
	const uint first = gid * ipg;
	const uint n = min( ipg, P.count - first );

	// The terms of each tuft, once.
	if ( tid < n )
	{
		const detail_rows d = detail_rows_faded( grass_rows[P.rows[first + tid]] );
		gs_rows[tid] = d;
#ifdef DETAIL_WAVE
		const float3 root = float3( d.m0.w, d.m1.w, d.m2.w );
		gs_wind[tid] = detail_wind_at( root, d.c0.w, da_wind_field, wave, dir2D );
#ifndef DETAIL_DEPTH
		// The same tuft under the previous frame's wind, only while motion vectors are rendered.
		[branch] if ( da_wind_prev_on() )
			gs_wind_prev[tid] = detail_wind_at( root, d.c0.w, da_wind_field_prev, wave_prev, dir2D_prev );
#endif
#endif
#ifndef DETAIL_DEPTH
		gs_tint[tid] = grass_tint.x > 0.001f ? detail_tint_n( float2( d.m0.w, d.m2.w ), grass_tint.y ) : 0.0f;
#endif
	}
	GroupMemoryBarrierWithGroupSync();

	SetMeshOutputCounts( n * nv, n * nt );

	for ( uint v = tid; v < n * nv; v += GRASS_MS_THREADS )
	{
		const uint i = v / nv;
		const detail_vtx src = grass_vertices[uint( grass_obj.x ) + ( v - i * nv )];
		const detail_rows d = gs_rows[i];
		// The vertex path's inputs: position with w = 1, (u, v, frac, matrix-id) as signed shorts.
		const float4 vpos = float4( src.pos, 1.0f );
		const int4 misc = int4( asint( src.uv << 16 ) >> 16, asint( src.uv ) >> 16,
			asint( src.tmid << 16 ) >> 16, asint( src.tmid ) >> 16 );

		const float4 pos_rest = detail_world_pos( d.m0, d.m1, d.m2, vpos );
		const float3 root = float3( d.m0.w, d.m1.w, d.m2.w );
#ifdef DETAIL_WAVE
		const float H = detail_height( d.m0, d.m1, d.m2, vpos.y );
		const float frac = misc.z * consts.x;		// fractional
		float4 pos = detail_bent_pos( pos_rest, H, detail_bend_v( root, gs_wind[i], H, frac, false ) );
#else
		const float H_s = detail_height( d.m0, d.m1, d.m2, vpos.y );
		// Wind motors: still details do not wave, but a boot or a blast still bends them.
		float4 pos = detail_motor_pos( pos_rest, root, H_s, false );
#endif

#ifdef DETAIL_DEPTH
		// detail_v2p's texture coordinates and position.
		GRASS_VERTEX O;
		O.tcdh = ( misc * consts ).xy;
		O.hpos = mul( m_WVP, pos );
#else
		float4 pos_prev = pos;
#ifdef DETAIL_WAVE
		[branch] if ( da_wind_prev_on() )
			pos_prev = detail_bent_pos( pos_rest, H, detail_bend_v( root, gs_wind_prev[i], H, frac, true ) );
#else
		[branch] if ( da_wind_prev_on() )
			pos_prev = detail_motor_pos( pos_rest, root, H_s, true );
#endif
		GRASS_VERTEX O = detail_v2p( root, pos, pos_prev, misc, consts, d.c0.x );
		float da_hemi = d.c0.w;
		[branch] if ( grass_tint.x > 0.001f )
			da_hemi = detail_tint_hemi( da_hemi, gs_tint[i], grass_tint, vpos.y );
		O.position.w = da_hemi;
#endif
		verts[v] = O;
	}

	for ( uint p = tid; p < n * nt; p += GRASS_MS_THREADS )
	{
		const uint i = p / nt;
		const uint ib = uint( grass_obj.y ) + ( p - i * nt ) * 3;
		tris[p] = uint3( detail_index( grass_indices, ib ), detail_index( grass_indices, ib + 1 ),
			detail_index( grass_indices, ib + 2 ) ) + i * nv;
	}
}
