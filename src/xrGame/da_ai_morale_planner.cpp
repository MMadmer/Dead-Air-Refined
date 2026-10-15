// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_morale.h"
#include "da_ai_npc.h"

#include "ai/stalker/ai_stalker.h"
#include "stalker_animation_manager.h"
#include "stalker_property_evaluators.h"

namespace
{
// Replaces the stock Panic evaluator: true while the NPC's morale is broken, which sends the planner into
// retreat_from_enemy, where the profile's retreat style runs. With the morale feature off, the stock one answers.
class CDaAiEvaluatorPanic final : public CStalkerPropertyEvaluator
{
    using inherited = CStalkerPropertyEvaluator;

    CStalkerPropertyEvaluatorPanic m_stock;

public:
    CDaAiEvaluatorPanic(CAI_Stalker* object, pcstr name)
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
        if (!DaAi::feature(DaAi::Feature::morale) || !DaAi::npc(object()))
            return m_stock.evaluate();
        if (object().animation().global_selector())
            return false;
        return DaAi::Morale::panicking(object());
    }
};
} // namespace

namespace DaAi::Morale
{
CPropertyEvaluator<CScriptGameObject>* make_panic_evaluator(CAI_Stalker* object, pcstr name)
{
    return xr_new<CDaAiEvaluatorPanic>(object, name);
}
} // namespace DaAi::Morale
