// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Reed beds (docs/dead-air/GRASS.md, reed beds). See da_reeds.h.

#include "stdafx.h"
#include "da_reeds.h"
#include "da_veg.h"
#include "da_veg_internal.h"
#include "da_grass_blades.h"
#include "da_grass_data.h"
#include "DetailManager.h"
#include "FLOD.h"
#include "FHierrarhyVisual.h"
#include "r__dsgraph_structure.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrCDB/xr_area.h"
#include "xrCDB/xr_collide_defs.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Layers/xrRenderPC_R4/da_gpu_trees.h"
#include "Layers/xrRenderPC_R4/da_rt.h"

namespace xray::render::RENDER_NAMESPACE
{
extern int ps_r__reeds;
extern float r_ssaLOD_A, r_ssaLOD_B, r_ssaVEG_DISCARD;

namespace
{
using namespace da_veg;

// A replaced stock model: its FLOD and the hierarchy it hangs from.
struct Reed
{
    FHierrarhyVisual* parent{};
    dxRender_Visual* flod{};
};

struct State
{
    bool loaded{}, detached{};
    float emerge{1.f};
    xr_vector<Reed> reeds;
    xr_vector<DaReedBed> beds;
};
State s;

// dead_air_x64_reeds.ltx: [reed_cards] a stock card (its level shader's base texture) -> the grass community grown
// where a model of it stands; [reed_beds] footprint (a bed's radius against the model's mean half width), emerge (how
// far the culms rise per metre of water over their roots).
struct Data
{
    bool file{};
    xr_vector<std::pair<shared_str, s32>> cards;
    float footprint{0.85f}, emerge{1.f};
};

Data read_data()
{
    Data d;
    string_path path;
    if (!FS.exist(path, "$game_config$", "dead_air_x64_reeds.ltx"))
        return d;
    d.file = true;
    const CInifile ini(path, TRUE);
    if (ini.section_exist("reed_cards"))
        for (const CInifile::Item& it : ini.r_section("reed_cards").Data)
        {
            if (!it.first.size() || !it.second.size())
                continue;
            s32 comm = -1;
            for (u32 c = 0; c < da_grass_communities && comm < 0; ++c)
                if (!xr_stricmp(da_grass_community_name(c), it.second.c_str()))
                    comm = s32(c);
            if (comm < 0)
            {
                Msg("! [reeds] %s: no grass community '%s'", it.first.c_str(), it.second.c_str());
                continue;
            }
            d.cards.emplace_back(it.first, comm);
        }
    if (ini.line_exist("reed_beds", "footprint"))
        d.footprint = ini.r_float("reed_beds", "footprint");
    if (ini.line_exist("reed_beds", "emerge"))
        d.emerge = ini.r_float("reed_beds", "emerge");
    return d;
}

// The base texture of a level shader string ("shader/texture,texture2").
void base_texture(pcstr shader, string_path& out)
{
    out[0] = 0;
    pcstr slash = shader ? strchr(shader, '/') : nullptr;
    if (!slash)
        return;
    xr_strcpy(out, slash + 1);
    if (pstr comma = strchr(out, ','))
        *comma = 0;
}

// The water over the bed at (x, z): the highest liquid triangle of the static collision between y_top and the bed (0:
// none). The renderer loads before the game builds its water field, so the mesh is asked directly.
float water_over(float x, float z, float y_top, float y_bed)
{
    CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    const CDB::TRI* tris = g_pGameLevel->ObjectSpace.GetStaticTris();
    if (!model || !tris || y_top <= y_bed)
        return 0.f;
    CDB::COLLIDER xrc;
    xrc.ray_query(0, model, Fvector{x, y_top, z}, Fvector{0.f, -1.f, 0.f}, y_top - y_bed);
    float surface = -flt_max;
    for (size_t i = 0; i < xrc.r_count(); ++i)
    {
        const CDB::RESULT& r = xrc.r_begin()[i];
        const SGameMtl* m = GMLib.GetMaterialByIdx(u16(tris[r.id].material));
        if (m && m->Flags.test(SGameMtl::flLiquid))
            surface = std::max(surface, y_top - r.range);
    }
    return surface > y_bed ? surface - y_bed : 0.f;
}

s32 card_community(const Data& d, pcstr texture)
{
    for (const auto& [card, comm] : d.cards)
        if (!xr_stricmp(card.c_str(), texture))
            return comm;
    return -1;
}

// The replaced models leave (or rejoin) their hierarchies, as the shrubs' do (da_shrubs.cpp set_detached): every view,
// shadow, the GPU-driven registry and the ray tracing scene walk the hierarchies. The blades grow the beds while they
// are away (reload: set the blades up again now; not while the level goes).
void set_detached(bool detach, bool reload)
{
    if (s.detached == detach)
        return;
    for (const Reed& r : s.reeds)
    {
        auto& children = r.parent->children;
        const auto it = std::find(children.begin(), children.end(), r.flod);
        if (detach && it != children.end())
            children.erase(it);
        else if (!detach && it == children.end())
            children.push_back(r.flod);
    }
    s.detached = detach;
    da_grass_set_reed_beds(detach ? xr_vector<DaReedBed>(s.beds) : xr_vector<DaReedBed>(), reload, s.emerge);
}
} // namespace

void da_reeds_level_load()
{
    da_reeds_level_unload();
    if (!g_pGameLevel || !g_pGamePersistent)
        return;
    const Data D = read_data();
    if (!D.file)
    {
        Msg("! [reeds] dead_air_x64_reeds.ltx not found: the stock reed models stand");
        return;
    }
    if (!da_grass_blades_available())
    {
        Msg("* [reeds] the blade grass is off: the stock reed models stand");
        return;
    }
    Census& C = census();
    string_path tex;
    u32 in_water = 0;
    for (const Candidate& c : C.candidates)
    {
        // A model the shrubs or the trees took is theirs; a model of reed cards only is a reed bed.
        if (c.taken != ~0u || !c.flod || !c.parent || c.flod->Type != MT_LOD)
            continue;
        s32 comm = -1;
        bool other = false;
        for (const u32 pi : c.parts)
        {
            base_texture(C.parts[pi].shader, tex);
            const s32 m = card_community(D, tex);
            other = other || m < 0;
            comm = m >= 0 ? m : comm;
        }
        if (comm < 0 || other)
            continue;
        const Fbox& box = c.flod->vis.box;
        Fvector centre;
        box.getcenter(centre);
        DaReedBed b;
        b.x = centre.x;
        b.z = centre.z;
        b.r = std::max(0.25f * ((box.vMax.x - box.vMin.x) + (box.vMax.z - box.vMin.z)) * D.footprint, 0.4f);
        // The bed: the static ground under the model, past the water (passable).
        b.y_bed = ground_at(centre.x, centre.z, box.vMax.y + 0.5f, box.vMax.y - box.vMin.y + 6.f, box.vMin.y);
        if (const float depth = water_over(centre.x, centre.z, box.vMax.y + 0.5f, b.y_bed); depth > 0.f)
        {
            b.depth = depth * D.emerge;
            ++in_water;
        }
        b.community = comm;
        s.beds.push_back(b);
        s.reeds.push_back(Reed{c.parent, c.flod});
    }
    if (s.reeds.empty())
        return;
    s.loaded = true;
    s.emerge = D.emerge;
    if (ps_r__reeds)
        set_detached(true, true);
    Msg("* [reeds] %u stock reed models stand as reed beds of the blades (%u in water)", u32(s.reeds.size()), in_water);
}

void da_reeds_level_unload()
{
    if (s.loaded)
        set_detached(false, false);
    s = State{};
}

void da_reeds_apply_switch()
{
    if (!s.loaded || s.detached == (ps_r__reeds != 0))
        return;
    // The GPU-driven registry and the ray tracing scene hold the hierarchies' models: they let go of them first and
    // gather them again after the reeds left (or rejoined) their parents.
    da_gpu_trees_level_unload();
    da_rt_level_unload();
    set_detached(ps_r__reeds != 0, true);
    da_gpu_trees_level_load();
    da_rt_level_load();
    Msg("* [reeds] %s", s.detached ? "the reed beds of the blades stand in for the stock reeds" : "the stock reeds are back (QA)");
}

void da_reeds_frame(R_dsgraph_structure& dsgraph)
{
    if (!s.detached || s.reeds.empty())
        return;
    // Inside the blades' radius the blades are the reeds; over its last 15 %, where the blades fade out, the billboard
    // fades in. Without the blades (details off) the billboards stand everywhere.
    const bool blades = da_grass_active();
    const float R = std::max(dm_fade, 8.f);
    const float cut_in = blades ? 0.85f * R : 0.f, cut_out = blades ? R : 0.f;
    const Fvector& cam = Device.vCameraPosition;
    const float span = r_ssaVEG_DISCARD * 3.f;
    for (const Reed& r : s.reeds)
    {
        FLOD* L = static_cast<FLOD*>(r.flod);
        const Fvector& c = L->vis.sphere.P;
        const float d2 = cam.distance_to_sqr(c) + EPS;
        if (d2 < cut_in * cut_in || !RImplementation.ViewBase.testSphere_dirty(c, L->vis.sphere.R))
            continue;
        const float ssa = L->vis.sphere.R / d2 * L->lod_factor;
        if (ssa < r_ssaVEG_DISCARD || !RImplementation.HOM.visible(L->vis))
            continue;
        float alpha = cut_out > cut_in ? clampr((_sqrt(d2) - cut_in) / (cut_out - cut_in), 0.f, 1.f) : 1.f;
        // The stock fade into the discard threshold (r__dsgraph_render_lods.cpp), which the encoded measure below
        // would skip.
        if (span > EPS_S)
            alpha *= clampr((ssa - r_ssaVEG_DISCARD) / span, 0.f, 1.f);
        if (alpha <= 0.004f)
            continue;
        // The billboard's alpha is 1 - (ssa - B) / (A - B) in the LOD batch: the measure that gives this alpha, the
        // billboard standing whatever its size (no model draws in its place).
        const float enc = r_ssaLOD_B + (1.f - alpha) * (r_ssaLOD_A - r_ssaLOD_B);
        dsgraph.mapLOD.insert_anyway(d2, R_dsgraph::_LodItem({enc, r.flod}));
    }
}

void da_reeds_replaced_visuals(xr_vector<dxRender_Visual*>& out)
{
    if (!s.detached)
        return;
    for (const Reed& r : s.reeds)
        out.push_back(r.flod);
}
} // namespace xray::render::RENDER_NAMESPACE
