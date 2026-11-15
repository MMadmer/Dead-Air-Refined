// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_level_edits.h"
#include "xms_core.h"

#include <cmath>

namespace XMS
{
namespace
{
constexpr u32 cut_magic = 0x31554358; // "XCU1"
constexpr u32 aiedit_magic = 0x31454158; // "XAE1"
constexpr u32 known_version = 1;
// A module is content from anywhere: what a file may ask for is capped before it is believed.
constexpr long max_cut_bytes = 64l << 20;
constexpr u32 max_regions = 4096;
constexpr u32 max_grid_side = 4096;
constexpr long max_aiedit_bytes = 64l << 20;
constexpr u32 max_ai_edits = 1u << 20;
constexpr size_t ai_edit_bytes = 36;

struct Collision
{
    xr_string level;
    u32 triangles{};
};
Collision s_collision;

// The whole file, or nothing when it is missing, empty or larger than `limit`; the size is known
// before a byte is read.
bool read_file(pcstr path, long limit, xr_vector<u8>& out, bool& too_large)
{
    too_large = false;
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > limit)
    {
        too_large = len > limit;
        fclose(f);
        return false;
    }
    out.resize(size_t(len));
    const size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

struct Cursor
{
    const u8* at;
    const u8* end;

    bool has(size_t bytes) const { return size_t(end - at) >= bytes; }
    u32 u32_() { u32 v; memcpy(&v, at, 4); at += 4; return v; }
    float f32() { float v; memcpy(&v, at, 4); at += 4; return v; }
    u8 u8_() { return *at++; }
};

template <typename Visit>
void for_each_file(pcstr level, pcstr file, Visit&& visit)
{
    if (!XMS::Active() || !level || !level[0])
        return;
    for (const Module& m : Modules())
    {
        if (!ModuleApplies(m))
            continue;
        string_path path;
        xr_sprintf(path, "%slevels" DELIMITER "%s" DELIMITER "%s", m.root.c_str(), level, file);
        visit(pcstr(path));
    }
}

bool finite(float v) { return std::isfinite(v); }

pcstr parse_cuts(Cursor& c, xr_vector<CutRegion>& out)
{
    if (!c.has(12) || c.u32_() != cut_magic)
        return "not an XCU1 file";
    const u32 version = c.u32_();
    if (version > known_version)
        return "made for a newer game";
    if (version == 0)
        return "version 0";
    const u32 count = c.u32_();
    if (count > max_regions)
        return "too many regions";
    if (!c.has(size_t(count) * 32))
        return "cut short";
    for (u32 i = 0; i < count; ++i)
    {
        if (!c.has(32))
            return "cut short";
        CutRegion r;
        r.kind = c.u8_();
        c.at += 3;
        r.centre.x = c.f32(); r.centre.y = c.f32(); r.centre.z = c.f32();
        r.half.x = c.f32(); r.half.y = c.f32(); r.half.z = c.f32();
        r.yaw = c.f32();
        if (r.kind > 1)
            return "a region of an unknown kind";
        if (!finite(r.centre.x) || !finite(r.centre.y) || !finite(r.centre.z) || !finite(r.yaw) ||
            !(r.half.x > 0.f && r.half.x < 1e6f) || !(r.half.z > 0.f && r.half.z < 1e6f) ||
            !(r.half.y >= 0.f && r.half.y < 1e6f))
            return "a region with no size or a value that is not a number";
        if (r.kind == 1)
        {
            if (!c.has(12))
                return "cut short";
            r.tolerance = c.f32();
            r.columns = c.u32_();
            r.rows = c.u32_();
            if (!(r.tolerance >= 0.f && r.tolerance < 1e6f) || r.columns < 2 || r.rows < 2 ||
                r.columns > max_grid_side || r.rows > max_grid_side)
                return "a ground region with a bad tolerance or grid";
            const size_t samples = size_t(r.columns) * r.rows;
            if (!c.has(samples * 4))
                return "cut short";
            r.heights.resize(samples);
            memcpy(r.heights.data(), c.at, samples * 4);
            c.at += samples * 4;
            for (const float h : r.heights)
                if (!finite(h))
                    return "a height that is not a number";
        }
        out.push_back(std::move(r));
    }
    return nullptr;
}

pcstr parse_ai_edits(Cursor& c, xr_vector<AiEdit>& out)
{
    if (!c.has(12) || c.u32_() != aiedit_magic)
        return "not an XAE1 file";
    const u32 version = c.u32_();
    if (version > known_version)
        return "made for a newer game";
    if (version == 0)
        return "version 0";
    const u32 count = c.u32_();
    if (count > max_ai_edits)
        return "too many edits";
    if (!c.has(size_t(count) * ai_edit_bytes))
        return "cut short";
    out.reserve(out.size() + count);
    for (u32 i = 0; i < count; ++i)
    {
        AiEdit e;
        e.x = c.f32(); e.z = c.f32();
        e.old_y = c.f32(); e.new_y = c.f32();
        e.normal.x = c.f32(); e.normal.y = c.f32(); e.normal.z = c.f32();
        e.flags = c.u32_();
        if (!finite(e.x) || !finite(e.z) || !finite(e.old_y) || !finite(e.new_y) || !finite(e.normal.x) ||
            !finite(e.normal.y) || !finite(e.normal.z))
            return "a value that is not a number";
        const float length = e.normal.magnitude();
        if (!(length > 1e-6f) || !(e.normal.y / length > 0.f))
            return "a normal that does not point up";
        e.normal.div(length);
        out.push_back(e);
    }
    return nullptr;
}
} // namespace

void ReadLevelCuts(pcstr level, LevelCuts& out)
{
    out.regions.clear();
    out.hash = 0;
    for_each_file(level, "overlay.cut", [&](pcstr path) {
        xr_vector<u8> raw;
        bool too_large;
        if (!read_file(path, max_cut_bytes, raw, too_large))
        {
            if (too_large)
                Msg("! [cuts] %s: larger than %u MB, refused", path, u32(max_cut_bytes >> 20));
            return;
        }
        Cursor c{raw.data(), raw.data() + raw.size()};
        xr_vector<CutRegion> regions;
        if (pcstr why = parse_cuts(c, regions))
        {
            Msg("! [cuts] %s: %s, refused", path, why);
            return;
        }
        for (CutRegion& r : regions)
            out.regions.push_back(std::move(r));
        out.hash = crc32(raw.data(), u32(raw.size()), out.hash);
    });
    // Files of no region cut nothing: the collision cache keeps the key it had without them.
    if (out.regions.empty())
        out.hash = 0;
}

void ReadAiEdits(pcstr level, xr_vector<AiEdit>& out)
{
    out.clear();
    for_each_file(level, "overlay.aiedit", [&](pcstr path) {
        xr_vector<u8> raw;
        bool too_large;
        if (!read_file(path, max_aiedit_bytes, raw, too_large))
        {
            if (too_large)
                Msg("! [ai edits] %s: larger than %u MB, refused", path, u32(max_aiedit_bytes >> 20));
            return;
        }
        Cursor c{raw.data(), raw.data() + raw.size()};
        xr_vector<AiEdit> edits;
        if (pcstr why = parse_ai_edits(c, edits))
        {
            Msg("! [ai edits] %s: %s, refused", path, why);
            return;
        }
        out.insert(out.end(), edits.begin(), edits.end());
    });
}

void NoteCollisionCuts(pcstr level, u32 triangles)
{
    s_collision.level = level ? level : "";
    s_collision.triangles = triangles;
}

u32 TakeCollisionCuts(pcstr level)
{
    const u32 triangles = level && s_collision.level == level ? s_collision.triangles : 0;
    s_collision = {};
    return triangles;
}
} // namespace XMS
