// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include <algorithm>
#include <cmath>

// Seasons (docs/dead-air/SEASONS.md). r__season chooses "calendar" (the computer's date) or one of the
// twelve months. The season day D in [0, 365) (0 = 1 January 00:00, a month's key is its 15th at noon)
// drives the grass, the snow and the stock details continuously, every frame, without a rebuild. The
// texture overlay folders (textures\seasons\<folder>\...) are latched per texture category at safe
// points (startup, level load, main menu) and reloaded by the renderer between frames.

// The overlay folder for the texture `name` (a path relative to textures\, either slash), chosen by
// the category the name belongs to ([season_categories] of dead_air_x64_seasons.ltx), or null when the
// base texture is worn. Latched: changes only at da_season_relatch().
ENGINE_API pcstr da_season_folder_for(pcstr name);
// The latched folder of the default category (terrain); null = base.
ENGINE_API pcstr da_season_folder();
// Changes whenever any category's latched folder changes (the renderer's reload test).
ENGINE_API u32 da_season_folders_stamp();
// D this frame and the previous frame (motion vectors of growing blades).
ENGINE_API float da_season_day();
ENGINE_API float da_season_day_prev();
// 0 = calendar, 1..12 = a month.
ENGINE_API u32 da_season_choice();
// A QA command (qa_season_year / qa_season_pin) drives D this frame.
ENGINE_API bool da_season_qa();
// Re-latch the texture folders from the current choice/date (safe points only).
ENGINE_API void da_season_relatch();
// xrEngine only: advances D once per frame (CRenderDevice::FrameMove, before seqFrame.Process()).
void da_season_frame();
// xrEngine only: r__season and the QA commands (CCC_Register, before user.ltx runs).
void da_season_register_commands();

// The 15th of each month at noon on the 365-day year: what a menu month shows, where the data keys sit.
inline constexpr float da_season_key_day[12] = {
    14.5f, 45.5f, 73.5f, 104.5f, 134.5f, 165.5f, 195.5f, 226.5f, 257.5f, 287.5f, 318.5f, 348.5f};

// One latched combination of texture overlay folders: a folder (null = base) per texture category of
// [season_categories], category 0 being the default. Immutable and alive until shutdown, so the pointer
// names the combination: the renderer serves one and reloads what differs when another is latched.
struct DaSeasonLatch
{
    static constexpr u32 max_categories = 16;
    u32 id = 0;                                // 1, 2, ... in the order the combinations were first latched
    pcstr folder[max_categories]{};            // interned: one name, one pointer
    char name[512]{};                          // "default=spring_early trees=winter ..." for the logs
};
// The latch the textures should wear now (never null; the first call reads the season data).
ENGINE_API const DaSeasonLatch* da_season_latch();
// The category of texture `name` (relative to textures\ or to the level folder, either slash, any case):
// the longest matching [season_categories] prefix, 0 when none matches. Thread-safe.
ENGINE_API u32 da_season_category(pcstr name);
// The overlay folder `latch` gives texture `name`; null = base.
inline pcstr da_season_latch_folder(const DaSeasonLatch* latch, pcstr name)
{
    return latch ? latch->folder[da_season_category(name)] : nullptr;
}

inline float da_season_wrap(float day) { return day - 365.f * std::floor(day / 365.f); }

// A periodic monotone cubic over the 365-day year through (day, value) keys (Fritsch-Butland tangents):
// equal neighbours give flat runs (winter zeros stay exactly zero, never negative), no overshoot. Keys
// may come in any order; build() sorts them. Colours: one curve per LINEAR rgb channel.
struct DaSeasonCurve
{
    static constexpr unsigned max_keys = 32;
    unsigned n = 0;
    float day[max_keys]{}, v[max_keys]{}, m[max_keys]{};

    void clear() { n = 0; }
    void add(float d, float value)
    {
        if (n < max_keys)
        {
            day[n] = da_season_wrap(d);
            v[n] = value;
            ++n;
        }
    }
    float span(unsigned k) const
    {
        const float h = n > 1 ? da_season_wrap(day[(k + 1) % n] - day[k]) : 365.f;
        return h > 1e-4f ? h : 365.f;
    }
    void build()
    {
        for (unsigned i = 1; i < n; ++i)
            for (unsigned j = i; j > 0 && day[j - 1] > day[j]; --j)
            {
                std::swap(day[j - 1], day[j]);
                std::swap(v[j - 1], v[j]);
            }
        for (unsigned k = 0; k < n; ++k)
        {
            const unsigned p = (k + n - 1) % n, q = (k + 1) % n;
            const float hp = span(p), hk = span(k);
            const float dp = (v[k] - v[p]) / hp, dk = (v[q] - v[k]) / hk;
            m[k] = (n < 3 || dp * dk <= 0.f) ? 0.f : 3.f * (hp + hk) / ((2.f * hk + hp) / dp + (hk + 2.f * hp) / dk);
        }
    }
    float at(float D) const
    {
        if (!n)
            return 0.f;
        if (n == 1)
            return v[0];
        const float x = da_season_wrap(D);
        unsigned k = n - 1;
        for (unsigned i = 0; i < n; ++i)
            if (day[i] <= x)
                k = i;
        const unsigned q = (k + 1) % n;
        const float h = span(k);
        const float s = std::min(std::max(da_season_wrap(x - day[k]) / h, 0.f), 1.f);
        const float s2 = s * s, s3 = s2 * s;
        return (2.f * s3 - 3.f * s2 + 1.f) * v[k] + (s3 - 2.f * s2 + s) * h * m[k] + (-2.f * s3 + 3.f * s2) * v[q] +
            (s3 - s2) * h * m[q];
    }
};
