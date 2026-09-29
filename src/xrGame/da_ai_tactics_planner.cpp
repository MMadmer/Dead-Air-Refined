// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_tactics.h"
#include "da_ai_knowledge.h"

#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "ai/stalker/ai_stalker_space.h"
#include "ai_space.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "member_order.h"
#include "sound_player.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "sight_manager.h"
#include "stalker_planner.h"
#include "stalker_decision_space.h"
#include "stalker_combat_action_base.h"
#include "stalker_property_evaluators.h"
#include "stalker_movement_manager_smart_cover.h"
#include "cover_point.h"
#include "movement_manager_space.h"
#include "detail_path_manager_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/ai_object_location.h"

using namespace StalkerDecisionSpace;
using namespace MonsterSpace;

namespace
{
// Replaced stock evaluator: holds the stock one and answers with it while its feature does not decide.
template <typename TStock>
class CDaAiTacticsStockEvaluator : public CStalkerPropertyEvaluator
{
    using inherited = CStalkerPropertyEvaluator;

protected:
    TStock m_stock;

public:
    CDaAiTacticsStockEvaluator(CAI_Stalker* object, pcstr name)
        : inherited(object ? object->lua_game_object() : nullptr, name), m_stock(object, name)
    {
    }

    void setup(CAI_Stalker* object, CPropertyStorage* storage) override
    {
        inherited::setup(object, storage);
        m_stock.setup(object, storage);
    }

    void setup(CScriptGameObject* object, CPropertyStorage* storage) override
    {
        inherited::setup(object, storage);
        m_stock.setup(object, storage);
    }

    _value_type evaluate() override { return m_stock.evaluate(); }
};

// TooFarToKillEnemy (combat planner and get-distance sub-planner): out of band with a covered approach the profile's
// aggression takes. True re-enables the get-distance sub-planner, which bounds from cover to cover.
class CDaAiEvaluatorTooFar final : public CDaAiTacticsStockEvaluator<CStalkerPropertyEvaluatorTooFarToKillEnemy>
{
public:
    using CDaAiTacticsStockEvaluator::CDaAiTacticsStockEvaluator;

    _value_type evaluate() override
    {
        if (!DaAi::feature(DaAi::Feature::doctrine) || !DaAi::Tactics::state_of(object()))
            return m_stock.evaluate();
        return DaAi::Tactics::doctrine(object()).too_far;
    }
};

// ShouldThrowGrenade: the stock conditions plus the profile's gren, a flush trigger and an own belief as the target.
class CDaAiEvaluatorShouldThrowGrenade final
    : public CDaAiTacticsStockEvaluator<CStalkerPropertyEvaluatorShouldThrowGrenade>
{
public:
    using CDaAiTacticsStockEvaluator::CDaAiTacticsStockEvaluator;

    _value_type evaluate() override
    {
        const bool positioned = m_storage->property(eWorldPropertyInCover) ||
            m_storage->property(eWorldPropertyPositionHolded) || m_storage->property(eWorldPropertyEnemyDetoured);
        bool result;
        if (DaAi::Tactics::grenade_decision(
                object(), m_storage->property(eWorldPropertyStartedToThrowGrenade), positioned, result))
        {
            return result;
        }
        return m_stock.evaluate();
    }
};

// The cover the stock best-cover search picked toward the belief, unless the NPC already stands at it.
bool cover_to_reach(const CAI_Stalker& npc, const CCoverPoint* cover)
{
    const float at = DaAi::Tactics::config().at_cover_m;
    return cover && cover->position().distance_to_sqr(npc.Position()) >= _sqr(at);
}

// Over a low wall toward the point the NPC waits or fires crouched; behind a corner it stands to see past it.
void cover_stance(CAI_Stalker& npc, const Fvector& point)
{
    const u32 vertex = npc.ai_location().level_vertex_id();
    if (!ai().level_graph().valid_vertex_id(vertex))
        return;
    Fvector direction = Fvector().sub(point, npc.Position());
    float yaw, pitch;
    direction.getHP(yaw, pitch);
    const bool low_wall = ai().level_graph().low_cover_in_direction(angle_normalize(yaw), vertex) <=
        DaAi::Tactics::config().lof_low_max;
    // A crouch needs a mind other than free (asserted), whatever set it since initialize.
    npc.movement().set_mental_state(eMentalStateDanger);
    npc.movement().set_body_state(low_wall ? eBodyStateCrouch : eBodyStateStand);
}

class CDaAiTacticsEvaluator : public CStalkerPropertyEvaluator
{
    using inherited = CStalkerPropertyEvaluator;

public:
    CDaAiTacticsEvaluator(CAI_Stalker* object, pcstr name)
        : inherited(object ? object->lua_game_object() : nullptr, name)
    {
    }
};

// Outside [min, engage_max] with no covered approach: hold the cover and let the enemy come (DaHoldAmbush).
class CDaAiEvaluatorOutOfBand final : public CDaAiTacticsEvaluator
{
public:
    using CDaAiTacticsEvaluator::CDaAiTacticsEvaluator;

