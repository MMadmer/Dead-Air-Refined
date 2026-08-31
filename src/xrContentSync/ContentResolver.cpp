#include "ContentResolver.h"

#include "ContentHash.h"
#include "ContentState.h"

#include <algorithm>
#include <chrono>
#include <limits>

#define WIN32_LEAN_AND_MEAN
// std::min / std::max are used throughout; the windows.h macros of the same name would
// swallow them.
#define NOMINMAX
#include <windows.h>

namespace
{
// Never cancelled. Lets the hashing calls take a reference unconditionally instead of
// branching on a null pointer at every call site.
const std::atomic_bool g_never{};

// An abandoned part file is a download nobody came back for. Two weeks is long enough that a
// player who left mid-fetch and returned still resumes, short enough that a failed install
// does not leave gigabytes forever.
constexpr auto PartLifetime = std::chrono::hours(24 * 14);

// Headroom above the download total. MoveFileExW replaces in place, but the incoming file and
// the one it replaces both exist for the length of the move.
constexpr std::uint64_t FreeSpaceMargin = 512ull * 1024 * 1024;

std::uint64_t file_size_or_zero(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}
}

namespace ContentResolver
{
bool Resolve(const ContentManifest::Manifest& manifest, const ContentPaths::Layout& paths,
    const Options& options, Plan& plan, std::string& error)
{
    plan = {};
    error.clear();

    const std::atomic_bool& cancel = options.cancel ? *options.cancel : g_never;
    const std::filesystem::path database = paths.Database();
    const std::filesystem::path cache = paths.Cache();
    const ContentState::Cache known = (options.verifyHashes && options.trustCache)
        ? ContentState::LoadCache(paths.State())
        : ContentState::Cache{};

    for (const ContentManifest::Bundle& bundle : manifest.bundles)
    {
        if (cancel.load(std::memory_order_acquire))
        {
            error = "cancelled";
            return false;
        }

        const std::filesystem::path installed = database / bundle.name;
        const std::uint64_t size = file_size_or_zero(installed);

        Fault fault = Fault::Missing;
        if (size == bundle.size)
        {
            if (!options.verifyHashes)
            {
                plan.satisfied.push_back(bundle);
                continue;
            }

            const std::uint64_t mtime = ContentState::FileTime(installed);
            const auto cached = known.find(bundle.name);
            const bool trusted = cached != known.end() && cached->second.size == size &&
                cached->second.mtime == mtime && mtime != 0 && cached->second.hash == bundle.hash;
            if (!trusted)
                ++plan.hashed;
            if (trusted || ContentHash::File(installed.wstring(), cancel) == bundle.hash)
            {
                plan.satisfied.push_back(bundle);
                continue;
            }
            if (cancel.load(std::memory_order_acquire))
            {
                error = "cancelled";
                return false;
            }
            fault = Fault::Corrupt;
        }
        else if (size)
        {
            fault = Fault::Size;
        }

        Job job;
        job.bundle = bundle;
        job.fault = fault;
        job.foundSize = size;

        // A verified copy already in the cache turns the job into a move. This is what lets a
        // player who downloaded and then quit before restarting pay nothing the second time -
        // but it is a full hash of a cache file, so a caller that asked for a cheap pass gets
        // the pessimistic answer instead.
        const std::filesystem::path staged = cache / bundle.hash;
        if (options.probeCache && file_size_or_zero(staged) == bundle.size &&
            ContentHash::File(staged.wstring(), cancel) == bundle.hash)
        {
            job.cached = true;
            plan.bytesToMove += bundle.size;
        }
        else
        {
            plan.bytesToFetch += bundle.size;
        }
        if (cancel.load(std::memory_order_acquire))
        {
            // A cancelled probe answers "not cached", which would otherwise be published as a
            // plan that says a bundle already on disk has to be downloaded again.
            error = "cancelled";
            return false;
        }
        plan.jobs.push_back(std::move(job));
    }

    // Anything bundle-shaped in database\ that this version does not declare. Listed, never
    // deleted here: the commit demotes them to the cache once the replacements are in place.
    // Driven by hand: the error_code overload only covers construction, and a range-for would
    // throw out of Resolve if the directory changed under the scan.
    std::error_code scanError;
    std::filesystem::directory_iterator entry(database, scanError);
    const std::filesystem::directory_iterator last;
    while (!scanError && entry != last)
    {
        std::error_code entryError;
        if (entry->is_regular_file(entryError) && !entryError)
        {
            const std::string name = entry->path().filename().string();
            if (ContentManifest::IsBundleName(name) && !manifest.FindByName(name))
                plan.obsolete.push_back(name);
        }
        entry.increment(scanError);
    }

    return true;
}

std::uint64_t RequiredFreeBytes(const Plan& plan)
{
    std::uint64_t largest = 0;
    for (const Job& job : plan.jobs)
        largest = std::max(largest, job.bundle.size);

    // Saturating, because this feeds a "do we have room?" comparison: a wrapped total would
    // read as a tiny requirement and wave through an install that cannot possibly fit.
    constexpr std::uint64_t ceiling = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t required = plan.bytesToFetch;
    if (ceiling - required < largest)
        return ceiling;
    required += largest;
    if (ceiling - required < FreeSpaceMargin)
        return ceiling;
    return required + FreeSpaceMargin;
}

std::uint64_t FreeBytes(const std::filesystem::path& path)
{
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr))
        return 0;
    return available.QuadPart;
}

