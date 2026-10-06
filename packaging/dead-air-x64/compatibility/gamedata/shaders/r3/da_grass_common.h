// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Blade grass after Ghost of Tsushima (docs/dead-air/GRASS.md): what the generation compute passes and the
// blade vertex shaders share - the layouts of the engine's buffers (da_grass_blades.cpp mirrors every
// number here), hashes, the profile and season tables, the blade record. SM5 only (FXC and DXC): Load4 /
// Store4 on raw buffers, no templated loads.
#ifndef DA_GRASS_COMMON_H
#define DA_GRASS_COMMON_H

// ---- Draw bins (a fixed window of the blade or head buffer each, near to far) ------------------------
#define GRASS_NBINS			16
#define GRASS_BIN_LOD0		0	// 13-vertex blades, 2 distance bins
#define GRASS_BIN_LOD1		2	// 7-vertex blades, 3 bins
#define GRASS_BIN_LOD2		5	// 3-vertex blades, 4 bins
#define GRASS_VIEW_BINS		9
#define GRASS_BIN_SH_NEAR	9	// sun near cascade, 7 vertices
#define GRASS_BIN_SH_MID	10	// sun middle cascade, 3 vertices
#define GRASS_BIN_HEAD0		11	// seed heads, 43 vertices
#define GRASS_BIN_HEAD1		12	// seed heads, 13 vertices, 2 bins
#define GRASS_BIN_HEAD_SH	14	// seed heads in the near cascade, 13 vertices
#define GRASS_BIN_SHORT		15	// the short leaves of the near lanes (basal pairs, litter, a stem's leaves), 7 vertices

#define GRASS_REC			64	// bytes per blade record
#define GRASS_HEAD_REC		16	// bytes per head record
#define GRASS_LMAX			24	// survival rank: the coarsest lattice level

// ---- Tables (one raw buffer, float4 units) -------------------------------------------------------------
// Species (the blade, culm, head and season of one real plant) and communities (the species shares a detail
// object of the level grows, by habitat).
#define GRASS_SPECIES		64
#define GRASS_COMMUNITIES	16
#define GRASS_COMM_SPECIES	8	// graminoids of a community
#define GRASS_COMM_FORBS	16	// forbs on top of them
#define GRASS_PROF_F4		28
#define GRASS_SEAS_F4		9
#define GRASS_COMM_F4		19
#define GRASS_T_PROF		0
#define GRASS_T_SEAS		(GRASS_SPECIES * GRASS_PROF_F4)
#define GRASS_T_SEAS_PREV	(GRASS_T_SEAS + GRASS_SPECIES * GRASS_SEAS_F4)
#define GRASS_T_COMM		(GRASS_T_SEAS_PREV + GRASS_SPECIES * GRASS_SEAS_F4)
#define GRASS_T_IDS			(GRASS_T_COMM + GRASS_COMMUNITIES * GRASS_COMM_F4)
#define GRASS_T_TOTAL		(GRASS_T_IDS + 16)

// Plant forms (row 19.z) and leaf outlines (row 19.y).
#define GRASS_FORM_GRAMINOID	0u
#define GRASS_FORM_FORB			1u	// a leafy stem: cauline leaves and a terminal inflorescence
#define GRASS_FORM_ROSETTE		2u	// basal leaves and leafless scapes
#define GRASS_FORM_RUSH			3u	// leafless green stems
#define GRASS_LEAF_GRASS		0u
#define GRASS_LEAF_LANCEOLATE	1u
#define GRASS_LEAF_OVATE		2u
#define GRASS_LEAF_BRISTLE		3u
#define GRASS_LEAF_STRAP		4u
#define GRASS_LEAF_PINNATE		5u	// pinnate, lobes cut out near the camera (tansy, mugwort, cow parsley)
#define GRASS_LEAF_DISSECTED	6u	// finely dissected, feathery (yarrow, field wormwood)
// A cut leaf's mean cover of its outline: what is left of it where its lobes are under a pixel.
#define GRASS_CUT_COVER			0.6f

ByteAddressBuffer	da_grass_tables	: register(t1);

float4	grass_t4	(uint f4)	{ return asfloat(da_grass_tables.Load4(f4 * 16)); }

