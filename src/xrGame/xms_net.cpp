// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"

#include "xms_net.h"

#include "ui/ModHttp.h"
#include "xrCore/ProductVersion.h"
#include "xrCore/XMS/xms_catalog.h"
#include "xrCore/XMS/xms_netmark.h"
#include "xrScriptEngine/script_engine.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
constexpr size_t kMaxAddress = 2048;
constexpr size_t kMaxBody = 256 * 1024;       // what a script sends
constexpr size_t kMaxAnswer = 4 * 1024 * 1024; // what it gets back
constexpr size_t kMaxHeaders = 16;
constexpr size_t kMaxHeaderValue = 1024;
constexpr u32 kMaxFlying = 4;   // requests on the wire at once
constexpr u32 kMaxWaiting = 32; // and queued behind them
constexpr u32 kDefaultSeconds = 20;
constexpr u32 kMaxSeconds = 60;

struct Job
{
    u32 id{};
    std::wstring url;
    std::wstring method;
    std::wstring headers;
    xr_string body;
    u32 timeoutMs{};
    std::atomic_bool cancelled{false};

    bool ok{};
    u32 status{};
    xr_string answer;
    xr_string error;
    xr_vector<std::pair<xr_string, xr_string>> answerHeaders;
};

struct Service
{
    std::mutex lock;
    std::deque<std::shared_ptr<Job>> waiting;
    xr_vector<std::shared_ptr<Job>> flying;
    xr_vector<std::shared_ptr<Job>> done;
    u32 nextId{1};
};

// Never destroyed: a request may still be on the wire when the process ends.
Service& service()
{
    static Service* instance = new Service;
    return *instance;
}

struct Handle
{
    HINTERNET h{};
    ~Handle()
    {
        if (h)
            WinHttpCloseHandle(h);
    }
};

xr_string narrow(const wchar_t* text, size_t length)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, int(length), nullptr, 0, nullptr, nullptr);
    xr_string out(size_t(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, int(length), out.data(), size, nullptr, nullptr);
    return out;
}

// "Name: value" lines of the answer, names in lower case.
void read_headers(HINTERNET request, xr_vector<std::pair<xr_string, xr_string>>& out)
{
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes,
        WINHTTP_NO_HEADER_INDEX);
    if (!bytes || bytes > 64 * 1024)
        return;
    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &bytes,
            WINHTTP_NO_HEADER_INDEX))
        return;
    const xr_string text = narrow(raw.data(), bytes / sizeof(wchar_t));
    size_t start = text.find("\r\n"); // past the status line
    while (start != xr_string::npos && out.size() < 64)
    {
        start += 2;
        const size_t end = text.find("\r\n", start);
        const std::string_view line(text.data() + start, (end == xr_string::npos ? text.size() : end) - start);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos && colon)
        {
            xr_string name(line.substr(0, colon));
            for (char& c : name)
                c = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
            size_t value = colon + 1;
            while (value < line.size() && line[value] == ' ')
                ++value;
            out.emplace_back(std::move(name), xr_string(line.substr(value)));
        }
        start = end;
    }
}

void run(Job& job)
{
    const std::wstring agent = L"Dead Air Refined/" + std::wstring(DeadAirRefined::VersionWide) + L" (module script)";
    Handle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0)};
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    // plain http is the catalog's QA alone (-qa_catalog): its mock on this computer, like its mirrors
    const bool cracked = session.h && WinHttpCrackUrl(job.url.c_str(), static_cast<DWORD>(job.url.size()), 0, &parts);
    const bool secure = cracked && parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!cracked || (!secure && !XMS::Catalog::QaMode()))
    {
        job.error = "the address cannot be asked";
        return;
    }
    WinHttpSetTimeouts(session.h, 5000, 10000, int(job.timeoutMs), int(job.timeoutMs));
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty())
        path = L"/";

    Handle connection{WinHttpConnect(session.h, host.c_str(), parts.nPort, 0)};
    Handle request{connection.h ? WinHttpOpenRequest(connection.h, job.method.c_str(), path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) :
                                  nullptr};
    if (!request.h)
    {
        job.error = "could not connect";
        return;
    }
    // The player's Windows sign-in never answers a server; a redirect is the script's to follow,
    // through the same rule as any address; nothing is remembered between requests.
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(request.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
    DWORD off = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
    if (!job.headers.empty())
        WinHttpAddRequestHeaders(request.h, job.headers.c_str(), static_cast<DWORD>(job.headers.size()),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    LPVOID data = job.body.empty() ? WINHTTP_NO_REQUEST_DATA : job.body.data();
    const DWORD size = static_cast<DWORD>(job.body.size());
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (job.cancelled.load(std::memory_order_acquire) ||
        !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, size, size, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr) ||
        !WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        job.error = "the request failed";
        return;
    }
    job.status = status;
    read_headers(request.h, job.answerHeaders);
    xr_vector<char> buffer(64 * 1024);
    while (!job.cancelled.load(std::memory_order_acquire))
    {
        DWORD received = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &received))
        {
            job.error = "the answer was interrupted";
            return;
        }
        if (!received)
        {
            job.ok = status >= 200 && status < 300;
            return;
        }
        if (job.answer.size() + received > kMaxAnswer)
        {
            job.answer.clear();
            job.error = "the answer is larger than 4 MB";
            return;
        }
        job.answer.append(buffer.data(), received);
    }
    job.error = "cancelled";
}

