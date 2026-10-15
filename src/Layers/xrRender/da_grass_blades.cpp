// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Procedural blade grass after Ghost of Tsushima (docs/dead-air/GRASS.md).
//
// Level load: the detail slots, the species and community tables, the CDB triangles bucketed into 4 m cells
// for the ground bake, the grass cover map of the terrain tint. Per frame: the season keys of every species
// at D and D_prev and the camera (main thread, da_grass_frame_latch), the tiles of the quincunx lattice and
// their compute groups (detail calc task, da_grass_calc), then on the immediate context the ground window
// strips the camera moved into, the push field of every creature, the generation pass, the argument pass
// and the readback that sizes the bins; the G-buffer and the sun cascades draw the bins indirectly.

#include "stdafx.h"
#include "da_grass_blades.h"
#include "da_grass_data.h"
#include "da_ground.h"
#include "DetailManager.h"
#include "BufferUtils.h"
#include "da_temporal.h"
#include "ResourceManager.h"
#include "xrRender_console.h"
#include "xrEngine/da_season.h"
#include "xrEngine/da_snow_field.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Layers/xrRenderPC_R4/da_async.h"
#if defined(XR_D3D12)
#include "Layers/xrRenderDX12/d12_api.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>

namespace xray::render::RENDER_NAMESPACE
{
// ======================================================================================================
// Season data (dead_air_x64_seasons.ltx)
// ======================================================================================================
namespace
{
DaSeasonData g_data;

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float c)
{
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

pcstr skip_space(pcstr s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

// "#RRGGBB" (sRGB) as linear rgb.
bool parse_colour(pcstr s, Fvector3& linear)
{
    s = skip_space(s);
    if (*s == '#')
        ++s;
    u32 v = 0;
    int digits = 0;
    for (; *s && digits < 6; ++s, ++digits)
    {
        const char c = *s;
        const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0)
            return false;
        v = v * 16 + u32(d);
    }
    if (digits != 6)
        return false;
    linear.set(srgb_to_linear(float((v >> 16) & 255u) / 255.f), srgb_to_linear(float((v >> 8) & 255u) / 255.f),
        srgb_to_linear(float(v & 255u) / 255.f));
    return true;
}

// Every "day:value" item of a key list ("14.5:0.3, 45.5:#88785F, ...").
template <typename Fn>
void for_each_key(pcstr list, Fn&& fn)
{
    string256 item;
    for (u32 i = 0, n = _GetItemCount(list); i < n; ++i)
    {
        _GetItem(list, i, item);
        pcstr colon = strchr(item, ':');
        if (!colon)
            continue;
        fn(float(atof(item)), colon + 1);
    }
}

void parse_curve(pcstr list, DaSeasonCurve& curve)
{
    curve.clear();
    for_each_key(list, [&curve](float day, pcstr value) { curve.add(day, float(atof(skip_space(value)))); });
    curve.build();
}

void parse_colour_curve(pcstr list, DaColourCurve& curve)
{
    for (DaSeasonCurve& c : curve.c)
        c.clear();
    for_each_key(list, [&curve](float day, pcstr value) {
        Fvector3 linear;
        if (parse_colour(value, linear))
            curve.add(day, linear);
    });
    curve.build();
}

// The value of `key` in a section (keys compared case-insensitively), or null.
pcstr find_value(const CInifile::Sect& sect, pcstr key)
{
    for (const CInifile::Item& item : sect.Data)
        if (item.first.c_str() && !xr_stricmp(item.first.c_str(), key))
            return item.second.c_str() ? item.second.c_str() : "";
    return nullptr;
}

void read_float(const CInifile::Sect& sect, pcstr key, float& out)
{
    if (pcstr v = find_value(sect, key))
        out = float(atof(v));
}

void read_pair(const CInifile::Sect& sect, pcstr key, float (&out)[2])
{
    pcstr v = find_value(sect, key);
    if (!v)
        return;
    string64 item;
    const u32 n = _GetItemCount(v);
    if (n >= 1)
        out[0] = out[1] = float(atof(_GetItem(v, 0, item)));
    if (n >= 2)
        out[1] = float(atof(_GetItem(v, 1, item)));
}

void read_vector(const CInifile::Sect& sect, pcstr key, Fvector3& out)
{
    pcstr v = find_value(sect, key);
    if (!v || _GetItemCount(v) < 3)
        return;
    string64 item;
    out.set(float(atof(_GetItem(v, 0, item))), float(atof(_GetItem(v, 1, item))), float(atof(_GetItem(v, 2, item))));
}

EDetailClass class_by_name(pcstr name)
{
    struct Entry
    {
        pcstr name;
        EDetailClass cls;
    };
    static constexpr Entry table[] = {{"PG_BUNCH", dcBunch}, {"PG_FINE", dcFine}, {"PG_LEAFY", dcLeafy},
        {"PG_PLUME", dcPlume}, {"PG_TUFT", dcTuft}, {"PG_RUSH", dcRush}, {"PG_DRY", dcDry}, {"FIX_EMPTY", dcBunch},
        {"KS_FORB", dcForb}, {"KS_HERB", dcHerb}, {"KS_FERN", dcFern}, {"KS_HORSETAIL", dcHorsetail},
        {"KS_SHRUB", dcShrub}, {"KS_LITTER", dcLitter}, {"KS_DECAL", dcDecal}};
    name = skip_space(name);
    for (const Entry& e : table)
        if (!_strnicmp(name, e.name, xr_strlen(e.name)))
            return e.cls;
    return dcUnknown;
}

void parse_profile(const CInifile::Sect& s, DaGrassProfile& p)
{
    p.used = true;
    read_float(s, "form", p.form);
    read_float(s, "outline", p.outline);
    read_pair(s, "leaf_h", p.leaf_h);
    read_pair(s, "leaf_w", p.leaf_w);
    read_pair(s, "leaf_tilt", p.leaf_tilt);
    read_pair(s, "leaf_bend", p.leaf_bend);
    read_pair(s, "leaf_droop", p.leaf_droop);
    read_float(s, "leaf_stiff", p.leaf_stiff);
    read_pair(s, "culm_h", p.culm_h);
    read_pair(s, "culm_w", p.culm_w);
    read_pair(s, "culm_tilt", p.culm_tilt);
    read_pair(s, "culm_bend", p.culm_bend);
    read_float(s, "culm_stiff", p.culm_stiff);
    read_float(s, "culm_knee", p.culm_knee);
    read_float(s, "culm_leaves", p.culm_leaves);
    read_pair(s, "cauline_h", p.cauline_h);
    read_pair(s, "cauline_w", p.cauline_w);
    read_pair(s, "cauline_tilt", p.cauline_tilt);
    read_pair(s, "cauline_span", p.cauline_span);
    read_float(s, "cauline_taper", p.cauline_taper);
    read_float(s, "cauline_arrange", p.cauline_arrange);
    read_float(s, "cauline_bend", p.cauline_bend);
    read_float(s, "cauline_droop", p.cauline_droop);
    read_pair(s, "basal_h", p.basal_h);
    read_pair(s, "basal_w", p.basal_w);
    read_pair(s, "basal_tilt", p.basal_tilt);
    read_pair(s, "basal_bend", p.basal_bend);
    read_pair(s, "basal_droop", p.basal_droop);
    read_float(s, "clump_cell", p.clump_cell);
    read_float(s, "clump_pull", p.clump_pull);
    read_float(s, "clump_face_away", p.clump_face_away);
    read_float(s, "clump_lean", p.clump_lean);
    read_float(s, "density_leaf", p.density_leaf);
    read_float(s, "density_culm", p.density_culm);
    read_float(s, "density_basal", p.density_basal);
    read_float(s, "density_cal", p.density_cal);
    read_float(s, "cover", p.cover);
    read_float(s, "shade_density", p.shade_density);
    read_float(s, "shade_height", p.shade_height);
    read_float(s, "head_share", p.head_share);
    read_pair(s, "head_nod", p.head_nod);
    read_float(s, "head_type", p.head_type);
    p.head_type = float(std::clamp(iFloor(p.head_type + 0.5f), 1, int(da_head_types) - 1));
    read_pair(s, "head_len", p.head_len);
    read_pair(s, "head_w", p.head_w);
    read_float(s, "head_branches", p.head_branches);
    read_pair(s, "head_open", p.head_open);
    read_float(s, "head_grain", p.head_grain);
    read_float(s, "head_petals", p.head_petals);
    p.head_petals = float(std::clamp(iFloor(p.head_petals + 0.5f), 0, 8));
    read_float(s, "fold_h", p.fold_h);
    read_pair(s, "tussock_h", p.tussock_h);
    read_float(s, "tussock_share", p.tussock_share);
    read_float(s, "tussock_r", p.tussock_r);
    read_vector(s, "stem_tint", p.stem_tint);
    read_float(s, "winter_stand", p.winter_stand);
    read_float(s, "dead_vary", p.dead_vary);
    read_float(s, "head_velvet", p.head_velvet);
    read_float(s, "water_depth", p.water_depth);
    // Seed or flower colour ramp "stage:#RRGGBB" at 0.25, 0.5, 0.75, 1.
    if (pcstr ramp = find_value(s, "seed_ramp"))
        for_each_key(ramp, [&p](float stage, pcstr colour) {
            const int i = std::clamp(iFloor(stage * 4.f + 0.5f) - 1, 0, 3);
            parse_colour(colour, p.seed_ramp[i]);
        });
}

// The species index of a name, or -1.
s32 species_by_name(pcstr name)
{
    for (u32 sp = 0; sp < da_grass_species; ++sp)
        if (g_data.profiles[sp].used && g_data.species_names[sp].size() && !xr_stricmp(g_data.species_names[sp].c_str(), name))
            return s32(sp);
    return -1;
}

// A community: "species = name:share, ..." the graminoids (and rushes) with their cover shares (renormalised),
// "forbs = name:scattered:area:dense, ..." the forbs among them (the share of the lanes scattered everywhere,
// the share of the area of their clonal patches, the share of the lanes inside them), "forb_patch" the patch
// size (m). Unknown names are skipped.
void parse_community(const CInifile::Sect& s, DaGrassCommunity& c)
{
    c = DaGrassCommunity{};
    string256 item;
    if (pcstr list = find_value(s, "species"))
        for (u32 i = 0, n = _GetItemCount(list); i < n && c.count < da_community_species; ++i)
        {
            _GetItem(list, i, item);
            pstr colon = strchr(item, ':');
            if (!colon)
                continue;
            *colon = 0;
            const s32 sp = species_by_name(skip_space(item));
            const float share = std::max(float(atof(colon + 1)), 0.f);
            if (sp < 0 || share <= 0.f)
                continue;
            c.species[c.count] = u8(sp);
            c.share[c.count] = share;
            ++c.count;
        }
    if (pcstr list = find_value(s, "forbs"))
        for (u32 i = 0, n = _GetItemCount(list); i < n && c.forbs < da_community_forbs; ++i)
        {
            _GetItem(list, i, item);
            float v[3]{};
            u32 fields = 0;
            pstr cur = strchr(item, ':');
            if (!cur)
                continue;
            *cur++ = 0;
            while (cur && fields < 3)
            {
                v[fields++] = std::clamp(float(atof(cur)), 0.f, 1.f);
                cur = strchr(cur, ':');
                cur = cur ? cur + 1 : nullptr;
            }
            const s32 sp = species_by_name(skip_space(item));
            if (sp < 0)
                continue;
            c.forb[c.forbs] = u8(sp);
            c.forb_scattered[c.forbs] = v[0];
            c.forb_area[c.forbs] = v[1];
            c.forb_dense[c.forbs] = v[2];
            ++c.forbs;
        }
    read_float(s, "forb_patch", c.forb_patch);
    read_float(s, "cover", c.cover);
    float total = 0.f;
    for (u32 i = 0; i < c.count; ++i)
        total += c.share[i];
    for (u32 i = 0; i < c.count; ++i)
        c.share[i] /= total;
    // The forbs' patches share the area, their scattered plants at most a third of the lanes: the stand stays a sward.
    float area = 0.f, scattered = 0.f;
    for (u32 f = 0; f < c.forbs; ++f)
    {
        area += c.forb_area[f];
        scattered += c.forb_scattered[f];
    }
    for (u32 f = 0; f < c.forbs; ++f)
    {
        c.forb_area[f] *= area > 1.f ? 1.f / area : 1.f;
        c.forb_scattered[f] *= scattered > 0.33f ? 0.33f / scattered : 1.f;
    }
    c.used = c.count > 0;
    read_float(s, "patch", c.patch);
    read_float(s, "scattered", c.scattered);
    read_float(s, "presence", c.presence);
    read_float(s, "litter", c.litter);
    read_pair(s, "litter_h", c.litter_h);
    read_float(s, "litter_w", c.litter_w);
}
} // namespace

bool da_parse_colour(pcstr s, Fvector3& linear) { return parse_colour(s, linear); }
void da_parse_curve(pcstr list, DaSeasonCurve& curve) { parse_curve(list, curve); }
void da_parse_colour_curve(pcstr list, DaColourCurve& curve) { parse_colour_curve(list, curve); }
pcstr da_ini_value(const CInifile::Sect& sect, pcstr key) { return find_value(sect, key); }

const DaSeasonData& da_season_data() { return g_data; }

void da_season_data_reload()
{
    // In place: the data of 48 species is over half a megabyte, too much for a temporary on the stack.
    g_data.~DaSeasonData();
    new (&g_data) DaSeasonData();
    for (s8& community : g_data.class_community)
        community = -1;
    // Defaults where the data says nothing: the stock look of a species is a mid-summer meadow.
    for (DaGrassProfile& p : g_data.profiles)
        for (Fvector3& c : p.seed_ramp)
            c.set(srgb_to_linear(0.63f), srgb_to_linear(0.54f), srgb_to_linear(0.39f));
    if (!FS.exist("$game_config$", "dead_air_x64_seasons.ltx"))
    {
        Msg("! [grass] dead_air_x64_seasons.ltx not found: every detail object stays a stock card");
        return;
    }
    string_path fn;
    FS.update_path(fn, "$game_config$", "dead_air_x64_seasons.ltx");
    CInifile ini(fn, TRUE);
    g_data.file = true;

    if (ini.section_exist("detail_types"))
        for (const CInifile::Item& item : ini.r_section("detail_types").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            const EDetailClass cls = class_by_name(item.second.c_str());
            if (cls == dcUnknown)
                continue;
            g_data.detail_types[u64(strtoull(item.first.c_str(), nullptr, 16))] = cls;
        }

    static constexpr pcstr stock_sections[da_season_classes] = {"stock_keys_grass_legacy", "stock_keys_ks_forb",
        "stock_keys_ks_herb", "stock_keys_ks_fern", "stock_keys_ks_horsetail", "stock_keys_ks_shrub",
        "stock_keys_ks_litter", "stock_keys_ks_decal"};
    for (u32 c = 0; c < da_season_classes; ++c)
    {
        if (!ini.section_exist(stock_sections[c]))
            continue;
        const CInifile::Sect& s = ini.r_section(stock_sections[c]);
        DaStockSeason& k = g_data.stock[c];
        if (pcstr v = find_value(s, "keep"))
            parse_curve(v, k.keep);
        if (pcstr v = find_value(s, "height"))
            parse_curve(v, k.height);
        if (pcstr v = find_value(s, "hide_rel"))
            parse_curve(v, k.hide_rel);
        read_float(s, "hide_abs", k.hide_abs);
        read_float(s, "hide_margin", k.hide_margin);
        float visible = 0.f;
        read_float(s, "grass_visible", visible);
        k.grass_visible = visible > 0.5f;
    }

    // The species and their season keys (the research names are accepted beside the short ones).
    static constexpr pcstr scalar_names[gkCount][2] = {{"new_growth_h", "new_growth_height"}, {"green", "green_fraction"},
        {"cohort_green", "cohort_green"}, {"thatch", "thatch_fraction"}, {"thatch_lodge", "thatch_lodging"},
        {"straw_lodge", "straw_lodging"}, {"heads", "seed_heads"}, {"seed_stage", "seed_stage"},
        {"tip_dry", "tip_dryness"}, {"wet", "wetness"}, {"frost", "frost"}, {"density", "density"}, {"flood", "flood"},
        {"flood_ice", "flood_ice"}};
    static constexpr pcstr colour_names[gcCount] = {"live", "yellowing", "straw", "thatch", "seed"};
    for (u32 p = 0; p < da_grass_species; ++p)
    {
        string64 name;
        xr_sprintf(name, "grass_species_%u", p + 1);
        if (!ini.section_exist(name))
            continue;
        const CInifile::Sect& sp = ini.r_section(name);
        parse_profile(sp, g_data.profiles[p]);
        pcstr id = find_value(sp, "name");
        g_data.species_names[p] = id ? skip_space(id) : name;
        xr_sprintf(name, "grass_keys_%u", p + 1);
        if (!ini.section_exist(name))
            continue;
        const CInifile::Sect& s = ini.r_section(name);
        DaGrassKeys& keys = g_data.keys[p];
        for (u32 k = 0; k < gkCount; ++k)
        {
            pcstr v = find_value(s, scalar_names[k][0]);
            v = v ? v : find_value(s, scalar_names[k][1]);
            // "thatch" is a scalar list; a colour list under that name is the thatch colour.
            if (v && !strchr(v, '#'))
                parse_curve(v, keys.s[k]);
        }
        for (u32 c = 0; c < gcCount; ++c)
        {
            string64 key;
            xr_sprintf(key, "%s_color", colour_names[c]);
            pcstr v = find_value(s, key);
            if (!v)
            {
                xr_sprintf(key, "%s_colour", colour_names[c]);
                v = find_value(s, key);
            }
            if (!v)
            {
                v = find_value(s, colour_names[c]);
                v = (v && strchr(v, '#')) ? v : nullptr;
            }
            if (v)
                parse_colour_curve(v, keys.c[c]);
        }
    }

    // The communities (by the species names read above), and which detail class grows which of them.
    shared_str (&community_names)[da_grass_communities] = g_data.community_names;
    for (u32 c = 0; c < da_grass_communities; ++c)
    {
        string64 name;
        xr_sprintf(name, "grass_community_%u", c + 1);
        if (!ini.section_exist(name))
            continue;
        const CInifile::Sect& s = ini.r_section(name);
        parse_community(s, g_data.communities[c]);
        pcstr id = find_value(s, "name");
        community_names[c] = id ? skip_space(id) : name;
    }
    if (ini.section_exist("grass_class_community"))
        for (const CInifile::Item& item : ini.r_section("grass_class_community").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            const EDetailClass cls = class_by_name(item.first.c_str());
            pcstr value = skip_space(item.second.c_str());
            if (cls == dcUnknown)
                continue;
            for (u32 c = 0; c < da_grass_communities; ++c)
                if (g_data.communities[c].used && community_names[c].size() && !xr_stricmp(community_names[c].c_str(), value))
                    g_data.class_community[cls] = s8(c);
        }

    if (ini.section_exist("ground_tint"))
    {
        const CInifile::Sect& s = ini.r_section("ground_tint");
        DaGroundTintData& t = g_data.tint;
        read_float(s, "near", t.near_k);
        read_float(s, "far", t.far_k);
        read_float(s, "start", t.start);
        read_float(s, "end", t.end);
        read_float(s, "end_margin", t.end_margin);
        static constexpr pcstr refs[5] = {"ref_autumn", "ref_spring_early", "ref_spring_mid", "ref_spring_late", "ref_summer"};
        for (u32 i = 0; i < 5; ++i)
            read_float(s, refs[i], t.ref[i]);
    }

    u32 species = 0, communities = 0;
    for (const DaGrassProfile& p : g_data.profiles)
        species += p.used ? 1 : 0;
    for (const DaGrassCommunity& c : g_data.communities)
        communities += c.used ? 1 : 0;
    Msg("* [grass] season data: %u detail keys, %u species in %u communities", u32(g_data.detail_types.size()), species,
        communities);
}

s32 da_grass_community_at(float x, float z)
{
    CDetailManager* dm = RImplementation.Details;
    if (!dm || !dm->dtSlots)
        return -1;
    const int sx = iFloor(x / DETAIL_SLOT_SIZE) + int(dm->dtH.x_offs()), sz = iFloor(z / DETAIL_SLOT_SIZE) + int(dm->dtH.z_offs());
    if (sx < 0 || sz < 0 || sx >= int(dm->dtH.x_size()) || sz >= int(dm->dtH.z_size()))
        return -1;
    // The slot's four objects by the mean alpha of their palette.
    const DetailSlot& s = dm->dtSlots[size_t(sz) * dm->dtH.x_size() + size_t(sx)];
    const u32 ids[4] = {s.id0, s.id1, s.id2, s.id3};
    float best = 0.f;
    s32 out = -1;
    for (u32 k = 0; k < 4; ++k)
    {
        if (ids[k] == DetailSlot::ID_Empty || ids[k] >= dm->objects.size())
            continue;
        const s32 c = da_grass_object_profile(*dm->objects[ids[k]]);
        const DetailPalette& pal = s.palette[k];
        const float a = float(pal.a0 + pal.a1 + pal.a2 + pal.a3);
        if (c >= 0 && a > best)
        {
            best = a;
            out = c;
        }
    }
    return out;
}

s32 da_grass_object_profile(const CDetail& object)
{
    if (object.m_class == dcUnknown || object.m_class >= dcCount)
        return -1;
    const s32 c = g_data.class_community[object.m_class];
    return (c >= 0 && g_data.communities[c].used) ? c : -1;
}

// ======================================================================================================
// Blades
// ======================================================================================================
namespace
{
// The layout of the shaders' buffers (r3\da_grass_common.h).
constexpr u32 kBins = 16;
constexpr u32 kViewBins = 9;
constexpr u32 kBinLod0 = 0, kBinLod1 = 2, kBinLod2 = 5, kBinShNear = 9, kBinShMid = 10;
constexpr u32 kBinHead0 = 11, kBinHead1 = 12, kBinHeadSh = 14, kBinShort = 15;
constexpr u32 kRec = 64, kHeadRec = 16;
constexpr u32 kProfF4 = 28, kSeasF4 = 9, kCommF4 = 19, kCommRows = 16;
constexpr u32 kTabSeas = da_grass_species * kProfF4;
constexpr u32 kTabSeasPrev = kTabSeas + da_grass_species * kSeasF4;
constexpr u32 kTabComm = kTabSeasPrev + da_grass_species * kSeasF4;
constexpr u32 kTabIds = kTabComm + kCommRows * kCommF4;
constexpr u32 kTabTotal = kTabIds + 16;
constexpr u32 kLmax = 24;
constexpr float kBand = 0.08f;
// Compute groups of a frame (64 lanes each): 2 M lanes, far beyond what the presets ask.
constexpr u32 kMaxGroups = 32768;
// Deformer stamps the push field takes per frame, 32 bytes each.
constexpr u32 kMaxStamps = 4096;
constexpr u32 kStampBytes = 32;
// The ground bake's triangle cells (m) and the push window: 512 texels of 12.5 cm around the camera.
constexpr float kCell = 4.f;
constexpr u32 kPushSize = 512;
constexpr float kPushTexel = 0.125f;
constexpr float kPushTau = 2.5f;
// How fast a stamp pulls the push field toward its own push (s): a boot presses its grass within a tenth of a second.
constexpr float kPushAttack = 0.06f;
// The near sun cascade's box (render_phase_sun.cpp, m_sun_cascades[0].size).
constexpr float kNearReach = 20.f;
// Staging copies of the bin counts in flight: enough for a GPU-bound frame queue of any depth to keep
// one free for the next copy.
constexpr u32 kReadbacks = 5;

// Vertex classes of the index buffer, 32 blades (or heads) per instance: 13 / 7 / 3-vertex strips, the
// 43-vertex near head (core 13 + 6 branches of 5) and the 13-vertex middle head (core 7 + 2 of 3).
enum : u32
{
    vc13,
    vc7,
    vc3,
    vcH43,
    vcH13,
    vcCount,
    vcNone = vcCount
};
struct VertexClass
{
    u32 verts, indices, first;
};
VertexClass g_vclass[vcCount]{};
constexpr u32 kBinClass[kBins] = {vc13, vc13, vc7, vc7, vc7, vc3, vc3, vc3, vc3, vc7, vc3, vcH43, vcH13, vcH13, vcH13, vc7};
// LOD each bin draws with (da_grass_draw.w: 0..2 view, 3 near cascade, 4 middle cascade).
constexpr u32 kBinLod[kBins] = {0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 4, 0, 1, 1, 3, 1};
// Each bin's first share of its buffer, before the readback has measured anything.
constexpr float kBinShare[kBins] = {0.05f, 0.06f, 0.08f, 0.07f, 0.07f, 0.05f, 0.06f, 0.08f, 0.1f, 0.1f, 0.14f,
    0.25f, 0.30f, 0.30f, 0.15f, 0.14f};
bool bin_is_head(u32 b) { return b >= kBinHead0 && b <= kBinHeadSh; }

// The grass columns of preset 0..4 (minimum..extreme): pixel floor k_px (render pixels), LOD switch
// distances d1/d2 (m), blades in the middle sun cascade, blade and head records.
struct PresetColumns
{
    float kpx, d1, d2;
    bool shadow_mid;
    u32 records, heads;
};
// The record budgets hold a full stand (the palette's weight no longer thins it), its basal pairs and litter and
// the forbs' leaves: a field of 1.6.0's first species round needed about 1.6 times the old windows at Extreme.
constexpr PresetColumns kPresets[5] = {{1.5f, 8.f, 25.f, false, 192u << 10, 16u << 10},
    {1.5f, 9.f, 30.f, false, 256u << 10, 16u << 10}, {1.25f, 10.f, 35.f, true, 400u << 10, 24u << 10},
    {1.0f, 11.f, 40.f, true, 640u << 10, 32u << 10}, {1.0f, 12.f, 45.f, true, 896u << 10, 48u << 10}};

struct GpuBuffer
{
    ID3D11Buffer* buf{};
    ID3D11ShaderResourceView* srv{};
    ID3D11UnorderedAccessView* uav{};
    u32 bytes{};
    void release()
    {
        _RELEASE(srv);
        _RELEASE(uav);
        _RELEASE(buf);
        bytes = 0;
    }
};

struct GpuTexture
{
    ID3D11Texture2D* tex{};
    ID3D11ShaderResourceView* srv{};
    ID3D11UnorderedAccessView* uav{};
    u32 size{};
    void release()
    {
        _RELEASE(srv);
        _RELEASE(uav);
        _RELEASE(tex);
        size = 0;
    }
};

// The compute passes: a CS, the season snow textures where the pass reads them, the samplers by name
// (each binding returns silently where the shader does not use it).
class CBlender_da_grass_compute : public IBlender
{
    pcstr m_cs;
    bool m_snow;

public:
    CBlender_da_grass_compute(pcstr cs, bool snow) : m_cs(cs), m_snow(snow) {}
    LPCSTR getComment() override { return "INTERNAL: blade grass, compute"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_ComputePass(m_cs);
        if (m_snow)
        {
            C.r_dx11Texture("s_da_snow_fine", "$user$da_snow_fine");
            C.r_dx11Texture("s_da_snow_level", "$user$da_snow_level");
            // (The generation: the water over a lane's root, r3\da_water_field.h.)
            C.r_dx11Texture("s_water_field", r2_RT_water_field);
        }
        C.r_dx11Sampler("smp_linear");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_dx11Sampler("smp_nofilter");
        C.r_End();
    }
};

// The draws: the G-buffer pass (stencil like every deferred surface, no culling: blades are two-sided)
// and the sun cascades (depth only).
class CBlender_da_grass_draw : public IBlender
{
    pcstr m_vs, m_ps;
    bool m_shadow;

public:
    CBlender_da_grass_draw(pcstr vs, pcstr ps, bool shadow) : m_vs(vs), m_ps(ps), m_shadow(shadow) {}
    LPCSTR getComment() override { return "INTERNAL: blade grass, draw"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        if (m_shadow)
        {
            C.r_Pass(m_vs, "null", FALSE, TRUE, TRUE, FALSE);
            C.r_CullMode(D3DCULL_NONE);
        }
        else
        {
            C.r_Pass(m_vs, m_ps, FALSE, TRUE, TRUE);
            C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
            C.r_CullMode(D3DCULL_NONE);
        }
        C.r_End();
    }
};

// What the main thread latches once per frame for the calc task and the passes of the frame.
struct Latch
{
    u32 frame{u32(-1)};
    Fvector cam{}, fwd{}, sun{};
    CFrustum view;
    Fvector4 planes[6]{};
    float R{}, R0{}, d1{}, d2{}, kpx{}, wmin_per_m{};
    float s0{}, dlat{}, density_scale{}, height{};
    float near_texel{}, mid_reach{}, mid_texel{};
    bool shadow_near{}, shadow_mid{};
    float time{}, time_prev{};
    bool motion{};
    bool snow{};
    float grass_visible{1.f};
    // Render density of each community's densest species now (1/m2): the tiles' density bound.
    float dens_now[da_grass_communities]{};
    Fvector4 wave[4]{}, wave_prev[4]{};
    float bin_far[12]{};
    u32 base[kBins]{}, cap[kBins]{};
    u32 frame_mod{};
};

struct State
{
    bool loaded{}, failed{}, active{};
    // The level: its detail slots, copied (a renderer reset recreates the detail manager and its file
    // mapping).
    xr_vector<DetailSlot> slot_copy;
    const DetailSlot* slots{};
    u32 size_x{}, size_z{};
    int offs_x{}, offs_z{};
    u8 id_profile[dm_max_objects]{}; // community + 1, 0 = not grown as blades
    // Per slot: 4 x (community + 1 in 4 bits | the id's highest palette corner in 4 bits).
    xr_vector<u32> slot_info;
    float profile_dens[da_grass_communities]{}; // the densest species' (leaves + culms) x calibration, 1/m2
    float level_hmax{};
    float max_area_width{};
    // Shaders.
    ref_shader s_gen, s_args, s_ground, s_stamp, s_resolve, s_blade, s_blade_cut, s_head, s_shadow, s_head_shadow;
    // Buffers and textures.
    GpuBuffer slots_b, tables_b, groups_b, stamps_b, cells_b, refs_b, verts_b, tris_b, blades_b, heads_b, counts_b, args_b;
    ID3D11ShaderResourceView* cdb_verts{};
    ID3D11ShaderResourceView* cdb_tris{};
    // The counts' readback ring: written in order at staging_next, a slot pending until it is read.
    ID3D11Buffer* staging[kReadbacks]{};
    bool staging_pending[kReadbacks]{};
    u32 staging_next{};
    u32 rebalance_frame{};
    GpuTexture ground, push[2], stamp;
    VertexStagingBuffer vb;
    IndexStagingBuffer ib;
    ref_geom geom;
    // The ground bake's cells over the slot area.
    u32 cells_x{}, cells_z{};
    Fvector2 cells_origin{};
    // The ground window: first world texel, size, texel; its contents are valid.
    int gx{}, gz{};
    u32 gsize{};
    float gtexel{0.25f};
    bool gvalid{};
    // The push window, this frame's and the previous (ping-pong).
    int px[2]{}, pz[2]{};
    u32 pcur{};
    bool pvalid{};
    // Records of the bins.
    u32 records{}, heads{};
    u32 base[kBins]{}, cap[kBins]{};
    float need[kBins]{};
    bool rebalance{};
    // The frame.
    Latch latch;
    Fvector4 tables[kTabTotal]{};
    xr_vector<u32> groups; // the calc task's output, 4 uints per group
    u32 group_count{}, lane_count{};
    std::atomic<float> green{0.f};
    u32 preset{2};
    u32 counts_seen[kBins]{};
    u32 overflow_frames{};
    // The record budget's pixel floor: over the buffer the blades of the whole view grow fewer and wider (the
    // pixel rule of the far field moved nearer), never dropped by the bins' windows in rings.
    float floor_k{1.f};
    // The ground tint of the frame (the terrain binders read it from any context).
    bool tint_on{};
    Fvector4 tint_xf{}, tint{}, tint2{}, canopy[16]{}, canopy_ref[16]{};
    // QA: the species every lane grows (species + 1, 0 = the communities decide).
    u32 pin{};
    // The communities' canopy colours are mixed again only when the day moves.
    float tint_day{-1000.f}, tint_ref_day{-1.f}, tint_height{-1.f}, canopy_shade[16]{};
    // The tables were filled for this day, previous day, height setting and grass visibility; upload pending.
    float tab_day{-1000.f}, tab_day_prev{-1000.f}, tab_height{-1.f}, tab_visible{-1.f};
    bool tab_upload{};
};

State g;

// A new table buffer (a level load) is filled and uploaded at the next latch.
bool reset_tables()
{
    g.tab_day = g.tab_day_prev = -1000.f;
    g.tab_height = g.tab_visible = -1.f;
    g.tab_upload = false;
    return true;
}
// $user$grass_cover: one CTexture for the renderer's lifetime (compiled passes keep pointers to it), its
// surface replaced per level (the water field's rule, r4_water_field.cpp).
ref_texture g_cover;

bool make_buffer(GpuBuffer& b, u32 bytes, UINT bind, UINT misc, const void* init)
{
    b.release();
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = std::max((bytes + 15) & ~15u, 16u);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | misc;
    xr_vector<u8> padded;
    D3D11_SUBRESOURCE_DATA data{};
    if (init)
    {
        padded.assign(desc.ByteWidth, 0);
        CopyMemory(padded.data(), init, bytes);
        data.pSysMem = padded.data();
    }
    if (FAILED(HW.pDevice->CreateBuffer(&desc, init ? &data : nullptr, &b.buf)))
        return false;
    b.bytes = desc.ByteWidth;
    if (bind & D3D11_BIND_SHADER_RESOURCE)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        view.BufferEx.NumElements = desc.ByteWidth / 4;
        if (FAILED(HW.pDevice->CreateShaderResourceView(b.buf, &view, &b.srv)))
            return false;
    }
    if (bind & D3D11_BIND_UNORDERED_ACCESS)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        view.Buffer.NumElements = desc.ByteWidth / 4;
        if (FAILED(HW.pDevice->CreateUnorderedAccessView(b.buf, &view, &b.uav)))
            return false;
    }
    return true;
}

