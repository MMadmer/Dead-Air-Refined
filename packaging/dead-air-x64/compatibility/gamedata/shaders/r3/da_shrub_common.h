// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Procedural vegetation (docs/dead-air/SHRUBS.md, TREES.md): what the vegetation vertex shaders share - the layouts
// of the engine's buffers (da_veg_internal.h mirrors every number here), the records of a template (axes, wood,
// leaves, blooms, clusters, blobs; the trees' stock tubes, patches and spray cards), the frame's instances and
// chunks, the species and season tables, the element's fate in a chunk (the pixel floor's survivors and a tier's
// crossfade, both by the element's own hash, the survivors grown to keep the area), the bend of the axis hierarchy
// under the wind service's field and motors, and a tree's trunk sway on its stock root's oscillator. SM5 only (FXC
// and DXC): Load4 on raw buffers.
#ifndef DA_SHRUB_COMMON_H
#define DA_SHRUB_COMMON_H

#include "da_season_common.h"
#include "da_material_codes.h"
#include "da_wind_field.h"
#include "da_wind_motors.h"

// ---- Buffers ---------------------------------------------------------------------------------------------
ByteAddressBuffer	da_shrub_elems	: register(t0);	// template elements, SHRUB_REC bytes each
ByteAddressBuffer	da_shrub_axes	: register(t1);	// template axes, SHRUB_AXIS_REC bytes each
ByteAddressBuffer	da_shrub_inst	: register(t2);	// the frame's instances, SHRUB_INST_REC bytes each
ByteAddressBuffer	da_shrub_chunks	: register(t3);	// the frame's chunks, 16 bytes each; past SHRUB_CUT_WORDS the trees'
													// GPU cut's lists (da_tree_cut.cs), a word an element
ByteAddressBuffer	da_shrub_tables	: register(t4);	// species and season tables, float4 units
ByteAddressBuffer	da_shrub_bend	: register(t5);	// the frame's bends, SHRUB_BEND_REC bytes a chunk slot (da_shrub_bend.cs)
ByteAddressBuffer	da_shrub_aux	: register(t6);	// the trees' rings and patch vertices, SHRUB_AUX_SLOT bytes a slot

#define SHRUB_REC			32
#define SHRUB_AXIS_REC		32
#define SHRUB_INST_REC		96
#define SHRUB_CHUNK			64		// elements per chunk
#define SHRUB_BEND_REC		36		// a chunk slot's bend (the frame's slots, then the view's a frame back)
#define SHRUB_BLOB_GROW		1.4f	// a blob's radius over its clusters' spread (da_shrub_gen.cpp kBlobGrow)
#define SHRUB_AUX_SLOT		32		// a ring is two slots, a patch vertex one
// The cut's lists in the chunk buffer: past its chunk records (da_veg_internal.h kMaxChunks of them: a vertex shader
// has eight resource slots, the far wood's bark array the last).
#define SHRUB_CUT_WORDS		4194304u

// Element kinds (da_shrub_draw.y).
#define SHRUB_K_WOOD		0u
#define SHRUB_K_LEAF		1u
#define SHRUB_K_BLOOM		2u
#define SHRUB_K_CLUSTER		3u
#define SHRUB_K_BLOB		4u
#define SHRUB_K_STOCK		5u
#define SHRUB_K_PATCH		6u

// Draws the shaders tell apart (da_veg_internal.h Draw), and how many there are.
#define SHRUB_DRAWS			20
#define SHRUB_DR_LEAF_CUT	3u
#define SHRUB_DR_SPRAY_CUT	8u
#define SHRUB_DR_NEEDLE2	11u

uniform float4	da_shrub_draw;	// x vertices per element, y kind, z draw (da_veg_internal.h Draw), w pass (0 view, 1.. cascade)
uniform float4	da_shrub_chunk0;	// x the draw's first chunk (a cut draw's first word), y the wood's QA view (r__dbg_wood,
								// 0 off), z 1: a draw of the trees' GPU cut, w 1: QA, the patches without their runs
								// (r__dbg_trees 32768)
uniform float4	da_shrub_cam;	// xyz camera (world), w metres of a render pixel per metre of view depth
uniform float4	da_shrub_fwd;	// xyz camera forward, w frame & 63
uniform float4	da_shrub_time;	// x time, y previous time, z 1 = motion vectors, w shadow texel (m) of a cascade pass

// The motion vectors take the wind's own motion a frame back (the second bend, sway and flutter) only within this view
// depth: past it a leaf's or a twig's sway is under a tenth of a pixel a frame, and the second pass was a third of the
// vertex shaders' work; the camera's motion stands everywhere.
#define SHRUB_MV_NEAR		15.0f
bool	shrub_mv_deform	(float3 p)	{ return da_shrub_time.z > 0.5f && dot(p - da_shrub_cam.xyz, da_shrub_fwd.xyz) < SHRUB_MV_NEAR; }
uniform float4	da_shrub_sun;	// xyz towards the sun (world), w snow on the branches now (0..1)
// The world's up in view space (the pixel shaders' way to the world: m_V reads zero there in these passes).
uniform float4	da_shrub_up;
uniform float4	da_shrub_bend_cap;	// x the bend buffer's slots of this frame, y of a frame back (the view's); past them the
							// vertex shaders walk the chain of axes themselves

// ---- Tables ----------------------------------------------------------------------------------------------
// The shrubs' species rows 0..47, the trees' 48..127.
#define SHRUB_SPECIES		128
#define SHRUB_PROF_F4		22
#define SHRUB_SEAS_F4		16
#define SHRUB_T_PROF		0
#define SHRUB_T_SEAS		(SHRUB_SPECIES * SHRUB_PROF_F4)
#define SHRUB_T_TOTAL		(SHRUB_T_SEAS + SHRUB_SPECIES * SHRUB_SEAS_F4)

float4	shrub_t4	(uint f4)	{ return asfloat(da_shrub_tables.Load4(f4 * 16)); }

