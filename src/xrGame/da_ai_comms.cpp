// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_comms.h"
#include "da_ai_npc.h"
#include "da_ai_senses.h"
#include "da_ai_tactics.h"
#include "da_ai_morale.h"
#include "da_ai_search.h"

#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_space.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "visual_memory_manager.h"
#include "agent_member_manager.h"
#include "member_order.h"
#include "Inventory.h"
#include "Weapon.h"
#include "Hit.h"
#include "xrScriptEngine/script_engine.hpp"

namespace DaAi::Comms
{
using namespace Internal;

namespace
{
Config g_comms;

// A report the HQ of a community passes to its other squads on the level.
struct HqReport
{
    Message msg;
    int community{};
    u16 squad{};
    u32 due{};
};

struct HqSent
{
    int community{};
    Kind kind{};
    u16 target{};
    u32 time{};
};

xr_vector<HqReport> g_hq;
xr_vector<HqSent> g_hq_sent;
// dead_air_x64_ai.on_ai_report was not found this level; looked up again after the next level load.
bool g_report_missing{};
bool g_report_failed{};

// A delivered report waiting for the frame's script dispatch (ids only: objects are resolved at the call).
struct ScriptReport
{
    u16 receiver;
    u16 target;
    u16 sender;
    Kind kind;
    Channel channel;
};
xr_vector<ScriptReport> g_script_reports;
xr_vector<ScriptReport> g_script_batch;
u32 g_script_frame{u32(-1)};
constexpr size_t script_report_cap = 256;

constexpr size_t inbox_cap = 32;
constexpr size_t outgoing_cap = 8;
constexpr size_t delivered_cap = 16;
constexpr size_t heard_cap = 16;
constexpr size_t sent_cap = 32;
constexpr size_t relayed_cap = 16;
constexpr size_t witnessed_cap = 4;
constexpr size_t stamp_cap = 8;

constexpr pcstr kind_names[]{"contact", "update", "lost", "man_down", "leader_down", "taking_fire", "grenade",
    "reloading", "flanking", "cover_me", "fall_back", "attack", "help", "search_assign", "all_clear", "surrender"};
static_assert(std::size(kind_names) == size_t(Kind::count));

constexpr pcstr channel_names[]{"voice", "hushed", "radio", "gesture", "pda", "hq"};
static_assert(std::size(channel_names) == size_t(Channel::count));

// Names of the stock barks as their ltx keys spell them (sound_<name>), in EStalkerSounds order.
constexpr pcstr bark_names[]{"death", "anomaly_death", "hit", "humming", "alarm", "attack_no_allies",
    "attack_allies_single_enemy", "attack_allies_several_enemies", "backup", "detour", "search1_with_allies",
    "search1_no_allies", "enemy_lost_no_allies", "enemy_lost_with_allies", "friendly_fire", "panic_human",
    "panic_monster", "tolls", "wounded", "grenade_alarm", "friendly_grenade_alarm", "need_backup", "running_in_danger",
    "kill_wounded", "enemy_critically_wounded", "enemy_killed_or_wounded", "throw_grenade"};
static_assert(std::size(bark_names) == stock_barks);

constexpr u8 bit(Channel channel) { return u8(1u << u32(channel)); }
constexpr u8 voice_bits = bit(Channel::voice) | bit(Channel::hushed);

template <typename T>
void push_capped(xr_vector<T>& vector, const T& value, size_t cap)
{
    if (vector.size() >= cap)
        vector.erase(vector.begin());
    vector.push_back(value);
}

// true when id was stamped less than interval ago; otherwise stamps it now.
bool stamped_recently(xr_vector<Stamp>& stamps, u16 id, u32 interval)
{
    const u32 now = Device.dwTimeGlobal;
    const auto found = std::find_if(stamps.begin(), stamps.end(), [id](const Stamp& stamp) { return stamp.id == id; });
    if (found != stamps.end())
    {
        if (now < found->time + interval)
            return true;
        found->time = now;
        return false;
    }
    push_capped(stamps, Stamp{id, now}, stamp_cap);
    return false;
}

// Horizontal unit vector of where a creature looks; its body direction when the eyes point straight up or down.
Fvector facing_of(const CAI_Stalker& npc)
{
    Fvector result = npc.eye_matrix.k;
    result.y = 0.f;
    if (result.square_magnitude() < EPS_S)
    {
        result = npc.Direction();
        result.y = 0.f;
    }
    if (result.square_magnitude() < EPS_S)
        return Fvector().set(0.f, 0.f, 1.f);
    return result.normalize();
}

bool kind_by_name(pcstr name, Kind& result)
{
    for (size_t index = 0; index < std::size(kind_names); ++index)
    {
        if (!xr_strcmp(kind_names[index], name))
        {
            result = Kind(index);
            return true;
        }
    }
    return false;
}

bool channel_by_name(pcstr name, Channel& result)
{
    for (size_t index = 0; index < std::size(channel_names); ++index)
    {
        if (!xr_strcmp(channel_names[index], name))
        {
            result = Channel(index);
            return true;
        }
    }
    return false;
}

u32 bark_by_name(pcstr name)
{
    for (u32 index = 0; index < stock_barks; ++index)
    {
        if (!xr_strcmp(bark_names[index], name))
            return index;
    }
    return no_line;
}

pcstr read_string(pcstr section, pcstr key)
{
    return pSettings->line_exist(section, key) ? pSettings->r_string(section, key) : nullptr;
}

Range read_range(pcstr section, pcstr key, const Range& fallback)
{
    const pcstr value = read_string(section, key);
    Range result{};
    const int count = value ? sscanf_s(value, "%f , %f", &result.lo, &result.hi) : 0;
    if (count < 1)
        return fallback;
    if (count < 2)
        result.hi = result.lo;
    if (result.hi < result.lo)
        std::swap(result.lo, result.hi);
    return result;
}

// The existing files of a comma list of sound names, each also tried with the numeric suffixes the sound player uses.
void read_sound_files(pcstr section, pcstr key, xr_vector<shared_str>& files)
{
    constexpr int max_variants = 32;
    const pcstr value = read_string(section, key);
    if (!value)
        return;
    string_path path;
    string256 name;
    string256 variant;
    for (int index = 0, items = _GetItemCount(value); index < items; ++index)
    {
        _GetItem(value, index, name);
        if (!name[0])
            continue;
        if (FS.exist(path, "$game_sounds$", name, ".ogg"))
            files.emplace_back(name);
        for (int suffix = 0; suffix < max_variants; ++suffix)
        {
            xr_sprintf(variant, "%s%d", name, suffix);
            if (FS.exist(path, "$game_sounds$", variant, ".ogg"))
                files.emplace_back(variant);
        }
    }
}

void load_config()
{
    constexpr pcstr section = "ai_comms";
    Config& c = g_comms;
    c = {};
    c.loaded = true;
    c.available = pSettings->section_exist(section);
    std::fill(std::begin(c.kind_line), std::end(c.kind_line), no_line);
    if (!c.available)
        return;

    c.hushed_volume = READ_IF_EXISTS(pSettings, r_float, section, "hushed_volume", c.hushed_volume);
    const pcstr base = read_string(section, "voice_base_section");
    c.voice_base_section = base ? base : "stalker";
    if (const pcstr value = read_string(section, "silent_probe"))
    {
        string64 item;
        for (int index = 0, items = _GetItemCount(value); index < items; ++index)
        {
            if (const u32 bark = bark_by_name(_GetItem(value, index, item)); bark != no_line)
                c.silent_probe.push_back(bark);
        }
    }
    read_sound_files(section, "radio_sound", c.radio_sounds);
    read_sound_files(section, "pda_sound", c.pda_sounds);
    c.radio_voice_volume = READ_IF_EXISTS(pSettings, r_float, section, "radio_voice_volume", c.radio_voice_volume);
    c.radio_squelch_range = READ_IF_EXISTS(pSettings, r_float, section, "radio_squelch_range", c.radio_squelch_range);
    c.radio_key_ms = read_range(section, "radio_key_ms", c.radio_key_ms);
    c.radio_message_ms = read_range(section, "radio_message_ms", c.radio_message_ms);
    c.pda_typing_ms = read_range(section, "pda_typing_ms", c.pda_typing_ms);
    c.pda_delay_ms = read_range(section, "pda_delay_ms", c.pda_delay_ms);
    c.pda_fire_block_ms = READ_IF_EXISTS(pSettings, r_u32, section, "pda_fire_block_ms", c.pda_fire_block_ms);
    c.pda_max_wait_ms = READ_IF_EXISTS(pSettings, r_u32, section, "pda_max_wait_ms", c.pda_max_wait_ms);
    c.pda_min_distance = READ_IF_EXISTS(pSettings, r_float, section, "pda_min_distance", c.pda_min_distance);
    c.pda_suppression_max =
        READ_IF_EXISTS(pSettings, r_float, section, "pda_suppression_max", c.pda_suppression_max);
    c.device_height = READ_IF_EXISTS(pSettings, r_float, section, "device_height", c.device_height);
    c.latency_floor_ms = READ_IF_EXISTS(pSettings, r_u32, section, "latency_floor_ms", c.latency_floor_ms);
    c.min_interval_ms = READ_IF_EXISTS(pSettings, r_u32, section, "min_interval_ms", c.min_interval_ms);
    c.report_callback_ms = READ_IF_EXISTS(pSettings, r_u32, section, "report_callback_ms", c.report_callback_ms);
    c.deliveries_per_update =
        READ_IF_EXISTS(pSettings, r_u32, section, "deliveries_per_update", c.deliveries_per_update);
    c.sounds_per_update = READ_IF_EXISTS(pSettings, r_u32, section, "sounds_per_update", c.sounds_per_update);
    c.transmits_per_update =
        READ_IF_EXISTS(pSettings, r_u32, section, "transmits_per_update", c.transmits_per_update);
    c.send_budget_us = READ_IF_EXISTS(pSettings, r_float, section, "send_budget_us", c.send_budget_us);
    c.take_budget_us = READ_IF_EXISTS(pSettings, r_float, section, "take_budget_us", c.take_budget_us);
    c.voice_length_ms = read_range(section, "voice_length_ms", c.voice_length_ms);
    c.voice_retry_ms = READ_IF_EXISTS(pSettings, r_u32, section, "voice_retry_ms", c.voice_retry_ms);
    c.hushed_understand_mult =
        READ_IF_EXISTS(pSettings, r_float, section, "hushed_understand_mult", c.hushed_understand_mult);
    c.masked_understand_mult =
        READ_IF_EXISTS(pSettings, r_float, section, "masked_understand_mult", c.masked_understand_mult);
    c.masked_ratio = READ_IF_EXISTS(pSettings, r_float, section, "masked_ratio", c.masked_ratio);
    c.urgent_decide_mult = READ_IF_EXISTS(pSettings, r_float, section, "urgent_decide_mult", c.urgent_decide_mult);
    c.hq_min_interval_ms = READ_IF_EXISTS(pSettings, r_u32, section, "hq_min_interval_ms", c.hq_min_interval_ms);
    c.update_interval_ms = READ_IF_EXISTS(pSettings, r_u32, section, "update_interval_ms", c.update_interval_ms);
    c.update_min_move = READ_IF_EXISTS(pSettings, r_float, section, "update_min_move", c.update_min_move);
    c.lost_after_ms = READ_IF_EXISTS(pSettings, r_u32, section, "lost_after_ms", c.lost_after_ms);
    c.lost_window_ms = READ_IF_EXISTS(pSettings, r_u32, section, "lost_window_ms", c.lost_window_ms);
    c.lost_repeat_ms = READ_IF_EXISTS(pSettings, r_u32, section, "lost_repeat_ms", c.lost_repeat_ms);
    c.relay_range = READ_IF_EXISTS(pSettings, r_float, section, "relay_range", c.relay_range);
    c.report_confidence = READ_IF_EXISTS(pSettings, r_float, section, "report_confidence", c.report_confidence);
    c.gesture_range = READ_IF_EXISTS(pSettings, r_float, section, "gesture_range", c.gesture_range);
    if (pSettings->line_exist(section, "gesture_fov"))
        c.gesture_cos = _cos(deg2rad(_max(0.f, _min(360.f, pSettings->r_float(section, "gesture_fov")))) * .5f);
    c.gesture_ms = read_range(section, "gesture_ms", c.gesture_ms);
    c.witness_ms = read_range(section, "witness_ms", c.witness_ms);
    c.witness_interval_ms = READ_IF_EXISTS(pSettings, r_u32, section, "witness_interval_ms", c.witness_interval_ms);
    c.witness_fresh_ms = READ_IF_EXISTS(pSettings, r_u32, section, "witness_fresh_ms", c.witness_fresh_ms);
    c.witness_range = READ_IF_EXISTS(pSettings, r_float, section, "witness_range", c.witness_range);
    c.witness_confidence = READ_IF_EXISTS(pSettings, r_float, section, "witness_confidence", c.witness_confidence);
    // The geometry of a friend's aim seen from outside is know's error model; read from its section, not copied.
    constexpr pcstr errors = "ai_errors";
    if (pSettings->section_exist(errors))
    {
        if (pSettings->line_exist(errors, "witness_cone_deg"))
            c.witness_cone = deg2rad(pSettings->r_float(errors, "witness_cone_deg"));
        c.witness_distance = read_range(errors, "witness_distance_prior", c.witness_distance);
    }

    string64 item;
    if (const pcstr value = read_string(section, "loud_only"))
    {
        for (int index = 0, items = _GetItemCount(value); index < items; ++index)
        {
            if (Kind kind; kind_by_name(_GetItem(value, index, item), kind))
                c.kind_loud_only[size_t(kind)] = true;
        }
    }
    if (const pcstr value = read_string(section, "urgent"))
    {
        for (int index = 0, items = _GetItemCount(value); index < items; ++index)
        {
            if (Kind kind; kind_by_name(_GetItem(value, index, item), kind))
                c.kind_urgent[size_t(kind)] = true;
        }
    }

    // Extra lines first: [ai_comms_voice] may name them.
    if (pSettings->section_exist("ai_comms_lines"))
    {
        for (const auto& line : pSettings->r_section("ai_comms_lines").Data)
        {
            if (!line.first.size() || !line.second.size())
                continue;
            c.line_prefixes.push_back(line.second);
            c.line_kind.push_back(Kind::contact);
            c.line_mapped.push_back(false);
        }
    }

    const auto line_by_name = [&c](pcstr name) -> u32 {
        if (const u32 bark = bark_by_name(name); bark != no_line)
            return bark;
        if (!pSettings->section_exist("ai_comms_lines"))
            return no_line;
        u32 index = 0;
        for (const auto& line : pSettings->r_section("ai_comms_lines").Data)
        {
            if (!line.first.size() || !line.second.size())
                continue;
            if (!xr_strcmp(line.first.c_str(), name))
                return sound_line + index;
            ++index;
        }
        return no_line;
    };

    // [ai_comms_barks]: stock bark (or extra line) = kind[, involuntary]
    if (pSettings->section_exist("ai_comms_barks"))
    {
        for (const auto& line : pSettings->r_section("ai_comms_barks").Data)
        {
            if (!line.first.size() || !line.second.size())
                continue;
            Kind kind;
            if (!kind_by_name(_GetItem(line.second.c_str(), 0, item), kind))
                continue;
            const bool involuntary = !xr_strcmp(_GetItem(line.second.c_str(), 1, item), "involuntary");
            const u32 id = line_by_name(line.first.c_str());
            if (id < stock_barks)
            {
                c.bark_kind[id] = kind;
                c.bark_mapped[id] = true;
                c.bark_involuntary[id] = involuntary;
            }
            else if (id != no_line && id - sound_line < c.line_kind.size())
            {
                c.line_kind[id - sound_line] = kind;
                c.line_mapped[id - sound_line] = true;
            }
        }
    }

    // [ai_comms_voice]: kind = the line it is said with
    if (pSettings->section_exist("ai_comms_voice"))
    {
        for (const auto& line : pSettings->r_section("ai_comms_voice").Data)
        {
            Kind kind;
            if (line.second.size() && kind_by_name(line.first.c_str(), kind))
                c.kind_line[size_t(kind)] = line_by_name(line.second.c_str());
        }
    }

    // [ai_comms_say_interval]: kind = ms one squad says it about one target at most once in
    if (pSettings->section_exist("ai_comms_say_interval"))
    {
        for (const auto& line : pSettings->r_section("ai_comms_say_interval").Data)
        {
            Kind kind;
            if (line.second.size() && kind_by_name(line.first.c_str(), kind))
                c.kind_say_interval[size_t(kind)] = u32(atoi(line.second.c_str()));
        }
    }

    // [ai_comms_kinds]: kind = the channels that may carry it
    if (pSettings->section_exist("ai_comms_kinds"))
    {
        for (const auto& line : pSettings->r_section("ai_comms_kinds").Data)
        {
            Kind kind;
            if (!line.second.size() || !kind_by_name(line.first.c_str(), kind))
                continue;
            u8 channels = 0;
            for (int index = 0, items = _GetItemCount(line.second.c_str()); index < items; ++index)
            {
                if (Channel channel; channel_by_name(_GetItem(line.second.c_str(), index, item), channel))
                    channels |= bit(channel);
            }
            // A voice callout is loud or hushed by the speaker's voice, never by the kind.
            if (channels & voice_bits)
                channels |= voice_bits;
            c.kind_channels[size_t(kind)] = channels;
        }
    }
    preload_device_sounds();
}

const Profile& profile_of(const CAI_Stalker& npc) { return DaAi::profile(npc); }

bool under_fire(const CAI_Stalker& npc, const CommsState& state)
{
    const Config& c = config();
    return (state.last_hit && Device.dwTimeGlobal < state.last_hit + c.pda_fire_block_ms) ||
        Sense::suppression(npc) > c.pda_suppression_max;
}

bool rate_allows(CommsSquad& squad, Kind kind, u16 target, u32 extra)
{
    const u32 now = Device.dwTimeGlobal;
    const u32 interval = config().min_interval_ms;
    for (auto& sent : squad.sent)
    {
        if (sent.kind != kind || sent.target != target || sent.extra != extra)
            continue;
        if (now < sent.time + interval)
            return false;
        sent.time = now;
        return true;
    }
    push_capped(squad.sent, CommsSquad::Sent{kind, target, extra, now}, sent_cap);
    return true;
}

// Spoken news per squad, kind and target whatever the extra: a search clears its spots one by one, and each clear was
// an "all clear" in the ear. The message itself still goes out as data.
bool said_recently(const CommsSquad& squad, Kind kind, u16 target, u32 interval)
{
    for (const auto& said : squad.said)
    {
        if (said.kind == kind && said.target == target)
            return Device.dwTimeGlobal < said.time + interval;
    }
    return false;
}

void stamp_said(CommsSquad& squad, Kind kind, u16 target)
{
    for (auto& said : squad.said)
    {
        if (said.kind == kind && said.target == target)
        {
            said.time = Device.dwTimeGlobal;
            return;
        }
    }
    push_capped(squad.said, CommsSquad::Sent{kind, target, 0, Device.dwTimeGlobal}, sent_cap);
}

// Alive members of the npc's squad with AI state, the npc itself excluded.
template <typename Callback>
void for_each_mate(const CAI_Stalker& npc, Callback&& callback)
{
    const SquadState* squad = squad_of(npc);
    if (!squad)
        return;
    for (const u16 id : squad->members)
    {
        NpcState* mate = DaAi::npc(id);
        if (id == npc.ID() || !mate || mate->dead || !mate->owner || !mate->comms)
            continue;
        callback(*mate);
    }
}

bool has_other_carrier(const CAI_Stalker& npc)
{
    bool found = false;
    for_each_mate(npc, [&found](const NpcState& mate) { found = found || mate.has_radio; });
    return found;
}

bool hq_link(const CAI_Stalker& npc, Kind kind)
{
    const NpcState* state = DaAi::npc(npc);
    const Profile& p = profile_of(npc);
    return state && state->has_radio && p.hq_relay && state->band >= p.hq_relay &&
        (config().kind_channels[size_t(kind)] & bit(Channel::hq));
}

void hq_enqueue(const CAI_Stalker& npc, const Message& message)
{
    const NpcState* state = DaAi::npc(npc);
    if (!state || message.involuntary)
        return;

    const u32 now = Device.dwTimeGlobal;
    const auto same = [&](const HqSent& sent) {
        return sent.community == state->community && sent.kind == message.kind && sent.target == message.target;
    };
    if (const auto found = std::find_if(g_hq_sent.begin(), g_hq_sent.end(), same); found != g_hq_sent.end())
    {
        if (now < found->time + config().hq_min_interval_ms)
            return;
        found->time = now;
    }
    else
        g_hq_sent.push_back({state->community, message.kind, message.target, now});

    HqReport report;
    report.msg = message;
    report.msg.channel = Channel::hq;
    report.msg.estimate = describe(message.estimate, Channel::hq, npc);
    report.community = state->community;
    report.squad = state->squad_id;
    report.due = now + u32(roll(profile_of(npc).hq_delay_s) * 1000.f);
    g_hq.push_back(report);
}

// The HQ passes a report on to the carriers of the community's other squads on the level.
void hq_dispatch()
{
    const u32 now = Device.dwTimeGlobal;
    for (size_t index = 0; index < g_hq.size();)
    {
        const HqReport report = g_hq[index];
        if (now < report.due)
        {
            ++index;
            continue;
        }
        g_hq.erase(g_hq.begin() + index);

        bool sent = false;
        for (CAI_Stalker* receiver : online_npcs())
        {
            NpcState* state = receiver->da_ai();
            if (!state || state->dead || !state->has_radio || !state->comms || state->community != report.community ||
                state->squad_id == report.squad)
            {
                continue;
            }
            Message message = report.msg;
            message.t_sent = now;
            message.t_available = now + understand_ms(*receiver, Channel::hq, false);
            queue(*receiver, *state->comms, message, now);
            sent = true;
        }
        if (sent)
            stat_add(Stat::c_sent_hq, 1.f, report.community);
    }
}

// Radio to the carriers of the squad (and the HQ link). false = there was nobody to talk to.
bool send_radio(CAI_Stalker& npc, CommsState& state, const Message& base, bool speech, u32 line)
{
    if (!feature(Feature::radio))
        return false;
    const NpcState* self = DaAi::npc(npc);
    if (!self->has_radio)
    {
        stat_add(Stat::c_radio_violation, 1.f, self->community);
        return false;
    }

    const bool hq = hq_link(npc, base.kind);
    xr_vector<NpcState*> receivers;
    for_each_mate(npc, [&receivers](NpcState& mate) {
        if (mate.has_radio)
            receivers.push_back(&mate);
    });
    if (receivers.empty() && !hq)
        return false;

    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    Message message = base;
    message.channel = Channel::radio;
    message.estimate = describe(base.estimate, Channel::radio, npc);
    stat_add(Stat::c_sent_radio, 1.f, self->community);

    play_device(npc, nullptr, no_line, false);
    if (speech)
    {
        // The words spoken into the radio are real speech: mates next to the sender hear them hushed.
        Message spoken = base;
        spoken.channel = Channel::hushed;
        spoken.estimate = describe(base.estimate, Channel::hushed, npc);
        speak(npc, state, spoken, line, c.radio_voice_volume, true);
    }

    const u32 key = u32(roll(c.radio_key_ms));
    const u32 length = u32(roll(c.radio_message_ms));
    // The words come out of the receiver nearest to the camera only: every radio of an army post replayed them.
    const NpcState* speaker_radio = nullptr;
    float speaker_distance = flt_max;
    for (NpcState* receiver : receivers)
    {
        const float distance = receiver->owner->Position().distance_to_sqr(Device.vCameraPosition);
        if (distance < speaker_distance)
        {
            speaker_distance = distance;
            speaker_radio = receiver;
        }
    }
    for (NpcState* receiver : receivers)
    {
        Message delivery = message;
        delivery.t_available = now + key + length + understand_ms(*receiver->owner, Channel::radio, false);
        const bool words = receiver == speaker_radio && state.voice != VoiceMode::none;
        queue(*receiver->owner, *receiver->comms, delivery, now + key, words ? line : no_line);
    }
    if (hq)
        hq_enqueue(npc, message);
    return true;
}

// Hand signals: the sender's band allows them and the kind may be signalled.
bool gesture_capable(const CAI_Stalker& npc, Kind kind)
{
    const NpcState* self = DaAi::npc(npc);
    const Profile& p = profile_of(npc);
    return self && p.gestures && self->band >= p.gestures &&
        (config().kind_channels[size_t(kind)] & bit(Channel::gesture));
}

// A viewer takes a hand signal when both see each other with their own eyes, it is within reach and the sender is in
// its field of view.
bool sees_signal(const CAI_Stalker& sender, const CAI_Stalker& viewer)
{
    const Config& c = config();
    const NpcState* state = DaAi::npc(viewer);
    if (&viewer == &sender || !state || state->dead || !state->comms || !viewer.g_Alive())
        return false;
    Fvector to_sender = Fvector().sub(sender.Position(), viewer.Position());
    to_sender.y = 0.f;
    const float distance = to_sender.magnitude();
    if (distance > c.gesture_range)
        return false;
    // A zombie understands nobody; an enemy is not signalled to.
    if (DaAi::profile(viewer).voice == Voice::none || sender.is_relation_enemy(&viewer) ||
        viewer.is_relation_enemy(&sender))
    {
        return false;
    }
    if (distance > EPS_L && facing_of(viewer).dotproduct(to_sender.div(distance)) < c.gesture_cos)
        return false;
    return sender.memory().visual().visible_now(&viewer) && viewer.memory().visual().visible_now(&sender);
}

template <typename Callback>
void for_each_signal_viewer(const CAI_Stalker& sender, Callback&& callback)
{
    for (CAI_Stalker* viewer : online_npcs())
    {
        if (viewer && sees_signal(sender, *viewer))
            callback(*viewer);
    }
}

// Hand signals to everyone who takes them. false = nobody saw them.
bool send_gesture(CAI_Stalker& npc, const Message& base)
{
    // Viewers first: the spot is described only for a signal somebody sees.
    xr_vector<CAI_Stalker*> viewers;
    for_each_signal_viewer(npc, [&viewers](CAI_Stalker& viewer) { viewers.push_back(&viewer); });
    if (viewers.empty())
        return false;

    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    Message message = base;
    message.channel = Channel::gesture;
    message.estimate = describe(base.estimate, Channel::gesture, npc);
    const u32 length = u32(roll(c.gesture_ms));
    for (CAI_Stalker* viewer : viewers)
    {
        Message delivery = message;
        delivery.t_available = now + length + understand_ms(*viewer, Channel::gesture, false);
        queue(*viewer, *DaAi::npc(*viewer)->comms, delivery);
    }
    stat_add(Stat::c_sent_gesture, 1.f, DaAi::npc(npc)->community);
    return true;
}

// PDA text to the squad members out of voice reach that the radio did not reach.
void send_pda(CAI_Stalker& npc, const Message& base, bool radioed)
{
    const Config& c = config();
    const NpcState* self = DaAi::npc(npc);
    xr_vector<NpcState*> receivers;
    const Fvector& position = npc.Position();
    for_each_mate(npc, [&](NpcState& mate) {
        if (radioed && mate.has_radio && self->has_radio)
            return;
        if (mate.owner->Position().distance_to(position) > c.pda_min_distance)
            receivers.push_back(&mate);
    });
    if (receivers.empty())
        return;

    const u32 now = Device.dwTimeGlobal;
    Message message = base;
    message.channel = Channel::pda;
    message.estimate = describe(base.estimate, Channel::pda, npc);
    stat_add(Stat::c_sent_pda, 1.f, self->community);

    const u32 arrival = now + u32(roll(c.pda_typing_ms) + roll(c.pda_delay_ms));
    for (NpcState* receiver : receivers)
    {
        Message delivery = message;
        delivery.t_available = arrival + understand_ms(*receiver->owner, Channel::pda, false);
        queue(*receiver->owner, *receiver->comms, delivery, arrival, no_line, true);
    }
}

// The payload at the moment the message starts: the sender's own belief, never the target's position.
Estimate payload(const CAI_Stalker& npc, const Outgoing& out)
{
    if (out.target == u16(-1))
        return {};
    Estimate result{};
    if (out.kind == Kind::help && out.target == npc.ID())
    {
        // "I'm down here": the sender knows where it lies.
        result.pos = npc.Position();
        result.t_observed = Device.dwTimeGlobal;
        result.target = out.target;
        result.reporter = npc.ID();
        result.source = Source::seen;
        result.confidence = 1.f;
    }
    else
        result = Know::report_of(npc, out.target);
    if (result.source == Source::none && out.relayed)
        result = out.fallback;
    return result;
}

// The squad said this a moment ago: the news still reaches the mates within earshot, it is only not said aloud
// again. Coordination rides on it - a search spot cleared, a target lost - so dropping it with the words left
// mates sent back to spots already cleared.
void pass_unsaid(CAI_Stalker& npc, const SquadState& squad, const Message& base)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    Message message = base;
    message.channel = Channel::hushed;
    message.estimate = describe(base.estimate, Channel::hushed, npc);
    for (const u16 id : squad.members)
    {
        NpcState* mate = DaAi::npc(id);
        if (id == npc.ID() || !mate || mate->dead || !mate->owner || !mate->comms)
            continue;
        if (mate->owner->Position().distance_to_sqr(npc.Position()) > _sqr(c.relay_range))
            continue;
        Message delivery = message;
        delivery.t_available = now + understand_ms(*mate->owner, Channel::hushed, false);
        queue(*mate->owner, *mate->comms, delivery);
    }
}

// Tries every channel still pending; the ones that are done or impossible are cleared.
void transmit(CAI_Stalker& npc, CommsState& state, Outgoing& out)
{
    const Config& c = config();
    const Profile& p = profile_of(npc);
    const u32 now = Device.dwTimeGlobal;
    const bool expired = now >= out.t_ready + c.voice_retry_ms;

    Message base;
    base.kind = out.kind;
    base.target = out.target;
    base.extra = out.extra;
    base.sender = npc.ID();
    base.origin = out.origin;
    base.t_origin = out.t_origin;
    base.t_sent = now;
    base.relayed = out.relayed;
    base.estimate = payload(npc, out);

    const u32 line = out.line != no_line ? out.line : c.kind_line[size_t(out.kind)];
    const bool want_radio = out.channels & bit(Channel::radio);
    bool voiced = false;

    const u32 say_interval = c.kind_say_interval[size_t(out.kind)];
    SquadState* say_squad = say_interval ? squad_of(npc) : nullptr;
    const bool words =
        !(say_squad && say_squad->comms && said_recently(*say_squad->comms, out.kind, out.target, say_interval));
    if (!words && (out.channels & voice_bits))
    {
        pass_unsaid(npc, *say_squad, base);
        out.channels &= ~voice_bits;
    }

    if (out.channels & bit(Channel::gesture))
    {
        // Signals go at once, whoever talks; a sneak signalled to someone keeps its mouth shut.
        const bool signalled = gesture_capable(npc, out.kind) && send_gesture(npc, base);
        out.channels &= ~bit(Channel::gesture);
        const bool urgent_kind = (out.flags & urgent) || c.kind_urgent[size_t(out.kind)];
        if (signalled && (out.flags & quiet) && !urgent_kind)
            out.channels &= ~voice_bits;
    }

    if (out.channels & voice_bits)
    {
        VoiceMode mode = state.voice;
        if (mode == VoiceMode::loud && (out.flags & quiet))
            mode = VoiceMode::hushed;
        const bool possible = mode != VoiceMode::none && p.callouts && playable(npc, line) != no_line &&
            (mode == VoiceMode::loud || !c.kind_loud_only[size_t(out.kind)]);
        if (!possible)
            out.channels &= ~voice_bits;
        else if (squad_talking(npc) && !expired)
            return; // one talker at a time; the radio waits for the words too
        else
        {
            Message spoken = base;
            spoken.channel = mode == VoiceMode::loud ? Channel::voice : Channel::hushed;
            spoken.estimate = describe(base.estimate, spoken.channel, npc);
            const float volume = mode == VoiceMode::loud ? 1.f : c.hushed_volume;
            voiced = speak(npc, state, spoken, line, volume, want_radio);
            if (voiced)
                stat_add(spoken.channel == Channel::voice ? Stat::c_sent_voice : Stat::c_sent_hushed, 1.f,
                    DaAi::npc(npc)->community);
            if (voiced || expired)
                out.channels &= ~voice_bits;
            else
                return;
        }
    }

    bool radioed = false;
    if (want_radio)
    {
        if (DaAi::npc(npc)->has_radio)
            radioed = send_radio(npc, state, base, words && !voiced && state.voice != VoiceMode::none && p.callouts,
                words ? line : no_line);
        out.channels &= ~bit(Channel::radio);
    }
    if (words && say_squad && say_squad->comms && (voiced || radioed))
        stamp_said(*say_squad->comms, out.kind, out.target);

    if (out.channels & bit(Channel::pda))
    {
        if (!p.pda)
            out.channels &= ~bit(Channel::pda);
        else if (under_fire(npc, state))
        {
            // Nobody types under fire; the text goes when it is quiet or not at all.
            if (now >= out.t_ready + c.pda_max_wait_ms)
                out.channels &= ~bit(Channel::pda);
        }
        else
        {
            send_pda(npc, base, radioed);
            out.channels &= ~bit(Channel::pda);
        }
    }

    // The HQ link goes with the radio.
    out.channels &= ~bit(Channel::hq);
}

// Relays of 2.6: a carrier passes the net's news on by voice to mates without a radio, and keys the radio for news
// a mate without a radio told it. One hop: a relayed message is never relayed again.
void relay(CAI_Stalker& npc, CommsState& state, const Message& message)
{
    if (message.relayed || message.involuntary || !feature(Feature::radio))
        return;
    const NpcState* self = DaAi::npc(npc);
    SquadState* squad = squad_of(npc);
    if (!self->has_radio || !squad || !squad->comms)
        return;

    const Config& c = config();
    const u8 kind_channels = c.kind_channels[size_t(message.kind)];
    u8 channels = 0;
    if (message.channel == Channel::radio || message.channel == Channel::hq)
    {
        // Every carrier with a mate without a radio near it tells him; the mate understands it once.
        bool deaf_mates = false;
        const Fvector& position = npc.Position();
        for_each_mate(npc, [&](const NpcState& mate) {
            deaf_mates = deaf_mates ||
                (!mate.has_radio && mate.owner->Position().distance_to(position) <= c.relay_range);
        });
        if (deaf_mates)
            channels = kind_channels & (voice_bits | bit(Channel::gesture));
    }
    else if (message.channel == Channel::voice || message.channel == Channel::hushed ||
        message.channel == Channel::gesture)
    {
        // One carrier keys the radio for news a mate without a radio told the squad.
        const MessageKey key{message.origin, message.kind, message.t_origin, message.extra};
        auto& relayed = squad->comms->relayed;
        const NpcState* sender = DaAi::npc(message.sender);
        if (sender && sender->squad_id == self->squad_id && !sender->has_radio &&
            std::find(relayed.begin(), relayed.end(), key) == relayed.end() &&
            (has_other_carrier(npc) || hq_link(npc, message.kind)))
        {
            channels = kind_channels & bit(Channel::radio);
            if (channels)
                push_capped(relayed, key, relayed_cap);
        }
    }
    if (!channels)
        return;

    if (state.outgoing.size() >= outgoing_cap)
        state.outgoing.erase(state.outgoing.begin());

    Outgoing out;
    out.kind = message.kind;
    out.target = message.target;
    out.extra = message.extra;
    out.t_ready = Device.dwTimeGlobal + u32(roll(profile_of(npc).decide_ms));
    out.t_origin = message.t_origin;
    out.origin = message.origin;
    out.relayed = true;
    out.channels = channels;
    out.fallback = message.estimate;
    state.outgoing.push_back(out);
    stat_add(Stat::c_relay, 1.f, self->community);
}

// dead_air_x64_ai.on_ai_report(receiver, target, kind, channel, sender), at most once per receiver and target per
// [ai_comms] report_callback_ms, called by the frame's dispatch_script_reports (at most a frame later).
void report_to_script(CAI_Stalker& receiver, CommsState& state, const Message& message)
{
    if (message.target == u16(-1) || g_report_missing ||
        stamped_recently(state.report_calls, message.target, config().report_callback_ms))
    {
        return;
    }
    if (g_script_reports.size() < script_report_cap)
        g_script_reports.push_back({receiver.ID(), message.target, message.sender, message.kind, message.channel});
}

void deliver(CAI_Stalker& npc, CommsState& state, const Message& message)
{
    const MessageKey key{message.origin, message.kind, message.t_origin, message.extra, message.involuntary};
    if (std::find(state.delivered.begin(), state.delivered.end(), key) != state.delivered.end())
        return;
    push_capped(state.delivered, key, delivered_cap);

    const NpcState* self = DaAi::npc(npc);
    const int community = self->community;
    // Receivers are picked among the carriers of the sender's net, and the trait is stable for life.
    if ((message.channel == Channel::radio || message.channel == Channel::hq) && !self->has_radio)
    {
        stat_add(Stat::c_radio_violation, 1.f, community);
        return;
    }

    const u32 now = Device.dwTimeGlobal;
    const bool located = message.estimate.source != Source::none;
    const float latency = float(now - (located ? message.estimate.t_observed : message.t_origin));
    stat_add(Stat(u16(Stat::c_recv_voice) + u16(message.channel)), 1.f, community);
    if (message.channel == Channel::voice || message.channel == Channel::hushed)
        stat_add(Stat::c_lat_voice_ms, latency, community);
    else if (message.channel == Channel::radio)
        stat_add(Stat::c_lat_radio_ms, latency, community);
    stat_add(Stat::c_lat_min_ms, latency, community);

    const bool about_other = message.target != u16(-1) && message.target != npc.ID();
    CGameObject* target = about_other ? smart_cast<CGameObject*>(Level().Objects.net_Find(message.target)) : nullptr;
    if (located && !message.involuntary && enemy_kind(message.kind) && about_other)
    {
        stat_add(Stat::c_rep_err, message.estimate.err, community);
        // observe() keeps whichever estimate is the more precise now, the receiver's own or the report.
        if (target)
            Know::observe(npc, target, message.estimate);
    }

    push_capped(state.heard, HeardRecord{now, message.origin, message.sender, message.kind}, heard_cap);

    Tactics::on_message(npc, message);
    Morale::on_message(npc, message);
    Search::on_message(npc, message);
    report_to_script(npc, state, message);

    // A carrier with the HQ link passes the squad net's news up.
    if (message.channel == Channel::radio && hq_link(npc, message.kind))
        hq_enqueue(npc, message);

    if ((message.kind == Kind::man_down || message.kind == Kind::leader_down) && !message.extra)
    {
        const u16 victim_id = message.involuntary ? message.origin : message.target;
        if (const auto* victim = smart_cast<const CAI_Stalker*>(Level().Objects.net_Find(victim_id)))
        {
            Morale::on_casualty(npc, *victim, !message.involuntary && message.kind == Kind::leader_down,
                message.involuntary ? Source::heard_voice : report_source(message.channel));
        }
    }

    relay(npc, state, message);
}

using StepClock = std::chrono::steady_clock;

float since_us(StepClock::time_point start)
{
    return std::chrono::duration<float, std::micro>(StepClock::now() - start).count();
}

// Takes in the due messages of the inbox; the number taken (the witness cues share the per-update share), the share
// itself once the update's taking in ran out of [ai_comms] take_budget_us.
u32 deliver_due(CAI_Stalker& npc, CommsState& state, StepClock::time_point start)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    const u32 cap = _max(c.deliveries_per_update, 1u);
    const u32 sounds_cap = _max(c.sounds_per_update, 1u);
    u32 sounds = 0;
    xr_vector<Message> due;
    for (size_t index = 0; index < state.inbox.size();)
    {
        Pending& pending = state.inbox[index];
        if (pending.t_play != u32(-1) && now >= pending.t_play)
        {
            // Every started sound is a new emitter (P10: a radio net's burst queued several on one receiver).
            if (sounds < sounds_cap)
            {
                const NpcState* sender = DaAi::npc(pending.msg.sender);
                play_device(npc, sender ? sender->owner : nullptr, pending.line, pending.pda);
                pending.t_play = u32(-1);
                ++sounds;
            }
            else
                stat_add(Stat::c_deferred, 1.f, DaAi::npc(npc)->community);
        }
        // Beyond the per-update share a due message waits for the next update, in order; its sound comes first.
        if (pending.t_play == u32(-1) && now >= pending.msg.t_available && Device.dwFrame > pending.frame &&
            due.size() < cap)
        {
            due.push_back(pending.msg);
            state.inbox.erase(state.inbox.begin() + index);
        }
        else
            ++index;
    }
    size_t delivered = 0;
    for (; delivered < due.size(); ++delivered)
    {
        // A message's handlers (tactics, morale, search, relays) vary in cost: after the first one the rest wait for
        // the next update, in order, once the budget is spent.
        if (delivered && since_us(start) > c.take_budget_us)
            break;
        const StepClock::time_point message_start = StepClock::now();
        deliver(npc, state, due[delivered]);
        stat_add(Stat::p_max_comms_msg_us, since_us(message_start));
    }
    if (delivered == due.size())
        return u32(delivered);

