// Copyright (c) 2026 Grizzly
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

// Rate of a cut. The stock loader wrapped the falloff of a motion with zero accrue to the top of
// the quantizer (1.5 * 65535 / 655.35): the cycles it replaced were gone in 6.7 ms and the new one
// was the only blend left, i.e. at full weight. Every first-person *_shoot is authored that way.
constexpr float CutRate = 150.f;

// The CAP slows a blend that is authored faster than min_time; a caller may turn it off (the
// player hud does). A zero authored rate is a cut where there is no cap: blending a 0.1 s shot
// in over 0.2 s hid the bolt, the hammer and the shell bones of the hud weapons.
IC float ApplyMinimumTime(const float authored_rate, const float minimum_time)
{
    if (authored_rate <= 0.f)
        return minimum_time > 0.f ? RateFromTime(minimum_time) : CutRate;

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
