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
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
using ModUpdateService::ModuleStatus;
using ModUpdateService::State;

constexpr std::wstring_view ModReleaseHost = L"https://github.com";
constexpr unsigned ModDownloadAttempts = 5;

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

    bool initialized{};
    std::atomic_bool stop{};
    std::atomic_bool restartPrompt{};
    std::atomic<u64> downloaded{};
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

std::wstring mod_widen(std::string_view text)
{
    // every string that reaches a URL here has already been held to an ASCII grammar
    return std::wstring(text.begin(), text.end());
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

class ModHttpGet
{
public:
    ~ModHttpGet()
    {
        if (m_request)
            WinHttpCloseHandle(m_request);
        if (m_connection)
            WinHttpCloseHandle(m_connection);
        if (m_session)
            WinHttpCloseHandle(m_session);
    }

    bool Send(const std::wstring& url, const std::wstring& headers, xr_string& error)
    {
        const std::wstring agent = L"Dead Air Refined/" + std::wstring(DeadAirRefined::VersionWide);
        m_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session)
        {
            error = "WinHTTP initialization failed";
            return false;
        }
        WinHttpSetTimeouts(m_session, 5000, 10000, 15000, 60000);

        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        {
            error = "invalid URL";
            return false;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        m_connection = WinHttpConnect(m_session, host.c_str(), parts.nPort, 0);
        m_request = m_connection ?
            WinHttpOpenRequest(m_connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) :
            nullptr;
        if (!m_request)
        {
            error = "could not connect";
            return false;
        }
        if (!headers.empty())
            WinHttpAddRequestHeaders(m_request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD);

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

private:
    HINTERNET m_session{};
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

ModFetch mod_fetch_descriptor(const xr_string& repository, const xr_string& moduleId,
    ModRelease::Descriptor& descriptor, xr_string& error)
{
    ModHttpGet request;
    if (!request.Send(mod_asset_url(repository, moduleId + ".update.ltx"), {}, error))
        return ModFetch::Failed;
    if (request.Status() == HTTP_STATUS_NOT_FOUND)
        return ModFetch::NotFound;
    if (request.Status() != HTTP_STATUS_OK)
    {
        error = "HTTP " + xr_string(std::to_string(request.Status()).c_str());
        return ModFetch::Failed;
    }

    xr_string body;
    std::array<char, 8192> buffer{};
    for (;;)
    {
        DWORD received = 0;
        if (!request.Read(buffer.data(), static_cast<DWORD>(buffer.size()), received))
        {
            error = "descriptor download was interrupted";
            return ModFetch::Failed;
        }
        if (!received)
            break;
        body.append(buffer.data(), received);
        if (body.size() > ModRelease::MaximumDescriptorBytes)
        {
            error = "descriptor is too large";
            return ModFetch::Failed;
        }
    }

    if (!ModRelease::ParseDescriptor(body, descriptor, error))
        return ModFetch::Failed;
    if (descriptor.id != moduleId)
    {
        error = "descriptor belongs to another module";
        return ModFetch::Failed;
    }
    return ModFetch::Ok;
}

// Turns a descriptor into the state the Mods menu shows. True when the release can be taken.
bool mod_evaluate(size_t index, const xr_string& installedVersion, const ModRelease::Descriptor& descriptor)
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
        status.totalBytes = newer && !unreadable ? descriptor.DownloadBytes() : 0;
        status.downloadedBytes = 0;
        status.state = !newer ? State::Current : (unreadable || gameTooOld) ? State::Blocked : State::Available;
    });
    return newer && !unreadable && !gameTooOld;
}

bool mod_sleep(unsigned milliseconds)
{
    const std::atomic_bool& stop = mod_service().stop;
    for (unsigned elapsed = 0; elapsed < milliseconds && !stop.load(std::memory_order_acquire); elapsed += 100)
        Sleep(100);
    return !stop.load(std::memory_order_acquire);
}

