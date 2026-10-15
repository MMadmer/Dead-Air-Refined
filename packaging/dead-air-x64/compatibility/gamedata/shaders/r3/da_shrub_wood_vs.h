// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural shrubs, the vertex shader of the wood (docs/dead-air/SHRUBS.md): every segment of a stem, branch,
// twig or shoot is a tube of five sides (eight within d0) between two rings, built from SV_VertexID (64 segments a
// chunk), bent ring by ring through its chain of axes. Where a segment meets the next one of its axis the ring is
// square to their mean direction and its frame is the axis's, so the two tubes share that ring and close (each
// square to its own direction left a wedge open on the outer side of every bend). The bark is the species' texture
// (tools/shrubs/textures), a whole number of its 6 cm tiles round every ring of the tapering stem and its real size
// along it, continuous over the axis. A twig is never thinner than the pixel floor (the wire rule), and under it keeps its cover by standing
// only at the ratio of its width to the floor, its own hash deciding (phone-wire AA: a twig at a tenth of a pixel
// held a pixel wide drew the crown's twigs many times their area, a web the far tiers then lost); the bark darkens
// inside the crown, the young growth takes the twig colour, the upper side takes the snow. SHRUB_SHADOW: the sun
// cascades, at least a texel thick, depth only. A tree's grafted wood (docs/dead-air/TREES.md) is drawn the same way,
// its normals through the stock placement's uneven scale, its bark in the species' zones, swaying with its trunk;
// the trees' stock tubes and patches (da_tree_stock_vs.h, da_tree_patch.vs) take the same bark outputs
// (shrub_wood_outputs) with SHRUB_WOOD_NO_MAIN. Where a tree's limb or thick branch leaves its parent the child's tube
// wears the parent's bark toward the parent's surface (a second mapping and its weight, shrub_join_map) and the parent's
// light, zone and age: the two meet without a seam. SHRUB_WOOD_FAR: the far wood (da_shrub_wood_far.ps) - the bark's
// mean worked out per vertex, five interpolants.

#include "common.h"
#include "da_velocity.h"
#include "da_shrub_common.h"

#ifdef SHRUB_WOOD_FAR
// The far wood's (da_shrub_wood_far.ps): the bark's mean colour with its zone, lichen, algae, snow and the crown's light
// worked out here - a far tree's wood took seventeen interpolants a vertex for a bark under a pixel.
struct v2p_shrub_wood
{
	float4	hpos		: SV_Position;
	float4	hpos_cur	: TEXCOORD0;
	float4	hpos_prev	: TEXCOORD1;
	float4	pv_g		: TEXCOORD2;	// view-space position, gloss
	float4	n_a			: TEXCOORD3;	// view-space normal, hemi x occlusion
	float4	alb_w		: TEXCOORD4;	// albedo (G-buffer encoding: the lower bark's of a zoned tree), the radius over the
									// pixel floor (the wire rule's share)
	float	snow		: TEXCOORD5;
	float4	up_z		: TEXCOORD6;	// a zoned tree's upper bark's albedo, the zone's share at the vertex (-1 none)
	float4	pr_k		: TEXCOORD7;	// the bark's rest position (the near wood's pr), the boundary run's frequency
};
#else
struct v2p_shrub_wood
{
	float4	hpos		: SV_Position;
#ifndef SHRUB_SHADOW
	float4	hpos_cur	: TEXCOORD0;
	float4	hpos_prev	: TEXCOORD1;
	float4	pv_g		: TEXCOORD2;	// view-space position, the young twig's gloss
	float4	n_u			: TEXCOORD3;	// view-space normal, the turn round the stem (0..1 from the axis's phase)
	float4	t_v			: TEXCOORD4;	// view-space axis direction, the bark's v (tiles along)
	float4	alb_h		: TEXCOORD5;	// the young twig colour (linear), hemi x occlusion
	float4	lit			: TEXCOORD6;	// the crown's light on the albedo (shrub_crown_light yzw), w a stock patch's world
									// projection's tile (the species', world metres)
	float4	misc		: TEXCOORD7;	// x bark layer, y young twig (0..1), z snow on the branch's top, w radius over the floor
	float4	moss		: TEXCOORD8;	// x the lichen's share here, y tiles round the axis's base (a stock patch: its world
									// projection's weight), z handedness (-1 mirrored), w algae
	nointerpolation float4	bark	: TEXCOORD9;	// the species' bark tint (linear rgb), its contrast
	nointerpolation float4	bark2	: TEXCOORD10;	// the young growth's share of the bark's detail, the tile's mean luminance,
												// the tiles round the segment's first and last ring
	float	ring		: TEXCOORD11;	// 0 at the segment's first ring .. 1 at its last; a stock patch's ring coordinate
									// from the first ring its triangle's runs span; a cut end's place along its
									// axis (m)
	float4	zone		: TEXCOORD12;	// a tree's upper bark: x its layer (-1 none or a cut end's), y its share
									// here (a cut end's: its rim gauge, 1 on the border, 0 inside, -2 none), z its
									// tile's mean luminance (negative: it takes the lower's ridges first; a cut
									// end's: 1 a flat cut, 0 a break); w the bark's
									// mapping: 0 round the axis,
									// 1 a stock patch's by its two tubes, 2 a cut end's end grain
	nointerpolation float4	bark3	: TEXCOORD13;	// the upper bark's contrast, its tiles per the lower's, its value and
												// chroma; a cut end's: its species, seed, mean rim radius (m) and callus
												// (1 a living tree)
	float3	pr			: TEXCOORD14;	// the rest position (m from the root, the instance's own offset): the bark's noises
	float3	jn			: TEXCOORD15;	// a second bark mapping: on a stock patch its second axis's (the turn round it, its
										// tiles along it, its weight); near a junction the parent's - the place against the
										// junction (along the child's way out, across it, along the parent; template metres:
										// linear, the turn and the weight the pixel shader's)
	float	jw			: TEXCOORD16;	// a patch's second axis's ring coordinate (as ring); a junction's: the parent
										// segment's fraction there
	nointerpolation float4	jc0	: TEXCOORD17;	// a junction's: the turn's offset, the tiles' offset and scale along the
												// parent, the parent segment's first ring's count of tiles round it; a
												// stock patch's: its second axis's count round the first ring its
												// triangle's runs span and the steps after it
	nointerpolation float4	jc1	: TEXCOORD18;	// a junction's: the parent's profile radius toward the child, the blend's
												// length, the junction's reach (0: no junction), the last ring's count; a
												// stock patch's: its first axis's counts as jc0's
#endif
};
#endif

