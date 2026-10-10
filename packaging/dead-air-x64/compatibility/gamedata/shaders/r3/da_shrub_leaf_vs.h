// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Procedural shrubs, the vertex shader of the leaves (docs/dead-air/SHRUBS.md). One leaf a record, its geometry
// from SV_VertexID, its look from the species' leaf texture (tools/shrubs/textures): near, the eight-vertex hull the
// texture's outline fits in (four rows of a pair, their places and widths fitted to the texture), folded along
// the midrib and drooping to the tip, the pixel shader cutting the leaf out of it by the texture's coverage;
// farther, and for a compound leaf, a lobed one, a needle sprig and a spray's billboard, the texture's whole square
// (a tuft and a far spray turn round their shoot to the camera); near, a spray stands as its own leaves round its
// stretch of shoot and a needle sprig as two crossed squares about its shoot. A leaf takes the species' leaf a or b and is mirrored by its own hash. The season decides each
// leaf by its own order: unfolded once the bush's leaf-out passes its rank (and grown over the next weeks), coloured
// by its own place in the autumn wave, gone once the fall passes its fall order (both shrink over a sliver of the
// order, never pop). It flutters about its petiole, then bends with its chain of axes. The pixel floor's survivors
// grow to keep the area, and a leaf seen nearly edge-on and small turns a little to the camera so it never thins to
// nothing. Its shading normal is half its own and half the crown's (the record's side word carries the crown's
// normal beside the leaf's roll): the bush is lit as one volume and a fluttering leaf does not flash. It flutters in
// the eddies the wind carries through the crown (shrub_flutter: neighbours in step, the strength swelling as an
// eddy passes). SHRUB_SHADOW: the sun cascades and the local lights, the texture's coverage cut in the shadow pixel
// shader, the leaves fluttering there as in the view so their shadows move with them.

#include "common.h"
#include "da_velocity.h"
#include "da_shrub_common.h"

struct v2p_shrub_leaf
{
	float4	hpos		: SV_Position;
#ifdef SHRUB_SHADOW
	float4	tex			: TEXCOORD0;	// u, v, layer, flags (shrub_leaf_flags)
#else
	float4	hpos_cur	: TEXCOORD0;
	float4	hpos_prev	: TEXCOORD1;
	float4	pv_u		: TEXCOORD2;	// view-space position, across the blade (-1..1)
	float4	n_v			: TEXCOORD3;	// face normal (view space, the upper side), along the blade (0..1)
	float4	s_g			: TEXCOORD4;	// side (view space, toward the texture's +u), gloss
	float4	alb_h		: TEXCOORD5;	// upper albedo (G-buffer encoding), hemi x occlusion
	float4	und_t		: TEXCOORD6;	// underside albedo, translucency
	float4	tex			: TEXCOORD7;	// u, v, layer, kind (SHRUB_LEAF_*) + 8 if mirrored + 16 x bite (0..15)
	float4	twig		: TEXCOORD8;	// a card's twig albedo (G-buffer encoding), the leaf's size on the screen (px)
	float4	crown		: TEXCOORD9;	// the crown's normal (view space), w its share of the shading normal
	float	fold		: TEXCOORD10;	// the fold along the midrib (rad: the halves turned up), shaded
#endif
};

// The leaf's corner: x across (-1..1 of the texture's width), y along (0 base .. 1 tip). The hull: four rows of a
// pair (the base, two rows at the texture's places, the tip) at the widths that hold the texture's outline; a
// quad: the texture's square.
float2	shrub_leaf_corner	(uint corner, bool quad, uint s)
{
	if (quad)
		return float2((corner & 1u) != 0u ? 1.0f : -1.0f, corner >= 2u ? 1.0f : 0.0f);
	const float4 H0 = shrub_prof(s, 9), H1 = shrub_prof(s, 10);
	const uint row = corner >> 1;
	const float2 vw = row == 0u ? float2(0.0f, H0.x) : (row == 1u ? H0.yz : (row == 2u ? float2(H0.w, H1.x) : float2(1.0f, H1.y)));
	return float2((corner & 1u) != 0u ? vw.y : -vw.y, vw.x);
}

