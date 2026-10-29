// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural shrubs and the trees, one bend record (docs/dead-air/SHRUBS.md): the bend of a point on axis `axis` under
// the wind and the motors - the turn of the axis itself kept apart (each vertex turns by its angle at the share of the
// axis it sits at) and the turns of the axes it hangs from composed into one rotation and a translation. Written by
// the slots' pass (da_shrub_bend.cs: an element's record) and the axes' (da_shrub_axis_bend.cs: an axis's, which every
// element on it reads). The including shader declares da_shrub_bend_out.

#ifndef DA_SHRUB_BEND_REC_H
#define DA_SHRUB_BEND_REC_H

void	bend_record	(uint b, uint axis, shrub_inst I, bool prev)
{
	const shrub_wind W = shrub_wind_at(I, prev);
	// The element's own axis: its turn kept apart.
	const shrub_axis X0 = shrub_axis_load(I.axis0 + axis);
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

#endif
