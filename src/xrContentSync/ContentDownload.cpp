#include "ContentDownload.h"

#include "ContentHash.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
// std::min / std::max are used throughout; the windows.h macros of the same name would
// swallow them.
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace
{
const std::atomic_bool g_never{};

constexpr DWORD TransferBuffer = 1024 * 1024;

// A resume rewinds this far before continuing. The last write before an interruption is the
// one most likely to be short or torn, and re-fetching a megabyte is cheaper than discovering
// at the end of a 300 MB download that the hash does not match.
constexpr std::uint64_t ResumeRewind = 1024 * 1024;

constexpr unsigned MaximumAttempts = 5;

// Silence, not total time. A 300 MB bundle on a slow line legitimately takes an hour; two
// minutes without a single byte is a dead connection.
constexpr DWORD IdleTimeoutMs = 120 * 1000;

// Cancellation has to feel immediate, so every wait is chopped into slices this long.
constexpr DWORD CancelPollMs = 200;

bool sleep_cancellable(unsigned milliseconds, const std::atomic_bool& cancel)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += CancelPollMs)
    {
        if (cancel.load(std::memory_order_acquire))
            return false;
        Sleep(std::min<DWORD>(CancelPollMs, milliseconds - elapsed));
    }
    return !cancel.load(std::memory_order_acquire);
}

std::wstring widen(const std::string& value)
{
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string narrow(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size,
        nullptr, nullptr);
    return result;
}

// A single GET, with the request built and sent in two steps so a caller can attach a Range
// header. UpdateService's equivalent sends inside its constructor, which is exactly why it
// cannot be reused here.
class HttpGet
{
public:
    ~HttpGet()
    {
        if (m_request)
            WinHttpCloseHandle(m_request);
        if (m_connection)
            WinHttpCloseHandle(m_connection);
        if (m_session)
            WinHttpCloseHandle(m_session);
    }

    bool Send(const std::wstring& url, const std::wstring& headers, std::string& error)
    {
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256]{};
        wchar_t path[2048]{};
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        {
            error = "the download URL could not be parsed";
            return false;
        }