v2p_shrub_leaf	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_shrub_leaf O = (v2p_shrub_leaf)0;
	shrub_vtx V = shrub_vertex(vid, iid, SHRUB_K_LEAF);
	const uint sp = V.I.species;
	const float4 P0 = shrub_prof(sp, 0), P1 = shrub_prof(sp, 1), P5 = shrub_prof(sp, 5);
	// (A young tree's spring ahead of the old trees'.)
	const float4 K0 = shrub_seas_lead(sp, V.I.lead);
	const uint kind = uint(P0.x + 0.5f);
	const uint per = uint(da_shrub_draw.x);
	// Near, a spray stands as its leaves: leaf j of the element, each its own hash, a little of its own season.
	// (Past d0 the same leaves stand as quads: four vertices each, the farther spray draw.)
	const uint lv = per == 4u * SHRUB_SPRAY_LEAVES ? 4u : 8u;
	const bool spray3 = per == lv * SHRUB_SPRAY_LEAVES;
	const uint j = spray3 ? V.corner / lv : 0u;
	// The quad draws: the far quads (drLeafCut) and the near lobed sprays' quads (drSprayCut); a near needle sprig's two
	// crossed quads (drNeedle2: vertices 4..7 the second).
	const uint draw_id = uint(da_shrub_draw.z + 0.5f);
	const bool cut = draw_id == SHRUB_DR_LEAF_CUT || draw_id == SHRUB_DR_SPRAY_CUT;
	const bool cross2 = draw_id == SHRUB_DR_NEEDLE2;
	const uint corner = spray3 ? V.corner - j * lv : (cross2 ? V.corner & 3u : V.corner);
	const float hash = spray3 ? frac(V.hash * 7.31f + float(j) * 0.618034f) : V.hash;
	// This bush's place in the season: its own few days early or late (the species' spread between bushes).
	const float early = shrub_early(V.I, sp);
	const float own = spray3 ? (hash - 0.5f) * 0.06f : 0.0f;
	// (Its orders shifted by its own days, held inside the season keys' ends: the share out 1 is every leaf out, the
	// share down 1 every leaf down - a late tree's last leaves past 1 hung on all winter, pale flecks on bare poplars.)
	const float rank = clamp(V.E.a - early + own, 0.025f, 0.975f), fall = clamp(V.E.b - early * 0.5f + own, 0.025f, 0.975f);
	float out_k = saturate((K0.x - rank) * 40.0f), stay_k = saturate((fall - K0.w) * 40.0f);
	// A tree's marcescent leaves - its last to fall, shrub_marcescent's share of them (on an old tree the inner, lower
	// crown's, where its autumn order leaves them) - hang on withered from their fall through the winter and drop as
	// its buds break; a Red Forest pine's dead needles stay on all year.
	bool withered = false;
	[branch] if (V.I.tree)
	{
		if (V.I.red)
			out_k = stay_k = 1.0f;
		else if (V.E.b > 1.0f - shrub_marcescent(V.I, sp) && (stay_k < 1.0f || K0.x < 0.02f))
		{
			withered = true;
			out_k = stay_k = 1.0f;
		}
	}
	if (!V.valid || V.I.dead || out_k * stay_k <= 0.0f || (spray3 && float(j) >= max(round(P0.z), 1.0f)))
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const shrub_elem E = V.E;
	const shrub_inst I = V.I;
	const float2 lw = shrub_h2(E.w.z);
#ifndef SHRUB_SHADOW
	// (Its texture's square past the element, a spray's leaves splayed round it, the pixel floor's growth.)
	[branch] if (!shrub_cut_draw())
	{
		if (shrub_out_of_view(I, shrub_world(I, E.p), 2.2f * max(lw.x, lw.y) * I.scale * max(V.grow, 1.0f)))
		{
			O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
			return O;
		}
	}
