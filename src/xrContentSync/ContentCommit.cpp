#include "ContentCommit.h"

#include "ContentHash.h"
#include "ContentState.h"

#define WIN32_LEAN_AND_MEAN
// std::min / std::max are used throughout; the windows.h macros of the same name would
// swallow them.
#define NOMINMAX
#include <windows.h>

namespace
{
const std::atomic_bool g_never{};

// A bundle that has just been written is exactly the file a real-time scanner is most likely
// to still have open. Retrying briefly turns a guaranteed failure into a pause nobody notices.
constexpr unsigned MoveAttempts = 5;
constexpr DWORD MoveRetryMs = 200;

bool move_over(const std::filesystem::path& from, const std::filesystem::path& to)
{
    for (unsigned attempt = 0; attempt < MoveAttempts; ++attempt)
    {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        Sleep(MoveRetryMs);
    }
    return false;
}
}

namespace ContentCommit
{
Result Run(const ContentManifest::Manifest& manifest, const ContentResolver::Plan& plan,
    const ContentPaths::Layout& paths, const Options& options)
{
    Result result;
    const auto log = [&](const std::string& line)
    {
        if (options.onLog)
            options.onLog(line);
    };

    std::error_code error;
    std::filesystem::create_directories(paths.Database(), error);
    std::filesystem::create_directories(paths.Cache(), error);

    // Across volumes MoveFileExW silently degrades to copy-and-delete, which doubles the peak
    // disk requirement the free-space check was sized against and stops being atomic. Refuse
    // rather than half-succeed at 5 GB.
    if (!ContentResolver::SameVolume(paths.Cache(), paths.Database()))
    {
        result.error = "the content cache and the database directory are on different volumes";
        return result;
    }

    // Phase 1: install. Every source has already been hash-verified into the cache, but verify
    // again here - the file may have sat in the cache across a reboot, and this is the last
    // point at which a wrong bundle can still be stopped.
    for (const ContentResolver::Job& job : plan.jobs)
    {
        const std::filesystem::path staged = paths.Cache() / job.bundle.hash;
        if (ContentHash::File(staged.wstring(), g_never) != job.bundle.hash)
        {
            result.error = job.bundle.name + " is not in the content cache";
            return result;
        }
        if (!move_over(staged, paths.Database() / job.bundle.name))
        {
            result.error = "could not install " + job.bundle.name;
            return result;
        }
        ++result.installed;
        log("installed " + job.bundle.name);
    }

    // Phase 2: demote. Only now, and by rename into the cache under the file's own hash, so a
    // bundle removed by mistake is still on disk and costs a move rather than a download to
    // put back.
    for (const std::string& name : plan.obsolete)
    {
        const std::filesystem::path installed = paths.Database() / name;
        const std::string hash = ContentHash::File(installed.wstring(), g_never);
        const std::filesystem::path destination = paths.Cache() / (hash.empty() ? name : hash);
        if (move_over(installed, destination))
        {
            ++result.demoted;
            log("retired " + name);
        }
        else
        {
            // Not fatal. A leftover bundle is refused by the mount gate and reported as a
            // notice; refusing the whole commit over one would turn a tidy-up into a failure.
            log("could not retire " + name + " - it will be reported and retried");
        }
    }

    // Rebuild the state cache from what is now installed, so the next launch is a stat per
    // bundle rather than a full rehash of everything just written.
    ContentState::Cache state;
    for (const ContentManifest::Bundle& bundle : manifest.bundles)
    {
        const std::filesystem::path file = paths.Database() / bundle.name;
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(file, sizeError);
        if (sizeError)
            continue;
        state.emplace(bundle.name, ContentState::Entry{bundle.hash, size, ContentState::FileTime(file)});
    }
    ContentState::SaveCache(paths.State(), manifest.contentId, state);

    // The latch goes only when a fresh look says there is nothing left to do. Trusting the
    // loop above would clear it on a commit that installed four bundles out of five.
    ContentResolver::Options verify;
    verify.verifyHashes = false; // everything was just hashed; a stat is enough
    ContentResolver::Plan after;
    std::string resolveError;
    if (!ContentResolver::Resolve(manifest, paths, verify, after, resolveError) || !after.Complete())
    {
        result.error = resolveError.empty() ? "the installation is still incomplete after the commit"
                                            : resolveError;
        return result;
    }

    ContentState::ClearLatch(paths.Latch());
    result.ok = true;
    return result;
}
}
