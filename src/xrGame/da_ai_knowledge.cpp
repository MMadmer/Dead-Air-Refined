// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_knowledge.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"
#include "da_ai_senses.h"
#include "da_ai_tactics.h"

#include "ai/stalker/ai_stalker.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "hit_memory_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "danger_object.h"
#include "memory_space_impl.h"
#include "stalker_movement_manager_smart_cover.h"
#include "Actor.h"
#include "Level.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrEngine/CameraBase.h"
#include "Include/xrRender/Kinematics.h"

namespace DaAi
{
struct KnowScratch
{
    xr_vector<MemorySpace::CVisibleObject> visible;
    xr_vector<MemorySpace::CSoundObject> sound;
    xr_vector<MemorySpace::CHitObject> hit;
};

namespace Know
{
namespace
{
constexpr pcstr errors_section = "ai_errors";
constexpr size_t source_count = size_t(Source::restored) + 1;
constexpr size_t channel_count = size_t(Source::report_hq) - size_t(Source::report_voice) + 1;

struct Linear
{
    float e0{};
    float k{};
};

// [ai_errors] of dead_air_x64_ai.ltx; the initial values are the compiled-in defaults of plan 2.5.
struct Config
{
    float extrap_s{.6f};
    float extrap_band[4]{.8f, 1.f, 1.2f, 1.4f};
    bool see_me_unknown{true};
    float see_me_cone_deg{60.f};
    float see_me_ray_s{.5f};
    float restore_grace_s{10.f};
    float script{25.f};
    float realise{.6f};
    float age_growth{1.5f};
    float age_cap{60.f};
    float clarity_k{1.5f};
    float center_height{.9f};
    Linear linear[source_count]{};
    float hit_cone_deg[2]{5.f, 20.f};
    float hit_distance_err{.4f};
    float near_miss_cone_deg{25.f};
    float near_miss_distance_prior[2]{20.f, 80.f};
    float witness_cone_deg{30.f};
    float witness_distance_prior[2]{30.f, 60.f};
    float describe_k[channel_count]{.15f, .15f, .1f, .2f, .1f, .1f};
    float describe_grid[channel_count]{5.f, 5.f, 10.f, 5.f, 25.f, 25.f};
    float forget_enemy_s{300.f};
    float forget_other_s{60.f};
    float reacquire_s{1.f};
    float aim_reroll_s{3.f};
    // Set by the config load: a literal here would dock a string while the DLL initialises, before xrCore's pool.
    shared_str bone_head;
    shared_str bone_body;
    float kill_wounded_seen_s{3.f};
    u32 beliefs_max{16};
    float vertex_search_m{22.f};
    u32 vertex_march_steps{16};
    // Near the player, far from him (level of detail).
    u32 enemy_callback_ms[2]{1000, 3000};
    u32 maintain_ms[2]{250, 1000};
    u32 relation_refresh_ms{1000};
    u32 sight_hold_ms{750};
    bool loaded{};
};

Config g_cfg;

// Reads "a, b, ..." into values; missing items keep their defaults.
void read_floats(pcstr key, float* values, int count)
{
    if (!pSettings->line_exist(errors_section, key))
        return;
    const pcstr line = pSettings->r_string(errors_section, key);
    if (!line)
        return;
    const int items = _min(count, _GetItemCount(line));
    string64 item;
    for (int index = 0; index < items; ++index)
        values[index] = float(atof(_GetItem(line, index, item, sizeof(item))));
}

void read_linear(pcstr key, Source source)
{
    float values[2]{g_cfg.linear[size_t(source)].e0, g_cfg.linear[size_t(source)].k};
    read_floats(key, values, 2);
    g_cfg.linear[size_t(source)] = {values[0], values[1]};
}

void load_config()
{
    g_cfg = {};
    g_cfg.linear[size_t(Source::heard_shot)] = {3.f, .08f};
    g_cfg.linear[size_t(Source::heard_step)] = {2.f, .12f};
    g_cfg.linear[size_t(Source::heard_reload)] = {2.f, .1f};
    g_cfg.linear[size_t(Source::heard_item)] = {2.f, .1f};
    g_cfg.linear[size_t(Source::heard_voice)] = {2.f, .1f};
    g_cfg.linear[size_t(Source::muzzle_flash)] = {1.f, .02f};
    g_cfg.linear[size_t(Source::flashlight)] = {2.f, .05f};
    g_cfg.loaded = true;
    if (!pSettings->section_exist(errors_section))
        return;

    const auto read = [](pcstr key, float fallback) {
        return READ_IF_EXISTS(pSettings, r_float, errors_section, key, fallback);
    };

    g_cfg.extrap_s = read("extrap_s", g_cfg.extrap_s);
    read_floats("extrap_band", g_cfg.extrap_band, 4);
    g_cfg.see_me_unknown = read("see_me_unknown", 1.f) != 0.f;
    g_cfg.see_me_cone_deg = read("see_me_cone_deg", g_cfg.see_me_cone_deg);
    g_cfg.see_me_ray_s = read("see_me_ray_s", g_cfg.see_me_ray_s);
    g_cfg.restore_grace_s = read("restore_grace_s", g_cfg.restore_grace_s);
    g_cfg.script = read("script", g_cfg.script);
    g_cfg.realise = read("realise", g_cfg.realise);
    g_cfg.age_growth = read("age_growth", g_cfg.age_growth);
    g_cfg.age_cap = read("age_cap", g_cfg.age_cap);
    g_cfg.clarity_k = read("clarity_k", g_cfg.clarity_k);
    g_cfg.center_height = read("center_height", g_cfg.center_height);
    read_linear("heard_shot", Source::heard_shot);
    read_linear("heard_step", Source::heard_step);
    // One line for every other heard kind (the key name the hearing sense reads too).
    read_linear("heard_other", Source::heard_reload);
    g_cfg.linear[size_t(Source::heard_item)] = g_cfg.linear[size_t(Source::heard_reload)];
    g_cfg.linear[size_t(Source::heard_voice)] = g_cfg.linear[size_t(Source::heard_reload)];
    read_linear("muzzle_flash", Source::muzzle_flash);
    read_linear("flashlight", Source::flashlight);
    read_floats("hit_cone_deg", g_cfg.hit_cone_deg, 2);
    g_cfg.hit_distance_err = read("hit_distance_err", g_cfg.hit_distance_err);
    g_cfg.near_miss_cone_deg = read("near_miss_cone_deg", g_cfg.near_miss_cone_deg);
    read_floats("near_miss_distance_prior", g_cfg.near_miss_distance_prior, 2);
    g_cfg.witness_cone_deg = read("witness_cone_deg", g_cfg.witness_cone_deg);
    read_floats("witness_distance_prior", g_cfg.witness_distance_prior, 2);

    constexpr pcstr channels[channel_count]{"voice", "hushed", "radio", "gesture", "pda", "hq"};
    for (size_t index = 0; index < channel_count; ++index)
    {
        string64 key;
        xr_sprintf(key, "describe_%s", channels[index]);
        float values[2]{g_cfg.describe_k[index], g_cfg.describe_grid[index]};
        read_floats(key, values, 2);
        g_cfg.describe_k[index] = values[0];
        g_cfg.describe_grid[index] = values[1];
    }

    g_cfg.forget_enemy_s = read("forget_enemy_s", g_cfg.forget_enemy_s);
    g_cfg.forget_other_s = read("forget_other_s", g_cfg.forget_other_s);
    g_cfg.reacquire_s = read("reacquire_s", g_cfg.reacquire_s);
    g_cfg.aim_reroll_s = read("aim_reroll_s", g_cfg.aim_reroll_s);
    g_cfg.bone_head = READ_IF_EXISTS(pSettings, r_string, errors_section, "aim_bone_head", "bip01_head");
    g_cfg.bone_body = READ_IF_EXISTS(pSettings, r_string, errors_section, "aim_bone_body", "bip01_spine2");
    g_cfg.kill_wounded_seen_s = read("kill_wounded_seen_s", g_cfg.kill_wounded_seen_s);
    g_cfg.beliefs_max = READ_IF_EXISTS(pSettings, r_u32, errors_section, "beliefs_max", g_cfg.beliefs_max);
    clamp(g_cfg.beliefs_max, 4u, 64u);
    g_cfg.vertex_search_m = read("vertex_search_m", g_cfg.vertex_search_m);
    g_cfg.vertex_march_steps =
        READ_IF_EXISTS(pSettings, r_u32, errors_section, "vertex_march_steps", g_cfg.vertex_march_steps);
    const auto read_pair = [](pcstr key, u32 (&values)[2]) {
        float pair[2]{float(values[0]), float(values[1])};
        read_floats(key, pair, 2);
        values[0] = u32(_max(pair[0], 0.f));
        values[1] = u32(_max(pair[1], 0.f));
    };
    read_pair("enemy_callback_ms", g_cfg.enemy_callback_ms);
    read_pair("maintain_ms", g_cfg.maintain_ms);
    g_cfg.relation_refresh_ms =
        READ_IF_EXISTS(pSettings, r_u32, errors_section, "relation_refresh_ms", g_cfg.relation_refresh_ms);
    g_cfg.sight_hold_ms = READ_IF_EXISTS(pSettings, r_u32, errors_section, "sight_hold_ms", g_cfg.sight_hold_ms);
}

const Config& cfg()
{
    if (!g_cfg.loaded)
        load_config();
    return g_cfg;
}

u32 now() { return Device.dwTimeGlobal; }
u32 ms(float seconds) { return seconds > 0.f ? u32(seconds * 1000.f) : 0; }

KnowState* state_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state && !state->dead ? state->know : nullptr;
}

KnowState* active_state(const CAI_Stalker& npc) { return feature(Feature::knowledge) ? state_of(npc) : nullptr; }

int community_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state ? state->community : -1;
}

u8 band_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state && state->band >= 1 && state->band <= 4 ? state->band : 2;
}