// Species s, row r (da_shrubs.cpp fill_profile):
//  0 leaf kind (SHRUB_LEAF_*), outline (SHRUB_OUT_*), leaflets (a spray's leaves), leaflet angle (rad)
//  1 translucency, gloss, flutter amplitude (rad), fold across the midrib (rad)
//  2 underside lightness (x the upper colour), underside blue shift, leaf value jitter, leaf hue jitter
//  3 bloom shape (da_shrubs.cpp bloom_shape: 0 none .. 8 lacecap), petals, fruit shape (fruit_shape: 0 none .. 10
//    aggregate), fruits per bloom
//  4 catkin shape (0 none, 1 hanging, 2 upright), bark roughness, twig gloss, lenticels
//  5 leaf droop, needle density, evergreen (0/1), leaf lobes (0 none, n lobes)
//  6 autumn mix: share of colour B, its pull to the lit leaves, the colour wave's start at the margin, browning
//    before the fall
//  7 phenology spread between bushes (days), a leaflet's (a spray leaf's) width over its length, the leaves'
//    phyllotaxis step (rad: pi two-ranked, 2.4 spiral, pi/2 crossed pairs), leaves per node (1 alternate, 2
//    opposite)
//  8 the leaf texture's layers: the leaf a, the leaf b (a second leaf, or a again), the spray card, a compound leaf's
//    leaflet (-1 none)
//  9 the near leaf's hull in its texture (four rows of a pair, build_arrays.py): the base row's width, the second
//    row's place along and width, the third row's place
// 10 the third row's width, the tip row's width, the bark texture's layer, the lichen on the old wood (0..1)
// 11 the leaf texture's square over the element: its length, its width, how far behind the element's base it starts
//    (a share of the element's length: the petiole and the margin the generator's element leaves out); w the
//    coverage of the clusters' leaf layer (its leaf's share of its square)
// 12 the same for the spray card; w the share of the bark's relief and detail the young growth keeps
// 13 the bark texture's tint (linear rgb) and its contrast about the tile's mean (a power of the luminance ratio)
// 14 the prickles' or thorns' colour (linear rgb; black: the young twig's), the bark tile's size over 6 cm
// 15 the young growth's flush on its upper, sunlit side (linear rgb) and its share there
// 16 the species' mean leaf element by area over its templates (m2: a leaf, a compound leaf, a spray, a needle tuft),
//    the far parts' value and chroma on their open, light top against the near leaves; trees: the radius (m) under
//    which the wood wears the young twig's skin (0 none)
// 17 trees: the upper bark's layer (-1 none), the zone it takes over in (two heights above the root, or two radii:
//    w 1 = by the wood's age, its radius)
// 18 the share of its leaves a sapling keeps withered through the winter (trees: marcescence, young oaks, hornbeam),
//    the bark tiles' stretch along the stem (1 square), the upper bark's tile size over 6 cm and its contrast (row 14
//    w's and row 13 w's when it has none of its own)
// 19 trees: the wood a cut end shows - the sapwood's colour (linear rgb), the heartwood's share of the radius
// 20 the heartwood's colour (linear rgb: the sapwood's where it has none), a year's ring width (m)
// 21 trees: the spray cards' value and chroma against the tiers beside them, the upper bark's value and chroma
//    against its layer's
float4	shrub_prof	(uint s, uint r)	{ return shrub_t4(SHRUB_T_PROF + s * SHRUB_PROF_F4 + r); }

// ---- Textures (tools/shrubs/textures: our own procedural sets, one layer a leaf or a bark) -----------------
// A leaf layer: u across the leaf (the midrib at 0.5, the widest part spanning the width), v along it (0 the
// petiole's base, 1 the tip), drawn at the species' proportions and stretched to the leaf's length and width.
// A bark layer: a 6 cm tile of the mature bark, u round the stem, v along it, tiling both ways.
Texture2DArray	s_shrub_leaf_d;		// r upper luminance x 0.5, g veins (1 midrib, rachis, a card's twig), b transmission, a coverage
Texture2DArray	s_shrub_leaf_n;		// rg the upper side's normal, b gloss, a underside luminance x 0.5
Texture2DArray	s_shrub_bark_d;		// rgb albedo (gamma), a cavity
Texture2DArray	s_shrub_bark_n;		// rg normal, b gloss, a lichen mask
#define SHRUB_BARK_TILE		0.06f

// A leaf's layer: the species' leaf a or b by the leaf's hash.
float	shrub_leaf_layer	(uint s, float h)
{
	const float4 T = shrub_prof(s, 8);
	return frac(h * 5.37f) < 0.5f ? T.x : T.y;
}

// A tangent-space normal from a texture's rg (z positive).
float3	shrub_unpack_n	(float2 rg)
{
	const float2 n = rg * 2.0f - 1.0f;
	return float3(n, sqrt(saturate(1.0f - dot(n, n))));
}

// Season keys of species s at the day (da_shrubs.cpp fill_season):
//  0 leaf out (share of the leaves unfolded), leaf growth (the cohort's size), autumn (colour progress of the
//    bush), fallen (share of the leaves down)
//  1 bloom (share open), bloom stage (0 bud .. 1 open .. 2 over), fruit (share set), ripeness (0 .. 1 ripe .. 2
//    shrivelled)
//  2 catkin stage (0 none .. 1 closed .. 2 open .. 3 shed), catkin length (share of full), autumn spread (how
//    much of the colour wave the leaves' order spans), fruit kept after the leaf fall (share)
//  3 young leaf, 4 summer leaf, 5 autumn A, 6 autumn B, 7 dead leaf, 8 leaf underside (linear rgb)
//  9 bark, 10 young twig, 11 petals, 12 flower centre, 13 unripe fruit, 14 ripe fruit, 15 catkin; the colours' w 0
//    but 11's, the flower's size, and 13's, the fruit group's (m: a bloom element is the larger of the two,
//    da_shrub_bloom.vs draws each at its own); trees: 3's, 4's and 5's w the leaf out, growth and fallen share of row
//    0 the policy's spring lead ahead (dead_air_x64_trees.ltx young_lead days) while that day has more leaves on - in
//    spring -, else today's (shrub_seas_lead)
float4	shrub_seas	(uint s, uint r)	{ return shrub_t4(SHRUB_T_SEAS + s * SHRUB_SEAS_F4 + r); }

#define SHRUB_OUT_OVATE		0u	// hazel, alder buckthorn, bird cherry
#define SHRUB_OUT_ELLIPTIC	1u	// spindle, honeysuckle, dogwood
#define SHRUB_OUT_LANCEOLATE	2u	// willows
#define SHRUB_OUT_ROUND		3u	// goat willow, apple, aspen
#define SHRUB_OUT_LOBED		4u	// guelder rose, hawthorn, currant
#define SHRUB_OUT_NARROW	5u	// purple and osier willows, broom

#define SHRUB_LEAF_SIMPLE	0u
#define SHRUB_LEAF_COMPOUND	1u
#define SHRUB_LEAF_NEEDLES	2u
#define SHRUB_LEAF_SPRAY	3u
// The leaves a near spray stands as (da_shrubs.cpp kSprayLeaves).
#define SHRUB_SPRAY_LEAVES	6u

// A leaf's flags (the leaf shaders' tex.w): its kind (SHRUB_LEAF_*), 8 if mirrored, 16 x its bite (0..15).
float	shrub_leaf_flags	(uint kind, float mirror, float hash)
{
	return float(kind) + (mirror < 0.0f ? 8.0f : 0.0f) + 16.0f * floor(frac(hash * 29.7f) * 16.0f);
}

// Insect bites on a few leaves in ten: one or two scalloped notches eaten from the margin, each leaf its own (the
// textures carry none: the same baked hole on every leaf of a species read as polka dots); the view and the shadow
// maps cut the same notches. uv: the leaf texture's coordinates, layer: its layer.
void	shrub_leaf_bite	(float2 uv, uint flags, float layer)
{
	const uint bite = flags >> 4;
	[branch] if (bite < 3u && (flags & 7u) != SHRUB_LEAF_NEEDLES)
	{
		const float bh = frac(float(bite) * 0.618f + layer * 0.137f);
		const float2 c0 = float2((bite & 1u) != 0u ? 0.07f : 0.93f, 0.3f + 0.5f * bh);
		const float2 c1 = c0 + float2((bite & 1u) != 0u ? -0.03f : 0.03f, 0.06f + 0.05f * bh);
		// (A circle on the leaf is an ellipse in its square: the square is about twice as long as wide.)
		const float2 k = float2(1.0f, 2.0f);
		const float r0 = 0.15f + 0.06f * bh, r1 = bite == 0u ? 0.12f : 0.0f;
		clip(min(length((uv - c0) * k) - r0, length((uv - c1) * k) - r1));
	}
}

