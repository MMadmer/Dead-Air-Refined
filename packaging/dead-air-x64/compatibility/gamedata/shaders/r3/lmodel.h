#ifndef	LMODEL_H
#define LMODEL_H

#include "common.h"
#include "da_material_codes.h"
#include "da_snow_light.h"
#include "da_light_falloff.h"

// The light model of the base tree (configs\shaders\r3\lmodel.h) with the materials above the stock range
// (docs/dead-air/GRASS.md, SNOW.md): codes 27-30 are two-sided foliage, which samples the Oren-Nayar slice
// of s_material and adds the transmission of sunlight through the blade (UE TwoSidedFoliage: a wrapped
// back term with a fixed GGX lobe around the light direction); code 31 is snow, whose term
// da_snow_light.h owns. Every stock material keeps its own slice and its stock result.

#define LIGHT_MUL1 0.7//.7f
#define LIGHT_MUL2 .0//.5f

// The specular of every stock material after DOOM Eternal (specBRDF): GGX without the 1/pi the diffuse lacks too,
// Schlick-Smith visibility with k = alpha^2, Schlick Fresnel with f90 from f0 (an f0 under 2 % fades the grazing
// sheen out). The width is DOOM's alpha^2 = (1 - 0.8 s)^4 from the smoothness the sky's term (hmodel.h) takes too: a
// dull surface is a rough one. Held at the stock slice's narrower lobe, dry ground lit from behind at a grazing view
// mirrored the sun at 15 times its diffuse light - a distant hill white as snow. f0 follows the gloss, which the content
// authored as the strength of its specular (the stock lobe's energy at a gloss of 0.05 is the 4 % of a dielectric). No
// floor: a dry road's gloss of 0.005 is the authors' "no specular", and the physical 2 % gave it five times the
// stock's sheen.
// da_spec (cl_da_spec): x 1 = this model (0 = the stock lobe), y f0 per unit of gloss, z its ceiling, w smoothness per
// unit of gloss.
uniform float4 da_spec;

bool da_spec_ggx_on(float m) { return da_spec.x > 0.5f && !da_is_foliage(m) && !da_is_snowy(m); }

// Perceptual smoothness 0..1 from the G-buffer gloss.
float da_spec_smoothness(float gloss) { return saturate(gloss * da_spec.w); }

float da_spec_f0(float gloss) { return min(gloss * da_spec.y, da_spec.z); }

// Geometric specular antialiasing (Kaplanyan & Tokuyoshi 2019): what the normal turns across the pixel's quad - the
// G-buffer's, a normal map's ripple at a distance, an edge - widens the lobe by its variance, so a highlight thinner
// than a pixel stops flickering. Taken once per pixel by the light passes, in uniform control flow (da_spec_aa).
static float da_spec_aa_a2 = 0.0f;
void da_spec_aa(float3 N)
{
	const float3 du = ddx(N), dv = ddy(N);
	// 2 sigma^2 of the filter with sigma^2 = 0.25 (|dN/dx|^2 + |dN/dy|^2), at most 0.18.
	da_spec_aa_a2 = min(0.5f * (dot(du, du) + dot(dv, dv)), 0.18f);
}

float da_spec_ggx(float gloss, float3 N, float3 V, float3 L)
{
	const float r = 1.0f - 0.8f * da_spec_smoothness(gloss);
	// DOOM's alpha^2 = (1 - 0.8 s)^4, then the pixel's own spread on the smooth surfaces whose highlight is thin enough
	// to flicker (a rough one only gains a grazing sheen from it).
	const float a2 = saturate(max(r * r * r * r, 0.002f) + da_spec_aa_a2 * saturate(2.0f * da_spec_smoothness(gloss)));
	const float3 H = normalize(L + V);
	const float nh = saturate(dot(N, H));
	const float nv = saturate(dot(N, V)) + 1e-4f;
	const float nl = saturate(dot(N, L));
	const float lh = saturate(dot(L, H));
	const float d = nh * nh * (a2 - 1.0f) + 1.0f;
	const float D = a2 / (d * d);
	const float Vis = 1.0f / (4.0f * (nv * (1.0f - a2) + a2) * (nl * (1.0f - a2) + a2));
	const float f0 = da_spec_f0(gloss);
	const float F = f0 + (saturate(50.0f * f0) - f0) * pow(1.0f - lh, 5.0f);
	return D * Vis * F * nl;
}

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

// The same with the surface's gloss: the stock materials take the GGX specular above.
float4 plight_infinity( float m, float3 pnt, float3 normal, float3 light_direction, float gloss )
{
	float4 ret = plight_infinity( m, pnt, normal, light_direction );
	[branch] if ( da_spec_ggx_on( m ) )
		ret.w = LIGHT_MUL1 * da_spec_ggx( gloss, normal, -normalize( pnt ), -light_direction );
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
  	float  att 	= da_light_att	(rsqr*light_range_rsq*L_RANGE)*L_BRIGHT;			// the falloff (da_light_falloff.h)
	// The LUT has one mip: read without gradients, the light volumes call this in flow control.
	float4 light	= s_material.SampleLevel( smp_material, float3( dot(L,N), dot(H,N), da_lut_z(m) ), 0 ).xxxy;		// sample material
	[branch] if (da_is_snowy(m))
		light = da_snow_light(light, N, V, L, da_is_glint(m));
	if (da_is_foliage(m))
		light.w *= da_foliage_fresnel(V, H);
  	float4 ret = (att*light*LIGHT_MUL1+att*float4(light.www*(Ldynamic_color.xyz*2),light.w)*Ldynamic_color.w/2*LIGHT_MUL2);

	ret.rgb = Vibrance(ret.rgb,2);

	return ret;
}

float4 plight_local( float m, float3 pnt, float3 normal, float3 light_position, float light_range_rsq, out float rsqr,
	float gloss )
{
	float4 ret = plight_local( m, pnt, normal, light_position, light_range_rsq, rsqr );
	[branch] if ( da_spec_ggx_on( m ) )
	{
		const float att = da_light_att( rsqr * light_range_rsq * L_RANGE ) * L_BRIGHT;
		ret.w = att * LIGHT_MUL1 * da_spec_ggx( gloss, normal, -normalize( pnt ), normalize( light_position - pnt ) );
	}
	return ret;
}

// The specular light's colour: combine_1 has the diffuse sum, whose Vibrance(2) doubled the lights' saturation around
// their luminance; half of it back is the lights' own hue, at unit luminance.
float3 da_spec_hue( float3 diffuse )
{
	const float l = dot( diffuse, LUMINANCE_VECTOR );
	return lerp( l.xxx, diffuse, 0.5f ) / max( l, 1e-4f );
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
