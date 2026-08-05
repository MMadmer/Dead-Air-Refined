#include "stdafx.h"
#pragma hdrstop

#include "CrashReport.h"
#include "StackTrace.h"
#include "xrCore/ProductVersion.h"
#include "xrCore/LocatorAPI.h"
#include "xrCore/_math.h"
#include "xrCore/log.h"
#include "xrCore/xrDebug.h"
#include "xrCore/xrMemory.h"

#if defined(XR_PLATFORM_WINDOWS)
#include <bcrypt.h>
#include <dbghelp.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <contrib/minizip/zip.h>

extern pcstr log_name();

namespace
{
constexpr pcstr ReportPrefix = "dar-report-";
constexpr pcstr CrashReportPrefix = "dar-report-crash-";
constexpr pcstr LegacyReportPrefix = "dar-crash-";
constexpr pcstr HandledCrashMarkerName = "handled-crash-report.txt";
constexpr pcstr HandledCrashMarkerTemporaryName = "handled-crash-report.tmp";
constexpr size_t MaximumReports = 10;
constexpr size_t MaximumStackFrames = 96;
constexpr size_t MaximumLogBytes = 4 * 1024 * 1024;
constexpr size_t MaximumHashedLooseFileBytes = 16 * 1024 * 1024;

enum class ReportKind
{
    Session,
    Crash
};

struct RuntimeContext
{
    struct SessionLoad
    {
        u64 samples{};
        double systemCpuTotal{};
        double processCpuTotal{};
        double fpsTotal{};
        double frameMillisecondsTotal{};
        double renderMillisecondsTotal{};
        u64 readBytesPerSecondTotal{};
        u64 writeBytesPerSecondTotal{};
        float systemCpuMaximum{};
        float processCpuMaximum{};
        float fpsMinimum{};
        float fpsMaximum{};
        float frameMillisecondsMaximum{};
        float renderMillisecondsMaximum{};
        u64 readBytesPerSecondMaximum{};
        u64 writeBytesPerSecondMaximum{};
    };

    string32 renderer{"unknown"};
    string128 location{"unknown"};
    string64 activity{"startup"};
    string64 lastOperation{"startup"};
    string_path savePath{};
    string256 gpuName{"unknown"};
    u32 gpuVendorId{};
    u32 gpuDeviceId{};
    u32 gpuFeatureLevel{};
    u64 gpuDedicatedBytes{};
    u64 gpuSharedBytes{};
    u64 gpuDriverVersion{};
    float systemCpuPercent{};
    float processCpuPercent{};
    u64 processReadBytesPerSecond{};
    u64 processWriteBytesPerSecond{};
    SessionLoad sessionLoad;
    CrashRuntimeTelemetry telemetry;
};

struct ModuleInfo
{
    xr_string name;
    std::wstring path;
    uintptr_t base{};
    size_t size{};
    u32 timestamp{};
    xr_string sha256;
};

struct ReportFile
{
    xr_string path;
    FILETIME writeTime{};
};

struct SystemSnapshot
{
    xr_string osName;
    u32 osMajor{};
    u32 osMinor{};
    u32 osBuild{};
    xr_string cpuName;
    u32 physicalCores{};
    u32 logicalProcessors{};
    u64 physicalTotal{};
    u64 physicalAvailable{};
    u64 commitTotal{};
    u64 commitAvailable{};
    u64 diskTotal{};
    u64 diskAvailable{};
    u64 processReadBytes{};
    u64 processWriteBytes{};
    u64 processOtherBytes{};
    u32 processHandles{};
    u32 processThreads{};
    u64 systemUptimeMilliseconds{};
    u64 sessionUptimeMilliseconds{};
    u64 gpuLocalBudget{};
    u64 gpuLocalUsage{};
    u64 gpuNonLocalBudget{};
    u64 gpuNonLocalUsage{};
};

struct ArchiveInfo
{
    xr_string name;
    u64 size{};
    u32 timestamp{};
};

struct LooseFileInfo
{
    xr_string path;
    u64 size{};
    u64 modified{};
    xr_string sha256;
};

struct ContentSnapshot
{
    std::vector<ArchiveInfo> archives;
    std::vector<LooseFileInfo> looseFiles;
    u64 looseBytes{};
};

struct Attachment
{
    pcstr name;
    const xr_string* data;
};

struct SaveAttachment
{
    xr_string name;
    HANDLE file{INVALID_HANDLE_VALUE};
    u64 size{};
    xr_string sha256;
};

struct SampleState
{
    ULONGLONG tick{};
    ULONGLONG idle{};
    ULONGLONG kernel{};
    ULONGLONG user{};
    ULONGLONG processKernel{};
    ULONGLONG processUser{};
    u64 readBytes{};
    u64 writeBytes{};
};

std::atomic_flag contextWriter = ATOMIC_FLAG_INIT;
std::atomic<u32> contextVersion{};
RuntimeContext runtimeContext;
std::atomic_flag reportWriter = ATOMIC_FLAG_INIT;
std::atomic_flag sampleWriter = ATOMIC_FLAG_INIT;
SampleState sampleState;
string_path reportDirectory{};
string_path latestReportPath{};
string_path pendingCrashReportPath{};
ULONGLONG sessionStartTick{};

template <typename Callback>
void update_context(Callback&& callback)
{
    while (contextWriter.test_and_set(std::memory_order_acquire))
        YieldProcessor();

    contextVersion.fetch_add(1, std::memory_order_acq_rel);
    callback(runtimeContext);
    contextVersion.fetch_add(1, std::memory_order_release);
    contextWriter.clear(std::memory_order_release);
}

RuntimeContext snapshot_context()
{
    RuntimeContext snapshot;
    for (u32 attempt = 0; attempt != 8; ++attempt)
    {
        const u32 before = contextVersion.load(std::memory_order_acquire);
        if (before & 1)
            continue;

        snapshot = runtimeContext;
        const u32 after = contextVersion.load(std::memory_order_acquire);
        if (before == after)
            return snapshot;
    }
    return snapshot;
}

void copy_context_string(pstr destination, size_t capacity, pcstr value)
{
    xr_strcpy(destination, capacity, value && *value ? value : "unknown");
}

ULONGLONG file_time_value(const FILETIME& value)
{
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
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

xr_string ansi_to_utf8(std::string_view value)
{
    if (value.empty())
        return {};

    const int wideLength =
        MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wideLength <= 0)
        return xr_string(value);

    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(
        CP_ACP, 0, value.data(), static_cast<int>(value.size()), wide.data(), wideLength);
    return wide_to_utf8(wide);
}

void append_json_string(xr_string& json, std::string_view value)
{
    json.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': json.append("\\\""); break;
        case '\\': json.append("\\\\"); break;
        case '\b': json.append("\\b"); break;
        case '\f': json.append("\\f"); break;
        case '\n': json.append("\\n"); break;
        case '\r': json.append("\\r"); break;
        case '\t': json.append("\\t"); break;
        default:
            if (character < 0x20)
            {
                char escaped[7]{};
                xr_sprintf(escaped, sizeof(escaped), "\\u%04x", character);
                json.append(escaped);
            }
            else
                json.push_back(static_cast<char>(character));
            break;
        }
    }
    json.push_back('"');
}

void append_json_ansi(xr_string& json, pcstr value)
{
    append_json_string(json, ansi_to_utf8(value ? value : ""));
}

void append_number(xr_string& json, u64 value)
{
    char buffer[32]{};
    const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (error == std::errc{})
        json.append(buffer, end);
    else
        json.push_back('0');
}

void append_float(xr_string& json, float value)
{
    char buffer[32]{};
    const auto [end, error] =
        std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::fixed, 3);
    if (error == std::errc{})
        json.append(buffer, end);
    else
        json.push_back('0');
}

void append_hex_string(xr_string& json, u64 value)
{
    char buffer[32]{};
    xr_sprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    append_json_string(json, buffer);
}

xr_string make_path(pcstr fileName)
{
    xr_string path = reportDirectory;
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path.push_back('\\');
    path.append(fileName);
    return path;
}

