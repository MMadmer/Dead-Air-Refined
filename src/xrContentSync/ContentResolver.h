#pragma once

// Turns "this is the manifest, this is the disk" into "this is what has to happen".
//
// Every actor asks the same question - the installer before it fetches, the game before it
// repairs, the updater before it commits - and they must all get the same answer, so the
// comparison lives in one place and nobody re-derives it.

#include "ContentManifest.h"
#include "ContentPaths.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ContentResolver
{
// Why a declared bundle is not usable. The distinction survives into the UI because it decides
// what the player is told, and into the log because it decides what a bug report is about.
enum class Fault
{
    Missing,   // not in database\ at all
    Size,      // present under the right name at the wrong length
    Corrupt    // right name, right size, wrong bytes
};

struct Job
{
    ContentManifest::Bundle bundle;
    Fault fault{Fault::Missing};

    // What was actually on disk. Carried so the message can say "is 230609 bytes, the manifest
    // says 234705" instead of the useless "wrong size".
    std::uint64_t foundSize{};

    // True when content-cache\ already holds a verified copy: the job is a move, not a
    // download. This is what makes "quit instead of restarting" cost nothing next launch.
    bool cached{};

    // A chain of published deltas leading from a revision this installation already has to the
    // one it wants, cheapest first. Empty when there is no usable chain, or when the chain is
    // not enough cheaper than the whole bundle to be worth the extra failure path.
    //
    // The base is normally one of the OBSOLETE files: an update renames a bundle whose bytes
    // changed, so the previous revision is sitting in the database directory under its own name
    // while the new one is missing. That is what the chain starts from.
    std::vector<ContentManifest::Delta> deltaPath;
    std::uint64_t deltaBytes{};
};

struct Plan
{
    std::vector<ContentManifest::Bundle> satisfied;
    std::vector<Job> jobs;

    // Bundle-shaped files in database\ that the manifest does not declare. Demoted to the
    // cache, never deleted inside a commit - see the add-before-delete rule.
    std::vector<std::string> obsolete;

    std::uint64_t bytesToFetch{}; // sum of jobs that are not already cached, deltas counted at
                                  // their asset size rather than the bundle's
    std::uint64_t bytesToMove{};  // sum of jobs that are

    // How many installed bundles had to be read rather than vouched for by the state cache.
    // Reported because it is the only way to see whether the warm path is working: a steady
    // state that keeps rehashing several GB on every launch is a bug that is otherwise silent.
    std::size_t hashed{};

    bool Complete() const { return jobs.empty(); }
};

// `verifyHashes` decides how much this costs. False is a stat per bundle and is what the
// installer and the commit use, where the bytes were just written and verified. True hashes
// everything the state cache cannot vouch for.
struct Options
{
    bool verifyHashes{true};

    // Whether to look for an already-downloaded copy in the cache. That check is a full hash of
    // a cache file, so a caller that wants a genuinely cheap pass clears it and accepts the
    // pessimistic answer - a job reported as a download that turns out to be a move.
    bool probeCache{true};

    // Whether to look for a delta chain instead of a whole bundle. Cleared on the cheap pass,
    // where the answer is not needed and the search would read the manifest's whole edge list.
    bool useDeltas{true};

    // Whether content-state.txt may vouch for an installed bundle. Cleared by a forced verify,
    // which exists precisely to re-read what the cache claims. The file itself is left alone:
    // a pass that is cancelled halfway must not cost the next launch a full rehash.
    bool trustCache{true};
    // Consulted between files so a cancelled wizard page does not have to wait out a 5 GB pass.
    const std::atomic_bool* cancel{};
};

bool Resolve(const ContentManifest::Manifest& manifest, const ContentPaths::Layout& paths,
    const Options& options, Plan& plan, std::string& error);

// Free space needed to carry out `plan`: every fetch, plus headroom for the largest single
// bundle being written while its predecessor still exists, plus a margin. Add-before-delete
// means the peak is genuinely higher than the download total.
std::uint64_t RequiredFreeBytes(const Plan& plan);

// Free bytes on the volume holding `path`, or 0 when it cannot be determined - which callers
// must treat as "unknown", never as "full".
std::uint64_t FreeBytes(const std::filesystem::path& path);

// The commit moves files from the cache into database\ with MoveFileExW, which is only atomic
// within a volume. Across volumes it silently degrades into copy-and-delete, doubling the peak
// disk requirement this plan was sized against.
bool SameVolume(const std::filesystem::path& left, const std::filesystem::path& right);

// Removes what the cache no longer needs: finished files that no live manifest wants, and
// abandoned `.part` files. Never touches anything `plan` is about to use.
void CollectCache(const ContentPaths::Layout& paths, const ContentManifest::Manifest& manifest,
    const Plan& plan, std::uint64_t capBytes);
}
