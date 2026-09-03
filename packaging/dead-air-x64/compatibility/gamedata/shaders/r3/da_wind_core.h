#ifndef DA_WIND_CORE_H
#define DA_WIND_CORE_H

// The wind field, in one place, for two compilers.
//
// This file is included by the vegetation vertex shaders AND by the engine
// (src/xrEngine/Environment.cpp includes it by relative path). It is written in the scalar
// subset that HLSL and C++ share, so the sound layer, the bullet drift, the grass, the trees
// and the clouds all evaluate the SAME function - the old CPU "twin" of the shader field
// drifted within an hour of being written and left the audio a third quieter than the eye.
//
// Rules for editing: scalar floats only (no float2/float3 - the C++ side has none), no
// intrinsics beyond the DA_* wrappers below, no statics. If it does not compile on both
// sides, it does not go in here.

// No #include here on purpose, not even for C++: the shader cache walks every #include line of
// a shader's dependency tree to hash it, cannot read <angle brackets>, and dies on the empty
// name. The C++ includer already has <cmath> through its precompiled header.
#ifdef __cplusplus
#define DA_FUNC inline
#define DA_FLOOR(x) std::floor(x)
#define DA_FRAC(x) ((x) - std::floor(x))
#define DA_SIN(x) std::sin(x)
#define DA_COS(x) std::cos(x)
#define DA_FMOD(x, y) std::fmod((x), (y))
#define DA_SAT(x) ((x) < 0.0f ? 0.0f : ((x) > 1.0f ? 1.0f : (x)))
#define DA_LOG(x) std::log(x)
#else
#define DA_FUNC
#define DA_FLOOR(x) floor(x)
#define DA_FRAC(x) frac(x)
#define DA_SIN(x) sin(x)
#define DA_COS(x) cos(x)
#define DA_FMOD(x, y) fmod((x), (y))
#define DA_SAT(x) saturate(x)
#define DA_LOG(x) log(x)
#endif

// ---- Constants shared by both sides -------------------------------------------------------
// The gust field repeats every 64 lattice cells of 40 m; the hash is periodic over the same
// 64 so the field tiles seamlessly at 2560 m and neither side ever feeds a large number to
// frac().
#define DA_WIND_CELL 40.0f
#define DA_WIND_PERIOD 64.0f
// Normalised strength 1.0 is this many metres per second at 10 m above open ground. Every
// physical consumer (bullets, bodies, particles, cloth) converts through this one number.
#define DA_WIND_MS_PER_NORM 11.0f

// ---- Lattice value noise ------------------------------------------------------------------
DA_FUNC float da_wf_hash(float ix, float iz)
{
    float px = DA_FMOD(ix + 4096.0f, DA_WIND_PERIOD);
    float pz = DA_FMOD(iz + 4096.0f, DA_WIND_PERIOD);
    px = DA_FRAC(px * 127.1f);
    pz = DA_FRAC(pz * 311.7f);
    const float d = px * (px + 34.23f) + pz * (pz + 34.23f);
    px += d;
    pz += d;
    return DA_FRAC(px * pz);
}

DA_FUNC float da_wf_noise(float px, float pz)
{
    const float ix = DA_FLOOR(px);
    const float iz = DA_FLOOR(pz);
    float fx = px - ix;
    float fz = pz - iz;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    const float a = da_wf_hash(ix, iz);
    const float b = da_wf_hash(ix + 1.0f, iz);
    const float c = da_wf_hash(ix, iz + 1.0f);
    const float d = da_wf_hash(ix + 1.0f, iz + 1.0f);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
}

// ---- The travelling gust field ------------------------------------------------------------
// Ghost of Tsushima's scheme: one heading, magnitude varied place to place by noise that rides
// downwind. (wx, wz) is the world position, (ox, oz) the scroll offset the CPU accumulates.
//
// Amplitude multiplier: lulls at 60 % of nominal, gust tongues to 125 %. The field varies the
// motion, it must not also throttle its average.
DA_FUNC float da_wind_field_gust(float wx, float wz, float ox, float oz)
{
    const float qx = (wx - ox) * (1.0f / DA_WIND_CELL);
    const float qz = (wz - oz) * (1.0f / DA_WIND_CELL);
    const float n = da_wf_noise(qx, qz) * 0.62f + da_wf_noise(qx * 2.17f + 13.7f, qz * 2.17f + 13.7f) * 0.38f;
    float g = DA_SAT((n - 0.35f) * 2.0f);
    g = g * g * (3.0f - 2.0f * g);
    return g * g;
}

DA_FUNC float da_wind_field_amp(float g) { return 0.60f + 0.65f * g; }

// Lean 0..1: only a real gust passing through a real tongue presses vegetation flat.
DA_FUNC float da_wind_field_lean(float g, float gustiness) { return g * DA_SAT(0.35f + 0.65f * gustiness); }

// Local heading deviation -1..1: a broader pattern (~75 m swirls) sampled off to the side so
// it does not correlate with the amplitude tongues. Rides the same scroll, so the swirls
// travel like eddies instead of being painted on the ground.
DA_FUNC float da_wind_field_dev(float wx, float wz, float ox, float oz)
{
    const float qx = (wx - ox) * (1.0f / DA_WIND_CELL);
    const float qz = (wz - oz) * (1.0f / DA_WIND_CELL);
    return da_wf_noise(qx * 0.53f + 91.7f, qz * 0.53f + 33.1f) * 2.0f - 1.0f;
}

// The angle (radians) a consumer turns the global heading by: light air meanders ~28 degrees
// place to place, a storm stream straightens to ~11.
DA_FUNC float da_wind_dev_angle(float dev, float strength) { return dev * (0.50f - 0.30f * DA_SAT(strength)); }

// ---- Vertical profile ---------------------------------------------------------------------
// Logarithmic wind profile of the surface layer: speed at height z relative to the 10 m
// reference, with z0 the roughness length (0.03 m open grass, 0.5 m forest/village). Below
// one metre the law is not meaningful and the ratio is held.
DA_FUNC float da_wind_profile(float z, float z0)
{
    const float zz = z < 1.0f ? 1.0f : z;
    return DA_LOG(zz / z0) / DA_LOG(10.0f / z0);
}

// ---- Waveforms ----------------------------------------------------------------------------
// da_sway: a static downwind lean with smooth harmonic oscillation around it - three
// incommensurable harmonics, C-infinity, ~[-1..1] with rare full peaks. The beat of the pair
// IS the "lean - hold - half spring-back - lean again" the eye expects.
DA_FUNC float da_sway(float ph)
{
    const float w = ph * 6.2831853f;
    return 0.62f * DA_SIN(w) + 0.28f * DA_SIN(w * 1.731f + 1.3f) + 0.10f * DA_SIN(w * 3.09f + 4.1f);
}

// Foliage flutter: zero-centred, smooth, non-repeating pair.
DA_FUNC float da_flutter(float ph)
{
    const float w = ph * 6.2831853f;
    return 0.60f * DA_SIN(w) + 0.40f * DA_SIN(w * 1.618f + 0.9f);
}

#endif // DA_WIND_CORE_H
