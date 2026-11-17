// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "xms_items.h"

#ifndef XMS_LUADATA_STANDALONE
#include "xms_characters.h"
#include "xms_core.h"
#include "xms_ltx.h"
#include "xrCore/xr_ini.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>

namespace XMS
{
namespace itemasset
{
namespace
{
using Value = LuaValue;

constexpr const char* kScenes = "items\\items\\animations_settings.ltx";

using assetline::Lists;

LuaString lower(LuaString text) { return assetline::Lower(std::move(text)); }
bool plain_name(const LuaString& name) { return assetline::PlainName(name); }
bool plain_value(const LuaString& value) { return assetline::PlainValue(value); }
LuaString number_text(double n) { return assetline::NumberText(n); }
bool names(const Value* v, LuaVector<LuaString>& out) { return assetline::Names(v, out); }

// A section an item makes: what a save reads into 64 characters.
bool section_name(const LuaString& name)
{
    if (name.empty() || name.size() >= 64)
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    return true;
}

bool valid_id(const LuaString& id)
{
    if (id.empty() || id.size() > 48 || (id[0] >= '0' && id[0] <= '9'))
        return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

LuaString joined(const LuaVector<LuaString>& parts, const char* separator)
{
    LuaString out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            out += separator;
        out += parts[i];
    }
    return out;
}

// The members of a list a change adds or drops: one value or a list of values.
bool members(const Value& v, LuaVector<LuaString>& out)
{
    if (v.type != Value::Table)
    {
        LuaString text;
        if (!ItemValueText(v, text) || text.empty())
            return false;
        out.push_back(text);
        return true;
    }
    if (!v.keys.empty())
        return false;
    for (const Value& x : v.arr)
    {
        LuaString text;
        if (x.type == Value::Table || !ItemValueText(x, text))
            return false;
        out.push_back(text);
    }
    return true;
}

bool text_value(const Value& v)
{
    if (v.type == Value::String)
        return true;
    if (v.type != Value::Table)
        return false;
    for (const Value& x : v.vals)
        if (x.type == Value::String)
            return true;
    return false;
}

LtxDirective key_op(LtxDirective::Op op, const LuaString& key, const LuaString& value)
{
    LtxDirective d;
    d.op = op;
    d.block = 1;
    d.key = key;
    d.value = value;
    return d;
}

// set / unset / add / drop of one section, in that order
bool changes(const Value& t, LuaVector<LtxDirective>& ops, LuaString& why, const LuaString& where)
{
    if (const Value* set = t.get("set"))
    {
        if (set->type != Value::Table || !set->arr.empty())
        {
            why = where + "set takes key = value";
            return false;
        }
        for (size_t i = 0; i < set->keys.size(); ++i)
        {
            const LuaString& key = set->keys[i];
            LuaString text;
            if (!plain_name(key))
                why = where + "'" + key + "' cannot be a key of a config";
            else if (!ItemValueText(set->vals[i], text))
                why = where + "the value of " + key + " is no string, number, boolean or list of them";
            else if (!plain_value(text))
                why = where + "the value of " + key + " holds characters no config can carry";
            if (!why.empty())
                return false;
            ops.push_back(key_op(LtxDirective::Set, key, text));
        }
    }
    if (const Value* unset = t.get("unset"))
    {
        LuaVector<LuaString> keys;
        if (!names(unset, keys))
        {
            why = where + "unset is a list of keys";
            return false;
        }
        for (const LuaString& key : keys)
        {
            if (!plain_name(key))
            {
                why = where + "'" + key + "' cannot be a key of a config";
                return false;
            }
            ops.push_back(key_op(LtxDirective::RemoveKey, key, LuaString()));
        }
    }
    for (const char* which : {"add", "drop"})
    {
        const Value* lists = t.get(which);
        if (!lists)
            continue;
        if (lists->type != Value::Table || !lists->arr.empty())
        {
            why = where + which + " takes key = member or { members }";
            return false;
        }
        for (size_t i = 0; i < lists->keys.size(); ++i)
        {
            const LuaString& key = lists->keys[i];
            LuaVector<LuaString> list;
            if (!plain_name(key))
                why = where + "'" + key + "' cannot be a key of a config";
            else if (!members(lists->vals[i], list))
                why = where + which + "." + key + " is a member or a list of them";
            else
                for (const LuaString& member : list)
                    if (!plain_value(member) || member.find(',') != LuaString::npos)
                        why = where + which + "." + key + " holds a member no list can carry";
            if (!why.empty())
                return false;
            ops.push_back(key_op(*which == 'a' ? LtxDirective::Add : LtxDirective::Drop, key, joined(list, ",")));
        }
    }
    return true;
}

bool parents_of(const Value* base, LuaString& out, LuaString& why, const LuaString& what)
{
    LuaVector<LuaString> list;
    if (!names(base, list))
    {
        why = what + " is a section or a list of them";
        return false;
    }
    for (LuaString& name : list)
    {
        name = lower(name);
        if (!plain_name(name))
        {
            why = what + " names a section no config can hold";
            return false;
        }
    }
    out = joined(list, ",");
    return true;
}

bool plan_identity(const Value& a, const LuaString& module, const LuaString& file, ItemPlan& p, LuaString& why)
{
    if (const Value* edit = a.get("edit"))
    {
        if (a.get("id") || a.get("section") || a.get("base") || a.get("abstract"))
            why = "a change names its section with edit alone: id, section, base and abstract make a new item";
        else if (edit->type != Value::String || !plain_name(lower(edit->s)))
            why = "edit names the section it changes";
        if (!why.empty())
            return false;
        p.edit = true;
        p.section = lower(edit->s);
        return true;
    }

    const Value* id = a.get("id");
    const Value* section = a.get("section");
    LuaString name;
    if (id)
        name = id->type == Value::String ? id->s : LuaString();
    else
    {
        // the file name is the id when the file names none, so a renamed file renames the item
        const size_t slash = file.find_last_of("\\/");
        name = slash == LuaString::npos ? file : file.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != LuaString::npos)
            name.erase(dot);
        name = lower(name);
        for (char& c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                c = '_';
    }
    // the id matters where it is written, or where it makes the section
    if ((id || !section) && !valid_id(name))
    {
        why = "the id is a-z, 0-9 and _, up to 48, not starting with a digit";
        return false;
    }
    if (section && section->type != Value::String)
    {
        why = "the section is a-z, 0-9, _, . and -, under 64 characters";
        return false;
    }
    p.section = section ? lower(section->s) : module + "_" + name;
    if (!section_name(p.section))
    {
        why = "the section [" + p.section + "] is not a-z, 0-9, _, . and -, under 64 characters";
        return false;
    }
    if (const Value* abstract_ = a.get("abstract"))
    {
        if (abstract_->type != Value::Bool)
        {
            why = "abstract is true or false";
            return false;
        }
        p.abstract_ = abstract_->b;
    }
    if (!parents_of(a.get("base"), p.parents, why, "base"))
        return false;
    if (p.parents.empty() && !p.abstract_)
    {
        why = "a new item starts from a base: write base (or edit, to change an item that exists)";
        return false;
    }
    return true;
}

bool plan_texts(const Value& a, ItemPlan& p, LuaString& why)
{
    const Value* name = a.get("name");
    const Value* short_name = a.get("short_name");
    const Value* description = a.get("description");
    for (const auto& [v, key] : {std::pair{name, "name"}, {short_name, "short_name"}, {description, "description"}})
        if (v && !text_value(*v))
        {
            why = LuaString(key) + " is a string or { language = text }";
            return false;
        }
    // the game's own string ids, so the item reads in every language its module names
    if (name)
    {
        p.texts.push_back({p.section + "_name", *name});
        p.own.push_back(key_op(LtxDirective::Set, "inv_name", p.section + "_name"));
    }
    if (short_name)
    {
        p.texts.push_back({p.section + "_short", *short_name});
        p.own.push_back(key_op(LtxDirective::Set, "inv_name_short", p.section + "_short"));
    }
    else if (name)
        p.own.push_back(key_op(LtxDirective::Set, "inv_name_short", p.section + "_name"));
    if (description)
    {
        p.texts.push_back({p.section + "_descr", *description});
        p.own.push_back(key_op(LtxDirective::Set, "description", p.section + "_descr"));
    }
    return true;
}

bool plan_icon(const Value& a, ItemPlan& p, LuaString& why)
{
    const Value* icon = a.get("icon");
    if (!icon)
        return true;
    // the authoring forms name a picture beside the asset; the module build bakes it into the game's form
    if (icon->type == Value::String || (icon->type == Value::Table && icon->get("file")))
    {
        p.icon_picture = true;
        return true;
    }
    const Value* x = icon->type == Value::Table ? icon->get("x") : nullptr;
    const Value* y = icon->type == Value::Table ? icon->get("y") : nullptr;
    if (!x || !y || x->type != Value::Number || y->type != Value::Number)
    {
        why = "icon is { texture, x, y, w, h }, in cells";
        return false;
    }
    if (const Value* texture = icon->get("texture"))
    {
        if (texture->type != Value::String || texture->s.empty() || !plain_value(texture->s))
        {
            why = "icon.texture names a texture";
            return false;
        }
        p.own.push_back(key_op(LtxDirective::Set, "icons_texture", texture->s));
    }
    else
        p.own.push_back(key_op(LtxDirective::RemoveKey, "icons_texture", LuaString()));
    p.own.push_back(key_op(LtxDirective::Set, "inv_grid_x", number_text(x->n)));
    p.own.push_back(key_op(LtxDirective::Set, "inv_grid_y", number_text(y->n)));
    for (const auto& [from, to] : {std::pair{"w", "inv_grid_width"}, {"h", "inv_grid_height"}})
        if (const Value* v = icon->get(from))
        {
            if (v->type != Value::Number)
            {
                why = LuaString("icon.") + from + " is a number of cells";
                return false;
            }
            p.own.push_back(key_op(LtxDirective::Set, to, number_text(v->n)));
        }
    return true;
}

bool plan_hud(const Value& a, ItemPlan& p, LuaString& why)
{
    const Value* hud = a.get("hud");
    if (!hud)
        return true;
    if (hud->type != Value::Table)
    {
        why = "hud is { base, set, unset, add, drop }";
        return false;
    }
    if (const Value* base = hud->get("base"))
    {
        if (p.edit)
        {
            why = "hud of a change changes the section the item's hud names: base belongs to a new item's hud";
            return false;
        }
        if (!parents_of(base, p.hud_parents, why, "hud.base"))
            return false;
        p.hud_parents_given = true;
    }
    if (!changes(*hud, p.hud_ops, why, "hud: "))
        return false;
    p.hud = true;
    if (!p.edit)
    {
        const Value* set = a.get("set");
        if (!(set && set->get("hud")))
            p.own.push_back(key_op(LtxDirective::Set, "hud", p.section + "_hud"));
    }
    return true;
}

bool plan_sections(const Value& a, ItemPlan& p, LuaString& why)
{
    const Value* list = a.get("sections");
    if (!list)
        return true;
    if (list->type != Value::Table || !list->keys.empty())
    {
        why = "sections is a list of { name, base, set } and { edit, set }";
        return false;
    }
    for (const Value& row : list->arr)
    {
        ItemSection s;
        const Value* name = row.type == Value::Table ? row.get("name") : nullptr;
        const Value* edit = row.type == Value::Table ? row.get("edit") : nullptr;
        if (!name == !edit || (name && name->type != Value::String) || (edit && edit->type != Value::String))
        {
            why = "a row of sections has a name (a section it makes) or edit (one it changes)";
            return false;
        }
        s.make = name != nullptr;
        s.name = lower(name ? name->s : edit->s);
        if (s.make ? !section_name(s.name) : !plain_name(s.name))
        {
            why = "the section [" + s.name + "] is not a-z, 0-9, _, . and -, under 64 characters";
            return false;
        }
        if (row.get("base") && !s.make)
        {
            why = "[" + s.name + "] exists and keeps its parents: base belongs to a section a row makes";
            return false;
        }
        if (!parents_of(row.get("base"), s.parents, why, "the base of [" + s.name + "]") ||
            !changes(row, s.ops, why, "[" + s.name + "]: "))
            return false;
        p.sections.push_back(s);
    }
    return true;
}

bool plan_scene(const Value& a, ItemPlan& p, Lists& out, LuaString& why)
{
    const Value* scene = a.get("scene");
    if (!scene)
        return true;
    const Value* t = scene->type == Value::Table ? scene : nullptr;
    if (!t && scene->type != Value::String)
    {
        why = "scene is the item whose scene it takes, or { like, sound, camera, time, helmet, fit, anm, hud_anm, hud, set }";
        return false;
    }
    const Value* like = t ? t->get("like") : scene;
    if (like && (like->type != Value::String || !plain_name(lower(like->s))))
    {
        why = "scene.like names the item whose scene it takes";
        return false;
    }
    const LuaString parents = like ? lower(like->s) : LuaString();
    LuaVector<LtxDirective> hud_ops;
    LuaString hud_parents;
    const Value* hud = t ? t->get("hud") : nullptr;
    if (hud && (hud->type != Value::Table || !parents_of(hud->get("base"), hud_parents, why, "scene.hud.base") ||
                   !changes(*hud, hud_ops, why, "scene.hud: ")))
    {
        if (why.empty())
            why = "scene.hud is { base, set, unset, add, drop }";
        return false;
    }
    // the scene's other keys, as its section holds them: what a module's own scripts read of it
    LuaVector<LtxDirective> own_ops;
    if (const Value* set = t ? t->get("set") : nullptr)
    {
        Value only;
        only.type = Value::Table;
        only.keys.push_back("set");
        only.vals.push_back(*set);
        if (!changes(only, own_ops, why, "scene: "))
            return false;
    }

    out.header(kScenes, p.section, parents);
    if (t)
    {
        // the keys the game's scene script reads, under the names an author writes
        for (const auto& [from, to] : {std::pair{"sound", "snd"}, {"camera", "cam"}, {"time", "tm"}, {"helmet", "helm"},
                 {"fit", "fit"}, {"anm", "anm"}, {"hud_anm", "hud_anm"}})
            if (const Value* v = t->get(from))
            {
                LuaString text;
                if (!ItemValueText(*v, text) || !plain_value(text))
                {
                    why = LuaString("scene.") + from + " is a value no config can carry";
                    return false;
                }
                out.line(LtxDirective::Set, to, text);
            }
        for (const LtxDirective& d : own_ops)
            out.line(d.op, d.key, d.value);
    }
    if (hud)
    {
        // the scene plays <anm>_hud of the system config
        const LuaString section = "item_ea_" + p.section + "_hud";
        if (!t->get("anm"))
            out.line(LtxDirective::Set, "anm", "item_ea_" + p.section);
        out.header(LuaString(), section, hud_parents);
        for (const LtxDirective& d : hud_ops)
            out.line(d.op, d.key, d.value);
    }
    return true;
}
} // namespace
} // namespace itemasset

namespace assetline
{
LuaString Lower(LuaString text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return text;
}

bool PlainName(const LuaString& name)
{
    if (name.empty() || name.size() > 255)
        return false;
    for (const char c : name)
        if (static_cast<unsigned char>(c) < 0x21 || c == 0x7f || std::strchr("[]=;,\"", c))
            return false;
    return true;
}

bool PlainValue(const LuaString& value)
{
    for (const char c : value)
        if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7f)
            return false;
    return true;
}

LuaString NumberText(double n)
{
    char buffer[64];
    if (n == std::floor(n) && std::fabs(n) < 1e15)
        std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(n));
    else
        std::snprintf(buffer, sizeof buffer, "%.15g", n);
    return buffer;
}

