////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_search_actions.cpp
//	Created 	: 25.03.2004
//  Modified 	: 08.10.2007
//	Author		: Dmitriy Iassenev
//	Description : stalker search enemy action classes
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "stalker_search_actions.h"
#include "ai/stalker/ai_stalker.h"
#include "script_game_object.h"
#include "script_game_object_impl.h"
#include "stalker_movement_manager_smart_cover.h"
#include "movement_manager_space.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "hit_memory_manager.h"
#include "sight_manager.h"
#include "cover_evaluators.h"
#include "cover_manager.h"
#include "stalker_movement_restriction.h"
#include "cover_point.h"
#include "detail_path_manager_space.h"
#include "da_ai_knowledge.h"
#include "da_ai_search.h"
#include "da_ai_tactics.h"

using namespace StalkerSpace;
using namespace StalkerDecisionSpace;

//////////////////////////////////////////////////////////////////////////
// CStalkerActionReachEnemyLocation
//////////////////////////////////////////////////////////////////////////

CStalkerActionReachEnemyLocation::CStalkerActionReachEnemyLocation(
    CAI_Stalker* object, CPropertyStorage* combat_storage, LPCSTR action_name)
    : inherited(object, action_name), m_combat_storage(combat_storage)
{
}

void CStalkerActionReachEnemyLocation::initialize()
{
    inherited::initialize();
    object().movement().set_desired_direction(0);
    object().movement().set_path_type(MovementManager::ePathTypeLevelPath);
    object().movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(eBodyStateStand);
    object().movement().set_movement_type(eMovementTypeWalk);

    aim_ready();

    object().agent_manager().member().member(m_object).cover(0);

    m_last_hit_time = DaAi::Know::hit_time(object(), object().memory().enemy().selected());

    //	object().sniper_update_rate			(true);
}

void CStalkerActionReachEnemyLocation::finalize()
{
    inherited::finalize();

    //	object().sniper_update_rate			(false);
}

