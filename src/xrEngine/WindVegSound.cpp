#include "stdafx.h"

#include "WindVegSound.h"
#include "Environment.h"
#include "IGame_Persistent.h"
#include "IGame_Level.h"
#include "xrMaterialSystem/GameMtlLib.h"

namespace
{
// Type voicing: grass whispers high and quiet, bushes sit in the middle, trees roar low.
// Calibrated twice in the field: the first set drowned under rain ambience, the raised set
// was clearly audible even in near-calm - this is that set divided by ~1.7, with the volume
// floor lowered too, so calm-weather rustle is a barely-there whisper.
const float k_volume[] = {0.26f, 0.50f, 0.78f};
// Mild per-type pitch shifts only: grass and canopy now carry their OWN recordings, so the
// heavy shifts that faked three plants out of one bush sound are no longer needed (and they
// still sit right when a missing asset falls back to the bush set).
const float k_pitch[] = {1.05f, 1.00f, 0.94f};
// Audible ranges by type, replacing the couple-of-metres range baked into the source file
// (the walk-through-bush collide sound). Matched to how far these are really heard: a big
// canopy in wind carries the better part of a hundred metres - it is one of the
// longest-carrying natural sounds - a bush tens of metres, grass only nearby. min is the
// full-volume core (a canopy is a WIDE source, not a point).
const float k_range_min[] = {1.f, 2.f, 4.f};
const float k_range_max[] = {22.f, 40.f, 90.f};
// The local wind strength a spot needs before it can rustle at all. (First calibration sat
// above what a storm actually produced after the field multiplies in - the world went mute.)
// Trigger floor: low on purpose - a light breeze already whispers. LOUDNESS carries the
// wind strength (see play_one): near the floor the rustle is barely audible, a gale is
// full volume, and a true calm fires nothing at all.
constexpr float k_threshold = 0.10f;
// Global polyphony cap across all types - the anti-cacophony valve.
constexpr u32 k_max_active = 5;
} // namespace

void CEffect_WindVeg::lazy_init()
{
    if (m_inited)
        return;
    m_inited = true;

    // Bush rustle (and the fallback for everything) comes from the actor-through-vegetation
    // collide pair in the material library - the exact sound the player already hears walking
    // through a bush. Search by substring so any data setup of the mod resolves without
    // hardcoded ids.
    xr_vector<shared_str> mtl_files;
    int idx_actor = -1, idx_veg = -1;
    const auto& mtls = GMLib.Materials();
    for (u32 i = 0; i < mtls.size(); ++i)
    {
        if (!mtls[i])
            continue;
        cpcstr name = mtls[i]->m_Name.c_str();
        if (idx_actor < 0 && strstr(name, "actor"))
            idx_actor = int(i);
        if (idx_veg < 0 && (strstr(name, "bush") || strstr(name, "grass")))
            idx_veg = int(i);
    }

    if (idx_actor >= 0 && idx_veg >= 0)
    {
        if (SGameMtlPair* pair = GMLib.GetMaterialPairByIndices(u16(idx_actor), u16(idx_veg)))
        {
            for (const ref_sound& s : pair->CollideSounds)
            {
                if (s._handle())
                    mtl_files.push_back(s._handle()->file_name());
            }
        }
    }

    // Dedicated per-type sounds: grass hiss and canopy leaves. A quick existence probe keeps
    // a missing/renamed asset from silencing the layer - it just falls back to the bush set.
    const auto probe = [](pcstr name) -> bool {
        ref_sound test;
        test.create(name, st_Effect, sg_SourceType);
        const bool ok = !!test._handle();
        test.destroy();
        return ok;
    };

    constexpr pcstr k_grass_file = "dead_air_x64\\grass_rustle";
    constexpr pcstr k_leaves_file = "dead_air_x64\\leaves_rustle";
    if (probe(k_grass_file))
        m_files[type_grass].emplace_back(k_grass_file);
    if (probe(k_leaves_file))
        m_files[type_tree].emplace_back(k_leaves_file);
    m_files[type_bush] = mtl_files;
    // Fallbacks: dedicated files missing -> bush set; no bush set either -> that type is mute.
    if (m_files[type_grass].empty())
        m_files[type_grass] = mtl_files;
    if (m_files[type_tree].empty())
        m_files[type_tree] = mtl_files;

    bool any = false;
    for (int t = 0; t < type_count; ++t)
    {
        if (m_files[t].empty())
            continue;
        any = true;
        for (auto& v : m_voices[t])
        {
            const shared_str& f = m_files[t][size_t(&v - m_voices[t]) % m_files[t].size()];
            v.snd.create(f.c_str(), st_Effect, sg_SourceType);
        }
    }

    if (!any)
    {
        Msg("! [wind-veg] no rustle sources at all (assets and material pair both missing) - disabled");
        return;
    }
    m_sound_ok = true;
    Msg("* [wind-veg] rustle pools ready: grass=%u bush=%u tree=%u file(s)", u32(m_files[type_grass].size()),
        u32(m_files[type_bush].size()), u32(m_files[type_tree].size()));
}

