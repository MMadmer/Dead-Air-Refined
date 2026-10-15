// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "ContentDownload.h"

#include "ContentChunks.h"
#include "ContentHash.h"
#include "ContentState.h"

#include <algorithm>
#include <charconv>
#include <memory>
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

// A rebuild from chunks is taken only when it saves a real share of the bundle. Past this the
// one streaming request is simpler and barely larger.
constexpr double ChunkWorthRatio = 0.8;

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

// "Could not be written" alone has already cost a field diagnosis: it covers a full disk, a part
// file another process holds open and a directory that is not there alike, and only the OS
// code tells them apart. Call it straight after the failing call, before anything else runs.
std::string with_last_error(const char* message)
{
    const DWORD code = GetLastError();
    std::string result(message);
    if (code)
        result += " (Windows error " + std::to_string(code) + ")";
    return result;
}

// One per worker. WinHTTP pools connections per session, so the dozens of ranged requests a
// rebuild makes against one host reuse a connection instead of shaking hands for each.
class HttpSession
{
public:
    ~HttpSession()
    {
        if (m_session)
            WinHttpCloseHandle(m_session);
    }

    HINTERNET Handle(std::string& error)
    {
        if (m_session)
            return m_session;
        m_session = WinHttpOpen(L"DeadAirRefined/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session)
        {
            error = "the HTTP client could not start";
            return nullptr;
        }
        // Resolve/connect/send stay short; only the receive-data timeout is long, because that
        // is the one measuring silence during a legitimately slow transfer.
        WinHttpSetTimeouts(m_session, 10000, 15000, 30000, IdleTimeoutMs);
        return m_session;
    }

private:
    HINTERNET m_session{};
};

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
    }

    bool Send(HttpSession& session, const std::wstring& url, const std::wstring& headers, std::string& error)
    {
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        {
            error = "the download URL could not be parsed";
            return false;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        // The storage URL a release download redirects to carries a long signed query, so the
        // path and the query travel together.
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        const HINTERNET handle = session.Handle(error);
        if (!handle)
            return false;

        m_connection = WinHttpConnect(handle, host.c_str(), parts.nPort, 0);
        if (!m_connection)
        {
            error = "the download host could not be reached";
            return false;
        }

        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        m_request = WinHttpOpenRequest(m_connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
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

    // Where the redirects ended. The storage URL answers ranged requests directly, which saves
    // the redirect on every request after the first; it is signed and expires, so a refusal from
    // it just sends the next request the long way round again.
    std::wstring FinalUrl() const
    {
        DWORD size = 0;
        WinHttpQueryOption(m_request, WINHTTP_OPTION_URL, nullptr, &size);
        if (!size)
            return {};
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryOption(m_request, WINHTTP_OPTION_URL, value.data(), &size))
            return {};
        value.resize(wcslen(value.c_str()));
        return value;
    }

    bool Read(void* buffer, DWORD capacity, DWORD& read) { return WinHttpReadData(m_request, buffer, capacity, &read) != FALSE; }

private:
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

void credit(Shared& shared, std::uint64_t bytes)
{
    std::lock_guard guard(shared.mutex);
    shared.done += bytes;
}

void debit(Shared& shared, std::uint64_t bytes)
{
    std::lock_guard guard(shared.mutex);
    shared.done -= std::min(shared.done, bytes);
}
}

namespace ContentDownload
{
namespace
{
// What the transfer loop needs to know about the bundle it is fetching: a named file of a known
// size and hash, published under a release tag.
struct Asset
{
    std::string name;
    std::string hash;
    std::string releaseTag;
    std::uint64_t size{};
};

// One job: a bundle, and what it needs to be rebuilt from chunks when that pays - the published
// map and the revisions of its slot this installation already holds.
struct Work
{
    Asset bundle;
    std::shared_ptr<const ContentChunks::Map> map;
    std::vector<std::filesystem::path> bases;
    // What this job is expected to move over the network, for the progress total.
    std::uint64_t expected{};
};

std::wstring asset_url(const Options& options, const std::string& releaseTag, const std::string& name)
{
    std::string base = options.qaBaseUrl.empty()
        ? "https://github.com/" + options.repo + "/releases/download"
        : options.qaBaseUrl;
    if (!base.empty() && base.back() == '/')
        base.pop_back();
    return widen(base + "/" + releaseTag + "/" + name);
}

enum class Small
{
    Ok,
    NotFound,
    Failed
};

// A small asset, whole, into memory.
Small fetch_small(HttpSession& session, const std::wstring& url, std::size_t limit, std::vector<std::uint8_t>& body,
    const std::atomic_bool& cancel, std::string& error)
{
    for (unsigned attempt = 0; attempt < MaximumAttempts; ++attempt)
    {
        if (attempt && !sleep_cancellable(1000u << (attempt - 1), cancel))
            break;
        body.clear();
        HttpGet request;
        if (!request.Send(session, url, {}, error))
            continue;
        if (request.Status() == 404)
            return Small::NotFound;
        if (request.Status() != 200)
        {
            error = "HTTP " + std::to_string(request.Status());
            continue;
        }
        std::vector<std::uint8_t> buffer(64 * 1024);
        bool ok = true;
        while (ok)
        {
            DWORD read = 0;
            if (cancel.load(std::memory_order_acquire) || !request.Read(buffer.data(), static_cast<DWORD>(buffer.size()), read))
            {
                ok = false;
                break;
            }
            if (!read)
                break;
            if (body.size() + read > limit)
            {
                error = "the asset is larger than expected";
                return Small::Failed;
            }
            body.insert(body.end(), buffer.begin(), buffer.begin() + read);
        }
        if (ok)
            return Small::Ok;
        error = "the transfer was interrupted";
    }
    if (cancel.load(std::memory_order_acquire))
        error = "cancelled";
    return Small::Failed;
}

// The revisions of a bundle's slot this installation holds: every bundle-shaped file in
// database\ with the same group and shard, whatever its hash - including a damaged copy under
// the wanted name, most of which is usually still right.
std::vector<std::filesystem::path> slot_bases(const ContentPaths::Layout& paths, const std::string& bundleName)
{
    std::vector<std::filesystem::path> bases;
    ContentManifest::BundleName wanted;
    if (!ContentManifest::SplitBundleName(bundleName, wanted))
        return bases;

    std::error_code scanError;
    std::filesystem::directory_iterator entry(paths.Database(), scanError);
    const std::filesystem::directory_iterator last;
    while (!scanError && entry != last)
    {
        std::error_code entryError;
        const std::string name = entry->path().filename().string();
        ContentManifest::BundleName parts;
        if (entry->is_regular_file(entryError) && !entryError && ContentManifest::SplitBundleName(name, parts) &&
            parts.group == wanted.group && parts.shard == wanted.shard)
        {
            if (name == bundleName)
                bases.insert(bases.begin(), entry->path());
            else
                bases.push_back(entry->path());
        }
        entry.increment(scanError);
    }

    // Revisions an earlier commit retired into the cache. There they are named by their hash
    // alone; the state file still says which bundle a hash was, because it records every bundle
    // a pass has read. A client before 1.6.0 retires the old revisions even when the new ones did
    // not arrive, so after such an update this is where the bases are.
    for (const auto& [known, record] : ContentState::LoadCache(paths.State()))
    {
        ContentManifest::BundleName parts;
        const std::filesystem::path retired = paths.Cache() / record.hash;
        std::error_code sizeError;
        if (known != bundleName && ContentManifest::SplitBundleName(known, parts) && parts.group == wanted.group &&
            parts.shard == wanted.shard && ContentManifest::IsSha256Hex(record.hash) &&
            std::filesystem::file_size(retired, sizeError) == record.size && !sizeError &&
            std::ranges::find(bases, retired) == bases.end())
        {
            bases.push_back(retired);
        }
    }
    return bases;
}

// Where a resumed transfer would start, the same arithmetic fetch_one uses.
std::uint64_t resume_offset(const ContentPaths::Layout& paths, const Asset& asset)
{
    std::error_code error;
    const std::filesystem::path part = paths.Cache() / (asset.hash + ".part");
    const std::uint64_t size = std::filesystem::exists(part, error) ? std::filesystem::file_size(part, error) : 0;
    if (error || size >= asset.size)
        return 0;
    return size > ResumeRewind ? size - ResumeRewind : 0;
}

// Fetches the chunk map of a bundle that has a local revision to rebuild from, and works out
// what the job is expected to download. A missing or unusable map is not an error: the bundle
// is then fetched whole, as it always was.
void prepare(Work& item, HttpSession& session, const ContentPaths::Layout& paths, const Options& options,
    const std::atomic_bool& cancel)
{
    const Asset& bundle = item.bundle;
    item.expected = bundle.size - resume_offset(paths, bundle);
    item.bases = slot_bases(paths, bundle.name);
    if (item.bases.empty())
        return;

    std::vector<std::uint8_t> bytes;
    std::string error;
    const Small fetched = fetch_small(session, asset_url(options, bundle.releaseTag, ContentChunks::MapName(bundle.name)),
        static_cast<std::size_t>(ContentChunks::MaximumMapBytes), bytes, cancel, error);
    if (fetched != Small::Ok)
    {
        if (options.onLog && fetched == Small::Failed && !cancel.load(std::memory_order_acquire))
            options.onLog(bundle.name + ": no chunk map (" + error + "), fetching it whole");
        return;
    }

    auto map = std::make_shared<ContentChunks::Map>();
    if (!ContentChunks::ParseMap(bytes.data(), bytes.size(), *map, error) || map->targetHash != bundle.hash ||
        map->targetSize != bundle.size)
    {
        if (options.onLog)
            options.onLog(bundle.name + ": the chunk map is not usable (" +
                (error.empty() ? std::string("it describes another file") : error) + "), fetching it whole");
        return;
    }

    const std::uint64_t from = resume_offset(paths, bundle);
    ContentChunks::Plan plan;
    if (!ContentChunks::PlanTarget(*map, item.bases, from, plan, cancel, error))
        return;
    if (plan.remoteBytes < ChunkWorthRatio * static_cast<double>(bundle.size - from))
    {
        item.map = std::move(map);
        item.expected = plan.remoteBytes;
        if (options.onLog)
            options.onLog(bundle.name + ": " + std::to_string(plan.localBytes) + " byte(s) already here, " +
                std::to_string(plan.remoteBytes) + " to fetch in " + std::to_string(plan.ranges) + " range(s)");
    }
}

enum class Rebuild
{
    Done,
    Fallback, // the bytes so far are good; finish the rest by streaming
    Failed    // keep the part for a resume, report the error
};

// Writes the target from `offset` to its end: runs the installation already has are copied out
// of the base files, the rest is fetched by range from the published bundle. Bytes go into the
// part file and the running digest in target order, so a part left by an interrupted rebuild is
// a prefix like any other and resumes either way.
Rebuild rebuild(const ContentChunks::Plan& plan, const std::wstring& url, const Asset& bundle, HANDLE output,
    std::uint64_t& offset, ContentHash::Stream& digest, HttpSession& session, Shared& shared,
    std::uint64_t& credited, const std::atomic_bool& cancel, std::string& error)
{
    std::vector<unsigned char> buffer(TransferBuffer);
    std::wstring direct;

    const auto write = [&](const unsigned char* data, DWORD size)
    {
        DWORD written = 0;
        if (!WriteFile(output, data, size, &written, nullptr) || written != size || !digest.Append(data, size))
        {
            error = with_last_error("the content cache could not be written");
            return false;
        }
        offset += size;
        return true;
    };

    for (const ContentChunks::Segment& segment : plan.segments)
    {
        if (cancel.load(std::memory_order_acquire))
        {
            error = "cancelled";
            return Rebuild::Failed;
        }
        if (segment.targetOffset != offset)
        {
            error = "the rebuild plan does not continue where the part ends";
            return Rebuild::Fallback;
        }

        if (!segment.Remote())
        {
            const HANDLE base = CreateFileW(plan.bases[segment.base].c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (base == INVALID_HANDLE_VALUE)
                return Rebuild::Fallback;
            LARGE_INTEGER seek{};
            seek.QuadPart = static_cast<LONGLONG>(segment.baseOffset);
            bool ok = SetFilePointerEx(base, seek, nullptr, FILE_BEGIN) != FALSE;
            for (std::uint64_t left = segment.length; ok && left;)
            {
                DWORD read = 0;
                const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(left, TransferBuffer));
                ok = ReadFile(base, buffer.data(), want, &read, nullptr) && read == want;
                if (ok && !write(buffer.data(), read))
                {
                    CloseHandle(base);
                    return Rebuild::Failed;
                }
                left -= read;
            }
            CloseHandle(base);
            // A base that changed under the plan is not an error worth stopping for: the bytes
            // written so far are hashed with the rest, and streaming finishes the job.
            if (!ok)
                return Rebuild::Fallback;
            continue;
        }

        const std::uint64_t end = segment.targetOffset + segment.length;
        for (unsigned failures = 0; offset < end;)
        {
            if (failures == MaximumAttempts)
                return Rebuild::Failed;
            if (failures && !sleep_cancellable(1000u << (failures - 1), cancel))
            {
                error = "cancelled";
                return Rebuild::Failed;
            }

            HttpGet request;
            const bool viaDirect = !direct.empty();
            const std::wstring headers = L"Range: bytes=" + std::to_wstring(offset) + L"-" + std::to_wstring(end - 1);
            if (!request.Send(session, viaDirect ? direct : url, headers, error))
            {
                direct.clear();
                ++failures;
                continue;
            }
            if (request.Status() == 200)
            {
                // The server ignores ranges. Streaming the rest from here is the one way left,
                // and the streaming path knows how to deal with such a server.
                return Rebuild::Fallback;
            }
            std::uint64_t start = 0;
            std::uint64_t total = 0;
            if (request.Status() != 206 || !parse_content_range(request.Header(WINHTTP_QUERY_CONTENT_RANGE), start, total) ||
                start != offset || total != bundle.size)
            {
                error = "the download returned HTTP " + std::to_string(request.Status());
                if (viaDirect)
                    direct.clear();
                else if (request.Status() == 404)
                    return Rebuild::Failed;
                else
                    ++failures;
                continue;
            }
            if (!viaDirect)
                direct = request.FinalUrl();

            while (offset < end)
            {
                if (cancel.load(std::memory_order_acquire))
                {
                    error = "cancelled";
                    return Rebuild::Failed;
                }
                DWORD read = 0;
                const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(end - offset, TransferBuffer));
                if (!request.Read(buffer.data(), want, read) || !read)
                {
                    error = "the transfer was interrupted";
                    ++failures;
                    break;
                }
                if (!write(buffer.data(), read))
                    return Rebuild::Failed;
                credited += read;
                credit(shared, read);
                // bytes that arrived are progress: a long range on a bad line is not a failing one
                failures = 0;
            }
        }
    }
    return Rebuild::Done;
}

// One asset, start to finish: resume if there is a usable part, otherwise from zero; rebuild
// from chunks where that pays, stream the rest; verify; publish into the cache under its hash.
bool fetch_one(const Asset& bundle, const Work& item, HttpSession& session, const ContentPaths::Layout& paths,
    const Options& options, const std::atomic_bool& cancel, Shared& shared, std::string& error)
{
    const std::filesystem::path part = paths.Cache() / (bundle.hash + ".part");
    const std::filesystem::path finished = paths.Cache() / bundle.hash;
    const std::wstring url = asset_url(options, bundle.releaseTag, bundle.name);

    // A mismatch after a complete transfer is worth exactly one retry from scratch: a truncated
    // proxy response is plausible, the same wrong bytes twice is not, and looping on it would
    // burn the player's bandwidth forever. The retry streams the whole bundle - if a rebuild
    // produced the wrong bytes, a second rebuild would only produce them again.
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
            error = with_last_error("the content cache could not be written");
            return false;
        }
        LARGE_INTEGER seek{};
        seek.QuadPart = static_cast<LONGLONG>(offset);
        SetFilePointerEx(output, seek, nullptr, FILE_BEGIN);
        SetEndOfFile(output);

        // Bytes this pass has already reported as progress. A restart gives them back, so the
        // bar cannot climb past the total on a retry.
        std::uint64_t credited = 0;
        bool transferred = false;
        bool restartFromZero = false;

        if (!restart && item.map && offset < bundle.size)
        {
            ContentChunks::Plan plan;
            std::string planError;
            if (ContentChunks::PlanTarget(*item.map, item.bases, offset, plan, cancel, planError) &&
                plan.remoteBytes < ChunkWorthRatio * static_cast<double>(bundle.size - offset))
            {
                switch (rebuild(plan, url, bundle, output, offset, digest, session, shared, credited, cancel, error))
                {
                case Rebuild::Done: transferred = offset >= bundle.size; break;
                case Rebuild::Fallback:
                    if (options.onLog)
                        options.onLog(bundle.name + ": the rebuild from chunks stopped, streaming the rest");
                    break;
                case Rebuild::Failed:
                    CloseHandle(output);
                    return false;
                }
            }
        }

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
            if (!request.Send(session, url, headers, sendError))
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
                    debit(shared, credited);
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
                    error = with_last_error("the content cache could not be written");
                    broke = true;
                    break;
                }
                offset += read;
                credited += read;
                credit(shared, read);
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
            debit(shared, credited);
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
            debit(shared, credited);
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
        debit(shared, credited);
        error = "the downloaded bundle did not match its hash";
        if (options.onLog)
            options.onLog(bundle.name + ": hash mismatch, restarting once");
    }
    return false;
}