bool has_suffix(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

void ensure_report_directory()
{
    if (!*reportDirectory)
    {
        string_path workingDirectory{};
        if (!GetCurrentDirectoryA(std::size(workingDirectory), workingDirectory))
            xr_strcpy(workingDirectory, ".");
        strconcat(reportDirectory, workingDirectory, "\\session_reports");
    }

    std::error_code error;
    std::filesystem::create_directories(reportDirectory, error);
}

bool is_report_name(std::string_view name)
{
    return name.starts_with(ReportPrefix) || name.starts_with(LegacyReportPrefix);
}

bool is_crash_report_name(std::string_view name)
{
    return name.starts_with(CrashReportPrefix) && has_suffix(name, ".zip");
}

std::string_view file_name(pcstr path)
{
    const std::string_view value = path ? path : "";
    const size_t separator = value.find_last_of("\\/");
    return separator == std::string_view::npos ? value : value.substr(separator + 1);
}

xr_string read_handled_crash_name()
{
    const xr_string markerPath = make_path(HandledCrashMarkerName);
    HANDLE file = CreateFileA(markerPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    char value[MAX_PATH]{};
    DWORD read = 0;
    const bool success = ReadFile(file, value, sizeof(value) - 1, &read, nullptr) && read < sizeof(value);
    CloseHandle(file);
    if (!success)
        return {};

    value[read] = '\0';
    return value;
}

bool write_handled_crash_name(std::string_view name)
{
    if (name.empty() || name.size() >= MAX_PATH)
        return false;

    const xr_string markerPath = make_path(HandledCrashMarkerName);
    const xr_string temporaryPath = make_path(HandledCrashMarkerTemporaryName);
    HANDLE file = CreateFileA(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const bool saved = WriteFile(file, name.data(), static_cast<DWORD>(name.size()), &written, nullptr) &&
        written == static_cast<DWORD>(name.size()) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!saved)
    {
        DeleteFileA(temporaryPath.c_str());
        return false;
    }

    const bool replaced = !!MoveFileExA(temporaryPath.c_str(), markerPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!replaced)
        DeleteFileA(temporaryPath.c_str());
    return replaced;
}

xr_string find_pending_crash_report()
{
    ensure_report_directory();
    const xr_string pattern = make_path("dar-report-crash-*.zip");

    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return {};

    xr_string newestPath;
    xr_string newestName;
    FILETIME newestWriteTime{};
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !is_crash_report_name(data.cFileName))
            continue;
        if (newestPath.empty() || CompareFileTime(&newestWriteTime, &data.ftLastWriteTime) < 0)
        {
            newestPath = make_path(data.cFileName);
            newestName = data.cFileName;
            newestWriteTime = data.ftLastWriteTime;
        }
    }
    while (FindNextFileA(find, &data));
    FindClose(find);

    if (newestName.empty() || newestName == read_handled_crash_name())
        return {};
    return newestPath;
}

std::vector<ReportFile> enumerate_reports(bool removeTemporaryFiles)
{
    std::vector<ReportFile> reports;
    const xr_string pattern = make_path("*");

    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return reports;

    do
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        const std::string_view name = data.cFileName;
        if (!is_report_name(name))
            continue;

        const xr_string path = make_path(data.cFileName);
        if (has_suffix(name, ".zip"))
            reports.push_back({path, data.ftLastWriteTime});
        else if (removeTemporaryFiles && name.find(".tmp") != std::string_view::npos)
            DeleteFileA(path.c_str());
    }
    while (FindNextFileA(find, &data));

    FindClose(find);
    std::ranges::sort(reports, [](const ReportFile& left, const ReportFile& right)
    {
        return CompareFileTime(&left.writeTime, &right.writeTime) < 0;
    });
    return reports;
}

void rotate_reports(size_t targetCount, bool removeTemporaryFiles)
{
    auto reports = enumerate_reports(removeTemporaryFiles);
    while (reports.size() > targetCount)
    {
        DeleteFileA(reports.front().path.c_str());
        reports.erase(reports.begin());
    }
}

u32 random_report_suffix()
{
    u32 value = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0)
        return value;

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<u32>(counter.QuadPart ^ GetTickCount64());
}

void make_report_names(
    ReportKind kind, xr_string& reportId, xr_string& finalPath, xr_string& dumpPath, xr_string& zipPath)
{
    SYSTEMTIME utc{};
    GetSystemTime(&utc);

    char timestamp[32]{};
    xr_sprintf(timestamp, sizeof(timestamp), "%04u%02u%02uT%02u%02u%02uZ",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);

    char suffix[16]{};
    xr_sprintf(suffix, sizeof(suffix), "%08x", random_report_suffix());
    reportId = xr_string(timestamp) + "-" + suffix;

    const pcstr kindName = kind == ReportKind::Crash ? "crash-" : "session-";
    const xr_string baseName = xr_string(ReportPrefix) + kindName + reportId;
    finalPath = make_path((baseName + ".zip").c_str());
    dumpPath = make_path((baseName + ".tmp.dmp").c_str());
    zipPath = make_path((baseName + ".tmp.zip").c_str());
}

BOOL CALLBACK minidump_callback(
    PVOID, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
{
    if (!input || !output)
        return FALSE;

    switch (input->CallbackType)
    {
    case ThreadCallback:
    case ThreadExCallback:
        output->ThreadWriteFlags &= ~ThreadWriteStack;
        return TRUE;
    case ModuleCallback:
        output->ModuleWriteFlags &= ~(ModuleWriteDataSeg | ModuleWriteTlsData | ModuleWriteCodeSegs);
        return TRUE;
    case MemoryCallback:
        return FALSE;
    case CancelCallback:
        output->Cancel = FALSE;
        output->CheckCancel = FALSE;
        return TRUE;
    default:
        return TRUE;
    }
}

bool write_minidump(pcstr path, _EXCEPTION_POINTERS* exceptionPointers)
{
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbghelp)
        return false;

    using MiniDumpWriteDumpFunc = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
        PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    const auto writeDump =
        reinterpret_cast<MiniDumpWriteDumpFunc>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!writeDump)
    {
        FreeLibrary(dbghelp);
        return false;
    }

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        FreeLibrary(dbghelp);
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = exceptionPointers;
    exception.ClientPointers = FALSE;

    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = minidump_callback;

    const auto dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpIgnoreInaccessibleMemory);
    const bool result = !!writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, dumpType,
        exceptionPointers ? &exception : nullptr, nullptr, &callback);

    FlushFileBuffers(file);
    CloseHandle(file);
    FreeLibrary(dbghelp);
    return result;
}

template <typename Type>
Type* rva_pointer(std::byte* base, size_t size, RVA rva, size_t count = 1)
{
    const size_t offset = rva;
    if (offset > size || count > (size - offset) / sizeof(Type))
        return nullptr;
    return reinterpret_cast<Type*>(base + offset);
}

void redact_minidump_string(std::byte* base, size_t size, RVA rva)
{
    auto* text = rva_pointer<MINIDUMP_STRING>(base, size, rva);
    if (!text || text->Length % sizeof(wchar_t) || rva > size - offsetof(MINIDUMP_STRING, Buffer))
        return;

    const size_t characters = text->Length / sizeof(wchar_t);
    const size_t availableCharacters =
        (size - rva - offsetof(MINIDUMP_STRING, Buffer)) / sizeof(wchar_t);
    if (characters + 1 > availableCharacters)
        return;

    wchar_t* buffer = text->Buffer;
    size_t nameStart = 0;
    for (size_t index = 0; index != characters; ++index)
    {
        if (buffer[index] == L'\\' || buffer[index] == L'/')
            nameStart = index + 1;
    }

    const size_t nameLength = characters - nameStart;
    if (nameStart)
        memmove(buffer, buffer + nameStart, nameLength * sizeof(wchar_t));
    memset(buffer + nameLength, 0, (characters - nameLength + 1) * sizeof(wchar_t));
    text->Length = static_cast<ULONG32>(nameLength * sizeof(wchar_t));
}

void redact_minidump_ascii_path(
    std::byte* base, size_t size, const MINIDUMP_LOCATION_DESCRIPTOR& location, size_t textOffset)
{
    if (!location.Rva || location.Rva > size || location.DataSize > size - location.Rva ||
        textOffset >= location.DataSize)
    {
        return;
    }

    char* text = reinterpret_cast<char*>(base + location.Rva + textOffset);
    const size_t capacity = location.DataSize - textOffset;
    const size_t length = strnlen_s(text, capacity);
    if (length == capacity)
        return;

    size_t nameStart = 0;
    for (size_t index = 0; index != length; ++index)
    {
        if (text[index] == '\\' || text[index] == '/')
            nameStart = index + 1;
    }
    if (!nameStart)
        return;

    const size_t nameLength = length - nameStart;
    memmove(text, text + nameStart, nameLength);
    memset(text + nameLength, 0, capacity - nameLength);
}

void redact_minidump_module_debug_paths(std::byte* base, size_t size, const MINIDUMP_MODULE& module)
{
    constexpr u32 RsdsSignature = 0x53445352;
    constexpr u32 Nb10Signature = 0x3031424e;
    if (module.CvRecord.DataSize >= sizeof(u32) && module.CvRecord.Rva <= size - sizeof(u32))
    {
        u32 signature = 0;
        memcpy(&signature, base + module.CvRecord.Rva, sizeof(signature));
        if (signature == RsdsSignature)
            redact_minidump_ascii_path(base, size, module.CvRecord, 24);
        else if (signature == Nb10Signature)
            redact_minidump_ascii_path(base, size, module.CvRecord, 16);
    }

    auto* misc = rva_pointer<IMAGE_DEBUG_MISC>(base, size, module.MiscRecord.Rva);
    if (!misc || module.MiscRecord.DataSize < offsetof(IMAGE_DEBUG_MISC, Data) ||
        misc->Length > module.MiscRecord.DataSize || misc->Unicode)
    {
        return;
    }
    redact_minidump_ascii_path(base, size, module.MiscRecord, offsetof(IMAGE_DEBUG_MISC, Data));
}

bool erase_minidump_location(
    std::byte* base, size_t size, MINIDUMP_LOCATION_DESCRIPTOR& location)
{
    if (!location.DataSize)
    {
        location.Rva = 0;
        return true;
    }
    if (!location.Rva || location.Rva > size || location.DataSize > size - location.Rva)
        return false;

    memset(base + location.Rva, 0, location.DataSize);
    location = {};
    return true;
}

