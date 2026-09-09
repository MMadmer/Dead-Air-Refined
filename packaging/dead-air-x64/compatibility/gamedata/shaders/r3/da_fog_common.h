#ifndef DA_FOG_COMMON_H_INCLUDED
#define DA_FOG_COMMON_H_INCLUDED

// One haze for every pass.
//
// The deferred combine and the forward passes each used to fog themselves, which is fine only
// while the numbers agree. They did not. The scene dissolves through three terms - the linear
// weather ramp, a height layer, and a colour taken from the sky along the view ray, all capped
// by a density ceiling - while water and smoke had the linear ramp alone, over a longer
// distance at that. In thick weather that left a lake sitting bright and sharp inside murk
// that had already swallowed the shore around it.
//
// The maths lives here so the two cannot drift again. The sampling does not: the combine reads
// the sky cubes as sky_s0/sky_s1 through smp_rtlinear, water reads the same two cubes as
// s_env0/s_env1 through smp_base, and a shader cannot rename another shader's bindings. So the
// caller takes its own two samples and hands the blended result to da_fog_mix_sky.
//
// The distance ramp itself is NOT here - it arrives already computed, from fog_params in the
// deferred pass and from the fog plane in the forward ones. Both binders derive it from the
// same bounds (Blender_Recorder_StandartBinding.cpp, fog_bounds).

uniform float4 da_fog;      // x sky share, y sky mip, z height density, w height falloff
uniform float4 da_fog2;     // x density ceiling, y layer base altitude, z horizon flattening
uniform float4 da_sky_tint; // rgb the weather's own sky colour

// The rain, as the weather constant the engine already solves it into (binder_da_rain2):
// x = rain rate R in mm/h, y = extinction sigma_ext = 0.312*R^0.67 per KILOMETRE,
// z = the rain quality tier, w reserved. Declared here rather than per-shader because rain
// extinction belongs to the haze: it is what makes a downpour close the distance in.
uniform float4 da_rain2;

// Rain is extinction like any other. Falling water is a suspension of drops with a real
// scattering cross-section, so a 25 mm/h storm removes ~2.7 per km - a third of the far end of
// a 150 m view. This is where the fix for the crown-tears-against-the-sky band lives too: the
// horizon stops being a hard plane once the murk in front of it grows with the weather.
//
// Added as a SECOND independent opacity over the linear ramp, exactly the way the height layer
// is, so fog_near/fog_far keep meaning what they say and dry weather is bit-identical.
float da_fog_rain(float linear_fog, float dist)
{
	[branch] if (da_rain2.y <= 0.001h)
		return linear_fog;

	// The published sigma is per kilometre; the path is in metres.
	const float rainf = 1.0h - exp(-da_rain2.y * 0.001h * max(dist, 0.0h));
	return saturate(linear_fog + rainf * (1.0h - linear_fog));
}

// Height fog, added to a linear factor as a SECOND independent opacity rather than replacing
// it - fog_near/fog_far keep working. Density falls off exponentially with altitude and the
// integral along the view ray is taken analytically; a near-horizontal ray is the degenerate
// case, where the division by the vertical component is unstable, so its limit is a branch.
float da_fog_height(float linear_fog, float3 wp, float3 cam, float dist)
{
	// Folded in here and not at each call site: every fogged pass in the tree already runs
	// through this one function, so the water, the scene, the sky and the streaks cannot end up
	// disagreeing about how far you can see in a storm.
	linear_fog = da_fog_rain(linear_fog, dist);

	[branch] if (da_fog.z <= 0.001h)
		return linear_fog;

	const float b = max(da_fog.w, 0.0001h);
	const float ry = (wp.y - cam.y) / max(dist, 0.001h);
	// Height counts from the LAYER's reference altitude, not from world zero: the player
	// usually stands near zero and the factor came out ~1 for any falloff.
	const float base = exp(-clamp(cam.y - da_fog2.y, -500.0h, 500.0h) * b);
	const float integ = (abs(ry) > 0.0001h)
		? base * (1.0h - exp(-dist * ry * b)) / (ry * b)
		: base * dist;
	// Density is per METRE of path: the 0..4 knob means thousandths, and the scale is applied
	// here rather than in the binder so the raw value survives the branch threshold above.
	// The integral is optical depth; opacity is 1-exp(-tau).
	const float fogH = 1.0h - exp(-da_fog.z * 0.001h * max(integ, 0.0h));
	return saturate(linear_fog + fogH * (1.0h - linear_fog));
}

// Density ceiling: a little contrast at the horizon keeps the hill silhouettes.
float da_fog_ceiling(float fog)
{
	[branch] if (da_fog2.x > 0.001h && da_fog2.x < 0.999h)
		return min(fog, da_fog2.x);
	return fog;
}

// Direction to sample the sky in. Flattened toward the horizon because scattering happens
// along the ray TO the object, and that ray is nearly horizontal. Without this a tall tree was
// painted with the sky ABOVE ITSELF and came out brighter than the murk around it at sunset.
float3 da_fog_sky_dir(float3 wp, float3 cam)
{
	float3 vd = normalize(wp - cam);
	vd.y *= saturate(1.0h - da_fog2.z);
	return normalize(vd);
}

// The colour the haze converges to. sky_sample is lerp(cube0, cube1, weight) taken by the
// caller at mip da_fog.y - blurred on purpose, since fog wants the general tone of a direction
// and not the cloud pattern.
//
// The SKYDOME tint and brightness, not env_color: the cubes are the very skybox textures, but
// the dome on screen is tinted by the weather's sky_color and scaled 2*0.33 (sky2.vs/ps),
// while env_color is the IBL tint - a different value. Fogged silhouettes converged to a
// colour the visible sky NEVER had: the bright-ghosts-in-front-of-dark-sky bug.
float3 da_fog_mix_sky(float3 base_rgb, float3 sky_sample)
{
	const float3 sky = da_sky_tint.rgb * sky_sample * 0.66h;
	return lerp(base_rgb, sky, saturate(da_fog.x));
}

// True when any extra term is worth the branch. Every caller gates on this, which is why the
// rain has to be in it: without the third test a level with no height layer and no sky share
// would skip da_fog_height entirely and the storm would take nothing off the distance.
bool da_fog_extras_on()
{
	return da_fog.x > 0.001h || da_fog.z > 0.001h || da_rain2.y > 0.001h;
}

#endif
