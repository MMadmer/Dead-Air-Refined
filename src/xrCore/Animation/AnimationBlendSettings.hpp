// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

class CInifile;

namespace AnimationBlend
{
enum Curve : u32
{
    Linear = 0,
    Smooth = 1,
};

extern XRCORE_API float g_min_time;
extern XRCORE_API u32 g_curve;
extern XRCORE_API float g_fall_at_end_time;
extern XRCORE_API float g_default_motion_accrue_time;
extern XRCORE_API float g_default_motion_falloff_time;
extern XRCORE_API float g_movement_blend_fraction;

XRCORE_API void LoadSettings(const CInifile* settings);

IC float RateFromTime(const float time) { return 1.f / time; }

// Two different things, and they were one. The CAP slows a blend that is authored faster
// than min_time, and a caller may turn it off (the player hud does - a first-person cycle
// is meant to cut). The FALLBACK answers a rate of zero, which is not "blend instantly" but
// "the file has no value": a mix-in blend then grows by dt * 0 and never becomes visible at
// all, and the cycle it replaced has already been faded out. Every first-person *_shoot in
// Dead Air's own motion files is authored that way, which is why the bolt on some weapons
// never moved, the shot animation looked cut, and a bolt-action rifle appeared to work its
// own bolt with the hands standing still. The fallback is not optional; the cap is.
IC float ApplyMinimumTime(const float authored_rate, const float minimum_time)
{
    if (authored_rate <= 0.f)
    {
        // 0.2 s when nothing else says otherwise: an infinite rate would be an instant cut, and
        // dt * inf is a NaN on the frame dt is zero.
        const float fallback = minimum_time > 0.f ? minimum_time : (g_min_time > 0.f ? g_min_time : 0.2f);
        return RateFromTime(fallback);
    }

    if (minimum_time <= 0.f)
        return authored_rate;

    return _min(authored_rate, RateFromTime(minimum_time));
}

IC float ApplyMinimumTime(const float authored_rate) { return ApplyMinimumTime(authored_rate, g_min_time); }

IC float ApplyCurve(const float amount, const float power)
{
    if (g_curve == Linear || power <= 0.f)
        return amount;

    const float alpha = clampr(amount / power, 0.f, 1.f);
    return power * alpha * alpha * (3.f - 2.f * alpha);
}
}