template <typename ThreadList, typename Thread>
bool erase_minidump_thread_stacks(
    std::byte* base, size_t size, const MINIDUMP_DIRECTORY& directory)
{
    auto* list = rva_pointer<ThreadList>(base, size, directory.Location.Rva);
    if (!list || directory.Location.Rva > size ||
        directory.Location.DataSize < offsetof(ThreadList, Threads))
    {
        return false;
    }

    const size_t availableEntries =
        (directory.Location.DataSize - offsetof(ThreadList, Threads)) / sizeof(Thread);
    if (list->NumberOfThreads > availableEntries)
        return false;

    for (ULONG32 index = 0; index != list->NumberOfThreads; ++index)
    {
        Thread& thread = list->Threads[index];
        if (!erase_minidump_location(base, size, thread.Stack.Memory))
            return false;
        thread.Stack.StartOfMemoryRange = 0;
    }
    return true;
}

bool erase_minidump_memory_list(
    std::byte* base, size_t size, const MINIDUMP_DIRECTORY& directory)
{
    auto* list = rva_pointer<MINIDUMP_MEMORY_LIST>(base, size, directory.Location.Rva);
    if (!list || directory.Location.Rva > size ||
        directory.Location.DataSize < offsetof(MINIDUMP_MEMORY_LIST, MemoryRanges))
    {
        return false;
    }

    const size_t availableEntries =
        (directory.Location.DataSize - offsetof(MINIDUMP_MEMORY_LIST, MemoryRanges)) /
        sizeof(MINIDUMP_MEMORY_DESCRIPTOR);
    if (list->NumberOfMemoryRanges > availableEntries)
        return false;

    for (ULONG32 index = 0; index != list->NumberOfMemoryRanges; ++index)
    {
        MINIDUMP_MEMORY_DESCRIPTOR& range = list->MemoryRanges[index];
        if (!erase_minidump_location(base, size, range.Memory))
            return false;
        range.StartOfMemoryRange = 0;
    }
    list->NumberOfMemoryRanges = 0;
    return true;
}

bool erase_minidump_memory64_list(
    std::byte* base, size_t size, const MINIDUMP_DIRECTORY& directory)
{
    auto* list = rva_pointer<MINIDUMP_MEMORY64_LIST>(base, size, directory.Location.Rva);
    if (!list || directory.Location.Rva > size ||
        directory.Location.DataSize < offsetof(MINIDUMP_MEMORY64_LIST, MemoryRanges))
    {
        return false;
    }

    const size_t availableEntries =
        (directory.Location.DataSize - offsetof(MINIDUMP_MEMORY64_LIST, MemoryRanges)) /
        sizeof(MINIDUMP_MEMORY_DESCRIPTOR64);
    if (list->NumberOfMemoryRanges > availableEntries || list->BaseRva > size)
        return false;

    u64 payloadRva = list->BaseRva;
    for (ULONG64 index = 0; index != list->NumberOfMemoryRanges; ++index)
    {
        MINIDUMP_MEMORY_DESCRIPTOR64& range = list->MemoryRanges[index];
        if (range.DataSize > size - payloadRva)
            return false;
        memset(base + static_cast<size_t>(payloadRva), 0, static_cast<size_t>(range.DataSize));
        payloadRva += range.DataSize;
        range = {};
    }
    list->NumberOfMemoryRanges = 0;
    list->BaseRva = 0;
    return true;
}

bool sanitize_minidump(pcstr path)
{
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 ||
        static_cast<u64>(fileSize.QuadPart) > std::numeric_limits<size_t>::max())
    {
        CloseHandle(file);
        return false;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!mapping)
    {
        CloseHandle(file);
        return false;
    }

    auto* base = static_cast<std::byte*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!base)
    {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    const size_t size = static_cast<size_t>(fileSize.QuadPart);
    auto* header = rva_pointer<MINIDUMP_HEADER>(base, size, 0);
    bool valid = header && header->Signature == MINIDUMP_SIGNATURE;
    if (valid)
    {
        auto* directories =
            rva_pointer<MINIDUMP_DIRECTORY>(base, size, header->StreamDirectoryRva, header->NumberOfStreams);
        valid = !!directories;
        for (ULONG32 index = 0; valid && index != header->NumberOfStreams; ++index)
        {
            const MINIDUMP_DIRECTORY& directory = directories[index];
            if (directory.StreamType == ThreadListStream)
            {
                valid = erase_minidump_thread_stacks<MINIDUMP_THREAD_LIST, MINIDUMP_THREAD>(
                    base, size, directory);
            }
            else if (directory.StreamType == ThreadExListStream)
            {
                valid = erase_minidump_thread_stacks<MINIDUMP_THREAD_EX_LIST, MINIDUMP_THREAD_EX>(
                    base, size, directory);
            }
            else if (directory.StreamType == ModuleListStream)
            {
                auto* list = rva_pointer<MINIDUMP_MODULE_LIST>(base, size, directory.Location.Rva);
                if (!list || directory.Location.Rva > size)
                    continue;
                const size_t moduleBytes = offsetof(MINIDUMP_MODULE_LIST, Modules) +
                    static_cast<size_t>(list->NumberOfModules) * sizeof(MINIDUMP_MODULE);
                if (moduleBytes > size - directory.Location.Rva)
                    continue;
                for (ULONG32 module = 0; module != list->NumberOfModules; ++module)
                {
                    redact_minidump_string(base, size, list->Modules[module].ModuleNameRva);
                    redact_minidump_module_debug_paths(base, size, list->Modules[module]);
                }
            }
            else if (directory.StreamType == UnloadedModuleListStream)
            {
                auto* list = rva_pointer<MINIDUMP_UNLOADED_MODULE_LIST>(base, size, directory.Location.Rva);
                if (!list || list->SizeOfEntry < sizeof(MINIDUMP_UNLOADED_MODULE) ||
                    directory.Location.Rva > size)
                {
                    continue;
                }
                const size_t entryBytes = static_cast<size_t>(list->NumberOfEntries) * list->SizeOfEntry;
                if (list->SizeOfHeader > size - directory.Location.Rva ||
                    entryBytes > size - directory.Location.Rva - list->SizeOfHeader)
                {
                    continue;
                }
                auto* entries = reinterpret_cast<std::byte*>(list) + list->SizeOfHeader;
                for (ULONG32 module = 0; module != list->NumberOfEntries; ++module)
                {
                    auto* entry = reinterpret_cast<MINIDUMP_UNLOADED_MODULE*>(
                        entries + static_cast<size_t>(module) * list->SizeOfEntry);
                    redact_minidump_string(base, size, entry->ModuleNameRva);
                }
            }
            else if (directory.StreamType == MemoryListStream)
            {
                valid = erase_minidump_memory_list(base, size, directory);
            }
            else if (directory.StreamType == Memory64ListStream)
            {
                valid = erase_minidump_memory64_list(base, size, directory);
            }
        }
    }

    FlushViewOfFile(base, 0);
    UnmapViewOfFile(base);
    CloseHandle(mapping);
    FlushFileBuffers(file);
    CloseHandle(file);
    return valid;
}

u32 module_timestamp(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    bool valid = ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0;
    if (!valid)
    {
        CloseHandle(file);
        return 0;
    }

    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    valid = !!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);

    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    valid = valid && ReadFile(file, &signature, sizeof(signature), &read, nullptr) &&
        read == sizeof(signature) && signature == IMAGE_NT_SIGNATURE;
    valid = valid && ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header);
    CloseHandle(file);
    return valid ? header.TimeDateStamp : 0;
}

xr_string sha256_file(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD resultLength = 0;
    xr_string result;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0);
    if (status >= 0)
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &resultLength, 0);

    std::vector<u8> object(objectLength);
    std::vector<u8> digest(hashLength);
    if (status >= 0)
        status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);

    std::array<u8, 64 * 1024> buffer{};
    DWORD read = 0;
    while (status >= 0 && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
        status = BCryptHashData(hash, buffer.data(), read, 0);

    if (status >= 0)
        status = BCryptFinishHash(hash, digest.data(), hashLength, 0);

    if (status >= 0)
    {
        static constexpr char hex[] = "0123456789abcdef";
        result.reserve(digest.size() * 2);
        for (const u8 byte : digest)
        {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
    }

    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return result;
}

xr_string sha256_handle(HANDLE file)
{
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN))
        return {};

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD resultLength = 0;
    xr_string result;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0);
    if (status >= 0)
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &resultLength, 0);

    std::vector<u8> object(objectLength);
    std::vector<u8> digest(hashLength);
    if (status >= 0)
        status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);

    std::array<u8, 64 * 1024> buffer{};
    while (status >= 0)
    {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            status = static_cast<NTSTATUS>(-1);
            break;
        }
        if (!read)
            break;
        status = BCryptHashData(hash, buffer.data(), read, 0);
    }

    if (status >= 0)
        status = BCryptFinishHash(hash, digest.data(), hashLength, 0);

    if (status >= 0)
    {
        static constexpr char hex[] = "0123456789abcdef";
        result.reserve(digest.size() * 2);
        for (const u8 byte : digest)
        {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
    }

    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN);
    return result;
}

void close_save_attachments(std::vector<SaveAttachment>& attachments)
{
    for (SaveAttachment& attachment : attachments)
    {
        if (attachment.file != INVALID_HANDLE_VALUE)
            CloseHandle(attachment.file);
        attachment.file = INVALID_HANDLE_VALUE;
    }
    attachments.clear();
}