// A tree's junction with its parent at a vertex: the weight of the parent's light, level and zone there (1 on the
// parent's surface .. 0 out of a wide blend: the child's first segment crosses the parent's surface far along it, its
// vertices' weights interpolated) and the parent's sky, level byte and radius there (world: an age zone's).
struct shrub_join
{
	float	w;
	float	light;
	uint	level_byte;
	float	r;
};

shrub_join	shrub_join_none	()
{
	shrub_join J;
	J.w = 0.0f;
	J.light = 0.0f;
	J.level_byte = 0u;
	J.r = 0.0f;
	return J;
}

// A junction record (da_trees.cpp pack_tree, two aux slots): the parent's axis point and tangent at the attach, the
// child's way out across it, the parent's mean radius and its profile's toward the child, the parent's arclength there,
// the turn of the way out in the parent's bark, its sky and level byte; the parent's segment there - its rings' radii
// and their arclengths from the attach.
struct shrub_join_rec
{
	float3	c, t, d0;
	float	r, r_out, s, turn, light;
	uint	level_byte;
	float	ra, rb, sa, sb;
};

shrub_join_rec	shrub_join_load	(uint slot)
{
	const uint4 a = da_shrub_aux.Load4(slot * SHRUB_AUX_SLOT), b = da_shrub_aux.Load4(slot * SHRUB_AUX_SLOT + 16u);
	const uint4 e = da_shrub_aux.Load4(slot * SHRUB_AUX_SLOT + 32u);
	shrub_join_rec J;
	// (A stock parent's radii in full - words 11 and 12 -, as its tube counts its tiles round them; a grafted parent's
	// in halves, as its own segment holds them.)
	const float2 seg_r = shrub_h2(e.x);
	const float ra_full = asfloat(e.w), rb_full = asfloat(da_shrub_aux.Load(slot * SHRUB_AUX_SLOT + 48u));
	J.ra = ra_full > 0.0f ? ra_full : seg_r.x;
	J.rb = ra_full > 0.0f ? rb_full : seg_r.y;
	J.sa = asfloat(e.y);
	J.sb = asfloat(e.z);
	J.c = asfloat(a.xyz);
	J.t = shrub_oct(a.w);
	J.d0 = shrub_oct(b.x);
	const float2 rr = shrub_h2(b.y);
	J.r = rr.x;
	J.r_out = rr.y;
	J.s = asfloat(b.z);
	J.turn = float(b.w & 0xFFFFu) / 65536.0f;
	J.light = float((b.w >> 16) & 0xFFu) / 255.0f;
	J.level_byte = b.w >> 24;
	return J;
}

// The whole count of tiles round a ring (r: its radius, template metres; tile: its axis's, world metres) - the tube's,
// its junctions' and its patches' alike (one expression: two rounded apart by an ulp about a half and seamed there).
float	shrub_ring_count	(float r, float scale, float tile)
{
	return max(round(6.2831853f * r * scale / tile), 1.0f);
}

// An axis's tile (m): the species' tile on every axis a tile round and more - one scale over a tree's trunk and its
// limbs (each axis's own, a whole number round its base girth, met a limb's bark with its trunk's at a fifth to half
// again the size) -, a thinner one's base girth (axis_r: its base radius, template metres; its single tile shrinks both
// ways rather than squeezing its lenticels into dashes); the rings' whole counts round them by shrub_ring_count, the
// base ring's in wraps0.
float	shrub_axis_tile	(shrub_inst I, float axis_r, out float wraps0)
{
	const float girth0 = 6.2831853f * max(axis_r * I.scale, 1e-4f);
	const float species_tile = SHRUB_BARK_TILE * max(shrub_prof(I.species, 14).w, 0.25f);
	const float tile = min(species_tile, girth0);
	wraps0 = max(round(girth0 / tile), 1.0f);
	return tile;
}

