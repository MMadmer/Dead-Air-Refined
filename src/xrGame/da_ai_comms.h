// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "da_ai_profile.h"
#include "da_ai_knowledge.h"

// Comms lane: physical communication between NPCs (voice, hushed voice, radio nets, gestures, PDA, HQ relay) with
// latency, reach and position estimates; the witness cue; the end of the agent memory pooling.
//
// A message never carries truth: its payload is the sender's own belief (Know::report_of) degraded by the channel,
// and it reaches a receiver only through a physical path - a voice line the receiver really heard, a radio net both
// carry, a gesture it saw, a PDA text - after the sender decided, spoke or typed, and the receiver understood.

class CAI_Stalker;
class CAgentManager;
class CAgentMemberManager;
class IGameObject;
class CStalkerSoundData;
struct SHit;

namespace DaAi
{
struct SquadState;

enum class Channel : u8
{
    voice,
    hushed,
    radio,
    gesture,
    pda,
    hq,
    count
};

namespace Comms
{
// Lua on_ai_report passes the kind as its index: append-only.
enum class Kind : u8
{
    contact,
    update,
    lost,
    man_down,
    leader_down,
    taking_fire,
    grenade,
    reloading,
    flanking,
    cover_me,
    fall_back,
    attack,
    help,
    search_assign,
    all_clear,
    surrender,
    count
};

// Flags of request().
enum Flag : u32
{
    quiet = 1u << 0, // no loud voice: the sender sneaks up, holds an ambush or searches quietly
    urgent = 1u << 1, // decided faster (grenade warnings)
};

// Kinds whose target is the enemy the message is about; the others name a friend (a casualty, the wounded), a
// grenade or nobody, and never become a belief about the target.
constexpr bool enemy_kind(Kind kind)
{
    switch (kind)
    {
    case Kind::contact:
    case Kind::update:
    case Kind::lost:
    case Kind::taking_fire:
    case Kind::reloading:
    case Kind::flanking:
    case Kind::cover_me:
    case Kind::fall_back:
    case Kind::attack:
    case Kind::search_assign: return true;
    default: return false;
    }
}

// Meaning of Message::extra by kind: flanking - side (0 left, 1 right); search_assign and all_clear - the search
// lane's packed addressee, generation and spot; grenade - 1 for a friendly grenade; man_down - 1 when the man is
// wounded, not dead. Other kinds: 0.
struct Message
{
    Kind kind{};
    u16 target{u16(-1)}; // binds the report to its entity; the object's position is never read
    Estimate estimate{}; // source report_* on delivery; source none = no position (involuntary cries, orders)
    u16 sender{u16(-1)}; // who transmitted this copy (the relaying NPC for a relay)
    u16 origin{u16(-1)}; // who said it first
    Channel channel{};
    u32 extra{};
    u32 t_sent{}; // start of this copy's transmission
    u32 t_origin{}; // start of the first transmission; origin, kind, t_origin and extra identify a message
    u32 t_available{}; // delivered in the receiver's first update after this time
    bool relayed{}; // passed on once already; never relayed again
    bool involuntary{}; // a pain, death or panic cry: no target estimate
};

// Sound ids of the comms voice lines in a stalker's sound player. Runtime only and far above the script range
// (sound_script + 10000 + n).
constexpr u32 sound_copy = 0x00DA0000u; // + stock bark: the base voice set's line where the NPC's section is empty
constexpr u32 sound_line = 0x00DA0100u; // + index of [ai_comms_lines]
constexpr u32 sound_radio = 0x00DA0200u; // tag of radio and PDA sounds: heard as a noise, never decoded as speech
constexpr u32 no_line = u32(-1);
// Stock barks: StalkerSpace::EStalkerSounds from eStalkerSoundDie to eStalkerSoundThrowGrenade.
constexpr u32 stock_barks = 27;

enum class VoiceMode : u8
{
    loud,
    hushed,
    none
};

// A message on its way to this NPC.
struct Pending
{
    Message msg;
    u32 t_play{u32(-1)}; // radio or PDA sound at the receiver; u32(-1) = none or already played
    u32 line{no_line}; // the sender's line the radio plays
    u32 frame{}; // never delivered in the frame it was queued
    bool pda{};
};

// A message this NPC decided to send, per channel still to try.
struct Outgoing
{
    Kind kind{};
    u16 target{u16(-1)};
    u32 extra{};
    u32 flags{};
    u32 line{no_line}; // preferred voice line; no_line = the kind's line
    u32 t_ready{}; // the decision is made
    u32 t_origin{};
    u16 origin{u16(-1)};
    bool relayed{};
    u8 channels{}; // bit per Channel
    Estimate fallback{}; // payload of a relay when the relaying NPC has no belief of its own
};

// A line this NPC spoke; hearers decode it from here.
struct Utterance
{
    Message msg;
    u32 start{};
    u32 length{};
    bool radioed{}; // the same message also went over the radio
};

struct MessageKey
{
    u16 origin{};
    Kind kind{};
    u32 t_origin{};
    u32 extra{};
    // A pain cry and the report the hit starts share kind and moment; hearing the cry must not drop the report.
    bool involuntary{};

