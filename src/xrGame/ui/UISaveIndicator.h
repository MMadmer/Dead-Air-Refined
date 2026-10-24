// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Static/UIShape.h"

// The save indicator: an ash-white gas mask on a dark disc at the middle of the screen's right edge, computed per
// pixel by the xms_ui_shape shader, big and bright enough to be caught from the corner of the eye. It slides in when
// a save the player is meant to see starts and an arc runs round the disc while the save is in flight. Then the arc
// closes into a ring: a success sends a ping out of it and a tick pops up on the mask's cheek; a failure turns ring,
// mask and disc red, shakes them and pops up a cross, and holds longer. Then it slides out. It replaces the
// "Saving...", "Game saved: <name>" and "Save failed" captions. The look and the place come from
// ui_save_indicator.xml.
class CUISaveIndicator final
{
public:
    // A save with its status shown was asked for. `holdMs`: it counts as in flight at least that long (a look at it).
    void Request(u32 holdMs = 0);
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
        // the disc (fill, its rim in line and border), the arc and the ring (fill, border its thickness), the mask
        SUIShapeStyle plate, ring, mask;
        // a failure: fill the ring's and the mask's colour, line the disc's
        u32 failed{color_rgba(229, 72, 61, 255)};
        u32 failedPlate{color_rgba(58, 14, 11, 240)};
        // 768-line units: the gap to the screen's right edge, the disc's centre, its diameter, the way it slides in
        float right{24.f};
        float y{384.f};
        float size{52.f};
        float slide{24.f};
        // the mask's height as a share of the disc's, the arc's length (radians)
        float maskShare{0.76f};
        float arc{2.2f};
        // seconds
        float enter{0.25f};
        float spin{1.1f};
        float close{0.25f};
        float badgeIn{0.3f};
        float ping{0.55f};
        float shake{0.45f};
        float hold{1.6f};
        float holdFailed{3.f};
        float exit{0.4f};
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
    u32 m_holdMs{};
    u32 m_requestFrame{};
    u32 m_drawnFrame{u32(-1)};
    // how far it is in (0 out of sight, 1 in place) last frame and as the current phase began: a change never jumps
    float m_shown{};
    float m_from{};
    // where the arc was: the ring closes from there
    float m_angle{};
};
