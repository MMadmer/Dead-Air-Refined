// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Blade grass, the vertex shader of leaves and culms (docs/dead-air/GRASS.md): no vertex streams - the
// strip vertex comes from SV_VertexID (32 blades per instance, merge-instancing), the blade from the bin's
// window of the record buffer. Leaves are flat ribbons drawn as screen-facing strips of their projected
// width (never thinner than k_px pixels: the generation thinned them so coverage stays exact), culms are
// screen-facing cylinders; the previous frame's shape gives exact motion vectors with the same offsets.
// GRASS_SHADOW: the sun cascades - the strip faces the light, at least one shadow texel wide, no velocity.

#include "common.h"
#include "da_velocity.h"
#include "da_grass_shape.h"

#ifdef GRASS_SHADOW
struct v2p_grass_shadow
{
	float4	hpos	: SV_Position;
};
#define GRASS_OUT	v2p_grass_shadow
#else
#define GRASS_OUT	v2p_grass
#endif

GRASS_OUT	main	(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
	GRASS_OUT O = (GRASS_OUT)0;
	const uint V = uint(da_grass_draw.z);
	const uint blade_i = inst * 32u + vid / V;
	const uint v = vid % V;
	const uint bin = uint(da_grass_draw.y);
	// The leftovers of the last instance and the void records of a lane that did not fit its bin (zero
	// shape): every vertex on one point, the rasteriser drops them.
	if (blade_i >= da_grass_counts.Load((GRASS_NBINS + bin) * 4u))
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}

	const grass_blade B = grass_load(uint(da_grass_draw.x) + blade_i);
	if (B.cur.height <= 0.0f)
	{
		O.hpos = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return O;
	}
	const uint p = grass_profile(B);
	const bool culm = grass_is_culm(B);
	const bool litter = grass_is_litter(B);
	const bool folded = (B.flags & GRASS_F_FOLD) != 0u && !culm;
	const uint lod = uint(da_grass_draw.w);
	const uint pairs = (V - 1u) >> 1;
	const float hash = grass_hash01(B);
	const float4 P7 = grass_prof(p, 7), P8 = grass_prof(p, 8), P12 = grass_prof(p, 12), P14 = grass_prof(p, 14);
	const float4 P19 = grass_prof(p, 19);
	const uint form = uint(P19.z + 0.5f);
	const float knee = culm ? P7.w : 0.0f;

	float t0, side, rows_lo;
	uint which;
	grass_decode(v, pairs, folded, t0, side, which, rows_lo);
	// The two leaves of a folded strip split by the fold yaw; a basal pair fans wider, a litter pair lies
	// splayed.
	const float fan = litter ? 3.0f * P8.x : ((B.flags & GRASS_F_BASAL) != 0u ? 1.7f * P8.x : P8.x);
	const float yaw = folded ? (which == 0u ? -fan : fan) : 0.0f;

	const float3 cam = da_grass_vcam.xyz;
	const float d = distance(B.root, cam);
	// The morph towards the next coarser LOD in the last fifth before its switch distance.
	float morph = 0.0f;
#ifndef GRASS_SHADOW
	if (lod == 0u)
		morph = smoothstep(0.8f * da_grass_vlod.x, da_grass_vlod.x, d);
	else if (lod == 1u)
		morph = smoothstep(0.8f * da_grass_vlod.y, da_grass_vlod.y, d);