void start_next_locked(Service& s);

void fly(std::shared_ptr<Job> job)
{
    std::thread([job] {
        run(*job);
        Service& s = service();
        std::lock_guard guard(s.lock);
        std::erase(s.flying, job);
        s.done.push_back(job);
        start_next_locked(s);
    }).detach();
}

void start_next_locked(Service& s)
{
    while (s.flying.size() < kMaxFlying && !s.waiting.empty())
    {
        std::shared_ptr<Job> job = s.waiting.front();
        s.waiting.pop_front();
        s.flying.push_back(job);
        fly(job);
    }
}

bool token_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

bool iequal(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
    });
}

// The lines a script gives are its own headers and nothing of the connection's.
bool own_headers(std::string_view text, xr_string& why)
{
    size_t count = 0;
    while (!text.empty())
    {
        const size_t end = text.find("\r\n");
        if (end == std::string_view::npos)
        {
            why = "a header is one line";
            return false;
        }
        const std::string_view line = text.substr(0, end);
        text.remove_prefix(end + 2);
        const size_t colon = line.find(": ");
        const std::string_view name = line.substr(0, colon);
        if (colon == std::string_view::npos || name.empty() || !std::all_of(name.begin(), name.end(), token_char))
        {
            why = "a header's name is letters, digits and '-'";
            return false;
        }
        const std::string_view value = line.substr(colon + 2);
        if (value.size() > kMaxHeaderValue || std::any_of(value.begin(), value.end(), [](char c) {
                return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f;
            }))
        {
            why = "a header's value is one line of at most 1024 plain characters";
            return false;
        }
        for (const std::string_view fixed : {"host", "content-length", "transfer-encoding", "connection", "upgrade",
                 "expect", "te", "trailer", "cookie", "user-agent", "proxy-authorization", "proxy-connection"})
            if (iequal(name, fixed))
            {
                why = xr_string("the header ") + xr_string(name) + " is the game's";
                return false;
            }
        if (++count > kMaxHeaders)
        {
            why = "at most 16 headers";
            return false;
        }
    }
    return true;
}

int refuse(lua_State* L, pcstr why)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

