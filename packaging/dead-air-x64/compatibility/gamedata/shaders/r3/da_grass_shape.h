// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Blade grass, the vertex side (docs/dead-air/GRASS.md): the record of a blade as the generation wrote it,
// the blade curve (GoT: a cubic Bezier with P1 = P0, the tip from tilt and facing, the midpoint bent "up
// and away", a sine bob phased by hash and t), the strip decode of the 13/7/3-vertex LODs with the folded
// pair of short grass, the season colour along the blade and the interpolants of the G-buffer pass.
// Needs common.h first.
#ifndef DA_GRASS_SHAPE_H
#define DA_GRASS_SHAPE_H

#include "da_season_common.h"
#include "da_grass_common.h"
#include "da_material_codes.h"

ByteAddressBuffer	da_grass_blades	: register(t0);
ByteAddressBuffer	da_grass_counts	: register(t2);

uniform float4	da_grass_draw;	// x first record of the bin, y bin, z vertices per blade, w LOD (0..2; 3 near, 4 middle cascade)
uniform float4	da_grass_vcam;	// xyz camera, w minimum width per metre of view depth (k_px x pixel)
uniform float4	da_grass_vfwd;	// xyz camera forward, w draw radius R
uniform float4	da_grass_vlod;	// x d1, y d2, z time, w previous time
uniform float4	da_grass_vsun;	// xyz direction toward the sun (world), w shadow texel (m)
uniform float4	da_grass_vmisc;	// x frame & 63, y 1 = motion vectors, z r__detail_height, w unused

struct grass_blade
{
	float3		root;
	uint		flags;
	grass_shape	cur;
	grass_shape	prev;
	float		sen, hc, ao, hemi;
	float3		nground;
	float		stage;
	float		snow;
	float		head_eq;	// culms: width of the folded head at the top of the far triangle
	float		head_area;	// culms: share of the head in the area of culm + head
	float3		root_prev;	// cauline leaves: their root moves with the culm
};

grass_blade	grass_load	(uint index)
{
	const uint4 a = da_grass_blades.Load4(index * GRASS_REC);
	const uint4 b = da_grass_blades.Load4(index * GRASS_REC + 16u);
	const uint4 c = da_grass_blades.Load4(index * GRASS_REC + 32u);
	const uint4 d = da_grass_blades.Load4(index * GRASS_REC + 48u);
	grass_blade B;
	B.root = asfloat(a.xyz);
	B.flags = a.w;
	B.cur = grass_unpack_shape(b);
	B.prev = grass_unpack_shape(c);
	const float4 cs = grass_unpack_u8(d.x);
	B.sen = cs.x; B.hc = cs.y; B.ao = cs.z; B.hemi = cs.w;
	const float2 ng = grass_unpack_sn2(d.y);
	B.nground = float3(ng.x, sqrt(saturate(1.0f - dot(ng, ng))), ng.y);
	const float2 st = grass_unpack_h2(d.z), hd = grass_unpack_h2(d.w);
	B.stage = st.x;
	const bool cauline = (B.flags & GRASS_F_CAULINE) != 0u;
	B.snow = cauline ? 0.0f : st.y;
	B.head_eq = cauline ? 0.0f : hd.x;
	B.head_area = cauline ? 0.0f : hd.y;
	B.root_prev = B.root + (cauline ? float3(hd.x, st.y, hd.y) : 0.0f);
	return B;
}

uint	grass_profile	(grass_blade B)	{ return grass_flags_species(B.flags); }
bool	grass_is_culm	(grass_blade B)	{ return (B.flags & GRASS_F_CULM) != 0u; }
bool	grass_is_litter	(grass_blade B)	{ return (B.flags & (GRASS_F_LITTER | GRASS_F_CULM)) == GRASS_F_LITTER; }
float	grass_hash01	(grass_blade B)	{ return float(B.flags & 0xFFFFu) / 65536.0f; }

