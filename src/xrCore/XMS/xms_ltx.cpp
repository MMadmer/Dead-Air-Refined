// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_ltx.h"
#include "xms_characters.h"
#include "xms_core.h"
#include "xms_items.h"
#include "xrCommon/xr_unordered_map.h"
#include "xrCore/xr_ini.h"

#include <io.h>

namespace XMS
{
namespace
{
struct PatchStats
{
    u32 files{0};
    u32 ops{0};
    u32 skipped{0};
};

char* trim(char* text)
{
    while (*text == ' ' || *text == '\t')
        ++text;
    char* end = text + xr_strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = 0;
    return text;
}

// comma list helpers for +key / -key ops
void list_to_items(pcstr value, xr_vector<xr_string>& out)
{
    string4096 buf;
    xr_strcpy(buf, value ? value : "");
    for (char* cursor = buf; cursor && *cursor;)
    {
        char* comma = strchr(cursor, ',');
        if (comma)
            *comma = 0;
        char* item = trim(cursor);
        if (*item)
            out.emplace_back(item);
        cursor = comma ? comma + 1 : nullptr;
    }
}

void items_to_list(const xr_vector<xr_string>& items, xr_string& out)
{
    out.clear();
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i)
            out += ",";
        out += items[i];
    }
}

void apply_list_op(CInifile& ini, pcstr section, pcstr key, pcstr value, bool add, u16 layer)
{
    xr_vector<xr_string> current;
    if (ini.section_exist(section) && ini.line_exist(section, key))
        list_to_items(ini.r_string(section, key), current);

    xr_vector<xr_string> patch_items;
    list_to_items(value, patch_items);

    if (add)
    {
        for (const xr_string& item : patch_items)
            if (std::find(current.begin(), current.end(), item) == current.end())
                current.emplace_back(item);
    }
    else
    {
        for (const xr_string& item : patch_items)
            current.erase(std::remove(current.begin(), current.end(), item), current.end());
    }

    xr_string joined;
    items_to_list(current, joined);
    ini.xms_set_line(section, key, joined.c_str(), nullptr, layer);
}

bool read_text(pcstr path, xr_string& out)
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
    return true;
}

void enumerate_files(pcstr dir, pcstr mask, xr_vector<xr_string>& out)
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
    std::sort(out.begin(), out.end()); // deterministic within a module
}

// ---- what modules patch of configs other than the system config ---------------------------
// Written by the stage, on the main thread, before any other reader exists; read-only after it.

struct FileOp
{
    LtxDirective d;
    xr_string module;
    u16 layer{0};
};

struct FilePatches
{
    xr_vector<FileOp> ops; // in module load order
    xr_unordered_map<xr_string, xr_vector<u32>> by_file;
    unsigned blocks{0};
    bool stage_done{false};
    xr_vector<xr_string> system_files; // what the system config was read from, during the stage
};

FilePatches& file_patches()
{
    static FilePatches state;
    return state;
}

thread_local int held_patches = 0;

bool system_file(const xr_string& file)
{
    const auto& files = file_patches().system_files;
    return std::binary_search(files.begin(), files.end(), file);
}

void keep_file_patches(const Module& m, const xr_vector<const LtxDirective*>& list)
{
    FilePatches& p = file_patches();
    const unsigned base = p.blocks;
    unsigned top = 0;
    string512 subject;
    for (const LtxDirective* d : list)
    {
        FileOp op;
        op.d = *d;
        // blocks are numbered per list; kept together they must stay apart
        op.d.block = d->block ? base + d->block : 0;
        top = std::max(top, d->block);
        op.module = m.id;
        op.layer = m.layer;
        p.by_file[op.d.file].push_back(u32(p.ops.size()));

        // the ledger names the file: what the patch does is known now, what it meets once the file is read
        const xr_string& f = op.d.file;
        switch (d->op)
        {
        case LtxDirective::RemoveSection:
            xr_sprintf(subject, "%s: [%s]", f.c_str(), d->section.c_str());
            AddConflict(ConflictKind::LtxSection, subject, "the file", (m.id + " (removed)").c_str());
            break;
        case LtxDirective::Set:
        case LtxDirective::Add:
        case LtxDirective::Drop:
        case LtxDirective::RemoveKey:
        {
            if (!d->block)
                break;
            xr_sprintf(subject, "%s: %s.%s", f.c_str(), d->section.c_str(), d->key.c_str());
            pcstr how = d->op == LtxDirective::Set ? " = " : d->op == LtxDirective::Add ? " += " : d->op == LtxDirective::Drop ? " -= " : nullptr;
            const xr_string winner = how ? m.id + how + d->value : m.id + " (removed)";
            AddConflict(ConflictKind::LtxKey, subject, "the file", winner.c_str());
            break;
        }
        default: break;
        }
        p.ops.push_back(std::move(op));
    }
    p.blocks = base + top;
}

