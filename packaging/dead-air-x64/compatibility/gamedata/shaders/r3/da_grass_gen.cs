// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Blade grass, generation (docs/dead-air/GRASS.md; the GoT "compute 1"): one lane per point of a quincunx
// lattice whose density halves per doubling of the distance, 64 lanes per group, the tiles of the frame
// built on the CPU (da_grass_blades.cpp). A lane takes the community of its slot's objects (weighted by the
// stock palette), the species of that community at its point (clonal patches and scattered individuals),
// the ground of the stock slot-box rule, evaluates the season of its blade, survives by the
// coverage-preserving pixel rule, bends with the wind field, the wind motors and the push of every
// creature, and writes 64-byte records into the window of its distance/LOD bin: near to far, so the
// draws get early-Z. A grass leaf lane carries the folded basal pairs of its tuft and the litter of its
// stand (near the camera only), a stem its cauline leaves. Shadow casters of the near and middle sun
// cascades get records of their own.
// FXC SM5: no early return before the group barriers ('alive' instead), Store4 only.

#include "common.h"
#include "da_wind_field.h"
#include "da_detail_common.h"
#include "da_season_common.h"
#include "da_grass_common.h"
#include "da_water_field.h"

ByteAddressBuffer	da_grass_slots		: register(t0);	// level.details slots, 16 bytes each
ByteAddressBuffer	da_grass_groups		: register(t2);	// 16 bytes per group: tile, level, flags, lanes
Texture2D<float>	da_grass_ground		: register(t3);	// ground of the slot-box rule, toroidal window
Texture2D<float2>	da_grass_push		: register(t4);	// push of the creatures, toroidal window
Texture2D<float2>	da_grass_push_prev	: register(t5);	// the same, previous frame
RWByteAddressBuffer	da_grass_blades		: register(u0);
RWByteAddressBuffer	da_grass_counts		: register(u1);
RWByteAddressBuffer	da_grass_heads		: register(u2);

uniform float4	da_grass_cam;		// xyz camera, w draw radius R
uniform float4	da_grass_fwd;		// xyz camera forward, w minimum width per metre of view depth (k_px x pixel)
uniform float4	da_grass_lod;		// x d1, y d2, z end fade start, w 1 / (R - fade start)
uniform float4	da_grass_lat;		// x lattice spacing (m), y lattice density (1/m2), z density scale, w height setting
uniform float4	da_grass_time;		// x time, y previous time, z 1 = previous state wanted, w frame & 63
uniform float4	da_grass_sh;		// x near cascade reach, y middle reach, z near texel (m), w middle texel (m)
uniform float4	da_grass_slot;		// x offs_x, y offs_z, z size_x, w size_z
uniform float4	da_grass_gwin;		// ground window: xy first texel, z size (texels), w texel (m)
uniform float4	da_grass_pwin;		// push window: x 1/extent, y half extent, zw centre
uniform float4	da_grass_pwin_prev;
uniform float4	da_grass_snow;		// x 1 = snow lies (sample its depth), y QA: the species every lane grows + 1, z QA: r__dbg_grass

uint	grass_dbg	()	{ return uint(da_grass_snow.z + 0.5f); }
uniform float4	da_grass_planes[6];	// view frustum (world): outside when dot(n, p) + d > r
uniform float4	da_grass_bin_d[3];	// upper distance of the 9 view bins
uniform float4	da_grass_bin_base[4];	// first record of each bin
uniform float4	da_grass_bin_cap[4];	// records each bin holds
// x how far a culm rises per metre of water over its root (dead_air_x64_reeds.ltx emerge), y the most it rises (the
// deepest reed bed's: the blades' bounds count with it)
uniform float4	da_grass_water;
uniform float4	da_grass_wave[4];		// wave, dir2D of the two wind groups (deffer_detail_w_flat.vs)
uniform float4	da_grass_wave_prev[4];

groupshared uint	gs_n[GRASS_NBINS];
groupshared uint	gs_b[GRASS_NBINS];

float	grass_c	(float4 v, uint i)	{ return i == 0u ? v.x : (i == 1u ? v.y : (i == 2u ? v.z : v.w)); }
uint	grass_bin_base	(uint b)	{ return uint(grass_c(da_grass_bin_base[b >> 2], b & 3u)); }
uint	grass_bin_cap	(uint b)	{ return uint(grass_c(da_grass_bin_cap[b >> 2], b & 3u)); }
float	grass_bin_far	(uint b)	{ return grass_c(da_grass_bin_d[b >> 2], b & 3u); }

// Quincunx levels: L_2m = {g : g = 0 mod 2^m}, L_2m+1 = L_2m with (x>>m)+(y>>m) even; density 2^-l, nested.
uint	grass_exact_level	(int2 g)
{
	const uint m = min(firstbitlow(asuint(g.x) | (1u << 30)), firstbitlow(asuint(g.y) | (1u << 30)));
	const int2 h = g >> int(m);
	return min(2u * m + (((h.x & h.y) & 1) != 0 ? 1u : 0u), uint(GRASS_LMAX));
}

// Survival rank, uniform and nested: coarser points survive longest, P(u < f) = f.
float	grass_rank	(int2 g, float hsh)
{
	const uint l = grass_exact_level(g);
	return l < uint(GRASS_LMAX) ? exp2(-float(l + 1u)) * (1.0f + hsh) : exp2(-float(GRASS_LMAX)) * hsh;
}

bool	grass_visible	(float3 c, float r)
{
	[unroll] for (uint i = 0; i < 6; ++i)
	{
		if (dot(da_grass_planes[i].xyz, c) + da_grass_planes[i].w > r)
			return false;
	}
	return true;
}

// Survival of a lane under a keep, with the relative 8 % fade band and the lattice's supply cap
// (level: the tile's enumeration level). Returns the width factor that makes the expected coverage
// exact (0 = dead): the pixel widening, the shortage compensation and the band's mean.
float	grass_keep	(float keep, float u, uint level)
{
	const float B = 0.08f;
	const float cap = exp2(-float(level)) / (1.0f + B);
	const float k = max(min(keep, cap), 1e-6f);
	const float fade = saturate((k * (1.0f + B) - u) / (B * k));
	const float ek = (k * (1.0f + B) <= 1.0f) ? k * (1.0f + 0.5f * B)
		: k + (k * (1.0f + B) * (1.0f - k) - 0.5f * (1.0f - k * k)) / (B * k);
	return fade * (keep / k) * (k / max(ek, 1e-6f));
}

// The ground of the stock slot-box rule at xz from 4 point taps of the window: false outside the window,
// where a tap holds no ground, or across a step of more than 0.25 m (kerbs, slab edges).
bool	grass_ground	(float2 xz, out float y, out float3 n)
{
	const float texel = da_grass_gwin.w;
	const float2 f = xz / texel - 0.5f;
	const int2 i0 = int2(floor(f));
	const float2 w = f - float2(i0);
	const int size = int(da_grass_gwin.z);
	const int2 rel = i0 - int2(da_grass_gwin.xy);
	y = 0.0f;
	n = float3(0.0f, 1.0f, 0.0f);
	if (any(rel < 0) || any(rel + 1 >= size))
		return false;
	// The window is 1024 or 2048 texels: the toroidal wrap is a mask (two's complement covers negatives).
	const int2 t00 = i0 & (size - 1);
	const int2 t11 = (i0 + 1) & (size - 1);
	const float h00 = da_grass_ground.Load(int3(t00.x, t00.y, 0));
	const float h10 = da_grass_ground.Load(int3(t11.x, t00.y, 0));
	const float h01 = da_grass_ground.Load(int3(t00.x, t11.y, 0));
	const float h11 = da_grass_ground.Load(int3(t11.x, t11.y, 0));
	const float lo = min(min(h00, h10), min(h01, h11));
	const float hi = max(max(h00, h10), max(h01, h11));
	if (lo < -1e20f || hi - lo > 0.25f)
		return false;
	y = lerp(lerp(h00, h10, w.x), lerp(h01, h11, w.x), w.y);
	const float dx = ((h10 - h00) + (h11 - h01)) * 0.5f / texel;
	const float dz = ((h01 - h00) + (h11 - h10)) * 0.5f / texel;
	n = normalize(float3(-dx, 1.0f, -dz));
	return true;
}

float2	grass_push_at	(float2 xz, bool prev)
{
	const float4 win = prev ? da_grass_pwin_prev : da_grass_pwin;
	if (any(abs(xz - win.zw) > win.y))
		return 0.0f;
	const float2 uv = frac(xz * win.x);
	// Wrapping bilinear: the window is toroidal.
	return prev ? da_grass_push_prev.SampleLevel(smp_linear, uv, 0) : da_grass_push.SampleLevel(smp_linear, uv, 0);
}

