// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "da_season.h"

#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>

namespace
{
constexpr pcstr season_file = "dead_air_x64_seasons.ltx";

// r__season, kept in user.ltx: 0 follows the computer's calendar, 1..12 show a month's 15th at noon.
u32 ps_r__season = 0;
const xr_token season_token[] = {{"calendar", 0}, {"january", 1}, {"february", 2}, {"march", 3}, {"april", 4},
    {"may", 5}, {"june", 6}, {"july", 7}, {"august", 8}, {"september", 9}, {"october", 10}, {"november", 11},
    {"december", 12}, {nullptr, 0}};

pcstr choice_name(u32 choice) { return choice <= 12 ? season_token[choice].name : "?"; }
pcstr month_name(u32 month) { return season_token[month + 1].name; }

double wrap_days(double day) { return day - 365.0 * std::floor(day / 365.0); }
// A day in [0, 365): rounding can land a wrapped value on 365 itself.
float day_of(double day)
{
    const float wrapped = float(wrap_days(day));
    return wrapped < 365.f ? wrapped : 0.f;
}
// The shorter way around the year from one day to another, in (-182.5, 182.5].
float arc_days(float delta) { return delta - 365.f * std::ceil(delta / 365.f - 0.5f); }

//------------------------------------------------------------------------------------------------------
// Texture categories and their folder bands ([season_categories], [season_folders])

// Without the file or one of its sections: the design's tables. The ground sets never wear winter (the
// snow layer whitens open ground, and painted snow would show under roofs and in trails); trees and the
// maps wear it only while the open cover is complete (15 Dec .. 15 Feb). Autumn is the base set ("-").
constexpr std::pair<pcstr, pcstr> builtin_categories[] = {{"terrain\\", "default"}, {"build_details", "default"},
    {"trees\\", "trees"}, {"map\\", "trees"}, {"ui\\", "trees"}, {"grnd\\", "ground"},
    {"detail\\", "ground"}, {"roof\\", "ground"}, {"decal\\", "decal"}, {"vine\\", "vine"}};
constexpr std::pair<pcstr, pcstr> builtin_folders[] = {
    {"default", "99:spring_mid, 120:spring_late, 151:summer, 262:-, 304:spring_early"},
    {"trees", "46:spring_early, 99:spring_mid, 120:spring_late, 151:summer, 262:-, 304:spring_early, 348:winter"},
    {"ground", "-"}, {"decal", "46:-, 348:winter"}, {"vine", "99:-, 304:winter"}};

struct Band
{
    float day;    // the band starts here and runs to the next one; the last runs into the first
    pcstr folder; // interned, null = base
};

struct Category
{
    shared_str name;
    xr_vector<Band> bands; // by day; none = base all year
    bool listed = false;   // has a [season_folders] line
};

struct Prefix
{
    xr_string text; // lower case, backslashes
    u32 category;
};

struct SeasonData
{
    float glide = 2.f;
    u32 default_choice = 0;
    xr_vector<Category> categories; // [0] = default
    xr_vector<Prefix> prefixes;     // longest first
    xr_vector<shared_str> folders;  // keeps the interned folder names alive
};

SeasonData s_data;
std::once_flag s_data_once;

char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

// A folder name becomes a path component: plain letters, digits and underscores only.
bool folder_name_ok(pcstr name)
{
    if (!name[0] || xr_strlen(name) > 63)
        return false;
    for (pcstr c = name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
            return false;
    return true;
}

u32 category_index(SeasonData& data, pcstr name)
{
    string64 lowered;
    size_t length = 0;
    for (; name[length] && length + 1 < sizeof lowered; ++length)
        lowered[length] = lower_ascii(name[length]);
    lowered[length] = 0;
    for (u32 i = 0; i < data.categories.size(); ++i)
        if (!xr_strcmp(data.categories[i].name.c_str(), lowered))
            return i;
    if (data.categories.size() >= DaSeasonLatch::max_categories)
    {
        Msg("! [season] %s: more than %u texture categories, '%s' falls back to default", season_file,
            DaSeasonLatch::max_categories, lowered);
        return 0;
    }
    data.categories.push_back({shared_str(lowered), {}, false});
    return u32(data.categories.size() - 1);
}

pcstr intern_folder(SeasonData& data, pcstr folder)
{
    const shared_str name(folder);
    for (const shared_str& known : data.folders)
        if (known == name)
            return known.c_str();
    data.folders.push_back(name);
    return data.folders.back().c_str();
}

void add_prefix(SeasonData& data, pcstr prefix, pcstr category)
{
    if (!prefix || !prefix[0] || !category || !category[0])
    {
        Msg("! [season] %s [season_categories]: a line without a prefix or a category", season_file);
        return;
    }
    xr_string text = prefix;
    for (char& c : text)
        c = c == '/' ? '\\' : lower_ascii(c);
    const u32 index = category_index(data, category);
    for (Prefix& known : data.prefixes)
        if (known.text == text)
        {
            known.category = index;
            return;
        }
    data.prefixes.push_back({std::move(text), index});
}

// "<day>:<folder>, ..." with '-' for the base set; a lone item without a day holds all year.
void set_bands(SeasonData& data, pcstr category, pcstr list)
{
    if (!category || !category[0])
        return;
    Category& target = data.categories[category_index(data, category)];
    target.listed = true;
    target.bands.clear();
    const int count = list ? _GetItemCount(list) : 0;
    for (int i = 0; i < count; ++i)
    {
        string256 item;
        _GetItem(list, i, item, sizeof item);
        if (!item[0])
            continue;
        float day = 0.f;
        pstr folder = item;
        if (pstr colon = strchr(item, ':'))
        {
            *colon = 0;
            pstr end = nullptr;
            day = strtof(item, &end);
            if (end == item || !std::isfinite(day))
            {
                Msg("! [season] %s [season_folders] %s: '%s' is not a day", season_file, category, item);
                continue;
            }
            folder = colon + 1;
        }
        _Trim(folder);
        for (pstr c = folder; *c; ++c)
            *c = lower_ascii(*c);
        pcstr interned = nullptr;
        if (xr_strcmp(folder, "-"))
        {
            if (!folder_name_ok(folder))
            {
                Msg("! [season] %s [season_folders] %s: '%s' is not a folder name", season_file, category, folder);
                continue;
            }
            interned = intern_folder(data, folder);
        }
        target.bands.push_back({day_of(day), interned});
    }
    std::stable_sort(target.bands.begin(), target.bands.end(),
        [](const Band& a, const Band& b) { return a.day < b.day; });
}

void parse_season_data(const CInifile* ini)
{
    SeasonData& data = s_data;
    if (ini && ini->section_exist("season"))
    {
        if (ini->line_exist("season", "glide"))
            data.glide = std::clamp(ini->r_float("season", "glide"), 0.f, 30.f);
        if (ini->line_exist("season", "default"))
        {
            pcstr token = ini->r_string("season", "default");
            bool found = false;
            for (const xr_token* t = season_token; t->name && !found; ++t)
                if (token && !xr_stricmp(t->name, token))
                {
                    data.default_choice = u32(t->id);
                    found = true;
                }
            if (!found)
                Msg("! [season] %s [season] default = %s: not a r__season token, calendar holds", season_file, token);
        }
    }

    if (ini && ini->section_exist("season_categories"))
    {
        for (const auto& item : ini->r_section("season_categories").Data)
            add_prefix(data, item.first.c_str(), item.second.c_str());
    }
    else
    {
        for (const auto& [prefix, category] : builtin_categories)
            add_prefix(data, prefix, category);
    }

    if (ini && ini->section_exist("season_folders"))
    {
        for (const auto& item : ini->r_section("season_folders").Data)
            set_bands(data, item.first.c_str(), item.second.c_str());
    }
    else
    {
        for (const auto& [category, list] : builtin_folders)
            set_bands(data, category, list);
    }

    for (const Category& category : data.categories)
        if (!category.listed)
            Msg("! [season] %s: category '%s' has no [season_folders] line, it keeps the base textures", season_file,
                category.name.c_str());
    std::stable_sort(data.prefixes.begin(), data.prefixes.end(),
        [](const Prefix& a, const Prefix& b) { return a.text.size() > b.text.size(); });
    Msg("* [season] %zu texture categories, %zu prefixes, glide %.1f s, default %s", data.categories.size(),
        data.prefixes.size(), data.glide, choice_name(data.default_choice));
}

void load_season_data()
{
    s_data.categories.push_back({shared_str("default"), {}, false});
    if (!FS.exist("$game_config$", season_file))
    {
        Msg("! [season] %s is missing: the built-in season tables hold", season_file);
        parse_season_data(nullptr);
        return;
    }
    string_path path;
    FS.update_path(path, "$game_config$", season_file);
    const CInifile ini(path, TRUE);
    parse_season_data(&ini);
}

const SeasonData& season_data()
{
    std::call_once(s_data_once, load_season_data);
    return s_data;
}

bool name_has_prefix(pcstr name, const xr_string& prefix)
{
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        const char c = name[i];
        if (!c || (c == '/' ? '\\' : lower_ascii(c)) != prefix[i])
            return false;
    }
    return true;
}

pcstr folder_at(const Category& category, float day)
{
    if (category.bands.empty())
        return nullptr;
    const Band* band = &category.bands.back();
    for (const Band& candidate : category.bands)
        if (candidate.day <= day)
            band = &candidate;
    return band->folder;
}

//------------------------------------------------------------------------------------------------------
// The day

// Console, frame and the renderer's safe points: everything below is theirs under this lock.
std::mutex s_lock;

struct Glide
{
    bool active = false;
    float from = 0.f;    // the day it left
    float delta = 0.f;   // days along the shorter arc to the target it aimed at
    float target0 = 0.f; // that target (the calendar keeps moving under a glide)
    float elapsed = 0.f; // s of a rendered level
};

struct YearRun
{
    bool active = false;
    bool loop = false;
    float seconds = 60.f; // wall clock per year
    double from = 0.0;    // start day
    double elapsed = 0.0; // s of a rendered level
    u32 laps = 0;
};

Glide s_glide;
YearRun s_year;
float s_pin = -1.f;  // a pinned day, < 0 = none
bool s_snap = true;  // the next frame reports no motion (startup, level load, a jump)

std::atomic<float> s_day{0.f};
std::atomic<float> s_day_prev{0.f};
std::atomic<bool> s_qa{false};

// The latches handed out, alive until shutdown; the renderer keeps the pointer it serves.
std::mutex s_latch_lock;
xr_vector<std::unique_ptr<DaSeasonLatch>> s_latches;
std::atomic<const DaSeasonLatch*> s_latch{};

// The computer's local date and time of day on the 365-day year; a leap year is squeezed into it.
// Read once per second.
float calendar_day(tm* out = nullptr)
{
    static time_t cached_second = -1;
    static tm cached_tm{};
    static float cached_day = 0.f;
    const time_t now = time(nullptr);
    if (now != cached_second)
    {
        cached_second = now;
#if defined(XR_PLATFORM_WINDOWS)
        localtime_s(&cached_tm, &now);
#else
        localtime_r(&now, &cached_tm);
#endif
        const int year = cached_tm.tm_year + 1900;
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        const double seconds = cached_tm.tm_hour * 3600.0 + cached_tm.tm_min * 60.0 + cached_tm.tm_sec;
        cached_day = day_of(365.0 * (cached_tm.tm_yday + seconds / 86400.0) / (leap ? 366.0 : 365.0));
    }
    if (out)
        *out = cached_tm;
    return cached_day;
}

// What the choice shows: the calendar's day, or the month's 15th at noon.
float choice_day()
{
    const u32 choice = ps_r__season;
    return choice >= 1 && choice <= 12 ? da_season_key_day[choice - 1] : calendar_day();
}

// The day the texture folders follow: a pin's, else the choice's. A year run never moves them.
float latch_day() { return s_pin >= 0.f ? s_pin : choice_day(); }

bool level_rendered() { return g_pGameLevel && g_pGamePersistent && !g_pGamePersistent->IsMainMenuActive(); }

const DaSeasonLatch* intern_latch(float day)
{
    const SeasonData& data = season_data();
    DaSeasonLatch probe;
    for (u32 c = 0; c < data.categories.size(); ++c)
        probe.folder[c] = folder_at(data.categories[c], day);

    std::lock_guard guard(s_latch_lock);
    for (const auto& latch : s_latches)
        if (std::equal(std::begin(probe.folder), std::end(probe.folder), std::begin(latch->folder)))
            return latch.get();
    auto latch = std::make_unique<DaSeasonLatch>(probe);
    latch->id = u32(s_latches.size()) + 1;
    for (u32 c = 0; c < data.categories.size(); ++c)
    {
        string256 part;
        xr_sprintf(part, "%s%s=%s", c ? " " : "", data.categories[c].name.c_str(), probe.folder[c] ? probe.folder[c] : "-");
        if (xr_strlen(latch->name) + xr_strlen(part) + 4 >= sizeof latch->name)
        {
            xr_strcat(latch->name, " ...");
            break;
        }
        xr_strcat(latch->name, part);
    }
    s_latches.push_back(std::move(latch));
    return s_latches.back().get();
}

void latch_for(float day, pcstr why)
{
    const DaSeasonLatch* latch = intern_latch(day);
    if (s_latch.exchange(latch, std::memory_order_acq_rel) != latch)
        Msg("* [season] textures #%u for day %.1f (%s): %s", latch->id, day, why, latch->name);
}

void snap_to(float day)
{
    s_glide.active = false;
    s_day.store(day, std::memory_order_relaxed);
    s_day_prev.store(day, std::memory_order_relaxed);
    s_snap = true;
}

// Hands D back to the choice from `from`: along the shorter arc of the year over [season] glide seconds
// of a rendered level; at once while no level exists, since nobody sees it then.
void glide_to_choice(float from)
{
    const float target = choice_day();
    if (!g_pGameLevel || season_data().glide <= 0.f)
    {
        snap_to(target);
        return;
    }
    s_glide = {true, from, arc_days(target - from), target, 0.f};
}

void year_report(double before, double after)
{
    const YearRun& run = s_year;
    const double span = 365.0 * (after - before) / run.seconds;
    const double start = run.from + 365.0 * before / run.seconds;
    // The keys this frame passed, in the order the run met them. A key on a frame's end belongs to that
    // frame, so one sitting on the run's start is met at the end of the lap.
    constexpr double eps = 1e-6;
    std::pair<double, u32> crossed[12];
    u32 count = 0;
    for (u32 k = 0; k < 12; ++k)
    {
        double ahead = wrap_days(double(da_season_key_day[k]) - start);
        if (ahead <= eps || ahead >= 365.0 - eps)
            ahead = 365.0;
        if (ahead <= span + eps)
            crossed[count++] = {ahead, k};
    }
    std::sort(crossed, crossed + count);
    for (u32 i = 0; i < count; ++i)
    {
        const auto [ahead, k] = crossed[i];
        Msg("[qa] season_year %s day %.1f at %.2f s", month_name(k), da_season_key_day[k],
            before + ahead / 365.0 * run.seconds);
    }
}

void year_finish(pcstr how)
{
    Msg("[qa] season_year_done %.2f s%s", s_year.elapsed, how);
    s_year.active = false;
    glide_to_choice(s_day.load(std::memory_order_relaxed));
}

float year_advance(float dt)
{
    YearRun& run = s_year;
    const double before = run.elapsed;
    double after = before + dt;
    const bool done = !run.loop && after >= run.seconds;
    if (done)
        after = run.seconds;
    run.elapsed = after;
    if (after > before)
        year_report(before, after);
    if (run.loop)
    {
        const u32 lap = u32(after / run.seconds);
        if (lap > run.laps)
        {
            run.laps = lap;
            Msg("[qa] season_year lap %u at %.2f s", lap, after);
        }
    }
    const float day = day_of(run.from + 365.0 * after / run.seconds);
    if (done)
    {
        s_day.store(day, std::memory_order_relaxed);
        year_finish("");
        // The hand-back may have snapped (no level); otherwise the glide starts from here next frame.
        return s_glide.active ? day : s_day.load(std::memory_order_relaxed);
    }
    return day;
}

void year_stop(pcstr how)
{
    if (s_year.active)
        year_finish(how);
}

bool parse_float(pcstr text, float& value)
{
    pstr end = nullptr;
    const float parsed = strtof(text, &end);
    if (end == text || *end || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

// A day for the QA commands: a r__season token (calendar = today, a month = its 15th) or 0..365.
bool parse_day(pcstr text, float& day)
{
    for (const xr_token* t = season_token; t->name; ++t)
        if (!xr_stricmp(t->name, text))
        {
            day = t->id ? da_season_key_day[t->id - 1] : calendar_day();
            return true;
        }
    float value;
    if (!parse_float(text, value) || value < 0.f || value > 365.f)
        return false;
    day = day_of(value);
    return true;
}

void on_choice(u32 was)
{
    std::lock_guard guard(s_lock);
    Msg("* [season] r__season %s -> %s", choice_name(was), choice_name(ps_r__season));
    // The player chose: the folders follow at once (the reload is expected now, mostly behind a menu).
    latch_for(latch_day(), "choice");
    if (!s_year.active && s_pin < 0.f)
        glide_to_choice(s_day.load(std::memory_order_relaxed));
}

//------------------------------------------------------------------------------------------------------
// Console

class CCC_Season final : public CCC_Token
{
public:
    CCC_Season(pcstr name) : CCC_Token(name, &ps_r__season, season_token) {}

    void Execute(pcstr args) override
    {
        // 1.6.0 development builds kept a look, not a month: each goes to the month in its middle. Autumn
        // was the untouched default (a token is always saved), so it cannot be told from a pick.
        static constexpr std::pair<pcstr, pcstr> legacy[] = {{"autumn", "calendar"}, {"winter", "january"},
            {"spring_early", "march"}, {"spring", "april"}, {"spring_late", "may"}, {"summer", "july"}};
        for (const auto& [old, now] : legacy)
            if (!xr_stricmp(args, old))
            {
                args = now;
                break;
            }
        const u32 was = *value;
        CCC_Token::Execute(args);
        if (*value != was)
            on_choice(was);
    }
};

// qa_season_year [seconds=60] [from=<month|day>] [loop] | stop: the season day runs through the year in
// that many seconds of a rendered level (wall clock: time_factor, game time and weather stay as they are).
class CCC_QaSeasonYear final : public IConsole_Command
{
public:
    CCC_QaSeasonYear(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        std::lock_guard guard(s_lock);
        float seconds = 60.f;
        float from = s_day.load(std::memory_order_relaxed);
        bool jump = false, loop = false;
        const int count = args ? _GetItemCount(args, ' ') : 0;
        for (int i = 0; i < count; ++i)
        {
            string128 item;
            _GetItem(args, i, item, sizeof item, ' ');
            if (!item[0])
                continue;
            if (!xr_stricmp(item, "stop"))
            {
                if (s_year.active)
                    year_stop(" (stopped)");
                else
                    Msg("[qa] season_year is not running");
                return;
            }
            if (!xr_stricmp(item, "loop"))
                loop = true;
            else if (!strncmp(item, "seconds=", 8) ? parse_float(item + 8, seconds) : parse_float(item, seconds))
                continue;
            else if (!strncmp(item, "from=", 5) ? parse_day(item + 5, from) : parse_day(item, from))
                jump = true;
            else
            {
                Msg("! qa_season_year: '%s' is neither seconds, from=<calendar|month|0..365>, loop nor stop", item);
                return;
            }
        }
        seconds = std::clamp(seconds, 1.f, 3600.f);
        year_stop(" (restarted)");
        if (s_pin >= 0.f)
        {
            s_pin = -1.f;
            latch_for(latch_day(), "qa_season_year");
        }
        s_glide.active = false;
        s_year = {true, loop, seconds, double(from), 0.0, 0};
        if (jump)
            snap_to(from);
        Msg("[qa] season_year start: %.1f s from day %.1f%s", seconds, from, loop ? ", loop" : "");
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "qa_season_year [seconds=60] [from=<month|day>] [loop] | stop - run the season year in wall seconds");
    }
};

// qa_season_pin <0..365 | calendar | january..december | off>: holds the season day (screenshots, frame
// parity) and latches the texture folders to it; off hands both back to r__season.
class CCC_QaSeasonPin final : public IConsole_Command
{
public:
    CCC_QaSeasonPin(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        std::lock_guard guard(s_lock);
        float day = 0.f;
        if (!args || !args[0] || !xr_stricmp(args, "off") || !xr_strcmp(args, "-1"))
        {
            if (s_pin >= 0.f)
            {
                s_pin = -1.f;
                latch_for(latch_day(), "qa_season_pin off");
                glide_to_choice(s_day.load(std::memory_order_relaxed));
            }
            Msg("[qa] season_pin off");
            return;
        }
        if (!parse_day(args, day))
        {
            Msg("! qa_season_pin: '%s' is neither 0..365, calendar, a month nor off", args);
            return;
        }
        year_stop(" (pinned)");
        s_pin = day;
        snap_to(day);
        latch_for(day, "qa_season_pin");
        Msg("[qa] season_pin day %.1f", day);
    }

    void Info(TInfo& I) override { xr_strcpy(I, "qa_season_pin <0..365|calendar|january..december|off> - hold the season day"); }
};

class CCC_QaSeasonState final : public IConsole_Command
{
public:
    CCC_QaSeasonState(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr) override
    {
        std::lock_guard guard(s_lock);
        tm now{};
        const float calendar = calendar_day(&now);
        Msg("[qa] season_state choice %s, pc %04d-%02d-%02d %02d:%02d:%02d = day %.2f, target %.2f, day %.3f, prev %.3f",
            choice_name(ps_r__season), now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min,
            now.tm_sec, calendar, choice_day(), s_day.load(std::memory_order_relaxed),
            s_day_prev.load(std::memory_order_relaxed));
        if (s_glide.active)
            Msg("[qa] season_state glide %.2f of %.1f s from day %.2f by %+.2f days", s_glide.elapsed, season_data().glide,
                s_glide.from, s_glide.delta);
        else
            Msg("[qa] season_state glide off");
        if (s_year.active)
            Msg("[qa] season_state year run %.2f of %.1f s from day %.1f%s", s_year.elapsed, s_year.seconds, s_year.from,
                s_year.loop ? ", loop" : "");
        else if (s_pin >= 0.f)
            Msg("[qa] season_state pinned at day %.1f", s_pin);
        else
            Msg("[qa] season_state qa off");
        if (const DaSeasonLatch* latch = s_latch.load(std::memory_order_acquire))
            Msg("[qa] season_state textures #%u for day %.1f: %s", latch->id, latch_day(), latch->name);
    }

    void Info(TInfo& I) override { xr_strcpy(I, "qa_season_state - print the season day, the choice and the texture folders"); }
};
} // namespace

pcstr da_season_folder_for(pcstr name) { return da_season_latch_folder(da_season_latch(), name); }
pcstr da_season_folder() { return da_season_latch()->folder[0]; }
u32 da_season_folders_stamp() { return da_season_latch()->id; }
float da_season_day() { return s_day.load(std::memory_order_relaxed); }
float da_season_day_prev() { return s_day_prev.load(std::memory_order_relaxed); }
u32 da_season_choice() { return ps_r__season; }
bool da_season_qa() { return s_qa.load(std::memory_order_relaxed); }

const DaSeasonLatch* da_season_latch()
{
    if (const DaSeasonLatch* latch = s_latch.load(std::memory_order_acquire))
        return latch;
    std::lock_guard guard(s_lock);
    if (!s_latch.load(std::memory_order_acquire))
        latch_for(latch_day(), "startup");
    return s_latch.load(std::memory_order_acquire);
}

u32 da_season_category(pcstr name)
{
    if (!name)
        return 0;
    for (const Prefix& prefix : season_data().prefixes)
        if (name_has_prefix(name, prefix.text))
            return prefix.category;
    return 0;
}

void da_season_relatch()
{
    std::lock_guard guard(s_lock);
    latch_for(latch_day(), "safe point");
    // Startup and a level load show the target at once.
    if (!s_year.active && s_pin < 0.f)
        snap_to(choice_day());
}

void da_season_frame()
{
    std::lock_guard guard(s_lock);
    const float last = s_day.load(std::memory_order_relaxed);
    const bool rendered = level_rendered();
    // Wall clock of a rendered level: the glide and the QA run wait behind a menu.
    const float dt = rendered ? Device.fTimeDeltaUnscaled : 0.f;
    const float target = choice_day();
    float day = target;
    if (s_year.active)
        day = year_advance(dt);
    else if (s_pin >= 0.f)
        day = s_pin;
    else if (s_glide.active)
    {
        if (!g_pGameLevel)
        {
            s_glide.active = false;
            s_snap = true;
        }
        else
        {
            s_glide.elapsed += dt;
            const float glide = season_data().glide;
            const float t = glide > 0.f ? std::clamp(s_glide.elapsed / glide, 0.f, 1.f) : 1.f;
            const float eased = t * t * (3.f - 2.f * t);
            const float delta = s_glide.delta + arc_days(target - s_glide.target0);
            day = day_of(s_glide.from + delta * eased);
            if (t >= 1.f)
            {
                s_glide.active = false;
                day = target;
            }
        }
    }
    else if (!g_pGameLevel)
        s_snap = true;

    s_day_prev.store(s_snap ? day : last, std::memory_order_relaxed);
    s_day.store(day, std::memory_order_relaxed);
    s_snap = false;
    s_qa.store(s_year.active || s_pin >= 0.f, std::memory_order_relaxed);

    // Safe point: no level, or a menu over it. The calendar's folders may move here, never in play.
    if (!rendered)
        latch_for(latch_day(), "menu");
}

void da_season_register_commands()
{
    ps_r__season = season_data().default_choice;
    {
        std::lock_guard guard(s_lock);
        snap_to(choice_day());
    }
    // Here and not in the renderer: the choice outlives a renderer switch and is read on both sides.
    CMD1(CCC_Season, "r__season");
    CMD1(CCC_QaSeasonYear, "qa_season_year");
    CMD1(CCC_QaSeasonPin, "qa_season_pin");
    CMD1(CCC_QaSeasonState, "qa_season_state");
}
