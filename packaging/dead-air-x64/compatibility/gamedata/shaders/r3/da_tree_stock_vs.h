// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Trees, the vertex shader of the stock wood (docs/dead-air/TREES.md): the stock trunk's and limbs' tubes on the
// rings the skeleton took from the stock mesh (gamedata/meshes/da/trees/*.dtsk), where they stand in the model -
// each ring's cross-section its minimax Fourier profile between the stock faces and vertices, r(th) = r0 (1 + sum
// ak cos k th + bk sin k th) about its reference normal, the spine a centripetal Catmull-Rom curve through the stock
// ring centroids (the generator's subdivisions, da_shrub_gen.cpp). Thirty-two, sixteen or eight sides by the draw,
// fewer by the on-screen error of the ring's silhouette (n = ceil(pi / acos(1 - eps p(d) / r)), eps half a pixel, a
// shadow map's texel): the class's other vertices collapse onto them. The stock wood is fixed: no bend of its own,
// the trunk's sway alone (held to a few centimetres on the climbable wood); its bark is the species' (the shrubs' bark
// pixel shader, its zones), a limb's toward its parent the parent's (its junction record). SHRUB_SHADOW: the sun
// cascades and the local lights, depth only.

#define SHRUB_WOOD_NO_MAIN
#include "da_shrub_wood_vs.h"

// A ring (two aux slots, da_trees.cpp pack): centroid and arclength, tangent and reference normal (oct), the mean radius,
// the climbable share and the stock ring's side count, the profile's six coefficients (a1 b1 a2 b2 a3 b3) and the step
// back of each sixteenth of its turn the patches cover, four bits each (da_shrub_gen.cpp tree_patch_cover).
struct tree_ring
{
	float3	c;
	float	s;
	float3	t;
	float3	n;
	float	r0;
	float3	a;	// a1 a2 a3
	float3	b;	// b1 b2 b3
	uint2	back;	// sixteenths 0-7, 8-15: a level each (0: not covered)
};

tree_ring	tree_ring_load	(uint slot)
{
	const uint at = slot * SHRUB_AUX_SLOT;
	const uint4 w0 = da_shrub_aux.Load4(at), w1 = da_shrub_aux.Load4(at + 16u);
	const uint4 w2 = da_shrub_aux.Load4(at + 32u), w3 = da_shrub_aux.Load4(at + 48u);
	tree_ring R;
	R.c = asfloat(w0.xyz);
	R.s = asfloat(w0.w);
	R.t = shrub_oct(w1.x);
	R.n = shrub_oct(w1.y);
	R.r0 = asfloat(w1.z);
	R.a = float3(asfloat(w2.x), asfloat(w2.z), asfloat(w3.x));
	R.b = float3(asfloat(w2.y), asfloat(w2.w), asfloat(w3.y));
	R.back = w3.zw;
	return R;
}

float	tree_ring_r0	(uint slot)	{ return asfloat(da_shrub_aux.Load(slot * SHRUB_AUX_SLOT + 24u)); }
float3	tree_ring_c		(uint slot)	{ return asfloat(da_shrub_aux.Load3(slot * SHRUB_AUX_SLOT)); }

// Ring R's vertex k of a tube of `sides` (template space; px: a pixel's or a texel's metres at its depth, scale: the
// tree's): the sides the silhouette needs at half a pixel of error, the class's at the most - by the ring's own radius
// and depth: both segments that share a ring build it alike (a count by the segment flipped between neighbours and
// opened sparkling gaps along the trunks) -, the profile at the vertex's turn th and, under a stock patch, its covered
// sixteenths stepped back (none on the far level); its radius r, the profile's slope dr and the frame's radial.
struct tree_ring_vtx
{
	float	idx;
	float	need;
	float	th;
	float	r;
	float	dr;
	float3	radial;
	float3	nrm;	// unnormalised, template space
};

