// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The directives of a config patch, as a list: what a module's .ltxp files say and what the game
// makes of its items. `#file <path>` sends the directives after it to that config instead of the
// system config. Free of the engine's types under XMS_LUADATA_STANDALONE, for tools/qa/ltx.

#include "xms_luadata.h"

namespace XMS
{
struct LtxDirective
{
    enum Op : unsigned char
    {
        Section,       // [section] or [section] : parents - makes the section where it is missing
        RemoveSection, // [!section]
        Set,           // key = value; an empty value is a bare key
        Add,           // +key = a, b
        Drop,          // -key = a, b
        RemoveKey,     // !key
    };
    Op op{Set};
    // The header a key is under, unique within one list. 0: none since the start or the last #file, so
    // the key changes nothing; a header that is refused (a missing parent) takes its keys with it.
    unsigned block{0};
    LuaString file;    // the config it patches, by its path under configs (ConfigPath); empty: the system config
    LuaString section; // lower case
    LuaString parents; // Section: what a section it makes inherits, as written after ':'
    LuaString key;
    LuaString value;
};

// A config's path under configs as patches name it: lower case, '\' separators, no leading or doubled
// separator, "." and ".." resolved. Empty for a path that is empty or climbs out.
LuaString ConfigPath(const LuaString& path);

// The directives of one .ltxp text, in the order written, appended to `out`; block numbers go on
// from the highest one `out` holds. The text ends at `size` or at a zero byte.
void ParseLtxp(const char* text, size_t size, LuaVector<LtxDirective>& out);
} // namespace XMS
