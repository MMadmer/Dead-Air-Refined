// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "xms_tables.h"

#include "xms_items.h"

#ifndef XMS_LUADATA_STANDALONE
#include "xms_characters.h"
#include "xms_core.h"
#include "xrCore/xr_ini.h"
#endif

#include <cmath>
#include <cstdio>

namespace XMS
{
namespace tableasset
{
namespace
{
using Value = LuaValue;
using assetline::Lists;
using assetline::Lower;
using assetline::Names;
using assetline::NumberText;
using assetline::PlainName;
using assetline::PlainValue;

constexpr const char* kDeathCounts = "misc\\death_generic.ltx";
constexpr const char* kDeathChances = "misc\\death_items_by_communities.ltx";

// one click of Craft makes that many objects at once: more is a typing mistake that would stop the game
constexpr double kMaxRecipeCount = 1000;

// The rows of a file: the list under `key`. False when the key holds anything else.
bool rows_of(const Value& a, const char* key, const LuaVector<Value>*& out)
{
    static const LuaVector<Value> none;
    out = &none;
    const Value* list = a.get(key);
    if (!list)
        return true;
    if (list->type != Value::Table || !list->keys.empty())
        return false;
    out = &list->arr;
    return true;
}

// The section a row is about, lower case; empty when the key is missing or names none.
LuaString section_of(const Value& row, const char* key)
{
    const Value* v = row.type == Value::Table ? row.get(key) : nullptr;
    const LuaString name = v && v->type == Value::String ? Lower(v->s) : LuaString();
    return PlainName(name) ? name : LuaString();
}

bool plan_trade(const Value& a, TablePlan& p, LuaString& why)
{
    const Value* trader = a.get("trader");
    if (!trader || trader->type != Value::String || trader->s.empty())
    {
        why = "a trader file names the trader: trader";
        return false;
    }
    const LuaString name = Lower(trader->s);
    const bool path = name.size() > 4 && name.compare(name.size() - 4, 4, ".ltx") == 0;
    const LuaString file = ConfigPath(path ? name : "misc\\trade\\" + name + ".ltx");
    if (file.empty() || file.find(':') != LuaString::npos || !PlainValue(file))
    {
        why = "trader is a trader's config (trade_military), or a config's path under configs";
        return false;
    }
    const LuaVector<Value>* rows = nullptr;
    if (!rows_of(a, "stock", rows))
    {
        why = "stock is a list of { item, supplies, count, chance }";
        return false;
    }
    Lists out{p.lists};
    for (const Value& row : *rows)
    {
        const LuaString item = section_of(row, "item");
        if (item.empty())
        {
            why = "a row of stock names its item: item";
            return false;
        }
        LuaVector<LuaString> supplies;
        const Value* count = row.get("count");
        const Value* chance = row.get("chance");
        if (!Names(row.get("supplies"), supplies))
            why = "stock.supplies is a section of the trader's config or a list of them";
        else if ((count && (count->type != Value::Number || count->n < 0)) || (chance && chance->type != Value::Number))
            why = "stock.count and stock.chance are numbers, the count not below 0";
        if (!why.empty())
            return false;
        if (supplies.empty())
            supplies.push_back("supplies_1");
        // a count of 0 takes the item off the shelf
        const bool gone = count && count->n == 0;
        const LuaString line = NumberText(count ? count->n : 1.0) + ", " + NumberText(chance ? chance->n : 1.0);
        for (const LuaString& s : supplies)
        {
            const LuaString section = Lower(s);
            if (!PlainName(section))
            {
                why = "stock.supplies names a section no config can hold";
                return false;
            }
            out.header(file, section);
            out.line(gone ? LtxDirective::RemoveKey : LtxDirective::Set, item, gone ? LuaString() : line);
        }
        ++p.rows;
    }
    return true;
}

bool plan_loot(const Value& a, TablePlan& p, LuaString& why)
{
    const LuaVector<Value>* rows = nullptr;
    if (!rows_of(a, "drops", rows))
    {
        why = "drops is a list of { item, count, chance, by }";
        return false;
    }
    Lists out{p.lists};
    for (const Value& row : *rows)
    {
        const LuaString item = section_of(row, "item");
        if (item.empty())
        {
            why = "a row of drops names its item: item";
            return false;
        }
        if (const Value* count = row.get("count"))
        {
            LuaString text;
            if (count->type == Value::Number)
                text = NumberText(count->n);
            else if (count->type == Value::Table && count->keys.empty() && count->arr.size() == 2 &&
                count->arr[0].type == Value::Number && count->arr[1].type == Value::Number)
                text = NumberText(count->arr[0].n) + ", " + NumberText(count->arr[1].n);
            else
            {
                why = "drops.count is a number or { min, max }";
                return false;
            }
            out.header(kDeathCounts, "item_count");
            out.line(LtxDirective::Set, item, text);
        }
        if (const Value* chance = row.get("chance"))
        {
            if (chance->type != Value::Number)
            {
                why = "drops.chance is a number";
                return false;
            }
            // every community and rank inherits [base]
            out.header(kDeathChances, "base");
            out.line(LtxDirective::Set, item, NumberText(chance->n));
        }
        if (const Value* by = row.get("by"))
        {
            if (by->type != Value::Table || !by->arr.empty())
            {
                why = "drops.by is { <community or community_rank> = chance }";
                return false;
            }
            for (size_t i = 0; i < by->keys.size(); ++i)
            {
                const LuaString who = Lower(by->keys[i]);
                if (!PlainName(who) || by->vals[i].type != Value::Number)
                {
                    why = "drops.by is { <community or community_rank> = chance }";
                    return false;
                }
                out.header(kDeathChances, who);
                out.line(LtxDirective::Set, item, NumberText(by->vals[i].n));
            }
        }
        ++p.rows;
    }
    return true;
}

bool plan_recipes(const Value& a, TablePlan& p, LuaString& why)
{
    const LuaVector<Value>* rows = nullptr;
    if (!rows_of(a, "recipes", rows))
    {
        why = "recipes is a list of { makes, count, at, parts }";
        return false;
    }
    for (const Value& row : *rows)
    {
        TableRecipe r;
        r.makes = section_of(row, "makes");
        r.at = section_of(row, "at");
        const Value* count = row.type == Value::Table ? row.get("count") : nullptr;
        if (r.makes.empty())
            why = "a recipe names what it makes: makes";
        else if (r.at.empty())
            why = "a recipe names the tool or workbench it is made at: at";
        else if (count && (count->type != Value::Number || count->n < 1 || count->n > kMaxRecipeCount ||
                     count->n != std::floor(count->n)))
            why = "a recipe's count is a whole number from 1 to 1000";
        else if (!Names(row.get("parts"), r.parts) || r.parts.empty() || r.parts.size() > 4)
            why = "a recipe takes one to four parts";
        if (!why.empty())
            return false;
        r.count = count ? int(count->n) : 1;
        for (LuaString& part : r.parts)
        {
            part = Lower(part);
            if (!PlainName(part))
            {
                why = "a part of a recipe names a section no config can hold";
                return false;
            }
        }
        p.recipes.push_back(r);
        ++p.rows;
    }
    return true;
}
} // namespace
} // namespace tableasset

bool PlanTable(TableKind kind, const LuaValue& asset, TablePlan& out, LuaString& why)
{
    using namespace tableasset;
    out = TablePlan();
    why.clear();
    if (asset.type != LuaValue::Table)
    {
        why = "the file returns one table";
        return false;
    }
    switch (kind)
    {
    case TableKind::Trade: return plan_trade(asset, out, why);
    case TableKind::Loot: return plan_loot(asset, out, why);
    default: return plan_recipes(asset, out, why);
    }
}
} // namespace XMS

