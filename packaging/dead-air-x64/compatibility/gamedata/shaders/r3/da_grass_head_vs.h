// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Blade grass, the seed heads (docs/dead-air/GRASS.md): a second indirect draw of procedural micro-meshes
// on the culms the generation headed. A head is a screen-facing core ribbon continuing its culm from the
// top plus branch ribbons (quadratic, opening with the seed stage, drooping under their weight); the
// vertex shader reads the culm's record and evaluates the culm's top for this and the previous frame, so
// head and culm never part and the motion vectors have no seam. 43 vertices near (core 13 + 6 branches
// of 5), 13 in the middle distance (core 7 + 2 envelope branches of 3); far away the culm's own triangle
// carries the head. The spike types are the core alone; a panicle is a thin rachis and its branches: the
// dense Calamagrostis panicle a spindle of six short branches pressed to the axis, the open panicles and
// plumes wider and drooping. The pixel shader (da_grass_head.ps) builds every ribbon out of spikelets -
// the head's own grains of its type, with air between them - and cuts the lobes and the fluffy rim, so no
// ribbon reads as a flat card. GRASS_SHADOW: the middle-distance head facing the light in the near sun
// cascade.

#include "common.h"
#include "da_velocity.h"
#include "da_grass_shape.h"

ByteAddressBuffer	da_grass_heads	: register(t3);

#ifdef GRASS_SHADOW
struct v2p_grass_shadow
{
	float4	hpos	: SV_Position;
};
#define GRASS_OUT	v2p_grass_shadow
#else
// The blades' interpolants and the spikelet frame of the ribbon (da_grass_head.ps).
struct v2p_grass_head
{
	float4	hpos		: SV_Position;
	float4	hpos_cur	: TEXCOORD0;
	float4	hpos_prev	: TEXCOORD1;
	float4	pv_t		: TEXCOORD2;
	float4	nb_u		: TEXCOORD3;
	float4	lat_g		: TEXCOORD4;
	float4	nc			: TEXCOORD5;
	float4	alb_h		: TEXCOORD6;
	float4	misc		: TEXCOORD7;
	float4	sp			: TEXCOORD8;	// x metres along the head, y metres across the ribbon, z ribbon seed, w type
	float4	sp2			: TEXCOORD9;	// x the species' grain scale
};
#define GRASS_OUT	v2p_grass_head
#endif

// Head types: 1 narrow spike, 2 cylinder, 3 one-sided cluster, 4 open pyramid, 5 airy cloud, 6 dense lobed
// panicle, 7 plume, 8 nodding awned, 9 sedge spikes; the forbs' 10 flat corymb (yarrow, tansy, St John's
// wort), 11 golden panicle of arching one-sided racemes (goldenrod), 12 panicle of tiny heads (mugwort,
// fleabane, lady's bedstraw, nettle), 13 compound umbel (cow parsley), 14 raceme of large flowers (fireweed,
// loosestrife, lily of the valley), 15 globe: a round head of florets (clovers, dandelion and its clock,
// knapweed, sheep's-bit).
bool	grass_head_rayed	(uint type)	{ return type == 10u || type == 13u; }

// The build of a head type: x core width / spread, y branch width / spread, z branch length / head
// length, w branch droop.
float4	grass_head_build	(uint type)
{
	if (type == 1u || type == 2u || type == 14u || type == 15u)
		return float4(1.0f, 0.0f, 0.0f, 0.0f);
	if (type == 9u)
		return float4(1.0f, 0.9f, 0.35f, 0.2f);
	if (type == 3u)
		return float4(0.12f, 0.26f, 0.45f, 0.1f);
	if (type == 6u)
		return float4(0.12f, 0.42f, 0.4f, 0.06f);
	if (type == 4u)
		return float4(0.08f, 0.09f, 0.45f, 0.35f);
	if (type == 5u)
		return float4(0.06f, 0.08f, 0.5f, 0.3f);
	if (type == 7u)
		return float4(0.1f, 0.16f, 0.5f, 0.4f);
	if (type == 10u)
		return float4(0.06f, 0.15f, 1.0f, 0.0f);
	if (type == 11u)
		return float4(0.1f, 0.12f, 0.55f, 0.55f);
	if (type == 12u)
		return float4(0.14f, 0.14f, 0.35f, 0.1f);
	if (type == 13u)
		return float4(0.04f, 0.2f, 1.0f, 0.0f);
	return float4(0.1f, 0.12f, 0.5f, 0.45f);
}

