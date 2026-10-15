// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Procedural shrubs: the leaves of a cluster or a blob quad (docs/dead-air/SHRUBS.md), shared by the G-buffer and
// the shadow pixel shaders. The quad is a grid of cells, one leaf each - the species' own leaf texture (a compound
// leaf's leaflet) turned its own way, mirrored and jittered in its cell, the nearest leaf's centre deciding which
// leaf a pixel shows, its texture's coverage its outline - inside the part's round outline; a leaf stands where its own hash is under the part's
// cover (the Beer-Lambert cover of its leaf area) and the season keeps it: each takes a rank and a fall order
// spread round the part's mean and passes the same tests as a near leaf (out once the leaf-out passes its rank,
// down once the fall passes its fall order), so a bush changes the same way at every distance. Bare, the part
// shows its twigs: a short twig per cell turned its own way, at a quarter of the cover in the twig colour. The leaves
// clump: a cell's chance of a leaf follows a noise over a few cells round the part's cover. Leaves under three
// pixels gather into clumps of four, sixteen or sixty-four (da_shrub_cluster_vs.h: fewer, larger cells at the same
// cover), so a far part keeps a ragged leafy outline and its holes; only a part under nine pixels gives way to its
// outline at its mean cover (a clumpy blot of that share of its area, the cover times the leaves on): the two blend
// as distances, so the leaves widen into the blot with a sharp edge and no see-through haze between them.
#ifndef DA_SHRUB_MASS_H
#define DA_SHRUB_MASS_H

#include "da_shrub_common.h"

// A cell's hash (0..1) under a salt: the integer hash of its coordinates and the salt's bits (the sine hash it replaced
// took a quarter-rate sine a call, some sixty a pixel of a cluster: most of the clusters' and the blobs' cost).
float	shrub_m_hash	(float2 c, float salt)
{
	const int2 q = int2(floor(c));
	return shrub_u01(shrub_hash(uint(q.x) * 0x8da6b343u ^ uint(q.y) * 0xd8163841u ^ asuint(salt) * 0xcb1ab31fu));
}

struct shrub_mass
{
	float	cover;	// 0..1 at this pixel
	float	leaf;	// 1 a leaf, 0 a twig
	float	h;		// the leaf's hash
	float	rank, fall;
	float2	tilt;	// the leaf's lean in the quad (for the normal)
	float	bloom;	// 1 = a flower or fruit cell
	float4	tex;	// the leaf texture's detail at the pixel (s_shrub_leaf_d)
	float	hs;		// the hash of the leaf within a clump at the pixel (its own facing and colour step)
	float	withered;	// 1 a tree's marcescent leaf hanging on through the winter (the dead colour)
};

// The plant's state the vertex shader packed (shrub_mass_state): 0 alive, 1 dead, 2 a Red Forest pine's red needles;
// the marcescent share; the spring lead.
uint	shrub_mass_kind	(float4 misc)	{ return uint(round(misc.x)) & 3u; }
float2	shrub_mass_unpack	(float4 misc)
{
	const uint s = uint(round(misc.x));
	return float2(float((s >> 2) & 0xFFu), float(s >> 10)) / 255.0f;
}

// The part's frame, shared by its leaves and its mean blot: its ragged outline and the pixel's ramp inside it, the
// plant's season state and the share of its leaves on.
struct shrub_mass_frame
{
	float	r, rim, inside;
	bool	needle, dead_k, red_k, winter;
	float	marc, early, on_share;
	float4	K0;
};

