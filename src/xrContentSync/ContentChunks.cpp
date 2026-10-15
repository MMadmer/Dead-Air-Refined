// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "ContentChunks.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
// std::min / std::max are used throughout; the windows.h macros of the same name would
// swallow them.
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace
{
constexpr std::uint64_t BoundaryMask = ContentChunks::AverageChunk - 1;

// A local run shorter than this between two fetched ones is fetched too: one more request
// costs more than the bytes it would save.
constexpr std::uint64_t BridgeBytes = 64 * 1024;

// Range requests per bundle. A bundle that would need more has its gaps widened until it fits,
// trading a few unchanged megabytes for not spending minutes on round trips.
constexpr std::size_t MaximumRanges = 256;

// Bases cut this process, by path, size and write time. Bounded: a full content set is a few
// dozen bundles.
constexpr std::size_t MemoEntries = 128;

// Gear table of the rolling hash. Generated from a fixed linear congruential sequence rather
// than shipped as a literal block: the only properties that matter are that the values are well
// distributed and identical on every machine - the builder and the client have to cut at the
// same places.
const std::uint64_t* gear_table()
{
    static const auto table = []
    {
        std::array<std::uint64_t, 256> values{};
        std::uint64_t state = 0x9e3779b97f4a7c15ull;
        for (std::uint64_t& value : values)
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            value = state ^ (state >> 31);
        }
        return values;
    }();
    return table.data();
}

class Sha256
{
public:
    Sha256() { m_ready = BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0; }
    ~Sha256()
    {
        if (m_algorithm)
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool Key(const std::uint8_t* data, std::uint32_t size, ContentChunks::Key& key) const
    {
        std::uint8_t digest[32]{};
        if (!m_ready || BCryptHash(m_algorithm, nullptr, 0, const_cast<PUCHAR>(data), size, digest, sizeof(digest)) < 0)
            return false;
        std::memcpy(key.data(), digest, key.size());
        return true;
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm{};
    bool m_ready{};
};

struct KeyHash
{
    std::size_t operator()(const ContentChunks::Key& key) const
    {
        std::size_t value = 0;
        std::memcpy(&value, key.data(), sizeof(value));
        return value;
    }
};

struct Mapping
{
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE mapping{};
    const std::uint8_t* data{};
    std::uint64_t size{};

    ~Mapping()
    {
        if (data)
            UnmapViewOfFile(data);
        if (mapping)
            CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }

    bool Open(const std::filesystem::path& path)
    {
        // Shared for deletion too: the commit may rename this very file into the cache later,
        // and a reader that outlived its pass must not be the reason that fails.
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file, &length))
            return false;
        size = static_cast<std::uint64_t>(length.QuadPart);
        if (!size)
            return true;
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping)
            return false;
        data = static_cast<const std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }
};

bool cut(const std::uint8_t* data, std::uint64_t size, std::vector<ContentChunks::Chunk>& chunks,
    const std::atomic_bool* cancel, std::string& error)
{
    chunks.clear();
    chunks.reserve(static_cast<std::size_t>(size / ContentChunks::AverageChunk) + 16);
    const Sha256 sha;
    for (std::uint64_t offset = 0; offset < size;)
    {
        // Polled per few megabytes, not per chunk: an atomic load per 32 KiB is noise, but there
        // is no reason to pay it.
        if (cancel && !(chunks.size() & 127) && cancel->load(std::memory_order_acquire))
        {
            error = "cancelled";
            return false;
        }
        ContentChunks::Chunk chunk;
        chunk.offset = offset;
        chunk.length = ContentChunks::NextBoundary(data, size, offset);
        if (!sha.Key(data + offset, chunk.length, chunk.key))
        {
            error = "the hashing provider failed";
            return false;
        }
        chunks.push_back(chunk);
        offset += chunk.length;
    }
    return true;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int index = 0; index != 4; ++index)
        out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (int index = 0; index != 8; ++index)
        out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

std::uint32_t get_u32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8 |
        static_cast<std::uint32_t>(data[2]) << 16 | static_cast<std::uint32_t>(data[3]) << 24;
}

std::uint64_t get_u64(const std::uint8_t* data)
{
    return static_cast<std::uint64_t>(get_u32(data)) | static_cast<std::uint64_t>(get_u32(data + 4)) << 32;
}