// The self-shadowing the far shadow maps lose (their casters are clusters and blobs, the near cascade's own leaves end
// some fifteen metres out): an element darkens by its own shade inside the crown (its sky visibility, which goes
// with the sun it gets) as the bush leaves the near cascade - measured at 10 and 40 m, the same bushes read up to two
// fifths lighter far away, a tenth with this. light: the element's sky visibility, dist: its view depth (m).
float	shrub_far_shade	(float light, float dist)
{
	return lerp(1.0f, lerp(0.5f, 1.0f, saturate(light)), smoothstep(14.0f, 22.0f, dist));
}


// ---- Hashes and packing ----------------------------------------------------------------------------------
uint	shrub_hash	(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
float	shrub_u01	(uint x)	{ return float(x >> 8) * (1.0f / 16777216.0f); }
// A smooth value noise of a point (0..1): its lattice cell's eight corners hashed, blended by the smoothstep.
float	shrub_noise3	(float3 p)
{
	const float3 i = floor(p), f = p - i, u = f * f * (3.0f - 2.0f * f);
	const uint3 c = asuint(int3(i));
	const uint x0 = c.x * 73856093u, x1 = x0 + 73856093u, y0 = c.y * 19349663u, y1 = y0 + 19349663u;
	const uint z0 = c.z * 83492791u, z1 = z0 + 83492791u;
	const float4 lo = float4(shrub_hash(x0 ^ y0 ^ z0), shrub_hash(x1 ^ y0 ^ z0), shrub_hash(x0 ^ y1 ^ z0), shrub_hash(x1 ^ y1 ^ z0));
	const float4 hi = float4(shrub_hash(x0 ^ y0 ^ z1), shrub_hash(x1 ^ y0 ^ z1), shrub_hash(x0 ^ y1 ^ z1), shrub_hash(x1 ^ y1 ^ z1));
	const float4 m = lerp(lo, hi, u.z) * (1.0f / 4294967296.0f);
	const float2 e = lerp(m.xz, m.yw, u.x);
	return lerp(e.x, e.y, u.y);
}

// A zoned tree's boundary run at a point (the bark's rest position, pr; kt: its tongues' frequency by the species' tile):
// tongues a tile and more across, three times as tall, over a swell a metre and more across - the near bark's
// (da_shrub_wood.ps) and the far wood's (da_shrub_wood_far.ps) alike.
float	shrub_zone_run	(float3 pr, float kt)
{
	const float3 f1 = float3(3.0f * kt, 1.0f * kt, 3.0f * kt), f2 = float3(8.0f * kt, 2.6f * kt, 8.0f * kt);
	return saturate((shrub_noise3(pr * float3(0.8f, 0.3f, 0.8f) + 5.0f) * 0.4f + shrub_noise3(pr * f1) * 0.4f
		+ shrub_noise3(pr * f2 + 17.0f) * 0.2f - 0.5f) * 2.2f + 0.5f);
}
float2	shrub_h2	(uint v)	{ return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16)); }
float2	shrub_sn2	(uint v)
{
	const int2 q = int2(asint(v << 16) >> 16, asint(v) >> 16);
	return float2(q) / 32767.0f;
}
// A unit vector from its octahedral snorm16 pair (y up).
float3	shrub_oct	(uint v)
{
	const float2 e = shrub_sn2(v);
	float3 n = float3(e.x, 1.0f - abs(e.x) - abs(e.y), e.y);
	if (n.y < 0.0f)
		n.xz = (1.0f - abs(n.zx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.z >= 0.0f ? 1.0f : -1.0f);
	return normalize(n);
}
// The same at 12 bits a coordinate, the low 24 bits of v (da_shrubs.cpp oct12).
float3	shrub_oct12	(uint v)
{
	const float2 e = float2(float(v & 0xFFFu), float((v >> 12) & 0xFFFu)) * (2.0f / 4095.0f) - 1.0f;
	float3 n = float3(e.x, 1.0f - abs(e.x) - abs(e.y), e.y);
	if (n.y < 0.0f)
		n.xz = (1.0f - abs(n.zx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.z >= 0.0f ? 1.0f : -1.0f);
	return normalize(n);
}
// A leaf's side from its midrib and its roll (eight bits over a turn) from the level side across it (da_shrubs.cpp
// leaf_roll8).
float3	shrub_leaf_side	(float3 dir, uint roll8)
{
	const float3 c = abs(dir.y) < 0.99f ? normalize(cross(float3(0.0f, 1.0f, 0.0f), dir))
		: normalize(cross(dir, float3(1.0f, 0.0f, 0.0f)));
	float s, co;
	sincos((float(roll8 & 0xFFu) * (2.0f / 255.0f) - 1.0f) * 3.14159265f, s, co);
	return c * co + cross(dir, c) * s;
}

// ---- Records ---------------------------------------------------------------------------------------------
// An axis (32 bytes): a base xyz, w parent (24 bits, 0xFFFFFF none) | level (8: the level in the low six bits, 0x40 a
// fixed axis - a tree's stock wood, no bend of its own); b x direction (oct), y length | attach (f16), z flex |
// frequency (f16), w base radius (f16) | phase (16).
#define SHRUB_NO_AXIS		0xFFFFFFu
#define SHRUB_AXIS_FIXED	0x40u
struct shrub_axis
{
	float3	base;
	uint	parent;
	uint	level;
	bool	fixed;
	float3	dir;
	float	len;
	float	attach;
	float	flex;
	float	freq;
	float	r;
	float	phase;
};

shrub_axis	shrub_axis_load	(uint index)
{
	const uint4 a = da_shrub_axes.Load4(index * SHRUB_AXIS_REC);
	const uint4 b = da_shrub_axes.Load4(index * SHRUB_AXIS_REC + 16u);
	shrub_axis X;
	X.base = asfloat(a.xyz);
	X.parent = a.w & SHRUB_NO_AXIS;
	X.level = a.w >> 24;
	X.fixed = (X.level & SHRUB_AXIS_FIXED) != 0u;
	X.dir = shrub_oct(b.x);
	const float2 la = shrub_h2(b.y), ff = shrub_h2(b.z);
	X.len = la.x; X.attach = la.y; X.flex = ff.x; X.freq = ff.y;
	X.r = f16tof32(b.w & 0xFFFFu);
	X.phase = float(b.w >> 16) * (1.0f / 65536.0f);
	return X;
}

// An element (32 bytes): a xyz position (template space), w axis (16) | a (8) | b (8); b four words by kind:
//  wood    x direction (oct), y length | base radius, z tip radius | arclength s0, w level (8) | light (8) |
//          hash (16)
//  leaf    x midrib (oct), y roll (8) | crown normal (oct12), z length | width, w arclength (f16) | light (8) |
//          hash (8); a rank, b fall
//  bloom   x direction (oct), y size | arclength, z kind (8) | hash (24), w light (8); a rank
//  cluster x crown normal (oct), y radius | leaf area, z arclength (f16) | light (8) | blooms (8), w leaves (16) |
//          hash (16); a rank, b fall
//  blob    x crown normal (oct), y radius | leaf area, z bloom share (8) | light (8) | hash (16), w arclength on the
//          first stem (f16); a rank, b fall
// (light: the light the element gets past the crown, da_shrub_gen.cpp sky_visibility.)
// A tree's elements (the wide layout: a template of a million axes): w axis (24) | a (8); the leaves', spray cards',
// clusters' and blobs' x their direction or crown normal at twelve bits (oct12) | b (8); the wood's and blooms' x as
// above (a wood's a: bit 0 within its axis's junction's reach - the template's junction table names its record; a bloom
// has no b). Its own kinds:
//  stock   a centroid of its first ring; w axis (24) | flags (8: its level of detail); b x the first ring's aux slot,
//          y the next ring's, z light (8) | level (8: 0x80 a dead stretch of a limb, weathered grey) | hash (16), w
//          climbable (8: 0..255) | its junction record's first aux slot (24: 0 none; within the junction's reach)
//  patch   a its first vertex; w axis (24) | flags (8); b x, y, z the triangle's three vertices' aux slots, w light (8)
//          | weathered grey (bit 8: a cut end's cap, a dead stretch of a limb) | hash (16) << 16
struct shrub_elem
{
	float3	p;
	uint	axis;
	float	a, b;
	uint4	w;
};

shrub_elem	shrub_elem_load	(uint index, bool wide)
{
	const uint4 a = da_shrub_elems.Load4(index * SHRUB_REC);
	shrub_elem E;
	E.p = asfloat(a.xyz);
	E.w = da_shrub_elems.Load4(index * SHRUB_REC + 16u);
	[flatten] if (wide)
	{
		E.axis = a.w & SHRUB_NO_AXIS;
		E.a = float(a.w >> 24) / 255.0f;
		E.b = float(E.w.x >> 24) / 255.0f;
	}
	else
	{
		E.axis = a.w & 0xFFFFu;
		E.a = float((a.w >> 16) & 0xFFu) / 255.0f;
		E.b = float(a.w >> 24) / 255.0f;
	}
	return E;
}

// A leaf's midrib, a card's direction, a cluster's or a blob's crown normal from the record's x (wide: oct12).
float3	shrub_elem_dir	(shrub_elem E, bool wide)	{ return wide ? shrub_oct12(E.w.x) : shrub_oct(E.w.x); }

// The element's own hash (0..1) for its fate in a chunk, by kind.
float	shrub_elem_hash	(shrub_elem E, uint kind)
{
	if (kind == SHRUB_K_WOOD)
		return float(E.w.w >> 16) / 65536.0f;
	if (kind == SHRUB_K_STOCK)
		return float(E.w.z >> 16) / 65536.0f;
	if (kind == SHRUB_K_LEAF)
		return (float(E.w.w >> 24) + 0.5f) / 256.0f;
	if (kind == SHRUB_K_BLOOM)
		return float(E.w.z >> 8) / 16777216.0f;
	if (kind == SHRUB_K_CLUSTER)
		return float(E.w.w >> 16) / 65536.0f;
	if (kind == SHRUB_K_PATCH)
		return float(E.w.w >> 16) / 65536.0f;
	return float(E.w.z >> 16) / 65536.0f;
}

// An instance (SHRUB_INST_REC, 96 bytes): rows 0..2 of the template-to-world 3x4 transform; then x species (8) |
// flags (8: 1 dead, 2 a tree, 4 a Red Forest pine's dead rust-red needles kept all year) | phenology offset (8,
// signed: a share of the species' spread) | a young tree's spring lead (8: its share of the policy's), y the
// template's first axis, z hemi (8) | sun (8) | a tree's ground at its trunk over its root (16, signed cm), w the element
// sizes' scale (f16) | seed (16); then the wind at its root (heading, push) now and a frame back (f16 pairs); then a
// tree's trunk sway on its stock root's oscillator (da_trees.cpp record_tail): x the downwind push per metre now, y a
// frame back (f16 pairs), z the tree's height above its root | its climbable top (f16), w the template's first aux slot
// (24) | its youth (8).
struct shrub_inst
{
	float3x4	xf;
	uint		species;
	bool		dead;
	bool		tree;
	bool		red;		// a Red Forest pine: its dead needles on all year in the dead colour
	float		pheno;		// the plant's phenology offset (share of the season keys' order)
	float		lead;		// a young tree's spring lead (0 .. 1 of the trees' policy's: shrub_seas_lead)
	float		youth;		// a tree's youth (1 a sapling .. 0 an old tree: its height in the policy's young range)
	uint		axis0;
	float		hemi, sun;
	float		ground;		// a tree's ground at its trunk over its root (m): its bark's zones count from it
	float		scale;
	uint		seed;
	// The wind at its root (da_veg.cpp frame_record, da_wind_core.h's maths): the local heading, the pressure and the
	// gust lean, now and a frame back.
	float2		wdir, wdir_prev;
	float2		push, push_prev;
	// A tree's trunk sway (shrub_tree_sway): the push per metre of height now and a frame back, its height, the top of
	// its climbable wood, its first aux slot.
	float2		tpush, tpush_prev;
	float		tree_h, climb_top;
	uint		aux0;
};

shrub_inst	shrub_inst_load	(uint index)
{
	shrub_inst I;
	const uint base = index * SHRUB_INST_REC;
	I.xf = float3x4(asfloat(da_shrub_inst.Load4(base)), asfloat(da_shrub_inst.Load4(base + 16u)),
		asfloat(da_shrub_inst.Load4(base + 32u)));
	const uint4 m = da_shrub_inst.Load4(base + 48u);
	I.species = m.x & 0xFFu;
	I.dead = ((m.x >> 8) & 1u) != 0u;
	I.tree = ((m.x >> 8) & 2u) != 0u;
	I.red = ((m.x >> 8) & 4u) != 0u;
	I.pheno = float(asint(m.x << 8) >> 24) / 127.0f;
	I.lead = float(m.x >> 24) / 255.0f;
	I.axis0 = m.y;
	I.hemi = float(m.z & 0xFFu) / 255.0f;
	I.sun = float((m.z >> 8) & 0xFFu) / 255.0f;
	I.ground = float(asint(m.z) >> 16) * 0.01f;
	I.scale = f16tof32(m.w & 0xFFFFu);
	I.seed = m.w >> 16;
	const uint4 w = da_shrub_inst.Load4(base + 64u);
	I.wdir = shrub_h2(w.x);
	I.push = shrub_h2(w.y);
	I.wdir_prev = shrub_h2(w.z);
	I.push_prev = shrub_h2(w.w);
	const uint4 t = da_shrub_inst.Load4(base + 80u);
	I.tpush = shrub_h2(t.x);
	I.tpush_prev = shrub_h2(t.y);
	const float2 hc = shrub_h2(t.z);
	I.tree_h = hc.x; I.climb_top = hc.y;
	I.aux0 = t.w & 0xFFFFFFu;
	I.youth = float(t.w >> 24) / 255.0f;
	return I;
}

// The plant's place in the season as a share of the season keys' order: its own few days early or late (the species'
// spread between plants).
float	shrub_early	(shrub_inst I, uint sp)	{ return I.pheno * shrub_prof(sp, 7).x * (1.0f / 120.0f); }
// The species' season row 0 as a young tree has it (lead: its share of the policy's spring lead): its buds break and
// its leaves grow ahead of the old trees' (the phenological escape of the understorey saplings); autumn keeps the old
// trees' dates (the table's lead state is today's then).
float4	shrub_seas_lead	(uint sp, float lead)
{
	const float4 K0 = shrub_seas(sp, 0);
	[branch] if (lead <= 0.0f)
		return K0;
	return float4(lerp(K0.x, shrub_seas(sp, 3).w, lead), lerp(K0.y, shrub_seas(sp, 4).w, lead), K0.z,
		lerp(K0.w, shrub_seas(sp, 5).w, lead));
}
// The share of a tree's leaves that hang on withered through the winter (marcescence): its species' on a sapling, a
// twelfth of it on an old tree.
float	shrub_marcescent	(shrub_inst I, uint sp)	{ return I.tree ? shrub_prof(sp, 18).x * lerp(0.08f, 1.0f, I.youth) : 0.0f; }
// The far tiers' plant state in one interpolated value (da_shrub_mass.h shrub_mass_unpack): 0 alive, 1 dead, 2 a Red
// Forest pine's red needles, the marcescent share and the spring lead at eight bits each.
float	shrub_mass_state	(shrub_inst I, uint sp)
{
	const uint state = I.dead ? 1u : (I.red ? 2u : 0u);
	return float(state | (uint(round(saturate(shrub_marcescent(I, sp)) * 255.0f)) << 2) | (uint(round(I.lead * 255.0f)) << 10));
}

float3	shrub_world	(shrub_inst I, float3 p)	{ return mul(I.xf, float4(p, 1.0f)); }
float3	shrub_world_dir	(shrub_inst I, float3 d)	{ return mul((float3x3)I.xf, d); }
// A normal into the world: a tree's transform scales unevenly (the stock placement's), so its normals go through the
// inverse transpose (the cofactors of the 3x3, its determinant's sign kept); a shrub's as its directions.
float3	shrub_world_nrm	(shrub_inst I, float3 n)
{
	[branch] if (!I.tree)
		return mul((float3x3)I.xf, n);
	const float3 r0 = I.xf[0].xyz, r1 = I.xf[1].xyz, r2 = I.xf[2].xyz;
	const float3 c0 = cross(r1, r2), c1 = cross(r2, r0), c2 = cross(r0, r1);
	return float3(dot(c0, n), dot(c1, n), dot(c2, n)) * (dot(r0, c0) < 0.0f ? -1.0f : 1.0f);
}

// ---- The element of a vertex -------------------------------------------------------------------------------
// A chunk (16 bytes): x instance record, y first element, z count (8) | crossfade lo (8) | hi (8) | flags (8: 0x80
// rigid - its elements ride fixed axes, no bend; 0x40 the far path - a stock patch's bark without its runs; the low six
// k: the share 1 - k/63 of its parts kept past the floor, their cover made up - da_shrub_cluster_vs.h), w keep
// (f16): the share of the instance's elements the pixel floor kept (the survivors grow by 1/sqrt(keep) to hold the area)
// | its bend slot (16: the frame's chunks that bend numbered in order, da_veg.cpp number_bends - the rigid ones take
// none).
#define SHRUB_CHUNK_RIGID	0x80000000u
#define SHRUB_CHUNK_FAR		0x40000000u
#define SHRUB_SLOT_RIGID	0xFFFFFFFFu
struct shrub_vtx
{
	bool		valid;
	uint		corner;		// vertex of the element
	uint		index;		// element index
	shrub_elem	E;
	shrub_inst	I;
	float		keep;
	float		grow;		// size factor: the pixel floor's growth and the crossfade's shrink
	float		thin;		// the share of the parts kept past the floor (their cover made up: da_shrub_cluster_vs.h)
	float		hash;
	uint		slot;		// the element's slot in the frame's chunks (its bend record; SHRUB_SLOT_RIGID none)
	bool		far;		// the chunk's far path (SHRUB_CHUNK_FAR)
	float		aux;		// a rigid chunk's keep word's upper half (the trees' stock wood: its morphs, da_trees.cpp emit_stock)
};

// A draw of the trees' GPU cut: its elements are in the view and over the wire rule already (the vertex shaders' own
// tests would only run again).
bool	shrub_cut_draw	()	{ return da_shrub_chunk0.z > 0.5f; }

shrub_vtx	shrub_vertex	(uint vid, uint iid, uint kind)
{
	shrub_vtx V = (shrub_vtx)0;
	const uint per = uint(da_shrub_draw.x);
	uint chunk = uint(da_shrub_chunk0.x) + iid, local = vid / per;
	V.corner = vid - local * per;
	// A draw of the trees' GPU cut (da_tree_cut.cs): an instance's elements are words of its draw's list - the chunk (18
	// bits), the element in it (6) and its crossfade's shrink (8: the cut decided its window); all ones the list's tail.
	float cut_fade = -1.0f;
	[branch] if (da_shrub_chunk0.z > 0.5f)
	{
		const uint word = da_shrub_chunks.Load(SHRUB_CUT_WORDS + (uint(da_shrub_chunk0.x) + iid * SHRUB_CHUNK + local) * 4u);
		if (word == 0xFFFFFFFFu)
			return V;
		chunk = word & 0x3FFFFu;
		local = (word >> 18) & 0x3Fu;
		cut_fade = float(word >> 24) / 255.0f;
	}
	const uint4 C = da_shrub_chunks.Load4(chunk * 16u);
	V.slot = (C.z & SHRUB_CHUNK_RIGID) != 0u ? SHRUB_SLOT_RIGID : (C.w >> 16) * SHRUB_CHUNK + local;
	V.aux = (C.z & SHRUB_CHUNK_RIGID) != 0u ? f16tof32(C.w >> 16) : 0.0f;
	V.far = (C.z & SHRUB_CHUNK_FAR) != 0u;
	V.thin = 1.0f - float((C.z >> 24) & 0x3Fu) / 63.0f;
	if (local >= (C.z & 0xFFu))
		return V;
	V.index = C.y + local;
	V.I = shrub_inst_load(C.x);
	V.E = shrub_elem_load(V.index, V.I.tree);
	V.keep = max(f16tof32(C.w & 0xFFFFu), 1e-3f);
	V.hash = shrub_elem_hash(V.E, kind);
	// The tier's crossfade: the elements whose hash is inside [lo, hi) stand; the ones crossing an end shrink
	// away over a tenth of the hash range instead of popping (exact motion vectors, no screen-door dither).
	const float lo = float((C.z >> 8) & 0xFFu) / 255.0f, hi = float((C.z >> 16) & 0xFFu) / 255.0f;
	const float fade = cut_fade >= 0.0f ? cut_fade
		: saturate((V.hash - lo) * 10.0f + (lo <= 0.0f ? 1.0f : 0.0f)) * saturate((hi - V.hash) * 10.0f + (hi >= 1.0f ? 1.0f : 0.0f));
	V.grow = rsqrt(V.keep) * fade;
	V.valid = fade > 0.0f;
	return V;
}

// ---- Wind ------------------------------------------------------------------------------------------------
// The bend of a point attached to axis `a` at arclength s (template space p), in world space. Every axis of the
// chain (shoot, twig, branch, stem) turns rigidly about its base by its own angle at the attachment of the next
// one in (Pivot Painter's scheme): a cantilever's deflection at a share u of its length is about its tip angle
// times sqrt(u) seen from the base. The angle is the axis's flex times the wind's pressure: the field's strength
// and gust tongue at the bush's root, a static downwind lean and the da_sway waveform at the axis's own natural
// frequency and phase, the gust lean on top; the motors (bodies, shots, blasts) push each axis from its base,
// the lever its length. Applied from the innermost axis out, each about its rest base, the chain composes like a
// skeleton; the normal and the tangent of the point turn with it.
struct shrub_wind
{
	float2	dir;		// local downwind heading (world xz)
	float	pressure;	// the field's strength x the gust tongue (0 .. ~1.6)
	float	lean;		// gust lean 0..1
	float	time;
	float4	field;
	bool	prev;
	bool	motors;
};

// The wind of bush I now or a frame back: its root's field from its record (the CPU evaluates the gust field once a
// bush, da_shrubs.cpp frame_record), the strength, the clock and the motors from the frame's constants.
shrub_wind	shrub_wind_at	(shrub_inst I, bool prev)
{
	shrub_wind W;
	W.field = prev ? da_wind_field_prev : da_wind_field;
	W.prev = prev;
	W.time = prev ? da_shrub_time.y : da_shrub_time.x;
	W.dir = prev ? I.wdir_prev : I.wdir;
	const float2 push = prev ? I.push_prev : I.push;
	W.pressure = push.x;
	W.lean = push.y;
	W.motors = da_wm_count(prev) > 0;
	return W;
}

float3	shrub_rotate	(float3 v, float3 k, float s, float c)
{
	return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0f - c));
}

