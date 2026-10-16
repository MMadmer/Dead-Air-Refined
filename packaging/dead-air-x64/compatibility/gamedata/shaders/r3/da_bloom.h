#ifndef DA_BLOOM_H
#define DA_BLOOM_H

// The bloom pyramid (r4_rendertarget_phase_da_bloom.cpp), after DOOM Eternal's: a bright pass with a soft knee and
// temporal stabilisation at half the render size, five downsamples, and from the smallest level up a separable
// Gaussian of each level plus the level below it upsampled, each level by its own weight - a tight core and a wide
// halo from the same light.

// One step: xy one texel of the source in uv along the filter (the blur) or both axes (the downsamples),
// z the weight of this level (the vertical blur), w unused.
uniform float4 da_bloom_step;

// DOOM's downsample: the centre and four bilinear taps one texel out, turned by 30 degrees - 13 texels.
static const float2 da_bloom_ring[4] = { float2(0.866f, 0.5f), float2(-0.5f, 0.866f), float2(-0.866f, -0.5f), float2(0.5f, -0.866f) };

// A Gaussian of sigma 2.5 texels over 17 of them in 9 bilinear taps: the centre and pairs (offset, weight).
static const float da_bloom_w0 = 0.159676f;
static const float2 da_bloom_taps[4] = { float2(1.4403f, 0.263348f), float2(3.3635f, 0.122118f), float2(5.2932f, 0.030573f), float2(7.2315f, 0.004122f) };

float3 da_bloom_gauss(Texture2D src, float2 uv, float2 step)
{
	float3 c = src.SampleLevel(smp_rtlinear, uv, 0).rgb * da_bloom_w0;
	[unroll]
	for (int i = 0; i < 4; ++i)
	{
		const float2 o = step * da_bloom_taps[i].x;
		c += (src.SampleLevel(smp_rtlinear, uv + o, 0).rgb + src.SampleLevel(smp_rtlinear, uv - o, 0).rgb) * da_bloom_taps[i].y;
	}
	return c;
}

// The consumer (combine_2): the finished pyramid times its share, da_bloom_params.x (0 when it did not run).
Texture2D s_da_bloom;
uniform float4 da_bloom_params;

float3 da_bloom_glow(float2 uv)
{
	[branch] if (da_bloom_params.x <= 0.0f)
		return 0.0f;
	return s_da_bloom.SampleLevel(smp_rtlinear, uv, 0).rgb * da_bloom_params.x;
}

#endif
