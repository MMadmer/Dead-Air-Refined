// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"
#include "da_ai_knowledge.h"
#include "da_ai_senses.h"
#include "da_ai_comms.h"
#include "da_ai_tactics.h"
#include "da_ai_morale.h"
#include "da_ai_search.h"

#include "ai/stalker/ai_stalker.h"
#include "character_community.h"
#include "Hit.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include <array>

namespace DaAi
{
u32 g_features = (1u << u32(Feature::count)) - 1;
int g_trace{};
Config g_config;

namespace
{
constexpr pcstr feature_names[]{"knowledge", "awareness", "concealment", "full_los", "hearing", "noise", "light",
    "near_miss", "comms", "radio", "doctrine", "cover_score", "roles", "suppression", "grenades", "reaction", "settle",
    "morale", "search", "scripts"};
static_assert(std::size(feature_names) == size_t(Feature::count));

struct StatInfo
{
    pcstr name;
    StatKind kind;
};

#define DA_AI_STAT_INFO(name, kind) {#name, StatKind::kind},
constexpr StatInfo stat_infos[]{DA_AI_STATS(DA_AI_STAT_INFO)};
#undef DA_AI_STAT_INFO
static_assert(std::size(stat_infos) == size_t(Stat::count));

struct Accum
{
    double sum{};
    u32 samples{};
    float min{flt_max};
    float max{};
};

using StatTable = std::array<Accum, size_t(Stat::count)>;

StatTable g_global;
xr_flat_hash_map<int, StatTable> g_by_community;
u32 g_period_start{};
u32 g_period_frame{};
int g_log_period_ms{};
u32 g_last_log{};

u32 g_ray_frame{u32(-1)};
u32 g_rays_used{};
u32 g_rays_granted{};

float gauge_value(Stat stat)
{
    switch (stat)
    {
    case Stat::npcs: return float(npc_count());
    case Stat::squads: return float(squad_count());
    default: return 0.f;
    }
}

float period_seconds() { return _max(.001f, float(Device.dwTimeGlobal - g_period_start) / 1000.f); }

bool has_value(const Accum& accum, Stat stat)
{
    return stat_infos[size_t(stat)].kind == StatKind::Gauge ? gauge_value(stat) > 0.f : accum.samples > 0;
}

float value_of(const Accum& accum, Stat stat)
{
    switch (stat_infos[size_t(stat)].kind)
    {
    case StatKind::Count:
    case StatKind::Sum: return float(accum.sum);
    case StatKind::Avg: return accum.samples ? float(accum.sum / accum.samples) : 0.f;
    case StatKind::Min: return accum.samples ? accum.min : 0.f;
    case StatKind::Max: return accum.max;
    case StatKind::Rate: return float(accum.sum / period_seconds());
    case StatKind::Gauge: return gauge_value(stat);
    }
    return 0.f;
}

void append_value(xr_string& line, const Accum& accum, Stat stat)
{
    string64 text;
    const float value = value_of(accum, stat);
    switch (stat_infos[size_t(stat)].kind)
    {
    case StatKind::Count:
    case StatKind::Min:
    case StatKind::Max:
    case StatKind::Gauge: xr_sprintf(text, " %s=%u", stat_infos[size_t(stat)].name, u32(value + .5f)); break;
    default: xr_sprintf(text, " %s=%.1f", stat_infos[size_t(stat)].name, value); break;
    }
    line += text;
}

// One table as "[ai] <prefix> key=value ..." lines, split so that no line outgrows the log buffer. The log file keeps
// at most 1023 characters of a line and then drops its line break (IWriter::VPrintf): a longer line lost its tail
// and ran into the next one (K11: most global lines of an R2 run).
void print_table(pcstr prefix, const StatTable& table)
{
    constexpr size_t max_line = 900;
    xr_string line;
    for (size_t index = 0; index < size_t(Stat::count); ++index)
    {
        const Stat stat = Stat(index);
        if (!has_value(table[index], stat))
            continue;
        append_value(line, table[index], stat);
        if (line.size() > max_line)
        {
            Msg("[ai] %s%s", prefix, line.c_str());
            line.clear();
        }
    }
    if (!line.empty())
        Msg("[ai] %s%s", prefix, line.c_str());
}

void add_to(Accum& accum, float value)
{
    accum.sum += value;
    ++accum.samples;
    accum.min = _min(accum.min, value);
    accum.max = _max(accum.max, value);
}
} // namespace

pcstr feature_name(Feature which) { return which < Feature::count ? feature_names[size_t(which)] : ""; }

bool feature_by_name(pcstr name, Feature& result)
{
    if (!name)
        return false;
    for (size_t index = 0; index < size_t(Feature::count); ++index)
    {
        if (!xr_strcmp(feature_names[index], name))
        {
            result = Feature(index);
            return true;
        }
    }
    return false;
}

void stat_add(Stat stat, float value, int community)
{
    if (stat >= Stat::count)
        return;
    add_to(g_global[size_t(stat)], value);
    if (community >= 0)
        add_to(g_by_community[community][size_t(stat)], value);
}

float stat_value(Stat stat) { return stat < Stat::count ? value_of(g_global[size_t(stat)], stat) : 0.f; }

bool stat_by_name(pcstr name, Stat& result)
{
    if (!name)
        return false;
    for (size_t index = 0; index < size_t(Stat::count); ++index)
    {
        if (!xr_strcmp(stat_infos[index].name, name))
        {
            result = Stat(index);
            return true;
        }
    }
    return false;
}

void stats_print_and_reset()
{
    string128 prefix;
    xr_sprintf(prefix, "t=%.1f dt=%.1f frames=%u", float(Device.dwTimeGlobal) / 1000.f, period_seconds(),
        Device.dwFrame - g_period_frame);
    print_table(prefix, g_global);

    for (const auto& [community, table] : g_by_community)
    {
        const shared_str name = CHARACTER_COMMUNITY::IndexToId(community, nullptr, true);
        xr_sprintf(prefix, "c=%s", name.size() ? name.c_str() : "unknown");
        print_table(prefix, table);
    }

    g_global = {};
    g_by_community.clear();
    g_period_start = Device.dwTimeGlobal;
    g_period_frame = Device.dwFrame;
}

namespace RayBudget
{
bool take(u32 count)
{
    if (g_ray_frame != Device.dwFrame)
    {
        g_ray_frame = Device.dwFrame;
        g_rays_used = 0;
    }
    if (g_rays_used + count > g_config.rays_per_frame)
    {
        stat_add(Stat::rays_deferred, float(count));
        return false;
    }
    g_rays_used += count;
    g_rays_granted += count;
    stat_add(Stat::rays, float(count));
    return true;
}

u32 left()
{
    if (g_ray_frame != Device.dwFrame)
        return g_config.rays_per_frame;
    return g_rays_used < g_config.rays_per_frame ? g_config.rays_per_frame - g_rays_used : 0;
}

u32 granted() { return g_rays_granted; }
} // namespace RayBudget

void load_config()
{
    constexpr pcstr section = "ai_config";
    g_config = {};
    g_config.rays_per_frame = READ_IF_EXISTS(pSettings, r_u32, section, "rays_per_frame", g_config.rays_per_frame);
    g_config.squad_tick_ms = READ_IF_EXISTS(pSettings, r_u32, section, "squad_tick_ms", g_config.squad_tick_ms);
    g_config.squad_refresh_ms =
        READ_IF_EXISTS(pSettings, r_u32, section, "squad_refresh_ms", g_config.squad_refresh_ms);
    g_config.lod_near_m = READ_IF_EXISTS(pSettings, r_float, section, "lod_near_m", g_config.lod_near_m);
    load_profile_config();
    g_config.loaded = true;
}

namespace
{
void log_periodically()
{
    if (g_log_period_ms > 0 && Device.dwTimeGlobal >= g_last_log + u32(g_log_period_ms))
    {
        g_last_log = Device.dwTimeGlobal;
        stats_print_and_reset();
    }
}

class CCC_AiFeature final : public IConsole_Command
{
public:
    CCC_AiFeature(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = false; }

