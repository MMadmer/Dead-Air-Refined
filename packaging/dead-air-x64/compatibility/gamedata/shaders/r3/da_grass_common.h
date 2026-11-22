// Copyright (c) 2026 Grizzly
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
#define GRASS_PROF_F4		32
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
// 19 clump height variance, leaf outline, form, a cattail's velvet head (1)
// 20 basal leaf h min/max, w min/max     21 basal tilt min/max (rad), basal bend min/max
// 22 basal droop min/max, basal pairs per leaf lane, clump lean (rad)
// 23 cauline leaf h min/max, w min/max   24 cauline tilt min/max (rad), first and last node (share of the stem)
// 25 cauline upper/lower size, opposite (0/1), bend, droop
// 26 stem tint (linear rgb factor of the culm colour), how much the dead tissue varies (grass_dead_vary)
// 27 the widest a lane can be for the pixel rule (the generation's early out), the deepest water the species stands in,
//    a leaf's arch (rad: how far its upper part turns over on average - a leaf rises and arcs over, its tip level or
//    hanging)
// The stand's disorder (GRASS.md "Disorder"), what a species' photos show beside its neat mean:
// 28 the share of leaves that splay out of the tuft, how far (rad from the vertical), the share of leaves folded over
//    in a fresh stand and in a worn one
// 29 a leaf's twist about its length (turns), a dead leaf's curl (rad at the tip), a leaf's sideways sweep (share of
//    its length)
// 30 the share of culms that lean out of the stand, how far (rad more), the share of worn culms snapped at a node, a
//    culm's sinuosity (share of its length)
// 31 the height's spread patch to patch (+- share), the share of short young leaves, how far the stand is laid in
//    patches (0..1 of 62 degrees), the stand's set: the lean a whole stretch of it takes one way (rad)
float4	grass_prof	(uint p, uint r)	{ return grass_t4(GRASS_T_PROF + p * GRASS_PROF_F4 + r); }

// Season keys of species p at the day (prev: the previous frame's day), da_grass_data.h order:
//  0 new growth height, green, cohort green, thatch     1 thatch lodging, straw lodging, heads, seed stage
//  2 tip dryness, wetness, frost, density               3 flood, flood ice, grass visible, old heads
//  4..8 live, yellowing, straw, thatch, seed colours (linear rgb); 4 w the stand's wear (0 a fresh stand .. 1 a
//  battered one: folded leaves, leaning and snapped culms), 5 w how much of its laid patches lie now (0..1)
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

// ---- Hashes, patches, clumps -------------------------------------------------------------------------
#include "da_grass_hash.h"

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

// The cubic Bezier of a blade at rest of its bob (GoT: P1 = P0, the tip from tilt and facing, the midpoint bent "up
// and away"), and its side. The vertex shaders draw it (grass_curve adds the bob); the generation roots a stem's
// leaves on it (a leaf on the chord stood off a bent reed by a hand and hung in the air).
void	grass_ctrl	(grass_shape s, float3 root, float yaw, float knee, out float3 P0, out float3 P1, out float3 P2,
	out float3 P3, out float3 S)
{
	const float2 f = grass_rotate(s.facing, yaw);
	S = float3(f.y, 0.0f, -f.x);
	float st, ct;
	sincos(s.tilt, st, ct);
	const float3 fwd = float3(f.x, 0.0f, f.y);
	P0 = root;
	P3 = root + s.height * float3(f.x * st, ct, f.y * st);
	// Arching leaves (sedge): the tip falls forward and down, never into the ground (a lodged leaf).
	P3 += (fwd * 0.25f - float3(0.0f, 0.45f, 0.0f)) * (s.droop * s.height);
	P3.y = max(P3.y, root.y + 0.03f * s.height);
	// The bend lifts the midpoint to the inner, upper side of the chord: the blade leaves its root steeper
	// than the chord and arches outward toward the tip, as real leaves and culms do. The outer, lower side
	// made every blade start almost flat and turn up at the end (a J).
	const float3 up2 = normalize(cross(P3 - P0, S) + float3(0.0f, 1e-5f, 0.0f));
	P2 = lerp(P0, P3, 2.0f / 3.0f) + up2 * (s.bend * s.height);
	// A geniculate (kneed) culm leaves its root upright for a share of its height.
	P1 = P0 + float3(0.0f, knee * s.height, 0.0f);
}

