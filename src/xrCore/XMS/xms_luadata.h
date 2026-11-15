// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// Plain data written in Lua's syntax - `return { ... }` of literals - read as data: what a module
// describes (a character, a texture, a sound, a model) comes up long before any Lua state, and
// nothing a module ships should run that early, so a chunk with anything in it but values is
// refused. Free of the engine's types under XMS_LUADATA_STANDALONE, for tools/qa/luadata.

#include <cstddef>

#ifdef XMS_LUADATA_STANDALONE
#include <string>
#include <vector>
#define XMS_LUADATA_API
namespace XMS
{
using LuaString = std::string;
template <class T>
using LuaVector = std::vector<T>;
} // namespace XMS
#else
#include "xrCore/xrCore.h"
#define XMS_LUADATA_API XRCORE_API
namespace XMS
{
using LuaString = xr_string;
template <class T>
using LuaVector = xr_vector<T>;
} // namespace XMS
#endif

namespace XMS
{
struct LuaValue
{
    enum Type : unsigned char
    {
        Nil,
        Bool,
        Number,
        String,
        Table,
    };
    Type type{Nil};
    bool b{false};
    double n{0.0};
    LuaString s;
    LuaVector<LuaString> keys; // a table's named fields, in the file's order
    LuaVector<LuaValue> vals;
    LuaVector<LuaValue> arr; // a table's list part

    const LuaValue* get(const char* key) const
    {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key)
                return &vals[i];
        return nullptr;
    }
    LuaString text(const char* key) const
    {
        const LuaValue* v = get(key);
        return v && v->type == String ? v->s : LuaString();
    }
    double number(const char* key, double fallback) const
    {
        const LuaValue* v = get(key);
        return v && v->type == Number ? v->n : fallback;
    }
    bool flag(const char* key, bool fallback) const
    {
        const LuaValue* v = get(key);
        return v && v->type == Bool ? v->b : fallback;
    }
    void set(const LuaString& key, LuaValue&& value)
    {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key)
            {
                vals[i] = static_cast<LuaValue&&>(value);
                return;
            }
        keys.push_back(key);
        vals.emplace_back(static_cast<LuaValue&&>(value));
    }
};

// `text` holds `size` bytes and a zero after them. False with `error` ("line N: ...") when the
// text is not one plain value.
XMS_LUADATA_API bool ReadLuaData(const char* text, size_t size, LuaValue& out, LuaString& error);

// One step of a kind's chain: lifts an asset of version v to v + 1 in place.
using LuaDataStep = void (*)(LuaValue& asset);

// The version rule of every reader of a module's assets. The header `key` holds the version;
// one above `current` is refused ("made for a newer game (version N, this game reads up to M)"),
// an older one goes through `steps[v - 1]` for every v up to `current` (`steps` holds
// `current - 1` entries; a null one changes nothing) and its header is set to `current`. A
// missing header is version 1 when `required` is false, and refused otherwise. False with
// `why` when the asset cannot be read.
XMS_LUADATA_API bool UpgradeLuaData(LuaValue& asset, const char* key, int current, const LuaDataStep* steps,
    bool required, LuaString& why);
} // namespace XMS
