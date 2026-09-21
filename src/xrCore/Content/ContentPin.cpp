// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "ContentPin.h"
#include "xrContentSync/ContentManifest.h"

#include <filesystem>

namespace
{
struct PinState
{
    bool loaded{};
    string_path metaDirectory{};
    ContentManifest::Manifest manifest;
    // Parallel arrays rather than a pair, because Skipped() is exported across the DLL boundary
    // and a vector of a project-local struct would pin its layout into the ABI.
    xr_vector<shared_str> skipped;
    xr_vector<shared_str> reasons;
};

PinState& state()
{
    static PinState instance;
    return instance;
}
}

namespace ContentPin
{
void RecordSkipped(pcstr fileName, pcstr reason)
{
    PinState& pin = state();

    // The same archive reaches ProcessArchive under several spellings of its path - `$arch_dir$`
    // and its aliases - and a refused file never enters m_archives, so the usual duplicate check
    // cannot see it. Without this the report lists one broken bundle five times.
    for (const shared_str& existing : pin.skipped)
        if (xr_strcmp(existing.c_str(), fileName) == 0)
            return;

    pin.skipped.emplace_back(fileName);
    pin.reasons.emplace_back(reason);
    Msg("! [content] skipped %s (%s)", fileName, reason);
}

bool IsBundle(pcstr fileName)
{
    return fileName && ContentManifest::IsBundleName(fileName);
}

void Load(pcstr fsRoot)
{
    PinState& pin = state();
    pin.loaded = false;
    pin.manifest = {};
    pin.skipped.clear();
    pin.reasons.clear();

    xr_strcpy(pin.metaDirectory, fsRoot ? fsRoot : "");
    const size_t length = xr_strlen(pin.metaDirectory);
    if (length && pin.metaDirectory[length - 1] != '\\' && pin.metaDirectory[length - 1] != '/')
        xr_strcat(pin.metaDirectory, "\\");
    xr_strcat(pin.metaDirectory, ".dead-air-x64\\");

    string_path manifestPath;
    xr_strcpy(manifestPath, pin.metaDirectory);
    xr_strcat(manifestPath, "content-manifest.txt");

    std::string error;
    if (!ContentManifest::ParseFile(std::filesystem::path(manifestPath).wstring(), pin.manifest, error))
    {
        // Not fatal here. The gate then refuses every bundle, the game reports an incomplete
        // installation and offers repair - a far better outcome than mounting whatever happens
        // to be lying in database\ and crashing later on a missing asset.
        Msg("! [content] manifest unavailable: %s", error.c_str());
        return;
    }

    pin.loaded = true;
    Msg("* [content] manifest %s, %u bundle(s), content-id %s", pin.manifest.version.c_str(),
        static_cast<u32>(pin.manifest.bundles.size()), pin.manifest.contentId.c_str());
}

bool ManifestLoaded() { return state().loaded; }

const xr_vector<shared_str>& Skipped() { return state().skipped; }

const xr_vector<shared_str>& SkipReasons() { return state().reasons; }

pcstr MetaDirectory() { return state().metaDirectory; }

bool ShouldMount(pcstr fileName, size_t size)
{
    ContentManifest::BundleName parts;
    if (!fileName || !ContentManifest::SplitBundleName(fileName, parts))
        return true; // not ours - stock archives and third-party mods are none of our business

    PinState& pin = state();
    if (!pin.loaded)
    {
        RecordSkipped(fileName, "no content manifest");
        return false;
    }

    const ContentManifest::Bundle* bundle = pin.manifest.FindByName(fileName);
    if (!bundle)
    {
        // Two very different situations wear the same shape on disk, and the distinction is
        // what the repair path acts on: a bundle occupying a slot this version knows is simply
        // an older revision left behind by a failed commit, whereas an unknown slot is a file
        // from another branch of the project or a hand-renamed archive.
        const bool stale = pin.manifest.FindBySlot(parts.group, parts.shard) != nullptr;
        RecordSkipped(fileName, stale ? "stale bundle" : "unrecognised bundle-shaped archive");
        return false;
    }
    if (bundle->size != size)
    {
        // The name carries the hash of the intended bytes, so a size mismatch under a correct
        // name means a truncated or partially written file. Mounting it would assert deep
        // inside the archive reader with nothing pointing back at the real cause.
        RecordSkipped(fileName, "size does not match the manifest");
        return false;
    }
    return true;
}
}