#endif
	const float3 dir_t = shrub_elem_dir(E, I.tree), side_t = shrub_leaf_side(dir_t, E.w.y), crown_t = shrub_oct12(E.w.y >> 8);
	const float s_axis = f16tof32(E.w.w & 0xFFFFu);
	const float light = float((E.w.w >> 16) & 0xFFu) / 255.0f;
	// The leaf's size: the cohort's growth, its own expansion over its first weeks out, the survivors' growth.
	// (Once the whole bush is out the last cohort has grown too: the lead closes as the leaf-out completes.)
	const float expand = withered ? 1.0f : smoothstep(0.0f, 0.25f, K0.x - rank + 0.25f * saturate((K0.x - 0.75f) * 4.0f));
	const float size = (withered ? 1.0f : K0.y) * lerp(0.35f, 1.0f, expand) * out_k * stay_k * V.grow;
	const float L = lw.x * size * I.scale, Wd = lw.y * size * I.scale;

	const float3 root = float3(I.xf[0].w, I.xf[1].w, I.xf[2].w);
	const float3 base = shrub_world(I, E.p);
	float3 dir = normalize(shrub_world_dir(I, dir_t));
	// (A mirrored bush's transform turns the cross product over: the side follows so the upper face stays up.)
	const float hand = determinant((float3x3)I.xf) < 0.0f ? -1.0f : 1.0f;
	float3 side = normalize(shrub_world_dir(I, side_t)) * hand;
	float3 nrm = normalize(cross(dir, side));
	float3 lbase = base;
	float Ll = L, Wl = Wd;
	[branch] if (spray3)
	{
		// Leaf j of n: at its node along the stretch (an opposite pair shares one), round the shoot by the species'
		// phyllotaxis from the spray's own side (a two-ranked spray lies flat), splayed by the leaf angle; its length
		// from the spray's width (two splayed leaves and one leaf's width), its width by the species' ratio.
		const float4 P7 = shrub_prof(sp, 7);
		const float n = max(round(P0.z), 1.0f), splay = P0.w, ratio = P7.y;
		const uint per_node = P7.w > 1.5f ? 2u : 1u;
		const float nodes = ceil(n / float(per_node));
		const float node = float(j / per_node), m = float(j % per_node);
		const float ll = Wd / (2.0f * max(sin(splay), 0.3f) + ratio);
		float s_az, c_az;
		sincos(node * P7.z + m * 3.14159265f + (V.hash - 0.5f) * 0.8f, s_az, c_az);
		const float3 radial = side * c_az + nrm * s_az;
		lbase = base + dir * (L * (node + 0.25f) / nodes);
		const float3 ld = normalize(dir * cos(splay) + radial * sin(splay));
		side = normalize(cross(dir, radial));
		dir = ld;
		nrm = normalize(cross(dir, side));
		Ll = ll;
		Wl = ll * ratio;
	}
	// The texture's square: the element's length and width by the species' fit (the square holds the petiole and a
	// margin round the leaf the element leaves out), starting that far behind the element's base. A far spray's
	// billboard is its card.
	const float4 T8 = shrub_prof(sp, 8);
	const bool sprayish = kind == SHRUB_LEAF_SPRAY;
	const bool card = sprayish && !spray3 && T8.z >= 0.0f;
	const float3 fit = shrub_prof(sp, card ? 12u : 11u).xyz;
	lbase -= dir * (fit.z * Ll);
	Ll *= fit.x;
	Wl *= fit.y;
	// A needle sprig and a far spray are round, a brush about their shoot: the quad stands broadside to the camera
	// (the light in the cascades), along the shoot as the screen shows it - a shoot seen end-on still shows its
	// needles instead of a foreshortened sliver.
#ifdef SHRUB_SHADOW
	const float3 to_eye = da_shrub_sun.xyz;
#else
	const float3 to_eye = normalize(da_shrub_cam.xyz - lbase);
