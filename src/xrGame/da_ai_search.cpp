// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_search.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"
#include "da_ai_senses.h"
#include "da_ai_comms.h"
#include "da_ai_tactics.h"

#include "agent_enemy_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_space.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "ai_space.h"
#include "character_community.h"
#include "cover_manager.h"
#include "cover_point.h"
#include "danger_manager.h"
#include "danger_object.h"
#include "enemy_manager.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "Level.h"
#include "memory_manager.h"
#include "movement_manager.h"
#include "sound_player.h"
#include "stalker_movement_manager_smart_cover.h"
#include "stalker_planner.h"
#include "visual_memory_manager.h"
// CTorch inherits engine interfaces by dominance; the C4250 notes belong to its header, not to this file.
#pragma warning(push)
#pragma warning(disable : 4250)
#include "Torch.h"
#pragma warning(pop)
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrAICore/Navigation/level_graph.h"

namespace DaAi::Search
{
namespace
{
// Plan builds under way at once, at most (the slots of [ai_search] plan_builders).
constexpr u32 plan_builders_max = 4;

// [ai_search] of dead_air_x64_ai.ltx, read at the first use on a level.
struct Config
{
    float spot_radius{30.f};
    float spot_radius_max{60.f};
    float widen_mult{1.6f};
    u32 widen_max{2};
    u32 spots_max{10};
    float spot_min_gap{6.f};
    float spot_max_dy{3.f};
    float hidden_max{.5f};
    u32 ring_samples{12};
    float escape_weight{1.f};
    float distance_weight{.5f};
    float travel_weight{.5f};
    float escape_speed_min{.5f};
    float clear_reach{2.5f};
    float clear_view{25.f};
    float clear_height{.9f};
    u32 clear_rays{2};
    float replan_dist{10.f};
    u32 assign_timeout_ms{25000};
    u32 assign_tries{2};
    u32 assign_wait_ms{5000};
    u32 task_max_ms{40000};
    float pair_max_gap{30.f};
    float overwatch_dist{8.f};
    u32 overwatch_max_ms{30000};
    u32 takeover_ms{4000};
    u32 unselected_drop_ms{20000};
    float sworn_mult{1.3f};
    u32 bark_pause_lo_ms{8000};
    u32 bark_pause_hi_ms{15000};
    float sneak_dist{8.f};
    float investigate_same_m{10.f};
    float investigate_reach_m{100.f};
    u32 investigate_told_ms{30000};
    u32 investigate_max_ms{45000};
    u32 torch_check_ms{1000};
    u32 plan_builds_per_frame{1};
    float plan_tile_m{15.f};
    u32 plan_pops_per_step{256};
    float plan_step_budget_us{120.f};
    u32 plan_builders{4};
    u32 plan_build_stale_ms{2000};
    xr_vector<std::pair<shared_str, shared_str>> sworn; // symmetric community pairs
    bool loaded{};
};

// search_assign / all_clear about an investigation rather than a spot of a search plan: plan generations skip 0 in
// their 7 message bits, and no plan has more than 64 spots. Index 0xff: "I go" (addressee = the mate asked to cover,
// with the overwatch bit) or "all clear"; 0xfe: a leader's order to the addressee to go and look.
constexpr u8 investigate_generation = 0;
constexpr u8 investigate_index = 0xff;
constexpr u8 investigate_go_index = 0xfe;

Config g_config;
xr_vector<CCoverPoint*> g_covers;
xr_vector<u16> g_targets;
xr_vector<NpcState*> g_searchers;
xr_vector<NpcState*> g_unpaired;
// Plan build steps of the current frame, capped by [ai_search] plan_builds_per_frame.
u32 g_plan_frame{u32(-1)};
u32 g_plan_builds{};

struct Candidate
{
    Fvector pos;
    u32 vertex;
    float order;
};

u32 to_ms(float seconds) { return u32(_max(0.f, seconds) * 1000.f); }

// [ai_community_alias] maps a mod's community to the one whose data it uses.
shared_str alias(pcstr community)
{
    if (!community || !community[0])
        return {};
    return READ_IF_EXISTS(pSettings, r_string, "ai_community_alias", community, community);
}

void load_config()
{
    constexpr pcstr section = "ai_search";
    Config cfg;
    const auto read = [section](pcstr key, float value) {
        return READ_IF_EXISTS(pSettings, r_float, section, key, value);
    };
    const auto read_u32 = [section](pcstr key, u32 value) {
        return READ_IF_EXISTS(pSettings, r_u32, section, key, value);
    };
    const auto read_ms = [section](pcstr key, u32 value) {
        return to_ms(READ_IF_EXISTS(pSettings, r_float, section, key, float(value) / 1000.f));
    };

    cfg.spot_radius = _max(1.f, read("spot_radius", cfg.spot_radius));
    cfg.spot_radius_max = _max(cfg.spot_radius, read("spot_radius_max", cfg.spot_radius_max));
    cfg.widen_mult = _max(1.f, read("widen_mult", cfg.widen_mult));
    cfg.widen_max = read_u32("widen_max", cfg.widen_max);
    // a spot index travels in 8 bits of a message
    cfg.spots_max = std::clamp(read_u32("spots_max", cfg.spots_max), 1u, 64u);
    cfg.spot_min_gap = _max(0.f, read("spot_min_gap", cfg.spot_min_gap));
    cfg.spot_max_dy = read("spot_max_dy", cfg.spot_max_dy);
    cfg.hidden_max = read("hidden_max", cfg.hidden_max);
    cfg.ring_samples = _min(read_u32("ring_samples", cfg.ring_samples), 64u);
    cfg.escape_weight = read("escape_weight", cfg.escape_weight);
    cfg.distance_weight = read("distance_weight", cfg.distance_weight);
    cfg.travel_weight = read("travel_weight", cfg.travel_weight);
    cfg.escape_speed_min = read("escape_speed_min", cfg.escape_speed_min);
    cfg.clear_reach = read("clear_reach", cfg.clear_reach);
    cfg.clear_view = read("clear_view", cfg.clear_view);
    cfg.clear_height = read("clear_height", cfg.clear_height);
    cfg.clear_rays = read_u32("clear_rays", cfg.clear_rays);
    cfg.replan_dist = read("replan_dist", cfg.replan_dist);
    cfg.assign_timeout_ms = read_ms("assign_timeout_s", cfg.assign_timeout_ms);
    cfg.assign_tries = _max(1u, read_u32("assign_tries", cfg.assign_tries));
    cfg.assign_wait_ms = read_ms("assign_wait_s", cfg.assign_wait_ms);
    cfg.task_max_ms = read_ms("task_max_s", cfg.task_max_ms);
    cfg.pair_max_gap = read("pair_max_gap", cfg.pair_max_gap);
    cfg.overwatch_dist = read("overwatch_dist", cfg.overwatch_dist);
    cfg.overwatch_max_ms = read_ms("overwatch_max_s", cfg.overwatch_max_ms);
    cfg.takeover_ms = read_ms("takeover_s", cfg.takeover_ms);
    cfg.unselected_drop_ms = read_ms("unselected_drop_s", cfg.unselected_drop_ms);
    cfg.sworn_mult = read("sworn_mult", cfg.sworn_mult);
    if (pcstr pause = READ_IF_EXISTS(pSettings, r_string, section, "bark_pause_s", nullptr); pause && pause[0])
    {
        string64 item;
        const float lo = float(atof(_GetItem(pause, 0, item)));
        const float hi = _GetItemCount(pause) > 1 ? float(atof(_GetItem(pause, 1, item))) : lo;
        cfg.bark_pause_lo_ms = to_ms(_min(lo, hi));
        cfg.bark_pause_hi_ms = to_ms(_max(lo, hi));
    }
    cfg.sneak_dist = read("sneak_dist", cfg.sneak_dist);
    cfg.investigate_same_m = read("investigate_same_m", cfg.investigate_same_m);
    cfg.investigate_reach_m = read("investigate_reach_m", cfg.investigate_reach_m);
    cfg.investigate_told_ms = read_ms("investigate_told_s", cfg.investigate_told_ms);
    cfg.investigate_max_ms = read_ms("investigate_max_s", cfg.investigate_max_ms);
    cfg.torch_check_ms = read_ms("torch_check_s", cfg.torch_check_ms);
    cfg.plan_builds_per_frame = _max(1u, read_u32("plan_builds_per_frame", cfg.plan_builds_per_frame));
    cfg.plan_tile_m = _max(1.f, read("plan_tile_m", cfg.plan_tile_m));
    cfg.plan_pops_per_step = _max(1u, read_u32("plan_pops_per_step", cfg.plan_pops_per_step));
    cfg.plan_step_budget_us = _max(1.f, read("plan_step_budget_us", cfg.plan_step_budget_us));
    cfg.plan_builders = std::clamp(read_u32("plan_builders", cfg.plan_builders), 1u, plan_builders_max);
    cfg.plan_build_stale_ms = read_ms("plan_build_stale_s", cfg.plan_build_stale_ms);

    constexpr pcstr sworn = "ai_sworn_enemies";
    if (pSettings->section_exist(sworn))
    {
        for (const auto& item : pSettings->r_section(sworn).Data)
        {
            const shared_str from = alias(item.first.c_str());
            pcstr list = item.second.c_str();
            if (!from.size() || !list)
                continue;
            string128 name;
            for (int index = 0, count = _GetItemCount(list); index < count; ++index)
            {
                if (const shared_str to = alias(_GetItem(list, index, name)); to.size())
                    cfg.sworn.emplace_back(from, to);
            }
        }
    }

    cfg.loaded = true;
    g_config = std::move(cfg);
}

const Config& config()
{
    if (!g_config.loaded)
        load_config();
    return g_config;
}

// Message payload of search_assign / all_clear: addressee (16 bits) | overwatch (1) | generation (7) | spot (8).
struct Packed
{
    u16 addressee;
    u8 generation;
    u8 index;
    bool overwatch;
};

u32 pack(u16 addressee, u8 generation, u8 index, bool overwatch)
{
    return u32(addressee) << 16 | u32(overwatch) << 15 | u32(generation & 0x7f) << 8 | index;
}

Packed unpack(u32 extra)
{
    return {u16(extra >> 16), u8((extra >> 8) & 0x7f), u8(extra & 0xff), ((extra >> 15) & 1) != 0};
}

bool same_generation(u8 generation, u8 packed) { return (generation & 0x7f) == packed; }

bool investigation_message(const Packed& packed)
{
    return packed.generation == investigate_generation &&
        (packed.index == investigate_index || packed.index == investigate_go_index);
}

const CLevelGraph& graph() { return ai().level_graph(); }

float distance_xz(const Fvector& a, const Fvector& b) { return _sqrt(_sqr(a.x - b.x) + _sqr(a.z - b.z)); }

u32 vertex_at(const Fvector& position)
{
    const CLevelGraph& level = graph();
    if (!level.valid_vertex_position(position))
        return u32(-1);
    const u32 vertex = level.vertex_id(position);
    return level.valid_vertex_id(vertex) ? vertex : u32(-1);
}

// The vertex under a position, else the nearest one found by knowledge's bounded window search from the spot back to
// the NPC (the stock nearest-vertex search from a hint could walk the whole graph for a spot off it).
u32 nearest_vertex(const Fvector& position, const Fvector& anchor)
{
    const u32 vertex = vertex_at(position);
    return vertex != u32(-1) ? vertex : Know::find_vertex(position, &anchor);
}

// A plan build step fits into this frame's share; the rest wait for a later update or squad step (P10: several
// searches starting in one frame each built their spot pool at once).
bool take_plan_build(int community)
{
    if (g_plan_frame != Device.dwFrame)
    {
        g_plan_frame = Device.dwFrame;
        g_plan_builds = 0;
    }
    if (g_plan_builds >= config().plan_builds_per_frame)
    {
        stat_add(Stat::r_plan_deferred, 1.f, community);
        return false;
    }
    ++g_plan_builds;
    return true;
}

// A place is hidden from the LKP when the level graph gives it cover in that direction (the covers' own measure).
bool hidden_from(const Fvector& lkp, const Fvector& position, u32 vertex)
{
    float yaw, pitch;
    Fvector().sub(lkp, position).getHP(yaw, pitch);
    yaw = angle_normalize(yaw);
    const float cover =
        _min(graph().high_cover_in_direction(yaw, vertex), graph().low_cover_in_direction(yaw, vertex));
    return cover <= config().hidden_max;
}

SearchState* state_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state && !state->dead ? state->search : nullptr;
}

int community_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state ? state->community : -1;
}