// (url, method, headers, body, seconds) -> id | nil, why
int native_request(lua_State* L)
{
    size_t bodySize = 0, headerSize = 0;
    const pcstr url = luaL_checkstring(L, 1);
    const pcstr method = luaL_optstring(L, 2, "GET");
    const pcstr headers = luaL_optlstring(L, 3, "", &headerSize);
    const pcstr body = luaL_optlstring(L, 4, "", &bodySize);
    const lua_Number asked = luaL_optnumber(L, 5, 0);

    // Decided in a helper: no object of this frame is alive when the answer is pushed.
    struct Decision
    {
        static bool make(pcstr url, pcstr method, std::string_view headers, std::string_view body, double asked, u32& id,
            string256& why)
        {
            xr_string reason;
            const auto no = [&](const xr_string& text) {
                strncpy_s(why, sizeof(why), text.c_str(), _TRUNCATE);
                return false;
            };
            if (!XMS::NetMark::AnyEnabledModuleUsesNetwork())
                return no("no enabled module carries the network mark");
            if (!XmsNet::PublicAddress(url, reason))
                return no(reason);
            bool known = false;
            for (const std::string_view verb : {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"})
                known = known || verb == method;
            if (!known)
                return no("the method is GET, POST, PUT, PATCH, DELETE or HEAD");
            if (!own_headers(headers, reason))
                return no(reason);
            if (body.size() > kMaxBody)
                return no("the body is larger than 256 KB");

            auto job = std::make_shared<Job>();
            job->url = ModHttp::Widen(url);
            job->method = ModHttp::Widen(method);
            job->headers = ModHttp::Widen(headers);
            job->body.assign(body.data(), body.size());
            const double seconds = asked > 0 ? std::min(asked, double(kMaxSeconds)) : double(kDefaultSeconds);
            job->timeoutMs = u32(std::max(seconds, 1.0) * 1000.0);

            Service& s = service();
            std::lock_guard guard(s.lock);
            if (s.waiting.size() >= kMaxWaiting)
                return no("too many requests are waiting");
            job->id = id = s.nextId++;
            s.waiting.push_back(std::move(job));
            start_next_locked(s);
            return true;
        }
    };
    u32 id = 0;
    string256 why{};
    if (!Decision::make(url, method, std::string_view(headers, headerSize), std::string_view(body, bodySize), asked, id, why))
        return refuse(L, why);
    lua_pushnumber(L, id);
    return 1;
}

// (id): a request nobody waits for any more
int native_cancel(lua_State* L)
{
    const u32 id = u32(luaL_checknumber(L, 1));
    Service& s = service();
    std::lock_guard guard(s.lock);
    std::erase_if(s.waiting, [id](const std::shared_ptr<Job>& job) { return job->id == id; });
    for (const std::shared_ptr<Job>& job : s.flying)
        if (job->id == id)
            job->cancelled.store(true, std::memory_order_release);
    for (const std::shared_ptr<Job>& job : s.done)
        if (job->id == id)
            job->cancelled.store(true, std::memory_order_release);
    return 0;
}

// () -> true | false, why
int native_allowed(lua_State* L)
{
    if (XMS::NetMark::AnyEnabledModuleUsesNetwork())
    {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "no enabled module carries the network mark");
    return 2;
}
} // namespace

bool XmsNet::PublicAddress(pcstr url, xr_string& why)
{
    const std::string_view text(url ? url : "");
    // the catalog's QA asks its own mock on this computer
    if (XMS::Catalog::QaMode() && text.starts_with("http://127.0.0.1:") && text.size() <= kMaxAddress &&
        XMS::Protect::WebAddress(url))
        return true;
    if (!text.starts_with("https://") || text.size() > kMaxAddress || !XMS::Protect::WebAddress(url))
    {
        why = "the address is https:// and at most 2048 characters";
        return false;
    }
    std::string_view authority = text.substr(8);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (authority.find('@') != std::string_view::npos)
    {
        why = "an address carries no user name";
        return false;
    }
    std::string_view host = authority;
    if (const size_t colon = authority.find(':'); colon != std::string_view::npos)
    {
        host = authority.substr(0, colon);
        const std::string_view port = authority.substr(colon + 1);
        u32 number = 0;
        for (const char c : port)
            number = c >= '0' && c <= '9' && number < 100000 ? number * 10 + u32(c - '0') : 100000;
        if (port.empty() || (number != 443 && (number < 1024 || number > 65535)))
        {
            why = "the port is 443 or from 1024 to 65535";
            return false;
        }
    }
    // A public server by its name: not this computer, not a device of the home network by its
    // number, not a name only the local network knows.
    const size_t dot = host.rfind('.');
    const std::string_view last = dot == std::string_view::npos ? std::string_view() : host.substr(dot + 1);
    const bool named = !host.empty() && dot != std::string_view::npos && dot && !last.empty() && host.front() != '.' &&
        host.front() != '-' && host.find("..") == std::string_view::npos &&
        std::all_of(host.begin(), host.end(), [](char c) { return token_char(c) || c == '.'; }) &&
        std::all_of(last.begin(), last.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
    if (!named)
    {
        why = "the host is a public server's name";
        return false;
    }
    for (const std::string_view local : {"local", "localhost", "internal", "lan", "home", "arpa", "test", "invalid"})
        if (iequal(last, local))
        {
            why = "the host is a public server's name";
            return false;
        }
    return true;
}

void XmsNet::Register(lua_State* L)
{
    lua_register(L, "xms_native_http_request", native_request);
    lua_register(L, "xms_native_http_cancel", native_cancel);
    lua_register(L, "xms_native_http_allowed", native_allowed);
}

void XmsNet::Pump()
{
    Service& s = service();
    xr_vector<std::shared_ptr<Job>> done;
    {
        std::lock_guard guard(s.lock);
        if (s.done.empty())
            return;
        done.swap(s.done);
    }
    lua_State* L = GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr;
    if (!L)
        return;
    for (const std::shared_ptr<Job>& job : done)
    {
        if (job->cancelled.load(std::memory_order_acquire))
            continue;
        const int top = lua_gettop(L);
        lua_getglobal(L, "xms");
        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "_http_done");
            if (lua_isfunction(L, -1))
            {
                lua_pushnumber(L, job->id);
                lua_pushboolean(L, job->ok);
                lua_pushnumber(L, job->status);
                lua_pushlstring(L, job->answer.data(), job->answer.size());
                if (job->error.empty())
                    lua_pushnil(L);
                else
                    lua_pushstring(L, job->error.c_str());
                lua_newtable(L);
                for (const auto& [name, value] : job->answerHeaders)
                {
                    lua_pushlstring(L, value.data(), value.size());
                    lua_setfield(L, -2, name.c_str());
                }
                if (lua_pcall(L, 6, 0, 0) != 0)
                    Msg("! XMS: a script failed on the answer to its request: %s", lua_tostring(L, -1));
            }
        }
        lua_settop(L, top);
    }
}
