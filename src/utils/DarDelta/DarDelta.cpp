// Builds a .darpatch between two revisions of a content bundle.
//
// A release-time tool, never shipped to a player. It exists because a content update usually
// rewrites a handful of files inside one large archive: without a delta every player
// re-downloads the whole bundle for a small edit, and at several gigabytes of content that is
// the difference between an update people take and one they skip.
//
// The method is content-defined chunking. Splitting on fixed boundaries would work only until
// something changed length, after which every later boundary shifts and nothing matches; a
// rolling hash puts the boundaries where the DATA says, so an insertion perturbs one chunk and
// leaves the rest recognisable. Chunks of the base are indexed by a strong hash, the target is
// chunked the same way, and a hit becomes a COPY while a miss accumulates into an INSERT.
//
//   DarDelta.exe <base.xdb0> <target.xdb0> <out.darpatch>
//
// Exits non-zero on failure. It deliberately does NOT decide whether the delta is worth
// publishing - that is a ratio the publisher applies, because it depends on policy rather than
// on anything visible here.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace
{
// Chunk sizing. The average sets the index's granularity: smaller chunks match more precisely
// across an edit but cost more index entries and more per-chunk overhead in the output. 32 KiB
// against bundles of tens to hundreds of megabytes keeps the index in the low hundreds of
// thousands of entries while still catching a small edit inside a large archive.
constexpr std::size_t MinimumChunk = 8 * 1024;
constexpr std::size_t AverageChunk = 32 * 1024;
constexpr std::size_t MaximumChunk = 128 * 1024;

// A boundary is declared when the rolling hash has this many low bits clear, which happens on
// average once per AverageChunk bytes over random data.
constexpr std::uint64_t BoundaryMask = AverageChunk - 1;

// Gear table for the rolling hash. Generated from a fixed linear congruential sequence rather
// than shipped as a literal block: the only property that matters is that the values are well
// distributed and identical on every machine, and a seeded generator says that more clearly
// than 256 magic numbers would.
const std::uint64_t* gear_table()
{
    static std::uint64_t table[256];
    static bool ready = false;
    if (!ready)
    {
        std::uint64_t state = 0x9e3779b97f4a7c15ull;
        for (std::uint64_t& value : table)
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            value = state ^ (state >> 31);
        }
        ready = true;
    }
    return table;
}

struct Mapping
{
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE mapping{};
    const unsigned char* data{};
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

    bool Open(const std::wstring& path)
    {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file, &length) || !length.QuadPart)
            return false;
        size = static_cast<std::uint64_t>(length.QuadPart);
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping)
            return false;
        data = static_cast<const unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }
};

bool sha256(const void* data, std::size_t size, unsigned char (&digest)[32])
{
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0,
        static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), digest, 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0;
}

// Where the next chunk ends, starting at `offset`. Boundaries are content-defined within the
// min/max window, so an edit shifts at most the chunks it touches.
std::size_t next_boundary(const unsigned char* data, std::uint64_t size, std::uint64_t offset)
{
    const std::uint64_t remaining = size - offset;
    if (remaining <= MinimumChunk)
        return static_cast<std::size_t>(remaining);

    const std::uint64_t* gear = gear_table();
    const std::uint64_t limit = std::min<std::uint64_t>(remaining, MaximumChunk);
    std::uint64_t hash = 0;
    for (std::uint64_t index = MinimumChunk; index < limit; ++index)
    {
        hash = (hash << 1) + gear[data[offset + index]];
        if (!(hash & BoundaryMask))
            return static_cast<std::size_t>(index + 1);
    }
    return static_cast<std::size_t>(limit);
}

struct Key
{
    std::uint64_t words[4]{};
    bool operator==(const Key& other) const
    {
        return words[0] == other.words[0] && words[1] == other.words[1] &&
            words[2] == other.words[2] && words[3] == other.words[3];
    }
};

struct KeyHash
{
    std::size_t operator()(const Key& key) const { return static_cast<std::size_t>(key.words[0]); }
};

Key digest_key(const unsigned char (&digest)[32])
{
    Key key;
    std::memcpy(key.words, digest, sizeof(key.words));
    return key;
}

struct Chunk
{
    std::uint64_t offset{};
    std::uint32_t length{};
};

void write_u64(std::vector<unsigned char>& out, std::uint64_t value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}

void write_u32(std::vector<unsigned char>& out, std::uint32_t value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}

