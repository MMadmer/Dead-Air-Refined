// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "xms_screens.h"

namespace
{
using namespace XMS;

struct Candidate
{
    const Module* module{};
    const ScreenDecl* screen{};
    float weight{1.f};
};

xr_string s_forced[ScreenSurfaceCount];
XmsScreens::Choice s_menu;

bool listed(const xr_vector<xr_string>& list, pcstr name)
{
    return std::any_of(list.begin(), list.end(), [&](const xr_string& v) { return 0 == _stricmp(v.c_str(), name); });
}

bool has_layer(const ScreenDecl& d)
{
    return std::any_of(d.layers.begin(), d.layers.end(), [](const ScreenLayer& l) { return l.kind != ScreenLayer::Empty; });
}

bool shows(const ScreenDecl& d, ScreenSurface surface, pcstr level)
{
    if (!(d.show & (1u << surface)) || d.weight <= 0.f || !has_layer(d))
        return false;
    // the main menu belongs to no game and no level
    if (surface == ScreenMenu)
        return true;
    if (!d.modes.empty() && !std::any_of(d.modes.begin(), d.modes.end(), [](const xr_string& m) { return ModeActive(m.c_str()); }))
        return false;
    return d.levels.empty() || (level && level[0] && listed(d.levels, level));
}

// the last module of the load order that claims the surface has it
template <typename Claims>
const Module* claimant(Claims&& claims)
{
    const auto& modules = Modules();
    for (auto it = modules.rbegin(); it != modules.rend(); ++it)
        if (!it->disabled && claims(*it))
            return &*it;
    return nullptr;
}

template <typename Item>
const Item* weighted(const xr_vector<Item>& items)
{
    float total = 0.f;
    for (const Item& item : items)
        total += item.weight;
    if (items.empty() || total <= 0.f)
        return nullptr;
    float roll = ::Random.randF(total);
    for (const Item& item : items)
    {
        roll -= item.weight;
        if (roll <= 0.f)
            return &item;
    }
    return &items.back();
}

void add_track(XmsScreens::Choice& c, const TrackDecl& t)
{
    c.tracks.push_back({ t.name, t.weight, t.volume });
}

void music_for(XmsScreens::Choice& c, ScreenSurface surface)
{
    const u32 side = ScreensRules::Side(surface);
    c.game_music = surface != ScreenLoading;
    c.tracks.clear();

    const Module* only = claimant([&](const Module& m) { return m.screens.only_music[side]; });
    if (only)
    {
        c.game_music = false;
        c.next_track = only->screens.next_track[side];
        for (const TrackDecl& t : only->screens.tracks)
            if (t.play & (1u << surface))
                add_track(c, t);
        return;
    }

    if (c.screen && c.module)
    {
        switch (c.screen->music)
        {
        case ScreenDecl::MusicNone:
            c.game_music = false;
            return;
        case ScreenDecl::MusicVideo:
            c.game_music = false;
            c.video_sound = true;
            return;
        case ScreenDecl::MusicTracks:
            c.game_music = false;
            c.next_track = c.module->screens.next_track[side];
            for (const xr_string& id : c.screen->tracks)
                for (const TrackDecl& t : c.module->screens.tracks)
                    if (t.id == id)
                        add_track(c, t);
            return;
        default: break;
        }
    }

    for (const Module& m : Modules())
    {
        if (m.disabled)
            continue;
        for (const TrackDecl& t : m.screens.tracks)
            if (t.play & (1u << surface))
            {
                add_track(c, t);
                c.next_track |= m.screens.next_track[side];
            }
    }
}

bool forced_choice(XmsScreens::Choice& c, ScreenSurface surface)
{
    const xr_string& forced = s_forced[surface];
    if (forced.empty())
        return false;
    if (forced == "game")
        return true;
    const size_t dot = forced.rfind('.');
    if (dot == xr_string::npos)
        return false;
    const Module* m = FindModule(forced.substr(0, dot).c_str());
    if (!m || m->disabled)
        return false;
    const xr_string id = forced.substr(dot + 1);
    for (const ScreenDecl& d : m->screens.screens)
        if (d.id == id && has_layer(d))
        {
            c.module = m;
            c.screen = &d;
            return true;
        }
    return false;
}
} // namespace

