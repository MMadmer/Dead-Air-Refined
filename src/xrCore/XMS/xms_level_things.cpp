// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_level_things.h"
#include "xms_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/stat.h>

namespace XMS
{
namespace
{
// A module is content from anywhere: what a file may ask for is capped before it is believed.
constexpr long max_bytes = 4l << 20;
// per file: one module's file never takes the room of another's
constexpr size_t max_things = 4096;
constexpr size_t max_lines = 32;
constexpr size_t max_name = 260;
// a volume of fog is a simulation of its own: a level holds a handful
constexpr size_t max_fogs = 16;
constexpr size_t max_occluders = 16;

struct Block
{
    xr_string kind;
    xr_string name;
    xr_vector<std::pair<xr_string, xr_string>> lines;

    pcstr value(pcstr key) const
    {
        for (const auto& line : lines)
            if (line.first == key)
                return line.second.c_str();
        return nullptr;
    }
};

xr_string trimmed(const char* from, const char* to)
{
    while (from < to && (*from == ' ' || *from == '\t' || *from == '\r'))
        ++from;
    while (to > from && (to[-1] == ' ' || to[-1] == '\t' || to[-1] == '\r'))
        --to;
    return xr_string(from, size_t(to - from));
}

// `[kind:name]` blocks of `key = value` lines; `;` starts a comment. At most `max_things`
// blocks are read, and one more that says there were more; a block keeps its first lines.
void parse_blocks(const xr_vector<u8>& raw, xr_vector<Block>& out)
{
    const char* at = reinterpret_cast<const char*>(raw.data());
    const char* const end = at + raw.size();
    while (at < end)
    {
        const char* eol = static_cast<const char*>(memchr(at, '\n', size_t(end - at)));
        if (!eol)
            eol = end;
        const char* stop = static_cast<const char*>(memchr(at, ';', size_t(eol - at)));
        const xr_string line = trimmed(at, stop ? stop : eol);
        at = eol < end ? eol + 1 : end;
        if (line.empty())
            continue;
        if (line.front() == '[')
        {
            const size_t close = line.find(']');
            if (close == xr_string::npos)
                continue;
            const xr_string head = line.substr(1, close - 1);
            const size_t colon = head.find(':');
            Block b;
            b.kind = colon == xr_string::npos ? head : head.substr(0, colon);
            b.name = colon == xr_string::npos ? xr_string() : head.substr(colon + 1);
            // a name only tells the blocks apart in the log: it is never as long as a file
            if (b.kind.size() > 16)
                b.kind.resize(16);
            if (b.name.size() > 64)
                b.name.resize(64);
            out.push_back(std::move(b));
            if (out.size() > max_things)
                return;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == xr_string::npos || out.empty() || out.back().lines.size() >= max_lines)
            continue;
        out.back().lines.emplace_back(
            trimmed(line.c_str(), line.c_str() + eq), trimmed(line.c_str() + eq + 1, line.c_str() + line.size()));
    }
}

// Up to `count` finite numbers separated by commas; false when one is missing or is no number.
bool numbers(pcstr text, float* out, int count)
{
    if (!text)
        return false;
    for (int i = 0; i < count; ++i)
    {
        char* next = nullptr;
        const float v = std::strtof(text, &next);
        if (next == text || !std::isfinite(v))
            return false;
        out[i] = v;
        text = next;
        while (*text == ' ' || *text == '\t')
            ++text;
        if (i + 1 < count)
        {
            if (*text != ',')
                return false;
            ++text;
        }
    }
    return true;
}

bool read_vector(const Block& b, pcstr key, Fvector& out)
{
    float v[3];
    if (!numbers(b.value(key), v, 3))
        return false;
    out.set(v[0], v[1], v[2]);
    return true;
}

void read_number(const Block& b, pcstr key, float& out)
{
    float v;
    if (numbers(b.value(key), &v, 1))
        out = v;
}

void read_pair(const Block& b, pcstr key, Ivector2& out)
{
    float v[2];
    if (numbers(b.value(key), v, 2) && v[0] >= 0.f && v[1] >= v[0] && v[1] < 2.0e9f)
        out.set(int(v[0]), int(v[1]));
}

bool truth(const Block& b, pcstr key)
{
    pcstr v = b.value(key);
    return v && (0 == xr_strcmp(v, "true") || 0 == xr_strcmp(v, "on") || 0 == xr_strcmp(v, "1"));
}

// A name of the game's own: letters, digits and the few signs its file names use. No dot: a
// name is said without its extension, and the sound engine cuts one at its last dot.
bool plain_name(pcstr name)
{
    if (!name || !name[0] || xr_strlen(name) >= max_name)
        return false;
    for (pcstr c = name; *c; ++c)
    {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            *c == '_' || *c == '-' || *c == '\\' || *c == '/' || *c == '$' || *c == ' ';
        if (!ok)
            return false;
    }
    return true;
}

// A name of a file under a folder of the game, its extension with it: a plain name with dots that
// never step up.
bool plain_file(pcstr name)
{
    if (!name || !name[0] || xr_strlen(name) >= max_name || strstr(name, ".."))
        return false;
    for (pcstr c = name; *c; ++c)
    {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            *c == '_' || *c == '-' || *c == '\\' || *c == '/' || *c == '.';
        if (!ok)
            return false;
    }
    return name[0] != '\\' && name[0] != '/';
}

// A box: `position` (read by the caller), `rotation` and `size`, each optional.
void read_box(const Block& b, const Fvector& position, LevelBox& out)
{
    out.position = position;
    read_vector(b, "rotation", out.rotation);
    Fvector size;
    if (read_vector(b, "size", size))
        out.size = size;
    for (float* s : {&out.size.x, &out.size.y, &out.size.z})
        clamp(*s, 0.01f, 4000.f);
}

// One optional colour of an environment modifier: said, it is used.
void env_colour(const Block& b, pcstr key, Fvector& out, u16 bit, u16& use)
{
    if (read_vector(b, key, out))
        use |= bit;
}

// One block into `out`. `fogs_before` is how many fog volumes `out` held before this file: the
// cap on them is each file's own, like the cap on things.
pcstr read_block(const Block& b, LevelThings& out, size_t fogs_before)
{
    Fvector position;
    if (!read_vector(b, "position", position))
        return "no position";
    if (b.kind == "sound")
    {
        LevelSound s;
        pcstr name = b.value("name");
        if (!plain_name(name))
            return "no sound name";
        s.name = name;
        s.position = position;
        read_number(b, "volume", s.volume);
        read_number(b, "frequency", s.frequency);
        read_pair(b, "active", s.active);
        read_pair(b, "play", s.play);
        read_pair(b, "pause", s.pause);
        clamp(s.volume, 0.f, 4.f);
        clamp(s.frequency, 0.1f, 4.f);
        out.sounds.push_back(std::move(s));
        return nullptr;
    }
    if (b.kind == "particles")
    {
        LevelParticles p;
        pcstr name = b.value("name");
        if (!plain_name(name))
            return "no effect name";
        p.name = name;
        p.position = position;
        read_vector(b, "rotation", p.rotation);
        out.particles.push_back(std::move(p));
        return nullptr;
    }
    if (b.kind == "light")
    {
        LevelLight l;
        l.position = position;
        pcstr type = b.value("type");
        l.spot = type && 0 == xr_strcmp(type, "spot");
        if (read_vector(b, "direction", l.direction) && l.direction.square_magnitude() > 1e-6f)
            l.direction.normalize();
        else
            l.direction.set(0.f, -1.f, 0.f);
        read_vector(b, "color", l.color);
        read_number(b, "range", l.range);
        read_number(b, "cone", l.cone);
        l.shadows = truth(b, "shadows");
        clamp(l.range, 0.1f, 200.f);
        clamp(l.cone, 0.05f, 3.1f);
        for (float* c : {&l.color.x, &l.color.y, &l.color.z})
            clamp(*c, 0.f, 16.f);
        out.lights.push_back(l);
        return nullptr;
    }
    if (b.kind == "env")
    {
        LevelEnv e;
        e.position = position;
        read_number(b, "radius", e.radius);
        read_number(b, "power", e.power);
        clamp(e.radius, 0.1f, 10000.f);
        clamp(e.power, 0.f, 1.f);
        float far_plane = 0.f;
        if (numbers(b.value("far_plane"), &far_plane, 1) && far_plane > 0.f)
        {
            e.far_plane = far_plane;
            e.use |= 1;
        }
        env_colour(b, "fog_color", e.fog_color, 2, e.use);
        float density = 0.f;
        if (numbers(b.value("fog_density"), &density, 1) && density >= 0.f)
        {
            e.fog_density = density;
            e.use |= 4;
        }
        env_colour(b, "ambient", e.ambient, 8, e.use);
        env_colour(b, "sky_color", e.sky_color, 16, e.use);
        env_colour(b, "hemi_color", e.hemi_color, 32, e.use);
        if (!e.use)
            return "it changes nothing";
        out.envs.push_back(e);
        return nullptr;
    }
    if (b.kind == "glow")
    {
        LevelGlow g;
        g.position = position;
        pcstr texture = b.value("texture");
        if (!plain_name(texture))
            return "no texture name";
        g.texture = texture;
        // a shader said empty is a shader not said
        if (pcstr shader = b.value("shader"); shader && shader[0])
        {
            if (!plain_name(shader))
                return "a shader name that is none";
            g.shader = shader;
        }
        read_number(b, "radius", g.radius);
        read_vector(b, "color", g.color);
        g.fixed = truth(b, "fixed");
        if (g.fixed)
            clamp(g.radius, 0.001f, 1.f);
        else
            clamp(g.radius, 0.01f, 50.f);
        for (float* c : {&g.color.x, &g.color.y, &g.color.z})
            clamp(*c, 0.f, 4.f);
        out.glows.push_back(std::move(g));
        return nullptr;
    }
    if (b.kind == "fog")
    {
        if (out.fogs.size() - fogs_before >= max_fogs)
            return "one fog volume too many for a module's level";
        LevelFog f;
        pcstr profile = b.value("profile");
        if (!plain_file(profile))
            return "no profile name";
        f.profile = profile;
        read_box(b, position, f.box);
        // `occluder = position, rotation, size`: nine numbers a line, any number of lines
        for (const auto& line : b.lines)
        {
            if (line.first != "occluder" || f.occluders.size() >= max_occluders)
                continue;
            float v[9];
            if (!numbers(line.second.c_str(), v, 9))
                return "an occluder that is no nine numbers";
            LevelBox o;
            o.position.set(v[0], v[1], v[2]);
            o.rotation.set(v[3], v[4], v[5]);
            o.size.set(v[6], v[7], v[8]);
            for (float* s : {&o.size.x, &o.size.y, &o.size.z})
                clamp(*s, 0.01f, 4000.f);
            f.occluders.push_back(o);
        }
        out.fogs.push_back(std::move(f));
        return nullptr;
    }
    if (b.kind == "sound_env")
    {
        LevelSoundEnv e;
        pcstr inner = b.value("inner");
        pcstr outer = b.value("outer");
        if (!plain_name(inner) || !plain_name(outer))
            return "no environment names";
        e.inner = inner;
        e.outer = outer;
        read_box(b, position, e.box);
        out.sound_envs.push_back(std::move(e));
        return nullptr;
    }
    return "an unknown kind";
}

// What was read last: the level, what its files were (every applying module's file by size and
// time) and the things. A level's load asks three times - the sounds, the effects and lights,
// the environment - and reads and reports once.
struct Read
{
    xr_string level;
    xr_string files;
    LevelThings things;
};

std::mutex g_read_guard;
Read g_read;

xr_string things_path(const Module& m, pcstr level)
{
    xr_string path = m.root.c_str();
    path += "levels" DELIMITER;
    path += level;
    path += DELIMITER "overlay.things";
    return path;
}

// One module's file into `out`; what it holds is counted against the file's own cap.
void read_file(const Module& m, pcstr level, const xr_string& path, LevelThings& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > max_bytes)
    {
        fclose(f);
        if (len > max_bytes)
            Msg("! [level things] %s: larger than %u MB, refused", path.c_str(), u32(max_bytes >> 20));
        return;
    }
    xr_vector<u8> raw(static_cast<size_t>(len));
    const size_t got = fread(raw.data(), 1, raw.size(), f);
    fclose(f);
    if (got != raw.size())
        return;
    xr_vector<Block> blocks;
    parse_blocks(raw, blocks);
    if (blocks.size() > max_things)
    {
        Msg("! [level things] %s: more than %u things, the rest of the file is left out", path.c_str(),
            u32(max_things));
        blocks.resize(max_things);
    }
    size_t taken = 0;
    const size_t fogs_before = out.fogs.size();
    for (const Block& b : blocks)
    {
        if (pcstr why = read_block(b, out, fogs_before))
            Msg("! [level things] %s: [%s:%s] %s, left out", path.c_str(), b.kind.c_str(), b.name.c_str(), why);
        else
            ++taken;
    }
    Msg("* [level things] %s: %u things of %s", level, u32(taken), m.id.c_str());
}
} // namespace

