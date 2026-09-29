// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_morale.h"
#include "da_ai_comms.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"
#include "da_ai_senses.h"

#include "ai/stalker/ai_stalker.h"
#include "agent_location_manager.h"
#include "agent_manager.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "character_community.h"
#include "cover_manager.h"
#include "cover_point.h"
#include "danger_manager.h"
#include "danger_object.h"
#include "detail_path_manager_space.h"
#include "enemy_manager.h"
#include "EntityCondition.h"
#include "Hit.h"
#include "Inventory.h"
#include "memory_manager.h"
#include "movement_manager_space.h"
#include "sight_action.h"
#include "sight_manager.h"
#include "stalker_combat_actions.h"
#include "stalker_movement_manager_smart_cover.h"
#include "stalker_movement_restriction.h"
#include "visual_memory_manager.h"
#include "Weapon.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrServerEntities/xrServer_Objects_ALife_Monsters.h"

using namespace MonsterSpace;

namespace DaAi::Morale
{
namespace
{
constexpr u16 no_id = u16(-1);
constexpr float reject = -flt_max;

// A stalker death, kept so that others can learn it later by seeing the body.
struct Death
{
    u16 victim;
    bool leader; // the squad's AI leader at the moment of death
    u32 time;
    Fvector position;
};

// [ai_morale], [ai_morale_weapons], [ai_morale_monsters] and [ai_modifiers]. The initial values mirror the shipped
// dead_air_x64_ai.ltx and apply only when a key is missing.
struct Config
{
    bool loaded{};

    u32 update_ms{250};
    u32 quiet_update_ms{1000};
    float levels[4]{.15f, .35f, .65f, .85f};
    float hysteresis{.04f};
    float conf_base{.5f};
    float ratio_k{.15f};
    float ratio_max{4.f};
    float courage_k{.4f};
    float health_k{.3f};
    float ammo_k{.1f};
    float fear_k{.5f};
    float loss_k{.4f};
    float kill_k{.05f};
    float kill_max{.15f};
    float leader_conf{.05f};

    float unknown_enemy_strength{1.f};
    float monster_strength{.6f};
    float band_strength[4]{.8f, 1.f, 1.25f, 1.5f};
    float armour_strength[size_t(ArmourClass::count)]{.8f, 1.f, 1.25f, 1.6f};
    float weapon_strength[size_t(WeaponClass::count)]{.3f, .6f, .9f, 1.f, .85f, 1.f, 1.4f, 1.2f, 1.5f};
    float wounded_health{.5f};
    float wounded_strength{.6f};
    float ally_weight{.7f};
    float ally_radius{50.f};
    u32 sightings_max{16};
    float sighting_memory_s{120.f};

    float threat_window_s{15.f};
    float threat_distance_m{60.f};
    float threat_suppression{.2f};
    float ratio_hold_s{3.f};
    float calm_reset_s{30.f};
    float leader_contact_m{40.f};
    float death_memory_s{600.f};
    u32 death_records{64};
    float enemy_death_seen_s{10.f};
    float safe_record_s{5.f};
    float rally_max_m{200.f};

    float fear_courage_base{1.5f};
    float fear_band[4]{1.3f, 1.f, .8f, .65f};
    float fear_hit{.12f};
    float fear_hit_dmg{.4f};
    float fear_friendly_hit{.5f};
    float fear_supp{.25f};
    float fear_casualty{.2f};
    float fear_report_mult{.6f};
    float fear_close_m{20.f};
    float fear_close_mult{1.5f};
    float fear_leader{.25f};
    float fear_ally_death{.1f};
    float fear_contagion{.15f};
    float fear_mate_fallback{.08f};
    float fear_grenade{.2f};
    float fear_grenade_m{8.f};
    float order_relief{.1f};
    float fear_decay_k{.35f};
    float cover_decay_mult{1.5f};
    float in_cover_protection{.6f};

    float arrive_m{1.5f};
    float replan_s{4.f};
    float orderly_step_m{35.f};
    float orderly_min_gain_m{5.f};
    Range orderly_hold_s{2.f, 5.f};
    float orderly_safe_m{80.f};
    float retreat_min_enemy_m{15.f};
    float retreat_back_angle{60.f};
    Range scatter_angle{20.f, 70.f};
    float scatter_search_m{50.f};
    float scatter_dist{45.f};
    float scatter_cone{50.f};
    float scatter_hide_s{2.f};
    float scatter_hide_radius{15.f};
    float scatter_respot_m{30.f};
    float relocate_radius{25.f};
    float relocate_tolerance_m{3.f};
    float relocate_min_angle{15.f};
    float relocate_min_travel_m{6.f};
    float relocate_min_enemy_m{8.f};
    float relocate_max_s{12.f};
    float relocate_cooldown_s{10.f};
    float surrender_health{.5f};
    float surrender_cornered_m{12.f};
    float cover_w_gain{1.f};
    float cover_w_protect{1.f};
    float cover_w_rally{.5f};
    float cover_w_travel{.4f};
    float cover_w_danger{2.f};
    float cover_max_dy{3.f};

    float acc_withdraw{.3f};
    float stamina_run_s{40.f};
    float stamina_sprint_mult{1.6f};
    float stamina_jog_mult{.3f};
    float stamina_rest_s{12.f};
    float stamina_walk_s{40.f};
    float stamina_move_speed{.3f};
    float stamina_wound_k{1.f};
    float stamina_armour[size_t(ArmourClass::count)]{.9f, 1.f, 1.3f, .8f};
    float stamina_tired{.35f};
    float stamina_exhausted{.1f};
    float stamina_max_dt_s{2.f};
    float help_health{.3f};
    float help_repeat_s{20.f};
    u32 help_max{3};
    bool help_fanatic{};

    xr_vector<std::pair<CLASS_ID, float>> monsters; // by the monster's class id (the section's class key)