// Species s, row r (da_grass_blades.cpp fill_species):
//  0 leaf_h min/max, leaf_w min/max          1 culm_h min/max, culm_w min/max
//  2 leaf tilt min/max, culm tilt min/max (rad)  3 leaf bend min/max, culm bend min/max
//  4 leaf droop min/max, leaf stiff, culm stiff  5 clump cell, clump pull, clump face away, cover
//  6 density (render), culm share, cauline leaves, folded (0/1)
//  7 head share, head nod min/max (rad), culm knee
//  8 fold yaw (rad), shade density, shade height, clump facing coherence
//  9 tussock h min/max, tussock share, tussock radius
// 10 head type, head length min/max, grain scale      11 head spread min/max, branches, flower petals
// 12 head opening at anthesis / ripe (rad), bob amplitude, bob frequency
// 13 wind tilt, wind bend, push tilt, slope tilt
// 14 gloss live, gloss dead, translucency root, translucency tip
// 15..18 seed or flower colour ramp (linear) at stage 0.25, 0.5, 0.75, 1
// 19 clump height variance, leaf outline, form, unused
// 20 basal leaf h min/max, w min/max     21 basal tilt min/max (rad), basal bend min/max
// 22 basal droop min/max, basal pairs per leaf lane, clump lean (rad)
// 23 cauline leaf h min/max, w min/max   24 cauline tilt min/max (rad), first and last node (share of the stem)
// 25 cauline upper/lower size, opposite (0/1), bend, droop
// 26 stem tint (linear rgb factor of the culm colour), unused
// 27 the widest a lane can be for the pixel rule (the generation's early out), unused
float4	grass_prof	(uint p, uint r)	{ return grass_t4(GRASS_T_PROF + p * GRASS_PROF_F4 + r); }

// Season keys of species p at the day (prev: the previous frame's day), da_grass_data.h order:
//  0 new growth height, green, cohort green, thatch     1 thatch lodging, straw lodging, heads, seed stage
//  2 tip dryness, wetness, frost, density               3 flood, flood ice, grass visible, old heads
//  4..8 live, yellowing, straw, thatch, seed colours (linear rgb)
float4	grass_seas	(uint p, uint r, bool prev)
{
	return grass_t4((prev ? GRASS_T_SEAS_PREV : GRASS_T_SEAS) + p * GRASS_SEAS_F4 + r);
}

// Community c, row r:
//  0 patch size (m), scattered share, graminoid count, full-presence alpha of the stock palette
//  1 graminoid cumulative shares 0..3     2 graminoid cumulative shares 4..7
//  3 x graminoid ids 0..3, y 4..7 (8 bits each), z forb count, w the largest share a forb takes in its patches
//    (asfloat)
//  4 litter pairs per leaf lane, litter length min/max (m), litter width (m)
//  5 size of the forbs' patches (m), the share of the ground the stand grows on, all forbs' scattered share, the
//    area all their patches cover
//  6 forb ids 0..15, 8 bits each
//  7..10 the forbs' cumulative scattered shares    11..14 their patches' cumulative areas
//  15..18 the share of the lanes each forb takes inside its patches
float4	grass_comm	(uint c, uint r)	{ return grass_t4(GRASS_T_COMM + c * GRASS_COMM_F4 + r); }
// Forb f's entry of a community's 16-entry block that starts at row r0.
float	grass_comm_f	(uint c, uint r0, uint f)	{ return grass_comm(c, r0 + (f >> 2))[f & 3u]; }

// Detail object id -> community + 1 (0 = not grown as blades: stock class, unknown, empty).
uint	grass_id_community	(uint id)
{
	return (da_grass_tables.Load((GRASS_T_IDS * 16) + id * 4) & 0x1Fu);
}

// ---- Hashes ----------------------------------------------------------------------------------------
uint	grass_hash	(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
float	grass_u01	(uint x)	{ return float(x >> 8) * (1.0f / 16777216.0f); }
uint	grass_seed	(int2 g, uint salt)	{ return grass_hash(asuint(g.x) * 0x9E3779B1u ^ grass_hash(asuint(g.y) + salt)); }

// ---- Packing ---------------------------------------------------------------------------------------
uint	grass_pack_h2	(float a, float b)	{ return f32tof16(a) | (f32tof16(b) << 16); }
float2	grass_unpack_h2	(uint v)	{ return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16)); }
uint	grass_pack_sn2	(float2 v)
{
	const int2 q = int2(round(clamp(v, -1.0f, 1.0f) * 32767.0f));
	return (asuint(q.x) & 0xFFFFu) | (asuint(q.y) << 16);
}
float2	grass_unpack_sn2	(uint v)
{
	const int2 q = int2(asint(v << 16) >> 16, asint(v) >> 16);
	return float2(q) / 32767.0f;
}
uint	grass_pack_u8	(float4 v)
{
	const uint4 q = uint4(round(saturate(v) * 255.0f));
	return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}