// A patch of the system config is applied now; one of another config is kept for when that is
// read. A file the system config was read from is both: other configs may read it too.
void route(CInifile& ini, const Module& m, const xr_vector<LtxDirective>& list, DirectiveCounts& counts)
{
    xr_vector<const LtxDirective*> system, other;
    for (const LtxDirective& d : list)
    {
        if (d.file.empty() || system_file(d.file))
            system.push_back(&d);
        if (!d.file.empty())
            other.push_back(&d);
    }
    if (!system.empty())
        ApplyDirectives(ini, system.data(), system.size(), m.id.c_str(), m.layer, true, counts);
    if (!other.empty())
        keep_file_patches(m, other);
}
} // namespace

void ApplyDirectives(CInifile& ini, const LtxDirective* const* list, size_t count, pcstr module, u16 layer,
    bool ledger, DirectiveCounts& counts)
{
    unsigned block = 0;
    bool valid = false;
    string512 subject, winner;
    for (size_t i = 0; i < count; ++i)
    {
        const LtxDirective& d = *list[i];
        if (d.op == LtxDirective::Section)
        {
            block = d.block;
            valid = ini.xms_create_section(d.section.c_str(), d.parents.c_str(), layer);
            if (!valid)
                ++counts.skipped;
            continue;
        }
        if (d.op == LtxDirective::RemoveSection)
        {
            block = d.block;
            valid = false;
            if (ini.xms_remove_section(d.section.c_str()))
            {
                if (ledger)
                {
                    xr_sprintf(subject, "[%s]", d.section.c_str());
                    AddConflict(ConflictKind::LtxSection, subject, "base/other", (xr_string(module) + " (removed)").c_str());
                }
                ++counts.ops;
            }
            else
                ++counts.skipped;
            continue;
        }
        // a key only under its own header, and only when that header made or found its section
        if (!valid || !d.block || d.block != block)
            continue;
        pcstr section = d.section.c_str();
        switch (d.op)
        {
        case LtxDirective::Set:
        {
            shared_str old_value;
            if (ini.xms_set_line(section, d.key.c_str(), d.value.c_str(), &old_value, layer) && ledger)
            {
                xr_sprintf(subject, "%s.%s", section, d.key.c_str());
                xr_sprintf(winner, "%s = %s", module, d.value.c_str());
                AddConflict(ConflictKind::LtxKey, subject, old_value.c_str() ? old_value.c_str() : "", winner);
            }
            ++counts.ops;
            break;
        }
        case LtxDirective::Add:
        case LtxDirective::Drop:
            apply_list_op(ini, section, d.key.c_str(), d.value.c_str(), d.op == LtxDirective::Add, layer);
            ++counts.ops;
            break;
        case LtxDirective::RemoveKey:
            if (!d.key.empty() && ini.xms_remove_line(section, d.key.c_str()))
                ++counts.ops;
            else
                ++counts.skipped;
            break;
        default: break;
        }
    }
}

bool WantLtxFiles()
{
    const FilePatches& p = file_patches();
    return Active() && (!p.stage_done || !p.ops.empty());
}

bool HasFilePatches() { return !file_patches().ops.empty(); }

void HoldFilePatches(bool hold) { held_patches += hold ? 1 : -1; }

bool ConfigRelative(pcstr path, xr_string& relative)
{
    if (!path || !path[0] || !FS.path_exist("$game_config$"))
        return false;
    static const xr_string root = ConfigPath(FS.get_path("$game_config$")->m_Path);
    if (root.empty())
        return false;
    const xr_string full = ConfigPath(path);
    if (full.size() <= root.size() + 1 || full.compare(0, root.size(), root) != 0 || full[root.size()] != '\\')
        return false;
    relative = full.substr(root.size() + 1);
    return true;
}

