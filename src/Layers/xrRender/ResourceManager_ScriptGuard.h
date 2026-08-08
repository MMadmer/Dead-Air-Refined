// ResourceManager_ScriptGuard.h: containment boundary for shader-script (Lua) invocations.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include <exception>

#include "xrCommon/xr_hash_map.h"
#include "xrCore/log.h"
#include "xrCore/xrstring.h"
#include "xrScriptEngine/script_space.hpp"

namespace xray::render::RENDER_NAMESPACE
{
// luabind leaves the error message on the Lua stack when a shader script fails, and the very same
// state keeps serving every remaining shader of the level. Restore the top on all paths.
class shader_script_stack_guard
{
    lua_State* const state;
    const int top;

public:
    explicit shader_script_stack_guard(lua_State* luaState) : state(luaState), top(luaState ? lua_gettop(luaState) : 0)
    {
    }
    shader_script_stack_guard(const shader_script_stack_guard&) = delete;
    shader_script_stack_guard& operator=(const shader_script_stack_guard&) = delete;
    ~shader_script_stack_guard()
    {
        if (state)
            lua_settop(state, top);
    }
};

// A broken script is re-entered once per material instance, so report each shader/element pair once.
// Runs with ScriptEngineLock held, which is what keeps this registry single-threaded.
inline bool shader_script_first_failure(pcstr namesp, pcstr name)
{
    static xr_flat_hash_map<shared_str, u32> failures;

    shared_str key;
    xr_sprintf(key, "%s.%s", namesp, name);
    return 1 == ++failures[key];
}

// Narrow exception boundary around one shader-script element. Returns false when the script failed,
// in which case the caller must drop the partially recorded element instead of publishing it.
template <typename Invoker>
bool run_shader_script(lua_State* state, pcstr namesp, pcstr name, int element, Invoker&& invoker)
{
    const shader_script_stack_guard guard(state);
    try
    {
        invoker();
        return true;
    }
    // luabind::error derives from std::exception and already carries the Lua message. A dedicated
    // catch is impossible anyway: LUABIND_NO_EXCEPTIONS removes the type in ReleaseMasterGold.
    catch (const std::exception& failure)
    {
        if (shader_script_first_failure(namesp, name))
            Msg("! shader script [%s.%s], element [%d]: %s", namesp, name, element, failure.what());
    }
    catch (...)
    {
        if (shader_script_first_failure(namesp, name))
            Msg("! shader script [%s.%s], element [%d]: unknown exception", namesp, name, element);
    }
    return false;
}
} // namespace xray::render::RENDER_NAMESPACE
