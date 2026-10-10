// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Trees, the view's GPU cut (docs/dead-air/TREES.md): one lane per element of the frame's candidate chunks (a group a
// chunk: da_veg.cpp cut, da_trees.cpp view), each element deciding alone - out of the view by its own bent place, under
// the wire rule at its own depth, its wood's sides and its leaves' tier by its own distance and depth - and joining the
// list of the draw it takes (its route's: shrub_vertex reads the lists). The tree's own choice drew a near crown's far
// side at its near side's detail, and the vertex shaders ran the part of a near crown out of the view and its
// sub-pixel twigs only to throw them away: two near trees were three milliseconds.

#include "common.h"
#include "da_shrub_common.h"

// The chunk buffer (da_shrub_chunks: the chunk records read here, the draws' lists past SHRUB_CUT_WORDS written: a word
// an element, shrub_vertex's).
RWByteAddressBuffer	da_tree_cut_chunks	: register(u0);
RWByteAddressBuffer	da_tree_cut_counts	: register(u1);	// the words each draw took
ByteAddressBuffer	da_tree_cut_routes	: register(t7);	// a candidate chunk's route (16 bytes): x its class (2 bits) and
														// four draws (5 bits each), yzw its class's parameters

uniform float4	da_tree_cut0;					// x the first candidate chunk, y the candidates, z groups a row
uniform float4	da_tree_cut_planes[6];			// the view's frustum (world): out where dot(n, p) + w > r
uniform float4	da_tree_cut_reg[SHRUB_DRAWS];	// a draw's list: x its first word, y its room

// The classes (da_veg_internal.h CutClass): a tree's leaves - near, quads, far by their own distance (y, z: the hulls'
// and the quads' ends, metres; w: the clusters' share of the hashes, the tree's) -, its grafted wood and its stock tubes -
// four draws by the sides their silhouette needs (y: the bark's scale) -, its stock patches (a draw near, one far).
#define CUT_LEAF	0u
#define CUT_WOOD	1u
#define CUT_STOCK	2u
#define CUT_PATCH	3u
// The crossfade band of a tier switch (da_veg_internal.h kBand).
#define CUT_BAND	0.15f

groupshared uint	gs_n[4];
groupshared uint	gs_at[4];

// A window's shrink (shrub_vertex's).
float	cut_fade	(float h, float lo, float hi)
{
	return saturate((h - lo) * 10.0f + (lo <= 0.0f ? 1.0f : 0.0f)) * saturate((hi - h) * 10.0f + (hi >= 1.0f ? 1.0f : 0.0f));
}

float	cut_step	(float a, float b, float x)
{
	const float t = saturate((x - a) / max(b - a, 1e-4f));
	return t * t * (3.0f - 2.0f * t);
}

// The route's draw of slot k.
uint	cut_draw	(uint route, uint k)	{ return (route >> (2u + 5u * k)) & 0x1Fu; }

