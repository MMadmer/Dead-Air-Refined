// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Static/UIShape.h"

// The save indicator: a semi-transparent gas mask at the right edge of the screen, computed per
// pixel by the xms_ui_shape shader. It blinks while a save the player is meant to see is in flight,
// then rises to its full alpha with a tick (or a cross, if a save failed) popping up on its cheek,
// holds and fades out. It replaces the "Saving...", "Game saved: <name>" and "Save failed"
// captions. The look and the place come from ui_save_indicator.xml.
class CUISaveIndicator final
{
public:
    // A save with its status shown was asked for.
    void Request();
    // A save with its status shown ended. The outcome waits until no other save is in flight.
    void Finish(bool succeeded);
    // The frame's UI pass, in game and over the main menu: draws at most once a frame.
    void Render();

private:
    enum class Phase : u8
    {
        Idle,
        Saving,
        Result,
    };

    struct Look
    {
        SUIShapeStyle shape;
        // 768-line units: the gap to the screen's right edge, the icon's centre, its height
        float right{12.f};
        float y{698.f};
        float size{34.f};
        // seconds
        float fadeIn{0.3f};
        float pulsePeriod{1.4f};
        float rise{0.2f};
        float badgeIn{0.3f};
        float hold{1.2f};
        float fadeOut{0.6f};
        // the share of the full alpha a blink dips to
        float pulseLow{0.25f};
    };

    void ReadLook();
    void Enter(Phase phase, u32 now);
    float Seconds(u32 now) const { return (now - m_phaseAt) * 0.001f; }

    ui_shader m_shader;
    Look m_look;
    bool m_lookRead{};
    Phase m_phase{Phase::Idle};
    bool m_failed{};
    bool m_finished{};
    u32 m_phaseAt{};
    u32 m_requestedAt{};
    u32 m_requestFrame{};
    u32 m_drawnFrame{u32(-1)};
    // the last frame's opacity and the one the current phase starts from: a change never jumps
    float m_opacity{};
    float m_from{};
};
