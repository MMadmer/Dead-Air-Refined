#ifndef	LMODEL_H
#define LMODEL_H

#include "common.h"
#include "da_material_codes.h"
#include "da_snow_light.h"

// The light model of the base tree (configs\shaders\r3\lmodel.h) with the materials above the stock range
// (docs/dead-air/GRASS.md, SNOW.md): codes 27-30 are two-sided foliage, which samples the Oren-Nayar slice
// of s_material and adds the transmission of sunlight through the blade (UE TwoSidedFoliage: a wrapped
// back term with a fixed GGX lobe around the light direction); code 31 is snow, whose term
// da_snow_light.h owns. Every stock material keeps its own slice and its stock result.

#define LIGHT_MUL1 0.7//.7f
#define LIGHT_MUL2 .0//.5f

// Transmission tint: light through a grass blade is warmer than the light on it (combine multiplies by
// the albedo).
#define DA_FOLIAGE_TINT	float3(1.0, 0.95, 0.70)

// The reflection off a leaf's cuticle follows Fresnel (Schlick): faint face-on, strong along the leaf into the light.
// The stock slice's lobe has none, and a field lit from behind the viewer - every blade it sees facing the camera
// and the sun at once, the lobe at its peak - washed out white; into the sun (the sheen GoT shows) it stays whole.
float	da_foliage_fresnel	(float3 V, float3 H)
{
	const float t = 1.0f - saturate(dot(V, H));
	return lerp(0.15f, 1.0f, smoothstep(0.1f, 0.6f, t));
}


// The sun's transmission through a blade of material m (UE TwoSidedBxDF): zero unless m is foliage.
float3	da_foliage_transmission	(float m, float3 N, float3 V, float3 L)
{
	[branch] if (!da_is_foliage(m))
		return 0;
	const float wrap	= saturate((-dot(N, L) + 0.5f) / 2.25f);
	const float x		= saturate(-dot(V, L));
	const float dd		= (x * 0.36f - x) * x + 1.0f;
	const float scat	= 0.36f / (3.14159265f * dd * dd);	// D_GGX(a2 = 0.36): 0.115 side-lit .. 0.884 into the sun
	return LIGHT_MUL1 * da_foliage_tr(m) * wrap * scat * DA_FOLIAGE_TINT;
}

//////////////////////////////////////////////////////////////////////////////////////////
// Lighting formulas			//
float4 plight_infinity( float m, float3 pnt, float3 normal, float3 light_direction )
{
  	float3 N			= normal;							// normal
  	float3 V 		= -normalize	(pnt);					// vector2eye
  	float3 L 		= -light_direction;						// vector2light
  	float3 H			= normalize	(L+V);						// float-angle-vector
	float4 ret;
	float4 light =  s_material.Sample( smp_material, float3( dot(L,N), dot(H,N), da_lut_z(m) ) ).xxxy;		// sample material
	[branch] if (da_is_snowy(m))
		light = da_snow_light(light, N, V, L, da_is_glint(m));
	if (da_is_foliage(m))
		light.w *= da_foliage_fresnel(V, H);
	ret = (light*LIGHT_MUL1+float4(light.www*(Ldynamic_color.xyz*2),light.w)*Ldynamic_color.w/2*LIGHT_MUL2);

	ret.rgb = Vibrance(ret.rgb,2);
	// After Vibrance: the transmission tint is not doubled.
	ret.rgb += da_foliage_transmission(m, N, V, L);

	return ret;
}

float4 plight_local( float m, float3 pnt, float3 normal, float3 light_position, float light_range_rsq, out float rsqr )
{
  	float3 N		= normal;							// normal
  	float3 L2P 	= pnt-light_position;                         		// light2point
  	float3 V 	= -normalize	(pnt);					// vector2eye
  	float3 L 	= -normalize	((float3)L2P);					// vector2light
  	float3 H		= normalize	(L+V);						// float-angle-vector
		rsqr	= dot		(L2P,L2P);					// distance 2 light (squared)
  	float  att 	= saturate	(1 - rsqr*light_range_rsq*L_RANGE)*L_BRIGHT;			// q-linear attenuate
	float4 light	= s_material.Sample( smp_material, float3( dot(L,N), dot(H,N), da_lut_z(m) ) ).xxxy;		// sample material
	[branch] if (da_is_snowy(m))
		light = da_snow_light(light, N, V, L, da_is_glint(m));
	if (da_is_foliage(m))
		light.w *= da_foliage_fresnel(V, H);
  	float4 ret = (att*light*LIGHT_MUL1+att*float4(light.www*(Ldynamic_color.xyz*2),light.w)*Ldynamic_color.w/2*LIGHT_MUL2);

	ret.rgb = Vibrance(ret.rgb,2);

	return ret;
}

//	TODO: DX10: Remove path without blending
float4 blendp( float4 value, float4 tcp)
{
	return 	value;
}

float4 blend( float4 value, float2 tc)
{
	return 	value;
}

#endif
