// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "ModUpdateService.h"

#include "ContentService.h"
#include "ModCatalog.h"
#include "ModHttp.h"
#include "ModRelease.h"

#include "xrContentSync/ContentHash.h"
#include "xrCore/ProductVersion.h"
#include "xrCore/XMS/xms_catalog.h"
#include "xrCore/XMS/xms_core.h"
#include "xrEngine/XR_IOConsole.h"

#ifdef XR_PLATFORM_WINDOWS
#include <shellapi.h>
#include <winhttp.h>

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
using ModUpdateService::ModuleStatus;
using ModUpdateService::State;

constexpr std::wstring_view ModReleaseHost = L"https://github.com";
// consecutive failures a download survives without having finished a single file in between
constexpr unsigned ModDownloadAttempts = 5;
// Two wanted files this close inside a package travel in one request: on a slow line the bytes
// skipped between them cost about what another round trip through the redirects would.
constexpr u64 ModRangeGap = 256 * 1024;

// what the check learned about a release, kept for the update that usually follows
struct ModKnownIndex
{
    xr_string sha256;
    std::shared_ptr<const ModRelease::FileIndex> index;
};

// What comparing an installed module with a release of its own version found. The comparison
// reads the whole module, so its answer is kept for as long as it holds: the same release (the
// hash of its index) over the same installation (the manifest's size and time).
struct ModVerdict
{
    xr_string id;
    xr_string index;
    xr_string stamp;
    bool same{};
    u64 bytes{}; // !same: what the update is expected to download
};

struct ModService
{
    ~ModService()
    {
        stop.store(true, std::memory_order_release);
        if (worker.joinable())
            worker.join();
    }

    std::mutex mutex;
    xr_vector<ModuleStatus> modules;
    std::deque<size_t> queue;
    // Mod Browser installations: their statuses, the ones waiting, the one in flight
    xr_vector<ModuleStatus> installs;
    std::deque<xr_string> installQueue;
    xr_string activeInstall;
    // console requests waiting for the check to say what is available
    xr_vector<xr_string> requestedIds;
    bool requestedAll{};
    bool checkRequested{};
    bool workerRunning{};
    u32 stagedThisRun{};

    // worker thread only: one worker runs at a time, so these need no lock
    xr_vector<ModKnownIndex> indexes;
    xr_vector<ModVerdict> verdicts;
    bool verdictsLoaded{};

    bool initialized{};
    std::atomic_bool stop{};
    std::atomic_bool restartPrompt{};
    std::atomic<u64> progressDone{};
    std::atomic<u64> progressTotal{};
    std::thread worker;
};

ModService& mod_service()
{
    static ModService instance;
    return instance;
}

// The status slot of the installation in flight: an installed module has a row of XMS::Modules(),
// a module being installed has none yet.
constexpr size_t ModInstallSlot = static_cast<size_t>(-1);

template <typename Change>
void mod_set_status(size_t index, const Change& change)
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    if (index != ModInstallSlot)
    {
        change(service.modules[index]);
        return;
    }
    for (ModuleStatus& status : service.installs)
        if (status.id == service.activeInstall)
            change(status);
}

void mod_set_state(size_t index, State state, xr_string message = {})
{
    mod_set_status(index, [&](ModuleStatus& status)
    {
        status.state = state;
        status.message = std::move(message);
    });
}

bool mod_stopped() { return mod_service().stop.load(std::memory_order_acquire); }

std::wstring mod_widen(std::string_view text)
{
    // every string that reaches a URL here has already been held to an ASCII grammar
    return std::wstring(text.begin(), text.end());
}

std::filesystem::path mod_utf8_path(std::string_view text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return std::filesystem::path(std::move(wide)).make_preferred();
}

