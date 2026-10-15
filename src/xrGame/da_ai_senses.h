// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "da_ai.h"
#include "da_ai_knowledge.h"
#include "xrCDB/xr_collide_defs.h"
#include "xrCommon/xr_hash_map.h"

// Sense lane: the awareness meter, full-test line of sight, hearing with occlusion and masking, the noise bus,
// muzzle flash, torch beams and near misses. The engine stimulus hooks (da_ai_sense_*) are declared in da_ai.h.

class CAI_Stalker;
class CGameObject;
class CWeapon;
class IGameObject;
struct SHit;

namespace MemorySpace
{
struct CHitObject;
} // namespace MemorySpace

namespace DaAi
{
// Kinds of the noise bus and of Sense::hear (Lua level.ai_noise names: step, reload, item, impact, voice).
enum class Noise : u8
{
    step,
    landing,
    reload,
    grenade_draw,
    item,
    torch_click,
    impact,
    voice,
    shot,
    explosion,
    count
};

// [ai:sense] begin state
struct SenseState
{
    // Awareness of one object that entered the NPC's view.
    struct Target
    {
        float awareness{};
        u32 t_update{};
        u32 t_first{}; // first stimulus of the current rise from zero
        u32 t_identified{}; // last identification
        u32 t_suspect{}; // last suspicion handed to knowledge
        u32 t_near_miss{}; // last near-miss estimate of this shooter
        u32 cue_until{}; // a muzzle flash drew the eye: extra meter rate until then
        float cue_rate{};
        // The script hook's intensity, reused while distance and vision state hold ([ai_awareness] intensity_cache).
        float intensity{};
        float intensity_d{};
        u32 intensity_time{};
        bool intensity_danger{};
        bool identified{};
        bool suspicious{};
        bool counted{}; // the identification of this rise is in s_identified
    };

    // One occlusion ray toward a sound source, reused for [ai_noise] occlusion_cache_ms.
    struct Occlusion
    {
        Fvector position;
        u32 time{};
        u16 source{};
        float factor{1.f};
    };

    // The last heard estimate of a source, reused for a repeat of the same kind from about the same spot within
    // [ai_noise] heard_repeat: a burst is one observation, not one per round.
    struct Repeat
    {
        Estimate estimate{};
        Fvector position{};
        u32 time{};
        u16 source{u16(-1)};
        Noise kind{Noise::count};
    };

    // The estimate of the hit being processed, handed to the hit memory entry of the same hit.
    struct HitNote
    {
        Estimate estimate{};
        u32 frame{u32(-1)};
        u32 time{};
        u16 who{u16(-1)};
    };

    // A stimulus waiting for the NPC's own update (step 4 of the stalker update): a noise of the bus, a muzzle flash
    // or a near miss.
    enum class Cue : u8
    {
        noise,
        flash,
        near_miss
    };
    struct Stimulus
    {
        Fvector position; // noise source, muzzle, or where the bullet crossed the NPC's sphere
        Fvector direction; // bullet direction of a near miss
        Fvector head; // flash: the shooter's head it lights
        float power{}; // noise: power at the hearer before its ears and the walls
        u32 time{};
        u16 who{u16(-1)};
        Noise kind{Noise::count};
        Cue cue{Cue::noise};
        bool muzzle_hidden{}; // flash: the line to the muzzle is blocked, the head is still to try
    };

    xr_flat_hash_map<u16, Target> targets;
    xr_vector<Occlusion> occlusion;
    xr_vector<Repeat> repeats;
    xr_vector<Stimulus> inbox;
    HitNote hit;
    float suppression{};
    u32 suppression_time{};
    u32 torch_time{}; // next look for lamps
    u32 lamp_cursor{}; // where the next look for lamps goes on (the beams' spots)
    u32 facing_skip{}; // lamps facing the NPC a capped look left for the next one
    u32 cleanup_time{};
};
// [ai:sense] end state

namespace Sense
{
// Tuning of the lane: [ai_awareness], [ai_noise], [ai_light] and the sense sources of [ai_errors]. Read at the first
// use on a level; the compiled-in values equal the shipped ltx.
struct Linear
{
    float e0{};
    float k{};

