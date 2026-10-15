// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

// Cuts the chunk map published beside a content bundle. A release-time tool, never shipped to a
// player.
//
//   DarDelta.exe --chunk-map <bundle.xdb0> <out.chunks>
//
// The map lists the bundle's content-defined chunks - length and hash, nothing else. A client
// cuts whatever revision of the slot it already holds with the same function
// (ContentChunks::NextBoundary), finds every chunk it has, and fetches only the rest of the
// published bundle by range: the delta between any two revisions, without anything published
// per pair of them.
//
// Exits non-zero on failure.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include "xrContentSync/ContentChunks.h"

#pragma comment(lib, "bcrypt.lib")

namespace
{
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

bool sha256(const void* data, std::uint64_t size, std::string& hex)
{
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    // BCryptHashData takes a ULONG, and a bundle may be several gigabytes.
    for (std::uint64_t offset = 0; ok && offset < size;)
    {
        const ULONG piece = static_cast<ULONG>(std::min<std::uint64_t>(size - offset, 1u << 30));
        ok = BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)) + offset, piece, 0) >= 0;
        offset += piece;
    }
    unsigned char digest[32]{};
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    static constexpr char digits[] = "0123456789abcdef";
    hex.clear();
    for (const unsigned char byte : digest)
    {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    return ok;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4 || std::wstring_view(argv[1]) != L"--chunk-map")
    {
        std::fwprintf(stderr, L"usage: DarDelta --chunk-map <bundle.xdb0> <out.chunks>\n");
        return 2;
    }

    Mapping bundle;
    if (!bundle.Open(argv[2]))
    {
        std::fwprintf(stderr, L"could not read the bundle: %s\n", argv[2]);
        return 3;
    }

    ContentChunks::Map map;
    map.targetSize = bundle.size;
    if (!sha256(bundle.data, bundle.size, map.targetHash))
    {
        std::fwprintf(stderr, L"could not hash the bundle\n");
        return 4;
    }

    std::string error;
    if (!ContentChunks::ChunkBuffer(bundle.data, bundle.size, map.chunks, error) ||
        !ContentChunks::WriteMap(argv[3], map, error))
    {
        std::fwprintf(stderr, L"could not write the chunk map: %hs\n", error.c_str());
        return 6;
    }
    std::wprintf(L"%llu %llu\n", static_cast<unsigned long long>(map.chunks.size()),
        static_cast<unsigned long long>(bundle.size));
    return 0;
}
