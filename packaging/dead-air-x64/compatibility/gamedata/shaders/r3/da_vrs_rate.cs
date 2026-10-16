#include "common.h"
#include "da_vrs.h"

//	phase_vrs_rate (r4_rendertarget_phase_vrs.cpp): the frame's shading-rate image, one texel per
//	hardware tile, built before the sun accumulation (docs/dead-air/dx12-features/vrs.md 3.2). A group
//	of 8x8 threads covers two by two tiles of 8 or 16 (16x16 or 32x32 pixels) or one tile of 32, a
//	thread 2x2 or 4x4 pixels of it. Per pixel: the depth silhouette, the sky and the first-person HUD;
//	per thread block: the contrast the previous frame had where the block was then; per tile: the rate
//	each axis can afford. Every load that does not depend on another is issued before the first
//	barrier, so a group waits on memory twice: for the depth and for the contrast.
Texture2D s_vrs_depth;			//	the hardware depth, render size, cleared to 1
Texture2D s_vrs_contrast;		//	phase_vrs_luma of the previous frame, one texel per 8x8 pixels
Texture2D s_vrs_velocity;		//	NDC(current) - NDC(previous), y up, when the G-buffer wrote it
Texture2D<uint> s_vrs_prev;		//	the previous frame's rates
#ifdef GRIZZLY_VULKAN
// The shading rate image is R8_UINT; the stats image takes atomics, so this shader keeps typed image formats.
[[vk::image_format("r8ui")]]
#endif
RWTexture2D<uint> u_vrs_rate : register(u0);
RWTexture2D<uint> u_vrs_stats : register(u1);	//	tiles per rate, 7 bins (r__vrs_log)

//	x = the tile size, y = the mode (1 conservative, 2 aggressive, 3 forced 2x2, 4 forced 4x4),
//	z = the contrast threshold scale (s_mode * r__vrs_threshold * render / display width),
//	w = the depth ratio of a silhouette (k_jump).
uniform float4 da_vrs_params;
//	x, y = the projection's _33 and _43, z = the HUD depth limit.
uniform float4 da_vrs_params2;
//	xy = the render size, z = 1 when the previous rates are this chain's, w = 1 when the velocity is.
uniform float4 da_vrs_params3;
//	x = 1: 2x4, 4x2 and 4x4 exist; y = 1: count the tiles; z = the depth of field's far plane in view
//	metres (0 = none).
uniform float4 da_vrs_params4;
//	View space -> the previous frame's clip space: the camera's motion alone.
uniform float4x4 da_vrs_reproj;

#define VRS_EDGE 1u			//	any: a depth silhouette, sky next to ground, the HUD next to the world
#define VRS_HUD 2u			//	any: the first-person HUD
#define VRS_INVALID 4u		//	any: the previous frame did not see it
#define VRS_SKY 8u			//	all: the sky
#define VRS_FAR_DOF 16u		//	all: blurred by the depth of field

//	Luma sums in fixed point (the mean luma of a contrast texel is at most 1, a tile has at most 64).
#define VRS_LUMA_SCALE 4096.0

//	The depth of the largest region (32 pixels) and a one-pixel apron.
groupshared float g_depth[34 * 34];
//	Per tile (up to four): max Ex, max Ey (float bits: non-negative floats order like their bits),
//	luma sum, samples, flags any, flags all.
groupshared uint g_ex[4];
groupshared uint g_ey[4];
groupshared uint g_luma[4];
groupshared uint g_samples[4];
groupshared uint g_any[4];
groupshared uint g_all[4];

//	P33 - d: the view depth is P43 / (d - P33), so two depths compare as the inverse ratio of this.
float vrs_w(float d)
{
	return da_vrs_params2.x - d;
}

float4 vrs_w(float4 d)
{
	return da_vrs_params2.xxxx - d;
}

//	1, 2 or 4 pixels on one axis for the block error e: a block already shaded coarse shows its error
//	only at its borders (the RMS grows by sqrt 2 per step), and a rate is left only 20 % past its
//	threshold, so a tile does not flicker between two rates.
uint vrs_axis(float e, uint prev, float tau, bool quarter)
{
	e /= prev == 4u ? 2.0 : (prev == 2u ? 1.41421356 : 1.0);
	const bool half_ok = e < tau * (prev >= 2u ? 1.0 : 0.8);
	const bool quarter_ok = 2.13 * e < tau * (prev >= 4u ? 1.0 : 0.8);
	return quarter && quarter_ok ? 4u : (half_ok ? 2u : 1u);
}

