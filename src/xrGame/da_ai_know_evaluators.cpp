// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_knowledge.h"

#include "ai/stalker/ai_stalker.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "stalker_property_evaluators.h"

namespace
{
// Replaces the stock EnemySeeMe evaluator (W4): the stock one read the enemy's own vision; this one is the NPC's
// estimate from what it perceives. Holds the stock evaluator and answers with it while knowledge does not decide.
class CDaAiEvaluatorEnemySeeMe final : public CStalkerPropertyEvaluator
{
    using inherited = CStalkerPropertyEvaluator;

    CStalkerPropertyEvaluatorEnemySeeMe m_stock;

public:
    CDaAiEvaluatorEnemySeeMe(CAI_Stalker* object, pcstr name)
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

    _value_type evaluate() override
    {
        if (!m_object || !DaAi::Know::active(*m_object))
            return m_stock.evaluate();
        const CEntityAlive* enemy = m_object->memory().enemy().selected();
        return enemy && DaAi::Know::enemy_sees_me(*m_object, *enemy);
    }
};
} // namespace

namespace DaAi::Know
{
CPropertyEvaluator<CScriptGameObject>* make_enemy_see_me_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorEnemySeeMe>(object, name);
}
} // namespace DaAi::Know
