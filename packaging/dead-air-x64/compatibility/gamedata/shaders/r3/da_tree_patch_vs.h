// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Trees, the vertex shader of the stock patches (docs/dead-air/TREES.md): where the skeleton's tubes miss the stock
// trunk by more than 2 cm or 5 % of its radius (forks, caps, cut ends, gnarled bases) its original triangles stay (their
// borders laid onto the tubes), every vertex skinned to its axis and arclength. A triangle a record (its three vertices
// in the aux buffer), the species' bark by the two nearest tubes' mappings (the turn round each axis and its arclength,
// as the tubes lay them, the second by its weight: a fork's patch takes both limbs' bark where they meet, the patch and
// the tube meet without a seam), the pixel shader's frame from the mapping's derivatives; a cut end's cap its end grain
// (the cross-section's coordinates, the rim gauge and the place along the cap's axis in its vertices' own copies, the
// pixel shader's rings, rays and checks, or a break's torn fibres). Fixed wood: the trunk's sway alone. SHRUB_SHADOW: the shadow maps, depth only.

#define SHRUB_WOOD_NO_MAIN
#include "da_shrub_wood_vs.h"

// A patch vertex (one aux slot, da_trees.cpp pack): its position and normal (oct), its first axis (24 bits), its
// arclength on it, the turn round its second axis (0..1) and its first axis's run (the template's ring before it, 16
// bits, 0xFFFF none, and its share of the way to the next, 16 bits); a cap's
// copy: the cross-section's coordinates instead (template metres from the cap's centroid), in place of the arclength a
// pair of halves - the rim gauge (1 on the open border, 0 inside, -2 with no inner vertex; 4 added on a break) and the
// place along the cap's own axis (template metres) - and the cap's mean rim radius in the axis word's top byte (4 mm
// steps).
struct tree_patch_vtx
{
	float3	p;
	float3	n;
	uint	axis;
	float	s;
	float2	uv;		// a cap's copy's cross-section; a bark vertex's turn round its second axis in x
	uint	run;	// a bark vertex's first axis's run
	uint	rim_q;
};

// A patch vertex's sidecar word (da_trees.cpp pack_tree, the aux header's first half: the patch vertices' first slot
// and count, the sidecar's slot): its turn round its first axis, the second mapping's weight there (0 when the slot is
// no patch vertex: a cap's copy).
uint	tree_patch_side	(uint4 H, uint aux0, uint slot)
{
	const uint v = slot - aux0 - H.x;
	return v < H.y ? da_shrub_aux.Load((aux0 + H.z) * SHRUB_AUX_SLOT + v * 4u) : 0u;
}

// Its second sidecar's two words (after the sidecar's slots): the second axis (24 bits, all ones: none) and the world
// projection's weight (8 bits), the second axis's run.
uint2	tree_patch_side2	(uint4 H, uint aux0, uint slot)
{
	const uint v = slot - aux0 - H.x;
	return v < H.y ? da_shrub_aux.Load2((aux0 + H.z + (H.y + 7u) / 8u) * SHRUB_AUX_SLOT + v * 8u) : uint2(0x00FFFFFFu, 0xFFFFu);
}

// A ring's arclength and mean radius (its record's words 3 and 6, da_trees.cpp pack; H.w the rings' first slot).
float2	tree_patch_ring_sr	(uint4 H, uint aux0, uint k)
{
	const uint at = (aux0 + H.w + 2u * k) * SHRUB_AUX_SLOT;
	return float2(asfloat(da_shrub_aux.Load(at + 12u)), asfloat(da_shrub_aux.Load(at + 24u)));
}