float3	grass_bez	(float3 a, float3 b, float3 c, float3 d, float t)
{
	const float s = 1.0f - t;
	return s * s * s * a + 3.0f * s * s * t * b + 3.0f * s * t * t * c + t * t * t * d;
}
float3	grass_dbez	(float3 a, float3 b, float3 c, float3 d, float t)
{
	const float s = 1.0f - t;
	return 3.0f * s * s * (b - a) + 6.0f * s * t * (c - b) + 3.0f * t * t * (d - c);
}

float2	grass_rotate	(float2 v, float a)
{
	float s, c;
	sincos(a, s, c);
	return float2(v.x * c - v.y * s, v.x * s + v.y * c);
}

// The point, tangent and side of a blade at t (0 root .. 1 tip). yaw turns the facing (folded pairs), bob
// adds the GoT sine (x amplitude, y frequency; 0 = none), time picks this or the previous frame.
void	grass_curve	(grass_shape s, float3 root, float t, float yaw, float2 bob, float time, float hash, float knee,
	out float3 P, out float3 dP, out float3 S)
{
	const float2 f = grass_rotate(s.facing, yaw);
	S = float3(f.y, 0.0f, -f.x);
	float st, ct;
	sincos(s.tilt, st, ct);
	const float3 fwd = float3(f.x, 0.0f, f.y);
	const float3 P0 = root;
	float3 P3 = root + s.height * float3(f.x * st, ct, f.y * st);
	// Arching leaves (sedge): the tip falls forward and down, never into the ground (a lodged leaf).
	P3 += (fwd * 0.25f - float3(0.0f, 0.45f, 0.0f)) * (s.droop * s.height);
	P3.y = max(P3.y, root.y + 0.03f * s.height);
	// The bend lifts the midpoint to the inner, upper side of the chord: the blade leaves its root steeper
	// than the chord and arches outward toward the tip, as real leaves and culms do. The outer, lower side
	// made every blade start almost flat and turn up at the end (a J).
	const float3 up2 = normalize(cross(P3 - P0, S) + float3(0.0f, 1e-5f, 0.0f));
	float3 P2 = lerp(P0, P3, 2.0f / 3.0f) + up2 * (s.bend * s.height);
	[branch] if (bob.x > 0.0f)
	{
		const float ph = bob.y * time + 6.2831853f * hash;
		const float3 bob_dir = normalize(fwd + 0.3f * S);
		const float a = bob.x * s.wind * s.height;
		P2 += bob_dir * (a * 0.5f * sin(ph + 0.7f));
		P3 += bob_dir * (a * sin(ph + 1.4f));
	}
	// A geniculate (kneed) culm leaves its root upright for a share of its height.
	const float3 P1 = P0 + float3(0.0f, knee * s.height, 0.0f);
	P = grass_bez(P0, P1, P2, P3, t);
	dP = grass_dbez(P0, P1, P2, P3, max(t, 0.02f));
}

// Vertex v of a strip of `pairs` pairs and a tip: t0 along the blade (before redistribution), side
// -1/+1 (0 at the tip), which blade of a folded pair (0 = A: flat top, no tip; 1 = B: rows and the tip)
// and the rows its low LOD keeps (morph targets).
void	grass_decode	(uint v, uint pairs, bool folded, out float t0, out float side, out uint which, out float rows_lo)
{
	const uint p = v >> 1;
	const bool tip = v == 2u * pairs;
	side = tip ? 0.0f : ((v & 1u) ? 1.0f : -1.0f);
	which = 1u;
	rows_lo = max(float(pairs >> 1), 1.0f);
	if (!folded || pairs < 2u)
	{
		t0 = tip ? 1.0f : float(p) / float(pairs);
		return;
	}
	const uint b = pairs >> 1;
	if (tip)
		t0 = 1.0f;
	else if (p < b)
	{
		which = 0u;
		t0 = float(b - p) / float(b);
	}
	else
		t0 = float(p - b) / float(pairs - b);
	// The low LOD of a folded pair: A keeps its top row, B keeps its middle row and tip.
	rows_lo = which == 0u ? 1.0f : 2.0f;
}

