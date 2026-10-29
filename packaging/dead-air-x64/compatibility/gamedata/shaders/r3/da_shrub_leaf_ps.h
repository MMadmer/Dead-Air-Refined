// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural shrubs, the G-buffer pixel shader of the leaves (docs/dead-air/SHRUBS.md): the species' leaf texture
// (tools/shrubs/textures) cuts the leaf out of its hull or quad by its coverage (its mips keep the coverage at a
// half, so a far leaf neither thins nor swells) and gives it its veins, mottling, margin and relief. Two-sided: the
// face the camera sees decides the upper or the underside colour and the texture's luminance of that side (the
// underside's veins raised and paler), turning over smoothly as a leaf passes edge-on; the shading normal is the
// blade's (rounded a little across it), half the crown's (the leaf's own half fading out as it goes edge-on, so a
// fluttering leaf never flashes), then the texture's relief in the leaf's own frame - the underside the same sheet
// seen from below - kept on the viewer's side, the gloss the texture's share of the species' and filtered by the
// normal's screen variance (Tokuyoshi & Kaplanyan 2021). A card's or a sprig's twig takes the twig colour. The
// translucent foliage codes 27-30 (da_material_codes.h), the light through the lamina, not the veins.

#include "common.h"
#include "da_velocity.h"
#include "da_material_codes.h"
#include "da_shrub_common.h"

struct p_shrub_leaf
{
	float4	hpos		: SV_Position;
	float3	hpos_cur	: TEXCOORD0;
	float3	hpos_prev	: TEXCOORD1;
	float3	pv			: TEXCOORD2;
	float2	uv			: TEXCOORD3;
	nointerpolation uint4	a	: TEXCOORD4;	// (da_shrub_leaf_vs.h)
	nointerpolation uint4	b	: TEXCOORD5;
};

