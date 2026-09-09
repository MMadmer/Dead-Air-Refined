#include "StdAfx.h"
#include "Common/LevelGameDef.h"
#include "ai_space.h"
#include "ParticlesObject.h"
#include "xrScriptEngine/script_process.hpp"
#include "xrScriptEngine/script_engine.hpp"
#include "Level.h"
#include "game_cl_base.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "xrPhysics/PhysicsCommon.h"
#include "level_sounds.h"
#include "GamePersistent.h"
#include "xrEngine/Rain.h"
#include "xrEngine/Environment.h"

namespace
{
// Worst case triangles inspected per pass. Every shipped level fits under it and is swept whole;
// the stride only ever engages on something far denser, and a water body tessellated into
// hundreds of triangles survives it anyway.
constexpr u32 water_sweep_budget = 1u << 18;
// Cells per axis over the water extent - one down-ray per cell that holds water, so a thousand
// picks at the absolute worst and in practice a couple of dozen. The resolution also decides how
// finely two ponds can be told apart, which is why it is not the dozen it started at: a Zone
// level is about a kilometre across, so this is a cell every thirty metres.
constexpr int water_grid = 32;
constexpr float water_probe_range = 60.f; // metres of bed to search for; past it the sample drops
constexpr float water_probe_drop = 0.05f; // start the ray just under the surface, not on it
// Depth reported when not one ray found a bed - water over a hole in the collision, or a bed out
// of probe range. The same value CEnvironment falls back to, so the two never disagree.
constexpr float water_depth_unknown = 1.5f;

struct water_cell
{
    Fvector top; // highest liquid centroid seen in this cell
    Fbox2 span;  // XZ footprint of the liquid actually found here, not the cell's own rectangle
    int body;    // which connected sheet this cell belongs to, -1 until the flood fill runs
    bool used;
};

bool is_liquid(const CDB::TRI& tri)
{
    const SGameMtl* mtl = GMLib.GetMaterialByIdx(u16(tri.material));
    return mtl && mtl->Flags.test(SGameMtl::flLiquid);
}

// Measures the level's water body once, from the static collision - the same liquid-material
// sweep qa_water_goto walks. The extent gives the sea-state solver its fetch, the mean depth
// gives the waves their shallow-water attenuation and the optics their path length. A level
// with no liquid triangle resets the body and says so, and costs nothing further.
void measure_water_body(CObjectSpace& space)
{
    CEnvironment& env = g_pGamePersistent->Environment();
    env.reset_water_body();

    CDB::MODEL* model = space.GetStaticModel();
    const CDB::TRI* tris = space.GetStaticTris();
    const Fvector* verts = space.GetStaticVerts();
    if (!model || !tris || !verts)
        return;
    const u32 count = model->get_tris_count();
    if (!count)
        return;

    const u32 stride = _max(1u, count / water_sweep_budget);

    Fbox extent;
    extent.invalidate();
    for (u32 i = 0; i < count; i += stride)
    {
        const CDB::TRI& tri = tris[i];
        if (!is_liquid(tri))
            continue;
        for (int k = 0; k < 3; ++k)
            extent.modify(verts[tri.verts[k]]);
    }
    if (!extent.is_valid())
        return;

    // Second pass bins the liquid triangles into a coarse grid and keeps the topmost centroid of
    // each cell. Probing from a real centroid rather than from the cell's centre keeps every ray
    // starting under actual water - the extent of an L-shaped pond covers a lot of dry bank, and
    // banks would drag the mean depth to nothing. Two ponds at different heights each keep their
    // own surface this way as well.
    water_cell cells[water_grid * water_grid]{};
    const float span_x = _max(extent.vMax.x - extent.vMin.x, EPS_S);
    const float span_z = _max(extent.vMax.z - extent.vMin.z, EPS_S);
    for (u32 i = 0; i < count; i += stride)
    {
        const CDB::TRI& tri = tris[i];
        if (!is_liquid(tri))
            continue;

        Fvector c;
        c.add(verts[tri.verts[0]], verts[tri.verts[1]]);
        c.add(verts[tri.verts[2]]);
        c.div(3.f);

        int cx = iFloor((c.x - extent.vMin.x) / span_x * water_grid);
        int cz = iFloor((c.z - extent.vMin.z) / span_z * water_grid);
        clamp(cx, 0, water_grid - 1);
        clamp(cz, 0, water_grid - 1);

        water_cell& cell = cells[cz * water_grid + cx];
        if (!cell.used || c.y > cell.top.y)
            cell.top = c;
        if (cell.used)
            cell.span.modify(Fvector2{c.x, c.z});
        else
            cell.span.set(c.x, c.z, c.x, c.z);
        cell.used = true;
        cell.body = -1;
    }

    // Split the occupied cells into connected sheets. A level is a kilometre across and almost
    // every one carries several disjoint bodies - a stream at one edge, a flooded pit at the
    // other - so one box around all of them would hand a two-metre pool the fetch and the depth
    // of a lake, and fetch is most of what decides how big the waves are. Plain 4-connected
    // flood fill over the grid, iterative so a long winding river cannot blow the stack.
    struct sheet
    {
        Fbox extent;
        double depth_sum;
        u32 probes;
        u32 cells;
    };
    xr_vector<sheet> sheets;
    xr_vector<int> stack;
    for (int start = 0; start < water_grid * water_grid; ++start)
    {
        if (!cells[start].used || cells[start].body >= 0)
            continue;

        const int id = int(sheets.size());
        sheets.emplace_back(sheet{});
        sheet& sh = sheets.back();
        sh.extent.invalidate();
        sh.depth_sum = 0.0;
        sh.probes = 0;
        sh.cells = 0;

        stack.clear();
        stack.push_back(start);
        cells[start].body = id;
        while (!stack.empty())
        {
            const int at = stack.back();
            stack.pop_back();
            water_cell& cell = cells[at];
            ++sh.cells;

            sh.extent.modify(Fvector{cell.span.min.x, cell.top.y, cell.span.min.y});
            sh.extent.modify(Fvector{cell.span.max.x, cell.top.y, cell.span.max.y});

            // One ray per cell, straight down from a real liquid centroid rather than from the
            // cell's centre: the extent of an L-shaped pond covers a lot of dry bank, and banks
            // would drag the mean depth to nothing.
            Fvector from = cell.top;
            from.y -= water_probe_drop;
            collide::rq_result bed;
            if (space.RayPick(from, Fvector{0.f, -1.f, 0.f}, water_probe_range, collide::rqtStatic, bed, nullptr))
            {
                // Another liquid surface below is not the bed either (stacked water, sheets
                // modelled two-sided).
                if (!(bed.element >= 0 && is_liquid(tris[bed.element])))
                {
                    sh.depth_sum += bed.range;
                    ++sh.probes;
                }
            }

            const int cx = at % water_grid;
            const int cz = at / water_grid;
            const int nb[4] = {cx > 0 ? at - 1 : -1, cx < water_grid - 1 ? at + 1 : -1,
                cz > 0 ? at - water_grid : -1, cz < water_grid - 1 ? at + water_grid : -1};
            for (const int n : nb)
            {
                if (n < 0 || !cells[n].used || cells[n].body >= 0)
                    continue;
                cells[n].body = id;
                stack.push_back(n);
            }
        }
    }
    if (sheets.empty())
        return;

    // Biggest first, then keep as many as the engine has room for. A level with a dozen puddles
    // loses the smallest of them to the pond-sized default, which is what they are anyway.
    std::sort(sheets.begin(), sheets.end(), [](const sheet& a, const sheet& b) { return a.cells > b.cells; });

    CEnvironment::SWaterBody bodies[CEnvironment::water_body_max];
    int written = 0;
    for (const sheet& sh : sheets)
    {
        if (written >= CEnvironment::water_body_max)
            break;
        if (!sh.extent.is_valid())
            continue;
        bodies[written].extent = sh.extent;
        bodies[written].depth = sh.probes ? float(sh.depth_sum / sh.probes) : water_depth_unknown;
        ++written;
    }
    env.set_water_bodies(bodies, written);
}
} // namespace

