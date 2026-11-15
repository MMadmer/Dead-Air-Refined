#include "stdafx.h"

#include "Layers/xrRender/ResourceManager.h"
#include "Layers/xrRender/FBasicVisual.h"
#include "xrCore/FMesh.hpp"
#include "Common/LevelStructure.hpp"
#include "xrEngine/IGame_Persistent.h"
#include "xrCore/stream_reader.h"
#include "xrCore/XMS/xms_core.h"
#include "xrCore/XMS/xms_level_edits.h"
#include "xrCore/_fbox2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

#include "Layers/xrRender/FHierrarhyVisual.h"
#include "Layers/xrRender/FProgressive.h"
#include "Layers/xrRender/FLOD.h"
#include "Layers/xrRender/FTreeVisual.h"
#include "xrEngine/Environment.h"
#include "xrEngine/WindVegSound.h"
#if RENDER == R_R4
#include "Layers/xrRenderPC_R4/da_smap_cache.h"
#include "Layers/xrRenderPC_R4/da_light_volumes.h"
#endif
#include "Layers/xrRender/r__sector.h"

#if defined(USE_DX11)
#include "Layers/xrRenderDX11/3DFluid/dx113DFluidVolume.h"
#include "Layers/xrRenderDX11/3DFluid/dx113DFluidManager.h"
#endif
#include "xrEngine/da_season.h"
#if RENDER == R_R4
#include "Layers/xrRender/da_ground.h"
#include "Layers/xrRender/da_snow.h"
#include "Layers/xrRender/da_grass_blades.h"
#include "Layers/xrRender/da_veg.h"
#include "Layers/xrRender/da_clusters.h"
#include "Layers/xrRender/da_reeds.h"
#include "Layers/xrRender/da_trees_bake.h"
#include "Layers/xrRenderPC_R4/da_gpu_trees.h"
#include "Layers/xrRenderPC_R4/da_offload.h"
#include "Layers/xrRenderPC_R4/da_rt.h"
#include "Layers/xrRenderPC_R4/da_dxc.h"
#include "Layers/xrRenderPC_R4/da_dstorage.h" // [dx12:ds] hook
#endif