// A triangle's tiles round its axis as the tube lays them (da_tree_stock_vs.h: each ring's girth over the axis's tile,
// rounded, the count blended from one ring's to the next's along the stretch between them): over the stretches its
// corners' runs span, the count round its first ring and the steps after it - sparse (counts: x the count; y, z, w two
// steps each - a step's code one more than its stretch from that ring times 16 and its change plus 8, the second's
// times 2048) while six steps of -8 .. 7 hold them, else dense where every step is one (x minus the count; y, z, w
// twelve stretches each, two bits a stretch from the lowest: 0 none, 1 up, 2 down; 36 stretches), else sparse with
// the rest's change on the sixth (a root's flare over many stretches: a seam at the tube there) - and this corner's
// ring coordinate from that ring (x), its arclength and its ring's radius. A triangle off any stretch (a corner on an
// axis without rings) takes the axis's base count.
// (A count per ring of three rings from the triangle's lowest took the base count over wider triangles - a fifth of an
// oak's patches, its triangles running over up to 28 stretches - and their arclength nought: the second mapping in
// streaks along its axis.)
struct tree_patch_runs
{
	float4	counts;
	float	x;
	float	s;
	float	r;
};

float	tree_patch_step	(uint j, float d)
{
	return float(1u + j * 16u) + clamp(d, -8.0f, 7.0f) + 8.0f;
}

tree_patch_runs	tree_patch_runs_of	(shrub_inst I, uint4 H, uint own, uint r0, uint r1, uint r2, float axis_r, float s_off, float tile,
	float wraps0, bool counts)
{
	tree_patch_runs T;
	T.counts = float4(wraps0, 0.0f, 0.0f, 0.0f);
	T.x = 0.0f;
	T.s = s_off;
	T.r = axis_r;
	const uint k = own & 0xFFFFu;
	const uint kmin = min(r0 & 0xFFFFu, min(r1 & 0xFFFFu, r2 & 0xFFFFu)), kmax = max(r0 & 0xFFFFu, max(r1 & 0xFFFFu, r2 & 0xFFFFu));
	[branch] if (k == 0xFFFFu)
		return T;
	const float u = float(own >> 16) * (1.0f / 65535.0f);
	const float2 a = tree_patch_ring_sr(H, I.aux0, k), b = tree_patch_ring_sr(H, I.aux0, k + 1u);
	T.s = lerp(a.x, b.x, u);
	T.r = lerp(a.y, b.y, u);
	[branch] if (kmax == 0xFFFFu)
		return T;
	T.x = float(k - kmin) + u;
	// (The counts go to the pixel shader nointerpolation: the triangle's first vertex's alone stand - the other two
	// skip their loop.)
	[branch] if (!counts)
		return T;
	// (The steps over the span - 62 stretches at the most, the sparse code's six bits -, both codes at once.)
	const uint span = min(kmax - kmin, 62u);
	const float c0 = shrub_ring_count(tree_patch_ring_sr(H, I.aux0, kmin).y, I.scale, tile);
	float c = c0, d6 = 0.0f, big = 0.0f;
	float3 dense = 0.0f;
	T.counts.x = c0;
	uint n = 0u, j6 = 0u;
	[loop] for (uint j = 0u; j <= span; ++j)
	{
		const float c1 = shrub_ring_count(tree_patch_ring_sr(H, I.aux0, kmin + j + 1u).y, I.scale, tile);
		[branch] if (c1 != c)
		{
			const float d = c1 - c;
			[branch] if (n < 5u)
			{
				const float e = tree_patch_step(j, d) * ((n & 1u) != 0u ? 2048.0f : 1.0f);
				T.counts.y += n < 2u ? e : 0.0f;
				T.counts.z += n >= 2u && n < 4u ? e : 0.0f;
				T.counts.w += n >= 4u ? e : 0.0f;
			}
			else
			{
				j6 = n == 5u ? j : j6;
				d6 += d;
			}
			big = max(big, abs(d));
			const float e2 = float((d > 0.0f ? 1u : 2u) << (2u * (j % 12u)));
			dense += j < 12u ? float3(e2, 0.0f, 0.0f) : (j < 24u ? float3(0.0f, e2, 0.0f) : float3(0.0f, 0.0f, e2));
			++n;
		}
		c = c1;
	}
	T.counts.w += n > 5u ? tree_patch_step(j6, d6) * 2048.0f : 0.0f;
	// (The dense code where the sparse cannot hold the steps and it can: every step one count, 36 stretches.)
	[flatten] if (n > 6u && big < 1.5f && span < 36u)
		T.counts = float4(-c0, dense);
	return T;
}