bool CLevel::Load_GameSpecific_Before()
{
    ZoneScoped;

    // AI space
    g_pGamePersistent->LoadTitle("st_loading_ai_objects");
    string_path fn_game;

    if (GamePersistent().GameType() == eGameIDSingle && !ai().get_alife() && FS.exist(fn_game, "$level$", "level.ai") &&
        !net_Hosts.empty())
        ai().load(net_SessionName());

    if (!GEnv.isDedicatedServer && !ai().get_alife() && ai().get_game_graph() && FS.exist(fn_game, "$level$", "level.game"))
    {
        IReader* stream = FS.r_open(fn_game);
        ai().patrol_path_storage_raw(*stream);
        FS.r_close(stream);
    }

    return (TRUE);
}

bool CLevel::Load_GameSpecific_After()
{
    ZoneScoped;

    R_ASSERT(m_StaticParticles.empty());
    // loading static particles
    string_path fn_game;
    if (FS.exist(fn_game, "$level$", "level.ps_static"))
    {
        ZoneScopedN("Load static particles");

        IReader* F = FS.r_open(fn_game);
        CParticlesObject* pStaticParticles;
        u32 chunk = 0;
        string256 ref_name;
        Fmatrix transform;
        Fvector zero_vel = {0.f, 0.f, 0.f};
        u32 ver = 0;
        for (IReader* OBJ = F->open_chunk_iterator(chunk); OBJ; OBJ = F->open_chunk_iterator(chunk, OBJ))
        {
            if (chunk == 0)
            {
                if (OBJ->length() == sizeof(u32))
                {
                    ver = OBJ->r_u32();
#ifndef MASTER_GOLD
                    Msg("PS new version, %d", ver);
#endif // #ifndef MASTER_GOLD
                    continue;
                }
            }
            u16 gametype_usage = 0;
            if (ver > 0)
            {
                gametype_usage = OBJ->r_u16();
            }
            OBJ->r_stringZ(ref_name, sizeof(ref_name));
            OBJ->r(&transform, sizeof(Fmatrix));
            transform.c.y += 0.01f;

            if ((g_pGamePersistent->m_game_params.m_e_game_type & EGameIDs(gametype_usage)) || (ver == 0))
            {
                pStaticParticles = CParticlesObject::Create(ref_name, FALSE, false);
                pStaticParticles->UpdateParent(transform, zero_vel);
                pStaticParticles->Play(false);
                m_StaticParticles.push_back(pStaticParticles);
            }
        }
        FS.r_close(F);
    }

    if (!GEnv.isDedicatedServer)
    {
        // loading static sounds
        VERIFY(m_level_sound_manager);
        m_level_sound_manager->Load();

        // loading sound environment
        if (FS.exist(fn_game, "$level$", "level.snd_env"))
        {
            IReader* F = FS.r_open(fn_game);
            Sound->set_geometry_env(F);
            FS.r_close(F);
        }
        // loading SOM
        if (FS.exist(fn_game, "$level$", "level.som"))
        {
            IReader* F = FS.r_open(fn_game);
            Sound->set_geometry_som(F);
            FS.r_close(F);
        }

        // loading random (around player) sounds
        if (pSettings->section_exist("sounds_random"))
        {
            ZoneScopedN("Load random sounds");

            CInifile::Sect& S = pSettings->r_section("sounds_random");
            Sounds_Random.reserve(S.Data.size());
            for (const auto& I : S.Data)
            {
                Sounds_Random.emplace_back().create(I.first.c_str(), st_Effect, sg_SourceType);
            }
            Sounds_Random_dwNextTime = Device.TimerAsync() + 50000;
            Sounds_Random_Enabled = FALSE;
        }

        if (g_pGamePersistent->pEnvironment && g_pGamePersistent->pEnvironment->eff_Rain)
            g_pGamePersistent->pEnvironment->eff_Rain->InvalidateState();

        if (FS.exist(fn_game, "$level$", "level.fog_vol"))
        {
            ZoneScopedN("Load fog volume");
            IReader* F = FS.r_open(fn_game);
            u16 version = F->r_u16();
            if (version == 2)
            {
                u32 cnt = F->r_u32();

                Fmatrix volume_matrix;
                for (u32 i = 0; i < cnt; ++i)
                {
                    F->r(&volume_matrix, sizeof(volume_matrix));
                    u32 sub_cnt = F->r_u32();
                    for (u32 is = 0; is < sub_cnt; ++is)
                    {
                        F->r(&volume_matrix, sizeof(volume_matrix));
                    }
                }
            }
            FS.r_close(F);
        }
    }

    if (!GEnv.isDedicatedServer)
    {
        // loading scripts
        auto& scriptEngine = *GEnv.ScriptEngine;
        scriptEngine.remove_script_process(ScriptProcessor::Level);
        shared_str scripts;
        if (pLevel->section_exist("level_scripts") && pLevel->line_exist("level_scripts", "script"))
            scripts = pLevel->r_string("level_scripts", "script");
        else
            scripts = "";
        scriptEngine.add_script_process(ScriptProcessor::Level, scriptEngine.CreateScriptProcess("level", scripts));
    }

    BlockCheatLoad();

    g_pGamePersistent->Environment().SetGameTime(GetEnvironmentGameDayTimeSec(), game->GetEnvironmentGameTimeFactor());

    // Here and not earlier: the collision model is loaded and Load_GameSpecific_CFORM has already
    // remapped the game-material ids onto its triangles, so flLiquid means what it says.
    measure_water_body(ObjectSpace);

    return TRUE;
}

