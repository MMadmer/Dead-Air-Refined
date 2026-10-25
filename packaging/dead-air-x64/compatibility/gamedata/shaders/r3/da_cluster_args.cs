// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The cluster hierarchy's draws (docs/dead-air/CLUSTERS.md; da_cluster_cull.cs): one group lays out the view's draws in
// the frame's index pool - their counts summed in order (each lane a run of consecutive draws, a scan over the lanes),
// the view's whole taken from the pool's top at once - and writes each draw's DrawIndexedInstanced arguments, the
// expansion's dispatch (a group a kept cluster, in rows of 65535) and the second pass's (a group 64 hidden clusters). A
// view the pool cannot take draws nothing (its counts zeroed, the pool's top given back for the views after it: the
// frame's log says so).

#define DA_CL_POOL_TOP			0u
#define DA_CL_COUNT_VISIBLE(v)	((1u + (v) * 2u) * 4u)
#define DA_CL_COUNT_SECOND(v)	((2u + (v) * 2u) * 4u)

RWByteAddressBuffer	da_cl_counts	: register(u0);
RWByteAddressBuffer	da_cl_args		: register(u1);	// the views' dispatches (32 bytes a view), then 20 bytes a draw

uniform float4	da_cl_view;		// x the view's first draw, y its draws, z the pool's indices, w the view
uniform float4	da_cl_view2;	// x the draws' first counter, y the args' first draw byte, z the list's room, w the second
								// list's room

groupshared uint	g_sum[1024];
groupshared uint	g_base;

[numthreads(1024, 1, 1)]
void	main	(uint3 tid : SV_GroupThreadID)
{
	const uint first = uint(da_cl_view.x), n = uint(da_cl_view.y), view = uint(da_cl_view.w);
	// Each lane a run of `per` consecutive draws.
	const uint per = (n + 1023u) / 1024u, d0 = tid.x * per;
	uint own = 0u;
	for (uint k = 0u; k < per; ++k)
	{
		const uint d = d0 + k;
		if (d < n)
			own += da_cl_counts.Load((uint(da_cl_view2.x) + first + d) * 4u);
	}
	g_sum[tid.x] = own;
	GroupMemoryBarrierWithGroupSync();
	// Hillis and Steele: the inclusive sums over the lanes.
	[unroll] for (uint step = 1u; step < 1024u; step <<= 1)
	{
		const uint add = tid.x >= step ? g_sum[tid.x - step] : 0u;
		GroupMemoryBarrierWithGroupSync();
		g_sum[tid.x] += add;
		GroupMemoryBarrierWithGroupSync();
	}
	if (tid.x == 0u)
	{
		const uint total = g_sum[1023];
		uint base;
		da_cl_counts.InterlockedAdd(DA_CL_POOL_TOP * 4u, total, base);
		g_base = base + total <= uint(da_cl_view.z) ? base : 0xFFFFFFFFu;
		// A view that does not fit gives its room back (the views run one after another on the queue).
		if (g_base == 0xFFFFFFFFu)
			da_cl_counts.InterlockedAdd(DA_CL_POOL_TOP * 4u, 0u - total);
	}
	GroupMemoryBarrierWithGroupSync();
	const bool fits = g_base != 0xFFFFFFFFu;
	uint start = g_base + g_sum[tid.x] - own;
	for (uint k = 0u; k < per; ++k)
	{
		const uint d = d0 + k;
		if (d >= n)
			break;
		const uint c = da_cl_counts.Load((uint(da_cl_view2.x) + first + d) * 4u);
		const uint a = uint(da_cl_view2.y) + (first + d) * 20u;
		da_cl_args.Store4(a, uint4(fits ? c : 0u, 1u, fits ? start : 0u, 0u));
		da_cl_args.Store(a + 16u, 0u);
		start += c;
	}
	if (tid.x == 0u)
	{
		const uint kept = fits ? min(da_cl_counts.Load(DA_CL_COUNT_VISIBLE(view)), uint(da_cl_view2.z)) : 0u;
		const uint rows = (kept + 65534u) / 65535u;
		da_cl_args.Store3(view * 32u, uint3(min(kept, 65535u), rows, kept ? 1u : 0u));
		const uint hidden = min(da_cl_counts.Load(DA_CL_COUNT_SECOND(view)), uint(da_cl_view2.w));
		const uint groups = (hidden + 63u) / 64u;
		da_cl_args.Store3(view * 32u + 16u, uint3(min(groups, 65535u), (groups + 65534u) / 65535u, groups ? 1u : 0u));
	}
}
