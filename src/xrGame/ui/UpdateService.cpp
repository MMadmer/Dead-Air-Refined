#include "StdAfx.h"
#include "UpdateService.h"

#include "xrCore/ProductVersion.h"
#include "xrEngine/Engine.h"

#ifdef XR_PLATFORM_WINDOWS
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace
{
constexpr std::wstring_view ReleasesUrl =
    L"https://api.github.com/repos/MMadmer/Dead-Air-Refined/releases?per_page=30";
constexpr pcstr UpdateAssetPrefix = "Dead-Air-Refined-";
constexpr pcstr UpdateAssetSuffix = "-Update.zip";
constexpr size_t MaximumApiResponseBytes = 4 * 1024 * 1024;

struct SemanticVersion
{
    u32 major{};
    u32 minor{};
    u32 patch{};

    auto operator<=>(const SemanticVersion&) const = default;
};

struct ReleaseAsset
{
    xr_string name;
    xr_string url;
    xr_string digest;
    u64 size{};
};

struct Release
{
    xr_string tag;
    xr_string body;
    bool draft{};
    bool prerelease{};
    xr_vector<ReleaseAsset> assets;
};

struct ServiceState
{
    ~ServiceState()
    {
        if (worker.joinable())
            worker.join();
    }

    std::atomic<UpdateService::State> state{UpdateService::State::Idle};
    std::atomic<u64> downloadedBytes{};
    std::atomic<u64> totalBytes{};
    std::atomic_bool started{};
    std::atomic_bool stopRequested{};
    std::mutex dataMutex;
    xr_string version;
    xr_string message;
    xr_string changesEn;
    xr_string changesRu;
    xr_string downloadUrl;
    xr_string digest;
    std::filesystem::path archivePath;
    std::thread worker;
};

ServiceState& service()
{
    static ServiceState instance;
    return instance;
}

class JsonReader
{
public:
    explicit JsonReader(std::string_view source) : m_source(source) {}

    bool Consume(char expected)
    {
        SkipWhitespace();
        if (m_offset == m_source.size() || m_source[m_offset] != expected)
            return false;
        ++m_offset;
        return true;
    }

    bool Peek(char expected)
    {
        SkipWhitespace();
        return m_offset != m_source.size() && m_source[m_offset] == expected;
    }

    bool AtEnd()
    {
        SkipWhitespace();
        return m_offset == m_source.size();
    }

    bool ReadString(xr_string& result)
    {
        SkipWhitespace();
        if (m_offset == m_source.size() || m_source[m_offset++] != '"')
            return false;

        result.clear();
        while (m_offset != m_source.size())
        {
            const unsigned char character = m_source[m_offset++];
            if (character == '"')
                return true;
            if (character != '\\')
            {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (m_offset == m_source.size())
                return false;

            const char escaped = m_source[m_offset++];
            switch (escaped)
            {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u':
            {
                u32 value = 0;
                if (m_source.size() - m_offset < 4)
                    return false;
                for (u32 index = 0; index != 4; ++index)
                {
                    const char digit = m_source[m_offset++];
                    value <<= 4;
                    if (digit >= '0' && digit <= '9')
                        value |= digit - '0';
                    else if (digit >= 'a' && digit <= 'f')
                        value |= digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F')
                        value |= digit - 'A' + 10;
                    else
                        return false;
                }
                AppendUtf8(result, value);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ReadBool(bool& result)
    {
        SkipWhitespace();
        if (m_source.substr(m_offset, 4) == "true")
        {
            m_offset += 4;
            result = true;
            return true;
        }
        if (m_source.substr(m_offset, 5) == "false")
        {
            m_offset += 5;
            result = false;
            return true;
        }
        return false;
    }

    bool ReadUnsigned(u64& result)
    {
        SkipWhitespace();
        const char* begin = m_source.data() + m_offset;
        const char* end = m_source.data() + m_source.size();
        const auto [parsedEnd, error] = std::from_chars(begin, end, result);
        if (error != std::errc{} || parsedEnd == begin)
            return false;
        m_offset = static_cast<size_t>(parsedEnd - m_source.data());
        return true;
    }

    bool SkipValue()
    {
        SkipWhitespace();
        if (m_offset == m_source.size())
            return false;

        if (m_source[m_offset] == '"')
        {
            xr_string ignored;
            return ReadString(ignored);
        }
        if (m_source[m_offset] == '{')
            return SkipContainer('{', '}');
        if (m_source[m_offset] == '[')
            return SkipContainer('[', ']');

        const size_t start = m_offset;
        while (m_offset != m_source.size() && !strchr(",}] \t\r\n", m_source[m_offset]))
            ++m_offset;
        return m_offset != start;
    }

private:
    std::string_view m_source;
    size_t m_offset{};

    void SkipWhitespace()
    {
        while (m_offset != m_source.size() &&
            std::isspace(static_cast<unsigned char>(m_source[m_offset])))
        {
            ++m_offset;
        }
    }

    static void AppendUtf8(xr_string& output, u32 value)
    {
        if (value <= 0x7f)
            output.push_back(static_cast<char>(value));
        else if (value <= 0x7ff)
        {
            output.push_back(static_cast<char>(0xc0 | value >> 6));
            output.push_back(static_cast<char>(0x80 | value & 0x3f));
        }
        else
        {
            output.push_back(static_cast<char>(0xe0 | value >> 12));
            output.push_back(static_cast<char>(0x80 | value >> 6 & 0x3f));
            output.push_back(static_cast<char>(0x80 | value & 0x3f));
        }
    }

    bool SkipContainer(char opening, char closing)
    {
        if (!Consume(opening))
            return false;

        while (true)
        {
            SkipWhitespace();
            if (m_offset == m_source.size())
                return false;
            if (m_source[m_offset] == closing)
            {
                ++m_offset;
                return true;
            }

            if (opening == '{')
            {
                xr_string key;
                if (!ReadString(key) || !Consume(':'))
                    return false;
            }
            if (!SkipValue())
                return false;

            SkipWhitespace();
            if (m_offset != m_source.size() && m_source[m_offset] == ',')
            {
                ++m_offset;
                continue;
            }
            if (m_offset != m_source.size() && m_source[m_offset] == closing)
            {
                ++m_offset;
                return true;
            }
            return false;
        }
    }
};

bool parse_asset(JsonReader& reader, ReleaseAsset& asset)
{
    if (!reader.Consume('{'))
        return false;
    while (!reader.Peek('}'))
    {
        xr_string key;
        if (!reader.ReadString(key) || !reader.Consume(':'))
            return false;
        if (key == "name")
        {
            if (!reader.ReadString(asset.name))
                return false;
        }
        else if (key == "browser_download_url")
        {
            if (!reader.ReadString(asset.url))
                return false;
        }
        else if (key == "digest")
        {
            if (!reader.ReadString(asset.digest))
                return false;
        }
        else if (key == "size")
        {
            if (!reader.ReadUnsigned(asset.size))
                return false;
        }
        else if (!reader.SkipValue())
            return false;

        if (reader.Peek(','))
        {
            reader.Consume(',');
            continue;
        }
        break;
    }
    return reader.Consume('}');
}

bool parse_assets(JsonReader& reader, xr_vector<ReleaseAsset>& assets)
{
    if (!reader.Consume('['))
        return false;
    while (!reader.Peek(']'))
    {
        ReleaseAsset asset;
        if (!parse_asset(reader, asset))
            return false;
        assets.push_back(std::move(asset));
        if (reader.Peek(','))
        {
            reader.Consume(',');
            continue;
        }
        break;
    }
    return reader.Consume(']');
}

bool parse_release(JsonReader& reader, Release& release)
{
    if (!reader.Consume('{'))
        return false;
    while (!reader.Peek('}'))
    {
        xr_string key;
        if (!reader.ReadString(key) || !reader.Consume(':'))
            return false;
        if (key == "tag_name")
        {
            if (!reader.ReadString(release.tag))
                return false;
        }
        else if (key == "body")
        {
            if (reader.Peek('"'))
            {
                if (!reader.ReadString(release.body))
                    return false;
            }
            else if (!reader.SkipValue())
                return false;
        }
        else if (key == "draft")
        {
            if (!reader.ReadBool(release.draft))
                return false;
        }
        else if (key == "prerelease")
        {
            if (!reader.ReadBool(release.prerelease))
                return false;
        }
        else if (key == "assets")
        {
            if (!parse_assets(reader, release.assets))
                return false;
        }
        else if (!reader.SkipValue())
            return false;

        if (reader.Peek(','))
        {
            reader.Consume(',');
            continue;
        }
        break;
    }
    return reader.Consume('}');
}

bool parse_releases(std::string_view source, xr_vector<Release>& releases)
{
    JsonReader reader(source);
    if (!reader.Consume('['))
        return false;
    while (!reader.Peek(']'))
    {
        Release release;
        if (!parse_release(reader, release))
            return false;
        releases.push_back(std::move(release));
        if (reader.Peek(','))
        {
            reader.Consume(',');
            continue;
        }
        break;
    }
    return reader.Consume(']') && reader.AtEnd();
}

std::optional<SemanticVersion> parse_version(std::string_view value)
{
    SemanticVersion version;
    std::array<u32*, 3> components{&version.major, &version.minor, &version.patch};
    for (size_t index = 0; index != components.size(); ++index)
    {
        const size_t separator = value.find('.');
        const std::string_view component = separator == std::string_view::npos ? value : value.substr(0, separator);
        if (component.empty())
            return std::nullopt;
        const auto [end, error] = std::from_chars(
            component.data(), component.data() + component.size(), *components[index]);
        if (error != std::errc{} || end != component.data() + component.size())
            return std::nullopt;
        if (index + 1 == components.size())
        {
            if (separator != std::string_view::npos)
                return std::nullopt;
        }
        else
        {
            if (separator == std::string_view::npos)
                return std::nullopt;
            value.remove_prefix(separator + 1);
        }
    }
    return version;
}

std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

xr_string wide_to_utf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    xr_string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::filesystem::path game_directory()
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
        return {};
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

std::wstring qa_api_url()
{
    if (!Core.Params || !strstr(Core.Params, "-qa_update"))
        return {};

    std::array<wchar_t, 2048> value{};
    const DWORD length = GetEnvironmentVariableW(L"DAR_QA_UPDATE_API", value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size())
        return {};
    std::wstring result(value.data(), length);
    if (!result.starts_with(L"http://127.0.0.1:") && !result.starts_with(L"http://localhost:"))
        return {};
    return result;
}

class HttpRequest
{
public:
    ~HttpRequest()
    {
        if (request)
            WinHttpCloseHandle(request);
        if (connection)
            WinHttpCloseHandle(connection);
        if (session)
            WinHttpCloseHandle(session);
    }

    bool Open(std::wstring_view url, xr_string& error)
    {
        const std::wstring userAgent = L"Dead Air Refined/" + std::wstring(DeadAirRefined::VersionWide);
        session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
        {
            error = "WinHTTP initialization failed";
            return false;
        }
        WinHttpSetTimeouts(session, 5000, 10000, 15000, 30000);

        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &components))
        {
            error = "Invalid update URL";
            return false;
        }

        std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength)
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

        connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
        const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
        if (!connection || !request)
        {
            error = "Could not connect to GitHub";
            return false;
        }

        constexpr wchar_t headers[] =
            L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
        {
            error = "Update request failed";
            return false;
        }

        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
        {
            error = "Update response did not contain a status code";
            return false;
        }
        return true;
    }

    HINTERNET session{};
    HINTERNET connection{};
    HINTERNET request{};
    DWORD status{};
};

bool read_response(HttpRequest& request, xr_string& body, size_t maximumBytes, xr_string& error)
{
    if (request.status != HTTP_STATUS_OK)
    {
        char message[96]{};
        xr_sprintf(message, sizeof(message), "GitHub returned HTTP %u", request.status);
        error = message;
        return false;
    }

    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request.request, &available) && available)
    {
        if (body.size() + available > maximumBytes)
        {
            error = "GitHub response exceeded the size limit";
            return false;
        }
        const size_t previous = body.size();
        body.resize(previous + available);
        DWORD received = 0;
        if (!WinHttpReadData(request.request, body.data() + previous, available, &received))
        {
            error = "Could not read the GitHub response";
            return false;
        }
        body.resize(previous + received);
    }
    return true;
}

bool query_releases(xr_vector<Release>& releases, xr_string& error)
{
    std::wstring url = qa_api_url();
    if (url.empty())
        url = ReleasesUrl;

    HttpRequest request;
    if (!request.Open(url, error))
        return false;

    xr_string body;
    if (!read_response(request, body, MaximumApiResponseBytes, error))
        return false;
    if (!parse_releases(body, releases))
    {
        error = "GitHub returned an invalid release list";
        return false;
    }
    return true;
}

bool valid_digest(std::string_view digest)
{
    constexpr std::string_view prefix = "sha256:";
    if (!digest.starts_with(prefix) || digest.size() != prefix.size() + 64)
        return false;
    return std::ranges::all_of(digest.substr(prefix.size()), [](unsigned char character)
    {
        return std::isxdigit(character) != 0;
    });
}

std::string_view trim_markdown_line(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        line.remove_suffix(1);
    return line;
}

xr_string extract_release_changes(
    std::string_view body, std::string_view languageHeading, std::string_view changesHeading)
{
    enum class ParseState
    {
        SeekingLanguage,
        SeekingChanges,
        Collecting
    };

    ParseState state = ParseState::SeekingLanguage;
    xr_string result;
    while (!body.empty())
    {
        const size_t end = body.find('\n');
        const std::string_view line = trim_markdown_line(body.substr(0, end));
        if (end == std::string_view::npos)
            body = {};
        else
            body.remove_prefix(end + 1);

        if (line.starts_with("## "))
        {
            if (state == ParseState::SeekingLanguage)
            {
                if (line == languageHeading)
                    state = ParseState::SeekingChanges;
            }
            else if (state == ParseState::SeekingChanges && line == changesHeading)
                state = ParseState::Collecting;
            else
                break;
            continue;
        }

        if (state != ParseState::Collecting || line.size() < 3 ||
            !((line[0] == '*' || line[0] == '-') && line[1] == ' '))
        {
            continue;
        }

        constexpr size_t MaximumChangelogBytes = 32 * 1024;
        const std::string_view item = trim_markdown_line(line.substr(2));
        const size_t extraBytes = item.size() + (result.empty() ? 2 : 3);
        if (item.empty() || result.size() + extraBytes > MaximumChangelogBytes)
            continue;
        if (!result.empty())
            result.push_back('\n');
        result.append("- ");
        result.append(item.data(), item.size());
    }
    return result;
}

xr_string utf8_to_windows_1251(std::string_view value)
{
    const std::wstring wide = utf8_to_wide(value);
    if (wide.empty())
        return {};

    const int length = WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, "?", nullptr);
    if (length <= 0)
        return {};

    xr_string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, wide.data(), static_cast<int>(wide.size()),
        result.data(), length, "?", nullptr);
    return result;
}