[numthreads(SHRUB_CHUNK, 1, 1)]
void	main	(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint g = gid.y * uint(da_tree_cut0.z) + gid.x;
	const bool group_on = g < uint(da_tree_cut0.y);
	if (tid.x < 4u)
		gs_n[tid.x] = 0u;
	GroupMemoryBarrierWithGroupSync();
	const uint chunk = uint(da_tree_cut0.x) + g, local = tid.x;
	const uint4 C = group_on ? da_tree_cut_chunks.Load4(chunk * 16u) : uint4(0u, 0u, 0u, 0u);
	const uint4 R = group_on ? da_tree_cut_routes.Load4(g * 16u) : uint4(0u, 0u, 0u, 0u);
	const uint cls = R.x & 3u;
	uint pick = 4u;
	float fade = 0.0f;
	[branch] if (group_on && local < (C.z & 0xFFu))
	{
		const shrub_inst I = shrub_inst_load(C.x);
		const shrub_elem E = shrub_elem_load(C.y + local, true);
		// The element's middle and reach (template metres), its arclength on its axis (the bend), its radius (the wood's
		// rules) and its kind's hash (the crossfades').
		float3 c = E.p;
		float reach = 0.0f, s = 0.0f, r_mean = 0.0f, r_max = 0.0f, hash = 0.0f;
		[branch] if (cls == CUT_LEAF)
		{
			const float2 lw = shrub_h2(E.w.z);
			reach = 2.2f * max(lw.x, lw.y) * max(rsqrt(max(f16tof32(C.w & 0xFFFFu), 1e-3f)), 1.0f);
			s = f16tof32(E.w.w & 0xFFFFu);
			hash = (float(E.w.w >> 24) + 0.5f) / 256.0f;
		}
		else if (cls == CUT_WOOD)
		{
			const float3 dir = shrub_oct(E.w.x);
			const float2 lr = shrub_h2(E.w.y), rs = shrub_h2(E.w.z);
			c = E.p + dir * (0.5f * lr.x);
			r_mean = 0.5f * (lr.y + rs.x);
			r_max = max(lr.y, rs.x);
			reach = 0.5f * lr.x + r_max;
			s = rs.y + 0.5f * lr.x;
			hash = float(E.w.w >> 16) / 65536.0f;
		}
		else if (cls == CUT_STOCK)
		{
			const uint sa = (I.aux0 + E.w.x) * SHRUB_AUX_SLOT, sb = (I.aux0 + E.w.y) * SHRUB_AUX_SLOT;
			const float3 ca = asfloat(da_shrub_aux.Load3(sa)), cb = asfloat(da_shrub_aux.Load3(sb));
			r_max = r_mean = max(asfloat(da_shrub_aux.Load(sa + 24u)), asfloat(da_shrub_aux.Load(sb + 24u)));
			c = 0.5f * (ca + cb);
			reach = 0.5f * length(cb - ca) + 1.6f * r_max;
			hash = float(E.w.z >> 16) / 65536.0f;
		}
		else
		{
			const float3 a = asfloat(da_shrub_aux.Load3((I.aux0 + E.w.x) * SHRUB_AUX_SLOT));
			const float3 b = asfloat(da_shrub_aux.Load3((I.aux0 + E.w.y) * SHRUB_AUX_SLOT));
			const float3 e = asfloat(da_shrub_aux.Load3((I.aux0 + E.w.z) * SHRUB_AUX_SLOT));
			c = (a + b + e) * (1.0f / 3.0f);
			reach = sqrt(max(max(dot(a - c, a - c), dot(b - c, b - c)), dot(e - c, e - c))) + 0.01f;
			hash = float(E.w.w >> 16) / 65536.0f;
		}
		// Bent as its vertices are (its chain's record; the stock wood the trunk's sway alone), then in the view or not
		// with all its reach.
		const float3 w_rest = shrub_world(I, c);
		float3 w = w_rest;
		[branch] if (cls <= CUT_WOOD && (C.z & SHRUB_CHUNK_RIGID) == 0u)
		{
			float3 n = float3(0.0f, 1.0f, 0.0f), t = float3(1.0f, 0.0f, 0.0f);
			shrub_bend(w, n, t, (C.w >> 16) * SHRUB_CHUNK + local, E.axis, s, I, false);
		}
		[branch] if (I.tree)
			w = shrub_tree_sway(w, I, shrub_root_height(I, w_rest), false);
		const float r = reach * I.scale + 0.05f;
		bool in_view = true;
		[unroll] for (uint k = 0u; k < 6u; ++k)
			in_view = in_view && dot(da_tree_cut_planes[k].xyz, w) + da_tree_cut_planes[k].w <= r;
		// Its depth at rest (the vertex shaders' wire rules take it there) and a render pixel's metres there.
		const float z = max(dot(w_rest - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
		const float px = da_shrub_cam.w * z, px_sides = da_shrub_cam.w * max(z, 1.5f);
		[branch] if (in_view)
		{
			[branch] if (cls == CUT_LEAF)
			{
				// Its window by its own distance: near [fh, 1), quads [fs, fh), far [fc, fs) of the hashes; under fc the
				// clusters (the tree's).
				const float d = length(w_rest - da_shrub_cam.xyz);
				const float h_end = asfloat(R.y), q_end = asfloat(R.z), fc = asfloat(R.w);
				const float fh = cut_step(h_end * (1.0f - CUT_BAND), h_end, d);
				const float fs = min(cut_step(q_end * (1.0f - CUT_BAND), q_end, d), fh);
				pick = hash >= fh ? 0u : (hash >= fs ? 1u : (hash >= fc ? 2u : 4u));
				fade = pick == 0u ? cut_fade(hash, fh, 1.0f) : (pick == 1u ? cut_fade(hash, fs, fh) : cut_fade(hash, fc, fs));
			}
			else
			{
				fade = cut_fade(hash, float((C.z >> 8) & 0xFFu) / 255.0f, float((C.z >> 16) & 0xFFu) / 255.0f);
				// The wire rule (the vertex shaders'): a segment under a quarter of the pixel there stands nowhere.
				const bool wire = cls != CUT_PATCH && r_mean * I.scale < 0.25f * px;
				// The far wood where the bark's tile is under four pixels (a zoned species' hem its pixel shader's too);
				// else the sides the silhouette needs at half a pixel of error (32 from a 26 px radius, 16 from 6.6, 8
				// from 2.6, five under it, the far wood under one and a half) - the route's draws hold a group's cap.
				const bool far_bark = 0.06f * max(asfloat(R.y), 0.25f) < 4.0f * px;
				const float rp = r_max * I.scale / px_sides;
				if (cls == CUT_PATCH)
					pick = far_bark ? 1u : 0u;
				else if (cls == CUT_STOCK)
					pick = far_bark || rp < 1.5f ? 3u : (rp >= 26.0f ? 0u : (rp >= 6.57f ? 1u : 2u));
				else
					pick = far_bark || rp < 1.5f ? 3u : (rp >= 6.57f ? 0u : (rp >= 2.62f ? 1u : 2u));
				if (wire)
					pick = 4u;
			}
			if (round(saturate(fade) * 255.0f) < 0.5f)
				pick = 4u;
		}
	}
	// The group's places in its draws' lists: one count a draw a group.
	uint mine = 0u;
	if (pick < 4u)
		InterlockedAdd(gs_n[pick], 1u, mine);
	GroupMemoryBarrierWithGroupSync();
	if (tid.x < 4u && gs_n[tid.x] > 0u)
	{
		uint at;
		da_tree_cut_counts.InterlockedAdd(cut_draw(R.x, tid.x) * 4u, gs_n[tid.x], at);
		gs_at[tid.x] = at;
	}
	GroupMemoryBarrierWithGroupSync();
	if (pick < 4u)
	{
		const uint d = cut_draw(R.x, pick), at = gs_at[pick] + mine;
		if (at < uint(da_tree_cut_reg[d].y))
		{
			uint word = chunk | (local << 18) | (uint(round(saturate(fade) * 255.0f)) << 24);
			// (All ones marks a list's tail.)
			if (word == 0xFFFFFFFFu)
				word = 0xFEFFFFFFu;
			da_tree_cut_chunks.Store(SHRUB_CUT_WORDS + (uint(da_tree_cut_reg[d].x) + at) * 4u, word);
		}
	}
}
