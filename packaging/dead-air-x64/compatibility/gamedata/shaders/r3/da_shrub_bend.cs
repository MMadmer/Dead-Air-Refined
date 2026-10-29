// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural shrubs, the frame's bends (docs/dead-air/SHRUBS.md): one lane per slot of the frame's chunks (a group a
// chunk), each the bend of its element under the wind and the motors - the turn of the element's own axis kept apart
// (each vertex turns by its angle at the share of the axis it sits at) and the turns of the axes it hangs from
// composed into one rotation and a translation. The vertex shaders read the record (da_shrub_common.h shrub_bend)
// instead of walking the chain of axes for every vertex: a near willow's sprays walked it for each of their
// forty-eight vertices, twice. Records of SHRUB_BEND_REC bytes - the quaternion, the translation, the own axis's turn
// axis (octahedral) and its tip angle: the frame's slots first (da_shrub_bend_cap.x of them), then the view's a
// frame back for the motion vectors (the view's chunks come first in the frame, da_shrub_bend_cap.y slots). Slots past
// either the vertex shaders bend themselves.

#include "common.h"
#include "da_shrub_common.h"

RWByteAddressBuffer	da_shrub_bend_out	: register(u0);
uniform float4		da_shrub_bend0;		// x the first chunk, y the chunks, z groups a row, w the view's chunks (from 0)

#include "da_shrub_bend_rec.h"

[numthreads(SHRUB_CHUNK, 1, 1)]
void	main	(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint g = gid.y * uint(da_shrub_bend0.z) + gid.x;
	if (g >= uint(da_shrub_bend0.y))
		return;
	const uint chunk = uint(da_shrub_bend0.x) + g;
	const uint4 C = da_shrub_chunks.Load4(chunk * 16u);
	// (The chunk's own bend slot: the ones that bend numbered in order.)
	const uint slot = (C.w >> 16) * SHRUB_CHUNK + tid.x;
	// (A rigid chunk rides a tree's fixed stock axes: its vertex shaders bend nothing. A plant bent by its axes has their
	// records: da_shrub_axis_bend.cs.)
	if (tid.x >= (C.z & 0xFFu) || slot >= uint(da_shrub_bend_cap.x) || (C.z & SHRUB_CHUNK_RIGID) != 0u || (C.w >> 16) == SHRUB_CHUNK_AXES)
		return;
	const shrub_inst I = shrub_inst_load(C.x);
	const shrub_elem E = shrub_elem_load(C.y + tid.x, I.tree);
	bend_record(slot * SHRUB_BEND_REC, E.axis, I, false);
	// A frame back for the view's slots only (the motion vectors; the shadow passes draw no velocity).
	[branch] if (da_shrub_time.z > 0.5f && chunk < uint(da_shrub_bend0.w) && slot < uint(da_shrub_bend_cap.y))
		bend_record((uint(da_shrub_bend_cap.x) + slot) * SHRUB_BEND_REC, E.axis, I, true);
}
