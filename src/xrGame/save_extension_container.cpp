#include "StdAfx.h"
#include "save_extension_container.h"

#include <array>
#include <mutex>
#include <type_traits>

namespace SaveExtensionContainer
{
namespace
{
constexpr u32 magic = 0x31564F53;
// The first 48 bytes remain byte-compatible with v1/v2; v3 freezes the directory contract.
constexpr u32 legacyVersion = 1;
constexpr u32 fixedChunkVersion = 2;
constexpr u32 directoryVersion = 3;
constexpr u16 legacyHelmetFilterChunkVersion = 1;
constexpr size_t legacyHeaderSize = 48;
constexpr size_t fixedChunkHeaderSize = sizeof(u32) + 2 * sizeof(u16) + sizeof(u32);
constexpr size_t directoryHeaderSize = 88;
constexpr size_t directoryEntrySize = 32;
constexpr size_t headerChecksumOffset = 84;
constexpr u32 customDataPresent = 1u << 0;
constexpr u32 supportedContainerFlags = customDataPresent;
constexpr size_t maxContainerSize = 64ull * 1024 * 1024;
constexpr size_t maxChunkSize = 16ull * 1024 * 1024;
constexpr u32 maxChunkCount = 1024;
constexpr size_t signatureBufferSize = 64 * 1024;

std::mutex loadedChunksMutex;
ChunkSnapshot loadedChunks = std::make_shared<const ChunkList>();

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

template <typename T>
void write_value(xr_vector<u8>& contents, size_t& offset, const T& value)
{
    static_assert(std::is_unsigned_v<T>);
    for (size_t index = 0; index < sizeof(value); ++index)
        contents[offset + index] = static_cast<u8>(value >> (index * 8));
    offset += sizeof(value);
}

bool range_fits(size_t offset, size_t size, size_t total)
{
    return offset <= total && size <= total - offset;
}

bool binding_matches(const Binding& stored, const FileSignature& actualScop, const FileSignature& actualScoc)
{
    return actualScop.present && stored.scop == actualScop && stored.scoc == actualScoc;
}

u32 header_checksum(std::span<const u8> header)
{
    xr_vector<u8> copy(header.begin(), header.end());
    memset(copy.data() + headerChecksumOffset, 0, sizeof(u32));
    return crc32(copy.data(), static_cast<u32>(copy.size()));
}

bool parse_legacy_binding(std::span<const u8> contents, Binding& binding, u32& version)
{
    size_t offset = 0;
    u32 storedMagic = 0;
    if (!read_value(contents, offset, storedMagic) || storedMagic != magic ||
        !read_value(contents, offset, version) || !read_value(contents, offset, binding.saveId) ||
        !read_value(contents, offset, binding.sourceSize) || !read_value(contents, offset, binding.compressedSize) ||
        !read_value(contents, offset, binding.scop.size) || !read_value(contents, offset, binding.scop.checksum) ||
        !read_value(contents, offset, binding.scoc.size) || !read_value(contents, offset, binding.scoc.checksum))
    {
        return false;
    }

    binding.scop.present = true;
    binding.scoc.present = binding.scoc.size != 0 || binding.scoc.checksum != 0;
    return true;
}

LoadResult parse_fixed_chunk(std::span<const u8> contents, Container& result)
{
    if (contents.size() < legacyHeaderSize + fixedChunkHeaderSize)
        return LoadResult::Invalid;

    size_t offset = legacyHeaderSize;
    Chunk chunk;
    u16 count = 0;
    u32 expectedChecksum = 0;
    if (!read_value(contents, offset, chunk.type) || chunk.type != legacyHelmetFilterChunkType ||
        !read_value(contents, offset, chunk.version) || chunk.version != legacyHelmetFilterChunkVersion ||
        !read_value(contents, offset, count) ||
        !read_value(contents, offset, expectedChecksum))
    {
        return LoadResult::Invalid;
    }

    constexpr size_t recordSize = 2 * sizeof(u16) + sizeof(u32);
    const size_t payloadSize = static_cast<size_t>(count) * recordSize;
    if (!range_fits(offset, payloadSize, contents.size()) || offset + payloadSize != contents.size() ||
        crc32(contents.data() + offset, static_cast<u32>(payloadSize)) != expectedChecksum)
    {
        return LoadResult::Invalid;
    }

    chunk.payload.resize(sizeof(u32) + payloadSize);
    const u32 recordCount = count;
    size_t payloadOffset = 0;
    write_value(chunk.payload, payloadOffset, recordCount);
    memcpy(chunk.payload.data() + sizeof(recordCount), contents.data() + offset, payloadSize);
    result.chunks.emplace_back(std::move(chunk));
    return LoadResult::Valid;
}

LoadResult parse_directory(std::span<const u8> contents, Container& result)
{
    size_t offset = legacyHeaderSize;
    u32 headerSize = 0;
    u32 flags = 0;
    u32 chunkCount = 0;
    u32 entrySize = 0;
    u64 directoryOffset = 0;
    u64 containerSize = 0;
    u32 expectedDirectoryChecksum = 0;
    u32 expectedHeaderChecksum = 0;
    if (!read_value(contents, offset, headerSize) || !read_value(contents, offset, flags) ||
        !read_value(contents, offset, chunkCount) || !read_value(contents, offset, entrySize) ||
        !read_value(contents, offset, directoryOffset) || !read_value(contents, offset, containerSize) ||
        !read_value(contents, offset, expectedDirectoryChecksum) ||
        !read_value(contents, offset, expectedHeaderChecksum))
    {
        return LoadResult::Invalid;
    }

    if (headerSize != directoryHeaderSize || headerSize > contents.size() ||
        flags & ~supportedContainerFlags || chunkCount > maxChunkCount || entrySize != directoryEntrySize ||
        containerSize != contents.size() || directoryOffset != directoryHeaderSize ||
        header_checksum(contents.first(headerSize)) != expectedHeaderChecksum)
    {
        return LoadResult::Invalid;
    }

    result.binding.scoc.present = !!(flags & customDataPresent);
    const size_t directoryStart = static_cast<size_t>(directoryOffset);
    const size_t directorySize = static_cast<size_t>(chunkCount) * directoryEntrySize;
    if (!range_fits(directoryStart, directorySize, contents.size()) ||
        crc32(contents.data() + directoryStart, static_cast<u32>(directorySize)) != expectedDirectoryChecksum)
    {
        return LoadResult::Invalid;
    }

    struct ParsedEntry
    {
        Chunk chunk;
        size_t offset{};
        size_t size{};
        u32 checksum{};
    };

    xr_vector<ParsedEntry> entries;
    entries.reserve(chunkCount);
    offset = directoryStart;
    for (u32 i = 0; i < chunkCount; ++i)
    {
        ParsedEntry entry;
        u64 payloadOffset = 0;
        u64 payloadSize = 0;
        u32 reserved = 0;
        if (!read_value(contents, offset, entry.chunk.type) || !read_value(contents, offset, entry.chunk.version) ||
            !read_value(contents, offset, entry.chunk.flags) || !read_value(contents, offset, payloadOffset) ||
            !read_value(contents, offset, payloadSize) || !read_value(contents, offset, entry.checksum) ||
            !read_value(contents, offset, reserved) || !entry.chunk.type || !entry.chunk.version || reserved != 0 ||
            payloadSize > maxChunkSize || (payloadOffset & 7u) ||
            payloadOffset > std::numeric_limits<size_t>::max() || payloadSize > std::numeric_limits<size_t>::max())
        {
            return LoadResult::Invalid;
        }

        entry.offset = static_cast<size_t>(payloadOffset);
        entry.size = static_cast<size_t>(payloadSize);
        if (entry.offset < directoryStart + directorySize ||
            !range_fits(entry.offset, entry.size, contents.size()))
        {
            return LoadResult::Invalid;
        }
        entries.emplace_back(std::move(entry));
    }

    xr_vector<const ParsedEntry*> byType;
    xr_vector<const ParsedEntry*> byOffset;
    byType.reserve(entries.size());
    byOffset.reserve(entries.size());
    for (const ParsedEntry& entry : entries)
    {
        byType.push_back(&entry);
        byOffset.push_back(&entry);
    }

    std::sort(byType.begin(), byType.end(), [](const ParsedEntry* left, const ParsedEntry* right)
    {
        return left->chunk.type < right->chunk.type;
    });
    if (std::adjacent_find(byType.begin(), byType.end(), [](const ParsedEntry* left, const ParsedEntry* right)
        {
            return left->chunk.type == right->chunk.type;
        }) != byType.end())
    {
        return LoadResult::Invalid;
    }

    std::sort(byOffset.begin(), byOffset.end(), [](const ParsedEntry* left, const ParsedEntry* right)
    {
        return left->offset < right->offset;
    });
    for (size_t i = 1; i < byOffset.size(); ++i)
    {
        const ParsedEntry& previous = *byOffset[i - 1];
        if (previous.size && byOffset[i]->offset < previous.offset + previous.size)
            return LoadResult::Invalid;
    }

    result.chunks.reserve(entries.size());
    for (ParsedEntry& entry : entries)
    {
        if (crc32(contents.data() + entry.offset, static_cast<u32>(entry.size)) != entry.checksum)
        {
            ++result.skippedChunks;
            continue;
        }

        entry.chunk.payload.assign(
            contents.begin() + entry.offset, contents.begin() + entry.offset + entry.size);
        result.chunks.emplace_back(std::move(entry.chunk));
    }
    return LoadResult::Valid;
}
}

FileSignature make_signature(std::span<const u8> contents)
{
    FileSignature result;
    result.size = contents.size();
    result.checksum = crc32(contents.data(), static_cast<u32>(contents.size()));
    result.present = true;
    return result;
}

SignatureResult read_file_signature(pcstr path, FileSignature& result)
{
    result = {};
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
            SignatureResult::Missing : SignatureResult::Error;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<u64>(size.QuadPart) > std::numeric_limits<u32>::max())
    {
        CloseHandle(file);
        return SignatureResult::Error;
    }