    float at(float distance) const { return e0 + k * distance; }
};

struct Config
{
    // [ai_awareness]
    float identify{1.f};
    float suspicious{.35f};
    float crouch_mult{.6f};
    float rate_danger{4.f};
    float rate_free{32.f};
    float band_rate[4]{1.f, 1.2f, 1.55f, 1.9f};
    float peripheral_mult{.35f};
    float close_m{10.f};
    float close_gain{4.f};
    float decay_danger{.15f};
    float decay_free{.3f};
    float reacquire_floor{.7f};
    float reacquire_s{8.f};
    float suspect_err_far{15.f};
    float suspect_err_near{2.f};
    u32 suspect_period_ms{1000};
    float opaque_below{.05f};
    float fog_sight{.7f};
    float daylight_lum{.6f};
    float intensity_cache_ms{400.f};
    float intensity_cache_m{4.f};
    float touch_m{2.5f};
    u32 forget_ms{10000};

    // [ai_noise]
    float occlusion_floor{.25f};
    u32 occlusion_hits{3};
    u32 occlusion_cache_ms{2000};
    u32 occlusion_cache_far_ms{4000};
    float occlusion_move_m{2.f};
    float occlusion_move_k{.08f}; // plus this share of the hearer's distance to the source
    u32 heard_repeat_ms{250};
    float heard_repeat_m{2.f};
    u32 heard_repeat_far_ms{1000};
    float heard_repeat_far_m{6.f};
    u32 pre_rays{2};
    float pre_budget_us{150.f};
    float rain_mask{.08f};
    float detached_m{6.f};
    float gait[4]{.12f, .35f, .7f, 1.f}; // crouch, walk, run, sprint
    float shuffle{.12f}; // a step in place (turning)
    float landing{.9f};
    float step_range{36.f}; // audible radius in silence per unit of loudness: steps and landings
    float noise_range{40.f}; // the same for every other kind of the bus
    float cull_mult{1.25f}; // hearers beyond the audible radius x this are not tested
    float edge_power{.08f}; // received power of a noise at its audible radius; inverse square inside it
    float load_free_kg{25.f};
    float load_step_mult{.015f};
    float reload{.5f};
    float torch_click{.2f};
    u32 inbox_max{16};
    u32 stimulus_ttl_ms{1500};
    u32 stimuli_per_update{8};
    float near_miss_suppression{.25f};
    float near_miss_radius{1.5f};
    float near_miss_min{.3f};
    float suppression_decay{.3f};
    u32 near_miss_estimate_ms{500};
    u32 near_miss_hit_ms{300};
    u32 near_miss_queue_max{256};

    // [ai_light]
    u32 flash_period_ms{500};
    float ambient_day{.6f};
    float ambient_night{.15f};
    float night_hours[4]{19.5f, 21.f, 4.5f, 6.f}; // dusk begins, full dark, dawn begins, full light
    float muzzle_range_day{80.f};
    float muzzle_range_night{300.f};
    float muzzle_silenced{0.f};
    float flash_rate{3.f};
    u32 flash_cue_ms{600};
    float flash_day_mult{.3f};
    float torch_range_day{15.f};
    float torch_range_night{150.f};
    float torch_strength_min{.3f};
    float torch_strength_max{1.5f};
    float torch_rate{1.f};
    float torch_notice{.15f};
    u32 torch_period_ms{500};
    u32 torch_scan_rays{4};
    u32 torch_scan_checks{8};
    float spot_darkness{.6f};
    float spot_range{60.f};
    float spot_err_k{.5f};

    // [ai_errors], the sources sense produces
    Linear heard_shot{3.f, .08f};
    Linear heard_step{2.f, .12f};
    Linear heard_other{2.f, .1f};
    float heard_silenced_mult{2.f};
    float heard_occlusion_mult{1.f};
    float heard_skill_base{1.5f};
    Linear hit_cone_deg{5.f, 20.f};
    float hit_distance_err{.4f};
    float hit_distance_prior_min{30.f};
    float hit_distance_prior_max{60.f};
    float near_miss_cone_deg{25.f};
    float near_miss_distance_prior_min{20.f};
    float near_miss_distance_prior_max{80.f};
    Linear muzzle_flash{1.f, .02f};
    Linear flashlight{2.f, .05f};