std::vector<SaveAttachment> open_save_attachments(pcstr mainPath)
{
    std::vector<SaveAttachment> attachments;
    if (!mainPath || !*mainPath)
        return attachments;

    std::filesystem::path source = mainPath;
    const std::array extensions{".scop", ".scoc", ".scov"};
    for (const pcstr extension : extensions)
    {
        std::filesystem::path path = source;
        path.replace_extension(extension);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (extension == extensions.front())
                return {};
            continue;
        }

        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            close_save_attachments(attachments);
            return {};
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0)
        {
            CloseHandle(file);
            close_save_attachments(attachments);
            return {};
        }

        SaveAttachment attachment;
        attachment.name = "save";
        attachment.name += extension;
        attachment.file = file;
        attachment.size = static_cast<u64>(size.QuadPart);
        attachment.sha256 = sha256_handle(file);
        if (attachment.sha256.empty())
        {
            CloseHandle(file);
            close_save_attachments(attachments);
            return {};
        }
        attachments.push_back(std::move(attachment));
    }
    return attachments;
}

std::vector<ModuleInfo> enumerate_modules(uintptr_t exceptionAddress, size_t& faultingModule)
{
    std::vector<ModuleInfo> modules;
    faultingModule = std::numeric_limits<size_t>::max();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return modules;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            ModuleInfo module;
            module.name = wide_to_utf8(entry.szModule);
            module.path = entry.szExePath;
            module.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            module.size = entry.modBaseSize;
            module.timestamp = module_timestamp(module.path);

            const size_t index = modules.size();
            if (exceptionAddress >= module.base && exceptionAddress - module.base < module.size)
                faultingModule = index;
            modules.push_back(std::move(module));
        }
        while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!modules.empty())
        modules.front().sha256 = sha256_file(modules.front().path);
    if (faultingModule < modules.size() && faultingModule != 0)
        modules[faultingModule].sha256 = sha256_file(modules[faultingModule].path);
    return modules;
}

pcstr exception_name(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "access_violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array_bounds_exceeded";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype_misalignment";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float_divide_by_zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "float_invalid_operation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal_instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in_page_error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer_divide_by_zero";
    case EXCEPTION_INT_OVERFLOW: return "integer_overflow";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable_exception";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged_instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack_overflow";
    default: return "unknown";
    }
}

class Sanitizer
{
public:
    Sanitizer()
    {
        add_replacement(Core.UserName, "<user>");
        add_replacement(Core.CompName, "<machine>");

        string_path value{};
        if (GetEnvironmentVariableA("USERPROFILE", value, std::size(value)))
            add_replacement(value, "<user-profile>");
        if (GetCurrentDirectoryA(std::size(value), value))
            add_replacement(value, "<game-root>");
        if (xr_FS)
        {
            if (FS.update_path(value, "$app_data_root$", "", false))
                add_replacement(value, "<app-data>");
            if (FS.update_path(value, "$game_data$", "", false))
                add_replacement(value, "<game-data>");
        }

        std::ranges::sort(replacements, [](const Replacement& left, const Replacement& right)
        {
            return left.value.size() > right.value.size();
        });
    }

    xr_string Apply(std::string_view source, bool redactSecrets) const
    {
        xr_string result(source);
        for (const Replacement& replacement : replacements)
            replace_case_insensitive(result, replacement.value, replacement.marker);

        xr_string sanitized;
        sanitized.reserve(result.size());
        size_t offset = 0;
        while (offset < result.size())
        {
            const size_t end = result.find('\n', offset);
            xr_string line = result.substr(offset, end == xr_string::npos ? xr_string::npos : end - offset);
            sanitize_line(line, redactSecrets);
            sanitized.append(line);
            if (end == xr_string::npos)
                break;
            sanitized.push_back('\n');
            offset = end + 1;
        }
        return ansi_to_utf8(sanitized);
    }

private:
    struct Replacement
    {
        xr_string value;
        xr_string marker;
    };

    std::vector<Replacement> replacements;

    void add_replacement(pcstr value, pcstr marker)
    {
        if (value && xr_strlen(value) >= 3)
            replacements.push_back({value, marker});
    }

    static void replace_case_insensitive(xr_string& text, std::string_view value, std::string_view replacement)
    {
        if (value.empty())
            return;

        auto begin = text.begin();
        while (begin != text.end())
        {
            const auto found = std::search(begin, text.end(), value.begin(), value.end(),
                [](unsigned char left, unsigned char right)
                {
                    return std::tolower(left) == std::tolower(right);
                });
            if (found == text.end())
                break;
            const size_t offset = static_cast<size_t>(found - text.begin());
            text.replace(offset, value.size(), replacement);
            begin = text.begin() + static_cast<ptrdiff_t>(offset + replacement.size());
        }
    }

    static bool looks_like_ipv4(std::string_view token)
    {
        size_t segments = 0;
        size_t digits = 0;
        u32 value = 0;
        for (const unsigned char character : token)
        {
            if (std::isdigit(character))
            {
                value = value * 10 + character - '0';
                if (++digits > 3 || value > 255)
                    return false;
            }
            else if (character == '.')
            {
                if (!digits)
                    return false;
                ++segments;
                digits = 0;
                value = 0;
            }
            else if (character == ':' && segments == 3 && digits)
                return true;
            else
                return false;
        }
        return segments == 3 && digits;
    }

    static bool looks_like_network_token(std::string_view token)
    {
        while (!token.empty() && strchr("[](){}<>'\"", token.front()))
            token.remove_prefix(1);
        while (!token.empty() && strchr("[](){}<>'\".,;", token.back()))
            token.remove_suffix(1);
        if (token.empty())
            return false;

        const size_t at = token.find('@');
        if (at != std::string_view::npos && at && at + 1 < token.size() &&
            token.find('.', at) != std::string_view::npos)
        {
            return true;
        }
        if (looks_like_ipv4(token))
            return true;

        size_t colons = 0;
        bool onlyAddressCharacters = true;
        for (const unsigned char character : token)
        {
            colons += character == ':';
            onlyAddressCharacters &= std::isxdigit(character) || character == ':' || character == '%';
        }
        return token.size() >= 7 && colons >= 2 && onlyAddressCharacters;
    }

    static void redact_network_tokens(xr_string& line)
    {
        size_t offset = 0;
        while (offset < line.size())
        {
            while (offset < line.size() && std::isspace(static_cast<unsigned char>(line[offset])))
                ++offset;
            const size_t begin = offset;
            while (offset < line.size() && !std::isspace(static_cast<unsigned char>(line[offset])))
                ++offset;
            if (begin != offset && looks_like_network_token(
                    std::string_view(line).substr(begin, offset - begin)))
            {
                line.replace(begin, offset - begin, "<network>");
                offset = begin + xr_strlen("<network>");
            }
        }
    }

    static void redact_save_names(xr_string& line)
    {
        xr_string lower = line;
        std::ranges::transform(lower, lower.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        constexpr std::array suffixes{".scop", ".scoc", ".scov", ".sav", ".save"};
        for (const std::string_view suffix : suffixes)
        {
            size_t found = lower.find(suffix);
            while (found != xr_string::npos)
            {
                size_t begin = found;
                while (begin && !std::isspace(static_cast<unsigned char>(line[begin - 1])) &&
                    !strchr("\"'=()[]", line[begin - 1]))
                {
                    --begin;
                }
                size_t end = found + suffix.size();
                line.replace(begin, end - begin, "<save>");
                lower.replace(begin, end - begin, "<save>");
                found = lower.find(suffix, begin + xr_strlen("<save>"));
            }
        }
    }

    static void sanitize_line(xr_string& line, bool redactSecrets)
    {
        xr_string lower = line;
        std::ranges::transform(lower, lower.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

        if (const size_t command = lower.find("command line"); command != xr_string::npos)
        {
            const size_t separator = line.find(':', command);
            line.replace(separator == xr_string::npos ? command : separator + 1, xr_string::npos, " <redacted>");
            return;
        }

        if (redactSecrets)
        {
            constexpr std::array sensitive{
                "password", "passwd", "token", "secret", "authorization", "auth_key",
                "login", "email", "player_name", "account_name"};
            for (const std::string_view key : sensitive)
            {
                const size_t position = lower.find(key);
                if (position == xr_string::npos)
                    continue;
                size_t separator = line.find('=', position + key.size());
                if (separator == xr_string::npos)
                    separator = line.find_first_of(" \t", position + key.size());
                if (separator != xr_string::npos)
                    line.replace(separator + 1, xr_string::npos, " <redacted>");
                break;
            }
        }

        redact_save_names(line);
        redact_network_tokens(line);
    }
};

xr_string read_file_tail(pcstr path, size_t maximumBytes)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0)
    {
        CloseHandle(file);
        return {};
    }

    const size_t bytes = static_cast<size_t>(
        std::min<u64>(static_cast<u64>(fileSize.QuadPart), maximumBytes));
    LARGE_INTEGER offset{};
    offset.QuadPart = fileSize.QuadPart - static_cast<LONGLONG>(bytes);
    SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);

    xr_string result(bytes, '\0');
    DWORD read = 0;
    const bool success = ReadFile(file, result.data(), static_cast<DWORD>(bytes), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!success)
        return {};
    result.resize(read);

    if (offset.QuadPart)
    {
        const size_t firstLine = result.find('\n');
        if (firstLine != xr_string::npos)
            result.erase(0, firstLine + 1);
        result.insert(0, "[earlier log data omitted]\n");
    }
    return result;
}

xr_string resolve_user_config_path()
{
    pcstr configName = "user.ltx";
    if (IUserConfigHandler* handler = xrDebug::GetUserConfigHandler())
    {
        if (pcstr configured = handler->GetUserConfigFileName(); configured && *configured)
            configName = configured;
    }

    if (std::filesystem::path(configName).is_absolute())
        return configName;

    string_path path{};
    if (xr_FS && FS.update_path(path, "$app_data_root$", configName, false))
        return path;
    return configName;
}