    std::array<u8, signatureBufferSize> buffer;
    u64 remaining = static_cast<u64>(size.QuadPart);
    u32 checksum = 0;
    while (remaining)
    {
        const DWORD requested = static_cast<DWORD>(std::min<u64>(remaining, buffer.size()));
        DWORD bytesRead = 0;
        if (!ReadFile(file, buffer.data(), requested, &bytesRead, nullptr) || bytesRead != requested)
        {
            CloseHandle(file);
            return SignatureResult::Error;
        }
        checksum = crc32(buffer.data(), bytesRead, checksum);
        remaining -= bytesRead;
    }
    CloseHandle(file);

    result.size = static_cast<u64>(size.QuadPart);
    result.checksum = checksum;
    result.present = true;
    return SignatureResult::Valid;
#else
    const int file = open(path, O_RDONLY);
    if (file < 0)
        return errno == ENOENT ? SignatureResult::Missing : SignatureResult::Error;

    struct stat info;
    if (fstat(file, &info) != 0 || info.st_size < 0 ||
        static_cast<u64>(info.st_size) > std::numeric_limits<u32>::max())
    {
        close(file);
        return SignatureResult::Error;
    }

    std::array<u8, signatureBufferSize> buffer;
    u64 remaining = static_cast<u64>(info.st_size);
    u32 checksum = 0;
    while (remaining)
    {
        const size_t requested = static_cast<size_t>(std::min<u64>(remaining, buffer.size()));
        const ssize_t bytesRead = read(file, buffer.data(), requested);
        if (bytesRead <= 0)
        {
            close(file);
            return SignatureResult::Error;
        }
        checksum = crc32(buffer.data(), static_cast<u32>(bytesRead), checksum);
        remaining -= static_cast<u64>(bytesRead);
    }
    close(file);

