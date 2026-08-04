#include "StdAfx.h"
#include "BugReportService.h"
#include "BugReportConfig.h"
#include "xrCore/ProductVersion.h"

#ifdef XR_PLATFORM_WINDOWS
#include <winhttp.h>

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
struct ServiceState
{
    ~ServiceState()
    {
        if (worker.joinable())
            worker.join();
        if (availabilityWorker.joinable())
            availabilityWorker.join();
    }

    std::atomic<BugReportService::State> state{BugReportService::State::Idle};
    std::mutex messageMutex;
    xr_string message;
    std::thread worker;
    std::atomic<BugReportService::Availability> availability{BugReportService::Availability::Unavailable};
    std::thread availabilityWorker;
};

ServiceState& service()
{
    static ServiceState instance;
    return instance;
}

xr_string ansi_to_utf8(pcstr value)
{
    if (!value || !*value)
        return {};

    const int sourceLength = static_cast<int>(xr_strlen(value));
    const int wideLength = MultiByteToWideChar(CP_ACP, 0, value, sourceLength, nullptr, 0);
    if (wideLength <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, sourceLength, wide.data(), wideLength);

    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return {};

    xr_string result(static_cast<size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), utf8Length, nullptr, nullptr);
    return result;
}

void append_bytes(xr_vector<u8>& destination, std::string_view value)
{
    destination.insert(destination.end(), value.begin(), value.end());
}

void append_part(xr_vector<u8>& body, std::string_view boundary, pcstr name, std::string_view value)
{
    append_bytes(body, "--");
    append_bytes(body, boundary);
    append_bytes(body, "\r\nContent-Disposition: form-data; name=\"");
    append_bytes(body, name);
    append_bytes(body, "\"\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n");
    append_bytes(body, value);
    append_bytes(body, "\r\n");
}

bool read_attachment(pcstr path, xr_vector<u8>& data)
{
    if (!path || !*path)
        return true;

    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
        static_cast<u64>(size.QuadPart) <= BugReportConfig::AttachmentMaximum;
    if (!validSize)
    {
        CloseHandle(file);
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool success = data.empty() ||
        (ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) &&
            read == static_cast<DWORD>(data.size()));
    CloseHandle(file);
    return success;
}

void append_attachment(xr_vector<u8>& body, std::string_view boundary, std::span<const u8> data)
{
    append_bytes(body, "--");
    append_bytes(body, boundary);
    append_bytes(body,
        "\r\nContent-Disposition: form-data; name=\"report\"; filename=\"diagnostic-report.zip\"\r\n"
        "Content-Type: application/zip\r\n\r\n");
    body.insert(body.end(), data.begin(), data.end());
    append_bytes(body, "\r\n");
}

xr_string extract_error_message(std::string_view response)
{
    constexpr std::string_view key = "\"message\"";
    const size_t keyOffset = response.find(key);
    if (keyOffset == std::string_view::npos)
        return {};

    const size_t colon = response.find(':', keyOffset + key.size());
    const size_t quote = colon == std::string_view::npos ? colon : response.find('"', colon + 1);
    if (quote == std::string_view::npos)
        return {};

    xr_string message;
    for (size_t index = quote + 1; index < response.size(); ++index)
    {
        const char character = response[index];
        if (character == '"' && (index == quote + 1 || response[index - 1] != '\\'))
            break;
        if (character == '\\' && index + 1 < response.size())
        {
            const char escaped = response[++index];
            message.push_back(escaped == 'n' ? '\n' : escaped);
        }
        else
            message.push_back(character);
    }
    return message;
}