Belief* find(KnowState& state, u16 id)
{
    for (Belief& belief : state.beliefs)
    {
        if (belief.e.target == id)
            return &belief;
    }
    return nullptr;
}

const Belief* find(const KnowState& state, u16 id) { return find(const_cast<KnowState&>(state), id); }

CGameObject* resolve(u16 id)
{
    if (id == u16(-1))
        return nullptr;
    auto* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object && !object->getDestroy() ? object : nullptr;
}

const CEntityAlive* resolve_alive(u16 id)
{
    const auto* entity = smart_cast<const CEntityAlive*>(resolve(id));
    return entity && entity->g_Alive() && !entity->H_Parent() ? entity : nullptr;
}

// The stock memory convention of a position: x, z of the object's position, y of its centre.
Fvector memory_convention(const CGameObject& object)
{
    Fvector result;
    object.Center(result);
    result.x = object.Position().x;
    result.z = object.Position().z;
    return result;
}

bool visual_class(Source source)
{
    switch (source)
    {
    case Source::seen:
    case Source::report_voice:
    case Source::report_hushed:
    case Source::report_radio:
    case Source::report_gesture:
    case Source::report_pda:
    case Source::report_hq:
    case Source::witness:
    case Source::script:
    case Source::restored: return true;
    default: return false;
    }
}

bool heard_class(Source source) { return source >= Source::heard_shot && source <= Source::heard_voice; }
bool report_class(Source source) { return source >= Source::report_voice && source <= Source::witness; }

float effective_error_at(const Estimate& estimate, u32 time)
{
    const float age = time > estimate.t_observed ? float(time - estimate.t_observed) / 1000.f : 0.f;
    const Config& config = cfg();
    const float growth = _min(config.age_growth * age, _max(0.f, config.age_cap - estimate.err));
    return estimate.err + growth;
}

u32 extrap_ms(const CAI_Stalker& npc) { return ms(cfg().extrap_s * cfg().extrap_band[band_of(npc) - 1]); }

// The belief's position now: a lost target keeps moving with its last seen velocity for the extrapolation window.
Fvector believed_position(const CAI_Stalker& npc, const Belief& belief)
{
    if (belief.e.source != Source::seen || belief.vel.similar(Fvector().set(0.f, 0.f, 0.f)) || !belief.t_seen_own)
        return belief.e.pos;
    const u32 elapsed = _min(now() - belief.t_seen_own, extrap_ms(npc));
    return Fvector().mad(belief.e.pos, belief.vel, float(elapsed) / 1000.f);
}

u32 believed_vertex(const CAI_Stalker& npc, const Belief& belief, const Fvector& position)
{
    if (position.similar(belief.e.pos) || !ai().get_level_graph())
        return belief.e.vertex;
    const CLevelGraph& graph = ai().level_graph();
    return graph.valid_vertex_id(belief.e.vertex) ? graph.vertex(belief.e.vertex, position) :
                                                     find_vertex(position, &npc.Position());
}

bool sanitize(Estimate& estimate)
{
    if (!_valid(estimate.pos) || !_valid(estimate.err) || !_valid(estimate.confidence))
        return false;
    if (u8(estimate.source) >= source_count || estimate.source == Source::none)
        return false;
    estimate.err = _max(estimate.err, 0.f);
    clamp(estimate.confidence, 0.f, 1.f);
    if (!estimate.t_observed || estimate.t_observed > now())
        estimate.t_observed = now();
    return true;
}

void count_source(const CAI_Stalker& npc, Source source)
{
    const int community = community_of(npc);
    if (heard_class(source))
        stat_add(Stat::k_heard, 1.f, community);
    else if (source == Source::hit || source == Source::near_miss)
        stat_add(Stat::k_hit, 1.f, community);
    else if (report_class(source))
        stat_add(Stat::k_report, 1.f, community);
    else if (source == Source::script)
        stat_add(Stat::k_rumour, 1.f, community);
}

Belief& insert(const CAI_Stalker& npc, KnowState& state)
{
    if (state.beliefs.size() < cfg().beliefs_max)
        return state.beliefs.emplace_back();

    // Evict the oldest non-enemy, then the oldest.
    Belief* victim = nullptr;
    for (Belief& belief : state.beliefs)
    {
        if (!belief.enemy && (!victim || belief.t_received < victim->t_received))
            victim = &belief;
    }
    if (!victim)
    {
        for (Belief& belief : state.beliefs)
        {
            if (!victim || belief.t_received < victim->t_received)
                victim = &belief;
        }
    }
    stat_add(Stat::k_forgot, 1.f, community_of(npc));
    *victim = {};
    return *victim;
}