// Spikelet (floret) lobes along a head and how fluffy its rim is (0..1), for the pixel shader.
float2	grass_head_texture	(uint type)
{
	if (type == 1u)
		return float2(7.0f, 0.3f);
	if (type == 2u)
		return float2(12.0f, 0.5f);
	if (type == 9u)
		return float2(9.0f, 0.15f);
	if (type == 6u)
		return float2(5.0f, 0.55f);
	if (type == 5u || type == 7u)
		return float2(6.0f, 0.7f);
	if (type == 10u || type == 13u || type == 14u)
		return float2(0.0f, 0.2f);
	if (type == 15u)
		return float2(0.0f, 0.45f);
	if (type == 11u)
		return float2(6.0f, 0.4f);
	if (type == 12u)
		return float2(9.0f, 0.35f);
	return float2(5.0f, 0.35f);
}

// The width envelope of a head's core along it (s: 0 at the culm top .. 1 at the head's tip).
float	grass_head_env	(uint type, float s)
{
	const float pi = 3.14159265f;
	if (type == 1u)
		return smoothstep(0.0f, 0.08f, s) * (1.0f - 0.6f * smoothstep(0.85f, 1.0f, s));
	if (type == 2u || type == 9u)
		return smoothstep(0.0f, 0.05f, s) * (1.0f - 0.3f * smoothstep(0.9f, 1.0f, s));
	if (type == 3u)
		return 1.0f - 0.5f * s;
	if (type == 4u || type == 11u)
		return pow(saturate(1.0f - s), 0.8f);
	if (type == 5u || type == 12u)
		return pow(saturate(sin(pi * s)), 0.5f);
	if (type == 6u)
		return pow(saturate(sin(pi * pow(s, 0.85f))), 0.7f);
	if (type == 7u)
		return pow(saturate(sin(pi * pow(s, 0.8f))), 0.6f);
	if (type == 14u)
		return smoothstep(0.0f, 0.06f, s) * pow(saturate(1.0f - s), 0.4f);
	// A globe is round: its chord across the core.
	if (type == 15u)
		return sqrt(saturate(1.0f - (2.0f * s - 1.0f) * (2.0f * s - 1.0f)));
	return pow(saturate(1.0f - s), 0.6f);
}

// The head's axis frame at the culm's top: axis A (nodding), side X, Y.
void	grass_head_frame	(float3 T, float2 facing, float nod, out float3 A, out float3 X, out float3 Y)
{
	const float3 fwd = float3(facing.x, 0.0f, facing.y);
	const float3 perp = fwd - T * dot(fwd, T);
	const float pl = length(perp);
	float sn, cn;
	sincos(nod, sn, cn);
	A = normalize(T * cn + (pl > 1e-4f ? perp / pl : float3(1.0f, 0.0f, 0.0f)) * sn);
	X = normalize(cross(A, float3(0.0f, 0.0f, 1.0f)) + cross(A, float3(1.0f, 0.0f, 0.0f)) * 0.01f);
	Y = cross(A, X);
}

// The point of head vertex v from the culm's top P with tangent T (Wc: core ribbon width, Wb: branch ribbon
// width, build: grass_head_build, spread: the head's width, Hx: the horizontal across the view). Returns the
// point; outputs the local tangent (for the screen-facing side), the width, the side and s along the head.
// The places along a cattail's head of its core's pairs (6 near, 3 in the middle distance): the female spike's rounded
// foot and shoulder, its join to the male spike, the male spike's tip (spread evenly, the sausage read as a spindle).
static const float grass_velvet_s6[6] = { 0.0f, 0.05f, 0.53f, 0.575f, 0.62f, 0.93f };
static const float grass_velvet_s3[3] = { 0.0f, 0.55f, 0.6f };

