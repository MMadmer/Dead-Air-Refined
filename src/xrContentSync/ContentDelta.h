// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// Binary deltas between two revisions of a bundle.
//
// A content release usually changes a handful of files inside one 300 MB bundle. Without deltas
// every player re-downloads all 300 MB for a 2 MB edit, and at five gigabytes of content that
// is the difference between an update people take and one they skip.
//
// The format is deliberately dull: a header naming both endpoints by hash and size, then a flat
// list of COPY and INSERT operations. There is no compression and no dictionary, because the
// bundles are already compressed archives and the interesting redundancy is between two
// revisions of the same file, not inside one.
//
//   u8[8]  "DARPATCH"
//   u32    version = 1
//   u8[32] baseSha256      u64 baseSize
//   u8[32] targetSha256    u64 targetSize
//   u64    opCount
//   opCount x { u8 kind; COPY: u64 baseOffset, u64 length | INSERT: u64 length, u8[length] }
//
// Every claim in that header is checked before a single output byte is written, and the output
// is hashed as it is produced. A delta can therefore be wrong, truncated or hostile and still
// only ever cost a wasted download - it can never produce a file that gets installed.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ContentDelta
{
inline constexpr char Magic[] = "DARPATCH";
inline constexpr std::uint32_t Version = 1;

enum class Op : std::uint8_t
{
    Copy = 0,
    Insert = 1
};

// Sanity bounds. A delta worth applying is a fraction of its target; anything beyond these is
// corrupt or hostile, not ambitious, and refusing early keeps a malformed header from making
// this process allocate its way out of memory.
inline constexpr std::uint64_t MaximumOps = 8u * 1024 * 1024;
inline constexpr std::uint64_t MaximumInsert = 256u * 1024 * 1024;
inline constexpr std::uint64_t MaximumSize = 8ull * 1024 * 1024 * 1024;

// Why an apply did not produce a usable target. The distinction decides whether the delta is
// recorded as bad or simply retried later, and getting that wrong is expensive in both
// directions: blacklisting a good delta upgrades a 20 MB download into a 400 MB one forever,
// and retrying a genuinely corrupt one loops.
enum class Failure
{
    None,
    // The delta itself is wrong: bad magic, impossible offsets, a hash that does not match with
    // every read and write having succeeded. Worth remembering.
    Integrity,
    // Something outside the delta went wrong - a disk filled up, a file could not be read, the
    // work was cancelled. Never remembered.
    Transient
};

struct Result
{
    bool ok{};
    Failure failure{Failure::None};
    std::string error;
};

// Applies `delta` to `base` and writes `target`, truncating whatever is already there. Callers
// pass a cache path named by the hash the output is supposed to have, so the only file that can
// be standing in the way is a leftover from an apply that was killed mid-write - overwriting it
// is the recovery, and refusing would jam that path until someone deleted the file by hand.
// The output is hashed as it is written and must match both the delta header's target hash and
// `expectedHash`; on any mismatch the output is removed and nothing is left behind.
//
// `base` is re-hashed here rather than trusted from the plan: time passes between deciding a
// delta is eligible and applying it, and the whole point of a delta is that it is meaningless
// against the wrong base.
Result Apply(const std::filesystem::path& delta, const std::filesystem::path& base,
    const std::filesystem::path& target, const std::string& expectedHash, const std::atomic_bool& cancel);

// Deltas that failed a hash verdict with all I/O succeeding, one asset name per line. Consulted
// by the resolver so a bad delta is not fetched twice, and cleared whenever a commit completes
// against a new content-id - a delta that was wrong for one release says nothing about the next.
void RecordRejected(const std::filesystem::path& path, const std::string& assetName);
bool IsRejected(const std::filesystem::path& path, const std::string& assetName);
void ClearRejected(const std::filesystem::path& path);
}
