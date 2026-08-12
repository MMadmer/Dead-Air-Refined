#include "stdafx.h"

#include "xms_ltx.h"
#include "xms_core.h"
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

void apply_list_op(CInifile* ini, const Module& m, pcstr section, pcstr key, pcstr value, bool add, PatchStats& stats)
{
    xr_vector<xr_string> current;
    if (ini->section_exist(section) && ini->line_exist(section, key))
        list_to_items(ini->r_string(section, key), current);

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
    ini->xms_set_line(section, key, joined.c_str(), nullptr);
    ++stats.ops;
}

void apply_patch_file(CInifile* ini, const Module& m, pcstr path, PatchStats& stats)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0)
    {
        fclose(f);
        return;
    }
    char* buffer = xr_alloc<char>(size_t(len) + 1);
    const size_t got = fread(buffer, 1, size_t(len), f);
    fclose(f);
    buffer[got] = 0;
    ++stats.files;

    string512 section = "";
    bool section_valid = false;
    string512 subject, winner;

    for (char* line = buffer; line && *line;)
    {
        char* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (char* comment = strchr(line, ';'))
            *comment = 0;
        char* text = trim(line);
        line = next;
        if (!*text)
            continue;

        if (text[0] == '[')
        {
            char* close = strchr(text, ']');
            if (!close)
                continue;
            *close = 0;
            char* name = trim(text + 1);
            char* parents = trim(close + 1);
            if (parents[0] == ':')
                parents = trim(parents + 1);
            else
                parents = nullptr;

            if (name[0] == '!') // [!section] = delete section
            {
                name = trim(name + 1);
                xr_strlwr(name);
                if (ini->xms_remove_section(name))
                {
                    xr_sprintf(subject, "[%s]", name);
                    AddConflict(ConflictKind::LtxSection, subject, "base/other", (m.id + " (removed)").c_str());
                    ++stats.ops;
                }
                else
                    ++stats.skipped;
                section_valid = false;
                continue;
            }
            xr_strlwr(name);
            xr_strcpy(section, name);
            section_valid = ini->xms_create_section(section, parents);
            if (!section_valid)
                ++stats.skipped;
            continue;
        }

        if (!section_valid)
            continue;

        if (char* eq = strchr(text, '='))
        {
            *eq = 0;
            char* key = trim(text);
            char* value = trim(eq + 1);
            if (key[0] == '+') // +key = append list items
            {
                apply_list_op(ini, m, section, trim(key + 1), value, true, stats);
            }
            else if (key[0] == '-') // -key = remove list items
            {
                apply_list_op(ini, m, section, trim(key + 1), value, false, stats);
            }
            else if (*key)
            {
                shared_str old_value;
                if (ini->xms_set_line(section, key, value, &old_value))
                {
                    xr_sprintf(subject, "%s.%s", section, key);
                    xr_sprintf(winner, "%s = %s", m.id.c_str(), value);
                    AddConflict(ConflictKind::LtxKey, subject, old_value.c_str() ? old_value.c_str() : "", winner);
                }
                ++stats.ops;
            }
        }
        else if (text[0] == '!') // !key = delete key
        {
            char* key = trim(text + 1);
            if (*key && ini->xms_remove_line(section, key))
                ++stats.ops;
            else
                ++stats.skipped;
        }
    }
    xr_free(buffer);
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
} // namespace

void ApplyConfigStage(CInifile* ini)
{
    if (!Active() || !ini)
        return;

    const bool merge_was = LtxMergeEnabled();
    SetLtxMergeEnabled(true);
    PatchStats stats;

    for (const Module& m : Modules())
    {
        if (m.disabled)
            continue;

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
                apply_patch_file(ini, m, file.c_str(), stats);
            PopLtxFile();
        }
    }

    SetLtxMergeEnabled(merge_was);
    if (stats.files || stats.ops)
        Msg("* XMS: config patches applied: %u file(s), %u op(s), %u skipped", stats.files, stats.ops, stats.skipped);
    WriteReport();
}
} // namespace XMS