std::vector<Work> collect_work(const ContentResolver::Plan& plan)
{
    std::vector<Work> work;
    for (const ContentResolver::Job& job : plan.jobs)
    {
        if (job.cached)
            continue;

        Work item;
        item.bundle = {job.bundle.name, job.bundle.hash, job.bundle.releaseTag, job.bundle.size};
        work.push_back(std::move(item));
    }
    return work;
}

// Works out every job's download before any of them starts, so the total the player sees is the
// one that will actually move. Several workers, because a rebuild's plan reads the bases it
// rebuilds from, and that is disk time worth overlapping.
bool prepare_all(std::vector<Work>& work, const ContentPaths::Layout& paths, const Options& options,
    const std::atomic_bool& cancel)
{
    std::atomic_size_t next{};
    const unsigned threads = std::max(1u, std::min(options.concurrency, static_cast<unsigned>(work.size())));
    std::atomic_uint running{threads};
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < threads; ++index)
    {
        workers.emplace_back([&]
        {
            HttpSession session;
            for (std::size_t item = next++; item < work.size() && !cancel.load(std::memory_order_acquire); item = next++)
                prepare(work[item], session, paths, options, cancel);
            --running;
        });
    }
    // The same heartbeat as the transfer: the installer's stall detector watches the progress
    // file, and cutting a large base takes seconds.
    while (true)
    {
        if (options.onProgress)
        {
            Progress progress;
            progress.planning = true;
            options.onProgress(progress);
        }
        if (!running.load())
            break;
        Sleep(CancelPollMs * 2);
    }
    for (std::thread& worker : workers)
        worker.join();
    return !cancel.load(std::memory_order_acquire);
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