    bool operator==(const MessageKey&) const = default;
};

struct HeardRecord
{
    u32 time{};
    u16 origin{};
    u16 sender{};
    Kind kind{};
};

struct Decoded
{
    u16 speaker{};
    u32 start{};
};

// A busy friend this NPC saw: where he looked, understood a moment later.
struct Witness
{
    Estimate estimate; // target = the friend's enemy when he fights one the viewer is hostile to, else none
    u16 mate{u16(-1)};
    u32 t_available{};
    u32 frame{};
    bool fighting{};
};

struct Stamp
{
    u16 id{};
    u32 time{};
};
} // namespace Comms

// [ai:comms] begin state
struct CommsState
{
    u16 id{u16(-1)};
    const Profile* profile{}; // the profile the voice mode was resolved for
    Comms::VoiceMode voice{};
    bool silent_section{}; // the NPC's own section leaves its combat lines empty (DA's silent tiers)
    u32 last_hit{};
    u16 combat_target{u16(-1)}; // selected enemy at the last update (lost-contact callout)
    u16 update_target{u16(-1)};
    u32 update_time{};
    Fvector update_pos{}; // believed position of the last position update sent
    bool reloading{}; // the active weapon was reloading at the last update
    xr_vector<Comms::Outgoing> outgoing;
    xr_vector<Comms::Pending> inbox;
    xr_vector<Comms::Utterance> spoken;
    xr_vector<Comms::Decoded> decoded;
    xr_vector<Comms::MessageKey> delivered;
    xr_vector<Comms::HeardRecord> heard;
    xr_vector<Comms::Witness> witnessed;
    xr_vector<Comms::Stamp> witness_seen; // friend -> last witness cue taken from him
    xr_vector<Comms::Stamp> report_calls; // target -> last Lua on_ai_report of this receiver
};

struct CommsSquad
{
    struct Sent
    {
        Comms::Kind kind{};
        u16 target{};
        u32 extra{};
        u32 time{};
    };