#endif
	// The bob, not in the middle cascade (large-scale wind only there).
	const float2 bob = (lod == 4u) ? float2(0.0f, 0.0f) : P12.zw;

	float3 C, dC, S;
	grass_curve_lod(B.cur, B.root, t0, culm, rows_lo, morph, yaw, bob, da_grass_vlod.z, hash, knee, C, dC, S);
	const float t = grass_t(t0, culm);
	const float3 Tn = normalize(dC);

	// Litter is grass straw whatever the species' leaf; basal pairs are grass leaves. A cut leaf (pinnate,
	// dissected) has its lobes cut out in the short bin's pixel shader; elsewhere (the leaves past d1, the
	// shadows) it is drawn as narrow as their mean cover.
	const uint outline = (culm || litter || (B.flags & GRASS_F_BASAL) != 0u) ? GRASS_LEAF_GRASS : uint(P19.y + 0.5f);
	const bool cut = outline >= GRASS_LEAF_PINNATE;
	const float cut_w = (cut && bin != GRASS_BIN_SHORT) ? GRASS_CUT_COVER : 1.0f;

	// Width along the blade: leaves taper (the A blade of a folded pair has a flat top), culms barely do;
	// the far culm is a triangle from the root to a top as wide as the culm and its folded head. In the near sun
	// cascade a forb's stem turns into it halfway to d1, its leaves folded in (a dozen casters a stem); its head
	// stays in the cascade's head bin there, so the triangle takes the head's share only when the head pass
	// does not draw it (no room in its bin).
	float width;
	const bool forb_stem = culm && form != GRASS_FORM_GRAMINOID;
	const bool far_culm = culm && (lod == 2u || lod == 4u);
	const float head_k = (forb_stem && lod == 3u && (B.flags & GRASS_F_HEAD) != 0u) ? 0.0f : 1.0f;
	if (culm)
	{
		const float head_part = 2.0f * B.head_area * (B.cur.width + 0.5f * B.head_eq);
		const float w_top = 2.0f * B.cur.width + max(B.head_eq - head_part * (1.0f - head_k), 0.0f);
		const float w_far = w_top * t;
		float m12 = (lod == 1u) ? morph : 0.0f;
		if (forb_stem && lod == 3u)
			m12 = smoothstep(0.4f * da_grass_vlod.x, 0.5f * da_grass_vlod.x, d);
		width = far_culm ? w_far : lerp(B.cur.width * lerp(1.0f, 0.7f, t), w_far, m12);
	}
	else
	{
		const float rows_a = float(pairs >> 1);
		const float tw = (folded && which == 0u) ? t * rows_a / (rows_a + 1.0f) : t;
		width = B.cur.width * grass_leaf_width(tw, outline) * cut_w;
	}

#ifdef GRASS_SHADOW
	// Facing the light, at least one texel of the cascade.
	const float3 L = da_grass_vsun.xyz;
	const float3 R = normalize(cross(Tn, L) + float3(1e-6f, 0.0f, 0.0f));
	const float w_proj = culm ? width : width * lerp(0.25f, 1.0f, sqrt(saturate(1.0f - dot(S, L) * dot(S, L))));
	const float hw = 0.5f * max(w_proj, da_grass_vsun.w);
	O.hpos = mul(m_WVP, float4(C + R * (side * hw), 1.0f));
	return O;
