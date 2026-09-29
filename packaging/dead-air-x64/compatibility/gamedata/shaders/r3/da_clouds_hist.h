#ifndef DA_CLOUDS_HIST_H
#define DA_CLOUDS_HIST_H

// Reading the march target ($user$clouds0 in the composite, $user$clouds1 as the march's
// history). The pixel path's early-out (da_clouds_march.h) writes a = DA_CLOUDS_HIDDEN instead of
// a march where geometry covers the texel's whole footprint; a bilinear read leaves such texels
// out. Where none of the four taps is one this is the hardware filter, bit for bit; otherwise
// the valid taps are reweighted, and valid = false when all four are hidden.
#define DA_CLOUDS_HIDDEN (-1.0f)

// x = 1: this march skips hidden texels (pixel pass only). y = 1: the march targets may hold
// sentinels; 0 without the depth alias (DirectX 11, MSAA), where the read is the plain one.
uniform float4 da_cloud_early;

float4 da_clouds_fetch(Texture2D tex, float2 uv, out bool valid)
{
	valid = true;
	[branch] if (da_cloud_early.y < 0.5f)
		return tex.SampleLevel(smp_rtlinear, uv, 0);
	const float4 a = tex.GatherAlpha(smp_rtlinear, uv);
	[branch] if (min(min(a.x, a.y), min(a.z, a.w)) >= 0.0f)
		return tex.SampleLevel(smp_rtlinear, uv, 0);

	uint w, h;
	tex.GetDimensions(w, h);
	const float2 st = uv * float2(w, h) - 0.5f;
	const float2 f = frac(st);
	const int2 p0 = int2(floor(st));
	const int2 hi = int2(w, h) - 1;
	const float4 c00 = tex.Load(int3(clamp(p0, 0, hi), 0));
	const float4 c10 = tex.Load(int3(clamp(p0 + int2(1, 0), 0, hi), 0));
	const float4 c01 = tex.Load(int3(clamp(p0 + int2(0, 1), 0, hi), 0));
	const float4 c11 = tex.Load(int3(clamp(p0 + int2(1, 1), 0, hi), 0));
	// Bilinear weights of the four, zero for a hidden one (judged on the loaded texel itself).
	const float4 wt = float4((1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y), (1.0f - f.x) * f.y, f.x * f.y)
		* float4(c00.a >= 0.0f, c10.a >= 0.0f, c01.a >= 0.0f, c11.a >= 0.0f);
	const float sum = dot(wt, 1.0f);
	valid = sum > 0.0f;
	[branch] if (!valid)
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	return (c00 * wt.x + c10 * wt.y + c01 * wt.z + c11 * wt.w) / sum;
}

#endif // DA_CLOUDS_HIST_H