bool make_texture(GpuTexture& t, u32 size, DXGI_FORMAT format, u32 texel_bytes, bool zero)
{
    t.release();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    xr_vector<u8> init;
    D3D11_SUBRESOURCE_DATA data{};
    if (zero)
    {
        init.assign(size_t(size) * size * texel_bytes, 0);
        data.pSysMem = init.data();
        data.SysMemPitch = size * texel_bytes;
    }
    if (FAILED(HW.pDevice->CreateTexture2D(&desc, zero ? &data : nullptr, &t.tex)))
        return false;
    if (FAILED(HW.pDevice->CreateShaderResourceView(t.tex, nullptr, &t.srv)) ||
        FAILED(HW.pDevice->CreateUnorderedAccessView(t.tex, nullptr, &t.uav)))
        return false;
    t.size = size;
    return true;
}

bool compute_ready(const ref_shader& s)
{
    return s && s->E[0] && !s->E[0]->passes.empty() && s->E[0]->passes[0]->cs && s->E[0]->passes[0]->cs->sh;
}

bool draw_ready(const ref_shader& s)
{
    return s && s->E[0] && !s->E[0]->passes.empty() && s->E[0]->passes[0]->vs && s->E[0]->passes[0]->vs->sh;
}

// Every shader of the blades ships with the content: an install with older shaders has none of them,
// and compiling a missing file is fatal.
constexpr pcstr kShaderFiles[] = {"da_grass_gen.cs", "da_grass_args.cs", "da_grass_ground.cs", "da_grass_push_stamp.cs",
    "da_grass_push_resolve.cs", "da_grass_blade.vs", "da_grass_head.vs", "da_grass_shadow.vs", "da_grass_head_shadow.vs",
    "da_grass_blade.ps", "da_grass_blade_cut.ps", "da_grass_head.ps"};

