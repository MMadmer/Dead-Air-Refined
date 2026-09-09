#ifndef DA_RIPPLE_FFT_H
#define DA_RIPPLE_FFT_H

//	One pass of a radix-2 Stockham FFT over every line of a complex RG32F texture: along X with
//	one thread group per row, or along Y with one per column. Two passes make a 2D transform.
//	Stockham rather than Cooley-Tukey because it sorts itself - the output comes out in natural
//	order with no bit-reversal permutation, which on a GPU is the difference between two
//	coalesced writes per butterfly and a scatter.
//
//	The line lives in group-shared memory for the whole transform (two buffers of N complex
//	values, 16 KB at N = 1024, half the limit), each thread owns one butterfly per stage, and
//	the stages are separated by a barrier. The butterfly index j and the stage size Ns give
//		k = j mod Ns,   w = exp(sign * 2 pi i k / 2Ns),
//		out[2(j - k) + k] = in[j] + w in[j + N/2],   out[2(j - k) + k + Ns] = in[j] - w in[j + N/2]
//	which is the textbook GPU Stockham (Govindaraju et al.), checked here against numpy before
//	it was typed. sign is -1 forward and +1 inverse; the inverse's 1/N goes on through the
//	scale control, one axis at a time.
//
//	DA_FFT_N is set by the including .cs (da_ripple_fft_256/512/1024.cs): the group size and
//	the shared arrays have to be compile-time, so the ripple field's three resolutions are
//	three shaders of the same body.

#include "common.h"

Texture2D<float2> s_fft_in;
RWTexture2D<float2> u_fft_out : register(u0);

//	x = axis (0 = along X, one group per row; 1 = along Y, one group per column), y = 1 for the
//	inverse transform, z = the scale applied on the way out (1/N for one axis of the inverse).
uniform float4 da_fft_ctl;

groupshared float2 sh_fft[2][DA_FFT_N];

[numthreads(DA_FFT_N / 2, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	const uint ln = gid.x;
	const uint j = tid.x;
	const bool cols = da_fft_ctl.x > 0.5f;
	const float sign = (da_fft_ctl.y > 0.5f) ? 1.0f : -1.0f;

	const int2 p0 = cols ? int2(ln, j) : int2(j, ln);
	const int2 p1 = cols ? int2(ln, j + DA_FFT_N / 2) : int2(j + DA_FFT_N / 2, ln);
	sh_fft[0][j] = s_fft_in.Load(int3(p0, 0));
	sh_fft[0][j + DA_FFT_N / 2] = s_fft_in.Load(int3(p1, 0));
	GroupMemoryBarrierWithGroupSync();

	uint src = 0;
	[loop]
	for (uint Ns = 1; Ns < DA_FFT_N; Ns <<= 1)
	{
		const uint k = j & (Ns - 1);
		float sn, cs;
		sincos(sign * 6.2831853f * float(k) / float(2 * Ns), sn, cs);
		const float2 a = sh_fft[src][j];
		const float2 b = sh_fft[src][j + DA_FFT_N / 2];
		const float2 bw = float2(b.x * cs - b.y * sn, b.x * sn + b.y * cs);
		const uint d = (j - k) * 2 + k;
		sh_fft[src ^ 1][d] = a + bw;
		sh_fft[src ^ 1][d + Ns] = a - bw;
		GroupMemoryBarrierWithGroupSync();
		src ^= 1;
	}

	u_fft_out[p0] = sh_fft[src][j] * da_fft_ctl.z;
	u_fft_out[p1] = sh_fft[src][j + DA_FFT_N / 2] * da_fft_ctl.z;
}

#endif	// DA_RIPPLE_FFT_H
