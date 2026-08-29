#pragma once

#include "WeaponBinoculars.h"

// The 3D PDA presenter item (docs/dead-air/pda-3d-port-plan.md §4). A hidden WP_BINOC-class
// item living in the animation slot - the same trick DA already ships for the mask-cleaning
// and knife-hit animations, and the same one the Gunslinger original used. Being a hud item
// buys everything the feature needs for free: hands + item model on the HUD render path
// (sun, dynamic lights, hud self-shadow, rain on the body), the show/hide/idle state
// machine, and the weapon zoom machinery as the second input stage ("raised to the face").
//
// The item itself only glues states together: the PDA dialog is shown render-only while the
// device is in hands (no input focus, the player keeps full control), and enters the real
// dialog stack when zoomed in. The screen content arrives through $user$ui + models_pda.s.
class CPdaAnimatorItem final : public CWeaponBinoculars
{
    typedef CWeaponBinoculars inherited;

public:
    CPdaAnimatorItem() = default;

    void OnStateSwitch(u32 S, u32 oldState) override;
    void OnZoomIn() override;
    void OnZoomOut() override;
    void UpdateCL() override;

    // No binocular overlay, no crosshair, no dynamic "vision" marks on the PDA.
    void render_item_ui() override {}
    bool render_item_ui_query() override { return false; }
    bool use_crosshair() const override { return false; }

private:
    void attach_ui();
    void detach_ui();

    bool m_ui_attached{};
};