    result.size = static_cast<u64>(info.st_size);
    result.checksum = checksum;
    result.present = true;
    return SignatureResult::Valid;
#endif
}

LoadResult load(pcstr path, const FileSignature& actualScop, const FileSignature& actualScoc, Container& result)
{
    result = {};
    IReader* stream = FS.r_open(path);
    if (!stream)
    {
#if defined(XR_PLATFORM_WINDOWS)
        const DWORD attributes = GetFileAttributesA(path);
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return LoadResult::Missing;
        }
#else
        if (access(path, F_OK) != 0 && errno == ENOENT)
            return LoadResult::Missing;
#endif
        return LoadResult::IoError;
    }

    const size_t size = stream->length();
    if (size < legacyHeaderSize || size > maxContainerSize)
    {
        FS.r_close(stream);
        return LoadResult::Invalid;
    }

    const std::span<const u8> contents(static_cast<const u8*>(stream->pointer()), size);
    u32 version = 0;
    if (!parse_legacy_binding(contents, result.binding, version))
    {
        FS.r_close(stream);
        return LoadResult::Invalid;
    }

    LoadResult parsed = LoadResult::Invalid;
    if (version == legacyVersion && size == legacyHeaderSize)
        parsed = LoadResult::Valid;
    else if (version == fixedChunkVersion)
        parsed = parse_fixed_chunk(contents, result);
    else if (version == directoryVersion)
        parsed = parse_directory(contents, result);

    if (parsed == LoadResult::Valid && !binding_matches(result.binding, actualScop, actualScoc))
        parsed = LoadResult::SignatureMismatch;

    FS.r_close(stream);
    if (parsed != LoadResult::Valid)
    {
        result.chunks.clear();
        result.skippedChunks = 0;
    }
    return parsed;
}

