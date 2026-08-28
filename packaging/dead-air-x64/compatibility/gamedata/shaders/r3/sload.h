#ifndef SLOAD_H
#define SLOAD_H

#include "common.h"
#include "da_hextile.h"

#define GLOSS_MUL 2//.1f

#ifdef	MSAA_ALPHATEST_DX10_1
#if MSAA_SAMPLES == 2
static const float2 MSAAOffsets[2] = { float2(4,4), float2(-4,-4) };
#endif
#if MSAA_SAMPLES == 4
static const float2 MSAAOffsets[4] = { float2(-2,-6), float2(6,-2), float2(-6,2), float2(2,6) };
#endif
#if MSAA_SAMPLES == 8
static const float2 MSAAOffsets[8] = { float2(1,-3), float2(-1,3), float2(5,1), float2(-3,-5),
								               float2(-5,5), float2(-7,-1), float2(3,7), float2(7,-7) };
#endif
#endif	//	MSAA_ALPHATEST_DX10_1

//////////////////////////////////////////////////////////////////////////////////////////
// Bumped surface loader                //
//////////////////////////////////////////////////////////////////////////////////////////
struct	surface_bumped
{
	float4	base;
	float3	normal;
	float	gloss;
	float	height;

};

// Specular antialiasing by normal variance (Toksvig 2005; the measure is Filament's: the
// variance of the FINAL normal across the screen, so it sees base relief and detail alike).
// x = strength (0 = off, multiplier is one), y = ceiling for the added variance (without it
// far small geometry drives the factor to zero and metal goes matte; 0.15 is Filament's
// default), z = power, w = debug (1 = show the factor, 2 = inverted).
// The material table s_material is non-monotonic, so the lobe cannot be WIDENED per pixel -
// what remains is Toksvig's original form: if the peak cannot widen, damp it by as much.
uniform float4 da_spec_aa;

float da_spec_aa_factor( float3 N )
{
	float3 du	= ddx( N );
	float3 dv	= ddy( N );
	float  var	= da_spec_aa.x * ( dot(du,du) + dot(dv,dv) );
	float  kern	= min( 2.0h * var, da_spec_aa.y );
	return 1.0h / ( 1.0h + da_spec_aa.z * kern );
}

// Detail mip bias (r__detail_mipbias, .x) for the wall/prop detail samples; zero = stock.
uniform float4 da_detail_bias;

// Steep parallax knobs (r__parallax_*): x = full-strength distance (m), y = end distance,
// z = depth, w = self-shadow strength.
uniform float4 da_parallax;
// x = max search steps, y = min steps, z = sun-ray steps, w = debug mode.
uniform float4 da_parallax2;

// Relief self-shadow. ONE means "nothing shaded" - exactly what shaders compiled WITHOUT
// steep parallax see, so multiplying by it is unconditionally safe.
static float da_parallax_shadow = 1.0h;
// Height at the found point and a "the parallax branch ran here" flag - debug only. The
// flag exists because "the correction is small" and "this code never runs here" look the
// same on screen without it.
static float da_parallax_height = 0.0h;
static float da_parallax_hit = 0.0h;

float4 tbase( float2 tc )
{
	// Repeat-breaking for FLAT surfaces too - two thirds of the world has no bump map and
	// never enters sload_i. Checked BEFORE the grid math: there are ddx/ddy inside, and
	// paying for them under a disabled knob would be waste. The branch is uniform (the value
	// comes from a constant buffer, one per draw), so derivatives inside it are legal.
#ifdef DA_HEX_ALLOW
	if ( da_hex_enabled() )
	{
		da_hex_setup H = da_hex_prepare( tc );
		float4 c; float3 W;
		da_hex_sample_base( c, W, s_base, smp_base, H );
		return c;
	}
#endif
	return	s_base.Sample( smp_base, tc);
}

#if defined(ALLOW_STEEPPARALLAX) && defined(USE_STEEPPARALLAX)