bool shaders_present()
{
    for (pcstr file : kShaderFiles)
        if (!da_async_shader_exists(file))
            return false;
    return true;
}

bool create_shaders()
{
    static CBlender_da_grass_compute b_gen("da_grass_gen", true), b_args("da_grass_args", false),
        b_ground("da_grass_ground", false), b_stamp("da_grass_push_stamp", false), b_resolve("da_grass_push_resolve", false);
    static CBlender_da_grass_draw b_blade("da_grass_blade", "da_grass_blade", false),
        b_blade_cut("da_grass_blade", "da_grass_blade_cut", false), b_head("da_grass_head", "da_grass_head", false),
        b_shadow("da_grass_shadow", "null", true), b_head_shadow("da_grass_head_shadow", "null", true);
    g.s_gen.create(&b_gen, "r2" DELIMITER "da_grass_gen");
    g.s_args.create(&b_args, "r2" DELIMITER "da_grass_args");
    g.s_ground.create(&b_ground, "r2" DELIMITER "da_grass_ground");
    g.s_stamp.create(&b_stamp, "r2" DELIMITER "da_grass_push_stamp");
    g.s_resolve.create(&b_resolve, "r2" DELIMITER "da_grass_push_resolve");
    g.s_blade.create(&b_blade, "r2" DELIMITER "da_grass_blade");
    g.s_blade_cut.create(&b_blade_cut, "r2" DELIMITER "da_grass_blade_cut");
    g.s_head.create(&b_head, "r2" DELIMITER "da_grass_head");
    g.s_shadow.create(&b_shadow, "r2" DELIMITER "da_grass_shadow");
    g.s_head_shadow.create(&b_head_shadow, "r2" DELIMITER "da_grass_head_shadow");
    return compute_ready(g.s_gen) && compute_ready(g.s_args) && compute_ready(g.s_ground) && compute_ready(g.s_stamp) &&
        compute_ready(g.s_resolve) && draw_ready(g.s_blade) && draw_ready(g.s_blade_cut) && draw_ready(g.s_head) &&
        draw_ready(g.s_shadow) && draw_ready(g.s_head_shadow);
}

// A strip of `pairs` pairs and a tip as a triangle list (local vertex ids).
void strip_triangles(xr_vector<u16>& out, u32 first_vertex, u32 pairs)
{
    for (u32 p = 0; p + 1 < pairs; ++p)
    {
        const u16 a = u16(first_vertex + 2 * p);
        out.insert(out.end(), {a, u16(a + 1), u16(a + 2), u16(a + 1), u16(a + 3), u16(a + 2)});
    }
    const u16 last = u16(first_vertex + 2 * (pairs - 1));
    out.insert(out.end(), {last, u16(last + 1), u16(first_vertex + 2 * pairs)});
}

