// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural shrubs, the vertex shader of the middle and far distance (docs/dead-air/SHRUBS.md): a cluster (the
// leaves of a stretch of a shoot) or a blob (a part of the crown) is one quad turned to the camera (to the sun
// in the cascades), as wide as the part, its leaves drawn by da_shrub_mass.h. It bends with its shoot (a blob with
// the first stem at its height); the pixel floor's survivors grow to keep the area. The season's share of the
// leaves on, the colour wave's spread and the blooms of the part go to the pixel shader, which draws its leaves
// by the same rules as the near ones. SHRUB_SHADOW: the sun cascades.

#include "common.h"
#include "da_velocity.h"
#include "da_shrub_common.h"
#ifdef SHRUB_SHADOW
#include "da_shrub_mass.h"
#endif

struct v2p_shrub_mass
{
	float4	hpos		: SV_Position;
#ifndef SHRUB_SHADOW
	float4	hpos_cur	: TEXCOORD0;
	float4	hpos_prev	: TEXCOORD1;
	float4	pv_x		: TEXCOORD2;	// view-space position, x across the quad (-1..1)
	float4	n_y			: TEXCOORD3;	// the part's normal (view space), the leaves' clump level (continuous: cells of 4^w)
	float4	t_h			: TEXCOORD4;	// the quad's x direction (view space), hemi x occlusion
#endif
	float4	part		: TEXCOORD5;	// x species, y rank, z fall, w light
	float4	dens		: TEXCOORD6;	// x leaf cover (Beer-Lambert of its area), y cells across, z the cells' share in
										// flower or fruit, w hash
	float4	misc		: TEXCOORD7;	// x the plant's state (shrub_mass_state), y its phenology offset, z blob (0/1), w size on
										// screen (px)
	float4	xy_t		: TEXCOORD8;	// x, y across and along the quad (-1..1), z translucency where the light comes through
										// (the shadow maps: the share of the part's leaves on), w a leaf's width over its
										// length
};

