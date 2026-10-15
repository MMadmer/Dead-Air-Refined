// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The grass and detail season data of dead_air_x64_seasons.ltx (docs/dead-air/SEASON_DATA.md), parsed once
// per level load and shared by the detail manager (classes, stock season keys) and the blade grass
// (profiles, season keys, ground tint). Private to xrRender: DetailManager.cpp and da_grass_blades.cpp.
#pragma once

#include "xrEngine/da_season.h"
#include "DetailModel.h"

namespace xray::render::RENDER_NAMESPACE
{
// Three DaSeasonCurves over LINEAR rgb.
struct DaColourCurve
{
    DaSeasonCurve c[3];
    void add(float day, const Fvector3& linear)
    {
        c[0].add(day, linear.x);
        c[1].add(day, linear.y);
        c[2].add(day, linear.z);
    }
    void build()
    {
        for (DaSeasonCurve& curve : c)
            curve.build();
    }
    Fvector3 at(float day) const { return {c[0].at(day), c[1].at(day), c[2].at(day)}; }
    bool empty() const { return !c[0].n; }
};

// Keep, height and burial rule of a stock season class (brief 4.5).
struct DaStockSeason
{
    DaSeasonCurve keep, height, hide_rel;
    float hide_abs{0.03f};
    float hide_margin{0.03f};
    bool grass_visible{}; // keep is multiplied by grass_visible(D)
};

// Season scalars of a blade profile, in the order of the GPU block (da_grass_common.h).
enum DaGrassKey : u32
{
    gkNewGrowth,
    gkGreen,
    gkCohortGreen,
    gkThatch,
    gkThatchLodge,
    gkStrawLodge,
    gkHeads,
    gkSeedStage,
    gkTipDry,
    gkWet,
    gkFrost,
    gkDensity,
    gkFlood,
    gkFloodIce,
    gkCount
};

enum DaGrassColour : u32
{
    gcLive,
    gcYellowing,
    gcStraw,
    gcThatch,
    gcSeed,
    gcCount
};

struct DaGrassKeys
{
    DaSeasonCurve s[gkCount];
    DaColourCurve c[gcCount];
};

constexpr u32 da_grass_species = 64;     // r3\da_grass_common.h GRASS_SPECIES: the 6 species bits of a record
constexpr u32 da_grass_communities = 15; // community + 1 fits the 4 bits of a slot's id info
constexpr u32 da_community_species = 8;  // GRASS_COMM_SPECIES: graminoids of a community
constexpr u32 da_community_forbs = 16;   // GRASS_COMM_FORBS: forbs on top of them
constexpr u32 da_head_types = 16;        // 1..15, 0 = none

// One plant species ([grass_species_N]): angles in degrees, lengths in metres. Graminoids draw leaves
// (with the basal pairs of their tuft) and culms with seed heads; forbs a leafy stem with an inflorescence
// and, for rosettes, basal leaves and leafless scapes; rushes green leafless stems.
struct DaGrassProfile
{
    bool used{}; // a section was read (else the species draws nothing)
    float form{};    // 0 graminoid, 1 forb, 2 rosette forb, 3 rush (GRASS_FORM_*)
    float outline{}; // leaf outline: 0 grass, 1 lanceolate, 2 ovate, 3 bristle, 4 strap (GRASS_LEAF_*)
    float leaf_h[2]{0.3f, 0.6f}, leaf_w[2]{0.004f, 0.008f}, leaf_tilt[2]{10.f, 30.f}, leaf_bend[2]{0.2f, 0.4f};
    float leaf_droop[2]{0.f, 0.f}, leaf_stiff{0.4f};
    float culm_h[2]{0.6f, 1.f}, culm_w[2]{0.003f, 0.003f}, culm_tilt[2]{0.f, 10.f}, culm_bend[2]{0.05f, 0.1f};
    float culm_stiff{0.6f}, culm_knee{}, culm_leaves{};
    // Leaves on a stem (reeds, forbs): size, angle from the vertical, the share of the stem they span,
    // the upper leaves' size against the lowest, arrangement (0 two-ranked, 1 spiral, 2 opposite), bend, droop.
    float cauline_h[2]{0.1f, 0.2f}, cauline_w[2]{0.005f, 0.01f}, cauline_tilt[2]{40.f, 70.f}, cauline_span[2]{0.25f, 0.76f};
    float cauline_taper{1.f}, cauline_arrange{}, cauline_bend{0.3f}, cauline_droop{0.2f};
    // The dense base of short folded leaves (drawn near the camera; their leaves per m2 in density_basal).
    float basal_h[2]{0.12f, 0.3f}, basal_w[2]{0.003f, 0.006f}, basal_tilt[2]{25.f, 60.f}, basal_bend[2]{0.4f, 0.7f};
    float basal_droop[2]{0.1f, 0.3f};
    float clump_cell{0.25f}, clump_pull{0.2f}, clump_face_away{}, clump_lean{};
    float density_leaf{200.f}, density_culm{40.f}, density_basal{}, density_cal{0.55f}, cover{1.f};
    float shade_density{1.f}, shade_height{1.f};
    // The seed head or inflorescence: type (1..14), length and spread (m), branch ribbons, opening at anthesis and
    // when ripe (degrees), the grain scale against the type's, the petals of its flowers (0: round grains), the
    // share of culms that carry one, the nod.
    float head_type{6.f}, head_len[2]{0.1f, 0.2f}, head_w[2]{0.01f, 0.02f}, head_branches{}, head_open[2]{};
    float head_grain{1.f}, head_petals{}, head_share{}, head_nod[2]{};
    float fold_h{0.15f};
    float tussock_h[2]{}, tussock_share{}, tussock_r{};
    Fvector3 seed_ramp[4]{};          // linear, at seed stage 0.25, 0.5, 0.75, 1
    Fvector3 stem_tint{1.f, 1.f, 1.f}; // linear factor of the culm colour
    // 1: last year's culms stand through the winter (a reed bed, a cattail stand): the snow's grass visibility does not
    // thin it, only the snow over a blade buries that blade.
    float winter_stand{};
    // How much the dead tissue's colour varies culm to culm and clone to clone (0 none, 1 a reed bed's: tawny to
    // grey-buff, pale to dark).
    float dead_vary{};
};

// A plant community ([grass_community_N]): the graminoids a detail object grows and their cover shares in
// clonal patches; the forbs among them, each with the share of the lanes it takes scattered everywhere, the
// share of the area its clonal patches cover and the share of the lanes it takes inside them; the stock
// palette's alpha from which the stand is complete; the litter under it.
struct DaGrassCommunity
{
    bool used{};
    u32 count{}, forbs{};
    u8 species[da_community_species]{};
    float share[da_community_species]{};
    u8 forb[da_community_forbs]{};
    float forb_scattered[da_community_forbs]{}, forb_area[da_community_forbs]{}, forb_dense[da_community_forbs]{};
    float forb_patch{4.f};
    // cover: the share of the ground the stand grows on (a pine forest floor is mostly needles and moss).
    float patch{3.f}, scattered{0.3f}, presence{0.4f}, cover{1.f};
    float litter{}, litter_h[2]{0.08f, 0.2f}, litter_w{0.004f};
    // The mean share of the lanes forb f takes, and all the forbs together.
    float forb_lanes(u32 f) const { return forb_scattered[f] + forb_area[f] * forb_dense[f]; }
    float forb_total() const
    {
        float t = 0.f;
        for (u32 i = 0; i < forbs; ++i)
            t += forb_lanes(i);
        return t;
    }
};

struct DaGroundTintData
{
    float near_k{0.25f}, far_k{0.75f}, start{0.7f}, end{1.f}, end_margin{20.f};
    // Reference day of each latched terrain folder: autumn (base), spring_early, spring_mid, spring_late, summer.
    float ref[5]{287.5f, 73.5f, 109.5f, 134.5f, 195.5f};
};

constexpr u32 da_season_classes = 8; // 0 generic grass (GRASS_LEGACY), 1..7 forb .. decal

struct DaSeasonData
{
    xr_unordered_map<u64, EDetailClass> detail_types;
    s8 class_community[dcCount]{}; // detail class -> community 0..14 grown as blades, -1 none (stock card)
    DaStockSeason stock[da_season_classes];
    DaGrassKeys keys[da_grass_species];
    DaGrassProfile profiles[da_grass_species];
    shared_str species_names[da_grass_species];
    DaGrassCommunity communities[da_grass_communities];
    shared_str community_names[da_grass_communities];
    DaGroundTintData tint;
    bool file{}; // the file was found
};

// The key syntax of the season files, shared with the shrubs (da_shrubs.cpp): "#RRGGBB" (sRGB) as linear rgb, a
// "day:value, ..." curve, a "day:#RRGGBB, ..." colour curve, a key's value in a section (case-insensitive) or null.
bool da_parse_colour(pcstr s, Fvector3& linear);
void da_parse_curve(pcstr list, DaSeasonCurve& curve);
void da_parse_colour_curve(pcstr list, DaColourCurve& curve);
pcstr da_ini_value(const CInifile::Sect& sect, pcstr key);

// The data of the current level (reloaded by CDetailManager::Load; default-constructed without a level).
const DaSeasonData& da_season_data();
void da_season_data_reload();
// The community (0..14) a model is grown as blades with, or -1 (a stock card: its class maps to no community,
// or the community grows nothing).
s32 da_grass_object_profile(const CDetail& object);
// A community's name ("" for an unused one).
inline pcstr da_grass_community_name(u32 c)
{
    const shared_str& n = da_season_data().community_names[c];
    return n.size() ? n.c_str() : "";
}
// The community whose detail objects cover the most of the slot at world (x, z), 0..14, or -1 (no slot, no
// community there).
s32 da_grass_community_at(float x, float z);

// The season class of a detail class: 0 generic grass for unknown keys and the blade classes, else 1..7.
inline u32 da_season_class(u32 detail_class)
{
    return (detail_class >= dcForb && detail_class <= dcDecal) ? detail_class : 0u;
}
} // namespace xray::render::RENDER_NAMESPACE