struct Operation
{
    bool copy{};
    std::uint64_t baseOffset{};
    std::uint64_t length{};
    std::uint64_t targetOffset{}; // for INSERT: where its bytes start in the target
};
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4)
    {
        std::fwprintf(stderr, L"usage: DarDelta <base> <target> <out.darpatch>\n");
        return 2;
    }

    Mapping base;
    Mapping target;
    if (!base.Open(argv[1]))
    {
        std::fwprintf(stderr, L"could not read the base bundle: %s\n", argv[1]);
        return 3;
    }
    if (!target.Open(argv[2]))
    {
        std::fwprintf(stderr, L"could not read the target bundle: %s\n", argv[2]);
        return 3;
    }

    unsigned char baseDigest[32]{};
    unsigned char targetDigest[32]{};
    if (!sha256(base.data, static_cast<std::size_t>(base.size), baseDigest) ||
        !sha256(target.data, static_cast<std::size_t>(target.size), targetDigest))
    {
        std::fwprintf(stderr, L"could not hash the bundles\n");
        return 4;
    }

    // Index the base. The last occurrence of a repeated chunk wins arbitrarily - any occurrence
    // reconstructs the same bytes, so which one is recorded does not matter.
    std::unordered_map<Key, Chunk, KeyHash> index;
    index.reserve(static_cast<std::size_t>(base.size / AverageChunk) + 16);
    for (std::uint64_t offset = 0; offset < base.size;)
    {
        const std::size_t length = next_boundary(base.data, base.size, offset);
        unsigned char digest[32]{};
        if (!sha256(base.data + offset, length, digest))
        {
            std::fwprintf(stderr, L"could not hash a base chunk\n");
            return 4;
        }
        index[digest_key(digest)] = {offset, static_cast<std::uint32_t>(length)};
        offset += length;
    }

    // Walk the target with the same chunker. A hit becomes a COPY; a miss extends the pending
    // INSERT, so a run of changed chunks costs one operation rather than one each.
    std::vector<Operation> operations;
    std::uint64_t pendingInsert = 0;
    std::uint64_t pendingInsertStart = 0;

    const auto flush_insert = [&]
    {
        if (!pendingInsert)
            return;
        operations.push_back({false, 0, pendingInsert, pendingInsertStart});
        pendingInsert = 0;
    };

    for (std::uint64_t offset = 0; offset < target.size;)
    {
        const std::size_t length = next_boundary(target.data, target.size, offset);
        unsigned char digest[32]{};
        if (!sha256(target.data + offset, length, digest))
        {
            std::fwprintf(stderr, L"could not hash a target chunk\n");
            return 4;
        }

        const auto found = index.find(digest_key(digest));
        if (found != index.end() && found->second.length == length)
        {
            flush_insert();
            // Merged with the previous COPY when it continues where that one ended: a whole
            // unchanged region becomes one operation instead of thousands.
            if (!operations.empty() && operations.back().copy &&
                operations.back().baseOffset + operations.back().length == found->second.offset)
            {
                operations.back().length += length;
            }
            else
            {
                operations.push_back({true, found->second.offset, length, offset});
            }
        }
        else
        {
            if (!pendingInsert)
                pendingInsertStart = offset;
            pendingInsert += length;
        }
        offset += length;
    }
    flush_insert();

    std::vector<unsigned char> out;
    out.reserve(static_cast<std::size_t>(target.size / 4) + 4096);
    out.insert(out.end(), {'D', 'A', 'R', 'P', 'A', 'T', 'C', 'H'});
    write_u32(out, 1);
    out.insert(out.end(), baseDigest, baseDigest + 32);
    write_u64(out, base.size);
    out.insert(out.end(), targetDigest, targetDigest + 32);
    write_u64(out, target.size);
    write_u64(out, operations.size());

    std::uint64_t produced = 0;
    for (const Operation& operation : operations)
    {
        if (operation.copy)
        {
            out.push_back(0);
            write_u64(out, operation.baseOffset);
            write_u64(out, operation.length);
        }
        else
        {
            out.push_back(1);
            write_u64(out, operation.length);
            out.insert(out.end(), target.data + operation.targetOffset,
                target.data + operation.targetOffset + operation.length);
        }
        produced += operation.length;
    }

    // The applier checks this too, but a builder that can emit a delta it knows is wrong is a
    // builder that will eventually publish one.
    if (produced != target.size)
    {
        std::fwprintf(stderr, L"the operations do not reconstruct the target\n");
        return 5;
    }

    const HANDLE output = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
    {
        std::fwprintf(stderr, L"could not write the delta: %s\n", argv[3]);
        return 6;
    }
    DWORD written = 0;
    const bool ok = WriteFile(output, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
        written == out.size();
    CloseHandle(output);
    if (!ok)
    {
        std::fwprintf(stderr, L"could not write the delta\n");
        return 6;
    }

    std::wprintf(L"%llu %llu %llu\n", static_cast<unsigned long long>(out.size()),
        static_cast<unsigned long long>(target.size),
        static_cast<unsigned long long>(operations.size()));
    return 0;
}