tree_ring_vtx	tree_ring_vertex	(tree_ring R, uint k, uint sides, float px, float scale, bool far)
{
	tree_ring_vtx T;
	const float rel = saturate(1.0f - 0.5f * px / max(R.r0 * scale, 1e-4f));
	T.need = clamp(ceil(3.14159265f / max(acos(rel), 1e-3f)), 5.0f, float(sides));
	T.idx = round(float(k) * T.need / float(sides));
	T.th = 6.2831853f * T.idx / T.need;
	float sn1, cs1, sn2, cs2, sn3, cs3;
	sincos(T.th, sn1, cs1);
	sincos(2.0f * T.th, sn2, cs2);
	sincos(3.0f * T.th, sn3, cs3);
	float r = R.r0 * (1.0f + R.a.x * cs1 + R.b.x * sn1 + R.a.y * cs2 + R.b.y * sn2 + R.a.z * cs3 + R.b.z * sn3);
	// Under a stock patch the tube steps back by its sixteenths' level L - the skeleton's tolerance and ((L - 1) / 14)^2
	// of the rest of its radius there, the least that takes it a centimetre under the patch there -, 15 % of the radius
	// left at the least: the patch shows, the two never fight for the depth. A vertex by the least level of the
	// sixteenths its two edges run over: one on the border of a covered sixteenth stepped back by the side its rounding
	// fell on (FXC and DXC apart) and opened a dark crack under the patch's border laid onto the tube.
	// (The far level stands whole: no patch over it there - kChunkFar.)
	const float sx = T.th * (16.0f / 6.2831853f), sdx = 16.0f / T.need;
	uint level = far ? 0u : 15u;
	[loop] for (int sj = int(floor(sx - sdx + 0.501f)); !far && sj <= int(floor(sx + sdx + 0.499f)); ++sj)
	{
		const uint sixteenth = uint(sj) & 15u;
		level = min(level, ((sixteenth < 8u ? R.back.x : R.back.y) >> (4u * (sixteenth & 7u))) & 15u);
	}
	[branch] if (level != 0u)
	{
		const float tol = max(0.02f, 0.05f * R.r0), q = float(level - 1u) / 14.0f;
		r -= min(tol + q * q * max(r - tol, 0.0f), 0.85f * r);
	}
	T.r = r;
	T.dr = R.r0 * (-R.a.x * sn1 + R.b.x * cs1 - 2.0f * R.a.y * sn2 + 2.0f * R.b.y * cs2 - 3.0f * R.a.z * sn3 + 3.0f * R.b.z * cs3);
	// The profile's frame: its reference normal (the angle's zero) and b = cross(t, n) (the angle grows toward it).
	const float3 bn = cross(R.t, R.n);
	T.radial = R.n * cs1 + bn * sn1;
	T.nrm = R.n * (r * cs1 + T.dr * sn1) + bn * (r * sn1 - T.dr * cs1);
	return T;
}

v2p_shrub_wood	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_shrub_wood O = (v2p_shrub_wood)0;
	const shrub_vtx V = shrub_vertex(vid, iid, SHRUB_K_STOCK);
	if (!V.valid)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const shrub_elem E = V.E;
	const shrub_inst I = V.I;
	const uint slot_a = I.aux0 + E.w.x, slot_b = I.aux0 + E.w.y;
	const uint sides = uint(da_shrub_draw.x) / 2u - 1u;
	const uint ring = V.corner / (sides + 1u), k = V.corner - ring * (sides + 1u);
	const tree_ring R = tree_ring_load(ring != 0u ? slot_b : slot_a);
	// The segment decides as one - in view or not, under the wire rule or not, its side count - on both rings' centres
	// and the thicker radius: a ring deciding alone pulled its half of the tube to the screen's centre (a culled
	// vertex's place) and its sides off its neighbour's.
	const float3 Ca = shrub_world(I, tree_ring_c(slot_a)), Cb = shrub_world(I, tree_ring_c(slot_b));
	const float r_seg = max(tree_ring_r0(slot_a), tree_ring_r0(slot_b)) * I.scale;
#ifndef SHRUB_SHADOW
	[branch] if (!shrub_cut_draw())
	{
		if (shrub_out_of_view(I, 0.5f * (Ca + Cb), 0.5f * length(Cb - Ca) + r_seg * 1.6f + 0.5f))
		{
			O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
			return O;
		}
	}
#endif
	const float3 C = shrub_world(I, R.c);
#ifdef SHRUB_SHADOW
	const float px = da_shrub_time.w, px_seg = px;
