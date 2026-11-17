// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS items - a module's inventory items. One .itemasset per item, anywhere in the module, is a NEW
// item or a CHANGE of one that exists: the game reads them when the configs come up and makes the
// sections, the strings and the use scene itself. An item says what the item is and nothing of
// another thing: who sells it, who carries it and what makes it are files of their own
// (xms_tables.h). Contract: docs/dead-air/MODDING.md, "A module's items". What an asset asks for
// (its plan) is free of the engine's types under XMS_LUADATA_STANDALONE, for tools/qa/ltx.

#include "xms_ltxp.h"

#ifndef XMS_LUADATA_STANDALONE
class XRCORE_API CInifile;
#endif

namespace XMS
{
// A string the item names and what it says, one string or { language = text }.
struct ItemText
{
    LuaString id;
    LuaValue text;
};

// Another section the item makes or changes.
struct ItemSection
{
    LuaString name;
    bool make{false}; // one it makes; otherwise one that has to exist
    LuaString parents;
    LuaVector<LtxDirective> ops; // Set / RemoveKey / Add / Drop, section left empty
};

struct ItemPlan
{
    LuaString file; // inside the module, for the log
    bool edit{false};
    bool abstract_{false};
    LuaString section;
    LuaString parents; // a new item's bases, as `[section] : a, b` names them
    // The item's own section, in order: set, unset, add, drop, the strings, the icon, its hud key.
    // Section left empty.
    LuaVector<LtxDirective> own;
    // The hud section: a new item's <section>_hud (over `hud_parents`, or over the one its bases name
    // when that is left out), an edit's the one its hud key names once `own` is applied.
    bool hud{false};
    bool hud_parents_given{false};
    LuaString hud_parents;
    LuaVector<LtxDirective> hud_ops; // section left empty
    LuaVector<ItemSection> sections;
    LuaVector<ItemText> texts;
    // the use scene: directives of the scenes' config, and of the system config (no file) for its hud
    LuaVector<LtxDirective> lists;
    bool icon_picture{false}; // the icon is a picture only a built module turns into a texture
    // keys the file has that are another file's to say (`sold`, `loot`, `craft`, `lists`): never read
    LuaVector<LuaString> foreign;
};

// What an asset of the module (`file` its path inside the module) asks for. False with `why`, the
// reason the log gives after "left out - ", when it cannot be an item.
bool PlanItem(const LuaValue& asset, const LuaString& module, const LuaString& file, ItemPlan& out, LuaString& why);

// A value as a config line carries it: a string as written, a number without needless decimals, a
// boolean as true / false, a list of those joined by ", ". False for anything else.
bool ItemValueText(const LuaValue& value, LuaString& out);

// What the readers of a module's data share.
namespace assetline
{
LuaString Lower(LuaString text);
// A section or a key a config line can hold.
bool PlainName(const LuaString& name);
// A value a config line can hold: no line break, no other control character.
bool PlainValue(const LuaString& value);
// A number without needless decimals.
LuaString NumberText(double n);
// A string or a list of strings; false for anything else. Nothing (no key) is an empty list.
bool Names(const LuaValue* v, LuaVector<LuaString>& out);

// Directives of another config (or of the system config, `file` empty), each block with its header.
struct Lists
{
    LuaVector<LtxDirective>& out;
    unsigned block{0};
    LuaString file, section;

    void header(const LuaString& f, const LuaString& s, const LuaString& parents = LuaString());
    void line(LtxDirective::Op op, const LuaString& key, const LuaString& value);
};
} // namespace assetline

#ifndef XMS_LUADATA_STANDALONE
struct Module;

// Makes the module's items in `settings` - new sections first, then changes in the order of their
// paths - and appends their use scenes, lines of the scenes' config, to `lists`. False when the
// module has no item.
bool BuildItems(const Module& m, CInifile& settings, xr_vector<LtxDirective>& lists);
#endif
} // namespace XMS
