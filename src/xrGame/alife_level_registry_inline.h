////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_level_registry_inline.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife level registry inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ai_space.h"

IC CALifeLevelRegistry::CALifeLevelRegistry(const GameGraph::_LEVEL_ID& level_id) { m_level_id = level_id; }
IC GameGraph::_LEVEL_ID CALifeLevelRegistry::level_id() const { return (m_level_id); }
IC void CALifeLevelRegistry::add(CSE_ALifeDynamicObject* object)
{
    // an invalid graph vertex from a save meant an OOB read right below; the object just
    // stays off this level's registry
    if (!ai().game_graph().valid_vertex_id(object->m_tGraphID))
    {
        Msg("! Level registry: object [%s] section[%s] id[%d] carries graph vertex %u of %u total - not registered",
            object->name_replace(), object->s_name.c_str(), object->ID, u32(object->m_tGraphID),
            u32(ai().game_graph().header().vertex_count()));
        return;
    }

    if (ai().game_graph().vertex(object->m_tGraphID)->level_id() != level_id())
        return;

    // a duplicate id would THROW2 -> std::terminate inside inherited::add; tolerate the
    // re-add at the single entry point instead
    if (objects().find(object->ID) != objects().end())
    {
        Msg("! Level registry: object [%s] section[%s] id[%d] already registered - repeat skipped",
            object->name_replace(), object->s_name.c_str(), object->ID);
        return;
    }

#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("[LSS] adding object [%s][%d] to current level", object->name_replace(), object->ID);
    }
#endif
    inherited::add(object->ID, object);
}

IC void CALifeLevelRegistry::remove(CSE_ALifeDynamicObject* object, bool no_assert)
{
#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("[LSS] removing object [%s][%d] from current level", object->name_replace(), object->ID);
    }
#endif
    inherited::remove(object->ID, no_assert);
}

template <typename _update_predicate>
IC void CALifeLevelRegistry::update(const _update_predicate& predicate, bool const iterate_as_first_time_next_time)
{
    //	u32					object_count =
    inherited::update(predicate, iterate_as_first_time_next_time);
#ifdef FULL_LEVEL_UPDATE
    m_first_update = true;
#endif
#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        //		Msg				("[LSS][OOS][%d : %d]",object_count, objects().size());
    }
#endif
}

IC CSE_ALifeDynamicObject* CALifeLevelRegistry::object(const ALife::_OBJECT_ID& id, bool no_assert) const
{
    _REGISTRY::const_iterator I = objects().find(id);
    if (I == objects().end())
    {
        THROW2(no_assert, "The spesified object hasn't been found in the current level!");
        return (0);
    }
    return ((*I).second);
}
