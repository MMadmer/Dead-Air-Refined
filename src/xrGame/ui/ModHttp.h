// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// WinHTTP for the Mods menu: the module updater, the mod catalog, VirusTotal and the review
// inbox. One session per worker - WinHTTP pools connections per session.

#ifdef XR_PLATFORM_WINDOWS
#include <winhttp.h>

#include <atomic>
#include <string>
#include <string_view>

namespace ModHttp
{
class Session
{
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    HINTERNET Handle();

private:
    HINTERNET m_session{};
};

class Request
{
public:
    Request() = default;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    ~Request();

    // [rangeBegin, rangeEnd) of the resource, or all of it when rangeEnd is zero. `headers` are
    // extra request headers, each ending in CRLF; `body` is sent as is.
    bool Send(Session& session, const std::wstring& url, u64 rangeBegin, u64 rangeEnd, xr_string& error,
        const wchar_t* method = L"GET", const std::wstring& headers = {}, std::string_view body = {});

    DWORD Status() const { return m_status; }
    bool Read(void* buffer, DWORD capacity, DWORD& received);
    // first byte the server is sending, from Content-Range; false when the header is absent
    bool RangeStart(u64& start) const;
    // where the redirects ended
    std::wstring FinalUrl() const;

private:
    HINTERNET m_connection{};
    HINTERNET m_request{};
    DWORD m_status{};
};

enum class Fetch
{
    Ok,
    NotFound,
    Failed
};

// A small text resource, whole, into memory. `stop` is polled between reads.
Fetch FetchText(Session& session, const std::wstring& url, size_t limit, xr_string& body, xr_string& error,
    const std::atomic_bool* stop = nullptr, const std::wstring& headers = {});

// A request with a body; the response body (up to `limit`) and the status come back whatever
// the status is - an API answers errors in its body.
bool Exchange(Session& session, const wchar_t* method, const std::wstring& url, const std::wstring& headers,
    std::string_view body, size_t limit, xr_string& response, DWORD& status, xr_string& error);

// Every string that reaches a URL has been held to an ASCII grammar before.
std::wstring Widen(std::string_view ascii);

// application/x-www-form-urlencoded value
xr_string UrlEncode(std::string_view text);
} // namespace ModHttp
#endif