#ifndef XMS_LUADATA_STANDALONE
namespace XMS
{
namespace tableasset
{
namespace
{
struct KindRow
{
    TableKind kind;
    pcstr mask;
    pcstr header;
};

// No step yet: version 1 is the only one of each kind, and a file without the header is version 1.
constexpr int kTableVersion = 1;
constexpr KindRow kKinds[] = {
    {TableKind::Trade, "*.tradeasset", "trade"},
    {TableKind::Loot, "*.lootasset", "loot"},
    {TableKind::Recipes, "*.recipeasset", "recipe"},
};

bool read_whole(pcstr path, xr_string& out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(std::max(0l, size)));
    const size_t got = size > 0 ? fread(out.data(), 1, size_t(size), f) : 0;
    fclose(f);
    out.resize(got);
    return true;
}

// `<n> = at, result, count, part, ...`: n goes on after the highest number the section holds, padded so that the
// order of the keys as text, which is how a section's lines come back, is the order the recipes were added in
void add_recipes(CInifile& ini, const Module& m, const xr_vector<TableRecipe>& recipes)
{
    if (recipes.empty())
        return;
    u32 next = 1;
    if (ini.section_exist("xms_craft"))
        for (const CInifile::Item& item : ini.r_section("xms_craft").Data)
        {
            const pcstr key = item.first.c_str();
            bool number = key && *key;
            for (pcstr c = key; number && *c; ++c)
                number = *c >= '0' && *c <= '9';
            if (number)
                next = std::max(next, u32(atoi(key)) + 1);
        }
    for (const TableRecipe& r : recipes)
    {
        string32 count, key;
        xr_sprintf(count, "%d", r.count);
        xr_sprintf(key, "%06u", next++);
        xr_string line = r.at + ", " + r.makes + ", " + count;
        for (const xr_string& part : r.parts)
            line += ", " + part;
        ini.xms_set_line("xms_craft", key, GameText(line).c_str(), nullptr, m.layer);
    }
}
} // namespace
} // namespace tableasset

