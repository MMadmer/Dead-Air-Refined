#include "stdafx.h"

#include "xr_area.h"
#include "xrEngine/xr_object.h"
#include "Common/LevelStructure.hpp"
#include "xrEngine/xr_collide_form.h"
#include "xrCore/XMS/xms_core.h"

#include <filesystem>


//----------------------------------------------------------------------
// Class	: CObjectSpaceData
// Purpose	: stores thread sensitive data
//----------------------------------------------------------------------
thread_local xrXRC CObjectSpaceData::xrc("object space");
thread_local collide::rq_results CObjectSpaceData::r_temp;
thread_local xr_vector<ISpatial*> CObjectSpaceData::r_spatial;

using namespace collide;

namespace
{
void prune_inactive_level_caches(const std::filesystem::path& activeCacheFile)
{
    const std::filesystem::path activeDirectory = activeCacheFile.parent_path();
    const std::filesystem::path cacheRoot = activeDirectory.parent_path();
    if (activeDirectory.empty() || cacheRoot.empty() || activeCacheFile.filename() != "objspace.bin" ||
        cacheRoot.filename() != "cdb_cache")
        return;

    std::error_code error;
    std::filesystem::directory_iterator iterator(
        cacheRoot, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    if (error)
        return;

    u32 removedDirectories = 0;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
            break;

        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.path() == activeDirectory)
            continue;

        const std::filesystem::file_status status = entry.symlink_status(error);
        if (error)
        {
            error.clear();
            continue;
        }

        if (status.type() != std::filesystem::file_type::directory)
            continue;

        bool containsCacheFile = false;
        for (const char* cacheFile : {"objspace.bin", "hom.bin", "portals.bin"})
        {
            containsCacheFile = std::filesystem::is_regular_file(entry.path() / cacheFile, error);
            if (containsCacheFile)
                break;
            error.clear();
        }

        if (!containsCacheFile)
            continue;

#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
#endif

        // Keep the virtual file index synchronized with files removed from disk.
        const std::string directory = entry.path().string() + DELIMITER;
        FS.dir_delete(directory.c_str(), true);
        const bool stillExists = std::filesystem::exists(entry.path(), error);
        if (!error && !stillExists)
            ++removedDirectories;
        else
            error.clear();
    }

#ifndef MASTER_GOLD
    if (removedDirectories)
        Msg("* Removed %u inactive level cache director%s", removedDirectories, removedDirectories == 1 ? "y" : "ies");
#endif
}
} // namespace

//----------------------------------------------------------------------
// Class	: CObjectSpace
// Purpose	: stores space slots
//----------------------------------------------------------------------
CObjectSpace::CObjectSpace(ISpatial_DB* spatialSpace)
    : SpatialSpace(spatialSpace)
{
#ifdef DEBUG
    if (GEnv.RenderFactory)
        m_pRender = xr_new<FactoryPtr<IObjectSpaceRender>>();
#endif
    m_BoundingVolume.invalidate();
}
//----------------------------------------------------------------------
CObjectSpace::~CObjectSpace()
{
#ifdef DEBUG
    xr_delete(m_pRender);
#endif
}
//----------------------------------------------------------------------

//----------------------------------------------------------------------
int CObjectSpace::GetNearest(xr_vector<ISpatial*>& q_spatial, xr_vector<IGameObject*>& q_nearest, const Fvector& point,
    float range, IGameObject* ignore_object)
{
    ZoneScoped;

    q_spatial.clear();
    // Query objects
    q_nearest.clear();
    Fsphere Q;
    Q.set(point, range);
    Fvector B;
    B.set(range, range, range);
    SpatialSpace->q_box(q_spatial, 0, STYPE_COLLIDEABLE, point, B);

    // Iterate
    for (auto& it : q_spatial)
    {
        IGameObject* O = it->dcast_GameObject();
        if (0 == O)
            continue;
        if (O == ignore_object)
            continue;
        Fsphere mS = {O->GetSpatialData().sphere.P, O->GetSpatialData().sphere.R};
        if (Q.intersect(mS))
            q_nearest.push_back(O);
    }

    return q_nearest.size();
}