v2p_shrub_mass	main	(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p_shrub_mass O = (v2p_shrub_mass)0;
	const uint kind = uint(da_shrub_draw.y + 0.5f);
	const shrub_vtx V = shrub_vertex(vid, iid, kind);
	if (!V.valid)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const shrub_elem E = V.E;
	const shrub_inst I = V.I;
	const bool blob = kind == SHRUB_K_BLOB;
	const float2 ra = shrub_h2(E.w.y);
	const float k_l = shrub_prof(I.species, 0).x;
	const bool needles = k_l > 1.5f && k_l < 2.5f;
	// (A needle brush's part reaches about a tuft's length past its tufts' bases, the spread the generator measures: a
	// tenth more.)
	const float brush = needles ? 1.1f : 1.0f;
#ifndef SHRUB_SHADOW
	// (A quad's corner reaches its radius times the square root of two.)
	if (shrub_out_of_view(I, shrub_world(I, E.p), 1.42f * ra.x * brush * I.scale * V.grow))
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
#endif
#ifdef SHRUB_SHADOW
	// A blob casts at its clusters' own spread (the view's is wider so neighbours overlap, da_shrub_gen.cpp blobs) and
	// that spread's cover: the cascades' quads stay the pixels they were.
	const float spread = blob ? 1.0f / SHRUB_BLOB_GROW : 1.0f;
#else
	const float spread = 1.0f;
#endif
	const float r = ra.x * brush * I.scale * V.grow * spread;
	const float light = blob ? float((E.w.z >> 8) & 0xFFu) / 255.0f : float((E.w.z >> 16) & 0xFFu) / 255.0f;
	const float s_axis = blob ? f16tof32(E.w.w & 0xFFFFu) : f16tof32(E.w.z & 0xFFFFu);
#ifdef SHRUB_SHADOW
	// (A blob casts from where its clusters are: the view draws it wider and drawn in along its crown normal by the
	// record's w high half.)
	const float3 c = shrub_world(I, blob ? E.p + shrub_elem_dir(E, I.tree) * f16tof32(E.w.w >> 16) : E.p);
#else
	const float3 c = shrub_world(I, E.p);
#endif
#ifdef SHRUB_SHADOW
	const float3 to_eye = da_shrub_sun.xyz;
#else
	const float3 to_eye = normalize(da_shrub_cam.xyz - c);
#endif
	const float3 upw = float3(0.0f, 1.0f, 0.0f);
	const float3 ay = normalize(upw - to_eye * dot(upw, to_eye) + float3(1e-5f, 0.0f, 0.0f));
	const float3 ax = normalize(cross(ay, to_eye));
	const float cx = (V.corner & 1u) != 0u ? 1.0f : -1.0f, cy = V.corner >= 2u ? 1.0f : -1.0f;
	const float3 off = (ax * cx + ay * cy) * r;
	const float3 nrm = normalize(shrub_world_nrm(I, shrub_elem_dir(E, I.tree)));
	float3 q = c + off, n = nrm, t = ax;
	const float h_rest = shrub_root_height(I, q);
	shrub_bend(q, n, t, V.slot, E.axis, s_axis, I, false);
	[branch] if (I.tree)
		q = shrub_tree_sway(q, I, h_rest, false);
	// The leaves' cover of the quad: Beer-Lambert of their area over the quad's disc (random leaves overlap: a
	// sum of the areas overstates it), the survivors' growth folded in.
	const float disc = 3.14159f * (ra.x * brush * I.scale) * (ra.x * brush * I.scale);
	// (A needle brush's tufts stand turned to the eye near, two thirds of their area facing it - measured against the
	// near tier at 14 and 40 m: with their whole area the far junipers read a third denser.)
	const float proj = needles ? 0.65f : 0.5f;
	float cover = min((1.0f - exp(-proj * ra.y * I.scale * I.scale / max(disc, 1e-4f))) / (spread * spread), 0.97f);
	// A chunk keeps a share of its parts past the pixel floor (V.thin, its outer ones: the parts go by importance): the
	// survivors' cover makes the light through the crown's stacked layers up (Beer-Lambert) - a crown's clusters stood
	// ten deep in the middle cascade's texels and three and four deep in a far crown's pixels, each shaded as often.
	cover = 1.0f - pow(max(1.0f - cover, 0.03f), 1.0f / max(V.thin, 0.05f));
	const float leaves = blob ? 60.0f : float(E.w.w & 0xFFFFu);
	const float bloom_share = blob ? float(E.w.z & 0xFFu) / 255.0f : float((E.w.z >> 24) & 0xFFu) / max(leaves + float((E.w.z >> 24) & 0xFFu), 1.0f);
	// The cells are the species' own leaves (leaflets): a cluster's mean leaf from its area over its leaves (the
	// generator counts a spray's leaves and a compound leaf's leaflets), a blob's the species' mean element over its
	// leaves or leaflets (one leaf texture a cell), a cell three quarters of its length so neighbours overlap. (A blob a
	// dozen across drew leaves several times their size: flakes, coins and confetti on the far crowns' edges.)
	const float ratio = clamp(shrub_prof(I.species, 7).y, 0.08f, 1.0f);
	const float e_area = blob ? shrub_prof(I.species, 16).x / max(shrub_prof(I.species, 0).z, 1.0f) : max(ra.y, 1e-6f) / max(leaves, 1.0f);
	const float leaf_len = sqrt(max(e_area, 1e-6f) / (0.67f * ratio)) * I.scale;
	const float cells = blob && e_area <= 0.0f ? 12.0f : clamp(2.0f * ra.x * I.scale / max(0.75f * leaf_len, 1e-3f), 3.0f, blob ? 48.0f : 36.0f);
	O.part = float4(float(I.species), E.a, E.b, light);
	// (The share of the cells that flower or fruit now: a fruit cell at the share of its group's fruits the bush carries,
	// as the near tier draws them.)
	const float4 K1 = shrub_seas(I.species, 1);
	O.dens = float4(cover, cells, bloom_share * max(K1.x, K1.z * 0.7f * shrub_fruit_keep(I.species, I.seed)), V.hash);
	// (The plant's state for da_shrub_mass.h - alive, dead, a Red Forest pine's red needles, its marcescent share and
	// spring lead -, its phenology offset.)
	O.misc = float4(shrub_mass_state(I, I.species), I.pheno, blob ? 1.0f : 0.0f, 0.0f);
	O.xy_t = float4(cx, cy, shrub_translucency(shrub_prof(I.species, 1).x, to_eye), ratio);

#ifdef SHRUB_SHADOW
	// (Its size in the map's texels: da_shrub_cluster_shadow.ps casts its mean blot where its leaves are under one. The
	// share of its leaves on the same over the quad: worked out here, not every texel.)
	O.misc.w = 2.0f * r / max(da_shrub_time.w, 1e-5f);
	O.xy_t.z = shrub_mass_on_share(O.part, O.misc);
	O.hpos = mul(m_WVP, float4(q, 1.0f));
	return O;
#else
	float3 qp = q;
	[branch] if (shrub_mv_deform(c))
	{
		float3 np = nrm, tp = ax;
		qp = c + off;
		shrub_bend(qp, np, tp, V.slot, E.axis, s_axis, I, true);
		[branch] if (I.tree)
			qp = shrub_tree_sway(qp, I, h_rest, true);
	}
	O.hpos = mul(m_WVP, float4(q, 1.0f));
	O.hpos_cur = mul(m_WVP_nojit, float4(q, 1.0f));
	O.hpos_prev = mul(m_WVP_prev, float4(qp, 1.0f));
	O.pv_x = float4(mul(m_WV, float4(q, 1.0f)).xyz, cx);
	// The light inside the crown, as the near leaves have it (da_shrub_leaf_vs.h; the pixel shader tints its leaves).
	const float3 leaf_c = I.red ? shrub_seas(I.species, 7).rgb
		: shrub_leaf_colour_k(I.species, shrub_seas_lead(I.species, I.lead), E.a, E.b, light, V.hash, 0.0f).rgb;
	// (The innermost parts at least a third of the lit ones, a blob's at nearly half: near-black discs inside the far
	// crowns.)
	O.t_h = float4(mul((float3x3)m_WV, t), saturate(I.hemi) * max(shrub_crown_light(light, I.species, I.dead, leaf_c).x,
		blob ? 0.45f : 0.35f));
	const float view_z = max(dot(c - da_shrub_cam.xyz, da_shrub_fwd.xyz), 0.05f);
	O.misc.w = 2.0f * r / max(da_shrub_cam.w * view_z, 1e-5f);
	// Leaves under three pixels gather into clumps of four, sixteen or sixty-four - fewer, larger cells at the same
	// cover (da_shrub_mass.h): the continuous level, each part's own hash moving its switches, for the pixel shader to
	// dither round each switch (dens.y keeps the leaves' own cells).
	O.n_y = float4(mul((float3x3)m_WV, n), log2(3.0f * cells / max(O.misc.w, 1e-3f)) + (V.hash - 0.5f) * 0.6f);
	return O;
#endif
}