bool hex_to_bytes(std::string_view hex, std::uint8_t* out, std::size_t bytes)
{
    if (hex.size() != bytes * 2)
        return false;
    const auto nibble = [](char character) -> int
    {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index != bytes; ++index)
    {
        const int high = nibble(hex[index * 2]);
        const int low = nibble(hex[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        out[index] = static_cast<std::uint8_t>(high << 4 | low);
    }
    return true;
}
}

namespace ContentChunks
{
std::string MapName(std::string_view bundleName) { return std::string(bundleName) + ".chunks"; }

std::uint32_t NextBoundary(const std::uint8_t* data, std::uint64_t size, std::uint64_t offset)
{
    const std::uint64_t remaining = size - offset;
    if (remaining <= MinimumChunk)
        return static_cast<std::uint32_t>(remaining);

    const std::uint64_t* gear = gear_table();
    const std::uint64_t limit = std::min<std::uint64_t>(remaining, MaximumChunk);
    std::uint64_t hash = 0;
    for (std::uint64_t index = MinimumChunk; index < limit; ++index)
    {
        hash = (hash << 1) + gear[data[offset + index]];
        if (!(hash & BoundaryMask))
            return static_cast<std::uint32_t>(index + 1);
    }
    return static_cast<std::uint32_t>(limit);
}

bool ChunkBuffer(const std::uint8_t* data, std::uint64_t size, std::vector<Chunk>& chunks, std::string& error)
{
    return cut(data, size, chunks, nullptr, error);
}

bool ChunkFile(const std::filesystem::path& path, std::vector<Chunk>& chunks, const std::atomic_bool& cancel,
    std::string& error)
{
    static std::mutex memoMutex;
    static std::map<std::wstring, std::shared_ptr<const std::vector<Chunk>>> memo;

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
    {
        error = "the file could not be examined";
        return false;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32 | attributes.nFileSizeLow;
    const std::uint64_t written = static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32 |
        attributes.ftLastWriteTime.dwLowDateTime;
    const std::wstring key = path.wstring() + L"|" + std::to_wstring(size) + L"|" + std::to_wstring(written);
    {
        std::lock_guard guard(memoMutex);
        if (const auto found = memo.find(key); found != memo.end())
        {
            chunks = *found->second;
            return true;
        }
    }

    Mapping file;
    if (!file.Open(path))
    {
        error = "the file could not be read";
        return false;
    }
    if (!cut(file.data, file.size, chunks, &cancel, error))
        return false;

    std::lock_guard guard(memoMutex);
    if (memo.size() >= MemoEntries)
        memo.erase(memo.begin());
    memo[key] = std::make_shared<const std::vector<Chunk>>(chunks);
    return true;
}

bool ParseMap(const void* data, std::size_t size, Map& map, std::string& error)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (size < HeaderBytes || size > MaximumMapBytes)
    {
        error = "the chunk map has an impossible size";
        return false;
    }
    if (std::memcmp(bytes, Magic, 8) != 0 || get_u32(bytes + 8) != Version)
    {
        error = "the chunk map has an unknown format";
        return false;
    }
    if (get_u32(bytes + 12) != MinimumChunk || get_u32(bytes + 16) != AverageChunk || get_u32(bytes + 20) != MaximumChunk)
    {
        error = "the chunk map was cut with other chunk sizes";
        return false;
    }

    Map parsed;
    static constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index != 32; ++index)
    {
        parsed.targetHash.push_back(hex[bytes[24 + index] >> 4]);
        parsed.targetHash.push_back(hex[bytes[24 + index] & 0x0f]);
    }
    parsed.targetSize = get_u64(bytes + 56);
    const std::uint64_t count = get_u64(bytes + 64);
    if (!parsed.targetSize || !count || count > (size - HeaderBytes) / RecordBytes ||
        HeaderBytes + count * RecordBytes != size)
    {
        error = "the chunk map is truncated or padded";
        return false;
    }

    parsed.chunks.resize(static_cast<std::size_t>(count));
    std::uint64_t offset = 0;
    for (std::size_t index = 0; index != parsed.chunks.size(); ++index)
    {
        const std::uint8_t* record = bytes + HeaderBytes + index * RecordBytes;
        Chunk& chunk = parsed.chunks[index];
        chunk.offset = offset;
        chunk.length = get_u32(record);
        std::memcpy(chunk.key.data(), record + 4, KeyBytes);
        if (!chunk.length || chunk.length > MaximumChunk || chunk.length > parsed.targetSize - offset)
        {
            error = "the chunk map describes a chunk outside the bundle";
            return false;
        }
        offset += chunk.length;
    }
    if (offset != parsed.targetSize)
    {
        error = "the chunk map does not cover the bundle";
        return false;
    }
    map = std::move(parsed);
    return true;
}

bool WriteMap(const std::filesystem::path& path, const Map& map, std::string& error)
{
    std::vector<std::uint8_t> out;
    out.reserve(HeaderBytes + map.chunks.size() * RecordBytes);
    out.insert(out.end(), Magic, Magic + 8);
    put_u32(out, Version);
    put_u32(out, MinimumChunk);
    put_u32(out, AverageChunk);
    put_u32(out, MaximumChunk);
    std::uint8_t digest[32]{};
    if (!hex_to_bytes(map.targetHash, digest, sizeof(digest)))
    {
        error = "the target hash is not 64 lowercase hex";
        return false;
    }
    out.insert(out.end(), digest, digest + sizeof(digest));
    put_u64(out, map.targetSize);
    put_u64(out, map.chunks.size());
    for (const Chunk& chunk : map.chunks)
    {
        put_u32(out, chunk.length);
        out.insert(out.end(), chunk.key.begin(), chunk.key.end());
    }

    // Through a temporary: a map cut short by an interrupted run must never be found under the
    // map's name and published, since a published asset is never replaced.
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "the chunk map could not be created";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) && written == out.size() &&
        FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        error = "the chunk map could not be written";
        return false;
    }
    return true;
}