// Relief self-shadow: a ray from the found point towards the sun over the same height map.
// Without it parallax reads as "the texture swims" - depth with no shade in the groove is
// scored by the eye as a defect. The largest single visual gain available in this shader,
// for one loop of samples. L_sun_dir_e comes from static_globals (shared/common.h), present
// in every shader including the G-buffer pass; it points ALONG the rays, hence the minus.
// The result goes into the ALBEDO - there is no free G-buffer channel (the packed fourth
// component of rt_Position holds hemi+mtl) - see deffer_base_bump.ps.
float da_parallax_selfshadow( float2 tc, float h0, float3 Lts, float scale, float2 dTcDx, float2 dTcDy )
{
	// Sun below the tangent plane: the surface is not turned to it anyway, lighting gives
	// zero on its own. Return ONE, not zero - zero would also cut the ambient and every
	// sun-averted wall would darken at once for no visible reason.
	if ( Lts.z <= 0.001h )
		return 1.0h;

	int    steps = (int)da_parallax2.z;
	float  inv   = 1.0h / steps;
	// Climb per step and the matching lateral shift. Dividing by Lts.z, as textbooks write,
	// is WRONG here: at a low sun the ray length goes to infinity and the shadow stretches
	// across the whole texture.
	float  dh   = ( 1.0h - h0 ) * inv;
	float2 dtc  = Lts.xy * scale * dh;

	float occ = 0.0h;
	[loop]
	for ( int i = 1; i <= steps; ++i )
	{
		float hr = h0 + dh * i;                                        // ray height
		float hs = s_bumpX.SampleGrad( smp_base, tc + dtc * i, dTcDx, dTcDy ).a; // surface height
		// Weighted by distance: the near wall of the groove shades harder than the far one -
		// dense shadow at the obstacle, soft at the edge, no stepping from the step count.
		occ = max( occ, ( hs - hr ) * ( 1.0h - i * inv ) );
	}

	// The knob IS the density: occlusion lives in height fractions and rarely passes 0.15,
	// so meaningful r__parallax_shadow values are units, not fractions.
	return saturate( 1.0h - occ * da_parallax.w );
}

void UpdateTC( inout p_bumped I)
{
	if (I.position.z < da_parallax.y)
	{
		float maxSamples = da_parallax2.x;
		float minSamples = da_parallax2.y;
		float fParallaxOffset = -da_parallax.z;

		float3	 eye = mul (float3x3(I.M1.x, I.M2.x, I.M3.x,
									 I.M1.y, I.M2.y, I.M3.y,
									 I.M1.z, I.M2.z, I.M3.z), -I.position.xyz);

		eye = normalize(eye);

		// Fade computed HERE, not at the end: needed twice - first to cut the step count by
		// distance, then to damp the offset itself.
		float	fParallaxFade 	= smoothstep(da_parallax.y, da_parallax.x, I.position.z);

		//	Calculate number of steps
		// Fewer steps not only at grazing angles but BY DISTANCE: a far wall covers a few
		// pixels, detail is invisible there, the minimum suffices. Zero saving at the
		// camera, the whole gain on the far end.
		float nNumSteps = lerp( minSamples, lerp( maxSamples, minSamples, eye.z ), fParallaxFade );

		float	fStepSize			= 1.0 / nNumSteps;
		float2	vDelta				= eye.xy * fParallaxOffset*1.2;
		float2	vTexOffsetPerStep	= fStepSize * vDelta;

		// Derivatives taken HERE, while control flow is uniform across the quad. Inside the
		// loop neighbouring pixels take different step counts, and any Sample without an
		// explicit mip gets derivatives from divergent flow - undefined by HLSL rules, and
		// in practice random per-pixel mip levels (the mush and ray smears on brickwork).
		// Mip 0 instead is wrong differently: a far slanted wall starts to shimmer.
		float2 dTcDx = ddx( I.tcdh.xy );
		float2 dTcDy = ddy( I.tcdh.xy );

		//	Prepare start data for cycle
		float2	vTexCurrentOffset	= I.tcdh;
		float	fCurrHeight			= 0.0;
		float	fCurrentBound		= 1.0;

		// [loop] is mandatory: the bound now comes from a constant buffer and cannot be
		// unrolled - without the attribute fxc either refuses or crashes without naming a
		// file or line (the SSR loop in soft water behaved exactly like that).
		[loop]
		for( int i=0; i<nNumSteps; ++i )
		{
			if (fCurrHeight < fCurrentBound)
			{
				vTexCurrentOffset += vTexOffsetPerStep;
				fCurrHeight = s_bumpX.SampleGrad( smp_base, vTexCurrentOffset.xy, dTcDx, dTcDy ).a;
				fCurrentBound -= fStepSize;
			}
		}

		// Binary refinement of the intersection instead of one linear estimate - the cure
		// for the "stairs" on brick edges. The linear search only brackets the hit; the old
		// code drew a line through the bracket, a first-order guess that works only when
		// height changes linearly between steps. Brickwork changes by a CLIFF, the guess
		// misses, and the miss is quantised by step length - stairs no step count fixes
		// (Tatarchuk, ATI 2006). Five halvings shrink the bracket 32x - the edge lands
		// inside a pixel - for five samples against the 8..32 of the linear search.
		float2	tcAbove = vTexCurrentOffset - vTexOffsetPerStep; // still ABOVE the surface
		float2	tcBelow = vTexCurrentOffset;                     // already BELOW it
		float	bAbove = fCurrentBound + fStepSize;
		float	bBelow = fCurrentBound;
		[unroll]
		for ( int b = 0; b < 5; ++b )
		{
			float2 tcMid = ( tcAbove + tcBelow ) * 0.5h;
			float  bMid  = ( bAbove  + bBelow  ) * 0.5h;
			float  hMid  = s_bumpX.SampleGrad( smp_base, tcMid, dTcDx, dTcDy ).a;
			// Ray above the surface - move the near bound, otherwise the far one.
			if ( hMid < bMid ) { tcAbove = tcMid; bAbove = bMid; }
			else               { tcBelow = tcMid; bBelow = bMid; }
		}
		float2	vTexCoord = ( tcAbove + tcBelow ) * 0.5h;
		// Distance fade, same meaning as before: the OFFSET itself is damped to zero.
		vTexCoord = lerp( I.tcdh.xy, vTexCoord, fParallaxFade );

		//	Output the result
		I.tcdh = vTexCoord;

		// Self-shadow - at the same coordinates as the colour and with the SAME fade: at the
		// far border the relief is already flat and the shadow must vanish with it, or a
		// dark band hangs at the seam with no visible cause.
		if ( da_parallax.w > 0.001h )
		{
			float3 Lts = normalize( mul( float3x3(I.M1.x, I.M2.x, I.M3.x,
												  I.M1.y, I.M2.y, I.M3.y,
												  I.M1.z, I.M2.z, I.M3.z), -L_sun_dir_e ) );
			float  h0  = s_bumpX.SampleGrad( smp_base, vTexCoord, dTcDx, dTcDy ).a;
			float  sh  = da_parallax_selfshadow( vTexCoord, h0, Lts, da_parallax.z * 1.2, dTcDx, dTcDy );
			da_parallax_shadow = lerp( 1.0h, sh, fParallaxFade );
			da_parallax_height = h0;
		}
		da_parallax_hit = 1.0h;

#if defined(USE_TDETAIL) && defined(USE_STEEPPARALLAX)
		I.tcdbump = vTexCoord * dt_params;
#endif
	}

}