// The static geometry of every vertex class: an index list of 32 blades each, values blade x V + local;
// the vertex stream is a placeholder (the vertex shaders read SV_VertexID only).
bool create_geometry()
{
    struct Shape
    {
        u32 verts;
        u32 core_pairs;
        u32 branches, branch_pairs;
    };
    constexpr Shape shapes[vcCount] = {{13, 6, 0, 0}, {7, 3, 0, 0}, {3, 1, 0, 0}, {43, 6, 6, 2}, {13, 3, 2, 1}};
    xr_vector<u16> indices;
    for (u32 c = 0; c < vcCount; ++c)
    {
        xr_vector<u16> one;
        strip_triangles(one, 0, shapes[c].core_pairs);
        const u32 core_v = 2 * shapes[c].core_pairs + 1;
        for (u32 j = 0; j < shapes[c].branches; ++j)
            strip_triangles(one, core_v + j * (2 * shapes[c].branch_pairs + 1), shapes[c].branch_pairs);
        g_vclass[c] = {shapes[c].verts, u32(one.size()) * 32, u32(indices.size())};
        for (u32 blade = 0; blade < 32; ++blade)
            for (const u16 i : one)
                indices.push_back(u16(blade * shapes[c].verts + i));
    }
    static const VertexElement decl[] = {{0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    const u32 verts = 32 * 43;
    const xr_vector<float> zero(verts, 0.f);
    g.vb.Create(verts * sizeof(float));
    CopyMemory(g.vb.Map(), zero.data(), verts * sizeof(float));
    g.vb.Unmap(true);
    g.ib.Create(indices.size() * sizeof(u16));
    CopyMemory(g.ib.Map(), indices.data(), indices.size() * sizeof(u16));
    g.ib.Unmap(true);
    g.geom.create(decl, g.vb, g.ib);
    return bool(g.geom);
}

// The bins' windows of the blade and head buffers: shares of each buffer by what the bins needed.
void layout_bins(const float (&need)[kBins])
{
    float sum_b = 0.f, sum_h = 0.f;
    for (u32 b = 0; b < kBins; ++b)
        (bin_is_head(b) ? sum_h : sum_b) += need[b];
    u32 at_b = 0, at_h = 0;
    for (u32 b = 0; b < kBins; ++b)
    {
        const bool head = bin_is_head(b);
        const u32 total = head ? g.heads : g.records;
        const float sum = head ? sum_h : sum_b;
        u32 cap = (kBinClass[b] == vcNone || sum <= 0.f) ? 0u : u32(float(total) * need[b] / sum);
        cap &= ~31u;
        u32& at = head ? at_h : at_b;
        g.base[b] = at;
        g.cap[b] = cap;
        at += cap;
    }
}

bool create_bins()
{
    const PresetColumns& p = kPresets[g.preset];
    g.records = p.records;
    g.heads = p.heads;
    if (!make_buffer(g.blades_b, g.records * kRec, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, nullptr) ||
        !make_buffer(g.heads_b, g.heads * kHeadRec, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, nullptr))
        return false;
    for (u32 b = 0; b < kBins; ++b)
        g.need[b] = kBinShare[b];
    layout_bins(g.need);
    g.rebalance = false;
    g.floor_k = 1.f;
    return true;
}

void release_all()
{
    g.s_gen.destroy();
    g.s_args.destroy();
    g.s_ground.destroy();
    g.s_stamp.destroy();
    g.s_resolve.destroy();
    g.s_blade.destroy();
    g.s_blade_cut.destroy();
    g.s_head.destroy();
    g.s_shadow.destroy();
    g.s_head_shadow.destroy();
    for (GpuBuffer* b : {&g.slots_b, &g.tables_b, &g.groups_b, &g.stamps_b, &g.cells_b, &g.refs_b, &g.verts_b, &g.tris_b,
             &g.blades_b, &g.heads_b, &g.counts_b, &g.args_b})
        b->release();
    for (ID3D11Buffer*& s : g.staging)
        _RELEASE(s);
    g.ground.release();
    g.push[0].release();
    g.push[1].release();
    g.stamp.release();
    if (g.geom)
        g.geom.destroy();
    if (g.ib.IsValid())
        g.ib.Release();
    if (g.vb.IsValid())
        g.vb.Release();
    if (g_cover)
        g_cover->surface_set(nullptr);
    // Every file-scope value belongs to one level (second-load bugs): the next one starts from scratch.
    const u32 preset = g.preset;
    const bool failed = g.failed;
    g.~State();
    new (&g) State();
    g.preset = preset;
    g.failed = failed;
}

// The level's collision triangles that can be grass ground (up-facing, not passable) bucketed into
// 4 m cells over the slots that hold blade grass; the triangle SRVs of da_ground (one upload for snow
// and grass) when they are the CDB's, else an upload of our own in the same format.
bool create_ground_cells()
{
    CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    const CDB::TRI* tris = g_pGameLevel->ObjectSpace.GetStaticTris();
    const Fvector* verts = g_pGameLevel->ObjectSpace.GetStaticVerts();
    if (!model || !tris || !verts)
        return false;
    const u32 tri_count = u32(model->get_tris_count());
    const u32 vert_count = u32(model->get_verts_count());

    g.cells_origin.set(-float(g.offs_x) * DETAIL_SLOT_SIZE, -float(g.offs_z) * DETAIL_SLOT_SIZE);
    g.cells_x = std::max(u32(std::ceil(float(g.size_x) * DETAIL_SLOT_SIZE / kCell)), 1u);
    g.cells_z = std::max(u32(std::ceil(float(g.size_z) * DETAIL_SLOT_SIZE / kCell)), 1u);
    xr_vector<u8> cell_grass(size_t(g.cells_x) * g.cells_z, 0);
    for (u32 z = 0; z < g.size_z; ++z)
        for (u32 x = 0; x < g.size_x; ++x)
        {
            if ((g.slot_info[size_t(z) * g.size_x + x] & 0x0F0F0F0Fu) == 0)
                continue;
            const u32 cx = std::min(u32(float(x) * DETAIL_SLOT_SIZE / kCell), g.cells_x - 1);
            const u32 cz = std::min(u32(float(z) * DETAIL_SLOT_SIZE / kCell), g.cells_z - 1);
            cell_grass[size_t(cz) * g.cells_x + cx] = 1;
        }

    xr_vector<u8> passable(1u << 14, 0);
    const u32 materials = std::min(u32(GMLib.CountMaterial()), u32(passable.size()));
    for (u32 m = 0; m < materials; ++m)
        if (const SGameMtl* mtl = GMLib.GetMaterialByIdx(u16(m)))
            passable[m] = mtl->Flags.test(SGameMtl::flPassable) ? 1 : 0;

    xr_vector<u32> cells(size_t(g.cells_x) * g.cells_z * 2, 0);
    const auto for_cells = [&](u32 t, const auto& fn) {
        const CDB::TRI& T = tris[t];
        if (passable[T.material])
            return;
        const Fvector &a = verts[T.verts[0]], &b = verts[T.verts[1]], &c = verts[T.verts[2]];
        // The culled downward ray of the stock rule hits up-facing triangles only.
        const float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        if (ny <= 1e-5f)
            return;
        const float x0 = std::min({a.x, b.x, c.x}) - g.cells_origin.x, x1 = std::max({a.x, b.x, c.x}) - g.cells_origin.x;
        const float z0 = std::min({a.z, b.z, c.z}) - g.cells_origin.y, z1 = std::max({a.z, b.z, c.z}) - g.cells_origin.y;
        const int cx0 = std::max(iFloor(x0 / kCell), 0), cx1 = std::min(iFloor(x1 / kCell), int(g.cells_x) - 1);
        const int cz0 = std::max(iFloor(z0 / kCell), 0), cz1 = std::min(iFloor(z1 / kCell), int(g.cells_z) - 1);
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx)
            {
                const size_t cell = size_t(cz) * g.cells_x + size_t(cx);
                if (cell_grass[cell])
                    fn(cell);
            }
    };
    for (u32 t = 0; t < tri_count; ++t)
        for_cells(t, [&cells](size_t cell) { ++cells[cell * 2 + 1]; });
    u32 refs_total = 0;
    for (size_t i = 0; i < cells.size() / 2; ++i)
    {
        cells[i * 2] = refs_total;
        refs_total += cells[i * 2 + 1];
        cells[i * 2 + 1] = 0;
    }
    xr_vector<u32> refs(std::max(refs_total, 1u), 0);
    for (u32 t = 0; t < tri_count; ++t)
        for_cells(t, [&cells, &refs, t](size_t cell) { refs[cells[cell * 2] + cells[cell * 2 + 1]++] = t; });
    if (!make_buffer(g.cells_b, u32(cells.size() * 4), D3D11_BIND_SHADER_RESOURCE, 0, cells.data()) ||
        !make_buffer(g.refs_b, u32(refs.size() * 4), D3D11_BIND_SHADER_RESOURCE, 0, refs.data()))
        return false;

    DaGroundCdb cdb;
    if (da_ground_cdb(cdb) && cdb.verts && cdb.tris && cdb.tri_count == tri_count)
    {
        g.cdb_verts = cdb.verts;
        g.cdb_tris = cdb.tris;
    }
    else
    {
        xr_vector<u32> tri_rows(size_t(tri_count) * 4);
        for (u32 t = 0; t < tri_count; ++t)
        {
            const CDB::TRI& T = tris[t];
            tri_rows[size_t(t) * 4 + 0] = T.verts[0];
            tri_rows[size_t(t) * 4 + 1] = T.verts[1];
            tri_rows[size_t(t) * 4 + 2] = T.verts[2];
            tri_rows[size_t(t) * 4 + 3] = T.material | (passable[T.material] ? (1u << 16) : 0u);
        }
        if (!make_buffer(g.verts_b, vert_count * 12, D3D11_BIND_SHADER_RESOURCE, 0, verts) ||
            !make_buffer(g.tris_b, u32(tri_rows.size() * 4), D3D11_BIND_SHADER_RESOURCE, 0, tri_rows.data()))
            return false;
        g.cdb_verts = g.verts_b.srv;
        g.cdb_tris = g.tris_b.srv;
    }
    Msg("* [grass] ground bake: %u x %u cells of %.0f m, %u triangle references", g.cells_x, g.cells_z, kCell, refs_total);
    return true;
}

// The grass cover map of the terrain tint: per slot r = blade grass (the stand's presence: its palette weight
// against the community's full-presence alpha), g = herbs and b = shrubs still drawn as cards, a = the dominant
// community / 15. Unknown content contributes nothing, so a mod's grass is never tinted.
void publish_cover(const CDetailManager& dm)
{
    xr_vector<u32> texels(size_t(g.size_x) * g.size_z, 0);
    for (size_t i = 0; i < texels.size(); ++i)
    {
        const DetailSlot& s = g.slots[i];
        const u32 ids[4] = {s.id0, s.id1, s.id2, s.id3};
        float r = 0.f, gr = 0.f, b = 0.f, best = 0.f;
        u32 dominant = 0;
        for (u32 k = 0; k < 4; ++k)
        {
            if (ids[k] == DetailSlot::ID_Empty || ids[k] >= dm.objects.size())
                continue;
            const DetailPalette& pal = s.palette[k];
            const float a = float(pal.a0 + pal.a1 + pal.a2 + pal.a3) / 60.f;
            const u32 cls = dm.objects[ids[k]]->m_class;
            if (const u32 c1 = g.id_profile[ids[k]])
            {
                const DaGrassCommunity& C = da_season_data().communities[c1 - 1u];
                r += a / std::max(C.presence, 0.05f) * C.cover;
                if (a > best)
                {
                    best = a;
                    dominant = c1 - 1u;
                }
            }
            else if (cls == dcForb || cls == dcHerb || cls == dcFern || cls == dcHorsetail)
                gr += a;
            else if (cls == dcShrub)
                b += a;
        }
        const auto q = [](float v) { return u32(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        texels[i] = q(r) | (q(gr) << 8) | (q(b) << 16) | (q(float(dominant) / 15.f) << 24);
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = g.size_x;
    desc.Height = g.size_z;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{texels.data(), g.size_x * 4, 0};
    ID3D11Texture2D* surface = nullptr;
    if (FAILED(HW.pDevice->CreateTexture2D(&desc, &data, &surface)) || !surface)
        return;
    if (!g_cover)
        g_cover = RImplementation.Resources->_CreateTexture("$user$grass_cover");
    g_cover->surface_set(surface);
    _RELEASE(surface);
}
} // namespace

namespace
{
// ---- Tables of the frame -----------------------------------------------------------------------------
float curve_or(const DaSeasonCurve& c, float day, float fallback) { return c.n ? c.at(day) : fallback; }

// A season key list the data does not have: a summer meadow.
constexpr float kKeyFallback[gkCount] = {1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.1f, 0.2f, 0.f, 1.f, 0.f, 0.f};
constexpr u32 kColourFallback[gcCount] = {0x4D5433, 0x6A6B3A, 0x968355, 0x88785F, 0xA08A64};

Fvector3 linear_of(u32 srgb)
{
    return {srgb_to_linear(float((srgb >> 16) & 255u) / 255.f), srgb_to_linear(float((srgb >> 8) & 255u) / 255.f),
        srgb_to_linear(float(srgb & 255u) / 255.f)};
}

// The season block of profile p at `day` (r3\da_grass_common.h grass_seas).
void fill_season(u32 p, float day, Fvector4* rows, float grass_visible)
{
    const DaGrassKeys& k = g_data.keys[p];
    float v[gkCount];
    for (u32 i = 0; i < gkCount; ++i)
        v[i] = std::max(curve_or(k.s[i], day, kKeyFallback[i]), 0.f);
    // Old heads ride on last season's thatch culms after the rollover: the December head share, weathering
    // away by 15 % a month (the window ends with the new season's heads).
    const float since_dec = da_season_wrap(day - 348.5f) / 30.42f;
    const float old_heads = since_dec < 7.f ? curve_or(k.s[gkHeads], 348.5f, 0.f) * std::min(v[gkThatch], 1.f) *
            std::max(1.f - 0.15f * since_dec, 0.f) : 0.f;
    rows[0].set(std::min(v[gkNewGrowth], 1.f), std::min(v[gkGreen], 1.f), std::min(v[gkCohortGreen], 1.f), std::min(v[gkThatch], 1.f));
    rows[1].set(std::min(v[gkThatchLodge], 1.f), std::min(v[gkStrawLodge], 1.f), std::min(v[gkHeads], 1.f), std::min(v[gkSeedStage], 1.f));
    rows[2].set(std::min(v[gkTipDry], 1.f), std::min(v[gkWet], 1.f), std::min(v[gkFrost], 1.f), v[gkDensity]);
    rows[3].set(std::min(v[gkFlood], 1.f), std::min(v[gkFloodIce], 1.f),
        g_data.profiles[p].winter_stand > 0.5f ? 1.f : grass_visible, old_heads);
    for (u32 c = 0; c < gcCount; ++c)
    {
        const Fvector3 col = k.c[c].empty() ? linear_of(kColourFallback[c]) : k.c[c].at(day);
        rows[4 + c].set(std::max(col.x, 0.f), std::max(col.y, 0.f), std::max(col.z, 0.f), 0.f);
    }
}

// Short or fine graminoid leaves fold two into one strip (GoT); r__detail_height enters, low grass folds more.
bool species_folds(const DaGrassProfile& P, float height) { return P.form < 0.5f && P.leaf_h[1] * height < P.fold_h; }

// The lanes per m2 a species needs at full density (a folded strip is one lane for two leaves), calibrated.
float species_lanes(const DaGrassProfile& P, float height)
{
    return std::max(P.density_leaf * (species_folds(P, height) ? 0.5f : 1.f) + P.density_culm, 0.f) * P.density_cal;
}

// The species block p (r3\da_grass_common.h grass_prof).
void fill_profile(u32 p, Fvector4* r, float height)
{
    const DaGrassProfile& P = g_data.profiles[p];
    for (u32 i = 0; i < kProfF4; ++i)
        r[i].set(0.f, 0.f, 0.f, 0.f);
    if (!P.used)
        return;
    const bool graminoid = P.form < 0.5f;
    r[0].set(P.leaf_h[0], P.leaf_h[1], P.leaf_w[0], P.leaf_w[1]);
    r[1].set(P.culm_h[0], P.culm_h[1], P.culm_w[0], P.culm_w[1]);
    r[2].set(deg2rad(P.leaf_tilt[0]), deg2rad(P.leaf_tilt[1]), deg2rad(P.culm_tilt[0]), deg2rad(P.culm_tilt[1]));
    r[3].set(P.leaf_bend[0], P.leaf_bend[1], P.culm_bend[0], P.culm_bend[1]);
    r[4].set(P.leaf_droop[0], P.leaf_droop[1], P.leaf_stiff, P.culm_stiff);
    r[5].set(P.clump_cell, P.clump_pull, P.clump_face_away, P.cover);
    // Lanes per m2 and the culms' share of them: a folded strip is one lane for two leaves.
    const bool folded = species_folds(P, height);
    const float lanes = std::max(P.density_leaf * (folded ? 0.5f : 1.f) + P.density_culm, 0.f);
    r[6].set(lanes * P.density_cal, lanes > 0.f ? P.density_culm / lanes : 0.f, P.culm_leaves, folded ? 1.f : 0.f);
    r[7].set(P.head_share, deg2rad(P.head_nod[0]), deg2rad(P.head_nod[1]), P.culm_knee);
    // Fold yaw 15 degrees, shade response, clump facing coherence.
    r[8].set(deg2rad(15.f), P.shade_density, P.shade_height, 0.3f);
    r[9].set(P.tussock_h[0], P.tussock_h[1], P.tussock_share, P.tussock_r);
    r[10].set(P.head_type, P.head_len[0], P.head_len[1], P.head_grain);
    r[11].set(P.head_w[0], P.head_w[1], P.head_branches, P.head_petals);
    // Head opening (degrees, the head vertex shader converts), bob of the vertex shader (amplitude per unit of
    // wind, rad/s): a light flutter on top of the lean (the old 0.12 at 4.5 rad/s read as the whole meadow
    // rocking); a forb's stem is stiffer still.
    r[12].set(P.head_open[0], P.head_open[1], graminoid ? 0.05f : 0.03f, 3.2f);
    // Wind gain on the stock tuft bend, bend gain, push gain, downhill lean. The stock wave was tuned for
    // short wide tufts; tall thin blades carry it at two thirds.
    r[13].set(0.65f, 0.35f, 1.f, 0.25f);
    // Gloss live / dead, translucency root / tip (GoT slide 38 read). The gloss scales the sun's specular
    // five times over (combine_1.ps) on a dark albedo: above a few hundredths it paints the blades white.
    r[14].set(0.02f, 0.012f, 0.60f, 0.85f);
    for (u32 i = 0; i < 4; ++i)
        r[15 + i].set(P.seed_ramp[i].x, P.seed_ramp[i].y, P.seed_ramp[i].z, 0.f);
    r[19].set(0.5f, P.outline, P.form, P.head_velvet);
    // The basal pairs: shape, and how many a leaf lane carries (leaf lanes are records of one leaf, or of
    // two in a folded profile; a basal record is always a pair).
    r[20].set(P.basal_h[0], P.basal_h[1], P.basal_w[0], P.basal_w[1]);
    r[21].set(deg2rad(P.basal_tilt[0]), deg2rad(P.basal_tilt[1]), P.basal_bend[0], P.basal_bend[1]);
    const float leaf_records = P.density_leaf * (folded ? 0.5f : 1.f);
    const float basal_per_leaf = (graminoid && leaf_records > 0.f) ? std::max(P.density_basal, 0.f) * 0.5f / leaf_records : 0.f;
    r[22].set(P.basal_droop[0], P.basal_droop[1], std::min(basal_per_leaf, 3.f), deg2rad(P.clump_lean));
    r[23].set(P.cauline_h[0], P.cauline_h[1], P.cauline_w[0], P.cauline_w[1]);
    r[24].set(deg2rad(P.cauline_tilt[0]), deg2rad(P.cauline_tilt[1]), P.cauline_span[0], P.cauline_span[1]);
    r[25].set(P.cauline_taper, P.cauline_arrange, P.cauline_bend, P.cauline_droop);
    r[26].set(P.stem_tint.x, P.stem_tint.y, P.stem_tint.z, P.dead_vary);
    // The widest a lane of the species can be for the pixel rule (da_grass_gen.cs's early out): a leaf, or a stem
    // with its leaves and head over its shortest height (a head over a third of it, the stem growing in spring).
    const float stem_h = std::max(P.culm_h[0], 0.05f);
    const float stem = P.culm_w[1] + P.culm_leaves * P.cauline_h[1] * P.cauline_w[1] / stem_h +
        P.head_w[1] * 0.6f * P.head_len[1] / std::max(0.3f * stem_h, 0.05f);
    r[27].set(1.2f * std::max(P.leaf_w[1], P.density_culm > 0.f ? stem : 0.f), std::max(P.water_depth, 0.f), 0.f, 0.f);
}

// The community block c (r3\da_grass_common.h grass_comm): the species ids ride as raw bits.
void fill_community(u32 c, Fvector4* r)
{
    for (u32 i = 0; i < kCommF4; ++i)
        r[i].set(0.f, 0.f, 0.f, 0.f);
    if (c >= da_grass_communities || !g_data.communities[c].used)
        return;
    const DaGrassCommunity& C = g_data.communities[c];
    r[0].set(C.patch, C.scattered, float(C.count), C.presence);
    float cum[da_community_species];
    float acc = 0.f;
    for (u32 i = 0; i < da_community_species; ++i)
    {
        acc += i < C.count ? C.share[i] : 0.f;
        cum[i] = i + 1 >= C.count ? 1.f : acc;
    }
    r[1].set(cum[0], cum[1], cum[2], cum[3]);
    r[2].set(cum[4], cum[5], cum[6], cum[7]);
    // The forbs' block: cumulative scattered shares and patch areas (the generation finds a lane's forb in them),
    // the share of the lanes each takes in its patches, and the largest of those (lanes above it skip the patch).
    float scattered[da_community_forbs], area[da_community_forbs], dense[da_community_forbs], dense_max = 0.f;
    float sum_s = 0.f, sum_a = 0.f;
    u32 forb_ids[4]{};
    for (u32 f = 0; f < da_community_forbs; ++f)
    {
        const bool used = f < C.forbs;
        sum_s += used ? C.forb_scattered[f] : 0.f;
        sum_a += used ? C.forb_area[f] : 0.f;
        scattered[f] = used ? sum_s : 2.f;
        area[f] = used ? sum_a : 2.f;
        dense[f] = used ? C.forb_dense[f] : 0.f;
        dense_max = std::max(dense_max, dense[f]);
        if (used)
            forb_ids[f >> 2] |= u32(C.forb[f]) << ((f & 3) * 8);
    }
    u32 words[4]{};
    for (u32 i = 0; i < C.count; ++i)
        words[i >> 2] |= u32(C.species[i]) << ((i & 3) * 8);
    words[2] = C.forbs;
    CopyMemory(&words[3], &dense_max, sizeof(float));
    CopyMemory(&r[3], words, sizeof(words));
    r[4].set(C.litter, C.litter_h[0], C.litter_h[1], C.litter_w);
    r[5].set(C.forb_patch, std::clamp(C.cover, 0.f, 1.f), sum_s, sum_a);
    CopyMemory(&r[6], forb_ids, sizeof(forb_ids));
    for (u32 i = 0; i < 4; ++i)
    {
        r[7 + i].set(scattered[4 * i], scattered[4 * i + 1], scattered[4 * i + 2], scattered[4 * i + 3]);
        r[11 + i].set(area[4 * i], area[4 * i + 1], area[4 * i + 2], area[4 * i + 3]);
        r[15 + i].set(dense[4 * i], dense[4 * i + 1], dense[4 * i + 2], dense[4 * i + 3]);
    }
}

void fill_tables(float day, float day_prev, float height, float grass_visible)
{
    for (u32 p = 0; p < da_grass_species; ++p)
    {
        fill_profile(p, &g.tables[p * kProfF4], height);
        fill_season(p, day, &g.tables[kTabSeas + p * kSeasF4], grass_visible);
        fill_season(p, day_prev, &g.tables[kTabSeasPrev + p * kSeasF4], grass_visible);
    }
    for (u32 c = 0; c < kCommRows; ++c)
        fill_community(c, &g.tables[kTabComm + c * kCommF4]);
    u32 ids[dm_max_objects]{};
    for (u32 i = 0; i < dm_max_objects; ++i)
        ids[i] = g.id_profile[i];
    CopyMemory(&g.tables[kTabIds], ids, sizeof(ids));
}

// The canopy colour of species p at the day (linear): its live, thatch and dead shares.
Fvector3 species_canopy(u32 p, float day)
{
    const DaGrassKeys& k = g_data.keys[p];
    const float green = std::clamp(curve_or(k.s[gkGreen], day, 1.f), 0.f, 1.f);
    const float thatch = std::clamp(curve_or(k.s[gkThatch], day, 0.f), 0.f, 1.f - green);
    const float dead = std::max(1.f - green - thatch, 0.f);
    const auto colour = [&k, day](u32 c) { return k.c[c].empty() ? linear_of(kColourFallback[c]) : k.c[c].at(day); };
    const Fvector3 live = colour(gcLive), th = colour(gcThatch), ye = colour(gcYellowing), st = colour(gcStraw);
    Fvector3 c;
    c.set(green * live.x + thatch * th.x + dead * (0.25f * ye.x + 0.75f * st.x),
        green * live.y + thatch * th.y + dead * (0.25f * ye.y + 0.75f * st.y),
        green * live.z + thatch * th.z + dead * (0.25f * ye.z + 0.75f * st.z));
    return c;
}

// The ground tint of the frame: cover map transform, strength and fade, where the basal pairs fade out
// (the ground under the blades turns from their litter to the stand's under-layer there), the canopy colour
// of every community at the day (its species by their shares; w = grass_visible) and at the reference day of
// the latched terrain folder (G-buffer albedo space, as the terrain textures store it; w = how deep the shade
// under the community's blades is now: their standing growth or lodged thatch against a 0.45 m sward, times
// their cover).
void fill_tint(float day, float R, float grass_visible, float height)
{
    g.tint_on = g.loaded && g_cover;
    if (!g.tint_on)
        return;
    g.tint_xf.set(0.5f / float(g.size_x), 0.5f / float(g.size_z), float(g.offs_x) / float(g.size_x),
        float(g.offs_z) / float(g.size_z));
    const DaGroundTintData& t = g_data.tint;
    g.tint.set(t.near_k, t.far_k, t.start * R, t.end * R + t.end_margin);
    // The frame latch's LOD distances (da_grass_frame_latch); da_grass_gen.cs fades the basal pairs over them.
    const PresetColumns& preset = kPresets[g.preset];
    const float d1 = std::min(preset.d1, R);
    const float d2 = std::clamp(preset.d2, d1 + 1.f, std::max(R, d1 + 1.f));
    g.tint2.set(d1, d1 + 0.35f * (d2 - d1), 0.f, 0.f);
    // Autumn is the base set (no folder): ref[0].
    float ref = t.ref[0];
    if (pcstr folder = da_season_folder())
    {
        static constexpr pcstr names[4] = {"spring_early", "spring_mid", "spring_late", "summer"};
        for (u32 i = 0; i < 4; ++i)
            if (!xr_strcmp(folder, names[i]))
                ref = t.ref[i + 1];
    }
    // The mix moves with the day: a few hundredths of a day are not worth the curves of every species.
    const bool same = std::abs(da_season_wrap(day - g.tint_day + 182.5f) - 182.5f) < 0.02f && ref == g.tint_ref_day &&
        height == g.tint_height;
    if (!same)
    {
        g.tint_day = day;
        g.tint_ref_day = ref;
        g.tint_height = height;
        Fvector3 now[da_grass_species]{}, then[da_grass_species]{};
        for (u32 p = 0; p < da_grass_species; ++p)
            if (g_data.profiles[p].used)
            {
                now[p] = species_canopy(p, day);
                then[p] = species_canopy(p, ref);
            }
        for (u32 c = 0; c < 16; ++c)
        {
            g.canopy[c].set(0.f, 0.f, 0.f, 0.f);
            g.canopy_ref[c].set(0.f, 0.f, 0.f, 0.f);
            if (c >= da_grass_communities || !g_data.communities[c].used)
                continue;
            const DaGrassCommunity& C = g_data.communities[c];
            Fvector3 a{}, b{};
            float shade = 0.f;
            const float forbs = C.forb_total();
            for (u32 i = 0; i < C.count + C.forbs; ++i)
            {
                const u32 p = i < C.count ? C.species[i] : C.forb[i - C.count];
                const float w = i < C.count ? C.share[i] * (1.f - forbs) : C.forb_lanes(i - C.count);
                a.mad(now[p], w);
                b.mad(then[p], w);
                const DaGrassProfile& P = g_data.profiles[p];
                const DaGrassKeys& k = g_data.keys[p];
                const float growth = std::max(curve_or(k.s[gkNewGrowth], day, 1.f), 0.35f * curve_or(k.s[gkThatch], day, 0.f));
                const float top = P.form < 0.5f ? P.leaf_h[1] : 0.6f * P.culm_h[1];
                shade += w * std::clamp(top * growth * height / 0.45f, 0.f, 1.f) * P.cover;
            }
            g.canopy[c].set(linear_to_srgb(a.x), linear_to_srgb(a.y), linear_to_srgb(a.z), 1.f);
            g.canopy_ref[c].set(linear_to_srgb(b.x), linear_to_srgb(b.y), linear_to_srgb(b.z), shade);
            g.canopy_shade[c] = shade;
        }
    }
    // grass_visible moves every frame (the snow): only the w columns follow it.
    for (u32 c = 0; c < 16; ++c)
    {
        const bool used = c < da_grass_communities && g_data.communities[c].used;
        g.canopy[c].w = used ? grass_visible : 0.f;
        g.canopy_ref[c].w = used ? g.canopy_shade[c] * grass_visible : 0.f;
    }
}

// ---- Tiles of the frame (detail calc task) -----------------------------------------------------------
struct Tile
{
    float dn;
    int x, z;
    u32 level, k, flags, lanes;
};

u32 ring_level(float d, float R0) { return d < 2.f * R0 ? 0u : u32(std::floor(std::log2(d / R0))); }

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

float box_distance_2d(const Fvector& c, float x0, float z0, float x1, float z1)
{
    const float dx = std::max({x0 - c.x, 0.f, c.x - x1});
    const float dz = std::max({z0 - c.z, 0.f, c.z - z1});
    return std::sqrt(dx * dx + dz * dz);
}

// The shadow of a box (grass on it, the sun's direction) can fall into the view.
bool casts_into_view(const Latch& L, Fbox box)
{
    Fvector ld;
    ld.invert(L.sun);
    const float down = std::max(-ld.y, 0.1f);
    Fvector offset;
    offset.mul(ld, std::min((box.vMax.y - box.vMin.y) / down, 40.f));
    Fbox shifted = box;
    shifted.vMin.add(offset);
    shifted.vMax.add(offset);
    box.merge(shifted);
    u32 mask = L.view.getMask();
    return L.view.testAABB(box.data(), mask) != fcvNone;
}

void build_tiles()
{
    const Latch& L = g.latch;
    g.groups.clear();
    g.group_count = g.lane_count = 0;
    // (Under the winter's snow no meadow grows - its communities' densities are zero - but a reed bed stands.)
    if (L.dlat <= 0.f)
    {
        g.green.store(0.f, std::memory_order_relaxed);
        return;
    }
    const float s0 = L.s0;
    const float near_reach = L.shadow_near ? kNearReach : 0.f;
    const float reach = std::max({L.R, near_reach, L.shadow_mid ? L.mid_reach : 0.f});
    const u32 k_max = std::min(ring_level(reach, L.R0) / 2u, 6u);
    const float hmax = g.level_hmax * L.height;
    // Shadow-only tiles start at the level their (coarser) shadow keep needs.
    const auto shadow_level = [&L](float texel) {
        const float keep = std::min(1.f, g.max_area_width / std::max(texel, 1e-4f)) * (1.f + kBand);
        return keep >= 1.f ? 0u : u32(std::floor(std::log2(1.f / keep)));
    };
    // The casters' widths of da_grass_gen.cs: 2 texels of the near cascade, 3 of the middle one.
    const u32 level_near = shadow_level(2.f * L.near_texel), level_mid = shadow_level(3.f * L.mid_texel);

    thread_local xr_vector<Tile> tiles;
    thread_local xr_vector<std::pair<int, int>> stack_xz;
    thread_local xr_vector<u32> stack_k;
    tiles.clear();
    stack_xz.clear();
    stack_k.clear();
    const int top = 32 << k_max;
    const int x_lo = floor_div(iFloor((L.cam.x - reach) / s0), top) * top, x_hi = iFloor((L.cam.x + reach) / s0);
    const int z_lo = floor_div(iFloor((L.cam.z - reach) / s0), top) * top, z_hi = iFloor((L.cam.z + reach) / s0);
    for (int z = z_lo; z <= z_hi; z += top)
        for (int x = x_lo; x <= x_hi; x += top)
        {
            stack_xz.emplace_back(x, z);
            stack_k.push_back(k_max);
        }
    u64 total_groups = 0;
    while (!stack_k.empty())
    {
        const auto [x, z] = stack_xz.back();
        const u32 k = stack_k.back();
        stack_xz.pop_back();
        stack_k.pop_back();
        const int T = 32 << k;
        const float x0 = float(x) * s0, z0 = float(z) * s0, x1 = float(x + T) * s0, z1 = float(z + T) * s0;
        const float dn = box_distance_2d(L.cam, x0, z0, x1, z1);
        if (dn > reach)
            continue;
        const u32 ring = ring_level(dn, L.R0);
        if (k > 0 && ring < 2 * k)
        {
            const int h = T / 2;
            for (int c = 0; c < 4; ++c)
            {
                stack_xz.emplace_back(x + (c & 1) * h, z + (c >> 1) * h);
                stack_k.push_back(k - 1);
            }
            continue;
        }
        // The densest blade grass the tile's slots can grow now, and their height range.
        const int sx0 = std::max(iFloor(x0 / DETAIL_SLOT_SIZE) + g.offs_x, 0);
        const int sx1 = std::min(iFloor(x1 / DETAIL_SLOT_SIZE) + g.offs_x, int(g.size_x) - 1);
        const int sz0 = std::max(iFloor(z0 / DETAIL_SLOT_SIZE) + g.offs_z, 0);
        const int sz1 = std::min(iFloor(z1 / DETAIL_SLOT_SIZE) + g.offs_z, int(g.size_z) - 1);
        float dmax = 0.f, ymin = flt_max, ymax = -flt_max;
        for (int sz = sz0; sz <= sz1; ++sz)
            for (int sx = sx0; sx <= sx1; ++sx)
            {
                const size_t i = size_t(sz) * g.size_x + size_t(sx);
                const u32 info = g.slot_info[i];
                if (!(info & 0x0F0F0F0Fu))
                    continue;
                // The slot's presence is the summed weight of its blade objects (da_grass_gen.cs).
                float total = 0.f;
                for (u32 id = 0; id < 4; ++id)
                    total += ((info >> (8 * id)) & 15u) ? float((info >> (8 * id + 4)) & 15u) / 15.f : 0.f;
                // Full presence near the camera, the palette's weight from d1 to d2 on.
                const float far_k = std::clamp((dn - 0.75f * L.d1) / std::max(L.d2 - 0.75f * L.d1, 1.f), 0.f, 1.f);
                for (u32 id = 0; id < 4; ++id)
                {
                    const u32 comm = (info >> (8 * id)) & 15u;
                    if (!comm)
                        continue;
                    const DaGrassCommunity& C = g_data.communities[comm - 1];
                    const float near_p = std::min(total / std::max(C.presence, 0.05f), 1.f);
                    const float presence = near_p + (std::min(total, 1.f) - near_p) * far_k;
                    dmax = std::max(dmax, presence * C.cover * L.dens_now[comm - 1]);
                }
                const DetailSlot& s = g.slots[i];
                ymin = std::min(ymin, s.r_ybase());
                ymax = std::max(ymax, s.r_ybase() + s.r_yheight());
            }
        if (dmax <= 0.f)
            continue;
        const u32 j = dmax >= L.dlat ? 0u : u32(std::floor(std::log2(L.dlat / dmax)));
        // Lanes leave their lattice cell by the jitter and the clump pull: the box takes them in.
        Fbox box;
        box.vMin.set(x0 - 1.f, ymin, z0 - 1.f);
        box.vMax.set(x1 + 1.f, ymax + hmax, z1 + 1.f);
        u32 mask = L.view.getMask();
        const bool view = dn < L.R && L.view.testAABB(box.data(), mask) != fcvNone;
        const bool want_near = near_reach > 0.f && dn < near_reach;
        const bool want_mid = L.shadow_mid && dn < L.mid_reach;
        const bool cast = !view && (want_near || want_mid) && casts_into_view(L, box);
        const bool sh_near = want_near && (view || cast), sh_mid = want_mid && (view || cast);
        if (!view && !sh_near && !sh_mid)
            continue;
        u32 level = view ? ring + j : (sh_near ? level_near : level_mid) + j;
        level = std::min({level, 2 * k + 9, kLmax - 1});
        const u32 n = u32(32 << k) >> (level >> 1);
        const u32 lanes = (level & 1) ? n * (n / 2) : n * n;
        tiles.push_back({dn, x, z, level, k, (view ? 1u : 0u) | (sh_near ? 2u : 0u) | (sh_mid ? 4u : 0u), lanes});
        total_groups += (lanes + 63) / 64;
    }
    // Nearest tiles first: over the group budget, and in a bin that overflows (the records go out in about the
    // order of the groups), the far ones lose.
    std::sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) { return a.dn < b.dn; });
    g.groups.reserve(size_t(std::min<u64>(total_groups, kMaxGroups)) * 4);
    for (const Tile& t : tiles)
    {
        const u32 groups = (t.lanes + 63) / 64;
        if (g.group_count + groups > kMaxGroups)
            break;
        for (u32 off = 0; off < t.lanes; off += 64)
            g.groups.insert(g.groups.end(), {u32(t.x), u32(t.z), t.level | (t.k << 8) | (t.flags << 16), off | (t.lanes << 16)});
        g.group_count += groups;
        g.lane_count += t.lanes;
    }

    // How much blade grass grows around the camera now (the vegetation audio's "green").
    float green = 0.f, slots = 0.f;
    const int cx = iFloor(L.cam.x / DETAIL_SLOT_SIZE) + g.offs_x, cz = iFloor(L.cam.z / DETAIL_SLOT_SIZE) + g.offs_z;
    for (int sz = cz - 6; sz <= cz + 6; ++sz)
        for (int sx = cx - 6; sx <= cx + 6; ++sx)
        {
            slots += 1.f;
            if (sx < 0 || sz < 0 || sx >= int(g.size_x) || sz >= int(g.size_z))
                continue;
            const u32 info = g.slot_info[size_t(sz) * g.size_x + size_t(sx)];
            float best = 0.f;
            for (u32 id = 0; id < 4; ++id)
            {
                const u32 comm = (info >> (8 * id)) & 15u;
                if (comm && g.profile_dens[comm - 1] > 0.f)
                    best = std::max(best, std::min(float((info >> (8 * id + 4)) & 15u) / 15.f /
                        std::max(g_data.communities[comm - 1].presence, 0.05f), 1.f) * L.dens_now[comm - 1] /
                        (g.profile_dens[comm - 1] * L.density_scale));
            }
            green += std::min(best, 1.f);
        }
    g.green.store(slots > 0.f ? green / slots : 0.f, std::memory_order_relaxed);
}

// ---- Compute and draw helpers --------------------------------------------------------------------------
using SrvBinding = std::pair<u32, ID3D11ShaderResourceView*>;

void dispatch(CBackend& cmd, const ref_shader& shader, u32 gx, u32 gy, std::initializer_list<SrvBinding> srvs,
    std::initializer_list<ID3D11UnorderedAccessView*> uavs, const std::function<void()>& constants)
{
    SPass& P = *shader->E[0]->passes[0];
    cmd.set_States(P.state);
    cmd.set_CS(P.cs);
    cmd.set_Constants(P.constants);
    if (P.T)
        cmd.set_Textures(P.T);
    constants();
    for (const auto& [slot, srv] : srvs)
        cmd.SRVSManager.SetCSResource(slot, srv);
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    ID3D11UnorderedAccessView* bound[8]{};
    const UINT initial[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    u32 count = 0;
    for (ID3D11UnorderedAccessView* uav : uavs)
        bound[count++] = uav;
    ctx->CSSetUnorderedAccessViews(0, count, bound, initial);
    cmd.Compute(gx, gy, 1);
    ID3D11UnorderedAccessView* none[8]{};
    ctx->CSSetUnorderedAccessViews(0, count, none, initial);
    for (const auto& [slot, srv] : srvs)
        cmd.SRVSManager.SetCSResource(slot, nullptr);
    cmd.SRVSManager.Apply(cmd.context_id);
    cmd.clear_CS_resources();
}

void set_bins(CBackend& cmd, pcstr name, const u32 (&values)[kBins])
{
    for (u32 i = 0; i < 4; ++i)
        cmd.set_ca(name, i, float(values[i * 4]), float(values[i * 4 + 1]), float(values[i * 4 + 2]), float(values[i * 4 + 3]));
}

// The vertex shaders' constants and resources, then the bins [first, last] near to far.
void draw_bins(CBackend& cmd, const ref_shader& shader, u32 first, u32 last, float texel)
{
    const Latch& L = g.latch;
    cmd.set_Element(shader->E[0]);
    cmd.set_Geometry(g.geom);
    cmd.set_c("da_grass_vcam", L.cam.x, L.cam.y, L.cam.z, L.wmin_per_m);
    cmd.set_c("da_grass_vfwd", L.fwd.x, L.fwd.y, L.fwd.z, L.R);
    cmd.set_c("da_grass_vlod", L.d1, L.d2, L.time, L.time_prev);
    cmd.set_c("da_grass_vsun", L.sun.x, L.sun.y, L.sun.z, texel);
    cmd.set_c("da_grass_vmisc", float(L.frame_mod), L.motion ? 1.f : 0.f, L.height, 0.f);
    cmd.SRVSManager.SetVSResource(0, g.blades_b.srv);
    cmd.SRVSManager.SetVSResource(1, g.tables_b.srv);
    cmd.SRVSManager.SetVSResource(2, g.counts_b.srv);
    cmd.SRVSManager.SetVSResource(3, g.heads_b.srv);
    for (u32 b = first; b <= last; ++b)
    {
        if (!L.cap[b] || kBinClass[b] == vcNone)
            continue;
        const VertexClass& vc = g_vclass[kBinClass[b]];
        cmd.set_c("da_grass_draw", float(L.base[b]), float(b), float(vc.verts), float(kBinLod[b]));
        cmd.RenderIndirect(g.args_b.buf, b * 20, 1);
    }
    for (u32 slot = 0; slot < 4; ++slot)
        cmd.SRVSManager.SetVSResource(slot, nullptr);
    cmd.SRVSManager.Apply(cmd.context_id);
}

// ---- The ground window and the push field -----------------------------------------------------------
void update_ground(CBackend& cmd)
{
    const Latch& L = g.latch;
    const float texel = L.R > 240.f ? 0.5f : 0.25f;
    const u32 size = 2.f * (L.R + 16.f) / texel <= 1024.f ? 1024u : 2048u;
    if (g.ground.size != size || g.gtexel != texel)
    {
        if (!make_texture(g.ground, size, DXGI_FORMAT_R32_FLOAT, 4, false))
            return;
        g.gtexel = texel;
        g.gvalid = false;
    }
    // The window moves in steps of 32 texels; what it moved onto is baked, the rest stays.
    const int nx = floor_div(iFloor(L.cam.x / texel), 32) * 32 - int(size / 2);
    const int nz = floor_div(iFloor(L.cam.z / texel), 32) * 32 - int(size / 2);
    struct Rect
    {
        int x, z, w, h;
    };
    Rect rects[2]{};
    u32 count = 0;
    const int S = int(size);
    if (!g.gvalid || std::abs(nx - g.gx) >= S || std::abs(nz - g.gz) >= S)
        rects[count++] = {nx, nz, S, S};
    else
    {
        const int dx = nx - g.gx, dz = nz - g.gz;
        if (dx > 0)
            rects[count++] = {g.gx + S, nz, dx, S};
        else if (dx < 0)
            rects[count++] = {nx, nz, -dx, S};
        const int xs = std::max(nx, g.gx), xe = std::min(nx, g.gx) + S;
        if (dz > 0 && xe > xs)
            rects[count++] = {xs, g.gz + S, xe - xs, dz};
        else if (dz < 0 && xe > xs)
            rects[count++] = {xs, nz, xe - xs, -dz};
    }
    g.gx = nx;
    g.gz = nz;
    g.gvalid = true;
    for (u32 i = 0; i < count; ++i)
    {
        const Rect& r = rects[i];
        dispatch(cmd, g.s_ground, u32(r.w + 7) / 8, u32(r.h + 7) / 8,
            {{0, g.slots_b.srv}, {1, g.tables_b.srv}, {2, g.cdb_verts}, {3, g.cdb_tris}, {4, g.cells_b.srv}, {5, g.refs_b.srv}},
            {g.ground.uav}, [&] {
                cmd.set_c("da_gg_rect", float(r.x), float(r.z), float(r.w), float(r.h));
                cmd.set_c("da_gg_win", texel, float(size), kCell, 0.f);
                cmd.set_c("da_gg_grid", g.cells_origin.x, g.cells_origin.y, float(g.cells_x), float(g.cells_z));
                cmd.set_c("da_grass_slot", float(g.offs_x), float(g.offs_z), float(g.size_x), float(g.size_z));
            });
    }
}

Fvector4 push_window(u32 i)
{
    const float extent = float(kPushSize) * kPushTexel;
    return {1.f / extent, 0.5f * extent - kPushTexel, (float(g.px[i]) + 0.5f * float(kPushSize)) * kPushTexel,
        (float(g.pz[i]) + 0.5f * float(kPushSize)) * kPushTexel};
}

void update_push(CBackend& cmd)
{
    const Latch& L = g.latch;
    const u32 cur = g.pcur ^ 1u, prev = g.pcur;
    g.px[cur] = iFloor(L.cam.x / kPushTexel) - int(kPushSize / 2);
    g.pz[cur] = iFloor(L.cam.z / kPushTexel) - int(kPushSize / 2);
    const float half = 0.5f * float(kPushSize) * kPushTexel;
    // This frame's deformers: feet push the grass around the legs, bodies and settled objects flatten it.
    thread_local xr_vector<float> rows;
    rows.clear();
    for (const SSnowStamp& s : da_snow_stamps())
    {
        if (s.kind == 3 || rows.size() / 8 >= kMaxStamps)
            continue;
        if (std::abs(s.xz.x - L.cam.x) > half + 2.f || std::abs(s.xz.y - L.cam.z) > half + 2.f)
            continue;
        const float radius = s.kind == 0 ? std::max(s.radius * 2.2f, 0.35f) : std::max(s.radius, 0.1f);
        const float strength = s.kind == 1 ? 0.85f : 1.f;
        rows.insert(rows.end(), {s.xz.x, s.xz.y, s.foot_y, radius, s.push.x, s.push.y, strength, 0.f});
    }
    const u32 stamps = u32(rows.size() / 8);
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    const Fvector4 win_cur{kPushTexel, float(kPushSize), float(g.px[cur]), float(g.pz[cur])};
    if (stamps)
    {
        const D3D11_BOX box{0, 0, 0, stamps * kStampBytes, 1, 1};
        ctx->UpdateSubresource(g.stamps_b.buf, 0, &box, rows.data(), 0, 0);
        dispatch(cmd, g.s_stamp, stamps, 1, {{0, g.stamps_b.srv}, {3, g.ground.srv}}, {g.stamp.uav}, [&] {
            cmd.set_c("da_gp_win", win_cur);
            cmd.set_c("da_gp_count", float(stamps), 0.f, 0.f, 0.f);
            cmd.set_c("da_grass_gwin", float(g.gx), float(g.gz), float(g.ground.size), g.gtexel);
        });
    }
    const float decay = std::exp(-Device.fTimeDelta / kPushTau);
    dispatch(cmd, g.s_resolve, kPushSize / 8, kPushSize / 8, {{0, g.push[prev].srv}}, {g.stamp.uav, g.push[cur].uav}, [&] {
        cmd.set_c("da_gp_win", win_cur);
        cmd.set_c("da_gp_win_prev", kPushTexel, float(kPushSize), float(g.px[prev]), float(g.pz[prev]));
        cmd.set_c("da_gp_decay", decay, g.pvalid ? 1.f : 0.f, 1.f - std::exp(-Device.fTimeDelta / kPushAttack), 0.f);
    });
    g.pcur = cur;
    g.pvalid = true;
}

// The counts of earlier frames: the bins' windows follow what they needed (overflow at once, the rest
// every few seconds), applied at the next latch so a frame never mixes two layouts. Every finished copy is
// read, oldest first (the GPU finishes them in order, so the first busy one ends the poll), and this
// frame's counts go to the next free slot, so the readback keeps up however far the GPU runs behind.
void read_back(ID3D11DeviceContext* ctx)
{
    const u32 frame = Device.dwFrame;
    for (u32 n = 0; n < kReadbacks; ++n)
    {
        const u32 slot = (g.staging_next + n) % kReadbacks;
        if (!g.staging_pending[slot])
            continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(g.staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
            break;
        CopyMemory(g.counts_seen, mapped.pData, sizeof(g.counts_seen));
        ctx->Unmap(g.staging[slot], 0);
        g.staging_pending[slot] = false;
        bool overflow = false;
        float blades = 0.f;
        for (u32 b = 0; b < kBins; ++b)
        {
            if (kBinClass[b] == vcNone)
                continue;
            const float want = std::max(float(g.counts_seen[b]) * 1.3f, 256.f);
            g.need[b] = std::max(want, g.need[b] * 0.995f);
            overflow = overflow || g.counts_seen[b] > g.cap[b];
            if (!bin_is_head(b))
                blades += float(g.counts_seen[b]);
        }
        if (overflow)
            ++g.overflow_frames;
        // The floor follows the blades' share of their buffer towards 85 %: up at once (a dense forb stand comes
        // into view), down slowly so it does not swing.
        const float fill = blades / float(std::max(g.records, 1u));
        if (fill > 0.9f)
            g.floor_k = std::min(g.floor_k * std::min(std::sqrt(fill / 0.85f), 1.25f), 3.f);
        else if (fill < 0.7f)
            g.floor_k = std::max(g.floor_k * 0.985f, 1.f);
        if (overflow || frame - g.rebalance_frame >= 240)
        {
            g.rebalance = true;
            g.rebalance_frame = frame;
        }
    }
    if (g.staging_pending[g.staging_next])
        return;
    ctx->CopyResource(g.staging[g.staging_next], g.counts_b.buf);
    g.staging_pending[g.staging_next] = true;
    g.staging_next = (g.staging_next + 1) % kReadbacks;
}
} // namespace

// ======================================================================================================
// Interface
// ======================================================================================================
bool da_grass_blades_available()
{
    static const bool present = shaders_present();
    return present && !g.failed;
}

bool da_grass_active() { return g.active && psDeviceFlags.is(rsDrawDetails); }

void da_grass_apply_preset(u32 preset) { g.preset = std::min(preset, 4u); }

void da_grass_level_unload()
{
    release_all();
    g.failed = false;
}

void da_grass_destroy()
{
    release_all();
    g.failed = false;
    // The CTexture outlives levels (compiled terrain passes point at it), never the resource manager.
    g_cover = nullptr;
}

namespace
{
// The level's reed beds (da_grass_set_reed_beds): they outlive a reload of the blades (a renderer reset).
xr_vector<DaReedBed> s_reed_beds;
// Metres the culms of a reed bed stand taller for the water they root under (the slot's emergence bits).
float s_reed_emerge_max = 0.f;
// How far the culms rise per metre of water over their roots (dead_air_x64_reeds.ltx [reed_beds] emerge).
float s_reed_emerge_k = 1.f;

// The reed beds into the blades' copy of the slots. A community the beds grow takes a free object id of the level's
// (from the top of the table down past its own objects); a slot under a bed carries that id with the beds' cover at
// its four corners (soft edges, the bed's own patches do the rest), its other objects giving way by as much; its box
// reaches down to the bed, so the ground window finds it under the water (the water mesh is passable); its colour bits
// carry how far the culms rise for the water (red: 0.1 m units, the deepest bed's), which of its four objects are the
// beds' (green: a mask) and how far the box went down (blue: 0.2 m units - the meadow's objects keep to the stock box,
// or they grew up through the water from the bed). The stock decompress never sees the copy.
void apply_reed_beds(const CDetailManager& dm)
{
    s_reed_emerge_max = 0.f;
    if (s_reed_beds.empty())
        return;
    u8 vid_of[da_grass_communities];
    std::fill(std::begin(vid_of), std::end(vid_of), u8(DetailSlot::ID_Empty));
    u32 next = DetailSlot::ID_Empty - 1;
    for (const DaReedBed& b : s_reed_beds)
    {
        if (b.community < 0 || b.community >= s32(da_grass_communities) || !g_data.communities[b.community].used ||
            vid_of[b.community] != DetailSlot::ID_Empty)
            continue;
        if (next < dm.objects.size())
        {
            Msg("! [grass] no free detail id for the reed community '%s'", da_grass_community_name(u32(b.community)));
            continue;
        }
        vid_of[b.community] = u8(next);
        g.id_profile[next] = u8(b.community + 1);
        --next;
    }
    // Cover of a community at a point: full within three quarters of a bed's radius, nothing three quarters of a
    // metre past it (a slot's corner is two metres from the next).
    const auto cover = [](const DaReedBed& b, float x, float z) {
        const float d = _sqrt((x - b.x) * (x - b.x) + (z - b.z) * (z - b.z));
        const float r_in = 0.75f * b.r, r_out = b.r + 0.75f;
        return d <= r_in ? 1.f : d >= r_out ? 0.f : 1.f - (d - r_in) / (r_out - r_in);
    };
    const auto q = [](float v) { return u16(std::clamp(iFloor(v * 15.f + 0.5f), 0, 15)); };
    u32 slots = 0;
    for (const DaReedBed& b : s_reed_beds)
    {
        if (b.community < 0 || b.community >= s32(da_grass_communities) || vid_of[b.community] == DetailSlot::ID_Empty)
            continue;
        const float reach = b.r + 0.75f;
        const int sx0 = std::max(iFloor((b.x - reach) / DETAIL_SLOT_SIZE) + g.offs_x, 0);
        const int sx1 = std::min(iFloor((b.x + reach) / DETAIL_SLOT_SIZE) + g.offs_x, int(g.size_x) - 1);
        const int sz0 = std::max(iFloor((b.z - reach) / DETAIL_SLOT_SIZE) + g.offs_z, 0);
        const int sz1 = std::min(iFloor((b.z + reach) / DETAIL_SLOT_SIZE) + g.offs_z, int(g.size_z) - 1);
        const u8 vid = vid_of[b.community];
        for (int sz = sz0; sz <= sz1; ++sz)
            for (int sx = sx0; sx <= sx1; ++sx)
            {
                DetailSlot& S = g.slot_copy[size_t(sz) * g.size_x + size_t(sx)];
                const float x0 = float(sx - g.offs_x) * DETAIL_SLOT_SIZE, z0 = float(sz - g.offs_z) * DETAIL_SLOT_SIZE;
                const float x1 = x0 + DETAIL_SLOT_SIZE, z1 = z0 + DETAIL_SLOT_SIZE;
                // Corners in the order of the palette's nibbles (r3 da_grass_gen.cs: a0 (x0, z0), a1 (x1, z0),
                // a2 (x0, z1), a3 (x1, z1)).
                const float cv[4] = {cover(b, x0, z0), cover(b, x1, z0), cover(b, x0, z1), cover(b, x1, z1)};
                if (std::max({cv[0], cv[1], cv[2], cv[3]}) <= 0.f)
                    continue;
                // Its entry: the community's own, else an empty one, else the weakest of the slot's.
                u32 k = 4, weakest = 0, weakest_sum = ~0u;
                bool bare = true;
                for (u32 e = 0; e < 4; ++e)
                {
                    const u32 id = S.r_id(e);
                    if (id == vid)
                        k = e;
                    const DetailPalette& pal = S.palette[e];
                    const u32 sum = id == DetailSlot::ID_Empty ? 0u : u32(pal.a0 + pal.a1 + pal.a2 + pal.a3);
                    bare = bare && sum == 0u;
                    if (sum < weakest_sum)
                    {
                        weakest_sum = sum;
                        weakest = e;
                    }
                }
                const bool fresh = k == 4;
                if (fresh)
                    k = weakest;
                DetailPalette& P = S.palette[k];
                const float was[4] = {fresh ? 0.f : P.a0 / 15.f, fresh ? 0.f : P.a1 / 15.f, fresh ? 0.f : P.a2 / 15.f,
                    fresh ? 0.f : P.a3 / 15.f};
                S.w_id(k, vid);
                P.a0 = q(std::max(was[0], cv[0]));
                P.a1 = q(std::max(was[1], cv[1]));
                P.a2 = q(std::max(was[2], cv[2]));
                P.a3 = q(std::max(was[3], cv[3]));
                // The meadow gives way under the reeds (the beds' own entries keep theirs).
                for (u32 e = 0; e < 4; ++e)
                {
                    const u32 id = S.r_id(e);
                    if (e == k || id == DetailSlot::ID_Empty || (id > next && id < DetailSlot::ID_Empty))
                        continue;
                    DetailPalette& O = S.palette[e];
                    O.a0 = q(O.a0 / 15.f * (1.f - cv[0]));
                    O.a1 = q(O.a1 / 15.f * (1.f - cv[1]));
                    O.a2 = q(O.a2 / 15.f * (1.f - cv[2]));
                    O.a3 = q(O.a3 / 15.f * (1.f - cv[3]));
                }
                // The box down to the bed, a hand over it at least; a slot the level left bare gets the bed's alone.
                // (Its stock base: the box as an earlier bed left it, raised by what that bed took.)
                const float base = S.r_ybase(), top = base + S.r_yheight(), stock_base = base + float(S.c_b) * 0.2f;
                const float nb = bare ? b.y_bed - 0.3f : std::min(base, b.y_bed - 0.3f);
                const float nt = bare ? b.y_bed + 0.6f : std::max(top, b.y_bed + 0.6f);
                S.w_y(nb, nt - nb);
                S.c_b = bare ? 0u : u32(std::clamp(iFloor((stock_base - S.r_ybase()) / 0.2f + 0.5f), 0, 15));
                // A slot the level left bare has no sky term of its own: an open marsh.
                if (bare && S.c_hemi < 12)
                    S.c_hemi = 12;
                const u32 emerge = u32(std::clamp(iFloor(b.depth / 0.1f + 0.5f), 0, 15));
                S.c_r = std::max(u32(S.c_r), emerge);
                // (green: which of the slot's four objects are beds)
                S.c_g = S.c_g | (1u << k);
                s_reed_emerge_max = std::max(s_reed_emerge_max, float(S.c_r) * 0.1f);
                ++slots;
            }
    }
    Msg("* [grass] %u reed beds grow in %u slot touches (deepest emergence %.1f m)", u32(s_reed_beds.size()), slots,
        s_reed_emerge_max);
}
} // namespace

void da_grass_set_reed_beds(xr_vector<DaReedBed>&& beds, bool reload, float emerge)
{
    s_reed_beds = std::move(beds);
    s_reed_emerge_k = emerge;
    if (reload && RImplementation.Details && RImplementation.Details->m_blades && g_pGameLevel && !g.failed)
    {
        // In a running level the detail calc task builds this frame's tiles from the slots on a worker: done first.
        RImplementation.Details->WaitForCalc();
        da_grass_level_load();
    }
}

void da_grass_level_load()
{
    release_all();
    g.failed = false;
    CDetailManager* dm = RImplementation.Details;
    if (!dm || !dm->m_blades || !dm->dtSlots || !g_pGameLevel)
        return;
    const auto fail = [dm](pcstr why) {
        Msg("! [grass] the blade grass is off on this level (%s): the detail cards draw every class", why);
        release_all();
        g.failed = true;
        dm->SetBladesActive(false);
    };
    if (!shaders_present())
        return fail("the compute shaders are missing");
    if (!create_shaders())
        return fail("the shaders failed to compile");

    g.size_x = dm->dtH.x_size();
    g.size_z = dm->dtH.z_size();
    g.offs_x = int(dm->dtH.x_offs());
    g.offs_z = int(dm->dtH.z_offs());
    g.slot_copy.assign(dm->dtSlots, dm->dtSlots + size_t(g.size_x) * g.size_z);
    g.slots = g.slot_copy.data();
    // The stock colour bits mean nothing to the blades; red, green and blue carry the reed beds' water, mask and the
    // box's drop below.
    for (DetailSlot& S : g.slot_copy)
    {
        S.c_r = 0;
        S.c_g = 0;
        S.c_b = 0;
    }
    for (u32 i = 0; i < dm->objects.size() && i < dm_max_objects; ++i)
        g.id_profile[i] = u8(da_grass_object_profile(*dm->objects[i]) + 1);
    apply_reed_beds(*dm);
    g.level_hmax = 0.3f;
    g.max_area_width = 0.006f;
    for (u32 c = 0; c < da_grass_communities; ++c)
    {
        const DaGrassCommunity& C = g_data.communities[c];
        bool used = false;
        for (u32 i = 0; i < dm_max_objects; ++i)
            used = used || g.id_profile[i] == c + 1;
        g.profile_dens[c] = 0.f;
        if (!used || !C.used)
            continue;
        for (u32 i = 0; i < C.count + C.forbs; ++i)
        {
            const DaGrassProfile& P = g_data.profiles[i < C.count ? C.species[i] : C.forb[i - C.count]];
            g.profile_dens[c] = std::max(g.profile_dens[c], species_lanes(P, ps_r__Detail_height));
            g.level_hmax = std::max({g.level_hmax, P.leaf_h[1], P.culm_h[1] + P.head_len[1]});
            g.max_area_width = std::max({g.max_area_width, P.leaf_w[1], P.culm_w[1] * 2.f});
        }
    }
    // A reed bed's culms stand that much taller for the water over their roots.
    g.level_hmax += s_reed_emerge_max;
    g.slot_info.assign(size_t(g.size_x) * g.size_z, 0);
    bool any = false;
    for (size_t i = 0; i < g.slot_info.size(); ++i)
    {
        const DetailSlot& s = g.slots[i];
        const u32 ids[4] = {s.id0, s.id1, s.id2, s.id3};
        u32 info = 0;
        for (u32 k = 0; k < 4; ++k)
        {
            if (ids[k] == DetailSlot::ID_Empty || ids[k] >= dm_max_objects || !g.id_profile[ids[k]])
                continue;
            const DetailPalette& pal = s.palette[k];
            const u32 amax = std::max({u32(pal.a0), u32(pal.a1), u32(pal.a2), u32(pal.a3)});
            if (amax)
                info |= (u32(g.id_profile[ids[k]]) | (amax << 4)) << (8 * k);
        }
        g.slot_info[i] = info;
        any = any || info;
    }
    if (!any)
        return fail("no slot of the level grows blade grass");

    const UINT srv = D3D11_BIND_SHADER_RESOURCE, uav = D3D11_BIND_UNORDERED_ACCESS;
    const u32 zero_counts[kBins * 2]{};
    const u32 zero_args[kBins * 5]{};
    if (!make_buffer(g.slots_b, u32(g.slot_info.size() * sizeof(DetailSlot)), srv, 0, g.slots) ||
        !make_buffer(g.tables_b, kTabTotal * 16, srv, 0, nullptr) || !reset_tables() ||
        !make_buffer(g.groups_b, kMaxGroups * 16, srv, 0, nullptr) ||
        !make_buffer(g.stamps_b, kMaxStamps * kStampBytes, srv, 0, nullptr) ||
        !make_buffer(g.counts_b, sizeof(zero_counts), srv | uav, 0, zero_counts) ||
        !make_buffer(g.args_b, sizeof(zero_args), uav, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, zero_args))
        return fail("the buffers could not be created");
    for (ID3D11Buffer*& staging : g.staging)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = g.counts_b.bytes;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(HW.pDevice->CreateBuffer(&desc, nullptr, &staging)))
            return fail("the readback buffers could not be created");
    }
    if (!create_ground_cells())
        return fail("the collision mesh is not available");
    if (!make_texture(g.push[0], kPushSize, DXGI_FORMAT_R16G16_FLOAT, 4, true) ||
        !make_texture(g.push[1], kPushSize, DXGI_FORMAT_R16G16_FLOAT, 4, true) ||
        !make_texture(g.stamp, kPushSize, DXGI_FORMAT_R32_UINT, 4, true))
        return fail("the push field could not be created");
    if (!create_bins())
        return fail("the blade buffers could not be created");
    if (!create_geometry())
        return fail("the blade geometry could not be created");
    publish_cover(*dm);
    g.loaded = g.active = true;
    fill_tint(da_season_day(), std::max(dm_fade, 8.f), std::clamp(da_snow_frame().grass_visible, 0.f, 1.f), ps_r__Detail_height);
    Msg("* [grass] blades on: %u x %u slots, %u MB of records", g.size_x, g.size_z,
        (g.records * kRec + g.heads * kHeadRec) >> 20);
}

void da_grass_frame_latch()
{
    if (!g.active)
        return;
    Latch& L = g.latch;
    if (L.frame == Device.dwFrame)
        return;
    CDetailManager* dm = RImplementation.Details;
    if (!dm)
        return;
    const bool continuous = L.frame + 1 == Device.dwFrame && !da_history_reset();
    L.frame = Device.dwFrame;
    L.frame_mod = Device.dwFrame & 63u;
    L.cam = Device.vCameraPosition;
    L.fwd = Device.vCameraDirection;
    L.view.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);
    for (u32 i = 0; i < 6; ++i)
    {
        if (i < L.view.p_count)
            L.planes[i].set(L.view.planes[i].n.x, L.view.planes[i].n.y, L.view.planes[i].n.z, L.view.planes[i].d);
        else
            L.planes[i].set(0.f, 0.f, 0.f, -1e30f);
    }
    const PresetColumns& preset = kPresets[g.preset];
    // A preset with other buffer sizes: the new buffers before anything of this frame records a draw.
    if (g.records != preset.records && !create_bins())
    {
        Msg("! [grass] the blade buffers could not be resized");
        g.active = false;
        return;
    }
    L.R = std::max(dm_fade, 8.f);
    L.d1 = std::min(preset.d1, L.R);
    L.d2 = std::clamp(preset.d2, L.d1 + 1.f, std::max(L.R, L.d1 + 1.f));
    L.kpx = preset.kpx;
    // One render pixel per metre of view depth (the upscaler's input height).
    const float render_h = float(std::max(Device.dwRenderHeight ? Device.dwRenderHeight : Device.dwHeight, 1u));
    L.wmin_per_m = L.kpx * g.floor_k * 2.f * std::tan(deg2rad(Device.fFOV) * 0.5f) / render_h;
    // The full-density ring holds the typical 6 mm leaf above its pixel floor (the supply condition).
    L.R0 = std::clamp(0.006f / std::max(L.wmin_per_m, 1e-6f) * (1.f + kBand), 6.f, 24.f);
    // r__detail_density (a spacing) linear in 1/spacing (D4); r__detail_height scales lengths and the fold.
    L.density_scale = std::clamp(0.25f / std::max(ps_current_detail_density, 0.05f), 0.25f, 1.6f);
    L.height = ps_r__Detail_height;
    // The lattice holds the densest species of the level's communities at the height setting (it decides the folds).
    float dlat = 0.f;
    for (u32 c = 0; c < da_grass_communities; ++c)
    {
        if (!g.profile_dens[c])
            continue;
        const DaGrassCommunity& C = g_data.communities[c];
        float lanes = 0.f;
        for (u32 i = 0; i < C.count + C.forbs; ++i)
            lanes = std::max(lanes, species_lanes(g_data.profiles[i < C.count ? C.species[i] : C.forb[i - C.count]], L.height));
        g.profile_dens[c] = std::max(lanes, 1e-3f);
        dlat = std::max(dlat, lanes * L.density_scale);
    }
    L.dlat = dlat;
    L.s0 = dlat > 0.f ? 1.f / std::sqrt(dlat) : 1.f;
    const float smap = float(std::max(u32(RImplementation.o.smapsize), 256u));
    L.shadow_near = true;
    L.shadow_mid = preset.shadow_mid;
    // A cascade's ortho box spans its size (render_phase_sun.cpp: +/- size / 2): one texel is size / smap.
    L.near_texel = g.floor_k * kNearReach / smap;
    L.mid_reach = float(ps_r__grass_shadow_dist);
    L.mid_texel = g.floor_k * std::max(ps_r__sun_cascade_mid, 1.f) / smap;
    if (g_pGamePersistent)
    {
        L.sun = g_pGamePersistent->Environment().CurrentEnv.sun_dir;
        L.sun.invert();
        if (L.sun.square_magnitude() < 1e-6f)
            L.sun.set(0.f, 1.f, 0.f);
        L.sun.normalize();
    }
    const float now = Device.fTimeGlobal;
    L.time_prev = continuous ? L.time : now;
    L.time = now;
    L.motion = da_motion_vectors_active() || ps_r__motion_vectors;
    const SSnowFrame& snow = da_snow_frame();
    L.snow = snow.active;
    L.grass_visible = std::clamp(snow.grass_visible, 0.f, 1.f);
    const float day = da_season_day(), day_prev = continuous ? da_season_day_prev() : day;
    // Each community's densest species at the day (its season density), the tiles' bound.
    for (u32 c = 0; c < da_grass_communities; ++c)
    {
        L.dens_now[c] = 0.f;
        if (!g.profile_dens[c])
            continue;
        const DaGrassCommunity& C = g_data.communities[c];
        for (u32 i = 0; i < C.count + C.forbs; ++i)
        {
            const u32 p = i < C.count ? C.species[i] : C.forb[i - C.count];
            const DaGrassProfile& P = g_data.profiles[p];
            L.dens_now[c] = std::max(L.dens_now[c], species_lanes(P, L.height) * L.density_scale *
                std::max(curve_or(g_data.keys[p].s[gkDensity], day, 1.f), 0.f) * (P.winter_stand > 0.5f ? 1.f : L.grass_visible));
        }
    }
    for (u32 i = 0; i < 2; ++i)
    {
        const CDetailManager::DumpInputs in = dm->hw_DumpInputs(i);
        L.wave[i * 2] = in.wave;
        L.wave[i * 2 + 1] = in.wind;
        L.wave_prev[i * 2] = in.wave_prev;
        L.wave_prev[i * 2 + 1] = in.wind_prev;
    }
    // View bins: two near, three middle, four far (geometric), near to far.
    const float q = std::pow(std::max(L.R / L.d2, 1.f), 0.25f);
    const float bounds[kViewBins] = {L.d1 * 0.5f, L.d1, L.d1 + (L.d2 - L.d1) / 3.f, L.d1 + 2.f * (L.d2 - L.d1) / 3.f, L.d2,
        L.d2 * q, L.d2 * q * q, L.d2 * q * q * q, L.R + 1.f};
    for (u32 i = 0; i < 12; ++i)
        L.bin_far[i] = i < kViewBins ? bounds[i] : L.R + 1.f;
    // The tables change only with the day, the height setting and the snow's grass visibility: unchanged, they are
    // neither filled nor uploaded again (64 species' keys are a few thousand curve reads a frame).
    if (day != g.tab_day || day_prev != g.tab_day_prev || L.height != g.tab_height || L.grass_visible != g.tab_visible)
    {
        fill_tables(day, day_prev, L.height, L.grass_visible);
        g.tab_day = day;
        g.tab_day_prev = day_prev;
        g.tab_height = L.height;
        g.tab_visible = L.grass_visible;
        g.tab_upload = true;
    }
    fill_tint(day, L.R, L.grass_visible, L.height);
    if (g.rebalance)
    {
        layout_bins(g.need);
        g.rebalance = false;
    }
    std::copy(std::begin(g.base), std::end(g.base), std::begin(L.base));
    std::copy(std::begin(g.cap), std::end(g.cap), std::begin(L.cap));
}

