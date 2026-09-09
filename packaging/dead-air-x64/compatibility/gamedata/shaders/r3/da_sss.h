#ifndef da_sss_h_included
#define da_sss_h_included

// Screen-space contact shadows (ported from the sibling engine, d13e266).
//
// The sun shadow map covers tens of metres at one resolution, so anything thinner than a
// texel - a grass blade at its root - never reaches it and appears to float above the
// ground. This ray marches through the depth buffer toward the sun and catches exactly that
// scale. Near sun pass only: in the far cascade at 160 m the whole ray is shorter than one
// pixel.
//
// Known limits of the technique, not bugs:
//   - only VISIBLE geometry casts; an object behind the camera gives no shadow;
//   - at the screen edge the ray leaves the frame and the shadow cuts off, hence the
//     short ray length;
//   - the depth buffer is flat and knows no object thickness. da_sss.z is that assumed
//     thickness: a depth gap larger than it counts as "the ray passed BEHIND the object".

// x = strength (0 = off), y = ray length in metres, z = thickness, w = step count.
uniform float4 da_sss;

// Screen point from a view-space position, using the same pair of numbers
// gbuffer_load_data uses to reconstruct the position. No projection matrix needed:
//   P.xy = P.z * ( uv * dp.zw - dp.xy )   =>   uv = ( P.xy / P.z + dp.xy ) / dp.zw
float2 da_sss_project( float3 pv )
{
	return ( pv.xy / max( pv.z, 0.001f ) + pos_decompression_params.xy ) / pos_decompression_params.zw;
}

// Per-pixel jitter of the ray start. Without it every pixel steps in lockstep and the
// shadow shows the steps as bands. Interleaved gradient noise (Jimenez 2014): no visible
// period, the same value for a pixel on every frame. The previous
// frac( dot( floor( pos ), ( 1/16, 1/4 ) ) * 4 ) had an integer y term, so frac() reduced
// it to frac( x / 4 ) - four vertical stripes sliding over the geometry with every camera move.
float da_sss_dither( float2 pos2d )
{
	return frac( 52.9829189f * frac( dot( floor( pos2d ), float2( 0.06711056f, 0.00583715f ) ) ) );
}

// Returns a lighting factor: 1 = lit, 0 = fully shadowed.
float da_screen_space_shadow( float3 pv, float3 light_dir_view, float2 pos2d )
{
	[branch] if ( da_sss.x <= 0.001f )
		return 1.0f;

	const int   steps = (int)max( da_sss.w, 2.0f );
	const float len   = da_sss.y;
	const float thick = da_sss.z;
	const float dstep = len / steps;

	// The ray walks TOWARD the sun, so negate: Ldynamic_dir points AT the surface.
	const float3 dir = -normalize( light_dir_view );

	// Start offset in [0.5, 1) step: the first test lands 1.5..2 steps out, clear of the
	// virtual-offset region where the ray still sits inside its own surface's bias.
	const float start = 0.5f + 0.5f * da_sss_dither( pos2d );
	float3 rp = pv + dir * dstep * start;

	float occ = 0.0f;

	[loop]
	for ( int i = 0; i < steps; i++ )
	{
		rp += dir * dstep;

		const float2 uv = da_sss_project( rp );
		[branch] if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f )
			break;

		// Scene depth at that screen point - the raw value the position buffer carries in .z.
		// With MSAA the position target is a Texture2DMS, which has no SampleLevel at all: the
		// march has to Load sample zero instead. Without this the whole sun accumulator fails to
		// COMPILE the moment a player turns MSAA on with contact shadows enabled, which lands as
		// a hard fault in the middle of a level load.
#ifdef USE_MSAA
		const float scene_z = s_position.Load( int3( uv * pos_decompression_params2.xy, 0 ), 0 ).z;
#else
		const float scene_z = s_position.SampleLevel( smp_nofilter, uv, 0 ).z;
#endif

		// The sky casts no shadow.
		[branch] if ( scene_z < 0.05f )
			continue;

		const float diff = rp.z - scene_z;

		// Hit: the ray ended up behind a surface, but not deeper than its assumed thickness.
		// The upper bound is mandatory - without it any object would be infinitely deep and
		// drag its shadow across the whole scene.
		// Two ramps instead of a step: behind by a couple of centimetres starts to count,
		// one step deeper counts fully, and the thickness bound fades over its upper half.
		// Neighbouring pixels start at different offsets (the dither), so a step function
		// turns every sample near the boundary into full-contrast noise; the ramps turn the
		// same ambiguity into a gradient.
		const float hit = saturate( ( diff - 0.02f ) / dstep ) * saturate( ( thick - diff ) / ( 0.5f * thick ) );

		// Fade toward the ray end, otherwise the length limit shows as a hard step. Measured
		// along the ray (start offset included) rather than by step index, so the fade does
		// not quantise into the bands the dither exists to hide.
		const float fade = saturate( 1.0f - ( start + (float)i ) / (float)steps );
		occ = max( occ, hit * fade );

		[branch] if ( hit >= 1.0f )
			break;
	}

	return saturate( 1.0f - occ * da_sss.x );
}

#endif
