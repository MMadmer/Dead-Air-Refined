////////////////////////////////////////////////////////////////////////////
//	Module 		: xrServer_Objects_ALife_Monsters_script3.cpp
//	Created 	: 19.09.2002
//  Modified 	: 04.06.2003
//	Author		: Dmitriy Iassenev
//	Description : Server monsters for ALife simulator, script export, the second part
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_script_macroses.h"

void CSE_ALifeCreatureActor::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        luabind_class_creature3(CSE_ALifeCreatureActor, "cse_alife_creature_actor", CSE_ALifeCreatureAbstract,
                                CSE_ALifeTraderAbstract, CSE_PHSkeleton)
    ];
}

void CSE_ALifeTorridZone::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        luabind_class_dynamic_alife2(CSE_ALifeTorridZone, "cse_torrid_zone", CSE_ALifeCustomZone, CSE_Motion)
    ];
}

void CSE_ALifeZoneVisual::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        luabind_class_dynamic_alife2(CSE_ALifeZoneVisual, "cse_zone_visual", CSE_ALifeAnomalousZone, CSE_Visual)
    ];
}

void CSE_ALifeCreaturePhantom::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        luabind_class_creature1(CSE_ALifeCreaturePhantom, "cse_alife_creature_phantom", CSE_ALifeCreatureAbstract)
    ];
}

void CSE_ALifeCreatureAbstract::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        luabind_class_creature1(CSE_ALifeCreatureAbstract, "cse_alife_creature_abstract", CSE_ALifeDynamicObjectVisual)
            .def("health", &CSE_ALifeCreatureAbstract::get_health)
            .def("alive", &CSE_ALifeCreatureAbstract::g_Alive)
            .def_readwrite("team", &CSE_ALifeCreatureAbstract::s_team)
            .def_readwrite("squad", &CSE_ALifeCreatureAbstract::s_squad)
            .def_readwrite("group", &CSE_ALifeCreatureAbstract::s_group)
            .def("o_torso", +[](CSE_ALifeCreatureAbstract* self) { return &self->o_torso; })
    ];
}

namespace
{
// The squad accessors read the member vector straight off the pointer, and the ALife scripts keep
// squad handles across a level change - the simulation board still holds a squad the server has
// already released. On such a pointer begin() is whatever the allocator left behind, which is the
// reported access violation a few seconds after a transition. Refuse the call and answer an empty
// squad instead.
CSE_ALifeOnlineOfflineGroup::MEMBERS& empty_members()
{
    static CSE_ALifeOnlineOfflineGroup::MEMBERS nobody;
    return nobody;
}

ALife::_OBJECT_ID group_commander_id(CSE_ALifeOnlineOfflineGroup* group)
{
    return script_object_usable(group, "commander_id") ? group->commander_id() : ALife::_OBJECT_ID(-1);
}

CSE_ALifeOnlineOfflineGroup::MEMBERS const& group_squad_members(CSE_ALifeOnlineOfflineGroup* group)
{
    return script_object_usable(group, "squad_members") ? group->squad_members() : empty_members();
}

u32 group_npc_count(CSE_ALifeOnlineOfflineGroup* group)
{
    return script_object_usable(group, "npc_count") ? group->npc_count() : 0;
}

void group_register_member(CSE_ALifeOnlineOfflineGroup* group, ALife::_OBJECT_ID member_id)
{
    if (script_object_usable(group, "register_member"))
        group->register_member(member_id);
}

void group_unregister_member(CSE_ALifeOnlineOfflineGroup* group, ALife::_OBJECT_ID member_id)
{
    if (script_object_usable(group, "unregister_member"))
        group->unregister_member(member_id);
}

void group_clear_location_types(CSE_ALifeOnlineOfflineGroup* group)
{
    if (script_object_usable(group, "clear_location_types"))
        group->clear_location_types();
}

void group_add_location_type(CSE_ALifeOnlineOfflineGroup* group, pcstr mask)
{
    if (script_object_usable(group, "add_location_type"))
        group->add_location_type(mask);
}

void group_force_change_position(CSE_ALifeOnlineOfflineGroup* group, Fvector position)
{
    if (script_object_usable(group, "force_change_position"))
        group->force_change_position(position);
}
} // namespace

void CSE_ALifeOnlineOfflineGroup::script_register(lua_State* luaState)
{
    using namespace luabind;
    using namespace luabind::policy;

    module(luaState)
    [
        class_<MEMBERS::value_type>("MEMBERS__value_type")
            .def_readonly("id", &MEMBERS::value_type::first)
            .def_readonly("object", &MEMBERS::value_type::second),

        luabind_class_online_offline_group2(CSE_ALifeOnlineOfflineGroup, "cse_alife_online_offline_group",
                                            CSE_ALifeDynamicObject, CSE_ALifeSchedulable)
#ifdef XRGAME_EXPORTS
            .def("register_member", &group_register_member)
            .def("unregister_member", &group_unregister_member)
            .def("commander_id", &group_commander_id)
            .def("squad_members", &group_squad_members, return_stl_iterator())
            .def("npc_count", &group_npc_count)
            .def("add_location_type", &group_add_location_type)
            .def("clear_location_types", &group_clear_location_types)
            .def("force_change_position", &group_force_change_position)
            //.def("force_change_game_vertex_id", &CSE_ALifeOnlineOfflineGroup::force_change_game_vertex_id)
#endif
    ];
}
