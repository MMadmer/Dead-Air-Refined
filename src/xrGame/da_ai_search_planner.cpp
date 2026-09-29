// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_search.h"
#include "da_ai_knowledge.h"
#include "da_ai_tactics.h"

#include "ai/stalker/ai_stalker.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "detail_path_manager_space.h"
#include "enemy_manager.h"
#include "memory_manager.h"
#include "movement_manager_space.h"
#include "property_storage.h"
#include "sight_manager.h"
#include "stalker_combat_action_base.h"
#include "stalker_decision_space.h"
#include "stalker_movement_manager_smart_cover.h"
#include "stalker_property_evaluators.h"

using namespace MonsterSpace;
using namespace StalkerDecisionSpace;

namespace
{
using DaAi::SearchRole;

class CDaAiEvaluatorContactLost final : public CStalkerPropertyEvaluator
{
    using inherited = CStalkerPropertyEvaluator;

public:
    CDaAiEvaluatorContactLost(CAI_Stalker* object, pcstr name)
        : inherited(object ? object->lua_game_object() : nullptr, name)
    {
    }

    _value_type evaluate() override { return DaAi::Search::contact_lost(*m_object); }
};

// Contact lost: the ambush chain (look out, hold, detour) is over, whatever step it had reached or restarted; the
// flags the stock search sub-planner requires are set at once and the planner moves on to it.
class CDaAiActionSearchHandover final : public CStalkerActionCombatBase
{
    using inherited = CStalkerActionCombatBase;

public:
    CDaAiActionSearchHandover(CAI_Stalker* object, pcstr name) : inherited(object, name) {}

    void initialize() override
    {
        inherited::initialize();
        m_storage->set_property(eWorldPropertyInCover, false);
        m_storage->set_property(eWorldPropertyLookedOut, true);
        m_storage->set_property(eWorldPropertyPositionHolded, true);
        m_storage->set_property(eWorldPropertyEnemyDetoured, true);
        aim_ready();
        if (const CEntityAlive* enemy = object().memory().enemy().selected())
            DaAi::Search::on_contact_lost(object(), enemy->ID());
    }
};

class CDaAiEvaluatorSearchSpot final : public CStalkerPropertyEvaluator
{
    using inherited = CStalkerPropertyEvaluator;

public:
    CDaAiEvaluatorSearchSpot(CAI_Stalker* object, pcstr name)
        : inherited(object ? object->lua_game_object() : nullptr, name)
    {
    }

    _value_type evaluate() override { return DaAi::Search::wants_sweep(*m_object); }
};

class CDaAiEvaluatorSearchCover final : public CStalkerPropertyEvaluator
{
    using inherited = CStalkerPropertyEvaluator;

public:
    CDaAiEvaluatorSearchCover(CAI_Stalker* object, pcstr name)
        : inherited(object ? object->lua_game_object() : nullptr, name)
    {
    }

    _value_type evaluate() override { return DaAi::Search::wants_overwatch(*m_object); }
};

// Shared by both search operators: combat movement, a new hit sends the combat planner back to cover (as the stock
// search actions do), and the destination is set only when the task changes.
class CDaAiActionSearchBase : public CStalkerActionCombatBase
{
    using inherited = CStalkerActionCombatBase;

    CPropertyStorage* m_combat_storage;
    u32 m_last_hit_time{};
    u32 m_destination{u32(-1)};
    u32 m_goal{u32(-1)};

    u32 last_hit_time() const { return DaAi::Know::hit_time(object(), object().memory().enemy().selected()); }

protected:
    CDaAiActionSearchBase(CAI_Stalker* object, CPropertyStorage* combat_storage, pcstr name)
        : inherited(object, name), m_combat_storage(combat_storage)
    {
    }

    // true = a new hit arrived and the combat planner has to choose again
    bool hit_again()
    {
        const u32 time = last_hit_time();
        if (time <= m_last_hit_time)
            return false;
        m_last_hit_time = time;
        m_combat_storage->set_property(eWorldPropertyLookedOut, false);
        m_combat_storage->set_property(eWorldPropertyPositionHolded, false);
        m_combat_storage->set_property(eWorldPropertyEnemyDetoured, false);
        return true;
    }

    // true = a new destination
    bool go_to(const Fvector& position, u32 vertex)
    {
        if (vertex == m_destination)
            return false;
        m_destination = vertex;
        auto& movement = object().movement();
        if (movement.accessible(vertex))
        {
            movement.set_level_dest_vertex(vertex);
            movement.set_desired_position(nullptr);
        }
        else
            movement.set_nearest_accessible_position(position, vertex);
        return true;
    }

    // A short gun goes by covered bounds or holds where it is and watches (holding); other weapons walk straight.
    // true = a new goal (not a new bound on the way to it).
    bool go_covered(const Fvector& position, u32 vertex, bool& holding)
    {
        const bool fresh = vertex != m_goal;
        m_goal = vertex;
        Fvector step_position;
        u32 step_vertex;
        holding = false;
        switch (DaAi::Tactics::covered_step(object(), position, step_position, step_vertex))
        {
        case DaAi::Tactics::Step::direct: go_to(position, vertex); break;
        case DaAi::Tactics::Step::bound: go_to(step_position, step_vertex); break;
        default:
            holding = true;
            m_destination = u32(-1);
            object().movement().set_nearest_accessible_position();
            break;
        }
        return fresh;
    }