// Stores an estimate into the NPC's belief about a creature; a worse estimate than the current one is ignored.
void store(const CAI_Stalker& npc, KnowState& state, const CEntityAlive& target, const Estimate& estimate)
{
    const u32 time = now();
    const bool enemy = target.g_Alive() && npc.is_relation_enemy(&target);
    Belief* belief = find(state, target.ID());

    if (estimate.source == Source::seen)
    {
        if (!belief)
            belief = &insert(npc, state);

        const bool acquired = !belief->t_seen_own || time > belief->t_seen_own + ms(cfg().reacquire_s);
        Fvector velocity{};
        if (!acquired && belief->e.source == Source::seen && time > belief->t_seen_own)
        {
            const float dt = float(time - belief->t_seen_own) / 1000.f;
            velocity.sub(estimate.pos, belief->e.pos).div(dt);
            velocity.y = 0.f;
            velocity.lerp(belief->vel, velocity, .5f);
        }
        belief->e = estimate;
        belief->vel = velocity;
        belief->t_received = time;
        belief->t_seen_own = time;
        belief->t_seen_frame = Device.dwFrame;
        belief->enemy = enemy;
        if (acquired)
            stat_add(Stat::k_seen, 1.f, community_of(npc));
        return;
    }

    // Keep the more precise of the two as it stands now; an own sighting wins a tie.
    if (belief && belief->e.source != Source::none)
    {
        const float incoming = effective_error_at(estimate, time);
        const float held = effective_error_at(belief->e, time);
        if (incoming > held || (incoming == held && belief->e.source == Source::seen))
        {
            belief->enemy = enemy;
            return;
        }
    }

    if (!belief)
        belief = &insert(npc, state);

    const u32 seen_own = belief->t_seen_own;
    const u32 seen_frame = belief->t_seen_frame;
    belief->e = estimate;
    belief->vel = {};
    belief->t_received = time;
    belief->t_seen_own = seen_own;
    belief->t_seen_frame = seen_frame;
    belief->enemy = enemy;
    count_source(npc, estimate.source);
}

bool bone_point(const CGameObject& target, const shared_str& bone, Fvector& result)
{
    IKinematics* kinematics = smart_cast<IKinematics*>(const_cast<CGameObject&>(target).Visual());
    if (!kinematics || !bone.size())
        return false;
    const u16 bone_id = kinematics->LL_BoneID(bone);
    if (bone_id == BI_NONE)
        return false;
    Fmatrix final;
    final.mul_43(target.XFORM(), kinematics->LL_GetTransform(bone_id));
    result = final.c;
    return true;
}

void roll_aim(const CAI_Stalker& npc, KnowState& state, u16 target)
{
    const u32 time = now();
    if (state.aim_target == target && time < state.aim_roll_time + ms(cfg().aim_reroll_s))
        return;
    const Profile& profile = DaAi::profile(npc);
    state.aim_target = target;
    state.aim_roll_time = time;
    state.aim_head = ::Random.randF(0.f, 1.f) < profile.aim_head;
    state.aim_lead = 1.f + ::Random.randF(-profile.lead_error, profile.lead_error);
}

// Where an unseen creature is aimed or looked at: the belief, else straight ahead of the NPC's own eyes.
Fvector unseen_point(const CAI_Stalker& npc, KnowState& state, const CGameObject& target)
{
    if (state.aim_fallback_target != target.ID())
    {
        state.aim_fallback_target = target.ID();
        stat_add(Stat::k_aim_fallback, 1.f, community_of(npc));
    }
    if (const Belief* belief = find(state, target.ID()))
        return believed_position(npc, *belief);
    const auto& head = const_cast<CAI_Stalker&>(npc).movement().m_head.current;
    return Fvector().mad(npc.eye_matrix.c, Fvector().setHP(-head.yaw, -head.pitch), 10.f);
}

void fill_info(const CAI_Stalker& npc, const Belief& belief, const CEntityAlive& target, bool seen,
    MemorySpace::CMemoryInfo& result)
{
    const u32 time = now();
    result.m_object = &target;
    if (seen)
    {
        result.m_object_params.fill(static_cast<const CGameObject*>(&target));
        result.m_level_time = time;
    }
    else
    {
        result.m_object_params.m_position = believed_position(npc, belief);
        result.m_object_params.m_level_vertex_id = believed_vertex(npc, belief, result.m_object_params.m_position);
        result.m_level_time = belief.e.t_observed;
    }
    result.m_last_level_time = result.m_level_time;
    result.m_self_params.fill(static_cast<const CGameObject*>(&npc));
    const squad_mask_type mask = npc.memory().visual().mask();
    result.m_squad_mask.assign(mask);
    result.m_visible.assign(seen ? mask : 0);
    result.m_enabled = true;
    const Source source = seen ? Source::seen : belief.e.source;
    result.m_visual_info = visual_class(source) || source == Source::muzzle_flash || source == Source::flashlight;
    result.m_sound_info = heard_class(source);
    result.m_hit_info = source == Source::hit || source == Source::near_miss;
}

KnowScratch& scratch_of(KnowState& state)
{
    if (!state.scratch)
        state.scratch = xr_new<KnowScratch>();
    return *state.scratch;
}

// Where a creature's eyes are and where it looks, as an observer who sees it can tell (head and weapon facing).
bool view_of(const CEntityAlive& who, Fvector& eye, Fvector& direction)
{
    if (const auto* monster = smart_cast<const CCustomMonster*>(&who))
    {
        eye = monster->eye_matrix.c;
        direction = monster->eye_matrix.k;
    }
    else if (const auto* actor = smart_cast<const CActor*>(&who))
    {
        CCameraBase* camera = const_cast<CActor*>(actor)->cam_Active();
        if (!camera)
            return false;
        Fvector position, normal;
        camera->Get(position, direction, normal);
        eye.set(actor->Position().x, actor->Position().y + actor->CurrentHeight, actor->Position().z);
    }
    else
        return false;
    return _valid(eye) && _valid(direction) && direction.square_magnitude() > EPS_S;
}

// The LOS part of enemy_sees_me: one budgeted ray per target and [ai_errors] see_me_ray_s. A deferred ray keeps the
// last answer about this enemy, else assumes the line is open (this NPC sees the enemy along the same line).
bool see_me_line(KnowState& state, const CEntityAlive& enemy, const Fvector& eye, const Fvector& head)
{
    const u32 time = now();
    const bool same = state.see_me_target == enemy.ID();
    if (same && time < state.see_me_time + ms(cfg().see_me_ray_s))
        return state.see_me_los;
    if (!RayBudget::left())
        return same ? state.see_me_los : true;
    state.see_me_target = enemy.ID();
    state.see_me_time = time;
    state.see_me_los = Sense::los(eye, head, &enemy);
    return state.see_me_los;
}

// The first vertex of every x column of the xz-sorted vertex array (the last entry is the vertex count), built once
// per level graph. A column's cells are then found by a search inside the column, not over the whole graph: each of
// those was a dozen and more cache misses, and a window 32 cells wide over empty ground was 65 of them.
struct ColumnIndex
{
    const CLevelGraph* graph{};
    u32 vertices{};
    xr_vector<u32> first;
};
ColumnIndex g_columns;

const xr_vector<u32>& column_index(const CLevelGraph& graph)
{
    const u32 count = graph.header().vertex_count();
    if (g_columns.graph == &graph && g_columns.vertices == count)
        return g_columns.first;
    g_columns.graph = &graph;
    g_columns.vertices = count;
    g_columns.first.clear();
    const u32 row_length = graph.row_length();
    if (!count || !row_length)
        return g_columns.first;

    const auto begin = graph.begin();
    u32 previous = 0;
    for (u32 index = 0; index < count; ++index)
    {
        const u32 xz = begin[index].position().xz();
        // Not sorted: the searches keep the whole-array binary search.
        if (xz < previous)
        {
            g_columns.first.clear();
            return g_columns.first;
        }
        previous = xz;
        for (const u32 column = xz / row_length; g_columns.first.size() <= column;)
            g_columns.first.push_back(index);
    }
    g_columns.first.push_back(count);
    return g_columns.first;
}

