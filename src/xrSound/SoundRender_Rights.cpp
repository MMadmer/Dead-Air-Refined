// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"

#include "xrCore/XMS/xms_assets.h"

#include <mutex>

// The streamer mode. One lookup decides for every sound, by its name: the sound's description (a module's
// .sndasset: `copyrighted`, `streamer`), else the game's list. The sound engine, the music thread and the scripts
// all ask it. Contract: docs/dead-air/MODDING.md, "Streamer mode".
XRSOUND_API int psSoundStreamerMode = 0;

namespace
{
constexpr pcstr kListFile = "dead_air_x64_sound_rights.ltx";
constexpr pcstr kListSection = "copyrighted";

struct ListLine
{
    xr_string mask;        // a name, or a mask where * stands for any run of characters
    xr_string replacement; // empty for none
};

struct Verdict
{
    bool copyrighted{false};
    xr_string replacement; // a sound that exists and is not copyrighted itself; empty for none
};

struct Rights
{
    xr_vector<ListLine> list;
    std::mutex lock;
    // remembered by name; a node's address stays while the map grows
    xr_unordered_map<xr_string, Verdict> verdicts;
};

Rights& rights()
{
    static Rights state;
    return state;
}

// The game's name of a sound: lower case, back slashes, no leading slash, no .ogg.
xr_string normal(pcstr name)
{
    xr_string out = name ? name : "";
    for (char& c : out)
        c = c == '/' ? '\\' : char(tolower(u8(c)));
    const size_t first = out.find_first_not_of(" \t\\");
    const size_t last = out.find_last_not_of(" \t");
    out = first == xr_string::npos ? xr_string() : out.substr(first, last - first + 1);
    if (out.size() > 4 && out.ends_with(".ogg"))
        out.resize(out.size() - 4);
    return out;
}

bool matches(pcstr mask, pcstr name)
{
    pcstr star = nullptr;
    pcstr resume = nullptr;
    while (*name)
    {
        if (*mask == '*')
        {
            star = mask++;
            resume = name;
        }
        else if (*mask == *name)
        {
            ++mask;
            ++name;
        }
        else if (star)
        {
            mask = star + 1;
            name = ++resume;
        }
        else
            return false;
    }
    while (*mask == '*')
        ++mask;
    return !*mask;
}

// Whether the sound is marked, and the replacement the one that marks it names.
bool marked(const xr_string& name, xr_string* replacement)
{
    if (const XMS::Assets::Sound* s = XMS::Assets::FindSound(name.c_str()); s && s->copyrighted)
    {
        if (replacement)
            *replacement = s->streamer;
        return true;
    }
    for (const ListLine& line : rights().list)
        if (matches(line.mask.c_str(), name.c_str()))
        {
            if (replacement)
                *replacement = line.replacement;
            return true;
        }
    return false;
}

const Verdict& verdict(pcstr raw)
{
    const xr_string name = normal(raw);
    Rights& r = rights();
    std::lock_guard guard(r.lock);
    if (const auto it = r.verdicts.find(name); it != r.verdicts.end())
        return it->second;

    Verdict v;
    xr_string replacement;
    v.copyrighted = !name.empty() && marked(name, &replacement);
    if (v.copyrighted && !replacement.empty())
    {
        // no chains: a replacement that needs one of its own is none
        string_path path;
        if (replacement == name || marked(replacement, nullptr))
            Msg("! [streamer] %s: its replacement %s is copyrighted itself - nothing plays in its place", name.c_str(),
                replacement.c_str());
        else if (!FS.exist(path, "$game_sounds$", replacement.c_str(), ".ogg"))
            Msg("! [streamer] %s: its replacement %s is no sound of the game - nothing plays in its place", name.c_str(),
                replacement.c_str());
        else
            v.replacement = std::move(replacement);
    }
    return r.verdicts.emplace(name, std::move(v)).first->second;
}
} // namespace