// A dropped connection resumes from where it stopped: the running digest already covers every
// byte on disk, so a 206 that starts at that offset simply continues both. A server that
// answers a ranged request with 200 is sending the file from its first byte, so both restart.
bool mod_download(const std::wstring& url, const ModRelease::Package& package, const std::filesystem::path& target,
    u64 alreadyDownloaded, xr_string& error)
{
    ModService& service = mod_service();
    const HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "could not create the package file";
        return false;
    }

    ContentHash::Stream digest;
    bool success = digest.Open();
    u64 offset = 0;
    bool complete = false;
    std::vector<u8> buffer(256 * 1024);
    for (unsigned attempt = 0; success && !complete && attempt != ModDownloadAttempts; ++attempt)
    {
        if (attempt && !mod_sleep(1000u << attempt))
            break;

        ModHttpGet request;
        const std::wstring range = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n" : std::wstring();
        if (!request.Send(url, range, error))
            continue;

        u64 start = 0;
        const bool resumed = request.Status() == HTTP_STATUS_PARTIAL_CONTENT && request.RangeStart(start) && start == offset;
        if (!resumed && request.Status() != HTTP_STATUS_OK)
        {
            error = "HTTP " + xr_string(std::to_string(request.Status()).c_str());
            // nothing a retry would change
            if (request.Status() == HTTP_STATUS_NOT_FOUND)
                break;
            continue;
        }
        if (!resumed && offset)
        {
            offset = 0;
            digest = ContentHash::Stream();
            success = digest.Open() && SetFilePointerEx(file, {}, nullptr, FILE_BEGIN) && SetEndOfFile(file);
        }

        while (success && !service.stop.load(std::memory_order_acquire))
        {
            DWORD received = 0;
            if (!request.Read(buffer.data(), static_cast<DWORD>(buffer.size()), received))
            {
                error = "download was interrupted";
                break;
            }
            if (!received)
            {
                // a stream that ends early is an interruption like any other
                complete = offset >= package.size;
                error = "download was interrupted";
                break;
            }
            DWORD written = 0;
            offset += received;
            success = offset <= package.size && WriteFile(file, buffer.data(), received, &written, nullptr) &&
                written == received && digest.Append(buffer.data(), received);
            if (!success)
                error = offset > package.size ? "package is larger than declared" : "could not write the package file";
            service.downloaded.store(alreadyDownloaded + offset, std::memory_order_release);
        }
        if (service.stop.load(std::memory_order_acquire))
            break;
    }
    CloseHandle(file);

    if (service.stop.load(std::memory_order_acquire))
    {
        error = "cancelled";
        return false;
    }
    if (!success || !complete)
    {
        if (error.empty())
            error = "download failed";
        return false;
    }
    if (offset != package.size || digest.Finish() != package.sha256.c_str())
    {
        error = offset != package.size ? "package size does not match the descriptor" :
                                         "package checksum does not match the descriptor";
        return false;
    }
    return true;
}

bool mod_stage(size_t index, const xr_string& moduleId, const xr_string& repository,
    const ModRelease::Descriptor& descriptor, xr_string& error)
{
    ModService& service = mod_service();
    const std::filesystem::path root = std::filesystem::path(XMS::StagedRoot().c_str()) / moduleId.c_str();
    const std::filesystem::path download = root / L"download";
    const std::filesystem::path payload = root / L"payload";

    XMS::DiscardStaged(moduleId.c_str());
    std::error_code fileError;
    std::filesystem::create_directories(download, fileError);
    if (fileError)
    {
        error = "could not create the staging folder";
        return false;
    }

    // the archives and what they unpack to exist side by side until the unpack is done
    const u64 required = descriptor.DownloadBytes() + std::max(descriptor.unpacked, descriptor.DownloadBytes());
    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) && available.QuadPart < required)
    {
        error = "not enough free disk space";
        return false;
    }

    u64 done = 0;
    for (const ModRelease::Package& package : descriptor.packages)
    {
        if (!mod_download(mod_asset_url(repository, package.name), package, download / package.name.c_str(), done, error))
            return false;
        done += package.size;
    }

    mod_set_state(index, State::Unpacking);
    ModRelease::UnpackTotals totals;
    for (const ModRelease::Package& package : descriptor.packages)
    {
        if (!ModRelease::Unpack(download / package.name.c_str(), moduleId, payload, descriptor.files,
                descriptor.unpacked, totals, service.stop, error))
            return false;
    }

    xr_string id, version;
    if (!ModRelease::ReadManifestIdentity(payload / L"mod.ltx", id, version) || id != moduleId ||
        ModRelease::ParseVersion(version) != ModRelease::ParseVersion(descriptor.version))
    {
        error = "package does not hold the module and version its descriptor names";
        return false;
    }

    std::filesystem::remove_all(download, fileError);
    return XMS::MarkStagedReady(moduleId.c_str(), descriptor.version.c_str(), error);
}