#endif
	// A two-ranked spray (flat sprays) keeps its own plane.
	const bool flat_spray = abs(shrub_prof(sp, 7).z - 3.14159265f) < 0.3f;
	const bool round_kind = kind == SHRUB_LEAF_NEEDLES || (sprayish && !spray3 && !flat_spray);
	// A spray's billboard is lit by its leaves' own facing (their upper or under side, as the near leaves show it); a
	// needle brush looks the same from every side and keeps the billboard's.
	float3 nrm_lit = sprayish ? nrm : float3(0.0f, 0.0f, 0.0f);
	// The billboard's side (a near sprig's texture frame: its second quad's own side can face the eye).
	const float3 side_lit = normalize(cross(to_eye, dir) + float3(0.0f, 1e-5f, 0.0f));
	[branch] if (cross2)
	{
		// Near, a needle sprig is two crossed quads about its shoot - its own side and square to it - a brush from any
		// side (one quad turned to the eye showed a flat frond, a fern); each four fifths as wide (crossed, the two cover
		// a quarter more than one), lit by the billboard's normal (a brush looks the same from every side).
		if (V.corner >= 4u)
		{
			const float3 s2 = nrm;
			nrm = -side;
			side = s2;
		}
		Wl *= 0.8f;
		nrm_lit = normalize(cross(dir, side_lit));
	}
	else if (round_kind)
	{
		float3 d_s = dir - to_eye * dot(dir, to_eye);
		const float3 up_s = float3(0.0f, 1.0f, 0.0f) - to_eye * to_eye.y;
		d_s += up_s * (saturate(0.35f - length(d_s)) * 3.0f) + float3(1e-5f, 0.0f, 0.0f);
		dir = normalize(d_s);
		side = normalize(cross(to_eye, dir) + float3(0.0f, 1e-5f, 0.0f));
		nrm = normalize(cross(dir, side));
	}

	const float2 c = shrub_leaf_corner(corner, cut || cross2 || (spray3 ? lv : per) == 4u, sp);
	// The blade's shape: the midrib drooping to the tip. Its fold along the midrib (a compound leaf's along its rachis)
	// is shaded (the pixel shader turns each half's normal up toward the midrib): the hull has no vertex on the midrib
	// to fold the geometry about, and a quad's lifted halves floated the leaf off its petiole.
	const float droop = round_kind ? 0.0f : P5.x;
	const float fold = round_kind ? 0.0f : P1.w;
	float3 off = dir * (c.y * Ll) - nrm * (droop * Ll * c.y * c.y) + side * (c.x * 0.5f * Wl);
	// The texture: the species' leaf a or b (a far spray's billboard its card), mirrored by the leaf's own hash.
	const float layer = card ? T8.z : shrub_leaf_layer(sp, hash);
	const float mirror = frac(hash * 11.3f) < 0.5f ? -1.0f : 1.0f;
	const float2 uv = float2(0.5f + 0.5f * c.x * mirror, c.y);