// p: quad coordinates (-1..1); part: species, rank, fall, light; dens: cover, cells, the share of cells in flower or
// fruit, hash; misc: the plant's state (shrub_mass_state), phenology offset, blob, size (px, a shadow map's texels).
// The outline: a round part with a ragged rim (leaves stick out of it) - its radius at p, the rim there and the pixel's
// ramp inside it.
void	shrub_mass_outline	(float2 p, float4 part, float4 dens, float4 misc, out float r, out float rim, out float inside,
	out bool needle)
{
	r = length(p);
	// (Leaf clumps stick out of it at a few scales, smoothly round the part: a stepped rim and a crisp disc read as
	// stacked camouflage discs from afar.)
	const float ang = atan2(p.x, p.y), ph = dens.w * 6.2832f;
	rim = 1.0f - 0.13f * (0.5f + 0.5f * sin(ang * 3.0f + ph)) - 0.08f * (0.5f + 0.5f * sin(ang * 7.0f + ph * 3.1f))
		- 0.05f * (0.5f + 0.5f * sin(ang * 13.0f + ph * 7.3f));
	// A needle brush's part bristles round its rim with its tufts' tips, two dozen of them, its area kept (a smooth
	// lobed rim read as a fish scale, a crown of them as topiary), faded out where the part is under two dozen pixels
	// and the bristles would alias.
	needle = abs(shrub_prof(uint(part.x + 0.5f), 0).x - 2.0f) < 0.5f;
	if (needle)
		rim += 0.16f * (0.5f - abs(frac(ang * (22.0f / 6.2832f) + dens.w * 5.0f) - 0.5f) * 2.0f) * saturate((misc.w - 16.0f) / 24.0f);
	// (Its edge's ramp over the outline's own change: the bristles turn it as fast as the radius.)
	inside = saturate((rim - r) / max(fwidth(rim - r), 1e-4f) + 0.5f);
}

// The plant's season state and the share of the part's leaves on now - the same over the whole quad (the shadow maps'
// vertex shader works it out once: shrub_mass_on_share).
void	shrub_mass_season	(float4 part, float4 misc, inout shrub_mass_frame F)
{
	const uint sp = uint(part.x + 0.5f);
	// The plant's state: dead (no leaves), a Red Forest pine (its dead needles on all year), its marcescent share (the
	// last of its leaves to fall hang on withered from their fall to its bud break), a young tree's spring lead
	// (shrub_seas_lead).
	const uint kind = shrub_mass_kind(misc);
	F.dead_k = kind == 1u;
	F.red_k = kind == 2u;
	const float2 ml = shrub_mass_unpack(misc);
	F.marc = ml.x;
	F.K0 = shrub_seas_lead(sp, ml.y);
	F.early = misc.y * shrub_prof(sp, 7).x * (1.0f / 120.0f);
	F.winter = F.K0.x < 0.02f;
	// The share of the part's leaves on now (for the far mean): their ranks spread +-0.25 round the part's; the withered
	// ones after the fall and through the winter.
	const float kept = F.marc * (F.winter ? 1.0f : (F.K0.w > 0.01f ? saturate((F.K0.w - (part.z - F.early * 0.5f) + 0.25f) * 2.0f) : 0.0f));
	// (The cells' orders are held inside the keys' ends as the near leaves' are: every leaf out at 1, every one down at 1.)
	F.on_share = F.dead_k ? 0.0f : (F.red_k ? 1.0f : max((F.K0.x < 0.025f ? 0.0f : saturate((F.K0.x - (part.y - F.early) + 0.25f) * 2.0f))
		* (F.K0.w > 0.975f ? 0.0f : saturate(((part.z - F.early * 0.5f) - F.K0.w + 0.25f) * 2.0f)), kept));
}

// The part's frame, shared by its leaves and its mean blot: its ragged outline and the pixel's ramp inside it, the
// plant's season state and the share of its leaves on.
shrub_mass_frame	shrub_mass_frame_at	(float2 p, float4 part, float4 dens, float4 misc)
{
	shrub_mass_frame F = (shrub_mass_frame)0;
	shrub_mass_outline(p, part, dens, misc, F.r, F.rim, F.inside, F.needle);
	shrub_mass_season(part, misc, F);
	return F;
}

float	shrub_mass_on_share	(float4 part, float4 misc)
{
	shrub_mass_frame F = (shrub_mass_frame)0;
	shrub_mass_season(part, misc, F);
	return F.on_share;
}