f_deffer	main	(p_shrub_leaf I)
{
	// (QA, r__dbg_trees 65536: every pixel out at once - the draw's vertices, its rasterizing and its shader launches
	// alone.)
	[branch] if (da_shrub_chunk0.w > 1.5f)
		discard;
	// The leaf's constants (da_shrub_leaf_vs.h).
	const uint flags = I.b.w & 0xFFu;
	const uint kind = flags & 7u;
	const float mirror = (flags & 8u) != 0u ? -1.0f : 1.0f;
	const float3 tc = float3(I.uv, float(I.b.y >> 24));
	// (A far leaf's triangle reaches past its texture's square.)
	clip(0.5f - max(abs(tc.x - 0.5f), abs(tc.y - 0.5f)));
	const float px_w = f16tof32(I.b.z & 0xFFFFu), fold = f16tof32(I.b.w >> 16);
	const float4 alb_h = shrub_leaf_u8x4_f(I.a.w), und_t = shrub_leaf_u8x4_f(I.b.x);
	// (Its share of the shading normal: a third near, half from 15 m.)
	const float3 crown = shrub_oct(I.a.z);
	const float crown_w = lerp(0.3f, 0.5f, smoothstep(5.0f, 15.0f, I.pv.z));
	const float4 D = s_shrub_leaf_d.Sample(smp_base, tc);
	// The cut-out from a mip no coarser than sixteen texels a side: a leaf of a few pixels was cut by a coarse mip's
	// blot, a square at two or three - a front-lit crown at a dozen metres read as a brick mosaic (the temporal resolve
	// smooths the finer edge; the mips keep the coverage, so the leaf neither thins nor swells). Not a needle sprig's:
	// its needles under a pixel cut from a fine mip scattered into a haze, the near junipers thin and pale from 15 m.
	// (The mip by the array's loaded size - the texture quality drops its top mips - and the isotropic level: the
	// anisotropic one is finer than a plain sample at it.)
	uint lw, lh, le, lm;
	s_shrub_leaf_d.GetDimensions(0, lw, lh, le, lm);
	const float cut_lod = kind == SHRUB_LEAF_NEEDLES ? 16.0f : log2(max(float(lw), 16.0f) / 16.0f);
	// Under eight pixels the cut-out turns stochastic over the frames (hashed alpha, Wyman and McGuire 2017): its threshold
	// an interleaved gradient noise of the pixel and the frame, its alpha the mip its pixels read, so the temporal resolve
	// (TAA, DLSS, FSR) sums a soft leaf of its own outline and area where the hard cut drew a square of a few pixels - an
	// upscaler's sharpening squared it more, and a crown at 20-40 m read as a mosaic of them. Hard without a temporal
	// resolve (one frame of it is noise), and for a needle sprig (its fine mip above).
	const float frame = da_shrub_time.z > 0.5f ? da_shrub_fwd.w : -1.0f;
	const float soft = frame >= 0.0f && kind != SHRUB_LEAF_NEEDLES ? saturate((8.0f - px_w) / 5.0f) : 0.0f;
	const float lod = s_shrub_leaf_d.CalculateLevelOfDetail(smp_linear, tc.xy);
	const float2 nq = floor(I.hpos.xy) + float2(13.0f, 41.0f) + 7.13f * max(frame, 0.0f);
	const float noise = frac(52.9829189f * frac(dot(nq, float2(0.06711056f, 0.00583715f))));
	clip(s_shrub_leaf_d.SampleLevel(smp_base, tc, lerp(min(lod, cut_lod), lod, soft)).a - lerp(0.5f, noise, soft));
	shrub_leaf_bite(tc.xy, flags, tc.z);
	// The relief, the gloss and the underside's luminance resolve on a leaf of a few pixels and more; under that, their
	// means (a flat leaf, the texture's mean gloss).
	float4 Nm = float4(0.5f, 0.5f, 0.7f, 0.5f);
	[branch] if (px_w > 3.0f)
		Nm = s_shrub_leaf_n.Sample(smp_base, tc);

	const float3 V = -normalize(I.pv);
	const float3 n = shrub_oct(I.a.x);
	// The face the camera sees, turning over across a sliver of angles rather than in one frame.
	const float facing = dot(n, V);
	const float side_k = smoothstep(-0.2f, 0.2f, facing);
	const float sgn = facing < 0.0f ? -1.0f : 1.0f;
	// The texture's frame on the upper side: +u across (mirrored with the leaf), +v along to the tip.
	const float3 s_v = shrub_oct(I.a.y);
	const float3 tu = normalize(s_v - n * dot(s_v, n));
	const float3 tv = cross(tu, n) * mirror;
	// Folded along the midrib (each half's normal turned up toward it, smoothly over the midrib's few percent of the
	// width) and rounded toward the margins (convex), both less as the leaf shrinks on the screen (the texture's relief
	// carries the fine rest).
	const float across = (tc.x - 0.5f) * 2.0f * mirror;
	const float shape_k = saturate(px_w / 12.0f);
	const float turn = shape_k * (radians(14.0f) * across * abs(across) - fold * clamp(across * 12.0f, -1.0f, 1.0f));
	float sr, cr;
	sincos(turn, sr, cr);
	const float3 n_geo = normalize(n * cr + tu * (mirror * sr)) * sgn;
	// Half the crown's: the leaf's own half fades as it turns edge-on (a fluttering leaf passing edge-on would flip
	// its light).
	const float own = abs(2.0f * side_k - 1.0f);
	float3 N = normalize(n_geo * ((1.0f - crown_w) * own) + crown * crown_w + V * 0.02f);
	// The texture's relief in the leaf's frame (seen from below, the same sheet: its slopes the other way round).
	const float3 nt = shrub_unpack_n(Nm.rg);
	N = normalize(N * nt.z + (tu * nt.x + tv * nt.y) * sgn);
	N = normalize(N + V * saturate((kind == SHRUB_LEAF_NEEDLES ? 0.35f : 0.12f) - dot(N, V)));

	// The upper side: the texture's luminance (the veins, the mottling, the margin), the veins a touch yellower; the
	// underside its own. The factors are linear, the albedo is in the G-buffer's encoding.
	float3 up = alb_h.rgb * pow(max(D.r * 2.0f, 0.0f), 1.0f / 2.2f);
	up = lerp(up, up * float3(1.03f, 1.02f, 0.9f), D.g * 0.4f);
	const float3 dn = und_t.rgb * pow(max(Nm.a * 2.0f, 0.0f), 1.0f / 2.2f);
	float3 albedo = lerp(dn, up, side_k);
	// A card's or a sprig's twig (a vein at one, no light through it): the twig colour.
	const float twig = saturate((D.g - 0.9f) * 10.0f) * saturate(1.0f - D.b * 20.0f);
	albedo = lerp(albedo, shrub_leaf_u8x4_f(I.b.y).rgb * pow(max(D.r * 2.0f, 0.0f), 1.0f / 2.2f), twig);

	float gloss = f16tof32(I.b.z >> 16) * Nm.b * lerp(0.5f, 1.0f, side_k) * (1.0f - 0.5f * twig);
	{
		const float3 dndu = ddx(N), dndv = ddy(N);
		const float kernel_r2 = 2.0f * 0.15915494f * (dot(dndu, dndu) + dot(dndv, dndv));
		const float a2 = 2.0f / (16.0f + 2.0f);
		gloss *= a2 / (a2 + min(kernel_r2, 0.18f));
	}
	const float tr = lerp(DA_FOLIAGE_TR_MIN, und_t.w, saturate(D.b) * (1.0f - twig));
	const float mtl = da_pack_foliage_mtl(tr, I.hpos.xy, uint(da_shrub_time.z > 0.5f ? da_shrub_fwd.w : 0.0f));
	f_deffer O = pack_gbuffer(float4(N, shrub_face_hemi(alb_h.w, N)), float4(I.pv, mtl), float4(saturate(albedo), saturate(gloss)));
	O.velocity = da_velocity(float4(I.hpos_cur.xy, 0.0f, I.hpos_cur.z), float4(I.hpos_prev.xy, 0.0f, I.hpos_prev.z));
	return O;
}