// QA only: a loopback stand-in for github.com, judged on the parsed host - the string
// "http://127.0.0.1:@evil.example/" starts with the right characters and points elsewhere.
std::wstring mod_qa_base()
{
    if (!Core.Params || !strstr(Core.Params, "-qa_update"))
        return {};
    std::array<wchar_t, 512> value{};
    const DWORD length = GetEnvironmentVariableW(L"DAR_QA_MOD_UPDATE_BASE", value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size())
        return {};

    std::array<wchar_t, 256> host{};
    std::array<wchar_t, 8> user{};
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUserName = user.data();
    parts.dwUserNameLength = static_cast<DWORD>(user.size());
    if (!WinHttpCrackUrl(value.data(), length, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTP || user[0])
        return {};
    if (_wcsicmp(host.data(), L"127.0.0.1") != 0 && _wcsicmp(host.data(), L"localhost") != 0)
        return {};

    std::wstring base(value.data(), length);
    while (!base.empty() && base.back() == L'/')
        base.pop_back();
    return base;
}

// The one URL shape of the contract: the asset of whatever release GitHub calls Latest.
std::wstring mod_asset_url(const xr_string& repository, const xr_string& asset)
{
    std::wstring url = mod_qa_base();
    if (url.empty())
        url = ModReleaseHost;
    return url.append(L"/").append(mod_widen(repository)).append(L"/releases/latest/download/").append(mod_widen(asset));
}

// The shared client (ModHttp): one session for a worker's whole run - WinHTTP pools connections
// per session, and an update made of ranged requests is mostly connection setup without that.
using ModHttpSession = ModHttp::Session;
using ModHttpGet = ModHttp::Request;
using ModFetch = ModHttp::Fetch;

// a small text asset, whole, into memory
ModFetch mod_fetch_text(ModHttpSession& session, const std::wstring& url, size_t limit, xr_string& body, xr_string& error)
{
    const ModFetch fetched = ModHttp::FetchText(session, url, limit, body, error, &mod_service().stop);
    if (fetched == ModFetch::Failed && error == "the answer is larger than allowed")
        error = "asset is larger than the contract allows";
    return fetched;
}

ModFetch mod_fetch_descriptor(ModHttpSession& session, const XMS::Module& module, ModRelease::Descriptor& descriptor,
    xr_string& error, xr_string* text = nullptr)
{
    xr_string body;
    const ModFetch fetched = mod_fetch_text(session, mod_asset_url(module.update_github, module.id + ".update.ltx"),
        ModRelease::MaximumDescriptorBytes, body, error);
    if (fetched != ModFetch::Ok)
        return fetched;
    if (text)
        *text = body;
    if (!ModRelease::ParseDescriptor(body, descriptor, error))
        return ModFetch::Failed;
    if (descriptor.id != module.id)
    {
        error = "descriptor belongs to another module";
        return ModFetch::Failed;
    }
    return ModFetch::Ok;
}

// The index the descriptor names: the one the check already fetched when it is still the same
// file, a fresh download otherwise.
std::shared_ptr<const ModRelease::FileIndex> mod_fetch_index(ModHttpSession& session, size_t moduleIndex,
    const XMS::Module& module, const ModRelease::Descriptor& descriptor, xr_string& error)
{
    ModKnownIndex scratch;
    ModKnownIndex& known = moduleIndex == ModInstallSlot ? scratch : mod_service().indexes[moduleIndex];
    if (known.index && known.sha256 == descriptor.index.sha256)
        return known.index;

    xr_string body;
    if (mod_fetch_text(session, mod_asset_url(module.update_github, descriptor.index.name),
            static_cast<size_t>(descriptor.index.size), body, error) != ModFetch::Ok)
    {
        if (error.empty())
            error = "release has no file index";
        return nullptr;
    }
    if (body.size() != descriptor.index.size || ContentHash::Buffer(body.data(), body.size()) != descriptor.index.sha256.c_str())
    {
        error = "file index does not match the descriptor";
        return nullptr;
    }

    auto index = std::make_shared<ModRelease::FileIndex>();
    if (!ModRelease::ParseFileIndex(body, descriptor, *index, error))
        return nullptr;
    known.sha256 = descriptor.index.sha256;
    known.index = index;
    return index;
}

// What the player is told before they press Update: the packed size of every file the module
// does not already hold under the same path and size. Cheap on purpose - no hashing at startup.
u64 mod_estimate_download(const XMS::Module& module, const ModRelease::FileIndex& index)
{
    const std::filesystem::path root(module.root.c_str());
    u64 bytes = 0;
    for (const ModRelease::FileEntry& entry : index.files)
    {
        std::error_code error;
        const u64 size = std::filesystem::file_size(root / mod_utf8_path(entry.path), error);
        // the manifest of another version is another file even when "1.0.0" became "1.0.1"
        // and the size stayed put
        if (error || size != entry.size || 0 == xr_stricmp(entry.path.c_str(), "mod.ltx"))
            bytes += entry.packed;
    }
    return bytes;
}

// ---- a release of the version that is already installed -----------------------------------
//
// An author who takes a release down and publishes the same version again means the players
// who have it to get the new files. Versions cannot tell the two releases apart; content can.

// <game>\modules\.update_state: "<id>\t<index sha256>\t<stamp>\t<same 0|1>\t<bytes>" per line
std::filesystem::path mod_verdict_file()
{
    return std::filesystem::path(XMS::StagedRoot().c_str()).parent_path().parent_path() / L".update_state";
}

void mod_load_verdicts(ModService& service)
{
    if (service.verdictsLoaded)
        return;
    service.verdictsLoaded = true;
    FILE* file = nullptr;
    if (_wfopen_s(&file, mod_verdict_file().c_str(), L"rb") || !file)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), file) && service.verdicts.size() < 4096)
    {
        ModVerdict verdict;
        xr_string* fields[] = {&verdict.id, &verdict.index, &verdict.stamp};
        pcstr cursor = line;
        bool whole = true;
        for (xr_string* field : fields)
        {
            pcstr tab = strchr(cursor, '\t');
            if (!(whole = tab != nullptr))
                break;
            field->assign(cursor, tab);
            cursor = tab + 1;
        }
        unsigned same = 0;
        unsigned long long bytes = 0;
        if (!whole || sscanf_s(cursor, "%u\t%llu", &same, &bytes) != 2 || verdict.id.empty())
            continue;
        verdict.same = same != 0;
        verdict.bytes = bytes;
        service.verdicts.push_back(std::move(verdict));
    }
    fclose(file);
}

void mod_save_verdicts(const ModService& service)
{
    const std::filesystem::path path = mod_verdict_file();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    // a lost file costs one more comparison, so no temp-and-rename ceremony
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") || !file)
        return;
    for (const ModVerdict& verdict : service.verdicts)
        fprintf(file, "%s\t%s\t%s\t%u\t%llu\n", verdict.id.c_str(), verdict.index.c_str(), verdict.stamp.c_str(),
            verdict.same ? 1u : 0u, static_cast<unsigned long long>(verdict.bytes));
    fclose(file);
}

// Staging leaves the installed module where it is until the next start, and whether that start
// swaps it in is not known here. The verdict is dropped rather than guessed; the first check
// after the swap compares once and writes the true one.
void mod_forget_verdict(ModService& service, const xr_string& id)
{
    mod_load_verdicts(service);
    const auto stale = std::ranges::remove_if(service.verdicts, [&](const ModVerdict& verdict) { return verdict.id == id; });
    if (stale.empty())
        return;
    service.verdicts.erase(stale.begin(), stale.end());
    mod_save_verdicts(service);
}

xr_string mod_manifest_stamp(const XMS::Module& module)
{
    const std::filesystem::path manifest = std::filesystem::path(module.root.c_str()) / L"mod.ltx";
    std::error_code error;
    const u64 size = std::filesystem::file_size(manifest, error);
    if (error)
        return {};
    const auto written = std::filesystem::last_write_time(manifest, error);
    if (error)
        return {};
    string64 text;
    xr_sprintf(text, "%llu:%lld", static_cast<unsigned long long>(size),
        static_cast<long long>(written.time_since_epoch().count()));
    return text;
}

enum class ModCompare
{
    Same,
    Differs,
    Cancelled
};

// Every file the release lists, where the release wants it: the size first - another size
// settles it without a read - then the content. Files the module holds beyond the release do not
// count; a stray desktop.ini is not an update. `bytes` is the packed size of the content that
// did not match, each content once, which is what staging would go and fetch.
ModCompare mod_compare_installed(const XMS::Module& module, const ModRelease::FileIndex& index, u64& bytes)
{
    ModService& service = mod_service();
    const std::filesystem::path root(module.root.c_str());
    std::unordered_set<std::string> wanted;
    std::vector<std::pair<const ModRelease::FileEntry*, std::filesystem::path>> sized;
    u64 total = 0;
    bytes = 0;
    for (const ModRelease::FileEntry& entry : index.files)
    {
        std::filesystem::path path = root / mod_utf8_path(entry.path);
        std::error_code error;
        const u64 size = std::filesystem::file_size(path, error);
        if (error || size != entry.size)
        {
            if (wanted.insert(entry.sha256.c_str()).second)
                bytes += entry.packed;
            continue;
        }
        total += size;
        sized.emplace_back(&entry, std::move(path));
    }

    service.progressDone.store(0, std::memory_order_release);
    service.progressTotal.store(total, std::memory_order_release);
    for (const auto& [entry, path] : sized)
    {
        const std::string digest = ContentHash::File(path.wstring(), service.stop);
        if (mod_stopped())
            return ModCompare::Cancelled;
        if (digest != entry->sha256.c_str() && wanted.insert(entry->sha256.c_str()).second)
            bytes += entry->packed;
        service.progressDone.fetch_add(entry->size, std::memory_order_acq_rel);
    }
    return wanted.empty() ? ModCompare::Same : ModCompare::Differs;
}