// The vertex nearest to pos (squared distance to its cell contour, the stock search's metric) among the cells within
// radius cells of it: one search per column of the xz-sorted vertex array, then the column's few cells.
u32 nearest_in_window(const CLevelGraph& graph, const Fvector& pos, int radius, float& distance_sqr)
{
    distance_sqr = flt_max;
    u32 result = u32(-1);
    const float cell = graph.header().cell_size();
    const Fvector& origin = graph.header().box().vMin;
    const int row_length = int(graph.row_length());
    const int column = iFloor((pos.x - origin.x) / cell + .5f);
    const int row = iFloor((pos.z - origin.z) / cell + .5f);
    const int x0 = _max(column - radius, 0), x1 = _min(column + radius, int(graph.max_x()));
    const int z0 = _max(row - radius, 0), z1 = _min(_min(row + radius, int(graph.max_z())), row_length - 1);
    if (x0 > x1 || z0 > z1)
        return result;

    // The contour distance of a cell is at least the flat distance to its square: columns and cells farther than the
    // best one so far are skipped on that alone, and the columns go outward from the spot's own, so the full distance
    // is taken of a few cells near the spot instead of every cell of the window (a window 32 cells wide was ~0.5 ms).
    const float half = cell * .5f;
    const auto gap_sqr = [half](float centre, float at) {
        const float gap = _abs(centre - at) - half;
        return gap > 0.f ? gap * gap : 0.f;
    };
    const auto begin = graph.begin(), end = graph.end();
    const xr_vector<u32>& columns = column_index(graph);
    const auto scan_column = [&](int x, float gap_x) {
        const u32 base = u32(x * row_length);
        const u32 first = base + u32(z0), last = base + u32(z1);
        auto from = begin, to = end;
        if (!columns.empty())
        {
            // Columns past the last one with cells are empty.
            if (size_t(x) + 1 >= columns.size())
                return;
            from = begin + columns[size_t(x)];
            to = begin + columns[size_t(x) + 1];
        }
        for (auto it = std::lower_bound(from, to, first, &CLevelGraph::vertex::predicate2);
             it != to && it->position().xz() <= last; ++it)
        {
            const float z = origin.z + float(it->position().xz() - base) * cell;
            if (gap_x + gap_sqr(z, pos.z) > distance_sqr)
                continue;
            const u32 id = graph.vertex(it);
            // A tie keeps the lowest id, as the stock search does.
            if (const float d = graph.distance(id, pos); d < distance_sqr || (d == distance_sqr && id < result))
            {
                distance_sqr = d;
                result = id;
            }
        }
    };
    for (int step = 0; column + step <= x1 || column - step >= x0; ++step)
    {
        const int right = column + step, left = column - step;
        const float gap_right = gap_sqr(origin.x + float(right) * cell, pos.x);
        const float gap_left = gap_sqr(origin.x + float(left) * cell, pos.x);
        // Columns farther out only get farther.
        if (_min(gap_right, gap_left) > distance_sqr)
            break;
        if (right >= x0 && right <= x1 && gap_right <= distance_sqr)
            scan_column(right, gap_right);
        if (step && left >= x0 && left <= x1 && gap_left <= distance_sqr)
            scan_column(left, gap_left);
    }
    return result;
}
} // namespace

KnowState* create(CAI_Stalker& npc)
{
    auto* state = xr_new<KnowState>();
    state->beliefs.reserve(cfg().beliefs_max);
    apply_staged(npc, *state);
    return state;
}

SaveParams save_params()
{
    const Config& config = cfg();
    return {ms(config.forget_enemy_s), ms(config.forget_other_s), config.beliefs_max, ms(config.restore_grace_s),
        config.age_growth, config.age_cap};
}

void destroy(KnowState* state)
{
    if (!state)
        return;
    xr_delete(state->scratch);
    xr_delete(state);
}

void update(CAI_Stalker& npc, float dt)
{
    KnowState* state = state_of(npc);
    if (!state)
        return;

    const int community = community_of(npc);
    ScopedTimer timer(Stat::p_know_us, community);
    expire_staged();
    const u32 time = now();
    const Config& config = cfg();
    // Ageing is coarse (a belief is dropped after a minute and more, relations are re-read every second, the end of the
    // extrapolation window only freezes what believed_position() already clamps): it runs at [ai_errors]
    // maintain_ms, slower far from the player. Readers resolve a belief's target themselves; a late drop is memory.
    if (time < state->maintain_time)
        return;
    const NpcState* self = npc.da_ai();
    state->maintain_time = time + config.maintain_ms[self && self->distant ? 1 : 0];
    // Relations change rarely: every flag is refreshed at this pace, and each new observation refreshes its own.
    const bool relations = time >= state->relations_time;
    if (relations)
        state->relations_time = time + config.relation_refresh_ms;
    for (size_t index = state->beliefs.size(); index-- > 0;)
    {
        Belief& belief = state->beliefs[index];
        const auto* target = smart_cast<const CEntityAlive*>(resolve(belief.e.target));
        // The target of a restored belief may come online a little after its observer.
        if (!target && belief.e.source == Source::restored && time < belief.t_received + ms(config.restore_grace_s))
            continue;
        bool drop = !target || target->H_Parent();
        if (!drop && relations)
            belief.enemy = target->g_Alive() && npc.is_relation_enemy(target);
        if (!drop && !sees_now(npc, target))
        {
            const float age = float(time - _min(time, belief.e.t_observed)) / 1000.f;
            drop = age > (belief.enemy ? config.forget_enemy_s : config.forget_other_s);

            // The extrapolation window is over: the belief stays where the lost target was predicted to be.
            if (!drop && belief.e.source == Source::seen && !belief.vel.similar(Fvector().set(0.f, 0.f, 0.f)) &&
                time >= belief.t_seen_own + extrap_ms(npc))
            {
                const Fvector position = believed_position(npc, belief);
                belief.e.vertex = believed_vertex(npc, belief, position);
                belief.e.pos = position;
                belief.vel = {};
            }
        }

        if (drop)
        {
            state->beliefs.erase(state->beliefs.begin() + index);
            stat_add(Stat::k_forgot, 1.f, community);
        }
    }
}

void on_death(CAI_Stalker& npc, IGameObject* who) {}

void level_unload()
{
    g_cfg.loaded = false;
    g_columns = {};
}

void level_prepare()
{
    // A walk over every vertex (~1.4 ms on Cordon): at the level's first spawn, not inside an NPC's update (r4a9).
    if (ai().get_level_graph())
        column_index(ai().level_graph());
}

bool active(const CAI_Stalker& npc) { return !!active_state(npc); }

u32 hit_time(const CAI_Stalker& npc, const CEntityAlive* enemy)
{
    if (!enemy)
        return 0;
    const CHitMemoryManager& hits = npc.memory().hit();
    if (active_state(npc))
        return hits.last_hit_object_id() == enemy->ID() ? hits.last_hit_time() : 0;
    const MemorySpace::CHitObject* hit = hits.hit(enemy);
    return hit ? hit->m_level_time : 0;
}

