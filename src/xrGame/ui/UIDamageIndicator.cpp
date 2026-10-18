// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIDamageIndicator.h"
#include "xrUICore/Static/UIShape.h"

int g_dar_damage_indicator = 1;

namespace
{
// interface units (768 to the screen's height): the ring's radius
constexpr float RingRadius = 168.f;
// hits whose sides are closer than this (radians) make one arc
constexpr float MergeAngle = 0.45f;
constexpr size_t MaxMarks = 8;

float saturate(float v) { return std::clamp(v, 0.f, 1.f); }

// a scratch on the health reads faint and quick, a quarter of it bright and lasting
float strength_of(float lost) { return saturate(0.4f + lost * 2.5f); }
} // namespace

void CDamageIndicator::Add(const Fvector& from, float lost)
{
    Fvector2 dir{ from.x, from.z };
    const float length = dir.magnitude();
    // straight from above or below: no side to show
    if (length < 0.05f || lost <= 0.f)
        return;
    dir.div(length);

    const float strength = strength_of(lost);
    const float life = 0.9f + 1.4f * strength;
    for (SMark& mark : m_marks)
    {
        if (std::acos(std::clamp(mark.from.dotproduct(dir), -1.f, 1.f)) > MergeAngle)
            continue;
        // the same side again: the arc lights up anew, as strong as the stronger hit, towards the newer side
        const float age = Device.fTimeGlobal - mark.start;
        const float left = saturate(1.f - age / mark.life);
        mark.from.add(dir).normalize_safe();
        mark.strength = std::max(mark.strength * left, strength);
        mark.life = std::max(mark.life, life);
        mark.start = Device.fTimeGlobal;
        return;
    }
    if (m_marks.size() >= MaxMarks)
        m_marks.erase(m_marks.begin());
    m_marks.push_back({ dir, strength, Device.fTimeGlobal, life });
}

void CDamageIndicator::Render()
{
    if (m_marks.empty())
        return;
    if (!g_dar_damage_indicator)
    {
        m_marks.clear();
        return;
    }

    // the camera's heading on the ground: forward is the top of the ring, right its right
    Fvector2 forward{ Device.vCameraDirection.x, Device.vCameraDirection.z };
    if (forward.magnitude() < EPS_L)
        forward.set(0.f, 1.f);
    forward.normalize();
    const Fvector2 right{ forward.y, -forward.x };

    SUIShapeStyle look;
    if (const SUIShapeSkin* skin = UITheme::Skin("damage_arc"))
        look = skin->looks[SUIShapeSkin::Normal];
    else
    {
        look.fill = color_rgba(214, 58, 46, 255);
        look.border = 9.f;
        look.gap = 0.5f;
    }
    look.kind = SUIShapeStyle::Kind::HitArc;
    const u32 alpha = color_get_A(look.fill);
    const float spread = look.gap;

    // a square on the screen round its centre
    const float kx = UI().get_current_kx();
    const float cx = UI_BASE_WIDTH * 0.5f, cy = UI_BASE_HEIGHT * 0.5f;
    const Frect ring{ cx - RingRadius * kx, cy - RingRadius, cx + RingRadius * kx, cy + RingRadius };

    const float now = Device.fTimeGlobal;
    for (auto it = m_marks.begin(); it != m_marks.end();)
    {
        const float t = (now - it->start) / it->life;
        if (t >= 1.f)
        {
            it = m_marks.erase(it);
            continue;
        }
        // full at once, held for a moment, then gone softly
        const float fade = t < 0.15f ? 1.f : 1.f - (t - 0.15f) / 0.85f;
        const float opacity = it->strength * fade * fade;

        SUIShapeStyle arc = look;
        arc.fraction = std::atan2(it->from.dotproduct(right), it->from.dotproduct(forward));
        // a harder hit spreads wider round the ring
        arc.gap = spread * (0.75f + 0.5f * it->strength);
        arc.fill = subst_alpha(look.fill, u32(alpha * opacity));
        UIShape::Draw(ring, arc);
        ++it;
    }
}