    bool loaded{};
};

const Config& config();

// Times sense work into p_sense_us; work outside the core hooks (memory managers, vision) also into p_total_us.
class Timer
{
    ScopedTimer m_timer;
    int m_community;
    bool m_outside_hooks;

public:
    Timer(int community, bool outside_hooks);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
};

SenseState* create(CAI_Stalker& npc);
void destroy(SenseState* state);
// Step 4 of the stalker update: drains the NPC's stimulus inbox into estimates.
void pre_memory(CAI_Stalker& npc, float dt);
void on_death(CAI_Stalker& npc, IGameObject* who);
void level_unload();

// CDB flags for the rays that already exist and only change flags (vision traces, can_kill_entity, the look-out
// ray): unculled while full_los is on, so an NPC sees and aims through exactly what bullets pass. Never budgeted.
inline u32 ray_flags() { return feature(Feature::full_los) ? u32(CDB::OPT_FULL_TEST) : u32(CDB::OPT_CULL); }
// CObjectSpace::RayPick with ray_flags(), for an existing ray.
bool ray_pick(const Fvector& start, const Fvector& dir, float range, collide::rq_target target,
    collide::rq_result& result, IGameObject* ignore);
// Full-test ray under the ray budget; false when blocked or when the budget defers it. vis receives the vision
// transparency product along the ray (0 = blocked or deferred).
bool los(const Fvector& from, const Fvector& to, const IGameObject* ignore, float* vis = nullptr);
// The vision transparency product of the same ray outside the budget (Lua level.ai_line_vis): a line is clear above
// [ai_awareness] opaque_below.
float line_vis(const Fvector& from, const Fvector& to);

// The direction the NPC's head looks; whether a point lies inside its field of view.
Fvector view_direction(const CAI_Stalker& npc);
bool in_view(const CAI_Stalker& npc, const Fvector& point);
// 0 in daylight .. 1 at night, from the brightness of the current weather ([ai_light] ambient_day/night).
float darkness();

// Vision. The meter runs inside CVisualMemoryManager::visible for stalkers with state.
bool metered(const CAI_Stalker& npc);
struct Glimpse
{
    float intensity{}; // hook value of one time quantum: dt-independent
    float object_distance{};
    bool in_range{}; // inside the view distance at this angle
    bool instant{}; // the hook value alone passes the threshold (always-visible semantics)
    bool danger{}; // danger vision parameters
};
// One meter step of an object the NPC's rays reach; returns the stock not-yet-visible value in the current units.
float vision_seen(CAI_Stalker& npc, const CGameObject& object, const Glimpse& glimpse, float threshold, float dt);
// Decay of an object the rays did not reach this update.
float vision_unseen(CAI_Stalker& npc, const CGameObject& object, bool danger, float threshold, float dt);
// Share of an AI stalker's full view distance the weather's fog leaves (the renderer's linear fog: clear to fog_near,
// opaque at fog_far; sight ends at fog_sight of the way), for CCustomMonster::update_range_fov.
float fog_sight_factor(float full_range);
// Brightness of a living target for an AI stalker's eye: the renderer lights only what the player's camera drew
// (a body nobody rendered reads 0, one not drawn since keeps its old light), so a stale value follows the
// environment's light down and the weather's daylight is the floor ([ai_awareness] daylight_lum).
float target_luminocity(const CGameObject& target, float rendered);
// The script hook's intensity of a glimpse (one time quantum) is reused per target while it is fresh, the distance
// barely changed and the vision state is the same: the hook is a Lua call per object and NPC update.
bool cached_intensity(CAI_Stalker& npc, const CGameObject& object, bool danger, float distance, float& result);
// The meter of this object is full: in range it stays so whatever a glimpse's intensity is.
bool identified(const CAI_Stalker& npc, const CGameObject& object);
void store_intensity(CAI_Stalker& npc, const CGameObject& object, bool danger, float distance, float value);
// Transparency threshold of the vision traces: concealment keeps only "fully blocked".
float vision_threshold(const CAI_Stalker& npc, float stock);
// Combat re-trace of the selected enemy on the tick without traces (C18); called from Exec_Visibility with the
// time since the last regular trace.
void retrace(CAI_Stalker& npc, float dt);
float awareness(const CAI_Stalker& npc, const CGameObject* object);
// 0..1: bullets passing close to the NPC recently (near misses), decaying.
float suppression(const CAI_Stalker& npc);

// Hearing. The visitor of the sound memory (voice decoding) runs after the hearing threshold for stalkers.
inline bool late_visitor(const CAI_Stalker* stalker) { return stalker && feature(Feature::hearing); }

struct Heard
{
    Estimate estimate{}; // filled by on_heard
    float occlusion{}; // 0 clear .. 1 blocked (CDB ray)
    Noise kind{Noise::count};
    bool silenced{};
    bool store{}; // the sound memory entry takes the estimate
};

// An engine sound event reaching a stalker (feel_sound_new, after the perceive factors). Adjusts power to what the
// NPC receives (0 for kinds only the noise bus carries); false = the stock path handles it.
bool engine_sound(CAI_Stalker& hearer, const IGameObject* who, u32 sound_type, const Fvector& position, float& power,
    float threshold, Heard& heard);
// The sound passed the threshold: the estimate, knowledge and counters. True = heard.estimate is valid.
bool on_heard(CAI_Stalker& hearer, const IGameObject* who, u32 sound_type, const Fvector& position, float power,
    Heard& heard);
// Power after hearing gain, rain masking and one CDB occlusion ray (budgeted, cached per source); threshold only lets
// a sound that cannot pass skip the ray. occlusion receives 1 - the ray factor.
float receive(CAI_Stalker& hearer, const Fvector& position, float power, u16 source, float threshold,
    float* occlusion = nullptr);
// The one hearing function of the noise bus: the power the hearer receives (0 = not heard).
float hear(CAI_Stalker& hearer, const Fvector& position, float power, Noise kind, const CGameObject* who);
// The noise bus: queues the noise into the inbox of every stalker near enough to hear it in silence (audible radius
// = loudness x [ai_noise] step_range or noise_range); each one hears it through hear() in its own next update.
void emit_noise(const Fvector& position, float loudness, Noise kind, const CGameObject* who);
bool noise_by_name(pcstr name, Noise& result);

// Stimuli of the inbox, drained in pre_memory (da_ai_hearing.cpp, da_ai_light.cpp, da_ai_senses.cpp). A stimulus
// that has to wait for the ray budget, or for a new ray once the update's [ai_noise] pre_rays are spent (may_trace
// false, rays 0), returns false and stays for the next update; a flash keeps what its first ray found.
void queue(CAI_Stalker& npc, const SenseState::Stimulus& stimulus);
bool process_noise(CAI_Stalker& npc, SenseState& state, const SenseState::Stimulus& stimulus, bool may_trace);
bool process_flash(CAI_Stalker& npc, SenseState& state, SenseState::Stimulus& stimulus, u32 rays);
bool process_near_miss(CAI_Stalker& npc, SenseState& state, const SenseState::Stimulus& stimulus);

// Light (da_ai_light.cpp): a muzzle flash for the NPCs that face the shooter; the lamps and lit spots an NPC sees;
// the glow of a carried lamp toward an observer (0 = none .. torch_strength max), which speeds the awareness meter.
void muzzle_flash(CWeapon& weapon);
// One look for lamps with at most rays new rays (and [ai_light] torch_scan_rays).
void scan_lamps(CAI_Stalker& npc, SenseState& state, u32 rays);
float lamp_glow(const CAI_Stalker& observer, const CGameObject& carrier);
void light_level_unload();
void hearing_level_unload();

// Hits: a direction cone and a distance guess unless the shooter is in own vision.
void on_hit(CAI_Stalker& npc, const SHit& hit);
// A direction guess of where shots came from: the reversed bullet direction within +-cone (pitch half of it), at the
// distance of the NPC's own heard-shot belief of the shooter, else a random prior, +-distance_err. Source none when
// the bullet direction is unknown.
Estimate direction_guess(const CAI_Stalker& npc, const CGameObject& shooter, const Fvector& bullet_dir,
    float cone_deg, float prior_min, float prior_max, float distance_err, Source source);
// Called by the hit memory manager right after it filled the entry of that hit.
void apply_hit_estimate(const CAI_Stalker& npc, MemorySpace::CHitObject& entry);

// An estimate of truth with the error the NPC assumes: knowledge's realisation (disk sample, level vertex, body
// centre height), with the lane's own confidence.
Estimate make_estimate(const CAI_Stalker& npc, const CGameObject* target, const Fvector& truth, float err,
    Source source, float confidence);
} // namespace Sense
} // namespace DaAi
