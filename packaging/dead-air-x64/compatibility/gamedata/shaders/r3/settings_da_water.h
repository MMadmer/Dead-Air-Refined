// Every water tuning knob in one place. Included by water.ps (and via water_green.ps with the
// green standing-water profile switched in). Compile-time on purpose: these are looks, not
// options - a shader-cache wipe applies changes without an engine rebuild.
// Ported from the sibling engine's settings_da_water.h; the duckweed/silt layers were left
// behind (they rebind the debris texture), everything else is intact.
#ifndef SETTINGS_DA_WATER_H
#define SETTINGS_DA_WATER_H

// ---- Depth colour -------------------------------------------------------------------------
// Real water is a coloured filter: red is absorbed within a metre, green survives several,
// blue furthest - the colour SHIFTS with depth instead of just darkening. Shallow is a
// boot-deep puddle over silt; deep is Zone standing water, green-black rather than blue.
#define WATER_TINT_SHALLOW	float3(0.10, 0.16, 0.15)
#define WATER_TINT_DEEP		float3(0.035, 0.085, 0.095)
#define WATER_TINT_FALLOFF	0.26	// larger = the deep colour takes over closer to the shore

// ---- Shoreline foam -----------------------------------------------------------------------
#define WATER_FOAM_WIDTH	0.25	// metres of depth over which the foam fades out
#define WATER_FOAM_STRENGTH	0.90
#define WATER_FOAM_COLOR	float3(0.62, 0.62, 0.58)

// ---- Wave calming with distance -----------------------------------------------------------
#define WATER_WAVE_AMP		0.55
#define WATER_FAR_START		25.0	// metres: up to here the water stays exactly as it was
#define WATER_FAR_END		120.0	// metres: where the damping reaches its limit
#define WATER_FAR_AMP		0.12	// residual wave share far away; 0 = mirror

// ---- Reflections --------------------------------------------------------------------------
#define WATER_REFL_CLAMP	1.25	// luminance ceiling: kills fireflies the march picks up
#define WATER_SSR_START		30.0	// metres: up to here reflections are fully ray-marched
#define WATER_SSR_END		45.0	// metres: past this only the cubemap, no ray at all

// ---- Floating debris (leaves layer) -------------------------------------------------------
#define WATER_LEAVES_NEAR	0.05	// metres of depth where debris starts
#define WATER_LEAVES_FAR	4.00	// and where it fades out
#define WATER_LEAVES_STRENGTH	1.0

// ---- Alpha discard ------------------------------------------------------------------------
#define WATER_ALPHA_CLIP	0.0	// zero discards nothing

// ---- Sun glint ----------------------------------------------------------------------------
#define WATER_SUN_POWER		280.0
#define WATER_SUN_STRENGTH	2.6

// ---- Green standing-water profile (DA_WATER_GREEN, set by water_green.ps) -----------------
#ifdef DA_WATER_GREEN
#undef  WATER_TINT_SHALLOW
#undef  WATER_TINT_DEEP
#undef  WATER_TINT_FALLOFF
#undef  WATER_FOAM_COLOR
#undef  WATER_FOAM_STRENGTH
#undef  WATER_SUN_STRENGTH
#define WATER_TINT_SHALLOW	float3(0.13, 0.20, 0.08)	// at the shore: sludge, green with yellow
#define WATER_TINT_DEEP		float3(0.040, 0.100, 0.030)	// deep: dark green, no blue at all
#define WATER_TINT_FALLOFF	0.55						// suspended silt: deep colour takes over fast
#define WATER_FOAM_COLOR	float3(0.50, 0.54, 0.40)	// the shore rim is greenish, not white
#define WATER_FOAM_STRENGTH	0.55
#define WATER_SUN_STRENGTH	1.6							// mirrors weaker than flowing water
#endif

// ---- Fresnel ------------------------------------------------------------------------------
// Schlick against water's real IOR 1.33: F0 = ((1-1.33)/(1+1.33))^2 = 0.02.
#define WATER_FRESNEL_F0       0.02
#define WATER_FRESNEL_POWER    5.0

// ---- Sky reflection fix -------------------------------------------------------------------
#define WATER_SKY_MAP_FIX      1	// 0 restores the stock vertical squash of the reflected sky

// ---- Rain ripples -------------------------------------------------------------------------
#define WATER_RAIN_RIPPLES     1
#define WATER_RIPPLE_SCALE     0.55   // map tiles per metre; larger = finer ripples
#define WATER_RIPPLE_AMP       0.55   // normal perturbation at full rain
#define WATER_RING_AMP         1.3    // impact ring normal perturbation: bullets, blasts, feet, bodies
#define WATER_RIPPLE_SPEED     0.09   // layer drift speed
#define WATER_RIPPLE_FADE      35.0   // metres, past this a ripple is subpixel

// Wind on open water: two more normal-map layers stretched along the wind and advected with
// it, and the base waves calm down when the air is still.
#define WATER_WIND_WAVES       1
#define WATER_WIND_AMP         0.22   // normal perturbation at a full gale (0.50 smeared the SSR mirror)
#define WATER_WIND_CALM        0.55   // share of the base waves left in still air
#define WATER_WIND_FADE        90.0   // metres, past this a wind wave is subpixel
// The two wind layers: map tiles per metre along / across the wind. Metre-scale cells with a
// mild stretch; the second layer sits WATER_WIND_ROT off the wind so the two lattices never
// line up. The first version tiled the map every 14 x 5 m along the wind and slid both
// layers the same way at ~3 m/s - a repeating texture racing over the whole marsh.
#define WATER_WIND_TILE_0      float2(0.45, 0.75)
#define WATER_WIND_TILE_1      float2(1.10, 1.60)
#define WATER_WIND_ROT         0.47   // radians (~27 deg) between the two layers
#define WATER_WIND_DRIFT       0.03   // tiles/s per m/s of wind: ~0.35 m/s crest speed in a 5 m/s breeze

// ---- Screen-space refraction --------------------------------------------------------------
#define WATER_REFRACT          1
#define WATER_REFRACT_STRENGTH 0.030  // screen share per unit of wave slope
#define WATER_REFRACT_WEIGHT   0.85
#define WATER_REFRACT_FADE     40.0   // metres

#endif	// SETTINGS_DA_WATER_H