void ReadLevelThings(pcstr level, LevelThings& out)
{
    out = {};
    if (!XMS::Active() || !level || !level[0])
        return;
    // which files there are, and whether they are the ones read last
    xr_vector<std::pair<const Module*, xr_string>> files;
    xr_string stamp;
    for (const Module& m : Modules())
    {
        if (!ModuleApplies(m))
            continue;
        xr_string path = things_path(m, level);
        struct _stat64 st;
        if (0 != _stat64(path.c_str(), &st))
            continue;
        char word[64];
        std::snprintf(word, sizeof(word), "|%lld:%lld", static_cast<long long>(st.st_size),
            static_cast<long long>(st.st_mtime));
        stamp += path;
        stamp += word;
        files.emplace_back(&m, std::move(path));
    }
    std::lock_guard<std::mutex> lock(g_read_guard);
    if (g_read.level != level || g_read.files != stamp)
    {
        g_read.level = level;
        g_read.files = stamp;
        g_read.things = {};
        for (const auto& file : files)
            read_file(*file.first, level, file.second, g_read.things);
    }
    out = g_read.things;
}

void ReadLevelThings(LevelThings& out)
{
    out = {};
    if (!FS.path_exist("$level$"))
        return;
    pcstr folder = FS.get_path("$level$")->m_Add;
    if (!folder)
        return;
    xr_string level = folder;
    while (!level.empty() && (level.back() == '\\' || level.back() == '/'))
        level.pop_back();
    if (!level.empty())
        ReadLevelThings(level.c_str(), out);
}
} // namespace XMS
