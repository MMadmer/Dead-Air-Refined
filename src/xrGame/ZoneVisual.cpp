#include "StdAfx.h"
#include "CustomZone.h"
#include "Include/xrRender/KinematicsAnimated.h"
#include "ZoneVisual.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "Include/xrRender/RenderVisual.h"

CVisualZone::CVisualZone() {}
CVisualZone::~CVisualZone() {}
bool CVisualZone::net_Spawn(CSE_Abstract* DC)
{
    if (!inherited::net_Spawn(DC))
        return (FALSE);

    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeZoneVisual* Z = smart_cast<CSE_ALifeZoneVisual*>(e);
    IKinematicsAnimated* SA = smart_cast<IKinematicsAnimated*>(Visual());

    // The animation names come from level spawn data, not from configs, and some Dead Air
    // levels ship visual zones with an empty attack animation. That is a data gap, not a
    // reason to end the session: fall back to the idle cycle, or to no animation at all.
    m_idle_animation = SA->ID_Cycle_Safe(Z->startup_animation);
    m_attack_animation = SA->ID_Cycle_Safe(Z->attack_animation);

    if (!m_attack_animation.valid())
    {
        Msg("! %s: no attack animation [%s] in model [%s], using the idle cycle", cName().c_str(),
            Z->attack_animation.c_str(), cNameVisual().c_str());
        m_attack_animation = m_idle_animation;
    }

    if (!m_idle_animation.valid())
        Msg("! %s: no startup animation [%s] in model [%s], the zone stays unanimated", cName().c_str(),
            Z->startup_animation.c_str(), cNameVisual().c_str());

    if (m_idle_animation.valid())
        SA->PlayCycle(m_idle_animation);

    setVisible(TRUE);

    return (TRUE);
}

void CVisualZone::SwitchZoneState(EZoneState new_state)
{
    if (m_eZoneState == eZoneStateBlowout && new_state != eZoneStateBlowout && m_idle_animation.valid())
    {
        //	IKinematicsAnimated*	SA=smart_cast<IKinematicsAnimated*>(Visual());
        smart_cast<IKinematicsAnimated*>(Visual())->PlayCycle(m_idle_animation);
    }

    inherited::SwitchZoneState(new_state);
}
void CVisualZone::Load(LPCSTR section)
{
    inherited::Load(section);
    m_dwAttackAnimaionStart = pSettings->r_u32(section, "attack_animation_start");
    m_dwAttackAnimaionEnd = pSettings->r_u32(section, "attack_animation_end");
    VERIFY2(m_dwAttackAnimaionStart < m_dwAttackAnimaionEnd,
        "attack_animation_start must be less then attack_animation_end");
}

void CVisualZone::UpdateBlowout()
{
    inherited::UpdateBlowout();
    if (m_dwAttackAnimaionStart >= (u32)m_iPreviousStateTime && m_dwAttackAnimaionStart < (u32)m_iStateTime &&
        m_attack_animation.valid())
        smart_cast<IKinematicsAnimated*>(Visual())->PlayCycle(m_attack_animation);

    if (m_dwAttackAnimaionEnd >= (u32)m_iPreviousStateTime && m_dwAttackAnimaionEnd < (u32)m_iStateTime &&
        m_idle_animation.valid())
        smart_cast<IKinematicsAnimated*>(Visual())->PlayCycle(m_idle_animation);
}