// ---- The stand's disorder ------------------------------------------------------------------------------
// What a blade does beside the curve of its tilt and bend, by its hash, its species (rows 27-31) and the season's
// wear: a leaf arcs over - a little or all the way, each its own -, or folds where its midrib gave way and hangs from
// there, a dead one curls, every leaf twists about its length and sweeps a little sideways; a culm is sinuous node to
// node, and a worn one snaps at a node, its top hanging with the head. The vertex shaders draw it, and the generation
// roots a stem's leaves on it.
struct grass_warp
{
	float	fold_t;		// where the blade turns over from (0..1 along it; 2: nowhere)
	float	fold_a;		// by how much (rad): what lies past it turns down by as much
	float	fold_w;		// the stretch of the blade the turn takes (a snapped midrib 0.05, an arch the leaf's upper part)
	float2	fold_axis;	// the turn's axis off the blade's side, about the vertical (cos, sin)
	float	curl;		// a dry tip's curl past the fold point (rad at the tip)
	float	sweep;		// the sideways curve of the whole blade (share of its length at the tip)
	float	grow;		// the blade's length against its record's: a leaf that turns over is the longer for it
	float3	wave;		// a culm's sinuosity: amplitude (share of its length), radians over its length, phase
	float2	twist;		// a leaf's twist about its length: phase, radians over its length
};

grass_warp	grass_warp_none	()
{
	grass_warp W;
	W.fold_t = 2.0f; W.fold_a = 0.0f; W.fold_w = 0.1f; W.fold_axis = float2(1.0f, 0.0f); W.curl = 0.0f; W.sweep = 0.0f;
	W.wave = float3(0.0f, 0.0f, 0.0f); W.twist = float2(0.0f, 0.0f); W.grow = 1.0f;
	return W;
}

// A warp without what turns the blade over, at the record's own length: the far triangle and the middle cascade's
// caster run from the root to the blade's end, and a blade that ends at its hanging tip is a stand lying flat (the
// field deflated on its way out to d2). Their one line stands for the blade as it rises.
grass_warp	grass_warp_plain	(grass_warp W)
{
	W.fold_t = 2.0f; W.fold_a = 0.0f; W.curl = 0.0f; W.grow = 1.0f;
	return W;
}

