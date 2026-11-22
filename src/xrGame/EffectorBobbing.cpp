#include "StdAfx.h"
#include "EffectorBobbing.h"

#include "Actor.h"
#include "actor_defs.h"

#define BOBBING_SECT "bobbing_effector"

#define CROUCH_FACTOR 0.75f
#define SPEED_REMINDER 5.f

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CEffectorBobbing::CEffectorBobbing()
    : CEffectorCam(eCEBobbing, 10000.f), fYAmplitude(0),
      fSpeed(0), dwMState(0), m_bZoomMode(false)
{
    fTime = 0;
    fReminderFactor = 0;
    is_limping = false;

    m_fAmplitudeRun = pSettings->r_float(BOBBING_SECT, "run_amplitude");
    m_fAmplitudeWalk = pSettings->r_float(BOBBING_SECT, "walk_amplitude");
    m_fAmplitudeLimp = pSettings->r_float(BOBBING_SECT, "limp_amplitude");

    m_fSpeedRun = pSettings->r_float(BOBBING_SECT, "run_speed");
    m_fSpeedWalk = pSettings->r_float(BOBBING_SECT, "walk_speed");
    m_fSpeedLimp = pSettings->r_float(BOBBING_SECT, "limp_speed");
    m_amplitude_smooth_time = pSettings->read_if_exists<float>(BOBBING_SECT, "amplitude_smooth_time", 0.15f);
}

CEffectorBobbing::~CEffectorBobbing() {}
void CEffectorBobbing::SetState(u32 mstate, bool limping, bool ZoomMode)
{
    dwMState = mstate;
    is_limping = limping;
    m_bZoomMode = ZoomMode;
}

bool CEffectorBobbing::ProcessCam(SCamEffectorInfo& info)
{
    fTime += Device.fTimeDelta;
    if (dwMState & ACTOR_DEFS::mcAnyMove)
    {
        if (fReminderFactor < 1.f)
            fReminderFactor += SPEED_REMINDER * Device.fTimeDelta;
        else
            fReminderFactor = 1.f;
    }
    else
    {
        if (fReminderFactor > 0.f)
            fReminderFactor -= SPEED_REMINDER * Device.fTimeDelta;
        else
            fReminderFactor = 0.f;
    }
    if (!fsimilar(fReminderFactor, 0))
    {
        Fmatrix M;
        M.identity();
        M.j.set(info.n);
        M.k.set(info.d);
        M.i.crossproduct(info.n, info.d);
        M.c.set(info.p);

        // apply footstep bobbing effect
        Fvector dangle;
        float k = ((dwMState & ACTOR_DEFS::mcCrouch) ? CROUCH_FACTOR : 1.f);

        float A, pace;

        if (isActorAccelerated(dwMState, m_bZoomMode))
        {
            A = m_fAmplitudeRun * k;
            pace = m_fSpeedRun * k;
        }
        else if (is_limping)
        {
            A = m_fAmplitudeLimp * k;
            pace = m_fSpeedLimp * k;
        }
        else
        {
            A = m_fAmplitudeWalk * k;
            pace = m_fSpeedWalk * k;
        }
        m_phase = std::fmod(m_phase + pace * Device.fTimeDelta, PI_MUL_2);
        const float ST = m_phase;
        if (m_amplitude_smooth_time > EPS_S)
            m_amplitude += (A - m_amplitude) * (1.f - std::exp(-Device.fTimeDelta / m_amplitude_smooth_time));
        else
            m_amplitude = A;
        A = m_amplitude;

        float _sinA = _abs(_sin(ST) * A) * fReminderFactor;
        float _cosA = _cos(ST) * A * fReminderFactor;

        info.p.y += _sinA;
        dangle.x = _cosA;
        dangle.z = _cosA;
        dangle.y = _sinA;

        Fmatrix R;
        R.setHPB(dangle.x, dangle.y, dangle.z);

        Fmatrix mR;
        mR.mul(M, R);

        info.d.set(mR.k);
        info.n.set(mR.j);
    }
    //	else{
    //		fTime		= 0;
    //	}
    return TRUE;
}