// ---- Species of a lane -----------------------------------------------------------------------------
// Value noise of the cell lattice at q (0..1).
float	grass_vnoise	(float2 q, uint salt)
{
	const float2 f = floor(q);
	const int2 i = int2(f);
	float2 t = q - f;
	t = t * t * (3.0f - 2.0f * t);
	const float a00 = grass_u01(grass_seed(i, salt)), a10 = grass_u01(grass_seed(i + int2(1, 0), salt));
	const float a01 = grass_u01(grass_seed(i + int2(0, 1), salt)), a11 = grass_u01(grass_seed(i + int2(1, 1), salt));
	return lerp(lerp(a00, a10, t.x), lerp(a01, a11, t.x), t.y);
}

// A patch of clonal growth at xz: the cell of a grid one patch size apart under a warp of two incommensurate sine
// waves (most of a cell), so the cells are organic blobs, and the cell's own uniform value (so the shares of the
// patches are exact).
float	grass_patch	(float2 xz, float size, uint salt)
{
	const float2 q = xz / max(size, 0.25f);
	const float ph = float(salt & 1023u) * 0.0061359f;
	const float2 w = float2(sin(q.y * 1.37f + ph) + 0.5f * sin(q.x * 2.71f - ph), sin(q.x * 1.19f + 2.0f * ph) + 0.5f * sin(q.y * 2.33f + ph));
	return grass_u01(grass_hash(grass_seed(int2(floor(q + w * 0.4f)), salt) ^ 0xA7C3u));
}

// The species a lane of community c grows. The forbs first: a patch field of the community's forb patch size
// gives each forb its share of the area as clonal patches (goldenrod and nettle clones, lily of the valley
// carpets), where it takes its share of the lanes (and the grass the rest); everywhere each forb also takes a
// small share scattered as single plants. The other lanes are the graminoids: a share scattered as
// individuals (their own hash), the others in clonal patches, both by the graminoids' cover shares.
// The entry of a community's 16 cumulative shares (rows r0..r0+3, unused entries 2) that v falls in: the count of
// the shares at or below it, four vector compares instead of a loop.
uint	grass_cum_index	(uint c, uint r0, float v)
{
	const float4 one = float4(1.0f, 1.0f, 1.0f, 1.0f);
	return uint(dot(step(grass_comm(c, r0), v.xxxx), one) + dot(step(grass_comm(c, r0 + 1u), v.xxxx), one)
		+ dot(step(grass_comm(c, r0 + 2u), v.xxxx), one) + dot(step(grass_comm(c, r0 + 3u), v.xxxx), one));
}

uint	grass_species_pick	(uint c, float2 xz, uint seed)
{
	const uint4 ids = asuint(grass_comm(c, 3));
	const uint nf = min(ids.z, GRASS_COMM_FORBS);
	uint forb = 0xFFu;
	[branch] if (nf > 0u)
	{
		// A forb's clonal patch takes its share of the lanes there (one patch field per community, the cumulative
		// areas say whose patch a point lies in), looked up only for the lanes a patch could take; the scattered
		// forbs take their shares everywhere (their cumulative shares). Most lanes are grass and skip both.
		const float4 C5 = grass_comm(c, 5);
		const uint4 fid = asuint(grass_comm(c, 6));
		const float r_in = grass_u01(grass_hash(seed ^ 0xF0B5u));
		[branch] if (r_in < asfloat(ids.w) && C5.w > 0.0f)
		{
			const float v = grass_patch(xz, C5.x, 0x6F0Bu + c * 0x101u);
			[branch] if (v < C5.w)
			{
				const uint k = min(grass_cum_index(c, 11u, v), nf - 1u);
				if (r_in < grass_comm_f(c, 15u, k))
					forb = (fid[k >> 2] >> ((k & 3u) * 8u)) & 0xFFu;
			}
		}
		const float rf = grass_u01(grass_hash(seed ^ 0xF0B6u));
		[branch] if (forb == 0xFFu && rf < C5.z)
		{
			const uint k = min(grass_cum_index(c, 7u, rf), nf - 1u);
			forb = (fid[k >> 2] >> ((k & 3u) * 8u)) & 0xFFu;
		}
	}
	if (forb != 0xFFu)
		return forb;
	const float4 C0 = grass_comm(c, 0), C1 = grass_comm(c, 1), C2 = grass_comm(c, 2);
	const uint n = clamp(uint(C0.z + 0.5f), 1u, GRASS_COMM_SPECIES);
	const float r = grass_u01(grass_hash(seed ^ 0x5EC1u)) < C0.y ? grass_u01(grass_hash(seed ^ 0x5EC2u))
		: grass_patch(xz, C0.x, 0x7A7Cu + c * 0x1F3Du);
	const float cum[8] = { C1.x, C1.y, C1.z, C1.w, C2.x, C2.y, C2.z, C2.w };
	uint k = n - 1u;
	[unroll] for (int j2 = 7; j2 >= 0; --j2)
	{
		if (uint(j2) < n && r < cum[j2])
			k = uint(j2);
	}
	const uint word = k < 4u ? ids.x : ids.y;
	return (word >> ((k & 3u) * 8u)) & 0xFFu;
}

// A blade's season state (brief 4.3) from the keys of its species.
struct grass_life
{
	bool	thatch;
	float	sen;		// senescence 0 live .. 1 dead
	float	hrel;		// height share of the species' full height
	float	lodge;		// 0 upright .. 1 lodged
	float	stage;		// seed stage
	float	heads;		// share of headed culms
};

grass_life	grass_life_at	(uint p, bool culm, float h_thatch, float h_sen, float h_len, float h_lodge, float hc, bool prev)
{
	const float4 K0 = grass_seas(p, 0, prev), K1 = grass_seas(p, 1, prev), K3 = grass_seas(p, 3, prev);
	grass_life l;
	l.thatch = h_thatch < K0.w;
	const float thr = saturate(0.15f + 0.7f * lerp(h_sen, hc, 0.6f));
	l.sen = l.thatch ? 1.0f : saturate((1.0f - K0.z - thr) / 0.15f + 0.5f);
	l.hrel = l.thatch ? (culm ? lerp(0.4f, 0.9f, h_len) : lerp(0.25f, 0.5f, h_len)) : K0.x;
	const float lodging = l.thatch ? K1.x : K1.y * (1.0f - K0.z);
	const float lthr = lerp(h_lodge, hc, 0.5f);
	l.lodge = smoothstep(lthr - 0.06f, lthr + 0.06f, lodging);
	l.stage = l.thatch ? 1.0f : K1.w;
	l.heads = l.thatch ? K3.w : K1.z;
	return l;
}

// The tip of a blade (rest tilt and facing, length L) displaced by d (world xz, m): new facing and tilt, the
// length kept, never below 5 % of the length above its root. The blade turns its flat side toward where its tip
// leans by as much as it leans: near upright the tip's heading swings round at the least push, and a trampled blade
// rising back past upright as its push faded spun about its length in a frame. Within a twentieth of its length of
// upright it keeps its own facing, at a third it faces its lean (the tip's place is the same either way).
void	grass_lean	(inout float2 facing, inout float tilt, float L, float2 d)
{
	float s, c;
	sincos(tilt, s, c);
	float3 tip = float3(facing.x * s, c, facing.y * s) * L + float3(d.x, 0.0f, d.y);
	tip.y = max(tip.y, 0.05f * L);
	tip = normalize(tip);
	tilt = acos(saturate(tip.y));
	const float hl = length(tip.xz);
	if (hl > 1e-4f)
	{
		const float2 to = tip.xz / hl;
		const float turn = atan2(facing.x * to.y - facing.y * to.x, dot(facing, to)) * smoothstep(0.05f, 0.35f, hl);
		float st, ct;
		sincos(turn, st, ct);
		facing = float2(facing.x * ct - facing.y * st, facing.x * st + facing.y * ct);
	}
}

// The forces on a lane's blades at this or the previous frame: the tip's lean per metre of upright height
// (the stock tuft's wind field, waves and motors, the creatures' push, the slope), the wind strength and the
// push share. Every term grows with the height, so the basal leaves of a lane share their leaf's forces.
struct grass_force
{
	float2	lean;
	float	wind;
	float	push;
};