namespace xray::render::RENDER_NAMESPACE
{
// r2.cpp: the rain_params binder's ground wetness starts over with the level.
extern void da_rain_wetness_on_level_load();

void CRender::level_Load(IReader* fs)
{
    ZoneScoped;

    R_ASSERT(g_pGameLevel);
    R_ASSERT(!b_loaded);
    da_rain_wetness_on_level_load();
#if RENDER == R_R4
    // The previous level's lamp slots (da_smap_cache.h).
    da_smap_cache_reset();
    da_lv_reset();
#endif
    // Nothing the previous level left in TAA or the upscaler belongs to this one.
    da_history_request_reset("level load");
    // A level load is a safe point: the season latches its texture folders (the calendar may have moved
    // on) and snaps its day; the change applies before the level's textures.
    da_season_relatch();
    Resources->SyncTextureSeason();
    Resources->BeginLevelTextureTracking();

    // Begin
    g_pGamePersistent->LoadBegin();
    Resources->DeferredLoad(TRUE);
    IReader* chunk;

    // Shaders
    g_pGamePersistent->LoadTitle("st_loading_shaders");
    {
        ZoneScopedN("Load shaders");
        chunk = fs->open_chunk(fsL_SHADERS);
        R_ASSERT2(chunk, "Level doesn't builded correctly.");
        u32 count = chunk->r_u32();
        Shaders.resize(count);
        // The names stay for the procedural shrubs' content keys (da_shrubs.cpp) and the -wvdbg listing.
        ShaderNames_dbg.clear();
        ShaderNames_dbg.resize(count);
        for (u32 i = 0; i < count; i++) // skip first shader as "reserved" one
        {
            string512 n_sh, n_tlist;
            LPCSTR n = LPCSTR(chunk->pointer());
            chunk->skip_stringZ();
            if (0 == n[0])
                continue;
            ShaderNames_dbg[i] = n;
            xr_strcpy(n_sh, n);
            pstr delim = strchr(n_sh, '/');
            *delim = 0;
            xr_strcpy(n_tlist, delim + 1);
            Shaders[i] = Resources->Create(n_sh, n_tlist);
        }
        chunk->close();
    }
#if RENDER == R_R4
    // The level's shaders exist: the pipelines of earlier sessions with them build while the rest loads.
    HW.PrewarmPipelines();
#endif

    // Texture I/O and GPU creation can run while the main thread parses the rest of the level.
    Resources->BeginDeferredUpload();

    // Components
    Wallmarks = xr_new<CWallmarksEngine>();
    Details = xr_new<CDetailManager>();

    if (!GEnv.isDedicatedServer)
    {
        // VB,IB,SWI
        g_pGamePersistent->LoadTitle("st_loading_geometry");
        {
            CStreamReader* geom = FS.rs_open("$level$", "level.geom");
            R_ASSERT2(geom, "level.geom");
            LoadBuffers(geom, false);
            LoadSWIs(geom);
            FS.r_close(geom);
        }

        //...and alternate/fast geometry
        if (CStreamReader* geom = FS.rs_open("$level$", "level.geomX"))
        {
            LoadBuffers(geom, true);
            FS.r_close(geom);
            m_fast_geom_loaded = true;
        }

        // Visuals
        g_pGamePersistent->LoadTitle("st_loading_spatial_db");
        chunk = fs->open_chunk(fsL_VISUALS);
        LoadVisuals(chunk);
        chunk->close();
        g_pGamePersistent->LoadPump();

        // Details
        g_pGamePersistent->LoadTitle("st_loading_details");
        Details->Load();
#if RENDER == R_R4
        // The collision mesh on the GPU, then the snow fields on it, then the blades over both.
        // (Between these long steps the loading screen gets a frame: LoadPump.)
        g_pGamePersistent->LoadPump();
        da_ground_level_load();
        g_pGamePersistent->LoadPump();
        da_snow_level_load();
        g_pGamePersistent->LoadPump();
        da_grass_level_load();
#endif
    }

    // Sectors
    g_pGamePersistent->LoadTitle("st_loading_sectors_portals");
    LoadSectors(fs);
    g_pGamePersistent->LoadPump();

#if defined(USE_DX11)
    // 3D Fluid
    Load3DFluid();
#endif

    // XMS: the exact cuts take the level's own visuals before anything of a module hangs beside them
    ApplyLevelCuts();

    // XMS: module-added geometry goes in right after the sectors exist
    LoadOverlayVisuals();

#if RENDER == R_R4
    // The offloads' auto choice measures the new level from scratch; its path decides whether the GPU
    // registry (built below in auto either way) is applied from the first frame.
    da_offload_level_load();
    g_pGamePersistent->LoadPump();
    // The stock bushes and trees the procedural vegetation stands in for, before the GPU registry leaves them out.
    da_veg_level_load();
    g_pGamePersistent->LoadPump();
    // The stock reeds the blades' reed beds stand in for (the same census), before the GPU registry leaves them out.
    da_reeds_level_load();
    g_pGamePersistent->LoadPump();
    // The stock path's vegetation whose billboard cells the bake draws again (the census of da_veg_level_load).
    da_trees_bake_level_load();
    g_pGamePersistent->LoadPump();
#endif

    // XMS: the level's pieces, after the vegetation's census (a piece may leave again, the census keeps what it
    // counted) and before everything below that walks the finished tree.
    if (!GEnv.isDedicatedServer)
        LoadPieces();

#if RENDER == R_R4
    // Every tree of the level is known now, module overlays included: the GPU registry.
    da_gpu_trees_level_load();
    // The level's static visuals by clusters: their hierarchy from the disk cache or built on worker threads while the
    // game starts; the stock draws stand in until it is on the GPU.
    da_clusters_level_load();
#endif

    // HOM
    g_pGamePersistent->LoadPump();
    HOM.Load();

    // Lights
    g_pGamePersistent->LoadTitle("st_loading_lights");
    LoadLights(fs);

    // End
    g_pGamePersistent->LoadEnd();

    // signal loaded
    b_loaded = TRUE;

    // Publish tree world positions for the vegetation-audio layer (wind rustle picks the
    // nearest crowns). Static tree instances are ordinary visuals with a baked xform; walking
    // them once at load costs nothing and the engine side cannot see render types itself.
    {
        auto& tree_out = g_pGamePersistent->Environment().wind_veg_trees;
        tree_out.clear();
        u32 rigid = 0;
        // -wvdbg: what the tree shader is bending, per level shader entry ("blender/texture"):
        // count, height, and whether the root carries foliage. The answer to "the stump sways"
        // starts with the blender a stump was built with.
        struct tree_kind { u32 count{}; u32 foliage{}; float h_min{1e9f}, h_max{}; };
        xr_map<shared_str, tree_kind> kinds;
        const bool dump = !!strstr(Core.Params, "-wvdbg");
        for (dxRender_Visual* V : Visuals)
        {
            if (V && (V->Type == MT_TREE_ST || V->Type == MT_TREE_PM))
            {
                const FTreeVisual* T = static_cast<FTreeVisual*>(V);
                tree_out.push_back(T->root_position());
                if (T->rigid())
                    ++rigid;
                if (dump)
                {
                    shared_str tex("?");
                    for (u32 i = 0; i < Shaders.size() && i < ShaderNames_dbg.size(); ++i)
                        if (Shaders[i]._get() == V->shader._get())
                        {
                            tex = ShaderNames_dbg[i];
                            break;
                        }
                    tree_kind& k = kinds[tex];
                    ++k.count;
                    if (!T->rigid())
                        ++k.foliage;
                    const float h = V->vis.box.vMax.y - T->root_position().y;
                    k.h_min = std::min(k.h_min, h);
                    k.h_max = std::max(k.h_max, h);
                }
            }
        }
        // rigid = tree-shader visuals whose root carries no foliage (stumps, logs, snags, the
        // trunk_wave odds and ends): the wind leaves them alone, which is worth a number in the
        // log when someone says it does not.
        Msg("* [wind-veg] %u tree positions published, %u rigid", u32(tree_out.size()), rigid);
        for (const auto& [tex, k] : kinds)
            Msg("* [wind-veg] tree kind [%s]: %u visual(s), %u with foliage at the root, height %.1f..%.1f m",
                tex.c_str(), k.count, k.foliage, k.h_min, k.h_max);
    }

#if defined(USE_DX11)
    //  A level change unloads every texture the resource manager holds, and the fluid's own
    //  named $user$ textures go with them: the simulation keeps stepping and the grid is still
    //  voxelised - the log even says so - while the march reads and writes nowhere, and a
    //  campfire renders as empty air. It has nothing to fall back on either, because by then
    //  the crossover has already faded the marched flame out.
    //
    //  Only the surfaces are made again. Standing the whole subsystem up instead - which is
    //  what this did first - dropped the manager's references to those textures and rebuilt the
    //  fluid shaders against whatever the registry handed back, and crashed the load outright
    //  about half the time on levels where the deferred unload had already run.
    FluidManager.RebindResources();
    FluidManager.SetScreenSize(Device.dwRenderWidth, Device.dwRenderHeight);
#endif

    // Headlamp projector warmup - see m_torch_spot_warm in r2.h. The texture name is the
    // TorchType 2 branch of xr_actor.script, i.e. exactly what the beam really uses.
    // If the texture is absent, create() just yields a stub - no warmup, no harm either.
    if (Target)
    {
        static constexpr pcstr torch_spot_texture = "internal" DELIMITER "torch1";
#if RENDER == R_R4
        // What light::set_texture makes on these renderers (light.cpp).
        m_torch_projector_warm.create(torch_spot_texture);
        m_torch_spot_warm.create("accum_volumetric_nomsaa", torch_spot_texture);
        m_torch_volume_warm.create("accum_volumetric_unshadowed", torch_spot_texture);
#else
        string256 warm_name;
        strconcat(sizeof(warm_name), warm_name, "r2" DELIMITER "accum_spot_", torch_spot_texture);
        m_torch_spot_warm.create(Target->b_accum_spot, warm_name, torch_spot_texture);
#endif
    }

#if RENDER == R_R4
    // The ray tracing scene may walk the finished level from now on; then the shader compiler's statistics.
    da_rt_level_load();
    da_dxc_log_stats();
#endif
}

void CRender::level_Unload()
{
    // the warmup held the projector shader and its texture - released with the level
    m_torch_spot_warm.destroy();
#if RENDER == R_R4
    m_torch_projector_warm.destroy();
    m_torch_volume_warm.destroy();
#endif

    // the published tree list points into this level's visuals - gone with them
    if (g_pGamePersistent)
    {
        g_pGamePersistent->Environment().wind_veg_trees.clear();
        g_pGamePersistent->Environment().wind_veg_green = 0.f;
        if (g_pGamePersistent->Environment().eff_WindVeg)
            g_pGamePersistent->Environment().eff_WindVeg->OnLevelUnload();
    }

    ZoneScoped;

    if (!g_pGameLevel)
        return;
    if (!b_loaded)
        return;

#if RENDER == R_R4
    // First the GPU users of the level's geometry: ray tracing waits for its work and frees the
    // scene, the tree registry puts its nodes back into the sector roots.
    da_rt_level_unload();
    da_gpu_trees_level_unload();
    da_clusters_level_unload();
#endif

#if defined(USE_DX11)
    // Before anything else: the volumes hang off sector roots and must be detached while the visual
    // graph is still intact.
    Unload3DFluid();
#endif

    // The pieces hang on the same roots, attached after the overlays: they go first.
    UnloadPieces();

    // Same deal for the module overlays
    UnloadOverlayVisuals();

    // HOM
    HOM.Unload();

#if RENDER == R_R4
    // In reverse of the load, and before the details: the blades point into the detail slots.
    da_trees_bake_level_unload();
    da_reeds_level_unload();
    da_veg_level_unload();
    da_grass_level_unload();
    da_snow_level_unload();
    da_ground_level_unload();
#endif

    //*** Details
    if (Details) // a double unload is reachable through the deferred-quit race
        Details->Unload();

    //*** Sectors
    // 1.
    xr_delete(rmPortals);
    last_sector_id = IRender_Sector::INVALID_SECTOR_ID;
    Device.vCameraPositionSaved.set(0, 0, 0);

    // 2.
    cleanup_contexts();

    //*** Lights
    // Glows.Unload			();
    Lights.Unload();

    //*** Visuals
    for (dxRender_Visual* visual : Visuals)
    {
        visual->Release();
        xr_delete(visual);
    }
    Visuals.clear();
    // The buffers the cut visuals held, now that none holds them
    UnloadLevelCuts();

    //*** SWI
    for (auto& swi : SWIs)
        xr_free(swi.sw);
    SWIs.clear();

    //*** VB/IB
    for (auto& indexBuffer : nVB)
    {
        indexBuffer.Release();
    }
    nVB.clear();

    for (auto& vertexBuffer : xVB)
    {
        vertexBuffer.Release();
    }
    xVB.clear();

    for (auto& indexBuffer : nIB)
    {
        indexBuffer.Release();
    }
    nIB.clear();

    for (auto& vertexBuffer : xIB)
    {
        vertexBuffer.Release();
    }
    xIB.clear();

    nDC.clear();
    xDC.clear();

    m_fast_geom_loaded = false;

    //*** Components
    xr_delete(Details);
    xr_delete(Wallmarks);

    //*** Shaders
    Shaders.clear();
    b_loaded = FALSE;
    if (ps_r__clear_models_on_unload)
    {
        Models->DeleteQueue();
        Models->ClearPool(true);
    }

    // The level's textures always go with it, wherever the renderer can drop them.
#if defined(USE_DX11)
#if RENDER == R_R4
    for (auto& context : contexts_pool)
        context.cmd_list.OnFrameEnd();
#else
    RCache.OnFrameEnd();
#endif
    Resources->UnloadLevelTextures();
    Memory.mem_compact();
#endif
}

void CRender::LoadBuffers(CStreamReader* base_fs, bool alternative)
{
    ZoneScoped;

    R_ASSERT2(base_fs, "Could not load geometry. File not found.");
    Resources->Evict();
#if RENDER == R_R4 // [dx12:ds] hook
    const DaGeometryTimer da_geometry_timer(alternative); // [dx12:ds] hook
    if (da_dstorage_load_buffers(*this, base_fs, alternative)) return; // [dx12:ds] hook
#endif // [dx12:ds] hook
    // Vertex buffers
    {
        ZoneScopedN("Load VBs");
        xr_vector<VertexDeclarator>& decls = alternative ? xDC : nDC;
        xr_vector<VertexStagingBuffer>& vbuffers = alternative ? xVB : nVB;

        // Use DX9-style declarators
        CStreamReader* fs = base_fs->open_chunk(fsL_VB);
        R_ASSERT2(fs, "Could not load geometry. File 'level.geom?' corrupted.");

        const u32 count = fs->r_u32();
        decls.resize(count);
        vbuffers.resize(count);

        constexpr size_t buffer_size = (MAXD3DDECLLENGTH + 1) * sizeof(VertexElement);
        for (u32 i = 0; i < count; i++)
        {
            // decl
            VertexElement* dcl = (VertexElement*)xr_alloca(buffer_size);
            fs->r(dcl, buffer_size);
            fs->advance(-(int)buffer_size);

            const u32 dcl_len = GetDeclLength(dcl) + 1;
            decls[i].resize(dcl_len);
            fs->r(decls[i].begin(), dcl_len * sizeof(VertexElement));

            // count, size
            const u32 vCount = fs->r_u32();
            const u32 vSize = GetDeclVertexSize(dcl, 0);
#ifndef MASTER_GOLD
            Msg("* [Loading VB] %d verts, %d Kb", vCount, (vCount * vSize) / 1024);
#endif

            // Create and fill
            //  TODO: DX11: Check fragmentation.
            //  Check if buffer is less then 2048 kb
            const size_t dataSize = size_t(vCount) * vSize;
            const void* data = fs->pointer_contiguous(dataSize);
            vbuffers[i].CreateFromData(data, dataSize);
            fs->advance(static_cast<int>(dataSize));
        }
        fs->close();
    }

    // Index buffers
    {
        ZoneScopedN("Load IBs");
        xr_vector<IndexStagingBuffer>& ibuffers = alternative ? xIB : nIB;

        CStreamReader* fs = base_fs->open_chunk(fsL_IB);
        const u32 count = fs->r_u32();
        ibuffers.resize(count);
        for (u32 i = 0; i < count; i++)
        {
            const u32 iCount = fs->r_u32();
#ifndef MASTER_GOLD
            Msg("* [Loading IB] %d indices, %d Kb", iCount, (iCount * 2) / 1024);
#endif

            // Create and fill
            //  TODO: DX11: Check fragmentation.
            //  Check if buffer is less then 2048 kb
            const size_t dataSize = size_t(iCount) * 2;
            const void* data = fs->pointer_contiguous(dataSize);
            ibuffers[i].CreateFromData(data, dataSize);
            fs->advance(static_cast<int>(dataSize));
        }
        fs->close();
    }
}

void CRender::LoadVisuals(IReader* fs)
{
    u32 index = 0;
    IReader* chunk = nullptr;

    ZoneScoped;

    while ((chunk = fs->open_chunk(index)) != 0)
    {
        ogf_header H;
        chunk->r_chunk_safe(OGF_HEADER, &H, sizeof(H));

        dxRender_Visual* visual = Models->Instance_Create(H.type);
        visual->Load(nullptr, chunk, 0);
        Visuals.push_back(visual);

        chunk->close();
        index++;
    }
}

void CRender::LoadLights(IReader* fs)
{
    ZoneScoped;
    // lights
    Lights.Load(fs);
    Lights.LoadHemi();
}

void CRender::LoadSectors(IReader* fs)
{
    ZoneScoped;

    // allocate memory for portals
    const u32 size = fs->find_chunk(fsL_PORTALS);
    R_ASSERT(0 == size % sizeof(CPortal::level_portal_data_t));

    const u32 portals_count = size / sizeof(CPortal::level_portal_data_t);
    xr_vector<CPortal::level_portal_data_t> portals_data{portals_count};

    // load sectors
    xr_vector<CSector::level_sector_data_t> sectors_data;

    float largest_sector_vol = 0.0f;
    IReader* S = fs->open_chunk(fsL_SECTORS);
    for (u32 i = 0;; i++)
    {
        IReader* P = S->open_chunk(i);
        if (!P)
            break;

        ZoneScopedN("Load sector");
        auto& sector_data = sectors_data.emplace_back();
        {
            u32 size = P->find_chunk(fsP_Portals);
            R_ASSERT(0 == (size & 1));
            u32 portals_in_sector = size / sizeof(u16);

            sector_data.portals_id.reserve(portals_in_sector);
            while (portals_in_sector)
            {
                const u16 ID = P->r_u16();
                sector_data.portals_id.emplace_back(ID);
                --portals_in_sector;
            }

            size = P->find_chunk(fsP_Root);
            R_ASSERT(size == 4);
            sector_data.root_id = P->r_u32();

            // Search for default sector - assume "default" or "outdoor" sector is the largest one
            // XXX: hack: need to know real outdoor sector
            auto* V = static_cast<dxRender_Visual*>(RImplementation.getVisual(sector_data.root_id));
            float vol = V->vis.box.getvolume();
            if (vol > largest_sector_vol)
            {
                largest_sector_vol = vol;
                largest_sector_id = static_cast<IRender_Sector::sector_id_t>(i);
            }
        }
        P->close();
    }
    S->close();

    // load portals
    if (portals_count)
    {
        static const bool use_cache = !strstr(Core.Params, "-no_cdb_cache");
        static const bool skip_crc32_check = strstr(Core.Params, "-skip_cdb_cache_crc32_check");

        ZoneScopedN("Load portals");

        // build portal model
        bool do_rebuild = true;
        const auto chunk_size = fs->find_chunk(fsL_PORTALS);

        rmPortals = xr_new<CDB::MODEL>();
        if (use_cache)
            rmPortals->set_model_crc32(crc32(fs->pointer(), chunk_size));

        string_path file_name;
        strconcat(file_name, "cdb_cache" DELIMITER, FS.get_path("$level$")->m_Add, "portals.bin");
        FS.update_path(file_name, "$app_data_root$", file_name);

        if (use_cache && FS.exist(file_name) && rmPortals->deserialize(file_name, skip_crc32_check))
        {
#ifndef MASTER_GOLD
            Msg("* Loaded portals cache (%s)...", file_name);
#endif
            do_rebuild = false;
        }
        else
        {
#ifndef MASTER_GOLD
            Msg("* Portals cache for '%s' was not loaded. "
                "Building the model from scratch..", file_name);
#endif
        }

        CDB::Collector CL;
        for (u32 i = 0; i < portals_count; i++)
        {
            ZoneScopedN("Build portal from chunk");
            auto &P = portals_data[i];
            fs->r(&P, sizeof(P));

            if (do_rebuild)
            {
                for (u32 j = 2; j < P.vertices.size(); j++)
                    CL.add_face_packed_D(P.vertices[0], P.vertices[j - 1], P.vertices[j], u32(i));
            }
        }

        if (do_rebuild)
        {
            if (CL.getTS() < 2)
            {
                Fvector v1, v2, v3;
                v1.set(-20000.f, -20000.f, -20000.f);
                v2.set(-20001.f, -20001.f, -20001.f);
                v3.set(-20002.f, -20002.f, -20002.f);
                CL.add_face_packed_D(v1, v2, v3, 0);
            }
            rmPortals->build(CL.getV(), CL.getVS(), CL.getT(), CL.getTS());
            if (use_cache)
                rmPortals->serialize(file_name);
        }
    }
    else
    {
        rmPortals = nullptr;
    }

    for (u32 id = 0; id < R__NUM_PARALLEL_CONTEXTS; ++id)
    {
        auto& dsgraph = contexts_pool[id];
        dsgraph.reset();
        dsgraph.load(sectors_data, portals_data);
        contexts_used.set(id, false);
    }

    auto& dsgraph = get_imm_context();
    dsgraph.reset();
    dsgraph.load(sectors_data, portals_data);

    last_sector_id = IRender_Sector::INVALID_SECTOR_ID;
}

void CRender::LoadSWIs(CStreamReader* base_fs)
{
    ZoneScoped;

    // allocate memory for portals
    if (base_fs->find_chunk(fsL_SWIS))
    {
        CStreamReader* fs = base_fs->open_chunk(fsL_SWIS);
        u32 item_count = fs->r_u32();

        for (auto& SWI : SWIs)
            xr_free(SWI.sw);

        SWIs.clear();

        SWIs.resize(item_count);
        for (u32 c = 0; c < item_count; c++)
        {
            FSlideWindowItem& swi = SWIs[c];
            swi.reserved[0] = fs->r_u32();
            swi.reserved[1] = fs->r_u32();
            swi.reserved[2] = fs->r_u32();
            swi.reserved[3] = fs->r_u32();
            swi.count = fs->r_u32();
            VERIFY(nullptr == swi.sw);
            swi.sw = xr_alloc<FSlideWindow>(swi.count);
            fs->r(swi.sw, sizeof(FSlideWindow) * swi.count);
        }
        fs->close();
    }
}

// ---- XMS visual overlays ----------------------------------------------------
// Modules ship world-space .ogf files listed in
// <module>/levels/<level>/overlay_visuals.ltx (written by the editor's OGF
// export). The registry lives outside gamedata on purpose: VFS mounting is
// last-writer-wins, so a shared virtual path would let one module silence every
// other one. Reading each module's file physically keeps the contributions
// additive, which is the whole point of the overlay layer.
//
// Section layout, one entry per visual:
//   [v_0]
//   file   = visuals/my_shed.ogf   ; relative to levels/<level>/
//   mode   = hardcore              ; optional extra game-mode gate
//   sector = 12                    ; optional, skips sector auto-detection
namespace
{
bool xms_read_physical(pcstr path, xr_vector<u8>& out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(std::max(0l, len)));
    const size_t got = len > 0 ? fread(out.data(), 1, out.size(), f) : 0;
    fclose(f);
    return got == out.size() && !out.empty();
}
} // namespace

