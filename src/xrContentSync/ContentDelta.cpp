// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "ContentDelta.h"

#include "ContentHash.h"
#include "ContentManifest.h"

#include <algorithm>
#include <fstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{
constexpr DWORD TransferBuffer = 1024 * 1024;

// Reads exactly `size` bytes or fails. A short read here is not a smaller answer, it is a
// truncated delta, and treating it as either would be wrong.
bool read_exact(HANDLE file, void* buffer, DWORD size)
{
    auto* cursor = static_cast<unsigned char*>(buffer);
    while (size)
    {
        DWORD read = 0;
        if (!ReadFile(file, cursor, size, &read, nullptr) || !read)
            return false;
        cursor += read;
        size -= read;
    }
    return true;
}

bool read_u64(HANDLE file, std::uint64_t& value) { return read_exact(file, &value, sizeof(value)); }
bool read_u32(HANDLE file, std::uint32_t& value) { return read_exact(file, &value, sizeof(value)); }

std::string to_hex(const unsigned char (&digest)[32])
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const unsigned char byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

struct Header
{
    std::string baseHash;
    std::string targetHash;
    std::uint64_t baseSize{};
    std::uint64_t targetSize{};
    std::uint64_t opCount{};
};

bool read_header(HANDLE file, Header& header)
{
    char magic[8]{};
    std::uint32_t version = 0;
    unsigned char baseDigest[32]{};
    unsigned char targetDigest[32]{};

    if (!read_exact(file, magic, sizeof(magic)) || std::memcmp(magic, ContentDelta::Magic, 8) != 0)
        return false;
    if (!read_u32(file, version) || version != ContentDelta::Version)
        return false;
    if (!read_exact(file, baseDigest, sizeof(baseDigest)) || !read_u64(file, header.baseSize))
        return false;
    if (!read_exact(file, targetDigest, sizeof(targetDigest)) || !read_u64(file, header.targetSize))
        return false;
    if (!read_u64(file, header.opCount))
        return false;

    header.baseHash = to_hex(baseDigest);
    header.targetHash = to_hex(targetDigest);
    return header.baseSize <= ContentDelta::MaximumSize &&
        header.targetSize && header.targetSize <= ContentDelta::MaximumSize &&
        header.opCount && header.opCount <= ContentDelta::MaximumOps;
}
}

