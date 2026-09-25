#pragma once

// Protected mode for scripts (docs/dead-air/MOD_CATALOG.md, section 9): what a Lua state may do
// while a module from the mod catalog is enabled. XMS decides whether it is on
// (XMS::Catalog::ProtectedMode); this applies it to one freshly opened state. The engine's own
// Lua bindings that touch files (getFS(), ini_file, the console) ask XMS::Protect themselves.

#include "xrScriptEngine/xrScriptEngine.hpp"

struct lua_State;

namespace ScriptProtect
{
// Called after the standard libraries, marshal, lfs and jit are open and before any script
// runs: removes what reaches native code or programs, wraps what touches files. The engine does
// not open ffi at all in protected mode; this does not undo it if it did.
void Apply(lua_State* L);

// The script and line of the innermost Lua frame, for the log of a refusal.
XRSCRIPTENGINE_API xr_string Location(lua_State* L);

// For a binding: logs the refusal with the calling script's position.
XRSCRIPTENGINE_API void Refused(lua_State* L, pcstr what, pcstr why);
}
