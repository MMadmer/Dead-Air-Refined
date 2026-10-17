#include "common.h"
#define DA_FROXEL_NO_SAMPLE
#include "da_froxel.h"
#include "da_clouds.h"

// The sunlit air of every froxel (da_froxel.h): its density, the sun it sees through the cascades and the clouds,
// DOOM Eternal's depth jitter inside the slice, and the previous frame's value reprojected into it. rgb: the light the
// air scatters per metre (isotropic: the shafts must not crowd the sun, see the history of accum_volumetric_sun.ps),
// a: its extinction per metre.

Texture2DArray s_smap_array;	// the sun cascades, slice = cascade
SamplerComparisonState smp_smap;
Texture2D s_cloud_map;
Texture2D s_blue_noise;
Texture3D s_froxel_prev;
RWTexture3D<float4> u_froxel : register(u0);

uniform float4x4 m_froxel_v2w;		// view -> world
uniform float4x4 m_froxel_reproj;	// view -> the previous frame's clip space (unjittered)
uniform float4x4 m_froxel_shadow0;	// view -> cascade texture space, near
uniform float4x4 m_froxel_shadow1;	// middle
uniform float4x4 m_froxel_shadow2;	// far
// x: scattering per metre at the eye's height, y: its fall-off with height per metre, z: the noise's share, w: unused.
uniform float4 da_froxel_media;
// xyz: the medium's drift with the wind (m, wrapped at the noise's period), w: the noise's frequency (1 / m).
uniform float4 da_froxel_wind;
// rgb: the colour of the light the air scatters, times the weather's strength; w: 1 when the history is usable.
uniform float4 da_froxel_sun;
// x: this frame's depth jitter phase 0..1, y: the cascades to read, z: the history's share kept, w: unused.
uniform float4 da_froxel_frame;

// Value noise over a lattice periodic in 256 cells (the wind's drift wraps at that period), 0..1.
float da_froxel_noise(float3 p)
{
	const float3 i = floor(p);
	float3 f = p - i;
	f = f * f * (3.0f - 2.0f * f);
	const float3 a = i - 256.0f * floor(i * (1.0f / 256.0f));
	const float3 b = (a + 1.0f) - 256.0f * floor((a + 1.0f) * (1.0f / 256.0f));
	const float c000 = da_lattice_hash3(a.x, a.y, a.z, 11u), c100 = da_lattice_hash3(b.x, a.y, a.z, 11u);
	const float c010 = da_lattice_hash3(a.x, b.y, a.z, 11u), c110 = da_lattice_hash3(b.x, b.y, a.z, 11u);
	const float c001 = da_lattice_hash3(a.x, a.y, b.z, 11u), c101 = da_lattice_hash3(b.x, a.y, b.z, 11u);
	const float c011 = da_lattice_hash3(a.x, b.y, b.z, 11u), c111 = da_lattice_hash3(b.x, b.y, b.z, 11u);
	return lerp(lerp(lerp(c000, c100, f.x), lerp(c010, c110, f.x), f.y),
		lerp(lerp(c001, c101, f.x), lerp(c011, c111, f.x), f.y), f.z);
}

// Thinner with height above the eye; wisps drifting with the wind, the mean density kept.
float da_froxel_density(float3 Pw)
{
	float d = da_froxel_media.x * exp(-clamp(Pw.y - eye_position.y, -40.0f, 400.0f) * da_froxel_media.y);
	[branch] if (da_froxel_media.z > 0.0f)
	{
		const float3 q = (Pw + da_froxel_wind.xyz) * da_froxel_wind.w;
		const float n = da_froxel_noise(q) * 0.65f + da_froxel_noise(q * 2.0f + 31.0f) * 0.35f;
		d *= lerp(1.0f, smoothstep(0.2f, 0.8f, n) * 2.0f, da_froxel_media.z);
	}
	return d;
}

float da_froxel_cascade(float4x4 m, float3 Pv, float slice, inout bool found)
{
	const float4 s = mul(m, float4(Pv, 1.0f));
	const float3 t = s.xyz / s.w;
	[branch] if (all(abs(t.xy - 0.5f) < 0.495f) && t.z > 0.0f && t.z < 1.0f)
	{
		found = true;
		return s_smap_array.SampleCmpLevelZero(smp_smap, float3(t.xy, slice), t.z).x;
	}
	return 1.0f;
}

// The sun through the finest cascade that holds the point; beyond them all the air is lit.
float da_froxel_sun_vis(float3 Pv)
{
	bool found = false;
	float v = da_froxel_cascade(m_froxel_shadow0, Pv, 0.0f, found);
	[branch] if (!found && da_froxel_frame.y > 1.5f)
		v = da_froxel_cascade(m_froxel_shadow1, Pv, 1.0f, found);
	[branch] if (!found && da_froxel_frame.y > 2.5f)
		v = da_froxel_cascade(m_froxel_shadow2, Pv, 2.0f, found);
	return v;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (any(id >= uint3(da_froxel_grid.xyz)))
		return;

	// Jittered in depth only, inside the slice: the frame's phase and a blue-noise offset per column.
	const float2 bn = s_blue_noise.Load(int3(id.xy & 127u, 0)).xy;
	const float jitter = frac(da_froxel_frame.x + bn.x);
	const float2 uv = (float2(id.xy) + 0.5f) / da_froxel_grid.xy;
	const float3 Pv = da_froxel_view(uv, da_froxel_depth(float(id.z) + jitter));
	const float3 Pw = mul(m_froxel_v2w, float4(Pv, 1.0f)).xyz;

	const float sigma = da_froxel_density(Pw);
	float4 cur = 0.0f;
	[branch] if (sigma > 0.0f)
	{
		const float vis = da_froxel_sun_vis(Pv) * da_cloud_transmittance(s_cloud_map, Pw);
		cur = float4(da_froxel_sun.rgb * (sigma * vis), sigma);
	}

	// The froxel's centre in the previous frame: its share of the history while it was on screen and in the grid.
	[branch] if (da_froxel_sun.w > 0.5f)
	{
		const float4 prev = mul(m_froxel_reproj, float4(da_froxel_view(uv, da_froxel_depth(float(id.z) + 0.5f)), 1.0f));
		[branch] if (prev.w > da_froxel_range.x)
		{
			const float3 tc = float3(prev.xy / prev.w * float2(0.5f, -0.5f) + 0.5f, da_froxel_slice(prev.w) / da_froxel_grid.z);
			[branch] if (all(tc == saturate(tc)))
				cur = lerp(cur, s_froxel_prev.SampleLevel(smp_rtlinear, tc, 0), da_froxel_frame.z);
		}
	}
	u_froxel[id] = cur;
}
