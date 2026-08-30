#include "StdAfx.h"
#include "ContentService.h"

#include "xrCore/Content/ContentPin.h"
#include "xrContentSync/ContentHash.h"
#include "xrContentSync/ContentManifest.h"

#ifdef XR_PLATFORM_WINDOWS
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
// A stat-only pass over a few dozen names has to stay comfortably under a frame, and a full
// hash pass over several GB has to stay off the main thread. Both are load-bearing: the first
// runs before the play gate can be asked anything, the second while the menu is on screen.
constexpr pcstr LatchFile = "content-incomplete.txt";
constexpr pcstr StateFile = "content-state.txt";

struct CachedState
{
    xr_string hash;
    u64 size{};
    u64 mtime{};
};

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
    // That is the one case where the hash pass has to finish before the question can be
    // answered, so it runs on the calling thread instead of a worker.
    std::atomic_bool latchedOnly{};

    std::mutex dataMutex;
    ContentManifest::Manifest manifest;
    xr_vector<xr_string> problems;
    xr_vector<xr_string> notices;
    std::filesystem::path archiveDirectory;
    std::filesystem::path metaDirectory;
    u32 present{};
    u64 missingBytes{};
    // How many entries of `problems` came from Initialize(). A verify pass owns everything
    // after that mark and rewrites it; what came before is not its to retract.
    size_t initialProblems{};
    std::thread worker;
};

ServiceState& service()
{
    static ServiceState instance;
    return instance;
}

std::wstring wide(const std::filesystem::path& path) { return path.wstring(); }

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

// ---------------------------------------------------------------------------------------
// The latch
//
// Written the moment an installation is known to be incomplete, and removed only when a full
// pass says otherwise. It exists so that a crash between "started fixing content" and
// "finished fixing content" cannot present itself as a healthy installation on the next
// launch - the file outlives the process that discovered the problem.

std::filesystem::path latch_path() { return service().metaDirectory / LatchFile; }

bool latch_present()
{
    std::error_code error;
    return std::filesystem::exists(latch_path(), error) && !error;
}

