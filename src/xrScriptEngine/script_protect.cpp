// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "pch.hpp"

#include "script_protect.hpp"
#include "xrCore/XMS/xms_catalog.h"

#include <algorithm>
#include <string_view>

// A Lua error unwinds as an exception that the C++ frames it passes were compiled not to expect
// (/EHsc: a C function never throws), and a frame holding an object with a destructor ends the
// process when one goes through it. So every guard below decides, logs and formats in a function
// of its own, whose objects are gone before the guard raises an error or hands over to a library
// function that may raise one; the guards themselves hold nothing but plain buffers. Those helpers
// are never inlined: inlined, their objects would be the guard's again.
#define PROTECT_HELPER __declspec(noinline)

namespace
{
using XMS::Protect::Access;

// The library functions a guard hands over to once a call is allowed. They are C functions of
// LuaJIT, lfs and marshal: the same addresses in every state, and kept here rather than in the
// state, where debug.getupvalue could have fished them out.
lua_CFunction g_io_open{}, g_io_lines{}, g_io_input{}, g_io_output{};
lua_CFunction g_os_remove{}, g_os_rename{};
lua_CFunction g_load{}, g_loadstring{}, g_loadfile{};
lua_CFunction g_lfs_attributes{}, g_lfs_symlinkattributes{}, g_lfs_dir{}, g_lfs_mkdir{}, g_lfs_rmdir{},
    g_lfs_touch{}, g_lfs_lock_dir{};
lua_CFunction g_mar_encode{}, g_mar_encode_begin{}, g_mar_decode{}, g_mar_clone{};

using Message = string512;

PROTECT_HELPER void log_refusal(lua_State* L, pcstr what, pcstr detail, pcstr why)
{
    string512 subject{};
    if (detail && *detail)
        xr_sprintf(subject, "%s(\"%s\")", what, detail);
    else
        xr_strcpy(subject, what);
    XMS::Protect::Refused(subject, why, ScriptProtect::Location(L).c_str());
}

// Logs the refusal and writes the message the script gets.
PROTECT_HELPER void refusal(lua_State* L, pcstr what, pcstr detail, pcstr why, Message& message)
{
    log_refusal(L, what, detail, why);
    xr_sprintf(message, "%s: refused by protected mode: %s", detail && *detail ? detail : what, why);
}

// True, with the message, when protected mode refuses `access` to `path`.
PROTECT_HELPER bool denied(lua_State* L, pcstr what, pcstr path, Access access, Message& message)
{
    xr_string why;
    if (XMS::Protect::Allow(path, access, why))
        return false;
    refusal(L, what, path, why.c_str(), message);
    return true;
}

// io and os report a failure as nil and a message; so does a refusal.
int push_refusal(lua_State* L, const Message& message)
{
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

// What a removed function turns into: the call fails, and says what it was.
int removed_function(lua_State* L)
{
    const pcstr name = lua_tostring(L, lua_upvalueindex(1));
    Message message{};
    refusal(L, name ? name : "function", nullptr, "not available in protected mode", message);
    return push_refusal(L, message);
}

// a folder is removed and renamed as a folder
Access write_access_of(pcstr path)
{
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? Access::WriteDir :
                                                                                             Access::Write;
}

// ---- io and os ---------------------------------------------------------------------------------

int io_open(lua_State* L)
{
    const pcstr path = luaL_checkstring(L, 1);
    const pcstr mode = luaL_optstring(L, 2, "r");
    Message message{};
    if (denied(L, "io.open", path, strpbrk(mode, "wa+") ? Access::Write : Access::Read, message))
        return push_refusal(L, message);
    return g_io_open(L);
}

int io_lines(lua_State* L)
{
    Message message{};
    if (lua_type(L, 1) == LUA_TSTRING && denied(L, "io.lines", lua_tostring(L, 1), Access::Read, message))
        return luaL_error(L, "%s", message);
    return g_io_lines(L);
}

int io_input(lua_State* L)
{
    Message message{};
    if (lua_type(L, 1) == LUA_TSTRING && denied(L, "io.input", lua_tostring(L, 1), Access::Read, message))
        return luaL_error(L, "%s", message);
    return g_io_input(L);
}

int io_output(lua_State* L)
{
    Message message{};
    if (lua_type(L, 1) == LUA_TSTRING && denied(L, "io.output", lua_tostring(L, 1), Access::Write, message))
        return luaL_error(L, "%s", message);
    return g_io_output(L);
}

int os_remove(lua_State* L)
{
    const pcstr path = luaL_checkstring(L, 1);
    Message message{};
    if (denied(L, "os.remove", path, write_access_of(path), message))
        return push_refusal(L, message);
    return g_os_remove(L);
}

int os_rename(lua_State* L)
{
    const pcstr from = luaL_checkstring(L, 1);
    const pcstr to = luaL_checkstring(L, 2);
    const Access access = write_access_of(from);
    Message message{};
    if (denied(L, "os.rename", from, access, message) || denied(L, "os.rename", to, access, message))
        return push_refusal(L, message);
    return g_os_rename(L);
}

// ---- chunks: text only -------------------------------------------------------------------------

// load(chunk, chunkname, mode, env) and loadstring take the mode third, loadfile second: the
// mode is overwritten with "t", so whatever the script asked for, bytecode is refused by
// LuaJIT's own loader.
int with_text_mode(lua_State* L, lua_CFunction original, int modeArgument)
{
    if (lua_gettop(L) < modeArgument)
        lua_settop(L, modeArgument);
    lua_pushliteral(L, "t");
    lua_replace(L, modeArgument);
    return original(L);
}

int base_load(lua_State* L) { return with_text_mode(L, g_load, 3); }
int base_loadstring(lua_State* L) { return with_text_mode(L, g_loadstring, 3); }

int base_loadfile(lua_State* L)
{
    Message message{};
    if (lua_type(L, 1) == LUA_TSTRING && denied(L, "loadfile", lua_tostring(L, 1), Access::Read, message))
        return push_refusal(L, message);
    return with_text_mode(L, g_loadfile, 2);
}

int base_dofile(lua_State* L)
{
    const pcstr path = luaL_optstring(L, 1, nullptr);
    Message message{};
    if (path && denied(L, "dofile", path, Access::Read, message))
        return luaL_error(L, "%s", message);
    lua_settop(L, 1);
    if (luaL_loadfilex(L, path, "t") != 0)
        return lua_error(L);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

// Where require's Lua searcher finds `name` along package.path: the file, or the list of what
// was tried; a refusal is a message.
enum class Search : u8
{
    Found,
    Missing,
    Refused,
};

PROTECT_HELPER Search search_lua_file(
    lua_State* L, pcstr name, pcstr templates, string_path& file, string2048& tried, Message& message)
{
    xr_string module(name);
    std::ranges::replace(module, '.', '\\');
    xr_string list;
    std::string_view rest(templates);
    while (!rest.empty())
    {
        const size_t end = rest.find(';');
        const std::string_view pattern = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (pattern.empty())
            continue;
        xr_string candidate;
        for (const char c : pattern)
            candidate += c == '?' ? module : xr_string(1, c);
        const DWORD attributes = GetFileAttributesA(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            list += "\n\tno file '" + candidate + "'";
            continue;
        }
        if (denied(L, "require", candidate.c_str(), Access::Read, message))
            return Search::Refused;
        strncpy_s(file, sizeof(file), candidate.c_str(), _TRUNCATE);
        return Search::Found;
    }
    // a long package.path is cut short in the message, never overflows it
    strncpy_s(tried, sizeof(tried), list.c_str(), _TRUNCATE);
    return Search::Missing;
}

// require's searcher for Lua files, in place of LuaJIT's: the same package.path templates, a
// file the path rules let a script read, text only.
int lua_file_searcher(lua_State* L)
{
    const pcstr name = luaL_checkstring(L, 1);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const pcstr templates = lua_tostring(L, -1);
    if (!templates)
        return luaL_error(L, "'package.path' must be a string");

    string_path file{};
    string2048 tried{};
    Message message{};
    switch (search_lua_file(L, name, templates, file, tried, message))
    {
    case Search::Refused: return luaL_error(L, "%s", message);
    case Search::Missing: lua_pushstring(L, tried); return 1;
    case Search::Found: break;
    }
    if (luaL_loadfilex(L, file, "t") != 0)
        return luaL_error(L, "error loading module '%s' from file '%s':\n\t%s", name, file, lua_tostring(L, -1));
    return 1;
}

// ---- lfs ---------------------------------------------------------------------------------------

int lfs_read(lua_State* L, lua_CFunction original, pcstr what)
{
    Message message{};
    if (lua_type(L, 1) == LUA_TSTRING && denied(L, what, lua_tostring(L, 1), Access::Read, message))
        return push_refusal(L, message);
    return original(L);
}

int lfs_write(lua_State* L, lua_CFunction original, pcstr what, Access access)
{
    const pcstr path = luaL_checkstring(L, 1);
    Message message{};
    if (denied(L, what, path, access, message))
        return push_refusal(L, message);
    return original(L);
}

int lfs_attributes(lua_State* L) { return lfs_read(L, g_lfs_attributes, "lfs.attributes"); }
int lfs_symlinkattributes(lua_State* L) { return lfs_read(L, g_lfs_symlinkattributes, "lfs.symlinkattributes"); }
int lfs_mkdir(lua_State* L) { return lfs_write(L, g_lfs_mkdir, "lfs.mkdir", Access::WriteDir); }
int lfs_rmdir(lua_State* L) { return lfs_write(L, g_lfs_rmdir, "lfs.rmdir", Access::WriteDir); }
int lfs_touch(lua_State* L) { return lfs_write(L, g_lfs_touch, "lfs.touch", write_access_of(luaL_checkstring(L, 1))); }
int lfs_lock_dir(lua_State* L) { return lfs_write(L, g_lfs_lock_dir, "lfs.lock_dir", Access::WriteDir); }

int lfs_dir(lua_State* L)
{
    const pcstr path = luaL_checkstring(L, 1);
    Message message{};
    if (denied(L, "lfs.dir", path, Access::Read, message))
        return luaL_error(L, "%s", message);
    return g_lfs_dir(L);
}

// ---- marshal: no functions, so no bytecode ------------------------------------------------------

// Whether encoding the value at the absolute `index` would write a function, asked the way the
// encoder asks (lmarshal.c): a function value; a table with __persist, whose hook always hands
// over a function; a userdata whose __persist returns a function rather than Lua source text.
// The game's CTime returns source text, and a save without CTimes does not exist.
// `seen` is the absolute index of a table of the tables already walked.
bool holds_function(lua_State* L, int index, int seen, int depth)
{
    const int type = lua_type(L, index);
    if (type == LUA_TFUNCTION)
        return true;
    if (type != LUA_TTABLE && type != LUA_TUSERDATA)
        return false;
    // too deep to walk, or no room left on the stack to walk it: refused rather than guessed
    if (depth > 256 || !lua_checkstack(L, 4))
        return true;
    if (luaL_getmetafield(L, index, "__persist"))
    {
        if (type == LUA_TTABLE)
        {
            lua_pop(L, 1);
            return true;
        }
        // A hook that fails here fails again in the encoder, which reports it.
        lua_pushvalue(L, index);
        const bool function = lua_pcall(L, 1, 1, 0) == 0 && lua_isfunction(L, -1);
        lua_pop(L, 1);
        return function;
    }
    if (type != LUA_TTABLE)
        return false;
    lua_pushvalue(L, index);
    lua_rawget(L, seen);
    const bool walked = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (walked)
        return false;
    lua_pushvalue(L, index);
    lua_pushboolean(L, true);
    lua_rawset(L, seen);
    lua_pushnil(L);
    while (lua_next(L, index))
    {
        const int value = lua_gettop(L);
        if (holds_function(L, value - 1, seen, depth + 1) || holds_function(L, value, seen, depth + 1))
        {
            lua_pop(L, 2);
            return true;
        }
        lua_pop(L, 1);
    }
    return false;
}

PROTECT_HELPER bool stores_function(lua_State* L, pcstr what, Message& message)
{
    // The walked tables live in a Lua table: no object here for an error to unwind through.
    lua_newtable(L);
    const bool function = holds_function(L, 1, lua_gettop(L), 0);
    lua_pop(L, 1);
    if (!function)
        return false;
    log_refusal(L, what, nullptr, "functions cannot be stored in protected mode");
    xr_sprintf(message, "%s refused by protected mode: functions cannot be stored", what);
    return true;
}

int encode_guard(lua_State* L, lua_CFunction original, pcstr what)
{
    Message message{};
    if (stores_function(L, what, message))
        return luaL_error(L, "%s", message);
    return original(L);
}

int mar_encode(lua_State* L) { return encode_guard(L, g_mar_encode, "marshal.encode"); }
int mar_encode_begin(lua_State* L) { return encode_guard(L, g_mar_encode_begin, "marshal.encode_begin"); }
int mar_clone(lua_State* L) { return encode_guard(L, g_mar_clone, "marshal.clone"); }

int mar_decode(lua_State* L)
{
    size_t size = 0;
    const pcstr data = luaL_checklstring(L, 1, &size);
    // every function marshal writes is a LuaJIT dump, and every dump starts with this
    if (std::string_view(data, size).find("\x1bLJ") != std::string_view::npos)
    {
        log_refusal(L, "marshal.decode", nullptr, "the data holds compiled code");
        return luaL_error(L, "marshal.decode refused by protected mode: the data holds compiled code");
    }
    return g_mar_decode(L);
}

// ---- wiring -------------------------------------------------------------------------------------

// Pushes the table `name` of the globals (the globals themselves for nullptr); false when absent.
bool push_table(lua_State* L, pcstr name)
{
    if (!name)
    {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        return true;
    }
    lua_getglobal(L, name);
    if (lua_istable(L, -1))
        return true;
    lua_pop(L, 1);
    return false;
}

void set_removed(lua_State* L, pcstr table, pcstr field, pcstr shown)
{
    if (!push_table(L, table))
        return;
    lua_pushstring(L, shown);
    lua_pushcclosure(L, removed_function, 1);
    lua_setfield(L, -2, field);
    lua_pop(L, 1);
}

// Puts `guard` in place of table.field and keeps the original; a field that is not a C function
// is removed instead - a guard never hands over to what it cannot vouch for.
void wrap(lua_State* L, pcstr table, pcstr field, lua_CFunction guard, lua_CFunction& original, pcstr shown)
{
    if (!push_table(L, table))
        return;
    lua_getfield(L, -1, field);
    const bool present = !lua_isnil(L, -1);
    original = lua_tocfunction(L, -1);
    if (original)
    {
        // The original runs inside the guard's call, and LuaJIT's io gives a new file the
        // environment of the running C function as its metatable: without the original's, a
        // file io.open returns has no methods.
        lua_pushcfunction(L, guard);
        lua_getfenv(L, -2);
        lua_setfenv(L, -2);
        lua_setfield(L, -3, field);
    }
    lua_pop(L, 2);
    if (!original && present)
        set_removed(L, table, field, shown);
}

void clear_module(lua_State* L, pcstr registryTable, pcstr name)
{
    lua_getglobal(L, "package");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, registryTable);
        if (lua_istable(L, -1))
        {
            lua_pushnil(L);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}
} // namespace

void ScriptProtect::Refused(lua_State* L, pcstr what, pcstr why)
{
    XMS::Protect::Refused(what, why, Location(L).c_str());
}

xr_string ScriptProtect::Location(lua_State* L)
{
    if (!L)
        return {};
    lua_Debug ar{};
    for (int level = 0; level < 16 && lua_getstack(L, level, &ar); ++level)
    {
        if (lua_getinfo(L, "Sl", &ar) && ar.currentline > 0 && ar.what && 0 != xr_strcmp(ar.what, "C"))
        {
            string512 text{};
            xr_sprintf(text, "%s:%d", ar.short_src, ar.currentline);
            return text;
        }
    }
    return {};
}

void ScriptProtect::Apply(lua_State* L)
{
    const int top = lua_gettop(L);

    // native code, programs and the world outside the game
    set_removed(L, "os", "execute", "os.execute");
    set_removed(L, "os", "tmpname", "os.tmpname");
    set_removed(L, "os", "getenv", "os.getenv");
    set_removed(L, "io", "popen", "io.popen");
    set_removed(L, "io", "tmpfile", "io.tmpfile");
    set_removed(L, "string", "dump", "string.dump");
    set_removed(L, "package", "loadlib", "package.loadlib");
    set_removed(L, "lfs", "link", "lfs.link");
    set_removed(L, "lfs", "chdir", "lfs.chdir");
    clear_module(L, "loaded", "ffi");
    clear_module(L, "preload", "ffi");
    clear_module(L, "loaded", "jit.util");
    clear_module(L, "preload", "jit.util");

    // files
    wrap(L, "io", "open", io_open, g_io_open, "io.open");
    wrap(L, "io", "lines", io_lines, g_io_lines, "io.lines");
    wrap(L, "io", "input", io_input, g_io_input, "io.input");
    wrap(L, "io", "output", io_output, g_io_output, "io.output");
    wrap(L, "os", "remove", os_remove, g_os_remove, "os.remove");
    wrap(L, "os", "rename", os_rename, g_os_rename, "os.rename");
    wrap(L, "lfs", "attributes", lfs_attributes, g_lfs_attributes, "lfs.attributes");
    wrap(L, "lfs", "symlinkattributes", lfs_symlinkattributes, g_lfs_symlinkattributes, "lfs.symlinkattributes");
    wrap(L, "lfs", "dir", lfs_dir, g_lfs_dir, "lfs.dir");
    wrap(L, "lfs", "mkdir", lfs_mkdir, g_lfs_mkdir, "lfs.mkdir");
    wrap(L, "lfs", "rmdir", lfs_rmdir, g_lfs_rmdir, "lfs.rmdir");
    wrap(L, "lfs", "touch", lfs_touch, g_lfs_touch, "lfs.touch");
    wrap(L, "lfs", "lock_dir", lfs_lock_dir, g_lfs_lock_dir, "lfs.lock_dir");

    // chunks
    wrap(L, nullptr, "load", base_load, g_load, "load");
    wrap(L, nullptr, "loadstring", base_loadstring, g_loadstring, "loadstring");
    wrap(L, nullptr, "loadfile", base_loadfile, g_loadfile, "loadfile");
    lua_CFunction unusedDofile{};
    wrap(L, nullptr, "dofile", base_dofile, unusedDofile, "dofile");

    // require: the preload searcher stays, the Lua one is ours, the C ones go
    lua_getglobal(L, "package");
    if (lua_istable(L, -1))
    {
        lua_pushliteral(L, "");
        lua_setfield(L, -2, "cpath");
        for (pcstr name : {"loaders", "searchers"})
        {
            lua_getfield(L, -1, name);
            if (lua_istable(L, -1))
            {
                lua_pushcfunction(L, lua_file_searcher);
                lua_rawseti(L, -2, 2);
                lua_pushnil(L);
                lua_rawseti(L, -2, 4);
                lua_pushnil(L);
                lua_rawseti(L, -2, 3);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // marshal: functions travel as bytecode
    wrap(L, "marshal", "encode", mar_encode, g_mar_encode, "marshal.encode");
    wrap(L, "marshal", "encode_begin", mar_encode_begin, g_mar_encode_begin, "marshal.encode_begin");
    wrap(L, "marshal", "clone", mar_clone, g_mar_clone, "marshal.clone");
    wrap(L, "marshal", "decode", mar_decode, g_mar_decode, "marshal.decode");

    // debug: what error reports and the profiler use, nothing that reaches around the guards
    if (push_table(L, "debug"))
    {
        for (pcstr name : {"getupvalue", "setupvalue", "upvalueid", "upvaluejoin", "getlocal", "setlocal",
                 "getregistry", "getmetatable", "setmetatable", "getfenv", "setfenv", "debug"})
        {
            lua_pushnil(L);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);
    }

    lua_settop(L, top);
    Msg("* [protect] protected mode applied to the script state (%s)", XMS::Catalog::ProtectedReason().c_str());
}
