#ifndef DA_FLUID_RAY_H
#define DA_FLUID_RAY_H

// Rendering for the campfire's fluid volume. The stock ray-cast reads one density channel and
// pushes it through an artist look-up; this one reads the real field - temperature, burn and
// soot - and integrates emission and absorption along the ray, so the flame has a white-yellow
// core, an orange body, red wisps at the edge and dark gaps between the tongues on its own,
// and the smoke above it absorbs and scatters instead of glowing.
//
// The plumbing (ray data, near-plane case, edge re-render) is the engine's; only the sample
// and the accumulation are ours.

#define OCCLUDED_PIXEL_RAYVALUE     float4(1, 0, 0, 100000)
#define Z_EPSILON	0.00001
#define Z_MAX		100000

Texture2D	sceneDepthTex;
Texture3D	colorTex;
Texture2D	rayDataTex;
Texture2D	rayCastTex;
Texture2D	edgeTex;
Texture2D	jitterTex;

sampler	samPointClamp;
sampler	samLinearClamp;
sampler	samRepeat;

cbuffer FluidRenderConfig
{
	float		RTWidth;
	float		RTHeight;
	float4		DiffuseLight;
	float4x4	WorldViewProjection;
	float4x4	InvWorldViewProjection;
	float		ZNear;
	float		ZFar;
	float4		gridDim;
	float4		recGridDim;
	float		maxGridDim;
	float		gridScaleFactor;
	float4		eyeOnGrid;
}

cbuffer DaFireRender
{
	float4	da_fr_a;	// x = emission gain, y = soot absorption, z = smoke albedo, w = ember gain
	float4	da_fr_b;	// xyz = the fire's own light on its smoke, w = the temperature the core reaches
	float4	da_fr_c;	// x = smoke gain, y = how much glowing soot absorbs, z = time, w = crossover
	float4	da_fr_d;	// xyz = one step toward the sun in grid space, w = how hard the plume shades itself
	float4	da_fr_e;	// rgb = the sun on the plume, w = how steeply emission climbs with temperature
	float4	da_fr_f;	// x = how many cells before a face of the box everything fades out
}

struct VS_INPUT
{
	float3 pos		: POSITION;
};

struct PS_INPUT_RAYCAST
{
	float4 pos		: SV_Position;
	float3 posInGrid: POSITION;
};

//////////////////////////////////////////////////////////////////////////////////////////
//	The flame's colour by temperature. Not a fitted blackbody: a camera and the dark-adapted
//	eye both clip a fire's core to white, and every reference photo of a campfire at night
//	shows the same four bands - dull red at the tips, orange through the body, yellow, and a
//	white-yellow core. The values run well past one on purpose; the core is meant to blow out.
float3 da_fr_ramp(float t)
{
	float3 c = lerp(float3(0.55, 0.05, 0.01), float3(1.00, 0.26, 0.02), smoothstep(0.06, 0.34, t));
	c = lerp(c, float3(1.70, 0.72, 0.10), smoothstep(0.32, 0.66, t));
	c = lerp(c, float3(3.10, 2.05, 0.80), smoothstep(0.62, 1.00, t));
	//	Past this the ramp is only telling the exposure to clip: a real flame never gets there.
	c = lerp(c, float3(5.60, 4.40, 2.90), smoothstep(0.96, 1.55, t));
	return c;
}