void mod_run_update(size_t index)
{
    const XMS::Module& module = XMS::Modules()[index];
    ModService& service = mod_service();
    service.downloaded.store(0, std::memory_order_release);

    // Fetched again rather than kept from the check: the assets are addressed through
    // "latest", and a release published in between would answer with another version's files.
    ModRelease::Descriptor descriptor;
    xr_string error;
    const ModFetch fetched = mod_fetch_descriptor(module.update_github, module.id, descriptor, error);
    if (fetched != ModFetch::Ok)
    {
        Msg("! [mods] %s: update failed - %s", module.id.c_str(), error.c_str());
        mod_set_state(index, fetched == ModFetch::NotFound ? State::NoRelease : State::Failed, std::move(error));
        return;
    }
    if (!mod_evaluate(index, module.version, descriptor))
        return;

    mod_set_state(index, State::Downloading);
    Msg("* [mods] %s: downloading %s (%llu bytes)", module.id.c_str(), descriptor.version.c_str(),
        static_cast<unsigned long long>(descriptor.DownloadBytes()));
    if (mod_stage(index, module.id, module.update_github, descriptor, error))
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

void mod_run_check()
{
    ModService& service = mod_service();
    XMS::DiscardUnarmedStaged();

    const xr_vector<XMS::Module>& modules = XMS::Modules();
    for (size_t index = 0; index != modules.size() && !service.stop.load(std::memory_order_acquire); ++index)
    {
        {
            std::lock_guard lock(service.mutex);
            ModuleStatus& status = service.modules[index];
            if (status.state != State::Unchecked && status.state != State::CheckFailed)
                continue;
            status.state = State::Checking;
        }

        ModRelease::Descriptor descriptor;
        xr_string error;
        switch (mod_fetch_descriptor(modules[index].update_github, modules[index].id, descriptor, error))
        {
        case ModFetch::Ok:
            mod_evaluate(index, modules[index].version, descriptor);
            if (!descriptor.version.empty())
                Msg("* [mods] %s: installed %s, released %s", modules[index].id.c_str(),
                    modules[index].version.c_str(), descriptor.version.c_str());
            break;
        case ModFetch::NotFound:
            Msg("* [mods] %s: %s publishes no release for it", modules[index].id.c_str(),
                modules[index].update_github.c_str());
            mod_set_state(index, State::NoRelease);
            break;
        case ModFetch::Failed:
            Msg("* [mods] %s: update check failed - %s", modules[index].id.c_str(), error.c_str());
            mod_set_state(index, State::CheckFailed, std::move(error));
            break;
        }
    }
}

bool mod_queue(ModService& service, size_t index);

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
    for (;;)
    {
        size_t update = 0;
        bool check = false;
        {
            std::lock_guard lock(service.mutex);
            if (service.stop.load(std::memory_order_acquire))
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
            mod_run_check();
            mod_queue_requests();
        }
        else
            mod_run_update(update);
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
    case State::Downloading: return "downloading";
    case State::Unpacking: return "unpacking";
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

bool ModUpdateService::RestartPromptPending()
{
    return mod_service().restartPrompt.load(std::memory_order_acquire);
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

void ModUpdateService::AcknowledgeRestartPrompt()
{
    ModService& service = mod_service();
    service.restartPrompt.store(false, std::memory_order_release);
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
        if (status.state == State::Downloading)
            status.downloadedBytes = service.downloaded.load(std::memory_order_acquire);
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
