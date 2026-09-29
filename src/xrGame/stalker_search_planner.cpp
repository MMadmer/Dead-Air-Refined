////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_search_planner.cpp
//	Created 	: 31.05.2005
//  Modified 	: 31.05.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker search planner class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_search_planner.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "stalker_decision_space.h"
#include "script_game_object.h"
#include "script_game_object_impl.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "stalker_property_evaluators.h"
#include "stalker_search_actions.h"
#include "da_ai_ids.h"
#include "da_ai_search.h"

using namespace StalkerDecisionSpace;

CStalkerSearchPlanner::CStalkerSearchPlanner(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name)
{
}

void CStalkerSearchPlanner::setup(CAI_Stalker* object, CPropertyStorage* storage)
{
    inherited::setup(object, storage);
    clear();
    add_evaluators();
    add_actions();
}

void CStalkerSearchPlanner::initialize()
{
    inherited::initialize();

    object().agent_manager().member().member(&object()).cover(0);

    CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyLocationReached, false);
    CScriptActionPlanner::m_storage.set_property(eWorldPropertyAmbushLocationReached, false);

    DaAi::Search::begin(object());
    // search_style none: no walk to the enemy's last position, the NPC waits where it is
    if (DaAi::Search::holds_in_place(object()))
    {
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyLocationReached, true);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyAmbushLocationReached, true);
    }
}

void CStalkerSearchPlanner::update() { inherited::update(); }
void CStalkerSearchPlanner::finalize() { inherited::finalize(); }
void CStalkerSearchPlanner::add_evaluators()
{
    add_evaluator(eWorldPropertyPureEnemy, xr_new<CStalkerPropertyEvaluatorConst>(true, "is_there_enemies_delayed"));
    add_evaluator(eWorldPropertyEnemyLocationReached,
        xr_new<CStalkerPropertyEvaluatorMember>(
            (CPropertyStorage*)0, eWorldPropertyEnemyLocationReached, true, true, "enemy location reached"));
    add_evaluator(eWorldPropertyAmbushLocationReached,
        xr_new<CStalkerPropertyEvaluatorMember>(
            (CPropertyStorage*)0, eWorldPropertyAmbushLocationReached, true, true, "ambush location reached"));
    // Plain evaluators: they save nothing, the planner's save stream stays as it was.
    add_evaluator(DaAi::Ids::prop_search_spot, DaAi::Search::make_search_spot_evaluator(m_object, "da search spot"));
    add_evaluator(
        DaAi::Ids::prop_search_cover, DaAi::Search::make_search_cover_evaluator(m_object, "da search cover"));
}

void CStalkerSearchPlanner::add_actions()
{
    CStalkerActionBase* action;

    // The stock chain (enemy location, ambush, hold) runs once no spot is left to sweep or to cover.
    action = xr_new<CStalkerActionReachEnemyLocation>(
        m_object, CActionBase<CScriptGameObject>::m_storage, "reach enemy location");
    add_condition(action, eWorldPropertyEnemyLocationReached, false);
    add_condition(action, DaAi::Ids::prop_search_spot, false);
    add_condition(action, DaAi::Ids::prop_search_cover, false);
    add_effect(action, eWorldPropertyEnemyLocationReached, true);
    add_operator(eWorldOperatorReachEnemyLocation, action);

    action = xr_new<CStalkerActionReachAmbushLocation>(
        m_object, CActionBase<CScriptGameObject>::m_storage, "reach ambush location");
    add_condition(action, eWorldPropertyEnemyLocationReached, true);
    add_condition(action, eWorldPropertyAmbushLocationReached, false);
    add_condition(action, DaAi::Ids::prop_search_spot, false);
    add_condition(action, DaAi::Ids::prop_search_cover, false);
    add_effect(action, eWorldPropertyAmbushLocationReached, true);
    add_operator(eWorldOperatorReachAmbushLocation, action);

    action = xr_new<CStalkerActionHoldAmbushLocation>(
        m_object, CActionBase<CScriptGameObject>::m_storage, "hold ambush location");
    add_condition(action, eWorldPropertyAmbushLocationReached, true);
    add_condition(action, DaAi::Ids::prop_search_spot, false);
    add_condition(action, DaAi::Ids::prop_search_cover, false);
    add_effect(action, eWorldPropertyPureEnemy, false);
    add_operator(eWorldOperatorHoldAmbushLocation, action);
    action->set_inertia_time(15000);

    // Plain operators of the search lane: sweep an assigned spot, or cover the partner who sweeps it.
    CActionBase<CScriptGameObject>* operation = DaAi::Search::make_search_sweep_action(
        m_object, CActionBase<CScriptGameObject>::m_storage, "da search sweep");
    add_condition(operation, DaAi::Ids::prop_search_spot, true);
    add_condition(operation, DaAi::Ids::prop_search_cover, false);
    add_effect(operation, DaAi::Ids::prop_search_spot, false);
    add_operator(DaAi::Ids::op_search_sweep, operation);

    operation = DaAi::Search::make_search_overwatch_action(
        m_object, CActionBase<CScriptGameObject>::m_storage, "da search overwatch");
    add_condition(operation, DaAi::Ids::prop_search_cover, true);
    add_effect(operation, DaAi::Ids::prop_search_cover, false);
    add_operator(DaAi::Ids::op_search_overwatch, operation);
}