void da_grass_calc(const Fvector&)
{
    if (!g.active || g.latch.frame != Device.dwFrame)
        return;
    build_tiles();
}

float da_grass_green() { return g.active ? g.green.load(std::memory_order_relaxed) : 0.f; }

bool da_grass_brushes(const Fvector& feet, float, float min_height, float below_feet, float above_feet)
{
    if (!g.active || g.latch.grass_visible <= 0.01f)
        return false;
    const int sx = iFloor(feet.x / DETAIL_SLOT_SIZE) + g.offs_x, sz = iFloor(feet.z / DETAIL_SLOT_SIZE) + g.offs_z;
    if (sx < 0 || sz < 0 || sx >= int(g.size_x) || sz >= int(g.size_z))
        return false;
    const DetailSlot& s = g.slots[size_t(sz) * g.size_x + size_t(sx)];
    const float fx = feet.x / DETAIL_SLOT_SIZE - std::floor(feet.x / DETAIL_SLOT_SIZE);
    const float fz = feet.z / DETAIL_SLOT_SIZE - std::floor(feet.z / DETAIL_SLOT_SIZE);
    const u32 ids[4] = {s.id0, s.id1, s.id2, s.id3};
    float height = 0.f;
    for (u32 k = 0; k < 4; ++k)
    {
        if (ids[k] == DetailSlot::ID_Empty || ids[k] >= dm_max_objects || !g.id_profile[ids[k]])
            continue;
        const DetailPalette& pal = s.palette[k];
        const float a = ((float(pal.a0) * (1.f - fx) + float(pal.a1) * fx) * (1.f - fz) +
            (float(pal.a2) * (1.f - fx) + float(pal.a3) * fx) * fz) / 15.f;
        if (a < 0.2f)
            continue;
        // This season's blades of the community: the new growth, or last year's thatch while it stands.
        const DaGrassCommunity& C = g_data.communities[g.id_profile[ids[k]] - 1u];
        float h_comm = 0.f;
        for (u32 i = 0; i < C.count; ++i)
        {
            const u32 p = C.species[i];
            const DaGrassProfile& P = g_data.profiles[p];
            const Fvector4& K0 = g.tables[kTabSeas + p * kSeasF4];
            const float rel = std::max(K0.x, K0.w > 0.3f ? 0.4f : 0.f);
            h_comm += C.share[i] * (P.form < 0.5f ? P.leaf_h[1] : 0.6f * P.culm_h[1]) * rel;
        }
        height = std::max(height, h_comm * g.latch.height * 0.94f);
    }
    if (height < min_height)
        return false;
    const float base = s.r_ybase(), top = base + s.r_yheight();
    if (base > feet.y + above_feet)
        return false;
    const float root = std::clamp(feet.y, base, top);
    if (root + height < feet.y - below_feet)
        return false;
    if (g.latch.snow && da_snow_depth_at(feet.x, feet.z) >= height * 0.8f)
        return false;
    return true;
}