namespace ContentDelta
{
Result Apply(const std::filesystem::path& delta, const std::filesystem::path& base,
    const std::filesystem::path& target, const std::string& expectedHash, const std::atomic_bool& cancel)
{
    Result result;
    const auto fail = [&](Failure kind, const char* message)
    {
        result.failure = kind;
        result.error = message;
        return result;
    };

    const HANDLE deltaFile = CreateFileW(delta.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (deltaFile == INVALID_HANDLE_VALUE)
        return fail(Failure::Transient, "the delta could not be opened");

    Header header;
    if (!read_header(deltaFile, header))
    {
        CloseHandle(deltaFile);
        return fail(Failure::Integrity, "the delta header is not valid");
    }

    // The base is re-hashed here, not taken on trust from whatever decided this delta was
    // eligible. Time has passed since then, and a delta applied to the wrong base produces
    // garbage that is only caught at the very end, after all the work.
    const std::string actualBase = ContentHash::File(base.wstring(), cancel);
    if (cancel.load(std::memory_order_acquire))
    {
        CloseHandle(deltaFile);
        return fail(Failure::Transient, "cancelled");
    }
    if (actualBase != header.baseHash)
    {
        CloseHandle(deltaFile);
        return fail(Failure::Transient, "the installed bundle is not the base this delta expects");
    }

    std::error_code sizeError;
    if (std::filesystem::file_size(base, sizeError) != header.baseSize || sizeError)
    {
        CloseHandle(deltaFile);
        return fail(Failure::Transient, "the installed bundle is not the size this delta expects");
    }

    const HANDLE baseFile = CreateFileW(base.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (baseFile == INVALID_HANDLE_VALUE)
    {
        CloseHandle(deltaFile);
        return fail(Failure::Transient, "the installed bundle could not be read");
    }

    std::error_code createError;
    std::filesystem::create_directories(target.parent_path(), createError);
    const HANDLE targetFile = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (targetFile == INVALID_HANDLE_VALUE)
    {
        CloseHandle(baseFile);
        CloseHandle(deltaFile);
        return fail(Failure::Transient, "the content cache could not be written");
    }

    ContentHash::Stream digest;
    Failure failure = Failure::None;
    const char* message = "";
    std::uint64_t produced = 0;
    std::vector<unsigned char> buffer(TransferBuffer);

    if (!digest.Open())
    {
        failure = Failure::Transient;
        message = "the hashing provider could not start";
    }

    for (std::uint64_t index = 0; index < header.opCount && failure == Failure::None; ++index)
    {
        if (cancel.load(std::memory_order_acquire))
        {
            failure = Failure::Transient;
            message = "cancelled";
            break;
        }

        std::uint8_t kind = 0;
        if (!read_exact(deltaFile, &kind, sizeof(kind)))
        {
            failure = Failure::Integrity;
            message = "the delta ended early";
            break;
        }

        if (kind == static_cast<std::uint8_t>(Op::Copy))
        {
            std::uint64_t offset = 0;
            std::uint64_t length = 0;
            if (!read_u64(deltaFile, offset) || !read_u64(deltaFile, length))
            {
                failure = Failure::Integrity;
                message = "the delta ended early";
                break;
            }
            // Checked before the seek, not after: an operation that reaches past the base is a
            // malformed delta, and finding that out by reading garbage would be too late.
            if (!length || offset > header.baseSize || length > header.baseSize - offset ||
                length > header.targetSize - produced)
            {
                failure = Failure::Integrity;
                message = "the delta copies from outside the bundle";
                break;
            }

            LARGE_INTEGER seek{};
            seek.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(baseFile, seek, nullptr, FILE_BEGIN))
            {
                failure = Failure::Transient;
                message = "the installed bundle could not be read";
                break;
            }

            while (length && failure == Failure::None)
            {
                const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(length, TransferBuffer));
                DWORD read = 0;
                if (!ReadFile(baseFile, buffer.data(), want, &read, nullptr) || read != want)
                {
                    failure = Failure::Transient;
                    message = "the installed bundle could not be read";
                    break;
                }
                DWORD written = 0;
                if (!WriteFile(targetFile, buffer.data(), read, &written, nullptr) || written != read ||
                    !digest.Append(buffer.data(), read))
                {
                    failure = Failure::Transient;
                    message = "the content cache could not be written";
                    break;
                }
                length -= read;
                produced += read;
            }
        }
        else if (kind == static_cast<std::uint8_t>(Op::Insert))
        {
            std::uint64_t length = 0;
            if (!read_u64(deltaFile, length))
            {
                failure = Failure::Integrity;
                message = "the delta ended early";
                break;
            }
            if (!length || length > MaximumInsert || length > header.targetSize - produced)
            {
                failure = Failure::Integrity;
                message = "the delta inserts more than the bundle can hold";
                break;
            }

            while (length && failure == Failure::None)
            {
                const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(length, TransferBuffer));
                if (!read_exact(deltaFile, buffer.data(), want))
                {
                    failure = Failure::Integrity;
                    message = "the delta ended early";
                    break;
                }
                DWORD written = 0;
                if (!WriteFile(targetFile, buffer.data(), want, &written, nullptr) || written != want ||
                    !digest.Append(buffer.data(), want))
                {
                    failure = Failure::Transient;
                    message = "the content cache could not be written";
                    break;
                }
                length -= want;
                produced += want;
            }
        }
        else
        {
            failure = Failure::Integrity;
            message = "the delta contains an operation this build does not understand";
            break;
        }
    }

    if (failure == Failure::None && produced != header.targetSize)
    {
        failure = Failure::Integrity;
        message = "the delta does not produce a bundle of the declared size";
    }

    const std::string actual = failure == Failure::None ? digest.Finish() : std::string{};

    CloseHandle(targetFile);
    CloseHandle(baseFile);
    CloseHandle(deltaFile);

    // Two different checks, and they blame different things. Output that does not match the
    // delta's OWN header means the delta is wrong, and that is worth remembering. Output that
    // matches the header but not what the caller asked for means the chain was assembled
    // wrongly - the delta did exactly what it said it would, so holding it against the asset
    // would blacklist a good file and turn a small download into a large one forever.
    if (failure == Failure::None && actual != header.targetHash)
    {
        failure = Failure::Integrity;
        message = "the rebuilt bundle does not match the delta's own hash";
    }
    else if (failure == Failure::None && actual != expectedHash)
    {
        failure = Failure::Transient;
        message = "the delta does not rebuild the bundle this version wants";
    }

    if (failure != Failure::None)
    {
        std::error_code removeError;
        std::filesystem::remove(target, removeError);
        return fail(failure, message);
    }

    result.ok = true;
    return result;
}

void RecordRejected(const std::filesystem::path& path, const std::string& assetName)
{
    if (IsRejected(path, assetName))
        return;

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (output)
        output << assetName << '\n';
}

bool IsRejected(const std::filesystem::path& path, const std::string& assetName)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    std::string line;
    while (std::getline(input, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line == assetName)
            return true;
    }
    return false;
}

void ClearRejected(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}
}