//----------------------------------------------------------------------
int CObjectSpace::GetNearest(
    xr_vector<IGameObject*>& q_nearest, const Fvector& point, float range, IGameObject* ignore_object)
{
    return (GetNearest(r_spatial, q_nearest, point, range, ignore_object));
}

//----------------------------------------------------------------------
int CObjectSpace::GetNearest(xr_vector<IGameObject*>& q_nearest, ICollisionForm* obj, float range)
{
    IGameObject* O = obj->Owner();
    return GetNearest(q_nearest, O->GetSpatialData().sphere.P, range + O->GetSpatialData().sphere.R, O);
}

//----------------------------------------------------------------------

void CObjectSpace::Load(CDB::build_callback build_callback,
    CDB::serialize_callback serialize_callback,
    CDB::deserialize_callback deserialize_callback,
    CDB::remapping_materials_callback remapping_materials_callback)
{
    Load("$level$", "level.cform", build_callback, serialize_callback, deserialize_callback, remapping_materials_callback);
}

void CObjectSpace::Load(LPCSTR path, LPCSTR fname,
    CDB::build_callback build_callback,
    CDB::serialize_callback serialize_callback,
    CDB::deserialize_callback deserialize_callback,
    CDB::remapping_materials_callback remapping_materials_callback)
{
    IReader* F = FS.r_open(path, fname);
    R_ASSERT(F);
    u32 sourceCrc = 0;
    FS.archived_file_crc(path, fname, sourceCrc);
    Load(F, build_callback, serialize_callback, deserialize_callback, remapping_materials_callback, sourceCrc);
}