// A unit vector at eight bits a coordinate (da_veg.cpp oct8).
float3	tree_oct8	(uint v)
{
	const float2 e = float2(float(v & 0xFFu), float((v >> 8) & 0xFFu)) * (2.0f / 255.0f) - 1.0f;
	float3 n = float3(e.x, 1.0f - abs(e.x) - abs(e.y), e.y);
	if (n.y < 0.0f)
		n.xz = (1.0f - abs(n.zx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.z >= 0.0f ? 1.0f : -1.0f);
	return normalize(n);
}

tree_patch_vtx	tree_patch_load	(uint slot)
{
	const uint at = slot * SHRUB_AUX_SLOT;
	const uint4 w0 = da_shrub_aux.Load4(at), w1 = da_shrub_aux.Load4(at + 16u);
	tree_patch_vtx P;
	P.p = asfloat(w0.xyz);
	P.n = shrub_oct(w0.w);
	P.axis = w1.x & SHRUB_NO_AXIS;
	P.rim_q = w1.x >> 24;
	P.s = asfloat(w1.y);
	P.uv = asfloat(w1.zw);
	P.run = w1.w;
	return P;
}

v2p_shrub_wood	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_shrub_wood O = (v2p_shrub_wood)0;
	const shrub_vtx V = shrub_vertex(vid, iid, SHRUB_K_PATCH);
	if (!V.valid)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const shrub_elem E = V.E;
	const shrub_inst I = V.I;
	const uint corner = min(V.corner, 2u);
	const uint s0 = I.aux0 + E.w.x, s1 = I.aux0 + E.w.y, s2 = I.aux0 + E.w.z;
	const uint slot = corner == 0u ? s0 : (corner == 1u ? s1 : s2);
	tree_patch_vtx P = tree_patch_load(slot);
	// The tessellation eased in near the trunk (da_trees.cpp near_patches; the chunk's aux the share eased out, 1 the stock's
	// own facets - where the far set takes over, unseen): a tessellated vertex's lift over its stock facet and the facet's
	// normal there (its third sidecar, past the second; a cap's copy none). An angular trunk turned smooth at once.
	[branch] if (V.aux > 0.0f)
	{
		const uint4 H0 = da_shrub_aux.Load4(I.aux0 * SHRUB_AUX_SLOT);
		const uint v = slot - I.aux0 - H0.x;
		[branch] if (v < H0.y)
		{
			const uint2 L = da_shrub_aux.Load2((I.aux0 + H0.z + (H0.y + 7u) / 8u + (H0.y + 3u) / 4u) * SHRUB_AUX_SLOT + v * 8u);
			P.p -= float3(f16tof32(L.x), f16tof32(L.x >> 16), f16tof32(L.y)) * saturate(V.aux);
			P.n = normalize(lerp(P.n, tree_oct8(L.y >> 16), saturate(V.aux)));
		}
	}
	// (Out along its stock normal: the tube under a patch runs within the skeleton's tolerance of the stock surface, and
	// the two must not fight for the depth where they meet - a millimetre near, the depth buffer's step farther off, four
	// at the most: at four near, the screen-space AO read the step at the patch's border and shaded the tube's end under
	// a fork apart, a straight-edged tent.)
#ifdef SHRUB_SHADOW
	const float lift = 0.004f;
#else
	const float lift = clamp(0.0002f * dot(shrub_world(I, P.p) - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.001f, 0.004f);
#endif
	const float3 q0 = shrub_world(I, P.p + P.n * lift);
#if !defined(SHRUB_SHADOW) && !defined(SHRUB_WOOD_FAR)
	// The triangle decides as one: a corner culled alone went to the screen's centre and drew a wedge across the view.
	// (The far wood draws its few triangles uncut: the test's three more loads a vertex cost more than the clipper; the
	// GPU cut's are in the view.)
	[branch] if (!shrub_cut_draw())
	{
		const float3 a = shrub_world(I, asfloat(da_shrub_aux.Load3(s0 * SHRUB_AUX_SLOT)));
		const float3 b = shrub_world(I, asfloat(da_shrub_aux.Load3(s1 * SHRUB_AUX_SLOT)));
		const float3 c = shrub_world(I, asfloat(da_shrub_aux.Load3(s2 * SHRUB_AUX_SLOT)));
		const float3 m = (a + b + c) * (1.0f / 3.0f);
		if (shrub_out_of_view(I, m, sqrt(max(max(dot(a - m, a - m), dot(b - m, b - m)), dot(c - m, c - m))) + 0.5f))
		{
			O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
			return O;
		}
	}
#endif
	const float3 n = normalize(shrub_world_nrm(I, P.n));
	const float h_rest = shrub_root_height(I, q0);
	const float3 q = shrub_tree_sway(q0, I, h_rest, false);
#ifdef SHRUB_SHADOW
	O.hpos = mul(m_WVP, float4(q, 1.0f));
	return O;
#else
	const float3 qp = da_shrub_time.z > 0.5f ? shrub_tree_sway(q0, I, h_rest, true) : q;
	O.hpos = mul(m_WVP, float4(q, 1.0f));
	O.hpos_cur = mul(m_WVP_nojit, float4(q, 1.0f));
	O.hpos_prev = mul(m_WVP_prev, float4(qp, 1.0f));
	const shrub_axis X = shrub_axis_load(I.axis0 + P.axis);
	const float3 t = normalize(shrub_world_dir(I, X.dir));
	const float view_z = max(dot(q0 - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
#ifdef SHRUB_WOOD_FAR
	// The far wood: its first axis's bark as its mean (shrub_wood_outputs); a cut end's cap its weathered wood's grey.
	{
		const float floor_r = 0.5f * da_shrub_cam.w * view_z;
		shrub_wood_outputs(O, I, q, n, t, q0, 0.0f, P.s, X.r, X.r, max(X.r * I.scale, floor_r), floor_r,
			(E.w.w & 0x100u) != 0u ? 0x80u : 0u, float(E.w.w & 0xFFu) / 255.0f, 0u, X.r, X.len, X.base.y, X.phase, 0.0f, q0, shrub_join_none());
		[branch] if ((E.w.w & 0x200u) != 0u && P.rim_q != 0u)
			O.alb_w.rgb = dot(O.alb_w.rgb, float3(0.2126f, 0.7152f, 0.0722f)) * float3(0.95f, 0.97f, 1.0f);
		return O;
	}
#else
	// (Bit 8: old wood, no young growth's colour.)
	// The sidecar (a bark vertex's): the turn round its first axis and the second mapping's weight; the first axis's runs
	// over the triangle (the tube's counts of tiles round the rings, the ring's radius here: the age's radius).
	const uint4 H = da_shrub_aux.Load4(I.aux0 * SHRUB_AUX_SLOT);
	const bool cap = (E.w.w & 0x200u) != 0u && P.rim_q != 0u;
	const uint side = cap ? 0u : tree_patch_side(H, I.aux0, slot);
	float wraps0;
	const float tile = shrub_axis_tile(I, X.r, wraps0);
	tree_patch_runs R1 = (tree_patch_runs)0;
	R1.r = X.r;
	R1.counts.x = wraps0;
	[branch] if (!cap && (da_shrub_chunk0.w < 0.5f || da_shrub_chunk0.w > 1.5f))
		R1 = tree_patch_runs_of(I, H, P.run, da_shrub_aux.Load(s0 * SHRUB_AUX_SLOT + 28u), da_shrub_aux.Load(s1 * SHRUB_AUX_SLOT + 28u),
			da_shrub_aux.Load(s2 * SHRUB_AUX_SLOT + 28u), X.r, P.s, tile, wraps0, corner == 0u);
	const float r_ring = R1.r;
	// (Never under the pixel floor: a patch is not widened to it, and the wire rule's dither cut holes in a thin fork's.)
	const float floor_r = 0.5f * da_shrub_cam.w * view_z;
	shrub_wood_outputs(O, I, q, n, t, q0, 0.0f, P.s, r_ring, r_ring, max(r_ring * I.scale, floor_r), floor_r,
		(E.w.w & 0x100u) != 0u ? 0x80u : 0u, float(E.w.w & 0xFFu) / 255.0f, 0u, X.r, X.len, X.base.y, X.phase, 0.0f, q0, shrub_join_none());
	O.bark2.zw = float2(1.0f, 1.0f);
	[branch] if (cap)
	{
		// A cut end's cap (bit 9): its end grain by the cross-section's coordinates, the rim gauge and the mean rim radius
		// (world metres), the species, a seed of its own (its axis and mean rim on the tree's seed) and the callus a
		// living tree rolls over the cut's edge; no bark, lichen or algae on it (zone.w 2, da_shrub_wood.ps).
		const float2 ga = shrub_h2(asuint(P.s));
		const bool torn = ga.x > 1.5f;
		O.n_u.w = P.uv.x * I.scale;
		O.t_v.w = P.uv.y * I.scale;
		O.bark3 = float4(float(I.species), float(shrub_hash(I.seed ^ (P.axis * 2654435761u) ^ (P.rim_q * 40503u)) >> 8) / 16777216.0f,
			float(P.rim_q) * 0.004f * I.scale, I.dead ? 0.0f : 1.0f);
		// (zone.y the gauge; zone.z 1 a saw's flat cut, 0 a break standing off its plane, no rings on it; ring the place
		// along the cap's own axis, world metres: the break's fibres run along it.)
		O.zone = float4(-1.0f, torn ? ga.x - 4.0f : ga.x, torn ? 0.0f : 1.0f, 2.0f);
		O.ring = ga.y * I.scale;
		O.moss = float4(0.0f, 1.0f, 1.0f, 0.0f);
		// (The rim's bark as the species wears it: the cap's old-wood bit is no dead stub's.)
		O.bark = shrub_prof(I.species, 13);
		return O;
	}
	// The bark by two tubes (da_shrub_gen.cpp tree_patch_pairs): the first axis's turn and its tiles along as its tube
	// lays them, the tube's own counts of tiles round the rings the triangle's runs span (jc1, nointerpolation: the first
	// ring's and the steps after it) and the place's ring coordinate from that ring (ring: the pixel shader steps the
	// counts where the tube does; zone.w 1: the frame from the mapping's derivatives); the second axis's the same with
	// its weight (jc0, jw), on every corner of a triangle whose pair has one. Each turn unwrapped to the triangle's first vertex's: two
	// triangles of one quad may stand a whole turn apart. Where both smear, the world's projection's weight (moss.y) and
	// the species' tile (lit.w, world metres: one over the whole patch, as shrub_wood_outputs gives every wood).
	const float stretch = max(shrub_prof(I.species, 18).y, 0.25f);
	const uint2 S2 = tree_patch_side2(H, I.aux0, slot);
	{
		const float turn = float(side & 0xFFFu) / 4096.0f;
		const float turn0 = float(tree_patch_side(H, I.aux0, s0) & 0xFFFu) / 4096.0f;
		O.n_u.w = turn0 + frac(turn - turn0 + 0.5f) - 0.5f + X.phase;
		O.t_v.w = P.s * I.scale / (tile * stretch) + X.phase * 7.0f;
		O.ring = R1.x;
		O.jc1 = R1.counts;
		O.zone.w = 1.0f;
		O.moss.y = float(S2.x >> 24) / 255.0f;
	}
	[branch] if ((S2.x & 0xFFFFFFu) != 0xFFFFFFu && (da_shrub_chunk0.w < 0.5f || da_shrub_chunk0.w > 1.5f))
	{
		const shrub_axis X2 = shrub_axis_load(I.axis0 + (S2.x & 0xFFFFFFu));
		const float turn0 = asfloat(da_shrub_aux.Load(s0 * SHRUB_AUX_SLOT + 24u));
		float wraps2;
		const float tile2 = shrub_axis_tile(I, X2.r, wraps2);
		const tree_patch_runs R2 = tree_patch_runs_of(I, H, S2.y, tree_patch_side2(H, I.aux0, s0).y, tree_patch_side2(H, I.aux0, s1).y,
			tree_patch_side2(H, I.aux0, s2).y, X2.r, 0.0f, tile2, wraps2, corner == 0u);
		O.jn = float3(turn0 + frac(P.uv.x - turn0 + 0.5f) - 0.5f + X2.phase, R2.s * I.scale / (tile2 * stretch) + X2.phase * 7.0f,
			float(side >> 24) / 255.0f);
		O.jw = R2.x;
		O.jc0 = R2.counts;
	}
	return O;
#endif
#endif
}