    Pending waiting;
    for (size_t index = due.size(); index-- > delivered;)
    {
        waiting.msg = due[index];
        state.inbox.insert(state.inbox.begin(), waiting);
    }
    stat_add(Stat::c_deferred, float(due.size() - delivered), DaAi::npc(npc)->community);
    return cap;
}

void transmit_due(CAI_Stalker& npc, CommsState& state)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    const u32 cap = _max(c.transmits_per_update, 1u);
    const StepClock::time_point start = StepClock::now();
    u32 sent = 0;
    for (size_t index = 0; index < state.outgoing.size();)
    {
        if (now >= state.outgoing[index].t_ready)
        {
            // Each transmission describes its spot per channel and may reach every mate: a few per update within the
            // update's budget, an urgent warning always.
            const Outgoing& next = state.outgoing[index];
            if ((sent >= cap || (sent && since_us(start) > c.send_budget_us)) && !(next.flags & urgent) &&
                !c.kind_urgent[size_t(next.kind)])
            {
                stat_add(Stat::c_deferred, 1.f, DaAi::npc(npc)->community);
                ++index;
                continue;
            }
            ++sent;
            // transmit may queue relays elsewhere but never touches this vector
            Outgoing out = state.outgoing[index];
            transmit(npc, state, out);
            if (!out.channels)
            {
                state.outgoing.erase(state.outgoing.begin() + index);
                continue;
            }
            state.outgoing[index] = out;
        }
        ++index;
    }
}

