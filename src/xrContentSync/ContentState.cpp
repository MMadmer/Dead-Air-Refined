// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "ContentState.h"

#include <string_view>

#include "ContentManifest.h"

#include <charconv>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
// std::min / std::max are used throughout; the windows.h macros of the same name would
// swallow them.
#define NOMINMAX
#include <windows.h>

namespace
{
bool parse_u64(std::string_view value, std::uint64_t& out)
{
    if (value.empty())
        return false;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    return error == std::errc{} && end == value.data() + value.size();
}

void strip_eol(std::string& line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
}
}

namespace ContentState
{
const char* ReasonText(Reason reason)
{
    switch (reason)
    {
    case Reason::InstallCommit: return "install-commit";
    case Reason::UpdateCommit: return "update-commit";
    case Reason::RepairCommit: return "repair-commit";
    case Reason::VerifyFailed: return "verify-failed";
    case Reason::ManifestMissing: return "manifest-missing";
    case Reason::ManifestInvalid: return "manifest-invalid";
    }
    return "verify-failed";
}

bool WriteLatch(const std::filesystem::path& path, const std::string& version, Reason reason)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const std::uint64_t stamp = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;

    output << "schema=" << ContentManifest::SchemaIncomplete << '\n';
    output << "version=" << (version.empty() ? "0.0.0" : version) << '\n';
    output << "reason=" << ReasonText(reason) << '\n';
    output << "time=" << stamp << '\n';
    // Flushed before the verdict: a stream that has buffered everything is still "good"
    // and would report success for a write that never reached the disk - which is precisely the
    // failure this function exists to be able to report.
    output.flush();
    return output.good();
}

bool LatchPresent(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

void ClearLatch(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

std::uint64_t FileTime(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return 0;
    return (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
        attributes.ftLastWriteTime.dwLowDateTime;
}

std::string LoadContentId(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    std::string line;
    if (!std::getline(input, line))
        return {};
    strip_eol(line);
    if (line != std::string("schema=") + ContentManifest::SchemaState)
        return {};

    if (!std::getline(input, line))
        return {};
    strip_eol(line);

    constexpr std::string_view prefix = "content-id=";
    if (line.compare(0, prefix.size(), prefix) != 0)
        return {};
    return line.substr(prefix.size());
}

Cache LoadCache(const std::filesystem::path& path)
{
    Cache cache;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return cache;

    std::string line;
    if (!std::getline(input, line))
        return cache;
    strip_eol(line);
    if (line != std::string("schema=") + ContentManifest::SchemaState)
        return cache;

    // Line 2 is content-id. Recorded for diagnostics only and deliberately not used to
    // invalidate: versions share bundles, so throwing the cache away on every content-id
    // change would rehash several GB on every update for no gain.
    std::getline(input, line);

    while (std::getline(input, line))
    {
        strip_eol(line);
        if (line.empty())
            continue;

        const std::string_view view(line);
        std::string_view fields[4];
        std::size_t count = 0;
        std::size_t start = 0;
        while (count < 4)
        {
            const std::size_t tab = view.find('\t', start);
            if (tab == std::string_view::npos)
            {
                fields[count++] = view.substr(start);
                break;
            }
            fields[count++] = view.substr(start, tab - start);
            start = tab + 1;
        }
        if (count != 4)
            continue;

        Entry entry;
        if (!ContentManifest::IsSha256Hex(fields[0]) || !parse_u64(fields[1], entry.size) ||
            !parse_u64(fields[2], entry.mtime) || fields[3].empty())
            continue;
        entry.hash.assign(fields[0]);
        cache.emplace(std::string(fields[3]), entry);
    }
    return cache;
}

void SaveCache(const std::filesystem::path& path, const std::string& contentId, const Cache& cache)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return;

    output << "schema=" << ContentManifest::SchemaState << '\n';
    output << "content-id=" << contentId << '\n';
    for (const auto& [name, entry] : cache)
        output << entry.hash << '\t' << entry.size << '\t' << entry.mtime << '\t' << name << '\n';
}
}
