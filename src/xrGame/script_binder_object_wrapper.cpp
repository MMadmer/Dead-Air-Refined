////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder_object_wrapper.cpp
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script object binder wrapper
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_binder_object_wrapper.h"
#include "script_game_object.h"
#include "xrServer_Objects_ALife.h"

#include <luabind/function.hpp>

#ifndef LUAJIT_DISABLE_GC64
#define LUAJIT_DISABLE_GC64
#endif
#include <lj_bc.h>
#include <lj_obj.h>

namespace
{
bool is_empty_lua_function(lua_State* luaState)
{
    const TValue* value = luaState->top - 1;
    if (!tvisfunc(value))
        return false;

    const GCfunc* function = funcV(value);
    if (!isluafunc(function))
        return false;

    const GCproto* prototype = funcproto(function);
    return prototype->sizebc == 2 && bc_op(proto_bc(prototype)[1]) == BC_RET0;
}
}

CScriptBinderObjectWrapper::CScriptBinderObjectWrapper(CScriptGameObject* object)
    : CScriptBinderObject(object), m_netRelcaseCheckFrame(u32(-1)), m_hasNetRelcaseOverride(false)
{
}
CScriptBinderObjectWrapper::~CScriptBinderObjectWrapper() {}
void CScriptBinderObjectWrapper::reinit() { luabind::call_member<void>(this, "reinit"); }
void CScriptBinderObjectWrapper::reinit_static(CScriptBinderObject* script_binder_object)
{
    script_binder_object->CScriptBinderObject::reinit();
}

void CScriptBinderObjectWrapper::reload(LPCSTR section) { luabind::call_member<void>(this, "reload", section); }
void CScriptBinderObjectWrapper::reload_static(CScriptBinderObject* script_binder_object, LPCSTR section)
{
    script_binder_object->CScriptBinderObject::reload(section);
}

bool CScriptBinderObjectWrapper::net_Spawn(SpawnType DC) { return (luabind::call_member<bool>(this, "net_spawn", DC)); }
bool CScriptBinderObjectWrapper::net_Spawn_static(CScriptBinderObject* script_binder_object, SpawnType DC)
{
    return (script_binder_object->CScriptBinderObject::net_Spawn(DC));
}

void CScriptBinderObjectWrapper::net_Destroy() { luabind::call_member<void>(this, "net_destroy"); }
void CScriptBinderObjectWrapper::net_Destroy_static(CScriptBinderObject* script_binder_object)
{
    script_binder_object->CScriptBinderObject::net_Destroy();
}

void CScriptBinderObjectWrapper::net_Import(NET_Packet* net_packet)
{
    luabind::call_member<void>(this, "net_import", net_packet);
}

void CScriptBinderObjectWrapper::net_Import_static(CScriptBinderObject* script_binder_object, NET_Packet* net_packet)
{
    script_binder_object->CScriptBinderObject::net_Import(net_packet);
}

void CScriptBinderObjectWrapper::net_Export(NET_Packet* net_packet)
{
    luabind::call_member<void>(this, "net_export", net_packet);
}

void CScriptBinderObjectWrapper::net_Export_static(CScriptBinderObject* script_binder_object, NET_Packet* net_packet)
{
    script_binder_object->CScriptBinderObject::net_Export(net_packet);
}

void CScriptBinderObjectWrapper::shedule_Update(u32 time_delta)
{
    luabind::call_member<void>(this, "update", time_delta);
}

void CScriptBinderObjectWrapper::shedule_Update_static(CScriptBinderObject* script_binder_object, u32 time_delta)
{
    script_binder_object->CScriptBinderObject::shedule_Update(time_delta);
}

void CScriptBinderObjectWrapper::save(NET_Packet* output_packet)
{
    luabind::call_member<void>(this, "save", output_packet);
}

void CScriptBinderObjectWrapper::save_static(CScriptBinderObject* script_binder_object, NET_Packet* output_packet)
{
    script_binder_object->CScriptBinderObject::save(output_packet);
}

void CScriptBinderObjectWrapper::load(IReader* input_packet) { luabind::call_member<void>(this, "load", input_packet); }
void CScriptBinderObjectWrapper::load_static(CScriptBinderObject* script_binder_object, IReader* input_packet)
{
    script_binder_object->CScriptBinderObject::load(input_packet);
}

bool CScriptBinderObjectWrapper::net_SaveRelevant() { return (luabind::call_member<bool>(this, "net_save_relevant")); }
bool CScriptBinderObjectWrapper::net_SaveRelevant_static(CScriptBinderObject* script_binder_object)
{
    return (script_binder_object->CScriptBinderObject::net_SaveRelevant());
}

void CScriptBinderObjectWrapper::net_Relcase(CScriptGameObject* object)
{
    if (m_netRelcaseCheckFrame != Device.dwFrame)
    {
        // Match luabind's virtual dispatch selection without invoking empty Lua or native fallbacks.
        const auto& self = luabind::detail::wrap_access::ref(*this);
        lua_State* luaState = self.state();
        self.get(luaState);
        lua_pushliteral(luaState, "net_Relcase");
        lua_gettable(luaState, -2);
        m_hasNetRelcaseOverride =
            !luabind::detail::is_luabind_function(luaState, -1) && !is_empty_lua_function(luaState);
        lua_pop(luaState, 2);
        m_netRelcaseCheckFrame = Device.dwFrame;
    }

    if (!m_hasNetRelcaseOverride)
        return;

    luabind::call_member<void>(this, "net_Relcase", object);
}

void CScriptBinderObjectWrapper::net_Relcase_static(
    CScriptBinderObject* script_binder_object, CScriptGameObject* object)
{
    script_binder_object->CScriptBinderObject::net_Relcase(object);
}