// Callouts no stock bark makes: contact lost, and position updates over the radio while the enemy is in sight.
void watch_combat(CAI_Stalker& npc, CommsState& state)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    const u16 id = enemy ? enemy->ID() : u16(-1);

    if (state.combat_target != u16(-1) && id == u16(-1))
    {
        // Said by an NPC that saw him itself a moment ago and lost him: not by one that let a rumour go or gave a
        // search up (the belief is forgotten then), and not again for the same man for a while.
        const auto* lost = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(state.combat_target));
        u32 seen = 0;
        if (lost && lost->g_Alive())
        {
            if (const Belief* belief = Know::belief(npc, lost->ID()))
                seen = belief->t_seen_own;
            else if (!Know::active(npc))
                seen = Know::last_seen_time(npc, lost);
        }
        const bool fresh = seen && seen != u32(-1) && now >= seen + c.lost_after_ms && now < seen + c.lost_window_ms;
        const bool repeat = state.lost_target == state.combat_target && now < state.lost_at + c.lost_repeat_ms;
        if (fresh && !repeat && request(npc, Kind::lost, state.combat_target))
        {
            state.lost_target = state.combat_target;
            state.lost_at = now;
        }
    }
    state.combat_target = id;

    // "Reloading!" is shouted only by loud voices ([ai_comms] loud_only) and only with an enemy about.
    const auto* weapon = smart_cast<const CWeapon*>(npc.inventory().ActiveItem());
    const bool reloading = weapon && weapon->GetState() == CWeapon::eReload;
    if (reloading && !state.reloading && enemy)
        request(npc, Kind::reloading, id);
    state.reloading = reloading;

    const NpcState* self = DaAi::npc(npc);
    if (!enemy || !self->has_radio || now < state.update_time + c.update_interval_ms || !Know::sees_now(npc, enemy))
        return;
    const Estimate own = Know::report_of(npc, id);
    if (own.source == Source::none)
        return;
    if (state.update_target == id && own.pos.distance_to(state.update_pos) < c.update_min_move)
        return;
    state.update_target = id;
    state.update_pos = own.pos;
    state.update_time = now;
    request(npc, Kind::update, id);
}

