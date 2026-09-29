// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include <chrono>

// Human NPC AI: feature switches, counters, timers, the ray budget and every hook the engine calls. Mutants never
// reach this code: each hook is placed on a CAI_Stalker path, and state exists only for alive online stalkers.

class CAI_Stalker;
class CWeapon;
class CTorch;
class IGameObject;
struct SHit;
struct SGameMtlPair;

namespace DaAi
{
// Runtime switches of the console command ai_feature. Console only, never saved, all on by default; a switched off
// feature falls back to the stock engine path.
enum class Feature : u8
{
    knowledge,
    awareness,
    concealment,
    full_los,
    hearing,
    noise,
    light,
    near_miss,
    comms,
    radio,
    doctrine,
    cover_score,
    roles,
    suppression,
    grenades,
    reaction,
    settle,
    morale,
    search,
    scripts,
    count
};

extern u32 g_features;
// Console ai_trace (QA, never saved): per-NPC lines of the decisions that the period counters cannot explain.
extern int g_trace;

inline bool feature(Feature which) { return (g_features >> u32(which)) & 1u; }
inline bool tracing() { return g_trace != 0; }
pcstr feature_name(Feature which);
bool feature_by_name(pcstr name, Feature& result);

// How a counter is printed: Count and Sum add up, Avg divides by the number of samples, Min and Max keep the
// extreme, Rate is divided by the seconds since the last print, Gauge is sampled from the registry at print time.
enum class StatKind : u8
{
    Count,
    Sum,
    Avg,
    Min,
    Max,
    Rate,
    Gauge
};

// clang-format off
// [ai:core] begin stats
#define DA_AI_STATS_CORE(X) \
    X(npcs, Gauge) X(squads, Gauge) X(profile_default, Count) X(rays, Rate) X(rays_deferred, Rate) \
    X(p_total_us, Rate) X(p_max_us, Max) X(p_max_pre_us, Max) X(p_max_know_us, Max) X(p_max_comms_us, Max) \
    X(p_max_morale_us, Max) X(p_max_tactics_us, Max) X(p_max_search_us, Max) X(p_max_squad_us, Max) \
    X(p_max_comms_tx_us, Max) X(p_max_comms_rx_us, Max) X(p_max_flank_us, Max) X(p_max_comms_snd_us, Max) \
    X(p_max_comms_desc_us, Max) X(p_max_comms_msg_us, Max) X(p_max_sq_comms_us, Max) X(p_max_sq_tactics_us, Max) \
    X(p_max_sq_morale_us, Max) X(p_max_sq_search_us, Max)
// [ai:core] end stats

// [ai:know] begin stats
#define DA_AI_STATS_KNOW(X) \
    X(k_seen, Count) X(k_heard, Count) X(k_hit, Count) X(k_report, Count) X(k_rumour, Count) X(k_restored, Count) \
    X(k_forgot, Count) X(k_aim_fallback, Count) X(k_blind_shot, Count) X(k_candidates, Count) X(k_callback, Count) \
    X(p_know_us, Rate) X(p_know_cand_us, Rate)
// [ai:know] end stats

// [ai:sense] begin stats
#define DA_AI_STATS_SENSE(X) \
    X(s_identified, Count) X(s_ident_ms, Avg) X(s_suspicious, Count) X(s_noise_step, Count) \
    X(s_noise_reload, Count) X(s_noise_shot, Count) X(s_noise_shot_sil, Count) X(s_flash, Count) X(s_torch, Count) \
    X(s_near_miss, Count) X(s_occluded, Count) X(s_occlusion_rays, Count) X(s_heard_err, Avg) X(s_retrace, Count) \
    X(s_flash_q, Count) X(s_flash_x_period, Count) X(s_flash_x_range, Count) X(s_flash_x_view, Count) \
    X(s_flash_x_seen, Count) X(s_flash_x_los, Count) X(s_flash_x_wait, Count) X(p_sense_us, Rate)
// [ai:sense] end stats

// [ai:comms] begin stats
#define DA_AI_STATS_COMMS(X) \
    X(c_sent_voice, Count) X(c_sent_hushed, Count) X(c_sent_radio, Count) X(c_sent_gesture, Count) \
    X(c_sent_pda, Count) X(c_sent_hq, Count) X(c_recv_voice, Count) X(c_recv_hushed, Count) \
    X(c_recv_radio, Count) X(c_recv_gesture, Count) X(c_recv_pda, Count) X(c_recv_hq, Count) \
    X(c_lat_voice_ms, Avg) X(c_lat_radio_ms, Avg) X(c_lat_min_ms, Min) X(c_rep_err, Avg) X(c_net_lost, Count) \
    X(c_witness, Count) X(c_relay, Count) X(c_radio_npcs, Count) X(c_radio_violation, Count) X(c_deferred, Count) \
    X(p_comms_us, Rate)
// [ai:comms] end stats

// [ai:tactics] begin stats
#define DA_AI_STATS_TACTICS(X) \
    X(t_shots, Count) X(t_shots_supp, Count) X(t_shots_oob, Count) X(t_react_ms, Avg) X(t_react_due_ms, Avg) \
    X(t_react_base_ms, Avg) X(t_cover_ms, Count) X(t_combat_ms, Count) X(t_open_m, Sum) X(t_flank, Count) \
    X(t_flank_role, Count) X(t_flank_noroute, Count) X(t_alt_advance, Count) X(t_alt_grenade, Count) \
    X(t_alt_hold, Count) X(t_suppress, Count) X(t_fallback, Count) X(t_ambush, Count) X(t_advance, Count) \
    X(t_grenade, Count) X(t_return_fire, Count) X(p_tactics_us, Rate)
// [ai:tactics] end stats

// [ai:morale] begin stats
#define DA_AI_STATS_MORALE(X) \
    X(m_worried, Count) X(m_panicked, Count) X(m_retreat_orderly, Count) X(m_retreat_scatter, Count) \
    X(m_relocate, Count) X(m_surrender, Count) X(m_leader_known, Count) X(m_casualty_known, Count) \
    X(m_recovered, Count) X(m_help_shout, Count) X(p_morale_us, Rate)
// [ai:morale] end stats

// [ai:search] begin stats
#define DA_AI_STATS_SEARCH(X) \
    X(r_search, Count) X(r_spots, Count) X(r_cleared, Count) X(r_found, Count) X(r_giveup, Count) \
    X(r_giveup_spread_ms, Avg) X(r_quiet, Count) X(r_investigate, Count) X(r_torch_on, Count) X(r_handover, Count) \
    X(r_drop, Count) X(r_plan_deferred, Count) X(p_search_us, Rate)
// [ai:search] end stats

#define DA_AI_STATS(X) \
    DA_AI_STATS_CORE(X) DA_AI_STATS_KNOW(X) DA_AI_STATS_SENSE(X) DA_AI_STATS_COMMS(X) DA_AI_STATS_TACTICS(X) \
    DA_AI_STATS_MORALE(X) DA_AI_STATS_SEARCH(X)
// clang-format on

#define DA_AI_STAT_ENUM(name, kind) name,
enum class Stat : u16
{
    DA_AI_STATS(DA_AI_STAT_ENUM) count
};
#undef DA_AI_STAT_ENUM

// Counters are plain increments; nothing is printed unless ai_stats or ai_log_period asks for it. community is the
// NPC's community index (-1 = global line only).
void stat_add(Stat stat, float value = 1.f, int community = -1);
float stat_value(Stat stat);
bool stat_by_name(pcstr name, Stat& result);
void stats_print_and_reset();

// Adds the microseconds of its scope to a Rate counter (the p_*_us timers).
class ScopedTimer
{
    using clock = std::chrono::steady_clock;

