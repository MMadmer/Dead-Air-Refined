// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The cluster hierarchy's indices (docs/dead-air/CLUSTERS.md; da_cluster_cull.cs, da_cluster_args.cs): a group a kept
// cluster, a lane two of its triangles, writes their six 16-bit indices - each corner's byte in the cluster's vertex
// table, the table's word the vertex in the level's own vertex buffer - into its draw's list at the place the cull gave
// it (an odd count's last pair ends with a triangle of no area).

#include "da_cluster.h"

ByteAddressBuffer	da_cl_clusters	: register(t0);
ByteAddressBuffer	da_cl_tris		: register(t1);	// three bytes a triangle
ByteAddressBuffer	da_cl_verts		: register(t2);	// a 16-bit vertex a table entry
ByteAddressBuffer	da_cl_visible	: register(t3);
ByteAddressBuffer	da_cl_args		: register(t4);
ByteAddressBuffer	da_cl_counts	: register(t5);
RWByteAddressBuffer	da_cl_indices	: register(u0);	// the frame's index pool, the draws' index buffer

uniform float4	da_cl_view;		// x the list's first, y its room, z the args' first draw byte, w the view

uint3	da_cl_tri	(uint t)
{
	const uint b = t * 3u, s = (b & 3u) * 8u;
	const uint2 w = da_cl_tris.Load2(b & ~3u);
	const uint bits = s ? (w.x >> s) | (w.y << (32u - s)) : w.x;
	return uint3(bits & 0xFFu, (bits >> 8) & 0xFFu, (bits >> 16) & 0xFFu);
}

uint	da_cl_vertex	(uint e)
{
	const uint w = da_cl_verts.Load((e >> 1) * 4u);
	return (e & 1u) ? w >> 16 : w & 0xFFFFu;
}

[numthreads(64, 1, 1)]
void	main	(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint group = gid.x + gid.y * 65535u;
	const uint view = uint(da_cl_view.w);
	if (group >= min(da_cl_counts.Load((1u + view * 2u) * 4u), uint(da_cl_view.y)))
		return;
	const uint3 e = da_cl_visible.Load3((uint(da_cl_view.x) + group) * 12u);
	const DaCluster c = da_cl_load(da_cl_clusters, e.x);
	const uint t0 = tid.x * 2u;
	if (t0 >= c.tris)
		return;
	const uint start = da_cl_args.Load(uint(da_cl_view.z) + e.y * 20u + 8u) + e.z;
	// A draw the pool could not take has no room: nothing is written.
	if (da_cl_args.Load(uint(da_cl_view.z) + e.y * 20u) == 0u)
		return;
	const uint3 a = da_cl_tri(c.tri_first + t0);
	const uint3 b = t0 + 1u < c.tris ? da_cl_tri(c.tri_first + t0 + 1u) : a.xxx;
	const uint i0 = da_cl_vertex(c.vert_first + a.x), i1 = da_cl_vertex(c.vert_first + a.y), i2 = da_cl_vertex(c.vert_first + a.z);
	const uint j0 = da_cl_vertex(c.vert_first + b.x), j1 = da_cl_vertex(c.vert_first + b.y), j2 = da_cl_vertex(c.vert_first + b.z);
	da_cl_indices.Store3((start + t0 * 3u) * 2u, uint3(i0 | (i1 << 16), i2 | (j0 << 16), j1 | (j2 << 16)));
}
