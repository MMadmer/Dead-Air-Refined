#ifndef da_fade_h_included
#define da_fade_h_included

// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// A visual crossing its draw distance appears and vanishes as a dithered share of its pixels (r__dsgraph_build.cpp,
// r__fade_time) instead of a pop. DOOM Eternal's stochastic alpha test is the model: a noise threshold that turns
// every frame, which TAA resolves into a partial coverage.

// x: the share of the pixels drawn (1 = all), y: the frame's turn of the pattern (0..63), z: 1 the complementary pixels
// (a level of detail fading in over the one fading out keeps those the other leaves: da_autolod.h).
uniform float4 da_fade;

// hpos: the unjittered clip position the G-buffer shaders carry for the motion vectors.
void da_fade_clip( float4 hpos )
{
	[branch] if ( da_fade.x < 0.999f )
	{
		const float2 ndc = hpos.xy / hpos.w;
		const float2 px = floor( ( ndc * float2( 0.5f, -0.5f ) + 0.5f ) * pos_decompression_params2.xy );
		// Interleaved gradient noise (Jimenez 2014) with its temporal step.
		const float2 p = px + 5.588238f * da_fade.y;
		const float n = frac( 52.9829189f * frac( dot( p, float2( 0.06711056f, 0.00583715f ) ) ) );
		clip( da_fade.z > 0.5f ? n - ( 1.0f - da_fade.x ) : da_fade.x - n - 1e-4f );
	}
}

#endif
