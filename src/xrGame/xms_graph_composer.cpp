#include "StdAfx.h"

// XMS P5: composite game.graph. The base blob from all.spawn chunk 4 is
// decoded into memory, module levels are appended (vertices sampled from the
// level's own level.ai when no hand-made fragment exists), cross-level links
// are stitched, and a fresh blob is re-emitted. Base vertex INDICES and the
// base GUID never change, so existing saves and the base all.spawn stay valid.

#include "xms_game.h"
#include "xrCore/XMS/xms_core.h"
#include "xrAICore/Navigation/game_graph.h"
#include "Common/LevelStructure.hpp"

#include <io.h>
#include <sys/stat.h>

namespace
{
// owns the composed blob (CTempReader is not exported from xrCore)
class XmsBlobReader final : public IReader
{
public:
    XmsBlobReader(void* blob, size_t size) : IReader(blob, size, 0) {}
    ~XmsBlobReader() override
    {
        void* blob = data;
        xr_free(blob);
    }
};
} // namespace

namespace
{
#pragma pack(push, 1)
// mirrors GameGraph::CGameVertex on disk
struct RawVertex
{
    Fvector local;
    Fvector global;
    u32 level_and_node; // level_id:8 | node_id:24
    u8 vertex_types[4];
    u32 edge_offset;  // byte offset from the vertex array base
    u32 point_offset; // byte offset from the vertex array base
    u8 edge_count;
    u8 point_count;
};
struct RawEdge
{
    u16 vertex;
    float distance;
};
struct RawLevelPoint
{
    Fvector position;
    u32 node;
    float distance;
};
struct RawCrossHeader
{
    u32 version;
    u32 node_count;
    u32 game_vertex_count;
    xrGUID level_guid;
    xrGUID game_guid;
};
// level.ai header (hdrNODES8)
struct RawAiHeader
{
    u32 version;
    u32 count;
    float cell_size;
    float factor_y;
    Fbox aabb;
    xrGUID guid;
};
#pragma pack(pop)

static_assert(sizeof(RawVertex) == 42, "game vertex layout drifted");
static_assert(sizeof(RawEdge) == 6, "game edge layout drifted");
static_assert(sizeof(RawLevelPoint) == 20, "level point layout drifted");
static_assert(sizeof(RawAiHeader) == 56, "level.ai header layout drifted");

constexpr float kSampleGrid = 32.f;     // meters between sampled game vertices
constexpr u32 kMaxSamplesPerLevel = 240;
constexpr u32 kNeighbourLinks = 4;

// per-version node layout: size + packed position offsets
struct AiNodeLayout
{
    u32 node_size;
    u32 xz_offset;
    u32 xz_bytes; // 3 (24-bit) or 4
    u32 y_offset;
};

bool ai_layout_for_version(u32 version, AiNodeLayout& out)
{
    switch (version)
    {
    case XRAI_VERSION_SKYLOADER: // 13: NodeCompressed13, 25 bytes
        out = {25, 19, 4, 23};
        return true;
    case XRAI_VERSION_BORSHT_BIG: // 12: 13+2+2+2+6 = 25? layout matches 13 minus link width
        out = {25, 19, 4, 23};
        return true;
    case XRAI_VERSION_CS_COP: // 10: NodeCompressed10, 23 bytes, NodePosition4
        out = {23, 18, 3, 21};
        return true;
    default:
        return false;
    }
}

struct DecodedVertex
{
    Fvector local;
    Fvector global;
    u32 level_and_node;
    u8 vertex_types[4]{};
    xr_vector<RawEdge> edges;
    xr_vector<RawLevelPoint> points;
};

struct AiNodePos
{
    Fvector world;
    u32 id;
};

struct NewLevel
{
    xr_string name;
    xr_string module_id;
    xr_string ai_path;
    u8 id{0};
    Fvector offset{};
    xrGUID guid{};
    u32 first_vertex{0}; // composed index of its first game vertex
    xr_vector<AiNodePos> samples;
    xr_vector<u8> cross_blob; // u32 size-including-prefix + RawCrossHeader + cells
};

bool read_file(pcstr path, xr_vector<u8>& out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(std::max(0l, len)));
    const size_t got = len > 0 ? fread(out.data(), 1, size_t(len), f) : 0;
    fclose(f);
    out.resize(got);
    return !out.empty();
}