bool BuildTables(const Module& m, CInifile& settings, xr_vector<LtxDirective>& lists)
{
    using namespace tableasset;
    unsigned blocks = 0;
    for (const LtxDirective& d : lists)
        blocks = std::max(blocks, d.block);

    u32 read[3] = {0, 0, 0};
    u32 rows = 0;
    for (size_t k = 0; k < std::size(kKinds); ++k)
    {
        const KindRow& kind = kKinds[k];
        xr_vector<xr_string> files;
        ListFilesRelative(m.root.c_str(), kind.mask, true, files);
        for (const xr_string& file : files)
        {
            xr_string text, error;
            if (!read_whole((m.root + file).c_str(), text))
            {
                Msg("! XMS: [%s] %s: cannot be read", m.id.c_str(), file.c_str());
                continue;
            }
            LuaValue value;
            if (!ReadLuaData(text.c_str(), text.size(), value, error) || value.type != LuaValue::Table)
            {
                Msg("! XMS: [%s] %s: left out - %s", m.id.c_str(), file.c_str(),
                    error.empty() ? "the file returns one table" : error.c_str());
                continue;
            }
            if (!UpgradeLuaData(value, kind.header, kTableVersion, nullptr, false, error))
            {
                Msg("! %s\\%s: %s", m.id.c_str(), file.c_str(), error.c_str());
                continue;
            }
            TablePlan plan;
            if (!PlanTable(kind.kind, value, plan, error))
            {
                Msg("! XMS: [%s] %s: left out - %s", m.id.c_str(), file.c_str(), error.c_str());
                continue;
            }
            add_recipes(settings, m, plan.recipes);
            // every plan numbers its blocks from 1; side by side they stay apart
            const unsigned base = blocks;
            for (const LtxDirective& d : plan.lists)
            {
                LtxDirective& kept = lists.emplace_back(d);
                kept.block = d.block ? base + d.block : 0;
                kept.key = GameText(kept.key);
                kept.value = GameText(kept.value);
                blocks = std::max(blocks, kept.block);
            }
            ++read[k];
            rows += plan.rows;
        }
    }
    const u32 all = read[0] + read[1] + read[2];
    if (all)
        Msg("* XMS: [%s] %u trader(s), %u loot table(s) and %u recipe file(s) read from the module (%u row(s))",
            m.id.c_str(), read[0], read[1], read[2], rows);
    return all > 0;
}
} // namespace XMS
#endif