    float armour_break[size_t(ArmourClass::count)]{-.1f, 0.f, .1f, .1f};
    float armour_aggr[size_t(ArmourClass::count)]{0.f, 0.f, .1f, .1f};
    float armour_cover[size_t(ArmourClass::count)]{.1f, 0.f, -.1f, -.1f};
    float wounded_break{-.15f};
    float wounded_aggr{-.3f};
    float low_ammo_aggr{-.2f};
    float fear_aggr{-.4f};
    float leader_alive_break{.1f};
    float sworn_break{.1f};
    float level_aggr[5]{0.f, 0.f, 0.f, .05f, .1f}; // panicked .. heroic
    float fanatic_push_aggr{.9f};
    u8 wounded_max_level{1}; // Level::worried
    Range leader_handover_s{3.f, 6.f};
    xr_vector<int> leader_alive; // community indices whose followers take heart from a leader in contact
    xr_vector<int> leader_handover; // community indices where the next in rank takes over
    xr_vector<std::pair<int, int>> sworn; // [ai_sworn_enemies]
};

Config g_cfg;
xr_vector<Death> g_deaths;
xr_flat_hash_map<int, int> g_alias;

void read(pcstr section, pcstr key, float& value) { value = READ_IF_EXISTS(pSettings, r_float, section, key, value); }
void read(pcstr section, pcstr key, u32& value) { value = READ_IF_EXISTS(pSettings, r_u32, section, key, value); }
void read(pcstr section, pcstr key, bool& value) { value = READ_IF_EXISTS(pSettings, r_bool, section, key, value); }

void read(pcstr section, pcstr key, float* values, size_t count)
{
    if (!pSettings->line_exist(section, key))
        return;
    const pcstr line = pSettings->r_string(section, key);
    const size_t items = _GetItemCount(line);
    string64 item;
    for (size_t index = 0; index < count && index < items; ++index)
        values[index] = float(atof(_GetItem(line, int(index), item)));
}

void read(pcstr section, pcstr key, Range& range)
{
    float values[2]{range.lo, range.hi};
    read(section, key, values, 2);
    range = {values[0], _max(values[0], values[1])};
}

// A level by name (panicked .. heroic) or by its index.
void read_level(pcstr section, pcstr key, u8& value)
{
    if (!pSettings->line_exist(section, key))
        return;
    static constexpr pcstr names[]{"panicked", "worried", "steady", "confident", "heroic"};
    const pcstr text = pSettings->r_string(section, key);
    for (u8 index = 0; index < std::size(names); ++index)
    {
        if (!xr_strcmp(text, names[index]))
        {
            value = index;
            return;
        }
    }
    value = u8(clampr(atoi(text), 0, int(std::size(names)) - 1));
}

int community_index(pcstr name) { return CHARACTER_COMMUNITY::IdToIndex(shared_str(name), -1, true); }

void read(pcstr section, pcstr key, xr_vector<int>& communities)
{
    communities.clear();
    if (!pSettings->line_exist(section, key))
        return;
    const pcstr line = pSettings->r_string(section, key);
    string64 item;
    for (int index = 0, count = _GetItemCount(line); index < count; ++index)
    {
        if (const int community = community_index(_GetItem(line, index, item)); community >= 0)
            communities.push_back(community);
    }
}

void load_config()
{
    Config& c = g_cfg;
    c = {};
    c.loaded = true;

    constexpr pcstr m = "ai_morale";
    if (pSettings->section_exist(m))
    {
        read(m, "update_ms", c.update_ms);
        read(m, "quiet_update_ms", c.quiet_update_ms);
        read(m, "levels", c.levels, std::size(c.levels));
        read(m, "hysteresis", c.hysteresis);
        read(m, "conf_base", c.conf_base);
        read(m, "ratio_k", c.ratio_k);
        read(m, "ratio_max", c.ratio_max);
        read(m, "courage_k", c.courage_k);
        read(m, "health_k", c.health_k);
        read(m, "ammo_k", c.ammo_k);
        read(m, "fear_k", c.fear_k);
        read(m, "loss_k", c.loss_k);
        read(m, "kill_k", c.kill_k);
        read(m, "kill_max", c.kill_max);
        read(m, "leader_conf", c.leader_conf);
        read(m, "unknown_enemy_strength", c.unknown_enemy_strength);
        read(m, "monster_strength", c.monster_strength);
        read(m, "band_strength", c.band_strength, std::size(c.band_strength));
        read(m, "armour_strength", c.armour_strength, std::size(c.armour_strength));
        read(m, "wounded_health", c.wounded_health);
        read(m, "wounded_strength", c.wounded_strength);
        read(m, "ally_weight", c.ally_weight);
        read(m, "ally_radius", c.ally_radius);
        read(m, "sightings_max", c.sightings_max);
        read(m, "sighting_memory_s", c.sighting_memory_s);
        read(m, "threat_window_s", c.threat_window_s);
        read(m, "threat_distance_m", c.threat_distance_m);
        read(m, "threat_suppression", c.threat_suppression);
        read(m, "ratio_hold_s", c.ratio_hold_s);
        read(m, "calm_reset_s", c.calm_reset_s);
        read(m, "leader_contact_m", c.leader_contact_m);
        read(m, "death_memory_s", c.death_memory_s);
        read(m, "death_records", c.death_records);
        read(m, "enemy_death_seen_s", c.enemy_death_seen_s);
        read(m, "safe_record_s", c.safe_record_s);
        read(m, "rally_max_m", c.rally_max_m);
        read(m, "fear_courage_base", c.fear_courage_base);
        read(m, "fear_band", c.fear_band, std::size(c.fear_band));
        read(m, "fear_hit", c.fear_hit);
        read(m, "fear_hit_dmg", c.fear_hit_dmg);
        read(m, "fear_friendly_hit", c.fear_friendly_hit);
        read(m, "fear_supp", c.fear_supp);
        read(m, "fear_casualty", c.fear_casualty);
        read(m, "fear_report_mult", c.fear_report_mult);
        read(m, "fear_close_m", c.fear_close_m);
        read(m, "fear_close_mult", c.fear_close_mult);
        read(m, "fear_leader", c.fear_leader);
        read(m, "fear_ally_death", c.fear_ally_death);
        read(m, "fear_contagion", c.fear_contagion);
        read(m, "fear_mate_fallback", c.fear_mate_fallback);
        read(m, "fear_grenade", c.fear_grenade);
        read(m, "fear_grenade_m", c.fear_grenade_m);
        read(m, "order_relief", c.order_relief);
        read(m, "fear_decay_k", c.fear_decay_k);
        read(m, "cover_decay_mult", c.cover_decay_mult);
        read(m, "in_cover_protection", c.in_cover_protection);
        read(m, "arrive_m", c.arrive_m);
        read(m, "replan_s", c.replan_s);
        read(m, "orderly_step_m", c.orderly_step_m);
        read(m, "orderly_min_gain_m", c.orderly_min_gain_m);
        read(m, "orderly_hold_s", c.orderly_hold_s);
        read(m, "orderly_safe_m", c.orderly_safe_m);
        read(m, "retreat_min_enemy_m", c.retreat_min_enemy_m);
        read(m, "retreat_back_angle", c.retreat_back_angle);
        read(m, "scatter_angle", c.scatter_angle);
        read(m, "scatter_search_m", c.scatter_search_m);
        read(m, "scatter_dist", c.scatter_dist);
        read(m, "scatter_cone", c.scatter_cone);
        read(m, "scatter_hide_s", c.scatter_hide_s);
        read(m, "scatter_hide_radius", c.scatter_hide_radius);
        read(m, "scatter_respot_m", c.scatter_respot_m);
        read(m, "relocate_radius", c.relocate_radius);
        read(m, "relocate_tolerance_m", c.relocate_tolerance_m);
        read(m, "relocate_min_angle", c.relocate_min_angle);
        read(m, "relocate_min_travel_m", c.relocate_min_travel_m);
        read(m, "relocate_min_enemy_m", c.relocate_min_enemy_m);
        read(m, "relocate_max_s", c.relocate_max_s);
        read(m, "relocate_cooldown_s", c.relocate_cooldown_s);
        read(m, "surrender_health", c.surrender_health);
        read(m, "surrender_cornered_m", c.surrender_cornered_m);
        read(m, "cover_w_gain", c.cover_w_gain);
        read(m, "cover_w_protect", c.cover_w_protect);
        read(m, "cover_w_rally", c.cover_w_rally);
        read(m, "cover_w_travel", c.cover_w_travel);
        read(m, "cover_w_danger", c.cover_w_danger);
        read(m, "cover_max_dy", c.cover_max_dy);
        read(m, "acc_withdraw", c.acc_withdraw);
        read(m, "stamina_run_s", c.stamina_run_s);
        read(m, "stamina_sprint_mult", c.stamina_sprint_mult);
        read(m, "stamina_jog_mult", c.stamina_jog_mult);
        read(m, "stamina_rest_s", c.stamina_rest_s);
        read(m, "stamina_walk_s", c.stamina_walk_s);
        read(m, "stamina_move_speed", c.stamina_move_speed);
        read(m, "stamina_wound_k", c.stamina_wound_k);
        read(m, "stamina_armour", c.stamina_armour, std::size(c.stamina_armour));
        read(m, "stamina_tired", c.stamina_tired);
        read(m, "stamina_exhausted", c.stamina_exhausted);
        read(m, "stamina_max_dt_s", c.stamina_max_dt_s);
        read(m, "help_health", c.help_health);
        read(m, "help_repeat_s", c.help_repeat_s);
        read(m, "help_max", c.help_max);
        read(m, "help_fanatic", c.help_fanatic);
    }
    else
        Msg("! [ai] section [%s] is missing, morale uses its compiled-in defaults", m);

    constexpr pcstr weapons = "ai_morale_weapons";
    for (size_t index = 0; index < size_t(WeaponClass::count); ++index)
        read(weapons, weapon_class_name(WeaponClass(index)), c.weapon_strength[index]);

    constexpr pcstr monsters = "ai_morale_monsters";
    if (pSettings->section_exist(monsters))
    {
        for (const auto& item : pSettings->r_section(monsters).Data)
        {
            if (item.first.size() && item.first.size() <= 8 && item.second.size())
                c.monsters.emplace_back(TEXT2CLSID(item.first.c_str()), float(atof(item.second.c_str())));
        }
    }

    constexpr pcstr mods = "ai_modifiers";
    read(mods, "armour_break", c.armour_break, std::size(c.armour_break));
    read(mods, "armour_aggr", c.armour_aggr, std::size(c.armour_aggr));
    read(mods, "armour_cover", c.armour_cover, std::size(c.armour_cover));
    read(mods, "wounded_break", c.wounded_break);
    read(mods, "wounded_aggr", c.wounded_aggr);
    read(mods, "low_ammo_aggr", c.low_ammo_aggr);
    read(mods, "fear_aggr", c.fear_aggr);
    read(mods, "leader_alive_break", c.leader_alive_break);
    read(mods, "sworn_break", c.sworn_break);
    read(mods, "level_aggr", c.level_aggr, std::size(c.level_aggr));
    read(mods, "fanatic_push_aggr", c.fanatic_push_aggr);
    read_level(mods, "wounded_max_level", c.wounded_max_level);
    read(mods, "leader_handover_s", c.leader_handover_s);
    read(mods, "leader_alive", c.leader_alive);
    read(mods, "leader_handover", c.leader_handover);

    // Shared with the search lane: "<community> = <community>, ..." pairs, symmetric.
    constexpr pcstr sworn = "ai_sworn_enemies";
    if (pSettings->section_exist(sworn))
    {
        string64 item;
        for (const auto& line : pSettings->r_section(sworn).Data)
        {
            const int a = line.first.size() ? community_index(line.first.c_str()) : -1;
            if (a < 0 || !line.second.size())
                continue;
            for (int index = 0, count = _GetItemCount(line.second.c_str()); index < count; ++index)
            {
                if (const int b = community_index(_GetItem(line.second.c_str(), index, item)); b >= 0)
                    c.sworn.emplace_back(a, b);
            }
        }
    }
}

const Config& cfg()
{
    if (!g_cfg.loaded)
        load_config();
    return g_cfg;
}

u32 now() { return Device.dwTimeGlobal; }
u32 ms(float seconds) { return u32(_max(0.f, seconds) * 1000.f); }
float random(const Range& range) { return ::Random.randF(range.lo, range.hi); }
size_t band_index(u8 band) { return size_t(clampr<int>(band, 1, 4) - 1); }

// A mod's community (renegade) behaves like the one [ai_community_alias] names (bandit).
int base_community(int index)
{
    if (index < 0)
        return index;
    if (const auto found = g_alias.find(index); found != g_alias.end())
        return found->second;

    int result = index;
    const shared_str name = CHARACTER_COMMUNITY::IndexToId(index, nullptr, true);
    constexpr pcstr aliases = "ai_community_alias";
    if (name.size() && pSettings->line_exist(aliases, name.c_str()))
    {
        if (const int alias = community_index(pSettings->r_string(aliases, name.c_str())); alias >= 0)
            result = alias;
    }
    g_alias.emplace(index, result);
    return result;
}

bool listed(const xr_vector<int>& communities, int community)
{
    return std::find(communities.begin(), communities.end(), base_community(community)) != communities.end();
}

MoraleState* state_of(const CAI_Stalker& npc)
{
    const NpcState* state = DaAi::npc(npc);
    return state ? state->morale : nullptr;
}

MoraleState* active_state(const CAI_Stalker& npc) { return feature(Feature::morale) ? state_of(npc) : nullptr; }

Fvector flat_direction(const Fvector& from, const Fvector& to)
{
    Fvector result = Fvector().sub(to, from);
    result.y = 0.f;
    return result.normalize_safe();
}

Fvector rotate_y(const Fvector& direction, float angle)
{
    const float c = _cos(angle), s = _sin(angle);
    return Fvector().set(direction.x * c - direction.z * s, 0.f, direction.x * s + direction.z * c);
}

// How well a level vertex hides someone from a threat position: 0 open .. 1 covered (the engine's cover data).
float protection(u32 vertex, const Fvector& from, const Fvector& threat)
{
    if (!ai().level_graph().valid_vertex_id(vertex))
        return 0.f;
    Fvector direction = Fvector().sub(threat, from);
    float yaw, pitch;
    direction.getHP(yaw, pitch);
    yaw = angle_normalize(yaw);
    const CLevelGraph& graph = ai().level_graph();
    return 1.f - _min(graph.high_cover_in_direction(yaw, vertex), graph.low_cover_in_direction(yaw, vertex));
}

// ---------------------------------------------------------------------------------------------------------------
// strength, as a person can tell it from the look of someone
// ---------------------------------------------------------------------------------------------------------------

float weapon_strength(const CInventoryItem* item)
{
    const auto* weapon = smart_cast<const CWeapon*>(item);
    return g_cfg.weapon_strength[size_t(weapon ? weapon_class(*item) : WeaponClass::none)];
}

// What one sees of another's weapon: never that NPC's weapon choice, which runs Lua and moves its throttle.
const CInventoryItem* visible_weapon(const CEntityAlive& who)
{
    if (const auto* stalker = smart_cast<const CAI_Stalker*>(&who))
        return held_weapon(*stalker);
    if (const auto* owner = smart_cast<const CInventoryOwner*>(&who))
        return owner->inventory().ActiveItem();
    return nullptr;
}

float wound_factor(const CEntityAlive& who)
{
    return who.conditions().GetHealth() < g_cfg.wounded_health ? g_cfg.wounded_strength : 1.f;
}

float human_strength(const CEntityAlive& who, const CInventoryItem* weapon)
{
    return g_cfg.armour_strength[size_t(armour_class(who))] * weapon_strength(weapon) * wound_factor(who);
}

float enemy_strength(const CEntityAlive& enemy)
{
    if (smart_cast<const CInventoryOwner*>(&enemy))
        return human_strength(enemy, visible_weapon(enemy));

    float result = g_cfg.monster_strength;
    for (const auto& [monster_class, strength] : g_cfg.monsters)
    {
        if (monster_class == enemy.CLS_ID)
        {
            result = strength;
            break;
        }
    }
    return result * wound_factor(enemy);
}

// A mate's rank is known to his squad, unlike an enemy's.
float mate_strength(const CAI_Stalker& mate, u8 band)
{
    return g_cfg.band_strength[band_index(band)] * human_strength(mate, held_weapon(mate));
}

// ---------------------------------------------------------------------------------------------------------------
// roster, deaths, leadership
// ---------------------------------------------------------------------------------------------------------------

template <typename State>
auto find_mate(State& s, u16 id)
{
    const auto found = std::find_if(s.mates.begin(), s.mates.end(), [id](const auto& mate) { return mate.id == id; });
    return found == s.mates.end() ? nullptr : &*found;
}

bool knows_death(const MoraleState& s, u16 id)
{
    return std::find(s.known_deaths.begin(), s.known_deaths.end(), id) != s.known_deaths.end();
}

const Death* recorded_death(u16 id)
{
    const auto found = std::find_if(g_deaths.begin(), g_deaths.end(), [id](const Death& d) { return d.victim == id; });
    return found == g_deaths.end() ? nullptr : &*found;
}

void expire_deaths()
{
    const u32 t = now();
    const u32 memory = ms(g_cfg.death_memory_s);
    const auto expired = [&](const Death& death) { return t > death.time + memory; };
    g_deaths.erase(std::remove_if(g_deaths.begin(), g_deaths.end(), expired), g_deaths.end());
}

void add_fear(MoraleState& s, float amount) { s.fear = clampr(s.fear + amount, 0.f, 1.f); }

float fear_sensitivity(const Profile& p, u8 band)
{
    return p.fanatic ? 0.f : _max(0.f, g_cfg.fear_courage_base - p.courage) * g_cfg.fear_band[band_index(band)];
}

// Mates who left the squad alive (another squad, offline) are dropped; the dead stay until the NPC learns of it.
void sync_mates(const CAI_Stalker& npc, MoraleState& s, const SquadState* squad)
{
    const auto in_squad = [squad](u16 id) {
        return squad && std::find(squad->members.begin(), squad->members.end(), id) != squad->members.end();
    };
    s.mates.erase(std::remove_if(s.mates.begin(), s.mates.end(),
                      [&](const MoraleState::Mate& mate) { return !in_squad(mate.id) && !recorded_death(mate.id); }),
        s.mates.end());

    s.known_deaths.erase(std::remove_if(s.known_deaths.begin(), s.known_deaths.end(),
                             [](u16 id) { return !recorded_death(id); }),
        s.known_deaths.end());

    if (!squad)
        return;
    for (const u16 id : squad->members)
    {
        if (id == npc.ID() || find_mate(s, id) || knows_death(s, id))
            continue;
        const NpcState* mate = DaAi::npc(id);
        if (mate && mate->owner && !mate->dead)
            s.mates.push_back({id, mate_strength(*mate->owner, mate->band)});
    }
}

// The believed leader follows the squad's AI leader, except that a dead leader stays the believed one until his
// death is learned, and after that the squad is leaderless until the handover (or the next calm).
void sync_leader(MoraleState& s, const SquadState* squad)
{
    const u16 current = squad ? squad->leader : no_id;
    if (s.handover_at && now() >= s.handover_at)
    {
        s.handover_at = 0;
        s.leader = current;
        return;
    }
    if (s.leader_dead || s.leader == current)
        return;
    if (s.leader != no_id && recorded_death(s.leader))
        return;
    s.leader = current;
}

bool leader_in_contact(const CAI_Stalker& npc, const MoraleState& s)
{
    if (s.leader == no_id || s.leader == npc.ID() || !find_mate(s, s.leader))
        return false;
    const NpcState* leader = DaAi::npc(s.leader);
    const NpcState* self = DaAi::npc(npc);
    if (!leader || !leader->owner || !self)
        return false;
    if (self->has_radio && leader->has_radio)
        return true;
    return Know::sees_now(npc, leader->owner) ||
        npc.Position().distance_to(leader->owner->Position()) <= g_cfg.leader_contact_m;
}

void reset_engagement(MoraleState& s)
{
    s.baseline = u8(_min<size_t>(255, 1 + s.mates.size()));
    s.losses = 0;
    s.enemy_kills = 0;
    s.low_ratio_since = 0;
    s.leader_dead = false;
    s.handover_at = 0;
}

void learn_death(CAI_Stalker& npc, MoraleState& s, u16 victim, bool was_leader, Source source, const Fvector* where)
{
    if (victim == npc.ID() || knows_death(s, victim))
        return;
    s.known_deaths.push_back(victim);
    s.quiet = false;

    const auto* mate = find_mate(s, victim);
    if (!mate)
        return;
    s.mates.erase(s.mates.begin() + (mate - s.mates.data()));

    const NpcState* self = DaAi::npc(npc);
    const Profile& p = profile(npc);
    const float sensitivity = fear_sensitivity(p, self->band);
    const bool reported = source >= Source::report_voice && source <= Source::report_hq;
    const bool close = where && npc.Position().distance_to(*where) <= g_cfg.fear_close_m;
    const float impact = (reported ? g_cfg.fear_report_mult : 1.f) * (close ? g_cfg.fear_close_mult : 1.f);

    s.losses = u8(_min(255, s.losses + 1));
    s.last_threat = now();
    add_fear(s, g_cfg.fear_casualty * sensitivity * impact);
    stat_add(Stat::m_casualty_known, 1.f, self->community);

    if (victim != s.leader && !was_leader)
        return;
    s.leader = no_id;
    s.leader_dead = true;
    s.handover_at = listed(g_cfg.leader_handover, self->community) ? now() + ms(random(g_cfg.leader_handover_s)) : 0;
    add_fear(s, g_cfg.fear_leader * sensitivity * (reported ? g_cfg.fear_report_mult : 1.f));
    stat_add(Stat::m_leader_known, 1.f, self->community);
}

// Deaths the NPC sees: a mate's (at the moment or his body later), a fresh enemy or ally death in view.
void perceive_deaths(CAI_Stalker& npc, MoraleState& s, float sensitivity, bool engaged)
{
    const u32 t = now();
    const u32 fresh = ms(g_cfg.enemy_death_seen_s);
    for (const Death& death : g_deaths)
    {
        if (death.victim == npc.ID() || knows_death(s, death.victim))
            continue;
        const bool mate = find_mate(s, death.victim);
        if (!mate && t > death.time + fresh)
            continue;
        const NpcState* victim = DaAi::npc(death.victim);
        if (!victim || !victim->owner || !Know::sees_now(npc, victim->owner))
            continue;
        if (mate)
        {
            learn_death(npc, s, death.victim, death.leader, Source::seen, &death.position);
            continue;
        }

        s.known_deaths.push_back(death.victim);
        switch (npc.tfGetRelationType(victim->owner))
        {
        case ALife::eRelationTypeEnemy:
        case ALife::eRelationTypeWorstEnemy:
            if (engaged)
                s.enemy_kills = u8(_min(255, s.enemy_kills + 1));
            break;
        case ALife::eRelationTypeFriend:
            if (npc.Position().distance_to(death.position) <= g_cfg.ally_radius)
            {
                const bool close = npc.Position().distance_to(death.position) <= g_cfg.fear_close_m;
                add_fear(s, g_cfg.fear_ally_death * sensitivity * (close ? g_cfg.fear_close_mult : 1.f));
            }
            break;
        default: break;
        }
    }
}

void sample_enemy(MoraleState& s, u16 id, float strength, u32 t)
{
    for (auto& sighting : s.sightings)
    {
        if (sighting.id == id)
        {
            sighting.strength = strength;
            sighting.time = t;
            return;
        }
    }
    if (!s.sightings.empty() && s.sightings.size() >= g_cfg.sightings_max)
    {
        s.sightings.erase(std::min_element(s.sightings.begin(), s.sightings.end(),
            [](const auto& a, const auto& b) { return a.time < b.time; }));
    }
    s.sightings.push_back({id, strength, t});
}

const MoraleState::Sighting* sighting_of(const MoraleState& s, u16 id)
{
    const auto found =
        std::find_if(s.sightings.begin(), s.sightings.end(), [id](const auto& sighting) { return sighting.id == id; });
    return found == s.sightings.end() ? nullptr : &*found;
}

// ---------------------------------------------------------------------------------------------------------------
// levels and breaking
// ---------------------------------------------------------------------------------------------------------------

Level level_of(float confidence, u8 current)
{
    const int top = int(Level::heroic);
    int result = 0;
    while (result < top && confidence >= g_cfg.levels[result])
        ++result;

    const int previous = _min(int(current), top);
    if (result > previous)
    {
        while (result > previous && confidence < g_cfg.levels[result - 1] + g_cfg.hysteresis)
            --result;
    }
    else if (result < previous)
    {
        while (result < previous && confidence >= g_cfg.levels[result] - g_cfg.hysteresis)
            ++result;
    }
    return Level(result);
}

// Shift of the break thresholds in fractions of the squad (lore.md 8.3): positive = holds longer.
float break_shift(const CAI_Stalker& npc, const MoraleState& s, const Profile& p, const CEntityAlive& enemy,
    bool leader_contact, int community)
{
    float shift = g_cfg.armour_break[s.armour] + (s.wounded ? g_cfg.wounded_break : 0.f) +
        (s.leader_dead ? p.leader_dead : 0.f);
    if (leader_contact && listed(g_cfg.leader_alive, community))
        shift += g_cfg.leader_alive_break;
    if (sworn_enemy(npc, enemy))
        shift += g_cfg.sworn_break;
    return shift;
}

void clear_plan(MoraleState& s)
{
    s.cover = nullptr;
    s.target_vertex = u32(-1);
    s.hold_until = 0;
    s.replan_at = 0;
    s.hidden_since = 0;
    s.scatter_dir.set(0.f, 0.f, 0.f);
    s.arrived = false;
}

void end_episode(MoraleState& s)
{
    s.broken = false;
    s.style = Style::none;
    clear_plan(s);
}

bool try_surrender(CAI_Stalker& npc, MoraleState& s, const Profile& p, const CEntityAlive& enemy, bool cornered)
{
    // Nobody hands himself over to a mutant; a fanatic never does.
    if (s.surrender_rolled || p.surrender <= 0.f || p.fanatic || !smart_cast<const CInventoryOwner*>(&enemy))
        return false;
    if (!cornered && npc.conditions().GetHealth() >= g_cfg.surrender_health)
        return false;

    s.surrender_rolled = true;
    if (::Random.randF() >= p.surrender)
        return false;

    s.style = Style::surrender;
    s.counted = false;
    clear_plan(s);
    Comms::request(npc, Comms::Kind::surrender, enemy.ID(), 0);
    return true;
}

Style style_of(RoutStyle rout)
{
    switch (rout)
    {
    case RoutStyle::orderly: return Style::orderly;
    case RoutStyle::scatter: return Style::scatter;
    case RoutStyle::relocate: return Style::relocate;
    default: return Style::none;
    }
}

void begin_break(CAI_Stalker& npc, MoraleState& s, const Profile& p, const CEntityAlive& enemy, bool ordered)
{
    s.style = style_of(p.rout_style);
    if (s.style == Style::none)
        return;

    s.broken = true;
    s.break_time = now();
    s.counted = false;
    s.surrender_rolled = false;
    clear_plan(s);
    // A fanatic's relocation is no panic.
    if (s.style != Style::relocate)
        stat_add(Stat::m_panicked, 1.f, DaAi::npc(npc)->community);

    const bool cornered =
        Know::sees_now(npc, &enemy) && Know::known_distance(npc, enemy) <= g_cfg.surrender_cornered_m;
    if (try_surrender(npc, s, p, enemy, cornered))
        return;
    // An order to fall back is not repeated by everyone who obeys it.
    if (s.style == Style::orderly && !ordered)
        Comms::request(npc, Comms::Kind::fall_back, enemy.ID(), 0);
}

void finish_relocation(MoraleState& s)
{
    end_episode(s);
    s.cooldown_until = now() + ms(g_cfg.relocate_cooldown_s);
    reset_engagement(s);
}

// ---------------------------------------------------------------------------------------------------------------
// fatigue and wounds
// ---------------------------------------------------------------------------------------------------------------

// Running drains the profile's sprint budget (its stamina is the fitness: a fit man runs longer and gets his breath
// back sooner), faster in a panic sprint, in heavy armour and when hurt; walking and rest restore it.
void update_stamina(CAI_Stalker& npc, MoraleState& s, const Profile& p, float elapsed, float health)
{
    const float dt = _min(elapsed, g_cfg.stamina_max_dt_s);
    if (dt <= 0.f)
        return;

    const auto& movement = npc.movement();
    const float fitness = _max(.05f, p.stamina);
    const bool moving = movement.speed() > g_cfg.stamina_move_speed;
    float gait = 0.f;
    if (moving && movement.movement_type() == eMovementTypeRun)
    {
        switch (movement.mental_state())
        {
        case eMentalStatePanic: gait = g_cfg.stamina_sprint_mult; break;
        case eMentalStateFree: gait = g_cfg.stamina_jog_mult; break;
        default: gait = 1.f; break;
        }
    }

    if (gait > 0.f)
    {
        const float wounds = 1.f + g_cfg.stamina_wound_k * (1.f - clampr(health, 0.f, 1.f));
        const float drain = gait * g_cfg.stamina_armour[s.armour] * wounds / (_max(1.f, g_cfg.stamina_run_s) * fitness);
        s.stamina = _max(0.f, s.stamina - drain * dt);
    }
    else
    {
        const float recover_s = _max(1.f, moving ? g_cfg.stamina_walk_s : g_cfg.stamina_rest_s);
        s.stamina = _min(1.f, s.stamina + fitness / recover_s * dt);
    }

    if (s.stamina <= g_cfg.stamina_exhausted)
        s.winded = true;
    else if (s.stamina >= g_cfg.stamina_tired)
        s.winded = false;
}

// A badly hurt NPC calls for help while someone who could come is believed near: aloud while it still fights, a
// weak call from the ground once the script's wounded scheme laid it down. Repeated after help_repeat_s, at most
// help_max times, and anew when it goes down. The words carry where it lies (comms: payload = its own position).
void call_for_help(CAI_Stalker& npc, MoraleState& s, const Profile& p, float health, bool threatened)
{
    // A fanatic bears it in silence, as DA's own wounded scheme has it ([ai_morale] help_fanatic).
    if (p.fanatic && !g_cfg.help_fanatic)
        return;
    const bool down = npc.wounded();
    if (!down && health >= g_cfg.help_health)
    {
        s.help_calls = 0;
        s.down = false;
        return;
    }

    const bool fell = down && !s.down;
    s.down = down;
    if ((s.broken && s.style == Style::surrender) || (s.mates.empty() && !s.allies) || (!down && !threatened))
        return;
    if (!fell && (s.help_calls >= g_cfg.help_max || (s.help_time && now() < s.help_time + ms(g_cfg.help_repeat_s))))
        return;
    if (!Comms::request(npc, Comms::Kind::help, npc.ID(), 0, down ? Comms::quiet : 0))
        return;

    s.help_time = now();
    s.help_calls = u8(_min(255, s.help_calls + 1));
    stat_add(Stat::m_help_shout, 1.f, DaAi::npc(npc)->community);
}

// ---------------------------------------------------------------------------------------------------------------
// retreat: covers and movement
// ---------------------------------------------------------------------------------------------------------------

xr_vector<CCoverPoint*> g_nearest;

// The best scored cover within the radius; the score returns `reject` for covers the style must not use. The chosen
// cover is registered with the agent manager like the stock cover searches do.
template <typename Score>
const CCoverPoint* choose_cover(CAI_Stalker& npc, float radius, Score&& score)
{
    const Fvector& position = npc.Position();
    ai().cover_manager().covers().nearest(position, radius, g_nearest);
    CStalkerMovementRestrictor restrictor(&npc, false);
    const CCoverPoint* best = nullptr;
    float best_value = reject;
    for (const CCoverPoint* cover : g_nearest)
    {
        if (cover->m_is_smart_cover || position.distance_to_sqr(cover->position()) > _sqr(radius) ||
            _abs(position.y - cover->position().y) > g_cfg.cover_max_dy)
        {
            continue;
        }
        if (!npc.movement().restrictions().accessible(cover->position()) || !restrictor(cover))
            continue;
        float value = score(*cover);
        if (value == reject)
            continue;
        value -= g_cfg.cover_w_danger * (1.f - restrictor.weight(cover));
        if (value > best_value)
        {
            best = cover;
            best_value = value;
        }
    }
    restrictor.finalize(best);
    return best;
}

// The farthest reachable vertex in a straight line, like the script's vertex_in_direction.
u32 vertex_toward(CAI_Stalker& npc, const Fvector& direction, float distance)
{
    const u32 start = npc.ai_location().level_vertex_id();
    const CLevelGraph& graph = ai().level_graph();
    if (!graph.valid_vertex_id(start) || !npc.movement().restrictions().accessible(start))
        return u32(-1);

    const Fvector from = graph.vertex_position(start);
    const Fvector to = Fvector().mad(from, direction, distance);
    u32 result = u32(-1);
    npc.movement().restrictions().add_border(start, distance);
    graph.farthest_vertex_in_direction(start, from, to, result, nullptr, true);
    npc.movement().restrictions().remove_border();
    if (!graph.valid_vertex_id(result) || result == start || !npc.movement().restrictions().accessible(result))
        return u32(-1);
    return result;
}

bool has_target(const MoraleState& s) { return ai().level_graph().valid_vertex_id(s.target_vertex); }

void set_target(MoraleState& s, const CCoverPoint& cover)
{
    s.cover = &cover;
    s.target = cover.position();
    s.target_vertex = cover.level_vertex_id();
    s.arrived = false;
}

void set_target(MoraleState& s, u32 vertex)
{
    s.cover = nullptr;
    s.target = ai().level_graph().vertex_position(vertex);
    s.target_vertex = vertex;
    s.arrived = false;
}

// A tired man no longer sprints; a winded one walks until he has his breath back.
void move(CAI_Stalker& npc, const MoraleState& s, EMovementType type, EMentalState mental)
{
    if (mental == eMentalStatePanic && s.stamina < g_cfg.stamina_tired)
        mental = eMentalStateDanger;
    if (s.winded)
    {
        type = eMovementTypeWalk;
        mental = eMentalStateDanger;
    }

    auto& movement = npc.movement();
    movement.set_desired_direction(nullptr);
    movement.set_path_type(MovementManager::ePathTypeLevelPath);
    movement.set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    movement.set_level_dest_vertex(s.target_vertex);
    movement.set_desired_position(&s.target);
    movement.set_body_state(eBodyStateStand);
    movement.set_movement_type(type);
    movement.set_mental_state(mental);
}

void stay(CAI_Stalker& npc, EBodyState body, EMentalState mental)
{
    auto& movement = npc.movement();
    movement.set_desired_direction(nullptr);
    movement.set_path_type(MovementManager::ePathTypeLevelPath);
    movement.set_detail_path_type(DetailPathManager::eDetailPathTypeSmooth);
    movement.set_nearest_accessible_position();
    movement.set_movement_type(eMovementTypeStand);
    // The movement manager throws on a crouch with a free mind, from either side: a crouch takes the mind first, a
    // free mind takes the standing body first (the NPC may come from a calm action).
    if (body == eBodyStateCrouch)
    {
        movement.set_mental_state(mental);
        movement.set_body_state(body);
    }
    else
    {
        movement.set_body_state(body);
        movement.set_mental_state(mental);
    }
}

// Rally point of an orderly withdrawal: where the NPC was last calm, else its smart terrain, if either lies away
// from the threat and within reach.
bool rally_point(const CAI_Stalker& npc, const MoraleState& s, const Fvector& threat, Fvector& result)
{
    const Fvector& position = npc.Position();
    const float distance = position.distance_to(threat);
    const auto usable = [&](const Fvector& point) {
        return position.distance_to(point) <= g_cfg.rally_max_m && threat.distance_to(point) > distance;
    };
    if (s.safe_time && usable(s.safe_pos))
    {
        result = s.safe_pos;
        return true;
    }

    if (!ai().get_alife())
        return false;
    const auto* self = smart_cast<const CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(npc.ID(), true));
    if (!self || self->m_smart_terrain_id == no_id)
        return false;
    const CSE_ALifeDynamicObject* smart = ai().alife().objects().object(self->m_smart_terrain_id, true);
    if (!smart || !ai().game_graph().valid_vertex_id(smart->m_tGraphID) ||
        ai().game_graph().vertex(smart->m_tGraphID)->level_id() != ai().level_graph().level_id() ||
        !usable(smart->o_Position))
    {
        return false;
    }
    result = smart->o_Position;
    return true;
}

const CCoverPoint* orderly_cover(CAI_Stalker& npc, const MoraleState& s, const Fvector& threat, float distance)
{
    const Fvector& position = npc.Position();
    const Fvector to_threat = flat_direction(position, threat);
    const float back_cos = _cos(deg2rad(g_cfg.retreat_back_angle));
    Fvector rally;
    const bool has_rally = rally_point(npc, s, threat, rally);
    const float rally_distance = has_rally ? position.distance_to(rally) : 0.f;
    const float step = _max(1.f, g_cfg.orderly_step_m);

    return choose_cover(npc, step, [&](const CCoverPoint& cover) {
        const float enemy_distance = threat.distance_to(cover.position());
        const float gain = enemy_distance - distance;
        if (gain < g_cfg.orderly_min_gain_m || enemy_distance < g_cfg.retreat_min_enemy_m)
            return reject;
        const float travel = position.distance_to(cover.position());
        // Never a path that starts toward the enemy.
        if (travel < g_cfg.arrive_m || flat_direction(position, cover.position()).dotproduct(to_threat) > back_cos)
            return reject;

        float value = g_cfg.cover_w_gain * gain / step +
            g_cfg.cover_w_protect * protection(cover.level_vertex_id(), cover.position(), threat) -
            g_cfg.cover_w_travel * travel / step;
        if (has_rally)
        {
            const float progress = (rally_distance - cover.position().distance_to(rally)) / step;
            value += g_cfg.cover_w_rally * clampr(progress, -1.f, 1.f);
        }
        return value;
    });
}

const CCoverPoint* scatter_cover(CAI_Stalker& npc, const MoraleState& s, const Fvector& threat, float distance)
{
    const Fvector& position = npc.Position();
    const float cone = _cos(deg2rad(g_cfg.scatter_cone));
    const float reach = _max(1.f, g_cfg.scatter_dist);

    return choose_cover(npc, g_cfg.scatter_search_m, [&](const CCoverPoint& cover) {
        const float travel = position.distance_to(cover.position());
        const float align = flat_direction(position, cover.position()).dotproduct(s.scatter_dir);
        const float enemy_distance = threat.distance_to(cover.position());
        if (travel < g_cfg.arrive_m || align < cone || enemy_distance <= distance)
            return reject;
        return align + g_cfg.cover_w_protect * protection(cover.level_vertex_id(), cover.position(), threat) +
            g_cfg.cover_w_gain * (enemy_distance - distance) / reach -
            g_cfg.cover_w_travel * _abs(travel - reach) / reach;
    });
}

// Out of the enemy's sight after a run: the nearest cover that hides from him, never closer to him.
const CCoverPoint* hide_cover(CAI_Stalker& npc, const Fvector& threat)
{
    const Fvector& position = npc.Position();
    const float distance = position.distance_to(threat);
    const float radius = _max(1.f, g_cfg.scatter_hide_radius);

    return choose_cover(npc, radius, [&](const CCoverPoint& cover) {
        const float value = protection(cover.level_vertex_id(), cover.position(), threat);
        if (value < g_cfg.in_cover_protection || threat.distance_to(cover.position()) < distance - g_cfg.arrive_m)
            return reject;
        return g_cfg.cover_w_protect * value - g_cfg.cover_w_travel * position.distance_to(cover.position()) / radius;
    });
}

// A fanatic's "retreat": another cover to the side, never farther from the fight.
const CCoverPoint* relocate_cover(CAI_Stalker& npc, const Fvector& threat, float distance)
{
    const Fvector& position = npc.Position();
    const Fvector bearing = flat_direction(threat, position);
    const float lateral_cos = _cos(deg2rad(g_cfg.relocate_min_angle));
    const float radius = _max(1.f, g_cfg.relocate_radius);

    return choose_cover(npc, radius, [&](const CCoverPoint& cover) {
        const float enemy_distance = threat.distance_to(cover.position());
        if (enemy_distance > distance + g_cfg.relocate_tolerance_m || enemy_distance < g_cfg.relocate_min_enemy_m)
            return reject;
        const float travel = position.distance_to(cover.position());
        const float side = flat_direction(threat, cover.position()).dotproduct(bearing);
        // lateral: at least the minimum angle off the current bearing, and never past the enemy's flank
        if (travel < g_cfg.relocate_min_travel_m || side > lateral_cos || side < 0.f)
            return reject;
        return g_cfg.cover_w_protect * protection(cover.level_vertex_id(), cover.position(), threat) +
            g_cfg.cover_w_gain * (distance - enemy_distance) / radius - g_cfg.cover_w_travel * travel / radius;
    });
}

void look(CAI_Stalker& npc, RetreatStep& step, const CEntityAlive& enemy, const Fvector& threat, bool visible,
    bool moving, bool may_fire)
{
    if (visible && may_fire)
    {
        npc.sight().setup(CSightAction(&enemy, true, true));
        step.fire = true;
    }
    else if (moving)
        npc.sight().setup(CSightAction(SightManager::eSightTypePathDirection, false));
    else
    {
        npc.sight().setup(CSightAction(SightManager::eSightTypePosition, threat, true));
        step.aim = may_fire;
    }
}

RetreatStep surrender_step(CAI_Stalker& npc, const Fvector& threat)
{
    stay(npc, eBodyStateStand, eMentalStateFree);
    npc.sight().setup(CSightAction(SightManager::eSightTypePosition, threat, true));
    RetreatStep step;
    step.handled = true;
    return step;
}

// Withdraw cover to cover away from the enemy toward the rally point, fighting back when he shows himself.
RetreatStep orderly(CAI_Stalker& npc, MoraleState& s, const Profile& p, const CEntityAlive& enemy,
    const Fvector& threat, bool visible)
{
    const u32 t = now();
    const Fvector& position = npc.Position();
    const float distance = position.distance_to(threat);

    if (has_target(s) && !s.arrived && position.distance_to(s.target) <= g_cfg.arrive_m)
    {
        s.arrived = true;
        s.hold_until = t + ms(random(g_cfg.orderly_hold_s));
    }

    const bool far_enough = distance >= g_cfg.orderly_safe_m;
    const bool stale = !s.arrived && t >= s.replan_at && s.target.distance_to(threat) < distance;
    if (!far_enough && (!has_target(s) || (s.arrived && t >= s.hold_until) || stale))
    {
        if (const CCoverPoint* cover = orderly_cover(npc, s, threat, distance))
        {
            set_target(s, *cover);
            s.replan_at = t + ms(g_cfg.replan_s);
        }
        else if (!has_target(s))
        {
            // Nowhere to fall back to.
            if (try_surrender(npc, s, p, enemy, true))
                return surrender_step(npc, threat);
        }
        else if (s.arrived)
            s.hold_until = t + ms(random(g_cfg.orderly_hold_s));
    }

    RetreatStep step;
    step.handled = true;
    const bool moving = has_target(s) && !s.arrived && !far_enough;
    if (moving)
        move(npc, s, visible ? eMovementTypeWalk : eMovementTypeRun, eMentalStateDanger);
    else
        stay(npc, eBodyStateCrouch, eMentalStateDanger);
    look(npc, step, enemy, threat, visible, moving, true);
    return step;
}

// Run in a direction of one's own, no fire, drop into cover once out of sight.
RetreatStep scatter(CAI_Stalker& npc, MoraleState& s, const Profile& p, const CEntityAlive& enemy,
    const Fvector& threat, bool visible)
{
    const u32 t = now();
    const Fvector& position = npc.Position();
    const float distance = position.distance_to(threat);

    if (visible)
        s.hidden_since = 0;
    else if (!s.hidden_since)
        s.hidden_since = t;

    // Found again up close while hiding: run again.
    if (s.arrived && visible && distance < g_cfg.scatter_respot_m)
        clear_plan(s);

    if (s.scatter_dir.square_magnitude() < EPS)
    {
        Fvector away = flat_direction(threat, position);
        if (away.square_magnitude() < EPS)
            away.set(1.f, 0.f, 0.f);
        const float angle = deg2rad(random(g_cfg.scatter_angle)) * (::Random.randI(2) ? 1.f : -1.f);
        s.scatter_dir = rotate_y(away, angle);
    }

    if (has_target(s) && !s.arrived && position.distance_to(s.target) <= g_cfg.arrive_m)
        s.arrived = true;

    // Across open ground, out of sight for a moment, or at a spot without cover: into the nearest cover.
    const bool exposed_spot = s.arrived && !s.cover;
    const bool out_of_sight =
        !s.arrived && !s.cover && s.hidden_since && t >= s.hidden_since + ms(g_cfg.scatter_hide_s);
    if ((exposed_spot || out_of_sight) && t >= s.replan_at)
    {
        s.replan_at = t + ms(g_cfg.replan_s);
        if (const CCoverPoint* cover = hide_cover(npc, threat))
            set_target(s, *cover);
    }

    if (!has_target(s))
    {
        if (const CCoverPoint* cover = scatter_cover(npc, s, threat, distance))
            set_target(s, *cover);
        else if (const u32 vertex = vertex_toward(npc, s.scatter_dir, g_cfg.scatter_dist); vertex != u32(-1))
            set_target(s, vertex);
        else if (try_surrender(npc, s, p, enemy, visible && distance <= g_cfg.surrender_cornered_m))
            return surrender_step(npc, threat);
    }

    RetreatStep step;
    step.handled = true;
    if (has_target(s) && !s.arrived)
    {
        move(npc, s, eMovementTypeRun, eMentalStatePanic);
        npc.sight().setup(CSightAction(SightManager::eSightTypePathDirection, false));
        step.panic_sound = true;
        return step;
    }
    stay(npc, eBodyStateCrouch, eMentalStateDanger);
    npc.sight().setup(CSightAction(SightManager::eSightTypeCover, true));
    return step;
}

// Change cover sideways and keep fighting.
RetreatStep relocate(CAI_Stalker& npc, MoraleState& s, const CEntityAlive& enemy, const Fvector& threat, bool visible)
{
    const u32 t = now();
    const Fvector& position = npc.Position();
    const float distance = position.distance_to(threat);

    if (!has_target(s))
    {
        if (const CCoverPoint* cover = relocate_cover(npc, threat, distance))
        {
            set_target(s, *cover);
            if (!s.counted)
            {
                s.counted = true;
                stat_add(Stat::m_relocate, 1.f, DaAi::npc(npc)->community);
            }
        }
    }

    const bool done = !has_target(s) || position.distance_to(s.target) <= g_cfg.arrive_m ||
        t > s.break_time + ms(g_cfg.relocate_max_s);
    if (done)
        finish_relocation(s);

    RetreatStep step;
    step.handled = true;
    if (done)
        stay(npc, eBodyStateStand, eMentalStateDanger);
    else
        move(npc, s, visible ? eMovementTypeWalk : eMovementTypeRun, eMentalStateDanger);
    look(npc, step, enemy, threat, visible, !done, true);
    return step;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------
// lifetime
// ---------------------------------------------------------------------------------------------------------------

MoraleState* create(CAI_Stalker& npc)
{
    cfg();
    auto* state = xr_new<MoraleState>();
    if (const SquadState* squad = squad_of(npc))
        state->leader = squad->leader;
    return state;
}

void destroy(MoraleState* state) { xr_delete(state); }

// Morale lives in each NPC: a squad-wide morale state would let a man feel losses he never perceived.
MoraleSquad* create_squad(SquadState& squad) { return nullptr; }
void destroy_squad(MoraleSquad* state) {}
void squad_update(SquadState& squad) {}

void level_unload()
{
    g_deaths.clear();
    g_alias.clear();
    g_nearest.clear();
    g_cfg.loaded = false;
}

void on_death(CAI_Stalker& npc, IGameObject* who)
{
    cfg();
    if (g_deaths.size() >= g_cfg.death_records && !g_deaths.empty())
        g_deaths.erase(g_deaths.begin());
    g_deaths.push_back({npc.ID(), is_leader(npc), now(), npc.Position()});
}

void on_hit(CAI_Stalker& npc, const SHit& hit)
{
    MoraleState* s = active_state(npc);
    if (!s)
        return;

    const NpcState* self = DaAi::npc(npc);
    ScopedTimer timer(Stat::p_morale_us, self->community);
    const u32 t = now();
    s->last_threat = t;
    s->quiet = false;

    const auto* shooter = smart_cast<const CEntityAlive*>(hit.initiator());
    const bool friendly = shooter && shooter != &npc && npc.tfGetRelationType(shooter) == ALife::eRelationTypeFriend;
    const float amount =
        (g_cfg.fear_hit + g_cfg.fear_hit_dmg * hit.damage()) * (friendly ? g_cfg.fear_friendly_hit : 1.f);
    add_fear(*s, amount * fear_sensitivity(profile(npc), self->band));

    // Shot at while giving up: run for it.
    if (s->broken && s->style == Style::surrender && !friendly)
    {
        s->style = Style::scatter;
        s->counted = false;
        clear_plan(*s);
    }
}

void on_message(CAI_Stalker& npc, const Comms::Message& message)
{
    MoraleState* s = active_state(npc);
    if (!s)
        return;
    s->quiet = false;

    const NpcState* self = DaAi::npc(npc);
    const Profile& p = profile(npc);
    const float sensitivity = fear_sensitivity(p, self->band);
    const bool from_mate = find_mate(*s, message.sender);
    const bool from_leader = message.sender == s->leader && message.sender != npc.ID();

    // Deaths (man_down / leader_down, heard dying cries) arrive through on_casualty; comms also uses man_down with
    // extra = 1 for a mate who is only wounded.
    using Comms::Kind;
    switch (message.kind)
    {
    case Kind::taking_fire:
    case Kind::grenade:
    case Kind::help:
        if (from_mate)
            s->last_threat = now();
        break;
    case Kind::fall_back:
        if (const CEntityAlive* enemy = npc.memory().enemy().selected();
            from_leader && enemy && !s->broken && p.rout_style == RoutStyle::orderly)
        {
            // The leader orders the withdrawal: the squad goes as a unit.
            begin_break(npc, *s, p, *enemy, true);
        }
        else if (from_mate)
            add_fear(*s, g_cfg.fear_mate_fallback * sensitivity);
        break;
    case Kind::surrender:
        if (from_mate)
            add_fear(*s, g_cfg.fear_mate_fallback * sensitivity);
        break;
    case Kind::attack:
        if (from_leader)
            add_fear(*s, -g_cfg.order_relief);
        break;
    default: break;
    }
}

void on_casualty(CAI_Stalker& npc, const CAI_Stalker& victim, bool was_leader, Source source)
{
    // A heard cry tells how close it was; a report does not.
    const bool reported = source >= Source::report_voice && source <= Source::report_hq;
    if (MoraleState* s = active_state(npc))
        learn_death(npc, *s, victim.ID(), was_leader, source, reported ? nullptr : &victim.Position());
}

// ---------------------------------------------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------------------------------------------

void update(CAI_Stalker& npc, float)
{
    NpcState* self = DaAi::npc(npc);
    MoraleState* s = self ? self->morale : nullptr;
    if (!s)
        return;

    if (!feature(Feature::morale))
    {
        end_episode(*s);
        s->level = s->raw_level = u8(Level::steady);
        s->last_update = 0;
        return;
    }

    const u32 t = now();
    // Every step below is a rate times the elapsed time, so a calm NPC is simply evaluated less often.
    const u32 period = s->quiet && !npc.memory().enemy().selected() ? g_cfg.quiet_update_ms : g_cfg.update_ms;
    if (s->last_update && t < s->last_update + period)
        return;
    const float elapsed = s->last_update ? float(t - s->last_update) / 1000.f : 0.f;
    s->last_update = t;
    ScopedTimer timer(Stat::p_morale_us, self->community);

    const Profile& p = profile(npc);
    const float sensitivity = fear_sensitivity(p, self->band);
    const SquadState* squad = squad_of(npc);
    const CEntityAlive* selected = npc.memory().enemy().selected();
    const auto engaged = [&] { return selected || (s->last_threat && t < s->last_threat + ms(g_cfg.calm_reset_s)); };

    expire_deaths();
    sync_mates(npc, *s, squad);
    sync_leader(*s, squad);
    perceive_deaths(npc, *s, sensitivity, engaged());

    // own condition; the weapon in hand or the last choice (the weapon choice itself is a Lua call the planner makes)
    const float health = npc.conditions().GetHealth();
    const CInventoryItem* weapon = held_weapon(npc);
    const auto* gun = smart_cast<const CWeapon*>(weapon);
    s->wounded = health < g_cfg.wounded_health;
    s->armour = u8(armour_class(npc));
    s->low_ammo = gun && !npc.infinite_ammo() && gun->GetSuitableAmmoTotal() < gun->GetAmmoMagSize();
    update_stamina(npc, *s, p, elapsed, health);

    // Known friendly strength: the NPC, the mates it believes alive (their state refreshed while seen) and friendly
    // fighters in sight. A mate or ally seen running is catching.
    float friends = g_cfg.band_strength[band_index(self->band)] * human_strength(npc, weapon);
    for (auto& mate : s->mates)
    {
        const NpcState* other = DaAi::npc(mate.id);
        if (other && other->owner && !other->dead && Know::sees_now(npc, other->owner))
        {
            mate.strength = mate_strength(*other->owner, other->band);
            if (other->morale && other->morale->broken)
                add_fear(*s, g_cfg.fear_contagion * elapsed * sensitivity);
        }
        friends += mate.strength;
    }
    s->allies = 0;
    for (const auto& seen : npc.memory().visual().objects())
    {
        const auto* ally = smart_cast<const CEntityAlive*>(seen.m_object);
        if (!ally || ally == &npc || !ally->g_Alive() || find_mate(*s, ally->ID()) ||
            !smart_cast<const CInventoryOwner*>(ally) ||
            npc.Position().distance_to(ally->Position()) > g_cfg.ally_radius ||
            npc.tfGetRelationType(ally) != ALife::eRelationTypeFriend || !Know::sees_now(npc, ally))
        {
            continue;
        }
        friends += g_cfg.ally_weight * human_strength(*ally, visible_weapon(*ally));
        s->allies = u8(_min(255, s->allies + 1));
        if (const auto* stalker = smart_cast<const CAI_Stalker*>(ally))
        {
            if (const MoraleState* other = state_of(*stalker); other && other->broken)
                add_fear(*s, g_cfg.fear_contagion * elapsed * sensitivity);
        }
    }

    // Known enemy strength: attributes only from the NPC's own sightings; an enemy known only by report or sound
    // counts at the unknown strength.
    const u32 sighting_memory = ms(g_cfg.sighting_memory_s);
    s->sightings.erase(std::remove_if(s->sightings.begin(), s->sightings.end(),
                           [&](const auto& sighting) { return t > sighting.time + sighting_memory; }),
        s->sightings.end());
    float enemies = 0.f;
    for (const CEntityAlive* enemy : npc.memory().enemy().objects())
    {
        if (!enemy)
            continue;
        if (Know::sees_now(npc, enemy))
        {
            sample_enemy(*s, enemy->ID(), enemy_strength(*enemy), t);
            if (Know::known_distance(npc, *enemy) <= g_cfg.threat_distance_m)
                s->last_threat = t;
        }
        const auto* sighting = sighting_of(*s, enemy->ID());
        enemies += sighting ? sighting->strength : g_cfg.unknown_enemy_strength;
    }
    const float ratio_max = _max(1.f, g_cfg.ratio_max);
    s->ratio = enemies > 0.f ? clampr(friends / enemies, 1.f / ratio_max, ratio_max) : ratio_max;

    // other fear sources
    const float suppression = Sense::suppression(npc);
    if (suppression >= g_cfg.threat_suppression)
        s->last_threat = t;
    add_fear(*s, suppression * g_cfg.fear_supp * elapsed * sensitivity);
    if (const CDangerObject* danger = npc.memory().danger().selected();
        danger && danger->type() == CDangerObject::eDangerTypeGrenade && danger->time() > s->last_grenade &&
        npc.Position().distance_to(danger->position()) <= g_cfg.fear_grenade_m)
    {
        s->last_grenade = danger->time();
        s->last_threat = t;
        add_fear(*s, g_cfg.fear_grenade * sensitivity);
    }

    // Fear fades, faster in cover.
    const Fvector threat = selected ? Know::known_position(npc, *selected) : npc.Position();
    const bool in_cover = selected &&
        protection(npc.ai_location().level_vertex_id(), npc.Position(), threat) >= g_cfg.in_cover_protection;
    const float tau =
        _max(1.f, p.recover_s * g_cfg.fear_decay_k) / (in_cover ? _max(1.f, g_cfg.cover_decay_mult) : 1.f);
    s->fear = clampr(s->fear - s->fear * _min(1.f, elapsed / tau), 0.f, 1.f);

    // Calm: the squad as it stands now is the baseline for the next fight; the spot is a rally point.
    if (!engaged() && !s->broken)
    {
        reset_engagement(*s);
        if (t >= s->safe_time + ms(g_cfg.safe_record_s) &&
            ai().level_graph().valid_vertex_id(npc.ai_location().level_vertex_id()))
        {
            s->safe_pos = npc.Position();
            s->safe_time = t;
        }
    }

    // confidence (Days Gone form) and its level
    const float loss_fraction = s->baseline ? _min(1.f, float(s->losses) / float(s->baseline)) : 0.f;
    const bool contact = leader_in_contact(npc, *s);
    float confidence = g_cfg.conf_base + g_cfg.ratio_k * std::log2(s->ratio) + g_cfg.courage_k * (p.courage - .5f) -
        g_cfg.health_k * (1.f - health) - (s->low_ammo ? g_cfg.ammo_k : 0.f) - g_cfg.fear_k * s->fear -
        g_cfg.loss_k * loss_fraction + _min(g_cfg.kill_max, g_cfg.kill_k * s->enemy_kills) +
        (contact ? g_cfg.leader_conf : 0.f);
    if (p.fanatic)
        confidence = _max(confidence, g_cfg.levels[size_t(Level::worried)] + g_cfg.hysteresis);
    s->raw_level = u8(level_of(confidence, s->raw_level));

    // breaking and recovering
    const bool threatened = s->last_threat && t <= s->last_threat + ms(g_cfg.threat_window_s);
    if (!s->broken)
    {
        if (selected && p.rout_style != RoutStyle::never && t >= s->cooldown_until)
        {
            const float shift = break_shift(npc, *s, p, *selected, contact, self->community);
            const bool casualties = s->losses && p.break_cas < 1.f && loss_fraction >= p.break_cas + shift &&
                s->ratio < _max(1.f, p.winning_ratio);
            if (threatened && s->ratio < p.break_ratio * (1.f - shift))
                s->low_ratio_since = s->low_ratio_since ? s->low_ratio_since : t;
            else
                s->low_ratio_since = 0;
            const bool outmatched = s->low_ratio_since && t >= s->low_ratio_since + ms(g_cfg.ratio_hold_s);
            const bool terrified = !p.fanatic && threatened && s->raw_level == u8(Level::panicked);
            if (casualties || outmatched || terrified)
                begin_break(npc, *s, p, *selected, false);
        }
    }
    else if (s->style == Style::relocate)
    {
        if (t > s->break_time + ms(g_cfg.relocate_max_s))
            finish_relocation(*s);
    }
    else if (t >= s->last_threat + ms(p.recover_s))
    {
        end_episode(*s);
        reset_engagement(*s);
        stat_add(Stat::m_recovered, 1.f, self->community);
    }

    const u8 previous = s->level;
    if (s->broken && s->style != Style::relocate)
        s->level = u8(s->style == Style::surrender ? Level::surrendered : Level::panicked);
    else if (s->wounded && threatened && !p.fanatic)
    {
        // Hurt under fire, a man looks for cover first (tactics falls back from a worried NPC in the open).
        s->level = _min(s->raw_level, g_cfg.wounded_max_level);
    }
    else
        s->level = s->raw_level;
    if (s->level == u8(Level::worried) && previous != u8(Level::worried))
        stat_add(Stat::m_worried, 1.f, self->community);

    call_for_help(npc, *s, p, health, threatened);
    s->quiet = !engaged() && !s->broken && !s->down && !s->wounded;
}

// ---------------------------------------------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------------------------------------------

Level level(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s ? Level(s->level) : Level::steady;
}

float fear(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s ? s->fear : 0.f;
}

float ratio(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s ? s->ratio : 1.f;
}

float aggression(const CAI_Stalker& npc)
{
    const Profile& p = profile(npc);
    const MoraleState* s = active_state(npc);
    if (!s)
        return p.aggr;
    const float base = s->ratio >= p.winning_ratio ? _max(p.aggr, p.aggr_winning) : p.aggr;
    // A man giving himself up has no heart for the fight.
    const size_t level_index = s->level == u8(Level::surrendered) ? size_t(Level::panicked) : s->level;
    const float heart = g_cfg.level_aggr[_min(level_index, std::size(g_cfg.level_aggr) - 1)];
    float result = base + heart + g_cfg.armour_aggr[s->armour] + (s->wounded ? g_cfg.wounded_aggr : 0.f) +
        (s->low_ammo ? g_cfg.low_ammo_aggr : 0.f) + (p.fanatic ? 0.f : g_cfg.fear_aggr * s->fear);
    // A heroic fanatic pushes, wounds or not.
    if (p.fanatic && s->level == u8(Level::heroic))
        result = _max(result, g_cfg.fanatic_push_aggr);
    return clampr(result, 0.f, 1.f);
}

float cover(const CAI_Stalker& npc)
{
    const Profile& p = profile(npc);
    const MoraleState* s = active_state(npc);
    return s ? clampr(p.cover + g_cfg.armour_cover[s->armour], 0.f, 1.f) : p.cover;
}

bool sworn_enemy(const CAI_Stalker& npc, const CEntityAlive& enemy)
{
    const auto* owner = smart_cast<const CInventoryOwner*>(&enemy);
    if (!owner || cfg().sworn.empty())
        return false;
    const int a = base_community(npc.Community());
    const int b = base_community(owner->Community());
    return std::any_of(g_cfg.sworn.begin(), g_cfg.sworn.end(), [a, b](const auto& pair) {
        return (pair.first == a && pair.second == b) || (pair.first == b && pair.second == a);
    });
}

u16 believed_leader(const CAI_Stalker& npc)
{
    if (const MoraleState* s = active_state(npc))
        return s->leader;
    const SquadState* squad = squad_of(npc);
    return squad ? squad->leader : no_id;
}

bool panicking(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s && s->broken;
}

Style style(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s && s->broken ? s->style : Style::none;
}

float accuracy_mult(const CAI_Stalker& npc)
{
    // Covering fire while withdrawing is hasty.
    const MoraleState* s = active_state(npc);
    return s && s->broken && s->style == Style::orderly ? 1.f + g_cfg.acc_withdraw : 1.f;
}

float stamina(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s ? s->stamina : 1.f;
}

bool tired(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s && s->stamina < g_cfg.stamina_tired;
}

bool winded(const CAI_Stalker& npc)
{
    const MoraleState* s = active_state(npc);
    return s && s->winded;
}

// ---------------------------------------------------------------------------------------------------------------
// retreat action
// ---------------------------------------------------------------------------------------------------------------

void retreat_begin(CAI_Stalker& npc)
{
    if (MoraleState* s = active_state(npc); s && s->broken && s->style != Style::relocate)
        clear_plan(*s);
}

void retreat_end(CAI_Stalker& npc)
{
    if (MoraleState* s = state_of(npc))
        clear_plan(*s);
}

RetreatStep retreat_execute(CStalkerActionRetreatFromEnemy& action)
{
    CAI_Stalker& npc = action.object();
    MoraleState* s = active_state(npc);
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    if (!s || !s->broken || !enemy)
        return {};

    const NpcState* self = DaAi::npc(npc);
    ScopedTimer timer(Stat::p_morale_us, self->community);
    if (!s->counted && s->style != Style::relocate)
    {
        s->counted = true;
        switch (s->style)
        {
        case Style::orderly: stat_add(Stat::m_retreat_orderly, 1.f, self->community); break;
        case Style::scatter: stat_add(Stat::m_retreat_scatter, 1.f, self->community); break;
        case Style::surrender: stat_add(Stat::m_surrender, 1.f, self->community); break;
        default: break;
        }
    }

    const Profile& p = profile(npc);
    const Fvector threat = Know::known_position(npc, *enemy);
    const bool visible = Know::sees_now(npc, enemy);
    switch (s->style)
    {
    case Style::orderly: return orderly(npc, *s, p, *enemy, threat, visible);
    case Style::scatter: return scatter(npc, *s, p, *enemy, threat, visible);
    case Style::relocate: return relocate(npc, *s, *enemy, threat, visible);
    case Style::surrender: return surrender_step(npc, threat);
    default: return {};
    }
}
} // namespace DaAi::Morale