// True when the release is the installed version with other content. A comparison that could
// not be made answers false: nothing is offered without proof.
bool mod_reissued(ModHttpSession& session, size_t moduleIndex, const XMS::Module& module,
    const ModRelease::Descriptor& descriptor, u64& bytes)
{
    ModService& service = mod_service();
    mod_load_verdicts(service);
    const xr_string stamp = mod_manifest_stamp(module);
    for (const ModVerdict& verdict : service.verdicts)
    {
        if (verdict.id != module.id || stamp.empty() || verdict.stamp != stamp || verdict.index != descriptor.index.sha256)
            continue;
        bytes = verdict.bytes;
        return !verdict.same;
    }

    xr_string error;
    const auto files = mod_fetch_index(session, moduleIndex, module, descriptor, error);
    if (!files)
    {
        Msg("* [mods] %s: the release of the installed version could not be compared - %s", module.id.c_str(),
            error.c_str());
        return false;
    }
    mod_set_state(moduleIndex, State::Preparing);
    const ModCompare compared = mod_compare_installed(module, *files, bytes);
    if (compared == ModCompare::Cancelled)
        return false;

    if (compared == ModCompare::Same)
        Msg("* [mods] %s: the installed files are release %s", module.id.c_str(), descriptor.version.c_str());
    if (!stamp.empty())
    {
        const auto stale = std::ranges::remove_if(service.verdicts, [&](const ModVerdict& verdict) { return verdict.id == module.id; });
        service.verdicts.erase(stale.begin(), stale.end());
        service.verdicts.push_back({module.id, descriptor.index.sha256, stamp, compared == ModCompare::Same, bytes});
        mod_save_verdicts(service);
    }
    return compared == ModCompare::Differs;
}

// a release this build cannot read, or one that asks for a newer game
bool mod_release_blocked(const ModRelease::Descriptor& descriptor)
{
    return descriptor.schema > ModRelease::SupportedSchema ||
        (!descriptor.requiresGame.empty() &&
            ModRelease::ParseVersion(descriptor.requiresGame) > ModRelease::ParseVersion(DeadAirRefined::Version));
}

// Turns a descriptor into the state the Mods menu shows. True when the release can be taken.
// `reissue`: the release is the installed version with other content - see mod_reissued.
bool mod_evaluate(size_t index, const xr_string& installedVersion, const ModRelease::Descriptor& descriptor,
    u64 downloadBytes, bool reissue = false)
{
    const bool ahead = ModRelease::ParseVersion(descriptor.version) > ModRelease::ParseVersion(installedVersion);
    reissue = reissue && !ahead;
    const bool newer = ahead || reissue;
    const bool unreadable = descriptor.schema > ModRelease::SupportedSchema;
    const bool gameTooOld = !unreadable && !descriptor.requiresGame.empty() &&
        ModRelease::ParseVersion(descriptor.requiresGame) > ModRelease::ParseVersion(DeadAirRefined::Version);

    mod_set_status(index, [&](ModuleStatus& status)
    {
        status.message.clear();
        status.version = newer ? descriptor.version : xr_string();
        status.reissue = reissue;
        status.requiresGame = newer && gameTooOld ? descriptor.requiresGame : xr_string();
        status.totalBytes = newer && !unreadable ? downloadBytes : 0;
        status.downloadedBytes = 0;
        status.state = !newer ? State::Current : (unreadable || gameTooOld) ? State::Blocked : State::Available;
    });
    return newer && !unreadable && !gameTooOld;
}

// ---- the mod catalog (docs/dead-air/MOD_CATALOG.md) -----------------------------------------

// A module the catalog lists under its own repository, or one the Mod Browser installed.
bool mod_is_catalog(const XMS::Module& module)
{
    return XMS::Catalog::InstalledFromBrowser(module.id.c_str()) || XMS::Catalog::Listed(module);
}

// The catalog's word on a release of a catalog module: listed under this repository, not revoked,
// not below the lowest listed version, signed by the author key the catalog names. Empty = taken.
xr_string mod_catalog_refusal(const XMS::Module& module, std::string_view text, const ModRelease::Descriptor& descriptor)
{
    const std::shared_ptr<const XMS::Catalog::Index> index = XMS::Catalog::Current();
    const XMS::Catalog::Listing* listing = index ? index->Find(module.id) : nullptr;
    if (!listing)
        return index ? "the mod catalog no longer lists this module" : "the mod catalog has not been loaded yet";
    if (0 != xr_stricmp(listing->github.c_str(), module.update_github.c_str()))
        return "the catalog names another repository for this module";
    if (const XMS::Catalog::Revocation* revoked = index->Revoked(module.id, descriptor.version, listing->key.id))
        return "withdrawn from the mod catalog: " + (revoked->reason.empty() ? xr_string("no reason given") : revoked->reason);
    if (ModRelease::ParseVersion(descriptor.version) < ModRelease::ParseVersion(listing->minVersion))
        return "below the lowest version the catalog lists (" + listing->minVersion + ")";
    xr_string error;
    if (!XMS::Catalog::VerifySigned(text, listing->key, error))
        return "not signed by the author's key the catalog names (" + error + ")";
    return {};
}

// What the index of a catalog release may hold (MOD_CATALOG 8, paths) and that its card and its
// manifest are one text.
xr_string mod_catalog_index_refusal(const ModRelease::Descriptor& descriptor, const ModRelease::FileIndex& index)
{
    for (const ModRelease::FileEntry& entry : index.files)
    {
        xr_string why;
        if (!ModRelease::CatalogPathAllowed(entry.path, why))
            return entry.path + ": " + why;
        if (entry.path == "mod.ltx" && descriptor.card.manifest.present &&
            (descriptor.card.manifest.sha256 != entry.sha256 || descriptor.card.manifest.size != entry.size))
            return "the card does not describe this release's mod.ltx";
    }
    return {};
}

void mod_log_disclaimer()
{
    Msg("~ [mods] Modules are made by third parties. The author of Dead Air: Refined is not responsible for "
        "them: the safety measures of the mod catalog are taken, but there is no absolute guarantee.");
}