#elif	defined(USE_PARALLAX) || defined(USE_STEEPPARALLAX)

void UpdateTC( inout p_bumped I)
{
	float3	 eye = mul (float3x3(I.M1.x, I.M2.x, I.M3.x,
								 I.M1.y, I.M2.y, I.M3.y,
								 I.M1.z, I.M2.z, I.M3.z), -I.position.xyz);

	float	height	= s_bumpX.Sample( smp_base, I.tcdh).w;	//
			//height  /= 2;
			//height  *= 0.8;
			height	= height*(parallax.x) + (parallax.y);	//
	float2	new_tc  = I.tcdh + height * normalize(eye);	//

	//	Output the result
	I.tcdh	= new_tc;
}

#else	//	USE_PARALLAX

void UpdateTC( inout p_bumped I)
{
	;
}

#endif	//	USE_PARALLAX

surface_bumped sload_i( p_bumped I)
{
	surface_bumped	S;

	// Repeat-breaking by hex grid - see da_hextile.h. The grid is computed ONLY with the
	// knob on: there are ddx/ddy inside. The branch is uniform (constant-buffer value, one
	// per draw call), so the derivatives are legal.
	da_hex_setup	H		= (da_hex_setup)0;
#ifdef DA_HEX_ALLOW
	bool			hexOn	= da_hex_enabled();
#else
	bool			hexOn	= false;
#endif

	// Parallax must walk the DOMINANT tile - the height map that will actually be shown.
	// Otherwise the offset is computed on one brick and another gets drawn, and the depth
	// stops matching the picture.
	if ( hexOn )
	{
		H = da_hex_prepare( I.tcdh.xy );
		I.tcdh.xy = H.stDom;
	}

	UpdateTC(I);	//	All kinds of parallax are applied here.

	float4	Nu, NuE;
	if ( hexOn )
	{
		// The correction found by parallax is spread over all three samples.
		da_hex_shift( H, I.tcdh.xy - H.stDom );

		// Weights come FROM THE COLOUR and are reused by relief and height - otherwise those
		// would drift to different tiles and the normal at a seam would answer for a brick
		// that is not the one drawn.
		float3 W;
		da_hex_sample_base( S.base, W, s_base, smp_base, H );
		Nu  = da_hex_sample_w( s_bump,  smp_base, H, W );
		NuE = da_hex_sample_w( s_bumpX, smp_base, H, W );
	}
	else
	{
	Nu	= s_bump.Sample( smp_base, I.tcdh );		// IN:	normal.gloss
	NuE	= s_bumpX.Sample( smp_base, I.tcdh);	// IN:	normal_error.height

	S.base		= tbase(I.tcdh);				//	IN:  rgb.a
	}
	S.normal	= Nu.wzy + (NuE.xyz - 1.0h);	//	(Nu.wzyx - .5h) + (E-.5)
	S.gloss		= Nu.x*Nu.x;					//	S.gloss = Nu.x*Nu.x;
	S.height	= NuE.z;
	//S.height	= 0;

#ifdef        USE_TDETAIL
#ifdef        USE_TDETAIL_BUMP
	float4 NDetail		= s_detailBump.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	float4 NDetailX		= s_detailBumpX.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	S.gloss				= S.gloss * NDetail.x * GLOSS_MUL;
	//S.normal			+= NDetail.wzy-.5;
	S.normal			+= NDetail.wzy + NDetailX.xyz - 1.0h; //	(Nu.wzyx - .5h) + (E-.5)

	float4 detail		= s_detail.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	S.base.rgb			= S.base.rgb * detail.rgb * 2;

//	S.base.rgb			= float3(1,0,0);
#else        //	USE_TDETAIL_BUMP
	float4 detail		= s_detail.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	S.base.rgb			= S.base.rgb * detail.rgb * 2;
	S.gloss				= S.gloss * detail.w * GLOSS_MUL;
#endif        //	USE_TDETAIL_BUMP
#endif

	return S;
}