std::uint64_t Estimate(const ContentResolver::Plan& plan, const ContentPaths::Layout& paths, const Options& options)
{
    const std::atomic_bool& cancel = options.cancel ? *options.cancel : g_never;
    std::vector<Work> work = collect_work(plan);
    Options quiet = options;
    quiet.onProgress = nullptr;
    prepare_all(work, paths, quiet, cancel);
    std::uint64_t total = 0;
    for (const Work& item : work)
        total += item.expected;
    return total;
}

Result Fetch(const ContentResolver::Plan& plan, const ContentPaths::Layout& paths, const Options& options)
{
    Result result;
    const std::atomic_bool& cancel = options.cancel ? *options.cancel : g_never;

    std::vector<Work> work = collect_work(plan);
    if (work.empty())
    {
        result.ok = true;
        return result;
    }
    if (!prepare_all(work, paths, options, cancel))
    {
        result.error = "cancelled";
        return result;
    }

    Shared shared;
    for (const Work& item : work)
        shared.total += item.expected;

    const unsigned threads = std::max(1u, std::min(options.concurrency, static_cast<unsigned>(work.size())));
    shared.running = threads;
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (unsigned index = 0; index < threads; ++index)
    {
        workers.emplace_back([&]
        {
            HttpSession session;
            while (true)
            {
                const Work* item = nullptr;
                {
                    std::lock_guard guard(shared.mutex);
                    if (shared.failed || shared.next >= work.size())
                    {
                        --shared.running;
                        return;
                    }
                    item = &work[shared.next++];
                }
                if (cancel.load(std::memory_order_acquire))
                {
                    std::lock_guard guard(shared.mutex);
                    --shared.running;
                    return;
                }

                std::string error;
                if (options.onLog)
                    options.onLog("fetching " + item->bundle.name);
                const bool ok = fetch_one(item->bundle, *item, session, paths, options, cancel, shared, error);
                if (!ok)
                    error = item->bundle.name + ": " + (error.empty() ? "download failed" : error);

                if (!ok)
                {
                    std::lock_guard guard(shared.mutex);
                    if (!shared.failed)
                    {
                        shared.failed = true;
                        shared.error = error;
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
            // Keyed on live workers, not on bytes: a fallback from a rebuild to streaming moves
            // more than was planned, and a retry gives bytes back.
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