bool mod_sleep(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds && !mod_stopped(); elapsed += 100)
        Sleep(100);
    return !mod_stopped();
}

// A hard link where the volume has them: the staged module is the installed one plus what
// changed, and copying the unchanged gigabytes to say so would be the slow part of an update.
bool mod_reuse_file(const std::filesystem::path& source, const std::filesystem::path& target, bool& linksWork)
{
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (linksWork && CreateHardLinkW(target.c_str(), source.c_str(), nullptr))
        return true;
    if (linksWork && GetLastError() == ERROR_ALREADY_EXISTS)
        return false;
    linksWork = false;
    return CopyFileW(source.c_str(), target.c_str(), TRUE) != FALSE;
}

// Installed files by content. Only files whose size the release names are read at all, and what
// comes back is what was actually hashed just now - nothing on disk is taken on trust.
bool mod_hash_installed(const std::filesystem::path& root, const ModRelease::FileIndex& index,
    std::unordered_map<std::string, std::filesystem::path>& byHash)
{
    ModService& service = mod_service();
    std::unordered_set<u64> sizes;
    for (const ModRelease::FileEntry& entry : index.files)
        sizes.insert(entry.size);

    std::vector<std::pair<std::filesystem::path, u64>> candidates;
    u64 total = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error))
    {
        std::error_code entryError;
        if (!it->is_regular_file(entryError))
            continue;
        const u64 size = it->file_size(entryError);
        if (entryError || !sizes.contains(size))
            continue;
        candidates.emplace_back(it->path(), size);
        total += size;
    }

    service.progressDone.store(0, std::memory_order_release);
    service.progressTotal.store(total, std::memory_order_release);
    for (const auto& [path, size] : candidates)
    {
        const std::string digest = ContentHash::File(path.wstring(), service.stop);
        if (mod_stopped())
            return false;
        if (!digest.empty())
            byHash.try_emplace(digest, path);
        service.progressDone.fetch_add(size, std::memory_order_acq_rel);
    }
    return true;
}

enum class ModRead
{
    Done,
    Network,  // worth another attempt
    Rejected  // the bytes arrived and are wrong: another attempt would fetch the same ones
};

// The file a package download is in the middle of. It outlives a dropped connection: the sink
// keeps its unpacking state, so the next request asks for the byte after the last one it took.
struct ModTransfer
{
    ModRelease::FileSink sink;
    size_t next{};   // first wanted file that is not finished
    u64 taken{};     // packed bytes of that file already fed to the sink
    bool open{};
    unsigned failures{};
};

// Consumes one ranged response: skips to each wanted file in turn and feeds it to the sink.
ModRead mod_read_span(ModHttpGet& request, u64 position, const xr_vector<const ModRelease::FileEntry*>& wanted,
    size_t last, const std::filesystem::path& payload, ModTransfer& transfer, xr_string& error)
{
    ModService& service = mod_service();
    std::vector<u8> buffer(256 * 1024);

    // pulls exactly `count` bytes off the response, handing each piece to `consume`
    const auto pull = [&](u64 count, const auto& consume)
    {
        while (count)
        {
            if (mod_stopped())
                return ModRead::Network;
            DWORD received = 0;
            const DWORD want = static_cast<DWORD>(std::min<u64>(count, buffer.size()));
            if (!request.Read(buffer.data(), want, received) || !received)
            {
                error = "download was interrupted";
                return ModRead::Network;
            }
            if (!consume(buffer.data(), received))
                return ModRead::Rejected;
            count -= received;
            position += received;
        }
        return ModRead::Done;
    };

    while (transfer.next <= last)
    {
        const ModRelease::FileEntry& entry = *wanted[transfer.next];
        const u64 resumeAt = entry.offset + transfer.taken;
        if (resumeAt < position)
        {
            error = "file index names overlapping data";
            return ModRead::Rejected;
        }
        ModRead result = pull(resumeAt - position, [](const u8*, DWORD) { return true; });
        if (result != ModRead::Done)
            return result;

        if (!transfer.open && !transfer.sink.Open(payload / mod_utf8_path(entry.path), entry, error))
            return ModRead::Rejected;
        transfer.open = true;
        result = pull(entry.packed - transfer.taken, [&](const u8* data, DWORD size)
        {
            // bytes that arrived are progress: a long file on a bad line is not a failing one
            transfer.failures = 0;
            transfer.taken += size;
            service.progressDone.fetch_add(size, std::memory_order_acq_rel);
            return transfer.sink.Append(data, size, error);
        });
        if (result != ModRead::Done)
            return result;
        if (!transfer.sink.Finish(error))
            return ModRead::Rejected;

        transfer.open = false;
        transfer.taken = 0;
        ++transfer.next;
    }
    return ModRead::Done;
}

// Everything one package has to give, in ascending order of offset.
bool mod_fetch_package(ModHttpSession& session, const std::wstring& canonicalUrl,
    const xr_vector<const ModRelease::FileEntry*>& wanted, const std::filesystem::path& payload, xr_string& error)
{
    // The storage URL the first request was redirected to answers ranged requests directly,
    // which saves two redirects per request. It is signed and expires, so a refusal from it
    // just sends the next request the long way round again.
    std::wstring directUrl;
    ModTransfer transfer;
    while (transfer.next != wanted.size())
    {
        if (transfer.failures == ModDownloadAttempts || (transfer.failures && !mod_sleep(500u << transfer.failures)) ||
            mod_stopped())
        {
            if (mod_stopped())
                error = "cancelled";
            return false;
        }

        size_t last = transfer.next;
        const u64 begin = wanted[last]->offset + transfer.taken;
        u64 end = wanted[last]->offset + wanted[last]->packed;
        while (last + 1 != wanted.size() && wanted[last + 1]->offset <= end + ModRangeGap)
        {
            ++last;
            end = std::max(end, wanted[last]->offset + wanted[last]->packed);
        }

        ModHttpGet request;
        const bool direct = !directUrl.empty();
        // an empty file is a span of no bytes, which no Range header can spell
        if (!request.Send(session, direct ? directUrl : canonicalUrl, begin, std::max(end, begin + 1), error))
        {
            directUrl.clear();
            ++transfer.failures;
            continue;
        }

        u64 position = 0;
        const bool partial = request.Status() == HTTP_STATUS_PARTIAL_CONTENT && request.RangeStart(position) && position <= begin;
        // 416 is what a range starting at the very end of a package gets: only empty files
        const bool nothingToRead = request.Status() == 416 && end == begin;
        if (!partial && !nothingToRead && request.Status() != HTTP_STATUS_OK)
        {
            error = "HTTP " + xr_string(std::to_string(request.Status()).c_str());
            if (direct)
                directUrl.clear();
            else if (request.Status() == HTTP_STATUS_NOT_FOUND)
                return false;
            else
                ++transfer.failures;
            continue;
        }
        if (nothingToRead)
            position = begin;
        else if (!partial)
            position = 0; // the whole package from its first byte: what is not wanted gets skipped
        if (!direct)
            directUrl = request.FinalUrl();

        switch (mod_read_span(request, position, wanted, last, payload, transfer, error))
        {
        case ModRead::Done: break;
        case ModRead::Network: ++transfer.failures; break;
        case ModRead::Rejected: return false;
        }
    }
    return true;
}

