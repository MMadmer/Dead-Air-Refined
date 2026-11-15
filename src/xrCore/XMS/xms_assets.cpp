// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_assets.h"
#include "xms_core.h"
#include "xms_luadata.h"
#include "xrCore/Animation/Motion.hpp"

namespace XMS::Assets
{
namespace
{
// Version 1 is the only one of each kind so far: the chains are empty, and a step for version v
// goes at index v - 1 when version v + 1 comes.
constexpr int kTextureVersion = 1, kSoundVersion = 1, kModelVersion = 1;
const LuaDataStep* const kTextureSteps = nullptr;
const LuaDataStep* const kSoundSteps = nullptr;
const LuaDataStep* const kModelSteps = nullptr;

struct Registry
{
    xr_map<xr_string, Texture> textures;
    xr_map<xr_string, Sound> sounds;
    xr_map<xr_string, Model> models;
};

Registry& registry()
{
    static Registry state;
    return state;
}

// The game's name of a thing: lower case, back slashes, no leading slash.
xr_string normal(const xr_string& text)
{
    xr_string out = text;
    for (char& c : out)
        c = c == '/' ? '\\' : char(tolower(u8(c)));
    const size_t first = out.find_first_not_of('\\');
    return first == xr_string::npos ? xr_string() : out.substr(first);
}

xr_string without_extension(const xr_string& path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('\\');
    return dot != xr_string::npos && (slash == xr_string::npos || dot > slash) ? path.substr(0, dot) : path;
}

bool read_text(const xr_string& path, xr_string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
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

// ---- the kinds ------------------------------------------------------------------

void parse_texture(const LuaValue& v, Texture& t)
{
    const xr_string use = normal(v.text("use"));
    t.described = use != "ui" && use != "cube";

    if (const LuaValue* bump = v.get("bump"))
    {
        if (bump->type == LuaValue::Bool)
            t.bump = bump->b ? 1 : 0;
        else if (bump->type == LuaValue::Table)
        {
            t.bump = 1;
            t.bump_name = normal(bump->text("texture"));
            t.parallax = bump->flag("parallax", false);
        }
    }

    if (const LuaValue* detail = v.get("detail"))
    {
        if (detail->type == LuaValue::Bool && !detail->b)
            t.detail = 0;
        else if (detail->type == LuaValue::Table && !detail->text("texture").empty())
        {
            t.detail = 1;
            t.detail_name = normal(detail->text("texture"));
            t.detail_scale = float(detail->number("scale", 1.0));
            // left out it is laid on the colour; "none" names it and lays it on nothing
            const xr_string on = normal(detail->text("on"));
            t.detail_diffuse = on != "bump" && on != "none";
            t.detail_bump = on == "bump" || on == "both";
        }
    }

    // A .thm names a pair of lighting models and a weight between them; the description names the
    // first of the pair, the weight moves towards the next.
    if (const LuaValue* material = v.get("material"))
    {
        constexpr pcstr pairs[] = {"oren_nayar", "blinn", "phong", "metal"};
        float base = -1.f;
        if (material->type == LuaValue::Number)
            base = float(material->n);
        else if (material->type == LuaValue::String)
            for (u32 i = 0; i < std::size(pairs); ++i)
                if (normal(material->s) == pairs[i])
                    base = float(i);
        if (base >= 0.f)
        {
            t.has_material = true;
            t.material = base + (material->type == LuaValue::String ? clampr(float(v.number("weight", 0.0)), 0.f, 1.f) : 0.f);
        }
    }
    else if (const LuaValue* weight = v.get("weight"); weight && weight->type == LuaValue::Number)
    {
        // a weight alone moves from the model a texture nobody describes has: "blinn"
        t.has_material = true;
        t.material = 1.f + clampr(float(weight->n), 0.f, 1.f);
    }
}

void parse_sound(const LuaValue& v, Sound& s)
{
    for (auto [key, has, value] : {std::tuple{"min_distance", &s.has_min, &s.min_distance},
             {"max_distance", &s.has_max, &s.max_distance}, {"volume", &s.has_volume, &s.volume},
             {"ai_distance", &s.has_ai, &s.ai_distance}})
        if (const LuaValue* x = v.get(key); x && x->type == LuaValue::Number)
        {
            *has = true;
            *value = float(x->n);
        }
    if (const LuaValue* type = v.get("type"))
    {
        if (type->type == LuaValue::String)
            s.type.push_back(normal(type->s));
        for (const LuaValue& word : type->arr)
            if (word.type == LuaValue::String)
                s.type.push_back(normal(word.s));
        s.has_type = type->type == LuaValue::String || type->type == LuaValue::Table;
    }
}

void parse_model(const LuaValue& v, Model& m)
{
    if (const LuaValue* surfaces = v.get("surfaces"))
        for (const LuaValue& row : surfaces->arr)
        {
            Surface s;
            if (row.type == LuaValue::Table)
            {
                s.texture = normal(row.text("texture"));
                s.shader = normal(row.text("shader"));
            }
            m.surfaces.push_back(s);
        }

    if (const LuaValue* bones = v.get("bones"))
        for (const LuaValue& row : bones->arr)
        {
            if (row.type != LuaValue::Table || row.text("name").empty())
                continue;
            Bone b;
            b.name = row.text("name");
            b.shape = normal(row.text("shape"));
            // a mass the physics can take: a number above zero, not an infinity
            if (const LuaValue* mass = row.get("mass");
                mass && mass->type == LuaValue::Number && std::isfinite(float(mass->n)) && mass->n > 0.0)
            {
                b.has_mass = true;
                b.mass = float(mass->n);
            }
            b.material = normal(row.text("material"));
            m.bones.push_back(b);
        }

    if (const LuaValue* animations = v.get("animations"))
        for (const LuaValue& row : animations->arr)
        {
            if (row.type != LuaValue::Table || row.text("name").empty())
                continue;
            Animation a;
            a.name = normal(row.text("name"));
            if (const LuaValue* speed = row.get("speed"); speed && speed->type == LuaValue::Number)
            {
                a.has_speed = true;
                a.speed = float(speed->n);
            }
            // `loop` is the motion's stop-at-end flag the other way round
            if (const LuaValue* loop = row.get("loop"); loop && loop->type == LuaValue::Bool)
                (loop->b ? a.clear : a.set) |= u16(esmStopAtEnd);
            for (auto [key, bit] : {std::pair{"no_mix", esmNoMix}, {"sync_part", esmSyncPart},
                     {"foot_steps", esmUseFootSteps}, {"root_mover", esmRootMover}, {"idle", esmIdle},
                     {"weapon_bone", esmUseWeaponBone}})
                if (const LuaValue* flag = row.get(key); flag && flag->type == LuaValue::Bool)
                    (flag->b ? a.set : a.clear) |= u16(bit);
            m.animations.push_back(a);
        }

    if (const LuaValue* userdata = v.get("userdata"); userdata && userdata->type == LuaValue::String)
    {
        m.has_userdata = true;
        m.userdata = userdata->s;
    }
}

// Reads every description of one kind in one module into `into`; the number read.
template <class T, class Parse>
u32 read_kind(const Module& m, pcstr folder, pcstr mask, pcstr header, int version, const LuaDataStep* steps,
    xr_map<xr_string, T>& into, Parse parse)
{
    const xr_string dir = m.root + "gamedata\\" + folder + "\\";
    xr_vector<xr_string> files;
    ListFilesRelative(dir.c_str(), mask, true, files);
    u32 count = 0;
    for (const xr_string& rel : files)
    {
        const xr_string label = m.id + "\\gamedata\\" + folder + "\\" + rel;
        xr_string text, error;
        LuaValue value;
        if (!read_text(dir + rel, text))
        {
            Msg("! %s: cannot be read", label.c_str());
            continue;
        }
        if (!ReadLuaData(text.c_str(), text.size(), value, error) || value.type != LuaValue::Table)
        {
            Msg("! %s: %s", label.c_str(), error.empty() ? "a description returns one table" : error.c_str());
            continue;
        }
        if (!UpgradeLuaData(value, header, version, steps, true, error))
        {
            Msg("! %s: %s", label.c_str(), error.c_str());
            continue;
        }
        xr_string name = normal(value.text("name"));
        if (name.empty())
            name = normal(without_extension(rel));
        T item;
        item.file = label;
        parse(value, item);
        into[name] = std::move(item);
        ++count;
    }
    return count;
}

template <class T>
const T* find(const xr_map<xr_string, T>& map, pcstr name)
{
    if (map.empty() || !name || !*name)
        return nullptr;
    const auto it = map.find(normal(name));
    return it == map.end() ? nullptr : &it->second;
}
} // namespace

void Build()
{
    Registry& r = registry();
    u32 described_by = 0;
    for (const Module& m : Modules())
    {
        if (m.disabled)
            continue;
        const u32 count = read_kind(m, "textures", "*.texasset", "tex", kTextureVersion, kTextureSteps, r.textures,
                              parse_texture) +
            read_kind(m, "sounds", "*.sndasset", "snd", kSoundVersion, kSoundSteps, r.sounds, parse_sound) +
            read_kind(m, "meshes", "*.meshasset", "mesh", kModelVersion, kModelSteps, r.models, parse_model);
        described_by += count ? 1 : 0;
    }
    if (described_by)
        Msg("* [assets] %zu texture(s), %zu sound(s), %zu model(s) described by %u module(s)", r.textures.size(),
            r.sounds.size(), r.models.size(), described_by);
}

const Texture* FindTexture(pcstr name) { return find(registry().textures, name); }
const Sound* FindSound(pcstr name) { return find(registry().sounds, name); }
const Model* FindModel(pcstr name) { return find(registry().models, name); }
const xr_map<xr_string, Texture>& Textures() { return registry().textures; }

const Surface* FindSurface(pcstr visual_name, bool whole, const Model** model)
{
    if (registry().models.empty() || !visual_name)
        return nullptr;
    xr_string name = visual_name;
    size_t index = 1;
    if (const size_t colon = name.find(':'); colon != xr_string::npos)
    {
        // "<model>:<n>" is a child; a level of detail ("<model>:lod:1") or a child's child is no surface of it
        const xr_string number = name.substr(colon + 1);
        if (number.empty() || number.find_first_not_of("0123456789") != xr_string::npos)
            return nullptr;
        index = size_t(atoi(number.c_str()));
        name.resize(colon);
    }
    else if (!whole)
        return nullptr;
    const Model* found = FindModel(without_extension(normal(name)).c_str());
    if (!found || !index || index > found->surfaces.size())
        return nullptr;
    if (model)
        *model = found;
    return &found->surfaces[index - 1];
}
} // namespace XMS::Assets