    void Execute(pcstr args) override
    {
        string64 name{};
        int value = -1;
        if (sscanf_s(args, "%63s %d", name, u32(std::size(name)), &value) != 2 || (value != 0 && value != 1))
        {
            InvalidSyntax();
            return;
        }

        if (!xr_strcmp(name, "all"))
        {
            g_features = value ? (1u << u32(Feature::count)) - 1 : 0u;
            return;
        }

        Feature which;
        if (!feature_by_name(name, which))
        {
            Msg("! ai_feature: unknown feature '%s'", name);
            return;
        }
        if (value)
            g_features |= 1u << u32(which);
        else
            g_features &= ~(1u << u32(which));
    }

    void Info(TInfo& info) override { xr_strcpy(info, "<feature|all> <0|1>, see ai_features"); }
};

class CCC_AiFeatures final : public IConsole_Command
{
public:
    CCC_AiFeatures(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr) override
    {
        for (size_t index = 0; index < size_t(Feature::count); ++index)
            Msg("[ai] feature %s=%d", feature_names[index], feature(Feature(index)) ? 1 : 0);
    }
};

class CCC_AiStats final : public IConsole_Command
{
public:
    CCC_AiStats(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr) override { stats_print_and_reset(); }
};

// QA parameter of the live feature: never written to user.ltx.
class CCC_AiLogPeriod final : public CCC_Integer
{
public:
    CCC_AiLogPeriod(pcstr name, int* value) : CCC_Integer(name, value, 0, 600000) {}