void on_own_shot(CAI_Stalker& npc)
{
    if (!active_state(npc))
        return;
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    if (!enemy || sees_now(npc, enemy))
        return;
    if (Tactics::fire_mode(npc, enemy) == Tactics::FireMode::suppress)
        return;
    stat_add(Stat::k_blind_shot, 1.f, community_of(npc));
}

void observe(CAI_Stalker& npc, const CGameObject* target, const Estimate& estimate)
{
    KnowState* state = state_of(npc);
    if (!state || !target || target->ID() == npc.ID())
        return;
    const auto* entity = smart_cast<const CEntityAlive*>(target);
    if (!entity)
        return;

    // "seen" belongs to the own-vision path and is exact; a glimpse with an error is a suspicion (suspect).
    Estimate value = estimate;
    if (!sanitize(value) || (value.source == Source::seen && value.err > 0.f))
        return;
    value.target = target->ID();
    if (value.source != Source::seen && ai().get_level_graph() &&
        (!ai().level_graph().valid_vertex_id(value.vertex) || !ai().level_graph().inside(value.vertex, value.pos)))
    {
        value.vertex = resolve_vertex(value.pos, &npc.Position());
    }
    store(npc, *state, *entity, value);
}

void suspect(CAI_Stalker& npc, const Estimate& estimate, u32 danger_type, u32 perceive_type)
{
    if (!npc.g_Alive() || !_valid(estimate.pos))
        return;
    if (danger_type > u32(CDangerObject::eDangerTypeEnemySound) ||
        perceive_type > u32(CDangerObject::eDangerPerceiveTypeHit))
    {
        return;
    }

    const CEntityAlive* object = smart_cast<const CEntityAlive*>(resolve(estimate.target));
    if (object && object->ID() == npc.ID())
        return;
    const u32 time = estimate.t_observed && estimate.t_observed <= now() ? estimate.t_observed : now();
    npc.memory().danger().add(CDangerObject(object, estimate.pos, time, CDangerObject::EDangerType(danger_type),
        CDangerObject::EDangerPerceiveType(perceive_type)));
}

const Belief* belief(const CAI_Stalker& npc, u16 id)
{
    const KnowState* state = state_of(npc);
    return state ? find(*state, id) : nullptr;
}

bool sees_now(const CAI_Stalker& npc, const CGameObject* object)
{
    return object && npc.memory().visual().visible_now(object);
}

bool knows(const CAI_Stalker& npc, const CGameObject* object)
{
    if (!object)
        return false;
    const KnowState* state = active_state(npc);
    if (!state)
        return sees_now(npc, object) || npc.memory().memory_time(object) != 0;
    // Things and corpses nobody believes in are world state; a live creature is known by sight or belief.
    const auto* entity = smart_cast<const CEntityAlive*>(object);
    if (sees_now(npc, object) || find(*state, object->ID()))
        return true;
    return !entity || !entity->g_Alive();
}

Fvector known_position(const CAI_Stalker& npc, const CGameObject& object)
{
    const KnowState* state = active_state(npc);
    const auto* entity = smart_cast<const CEntityAlive*>(&object);
    if (!state || !entity)
        return object.Position();
    if (sees_now(npc, &object))
        return memory_convention(object);
    if (const Belief* belief = find(*state, object.ID()))
        return believed_position(npc, *belief);
    return entity->g_Alive() ? npc.Position() : object.Position();
}

float known_distance(const CAI_Stalker& npc, const CGameObject& object)
{
    if (!active_state(npc))
        return npc.Position().distance_to(object.Position());
    if (!knows(npc, &object))
        return -1.f;
    return npc.Position().distance_to(known_position(npc, object));
}

Fvector known_direction(const CAI_Stalker& npc, const CGameObject& object)
{
    if (!active_state(npc))
        return Fvector().sub(object.Position(), npc.Position()).normalize_safe();
    if (!knows(npc, &object))
        return Fvector().set(0.f, 0.f, 0.f);
    return Fvector().sub(known_position(npc, object), npc.Position()).normalize_safe();
}

u32 last_seen_time(const CAI_Stalker& npc, const CGameObject* object)
{
    return npc.memory().visual().visible_object_time_last_seen(object);
}

bool seen_own_within(const CAI_Stalker& npc, const CGameObject& object, u32 ms)
{
    if (npc.memory().visual().visible_now(&object))
        return true;
    if (const KnowState* state = active_state(npc))
    {
        const Belief* known = find(*state, object.ID());
        return known && known->t_seen_own && now() < known->t_seen_own + ms;
    }
    const u32 seen = last_seen_time(npc, &object);
    return seen != u32(-1) && Device.dwTimeGlobal < seen + ms;
}

Estimate report_of(const CAI_Stalker& sender, u16 id)
{
    Estimate result{};
    result.target = id;
    result.reporter = sender.ID();
    const KnowState* state = state_of(sender);
    if (!state)
        return result;

    // What the sender sees right now is reported exactly; anything else is its belief as it stands (Estimate::err
    // is the error at t_observed, the age growth is added by whoever reads it).
    if (const CGameObject* object = resolve(id); object && sees_now(sender, object))
    {
        result.pos = memory_convention(*object);
        result.err = 0.f;
        result.t_observed = now();
        result.source = Source::seen;
        result.confidence = 1.f;
        result.vertex = object->ai_location().level_vertex_id();
        return result;
    }

    const Belief* belief = find(*state, id);
    if (!belief)
        return result;
    result = belief->e;
    result.pos = believed_position(sender, *belief);
    result.vertex = believed_vertex(sender, *belief, result.pos);
    result.reporter = sender.ID();
    return result;
}

void rumour(CAI_Stalker& npc, const CEntityAlive* object, float err, Source source)
{
    if (!object || object->getDestroy())
        return;
    if (!active_state(npc))
    {
        npc.memory().make_object_visible_somewhen(object);
        return;
    }
    if (object->ID() == npc.ID())
        return;

    // A snapshot of the truth with the rumour's error, never a sighting.
    const Source kind = source == Source::seen || source == Source::none ? Source::script : source;
    const float radius = err >= 0.f ? err : cfg().script;
    observe(npc, object, realise(npc, object, memory_convention(*object), kind, radius));
}

bool enemy_sees_me(const CAI_Stalker& npc, const CEntityAlive& enemy)
{
    KnowState* state = active_state(npc);
    if (!state)
    {
        const CVisualMemoryManager* visual = enemy.visual_memory();
        return visual && visual->visible_now(&npc);
    }
    if (!enemy.g_Alive())
        return false;
    if (!sees_now(npc, &enemy))
        return cfg().see_me_unknown;

    Fvector eye, direction;
    if (!view_of(enemy, eye, direction))
        return cfg().see_me_unknown;
    const Fvector head = npc.eye_matrix.c;
    Fvector to_me = Fvector().sub(head, eye);
    const float distance = to_me.magnitude();
    if (distance < EPS_L)
        return true;
    to_me.div(distance);
    direction.normalize();
    if (to_me.dotproduct(direction) < _cos(deg2rad(cfg().see_me_cone_deg)))
        return false;
    return see_me_line(*state, enemy, eye, head);
}

void forget(CAI_Stalker& npc, u16 id)
{
    KnowState* state = state_of(npc);
    if (!state)
        return;
    const auto found = std::find_if(
        state->beliefs.begin(), state->beliefs.end(), [id](const Belief& belief) { return belief.e.target == id; });
    if (found == state->beliefs.end())
        return;
    state->beliefs.erase(found);
    stat_add(Stat::k_forgot, 1.f, community_of(npc));
}