// ---- XMS collision overlays (.xcform) ---------------------------------------
// Module-supplied world-space triangle soups merged into the static model
// BEFORE the tree is built: base triangles keep their indices, overlay
// triangles append at the end, so rq_result.element semantics never change.
namespace
{
#pragma pack(push, 1)
struct XcformTri
{
    u32 v0, v1, v2;
    u16 material_index;
    u16 pad;
};
#pragma pack(pop)

struct XcformMerge
{
    xr_vector<Fvector> verts;
    xr_vector<CDB::TRI> tris;
    // Cut regions drop BASE triangles that fall entirely inside them, which is
    // what makes a hole possible at all - appending alone can only add matter.
    // Vertices are never removed: overlay triangles index the base vertex array
    // by offset, and compacting it would mean remapping every consumer of
    // CDB::TRI::verts.
    xr_vector<Fbox> cuts;
    u32 hash{0};
};

bool xms_load_xcform_overlays(pcstr level_folder, u32 base_verts, XcformMerge& out)
{
    if (!XMS::Active())
        return false;
    // "$level$" m_Add carries a trailing delimiter - strip it for the key
    string_path level_name;
    xr_strcpy(level_name, level_folder ? level_folder : "");
    if (const size_t len = xr_strlen(level_name); len && level_name[len - 1] == _DELIMITER)
        level_name[len - 1] = 0;

    u32 vertex_cursor = base_verts;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (!XMS::ModuleApplies(m))
            continue;
        string_path path;
        xr_sprintf(path, "%slevels" DELIMITER "%s" DELIMITER "overlay.xcform", m.root.c_str(), level_name);
        FILE* f = fopen(path, "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        const long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        xr_vector<u8> raw(size_t(std::max(0l, len)));
        const size_t got = len > 0 ? fread(raw.data(), 1, size_t(len), f) : 0;
        fclose(f);
        raw.resize(got);
        if (raw.size() < 16)
            continue;

        const u8* cursor = raw.data();
        const u8* end = raw.data() + raw.size();
        const auto read_u32 = [&]() { u32 v; memcpy(&v, cursor, 4); cursor += 4; return v; };
        const auto read_u16 = [&]() { u16 v; memcpy(&v, cursor, 2); cursor += 2; return v; };
        if (read_u32() != 0x31464358 /*XCF1*/)
        {
            Msg("! XMS: bad xcform header [%s]", path);
            continue;
        }
        const u32 xcform_version = read_u32();
        if (xcform_version != 1 && xcform_version != 2)
        {
            Msg("! XMS: unsupported xcform version %u [%s]", xcform_version, path);
            continue;
        }
        const u16 sector = read_u16();
        read_u16(); // reserved
        const u32 material_count = read_u32();
        const u16 default_material = XMS::ResolveMaterial("default");
        if (default_material == u16(-1))
        {
            // no resolver registered - ids would blow up the remap pass
            Msg("! XMS: material resolver missing, xcform overlay skipped [%s]", path);
            continue;
        }
        xr_vector<u16> materials;
        materials.reserve(material_count);
        for (u32 i = 0; i < material_count && cursor < end; ++i)
        {
            pcstr name = (pcstr)cursor;
            cursor += xr_strlen(name) + 1;
            const u16 game_id = XMS::ResolveMaterial(name);
            if (game_id == u16(-1))
                Msg("! XMS: unknown game material [%s] in %s, using default", name, path);
            materials.push_back(game_id == u16(-1) ? default_material : game_id);
        }
        if (cursor + 4 > end)
            continue;
        const u32 vertex_count = read_u32();
        if (cursor + size_t(vertex_count) * sizeof(Fvector) + 4 > end)
        {
            Msg("! XMS: truncated xcform [%s]", path);
            continue;
        }
        const Fvector* file_verts = (const Fvector*)cursor;
        cursor += size_t(vertex_count) * sizeof(Fvector);
        const u32 tri_count = read_u32();
        if (cursor + size_t(tri_count) * sizeof(XcformTri) > end)
        {
            Msg("! XMS: truncated xcform tris [%s]", path);
            continue;
        }
        const XcformTri* file_tris = (const XcformTri*)cursor;

        out.verts.insert(out.verts.end(), file_verts, file_verts + vertex_count);
        for (u32 i = 0; i < tri_count; ++i)
        {
            const XcformTri& src = file_tris[i];
            if (src.v0 >= vertex_count || src.v1 >= vertex_count || src.v2 >= vertex_count)
                continue;
            CDB::TRI tri{};
            tri.verts[0] = vertex_cursor + src.v0;
            tri.verts[1] = vertex_cursor + src.v1;
            tri.verts[2] = vertex_cursor + src.v2;
            // material here is the persistent game-mtl ID; the game's
            // build_callback remaps it to a runtime index like base tris
            tri.material =
                (src.material_index < materials.size() ? materials[src.material_index] : default_material) & 0x3FFF;
            tri.sector = sector;
            out.tris.emplace_back(tri);
        }
        cursor += size_t(tri_count) * sizeof(XcformTri);

        u32 cut_count = 0;
        if (xcform_version >= 2 && cursor + 4 <= end)
        {
            cut_count = read_u32();
            const auto read_float = [&]() { float v; memcpy(&v, cursor, 4); cursor += 4; return v; };
            for (u32 i = 0; i < cut_count && cursor + 24 <= end; ++i)
            {
                Fvector centre, extent;
                centre.x = read_float(); centre.y = read_float(); centre.z = read_float();
                extent.x = read_float(); extent.y = read_float(); extent.z = read_float();
                out.cuts.emplace_back().setb(centre, extent); // setb takes half-extents
            }
        }

        vertex_cursor = base_verts + u32(out.verts.size());
        out.hash = crc32(raw.data(), u32(raw.size()), out.hash);
        Msg("* XMS: collision overlay [%s]: %u verts, %u tris, %u cut(s) (module %s)", level_name, vertex_count,
            tri_count, cut_count, m.id.c_str());
    }
    return !out.tris.empty() || !out.cuts.empty();
}
} // namespace

