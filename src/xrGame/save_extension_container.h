#pragma once

#include "save_extension_chunk_ids.h"
#include "xrCore/xr_types.h"
#include "xrCommon/xr_vector.h"

#include <memory>
#include <span>

namespace SaveExtensionContainer
{
inline constexpr pcstr extension = ".scov";
inline constexpr u32 legacyHelmetFilterChunkType = SaveExtensionChunkIds::HelmetFilters;

struct FileSignature
{
    u64 size{};
    u32 checksum{};
    bool present{};

    bool operator==(const FileSignature&) const = default;
};

struct Binding
{
    u64 saveId{};
    u32 sourceSize{};
    u32 compressedSize{};
    FileSignature scop;
    FileSignature scoc;
};

struct Chunk
{
    u32 type{};
    u16 version{};
    u16 flags{};
    xr_vector<u8> payload;
};

using ChunkList = xr_vector<Chunk>;
using ChunkSnapshot = std::shared_ptr<const ChunkList>;

struct Container
{
    Binding binding;
    ChunkList chunks;
    u32 skippedChunks{};
};

enum class SignatureResult
{
    Missing,
    Valid,
    Error
};

enum class LoadResult
{
    Missing,
    IoError,
    Valid,
    Invalid,
    SignatureMismatch
};

enum class UpdateResult
{
    Updated,
    PreservedNewer,
    Duplicate,
    Invalid
};

[[nodiscard]] FileSignature make_signature(std::span<const u8> contents);
[[nodiscard]] SignatureResult read_file_signature(pcstr path, FileSignature& result);
[[nodiscard]] LoadResult load(
    pcstr path, const FileSignature& actualScop, const FileSignature& actualScoc, Container& result);
[[nodiscard]] bool build(const Binding& binding, const ChunkList& chunks, xr_vector<u8>& result);

[[nodiscard]] const Chunk* find_chunk(const ChunkList& chunks, u32 type);
[[nodiscard]] UpdateResult upsert_supported_chunk(
    ChunkList& chunks, Chunk replacement, u16 newestSupportedVersion);
[[nodiscard]] UpdateResult erase_supported_chunk(ChunkList& chunks, u32 type, u16 newestSupportedVersion);

void set_loaded_chunks(ChunkList chunks);
void clear_loaded_chunks();
[[nodiscard]] ChunkSnapshot snapshot_loaded_chunks();
[[nodiscard]] bool copy_loaded_chunk(u32 type, Chunk& result);
}