bool mod_stage(ModHttpSession& session, size_t moduleIndex, const XMS::Module& module,
    const ModRelease::Descriptor& descriptor, const ModRelease::FileIndex& index, xr_string& error, bool catalog,
    bool install, bool& refused)
{
    ModService& service = mod_service();
    refused = false;
    if (catalog)
    {
        error = mod_catalog_index_refusal(descriptor, index);
        if (!error.empty())
        {
            refused = true;
            return false;
        }
    }
    const std::filesystem::path installed(module.root.c_str());
    const std::filesystem::path staging = std::filesystem::path(XMS::StagedRoot().c_str()) / module.id.c_str();
    const std::filesystem::path payload = staging / L"payload";

    XMS::DiscardStaged(module.id.c_str());
    std::error_code fileError;
    std::filesystem::create_directories(payload, fileError);
    if (fileError)
    {
        error = "could not create the staging folder";
        return false;
    }

    std::unordered_map<std::string, std::filesystem::path> byHash;
    if (!mod_hash_installed(installed, index, byHash))
    {
        error = "cancelled";
        return false;
    }

    // Reuse what is already here, under whatever path the new version wants it. Of the rest,
    // one file per distinct content is fetched; its other paths are filled from that copy.
    xr_vector<xr_vector<const ModRelease::FileEntry*>> wanted(descriptor.packages.size());
    xr_vector<const ModRelease::FileEntry*> repeats;
    std::unordered_map<std::string, const ModRelease::FileEntry*> fetching;
    bool linksWork = true;
    u64 reusedFiles = 0, downloadBytes = 0, writeBytes = 0;
    for (const ModRelease::FileEntry& entry : index.files)
    {
        if (const auto local = byHash.find(entry.sha256.c_str()); local != byHash.end())
        {
            if (!mod_reuse_file(local->second, payload / mod_utf8_path(entry.path), linksWork))
            {
                error = "could not carry an installed file over";
                return false;
            }
            ++reusedFiles;
        }
        else if (!fetching.try_emplace(entry.sha256.c_str(), &entry).second)
            repeats.push_back(&entry);
        else
        {
            wanted[entry.package].push_back(&entry);
            downloadBytes += entry.packed;
            writeBytes += entry.size;
        }
    }
    for (const ModRelease::FileEntry* entry : repeats)
        writeBytes += entry->size;

    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(staging.c_str(), &available, nullptr, nullptr) && available.QuadPart < writeBytes)
    {
        error = "not enough free disk space";
        return false;
    }

    Msg("* [mods] %s: %llu of %zu file(s) already here, downloading %llu byte(s)", module.id.c_str(),
        static_cast<unsigned long long>(reusedFiles), index.files.size(), static_cast<unsigned long long>(downloadBytes));
    service.progressDone.store(0, std::memory_order_release);
    service.progressTotal.store(downloadBytes, std::memory_order_release);
    mod_set_state(moduleIndex, State::Downloading);

    for (size_t package = 0; package != wanted.size(); ++package)
    {
        if (wanted[package].empty())
            continue;
        std::ranges::sort(wanted[package], {}, &ModRelease::FileEntry::offset);
        if (!mod_fetch_package(session, mod_asset_url(module.update_github, descriptor.packages[package].name),
                wanted[package], payload, error))
            return false;
    }
    for (const ModRelease::FileEntry* entry : repeats)
    {
        const std::filesystem::path source = payload / mod_utf8_path(fetching[entry->sha256.c_str()]->path);
        if (!mod_reuse_file(source, payload / mod_utf8_path(entry->path), linksWork))
        {
            error = "could not write a module file";
            return false;
        }
    }

    xr_string id, version;
    if (!ModRelease::ReadManifestIdentity(payload / L"mod.ltx", id, version) || id != module.id ||
        ModRelease::ParseVersion(version) != ModRelease::ParseVersion(descriptor.version))
    {
        error = "release does not hold the module and version its descriptor names";
        return false;
    }
    // every byte is the author's now; what they are is the catalog's question (MOD_CATALOG 8)
    if (catalog)
    {
        for (const ModRelease::FileEntry& entry : index.files)
        {
            xr_string why;
            if (!ModRelease::CatalogFileAllowed(payload / mod_utf8_path(entry.path), why))
            {
                error = entry.path + ": " + why;
                refused = true;
                return false;
            }
        }
    }
    return install ? XMS::MarkStagedInstall(module.id.c_str(), descriptor.version.c_str(), error) :
                     XMS::MarkStagedReady(module.id.c_str(), descriptor.version.c_str(), error);
}

void mod_run_update(ModHttpSession& session, size_t index)
{
    const XMS::Module& module = XMS::Modules()[index];
    ModService& service = mod_service();
    service.progressDone.store(0, std::memory_order_release);
    service.progressTotal.store(0, std::memory_order_release);
    mod_set_state(index, State::Preparing);

    // Fetched again rather than kept from the check: the assets are addressed through
    // "latest", and a release published in between would answer with another version's files.
    ModRelease::Descriptor descriptor;
    xr_string error, text;
    const ModFetch fetched = mod_fetch_descriptor(session, module, descriptor, error, &text);
    if (fetched != ModFetch::Ok)
    {
        Msg("! [mods] %s: update failed - %s", module.id.c_str(), error.c_str());
        mod_set_state(index, fetched == ModFetch::NotFound ? State::NoRelease : State::Failed, std::move(error));
        return;
    }
    const bool catalog = mod_is_catalog(module);
    // the check has usually compared already, and then this is a lookup
    u64 reissueBytes = 0;
    const bool reissue = ModRelease::ParseVersion(descriptor.version) == ModRelease::ParseVersion(module.version) &&
        mod_reissued(session, index, module, descriptor, reissueBytes);
    if (mod_stopped() || !mod_evaluate(index, module.version, descriptor, descriptor.PackageBytes(), reissue))
        return;
    if (catalog)
    {
        xr_string refusal = mod_catalog_refusal(module, text, descriptor);
        if (!refusal.empty())
        {
            Msg("! [mods] %s: update %s refused - %s", module.id.c_str(), descriptor.version.c_str(), refusal.c_str());
            mod_set_state(index, State::Refused, std::move(refusal));
            return;
        }
    }

    mod_set_state(index, State::Preparing);
    bool refused = false;
    const auto files = mod_fetch_index(session, index, module, descriptor, error);
    if (files && mod_stage(session, index, module, descriptor, *files, error, catalog, false, refused))
    {
        Msg("* [mods] %s: %s staged, applied by the next start", module.id.c_str(), descriptor.version.c_str());
        mod_forget_verdict(service, module.id);
        mod_set_state(index, State::Staged);
        std::lock_guard lock(service.mutex);
        ++service.stagedThisRun;
        return;
    }

    XMS::DiscardStaged(module.id.c_str());
    Msg("! [mods] %s: update %s - %s", module.id.c_str(), refused ? "refused" : "failed", error.c_str());
    mod_set_state(index, refused ? State::Refused : State::Failed, std::move(error));
}

