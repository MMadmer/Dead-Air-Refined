#ifndef DA_WIND_FIELD_H
#define DA_WIND_FIELD_H

// Travelling spatial gust field (the Ghost of Tsushima scheme: wind keeps one heading, its
// MAGNITUDE varies place to place with noise that rides downwind). Evaluated once per grass
// tuft / tree root in the vertex shader, so a gust front visibly rolls across a meadow while
// thirty metres away the air stands still - instead of the whole field swaying in lockstep.
//
// da_wind_field const (cl_da_wind_field): xy = wrapped world-space scroll offset of the field
// (accumulated downwind on the CPU, wrapped at 64 cells x 40 m), z = slow strength envelope,
// w = gustiness (drives the lean term).
uniform float4 da_wind_field;

// Periodic lattice hash: fmod over the same 64-cell period the CPU wraps the offset with -
// the field tiles seamlessly at 2560 m (bigger than any level, invisible), and neither side
// of the maths ever feeds large numbers into frac().
float da_wf_hash(float2 i)
{
    float2 p = fmod(i + 4096.0f, 64.0f);
    p = frac(p * float2(127.1f, 311.7f));
    p += dot(p, p + 34.23f);
    return frac(p.x * p.y);
}

float da_wf_noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = da_wf_hash(i);
    float b = da_wf_hash(i + float2(1.0f, 0.0f));
    float c = da_wf_hash(i + float2(0.0f, 1.0f));
    float d = da_wf_hash(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Returns: x = amplitude multiplier (~0.40 lull .. ~1.15 gust tongue),
//          y = lean 0..1 (how hard the local flow presses vegetation down-wind),
//          z = local heading deviation -1..1 (an INDEPENDENT, broader noise pattern riding
//              the same scroll - the consumer turns its wind direction by this, so different
//              parts of the map genuinely blow different ways and eddies read as eddies).
// Two octaves: broad tongues (~40 m) carry the front, a half-scale layer breaks its edge up.
// The smoothstep + square shaping makes wide calm areas with rare gust tongues whose edges
// are soft and NONLINEAR - the "thin seam of in-between speed" between two flows.
float3 da_wind_field_eval(float2 wp)
{
    const float2 q = (wp - da_wind_field.xy) * (1.0f / 40.0f);
    const float n = da_wf_noise(q) * 0.62f + da_wf_noise(q * 2.17f + 13.7f) * 0.38f;
    float g = smoothstep(0.35f, 0.85f, n);
    g *= g;
    // Lulls at ~60% of nominal, gust tongues up to ~125%: the field VARIES the motion, it must
    // not also throttle its average (that is what flattened storms on the first flight).
    const float amp = 0.60f + 0.65f * g;
    // Lean follows the gust tongue and the global gustiness together: only a real gust
    // passing through a real tongue presses the grass flat.
    const float lean = g * saturate(0.35f + 0.65f * da_wind_field.w);
    // Heading deviation: an even broader pattern (~75 m swirls) sampled off to the side so it
    // does not correlate with the amplitude tongues. NOTE: the amplitude half above has a CPU
    // twin (CEnvironment::SampleWindField) that must stay formula-identical; this channel is
    // shader-only and free to differ.
    const float nd = da_wf_noise(q * 0.53f + float2(91.7f, 33.1f));
    return float3(amp, lean, nd * 2.0f - 1.0f);
}

// The local wind direction for a consumer rooted at this spot: the global heading dir2D
// turned by the field's deviation channel. Light air meanders up to ~28 degrees place to
// place, a storm stream straightens to ~11 - and the whole pattern rides downwind, so the
// swirls TRAVEL like real eddies instead of being painted on the ground.
float2 da_wind_local_dir(float2 dir, float dev)
{
    const float ang = dev * (0.50f - 0.30f * saturate(da_wind_field.z));
    float sa, ca;
    sincos(ang, sa, ca);
    return float2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
}

#endif // DA_WIND_FIELD_H