// ---- Oscillators -----------------------------------------------------------------------------------------
// The clock (da_shrub_time.xy) is the frame times summed in double and wrapped at 64 s: a float count of seconds
// since the start keeps whole milliseconds only after a couple of hours, and every leaf and twig would then step
// instead of move. A frequency is held to a whole number of cycles per 64 s, so the wrap turns over seamlessly.
float	shrub_fq	(float f)	{ return round(f * 64.0f) * (1.0f / 64.0f); }

// da_sway (da_wind_core.h) at f Hz from phase p (cycles), each harmonic on its own held frequency.
float	shrub_sway	(float t, float f, float p)
{
	const float a = shrub_fq(f), b = shrub_fq(f * 1.731f), c = shrub_fq(f * 3.09f);
	return 0.62f * sin(6.2831853f * (a * t + p)) + 0.28f * sin(6.2831853f * (b * t + 1.731f * p) + 1.3f)
		+ 0.10f * sin(6.2831853f * (c * t + 3.09f * p) + 4.1f);
}

// da_flutter (da_wind_core.h) the same way.
float	shrub_flut	(float t, float f, float p)
{
	const float a = shrub_fq(f), b = shrub_fq(f * 1.618f);
	return 0.60f * sin(6.2831853f * (a * t + p)) + 0.40f * sin(6.2831853f * (b * t + 1.618f * p) + 0.9f);
}