// A vertex of the child near its junction (q, template space; child_r the child's base radius): its place against the
// junction (along the child's way out, across it - the turn's zero and quarter -, along the parent: linear, so the pixel
// shader takes the turn round the parent and the bark's weight per pixel; interpolated, both went wrong across the
// child's first segment, which crosses the parent's surface far along it), the parent segment's fraction there and the
// junction's constants (jc0: the turn's offset, the tiles' offset and scale along the parent, the segment's first ring's
// count of tiles round it; jc1: the parent's profile radius toward the child, the bark's blend - 80 % of the child's
// radius, 1.2 .. 25 cm -, the junction's reach - da_trees.cpp marks the wood within it -, the last ring's count); the
// weight of the parent's light, level and zone here (a blend three times the bark's, 30 cm at the least).
void	shrub_join_vertex	(shrub_inst I, shrub_join_rec J, shrub_axis P, float child_r, float3 q, out float3 jn, out float jf,
	out float4 jc0, out float4 jc1, out float w_attr)
{
	const float blend = clamp(0.8f * child_r, 0.012f, 0.25f);
	const float reach = 2.0f * (J.r + blend) + 0.05f;
	const float3 d = q - J.c;
	jn = float3(dot(d, J.d0), dot(d, cross(J.t, J.d0)), dot(d, J.t));
	float wraps0;
	const float tile = shrub_axis_tile(I, P.r, wraps0);
	const float vs = I.scale / (tile * max(shrub_prof(I.species, 18).y, 0.25f));
	jc0 = float4(J.turn + P.phase, J.s * vs + P.phase * 7.0f, vs, shrub_ring_count(J.ra, I.scale, tile));
	jc1 = float4(J.r_out * 1.02f, blend, reach, shrub_ring_count(J.rb, I.scale, tile));
	jf = (jn.z - J.sa) / max(J.sb - J.sa, 1e-4f);
	w_attr = (1.0f - smoothstep(J.r_out, J.r_out + max(3.0f * blend, 0.3f), length(jn.xy))) * (1.0f - smoothstep(0.5f * reach, reach, length(d)));
}

