#include "StdAfx.h"
#include "ContentService.h"

#include "xrCore/Content/ContentPin.h"
#include "xrContentSync/ContentManifest.h"
#include "xrContentSync/ContentPaths.h"
#include "xrContentSync/ContentResolver.h"
#include "xrContentSync/ContentState.h"

#ifdef XR_PLATFORM_WINDOWS
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <thread>

namespace
{
struct ServiceState
{
    ~ServiceState()
    {
        // The verifier can be started without a main menu ever existing, so this destructor is
        // a real shutdown path, not a formality. Ask first, then join - joining a hashing
        // thread that was never told to stop can hold process teardown for a full pass.
        stopRequested.store(true, std::memory_order_release);
        if (worker.joinable())
            worker.join();
    }

    std::atomic<ContentService::State> state{ContentService::State::Unknown};
    std::atomic_bool stopRequested{};
    std::atomic_bool verifyStarted{};
    // Set when the cheap pass found nothing wrong but a previous run left the latch behind.
    // That is the one case where a hash pass may run while the state is still Incomplete.
    std::atomic_bool latchedOnly{};

    std::mutex dataMutex;
    ContentPaths::Layout paths;
    ContentManifest::Manifest manifest;
    xr_vector<xr_string> problems;
    xr_vector<xr_string> notices;
    u32 present{};
    u64 missingBytes{};
    // How many entries of `problems` came from the initial pass. A verify pass owns everything
    // after that mark and rewrites it; what came before is not its to retract.
    size_t initialProblems{};
    std::thread worker;
};

ServiceState& service()
{
    static ServiceState instance;
    return instance;
}

void add_problem(ServiceState& instance, const xr_string& line)
{
    instance.problems.push_back(line);
    Msg("! [content] %s", line.c_str());
}

// Something is wrong on disk but the game can still run: a leftover file that is not mounted
// and not needed. Worth reporting and worth cleaning up, never worth refusing a session over.
void add_notice(ServiceState& instance, const xr_string& line)
{
    instance.notices.push_back(line);
    Msg("~ [content] %s", line.c_str());
}

xr_string format(pcstr pattern, ...)
{
    string4096 buffer{};
    va_list arguments;
    va_start(arguments, pattern);
    vsnprintf(buffer, sizeof(buffer), pattern, arguments);
    va_end(arguments);
    return xr_string(buffer);
}

xr_string describe(const ContentResolver::Job& job)
{
    switch (job.fault)
    {
    case ContentResolver::Fault::Size:
        return format("%s is %llu bytes, the manifest says %llu", job.bundle.name.c_str(),
            static_cast<unsigned long long>(job.foundSize),
            static_cast<unsigned long long>(job.bundle.size));
    case ContentResolver::Fault::Corrupt:
        return format("%s does not match the manifest", job.bundle.name.c_str());
    default:
        return format("%s is missing", job.bundle.name.c_str());
    }
}

// Where the engine actually mounts archives from. The content system installs into
// `{app}\database` by absolute path, so if fsgame.ltx points $arch_dir$ elsewhere the two
// disagree and a repair would fetch into a directory the game never reads.
std::filesystem::path resolve_archive_directory()
{
    FS_Path* path = nullptr;
    if (!FS.get_path("$arch_dir$", &path) || !path)
        return {};
    return std::filesystem::path(path->m_Path);
}

// Lexical, not std::filesystem::equivalent: equivalent() needs both directories to exist, so a
// missing database\ would be reported as a redirect rather than as what it is.
bool same_directory(const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::wstring a = left.lexically_normal().wstring();
    std::wstring b = right.lexically_normal().wstring();
    for (std::wstring* value : {&a, &b})
    {
        while (!value->empty() && (value->back() == L'\\' || value->back() == L'/'))
            value->pop_back();
        std::ranges::transform(*value, value->begin(), [](wchar_t c) { return towlower(c); });
        std::ranges::replace(*value, L'/', L'\\');
    }
    return a == b;
}

void verify_worker(bool ignoreCache)
{
    ServiceState& instance = service();

    ContentPaths::Layout paths;
    ContentManifest::Manifest manifest;
    {
        std::lock_guard guard(instance.dataMutex);
        paths = instance.paths;
        manifest = instance.manifest;
    }

    // A forced pass drops the advisory cache first: without that it would trust exactly the
    // entries the player asked to have re-checked.
    if (ignoreCache)
        ContentState::SaveCache(paths.State(), manifest.contentId, {});

    CTimer timer;
    timer.Start();

    ContentResolver::Options options;
    options.verifyHashes = true;
    options.cancel = &instance.stopRequested;

    ContentResolver::Plan plan;
    std::string error;
    const bool resolved = ContentResolver::Resolve(manifest, paths, options, plan, error);

    if (!resolved || instance.stopRequested.load(std::memory_order_acquire))
    {
        // Cancelled mid-pass. Leaving the state at Verifying would tell the play gate the
        // installation is fine on the strength of a pass that never finished.
        Msg("* [content] verification cancelled");
        instance.verifyStarted.store(false, std::memory_order_release);
        instance.state.store(ContentService::State::Incomplete, std::memory_order_release);
        return;
    }

    Msg("* [content] verified %u of %u bundle(s), %u hashed, %.1f s", static_cast<u32>(plan.satisfied.size()),
        static_cast<u32>(manifest.bundles.size()), static_cast<u32>(plan.hashed), timer.GetElapsed_sec());

    // Rebuild the advisory cache from what verified, so the next launch is a stat per bundle.
    ContentState::Cache state;
    for (const ContentManifest::Bundle& bundle : plan.satisfied)
    {
        const std::filesystem::path file = paths.Database() / bundle.name;
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(file, sizeError);
        if (sizeError)
            continue;
        state.emplace(bundle.name, ContentState::Entry{bundle.hash, size, ContentState::FileTime(file)});
    }
    ContentState::SaveCache(paths.State(), manifest.contentId, state);

    // The verdict is over EVERYTHING wrong with the installation, not just what this pass
    // found. The initial pass may already have recorded a refused bundle or a redirected
    // archive directory - failures no amount of hashing can retract - and clearing the latch
    // on the strength of a clean hash pass would unblock play on an installation that is still
    // broken, for a reason still printed in the log.
    bool clean = false;
    {
        std::lock_guard guard(instance.dataMutex);

        // Replace this pass's findings rather than append: a re-verify after a repair must be
        // able to retire a hash mismatch, and appending would keep reporting a bundle that is
        // now correct.
        instance.problems.erase(instance.problems.begin() + instance.initialProblems,
            instance.problems.end());
        for (const ContentResolver::Job& job : plan.jobs)
            add_problem(instance, describe(job));
        clean = instance.problems.empty();
    }

    if (clean)
    {
        if (instance.latchedOnly.exchange(false, std::memory_order_acq_rel))
            Msg("* [content] verification passed - the incomplete latch is cleared");
        ContentState::ClearLatch(paths.Latch());
        instance.state.store(ContentService::State::Complete, std::memory_order_release);
    }
    else
    {
        ContentState::WriteLatch(paths.Latch(), manifest.version, ContentState::Reason::VerifyFailed);
        instance.state.store(ContentService::State::Incomplete, std::memory_order_release);
    }
}

// The cheap pass, split out from the public entry point so that every early return still gets
// the same bookkeeping afterwards - in particular the initialProblems mark, which a verify
// pass reads to know which findings are its to rewrite.
void run_initial_pass(ServiceState& instance)
{
    using State = ContentService::State;

    string_path root;
    xr_strcpy(root, Core.ApplicationPath);
    instance.paths.root = std::filesystem::path(root);
    instance.problems.clear();
    instance.notices.clear();
    instance.present = 0;
    instance.missingBytes = 0;

    const auto fail = [&](pcstr line, ContentState::Reason reason, State state)
    {
        add_problem(instance, xr_string(line));
        ContentState::WriteLatch(instance.paths.Latch(), instance.manifest.version, reason);
        instance.state.store(state, std::memory_order_release);
    };

    if (!ContentPin::ManifestLoaded())
    {
        // No manifest means the installation cannot describe itself. That is a broken install
        // with a repair path, not a neutral "this build has no content" - a build that ships
        // content always ships its manifest next to the executable.
        fail("the content manifest is missing or unreadable", ContentState::Reason::ManifestMissing,
            State::Recovery);
        return;
    }

    std::string error;
    if (!ContentManifest::ParseFile(instance.paths.Manifest().wstring(), instance.manifest, error))
    {
        add_problem(instance, format("the content manifest is invalid: %s", error.c_str()));
        ContentState::WriteLatch(instance.paths.Latch(), {}, ContentState::Reason::ManifestInvalid);
        instance.state.store(State::Recovery, std::memory_order_release);
        return;
    }

    if (!instance.manifest.ContentIdMatches())
    {
        fail("the content manifest has been modified - its content-id does not match its bundles",
            ContentState::Reason::ManifestInvalid, State::Recovery);
        return;
    }

    const std::filesystem::path mounted = resolve_archive_directory();
    if (mounted.empty())
    {
        fail("$arch_dir$ is not defined, so content bundles have nowhere to live",
            ContentState::Reason::VerifyFailed, State::Incomplete);
        return;
    }
    if (!same_directory(mounted, instance.paths.Database()))
    {
        // Repair writes to {app}\database. If the engine mounts from somewhere else, fetching
        // content would install it where the game will never look, and the player would
        // re-download forever. Report it instead of entering that loop.
        add_problem(instance, format("$arch_dir$ points at %s instead of %s - content cannot be repaired there",
            mounted.string().c_str(), instance.paths.Database().string().c_str()));
        ContentState::WriteLatch(instance.paths.Latch(), instance.manifest.version,
            ContentState::Reason::VerifyFailed);
        instance.state.store(State::Incomplete, std::memory_order_release);
        return;
    }

    ContentResolver::Options options;
    options.verifyHashes = false; // stat only; the hash pass is the worker's job
    ContentResolver::Plan plan;
    std::string resolveError;
    if (!ContentResolver::Resolve(instance.manifest, instance.paths, options, plan, resolveError))
    {
        add_problem(instance, format("the content installation could not be examined: %s",
            resolveError.c_str()));
        ContentState::WriteLatch(instance.paths.Latch(), instance.manifest.version,
            ContentState::Reason::VerifyFailed);
        instance.state.store(State::Incomplete, std::memory_order_release);
        return;
    }

    instance.present = static_cast<u32>(plan.satisfied.size());
    xr_vector<xr_string> reported;
    for (const ContentResolver::Job& job : plan.jobs)
    {
        add_problem(instance, describe(job));
        reported.emplace_back(job.bundle.name.c_str());
        instance.missingBytes += job.bundle.size;
    }

    // What the mount gate refused matters too - those files exist, are not mounted, and the
    // game is running without them. Only the ones the pass above did not already explain, and
    // it explains them better. What survives here is what the stat pass structurally cannot
    // see: a stale or unrecognised name, or a correctly sized bundle with a corrupt index.
    const auto& skipped = ContentPin::Skipped();
    const auto& reasons = ContentPin::SkipReasons();
    for (size_t index = 0; index != skipped.size(); ++index)
    {
        const xr_string name(skipped[index].c_str());
        if (std::ranges::find(reported, name) != reported.end())
            continue;

        const xr_string line = format("%s was not mounted: %s", name.c_str(),
            index < reasons.size() ? reasons[index].c_str() : "unknown reason");

        // A refused file the manifest declares is content the game needs and cannot use - a
        // correctly sized bundle with a corrupt index is exactly this. A refused file the
        // manifest does not declare is a leftover: it is not mounted, nothing wants it, and
        // refusing to start the game over a stray file would be punishing the player for the
        // installer's mess. The cleanup pass deletes it; the game plays.
        if (instance.manifest.FindByName(name.c_str()))
            add_problem(instance, line);
        else
            add_notice(instance, line);
    }

    if (!instance.problems.empty())
    {
        ContentState::WriteLatch(instance.paths.Latch(), instance.manifest.version,
            ContentState::Reason::VerifyFailed);
        instance.state.store(State::Incomplete, std::memory_order_release);
        return;
    }

    if (ContentState::LatchPresent(instance.paths.Latch()))
    {
        // Every file is where it should be and the right size, but a previous run recorded
        // that it was mid-repair. Only a full hash pass may clear that.
        Msg("* [content] the incomplete latch is set - a full verification pass is required");
        instance.latchedOnly.store(true, std::memory_order_release);
        instance.state.store(State::Incomplete, std::memory_order_release);
    }
    else
    {
        Msg("* [content] %u bundle(s) present, content-id %s", instance.present,
            instance.manifest.contentId.c_str());
        instance.state.store(State::Complete, std::memory_order_release);
    }
}
}