std::optional<std::pair<Release, ReleaseAsset>> select_update(const xr_vector<Release>& releases, xr_string& error)
{
    const auto current = parse_version(DeadAirRefined::Version);
    if (!current)
    {
        error = "The installed version is not valid SemVer";
        return std::nullopt;
    }

    const Release* selectedRelease = nullptr;
    const ReleaseAsset* selectedAsset = nullptr;
    SemanticVersion selectedVersion = *current;
    for (const Release& release : releases)
    {
        if (release.draft || release.prerelease)
            continue;
        const auto version = parse_version(release.tag);
        if (!version || *version <= selectedVersion)
            continue;

        const xr_string expectedName = xr_string(UpdateAssetPrefix) + release.tag + UpdateAssetSuffix;
        const auto asset = std::ranges::find_if(release.assets, [&](const ReleaseAsset& candidate)
        {
            return candidate.name == expectedName && candidate.size && !candidate.url.empty() &&
                valid_digest(candidate.digest);
        });
        if (asset == release.assets.end())
            continue;

        selectedVersion = *version;
        selectedRelease = &release;
        selectedAsset = &*asset;
    }

    if (!selectedRelease)
        return std::nullopt;
    return std::pair{*selectedRelease, *selectedAsset};
}

void set_state(UpdateService::State state, xr_string message = {})
{
    ServiceState& instance = service();
    {
        std::lock_guard lock(instance.dataMutex);
        instance.message = std::move(message);
    }
    instance.state.store(state, std::memory_order_release);
}