// The warp of the blade of these flags (its 16 hash bits, kind and cohort) of species p; sen: a leaf's senescence
// (a culm's is not asked: its leaves and its head are rooted on it by the flags alone). off: QA, none. plain: only
// what a blade drawn as one line keeps (grass_warp_plain: the sweep, the wave, the twist) - the far triangle's and
// the middle cascade's, most of a view's records, which spares them the turn's tables and draws.
grass_warp	grass_warp_of	(uint flags, uint p, float sen, bool off, bool plain)
{
	grass_warp W = grass_warp_none();
	const bool culm = (flags & GRASS_F_CULM) != 0u;
	if (off)
		return W;
	const bool thatch = (flags & GRASS_F_THATCH) != 0u;
	const uint h1 = grass_hash((flags & 0xFFFFu) ^ 0x9E3779B9u), h2 = grass_hash(h1 + 0x7F4A7C15u);
	const float4 u = float4(h1 & 0xFFu, (h1 >> 8) & 0xFFu, (h1 >> 16) & 0xFFu, h1 >> 24) * (1.0f / 256.0f);
	const float4 q = float4(h2 & 0xFFu, (h2 >> 8) & 0xFFu, (h2 >> 16) & 0xFFu, h2 >> 24) * (1.0f / 256.0f);
	// The stand's litter lies flat: old straws bowed and wavy on the ground, not ruled lines.
	if (!culm && (flags & GRASS_F_LITTER) != 0u)
	{
		W.sweep = (q.z - 0.5f) * 0.7f;
		W.wave = float3(0.03f + 0.04f * q.y, lerp(4.0f, 9.0f, u.w), q.w * 6.2831853f);
		return W;
	}
	[branch] if (culm)
	{
		const float4 P30 = grass_prof(p, 30);
		W.sweep = (q.z - 0.5f) * 3.0f * P30.w;
		W.wave = float3(P30.w * lerp(0.5f, 1.5f, q.y), lerp(5.0f, 11.0f, u.w), q.w * 6.2831853f);
		// A stem snaps as the stand wears; last year's went through all of it. (A corymb's or an umbel's stem has no
		// share in the data: its disc is drawn level whatever the stem does, and hung to the ground on a snapped one.)
		const float snap = plain ? 0.0f : P30.z * (thatch ? 1.0f : smoothstep(0.35f, 1.0f, grass_seas(p, 4, false).w));
		if (u.x < snap)
		{
			// At a node (da_grass_blade.ps draws them at 15, 35 and 60 % of the height).
			W.fold_t = u.y < 0.5f ? 0.35f : (u.y < 0.85f ? 0.6f : 0.15f);
			W.fold_a = radians(lerp(50.0f, 165.0f, u.z));
			W.fold_w = 0.05f;
			sincos((q.x - 0.5f) * 3.0f, W.fold_axis.y, W.fold_axis.x);
		}
		return W;
	}
	const float4 P29 = grass_prof(p, 29);
	W.sweep = (q.z - 0.5f) * 2.0f * P29.z;
	W.twist = float2(q.w * 6.2831853f, P29.x * 6.2831853f * lerp(0.3f, 1.0f, u.w) * (q.x < 0.5f ? -1.0f : 1.0f));
	if (plain)
		return W;
	const float4 P28 = grass_prof(p, 28);
	const float wear = grass_seas(p, 4, false).w;
	const bool cauline = (flags & GRASS_F_CAULINE) != 0u;
	const float worn = thatch ? 1.0f : max(wear, sen);
	const bool basal = (flags & GRASS_F_BASAL) != 0u;
	// A stem leaf folds as it wilts; a short basal leaf has less to fold.
	float fold = cauline ? P28.w * sen : lerp(P28.z, P28.w, worn) * (thatch ? 0.8f : 0.6f);
	if (basal)
		fold *= 0.5f;
	const float curl = P29.y * (thatch ? 1.0f : smoothstep(0.4f, 1.0f, sen)) * lerp(0.3f, 1.7f, q.y);
	// The arch: most leaves a little, a few all the way over (the mean kept), more as the leaf ages and loses its
	// turgor; a stem leaf's as it wilts.
	const float arch = grass_prof(p, 27).z * (cauline ? sen : lerp(0.65f, 1.4f, worn)) * (basal ? 0.7f : 1.0f)
		* (0.15f + 2.55f * u.z * u.z);
	if (u.x < fold)
	{
		// One fold in six is a crease (the midrib snapped), the rest tight bends: a dead leaf's end hangs in an arc,
		// and a stand of creased leaves read as broken sticks.
		W.fold_t = 0.35f + 0.45f * u.y;
		W.fold_a = radians(lerp(45.0f, 150.0f, u.z * lerp(0.6f, 1.0f, worn)));
		W.fold_w = u.w < 0.16f ? 0.05f : lerp(0.12f, 0.45f, u.w);
		sincos((q.x - 0.5f) * 1.6f, W.fold_axis.y, W.fold_axis.x);
	}
	else if (arch > 0.03f || curl > 0.05f)
	{
		// A leaf rises over most of its length and arcs over in its upper third or so (turned over from lower down,
		// a stand of arching leaves stood at two thirds of its height); the turn is done by the tip.
		W.fold_t = 0.55f + 0.3f * u.y;
		W.fold_a = min(arch, 2.6f);
		W.fold_w = min(lerp(0.2f, 0.45f, u.w), 0.98f - W.fold_t);
		sincos((q.x - 0.5f) * 0.9f, W.fold_axis.y, W.fold_axis.x);
	}
	W.curl = curl;
	// What turns over no longer rises: the leaf is the longer for it (a species' leaf height is its stand's, and its
	// leaves reach that height and then arc over and hang), by what the part past the turn would have risen - so the
	// canopy keeps its height (0.94-0.98 of the neat stand's in summer by the numbers, the turn at a right angle and
	// more; a stand of October is a tenth lower).
	if (W.fold_t < 1.5f)
		W.grow = 1.0f + min((1.0f / (0.1f + 0.9f * W.fold_t) - 1.0f) * saturate(W.fold_a / 1.5707963f), 0.6f);
	return W;
}

// The sideways offset of a warped blade at t (shares of its length along its side and along its facing) and its
// derivative: the sweep, and a culm's wave in both.
void	grass_warp_side	(grass_warp W, float t, out float2 off, out float2 doff)
{
	off = float2(W.sweep * t * t, 0.0f);
	doff = float2(2.0f * W.sweep * t, 0.0f);
	[branch] if (W.wave.x > 0.0f)
	{
		float ws, wc, vs, vc;
		sincos(W.wave.y * t + W.wave.z, ws, wc);
		sincos(0.83f * W.wave.y * t + W.wave.z + 1.9f, vs, vc);
		off += W.wave.x * t * float2(ws, vs);
		doff += W.wave.x * float2(W.wave.y * wc * t + ws, 0.83f * W.wave.y * vc * t + vs);
	}
}

// A blade ready to be evaluated: its control points (grass_ctrl; with the bob, da_grass_shape.h), its side and its
// length, and where and how far it may turn over - worked out once for every point of it.
struct grass_frame
{
	float3	P0, P1, P2, P3, S0;
	float	height;
	float3	Pk;		// the point the blade turns over about
	float3	A;		// the turn's axis
	float	a_max;	// the most it turns
};

