#include "StdAfx.h"
#include "xrCommon/xr_hash_map.h"
#include "Bolt.h"
#include "Actor.h"
#include "Inventory.h"
#include "Level.h"
#include "ParticlesObject.h"
#include "xrPhysics/PhysicsShell.h"
#include "xrEngine/xr_level_controller.h"

namespace
{
xr_flat_hash_map<shared_str, bool> infiniteBoltSections;
shared_str lastConsumedBoltSection;
bool infiniteBoltRestockRequested = false;
u32 infiniteBoltRestockRequestFrame = 0;

bool is_infinite_bolt(const shared_str& section)
{
    if (!psActorFlags.test(AF_FINITE_BOLTS))
        return true;

    return infiniteBoltSections.try_emplace(
        section, pSettings->read_if_exists<bool>(section, "infinite", false)).first->second;
}
}

void RequestInfiniteBoltRestock()
{
    infiniteBoltRestockRequested = true;
    infiniteBoltRestockRequestFrame = Device.dwFrame;
}

void EnsureInfiniteBoltRestock(CActor& actor)
{
    if (!infiniteBoltRestockRequested || psActorFlags.test(AF_FINITE_BOLTS) || !actor.g_Alive() ||
        !actor.use_bolts() ||
        Device.dwFrame - infiniteBoltRestockRequestFrame < 2)
    {
        return;
    }

    for (CInventoryItem* item : actor.inventory().m_all)
    {
        CBolt* bolt = smart_cast<CBolt*>(item);
        if (bolt && !bolt->getDestroy())
        {
            infiniteBoltRestockRequested = false;
            return;
        }
    }

    const pcstr section = lastConsumedBoltSection.size() ? lastConsumedBoltSection.c_str() : "bolt";
    Level().spawn_item(section, actor.Position(),
        actor.ai_location().level_vertex_id(), actor.ID());
    infiniteBoltRestockRequested = false;
}

CBolt::CBolt(void) { m_thrower_id = u16(-1); }
CBolt::~CBolt(void) {}
void CBolt::Load(LPCSTR section)
{
    inherited::Load(section);
    infiniteBoltSections[m_section_id] = pSettings->read_if_exists<bool>(section, "infinite", false);
}
void CBolt::OnH_A_Chield()
{
    inherited::OnH_A_Chield();
    IGameObject* o = H_Parent()->H_Parent();
    if (o)
        SetInitiator(o->ID());
}

void CBolt::Throw()
{
    CMissile* l_pBolt = smart_cast<CMissile*>(m_fake_missile);
    if (!l_pBolt)
        return;
    l_pBolt->set_destroy_time(u32(m_dwDestroyTimeMax / phTimefactor));
    inherited::Throw();

    if (is_infinite_bolt(m_section_id))
    {
        spawn_fake_missile();
        return;
    }

    if (!Local())
        return;

    lastConsumedBoltSection = m_section_id;

    CInventory* inventory = m_pInventory;
    IGameObject* owner = H_Parent();
    if (inventory && owner)
    {
        CBolt* next = smart_cast<CBolt*>(inventory->SameSlot(BaseSlot(), this, true));
        R_ASSERT2(inventory->Ruck(this), make_string("Cannot consume finite bolt '%s'", cNameSect().c_str()));

        NET_Packet packet;
        u_EventGen(packet, GEG_PLAYER_ITEM2RUCK, owner->ID());
        packet.w_u16(ID());
        u_EventSend(packet);

        if (next)
        {
            R_ASSERT2(inventory->Slot(next->BaseSlot(), next),
                make_string("Cannot activate next finite bolt '%s'", next->cNameSect().c_str()));
            next->u_EventGen(packet, GEG_PLAYER_ITEM2SLOT, owner->ID());
            packet.w_u16(next->ID());
            packet.w_u16(next->BaseSlot());
            next->u_EventSend(packet);
            inventory->SetActiveSlot(next->BaseSlot());
        }
        else if (CActor* actor = smart_cast<CActor*>(inventory->GetOwner()))
        {
            actor->OnPrevWeaponSlot();
        }
    }

    DestroyObject();
}

bool CBolt::Useful() const { return !is_infinite_bolt(m_section_id); }

bool CBolt::GetBriefInfo(II_BriefInfo& info)
{
    info.clear();
    info.name = m_nameShort;
    info.icon = cNameSect();

    const u32 count = m_pInventory ? m_pInventory->dwfGetSameItemCount(cNameSect().c_str(), true) : 1;
    string16 value;
    xr_sprintf(value, "%u", count);
    info.cur_ammo = value;
    return true;
}
bool CBolt::Action(u16 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;
    /*
        switch(cmd)
        {
        case kDROP:
            {
                if(flags&CMD_START)
                {
                    m_throw = false;
                    if(State() == MS_IDLE) State(MS_THREATEN);
                }
                else if(State() == MS_READY || State() == MS_THREATEN)
                {
                    m_throw = true;
                    if(State() == MS_READY) State(MS_THROW);
                }
            }
            return true;
        }
    */
    return false;
}

void CBolt::activate_physic_shell()
{
    inherited::activate_physic_shell();
    m_pPhysicsShell->SetAirResistance(.0001f);
}

void CBolt::SetInitiator(u16 id) { m_thrower_id = id; }
u16 CBolt::Initiator() { return m_thrower_id; }