void check_worker()
{
    xr_vector<Release> releases;
    xr_string error;
    if (!query_releases(releases, error))
    {
        Msg("* Update check skipped: %s", error.c_str());
        set_state(UpdateService::State::CheckFailed, std::move(error));
        return;
    }

    if (releases.empty())
    {
        Msg("* Update check completed: no published releases");
        set_state(UpdateService::State::Current);
        return;
    }

    const auto update = select_update(releases, error);
    if (!update)
    {
        Msg("* Update check completed: version %s is current", DeadAirRefined::Version);
        set_state(UpdateService::State::Current);
        return;
    }

    ServiceState& instance = service();
    {
        std::lock_guard lock(instance.dataMutex);
        instance.version = update->first.tag;
        instance.changesEn = extract_release_changes(update->first.body, "## EN", "## Changes");
        instance.changesRu = utf8_to_windows_1251(
            extract_release_changes(update->first.body, "## RU", "## Изменения"));
        instance.downloadUrl = update->second.url;
        instance.digest = update->second.digest;
        instance.message.clear();
    }
    instance.totalBytes.store(update->second.size, std::memory_order_release);
    instance.downloadedBytes.store(0, std::memory_order_release);
    Msg("* Update available: %s -> %s (%llu bytes)", DeadAirRefined::Version,
        update->first.tag.c_str(), static_cast<unsigned long long>(update->second.size));
    Msg("* Update changelog: EN %zu bytes, RU %zu bytes",
        instance.changesEn.size(), instance.changesRu.size());
    instance.state.store(UpdateService::State::Available, std::memory_order_release);
}