xr_string resolve_fsgame_path()
{
    std::string_view commandLine = Core.Params ? Core.Params : "";
    size_t option = commandLine.find("-fsltx");
    if (option == std::string_view::npos)
        return "fsgame.ltx";

    option += xr_strlen("-fsltx");
    while (option < commandLine.size() && std::isspace(static_cast<unsigned char>(commandLine[option])))
        ++option;
    if (option == commandLine.size())
        return "fsgame.ltx";

    const char quote = commandLine[option] == '"' || commandLine[option] == '\'' ? commandLine[option++] : '\0';
    size_t end = option;
    while (end < commandLine.size() &&
        (quote ? commandLine[end] != quote : !std::isspace(static_cast<unsigned char>(commandLine[end]))))
    {
        ++end;
    }
    return xr_string(commandLine.substr(option, end - option));
}

u32 physical_core_count()
{
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (!bytes)
        return 0;

    std::vector<std::byte> buffer(bytes);
    auto* information = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, information, &bytes))
        return 0;

    u32 cores = 0;
    size_t offset = 0;
    while (offset < bytes)
    {
        auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (!entry->Size || entry->Size > bytes - offset)
            break;
        cores += entry->Relationship == RelationProcessorCore;
        offset += entry->Size;
    }
    return cores;
}

xr_string cpu_name()
{
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 0x80000000);
    if (static_cast<u32>(registers[0]) < 0x80000004)
        return "unknown";

    std::array<char, 49> brand{};
    for (int part = 0; part != 3; ++part)
    {
        __cpuid(registers.data(), 0x80000002 + part);
        memcpy(brand.data() + part * 16, registers.data(), 16);
    }

    xr_string result = brand.data();
    const size_t first = result.find_first_not_of(' ');
    const size_t last = result.find_last_not_of(' ');
    if (first == xr_string::npos)
        return "unknown";
    return result.substr(first, last - first + 1);
}

void query_gpu_memory(const RuntimeContext& context, SystemSnapshot& snapshot)
{
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi)
        return;

    using CreateFactoryFunction = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory =
        reinterpret_cast<CreateFactoryFunction>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!createFactory)
    {
        FreeLibrary(dxgi);
        return;
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    {
        FreeLibrary(dxgi);
        return;
    }

    for (UINT index = 0;; ++index)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            continue;

        DXGI_ADAPTER_DESC1 description{};
        const bool matched = SUCCEEDED(adapter->GetDesc1(&description)) &&
            (!context.gpuVendorId ||
                (description.VendorId == context.gpuVendorId && description.DeviceId == context.gpuDeviceId));
        if (matched)
        {
            IDXGIAdapter3* adapter3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))))
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO local{};
                DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
                {
                    snapshot.gpuLocalBudget = local.Budget;
                    snapshot.gpuLocalUsage = local.CurrentUsage;
                }
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
                {
                    snapshot.gpuNonLocalBudget = nonLocal.Budget;
                    snapshot.gpuNonLocalUsage = nonLocal.CurrentUsage;
                }
                adapter3->Release();
            }
            adapter->Release();
            break;
        }
        adapter->Release();
    }

    factory->Release();
    FreeLibrary(dxgi);
}

SystemSnapshot capture_system_snapshot(const RuntimeContext& context)
{
    SystemSnapshot snapshot;
    snapshot.osName = "Windows";

    struct OsVersionInfo
    {
        ULONG size;
        ULONG major;
        ULONG minor;
        ULONG build;
        ULONG platform;
        WCHAR servicePack[128];
    };
    using RtlGetVersionFunction = LONG(WINAPI*)(OsVersionInfo*);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    {
        const auto getVersion =
            reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (getVersion)
        {
            OsVersionInfo version{};
            version.size = sizeof(version);
            if (getVersion(&version) >= 0)
            {
                snapshot.osMajor = version.major;
                snapshot.osMinor = version.minor;
                snapshot.osBuild = version.build;
            }
        }
    }

    snapshot.cpuName = cpu_name();
    snapshot.physicalCores = physical_core_count();
    snapshot.logicalProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        snapshot.physicalTotal = memory.ullTotalPhys;
        snapshot.physicalAvailable = memory.ullAvailPhys;
        snapshot.commitTotal = memory.ullTotalPageFile;
        snapshot.commitAvailable = memory.ullAvailPageFile;
    }

    ULARGE_INTEGER diskAvailable{};
    ULARGE_INTEGER diskTotal{};
    if (GetDiskFreeSpaceExA(reportDirectory, &diskAvailable, &diskTotal, nullptr))
    {
        snapshot.diskTotal = diskTotal.QuadPart;
        snapshot.diskAvailable = diskAvailable.QuadPart;
    }

    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io))
    {
        snapshot.processReadBytes = io.ReadTransferCount;
        snapshot.processWriteBytes = io.WriteTransferCount;
        snapshot.processOtherBytes = io.OtherTransferCount;
    }
    DWORD processHandles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &processHandles))
        snapshot.processHandles = processHandles;

    HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threads != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(threads, &entry))
        {
            do
            {
                snapshot.processThreads += entry.th32OwnerProcessID == GetCurrentProcessId();
            }
            while (Thread32Next(threads, &entry));
        }
        CloseHandle(threads);
    }

    snapshot.systemUptimeMilliseconds = GetTickCount64();
    snapshot.sessionUptimeMilliseconds = snapshot.systemUptimeMilliseconds - sessionStartTick;
    query_gpu_memory(context, snapshot);
    return snapshot;
}

bool should_hash_loose_file(const std::filesystem::path& path, u64 size)
{
    if (size <= MaximumHashedLooseFileBytes)
        return true;

    xr_string extension = path.extension().string().c_str();
    std::ranges::transform(extension, extension.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    constexpr std::array importantExtensions{
        ".script", ".ltx", ".xml", ".ini", ".cfg", ".json", ".lua", ".hlsl",
        ".ps", ".vs", ".gs", ".cs", ".dll", ".exe"};
    return std::ranges::find(importantExtensions, extension) != importantExtensions.end();
}

ContentSnapshot capture_content_snapshot(const Sanitizer& sanitizer)
{
    ContentSnapshot snapshot;
    if (!xr_FS)
        return snapshot;

    snapshot.archives.reserve(FS.m_archives.size());
    for (const CLocatorAPI::archive& archive : FS.m_archives)
    {
        ArchiveInfo information;
        std::filesystem::path path(archive.path.c_str());
        information.name = sanitizer.Apply(path.filename().string(), false);
        information.size = archive.size;
        information.timestamp = archive.modif;
        snapshot.archives.push_back(std::move(information));
    }

    string_path gameData{};
    if (!FS.update_path(gameData, "$game_data$", "", false) || !*gameData)
        return snapshot;

    std::error_code error;
    const std::filesystem::path root(gameData);
    if (!std::filesystem::exists(root, error))
        return snapshot;

    for (std::filesystem::recursive_directory_iterator iterator(
             root, std::filesystem::directory_options::skip_permission_denied, error), end;
         iterator != end; iterator.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }
        if (!iterator->is_regular_file(error))
            continue;

        LooseFileInfo information;
        const std::filesystem::path path = iterator->path();
        information.size = iterator->file_size(error);
        if (error)
        {
            error.clear();
            information.size = 0;
        }
        snapshot.looseBytes += information.size;

        const std::filesystem::path relative = std::filesystem::relative(path, root, error);
        information.path = sanitizer.Apply(
            (error ? path.filename() : relative).generic_string(), false);
        error.clear();

        const auto writeTime = iterator->last_write_time(error);
        if (!error)
            information.modified = static_cast<u64>(writeTime.time_since_epoch().count());
        error.clear();

        if (should_hash_loose_file(path, information.size))
            information.sha256 = sha256_file(path.wstring());
        snapshot.looseFiles.push_back(std::move(information));
    }
    return snapshot;
}

