#ifndef _EFFECTOR_BOBBING_H
#define _EFFECTOR_BOBBING_H
#pragma once

#include "CameraEffector.h"
#include "xrEngine/CameraManager.h"

class CEffectorBobbing : public CEffectorCam
{
    float fTime;
    float fYAmplitude;
    float fSpeed;

    u32 dwMState;
    float fReminderFactor;
    bool is_limping;
    bool m_bZoomMode;

    float m_fAmplitudeRun;
    float m_fAmplitudeWalk;
    float m_fAmplitudeLimp;

    float m_fSpeedRun;
    float m_fSpeedWalk;
    float m_fSpeedLimp;

    // The step phase runs on at the current pace, so a change of pace (walk, run, crouch) goes on
    // from where the step was; the amplitude follows the pace's own over amplitude_smooth_time.
    float m_phase{};
    float m_amplitude{};
    float m_amplitude_smooth_time{};

public:
    CEffectorBobbing();
    virtual ~CEffectorBobbing();
    virtual bool ProcessCam(SCamEffectorInfo& info);
    void SetState(u32 st, bool limping, bool ZoomMode);
};

#endif //_EFFECTOR_BOBBING_H