void inform(CAI_Stalker& npc, const CGameObject* target, const Fvector& position, float err, Source source)
{
    if (!target || !_valid(position) || !_valid(err))
        return;
    // A script tells, it never lends its eyes: a "seen" report is a rumour.
    Estimate estimate{};
    estimate.pos = position;
    estimate.err = _max(err, 0.f);
    estimate.t_observed = now();
    estimate.reporter = npc.ID();
    estimate.source = source == Source::seen || source == Source::none ? Source::script : source;
    estimate.confidence = 1.f;
    observe(npc, target, estimate);
}

float source_error(Source source, float distance)
{
    const Config& config = cfg();
    distance = _max(distance, 0.f);
    switch (source)
    {
    case Source::seen:
    case Source::none:
    case Source::restored: return 0.f;
    case Source::hit:
        return distance *
            _max(config.hit_distance_err, _sin(deg2rad(config.hit_cone_deg[0] + config.hit_cone_deg[1] * .5f)));
    case Source::near_miss:
        return distance * _sin(deg2rad(config.near_miss_cone_deg)) +
            (config.near_miss_distance_prior[1] - config.near_miss_distance_prior[0]) * .5f;
    case Source::witness:
        return distance * _sin(deg2rad(config.witness_cone_deg)) +
            (config.witness_distance_prior[1] - config.witness_distance_prior[0]) * .5f;
    case Source::script: return config.script;
    default: break;
    }
    if (source >= Source::report_voice && source <= Source::report_hq)
        return config.describe_k[size_t(source) - size_t(Source::report_voice)] * distance;
    const Linear& linear = config.linear[size_t(source)];
    return linear.e0 + linear.k * distance;
}

float effective_error(const Estimate& estimate) { return effective_error_at(estimate, now()); }

float report_grid(Source channel)
{
    if (channel < Source::report_voice || channel > Source::report_hq)
        return 0.f;
    return cfg().describe_grid[size_t(channel) - size_t(Source::report_voice)];
}

Estimate realise(const CAI_Stalker& observer, const CGameObject* target, const Fvector& truth, Source source,
    float err, float clarity, const Fvector* anchor, bool resolve)
{
    const Config& config = cfg();
    clamp(clarity, 0.f, 1.f);
    Estimate result{};
    result.err = _max(err, 0.f) * (1.f + config.clarity_k * (1.f - clarity));
    result.t_observed = now();
    result.target = target ? target->ID() : u16(-1);
    result.reporter = observer.ID();
    result.source = source;
    result.confidence = clarity;
    result.pos = truth;
    if (result.err > 0.f)
    {
        const float radius = result.err * config.realise * _sqrt(::Random.randF(0.f, 1.f));
        const float angle = ::Random.randF(0.f, PI_MUL_2);
        result.pos.x += radius * _cos(angle);
        result.pos.z += radius * _sin(angle);
        if (resolve)
            result.vertex = resolve_vertex(result.pos, anchor ? anchor : &observer.Position());
    }
    else if (resolve)
        result.vertex = find_vertex(truth, anchor ? anchor : &observer.Position());
    return result;
}

u32 find_vertex(const Fvector& pos, const Fvector* anchor)
{
    if (!ai().get_level_graph() || !_valid(pos))
        return u32(-1);
    const CLevelGraph& graph = ai().level_graph();
    if (graph.valid_vertex_position(pos))
    {
        if (const u32 vertex = graph.vertex_id(pos); graph.valid_vertex_id(vertex))
            return vertex;
    }

    // Growing windows of cells around the spot; the stock search walks every column out to the nearest vertex, which
    // cost a heard shot landing in a building ~0.1-1 ms. A window's winner is the global nearest once it lies within
    // the window's radius: every vertex outside is farther than that.
    const float cell = graph.header().cell_size();
    const int limit = cell > EPS_L ? iCeil(cfg().vertex_search_m / cell) : 0;
    for (int radius = _min(4, limit); radius > 0; radius = radius < limit ? _min(radius * 2, limit) : 0)
    {
        float distance_sqr;
        if (const u32 vertex = nearest_in_window(graph, pos, radius, distance_sqr);
            graph.valid_vertex_id(vertex) && distance_sqr <= _sqr(float(radius) * cell))
        {
            return vertex;
        }
    }
    // Farther off the graph (a guessed spot outside the level, over water, inside a large building): windows from
    // the spot back toward the anchor, the observer on the graph, and the first cell found. The stock search below
    // expands over every column until it passes the distance off the graph: milliseconds for a spot far outside.
    if (anchor && _valid(*anchor) && limit > 0)
    {
        const float span = pos.distance_to_xz(*anchor);
        const int steps = clampr(iCeil(span / (float(limit) * cell)), 1, int(_max(cfg().vertex_march_steps, 1u)));
        for (int step = 1; step <= steps; ++step)
        {
            const Fvector point = Fvector().lerp(pos, *anchor, float(step) / float(steps));
            float distance_sqr;
            if (const u32 vertex = nearest_in_window(graph, point, limit, distance_sqr); graph.valid_vertex_id(vertex))
                return vertex;
        }
    }
    // The stock whole-graph scan: consumers of memory entries expect a valid vertex.
    return graph.vertex(u32(-1), pos);
}

u32 resolve_vertex(Fvector& pos, const Fvector* anchor)
{
    const u32 vertex = find_vertex(pos, anchor);
    if (vertex == u32(-1) || !ai().level_graph().valid_vertex_id(vertex))
        return u32(-1);
    const CLevelGraph& graph = ai().level_graph();
    if (graph.inside(vertex, pos))
        pos.y = graph.vertex_plane_y(vertex, pos.x, pos.z);
    else
        pos = graph.vertex_position(vertex);
    pos.y += cfg().center_height;
    return vertex;
}

namespace
{
// A live creature this NPC's own vision did not confirm at its last visual update (within the still-visible time), or
// confirmed longer than sight_hold_ms ago. The bit is judged only at that update (hook_make_invisible); a read before
// the next one must not see a bit another member set since (W2). DA has no still_visible_time (0), so the update, not
// now, is the reference; the hold bounds a stale bit (r4a4: see true for 5.6 s without an own sighting) by the
// staleness P2 allows. Corpses keep the stock rule: the dead-vision rest refreshes them without a sighting.
bool unconfirmed(const CAI_Stalker& npc, const CGameObject* object, u32 last_update)
{
    const KnowState* state = active_state(npc);
    if (!state || !object)
        return false;
    const auto seen = state->own_seen.find(object->ID());
    const u32 still = npc.memory().visual().current_state().m_still_visible_time;
    if (seen != state->own_seen.end() && seen->second + still >= last_update &&
        seen->second + still + cfg().sight_hold_ms >= now())
    {
        return false;
    }
    const auto* entity = smart_cast<const CEntityAlive*>(object);
    return entity && entity->g_Alive();
}
} // namespace

bool hook_visible_now(const CAI_Stalker& npc, const CGameObject* object, u32 last_update, bool& result)
{
    if (!unconfirmed(npc, object, last_update))
        return false;
    result = false;
    return true;
}

bool hook_visible_right_now(const CAI_Stalker& npc, const CGameObject* object, u32 last_update, bool& result)
{
    return hook_visible_now(npc, object, last_update, result);
}

