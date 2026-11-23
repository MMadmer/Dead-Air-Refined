#ifndef        HMODEL_H
#define HMODEL_H

#include "common.h"
#include "da_material_codes.h"
#include "da_fog_common.h"
#include "lmodel.h"

// The ambient (hemisphere) model of the base tree (configs\shaders\r3\hmodel.h) with the materials above
// the stock range (docs/dead-air/GRASS.md): foliage and snow sample the Oren-Nayar slice of s_material
// (their codes would otherwise land between slices), and foliage lets some of the sky through the
// blade (UE adds the opposite hemisphere's light times the subsurface colour).

TextureCube		env_s0;
TextureCube		env_s1;
TextureCube		sky_s0;
TextureCube		sky_s1;

uniform float4	env_color;        // color.w  = lerp factor
uniform float3x4	m_v2w;

// The specular term in its two factors: the sky's cube in the mirror direction (env_s) and its weight by the
// material and the gloss (hspecular = env_s * spec_w). combine_1 puts the traced or screen-space reflection in place of
// the cube, at spec_e: the same weight without the sky the point sees (h), which a reflection of what is really there
// needs no longer - under a roof the cube is dimmed by it, the ceiling a ray finds is not.
void hmodel_parts
(
	out float3 hdiffuse, out float3 env_s, out float spec_w, out float spec_e,
	float m, float h, float s, float3 Pnt, float3 normal
)
{
        // hscale - something like diffuse reflection
	float3	nw		= mul( m_v2w, normal );
	float	hscale	= h;	//. *        (.5h + .5h*nw.y);

#ifdef         USE_GAMMA_22
			hscale	= (hscale*hscale);        // make it more linear
#endif

	// reflection vector
	float3	v2PntL	= normalize( Pnt );
	float3	v2Pnt	= mul( m_v2w, v2PntL );
	float3	vreflect= reflect( v2Pnt, nw );
	float	hspec	= .5h + .5h * dot( vreflect, v2Pnt );

	// material	// sample material
	float4	light	= s_material.SampleLevel( smp_material, float3( hscale, hspec, da_lut_z(m) ), 0 ).xxxy;

	// diffuse color
	float3	e0d		= env_s0.SampleLevel( smp_rtlinear, nw, 0 );
	float3	e1d		= env_s1.SampleLevel( smp_rtlinear, nw, 0 );
	float3	env_d	= env_color.xyz * lerp( e0d, e1d, env_color.w );
			env_d	*=env_d;	// contrast
			hdiffuse= env_d * light.xyz + L_ambient.rgb;
	// Sky light through a translucent blade.
	[branch] if (da_is_foliage(m))
		hdiffuse *= 1.0f + 0.35f * da_foliage_tr(m);

	// specular color
	vreflect.y      = vreflect.y*2-1;	// fake remapping
	float3	e0s		= env_s0.SampleLevel( smp_rtlinear, vreflect, 0 );
	float3	e1s		= env_s1.SampleLevel( smp_rtlinear, vreflect, 0 );
			env_s	= env_color.xyz * lerp( e0s, e1s, env_color.w);
			env_s	*=env_s;	// contrast
			spec_w	= light.w*s;
			spec_e	= spec_w;

	// The stock materials under the GGX model (lmodel.h) reflect the sky itself, as the dome shows it (the fog's
	// tint, da_fog_common.h), sharp or blurred by the surface's roughness - DOOM picks the probe's mip by 6 (1 - s) -
	// in the true mirror direction, weighted by DOOM's analytic environment BRDF and the sky the point sees (h). The
	// stock term was the blurred irradiance cube at its sharpest mip for every surface, the gloss only its strength.
	[branch] if ( da_spec_ggx_on( m ) )
	{
		const float3 R = reflect( v2Pnt, nw );
		const float smooth = da_spec_smoothness( s );
		const float mip = 6.0f * ( 1.0f - smooth );
		const float3 k0 = sky_s0.SampleLevel( smp_rtlinear, R, mip );
		const float3 k1 = sky_s1.SampleLevel( smp_rtlinear, R, mip );
		env_s = da_sky_tint.rgb * lerp( k0, k1, env_color.w ) * 0.66f;
		const float nv = saturate( dot( -v2Pnt, nw ) );
		const float A = ( 0.095f + smooth * ( 0.6f + 4.19f * smooth ) ) * ( nv + 0.025f ) * exp2( 1.0f - 14.0f * nv );
		const float B = 0.4f + 0.6f * ( 1.0f - exp2( -9.5f * smooth * nv ) );
		const float f0 = da_spec_f0( s );
		spec_e = A * ( 1.0f - f0 ) + B * f0;
		spec_w = spec_e * saturate( hscale );
	}
}

void hmodel
(
	out float3 hdiffuse, out float3 hspecular,
	float m, float h, float s, float3 Pnt, float3 normal
)
{
	float3	env_s;
	float	spec_w, spec_e;
	hmodel_parts( hdiffuse, env_s, spec_w, spec_e, m, h, s, Pnt, normal );
		hspecular	= env_s*spec_w;                //*h*m*s        ;        //env_s        *light.w         * s;
}

#endif