    _value_type evaluate() override { return DaAi::Tactics::doctrine(object()).out_of_band; }
};

// The suppressor or rearguard role, a mate known exposed, the own last-seen point inside the window (F5).
class CDaAiEvaluatorSuppressRole final : public CDaAiTacticsEvaluator
{
public:
    using CDaAiTacticsEvaluator::CDaAiTacticsEvaluator;

    _value_type evaluate() override { return DaAi::Tactics::suppress_role(object()); }
};

class CDaAiEvaluatorShouldFallBack final : public CDaAiTacticsEvaluator
{
public:
    using CDaAiTacticsEvaluator::CDaAiTacticsEvaluator;

    _value_type evaluate() override
    {
        return DaAi::Tactics::should_fall_back(object(), m_storage->property(eWorldPropertyInCover));
    }
};

// Caught in the open at close range and seen: a trained profile shoots back before it runs for cover.
class CDaAiEvaluatorReturnFire final : public CDaAiTacticsEvaluator
{
public:
    using CDaAiTacticsEvaluator::CDaAiTacticsEvaluator;

    _value_type evaluate() override { return DaAi::Tactics::return_fire(object()); }
};

// The ordered flanker with a covered route: it leaves at once, whatever it sees (DaFlank).
class CDaAiEvaluatorFlankRole final : public CDaAiTacticsEvaluator
{
public:
    using CDaAiTacticsEvaluator::CDaAiTacticsEvaluator;

    _value_type evaluate() override { return DaAi::Tactics::flank_go(object()); }
};

// Fires bursts at the own last-seen point and the cover edges beside it, with pauses, from cover: it first moves into
// the best cover toward the belief (so no take-cover step has to win the planner first), then fires from there.
class CDaAiActionSuppressFire final : public CStalkerActionCombatBase
{
    using inherited = CStalkerActionCombatBase;

public:
    CDaAiActionSuppressFire(CAI_Stalker* object, pcstr name) : inherited(object, name) { set_weight(2); }

    void initialize() override
    {
        inherited::initialize();
        CAI_Stalker& npc = object();
        npc.movement().set_desired_direction(nullptr);
        npc.movement().set_path_type(MovementManager::ePathTypeLevelPath);
        npc.movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
        npc.movement().set_nearest_accessible_position();
        npc.movement().set_mental_state(eMentalStateDanger);
        npc.movement().set_body_state(eBodyStateStand);
        npc.movement().set_movement_type(eMovementTypeStand);
        npc.brain().affect_cover(true);
        aim_ready();
        DaAi::Tactics::suppress_begin(npc);
    }

    void execute() override
    {
        inherited::execute();
        CAI_Stalker& npc = object();
        const CEntityAlive* enemy = npc.memory().enemy().selected();
        if (!enemy)
            return;

        const Fvector belief = DaAi::Know::known_position(npc, *enemy);
        const CCoverPoint* cover = npc.best_cover(belief);
        Fvector point = belief;
        if (cover_to_reach(npc, cover))
        {
            // On the way it watches the own last-seen point and holds its fire.
            setup_cover(*cover);
            npc.movement().set_body_state(eBodyStateStand);
            npc.movement().set_movement_type(
                DaAi::Tactics::take_cover_run(npc) ? eMovementTypeRun : eMovementTypeWalk);
            npc.brain().affect_cover(false);
            DaAi::Tactics::suppress_point(npc, point);
            npc.sight().setup(CSightAction(SightManager::eSightTypePosition, point, true));
            aim_ready();
            return;
        }

        npc.movement().set_nearest_accessible_position();
        npc.movement().set_movement_type(eMovementTypeStand);
        npc.brain().affect_cover(true);
        if (cover)
            m_storage->set_property(eWorldPropertyInCover, true);
        const bool burst = DaAi::Tactics::suppress_step(npc, point);
        cover_stance(npc, point);
        npc.sight().setup(CSightAction(SightManager::eSightTypePosition, point, true));
        if (burst)
            fire();
        else
            aim_ready();
    }