class Sha256
{
public:
    ~Sha256()
    {
        if (m_hash)
            BCryptDestroyHash(m_hash);
        if (m_algorithm)
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }

    bool Initialize()
    {
        DWORD bytes = 0;
        if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(m_algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&m_objectLength),
                sizeof(m_objectLength), &bytes, 0) < 0 ||
            BCryptGetProperty(m_algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&m_digestLength),
                sizeof(m_digestLength), &bytes, 0) < 0)
        {
            return false;
        }
        m_object.resize(m_objectLength);
        return BCryptCreateHash(m_algorithm, &m_hash, m_object.data(), m_objectLength, nullptr, 0, 0) >= 0;
    }

    bool Append(const void* data, DWORD size)
    {
        return BCryptHashData(m_hash, static_cast<PUCHAR>(const_cast<void*>(data)), size, 0) >= 0;
    }

    xr_string Finish()
    {
        xr_vector<u8> digest(m_digestLength);
        if (BCryptFinishHash(m_hash, digest.data(), m_digestLength, 0) < 0)
            return {};
        static constexpr char hex[] = "0123456789abcdef";
        xr_string result;
        result.reserve(digest.size() * 2);
        for (const u8 byte : digest)
        {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
        return result;
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm{};
    BCRYPT_HASH_HANDLE m_hash{};
    DWORD m_objectLength{};
    DWORD m_digestLength{};
    xr_vector<u8> m_object;
};

bool download_update(const xr_string& url, const xr_string& digest, u64 expectedSize,
    const std::filesystem::path& destination, xr_string& error)
{
    HttpRequest request;
    if (!request.Open(utf8_to_wide(url), error))
        return false;
    if (request.status != HTTP_STATUS_OK)
    {
        char message[96]{};
        xr_sprintf(message, sizeof(message), "Download returned HTTP %u", request.status);
        error = message;
        return false;
    }

    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError)
    {
        error = "Could not create the update cache";
        return false;
    }

    HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "Could not create the update archive";
        return false;
    }

    Sha256 hash;
    bool success = hash.Initialize();
    u64 total = 0;
    std::array<u8, 64 * 1024> buffer{};
    while (success && !service().stopRequested.load(std::memory_order_acquire))
    {
        DWORD received = 0;
        if (!WinHttpReadData(request.request, buffer.data(), static_cast<DWORD>(buffer.size()), &received))
        {
            error = "The update download was interrupted";
            success = false;
            break;
        }
        if (!received)
            break;

        DWORD written = 0;
        success = WriteFile(file, buffer.data(), received, &written, nullptr) && written == received &&
            hash.Append(buffer.data(), received);
        if (!success)
        {
            error = "Could not write the update archive";
            break;
        }
        total += received;
        service().downloadedBytes.store(total, std::memory_order_release);
        if (total > expectedSize)
        {
            error = "The update archive is larger than declared";
            success = false;
        }
    }

    FlushFileBuffers(file);
    CloseHandle(file);

    if (service().stopRequested.load(std::memory_order_acquire))
    {
        error = "The update download was cancelled";
        success = false;
    }

    const xr_string actualDigest = success ? hash.Finish() : xr_string{};
    const std::string_view expectedDigest(digest.data() + xr_strlen("sha256:"), 64);
    success = success && total == expectedSize && actualDigest.size() == expectedDigest.size() &&
        std::ranges::equal(actualDigest, expectedDigest, [](unsigned char left, unsigned char right)
        {
            return std::tolower(left) == std::tolower(right);
        });
    if (!success)
    {
        if (error.empty())
            error = total != expectedSize ? "The update size does not match GitHub" : "The update checksum is invalid";
        DeleteFileW(destination.c_str());
    }
    return success;
}

