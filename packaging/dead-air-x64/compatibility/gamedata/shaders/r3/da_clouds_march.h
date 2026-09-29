#ifndef DA_CLOUDS_MARCH_H
#define DA_CLOUDS_MARCH_H

#include "common.h"
#include "da_clouds_vol.h"
#include "da_clouds_hist.h"

// The volumetric march, at half resolution, for every direction in the frame (the sky mask
// is applied at composite). Output: rgb = in-scattered light (premultiplied), a = the
// transmittance of the deck along the ray. Accumulated over frames: the start offset is
// jittered per frame and the result is blended with the previous frame's, reprojected by
// direction - the deck is far enough that the camera's translation does not matter.
Texture2D s_cloud_map;
Texture2D s_clouds_hist;

// da_cloud_temporal: x = history blend (0 = none), y = frame index, z = history valid, w = steps.
uniform float4 da_cloud_temporal;
uniform float4 da_cloud_debug; // x: 3 = show the reprojection offset
// The previous frame's camera basis, the same layout as da_cloud_cam_*.
uniform float4 da_cloud_prev_r;
uniform float4 da_cloud_prev_u;
uniform float4 da_cloud_prev_d;
// The march target: xy = its size in texels, zw = 1 / size.
uniform float4 da_cloud_target;

// The early-out on DirectX 12 (PLAN.md C14): the march runs after the G-buffer, so it can skip the
// texels the composite never shows. DirectX 11 has no depth alias and marches everything.
// da_cloud_early.x = 1 (da_clouds_hist.h): s_cloud_depth holds this frame's G-buffer depth.
Texture2D s_cloud_depth; // the scene depth, render size ($user$cloud_depth)

// Geometry in front of the far plane over the texel's whole 2x2 footprint and a one-pixel border
// (the full-res block from 2 pix - 1 to 2 pix + 2). The composite draws on sky pixels only, and no
// bilinear tap of a sky pixel reaches a texel whose border holds no sky: such a texel is never seen.
bool da_clouds_hidden(uint2 pix)
{
	// Four gathers of 2x2 over the 4x4 block; clamp addressing repeats the screen's rim.
	const float2 uv = (float2(pix) * 2.0f) * pos_decompression_params2.zw;
	const float4 d0 = s_cloud_depth.GatherRed(smp_nofilter, uv, int2(0, 0));
	const float4 d1 = s_cloud_depth.GatherRed(smp_nofilter, uv, int2(2, 0));
	const float4 d2 = s_cloud_depth.GatherRed(smp_nofilter, uv, int2(0, 2));
	const float4 d3 = s_cloud_depth.GatherRed(smp_nofilter, uv, int2(2, 2));
	return all(max(max(d0, d1), max(d2, d3)) < 1.0f);
}