// Whether any channel of the kind can carry a message of this NPC at all (false = the caller keeps its stock path).
bool can_send(const CAI_Stalker& npc, const CommsState& state, Kind kind, u32 flags, u32 line, u8 channels)
{
    const Config& c = config();
    const Profile& p = profile_of(npc);
    const NpcState* self = DaAi::npc(npc);
    if (channels & voice_bits)
    {
        const VoiceMode mode = state.voice == VoiceMode::loud && (flags & quiet) ? VoiceMode::hushed : state.voice;
        const u32 said = line != no_line ? line : c.kind_line[size_t(kind)];
        if (mode != VoiceMode::none && p.callouts && playable(npc, said) != no_line &&
            (mode == VoiceMode::loud || !c.kind_loud_only[size_t(kind)]))
        {
            return true;
        }
    }
    if ((channels & bit(Channel::radio)) && feature(Feature::radio) && self->has_radio &&
        (has_other_carrier(npc) || hq_link(npc, kind)))
    {
        return true;
    }
    if ((channels & bit(Channel::gesture)) && gesture_capable(npc, kind))
    {
        bool seen = false;
        for_each_signal_viewer(npc, [&seen](const CAI_Stalker&) { seen = true; });
        if (seen)
            return true;
    }
    if ((channels & bit(Channel::pda)) && p.pda)
    {
        bool far_mate = false;
        const Fvector& position = npc.Position();
        for_each_mate(npc, [&](const NpcState& mate) {
            far_mate = far_mate || mate.owner->Position().distance_to(position) > c.pda_min_distance;
        });
        return far_mate;
    }
    return false;
}