    void finalize() override
    {
        inherited::finalize();
        if (object().g_Alive())
            DaAi::Tactics::suppress_end(object());
    }
};

// Moves to a cover away from the belief (a relocation for fanatics), firing only at what it sees on the way.
class CDaAiActionFallBack final : public CStalkerActionCombatBase
{
    using inherited = CStalkerActionCombatBase;

    Fvector m_position{};
    u32 m_vertex{u32(-1)};
    bool m_moving{};
    bool m_cover{}; // the destination is a cover point

public:
    CDaAiActionFallBack(CAI_Stalker* object, pcstr name) : inherited(object, name) {}

    void initialize() override
    {
        inherited::initialize();
        CAI_Stalker& npc = object();
        npc.movement().set_desired_direction(nullptr);
        npc.movement().set_path_type(MovementManager::ePathTypeLevelPath);
        npc.movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
        npc.movement().set_mental_state(eMentalStateDanger);
        npc.movement().set_body_state(eBodyStateStand);
        npc.movement().set_movement_type(DaAi::Tactics::may_run(npc) ? eMovementTypeRun : eMovementTypeWalk);
        npc.brain().affect_cover(false);

        m_moving = DaAi::Tactics::fall_back_begin(npc, m_position, m_vertex, m_cover);
        if (m_moving)
        {
            npc.movement().set_level_dest_vertex(m_vertex);
            npc.movement().set_desired_position(&m_position);
        }
        else
        {
            npc.movement().set_movement_type(eMovementTypeStand);
            npc.movement().set_nearest_accessible_position();
        }
        aim_ready();
    }

    void execute() override
    {
        inherited::execute();
        CAI_Stalker& npc = object();
        const CEntityAlive* enemy = npc.memory().enemy().selected();
        if (enemy && DaAi::Know::sees_now(npc, enemy))
        {
            npc.sight().setup(CSightAction(enemy, true, true));
            fire();
        }
        else
        {
            npc.sight().setup(CSightAction(SightManager::eSightTypePathDirection, true, true));
            aim_ready();
        }

        if (!m_moving)
            return;
        // A destination the path never reaches ends the fall back without claiming the cover.
        if (Device.dwTimeGlobal > start_level_time() + DaAi::Tactics::config().fallback_max_ms)
        {
            m_moving = false;
            DaAi::Tactics::fall_back_end(npc);
            return;
        }
        if (!npc.movement().path_completed())
            return;

        m_moving = false;
        // Only a cover point is cover; a spot straight away over open ground sends it on to take cover.
        m_storage->set_property(eWorldPropertyInCover, m_cover);
        m_storage->set_property(eWorldPropertyLookedOut, false);
        m_storage->set_property(eWorldPropertyPositionHolded, false);
        npc.brain().affect_cover(m_cover);
        npc.best_cover_invalidate();
        DaAi::Tactics::fall_back_end(npc);
    }

    void finalize() override
    {
        inherited::finalize();
        if (!m_moving)
            return;
        m_moving = false;
        if (object().g_Alive())
            DaAi::Tactics::fall_back_end(object());
    }
};

// Outranged: stay in cover, watch the belief's approach and fire only once the fire gate allows it (inside the band).
// Exposed toward the belief it first moves into the cover the best-cover search found (whose path never crosses more
// open ground than a short gun allows); with no cover at all it waits where it stands.
class CDaAiActionHoldAmbush final : public CStalkerActionCombatBase
{
    using inherited = CStalkerActionCombatBase;