void ContentService::Initialize()
{
    ServiceState& instance = service();
    if (instance.state.load(std::memory_order_acquire) != State::Unknown)
        return;

    run_initial_pass(instance);
    instance.initialProblems = instance.problems.size();

    // Not left to the main menu. A launch that goes straight into a level - `-start`, or the
    // console commands user.ltx runs at startup - never activates the menu, and an
    // installation that was latched would then stay refused for the whole session even though
    // a single pass would have cleared it.
    StartVerify();
}

void ContentService::StartVerify()
{
    Initialize();
    ServiceState& instance = service();

    // Verifying is a permissive state - the play gate lets a level load while the pass runs -
    // so it may only be entered from a picture that is already good. An installation the cheap
    // pass condemned must never be promoted into it, or the gate swings open for the whole
    // duration of the hash pass on exactly the installations it exists to stop. The one
    // exception is the latch-only case, where nothing is known to be wrong and the pass exists
    // to adjudicate a leftover marker.
    const State current = instance.state.load(std::memory_order_acquire);
    const bool latched = instance.latchedOnly.load(std::memory_order_acquire);
    if (current != State::Complete && !(current == State::Incomplete && latched))
        return;
    if (instance.verifyStarted.exchange(true, std::memory_order_acq_rel))
        return;

    // A latched installation stays Incomplete while the pass runs behind it: the gate stays
    // shut, the dialog stays up, and when the pass comes back clean the state flips and the
    // dialog closes itself. Hashing several GB inline instead would freeze the window with no
    // message pump and no feedback for as long as the disk takes.
    if (!latched)
        instance.state.store(State::Verifying, std::memory_order_release);

    if (instance.worker.joinable())
        instance.worker.join();
    instance.worker = std::thread([] { verify_worker(false); });
}

