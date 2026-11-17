// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_netmark.h"

#include "xms_netrule.h"

#include <mutex>

namespace XMS::NetMark
{
namespace
{
bool read_text(const xr_string& path, size_t limit, xr_string& out)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f)
        return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    const bool fits = size >= 0 && size_t(size) <= limit;
    if (fits)
    {
        out.resize(size_t(size));
        out.resize(size ? fread(out.data(), 1, size_t(size), f) : 0);
    }
    fclose(f);
    return fits;
}

struct Answer
{
    bool uses{};
    xr_string where;
};

struct Cache
{
    std::mutex lock;
    xr_map<xr_string, Answer> modules; // by the module's root
};

Cache& cache()
{
    static Cache instance;
    return instance;
}
} // namespace

bool TextUsesNetwork(std::string_view text, u32* line)
{
    unsigned at = 0;
    if (!NetRule::UsesNetwork(text, &at))
        return false;
    if (line)
        *line = at;
    return true;
}

bool CodeFile(std::string_view name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return false;
    xr_string extension(name.substr(dot + 1));
    for (char& c : extension)
        c = NetRule::lower(c);
    // scripts, shader scripts, and the assets that carry code as text
    for (const std::string_view code : {"script", "lua", "s", "nqasset", "behasset", "pdaasset"})
        if (extension == code)
            return true;
    return false;
}

bool ModuleUsesNetwork(const Module& module, xr_string* where)
{
    Cache& c = cache();
    {
        std::lock_guard guard(c.lock);
        if (const auto it = c.modules.find(module.root); it != c.modules.end())
        {
            if (where)
                *where = it->second.where;
            return it->second.uses;
        }
    }
    Answer answer;
    xr_vector<xr_string> files;
    ListFilesRelative(module.root.c_str(), "*", true, files);
    for (const xr_string& file : files)
    {
        if (!CodeFile(file))
            continue;
        xr_string text;
        // a code file larger than this is no script anybody wrote
        if (!read_text(module.root + file, 4 * 1024 * 1024, text))
            continue;
        u32 line = 0;
        if (!TextUsesNetwork(text, &line))
            continue;
        answer.uses = true;
        answer.where = file + ":" + xr_string(std::to_string(line).c_str());
        break;
    }
    if (answer.uses)
        Msg("* XMS: [%s] works with the network (%s)", module.id.c_str(), answer.where.c_str());
    std::lock_guard guard(c.lock);
    c.modules[module.root] = answer;
    if (where)
        *where = answer.where;
    return answer.uses;
}

bool AnyEnabledModuleUsesNetwork()
{
    for (const Module& module : Modules())
        if (!module.disabled && ModuleUsesNetwork(module))
            return true;
    return false;
}
} // namespace XMS::NetMark
