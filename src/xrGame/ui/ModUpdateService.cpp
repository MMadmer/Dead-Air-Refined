#include "StdAfx.h"
#include "ModUpdateService.h"

#include "ContentService.h"
#include "ModRelease.h"

#include "xrContentSync/ContentHash.h"
#include "xrCore/ProductVersion.h"
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
    // console requests waiting for the check to say what is available
    xr_vector<xr_string> requestedIds;
    bool requestedAll{};
    bool checkRequested{};
    bool workerRunning{};
    u32 stagedThisRun{};

    // worker thread only: one worker runs at a time, so these need no lock
    xr_vector<ModKnownIndex> indexes;

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

template <typename Change>
void mod_set_status(size_t index, const Change& change)
{
    ModService& service = mod_service();
    std::lock_guard lock(service.mutex);
    change(service.modules[index]);
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

// One session for a worker's whole run: WinHTTP pools connections per session, and an update
// made of ranged requests is mostly connection setup without that.
class ModHttpSession
{
public:
    ~ModHttpSession()
    {
        if (m_session)
            WinHttpCloseHandle(m_session);
    }

    HINTERNET Handle()
    {
        if (!m_session)
        {
            const std::wstring agent = L"Dead Air Refined/" + std::wstring(DeadAirRefined::VersionWide);
            m_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS, 0);
            if (m_session)
                WinHttpSetTimeouts(m_session, 5000, 10000, 15000, 60000);
        }
        return m_session;
    }

private:
    HINTERNET m_session{};
};

class ModHttpGet
{
public:
    ~ModHttpGet()
    {
        if (m_request)
            WinHttpCloseHandle(m_request);
        if (m_connection)
            WinHttpCloseHandle(m_connection);
    }