// t along the blade of strip position x: leaves redistribute their rows towards the tip (GoT), culms are
// emitted top-down (the strip's tip is the root, the far LOD an inverted triangle).
float	grass_t	(float x, bool culm)	{ return culm ? 1.0f - x : pow(x, 0.8f); }

// The point of a blade with the LOD morph: towards the polyline of the next coarser LOD (rows_lo rows of
// the strip), exactly that polyline at morph 1, so the switch cannot pop.
void	grass_curve_lod	(grass_shape s, float3 root, float t0, bool culm, float rows_lo, float morph, float yaw, float2 bob,
	float time, float hash, float knee, out float3 P, out float3 dP, out float3 S)
{
	grass_curve(s, root, grass_t(t0, culm), yaw, bob, time, hash, knee, P, dP, S);
	[branch] if (morph > 0.0f)
	{
		const float tlo = floor(t0 * rows_lo) / rows_lo;
		const float thi = min(tlo + 1.0f / rows_lo, 1.0f);
		float3 Pa, Pb, dA, dB, Sx;
		grass_curve(s, root, grass_t(tlo, culm), yaw, bob, time, hash, knee, Pa, dA, Sx);
		grass_curve(s, root, grass_t(thi, culm), yaw, bob, time, hash, knee, Pb, dB, Sx);
		const float f = saturate((t0 - tlo) * rows_lo);
		P = lerp(P, lerp(Pa, Pb, f), morph);
		dP = lerp(dP, lerp(dA, dB, f), morph);
	}
}

// Leaf-like width along the blade: widest a third up, tapering to the tip.
float	grass_taper	(float t)	{ return (t + 0.35f) * pow(saturate(1.0f - t), 0.9f) / 0.53f; }

// The width along a leaf of a species' outline (t: 0 root .. 1 tip; 1 = the leaf's width): a grass blade,
// a lanceolate forb leaf (widest below the middle, a long point), an ovate one (a broad base: nettle,
// plantain, lily of the valley), a bristle (rolled fescue and hair-grass leaves) and a strap (cattail).
float	grass_leaf_width	(float t, uint outline)
{
	const float pi = 3.14159265f;
	if (outline == GRASS_LEAF_LANCEOLATE)
		return pow(saturate(sin(pi * pow(saturate(t), 0.82f))), 0.9f);
	if (outline == GRASS_LEAF_OVATE)
		return pow(saturate(sin(pi * pow(saturate(t), 0.6f))), 0.6f);
	if (outline == GRASS_LEAF_BRISTLE)
		return pow(saturate(1.0f - t), 0.45f);
	if (outline == GRASS_LEAF_STRAP)
		return lerp(0.75f, 1.0f, smoothstep(0.0f, 0.15f, t)) * saturate(1.0f - pow(saturate((t - 0.5f) * 2.0f), 2.0f));
	// A pinnate leaf's outline is an oblong blade from a short stalk; a dissected one a narrow lanceolate plume.
	if (outline == GRASS_LEAF_PINNATE)
		return smoothstep(0.0f, 0.12f, t) * pow(saturate(1.0f - pow(saturate(t), 2.2f)), 0.55f);
	if (outline == GRASS_LEAF_DISSECTED)
		return smoothstep(0.0f, 0.08f, t) * pow(saturate(sin(3.14159265f * pow(saturate(t), 0.9f))), 0.7f);
	return grass_taper(t);
}

