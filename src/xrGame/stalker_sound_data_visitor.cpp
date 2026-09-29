////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_sound_data_visitor.cpp
//	Created 	: 02.02.2005
//  Modified 	: 02.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker sound data visitor
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "stalker_sound_data_visitor.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_sound_data.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "memory_manager.h"
#include "hit_memory_manager.h"
#include "visual_memory_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "da_ai_comms.h"

CStalkerSoundDataVisitor::~CStalkerSoundDataVisitor() {}

// With comms on, a heard voice is decoded by the hearer (Comms::on_heard_voice) after the hearing threshold and
// never hands over the speaker's enemy or danger. With comms off, the stock transfer runs once per sound event:
// in the early call of the sound memory manager while that call exists, after the threshold once it is gone.
void CStalkerSoundDataVisitor::visit(CStalkerSoundData* data)
{
    const bool comms = DaAi::feature(DaAi::Feature::comms) && object().da_ai();
    if (!m_position)
    {
        m_stock_data = nullptr;
        if (comms)
            return;
        m_stock_data = data;
        stock_visit(data);
        return;
    }

    if (comms)
        DaAi::Comms::on_heard_voice(object(), *data, *m_position, m_power, m_masking);
    else if (m_stock_data != data)
        stock_visit(data);
    m_stock_data = nullptr;
}

void CStalkerSoundDataVisitor::heard(CSound_UserData& data, const Fvector& position, float power, float masking)
{
    m_position = &position;
    m_power = power;
    m_masking = masking;
    data.accept(this);
    m_position = nullptr;
}

void CStalkerSoundDataVisitor::stock_visit(CStalkerSoundData* data)
{
    if (object().memory().enemy().selected())
        return;

    if (object().is_relation_enemy(&data->object()))
        return;

    if (!data->object().memory().enemy().selected())
    {
        if (!object().memory().danger().selected() && data->object().memory().danger().selected())
            object().memory().danger().add(*data->object().memory().danger().selected());
        return;
    }

    if (data->object().memory().enemy().selected()->getDestroy())
        return;

    if (!object().is_relation_enemy(data->object().memory().enemy().selected()))
        return;

    if (!data->object().g_Alive())
        return;

    if (!object().g_Alive())
        return;
#ifdef DEBUG
    Msg("%s : Adding fiction hit by sound info from stalker %s", object().cName().c_str(), data->object().cName().c_str());
#endif //#ifdef DEBUG
    object().memory().make_object_visible_somewhen(data->object().memory().enemy().selected());

    //	const MemorySpace::CHitObject	*m =
    // data->object().memory().hit().hit(data->object().memory().enemy().selected());
    //	if (!m)
    //		return;
    //	object().memory().hit().add		(*m);
}
