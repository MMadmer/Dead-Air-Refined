// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_comms.h"
#include "da_ai_npc.h"

#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_space.h"
#include "stalker_sound_data.h"
#include "sound_player.h"
#include "danger_object.h"
#include "memory_manager.h"
#include "enemy_manager.h"

namespace DaAi::Comms
{
using namespace Internal;

namespace
{
// A radio or PDA sound playing at a carrier, outside its sound player.
struct DeviceSound
{
    ref_sound sound;
    u16 owner{};
};

xr_vector<DeviceSound> g_devices;
// Loaded radio squelch and PDA tone sounds, cloned for every device sound.
xr_vector<ref_sound> g_radio_sources;
xr_vector<ref_sound> g_pda_sources;

constexpr size_t spoken_cap = 4;
constexpr size_t decoded_cap = 8;

template <typename T>
void push_capped(xr_vector<T>& vector, const T& value, size_t cap)
{
    if (vector.size() >= cap)
        vector.erase(vector.begin());
    vector.push_back(value);
}

bool collection_has_sounds(const CSoundPlayer::SOUND_COLLECTIONS& sounds, u32 type)
{
    const auto found = sounds.find(type);
    return found != sounds.end() && !found->second.second->m_sounds.empty();
}

Fvector device_position(const CAI_Stalker& owner)
{
    Fvector result = owner.Position();
    result.y += config().device_height;
    return result;
}

// Somebody can hear a device sound at the carrier: the player's camera or an NPC that is not the carrier's friend (a
// friend's radio tells a hearer nothing) within the squelch range. Starting a sound costs tens of microseconds; most
// carriers talk far from both (P10: the sound starts were most of comms' worst update).
bool device_audible(const CAI_Stalker& owner)
{
    const float reach_sqr = _sqr(config().radio_squelch_range * 1.25f);
    const Fvector& position = owner.Position();
    if (Device.vCameraPosition.distance_to_sqr(position) <= reach_sqr)
        return true;
    for (const CAI_Stalker* hearer : online_npcs())
    {
        if (hearer && hearer != &owner && hearer->g_Alive() &&
            hearer->Position().distance_to_sqr(position) <= reach_sqr &&
            hearer->tfGetRelationType(&owner) != ALife::eRelationTypeFriend)
        {
            return true;
        }
    }
    return false;
}

void start_device(CAI_Stalker& owner, const ref_sound* clone_of, pcstr file)
{
    DeviceSound& device = g_devices.emplace_back();
    device.owner = owner.ID();
    if (clone_of)
        device.sound.clone(*clone_of, st_Effect, SOUND_TYPE_MONSTER_TALKING);
    else
        device.sound.create(file, st_Effect, SOUND_TYPE_MONSTER_TALKING);
    if (!device.sound._get())
    {
        g_devices.pop_back();
        return;
    }

    auto* data = xr_new<CStalkerSoundData>(&owner);
    data->bark(sound_radio);
    device.sound._get()->g_userdata = CSound_UserDataPtr(data);
    device.sound.play_at_pos(&owner, device_position(owner));

    // Audible to the player and to the AI only within the squelch range of the carrier, at full volume within half of
    // it: the squelch files keep their loudness only to 0.1 m (about -26 dB at 2 m otherwise).
    if (const CSound_params* params = device.sound.get_params())
    {
        const float range = config().radio_squelch_range;
        if (params->max_ai_distance > range)
            device.sound.set_volume(range / params->max_ai_distance);
        device.sound.set_range(range * .5f, range);
    }
}

const CSoundPlayer::CSoundSingle* playing_of(const CAI_Stalker& speaker, const CSound_UserData* data)
{
    for (const auto& single : speaker.sound().playing_sounds())
    {
        if (single.m_sound->_get() && single.m_sound->_get()->g_userdata._get() == data)
            return &single;
    }
    return nullptr;
}

// The utterance of a stock bark, made when the first hearer decodes it: the kind by [ai_comms_barks], the speaker's
// own belief of its enemy as the payload.
Utterance* stock_utterance(const CAI_Stalker& speaker, CommsState& state, u32 tag, u32 start, u32 length)
{
    const Config& c = config();
    Kind kind;
    bool involuntary = false;
    if (tag < stock_barks || (tag >= sound_copy && tag < sound_copy + stock_barks))
    {
        const u32 bark = tag < stock_barks ? tag : tag - sound_copy;
        if (!c.bark_mapped[bark])
            return nullptr;
        kind = c.bark_kind[bark];
        involuntary = c.bark_involuntary[bark];
    }
    else if (tag >= sound_line && tag - sound_line < c.line_kind.size() && c.line_mapped[tag - sound_line])
        kind = c.line_kind[tag - sound_line];
    else
        return nullptr;

    Utterance utterance;
    utterance.start = start;
    utterance.length = length;
    Message& message = utterance.msg;
    message.kind = kind;
    message.involuntary = involuntary;
    message.sender = message.origin = speaker.ID();
    message.t_sent = message.t_origin = start;
    message.channel = state.voice == VoiceMode::hushed ? Channel::hushed : Channel::voice;
    // A stock line names no friend or grenade: only an enemy kind carries the speaker's belief of its enemy, and a
    // call for help names the caller.
    if (!involuntary && enemy_kind(kind))
    {
        if (const CEntityAlive* enemy = speaker.memory().enemy().selected())
        {
            message.target = enemy->ID();
            message.estimate = describe(Know::report_of(speaker, enemy->ID()), message.channel, speaker);
        }
    }
    else if (!involuntary && kind == Kind::help)
        message.target = speaker.ID();

    push_capped(state.spoken, utterance, spoken_cap);
    if (const NpcState* self = DaAi::npc(speaker))
        stat_add(message.channel == Channel::voice ? Stat::c_sent_voice : Stat::c_sent_hushed, 1.f, self->community);
    return &state.spoken.back();
}
} // namespace

void load_sounds(CAI_Stalker& npc, pcstr section)
{
    CSoundPlayer& player = npc.sound();
    for (const auto& [type, entry] : player.objects())
    {
        if (auto* data = smart_cast<CStalkerSoundData*>(entry.first.m_data._get()))
            data->bark(type);
    }

    const Config& c = config();
    if (!c.available)
        return;

    // DA's silent tiers blank every line of their section; the base voice set supplies them for the hushed voice,
    // the involuntary cries included (C23). Stock code never plays these ids, only comms does.
    const pcstr base = c.voice_base_section.c_str();
    if (pSettings->section_exist(base))
    {
        string64 key;
        for (u32 bark = 0; bark < stock_barks; ++bark)
        {
            const auto own = player.objects().find(bark);
            if (own == player.objects().end() || !own->second.first.m_data._get() ||
                !own->second.second->m_sounds.empty())
                continue;
            xr_sprintf(key, "sound_%s", bark_name(bark));
            const pcstr prefix = pSettings->line_exist(base, key) ? pSettings->r_string(base, key) : nullptr;
            if (!prefix || !prefix[0])
                continue;

            const CSoundPlayer::CSoundCollectionParamsFull params = own->second.first;
            auto* data = xr_new<CStalkerSoundData>(&npc);
            data->bark(sound_copy + bark);
            player.add(prefix, params.m_max_count, params.m_type, params.m_priority, params.m_synchro_mask,
                sound_copy + bark, params.m_bone_name.c_str(), data);
        }
    }

    // Extra lines take the alarm's priority and synchronisation.
    const auto alarm = player.objects().find(StalkerSpace::eStalkerSoundAlarm);
    if (alarm == player.objects().end())
        return;
    const CSoundPlayer::CSoundCollectionParamsFull params = alarm->second.first;
    for (u32 index = 0; index < c.line_prefixes.size(); ++index)
    {
        auto* data = xr_new<CStalkerSoundData>(&npc);
        data->bark(sound_line + index);
        player.add(c.line_prefixes[index].c_str(), params.m_max_count, SOUND_TYPE_MONSTER_TALKING,
            params.m_priority, params.m_synchro_mask, sound_line + index, params.m_bone_name.c_str(), data);
    }
}

void on_reload(CAI_Stalker& npc, pcstr section)
{
    // The sound player was cleared and reloaded: the voice mode is resolved again on the next update.
    if (CommsState* state = state_of(npc))
        state->profile = nullptr;
}

void on_heard_voice(
    CAI_Stalker& hearer, const CStalkerSoundData& voice, const Fvector& position, float power, float masking)
{
    CommsState* state = state_of(hearer);
    const CAI_Stalker& speaker = voice.object();
    const u32 tag = voice.bark();
    if (!state || !feature(Feature::comms) || &speaker == &hearer || tag == sound_radio || tag == no_line)
        return;
    // An enemy's words are only a noise to this hearer (sense); a zombie understands nobody.
    if (hearer.is_relation_enemy(&speaker) || DaAi::profile(hearer).voice == Voice::none)
        return;
    CommsState* speaker_state = state_of(speaker);
    if (!speaker_state)
        return;

    ScopedTimer timer(Stat::p_comms_us, DaAi::npc(hearer)->community);
    const Config& c = config();
    const CSoundPlayer::CSoundSingle* single = playing_of(speaker, &voice);
    const u32 start = single ? single->m_start_time : Device.dwTimeGlobal;

    // The engine repeats the AI event of a playing sound every half second: one line is understood once.
    const Decoded decoded{speaker.ID(), start};
    if (std::any_of(state->decoded.begin(), state->decoded.end(),
            [&](const Decoded& entry) { return entry.speaker == decoded.speaker && entry.start == decoded.start; }))
    {
        return;
    }
    push_capped(state->decoded, decoded, decoded_cap);

    auto& spoken = speaker_state->spoken;
    Utterance* utterance = nullptr;
    for (Utterance& entry : spoken)
    {
        if (entry.start == start)
            utterance = &entry;
    }
    if (!utterance)
    {
        u32 length = single ? u32(iFloor(single->m_sound->get_length_sec() * 1000.f)) : 0;
        if (!length)
            length = u32(roll(c.voice_length_ms));
        utterance = stock_utterance(speaker, *speaker_state, tag, start, length);
        if (!utterance)
            return;
    }

    Message message = utterance->msg;
    if (message.involuntary)
    {
        // A cry tells where a mate is hurt or dead, as the hearer placed the sound - nothing about who did it.
        const float distance = hearer.Position().distance_to(position);
        const Estimate spot = Know::realise(hearer, &speaker, position, Source::heard_voice,
            Know::source_error(Source::heard_voice, distance), _min(1.f, power));
        const bool death = message.kind == Kind::man_down || message.kind == Kind::leader_down;
        Know::suspect(hearer, spot,
            death ? CDangerObject::eDangerTypeEntityDeath : CDangerObject::eDangerTypeEntityAttacked,
            CDangerObject::eDangerPerceiveTypeSound);
        message.estimate = spot;
    }

    message.t_available =
        utterance->start + utterance->length + understand_ms(hearer, message.channel, masking >= c.masked_ratio);
    queue(hearer, *state, message);
}

namespace Internal
{
u32 playable(const CAI_Stalker& npc, u32 line)
{
    if (line == no_line)
        return no_line;
    const auto& sounds = npc.sound().objects();
    if (line < stock_barks)
    {
        if (collection_has_sounds(sounds, line))
            return line;
        return collection_has_sounds(sounds, sound_copy + line) ? sound_copy + line : no_line;
    }
    return collection_has_sounds(sounds, line) ? line : no_line;
}

bool speak(CAI_Stalker& npc, CommsState& state, const Message& message, u32 line, float volume, bool radioed)
{
    PartTimer timer(g_sound_us);
    const u32 type = playable(npc, line);
    if (type == no_line)
        return false;

    CSoundPlayer& player = npc.sound();
    const CSound_UserData* data = player.objects().find(type)->second.first.m_data._get();
    player.play(type);

    const auto& playing = player.playing_sounds();
    if (playing.empty())
        return false;
    const CSoundPlayer::CSoundSingle& single = playing.back();
    if (single.m_start_time != Device.dwTimeGlobal || !single.m_sound->_feedback() ||
        single.m_sound->_get()->g_userdata._get() != data)
    {
        return false;
    }
    if (volume < 1.f)
        single.m_sound->set_volume(volume);

    Utterance utterance;
    utterance.msg = message;
    utterance.msg.t_sent = single.m_start_time;
    utterance.start = single.m_start_time;
    utterance.length = u32(iFloor(single.m_sound->get_length_sec() * 1000.f));
    if (!utterance.length)
        utterance.length = u32(roll(config().voice_length_ms));
    utterance.radioed = radioed;
    push_capped(state.spoken, utterance, spoken_cap);
    return true;
}

bool squad_talking(const CAI_Stalker& npc)
{
    const SquadState* squad = squad_of(npc);
    if (!squad)
        return false;
    for (const u16 id : squad->members)
    {
        const NpcState* mate = DaAi::npc(id);
        if (id != npc.ID() && mate && !mate->dead && mate->owner && mate->owner->sound().active_sound_count(true))
            return true;
    }
    return false;
}

void cry(CAI_Stalker& npc, CommsState& state, u32 bark)
{
    // Barking sections cry through the stock code; only a blanked line is replaced by the base voice set's.
    if (bark >= stock_barks || state.voice == VoiceMode::none || playable(npc, bark) != sound_copy + bark)
        return;
    const Config& c = config();
    if (!c.bark_mapped[bark])
        return;

    Message message;
    message.kind = c.bark_kind[bark];
    message.involuntary = c.bark_involuntary[bark];
    message.sender = message.origin = npc.ID();
    message.t_sent = message.t_origin = Device.dwTimeGlobal;
    message.channel = state.voice == VoiceMode::loud ? Channel::voice : Channel::hushed;
    if (speak(npc, state, message, bark, state.voice == VoiceMode::loud ? 1.f : c.hushed_volume, false))
    {
        stat_add(message.channel == Channel::voice ? Stat::c_sent_voice : Stat::c_sent_hushed, 1.f,
            DaAi::npc(npc)->community);
    }
}

void play_device(CAI_Stalker& owner, const CAI_Stalker* speaker, u32 line, bool pda)
{
    PartTimer timer(g_sound_us);
    if (!device_audible(owner))
        return;
    const auto& sources = pda ? g_pda_sources : g_radio_sources;
    if (!sources.empty())
        start_device(owner, &sources[::Random.randI(int(sources.size()))], nullptr);

    // The sender's own words out of the receiver's radio.
    if (pda || !speaker || line == no_line)
        return;
    const u32 type = playable(*speaker, line);
    if (type == no_line)
        return;
    const auto found = speaker->sound().objects().find(type);
    start_device(owner, &found->second.second->random(u32(-1)), nullptr);
}

void update_sounds(CAI_Stalker& npc, CommsState& state)
{
    const u16 id = npc.ID();
    for (size_t index = 0; index < g_devices.size();)
    {
        DeviceSound& device = g_devices[index];
        if (device.owner != id)
        {
            ++index;
            continue;
        }
        if (!device.sound._feedback())
        {
            g_devices.erase(g_devices.begin() + index);
            continue;
        }
        device.sound.set_position(device_position(npc));
        ++index;
    }

    // A hushed profile on a barking section: its stock lines are spoken quietly.
    if (state.voice != VoiceMode::hushed || state.silent_section || !feature(Feature::comms))
        return;
    const float volume = config().hushed_volume;
    for (const auto& single : npc.sound().playing_sounds())
    {
        const CSound_emitter* emitter = single.m_sound->_feedback();
        const CSound_params* params = emitter ? single.m_sound->get_params() : nullptr;
        const auto* data = single.m_sound->_get() ? smart_cast<const CStalkerSoundData*>(
                                                         single.m_sound->_get()->g_userdata._get()) :
                                                    nullptr;
        if (params && data && data->bark() < stock_barks && params->volume > volume)
            single.m_sound->set_volume(volume);
    }
}

void stop_sounds(u16 owner)
{
    for (size_t index = 0; index < g_devices.size();)
    {
        if (g_devices[index].owner != owner)
        {
            ++index;
            continue;
        }
        g_devices[index].sound.stop();
        g_devices.erase(g_devices.begin() + index);
    }
}

void clear_sounds()
{
    for (DeviceSound& device : g_devices)
        device.sound.stop();
    g_devices.clear();
    g_radio_sources.clear();
    g_pda_sources.clear();
}

void preload_device_sounds()
{
    const Config& c = config();
    const auto load = [](const xr_vector<shared_str>& files, xr_vector<ref_sound>& sources) {
        sources.clear();
        for (const shared_str& file : files)
        {
            ref_sound& sound = sources.emplace_back();
            sound.create(file.c_str(), st_Effect, SOUND_TYPE_MONSTER_TALKING);
            if (!sound._handle())
                sources.pop_back();
        }
    };
    load(c.radio_sounds, g_radio_sources);
    load(c.pda_sounds, g_pda_sources);
}
} // namespace Internal
} // namespace DaAi::Comms