// A leaf's flutter (pitch and roll about its petiole, radians). The wind runs through a crown in eddies carried
// downwind (frozen turbulence): the leaves within one beat in step and its strength swells and fades as it passes,
// so the wind is seen running through the bush and its shadow shifts in patches - leaves each on a random phase
// boiled into flickering sun flecks. A leaf beats at its own frequency, phased by a field of about a hand's span
// round the bush's root (neighbours keep step) and by the time the wind takes to reach it; the eddies' strength is
// a pair of waves about two metres long crossing the crown, carried with it; the sheltered inner leaves move least.
// Analytic waves, not lattice noise: the leaves' vertex shader runs this twice for every vertex (this frame and the
// last). p: the leaf's petiole (world); amp: the species' flutter (rad); light: the leaf's exposure.
float2	shrub_flutter	(float3 p, float3 root, float hash, float amp, float light, shrub_wind W)
{
	const float3 r = p - root;
	const float strength = saturate(W.field.z);
	// The eddies ride downwind at the wind's speed (m/s).
	const float speed = 1.5f + 4.0f * strength;
	const float t = W.time - dot(r.xz, W.dir) / speed;
	const float2 xz = float2(r.x + 0.6f * r.y, r.z - 0.4f * r.y);
	const float e1 = sin(dot(xz, float2(3.1f, 1.7f)) - 6.2831853f * shrub_fq(0.37f) * t);
	const float e2 = sin(dot(xz, float2(-1.3f, 2.9f)) - 6.2831853f * shrub_fq(0.53f) * t + 1.9f);
	const float eddy = saturate(0.5f + 0.3f * (e1 + e2));
	const float ph = 0.25f * (sin(dot(xz, float2(7.3f, 4.1f)) + 5.3f * r.y) + sin(dot(xz, float2(-4.7f, 6.9f)) - 3.7f * r.y))
		+ hash * 0.15f;
	const float f = lerp(1.8f, 3.4f, hash);
	const float a = amp * W.pressure * (0.4f + 0.6f * strength) * (0.3f + 0.9f * eddy) * lerp(0.45f, 1.0f, light);
	return float2(a * shrub_flut(t, f, ph), 0.7f * a * shrub_flut(t, f * 1.37f, ph + 0.31f));
}

