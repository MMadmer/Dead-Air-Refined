// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "RemoteConfig.h"

#if defined(XR_PLATFORM_WINDOWS) && __has_include("RemoteConfigSecrets.local.h")
#include "RemoteConfigSecrets.local.h"
#include "xrCore/ProductVersion.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
using RemoteConfig::Document;

constexpr std::array<pcstr, size_t(Document::Count)> names = { "menu", "services" };
constexpr wchar_t Host[] = L"raw.githubusercontent.com";
constexpr wchar_t BasePath[] = L"/MMadmer/Dead-Air-Refined/remote/";
constexpr size_t Limit = 1024 * 1024;

// "DARC", the format, the document, two zero bytes - the authenticated data -, then the nonce, the tag
// and the AES-256-GCM ciphertext.
constexpr size_t HeaderSize = 8;
constexpr size_t NonceSize = 12;
constexpr size_t TagSize = 16;
constexpr u8 Format = 1;

struct Slot
{
    xr_string file; // as on GitHub and on the disk
    xr_string text;
    u32 revision{};
    string_path cache{}; // resolved on the main thread: the file system's paths are not the worker's to read
};

struct State
{
    ~State() { stop_and_join(); }

    void stop_and_join()
    {
        stop.store(true, std::memory_order_release);
        close_net();
        if (worker.joinable())
            worker.join();
    }

    // Closing a handle is WinHTTP's way to cancel the call blocked on it: the game does not wait for GitHub
    // when it closes.
    void close_net()
    {
        std::lock_guard guard(net_lock);
        for (HINTERNET* handle : { &request, &connection, &session })
        {
            if (*handle)
                WinHttpCloseHandle(*handle);
            *handle = nullptr;
        }
    }

    std::mutex lock;
    std::array<Slot, size_t(Document::Count)> slots;
    std::atomic_bool stop{};
    std::thread worker;
    bool started{};

    std::mutex net_lock;
    HINTERNET session{};
    HINTERNET connection{};
    HINTERNET request{};
};

State& state()
{
    static State instance;
    return instance;
}

bool decrypt(std::string_view file, Document document, xr_string& text)
{
    if (file.size() < HeaderSize + NonceSize + TagSize || memcmp(file.data(), "DARC", 4) || u8(file[4]) != Format ||
        u8(file[5]) != u8(document))
        return false;

    u8 key[32];
    RemoteConfigSecrets::Key(key);

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE handle{};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, key, sizeof(key), 0));
    SecureZeroMemory(key, sizeof(key));

    if (ok)
    {
        const auto bytes = reinterpret_cast<PUCHAR>(const_cast<char*>(file.data()));
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbAuthData = bytes;
        info.cbAuthData = HeaderSize;
        info.pbNonce = bytes + HeaderSize;
        info.cbNonce = NonceSize;
        info.pbTag = bytes + HeaderSize + NonceSize;
        info.cbTag = TagSize;

        constexpr size_t body = HeaderSize + NonceSize + TagSize;
        text.resize(file.size() - body);
        ULONG written = 0;
        ok = BCRYPT_SUCCESS(BCryptDecrypt(handle, bytes + body, ULONG(file.size() - body), &info, nullptr, 0,
                 reinterpret_cast<PUCHAR>(text.data()), ULONG(text.size()), &written, 0)) &&
            written == text.size();
    }
    if (handle)
        BCryptDestroyKey(handle);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        text.clear();
    return ok;
}

bool read_cache(pcstr path, xr_string& file)
{
    const HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) && size.QuadPart > 0 && size.QuadPart <= LONGLONG(Limit);
    if (ok)
    {
        file.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(handle, file.data(), DWORD(file.size()), &read, nullptr) && read == file.size();
    }
    CloseHandle(handle);
    return ok;
}

// through a file of its own and a rename: a crash on the way leaves the old copy whole
bool write_cache(pcstr path, const xr_string& file)
{
    string_path part;
    xr_sprintf(part, "%s.part", path);
    const HANDLE handle = CreateFileA(part, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(handle, file.data(), DWORD(file.size()), &written, nullptr) && written == file.size();
    CloseHandle(handle);
    return ok && MoveFileExA(part, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void take(Document document, xr_string file, xr_string text)
{
    State& s = state();
    std::lock_guard guard(s.lock);
    Slot& slot = s.slots[size_t(document)];
    slot.file = std::move(file);
    slot.text = std::move(text);
    ++slot.revision;
}

enum class Fetch
{
    Ok,
    NotFound,
    Failed
};

// One file of the branch, whole; the handles lie in State while the calls block, for Stop to close.
Fetch fetch(State& s, pcstr name, xr_string& body, xr_string& error)
{
    std::wstring path = BasePath;
    for (pcstr c = name; *c; ++c)
        path += wchar_t(*c);
    path += L".dar";

    HINTERNET request{};
    {
        std::lock_guard guard(s.net_lock);
        if (s.stop.load(std::memory_order_acquire))
            return Fetch::Failed;
        if (!s.session)
        {
            const std::wstring agent = L"Dead Air Refined/" + std::wstring(DeadAirRefined::VersionWide);
            s.session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS, 0);
            // two small files: short waits
            if (s.session)
                WinHttpSetTimeouts(s.session, 3000, 3000, 5000, 5000);
        }
        if (s.session && !s.connection)
            s.connection = WinHttpConnect(s.session, Host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (s.connection)
            s.request = WinHttpOpenRequest(s.connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        request = s.request;
    }
    const auto done = [&s](Fetch result)
    {
        std::lock_guard guard(s.net_lock);
        if (s.request)
            WinHttpCloseHandle(s.request);
        s.request = nullptr;
        return result;
    };
    if (!request)
    {
        error = "no connection";
        return done(Fetch::Failed);
    }

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX))
    {
        error = "network error " + xr_string(std::to_string(GetLastError()).c_str());
        return done(Fetch::Failed);
    }
    if (status == HTTP_STATUS_NOT_FOUND)
        return done(Fetch::NotFound);
    if (status != HTTP_STATUS_OK)
    {
        error = "HTTP " + xr_string(std::to_string(status).c_str());
        return done(Fetch::Failed);
    }

    char buffer[16 * 1024];
    for (;;)
    {
        DWORD received = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &received))
        {
            error = "the download was interrupted";
            return done(Fetch::Failed);
        }
        if (!received)
            return done(Fetch::Ok);
        body.append(buffer, received);
        if (body.size() > Limit)
        {
            error = "the file is larger than allowed";
            return done(Fetch::Failed);
        }
    }
}

