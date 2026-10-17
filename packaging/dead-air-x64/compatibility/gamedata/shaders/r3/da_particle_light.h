#ifndef DA_PARTICLE_LIGHT_H
#define DA_PARTICLE_LIGHT_H

#define DA_FROXEL_NO_SAMPLE
#include "da_froxel.h"

// Lit particles (da_particle_light.cpp), after DOOM Eternal: the blended particles take the sun with its shadow (the
// froxel volume's sun visibility at the vertex), the sky, the ambient and the frame's nearest local lights, through
// the scene's exposure and tone curve, as the light a white surface there would show - relative to the light the
// effects were authored for, a white surface at the exposure's white. Additive and multiplying particles are light
// themselves and stay as they are.

Texture3D s_froxel_cur;	// x: the air's density, y: the sun's visibility
// x: 1 = lit (2: QA, the light itself), y: 1 = the froxel volume is this frame's, z: the local lights, w: the tone
// curve's white point.
uniform float4 da_particle_light;
// Per light: position and 1 / range^2; colour and the spot's cosine of its half cone (-2: a point light); direction.
uniform float4 da_particle_lights[24];
// The scene's light terms as combine_1 takes them: the sun light's colour (w: the clouds' share of it); the sky's
// tint (w: the weathers' blend); the ambient.
uniform float4 da_particle_sun;
uniform float4 da_particle_env;
uniform float4 da_particle_amb;
TextureCube env_s0;
TextureCube env_s1;

float3 da_particle_tm(float3 x, float w2) { return x * (1.0f + x / w2) / (1.0f + x); }

float3 da_particle_lighting(float3 Pw)
{
	[branch] if (da_particle_light.x < 0.5f)
		return 1.0f;
	// Outside the volume, or on a frame without it, the sun as the clouds let it through.
	float vis = da_particle_sun.w;
	[branch] if (da_particle_light.y > 0.5f)
	{
		const float3 Pv = mul(m_V, float4(Pw, 1.0f)).xyz;
		[branch] if (Pv.z > da_froxel_range.x)
		{
			const float2 ndc = Pv.xy / (Pv.z * da_froxel_ray.xy);
			const float3 tc = float3(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f, da_froxel_slice(Pv.z) / da_froxel_grid.z);
			[branch] if (all(tc == saturate(tc)))
				vis = s_froxel_cur.SampleLevel(smp_rtlinear, tc, 0).y;
		}
	}
	// A puff takes the sun on its lit half (the diffuse's 0.7 of lmodel.h), and the sky and the ambient from all round:
	// the irradiance cube looked up and to the side, squared like the scene's (hmodel.h).
	const float3 up = lerp(env_s0.SampleLevel(smp_rtlinear, float3(0.0f, 1.0f, 0.0f), 0).rgb,
		env_s1.SampleLevel(smp_rtlinear, float3(0.0f, 1.0f, 0.0f), 0).rgb, da_particle_env.w);
	const float3 side = lerp(env_s0.SampleLevel(smp_rtlinear, float3(0.7f, 0.0f, 0.7f), 0).rgb,
		env_s1.SampleLevel(smp_rtlinear, float3(0.7f, 0.0f, 0.7f), 0).rgb, da_particle_env.w);
	const float3 sky = da_particle_env.rgb * (0.5f * (up + side));
	float3 E = da_particle_sun.rgb * (0.35f * vis) + sky * sky + da_particle_amb.rgb;
	[loop]
	for (int i = 0; i < int(da_particle_light.z); ++i)
	{
		const float4 p = da_particle_lights[i * 3];
		const float4 c = da_particle_lights[i * 3 + 1];
		const float3 to_light = p.xyz - Pw;
		const float r2 = dot(to_light, to_light);
		float att = saturate(1.0f - r2 * p.w);
		att *= att;
		[branch] if (c.w > -1.5f)
			att *= smoothstep(c.w, lerp(c.w, 1.0f, 0.3f), dot(-to_light * rsqrt(max(r2, 1e-4f)), da_particle_lights[i * 3 + 2].xyz));
		E += c.rgb * att;
	}
	const float scale = s_tonemap.Load(int3(0, 0, 0)).x;
	const float w2 = da_particle_light.w * da_particle_light.w;
	return min(da_particle_tm(E * scale, w2) / da_particle_tm(1.0f, w2), 1.5f);
}

#endif