// A module the catalog lists, installed from nothing: its live release, the catalog's rules,
// the same staging as an update with no installed files to reuse.
void mod_run_install(ModHttpSession& session, const xr_string& id)
{
    ModService& service = mod_service();
    service.progressDone.store(0, std::memory_order_release);
    service.progressTotal.store(0, std::memory_order_release);
    mod_set_state(ModInstallSlot, State::Preparing);

    const std::shared_ptr<const XMS::Catalog::Index> catalog = XMS::Catalog::Current();
    const XMS::Catalog::Listing* listing = catalog ? catalog->Find(id) : nullptr;
    xr_string error;
    if (!listing || XMS::FindModule(id.c_str()))
    {
        error = listing ? "the module is installed already" : "the mod catalog does not list this module";
        Msg("! [mods] %s: installation failed - %s", id.c_str(), error.c_str());
        mod_set_state(ModInstallSlot, State::Failed, std::move(error));
        return;
    }
    XMS::Module module;
    module.id = id;
    module.update_github = listing->github;

    ModRelease::Descriptor descriptor;
    xr_string text;
    const ModFetch fetched = mod_fetch_descriptor(session, module, descriptor, error, &text);
    if (fetched != ModFetch::Ok)
    {
        if (fetched == ModFetch::NotFound)
            error = "the module's repository publishes no release";
        Msg("! [mods] %s: installation failed - %s", id.c_str(), error.c_str());
        mod_set_state(ModInstallSlot, State::Failed, std::move(error));
        return;
    }
    const bool unreadable = descriptor.schema > ModRelease::SupportedSchema;
    if (unreadable || (!descriptor.requiresGame.empty() &&
            ModRelease::ParseVersion(descriptor.requiresGame) > ModRelease::ParseVersion(DeadAirRefined::Version)))
    {
        mod_set_status(ModInstallSlot, [&](ModuleStatus& status)
        {
            status.state = State::Blocked;
            status.version = descriptor.version;
            status.requiresGame = unreadable ? xr_string() : descriptor.requiresGame;
        });
        return;
    }
    xr_string refusal = mod_catalog_refusal(module, text, descriptor);
    if (!refusal.empty())
    {
        Msg("! [mods] %s: installation of %s refused - %s", id.c_str(), descriptor.version.c_str(), refusal.c_str());
        mod_set_state(ModInstallSlot, State::Refused, std::move(refusal));
        return;
    }
    mod_set_status(ModInstallSlot, [&](ModuleStatus& status) { status.version = descriptor.version; });

    bool refused = false;
    const auto files = mod_fetch_index(session, ModInstallSlot, module, descriptor, error);
    if (files && mod_stage(session, ModInstallSlot, module, descriptor, *files, error, true, true, refused))
    {
        Msg("* [mods] %s: %s staged for installation by the next start", id.c_str(), descriptor.version.c_str());
        mod_set_state(ModInstallSlot, State::Staged);
        std::lock_guard lock(service.mutex);
        ++service.stagedThisRun;
        return;
    }
    XMS::DiscardStaged(id.c_str());
    Msg("! [mods] %s: installation %s - %s", id.c_str(), refused ? "refused" : "failed", error.c_str());
    mod_set_state(ModInstallSlot, refused ? State::Refused : State::Failed, std::move(error));
}

void mod_run_check(ModHttpSession& session)
{
    ModService& service = mod_service();
    XMS::DiscardUnarmedStaged();

    const xr_vector<XMS::Module>& modules = XMS::Modules();
    for (size_t index = 0; index != modules.size() && !mod_stopped(); ++index)
    {
        {
            std::lock_guard lock(service.mutex);
            ModuleStatus& status = service.modules[index];
            if (status.state != State::Unchecked && status.state != State::CheckFailed)
                continue;
            status.state = State::Checking;
        }

        const XMS::Module& module = modules[index];
        ModRelease::Descriptor descriptor;
        xr_string error, text;
        const ModFetch fetched = mod_fetch_descriptor(session, module, descriptor, error, &text);
        switch (fetched)
        {
        case ModFetch::Ok:
        {
            Msg("* [mods] %s: installed %s, released %s", module.id.c_str(), module.version.c_str(),
                descriptor.version.c_str());
            // The same number is not yet "nothing new": the author may have taken that release
            // down and published it again. Worth a comparison only for a release this game can take.
            if (ModRelease::ParseVersion(descriptor.version) == ModRelease::ParseVersion(module.version) &&
                !mod_release_blocked(descriptor))
            {
                u64 download = 0;
                const bool reissue = mod_reissued(session, index, module, descriptor, download);
                if (mod_stopped())
                {
                    mod_set_state(index, State::Unchecked);
                    break;
                }
                mod_evaluate(index, module.version, descriptor, download, reissue);
                if (reissue)
                    Msg("* [mods] %s: %s was published again with other content - about %llu of %llu byte(s) to download",
                        module.id.c_str(), descriptor.version.c_str(), static_cast<unsigned long long>(download),
                        static_cast<unsigned long long>(descriptor.PackageBytes()));
                break;
            }
            if (!mod_evaluate(index, module.version, descriptor, descriptor.PackageBytes()))
                break;
            // An update on offer comes with its size. The index is what knows it; without one
            // the offer still stands and quotes the packages whole.
            xr_string indexError;
            if (const auto files = mod_fetch_index(session, index, module, descriptor, indexError))
            {
                const u64 download = mod_estimate_download(module, *files);
                mod_set_status(index, [&](ModuleStatus& status) { status.totalBytes = download; });
                Msg("* [mods] %s: about %llu of %llu byte(s) to download", module.id.c_str(),
                    static_cast<unsigned long long>(download), static_cast<unsigned long long>(descriptor.PackageBytes()));
            }
            break;
        }
        case ModFetch::NotFound:
            Msg("* [mods] %s: %s publishes no release for it", module.id.c_str(), module.update_github.c_str());
            mod_set_state(index, State::NoRelease);
            break;
        case ModFetch::Failed:
            Msg("* [mods] %s: update check failed - %s", module.id.c_str(), error.c_str());
            mod_set_state(index, State::CheckFailed, std::move(error));
            break;
        }

        // an update of a catalog module is on offer only when the catalog takes it
        bool offered = false;
        {
            std::lock_guard lock(service.mutex);
            offered = service.modules[index].state == State::Available;
        }
        if (fetched == ModFetch::Ok && offered && mod_is_catalog(module))
        {
            xr_string refusal = mod_catalog_refusal(module, text, descriptor);
            if (!refusal.empty())
            {
                Msg("! [mods] %s: update %s refused - %s", module.id.c_str(), descriptor.version.c_str(), refusal.c_str());
                mod_set_state(index, State::Refused, std::move(refusal));
            }
        }
    }
}