grass_force	grass_force_at	(float3 root, float hemi, uint grp, float stiff, float4 W, float2 downhill, float H, bool prev)
{
	const float4 wave = prev ? da_grass_wave_prev[grp * 2u] : da_grass_wave[grp * 2u];
	const float4 dir = prev ? da_grass_wave_prev[grp * 2u + 1u] : da_grass_wave[grp * 2u + 1u];
	const float4 field = prev ? da_wind_field_prev : da_wind_field;
	detail_wind w = detail_wind_at(root, hemi, field, wave, dir);
	// The wave, the gust lean and the bob follow the real wind (field.z, 0..1.25): the gust field's
	// amplitude never drops below 0.6, so without this a dead calm still rocked every blade. The motors stay
	// whole: a boot or a blast moves grass in any weather.
	const float calm_k = smoothstep(0.03f, 0.45f, field.z);
	w.flow.y *= calm_k;
	const float inten = H * w.dp * (0.05f + 0.95f * w.shelter) * w.flow.x * calm_k;
	// The stock tuft's bend at the tip (motors included, capped short of lying flat), softened by stiffness.
	float2 d = detail_bend_tail(root, w, H, 1.0f, inten, prev) * lerp(1.25f, 0.5f, stiff) * W.x;
	// The creatures' push: lean away along the push, harder for soft blades.
	const float2 push = grass_push_at(root.xz, prev);
	grass_force f;
	f.push = saturate(length(push));
	d += push * (H * 0.9f * lerp(1.0f, 0.7f, stiff) * W.z);
	// Downhill (GoT): blades on a slope lean down it.
	d += downhill * (H * W.w);
	f.lean = d / H;
	f.wind = saturate(w.flow.x * w.dp * (0.05f + 0.95f * w.shelter)) * calm_k * (1.0f - f.push);
	return f;
}

// A blade's shape under its lane's forces: the shape the vertex shader draws (it adds only the bob).
grass_shape	grass_apply	(grass_shape rest, grass_force f, float4 W)
{
	grass_shape s = rest;
	const float2 d = f.lean * max(s.height * cos(s.tilt), 0.02f);
	grass_lean(s.facing, s.tilt, s.height, d);
	s.bend += W.y * saturate(length(d) / max(s.height, 1e-3f)) + 0.4f * f.push;
	s.wind = f.wind;
	return s;
}


// Everything a surviving lane writes.
struct grass_lane
{
	bool		rec_v, rec_n, rec_m, headed, culm, thatch, cut, forb;
	uint		flags, head_type, cauline, basal, litter;
	uint		seed, species, comm;
	float3		root;
	float2		centre;
	float		d, wmin, base_raise;
	// Metres a reed bed's blade rises for the water over its root (the slot's emergence, da_grass_blades.cpp).
	float		emerge;
	// The lowest ground the lane may root on: a meadow object's stock box base where a reed bed took the slot's box
	// down to its bed under the water (-1e9: none).
	float		floor_y;
	grass_shape	cur, prev;
	grass_force	f_cur, f_prev;
	float		wv, wn, wm;
	float		sen, hc, ao, stage, snow, head_eq, head_area, head_len, head_w, height_k, hrel, end_fade, cauline_k;
	float2		nground;
};

// A basal pair as evaluated: its rest shapes now and on the previous frame, root, width scale, colour state.
struct grass_basal_rec
{
	grass_shape	rest, rest_prev;
	float3		root;
	float		ws, sen;
	uint		flags;
};

// The direction a lodged mat lies in at xz: wind, rain and the snow's load lay a stand down in swathes a few
// metres across, each combed one way (the angle turns smoothly with a value noise of 4 m cells), partly down
// the slope. A lodged blade at a random facing of its own made a mat of crossed sticks.
float2	grass_comb	(float2 xz, float2 downhill)
{
	const float a = grass_vnoise(xz * 0.25f, 0xC0B1u) * 12.566371f;
	return normalize(float2(cos(a), sin(a)) + downhill + 1e-5f);
}

// The k-th basal leaf pair of a leaf lane (GoT's dense base of short folded leaves): a short arching
// leaf of the tuft beside the lane's root, of its own cohort, size and facing (away from the clump
// centre), at the pixel rule like every blade and faded out from d1 to a third of the way to d2: at that
// grazing view the taller layer covers the base. False = not drawn.
bool	grass_basal	(grass_lane L, uint k, uint p, out grass_basal_rec R)
{
	const float4 P20 = grass_prof(p, 20), P21 = grass_prof(p, 21), P22 = grass_prof(p, 22);
	float h[12];
	{
		uint s = grass_hash(L.seed ^ (0xBA5A1u + k * 0x9E3779B9u));
		[unroll] for (uint i = 0; i < 12; ++i)
		{
			s = grass_hash(s + 0x6C8E9CF5u);
			h[i] = grass_u01(s);
		}
	}
	// Beside the lane's root on the local ground plane, pulled into the tuft.
	const float a = h[0] * 6.2831853f;
	float2 xz = L.root.xz + float2(cos(a), sin(a)) * (0.015f + 0.05f * h[1]);
	xz = lerp(xz, L.centre, 0.25f);
	const float ny = sqrt(saturate(1.0f - dot(L.nground, L.nground)));
	R.root = float3(xz.x, L.root.y - dot(L.nground, xz - L.root.xz) / max(ny, 0.3f), xz.y);

	// The youngest leaves of the tuft cure last: the base stays green into autumn.
	const float h_sen = lerp(h[3], 1.0f, 0.4f);
	const grass_life life = grass_life_at(p, false, h[2], h_sen, h[4], h[5], L.hc, false);
	const grass_life life_prev = grass_life_at(p, false, h[2], h_sen, h[4], h[5], L.hc, true);
	R.sen = life.sen;
	R.flags = ((L.seed + (k + 1u) * 0x5BD1u) & 0xFFFFu) | grass_species_bits(p) | GRASS_F_FOLD | GRASS_F_BASAL
		| (life.thatch ? GRASS_F_THATCH : 0u);
	const float hmax = lerp(P20.x, P20.y, h[6]) * L.height_k * L.end_fade;
	const float2 away = xz - L.centre;
	const float al = length(away);
	const float2 face = float2(cos(h[7] * 6.2831853f), sin(h[7] * 6.2831853f));
	R.rest.facing = normalize(lerp(face, al > 1e-4f ? away / al : face, 0.65f) + 1e-5f);
	if (life.lodge > 0.0f)
		R.rest.facing = normalize(lerp(R.rest.facing, grass_comb(xz, -L.nground * 2.0f), 0.8f * life.lodge) + 1e-5f);
	const float tilt = lerp(P21.x, P21.y, h[8]);
	R.rest.tilt = lerp(tilt, radians(80.0f), life.lodge);
	R.rest.bend = lerp(lerp(P21.z, P21.w, h[9]), 0.04f, life.lodge);
	R.rest.droop = lerp(P22.x, P22.y, h[10]) * (1.0f - life.lodge);
	R.rest.wind = 0.0f;
	R.rest.height = hmax * life.hrel;
	R.rest.width = lerp(P20.z, P20.w, h[10]);
	R.rest_prev = R.rest;
	R.rest_prev.tilt = lerp(tilt, radians(80.0f), life_prev.lodge);
	R.rest_prev.height = hmax * life_prev.hrel;
	R.ws = 0.0f;
	// Buried by snow, pressed under a patch of it like the lane's leaf, or too short to read.
	if (R.rest.height <= 0.01f || L.base_raise + R.rest.height * cos(R.rest.tilt) - L.snow <= 0.03f
		|| L.snow > lerp(0.01f, 0.04f, h[1]))
		return false;
	R.rest.bend += 0.6f * saturate(L.snow / R.rest.height);
	R.rest_prev.bend = R.rest.bend;
	const float fade = 1.0f - smoothstep(da_grass_lod.x, da_grass_lod.x + 0.35f * (da_grass_lod.y - da_grass_lod.x), L.d);
	R.ws = grass_keep(saturate(R.rest.width / L.wmin) * fade, h[11], 0u) * max(1.0f, L.wmin / R.rest.width);
	return R.ws > 0.0f;
}

// The k-th litter pair of a leaf lane: the felt of an old meadow - last years' leaves and straw lying matted
// on the ground under the stand, each pair combed with its swathe, weathered (the vertex shader colours it
// from the species' thatch and straw). Static, never in the shadows, inside d1 only (the lane decided the
// count: no snow on it).
void	grass_litter	(grass_lane L, uint k, out grass_shape S, out float3 root, out uint flags)
{
	const float4 C4 = grass_comm(L.comm, 4);
	float u[8];
	{
		uint s = grass_hash(L.seed ^ (0x1177Du + k * 0x9E3779B9u));
		[unroll] for (uint i = 0; i < 8; ++i)
		{
			s = grass_hash(s + 0x6C8E9CF5u);
			u[i] = grass_u01(s);
		}
	}
	const float a = u[0] * 6.2831853f;
	const float2 xz = L.root.xz + float2(cos(a), sin(a)) * (0.02f + 0.11f * u[1]);
	const float ny = sqrt(saturate(1.0f - dot(L.nground, L.nground)));
	// On the ground, under the tussock's lift.
	root = float3(xz.x, L.root.y - L.base_raise - dot(L.nground, xz - L.root.xz) / max(ny, 0.3f) + 0.004f, xz.y);
	const float fa = u[2] * 6.2831853f;
	S.facing = normalize(lerp(float2(cos(fa), sin(fa)), grass_comb(xz, -L.nground * 2.0f), 0.55f) + 1e-5f);
	S.tilt = radians(lerp(79.0f, 87.0f, u[3]));
	S.bend = lerp(0.02f, 0.08f, u[4]);
	S.droop = 0.0f;
	S.wind = 0.0f;
	S.height = lerp(C4.y, C4.z, u[5]) * L.height_k;
	S.width = C4.w * lerp(0.7f, 1.3f, u[6]);
	flags = ((L.seed + (k + 7u) * 0x2F1Bu) & 0xFFFFu) | grass_species_bits(L.species) | GRASS_F_FOLD | GRASS_F_LITTER;
	// Thinner towards d1, where the stand's under-layer of the ground tint takes over; at least the pixel floor.
	S.width = max(S.width * (1.0f - smoothstep(0.6f * da_grass_lod.x, da_grass_lod.x, L.d)), 0.0f);
	S.width = S.width > 0.0f ? max(S.width, L.wmin) : 0.0f;
}