bool PlanTarget(const Map& map, const std::vector<std::filesystem::path>& bases, std::uint64_t from, Plan& plan,
    const std::atomic_bool& cancel, std::string& error)
{
    plan = {};
    plan.bases = bases;
    if (from >= map.targetSize)
        return true;

    // Where each target chunk can come from. Duplicates are common - runs of zeros, repeated
    // headers - so one key may stand for several target chunks.
    std::unordered_map<Key, std::vector<std::uint32_t>, KeyHash> wanted;
    wanted.reserve(map.chunks.size());
    for (std::size_t index = 0; index != map.chunks.size(); ++index)
    {
        if (map.chunks[index].offset + map.chunks[index].length > from)
            wanted[map.chunks[index].key].push_back(static_cast<std::uint32_t>(index));
    }

    struct Source
    {
        int base{-1};
        std::uint64_t offset{};
    };
    std::vector<Source> sources(map.chunks.size());
    std::size_t unresolved = wanted.size();

    for (std::size_t baseIndex = 0; baseIndex != bases.size() && unresolved; ++baseIndex)
    {
        std::vector<Chunk> chunks;
        std::string cutError;
        if (!ChunkFile(bases[baseIndex], chunks, cancel, cutError))
        {
            if (cancel.load(std::memory_order_acquire))
            {
                error = "cancelled";
                return false;
            }
            continue;
        }
        for (const Chunk& chunk : chunks)
        {
            const auto found = wanted.find(chunk.key);
            if (found == wanted.end() || sources[found->second.front()].base >= 0)
                continue;
            // The key is half a SHA-256, but a length that disagrees is not the same chunk.
            if (map.chunks[found->second.front()].length != chunk.length)
                continue;
            for (const std::uint32_t target : found->second)
                sources[target] = {static_cast<int>(baseIndex), chunk.offset};
            --unresolved;
        }
    }

    // Runs, merged as they are laid down: consecutive fetched chunks become one range, and
    // consecutive copies from the same place in the same base become one copy.
    std::vector<Segment> runs;
    for (std::size_t index = 0; index != map.chunks.size(); ++index)
    {
        const Chunk& chunk = map.chunks[index];
        if (chunk.offset + chunk.length <= from)
            continue;
        const std::uint64_t skip = from > chunk.offset ? from - chunk.offset : 0;
        Segment segment;
        segment.targetOffset = chunk.offset + skip;
        segment.length = chunk.length - skip;
        segment.base = sources[index].base;
        segment.baseOffset = sources[index].offset + skip;

        if (!runs.empty())
        {
            Segment& last = runs.back();
            const bool continues = last.targetOffset + last.length == segment.targetOffset;
            if (continues && last.Remote() && segment.Remote())
            {
                last.length += segment.length;
                continue;
            }
            if (continues && !last.Remote() && last.base == segment.base &&
                last.baseOffset + last.length == segment.baseOffset)
            {
                last.length += segment.length;
                continue;
            }
        }
        runs.push_back(segment);
    }

    // A short copy between two fetches is fetched as well. Widened until the bundle fits the
    // request budget.
    for (std::uint64_t bridge = BridgeBytes;; bridge *= 2)
    {
        std::vector<Segment> merged;
        for (std::size_t index = 0; index != runs.size(); ++index)
        {
            Segment segment = runs[index];
            const bool between = !segment.Remote() && segment.length < bridge && !merged.empty() &&
                merged.back().Remote() && index + 1 != runs.size() && runs[index + 1].Remote();
            if (between)
            {
                segment.base = -1;
                segment.baseOffset = 0;
            }
            if (!merged.empty() && merged.back().Remote() && segment.Remote())
                merged.back().length += segment.length;
            else
                merged.push_back(segment);
        }

        const std::size_t ranges = static_cast<std::size_t>(
            std::ranges::count_if(merged, [](const Segment& segment) { return segment.Remote(); }));
        if (ranges <= MaximumRanges || bridge >= map.targetSize)
        {
            plan.segments = std::move(merged);
            plan.ranges = ranges;
            break;
        }
    }

    for (const Segment& segment : plan.segments)
        (segment.Remote() ? plan.remoteBytes : plan.localBytes) += segment.length;
    return true;
}
}