// Dead tissue of a species whose culms differ from one another (profile row 26 w: reed, cattail, the marsh sedges and
// grasses), per blade (hash hb) and a little per clump (hc). 40 photos of dry reed beds, autumn to winter: hue 24-41
// deg p10-p90, saturation 0.18-0.55, r/g x1.6 and b/g x2 across a bed, neighbouring culms 30 % apart in value; so
// from tawny to grey-buff, from pale to dark, at the season colour's luminance on average.
float3	grass_dead_vary	(float3 dead, uint p, float hb, float hc)
{
	const float dv = grass_prof(p, 26).w;
	[branch] if (dv <= 0.0f)
		return dead;
	const float3 LUMW = float3(0.2126f, 0.7152f, 0.0722f);
	const float y0 = dot(dead, LUMW);
	float3 v = dead * lerp(float3(1.1f, 0.92f, 0.75f), float3(0.94f, 1.04f, 1.3f), lerp(frac(hb * 53.91f), hc, 0.35f));
	const float y = dot(v, LUMW);
	v = max(y + (v - y) * lerp(0.55f, 1.3f, frac(hb * 17.31f)), 0.0f) * (y0 / max(y, 1e-4f));
	return lerp(dead, v * lerp(0.78f, 1.22f, frac(hb * 71.13f)), dv);
}

// Season colour of a blade at t (linear rgb, dry albedo with dew, frost, flood and snow on top).
float3	grass_colour	(grass_blade B, uint p, float t, float3 n_world)
{
	const float4 K2 = grass_seas(p, 2, false), K3 = grass_seas(p, 3, false);
	const float3 live = grass_seas(p, 4, false).rgb, yellowing = grass_seas(p, 5, false).rgb;
	const float3 straw = grass_seas(p, 6, false).rgb, thatch_c = grass_seas(p, 7, false).rgb;
	const bool thatch = (B.flags & GRASS_F_THATCH) != 0u;
	// Per blade (its hash) a value step and, for a few live blades, a step of hue towards the yellowing
	// colour; per clump a value step (GoT: a colour column per clump, kept small).
	const float hb = grass_hash01(B);
	const float vb = frac(hb * 37.17f), yb = frac(hb * 91.73f);
	// The stand's litter: last years' leaves weathered grey-brown, some still straw, matted and damp
	// underneath, so it reads as felt and not as bare soil or as a second layer of fresh straw.
	if (grass_is_litter(B))
	{
		const float3 LUMW_L = float3(0.2126f, 0.7152f, 0.0722f);
		float3 lit = lerp(thatch_c, straw, 0.25f * yb);
		lit = lerp(dot(lit, LUMW_L), lit, 0.6f) * lerp(0.42f, 0.7f, vb) * lerp(0.9f, 1.08f, B.hc);
		lit *= lerp(1.0f, 0.55f, max(K2.y * 0.5f, da_season_env.y * 0.6f) * 0.5f);
		lit = lerp(lit, float3(0.62f, 0.66f, 0.70f), 0.6f * da_season_env.x);
		return lit;
	}
	// Tips cure first; the dry share of the tip comes from the season.
	const float sen_v = thatch ? 1.0f : saturate(B.sen * 1.6f - (1.0f - t) * 0.6f + step(1.0f - K2.x, t));
	// Live tissue: the field greens of the keys (photo measurements) come out khaki under the weathers' warm
	// light and the colour grade; turned towards blue-green with a little more chroma at the same luminance
	// they read on screen as the photos do (GoT pushes its summer greens the same way). Dead tissue the other
	// way: the warm light and the straw's yellow transmission make the measured straw read as yellow on
	// screen, so it loses some chroma at the same luminance.
	const float3 LUMW = float3(0.2126f, 0.7152f, 0.0722f);
	float3 dead = thatch ? thatch_c : lerp(yellowing, straw, saturate(sen_v * 2.0f - 1.0f));
	dead = lerp(dot(dead, LUMW), dead, 0.82f);
	dead = grass_dead_vary(dead, p, hb, B.hc);
	// A green that is already cool (the glaucous fescues, grey hair-grass, wormwoods: blue over green above half)
	// keeps its hue: turned further to blue on top of its photo colour it read cyan-white in a front-lit field. The
	// chroma it keeps like every green (the colour grade takes a quarter of the greens' saturation).
	const float push = saturate((0.5f - live.b / max(live.g, 1e-4f)) / 0.25f);
	float3 live_g = live * lerp(float3(1.0f, 1.0f, 1.0f), float3(0.76f, 1.0f, 1.12f), push);
	const float y_g = dot(live_g, LUMW);
	live_g = max(y_g + (live_g - y_g) * 1.3f, 0.0f) * (dot(live, LUMW) / max(y_g, 1e-4f));
	const float3 live_b = lerp(live_g, yellowing, 0.25f * yb * yb);
	float3 c = lerp(live_b, dead, saturate(sen_v * 2.0f)) * (lerp(0.88f, 1.12f, B.hc) * lerp(0.86f, 1.14f, vb));
	// Dark base, light top (GoT: colour along the blade); the occlusion darkens the base further.
	c *= lerp(0.62f, 1.04f, sqrt(t));
	// Silt on the lower blade after a flood.
	c *= lerp(1.0f, 0.75f, K3.x * (1.0f - smoothstep(0.1f, 0.35f, t)));
	// Hoarfrost on the upper blade.
	c = lerp(c, float3(0.62f, 0.66f, 0.70f), 0.7f * da_season_env.x * sqrt(t));
	// Dew and the season's damp: porous straw darkens more than waxy live blades (Lagarde).
	const float wet = max(K2.y * 0.5f, da_season_env.y * 0.6f);
	c *= lerp(1.0f, lerp(0.85f, 0.4f, sen_v), wet * 0.5f);
	// Falling snow settles on the upper, up-facing part.
	c = lerp(c, 0.85f, 0.8f * da_season_env.w * saturate(n_world.y * 2.0f - 0.5f) * smoothstep(0.3f, 0.6f, t));
	return c;
}