//	prev_rate: the tile's rate in the previous frame (0, 1x1, when there is none).
uint vrs_rate(uint slot, uint prev_rate)
{
	const uint mode = uint(da_vrs_params.y);
	const bool additional = da_vrs_params4.x > 0.0;
	if (mode >= 3u)
		return mode == 4u && additional ? 10u : 5u;
	const float samples = float(g_samples[slot]);
	if (samples <= 0.0 || (g_any[slot] & (VRS_EDGE | VRS_HUD | VRS_INVALID)) != 0u)
		return 0u;
	//	The sky may go to a quarter in either mode, but only as far as its contrast allows: cloud
	//	detail at 4x4 shows as steps (vrs.md 9.3, foliage).
	const bool sky = (g_all[slot] & VRS_SKY) != 0u;
	const bool quarter = mode == 2u || sky;
	const float tau = da_vrs_params.z * (float(g_luma[slot]) / (VRS_LUMA_SCALE * samples) + 0.08);
	const uint2 prev = da_vrs_decode(prev_rate);
	uint2 r = uint2(vrs_axis(asfloat(g_ex[slot]), prev.x, tau, quarter), vrs_axis(asfloat(g_ey[slot]), prev.y, tau, quarter));
	if ((g_all[slot] & VRS_FAR_DOF) != 0u)
		r = min(r * 2u, uint2(4u, 4u));
	if (!quarter || !additional)
		r = min(r, uint2(2u, 2u));
	//	1x4 and 4x1 are no rates.
	if (r.x == 4u && r.y == 1u)
		r.x = 2u;
	if (r.y == 4u && r.x == 1u)
		r.y = 2u;
	return da_vrs_encode(r);
}

void vrs_write(uint2 tile_pos, uint rate)
{
	u_vrs_rate[tile_pos] = rate;
	[branch] if (da_vrs_params4.y > 0.0)
	{
		//	0 1x1, 1 1x2, 4 2x1, 5 2x2, 6 2x4, 9 4x2, 10 4x4
		const uint bin = rate == 0u ? 0u : rate == 1u ? 1u : rate == 4u ? 2u : rate == 5u ? 3u : rate == 6u ? 4u : rate == 9u ? 5u : 6u;
		InterlockedAdd(u_vrs_stats[uint2(bin, 0)], 1u);
	}
}

