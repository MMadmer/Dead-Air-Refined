// What is left of the water knobs after the rework: the things that are genuinely a LOOK and
// have no physical value to derive them from. Everything that does have one - wave height and
// length, surface roughness, absorption, foam width, Fresnel - now comes from the engine as a
// solved quantity (see da_water_common.h), so it is not here any more and cannot drift out of
// agreement with the weather.
//
// Compile-time on purpose: a shader-cache wipe applies a change without an engine rebuild.
#ifndef SETTINGS_DA_WATER_H
#define SETTINGS_DA_WATER_H

// ---- Sub-wave detail layer ----------------------------------------------------------------
// The animated normal map, sampled in world space: the band between the shortest wave the
// engine seeds and the capillary ripple that only exists as roughness.
//
// Its amplitude is not authored - it takes a SHARE of the same Cox-Munk slope budget the waves
// and the specular lobe divide up, so it grows and shrinks with the wind on its own and cannot
// quietly make the surface rougher than the wind says it is. Left as a free amplitude it was
// putting more slope variance into the normal at a gale than the whole surface has, which
// collapsed the glint and drew the sea as corduroy.
#define WATER_DETAIL_SHARE	0.30	// share of the slope budget this layer carries
// Slope-to-amplitude for this particular normal map: the sampled value is a biased tangent
// normal, so how much slope one unit of amplitude is worth depends on how aggressive the map
// is. Calibrated against water_normal; re-measure if that texture is replaced.
#define WATER_DETAIL_GAIN	2.80
#define WATER_DETAIL_FADE	55.0	// metres; past this one texel spans more than a ripple

// ---- Shallow-water business: rain, and the rings things leave ------------------------------
#define WATER_RIPPLE_SCALE	0.55	// rain-ripple map tiles per metre
#define WATER_RIPPLE_AMP	0.55	// slope perturbation at full rain
#define WATER_RIPPLE_SPEED	0.09	// rain-layer drift
#define WATER_RING_AMP		1.30	// impact rings: bullets, blasts, feet, bodies
#define WATER_RIPPLE_FADE	35.0	// metres; past this a ripple is subpixel

// The simulated ripple field. It hands back a world-XZ slope already, so the gain is a trim and
// not a conversion - leave it at one unless the sim's own amplitude is being re-scaled. The
// hand-over to the analytic rings at the window's edge is not a knob: it happens over the sim's
// own absorbing band, DA_WF_RIM_M in da_water_field.h.
#define WATER_FIELD_RIPPLE_AMP	1.00	// trim on the field's slope

// ---- Fetch shelter --------------------------------------------------------------------------
// How far open water has to run UPWIND before the wind sea is at full height. At this fetch and
// this wind the waves are decimetres long and build inside a few metres, so ten is the whole
// ramp: past it the pond is as rough as the wind allows, and hard against the upwind bank it is
// glass. This is also the distance the shader steps upwind to take its one field sample.
#define WATER_FETCH_LEN		10.0	// metres

// ---- Seen from below ------------------------------------------------------------------------
// Refraction is much stronger out of the dense side - the whole sky compresses into Snell's 97
// degree cone - so the screen offset per unit of surface slope is scaled up under water. It is
// a fixed amount rather than a depth ramp: from below there is no "how much water is left".
#define WATER_REFRACT_UNDER	3.0

// ---- Reflections ---------------------------------------------------------------------------
#define WATER_SSR_START		30.0	// metres: up to here the march is trusted outright
#define WATER_SSR_END		45.0	// metres: past this the sky cube alone, no ray at all

// ---- Sun and moon glint --------------------------------------------------------------------
// Strength only. The lobe's WIDTH is the Cox-Munk slope variance of the actual wind.
#define WATER_SUN_STRENGTH	2.6

// ---- Shoreline foam ------------------------------------------------------------------------
// Width is not here: the band is as wide as the waves run up the bank, ~2x the significant
// wave height, so it grows with the wind on its own.
#define WATER_FOAM_STRENGTH	0.90
#define WATER_FOAM_COLOR	float3(0.62, 0.62, 0.58)

// ---- Floating debris -----------------------------------------------------------------------
#define WATER_LEAVES_NEAR	0.05	// metres of depth where debris starts collecting
#define WATER_LEAVES_FAR	4.00	// and where it stops
#define WATER_LEAVES_STRENGTH	1.0

// ---- Screen-space refraction ---------------------------------------------------------------
#define WATER_REFRACT_STRENGTH	0.030	// screen share per unit of surface slope

#endif	// SETTINGS_DA_WATER_H