// Quaternions (xyz, w) for the composed bends.
float4	shrub_qmul	(float4 a, float4 b)	{ return float4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz)); }
float3	shrub_qrot	(float4 q, float3 v)
{
	const float3 t = 2.0f * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}
// A unit vector as its octahedral snorm16 pair (the inverse of shrub_oct).
uint	shrub_oct_pack	(float3 n)
{
	const float l1 = abs(n.x) + abs(n.y) + abs(n.z);
	float2 p = l1 > 0.0f ? n.xz / l1 : float2(0.0f, 0.0f);
	if (n.y < 0.0f)
		p = (1.0f - abs(p.yx)) * float2(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
	const int2 q = int2(round(clamp(p, -1.0f, 1.0f) * 32767.0f));
	return (uint(q.x) & 0xFFFFu) | (uint(q.y) << 16);
}

// One axis's turn under the wind and the motors: its base (world), the axis it turns about and its tip angle (0:
// none); a point at a share u of the axis turns by A sqrt(u) about the base (a cantilever's deflection seen from its
// base).
void	shrub_axis_turn_of	(shrub_axis X, shrub_inst I, shrub_wind W, out float3 base, out float3 k, out float A)
{
	base = shrub_world(I, X.base);
	k = float3(0.0f, 1.0f, 0.0f);
	A = 0.0f;
	const float3 dirw = normalize(shrub_world_dir(I, X.dir));
	const float lenw = X.len * I.scale;
	// The wind's push at the tip (travel per metre of length): the mean lean plus the sway at the axis's
	// frequency, each bush on its own phase.
	const float mean = 0.45f + 0.35f * saturate(W.field.z);
	const float sway = mean + (1.0f - mean) * shrub_sway(W.time, X.freq, X.phase + float(I.seed) * (1.0f / 65536.0f));
	float2 push = W.dir * (X.flex * W.pressure * (sway + 0.5f * W.lean));
	// (A rigid axis - a prickle, a dead stub - rides its wood: no motor turns it.)
	[branch] if (W.motors && X.flex > 0.0f)
	{
		float press_w;
		push += da_wind_motors_bend_ex(base, lenw, W.prev, press_w) * (0.6f / max(lenw, 0.1f));
	}
	const float tip = length(push);
	[branch] if (tip < 1e-4f)
		return;
	// A soft limit on the tip angle (about 50 degrees).
	A = 0.87f * tanh(tip / 0.87f);
	const float3 w3 = float3(push.x, 0.0f, push.y) / tip;
	k = cross(dirw, w3);
	const float kl = length(k);
	k = kl > 1e-4f ? k / kl : float3(-w3.z, 0.0f, w3.x);
}

// The bend of a point attached to axis `axis` at arclength s by walking its chain of axes from its own out (the
// slots the bend buffer has no room for: a frame of more chunks than it grew to); seven at most (a shrub's five - a
// thorn on a shoot of a hawthorn's third level -, a tree's leaf on a filling shoot of its third grafted level on a
// stock limb), stopping at a tree's fixed stock axes (they move with the trunk's sway alone).
void	shrub_bend_chain	(inout float3 q, inout float3 n, inout float3 t, uint axis, float s, shrub_inst I, bool prev)
{
	const shrub_wind W = shrub_wind_at(I, prev);
	uint a = axis;
	float at = s;
	[loop] for (uint lvl = 0u; lvl < 7u; ++lvl)
	{
		const shrub_axis X = shrub_axis_load(I.axis0 + a);
		if (X.fixed)
			break;
		float3 base, k;
		float A;
		shrub_axis_turn_of(X, I, W, base, k, A);
		[branch] if (A > 0.0f)
		{
			float sn, cs;
			sincos(A * sqrt(saturate(at / max(X.len, 1e-3f))), sn, cs);
			q = base + shrub_rotate(q - base, k, sn, cs);
			n = shrub_rotate(n, k, sn, cs);
			t = shrub_rotate(t, k, sn, cs);
		}
		if (X.parent == SHRUB_NO_AXIS)
			break;
		at = X.attach;
		a = X.parent;
	}
}

// The bend of a point attached to axis `axis` at arclength s, its normal and its tangent turned along, as the frame's
// bend record of the element has it (da_shrub_bend.cs: the chain of axes from the stem out composed once an element,
// not again for every vertex of it): the element's own axis turns it by its angle at the point's share of the axis,
// then the turns of the axes it hangs from, composed into one rotation and a translation. prev: a frame back (the
// view's slots only).
void	shrub_bend	(inout float3 q, inout float3 n, inout float3 t, uint slot, uint axis, float s, shrub_inst I, bool prev)
{
	// (A rigid chunk's elements ride a tree's fixed stock axes: no bend of their own, the trunk's sway alone.)
	[branch] if (slot == SHRUB_SLOT_RIGID)
		return;
	const uint cap = uint(prev ? da_shrub_bend_cap.y : da_shrub_bend_cap.x);
	[branch] if (slot >= cap)
	{
		shrub_bend_chain(q, n, t, axis, s, I, prev);
		return;
	}
	const uint b = (prev ? uint(da_shrub_bend_cap.x) + slot : slot) * SHRUB_BEND_REC;
	const float4 Q = asfloat(da_shrub_bend.Load4(b));
	const float3 T = asfloat(da_shrub_bend.Load3(b + 16u));
	const uint2 kA = da_shrub_bend.Load2(b + 28u);
	const float A = asfloat(kA.y);
	[branch] if (A > 0.0f)
	{
		const shrub_axis X = shrub_axis_load(I.axis0 + axis);
		const float3 base = shrub_world(I, X.base), k = shrub_oct(kA.x);
		float sn, cs;
		sincos(A * sqrt(saturate(s / max(X.len, 1e-3f))), sn, cs);
		q = base + shrub_rotate(q - base, k, sn, cs);
		n = shrub_rotate(n, k, sn, cs);
		t = shrub_rotate(t, k, sn, cs);
	}
	q = shrub_qrot(Q, q) + T;
	n = shrub_qrot(Q, n);
	t = shrub_qrot(Q, t);
}

// A tree's trunk sway (docs/dead-air/TREES.md): the stock tree's bend (da_tree_bend.h) on its root's oscillator - the
// downwind push per metre that the CPU takes from FTreeVisual's wind state (da_trees.cpp record_tail) times the
// point's height and its share of the tree's (the stock trunk's profile), the tree rule's motors, the stock soft cap at
// half the height and the drop that keeps the length. On the climbable stock wood the sway is held to a few
// centimetres in a breeze (its collision does not move: a climber's handholds), the wood above it swaying on from
// there. h: the point's rest height above the root (world), q: its bent position; the same in every pass.
float3	shrub_tree_sway	(float3 q, shrub_inst I, float h, bool prev)
{
	const float2 push = prev ? I.tpush_prev : I.tpush;
	const float H = max(I.tree_h, 1.0f);
	const float hh = max(h, 0.0f);
	float2 d = push * (hh * saturate(hh / H));
	[branch] if (I.climb_top > 0.0f)
	{
		const float hc = min(hh, I.climb_top);
		const float2 dc = push * (hc * saturate(hc / H));
		const float wind_k = saturate(prev ? da_wind_field_prev.z : da_wind_field.z);
		const float cap = 0.03f * (1.0f + 4.0f * wind_k * wind_k);
		const float lc = length(dc);
		d += (lc > 1e-4f ? dc * (cap * tanh(lc / cap) / lc) : dc) - dc;
	}
	[branch] if (da_wm_count(prev) > 0)
		d += da_tree_motors_bend_ex(q, float3(I.xf[0].w, I.xf[1].w, I.xf[2].w), hh, H, saturate(hh / H), prev);
	const float len = length(d);
	const float lim = max(hh * 0.5f, 1e-3f);
	[branch] if (len > 0.001f)
		d *= lim * tanh(len / lim) / len;
	const float drop = hh - sqrt(max(hh * hh - dot(d, d), 0.0f));
	return q + float3(d.x, -drop, d.y);
}

// The height of a world point above the instance's root (a tree's trunk sway).
float	shrub_root_height	(shrub_inst I, float3 p)	{ return p.y - I.xf[1].w; }

// Is an element out of the view with all its reach - its own size and how far the wind and the motors can carry it
// (the bend's tip angles are held under fifty degrees: a point h metres up its chain moves at most some 0.9 h)? The
// main pass only (the cascades see other parts of the bush). A camera inside a big bush leaves half its leaves
// behind it, and a near willow's sprays ran their whole chain of bends for every vertex of them all.
bool	shrub_out_of_view	(shrub_inst I, float3 p, float size)
{
	const float3 root = float3(I.xf[0].w, I.xf[1].w, I.xf[2].w);
	const float r = 0.3f + size + 0.9f * max(p.y - root.y, 0.0f);
	const float4 c = mul(m_WVP, float4(p, 1.0f));
	// A side plane's distance in clip units is the world's times the length of its normal (sqrt(P^2 + 1) of the
	// projection's scale on that axis: a scope's narrow field of view makes it many times the world's).
	const float mx = r * sqrt(m_P._11 * m_P._11 + 1.0f), my = r * sqrt(m_P._22 * m_P._22 + 1.0f);
	return c.w < -r || c.x - c.w > mx || -c.x - c.w > mx || c.y - c.w > my || -c.y - c.w > my;
}

// ---- Shared shading --------------------------------------------------------------------------------------
// The G-buffer holds albedo the way the stock textures store it (sRGB-encoded); the keys are linear.
float3	shrub_to_albedo	(float3 linear_rgb)	{ return pow(max(linear_rgb, 0.0f), 1.0f / 2.2f); }

// The sky light a leaf's face takes (the G-buffer's hemi; hmodel's sky term does not look at the normal): a face
// turned down sees the ground under the crown, in the crown's own shade, some three tenths of the sky an upward face
// sees. Lit by the whole sky, a white poplar's felted undersides - all a crown shows from below - read white over
// every tree of it.
float	shrub_face_hemi	(float hemi, float3 n_view)
{
	return hemi * lerp(1.0f, 0.3f, saturate(-dot(n_view, da_shrub_up.xyz)));
}

// The light inside a crown (x, the G-buffer's occlusion): the sky an element sees past the leaves (da_shrub_gen.cpp
// sky_visibility, baked with every leaf on and grown), the crown thinned to the leaf area on now (its optical depth
// goes with its leaf area: the visibility to the power of the share of the leaves out, grown and not fallen; a bare
// or dead bush's wood sees the whole sky), over the light the leaves scatter onto each other. That floor goes with
// the leaves' albedo (a pale or a yellow crown is lighter inside), and the scattered light went through and off
// leaves: it is their colour, so its share of the element's light takes some of the leaf colour's chroma once more
// at the same luminance (yzw, the albedo's factor) - a crown's inside is a deep green, not the sky's grey-blue
// dimmed. The leaves alone under-shade a crown: its twigs and branches and the clumping round the shoots take a
// fifth more (calibrated against a pack of summer photos of bushes next to their meadows: a crown's median pixel a
// quarter of its lit leaves, two fifths of the meadow's).
float4	shrub_crown_light	(float vis, uint s, bool dead, float3 leaf)
{
	const float3 LUMW = float3(0.2126f, 0.7152f, 0.0722f);
	const float4 K0 = shrub_seas(s, 0);
	const float grown = saturate(K0.y);
	const float on = dead ? 0.0f : saturate(K0.x) * (1.0f - saturate(K0.w)) * grown * grown;
	const float sky = pow(max(vis, 1e-3f), on * 1.2f);
	const float y = max(dot(leaf, LUMW), 1e-4f);
	const float floor_k = 0.13f * clamp(y / 0.06f, 0.7f, 2.0f);
	const float ao = lerp(floor_k, 1.0f, sky);
	const float share = floor_k * (1.0f - sky) / ao;
	// Half the leaf's chroma again (the scattered light is partly the sky's that came straight through the gaps), at
	// most doubling a channel, at the albedo's luminance: with all of it a crown's shade was half again as saturated
	// as the photos' bushes'.
	const float3 tint = min(leaf / y, 2.0f);
	const float3 f = lerp(float3(1.0f, 1.0f, 1.0f), tint, 0.5f * share);
	return float4(ao, f * min(y / max(dot(leaf * f, LUMW), 1e-4f), 4.0f));
}

// Translucency only where the light comes through (da_grass_shape.h, grass_translucency).
float	shrub_translucency	(float tr, float3 to_cam)
{
	return lerp(DA_FOLIAGE_TR_MIN, tr, smoothstep(-0.15f, 0.55f, dot(-to_cam, da_shrub_sun.xyz)));
}

// The live greens: a little of the blade grass's extra chroma (the weathers' warm light and the colour grade take a
// quarter of a green's saturation), at the same luminance; not its push toward blue-green - a meadow mixes its greens
// with yellowing blades, straw and seed heads, a crown is one solid mass of its green, and with the push and all of
// the chroma on top of the photos' it read acid against the meadow, then cool and frosted on its sunlit top.
float3	shrub_live_green	(float3 c)
{
	const float y = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
	return max(y + (c - y) * 1.08f, 0.0f);
}

// The season colour of a leaf of species s: its unfolding age, its own colour wave in autumn (by its fall
// order: the first to fall colour first), the share of colour B by the leaf's hash and light, browning before the
// fall. x of the result's w: the leaf's state 0 live .. 1 dead (gloss, translucency).
float4	shrub_leaf_colour_k	(uint s, float4 K0, float rank, float fall, float light, float h, float pheno)
{
	const float4 K2 = shrub_seas(s, 2);
	const float4 P2 = shrub_prof(s, 2), P6 = shrub_prof(s, 6);
	const float3 young = shrub_seas(s, 3).rgb, summer = shrub_seas(s, 4).rgb;
	const float3 aut_a = shrub_seas(s, 5).rgb, aut_b = shrub_seas(s, 6).rgb, dead = shrub_seas(s, 7).rgb;
	// Young leaves are the flush colour for their first weeks out.
	const float age = saturate((K0.x + pheno - rank) * 4.0f);
	float3 c = lerp(young, shrub_live_green(summer), age);
	// The autumn wave: each leaf colours over its own window (K2.z of the bush's progress) starting where its fall
	// order puts it, the first to fall first.
	const float spread = max(K2.z, 0.02f);
	const float a = saturate((K0.z + pheno - fall * (1.0f - spread)) / spread);
	const float b_share = saturate(P6.x + P6.y * (light - 0.5f) + (frac(h * 7.31f) - 0.5f) * 0.6f);
	const float3 autumn = lerp(aut_a, aut_b, step(frac(h * 3.17f), b_share));
	c = lerp(c, autumn, smoothstep(0.0f, 0.6f, a));
	c = lerp(c, dead, smoothstep(1.0f - 0.4f * P6.w, 1.0f, a) * P6.w);
	// Per leaf a value and a hue step (the species' jitter).
	c *= 1.0f + P2.z * (frac(h * 13.7f) * 2.0f - 1.0f);
	c = lerp(c, c.gbr * float3(1.0f, 1.0f, 0.9f), P2.w * frac(h * 29.3f) * 0.5f);
	return float4(max(c, 0.0f), a);
}

float4	shrub_leaf_colour	(uint s, float rank, float fall, float light, float h, float pheno)
{
	return shrub_leaf_colour_k(s, shrub_seas(s, 0), rank, fall, light, h, pheno);
}

// The colour of a crown's middle leaf now, without any one leaf's draw: the young flush or the summer green, then
// colours A and B mixed at the species' share along the autumn wave (the light the crown scatters onto its wood and
// fruit; one leaf's hash picked colour B for every species and its value and hue jitter on top).
float3	shrub_leaf_colour_mean	(uint s)
{
	const float4 K0 = shrub_seas(s, 0), K2 = shrub_seas(s, 2);
	const float4 P6 = shrub_prof(s, 6);
	const float3 c0 = lerp(shrub_seas(s, 3).rgb, shrub_live_green(shrub_seas(s, 4).rgb), saturate((K0.x - 0.5f) * 4.0f));
	const float spread = max(K2.z, 0.02f);
	const float a = saturate((K0.z - 0.5f * (1.0f - spread)) / spread);
	const float3 autumn = lerp(shrub_seas(s, 5).rgb, shrub_seas(s, 6).rgb, saturate(P6.x));
	const float3 c = lerp(c0, autumn, smoothstep(0.0f, 0.6f, a));
	return max(lerp(c, shrub_seas(s, 7).rgb, smoothstep(1.0f - 0.4f * P6.w, 1.0f, a) * P6.w), 0.0f);
}

#endif // DA_SHRUB_COMMON_H
