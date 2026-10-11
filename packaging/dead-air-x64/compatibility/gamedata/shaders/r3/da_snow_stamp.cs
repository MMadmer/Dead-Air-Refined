// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Deformer stamps into a trail map (da_snow.cpp; RotTR packing): one group per stamp (feet, bodies,
// settled objects, craters of da_snow_stamps()), its threads walking the stamp's texels. A stamp is the
// capsule its deformer swept since the last frame (a disc when it stood still), so a fast leg or body
// leaves a continuous furrow. The stamped surface is the floor under the deformer rising to the snow surface
// at its radius - a print's flat sole and steep walls (dist^4), a body's or a furrow's bowl (dist^2), at
// least dist^2 * sharp -, merged with InterlockedMin so the deepest deformer wins; walls that stopped short
// of the surface at the radius stood as a step there, a staircase of texels along every trail. The floor:
//   pack > 0  a planted foot packs the loose snow under it to the creature's density and sinks until the
//             packed column carries it: D * rho_snow / pack over the ground (most of fresh powder, little
//             of old snow), never under the sole;
//   fall > 0  a swinging leg or a low body ploughs through the snow: what lies above it is pushed aside and
//             this share of it falls back into the furrow;
//   else      the deformer's bottom (settled objects, craters);
// never deeper than the creature's reach under the snow surface (sink). A resting object (kind 2) lies on the
// ground under the snow (its physics stands on the collision): it packs by its weight but no more than `sink` of
// it is buried - a share of its height (a rifle went under a drift whole, out of sight); a loose object (sink 0)
// lies on the flat floor of its own hollow, walled as a print (a bowl narrower than the snow grid's cells closed
// over it again). Partial carving: under the deformers
// stamped every frame (feet, bodies) a texel goes down by at most the frame's carve step from where it
// stands; a resting object or a crater (stamped once) carves at once. A crater's floor is bare ground (the
// word 0); every other deformer keeps the low half above 0. A deformer far below the snow base is under a
// roof or a bridge. A deformer standing on a cover (a roof, a trailer, a car: its foot at the top the level's 1 m
// field holds, 0.6 m over its terrain and more) presses the cover's snow instead, a shell's or the layer's coat:
// the roof map holds how far under that surface each texel is pressed (da_snow_shell.ps carves it in, the coat
// thins over it), the deepest deformer winning - a planted foot three quarters of the roof's snow, a body or a
// plough half, anything resting, a crater or a bullet nearly all of it.

#include "da_snow_common.h"

// per stamp: (x, z, sole y, radius), (sharp, kind, sink, pack), (x0, z0, sole y0, fall) - the capsule from
// (x0, z0) to (x, z)
StructuredBuffer<float4> s_sn_stamps : register(t8);

RWTexture2D<uint> u_map : register(u0);
// the roof map (the near window's alone): how far under the shells' surface, unorm over the cap
RWTexture2D<uint> u_roof : register(u1);
// the level's topmost surface and its terrain at 1 m (da_snow.cpp, R16_UNORM: y = da_sn_lvl.x + v * da_sn_lvl.y, 0 none)
Texture2D<float> s_da_sn_top;
Texture2D<float> s_da_sn_terrain;

