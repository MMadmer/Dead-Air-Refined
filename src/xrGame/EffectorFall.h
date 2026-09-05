#pragma once

#include "xrEngine/Effector.h"

// приседание после падения
class CEffectorFall : public CEffectorCam
{
    float fPower;
    float fPhase;

public:
    CEffectorFall(float power, float life_time = 1);
    virtual bool ProcessCam(SCamEffectorInfo& info);
};

// The landing roll: one full forward turn of the view about the camera's right axis over the
// roll's duration, eased at both ends. Yaw stays the player's (the base camera is untouched),
// pitch input is locked by the actor for the same time.
class CEffectorFallRoll : public CEffectorCam
{
    float m_time;
    float m_duration;

public:
    CEffectorFallRoll(float duration);
    virtual bool ProcessCam(SCamEffectorInfo& info);
};

class CEffectorDOF : public CEffectorCam
{
    float m_fPhase;

public:
    CEffectorDOF(const Fvector4& dof);
    virtual bool ProcessCam(SCamEffectorInfo& info);
};
