// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "da_ai_knowledge.h"

// Morale lane: confidence and fear from what the NPC knows, casualties and leader death by perception, retreat
// styles, surrender, fanatics, wounds and fatigue.

class CAI_Stalker;
class CCoverPoint;
class CEntityAlive;
class CScriptGameObject;
class CStalkerActionRetreatFromEnemy;
class IGameObject;
struct SHit;

template <typename _object_type>
class CPropertyEvaluator;

namespace DaAi
{
struct SquadState;
namespace Comms
{
struct Message;
}

// [ai:morale] begin state
namespace Morale
{
// How a broken NPC leaves the fight (the profile's rout_style, or a surrender).
enum class Style : u8
{
    none,
    orderly,
    scatter,
    relocate,
    surrender
};
} // namespace Morale

struct MoraleState
{
    // A squad mate as this NPC believes him: alive until his death is perceived or reported.
    struct Mate
    {
        u16 id;
        float strength;
    };

    // Enemy attributes, sampled only while the enemy is in the NPC's own vision.
    struct Sighting
    {
        u16 id;
        float strength;
        u32 time;
    };

    xr_vector<Mate> mates;
    xr_vector<Sighting> sightings;
    xr_vector<u16> known_deaths; // victims whose death this NPC has already taken into account

    float fear{};
    float ratio{1.f}; // known friendly strength / known enemy strength
    u8 level{2}; // Morale::Level
    u8 raw_level{2};

    // own condition, sampled at each evaluation
    u8 armour{}; // ArmourClass
    bool wounded{};
    bool low_ammo{};

    // engagement bookkeeping (reset when the NPC is calm again)
    u8 baseline{1}; // believed strength of the squad in men at the last calm moment, the NPC included
    u8 losses{}; // squad deaths learned since then
    u8 enemy_kills{}; // enemy deaths seen since then
    u32 last_update{};
    u32 last_threat{};
    u32 low_ratio_since{};
    u32 last_grenade{};

    // leadership as believed
    u16 leader{u16(-1)};
    u32 handover_at{};
    bool leader_dead{};

    // break episode
    Morale::Style style{};
    bool broken{};
    bool counted{}; // the episode's style counter was emitted
    bool surrender_rolled{};
    u32 break_time{};
    u32 cooldown_until{};

    // retreat plan of the running episode
    const CCoverPoint* cover{};
    Fvector target{};
    u32 target_vertex{u32(-1)};
    u32 hold_until{};
    u32 replan_at{};
    u32 hidden_since{};
    Fvector scatter_dir{};
    bool arrived{};

    // rally point: where the NPC last was calm
    Fvector safe_pos{};
    u32 safe_time{};

    // fatigue: 1 fresh .. 0 exhausted; winded = exhausted, walks until it has its breath back
    float stamina{1.f};
    bool winded{};

    // calls for help while wounded
    u32 help_time{};
    u8 help_calls{};
    bool down{}; // laid down by the script's wounded scheme
    u8 allies{}; // friendly fighters of other squads in sight at the last evaluation
    // Calm, unhurt and not broken at the last evaluation: the next one waits [ai_morale] quiet_update_ms unless an
    // enemy is selected, a hit or a message arrives or a death is learned.
    bool quiet{};
};

struct MoraleSquad
{
};
// [ai:morale] end state

namespace Morale
{
// Lua ai_morale returns the index: panicked 0 .. heroic 4; surrendered 5 (the script plays hands_up).
enum class Level : u8
{
    panicked,
    worried,
    steady,
    confident,
    heroic,
    surrendered
};

MoraleState* create(CAI_Stalker& npc);
void destroy(MoraleState* state);
MoraleSquad* create_squad(SquadState& squad);
void destroy_squad(MoraleSquad* state);
void update(CAI_Stalker& npc, float dt);
void squad_update(SquadState& squad);
void on_hit(CAI_Stalker& npc, const SHit& hit);
void on_death(CAI_Stalker& npc, IGameObject* who);
void on_message(CAI_Stalker& npc, const Comms::Message& message);
void level_unload();

Level level(const CAI_Stalker& npc);
float fear(const CAI_Stalker& npc);
// A friendly casualty the NPC perceived or was told about (comms: a heard dying cry, Source::heard_voice).
void on_casualty(CAI_Stalker& npc, const CAI_Stalker& victim, bool was_leader, Source source);
// Dispersion multiplier for what tactics does not already apply: hasty fire while withdrawing. Fear's reaction and
// settle time and fatigue are tactics' ([ai_tactics] react_/settle_/acc_ keys read fear() and stamina()).
float accuracy_mult(const CAI_Stalker& npc);
// 1 fresh .. 0 exhausted: running drains the profile's sprint budget, rest restores it; 1 without state.
float stamina(const CAI_Stalker& npc);
// Too tired to sprint ([ai_morale] stamina_tired); winded: exhausted, walks until stamina_tired again.
bool tired(const CAI_Stalker& npc);
bool winded(const CAI_Stalker& npc);

// Known friendly strength / known enemy strength (Days Gone confidence ratio), 1 without state.
float ratio(const CAI_Stalker& npc);
// Effective aggression: the profile's aggr (aggr_winning while the ratio holds winning_ratio) with the armour,
// wound, ammo, fear and confidence modifiers of [ai_modifiers]; a heroic fanatic pushes (fanatic_push_aggr).
float aggression(const CAI_Stalker& npc);
// Effective cover discipline: the profile's cover with the armour modifier of [ai_modifiers].
float cover(const CAI_Stalker& npc);
// The enemy's community is a sworn enemy of the NPC's ([ai_modifiers] sworn).
bool sworn_enemy(const CAI_Stalker& npc, const CEntityAlive& enemy);
// The squad leader as this NPC believes it: a dead leader until his death is learned, nobody until the handover.
// Without morale state, the squad's AI leader. An NPC acts as leader when this is its own id.
u16 believed_leader(const CAI_Stalker& npc);
// true while the NPC is broken and its style takes it out of the fight (the Panic property).
bool panicking(const CAI_Stalker& npc);
Style style(const CAI_Stalker& npc);

// Body of the retreat action (the [ai:morale] block of stalker_combat_actions.cpp).
struct RetreatStep
{
    bool handled{}; // false: the stock retreat runs
    bool fire{}; // fire at the selected enemy, which is in the NPC's own vision
    bool aim{}; // weapon ready, no fire
    bool panic_sound{};
};
void retreat_begin(CAI_Stalker& npc);
RetreatStep retreat_execute(CStalkerActionRetreatFromEnemy& action);
void retreat_end(CAI_Stalker& npc);

// planner (replaces the stock Panic evaluator of the combat planner)
CPropertyEvaluator<CScriptGameObject>* make_panic_evaluator(CAI_Stalker* object, pcstr name);
} // namespace Morale
} // namespace DaAi