shared_str community_name(const CGameObject& object)
{
    const auto* owner = smart_cast<const CInventoryOwner*>(&object);
    if (!owner)
        return {};
    const shared_str name = CHARACTER_COMMUNITY::IndexToId(owner->Community(), nullptr, true);
    return alias(name.c_str());
}

bool sworn(const CAI_Stalker& npc, const CGameObject& target)
{
    const Config& cfg = config();
    if (cfg.sworn.empty())
        return false;
    const shared_str mine = community_name(npc);
    const shared_str theirs = community_name(target);
    if (!mine.size() || !theirs.size())
        return false;
    return std::any_of(cfg.sworn.begin(), cfg.sworn.end(), [&](const auto& pair) {
        return (pair.first == mine && pair.second == theirs) || (pair.first == theirs && pair.second == mine);
    });
}

bool can_sweep(const CAI_Stalker& npc)
{
    const SearchStyle style = profile(npc).search_style;
    return style == SearchStyle::pairs || style == SearchStyle::single;
}

SearchPool* find_pool(SearchSquad& squad, u16 target)
{
    const auto found = std::find_if(
        squad.pools.begin(), squad.pools.end(), [target](const SearchPool& pool) { return pool.target == target; });
    return found == squad.pools.end() ? nullptr : &*found;
}

SearchPool* pool_of(const CAI_Stalker& npc, u16 target)
{
    const SquadState* squad = squad_of(npc);
    return squad && squad->search ? find_pool(*squad->search, target) : nullptr;
}

void reset(SearchState& state)
{
    // The plan's buffer is kept for the next search, and so is the target the NPC last gave up on; an investigation
    // belongs to the danger state and outlives the search.
    xr_vector<SearchSpot> spots = std::move(state.own.spots);
    spots.clear();
    const u16 given_up = state.given_up;
    const u32 t_given_up = state.t_given_up;
    const u16 engaged = state.engaged;
    const Investigation investigation = state.investigation;
    const InvestigateNews news = state.news;
    state = {};
    state.own.spots = std::move(spots);
    state.given_up = given_up;
    state.t_given_up = t_given_up;
    state.engaged = engaged;
    state.investigation = investigation;
    state.news = news;
}

u32 quiet_flags(const CAI_Stalker& npc) { return profile(npc).quiet_search ? Comms::quiet : 0u; }

// A lit torch of the NPC, checked at most every torch_check_s.
bool torch_lit(const CAI_Stalker& npc, u32& t_check)
{
    const u32 now = Device.dwTimeGlobal;
    if (t_check && now < t_check + config().torch_check_ms)
        return false;
    t_check = now;
    const auto& items = npc.inventory().m_all;
    return std::any_of(items.begin(), items.end(), [](PIItem item) {
        const CTorch* torch = smart_cast<CTorch*>(item);
        return torch && torch->torch_active();
    });
}

u32 deadline(const SearchState& state) { return _max(state.t_start, state.t_lkp) + state.duration_ms; }

// Who builds a plan: an NPC's own plan (owner = the NPC) or a squad's pool (owner = the squad), for one target.
struct PlanKey
{
    u16 owner{u16(-1)};
    u16 target{u16(-1)};
    bool pool{};

    bool operator==(const PlanKey& other) const = default;
};

// Spots within [lo, hi] of the LKP that cannot be seen from it: cover points and level vertices on two rings, ordered
// by the estimated escape direction and the distance, at least spot_min_gap apart. A plan is built in steps (P10: a
// plan around a 48-60 m ring walked thousands of covers in one update, 0.3-0.6 ms): the ring around the LKP is gathered
// in square tiles whose circles are [ai_search] plan_tile_m (Tactics::next_cover_tile), then the candidates are taken
// in order of preference, at most plan_pops_per_step per step, and a step goes on to its next tile or candidate only
// while it has taken less than plan_step_budget_us. The same covers are found as by one query of the disk. Between
// steps a build waits in one of plan_builders slots; a plan that finds none free waits (r_plan_deferred), and a build
// its owner stops asking for is dropped after plan_build_stale_s.
struct PlanBuild
{
    PlanKey key;
    bool active{};
    bool heaped{};
    bool lkp_spot{};
    CoverTiles tiles;
    Fvector lkp{};
    Fvector escape{};
    u32 lkp_vertex{u32(-1)};
    u32 t_lkp{};
    float lo{};
    float hi{};
    size_t heap_size{};
    u32 t_step{};
    xr_vector<Candidate> candidates;
    xr_vector<SearchSpot> spots;
};

PlanBuild g_builds[plan_builders_max];

void plan_consider(PlanBuild& build, const Fvector& position, u32 vertex)
{
    const Config& cfg = config();
    if (!graph().valid_vertex_id(vertex) || _abs(position.y - build.lkp.y) > cfg.spot_max_dy)
        return;
    const float distance = distance_xz(position, build.lkp);
    if (distance < build.lo || distance > build.hi || distance < EPS_L)
        return;
    // Hidden from the LKP, tested once per candidate here rather than when it is taken: the same spots pass.
    if (!hidden_from(build.lkp, position, vertex))
        return;
    const Fvector direction = Fvector().set(position.x - build.lkp.x, 0.f, position.z - build.lkp.z).div(distance);
    const float away = 1.f - direction.dotproduct(build.escape);
    build.candidates.push_back(
        {position, vertex, cfg.escape_weight * away + cfg.distance_weight * distance / build.hi});
}

void plan_start(PlanBuild& build, const PlanKey& key, const Fvector& lkp, u32 lkp_vertex, const Fvector& escape,
    u32 t_lkp, float lo, float hi)
{
    build.key = key;
    build.active = true;
    build.heaped = false;
    build.lkp_spot = false;
    build.lkp = lkp;
    build.escape = escape;
    build.lkp_vertex = lkp_vertex;
    build.t_lkp = t_lkp;
    build.lo = lo;
    build.hi = hi;
    build.heap_size = 0;
    build.candidates.clear();
    build.spots.clear();
    Tactics::start_cover_tiles(build.tiles, lkp, hi, lo, config().plan_tile_m);
}

