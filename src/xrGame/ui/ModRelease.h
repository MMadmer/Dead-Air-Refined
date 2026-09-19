#pragma once

// The release side of the XMS module update contract (docs/dead-air/MOD_UPDATES.md, section 2):
// the descriptor and file index grammars, version ordering, and the sink that turns a stretch of
// package bytes back into a verified file. No network and no engine state, so every rule here
// can be exercised on its own.

#include <algorithm>
#include <compare>
#include <filesystem>
#include <memory>
#include <string_view>

namespace ModRelease
{
inline constexpr u32 SupportedSchema = 1;
inline constexpr u32 SupportedIndexVersion = 1;
inline constexpr size_t MaximumDescriptorBytes = 64 * 1024;
inline constexpr size_t MaximumIndexBytes = 64 * 1024 * 1024;
inline constexpr size_t MaximumPackages = 64;

struct Version
{
    u32 parts[4]{};
    bool valid{};

    std::strong_ordering operator<=>(const Version& other) const
    {
        // an unparseable version is older than every real one
        if (valid != other.valid)
            return valid <=> other.valid;
        return std::lexicographical_compare_three_way(
            std::begin(parts), std::end(parts), std::begin(other.parts), std::end(other.parts));
    }
    bool operator==(const Version& other) const { return (*this <=> other) == 0; }
};

// The XMS module id grammar: [a-z0-9_.-]+
bool ValidModuleId(std::string_view id);

// One to four dot-separated numbers; whatever follows the numeric part is ignored.
Version ParseVersion(std::string_view text);

struct Asset
{
    xr_string name;
    xr_string sha256; // lowercase hex
    u64 size{};
};

struct Descriptor
{
    u32 schema{};
    xr_string id;
    xr_string version;
    xr_string requiresGame;
    u64 files{};    // 0 = not declared
    u64 unpacked{}; // 0 = not declared
    Asset index;
    xr_vector<Asset> packages;

    u64 PackageBytes() const;
};

// False with a reason when the text is not a usable descriptor. A schema above SupportedSchema
// still parses, with id and version only: what to do about it is the caller's decision, and
// nobody can validate the rest of a format that does not exist yet.
bool ParseDescriptor(std::string_view text, Descriptor& out, xr_string& error);

// The path rules of 2.2: relative to the module folder, forward slashes only.
bool SafeRelativePath(std::string_view path);

struct FileEntry
{
    xr_string path;
    xr_string sha256;
    u64 size{};
    u64 offset{}; // first byte of the stored data inside the package
    u64 packed{}; // length of that data
    u32 package{}; // index into Descriptor::packages
    bool deflated{};
};

struct FileIndex
{
    xr_vector<FileEntry> files;
    u64 unpacked{};
};

// Parses an index against the descriptor that named it: every `pack` has to be one of its
// packages, every `file` has to lie inside the package it points into, and the totals have to
// be the ones the descriptor declared.
bool ParseFileIndex(std::string_view text, const Descriptor& descriptor, FileIndex& out, xr_string& error);

// Writes one file of a release from the bytes a package stores for it. The data arrives in
// whatever pieces the network delivers; Finish() is where the file either is what the index
// promised - size and SHA-256 - or is deleted.
class FileSink
{
public:
    FileSink();
    ~FileSink();
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    // Creates the file, never replaces one: the staging folder starts empty, so an existing
    // file is a path the release names twice.
    bool Open(const std::filesystem::path& target, const FileEntry& entry, xr_string& error);
    bool Append(const void* data, size_t size, xr_string& error);
    bool Finish(xr_string& error);

private:
    void Discard();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// [module] id and version of a manifest on disk.
bool ReadManifestIdentity(const std::filesystem::path& manifest, xr_string& id, xr_string& version);
}
