// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Blade grass, the push of every creature (docs/dead-air/GRASS.md): one group per deformer stamp of the
// frame (feet, bodies, settled objects; da_snow_field.h) stamps its disc into the camera's toroidal push
// window - strength and direction packed into one uint, the strongest push of a texel wins
// (InterlockedMax). A stamp more than a metre above or below the grass ground there (a roof, a cellar)
// pushes nothing. da_grass_push_resolve.cs turns the frame's stamps into the decaying push field.

ByteAddressBuffer	da_gp_stamps		: register(t0);	// 32 bytes: x, z, foot y, radius | push x, z, strength, 0
Texture2D<float>	da_grass_ground		: register(t3);
RWTexture2D<uint>	da_gp_target		: register(u0);

uniform float4	da_gp_win;		// x texel (m), y window size (texels), zw first world texel of the window
uniform float4	da_gp_count;	// x stamps this frame
uniform float4	da_grass_gwin;	// ground window: xy first texel, z size (texels), w texel (m)

float	grass_ground_at	(float2 xz)
{
	const int2 t = int2(floor(xz / da_grass_gwin.w));
	const int size = int(da_grass_gwin.z);
	const int2 rel = t - int2(da_grass_gwin.xy);
	if (any(rel < 0) || any(rel >= size))
		return -1e30f;
	return da_grass_ground.Load(int3((t % size + size) % size, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	if (gid.x >= uint(da_gp_count.x))
		return;
	const float4 a = asfloat(da_gp_stamps.Load4(gid.x * 32u));
	const float4 b = asfloat(da_gp_stamps.Load4(gid.x * 32u + 16u));
	const float2 c = a.xy;
	const float r = max(a.w, 0.05f);
	const float ground = grass_ground_at(c);
	if (ground < -1e20f || abs(a.z - ground) > 1.0f)
		return;
	const float texel = da_gp_win.x;
	const int size = int(da_gp_win.y);
	const int2 first = int2(da_gp_win.zw);
	const int2 lo = max(int2(floor((c - r) / texel)), first);
	const int2 hi = min(int2(floor((c + r) / texel)), first + size - 1);
	const float speed = length(b.xy);
	const float2 along = speed > 0.05f ? b.xy / speed * saturate(speed / 2.0f) : 0.0f;
	for (int z = lo.y + int(tid.y); z <= hi.y; z += 8)
	{
		for (int x = lo.x + int(tid.x); x <= hi.x; x += 8)
		{
			const float2 dv = (float2(x, z) + 0.5f) * texel - c;
			const float q = dot(dv, dv) / (r * r);
			const float k = saturate(1.0f - q) * saturate(b.z);
			if (k <= 0.0f)
				continue;
			// Outwards from the body and along its motion.
			const float2 dir = normalize(dv / r * 0.8f + along * 0.6f + float2(1e-5f, 0.0f));
			const float ang = frac(atan2(dir.y, dir.x) * 0.15915494f + 1.0f);
			const uint code = (uint(k * 65535.0f) << 16) | uint(ang * 65535.0f);
			InterlockedMax(da_gp_target[uint2((int2(x, z) % size + size) % size)], code);
		}
	}
}
