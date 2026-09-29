// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_profile.h"
#include "da_ai_npc.h"

#include "ai/stalker/ai_stalker.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "character_community.h"
#include "character_rank.h"
#include "CustomOutfit.h"
#include "EntityCondition.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryOwner.h"
#include "Weapon.h"
#include "xrServerEntities/xrServer_Objects_ALife_Monsters.h"

#include <cmath>

namespace DaAi
{
namespace
{
constexpr pcstr default_section = "ai_profile_default";

constexpr pcstr weapon_class_names[]{"none", "pistol", "smg", "shotgun", "rifle_old", "rifle", "mg", "dmr", "launcher"};
constexpr pcstr armour_class_names[]{"none", "light", "heavy", "exo"};
constexpr pcstr voice_names[]{"auto", "loud", "hushed", "none"};
constexpr pcstr rout_style_names[]{"orderly", "scatter", "relocate", "never"};
constexpr pcstr search_style_names[]{"pairs", "single", "straight", "none"};
constexpr pcstr investigate_names[]{"pair", "single", "none", "advance"};
constexpr pcstr role_names[]{"opportunist", "suppressor", "flanker", "grenadier", "rearguard"};
static_assert(std::size(weapon_class_names) == size_t(WeaponClass::count));
static_assert(std::size(armour_class_names) == size_t(ArmourClass::count));
static_assert(std::size(role_names) == size_t(Role::count));

// Compiled-in bands (the plan's values); [ai_weapon_classes] overrides them.
constexpr WeaponBand compiled_weapon_bands[size_t(WeaponClass::count)]{
    {0.f, 0.f, 0.f}, // none
    {25.f, 40.f, 0.f}, // pistol
    {60.f, 100.f, 0.f}, // smg
    {20.f, 35.f, 0.f}, // shotgun
    {200.f, 300.f, 10.f}, // rifle_old
    {200.f, 300.f, 0.f}, // rifle
    {400.f, 500.f, 10.f}, // mg
    {600.f, 800.f, 30.f}, // dmr
    {200.f, 300.f, 30.f}, // launcher
};

// [ai_weapon_class_rules]. Without the section every firearm is a rifle: the names of slots and ammo families are
// data, never code.
struct ClassRules
{
    xr_vector<int> pistol_slots;
    xr_vector<shared_str> none_ammo;
    xr_vector<shared_str> launcher_ammo;
    xr_vector<shared_str> shotgun_ammo;
    xr_vector<shared_str> smg_ammo;
    u32 mg_mag{45};
    float dmr_zoom{4.f};
    u32 dmr_max_mag{20}; // 0 = any magazine
    float scoped_range_mult{1.33f};
    float slug_engage_max{60.f};
};

// What the rules decide for a weapon section; the scope rule still depends on the carried item.
struct SectionClass
{
    WeaponClass base{WeaponClass::rifle};
    bool scope_rule{}; // no earlier rule decided and the magazine fits a marksman rifle
    bool scope_attachable{};
    float scope_zoom{}; // built-in scope
};

struct Chain
{
    shared_str names[Profile::max_chain];
    u8 size{};

    void add(pcstr section)
    {
        if (!section || !section[0] || size == Profile::max_chain || !pSettings->section_exist(section))
            return;
        for (u8 index = 0; index < size; ++index)
        {
            if (!xr_strcmp(names[index], section))
                return;
        }
        names[size++] = section;
    }