#ifndef SHRUB_SHADOW
	// Small and edge-on, a leaf turns a little toward the camera (it never thins below the floor).
	const float view_z = max(dot(lbase - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
	const float px_w = Wl / max(da_shrub_cam.w * view_z, 1e-5f);
	const float edge = (1.0f - abs(dot(nrm, to_eye))) * saturate(1.0f - px_w / 6.0f);
	off += cross(dir, to_eye) * (c.x * 0.5f * Wl * 0.6f * edge);
#endif

	// Flutter about the petiole (pitch about the side, roll about the midrib) in the wind's eddies.
	const shrub_wind W = shrub_wind_at(I, false);
	const float2 fl = shrub_flutter(lbase, root, hash, P1.z, light, W);
	float sp1, cp1, sr1, cr1;
	sincos(fl.x, sp1, cp1);
	sincos(fl.y, sr1, cr1);
	float3 o1 = shrub_rotate(shrub_rotate(off, side, sp1, cp1), dir, sr1, cr1);
	float3 q = lbase + o1;
	float3 n = shrub_rotate(shrub_rotate((round_kind && sprayish) || cross2 ? nrm_lit : nrm, side, sp1, cp1), dir, sr1, cr1);
	float3 t = shrub_rotate(cross2 ? side_lit : side, dir, sr1, cr1);
	const float h_rest = shrub_root_height(I, q);
	shrub_bend(q, n, t, V.slot, E.axis, s_axis, I, false);
	[branch] if (I.tree)
		q = shrub_tree_sway(q, I, h_rest, false);

#ifdef SHRUB_SHADOW
	O.hpos = mul(m_WVP, float4(q, 1.0f));
	O.tex = float4(uv, layer, shrub_leaf_flags(spray3 ? SHRUB_LEAF_SIMPLE : kind, mirror, hash));
	return O;
#else
	// The same leaf on the previous frame: the previous wind and flutter, the same shape.
	float3 qp = q;
	[branch] if (shrub_mv_deform(lbase))
	{
		const shrub_wind Wp = shrub_wind_at(I, true);
		const float2 flp = shrub_flutter(lbase, root, hash, P1.z, light, Wp);
		float a, b, e, f;
		sincos(flp.x, a, b);
		sincos(flp.y, e, f);
		qp = lbase + shrub_rotate(shrub_rotate(off, side, a, b), dir, e, f);
		float3 np = nrm, tp = side;
		shrub_bend(qp, np, tp, V.slot, E.axis, s_axis, I, true);
		[branch] if (I.tree)
			qp = shrub_tree_sway(qp, I, h_rest, true);
	}
	O.hpos = mul(m_WVP, float4(q, 1.0f));
	O.hpos_cur = mul(m_WVP_nojit, float4(q, 1.0f));
	O.hpos_prev = mul(m_WVP_prev, float4(qp, 1.0f));
	O.pv_u = float4(mul(m_WV, float4(q, 1.0f)).xyz, c.x);
	O.n_v = float4(mul((float3x3)m_WV, n), c.y);

	// Colour: the season's, per leaf; the underside lighter and, for the willows' silvery ones, bluer.
	float4 col = shrub_leaf_colour_k(sp, K0, rank, fall, light, hash, 0.0f);
	if (I.red || withered)
		col = float4(shrub_seas(sp, 7).rgb * (1.0f + shrub_prof(sp, 2).z * (frac(hash * 13.7f) * 2.0f - 1.0f)), 1.0f);
	const float4 P2 = shrub_prof(sp, 2);
	float3 upper = col.rgb;
	float3 under = lerp(upper * P2.x, shrub_seas(sp, 8).rgb, 0.5f * (1.0f - col.w));
	under.b *= 1.0f + P2.y;
	// Snow settles on the upward faces of the leaves still on in winter (evergreens, early snow): a leaf's by its own
	// facing; a card's or a needle sprig's - turned to the eye, their own facing level - by the crown's there (the snow
	// caps a conifer's branches on the crown's upper side; by the card's own the near pines stood bare against white
	// clusters behind them). The clusters and the blobs take the same rule (da_shrub_cluster.ps).
	const float3 n_snow = card || kind == SHRUB_LEAF_NEEDLES ? normalize(n + 2.0f * normalize(shrub_world_nrm(I, crown_t))) : n;
	const float snow = da_shrub_sun.w * saturate(n_snow.y * 1.8f - 0.4f);
	upper = lerp(upper, float3(0.8f, 0.82f, 0.85f), snow);
	// The light inside the crown: the outer top open, the inside and the underside in its shade, lit green; past the
	// near cascade, the self-shadow its shadow maps lose.
	const float4 cl = shrub_crown_light(light, sp, false, col.rgb);
	const float far_k = shrub_far_shade(light, view_z);
	O.alb_h = float4(shrub_to_albedo(upper * cl.yzw * far_k), saturate(I.hemi) * cl.x);
	// (A withered leaf is dry and thick: little light through it - backlit at the living leaves' share it glowed red.)
	O.und_t = float4(shrub_to_albedo(under * cl.yzw * far_k), shrub_translucency(P1.x * (withered ? 0.15f : lerp(1.0f, 0.6f, col.w)), to_eye));
	// The G-buffer's gloss scales the sun's specular five times over (combine_1.ps): the species' cuticle gloss
	// (0 matte .. 0.4 a cherry's) maps to a few hundredths, as the blades' (da_grass_blades.cpp row 14).
	// A needle brush's shine scatters over its needles' every angle: a third of a blade's.
	const float gloss = (0.01f + 0.075f * P1.y) * lerp(1.0f, 0.3f, col.w) * lerp(1.0f, 0.5f, smoothstep(10.0f, 40.0f, view_z))
		* (kind == SHRUB_LEAF_NEEDLES ? 0.35f : 1.0f);
	// The texture's +u in view space (a mirrored leaf's runs the other way).
	O.s_g = float4(mul((float3x3)m_WV, t) * mirror, gloss);
	// (Its bite: 0..15 by its hash, the pixel shader bites the margin of the low few.)
	O.tex = float4(uv, layer, shrub_leaf_flags(spray3 ? SHRUB_LEAF_SIMPLE : kind, mirror, hash));
	// A card's or a sprig's twig: the young twig colour in the crown's light.
	O.twig = float4(shrub_to_albedo(shrub_seas(sp, 10).rgb * cl.yzw * far_k), px_w);
	// Part of the light's turn across the bush is the crown's (the production foliage's bent normals): a third near,
	// where each leaf's own facing and relief read, half from 15 m.
	O.crown = float4(mul((float3x3)m_WV, normalize(shrub_world_nrm(I, crown_t))), lerp(0.3f, 0.5f, smoothstep(5.0f, 15.0f, view_z)));
	O.fold = fold;
	return O;
#endif
}
