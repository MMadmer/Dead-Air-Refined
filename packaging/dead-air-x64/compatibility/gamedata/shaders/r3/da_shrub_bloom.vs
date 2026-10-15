// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural shrubs, the vertex shader of the flowers, fruits and catkins (docs/dead-air/SHRUBS.md). A bloom
// record is one quad: an open flower or a flat-topped inflorescence lies across its stalk facing up and out (seen
// from above it is a flower, from the side a sliver); a raceme, a catkin and a fruit group turn to the camera
// about their own axis. The season picks what it is: a catkin by its stage (closed through the winter, open and
// yellow before the leaves, then shed), a bloom site by its rank against the open and the fruit-set shares - a
// bud, an open flower, a withering one, then the fruit through its ripening - and it bends with its twig. A fruit
// group is laid out by its species' inflorescence (FL_*: a single fruit or a pair, a bunch, a flat corymb, an
// elder's rayed cyme, a raceme, a panicle, a spindle's capsules forking from the end of a long thin stalk); this
// shader hangs it from its stalk - heavier and lower as it grows and ripens - and passes the group's axis, its
// stalk's base and its sizes to the pixel shader, which draws every fruit and stalk of it. A broom's pod stands out
// from its site along its own quad, one to a site, as long as the species' fruit size; a false indigo's spike of
// small pods is a raceme group. A single fruit or a pod ripens on its own day round its bush's.

#include "common.h"
#include "da_velocity.h"
#include "da_shrub_common.h"

// Fruit layouts (da_shrub_bloom.ps reads the same codes).
#define FL_SINGLE	0.0f	// one fruit on its stalk, a pair on one stalk, a few fanned on their own
#define FL_BUNCH	1.0f	// a tight bunch on a short stalk (hazelnuts) or a spike crowding the twig (sea buckthorn)
#define FL_FLAT		2.0f	// a corymb's flat-topped disc (guelder rose, hawthorn)
#define FL_UMBEL	3.0f	// an elder's cyme: five rays under a broad dome
#define FL_RACEME	4.0f	// fruits on short stalks round a rachis (bird cherry, currants)
#define FL_PANICLE	5.0f	// fruits round a branched axis (red elder, lilac)
#define FL_CYME		6.0f	// capsules forking from the end of a long thin stalk (spindles)

struct v2p_shrub_bloom
{
	float4	hpos		: SV_Position;
	float4	hpos_cur	: TEXCOORD0;
	float4	hpos_prev	: TEXCOORD1;
	float4	pv_u		: TEXCOORD2;	// view-space position, x across the quad (-1..1)
	float4	n_v			: TEXCOORD3;	// quad normal (view space), y along the quad (-1..1)
	float4	t_h			: TEXCOORD4;	// quad's x direction (view space), hemi x occlusion
	float4	c0			: TEXCOORD5;	// main colour (petals, fruit, catkin; G-buffer encoding), w what: 0 flower 1 fruit 2 catkin
	float4	c1			: TEXCOORD6;	// second colour (flower centre, unripe fruit), w shape
	float4	misc		: TEXCOORD7;	// x petals or fruits, y stage (bud 0 .. open 1 .. over 2; ripeness), z hash, w aspect
	float4	fa			: TEXCOORD8;	// a fruit group's axis in the quad's frame (x across, y along, z toward the eye)
										// times its stalk's base's distance behind the quad's centre, w its layout (FL_*)
	float4	fb			: TEXCOORD9;	// x the group's radius, y a stalk's radius (quad units; a pod's too), z a shrivelled
										// fruit's size
	float4	c2			: TEXCOORD10;	// the stalks' colour (G-buffer encoding; a pod's too)
};