// One lane of species p in community c: clump, ground, season, snow, keep, wind (h: the lane's 16 hashes).
void	grass_evaluate	(inout grass_lane L, int2 g, uint seed, float h[16], float2 xz, uint p, uint c, float hemi, uint level,
	uint tflags)
{
	const float4 P6 = grass_prof(p, 6);
	const bool culm = h[2] < P6.y;
	const bool folded = !culm && P6.w > 0.5f;
	L.culm = culm;
	L.seed = seed;
	L.species = p;
	L.comm = c;

	// Early out, before the species' tables are read: the most this lane could keep - its species' widest lane,
	// at the nearest its root can lie in view depth (a point in the frustum lies at least 0.45 of its distance
	// deep; the clump pull moves it 0.6 m at most), every density factor at its top, a leaf's basal cover at its
	// most - is below its rank: the pixel rule would drop it after all the work below. Most lanes of the middle
	// and far field end here.
	{
		const float dens_ub = P6.x * da_grass_lat.z * grass_seas(p, 2, false).w * grass_seas(p, 3, false).z
			/ max(da_grass_lat.y, 1e-3f) * (culm ? 1.0f : 3.0f);
		const float wide = grass_prof(p, 27).x;
		const float depth = max(0.45f * (distance(xz, da_grass_cam.xz) - 0.6f), 0.05f);
		float ub = (tflags & 1u) != 0u ? saturate(wide / (da_grass_fwd.w * depth)) * dens_ub : 0.0f;
		if ((tflags & 2u) != 0u)
			ub = max(ub, saturate(wide / (2.0f * da_grass_sh.z)) * dens_ub);
		if ((tflags & 4u) != 0u)
			ub = max(ub, saturate(wide / (3.0f * da_grass_sh.w)) * dens_ub);
		if ((grass_dbg() & 8u) == 0u && grass_rank(g, h[15]) >= min(ub, exp2(-float(level)) / 1.08f) * 1.08f)
			return;
		if ((grass_dbg() & 64u) != 0u)
			return;
	}
	const float4 P0 = grass_prof(p, 0), P1 = grass_prof(p, 1), P2 = grass_prof(p, 2), P3 = grass_prof(p, 3);
	const float4 P4 = grass_prof(p, 4), P5 = grass_prof(p, 5), P7 = grass_prof(p, 7);
	const float4 P8 = grass_prof(p, 8), P9 = grass_prof(p, 9), P10 = grass_prof(p, 10), P11 = grass_prof(p, 11);
	const float4 P13 = grass_prof(p, 13), P19 = grass_prof(p, 19), P22 = grass_prof(p, 22);
	const uint form = uint(P19.z + 0.5f);
	// A pinnate or dissected leaf: near the camera it takes the short bin, whose pixel shader cuts its lobes.
	L.cut = !culm && uint(P19.y + 0.5f) >= GRASS_LEAF_PINNATE;
	L.forb = form != GRASS_FORM_GRAMINOID;

	// The clump (GoT: a Voronoi cell of 3x3 jittered points), the species' own.
	float2 centre = xz;
	float cdist = 0.0f;
	uint cid = 0u;
	{
		const float cell = max(P5.x, 0.02f);
		const float2 cq = xz / cell, b = floor(cq);
		float best = 1e9f;
		float2 best_cell = b, best_pt = b;
		[unroll] for (int j = -1; j <= 1; ++j)
		{
			[unroll] for (int i = -1; i <= 1; ++i)
			{
				const float2 cc = b + float2(i, j);
				const uint hc2 = grass_seed(int2(cc), 0x2545F491u + p * 0x3B9Du);
				const float2 pt = cc + float2(grass_u01(hc2), grass_u01(grass_hash(hc2)));
				const float d2 = dot(cq - pt, cq - pt);
				if (d2 < best)
				{
					best = d2;
					best_cell = cc;
					best_pt = pt;
				}
			}
		}
		centre = best_pt * cell;
		cdist = sqrt(best) * cell;
		cid = grass_seed(int2(best_cell), 0x7F4A7C15u + p * 0x3B9Du);
	}
	const float hc = grass_u01(grass_hash(cid ^ 0x51u));
	L.hc = hc;
	// Bare gaps between tufts (sand: cover < 1).
	if (grass_u01(grass_hash(cid ^ 0xC0C0u)) >= P5.w)
		return;
	// Pull towards the clump centre (culms keep half of it: they stand inside the tuft).
	xz = lerp(xz, centre, P5.y * (culm ? 0.5f : 1.0f));

	float yG = 0.0f;
	float3 nG = float3(0.0f, 1.0f, 0.0f);
	if (!grass_ground(xz, yG, nG) || yG < L.floor_y)
		return;
	// The water over the root (the level's baked water field): no deeper than the species stands in - a reed a metre and
	// a half, a sedge a span, a meadow grass or a nettle a puddle's (a hand either way at the margin) -, and the culms rise
	// by what is over their own root (the reed bed's deepest water raised every culm of a slot, a shore's too). Without
	// the field the slot's own rise of a reed bed holds.
	[branch] if (da_water_map2.w > 0.5f)
	{
		const float4 wf = da_wf_sample(xz);
		const float wd = wf.y > 0.5f ? max(wf.x - yG, 0.0f) : 0.0f;
		if (wd > grass_prof(p, 27).y * lerp(0.75f, 1.25f, grass_u01(grass_hash(seed ^ 0x3A7Eu))))
			return;
		L.emerge = min(wd * da_grass_water.x, da_grass_water.y);
	}
	L.nground = nG.xz;
	// Tussocks lift the roots in a clump's centre (wet meadow, sedge marsh).
	const bool tussock = grass_u01(grass_hash(cid ^ 0x7055u)) < P9.z;
	const float base_raise = tussock ? lerp(P9.x, P9.y, hc) * saturate(1.0f - (cdist * cdist) / max(P9.w * P9.w, 1e-4f)) : 0.0f;
	const float3 root = float3(xz.x, yG + base_raise, xz.y);
	L.root = root;
	L.centre = centre;
	L.base_raise = base_raise;

	const float3 cam = da_grass_cam.xyz;
	const float d = distance(root, cam);
	L.d = d;
	const float hset = da_grass_lat.w;
	const float hbound = max(P0.y, P1.y + P10.z + 0.5f) * hset + L.emerge;
	const bool view = (tflags & 1u) != 0u && d < da_grass_cam.w
		&& grass_visible(root + float3(0.0f, 0.5f * hbound, 0.0f), 0.6f * hbound + 0.3f);
	const bool sh_near = (tflags & 2u) != 0u && d < da_grass_sh.x;
	const bool sh_mid = (tflags & 4u) != 0u && d < da_grass_sh.y;
	if (!view && !sh_near && !sh_mid)
		return;

	// The season of the blade (brief 4.3).
	const grass_life life = grass_life_at(p, culm, h[3], h[4], h[5], h[6], hc, false);
	const grass_life life_prev = grass_life_at(p, culm, h[3], h[4], h[5], h[6], hc, true);
	const float4 K0 = grass_seas(p, 0, false), K2 = grass_seas(p, 2, false), K3 = grass_seas(p, 3, false);
	const float shade = saturate((0.6f - hemi) / 0.3f);
	const float height_k = lerp(0.85f, 1.15f, lerp(0.5f, hc, P19.x)) * lerp(1.0f, P8.z, shade) * hset;
	// (The rise for the water is added at the blade's full height: a reed in a metre of water stands its own height
	// over the surface, as its growth follows the water level.)
	const float hmax = (culm ? lerp(P1.x, P1.y, h[7]) : lerp(P0.x, P0.y, h[7])) + L.emerge / max(height_k, 0.1f);
	const float end_fade = 1.0f - smoothstep(0.0f, 1.0f, saturate((d - da_grass_lod.z) * da_grass_lod.w));
	L.thatch = life.thatch;
	L.sen = life.sen;
	L.stage = life.stage;
	L.height_k = height_k;
	L.hrel = life.hrel;
	L.end_fade = end_fade;

	grass_shape rest;
	{
		const float2 tilt_range = culm ? P2.zw : P2.xy;
		const float2 bend_range = culm ? P3.zw : P3.xy;
		// Facing: random, partly the clump's, partly away from its centre (GoT).
		const float a = h[8] * 6.2831853f;
		float2 face = float2(cos(a), sin(a));
		const float ca = grass_u01(grass_hash(cid ^ 0xFACEu)) * 6.2831853f;
		face = normalize(lerp(face, float2(cos(ca), sin(ca)), P8.w) + 1e-5f);
		const float2 away = xz - centre;
		const float al = length(away);
		if (al > 1e-4f)
			face = normalize(lerp(face, away / al, P5.z) + 1e-5f);
		// A lodged blade lies with its swathe.
		if (life.lodge > 0.0f)
			face = normalize(lerp(face, grass_comb(xz, -nG.xz * 2.0f), 0.8f * life.lodge) + 1e-5f);
		rest.facing = face;
		rest.tilt = lerp(lerp(tilt_range.x, tilt_range.y, h[9]), radians(80.0f), life.lodge);
		// A lodged blade lies flat along the ground, not in an arch over it.
		rest.bend = lerp(lerp(bend_range.x, bend_range.y, h[10]), 0.04f, life.lodge);
		rest.droop = culm ? 0.0f : lerp(P4.x, P4.y, h[10]) * (1.0f - life.lodge);
		rest.wind = 0.0f;
		rest.height = hmax * life.hrel * height_k * end_fade;
		if (life.thatch && culm)
			rest.height *= lerp(1.0f, 0.7f, life.lodge);
		rest.width = culm ? lerp(P1.z, P1.w, h[11]) : lerp(P0.z, P0.w, h[11]);
	}
	grass_shape rest_prev = rest;
	rest_prev.tilt = lerp(lerp(culm ? P2.z : P2.x, culm ? P2.w : P2.y, h[9]), radians(80.0f), life_prev.lodge);
	rest_prev.height = hmax * life_prev.hrel * height_k * end_fade;
	if (life_prev.thatch && culm)
		rest_prev.height *= lerp(1.0f, 0.7f, life_prev.lodge);
	if (rest.height <= 0.01f)
		return;
	// The clump leans as one (GoT: shared direction), its culms the most.
	{
		const float lean = P22.w * grass_u01(grass_hash(cid ^ 0x1EA4u)) * (culm ? 1.0f : 0.5f);
		const float la = grass_u01(grass_hash(cid ^ 0xD1A5u)) * 6.2831853f;
		const float2 dl = float2(cos(la), sin(la)) * sin(lean);
		grass_lean(rest.facing, rest.tilt, 1.0f, dl);
		grass_lean(rest_prev.facing, rest_prev.tilt, 1.0f, dl);
	}

	// Seed head or inflorescence of a culm: the species' type and size, its share by the season.
	const uint head_type = clamp(uint(P10.x + 0.5f), 1u, 15u);
	const bool headed = culm && h[12] < P7.x * life.heads;
	const float head_len = headed ? lerp(P10.y, P10.z, h[13]) * hset : 0.0f;
	const float head_w = headed ? lerp(P11.x, P11.y, h[14]) : 0.0f;
	L.headed = headed;
	L.head_type = head_type;
	L.head_len = head_len;
	L.head_w = head_w;
	// A culm nods under its head as the seed ripens: the top arches over along its facing.
	[branch] if (headed)
	{
		const float heavy = saturate(head_len / max(0.2f * hset, 0.05f)) * lerp(0.6f, 1.4f, h[13]);
		const float nod = smoothstep(0.3f, 0.8f, life.stage) * heavy, nod_prev = smoothstep(0.3f, 0.8f, life_prev.stage) * heavy;
		grass_lean(rest.facing, rest.tilt, 1.0f, rest.facing * (0.12f * nod));
		grass_lean(rest_prev.facing, rest_prev.tilt, 1.0f, rest_prev.facing * (0.12f * nod_prev));
		rest.bend += 0.08f * nod;
		rest_prev.bend += 0.08f * nod_prev;
	}

	// Snow: buried blades are gone, the rest poke through (brief 3.8); wet early snow lays them down.
	float snow = 0.0f;
	[branch] if (da_grass_snow.x > 0.5f)
		snow = da_snow_depth(xz);
	L.snow = snow;
	if (base_raise + (rest.height + head_len) * cos(rest.tilt) - snow <= 0.03f)
		return;
	// Leaves and a lodged mat lie under a snow patch, pressed flat by its load, so only the stiff standing
	// culms pierce it, upright; the leaves thin out over the patch's last few centimetres instead of ending
	// on a contour line, and bow there under the snow's weight.
	if ((!culm || life.lodge > 0.5f) && snow > lerp(0.01f, 0.04f, grass_u01(grass_hash(seed ^ 0x5B0Du))))
		return;
	const float snow_bend = culm ? 0.0f : 0.6f * saturate(snow / rest.height);
	rest.bend += snow_bend;
	rest_prev.bend += snow_bend;

	// Keep: density x pixel rule, coverage preserved. Culms count with their head's area and their leaves'
	// (the far LOD carries both); density thins without width compensation, the pixel rule with it.
	const float dens = P6.x * da_grass_lat.z * K2.w * K3.z * lerp(1.0f, P8.y, shade);
	// (A folded profile's density counts its strips: one lane for two leaves.)
	const float dens_keep = dens / max(da_grass_lat.y, 1e-3f);
	const float4 P23 = grass_prof(p, 23);
	// A forb's dead stem of last year stands bare; a reed's keeps its sheaths.
	const uint leaves = (culm && !(life.thatch && form != GRASS_FORM_GRAMINOID)) ? min(uint(P6.z), GRASS_CAULINE_MAX) : 0u;
	const float leaf_a = float(leaves) * 0.25f * (P23.x + P23.y) * (P23.z + P23.w) * height_k * life.hrel
		* (form != GRASS_FORM_GRAMINOID ? lerp(1.0f, 0.55f, life.sen) : 1.0f);
	const float head_a = headed ? head_w * 0.6f * head_len : 0.0f;
	const float area_w = rest.width + (leaf_a + head_a) / max(rest.height, 0.05f);
	const float u = grass_rank(g, h[15]);
	// The pixel floor widens to 1.5 pixels from d2 to 2.5 d2: the far field's blades are fewer and wider (TAA and
	// the upscalers resolve them the same), its coverage stays exact.
	const float wmin = da_grass_fwd.w * max(dot(root - cam, da_grass_fwd.xyz), 0.05f)
		* lerp(1.0f, 1.5f, smoothstep(da_grass_lod.y, 2.5f * da_grass_lod.y, d));
	L.wmin = wmin;
	// Past its basal pairs (they fade out from d1 to halfway to d2) a leaf carries their share of the stand's
	// cover in the view, so the canopy's coverage runs on across the fade: a gap between the taller blades
	// looks into the stand's lower leaves, not onto the ground. Only as far as the ground faces the camera (a
	// hill at 10-40 m read as thin pale strokes over dark ground and forbs); at a grazing view the taller
	// layer covers the base anyway and nothing is added. The shadows keep the stand's own leaves: the basal
	// pairs cast none.
	float cover_k = 1.0f;
	[branch] if (!culm && P22.z > 0.0f)
	{
		const float4 P20 = grass_prof(p, 20);
		const float fade_b = smoothstep(da_grass_lod.x, da_grass_lod.x + 0.35f * (da_grass_lod.y - da_grass_lod.x), d);
		const float facing = saturate(abs(dot(cam - root, nG)) / max(0.25f * d, 1e-3f));
		// leaf area of the lane's basal pairs (P22.z pairs of two leaves) over its own (one leaf, two folded)
		const float basal = 2.0f * P22.z * (P20.x + P20.y) * (P20.z + P20.w) / max((P0.x + P0.y) * (P0.z + P0.w), 1e-6f)
			* (folded ? 0.5f : 1.0f);
		cover_k = 1.0f + min(basal, 2.0f) * fade_b * facing;
	}
	if (view)
		L.wv = grass_keep(saturate(area_w / wmin) * dens_keep * cover_k, u, level) * max(1.0f, wmin / area_w);
	// The casters keep 2 texels of the near cascade and 3 of the middle one: fewer, wider casters, the same
	// shadow coverage (da_grass_blades.cpp builds the shadow-only tiles for the same widths).
	const float tn = 2.0f * da_grass_sh.z, tm = 3.0f * da_grass_sh.w;
	if (sh_near)
		L.wn = grass_keep(saturate(area_w / tn) * dens_keep, u, level) * max(1.0f, tn / area_w);
	if (sh_mid)
		L.wm = grass_keep(saturate(area_w / tm) * dens_keep, u, level) * max(1.0f, tm / area_w);
	L.rec_v = L.wv > 0.0f;
	L.rec_n = L.wn > 0.0f;
	L.rec_m = L.wm > 0.0f;
	if (!L.rec_v && !L.rec_n && !L.rec_m)
		return;

	// Wind, push, slope: the shape of this and the previous frame.
	const float stiff = culm ? P4.w : P4.z;
	const float2 downhill = -nG.xz * saturate(1.0f - nG.y) * 4.0f;
	const uint grp = (seed >> 7) & 1u;
	const float H = max(rest.height * cos(rest.tilt), 0.02f);
	L.f_cur = (grass_force)0;
	[branch] if ((grass_dbg() & 128u) == 0u)
		L.f_cur = grass_force_at(root, hemi, grp, stiff, P13, downhill, H, false);
	L.cur = grass_apply(rest, L.f_cur, P13);
	L.f_prev = L.f_cur;
	L.prev = L.cur;
	// The previous frame's forces only where they move a blade by a pixel: past halfway to d2 a gust's lean changes
	// by under half a pixel a frame (the bob, the fast motion, still has its previous time in the vertex shader).
	[branch] if (da_grass_time.z > 0.5f && L.rec_v && d < 0.5f * (da_grass_lod.x + da_grass_lod.y) && (grass_dbg() & 128u) == 0u)
	{
		L.f_prev = grass_force_at(root, hemi, grp, stiff, P13, downhill, max(rest_prev.height * cos(rest_prev.tilt), 0.02f), true);
		L.prev = grass_apply(rest_prev, L.f_prev, P13);
	}

	L.ao = saturate(1.0f - 0.25f * saturate(1.0f - cdist / max(P5.x, 0.02f)));
	L.flags = (seed & 0xFFFFu) | grass_species_bits(p) | (culm ? GRASS_F_CULM : 0u) | (folded ? GRASS_F_FOLD : 0u)
		| (headed ? GRASS_F_HEAD : 0u) | (life.thatch ? GRASS_F_THATCH : 0u) | (culm ? head_type << 24 : 0u);
	// The far culm (a triangle from the root) carries the head and the stem's leaves: their share of its area
	// colours its top, their width equivalent widens it.
	L.head_area = headed ? saturate(head_a / max(area_w * rest.height, 1e-5f)) : 0.0f;
	L.head_eq = 2.0f * (head_a + leaf_a) / max(rest.height, 0.05f);
	// Stems carry their leaves as leaf records right after them up to d2 (they fade out over the last fifth
	// before it, where the culm widens into the far triangle); past d1 a forb's dozen leaves are four larger ones
	// of the same area (a leafy column still, at a third of the records; a triangle carrying them read as a
	// field of green paddles there). Grass leaves carry their basal pairs and the litter of the stand.
	const uint drawn = (form != GRASS_FORM_GRAMINOID && d >= da_grass_lod.x) ? min(leaves, 4u) : leaves;
	L.cauline = (d < da_grass_lod.y && (grass_dbg() & 16u) == 0u) ? drawn : 0u;
	L.cauline_k = sqrt(float(max(leaves, 1u)) / float(max(drawn, 1u)));
	const bool grass_leaf = !culm && form == GRASS_FORM_GRAMINOID && L.rec_v;
	L.basal = (grass_leaf && (grass_dbg() & 1u) == 0u) ? min(uint(P22.z + h[14]), GRASS_BASAL_MAX) : 0u;
	// The litter shows where the sward is thin: all of it in spring and autumn, a fifth under the summer canopy.
	const float litter = grass_comm(c, 4).x * lerp(1.0f, 0.2f, K0.y);
	L.litter = (grass_leaf && litter > 0.0f && snow <= 0.01f && d < da_grass_lod.x && (grass_dbg() & 2u) == 0u)
		? min(uint(litter + h[13]), GRASS_LITTER_MAX) : 0u;
}

