#include "ContentResolver.h"

#include "ContentDelta.h"
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

// A delta is only worth it when it is decisively smaller. At two thirds of the target it saves
// a third of the bandwidth and buys a base re-hash, an apply pass, twice the peak cache use and
// a whole extra way to fail; below about a third the saving is large enough to pay for that.
constexpr double DeltaSkipRatio = 0.35;

std::uint64_t file_size_or_zero(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}
}

namespace ContentResolver
{
namespace
{
// Finds the cheapest published chain of deltas from a revision this installation can produce to
// the one each job wants.
//
// The base is normally one of the obsolete files. An update renames a bundle whose bytes
// changed, so the previous revision sits in the database directory under its own name while the
// new one is simply missing - and because a bundle's name carries the first sixteen hex of its
// hash, a candidate base can be recognised from its name and size without reading a byte of it.
// The apply step re-hashes it in full before trusting it.
void plan_deltas(const ContentManifest::Manifest& manifest, const ContentPaths::Layout& paths,
    const Options& options, Plan& plan)
{
    if (!options.useDeltas || manifest.deltas.empty() || plan.jobs.empty())
        return;

    // Where a chain may start: any delta base this installation already holds, at the size that
    // delta says it should be.
    std::vector<std::string> available;
    for (const ContentManifest::Delta& delta : manifest.deltas)
    {
        const std::filesystem::path installed = paths.Database() / delta.baseName;
        const std::filesystem::path staged = paths.Cache() / delta.baseHash;
        if (file_size_or_zero(installed) == delta.baseSize || file_size_or_zero(staged) == delta.baseSize)
            available.push_back(delta.baseHash);
    }
    if (available.empty())
        return;

    const std::filesystem::path rejected = paths.RejectedDeltas();

    for (Job& job : plan.jobs)
    {
        if (job.cached)
            continue;

        // Dijkstra over a graph of a few hundred edges: cheapest total asset bytes from any
        // hash we can already produce to the one this job wants. Cheapest rather than shortest,
        // because two small hops beat one large one.
        struct Reached
        {
            std::string hash;
            std::uint64_t cost{};
            std::vector<ContentManifest::Delta> path;
        };
        std::vector<Reached> frontier;
        for (const std::string& hash : available)
            frontier.push_back({hash, 0, {}});

        std::vector<std::string> settled;
        std::vector<ContentManifest::Delta> best;
        std::uint64_t bestCost = 0;
        bool found = false;

        while (!frontier.empty() && !found)
        {
            const auto cheapest = std::ranges::min_element(frontier,
                [](const Reached& a, const Reached& b) { return a.cost < b.cost; });
            const Reached current = *cheapest;
            frontier.erase(cheapest);

            if (std::ranges::find(settled, current.hash) != settled.end())
                continue;
            settled.push_back(current.hash);

            if (current.hash == job.bundle.hash && !current.path.empty())
            {
                best = current.path;
                bestCost = current.cost;
                found = true;
                break;
            }

            for (const ContentManifest::Delta& delta : manifest.deltas)
            {
                if (delta.baseHash != current.hash)
                    continue;
                // A delta that already failed a hash verdict is not tried again - that verdict
                // means the published asset is wrong, and re-fetching it would waste the same
                // bandwidth to reach the same conclusion.
                if (ContentDelta::IsRejected(rejected, delta.name))
                    continue;
                if (std::ranges::find(settled, delta.targetHash) != settled.end())
                    continue;

                Reached next{delta.targetHash, current.cost + delta.size, current.path};
                next.path.push_back(delta);
                frontier.push_back(std::move(next));
            }
        }

        if (!found)
            continue;

        // Only decisively cheaper chains are worth the extra machinery. The peak cache cost of
        // a multi-hop chain is two intermediates at once, which is another reason not to take a
        // marginal saving.
        if (static_cast<double>(bestCost) >= DeltaSkipRatio * static_cast<double>(job.bundle.size))
            continue;

        job.deltaPath = std::move(best);
        job.deltaBytes = bestCost;
    }
}
}

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

    plan_deltas(manifest, paths, options, plan);

    for (const Job& job : plan.jobs)
    {
        if (job.cached)
            continue;
        plan.bytesToFetch += job.deltaPath.empty() ? job.bundle.size : job.deltaBytes;
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