#else
	const float view_z = max(dot(C - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
	const float px = da_shrub_cam.w * view_z;
	const float px_seg = da_shrub_cam.w * max(dot(0.5f * (Ca + Cb) - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
#endif
	const float r_w = R.r0 * I.scale;
	const float floor_r = 0.5f * px;
	// The wire rule of the thin stock twigs (a limb's tip at a distance): a segment under half the floor stands nowhere,
	// over it grows to the floor, its pixels all kept - by its radius alone: the CPU sorts and cuts the tubes by it
	// (da_veg_internal.h WoodCut).
	if (r_seg < 0.25f * px_seg)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const tree_ring_vtx RV = tree_ring_vertex(R, k, sides, px, I.scale, V.far);
	const float idx = RV.idx, need = RV.need, r = RV.r;
	const float3 radial = RV.radial;
	float3 nrm_t = RV.nrm;
	// (Never under the floor: the tube grows to it, keeping its shape.)
	const float grow = max(floor_r / max(r_w, 1e-5f), 1.0f) * V.grow;
#ifdef SHRUB_SHADOW
	const float shrink = 0.97f;
#else
	const float shrink = 1.0f;
#endif
	float3 p_t = R.c + radial * (r * grow * shrink);
	// A level's dropped rings onto the next level's tube over the last of its range (da_trees.cpp stock_level; the chunk's
	// aux the share of the way): this vertex moves to where the strip of the coarser tube between the two rings that
	// level keeps round this one passes at its arclength (the rings' morph table, past the junction records: their
	// distances back and on), so the switch itself moves nothing - a bent pine's trunk turned from a curve into kinks at
	// once at eight metres.
	[branch] if (V.aux > 0.0f)
	{
		const uint lvl = uint(E.a * 255.0f + 0.5f) + 1u;
		const uint4 H0 = da_shrub_aux.Load4(I.aux0 * SHRUB_AUX_SLOT), H1 = da_shrub_aux.Load4(I.aux0 * SHRUB_AUX_SLOT + 16u);
		const uint kr = ((ring != 0u ? E.w.y : E.w.x) - H0.w) / 2u;
		const uint code = lvl < 3u ? (da_shrub_aux.Load((I.aux0 + H1.z + 2u * H1.w) * SHRUB_AUX_SLOT + kr * 4u) >> (16u * (lvl - 1u))) & 0xFFFFu : 0u;
		[branch] if ((code & 0xFFu) != 0u && (code >> 8) != 0u)
		{
			const tree_ring A = tree_ring_load(I.aux0 + H0.w + 2u * (kr - (code & 0xFFu)));
			const tree_ring B = tree_ring_load(I.aux0 + H0.w + 2u * (kr + (code >> 8)));
			const float u = saturate((R.s - A.s) / max(B.s - A.s, 1e-5f)), m = saturate(V.aux);
			const tree_ring_vtx VA = tree_ring_vertex(A, k, sides, px, I.scale, V.far), VB = tree_ring_vertex(B, k, sides, px, I.scale, V.far);
			const float3 pa = A.c + VA.radial * (VA.r * grow * shrink), pb = B.c + VB.radial * (VB.r * grow * shrink);
			p_t = lerp(p_t, lerp(pa, pb, u), m);
			nrm_t = lerp(nrm_t, lerp(normalize(VA.nrm), normalize(VB.nrm), u) * length(nrm_t), m);
		}
	}
	const float3 q0 = shrub_world(I, p_t);
	const float3 n = normalize(shrub_world_nrm(I, nrm_t));
	const float3 t = normalize(shrub_world_dir(I, R.t));
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
	const shrub_axis X = shrub_axis_load(I.axis0 + E.axis);
	// A limb near its parent: its junction record's slot in w's upper bits (da_trees.cpp pack_tree).
	shrub_join J = shrub_join_none();
	float3 jn = 0.0f;
	float jw = 1.0f;
	float4 jc0 = 0.0f, jc1 = 0.0f;
#ifndef SHRUB_WOOD_FAR
	[branch] if ((E.w.w >> 8) != 0u)
	{
		const shrub_join_rec JR = shrub_join_load(I.aux0 + (E.w.w >> 8));
		shrub_join_vertex(I, JR, shrub_axis_load(I.axis0 + X.parent), X.r, R.c + radial * r, jn, jw, jc0, jc1, J.w);
		J.light = JR.light;
		J.level_byte = JR.level_byte;
		J.r = JR.r * I.scale;
	}
#endif
	shrub_wood_outputs(O, I, q, n, t, C, idx / need + X.phase, R.s, tree_ring_r0(slot_a), tree_ring_r0(slot_b), r_w * grow, floor_r,
		(E.w.z >> 8) & 0xBFu, float(E.w.z & 0xFFu) / 255.0f, ring, X.r, X.len, X.base.y, X.phase, 0.0f, q0, J);
#ifndef SHRUB_WOOD_FAR
	O.jn = jn;
	O.jw = jw;
	O.jc0 = jc0;
	O.jc1 = jc1;
#endif
	return O;
#endif
}