// The mean blot of the far cells (> 0 inside): ragged inside its rim too (holes of a few cells), its cover thinning over
// the outer third, its outline lobed by a second octave of clumps (a smooth disc read as a coin).
float	shrub_mass_blot_d	(float2 p, float4 dens, float r, float rim, float on_share)
{
	const float2 hp = p * 2.2f + dens.w * 17.0f, hi = floor(hp), hf = hp - hi, hu = hf * hf * (3.0f - 2.0f * hf);
	const float holes = lerp(lerp(shrub_m_hash(hi, 3.0f), shrub_m_hash(hi + float2(1.0f, 0.0f), 3.0f), hu.x),
		lerp(shrub_m_hash(hi + float2(0.0f, 1.0f), 3.0f), shrub_m_hash(hi + float2(1.0f, 1.0f), 3.0f), hu.x), hu.y);
	const float2 hp2 = p * 4.7f + dens.w * 29.0f, hi2 = floor(hp2), hf2 = hp2 - hi2, hu2 = hf2 * hf2 * (3.0f - 2.0f * hf2);
	const float lobes = lerp(lerp(shrub_m_hash(hi2, 6.0f), shrub_m_hash(hi2 + float2(1.0f, 0.0f), 6.0f), hu2.x),
		lerp(shrub_m_hash(hi2 + float2(0.0f, 1.0f), 6.0f), shrub_m_hash(hi2 + float2(1.0f, 1.0f), 6.0f), hu2.x), hu2.y);
	return sqrt(saturate(dens.x * on_share * 1.1f)) * rim * (1.0f - 0.18f * holes) - r
		- 0.15f * smoothstep(0.55f, 1.0f, r) * holes - 0.16f * (lobes - 0.5f);
}

float	shrub_mass_blot_d	(float2 p, float4 dens, shrub_mass_frame F)	{ return shrub_mass_blot_d(p, dens, F.r, F.rim, F.on_share); }

// The part's cover as its mean blot alone (the shadow maps where a leaf is under a texel: the same shadow for a
// fraction of the leaves' work), the share of its leaves on given (shrub_mass_on_share).
float	shrub_mass_blot	(float2 p, float4 part, float4 dens, float4 misc, float on_share)
{
	float r, rim, inside;
	bool needle;
	shrub_mass_outline(p, part, dens, misc, r, rim, inside, needle);
	const float d = shrub_mass_blot_d(p, dens, r, rim, on_share);
	return saturate(d / max(fwidth(d), 1e-4f) + 0.5f) * inside;
}

// The part's leaves gathered into round clumps, `cells` of them across (a shadow map where a leaf is under a texel and
// a clump a few): one in a cell at the chance that keeps the part's cover (Beer-Lambert over a clump's area), jittered in
// it, inside the part's ragged outline - the dapple the near cascade's leaves cast, where the mean blot was a solid
// shadow (a step at the cascades' seam round the player).
float	shrub_mass_dapple	(float2 p, float4 part, float4 dens, float4 misc, float cells, float on_share)
{
	float r, rim, inside;
	bool needle;
	shrub_mass_outline(p, part, dens, misc, r, rim, inside, needle);
	const float2 g = p * cells * 0.5f, i = floor(g - 0.5f);
	// (A clump a disc 0.62 of a cell in radius, 1.21 cells of area, jittered by a fifth: the four cells round the pixel
	// hold every one that reaches it.)
	const float q = saturate(-log(1.0f - min(dens.x * on_share, 0.97f)) / 1.21f);
	float best = -1.0f;
	[unroll] for (int y = 0; y <= 1; ++y)
		[unroll] for (int x = 0; x <= 1; ++x)
		{
			const float2 c = i + float2(x, y);
			const float h = shrub_m_hash(c, dens.w + 5.0f);
			const float2 o = c + 0.5f + (float2(frac(h * 7.3f), frac(h * 13.7f)) - 0.5f) * 0.4f;
			best = frac(h * 3.3f) <= q ? max(best, 0.62f - length(g - o)) : best;
		}
	return saturate(best / max(fwidth(best), 1e-4f) + 0.5f) * inside;
}