void CObjectSpace::Load(IReader* F,
    CDB::build_callback build_callback,
    CDB::serialize_callback serialize_callback,
    CDB::deserialize_callback deserialize_callback,
    CDB::remapping_materials_callback remapping_materials_callback,
    u32 knownSourceCrc)
{
    ZoneScoped;

    static const bool use_cache = !strstr(Core.Params, "-no_cdb_cache");
    // crc over the WHOLE file, captured before the cursor moves
    const u32 base_crc = use_cache ? (knownSourceCrc ? knownSourceCrc : crc32(F->pointer(), F->length())) : 0;

    hdrCFORM H;
    F->r(&H, sizeof(hdrCFORM));

    Fvector* verts = (Fvector*)F->pointer();
    CDB::TRI* tris = (CDB::TRI*)(verts + H.vertcount);

    // SkyLoader: Check for the new format
    IReader* cacheStream = nullptr;
    size_t totalGeomSize = (static_cast<size_t>(H.vertcount) * sizeof(Fvector)) + (H.facecount * sizeof(CDB::TRI));
    F->advance(totalGeomSize);
    if (F->elapsed() > sizeof(u32))
    {
        u32 version = F->r_u32();
        if (version == CFORM_CACHE_CURRENT_VERSION)
            cacheStream = F;
    }

    // XMS: merge module collision overlays; the embedded tree only matches the
    // base geometry, so it is skipped when overlays are present
    XcformMerge merge;
    xr_vector<Fvector> merged_verts;
    xr_vector<CDB::TRI> merged_tris;
    if (FS.path_exist("$level$") && xms_load_xcform_overlays(FS.get_path("$level$")->m_Add, H.vertcount, merge))
    {
        merged_verts.reserve(size_t(H.vertcount) + merge.verts.size());
        merged_verts.assign(verts, verts + H.vertcount);
        merged_verts.insert(merged_verts.end(), merge.verts.begin(), merge.verts.end());

        merged_tris.reserve(size_t(H.facecount) + merge.tris.size());
        if (merge.cuts.empty())
            merged_tris.assign(tris, tris + H.facecount);
        else
        {
            // A base triangle goes only when it lies wholly inside a cut - a
            // partial test would eat the whole terrain sheet for a small box.
            // Base indices below the first survivor stay put; the ones after it
            // shift, which is safe because no triangle index outlives a level
            // load (the only place one is persisted is the OPCODE tree, and both
            // caches are invalidated right below).
            u32 dropped = 0;
            for (u32 i = 0; i < H.facecount; ++i)
            {
                const CDB::TRI& tri = tris[i];
                bool cut = false;
                for (const Fbox& region : merge.cuts)
                {
                    if (!region.contains(verts[tri.verts[0]]) || !region.contains(verts[tri.verts[1]]) ||
                        !region.contains(verts[tri.verts[2]]))
                        continue;
                    cut = true;
                    break;
                }
                if (cut)
                    ++dropped;
                else
                    merged_tris.push_back(tri);
            }
            Msg("* XMS: collision cuts removed %u base triangle(s) of %u", dropped, H.facecount);
        }
        merged_tris.insert(merged_tris.end(), merge.tris.begin(), merge.tris.end());
        for (const Fvector& v : merge.verts)
            H.aabb.modify(v);
        H.vertcount = u32(merged_verts.size());
        H.facecount = u32(merged_tris.size());
        verts = merged_verts.data();
        tris = merged_tris.data();
        cacheStream = nullptr;
        if (use_cache)
            Static.set_model_crc32(base_crc ^ merge.hash);
    }
    else if (use_cache)
        Static.set_model_crc32(base_crc);

    Create(verts, tris, H, build_callback, serialize_callback, deserialize_callback, remapping_materials_callback, cacheStream);
    FS.r_close(F);
}