bool later(const Candidate& left, const Candidate& right) { return left.order > right.order; }

// The rings' level vertices join the covers; the LKP itself is the first spot of a first ring (whoever checks it sees
// everything that is not hidden from it).
void plan_heap(PlanBuild& build)
{
    const Config& cfg = config();
    const CLevelGraph& level = graph();
    if (cfg.ring_samples)
    {
        const float radii[]{build.lo + (build.hi - build.lo) * .5f, build.hi};
        for (size_t ring = 0; ring < std::size(radii); ++ring)
        {
            for (u32 index = 0; index < cfg.ring_samples; ++index)
            {
                // the outer ring is rotated by half a step so the two rings do not line up
                const float angle = PI_MUL_2 * (float(index) + .5f * float(ring)) / float(cfg.ring_samples);
                const Fvector sample = Fvector().set(build.lkp.x + radii[ring] * _cos(angle), build.lkp.y + 1.f,
                    build.lkp.z + radii[ring] * _sin(angle));
                if (const u32 vertex = vertex_at(sample); vertex != u32(-1))
                    plan_consider(build, level.vertex_position(vertex), vertex);
            }
        }
    }
    // A heap pops the few taken, where sorting every cover of a 60 m ring (thousands) was much of a plan's cost.
    std::make_heap(build.candidates.begin(), build.candidates.end(), later);
    build.heap_size = build.candidates.size();
    if (build.lo <= 0.f && level.valid_vertex_id(build.lkp_vertex))
    {
        SearchSpot& spot = build.spots.emplace_back();
        spot.pos = level.vertex_position(build.lkp_vertex);
        spot.vertex = build.lkp_vertex;
        build.lkp_spot = true;
    }
    build.heaped = true;
}

// Candidates in order of preference until the plan is full; false = the step's share is spent.
bool plan_take(PlanBuild& build, const CAI_Stalker& owner, StepBudget& budget)
{
    const Config& cfg = config();
    for (u32 pops = 0; build.heap_size && build.spots.size() < cfg.spots_max; ++pops)
    {
        if (pops >= cfg.plan_pops_per_step || !budget.next())
            return false;
        const auto end = build.candidates.begin() + ptrdiff_t(build.heap_size);
        std::pop_heap(build.candidates.begin(), end, later);
        const Candidate& candidate = build.candidates[--build.heap_size];
        // The spacing test first: the access test (restrictions) only for a spot the plan would take.
        const bool crowded = std::any_of(build.spots.begin(), build.spots.end(), [&](const SearchSpot& spot) {
            return distance_xz(spot.pos, candidate.pos) < cfg.spot_min_gap;
        });
        if (crowded || !owner.movement().accessible(candidate.vertex))
            continue;
        SearchSpot& spot = build.spots.emplace_back();
        spot.pos = candidate.pos;
        spot.vertex = candidate.vertex;
        spot.order = candidate.order;
    }
    return true;
}

// One step of a plan's build; true once the plan holds its new spots (a new generation). A build under way goes on
// unless it is for another ring or its LKP moved as far as makes a plan stale.
bool build_step(SearchPlan& plan, const PlanKey& key, const CAI_Stalker& owner, const Fvector& lkp, u32 lkp_vertex,
    const Fvector& escape, u32 t_lkp, float lo, float hi, int community)
{
    const Config& cfg = config();
    const u32 now = Device.dwTimeGlobal;
    const u32 slots = _min(cfg.plan_builders, plan_builders_max);
    PlanBuild* build = nullptr;
    PlanBuild* free_slot = nullptr;
    for (u32 index = 0; index < slots; ++index)
    {
        PlanBuild& slot = g_builds[index];
        if (slot.active && now >= slot.t_step + cfg.plan_build_stale_ms)
            slot.active = false;
        if (slot.active && slot.key == key)
            build = &slot;
        else if (!slot.active && !free_slot)
            free_slot = &slot;
    }
    if (!build && !free_slot)
    {
        stat_add(Stat::r_plan_deferred, 1.f, community);
        return false;
    }
    if (!take_plan_build(community))
        return false;
    if (build &&
        (!fsimilar(build->lo, lo) || !fsimilar(build->hi, hi) || distance_xz(build->lkp, lkp) > cfg.replan_dist))
        plan_start(*build, key, lkp, lkp_vertex, escape, t_lkp, lo, hi);
    else if (!build)
    {
        build = free_slot;
        plan_start(*build, key, lkp, lkp_vertex, escape, t_lkp, lo, hi);
    }
    build->t_step = now;

    StepBudget budget(cfg.plan_step_budget_us);
    while (build->tiles.active)
    {
        if (!budget.next())
            return false;
        if (Tactics::next_cover_tile(build->tiles, g_covers))
        {
            for (const CCoverPoint* point : g_covers)
                plan_consider(*build, point->position(), point->level_vertex_id());
        }
    }
    if (!build->heaped)
    {
        if (!budget.next())
            return false;
        plan_heap(*build);
    }
    if (!plan_take(*build, owner, budget))
        return false;

    plan.spots.assign(build->spots.begin(), build->spots.end());
    plan.lkp = build->lkp;
    plan.escape = build->escape;
    plan.radius = build->hi;
    plan.t_lkp = build->t_lkp;
    plan.lkp_spot = build->lkp_spot;
    do
        ++plan.generation;
    while ((plan.generation & 0x7f) == investigate_generation);
    build->active = false;
    build->candidates.clear();
    return true;
}

PlanKey own_key(const CAI_Stalker& npc, const SearchState& state) { return {npc.ID(), state.target, false}; }
PlanKey pool_key(const SquadState& squad, const SearchPool& pool) { return {squad.id, pool.target, true}; }

// The next ring around the same LKP once every spot is cleared; false when the search may not widen any more, or its
// build is not finished (the next update goes on with it).
bool widen(SearchPlan& plan, const CAI_Stalker& owner, const PlanKey& key)
{
    const Config& cfg = config();
    if (plan.widened >= cfg.widen_max || plan.radius >= cfg.spot_radius_max)
        return false;
    const u8 widened = plan.widened + 1;
    const float radius = plan.radius;
    if (!build_step(plan, key, owner, plan.lkp, u32(-1), plan.escape, plan.t_lkp, radius,
            _min(radius * cfg.widen_mult, cfg.spot_radius_max), community_of(owner)))
    {
        return false;
    }
    plan.widened = widened;
    return true;
}

bool exhausted(const SearchPlan& plan)
{
    return std::all_of(plan.spots.begin(), plan.spots.end(), [](const SearchSpot& spot) { return spot.cleared; });
}

// The cheapest uncleared spot for a searcher: the plan's own order plus the walk to it.
int best_spot(const SearchPlan& plan, const CAI_Stalker& searcher, bool free_only)
{
    const Config& cfg = config();
    int best = -1;
    float best_cost = flt_max;
    for (size_t index = 0; index < plan.spots.size(); ++index)
    {
        const SearchSpot& spot = plan.spots[index];
        if (spot.cleared || (free_only && spot.sweeper != u16(-1)))
            continue;
        if (!searcher.movement().accessible(spot.vertex))
            continue;
        const float cost =
            spot.order + cfg.travel_weight * distance_xz(searcher.Position(), spot.pos) / _max(plan.radius, 1.f);
        if (cost < best_cost)
        {
            best_cost = cost;
            best = int(index);
        }
    }
    return best;
}

SearchTask make_task(const SearchPlan& plan, u8 index, SearchRole role, u16 target, u16 coordinator)
{
    const SearchSpot& spot = plan.spots[index];
    SearchTask task;
    task.pos = spot.pos;
    task.vertex = spot.vertex;
    task.t_given = Device.dwTimeGlobal;
    task.target = target;
    task.coordinator = coordinator;
    task.generation = plan.generation;
    task.index = index;
    task.role = role;
    task.lkp = plan.lkp_spot && index == 0;
    return task;
}

// The covering partner watches the spot from overwatch_dist on its own side, or from where it stands if closer.
void place_post(const CAI_Stalker& npc, SearchTask& task)
{
    const Config& cfg = config();
    const Fvector& from = npc.Position();
    task.post = from;
    task.post_vertex = npc.ai_location().level_vertex_id();

    const float distance = distance_xz(from, task.pos);
    if (distance <= cfg.overwatch_dist)
        return;
    Fvector point = Fvector()
                        .set(from.x - task.pos.x, 0.f, from.z - task.pos.z)
                        .div(distance)
                        .mul(cfg.overwatch_dist)
                        .add(task.pos);
    point.y = _max(task.pos.y, from.y) + 1.f;
    const u32 vertex = vertex_at(point);
    if (vertex == u32(-1) || !npc.movement().accessible(vertex))
        return;
    task.post = graph().vertex_position(vertex);
    task.post_vertex = vertex;
}

void take(CAI_Stalker& npc, SearchState& state, const SearchTask& task)
{
    state.task = task;
    state.task.t_given = Device.dwTimeGlobal;
    state.t_idle = 0;
    if (task.role == SearchRole::overwatch)
        place_post(npc, state.task);
    else
        stat_add(Stat::r_spots, 1.f, community_of(npc));
}