bool hook_time_last_seen(const CAI_Stalker& npc, const IGameObject* object, u32& result)
{
    const KnowState* state = active_state(npc);
    const auto* entity = smart_cast<const CEntityAlive*>(object);
    if (!state || !entity)
        return false;

    if (const Belief* belief = find(*state, entity->ID()); belief && belief->t_seen_own)
        result = belief->t_seen_own;
    else if (const auto seen = state->own_seen.find(entity->ID()); seen != state->own_seen.end())
        result = seen->second;
    else if (!entity->g_Alive())
        return false;
    else
        result = u32(-1);
    return true;
}

bool hook_make_invisible(const CAI_Stalker& npc, MemorySpace::CVisibleObject& object, u64 mask)
{
    const KnowState* state = active_state(npc);
    if (!state)
        return false;
    if (!object.visible(mask))
        return true;

    // Own sight only: a mate refreshing the shared entry no longer keeps this NPC "seeing" the target.
    const auto seen = state->own_seen.find(object_id(object.m_object));
    const u32 still = npc.memory().visual().current_state().m_still_visible_time;
    if (seen == state->own_seen.end() || seen->second + still < now())
        object.visible(mask, false);
    return true;
}

void on_add_visible(CAI_Stalker& npc, const CGameObject& object, bool fictitious)
{
    KnowState* state = state_of(npc);
    if (!state || fictitious)
        return;

    const u32 time = now();
    if (state->own_seen_stamp != time)
    {
        state->own_seen_stamp = time;
        constexpr u32 keep_ms = 10000;
        phmap::erase_if(state->own_seen, [time](const auto& entry) { return entry.second + keep_ms < time; });
    }
    state->own_seen[object.ID()] = time;

    const auto* entity = smart_cast<const CEntityAlive*>(&object);
    if (!entity || object.ID() == npc.ID())
        return;

    if (!entity->g_Alive())
    {
        // A corpse the NPC sees itself is its only fresh-corpse danger (C28).
        if (active_state(npc) && entity->killer_id() != ALife::_OBJECT_ID(-1))
        {
            npc.memory().danger().add(CDangerObject(entity, entity->Position(), time,
                CDangerObject::eDangerTypeFreshEntityCorpse, CDangerObject::eDangerPerceiveTypeVisual));
        }
        return;
    }

    Estimate estimate{};
    estimate.pos = memory_convention(object);
    estimate.t_observed = time;
    estimate.target = object.ID();
    estimate.reporter = npc.ID();
    estimate.source = Source::seen;
    estimate.confidence = 1.f;
    estimate.vertex = object.ai_location().level_vertex_id();
    store(npc, *state, *entity, estimate);
}

bool memory_info(const CAI_Stalker& npc, const IGameObject* object, MemorySpace::CMemoryInfo& result)
{
    const KnowState* state = active_state(npc);
    const auto* entity = smart_cast<const CEntityAlive*>(object);
    if (!state || !entity)
        return false;

    const bool seen = sees_now(npc, entity);
    const Belief* belief = find(*state, entity->ID());
    // Without a belief: an item-like corpse and a creature in own sight keep the stock entry, the rest is unknown.
    if (!belief)
        return entity->g_Alive() && !seen;
    fill_info(npc, *belief, *entity, seen, result);
    return true;
}

bool memory_time(const CAI_Stalker& npc, const IGameObject* object, u32& result)
{
    const KnowState* state = active_state(npc);
    const auto* entity = smart_cast<const CEntityAlive*>(object);
    if (!state || !entity)
        return false;

    const bool seen = sees_now(npc, entity);
    if (const Belief* belief = find(*state, entity->ID()))
    {
        result = seen ? now() : belief->e.t_observed;
        return true;
    }
    if (!entity->g_Alive() || seen)
        return false;
    result = 0;
    return true;
}

bool memory_position(const CAI_Stalker& npc, const IGameObject* object, Fvector& result)
{
    const KnowState* state = active_state(npc);
    const auto* entity = smart_cast<const CEntityAlive*>(object);
    if (!state || !entity)
        return false;

    const bool seen = sees_now(npc, entity);
    if (const Belief* belief = find(*state, entity->ID()))
    {
        result = seen ? memory_convention(*entity) : believed_position(npc, *belief);
        return true;
    }
    if (!entity->g_Alive() || seen)
        return false;
    result.set(0.f, 0.f, 0.f);
    return true;
}

void add_candidates(CAI_Stalker& npc, bool in_combat)
{
    const KnowState* state = active_state(npc);
    if (!state)
        return;

    const int community = community_of(npc);
    ScopedTimer timer(Stat::p_know_us, community);
    // The candidates' part of p_know_us (the enemy manager's checks and the script's enemy callback).
    ScopedTimer candidates_timer(Stat::p_know_cand_us, community);
    // Ids first: the enemy callback runs Lua, which may reach this NPC's beliefs through a rumour.
    u16 ids[64];
    u32 count = 0;
    for (const Belief& belief : state->beliefs)
    {
        // Enemies only: the enemy manager turns everybody else away by the same relation test. Out of combat a
        // heard-only enemy is a danger to investigate; one the NPC has seen itself stays a candidate whatever its
        // newest estimate is, as the stock visual memory kept him (r4a5: the merc pair's belief turned into a heard
        // one, and once out of combat they never came back to fight or search).
        const bool candidate = in_combat || visual_class(belief.e.source) || belief.t_seen_own;
        if (belief.enemy && candidate && count < std::size(ids))
            ids[count++] = belief.e.target;
    }

    for (u32 index = 0; index < count; ++index)
    {
        if (const CEntityAlive* entity = resolve_alive(ids[index]); entity && npc.memory().enemy().add(entity))
            stat_add(Stat::k_candidates, 1.f, community);
    }
}

void known_objects(const CAI_Stalker& npc, xr_vector<const CEntityAlive*>& result)
{
    result.clear();
    const KnowState* state = active_state(npc);
    if (!state)
        return;
    for (const Belief& belief : state->beliefs)
    {
        if (const CEntityAlive* entity = resolve_alive(belief.e.target))
            result.push_back(entity);
    }
}

namespace
{
template <typename T>
void observe_entry(CAI_Stalker& npc, const MemorySpace::CMemoryObject<T>& entry, Source source)
{
    const auto* entity = smart_cast<const CEntityAlive*>(entry.m_object);
    if (!entity || !entity->g_Alive() || entity->ID() == npc.ID())
        return;
    Estimate estimate{};
    estimate.pos = entry.m_object_params.m_position;
    estimate.t_observed = entry.m_level_time;
    estimate.reporter = npc.ID();
    estimate.source = source;
    estimate.confidence = 1.f;
    estimate.vertex = entry.m_object_params.m_level_vertex_id;
    observe(npc, entity, estimate);
}
} // namespace

void observe_pooled(CAI_Stalker& npc, const MemorySpace::CSoundObject& entry)
{
    observe_entry(npc, entry,
        (entry.m_sound_type & SOUND_TYPE_WEAPON) == SOUND_TYPE_WEAPON ? Source::heard_shot : Source::heard_item);
}

void observe_pooled(CAI_Stalker& npc, const MemorySpace::CHitObject& entry) { observe_entry(npc, entry, Source::hit); }

bool aim_point(const CAI_Stalker& npc, const CGameObject& target, Fvector& result)
{
    KnowState* state = active_state(npc);
    const auto* entity = smart_cast<const CEntityAlive*>(&target);
    if (!state || !entity || !entity->g_Alive())
        return false;

    if (!sees_now(npc, &target))
    {
        result = unseen_point(npc, *state, target);
        return true;
    }
    if (state->aim_fallback_target == target.ID())
        state->aim_fallback_target = u16(-1);

    if (npc.aim_bone_id().size())
        return false;
    if (!smart_cast<const CActor*>(&target) && !smart_cast<const CAI_Stalker*>(&target))
        return false;

    roll_aim(npc, *state, target.ID());
    return bone_point(target, state->aim_head ? cfg().bone_head : cfg().bone_body, result);
}