float4	grass_unpack_u8	(uint v)
{
	return float4(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24) / 255.0f;
}

// ---- The blade record (64 bytes) -------------------------------------------------------------------
// a: root xyz, flags                 b: this frame's shape     c: the previous frame's shape
// d: x colour state (senescence, clump colour, ground ao, hemi), y ground normal xz,
//    z stage | snow depth at the root, w head width equivalent | head colour share (culms)
// flags: 0-15 hash, 16-18 species bits 0-2, 19 culm, 20 folded, 21 headed, 22 thatch, 23 cauline leaf,
//        24-27 head type (culms) or 24 litter (leaves), 28 basal leaf pair, 29-31 species bits 3-5
// shape: x facing (snorm16 x2), y height | width, z tilt | bend, w wind | droop
#define GRASS_F_CULM	(1u << 19)
#define GRASS_F_FOLD	(1u << 20)
#define GRASS_F_HEAD	(1u << 21)	// the head pass draws the culm's head (else its far triangle carries it)
#define GRASS_F_THATCH	(1u << 22)
#define GRASS_F_CAULINE	(1u << 23)
#define GRASS_F_LITTER	(1u << 24)
#define GRASS_F_BASAL	(1u << 28)
// Basal leaf pairs and litter pairs a leaf lane carries at most (records after its own), cauline leaves a
// stem carries at most.
#define GRASS_BASAL_MAX		3u
#define GRASS_LITTER_MAX	3u
#define GRASS_CAULINE_MAX	12u

uint	grass_species_bits	(uint s)	{ return ((s & 7u) << 16) | ((s >> 3) << 29); }
uint	grass_flags_species	(uint flags)	{ return ((flags >> 16) & 7u) | ((flags >> 26) & 0x38u); }

struct grass_shape
{
	float2	facing;
	float	height;
	float	width;
	float	tilt;
	float	bend;
	float	wind;
	float	droop;
};

uint4	grass_pack_shape	(grass_shape s)
{
	return uint4(grass_pack_sn2(s.facing), grass_pack_h2(s.height, s.width), grass_pack_h2(s.tilt, s.bend),
		grass_pack_h2(s.wind, s.droop));
}

grass_shape	grass_unpack_shape	(uint4 v)
{
	grass_shape s;
	s.facing = grass_unpack_sn2(v.x);
	const float l = length(s.facing);
	s.facing = l > 1e-4f ? s.facing / l : float2(0.0f, 1.0f);
	const float2 hw = grass_unpack_h2(v.y), tb = grass_unpack_h2(v.z), wd = grass_unpack_h2(v.w);
	s.height = hw.x; s.width = hw.y; s.tilt = tb.x; s.bend = tb.y; s.wind = wd.x; s.droop = wd.y;
	return s;
}

// ---- Colour --------------------------------------------------------------------------------------
// The G-buffer holds albedo the way the stock textures store it (sRGB-encoded); the keys are linear.
float3	grass_to_albedo	(float3 linear_rgb)	{ return pow(max(linear_rgb, 0.0f), 1.0f / 2.2f); }

// Seed colour of profile p at stage s (0..1): the profile's 4-stop ramp, the season's colour below it.
float3	grass_seed_ramp	(uint p, float s, float3 season_seed)
{
	const float3 c0 = grass_prof(p, 15).rgb, c1 = grass_prof(p, 16).rgb;
	const float3 c2 = grass_prof(p, 17).rgb, c3 = grass_prof(p, 18).rgb;
	float3 c = lerp(season_seed, c0, saturate(s * 4.0f));
	c = lerp(c, c1, saturate(s * 4.0f - 1.0f));
	c = lerp(c, c2, saturate(s * 4.0f - 2.0f));
	return lerp(c, c3, saturate(s * 4.0f - 3.0f));
}

#endif // DA_GRASS_COMMON_H
