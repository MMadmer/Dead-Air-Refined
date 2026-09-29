#include "da_wind_core.h"

// GPU-driven trees (gpudriven.md 3.4): the crown oscillator of every registered root, once per frame -
// FTreeVisual::UpdateWindState on the GPU. Every root integrates every frame (the CPU integrates the
// roots it draws), so a crown entering a view never resumes from a stale state.
//
// gd_root_state, 48 bytes per root (read by da_gd_tree.h):
//   state q, frequency factor, root gust, root deviation      (this frame)
//   the same four as the previous frame drew with            (motion vectors)
//   height, foliage gate, velocity qd, frame of the last integration (bits; 0xFFFFFFFF = not settled)

// x, y = the gust field's scroll (eff_wind_field_ofs), z = gust event, w = wind frozen (QA)
uniform float4 gd_wind0;
// x = time step (clamped as the CPU does), y = history reset, z = tree speed, w = previous tree speed
uniform float4 gd_wind1;
// x = roots, y = frame (24 bits), z = the root wind_dbg watches, w = 1 while wind_dbg is on
uniform float4 gd_wind2;

// 32 bytes: root x, z, omega, height, foliage, 0, 0, 0
ByteAddressBuffer gd_root_static : register(t0);
RWByteAddressBuffer gd_root_state : register(u0);
// wind_dbg: target, q, qd, omega, height of the watched root
RWByteAddressBuffer gd_wind_dbg : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const uint root = id.x;
	if (root >= (uint)gd_wind2.x)
		return;
	const float4 s0 = asfloat(gd_root_static.Load4(root * 32));
	const float foliage = asfloat(gd_root_static.Load(root * 32 + 16));
	const float omega = s0.z;
	const uint a = root * 48;
	float4 now = asfloat(gd_root_state.Load4(a));
	float4 prev = asfloat(gd_root_state.Load4(a + 16));
	float qd = asfloat(gd_root_state.Load(a + 40));
	const uint last = gd_root_state.Load(a + 44);
	const uint frame = (uint)gd_wind2.y;
	const bool settled = last != 0xFFFFFFFFu;
	if (omega <= 0.0f)
		return;

	// What the previous frame drew with, when it integrated this root; otherwise the new state.
	const bool continuous = settled && ((last + 1) & 0xFFFFFFu) == frame && gd_wind1.y < 0.5f;
	const float q_last = now.x, gust_last = now.z, dev_last = now.w;
	const float gust = da_wind_field_gust(s0.x, s0.y, gd_wind0.x, gd_wind0.y);
	const float dev = da_wind_field_dev(s0.x, s0.y, gd_wind0.x, gd_wind0.y);
	const float target = (0.60f + 0.65f * gust) * (1.0f + 0.6f * gd_wind0.z);

	float q = now.x;
	// The first frame starts at the field; frozen wind holds the crown there.
	if (!settled || gd_wind0.w > 0.5f)
	{
		q = target;
		qd = 0.0f;
	}
	const float dt = gd_wind1.x;
	const float zeta = 0.30f;
	const uint steps = max(1u, (uint)(dt * omega * 4.0f) + 1u);
	const float h = dt / (float)steps;
	for (uint i = 0; i < steps; ++i)
	{
		const float acc = omega * omega * (target - q) - 2.0f * zeta * omega * qd;
		qd += acc * h;
		q += qd * h;
	}
	q = clamp(q, 0.35f, 1.6f);

	const float freq_k = clamp(omega / gd_wind1.z, 0.4f, 6.0f);
	const float freq_k_prev = clamp(omega / gd_wind1.w, 0.4f, 6.0f);
	now = float4(q, freq_k, gust, dev);
	prev = continuous ? float4(q_last, freq_k_prev, gust_last, dev_last) : float4(q, freq_k_prev, gust, dev);
	gd_root_state.Store4(a, asuint(now));
	gd_root_state.Store4(a + 16, asuint(prev));
	gd_root_state.Store4(a + 32, uint4(asuint(s0.w), asuint(foliage), asuint(qd), frame));

	if (gd_wind2.w > 0.5f && root == (uint)gd_wind2.z)
	{
		gd_wind_dbg.Store4(0, asuint(float4(target, q, qd, omega)));
		gd_wind_dbg.Store(16, asuint(s0.w));
	}
}