struct translation_pair
{
    u32 m_id;
    u16 m_index;

    IC translation_pair(u32 id, u16 index)
    {
        m_id = id;
        m_index = index;
    }

    IC bool operator==(const u16& id) const { return (m_id == id); }
    IC bool operator<(const translation_pair& pair) const { return (m_id < pair.m_id); }
    IC bool operator<(const u16& id) const { return (m_id < id); }
};

struct translation_struct
{
    u16 m_id;
    u16 m_index;
    bool m_suppress_shadows;
    bool m_suppress_wm;
};

void CLevel::Load_GameSpecific_CFORM_Serialize(IWriter& writer)
{
    writer.w_u32(GMLib.GetLibraryCrc32());
}

bool CLevel::Load_GameSpecific_CFORM_Deserialize(IReader& reader)
{
    const auto materials_crc32 = GMLib.GetLibraryCrc32();
    const auto cached_materials_crc32 = reader.r_u32();
    return materials_crc32 == cached_materials_crc32;
}

void CLevel::Load_GameSpecific_CFORM(CDB::TRI* tris, u32 count)
{
    ZoneScoped;

    typedef xr_vector<translation_pair> ID_INDEX_PAIRS;
    ID_INDEX_PAIRS translator;
    translator.reserve(GMLib.CountMaterial());
    u16 default_id = (u16)GMLib.GetMaterialIdx("default");
    translator.emplace_back(u32(-1), default_id);

    u16 index = 0, static_mtl_count = 1;
    int max_ID = 0;
    int max_static_ID = 0;
    for (auto I = GMLib.FirstMaterial(); GMLib.LastMaterial() != I; ++I, ++index)
    {
        if (!(*I)->Flags.test(SGameMtl::flDynamic))
        {
            ++static_mtl_count;
            translator.emplace_back((*I)->GetID(), index);
            if ((*I)->GetID() > max_static_ID)
                max_static_ID = (*I)->GetID();
        }
        if ((*I)->GetID() > max_ID)
            max_ID = (*I)->GetID();
    }
    // Msg("* Material remapping ID: [Max:%d, StaticMax:%d]",max_ID,max_static_ID);
    VERIFY(max_static_ID < 0xFFFF);

    if (static_mtl_count < 128)
    {
        CDB::TRI* I = tris;
        CDB::TRI* E = tris + count;
        for (; I != E; ++I)
        {
            const auto i = std::find(translator.cbegin(), translator.cend(), (u16)(*I).material);
            if (i != translator.end())
            {
                (*I).material = (*i).m_index;
                const SGameMtl* mtl = GMLib.GetMaterialByIdx((*i).m_index);
                (*I).suppress_shadows = mtl && mtl->Flags.is(SGameMtl::flSuppressShadows);
                (*I).suppress_wm = mtl && mtl->Flags.is(SGameMtl::flSuppressWallmarks);
                continue;
            }

            xrDebug::Fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
        }
        return;
    }

    std::sort(translator.begin(), translator.end());
    {
        CDB::TRI* I = tris;
        CDB::TRI* E = tris + count;
        for (; I != E; ++I)
        {
            const auto i = std::lower_bound(translator.cbegin(), translator.cend(), (u16)(*I).material);
            if ((i != translator.cend()) && ((*i).m_id == (*I).material))
            {
                (*I).material = (*i).m_index;
                const SGameMtl* mtl = GMLib.GetMaterialByIdx((*i).m_index);
                (*I).suppress_shadows = mtl->Flags.is(SGameMtl::flSuppressShadows);
                (*I).suppress_wm = mtl->Flags.is(SGameMtl::flSuppressWallmarks);
                continue;
            }

            xrDebug::Fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
        }
    }
}

