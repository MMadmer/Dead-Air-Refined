#ifndef DA_FLUID_H
#define DA_FLUID_H

#include "fluid_common.h"

// The campfire's own simulation, on top of the engine's 3D fluid grid. The field carried in
// the colour texture is (temperature, fuel, burn, smoke) instead of the stock single density,
// and the fuel is not a blob in mid-air: every cell that sits directly on top of a solid one
// inside the fire's disc feeds the flame, so the flame's base follows the logs, the ground or
// whatever else the campfire stands on, cell by cell.
//
// Everything here is in grid units: one step of the simulation is one unit of time and a
// velocity of 1 means one cell per step. The C++ side converts the real seconds and metres.

cbuffer DaFireSim
{
	float4	da_ff_src;		// xyz = the fire's centre in cells, w = its radius in cells
	float4	da_ff_burn;		// x = ignition temperature, y = burn per degree, z = degrees per burn, w = cooling
	float4	da_ff_burn2;	// x = fuel per burn, y = smoke per burn, z = expansion per burn, w = fuel at the bed
	float4	da_ff_wind;		// xyz = the wind in cells per step, w = how fast a parcel takes it up
	float4	da_ff_misc;		// x = buoyancy, y = injection speed, z = time in seconds, w = bed half-height in cells
	float4	da_ff_misc2;	// x = smoke fade, y = velocity damping, z = coupling rate, w = puffing frequency
	float4	da_ff_blast;	// x = how much of the charge is still going in, y = its radius in cells,
							// z = its outward speed, w = the temperature it starts at (0 = not a blast)
	float4	da_ff_misc3;	// x = the buoyancy soot keeps once the flame in it has gone out,
							// y = the ground jet, z = the dust it tears up, w = how much of that run is left
	float4	da_ff_misc4;	// x = the blast's own divergence this step, signed,
							// y = how much wind reaches the gas that is still down at the source,
							// z = the band at the walls the field drains in, w = how fast it drains there
}

//////////////////////////////////////////////////////////////////////////////////////////
//	Noise. Value noise off a hash - no texture, and only the handful of cells that sit on
//	the fuel bed ever evaluate it.

float da_ff_hash(float3 p)
{
	p = frac(p * 0.3183099 + float3(0.71, 0.113, 0.419));
	p *= 17.0;
	return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float da_ff_noise(float3 x)
{
	const float3 i = floor(x);
	float3 f = frac(x);
	f = f * f * (3.0 - 2.0 * f);
	const float n000 = da_ff_hash(i + float3(0, 0, 0));
	const float n100 = da_ff_hash(i + float3(1, 0, 0));
	const float n010 = da_ff_hash(i + float3(0, 1, 0));
	const float n110 = da_ff_hash(i + float3(1, 1, 0));
	const float n001 = da_ff_hash(i + float3(0, 0, 1));
	const float n101 = da_ff_hash(i + float3(1, 0, 1));
	const float n011 = da_ff_hash(i + float3(0, 1, 1));
	const float n111 = da_ff_hash(i + float3(1, 1, 1));
	return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
	            lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

//	Two octaves is enough to break the fuel sheet into separate tongues with dark gaps.
float da_ff_fbm(float3 x)
{
	return da_ff_noise(x) * 0.65 + da_ff_noise(x * 2.17 + 11.3) * 0.35;
}

//	The outward normal of the solid under a cell, from the gradient of the occupancy field.
//	Points away from the wood, so a log's flanks throw their flame sideways before it rolls up.
float3 da_ff_surface_normal(p_fluidsim input)
{
	float3 g;
	g.x = Texture_obstacles.SampleLevel(samPointClamp, LEFTCELL, 0).r
	    - Texture_obstacles.SampleLevel(samPointClamp, RIGHTCELL, 0).r;
	g.y = Texture_obstacles.SampleLevel(samPointClamp, BOTTOMCELL, 0).r
	    - Texture_obstacles.SampleLevel(samPointClamp, TOPCELL, 0).r;
	g.z = Texture_obstacles.SampleLevel(samPointClamp, DOWNCELL, 0).r
	    - Texture_obstacles.SampleLevel(samPointClamp, UPCELL, 0).r;
	return g;
}

//	How strongly this empty cell rests on a solid one. Grid y runs downward, so the cell below
//	in the world is the one the fluid header calls TOP. Two cells count, weaker the further
//	up: a bed of crossed logs is mostly gaps, and a one-cell band leaves a single thin jet
//	where a real fire has flame across its whole width.
float da_ff_on_surface(p_fluidsim input)
{
	if (IsNonEmptyCell(TOPCELL))
		return 1.0;
	const float3 t2 = float3(input.texcoords.x, input.texcoords.y + 2.0 / textureHeight, input.texcoords.z);
	if (IsNonEmptyCell(t2))
		return 0.6;
	return 0.0;
}

//	A blast's charge: a sphere of fuel thrown in over the first moments, broken up so the
//	fireball is lobed from the start rather than a clean ball that has to be roughened later.
float da_ff_blast_target(p_fluidsim input)
{
	if (da_ff_blast.x <= 0.0)
		return 0.0;
	const float3 d = input.cell0 - da_ff_src.xyz;
	const float r = length(d) / max(da_ff_blast.y, 0.5);
	if (r >= 1.0)
		return 0.0;
	const float n = da_ff_fbm(input.cell0 * 0.13 + float3(3.7, 11.3, 7.9));
	return da_ff_burn2.w * da_ff_blast.x * saturate(1.0 - r * r) * saturate(0.55 + 1.6 * (n - 0.5));
}

//	How much fuel this cell should hold: inside the disc, on a surface, near the bed, and
//	broken up by a slow noise so the fire burns as separate tongues instead of one sheet.
float da_ff_fuel_target(p_fluidsim input)
{
	if (da_ff_blast.w > 0.0)
		return da_ff_blast_target(input);

	const float3 d = input.cell0 - da_ff_src.xyz;
	const float r = length(d.xz) / max(da_ff_src.w, 0.5);
	if (r >= 1.0 || abs(d.y) > da_ff_misc.w)
		return 0.0;
	const float band = da_ff_on_surface(input);
	if (band <= 0.0)
		return 0.0;

	//	The noise drifts upward and around slowly: the hot spots wander over the bed the way
	//	the flame front wanders over real embers.
	const float3 q = input.cell0 * 0.19 + float3(0.13, -0.55, 0.07) * da_ff_misc.z;
	const float n = da_ff_fbm(q);
	//	A slow breath over the whole bed at the pool-fire puffing rate.
	const float puff = 0.82 + 0.18 * sin(6.2832 * da_ff_misc2.w * da_ff_misc.z);
	//	Contrast around the mean, not a threshold on it: the bed keeps the same amount of fuel
	//	overall but spends it in patches with dark gaps, which is what tongues are.
	return da_ff_burn2.w * band * saturate(1.0 - r * r) * saturate(0.92 + 2.0 * (n - 0.5)) * puff;
}

#endif // DA_FLUID_H