        m_session = WinHttpOpen(L"DeadAirRefined/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session)
        {
            error = "the HTTP client could not start";
            return false;
        }
        // Resolve/connect/send stay short; only the receive-data timeout is long, because that
        // is the one measuring silence during a legitimately slow transfer.
        WinHttpSetTimeouts(m_session, 10000, 15000, 30000, IdleTimeoutMs);

        m_connection = WinHttpConnect(m_session, host, parts.nPort, 0);
        if (!m_connection)
        {
            error = "the download host could not be reached";
            return false;
        }

        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        m_request = WinHttpOpenRequest(m_connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!m_request)
        {
            error = "the download request could not be created";
            return false;
        }

        if (!headers.empty())
            WinHttpAddRequestHeaders(m_request, headers.c_str(), static_cast<DWORD>(headers.size()),
                WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(m_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(m_request, nullptr))
        {
            error = "the download did not start";
            return false;
        }

        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(m_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        {
            error = "the download response could not be read";
            return false;
        }
        m_status = status;
        return true;
    }

    DWORD Status() const { return m_status; }

    std::wstring Header(DWORD query) const
    {
        DWORD size = 0;
        WinHttpQueryHeaders(m_request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
            WINHTTP_NO_HEADER_INDEX);
        if (!size || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return {};
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(m_request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                WINHTTP_NO_HEADER_INDEX))
            return {};
        value.resize(wcslen(value.c_str()));
        return value;
    }

    bool Read(void* buffer, DWORD capacity, DWORD& read) { return WinHttpReadData(m_request, buffer, capacity, &read) != FALSE; }

private:
    HINTERNET m_session{};
    HINTERNET m_connection{};
    HINTERNET m_request{};
    DWORD m_status{};
};

// `Content-Range: bytes <start>-<end>/<total>`. Both ends are checked by the caller: a server
// that answers 206 from the wrong offset, or for a resource of a different length, is
// answering about a different file.
bool parse_content_range(const std::wstring& header, std::uint64_t& start, std::uint64_t& total)
{
    const std::string value = narrow(header);
    const std::size_t bytes = value.find("bytes ");
    if (bytes == std::string::npos)
        return false;
    const std::size_t dash = value.find('-', bytes);
    const std::size_t slash = value.find('/', bytes);
    if (dash == std::string::npos || slash == std::string::npos || dash > slash)
        return false;

    const auto to_u64 = [](std::string_view text, std::uint64_t& out)
    {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
        return error == std::errc{} && end == text.data() + text.size();
    };
    return to_u64(std::string_view(value).substr(bytes + 6, dash - bytes - 6), start) &&
        to_u64(std::string_view(value).substr(slash + 1), total);
}

struct Shared
{
    std::mutex mutex;
    std::size_t next{};
    std::size_t running{};
    std::uint64_t done{};
    std::uint64_t total{};
    bool failed{};
    std::string error;
};
}

namespace ContentDownload
{
namespace
{
// One asset, start to finish: resume if there is a usable part, otherwise from zero; verify;
// publish into the cache under its hash.
bool fetch_one(const ContentManifest::Bundle& bundle, const ContentPaths::Layout& paths,
    const Options& options, const std::atomic_bool& cancel, Shared& shared, std::string& error)
{
    const std::filesystem::path part = paths.Cache() / (bundle.hash + ".part");
    const std::filesystem::path finished = paths.Cache() / bundle.hash;

    std::string base = options.qaBaseUrl.empty()
        ? "https://github.com/" + options.repo + "/releases/download"
        : options.qaBaseUrl;
    if (!base.empty() && base.back() == '/')
        base.pop_back();
    const std::wstring url = widen(base + "/" + bundle.releaseTag + "/" + bundle.name);

    // A mismatch after a complete transfer is worth exactly one retry from scratch: a truncated
    // proxy response is plausible, the same wrong bytes twice is not, and looping on it would
    // burn the player's bandwidth forever.
    for (unsigned restart = 0; restart < 2; ++restart)
    {
        std::uint64_t offset = 0;
        ContentHash::Stream digest;
        if (!digest.Open())
        {
            error = "the hashing provider could not start";
            return false;
        }

        std::error_code fsError;
        const std::uint64_t partSize = std::filesystem::exists(part, fsError)
            ? std::filesystem::file_size(part, fsError)
            : 0;

        if (!fsError && partSize >= bundle.size)
        {
            // A part that is already long enough is usually the finished file from a run that
            // died between the last write and the rename. One local hash is far cheaper than
            // re-fetching hundreds of megabytes to discover the same thing.
            if (ContentHash::File(part.wstring(), cancel) == bundle.hash)
            {
                std::error_code renameError;
                std::filesystem::rename(part, finished, renameError);
                if (!renameError)
                    return true;
            }
            if (cancel.load(std::memory_order_acquire))
            {
                error = "cancelled";
                return false;
            }
        }
        else if (!fsError && partSize > ResumeRewind)
        {
            // Rewind past the last write, which is the one most likely to be short or torn,
            // and re-feed what is kept so the running digest covers the whole file. That read
            // is the proof: bytes that cannot be read back are not bytes to resume from.
            const std::uint64_t keep = partSize - ResumeRewind;
            std::filesystem::resize_file(part, keep, fsError);
            if (!fsError)
            {
                ContentHash::Stream resumed;
                if (resumed.Open())
                {
                    std::vector<unsigned char> buffer(TransferBuffer);
                    HANDLE handle = CreateFileW(part.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                    bool ok = handle != INVALID_HANDLE_VALUE;
                    while (ok)
                    {
                        if (cancel.load(std::memory_order_acquire))
                        {
                            ok = false;
                            break;
                        }
                        DWORD read = 0;
                        if (!ReadFile(handle, buffer.data(), TransferBuffer, &read, nullptr))
                        {
                            ok = false;
                            break;
                        }
                        if (!read)
                            break;
                        ok = resumed.Append(buffer.data(), read);
                    }
                    if (handle != INVALID_HANDLE_VALUE)
                        CloseHandle(handle);
                    if (ok)
                    {
                        digest = std::move(resumed);
                        offset = keep;
                    }
                }
            }
        }

        if (cancel.load(std::memory_order_acquire))
        {
            // Cancelled while examining the part. Leave it alone - it is the resume state, and
            // deleting it here would turn a pause into a discard.
            error = "cancelled";
            return false;
        }
        if (!offset)
            std::filesystem::remove(part, fsError);

        std::error_code createError;
        std::filesystem::create_directories(paths.Cache(), createError);

        HANDLE output = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            offset ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE)
        {
            error = "the content cache could not be written";
            return false;
        }
        LARGE_INTEGER seek{};
        seek.QuadPart = static_cast<LONGLONG>(offset);
        SetFilePointerEx(output, seek, nullptr, FILE_BEGIN);

        bool transferred = false;
        bool restartFromZero = false;
        // Bytes this pass has already reported as progress. A restart gives them back, so the
        // bar cannot climb past the total on a retry.
        std::uint64_t credited = 0;
        for (unsigned attempt = 0; attempt < MaximumAttempts && !transferred; ++attempt)
        {
            if (attempt)
            {
                const unsigned backoff = 1000u << (attempt - 1); // 1, 2, 4, 8 s
                if (!sleep_cancellable(backoff, cancel))
                    break;
            }
            if (cancel.load(std::memory_order_acquire))
                break;

            // The write position is re-established from `offset` on every attempt. A previous
            // attempt that died on a short write left the OS file pointer past `offset`, and
            // resuming from there would splice the incoming bytes at the wrong place while the
            // digest - which was never fed the failed chunk - still hashed the correct stream.
            // The file would then verify against a hash it does not contain.
            seek.QuadPart = static_cast<LONGLONG>(offset);
            SetFilePointerEx(output, seek, nullptr, FILE_BEGIN);
            SetEndOfFile(output);

            HttpGet request;
            std::wstring headers;
            if (offset)
                headers = L"Range: bytes=" + std::to_wstring(offset) + L"-";

            std::string sendError;
            if (!request.Send(url, headers, sendError))
            {
                error = sendError;
                continue;
            }

            if (offset && request.Status() == 206)
            {
                std::uint64_t start = 0;
                std::uint64_t total = 0;
                if (!parse_content_range(request.Header(WINHTTP_QUERY_CONTENT_RANGE), start, total) ||
                    start != offset || total != bundle.size)
                {
                    // The server is answering about something else. Discard what we have and
                    // let the outer loop start the bundle from zero - keeping the part and
                    // giving up would make the state permanently unresumable.
                    error = "the server answered a partial request about a different file";
                    restartFromZero = true;
                    break;
                }
            }
            else if (request.Status() == 200)
            {
                if (offset)
                {
                    // The server ignored the range and is sending the whole thing. Start over
                    // rather than appending a second copy of the head.
                    seek.QuadPart = 0;
                    SetFilePointerEx(output, seek, nullptr, FILE_BEGIN);
                    SetEndOfFile(output);
                    offset = 0;
                    ContentHash::Stream fresh;
                    if (!fresh.Open())
                    {
                        error = "the hashing provider could not restart";
                        break;
                    }
                    digest = std::move(fresh);

                    std::lock_guard guard(shared.mutex);
                    shared.done -= credited;
                    credited = 0;
                }
            }
            else
            {
                error = "the download returned HTTP " + std::to_string(request.Status());
                continue;
            }

            std::vector<unsigned char> buffer(TransferBuffer);
            bool broke = false;
            while (true)
            {
                if (cancel.load(std::memory_order_acquire))
                {
                    broke = true;
                    break;
                }
                DWORD read = 0;
                if (!request.Read(buffer.data(), TransferBuffer, read))
                {
                    error = "the transfer was interrupted";
                    broke = true;
                    break;
                }
                if (!read)
                    break;

                DWORD written = 0;
                if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read ||
                    !digest.Append(buffer.data(), read))
                {
                    error = "the content cache could not be written";
                    broke = true;
                    break;
                }
                offset += read;
                credited += read;

                std::lock_guard guard(shared.mutex);
                shared.done += read;
            }
            if (restartFromZero)
                break;
            if (!broke)
                transferred = offset >= bundle.size;
            if (!transferred && !cancel.load(std::memory_order_acquire))
            {
                // Whatever landed is kept: the next attempt resumes from it rather than
                // starting the whole bundle again.
                continue;
            }
            break;
        }

        CloseHandle(output);
        if (cancel.load(std::memory_order_acquire))
        {
            error = "cancelled";
            return false;
        }
        if (restartFromZero)
        {
            // Drop the part so the next pass of the outer loop starts clean.
            std::error_code discardError;
            std::filesystem::remove(part, discardError);
            {
                std::lock_guard guard(shared.mutex);
                shared.done -= credited;
            }
            continue;
        }
        if (!transferred)
            return false;

        // The logical counter says the transfer is complete; the file on disk is the thing
        // that actually has to be. A length that disagrees means a write went somewhere the
        // digest did not follow, and the hash below would then vouch for bytes the file does
        // not contain.
        std::error_code lengthError;
        if (std::filesystem::file_size(part, lengthError) != bundle.size || lengthError)
        {
            std::error_code discardError;
            std::filesystem::remove(part, discardError);
            error = "the downloaded bundle is the wrong length";
            continue;
        }

        const std::string actual = digest.Finish();
        if (actual == bundle.hash)
        {
            std::error_code moveError;
            std::filesystem::rename(part, finished, moveError);
            if (moveError)
            {
                error = "the downloaded bundle could not be published into the cache";
                return false;
            }
            return true;
        }

        std::error_code removeError;
        std::filesystem::remove(part, removeError);
        error = "the downloaded bundle did not match its hash";
        if (options.onLog)
            options.onLog(bundle.name + ": hash mismatch, restarting once");
    }
    return false;
}
}

std::string QaBaseUrl()
{
    wchar_t value[512]{};
    const DWORD length =
        GetEnvironmentVariableW(L"DAR_QA_CONTENT_BASE", value, static_cast<DWORD>(std::size(value)));
    if (!length || length >= std::size(value))
        return {};

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t user[256]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUserName = user;
    parts.dwUserNameLength = static_cast<DWORD>(std::size(user));
    if (!WinHttpCrackUrl(value, length, 0, &parts))
        return {};
    if (parts.nScheme != INTERNET_SCHEME_HTTP || user[0])
        return {};
    if (_wcsicmp(host, L"127.0.0.1") != 0 && _wcsicmp(host, L"localhost") != 0 &&
        _wcsicmp(host, L"::1") != 0)
        return {};

    // Rebuilt from what was parsed rather than echoed back, so nothing the parser ignored can
    // ride along into the URL the downloader builds.
    return "http://" + narrow(host) + ":" + std::to_string(parts.nPort);
}

Result Fetch(const ContentResolver::Plan& plan, const ContentPaths::Layout& paths, const Options& options)
{
    Result result;
    const std::atomic_bool& cancel = options.cancel ? *options.cancel : g_never;

    std::vector<ContentManifest::Bundle> work;
    for (const ContentResolver::Job& job : plan.jobs)
    {
        if (!job.cached)
            work.push_back(job.bundle);
    }
    if (work.empty())
    {
        result.ok = true;
        return result;
    }

    Shared shared;
    for (const ContentManifest::Bundle& bundle : work)
        shared.total += bundle.size;

    const unsigned threads = std::max(1u, std::min(options.concurrency, static_cast<unsigned>(work.size())));
    shared.running = threads;
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (unsigned index = 0; index < threads; ++index)
    {
        workers.emplace_back([&]
        {
            while (true)
            {
                ContentManifest::Bundle bundle;
                {
                    std::lock_guard guard(shared.mutex);
                    if (shared.failed || shared.next >= work.size())
                    {
                        --shared.running;
                        return;
                    }
                    bundle = work[shared.next++];
                }
                if (cancel.load(std::memory_order_acquire))
                {
                    std::lock_guard guard(shared.mutex);
                    --shared.running;
                    return;
                }

                if (options.onLog)
                    options.onLog("fetching " + bundle.name);

                std::string error;
                if (!fetch_one(bundle, paths, options, cancel, shared, error))
                {
                    std::lock_guard guard(shared.mutex);
                    if (!shared.failed)
                    {
                        shared.failed = true;
                        shared.error = bundle.name + ": " + (error.empty() ? "download failed" : error);
                    }
                    --shared.running;
                    return;
                }
            }
        });
    }

    // The progress pump runs on the calling thread so the caller's file writes and UI updates
    // stay on one thread and need no locking of their own. It keeps running until the last
    // worker is gone, cancellation included: the installer's stall detector watches the
    // progress file's timestamp, and going quiet while the workers wind down would look
    // exactly like a fetcher that died.
    while (true)
    {
        bool running = false;
        Progress progress;
        {
            std::lock_guard guard(shared.mutex);
            progress.done = shared.done;
            progress.total = shared.total;
            // Keyed on live workers, not on bytes: a resumed part contributes bytes that were
            // already on disk, so `done` legitimately finishes below `total`.
            running = shared.running != 0;
        }
        if (options.onProgress)
            options.onProgress(progress);
        if (!running)
            break;
        Sleep(CancelPollMs * 2);
    }

    for (std::thread& worker : workers)
        worker.join();

    {
        std::lock_guard guard(shared.mutex);
        result.fetched = shared.done;
        result.ok = !shared.failed;
        result.error = shared.error;
    }
    if (cancel.load(std::memory_order_acquire))
    {
        result.ok = false;
        result.error = "cancelled";
    }
    return result;
}
}
