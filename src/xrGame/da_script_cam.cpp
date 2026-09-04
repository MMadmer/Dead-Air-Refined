#include "StdAfx.h"
#include "da_script_cam.h"

// Direction is smoothed softer than position on purpose: an angular jerk reads far stronger than
// the same jerk in place. Numbers from the Monolith original, tuned on this very task.
static constexpr u32 da_cam_direction_smoothing = 12;
static constexpr u32 da_cam_position_smoothing = 6;

CDaScriptCamEffector::CDaScriptCamEffector() : CEffectorCam(cefScriptOverride, 0.f)
{
    m_camera.identity();
    m_hpb.set(0.f, 0.f, 0.f);
    m_position.set(0.f, 0.f, 0.f);
    m_smoothing = 0;
    m_hud_enabled = false;
}

void CDaScriptCamEffector::ema(Fvector& current, const Fvector& target, u32 steps)
{
    // First frame: nothing to blend from, so start at the target instead of sliding in from the
    // origin across the whole map.
    if (fis_zero(current.x) && fis_zero(current.y) && fis_zero(current.z))
    {
        current.set(target);
        return;
    }

    // Frame-time-aware moving average, capped at one so a frame hitch cannot overshoot.
    const float alpha = 2.f / float(steps + 1);
    const float k = std::min(1.f, alpha * (float(Device.dwTimeDelta) / float(steps)));

    current.x += k * (target.x - current.x);
    current.y += k * (target.y - current.y);
    current.z += k * (target.z - current.z);
}

bool CDaScriptCamEffector::ProcessCam(SCamEffectorInfo& info)
{
    Fmatrix target;
    target.identity().setHPB(m_hpb.x, m_hpb.y, m_hpb.z).translate_over(m_position);

    if (m_smoothing == 1)
    {
        // The script integrates its own trajectory per frame; smoothing on top would read as lag.
        m_camera.j = target.j;
        m_camera.k = target.k;
        m_camera.c = target.c;
    }
    else
    {
        const u32 dir = m_smoothing ? m_smoothing : da_cam_direction_smoothing;
        const u32 pos = m_smoothing ? m_smoothing : da_cam_position_smoothing;
        ema(m_camera.j, target.j, dir);
        ema(m_camera.k, target.k, dir);
        ema(m_camera.c, target.c, pos);
    }

    info.n.set(m_camera.j);
    info.d.set(m_camera.k);
    info.p.set(m_camera.c);
    return true;
}