v2p_shrub_bloom	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_shrub_bloom O = (v2p_shrub_bloom)0;
	const shrub_vtx V = shrub_vertex(vid, iid, SHRUB_K_BLOOM);
	const uint sp = V.I.species;
	const float4 K1 = shrub_seas(sp, 1), K2 = shrub_seas(sp, 2);
	const float4 P3 = shrub_prof(sp, 3), P4 = shrub_prof(sp, 4);
	const bool catkin = (V.E.w.z & 0xFFu) == 1u;
	const float early = shrub_early(V.I, sp);
	const float rank = V.E.a - early;
	// The element's size is the larger of the species' flower and fruit (da_shrub_gen.cpp add_bloom); each draws at
	// its own (the petals' and the unripe fruit's season rows, w): a dog rose's hip is less than half its flower. Zero
	// from an engine without them.
	const float bloom_m = shrub_seas(sp, 11).w, fruit_m = shrub_seas(sp, 13).w;
	const float big = max(bloom_m, fruit_m);
	// What the site is now, and its size factor. A share of zero is none: an early bush's lowest-ranked sites stand
	// below it (rank < 0) and would otherwise flower or fruit out of season.
	float what = 0.0f, stage = 0.0f, k = 0.0f, shape = 0.0f, aspect = 1.0f;
	float layout = FL_SINGLE, rg = 0.4f, reach = 0.0f, hang = 0.0f, own = 1.0f;
	bool group = false;
	// The fruits on the site's group (the pixel shader drops the rest one by one); a site of one fruit (a hip, a drupe,
	// a pod) loses it whole: its share of the sites drops instead.
	const float keep = shrub_fruit_keep(sp, V.I.seed);
	const float fruit_sites = K1.z * (P3.w < 1.5f ? keep : 1.0f);
	if (catkin)
	{
		// Catkins: formed in late summer, closed through the winter, long and open before the leaves, then shed.
		const float C = K2.x;
		k = saturate(C * 2.0f - 0.5f) * saturate((3.0f - C) * 4.0f) * saturate((K2.x + 0.3f - rank) * 4.0f);
		what = 2.0f;
		stage = saturate(C - 1.0f);
		shape = P4.x;
		aspect = lerp(2.5f, 6.0f, K2.y);
	}
	else if (rank < fruit_sites && fruit_sites > 0.0f && P3.z > 0.5f)
	{
		// A fruit: from set to ripe to shrivelled. Its ripeness runs 0 .. 0.7 while it grows (full size by 0.6) and
		// 0.7 .. 1 while it colours (tools/shrubs/make_shrubs.py season_keys: green until its last weeks).
		what = 1.0f;
		stage = K1.w;
		k = saturate((fruit_sites - rank) * 30.0f) * lerp(0.45f, 1.0f, saturate(K1.w * 1.6667f));
		shape = P3.z;
		own = big > 0.0f ? fruit_m / big : 1.0f;
		// A broom's pod is slender - one to a site, six times as long as wide, as long as the fruit size (the quad
		// sized to it below) -, a single cone an egg; a willow's cotton is a round tuft. More pods are a raceme's (a
		// false indigo's spike), a group.
		if (shape > 7.5f && shape < 8.5f && P3.w < 2.5f)
			aspect = 3.4f;
		else if (shape > 6.5f && shape < 7.5f && P3.w < 1.5f)
			aspect = 1.45f;
		else if (shape < 8.5f || shape > 9.5f)
		{
			// A group of fruits laid out by the inflorescence it came from. Its radius, its stalk's base behind the
			// quad's centre (quad units) and how far it hangs (0 along its site's direction .. 1 down): a fruit
			// grows heavier and its group hangs lower as it fills and ripens.
			group = true;
			const float form = P3.x, n = P3.w;
			const bool caps = shape > 5.5f && shape < 6.5f, pods = shape > 7.5f && shape < 8.5f;
			const float fill = saturate(stage * 1.6667f), ripe = smoothstep(0.6f, 1.0f, stage);
			if (form > 3.5f && form < 4.5f)
			{
				// A false indigo's spike of pods stands as the capsules do; berries hang lower as they fill.
				layout = FL_RACEME;
				aspect = 2.0f;
				reach = 0.85f;
				hang = caps || pods ? 0.1f : 0.45f + 0.35f * fill;
			}
			else if (form > 4.5f && form < 5.5f)
			{
				// A lilac's capsules stand; a red elder's berries nod once ripe.
				layout = FL_PANICLE;
				aspect = caps ? 1.8f : 1.6f;
				reach = 0.85f;
				hang = caps ? 0.0f : 0.1f + 0.35f * ripe;
			}
			else if ((form > 1.5f && form < 2.5f) || (form > 7.5f && form < 8.5f))
			{
				// A corymb's (or a lacecap's) berries in a flat-topped disc; a spindle's capsules fork from the end
				// of a long thin stalk hanging from the twig (the group's breadth the fruit size, its stalk 1.3 times
				// that: the stalk leaves the site near the quad's edge).
				layout = caps ? FL_CYME : FL_FLAT;
				rg = caps ? 0.467f : 0.78f;
				reach = caps ? 0.95f : 0.3f;
				hang = caps ? 0.85f : 0.3f + 0.35f * fill;
			}
			else if (form > 2.5f && form < 3.5f)
			{
				// An elder's cyme turns over under its ripe berries.
				layout = FL_UMBEL;
				rg = 0.78f;
				reach = 0.3f;
				hang = 0.1f + 0.75f * smoothstep(0.55f, 1.0f, stage);
			}
			else if (form < 0.5f && n > 1.5f)
			{
				// Hazelnuts on a short common stalk, pointing out; sea buckthorn's berries along their spur.
				layout = FL_BUNCH;
				rg = 0.42f;
				reach = shape > 4.5f && shape < 5.5f ? 0.55f : 0.45f;
				hang = shape > 4.5f && shape < 5.5f ? 0.15f : 0.0f;
			}
			else
			{
				// One fruit on its stalk (sessile without a flower shape: a juniper's), a pair, a few fanned.
				layout = FL_SINGLE;
				rg = 0.4f;
				reach = form < 0.5f ? 0.42f : 0.61f;
				hang = n > 1.5f ? 0.45f : 0.3f + 0.3f * fill;
			}
		}
	}
	else if (rank < K1.x && K1.x > 0.0f && P3.x > 0.5f)
	{
		// A flower: a bud opening, open, then withering and shrinking.
		what = 0.0f;
		stage = K1.y;
		k = saturate((K1.x - rank) * 30.0f) * lerp(0.4f, 1.0f, saturate(K1.y)) * lerp(1.0f, 0.6f, saturate(K1.y - 1.0f));
		shape = P3.x;
		own = big > 0.0f ? bloom_m / big : 1.0f;
		aspect = (shape > 3.5f && shape < 5.5f) ? 2.2f : 1.0f; // racemes and panicles are long
	}
	k *= V.grow;
	// A young tree is still in its juvenile phase: a sapling bears none of its flowers, fruit and cones, a tree growing
	// up to the policy's heights a share of them (its youth).
	if (V.I.tree && frac(V.hash * 5.31f) < V.I.youth)
		k = 0.0f;
	if (!V.valid || V.I.dead || k <= 0.0f)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const shrub_elem E = V.E;
	const shrub_inst I = V.I;
	const float3 dir = normalize(shrub_world_dir(I, shrub_oct(E.w.x)));
	const float2 ss = shrub_h2(E.w.y);
	// A fruit group's species size is the group's own outline (a single fruit's length, a cluster's breadth, a
	// raceme's or a panicle's length); its quad holds the stalks round it too. A long quad's unit is its half-length:
	// a pod's quad is 1.2 times its length (its own stalk and its lean round it).
	const bool pod = what == 1.0f && shape > 7.5f && shape < 8.5f && !group;
	const float qf = pod ? 1.2f / sqrt(aspect) : (!group ? 1.0f : (aspect > 1.5f ? 0.85f : 1.0f / rg));
	const float size = ss.x * I.scale * k * own * qf;
	const float3 site = shrub_world(I, E.p);
	const float3 to_eye = normalize(da_shrub_cam.xyz - site);
	// A fruit group's axis: its site's direction pulled down and out by its weight.
	float3 gaxis = dir;
	if (group)
	{
		const float3 out_h = normalize(float3(dir.x, 0.0f, dir.z) + float3(1e-4f, 0.0f, 0.0f));
		gaxis = normalize(dir * (1.0f - hang) + (float3(0.0f, -0.8f, 0.0f) + out_h * 0.6f) * hang);
	}
	// The quad's frame: a flat flower or inflorescence across its stalk (facing up and out), the rest round
	// their axis to the camera. A fruit group faces the camera whole (the pixel shader lays it out in depth); a long
	// one's quad follows its axis as seen, so a raceme seen along its axis is a round bunch, not an edge-on quad.
	const bool flat = what == 0.0f && aspect < 1.5f;
	const bool along = !group && (what == 2.0f || aspect > 1.5f || pod);
	float3 axis_y, axis_x, nrm;
	if (flat)
	{
		nrm = normalize(dir + float3(0.0f, 0.6f, 0.0f));
		axis_x = normalize(cross(nrm, abs(nrm.y) < 0.95f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f)));
		axis_y = cross(nrm, axis_x);
	}
	else
	{
		const float3 upw = float3(0.0f, 1.0f, 0.0f);
		const float3 seen = gaxis - to_eye * dot(gaxis, to_eye);
		axis_y = along ? gaxis : (group && aspect > 1.5f && dot(seen, seen) > 1e-6f ? normalize(seen)
			: normalize(upw - to_eye * dot(upw, to_eye) + float3(1e-5f, 0.0f, 0.0f)));
		axis_x = normalize(cross(axis_y, to_eye) + float3(1e-5f, 0.0f, 0.0f));
		nrm = normalize(cross(axis_x, axis_y));
	}
	const float cx = (V.corner & 1u) != 0u ? 1.0f : -1.0f, cy = V.corner >= 2u ? 1.0f : -1.0f;
	// A catkin and a raceme hang from (or stand on) their stalk: the quad starts at the site. A fruit group's stalk
	// leaves its site `reach` quad units behind the quad's centre along the group's axis.
	float hx = 0.5f * size / sqrt(aspect), hy = 0.5f * size * sqrt(aspect);
	// A catkin and a flower's raceme or panicle are as long as their size (a closed catkin under half its full length),
	// as narrow as their aspect makes them: stretched by the aspect's root, they came out half again to twice their
	// length (a hazel catkin 13 cm).
	if (what == 2.0f || (what == 0.0f && aspect > 1.5f))
	{
		hy = 0.5f * size * (what == 2.0f ? lerp(0.4f, 1.0f, saturate(K2.y)) : 1.0f);
		hx = hy / aspect;
	}
	float3 centre = site + dir * (0.3f * size);
	if (along)
		centre = site + axis_y * hy;
	else if (group)
		centre = site + gaxis * (reach * 0.5f * size * sqrt(aspect));
	const float3 off = axis_x * (cx * hx) + axis_y * (cy * hy);
	float3 q = centre + off, n = nrm, t = axis_x;
	const float h_rest = shrub_root_height(I, q);
	shrub_bend(q, n, t, V.slot, E.axis, ss.y, I, false);
	[branch] if (I.tree)
		q = shrub_tree_sway(q, I, h_rest, false);
	float3 qp = q;
	[branch] if (shrub_mv_deform(centre))
	{
		float3 np = nrm, tp = axis_x;
		qp = centre + off;
		shrub_bend(qp, np, tp, V.slot, E.axis, ss.y, I, true);
		[branch] if (I.tree)
			qp = shrub_tree_sway(qp, I, h_rest, true);
	}

	O.hpos = mul(m_WVP, float4(q, 1.0f));
	O.hpos_cur = mul(m_WVP_nojit, float4(q, 1.0f));
	O.hpos_prev = mul(m_WVP_prev, float4(qp, 1.0f));
	O.pv_u = float4(mul(m_WV, float4(q, 1.0f)).xyz, cx);
	O.n_v = float4(mul((float3x3)m_WV, n), cy);
	// A single fruit or a pod ripens on its own day round the bush's (a raspberry cane in July has green, turning and
	// ripe berries, a broom green and darkening pods); a bunch's berries colour one by one in the pixel shader.
	if (what == 1.0f && (pod || (group && layout < 0.5f)))
		stage = max(stage + (V.hash - 0.5f) * (pod ? 0.4f : 0.3f), 0.0f);
	// The light inside the crown where the flower or fruit sits, as the leaves round it have it (da_shrub_leaf_vs.h).
	const float3 leaf_mean = shrub_leaf_colour_mean(sp);
	const float4 cl = shrub_crown_light(float(E.w.w & 0xFFu) / 255.0f, sp, I.dead, leaf_mean);
	O.t_h = float4(mul((float3x3)m_WV, t), saturate(I.hemi) * cl.x);
	// Colours: petals and centre, the fruit unripe to ripe to shrivelled, the catkin closed to open.
	float3 c0, c1;
	if (what == 2.0f)
	{
		const float3 open = shrub_seas(sp, 15).rgb;
		c0 = lerp(lerp(open, shrub_seas(sp, 9).rgb, 0.55f) * 0.8f, open, stage);
		c1 = c0 * 0.7f;
	}
	else if (what == 1.0f)
	{
		const float3 unripe = shrub_seas(sp, 13).rgb, ripe = shrub_seas(sp, 14).rgb;
		c0 = lerp(unripe, ripe, smoothstep(0.7f, 1.0f, stage));
		c0 = lerp(c0, c0 * float3(0.55f, 0.45f, 0.4f), saturate(stage - 1.0f));
		c1 = unripe;
		// A hazelnut: its husk (c1) leafy - the leaves' fresh green, the young flush's half way to the summer's,
		// browning after the nut ripens; the nut (c0) a young shell's pale cream-green until it turns (the generic
		// unripe green is the husk's look, and an olive nut read as an olive).
		if (shape > 4.5f && shape < 5.5f)
		{
			c1 = lerp(lerp(leaf_mean, shrub_seas(sp, 3).rgb, 0.45f) * 1.08f, lerp(leaf_mean, ripe, 0.6f) * 0.7f,
				saturate((stage - 0.95f) * 3.0f));
			c0 = lerp(lerp(unripe, float3(0.61f, 0.64f, 0.35f), 0.7f), ripe, smoothstep(0.7f, 1.0f, stage));
		}
	}
	else
	{
		const float3 petal = shrub_seas(sp, 11).rgb;
		c0 = lerp(lerp(shrub_seas(sp, 10).rgb, petal, 0.4f), petal, saturate(stage));
		c0 = lerp(c0, c0 * float3(0.7f, 0.6f, 0.45f), saturate(stage - 1.0f));
		c1 = shrub_seas(sp, 12).rgb;
	}
	// Snow on a flower's or a catkin's upper side. Not on a fruit group or a pod: their quad turns to the eye, and from
	// above every one of them read as snowed over, the haws under it pink-white (snow slides off a round fruit).
	const float snow = what == 1.0f ? 0.0f : da_shrub_sun.w * saturate(n.y);
	c0 = lerp(c0, 0.82f, snow * 0.6f);
	// Past the near cascade, the self-shadow the far shadow maps lose (as the leaves round it).
	const float far_k = shrub_far_shade(float(E.w.w & 0xFFu) / 255.0f, max(dot(site - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f));
	O.c0 = float4(shrub_to_albedo(c0 * cl.yzw * far_k), what);
	O.c1 = float4(shrub_to_albedo(c1 * cl.yzw * far_k), shape);
	O.misc = float4(what == 1.0f ? P3.w : P3.y, stage, V.hash, aspect);
	if (group || pod)
	{
		// The group's axis in the quad's own frame (the pixel shader lays the fruits out in it), scaled by the stalk
		// base's distance behind the quad's centre.
		const float3 a_q = float3(dot(gaxis, axis_x), dot(gaxis, axis_y), dot(gaxis, nrm));
		O.fa = float4(a_q * reach, group ? layout : -1.0f);
		// A pedicel is about a millimetre thick; a shrivelled fruit (ripeness past 1) shrinks by an eighth.
		const float unit = 0.5f * size * sqrt(aspect);
		O.fb = float4(rg, clamp(0.0005f / max(unit, 1e-4f), 0.004f, 0.06f), 1.0f - 0.12f * saturate((stage - 1.0f) * 2.5f),
			group ? keep : 1.0f);
		// The stalks: green as the young twig and the leaves round them, taking some of the ripe fruit's colour (an
		// elder's and a guelder rose's redden with their berries).
		const float3 stalk = lerp(lerp(shrub_seas(sp, 10).rgb, leaf_mean, 0.6f), shrub_seas(sp, 14).rgb * 0.6f,
			0.4f * smoothstep(0.7f, 1.0f, stage));
		O.c2 = float4(shrub_to_albedo(stalk * cl.yzw), 0.0f);
	}
	else
		O.fa = float4(0.0f, 0.0f, 0.0f, -1.0f);
	return O;
}