// The colour of a culm's seed head at its stage (old heads on thatch culms are weathered thatch).
// A grass head is a cluster of small spikelets with air and shade between them: darker than the ramp's colour
// of a single glume. A forb's flowers face the light with their petals: nearly their own colour.
float3	grass_head_colour	(grass_blade B, uint p, float stage)
{
	const float3 seed = grass_seed_ramp(p, stage, grass_seas(p, 8, false).rgb);
	const bool thatch = (B.flags & GRASS_F_THATCH) != 0u;
	const bool forb = uint(grass_prof(p, 19).z + 0.5f) != GRASS_FORM_GRAMINOID;
	const float3 c = (thatch ? lerp(seed, grass_seas(p, 7, false).rgb, 0.5f) : seed) * (forb && !thatch ? 1.0f : 0.8f);
	// Ripe and old plumes differ head to head as their culms do (silvery buff to purple-brown in a reed bed).
	return lerp(c, grass_dead_vary(c, p, frac(grass_hash01(B) * 7.31f), B.hc), thatch ? 1.0f : saturate(stage * 2.0f - 1.0f));
}

// The G-buffer interpolants of blades, culms and heads (da_grass_blade.ps).
struct v2p_grass
{
	float4	hpos		: SV_Position;
	float4	hpos_cur	: TEXCOORD0;
	float4	hpos_prev	: TEXCOORD1;
	float4	pv_t		: TEXCOORD2;	// view-space position, t along the blade or head
	float4	nb_u		: TEXCOORD3;	// face normal x cos(rounding) (view space), u across 0..1
	float4	lat_g		: TEXCOORD4;	// lateral part of the rounded normal (view space), gloss
	float4	nc			: TEXCOORD5;	// canopy normal (view space), its weight
	float4	alb_h		: TEXCOORD6;	// albedo (as the G-buffer holds it), hemi x ambient occlusion
	float4	misc		: TEXCOORD7;	// x translucency, y kind (0 leaf, 1 culm, 2 head), z detail (1 = near LOD), w metres or lobes
};

// Sky openness of a blade from its slot's baked hemi (4 bits): the stock grass multiplies by 3.5 and
// saturates, which makes the sky occlusion binary; 1.6 keeps open ground at 1 like the terrain and the
// sheltered slots graded.
float	grass_sky	(float slot_hemi)	{ return saturate(slot_hemi * 1.6f); }