#else
	const float3 Vw = normalize(cam - C);
	const float3 R = normalize(cross(Tn, Vw) + float3(1e-6f, 0.0f, 0.0f));
	const float view_z = max(dot(C - cam, da_grass_vfwd.xyz), 0.05f);
	// Flat leaves show their projected width (edge-on thickened, GoT); never below the pixel floor.
	const float w_proj = culm ? width : width * lerp(0.25f, 1.0f, sqrt(saturate(1.0f - dot(S, Vw) * dot(S, Vw))));
	const float hw = 0.5f * max(w_proj, side != 0.0f ? da_grass_vcam.w * view_z : 0.0f);
	const float3 off = R * (side * hw);
	const float3 pos = C + off;

	// The same blade on the previous frame: its shape, its root, the same offset.
	float3 Cp = C;
	[branch] if (da_grass_vmisc.y > 0.5f)
	{
		float3 dCp, Sp;
		grass_curve_lod(B.prev, B.root_prev, t0, culm, rows_lo, morph, yaw, bob, da_grass_vlod.w, hash, knee, Cp, dCp, Sp);
	}
	const float3 pos_prev = Cp + off;

	O.hpos = mul(m_WVP, float4(pos, 1.0f));
	O.hpos_cur = mul(m_WVP_nojit, float4(pos, 1.0f));
	O.hpos_prev = mul(m_WVP_prev, float4(pos_prev, 1.0f));
	O.pv_t = float4(mul(m_WV, float4(pos, 1.0f)).xyz, t);

	// Normals: a flat leaf's face (the pixel shader turns it to the viewer) rounded across the width
	// (GoT), a culm's cylinder; the canopy normal takes over with distance (specular anti-aliasing).
	float3 nb, lat;
	float round_a;
	if (culm)
	{
		nb = normalize(Vw - Tn * dot(Vw, Tn));
		round_a = radians(lerp(60.0f, 20.0f, saturate(d / da_grass_vlod.y)));
		lat = R * side;
	}
	else
	{
		nb = normalize(cross(dC, S) + float3(0.0f, 1e-6f, 0.0f));
		round_a = radians(lerp(30.0f, 10.0f, saturate(d / da_grass_vlod.y)));
		lat = S * (side * (dot(R, S) >= 0.0f ? 1.0f : -1.0f));
	}
	float sr, cr;
	sincos(round_a, sr, cr);
	O.nb_u = float4(mul((float3x3)m_WV, nb * cr), side * 0.5f + 0.5f);
	O.nc = float4(mul((float3x3)m_WV, grass_canopy_normal(B, cam - B.root)), grass_canopy_weight(d));

	// Colour along the blade; the far culm's top carries its folded head so the mean stays right (never
	// lighter than the lighter of the culm and its head: a bright top stood out as a field of candles).
	float3 albedo = grass_colour(B, p, t, normalize(nb + float3(0.0f, 0.5f, 0.0f)));
	if (culm && B.head_area > 0.0f && (far_culm || (lod == 1u && morph > 0.0f)))
	{
		const float3 c_head = grass_head_colour(B, p, B.stage);
		const float3 c_root = grass_colour(B, p, 0.0f, float3(0.0f, 1.0f, 0.0f));
		const float3 c_mid = grass_colour(B, p, 0.5f, float3(0.0f, 1.0f, 0.0f));
		const float3 c_mean = lerp(c_mid, c_head, B.head_area);
		const float3 c_top = min(saturate((3.0f * c_mean - c_root) * 0.5f), max(c_head, c_mid));
		const float k = far_culm ? 1.0f : morph;
		albedo = lerp(albedo, lerp(c_root, c_top, t), k);
	}
	// A dry culm is a thin, hollow, weathered stem: buff-grey and a little darker than the flat straw of a
	// leaf, so a stand of dead culms reads as stems and not as a field of yellow sticks.
	//
	// A culm much thinner than the pixel floor (da_grass_vcam.w per metre of view depth) stands for a stem whose
	// pixel mostly holds the stand round it: drawn at the floor in its own open, pale tone, the culms of the
	// middle distance read as thin pale strokes over the canopy. Its colour and its sky ease up to halfway to
	// those of the leaves round it (their colour and occlusion halfway up the canopy) as the stem's share of
	// its drawn width falls.
	float culm_sub = 0.0f;
	if (culm)
	{
		const float dry = (B.flags & GRASS_F_THATCH) != 0u ? 1.0f : smoothstep(0.3f, 0.8f, B.sen);
		const float y_c = dot(albedo, float3(0.2126f, 0.7152f, 0.0722f));
		albedo = lerp(albedo, lerp(y_c, albedo, 0.75f) * 0.88f, dry);
		// A species' own stem colour (the purple-brown of mugwort, the red of field wormwood), half of it left
		// on the dead stem.
		albedo *= lerp(grass_prof(p, 26).rgb, 1.0f, 0.5f * dry);
		const float4 P1c = grass_prof(p, 1);
		const float floor_w = da_grass_vcam.w * max(dot(C - cam, da_grass_vfwd.xyz), 0.05f);
		culm_sub = 0.5f * smoothstep(0.5f, 1.0f, 1.0f - 0.5f * (P1c.z + P1c.w) / max(floor_w, 1e-5f));
		albedo = lerp(albedo, grass_colour(B, p, 0.5f, float3(0.0f, 1.0f, 0.0f)), culm_sub);
	}
	// Occlusion by the height above the ground in canopy heights; litter lies at the foot of the stand, in its
	// shade but not in the dark of the tussocks' bases.
	const float y_rel = litter ? 0.25f : (C.y - B.root.y) / grass_canopy_h(p);
	O.alb_h = float4(grass_to_albedo(albedo), grass_sky(B.hemi) * lerp(grass_ao(d, B.ao, y_rel), grass_ao(d, B.ao, 0.5f), culm_sub));

	const float sen = ((B.flags & GRASS_F_THATCH) != 0u || litter) ? 1.0f : B.sen;
	const float gloss = lerp(P14.x, P14.y, sen) * lerp(1.0f, 0.4f, smoothstep(15.0f, 60.0f, d))
		+ 0.03f * max(grass_seas(p, 2, false).y * 0.5f, da_season_env.y);
	O.lat_g = float4(mul((float3x3)m_WV, lat * sr), gloss);
	const float tr = litter ? DA_FOLIAGE_TR_MIN : grass_translucency(lerp(P14.z, P14.w, t) * lerp(1.0f, 0.55f, sen), Vw);
	// Kind: 0 leaf, 1 grass culm (nodes), 3 a forb's stem or a rush (no nodes).
	const float kind = culm ? (form == GRASS_FORM_GRAMINOID ? 1.0f : 3.0f) : 0.0f;
	// The near detail (veins, nodes) for every blade inside d1: the short bin's 7-vertex leaves too. w: a culm's
	// height at the vertex (its nodes), a cut leaf's outline in the short bin (its lobes).
	O.misc = float4(tr, kind, (lod == 0u || (lod == 1u && d < da_grass_vlod.x)) ? 1.0f : 0.0f,
		culm ? t * B.cur.height : (cut_w < 1.0f ? 0.0f : float(outline)));
	return O;
#endif
}