void fetch_all()
{
    State& s = state();
    for (size_t i = 0; i < names.size() && !s.stop.load(std::memory_order_acquire); ++i)
    {
        const Document document = Document(i);
        const pcstr name = names[i];
        string_path cache;
        xr_string file, error;
        {
            std::lock_guard guard(s.lock);
            xr_strcpy(cache, s.slots[i].cache);
        }
        switch (fetch(s, name, file, error))
        {
        case Fetch::Ok:
        {
            {
                std::lock_guard guard(s.lock);
                if (s.slots[i].file == file)
                {
                    Msg("* [remote] %s: the kept copy is the newest", name);
                    continue;
                }
            }
            xr_string text;
            if (!decrypt(file, document, text))
            {
                Msg("! [remote] %s: GitHub's copy does not open, the kept one serves", name);
                continue;
            }
            if (!write_cache(cache, file))
                Msg("! [remote] %s: could not keep the new copy", name);
            Msg("* [remote] %s: a newer copy from GitHub, %u byte(s)", name, u32(text.size()));
            take(document, std::move(file), std::move(text));
            break;
        }
        case Fetch::NotFound:
            // The panel goes when the repository no longer offers it; an address stays as the last one known.
            if (document == Document::Menu)
            {
                DeleteFileA(cache);
                take(document, {}, {});
                Msg("* [remote] %s: no longer offered", name);
            }
            break;
        case Fetch::Failed:
            if (!s.stop.load(std::memory_order_acquire))
                Msg("~ [remote] %s: GitHub out of reach (%s), the kept copy serves", name, error.c_str());
            break;
        }
    }
    s.close_net();
}
} // namespace

void RemoteConfig::Start()
{
    State& s = state();
    if (s.started)
        return;
    s.started = true;

    for (size_t i = 0; i < names.size(); ++i)
    {
        Slot& slot = s.slots[i];
        string_path name;
        xr_sprintf(name, "remote\\%s.dar", names[i]);
        FS.update_path(slot.cache, "$app_data_root$", name);
        VerifyPath(slot.cache);

        xr_string file, text;
        if (!read_cache(slot.cache, file))
            continue;
        if (decrypt(file, Document(i), text))
        {
            Msg("* [remote] %s: the kept copy, %u byte(s)", names[i], u32(text.size()));
            take(Document(i), std::move(file), std::move(text));
        }
        else
        {
            Msg("! [remote] %s: the kept copy does not open, dropped", names[i]);
            DeleteFileA(slot.cache);
        }
    }
    s.worker = std::thread(fetch_all);
}

void RemoteConfig::Stop() { state().stop_and_join(); }

u32 RemoteConfig::Revision(Document document)
{
    State& s = state();
    std::lock_guard guard(s.lock);
    return s.slots[size_t(document)].revision;
}

xr_string RemoteConfig::Text(Document document, u32* revision)
{
    State& s = state();
    std::lock_guard guard(s.lock);
    const Slot& slot = s.slots[size_t(document)];
    if (revision)
        *revision = slot.revision;
    return slot.text;
}

xr_string RemoteConfig::Service(pcstr key)
{
    const xr_string text = Text(Document::Services);
    const size_t length = xr_strlen(key);
    const auto trim = [](std::string_view v)
    {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
            v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
            v.remove_suffix(1);
        return v;
    };
    size_t begin = 0;
    while (begin < text.size())
    {
        size_t end = text.find('\n', begin);
        if (end == xr_string::npos)
            end = text.size();
        std::string_view line = trim(std::string_view(text.data() + begin, end - begin));
        begin = end + 1;

        const size_t equals = line.find('=');
        if (line.empty() || line.front() == ';' || line.front() == '#' || equals == std::string_view::npos)
            continue;
        if (trim(line.substr(0, equals)) == std::string_view(key, length))
        {
            const std::string_view value = trim(line.substr(equals + 1));
            return xr_string(value.data(), value.size());
        }
    }
    return {};
}
#else
void RemoteConfig::Start() {}
void RemoteConfig::Stop() {}
u32 RemoteConfig::Revision(Document) { return 0; }
xr_string RemoteConfig::Text(Document, u32* revision)
{
    if (revision)
        *revision = 0;
    return {};
}
xr_string RemoteConfig::Service(pcstr) { return {}; }
#endif
