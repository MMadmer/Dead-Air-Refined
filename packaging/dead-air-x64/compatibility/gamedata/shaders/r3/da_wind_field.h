#ifndef DA_WIND_FIELD_H
#define DA_WIND_FIELD_H

#include "da_wind_core.h"

// Shader-side view of the wind service. The maths lives in da_wind_core.h, which the engine
// compiles too; this file only binds it to the constants and gives the vertex shaders their
// vector-typed entry points.
//
// da_wind_field (cl_da_wind_field): xy = wrapped world-space scroll offset of the gust field
// (accumulated downwind on the CPU), z = strength envelope 0..1.25, w = gustiness 0..1.
uniform float4 da_wind_field;
// da_wind_state (cl_da_wind_state): xy = unit heading (world XZ), z = speed in m/s at 10 m,
// w = the wind-tick time in seconds (for consumers that animate on the service clock).
uniform float4 da_wind_state;

// Vector-typed conveniences over the scalar core for the shaders that hash positions.
float da_wf_hash(float2 i) { return da_wf_hash(i.x, i.y); }
float da_wf_noise(float2 p) { return da_wf_noise(p.x, p.y); }

// Returns: x = amplitude multiplier (0.60 lull .. 1.25 gust tongue),
//          y = lean 0..1 (how hard the local flow presses vegetation down-wind),
//          z = local heading deviation -1..1.
float3 da_wind_field_eval(float2 wp)
{
    const float g = da_wind_field_gust(wp.x, wp.y, da_wind_field.x, da_wind_field.y);
    const float dev = da_wind_field_dev(wp.x, wp.y, da_wind_field.x, da_wind_field.y);
    return float3(da_wind_field_amp(g), da_wind_field_lean(g, da_wind_field.w), dev);
}

// The local wind direction for a consumer rooted at this spot: the global heading turned by
// the field's deviation channel.
float2 da_wind_local_dir(float2 dir, float dev)
{
    const float ang = da_wind_dev_angle(dev, da_wind_field.z);
    float sa, ca;
    sincos(ang, sa, ca);
    return float2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
}

// Wind velocity (m/s, world XZ) at a world position, for consumers that want physics rather
// than a bend: particles, cloth, debris. Height applies the surface-layer profile.
float2 da_wind_velocity(float3 wp, float ground_y)
{
    const float g = da_wind_field_gust(wp.x, wp.z, da_wind_field.x, da_wind_field.y);
    const float dev = da_wind_field_dev(wp.x, wp.z, da_wind_field.x, da_wind_field.y);
    const float2 dir = da_wind_local_dir(da_wind_state.xy, dev);
    const float speed = da_wind_state.z * da_wind_field_amp(g) * da_wind_profile(wp.y - ground_y, 0.03f);
    return dir * speed;
}

#endif // DA_WIND_FIELD_H
