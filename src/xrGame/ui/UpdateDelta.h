// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The update as a delta of the installation, whatever version it is on.
//
// A release publishes one complete archive, Update.zip. Its central directory says where every
// file sits and its manifest what every file must hash to, and both are a few kilobytes read by
// HTTP Range requests from the end of the archive. Comparing the manifest with the installation
// leaves the files that actually differ; only their records are downloaded, verified on arrival
// and written into a patch archive (schema /2, AUTO_UPDATE.md) that the installed updater applies
// like any other patch. Nobody publishes a patch per pair of versions: one version behind or ten,
// the download is what differs.

#ifdef XR_PLATFORM_WINDOWS
#include "ModHttp.h"

#include <atomic>
#include <filesystem>

namespace UpdateDelta
{
// One file of the release the installation does not hold as the release wants it.
struct Entry
{
    xr_string path;
    xr_string sha256;
    u64 size{};
    u64 header{}; // where its local record starts in the archive
    u64 end{};    // where the record after it starts
    u64 packed{};
    u32 crc{};
    u16 method{};
    u16 flags{};
    u16 time{};
    u16 date{};
};

struct Plan
{
    xr_string version;
    // The manifest's file rows, verbatim: the patch declares exactly the release's file set.
    xr_string rows;
    xr_vector<Entry> missing;
    xr_vector<xr_string> directories;
    u32 files{};
    u64 downloadBytes{};
};

// Reads the archive's directory and manifest by range and compares the manifest with the
// installation under `root`. False with a reason when the archive cannot be read this way - the
// caller then takes the whole archive.
bool Prepare(ModHttp::Session& session, const std::wstring& url, u64 archiveSize, const xr_string& version,
    const std::filesystem::path& root, Plan& plan, xr_string& error, const std::atomic_bool& stop);

// Downloads what the plan lacks into a patch archive at `archive`, every file checked against the
// manifest on arrival. `digest` receives the SHA-256 of the archive written, which is what the
// updater is handed; `progress` counts the bytes fetched.
bool Build(ModHttp::Session& session, const std::wstring& url, const Plan& plan,
    pcstr baseVersion, const std::filesystem::path& archive, xr_string& digest, std::atomic<u64>& progress,
    xr_string& error, const std::atomic_bool& stop);
} // namespace UpdateDelta
#endif
