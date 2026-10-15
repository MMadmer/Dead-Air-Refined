// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "ModHttp.h"

#ifdef XR_PLATFORM_WINDOWS
#include "xrCore/ProductVersion.h"

#include <array>
#include <vector>

#pragma comment(lib, "winhttp.lib")

ModHttp::Session::~Session()
{
    if (m_session)
        WinHttpCloseHandle(m_session);
}

HINTERNET ModHttp::Session::Handle()
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

ModHttp::Request::~Request()
{
    if (m_request)
        WinHttpCloseHandle(m_request);
    if (m_connection)
        WinHttpCloseHandle(m_connection);
}

bool ModHttp::Request::Send(Session& session, const std::wstring& url, u64 rangeBegin, u64 rangeEnd, xr_string& error,
    const wchar_t* method, const std::wstring& headers, std::string_view body)
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
        WinHttpOpenRequest(m_connection, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) :
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
    if (!headers.empty())
        WinHttpAddRequestHeaders(m_request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD);

    DWORD statusSize = sizeof(m_status);
    LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(m_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, size, size, 0) ||
        !WinHttpReceiveResponse(m_request, nullptr) ||
        !WinHttpQueryHeaders(m_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &m_status, &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        error = "request failed";
        return false;
    }
    return true;
}

bool ModHttp::Request::Read(void* buffer, DWORD capacity, DWORD& received)
{
    return WinHttpReadData(m_request, buffer, capacity, &received) != FALSE;
}

bool ModHttp::Request::RangeStart(u64& start) const
{
    std::array<wchar_t, 128> value{};
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(m_request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
            WINHTTP_NO_HEADER_INDEX))
        return false;
    const std::wstring_view header(value.data());
    const size_t space = header.find(L' ');
    if (space == std::wstring_view::npos)
        return false;
    start = wcstoull(header.data() + space + 1, nullptr, 10);
    return true;
}

std::wstring ModHttp::Request::FinalUrl() const
{
    std::array<wchar_t, 4096> value{};
    DWORD size = sizeof(value);
    return WinHttpQueryOption(m_request, WINHTTP_OPTION_URL, value.data(), &size) ? std::wstring(value.data()) :
                                                                                     std::wstring();
}

namespace
{
bool read_body(ModHttp::Request& request, size_t limit, xr_string& body, xr_string& error, const std::atomic_bool* stop)
{
    std::vector<char> buffer(64 * 1024);
    while (!stop || !stop->load(std::memory_order_acquire))
    {
        DWORD received = 0;
        if (!request.Read(buffer.data(), static_cast<DWORD>(buffer.size()), received))
        {
            error = "download was interrupted";
            return false;
        }
        if (!received)
            return true;
        body.append(buffer.data(), received);
        if (body.size() > limit)
        {
            error = "the answer is larger than allowed";
            return false;
        }
    }
    error = "cancelled";
    return false;
}
} // namespace

ModHttp::Fetch ModHttp::FetchText(Session& session, const std::wstring& url, size_t limit, xr_string& body,
    xr_string& error, const std::atomic_bool* stop, const std::wstring& headers)
{
    Request request;
    if (!request.Send(session, url, 0, 0, error, L"GET", headers))
        return Fetch::Failed;
    if (request.Status() == HTTP_STATUS_NOT_FOUND)
        return Fetch::NotFound;
    if (request.Status() != HTTP_STATUS_OK)
    {
        error = "HTTP " + xr_string(std::to_string(request.Status()).c_str());
        return Fetch::Failed;
    }
    return read_body(request, limit, body, error, stop) ? Fetch::Ok : Fetch::Failed;
}

bool ModHttp::Exchange(Session& session, const wchar_t* method, const std::wstring& url, const std::wstring& headers,
    std::string_view body, size_t limit, xr_string& response, DWORD& status, xr_string& error)
{
    Request request;
    if (!request.Send(session, url, 0, 0, error, method, headers, body))
        return false;
    status = request.Status();
    return read_body(request, limit, response, error, nullptr);
}

std::wstring ModHttp::Widen(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }

xr_string ModHttp::UrlEncode(std::string_view text)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    xr_string out;
    out.reserve(text.size() * 3);
    for (const char c : text)
    {
        const u8 byte = static_cast<u8>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            out += c;
        else
        {
            out += '%';
            out += digits[byte >> 4];
            out += digits[byte & 15];
        }
    }
    return out;
}
#endif