uniform float4 da_sn_map;   // x texels per metre, y texels per side, zw first world texel of the window
uniform float4 da_sn_map2;  // x texel (m), y carve step this frame (m), z density of the open snow (kg/m3), w smallest radius (m)
uniform float4 da_sn_stamp; // x first stamp of this dispatch, y stamp count, z 1 = the near window (the roof map too)

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint i = gid.x + gid.y * 1024u;
	[branch] if (i >= uint(da_sn_stamp.y))
		return;
	const uint s = i + uint(da_sn_stamp.x);
	const float4 a = s_sn_stamps[s * 3u];
	const float4 b = s_sn_stamps[s * 3u + 1u];
	const float4 c = s_sn_stamps[s * 3u + 2u];
	const float radius = max(a.w, da_sn_map2.w);
	const int n = int(da_sn_map.y);
	const int2 w0 = int2(da_sn_map.zw);
	const int2 lo = max(int2(floor((min(a.xy, c.xy) - radius) * da_sn_map.x)), w0);
	const int2 hi = min(int2(floor((max(a.xy, c.xy) + radius) * da_sn_map.x)) + 1, w0 + n);
	const float2 seg = a.xy - c.xy;
	const float seg2 = max(dot(seg, seg), 1e-8);

	[loop] for (int z = lo.y + int(tid.y); z < hi.y; z += 8)
	{
		[loop] for (int x = lo.x + int(tid.x); x < hi.x; x += 8)
		{
			const float2 p = (float2(x, z) + 0.5) * da_sn_map2.x;
			// the nearest point of the swept segment, and the deformer's bottom there
			const float t = saturate(dot(p - c.xy, seg) / seg2);
			const float2 dd = (p - (c.xy + seg * t)) / radius;
			const float r2 = dot(dd, dd);
			[branch] if (r2 > 1.0)
				continue;
			const float foot = lerp(c.z, a.z, t);
			const int2 addr = da_sn_wrap(int2(x, z), n);
			// On a cover (the level's top over its terrain), the foot on it (under the metre's highest point by a slope's run
			// over it and the beams a roof carries): the snow a shell or the layer's coat lays there.
			[branch] if (da_sn_stamp.z > 0.5)
			{
				const int3 tl = int3(int2(floor(p * da_snow_level_xf.xy + da_snow_level_xf.zw)), 0);
				const float vt = s_da_sn_top.Load(tl), vg = s_da_sn_terrain.Load(tl);
				const float top = da_sn_lvl.x + vt * da_sn_lvl.y, terrain = da_sn_lvl.x + vg * da_sn_lvl.y;
				const float Dr = da_sn_roof_depth(p, 1.0).x;
				// (any cover a stride's height up - a trailer, a car, a crate pile - where the coat takes the prints; the foot
				// in the cover's upper half, so a boot beside a low one in the same metre still prints the ground)
				[branch] if (vt > 0.0 && vg > 0.0 && top > terrain + 0.6 && foot > top - min(0.8, 0.5 * (top - terrain)) &&
					foot < top + Dr + 0.1)
				{
					[branch] if (Dr > 0.01)
					{
						const float share = (b.y < 0.5) ? 0.75 : ((b.y < 1.5) ? 0.5 : 0.85);
						const float wall = (b.y < 0.5) ? r2 * r2 : r2;
						InterlockedMax(u_roof[addr], da_sn_u16(Dr * share * (1.0 - wall)));
					}
					continue;
				}
			}
			const float3 base = da_sn_base(p);
			[branch] if (base.x < DA_SN_NONE * 0.5 || base.z > 0.5)
				continue;
			const float sole = foot - base.x;
			[branch] if (sole < -0.5)
				continue;
			// the depth the composite resolves against (the same smooth season)
			const float D = da_sn_depth_smooth(p);
			[branch] if (D < 0.005)
				continue;
			const float s0 = max(sole, 0.0);
			const bool resting = b.y > 1.5 && b.y < 2.5;
			float floor_h = s0;
			[branch] if (b.w > 0.0)
			{
				floor_h = max(s0, D * min(da_sn_map2.z / b.w, 1.0));
				[flatten] if (resting)
					floor_h = min(floor_h, s0 + b.z);
			}
			else if (c.w > 0.0)
			{
				// a leg skimming the surface leaves no furrow: drag marks in deep snow, clean prints in shallow
				[branch] if (D - s0 < 0.04)
					continue;
				floor_h = s0 + c.w * max(D - s0, 0.0);
			}
			[flatten] if (b.z > 0.0 && !resting)
				floor_h = max(floor_h, D - b.z);
			// a print's walls (dist^4: a flat floor, steep walls) for feet and craters (a blast's or a bullet's), flatter
			// still for a loose object (dist^8: its discs' floors join into one), a bowl (dist^2) for bodies, ploughs
			// and a crater asked for one (sink > 0: a fire's thaw)
			const bool loose = resting && b.z <= 0.0;
			const float wall = loose ? (r2 * r2) * (r2 * r2) : ((b.y < 0.5 || (b.y > 2.5 && b.z <= 0.0)) ? r2 * r2 : r2);
			const float target = floor_h + max(r2 * b.x, max(D - floor_h, 0.0) * wall);
			const uint cur = u_map[addr];
			const float now = (cur == DA_SN_UNTOUCHED) ? D : min(D, da_sn_f16(cur >> 16));
			const float h = (b.y < 1.5) ? max(target, now - da_sn_map2.y) : target;
			// (0: no compacted floor under it - a crater, a loose object lying on the ground under the snow)
			const uint low = (b.y > 2.5 || loose) ? 0u : max(da_sn_u16(s0), 1u);
			InterlockedMin(u_map[addr], (da_sn_u16(h) << 16) | low);
		}
	}
}