void da_grass_frame_ensure_latch()
{
    if (!da_grass_active() || g.latch.frame == Device.dwFrame)
        return;
    CDetailManager* dm = RImplementation.Details;
    if (!dm || !dm->m_blades)
        return;
    dm->WaitForCalc();
    da_grass_frame_latch();
}

void da_grass_frame_prepare(CBackend& cmd)
{
    if (!da_grass_active())
        return;
    CDetailManager* dm = RImplementation.Details;
    if (!dm || !dm->m_blades)
        return;
    dm->WaitForCalc();
    if (g.latch.frame != Device.dwFrame)
    {
        // A frame without the detail manager's update (a paused menu over the level): this frame's
        // state, last frame's tiles.
        da_grass_frame_latch();
    }
    if (!g.active)
        return;
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    if (g.tab_upload)
    {
        ctx->UpdateSubresource(g.tables_b.buf, 0, nullptr, g.tables, 0, 0);
        g.tab_upload = false;
    }
    if (g.group_count)
    {
        const D3D11_BOX box{0, 0, 0, g.group_count * 16, 1, 1};
        ctx->UpdateSubresource(g.groups_b.buf, 0, &box, g.groups.data(), 0, 0);
    }
    update_ground(cmd);
    update_push(cmd);

    const UINT zero[4]{};
    ctx->ClearUnorderedAccessViewUint(g.counts_b.uav, zero);
#if defined(XR_D3D12)
    xrd3d12::UavBarrier(ctx, nullptr);
#endif
    const Latch& L = g.latch;
    if (g.group_count)
        dispatch(cmd, g.s_gen, g.group_count, 1,
            {{0, g.slots_b.srv}, {1, g.tables_b.srv}, {2, g.groups_b.srv}, {3, g.ground.srv}, {4, g.push[g.pcur].srv},
                {5, g.push[g.pcur ^ 1u].srv}},
            {g.blades_b.uav, g.counts_b.uav, g.heads_b.uav}, [&] {
                cmd.set_c("da_grass_cam", L.cam.x, L.cam.y, L.cam.z, L.R);
                cmd.set_c("da_grass_fwd", L.fwd.x, L.fwd.y, L.fwd.z, L.wmin_per_m);
                const float fade_start = 0.85f * L.R;
                cmd.set_c("da_grass_lod", L.d1, L.d2, fade_start, 1.f / std::max(L.R - fade_start, 0.5f));
                cmd.set_c("da_grass_lat", L.s0, L.dlat, L.density_scale, L.height);
                cmd.set_c("da_grass_time", L.time, L.time_prev, L.motion ? 1.f : 0.f, float(L.frame_mod));
                cmd.set_c("da_grass_sh", L.shadow_near ? kNearReach : 0.f, L.shadow_mid ? L.mid_reach : 0.f, L.near_texel, L.mid_texel);
                cmd.set_c("da_grass_slot", float(g.offs_x), float(g.offs_z), float(g.size_x), float(g.size_z));
                cmd.set_c("da_grass_gwin", float(g.gx), float(g.gz), float(g.ground.size), g.gtexel);
                cmd.set_c("da_grass_pwin", push_window(g.pcur));
                cmd.set_c("da_grass_pwin_prev", push_window(g.pcur ^ 1u));
                cmd.set_c("da_grass_snow", L.snow ? 1.f : 0.f, float(g.pin), float(ps_r__dbg_grass), 0.f);
                cmd.set_c("da_grass_water", s_reed_emerge_k, s_reed_emerge_max, 0.f, 0.f);
                for (u32 i = 0; i < 6; ++i)
                    cmd.set_ca("da_grass_planes", i, L.planes[i]);
                for (u32 i = 0; i < 3; ++i)
                    cmd.set_ca("da_grass_bin_d", i, L.bin_far[i * 4], L.bin_far[i * 4 + 1], L.bin_far[i * 4 + 2], L.bin_far[i * 4 + 3]);
                set_bins(cmd, "da_grass_bin_base", L.base);
                set_bins(cmd, "da_grass_bin_cap", L.cap);
                for (u32 i = 0; i < 4; ++i)
                {
                    cmd.set_ca("da_grass_wave", i, L.wave[i]);
                    cmd.set_ca("da_grass_wave_prev", i, L.wave_prev[i]);
                }
            });
    dispatch(cmd, g.s_args, 1, 1, {}, {g.counts_b.uav, g.args_b.uav}, [&] {
        set_bins(cmd, "da_grass_bin_cap", L.cap);
        u32 per_instance[kBins]{}, first_index[kBins]{};
        for (u32 b = 0; b < kBins; ++b)
            if (kBinClass[b] != vcNone && L.cap[b])
            {
                per_instance[b] = g_vclass[kBinClass[b]].indices;
                first_index[b] = g_vclass[kBinClass[b]].first;
            }
        set_bins(cmd, "da_grass_args_n", per_instance);
        set_bins(cmd, "da_grass_args_i", first_index);
    });
    read_back(ctx);
}

