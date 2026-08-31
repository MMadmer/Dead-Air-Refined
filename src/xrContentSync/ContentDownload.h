#pragma once

// Fetching bundles.
//
// The shape of this is dictated by what actually goes wrong at 5 GB over a domestic
// connection: the transfer is interrupted far more often than it completes in one go, so
// resume is the normal path rather than the exceptional one, and every resume has to prove
// that the bytes already on disk belong to the bundle still being fetched.
//
// A partial file is content-addressed - `content-cache\<expected sha256>.part` - so a resume
// can never continue the wrong revision, and a finished file is renamed to
// `content-cache\<sha256>` only after its hash matches. Nothing partial can ever acquire a
// bundle name, which is what makes the mount gate's job possible.

#include "ContentManifest.h"
#include "ContentPaths.h"
#include "ContentResolver.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ContentDownload
{
struct Progress
{
    std::uint64_t done{};
    std::uint64_t total{};
    std::string current;
};

struct Options
{
    // owner/name of the assets repository. Pinned in shipped code, never taken from a
    // network-fetched manifest - a mirror change is a code change.
    std::string repo;

    // QA only: an alternative base URL, accepted solely for loopback addresses. It redirects
    // where the bytes come from and nothing else; hashes still gate every commit, and there is
    // deliberately no companion switch that skips the fetch.
    std::string qaBaseUrl;

    unsigned concurrency{3};
    std::function<void(const Progress&)> onProgress;
    std::function<void(const std::string&)> onLog;
    const std::atomic_bool* cancel{};
};

struct Result
{
    bool ok{};
    std::string error;
    std::uint64_t fetched{};
};

// The QA download override, read from DAR_QA_CONTENT_BASE. Empty unless the variable is set to
// a loopback http URL, which is judged on the parsed host rather than on a prefix - the string
// "http://127.0.0.1:@evil.example/" starts with the right characters and points somewhere else.
//
// It redirects where the bytes come from and nothing else: every hash still gates every commit,
// and there is deliberately no companion switch that skips the fetch. One implementation,
// because the game and the updater must not disagree about what counts as loopback.
std::string QaBaseUrl();

// Downloads every job in `plan` that is not already cached, verifies each against its manifest
// hash, and leaves the verified files in `content-cache\<sha256>`. Installs nothing: putting
// bytes into `database\` is the commit's job and only ever happens by rename.
Result Fetch(const ContentResolver::Plan& plan, const ContentPaths::Layout& paths, const Options& options);
}