void download_worker()
{
    ServiceState& instance = service();
    xr_string url;
    xr_string digest;
    xr_string version;
    {
        std::lock_guard lock(instance.dataMutex);
        url = instance.downloadUrl;
        digest = instance.digest;
        version = instance.version;
    }

    const std::filesystem::path root = game_directory();
    if (root.empty())
    {
        set_state(UpdateService::State::DownloadFailed, "Could not determine the game directory");
        return;
    }

    const std::filesystem::path archive = root / L".dead-air-x64" / L"update-cache" /
        utf8_to_wide(version) / utf8_to_wide(xr_string(UpdateAssetPrefix) + version + UpdateAssetSuffix);
    xr_string error;
    if (!download_update(url, digest, instance.totalBytes.load(std::memory_order_acquire), archive, error))
    {
        Msg("! Update download failed: %s", error.c_str());
        set_state(UpdateService::State::DownloadFailed, std::move(error));
        return;
    }

    {
        std::lock_guard lock(instance.dataMutex);
        instance.archivePath = archive;
        instance.message.clear();
    }
    Msg("* Update download ready: %s", version.c_str());
    instance.state.store(UpdateService::State::Ready, std::memory_order_release);
}

std::wstring quote_argument(std::wstring_view value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        if (character == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool write_restart_command(const std::filesystem::path& path)
{
    const wchar_t* commandLine = GetCommandLineW();
    if (!commandLine || !*commandLine)
        return false;
    const size_t bytes = (wcslen(commandLine) + 1) * sizeof(wchar_t);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool result = bytes <= std::numeric_limits<DWORD>::max() &&
        WriteFile(file, commandLine, static_cast<DWORD>(bytes), &written, nullptr) &&
        written == static_cast<DWORD>(bytes);
    FlushFileBuffers(file);
    CloseHandle(file);
    return result;
}
}

void UpdateService::StartCheck()
{
    ServiceState& instance = service();
    if (instance.started.exchange(true, std::memory_order_acq_rel))
        return;
    if (instance.worker.joinable())
        instance.worker.join();
    instance.state.store(State::Checking, std::memory_order_release);
    instance.worker = std::thread(check_worker);
}

bool UpdateService::StartDownload()
{
    ServiceState& instance = service();
    const State current = instance.state.load(std::memory_order_acquire);
    if (current != State::Available && current != State::DownloadFailed)
        return false;
    if (instance.worker.joinable())
        instance.worker.join();

    instance.downloadedBytes.store(0, std::memory_order_release);
    instance.stopRequested.store(false, std::memory_order_release);
    {
        std::lock_guard lock(instance.dataMutex);
        instance.message.clear();
    }
    instance.state.store(State::Downloading, std::memory_order_release);
    Msg("* Update download started");
    instance.worker = std::thread(download_worker);
    return true;
}

bool UpdateService::RestartAndApply()
{
    ServiceState& instance = service();
    const State current = instance.state.load(std::memory_order_acquire);
    if (current != State::Ready && current != State::ApplyFailed)
        return false;
    if (instance.worker.joinable())
        instance.worker.join();

    xr_string version;
    xr_string digest;
    std::filesystem::path archive;
    {
        std::lock_guard lock(instance.dataMutex);
        version = instance.version;
        digest = instance.digest;
        archive = instance.archivePath;
    }

    const std::filesystem::path root = game_directory();
    const std::filesystem::path installedUpdater = root / L"DeadAirUpdater.exe";
    const std::filesystem::path cache = archive.parent_path();
    const std::filesystem::path temporaryUpdater = cache / L"ApplyUpdate.exe";
    const std::filesystem::path restartCommand = cache / L"restart-command.bin";
    if (root.empty() || !CopyFileW(installedUpdater.c_str(), temporaryUpdater.c_str(), FALSE) ||
        !write_restart_command(restartCommand))
    {
        Msg("! Update process preparation failed");
        set_state(State::ApplyFailed, "Could not prepare the update process");
        return false;
    }

    std::wstring commandLine = quote_argument(temporaryUpdater.wstring());
    commandLine.append(L" --game-dir ").append(quote_argument(root.wstring()));
    commandLine.append(L" --archive ").append(quote_argument(archive.wstring()));
    commandLine.append(L" --version ").append(quote_argument(utf8_to_wide(version)));
    commandLine.append(L" --digest ").append(quote_argument(utf8_to_wide(digest)));
    commandLine.append(L" --wait-pid ").append(std::to_wstring(GetCurrentProcessId()));
    commandLine.append(L" --restart-command ").append(quote_argument(restartCommand.wstring()));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(temporaryUpdater.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process))
    {
        Msg("! Update process launch failed: Windows error %u", GetLastError());
        set_state(State::ApplyFailed, "Could not start the update process");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Engine.Event.Defer("KERNEL:quit");
    return true;
}

void UpdateService::Dismiss()
{
    ServiceState& instance = service();
    const State current = instance.state.load(std::memory_order_acquire);
    if (current == State::Available || current == State::DownloadFailed || current == State::ApplyFailed)
        instance.state.store(State::Dismissed, std::memory_order_release);
}

UpdateService::Snapshot UpdateService::GetSnapshot()
{
    ServiceState& instance = service();
    Snapshot snapshot;
    snapshot.state = instance.state.load(std::memory_order_acquire);
    snapshot.downloadedBytes = instance.downloadedBytes.load(std::memory_order_acquire);
    snapshot.totalBytes = instance.totalBytes.load(std::memory_order_acquire);
    {
        std::lock_guard lock(instance.dataMutex);
        snapshot.version = instance.version;
        snapshot.message = instance.message;
        snapshot.changesEn = instance.changesEn;
        snapshot.changesRu = instance.changesRu;
    }
    return snapshot;
}

void UpdateService::Shutdown()
{
    ServiceState& instance = service();
    instance.stopRequested.store(true, std::memory_order_release);
    if (instance.worker.joinable())
        instance.worker.join();
}
#else
void UpdateService::StartCheck() {}
bool UpdateService::StartDownload() { return false; }
bool UpdateService::RestartAndApply() { return false; }
void UpdateService::Dismiss() {}
UpdateService::Snapshot UpdateService::GetSnapshot() { return {}; }
void UpdateService::Shutdown() {}
#endif