void send_clear(CAI_Stalker& npc, const SearchTask& task)
{
    Comms::request(npc, Comms::Kind::all_clear, task.target,
        pack(task.coordinator, task.generation, task.index, false), quiet_flags(npc));
}

// The NPC's own eyes clear a spot: it stands on it, or looks into it within its field of view and a ray passes.
bool sees_spot(const CAI_Stalker& npc, const Fvector& spot, u32& rays)
{
    const Config& cfg = config();
    if (distance_xz(npc.Position(), spot) <= cfg.clear_reach && _abs(npc.Position().y - spot.y) <= cfg.spot_max_dy)
        return true;
    if (!rays)
        return false;

    const Fvector target = Fvector().set(spot.x, spot.y + cfg.clear_height, spot.z);
    const Fvector& eye = npc.eye_matrix.c;
    Fvector direction = Fvector().sub(target, eye);
    const float distance = direction.magnitude();
    if (distance > cfg.clear_view || distance < EPS_L)
        return false;
    direction.div(distance);
    if (direction.dotproduct(npc.eye_matrix.k) < _cos(deg2rad(npc.ffGetFov()) * .5f))
        return false;
    --rays;
    return Sense::los(eye, target, &npc);
}

SearchSpot* task_spot(CAI_Stalker& npc, SearchState& state)
{
    const SearchTask& task = state.task;
    SearchPlan* plan = nullptr;
    if (task.coordinator == u16(-1))
        plan = &state.own;
    else if (SearchPool* pool = pool_of(npc, task.target); pool && pool->coordinator == npc.ID())
        plan = &pool->plan;
    if (!plan || plan->generation != task.generation || task.index >= plan->spots.size())
        return nullptr;
    return &plan->spots[task.index];
}

void finish_sweep(CAI_Stalker& npc, SearchState& state)
{
    const SearchTask& task = state.task;
    stat_add(Stat::r_cleared, 1.f, community_of(npc));
    if (task.lkp)
        state.lkp_checked = true;

    if (task.coordinator != u16(-1) && task.coordinator != npc.ID())
        send_clear(npc, task);
    else if (SearchSpot* spot = task_spot(npc, state))
    {
        spot->cleared = true;
        // the coordinator's own spot: its covering partner learns by the same message as everyone else
        if (task.coordinator == npc.ID() && spot->overwatch != u16(-1) && spot->overwatch != npc.ID())
            send_clear(npc, task);
    }

    state.task = {};
    state.t_idle = Device.dwTimeGlobal;
}

// A spot not reached in time: an own or coordinated spot is dropped, an assigned one runs out on the coordinator's
// timer (the searcher says nothing it did not see).
void abandon(CAI_Stalker& npc, SearchState& state)
{
    const SearchTask& task = state.task;
    if (task.role == SearchRole::sweep && (task.coordinator == u16(-1) || task.coordinator == npc.ID()))
    {
        if (SearchSpot* spot = task_spot(npc, state))
            spot->cleared = true;
    }
    state.task = {};
    state.t_idle = Device.dwTimeGlobal;
}

void clear_by_sight(CAI_Stalker& npc, SearchState& state, SearchPlan* plan, u16 plan_owner)
{
    u32 rays = config().clear_rays;
    if (state.task.role == SearchRole::sweep && sees_spot(npc, state.task.pos, rays))
        finish_sweep(npc, state);

    if (!plan || plan->spots.empty())
        return;

    // A glance into one more spot of the plan the NPC holds, round robin.
    const u8 index = u8(state.ray_cursor++ % plan->spots.size());
    const SearchTask& task = state.task;
    if (task.role != SearchRole::none && task.coordinator == plan_owner && task.generation == plan->generation &&
        task.index == index)
    {
        return;
    }
    SearchSpot& spot = plan->spots[index];
    if (spot.cleared || !sees_spot(npc, spot.pos, rays))
        return;

    spot.cleared = true;
    stat_add(Stat::r_cleared, 1.f, community_of(npc));
    if (plan->lkp_spot && index == 0)
        state.lkp_checked = true;
    // A coordinator that saw into a spot it had sent someone to calls it clear.
    const bool assigned = (spot.sweeper != u16(-1) && spot.sweeper != npc.ID()) ||
        (spot.overwatch != u16(-1) && spot.overwatch != npc.ID());
    if (plan_owner == npc.ID() && assigned)
    {
        SearchTask clear;
        clear.target = state.target;
        clear.coordinator = npc.ID();
        clear.generation = plan->generation;
        clear.index = index;
        send_clear(npc, clear);
    }
}

// Own plan of a searcher no assignment reaches: build it, take the next spot, widen when every spot is cleared.
void next_own_task(CAI_Stalker& npc, SearchState& state)
{
    if (state.coordinated || !state.t_lkp || !can_sweep(npc))
        return;

    const Config& cfg = config();
    SearchPlan& plan = state.own;
    const PlanKey key = own_key(npc, state);
    if (plan.radius <= 0.f)
    {
        if (!build_step(plan, key, npc, state.lkp, state.lkp_vertex, state.escape, state.t_lkp, 0.f, cfg.spot_radius,
                community_of(npc)))
        {
            return;
        }
        plan.widened = 0;
    }

    int index = best_spot(plan, npc, false);
    while (index < 0 && exhausted(plan) && widen(plan, npc, key))
        index = best_spot(plan, npc, false);
    if (index >= 0)
        take(npc, state, make_task(plan, u8(index), SearchRole::sweep, state.target, u16(-1)));
}

void next_task(CAI_Stalker& npc, SearchState& state)
{
    const Config& cfg = config();
    const u32 now = Device.dwTimeGlobal;
    const SearchTask& task = state.task;
    if ((task.role == SearchRole::sweep && now >= task.t_given + cfg.task_max_ms) ||
        (task.role == SearchRole::overwatch && now >= task.t_given + cfg.overwatch_max_ms))
    {
        abandon(npc, state);
    }
    if (task.role != SearchRole::none)
        return;
    next_own_task(npc, state);
}

void record_give_up(SearchSquad& squad, u16 target)
{
    const u32 now = Device.dwTimeGlobal;
    const auto found = std::find_if(squad.give_ups.begin(), squad.give_ups.end(),
        [target](const SearchGiveUp& record) { return record.target == target; });
    if (found == squad.give_ups.end())
    {
        squad.give_ups.push_back({now, now, target, 1});
        return;
    }
    found->last = now;
    ++found->count;
}

void give_up(CAI_Stalker& npc, SearchState& state, const CEntityAlive& target)
{
    stat_add(Stat::r_giveup, 1.f, community_of(npc));
    if (SquadState* squad = squad_of(npc); squad && squad->search)
        record_give_up(*squad->search, state.target);
    const u16 id = state.target;
    reset(state);
    state.given_up = id;
    state.t_given_up = Device.dwTimeGlobal;
    // The stock search forgot the enemy the same way after its ambush hold; seeing him again brings him back.
    Know::forget(npc, id);
    npc.memory().enable(&target, false);
}

struct Lkp
{
    Fvector position;
    Fvector velocity;
    u32 time;
    u32 vertex;
    float err;
    Source source;
};

// The last known (or reported) position: the NPC's belief, else the stock memory entry.
bool read_lkp(const CAI_Stalker& npc, const CEntityAlive& target, Lkp& result)
{
    result.velocity.set(0.f, 0.f, 0.f);
    result.vertex = u32(-1);
    result.err = 0.f;
    if (const Belief* belief = Know::belief(npc, target.ID()))
    {
        result.position = belief->e.pos;
        // Knowledge keeps the estimate's cell: no second nearest-vertex search for the same spot.
        result.vertex = belief->e.vertex;
        result.velocity = belief->vel;
        result.time = belief->e.t_observed;
        result.err = belief->e.err;
        result.source = belief->e.source;
        return true;
    }

    const MemorySpace::CMemoryInfo info = npc.memory().memory(&target);
    if (!info.m_object)
        return false;
    result.position = info.m_object_params.m_position;
    result.vertex = info.m_object_params.m_level_vertex_id;
    result.time = info.m_last_level_time;
    result.source = info.m_visual_info ? Source::seen : info.m_hit_info ? Source::hit : Source::heard_shot;
    return true;
}

// Takes newer information about the target; true = it moved far enough from the previous LKP that a plan around
// that one is stale.
bool refresh_lkp(CAI_Stalker& npc, SearchState& state, const CEntityAlive& target)
{
    const Config& cfg = config();
    Lkp lkp;
    if (!read_lkp(npc, target, lkp) || (state.t_lkp && lkp.time <= state.t_lkp))
        return false;

    const bool had = state.t_lkp != 0;
    const Fvector previous = state.lkp;
    const float moved = had ? distance_xz(previous, lkp.position) : 0.f;
    state.lkp = lkp.position;
    state.t_lkp = lkp.time;
    state.lkp_err = lkp.err;
    state.lkp_source = lkp.source;
    state.lkp_vertex = graph().valid_vertex_id(lkp.vertex) ?
        lkp.vertex :
        nearest_vertex(lkp.position, npc.Position());

    // Escape direction: the last seen motion, else the way the estimates moved, else away from the searcher.
    Fvector escape = Fvector().set(lkp.velocity.x, 0.f, lkp.velocity.z);
    if (escape.magnitude() < cfg.escape_speed_min)
    {
        if (moved > cfg.spot_min_gap)
            escape.set(lkp.position.x - previous.x, 0.f, lkp.position.z - previous.z);
        else
            escape.set(lkp.position.x - npc.Position().x, 0.f, lkp.position.z - npc.Position().z);
    }
    state.escape = escape.magnitude() > EPS_L ? escape.normalize() : Fvector().set(0.f, 0.f, 0.f);

    return had && moved > cfg.replan_dist;
}