    void add_banded(pcstr base, u8 band)
    {
        string128 name;
        xr_sprintf(name, "%s_r%u", base, u32(band));
        add(name);
        add(base);
    }
};

WeaponBand g_weapon_bands[size_t(WeaponClass::count)];
ClassRules g_rules;
float g_armour_thresholds[3]{.15f, .35f, .55f}; // light, heavy, exo
bool g_config_loaded{};

// Level caches: NpcState keeps pointers into g_profiles (node map, stable addresses).
xr_node_hash_map<shared_str, Profile> g_profiles;
xr_flat_hash_map<shared_str, SectionClass> g_section_classes;
xr_flat_hash_map<shared_str, float> g_scope_zooms;
// Keyed by the interned ammo name the weapon holds; the entry pins that string, so its address stays unique.
xr_flat_hash_map<pcstr, std::pair<shared_str, bool>> g_slug_ammo;
xr_flat_hash_map<shared_str, u8> g_outfit_armour; // override, or count when none matches
xr_flat_hash_map<shared_str, u8> g_visual_armour; // ArmourClass, or count when no override matches
// Session: one log line per community without a profile section.
xr_flat_hash_map<shared_str, bool> g_logged_communities;

Profile make_default_profile()
{
    Profile result;
    result.section = default_section;
    result.chain[0] = default_section;
    result.chain_size = 1;
    return result;
}

void ensure_config()
{
    if (!g_config_loaded)
        load_profile_config();
}

CSE_ALifeDynamicObject* server_object(u16 id)
{
    return ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
}

shared_str rank_name(int rank) { return CHARACTER_RANK::IndexToId(CHARACTER_RANK::ValueToIndex(rank), nullptr, true); }

bool starts_with(pcstr text, pcstr prefix) { return !strncmp(text, prefix, xr_strlen(prefix)); }

// The item of the longest key of a section that prefixes text; with whole_word the key must equal text or be
// followed by '_' in it (wpn_mosin covers wpn_mosin_short, not wpn_mosinka).
const CInifile::Item* longest_prefix(pcstr section, pcstr text, bool whole_word)
{
    if (!text || !text[0] || !pSettings->section_exist(section))
        return nullptr;

    const CInifile::Item* best = nullptr;
    size_t best_length = 0;
    for (const CInifile::Item& item : pSettings->r_section(section).Data)
    {
        const size_t length = item.first.size();
        if (length <= best_length || strncmp(text, item.first.c_str(), length))
            continue;
        if (whole_word && text[length] && text[length] != '_')
            continue;
        best = &item;
        best_length = length;
    }
    return best;
}

template <typename E, size_t N>
bool parse_enum(pcstr text, const pcstr (&names)[N], E& out)
{
    for (size_t index = 0; index < N; ++index)
    {
        if (!xr_strcmp(names[index], text))
        {
            out = E(index);
            return true;
        }
    }
    return false;
}

bool parse_value(pcstr text, float& out)
{
    char* end = nullptr;
    const float value = strtof(text, &end);
    if (end == text || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

bool parse_value(pcstr text, Range& out)
{
    const int count = _GetItemCount(text);
    string64 item;
    Range result;
    if (!count || !parse_value(_GetItem(text, 0, item), result.lo))
        return false;
    result.hi = result.lo;
    if (count > 1 && !parse_value(_GetItem(text, 1, item), result.hi))
        return false;
    if (result.hi < result.lo)
        std::swap(result.lo, result.hi);
    out = result;
    return true;
}

// Four values by band; a shorter list repeats its last value.
bool parse_value(pcstr text, float (&out)[4])
{
    const int count = _GetItemCount(text);
    if (!count)
        return false;
    float result[4];
    string64 item;
    for (int band = 0; band < 4; ++band)
    {
        if (!parse_value(_GetItem(text, _min(band, count - 1), item), result[band]))
            return false;
    }
    std::copy(std::begin(result), std::end(result), std::begin(out));
    return true;
}

bool parse_value(pcstr text, u8& out)
{
    float value;
    if (!parse_value(text, value) || value < 0.f || value > 255.f)
        return false;
    out = u8(value + .5f);
    return true;
}

bool parse_value(pcstr text, bool& out)
{
    if (CInifile::isBool(text))
    {
        out = true;
        return true;
    }
    if (!xr_strcmp(text, "off") || !xr_strcmp(text, "no") || !xr_strcmp(text, "false"))
    {
        out = false;
        return true;
    }
    float value;
    if (!parse_value(text, value))
        return false;
    out = value != 0.f;
    return true;
}

bool parse_value(pcstr text, Voice& out) { return parse_enum(text, voice_names, out); }
bool parse_value(pcstr text, RoutStyle& out) { return parse_enum(text, rout_style_names, out); }
bool parse_value(pcstr text, SearchStyle& out) { return parse_enum(text, search_style_names, out); }
bool parse_value(pcstr text, Investigate& out) { return parse_enum(text, investigate_names, out); }

void format_value(pstr out, size_t size, float value) { xr_sprintf(out, size, "%g", value); }
void format_value(pstr out, size_t size, const Range& value) { xr_sprintf(out, size, "%g, %g", value.lo, value.hi); }
void format_value(pstr out, size_t size, const float (&value)[4])
{
    xr_sprintf(out, size, "%g, %g, %g, %g", value[0], value[1], value[2], value[3]);
}
void format_value(pstr out, size_t size, u8 value) { xr_sprintf(out, size, "%u", u32(value)); }
void format_value(pstr out, size_t size, bool value) { xr_strcpy(out, size, value ? "1" : "0"); }
void format_value(pstr out, size_t size, Voice value) { xr_strcpy(out, size, voice_names[size_t(value)]); }
void format_value(pstr out, size_t size, RoutStyle value) { xr_strcpy(out, size, rout_style_names[size_t(value)]); }
void format_value(pstr out, size_t size, SearchStyle value)
{
    xr_strcpy(out, size, search_style_names[size_t(value)]);
}
void format_value(pstr out, size_t size, Investigate value) { xr_strcpy(out, size, investigate_names[size_t(value)]); }

// Every plain key of 2.3.3; peek, aggr_winning, roles and grenade_issue have their own rules.
template <typename P, typename F>
void for_each_key(P& p, F&& f)
{
    f("react_ms", p.react_ms);
    f("react_idle_ms", p.react_idle_ms);
    f("settle_s", p.settle_s);
    f("detect_rate", p.detect_rate);
    f("hear_skill", p.hear_skill);
    f("hear_gain", p.hear_gain);
    f("aim_head", p.aim_head);
    f("lead_error", p.lead_error);
    f("light_discipline", p.light_discipline);
    f("radio_chance", p.radio_chance);
    f("radio_leader_bonus", p.radio_leader_bonus);
    f("hq_relay", p.hq_relay);
    f("hq_delay_s", p.hq_delay_s);
    f("voice", p.voice);
    f("gestures", p.gestures);
    f("pda", p.pda);
    f("decide_ms", p.decide_ms);
    f("understand_ms", p.understand_ms);
    f("callouts", p.callouts);
    f("aggr", p.aggr);
    f("winning_ratio", p.winning_ratio);
    f("cover", p.cover);
    f("flank", p.flank);
    f("supp", p.supp);
    f("supp_window_s", p.supp_window_s);
    f("gren", p.gren);
    f("bounding", p.bounding);
    f("fire_on_order", p.fire_on_order);
    f("return_fire_m", p.return_fire_m);
    f("flank_fire", p.flank_fire);
    f("courage", p.courage);
    f("break_cas", p.break_cas);
    f("break_ratio", p.break_ratio);
    f("leader_dead", p.leader_dead);
    f("rout_style", p.rout_style);
    f("surrender", p.surrender);
    f("recover_s", p.recover_s);
    f("fanatic", p.fanatic);
    f("stamina", p.stamina);
    f("search_delay_s", p.search_delay_s);
    f("search_s", p.search_s);
    f("search_style", p.search_style);
    f("investigate", p.investigate);
    f("quiet_search", p.quiet_search);
    f("giveup_stagger_s", p.giveup_stagger_s);
}

// The first section of the chain that has the key; an empty value comes back as "".
bool find_value(CInifile::Sect* const* sections, u8 count, pcstr key, pcstr& value)
{
    for (u8 index = 0; index < count; ++index)
    {
        if (sections[index]->line_exist(key, &value))
        {
            if (!value)
                value = "";
            return true;
        }
    }
    return false;
}

u32 parse_roles(pcstr text, pcstr section)
{
    u32 result = 0;
    string64 item;
    for (int index = 0, count = _GetItemCount(text); index < count; ++index)
    {
        Role role;
        if (parse_enum(_GetItem(text, index, item), role_names, role))
            result |= 1u << u32(role);
        else
            Msg("! [ai] profile %s: unknown role '%s'", section, item);
    }
    // Everybody can at least fight on his own.
    return result ? result : 1u << u32(Role::opportunist);
}

Profile build_profile(const Chain& chain, float rank_scale)
{
    Profile result;
    result.section = chain.size ? chain.names[0] : shared_str(default_section);
    result.chain_size = chain.size;
    std::copy(std::begin(chain.names), std::begin(chain.names) + chain.size, std::begin(result.chain));

    CInifile::Sect* sections[Profile::max_chain];
    for (u8 index = 0; index < chain.size; ++index)
        sections[index] = &pSettings->r_section(chain.names[index]);

    const pcstr head = result.section.c_str();
    for_each_key(result, [&](pcstr key, auto& value) {
        pcstr text;
        if (find_value(sections, chain.size, key, text) && text[0] && !parse_value(text, value))
            Msg("! [ai] profile %s: bad value '%s' of %s", head, text, key);
    });

    pcstr text;
    result.peek = result.cover;
    if (find_value(sections, chain.size, "peek", text) && text[0] && !parse_value(text, result.peek))
        Msg("! [ai] profile %s: bad value '%s' of peek", head, text);
    result.aggr_winning = result.aggr;
    if (find_value(sections, chain.size, "aggr_winning", text) && text[0] &&
        !parse_value(text, result.aggr_winning))
    {
        Msg("! [ai] profile %s: bad value '%s' of aggr_winning", head, text);
    }
    if (find_value(sections, chain.size, "roles", text))
        result.roles = parse_roles(text, head);
    if (find_value(sections, chain.size, "grenade_issue", text) && text[0])
    {
        string128 item;
        result.grenade_item = _GetItem(text, 0, item);
        float count = 0.f;
        float chance = 0.f;
        if ((_GetItemCount(text) > 1 && !parse_value(_GetItem(text, 1, item), count)) ||
            (_GetItemCount(text) > 2 && !parse_value(_GetItem(text, 2, item), chance)) || count < 0.f)
        {
            Msg("! [ai] profile %s: bad value '%s' of grenade_issue", head, text);
            result.grenade_item = nullptr;
        }
        else
        {
            result.grenade_count = u32(count + .5f);
            result.grenade_chance = std::clamp(chance, 0.f, 1.f);
        }
    }

    result.rank_scale = rank_scale;
    result.react_ms.lo *= rank_scale;
    result.react_ms.hi *= rank_scale;
    result.settle_s *= rank_scale;
    return result;
}

// Longest [ai_profile_by_character] prefix of the specific character id, else of the character profile id.
shared_str character_section(const CAI_Stalker& npc)
{
    const auto* trader = smart_cast<const CSE_ALifeTraderAbstract*>(server_object(npc.ID()));
    if (!trader)
        return nullptr;
    for (const shared_str& id : {trader->m_SpecificCharacter, trader->m_sCharacterProfile})
    {
        if (const CInifile::Item* item = longest_prefix("ai_profile_by_character", id.c_str(), false))
            return item->second;
    }
    return nullptr;
}

const Profile& find_profile(const CAI_Stalker& npc, const NpcState& state)
{
    shared_str community = CHARACTER_COMMUNITY::IndexToId(state.community, nullptr, true);
    if (community.size() && pSettings->line_exist("ai_community_alias", community))
    {
        const pcstr alias = pSettings->r_string("ai_community_alias", community.c_str());
        if (alias && alias[0])
            community = alias;
    }

    Chain chain;
    if (const shared_str character = character_section(npc); character.size())
        chain.add_banded(character.c_str(), state.band);
    if (community.size())
    {
        string128 name;
        xr_sprintf(name, "ai_profile_%s", community.c_str());
        chain.add_banded(name, state.band);
    }
    const bool own = chain.size > 0;
    chain.add_banded(default_section, state.band);

    const shared_str rank = rank_name(state.rank);
    float rank_scale = 1.f;
    if (rank.size() && pSettings->line_exist("ai_rank_scale", rank))
    {
        rank_scale = pSettings->r_float("ai_rank_scale", rank.c_str());
        if (!std::isfinite(rank_scale) || rank_scale <= 0.f)
            rank_scale = 1.f;
    }

    if (!own)
    {
        stat_add(Stat::profile_default, 1.f, state.community);
        if (bool& logged = g_logged_communities[community]; !logged)
        {
            logged = true;
            Msg("* [ai] profile %s -> default", community.size() ? community.c_str() : "<none>");
        }
    }
    if (!chain.size)
        return default_profile();

    xr_string key;
    for (u8 index = 0; index < chain.size; ++index)
    {
        key += chain.names[index].c_str();
        key += '|';
    }
    string32 scale;
    xr_sprintf(scale, "%g", rank_scale);
    key += scale;

    const shared_str cache_key = key.c_str();
    if (const auto found = g_profiles.find(cache_key); found != g_profiles.end())
        return found->second;
    return g_profiles.emplace(cache_key, build_profile(chain, rank_scale)).first->second;
}

bool class_by_name(pcstr name, WeaponClass& result) { return name && parse_enum(name, weapon_class_names, result); }

bool ammo_in(pcstr ammo, const xr_vector<shared_str>& families)
{
    return std::any_of(families.begin(), families.end(),
        [ammo](const shared_str& family) { return family.size() && starts_with(ammo, family.c_str()); });
}

bool is_automatic(pcstr section)
{
    if (!pSettings->line_exist(section, "fire_modes"))
        return false;
    const pcstr modes = pSettings->r_string(section, "fire_modes");
    string16 item;
    for (int index = 0, count = modes ? _GetItemCount(modes) : 0; index < count; ++index)
    {
        if (atoi(_GetItem(modes, index, item)) < 0)
            return true;
    }
    return false;
}

// Ordered rules of 2.3.2 (first match wins). Pistol-calibre families come before the machine gun rule: a 64-round
// Bizon or a 50-round P90 is a submachine gun, not a machine gun.
SectionClass compute_section_class(const shared_str& section)
{
    SectionClass result;
    const pcstr name = section.c_str();
    if (!name || !pSettings->section_exist(name))
    {
        result.base = WeaponClass::none;
        return result;
    }

    if (const CInifile::Item* item = longest_prefix("ai_weapon_class_overrides", name, true))
    {
        if (class_by_name(item->second.c_str(), result.base))
            return result;
        Msg("! [ai] unknown weapon class '%s' for %s", item->second.c_str(), name);
    }

    const pcstr ammo_list = READ_IF_EXISTS(pSettings, r_string, name, "ammo_class", nullptr);
    if (!ammo_list || !ammo_list[0])
    {
        result.base = WeaponClass::none;
        return result;
    }

    const int slot = READ_IF_EXISTS(pSettings, r_s32, name, "slot", -1);
    if (std::find(g_rules.pistol_slots.begin(), g_rules.pistol_slots.end(), slot) != g_rules.pistol_slots.end())
    {
        result.base = WeaponClass::pistol;
        return result;
    }

    string128 ammo;
    _GetItem(ammo_list, 0, ammo);
    if (ammo_in(ammo, g_rules.none_ammo))
        result.base = WeaponClass::none;
    else if (ammo_in(ammo, g_rules.launcher_ammo))
        result.base = WeaponClass::launcher;
    else if (ammo_in(ammo, g_rules.shotgun_ammo))
        result.base = WeaponClass::shotgun;
    else if (ammo_in(ammo, g_rules.smg_ammo))
        result.base = WeaponClass::smg;
    if (result.base != WeaponClass::rifle)
        return result;

    const u32 magazine = READ_IF_EXISTS(pSettings, r_u32, name, "ammo_mag_size", 0);
    if (is_automatic(name) && magazine >= g_rules.mg_mag)
    {
        result.base = WeaponClass::mg;
        return result;
    }

    if (longest_prefix("ai_weapon_class_rifle_old", name, true))
        result.base = WeaponClass::rifle_old;
    result.scope_rule = !g_rules.dmr_max_mag || magazine <= g_rules.dmr_max_mag;
    const int scope_status = READ_IF_EXISTS(pSettings, r_s32, name, "scope_status", 0);
    result.scope_attachable = scope_status == ALife::eAddonAttachable;
    if (scope_status == ALife::eAddonPermanent)
        result.scope_zoom = READ_IF_EXISTS(pSettings, r_float, name, "scope_zoom_factor", 0.f);
    return result;
}

const SectionClass& section_class(const shared_str& section)
{
    ensure_config();
    if (const auto found = g_section_classes.find(section); found != g_section_classes.end())
        return found->second;
    return g_section_classes.emplace(section, compute_section_class(section)).first->second;
}

float item_scope_zoom(const CInventoryItem& item, const SectionClass& info)
{
    if (!info.scope_attachable)
        return info.scope_zoom;
    const auto* weapon = smart_cast<const CWeapon*>(&item);
    if (!weapon || !weapon->IsScopeAttached())
        return 0.f;

    const shared_str scope = weapon->GetScopeName();
    if (const auto found = g_scope_zooms.find(scope); found != g_scope_zooms.end())
        return found->second;
    const float zoom = scope.size() && pSettings->section_exist(scope) ?
        READ_IF_EXISTS(pSettings, r_float, scope.c_str(), "scope_zoom_factor", 0.f) :
        0.f;
    g_scope_zooms.emplace(scope, zoom);
    return zoom;
}

WeaponClass class_of(const SectionClass& info, float zoom)
{
    return info.scope_rule && zoom >= g_rules.dmr_zoom ? WeaponClass::dmr : info.base;
}

bool is_slug(pcstr ammo)
{
    if (!ammo || !ammo[0])
        return false;
    if (const auto found = g_slug_ammo.find(ammo); found != g_slug_ammo.end())
        return found->second.second;
    const bool slug = longest_prefix("ai_slug_ammo", ammo, false);
    g_slug_ammo.emplace(ammo, std::pair<shared_str, bool>(shared_str(ammo), slug));
    return slug;
}

xr_vector<shared_str> read_list(pcstr section, pcstr key)
{
    xr_vector<shared_str> result;
    const pcstr text = READ_IF_EXISTS(pSettings, r_string, section, key, nullptr);
    string128 item;
    for (int index = 0, count = text ? _GetItemCount(text) : 0; index < count; ++index)
        result.emplace_back(_GetItem(text, index, item));
    return result;
}

bool armour_by_name(pcstr name, ArmourClass& result) { return name && parse_enum(name, armour_class_names, result); }

ArmourClass armour_by_protection(float protection)
{
    u8 result = 0;
    while (result < 3 && protection >= g_armour_thresholds[result])
        ++result;
    return ArmourClass(result);
}

// Override by the longest [ai_armour_overrides] prefix; count when none matches.
u8 armour_override(pcstr text)
{
    ArmourClass result;
    const CInifile::Item* item = longest_prefix("ai_armour_overrides", text, false);
    return item && armour_by_name(item->second.c_str(), result) ? u8(result) : u8(ArmourClass::count);
}
} // namespace

const Profile& default_profile()
{
    static const Profile profile = make_default_profile();
    return profile;
}

const Profile& profile(const CAI_Stalker& npc)
{
    const NpcState* state = DaAi::npc(npc);
    return state && state->profile ? *state->profile : default_profile();
}

pcstr profile_value(const CAI_Stalker& npc, pcstr key)
{
    static string256 buffer;
    if (!key || !key[0])
        return "";

    const Profile& p = profile(npc);
    buffer[0] = 0;
    bool found = false;
    for_each_key(p, [&](pcstr name, const auto& value) {
        if (!found && !xr_strcmp(name, key))
        {
            format_value(buffer, sizeof(buffer), value);
            found = true;
        }
    });

    if (found)
        return buffer;
    if (!xr_strcmp(key, "peek"))
        format_value(buffer, sizeof(buffer), p.peek);
    else if (!xr_strcmp(key, "aggr_winning"))
        format_value(buffer, sizeof(buffer), p.aggr_winning);
    else if (!xr_strcmp(key, "roles"))
    {
        for (u32 role = 0; role < u32(Role::count); ++role)
        {
            if (p.has_role(Role(role)))
            {
                if (buffer[0])
                    xr_strcat(buffer, ", ");
                xr_strcat(buffer, role_names[role]);
            }
        }
    }
    else if (!xr_strcmp(key, "grenade_issue"))
    {
        if (p.grenade_item.size())
            xr_sprintf(buffer, "%s, %u, %g", p.grenade_item.c_str(), p.grenade_count, p.grenade_chance);
    }
    else
    {
        // A key the struct does not know (a mod's own): raw from the chain.
        for (u8 index = 0; index < p.chain_size; ++index)
        {
            if (pSettings->line_exist(p.chain[index], key))
            {
                const pcstr value = pSettings->r_string(p.chain[index], key);
                return value ? value : "";
            }
        }
    }
    return buffer;
}

void resolve_profile(CAI_Stalker& npc)
{
    NpcState* state = DaAi::npc(npc);
    if (!state)
        return;

    ensure_config();
    state->community = npc.Community();
    state->rank = npc.Rank();
    state->band = band_of_rank(state->rank);
    state->profile = &find_profile(npc, *state);
}

void invalidate_profile(CAI_Stalker& npc)
{
    // The next registry refresh sees a community mismatch and resolves again.
    if (NpcState* state = DaAi::npc(npc))
        state->community = -1;
}

u8 band_of_rank(int rank)
{
    constexpr u8 unknown_band = 2;
    const shared_str name = rank_name(rank);
    if (!name.size() || !pSettings->line_exist("ai_rank_bands", name))
        return unknown_band;
    const int band = pSettings->r_s32("ai_rank_bands", name.c_str());
    return band >= 1 && band <= 4 ? u8(band) : unknown_band;
}

u32 section_crc(const CAI_Stalker& npc)
{
    const shared_str section = npc.cNameSect();
    return crc32(section.c_str(), u32(section.size()));
}

const CInventoryItem* held_weapon(const CAI_Stalker& npc)
{
    const CInventoryItem* active = npc.inventory().ActiveItem();
    if (smart_cast<const CWeapon*>(active))
        return active;
    // The last choice only while the NPC still carries it (a dropped weapon keeps the pointer until it is destroyed).
    const CInventoryItem* chosen = npc.m_best_item_to_kill;
    return chosen && chosen->object().H_Parent() == &npc ? chosen : nullptr;
}

WeaponClass weapon_class(const CInventoryItem& item)
{
    const SectionClass& info = section_class(item.object().cNameSect());
    return class_of(info, item_scope_zoom(item, info));
}

WeaponClass weapon_class(const shared_str& section)
{
    const SectionClass& info = section_class(section);
    return class_of(info, info.scope_zoom);
}

pcstr weapon_class_name(WeaponClass which)
{
    return which < WeaponClass::count ? weapon_class_names[size_t(which)] : weapon_class_names[0];
}

const WeaponBand& weapon_band(WeaponClass which)
{
    ensure_config();
    return which < WeaponClass::count ? g_weapon_bands[size_t(which)] : g_weapon_bands[0];
}

WeaponBand weapon_band(const CInventoryItem& item)
{
    const SectionClass& info = section_class(item.object().cNameSect());
    const float zoom = item_scope_zoom(item, info);
    const WeaponClass which = class_of(info, zoom);
    WeaponBand result = weapon_band(which);
    if (which == WeaponClass::rifle && zoom >= g_rules.dmr_zoom)
        result.engage_max *= g_rules.scoped_range_mult;
    else if (which == WeaponClass::shotgun)
    {
        if (const auto* weapon = smart_cast<const CWeapon*>(&item); weapon && is_slug(weapon->GetAmmoName()))
            result.engage_max = _max(result.engage_max, g_rules.slug_engage_max);
    }
    return result;
}

ArmourClass armour_class(const CEntityAlive& who)
{
    ensure_config();
    if (const auto* owner = smart_cast<const CInventoryOwner*>(&who))
    {
        if (const CCustomOutfit* outfit = owner->GetOutfit())
        {
            const shared_str section = outfit->cNameSect();
            auto found = g_outfit_armour.find(section);
            if (found == g_outfit_armour.end())
                found = g_outfit_armour.emplace(section, armour_override(section.c_str())).first;
            // The protection follows the outfit's condition, so only the override is cached.
            return found->second < u8(ArmourClass::count) ?
                ArmourClass(found->second) :
                armour_by_protection(outfit->GetDefHitTypeProtection(ALife::eHitTypeFireWound));
        }
    }

    const shared_str visual = who.cNameVisual();
    u8 matched = u8(ArmourClass::count);
    if (visual.size())
    {
        if (const auto found = g_visual_armour.find(visual); found != g_visual_armour.end())
            matched = found->second;
        else
        {
            // Visuals are paths (actors\stalker_dolg\stalker_dolg_2): the file name is tried first.
            const pcstr path = visual.c_str();
            pcstr file = path;
            for (pcstr cursor = path; *cursor; ++cursor)
            {
                if (*cursor == '\\' || *cursor == '/')
                    file = cursor + 1;
            }
            matched = armour_override(file);
            if (matched == u8(ArmourClass::count))
                matched = armour_override(path);
            g_visual_armour.emplace(visual, matched);
        }
    }
    if (matched < u8(ArmourClass::count))
        return ArmourClass(matched);
    return armour_by_protection(1.f - who.conditions().GetHitImmunity(ALife::eHitTypeFireWound));
}

void load_profile_config()
{
    std::copy(std::begin(compiled_weapon_bands), std::end(compiled_weapon_bands), std::begin(g_weapon_bands));
    constexpr pcstr classes = "ai_weapon_classes";
    for (size_t index = 1; index < size_t(WeaponClass::count); ++index)
    {
        const pcstr name = weapon_class_names[index];
        if (!pSettings->line_exist(classes, name))
            continue;
        const pcstr text = pSettings->r_string(classes, name);
        string64 item;
        WeaponBand band = g_weapon_bands[index];
        if (!text || _GetItemCount(text) < 3 || !parse_value(_GetItem(text, 0, item), band.effective) ||
            !parse_value(_GetItem(text, 1, item), band.engage_max) || !parse_value(_GetItem(text, 2, item), band.min))
        {
            Msg("! [ai] [%s] %s: expected effective, engage_max, min", classes, name);
            continue;
        }
        g_weapon_bands[index] = band;
    }

    constexpr pcstr rules = "ai_weapon_class_rules";
    g_rules = {};
    for (const shared_str& slot : read_list(rules, "pistol_slots"))
        g_rules.pistol_slots.push_back(atoi(slot.c_str()));
    g_rules.none_ammo = read_list(rules, "none_ammo");
    g_rules.launcher_ammo = read_list(rules, "launcher_ammo");
    g_rules.shotgun_ammo = read_list(rules, "shotgun_ammo");
    g_rules.smg_ammo = read_list(rules, "smg_ammo");
    g_rules.mg_mag = READ_IF_EXISTS(pSettings, r_u32, rules, "mg_mag", g_rules.mg_mag);
    g_rules.dmr_zoom = READ_IF_EXISTS(pSettings, r_float, rules, "dmr_zoom", g_rules.dmr_zoom);
    g_rules.dmr_max_mag = READ_IF_EXISTS(pSettings, r_u32, rules, "dmr_max_mag", g_rules.dmr_max_mag);
    g_rules.scoped_range_mult =
        READ_IF_EXISTS(pSettings, r_float, rules, "scoped_range_mult", g_rules.scoped_range_mult);
    g_rules.slug_engage_max = READ_IF_EXISTS(pSettings, r_float, rules, "slug_engage_max", g_rules.slug_engage_max);

    constexpr pcstr armour = "ai_armour_classes";
    for (size_t index = 1; index < size_t(ArmourClass::count); ++index)
    {
        g_armour_thresholds[index - 1] =
            READ_IF_EXISTS(pSettings, r_float, armour, armour_class_names[index], g_armour_thresholds[index - 1]);
    }

    g_section_classes.clear();
    g_scope_zooms.clear();
    g_slug_ammo.clear();
    g_outfit_armour.clear();
    g_visual_armour.clear();
    g_config_loaded = true;
}

void profile_level_unload()
{
    g_profiles.clear();
    g_section_classes.clear();
    g_scope_zooms.clear();
    g_slug_ammo.clear();
    g_outfit_armour.clear();
    g_visual_armour.clear();
    g_config_loaded = false;
}

namespace Traits
{
namespace
{
constexpr u8 flag_rolled = 1 << 0;
constexpr u8 flag_radio = 1 << 1;
constexpr u8 known_flags = flag_rolled | flag_radio;
constexpr size_t count_size = sizeof(u32);
constexpr size_t record_size = sizeof(u16) + sizeof(u32) + sizeof(u8);
constexpr u32 max_records = u16(-1);
constexpr u32 portion = 16;

struct Record
{
    u32 section_crc{};
    u8 flags{};
};

// Session registry (2.11): every NPC rolled this session or restored from AIT1, online or offline.
xr_flat_hash_map<u16, Record> g_table;

template <typename T>
void write_le(xr_vector<u8>& out, T value)
{
    for (size_t index = 0; index < sizeof(T); ++index)
        out.push_back(u8(value >> (index * 8)));
}

template <typename T>
T read_le(const u8*& cursor)
{
    T value = 0;
    for (size_t index = 0; index < sizeof(T); ++index)
        value |= T(cursor[index]) << (index * 8);
    cursor += sizeof(T);
    return value;
}

u64 splitmix64(u64 value)
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

// Whether the NPC is the AI leader of its sim squad at this moment (C22): the highest band alive over every member,
// online or offline, so the answer does not depend on the order in which members come online.
bool leads_squad(const CAI_Stalker& npc)
{
    const auto* monster = smart_cast<const CSE_ALifeMonsterAbstract*>(server_object(npc.ID()));
    if (!monster || monster->m_group_id == u16(-1))
        return true;
    const auto* group = smart_cast<const CSE_ALifeOnlineOfflineGroup*>(server_object(monster->m_group_id));
    if (!group)
        return true;

    u16 best = u16(-1);
    u8 best_band = 0;
    int best_rank = 0;
    for (const auto& [id, member] : group->squad_members())
    {
        const auto* trader = smart_cast<const CSE_ALifeTraderAbstract*>(member);
        if (!trader || !member->g_Alive())
            continue;
        const u8 band = band_of_rank(trader->m_rank);
        if (best == u16(-1) || band > best_band || (band == best_band && trader->m_rank > best_rank) ||
            (band == best_band && trader->m_rank == best_rank && id < best))
        {
            best = id;
            best_band = band;
            best_rank = trader->m_rank;
        }
    }
    return best == u16(-1) || best == npc.ID();
}

bool object_matches(u16 id, u32 crc)
{
    const CSE_ALifeDynamicObject* object = server_object(id);
    return object && crc32(object->s_name.c_str(), u32(object->s_name.size())) == crc;
}
} // namespace

void roll_radio(CAI_Stalker& npc)
{
    NpcState* state = DaAi::npc(npc);
    if (!state)
        return;

    const u16 id = npc.ID();
    const u32 crc = section_crc(npc);
    if (const auto found = g_table.find(id); found != g_table.end() && found->second.section_crc == crc)
        state->has_radio = found->second.flags & flag_radio;
    else
    {
        const Profile& p = profile(npc);
        float chance = p.radio_chance[std::clamp<u8>(state->band, 1, 4) - 1];
        if (leads_squad(npc))
            chance += p.radio_leader_bonus;
        const u64 bits = splitmix64(u64(id) ^ u64(crc) ^ 0xDAull);
        const double roll = double(bits >> 11) * 0x1.0p-53;
        state->has_radio = roll < std::min(1.0, double(chance));
        g_table[id] = {crc, u8(flag_rolled | (state->has_radio ? flag_radio : 0))};
    }
    if (state->has_radio)
        stat_add(Stat::c_radio_npcs, 1.f, state->community);
}

void save_begin(ChunkCapture& state)
{
    state = {};
    state.begun = true;
    state.ids.reserve(g_table.size());
    for (const auto& [id, record] : g_table)
        state.ids.push_back(id);
    std::sort(state.ids.begin(), state.ids.end());
}

bool save_continue(ChunkCapture& state, float budget_ms)
{
    if (!state.begun)
        save_begin(state);
    if (!(budget_ms > 0.f))
        return false;
    if (state.ids.size() > max_records)
    {
        state.failed = true;
        return true;
    }

    if (state.payload.empty())
    {
        state.payload.reserve(count_size + state.ids.size() * record_size);
        write_le<u32>(state.payload, 0);
    }

    const bool unlimited = budget_ms == flt_max;
    CTimer timer;
    timer.Start();
    u32 batch = 0;
    while (state.index < state.ids.size())
    {
        const u16 id = state.ids[state.index++];
        // Records of released objects or of an id reused by another section leave the table here.
        if (const auto found = g_table.find(id); found != g_table.end())
        {
            if (object_matches(id, found->second.section_crc))
            {
                write_le<u16>(state.payload, id);
                write_le<u32>(state.payload, found->second.section_crc);
                write_le<u8>(state.payload, found->second.flags);
            }
            else
                g_table.erase(found);
        }

        if (!unlimited && ++batch == portion)
        {
            batch = 0;
            if (timer.GetElapsed_sec() * 1000.f >= budget_ms)
                return false;
        }
    }

    const u32 count = u32((state.payload.size() - count_size) / record_size);
    for (size_t index = 0; index < count_size; ++index)
        state.payload[index] = u8(count >> (index * 8));
    state.present = count > 0;
    return true;
}

bool stage(const xr_vector<u8>& payload)
{
    if (payload.size() < count_size)
        return false;
    const u8* cursor = payload.data();
    const u32 count = read_le<u32>(cursor);
    if (count > max_records || payload.size() != count_size + size_t(count) * record_size)
        return false;

    struct Staged
    {
        u16 id;
        Record record;
    };
    xr_vector<Staged> records;
    records.reserve(count);
    u32 skipped = 0;
    for (u32 index = 0; index < count; ++index)
    {
        Staged staged;
        staged.id = read_le<u16>(cursor);
        staged.record.section_crc = read_le<u32>(cursor);
        staged.record.flags = read_le<u8>(cursor);
        if (staged.id == u16(-1) || !(staged.record.flags & flag_rolled) || (staged.record.flags & ~known_flags))
        {
            ++skipped;
            continue;
        }
        records.push_back(staged);
    }

    // Every copy of a duplicated id is dropped, as in the other record chunks.
    std::sort(records.begin(), records.end(),
        [](const Staged& left, const Staged& right) { return left.id < right.id; });
    g_table.clear();
    for (size_t index = 0; index < records.size();)
    {
        size_t next = index + 1;
        while (next < records.size() && records[next].id == records[index].id)
            ++next;
        if (next == index + 1)
            g_table.emplace(records[index].id, records[index].record);
        else
            skipped += u32(next - index);
        index = next;
    }

    if (skipped)
        Msg("! Ignored %u semantically invalid records in save-extension chunk 'AIT1'", skipped);
    return true;
}

void clear() { g_table.clear(); }
void forget(u16 object_id) { g_table.erase(object_id); }
} // namespace Traits
} // namespace DaAi