void snd_rights_read()
{
    Rights& r = rights();
    {
        std::lock_guard guard(r.lock);
        r.list.clear();
        r.verdicts.clear();
        string_path path;
        FS.update_path(path, "$game_config$", kListFile);
        if (FS.exist(path))
        {
            const CInifile ini(path, TRUE);
            if (ini.section_exist(kListSection))
                for (const auto& item : ini.r_section(kListSection).Data)
                    if (item.first.size())
                        r.list.push_back({ normal(item.first.c_str()), normal(item.second.c_str()) });
        }
    }
    u32 described = 0;
    for (const auto& [name, sound] : XMS::Assets::Sounds())
        described += sound.copyrighted ? 1 : 0;
    Msg("* [streamer] copyrighted: %zu line(s) of the game's list, %u sound(s) of modules by their descriptions",
        r.list.size(), described);
}

bool snd_copyrighted(pcstr name) { return verdict(name).copyrighted; }

xr_string snd_replacement(pcstr name) { return verdict(name).replacement; }

bool snd_playable(pcstr name)
{
    const Verdict& v = verdict(name);
    return !v.copyrighted || !v.replacement.empty();
}

// A source's word, worked out the first time the mode asks and kept on the source.
CSoundRender_Source* CSoundRender_Core::streamer_instead(CSoundRender_Source* own, bool& refused)
{
    SoundStreamerWord& word = own->streamer_word;
    u8 state = word.state.load(std::memory_order_acquire);
    if (state == SoundStreamerWord::Unasked)
    {
        const Verdict& v = verdict(own->file_name());
        CSoundRender_Source* instead = nullptr;
        if (v.copyrighted)
        {
            if (!v.replacement.empty())
                instead = i_create_source(v.replacement.c_str());
            if (instead)
                Msg("~ [streamer] %s: copyrighted - %s plays in its place", own->file_name(), v.replacement.c_str());
            else
                Msg("~ [streamer] %s: copyrighted - not played", own->file_name());
        }
        word.instead.store(instead, std::memory_order_relaxed);
        state = v.copyrighted ? SoundStreamerWord::Copyrighted : SoundStreamerWord::Free;
        word.state.store(state, std::memory_order_release);
    }
    const bool copyrighted = state == SoundStreamerWord::Copyrighted;
    CSoundRender_Source* instead = copyrighted ? word.instead.load(std::memory_order_relaxed) : nullptr;
    refused = copyrighted && !instead;
    return instead;
}

// Before a sound starts while the mode is on: false when it may not; a copyrighted one with a replacement gets the
// replacement in its place, with the caller's parameters, until it stops (the emitter's i_stop gives the sound back).
bool CSoundRender_Core::streamer_admits(ref_sound& S, bool& flat)
{
    auto* own = static_cast<CSoundRender_Source*>(S->handle);
    bool refused = false;
    CSoundRender_Source* instead = streamer_instead(own, refused);
    if (!instead && !refused)
        return true;
    // still playing: the mode came on since the last update
    if (S->feedback)
        S->feedback->stop(false);
    if (refused)
        return false;
    S->own_handle = own;
    S->own_bytes = S->dwBytesTotal;
    S->own_time = S->fTimeTotal;
    S->handle = instead;
    S->dwBytesTotal = instead->bytes_total();
    S->fTimeTotal = instead->length_sec();
    flat = flat || instead->channels_num() == 2;
    return true;
}

// The mode came on: what is playing and may not play stops. A replacement playing in a sound's place is free.
void CSoundRender_Core::streamer_stop_copyrighted()
{
    for (CSoundRender_Scene* scene : m_scenes)
        for (CSoundRender_Emitter* emitter : scene->get_emitters())
        {
            if (!emitter->isPlaying() || !emitter->owner_data)
                continue;
            bool refused = false;
            if (streamer_instead(emitter->source(), refused) || refused)
                emitter->stop(false);
        }
}