void ContentService::ForceVerify()
{
    Initialize();
    ServiceState& instance = service();
    if (instance.state.load(std::memory_order_acquire) == State::Recovery)
    {
        Msg("! [content] no usable manifest - nothing to verify against");
        return;
    }

    instance.stopRequested.store(true, std::memory_order_release);
    if (instance.worker.joinable())
        instance.worker.join();
    instance.stopRequested.store(false, std::memory_order_release);

    // Off-thread, like the background pass, and deliberately without touching the state: a
    // forced re-hash must not open the play gate for its own duration, and it must not freeze
    // the game for the length of a full content set either. The worker publishes the verdict.
    instance.verifyStarted.store(true, std::memory_order_release);
    instance.worker = std::thread([] { verify_worker(true); });
}

ContentService::Snapshot ContentService::GetSnapshot()
{
    Initialize();
    ServiceState& instance = service();
    Snapshot snapshot;
    snapshot.state = instance.state.load(std::memory_order_acquire);
    {
        std::lock_guard guard(instance.dataMutex);
        snapshot.version = instance.manifest.version.c_str();
        snapshot.contentId = instance.manifest.contentId.c_str();
        snapshot.declared = static_cast<u32>(instance.manifest.bundles.size());
        snapshot.present = instance.present;
        snapshot.missingBytes = instance.missingBytes;
        snapshot.problems = instance.problems;
        snapshot.notices = instance.notices;
    }
    return snapshot;
}