// The basal pairs of a lane after its leaf (the evaluated, drawn ones).
void	grass_store_basal	(uint at, grass_basal_rec R, grass_lane L, uint p, float hemi)
{
	const float4 P13 = grass_prof(p, 13);
	grass_shape c = grass_apply(R.rest, L.f_cur, P13), q = grass_apply(R.rest_prev, L.f_prev, P13);
	c.width *= R.ws;
	q.width *= R.ws;
	da_grass_blades.Store4(at * GRASS_REC, uint4(asuint(R.root.x), asuint(R.root.y), asuint(R.root.z), R.flags));
	da_grass_blades.Store4(at * GRASS_REC + 16u, grass_pack_shape(c));
	da_grass_blades.Store4(at * GRASS_REC + 32u, grass_pack_shape(q));
	da_grass_blades.Store4(at * GRASS_REC + 48u, uint4(grass_pack_u8(float4(R.sen, L.hc, L.ao, hemi)),
		grass_pack_sn2(L.nground), grass_pack_h2(L.stage, L.snow), 0u));
}

// A litter pair after the lane's basal pairs: dead (senescence 1), static (this frame's shape on both).
void	grass_store_litter	(uint at, grass_shape S, float3 root, uint flags, grass_lane L, float hemi)
{
	da_grass_blades.Store4(at * GRASS_REC, uint4(asuint(root.x), asuint(root.y), asuint(root.z), flags));
	da_grass_blades.Store4(at * GRASS_REC + 16u, grass_pack_shape(S));
	da_grass_blades.Store4(at * GRASS_REC + 32u, grass_pack_shape(S));
	da_grass_blades.Store4(at * GRASS_REC + 48u, uint4(grass_pack_u8(float4(1.0f, L.hc, L.ao, hemi)),
		grass_pack_sn2(L.nground), grass_pack_h2(L.stage, L.snow), 0u));
}