void da_grass_render_gbuffer(CBackend& cmd)
{
    if (!da_grass_active() || g.latch.frame != Device.dwFrame)
        return;
    cmd.set_xform_world(Fidentity);
    // Near to far for early-Z: the near leaves, their short leaves, then the middle and far bins.
    draw_bins(cmd, g.s_blade, kBinLod0, kBinLod1 - 1, 0.f);
    // The short bin's leaves have their lobes cut (the only blades drawn with a discard).
    draw_bins(cmd, g.s_blade_cut, kBinShort, kBinShort, 0.f);
    draw_bins(cmd, g.s_blade, kBinLod1, kViewBins - 1, 0.f);
    draw_bins(cmd, g.s_head, kBinHead0, kBinHead1 + 1, 0.f);
    cmd.set_CullMode(CULL_CCW);
}

bool da_grass_casts(u32 cascade)
{
    if (!da_grass_active() || g.latch.frame != Device.dwFrame)
        return false;
    return cascade == 0 ? g.latch.shadow_near : (cascade == 1 && g.latch.shadow_mid);
}

void da_grass_render_cascade(CBackend& cmd, u32 cascade)
{
    if (!da_grass_casts(cascade))
        return;
    cmd.set_xform_world(Fidentity);
    if (cascade == 0)
    {
        draw_bins(cmd, g.s_shadow, kBinShNear, kBinShNear, g.latch.near_texel);
        draw_bins(cmd, g.s_head_shadow, kBinHeadSh, kBinHeadSh, g.latch.near_texel);
    }
    else
        draw_bins(cmd, g.s_shadow, kBinShMid, kBinShMid, g.latch.mid_texel);
    cmd.set_CullMode(CULL_CCW);
}