grass_frame	grass_frame_of	(grass_warp W, float3 P0, float3 P1, float3 P2, float3 P3, float3 S0, float height)
{
	grass_frame F;
	F.P0 = P0; F.P1 = P1; F.P2 = P2; F.P3 = P3; F.S0 = S0;
	F.height = height;
	F.Pk = P0;
	F.A = S0;
	F.a_max = 0.0f;
	[branch] if (W.fold_t < 1.0f)
	{
		const float3 fwd = float3(-S0.z, 0.0f, S0.x);
		float2 koff, kdoff, eoff, edoff;
		grass_warp_side(W, W.fold_t, koff, kdoff);
		grass_warp_side(W, 1.0f, eoff, edoff);
		F.Pk = grass_bez(P0, P1, P2, P3, W.fold_t) + (S0 * koff.x + fwd * koff.y) * height;
		F.A = S0 * W.fold_axis.x + fwd * W.fold_axis.y;
		// What hangs ends above the ground: the turn takes the blade's end no lower than its root's level. The end
		// swings about the axis in the upright plane across it, from phi off the vertical (the axis lies level, the
		// turn is toward its forward side), on the chord from the turning point; it may come to the angle whose
		// cosine puts it that far below the turning point as the turning point stands over the root. (By the tangent
		// at the fold and the blade's length, a quarter of an autumn stand's leaves still reached the ground and lay
		// ruled along it: the blade goes on curving down past the fold.)
		const float3 fw = float3(-F.A.z, 0.0f, F.A.x);
		const float3 ve = P3 + (S0 * eoff.x + fwd * eoff.y) * height - F.Pk;
		const float hf = dot(ve, fw);
		const float len = max(length(float2(hf, ve.y)), 1e-4f);
		const float drop = clamp((F.Pk.y - P0.y - 0.03f * height) / len, -1.0f, 1.0f);
		F.a_max = max(acos(-drop) - atan2(hf, ve.y), 0.0f);
	}
	return F;
}

// The blade's point, tangent and side at t: the Bezier of its control points with the sideways sweep and the culm's
// wave on it, then the turn - what lies past the turning point swings about it, down and a little aside, by the fold
// or the arch and the curl growing to the tip, never past the frame's limit.
void	grass_frame_at	(grass_warp W, grass_frame F, float t, out float3 P, out float3 dP, out float3 S)
{
	const float3 fwd = float3(-F.S0.z, 0.0f, F.S0.x);
	float2 off, doff;
	grass_warp_side(W, t, off, doff);
	P = grass_bez(F.P0, F.P1, F.P2, F.P3, t) + (F.S0 * off.x + fwd * off.y) * F.height;
	dP = grass_dbez(F.P0, F.P1, F.P2, F.P3, max(t, 0.02f)) + (F.S0 * doff.x + fwd * doff.y) * F.height;
	S = F.S0;
	[branch] if (t > W.fold_t)
	{
		const float rest = max(1.0f - W.fold_t, 1e-3f);
		const float x = saturate((t - W.fold_t) / rest);
		const float k = saturate((t - W.fold_t) / W.fold_w);
		const float a_free = W.fold_a * k * k * (3.0f - 2.0f * k) + W.curl * x * x;
		const float a = min(a_free, F.a_max);
		// (The angle's own change along the blade turns the tangent too: without it a curled tip's ribbon stood
		// askew to its curve by half a right angle and more, and pinched thin.)
		const float da = a_free < F.a_max ? W.fold_a * 6.0f * k * (1.0f - k) / W.fold_w + 2.0f * W.curl * x / rest : 0.0f;
		float sa, ca;
		sincos(a, sa, ca);
		const float3 A = F.A;
		const float3 v = P - F.Pk;
		const float3 vr = v * ca + cross(A, v) * sa + A * (dot(A, v) * (1.0f - ca));
		P = F.Pk + vr;
		dP = dP * ca + cross(A, dP) * sa + A * (dot(A, dP) * (1.0f - ca)) + cross(A, vr) * da;
		S = F.S0 * ca + cross(A, F.S0) * sa + A * (dot(A, F.S0) * (1.0f - ca));
		// (The limit is the end's; a point on the way to it may dip a finger lower: it lies on the ground then.)
		P.y = max(P.y, F.P0.y + 0.02f * F.height);
	}
}

// A stem's own bob (da_grass_blade_vs.h): none for a stem that carries leaves - they are rooted on its curve at rest,
// and a bobbing stem left them behind.
float2	grass_stem_bob	(uint p, bool culm, float2 bob)
{
	return (culm && grass_prof(p, 6).z > 0.5f) ? float2(0.0f, 0.0f) : bob;
}

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