// unpacks node positions from a raw level.ai (versions 10/12/13)
bool load_ai_nodes(pcstr path, RawAiHeader& header, xr_vector<AiNodePos>& nodes, u32 stride_hint)
{
    xr_vector<u8> raw;
    if (!read_file(path, raw) || raw.size() < sizeof(RawAiHeader))
        return false;
    memcpy(&header, raw.data(), sizeof(header));
    AiNodeLayout layout;
    if (!ai_layout_for_version(header.version, layout))
    {
        Msg("! XMS: level.ai version %u unsupported: %s", header.version, path);
        return false;
    }
    const size_t need = sizeof(RawAiHeader) + size_t(header.count) * layout.node_size;
    if (raw.size() < need)
        return false;

    const u32 row_length = u32(iFloor((header.aabb.vMax.z - header.aabb.vMin.z) / header.cell_size + EPS_L + 1.5f));
    if (!row_length)
        return false;

    const u8* node = raw.data() + sizeof(RawAiHeader);
    nodes.reserve(header.count / std::max(1u, stride_hint) + 1);
    for (u32 i = 0; i < header.count; ++i, node += layout.node_size)
    {
        if (stride_hint > 1 && (i % stride_hint))
            continue;
        u32 xz = 0;
        u16 y;
        memcpy(&xz, node + layout.xz_offset, layout.xz_bytes); // little-endian: 3-byte packs land right
        memcpy(&y, node + layout.y_offset, sizeof(y));
        AiNodePos pos;
        pos.id = i;
        pos.world.x = header.aabb.vMin.x + float(xz / row_length) * header.cell_size;
        pos.world.z = header.aabb.vMin.z + float(xz % row_length) * header.cell_size;
        pos.world.y = header.aabb.vMin.y + (float(y) / 65535.f) * header.factor_y;
        nodes.emplace_back(pos);
    }
    return true;
}

// thin the node cloud to a sparse, well-spread game-vertex sample set
void sample_vertices(const xr_vector<AiNodePos>& nodes, xr_vector<AiNodePos>& out)
{
    xr_map<std::pair<s32, s32>, AiNodePos> buckets;
    for (const AiNodePos& node : nodes)
    {
        const auto key = std::make_pair(s32(node.world.x / kSampleGrid), s32(node.world.z / kSampleGrid));
        buckets.emplace(key, node); // first node of the bucket wins - deterministic
    }
    for (const auto& [_, node] : buckets)
    {
        out.emplace_back(node);
        if (out.size() >= kMaxSamplesPerLevel)
            break;
    }
}

