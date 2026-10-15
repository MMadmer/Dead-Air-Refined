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
} // namespace

void CUISaveIndicator::ReadLook()
{
    m_lookRead = true;
    m_look.shape.kind = SUIShapeStyle::Kind::GasMask;
    m_look.shape.fill = color_rgba(0, 0, 0, 191);
    m_look.shape.line = color_rgba(255, 255, 255, 26);
    m_look.shape.border = 4.f;

    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_save_indicator.xml", false))
        return;
    constexpr pcstr node = "save_indicator";
    UIShape::Read(xml, node, 0, m_look.shape);
    // the shape is the indicator's whatever the layout says
    m_look.shape.kind = SUIShapeStyle::Kind::GasMask;
    const auto read = [&](pcstr name, float& value) { value = xml.ReadAttribFlt(node, 0, name, value); };
    read("right", m_look.right);
    read("y", m_look.y);
    read("size", m_look.size);
    read("fade_in", m_look.fadeIn);
    read("pulse_period", m_look.pulsePeriod);
    read("pulse_low", m_look.pulseLow);
    read("rise", m_look.rise);
    read("badge_in", m_look.badgeIn);
    read("hold", m_look.hold);
    read("fade_out", m_look.fadeOut);
}

void CUISaveIndicator::Enter(Phase phase, u32 now)
{
    m_phase = phase;
    m_phaseAt = now;
    m_from = m_opacity;
}

void CUISaveIndicator::Request()
{
    const u32 now = Device.dwTimeContinual;
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

    const u32 now = Device.dwTimeContinual;
    if (m_phase == Phase::Saving)
    {
        const bool inFlight = CALifeStorageManager::save_in_flight() ||
            (!m_finished && now - m_requestedAt < requestGraceMs);
        if (!inFlight)
            Enter(Phase::Result, now);
    }

    const float start = m_from;
    float opacity;
    SUIShapeStyle shape = m_look.shape;
    if (m_phase == Phase::Saving)
    {
        // up from where it was, then a smooth blink between the full alpha and pulse_low of it
        const float t = Seconds(now);
        if (t < m_look.fadeIn)
            opacity = start + (1.f - start) * smooth(t / m_look.fadeIn);
        else
        {
            const float wave = 0.5f - 0.5f * std::cos(PI_MUL_2 * (t - m_look.fadeIn) / std::max(m_look.pulsePeriod, 0.1f));
            opacity = 1.f - (1.f - m_look.pulseLow) * wave;
        }
        m_opacity = opacity;
        if (Device.dwFrame <= m_requestFrame + thumbnailFrames)
            return;
    }
    else
    {
        // up to the full alpha, the badge pops, a hold, the fade
        const float t = Seconds(now);
        const float fadeAt = m_look.rise + m_look.hold;
        if (t >= fadeAt + m_look.fadeOut)
        {
            m_phase = Phase::Idle;
            m_opacity = 0.f;
            return;
        }
        opacity = t < fadeAt ? start + (1.f - start) * smooth(t / std::max(m_look.rise, 0.01f)) :
                               1.f - smooth((t - fadeAt) / std::max(m_look.fadeOut, 0.01f));
        shape.badge = m_failed ? SUIShapeStyle::Badge::Failed : SUIShapeStyle::Badge::Done;
        shape.badgeScale = pop(t / std::max(m_look.badgeIn, 0.01f));
        m_opacity = opacity;
    }

    shape.fill = scale_alpha(shape.fill, opacity);
    shape.line = scale_alpha(shape.line, opacity);

    // square on screen: layout x units are narrower than y units in a 16:9 layout
    const float kx = UI().ClientToScreenScaledY(1.f) / std::max(UI().ClientToScreenScaledX(1.f), 0.001f);
    const float width = m_look.size * kx;
    const float rightEdge = UI_BASE_WIDTH - m_look.right * kx;
    const float top = m_look.y - m_look.size * 0.5f;
    UIShape::Draw(m_shader, Frect().set(rightEdge - width, top, rightEdge, top + m_look.size), shape);
}