bool da_grass_ground_tint(Fvector4& cover_xf, Fvector4& tint, Fvector4& tint2, Fvector4 (&canopy)[16], Fvector4 (&canopy_ref)[16])
{
    cover_xf = g.tint_xf;
    tint = g.tint_on ? g.tint : Fvector4{0.f, 0.f, 0.f, 0.f};
    tint2 = g.tint2;
    for (u32 c = 0; c < 16; ++c)
    {
        canopy[c] = g.canopy[c];
        canopy_ref[c] = g.canopy_ref[c];
    }
    return g.tint_on;
}

void da_grass_pin_species(pcstr name)
{
    const pcstr arg = name ? skip_space(name) : "";
    g.pin = 0;
    if (*arg && xr_stricmp(arg, "off") && xr_stricmp(arg, "0"))
        for (u32 p = 0; p < da_grass_species; ++p)
            if (g_data.profiles[p].used && g_data.species_names[p].size() && !xr_stricmp(g_data.species_names[p].c_str(), arg))
                g.pin = p + 1;
    if (*arg && xr_stricmp(arg, "off") && !g.pin)
        Msg("! [grass] species pin: no species '%s'", arg);
    else
        Msg("* [grass] species pin: %s", g.pin ? g_data.species_names[g.pin - 1].c_str() : "off (the communities decide)");
}

void da_grass_stats()
{
    if (!g.active)
    {
        Msg("* [grass] blades off%s", g.failed ? " (failed on this level)" : "");
        return;
    }
    const Latch& L = g.latch;
    Msg("* [grass] preset %u, R %.0f m, R0 %.1f m, d1/d2 %.0f/%.0f m, lattice %.3f m (%.0f /m2), %u groups, %u lanes",
        g.preset, L.R, L.R0, L.d1, L.d2, L.s0, L.dlat, g.group_count, g.lane_count);
    static constexpr pcstr names[kBins] = {"lod0 a", "lod0 b", "lod1 a", "lod1 b", "lod1 c", "lod2 a", "lod2 b", "lod2 c",
        "lod2 d", "sun near", "sun mid", "head 0", "head 1 a", "head 1 b", "head sun", "short"};
    for (u32 b = 0; b < kBins; ++b)
        if (kBinClass[b] != vcNone)
            Msg("* [grass]   %-9s %7u / %7u records", names[b], g.counts_seen[b], g.cap[b]);
    Msg("* [grass] overflowing frames %u, budget floor x%.2f, ground window %u texels of %.2f m, %u MB of records",
        g.overflow_frames, g.floor_k, g.ground.size, g.gtexel, (g.records * kRec + g.heads * kHeadRec) >> 20);
}
} // namespace xray::render::RENDER_NAMESPACE
