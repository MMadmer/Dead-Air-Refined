////////////////////////////////////////////////////////////////////////////
//	Module 		: script_sound.cpp
//	Created 	: 06.02.2004
//  Modified 	: 06.02.2004
//	Author		: Dmitriy Iassenev
//	Description : XRay Script sound class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "script_particles.h"
#include "xrEngine/ObjectAnimator.h"

CScriptParticlesCustom::CScriptParticlesCustom(CScriptParticles* owner, LPCSTR caParticlesName)
    : CParticlesObject(caParticlesName, FALSE, true)
{
    //	CScriptParticlesCustom* self = this;
    //	Msg							("CScriptParticlesCustom: 0x%08x",*(int*)&self);
    m_owner = owner;
    m_animator = 0;
}

// XRCORE_API		fastdelegate::FastDelegate< void () >	g_verify_stalkers;

CScriptParticlesCustom::~CScriptParticlesCustom()
{
    //	CScriptParticlesCustom* self = this;
    //	Msg							("~CScriptParticlesCustom: 0x%08x",*(int*)&self);
    //	if ( g_verify_stalkers )
    //		g_verify_stalkers		();

    xr_delete(m_animator);

    //	if ( g_verify_stalkers )
    //		g_verify_stalkers		();
}

void CScriptParticlesCustom::PSI_internal_delete()
{
    if (m_owner)
        m_owner->m_particles = NULL;
    CParticlesObject::PSI_internal_delete();
}

void CScriptParticlesCustom::PSI_destroy()
{
    if (m_owner)
        m_owner->m_particles = NULL;
    CParticlesObject::PSI_destroy();
}

void CScriptParticlesCustom::shedule_Update(u32 _dt)
{
    CParticlesObject::shedule_Update(_dt);
    if (m_animator)
    {
        float dt = float(_dt) / 1000.f;
        Fvector prev_pos = m_animator->XFORM().c;
        m_animator->Update(dt);
        Fvector vel;
        vel.sub(m_animator->XFORM().c, prev_pos).div(dt);
        UpdateParent(m_animator->XFORM(), vel);
    }
}
void CScriptParticlesCustom::LoadPath(LPCSTR caPathName)
{
    if (!m_animator)
        m_animator = xr_new<CObjectAnimator>();
    if ((0 == m_animator->Name()) || (0 != xr_strcmp(m_animator->Name(), caPathName)))
    {
        m_animator->Clear();
        m_animator->Load(caPathName);
    }
}
void CScriptParticlesCustom::StartPath(bool looped)
{
    VERIFY(m_animator);
    m_animator->Play(looped);
}
void CScriptParticlesCustom::PausePath(bool val)
{
    VERIFY(m_animator);
    m_animator->Pause(val);
}

void CScriptParticlesCustom::StopPath()
{
    VERIFY(m_animator);
    m_animator->Stop();
}

void CScriptParticlesCustom::remove_owner()
{
    R_ASSERT(m_owner);
    m_owner = 0;
}

CScriptParticles::CScriptParticles(LPCSTR caParticlesName)
{
    m_particles = xr_new<CScriptParticlesCustom>(this, caParticlesName);
    m_transform.identity();
}

CScriptParticles::~CScriptParticles()
{
    if (m_particles)
    {
        // destroy particles
        m_particles->remove_owner();
        m_particles->PSI_destroy();
        m_particles = 0;
    }
}

void CScriptParticles::Play()
{
    if (!m_particles)
        return;
    m_particles->Play(false);
}

void CScriptParticles::PlayAtPos(const Fvector& position)
{
    if (!m_particles)
        return;
    //m_particles->play_at_pos(position);
    m_transform.translate_over(position);
    m_particles->UpdateParent(m_transform, zero_vel);
    m_particles->Play(false);
    m_particles->UpdateParent(m_transform, zero_vel);
}

void CScriptParticles::Stop()
{
    if (!m_particles)
        return;
    m_particles->Stop(FALSE);
}

void CScriptParticles::StopDeferred()
{
    if (!m_particles)
        return;
    m_particles->Stop(TRUE);
}

void CScriptParticles::MoveTo(const Fvector& pos, const Fvector& vel)
{
    if (!m_particles)
        return;
    //Fmatrix XF;
    //XF.translate(pos);
    m_transform.translate_over(pos);

    //m_particles->UpdateParent(XF, vel);
    m_particles->UpdateParent(m_transform, vel);
}

void CScriptParticles::SetDirection(const Fvector& dir)
{
    if (!m_particles)
        return;
    Fmatrix matrix;
    matrix.identity();
    matrix.k.set(dir);
    Fvector::generate_orthonormal_basis_normalized(matrix.k, matrix.j, matrix.i);
    matrix.translate_over(m_transform.c);
    m_transform.set(matrix);
    m_particles->UpdateParent(matrix, zero_vel);
}

void CScriptParticles::SetOrientation(float yaw, float pitch, float roll)
{
    if (!m_particles)
        return;
    Fmatrix matrix;
    matrix.setHPB(yaw, pitch, roll); // ?????????? matrix.c
    matrix.translate_over(m_transform.c);
    m_transform.set(matrix);
    m_particles->UpdateParent(matrix, zero_vel);
}

bool CScriptParticles::IsPlaying() const
{
    if (!m_particles)
        return false;
    return m_particles->IsPlaying();
}

bool CScriptParticles::IsLooped() const
{
    if (!m_particles)
        return false;
    return m_particles->IsLooped();
}

void CScriptParticles::LoadPath(LPCSTR caPathName)
{
    if (!m_particles)
        return;
    m_particles->LoadPath(caPathName);
}
// The engine frees a finished non-looped effect and nulls m_particles; a script that still holds
// the wrapper may call any of these afterwards, and that is routine mod usage, not a fault.
void CScriptParticles::StartPath(bool looped)
{
    if (m_particles)
        m_particles->StartPath(looped);
}
void CScriptParticles::StopPath()
{
    if (m_particles)
        m_particles->StopPath();
}
void CScriptParticles::PausePath(bool val)
{
    if (m_particles)
        m_particles->PausePath(val);
}