surface_bumped sload_i( p_bumped I, float2 pixeloffset )
{
	surface_bumped	S;

   // apply offset
#ifdef	MSAA_ALPHATEST_DX10_1
   I.tcdh.xy += pixeloffset.x * ddx(I.tcdh.xy) + pixeloffset.y * ddy(I.tcdh.xy);
#endif

	UpdateTC(I);	//	All kinds of parallax are applied here.

	float4 	Nu	= s_bump.Sample( smp_base, I.tcdh );		// IN:	normal.gloss
	float4 	NuE	= s_bumpX.Sample( smp_base, I.tcdh);	// IN:	normal_error.height

	S.base		= tbase(I.tcdh);				//	IN:  rgb.a
	S.normal	= Nu.wzyx + (NuE.xyz - 1.0h);	//	(Nu.wzyx - .5h) + (E-.5)
	S.gloss		= Nu.x*Nu.x;					//	S.gloss = Nu.x*Nu.x;
	S.height	= NuE.z;
	//S.height	= 0;

#ifdef        USE_TDETAIL
#ifdef        USE_TDETAIL_BUMP
#ifdef MSAA_ALPHATEST_DX10_1
#if ( (!defined(ALLOW_STEEPPARALLAX) ) && defined(USE_STEEPPARALLAX) )
   I.tcdbump.xy += pixeloffset.x * ddx(I.tcdbump.xy) + pixeloffset.y * ddy(I.tcdbump.xy);
#endif
#endif

	float4 NDetail		= s_detailBump.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	float4 NDetailX		= s_detailBumpX.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	S.gloss				= S.gloss * NDetail.x * GLOSS_MUL;
	//S.normal			+= NDetail.wzy-.5;
	S.normal			+= NDetail.wzy + NDetailX.xyz - 1.0h; //	(Nu.wzyx - .5h) + (E-.5)

	float4 detail		= s_detail.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	S.base.rgb			= S.base.rgb * detail.rgb * 2;

//	S.base.rgb			= float3(1,0,0);
#else        //	USE_TDETAIL_BUMP
#ifdef MSAA_ALPHATEST_DX10_1
   I.tcdbump.xy += pixeloffset.x * ddx(I.tcdbump.xy) + pixeloffset.y * ddy(I.tcdbump.xy);
#endif
	float4 detail		= s_detail.SampleBias( smp_base, I.tcdbump, da_detail_bias.x);
	S.base.rgb			= S.base.rgb * detail.rgb * 2;
	S.gloss				= S.gloss * detail.w * GLOSS_MUL;
#endif        //	USE_TDETAIL_BUMP
#endif

	return S;
}

surface_bumped sload ( p_bumped I)
{
      surface_bumped      S   = sload_i	(I);
		S.normal.z			*=	0.3;		//. make bump twice as contrast (fake, remove me if possible)

#ifdef	GBUFFER_OPTIMIZATION
	   S.height = 0;
#endif	//	GBUFFER_OPTIMIZATION
      return              S;
}

surface_bumped sload ( p_bumped I, float2 pixeloffset )
{
      surface_bumped      S   = sload_i	(I, pixeloffset );
		S.normal.z			*=	0.3;		//. make bump twice as contrast (fake, remove me if possible)
#ifdef	GBUFFER_OPTIMIZATION
	   S.height = 0;
#endif	//	GBUFFER_OPTIMIZATION
      return              S;
}

#endif