bool build(const Binding& binding, const ChunkList& chunks, xr_vector<u8>& result)
{
    result.clear();
    if (!binding.scop.present || (!binding.scoc.present && (binding.scoc.size || binding.scoc.checksum)) ||
        chunks.size() > maxChunkCount)
        return false;

    xr_vector<const Chunk*> ordered;
    ordered.reserve(chunks.size());
    size_t payloadSize = 0;
    for (const Chunk& chunk : chunks)
    {
        if (!chunk.type || !chunk.version || chunk.payload.size() > maxChunkSize)
            return false;
        ordered.push_back(&chunk);
        payloadSize += chunk.payload.size();
        if (payloadSize > maxContainerSize)
            return false;
    }

    std::sort(ordered.begin(), ordered.end(), [](const Chunk* left, const Chunk* right)
    {
        return left->type < right->type;
    });
    if (std::adjacent_find(ordered.begin(), ordered.end(), [](const Chunk* left, const Chunk* right)
        {
            return left->type == right->type;
        }) != ordered.end())
    {
        return false;
    }

    const size_t directorySize = ordered.size() * directoryEntrySize;
    const size_t initialPayloadOffset = directoryHeaderSize + directorySize;
    size_t containerSize = initialPayloadOffset;
    for (const Chunk* chunk : ordered)
    {
        containerSize = (containerSize + 7) & ~size_t{7};
        if (chunk->payload.size() > maxContainerSize - containerSize)
            return false;
        containerSize += chunk->payload.size();
    }
    if (containerSize > maxContainerSize)
        return false;

    result.assign(containerSize, 0);
    size_t offset = 0;
    write_value(result, offset, magic);
    write_value(result, offset, directoryVersion);
    write_value(result, offset, binding.saveId);
    write_value(result, offset, binding.sourceSize);
    write_value(result, offset, binding.compressedSize);
    write_value(result, offset, binding.scop.size);
    write_value(result, offset, binding.scop.checksum);
    write_value(result, offset, binding.scoc.size);
    write_value(result, offset, binding.scoc.checksum);

    write_value(result, offset, static_cast<u32>(directoryHeaderSize));
    const u32 flags = binding.scoc.present ? customDataPresent : 0;
    write_value(result, offset, flags);
    write_value(result, offset, static_cast<u32>(ordered.size()));
    write_value(result, offset, static_cast<u32>(directoryEntrySize));
    write_value(result, offset, static_cast<u64>(directoryHeaderSize));
    write_value(result, offset, static_cast<u64>(containerSize));
    const size_t directoryChecksumOffset = offset;
    write_value(result, offset, u32{});
    write_value(result, offset, u32{});

    size_t payloadOffset = initialPayloadOffset;
    for (const Chunk* chunk : ordered)
    {
        payloadOffset = (payloadOffset + 7) & ~size_t{7};
        write_value(result, offset, chunk->type);
        write_value(result, offset, chunk->version);
        write_value(result, offset, chunk->flags);
        write_value(result, offset, static_cast<u64>(payloadOffset));
        write_value(result, offset, static_cast<u64>(chunk->payload.size()));
        const u32 checksum = crc32(chunk->payload.data(), static_cast<u32>(chunk->payload.size()));
        write_value(result, offset, checksum);
        write_value(result, offset, u32{});

        if (!chunk->payload.empty())
            memcpy(result.data() + payloadOffset, chunk->payload.data(), chunk->payload.size());
        payloadOffset += chunk->payload.size();
    }

    const u32 directoryChecksum = crc32(result.data() + directoryHeaderSize, static_cast<u32>(directorySize));
    size_t checksumOffset = directoryChecksumOffset;
    write_value(result, checksumOffset, directoryChecksum);
    const u32 checksum = header_checksum(std::span<const u8>(result.data(), directoryHeaderSize));
    checksumOffset = headerChecksumOffset;
    write_value(result, checksumOffset, checksum);
    return true;
}