bool ContentService::PlayBlocked()
{
    // Lazily, because the play gate can be reached from user.ltx console commands before the
    // main menu has ever existed. Initialize() is a no-op after the first call.
    Initialize();
    switch (service().state.load(std::memory_order_acquire))
    {
    case State::Complete:
    case State::Verifying:
        // Verifying is deliberately permissive. The cheap pass already confirmed every bundle
        // is present and the right size, and the mount gate already refused anything it did
        // not recognise, so what remains is a hash check - not a reason to make the player
        // wait at the main menu.
        return false;

    default:
        return true;
    }
}

xr_string ContentService::BlockReason()
{
    ServiceState& instance = service();
    if (!PlayBlocked())
        return {};

    std::lock_guard guard(instance.dataMutex);
    if (instance.problems.empty())
        return xr_string("the content installation has not been checked yet");
    return instance.problems.front();
}

void ContentService::LogState()
{
    const Snapshot snapshot = GetSnapshot();
    pcstr name = "unknown";
    switch (snapshot.state)
    {
    case State::Complete: name = "complete"; break;
    case State::Verifying: name = "verifying"; break;
    case State::Incomplete: name = "incomplete"; break;
    case State::Recovery: name = "recovery"; break;
    default: break;
    }

    Msg("~ [content] state=%s version=%s content-id=%s", name,
        snapshot.version.empty() ? "-" : snapshot.version.c_str(),
        snapshot.contentId.empty() ? "-" : snapshot.contentId.c_str());
    Msg("~ [content] bundles present %u of %u, %llu byte(s) missing", snapshot.present, snapshot.declared,
        static_cast<unsigned long long>(snapshot.missingBytes));
    for (const auto& problem : snapshot.problems)
        Msg("~ [content] problem: %s", problem.c_str());
    for (const auto& notice : snapshot.notices)
        Msg("~ [content] notice: %s", notice.c_str());
}

void ContentService::Shutdown()
{
    ServiceState& instance = service();
    instance.stopRequested.store(true, std::memory_order_release);
    if (instance.worker.joinable())
        instance.worker.join();
}
#else
void ContentService::Initialize() {}
void ContentService::StartVerify() {}
void ContentService::ForceVerify() {}
ContentService::Snapshot ContentService::GetSnapshot() { return {}; }
bool ContentService::PlayBlocked() { return false; }
xr_string ContentService::BlockReason() { return {}; }
void ContentService::LogState() {}
void ContentService::Shutdown() {}
#endif
