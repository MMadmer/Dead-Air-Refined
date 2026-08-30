#include "ContentHash.h"

#include <algorithm>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace
{
class Digest
{
public:
    ~Digest()
    {
        if (m_hash)
            BCryptDestroyHash(m_hash);
        if (m_algorithm)
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }

    bool Open()
    {
        DWORD written = 0;
        if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(m_algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&m_objectLength),
                sizeof(m_objectLength), &written, 0) < 0 ||
            BCryptGetProperty(m_algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&m_digestLength),
                sizeof(m_digestLength), &written, 0) < 0)
        {
            return false;
        }
        m_object.resize(m_objectLength);
        return BCryptCreateHash(m_algorithm, &m_hash, m_object.data(), m_objectLength, nullptr, 0, 0) >= 0;
    }

    bool Append(const void* data, DWORD size)
    {
        return BCryptHashData(m_hash, static_cast<PUCHAR>(const_cast<void*>(data)), size, 0) >= 0;
    }

    std::string Finish()
    {
        std::vector<unsigned char> digest(m_digestLength);
        if (BCryptFinishHash(m_hash, digest.data(), m_digestLength, 0) < 0)
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

private:
    BCRYPT_ALG_HANDLE m_algorithm{};
    BCRYPT_HASH_HANDLE m_hash{};
    DWORD m_objectLength{};
    DWORD m_digestLength{};
    std::vector<unsigned char> m_object;
};
}

namespace ContentHash
{
std::string File(const std::wstring& path, const std::atomic_bool& cancel)
{
    // Sequential-scan hint and no share-write: a bundle being hashed must not be a bundle
    // someone else is mid-write on, or the digest describes a file that no longer exists.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    Digest digest;
    bool success = digest.Open();
    std::vector<unsigned char> buffer(1024 * 1024);
    while (success)
    {
        if (cancel.load(std::memory_order_acquire))
        {
            success = false;
            break;
        }

        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            success = false;
            break;
        }
        if (!read)
            break;
        success = digest.Append(buffer.data(), read);
    }
    CloseHandle(file);
    return success ? digest.Finish() : std::string{};
}


std::string Buffer(const void* data, std::size_t size)
{
    Digest digest;
    if (!digest.Open())
        return {};

    // BCryptHashData takes a DWORD; feed it in chunks so a large buffer cannot silently
    // truncate into a wrong-but-plausible digest.
    const auto* cursor = static_cast<const unsigned char*>(data);
    while (size)
    {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 64u * 1024 * 1024));
        if (!digest.Append(cursor, chunk))
            return {};
        cursor += chunk;
        size -= chunk;
    }
    return digest.Finish();
}
}