void CObjectSpace::Create(Fvector* verts, CDB::TRI* tris, const hdrCFORM& H,
    CDB::build_callback build_callback,
    CDB::serialize_callback serialize_callback,
    CDB::deserialize_callback deserialize_callback,
    CDB::remapping_materials_callback remapping_materials_callback,
    IReader* cacheStream /*= nullptr*/)
{
    ZoneScoped;

    R_ASSERT(CFORM_CURRENT_VERSION == H.version);

    string_path file_name;
    static const bool use_cache = !strstr(Core.Params, "-no_cdb_cache");
    static const bool skip_crc32_check = strstr(Core.Params, "-skip_cdb_cache_crc32_check");

    strconcat(file_name, "cdb_cache" DELIMITER, FS.get_path("$level$")->m_Add, "objspace.bin");
    FS.update_path(file_name, "$app_data_root$", file_name);

    if (use_cache)
        prune_inactive_level_caches(std::filesystem::path(file_name));

    if (use_cache && cacheStream)
    {
#ifndef MASTER_GOLD
        Msg("* Loading ObjectSpace cache from level.cform...");
#endif

        // Load geometry
        Static.load_geom(verts, H.vertcount, tris, H.facecount);

        // Read game material list
        xr_map<u16, shared_str> gameMtls;
        u32 cnt = cacheStream->r_u32();
        for (u32 i = 0; i < cnt; i++)
        {
            u16 idx = cacheStream->r_u16();
            shared_str mtlName;
            cacheStream->r_stringZ(mtlName);
            gameMtls[idx] = mtlName;
        }

        if (remapping_materials_callback)
            remapping_materials_callback(Static.get_tris(), Static.get_tris_count(), gameMtls);

        // Load OPCODE tree
        Static.deserialize_tree(cacheStream);
    }
    else if (use_cache && FS.exist(file_name) && Static.deserialize(file_name, skip_crc32_check, deserialize_callback))
    {
#ifndef MASTER_GOLD
        Msg("* Loaded ObjectSpace cache (%s)...", file_name);
#endif
    }
    else
    {
#ifndef MASTER_GOLD
        Msg("* ObjectSpace cache for '%s' was not loaded. "
            "Building the model from scratch..", file_name);
#endif
        Static.build(verts, H.vertcount, tris, H.facecount, build_callback);

        if (use_cache)
            Static.serialize(file_name, serialize_callback);
    }

    m_BoundingVolume.set(H.aabb);
}

//----------------------------------------------------------------------
#ifdef DEBUG
void CObjectSpace::dbgRender() { (*m_pRender)->dbgRender(); }
/*
void CObjectSpace::dbgRender()
{
    R_ASSERT(bDebug);

    RCache.set_Shader(sh_debug);
    for (u32 i=0; i<q_debug.boxes.size(); i++)
    {
        Fobb&		obb		= q_debug.boxes[i];
        Fmatrix		X,S,R;
        obb.xform_get(X);
        RCache.dbg_DrawOBB(X,obb.m_halfsize,color_xrgb(255,0,0));
        S.scale		(obb.m_halfsize);
        R.mul		(X,S);
        RCache.dbg_DrawEllipse(R,color_xrgb(0,0,255));
    }
    q_debug.boxes.clear();

    for (i=0; i<dbg_S.size(); i++)
    {
        std::pair<Fsphere,u32>& P = dbg_S[i];
        Fsphere&	S = P.first;
        Fmatrix		M;
        M.scale		(S.R,S.R,S.R);
        M.translate_over(S.P);
        RCache.dbg_DrawEllipse(M,P.second);
    }
    dbg_S.clear();
}
*/
#endif
// XXX stats: add to statistics
void CObjectSpace::DumpStatistics(IGameFont& font, IPerformanceAlert* alert) { xrc.DumpStatistics(font, alert); }