void start(CAI_Stalker& npc, SearchState& state, const CEntityAlive& target)
{
    const Config& cfg = config();
    const Profile& settings = profile(npc);
    const SearchTask pending = state.pending;
    reset(state);

    state.target = target.ID();
    state.active = true;
    state.t_start = Device.dwTimeGlobal;
    refresh_lkp(npc, state, target);

    const Range& range = settings.giveup_stagger_s;
    const float stagger = ::Random.randF(_min(range.lo, range.hi), _max(range.lo, range.hi));
    state.duration_ms = to_ms(settings.search_s * (sworn(npc, target) ? cfg.sworn_mult : 1.f) + stagger);
    stat_add(Stat::r_search, 1.f, community_of(npc));
    if (settings.quiet_search)
        stat_add(Stat::r_quiet, 1.f, community_of(npc));

    // An assignment that arrived before the search began.
    if (pending.role != SearchRole::none && pending.target == state.target &&
        Device.dwTimeGlobal < pending.t_given + cfg.assign_timeout_ms)
    {
        state.coordinated = true;
        take(npc, state, pending);
    }
}

bool member_busy(const SearchPool& pool, u16 id)
{
    return std::any_of(pool.plan.spots.begin(), pool.plan.spots.end(), [id](const SearchSpot& spot) {
        return !spot.cleared && (spot.sweeper == id || spot.overwatch == id);
    });
}

bool searching(const xr_vector<NpcState*>& searchers, u16 id)
{
    return std::any_of(searchers.begin(), searchers.end(), [id](const NpcState* state) { return state->id == id; });
}

NpcState* searcher(const xr_vector<NpcState*>& searchers, u16 id)
{
    const auto found = std::find_if(
        searchers.begin(), searchers.end(), [id](const NpcState* state) { return state->id == id; });
    return found == searchers.end() ? nullptr : *found;
}

// Searchers of one target in a squad that sweep (pairs or single style), alive, with state.
void collect_searchers(const SquadState& squad, u16 target, xr_vector<NpcState*>& result)
{
    result.clear();
    for (const u16 id : squad.members)
    {
        NpcState* state = npc(id);
        if (!state || state->dead || !state->search || !state->owner)
            continue;
        const SearchState& search = *state->search;
        if (search.active && search.target == target && can_sweep(*state->owner))
            result.push_back(state);
    }
}

bool anyone_searching(const SquadState& squad, u16 target)
{
    return std::any_of(squad.members.begin(), squad.members.end(), [target](u16 id) {
        const NpcState* state = npc(id);
        return state && !state->dead && state->search && state->search->active && state->search->target == target;
    });
}

// Coordinator: the searcher of the highest band; ties go to the squad's leader, then to the lowest id.
NpcState* pick_coordinator(const SquadState& squad, const xr_vector<NpcState*>& searchers)
{
    NpcState* best = nullptr;
    for (NpcState* state : searchers)
    {
        if (!best || state->band > best->band ||
            (state->band == best->band && best->id != squad.leader &&
                (state->id == squad.leader || state->id < best->id)))
        {
            best = state;
        }
    }
    return best;
}

// Sends one spot of the pool to a searcher; the coordinator gives its own task to itself.
bool assign(SearchPool& pool, CAI_Stalker& lead, NpcState& member, u8 index, SearchRole role)
{
    SearchSpot& spot = pool.plan.spots[index];
    if (member.id == lead.ID())
        take(lead, *member.search, make_task(pool.plan, index, role, pool.target, lead.ID()));
    else if (!Comms::request(lead, Comms::Kind::search_assign, pool.target,
                 pack(member.id, pool.plan.generation, index, role == SearchRole::overwatch), quiet_flags(lead)))
    {
        return false;
    }

    (role == SearchRole::sweep ? spot.sweeper : spot.overwatch) = member.id;
    spot.t_assigned = Device.dwTimeGlobal;
    if (role == SearchRole::sweep)
        ++spot.tries;
    return true;
}

void pair_up(SearchPool& pool, const xr_vector<NpcState*>& searchers)
{
    const Config& cfg = config();
    pool.pairs.erase(std::remove_if(pool.pairs.begin(), pool.pairs.end(),
                         [&](const SearchPair& pair) {
                             return !searching(searchers, pair.a) || !searching(searchers, pair.b);
                         }),
        pool.pairs.end());

    g_unpaired.clear();
    for (NpcState* state : searchers)
    {
        const bool paired = std::any_of(pool.pairs.begin(), pool.pairs.end(),
            [state](const SearchPair& pair) { return pair.a == state->id || pair.b == state->id; });
        if (!paired)
            g_unpaired.push_back(state);
    }

    while (g_unpaired.size() >= 2)
    {
        const NpcState* first = g_unpaired.front();
        auto partner = g_unpaired.end();
        float nearest = cfg.pair_max_gap;
        for (auto it = g_unpaired.begin() + 1; it != g_unpaired.end(); ++it)
        {
            const float distance = first->owner->Position().distance_to((*it)->owner->Position());
            if (distance <= nearest)
            {
                nearest = distance;
                partner = it;
            }
        }
        if (partner != g_unpaired.end())
        {
            pool.pairs.push_back({first->id, (*partner)->id});
            g_unpaired.erase(partner);
        }
        g_unpaired.erase(g_unpaired.begin());
    }
}

void hand_out(SearchPool& pool, CAI_Stalker& lead, const xr_vector<NpcState*>& searchers, bool pairs)
{
    if (pairs)
    {
        for (SearchPair& pair : pool.pairs)
        {
            NpcState* a = searcher(searchers, pair.a);
            NpcState* b = searcher(searchers, pair.b);
            if (!a || !b || member_busy(pool, a->id) || member_busy(pool, b->id))
                continue;
            NpcState& sweeper = pair.flip ? *b : *a;
            NpcState& partner = pair.flip ? *a : *b;
            const int index = best_spot(pool.plan, *sweeper.owner, true);
            if (index < 0)
                return;
            if (!assign(pool, lead, sweeper, u8(index), SearchRole::sweep))
            {
                // the order did not go out: the coordinator in the pair sweeps the spot alone
                if (partner.id == lead.ID())
                    assign(pool, lead, partner, u8(index), SearchRole::sweep);
                continue;
            }
            assign(pool, lead, partner, u8(index), SearchRole::overwatch);
            pair.flip = !pair.flip;
        }
    }

    for (NpcState* state : searchers)
    {
        const bool paired = pairs &&
            std::any_of(pool.pairs.begin(), pool.pairs.end(),
                [state](const SearchPair& pair) { return pair.a == state->id || pair.b == state->id; });
        if (paired || member_busy(pool, state->id))
            continue;
        const int index = best_spot(pool.plan, *state->owner, true);
        if (index < 0)
            return;
        assign(pool, lead, *state, u8(index), SearchRole::sweep);
    }
}

// One squad pool: keep a coordinator, keep its plan current, free timed-out assignments, widen, hand out spots.
void coordinate(SquadState& squad, SearchPool& pool)
{
    const Config& cfg = config();
    const u32 now = Device.dwTimeGlobal;
    collect_searchers(squad, pool.target, g_searchers);

    NpcState* coordinator = searcher(g_searchers, pool.coordinator);
    if (!coordinator)
    {
        if (pool.coordinator != u16(-1))
        {
            // The coordinator is gone: nobody inherits its plan, the next one starts its own after a moment.
            pool.coordinator = u16(-1);
            pool.t_coordinator_lost = now;
            pool.plan.spots.clear();
            pool.plan.radius = 0.f;
            pool.pairs.clear();
        }
        if (pool.t_coordinator_lost && now < pool.t_coordinator_lost + cfg.takeover_ms)
            return;
        coordinator = pick_coordinator(squad, g_searchers);
        if (!coordinator)
            return;
        pool.coordinator = coordinator->id;
        pool.t_coordinator_lost = 0;
        pool.plan.radius = 0.f;
    }

    CAI_Stalker& lead = *coordinator->owner;
    SearchState& own = *coordinator->search;
    own.coordinated = true;
    if (!own.t_lkp)
        return;

    const PlanKey key = pool_key(squad, pool);
    const bool stale = own.t_lkp > pool.plan.t_lkp && distance_xz(own.lkp, pool.plan.lkp) > cfg.replan_dist;
    if (pool.plan.radius <= 0.f || stale)
    {
        // A build not finished in this step goes on at the next squad step; nobody holds an order of it yet.
        if (!build_step(pool.plan, key, lead, own.lkp, own.lkp_vertex, own.escape, own.t_lkp, 0.f, cfg.spot_radius,
                squad.community))
        {
            return;
        }
        pool.plan.widened = 0;
        pool.pairs.clear();
        if (own.task.coordinator == lead.ID())
            own.task = {};
    }

    for (SearchSpot& spot : pool.plan.spots)
    {
        if (spot.cleared || (spot.sweeper == u16(-1) && spot.overwatch == u16(-1)) ||
            now < spot.t_assigned + cfg.assign_timeout_ms)
        {
            continue;
        }
        spot.sweeper = u16(-1);
        spot.overwatch = u16(-1);
        // nobody reported it clear after several orders: unreachable, dropped
        if (spot.tries >= cfg.assign_tries)
            spot.cleared = true;
    }

    if (exhausted(pool.plan) && !widen(pool.plan, lead, key))
        return;

    const bool pairs = profile(lead).search_style == SearchStyle::pairs;
    if (pairs)
        pair_up(pool, g_searchers);
    hand_out(pool, lead, g_searchers, pairs);
}