// One record: the shapes with the width of its list, the colour state.
void	grass_store	(uint at, float wscale, grass_lane L, float hemi, grass_shape prev)
{
	grass_shape c = L.cur, q = prev;
	c.width *= wscale;
	q.width *= wscale;
	da_grass_blades.Store4(at * GRASS_REC, uint4(asuint(L.root.x), asuint(L.root.y), asuint(L.root.z), L.flags));
	da_grass_blades.Store4(at * GRASS_REC + 16u, grass_pack_shape(c));
	da_grass_blades.Store4(at * GRASS_REC + 32u, grass_pack_shape(q));
	da_grass_blades.Store4(at * GRASS_REC + 48u, uint4(grass_pack_u8(float4(L.sen, L.hc, L.ao, hemi)),
		grass_pack_sn2(L.nground), grass_pack_h2(L.stage, L.snow), grass_pack_h2(L.head_eq * wscale, L.head_area)));
}

// The slots [at0, at1) a lane reserved below its bin's cap but could not fill (it needs several consecutive
// records and they run past the cap): a zero shape, which the vertex shaders collapse, so the drawn prefix
// never holds a stale record.
void	grass_store_void	(uint at0, uint at1)
{
	for (uint i = at0; i < at1; ++i)
		da_grass_blades.Store4(i * GRASS_REC + 16u, uint4(0u, 0u, 0u, 0u));
}