bool send_request(const xr_string& title, const xr_string& description, pcstr attachmentPath, xr_string& error)
{
    const xr_string token = BugReportConfig::UploadToken();
    if (token.empty())
    {
        error = "Upload token is not configured";
        return false;
    }

    xr_vector<u8> attachment;
    if (!read_attachment(attachmentPath, attachment))
    {
        error = "The diagnostic report could not be read or exceeds 5 MiB";
        return false;
    }

    char boundaryBuffer[64]{};
    xr_sprintf(boundaryBuffer, sizeof(boundaryBuffer), "----DeadAirRefined%08x%08x",
        GetCurrentProcessId(), static_cast<u32>(GetTickCount64()));
    const std::string_view boundary = boundaryBuffer;

    xr_vector<u8> body;
    body.reserve(title.size() + description.size() + attachment.size() + 1024);
    append_part(body, boundary, "title", title);
    append_part(body, boundary, "description", description);
    append_part(body, boundary, "version", DeadAirRefined::Version);
    if (attachmentPath && *attachmentPath)
        append_attachment(body, boundary, attachment);
    append_bytes(body, "--");
    append_bytes(body, boundary);
    append_bytes(body, "--\r\n");

    const std::wstring userAgent = L"Dead Air Refined/" + std::wstring(DeadAirRefined::VersionWide);
    HINTERNET session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        error = "WinHTTP initialization failed";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 10000, 15000, 15000);

    HINTERNET connection = WinHttpConnect(session, BugReportConfig::Host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", BugReportConfig::SubmitPath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (!connection || !request)
    {
        error = "Could not connect to the report server";
        if (request)
            WinHttpCloseHandle(request);
        if (connection)
            WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring headers = L"Content-Type: multipart/form-data; boundary=";
    headers.append(boundary.begin(), boundary.end());
    headers.append(L"\r\nAuthorization: Bearer ");
    headers.append(token.begin(), token.end());
    headers.append(L"\r\nIdempotency-Key: game-");

    char idempotency[48]{};
    xr_sprintf(idempotency, sizeof(idempotency), "%08x-%016llx", GetCurrentProcessId(),
        static_cast<unsigned long long>(GetTickCount64()));
    headers.append(idempotency, idempotency + xr_strlen(idempotency));

    const bool sent = body.size() <= std::numeric_limits<DWORD>::max() &&
        WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), body.data(),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr);

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (sent)
    {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    }

    xr_string response;
    if (sent)
    {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available && response.size() < 64 * 1024)
        {
            const size_t previous = response.size();
            response.resize(previous + std::min<DWORD>(available, 16 * 1024));
            DWORD received = 0;
            if (!WinHttpReadData(request, response.data() + previous,
                    static_cast<DWORD>(response.size() - previous), &received))
            {
                response.resize(previous);
                break;
            }
            response.resize(previous + received);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!sent)
    {
        error = "Network request failed";
        return false;
    }
    if (status != HTTP_STATUS_CREATED)
    {
        error = extract_error_message(response);
        if (error.empty())
        {
            char statusText[64]{};
            xr_sprintf(statusText, sizeof(statusText), "Server returned HTTP %u", status);
            error = statusText;
        }
        return false;
    }
    return true;
}

bool check_availability()
{
    const std::wstring userAgent = L"Dead Air Refined/" + std::wstring(DeadAirRefined::VersionWide);
    HINTERNET session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;

    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, BugReportConfig::Host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"HEAD", BugReportConfig::SubmitPath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;

    const bool received = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    const bool queried = received && WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return queried && status >= 200 && status < 500;
}

void set_result(BugReportService::State state, xr_string message)
{
    ServiceState& instance = service();
    {
        std::lock_guard lock(instance.messageMutex);
        instance.message = std::move(message);
    }
    instance.state.store(state, std::memory_order_release);
}
}

bool BugReportService::Submit(pcstr title, pcstr description, pcstr attachmentPath)
{
    ServiceState& instance = service();
    if (instance.state.load(std::memory_order_acquire) == State::Sending)
        return false;

    if (instance.worker.joinable())
        instance.worker.join();

    const xr_string utf8Title = ansi_to_utf8(title);
    const xr_string utf8Description = ansi_to_utf8(description);
    const xr_string path = attachmentPath ? attachmentPath : "";

    {
        std::lock_guard lock(instance.messageMutex);
        instance.message.clear();
    }
    instance.state.store(State::Sending, std::memory_order_release);
    instance.worker = std::thread([utf8Title, utf8Description, path]
    {
        xr_string error;
        if (send_request(utf8Title, utf8Description, path.c_str(), error))
            set_result(State::Succeeded, {});
        else
            set_result(State::Failed, std::move(error));
    });
    return true;
}

BugReportService::State BugReportService::GetState()
{
    return service().state.load(std::memory_order_acquire);
}

xr_string BugReportService::GetMessage()
{
    ServiceState& instance = service();
    std::lock_guard lock(instance.messageMutex);
    return instance.message;
}

void BugReportService::Reset()
{
    ServiceState& instance = service();
    if (instance.state.load(std::memory_order_acquire) == State::Sending)
        return;
    if (instance.worker.joinable())
        instance.worker.join();
    {
        std::lock_guard lock(instance.messageMutex);
        instance.message.clear();
    }
    instance.state.store(State::Idle, std::memory_order_release);
}

void BugReportService::CheckAvailability()
{
    ServiceState& instance = service();
    if (instance.availability.load(std::memory_order_acquire) == Availability::Checking)
        return;
    if (instance.availabilityWorker.joinable())
        instance.availabilityWorker.join();

    instance.availability.store(Availability::Checking, std::memory_order_release);
    instance.availabilityWorker = std::thread([]
    {
        ServiceState& instance = service();
        instance.availability.store(check_availability() ? Availability::Available : Availability::Unavailable,
            std::memory_order_release);
    });
}

BugReportService::Availability BugReportService::GetAvailability()
{
    return service().availability.load(std::memory_order_acquire);
}
#else
bool BugReportService::Submit(pcstr, pcstr, pcstr) { return false; }
BugReportService::State BugReportService::GetState() { return State::Failed; }
xr_string BugReportService::GetMessage() { return "Bug reports are supported only on Windows"; }
void BugReportService::Reset() {}
void BugReportService::CheckAvailability() {}
BugReportService::Availability BugReportService::GetAvailability() { return Availability::Unavailable; }
#endif
