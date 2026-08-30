#pragma once

#include <initializer_list>
#include "WeaponBinoculars.h"

// The 3D PDA presenter item (docs/dead-air/pda-3d-port-plan.md §4, pda-1to1-plan.md).
// A hidden WP_BINOC-class item living in the animation slot - the same trick DA already
// ships for the mask-cleaning and knife-hit animations, and the same one the Gunslinger
// original used. Being a hud item buys everything the feature needs for free: hands + item
// model on the HUD render path (sun, dynamic lights, hud self-shadow, rain on the body),
// the show/hide/idle state machine, and the weapon zoom machinery as the second input
// stage ("raised to the face").
//
// The item is a FOLLOWER: the PDA window's visibility is the source of truth
// (da_pda3d::update() raises/holsters the item to match it). Here lives only what a hud
// item must do itself: state glue, the composed idle animation (aim latch + joystick),
// the aim-aware hide, and the headlamp/NV acknowledgement motion.
class CPdaAnimatorItem final : public CWeaponBinoculars
{
    typedef CWeaponBinoculars inherited;

public:
    CPdaAnimatorItem() = default;

    void OnStateSwitch(u32 S, u32 oldState) override;
    void OnZoomIn() override;
    void OnZoomOut() override;
    void UpdateCL() override;
    void net_Destroy() override;
    void OnAnimationEnd(u32 state) override;

    void PlayAnimIdle() override;
    void PlayAnimHide() override;

    // No binocular overlay, no crosshair, no dynamic "vision" marks on the PDA.
    void render_item_ui() override {}
    bool render_item_ui_query() override { return false; }
    bool use_crosshair() const override { return false; }

private:
    void attach_ui();
    void detach_ui();
    // Composed idle: "anm_idle" [+ "_aim"] [+ joystick suffix] [+ "_moving"[ "_crouch"]],
    // walked right-to-left through isHUDAnimationExist until something resolves.
    void play_composed_idle();
    bool play_first_existing(std::initializer_list<pcstr> names, bool mix_in);

    bool m_ui_attached{};
    // Edge latch for the raise-to-face transition animations.
    bool m_aim_started{};
    // A one-shot overlay motion (aim start/end, headlamp ack) is playing; when it ends,
    // idle must be re-entered explicitly - the base OnAnimationEnd does nothing for eIdle.
    bool m_oneshot_playing{};
    // Torch/NV acknowledgement: last seen states, to play the flick motion on change.
    bool m_torch_seen{};
    bool m_nv_seen{};
    bool m_torch_nv_valid{};
};