    xr_vector<Sent> sent; // rate limit per kind, target and extra
    xr_vector<Comms::MessageKey> relayed; // news of mates without a radio a carrier already keyed
};
// [ai:comms] end state

namespace Comms
{
CommsState* create(CAI_Stalker& npc);
void destroy(CommsState* state);
CommsSquad* create_squad(SquadState& squad);
void destroy_squad(CommsSquad* state);
void update(CAI_Stalker& npc, float dt);
void squad_update(SquadState& squad);
void on_hit(CAI_Stalker& npc, const SHit& hit);
void on_death(CAI_Stalker& npc, IGameObject* who);
void on_reload(CAI_Stalker& npc, pcstr section);
void level_unload();
// Once per frame on the main thread, outside every NPC's update: the script notifications (on_ai_report) of the
// reports delivered since the last dispatch.
void dispatch_script_reports();

// End of CAI_Stalker::LoadSounds: tags the bark collections and loads the base voice set's lines where the NPC's
// own section is empty, plus the extra lines of [ai_comms_lines].
void load_sounds(CAI_Stalker& npc, pcstr section);

// Asks the comms system to send a message. true = comms took it (feature on); false = the caller keeps its stock
// behaviour (a stock bark). line = the voice line to say it with (a stock bark), no_line = the kind's line.
bool request(CAI_Stalker& npc, Kind kind, u16 target, u32 extra = 0, u32 flags = 0, u32 line = no_line);
// Called by the voice visitor of a hearer once the bark passed its hearing threshold: position = where the sound
// came from, masking = the hearer's hearing threshold over its floor (raised by recent gunfire).
void on_heard_voice(
    CAI_Stalker& hearer, const CStalkerSoundData& voice, const Fvector& position, float power, float masking);
// Seeing a fighting or alarmed friend: a witness estimate along his aim or facing and a suspicion, never the friend's
// enemy position or his danger object. false = comms is off and the caller keeps the stock transfer.
bool witness(CAI_Stalker& viewer, const CAI_Stalker& mate);
// false = the agent manager must not pool memory between members (stock: true).
bool pooling_allowed(const CAgentManager& agent_manager);
// Voice arbitration of the agent: true = a stock non-information phrase must wait, a member is saying or is about to
// say a comms callout.
bool hold_noninfo_phrase(const CAgentMemberManager& members);
// The NPC carries a radio and another carrier of its squad is alive.
bool has_net(const CAI_Stalker& npc);
bool has_radio(const CAI_Stalker& npc);
// Age in ms of the last delivered message of a kind from a sender (u16(-1) = anyone); u32(-1) = never.
u32 last_heard(const CAI_Stalker& npc, Kind kind, u16 from);

// Shared by the comms files only.
namespace Internal
{
struct Config
{
    float hushed_volume{.3f};
    shared_str voice_base_section; // set by load_config (no literal: g_comms is built while the DLL initialises)
    xr_vector<u32> silent_probe; // stock barks; all empty in the NPC's section = a silent tier
    xr_vector<shared_str> radio_sounds; // existing files of [ai_comms] radio_sound
    xr_vector<shared_str> pda_sounds;
    float radio_voice_volume{.3f};
    float radio_squelch_range{8.f};
    Range radio_key_ms{300.f, 600.f};
    Range radio_message_ms{600.f, 2000.f};
    Range pda_typing_ms{3000.f, 6000.f};
    Range pda_delay_ms{5000.f, 20000.f};
    u32 pda_fire_block_ms{5000};
    u32 pda_max_wait_ms{30000};
    float pda_min_distance{40.f};
    float pda_suppression_max{.2f};
    float device_height{1.2f};
    u32 latency_floor_ms{250};
    u32 min_interval_ms{4000};
    u32 report_callback_ms{2000};
    u32 deliveries_per_update{3}; // messages and witness cues one NPC update takes in; the rest wait for the next
    u32 sounds_per_update{2}; // radio or PDA sounds one NPC update starts on its device; the rest wait
    u32 transmits_per_update{2}; // outgoing messages one NPC update sends; the rest wait
    // After the first message of an update, the next ones only while the update's sending (taking in) took less.
    float send_budget_us{100.f};
    float take_budget_us{100.f};
    Range voice_length_ms{800.f, 1500.f};
    u32 voice_retry_ms{2000};
    float hushed_understand_mult{1.5f};
    float masked_understand_mult{1.5f};
    float masked_ratio{3.f};
    float urgent_decide_mult{.35f};
    u32 hq_min_interval_ms{20000};
    u32 update_interval_ms{6000};
    float update_min_move{8.f};
    u32 lost_after_ms{3000};
    float relay_range{40.f};
    float report_confidence{.8f};
    float gesture_range{25.f};
    float gesture_cos{.5f}; // cos of half of [ai_comms] gesture_fov
    Range gesture_ms{400.f, 800.f};
    Range witness_ms{300.f, 700.f};
    u32 witness_interval_ms{3000};
    float witness_range{100.f};
    float witness_confidence{.4f};
    // [ai_errors] witness_cone_deg and witness_distance_prior (know's model of a friend's aim seen from outside)
    float witness_cone{deg2rad(30.f)};
    Range witness_distance{30.f, 60.f};