xr_string build_manifest(pcstr reportId, ReportKind kind, _EXCEPTION_POINTERS* exceptionPointers,
    const RuntimeContext& context, const SystemSnapshot& system, const xrMemoryStatistics& memory,
    const xr_vector<AnonymousStackFrame>& stack, const std::vector<ModuleInfo>& modules,
    size_t faultingModule, const ContentSnapshot& content, pcstr dumpSha256,
    bool hasLog, bool hasUserConfig, bool hasFsConfig, std::span<const SaveAttachment> saves)
{
    const EXCEPTION_RECORD* exception = exceptionPointers ? exceptionPointers->ExceptionRecord : nullptr;
    const DWORD code = exception ? exception->ExceptionCode : 0;
    const uintptr_t address = exception ? reinterpret_cast<uintptr_t>(exception->ExceptionAddress) : 0;
    const ModuleInfo* fault = faultingModule < modules.size() ? &modules[faultingModule] : nullptr;
    const uintptr_t faultRva = fault && address >= fault->base ? address - fault->base : 0;

    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char createdUtc[32]{};
    xr_sprintf(createdUtc, sizeof(createdUtc), "%04u-%02u-%02uT%02u:%02u:%02uZ",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);

    xr_string json;
    json.reserve(128 * 1024 + content.looseFiles.size() * 160);
    json.append("{\"schema\":\"dead-air-refined.session-report/1\",\"report_id\":");
    append_json_string(json, reportId);
    json.append(",\"type\":");
    append_json_string(json, kind == ReportKind::Crash ? "crash" : "manual");
    json.append(",\"created_utc\":");
    append_json_string(json, createdUtc);
    json.append(",\"privacy\":{\"anonymous\":false,\"method\":\"replace-identifiers-preserve-structure\","
        "\"excluded\":[\"command_line\",\"environment\",\"player_identity\",\"raw_stack_memory\"],"
        "\"save_policy\":\"verbatim-active-save\",\"path_policy\":\"module_and_content_names_only\"}");

    json.append(",\"build\":{\"product\":\"Dead Air: Refined\",\"version\":");
    append_json_string(json, DeadAirRefined::Version);
    json.append(",\"architecture\":\"x64\",\"commit\":");
    append_json_ansi(json, Core.GetBuildCommit());
    json.append(",\"build_id\":");
    append_number(json, Core.GetBuildId());
    json.append(",\"executable_sha256\":");
    if (!modules.empty() && !modules.front().sha256.empty())
        append_json_string(json, modules.front().sha256);
    else
        json.append("null");
    json.append("}");

    json.append(",\"crash\":");
    if (kind != ReportKind::Crash)
        json.append("null");
    else
    {
        json.append("{\"exception_code\":");
        append_hex_string(json, code);
        json.append(",\"exception_name\":");
        append_json_string(json, exception_name(code));
        json.append(",\"module\":");
        if (fault)
            append_json_string(json, fault->name);
        else
            json.append("null");
        json.append(",\"module_rva\":");
        if (fault)
            append_hex_string(json, faultRva);
        else
            json.append("null");
        json.append(",\"thread_id\":");
        append_number(json, GetCurrentThreadId());
        json.push_back('}');
    }

    json.append(",\"runtime\":{\"renderer\":");
    append_json_ansi(json, context.renderer);
    json.append(",\"location\":");
    append_json_ansi(json, context.location);
    json.append(",\"activity\":");
    append_json_ansi(json, context.activity);
    json.append(",\"last_operation\":");
    append_json_ansi(json, context.lastOperation);
    json.append(",\"fps\":");
    append_float(json, context.telemetry.fps);
    json.append(",\"frame_ms\":");
    append_float(json, context.telemetry.frameMilliseconds);
    json.append(",\"render_ms\":");
    append_float(json, context.telemetry.renderMilliseconds);
    json.append(",\"system_cpu_percent\":");
    append_float(json, context.systemCpuPercent);
    json.append(",\"process_cpu_percent\":");
    append_float(json, context.processCpuPercent);
    json.append(",\"process_read_bytes_per_second\":");
    append_number(json, context.processReadBytesPerSecond);
    json.append(",\"process_write_bytes_per_second\":");
    append_number(json, context.processWriteBytesPerSecond);
    json.push_back('}');

    const RuntimeContext::SessionLoad& load = context.sessionLoad;
    json.append(",\"session_load\":{\"samples\":");
    append_number(json, load.samples);
    json.append(",\"system_cpu_percent\":{\"average\":");
    append_float(json, load.samples ? static_cast<float>(load.systemCpuTotal / load.samples) : 0.f);
    json.append(",\"maximum\":");
    append_float(json, load.systemCpuMaximum);
    json.append("},\"process_cpu_percent\":{\"average\":");
    append_float(json, load.samples ? static_cast<float>(load.processCpuTotal / load.samples) : 0.f);
    json.append(",\"maximum\":");
    append_float(json, load.processCpuMaximum);
    json.append("},\"fps\":{\"average\":");
    append_float(json, load.samples ? static_cast<float>(load.fpsTotal / load.samples) : 0.f);
    json.append(",\"minimum\":");
    append_float(json, load.fpsMinimum);
    json.append(",\"maximum\":");
    append_float(json, load.fpsMaximum);
    json.append("},\"frame_ms\":{\"average\":");
    append_float(json, load.samples ? static_cast<float>(load.frameMillisecondsTotal / load.samples) : 0.f);
    json.append(",\"maximum\":");
    append_float(json, load.frameMillisecondsMaximum);
    json.append("},\"render_ms\":{\"average\":");
    append_float(json, load.samples ? static_cast<float>(load.renderMillisecondsTotal / load.samples) : 0.f);
    json.append(",\"maximum\":");
    append_float(json, load.renderMillisecondsMaximum);
    json.append("},\"read_bytes_per_second\":{\"average\":");
    append_number(json, load.samples ? load.readBytesPerSecondTotal / load.samples : 0);
    json.append(",\"maximum\":");
    append_number(json, load.readBytesPerSecondMaximum);
    json.append("},\"write_bytes_per_second\":{\"average\":");
    append_number(json, load.samples ? load.writeBytesPerSecondTotal / load.samples : 0);
    json.append(",\"maximum\":");
    append_number(json, load.writeBytesPerSecondMaximum);
    json.append("}}");

    json.append(",\"system\":{\"os\":{\"name\":");
    append_json_string(json, system.osName);
    json.append(",\"major\":");
    append_number(json, system.osMajor);
    json.append(",\"minor\":");
    append_number(json, system.osMinor);
    json.append(",\"build\":");
    append_number(json, system.osBuild);
    json.append(",\"architecture\":\"x64\"},\"cpu\":{\"name\":");
    append_json_string(json, ansi_to_utf8(system.cpuName));
    json.append(",\"physical_cores\":");
    append_number(json, system.physicalCores);
    json.append(",\"logical_processors\":");
    append_number(json, system.logicalProcessors);
    json.append(",\"features\":{\"sse\":");
    json.append(CPU::HasSSE ? "true" : "false");
    json.append(",\"sse2\":");
    json.append(CPU::HasSSE2 ? "true" : "false");
    json.append(",\"sse4_2\":");
    json.append(CPU::HasSSE42 ? "true" : "false");
    json.append(",\"avx\":");
    json.append(CPU::HasAVX ? "true" : "false");
    json.append(",\"avx2\":");
    json.append(CPU::HasAVX2 ? "true" : "false");
    json.append(",\"avx512f\":");
    json.append(CPU::HasAVX512F ? "true" : "false");
    json.append("}},\"ram\":{\"physical_total_bytes\":");
    append_number(json, system.physicalTotal);
    json.append(",\"physical_available_bytes\":");
    append_number(json, system.physicalAvailable);
    json.append(",\"commit_limit_bytes\":");
    append_number(json, system.commitTotal);
    json.append(",\"commit_available_bytes\":");
    append_number(json, system.commitAvailable);
    json.append("},\"disk\":{\"total_bytes\":");
    append_number(json, system.diskTotal);
    json.append(",\"available_bytes\":");
    append_number(json, system.diskAvailable);
    json.append("},\"uptime\":{\"system_ms\":");
    append_number(json, system.systemUptimeMilliseconds);
    json.append(",\"session_ms\":");
    append_number(json, system.sessionUptimeMilliseconds);
    json.append("}}");

    json.append(",\"gpu\":{\"name\":");
    append_json_ansi(json, context.gpuName);
    json.append(",\"vendor_id\":");
    append_number(json, context.gpuVendorId);
    json.append(",\"device_id\":");
    append_number(json, context.gpuDeviceId);
    json.append(",\"feature_level\":");
    append_hex_string(json, context.gpuFeatureLevel);
    json.append(",\"driver_version\":");
    append_hex_string(json, context.gpuDriverVersion);
    json.append(",\"dedicated_bytes\":");
    append_number(json, context.gpuDedicatedBytes);
    json.append(",\"shared_bytes\":");
    append_number(json, context.gpuSharedBytes);
    json.append(",\"local_budget_bytes\":");
    append_number(json, system.gpuLocalBudget);
    json.append(",\"local_usage_bytes\":");
    append_number(json, system.gpuLocalUsage);
    json.append(",\"non_local_budget_bytes\":");
    append_number(json, system.gpuNonLocalBudget);
    json.append(",\"non_local_usage_bytes\":");
    append_number(json, system.gpuNonLocalUsage);
    json.push_back('}');

    json.append(",\"process\":{\"read_bytes\":");
    append_number(json, system.processReadBytes);
    json.append(",\"write_bytes\":");
    append_number(json, system.processWriteBytes);
    json.append(",\"other_io_bytes\":");
    append_number(json, system.processOtherBytes);
    json.append(",\"handles\":");
    append_number(json, system.processHandles);
    json.append(",\"threads\":");
    append_number(json, system.processThreads);
    json.push_back('}');

    json.append(",\"memory\":{\"virtual_free_bytes\":");
    append_number(json, memory.virtualFree);
    json.append(",\"largest_free_block_bytes\":");
    append_number(json, memory.largestFreeBlock);
    json.append(",\"virtual_reserved_bytes\":");
    append_number(json, memory.virtualReserved);
    json.append(",\"virtual_committed_bytes\":");
    append_number(json, memory.virtualCommitted);
    json.append(",\"private_bytes\":");
    append_number(json, memory.privateBytes);
    json.append(",\"working_set_bytes\":");
    append_number(json, memory.workingSet);
    json.append(",\"mapped_bytes\":");
    append_number(json, memory.mappedBytes);
    json.append(",\"textures_bytes\":");
    append_number(json, context.telemetry.textureBytes);
    json.append(",\"models_bytes\":");
    append_number(json, context.telemetry.modelBytes);
    json.append(",\"sound_cache_bytes\":");
    append_number(json, context.telemetry.soundBytes);
    json.append(",\"lua_bytes\":");
    append_number(json, context.telemetry.luaBytes);
    json.push_back('}');

    json.append(",\"objects\":{\"alife\":");
    append_number(json, context.telemetry.alifeObjects);
    json.append(",\"online\":");
    append_number(json, context.telemetry.onlineObjects);
    json.append(",\"pending_release\":");
    append_number(json, context.telemetry.pendingReleaseObjects);
    json.push_back('}');

    json.append(",\"dump\":{\"file\":\"session.dmp\",\"sha256\":");
    append_json_string(json, dumpSha256);
    json.append(",\"contains_stack_memory\":false,\"contains_data_segments\":false}");

    json.append(",\"attachments\":[\"session.dmp\"");
    if (hasLog)
        json.append(",\"session.log\"");
    if (hasUserConfig)
        json.append(",\"user.ltx\"");
    if (hasFsConfig)
        json.append(",\"fsgame.ltx\"");
    for (const SaveAttachment& save : saves)
    {
        json.push_back(',');
        append_json_string(json, save.name);
    }
    json.push_back(']');

    json.append(",\"save\":");
    if (saves.empty())
        json.append("null");
    else
    {
        json.append("{\"files\":[");
        for (size_t index = 0; index != saves.size(); ++index)
        {
            if (index)
                json.push_back(',');
            json.append("{\"file\":");
            append_json_string(json, saves[index].name);
            json.append(",\"size\":");
            append_number(json, saves[index].size);
            json.append(",\"sha256\":");
            append_json_string(json, saves[index].sha256);
            json.push_back('}');
        }
        json.append("]}");
    }

    json.append(",\"stack\":[");
    for (size_t index = 0; index != stack.size(); ++index)
    {
        if (index)
            json.push_back(',');
        json.append("{\"module\":");
        append_json_ansi(json, stack[index].module.c_str());
        json.append(",\"rva\":");
        append_hex_string(json, stack[index].moduleOffset);
        json.push_back('}');
    }
    json.push_back(']');

    json.append(",\"modules\":[");
    for (size_t index = 0; index != modules.size(); ++index)
    {
        if (index)
            json.push_back(',');
        const ModuleInfo& module = modules[index];
        json.append("{\"name\":");
        append_json_string(json, module.name);
        json.append(",\"image_size\":");
        append_number(json, module.size);
        json.append(",\"timestamp\":");
        append_number(json, module.timestamp);
        if (!module.sha256.empty())
        {
            json.append(",\"sha256\":");
            append_json_string(json, module.sha256);
        }
        json.push_back('}');
    }
    json.push_back(']');

    json.append(",\"content\":{\"archives\":[");
    for (size_t index = 0; index != content.archives.size(); ++index)
    {
        if (index)
            json.push_back(',');
        const ArchiveInfo& archive = content.archives[index];
        json.append("{\"name\":");
        append_json_string(json, archive.name);
        json.append(",\"size\":");
        append_number(json, archive.size);
        json.append(",\"timestamp\":");
        append_number(json, archive.timestamp);
        json.push_back('}');
    }
    json.append("],\"loose_total_bytes\":");
    append_number(json, content.looseBytes);
    json.append(",\"loose_files\":[");
    for (size_t index = 0; index != content.looseFiles.size(); ++index)
    {
        if (index)
            json.push_back(',');
        const LooseFileInfo& file = content.looseFiles[index];
        json.append("{\"path\":");
        append_json_string(json, file.path);
        json.append(",\"size\":");
        append_number(json, file.size);
        json.append(",\"modified\":");
        append_number(json, file.modified);
        if (!file.sha256.empty())
        {
            json.append(",\"sha256\":");
            append_json_string(json, file.sha256);
        }
        json.push_back('}');
    }
    json.append("]}}");
    return json;
}

