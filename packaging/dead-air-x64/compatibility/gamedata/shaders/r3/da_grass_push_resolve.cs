// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Blade grass, the push field (docs/dead-air/GRASS.md): one thread per texel of the camera's toroidal
// window. The previous field decays towards rest (the blades spring back over a few seconds), this
// frame's stamps (da_grass_push_stamp.cs) override where they push harder, and a texel the window has
// just scrolled onto starts at rest. The stamp target is cleared for the next frame on the way.

Texture2D<float2>		da_gp_prev		: register(t0);
RWTexture2D<uint>		da_gp_target	: register(u0);
RWTexture2D<float2>		da_gp_out		: register(u1);

uniform float4	da_gp_win;		// x texel (m), y window size (texels), zw first world texel of the window
uniform float4	da_gp_win_prev;	// zw first world texel of the previous frame's window
uniform float4	da_gp_decay;	// x decay factor of this frame, y 1 = the previous field is valid

int2	grass_world_texel	(int2 t, int2 first, int size)
{
	return first + ((t - first) % size + size) % size;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const int size = int(da_gp_win.y);
	if (id.x >= uint(size) || id.y >= uint(size))
		return;
	const uint code = da_gp_target[id.xy];
	da_gp_target[id.xy] = 0u;
	const int2 t = int2(id.xy);
	const bool same = all(grass_world_texel(t, int2(da_gp_win.zw), size) == grass_world_texel(t, int2(da_gp_win_prev.zw), size));
	const float2 old = (same && da_gp_decay.y > 0.5f) ? da_gp_prev.Load(int3(t, 0)) * da_gp_decay.x : 0.0f;
	float2 push = old;
	if (code != 0u)
	{
		const float k = float(code >> 16) / 65535.0f;
		const float ang = float(code & 0xFFFFu) / 65535.0f * 6.2831853f;
		const float2 fresh = float2(cos(ang), sin(ang)) * k;
		if (dot(fresh, fresh) > dot(old, old))
			push = fresh;
	}
	da_gp_out[id.xy] = push;
}