bool CRender::AttachOverlayVisual(const XMS::Module& module, pcstr level_name, pcstr rel_file, int forced_sector)
{
    string_path path;
    xr_sprintf(path, "%slevels" DELIMITER "%s" DELIMITER "%s", module.root.c_str(), level_name, rel_file);
    for (char* p = path; *p; ++p)
        if (*p == '/')
            *p = _DELIMITER;

    xr_vector<u8> raw;
    if (!xms_read_physical(path, raw))
    {
        Msg("! XMS: overlay visual not readable [%s]", path);
        return false;
    }

    IReader reader(raw.data(), raw.size());
    ogf_header H;
    if (reader.find_chunk(OGF_HEADER) != sizeof(H))
    {
        Msg("! XMS: not an ogf [%s]", path);
        return false;
    }
    reader.r(&H, sizeof(H));

    // Skeletons need a bone tick and an owning object; the editor refuses to bake
    // them, so anything animated here means a hand-edited registry.
    switch (H.type)
    {
    case MT_NORMAL:
    case MT_PROGRESSIVE:
    case MT_HIERRARHY:
    case MT_TREE_ST:
    case MT_TREE_PM: break;
    default:
        Msg("! XMS: overlay visual type %d is not static [%s]", H.type, path);
        return false;
    }

    reader.seek(0);
    dxRender_Visual* visual = Models->Instance_Create(H.type);
    if (!visual)
        return false;
    visual->Load(path, &reader, 0);

    const auto release = [&]()
    {
        visual->Release();
        xr_delete(visual);
    };

    auto& dsgraph = get_imm_context();
    auto sector_id = IRender_Sector::INVALID_SECTOR_ID;
    if (forced_sector >= 0)
    {
        if (size_t(forced_sector) >= dsgraph.Sectors.size())
        {
            Msg("! XMS: sector %d is out of range (level has %zu) [%s]", forced_sector, dsgraph.Sectors.size(), path);
            release();
            return false;
        }
        sector_id = IRender_Sector::sector_id_t(forced_sector);
    }
    else
    {
        sector_id = dsgraph.detect_sector(visual->vis.sphere.P);
        if (sector_id == IRender_Sector::INVALID_SECTOR_ID || size_t(sector_id) >= dsgraph.Sectors.size())
        {
            Msg("! XMS: no render sector at [%3.1f,%3.1f,%3.1f] - is the ogf baked in world space? [%s]",
                VPUSH(visual->vis.sphere.P), path);
            release();
            return false;
        }
    }

    dxRender_Visual* root = static_cast<CSector*>(dsgraph.get_sector(sector_id))->root();
    if (!root || root->getType() != MT_HIERRARHY)
    {
        Msg("! XMS: sector %d has no hierarchy root [%s]", int(sector_id), path);
        release();
        return false;
    }

    static_cast<FHierrarhyVisual*>(root)->children.push_back(visual);

    // The root's own bounds gate the entire sector subtree in add_static, so an
    // overlay outside them would simply never draw. Widen only when it actually
    // sticks out: a bigger root box costs every base visual in this sector its
    // fcvInside fast path.
    if (!root->vis.box.contains(visual->vis.box))
    {
        root->vis.box.merge(visual->vis.box);
        root->vis.box.getsphere(root->vis.sphere.P, root->vis.sphere.R);
    }

    m_overlay_visuals.push_back({root, visual});
    return true;
}

void CRender::LoadOverlayVisuals()
{
    ZoneScoped;

    // No visual graph on a dedicated server - LoadVisuals is skipped there too
    if (!XMS::Active() || GEnv.isDedicatedServer)
        return;

    // "$level$" is the level folder - its last component is the level name the
    // editor stamps into the module layout.
    string_path level_root;
    FS.update_path(level_root, "$level$", "");
    if (const size_t len = xr_strlen(level_root); len && (level_root[len - 1] == _DELIMITER || level_root[len - 1] == '/'))
        level_root[len - 1] = 0;
    pcstr level_name = level_root;
    for (pcstr p = level_root; *p; ++p)
        if (*p == _DELIMITER || *p == '/')
            level_name = p + 1;
    if (!level_name[0])
        return;

    // Two passes on purpose: every hide runs before any attach, so a module can
    // never delete geometry another module just added - hides only ever target
    // the base level.
    struct AddEntry
    {
        const XMS::Module* module;
        xr_string file;
        int sector;
    };
    struct HideEntry
    {
        Fbox region;
        int sector;
        bool overlap;
    };
    xr_vector<AddEntry> adds;
    xr_vector<HideEntry> hides;

    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled || !XMS::ModuleApplies(m))
            continue;

        string_path ltx_path;
        xr_sprintf(ltx_path, "%slevels" DELIMITER "%s" DELIMITER "overlay_visuals.ltx", m.root.c_str(), level_name);
        xr_vector<XMS::IniRecord> records;
        if (!XMS::ReadIniRecords(ltx_path, records))
            continue;

        // Records arrive in file order, so a section change closes the entry before it.
        xr_string section, file, mode, hide;
        int forced_sector = -1;
        bool overlap = false;
        const auto flush = [&]()
        {
            if (mode.empty() || XMS::ModeActive(mode.c_str()))
            {
                if (!file.empty())
                    adds.push_back({&m, file, forced_sector});
                Fvector centre, extent;
                if (!hide.empty() &&
                    6 == sscanf(hide.c_str(), "%f,%f,%f,%f,%f,%f", &centre.x, &centre.y, &centre.z,
                                &extent.x, &extent.y, &extent.z))
                {
                    HideEntry entry;
                    entry.region.setb(centre, extent); // setb takes half-extents
                    entry.sector = forced_sector;
                    entry.overlap = overlap;
                    hides.push_back(entry);
                }
            }
            file.clear();
            mode.clear();
            hide.clear();
            forced_sector = -1;
            overlap = false;
        };

        for (const XMS::IniRecord& r : records)
        {
            if (r.section != section)
            {
                flush();
                section = r.section;
            }
            if (r.key == "file")
                file = r.value;
            else if (r.key == "mode")
                mode = r.value;
            else if (r.key == "sector")
                forced_sector = atoi(r.value.c_str());
            else if (r.key == "hide")
                hide = r.value;
            else if (r.key == "overlap")
                overlap = 0 == xr_strcmp(r.value.c_str(), "true");
        }
        flush();
    }

    u32 hidden = 0;
    for (const HideEntry& entry : hides)
        hidden += HideOverlayGeometry(entry.region, entry.overlap, entry.sector);

    u32 attached = 0;
    for (const AddEntry& entry : adds)
        attached += AttachOverlayVisual(*entry.module, level_name, entry.file.c_str(), entry.sector) ? 1 : 0;

    if (attached || hidden)
        Msg("* [XMS] visual overlays on [%s]: %d attached, %d base visual(s) hidden", level_name, attached, hidden);
}

// Detaches base visuals from their sector so a module can replace a chunk of the
// level instead of just adding to it. Strict containment by default: an
// intersection test would take out the whole terrain visual for a small box.
// `overlap = true` is the opt-in for exactly that case - hide the big piece and
// ship your own baked replacement next to it.
u32 CRender::HideOverlayGeometry(const Fbox& region, bool overlap, int forced_sector)
{
    auto& dsgraph = get_imm_context();
    if (dsgraph.Sectors.empty())
        return 0;
    if (forced_sector >= 0 && size_t(forced_sector) >= dsgraph.Sectors.size())
    {
        Msg("! XMS: hide sector %d is out of range (level has %zu)", forced_sector, dsgraph.Sectors.size());
        return 0;
    }

    const size_t first = forced_sector >= 0 ? size_t(forced_sector) : 0;
    const size_t last = forced_sector >= 0 ? first + 1 : dsgraph.Sectors.size();

    u32 hidden = 0;
    for (size_t s = first; s < last; ++s)
    {
        dxRender_Visual* root = static_cast<CSector*>(dsgraph.get_sector(s))->root();
        if (!root || root->getType() != MT_HIERRARHY)
            continue;
        auto& children = static_cast<FHierrarhyVisual*>(root)->children;
        for (size_t i = children.size(); i-- > 0;)
        {
            const Fbox& box = children[i]->vis.box;
            if (!(overlap ? region.intersect(box) : region.contains(box)))
                continue;
            // The visual itself stays in Visuals and dies with the level; only the
            // parent link goes, so there is nothing to restore on unload.
            children.erase(children.begin() + i);
            ++hidden;
        }
    }
    return hidden;
}

void CRender::UnloadOverlayVisuals()
{
    for (auto& entry : m_overlay_visuals)
    {
        // Detach first: the sector root does not own its children, and the visual
        // graph may already be half torn down by the time this runs.
        if (entry.parent && entry.parent->getType() == MT_HIERRARHY)
            std::erase(static_cast<FHierrarhyVisual*>(entry.parent)->children, entry.visual);

        if (entry.visual)
        {
            entry.visual->Release();
            xr_delete(entry.visual);
        }
    }
    m_overlay_visuals.clear();
}

// ---- level pieces -----------------------------------------------------------
// A piece is a compiled part of the level's static picture that a module keeps in
// <module>\levels\<level>\pieces\<name>\ (docs/dead-air/XMS_ARCHITECTURE.md 8.3):
//   piece.ltx    [piece] version = 1, box = minx, miny, minz, maxx, maxy, maxz,
//                sector = auto | <id>, stream = true | false, mode = <id> (optional)
//   piece        chunks 1 header, 2 shader table, 3 visuals as the level file has them, 32 roots
//   piece.geom   chunks 9 vertex buffers, 10 index buffers, 11 sliding windows, as level.geom
//   piece.geomx  the fast buffers, as level.geomX (optional)
//   <name>.dds   a texture of the shader table that lies in the folder is the piece's own
// Only the picture is a piece: the collision, the AI map and the spawn are composed with the level.
thread_local CRender::PieceTables* CRender::s_piece_loading = nullptr;