float4 da_clouds_march_pixel(uint2 pix)
{
	const float2 tc = (float2(pix) + 0.5f) * da_cloud_target.zw;
	const float2 pos2d = float2(pix) + 0.5f;
	// Nested: && does not short-circuit here, and the gathers must not run with the early-out off.
	[branch] if (da_cloud_early.x > 0.5f)
	{
		[branch] if (da_clouds_hidden(pix))
			return float4(0.0f, 0.0f, 0.0f, DA_CLOUDS_HIDDEN);
	}
	const float3 dir = da_cloud_view_ray(tc);
	const float cover = da_cloud_params.x;
	float4 result = float4(0, 0, 0, 1);

	const float base_alt = da_cloud_params.y;
	const float thickness = max(da_cloud_params2.y, 50.0f);
	const float rise = base_alt - eye_position.y;
	// Below the horizon there is no deck; the far deck is edge-on anyway.
	[branch] if (dir.y > 0.012f && cover > 0.001f && rise > 50.0f)
	{
		const float3 to_sun = -da_sun_dir_real.xyz;
		const float cos_t = dot(dir, to_sun);
		// Phase depends on the ray, not the march sample: six HG lobes once per pixel.
		const float3 phases = float3(da_cloud_phase(cos_t, 1.0f),
			da_cloud_phase(cos_t, 0.5f), da_cloud_phase(cos_t, 0.25f));
		const float sun_up = saturate(to_sun.y * 4.0f + 0.2f);
		float3 sun, sky;
		da_cloud_lights(sun_up, sun, sky);
		const float ext = 0.045f;

		const float t0 = rise / dir.y;
		// A grazing ray would cross the slab for tens of kilometres; past a few of them the
		// deck is haze anyway (below), so the march budget stops there and the steps stay fine.
		const float t1 = min((rise + thickness) / dir.y, t0 + 7000.0f);
		// More steps toward the horizon, where a ray crosses kilometres of slab and the step
		// planes would otherwise show as streaks.
		const int steps = int(da_cloud_temporal.w + 0.5f) + int(14.0f * saturate(1.0f - dir.y * 2.5f));
		const float dt = (t1 - t0) / float(steps);
		// Golden-ratio frame offset over a per-pixel hash: with the temporal blend the step
		// planes average out over a dozen frames.
		const float jitter = frac(da_cl_hash(pos2d) + da_cloud_temporal.y * 0.618034f);

		// Nothing along the slab where the weather map is empty: three probes decide.
		const float3 w0 = da_cloud_field(s_cloud_map, (eye_position + dir * t0).xz);
		const float3 w1 = da_cloud_field(s_cloud_map, (eye_position + dir * (0.5f * (t0 + t1))).xz);
		const float3 w2 = da_cloud_field(s_cloud_map, (eye_position + dir * t1).xz);
		const float probe = w0.y + w1.y + w2.y;
		[branch] if (probe > 0.006f || isnan(probe))
		{
			float T = 1.0f;
			float3 acc = 0;
			[loop]
			for (int i = 0; i < steps; ++i)
			{
				const float t = t0 + (float(i) + jitter) * dt;
				const float3 p = eye_position + dir * t;
				const float hn = saturate((p.y - base_alt) / thickness);
				// Both authored height profiles are exactly zero outside this interval.
				[branch] if (hn <= 0.0f || hn >= 0.95f)
					continue;
				const float3 w = da_cloud_field(s_cloud_map, p.xz);
				// The far deck thins: what is kilometres away is haze, not a wall of cloud.
				const float d = da_cloud_density_vol(p, w, hn, false) * exp(-t * (1.0f / 12000.0f));
				[branch] if (d <= 0.002f)
					continue;
				float3 lit = 0;
				[branch] if (any(sun != 0.0f))
				{
					const float tau = da_cloud_sun_depth(s_cloud_map, p, to_sun, base_alt, thickness, ext);
					lit = da_cloud_sun_scatter(tau, cos_t, d, phases);
				}
				// Sky light: dark base, bright top, a touch of the ground's bounce below.
				const float3 amb = sky * (0.42f + 0.58f * hn);
				const float3 c = amb + sun * lit * 1.15f + da_cloud_lightning(p, d);
				const float a = 1.0f - exp(-d * ext * dt);
				acc += T * a * c;
				T *= 1.0f - a;
				[branch] if (T < 0.01f)
					break;
			}
			// Aerial perspective: a deck kilometres away takes the horizon's colour and thins
			// into the sky - the far edge of the slab must never stand as a wall.
			const float haze = 1.0f - exp(-t0 * (1.0f / 12000.0f));
			// The flash's share of the fog stays out of the haze too: a bolt lights its own
			// neighbourhood (da_cloud_lightning), not the far deck through the air.
			acc = lerp(acc, (fog_color.rgb - da_lightning_fog.rgb) * (1.0f - T), haze);
			T = lerp(T, 1.0f, haze);
			acc *= 1.0f - haze;
			result = float4(acc, T);
		}
	}

	// Temporal accumulation: the previous frame's result where this direction fell on it.
	// The deck is far enough that only the camera's rotation matters.
	[branch] if (da_cloud_temporal.x > 0.001f && da_cloud_temporal.z > 0.5f)
	{
		const float fwd = dot(dir, da_cloud_prev_d.xyz);
		[branch] if (fwd > 0.05f)
		{
			const float2 ndc = float2(dot(dir, da_cloud_prev_r.xyz) / (fwd * da_cloud_prev_r.w),
			                          dot(dir, da_cloud_prev_u.xyz) / (fwd * da_cloud_prev_u.w));
			const float2 puv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
			[branch] if (da_cloud_debug.x > 2.5f)
				return float4(abs(puv - tc) * 20.0f, 0.0f, 1.0f);
			[branch] if (all(puv > 0.001f) && all(puv < 0.999f))
			{
				// The history only helps while the view is nearly still: a turn drags it
				// into streaks, so the blend fades out with the reprojection's own motion.
				const float motion = length(puv - tc);
				const float blend = da_cloud_temporal.x * saturate(1.0f - motion * 150.0f);
				// Texels the early-out skipped last frame are no history.
				bool hist_valid;
				const float4 hist = da_clouds_fetch(s_clouds_hist, puv, hist_valid);
				[flatten] if (hist_valid)
					result = lerp(result, hist, blend);
			}
		}
	}
	return result;
}

#endif // DA_CLOUDS_MARCH_H