//////////////////////////////////////////////////////////////////////////////////////////
//	One sample along the ray, front to back. The weight is a length in metres: without it a
//	long ray would simply add up more emission than a short one through the same flame.
void DaSample(float weight, float3 O, inout float3 radiance, inout float trans)
{
	const float3 texcoords = float3(O.x, 1.0 - O.y, O.z);
	float4 s = colorTex.SampleLevel(samLinearClamp, texcoords, 0);

	//	The grid has walls, and a cloud that reaches one would otherwise be sliced off square.
	//	Everything fades out over the last few cells before every face. In cells, not metres:
	//	a campfire's box is two metres across and a blast's is ten, and a fade wide enough for
	//	the second would swallow the first whole.
	const float3 eo = min(O, 1.0 - O) * gridDim.xyz;
	const float ef = saturate(min(min(eo.x, eo.y), eo.z) / max(da_fr_f.x, 1.0));
	s *= ef * ef * (3.0 - 2.0 * ef);

	const float T = s.x / max(da_fr_b.w, 0.05);
	const float burn = s.z;
	const float smoke = s.w * da_fr_c.x;

	//	Hot soot radiates, and it radiates as the fourth power of its temperature, which is
	//	what leaves the dark gaps between the tongues: a little below a dull red there is
	//	simply nothing to see. The gas that is reacting right now carries the most of it.
	const float Tn = saturate(T);
	const float soot = 0.20 + 8.0 * burn + 0.60 * s.w;
	const float3 emission = da_fr_ramp(T) * pow(Tn, da_fr_e.w) * soot * da_fr_a.x;

	//	Cooled soot swallows light. Inside a flame the same soot is the thing doing the
	//	glowing, and a plume that absorbed there would hide the fire behind its own smoke -
	//	which is exactly what a flame does not do. A fireball is the opposite case: it is
	//	optically thick while it burns, and what you see is its surface, so a blast lets its
	//	hot soot absorb as well and gets a lit face and a dark limb out of it.
	const float cool = saturate(1.0 - T * 1.2);
	const float opac = lerp(cool, 1.0, da_fr_c.y);
	const float sigma = da_fr_a.y * smoke * opac;

	//	One tap toward the sun: whatever soot is between this parcel and the sun is what
	//	shades it. A single sample is enough to turn a flat grey ball into a lit one.
	float lit = 1.0;
	if (da_fr_d.w > 0.0 && smoke > 0.02)
	{
		const float ahead = colorTex.SampleLevel(samLinearClamp, texcoords + da_fr_d.xyz, 0).w;
		lit = exp(-da_fr_d.w * ahead);
	}
	const float3 scatter = (DiffuseLight.rgb + da_fr_e.rgb * lit
		+ da_fr_b.rgb * saturate(T * 2.0 + 0.15 * s.w)) * (da_fr_a.z * smoke * opac);

	radiance += trans * (emission + scatter) * weight;
	trans *= exp(-sigma * weight);
}

//////////////////////////////////////////////////////////////////////////////////////////
float4 DaRaycast(PS_INPUT_RAYCAST input)
{
	float4 rayData = rayDataTex.Sample(samLinearClamp, float2(input.pos.x / RTWidth, input.pos.y / RTHeight));

	//	The scene occludes the whole ray.
	if (rayData.x < 0)
		return 0;

	//	The near plane clipped the front face away: start at the fragment itself.
	if (rayData.y < 0)
	{
		rayData.xyz = input.posInGrid;
		rayData.w = rayData.w - ZNear;
	}

	const float3 rayOrigin = rayData.xyz;
	const float Offset = jitterTex.Sample(samRepeat, input.pos.xy / 256.0).r;
	const float rayLength = rayData.w;

	//	Two samples per voxel, jittered by a screen-space pattern so the steps never band.
	const float fSamples = (rayLength / gridScaleFactor * maxGridDim) * 2.0;
	const int nSamples = floor(fSamples);
	const float3 stepVec = normalize((rayOrigin - eyeOnGrid.xyz) * gridDim.xyz) * recGridDim.xyz * 0.5;

	float3 O = rayOrigin + stepVec * Offset;
	float3 radiance = 0;
	float trans = 1.0;

	//	How far one step is in the world: the box's longest side over its cell count, halved
	//	because we sample twice per cell.
	const float stepLen = gridScaleFactor / max(maxGridDim, 1.0) * 0.5;

	int i;
	for (i = 0; i < nSamples; ++i)
	{
		DaSample(stepLen, O, radiance, trans);
		O += stepVec;
		if (trans < 0.01)
			break;
	}
	if (i == nSamples)
		DaSample(frac(fSamples) * stepLen, O, radiance, trans);

	//	Where the ray ran into something inside the box, that something is the wood the fire
	//	stands on. The gas a step back from it is what is heating it, so the embers glow with
	//	the heat that is actually there instead of a painted-on mask.
	if (da_fr_a.w > 0.0 && trans > 0.02)
	{
		const float3 E = O - stepVec * 1.5;
		if (all(E > 0.001) && all(E < 0.999))
		{
			const float4 se = colorTex.SampleLevel(samLinearClamp, float3(E.x, 1.0 - E.y, E.z), 0);
			const float Te = se.x / max(da_fr_b.w, 0.05);
			radiance += trans * da_fr_ramp(Te * 0.85) * saturate(Te * 1.6) * saturate(Te * 1.6) * da_fr_a.w;
		}
	}

	//	Premultiplied: the emission is not attenuated by the smoke's own opacity. The ceiling
	//	is on the brightness, not on each channel: clamping the channels separately drags a
	//	hot amber core to white and throws its hue away.
	const float lum = dot(radiance, float3(0.2126, 0.7152, 0.0722));
	if (lum > 40.0)
		radiance *= 40.0 / lum;
	return float4(radiance * da_fr_c.w, (1.0 - trans) * da_fr_c.w);
}

#endif // DA_FLUID_RAY_H