bool add_zip_buffer(zipFile archive, pcstr name, const void* data, size_t size)
{
    if (size > std::numeric_limits<u32>::max())
        return false;
    if (zipOpenNewFileInZip64(archive, name, nullptr, nullptr, 0, nullptr, 0, nullptr,
            Z_DEFLATED, Z_BEST_COMPRESSION, false) != ZIP_OK)
    {
        return false;
    }

    const bool written = !size ||
        zipWriteInFileInZip(archive, data, static_cast<u32>(size)) == ZIP_OK;
    const bool closed = zipCloseFileInZip(archive) == ZIP_OK;
    return written && closed;
}

bool add_zip_file(zipFile archive, pcstr name, pcstr sourcePath)
{
    HANDLE source = CreateFileA(sourcePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (source == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    const bool sized = !!GetFileSizeEx(source, &size) && size.QuadPart >= 0;
    if (!sized || zipOpenNewFileInZip64(archive, name, nullptr, nullptr, 0, nullptr, 0, nullptr,
            Z_DEFLATED, Z_BEST_COMPRESSION, static_cast<u64>(size.QuadPart) >= std::numeric_limits<u32>::max()) != ZIP_OK)
    {
        CloseHandle(source);
        return false;
    }

    bool result = true;
    std::array<u8, 64 * 1024> buffer{};
    DWORD read = 0;
    while (true)
    {
        if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            result = false;
            break;
        }
        if (!read)
            break;
        if (zipWriteInFileInZip(archive, buffer.data(), read) != ZIP_OK)
        {
            result = false;
            break;
        }
    }

    CloseHandle(source);
    result = zipCloseFileInZip(archive) == ZIP_OK && result;
    return result;
}

bool add_zip_handle(zipFile archive, const SaveAttachment& attachment)
{
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(attachment.file, beginning, nullptr, FILE_BEGIN) ||
        zipOpenNewFileInZip64(archive, attachment.name.c_str(), nullptr, nullptr, 0, nullptr, 0, nullptr,
            Z_DEFLATED, Z_BEST_COMPRESSION, attachment.size >= std::numeric_limits<u32>::max()) != ZIP_OK)
    {
        return false;
    }

    bool result = true;
    std::array<u8, 64 * 1024> buffer{};
    while (true)
    {
        DWORD read = 0;
        if (!ReadFile(attachment.file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            result = false;
            break;
        }
        if (!read)
            break;
        if (zipWriteInFileInZip(archive, buffer.data(), read) != ZIP_OK)
        {
            result = false;
            break;
        }
    }
    return zipCloseFileInZip(archive) == ZIP_OK && result;
}

bool write_zip(pcstr path, const xr_string& manifest, pcstr dumpPath, std::span<const Attachment> attachments,
    std::span<const SaveAttachment> saves)
{
    zipFile archive = zipOpen64(path, APPEND_STATUS_CREATE);
    if (!archive)
        return false;

    bool result = add_zip_buffer(archive, "report.json", manifest.data(), manifest.size());
    result = result && add_zip_file(archive, "session.dmp", dumpPath);
    for (const Attachment& attachment : attachments)
    {
        if (result && attachment.data && !attachment.data->empty())
            result = add_zip_buffer(
                archive, attachment.name, attachment.data->data(), attachment.data->size());
    }
    for (const SaveAttachment& save : saves)
    {
        if (result)
            result = add_zip_handle(archive, save);
    }
    result = zipClose(archive, nullptr) == ZIP_OK && result;
    return result;
}

bool write_report(ReportKind kind, _EXCEPTION_POINTERS* exceptionPointers)
{
    if (reportWriter.test_and_set(std::memory_order_acq_rel))
        return false;

    struct WriterGuard
    {
        ~WriterGuard() { reportWriter.clear(std::memory_order_release); }
    } guard;

    if (kind == ReportKind::Crash)
        Memory.release_emergency_reserve();
    ensure_report_directory();
    rotate_reports(MaximumReports - 1, true);

    xr_string reportId;
    xr_string finalPath;
    xr_string dumpPath;
    xr_string zipPath;
    make_report_names(kind, reportId, finalPath, dumpPath, zipPath);

    bool result = write_minidump(dumpPath.c_str(), exceptionPointers);
    result = result && sanitize_minidump(dumpPath.c_str());

    if (result)
    {
        const EXCEPTION_RECORD* exception = exceptionPointers ? exceptionPointers->ExceptionRecord : nullptr;
        const uintptr_t address = exception ? reinterpret_cast<uintptr_t>(exception->ExceptionAddress) : 0;
        size_t faultingModule = std::numeric_limits<size_t>::max();
        auto modules = enumerate_modules(address, faultingModule);

        CONTEXT contextCopy{};
        if (exceptionPointers && exceptionPointers->ContextRecord)
            contextCopy = *exceptionPointers->ContextRecord;
        else
            RtlCaptureContext(&contextCopy);
        auto stack = BuildAnonymousStackTrace(&contextCopy, MaximumStackFrames);

        const RuntimeContext context = snapshot_context();
        auto saves = open_save_attachments(context.savePath);
        const SystemSnapshot system = capture_system_snapshot(context);
        const Sanitizer sanitizer;
        const ContentSnapshot content = capture_content_snapshot(sanitizer);

        FlushLog();
        const xr_string rawLog = read_file_tail(log_name(), MaximumLogBytes);
        const xr_string sanitizedLog = sanitizer.Apply(rawLog, false);
        const xr_string rawUserConfig = read_file_tail(resolve_user_config_path().c_str(), MaximumLogBytes);
        const xr_string sanitizedUserConfig = sanitizer.Apply(rawUserConfig, true);
        const xr_string rawFsConfig = read_file_tail(resolve_fsgame_path().c_str(), MaximumLogBytes);
        const xr_string sanitizedFsConfig = sanitizer.Apply(rawFsConfig, true);

        const xr_string dumpSha256 = sha256_file(std::filesystem::path(dumpPath).wstring());
        const xr_string manifest = build_manifest(reportId.c_str(), kind, exceptionPointers, context, system,
            Memory.statistics(), stack, modules, faultingModule, content, dumpSha256.c_str(),
            !sanitizedLog.empty(), !sanitizedUserConfig.empty(), !sanitizedFsConfig.empty(), saves);

        const std::array attachments{
            Attachment{"session.log", &sanitizedLog},
            Attachment{"user.ltx", &sanitizedUserConfig},
            Attachment{"fsgame.ltx", &sanitizedFsConfig}};
        result = write_zip(zipPath.c_str(), manifest, dumpPath.c_str(), attachments, saves);
        close_save_attachments(saves);
    }

    DeleteFileA(dumpPath.c_str());
    if (result)
    {
        result = !!MoveFileExA(zipPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (result)
            xr_strcpy(latestReportPath, finalPath.c_str());
    }
    if (!result)
        DeleteFileA(zipPath.c_str());

    rotate_reports(MaximumReports, true);
    return result;
}
}

void CrashReporter::Initialize()
{
    sessionStartTick = GetTickCount64();
    SamplePerformance(0.f, 0.f, 0.f);
    ensure_report_directory();
    rotate_reports(MaximumReports, true);
}

void CrashReporter::OnFilesystemInitialized()
{
    string_path path{};
    if (FS.update_path(path, "$app_data_root$", "session_reports", false) && *path)
        xr_strcpy(reportDirectory, path);
    ensure_report_directory();
    rotate_reports(MaximumReports, true);
}

void CrashReporter::SetRenderer(pcstr renderer)
{
    update_context([renderer](RuntimeContext& context)
    {
        copy_context_string(context.renderer, std::size(context.renderer), renderer);
    });
}

void CrashReporter::SetGpuInfo(pcstr name, u32 vendorId, u32 deviceId, u64 dedicatedBytes, u64 sharedBytes,
    u32 featureLevel, u64 driverVersion)
{
    update_context([=](RuntimeContext& context)
    {
        copy_context_string(context.gpuName, std::size(context.gpuName), name);
        context.gpuVendorId = vendorId;
        context.gpuDeviceId = deviceId;
        context.gpuDedicatedBytes = dedicatedBytes;
        context.gpuSharedBytes = sharedBytes;
        context.gpuFeatureLevel = featureLevel;
        context.gpuDriverVersion = driverVersion;
    });
}

void CrashReporter::SetLocation(pcstr location)
{
    update_context([location](RuntimeContext& context)
    {
        copy_context_string(context.location, std::size(context.location), location);
    });
}

void CrashReporter::SetSavePath(pcstr path)
{
    update_context([path](RuntimeContext& context)
    {
        xr_strcpy(context.savePath, std::size(context.savePath), path ? path : "");
    });
}

void CrashReporter::BeginOperation(pcstr operation)
{
    update_context([operation](RuntimeContext& context)
    {
        copy_context_string(context.activity, std::size(context.activity), operation);
        copy_context_string(context.lastOperation, std::size(context.lastOperation), operation);
    });
}

void CrashReporter::EndOperation()
{
    update_context([](RuntimeContext& context)
    {
        xr_strcpy(context.activity, "runtime");
    });
}

void CrashReporter::UpdateTelemetry(const CrashRuntimeTelemetry& telemetry)
{
    update_context([&telemetry](RuntimeContext& context)
    {
        context.telemetry = telemetry;
    });
}

void CrashReporter::SamplePerformance(float fps, float frameMilliseconds, float renderMilliseconds)
{
    const ULONGLONG now = GetTickCount64();
    if (sampleState.tick && now - sampleState.tick < 1000)
        return;
    if (sampleWriter.test_and_set(std::memory_order_acq_rel))
        return;

    FILETIME idleFile{};
    FILETIME kernelFile{};
    FILETIME userFile{};
    FILETIME processCreation{};
    FILETIME processExit{};
    FILETIME processKernelFile{};
    FILETIME processUserFile{};
    IO_COUNTERS io{};
    const bool sampled = GetSystemTimes(&idleFile, &kernelFile, &userFile) &&
        GetProcessTimes(GetCurrentProcess(), &processCreation, &processExit, &processKernelFile, &processUserFile) &&
        GetProcessIoCounters(GetCurrentProcess(), &io);

    if (sampled)
    {
        SampleState current;
        current.tick = now;
        current.idle = file_time_value(idleFile);
        current.kernel = file_time_value(kernelFile);
        current.user = file_time_value(userFile);
        current.processKernel = file_time_value(processKernelFile);
        current.processUser = file_time_value(processUserFile);
        current.readBytes = io.ReadTransferCount;
        current.writeBytes = io.WriteTransferCount;

        const SampleState previous = sampleState;
        sampleState = current;
        if (previous.tick)
        {
            const u64 total = current.kernel + current.user - previous.kernel - previous.user;
            const u64 idle = current.idle - previous.idle;
            const u64 process = current.processKernel + current.processUser -
                previous.processKernel - previous.processUser;
            const u64 elapsed = current.tick - previous.tick;
            update_context([&](RuntimeContext& context)
            {
                context.systemCpuPercent = total ? 100.f * static_cast<float>(total - idle) / total : 0.f;
                context.processCpuPercent = total ? 100.f * static_cast<float>(process) / total : 0.f;
                context.processReadBytesPerSecond =
                    elapsed ? (current.readBytes - previous.readBytes) * 1000 / elapsed : 0;
                context.processWriteBytesPerSecond =
                    elapsed ? (current.writeBytes - previous.writeBytes) * 1000 / elapsed : 0;
                context.telemetry.fps = fps;
                context.telemetry.frameMilliseconds = frameMilliseconds;
                context.telemetry.renderMilliseconds = renderMilliseconds;

                RuntimeContext::SessionLoad& load = context.sessionLoad;
                const bool firstSample = !load.samples;
                ++load.samples;
                load.systemCpuTotal += context.systemCpuPercent;
                load.processCpuTotal += context.processCpuPercent;
                load.fpsTotal += fps;
                load.frameMillisecondsTotal += frameMilliseconds;
                load.renderMillisecondsTotal += renderMilliseconds;
                load.readBytesPerSecondTotal += context.processReadBytesPerSecond;
                load.writeBytesPerSecondTotal += context.processWriteBytesPerSecond;
                load.systemCpuMaximum = std::max(load.systemCpuMaximum, context.systemCpuPercent);
                load.processCpuMaximum = std::max(load.processCpuMaximum, context.processCpuPercent);
                load.fpsMinimum = firstSample ? fps : std::min(load.fpsMinimum, fps);
                load.fpsMaximum = std::max(load.fpsMaximum, fps);
                load.frameMillisecondsMaximum = std::max(load.frameMillisecondsMaximum, frameMilliseconds);
                load.renderMillisecondsMaximum = std::max(load.renderMillisecondsMaximum, renderMilliseconds);
                load.readBytesPerSecondMaximum =
                    std::max(load.readBytesPerSecondMaximum, context.processReadBytesPerSecond);
                load.writeBytesPerSecondMaximum =
                    std::max(load.writeBytesPerSecondMaximum, context.processWriteBytesPerSecond);
            });
        }
    }
    sampleWriter.clear(std::memory_order_release);
}

bool CrashReporter::WriteCrash(_EXCEPTION_POINTERS* exceptionPointers)
{
    return write_report(ReportKind::Crash, exceptionPointers);
}

bool CrashReporter::WriteSession()
{
    const RuntimeContext context = snapshot_context();
    SamplePerformance(
        context.telemetry.fps, context.telemetry.frameMilliseconds, context.telemetry.renderMilliseconds);
    return write_report(ReportKind::Session, nullptr);
}

pcstr CrashReporter::LatestReportPath()
{
    return latestReportPath;
}

pcstr CrashReporter::PendingCrashReportPath()
{
    const xr_string pending = find_pending_crash_report();
    xr_strcpy(pendingCrashReportPath, pending.c_str());
    return pendingCrashReportPath;
}

bool CrashReporter::MarkCrashReportHandled(pcstr reportPath)
{
    const std::string_view name = file_name(reportPath);
    if (!is_crash_report_name(name) || !write_handled_crash_name(name))
        return false;

    if (file_name(pendingCrashReportPath) == name)
        pendingCrashReportPath[0] = '\0';
    return true;
}
#else
void CrashReporter::Initialize() {}
void CrashReporter::OnFilesystemInitialized() {}
void CrashReporter::SetRenderer(pcstr) {}
void CrashReporter::SetGpuInfo(pcstr, u32, u32, u64, u64, u32, u64) {}
void CrashReporter::SetLocation(pcstr) {}
void CrashReporter::BeginOperation(pcstr) {}
void CrashReporter::EndOperation() {}
void CrashReporter::UpdateTelemetry(const CrashRuntimeTelemetry&) {}
void CrashReporter::SamplePerformance(float, float, float) {}
bool CrashReporter::WriteCrash(_EXCEPTION_POINTERS*) { return false; }
bool CrashReporter::WriteSession() { return false; }
pcstr CrashReporter::LatestReportPath() { return ""; }
pcstr CrashReporter::PendingCrashReportPath() { return ""; }
bool CrashReporter::MarkCrashReportHandled(pcstr) { return false; }
#endif