// Witness cues the viewer has understood by now: a belief on the friend's enemy (when it fights one the viewer is
// hostile to) and a suspicion at the spot, both at the guessed position.
void deliver_witnessed(CAI_Stalker& npc, CommsState& state, u32 taken, StepClock::time_point start)
{
    const u32 now = Device.dwTimeGlobal;
    const u32 cap = _max(config().deliveries_per_update, 1u);
    for (size_t index = 0; index < state.witnessed.size();)
    {
        const Witness cue = state.witnessed[index];
        if (now < cue.t_available || Device.dwFrame <= cue.frame)
        {
            ++index;
            continue;
        }
        // The witness cues share the update's share and budget with the messages; the rest wait, in order.
        if (taken >= cap || (taken && since_us(start) > config().take_budget_us))
        {
            stat_add(Stat::c_deferred, 1.f, DaAi::npc(npc)->community);
            break;
        }
        ++taken;
        state.witnessed.erase(state.witnessed.begin() + index);

        Estimate spot = cue.estimate;
        const CEntityAlive* enemy =
            cue.fighting ? smart_cast<const CEntityAlive*>(Level().Objects.net_Find(spot.target)) : nullptr;
        if (enemy && (!enemy->g_Alive() || enemy->getDestroy()))
            enemy = nullptr;
        if (enemy)
            Know::observe(npc, enemy, spot);
        else
            spot.target = u16(-1);
        Know::suspect(npc, spot,
            enemy ? CDangerObject::eDangerTypeAttackSound : CDangerObject::eDangerTypeEnemySound,
            CDangerObject::eDangerPerceiveTypeVisual);
        stat_add(Stat::c_witness, 1.f, DaAi::npc(npc)->community);
    }
}

