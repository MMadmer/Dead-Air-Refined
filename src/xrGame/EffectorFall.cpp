#include "StdAfx.h"
#include "EffectorFall.h"
#include "CameraEffector.h"
#include "GamePersistent.h"

#define FALL_SPEED 3.5f
#define FALL_MAXDIST 0.15f

CEffectorFall::CEffectorFall(float power, float life_time) : CEffectorCam(eCEFall, life_time)
{
    SetHudAffect(false);
    fPower = (power > 1) ? 1 : ((power < 0) ? 0 : power * power);
    fPhase = 0;
}

bool CEffectorFall::ProcessCam(SCamEffectorInfo& info)
{
    fPhase += FALL_SPEED * Device.fTimeDelta;
    if (fPhase < 1)
        info.p.y -= FALL_MAXDIST * fPower * _sin(M_PI * fPhase + M_PI);
    else
        fLifeTime = -1;
    return TRUE;
}

CEffectorFallRoll::CEffectorFallRoll(float duration)
    : CEffectorCam(eCEFallRoll, duration + 0.2f), m_time(0.f), m_duration(_max(duration, 0.1f))
{
    SetHudAffect(false);
}

bool CEffectorFallRoll::ProcessCam(SCamEffectorInfo& info)
{
    m_time += Device.fTimeDelta;
    const float k = m_time / m_duration;
    if (k >= 1.f)
    {
        fLifeTime = -1;
        return TRUE;
    }
    // Progress with a soft start and stop: a tumble does not snap into rotation.
    const float s = k - _sin(PI_MUL_2 * k) / PI_MUL_2;
    Fmatrix M;
    M.rotation(info.r, PI_MUL_2 * s);
    M.transform_dir(info.d);
    M.transform_dir(info.n);
    info.d.normalize();
    info.n.normalize();
    return TRUE;
}

CEffectorDOF::CEffectorDOF(const Fvector4& dof) : CEffectorCam(eCEDOF, 100000)
{
    GamePersistent().SetEffectorDOF(Fvector().set(dof.x, dof.y, dof.z));
    m_fPhase = Device.fTimeGlobal + dof.w;
}

bool CEffectorDOF::ProcessCam(SCamEffectorInfo& info)
{
    if (m_fPhase < Device.fTimeGlobal)
    {
        GamePersistent().RestoreEffectorDOF();
        fLifeTime = -1;
    }
    return TRUE;
}
