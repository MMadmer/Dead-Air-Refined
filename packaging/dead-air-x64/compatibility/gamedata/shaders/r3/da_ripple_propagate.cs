//	One step of the ripple field's wave motion, in Fourier space (Tessendorf's eWave, 2014).
//
//	The real-space pass (da_water_ripple.ps) packs the field as one complex texture, height in
//	the real part and vertical velocity in the imaginary, and two FFT passes bring it here. Each
//	wavenumber is then an independent harmonic oscillator,
//		d/dt (H, V) = (V, -omega^2 H),   omega^2 = (g k + s k^3) tanh(k D)
//	- gravity, surface tension s = sigma/rho and the water's mean depth D, the full dispersion
//	relation of real water waves - and a harmonic oscillator has an exact solution over a step:
//		H' = H cos(omega dt) + V sin(omega dt) / omega,   V' = V cos(omega dt) - H omega sin(omega dt)
//	No stencil, no Courant number, no wavelength that the grid runs at the wrong speed: every
//	wave the grid can hold moves at exactly its own speed, which is what makes a ring what a
//	ring is on a pond - long waves running out ahead, short ones trailing, the train spreading
//	as it goes. The two leapfrog versions before this could not: one equation has one speed.
//
//	The loss is per wavenumber too: exp(-(gamma0 + nu k^2) dt), the wavelength-blind trickle
//	and a viscosity that takes the short waves first, as a pond does - a surface film damps by
//	k^2 as well, which is why nu is a few times water's own.
//
//	Two real fields ride one complex FFT: with Z = F(h + i v), H = (Z_k + conj Z_-k) / 2 and
//	V = (Z_k - conj Z_-k) / 2i, and Z' = H' + i V' goes back the same way. The mean (k = 0) is
//	pinned at zero: an impact digs a trough, and the water it displaced is not gone.

#include "common.h"

Texture2D<float2> s_fft_in;
RWTexture2D<float2> u_fft_out : register(u0);

//	x = wavenumber per spectrum index, 2 pi / window metres; y = the water body's mean depth in
//	metres; z = the step in seconds; w = the spectrum's size N.
uniform float4 da_ripple_disp;
//	x = surface tension over density, m^3/s^2 (7.28e-5 for water); y = the effective viscosity,
//	m^2/s; z = the wavelength-blind loss, 1/s.
uniform float4 da_ripple_disp2;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const int N = int(da_ripple_disp.w);
	const int2 p = int2(id.xy);
	[branch]
	if (p.x >= N || p.y >= N)
		return;
	const int2 n = int2((N - p.x) % N, (N - p.y) % N);	// the index of -k

	const float2 z = s_fft_in.Load(int3(p, 0));
	const float2 zn = s_fft_in.Load(int3(n, 0));
	float2 H = 0.5f * float2(z.x + zn.x, z.y - zn.y);
	const float2 D = 0.5f * float2(z.x - zn.x, z.y + zn.y);
	float2 V = float2(D.y, -D.x);	// D / i

	const float kx = float((p.x < N / 2) ? p.x : p.x - N) * da_ripple_disp.x;
	const float ky = float((p.y < N / 2) ? p.y : p.y - N) * da_ripple_disp.x;
	const float k = sqrt(kx * kx + ky * ky);
	[branch]
	if (k < 1e-4f)
	{
		u_fft_out[p] = 0.0f;
		return;
	}

	const float w2 = (9.81f * k + da_ripple_disp2.x * k * k * k) * tanh(k * max(da_ripple_disp.y, 0.02f));
	const float w = sqrt(w2);
	const float dt = da_ripple_disp.z;
	float sn, cs;
	sincos(w * dt, sn, cs);
	const float2 Hn = H * cs + V * (sn / w);
	const float2 Vn = V * cs - H * (w * sn);
	const float loss = exp(-(da_ripple_disp2.z + da_ripple_disp2.y * k * k) * dt);
	H = Hn * loss;
	V = Vn * loss;

	u_fft_out[p] = float2(H.x - V.y, H.y + V.x);
}