bool mod_queue(ModService& service, size_t index)
{
    ModuleStatus& status = service.modules[index];
    if (status.state != State::Available && status.state != State::Failed)
        return false;
    status.state = State::Queued;
    status.message.clear();
    service.queue.push_back(index);
    return true;
}

// worker thread: what the console asked for, now that the check has an answer
void mod_queue_requests()
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    // a request that arrived mid-pass reset modules this pass had already left behind; the pass
    // it asked for comes next and answers for all of them
    if (service.checkRequested)
        return;
    for (size_t index = 0; index != service.modules.size(); ++index)
    {
        const bool requested = service.requestedAll ||
            std::ranges::find(service.requestedIds, service.modules[index].id) != service.requestedIds.end();
        if (requested && service.modules[index].state == State::Available)
            mod_queue(service, index);
    }
    service.requestedAll = false;
    service.requestedIds.clear();
}

void mod_worker()
{
    ModService& service = mod_service();
    ModHttpSession session;
    for (;;)
    {
        size_t update = 0;
        bool check = false;
        xr_string install;
        {
            std::lock_guard lock(service.mutex);
            if (mod_stopped())
            {
                service.workerRunning = false;
                return;
            }
            if (!service.queue.empty())
            {
                update = service.queue.front();
                service.queue.pop_front();
            }
            else if (!service.installQueue.empty())
            {
                install = service.installQueue.front();
                service.installQueue.pop_front();
                service.activeInstall = install;
            }
            else if (service.checkRequested)
            {
                service.checkRequested = false;
                check = true;
            }
            else
            {
                if (service.stagedThisRun)
                    service.restartPrompt.store(true, std::memory_order_release);
                service.stagedThisRun = 0;
                service.workerRunning = false;
                return;
            }
        }
        if (check)
        {
            mod_run_check(session);
            mod_queue_requests();
        }
        else if (!install.empty())
            mod_run_install(session, install);
        else
            mod_run_update(session, update);
    }
}

pcstr mod_state_name(State state)
{
    switch (state)
    {
    case State::NoSource: return "no update source";
    case State::Unchecked: return "not checked";
    case State::Checking: return "checking";
    case State::Current: return "up to date";
    case State::NoRelease: return "no release";
    case State::Available: return "update available";
    case State::Blocked: return "update needs a newer game";
    case State::Refused: return "refused by the mod catalog's rules";
    case State::CheckFailed: return "check failed";
    case State::Queued: return "queued";
    case State::Preparing: return "verifying installed files";
    case State::Downloading: return "downloading";
    case State::Staged: return "staged for the next start";
    case State::Failed: return "update failed";
    }
    return "";
}

// main thread, with the mutex held
void mod_wake_worker(ModService& service)
{
    if (service.workerRunning)
        return;
    // a worker that cleared the flag has nothing left to do but return
    if (service.worker.joinable())
        service.worker.join();
    service.workerRunning = true;
    service.worker = std::thread(mod_worker);
}

void mod_initialize(ModService& service)
{
    if (service.initialized)
        return;
    service.initialized = true;
    for (const XMS::Module& module : XMS::Modules())
    {
        ModuleStatus status;
        status.id = module.id;
        // a folder with a broken manifest is listed under its folder name, which is not an id
        // anything can be asked about
        if (!ModRelease::ValidModuleId(module.id))
            status.state = State::NoSource;
        else if (XMS::StagedVersion(module.id.c_str(), status.version))
            status.state = State::Staged;
        else if (!module.update_github.empty())
            status.state = State::Unchecked;
        service.modules.emplace_back(std::move(status));
    }
    service.indexes.resize(service.modules.size());
}
}

void ModUpdateService::StartCheck()
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    mod_initialize(service);
    const bool pending = std::ranges::any_of(service.modules, [](const ModuleStatus& status)
    {
        return status.state == State::Unchecked || status.state == State::CheckFailed;
    });
    if (!pending)
        return;
    service.checkRequested = true;
    mod_wake_worker(service);
}

bool ModUpdateService::StartUpdate(pcstr moduleId)
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    mod_initialize(service);
    for (size_t index = 0; index != service.modules.size(); ++index)
    {
        if (service.modules[index].id != moduleId)
            continue;
        if (!mod_queue(service, index))
            return false;
        mod_wake_worker(service);
        return true;
    }
    return false;
}

u32 ModUpdateService::StartUpdateAll()
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    mod_initialize(service);
    u32 queued = 0;
    for (size_t index = 0; index != service.modules.size(); ++index)
    {
        // a failed one is retried by its own button, not swept up with the rest
        if (service.modules[index].state == State::Available && mod_queue(service, index))
            ++queued;
    }
    if (queued)
        mod_wake_worker(service);
    return queued;
}

void ModUpdateService::RequestUpdate(pcstr moduleId)
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    mod_initialize(service);
    mod_log_disclaimer();
    if (moduleId && moduleId[0])
        service.requestedIds.emplace_back(moduleId);
    else
        service.requestedAll = true;
    // the request is a fresh start for a module that already has an answer
    for (ModuleStatus& status : service.modules)
    {
        const bool requested = service.requestedAll || status.id == moduleId;
        if (requested && (status.state == State::Failed || status.state == State::Current ||
                status.state == State::NoRelease || status.state == State::Blocked))
            status.state = State::Unchecked;
    }
    service.checkRequested = true;
    mod_wake_worker(service);
}

