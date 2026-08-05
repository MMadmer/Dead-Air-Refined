#include "StdAfx.h"
#include "save_transaction_marker.h"

#include <array>
#include <span>
#include <type_traits>

namespace SaveTransactionMarker
{
namespace
{
constexpr u32 magic = 0x314E5854;
constexpr u32 version = 1;
constexpr u32 supportedFlags = customDataExpected | previousMainPresent |
    previousCustomPresent | previousSidecarPresent | legacyCustomCapture;
constexpr size_t markerSize = 4 * sizeof(u32) + sizeof(u64);
constexpr size_t checksumOffset = 3 * sizeof(u32) + sizeof(u64);

template <typename T>
void write_value(xr_vector<u8>& contents, size_t& offset, T value)
{
    static_assert(std::is_unsigned_v<T>);
    for (size_t index = 0; index < sizeof(value); ++index)
        contents[offset + index] = static_cast<u8>(value >> (index * 8));
    offset += sizeof(value);
}

template <typename T>
bool read_value(std::span<const u8> contents, size_t& offset, T& value)
{
    static_assert(std::is_unsigned_v<T>);
    if (offset > contents.size() || sizeof(value) > contents.size() - offset)
        return false;

    value = 0;
    for (size_t index = 0; index < sizeof(value); ++index)
        value |= static_cast<T>(contents[offset + index]) << (index * 8);
    offset += sizeof(value);
    return true;
}

ReadResult parse(std::span<const u8> contents, State& state)
{
    state = {};
    if (contents.size() != markerSize)
        return ReadResult::Invalid;

    size_t offset = 0;
    u32 storedMagic = 0;
    u32 storedVersion = 0;
    u32 storedChecksum = 0;
    if (!read_value(contents, offset, storedMagic) || !read_value(contents, offset, storedVersion) ||
        !read_value(contents, offset, state.saveId) || !read_value(contents, offset, state.flags) ||
        !read_value(contents, offset, storedChecksum) || storedMagic != magic || storedVersion != version ||
        !state.saveId || (state.flags & ~supportedFlags) ||
        crc32(contents.data(), static_cast<u32>(checksumOffset)) != storedChecksum)
    {
        state = {};
        return ReadResult::Invalid;
    }
    return ReadResult::Valid;
}
}

void build(const State& state, xr_vector<u8>& result)
{
    result.assign(markerSize, 0);
    size_t offset = 0;
    write_value(result, offset, magic);
    write_value(result, offset, version);
    write_value(result, offset, state.saveId);
    write_value(result, offset, state.flags & supportedFlags);
    const u32 checksum = crc32(result.data(), static_cast<u32>(checksumOffset));
    write_value(result, offset, checksum);
}

ReadResult read(pcstr path, State& state)
{
    state = {};
    std::array<u8, markerSize> contents{};
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
            ReadResult::Missing : ReadResult::Error;
    }

    LARGE_INTEGER size;
    DWORD bytesRead = 0;
    const bool readSucceeded = GetFileSizeEx(file, &size) &&
        size.QuadPart == static_cast<LONGLONG>(markerSize) &&
        ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &bytesRead, nullptr) &&
        bytesRead == static_cast<DWORD>(contents.size());
    CloseHandle(file);
    if (!readSucceeded)
        return ReadResult::Invalid;
#else
    const int file = open(path, O_RDONLY);
    if (file < 0)
        return errno == ENOENT ? ReadResult::Missing : ReadResult::Error;

    struct stat info;
    const ssize_t bytesRead = ::read(file, contents.data(), contents.size());
    const bool readSucceeded = fstat(file, &info) == 0 && info.st_size == static_cast<off_t>(markerSize) &&
        bytesRead == static_cast<ssize_t>(contents.size());
    close(file);
    if (!readSucceeded)
        return ReadResult::Invalid;
#endif
    return parse(contents, state);
}
}