bool ApplyFilePatches(CInifile& ini, const xr_vector<xr_string>& files)
{
    const FilePatches& p = file_patches();
    if (held_patches > 0 || p.ops.empty())
        return false;
    xr_vector<u32> picked;
    for (const xr_string& file : files)
        if (const auto found = p.by_file.find(file); found != p.by_file.end())
            picked.insert(picked.end(), found->second.begin(), found->second.end());
    if (picked.empty())
        return false;
    // modules in their load order, each its directives in the order written
    std::sort(picked.begin(), picked.end());

    // a header and its keys are one module's, so the list goes over module by module
    DirectiveCounts counts;
    xr_vector<const LtxDirective*> run;
    for (size_t i = 0; i < picked.size();)
    {
        const FileOp& first = p.ops[picked[i]];
        run.clear();
        for (; i < picked.size() && p.ops[picked[i]].layer == first.layer; ++i)
            run.push_back(&p.ops[picked[i]].d);
        ApplyDirectives(ini, run.data(), run.size(), first.module.c_str(), first.layer, false, counts);
    }
    xr_string name;
    if (!ConfigRelative(ini.fname(), name))
        name = ini.fname();
    Msg("* XMS: %s: %u patch op(s) of modules applied, %u skipped", name.c_str(), counts.ops, counts.skipped);
    return true;
}

void ApplyConfigStage(CInifile* ini)
{
    if (!Active() || !ini)
        return;

    const bool merge_was = LtxMergeEnabled();
    SetLtxMergeEnabled(true);
    PatchStats stats;
    FilePatches& patches = file_patches();
    // a patch of a file the system config was read from is a patch of the system config
    patches.system_files = ini->xms_files();
    std::sort(patches.system_files.begin(), patches.system_files.end());
    patches.system_files.erase(std::unique(patches.system_files.begin(), patches.system_files.end()),
        patches.system_files.end());

    for (const Module& m : Modules())
    {
        if (m.disabled)
            continue;

        // 0) what the module only describes: its overlays and patches below may still say otherwise
        {
            CharacterConfigs characters;
            if (BuildCharacters(m, *ini, characters))
            {
                IReader reader(characters.sections.data(), characters.sections.size());
                ini->xms_load_overlay(&reader, (m.root + "characters.ltx").c_str(), m.root.c_str());
                apply_list_op(*ini, "profiles", "files", characters.profiles.c_str(), true, m.layer);
                apply_list_op(*ini, "profiles", "specific_characters_files", characters.characters.c_str(), true, m.layer);
                stats.ops += 2;
            }
        }
        {
            xr_vector<LtxDirective> lists;
            if (BuildItems(m, *ini, lists))
            {
                DirectiveCounts counts;
                route(*ini, m, lists, counts);
            }
        }

        // 1) whole-file overlays with brand new / merged sections
        {
            xr_vector<xr_string> files;
            string_path dir;
            strconcat(dir, m.root.c_str(), "gamedata" DELIMITER "configs" DELIMITER "xms" DELIMITER);
            enumerate_files(dir, "*.ltx", files);
            for (const xr_string& file : files)
                ini->xms_load_overlay(file.c_str());
        }

        // 2) directive patches
        {
            xr_vector<xr_string> files;
            string_path dir;
            strconcat(dir, m.root.c_str(), "patch" DELIMITER);
            enumerate_files(dir, "*.ltxp", files);
            PushLtxFile((m.root + "patch" DELIMITER).c_str());
            for (const xr_string& file : files)
            {
                xr_string text;
                if (!read_text(file.c_str(), text) || text.empty())
                    continue;
                ++stats.files;
                xr_vector<LtxDirective> list;
                ParseLtxp(text.c_str(), text.size(), list);
                DirectiveCounts counts;
                route(*ini, m, list, counts);
                stats.ops += counts.ops;
                stats.skipped += counts.skipped;
            }
            PopLtxFile();
        }
    }

    FinishCharacters();
    patches.stage_done = true;
    xr_vector<xr_string>().swap(patches.system_files);
    SetLtxMergeEnabled(merge_was);
    if (stats.files || stats.ops)
        Msg("* XMS: config patches applied: %u file(s), %u op(s), %u skipped", stats.files, stats.ops, stats.skipped);
    if (!patches.ops.empty())
        Msg("* XMS: %u patch op(s) of %u other config file(s) kept for when they are read", u32(patches.ops.size()),
            u32(patches.by_file.size()));
    WriteReport();
}
} // namespace XMS