bool Names(const LuaValue* v, LuaVector<LuaString>& out)
{
    if (!v)
        return true;
    if (v->type == LuaValue::String)
    {
        out.push_back(v->s);
        return true;
    }
    if (v->type != LuaValue::Table || !v->keys.empty())
        return false;
    for (const LuaValue& x : v->arr)
    {
        if (x.type != LuaValue::String)
            return false;
        out.push_back(x.s);
    }
    return true;
}

void Lists::header(const LuaString& f, const LuaString& s, const LuaString& parents)
{
    LtxDirective d;
    d.op = LtxDirective::Section;
    d.block = ++block;
    d.file = file = f;
    d.section = section = s;
    d.parents = parents;
    out.push_back(d);
}

void Lists::line(LtxDirective::Op op, const LuaString& key, const LuaString& value)
{
    LtxDirective d;
    d.op = op;
    d.block = block;
    d.file = file;
    d.section = section;
    d.key = key;
    d.value = value;
    out.push_back(d);
}
} // namespace assetline

bool ItemValueText(const LuaValue& value, LuaString& out)
{
    switch (value.type)
    {
    case LuaValue::String: out = value.s; return true;
    case LuaValue::Number: out = itemasset::number_text(value.n); return true;
    case LuaValue::Bool: out = value.b ? "true" : "false"; return true;
    case LuaValue::Table:
    {
        if (!value.keys.empty())
            return false;
        out.clear();
        for (size_t i = 0; i < value.arr.size(); ++i)
        {
            LuaString part;
            if (value.arr[i].type == LuaValue::Table || !ItemValueText(value.arr[i], part))
                return false;
            if (i)
                out += ", ";
            out += part;
        }
        return true;
    }
    default: return false;
    }
}