namespace
{
constexpr u32 piece_format_version = 1;
constexpr u32 piece_roots_chunk = 32;
constexpr u16 piece_sector_auto = 0xFFFF;
// A piece loads nearer than the far plane plus its radius plus the margin and leaves a quarter beyond that,
// attached for piece_keep_ms at least; the distances are looked at every piece_check_frames.
constexpr float piece_margin = 50.f;
constexpr float piece_leave_factor = 1.25f;
constexpr u32 piece_keep_ms = 5000;
constexpr u32 piece_check_frames = 8;
// The pieces a task has read are made together: when nothing more is to be read, when so many wait, or after so many
// frames. Every change of the tree makes the trees' registry, the ray tracing scene and the lamps' caches again, so a
// queue of pieces - the far plane moved, the camera jumped - is a few such changes and not one a frame.
constexpr size_t piece_batch = 8;
constexpr u32 piece_batch_frames = 30;
// Every attach renames the piece's own textures anew: no other piece, nor a later load of this one, meets them.
std::atomic<u32> piece_serial{};

// A piece's own texture for the texture loader: the whole file in memory, freed with the reader.
class PieceFileReader final : public IReader
{
public:
    PieceFileReader(void* bytes, size_t size) : IReader(bytes, size, 0) {}
    ~PieceFileReader() override { xr_free(data); }
};

template <typename... Args>
void piece_fail(xr_string& error, pcstr format, Args... args)
{
    string512 text;
    xr_sprintf(text, format, args...);
    error = text;
}

bool piece_file_exists(pcstr path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

// The last component of "$level$": the name the module layout files the level under.
void piece_level_name(string_path& out)
{
    string_path root;
    FS.update_path(root, "$level$", "");
    if (const size_t len = xr_strlen(root); len && (root[len - 1] == _DELIMITER || root[len - 1] == '/'))
        root[len - 1] = 0;
    pcstr name = root;
    for (pcstr p = root; *p; ++p)
        if (*p == _DELIMITER || *p == '/')
            name = p + 1;
    xr_strcpy(out, name);
}

bool piece_visual_type(u8 type)
{
    switch (type)
    {
    case MT_NORMAL:
    case MT_PROGRESSIVE:
    case MT_HIERRARHY:
    case MT_LOD:
    case MT_TREE_ST:
    case MT_TREE_PM: return true;
    default: return false;
    }
}
} // namespace

struct CRender::LevelPieces
{
    struct Root
    {
        u32 visual;
        u16 sector;
    };

    // What a task reads and makes off the main thread; the safe point takes it over.
    struct Load
    {
        xr_vector<u8> file; // `piece`: its visuals are read from it at the safe point
        xr_vector<xr_string> shaders; // "shader/textures", the piece's own textures renamed
        xr_vector<std::pair<xr_string, xr_string>> textures; // a renamed texture, its file
        xr_vector<Root> roots;
        PieceTables tables; // the buffers, their declarations, the sliding windows
        u32 visuals{};
        float task_ms{};
        xr_string error;
    };

    struct Piece
    {
        xr_string name; // "<module>/<piece>"
        xr_string folder; // physical, with the trailing delimiter
        Fvector center{};
        float radius{};
        int sector{-1}; // the manifest's sector for every root; -1: each root's own
        bool stream{true};
        bool failed{};
        bool attached{};
        u32 attached_at{};
        u32 buffers{};
        std::atomic<bool> reading{};
        Load* load{};
        PieceTables tables;
        xr_vector<overlay_visual> links; // a root and the sector root it hangs on
        xr_vector<xr_string> textures; // the renamed ones
    };