void CLevel::Load_GameSpecific_CFORM_SetMaterials(CDB::TRI* tris, u32 count, xr_map<u16, shared_str>& gameMtls)
{
    // SkyLoader: reassing material indexes because they could have changed after various gamemtl.xr edits
    xr_vector<translation_struct> translator;
    translator.reserve(gameMtls.size());
    for (const auto& [id, mtlName] : gameMtls)
    {
        SGameMtl* mtl = GMLib.GetMaterial(mtlName.c_str());
        R_ASSERT2(mtl, make_string("Game material '%s' not found", mtlName.c_str()).c_str());

        translation_struct mat;
        mat.m_id = id;
        mat.m_index = GMLib.GetMaterialIdx(mtlName.c_str());
        mat.m_suppress_shadows = mtl->Flags.is(SGameMtl::flSuppressShadows);
        mat.m_suppress_wm = mtl->Flags.is(SGameMtl::flSuppressWallmarks);
        translator.emplace_back(std::move(mat));
    }

    CDB::TRI* I = tris;
    CDB::TRI* E = tris + count;
    for (; I != E; ++I)
    {
        auto it = std::find_if(translator.begin(), translator.end(), [=](const translation_struct& mat) { return mat.m_id == (u16)(*I).material; });
        if (it != translator.end())
        {
            (*I).material = (*it).m_index;
            (*I).suppress_shadows = (*it).m_suppress_shadows;
            (*I).suppress_wm = (*it).m_suppress_wm;
        }
        else
            xrDebug::Fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
    }
}


void CLevel::BlockCheatLoad()
{
#ifndef DEBUG
    if (game && (GameID() != eGameIDSingle))
        phTimefactor = 1.f;
#endif
}