bool PlanItem(const LuaValue& asset, const LuaString& module, const LuaString& file, ItemPlan& out, LuaString& why)
{
    using namespace itemasset;
    out = ItemPlan();
    out.file = file;
    why.clear();
    if (asset.type != LuaValue::Table)
    {
        why = "an item file returns one table";
        return false;
    }
    // what another file owns - a trader's shelf, the death loot, a recipe, a line of any config - is
    // not an item's to say: named, so the log can say where it belongs, and never read
    for (const char* key : {"sold", "loot", "craft", "lists"})
        if (asset.get(key))
            out.foreign.push_back(key);
    if (!plan_identity(asset, module, file, out, why) || !changes(asset, out.own, why, LuaString()))
        return false;
    // a template is its section and nothing else
    if (out.abstract_)
        return true;
    Lists lists{out.lists};
    return plan_texts(asset, out, why) && plan_icon(asset, out, why) && plan_hud(asset, out, why) &&
        plan_sections(asset, out, why) && plan_scene(asset, out, lists, why);
}
} // namespace XMS

#ifndef XMS_LUADATA_STANDALONE
namespace XMS
{
namespace itemasset
{
namespace
{
// No step yet: version 1 is the only one, and an item without the header is version 1.
constexpr int kItemVersion = 1;
const LuaDataStep* const kItemSteps = nullptr;

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

xr_vector<xr_string> split(const xr_string& csv)
{
    xr_vector<xr_string> out;
    for (size_t at = 0; at <= csv.size();)
    {
        const size_t comma = std::min(csv.find(',', at), csv.size());
        if (comma > at)
            out.emplace_back(csv.substr(at, comma - at));
        at = comma + 1;
    }
    return out;
}

// One section's changes, under a header that makes it (with `parents`) where it is missing.
void apply_to(CInifile& ini, const Module& m, const xr_string& section, const xr_string& parents,
    const xr_vector<LtxDirective>& ops, bool ledger)
{
    xr_vector<LtxDirective> list;
    list.reserve(ops.size() + 1);
    LtxDirective& header = list.emplace_back();
    header.op = LtxDirective::Section;
    header.block = 1;
    header.section = section;
    header.parents = parents;
    for (const LtxDirective& op : ops)
    {
        LtxDirective& d = list.emplace_back(op);
        d.block = 1;
        d.file.clear();
        d.section = section;
        d.key = GameText(d.key);
        d.value = GameText(d.value);
    }
    xr_vector<const LtxDirective*> order;
    order.reserve(list.size());
    for (const LtxDirective& d : list)
        order.push_back(&d);
    DirectiveCounts counts;
    ApplyDirectives(ini, order.data(), order.size(), m.id.c_str(), m.layer, ledger, counts);
}

xr_string value_of(const CInifile& ini, const xr_string& section, pcstr key)
{
    if (!ini.line_exist(section.c_str(), key))
        return xr_string();
    const pcstr value = ini.r_string(section.c_str(), key);
    return value ? value : "";
}

// The hud a change's item names once its own `set` is applied.
xr_string hud_of_change(const CInifile& ini, const ItemPlan& p)
{
    xr_string hud = value_of(ini, p.section, "hud");
    for (const LtxDirective& d : p.own)
        if (d.op == LtxDirective::Set && d.key == "hud")
            hud = d.value;
    return hud;
}

enum class Ready
{
    Go,
    Wait, // a base is not there yet: another item of the module may make it
    Refuse,
};

Ready ready(const CInifile& ini, const ItemPlan& p, xr_string& why)
{
    const auto missing_base = [&](const xr_string& parents, pcstr of) -> bool
    {
        for (const xr_string& base : split(parents))
            if (!ini.section_exist(base.c_str()))
            {
                why = "an unknown base [" + base + "]" + of;
                return true;
            }
        return false;
    };
    if (!p.edit)
    {
        if (ini.section_exist(p.section.c_str()))
        {
            why = "the section [" + p.section + "] exists already: write it as a change, with edit";
            return Ready::Refuse;
        }
        if (p.hud && ini.section_exist((p.section + "_hud").c_str()))
        {
            why = "the section [" + p.section + "_hud] exists already";
            return Ready::Refuse;
        }
    }
    else if (!ini.section_exist(p.section.c_str()))
    {
        why = "it changes [" + p.section + "], which is not there";
        return Ready::Refuse;
    }
    else if (p.hud)
    {
        const xr_string hud = hud_of_change(ini, p);
        if (hud.empty() || !ini.section_exist(hud.c_str()))
        {
            why = "it changes the hud of [" + p.section + "], which names no hud section";
            return Ready::Refuse;
        }
    }
    for (const ItemSection& s : p.sections)
    {
        if (s.make && (ini.section_exist(s.name.c_str()) || s.name == p.section))
        {
            why = "the section [" + s.name + "] exists already: change it with { edit = \"" + s.name + "\" }";
            return Ready::Refuse;
        }
        if (!s.make && !ini.section_exist(s.name.c_str()))
        {
            why = "it changes [" + s.name + "], which is not there";
            return Ready::Refuse;
        }
    }
    if (!p.edit && missing_base(p.parents, ""))
        return Ready::Wait;
    if (p.hud && p.hud_parents_given && missing_base(p.hud_parents, " of its hud"))
        return Ready::Wait;
    // a row may start from what the item itself makes before it: its own section, its hud, an earlier
    // row - a scope variant of a new weapon is written in the weapon's own file
    xr_vector<xr_string> made;
    if (!p.edit)
    {
        made.push_back(p.section);
        if (p.hud)
            made.push_back(p.section + "_hud");
    }
    for (const ItemSection& s : p.sections)
    {
        if (!s.make)
            continue;
        for (const xr_string& base : split(s.parents))
            if (!ini.section_exist(base.c_str()) && std::find(made.begin(), made.end(), base) == made.end())
            {
                why = "an unknown base [" + base + "] of [" + s.name + "]";
                return Ready::Wait;
            }
        made.push_back(s.name);
    }
    return Ready::Go;
}

void apply(CInifile& ini, const Module& m, const ItemPlan& p)
{
    if (!p.edit)
    {
        // the hud the bases name, read before the item's own keys may name another
        xr_string base_hud;
        for (const xr_string& base : split(p.parents))
            if (const xr_string hud = value_of(ini, base, "hud"); !hud.empty())
                base_hud = hud;
        apply_to(ini, m, p.section, p.parents, p.own, false);
        ini.xms_mark_item_section(p.section.c_str());
        if (p.hud)
        {
            const xr_string hud = p.section + "_hud";
            const xr_string parents =
                p.hud_parents_given ? p.hud_parents : (ini.section_exist(base_hud.c_str()) ? base_hud : xr_string());
            apply_to(ini, m, hud, parents, p.hud_ops, false);
            ini.xms_mark_item_section(hud.c_str());
        }
    }
    else
    {
        apply_to(ini, m, p.section, xr_string(), p.own, true);
        if (p.hud)
            apply_to(ini, m, value_of(ini, p.section, "hud"), xr_string(), p.hud_ops, true);
    }
    for (const ItemSection& s : p.sections)
    {
        apply_to(ini, m, s.name, s.make ? s.parents : xr_string(), s.ops, !s.make);
        if (s.make)
            ini.xms_mark_item_section(s.name.c_str());
    }
}

void publish_texts(const Module& m, const xr_vector<ItemText>& texts)
{
    if (texts.empty())
        return;
    // one table per language anything is named in, and always the game's two: a language an item
    // says nothing in falls back to its Russian, so no string is ever missing
    xr_vector<xr_string> langs{"rus", "eng"};
    for (const ItemText& t : texts)
        LanguagesOf(t.text, langs);
    for (const xr_string& lang : langs)
    {
        xr_string st = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n\r\n<string_table>\r\n";
        for (const ItemText& t : texts)
            st += "\t<string id=\"" + XmlText(t.id) + "\">\r\n\t\t<text>" + XmlText(TextIn(t.text, lang.c_str())) +
                "</text>\r\n\t</string>\r\n";
        st += "</string_table>\r\n";
        PublishGenerated("configs\\text\\" + lang + "\\" + m.id + "_items.xml", st);
    }
}
} // namespace
} // namespace itemasset

bool BuildItems(const Module& m, CInifile& settings, xr_vector<LtxDirective>& lists)
{
    using namespace itemasset;
    xr_vector<xr_string> files;
    ListFilesRelative(m.root.c_str(), "*.itemasset", true, files);
    if (files.empty())
        return false;

    xr_vector<ItemPlan> fresh, changes;
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
                error.empty() ? "an item file returns one table" : error.c_str());
            continue;
        }
        if (!UpgradeLuaData(value, "item", kItemVersion, kItemSteps, false, error))
        {
            Msg("! %s\\%s: %s", m.id.c_str(), file.c_str(), error.c_str());
            continue;
        }
        ItemPlan plan;
        if (!PlanItem(value, m.id, file, plan, error))
        {
            Msg("! XMS: [%s] %s: left out - %s", m.id.c_str(), file.c_str(), error.c_str());
            continue;
        }
        (plan.edit ? changes : fresh).emplace_back(std::move(plan));
    }

    u32 made = 0, changed = 0;
    xr_vector<ItemText> texts;
    unsigned blocks = 0;
    for (const LtxDirective& d : lists)
        blocks = std::max(blocks, d.block);
    const auto finish = [&](const ItemPlan& p)
    {
        apply(settings, m, p);
        if (p.icon_picture)
            Msg("! XMS: [%s] %s: the icon names a picture, which only a built module turns into a texture - the item "
                "keeps the icon it has",
                m.id.c_str(), p.file.c_str());
        for (const xr_string& key : p.foreign)
            Msg("! XMS: [%s] %s: '%s' is not an item's to say and is not read - a trader's stock is a .tradeasset, "
                "the death loot a .lootasset, a recipe a .recipeasset, a line of another config a .ltxp",
                m.id.c_str(), p.file.c_str(), key.c_str());
        texts.insert(texts.end(), p.texts.begin(), p.texts.end());
        // every plan numbers its blocks from 1; side by side they stay apart
        const unsigned base = blocks;
        for (const LtxDirective& d : p.lists)
        {
            LtxDirective& kept = lists.emplace_back(d);
            kept.block = d.block ? base + d.block : 0;
            kept.key = GameText(kept.key);
            kept.value = GameText(kept.value);
            blocks = std::max(blocks, kept.block);
        }
        ++(p.edit ? changed : made);
    };

    // new sections first: a base may be another new item of the module, so each waits for its bases
    xr_string why;
    for (bool progress = true; progress && !fresh.empty();)
    {
        progress = false;
        for (size_t i = 0; i < fresh.size();)
        {
            const Ready state = ready(settings, fresh[i], why);
            if (state == Ready::Wait)
            {
                ++i;
                continue;
            }
            if (state == Ready::Refuse)
                Msg("! XMS: [%s] %s: left out - %s", m.id.c_str(), fresh[i].file.c_str(), why.c_str());
            else
                finish(fresh[i]);
            fresh.erase(fresh.begin() + i);
            progress = true;
        }
    }
    for (const ItemPlan& p : fresh)
    {
        ready(settings, p, why);
        Msg("! XMS: [%s] %s: left out - %s", m.id.c_str(), p.file.c_str(), why.c_str());
    }
    // then the changes, in the order of their paths
    for (const ItemPlan& p : changes)
    {
        if (ready(settings, p, why) == Ready::Go)
            finish(p);
        else
            Msg("! XMS: [%s] %s: left out - %s", m.id.c_str(), p.file.c_str(), why.c_str());
    }

    publish_texts(m, texts);
    if (made + changed)
        Msg("* XMS: [%s] %u item(s) read from the module (%u new, %u change(s))", m.id.c_str(), made + changed, made,
            changed);
    return made + changed > 0;
}
} // namespace XMS
#endif