    // [ai_comms_barks]: stock bark -> kind; involuntary barks carry no estimate
    Kind bark_kind[stock_barks]{};
    bool bark_mapped[stock_barks]{};
    bool bark_involuntary[stock_barks]{};
    // [ai_comms_voice] and [ai_comms_kinds]
    u32 kind_line[size_t(Kind::count)]{};
    u8 kind_channels[size_t(Kind::count)]{};
    bool kind_loud_only[size_t(Kind::count)]{};
    bool kind_urgent[size_t(Kind::count)]{};
    // [ai_comms_lines]: extra lines by the voice prefix
    xr_vector<shared_str> line_prefixes;
    xr_vector<Kind> line_kind;
    xr_vector<bool> line_mapped;

    bool available{}; // [ai_comms] exists; without it comms leaves the stock paths alone
    bool loaded{};
};

const Config& config();
pcstr bark_name(u32 bark);
CommsState* state_of(const CAI_Stalker& npc);
float roll(const Range& range);
Source report_source(Channel channel);
u32 understand_ms(const CAI_Stalker& receiver, Channel channel, bool masked);
// Queues a delivery on the receiver; the floor of [ai_comms] latency_floor_ms is applied here.
void queue(CAI_Stalker& receiver, CommsState& state, const Message& message, u32 t_play = u32(-1),
    u32 line = no_line, bool pda = false);
// The describe error of a channel on top of the sender's own belief ([ai_errors] describe_<channel>), quantised to
// the channel's grid.
Estimate describe(const Estimate& estimate, Channel channel, const CAI_Stalker& sender);

// da_ai_comms_voice.cpp
// The sound id the NPC plays for a line (stock bark or sound_line + n), no_line when it has none.
u32 playable(const CAI_Stalker& npc, u32 line);
// Speaks a line now. true = it started; the utterance carrying the message is recorded for the hearers.
bool speak(CAI_Stalker& npc, CommsState& state, const Message& message, u32 line, float volume, bool radioed);
// Someone else of the squad is talking (one talker at a time).
bool squad_talking(const CAI_Stalker& npc);
// An involuntary cry of a silent section from its base voice set (pain, death).
void cry(CAI_Stalker& npc, CommsState& state, u32 bark);
// Radio squelch and speech (or a PDA tone) at a carrier: a real sound within [ai_comms] radio_squelch_range.
void play_device(CAI_Stalker& owner, const CAI_Stalker* speaker, u32 line, bool pda);
// Loads the radio and PDA device sounds once per level (a first load of an .ogg took ~7 ms in a firefight).
void preload_device_sounds();
void update_sounds(CAI_Stalker& npc, CommsState& state);
void stop_sounds(u16 owner);
void clear_sounds();

// What one comms update spent starting sounds and describing spots (P10: p_max_comms_snd_us, p_max_comms_desc_us);
// reset when the update starts, read when it ends.
extern float g_sound_us;
extern float g_describe_us;
class PartTimer
{
    std::chrono::steady_clock::time_point m_start{std::chrono::steady_clock::now()};
    float& m_total;

public:
    explicit PartTimer(float& total) : m_total(total) {}
    ~PartTimer()
    {
        m_total += std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - m_start).count();
    }

    PartTimer(const PartTimer&) = delete;
    PartTimer& operator=(const PartTimer&) = delete;
};
} // namespace Internal
} // namespace Comms
} // namespace DaAi
