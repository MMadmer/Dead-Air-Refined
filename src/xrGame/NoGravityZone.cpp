#include "StdAfx.h"
#include "NoGravityZone.h"
#include "xrPhysics/PhysicsShell.h"
#include "entity_alive.h"
#include "PHMovementControl.h"

#include "CharacterPhysicsSupport.h"
void CNoGravityZone::enter_Zone(SZoneObjectInfo& io)
{
    inherited::enter_Zone(io);
    switchGravity(io, false);
}
void CNoGravityZone::exit_Zone(SZoneObjectInfo& io)
{
    switchGravity(io, true);
    inherited::exit_Zone(io);
}
void CNoGravityZone::UpdateWorkload(u32 dt)
{
    auto i = m_ObjectInfoMap.begin(), e = m_ObjectInfoMap.end();
    for (; e != i; ++i)
        switchGravity(*i, false);
}
void CNoGravityZone::switchGravity(SZoneObjectInfo& io, bool val)
{
    if (io.object->getDestroy() || (io.zone_ignore && !val))
        return;
    CPhysicsShellHolder* sh = smart_cast<CPhysicsShellHolder*>(io.object);
    if (!sh)
        return;
    CPhysicsShell* shell = sh->PPhysicsShell();
    if (shell && shell->isActive())
    {
        const bool gravityWasEnabled = shell->get_ApplyByGravity();
        shell->set_ApplyByGravity(val);
        if (!val && gravityWasEnabled)
            shell->applyImpulse(Fvector().set(0.f, 1.f, 0.f), 0.1f);
        // shell->SetAirResistance(0.f,0.f);
        // shell->set_DynamicScales(1.f);
        return;
    }
    if (!io.nonalive_object)
    {
        CEntityAlive* ea = smart_cast<CEntityAlive*>(io.object);
        CPHMovementControl* mc = ea->character_physics_support()->movement();
        mc->SetApplyGravity(BOOL(val));
        mc->SetForcedPhysicsControl(!val);
    }
}