    void Execute(pcstr args) override
    {
        CCC_Integer::Execute(args);
        g_last_log = Device.dwTimeGlobal;
    }

    void Save(IWriter*) override {}
};

// QA switch of the trace lines: never written to user.ltx.
class CCC_AiTrace final : public CCC_Integer
{
public:
    CCC_AiTrace(pcstr name, int* value) : CCC_Integer(name, value, 0, 1) {}

    void Save(IWriter*) override {}
};
} // namespace
} // namespace DaAi

using namespace DaAi;

void da_ai_register_console()
{
    CMD1(CCC_AiFeature, "ai_feature");
    CMD1(CCC_AiFeatures, "ai_features");
    CMD1(CCC_AiStats, "ai_stats");
    CMD2(CCC_AiLogPeriod, "ai_log_period", &g_log_period_ms);
    CMD2(CCC_AiTrace, "ai_trace", &g_trace);
}

void da_ai_spawn(CAI_Stalker& npc)
{
    if (!npc.g_Alive())
        return;

    ScopedTimer timer(Stat::p_total_us);
    if (!g_config.loaded)
    {
        load_config();
        Know::level_prepare();
    }
    Registry::add(npc);
}

void da_ai_destroy(CAI_Stalker& npc)
{
    if (!npc.da_ai())
        return;

    ScopedTimer timer(Stat::p_total_us);
    Registry::remove(npc);
}

void da_ai_reload(CAI_Stalker& npc, pcstr section)
{
    if (!npc.da_ai())
        return;

    invalidate_profile(npc);
    Comms::on_reload(npc, section);
}

void da_ai_pre_memory(CAI_Stalker& npc, float dt)
{
    NpcState* state = npc.da_ai();
    if (!state || state->dead)
        return;

    ScopedTimer timer(Stat::p_total_us, state->community);
    state->distant = npc.Position().distance_to_sqr(Device.vCameraPosition) > _sqr(g_config.lod_near_m);
    state->calm = !npc.memory().enemy().selected();
    Sense::pre_memory(npc, dt);
    state->pre_us = timer.elapsed_us();
    stat_add(Stat::p_max_pre_us, state->pre_us);
}

