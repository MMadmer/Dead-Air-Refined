// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch_script.h"
#include "da_ai.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"
#include "da_ai_knowledge.h"
#include "da_ai_senses.h"
#include "da_ai_comms.h"
#include "da_ai_tactics.h"
#include "da_ai_morale.h"
#include "da_ai_search.h"

#include "ai/stalker/ai_stalker.h"
#include "danger_manager.h"
#include "inventory_item.h"
#include "memory_manager.h"
#include "script_game_object.h"
#include "xrEngine/IGame_Level.h"
#include "xrScriptEngine/script_engine.hpp"

// Lua API of the human NPC AI: game object methods (one binding per name, so a loose call never falls through an
// ambiguous overload set) and globals. Every method answers the neutral value for an object that is not a stalker.
namespace
{
using namespace DaAi;

CAI_Stalker* stalker_of(CScriptGameObject* self) { return self ? smart_cast<CAI_Stalker*>(&self->object()) : nullptr; }
const CGameObject* object_of(CScriptGameObject* object) { return object ? &object->object() : nullptr; }

const Belief* belief_of(CScriptGameObject* self, CScriptGameObject* object)
{
    const CAI_Stalker* npc = stalker_of(self);
    return npc && object ? Know::belief(*npc, object->ID()) : nullptr;
}

float ai_awareness(CScriptGameObject* self, CScriptGameObject* object)
{
    const CAI_Stalker* npc = stalker_of(self);
    const CGameObject* target = object_of(object);
    return npc && target ? Sense::awareness(*npc, target) : 0.f;
}

int ai_belief_source(CScriptGameObject* self, CScriptGameObject* object)
{
    const Belief* belief = belief_of(self, object);
    return belief ? int(belief->e.source) : int(Source::none);
}

float ai_belief_error(CScriptGameObject* self, CScriptGameObject* object)
{
    const Belief* belief = belief_of(self, object);
    return belief ? belief->e.err : -1.f;
}

int ai_belief_age(CScriptGameObject* self, CScriptGameObject* object)
{
    const Belief* belief = belief_of(self, object);
    return belief ? int(Device.dwTimeGlobal - belief->e.t_observed) : -1;
}

Fvector ai_belief_position(CScriptGameObject* self, CScriptGameObject* object)
{
    const Belief* belief = belief_of(self, object);
    return belief ? belief->e.pos : Fvector().set(0.f, 0.f, 0.f);
}

float ai_known_distance(CScriptGameObject* self, CScriptGameObject* object)
{
    const CAI_Stalker* npc = stalker_of(self);
    const CGameObject* target = object_of(object);
    return npc && target ? Know::known_distance(*npc, *target) : -1.f;
}

bool ai_enemy_sees_me(CScriptGameObject* self, CScriptGameObject* enemy)
{
    const CAI_Stalker* npc = stalker_of(self);
    const CEntityAlive* entity = enemy ? smart_cast<const CEntityAlive*>(&enemy->object()) : nullptr;
    return npc && entity && Know::enemy_sees_me(*npc, *entity);
}

void ai_inform(CScriptGameObject* self, CScriptGameObject* object, const Fvector& position, float err, int source)
{
    CAI_Stalker* npc = stalker_of(self);
    if (!npc || source <= int(Source::none) || source > int(Source::restored))
        return;
    Know::inform(*npc, object_of(object), position, err, Source(source));
}

bool ai_has_radio(CScriptGameObject* self)
{
    const CAI_Stalker* npc = stalker_of(self);
    return npc && Comms::has_radio(*npc);
}

bool ai_is_leader(CScriptGameObject* self)
{
    const CAI_Stalker* npc = stalker_of(self);
    return npc && is_leader(*npc);
}

int ai_morale(CScriptGameObject* self)
{
    const CAI_Stalker* npc = stalker_of(self);
    return int(npc ? Morale::level(*npc) : Morale::Level::steady);
}

float ai_fear(CScriptGameObject* self)
{
    const CAI_Stalker* npc = stalker_of(self);
    return npc ? Morale::fear(*npc) : 0.f;
}

int ai_role(CScriptGameObject* self)
{
    const CAI_Stalker* npc = stalker_of(self);
    return int(npc ? Tactics::role(*npc) : Role::opportunist);
}

float ai_suppression(CScriptGameObject* self)
{
    const CAI_Stalker* npc = stalker_of(self);
    return npc ? Sense::suppression(*npc) : 0.f;
}

bool ai_searching(CScriptGameObject* self)
{
    const CAI_Stalker* npc = stalker_of(self);
    return npc && Search::active(*npc);
}

pcstr ai_profile(CScriptGameObject* self, pcstr key)
{
    const CAI_Stalker* npc = stalker_of(self);
    return npc ? profile_value(*npc, key) : "";
}

// The type of the nearest danger the NPC holds within radius of the spot, perceived at or after since (time_global),
// else -1: a probe reads the suspicion a heard noise raised even while other dangers rank higher.
int ai_danger_near(CScriptGameObject* self, const Fvector& position, float radius, u32 since)
{
    const CAI_Stalker* npc = stalker_of(self);
    if (!npc)
        return -1;
    int result = -1;
    float best = _sqr(radius);
    for (const CDangerObject& danger : npc->memory().danger().objects())
    {
        const float distance = danger.position().distance_to_sqr(position);
        if (danger.time() >= since && distance <= best)
        {
            best = distance;
            result = int(danger.type());
        }
    }
    return result;
}

pcstr ai_weapon_class(CScriptGameObject* self, CScriptGameObject* item)
{
    const CInventoryItem* inventory_item = item ? smart_cast<const CInventoryItem*>(&item->object()) : nullptr;
    return inventory_item ? weapon_class_name(weapon_class(*inventory_item)) : "";
}

void level_ai_noise(const Fvector& position, float loudness, pcstr kind, CScriptGameObject* who)
{
    Noise noise;
    if (!Sense::noise_by_name(kind, noise))
    {
        GEnv.ScriptEngine->script_log(LuaMessageType::Error, "level.ai_noise: unknown kind '%s'", kind ? kind : "nil");
        return;
    }
    Sense::emit_noise(position, loudness, noise, object_of(who));
}

float level_ai_line_vis(const Fvector& from, const Fvector& to)
{
    return g_pGameLevel ? Sense::line_vis(from, to) : 0.f;
}

float level_ai_darkness() { return Sense::darkness(); }

float ai_stat(pcstr key)
{
    Stat stat;
    return stat_by_name(key, stat) ? stat_value(stat) : 0.f;
}

bool ai_feature_enabled(pcstr name)
{
    if (name && !xr_strcmp(name, "all"))
        return g_features == (1u << u32(Feature::count)) - 1;
    Feature which;
    return feature_by_name(name, which) && feature(which);
}
} // namespace