bool CEffect_WindVeg::play_one(int type, const Fvector& pos, float strength)
{
    // Global cap first: trees may steal the last voice from grass, never the other way round.
    u32 active = 0;
    for (int t = 0; t < type_count; ++t)
        for (auto& v : m_voices[t])
            if (Device.fTimeGlobal < v.busy_until)
                ++active;
    if (active >= k_max_active)
        return false;

    for (auto& v : m_voices[type])
    {
        if (Device.fTimeGlobal >= v.busy_until)
        {
            v.snd.play_at_pos(nullptr, pos, 0);
            // Volume follows the wind, not a fixed floor: the old clamp(…, 0.12, 1) made the
            // quietest possible rustle clearly audible in near-calm. The curve starts at
            // near-zero just above the trigger floor and eases up to full in a gale.
            const float vol =
                k_volume[type] * powf(clampr((strength - 0.08f) / 0.72f, 0.f, 1.f), 1.4f);
            v.snd.set_volume(vol);
            v.snd.set_frequency(k_pitch[type] * ::Random.randF(0.92f, 1.08f));
            // The source file is the walk-through-bush collide sound, whose baked audible
            // range is a couple of metres - played from a canopy 30 m away it was silently
            // culled ("no rustle from anything"). Per-type realistic ranges above.
            v.snd.set_range(k_range_min[type], k_range_max[type]);
            const float len = v.snd._handle() ? v.snd._handle()->length_sec() : 1.f;
            v.busy_until = Device.fTimeGlobal + len;
            return true;
        }
    }
    return false;
}

void CEffect_WindVeg::OnFrame()
{
    if (!g_pGameLevel || !g_pGamePersistent)
        return;
    if (Device.fTimeGlobal < m_next_think)
        return;
    m_next_think = Device.fTimeGlobal + 0.25f;

    lazy_init();
    if (!m_sound_ok)
        return;

    auto& env = g_pGamePersistent->Environment();
    const Fvector cam = Device.vCameraPosition;
    const float wind = env.eff_wind_norm;

    // ---- Trees: the loudest and the most located layer. -----------------------------------
    // Nearest-trees subset refreshes on a slow timer; per-tree cooldowns keep one oak from
    // machine-gunning its rustle while a long tongue sits on it.
    const auto& trees = env.wind_veg_trees;
    if (!trees.empty())
    {
        if (Device.fTimeGlobal >= m_next_tree_sort || m_near_trees.empty())
        {
            m_next_tree_sort = Device.fTimeGlobal + 2.f;
            m_near_trees.clear();
            for (u32 i = 0; i < trees.size(); ++i)
                if (trees[i].distance_to_sqr(cam) < 70.f * 70.f)
                    m_near_trees.push_back(i);
            if (m_near_trees.size() > 24)
            {
                std::sort(m_near_trees.begin(), m_near_trees.end(), [&](u32 a, u32 b) {
                    return trees[a].distance_to_sqr(cam) < trees[b].distance_to_sqr(cam);
                });
                m_near_trees.resize(24);
            }
            m_tree_cool.resize(trees.size(), 0.f);
        }

        for (const u32 ti : m_near_trees)
        {
            const Fvector& tp = trees[ti];
            // Wind field + wind motors: a blast ring passing through this crown makes it
            // rustle even on a windless day - the sound rides the visible bend, and a strong
            // enough blast punches through the retrigger cooldown.
            const float motors = env.SampleWindMotors(tp);
            if (Device.fTimeGlobal < m_tree_cool[ti] && motors < 0.3f)
                continue;
            const float local = wind * env.SampleWindField(tp.x, tp.z) + motors;
            if (local < k_threshold)
                continue;
            // A stochastic gate on top of the field keeps simultaneous fronts from firing
            // every crown at once - but it opens with the wind: in a gale the canopy MUST
            // be heard, silence there reads as a bug, not as restraint.
            if (::Random.randF() > 0.35f + 0.65f * clampr(local, 0.f, 1.f))
                continue;
            Fvector crown = tp;
            crown.y += 4.f; // the rustle lives in the canopy, not at the root
            if (play_one(type_tree, crown, local))
                m_tree_cool[ti] = Device.fTimeGlobal + ::Random.randF(4.f, 9.f);
        }
    }

    // ---- Grass and bushes: eight azimuth sectors around the listener. ---------------------
    // Gate on the published green density so bare concrete yards stay silent; the field
    // decides which sector speaks. Bush voice at ground level, grass slightly quieter still.
    if (env.wind_veg_green > 0.10f)
    {
        for (u32 s = 0; s < 8; ++s)
        {
            const float ang = float(s) * (PI_MUL_2 / 8.f) + ::Random.randF(-0.2f, 0.2f);
            const float dist = ::Random.randF(6.f, 16.f);
            Fvector p;
            p.set(cam.x + _sin(ang) * dist, cam.y + 0.4f, cam.z + _cos(ang) * dist);
            // Motors count here too: a grenade going off in the grass makes the grass answer,
            // cooldown or not.
            const float motors = env.SampleWindMotors(p);
            if (Device.fTimeGlobal < m_sector_cool[s] && motors < 0.3f)
                continue;
            const float local = (wind * env.SampleWindField(p.x, p.z) + motors) * env.wind_veg_green;
            if (local < k_threshold)
                continue;
            // Same wind-opened gate as the trees: strong wind guarantees ground rustle.
            if (::Random.randF() > 0.30f + 0.70f * clampr(local, 0.f, 1.f))
                continue;
            const int type = (::Random.randF() < 0.6f) ? type_bush : type_grass;
            if (play_one(type, p, local))
                m_sector_cool[s] = Device.fTimeGlobal + ::Random.randF(3.f, 7.f);
        }
    }
}

void CEffect_WindVeg::OnLevelUnload()
{
    m_near_trees.clear();
    m_tree_cool.clear();
    m_next_tree_sort = 0.f;
    for (auto& c : m_sector_cool)
        c = 0.f;
    for (int t = 0; t < type_count; ++t)
        for (auto& v : m_voices[t])
            if (v.snd._feedback())
                v.snd.stop();
}

CEffect_WindVeg::~CEffect_WindVeg()
{
    for (int t = 0; t < type_count; ++t)
        for (auto& v : m_voices[t])
            v.snd.destroy();
}