void da_ai_post_memory(CAI_Stalker& npc, float dt)
{
    NpcState* state = npc.da_ai();
    if (!state || state->dead)
        return;

    {
        ScopedTimer timer(Stat::p_total_us, state->community);
        // The worst of each part of an update (p_max_*_us, the registry refresh with know): what p_max_us is made of.
        float mark = 0.f;
        const auto lap = [&timer, &mark](Stat stat) {
            const float now = timer.elapsed_us();
            stat_add(stat, now - mark);
            mark = now;
        };
        Registry::refresh(npc);
        Know::update(npc, dt);
        lap(Stat::p_max_know_us);
        Comms::update(npc, dt);
        lap(Stat::p_max_comms_us);
        Morale::update(npc, dt);
        lap(Stat::p_max_morale_us);
        Tactics::update(npc, dt);
        lap(Stat::p_max_tactics_us);
        Search::update(npc, dt);
        lap(Stat::p_max_search_us);
        squad_tick(npc);
        lap(Stat::p_max_squad_us);
        stat_add(Stat::p_max_us, state->pre_us + timer.elapsed_us(), state->community);
    }
    log_periodically();
}

void da_ai_update_cl(CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    if (!state || state->dead)
        return;

    // Once per frame, whichever stalker updates first; timed on its own, not as this NPC's work.
    Comms::dispatch_script_reports();
    ScopedTimer timer(Stat::p_total_us, state->community);
    Tactics::prepare_fire(npc);
}

bool da_ai_may_fire(IGameObject& owner)
{
    CAI_Stalker* npc = smart_cast<CAI_Stalker*>(&owner);
    const NpcState* state = npc ? npc->da_ai() : nullptr;
    if (!state || state->dead)
        return true;

    // The weapon fires on its own update, a frame after the object handler asked for it, and the brain may have lost
    // the target in between: a round leaves only at a target in own sight or as honest suppression (F4, F5), and not
    // at one that has left the weapon's band since (r2e: a pistol round at 41 m against engage_max 40). In engine
    // combat no round leaves without a selected enemy; scripted scenes outside it shoot as stock.
    if (!Know::active(*npc))
        return true;
    const CEntityAlive* enemy = npc->memory().enemy().selected();
    if (!enemy)
        return !Tactics::engine_combat(*npc);
    if (Know::sees_now(*npc, enemy))
        return !Tactics::beyond_band(*npc, *enemy);
    return Tactics::fire_mode(*npc, enemy) == Tactics::FireMode::suppress;
}

void da_ai_on_hit(CAI_Stalker& npc, const SHit& hit)
{
    NpcState* state = npc.da_ai();
    if (!state || state->dead)
        return;

    ScopedTimer timer(Stat::p_total_us, state->community);
    // The script judges an attacker again at once, whatever answer about him was cached.
    if (const IGameObject* who = hit.initiator())
        Know::forget_enemy_callback(npc, who->ID());
    Morale::on_hit(npc, hit);
    Comms::on_hit(npc, hit);
    Tactics::on_hit(npc, hit);
}

void da_ai_on_death(CAI_Stalker& npc, IGameObject* who)
{
    NpcState* state = npc.da_ai();
    if (!state || state->dead)
        return;

    ScopedTimer timer(Stat::p_total_us, state->community);
    // Lanes see the squad as it was (leader included) before the registry drops the dead member.
    Know::on_death(npc, who);
    Sense::on_death(npc, who);
    Comms::on_death(npc, who);
    Tactics::on_death(npc, who);
    Morale::on_death(npc, who);
    Search::on_death(npc, who);
    Registry::died(npc);
}

void da_ai_level_unload()
{
    // Per-NPC and per-squad lane state goes first, the lanes' level-wide state after it. The trait table is a
    // session registry and stays.
    Registry::clear();
    profile_level_unload();
    Search::level_unload();
    Morale::level_unload();
    Tactics::level_unload();
    Comms::level_unload();
    Sense::level_unload();
    Know::level_unload();
    g_config.loaded = false;
}
