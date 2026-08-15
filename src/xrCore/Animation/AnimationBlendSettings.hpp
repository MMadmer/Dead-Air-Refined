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

IC float ApplyMinimumTime(const float authored_rate, const float minimum_time)
{
    if (minimum_time <= 0.f)
        return authored_rate;

    const float maximum_rate = RateFromTime(minimum_time);
    return authored_rate > 0.f ? _min(authored_rate, maximum_rate) : maximum_rate;
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