    // [rangeBegin, rangeEnd) of the resource, or all of it when rangeEnd is zero
    bool Send(ModHttpSession& session, const std::wstring& url, u64 rangeBegin, u64 rangeEnd, xr_string& error)
    {
        const HINTERNET handle = session.Handle();
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!handle || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        {
            error = handle ? "invalid URL" : "WinHTTP initialization failed";
            return false;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        m_connection = WinHttpConnect(handle, host.c_str(), parts.nPort, 0);
        m_request = m_connection ?
            WinHttpOpenRequest(m_connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) :
            nullptr;
        if (!m_request)
        {
            error = "could not connect";
            return false;
        }
        if (rangeEnd)
        {
            const std::wstring range =
                L"Range: bytes=" + std::to_wstring(rangeBegin) + L"-" + std::to_wstring(rangeEnd - 1) + L"\r\n";
            WinHttpAddRequestHeaders(m_request, range.c_str(), static_cast<DWORD>(range.size()), WINHTTP_ADDREQ_FLAG_ADD);
        }

        DWORD statusSize = sizeof(m_status);
        if (!WinHttpSendRequest(m_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(m_request, nullptr) ||
            !WinHttpQueryHeaders(m_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &m_status, &statusSize, WINHTTP_NO_HEADER_INDEX))
        {
            error = "request failed";
            return false;
        }
        return true;
    }

    DWORD Status() const { return m_status; }

    bool Read(void* buffer, DWORD capacity, DWORD& received)
    {
        return WinHttpReadData(m_request, buffer, capacity, &received) != FALSE;
    }

    // first byte the server is sending, from Content-Range; false when the header is absent
    bool RangeStart(u64& start) const
    {
        std::array<wchar_t, 128> value{};
        DWORD size = sizeof(value);
        if (!WinHttpQueryHeaders(m_request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, value.data(),
                &size, WINHTTP_NO_HEADER_INDEX))
            return false;
        const std::wstring_view header(value.data());
        const size_t space = header.find(L' ');
        if (space == std::wstring_view::npos)
            return false;
        start = wcstoull(header.data() + space + 1, nullptr, 10);
        return true;
    }

    // where the redirects ended: the storage URL behind releases/latest/download
    std::wstring FinalUrl() const
    {
        std::array<wchar_t, 4096> value{};
        DWORD size = sizeof(value);
        return WinHttpQueryOption(m_request, WINHTTP_OPTION_URL, value.data(), &size) ? std::wstring(value.data()) :
                                                                                         std::wstring();
    }

private:
    HINTERNET m_connection{};
    HINTERNET m_request{};
    DWORD m_status{};
};

enum class ModFetch
{
    Ok,
    NotFound,
    Failed
};

// a small text asset, whole, into memory
ModFetch mod_fetch_text(ModHttpSession& session, const std::wstring& url, size_t limit, xr_string& body, xr_string& error)
{
    ModHttpGet request;
    if (!request.Send(session, url, 0, 0, error))
        return ModFetch::Failed;
    if (request.Status() == HTTP_STATUS_NOT_FOUND)
        return ModFetch::NotFound;
    if (request.Status() != HTTP_STATUS_OK)
    {
        error = "HTTP " + xr_string(std::to_string(request.Status()).c_str());
        return ModFetch::Failed;
    }

    std::vector<char> buffer(64 * 1024);
    while (!mod_stopped())
    {
        DWORD received = 0;
        if (!request.Read(buffer.data(), static_cast<DWORD>(buffer.size()), received))
        {
            error = "download was interrupted";
            return ModFetch::Failed;
        }
        if (!received)
            return ModFetch::Ok;
        body.append(buffer.data(), received);
        if (body.size() > limit)
        {
            error = "asset is larger than the contract allows";
            return ModFetch::Failed;
        }
    }
    error = "cancelled";
    return ModFetch::Failed;
}

ModFetch mod_fetch_descriptor(ModHttpSession& session, const XMS::Module& module, ModRelease::Descriptor& descriptor,
    xr_string& error)
{
    xr_string body;
    const ModFetch fetched = mod_fetch_text(session, mod_asset_url(module.update_github, module.id + ".update.ltx"),
        ModRelease::MaximumDescriptorBytes, body, error);
    if (fetched != ModFetch::Ok)
        return fetched;
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
    ModKnownIndex& known = mod_service().indexes[moduleIndex];
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

// Turns a descriptor into the state the Mods menu shows. True when the release can be taken.
bool mod_evaluate(size_t index, const xr_string& installedVersion, const ModRelease::Descriptor& descriptor,
    u64 downloadBytes)
{
    const bool newer = ModRelease::ParseVersion(descriptor.version) > ModRelease::ParseVersion(installedVersion);
    const bool unreadable = descriptor.schema > ModRelease::SupportedSchema;
    const bool gameTooOld = !unreadable && !descriptor.requiresGame.empty() &&
        ModRelease::ParseVersion(descriptor.requiresGame) > ModRelease::ParseVersion(DeadAirRefined::Version);

    mod_set_status(index, [&](ModuleStatus& status)
    {
        status.message.clear();
        status.version = newer ? descriptor.version : xr_string();
        status.requiresGame = newer && gameTooOld ? descriptor.requiresGame : xr_string();
        status.totalBytes = newer && !unreadable ? downloadBytes : 0;
        status.downloadedBytes = 0;
        status.state = !newer ? State::Current : (unreadable || gameTooOld) ? State::Blocked : State::Available;
    });
    return newer && !unreadable && !gameTooOld;
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
    const ModRelease::Descriptor& descriptor, const ModRelease::FileIndex& index, xr_string& error)
{
    ModService& service = mod_service();
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
    return XMS::MarkStagedReady(module.id.c_str(), descriptor.version.c_str(), error);
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
    xr_string error;
    const ModFetch fetched = mod_fetch_descriptor(session, module, descriptor, error);
    if (fetched != ModFetch::Ok)
    {
        Msg("! [mods] %s: update failed - %s", module.id.c_str(), error.c_str());
        mod_set_state(index, fetched == ModFetch::NotFound ? State::NoRelease : State::Failed, std::move(error));
        return;
    }
    if (!mod_evaluate(index, module.version, descriptor, descriptor.PackageBytes()))
        return;

    mod_set_state(index, State::Preparing);
    const auto files = mod_fetch_index(session, index, module, descriptor, error);
    if (files && mod_stage(session, index, module, descriptor, *files, error))
    {
        Msg("* [mods] %s: %s staged, applied by the next start", module.id.c_str(), descriptor.version.c_str());
        mod_set_state(index, State::Staged);
        std::lock_guard lock(service.mutex);
        ++service.stagedThisRun;
        return;
    }

    XMS::DiscardStaged(module.id.c_str());
    Msg("! [mods] %s: update failed - %s", module.id.c_str(), error.c_str());
    mod_set_state(index, State::Failed, std::move(error));
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
        xr_string error;
        switch (mod_fetch_descriptor(session, module, descriptor, error))
        {
        case ModFetch::Ok:
        {
            Msg("* [mods] %s: installed %s, released %s", module.id.c_str(), module.version.c_str(),
                descriptor.version.c_str());
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
        Msg("*   %s %s: %s%s%s%s%s", status.id.c_str(), modules[index].version.c_str(), mod_state_name(status.state),
            status.version.empty() ? "" : " - ", status.version.c_str(), status.message.empty() ? "" : " - ",
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

ModUpdateService::Snapshot ModUpdateService::GetSnapshot()
{
    ModService& service = mod_service();
    Snapshot snapshot;
    std::lock_guard lock(service.mutex);
    mod_initialize(service);
    snapshot.modules = service.modules;
    snapshot.busy = service.workerRunning;
    snapshot.restartPrompt = service.restartPrompt.load(std::memory_order_acquire);
    for (ModuleStatus& status : snapshot.modules)
    {
        if (status.state != State::Preparing && status.state != State::Downloading)
            continue;
        status.downloadedBytes = service.progressDone.load(std::memory_order_acquire);
        status.totalBytes = service.progressTotal.load(std::memory_order_acquire);
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
ModUpdateService::Snapshot ModUpdateService::GetSnapshot() { return {}; }
void ModUpdateService::Shutdown() {}
#endif