// The investigation follows the NPC's selected danger: one naming the same entity, or one within investigate_same_m
// of the spot, is the same investigation, and the spot moves to the newest estimate. No danger, or an enemy to fight,
// ends it.
Investigation* follow(CAI_Stalker& npc, SearchState& state)
{
    Investigation& inv = state.investigation;
    const CDangerObject* danger = npc.memory().danger().selected();
    if (!danger || npc.memory().enemy().selected())
    {
        inv = {};
        return nullptr;
    }

    const u16 object = danger->object() ? danger->object()->ID() : u16(-1);
    const Fvector& position = danger->position();
    const bool same = inv.active &&
        ((object != u16(-1) && object == inv.object) ||
            distance_xz(inv.spot, position) <= config().investigate_same_m);
    if (!same)
    {
        // An NPC already out looking goes on to the new spot instead of starting over in cover.
        const Investigation previous = inv;
        inv = {};
        inv.active = true;
        inv.t_start = Device.dwTimeGlobal;
        inv.object = object;
        if (previous.active && previous.role == InvestigateRole::go)
        {
            inv.role = InvestigateRole::go;
            inv.mate = previous.mate;
            inv.t_go = Device.dwTimeGlobal;
            inv.torch_counted = previous.torch_counted;
        }
    }
    if (inv.vertex == u32(-1) || inv.spot.distance_to_sqr(position) > .25f)
    {
        inv.spot = position;
        inv.vertex = nearest_vertex(position, npc.Position());
    }
    return &inv;
}

// Report target of an investigation message: the entity the danger names, when the NPC takes it for an enemy.
u16 message_target(const CAI_Stalker& npc)
{
    const CDangerObject* danger = npc.memory().danger().selected();
    const CEntityAlive* object = danger ? danger->object() : nullptr;
    return object && npc.is_relation_enemy(object) ? object->ID() : u16(-1);
}

// "Who's there?" into the dark: a search line, unless the profile searches quietly.
void call_out(CAI_Stalker& npc)
{
    if (profile(npc).quiet_search || !npc.agent_manager().member().can_cry_noninfo_phrase())
        return;
    const Config& cfg = config();
    npc.sound().play(StalkerSpace::eStalkerSoundSearch1NoAllies, 0, 0, cfg.bark_pause_hi_ms, cfg.bark_pause_lo_ms);
}

bool squad_mate(const CAI_Stalker& self, u16 id)
{
    const SquadState* squad = squad_of(self);
    return squad && std::find(squad->members.begin(), squad->members.end(), id) != squad->members.end();
}

// A partner of a pair: the nearest squad mate the NPC sees right now, so it knows he is there to take the order.
u16 pick_partner(const CAI_Stalker& self, u16 except = u16(-1))
{
    const SquadState* squad = squad_of(self);
    if (!squad)
        return u16(-1);
    u16 best = u16(-1);
    float nearest = config().pair_max_gap;
    for (const u16 id : squad->members)
    {
        const NpcState* mate = npc(id);
        if (id == self.ID() || id == except || !mate || mate->dead || !mate->owner ||
            !self.memory().visual().visible_now(mate->owner))
        {
            continue;
        }
        if (const float distance = self.Position().distance_to(mate->owner->Position()); distance <= nearest)
        {
            nearest = distance;
            best = id;
        }
    }
    return best;
}

// The NPC leaves its cover for the spot. pair: it asks a mate it sees to cover it; single: it says it goes, so mates
// who hear it hold; advance: fanatics go together without a word; a man sent by his leader just goes.
void start_going(CAI_Stalker& npc, Investigation& inv, bool announce)
{
    const Profile& settings = profile(npc);
    inv.role = InvestigateRole::go;
    inv.t_go = Device.dwTimeGlobal;
    inv.mate = u16(-1);
    stat_add(Stat::r_investigate, 1.f, community_of(npc));
    if (!announce || settings.investigate == Investigate::advance)
        return;

    const u16 target = message_target(npc);
    const u32 flags = quiet_flags(npc);
    if (settings.investigate == Investigate::pair)
    {
        const u16 partner = pick_partner(npc);
        if (partner != u16(-1) &&
            Comms::request(npc, Comms::Kind::search_assign, target,
                pack(partner, investigate_generation, investigate_index, true), flags))
        {
            inv.mate = partner;
            return;
        }
    }
    const bool told = Comms::request(npc, Comms::Kind::search_assign, target,
        pack(u16(-1), investigate_generation, investigate_index, false), flags);
    if (settings.investigate == Investigate::single && !told)
        call_out(npc);
}

// A leader of a pair-style squad stays back and sends two men he sees: one to look, one to cover him (lore: the
// leader sends two). false = nobody to send, he goes himself.
bool send_pair(CAI_Stalker& npc, Investigation& inv)
{
    const u16 goer = pick_partner(npc);
    if (goer == u16(-1))
        return false;
    const u16 target = message_target(npc);
    const u32 flags = quiet_flags(npc);
    if (!Comms::request(npc, Comms::Kind::search_assign, target,
            pack(goer, investigate_generation, investigate_go_index, false), flags))
    {
        return false;
    }
    if (const u16 cover = pick_partner(npc, goer); cover != u16(-1))
    {
        Comms::request(npc, Comms::Kind::search_assign, target,
            pack(cover, investigate_generation, investigate_index, true), flags);
    }
    inv.role = InvestigateRole::hold;
    inv.mate = goer;
    inv.t_mate = Device.dwTimeGlobal;
    return true;
}

// Done: the spot was seen (the mates are told all clear) or the time ran out; the danger is dropped the way the stock
// search of this sub-planner dropped it, and every older danger with it.
void finish_investigation(CAI_Stalker& npc, SearchState& state, bool cleared)
{
    Investigation& inv = state.investigation;
    const CDangerObject* danger = npc.memory().danger().selected();
    const CEntityAlive* object = danger ? danger->object() : nullptr;
    if (cleared)
    {
        Comms::request(npc, Comms::Kind::all_clear, message_target(npc),
            pack(inv.mate, investigate_generation, investigate_index, false), quiet_flags(npc));
    }
    if (object)
        npc.memory().enable(object, false);
    npc.memory().danger().time_line(Device.dwTimeGlobal);
    inv = {};
    state.news = {};
}

void watch_investigation(CAI_Stalker& npc, SearchState& state)
{
    ScopedTimer timer(Stat::p_search_us, community_of(npc));
    Investigation* inv = follow(npc, state);
    if (!inv)
        return;

    const Config& cfg = config();
    const u32 now = Device.dwTimeGlobal;
    // An investigator that never got to look (stuck, or out of the sub-planner) gives up; a mate's word goes stale.
    if (inv->role == InvestigateRole::go && now >= inv->t_go + cfg.investigate_max_ms)
    {
        finish_investigation(npc, state, false);
        return;
    }
    if (inv->mate != u16(-1) && inv->role != InvestigateRole::go && now >= inv->t_mate + cfg.investigate_told_ms)
    {
        inv->role = InvestigateRole::none;
        inv->mate = u16(-1);
    }

    if (inv->torch_counted || (inv->role != InvestigateRole::go && inv->role != InvestigateRole::cover))
        return;
    if (torch_lit(npc, inv->t_torch))
    {
        inv->torch_counted = true;
        stat_add(Stat::r_torch_on, 1.f, community_of(npc));
    }
}

void on_investigate_order(CAI_Stalker& npc, SearchState& state, const Comms::Message& message, const Packed& packed)
{
    if (message.origin == npc.ID())
        return;
    InvestigateNews& news = state.news;
    const bool mine = packed.addressee == npc.ID();
    news.time = Device.dwTimeGlobal;
    news.origin = message.origin;
    news.go = mine && packed.index == investigate_go_index;
    news.cover = mine && !news.go && packed.overwatch;
    // Sent or asked to cover: the spot the mate described becomes this NPC's own suspicion, so it turns to it even
    // when it heard nothing itself.
    if ((news.go || news.cover) && message.estimate.source != Source::none)
    {
        Know::suspect(npc, message.estimate, CDangerObject::eDangerTypeEnemySound,
            CDangerObject::eDangerPerceiveTypeSound);
    }
}