luabind::class_<CScriptGameObject>& script_register_game_object_da_ai(luabind::class_<CScriptGameObject>& instance)
{
    instance
        .def("ai_awareness", &ai_awareness)
        .def("ai_belief_source", &ai_belief_source)
        .def("ai_belief_error", &ai_belief_error)
        .def("ai_belief_age", &ai_belief_age)
        .def("ai_belief_position", &ai_belief_position)
        .def("ai_known_distance", &ai_known_distance)
        .def("ai_enemy_sees_me", &ai_enemy_sees_me)
        .def("ai_inform", &ai_inform)
        .def("ai_has_radio", &ai_has_radio)
        .def("ai_is_leader", &ai_is_leader)
        .def("ai_morale", &ai_morale)
        .def("ai_fear", &ai_fear)
        .def("ai_role", &ai_role)
        .def("ai_suppression", &ai_suppression)
        .def("ai_searching", &ai_searching)
        .def("ai_profile", &ai_profile)
        .def("ai_weapon_class", &ai_weapon_class)
        .def("ai_danger_near", &ai_danger_near);
    return instance;
}

struct DaAiScriptExport
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void DaAiScriptExport::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState, "level")
    [
        def("ai_noise", &level_ai_noise),
        def("ai_line_vis", &level_ai_line_vis),
        def("ai_darkness", &level_ai_darkness)
    ];

    module(luaState)
    [
        def("ai_stat", &ai_stat),
        def("ai_feature_enabled", &ai_feature_enabled)
    ];
}
