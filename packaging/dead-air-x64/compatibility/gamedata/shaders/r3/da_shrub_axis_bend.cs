// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural shrubs and the trees, the frame's bends by axis (docs/dead-air/SHRUBS.md): a plant with more elements to
// bend this frame than its template has axes - a near tree's leaves and twigs, a bush's sprays - has every axis bent
// once, and each element reads its axis's record (da_shrub_common.h shrub_bend). The slots' pass (da_shrub_bend.cs)
// walked the chain of axes again for every element of a twig: its leaves, its segments and its fruit all came out with
// the very same record. One lane an axis; a job is a run of up to 64 axes of one plant. Records of SHRUB_BEND_REC bytes
// from da_shrub_bend_axis.x: this frame's, then the view's plants' a frame back (the motion vectors).

#include "common.h"
#include "da_shrub_common.h"

RWByteAddressBuffer	da_shrub_bend_out	: register(u0);
ByteAddressBuffer	da_shrub_axis_jobs	: register(t7);	// 16 bytes a job: the plant's record, its first axis, the count, 1 in the view
uniform float4		da_shrub_axis0;		// x the first job, y the jobs, z groups a row

#include "da_shrub_bend_rec.h"

[numthreads(SHRUB_CHUNK, 1, 1)]
void	main	(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint g = gid.y * uint(da_shrub_axis0.z) + gid.x;
	if (g >= uint(da_shrub_axis0.y))
		return;
	const uint4 J = da_shrub_axis_jobs.Load4((uint(da_shrub_axis0.x) + g) * 16u);
	if (tid.x >= J.z)
		return;
	const shrub_inst I = shrub_inst_load(J.x);
	const uint axis = J.y + tid.x, r = I.axis_bend + axis;
	if (I.axis_bend == 0xFFFFFFFFu || r >= uint(da_shrub_bend_axis.y))
		return;
	bend_record((uint(da_shrub_bend_axis.x) + r) * SHRUB_BEND_REC, axis, I, false);
	[branch] if (da_shrub_time.z > 0.5f && (J.w & 1u) != 0u && r < uint(da_shrub_bend_axis.z))
		bend_record((uint(da_shrub_bend_axis.x) + uint(da_shrub_bend_axis.y) + r) * SHRUB_BEND_REC, axis, I, true);
}