    xr_vector<Piece*> pieces;
    Piece* reading{}; // the one piece a task reads, at most
    xr_vector<u32> ready; // read, waiting to be made with the others
    u32 ready_frame{}; // when the first of them was read
    u32 next_check{};
    bool first_frame{true};
};

namespace
{
using PieceLoad = CRender::LevelPieces::Load;
using PieceState = CRender::LevelPieces::Piece;

void piece_free_tables(CRender::PieceTables& tables)
{
    for (FSlideWindowItem& swi : tables.swis)
        xr_free(swi.sw);
    tables.swis.clear();
    // The buffers' destructors free what the device holds; the visuals that took references went first.
    tables.nVB.clear();
    tables.xVB.clear();
    tables.nIB.clear();
    tables.xIB.clear();
    tables.nDC.clear();
    tables.xDC.clear();
}

void piece_free_load(PieceLoad*& load)
{
    if (!load)
        return;
    piece_free_tables(load->tables);
    xr_delete(load);
}

// One file of the geometry (piece.geom or piece.geomx): the buffers' layout first, the device buffers once every
// visual has been checked against it.
struct PieceGeom
{
    struct Buffer
    {
        const u8* data;
        size_t size;
        u32 count;
    };
    xr_vector<u8> bytes;
    xr_vector<CRender::VertexDeclarator> decls;
    xr_vector<Buffer> vbs, ibs;
};

bool piece_parse_geom(PieceGeom& geom, bool fast, CRender::PieceTables& tables, xr_string& error)
{
    IReader file(geom.bytes.data(), geom.bytes.size());
    IReader* vb = file.open_chunk(fsL_VB);
    IReader* ib = file.open_chunk(fsL_IB);
    const auto close = [&] {
        if (vb)
            vb->close();
        if (ib)
            ib->close();
    };
    if (!vb || !ib || vb->elapsed() < 4 || ib->elapsed() < 4)
    {
        error = fast ? "piece.geomx has no vertex or index buffers" : "piece.geom has no vertex or index buffers";
        close();
        return false;
    }
    constexpr size_t decl_bytes = (MAXD3DDECLLENGTH + 1) * sizeof(VertexElement);
    const u32 vb_count = vb->r_u32();
    for (u32 i = 0; i < vb_count; ++i)
    {
        VertexElement dcl[MAXD3DDECLLENGTH + 1]{};
        const size_t avail = std::min<size_t>(decl_bytes, size_t(vb->elapsed()));
        CopyMemory(dcl, vb->pointer(), avail);
        const u32 length = GetDeclLength(dcl);
        if (length == 0 || (length + 1) * sizeof(VertexElement) > avail)
        {
            error.assign("a vertex buffer's declaration is broken");
            close();
            return false;
        }
        vb->advance(int((length + 1) * sizeof(VertexElement)));
        if (vb->elapsed() < 4)
        {
            error.assign("a vertex buffer is cut short");
            close();
            return false;
        }
        const u32 count = vb->r_u32();
        const size_t size = size_t(count) * GetDeclVertexSize(dcl, 0);
        if (!size || size > size_t(vb->elapsed()))
        {
            error.assign("a vertex buffer is empty or cut short");
            close();
            return false;
        }
        geom.decls.emplace_back();
        geom.decls.back().assign(dcl, length + 1);
        geom.vbs.push_back({static_cast<const u8*>(vb->pointer()), size, count});
        vb->advance(int(size));
    }
    const u32 ib_count = ib->r_u32();
    for (u32 i = 0; i < ib_count; ++i)
    {
        if (ib->elapsed() < 4)
        {
            error.assign("an index buffer is cut short");
            close();
            return false;
        }
        const u32 count = ib->r_u32();
        const size_t size = size_t(count) * 2;
        if (!size || size > size_t(ib->elapsed()))
        {
            error.assign("an index buffer is empty or cut short");
            close();
            return false;
        }
        geom.ibs.push_back({static_cast<const u8*>(ib->pointer()), size, count});
        ib->advance(int(size));
    }
    close();

    // The sliding windows (progressive trees) live in piece.geom alone.
    if (!fast && file.find_chunk(fsL_SWIS))
    {
        IReader* sw = file.open_chunk(fsL_SWIS);
        bool ok = sw && sw->elapsed() >= 4;
        u32 count = ok ? sw->r_u32() : 0;
        // The count is the file's word: an item is twenty bytes at least, and the chunk holds only so many.
        if (ok && size_t(count) > size_t(sw->elapsed()) / 20)
        {
            ok = false;
            count = 0;
        }
        tables.swis.resize(count);
        for (u32 i = 0; ok && i < count; ++i)
        {
            FSlideWindowItem& swi = tables.swis[i];
            ok = sw->elapsed() >= 20;
            if (!ok)
                break;
            for (u32& r : swi.reserved)
                r = sw->r_u32();
            swi.count = sw->r_u32();
            ok = size_t(swi.count) * sizeof(FSlideWindow) <= size_t(sw->elapsed());
            if (!ok)
                break;
            swi.sw = xr_alloc<FSlideWindow>(swi.count ? swi.count : 1);
            sw->r(swi.sw, sizeof(FSlideWindow) * swi.count);
        }
        if (sw)
            sw->close();
        if (!ok)
        {
            error.assign("the sliding windows are cut short");
            return false;
        }
    }
    return true;
}

void piece_create_buffers(const PieceGeom& geom, bool fast, CRender::PieceTables& tables)
{
    auto& decls = fast ? tables.xDC : tables.nDC;
    auto& vbs = fast ? tables.xVB : tables.nVB;
    auto& ibs = fast ? tables.xIB : tables.nIB;
    decls = geom.decls;
    // Sized once: the buffers never move after the device holds them.
    vbs.resize(geom.vbs.size());
    ibs.resize(geom.ibs.size());
    for (size_t i = 0; i < geom.vbs.size(); ++i)
        vbs[i].CreateFromData(geom.vbs[i].data, geom.vbs[i].size);
    for (size_t i = 0; i < geom.ibs.size(); ++i)
        ibs[i].CreateFromData(geom.ibs[i].data, geom.ibs[i].size);
}

bool piece_check_range(const PieceGeom& geom, IReader& chunk, xr_string& error, u32 index, bool fast,
    u32* vertices = nullptr, u32* indices = nullptr)
{
    if (chunk.elapsed() < 24)
    {
        piece_fail(error, "visual %u: its geometry record is cut short", index);
        return false;
    }
    const u32 vb = chunk.r_u32(), vbase = chunk.r_u32(), vcount = chunk.r_u32();
    const u32 ib = chunk.r_u32(), ibase = chunk.r_u32(), icount = chunk.r_u32();
    if (vb >= geom.vbs.size() || u64(vbase) + vcount > geom.vbs[vb].count || ib >= geom.ibs.size() ||
        u64(ibase) + icount > geom.ibs[ib].count)
    {
        piece_fail(error, "visual %u: its %sgeometry lies outside the piece's buffers", index, fast ? "fast " : "");
        return false;
    }
    if (vertices)
        *vertices = vcount;
    if (indices)
        *indices = icount;
    return true;
}

// Sliding windows against the geometry they draw from: one at least, and each inside the visual's own indices
// and vertices - the renderer draws a window without looking at it again.
bool piece_check_windows(IReader& from, u32 count, u32 vertices, u32 indices)
{
    if (!count || size_t(count) * sizeof(FSlideWindow) > size_t(from.elapsed()))
        return false;
    for (u32 i = 0; i < count; ++i)
    {
        FSlideWindow window;
        from.r(&window, sizeof(window));
        if (u64(window.offset) + u64(window.num_tris) * 3 > indices || window.num_verts > vertices)
            return false;
    }
    return true;
}

// The windows a progressive visual carries itself (OGF_SWIDATA): sixteen reserved bytes, the count, the windows.
bool piece_check_swidata(IReader& chunk, size_t size, u32 vertices, u32 indices)
{
    if (size < 20 || size_t(chunk.elapsed()) < size)
        return false;
    chunk.advance(16);
    const u32 count = chunk.r_u32();
    return size_t(count) <= (size - 20) / sizeof(FSlideWindow) && piece_check_windows(chunk, count, vertices, indices);
}

// Every id a visual names, against the piece's own tables: a visual of a piece can never reach the level's.
bool piece_check_visual(IReader& chunk, u32 index, u32 shaders, const PieceGeom& geom, const PieceGeom* fast,
    const xr_vector<FSlideWindowItem>& swis, xr_string& error)
{
    ogf_header H;
    if (chunk.find_chunk(OGF_HEADER) != sizeof(H))
    {
        piece_fail(error, "visual %u has no header", index);
        return false;
    }
    chunk.r(&H, sizeof(H));
    if (H.format_version != xrOGF_FormatVersion || !piece_visual_type(H.type))
    {
        piece_fail(error, "visual %u is of type %u (format %u): not a static visual", index, H.type,
            H.format_version);
        return false;
    }
    if (H.shader_id >= shaders)
    {
        piece_fail(error, "visual %u names shader %u, the piece has %u", index, H.shader_id, shaders);
        return false;
    }
    // Containers a level visual never carries; their readers would reach past the piece.
    if (chunk.find_chunk(OGF_VCONTAINER) || chunk.find_chunk(OGF_ICONTAINER) || chunk.find_chunk(OGF_CHILDREN))
    {
        piece_fail(error, "visual %u carries a container a static visual does not", index);
        return false;
    }
    const bool drawn = H.type == MT_NORMAL || H.type == MT_PROGRESSIVE || H.type == MT_TREE_ST || H.type == MT_TREE_PM;
    const bool parent = H.type == MT_HIERRARHY || H.type == MT_LOD;
    u32 vertices = 0, indices = 0;
    const bool geometry = chunk.find_chunk(OGF_GCONTAINER);
    if (geometry && !piece_check_range(geom, chunk, error, index, false, &vertices, &indices))
        return false;
    // A visual that is drawn draws from the piece's buffers: one that carries its own vertices would be read by
    // counts nothing here has looked at.
    if (drawn && !geometry)
    {
        piece_fail(error, "visual %u has no geometry in the piece's buffers", index);
        return false;
    }
    // The tree's definition: its matrix and the two colour scales, as the tree reads them.
    if ((H.type == MT_TREE_ST || H.type == MT_TREE_PM) &&
        chunk.find_chunk(OGF_TREEDEF2) < sizeof(Fmatrix) + 2 * (sizeof(Fvector) + 2 * sizeof(float)))
    {
        piece_fail(error, "visual %u: a tree without its definition, or with one cut short", index);
        return false;
    }
    if (H.type == MT_PROGRESSIVE)
    {
        const size_t size = chunk.find_chunk(OGF_SWIDATA);
        if (!piece_check_swidata(chunk, size, vertices, indices))
        {
            piece_fail(error, "visual %u: a progressive visual without its windows, or with windows outside it",
                index);
            return false;
        }
    }
    // Eight faces of four vertices each, as the LOD reads them.
    if (H.type == MT_LOD && chunk.find_chunk(OGF_LODDEF2) < 8 * sizeof(FLOD::_face::v))
    {
        piece_fail(error, "visual %u: a LOD without its definition, or with one cut short", index);
        return false;
    }
    if (chunk.find_chunk(OGF_FASTPATH))
    {
        u32 fast_vertices = 0, fast_indices = 0;
        IReader* path = chunk.open_chunk(OGF_FASTPATH);
        IReader* def = path ? path->open_chunk(OGF_GCONTAINER) : nullptr;
        bool ok = def && fast && piece_check_range(*fast, *def, error, index, true, &fast_vertices, &fast_indices);
        if (def)
            def->close();
        // A progressive visual's fast geometry has windows of its own.
        bool windows = true;
        if (ok && H.type == MT_PROGRESSIVE)
        {
            const size_t size = path->find_chunk(OGF_SWIDATA);
            windows = piece_check_swidata(*path, size, fast_vertices, fast_indices);
        }
        if (path)
            path->close();
        if (!ok)
        {
            if (error.empty())
                piece_fail(error, "visual %u has fast geometry and the piece no piece.geomx", index);
            return false;
        }
        if (!windows)
        {
            piece_fail(error, "visual %u: its fast geometry has no windows, or windows outside it", index);
            return false;
        }
    }
    if (H.type == MT_TREE_PM)
    {
        const u32 id = chunk.find_chunk(OGF_SWICONTAINER) >= 4 ? chunk.r_u32() : u32(-1);
        if (id >= swis.size())
        {
            piece_fail(error, "visual %u names a sliding window the piece does not have", index);
            return false;
        }
        // The piece's window item against this tree's own geometry.
        const FSlideWindowItem& item = swis[id];
        bool inside = item.count > 0 && item.sw;
        for (u32 i = 0; inside && i < item.count; ++i)
            inside = u64(item.sw[i].offset) + u64(item.sw[i].num_tris) * 3 <= indices && item.sw[i].num_verts <= vertices;
        if (!inside)
        {
            piece_fail(error, "visual %u: its sliding windows lie outside its geometry", index);
            return false;
        }
    }
    // A hierarchy and a LOD are their children: without the list their loader stops the game.
    if (parent && !chunk.find_chunk(OGF_CHILDREN_L))
    {
        piece_fail(error, "visual %u: a hierarchy without its children", index);
        return false;
    }
    if (const size_t size = chunk.find_chunk(OGF_CHILDREN_L))
    {
        const u32 count = size >= 4 ? chunk.r_u32() : u32(-1);
        if (count == u32(-1) || size_t(count) * 4 + 4 > size)
        {
            piece_fail(error, "visual %u: its children are cut short", index);
            return false;
        }
        for (u32 i = 0; i < count; ++i)
        {
            // A child loads before its parent, as in the level: it is an earlier visual of the piece.
            if (chunk.r_u32() >= index)
            {
                piece_fail(error, "visual %u names a child that is not an earlier visual of the piece", index);
                return false;
            }
        }
    }
    return true;
}

// The task's part: the files, every check, the device buffers. Runs on a worker, or on the main thread when the
// piece loads with the level.
void piece_read(const PieceState& piece, PieceLoad& out)
{
    CTimer timer;
    timer.Start();
    const auto path = [&](pcstr file) {
        string_path p;
        strconcat(p, piece.folder.c_str(), file);
        return xr_string(p);
    };

    if (!xms_read_physical(path("piece").c_str(), out.file))
    {
        out.error.assign("piece is not readable");
        return;
    }
    PieceGeom geom, fast;
    if (!xms_read_physical(path("piece.geom").c_str(), geom.bytes))
    {
        out.error.assign("piece.geom is not readable");
        return;
    }
    const bool has_fast = xms_read_physical(path("piece.geomx").c_str(), fast.bytes);
    if (!piece_parse_geom(geom, false, out.tables, out.error) ||
        (has_fast && !piece_parse_geom(fast, true, out.tables, out.error)))
        return;

    IReader file(out.file.data(), out.file.size());
    hdrLEVEL H;
    if (file.find_chunk(fsL_HEADER) != sizeof(H))
    {
        out.error.assign("piece has no header");
        return;
    }
    file.r(&H, sizeof(H));
    if (H.XRLC_version != XRCL_PRODUCTION_VERSION)
    {
        piece_fail(out.error, "piece is of the compiler's version %u, the game reads %u", H.XRLC_version,
            XRCL_PRODUCTION_VERSION);
        return;
    }

    // The shader table: a texture whose file lies in the piece's folder is the piece's own, renamed so that it keeps
    // its first characters (the deferred blender tells a lightmap by its name) and nothing else answers to it.
    IReader* shaders = file.open_chunk(fsL_SHADERS);
    if (!shaders || shaders->elapsed() < 4)
    {
        if (shaders)
            shaders->close();
        out.error.assign("piece has no shader table");
        return;
    }
    const u32 serial = piece_serial.fetch_add(1, std::memory_order_relaxed);
    const u32 shader_count = shaders->r_u32();
    for (u32 i = 0; i < shader_count; ++i)
    {
        const void* end = shaders->elapsed() > 0 ? memchr(shaders->pointer(), 0, shaders->elapsed()) : nullptr;
        if (!end)
        {
            out.error.assign("the shader table is cut short");
            shaders->close();
            return;
        }
        xr_string entry;
        shaders->r_stringZ(entry);
        const size_t slash = entry.find('/');
        if (entry.empty() || slash == xr_string::npos)
        {
            if (!entry.empty())
            {
                piece_fail(out.error, "shader %u [%s] names no texture", i, entry.c_str());
                shaders->close();
                return;
            }
            out.shaders.emplace_back();
            continue;
        }
        xr_string renamed = entry.substr(0, slash + 1);
        size_t from = slash + 1;
        for (bool first = true;; first = false)
        {
            const size_t comma = entry.find(',', from);
            const xr_string texture = entry.substr(from, comma == xr_string::npos ? xr_string::npos : comma - from);
            if (!first)
                renamed += ',';
            string_path file_name;
            strconcat(file_name, piece.folder.c_str(), texture.c_str(), ".dds");
            for (char* p = file_name; *p; ++p)
                if (*p == '/')
                    *p = _DELIMITER;
            if (!texture.empty() && piece_file_exists(file_name))
            {
                string64 suffix;
                xr_sprintf(suffix, "@piece%u", serial);
                xr_string own = texture + suffix;
                xr_strlwr(own);
                out.textures.emplace_back(own, file_name);
                renamed += own;
            }
            else
                renamed += texture;
            if (comma == xr_string::npos)
                break;
            from = comma + 1;
        }
        out.shaders.push_back(std::move(renamed));
    }
    shaders->close();

    // The visuals, every one checked before anything of them is made.
    IReader* visuals = file.open_chunk(fsL_VISUALS);
    if (!visuals)
    {
        out.error.assign("piece has no visuals");
        return;
    }
    for (u32 index = 0;; ++index)
    {
        IReader* chunk = visuals->open_chunk(index);
        if (!chunk)
            break;
        const bool ok = piece_check_visual(*chunk, index, shader_count, geom, has_fast ? &fast : nullptr,
            out.tables.swis, out.error);
        chunk->close();
        if (!ok)
        {
            visuals->close();
            return;
        }
        out.visuals = index + 1;
    }
    visuals->close();

    const size_t roots_size = file.find_chunk(piece_roots_chunk);
    const u32 root_count = roots_size >= 4 ? file.r_u32() : 0;
    if (!root_count || roots_size != 4 + size_t(root_count) * 8)
    {
        out.error.assign("piece has no roots, or they are cut short");
        return;
    }
    for (u32 i = 0; i < root_count; ++i)
    {
        const u32 visual = file.r_u32();
        const u16 sector = file.r_u16();
        file.r_u16();
        if (visual >= out.visuals)
        {
            piece_fail(out.error, "root %u names visual %u, the piece has %u", i, visual, out.visuals);
            return;
        }
        out.roots.push_back({visual, sector});
    }

    piece_create_buffers(geom, false, out.tables);
    if (has_fast)
        piece_create_buffers(fast, true, out.tables);
    out.task_ms = timer.GetElapsed_sec() * 1000.f;
}
} // namespace

bool CRender::PieceTexturePath(pcstr name, string_path& path) const
{
    if (!m_piece_textures_count.load(std::memory_order_acquire))
        return false;
    std::lock_guard lock(m_piece_textures_lock);
    for (const auto& [own, file] : m_piece_textures)
    {
        if (!xr_stricmp(own.c_str(), name))
        {
            xr_strcpy(path, file.c_str());
            return true;
        }
    }
    return false;
}

IReader* CRender::PieceTextureRead(pcstr path) const
{
    if (!m_piece_textures_count.load(std::memory_order_acquire))
        return nullptr;
    {
        std::lock_guard lock(m_piece_textures_lock);
        const bool own = std::any_of(m_piece_textures.begin(), m_piece_textures.end(),
            [&](const auto& texture) { return !xr_stricmp(texture.second.c_str(), path); });
        if (!own)
            return nullptr;
    }
    xr_vector<u8> bytes;
    if (!xms_read_physical(path, bytes))
        return nullptr;
    u8* data = xr_alloc<u8>(bytes.size());
    CopyMemory(data, bytes.data(), bytes.size());
    return xr_new<PieceFileReader>(data, bytes.size());
}

void CRender::LoadPieces()
{
    ZoneScoped;

    if (!XMS::Active())
        return;
    string_path level_name;
    piece_level_name(level_name);
    if (!level_name[0])
        return;

    LevelPieces* found = nullptr;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled || !XMS::ModuleApplies(m))
            continue;
        string_path dir;
        xr_sprintf(dir, "%slevels" DELIMITER "%s" DELIMITER "pieces" DELIMITER, m.root.c_str(), level_name);
        xr_vector<xr_string> manifests;
        XMS::ListFilesRelative(dir, "piece.ltx", true, manifests);
        for (const xr_string& rel : manifests)
        {
            // <name>\piece.ltx: one folder a piece
            const size_t slash = rel.find(_DELIMITER);
            if (slash == xr_string::npos || rel.find(_DELIMITER, slash + 1) != xr_string::npos)
                continue;
            const xr_string piece_name = rel.substr(0, slash);
            string_path manifest;
            strconcat(manifest, dir, rel.c_str());
            xr_vector<XMS::IniRecord> records;
            if (!XMS::ReadIniRecords(manifest, records))
                continue;

            auto* piece = xr_new<LevelPieces::Piece>();
            piece->name = m.id + "/" + piece_name;
            strconcat(manifest, dir, piece_name.c_str(), DELIMITER);
            piece->folder = manifest;
            int version = 0;
            bool box_ok = false, gated = false;
            xr_string problem;
            Fbox box;
            for (const XMS::IniRecord& r : records)
            {
                if (r.section != "piece")
                    continue;
                if (r.key == "version")
                    version = atoi(r.value.c_str());
                else if (r.key == "box")
                    box_ok = 6 == sscanf(r.value.c_str(), "%f , %f , %f , %f , %f , %f", &box.vMin.x, &box.vMin.y,
                        &box.vMin.z, &box.vMax.x, &box.vMax.y, &box.vMax.z) && box.is_valid();
                else if (r.key == "sector")
                    piece->sector = r.value == "auto" ? -1 : atoi(r.value.c_str());
                else if (r.key == "stream")
                    piece->stream = r.value != "false";
                else if (r.key == "mode")
                    gated = !r.value.empty() && !XMS::ModeActive(r.value.c_str());
            }
            if (version != int(piece_format_version))
                piece_fail(problem, "piece.ltx is of version %d, the game reads %u", version, piece_format_version);
            else if (!box_ok)
                problem.assign("piece.ltx has no valid box");
            else if (piece->sector < -1)
                problem.assign("piece.ltx names a negative sector");
            if (gated || !problem.empty())
            {
                if (!problem.empty())
                    Msg("! [pieces] %s: %s", piece->name.c_str(), problem.c_str());
                xr_delete(piece);
                continue;
            }
            box.getsphere(piece->center, piece->radius);
            if (!found)
                found = xr_new<LevelPieces>();
            found->pieces.push_back(piece);
        }
    }
    if (!found)
        return;
    m_pieces = found;

    u32 streamed = 0;
    for (const LevelPieces::Piece* piece : m_pieces->pieces)
        streamed += piece->stream ? 1 : 0;
    Msg("* [pieces] %s: %u piece(s) found, %u streamed", level_name, u32(m_pieces->pieces.size()),
        ps_r__pieces_stream ? streamed : 0);

    // What is not streamed loads with the level; the rest at the first frame, where the camera stands.
    for (u32 i = 0; i < m_pieces->pieces.size(); ++i)
    {
        LevelPieces::Piece& piece = *m_pieces->pieces[i];
        if (piece.stream && ps_r__pieces_stream)
            continue;
        piece.load = xr_new<LevelPieces::Load>();
        piece_read(piece, *piece.load);
        AttachPiece(*m_pieces, i);
    }
}