bool XmsScreens::WaitsForLevel()
{
    if (!s_forced[ScreenLoading].empty())
        return false;
    for (const Module& m : Modules())
    {
        if (m.disabled)
            continue;
        for (const ScreenDecl& d : m.screens.screens)
            if ((d.show & (1u << ScreenLoading)) && has_layer(d) && (!d.levels.empty() || !d.modes.empty()))
                return true;
    }
    return false;
}

XmsScreens::Choice XmsScreens::Choose(ScreenSurface surface, pcstr level)
{
    Choice c;
    const u32 side = ScreensRules::Side(surface);
    if (!forced_choice(c, surface))
    {
        xr_vector<Candidate> pool;
        const auto add = [&](const Module& m) {
            for (const ScreenDecl& d : m.screens.screens)
                if (shows(d, surface, level))
                    pool.push_back({ &m, &d, d.weight });
        };
        // A claim counts only where the module has something to show: a screen limited to one level
        // leaves every other level to the rest.
        const Module* only = claimant([&](const Module& m) {
            return m.screens.only_screens[side] && std::any_of(m.screens.screens.begin(), m.screens.screens.end(),
                [&](const ScreenDecl& d) { return shows(d, surface, level); });
        });
        if (only)
            add(*only);
        else
        {
            pool.push_back({ nullptr, nullptr, 1.f });
            for (const Module& m : Modules())
                if (!m.disabled)
                    add(m);
        }
        if (const Candidate* picked = weighted(pool))
        {
            c.module = picked->module;
            c.screen = picked->screen;
        }
    }
    music_for(c, surface);
    return c;
}

XmsScreens::Choice& XmsScreens::CurrentMenu() { return s_menu; }

bool XmsScreens::Force(pcstr surface, pcstr what)
{
    ScreenSurface s;
    if (0 == xr_strcmp(surface, "loading"))
        s = ScreenLoading;
    else if (0 == xr_strcmp(surface, "menu"))
        s = ScreenMenu;
    else if (0 == xr_strcmp(surface, "pause"))
        s = ScreenPause;
    else
        return false;
    s_forced[s] = 0 == xr_strcmp(what, "off") ? "" : what;
    return true;
}

void XmsScreens::Describe()
{
    static constexpr pcstr names[ScreenSurfaceCount] = { "loading", "menu", "pause" };
    bool any = false;
    for (const Module& m : Modules())
    {
        if (m.disabled || (m.screens.screens.empty() && m.screens.tracks.empty()))
            continue;
        any = true;
        const ScreensRules& s = m.screens;
        Msg("* [%s] loading: screens %s, music %s; menu: screens %s, music %s", m.id.c_str(), s.only_screens[0] ? "only" : "mix",
            s.only_music[0] ? "only" : "mix", s.only_screens[1] ? "only" : "mix", s.only_music[1] ? "only" : "mix");
        for (const ScreenDecl& d : s.screens)
            Msg("*   screen %s: %u layer(s), shows on%s%s%s, weight %.2f", d.id.c_str(), u32(d.layers.size()),
                d.show & (1u << ScreenLoading) ? " loading" : "", d.show & (1u << ScreenMenu) ? " menu" : "",
                d.show & (1u << ScreenPause) ? " pause" : "", d.weight);
        for (const TrackDecl& t : s.tracks)
            Msg("*   track %s: %s, weight %.2f, volume %.2f", t.id.c_str(), t.name.c_str(), t.weight, t.volume);
    }
    if (!any)
        Msg("* XMS: no module brings screens or music");
    for (u32 i = 0; i < ScreenSurfaceCount; ++i)
    {
        const Choice c = Choose(ScreenSurface(i), nullptr);
        Msg("* %s now: %s.%s, %u track(s)%s%s", names[i], c.module ? c.module->id.c_str() : "game", c.screen ? c.screen->id.c_str() : "own",
            u32(c.tracks.size()), c.game_music ? " + the game's music" : "", c.video_sound ? " + the video's sound" : "");
        if (!s_forced[i].empty())
            Msg("*   forced: %s", s_forced[i].c_str());
    }
}