    bool m_moving{};

public:
    CDaAiActionHoldAmbush(CAI_Stalker* object, pcstr name) : inherited(object, name) {}

    void initialize() override
    {
        inherited::initialize();
        m_moving = false;
        CAI_Stalker& npc = object();
        npc.movement().set_desired_direction(nullptr);
        npc.movement().set_path_type(MovementManager::ePathTypeLevelPath);
        npc.movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
        npc.movement().set_nearest_accessible_position();
        npc.movement().set_mental_state(eMentalStateDanger);
        npc.movement().set_movement_type(eMovementTypeStand);
        npc.brain().affect_cover(true);
        aim_ready();
        DaAi::Tactics::on_ambush(npc);
    }

    void execute() override
    {
        inherited::execute();
        CAI_Stalker& npc = object();
        const CEntityAlive* enemy = npc.memory().enemy().selected();
        if (!enemy)
            return;

        const Fvector position = DaAi::Know::known_position(npc, *enemy);
        const CCoverPoint* cover = npc.best_cover(position);
        // A move once started goes on to the cover, so a spot that turns covered on the way does not stop it there.
        m_moving = cover_to_reach(npc, cover) && (m_moving || DaAi::Tactics::exposed(npc, position));
        if (m_moving)
        {
            setup_cover(*cover);
            npc.movement().set_body_state(eBodyStateStand);
            npc.movement().set_movement_type(DaAi::Tactics::may_run(npc) ? eMovementTypeRun : eMovementTypeWalk);
            npc.brain().affect_cover(false);
        }
        else
        {
            // Low wall toward the belief: wait crouched behind it.
            npc.movement().set_nearest_accessible_position();
            npc.movement().set_movement_type(eMovementTypeStand);
            npc.brain().affect_cover(true);
            cover_stance(npc, position);
        }

        if (DaAi::Know::sees_now(npc, enemy))
        {
            npc.sight().setup(CSightAction(enemy, true, true));
            fire();
            return;
        }
        aim_ready();
        npc.sight().setup(CSightAction(SightManager::eSightTypePosition, position, true));
    }
};

// Returns fire from where it stands; when the exchange ends (return_fire_ms, lost sight) the planner sends it to cover.
class CDaAiActionReturnFire final : public CStalkerActionCombatBase
{
    using inherited = CStalkerActionCombatBase;

public:
    CDaAiActionReturnFire(CAI_Stalker* object, pcstr name) : inherited(object, name) {}

    void initialize() override
    {
        inherited::initialize();
        CAI_Stalker& npc = object();
        npc.movement().set_desired_direction(nullptr);
        npc.movement().set_path_type(MovementManager::ePathTypeLevelPath);
        npc.movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
        npc.movement().set_nearest_accessible_position();
        npc.movement().set_mental_state(eMentalStateDanger);
        npc.movement().set_body_state(eBodyStateStand);
        npc.movement().set_movement_type(eMovementTypeStand);
        // Not at a cover: a hit here marks no cover as dangerous.
        npc.brain().affect_cover(false);
        aim_ready();
    }

    void execute() override
    {
        inherited::execute();
        CAI_Stalker& npc = object();
        const CEntityAlive* enemy = npc.memory().enemy().selected();
        if (!enemy)
            return;
        // Behind the cover it already has it shoots over a low wall crouched; in the open it stands.
        const Fvector position = DaAi::Know::known_position(npc, *enemy);
        if (!DaAi::Tactics::exposed(npc, position))
            cover_stance(npc, position);
        if (DaAi::Know::sees_now(npc, enemy))
        {
            npc.sight().setup(CSightAction(enemy, true, true));
            fire();
            return;
        }
        aim_ready();
        npc.sight().setup(CSightAction(SightManager::eSightTypePosition, position, true));
    }
};

// The ordered flank: straight on to the covers of the route the order named (the via cover, then the end), at a run
// unless winded, tired or in an exo suit; it fires at what it sees on the move only when its profile does
// (flank_fire). At the end the order is done and the NPC fights from that cover.
class CDaAiActionFlank final : public CStalkerActionCombatBase
{
    using inherited = CStalkerActionCombatBase;

public:
    CDaAiActionFlank(CAI_Stalker* object, pcstr name) : inherited(object, name) {}