// "All clear" from the mate it waits for, or from anyone of its squad, ends a hold.
void on_investigate_clear(CAI_Stalker& npc, SearchState& state, const Comms::Message& message)
{
    Investigation& inv = state.investigation;
    const bool told = state.news.origin == message.origin;
    if (inv.active && inv.role != InvestigateRole::go &&
        (told || inv.mate == message.origin || squad_mate(npc, message.origin)))
    {
        npc.memory().danger().time_line(Device.dwTimeGlobal);
        inv = {};
    }
    if (told)
        state.news = {};
}

// Engine combat ended while its enemy is alive, believed and not given up: nothing but a new selection can start the
// search now (r_drop). The trace names what the enemy manager and the planner saw at that moment.
void watch_engagement(CAI_Stalker& npc, SearchState& state)
{
    if (Tactics::engine_combat(npc))
    {
        if (const CEntityAlive* enemy = npc.memory().enemy().selected())
            state.engaged = enemy->ID();
        return;
    }
    const u16 id = state.engaged;
    if (id == u16(-1))
        return;
    state.engaged = u16(-1);

    const auto* target = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(id));
    const Belief* belief = target && target->g_Alive() && id != state.given_up ? Know::belief(npc, id) : nullptr;
    if (!belief)
        return;
    stat_add(Stat::r_drop, 1.f, community_of(npc));
    if (!tracing())
        return;

    const u32 now = Device.dwTimeGlobal;
    bool cached{};
    const int callback = Know::cached_enemy_callback(npc, id, cached) ? int(cached) : -1;
    Msg("[ai] trace drop npc=%u comm=%s target=%u src=%u age=%u seen_age=%d sees=%d enemy=%d brain=%u reg=%d "
        "agent=%d useful=%d callback=%d danger=%d",
        npc.ID(), community_name(npc).c_str(), id, u32(belief->e.source), now - _min(now, belief->e.t_observed),
        belief->t_seen_own ? int(now - _min(now, belief->t_seen_own)) : -1, Know::sees_now(npc, target) ? 1 : 0,
        belief->enemy ? 1 : 0, npc.brain().initialized() ? u32(npc.brain().current_action_id()) : u32(-1),
        npc.agent_manager().member().registered_in_combat(&npc) ? 1 : 0,
        npc.agent_manager().enemy().useful_enemy(target, &npc) ? 1 : 0,
        npc.memory().enemy().useful(target) ? 1 : 0, callback, npc.memory().danger().selected() ? 1 : 0);
}
} // namespace

SearchState* create(CAI_Stalker& npc) { return xr_new<SearchState>(); }
void destroy(SearchState* state) { xr_delete(state); }
SearchSquad* create_squad(SquadState& squad) { return xr_new<SearchSquad>(); }
void destroy_squad(SearchSquad* state) { xr_delete(state); }

void update(CAI_Stalker& npc, float dt)
{
    SearchState* state = state_of(npc);
    if (!state)
        return;
    if (!feature(Feature::search))
    {
        if (state->active)
            reset(*state);
        state->investigation = {};
        state->news = {};
        return;
    }
    watch_engagement(npc, *state);
    if (state->investigation.active)
        watch_investigation(npc, *state);
    if (!state->active)
        return;

    ScopedTimer timer(Stat::p_search_us, community_of(npc));
    const Config& cfg = config();
    const u32 now = Device.dwTimeGlobal;

    const auto* target = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(state->target));
    if (!target || target->getDestroy() || !target->g_Alive())
    {
        reset(*state);
        return;
    }

    if (Know::sees_now(npc, target))
    {
        stat_add(Stat::r_found, 1.f, community_of(npc));
        reset(*state);
        return;
    }

    if (npc.memory().enemy().selected() != target)
    {
        if (!state->t_unselected)
            state->t_unselected = now;
        else if (now >= state->t_unselected + cfg.unselected_drop_ms)
        {
            reset(*state);
            return;
        }
    }
    else
        state->t_unselected = 0;

    if (!state->torch_counted && torch_lit(npc, state->t_torch))
    {
        state->torch_counted = true;
        stat_add(Stat::r_torch_on, 1.f, community_of(npc));
    }

    // News far from the old LKP: a coordinator's pool follows it in the squad step; anyone else searches around it by
    // an own plan until the coordinator's next order.
    if (refresh_lkp(npc, *state, *target))
    {
        state->lkp_checked = false;
        if (const SearchPool* pool = pool_of(npc, state->target); !pool || pool->coordinator != npc.ID())
        {
            state->coordinated = false;
            state->own.radius = 0.f;
            state->task = {};
        }
    }

    if (now >= deadline(*state))
    {
        give_up(npc, *state, *target);
        return;
    }

    SearchPool* pool = pool_of(npc, state->target);
    if (pool && pool->coordinator == npc.ID())
        clear_by_sight(npc, *state, &pool->plan, npc.ID());
    else
        clear_by_sight(npc, *state, state->coordinated ? nullptr : &state->own, u16(-1));
    next_task(npc, *state);
}

void squad_update(SquadState& squad)
{
    SearchSquad* search = squad.search;
    if (!search)
        return;
    if (!feature(Feature::search))
    {
        search->pools.clear();
        search->give_ups.clear();
        return;
    }
    if (search->pools.empty() && search->give_ups.empty() &&
        std::none_of(squad.members.begin(), squad.members.end(), [](u16 id) {
            const NpcState* state = npc(id);
            return state && state->search && state->search->active;
        }))
    {
        return;
    }

    ScopedTimer timer(Stat::p_search_us, squad.community);

    g_targets.clear();
    for (const u16 id : squad.members)
    {
        const NpcState* state = npc(id);
        if (!state || state->dead || !state->search || !state->owner || !state->search->active ||
            !can_sweep(*state->owner))
        {
            continue;
        }
        if (std::find(g_targets.begin(), g_targets.end(), state->search->target) == g_targets.end())
            g_targets.push_back(state->search->target);
    }

    search->pools.erase(std::remove_if(search->pools.begin(), search->pools.end(),
                            [](const SearchPool& pool) {
                                return std::find(g_targets.begin(), g_targets.end(), pool.target) == g_targets.end();
                            }),
        search->pools.end());

    for (const u16 target : g_targets)
    {
        SearchPool* pool = find_pool(*search, target);
        if (!pool)
        {
            pool = &search->pools.emplace_back();
            pool->target = target;
        }
        coordinate(squad, *pool);
    }

    // Give-up spread of a squad: known once nobody of it searches the target any more.
    search->give_ups.erase(std::remove_if(search->give_ups.begin(), search->give_ups.end(),
                               [&squad](const SearchGiveUp& record) {
                                   if (anyone_searching(squad, record.target))
                                       return false;
                                   if (record.count >= 2)
                                       stat_add(Stat::r_giveup_spread_ms, float(record.last - record.first),
                                           squad.community);
                                   return true;
                               }),
        search->give_ups.end());
}

void on_death(CAI_Stalker& npc, IGameObject* who)
{
    if (SearchState* state = state_of(npc))
    {
        reset(*state);
        state->investigation = {};
        state->news = {};
    }
}

void level_unload()
{
    g_config = {};
    g_covers.clear();
    g_targets.clear();
    g_searchers.clear();
    g_unpaired.clear();
    // The builds' buffers held a level's covers: freed with it.
    for (PlanBuild& build : g_builds)
        build = {};
    g_plan_frame = u32(-1);
    g_plan_builds = 0;
}

bool active(const CAI_Stalker& npc)
{
    const SearchState* state = state_of(npc);
    if (!feature(Feature::search) || !state)
        return false;
    const InvestigateRole role = state->investigation.role;
    return state->active ||
        (state->investigation.active && (role == InvestigateRole::go || role == InvestigateRole::cover));
}

bool quiet(const CAI_Stalker& npc)
{
    const SearchState* state = state_of(npc);
    return feature(Feature::search) && state && (state->active || state->investigation.active) &&
        profile(npc).quiet_search;
}

void bark_pause(u32& max_stop, u32& min_stop)
{
    const Config& cfg = config();
    max_stop = cfg.bark_pause_hi_ms;
    min_stop = cfg.bark_pause_lo_ms;
}

bool sneak(const CAI_Stalker& npc, const Fvector& spot)
{
    return quiet(npc) && profile(npc).investigate != Investigate::advance &&
        distance_xz(npc.Position(), spot) <= config().sneak_dist;
}

bool contact_lost(const CAI_Stalker& npc)
{
    if (!feature(Feature::search) || !state_of(npc))
        return false;
    const float delay = profile(npc).search_delay_s;
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    if (delay < 0.f || !enemy || !enemy->g_Alive() || Know::sees_now(npc, enemy))
        return false;
    // The newest own sighting, heard or told estimate the NPC holds (the stock memory time without knowledge).
    const u32 time = npc.memory().memory_time(enemy);
    return time && Device.dwTimeGlobal >= time + to_ms(delay);
}

