#pragma once

// The two files that record what happened to the content on disk.
//
// Both are written by the game AND by the updater, so they live here rather than in either
// one. A schema or field-order drift between two writers would make an interrupted commit
// read as a healthy installation on the next launch - which is the single failure the latch
// exists to prevent.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace ContentState
{
// Why an installation is marked incomplete. The value is written into the latch and shown in
// diagnostics, so it has to survive round-tripping through the file.
enum class Reason
{
    InstallCommit,
    UpdateCommit,
    RepairCommit,
    VerifyFailed,
    ManifestMissing,
    ManifestInvalid
};

const char* ReasonText(Reason reason);

// Written before the first mutation of `database\`, removed only once a full pass reports
// nothing outstanding. Returns false when the file could not be written - the caller must say
// so out loud, because an installation that cannot record its own incompleteness will present
// as healthy after a crash.
bool WriteLatch(const std::filesystem::path& path, const std::string& version, Reason reason);
bool LatchPresent(const std::filesystem::path& path);
void ClearLatch(const std::filesystem::path& path);

// Advisory cache of "this exact file was this hash". Trusted only on an exact
// (size, mtime) match whose recorded hash is the one the manifest wants, so a stale entry
// costs a rehash and never a wrong answer.
struct Entry
{
    std::string hash;
    std::uint64_t size{};
    std::uint64_t mtime{}; // decimal FILETIME
};

using Cache = std::map<std::string, Entry>;

// Both are best-effort. A missing or malformed state file yields an empty cache, which costs
// a full rescan and nothing else.
Cache LoadCache(const std::filesystem::path& path);
// The content-id the state file records, or empty when there is no readable state. Used to tell
// "the same content set as last time" from "a different release", which is the only thing that
// makes a previously rejected delta worth trying again.
std::string LoadContentId(const std::filesystem::path& path);
void SaveCache(const std::filesystem::path& path, const std::string& contentId, const Cache& cache);

// Last-write FILETIME as a decimal, or 0 when the file cannot be stat'ed. Zero never matches
// a cached entry, so an unreadable file always falls through to a real hash.
std::uint64_t FileTime(const std::filesystem::path& path);
}
