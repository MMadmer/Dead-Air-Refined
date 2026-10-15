// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "da_ai.h"

// Behaviour profiles of the human NPC AI, resolved from configs/dead_air_x64_ai.ltx by specific character, community
// and rank band; weapon and armour classes; the radio trait rolled once per NPC lifetime (.scov chunk AIT1).

class CAI_Stalker;
class CEntityAlive;
class CInventoryItem;

namespace DaAi
{
enum class WeaponClass : u8
{
    none,
    pistol,
    smg,
    shotgun,
    rifle_old,
    rifle,
    mg,
    dmr,
    launcher,
    count
};

// Engagement band of a weapon class in metres ([ai_weapon_classes]).
struct WeaponBand
{
    float effective{};
    float engage_max{};
    float min{};
};

enum class ArmourClass : u8
{
    none,
    light,
    heavy,
    exo,
    count
};

enum class Voice : u8
{
    automatic, // hushed when the NPC's own section leaves its combat bark keys empty
    loud,
    hushed,
    none
};

enum class RoutStyle : u8
{
    orderly,
    scatter,
    relocate,
    never
};

enum class SearchStyle : u8
{
    pairs,
    single,
    straight,
    none
};

enum class Investigate : u8
{
    pair,
    single,
    none,
    advance
};

enum class Role : u8
{
    opportunist,
    suppressor,
    flanker,
    grenadier,
    rearguard,
    count
};

// A random range "lo, hi" of the ltx (react_ms = 700, 1000).
struct Range
{
    float lo{};
    float hi{};
};

// One resolved profile. The initial values are the compiled-in defaults that apply when even [ai_profile_default]
// is missing; lanes read profiles only through this struct. A key is read from the first section of the chain that
// has it: [ai_profile_by_character] match (+_r<band>), ai_profile_<community>_r<band>, ai_profile_<community>,
// ai_profile_default_r<band>, ai_profile_default.
struct Profile
{
    static constexpr size_t max_chain = 6;

    shared_str section; // head of the chain
    shared_str chain[max_chain];
    u8 chain_size{};
    float rank_scale{1.f}; // [ai_rank_scale] of the rank name, already applied to react_ms and settle_s

    // perception
    Range react_ms{700.f, 1000.f};
    float react_idle_ms{0.f};
    float settle_s{1.f};
    float detect_rate{1.f};
    float hear_skill{.5f};
    float hear_gain{1.f};
    float aim_head{.15f};
    float lead_error{.4f};
    float light_discipline{.5f};

    // comms
    float radio_chance[4]{.2f, .2f, .3f, .4f}; // by band R1..R4
    float radio_leader_bonus{.2f};
    u8 hq_relay{}; // 0 = none, else the minimum band of an HQ relay
    Range hq_delay_s{60.f, 120.f};
    Voice voice{Voice::automatic};
    u8 gestures{3}; // minimum band, 0 = never
    bool pda{};
    Range decide_ms{400.f, 900.f};
    Range understand_ms{300.f, 700.f};
    bool callouts{true};

    // tactics
    float aggr{.5f};
    // aggression while the known strength ratio is at least winning_ratio; an empty ltx value follows aggr
    float aggr_winning{.5f};
    float winning_ratio{1.5f};
    float cover{.7f};
    float flank{.3f};
    float supp{.3f};
    float supp_window_s{3.f};
    float gren{.2f};
    float peek{.7f}; // an empty ltx value follows cover
    bool bounding{};
    bool fire_on_order{};
    // caught in the open by an enemy this close who sees it: return fire before moving to cover (0 = cover first)
    float return_fire_m{};
    // the ordered flanker fires at what it sees on the move (else it holds its fire until it reaches the cover)
    bool flank_fire{};
    u32 roles{(1u << u32(Role::count)) - 1}; // bit per Role
    // grenade_issue = item, count, chance: count grenades at spawn plus one more with chance (empty = none)
    shared_str grenade_item;
    u32 grenade_count{};
    float grenade_chance{};

    // morale
    float courage{.5f};
    float break_cas{.45f};
    float break_ratio{.5f};
    float leader_dead{-.1f};
    RoutStyle rout_style{RoutStyle::orderly};
    float surrender{0.f};
    float recover_s{30.f};
    bool fanatic{};
    float stamina{.6f};

    // search
    // no own sighting and no newer heard or told estimate this long: the ambush chain hands over to the search
    // (negative = never, the stock chain alone)
    float search_delay_s{8.f};
    float search_s{90.f};
    SearchStyle search_style{SearchStyle::single};
    Investigate investigate{Investigate::single};
    bool quiet_search{};
    Range giveup_stagger_s{4.f, 12.f};

    bool has_role(Role role) const { return (roles >> u32(role)) & 1u; }
};

// The NPC's resolved profile; the default one for an NPC without AI state. Never null.
const Profile& profile(const CAI_Stalker& npc);
const Profile& default_profile();
// The value of a key of the NPC's resolved profile as a string, "" when absent (Lua ai_profile). Profile keys come
// back resolved (rank scale applied, derived keys filled); other keys are read raw from the section chain. The
// buffer is reused by the next call.
pcstr profile_value(const CAI_Stalker& npc, pcstr key);

// Resolution at spawn and on reload / community change (fills NpcState::profile and band).
void resolve_profile(CAI_Stalker& npc);
void invalidate_profile(CAI_Stalker& npc);

// Rank band 1..4 of a rank value ([game_relations] rating -> [ai_rank_bands]).
u8 band_of_rank(int rank);
// crc32 of the NPC's section name: the key of its trait record (AIT1).
u32 section_crc(const CAI_Stalker& npc);

// Class of a weapon section by the ordered rules of [ai_weapon_class_rules], cached per section; the item overload
// also sees an attached scope.
WeaponClass weapon_class(const CInventoryItem& item);
WeaponClass weapon_class(const shared_str& section);
pcstr weapon_class_name(WeaponClass which);
const WeaponBand& weapon_band(WeaponClass which);
// Band of an item as carried: its class band with the scoped rifle and shotgun slug modifiers.
WeaponBand weapon_band(const CInventoryItem& item);
// The weapon in the NPC's hands, else the one it last chose. Never runs the weapon choice (Lua), so it is safe for
// other NPCs and off the brain's own update.
const CInventoryItem* held_weapon(const CAI_Stalker& npc);

// Armour class of a living entity: outfit or visual override, else the outfit's fire-wound protection, else the
// entity's own fire-wound immunity.
ArmourClass armour_class(const CEntityAlive& who);

// Weapon classes and rules, read with [ai_config] at the first spawn of a level.
void load_profile_config();
// Frees the level's resolved profiles and class caches; runs after the registry, which points into them.
void profile_level_unload();

// Radio trait of 2.3.4: rolled once per NPC lifetime, kept in the session trait table and saved in AIT1.
namespace Traits
{
void roll_radio(CAI_Stalker& npc);
void save_begin(ChunkCapture& state);
bool save_continue(ChunkCapture& state, float budget_ms);
bool stage(const xr_vector<u8>& payload);
void clear();
void forget(u16 object_id);
} // namespace Traits
} // namespace DaAi
