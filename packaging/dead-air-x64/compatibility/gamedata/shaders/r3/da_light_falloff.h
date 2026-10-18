#ifndef da_light_falloff_h_included
#define da_light_falloff_h_included

// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The falloff of the local lights. The stock one is a window, saturate(1 - d^2/R^2): no hot spot, a pool of light reads
// as a flat disc. This is DOOM Eternal's and Unreal's shape - the inverse square with a soft core, held to zero at the
// light's range by (1 - (d/R)^4)^2 - scaled so a surface at half the range is lit as before: brighter under the lamp,
// darker toward its edge, the authored lamps' balance kept where the eye mostly is.

// x: the share of the new shape (0 = stock), y: the core's hardness k in 1 / (1 + k d^2/R^2), z: its gain.
uniform float4 da_light_falloff;

// x2: d^2 / R^2; stock: the caller's own window at x2.
float da_light_att( float x2, float stock )
{
	[branch] if ( da_light_falloff.x <= 0.0f )
		return stock;
	const float w = saturate( 1.0f - x2 * x2 );
	const float shaped = da_light_falloff.z * w * w / ( 1.0f + da_light_falloff.y * x2 );
	return lerp( stock, shaped, da_light_falloff.x );
}

float da_light_att( float x2 )
{
	return da_light_att( x2, saturate( 1.0f - x2 ) );
}

#endif