// The safe point's part: the shaders, the textures, the visuals and the links into the sector roots. The caller
// told the systems that keep the tree (NotifyPieces) when the level is already running.
bool CRender::AttachPiece(LevelPieces& pieces, u32 index)
{
    LevelPieces::Piece& piece = *pieces.pieces[index];
    LevelPieces::Load* load = piece.load;
    piece.load = nullptr;
    if (!load->error.empty())
    {
        Msg("! [pieces] %s: %s", piece.name.c_str(), load->error.c_str());
        piece.failed = true;
        piece_free_load(load);
        return false;
    }

    CTimer timer;
    timer.Start();
    piece.tables = std::move(load->tables);
    load->tables = {};
    PieceTables& tables = piece.tables;

    // The renamed textures answer before their shaders ask for them.
    if (!load->textures.empty())
    {
        std::lock_guard lock(m_piece_textures_lock);
        for (auto& texture : load->textures)
        {
            piece.textures.push_back(texture.first);
            m_piece_textures.push_back(std::move(texture));
        }
        m_piece_textures_count.store(u32(m_piece_textures.size()), std::memory_order_release);
    }

    tables.shaders.resize(load->shaders.size());
    for (size_t i = 0; i < load->shaders.size(); ++i)
    {
        const xr_string& entry = load->shaders[i];
        const size_t slash = entry.find('/');
        if (slash == xr_string::npos)
            continue;
        tables.shaders[i] = Resources->Create(entry.substr(0, slash).c_str(), entry.substr(slash + 1).c_str());
    }

    // While the visuals load, every id they name is the piece's (checked by the task).
    {
        IReader file(load->file.data(), load->file.size());
        IReader* visuals = file.open_chunk(fsL_VISUALS);
        s_piece_loading = &tables;
        for (u32 i = 0; i < load->visuals; ++i)
        {
            IReader* chunk = visuals->open_chunk(i);
            ogf_header H;
            chunk->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
            dxRender_Visual* visual = Models->Instance_Create(H.type);
            visual->Load(piece.name.c_str(), chunk, 0);
            tables.visuals.push_back(visual);
            chunk->close();
        }
        s_piece_loading = nullptr;
        visuals->close();
    }

    // The roots hang on the level's sectors, as an overlay does (AttachOverlayVisual).
    auto& dsgraph = get_imm_context();
    for (const LevelPieces::Root& root : load->roots)
    {
        dxRender_Visual* visual = tables.visuals[root.visual];
        auto sector_id = IRender_Sector::INVALID_SECTOR_ID;
        if (piece.sector >= 0)
            sector_id = IRender_Sector::sector_id_t(piece.sector);
        else if (root.sector != piece_sector_auto)
            sector_id = IRender_Sector::sector_id_t(root.sector);
        else
            sector_id = dsgraph.detect_sector(visual->vis.sphere.P);
        if (sector_id == IRender_Sector::INVALID_SECTOR_ID || size_t(sector_id) >= dsgraph.Sectors.size())
        {
            Msg("! [pieces] %s: visual %u has no sector of the level (%d, the level has %zu)", piece.name.c_str(),
                root.visual, int(sector_id), dsgraph.Sectors.size());
            continue;
        }
        dxRender_Visual* parent = static_cast<CSector*>(dsgraph.get_sector(sector_id))->root();
        if (!parent || parent->getType() != MT_HIERRARHY)
        {
            Msg("! [pieces] %s: sector %d has no hierarchy root", piece.name.c_str(), int(sector_id));
            continue;
        }
        static_cast<FHierrarhyVisual*>(parent)->children.push_back(visual);
        // The root's bounds gate the sector's subtree: grown only when the piece sticks out of them, a larger box
        // costs every base visual of the sector its fully-inside test.
        if (!parent->vis.box.contains(visual->vis.box))
        {
            parent->vis.box.merge(visual->vis.box);
            parent->vis.box.getsphere(parent->vis.sphere.P, parent->vis.sphere.R);
        }
        piece.links.push_back({parent, visual});
    }

    piece.buffers = u32(tables.nVB.size() + tables.nIB.size() + tables.xVB.size() + tables.xIB.size());
    piece.attached = true;
    piece.attached_at = Device.dwTimeGlobal;
    Msg("* [pieces] %s: attached, %u visual(s), %u root(s), %u buffer(s), %u texture(s), task %.1f ms, safe point "
        "%.1f ms",
        piece.name.c_str(), u32(tables.visuals.size()), u32(piece.links.size()), piece.buffers,
        u32(piece.textures.size()), load->task_ms, timer.GetElapsed_sec() * 1000.f);
    xr_delete(load);
    return true;
}

void CRender::ReleasePiece(LevelPieces& pieces, u32 index)
{
    LevelPieces::Piece& piece = *pieces.pieces[index];
    for (const overlay_visual& link : piece.links)
        std::erase(static_cast<FHierrarhyVisual*>(link.parent)->children, link.visual);
    piece.links.clear();

    // The parents first: a hierarchy's children are links (bDontDelete), freed as visuals of their own.
    PieceTables& tables = piece.tables;
    for (auto it = tables.visuals.rbegin(); it != tables.visuals.rend(); ++it)
    {
        (*it)->Release();
        xr_delete(*it);
    }
    tables.visuals.clear();
    tables.shaders.clear();
    piece_free_tables(tables);

    if (!piece.textures.empty())
    {
        std::lock_guard lock(m_piece_textures_lock);
        for (const xr_string& own : piece.textures)
            std::erase_if(m_piece_textures, [&](const auto& texture) { return texture.first == own; });
        m_piece_textures_count.store(u32(m_piece_textures.size()), std::memory_order_release);
    }
    piece.textures.clear();
    piece.attached = false;
    Msg("* [pieces] %s: released", piece.name.c_str());
}

void CRender::NotifyPieces(bool before, bool visible)
{
#if RENDER == R_R4
    // As the vegetation's switches do (da_veg_apply_switch): the GPU-driven trees and the ray tracing scene let go of
    // the tree before it changes and gather it again after. The trees' registry is made again for a piece without
    // trees too: while it is applied it keeps each sector root's children as they were when it was built.
    if (before)
    {
        da_gpu_trees_level_unload();
        da_rt_level_unload();
        return;
    }
    da_gpu_trees_level_load();
    da_rt_level_load();
    // The lamps' cached static slices hold the casters they drew, and their lists of a piece's trees by pointer:
    // every one is drawn again.
    da_smap_cache_reset();
#endif
    // A change the camera can see: the temporal history starts over.
    if (!before && visible)
        da_history_request_reset("level pieces");
}

void CRender::PiecesFrame()
{
    ZoneScoped;

    LevelPieces& P = *m_pieces;
    const u32 now = Device.dwTimeGlobal;
    const Fvector& camera = Device.vCameraPosition;
    float far_plane = g_pGamePersistent ? g_pGamePersistent->Environment().CurrentEnv.far_plane : 0.f;
    if (far_plane < 1.f)
        far_plane = 1000.f;
    const auto wanted = [&](const LevelPieces::Piece& piece) {
        if (!ps_r__pieces_stream || !piece.stream)
            return true;
        const float reach = far_plane + piece.radius + piece_margin;
        const float distance = camera.distance_to(piece.center);
        if (!piece.attached)
            return distance < reach;
        return distance <= reach * piece_leave_factor || now - piece.attached_at < piece_keep_ms;
    };
    const auto visible = [&](const LevelPieces::Piece& piece) {
        return camera.distance_to(piece.center) - piece.radius < far_plane;
    };

    xr_vector<u32> leaving, arriving;
    // A finished task: its piece waits for the others being read.
    if (P.reading && !P.reading->reading.load(std::memory_order_acquire))
    {
        const auto it = std::find(P.pieces.begin(), P.pieces.end(), P.reading);
        const u32 index = u32(it - P.pieces.begin());
        P.reading = nullptr;
        if (P.ready.empty())
            P.ready_frame = Device.dwFrame;
        P.ready.push_back(index);
    }

    const bool first = P.first_frame;
    if (first || Device.dwFrame >= P.next_check)
    {
        P.next_check = Device.dwFrame + piece_check_frames;
        P.first_frame = false;
        for (u32 i = 0; i < P.pieces.size(); ++i)
        {
            LevelPieces::Piece& piece = *P.pieces[i];
            if (piece.attached && !wanted(piece))
                leaving.push_back(i);
        }
        // The first frame of the level takes every piece it can see at once: the level starts whole.
        if (first)
        {
            if (P.reading)
            {
                while (P.reading->reading.load(std::memory_order_acquire))
                    std::this_thread::yield();
                arriving.push_back(u32(std::find(P.pieces.begin(), P.pieces.end(), P.reading) - P.pieces.begin()));
                P.reading = nullptr;
            }
            for (u32 i = 0; i < P.pieces.size(); ++i)
            {
                LevelPieces::Piece& piece = *P.pieces[i];
                if (piece.attached || piece.failed || piece.load || !wanted(piece))
                    continue;
                piece.load = xr_new<LevelPieces::Load>();
                piece_read(piece, *piece.load);
                arriving.push_back(i);
            }
        }
    }

    // What was read is made together (piece_batch): now, when no other piece is waiting to be read.
    if (!P.ready.empty())
    {
        bool more = !!P.reading;
        for (size_t i = 0; !more && i < P.pieces.size(); ++i)
        {
            const LevelPieces::Piece& piece = *P.pieces[i];
            more = !piece.attached && !piece.failed && !piece.load && wanted(piece);
        }
        if (first || !more || P.ready.size() >= piece_batch || Device.dwFrame >= P.ready_frame + piece_batch_frames)
        {
            for (const u32 i : P.ready)
            {
                if (wanted(*P.pieces[i]))
                    arriving.push_back(i);
                else
                    piece_free_load(P.pieces[i]->load);
            }
            P.ready.clear();
        }
    }

    if (!leaving.empty() || !arriving.empty())
    {
        bool seen = false;
        for (const u32 i : leaving)
            seen = seen || visible(*P.pieces[i]);
        for (const u32 i : arriving)
            seen = seen || visible(*P.pieces[i]);
        NotifyPieces(true, seen);
        for (const u32 i : leaving)
            ReleasePiece(P, i);
        for (const u32 i : arriving)
            AttachPiece(P, i);
        NotifyPieces(false, seen);
    }

    // The next task: the nearest piece wanted and not there.
    if (!P.reading)
    {
        LevelPieces::Piece* next = nullptr;
        float best = flt_max;
        for (LevelPieces::Piece* piece : P.pieces)
        {
            if (piece->attached || piece->failed || piece->load || !wanted(*piece))
                continue;
            const float distance = camera.distance_to(piece->center);
            if (distance < best)
            {
                best = distance;
                next = piece;
            }
        }
        if (next)
        {
            next->load = xr_new<LevelPieces::Load>();
            next->reading.store(true, std::memory_order_release);
            P.reading = next;
            TaskScheduler->AddTask([next] {
                piece_read(*next, *next->load);
                next->reading.store(false, std::memory_order_release);
            });
        }
    }
}

