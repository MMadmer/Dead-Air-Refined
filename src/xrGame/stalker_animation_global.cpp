////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_global.cpp
//	Created 	: 25.02.2003
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager : global animations
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "FoodItem.h"
#include "property_storage.h"
#include "stalker_movement_manager_smart_cover.h"
#include "ai/stalker/ai_stalker_space.h"
#include "stalker_animation_data.h"
#include "Weapon.h"
#include "Missile.h"
#include "stalker_animation_manager_impl.h"
#include "stalker_planner.h"
#include "stalker_decision_space.h"

using namespace StalkerSpace;

void CStalkerAnimationManager::global_play_callback(CBlend* blend)
{
    CAI_Stalker* object = (CAI_Stalker*)blend->CallbackParam;
    VERIFY(object);

    CStalkerAnimationManager& manager = object->animation();
    CStalkerAnimationPair& pair = manager.global();
    pair.on_animation_end();

    //	std::pair<LPCSTR,LPCSTR>	pair_id =
    // smart_cast<IKinematicsAnimated*>(object->Visual())->LL_MotionDefName_dbg(blend->motionID);
    //	Msg							("[%6d] global callback [%s][%s]", Device.dwTimeGlobal, pair_id.first,
    // pair_id.second);

    if (!manager.m_global_callback)
        return;

    manager.m_call_global_callback = true;
}

MotionID CStalkerAnimationManager::global_critical_hit()
{
    if (!object().critically_wounded())
        return (MotionID());

    if (global().animation())
        return (global().animation());

    // The wound began with a firearm in hand (critical_wound_external_conditions_suitable); a swap or a grenade since
    // leaves no pose to play, so the wound ends here as OnItemDrop ends it.
    CWeapon* weapon = smart_cast<CWeapon*>(object().inventory().ActiveItem());
    const u32 animation_slot = weapon ? weapon->animation_slot() : 0;
    if (animation_slot < 1 || animation_slot > 3)
    {
        object().brain().CStalkerPlanner::m_storage.set_property(
            StalkerDecisionSpace::eWorldPropertyCriticallyWounded, false);
        return MotionID();
    }

    return (global().select(m_data_storage->m_part_animations.A[eBodyStateStand]
                                .m_global.A[object().critical_wound_type() + 6 * (animation_slot - 1)]
                                .A,
        &object().critical_wound_weights()));
}

MotionID CStalkerAnimationManager::assign_global_animation(bool& animation_movement_controller)
{
    if (m_global_selector)
        return (m_global_selector(animation_movement_controller));

    animation_movement_controller = false;

    if (eMentalStatePanic != object().movement().mental_state())
        return (global_critical_hit());

    if (fis_zero(object().movement().speed(object().character_physics_support()->movement())))
        return (MotionID());

    return (global().select(m_data_storage->m_part_animations.A[body_state()].m_global.A[1].A));
}