#if !defined(SHRUB_SHADOW) && !defined(SHRUB_WOOD_FAR)
// The bark's outputs of a wood vertex (da_shrub_wood.ps): q its bent position, n and t its normal and the axis's
// direction there (world), C the ring's centre (world, unbent), u the turn round the stem (0..1 from the axis's
// phase), s the arclength along the axis (template metres), r_a and r_b the segment's first and last ring's radii and
// r_true this vertex's (template metres; the last two world), floor_r the pixel floor's radius (world), level the wood's
// level (the element's level byte: 0x80 a dead stub, 0x40 a prickle), light its sky visibility, ring 0 or 1, axis
// radius, length and base height (template space) and phase, the young growth's share (twig), the rest position
// (world, unbent: the bark's noises) and the junction there (its weight and the parent's sky and level: J).
void	shrub_wood_outputs	(inout v2p_shrub_wood O, shrub_inst I, float3 q, float3 n, float3 t, float3 C, float u, float s,
	float r_a, float r_b, float r_true, float floor_r, uint level_byte, float light, uint ring, float axis_r, float axis_len,
	float axis_base_y, float axis_phase, float twig_in, float3 q_rest, shrub_join J)
{
	const float3 root = float3(I.xf[0].w, I.xf[1].w, I.xf[2].w);
	const float view_z = max(dot(C - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
	// (The noises' space: metres from the root, each instance its own offset.)
	// (Up to 64 m: a world projection's coordinates over the tile stay a few hundred, its texel exact in fp32.)
	O.pr = q_rest - root + float3(shrub_u01(shrub_hash(I.seed * 3u + 1u)), shrub_u01(shrub_hash(I.seed * 3u + 2u)), shrub_u01(shrub_hash(I.seed * 3u + 3u))) * 64.0f;
	O.jn = 0.0f;
	O.jw = 1.0f;
	O.jc0 = 0.0f;
	O.jc1 = 0.0f;
	// The bark: the axis's tile (shrub_axis_tile: the species' on every axis a tile round and more; a thin twig's
	// single tile shrinks both ways rather than squeezing its lenticels into dashes), its real size along the axis and
	// a whole number of them round each ring - the count held from the base squeezed a tapering
	// stem's bark into slivers, a hawthorn's plates into smooth elephant skin. The pixel shader takes the turn round
	// the stem (0..1) times the count of the segment's first and of its last ring and blends the two along it where
	// they differ (one count interpolated between them seamed and twisted the bark; a count blended about each half
	// double-exposed every stem whose girth asked about a half); each axis's pattern turned and shifted its own way (no
	// two stems alike). A stock patch and a child near its junction take the very counts of their tube's rings.
	float wraps0;
	const float tile = shrub_axis_tile(I, axis_r, wraps0);
	const float wraps_a = shrub_ring_count(r_a, I.scale, tile);
	const float wraps_b = shrub_ring_count(r_b, I.scale, tile);
	const float4 P18 = shrub_prof(I.species, 18);
	O.n_u = float4(mul((float3x3)m_WV, n), u);
	// (Along the stem by the species' stretch: an aspen's rhombs taller than wide.)
	O.t_v = float4(mul((float3x3)m_WV, t), s * I.scale / (tile * max(P18.y, 0.25f)) + axis_phase * 7.0f);
	O.ring = ring != 0u ? 1.0f : 0.0f;

	// Bark on the stems and branches, the twig colour on the young growth (the tip of a branch, every twig and
	// shoot).
	const uint level = level_byte & 0x3Fu;
	// A dead stub of the stool (the level byte's top bit, da_shrubs.cpp pack_template): old wood, its bark without the
	// young growth's colour; a prickle or thorn (the next bit) in the species' own colour (a rose's red-brown hooks; in
	// the twig green they hid on the green canes).
	const bool stub = (level_byte & 0x80u) != 0u;
	const bool prickle = (level_byte & 0x40u) != 0u;
	const float3 prickle_c = shrub_prof(I.species, 14).rgb;
	// (Toward a junction the parent's: its old wood, its level, its sky.)
	const float twig = (stub ? 0.0f : twig_in) * (1.0f - J.w);
	const uint level_p = J.level_byte & 0x3Fu;
	const float level_f = lerp(float(level), float(level_p), J.w);
	light = lerp(light, J.light, J.w);
	// Snow on the upper side of the stems and branches, less on the thin shoots that shed it: how much, here; where,
	// per pixel (da_shrub_wood.ps: a crisp band along the top - by the vertices of a stem's few sides it washed the
	// whole upper half pale beige).
	const float snow = da_shrub_sun.w * saturate(1.0f - 0.2f * level_f);
	// Occlusion: the sky the segment sees past the leaves (da_shrub_gen.cpp sky_visibility), and the base and the
	// stems a little darker than the outer twigs (the crown's own wood and the ground round its foot).
	const float h_rel = saturate((C.y - root.y) / max(axis_len * I.scale + axis_base_y * I.scale, 0.3f));
	const float4 cl = shrub_crown_light(light, I.species, I.dead, shrub_leaf_colour_mean(I.species));
	// (The ground's bounce and the gaps keep a crown's wood out of black: at least a fifth of the sky.)
	const float ao = max(cl.x * lerp(0.7f, 1.0f, saturate(0.25f + 0.2f * level_f + 0.35f * h_rel)), 0.2f);
	O.pv_g = float4(mul(m_WV, float4(q, 1.0f)).xyz, shrub_prof(I.species, 4).z);
	// The young growth's colour, flushed on its upper, sunlit side where the species reddens there (a dog rose's canes).
	const float4 blush = shrub_prof(I.species, 15);
	const float3 young_c = lerp(shrub_seas(I.species, 10).rgb, blush.rgb, blush.w * smoothstep(-0.2f, 0.8f, normalize(n).y));
	O.alb_h = float4(prickle && dot(prickle_c, prickle_c) > 0.0f ? prickle_c : young_c, saturate(I.hemi) * ao);
	// (A tree's old wood takes a third of the crown's tint: a big spruce's trunk inside its crown took its needles' green
	// into its bark, an olive-brown felt - the light round a trunk is the stand's gaps' and the ground's as much as the
	// crown's. w: the species' tile, world metres - a stock patch's world projection, the upper bark's tongues.)
	const float3 tint = I.tree ? lerp(cl.yzw, 1.0f, 0.67f * (1.0f - twig)) : cl.yzw;
	O.lit = float4(tint * shrub_far_shade(light, view_z), SHRUB_BARK_TILE * max(shrub_prof(I.species, 14).w, 0.25f));
	// (A tree's widened wood stands whole: its pixels at the ratio itself, the pixel shader's root of its square.)
	const float wr = r_true / max(floor_r, 1e-5f);
	O.misc = float4(shrub_prof(I.species, 10).z, twig, snow, I.tree ? wr * wr : wr);
	// Lichen in sparse blotches on the old wood three centimetres thick and more, most near the foot; a green film of
	// algae on the foot's side away from the sun (its lowest forty centimetres).
	const float old_k = (1.0f - twig) * smoothstep(0.012f, 0.02f, r_true) * lerp(level <= 1u ? 1.0f : 0.0f, level_p <= 1u ? 1.0f : 0.0f, J.w);
	const float foot = saturate(1.0f - (C.y - root.y - I.ground) / 0.4f);
	const float3 sun_xz = normalize(float3(da_shrub_sun.x, 0.0f, da_shrub_sun.z) + float3(1e-4f, 0.0f, 0.0f));
	const float shade_side = saturate(-dot(normalize(float3(n.x, 0.0f, n.z) + float3(1e-4f, 0.0f, 0.0f)), sun_xz) + 0.25f);
	O.moss = float4(shrub_prof(I.species, 10).w * old_k * saturate(1.0f - 1.2f * h_rel), wraps0,
		determinant((float3x3)I.xf) < 0.0f ? -1.0f : 1.0f, shrub_prof(I.species, 10).w * old_k * foot * shade_side);
	// The bark as the species wears it (da_shrub_wood.ps) - a thin dead stub's at half its value, easing back by 4 cm
	// across by its true radius (not the pixel floor's: the same at every distance; a thin dead twig keeps its weathered
	// bark: its tile shrunk to its girth reads its mean, without its fissures' shade a pale dowel; a dead limb or its
	// patches keep theirs) -, and
	// its tile's mean luminance (the last mip's texel; the texture quality setting may load fewer mips).
	uint tw, th, tn, mips;
	s_shrub_bark_d.GetDimensions(0u, tw, th, tn, mips);
	const float3 bark_mean = pow(max(s_shrub_bark_d.Load(int4(0, 0, int(O.misc.x + 0.5f), int(mips) - 1)).rgb, 0.0f), 2.2f);
	O.bark = shrub_prof(I.species, 13);
	O.bark.rgb *= stub ? lerp(0.55f, 1.0f, smoothstep(0.012f, 0.02f, (ring != 0u ? r_b : r_a) * I.scale)) : 1.0f;
	O.bark3 = float4(P18.w, max(shrub_prof(I.species, 14).w, 0.25f) / max(P18.z, 0.25f), shrub_prof(I.species, 21).zw);
	O.bark2 = float4(shrub_prof(I.species, 12).w, max(dot(bark_mean, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f), wraps_a, wraps_b);
	// A tree's upper bark (pine's flaky orange above its plated base, a birch's white above its black fissured foot): its
	// zone by the height above the ground at its trunk, by its share of the tree's height there (a young tree's rough
	// foot as low as its height asks) or by the wood's age (its radius), the trunk and the limbs only - the young wood
	// wears the upper bark all along, but a dead twig or stub its weathered old bark (in the young one's smooth skin a
	// dead stub on an old trunk read as a pale dowel).
	O.zone = float4(-1.0f, 0.0f, 1.0f, 0.0f);
	[branch] if (I.tree)
	{
		const float4 Z = shrub_prof(I.species, 17);
		[branch] if (Z.x >= 0.0f)
		{
			// (The mode, and 4 more where the upper bark takes the lower's ridges first.)
			const float mode = Z.w > 3.5f ? Z.w - 4.0f : Z.w;
			const bool age = mode > 0.5f && mode < 1.5f;
			const float hg = C.y - root.y - I.ground;
			const float x = age ? r_true : (mode > 1.5f ? hg / max(I.tree_h - I.ground, 1.0f) : hg);
			const float zd = Z.z - Z.y + (Z.z >= Z.y ? 1e-4f : -1e-4f);
			const float k_age = smoothstep(0.0f, 1.0f, saturate((x - Z.y) / zd));
			// (An age zone's parent by its own radius.)
			const float k_age_p = age ? smoothstep(0.0f, 1.0f, saturate((J.r - Z.y) / zd)) : k_age;
			const float k = lerp(level >= 2u && !stub ? 1.0f : k_age, level_p >= 2u ? 1.0f : k_age_p, J.w);
			const float3 upper_mean = pow(max(s_shrub_bark_d.Load(int4(0, 0, int(Z.x + 0.5f), int(mips) - 1)).rgb, 0.0f), 2.2f);
			// (z: the upper bark's mean luminance, negative where it takes the ridges first.)
			O.zone = float4(Z.x, k, max(dot(upper_mean, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f) * (Z.w > 3.5f ? -1.0f : 1.0f), 0.0f);
		}
	}
}
#endif

#if !defined(SHRUB_SHADOW) && defined(SHRUB_WOOD_FAR)
// The far wood's colour of a bark (its mean in the species' tint, its cavity's): the young growth's over it, the lichen's
// and the algae's means, the snow.
float3	shrub_far_colour	(shrub_inst I, float3 bark, float cavity, float twig, float old_k, float h_rel, float foot, float shade_side,
	float snow, float3 young_c)
{
	const float3 LUMW = float3(0.2126f, 0.7152f, 0.0722f);
	float3 c = lerp(bark, young_c, twig) * lerp(1.0f, cavity, 0.35f);
	// (The lichen's blotches as their mean - a tenth of an old stem, half covering it - and the algae on the foot's
	// shaded side.)
	const float lichen = shrub_prof(I.species, 10).w * old_k * saturate(1.0f - 1.2f * h_rel);
	c = lerp(c, dot(c, LUMW) * 1.15f * float3(0.93f, 1.03f, 0.9f), 0.075f * saturate(lichen * 3.0f));
	c = lerp(c, c * float3(0.8f, 1.0f, 0.7f), saturate(shrub_prof(I.species, 10).w * old_k * foot * shade_side) * 0.6f);
	return lerp(c, float3(0.8f, 0.82f, 0.85f), snow);
}

// The far wood's outputs (da_shrub_wood_far.ps), the near ones' arguments: the bark as its tile's mean (its last mip) in
// the species' tint - a zoned tree's lower and upper barks' both, the zone's share at the vertex and the rest position
// for the pixel shader's hem (the near bark's boundary run; its lenses are under a pixel there) -, the young growth's
// colour, the cavity's, the lichen's and the algae's means, the snow, the crown's light.
void	shrub_wood_outputs	(inout v2p_shrub_wood O, shrub_inst I, float3 q, float3 n, float3 t, float3 C, float u, float s,
	float r_a, float r_b, float r_true, float floor_r, uint level_byte, float light, uint ring, float axis_r, float axis_len,
	float axis_base_y, float axis_phase, float twig_in, float3 q_rest, shrub_join J)
{
	const float3 root = float3(I.xf[0].w, I.xf[1].w, I.xf[2].w);
	const float view_z = max(dot(C - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
	const uint level = level_byte & 0x3Fu;
	const bool stub = (level_byte & 0x80u) != 0u;
	const float twig = stub ? 0.0f : twig_in;
	const float snow = da_shrub_sun.w * saturate(n.y * 1.6f - 0.3f) * saturate(1.0f - 0.2f * float(level));
	const float h_rel = saturate((C.y - root.y) / max(axis_len * I.scale + axis_base_y * I.scale, 0.3f));
	const float4 cl = shrub_crown_light(light, I.species, I.dead, shrub_leaf_colour_mean(I.species));
	const float ao = max(cl.x * lerp(0.7f, 1.0f, saturate(0.25f + 0.2f * float(level) + 0.35f * h_rel)), 0.2f);
	uint tw, th, tn, mips;
	s_shrub_bark_d.GetDimensions(0u, tw, th, tn, mips);
	const float4 lower = s_shrub_bark_d.Load(int4(0, 0, int(shrub_prof(I.species, 10).z + 0.5f), int(mips) - 1));
	float3 bark = pow(max(lower.rgb, 0.0f), 2.2f), bark_up = bark;
	float cavity = lower.a, cavity_up = lower.a, zone_k = -1.0f;
	[branch] if (I.tree)
	{
		const float4 Z = shrub_prof(I.species, 17);
		[branch] if (Z.x >= 0.0f)
		{
			const float mode = Z.w > 3.5f ? Z.w - 4.0f : Z.w;
			const bool age = mode > 0.5f && mode < 1.5f;
			const float hg = C.y - root.y - I.ground;
			const float x = age ? r_true : (mode > 1.5f ? hg / max(I.tree_h - I.ground, 1.0f) : hg);
			const float zd = Z.z - Z.y + (Z.z >= Z.y ? 1e-4f : -1e-4f);
			zone_k = level >= 2u && !stub ? 1.0f : smoothstep(0.0f, 1.0f, saturate((x - Z.y) / zd));
			// (The upper bark's value and chroma against its tile's, shrub_prof row 21.)
			const float4 upper = s_shrub_bark_d.Load(int4(0, 0, int(Z.x + 0.5f), int(mips) - 1));
			const float4 P21 = shrub_prof(I.species, 21);
			bark_up = pow(max(upper.rgb, 0.0f), 2.2f);
			bark_up = lerp(dot(bark_up, float3(0.2126f, 0.7152f, 0.0722f)).xxx, bark_up, P21.w) * P21.z;
			cavity_up = upper.a;
		}
	}
	// (A thin dead stub's bark at half its value, easing back by 4 cm across.)
	const float3 tint = shrub_prof(I.species, 13).rgb * (stub ? lerp(0.55f, 1.0f, smoothstep(0.012f, 0.02f, (ring != 0u ? r_b : r_a) * I.scale)) : 1.0f);
	const float4 blush = shrub_prof(I.species, 15);
	const float3 young_c = lerp(shrub_seas(I.species, 10).rgb, blush.rgb, blush.w * smoothstep(-0.2f, 0.8f, normalize(n).y));
	const float old_k = (1.0f - twig) * smoothstep(0.012f, 0.02f, r_true) * (level <= 1u ? 1.0f : 0.0f);
	const float foot = saturate(1.0f - (C.y - root.y - I.ground) / 0.4f);
	const float3 sun_xz = normalize(float3(da_shrub_sun.x, 0.0f, da_shrub_sun.z) + float3(1e-4f, 0.0f, 0.0f));
	const float shade_side = saturate(-dot(normalize(float3(n.x, 0.0f, n.z) + float3(1e-4f, 0.0f, 0.0f)), sun_xz) + 0.25f);
	const float3 lit = (I.tree ? lerp(cl.yzw, 1.0f, 0.67f * (1.0f - twig)) : cl.yzw) * shrub_far_shade(light, view_z);
	// (The cavity's shade on the normal's occlusion by the zone's share here: the hem's tongues are the pixel shader's.)
	const float cavity_k = lerp(cavity, cavity_up, saturate(zone_k));
	O.pv_g = float4(mul(m_WV, float4(q, 1.0f)).xyz, lerp(0.015f, 0.008f + 0.05f * shrub_prof(I.species, 4).z, twig) * (1.0f - snow));
	O.n_a = float4(mul((float3x3)m_WV, n), saturate(I.hemi) * ao * lerp(1.0f, cavity_k, 0.7f));
	const float wr = r_true / max(floor_r, 1e-5f);
	O.alb_w = float4(shrub_to_albedo(shrub_far_colour(I, bark * tint, cavity, twig, old_k, h_rel, foot, shade_side, snow, young_c) * lit),
		I.tree ? wr * wr : wr);
	O.snow = snow;
	O.up_z = float4(0.0f, 0.0f, 0.0f, zone_k);
	O.pr_k = 0.0f;
	[branch] if (zone_k >= 0.0f)
	{
		O.up_z.rgb = shrub_to_albedo(shrub_far_colour(I, bark_up * tint, cavity_up, twig, old_k, h_rel, foot, shade_side, snow, young_c) * lit);
		// (The near wood's rest position and run frequency: the hem's tongues where the near bark has them.)
		O.pr_k = float4(q_rest - root + float3(shrub_u01(shrub_hash(I.seed * 3u + 1u)), shrub_u01(shrub_hash(I.seed * 3u + 2u)),
			shrub_u01(shrub_hash(I.seed * 3u + 3u))) * 64.0f, clamp(0.3f / max(SHRUB_BARK_TILE * max(shrub_prof(I.species, 14).w, 0.25f), 0.05f), 0.6f, 2.5f));
	}
}
#endif

// A segment's ring at a joint, when the element beside it in the buffer is the next (previous) segment of its axis
// (the wood is packed axis by axis, in order along each): the mean of the two directions, and at the tip the next
// segment's own base point and arclength, so both sides build the very same ring and the bark runs on unbroken (the
// tip's own sum of half floats missed them by a few millimetres).
struct shrub_ring
{
	float3	t;		// the ring's normal
	float3	p;		// the ring's centre (template space)
	float	s;		// the arclength there
};

shrub_ring	shrub_ring_at	(uint index, shrub_elem E, float3 dir, float len, float s0, float r_end, bool tip, bool wide)
{
	shrub_ring R;
	R.t = dir;
	R.p = tip ? E.p + dir * len : E.p;
	R.s = tip ? s0 + len : s0;
	uint total;
	da_shrub_elems.GetDimensions(total);
	if ((!tip && index == 0u) || (tip && (index + 2u) * SHRUB_REC > total))
		return R;
	const shrub_elem B = shrub_elem_load(tip ? index + 1u : index - 1u, wide);
	const float3 bd = shrub_oct(B.w.x);
	const float2 b_lr = shrub_h2(B.w.y), b_rs = shrub_h2(B.w.z);
	const float3 b_joint = tip ? B.p : B.p + bd * b_lr.x;
	const float3 gap = R.p - b_joint;
	// The same axis, the same point (within 2 mm: the length is a half float, the direction an octahedral pair) and
	// the same radius there (a leaf or a blob beside the wood never matches all three).
	const float b_r = tip ? b_lr.y : b_rs.x;
	if (B.axis != E.axis || dot(gap, gap) >= 4e-6f || dot(bd, dir) <= 0.0f || abs(b_r - r_end) > 0.05f * r_end + 1e-5f)
		return R;
	R.t = normalize(dir + bd);
	if (tip)
	{
		R.p = B.p;
		R.s = b_rs.y;
	}
	return R;
}

#ifndef SHRUB_WOOD_NO_MAIN
v2p_shrub_wood	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_shrub_wood O = (v2p_shrub_wood)0;
	const shrub_vtx V = shrub_vertex(vid, iid, SHRUB_K_WOOD);
	if (!V.valid)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const shrub_elem E = V.E;
	const shrub_inst I = V.I;
	const float3 dir = shrub_oct(E.w.x);
	const float2 lr = shrub_h2(E.w.y), rs = shrub_h2(E.w.z);
#ifndef SHRUB_SHADOW
	[branch] if (!shrub_cut_draw())
	{
		if (shrub_out_of_view(I, shrub_world(I, E.p + dir * (0.5f * lr.x)), (0.5f * lr.x + lr.y) * I.scale))
		{
			O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
			return O;
		}
	}
#endif
	// The tube's sides (two rings of sides + 1 vertices, the last a copy of the first at a whole wrap of the bark).
	const uint sides = uint(da_shrub_draw.x) / 2u - 1u;
	const uint ring = V.corner / (sides + 1u), k = V.corner - ring * (sides + 1u);
	// The wire rule: never under the pixel floor (a shadow texel in the cascades); a segment under it stands by the
	// ratio of its mean width to the floor at its middle (the same for all its vertices), its axis's hash deciding - a
	// twig thins away from its tip, whole (each segment's own hash broke the far twigs into floating dashes) -, decided
	// before the ring's neighbour lookup below, which most of a crown's thin twigs would pay for nothing. In the view
	// the square root of the ratio stands and the pixel shader keeps the same share of its pixels (the ratio in all; the
	// two at the ratio each took its square); the shadow maps keep no share of pixels: the ratio stands. A tree's by its
	// radius alone, at half the floor, the pixels kept at the ratio itself (misc.w its square): the CPU sorts and cuts its
	// wood by radius (da_veg_internal.h WoodCut) - by the hash it could not split a crown's thin twigs off its thick ones.
	const float h_axis = I.tree ? 0.5f : float(shrub_hash(E.axis * 2654435761u ^ I.seed) >> 8) / 16777216.0f;
#ifdef SHRUB_SHADOW
	const float floor_mid = 0.5f * da_shrub_time.w;
	if (0.5f * (lr.y + rs.x) * I.scale < floor_mid * h_axis)
#else
	const float3 Cm = shrub_world(I, E.p + dir * (0.5f * lr.x));
	const float floor_mid = 0.5f * da_shrub_cam.w * max(dot(Cm - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
	if (0.5f * (lr.y + rs.x) * I.scale < floor_mid * (I.tree ? h_axis : h_axis * h_axis))
#endif
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const shrub_axis X = shrub_axis_load(I.axis0 + E.axis);
	// The ring: square to the joint's mean direction, its frame the axis's (the same at both sides of a joint) - within
	// d0, past it on the stems and branches (their joints' wedges opened pale rings onto the tubes' insides at thirty
	// metres) and the twigs within a dozen metres of their axis's base (their joints a bent wire's kinks at arm's length;
	// farther a twig's is under a pixel; by the axis, both sides of a joint decide alike). The shadow maps' thinner tubes
	// need none.
	shrub_ring RG;
	RG.t = dir;
	RG.p = ring != 0u ? E.p + dir * lr.x : E.p;
	RG.s = rs.y + (ring != 0u ? lr.x : 0.0f);
#ifdef SHRUB_SHADOW
	const bool share_ring = sides > 5u;
#else
	const bool share_ring = sides > 5u || (E.w.w & 0x3Fu) <= 1u
		|| dot(shrub_world(I, X.base) - da_shrub_cam.xyz, da_shrub_fwd.xyz) < 12.0f;
#endif
	[branch] if (share_ring)
		RG = shrub_ring_at(V.index, E, dir, lr.x, rs.y, ring != 0u ? rs.x : lr.y, ring != 0u, I.tree);
	const float3 t_ring = RG.t;
	const float s = RG.s;
	const float3 ref = abs(X.dir.y) < 0.9f ? cross(X.dir, float3(0.0f, 1.0f, 0.0f)) : cross(X.dir, float3(1.0f, 0.0f, 0.0f));
	const float3 u0 = normalize(ref - t_ring * dot(ref, t_ring) + float3(1e-6f, 0.0f, 0.0f)), w0 = cross(t_ring, u0);
	float sa, ca;
	sincos(6.2831853f * float(k) / float(sides), sa, ca);
	const float3 C = shrub_world(I, RG.p);
	const float3 n0 = normalize(shrub_world_nrm(I, u0 * ca + w0 * sa));
	const float3 t0 = normalize(shrub_world_dir(I, t_ring));
#ifdef SHRUB_SHADOW
	const float floor_r = 0.5f * da_shrub_time.w;
#else
	const float view_z = max(dot(C - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
	const float floor_r = 0.5f * da_shrub_cam.w * view_z;
#endif
	const float r_true = (ring != 0u ? rs.x : lr.y) * I.scale;
	// (Its level's dissolve at the end of its tier shrinks it away, da_shrubs.cpp view_bush.)
	// (The cascades' and the lights' tubes are five-sided where the view's are eight: a little thinner, so their
	// corners never stand out of the view's faces into a self-shadowing streak.)
#ifdef SHRUB_SHADOW
	const float r = max(r_true * 0.92f, floor_r) * V.grow;
#else
	const float r = max(r_true, floor_r) * V.grow;
#endif
	// (A tree's tube is round in the world: its rings' offsets go through the placement's scale as the positions do.)
	const float3 off = I.tree ? shrub_world_dir(I, normalize(u0 * ca + w0 * sa)) * (r / max(I.scale, 1e-4f)) : n0 * r;
	float3 q = C + off, n = n0, t = t0;
	const float h_rest = shrub_root_height(I, q);
	shrub_bend(q, n, t, V.slot, E.axis, s, I, false);
	[branch] if (I.tree)
		q = shrub_tree_sway(q, I, h_rest, false);

#ifdef SHRUB_SHADOW
	O.hpos = mul(m_WVP, float4(q, 1.0f));
	return O;
#else
	float3 qp = q;
	[branch] if (shrub_mv_deform(C))
	{
		float3 np = n0, tp = t0;
		qp = C + off;
		shrub_bend(qp, np, tp, V.slot, E.axis, s, I, true);
		[branch] if (I.tree)
			qp = shrub_tree_sway(qp, I, h_rest, true);
	}

	O.hpos = mul(m_WVP, float4(q, 1.0f));
	O.hpos_cur = mul(m_WVP_nojit, float4(q, 1.0f));
	O.hpos_prev = mul(m_WVP_prev, float4(qp, 1.0f));
	// The current year's growth - the last ten to forty-five centimetres of an axis, a filling shoot all of it - has the
	// young twig's colour and a smooth skin; the older wood its bark (by level, a twig was young end to end: a metre of
	// red wire). A tree's thin wood keeps the twig's skin to its species' radius (a birch's white bark on its limbs, its
	// twigs dark).
	const uint level = E.w.w & 0x3Fu;
	const float twig_r = shrub_prof(I.species, 16).w;
	const float r_tmpl = ring != 0u ? rs.x : lr.y;
	// (A dead tree's wood is all old: no young twig on it.)
	const float twig = I.tree && I.dead ? 0.0f : max(level >= 3u ? 1.0f : smoothstep(X.len - 0.45f, X.len - 0.1f, s),
		twig_r > 0.0f ? 1.0f - smoothstep(0.6f * twig_r, twig_r, r_tmpl) : 0.0f);
	// A thick grafted branch near its parent (the element's a, bit 0: da_trees.cpp pack_tree): its junction record by
	// the template's junction table (aux header's second half: the table's slot and length, the records' slot).
	shrub_join J = shrub_join_none();
	float3 jn = 0.0f;
	float jw = 1.0f;
	float4 jc0 = 0.0f, jc1 = 0.0f;
#ifndef SHRUB_WOOD_FAR
	[branch] if (I.tree && (uint(E.a * 255.0f + 0.5f) & 1u) != 0u)
	{
		const uint4 H = da_shrub_aux.Load4(I.aux0 * SHRUB_AUX_SLOT + 16u);
		const uint word = E.axis < H.y ? da_shrub_aux.Load((I.aux0 + H.x) * SHRUB_AUX_SLOT + (E.axis >> 1) * 4u) : 0u;
		const uint j = (E.axis & 1u) != 0u ? word >> 16 : word & 0xFFFFu;
		[branch] if (j != 0u && j <= H.w)
		{
			// (Two slots a record.)
			const shrub_join_rec R = shrub_join_load(I.aux0 + H.z + 2u * (j - 1u));
			shrub_join_vertex(I, R, shrub_axis_load(I.axis0 + X.parent), X.r, RG.p + normalize(u0 * ca + w0 * sa) * r_tmpl, jn, jw, jc0, jc1, J.w);
			J.light = R.light;
			J.level_byte = R.level_byte;
			J.r = R.r * I.scale;
		}
	}
#endif
	shrub_wood_outputs(O, I, q, n, t, C, float(k) / float(sides) + X.phase, s, lr.y, rs.x, r_true, floor_r, E.w.w & 0xFFu,
		float((E.w.w >> 8) & 0xFFu) / 255.0f, ring, X.r, X.len, X.base.y, X.phase, twig, C + off, J);
#ifndef SHRUB_WOOD_FAR
	O.jn = jn;
	O.jw = jw;
	O.jc0 = jc0;
	O.jc1 = jc1;
#endif
	return O;
#endif
}
#endif
