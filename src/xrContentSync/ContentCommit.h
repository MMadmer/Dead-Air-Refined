#pragma once

// Putting verified bundles into `database\`.
//
// Two rules make the rest of the system possible and neither is negotiable:
//
//   Bundles enter `database\` only by renaming a cache file whose hash already matched. A
//   partial or wrong-hash file therefore cannot acquire a bundle name - not "is checked and
//   rejected", but cannot, because nothing ever writes into `database\` directly.
//
//   Add before delete. An obsolete bundle is demoted back to the cache after its replacement
//   is in place, never removed as part of installing one. A commit interrupted halfway leaves
//   an installation with too many bundles, which the gate ignores and the next pass tidies -
//   rather than one with too few, which is a broken game.

#include "ContentManifest.h"
#include "ContentPaths.h"
#include "ContentResolver.h"

#include <functional>
#include <string>

namespace ContentCommit
{
struct Options
{
    std::function<void(const std::string&)> onLog;
};

struct Result
{
    bool ok{};
    std::string error;
    unsigned installed{};
    unsigned demoted{};
};

// Phase 1 installs every job the plan says is available in the cache; phase 2 demotes what the
// manifest no longer declares. The latch is cleared only when a fresh resolve afterwards
// reports nothing outstanding, so a commit that half-succeeds stays marked incomplete.
Result Run(const ContentManifest::Manifest& manifest, const ContentResolver::Plan& plan,
    const ContentPaths::Layout& paths, const Options& options);
}