    clock::time_point m_start;
    Stat m_stat;
    int m_community;

public:
    explicit ScopedTimer(Stat stat, int community = -1) : m_start(clock::now()), m_stat(stat), m_community(community) {}
    ~ScopedTimer() { stat_add(m_stat, elapsed_us(), m_community); }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    float elapsed_us() const
    {
        return std::chrono::duration<float, std::micro>(clock::now() - m_start).count();
    }
};

// Per-frame cap on the new rays of the AI ([ai_config] rays_per_frame). take() grants the rays or counts them as
// deferred; the caller retries on its next update, and since the scheduler rotates through the NPCs, the budget is
// shared round-robin. Rays that exist in the stock engine and only change flags are never charged here.
namespace RayBudget
{
bool take(u32 count = 1);
u32 left();
// Rays granted since the start of the session (wraps): what a caller's own step has cast is the difference.
u32 granted();
} // namespace RayBudget

// Resumable capture of one .scov record chunk (AIK1, AIT1): the object ids are snapshot at the start of the save and
// encoded 16 at a time within the save budget.
struct ChunkCapture
{
    xr_vector<u16> ids;
    xr_vector<u8> payload;
    size_t index{};
    bool begun{};
    bool present{};
    bool failed{};
};

// [ai_config] of dead_air_x64_ai.ltx, read at the first spawn of a level.
struct Config
{
    u32 rays_per_frame{48};
    u32 squad_tick_ms{250};
    u32 squad_refresh_ms{1000};
    float lod_near_m{60.f};
    bool loaded{};
};

extern Config g_config;
void load_config();
} // namespace DaAi

// Hooks of the stalker update and lifetime (every call site is on the human stalker path).
void da_ai_spawn(CAI_Stalker& npc);
void da_ai_destroy(CAI_Stalker& npc);
void da_ai_reload(CAI_Stalker& npc, pcstr section);
void da_ai_pre_memory(CAI_Stalker& npc, float dt);
void da_ai_post_memory(CAI_Stalker& npc, float dt);
// Every frame on the main thread (UpdateCL), before the object handler is queued for a worker thread.
void da_ai_update_cl(CAI_Stalker& npc);
// Asked by a firing weapon each update (main thread): false stops the queue. True for everyone but AI stalkers.
bool da_ai_may_fire(IGameObject& owner);

void da_ai_on_hit(CAI_Stalker& npc, const SHit& hit);
void da_ai_on_death(CAI_Stalker& npc, IGameObject* who);
void da_ai_level_unload();
void da_ai_register_console();

// Stimulus hooks placed in weapons, steps, torches and the bullet manager; bodies in da_ai_senses.cpp.
void da_ai_sense_on_shot(CWeapon& weapon);
void da_ai_sense_on_reload(CWeapon& weapon);
void da_ai_sense_on_step(IGameObject& walker, float power, SGameMtlPair* mtl_pair);
void da_ai_sense_on_torch(CTorch& torch, bool light_on);
// Thread-safe: the bullet manager may run off the main thread; the queue is drained on the main thread.
void da_ai_sense_queue_near_miss(u16 shooter, u16 victim, const Fvector& point, const Fvector& direction);
