// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The current snow depth for any shader (docs/dead-air/SNOW.md):
//   float  da_snow_depth(float2 xz)       current snow depth (m) at a world position, trails included
//   float  da_snow_depth_prev(float2 xz)  the same for the previous frame (motion vectors)
//   float2 da_snow_depth2(float2 xz)      both at once (one set of fetches)
// Every shader that includes this binds s_da_snow_fine = $user$da_snow_fine and
// s_da_snow_level = $user$da_snow_level in its blender (one shader stage per name and pass); the
// constants come from the standard binders. Both textures are R32_UINT: the low 16 bits hold the
// current depth, the high 16 bits the previous frame's, unorm over [0, cap]. The fine texture is the
// trail window around the camera (toroidal: a texel is the world texel modulo its size; its season depth
// a cubic B-spline of the level fields, smooth in slope), the level texture covers the whole level at 1 m
// with the coarse trails. Integer textures are read with Load, so the functions work in every stage
// without a sampler.
#ifndef DA_SNOW_SAMPLE_H
#define DA_SNOW_SAMPLE_H

Texture2D<uint> s_da_snow_fine;
Texture2D<uint> s_da_snow_level;

uniform float4 da_snow_fine_xf;  // x texels per metre, y texels per side, zw window centre xz (m)
uniform float4 da_snow_level_xf; // texel coordinates = xz * xy + zw (texel centres at +0.5)
uniform float4 da_snow_misc;     // x cap (m), y fine window half-size (m), z 1 = depth present, w fine->level blend (m)

float2 da_snow_unpack2(uint v)
{
	return float2(float(v & 0xFFFFu), float(v >> 16)) * (da_snow_misc.x / 65535.0);
}

float2 da_snow_fine2(float2 xz)
{
	const float n = da_snow_fine_xf.y;
	const float2 p = xz * da_snow_fine_xf.x - 0.5;
	const float2 i = floor(p);
	const float2 f = p - i;
	// World texel modulo the window size; exact in float for any level coordinate.
	const float2 a = i - n * floor(i / n);
	const float2 b = a + 1.0 - n * step(n, a + 1.0);
	const float2 d00 = da_snow_unpack2(s_da_snow_fine.Load(int3(int(a.x), int(a.y), 0)));
	const float2 d10 = da_snow_unpack2(s_da_snow_fine.Load(int3(int(b.x), int(a.y), 0)));
	const float2 d01 = da_snow_unpack2(s_da_snow_fine.Load(int3(int(a.x), int(b.y), 0)));
	const float2 d11 = da_snow_unpack2(s_da_snow_fine.Load(int3(int(b.x), int(b.y), 0)));
	return lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
}

float2 da_snow_level2(float2 xz)
{
	const float2 p = xz * da_snow_level_xf.xy + da_snow_level_xf.zw - 0.5;
	const float2 i = floor(p);
	const float2 f = p - i;
	const int2 t = int2(i);
	// Outside the level a Load returns zero: no snow there.
	const float2 d00 = da_snow_unpack2(s_da_snow_level.Load(int3(t, 0)));
	const float2 d10 = da_snow_unpack2(s_da_snow_level.Load(int3(t + int2(1, 0), 0)));
	const float2 d01 = da_snow_unpack2(s_da_snow_level.Load(int3(t + int2(0, 1), 0)));
	const float2 d11 = da_snow_unpack2(s_da_snow_level.Load(int3(t + int2(1, 1), 0)));
	return lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
}

float2 da_snow_depth2(float2 xz)
{
	float2 v = float2(0.0, 0.0);
	[branch] if (da_snow_misc.z > 0.5)
	{
		const float2 d = abs(xz - da_snow_fine_xf.zw);
		// 1 inside the fine window, 0 past its edge, a linear blend over the last da_snow_misc.w metres.
		const float wf = saturate((da_snow_misc.y - max(d.x, d.y)) / max(da_snow_misc.w, 1e-3));
		[branch] if (wf < 1.0)
			v = da_snow_level2(xz);
		[branch] if (wf > 0.0)
			v = lerp(v, da_snow_fine2(xz), wf);
	}
	return v;
}

float da_snow_depth(float2 xz) { return da_snow_depth2(xz).x; }
float da_snow_depth_prev(float2 xz) { return da_snow_depth2(xz).y; }

#endif // DA_SNOW_SAMPLE_H
