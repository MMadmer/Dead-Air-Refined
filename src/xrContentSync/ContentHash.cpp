// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "ContentHash.h"

#include <algorithm>
#include <vector>

#define WIN32_LEAN_AND_MEAN
// std::min / std::max are used throughout; the windows.h macros of the same name would
// swallow them.
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace
{
// One read size for everything. Large enough that a 300 MB bundle is a few hundred syscalls,
// small enough that a cancelled hash notices within a few milliseconds.
constexpr DWORD ReadChunk = 1024 * 1024;

// Hashes `limit` bytes of the file, or the whole file when `limit` is absent. Returns empty on
// any failure, including a file shorter than `limit` - a short file is not a prefix.
std::string hash_file(const std::wstring& path, const std::uint64_t* limit, const std::atomic_bool& cancel)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    ContentHash::Stream digest;
    bool success = digest.Open();
    std::uint64_t remaining = limit ? *limit : 0;
    std::vector<unsigned char> buffer(ReadChunk);
    while (success)
    {
        if (cancel.load(std::memory_order_acquire))
        {
            success = false;
            break;
        }
        if (limit && !remaining)
            break;

        DWORD want = ReadChunk;
        if (limit && remaining < want)
            want = static_cast<DWORD>(remaining);

        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), want, &read, nullptr))
        {
            success = false;
            break;
        }
        if (!read)
        {
            // End of file. With a limit still outstanding the file is shorter than the caller
            // believes, which is a failure rather than a short answer.
            success = !limit;
            break;
        }
        success = digest.Append(buffer.data(), read);
        if (limit)
            remaining -= read;
    }
    CloseHandle(file);
    return success ? digest.Finish() : std::string{};
}
}

namespace ContentHash
{
struct Stream::Impl
{
    ~Impl()
    {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength{};
    DWORD digestLength{};
    std::vector<unsigned char> object;
};

Stream::Stream() : m_impl(std::make_unique<Impl>()) {}
Stream::~Stream() = default;
Stream::Stream(Stream&&) noexcept = default;
Stream& Stream::operator=(Stream&&) noexcept = default;

bool Stream::Open()
{
    if (!m_impl)
        return false;
    Impl& impl = *m_impl;
    DWORD written = 0;
    if (BCryptOpenAlgorithmProvider(&impl.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(impl.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&impl.objectLength),
            sizeof(impl.objectLength), &written, 0) < 0 ||
        BCryptGetProperty(impl.algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&impl.digestLength),
            sizeof(impl.digestLength), &written, 0) < 0)
    {
        return false;
    }
    impl.object.resize(impl.objectLength);
    return BCryptCreateHash(impl.algorithm, &impl.hash, impl.object.data(), impl.objectLength, nullptr, 0, 0) >= 0;
}

bool Stream::Append(const void* data, std::size_t size)
{
    if (!m_impl)
        return false;
    Impl& impl = *m_impl;
    if (!impl.hash)
        return false;

    // BCryptHashData takes a DWORD, so a caller handing over a multi-gigabyte buffer must be
    // chunked here rather than silently truncated into a wrong-but-plausible digest.
    const auto* cursor = static_cast<const unsigned char*>(data);
    while (size)
    {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 64u * 1024 * 1024));
        if (BCryptHashData(impl.hash, const_cast<PUCHAR>(cursor), chunk, 0) < 0)
            return false;
        cursor += chunk;
        size -= chunk;
    }
    return true;
}

std::string Stream::Finish()
{
    if (!m_impl)
        return {};
    Impl& impl = *m_impl;
    if (!impl.hash)
        return {};

    std::vector<unsigned char> digest(impl.digestLength);
    if (BCryptFinishHash(impl.hash, digest.data(), impl.digestLength, 0) < 0)
        return {};

    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const unsigned char byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

std::string File(const std::wstring& path, const std::atomic_bool& cancel)
{
    return hash_file(path, nullptr, cancel);
}

std::string FilePrefix(const std::wstring& path, std::uint64_t bytes, const std::atomic_bool& cancel)
{
    return hash_file(path, &bytes, cancel);
}

std::string Buffer(const void* data, std::size_t size)
{
    Stream digest;
    if (!digest.Open() || !digest.Append(data, size))
        return {};
    return digest.Finish();
}
}
