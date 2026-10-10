// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The snow shells (da_ground.cpp, docs/dead-air/SNOW.md): the snow lying on everything standing over the
// terrain - roofs, decks, vehicles, crates, slabs, fence tops - drawn from the collision's faces that look up,
// cut into triangles of a quarter metre. A vertex stands on its face raised by the roof's snow:
//   - the open snow's formula for the face's own slope (all of it to 32 degrees, none past 60) with its
//     level texel's noise and melt, a share of it (the wind sweeps an exposed roof);
//   - rounded off toward the face's open edges (an eave, a gable, a crate's rim) over about its own depth, a
//     quarter circle, so the snow ends in a soft bullnose on the edge and never as a slab or a curtain; a
//     support narrower than twice the depth (a fence board, a pipe, a rail) carries only the round of it;
//   - a vertex under another surface (an eave over a lower roof) is an edge too.
// The normal follows the rounding; the snow layer shades the shell as deep snow (da_snow_layer.ps).

#include "common.h"
#include "da_velocity.h"
#include "da_snow_common.h"
#include "da_snow_shell.h"

uniform float4 da_sn_shell; // x reach (m), y unused, z depth bias (NDC z), w unused

struct da_sn_shell_vin
{
	float4 p : POSITION;  // xyz on the face, w metres to the nearest open edge
	float4 n : TEXCOORD0; // the face's normal x, z, the way away from that edge x, z
};

// Height of the rounded snow over the face at `dist` from the edge, and its slope there: a quarter circle of
// about the depth's radius, never steeper than 40 degrees from the edge (no fin of snow on a narrow support).
float2 da_sn_round(float D, float dist)
{
	const float R = clamp(D, 0.06, 0.5);
	const float u = saturate(dist / R);
	const float c = sqrt(saturate(1.0 - (1.0 - u) * (1.0 - u)));
	const float h = D * c;
	[branch] if (0.85 * dist < h)
		return float2(0.85 * dist, 0.85);
	return float2(h, (u < 1.0) ? min((1.0 - u) / max(c, 0.25), 4.0) * D / R : 0.0);
}

da_sn_shell_v2p main(da_sn_shell_vin I)
{
	const float3 Nf = float3(I.n.x, sqrt(saturate(1.0 - I.n.x * I.n.x - I.n.y * I.n.y)), I.n.y);
	const float2 D = da_sn_roof_depth(I.p.xz, Nf.y);
	const float2 r = da_sn_round(D.x, I.p.w);
	const float2 rp = da_sn_round(D.y, I.p.w);
	// a centimetre over the face, so the snow wins over it at the very edge
	const float3 P = float3(I.p.x, I.p.y + r.x + 0.01, I.p.z);
	const float3 Pp = float3(I.p.x, I.p.y + rp.x + 0.01, I.p.z);
	const float dist = length(P.xz - eye_position.xz);

	da_sn_shell_v2p o;
	o.Nw = normalize(Nf - r.y * float3(I.n.z, 0.0, I.n.w));
	o.Nf = Nf;
	o.lift = float2(r.x + 0.01, I.p.w);
	o.Pw = P;
	o.Pv = mul(m_V, float4(P, 1.0));
	o.hpos = mul(m_VP, float4(P, 1.0));
	o.hpos.z -= da_sn_shell.z * o.hpos.w;
	o.hcur = mul(m_VP_nojit, float4(P, 1.0));
	o.hprev = mul(m_VP_prev, float4(Pp, 1.0));
	o.clip = min(D.x - 0.015, da_sn_shell.x - dist);
	return o;
}