const Chunk* find_chunk(const ChunkList& chunks, u32 type)
{
    const auto found = std::find_if(chunks.begin(), chunks.end(), [type](const Chunk& chunk)
    {
        return chunk.type == type;
    });
    return found == chunks.end() ? nullptr : &*found;
}

UpdateResult upsert_supported_chunk(ChunkList& chunks, Chunk replacement, u16 newestSupportedVersion)
{
    if (!replacement.type || !replacement.version || replacement.version > newestSupportedVersion)
        return UpdateResult::Invalid;

    auto found = chunks.end();
    for (auto current = chunks.begin(); current != chunks.end(); ++current)
    {
        if (current->type != replacement.type)
            continue;
        if (found != chunks.end())
            return UpdateResult::Duplicate;
        found = current;
    }

    if (found != chunks.end() && (found->version > newestSupportedVersion || found->flags))
        return UpdateResult::PreservedNewer;
    if (found != chunks.end() && replacement.version < found->version)
        return UpdateResult::Invalid;
    if (found == chunks.end())
        chunks.emplace_back(std::move(replacement));
    else
        *found = std::move(replacement);
    return UpdateResult::Updated;
}

UpdateResult erase_supported_chunk(ChunkList& chunks, u32 type, u16 newestSupportedVersion)
{
    if (!type || !newestSupportedVersion)
        return UpdateResult::Invalid;

    auto found = chunks.end();
    for (auto current = chunks.begin(); current != chunks.end(); ++current)
    {
        if (current->type != type)
            continue;
        if (found != chunks.end())
            return UpdateResult::Duplicate;
        found = current;
    }

    if (found == chunks.end())
        return UpdateResult::Updated;
    if (found->version > newestSupportedVersion || found->flags)
        return UpdateResult::PreservedNewer;
    chunks.erase(found);
    return UpdateResult::Updated;
}

void set_loaded_chunks(ChunkList chunks)
{
    std::lock_guard lock(loadedChunksMutex);
    loadedChunks = std::make_shared<const ChunkList>(std::move(chunks));
}

void clear_loaded_chunks()
{
    std::lock_guard lock(loadedChunksMutex);
    loadedChunks = std::make_shared<const ChunkList>();
}

ChunkSnapshot snapshot_loaded_chunks()
{
    std::lock_guard lock(loadedChunksMutex);
    return loadedChunks;
}

bool copy_loaded_chunk(u32 type, Chunk& result)
{
    std::lock_guard lock(loadedChunksMutex);
    const Chunk* found = find_chunk(*loadedChunks, type);
    if (!found)
        return false;
    result = *found;
    return true;
}
}