float3	grass_head_point	(uint v, uint core_pairs, uint branch_pairs, uint type, float branches, float open_angle,
	float len, float Wc, float Wb, float4 build, float nod, float branch_k, float hash, float3 P, float3 T, float2 facing,
	float spread, float3 Hx, bool velvet, out float3 tangent, out float width, out float side, out float s, out float rib)
{
	float3 A, X, Y;
	grass_head_frame(T, facing, nod, A, X, Y);
	const float3 up = float3(0.0f, 1.0f, 0.0f);
	// Heavy heads droop: the axis bends down along its length.
	const float droop = 0.5f * sin(nod) + (type == 8u ? 0.25f : 0.0f);
	const uint core_v = 2u * core_pairs + 1u;
	const bool rayed = grass_head_rayed(type);
	float3 at;
	if (v < core_v)
	{
		const uint p = v >> 1;
		const bool tip = v == 2u * core_pairs;
		rib = 0.0f;
		side = tip ? 0.0f : ((v & 1u) ? 1.0f : -1.0f);
		s = tip ? 1.0f : (velvet ? (core_pairs >= 6u ? grass_velvet_s6[min(p, 5u)] : grass_velvet_s3[min(p, 2u)])
			: float(p) / float(core_pairs));
		// The florets of a corymb or an umbel: a flat disc on the rays' ends, the core its diameter across the view
		// and its width the chord there (the vertex shader lays the chord level), an umbel's a little domed.
		if (rayed)
		{
			const float x = 2.0f * s - 1.0f;
			const float r = 0.5f * spread;
			width = tip ? 0.0f : 2.0f * r * sqrt(saturate(1.0f - x * x));
			tangent = Hx;
			at = P + A * len + Hx * (x * r) + up * ((type == 13u ? 0.3f : 0.1f) * r * (1.0f - x * x));
		}
		else
		{
			width = tip ? 0.0f : Wc * grass_head_env(type, s);
			tangent = normalize(A - up * (2.0f * droop * s));
			at = P + A * (len * s) - up * (droop * len * s * s);
		}
	}
	else
	{
		const uint bv = 2u * branch_pairs + 1u;
		const uint j = (v - core_v) / bv;
		const uint b = (v - core_v) % bv;
		const uint p = b >> 1;
		const bool tip = b == 2u * branch_pairs;
		rib = float(j + 1u);
		side = tip ? 0.0f : ((b & 1u) ? 1.0f : -1.0f);
		const float sb = tip ? 1.0f : float(p) / float(branch_pairs);
		// Branch j of the type's count, spread along the axis from near its base; slots beyond the count
		// collapse to their attach point (zero area). The rays of an umbel all leave one point; a corymb's
		// branches leave the lower two thirds of its axis.
		const float nb = max(branches, 1.0f);
		const bool used = float(j) < branches;
		const float sj = !used ? 0.5f : (type == 13u ? 0.02f
			: (type == 10u ? 0.05f + 0.6f * (float(j) + 0.5f) / nb : 0.06f + 0.72f * (float(j) + 0.5f) / nb));
		const float3 attach = P + A * (len * sj) - up * (droop * len * sj * sj);
		// One-sided types branch to one side, the others in a 137.5 degree whorl.
		const bool one_sided = type == 3u || type == 8u;
		const float phi = one_sided ? ((j & 1u) ? 0.7f : -0.7f) : float(j) * 2.3999632f + hash * 6.2831853f;
		float sp, cp, so, co;
		sincos(phi, sp, cp);
		sincos(open_angle, so, co);
		float3 dir = normalize(A * co + (X * cp + Y * sp) * so);
		// Lower branches are the longest; a branch is a cluster of spikelets, widest in its middle. The rays of a
		// corymb or an umbel are thin stalks up to points under the disc; an umbel's end in their umbellets.
		float l = !used ? 0.0f : len * build.z * (1.0f - 0.45f * sj) * branch_k;
		if (rayed)
		{
			const float rho = 0.5f * spread * (0.3f + 0.6f * frac(float(j) * 0.618034f + hash));
			const float3 ray = P + A * len + (X * cp + Y * sp) * rho - attach;
			const float rl = max(length(ray), 1e-4f);
			dir = ray / rl;
			l = used ? rl * branch_k : 0.0f;
		}
		s = sj + sb * l / max(len, 1e-3f);
		const float prof = rayed ? (type == 13u ? lerp(0.2f, 1.0f, smoothstep(0.7f, 0.95f, sb)) : 0.2f)
			: pow(saturate(sin(3.14159265f * (0.2f + 0.8f * sb))), 0.6f);
		width = (used && !tip) ? Wb * prof * branch_k : 0.0f;
		tangent = normalize(dir - up * (2.0f * build.w * sb));
		at = attach + dir * (l * sb) - up * (build.w * l * sb * sb);
	}
	return at;
}

