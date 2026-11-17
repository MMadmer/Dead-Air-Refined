// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS tables - what a module adds to the game's shared tables, each in a file of the side that
// owns it: a trader's shelf (.tradeasset), what the dead carry (.lootasset), recipes
// (.recipeasset). An item never says any of it: who sells a thing is the trader's to say.
// Contract: docs/dead-air/MODDING.md, "A module's traders, loot and recipes". What a file asks for
// (its plan) is free of the engine's types under XMS_LUADATA_STANDALONE, for tools/qa/ltx.

#include "xms_ltxp.h"

#ifndef XMS_LUADATA_STANDALONE
class XRCORE_API CInifile;
#endif

namespace XMS
{
enum class TableKind
{
    Trade,   // .tradeasset: `trade = 1`, a trader of the game and rows of its stock
    Loot,    // .lootasset: `loot = 1`, rows of the death loot
    Recipes, // .recipeasset: `recipe = 1`, recipes
};

// `<n> = at, result, count, part, ...` of the system config's [xms_craft]
struct TableRecipe
{
    LuaString at;
    LuaString makes;
    int count{1};
    LuaVector<LuaString> parts;
};

struct TablePlan
{
    // lines of the configs the file is about: a trader's, the death loot's
    LuaVector<LtxDirective> lists;
    LuaVector<TableRecipe> recipes;
    unsigned rows{0};
};

// What a file of the kind asks for. False with `why`, the reason the log gives after "left out - ",
// when it cannot be read as its kind: a file is taken whole or not at all.
bool PlanTable(TableKind kind, const LuaValue& asset, TablePlan& out, LuaString& why);

#ifndef XMS_LUADATA_STANDALONE
struct Module;

// Reads the module's traders, loot tables and recipes - each kind in the order of its paths, after
// the module's items - adds the recipes to `settings` and appends the lines of the other configs to
// `lists`. False when the module has none.
bool BuildTables(const Module& m, CInifile& settings, xr_vector<LtxDirective>& lists);
#endif
} // namespace XMS
