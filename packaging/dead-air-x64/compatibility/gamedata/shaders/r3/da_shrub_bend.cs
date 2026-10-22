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

void	bend_record	(uint b, shrub_elem E, shrub_inst I, bool prev)
{
	const shrub_wind W = shrub_wind_at(I, prev);
	// The element's own axis: its turn kept apart.
	const shrub_axis X0 = shrub_axis_load(I.axis0 + E.axis);
	float3 base0, k0;
	float A0;
	shrub_axis_turn_of(X0, I, W, base0, k0, A0);
	// The axes it hangs from (a shrub's four at most: a chain of five from the stem out - a thorn on a hawthorn's shoot;
	// a tree's six, up to its fixed stock axes, which move with the trunk's sway alone), each turning the composed motion
	// of the ones inside it about its own base.
	float4 Q = float4(0.0f, 0.0f, 0.0f, 1.0f);
	float3 T = float3(0.0f, 0.0f, 0.0f);
	uint a = X0.parent;
	float at = X0.attach;
	float left = SHRUB_CHAIN_TURN;
	[loop] for (uint lvl = 1u; lvl < 7u && a != SHRUB_NO_AXIS; ++lvl)
	{
		const shrub_axis X = shrub_axis_load(I.axis0 + a);
		if (X.fixed)
			break;
		float3 base, k;
		float A;
		shrub_axis_turn_of(X, I, W, base, k, A);
		[branch] if (A > 0.0f)
		{
			const float turn = min(A * sqrt(saturate(at / max(X.len, 1e-3f))), left);
			left -= turn;
			float sn, cs;
			sincos(0.5f * turn, sn, cs);
			const float4 R = float4(k * sn, cs);
			Q = shrub_qmul(R, Q);
			T = shrub_qrot(R, T - base) + base;
		}
		at = X.attach;
		a = X.parent;
	}
	da_shrub_bend_out.Store4(b, asuint(Q));
	da_shrub_bend_out.Store3(b + 16u, asuint(T));
	da_shrub_bend_out.Store2(b + 28u, uint2(shrub_oct_pack(k0), asuint(A0)));
}

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
	// (A rigid chunk rides a tree's fixed stock axes: its vertex shaders bend nothing.)
	if (tid.x >= (C.z & 0xFFu) || slot >= uint(da_shrub_bend_cap.x) || (C.z & SHRUB_CHUNK_RIGID) != 0u)
		return;
	const shrub_inst I = shrub_inst_load(C.x);
	const shrub_elem E = shrub_elem_load(C.y + tid.x, I.tree);
	bend_record(slot * SHRUB_BEND_REC, E, I, false);
	// A frame back for the view's slots only (the motion vectors; the shadow passes draw no velocity).
	[branch] if (da_shrub_time.z > 0.5f && chunk < uint(da_shrub_bend0.w) && slot < uint(da_shrub_bend_cap.y))
		bend_record((uint(da_shrub_bend_cap.x) + slot) * SHRUB_BEND_REC, E, I, true);
}