GRASS_OUT	main	(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
	GRASS_OUT O = (GRASS_OUT)0;
	const uint V = uint(da_grass_draw.z);
	const uint head_i = inst * 32u + vid / V;
	const uint v = vid % V;
	const uint bin = uint(da_grass_draw.y);
	// Past the drawn count, or a head whose culm found no room in its bin: collapsed.
	if (head_i >= da_grass_counts.Load((GRASS_NBINS + bin) * 4u))
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const uint4 H = da_grass_heads.Load4((uint(da_grass_draw.x) + head_i) * GRASS_HEAD_REC);
	if (H.x == 0xFFFFFFFFu)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const grass_blade B = grass_load(H.x);
	const uint p = grass_profile(B);
	const uint type = clamp(H.y & 15u, 1u, 15u);
	const bool thatch = ((H.y >> 4) & 1u) != 0u;
	const float hash = float(H.y >> 16) / 65536.0f;
	const float2 lw = grass_unpack_h2(H.z), sh = grass_unpack_h2(H.w);
	const float stage = sh.x;
	const uint lod = uint(da_grass_draw.w);
	const bool near_lod = lod == 0u;
	// The species' head: branches, opening at anthesis and when ripe (degrees), grain scale.
	const float4 P7 = grass_prof(p, 7), P10 = grass_prof(p, 10), P11 = grass_prof(p, 11), P12 = grass_prof(p, 12);
	const float4 HT1 = float4(P11.z, P12.x, P12.y, 0.0f);
	const float4 build = grass_head_build(type);

	const float3 cam = da_grass_vcam.xyz;
	const float d = distance(B.root, cam);
	// Near heads lose their branches in the last fifth before d1; middle heads fold away before d2.
	float branch_k = 1.0f, head_k = 1.0f;
#ifndef GRASS_SHADOW
	if (near_lod)
		branch_k = 1.0f - smoothstep(0.8f * da_grass_vlod.x, da_grass_vlod.x, d);
	else
		head_k = 1.0f - smoothstep(0.8f * da_grass_vlod.y, da_grass_vlod.y, d);
#endif
	// Seed stage: opening from heading to anthesis, then towards the ripe angle; ripe heads fluff up,
	// shattered ones thin out.
	const float open_angle = radians(stage < 0.5f ? HT1.y * smoothstep(0.25f, 0.5f, stage)
		: lerp(HT1.y, HT1.z, saturate((stage - 0.5f) * 2.0f)));
	const float fluff = (1.0f + 0.25f * smoothstep(0.7f, 0.9f, stage)) * lerp(1.0f, 0.6f, smoothstep(0.95f, 1.0f, stage));
	float branches = build.y > 0.0f ? HT1.x : 0.0f;
	if (stage >= 0.999f && hash < 0.5f)
		branches = floor(branches * 0.5f);
	const float len = lw.x * head_k;
	// lw.y is the spread of the whole head: the spike types are the core alone, a panicle a thin rachis
	// (never under 2.5 mm) whose branches carry the spread.
	const float spread = lw.y * fluff * head_k;
	const float W_core = max(spread * build.x, 0.0025f * head_k);
	const float Wb = spread * build.y;
	// The middle-distance envelope: the core as wide as the branches spread (the dense and one-sided
	// panicles), two branch ribbons for the open types. A near head's core grows into that envelope while
	// its branches fade before d1, so the switch does not pop.
	const uint core_pairs = near_lod ? 6u : 3u;
	const uint branch_pairs = near_lod ? 2u : 1u;
	const bool open_type = type == 4u || type == 5u || type == 7u || type == 8u || type == 10u || type == 11u || type == 13u;
	const float W_env = (build.y > 0.0f && !open_type) ? max(W_core, spread * 0.45f) : W_core;
	const float W = near_lod ? lerp(W_env, W_core, branch_k) : W_env;
	if (!near_lod)
		branches = open_type ? 2.0f : 0.0f;
	const float nod = lerp(P7.y, P7.z, hash) * stage;

	float3 Ptop, dTop, Sx;
	const float2 bob = grass_stem_bob(p, true, P12.zw);
	grass_curve(B.cur, B.root, 1.0f, 0.0f, bob, da_grass_vlod.z, grass_hash01(B), P7.w, Ptop, dTop, Sx);
	// A corymb's or an umbel's disc opens from the buds' knot to its full width by anthesis; its diameter runs
	// level across the view (Hx), the same on the previous frame.
	const bool rayed = grass_head_rayed(type);
	const float spread_h = rayed ? spread * lerp(0.35f, 1.0f, smoothstep(0.2f, 0.5f, stage)) : spread;
	const float3 cam0 = da_grass_vcam.xyz;
	const float3 Hx = normalize(cross(float3(0.0f, 1.0f, 0.0f), cam0 - Ptop) + float3(1e-4f, 0.0f, 0.0f));
	const bool disc = rayed && v < 2u * core_pairs + 1u;
	float3 tangent;
	float width, side, s, rib;
	// A cattail's head (profile row 19 w): the female spike a smooth velvet cylinder over the lower 58 % of the head,
	// blunt at its ends, the male spike right above it a third as thick and withering to a bare spike; no lobes, no
	// fluff, no air between grains (as a spike type its brown sausage read as a granular dock).
	const bool velvet = grass_prof(p, 19).w > 0.5f;
	float3 C = grass_head_point(v, core_pairs, branch_pairs, type, branches, open_angle, len, W, Wb, build, nod,
		branch_k, hash, Ptop, normalize(dTop), B.cur.facing, spread_h, Hx, velvet, tangent, width, side, s, rib);
	const float male = velvet ? smoothstep(0.56f, 0.6f, s) : 0.0f;
	if (velvet && v < 2u * core_pairs + 1u)
		width = (side == 0.0f) ? 0.0f : W * lerp(lerp(0.6f, 1.0f, smoothstep(0.0f, 0.05f, s)) * (1.0f - 0.2f * smoothstep(0.53f, 0.575f, s)),
			lerp(0.36f, 0.22f, smoothstep(0.6f, 0.75f, stage)) * (1.0f - 0.6f * smoothstep(0.85f, 1.0f, s)), male);

	// The disc's chord lies level (Hz), with the florets' own depth upright: at least the pixel floor, so a disc
	// seen edge-on stays a line.
	const float3 Hz = cross(Hx, float3(0.0f, 1.0f, 0.0f));
#ifdef GRASS_SHADOW
	const float3 L = da_grass_vsun.xyz;
	const float3 R = normalize(cross(tangent, L) + float3(1e-6f, 0.0f, 0.0f));
	const float hw = 0.5f * max(width, side != 0.0f ? da_grass_vsun.w : 0.0f);
	const float3 off_s = disc ? Hz * (side * 0.5f * width) + float3(0.0f, side * 0.5f * max(0.006f, da_grass_vsun.w), 0.0f)
		: R * (side * hw);
	O.hpos = mul(m_WVP, float4(C + off_s, 1.0f));
	return O;
#else
	const float3 Vw = normalize(cam - C);
	const float3 R = normalize(cross(tangent, Vw) + float3(1e-6f, 0.0f, 0.0f));
	const float view_z = max(dot(C - cam, da_grass_vfwd.xyz), 0.05f);
	const float hw = disc ? 0.5f * width : 0.5f * max(width, (side != 0.0f && width > 0.0f) ? da_grass_vcam.w * view_z : 0.0f);
	const float3 off = disc ? Hz * (side * hw) + float3(0.0f, side * 0.5f * max(0.006f, da_grass_vcam.w * view_z), 0.0f)
		: R * (side * hw);
	const float3 pos = C + off;

	float3 Cp = C;
	[branch] if (da_grass_vmisc.y > 0.5f)
	{
		float3 Pp, dPp, Sp, tp;
		float wp, sdp, spp, rp;
		grass_curve(B.prev, B.root_prev, 1.0f, 0.0f, bob, da_grass_vlod.w, grass_hash01(B), P7.w, Pp, dPp, Sp);
		Cp = grass_head_point(v, core_pairs, branch_pairs, type, branches, open_angle, len, W, Wb, build, nod, branch_k,
			hash, Pp, normalize(dPp), B.prev.facing, spread_h, Hx, velvet, tp, wp, sdp, spp, rp);
	}
	const float3 pos_prev = Cp + off;

	O.hpos = mul(m_WVP, float4(pos, 1.0f));
	O.hpos_cur = mul(m_WVP_nojit, float4(pos, 1.0f));
	O.hpos_prev = mul(m_WVP_prev, float4(pos_prev, 1.0f));
	O.pv_t = float4(mul(m_WV, float4(pos, 1.0f)).xyz, s);

	// A rounded cylinder facing the camera: 60 degrees near, 20 at d2; a disc faces the sky, rounded at its rim.
	const float3 nb = disc ? float3(0.0f, 1.0f, 0.0f) : normalize(Vw - tangent * dot(Vw, tangent));
	float sr, cr;
	sincos(radians(lerp(60.0f, 20.0f, saturate(d / da_grass_vlod.y))), sr, cr);
	O.nb_u = float4(mul((float3x3)m_WV, nb * cr), side * 0.5f + 0.5f);
	O.lat_g = float4(mul((float3x3)m_WV, (disc ? Hz : R) * (side * sr)), 0.015f * lerp(1.0f, 0.4f, smoothstep(15.0f, 60.0f, d)));
	O.nc = float4(mul((float3x3)m_WV, grass_canopy_normal(B, cam - B.root)), 0.5f * grass_canopy_weight(d));
	const float y_rel = (C.y - B.root.y) / grass_canopy_h(p);
	// Beyond the near LOD a head is a few pixels standing out of the canopy against the darker leaves:
	// toned down, so the heads read as a haze over the field rather than a field of lit knobs.
	const float far_k = smoothstep(0.6f * da_grass_vlod.x, 1.5f * da_grass_vlod.x, d);
	float3 c_head = grass_head_colour(B, p, stage) * lerp(1.0f, 0.82f, far_k);
	// The male spike yellow with pollen while it flowers, grey-brown and thin once it withers.
	c_head = lerp(c_head, lerp(float3(0.42f, 0.33f, 0.12f), float3(0.17f, 0.14f, 0.1f), smoothstep(0.45f, 0.65f, stage)), male);
	O.alb_h = float4(grass_to_albedo(c_head), grass_sky(B.hemi) * grass_ao(d, B.ao, y_rel));
	// Translucency: flowering, ripe and fluffy, weathered (where back-lit).
	const float tr = grass_translucency(thatch ? 0.45f : (stage < 0.6f ? 0.55f : 0.7f), Vw);
	// Spikelet lobes and the rim's fluff for the pixel shader (lobes + fluff in one value); ripe heads
	// are fluffier, the far ones solid.
	const float2 tex = grass_head_texture(type);
	// A forb's flowers are petals and florets, not a fluffy panicle: their rim stays nearly whole.
	const bool forb_head = uint(grass_prof(p, 19).z + 0.5f) != GRASS_FORM_GRAMINOID;
	const float fuzz = tex.y * lerp(0.6f, 1.0f, smoothstep(0.5f, 0.85f, stage)) * (thatch ? 0.7f : 1.0f) * (forb_head ? 0.35f : 1.0f);
	O.misc = float4(tr, 2.0f, near_lod ? 1.0f : 0.0f, velvet ? 0.0f : tex.x + min(fuzz, 0.95f));
	// The spikelet frame: metres along the head and across the ribbon (the drawn half-width, the pixel
	// floor included), a seed per ribbon so no two branches share their grains.
	O.sp = float4(disc ? (2.0f * s - 1.0f) * 0.5f * spread_h : s * len, side * hw, frac(hash * 7.31f + rib * 0.618034f) * 512.0f,
		float(type));
	// The species' grain scale, its flowers' petals (0: round grains), a disc's radius (0: a ribbon).
	// (A cattail's velvet: the petals' slot at -1, the pixel shader keeps it whole.)
	O.sp2 = float4(max(P10.w, 0.25f), velvet ? -1.0f : P11.w, disc ? 0.5f * spread_h : 0.0f, forb_head ? 1.0f : 0.0f);
	return O;
#endif
}