void resolve_voice(const CAI_Stalker& npc, CommsState& state)
{
    const Config& c = config();
    const Profile& p = profile_of(npc);
    state.profile = &p;

    state.silent_section = !c.silent_probe.empty();
    const auto& sounds = npc.sound().objects();
    for (const u32 bark : c.silent_probe)
    {
        const auto found = sounds.find(bark);
        if (found != sounds.end() && !found->second.second->m_sounds.empty())
            state.silent_section = false;
    }

    switch (p.voice)
    {
    case Voice::loud: state.voice = VoiceMode::loud; break;
    case Voice::hushed: state.voice = VoiceMode::hushed; break;
    case Voice::none: state.voice = VoiceMode::none; break;
    default: state.voice = state.silent_section ? VoiceMode::hushed : VoiceMode::loud; break;
    }
}
} // namespace

namespace Internal
{
const Config& config()
{
    if (!g_comms.loaded)
        load_config();
    return g_comms;
}

pcstr bark_name(u32 bark) { return bark < stock_barks ? bark_names[bark] : ""; }

CommsState* state_of(const CAI_Stalker& npc)
{
    const NpcState* state = DaAi::npc(npc);
    return state ? state->comms : nullptr;
}

float roll(const Range& range) { return range.hi > range.lo ? ::Random.randF(range.lo, range.hi) : range.lo; }

Source report_source(Channel channel)
{
    switch (channel)
    {
    case Channel::voice: return Source::report_voice;
    case Channel::hushed: return Source::report_hushed;
    case Channel::radio: return Source::report_radio;
    case Channel::gesture: return Source::report_gesture;
    case Channel::pda: return Source::report_pda;
    case Channel::hq: return Source::report_hq;
    default: return Source::none;
    }
}

u32 understand_ms(const CAI_Stalker& receiver, Channel channel, bool masked)
{
    const Config& c = config();
    float result = roll(profile_of(receiver).understand_ms);
    if (channel == Channel::hushed)
        result *= c.hushed_understand_mult;
    if (masked)
        result *= c.masked_understand_mult;
    return u32(result);
}

void queue(CAI_Stalker& receiver, CommsState& state, const Message& message, u32 t_play, u32 line, bool pda)
{
    Pending pending;
    pending.msg = message;
    pending.t_play = t_play;
    pending.line = line;
    pending.pda = pda;
    pending.frame = Device.dwFrame;

    // Nothing is learned sooner than the floor after it was observed, and never in the frame it was sent.
    const u32 observed = message.estimate.source != Source::none ? message.estimate.t_observed : message.t_origin;
    pending.msg.t_available =
        std::max({message.t_available, observed + config().latency_floor_ms, message.t_sent + 1});

    if (state.inbox.size() >= inbox_cap)
        state.inbox.erase(state.inbox.begin());
    state.inbox.push_back(pending);
}

float g_sound_us{};
float g_describe_us{};

Estimate describe(const Estimate& estimate, Channel channel, const CAI_Stalker& sender)
{
    PartTimer timer(g_describe_us);
    if (estimate.source == Source::none)
        return estimate;

    // Describing a spot by sector or landmark: the farther it is from the speaker, the vaguer the words
    // ([ai_errors] describe_<channel>); the words land on the channel's grid.
    const Source source = report_source(channel);
    const float vagueness = Know::source_error(source, sender.Position().distance_to(estimate.pos));
    // A described spot off the graph snaps back toward the sender's own estimate (its cell), a few metres away; snapped
    // toward the sender, a spot far off the graph searched window after window (P10: a comms update of 2 ms).
    const CLevelGraph* graph = ai().get_level_graph();
    const Fvector anchor = graph && graph->valid_vertex_id(estimate.vertex) ? graph->vertex_position(estimate.vertex) :
                                                                                sender.Position();
    // On a grid the words move the spot again: its vertex is resolved once, after that.
    const float grid = Know::report_grid(source);
    Estimate result = Know::realise(sender, nullptr, estimate.pos, source, vagueness, 1.f, &anchor, grid <= 0.f);
    if (grid > 0.f)
    {
        result.pos.x = std::round(result.pos.x / grid) * grid;
        result.pos.z = std::round(result.pos.z / grid) * grid;
        result.vertex = Know::resolve_vertex(result.pos, &anchor);
    }
    result.err = estimate.err + vagueness;
    result.t_observed = estimate.t_observed;
    result.target = estimate.target;
    result.reporter = sender.ID();
    result.confidence = estimate.confidence * config().report_confidence;
    return result;
}
} // namespace Internal

