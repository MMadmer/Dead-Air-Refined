// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The cluster hierarchy's cull (docs/dead-air/CLUSTERS.md; da_clusters.cpp): a lane a cluster of the view's visuals -
// those the portal walk, the frustum and the occlusion map let through - keeps it where the cut takes it (da_cluster.h),
// its box is inside the view's frustum and, in the main view, it is not behind the depth pyramid; a kept cluster takes
// the room for its triangles in its draw's list (an even count: the indices go out in pairs of triangles) and joins
// the view's list for the expansion. Where the pyramid hides it in the first pass of the main view it joins the second
// list instead, tested again against the pyramid of the frame's own first draws.

#define DA_HZB_SLOT t3
#include "da_hzb.h"
#include "da_cluster.h"

ByteAddressBuffer	da_cl_clusters	: register(t0);
ByteAddressBuffer	da_cl_instances	: register(t1);	// 16 bytes each: the visual's first cluster, its clusters, its draw
ByteAddressBuffer	da_cl_table		: register(t2);	// 8 bytes a group of 64: the instance, its first cluster's place in it
RWByteAddressBuffer	da_cl_counts	: register(u0);	// the frame's counters (da_clusters.cpp, Counters)
RWByteAddressBuffer	da_cl_visible	: register(u1);	// 12 bytes each: the cluster, its draw, its place in the draw's list
RWByteAddressBuffer	da_cl_second	: register(u2);	// the second pass's list, 8 bytes each: the cluster, its draw

uniform float4	da_cl_planes[6];	// the view's frustum, world space, inside positive
uniform float4	da_cl_view;			// x the instances' first, y the table's first, z groups a row of the dispatch, w the view
uniform float4	da_cl_view2;		// x the draws' first counter, y the list's first, z its room, w flags: 1 the pyramid's
									// test, 2 the hidden into the second list, 4 this is the second pass (the second list
									// in, by the count it holds)
uniform float4	da_cl_view3;		// x the second list's first, y its room, z the view whose second list the second pass
									// reads, w the second pass's draws past the first's
uniform float4	da_cl_view4;		// x the first pass's table rows (the dispatch's rows over-cover them)

#define DA_CL_COUNT_VISIBLE(v)	((1u + (v) * 2u) * 4u)
#define DA_CL_COUNT_SECOND(v)	((2u + (v) * 2u) * 4u)

[numthreads(64, 1, 1)]
void	main	(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint group = gid.x + gid.y * uint(da_cl_view.z);
	const uint view = uint(da_cl_view.w);
	const uint flags = uint(da_cl_view2.w);
	uint cluster, draw;
	if (flags & 4u)
	{
		const uint at = group * 64u + tid.x;
		if (at >= min(da_cl_counts.Load(DA_CL_COUNT_SECOND(uint(da_cl_view3.z))), uint(da_cl_view3.y)))
			return;
		const uint2 e = da_cl_second.Load2((uint(da_cl_view3.x) + at) * 8u);
		cluster = e.x;
		draw = e.y + uint(da_cl_view3.w);
	}
	else
	{
		if (group >= uint(da_cl_view4.x))
			return;
		const uint2 entry = da_cl_table.Load2((uint(da_cl_view.y) + group) * 8u);
		const uint4 inst = da_cl_instances.Load4((uint(da_cl_view.x) + entry.x) * 16u);
		const uint k = entry.y + tid.x;
		if (k >= inst.y)
			return;
		cluster = inst.x + k;
		draw = inst.z;
	}
	const DaCluster c = da_cl_load(da_cl_clusters, cluster);
	if (!(flags & 4u))
	{
		if (!da_cl_in_cut(c))
			return;
		[unroll] for (uint i = 0u; i < 6u; ++i)
		{
			const float4 p = da_cl_planes[i];
			if (dot(p.xyz, c.box_c) + p.w + dot(abs(p.xyz), c.box_e) < 0.0f)
				return;
		}
	}
	if ((flags & 1u) && hzb_box_hidden(c.box_c - c.box_e, c.box_c + c.box_e))
	{
		if (flags & 2u)
		{
			uint at;
			da_cl_counts.InterlockedAdd(DA_CL_COUNT_SECOND(view), 1u, at);
			if (at < uint(da_cl_view3.y))
				da_cl_second.Store2((uint(da_cl_view3.x) + at) * 8u, uint2(cluster, draw));
		}
		return;
	}
	uint at;
	da_cl_counts.InterlockedAdd(DA_CL_COUNT_VISIBLE(view), 1u, at);
	if (at >= uint(da_cl_view2.z))
		return;
	uint slot;
	da_cl_counts.InterlockedAdd((uint(da_cl_view2.x) + draw) * 4u, ((c.tris + 1u) & ~1u) * 3u, slot);
	da_cl_visible.Store3((uint(da_cl_view2.y) + at) * 12u, uint3(cluster, draw, slot));
}