// Leaves on a stem, rooted on its curve at its node and moving with it (the previous frame's root offset rides in
// the record): a reed's on alternate sides, a forb's in a spiral or in opposite pairs turning a quarter per node,
// from the first to the last node (shares of the stem), the upper ones smaller; a dying forb's leaves curl.
void	grass_store_cauline	(uint at0, uint count, float wscale, grass_lane L, uint p, float hemi, bool with_prev)
{
	const float4 P23 = grass_prof(p, 23), P24 = grass_prof(p, 24), P25 = grass_prof(p, 25), P19 = grass_prof(p, 19);
	const float knee = grass_prof(p, 7).w;
	float3 C0, C1, C2, C3, Q0, Q1, Q2, Q3, Sx;
	grass_ctrl(L.cur, L.root, 0.0f, knee, C0, C1, C2, C3, Sx);
	// (HLSL picks no structure by a condition.)
	grass_shape sq = L.cur;
	if (with_prev)
		sq = L.prev;
	grass_ctrl(sq, L.root, 0.0f, knee, Q0, Q1, Q2, Q3, Sx);
	const uint mode = uint(P25.y + 0.5f);
	const uint nodes = mode == 2u ? (count + 1u) >> 1 : count;
	const float phi0 = grass_u01(grass_hash(L.seed ^ 0xCA01u)) * 6.2831853f;
	// Dying forb leaves curl up. The leaves hand over to the far culm over the last fifth before they end: before
	// d2 in the view, a forb's halfway to d1 in the near sun cascade. Fewer leaves stand for a forb's dozen past d1
	// (cauline_k: the same area).
	const float to = with_prev ? da_grass_lod.y : (L.forb ? 0.5f * da_grass_lod.x : 1e9f);
	const float curl = (L.forb ? lerp(1.0f, 0.55f, L.sen) : 1.0f) * (1.0f - smoothstep(0.8f * to, to, L.d));
	const float grow = with_prev ? L.cauline_k : 1.0f;
	for (uint k = 0; k < count; ++k)
	{
		float u[6];
		{
			uint s = grass_hash(L.seed ^ (0xCA0Bu + k * 0x9E3779B9u));
			[unroll] for (uint i = 0; i < 6; ++i)
			{
				s = grass_hash(s + 0x6C8E9CF5u);
				u[i] = grass_u01(s);
			}
		}
		const uint node = mode == 2u ? k >> 1 : k;
		const float rel = nodes > 1u ? float(node) / float(nodes - 1u) : 0.5f;
		const float tn = lerp(P24.z, P24.w, rel);
		const float size = lerp(1.0f, P25.x, rel);
		float a;
		if (mode == 0u)
			a = phi0 + ((k & 1u) ? 3.14159265f : 0.0f) + (u[0] - 0.5f) * 0.6f;
		else if (mode == 1u)
			a = phi0 + float(k) * 2.3999632f + (u[0] - 0.5f) * 0.4f;
		else
			a = phi0 + float(node) * 1.5707963f + ((k & 1u) ? 3.14159265f : 0.0f) + (u[0] - 0.5f) * 0.3f;
		grass_shape lc;
		lc.facing = float2(cos(a), sin(a));
		lc.height = lerp(P23.x, P23.y, u[1]) * size * L.height_k * L.hrel * grow;
		lc.width = lerp(P23.z, P23.w, u[2]) * size * curl * wscale * grow;
		lc.tilt = lerp(P24.x, P24.y, u[3]);
		lc.bend = P25.z * lerp(0.7f, 1.3f, u[4]);
		lc.droop = P25.w * lerp(0.6f, 1.4f, u[5]);
		lc.wind = L.cur.wind;
		const float3 r = grass_bez(C0, C1, C2, C3, tn);
		const float3 dp = grass_bez(Q0, Q1, Q2, Q3, tn) - r;
		da_grass_blades.Store4((at0 + k) * GRASS_REC, uint4(asuint(r.x), asuint(r.y), asuint(r.z),
			((L.seed + k * 0x3779u) & 0xFFFFu) | grass_species_bits(p) | GRASS_F_CAULINE | (L.thatch ? GRASS_F_THATCH : 0u)));
		da_grass_blades.Store4((at0 + k) * GRASS_REC + 16u, grass_pack_shape(lc));
		da_grass_blades.Store4((at0 + k) * GRASS_REC + 32u, grass_pack_shape(lc));
		da_grass_blades.Store4((at0 + k) * GRASS_REC + 48u, uint4(grass_pack_u8(float4(L.sen, L.hc, L.ao, hemi)),
			grass_pack_sn2(L.nground), grass_pack_h2(L.stage, dp.y), grass_pack_h2(dp.x, dp.z)));
	}
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	if (gi < GRASS_NBINS)
		gs_n[gi] = 0u;
	GroupMemoryBarrierWithGroupSync();

	// ---- The lattice point of this lane --------------------------------------------------------
	const uint4 G = da_grass_groups.Load4(gid.x * 16u);
	const int2 tile = int2(asint(G.x), asint(G.y));
	const uint level = G.z & 0xFFu;
	const uint tsize = 32u << ((G.z >> 8) & 0xFu);
	const uint tflags = G.z >> 16;
	const uint lane = (G.w & 0xFFFFu) + gi;
	bool alive = lane < (G.w >> 16);

	const uint m = level >> 1;
	const int step = int(1u << m);
	const uint n = max(tsize >> m, 1u);
	int2 g;
	if ((level & 1u) == 0u)
		g = tile + int2(int(lane % n), int(lane / n)) * step;
	else
	{
		const uint halfn = max(n >> 1, 1u);
		const uint row = lane / halfn, q = lane % halfn;
		const int c = ((tile.x >> int(m)) + (tile.y >> int(m)) + int(row)) & 1;
		g = tile + int2(int(2u * q) + c, int(row)) * step;
	}

	const uint seed = grass_seed(g, 0x6A09E667u);
	float h[16];
	{
		uint s = seed;
		[unroll] for (uint k = 0; k < 16; ++k)
		{
			s = grass_hash(s + 0x9E3779B9u);
			h[k] = grass_u01(s);
		}
	}
	const float s0 = da_grass_lat.x;
	const float jit = 0.5f * s0 * exp2(0.5f * float(min(grass_exact_level(g), 6u)));
	const float2 xz = (float2(g) + 0.5f) * s0 + (float2(h[0], h[1]) * 2.0f - 1.0f) * jit;
	alive = alive && distance(xz, da_grass_cam.xz) < da_grass_cam.w;

	// ---- The community of the slot's objects at this point ---------------------------------------
	// The stock palette weighs the slot's objects at the point; one that grows blades is picked by its weight.
	// Their summed weight is the presence: near the camera, from the community's full-presence alpha on, the
	// stand grows at its full density (the stock rule thinned the blades by the palette like its cards, so a
	// meadow painted at alpha 0.4 grew 40 % of its grass), below it the stand thins out to its edge. From d1 to
	// d2 the presence returns to the palette's weight: out there a blade is a pixel stroke, the pixel rule widens
	// the survivors and the ground tint carries the stand's colour, so a full stand would only cost (it doubled
	// the records of the middle and far fields).
	const int2 sxz = int2(floor(xz * 0.5f)) + int2(da_grass_slot.xy);
	alive = alive && all(sxz >= 0) && sxz.x < int(da_grass_slot.z) && sxz.y < int(da_grass_slot.w);
	uint4 S = uint4(0u, 0u, 0u, 0u);
	if (alive)
		S = da_grass_slots.Load4((uint(sxz.y) * uint(da_grass_slot.z) + uint(sxz.x)) * 16u);
	uint comm = 0u;
	uint picked = 0u;
	{
		const uint ids[4] = { (S.x >> 20) & 63u, (S.x >> 26) & 63u, S.y & 63u, (S.y >> 6) & 63u };
		const float2 f = frac(xz * 0.5f);
		float a[4];
		uint cs[4];
		float total = 0.0f;
		[unroll] for (uint k = 0; k < 4; ++k)
		{
			const uint pal = ((k < 2u) ? S.z : S.w) >> ((k & 1u) * 16u);
			const float4 c4 = float4(pal & 15u, (pal >> 4) & 15u, (pal >> 8) & 15u, (pal >> 12) & 15u) / 15.0f;
			cs[k] = ids[k] != 63u ? grass_id_community(ids[k]) : 0u;
			a[k] = cs[k] != 0u ? lerp(lerp(c4.x, c4.y, f.x), lerp(c4.z, c4.w, f.x), f.y) : 0.0f;
			total += a[k];
		}
		[branch] if (total > 0.0f)
		{
			float r = grass_u01(grass_hash(seed ^ 0x51EDu)) * total;
			bool found = false;
			[unroll] for (uint k2 = 0; k2 < 4; ++k2)
			{
				// The last object with a weight takes the rounding remainder.
				if (!found && a[k2] > 0.0f)
				{
					comm = cs[k2];
					picked = k2;
				}
				if (!found && a[k2] > 0.0f && r < a[k2])
					found = true;
				r -= a[k2];
			}
			const float full = max(grass_comm(comm - 1u, 0).w, 0.05f);
			const float far_k = smoothstep(0.75f * da_grass_lod.x, da_grass_lod.y, distance(xz, da_grass_cam.xz));
			const float presence = (grass_dbg() & 32u) != 0u ? saturate(total) : lerp(saturate(total / full), saturate(total), far_k);
			alive = alive && grass_u01(grass_hash(seed ^ 0xD17Au)) < presence * grass_comm(comm - 1u, 5).y;
		}
	}
	alive = alive && comm != 0u;
	const uint c = alive ? comm - 1u : 0u;
	const uint pin = uint(da_grass_snow.y + 0.5f);
	const uint p = !alive ? 0u : (pin != 0u ? pin - 1u : ((grass_dbg() & 4u) != 0u ? (asuint(grass_comm(c, 3)).x & 0xFFu)
		: grass_species_pick(c, xz, seed)));
	const float hemi = float((S.y >> 16) & 15u) / 15.0f;

	// ---- The blade (only for a lane that is still alive) ----------------------------------------
	grass_lane L = (grass_lane)0;
	// A reed bed's object (the green bits' mask): its blades rise out of the water over their roots (red, 0.1 m).
	const bool bed = (((S.y >> 24) >> picked) & 1u) != 0u;
	L.emerge = bed ? float((S.y >> 20) & 15u) * 0.1f : 0.0f;
	// (A meadow object of a slot a bed took down: not below its stock box, the blue bits' drop over the box's base,
	// less a hand for the base's 0.2 m step.)
	L.floor_y = !bed && (S.y >> 28) != 0u ? float(S.x & 0xFFFu) * 0.2f - 200.0f + float(S.y >> 28) * 0.2f - 0.1f : -1e9f;
	[branch] if (alive)
		grass_evaluate(L, g, seed, h, xz, p, c, hemi, level, tflags);

	// ---- Bins -----------------------------------------------------------------------------------
	const uint lod = L.d < da_grass_lod.x ? 0u : (L.d < da_grass_lod.y ? 1u : 2u);
	uint bin_v = GRASS_VIEW_BINS - 1u;
	{
		const uint first = lod == 0u ? GRASS_BIN_LOD0 : (lod == 1u ? GRASS_BIN_LOD1 : GRASS_BIN_LOD2);
		const uint last = lod == 0u ? GRASS_BIN_LOD1 - 1u : (lod == 1u ? GRASS_BIN_LOD2 - 1u : GRASS_VIEW_BINS - 1u);
		bin_v = last;
		for (uint b = last; b > first; --b)
		{
			if (L.d < grass_bin_far(b - 1u))
				bin_v = b - 1u;
		}
		if (lod == 0u && L.cut)
			bin_v = GRASS_BIN_SHORT;
	}
	const uint cauline_v = L.rec_v ? L.cauline : 0u;
	const uint cauline_n = (L.rec_n && (!L.forb || L.d < 0.5f * da_grass_lod.x)) ? L.cauline : 0u;
	// The lane's basal pairs that are drawn, evaluated once; its litter pairs counted (stored below).
	grass_basal_rec BR[GRASS_BASAL_MAX];
	uint basal_v = 0u;
	[loop] for (uint bk = 0; bk < L.basal; ++bk)
	{
		grass_basal_rec r;
		if (grass_basal(L, bk, p, r))
		{
			BR[basal_v] = r;
			++basal_v;
		}
	}
	const uint litter_v = L.litter;
	const uint extra_v = cauline_v + basal_v + litter_v;
	// A near lane's short leaves (basal pairs, litter, a stem's leaves) take the 7-vertex short bin: thirteen
	// vertices drew a 15 cm basal leaf as finely as a 60 cm one.
	const uint bin_x = lod == 0u ? GRASS_BIN_SHORT : bin_v;
	const bool split = bin_x != bin_v && extra_v != 0u;
	const bool head_v = L.headed && L.rec_v && lod < 2u;
	const bool head_n = L.headed && L.rec_n;
	const uint bin_h = lod == 0u ? GRASS_BIN_HEAD0 : (L.d < 0.5f * (da_grass_lod.x + da_grass_lod.y) ? GRASS_BIN_HEAD1 : GRASS_BIN_HEAD1 + 1u);

	uint o_v = 0u, o_x = 0u, o_n = 0u, o_m = 0u, o_h = 0u, o_hn = 0u;
	if (L.rec_v)
		InterlockedAdd(gs_n[bin_v], split ? 1u : 1u + extra_v, o_v);
	if (L.rec_v && split)
		InterlockedAdd(gs_n[bin_x], extra_v, o_x);
	if (L.rec_n)
		InterlockedAdd(gs_n[GRASS_BIN_SH_NEAR], 1u + cauline_n, o_n);
	if (L.rec_m)
		InterlockedAdd(gs_n[GRASS_BIN_SH_MID], 1u, o_m);
	if (head_v)
		InterlockedAdd(gs_n[bin_h], 1u, o_h);
	if (head_n)
		InterlockedAdd(gs_n[GRASS_BIN_HEAD_SH], 1u, o_hn);
	GroupMemoryBarrierWithGroupSync();
	// One global atomic per bin and group.
	if (gi < GRASS_NBINS && gs_n[gi] != 0u)
	{
		uint first_record;
		da_grass_counts.InterlockedAdd(gi * 4u, gs_n[gi], first_record);
		gs_b[gi] = first_record;
	}
	GroupMemoryBarrierWithGroupSync();

	// ---- Records (the bins' windows are capacity guards) ------------------------------------------
	// A culm's head flag says the head pass draws its head: one that finds no room in its bin is carried by the
	// culm's far triangle instead (a forb's stem widens by it), so a field of flowers past the head budget keeps
	// its colour.
	const bool head_v_fits = head_v && o_h + gs_b[bin_h] < grass_bin_cap(bin_h);
	const bool head_n_fits = head_n && o_hn + gs_b[GRASS_BIN_HEAD_SH] < grass_bin_cap(GRASS_BIN_HEAD_SH);
	const uint flags_all = L.flags;
	uint culm_v = 0xFFFFFFFFu, culm_n = 0xFFFFFFFFu;
	L.flags = head_v_fits ? flags_all : flags_all & ~GRASS_F_HEAD;
	if (L.rec_v)
	{
		const uint at = grass_bin_base(bin_v) + gs_b[bin_v] + o_v;
		const uint n_main = split ? 1u : 1u + extra_v;
		// The extras follow the lane's own record, or open a run of their own in the short bin.
		uint ax = at + 1u;
		bool extras = !split;
		if (o_v + gs_b[bin_v] + n_main <= grass_bin_cap(bin_v))
		{
			grass_store(at, L.wv, L, hemi, L.prev);
			culm_v = at;
		}
		else
		{
			extras = false;
			if (o_v + gs_b[bin_v] < grass_bin_cap(bin_v))
				grass_store_void(at, grass_bin_base(bin_v) + grass_bin_cap(bin_v));
		}
		if (split)
		{
			ax = grass_bin_base(bin_x) + gs_b[bin_x] + o_x;
			extras = o_x + gs_b[bin_x] + extra_v <= grass_bin_cap(bin_x);
			if (!extras && o_x + gs_b[bin_x] < grass_bin_cap(bin_x))
				grass_store_void(ax, grass_bin_base(bin_x) + grass_bin_cap(bin_x));
		}
		[branch] if (extras)
		{
			grass_store_cauline(ax, cauline_v, L.wv, L, p, hemi, true);
			[loop] for (uint bj = 0; bj < basal_v; ++bj)
				grass_store_basal(ax + cauline_v + bj, BR[bj], L, p, hemi);
			[loop] for (uint lj = 0; lj < litter_v; ++lj)
			{
				grass_shape ls;
				float3 lr;
				uint lf;
				grass_litter(L, lj, ls, lr, lf);
				grass_store_litter(ax + cauline_v + basal_v + lj, ls, lr, lf, L, hemi);
			}
		}
	}
	L.flags = head_n_fits ? flags_all : flags_all & ~GRASS_F_HEAD;
	if (L.rec_n)
	{
		const uint at = grass_bin_base(GRASS_BIN_SH_NEAR) + gs_b[GRASS_BIN_SH_NEAR] + o_n;
		if (o_n + gs_b[GRASS_BIN_SH_NEAR] + 1u + cauline_n <= grass_bin_cap(GRASS_BIN_SH_NEAR))
		{
			grass_store(at, L.wn, L, hemi, L.cur);
			grass_store_cauline(at + 1u, cauline_n, L.wn, L, p, hemi, false);
			culm_n = at;
		}
		else if (o_n + gs_b[GRASS_BIN_SH_NEAR] < grass_bin_cap(GRASS_BIN_SH_NEAR))
			grass_store_void(at, grass_bin_base(GRASS_BIN_SH_NEAR) + grass_bin_cap(GRASS_BIN_SH_NEAR));
	}
	L.flags = flags_all & ~GRASS_F_HEAD;
	if (L.rec_m)
	{
		if (o_m + gs_b[GRASS_BIN_SH_MID] < grass_bin_cap(GRASS_BIN_SH_MID))
			grass_store(grass_bin_base(GRASS_BIN_SH_MID) + gs_b[GRASS_BIN_SH_MID] + o_m, L.wm, L, hemi, L.cur);
	}

	// Heads: the culm's record, the head's own size, stage and type (the head VS evaluates the culm's top).
	// A head whose culm found no room still fills its slot, with no culm (0xFFFFFFFF: collapsed).
	const uint head_bits = L.head_type | ((L.thatch ? 1u : 0u) << 4) | ((seed >> 16) << 16);
	if (head_v_fits)
		da_grass_heads.Store4((grass_bin_base(bin_h) + gs_b[bin_h] + o_h) * GRASS_HEAD_REC,
			uint4(culm_v, head_bits, grass_pack_h2(L.head_len, L.head_w * L.wv), grass_pack_h2(L.stage, L.hc)));
	if (head_n_fits)
		da_grass_heads.Store4((grass_bin_base(GRASS_BIN_HEAD_SH) + gs_b[GRASS_BIN_HEAD_SH] + o_hn) * GRASS_HEAD_REC,
			uint4(culm_n, head_bits, grass_pack_h2(L.head_len, L.head_w * L.wn), grass_pack_h2(L.stage, L.hc)));
}
