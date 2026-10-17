#ifndef DA_MBLUR_H
#define DA_MBLUR_H

// Motion blur (r4_rendertarget_phase_da_mblur.cpp, the option r__mblur), the reconstruction filter of McGuire et al.
// 2012 as DOOM Eternal runs it: the largest motion of every 20x20 tile and of the tiles round it, then a gather along
// that motion whose samples each count by whether they are in front of the pixel or behind it, how far they move
// and how far they lie - a moving edge smears over what is behind it and nothing behind it smears over the edge.
// All lengths are in render pixels; the motion is half of what moves during the shutter, each way from the pixel.

Texture2D s_da_taa_velocity;	// NDC(current) - NDC(previous), y up; read while da_mblur.z is 1
Texture2D s_da_mblur_vz;		// the half motion (render pixels) and the depth of every render pixel
Texture2D s_da_mblur_tiles;		// the largest motion of each tile and its neighbours, render pixels

// x: the shutter, the share of a frame's motion the blur spans (0: no blur this frame); y: the longest half motion,
// render pixels (the tile size); z: 1 when the motion vectors are this frame's, else the camera's motion from the
// depth; w: unused.
uniform float4 da_mblur;
uniform float4 da_mblur_size;		// the render size in pixels, its inverse
uniform float4x4 m_da_mblur_prev;	// this frame's view space to the previous frame's clip space

// The first-person hands and item (nearer than a held item reaches, see da_taa.ps) blur a quarter as much: they
// move with the eye, and a turn smearing the weapon reads as a fault.
static const float da_mblur_hud_z = 1.10f;

float da_mblur_depth(float2 uv)
{
#ifdef USE_MSAA
	// A multisampled position target has no filtered read: its first sample.
	const float z = s_position.Load(int2(uv * pos_decompression_params2.xy), 0).z;
#else
	const float z = s_position.SampleLevel(smp_nofilter, uv, 0).z;
#endif
	return z > 0.01f ? z : 1e4f;
}

// The half motion at uv in render pixels, at most da_mblur.y long.
float2 da_mblur_motion(float2 uv, float z)
{
	float2 motion;
	[branch] if (da_mblur.z > 0.5f)
		motion = s_da_taa_velocity.SampleLevel(smp_nofilter, uv, 0).xy * float2(0.5f, -0.5f);
	else
	{
		// No vectors this frame: the camera's motion through the pixel's depth (the sky at a great distance).
		const float2 ray = uv * pos_decompression_params2.xy * pos_decompression_params.zw - pos_decompression_params.xy;
		const float4 prev = mul(m_da_mblur_prev, float4(float3(ray, 1.0f) * z, 1.0f));
		motion = prev.w > 0.0f ? uv - (prev.xy / prev.w * float2(0.5f, -0.5f) + 0.5f) : 0.0f;
	}
	float2 v = motion * da_mblur_size.xy * (0.5f * da_mblur.x);
	if (z < da_mblur_hud_z)
		v *= da_mblur.z > 0.5f ? 0.25f : 0.0f;
	const float l = length(v);
	return l > da_mblur.y ? v * (da_mblur.y / l) : v;
}

float da_mblur_cone(float d, float l) { return saturate(1.0f - d / l); }
float da_mblur_cylinder(float d, float l) { return 1.0f - smoothstep(0.95f * l, 1.05f * l, d); }

// The colour of src at uv, blurred along the motion round it; pos is the pixel's screen position (the jitter).
float3 da_mblur_gather(Texture2D src, float2 uv, float3 c, float2 pos)
{
	[branch] if (da_mblur.x <= 0.0f)
		return c;
	// Filtered between the tiles (DOOM's way): a hard step of the motion at every tile edge draws the tile grid.
	const float2 vmax = s_da_mblur_tiles.SampleLevel(smp_rtlinear, uv, 0).xy;
	const float lmax = length(vmax);
	[branch] if (lmax < 0.5f)
		return c;

	const float3 center = s_da_mblur_vz.SampleLevel(smp_nofilter, uv, 0).xyz;
	const float zc = center.z;
	const float lc = max(length(center.xy), 0.5f);
	// The sample positions differ pixel to pixel instead of banding. White noise: interleaved gradient noise drew
	// its diagonal lattice over the blur, which no temporal pass after this one resolves.
	// An integer hash of the pixel: a sine's range reduction at these arguments is coarse and patterned on some GPUs,
	// and DXC may reorder the float maths (da_lattice.h).
	uint h = uint(pos.x) * 1973u + uint(pos.y) * 9277u + 26699u;
	h = (h ^ (h >> 15)) * 0x2c1b3c6du;
	h = (h ^ (h >> 12)) * 0x297a2d39u;
	h ^= h >> 15;
	const float jitter = float(h & 0xffffu) * (1.0f / 65536.0f) - 0.5f;
	// A tap per two pixels of the motion, 4 to 12.
	const int taps = clamp(int(lmax * 0.5f + 0.5f) * 2, 4, 12);
	float total = 1.0f / lc;
	float3 sum = c * total;
	[loop]
	for (int i = 0; i < taps; ++i)
	{
		const float t = lerp(-1.0f, 1.0f, (float(i) + jitter + 1.0f) / float(taps + 1));
		const float2 uv_s = uv + vmax * t * da_mblur_size.zw;
		const float d = abs(t) * lmax;
		const float3 vz = s_da_mblur_vz.SampleLevel(smp_nofilter, uv_s, 0).xyz;
		const float zs = vz.z;
		const float ls = max(length(vz.xy), 0.5f);
		// In front of the pixel / behind it, softened over a twentieth of the depth.
		const float soft = 0.1f + 0.05f * min(zc, zs);
		const float front = saturate(1.0f - (zs - zc) / soft);
		const float back = saturate(1.0f - (zc - zs) / soft);
		const float w = front * da_mblur_cone(d, ls) + back * da_mblur_cone(d, lc) +
			da_mblur_cylinder(d, ls) * da_mblur_cylinder(d, lc) * 2.0f;
		sum += src.SampleLevel(smp_rtlinear, uv_s, 0).rgb * w;
		total += w;
	}
	return sum / total;
}

#endif