void set_latch(pcstr reason)
{
    ServiceState& instance = service();
    std::error_code error;
    std::filesystem::create_directories(instance.metaDirectory, error);

    std::ofstream output(latch_path(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        Msg("! [content] could not write %s - the incomplete state will not survive a restart", LatchFile);
        return;
    }

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const u64 stamp = (static_cast<u64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;

    output << "schema=" << ContentManifest::SchemaIncomplete << '\n';
    output << "version=" << (instance.manifest.version.empty() ? "0.0.0" : instance.manifest.version) << '\n';
    output << "reason=" << reason << '\n';
    output << "time=" << stamp << '\n';
}

void clear_latch()
{
    std::error_code error;
    std::filesystem::remove(latch_path(), error);
}

// ---------------------------------------------------------------------------------------
// The advisory state cache
//
// Trusted only on an exact (name, size, mtime) match whose recorded hash is the one the
// manifest wants. Deleting it costs a rescan and never correctness, so it is written
// best-effort and read pessimistically.

std::filesystem::path state_path() { return service().metaDirectory / StateFile; }

u64 file_mtime(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return 0;
    return (static_cast<u64>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
        attributes.ftLastWriteTime.dwLowDateTime;
}

bool parse_u64(std::string_view value, u64& out)
{
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    return error == std::errc{} && end == value.data() + value.size();
}

xr_map<xr_string, CachedState> load_state_cache()
{
    xr_map<xr_string, CachedState> cache;
    std::ifstream input(state_path(), std::ios::binary);
    if (!input)
        return cache;

    std::string line;
    if (!std::getline(input, line))
        return cache;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line != std::string("schema=") + ContentManifest::SchemaState)
        return cache;

    // Line 2 is content-id. It is recorded for diagnostics only and deliberately not used to
    // invalidate: versions share bundles, so throwing the cache away on every content-id
    // change would rehash several GB on every single update for no gain.
    std::getline(input, line);

    while (std::getline(input, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            continue;

        std::string_view view(line);
        std::string_view fields[4];
        size_t count = 0;
        size_t start = 0;
        while (count < 4)
        {
            const size_t tab = view.find('\t', start);
            if (tab == std::string_view::npos)
            {
                fields[count++] = view.substr(start);
                break;
            }
            fields[count++] = view.substr(start, tab - start);
            start = tab + 1;
        }
        if (count != 4)
            continue;

        CachedState entry;
        if (!ContentManifest::IsSha256Hex(fields[0]) || !parse_u64(fields[1], entry.size) ||
            !parse_u64(fields[2], entry.mtime) || fields[3].empty())
            continue;
        entry.hash.assign(fields[0].data(), fields[0].size());
        cache.emplace(xr_string(fields[3].data(), fields[3].size()), entry);
    }
    return cache;
}

void save_state_cache(const xr_map<xr_string, CachedState>& cache)
{
    ServiceState& instance = service();
    std::error_code error;
    std::filesystem::create_directories(instance.metaDirectory, error);

    std::ofstream output(state_path(), std::ios::binary | std::ios::trunc);
    if (!output)
        return;

    output << "schema=" << ContentManifest::SchemaState << '\n';
    output << "content-id=" << instance.manifest.contentId << '\n';
    for (const auto& [name, entry] : cache)
        output << entry.hash.c_str() << '\t' << entry.size << '\t' << entry.mtime << '\t' << name.c_str() << '\n';
}

// ---------------------------------------------------------------------------------------

// The bundles live where the engine mounts archives from. Reading that out of the FS rather
// than assuming `{app}\database` keeps verification honest: if a player has redirected
// $arch_dir$, the directory this checks is the directory the game actually loads, and the
// mismatch below is reported instead of producing a verifier that agrees with itself while
// the game starves.
std::filesystem::path resolve_archive_directory()
{
    FS_Path* path = nullptr;
    if (!FS.get_path("$arch_dir$", &path) || !path)
        return {};
    return std::filesystem::path(path->m_Path);
}

std::filesystem::path expected_archive_directory()
{
    string_path root;
    xr_strcpy(root, Core.ApplicationPath);
    return std::filesystem::path(root) / L"database";
}

// Lexical, not std::filesystem::equivalent: equivalent() needs both directories to exist, so
// a missing database\ would be reported as a redirect rather than as what it is.
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

// Recomputes content-id from the bundle list: SHA-256 over "<name>\n<sha256>\n" per bundle,
// sorted by name. A manifest whose own id does not match has been edited by hand, and every
// claim in it is then worth exactly nothing.
bool content_id_matches(const ContentManifest::Manifest& manifest)
{
    xr_vector<const ContentManifest::Bundle*> sorted;
    sorted.reserve(manifest.bundles.size());
    for (const auto& bundle : manifest.bundles)
        sorted.push_back(&bundle);
    std::ranges::sort(sorted, [](const auto* a, const auto* b) { return a->name < b->name; });

    std::string blob;
    for (const auto* bundle : sorted)
    {
        blob += bundle->name;
        blob += '\n';
        blob += bundle->hash;
        blob += '\n';
    }
    return ContentHash::Buffer(blob.data(), blob.size()) == manifest.contentId;
}

void verify_worker(bool ignoreCache)
{
    ServiceState& instance = service();

    std::filesystem::path directory;
    ContentManifest::Manifest manifest;
    {
        std::lock_guard guard(instance.dataMutex);
        directory = instance.archiveDirectory;
        manifest = instance.manifest;
    }

    const auto cache = ignoreCache ? xr_map<xr_string, CachedState>{} : load_state_cache();
    xr_map<xr_string, CachedState> refreshed;
    xr_vector<xr_string> problems;
    u32 hashed = 0;
    bool aborted = false;

    CTimer timer;
    timer.Start();

    for (const auto& bundle : manifest.bundles)
    {
        if (instance.stopRequested.load(std::memory_order_acquire))
        {
            // Shutting down mid-pass. Leaving the state at Verifying would tell the play gate
            // the installation is fine on the strength of a pass that never finished.
            aborted = true;
            break;
        }

        const std::filesystem::path file = directory / bundle.name;
        const u64 mtime = file_mtime(file);

        std::error_code sizeError;
        CachedState entry;
        entry.size = std::filesystem::file_size(file, sizeError);
        entry.mtime = mtime;
        if (sizeError)
            entry.size = 0;

        // The recorded size has to be the size on disk, not the size the manifest asked for:
        // comparing the manifest against itself would reduce the trust rule to (name, mtime)
        // and let a same-mtime replacement of a different length ride the cache through.
        const auto cached = cache.find(xr_string(bundle.name.c_str()));
        const bool trusted = cached != cache.end() && entry.size == bundle.size &&
            cached->second.size == entry.size && cached->second.mtime == mtime && mtime != 0 &&
            cached->second.hash == xr_string(bundle.hash.c_str());
        if (trusted)
        {
            entry.hash = cached->second.hash;
        }
        else
        {
            entry.hash = ContentHash::File(wide(file), instance.stopRequested).c_str();
            ++hashed;
        }

        if (entry.hash != xr_string(bundle.hash.c_str()))
        {
            if (instance.stopRequested.load(std::memory_order_acquire))
            {
                // An empty digest here means the hash was cancelled, not that the bundle is
                // wrong. Reporting it would fabricate corruption out of a clean shutdown.
                aborted = true;
                break;
            }
            problems.push_back(format("%s does not match the manifest", bundle.name.c_str()));
            continue;
        }
        refreshed.emplace(xr_string(bundle.name.c_str()), entry);
    }

    if (aborted)
    {
        Msg("* [content] verification cancelled after %u bundle(s)", hashed);
        instance.verifyStarted.store(false, std::memory_order_release);
        instance.state.store(ContentService::State::Incomplete, std::memory_order_release);
        return;
    }

    Msg("* [content] verified %u bundle(s), %u hashed, %.1f s", static_cast<u32>(manifest.bundles.size()),
        hashed, timer.GetElapsed_sec());

    save_state_cache(refreshed);

    // The verdict is over EVERYTHING wrong with the installation, not just what this pass
    // found. Initialize() may already have recorded a refused bundle or a bad archive
    // directory - failures no amount of hashing can retract - and clearing the latch on the
    // strength of a clean hash pass would unblock play on an installation that is still
    // broken, for a reason still printed in the log.
    bool clean = false;
    {
        std::lock_guard guard(instance.dataMutex);

        // Replace this pass's findings rather than append: a re-verify after a repair must be
        // able to retire a hash mismatch, and appending would keep reporting a bundle that is
        // now correct. Everything Initialize() found stays - those are separate facts, and
        // only a fresh Initialize() can retire them.
        instance.problems.erase(instance.problems.begin() + instance.initialProblems,
            instance.problems.end());
        for (const auto& line : problems)
            add_problem(instance, line);
        clean = instance.problems.empty();
    }

    if (clean)
    {
        if (instance.latchedOnly.exchange(false, std::memory_order_acq_rel))
            Msg("* [content] verification passed - the incomplete latch is cleared");
        clear_latch();
        instance.state.store(ContentService::State::Complete, std::memory_order_release);
    }
    else
    {
        set_latch("verify-failed");
        instance.state.store(ContentService::State::Incomplete, std::memory_order_release);
    }
}
}

namespace
{
// The cheap pass, split out from the public entry point so that every early return still gets
// the same bookkeeping afterwards - in particular the initialProblems mark, which a verify
// pass reads to know which findings are its to rewrite.
void run_initial_pass(ServiceState& instance)
{
    using State = ContentService::State;
    instance.metaDirectory = std::filesystem::path(ContentPin::MetaDirectory());
    instance.problems.clear();
    instance.notices.clear();
    instance.present = 0;
    instance.missingBytes = 0;

    if (!ContentPin::ManifestLoaded())
    {
        // No manifest means the installation cannot describe itself. That is a broken install
        // with a repair path, not a neutral "this build has no content" - a build that ships
        // content always ships its manifest next to the executable.
        add_problem(instance, "the content manifest is missing or unreadable");
        set_latch("manifest-missing");
        instance.state.store(State::Recovery, std::memory_order_release);
        return;
    }

    std::string error;
    if (!ContentManifest::ParseFile(wide(instance.metaDirectory / "content-manifest.txt"), instance.manifest, error))
    {
        add_problem(instance, format("the content manifest is invalid: %s", error.c_str()));
        set_latch("manifest-invalid");
        instance.state.store(State::Recovery, std::memory_order_release);
        return;
    }

    if (!content_id_matches(instance.manifest))
    {
        add_problem(instance, "the content manifest has been modified - its content-id does not match its bundles");
        set_latch("manifest-invalid");
        instance.state.store(State::Recovery, std::memory_order_release);
        return;
    }

    instance.archiveDirectory = resolve_archive_directory();
    const std::filesystem::path expected = expected_archive_directory();
    if (instance.archiveDirectory.empty())
    {
        add_problem(instance, "$arch_dir$ is not defined, so content bundles have nowhere to live");
        set_latch("verify-failed");
        instance.state.store(State::Incomplete, std::memory_order_release);
        return;
    }
    if (!same_directory(instance.archiveDirectory, expected))
    {
        // Repair writes to {app}\database. If the engine mounts from somewhere else, fetching
        // content would install it where the game will never look, and the player would
        // re-download forever. Report it instead of entering that loop.
        add_problem(instance, format("$arch_dir$ points at %s instead of %s - content cannot be repaired there",
            instance.archiveDirectory.string().c_str(), expected.string().c_str()));
        set_latch("verify-failed");
        instance.state.store(State::Incomplete, std::memory_order_release);
        return;
    }

    xr_vector<xr_string> reported;
    for (const auto& bundle : instance.manifest.bundles)
    {
        std::error_code sizeError;
        const std::filesystem::path file = instance.archiveDirectory / bundle.name;
        const auto size = std::filesystem::file_size(file, sizeError);
        if (sizeError)
        {
            add_problem(instance, format("%s is missing", bundle.name.c_str()));
            reported.emplace_back(bundle.name.c_str());
            instance.missingBytes += bundle.size;
            continue;
        }
        if (size != bundle.size)
        {
            add_problem(instance, format("%s is %llu bytes, the manifest says %llu", bundle.name.c_str(),
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(bundle.size)));
            reported.emplace_back(bundle.name.c_str());
            instance.missingBytes += bundle.size;
            continue;
        }
        ++instance.present;
    }

    // What the mount gate refused matters too - those files exist, are not mounted, and the
    // game is running without them. Only the ones the pass above did not already explain, and
    // it explains them better: "is 230609 bytes, the manifest says 234705" beats "size does not
    // match the manifest". What survives here is what the stat pass structurally cannot see -
    // a stale or unrecognised name, or a correctly sized bundle with a corrupt index.
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
        set_latch("verify-failed");
        instance.state.store(State::Incomplete, std::memory_order_release);
        return;
    }

    if (latch_present())
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

    // Only a clean picture may enter the permissive Verifying state. A latched installation
    // stays Incomplete while the same pass runs behind it: the gate stays shut, the dialog
    // stays up, and when the pass comes back clean the state flips and the dialog closes
    // itself. Hashing several GB inline instead would freeze the window with no message pump
    // and no feedback for as long as the disk takes.
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
        // Verifying is deliberately permissive. The synchronous pass already confirmed every
        // bundle is present and the right size, and the mount gate already refused anything
        // it did not recognise, so what remains is a hash check - not a reason to make the
        // player wait at the main menu.
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