    void initialize() override
    {
        inherited::initialize();
        CAI_Stalker& npc = object();
        npc.movement().set_desired_direction(nullptr);
        npc.movement().set_path_type(MovementManager::ePathTypeLevelPath);
        npc.movement().set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
        npc.movement().set_mental_state(eMentalStateDanger);
        npc.movement().set_body_state(eBodyStateStand);
        npc.movement().set_movement_type(DaAi::Tactics::may_run(npc, true) ? eMovementTypeRun : eMovementTypeWalk);
        npc.brain().affect_cover(false);
        CMemberOrder& member = npc.agent_manager().member().member(&npc);
        member.cover(nullptr);
        member.detour(true);
        aim_ready();
        // The move is announced through comms; without them the stock line.
        const CEntityAlive* enemy = npc.memory().enemy().selected();
        if (!DaAi::Tactics::flank_begin(npc) && enemy && enemy->human_being() &&
            npc.agent_manager().member().group_behaviour())
        {
            npc.sound().play(StalkerSpace::eStalkerSoundDetour);
        }
    }

    void execute() override
    {
        inherited::execute();
        CAI_Stalker& npc = object();
        const CEntityAlive* enemy = npc.memory().enemy().selected();
        const CCoverPoint* point = DaAi::Tactics::flank_expired(npc) ? nullptr : DaAi::Tactics::flank_waypoint(npc);
        if (!enemy || !point)
        {
            // The order ran out: it stops where it is and the planner chooses again.
            npc.movement().set_nearest_accessible_position();
            npc.movement().set_movement_type(eMovementTypeStand);
            if (enemy)
                aim_ready();
            return;
        }

        setup_cover(*point);
        if (npc.movement().path_completed() ||
            point->position().distance_to_sqr(npc.Position()) < _sqr(DaAi::Tactics::config().at_cover_m))
        {
            if (DaAi::Tactics::flank_reached(npc, *point))
            {
                // The end of the route: the order is done, it fights from this cover.
                m_storage->set_property(eWorldPropertyInCover, true);
                m_storage->set_property(eWorldPropertyLookedOut, false);
                m_storage->set_property(eWorldPropertyPositionHolded, false);
                m_storage->set_property(eWorldPropertyEnemyDetoured, false);
                npc.movement().set_movement_type(eMovementTypeStand);
                npc.brain().affect_cover(true);
                aim_ready();
                return;
            }
        }

        if (DaAi::Know::sees_now(npc, enemy))
        {
            if (DaAi::Tactics::flank_fire(npc))
            {
                npc.sight().setup(CSightAction(enemy, true, true));
                fire();
                return;
            }
            // Holding fire to the cover is the order, not a slow reaction.
            DaAi::Tactics::hold_fire(npc);
        }
        aim_ready();
        npc.sight().setup(CSightAction(SightManager::eSightTypePathDirection, true, true));
    }

    void finalize() override
    {
        inherited::finalize();
        if (object().g_Alive())
            object().agent_manager().member().member(&object()).detour(false);
    }
};
} // namespace

namespace DaAi::Tactics
{
CPropertyEvaluator<CScriptGameObject>* make_too_far_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorTooFar>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_should_throw_grenade_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorShouldThrowGrenade>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_out_of_band_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorOutOfBand>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_suppress_role_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorSuppressRole>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_should_fall_back_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorShouldFallBack>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_return_fire_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorReturnFire>(object, name);
}

CPropertyEvaluator<CScriptGameObject>* make_flank_role_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorFlankRole>(object, name);
}

CActionBase<CScriptGameObject>* make_suppress_fire_action(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiActionSuppressFire>(object, name);
}

CActionBase<CScriptGameObject>* make_fall_back_action(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiActionFallBack>(object, name);
}

CActionBase<CScriptGameObject>* make_hold_ambush_action(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiActionHoldAmbush>(object, name);
}

CActionBase<CScriptGameObject>* make_return_fire_action(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiActionReturnFire>(object, name);
}

CActionBase<CScriptGameObject>* make_flank_action(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiActionFlank>(object, name);
}
} // namespace DaAi::Tactics