void CStalkerActionReachEnemyLocation::execute()
{
#ifdef TEST_MENTAL_STATE
    VERIFY((start_level_time() == Device.dwTimeGlobal) || (object().movement().mental_state() == eMentalStateDanger));
#endif // TEST_MENTAL_STATE

    inherited::execute();

    MemorySpace::CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    if (DaAi::Know::hit_time(object(), object().memory().enemy().selected()) > m_last_hit_time)
    {
        m_combat_storage->set_property(eWorldPropertyLookedOut, false);
        m_combat_storage->set_property(eWorldPropertyPositionHolded, false);
        m_combat_storage->set_property(eWorldPropertyEnemyDetoured, false);
        return;
    }

    // The NPC's own sweep already looked into the enemy's last position.
    if (DaAi::Search::lkp_checked(object()))
    {
        m_storage->set_property(eWorldPropertyEnemyLocationReached, true);
        return;
    }

    // A crouch needs a mind other than free (asserted), whatever changed it since initialize.
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(
        DaAi::Search::sneak(object(), mem_object.m_object_params.m_position) ? eBodyStateCrouch : eBodyStateStand);

    // A short gun never walks straight at the enemy's last position over open ground: it bounds from cover to cover,
    // or holds where it is and watches it; held long enough, the position counts as watched.
    Fvector step_position;
    u32 step_vertex;
    const DaAi::Tactics::Step step =
        DaAi::Tactics::covered_step(object(), mem_object.m_object_params.m_position, step_position, step_vertex);
    if (step != DaAi::Tactics::Step::direct)
    {
        if (step == DaAi::Tactics::Step::bound)
        {
            object().movement().set_movement_type(eMovementTypeWalk);
            object().movement().set_level_dest_vertex(step_vertex);
            object().movement().set_desired_position(&step_position);
        }
        else
        {
            object().movement().set_nearest_accessible_position();
            object().movement().set_movement_type(eMovementTypeStand);
            object().movement().set_body_state(eBodyStateCrouch);
            if (step == DaAi::Tactics::Step::given_up)
                m_storage->set_property(eWorldPropertyEnemyLocationReached, true);
        }
        object().sight().setup(CSightAction(SightManager::eSightTypePosition,
            Fvector(mem_object.m_object_params.m_position).add(Fvector().set(0.f, .5f, 0.f)), true));
        return;
    }
    object().movement().set_movement_type(eMovementTypeWalk);

    if (object().movement().path_completed())
    {
#if 0
		object().m_ce_ambush->setup		(mem_object.m_object_params.m_position,mem_object.m_self_params.m_position,10.f);
		const CCoverPoint				*point = ai().cover_manager().best_cover(mem_object.m_object_params.m_position,10.f,*object().m_ce_ambush,CStalkerMovementRestrictor(m_object,true));
		if (!point) {
			object().m_ce_ambush->setup	(mem_object.m_object_params.m_position,mem_object.m_self_params.m_position,10.f);
			point						= ai().cover_manager().best_cover(mem_object.m_object_params.m_position,30.f,*object().m_ce_ambush,CStalkerMovementRestrictor(m_object,true));
		}

		if (point) {
			object().movement().set_level_dest_vertex	(point->level_vertex_id());
			object().movement().set_desired_position	(&point->position());
		}
		else
			object().movement().set_nearest_accessible_position	();
#else
        if (object().movement().accessible(mem_object.m_object_params.m_level_vertex_id))
        {
            object().movement().set_level_dest_vertex(mem_object.m_object_params.m_level_vertex_id);
            //			object().movement().set_desired_position	(0);
        }
        else
        {
            object().movement().set_nearest_accessible_position(
                mem_object.m_object_params.m_position, mem_object.m_object_params.m_level_vertex_id);
        }

        object().sight().setup(CSightAction(SightManager::eSightTypePosition,
            Fvector(mem_object.m_object_params.m_position).add(Fvector().set(0.f, .5f, 0.f)),
            //				mem_object.m_object_params.m_position,
            true));
#endif

        if (object().movement().path_completed())
        {
            m_storage->set_property(eWorldPropertyEnemyLocationReached, true);

#ifndef SILENT_COMBAT
            // A quiet search makes no search barks.
            if (!DaAi::Search::quiet(object()))
                play_start_search_sound(0, 0, 10000, 10000);
#endif // SILENT_COMBAT
        }
    }
    else
    {
        object().sight().setup(CSightAction(SightManager::eSightTypePosition,
            Fvector(mem_object.m_object_params.m_position).add(Fvector().set(0.f, .5f, 0.f)),
            //				mem_object.m_object_params.m_position,
            true));
    }
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionReachAmbushLocation
//////////////////////////////////////////////////////////////////////////

CStalkerActionReachAmbushLocation::CStalkerActionReachAmbushLocation(
    CAI_Stalker* object, CPropertyStorage* combat_storage, LPCSTR action_name)
    : inherited(object, action_name), m_combat_storage(combat_storage)
{
}

void CStalkerActionReachAmbushLocation::initialize()
{
    inherited::initialize();

    m_last_hit_time = DaAi::Know::hit_time(object(), object().memory().enemy().selected());
    m_lost_said = false;
}

void CStalkerActionReachAmbushLocation::finalize() { inherited::finalize(); }
void CStalkerActionReachAmbushLocation::execute()
{
    inherited::execute();

    MemorySpace::CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    if (DaAi::Know::hit_time(object(), object().memory().enemy().selected()) > m_last_hit_time)
    {
        m_combat_storage->set_property(eWorldPropertyLookedOut, false);
        m_combat_storage->set_property(eWorldPropertyPositionHolded, false);
        m_combat_storage->set_property(eWorldPropertyEnemyDetoured, false);
        return;
    }

    object().m_ce_ambush->setup(mem_object.m_object_params.m_position, mem_object.m_self_params.m_position, 10.f);
    const CCoverPoint* point = ai().cover_manager().best_cover(
        mem_object.m_object_params.m_position, 10.f, *object().m_ce_ambush, CStalkerMovementRestrictor(m_object, true));
    if (!point)
    {
        object().m_ce_ambush->setup(mem_object.m_object_params.m_position, mem_object.m_self_params.m_position, 10.f);
        point = ai().cover_manager().best_cover(mem_object.m_object_params.m_position, 30.f, *object().m_ce_ambush,
            CStalkerMovementRestrictor(m_object, true));
    }

    // A short gun reaches the ambush cover by covered bounds, or holds and watches from where it is.
    Fvector step_position;
    u32 step_vertex;
    const DaAi::Tactics::Step step = point ?
        DaAi::Tactics::covered_step(object(), point->position(), step_position, step_vertex) :
        DaAi::Tactics::Step::direct;
    if (step == DaAi::Tactics::Step::hold || step == DaAi::Tactics::Step::given_up)
    {
        object().movement().set_nearest_accessible_position();
        object().movement().set_movement_type(eMovementTypeStand);
        if (step == DaAi::Tactics::Step::given_up)
            m_storage->set_property(eWorldPropertyAmbushLocationReached, true);
        return;
    }
    object().movement().set_movement_type(eMovementTypeWalk);
    if (step == DaAi::Tactics::Step::bound)
    {
        object().movement().set_level_dest_vertex(step_vertex);
        object().movement().set_desired_position(&step_position);
        return;
    }
    if (point)
    {
        object().movement().set_level_dest_vertex(point->level_vertex_id());
        object().movement().set_desired_position(&point->position());
    }
    else
        object().movement().set_nearest_accessible_position();

    if (!object().movement().path_completed())
    {
#ifndef SILENT_COMBAT
        // Once per walk: a destination never reached (one in an anomaly) repeated it every 10 s.
        if (!m_lost_said && !DaAi::Search::quiet(object()))
        {
            play_enemy_lost_sound(0, 0, 10000, 10000);
            m_lost_said = true;
        }
#endif // SILENT_COMBAT
        return;
    }

    m_storage->set_property(eWorldPropertyAmbushLocationReached, true);
}

//////////////////////////////////////////////////////////////////////////
// CStalkerActionHoldAmbushLocation
//////////////////////////////////////////////////////////////////////////

CStalkerActionHoldAmbushLocation::CStalkerActionHoldAmbushLocation(
    CAI_Stalker* object, CPropertyStorage* combat_storage, LPCSTR action_name)
    : inherited(object, action_name), m_combat_storage(combat_storage)
{
}

void CStalkerActionHoldAmbushLocation::initialize()
{
    inherited::initialize();

    m_last_hit_time = DaAi::Know::hit_time(object(), object().memory().enemy().selected());

    // Stock relied on the combat mind of the action before; one that surrendered (morale) comes here with a free mind,
    // and a crouch with a free mind is asserted.
    object().movement().set_mental_state(eMentalStateDanger);
    object().movement().set_body_state(eBodyStateCrouch);
    object().sight().setup(CSightAction(SightManager::eSightTypeCoverLookOver, true));
}

void CStalkerActionHoldAmbushLocation::finalize() { inherited::finalize(); }
void CStalkerActionHoldAmbushLocation::execute()
{
    inherited::execute();

    MemorySpace::CMemoryInfo mem_object = object().memory().memory(object().memory().enemy().selected());

    if (!mem_object.m_object)
        return;

    if (DaAi::Know::hit_time(object(), object().memory().enemy().selected()) > m_last_hit_time)
    {
        m_combat_storage->set_property(eWorldPropertyLookedOut, false);
        m_combat_storage->set_property(eWorldPropertyPositionHolded, false);
        m_combat_storage->set_property(eWorldPropertyEnemyDetoured, false);
        return;
    }

    if (!completed())
        return;

    // The search gives up by its own profile timer, staggered per NPC, instead of right after the hold.
    if (DaAi::Search::on_hold_complete(object()))
        return;

    if (mem_object.m_last_level_time + 60000 < Device.dwTimeGlobal)
        return;

    object().memory().enable(object().memory().enemy().selected(), false);
}