bool SameVolume(const std::filesystem::path& left, const std::filesystem::path& right)
{
    const auto volume_of = [](const std::filesystem::path& path, std::wstring& out)
    {
        wchar_t buffer[MAX_PATH]{};
        if (!GetVolumePathNameW(path.c_str(), buffer, MAX_PATH))
            return false;
        out.assign(buffer);
        return true;
    };

    std::wstring a;
    std::wstring b;
    if (!volume_of(left, a) || !volume_of(right, b))
        return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

void CollectCache(const ContentPaths::Layout& paths, const ContentManifest::Manifest& manifest,
    const Plan& plan, std::uint64_t capBytes)
{
    const std::filesystem::path cache = paths.Cache();
    std::error_code error;
    if (!std::filesystem::exists(cache, error))
        return;

    // Anything the plan is about to use, or that the manifest still declares, is off limits
    // however old it is - deleting it would turn a free move into a fresh download.
    const auto wanted = [&](const std::string& name)
    {
        if (manifest.FindByHash(name))
            return true;
        return std::ranges::any_of(plan.jobs, [&](const Job& job) { return job.bundle.hash == name; });
    };

    struct Candidate
    {
        std::filesystem::path path;
        std::uint64_t size{};
        std::filesystem::file_time_type written;
    };
    std::vector<Candidate> candidates;
    std::uint64_t total = 0;

    // Every call gets its own error_code and one bad entry is skipped rather than ending the
    // sweep: a single locked or unstattable file must not stop the cache from being trimmed.
    std::filesystem::directory_iterator entry(cache, error);
    const std::filesystem::directory_iterator last;
    while (!error && entry != last)
    {
        const std::filesystem::path path = entry->path();
        std::error_code entryError;
        const bool regular = entry->is_regular_file(entryError) && !entryError;
        entry.increment(error);
        if (!regular)
            continue;

        const std::string name = path.filename().string();
        const std::uint64_t size = file_size_or_zero(path);

        if (path.extension() == L".part")
        {
            // Resumable, so worth keeping - but only for as long as somebody might plausibly
            // come back to it. Its bytes still count towards the cap: they occupy the same
            // disk the cap exists to bound.
            std::error_code timeError;
            const auto written = std::filesystem::last_write_time(path, timeError);
            if (!timeError && (std::filesystem::file_time_type::clock::now() - written) > PartLifetime)
            {
                std::error_code removeError;
                if (std::filesystem::remove(path, removeError))
                    continue;
            }
            total += size;
            continue;
        }

        if (!ContentManifest::IsSha256Hex(name))
            continue; // the README and the progress files are not ours to collect

        total += size;
        if (wanted(name))
            continue;

        std::error_code timeError;
        const auto written = std::filesystem::last_write_time(path, timeError);
        if (timeError)
            continue; // unknown age, so it cannot be ranked - leave it rather than guess

        candidates.push_back({path, size, written});
    }

    if (total <= capBytes)
        return;

    // Oldest first: a cache over its cap is trimmed by least-recently-written, so the bundles
    // most likely to be wanted again survive longest.
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) { return a.written < b.written; });
    for (const Candidate& candidate : candidates)
    {
        if (total <= capBytes)
            break;
        std::error_code removeError;
        if (std::filesystem::remove(candidate.path, removeError))
            total -= candidate.size;
    }
}
}