void on_contact_lost(CAI_Stalker& npc, u16 id)
{
    // The clock of a search runs from its start or from the newest information, whichever is later; the loss itself
    // is the LKP's observation time, so a running search only takes a fresh LKP.
    SearchState* state = state_of(npc);
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    if (!state || !feature(Feature::search) || !enemy || enemy->ID() != id)
        return;
    stat_add(Stat::r_handover, 1.f, community_of(npc));
    const bool running = state->active && state->target == id;
    if (running)
        refresh_lkp(npc, *state, *enemy);
    else
        begin(npc);
    if (tracing())
    {
        const u32 now = Device.dwTimeGlobal;
        const u32 seen = npc.memory().memory_time(enemy);
        Msg("[ai] trace handover npc=%u comm=%s target=%u age=%u delay_ms=%u searching=%d resumed=%d given_up=%d",
            npc.ID(), community_name(npc).c_str(), id, now - _min(now, seen), to_ms(profile(npc).search_delay_s),
            state->active && state->target == id ? 1 : 0, running ? 1 : 0, state->given_up == id ? 1 : 0);
    }
}

void on_message(CAI_Stalker& npc, const Comms::Message& message)
{
    SearchState* state = state_of(npc);
    if (!state || !feature(Feature::search))
        return;

    const Packed packed = unpack(message.extra);
    switch (message.kind)
    {
    case Comms::Kind::search_assign:
    {
        if (investigation_message(packed))
        {
            on_investigate_order(npc, *state, message, packed);
            return;
        }
        if (packed.addressee != npc.ID())
            return;
        // The order may reach the member through a mate's relay: the coordinator is its origin.
        const SearchPool* pool = pool_of(npc, message.target);
        if (!pool || pool->coordinator != message.origin ||
            !same_generation(pool->plan.generation, packed.generation) || packed.index >= pool->plan.spots.size())
        {
            return;
        }
        const SearchTask task = make_task(pool->plan, packed.index,
            packed.overwatch ? SearchRole::overwatch : SearchRole::sweep, message.target, message.origin);
        if (state->active && state->target == message.target)
        {
            state->coordinated = true;
            take(npc, *state, task);
        }
        else
            state->pending = task;
        break;
    }
    case Comms::Kind::all_clear:
    {
        if (investigation_message(packed))
        {
            on_investigate_clear(npc, *state, message);
            return;
        }
        // The coordinator marks the spot of its pool.
        if (packed.addressee == npc.ID())
        {
            SearchPool* pool = pool_of(npc, message.target);
            if (pool && pool->coordinator == npc.ID() && same_generation(pool->plan.generation, packed.generation) &&
                packed.index < pool->plan.spots.size())
            {
                pool->plan.spots[packed.index].cleared = true;
            }
        }
        // Whoever works the same spot (the covering partner, a sweeper the coordinator got ahead of) is done.
        const SearchTask& task = state->task;
        if (task.role != SearchRole::none && task.target == message.target && task.coordinator == packed.addressee &&
            same_generation(task.generation, packed.generation) && task.index == packed.index)
        {
            state->task = {};
            state->t_idle = Device.dwTimeGlobal;
        }
        break;
    }
    default: break;
    }
}

void begin(CAI_Stalker& npc)
{
    SearchState* state = state_of(npc);
    if (!state || !feature(Feature::search))
        return;
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    if (!enemy || (state->active && state->target == enemy->ID()))
        return;
    // A target the NPC gave up on is searched again only after newer information about it.
    if (Lkp lkp; enemy->ID() == state->given_up && (!read_lkp(npc, *enemy, lkp) || lkp.time <= state->t_given_up))
        return;

    start(npc, *state, *enemy);

    // The first searcher of its squad coordinates at once instead of waiting for the squad step.
    if (!can_sweep(npc))
        return;
    SquadState* squad = squad_of(npc);
    if (!squad || !squad->search || find_pool(*squad->search, state->target))
        return;
    SearchPool& pool = squad->search->pools.emplace_back();
    pool.target = state->target;
    coordinate(*squad, pool);
}

const SearchState* state(const CAI_Stalker& npc)
{
    const SearchState* result = state_of(npc);
    return result && result->active ? result : nullptr;
}

const SearchTask* task(const CAI_Stalker& npc)
{
    const SearchState* current = state(npc);
    return current && current->task.role != SearchRole::none ? &current->task : nullptr;
}

bool wants_sweep(const CAI_Stalker& npc)
{
    if (!feature(Feature::search))
        return false;
    const SearchState* current = state(npc);
    if (!current)
        return false;
    if (current->task.role == SearchRole::sweep)
        return true;
    // Between two assignments a coordinated searcher holds and watches instead of falling back to the ambush chain.
    return current->task.role == SearchRole::none && current->coordinated && current->t_idle &&
        Device.dwTimeGlobal < current->t_idle + config().assign_wait_ms;
}

bool wants_overwatch(const CAI_Stalker& npc)
{
    const SearchState* current = state(npc);
    return feature(Feature::search) && current && current->task.role == SearchRole::overwatch;
}

bool holds_in_place(const CAI_Stalker& npc)
{
    return feature(Feature::search) && state(npc) && profile(npc).search_style == SearchStyle::none;
}

bool lkp_checked(const CAI_Stalker& npc)
{
    const SearchState* current = state(npc);
    return feature(Feature::search) && current && current->lkp_checked;
}

bool on_hold_complete(CAI_Stalker& npc)
{
    const SearchState* current = state(npc);
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    return feature(Feature::search) && current && enemy && current->target == enemy->ID();
}

bool investigate_skip_cover(CAI_Stalker& npc)
{
    SearchState* current = state_of(npc);
    if (!current || !feature(Feature::search))
        return false;
    const Investigation* inv = follow(npc, *current);
    if (!inv)
        return false;
    const InvestigateNews& news = current->news;
    const bool sent = news.go && Device.dwTimeGlobal < news.time + config().investigate_told_ms;
    return inv->role == InvestigateRole::go || sent || profile(npc).investigate == Investigate::advance;
}

bool investigate_leave(CAI_Stalker& npc, bool completed, bool stock_leave)
{
    SearchState* current = state_of(npc);
    if (!current || !feature(Feature::search))
        return stock_leave;
    Investigation* inv = follow(npc, *current);
    if (!inv)
        return stock_leave;
    if (inv->role == InvestigateRole::go)
        return true;

    const Profile& settings = profile(npc);
    if (settings.investigate == Investigate::advance)
    {
        start_going(npc, *inv, true);
        return true;
    }
    // A danger farther than the reach is watched from where the NPC stands: nobody walks a quarter of a kilometre to a
    // report or a glimpse (the checkpoint's guards on Cordon left their post for a man seen on a hill).
    if (distance_xz(npc.Position(), inv->spot) > config().investigate_reach_m)
    {
        inv->role = InvestigateRole::hold;
        return false;
    }

    // A mate said he goes, asked to be covered, or the leader sent this NPC: the word of a mate replaces the stock
    // quota, which counted the agent's detouring members without anyone telling anybody.
    InvestigateNews& news = current->news;
    if (news.origin != u16(-1) && Device.dwTimeGlobal < news.time + config().investigate_told_ms)
    {
        if (news.go)
        {
            news = {};
            start_going(npc, *inv, false);
            return true;
        }
        inv->role = news.cover ? InvestigateRole::cover : InvestigateRole::hold;
        inv->mate = news.origin;
        inv->t_mate = news.time;
        return false;
    }
    // Waiting for a mate until he calls all clear or his time runs out (update() then drops the wait).
    if (inv->mate != u16(-1))
        return false;
    if (!completed)
        return false;

    // Style none: wait in cover and call out, never walk over.
    if (settings.investigate == Investigate::none)
    {
        inv->role = InvestigateRole::hold;
        if (!inv->called_out)
        {
            inv->called_out = true;
            call_out(npc);
        }
        return false;
    }
    if (settings.investigate == Investigate::pair && !inv->ordered && is_leader(npc))
    {
        inv->ordered = true;
        if (send_pair(npc, *inv))
            return false;
    }
    start_going(npc, *inv, true);
    return true;
}

bool investigate_skip_detour(CAI_Stalker& npc)
{
    SearchState* current = state_of(npc);
    if (!current || !feature(Feature::search))
        return false;
    const Investigation* inv = follow(npc, *current);
    if (!inv || inv->role != InvestigateRole::go)
        return false;
    if (profile(npc).investigate == Investigate::advance)
        return true;
    // The stock detour moves around an entity the NPC remembers; a spot without one is approached directly.
    const CEntityAlive* object = npc.memory().danger().selected()->object();
    return !object || !npc.memory().memory(object).m_object;
}

const Investigation* investigating(CAI_Stalker& npc)
{
    SearchState* current = state_of(npc);
    if (!current || !feature(Feature::search))
        return nullptr;
    const Investigation* inv = follow(npc, *current);
    return inv && inv->role == InvestigateRole::go ? inv : nullptr;
}

bool investigate_check(CAI_Stalker& npc)
{
    SearchState* current = state_of(npc);
    if (!current || !feature(Feature::search))
        return false;
    const Investigation* inv = follow(npc, *current);
    if (!inv || inv->role != InvestigateRole::go)
        return false;
    u32 rays = config().clear_rays;
    const bool seen = sees_spot(npc, inv->spot, rays);
    if (!seen && Device.dwTimeGlobal < inv->t_go + config().investigate_max_ms)
        return false;
    finish_investigation(npc, *current, seen);
    return true;
}
} // namespace DaAi::Search
