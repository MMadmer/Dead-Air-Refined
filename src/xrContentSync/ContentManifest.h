#pragma once

// Reader for the content manifest - the file that pins a game version to the exact set of
// content bundles it requires.
//
// This translation unit is compiled into xrCore (for the mount gate), into xrGame (for the
// content service) and into the external updater. One parser, three consumers: two readers
// held together by a test fixture is how the two halves of a format quietly drift apart.
// It therefore depends on nothing but the standard library - no xrCore types, no allocators.

#include <cstdint>
#include <string>
#include <vector>

namespace ContentManifest
{
inline constexpr char SchemaContent[] = "dead-air-refined.content/1";
inline constexpr char SchemaState[] = "dead-air-refined.content-state/1";
inline constexpr char SchemaIncomplete[] = "dead-air-refined.content-incomplete/1";

// Sanity bounds. Content is expected to reach a few thousand bundles at the very most; a
// manifest beyond this is corrupt or hostile, not ambitious.
inline constexpr size_t MaximumBundles = 4096;
inline constexpr size_t MaximumManifestBytes = 4u * 1024 * 1024;

struct Bundle
{
    std::string hash;      // SHA-256 of the packed .xdb0, 64 lowercase hex
    std::string name;      // file name, no path
    std::string releaseTag;
    std::uint64_t size{};
};

// A published binary delta from one revision of a bundle to another. Cumulative: every edge
// ever published for a bundle that still exists stays listed, so a player who skipped
// releases can still be walked from whatever revision they actually hold.
struct Delta
{
    std::string hash;        // SHA-256 of the delta asset itself
    std::string name;        // delta asset file name
    std::string releaseTag;
    std::string baseName;    // bundle file name the delta applies to
    std::string baseHash;
    std::string targetHash;  // a Bundle::hash, or the base of another delta that continues the chain
    std::uint64_t size{};
    std::uint64_t baseSize{};
};

struct Manifest
{
    std::string version;
    std::string contentId;
    std::string repo;
    std::vector<Bundle> bundles;
    std::vector<Delta> deltas;

    const Bundle* FindByName(std::string_view name) const;
    const Bundle* FindByHash(std::string_view hash) const;

    // Group + shard identify a slot; the hash in the name identifies the revision filling it.
    // Finding the slot but not the name is what separates "stale bundle from an older release"
    // from "file we have never heard of", and those deserve different diagnostics.
    const Bundle* FindBySlot(std::string_view group, std::string_view shard) const;
};

// The three parts of a bundle file name. They are views into the name that was passed in, so
// they live exactly as long as it does.
struct BundleName
{
    std::string_view group;
    std::string_view shard;   // two digits
    std::string_view hash16;  // first 16 hex of the file hash
};

// A bundle file name must match
//   xtra_dead_air_x64_content_<group>_<NN>_<16 hex>.xdb0
// exactly. This is not cosmetic: it is the delete authority. The uninstaller and the cache
// collector sweep `database\` by this pattern, so anything it accepts is something they may
// remove, and a looser pattern would let them eat a third-party archive.
bool SplitBundleName(std::string_view name, BundleName& parts);

inline bool IsBundleName(std::string_view name)
{
    BundleName parts;
    return SplitBundleName(name, parts);
}

// Parses the manifest text. Returns false and leaves `manifest` untouched on any deviation -
// there is no partial success, because a half-read manifest would understate what an
// installation needs and present a broken install as healthy.
bool Parse(std::string_view text, Manifest& manifest, std::string& error);

// Convenience: read the file at `path` and parse it. Missing file, unreadable file and
// malformed content are all failures, distinguished only by `error`.
bool ParseFile(const std::wstring& path, Manifest& manifest, std::string& error);

// Lowercase hex check, used by every consumer that accepts a hash from data.
bool IsSha256Hex(std::string_view value);
}
