#include "ContentCommit.h"

#include "ContentDelta.h"
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

    // Phase 1: install everything the plan says is ready. A job the plan already reports as not
    // cached is skipped rather than treated as a failure - stopping on the first one would make
    // the outcome depend on manifest row order and leave bundles uninstalled that were sitting
    // in the cache all along. What is genuinely missing is caught by the re-resolve at the end,
    // which is what actually decides whether the latch may go.
    //
    // Every source was hash-verified when it entered the cache, and is verified again here: it
    // may have sat there across a reboot, and this is the last point at which a wrong bundle
    // can still be stopped.
    ContentState::Cache verified;
    for (const ContentResolver::Job& job : plan.jobs)
    {
        if (!job.cached)
        {
            log(job.bundle.name + " is not in the content cache");
            continue;
        }

        const std::filesystem::path staged = paths.Cache() / job.bundle.hash;
        if (ContentHash::File(staged.wstring(), g_never) != job.bundle.hash)
        {
            result.error = job.bundle.name + " does not match its hash in the content cache";
            return result;
        }

        const std::filesystem::path installed = paths.Database() / job.bundle.name;
        if (!move_over(staged, installed))
        {
            result.error = "could not install " + job.bundle.name;
            return result;
        }
        ++result.installed;
        log("installed " + job.bundle.name);

        // Recorded because this pass actually read the bytes. Nothing else may be added below.
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(installed, sizeError);
        if (!sizeError)
        {
            verified.emplace(job.bundle.name,
                ContentState::Entry{job.bundle.hash, size, ContentState::FileTime(installed)});
        }
    }

    // Phase 2: demote. Only now, and by rename into the cache under the file's own hash, so a
    // bundle removed by mistake is still on disk and costs a move rather than a download to
    // put back.
    for (const std::string& name : plan.obsolete)
    {
        const std::filesystem::path installed = paths.Database() / name;
        const std::string hash = ContentHash::File(installed.wstring(), g_never);
        if (hash.empty())
        {
            // A file the commit cannot read is a file the cache collector could never reclaim
            // once it was renamed to something it does not recognise. Leave it where it is and
            // let the gate keep reporting it.
            log("could not read " + name + " - it will be reported and retried");
            continue;
        }

        if (move_over(installed, paths.Cache() / hash))
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

    // The advisory cache carries the entries this pass READ, on top of whatever a previous pass
    // recorded. Writing an entry for a bundle nobody hashed would be worse than writing
    // nothing: the resolver trusts a matching (size, mtime) entry and would then skip that
    // bundle forever, so a corrupt file would be vouched for by a record invented from the very
    // manifest it fails to match.
    // A delta blacklisted for the previous release says nothing about this one: the base it was
    // wrong against may not even be part of the new content set. Read before the state file is
    // rewritten, because rewriting it is what erases the answer.
    const std::string previousId = ContentState::LoadContentId(paths.State());
    if (!previousId.empty() && previousId != manifest.contentId)
        ContentDelta::ClearRejected(paths.RejectedDeltas());

    ContentState::Cache state = ContentState::LoadCache(paths.State());
    for (const auto& entry : verified)
        state[entry.first] = entry.second;
    ContentState::SaveCache(paths.State(), manifest.contentId, state);

    // The latch goes only when a fresh look says there is nothing left to do, and that look
    // hashes. A stat-only pass structurally cannot see the corruption a latch may have been set
    // for, so clearing it on that word would be clearing it on no evidence. Everything this
    // commit touched is in the state cache now, so the cost is a stat for those and a real read
    // only for bundles nothing has vouched for.
    ContentResolver::Options verify;
    verify.verifyHashes = true;
    verify.probeCache = false; // the question here is about the database directory, not the cache
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