void ModUpdateService::LogStatus()
{
    const Snapshot snapshot = GetSnapshot();
    const xr_vector<XMS::Module>& modules = XMS::Modules();
    Msg("* [mods] %zu module(s)%s", snapshot.modules.size(), snapshot.busy ? ", working" : "");
    for (size_t index = 0; index != snapshot.modules.size() && index != modules.size(); ++index)
    {
        const ModuleStatus& status = snapshot.modules[index];
        Msg("*   %s %s: %s%s%s%s%s%s", status.id.c_str(), modules[index].version.c_str(), mod_state_name(status.state),
            status.version.empty() ? "" : " - ", status.version.c_str(),
            status.reissue ? " (the installed version, published again)" : "", status.message.empty() ? "" : " - ",
            status.message.c_str());
    }
}

bool ModUpdateService::RestartPromptPending()
{
    return mod_service().restartPrompt.load(std::memory_order_acquire);
}

void ModUpdateService::AcknowledgeRestartPrompt()
{
    mod_service().restartPrompt.store(false, std::memory_order_release);
}

bool ModUpdateService::RestartGame()
{
    // read by the new process before it applies staged updates, see XMS::InitializeAndMount
    SetEnvironmentVariableW(L"DAR_RELAUNCH_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    if (!ContentService::Relaunch())
    {
        SetEnvironmentVariableW(L"DAR_RELAUNCH_WAIT_PID", nullptr);
        return false;
    }
    Console->Execute("quit");
    return true;
}

bool ModUpdateService::OpenWebsite(pcstr moduleId)
{
    const XMS::Module* module = XMS::FindModule(moduleId);
    if (!module || !XMS::ValidWebsite(module->website.c_str()))
        return false;
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", mod_widen(module->website).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
}

bool ModUpdateService::StartInstall(pcstr moduleId, xr_string& error)
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    mod_initialize(service);
    const std::shared_ptr<const XMS::Catalog::Index> catalog = XMS::Catalog::Current();
    if (!moduleId || !catalog || !catalog->Find(moduleId))
    {
        error = "the mod catalog does not list this module";
        return false;
    }
    if (XMS::FindModule(moduleId))
    {
        error = "the module is installed already";
        return false;
    }
    // VirusTotal's block is final for an installation (MOD_CATALOG 11)
    if (const auto snapshot = ModCatalog::Current(); const ModCatalog::Entry* entry = snapshot->Find(moduleId))
    {
        if (entry->vt.verdict == ModCatalog::Verdict::Block)
        {
            error = "VirusTotal: flagged by " + (entry->vt.flaggedBy.empty() ? xr_string("antivirus engines") : entry->vt.flaggedBy);
            return false;
        }
        if (!entry->revoked.empty())
        {
            error = "withdrawn from the mod catalog: " + entry->revoked;
            return false;
        }
    }
    const auto existing = std::ranges::find(service.installs, xr_string(moduleId), &ModuleStatus::id);
    if (existing != service.installs.end())
    {
        const State state = existing->state;
        if (state != State::Failed && state != State::Refused && state != State::Blocked)
        {
            error = "the module is being installed already";
            return false;
        }
        service.installs.erase(existing);
    }
    ModuleStatus& status = service.installs.emplace_back();
    status.id = moduleId;
    status.state = State::Queued;
    service.installQueue.emplace_back(moduleId);
    mod_log_disclaimer();
    mod_wake_worker(service);
    return true;
}

bool ModUpdateService::StageRemoval(pcstr moduleId, xr_string& error)
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    if (service.workerRunning && !service.queue.empty() && std::ranges::any_of(service.queue, [&](size_t index)
        { return index < service.modules.size() && service.modules[index].id == moduleId; }))
    {
        error = "the module is being updated";
        return false;
    }
    if (!XMS::StageRemoval(moduleId, error))
        return false;
    Msg("* [mods] %s: removed by the next start", moduleId);
    return true;
}

void ModUpdateService::CancelRemoval(pcstr moduleId)
{
    if (XMS::StagedAction(moduleId) == "remove")
        XMS::DiscardStaged(moduleId);
}

void ModUpdateService::LogDisclaimer() { mod_log_disclaimer(); }

ModUpdateService::Snapshot ModUpdateService::GetSnapshot()
{
    ModService& service = mod_service();
    Snapshot snapshot;
    std::lock_guard lock(service.mutex);
    mod_initialize(service);
    snapshot.modules = service.modules;
    snapshot.installs = service.installs;
    snapshot.busy = service.workerRunning;
    snapshot.restartPrompt = service.restartPrompt.load(std::memory_order_acquire);
    for (xr_vector<ModuleStatus>* list : {&snapshot.modules, &snapshot.installs})
    {
        for (ModuleStatus& status : *list)
        {
            if (status.state != State::Preparing && status.state != State::Downloading)
                continue;
            if (list == &snapshot.installs && status.id != service.activeInstall)
                continue;
            status.downloadedBytes = service.progressDone.load(std::memory_order_acquire);
            status.totalBytes = service.progressTotal.load(std::memory_order_acquire);
        }
    }
    return snapshot;
}

void ModUpdateService::Shutdown()
{
    ModService& service = mod_service();
    service.stop.store(true, std::memory_order_release);
    if (service.worker.joinable())
        service.worker.join();
}
#else
void ModUpdateService::StartCheck() {}
bool ModUpdateService::StartUpdate(pcstr) { return false; }
u32 ModUpdateService::StartUpdateAll() { return 0; }
void ModUpdateService::RequestUpdate(pcstr) {}
void ModUpdateService::LogStatus() {}
bool ModUpdateService::RestartPromptPending() { return false; }
void ModUpdateService::AcknowledgeRestartPrompt() {}
bool ModUpdateService::RestartGame() { return false; }
bool ModUpdateService::OpenWebsite(pcstr) { return false; }
bool ModUpdateService::StartInstall(pcstr, xr_string& error) { error = "not supported on this platform"; return false; }
bool ModUpdateService::StageRemoval(pcstr, xr_string& error) { error = "not supported on this platform"; return false; }
void ModUpdateService::CancelRemoval(pcstr) {}
void ModUpdateService::LogDisclaimer() {}
ModUpdateService::Snapshot ModUpdateService::GetSnapshot() { return {}; }
void ModUpdateService::Shutdown() {}
#endif
