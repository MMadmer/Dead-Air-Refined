#pragma once

// The release side of the XMS module update contract (docs/dead-air/MOD_UPDATES.md, section 2):
// the descriptor grammar, version ordering, and unpacking a package under the rules a package
// has to obey. No network and no engine state, so every rule here can be exercised on its own.

#include <algorithm>
#include <atomic>
#include <compare>
#include <filesystem>
#include <string_view>

namespace ModRelease
{
inline constexpr u32 SupportedSchema = 1;
inline constexpr size_t MaximumDescriptorBytes = 64 * 1024;
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

struct Package
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
    xr_vector<Package> packages;

    u64 DownloadBytes() const;
};

// False with a reason when the text is not a usable descriptor. A schema above SupportedSchema
// still parses, with id and version only: what to do about it is the caller's decision, and
// nobody can validate the rest of a format that does not exist yet.
bool ParseDescriptor(std::string_view text, Descriptor& out, xr_string& error);

// The path rules of 2.2 for what follows "modules/<id>/". Forward slashes only.
bool SafeRelativePath(std::string_view path);

struct UnpackTotals
{
    u64 files{};
    u64 bytes{};
};

// Unpacks one package into `destination`, stripping the mandatory "modules/<id>/" prefix.
// `totals` runs across the packages of one release, so a release larger than it declared is
// caught wherever the excess sits. The limits are upper bounds; zero means undeclared. Files
// are created, never replaced: a path repeated anywhere in the release fails the unpack.
bool Unpack(const std::filesystem::path& archive, std::string_view moduleId,
    const std::filesystem::path& destination, u64 fileLimit, u64 byteLimit, UnpackTotals& totals,
    const std::atomic_bool& cancel, xr_string& error);

// [module] id and version of an unpacked manifest.
bool ReadManifestIdentity(const std::filesystem::path& manifest, xr_string& id, xr_string& version);
}