void CRender::UnloadPieces()
{
    if (!m_pieces)
        return;
    LevelPieces& P = *m_pieces;
    // A task still reading owns its piece's load until it ends.
    if (P.reading)
    {
        while (P.reading->reading.load(std::memory_order_acquire))
            std::this_thread::yield();
        P.reading = nullptr;
    }
    for (u32 i = 0; i < P.pieces.size(); ++i)
    {
        LevelPieces::Piece* piece = P.pieces[i];
        if (piece->attached)
            ReleasePiece(P, i);
        piece_free_load(piece->load);
        xr_delete(piece);
    }
    xr_delete(m_pieces);
}

// ---- exact cuts ---------------------------------------------------------------
// overlay.cut (docs/dead-air/XMS_ARCHITECTURE.md 8.4): the regions clip the level's own normal and progressive visuals
// with the clip the collision uses. A visual a region cut draws from buffers of its own with what stays - a progressive
// one its full detail, neither its fast geometry - and one left with nothing is unlinked like a hidden one. It runs
// before the overlays and the pieces attach: they are never cut. Trees, LODs and everything else are left alone.
namespace
{
template <typename T>
T cut_load(const u8* at)
{
    T v;
    memcpy(&v, at, sizeof(T));
    return v;
}

template <typename T>
void cut_store(u8* at, T v)
{
    memcpy(at, &v, sizeof(T));
}

float cut_half_to_float(u16 h)
{
    const float sign = (h & 0x8000) ? -1.f : 1.f;
    const u32 exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0)
        return sign * float(mantissa) * 5.9604645e-8f;
    if (exponent == 0x1F)
        return mantissa ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity();
    return sign * std::ldexp(1.f + float(mantissa) / 1024.f, int(exponent) - 15);
}

u16 cut_float_to_half(float f)
{
    const u16 sign = std::signbit(f) ? 0x8000 : 0;
    const float a = std::fabs(f);
    if (a != a)
        return 0x7E00;
    if (!(a < 65520.f))
        return sign | 0x7C00;
    if (a < 6.1035156e-5f)
        return sign | u16(iFloor(a / 5.9604645e-8f + .5f));
    int e;
    const float m = std::frexp(a, &e); // a = m * 2^e, m in [0.5, 1)
    u32 mantissa = u32(iFloor((2.f * m - 1.f) * 1024.f + .5f));
    u32 exponent = u32(e - 1 + 15);
    if (mantissa == 1024)
    {
        mantissa = 0;
        ++exponent;
    }
    return sign | u16(exponent << 10) | u16(mantissa);
}

// A vertex format as the cut reads it: the position, and the packed base texture coordinate of the level's formats
// (a short TEXCOORD0 whose fractions are the alpha of the tangent and the binormal, unpack_tc_base in the shaders).
struct CutFormat
{
    const VertexElement* decl{};
    u32 elements{}, stride{};
    int position{-1}, tc{-1}, tangent{-1}, binormal{-1};
};

CutFormat cut_format(const VertexElement* decl)
{
    CutFormat f;
    f.decl = decl;
    f.elements = GetDeclLength(decl);
    f.stride = GetDeclVertexSize(decl, 0);
    for (u32 e = 0; e < f.elements; ++e)
    {
        const VertexElement& el = decl[e];
        if (el.Stream != 0)
            continue;
        if (el.Usage == D3DDECLUSAGE_POSITION && el.Type == D3DDECLTYPE_FLOAT3 && el.UsageIndex == 0)
            f.position = el.Offset;
        else if (el.Usage == D3DDECLUSAGE_TEXCOORD && el.UsageIndex == 0 &&
            (el.Type == D3DDECLTYPE_SHORT2 || el.Type == D3DDECLTYPE_SHORT4))
            f.tc = el.Offset;
        else if (el.Usage == D3DDECLUSAGE_TANGENT && el.Type == D3DDECLTYPE_D3DCOLOR && el.UsageIndex == 0)
            f.tangent = el.Offset;
        else if (el.Usage == D3DDECLUSAGE_BINORMAL && el.Type == D3DDECLTYPE_D3DCOLOR && el.UsageIndex == 0)
            f.binormal = el.Offset;
    }
    return f;
}

// The vertex at `w` of the triangle `src`, every element interpolated by its type; directions are renormalised. What
// has no linear meaning (the 10-bit packings) takes the nearest vertex's value.
void cut_blend(const CutFormat& f, const u8* const src[3], const float w[3], u8* dst)
{
    const int nearest = w[0] >= w[1] && w[0] >= w[2] ? 0 : w[1] >= w[2] ? 1 : 2;
    memcpy(dst, src[nearest], f.stride);
    const auto mix = [&](auto read) { return w[0] * read(src[0]) + w[1] * read(src[1]) + w[2] * read(src[2]); };
    for (u32 e = 0; e < f.elements; ++e)
    {
        const VertexElement& el = f.decl[e];
        if (el.Stream != 0)
            continue;
        const u32 o = el.Offset;
        const bool direction = el.Usage == D3DDECLUSAGE_NORMAL || el.Usage == D3DDECLUSAGE_TANGENT ||
            el.Usage == D3DDECLUSAGE_BINORMAL;
        switch (el.Type)
        {
        case D3DDECLTYPE_FLOAT1:
        case D3DDECLTYPE_FLOAT2:
        case D3DDECLTYPE_FLOAT3:
        case D3DDECLTYPE_FLOAT4:
        {
            const u32 n = u32(el.Type - D3DDECLTYPE_FLOAT1) + 1;
            float v[4];
            for (u32 k = 0; k < n; ++k)
                v[k] = mix([&](const u8* s) { return cut_load<float>(s + o + 4 * k); });
            if (direction && n >= 3)
            {
                Fvector d{v[0], v[1], v[2]};
                d.normalize_safe();
                v[0] = d.x, v[1] = d.y, v[2] = d.z;
            }
            for (u32 k = 0; k < n; ++k)
                cut_store(dst + o + 4 * k, v[k]);
            break;
        }
        case D3DDECLTYPE_FLOAT16_2:
        case D3DDECLTYPE_FLOAT16_4:
        {
            const u32 n = el.Type == D3DDECLTYPE_FLOAT16_2 ? 2 : 4;
            for (u32 k = 0; k < n; ++k)
                cut_store(dst + o + 2 * k,
                    cut_float_to_half(mix([&](const u8* s) { return cut_half_to_float(cut_load<u16>(s + o + 2 * k)); })));
            break;
        }
        case D3DDECLTYPE_D3DCOLOR:
        case D3DDECLTYPE_UBYTE4:
        case D3DDECLTYPE_UBYTE4N:
        {
            float v[4];
            for (u32 k = 0; k < 4; ++k)
                v[k] = mix([&](const u8* s) { return float(s[o + k]); });
            if (direction)
            {
                // Packed as (n + 1) / 2 * 255 per byte (u8_vec4); the fourth byte is data of its own.
                Fvector d{v[0] / 127.5f - 1.f, v[1] / 127.5f - 1.f, v[2] / 127.5f - 1.f};
                d.normalize_safe();
                v[0] = (d.x + 1.f) * 127.5f, v[1] = (d.y + 1.f) * 127.5f, v[2] = (d.z + 1.f) * 127.5f;
            }
            for (u32 k = 0; k < 4; ++k)
                dst[o + k] = u8(std::clamp(iFloor(v[k] + (direction && k < 3 ? 0.f : .5f)), 0, 255));
            break;
        }
        case D3DDECLTYPE_SHORT2:
        case D3DDECLTYPE_SHORT4:
        case D3DDECLTYPE_SHORT2N:
        case D3DDECLTYPE_SHORT4N:
        {
            const u32 n = el.Type == D3DDECLTYPE_SHORT2 || el.Type == D3DDECLTYPE_SHORT2N ? 2 : 4;
            for (u32 k = 0; k < n; ++k)
                cut_store(dst + o + 2 * k, s16(std::clamp(
                    iFloor(mix([&](const u8* s) { return float(cut_load<s16>(s + o + 2 * k)); }) + .5f), -32768, 32767)));
            break;
        }
        case D3DDECLTYPE_USHORT2N:
        case D3DDECLTYPE_USHORT4N:
        {
            const u32 n = el.Type == D3DDECLTYPE_USHORT2N ? 2 : 4;
            for (u32 k = 0; k < n; ++k)
                cut_store(dst + o + 2 * k, u16(std::clamp(
                    iFloor(mix([&](const u8* s) { return float(cut_load<u16>(s + o + 2 * k)); }) + .5f), 0, 65535)));
            break;
        }
        default: break;
        }
    }

    // The packed base coordinate is one number in two elements: interpolated whole, then packed again (s24_tc_base).
    if (f.tc >= 0 && f.tangent >= 0 && f.binormal >= 0)
    {
        for (u32 axis = 0; axis < 2; ++axis)
        {
            const u32 fraction = u32(axis ? f.binormal : f.tangent) + 3;
            const float v = mix([&](const u8* s) {
                return float(cut_load<s16>(s + f.tc + 2 * axis)) + float(s[fraction]) / 255.f;
            });
            const int primary = std::clamp(iFloor(v), -32768, 32767);
            cut_store(dst + f.tc + 2 * axis, s16(primary));
            dst[fraction] = u8(std::clamp(iFloor(255.5f * (v - float(primary))), 0, 255));
        }
    }
}

// The rectangle's box in the world, a centimetre wider: what lies beyond it cannot be cut.
Fbox2 cut_area(const XMS::CutRegion& region)
{
    const float cs = _abs(_cos(region.yaw)), sn = _abs(_sin(region.yaw));
    const float ex = cs * region.half.x + sn * region.half.z + .01f;
    const float ez = sn * region.half.x + cs * region.half.z + .01f;
    Fbox2 area;
    area.min.set(region.centre.x - ex, region.centre.z - ez);
    area.max.set(region.centre.x + ex, region.centre.z + ez);
    return area;
}
} // namespace