CommsState* create(CAI_Stalker& npc)
{
    CommsState* state = xr_new<CommsState>();
    state->id = npc.ID();
    resolve_voice(npc, *state);
    return state;
}

void destroy(CommsState* state)
{
    if (!state)
        return;
    stop_sounds(state->id);
    xr_delete(state);
}

CommsSquad* create_squad(SquadState& squad) { return xr_new<CommsSquad>(); }

void destroy_squad(CommsSquad* state) { xr_delete(state); }

void update(CAI_Stalker& npc, float dt)
{
    CommsState* state = state_of(npc);
    if (!state)
        return;

    ScopedTimer timer(Stat::p_comms_us, DaAi::npc(npc)->community);
    if (state->profile != &profile_of(npc))
        resolve_voice(npc, *state);
    update_sounds(npc, *state);

    if (!feature(Feature::comms) || !config().available)
    {
        state->outgoing.clear();
        state->inbox.clear();
        state->witnessed.clear();
        return;
    }
    g_sound_us = g_describe_us = 0.f;
    watch_combat(npc, *state);
    const float sending = timer.elapsed_us();
    transmit_due(npc, *state);
    const float taking = timer.elapsed_us();
    const StepClock::time_point taking_start = StepClock::now();
    deliver_witnessed(npc, *state, deliver_due(npc, *state, taking_start), taking_start);
    // The worst sending and taking in of one update: what the comms part of the worst update is made of (P10).
    stat_add(Stat::p_max_comms_tx_us, taking - sending);
    stat_add(Stat::p_max_comms_rx_us, timer.elapsed_us() - taking);
    stat_add(Stat::p_max_comms_snd_us, g_sound_us);
    stat_add(Stat::p_max_comms_desc_us, g_describe_us);
}

void squad_update(SquadState& squad)
{
    if (!squad.comms)
        return;

    hq_dispatch();

    const u32 now = Device.dwTimeGlobal;
    const u32 interval = config().min_interval_ms;
    auto& sent = squad.comms->sent;
    sent.erase(std::remove_if(sent.begin(), sent.end(),
                   [now, interval](const CommsSquad::Sent& entry) { return now >= entry.time + interval; }),
        sent.end());

    const u32 hq_interval = config().hq_min_interval_ms;
    g_hq_sent.erase(std::remove_if(g_hq_sent.begin(), g_hq_sent.end(),
                        [now, hq_interval](const HqSent& entry) { return now >= entry.time + hq_interval; }),
        g_hq_sent.end());
}

void on_hit(CAI_Stalker& npc, const SHit& hit)
{
    CommsState* state = state_of(npc);
    if (!state)
        return;
    state->last_hit = Device.dwTimeGlobal;
    if (!feature(Feature::comms) || !config().available)
        return;

    const auto* attacker = smart_cast<const CEntityAlive*>(hit.initiator());
    if (!attacker || attacker->ID() == npc.ID() || !npc.is_relation_enemy(attacker))
        return;

    ScopedTimer timer(Stat::p_comms_us, DaAi::npc(npc)->community);
    if (!npc.wounded())
        cry(npc, *state, StalkerSpace::eStalkerSoundInjuring);
    request(npc, Kind::taking_fire, attacker->ID());
}

void on_death(CAI_Stalker& npc, IGameObject* who)
{
    CommsState* state = state_of(npc);
    if (!state)
        return;

    const NpcState* self = DaAi::npc(npc);
    state->outgoing.clear();
    state->inbox.clear();
    state->witnessed.clear();

    if (self->has_radio)
    {
        bool others = false;
        for_each_mate(npc, [&others](const NpcState&) { others = true; });
        if (others && !has_other_carrier(npc))
            stat_add(Stat::c_net_lost, 1.f, self->community);
    }

    if (feature(Feature::comms) && config().available && npc.death_sound_enabled())
        cry(npc, *state, StalkerSpace::eStalkerSoundDie);
}

