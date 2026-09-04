#pragma once

// Script-owned camera for scenes that drive the view for a while (the ledge climb).
//
// The stock effectors ADD a correction to what the actor camera already computed (recoil, the
// walk bob, a hit). A climb needs the opposite: the camera has to sit exactly where the script
// puts it, and no bob may leak in. So ProcessCam overwrites position and axes instead of reading
// them, and AbsolutePositioning() puts the effector in front of the queue - otherwise the stock
// camera runs after it and wipes the result (CCameraManager::UpdateDeffered).
//
// Smoothing is a moving average with the frame time folded in, so a fast machine does not turn
// the same target into a twice sharper move. The state is kept as a matrix, not as angles: an
// angle blend flips sign across +-180 deg and the camera spins the long way round.
//
// Scheme after the Monolith engine's CFPCamEffector (demonized); rewritten for CEffectorCam.

#include "xrEngine/Effector.h"

class CDaScriptCamEffector final : public CEffectorCam
{
public:
    CDaScriptCamEffector();

    // The target the script writes every frame: position and heading/pitch/bank.
    Fvector m_position;
    Fvector m_hpb;

    // Smoothed state as a basis: j = up, k = forward, c = position.
    Fmatrix m_camera;

    // 0 = default smoothing, 1 = snap to the target, larger = softer.
    u32 m_smoothing;

    // Whether the hands are drawn over the owned camera.
    bool m_hud_enabled;

    // Lives until removed explicitly: a climb has no known duration up front.
    bool Valid() override { return true; }
    bool AbsolutePositioning() override { return true; }
    bool ProcessCam(SCamEffectorInfo& info) override;

private:
    static void ema(Fvector& current, const Fvector& target, u32 steps);
};