u16 nearest_index(const xr_vector<AiNodePos>& samples, const Fvector& position)
{
    u16 best = 0;
    float best_distance = flt_max;
    for (u16 i = 0; i < u16(samples.size()); ++i)
    {
        const float distance = samples[i].world.distance_to_sqr(position);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

// dense cross table: every level.ai node -> its nearest sampled game vertex
void build_cross_blob(NewLevel& level, const RawAiHeader& ai, const xr_vector<AiNodePos>& all_nodes,
    const xrGUID& game_guid, u32 total_vertices)
{
    const u32 cells_bytes = ai.count * u32(sizeof(u16) + sizeof(float));
    const u32 payload = sizeof(RawCrossHeader) + cells_bytes;
    const u32 total = sizeof(u32) + payload; // size prefix includes itself
    level.cross_blob.resize(total);
    u8* cursor = level.cross_blob.data();
    *(u32*)cursor = total;
    cursor += sizeof(u32);

    RawCrossHeader header;
    header.version = XRAI_CURRENT_VERSION;
    header.node_count = ai.count;
    header.game_vertex_count = total_vertices;
    header.level_guid = level.guid;
    header.game_guid = game_guid;
    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);

    // all_nodes covers every node (stride 1) in id order
    for (const AiNodePos& node : all_nodes)
    {
        const u16 local = nearest_index(level.samples, node.world);
        const u16 game_id = u16(level.first_vertex + local);
        const float distance = level.samples[local].world.distance_to(node.world);
        memcpy(cursor, &game_id, sizeof(game_id));
        cursor += sizeof(game_id);
        memcpy(cursor, &distance, sizeof(distance));
        cursor += sizeof(distance);
    }
}

struct LinkSpec
{
    xr_string from_level, to_level;
    Fvector from_pos{}, to_pos{};
    bool both{true};
};

bool parse_endpoint(pcstr text, xr_string& level, Fvector& position)
{
    // "level_name @ x, y, z"
    string512 buffer;
    xr_strcpy(buffer, text ? text : "");
    char* at = strchr(buffer, '@');
    if (!at)
        return false;
    *at = 0;
    char* name = buffer;
    while (*name == ' ' || *name == '\t')
        ++name;
    char* end = name + xr_strlen(name);
    while (end > name && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = 0;
    if (!*name)
        return false;
    level = name;
    for (char& c : level)
        c = char(tolower(u8(c)));
    return 3 == sscanf(at + 1, " %f , %f , %f", &position.x, &position.y, &position.z);
}
} // namespace

IReader* XmsGame::ComposeGameGraph(IReader* base_chunk)
{
    if (!XMS::Active() || !base_chunk)
        return base_chunk;

    // ---- collect candidate module levels ------------------------------------
    struct Candidate
    {
        xr_string name, module_id, ai_path;
    };
    xr_vector<Candidate> candidates;
    xr_vector<LinkSpec> links;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (!XMS::ModuleApplies(m))
            continue;
        string_path levels_dir;
        xr_sprintf(levels_dir, "%sgamedata" DELIMITER "levels" DELIMITER, m.root.c_str());
        string_path mask;
        strconcat(mask, levels_dir, "*");
        _finddata_t entry;
        const intptr_t handle = _findfirst(mask, &entry);
        if (handle != -1)
        {
            do
            {
                if (!(entry.attrib & _A_SUBDIR) || 0 == xr_strcmp(entry.name, ".") || 0 == xr_strcmp(entry.name, ".."))
                    continue;
                string_path ai_path;
                xr_sprintf(ai_path, "%s%s" DELIMITER "level.ai", levels_dir, entry.name);
                struct stat file_info;
                if (0 != stat(ai_path, &file_info))
                    continue;
                Candidate candidate;
                candidate.name = entry.name;
                for (char& c : candidate.name)
                    c = char(tolower(u8(c)));
                candidate.module_id = m.id;
                candidate.ai_path = ai_path;
                candidates.emplace_back(std::move(candidate));
            } while (_findnext(handle, &entry) == 0);
            _findclose(handle);
        }

        // cross-level connections declared by the module
        string_path links_path;
        xr_sprintf(links_path, "%sgraph_links.ltx", m.root.c_str());
        xr_vector<XMS::IniRecord> records;
        if (XMS::ReadIniRecords(links_path, records))
        {
            LinkSpec current;
            xr_string current_section;
            const auto flush = [&]()
            {
                if (!current.from_level.empty() && !current.to_level.empty())
                    links.emplace_back(current);
                current = LinkSpec();
            };
            for (const XMS::IniRecord& record : records)
            {
                if (record.section != current_section)
                {
                    flush();
                    current_section = record.section;
                }
                if (record.key == "from")
                    parse_endpoint(record.value.c_str(), current.from_level, current.from_pos);
                else if (record.key == "to")
                    parse_endpoint(record.value.c_str(), current.to_level, current.to_pos);
                else if (record.key == "both")
                    current.both = 0 != xr_strcmp(record.value.c_str(), "false");
            }
            flush();
        }
    }

    if (candidates.empty())
        return base_chunk;

    // ---- decode the base blob ------------------------------------------------
    GameGraph::CHeader base_header;
    base_header.load(base_chunk);

    const u32 base_vertex_count = base_header.vertex_count();
    const u8* vertex_base = static_cast<const u8*>(base_chunk->pointer());
    const RawVertex* base_vertices = reinterpret_cast<const RawVertex*>(vertex_base);

    xr_vector<DecodedVertex> vertices;
    vertices.reserve(base_vertex_count + candidates.size() * kMaxSamplesPerLevel);
    for (u32 i = 0; i < base_vertex_count; ++i)
    {
        const RawVertex& raw = base_vertices[i];
        DecodedVertex vertex;
        vertex.local = raw.local;
        vertex.global = raw.global;
        vertex.level_and_node = raw.level_and_node;
        memcpy(vertex.vertex_types, raw.vertex_types, sizeof(vertex.vertex_types));
        if (raw.edge_count)
        {
            const RawEdge* edges = reinterpret_cast<const RawEdge*>(vertex_base + raw.edge_offset);
            vertex.edges.assign(edges, edges + raw.edge_count);
        }
        if (raw.point_count)
        {
            const RawLevelPoint* points = reinterpret_cast<const RawLevelPoint*>(vertex_base + raw.point_offset);
            vertex.points.assign(points, points + raw.point_count);
        }
        vertices.emplace_back(std::move(vertex));
    }

    // base cross tables, verbatim, in ascending level-id order (= map order)
    const u8* cross_cursor = vertex_base + size_t(base_vertex_count) * sizeof(RawVertex) +
        size_t(base_header.edge_count()) * sizeof(RawEdge) +
        size_t(base_header.death_point_count()) * sizeof(RawLevelPoint);
    xr_vector<std::pair<u8, xr_vector<u8>>> cross_blobs; // level id -> blob
    for (const auto& [level_id, level] : base_header.levels())
    {
        const u32 size = *(const u32*)cross_cursor;
        xr_vector<u8> blob(cross_cursor, cross_cursor + size);
        cross_blobs.emplace_back(u8(level_id), std::move(blob));
        cross_cursor += size;
    }

    // ---- append module levels -------------------------------------------------
    xr_vector<NewLevel> new_levels;
    u32 ordinal = 0;
    for (const Candidate& candidate : candidates)
    {
        if (base_header.level_exist(candidate.name.c_str()))
            continue; // level of the base game - nothing to add
        bool duplicate = false;
        for (const NewLevel& existing : new_levels)
            duplicate |= existing.name == candidate.name;
        if (duplicate)
        {
            Msg("! XMS: level [%s] provided by two modules, first wins", candidate.name.c_str());
            continue;
        }

        RawAiHeader ai;
        xr_vector<AiNodePos> all_nodes;
        if (!load_ai_nodes(candidate.ai_path.c_str(), ai, all_nodes, 1))
        {
            Msg("! XMS: [%s] cannot read level.ai of [%s]", candidate.module_id.c_str(), candidate.name.c_str());
            continue;
        }

        NewLevel level;
        level.name = candidate.name;
        level.module_id = candidate.module_id;
        level.ai_path = candidate.ai_path;
        level.guid = ai.guid;
        if (!XMS::LevelId(candidate.name.c_str(), level.id))
            continue;
        // far offset keeps global points of module levels away from the base map
        level.offset.set(10000.f + 2000.f * float(ordinal), 0.f, 10000.f);
        ++ordinal;

        sample_vertices(all_nodes, level.samples);
        if (level.samples.empty())
        {
            Msg("! XMS: [%s] level [%s] has no usable ai nodes", candidate.module_id.c_str(), candidate.name.c_str());
            continue;
        }
        if (vertices.size() + level.samples.size() > 65534)
        {
            Msg("! XMS: game vertex budget exhausted at level [%s]", candidate.name.c_str());
            break;
        }

        level.first_vertex = u32(vertices.size());
        for (u32 i = 0; i < u32(level.samples.size()); ++i)
        {
            const AiNodePos& sample = level.samples[i];
            DecodedVertex vertex;
            vertex.local = sample.world;
            vertex.global.add(sample.world, level.offset);
            vertex.level_and_node = u32(level.id) | (sample.id << 8);
            // intra-level edges: k nearest samples, both directions
            for (u32 k = 0; k < u32(level.samples.size()); ++k)
            {
                if (k == i)
                    continue;
                vertex.edges.push_back({u16(level.first_vertex + k), sample.world.distance_to(level.samples[k].world)});
            }
            std::sort(vertex.edges.begin(), vertex.edges.end(),
                [](const RawEdge& a, const RawEdge& b) { return a.distance < b.distance; });
            if (vertex.edges.size() > kNeighbourLinks)
                vertex.edges.resize(kNeighbourLinks);
            vertices.emplace_back(std::move(vertex));
        }

        build_cross_blob(level, ai, all_nodes, base_header.guid(),
            u32(vertices.size())); // game_vertex_count snapshot; informational
        new_levels.emplace_back(std::move(level));
    }

    if (new_levels.empty())
    {
        // the header parse above moved the cursor - hand the chunk back rewound
        base_chunk->rewind();
        return base_chunk;
    }

    // ---- stitch cross-level links ----------------------------------------------
    const auto level_id_by_name = [&](const xr_string& name, u8& id) -> bool
    {
        if (const GameGraph::SLevel* base_level = base_header.level(name.c_str(), false))
        {
            id = u8(base_level->id());
            return true;
        }
        for (const NewLevel& level : new_levels)
            if (level.name == name)
            {
                id = level.id;
                return true;
            }
        return false;
    };
    const auto nearest_on_level = [&](u8 level_id, const Fvector& local_pos) -> u32
    {
        u32 best = u32(-1);
        float best_distance = flt_max;
        for (u32 i = 0; i < u32(vertices.size()); ++i)
        {
            if (u8(vertices[i].level_and_node & 0xFF) != level_id)
                continue;
            const float distance = vertices[i].local.distance_to_sqr(local_pos);
            if (distance < best_distance)
            {
                best_distance = distance;
                best = i;
            }
        }
        return best;
    };

    u32 linked = 0;
    for (const LinkSpec& link : links)
    {
        u8 from_id = 0, to_id = 0;
        if (!level_id_by_name(link.from_level, from_id) || !level_id_by_name(link.to_level, to_id))
        {
            Msg("! XMS: graph link skipped, unknown level (%s <-> %s)", link.from_level.c_str(), link.to_level.c_str());
            continue;
        }
        const u32 from_vertex = nearest_on_level(from_id, link.from_pos);
        const u32 to_vertex = nearest_on_level(to_id, link.to_pos);
        if (from_vertex == u32(-1) || to_vertex == u32(-1) || from_vertex == to_vertex)
            continue;
        const float distance = vertices[from_vertex].global.distance_to(vertices[to_vertex].global);
        const auto add_edge = [&](u32 from, u32 to)
        {
            for (const RawEdge& edge : vertices[from].edges)
                if (edge.vertex == u16(to))
                    return;
            if (vertices[from].edges.size() >= 255)
                return;
            vertices[from].edges.push_back({u16(to), distance});
        };
        add_edge(from_vertex, to_vertex);
        if (link.both)
            add_edge(to_vertex, from_vertex);
        ++linked;
    }

    // ---- re-emit the blob ---------------------------------------------------------
    u32 edge_total = 0, point_total = 0;
    for (const DecodedVertex& vertex : vertices)
    {
        edge_total += u32(vertex.edges.size());
        point_total += u32(vertex.points.size());
    }

    CMemoryWriter writer;
    // header
    const u8 version = u8(base_header.version());
    writer.w(&version, sizeof(version));
    const u16 vertex_count = u16(vertices.size());
    writer.w(&vertex_count, sizeof(vertex_count));
    writer.w_u32(edge_total);
    writer.w_u32(point_total);
    xrGUID guid = base_header.guid(); // identity of the base graph is preserved
    writer.w(&guid, sizeof(guid));
    writer.w_u8(u8(base_header.levels().size() + new_levels.size()));
    for (const auto& [level_id, level] : base_header.levels())
    {
        writer.w_stringZ(level.name());
        Fvector offset = level.offset();
        writer.w_fvector3(offset);
        const u8 id = u8(level.id());
        writer.w(&id, sizeof(id));
        writer.w_stringZ(level.section());
        xrGUID level_guid = level.guid();
        writer.w(&level_guid, sizeof(level_guid));
    }
    for (const NewLevel& level : new_levels)
    {
        writer.w_stringZ(level.name.c_str());
        Fvector offset = level.offset;
        writer.w_fvector3(offset);
        writer.w(&level.id, sizeof(level.id));
        writer.w_stringZ("");
        writer.w(&level.guid, sizeof(level.guid));
    }

    // vertices with recomputed byte offsets
    const u32 vertices_bytes = u32(vertices.size()) * sizeof(RawVertex);
    u32 edge_cursor = vertices_bytes;
    u32 point_cursor = vertices_bytes + edge_total * sizeof(RawEdge);
    for (const DecodedVertex& vertex : vertices)
    {
        RawVertex raw;
        raw.local = vertex.local;
        raw.global = vertex.global;
        raw.level_and_node = vertex.level_and_node;
        memcpy(raw.vertex_types, vertex.vertex_types, sizeof(raw.vertex_types));
        raw.edge_offset = edge_cursor;
        raw.point_offset = point_cursor;
        raw.edge_count = u8(vertex.edges.size());
        raw.point_count = u8(vertex.points.size());
        writer.w(&raw, sizeof(raw));
        edge_cursor += u32(vertex.edges.size()) * sizeof(RawEdge);
        point_cursor += u32(vertex.points.size()) * sizeof(RawLevelPoint);
    }
    for (const DecodedVertex& vertex : vertices)
        for (const RawEdge& edge : vertex.edges)
            writer.w(&edge, sizeof(edge));
    for (const DecodedVertex& vertex : vertices)
        for (const RawLevelPoint& point : vertex.points)
            writer.w(&point, sizeof(point));

    // cross tables: base verbatim, then module levels, ascending level id overall
    for (const auto& [level_id, blob] : cross_blobs)
        writer.w(blob.data(), blob.size());
    for (const NewLevel& level : new_levels)
        writer.w(level.cross_blob.data(), level.cross_blob.size());

    // hand the composed blob out as the registry's graph chunk
    u8* buffer = xr_alloc<u8>(writer.size());
    memcpy(buffer, writer.pointer(), writer.size());
    IReader* composed = xr_new<XmsBlobReader>(buffer, writer.size());
    base_chunk->close();

    for (const NewLevel& level : new_levels)
        Msg("* XMS: level [%s] joined the game graph: id=%u, %zu vertices (module %s)", level.name.c_str(),
            u32(level.id), level.samples.size(), level.module_id.c_str());
    Msg("* XMS: game graph composed: %u vertices (%u base), %u edges, %zu level(s), %u link(s)", vertex_count,
        base_vertex_count, edge_total, base_header.levels().size() + new_levels.size(), linked);
    XMS::WriteReport();
    return composed;
}
