// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Deformer stamps into a trail map (da_snow.cpp; RotTR packing): one group per stamp (feet, body
// discs, settled objects, craters of da_snow_stamps()), its threads walking the stamp's texels.
// The stamped surface is the deformer's bottom plus dist^2 * sharp (a soft-edged print), merged with
// InterlockedMin so the deepest deformer wins. A foot or a body sinks no deeper than its creature's
// sink under the snow surface (a rat 1.5 cm, a dog 4.5 cm, a man 6.5 cm, a pseudogiant 15 cm: the
// snow carries the rest), whatever depth its sole reaches. Partial carving: under the deformers stamped
// every frame (feet, bodies) a texel goes down by at most the frame's carve step from where it stands,
// so a body pressing in takes a few frames to sink instead of cutting through the snow at once; a
// resting object or a crater (stamped once) carves at once. A crater's floor is bare ground (the word
// 0); every other deformer keeps the low half above 0. A deformer far below the snow base is under a
// roof or a bridge.

#include "da_snow_common.h"

StructuredBuffer<float4> s_sn_stamps : register(t8); // per stamp: (x, z, sole y, radius), (sharp, kind, sink, 0)

RWTexture2D<uint> u_map : register(u0);

uniform float4 da_sn_map;  // x texels per metre, y texels per side, zw first world texel of the window
uniform float4 da_sn_map2;  // x texel (m), y carve step this frame (m), z unused, w smallest radius (m)
uniform float4 da_sn_stamp; // x first stamp of this dispatch, y stamp count

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint i = gid.x + gid.y * 1024u;
	[branch] if (i >= uint(da_sn_stamp.y))
		return;
	const uint s = i + uint(da_sn_stamp.x);
	const float4 a = s_sn_stamps[s * 2u];
	const float4 b = s_sn_stamps[s * 2u + 1u];
	const float radius = max(a.w, da_sn_map2.w);
	const int n = int(da_sn_map.y);
	const int2 w0 = int2(da_sn_map.zw);
	const int2 lo = max(int2(floor((a.xy - radius) * da_sn_map.x)), w0);
	const int2 hi = min(int2(floor((a.xy + radius) * da_sn_map.x)) + 1, w0 + n);

	[loop] for (int z = lo.y + int(tid.y); z < hi.y; z += 8)
	{
		[loop] for (int x = lo.x + int(tid.x); x < hi.x; x += 8)
		{
			const float2 p = (float2(x, z) + 0.5) * da_sn_map2.x;
			const float2 dd = (p - a.xy) / radius;
			const float r2 = dot(dd, dd);
			[branch] if (r2 > 1.0)
				continue;
			const float3 base = da_sn_base(p);
			[branch] if (base.x < DA_SN_NONE * 0.5 || base.z > 0.5)
				continue;
			const float sole = a.z - base.x;
			[branch] if (sole < -0.5)
				continue;
			// the depth the composite resolves against (the same smooth season)
			const float D = da_sn_depth_smooth(p);
			[branch] if (D < 0.005)
				continue;
			const float floor_h = (b.z > 0.0) ? max(max(sole, 0.0), D - b.z) : max(sole, 0.0);
			const float target = floor_h + r2 * b.x;
			const int2 addr = da_sn_wrap(int2(x, z), n);
			const uint cur = u_map[addr];
			const float now = (cur == DA_SN_UNTOUCHED) ? D : min(D, da_sn_f16(cur >> 16));
			const float h = (b.y < 1.5) ? max(target, now - da_sn_map2.y) : target;
			const uint low = (b.y > 2.5) ? 0u : max(da_sn_u16(max(sole, 0.0)), 1u);
			InterlockedMin(u_map[addr], (da_sn_u16(h) << 16) | low);
		}
	}
}