void CRender::ApplyLevelCuts()
{
    string_path level_name;
    piece_level_name(level_name);
    XMS::LevelCuts cuts;
    if (XMS::Active() && level_name[0])
        XMS::ReadLevelCuts(level_name, cuts);
    const u32 collision = XMS::TakeCollisionCuts(level_name);
    if (cuts.regions.empty())
        return;
    const u32 visuals = GEnv.isDedicatedServer ? 0 : CutLevelVisuals(cuts, level_name);
    Msg("* [cuts] %s: %u region(s), %u collision triangle(s) cut, %u visual(s) cut", level_name,
        u32(cuts.regions.size()), collision, visuals);
}

u32 CRender::CutLevelVisuals(const XMS::LevelCuts& cuts, pcstr level_name)
{
    ZoneScoped;

    xr_vector<Fbox2> areas;
    for (const XMS::CutRegion& region : cuts.regions)
        areas.push_back(cut_area(region));

    struct Candidate
    {
        Fvisual* visual;
        u32 vb, ib;
        u32 ibase, icount; // its full detail
    };
    xr_vector<Candidate> candidates;
    for (dxRender_Visual* v : Visuals)
    {
        if (!v || (v->Type != MT_NORMAL && v->Type != MT_PROGRESSIVE))
            continue;
        const Fbox& box = v->vis.box;
        const bool touched = std::any_of(areas.begin(), areas.end(), [&](const Fbox2& a) {
            return box.vMax.x >= a.min.x && box.vMin.x <= a.max.x && box.vMax.z >= a.min.y && box.vMin.z <= a.max.y;
        });
        Fvisual* f = static_cast<Fvisual*>(v);
        if (!touched || !f->p_rm_Vertices || !f->p_rm_Indices || nVB.empty() || nIB.empty())
            continue;
        // Only geometry in the level's own buffers is the level's.
        if (f->p_rm_Vertices < nVB.data() || f->p_rm_Vertices >= nVB.data() + nVB.size() ||
            f->p_rm_Indices < nIB.data() || f->p_rm_Indices >= nIB.data() + nIB.size())
            continue;
        Candidate c{f, u32(f->p_rm_Vertices - nVB.data()), u32(f->p_rm_Indices - nIB.data()), f->iBase,
            f->dwPrimitives * 3};
        if (v->Type == MT_PROGRESSIVE)
        {
            const FSlideWindowItem& w = static_cast<const FProgressive*>(v)->windows();
            if (!w.count || !w.sw)
                continue;
            c.ibase += w.sw[0].offset;
            c.icount = u32(w.sw[0].num_tris) * 3;
        }
        candidates.push_back(c);
    }
    if (candidates.empty())
        return 0;

    // The level's buffers again, those the candidates use: the device keeps no copy on this side.
    xr_vector<xr_vector<u8>> vb_bytes(nVB.size()), ib_bytes(nIB.size());
    {
        xr_vector<u8> want_vb(nVB.size()), want_ib(nIB.size());
        for (const Candidate& c : candidates)
            want_vb[c.vb] = want_ib[c.ib] = 1;
        CStreamReader* geom = FS.rs_open("$level$", "level.geom");
        if (!geom)
        {
            Msg("! [cuts] %s: level.geom cannot be read again, no visual is cut", level_name);
            return 0;
        }
        if (CStreamReader* fs = geom->open_chunk(fsL_VB))
        {
            const u32 count = std::min<u32>(fs->r_u32(), u32(nVB.size()));
            constexpr size_t decl_bytes = (MAXD3DDECLLENGTH + 1) * sizeof(VertexElement);
            for (u32 b = 0; b < count; ++b)
            {
                VertexElement dcl[MAXD3DDECLLENGTH + 1];
                fs->r(dcl, decl_bytes);
                fs->advance(-int(decl_bytes));
                fs->advance(int((GetDeclLength(dcl) + 1) * sizeof(VertexElement)));
                const size_t bytes = size_t(fs->r_u32()) * GetDeclVertexSize(dcl, 0);
                if (want_vb[b])
                {
                    const u8* data = static_cast<const u8*>(fs->pointer_contiguous(bytes));
                    vb_bytes[b].assign(data, data + bytes);
                }
                fs->advance(int(bytes));
            }
            fs->close();
        }
        if (CStreamReader* fs = geom->open_chunk(fsL_IB))
        {
            const u32 count = std::min<u32>(fs->r_u32(), u32(nIB.size()));
            for (u32 b = 0; b < count; ++b)
            {
                const size_t bytes = size_t(fs->r_u32()) * 2;
                if (want_ib[b])
                {
                    const u8* data = static_cast<const u8*>(fs->pointer_contiguous(bytes));
                    ib_bytes[b].assign(data, data + bytes);
                }
                fs->advance(int(bytes));
            }
            fs->close();
        }
        FS.r_close(geom);
    }

    xr_vector<XMS::Cut::Region> views;
    for (const XMS::CutRegion& region : cuts.regions)
        views.push_back(region.view());

    u32 cut = 0, left_whole = 0;
    for (const Candidate& c : candidates)
    {
        Fvisual& v = *c.visual;
        const CutFormat format = cut_format(nDC[c.vb].begin());
        const u32 stride = format.stride;
        constexpr u32 max_stride = 256;
        const xr_vector<u8>& source_vb = vb_bytes[c.vb];
        const xr_vector<u8>& source_ib = ib_bytes[c.ib];
        if (format.position < 0 || !stride || stride > max_stride ||
            (size_t(v.vBase) + v.vCount) * stride > source_vb.size() || (size_t(c.ibase) + c.icount) * 2 > source_ib.size())
            continue;

        xr_vector<u8> vertices(source_vb.begin() + size_t(v.vBase) * stride,
            source_vb.begin() + (size_t(v.vBase) + v.vCount) * stride);
        xr_vector<u16> indices(c.icount);
        memcpy(indices.data(), source_ib.data() + size_t(c.ibase) * 2, size_t(c.icount) * 2);
        if (std::any_of(indices.begin(), indices.end(), [&](u16 i) { return i >= v.vCount; }))
            continue;

        bool changed = false, overflow = false;
        xr_vector<u16> next;
        for (size_t r = 0; r < views.size() && !overflow; ++r)
        {
            if (v.vis.box.vMax.x < areas[r].min.x || v.vis.box.vMin.x > areas[r].max.x ||
                v.vis.box.vMax.z < areas[r].min.y || v.vis.box.vMin.z > areas[r].max.y)
                continue;
            next.clear();
            next.reserve(indices.size());
            for (size_t t = 0; t + 2 < indices.size() && !overflow; t += 3)
            {
                const u16 ids[3] = {indices[t], indices[t + 1], indices[t + 2]};
                u8 corners[3][max_stride];
                for (int k = 0; k < 3; ++k)
                    memcpy(corners[k], vertices.data() + size_t(ids[k]) * stride, stride);
                XMS::Cut::Result result;
                if (!XMS::Cut::ClipTriangle(views[r], reinterpret_cast<const float*>(corners[0] + format.position),
                        reinterpret_cast<const float*>(corners[1] + format.position),
                        reinterpret_cast<const float*>(corners[2] + format.position), result))
                {
                    next.insert(next.end(), ids, ids + 3);
                    continue;
                }
                changed = true;
                const u8* const src[3] = {corners[0], corners[1], corners[2]};
                u16 points[XMS::Cut::max_points];
                for (int p = 0; p < result.points && !overflow; ++p)
                {
                    const float* w = result.weights[p];
                    const int same = w[0] == 1.f ? 0 : w[1] == 1.f ? 1 : w[2] == 1.f ? 2 : -1;
                    if (same >= 0)
                    {
                        points[p] = ids[same];
                        continue;
                    }
                    // u16 indices and u16 window counts: 65535 vertices at most.
                    const size_t index = vertices.size() / stride;
                    if (index >= 65535)
                    {
                        overflow = true;
                        break;
                    }
                    vertices.resize(vertices.size() + stride);
                    cut_blend(format, src, w, vertices.data() + index * stride);
                    points[p] = u16(index);
                }
                for (int k = 0; k < result.triangles && !overflow; ++k)
                    for (int corner = 0; corner < 3; ++corner)
                        next.push_back(points[result.indices[k][corner]]);
            }
            indices.swap(next);
        }
        const u32 tris = u32(indices.size() / 3);
        if (overflow || (v.Type == MT_PROGRESSIVE && tris > 65535))
        {
            ++left_whole;
            continue;
        }
        if (!changed)
            continue;
        ++cut;

        if (!tris)
        {
            for (dxRender_Visual* parent : Visuals)
                if (parent && (parent->Type == MT_HIERRARHY || parent->Type == MT_LOD))
                    std::erase(static_cast<FHierrarhyVisual*>(parent)->children, static_cast<dxRender_Visual*>(&v));
            continue;
        }

        const u32 verts = u32(vertices.size() / stride);
        cut_geometry* geometry = xr_new<cut_geometry>();
        geometry->vb.CreateFromData(vertices.data(), vertices.size());
        geometry->ib.CreateFromData(indices.data(), indices.size() * 2);
        m_cut_geometry.push_back(geometry);

        v.rm_geom.destroy();
        v.p_rm_Vertices->Release();
        v.p_rm_Indices->Release();
        v.p_rm_Vertices = &geometry->vb;
        v.p_rm_Vertices->AddRef();
        v.p_rm_Indices = &geometry->ib;
        v.p_rm_Indices->AddRef();
        v.vBase = 0;
        v.vCount = verts;
        v.iBase = 0;
        v.iCount = tris * 3;
        v.dwPrimitives = tris;
        v.rm_geom.create(nDC[c.vb].begin(), geometry->vb, geometry->ib);
        if (v.Type == MT_PROGRESSIVE)
            static_cast<FProgressive&>(v).KeepOneWindow(u16(tris), u16(verts));
        else
            xr_delete(v.m_fast);
    }
    if (left_whole)
        Msg("! [cuts] %s: %u visual(s) would need more than 65535 vertices or triangles and stay whole", level_name,
            left_whole);
    return cut;
}

void CRender::UnloadLevelCuts()
{
    for (cut_geometry* geometry : m_cut_geometry)
        xr_delete(geometry);
    m_cut_geometry.clear();
}

#if defined(USE_DX11)
void CRender::Load3DFluid()
{
    ZoneScoped;

    // if (strstr(Core.Params,"-no_volumetric_fog"))
    if (!o.volumetricfog)
        return;

    string_path fn_game;
    if (FS.exist(fn_game, "$level$", "level.fog_vol"))
    {
        IReader* F = FS.r_open(fn_game);
        u16 version = F->r_u16();

        if (version == 3)
        {
            u32 cnt = F->r_u32();
            for (u32 i = 0; i < cnt; ++i)
            {
                dx113DFluidVolume* pVolume = xr_new<dx113DFluidVolume>();
                pVolume->Load("", F, 0);

                auto& dsgraph = get_imm_context();

                //	Attach to sector's static geometry
                const auto sector_id = dsgraph.detect_sector(pVolume->getVisData().sphere.P);
                auto* pSector = static_cast<CSector*>(dsgraph.get_sector(sector_id));
                //	3DFluid volume must be in render sector
                VERIFY(pSector);

                dxRender_Visual* pRoot = pSector->root();
                //	Sector must have root
                VERIFY(pRoot);
                VERIFY(pRoot->getType() == MT_HIERRARHY);

                ((FHierrarhyVisual*)pRoot)->children.push_back(pVolume);
                m_fluid_volumes.push_back({pRoot, pVolume});
            }
        }

        FS.r_close(F);
    }
}

void CRender::Unload3DFluid()
{
    for (auto& entry : m_fluid_volumes)
    {
        // Detach before releasing: the sector root does keep bDontDelete, but relying on that would
        // tie this to the destruction order of the visual graph.
        if (entry.parent && entry.parent->getType() == MT_HIERRARHY)
            std::erase(static_cast<FHierrarhyVisual*>(entry.parent)->children, entry.volume);

        if (entry.volume)
        {
            entry.volume->Release();
            xr_delete(entry.volume);
        }
    }
    m_fluid_volumes.clear();
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
