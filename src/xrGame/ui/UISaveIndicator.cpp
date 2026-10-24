// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UISaveIndicator.h"

#include "alife_storage_manager.h"
#include "xrUICore/XML/xrUIXmlParser.h"

namespace
{
// a request reaches the save queues through the local server: its first moments count as in flight
constexpr u32 requestGraceMs = 500;
// the frames the save's thumbnail is taken from must not carry the icon
constexpr u32 thumbnailFrames = 2;
// an arc this long (radians) is a whole ring: the shader fades it along a Gaussian of this width
constexpr float fullRing = 60.f;

float smooth(float x)
{
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

// past 1 and back: the badge pops instead of growing
float pop(float x)
{
    x = std::clamp(x, 0.f, 1.f) - 1.f;
    constexpr float overshoot = 1.2f;
    return 1.f + (overshoot + 1.f) * x * x * x + overshoot * x * x;
}

u32 scale_alpha(u32 argb, float factor)
{
    return subst_alpha(argb, iFloor(color_get_A(argb) * std::clamp(factor, 0.f, 1.f) + 0.5f));
}

u32 mix(u32 a, u32 b, float t)
{
    t = std::clamp(t, 0.f, 1.f);
    const auto channel = [&](u32 x, u32 y) { return u32(iFloor(float(x) + (float(y) - float(x)) * t + 0.5f)); };
    return color_rgba(channel(color_get_R(a), color_get_R(b)), channel(color_get_G(a), color_get_G(b)),
        channel(color_get_B(a), color_get_B(b)), channel(color_get_A(a), color_get_A(b)));
}
} // namespace

void CUISaveIndicator::ReadLook()
{
    m_lookRead = true;
    // black against ash, the mod's own
    m_look.plate.kind = SUIShapeStyle::Kind::Box;
    m_look.plate.fill = color_rgba(12, 12, 12, 224);
    m_look.plate.line = color_rgba(226, 224, 218, 64);
    m_look.plate.border = 1.5f;
    m_look.ring.kind = SUIShapeStyle::Kind::HitArc;
    m_look.ring.fill = color_rgba(226, 224, 218, 255);
    m_look.ring.border = 4.f;
    m_look.mask.kind = SUIShapeStyle::Kind::GasMask;
    m_look.mask.fill = color_rgba(226, 224, 218, 255);

    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_save_indicator.xml", false))
        return;
    constexpr pcstr node = "save_indicator";
    UIShape::Read(xml, "save_indicator:plate", 0, m_look.plate);
    UIShape::Read(xml, "save_indicator:ring", 0, m_look.ring);
    UIShape::Read(xml, "save_indicator:mask", 0, m_look.mask);
    // the shapes are the indicator's whatever the layout says
    m_look.plate.kind = SUIShapeStyle::Kind::Box;
    m_look.ring.kind = SUIShapeStyle::Kind::HitArc;
    m_look.mask.kind = SUIShapeStyle::Kind::GasMask;
    m_look.failed = UIShape::ParseColor(xml.ReadAttrib("save_indicator:failed", 0, "fill", nullptr), m_look.failed);
    m_look.failedPlate = UIShape::ParseColor(xml.ReadAttrib("save_indicator:failed", 0, "line", nullptr), m_look.failedPlate);
    const auto read = [&](pcstr name, float& value) { value = xml.ReadAttribFlt(node, 0, name, value); };
    read("right", m_look.right);
    read("y", m_look.y);
    read("size", m_look.size);
    read("slide", m_look.slide);
    read("mask", m_look.maskShare);
    read("arc", m_look.arc);
    read("enter", m_look.enter);
    read("spin", m_look.spin);
    read("close", m_look.close);
    read("badge_in", m_look.badgeIn);
    read("ping", m_look.ping);
    read("shake", m_look.shake);
    read("hold", m_look.hold);
    read("hold_failed", m_look.holdFailed);
    read("exit", m_look.exit);
}

void CUISaveIndicator::Enter(Phase phase, u32 now)
{
    m_phase = phase;
    m_phaseAt = now;
    m_from = m_shown;
}

void CUISaveIndicator::Request(u32 holdMs)
{
    const u32 now = Device.dwTimeContinual;
    m_holdMs = holdMs;
    m_requestFrame = Device.dwFrame;
    if (m_phase != Phase::Saving)
    {
        m_failed = false;
        Enter(Phase::Saving, now);
    }
    m_finished = false;
    m_requestedAt = now;
}

void CUISaveIndicator::Finish(bool succeeded)
{
    m_failed |= !succeeded;
    m_finished = true;
    // a save nobody announced (a script's own, say) still reports how it went
    if (m_phase == Phase::Idle)
        Enter(Phase::Result, Device.dwTimeContinual);
}

void CUISaveIndicator::Render()
{
    if (m_phase == Phase::Idle || m_drawnFrame == Device.dwFrame)
        return;
    m_drawnFrame = Device.dwFrame;
    if (!m_lookRead)
        ReadLook();
    const Look& look = m_look;

    const u32 now = Device.dwTimeContinual;
    if (m_phase == Phase::Saving)
    {
        const bool inFlight = CALifeStorageManager::save_in_flight() || now - m_requestedAt < m_holdMs ||
            (!m_finished && now - m_requestedAt < requestGraceMs);
        if (!inFlight)
        {
            m_angle = std::fmod(Seconds(now) / std::max(look.spin, 0.1f), 1.f) * PI_MUL_2;
            Enter(Phase::Result, now);
        }
    }

    const float t = Seconds(now);
    const float enter = std::max(look.enter, 0.01f);
    SUIShapeStyle plate = look.plate, ring = look.ring, mask = look.mask;
    float shown, angle, arc, failure = 0.f, ping = -1.f, dx = 0.f;
    if (m_phase == Phase::Saving)
    {
        // in from where it was, the arc running round
        shown = m_from + (1.f - m_from) * smooth(t / enter);
        angle = t / std::max(look.spin, 0.1f) * PI_MUL_2;
        arc = look.arc;
        m_shown = shown;
        if (Device.dwFrame <= m_requestFrame + thumbnailFrames)
            return;
    }
    else
    {
        // the ring closes where the arc was, the outcome shows, a hold, out
        const float close = std::max(look.close, 0.01f);
        const float exitAt = close + (m_failed ? look.holdFailed : look.hold);
        if (t >= exitAt + look.exit)
        {
            m_phase = Phase::Idle;
            m_shown = 0.f;
            return;
        }
        shown = t < exitAt ? m_from + (1.f - m_from) * smooth(t / enter) : 1.f - smooth((t - exitAt) / std::max(look.exit, 0.01f));
        const float closing = smooth(t / close);
        angle = m_angle;
        arc = look.arc + (fullRing - look.arc) * closing;
        const float outcome = (t - close * 0.5f) / std::max(look.badgeIn, 0.01f);
        mask.badge = m_failed ? SUIShapeStyle::Badge::Failed : SUIShapeStyle::Badge::Done;
        mask.badgeScale = outcome > 0.f ? pop(outcome) : 0.f;
        if (m_failed)
        {
            failure = closing;
            // a short shudder sideways, dying out
            const float s = (t - close * 0.5f) / std::max(look.shake, 0.01f);
            if (s > 0.f && s < 1.f)
                dx = std::sin(s * PI_MUL_2 * 3.5f) * 4.f * (1.f - s);
        }
        else
            ping = (t - close * 0.5f) / std::max(look.ping, 0.01f);
        m_shown = shown;
    }

    // square on screen: layout x units are narrower than y units in a 16:9 layout
    const float kx = UI().ClientToScreenScaledY(1.f) / std::max(UI().ClientToScreenScaledX(1.f), 0.001f);
    const float half = look.size * 0.5f;
    const float cx = UI_BASE_WIDTH - (look.right + half - (1.f - smooth(shown)) * look.slide - dx) * kx;
    const float cy = look.y;
    const auto square = [&](float halfHeight) {
        return Frect().set(cx - halfHeight * kx, cy - halfHeight, cx + halfHeight * kx, cy + halfHeight);
    };

    plate.radius = half;
    plate.fill = scale_alpha(mix(plate.fill, look.failedPlate, failure), shown);
    plate.line = scale_alpha(plate.line, shown);
    UIShape::Draw(m_shader, square(half), plate);

    ring.fraction = angle;
    ring.gap = arc;
    ring.fill = scale_alpha(mix(ring.fill, look.failed, failure), shown);
    UIShape::Draw(m_shader, square(half), ring);

    mask.fill = scale_alpha(mix(mask.fill, look.failed, failure), shown);
    mask.line = scale_alpha(mask.line, shown);
    UIShape::Draw(m_shader, square(half * look.maskShare), mask);

    // a success sends a ring out of the disc
    if (ping > 0.f && ping < 1.f)
    {
        SUIShapeStyle wave = look.ring;
        wave.gap = fullRing;
        wave.fill = scale_alpha(look.ring.fill, (1.f - ping) * (1.f - ping) * shown);
        UIShape::Draw(m_shader, square(half * (1.f + 0.6f * smooth(ping))), wave);
    }
}