// The canopy of profile p now (m): its leaves at this season's growth and the height setting, at least
// a lodged mat.
float	grass_canopy_h	(uint p)
{
	return max(grass_prof(p, 0).y * grass_seas(p, 0, false).x * da_grass_vmisc.z, 0.12f);
}

// Ambient occlusion inside the canopy (GoT: dark root, light top), by the point's height above the
// ground in canopy heights: lodged and arching blades lying in the base stay dark along their length,
// culms and heads above the canopy are open, clump centres are darker. The root eases a little with
// distance (the far field's pixel-floor blades show mostly their tops) but never fades away, so the far
// field keeps the near field's mean and the base never glows. It eases a step more where the basal pairs
// fade out (d1 to d2): a blade there stands for a stand whose lower leaves hide one another's dark bases.
float	grass_ao	(float d, float ao, float y_rel)
{
	const float root = saturate(lerp(0.3f, 0.5f, saturate(d / da_grass_vfwd.w)) + 0.1f * smoothstep(da_grass_vlod.x, da_grass_vlod.y, d)
		- 0.3f * (1.0f - ao));
	const float a = saturate(y_rel);
	return lerp(root, 1.0f, a * (2.0f - a));
}

// Weight of the canopy normal (GoT specular anti-aliasing: blades shade more like the canopy with distance).
float	grass_canopy_weight	(float d)
{
	return saturate(0.25f + 0.5f * smoothstep(0.0f, 40.0f, d) + 0.2f * smoothstep(40.0f, da_grass_vfwd.w, d));
}

// The canopy normal of a blade as the camera sees the field: upright blades turn their faces to the
// viewer, lodged ones face the sky. An up-facing normal alone lit the middle distance like a lawn seen
// from above, brighter than the near blades.
//
// It stands for the mean of the faces the camera sees, and that mean is shorter than a unit normal: a face
// turned from the view by phi shows cos(phi) of its area and catches the sun behind the viewer by cos(phi), so the
// lit faces average pi/4 of the full cosine. A unit canopy normal caught all of it, and with the sun behind the
// viewer the middle distance shone pale over the near blades (a cream band at 20-40 m). The normal leans away
// from the sun until its cosine is the faces' mean.
float3	grass_canopy_normal	(grass_blade B, float3 to_cam)
{
	const float vl = length(to_cam.xz);
	const float2 vh = vl > 1e-4f ? to_cam.xz / vl : float2(0.0f, 0.0f);
	float st, ct;
	sincos(B.cur.tilt, st, ct);
	float3 n = normalize(B.nground * (0.45f + 0.55f * st) + float3(vh.x, 0.0f, vh.y) * (0.8f * ct)
		+ float3(B.cur.facing.x, 0.0f, B.cur.facing.y) * (0.2f * st));
	const float3 Ls = da_grass_vsun.xyz;
	const float cl = dot(n, Ls);
	if (cl > 0.0f)
	{
		const float c = 0.785f * cl;
		const float3 t = n - Ls * cl;
		const float tl = length(t);
		const float3 tn = tl > 1e-4f ? t / tl : normalize(cross(Ls, float3(0.0f, 0.0f, 1.0f)) + float3(1e-4f, 0.0f, 0.0f));
		n = Ls * c + tn * sqrt(saturate(1.0f - c * c));
	}
	return n;
}

// Translucency only where the light comes through: the full value when the camera looks towards the sun,
// the lowest foliage step otherwise (hmodel.h lets sky through every foliage pixel by its step).
float	grass_translucency	(float tr, float3 to_cam)
{
	return lerp(DA_FOLIAGE_TR_MIN, tr, smoothstep(-0.15f, 0.55f, dot(-to_cam, da_grass_vsun.xyz)));
}

#endif // DA_GRASS_SHAPE_H
