#include "stdafx.h"

#include "xms_core.h"
#include "xrCore/LocatorAPI.h"

#include <io.h>
#include <sys/stat.h>
#include <direct.h>

#include <algorithm>
#include <ranges>
#include <string_view>

#include "xrCommon/xr_unordered_map.h"
#include "xrCommon/xr_string.h"

namespace XMS
{
namespace
{
struct Overlay
{
    xr_string vpath; // registered (virtual) lowercase path
    xr_string phys;  // physical location inside the module
    u32 size{0};
    u32 modif{0};
    u16 layer{0};
};

struct SpawnRangeRec
{
    u32 base{0};
    u32 size{0};
    u32 next{0}; // next free absolute id
};

struct State
{
    bool initialized{false};
    bool active{false};
    bool ltx_merge{false};
    xr_vector<Module> modules;
    xr_vector<Overlay> overlays;
    xr_vector<ConflictRow> conflicts;
    // interned CLocatorAPI file name pointer -> overlay index
    xr_unordered_map<const void*, size_t> redirect;
    // persistent registry
    xr_unordered_map<xr_string, u16> ns_by_id;
    xr_unordered_map<xr_string, SpawnRangeRec> range_by_id;
    xr_unordered_map<xr_string, xr_unordered_map<xr_string, u32>> keys_by_id;
    xr_unordered_map<xr_string, u8> level_id_by_name;
    bool registry_dirty{false};
    string_path registry_path{};
    string_path report_path{};
    xr_string staged_root; // modules\.staged\ with a trailing delimiter, empty until the FS is up
    // ltx parse attribution stack
    xr_vector<u16> ltx_layer_stack;
    u32 composition_hash{0};
    // game modes active for the current session
    xr_vector<xr_string> active_modes;
    xr_string active_modes_csv;
    MaterialResolver material_resolver{nullptr};
};

State& st()
{
    static State state;
    return state;
}

constexpr u32 kSpawnRangeFloor = 32768; // above the vanilla all.spawn id top
constexpr u32 kSpawnIdCeiling = 65000;  // headroom below the 0xFFFF invalid marker
// mirrors the TU-local constant in LocatorAPI.cpp
constexpr size_t kStandardVfs = std::numeric_limits<size_t>::max();

xr_string lower_copy(pcstr text)
{
    xr_string result(text);
    xr_strlwr(result);
    return result;
}

u32 fnv1a(pcstr text, u32 hash = 2166136261u)
{
    while (*text)
    {
        hash ^= u8(*text++);
        hash *= 16777619u;
    }
    return hash;
}

bool valid_id(pcstr id)
{
    if (!id || !id[0])
        return false;
    // "xms." is reserved for the engine's own persistent blobs (xms.save_data/load_data resolve it
    // before any module), so a module taking such an id would alias a core save chunk
    if (0 == strncmp(id, "xms.", 4))
        return false;
    for (pcstr c = id; *c; ++c)
    {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '.' || *c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void split_list(pcstr value, xr_vector<xr_string>& out)
{
    // comma separated, entries trimmed, version constraints after a space are dropped
    string4096 buf;
    xr_strcpy(buf, value);
    for (char* cursor = buf; cursor && *cursor;)
    {
        char* comma = strchr(cursor, ',');
        if (comma)
            *comma = 0;
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        char* end = cursor + xr_strlen(cursor);
        while (end > cursor && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            *--end = 0;
        // strip a version constraint ("mod.id >=1.0" / "mod.id = *")
        if (char* space = strchr(cursor, ' '))
            *space = 0;
        if (*cursor)
            out.emplace_back(cursor);
        cursor = comma ? comma + 1 : nullptr;
    }
}

// ---- tiny standalone ini reader (manifests must not depend on CInifile) ----

struct RawFile
{
    char* data{nullptr};
    size_t size{0};
    ~RawFile() { xr_free(data); }
};

bool read_raw(pcstr path, RawFile& out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0)
    {
        fclose(f);
        return false;
    }
    out.data = xr_alloc<char>(size_t(len) + 1);
    const size_t got = fread(out.data, 1, size_t(len), f);
    fclose(f);
    out.data[got] = 0;
    out.size = got;
    return true;
}

// First ';' that starts a comment. With `quoted` a ';' between double quotes is text: a
// manifest description is prose, and prose has semicolons.
char* find_comment(char* line, bool quoted)
{
    if (!quoted)
        return strchr(line, ';');
    bool inside = false;
    for (char* c = line; *c; ++c)
    {
        if (inside && *c == '\\' && c[1])
            ++c;
        else if (*c == '"')
            inside = !inside;
        else if (*c == ';' && !inside)
            return c;
    }
    return nullptr;
}

// callback(section, key, value). `quoted_values` is for manifests only: every other file this
// reads (.xspawn, registries, overlay lists) keeps the comment rule it shipped with.
template <typename Callback>
void parse_plain_ini(char* text, const Callback& cb, bool quoted_values = false)
{
    string256 section = "";
    for (char* line = text; line && *line;)
    {
        char* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (char* cr = strchr(line, '\r'))
            *cr = 0;
        while (*line == ' ' || *line == '\t')
            ++line;
        if (char* comment = find_comment(line, quoted_values))
            *comment = 0;
        if (line[0] == '[')
        {
            if (char* close = strchr(line, ']'))
            {
                *close = 0;
                xr_strcpy(section, line + 1);
                xr_strlwr(section);
            }
        }
        else if (char* eq = strchr(line, '='))
        {
            *eq = 0;
            char* key = line;
            char* key_end = key + xr_strlen(key);
            while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t'))
                *--key_end = 0;
            char* value = eq + 1;
            while (*value == ' ' || *value == '\t')
                ++value;
            char* val_end = value + xr_strlen(value);
            while (val_end > value && (val_end[-1] == ' ' || val_end[-1] == '\t'))
                *--val_end = 0;
            if (*key)
                cb(section, key, value);
        }
        line = next;
    }
}

// ---- persistent registry ---------------------------------------------------

void load_registry()
{
    State& s = st();
    RawFile raw;
    if (!read_raw(s.registry_path, raw))
        return;
    parse_plain_ini(raw.data, [&s](pcstr section, pcstr key, pcstr value)
    {
        if (0 == xr_strcmp(section, "ns"))
        {
            // a hand-edited value inside the reserved range is dropped and
            // the module simply gets a fresh one
            const u16 ns = u16(atoi(value));
            if (ns < kReservedNsBase)
                s.ns_by_id[key] = ns;
        }
        else if (0 == xr_strcmp(section, "spawn_range"))
        {
            SpawnRangeRec rec;
            if (3 == sscanf(value, "%u,%u,%u", &rec.base, &rec.size, &rec.next))
                s.range_by_id[key] = rec;
        }
        else if (0 == xr_strcmp(section, "level_ids"))
        {
            s.level_id_by_name[key] = u8(atoi(value));
        }
        else if (0 == strncmp(section, "spawn_keys.", 11))
        {
            s.keys_by_id[section + 11][key] = u32(atoi(value));
        }
    });
}

void save_registry()
{
    State& s = st();
    if (!s.registry_dirty || !s.registry_path[0])
        return;
    FILE* f = fopen(s.registry_path, "wb");
    if (!f)
    {
        Msg("! XMS: cannot write registry [%s]", s.registry_path);
        return;
    }
    fprintf(f, "; XMS persistent id registry. Machine-written, do not edit.\n[ns]\n");
    for (const auto& [id, ns] : s.ns_by_id)
        fprintf(f, "%s = %u\n", id.c_str(), u32(ns));
    fprintf(f, "\n[spawn_range]\n");
    for (const auto& [id, rec] : s.range_by_id)
        fprintf(f, "%s = %u,%u,%u\n", id.c_str(), rec.base, rec.size, rec.next);
    fprintf(f, "\n[level_ids]\n");
    for (const auto& [name, id] : s.level_id_by_name)
        fprintf(f, "%s = %u\n", name.c_str(), u32(id));
    for (const auto& [id, keys] : s.keys_by_id)
    {
        fprintf(f, "\n[spawn_keys.%s]\n", id.c_str());
        for (const auto& [key, value] : keys)
            fprintf(f, "%s = %u\n", key.c_str(), value);
    }
    fclose(f);
    s.registry_dirty = false;
}

// 0 when the id space below kReservedNsBase is used up
u16 acquire_ns(const xr_string& id)
{
    State& s = st();
    if (const auto it = s.ns_by_id.find(id); it != s.ns_by_id.end())
        return it->second;
    u32 candidate = 1;
    for (const auto& [_, ns] : s.ns_by_id)
        candidate = std::max<u32>(candidate, u32(ns) + 1);
    if (candidate >= kReservedNsBase)
        return 0;
    s.ns_by_id[id] = u16(candidate);
    s.registry_dirty = true;
    return u16(candidate);
}

// ---- discovery -------------------------------------------------------------

// Display text of a manifest: "..." is unwrapped and \n \" \\ are resolved inside it. A value
// without the quotes is taken as written, so a name like C:\new stays what the author typed.
xr_string display_text(pcstr value)
{
    const size_t length = xr_strlen(value);
    if (length < 2 || value[0] != '"' || value[length - 1] != '"')
        return value;
    xr_string out;
    out.reserve(length);
    for (size_t i = 1; i + 1 < length; ++i)
    {
        if (value[i] == '\\' && i + 2 < length)
        {
            const char next = value[i + 1];
            if (next == 'n' || next == '"' || next == '\\')
            {
                out += next == 'n' ? '\n' : next;
                ++i;
                continue;
            }
        }
        out += value[i];
    }
    return out;
}

void parse_manifest(Module& m, char* text)
{
    // a BOM in front of "[module]" would hide the section header
    if (u8(text[0]) == 0xEF && u8(text[1]) == 0xBB && u8(text[2]) == 0xBF)
        text += 3;

    xr_string website, github;
    parse_plain_ini(text, [&](pcstr section, pcstr key, pcstr value)
    {
        if (0 == xr_strcmp(section, "module"))
        {
            if (0 == xr_strcmp(key, "id"))
                m.id = lower_copy(value);
            else if (0 == xr_strcmp(key, "name"))
                m.name = display_text(value);
            else if (0 == xr_strcmp(key, "version"))
                m.version = value;
            else if (0 == xr_strcmp(key, "mode"))
                m.mode = lower_copy(value);
            else if (0 == xr_strcmp(key, "author"))
                m.author = display_text(value);
            else if (0 == xr_strcmp(key, "description"))
                m.description = display_text(value);
            else if (0 == xr_strcmp(key, "website"))
                website = value;
        }
        else if (0 == xr_strcmp(section, "update"))
        {
            if (0 == xr_strcmp(key, "github"))
                github = value;
        }
        else if (0 == xr_strcmp(section, "provides_mode"))
        {
            // pairs of id/title lines; id starts a new entry
            if (0 == xr_strcmp(key, "id"))
                m.provides_modes.push_back({lower_copy(value), xr_string()});
            else if (0 == xr_strcmp(key, "title") && !m.provides_modes.empty())
                m.provides_modes.back().title = value;
        }
        else if (0 == xr_strcmp(section, "requires"))
            m.requires_ids.emplace_back(lower_copy(key));
        else if (0 == xr_strcmp(section, "conflicts"))
            m.conflict_ids.emplace_back(lower_copy(key));
        else if (0 == xr_strcmp(section, "order"))
        {
            if (0 == xr_strcmp(key, "after"))
                split_list(value, m.after_ids);
            else if (0 == xr_strcmp(key, "before"))
                split_list(value, m.before_ids);
        }
        else if (0 == xr_strcmp(section, "budget"))
        {
            if (0 == xr_strcmp(key, "spawns"))
                m.budget_spawns = u32(atoi(value));
        }
        else if (0 == xr_strcmp(section, "vfs"))
            m.vfs_maps.push_back({lower_copy(key), value});
        else if (0 == xr_strcmp(section, "redirects"))
            m.redirect_maps.push_back({lower_copy(key), lower_copy(value)});
    }, true);

    // Refused values are dropped, not repaired: the Mods menu then shows a disabled button
    // instead of opening or polling something the manifest had no right to name.
    if (ValidWebsite(website.c_str()))
        m.website = std::move(website);
    else if (!website.empty())
        Msg("! XMS: module [%s] website ignored - only https://ap-pro.ru and https://moddb.com are allowed",
            m.id.c_str());
    if (ValidGithubRepo(github.c_str()))
        m.update_github = std::move(github);
    else if (!github.empty())
        Msg("! XMS: module [%s] update source ignored - [update] github must be owner/repo", m.id.c_str());
}

// Manifest paths arrive in whatever style the author typed. Virtual sides are
// lowercased by the callers above (the VFS is case-folded); here both sides
// get one separator style and are refused any attempt to climb out of their
// root - a manifest must not be able to name a file outside the module or
// outside gamedata.
bool normalize_rel_path(xr_string& p)
{
    xr_string out;
    out.reserve(p.size());
    for (char c : p)
        out += (c == '/') ? '\\' : c;
    while (!out.empty() && out.front() == '\\')
        out.erase(out.begin());
    while (!out.empty() && (out.back() == '\\' || out.back() == ' '))
        out.pop_back();
    if (out.empty() || out.find("..") != xr_string::npos)
        return false;
    p = std::move(out);
    return true;
}

// One id per line, same shape as order.ltx. Used for the ids the player has
// switched off and for nothing else.
void read_id_list(pcstr path, xr_vector<xr_string>& out)
{
    RawFile raw;
    if (!read_raw(path, raw))
        return;
    for (char* line = raw.data; line && *line;)
    {
        char* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (char* cr = strchr(line, '\r'))
            *cr = 0;
        while (*line == ' ' || *line == '\t')
            ++line;
        if (char* comment = strchr(line, ';'))
            *comment = 0;
        for (char* end = line + xr_strlen(line); end > line && (end[-1] == ' ' || end[-1] == '\t');)
            *--end = 0;
        if (*line)
            out.emplace_back(lower_copy(line));
        line = next;
    }
}

// legacy=true marks the folder XMS shares with JSGME: a module found there is
// visible to it, and letting JSGME "activate" a module copies it into the game
// tree - the one thing a module must never do.
void discover(pcstr mods_root, bool legacy)
{
    State& s = st();
    string_path mask;
    strconcat(mask, mods_root, "*");
    _finddata_t entry;
    const intptr_t handle = _findfirst(mask, &entry);
    if (handle == -1)
        return;
    do
    {
        if (!(entry.attrib & _A_SUBDIR) || 0 == xr_strcmp(entry.name, ".") || 0 == xr_strcmp(entry.name, ".."))
            continue;
        string_path manifest;
        strconcat(manifest, mods_root, entry.name, DELIMITER "mod.ltx");
        RawFile raw;
        if (!read_raw(manifest, raw))
            continue;
        Module m;
        parse_manifest(m, raw.data);
        m.root = xr_string(mods_root) + entry.name + DELIMITER;
        m.legacy_root = legacy;
        if (!valid_id(m.id.c_str()))
        {
            m.id = lower_copy(entry.name);
            m.disabled = true;
            m.disable_reason = "invalid or missing [module] id in mod.ltx";
        }
        if (m.name.empty())
            m.name = m.id;

        // the same id in both roots: the module's own folder wins, the copy in
        // the shared one is ignored rather than silently loaded twice
        bool duplicate = false;
        for (const Module& seen : s.modules)
            if (seen.id == m.id)
            {
                duplicate = true;
                Msg("! XMS: module [%s] found in both roots, using %s", m.id.c_str(), seen.root.c_str());
                break;
            }
        if (duplicate)
            continue;

        if (legacy)
            Msg("~ XMS: module [%s] sits in the folder JSGME manages. It works, but JSGME lists it as "
                "one of its own - activating it there copies it into the game tree. Move it to modules%s",
                m.id.c_str(), DELIMITER);

        s.modules.emplace_back(std::move(m));
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
}

// ---- ordering --------------------------------------------------------------

void order_modules(pcstr mods_root, pcstr legacy_root)
{
    State& s = st();
    auto& mods = s.modules;
    const size_t count = mods.size();
    if (!count)
        return;

    // user override order: <root>/order.ltx, one id per line, top loads first.
    // Both roots are read - an install that still keeps its modules in the
    // JSGME folder keeps the order file it already has.
    xr_unordered_map<xr_string, u32> user_rank;
    {
        xr_vector<xr_string> ranked;
        string_path order_path;
        strconcat(order_path, mods_root, "order.ltx");
        read_id_list(order_path, ranked);
        strconcat(order_path, legacy_root, "order.ltx");
        read_id_list(order_path, ranked);
        u32 rank = 0;
        for (const xr_string& id : ranked)
            user_rank.emplace(id, rank++);
    }

    xr_unordered_map<xr_string, size_t> index;
    for (size_t i = 0; i < count; ++i)
        index.emplace(mods[i].id, i);

    // requirements and declared conflicts
    for (Module& m : mods)
    {
        if (m.disabled)
            continue;
        for (const xr_string& req : m.requires_ids)
        {
            const auto it = index.find(req);
            if (it == index.end() || mods[it->second].disabled)
            {
                m.disabled = true;
                m.disable_reason = xr_string("missing required module: ") + req;
                break;
            }
        }
        if (m.disabled)
            continue;
        for (const xr_string& bad : m.conflict_ids)
        {
            const auto it = index.find(bad);
            if (it != index.end() && !mods[it->second].disabled)
            {
                m.disabled = true;
                m.disable_reason = xr_string("declares conflict with installed module: ") + bad;
                break;
            }
        }
    }

    // dependency edges: dep -> dependant
    xr_vector<xr_vector<size_t>> out_edges(count);
    xr_vector<u32> in_degree(count, 0);
    const auto add_edge = [&](size_t from, size_t to)
    {
        out_edges[from].push_back(to);
        ++in_degree[to];
    };
    for (size_t i = 0; i < count; ++i)
    {
        if (mods[i].disabled)
            continue;
        const auto link = [&](const xr_vector<xr_string>& ids, bool forward)
        {
            for (const xr_string& other : ids)
            {
                const auto it = index.find(other);
                if (it == index.end() || mods[it->second].disabled)
                    continue;
                forward ? add_edge(it->second, i) : add_edge(i, it->second);
            }
        };
        link(mods[i].requires_ids, true);
        link(mods[i].after_ids, true);
        link(mods[i].before_ids, false);
    }

    const auto rank_of = [&](size_t i) -> u32
    {
        const auto it = user_rank.find(mods[i].id);
        return it != user_rank.end() ? it->second : u32(-1);
    };

    // Kahn with deterministic tie-break: user rank, then id
    xr_vector<size_t> ready, sorted;
    for (size_t i = 0; i < count; ++i)
        if (!mods[i].disabled && !in_degree[i])
            ready.push_back(i);
    while (!ready.empty())
    {
        size_t best = 0;
        for (size_t k = 1; k < ready.size(); ++k)
        {
            const size_t a = ready[k], b = ready[best];
            if (rank_of(a) < rank_of(b) || (rank_of(a) == rank_of(b) && mods[a].id < mods[b].id))
                best = k;
        }
        const size_t current = ready[best];
        ready.erase(ready.begin() + best);
        sorted.push_back(current);
        for (const size_t next : out_edges[current])
            if (!mods[next].disabled && 0 == --in_degree[next])
                ready.push_back(next);
    }

    // whatever did not sort is part of a dependency cycle
    for (size_t i = 0; i < count; ++i)
    {
        if (mods[i].disabled)
            continue;
        if (std::find(sorted.begin(), sorted.end(), i) == sorted.end())
        {
            mods[i].disabled = true;
            mods[i].disable_reason = "dependency cycle";
        }
    }

    xr_vector<Module> final_order;
    final_order.reserve(count);
    for (const size_t i : sorted)
        final_order.emplace_back(std::move(mods[i]));
    for (Module& m : mods)
        if (!m.id.empty() && m.disabled)
            final_order.emplace_back(std::move(m));
    mods = std::move(final_order);

    u16 layer = 0;
    u32 hash = 2166136261u;
    for (Module& m : mods)
    {
        if (m.disabled)
            continue;
        const u16 ns = acquire_ns(m.id);
        if (!ns)
        {
            m.disabled = true;
            m.disable_reason = "namespace registry exhausted";
            continue;
        }
        m.layer = ++layer;
        m.ns = ns;
        hash = fnv1a(m.id.c_str(), hash);
        hash = fnv1a(m.version.c_str(), hash);
    }
    s.composition_hash = hash;
}

// ---- mounting --------------------------------------------------------------

void mount_entry(const Module& m, pcstr phys, pcstr virt, u32 size, u32 modif)
{
    State& s = st();
    Overlay overlay;
    overlay.vpath = lower_copy(virt);
    overlay.phys = phys;
    overlay.size = size;
    overlay.modif = modif;
    overlay.layer = m.layer;

    // ledger: someone already owns this virtual path
    if (const CLocatorAPI::file* prev = FS.GetFileDesc(overlay.vpath.c_str()))
    {
        if (prev->size_real || prev->vfs != kStandardVfs) // skip folder placeholders
        {
            pcstr prev_phys = ResolvePhysical(prev->name);
            xr_string loser = prev_phys ? prev_phys : prev->name;
            if (!prev_phys && prev->vfs != kStandardVfs)
                loser = xr_string("archive entry: ") + prev->name;
            AddConflict(ConflictKind::File, overlay.vpath.c_str(), loser.c_str(), overlay.phys.c_str());
        }
    }

    const CLocatorAPI::file* registered =
        FS.xms_register(overlay.vpath.c_str(), overlay.size, overlay.modif);
    if (!registered)
        return;
    s.overlays.emplace_back(std::move(overlay));
    s.redirect[registered->name] = s.overlays.size() - 1;
}

void mount_tree(const Module& m, pcstr phys_dir, pcstr virt_dir)
{
    string_path mask;
    strconcat(mask, phys_dir, "*");
    _finddata_t entry;
    const intptr_t handle = _findfirst(mask, &entry);
    if (handle == -1)
        return;
    do
    {
        if (0 == xr_strcmp(entry.name, ".") || 0 == xr_strcmp(entry.name, ".."))
            continue;
        string_path phys, virt;
        strconcat(phys, phys_dir, entry.name);
        strconcat(virt, virt_dir, entry.name);
        if (entry.attrib & _A_SUBDIR)
        {
            xr_strcat(phys, DELIMITER);
            xr_strcat(virt, DELIMITER);
            mount_tree(m, phys, virt);
            continue;
        }
        mount_entry(m, phys, virt, u32(entry.size), u32(entry.time_write));
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
}

// [vfs]: a module is free to keep any folder layout it likes and publish it
// at virtual paths through its manifest. Mounted AFTER the module's gamedata/
// mirror, so an explicit mapping wins over the module's own mirror; between
// modules the usual load order applies.
void mount_vfs_maps(const Module& m, pcstr gamedata_root)
{
    for (const PathMap& map : m.vfs_maps)
    {
        xr_string virt_rel = map.from;
        xr_string phys_rel = map.to;
        if (!normalize_rel_path(virt_rel) || !normalize_rel_path(phys_rel))
        {
            Msg("! XMS: [%s] bad [vfs] entry '%s = %s', skipped", m.id.c_str(), map.from.c_str(), map.to.c_str());
            continue;
        }

        string_path phys, virt;
        strconcat(phys, m.root.c_str(), phys_rel.c_str());
        strconcat(virt, gamedata_root, virt_rel.c_str());

        // a folder mapping mounts the tree under the virtual prefix, a file
        // mapping mounts that one file; the probe doubles as the existence check
        _finddata_t entry;
        const intptr_t handle = _findfirst(phys, &entry);
        if (handle == -1)
        {
            Msg("! XMS: [%s] [vfs] source '%s' does not exist, skipped", m.id.c_str(), phys_rel.c_str());
            continue;
        }
        const bool folder = 0 != (entry.attrib & _A_SUBDIR);
        const u32 size = u32(entry.size);
        const u32 modif = u32(entry.time_write);
        _findclose(handle);

        if (folder)
        {
            xr_strcat(phys, DELIMITER);
            xr_strcat(virt, DELIMITER);
            mount_tree(m, phys, virt);
        }
        else
            mount_entry(m, phys, virt, size, modif);
    }
}

// [redirects]: the retired name becomes one more overlay over the SAME
// physical file its current name resolves to. Works for renames inside the
// module and for pointing an old path at another module's (or loose base)
// file; archive-backed targets cannot be redirected to - a standard VFS entry
// cannot read out of an archive.
void mount_redirects(const Module& m, pcstr gamedata_root)
{
    for (const PathMap& map : m.redirect_maps)
    {
        xr_string old_rel = map.from;
        xr_string new_rel = map.to;
        if (!normalize_rel_path(old_rel) || !normalize_rel_path(new_rel))
        {
            Msg("! XMS: [%s] bad [redirects] entry '%s = %s', skipped", m.id.c_str(), map.from.c_str(),
                map.to.c_str());
            continue;
        }

        string_path old_virt, new_virt;
        strconcat(old_virt, gamedata_root, old_rel.c_str());
        strconcat(new_virt, gamedata_root, new_rel.c_str());

        const CLocatorAPI::file* target = FS.GetFileDesc(lower_copy(new_virt).c_str());
        if (!target)
        {
            Msg("! XMS: [%s] redirect target '%s' not found, '%s' skipped", m.id.c_str(), new_rel.c_str(),
                old_rel.c_str());
            continue;
        }
        pcstr phys = ResolvePhysical(target->name);
        if (!phys && target->vfs == kStandardVfs)
            phys = target->name; // loose base file: its registered name IS the physical path
        if (!phys)
        {
            Msg("! XMS: [%s] redirect target '%s' lives in an archive, '%s' skipped", m.id.c_str(),
                new_rel.c_str(), old_rel.c_str());
            continue;
        }
        mount_entry(m, phys, old_virt, target->size_real, target->modif);
    }
}

void mount_all()
{
    State& s = st();
    FS_Path* gamedata = nullptr;
    if (!FS.get_path("$game_data$", &gamedata))
        return;
    for (const Module& m : s.modules)
    {
        if (m.disabled)
            continue;
        string_path overlay_root;
        strconcat(overlay_root, m.root.c_str(), "gamedata" DELIMITER);
        mount_tree(m, overlay_root, gamedata->m_Path);
        mount_vfs_maps(m, gamedata->m_Path);
    }
    // a second pass, so a redirect may point at content of ANY module
    // regardless of load order, not only at earlier layers
    for (const Module& m : s.modules)
    {
        if (m.disabled)
            continue;
        mount_redirects(m, gamedata->m_Path);
    }
}

// ---- staged module updates -------------------------------------------------
// Layout and rules: docs/dead-air/MOD_UPDATES.md 3.2-3.3.

constexpr pcstr kStagedDir = ".staged";
constexpr pcstr kStagedReady = "ready.ltx";
constexpr pcstr kStagedPayload = "payload";
constexpr pcstr kStagedPrevious = "previous";

struct StagedInfo
{
    xr_string id;
    xr_string version;
    xr_string root;   // "modules" or "mods"
    xr_string folder; // the installed module folder inside that root
};

bool is_directory(const xr_string& path)
{
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool valid_folder_name(const xr_string& name)
{
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
        return false;
    return name.find_first_of("\\/:*?\"<>|") == xr_string::npos;
}

// Never walks through a link: a module folder that is a junction into an author's working
// copy loses the junction, not the working copy.
void remove_tree(const xr_string& path)
{
    const DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        WIN32_FIND_DATAA data;
        const HANDLE find = FindFirstFileA((path + DELIMITER "*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (0 == xr_strcmp(data.cFileName, ".") || 0 == xr_strcmp(data.cFileName, ".."))
                    continue;
                const xr_string child = path + DELIMITER + data.cFileName;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesA(child.c_str(), data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    remove_tree(child);
                else
                    DeleteFileA(child.c_str());
            } while (FindNextFileA(find, &data));
            FindClose(find);
        }
    }
    RemoveDirectoryA(path.c_str());
}

bool read_staged_info(const xr_string& ready_path, StagedInfo& info)
{
    RawFile raw;
    if (!read_raw(ready_path.c_str(), raw))
        return false;
    parse_plain_ini(raw.data, [&info](pcstr section, pcstr key, pcstr value)
    {
        if (0 != xr_strcmp(section, "staged"))
            return;
        if (0 == xr_strcmp(key, "id"))
            info.id = lower_copy(value);
        else if (0 == xr_strcmp(key, "version"))
            info.version = value;
        else if (0 == xr_strcmp(key, "root"))
            info.root = lower_copy(value);
        else if (0 == xr_strcmp(key, "folder"))
            info.folder = value;
    });
    return valid_id(info.id.c_str()) && (info.root == "modules" || info.root == "mods") &&
        valid_folder_name(info.folder) && info.folder != kStagedDir;
}

bool manifest_id_is(const xr_string& module_dir, const xr_string& id)
{
    RawFile raw;
    if (!read_raw((module_dir + DELIMITER "mod.ltx").c_str(), raw))
        return false;
    Module probe;
    parse_manifest(probe, raw.data);
    return probe.id == id;
}

// The Mods menu relaunches the game while the old process is still shutting down, and a
// folder cannot be renamed while that process has a file open in it.
void wait_for_relaunch_parent()
{
    char value[32]{};
    const DWORD length = GetEnvironmentVariableA("DAR_RELAUNCH_WAIT_PID", value, sizeof(value));
    if (!length || length >= sizeof(value))
        return;
    SetEnvironmentVariableA("DAR_RELAUNCH_WAIT_PID", nullptr);
    const DWORD pid = strtoul(value, nullptr, 10);
    if (!pid || pid == GetCurrentProcessId())
        return;
    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid))
    {
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
    }
}

// Two renames with the old folder parked in between, so every point of failure - including a
// power cut between them - leaves either the old module or the new one, never neither.
void apply_staged(pcstr game_root, const xr_string& dir, const StagedInfo& info)
{
    const xr_string target = xr_string(game_root) + info.root + DELIMITER + info.folder;
    const xr_string payload = dir + kStagedPayload;
    const xr_string previous = dir + kStagedPrevious;
    const bool has_target = is_directory(target);
    const bool has_previous = is_directory(previous);

    if (!is_directory(payload) || !manifest_id_is(payload, info.id))
    {
        if (!has_target && has_previous)
            MoveFileExA(previous.c_str(), target.c_str(), 0);
        Msg("! XMS: staged update of [%s] holds no such module - discarded", info.id.c_str());
        remove_tree(dir);
        return;
    }
    if (has_target ? !manifest_id_is(target, info.id) : !has_previous)
    {
        Msg("~ XMS: module [%s] is no longer installed - staged update %s discarded", info.id.c_str(),
            info.version.c_str());
        remove_tree(dir);
        return;
    }

    if (has_target)
    {
        remove_tree(previous);
        if (!MoveFileExA(target.c_str(), previous.c_str(), 0))
        {
            Msg("! XMS: module [%s] is in use (error %u) - update %s stays staged", info.id.c_str(),
                GetLastError(), info.version.c_str());
            return;
        }
    }
    if (!MoveFileExA(payload.c_str(), target.c_str(), 0))
    {
        const DWORD error = GetLastError();
        MoveFileExA(previous.c_str(), target.c_str(), 0);
        Msg("! XMS: module [%s] update %s could not be moved in (error %u) - previous version kept",
            info.id.c_str(), info.version.c_str(), error);
        return;
    }

    DeleteFileA((dir + kStagedReady).c_str());
    remove_tree(dir);
    Msg("* XMS: module [%s] updated to %s", info.id.c_str(), info.version.c_str());
}

// Runs before discovery: the one moment no module file is open by this process.
void apply_staged_updates(pcstr game_root)
{
    const xr_string& root = st().staged_root;
    xr_vector<xr_string> ids;
    {
        _finddata_t entry;
        const intptr_t handle = _findfirst((root + "*").c_str(), &entry);
        if (handle == -1)
            return;
        do
        {
            if ((entry.attrib & _A_SUBDIR) && valid_folder_name(entry.name) && valid_id(entry.name))
                ids.emplace_back(entry.name);
        } while (_findnext(handle, &entry) == 0);
        _findclose(handle);
    }

    bool waited = false;
    for (const xr_string& id : ids)
    {
        const xr_string dir = root + id + DELIMITER;
        StagedInfo info;
        // no ready.ltx = an interrupted download; the Mods menu clears it, nothing is applied
        if (!read_staged_info(dir + kStagedReady, info) || info.id != id)
            continue;
        if (!waited)
        {
            waited = true;
            wait_for_relaunch_parent();
        }
        apply_staged(game_root, dir, info);
    }
    // fails while anything is left inside, which is the point
    RemoveDirectoryA(root.substr(0, root.size() - 1).c_str());
}
} // namespace

// ---- public API ------------------------------------------------------------

void InitializeAndMount()
{
    State& s = st();
    if (s.initialized)
        return;
    s.initialized = true;

    FS_Path* fs_root = nullptr;
    if (!FS.get_path("$fs_root$", &fs_root) || !FS.path_exist("$game_data$") ||
        !FS.path_exist("$app_data_root$"))
        return;

    // Modules live in their OWN root. mods\ is the folder JSGME manages (it is
    // the same directory as MODS\ - Windows does not care about the case), and
    // a module there is a module JSGME offers to copy into the game tree, which
    // is exactly the merge a module exists to avoid. Still read, because
    // modules already installed there have to keep working.
    string_path mods_root, legacy_root;
    strconcat(mods_root, fs_root->m_Path, "modules" DELIMITER);
    strconcat(legacy_root, fs_root->m_Path, "mods" DELIMITER);

    FS.update_path(s.registry_path, "$app_data_root$", "xms_registry.ltx");
    FS.update_path(s.report_path, "$app_data_root$", "xms_report.json");

    // A module JSGME has already copied over the game leaves its manifest in
    // the root. Nothing here can undo that - say so, loudly. The check is
    // physical on purpose: FS.exist asks the virtual namespace, which does not
    // know about the game root while it is still coming up.
    {
        string_path stray;
        strconcat(stray, fs_root->m_Path, "mod.ltx");
        RawFile raw;
        if (read_raw(stray, raw))
            Msg("! XMS: mod.ltx in the game root - a module was installed with JSGME. Deactivate it there: "
                "its files are now merged into the game and are being applied twice");
    }

    s.staged_root = xr_string(mods_root) + kStagedDir + DELIMITER;
    apply_staged_updates(fs_root->m_Path);

    discover(mods_root, false);
    discover(legacy_root, true);
    if (s.modules.empty())
        return;

    // The player's own on/off list, one id per line. Kept next to the modules
    // rather than in appdata so it travels with the install, exactly like
    // order.ltx does - and in ONE place, whichever root a module happens to sit
    // in, so moving a module never leaves a stale switch behind.
    {
        xr_vector<xr_string> off;
        string_path off_path;
        strconcat(off_path, mods_root, "disabled.ltx");
        read_id_list(off_path, off);
        for (Module& m : s.modules)
            for (const xr_string& id : off)
                if (m.id == id)
                {
                    m.disabled = true;
                    m.disable_reason = "switched off in disabled.ltx";
                    break;
                }
    }

    load_registry();
    order_modules(mods_root, legacy_root);
    mount_all();
    save_registry();

    u32 enabled = 0;
    for (const Module& m : s.modules)
    {
        if (m.disabled)
            Msg("! XMS: module [%s] disabled: %s", m.id.c_str(), m.disable_reason.c_str());
        else
        {
            Msg("* XMS: [%u] %s %s (ns=%u, %s)", u32(m.layer), m.id.c_str(), m.version.c_str(), u32(m.ns),
                m.root.c_str());
            ++enabled;
        }
    }
    s.active = enabled != 0;
    Msg("* XMS: %u module(s) enabled, %zu overlay file(s), %zu conflict(s)", enabled, s.overlays.size(),
        s.conflicts.size());
    WriteReport();
}

bool Active() { return st().active; }
const xr_vector<Module>& Modules() { return st().modules; }

const Module* FindModule(pcstr id)
{
    for (const Module& m : st().modules)
        if (0 == xr_strcmp(m.id.c_str(), id))
            return &m;
    return nullptr;
}

u32 CompositionHash() { return st().composition_hash; }

bool ValidWebsite(pcstr url)
{
    constexpr std::string_view scheme = "https://";
    constexpr std::string_view hosts[] = {"ap-pro.ru", "www.ap-pro.ru", "moddb.com", "www.moddb.com"};
    const std::string_view text = url ? std::string_view(url) : std::string_view();
    if (text.size() <= scheme.size() || text.size() > 512 || text.substr(0, scheme.size()) != scheme)
        return false;

    // The host is everything up to the first '/', compared whole: a prefix test would pass
    // ap-pro.ru.evil.example and ap-pro.ru@evil.example.
    const std::string_view rest = text.substr(scheme.size());
    const size_t slash = rest.find('/');
    xr_string host(rest.substr(0, slash));
    xr_strlwr(host);
    if (std::ranges::find(hosts, std::string_view(host)) == std::end(hosts))
        return false;

    const std::string_view path = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
    return std::ranges::all_of(path, [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            std::string_view("-._~/?#&=%+").find(c) != std::string_view::npos;
    });
}

bool ValidGithubRepo(pcstr repo)
{
    const std::string_view text = repo ? std::string_view(repo) : std::string_view();
    const size_t slash = text.find('/');
    if (slash == std::string_view::npos)
        return false;
    const std::string_view owner = text.substr(0, slash);
    const std::string_view name = text.substr(slash + 1);
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (owner.empty() || owner.size() > 39 || name.empty() || name.size() > 100 || name == "." || name == "..")
        return false;
    return std::ranges::all_of(owner, [&](char c) { return alnum(c) || c == '-'; }) &&
        std::ranges::all_of(name, [&](char c) { return alnum(c) || c == '-' || c == '.' || c == '_'; });
}

xr_string StagedRoot() { return st().staged_root; }

bool MarkStagedReady(pcstr module_id, pcstr version, xr_string& err)
{
    const Module* m = FindModule(module_id);
    if (!m || st().staged_root.empty())
    {
        err = "no such module";
        return false;
    }
    // root is "<game>\<modules|mods>\<folder>\": the folder name is what the swap needs
    xr_string folder = m->root.substr(0, m->root.size() - 1);
    folder.erase(0, folder.find_last_of("\\/") + 1);
    if (!valid_folder_name(folder))
    {
        err = "unsupported module folder name";
        return false;
    }

    const xr_string path = st().staged_root + m->id + DELIMITER + kStagedReady;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
    {
        err = "cannot write ready.ltx";
        return false;
    }
    fprintf(f, "; XMS staged module update. Machine-written, applied by the next start.\n[staged]\n");
    fprintf(f, "id = %s\nversion = %s\nroot = %s\nfolder = %s\n", m->id.c_str(), version,
        m->legacy_root ? "mods" : "modules", folder.c_str());
    const bool ok = 0 == ferror(f);
    fclose(f);
    if (!ok)
    {
        DeleteFileA(path.c_str());
        err = "cannot write ready.ltx";
    }
    return ok;
}

void DiscardStaged(pcstr module_id)
{
    if (!st().staged_root.empty() && valid_id(module_id) && valid_folder_name(module_id))
        remove_tree(st().staged_root + module_id);
}

void DiscardUnarmedStaged()
{
    const xr_string& root = st().staged_root;
    if (root.empty())
        return;
    xr_vector<xr_string> unarmed;
    _finddata_t entry;
    const intptr_t handle = _findfirst((root + "*").c_str(), &entry);
    if (handle == -1)
        return;
    do
    {
        if (!(entry.attrib & _A_SUBDIR) || !valid_folder_name(entry.name))
            continue;
        const xr_string ready = root + entry.name + DELIMITER + kStagedReady;
        if (GetFileAttributesA(ready.c_str()) == INVALID_FILE_ATTRIBUTES)
            unarmed.emplace_back(entry.name);
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);

    for (const xr_string& name : unarmed)
        remove_tree(root + name);
    RemoveDirectoryA(root.substr(0, root.size() - 1).c_str());
}

bool StagedVersion(pcstr module_id, xr_string& version)
{
    if (st().staged_root.empty() || !valid_id(module_id))
        return false;
    StagedInfo info;
    if (!read_staged_info(st().staged_root + module_id + DELIMITER + kStagedReady, info) || info.id != module_id)
        return false;
    version = info.version;
    return true;
}

pcstr ResolvePhysical(pcstr registered_name)
{
    State& s = st();
    if (s.redirect.empty())
        return nullptr;
    const auto it = s.redirect.find(registered_name);
    return it != s.redirect.end() ? s.overlays[it->second].phys.c_str() : nullptr;
}

void OnRescanPath(pcstr full_path)
{
    State& s = st();
    if (!s.active)
        return;
    const size_t prefix_len = xr_strlen(full_path);
    // drop stale interned keys under the prefix, then re-register so overlays win again
    for (auto it = s.redirect.begin(); it != s.redirect.end();)
    {
        if (0 == strncmp(s.overlays[it->second].vpath.c_str(), full_path, prefix_len))
            it = s.redirect.erase(it);
        else
            ++it;
    }
    for (size_t i = 0; i < s.overlays.size(); ++i)
    {
        Overlay& overlay = s.overlays[i];
        if (0 != strncmp(overlay.vpath.c_str(), full_path, prefix_len))
            continue;
        const CLocatorAPI::file* registered =
            FS.xms_register(overlay.vpath.c_str(), overlay.size, overlay.modif);
        if (registered)
            s.redirect[registered->name] = i;
    }
}

u16 LayerOfPath(pcstr physical_path)
{
    if (!physical_path)
        return 0;
    for (const Module& m : st().modules)
    {
        if (m.disabled)
            continue;
        if (0 == _strnicmp(physical_path, m.root.c_str(), m.root.size()))
            return m.layer;
    }
    return 0;
}

bool SetModuleEnabled(pcstr id, bool enabled, xr_string& err)
{
    err.clear();
    if (!id || !id[0])
    {
        err = "no module id";
        return false;
    }
    const xr_string wanted = lower_copy(id);

    bool known = false;
    for (const Module& m : st().modules)
        known |= (m.id == wanted);
    if (!known)
    {
        err = "no such module (see xms_list)";
        return false;
    }

    // One list for every module, in the modules root - a module that later
    // moves between roots must not leave a switch behind in the old one.
    FS_Path* fs_root = nullptr;
    if (!FS.get_path("$fs_root$", &fs_root))
    {
        err = "no game root";
        return false;
    }
    string_path list_path;
    strconcat(list_path, fs_root->m_Path, "modules" DELIMITER);
    _mkdir(list_path);
    xr_strcat(list_path, sizeof(list_path), "disabled.ltx");

    xr_vector<xr_string> off;
    read_id_list(list_path, off);
    bool changed = false;
    if (enabled)
    {
        for (size_t i = 0; i < off.size();)
        {
            if (off[i] == wanted)
            {
                off.erase(off.begin() + i);
                changed = true;
            }
            else
                ++i;
        }
    }
    else
    {
        bool present = false;
        for (const xr_string& s : off)
            present |= (s == wanted);
        if (!present)
        {
            off.push_back(wanted);
            changed = true;
        }
    }
    if (!changed)
        return true;

    IWriter* w = FS.w_open(list_path);
    if (!w)
    {
        err = xr_string("cannot write ") + list_path;
        return false;
    }
    w->w_string("; XMS: modules switched off by the player, one id per line.");
    w->w_string("; Written by xms_disable / xms_enable. Nothing is copied anywhere -");
    w->w_string("; a module stays where it is and is simply not mounted next start.");
    for (const xr_string& s : off)
        w->w_string(s.c_str());
    FS.w_close(w);
    return true;
}

void AddConflict(ConflictKind kind, pcstr subject, pcstr loser, pcstr winner)
{
    ConflictRow row;
    row.kind = kind;
    row.subject = subject ? subject : "";
    row.loser = loser ? loser : "";
    row.winner = winner ? winner : "";
    st().conflicts.emplace_back(std::move(row));
}

const xr_vector<ConflictRow>& Conflicts() { return st().conflicts; }

void FindOwnedFiles(pcstr needle, xr_vector<OwnedFile>& out, size_t limit)
{
    out.clear();
    if (!needle || !needle[0])
        return;

    xr_string lowered(needle);
    for (char& c : lowered)
        c = char(tolower(u8(c)));

    const State& s = st();
    for (const Overlay& overlay : s.overlays)
    {
        // vpath is already lowercase at registration time
        if (xr_string::npos == overlay.vpath.find(lowered))
            continue;

        OwnedFile& row = out.emplace_back();
        row.vpath = overlay.vpath;
        row.physical = overlay.phys;
        for (const Module& m : s.modules)
            if (m.layer == overlay.layer)
            {
                row.module_id = m.id;
                break;
            }
        if (out.size() >= limit)
            return;
    }
}

namespace
{
void json_escape(FILE* f, pcstr text)
{
    for (pcstr c = text; *c; ++c)
    {
        switch (*c)
        {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': break;
        case '\t': fputs("\\t", f); break;
        default:
            if (u8(*c) >= 0x20)
                fputc(*c, f);
        }
    }
}

pcstr kind_name(ConflictKind kind)
{
    switch (kind)
    {
    case ConflictKind::File: return "file";
    case ConflictKind::LtxSection: return "ltx_section";
    case ConflictKind::LtxKey: return "ltx_key";
    case ConflictKind::XmlPatch: return "xml_patch";
    case ConflictKind::Script: return "script";
    default: return "other";
    }
}
} // namespace

void WriteReport()
{
    State& s = st();
    if (!s.report_path[0])
        return;
    FILE* f = fopen(s.report_path, "wb");
    if (!f)
        return;
    fprintf(f, "{\n  \"composition_hash\": %u,\n  \"modules\": [\n", s.composition_hash);
    for (size_t i = 0; i < s.modules.size(); ++i)
    {
        const Module& m = s.modules[i];
        fprintf(f, "    {\"id\": \"");
        json_escape(f, m.id.c_str());
        fprintf(f, "\", \"version\": \"");
        json_escape(f, m.version.c_str());
        fprintf(f, "\", \"layer\": %u, \"ns\": %u, \"enabled\": %s", u32(m.layer), u32(m.ns),
            m.disabled ? "false" : "true");
        if (m.disabled)
        {
            fprintf(f, ", \"reason\": \"");
            json_escape(f, m.disable_reason.c_str());
            fprintf(f, "\"");
        }
        fprintf(f, "}%s\n", i + 1 < s.modules.size() ? "," : "");
    }
    fprintf(f, "  ],\n  \"conflicts\": [\n");
    for (size_t i = 0; i < s.conflicts.size(); ++i)
    {
        const ConflictRow& row = s.conflicts[i];
        fprintf(f, "    {\"kind\": \"%s\", \"subject\": \"", kind_name(row.kind));
        json_escape(f, row.subject.c_str());
        fprintf(f, "\", \"loser\": \"");
        json_escape(f, row.loser.c_str());
        fprintf(f, "\", \"winner\": \"");
        json_escape(f, row.winner.c_str());
        fprintf(f, "\"}%s\n", i + 1 < s.conflicts.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

bool SpawnRange(pcstr module_id, u32 size, u32& base)
{
    State& s = st();
    if (const auto it = s.range_by_id.find(module_id); it != s.range_by_id.end())
    {
        base = it->second.base;
        return true;
    }
    u32 candidate = kSpawnRangeFloor;
    for (const auto& [_, rec] : s.range_by_id)
        candidate = std::max(candidate, rec.base + rec.size);
    if (candidate + size > kSpawnIdCeiling)
    {
        Msg("! XMS: spawn id space exhausted for [%s] (%u+%u > %u)", module_id, candidate, size, kSpawnIdCeiling);
        return false;
    }
    SpawnRangeRec rec;
    rec.base = candidate;
    rec.size = size;
    rec.next = candidate;
    s.range_by_id[module_id] = rec;
    s.registry_dirty = true;
    save_registry();
    base = candidate;
    return true;
}

bool SpawnLocalId(pcstr module_id, pcstr key, u32& id)
{
    State& s = st();
    auto& keys = s.keys_by_id[module_id];
    if (const auto it = keys.find(key); it != keys.end())
    {
        id = it->second;
        return true;
    }
    const auto range_it = s.range_by_id.find(module_id);
    if (range_it == s.range_by_id.end())
        return false;
    SpawnRangeRec& rec = range_it->second;
    if (rec.next >= rec.base + rec.size)
    {
        Msg("! XMS: spawn budget of [%s] exhausted (%u ids)", module_id, rec.size);
        return false;
    }
    id = rec.next++;
    keys[key] = id;
    s.registry_dirty = true;
    save_registry();
    return true;
}

void AdoptSpawnRange(pcstr module_id, u32 base, u32 size)
{
    State& s = st();
    const auto it = s.range_by_id.find(module_id);
    if (it != s.range_by_id.end())
        return; // local registry wins; it is grow-only anyway
    SpawnRangeRec rec;
    rec.base = base;
    rec.size = size;
    rec.next = base;
    s.range_by_id[module_id] = rec;
    s.registry_dirty = true;
    save_registry();
}

bool LevelId(pcstr level_name, u8& id)
{
    State& s = st();
    const xr_string key = lower_copy(level_name);
    if (const auto it = s.level_id_by_name.find(key); it != s.level_id_by_name.end())
    {
        id = it->second;
        return true;
    }
    // base game ids live well below 128; module levels take 128..254
    u8 candidate = 128;
    for (const auto& [_, taken] : s.level_id_by_name)
        candidate = std::max<u8>(candidate, u8(taken + 1));
    if (candidate >= 255)
    {
        Msg("! XMS: level id space exhausted for [%s]", level_name);
        return false;
    }
    s.level_id_by_name[key] = candidate;
    s.registry_dirty = true;
    save_registry();
    id = candidate;
    return true;
}

void AdoptLevelId(pcstr level_name, u8 id)
{
    State& s = st();
    const xr_string key = lower_copy(level_name);
    if (s.level_id_by_name.find(key) != s.level_id_by_name.end())
        return;
    s.level_id_by_name[key] = id;
    s.registry_dirty = true;
    save_registry();
}

void SetActiveModes(pcstr csv)
{
    State& s = st();
    s.active_modes.clear();
    s.active_modes_csv = csv ? lower_copy(csv) : "";
    split_list(s.active_modes_csv.c_str(), s.active_modes);
    Msg("* XMS: active modes: [%s]", s.active_modes_csv.empty() ? "none" : s.active_modes_csv.c_str());
}

pcstr ActiveModesCsv() { return st().active_modes_csv.c_str(); }

bool ModeActive(pcstr mode_id)
{
    if (!mode_id || !mode_id[0])
        return true;
    const xr_string needle = lower_copy(mode_id);
    for (const xr_string& mode : st().active_modes)
        if (mode == needle)
            return true;
    return false;
}

bool ModuleApplies(const Module& m)
{
    if (m.disabled)
        return false;
    // "*" opts out of gating: the module's level work lands in every game
    if (m.mode == "*")
        return true;
    if (m.mode.empty())
    {
        // a module that BRINGS a mode is implicitly for that mode
        if (!m.provides_modes.empty())
        {
            for (const ProvidedMode& p : m.provides_modes)
                if (ModeActive(p.id.c_str()))
                    return true;
            return false;
        }
        // no mode at all = built for the ORDINARY game; a stock new game has
        // no active modes, and a campaign like Revolution II must not inherit
        // props that were never made for it
        return st().active_modes.empty();
    }
    return ModeActive(m.mode.c_str());
}

void SetMaterialResolver(MaterialResolver resolver) { st().material_resolver = resolver; }

u16 ResolveMaterial(pcstr material_name)
{
    const MaterialResolver resolver = st().material_resolver;
    return resolver ? resolver(material_name) : u16(-1);
}

bool ReadIniRecords(pcstr physical_path, xr_vector<IniRecord>& out)
{
    RawFile raw;
    if (!read_raw(physical_path, raw))
        return false;
    parse_plain_ini(raw.data, [&out](pcstr section, pcstr key, pcstr value)
    {
        IniRecord record;
        record.section = section;
        record.key = key;
        record.value = value;
        out.emplace_back(std::move(record));
    });
    return true;
}

void ListFiles(pcstr dir, pcstr mask, xr_vector<xr_string>& out)
{
    string_path pattern;
    strconcat(pattern, dir, mask);
    _finddata_t entry;
    const intptr_t handle = _findfirst(pattern, &entry);
    if (handle == -1)
        return;
    do
    {
        if (!(entry.attrib & _A_SUBDIR))
            out.emplace_back(xr_string(dir) + entry.name);
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
    std::sort(out.begin(), out.end());
}

namespace
{
// '*' / '?' glob on a file name, case-insensitive. Done here rather than by
// _findfirst so a mask never matches through 8.3 short names.
bool wildcard_match(pcstr mask, pcstr name)
{
    pcstr star_mask = nullptr;
    pcstr star_name = nullptr;
    while (*name)
    {
        if (*mask == '*')
        {
            star_mask = ++mask;
            star_name = name;
            continue;
        }
        if (*mask == '?' || tolower(u8(*mask)) == tolower(u8(*name)))
        {
            ++mask;
            ++name;
            continue;
        }
        if (!star_mask)
            return false;
        mask = star_mask;
        name = ++star_name;
    }
    while (*mask == '*')
        ++mask;
    return !*mask;
}

void list_relative(const xr_string& root, const xr_string& rel, pcstr mask, bool recursive, xr_vector<xr_string>& out)
{
    string_path pattern;
    strconcat(pattern, root.c_str(), rel.c_str(), "*");
    _finddata_t entry;
    const intptr_t handle = _findfirst(pattern, &entry);
    if (handle == -1)
        return;
    do
    {
        if (entry.attrib & _A_SUBDIR)
        {
            if (recursive && 0 != xr_strcmp(entry.name, ".") && 0 != xr_strcmp(entry.name, ".."))
                list_relative(root, rel + entry.name + DELIMITER, mask, recursive, out);
        }
        else if (wildcard_match(mask, entry.name))
            out.emplace_back(rel + entry.name);
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
}
} // namespace

void ListFilesRelative(pcstr dir, pcstr mask, bool recursive, xr_vector<xr_string>& out)
{
    if (!dir || !dir[0] || !mask || !mask[0])
        return;
    list_relative(dir, xr_string(), mask, recursive, out);
    std::sort(out.begin(), out.end());
}

void PushLtxFile(pcstr physical_path) { st().ltx_layer_stack.push_back(LayerOfPath(physical_path)); }

void PopLtxFile()
{
    auto& stack = st().ltx_layer_stack;
    if (!stack.empty())
        stack.pop_back();
}

u16 CurrentLtxLayer()
{
    const auto& stack = st().ltx_layer_stack;
    return stack.empty() ? 0 : stack.back();
}

bool LtxMergeEnabled() { return st().ltx_merge; }
void SetLtxMergeEnabled(bool enabled) { st().ltx_merge = enabled; }
} // namespace XMS