void level_unload()
{
    g_hq.clear();
    g_hq_sent.clear();
    clear_sounds();
    g_comms.loaded = false;
    g_report_missing = false;
    g_report_failed = false;
    g_script_reports.clear();
    g_script_batch.clear();
    g_script_frame = u32(-1);
}

void dispatch_script_reports()
{
    if (g_script_reports.empty() || g_script_frame == Device.dwFrame)
        return;
    g_script_frame = Device.dwFrame;

    ScopedTimer timer(Stat::p_comms_us);
    // The script may deliver nothing itself, but a report queued while it runs waits for the next frame.
    g_script_batch.swap(g_script_reports);
    luabind::functor<void> callback;
    if (!g_report_missing && !GEnv.ScriptEngine->functor("dead_air_x64_ai.on_ai_report", callback))
        g_report_missing = true;

    const auto live = [](u16 id) {
        auto* object = id == u16(-1) ? nullptr : smart_cast<CGameObject*>(Level().Objects.net_Find(id));
        return object && !object->getDestroy() ? object : nullptr;
    };
    const auto report_failure = [](pcstr what) {
        if (!g_report_failed)
            Msg("! [ai] dead_air_x64_ai.on_ai_report failed (%s); further failures are not logged", what);
        g_report_failed = true;
    };
    for (const ScriptReport& report : g_script_batch)
    {
        CGameObject* receiver = g_report_missing ? nullptr : live(report.receiver);
        if (!receiver)
            continue;
        CGameObject* target = live(report.target);
        CGameObject* sender = live(report.sender);
        try
        {
            callback(receiver->lua_game_object(), target ? target->lua_game_object() : nullptr, int(report.kind),
                int(report.channel), sender ? sender->lua_game_object() : nullptr);
        }
#ifndef LUABIND_NO_EXCEPTIONS
        catch (luabind::error&)
        {
            report_failure("Lua error");
        }
#endif
        catch (std::exception& e)
        {
            report_failure(e.what());
        }
    }
    g_script_batch.clear();
    // Work of no NPC's update (p_max_us), but AI work all the same.
    stat_add(Stat::p_total_us, timer.elapsed_us());
}

bool request(CAI_Stalker& npc, Kind kind, u16 target, u32 extra, u32 flags, u32 line)
{
    if (!feature(Feature::comms) || kind >= Kind::count)
        return false;
    NpcState* self = DaAi::npc(npc);
    if (!self || self->dead || !self->comms || !config().available)
        return false;

    ScopedTimer timer(Stat::p_comms_us, self->community);
    CommsState& state = *self->comms;
    const Config& c = config();

    // The speaker says "the boss is down" when the man who fell is the one it follows.
    if (kind == Kind::man_down && !extra && target != u16(-1) && Morale::believed_leader(npc) == target)
        kind = Kind::leader_down;
    // A squad searching quietly talks low or by hand; a warning still goes out loud.
    if (profile_of(npc).quiet_search && !c.kind_urgent[size_t(kind)] && !(flags & urgent) && Search::active(npc))
        flags |= quiet;

    const u8 channels = c.kind_channels[size_t(kind)];
    if (!can_send(npc, state, kind, flags, line, channels))
        return false;

    if (SquadState* squad = squad_of(npc);
        squad && squad->comms && !rate_allows(*squad->comms, kind, target, extra))
    {
        return true; // said a moment ago
    }

    float decide = roll(profile_of(npc).decide_ms);
    if ((flags & urgent) || c.kind_urgent[size_t(kind)])
        decide *= c.urgent_decide_mult;

    Outgoing out;
    out.kind = kind;
    out.target = target;
    out.extra = extra;
    out.flags = flags;
    out.line = line;
    out.t_ready = Device.dwTimeGlobal + u32(decide);
    out.t_origin = Device.dwTimeGlobal;
    out.origin = npc.ID();
    out.channels = channels;
    if (state.outgoing.size() >= outgoing_cap)
        state.outgoing.erase(state.outgoing.begin());
    state.outgoing.push_back(out);
    return true;
}

bool witness(CAI_Stalker& viewer, const CAI_Stalker& mate)
{
    NpcState* self = DaAi::npc(viewer);
    if (!self || !self->comms || !feature(Feature::comms) || !config().available)
        return false;

    // A friend fights what he has seen himself a moment ago. One with an enemy he only believes in is searching:
    // what he shows is alarm. Passed on as a fight, it came back to him from the next friend as a fresh sighting,
    // and a garrison that sees itself kept the player "in sight" through walls for good.
    const Config& c = config();
    const CEntityAlive* enemy = mate.memory().enemy().selected();
    const bool sighted = enemy && enemy->g_Alive() && Know::seen_own_within(mate, *enemy, c.witness_fresh_ms);
    // An alarmed friend matters only to a viewer with no danger of its own (as the stock danger copy).
    if (!sighted && ((!enemy && !mate.memory().danger().selected()) || viewer.memory().danger().selected()))
        return true;
    if (viewer.Position().distance_to(mate.Position()) > c.witness_range ||
        !viewer.memory().visual().visible_now(&mate))
    {
        return true;
    }
    CommsState& state = *self->comms;
    if (stamped_recently(state.witness_seen, mate.ID(), c.witness_interval_ms))
        return true;

    ScopedTimer timer(Stat::p_comms_us, self->community);
    // Only what the viewer sees of him: he looks and aims somewhere, the fight is some way off in that direction.
    const float heading = facing_of(mate).getH() + ::Random.randF(-c.witness_cone, c.witness_cone);
    const float distance = roll(c.witness_distance);
    Fvector direction;
    direction.setHP(heading, 0.f);

    Witness cue;
    Estimate& spot = cue.estimate;
    spot.pos.mad(mate.Position(), direction, distance);
    spot.vertex = Know::resolve_vertex(spot.pos, &mate.Position());
    spot.err = Know::source_error(Source::witness, distance);
    spot.t_observed = Device.dwTimeGlobal;
    spot.reporter = mate.ID();
    spot.source = Source::witness;
    spot.confidence = c.witness_confidence;
    cue.fighting = sighted && viewer.is_relation_enemy(enemy);
    spot.target = cue.fighting ? enemy->ID() : u16(-1);
    cue.mate = mate.ID();
    cue.frame = Device.dwFrame;
    cue.t_available = _max(Device.dwTimeGlobal + u32(roll(c.witness_ms)), spot.t_observed + c.latency_floor_ms);
    push_capped(state.witnessed, cue, witnessed_cap);
    return true;
}

bool pooling_allowed(const CAgentManager& agent_manager)
{
    // Members share what they perceive only by the physical channels (H2).
    return !feature(Feature::comms) || !config().available;
}

bool hold_noninfo_phrase(const CAgentMemberManager& members)
{
    if (!feature(Feature::comms) || !config().available)
        return false;
    const u32 now = Device.dwTimeGlobal;
    for (const CMemberOrder* member : members.members())
    {
        const CommsState* state = state_of(member->object());
        if (!state)
            continue;
        // A callout waiting for a quiet moment, or one still being said, goes first.
        if (std::any_of(state->outgoing.begin(), state->outgoing.end(),
                [now](const Outgoing& out) { return (out.channels & voice_bits) && now >= out.t_ready; }) ||
            std::any_of(state->spoken.begin(), state->spoken.end(),
                [now](const Utterance& utterance) { return now < utterance.start + utterance.length; }))
        {
            return true;
        }
    }
    return false;
}

bool has_net(const CAI_Stalker& npc)
{
    const NpcState* self = DaAi::npc(npc);
    return self && !self->dead && self->has_radio && has_other_carrier(npc);
}

bool has_radio(const CAI_Stalker& npc)
{
    const NpcState* state = DaAi::npc(npc);
    return state && state->has_radio;
}

u32 last_heard(const CAI_Stalker& npc, Kind kind, u16 from)
{
    const CommsState* state = state_of(npc);
    if (!state)
        return u32(-1);
    u32 newest = 0;
    bool found = false;
    for (const HeardRecord& record : state->heard)
    {
        if (record.kind != kind || (from != u16(-1) && record.sender != from && record.origin != from))
            continue;
        newest = found ? std::max(newest, record.time) : record.time;
        found = true;
    }
    return found ? Device.dwTimeGlobal - newest : u32(-1);
}
} // namespace DaAi::Comms
