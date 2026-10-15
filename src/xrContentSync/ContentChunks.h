// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// Rebuilding a bundle from the revision an installation already holds.
//
// A content release usually rewrites a handful of files inside a large archive. The published
// bundle is cut into content-defined chunks and the list of them - length and hash, nothing
// else - is published beside it as a chunk map. A client cuts whatever revision of that slot it
// has with the same function, finds every chunk it already holds, and fetches the rest straight
// out of the published bundle with HTTP Range requests. Nothing is published per pair of
// releases, so the distance between the installed revision and the wanted one does not matter:
// one release behind or ten, the download is what actually differs.
//
// The map is not trusted. It only says where to look; the rebuilt file is hashed as it is
// written and must match the manifest, so a wrong or hostile map costs a wasted download and
// can never produce a file that gets installed.
//
//   u8[8]  "DARCHUNK"
//   u32    version = 1
//   u32    minimum chunk, u32 average chunk, u32 maximum chunk
//   u8[32] target SHA-256      u64 target size
//   u64    chunk count
//   count x { u32 length; u8[16] first half of the chunk's SHA-256 }
//
// Little-endian, no padding. A map cut with other chunk sizes is refused rather than half
// matched: the two sides only find each other when they cut at the same places.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ContentChunks
{
// The average sets the granularity: smaller chunks match more precisely across an edit but cost
// more map entries and more range requests. 32 KiB against bundles of hundreds of megabytes
// keeps a map well under a megabyte while still catching a small edit inside a large archive.
inline constexpr std::uint32_t MinimumChunk = 8 * 1024;
inline constexpr std::uint32_t AverageChunk = 32 * 1024;
inline constexpr std::uint32_t MaximumChunk = 128 * 1024;

inline constexpr char Magic[] = "DARCHUNK";
inline constexpr std::uint32_t Version = 1;
inline constexpr std::size_t KeyBytes = 16;
inline constexpr std::size_t HeaderBytes = 72;
inline constexpr std::size_t RecordBytes = 4 + KeyBytes;

// A map past this is corrupt or hostile: it would describe a bundle far beyond the 4 GiB the
// mount gate accepts.
inline constexpr std::uint64_t MaximumMapBytes = 32ull * 1024 * 1024;

using Key = std::array<std::uint8_t, KeyBytes>;

struct Chunk
{
    std::uint64_t offset{};
    std::uint32_t length{};
    Key key{};
};

// The asset a bundle's chunk map is published as, under the bundle's own release tag.
std::string MapName(std::string_view bundleName);

// Where the chunk starting at `offset` ends. The boundary is content-defined inside the
// min/max window, so an edit moves only the boundaries it touches. The builder and every
// client cut with this one function.
std::uint32_t NextBoundary(const std::uint8_t* data, std::uint64_t size, std::uint64_t offset);

// Cuts a file into chunks and hashes each. Kept per process by path, size and write time, so a
// base the update check already cut is not read again by the download that follows it.
bool ChunkFile(const std::filesystem::path& path, std::vector<Chunk>& chunks, const std::atomic_bool& cancel,
    std::string& error);

// Cuts a whole buffer: the builder's side.
bool ChunkBuffer(const std::uint8_t* data, std::uint64_t size, std::vector<Chunk>& chunks, std::string& error);

struct Map
{
    std::string targetHash; // lowercase hex
    std::uint64_t targetSize{};
    std::vector<Chunk> chunks;
};

// Every claim is checked before the map is used: the header, the chunk sizes, the lengths
// summing to the target size. Offsets are rebuilt from the lengths.
bool ParseMap(const void* data, std::size_t size, Map& map, std::string& error);
bool WriteMap(const std::filesystem::path& path, const Map& map, std::string& error);

// A run of the target: copied from a base file, or fetched from the published bundle.
struct Segment
{
    std::uint64_t targetOffset{};
    std::uint64_t length{};
    int base{-1};
    std::uint64_t baseOffset{};

    bool Remote() const { return base < 0; }
};

struct Plan
{
    std::vector<std::filesystem::path> bases;
    std::vector<Segment> segments;
    std::uint64_t remoteBytes{};
    std::uint64_t localBytes{};
    std::size_t ranges{};
};

// Plans the target from `from` onward - a resumed part already holds everything before it.
// Bases that cannot be read are skipped, not fatal: the worst outcome is a plan that fetches
// everything.
bool PlanTarget(const Map& map, const std::vector<std::filesystem::path>& bases, std::uint64_t from, Plan& plan,
    const std::atomic_bool& cancel, std::string& error);
}