// ratio: a leaf's width over its length; clump: the leaves' clump level (cells are clumps of 4^clump leaves); outer:
// the crown's outward normal in the quad (a blob's leaves thin on that side; zero for none).
shrub_mass	shrub_mass_at	(float2 p, float4 part, float4 dens, float4 misc, float ratio, float clump, float2 outer)
{
	shrub_mass M = (shrub_mass)0;
	const uint sp = uint(part.x + 0.5f);
	const float cells = dens.y;
	const shrub_mass_frame F = shrub_mass_frame_at(p, part, dens, misc);
	// (A pixel out of the part's ragged outline - the quad's corners, a fifth of it - has no cover whatever its leaves:
	// none of them is worked out.)
	[branch] if (F.inside <= 0.0f)
		return M;
	const float r = F.r, rim = F.rim, inside = F.inside;
	const bool needle = F.needle, dead_k = F.dead_k, red_k = F.red_k, winter = F.winter;
	const float marc = F.marc, early = F.early, on_share = F.on_share;
	const float4 K0 = F.K0;
	// The cells' grid turned its own way per part: the quad faces the eye, and a grid square to the screen with a leaf's
	// colour and facing a cell read as a brick mosaic where the cells are a few pixels.
	float g_s, g_c;
	sincos(dens.w * 6.2832f, g_s, g_c);
	const float2 pg = float2(p.x * g_c - p.y * g_s, p.x * g_s + p.y * g_c);
	// (The crown's outer side in the turned grid too.)
	const float2 og = float2(outer.x * g_c - outer.y * g_s, outer.x * g_s + outer.y * g_c);
	const float2 g = pg * cells * 0.5f;
	const float2 i = floor(g);
	// A leaf is an ellipse 1.36 cells long (neighbours overlap); a cell holds one with the probability that gives the
	// part its cover (Beer-Lambert of the leaves' area over the cells). A clump of narrow leaves or needles is a wider
	// blot than any one of them, its outline the ellipse's rather than the leaf texture's (a needle tuft's clump is a
	// brush, not a sliver of a needle: the far junipers stood bare); a broad leaf's clump keeps its leaf's outline (round
	// blots read as cotton wool). (One or the other: halfway, the alpha test cut the two outlines' overlap.)
	// (A clump's blot overfills its cell, 1.6 by 1.4: a dense part's blots then close up as its leaves do; one a cell
	// wide left a maze of gaps and a dense juniper read half see-through. A needle tuft is a brush at every level: its
	// cell a needle's sliver left the far junipers bare.)
	// (By the leaf kind: a juniper's tuft card is as wide as half its length, its ratio no needle's.)
	const float ck = needle || (clump >= 0.5f && ratio < 0.35f) ? 1.0f : 0.0f;
	// A broad leaf's clump is its species' spray card - a shoot's few leaves, their own size and a ragged outline - in a
	// round box (one leaf blown up to the clump read as coins of five to eight leaves across).
	const float4 T8 = shrub_prof(sp, 8);
	const bool clump_card = clump >= 0.5f && ck < 0.5f && T8.z >= 0.0f;
	const float major = clump_card ? 0.76f : lerp(0.68f, 0.8f, ck);
	const float minor = clump_card ? 0.72f : lerp(max(0.68f * ratio, 0.07f), 0.7f, ck);
	// A leaf's area in cells: its texture's coverage of the square the ellipse's box maps it to (the ellipse's own
	// without the data, or for a clump).
	const float cov_tex = shrub_prof(sp, 11).w;
	const float cov = lerp(cov_tex > 0.0f ? cov_tex : 0.785f, 0.785f, ck);
	const float f_leaf = 4.0f * major * minor * cov;
	const float tau = -log(1.0f - min(dens.x, 0.97f));
	const float q_on = saturate(tau / f_leaf);
	// A blob's leaves thin and shrink toward its rim, their count kept (the radial weight's mean over the disc is
	// one): its rim a fine speckle that its neighbours' overlap closes, not a ring of loose flakes against the sky.
	// They thin on the crown's outer side too, by up to three fifths (a blob over an arching cane reached into the sky
	// above it: flakes floating over the crown), the weight's mean kept (the thinning's mean over the disc 0.6 x 0.212).
	const bool blob = misc.z > 0.5f;
	const float out_k = 1.0f / (1.0f - 0.127f * min(length(outer), 1.0f));
	// The leaves clump on their shoots: a value noise over the cells at two fifths of their frequency moves each
	// cell's chance of a leaf, so a part's holes (and the darker inside showing through them) come in pockets of a
	// few leaves, not one leaf at a time - a crown strewn with single dark leaves read frosted and pepper-and-salt
	// against the photos of bushes. The three by three cells below need at most three lattice points a side: hashed
	// once here, not four times per cell.
	const float2 L0 = floor((i - 0.5f) / 2.5f);
	const float salt = dens.w + 7.0f;
	const float h00 = shrub_m_hash(L0, salt), h01 = shrub_m_hash(L0 + float2(1.0f, 0.0f), salt), h02 = shrub_m_hash(L0 + float2(2.0f, 0.0f), salt);
	const float h10 = shrub_m_hash(L0 + float2(0.0f, 1.0f), salt), h11 = shrub_m_hash(L0 + float2(1.0f, 1.0f), salt), h12 = shrub_m_hash(L0 + float2(2.0f, 1.0f), salt);
	const float h20 = shrub_m_hash(L0 + float2(0.0f, 2.0f), salt), h21 = shrub_m_hash(L0 + float2(1.0f, 2.0f), salt), h22 = shrub_m_hash(L0 + float2(2.0f, 2.0f), salt);
	float best = -1e3f, best_s = 1.0f;
	float2 best_q = float2(0.0f, 0.0f), best_d = float2(1.0f, 0.0f);
	// (The four cells whose centres surround the pixel: a leaf of a cell past them - its centre a cell off at the least,
	// jittered by 0.3 at the most, 0.8 long a half - reaches into them by a tenth of a cell, a third of a pixel at the
	// cells' three; the nine round its cell were most of a cluster's cost.)
	const float2 i2 = floor(g - 0.5f);
	[unroll] for (int y = 0; y <= 1; ++y)
		[unroll] for (int x = 0; x <= 1; ++x)
		{
			const float2 c = i2 + float2(x, y);
			const float h = shrub_m_hash(c, dens.w);
			const float rank = clamp(part.y + (frac(h * 5.7f) - 0.5f) * 0.5f - early, 0.025f, 0.975f);
			const float fall = clamp(part.z + (frac(h * 9.1f) - 0.5f) * 0.5f - early * 0.5f, 0.025f, 0.975f);
			// (A withered leaf: one of the last to fall, past its fall or in the winter before the buds break.)
			const bool withered = marc > 0.0f && !dead_k && !red_k && fall > 1.0f - marc && (fall < K0.w || winter);
			const bool on = !dead_k && (red_k || withered || (rank < K0.x && fall >= K0.w));
			// The chance the clump gives this cell, its mean the part's: a sparse part's leaves gather (0.1 to 1.9 of
			// the chance), a dense part's holes do (the same of the chance of a hole), never clipped at one.
			const float2 cq = (c + 0.5f) / 2.5f, cl = floor(cq), cf = cq - cl, cu = cf * cf * (3.0f - 2.0f * cf);
			const bool bx = cl.x > L0.x + 0.5f, by = cl.y > L0.y + 0.5f;
			const float n00 = by ? (bx ? h11 : h10) : (bx ? h01 : h00), n10 = by ? (bx ? h12 : h11) : (bx ? h02 : h01);
			const float n01 = by ? (bx ? h21 : h20) : (bx ? h11 : h10), n11 = by ? (bx ? h22 : h21) : (bx ? h12 : h11);
			const float k = 0.9f * (2.0f * lerp(lerp(n00, n10, cu.x), lerp(n01, n11, cu.x), cu.y) - 1.0f);
			// (A blob's cell at r: the weight 1.53 .. 0.47 from its middle to its rim, the leaf 1 .. 0.55 of its size,
			// so as many of them as its area asks.)
			const float2 pc = (c + 0.5f) * (2.0f / cells);
			const float rim_k = blob ? smoothstep(0.3f, 1.0f, dot(pc, pc)) : 0.0f;
			const float s_c = 1.0f - 0.45f * rim_k;
			const float w_c = (1.3f - 0.9f * min(dot(pc, pc), 1.0f)) * (1.0f / 0.85f) * (1.0f - 0.6f * saturate(dot(pc, og))) * out_k;
			const float q_c = blob ? saturate(tau * w_c / (f_leaf * s_c * s_c)) : q_on;
			const float q_cell = q_c <= 0.5f ? q_c * (1.0f + k) : 1.0f - (1.0f - q_c) * (1.0f - k);
			if (!on || frac(h * 3.3f) > q_cell)
				continue;
			// A leaf: an ellipse turned its own way, jittered in its cell (a needle tuft's clump a streak along the
			// upright shoots).
			const float2 o = c + 0.5f + (float2(shrub_m_hash(c, dens.w + 1.0f), shrub_m_hash(c, dens.w + 2.0f)) - 0.5f) * 0.6f;
			// (A needle tuft's upright in the quad: the grid's turn taken back.)
			const float a = needle ? 1.5708f + dens.w * 6.2832f + (h - 0.5f) * 0.6f : h * 6.2832f;
			const float2 d = float2(cos(a), sin(a));
			const float2 q = g - o;
			const float u = dot(q, d) / (major * s_c), v = (q.x * d.y - q.y * d.x) / (minor * s_c);
			const float e = 1.0f - sqrt(u * u + v * v);
			if (e > best)
			{
				best = e;
				best_s = s_c;
				M.h = h;
				M.rank = rank;
				M.fall = fall;
				M.withered = withered ? 1.0f : 0.0f;
				M.tilt = d * (frac(h * 13.1f) - 0.5f);
				best_q = q;
				best_d = d;
				M.bloom = frac(h * 21.7f) < dens.z ? 1.0f : 0.0f;
			}
		}
	// The leaves' distance in quad units (a cell is 2 / cells, the ellipse's minor semi-axis `minor` of it), the mean
	// blot's, blended by how far the cells resolve (whole from three pixels a cell: the clumps keep them there).
	const float resolve = saturate((misc.w / cells - 1.0f) * 0.5f);
	// The winning leaf's texture: across it the ellipse's minor axis, along it (base to tip) its major one, mirrored by
	// its hash; sampled with the gradients of the part's continuous cell coordinates (the leaf's frame jumps from cell
	// to cell, the mip must not).
	const float layer = clump_card ? T8.z : (T8.w >= 0.0f ? T8.w : shrub_leaf_layer(sp, M.h));
	const float mirror = frac(M.h * 11.3f) < 0.5f ? -1.0f : 1.0f;
	const float mn = minor * best_s, mj = major * best_s;
	const float2 lq = float2((best_q.x * best_d.y - best_q.y * best_d.x) / mn * mirror, dot(best_q, best_d) / mj);
	// (The cells' gradients from the part's own coordinates: a pixel beside it may clump at another level.)
	const float2 gx = ddx(pg) * cells * 0.5f, gy = ddy(pg) * cells * 0.5f;
	const float2 lx = float2((gx.x * best_d.y - gx.y * best_d.x) / mn * mirror, dot(gx, best_d) / mj);
	const float2 ly = float2((gy.x * best_d.y - gy.y * best_d.x) / mn * mirror, dot(gy, best_d) / mj);
	M.tex = s_shrub_leaf_d.SampleGrad(smp_base, float3(0.5f + 0.5f * lq, layer), 0.5f * lx, 0.5f * ly);
	const float in_box = best > -100.0f && max(abs(lq.x), abs(lq.y)) <= 1.0f ? 1.0f : 0.0f;
	// A needle tuft's blot is a brush: its needles' tips stand out of its outline at eight angles turned with the tuft
	// (its area kept), and its needles run along it in streaks over the darker inside between them, faded out under
	// four pixels a streak (a smooth blot of one colour read as cotton wool, the sky-lit top of a crown as a pale cap).
	// (The streaks' and the blots' screen footprints from the part's own gradients, not a quad's differences: each pixel
	// may clump at its own level.)
	const float sx = lq.x * 1.5f + M.h * 7.0f;
	const float s_px = 1.0f / max(1.5f * (abs(lx.x) + abs(ly.x)), 1e-4f);
	float eb = max(best, -1.0f);
	if (needle && best > -100.0f)
	{
		const float lr = max(length(lq), 1e-4f);
		const float c2 = 2.0f * (lq.y / lr) * (lq.y / lr) - 1.0f, c4 = 2.0f * c2 * c2 - 1.0f;
		eb += 0.12f * (2.0f * c4 * c4 - 1.0f) * saturate(lr);
		M.tex.r *= 1.0f + 0.25f * cos(sx * 6.2832f) * saturate((s_px - 2.0f) * 0.5f);
	}
	// The mean blot of the far cells (shrub_mass_blot_d).
	const float d_mean = shrub_mass_blot_d(p, dens, F);
	// (A pixel with no leaf in reach keeps best's -1000, clamped to -1 above: beside a leafy one its edge ramp swallowed
	// the step, a half cover dithered in dotted lines through the holes.)
	// (A needle tuft's tips turn its outline faster across: its ramp two fifths wider.)
	const float ellipse = saturate(eb / max((length(lx) + length(ly)) * (needle ? 1.4f : 1.0f), 1e-4f) + 0.5f);
	float leaf_cov = lerp(saturate(d_mean / max(fwidth(d_mean), 1e-4f) + 0.5f), lerp(M.tex.a * in_box, ellipse, ck), resolve);
	// The bare twigs show through where the leaves are off: a short straight twig in each leaf's cell (never a clump's:
	// a twig does not thicken with distance), turned its own way and off its centre (one stripe pattern across the
	// part read as a comb); under a pixel and a half their mean cover.
	const float cells0 = cells * exp2(clump);
	// The leaf within a clump at the pixel: its own facing and colour step (a clump of one facing and one colour read
	// as a smooth disc, a sequin).
	M.hs = shrub_m_hash(floor(pg * cells0 * 0.5f), dens.w + 9.0f);
	// (None while every leaf is on - the share is the part's, one for the whole quad: its derivatives stand.)
	float twig_cov = 0.0f;
	[branch] if (on_share < 0.999f)
	{
		const float2 tw = pg * cells0 * 1.7f;
		const float2 tc = floor(tw);
		const float ta = shrub_m_hash(tc, dens.w + 3.0f) * 3.14159f;
		const float2 td = float2(cos(ta), sin(ta));
		const float2 tq = tw - tc - 0.5f - (float2(shrub_m_hash(tc, dens.w + 4.0f), shrub_m_hash(tc, dens.w + 5.0f)) - 0.5f) * 0.3f;
		const float twig_d = min(0.07f - abs(tq.x * td.y - tq.y * td.x), 0.42f - abs(dot(tq, td)));
		const float hatch = saturate(twig_d / max(fwidth(tw.x), 1e-4f) + 0.5f);
		const float resolve0 = saturate((misc.w / cells0 - 1.5f) * 0.5f);
		twig_cov = lerp(0.22f, hatch * 0.6f, resolve0) * (1.0f - on_share) * saturate(dens.x * 2.0f);
	}
	M.leaf = leaf_cov >= twig_cov ? 1.0f : 0.0f;
	M.cover = max(leaf_cov, twig_cov) * inside;
	return M;
}

// Clips the pixel outside the part's leaves (and twigs): against interleaved gradient noise where the part is
// large on the screen, a plain alpha test at half cover where it is a few pixels (a dithered edge over most of them
// would read as smoke once the temporal resolve averages it). size_px: the part's size on the screen.
void	shrub_mass_clip	(float cover, float2 pix, float frame, float size_px)
{
	const float2 q = pix + float2(29.0f, 7.0f) + 7.13f * frame;
	const float noise = frac(52.9829189f * frac(dot(q, float2(0.06711056f, 0.00583715f))));
	clip(cover - lerp(0.5f, noise, saturate((size_px - 12.0f) / 20.0f)) - 1e-3f);
}

#endif // DA_SHRUB_MASS_H