bool look_point(const CAI_Stalker& npc, const CGameObject& target, Fvector& result)
{
    KnowState* state = active_state(npc);
    const auto* entity = smart_cast<const CEntityAlive*>(&target);
    if (!state || !entity || !entity->g_Alive() || sees_now(npc, &target))
        return false;
    result = unseen_point(npc, *state, target);
    return true;
}

float lead_factor(const CAI_Stalker& npc, const CGameObject& target)
{
    KnowState* state = active_state(npc);
    if (!state)
        return 1.f;
    roll_aim(npc, *state, target.ID());
    return state->aim_lead;
}

bool may_finish_unseen(const CAI_Stalker& npc, const CEntityAlive& victim, float reach)
{
    const KnowState* state = active_state(npc);
    if (!state)
        return true;
    const Belief* belief = find(*state, victim.ID());
    if (!belief || !belief->t_seen_own || now() > belief->t_seen_own + ms(cfg().kill_wounded_seen_s))
        return false;
    return npc.Position().distance_to(believed_position(npc, *belief)) <= reach;
}

bool cached_enemy_callback(const CAI_Stalker& npc, u16 id, bool& result)
{
    const KnowState* state = active_state(npc);
    if (!state)
        return false;
    const u32 time = now();
    for (const KnowState::Callback& entry : state->callbacks)
    {
        if (entry.id == id)
        {
            if (time >= entry.until)
                return false;
            result = entry.useful;
            return true;
        }
    }
    return false;
}

void store_enemy_callback(const CAI_Stalker& npc, u16 id, bool result)
{
    KnowState* state = active_state(npc);
    if (!state)
        return;
    const NpcState* self = npc.da_ai();
    stat_add(Stat::k_callback, 1.f, self ? self->community : -1);
    // The script's answer about a target changes on the scale of seconds (a hit clears it at once): a Lua call per
    // candidate and period, so NPCs far from the player ask it less often.
    const u32 period = cfg().enemy_callback_ms[self && self->distant ? 1 : 0];
    if (!period)
        return;

    constexpr size_t slots = 32;
    const u32 time = now();
    auto& entries = state->callbacks;
    auto slot = std::find_if(entries.begin(), entries.end(), [id](const auto& entry) { return entry.id == id; });
    if (slot == entries.end())
        slot = std::find_if(entries.begin(), entries.end(), [time](const auto& entry) { return entry.until <= time; });
    if (slot == entries.end())
    {
        if (entries.size() < slots)
            slot = entries.emplace(entries.end());
        else
            slot = std::min_element(
                entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.until < b.until; });
    }
    // Spread over 75-125 % of the period (a hash, not the shared generator), so the targets of one NPC do not all
    // call the script again in the same update.
    const u32 spread = (u32(id) * 2654435761u ^ u32(npc.ID()) * 40503u ^ time) % (period / 2 + 1);
    slot->until = time + period * 3 / 4 + spread;
    slot->id = id;
    slot->useful = result;
}

void forget_enemy_callback(const CAI_Stalker& npc, u16 id)
{
    if (KnowState* state = active_state(npc))
    {
        for (KnowState::Callback& entry : state->callbacks)
        {
            if (entry.id == id)
                entry.until = 0;
        }
    }
}

const xr_vector<MemorySpace::CVisibleObject>* script_visible_objects(CCustomMonster& monster)
{
    auto* npc = smart_cast<CAI_Stalker*>(&monster);
    KnowState* state = npc ? active_state(*npc) : nullptr;
    if (!state || !npc->g_Alive())
        return nullptr;

    auto& result = scratch_of(*state).visible;
    result.clear();
    const squad_mask_type mask = npc->memory().visual().mask();
    for (const MemorySpace::CVisibleObject& entry : npc->memory().visual().objects())
    {
        if (!entry.m_object || !entry.m_squad_mask.test(mask))
            continue;
        const auto* entity = smart_cast<const CEntityAlive*>(entry.m_object);
        if (!entity || !entity->g_Alive())
        {
            result.push_back(entry);
            continue;
        }

        // A live creature is listed only when this NPC has seen it itself, at what it believes.
        const bool seen = entry.visible(mask);
        const Belief* belief = find(*state, entity->ID());
        if (!seen && (!belief || !belief->t_seen_own))
            continue;
        MemorySpace::CVisibleObject& copy = result.emplace_back(entry);
        if (!seen)
        {
            copy.m_object_params.m_position = believed_position(*npc, *belief);
            copy.m_object_params.m_level_vertex_id = believed_vertex(*npc, *belief, copy.m_object_params.m_position);
            copy.m_level_time = belief->t_seen_own;
        }
    }
    return &result;
}

const xr_vector<MemorySpace::CSoundObject>* script_sound_objects(CCustomMonster& monster)
{
    auto* npc = smart_cast<CAI_Stalker*>(&monster);
    KnowState* state = npc ? active_state(*npc) : nullptr;
    if (!state || !npc->g_Alive())
        return nullptr;

    auto& result = scratch_of(*state).sound;
    result.clear();
    const squad_mask_type mask = npc->memory().visual().mask();
    for (const MemorySpace::CSoundObject& entry : npc->memory().sound().objects())
    {
        if (!entry.m_squad_mask.test(mask))
            continue;
        const auto* entity = smart_cast<const CEntityAlive*>(entry.m_object);
        if (!entity || !entity->g_Alive())
        {
            result.push_back(entry);
            continue;
        }
        const Belief* belief = find(*state, entity->ID());
        if (!belief)
            continue;
        MemorySpace::CSoundObject& copy = result.emplace_back(entry);
        copy.m_object_params.m_position = believed_position(*npc, *belief);
        copy.m_object_params.m_level_vertex_id = believed_vertex(*npc, *belief, copy.m_object_params.m_position);
        copy.m_level_time = belief->e.t_observed;
    }
    return &result;
}

const xr_vector<MemorySpace::CHitObject>* script_hit_objects(CCustomMonster& monster)
{
    auto* npc = smart_cast<CAI_Stalker*>(&monster);
    KnowState* state = npc ? active_state(*npc) : nullptr;
    if (!state || !npc->g_Alive())
        return nullptr;

    auto& result = scratch_of(*state).hit;
    result.clear();
    const squad_mask_type mask = npc->memory().visual().mask();
    for (const MemorySpace::CHitObject& entry : npc->memory().hit().objects())
    {
        if (!entry.m_object || !entry.m_squad_mask.test(mask))
            continue;
        const Belief* belief = find(*state, entry.m_object->ID());
        if (!belief)
            continue;
        MemorySpace::CHitObject& copy = result.emplace_back(entry);
        copy.m_object_params.m_position = believed_position(*npc, *belief);
        copy.m_object_params.m_level_vertex_id = believed_vertex(*npc, *belief, copy.m_object_params.m_position);
        copy.m_level_time = belief->e.t_observed;
    }
    return &result;
}

// The not-yet-visible list is the NPC's own detection progress (not a shared pool): the stock list is honest.
const xr_vector<MemorySpace::CNotYetVisibleObject>* script_not_yet_visible_objects(CCustomMonster& monster)
{
    return nullptr;
}
} // namespace Know
} // namespace DaAi