    void look_at(const Fvector& position, float height)
    {
        object().sight().setup(CSightAction(
            SightManager::eSightTypePosition, Fvector().set(position.x, position.y + height, position.z), true));
    }

public:
    void initialize() override
    {
        inherited::initialize();
        auto& movement = object().movement();
        movement.set_desired_direction(nullptr);
        movement.set_path_type(MovementManager::ePathTypeLevelPath);
        movement.set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
        movement.set_mental_state(eMentalStateDanger);
        movement.set_body_state(eBodyStateStand);
        movement.set_movement_type(eMovementTypeWalk);
        aim_ready();
        object().agent_manager().member().member(m_object).cover(0);
        m_last_hit_time = last_hit_time();
        m_destination = u32(-1);
        m_goal = u32(-1);
    }
};

// Walks to the assigned spot looking into it; between two spots it holds and watches the likely escape route.
class CDaAiActionSearchSweep final : public CDaAiActionSearchBase
{
    using inherited = CDaAiActionSearchBase;

public:
    CDaAiActionSearchSweep(CAI_Stalker* object, CPropertyStorage* combat_storage, pcstr name)
        : inherited(object, combat_storage, name)
    {
    }

    void execute() override
    {
        inherited::execute();
        if (hit_again())
            return;

        auto& movement = object().movement();
        if (const DaAi::SearchTask* task = DaAi::Search::task(object()); task && task->role == SearchRole::sweep)
        {
            // Loud searchers call to each other on the way to every spot; quiet ones sneak up on it.
            bool holding;
            if (go_covered(task->pos, task->vertex, holding) && !DaAi::Search::quiet(object()))
            {
                u32 max_stop, min_stop;
                DaAi::Search::bark_pause(max_stop, min_stop);
                play_start_search_sound(0, 0, max_stop, min_stop);
            }
            // A crouch needs a mind other than free (asserted), whatever changed it since initialize.
            movement.set_mental_state(eMentalStateDanger);
            // A short gun with no covered way watches the spot from where it is.
            const bool crouch = holding || DaAi::Search::sneak(object(), task->pos);
            movement.set_body_state(crouch ? eBodyStateCrouch : eBodyStateStand);
            movement.set_movement_type(holding ? eMovementTypeStand : eMovementTypeWalk);
            look_at(task->pos, 1.f);
            return;
        }

        if (const DaAi::SearchState* state = DaAi::Search::state(object()))
        {
            movement.set_movement_type(eMovementTypeStand);
            look_at(Fvector(state->lkp).mad(state->escape, 10.f), 1.f);
        }
    }
};

// Moves to a post from which it sees the spot its partner sweeps, then crouches and covers it.
class CDaAiActionSearchOverwatch final : public CDaAiActionSearchBase
{
    using inherited = CDaAiActionSearchBase;

public:
    CDaAiActionSearchOverwatch(CAI_Stalker* object, CPropertyStorage* combat_storage, pcstr name)
        : inherited(object, combat_storage, name)
    {
    }

    void execute() override
    {
        inherited::execute();
        if (hit_again())
            return;

        const DaAi::SearchTask* task = DaAi::Search::task(object());
        if (!task || task->role != SearchRole::overwatch)
            return;

        auto& movement = object().movement();
        bool holding;
        go_covered(task->post, task->post_vertex, holding);
        if (holding || movement.path_completed() || object().Position().distance_to_xz(task->post) < 1.5f)
        {
            movement.set_movement_type(eMovementTypeStand);
            movement.set_mental_state(eMentalStateDanger);
            movement.set_body_state(eBodyStateCrouch);
        }
        else
        {
            movement.set_movement_type(eMovementTypeWalk);
            movement.set_body_state(eBodyStateStand);
        }
        look_at(task->pos, 1.f);
    }
};
} // namespace

namespace DaAi::Search
{
CPropertyEvaluator<CScriptGameObject>* make_contact_lost_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorContactLost>(object, name);
}

CActionBase<CScriptGameObject>* make_search_handover_action(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiActionSearchHandover>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_search_spot_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorSearchSpot>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_search_cover_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorSearchCover>(object, name);
}

CActionBase<CScriptGameObject>* make_search_sweep_action(
    CAI_Stalker* object, CPropertyStorage* combat_storage, pcstr name)
{
    return xr_new<CDaAiActionSearchSweep>(object, combat_storage, name);
}

CActionBase<CScriptGameObject>* make_search_overwatch_action(
    CAI_Stalker* object, CPropertyStorage* combat_storage, pcstr name)
{
    return xr_new<CDaAiActionSearchOverwatch>(object, combat_storage, name);
}
} // namespace DaAi::Search