void vrs_contribute(uint slot, float4 contrast, uint2 flags)
{
	InterlockedMax(g_ex[slot], asuint(contrast.x));
	InterlockedMax(g_ey[slot], asuint(contrast.y));
	InterlockedAdd(g_luma[slot], uint(contrast.z * VRS_LUMA_SCALE + 0.5));
	InterlockedAdd(g_samples[slot], uint(contrast.w + 0.5));
	InterlockedOr(g_any[slot], flags.x);
	InterlockedAnd(g_all[slot], flags.y);
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex)
{
	const uint tile = uint(da_vrs_params.x);
	const uint mode = uint(da_vrs_params.y);
	const bool adaptive = mode < 3u;
	const int2 size = int2(da_vrs_params3.xy);
	const uint region = min(tile * 2u, 32u);
	const uint per_axis = region / tile;		//	tiles per group and axis: 2, or 1 with tiles of 32
	const uint foot = region / 8u;				//	pixels per thread and axis
	const uint apron = region + 2u;
	const int2 origin = int2(group.xy * region);

	//	The first threads write one tile each; their slot is the tile's place in the group.
	const uint2 tile_pos = group.xy * per_axis + uint2(index % per_axis, index / per_axis);
	const bool writer = index < per_axis * per_axis && all(tile_pos < (uint2(size) + tile - 1u) / tile);
	uint prev_rate = 0u;
	[branch] if (writer && adaptive && da_vrs_params3.z > 0.0)
		prev_rate = s_vrs_prev.Load(int3(tile_pos, 0));

	//	The block's centre pixel: its motion vector is read now, its depth after the barrier.
	const uint2 block = local.xy * foot;
	const uint2 centre = block + foot / 2u;
	const int2 centre_px = min(origin + int2(centre), size - 1);
	float2 velocity = float2(0.0, 0.0);
	[branch] if (adaptive && da_vrs_params3.w > 0.0)
		velocity = s_vrs_velocity.Load(int3(centre_px, 0)).xy;

	[branch] if (index < 4u)
	{
		g_ex[index] = 0u;
		g_ey[index] = 0u;
		g_luma[index] = 0u;
		g_samples[index] = 0u;
		g_any[index] = 0u;
		g_all[index] = VRS_SKY | VRS_FAR_DOF;
	}
	[branch] if (adaptive)
	{
		//	Apron texel a is frame pixel origin - 1 + a. One gather reads a 2x2 of them at the corner it
		//	shares with its right and lower neighbours; the sampler clamps what lies outside the frame to
		//	the frame's edge.
		const uint pairs = apron / 2u;
		const float2 texel = 1.0 / float2(size);
		for (uint i = index; i < pairs * pairs; i += 64u)
		{
			const uint2 a = uint2(i % pairs, i / pairs) * 2u;
			const float4 d = s_vrs_depth.GatherRed(smp_nofilter, float2(origin + int2(a)) * texel);
			const uint at = a.y * apron + a.x;
			g_depth[at] = d.w;
			g_depth[at + 1u] = d.z;
			g_depth[at + apron] = d.x;
			g_depth[at + apron + 1u] = d.y;
		}
	}
	GroupMemoryBarrierWithGroupSync();

	float4 contrast = float4(0.0, 0.0, 0.0, 0.0);
	uint2 flags = uint2(0u, VRS_SKY | VRS_FAR_DOF);
	[branch] if (adaptive)
	{
		const float k_jump = 1.0 + da_vrs_params.w;
		const float hud_limit = da_vrs_params2.z;
		bool inside = false;
		for (uint fy = 0u; fy < foot; ++fy)
		{
			for (uint fx = 0u; fx < foot; ++fx)
			{
				const uint2 lp = block + uint2(fx, fy);
				[branch] if (any(origin + int2(lp) >= size))
					continue;
				inside = true;
				const uint at = (lp.y + 1u) * apron + lp.x + 1u;
				const float d = g_depth[at];
				const float4 nd = float4(g_depth[at - 1u], g_depth[at + 1u], g_depth[at - apron], g_depth[at + apron]);
				const bool sky = d >= 1.0;
				const bool hud = d < hud_limit;
				uint f = (hud ? VRS_HUD : 0u) | (sky ? VRS_SKY : 0u);

				//	A silhouette: the view depth of a 4-neighbour differs by more than the ratio, or it
				//	is of another kind (sky, HUD). Grass over the ground right behind it is no edge; a
				//	trunk against a hill far away is.
				const bool4 nsky = nd >= 1.0;
				const bool4 nhud = nd < hud_limit;
				if (any(nsky != sky) || any(nhud != hud))
					f |= VRS_EDGE;
				else if (!sky)
				{
					const float4 nw = vrs_w(nd);
					const float w = vrs_w(d);
					const float lo = min(w, min(min(nw.x, nw.y), min(nw.z, nw.w)));
					const float hi = max(w, max(max(nw.x, nw.y), max(nw.z, nw.w)));
					if (hi > k_jump * lo)
						f |= VRS_EDGE;
				}
				if (da_vrs_params4.z > 0.0 && !sky && !hud && da_vrs_params2.y / -vrs_w(d) > da_vrs_params4.z)
					f |= VRS_FAR_DOF;
				flags.x |= f;
				flags.y &= f;
			}
		}

		[branch] if (inside)
		{
			//	Where the block was a frame ago, taken at its centre pixel: by the motion vector, else
			//	by the camera's motion. A contrast texel covers 8x8 pixels, so one sample per block
			//	resolves it. The sky has no vector yet (it is drawn after this pass; the target was
			//	cleared to zero): the camera's motion is exact for it.
			const float d = g_depth[(centre.y + 1u) * apron + centre.x + 1u];
			const float2 uv = (float2(centre_px) + 0.5) / float2(size);
			float2 prev_uv;
			[branch] if (da_vrs_params3.w > 0.0 && d < 1.0)
				prev_uv = uv - velocity * float2(0.5, -0.5);
			else
			{
				const float z = d >= 1.0 ? 1e6 : da_vrs_params2.y / -vrs_w(d);
				const float3 P = float3(z * ((float2(centre_px) + 0.5) * pos_decompression_params.zw - pos_decompression_params.xy), z);
				const float4 clip = mul(da_vrs_reproj, float4(P, 1.0));
				prev_uv = clip.w > 0.0 ? clip.xy / clip.w * float2(0.5, -0.5) + 0.5 : float2(-1.0, -1.0);
			}
			float4 c = float4(0.0, 0.0, 0.0, 0.0);
			[branch] if (all(prev_uv >= 0.0) && all(prev_uv < 1.0))
				c = s_vrs_contrast.Load(int3(uint2(prev_uv * float2((uint2(size) + 7u) / 8u)), 0));
			if (c.w <= 0.0)
				flags.x |= VRS_INVALID;
			contrast = float4(c.x, c.y, c.z, 1.0);
		}
	}

	//	The tile of this thread's block: a quarter of the group (4x4 threads) with two tiles per axis.
	const uint slot = per_axis > 1u ? (local.x >> 2) + 2u * (local.y >> 2) : 0u;
#if defined(GRIZZLY_WAVE_LANES)
	//	Each wave reduces per tile it holds, one lane of it contributes per tile.
	[unroll]
	for (uint s = 0u; s < 4u; ++s)
	{
		const bool mine = slot == s;
		[branch] if (WaveActiveAnyTrue(mine))
		{
			const float2 e = WaveActiveMax(mine ? contrast.xy : float2(0.0, 0.0));
			const float2 l = WaveActiveSum(mine ? contrast.zw : float2(0.0, 0.0));
			const uint any_flags = WaveActiveBitOr(mine ? flags.x : 0u);
			const uint all_flags = WaveActiveBitAnd(mine ? flags.y : 0xffffffffu);
			[branch] if (WaveIsFirstLane())
				vrs_contribute(s, float4(e, l), uint2(any_flags, all_flags));
		}
	}
#else
	vrs_contribute(slot, contrast, flags);
#endif
	GroupMemoryBarrierWithGroupSync();

	[branch] if (writer)
		vrs_write(tile_pos, vrs_rate(index, prev_rate));
}
