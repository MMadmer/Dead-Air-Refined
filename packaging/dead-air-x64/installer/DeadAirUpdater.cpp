#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C"
{
#include "contrib/minizip/ioapi.h"
#include "contrib/minizip/iowin32.h"
#include "contrib/minizip/unzip.h"
}

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
constexpr std::string_view ManifestSchema = "dead-air-refined.update/1";
constexpr size_t MaximumFiles = 1024;
constexpr unsigned long long MaximumExpandedBytes = 1024ull * 1024 * 1024;

struct Arguments
{
    std::filesystem::path gameDirectory;
    std::filesystem::path archive;
    std::filesystem::path restartCommand;
    std::filesystem::path cache;
    std::wstring version;
    std::wstring digest;
    DWORD waitPid{};
    bool finish{};
};

struct PayloadFile
{
    std::filesystem::path relativePath;
    std::string hash;
    unsigned long long size{};
};

struct Manifest
{
    std::wstring version;
    std::vector<PayloadFile> files;
};

std::optional<std::wstring> value_after(std::span<wchar_t*> arguments, std::wstring_view name)
{
    for (size_t index = 1; index + 1 < arguments.size(); ++index)
    {
        if (name == arguments[index])
            return arguments[index + 1];
    }
    return std::nullopt;
}

bool parse_unsigned(std::wstring_view value, DWORD& result)
{
    const std::wstring terminated(value);
    wchar_t* parsedEnd = nullptr;
    const unsigned long long converted = wcstoull(terminated.c_str(), &parsedEnd, 10);
    if (!parsedEnd || parsedEnd == terminated.c_str() || *parsedEnd ||
        converted > (std::numeric_limits<DWORD>::max)())
        return false;
    result = static_cast<DWORD>(converted);
    return true;
}

bool valid_version(std::wstring_view value)
{
    size_t component = 0;
    size_t digits = 0;
    for (const wchar_t character : value)
    {
        if (character >= L'0' && character <= L'9')
        {
            ++digits;
            continue;
        }
        if (character != L'.' || !digits || component == 2)
            return false;
        ++component;
        digits = 0;
    }
    return component == 2 && digits;
}

bool valid_digest(std::wstring_view value)
{
    constexpr std::wstring_view prefix = L"sha256:";
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 64)
        return false;
    return std::ranges::all_of(value.substr(prefix.size()), [](wchar_t character)
    {
        return (character >= L'0' && character <= L'9') ||
            (character >= L'a' && character <= L'f') || (character >= L'A' && character <= L'F');
    });
}

std::optional<Arguments> parse_arguments()
{
    int count = 0;
    wchar_t** raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw)
        return std::nullopt;
    const std::span arguments(raw, static_cast<size_t>(count));

    Arguments result;
    result.finish = std::ranges::any_of(arguments,
        [](const wchar_t* value) { return std::wstring_view(value) == L"--finish"; });
    const auto waitPid = value_after(arguments, L"--wait-pid");
    const auto restart = value_after(arguments, L"--restart-command");
    if (!waitPid || !restart || !parse_unsigned(*waitPid, result.waitPid))
    {
        LocalFree(raw);
        return std::nullopt;
    }
    result.restartCommand = *restart;

    if (result.finish)
    {
        const auto cache = value_after(arguments, L"--cache");
        if (!cache)
        {
            LocalFree(raw);
            return std::nullopt;
        }
        result.cache = *cache;
    }
    else
    {
        const auto gameDirectory = value_after(arguments, L"--game-dir");
        const auto archive = value_after(arguments, L"--archive");
        const auto version = value_after(arguments, L"--version");
        const auto digest = value_after(arguments, L"--digest");
        if (!gameDirectory || !archive || !version || !digest || !valid_version(*version) || !valid_digest(*digest))
        {
            LocalFree(raw);
            return std::nullopt;
        }
        result.gameDirectory = *gameDirectory;
        result.archive = *archive;
        result.version = *version;
        result.digest = *digest;
    }

    LocalFree(raw);
    return result;
}

bool wait_for_process(DWORD processId)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD wait = WaitForSingleObject(process, 5 * 60 * 1000);
    CloseHandle(process);
    return wait == WAIT_OBJECT_0;
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
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string wide_to_utf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string sha256_file(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0);
    if (status >= 0)
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &returned, 0);

    std::vector<unsigned char> object(objectLength);
    std::vector<unsigned char> digest(hashLength);
    if (status >= 0)
        status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);

    std::array<unsigned char, 64 * 1024> buffer{};
    DWORD bytes = 0;
    while (status >= 0 && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr) && bytes)
        status = BCryptHashData(hash, buffer.data(), bytes, 0);
    if (status >= 0)
        status = BCryptFinishHash(hash, digest.data(), hashLength, 0);

    std::string result;
    if (status >= 0)
    {
        static constexpr char hex[] = "0123456789abcdef";
        result.reserve(digest.size() * 2);
        for (const unsigned char byte : digest)
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

bool hash_matches(const std::filesystem::path& path, std::wstring_view expected)
{
    const std::string actual = sha256_file(path);
    const std::wstring_view hexadecimal = expected.substr(std::wstring_view(L"sha256:").size());
    if (actual.size() != hexadecimal.size())
        return false;
    for (size_t index = 0; index != actual.size(); ++index)
    {
        if (static_cast<wchar_t>(actual[index]) != static_cast<wchar_t>(towlower(hexadecimal[index])))
            return false;
    }
    return true;
}

bool safe_relative_path(std::wstring_view value)
{
    if (value.empty() || value.front() == L'/' || value.front() == L'\\' || value.find(L':') != std::wstring_view::npos)
        return false;
    const std::filesystem::path path(value);
    for (const auto& component : path)
    {
        if (component == L"." || component == L".." || component.empty())
            return false;
    }
    return !path.is_absolute() && !path.has_root_path();
}

bool write_zip_entry(unzFile archive, const std::filesystem::path& destination, unsigned long long declaredSize)
{
    if (unzOpenCurrentFile(archive) != UNZ_OK)
        return false;
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
    {
        unzCloseCurrentFile(archive);
        return false;
    }

    HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool success = file != INVALID_HANDLE_VALUE;
    unsigned long long total = 0;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (success)
    {
        const int received = unzReadCurrentFile(archive, buffer.data(), static_cast<unsigned int>(buffer.size()));
        if (received < 0)
        {
            success = false;
            break;
        }
        if (!received)
            break;
        DWORD written = 0;
        success = WriteFile(file, buffer.data(), static_cast<DWORD>(received), &written, nullptr) &&
            written == static_cast<DWORD>(received);
        total += static_cast<unsigned int>(received);
        if (total > declaredSize)
            success = false;
    }
    if (file != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    success = unzCloseCurrentFile(archive) == UNZ_OK && success && total == declaredSize;
    if (!success)
        DeleteFileW(destination.c_str());
    return success;
}

bool extract_archive(const std::filesystem::path& archivePath, const std::filesystem::path& stage,
    std::set<std::wstring, std::less<>>& extractedFiles)
{
    zlib_filefunc64_def fileFunctions{};
    fill_win32_filefunc64W(&fileFunctions);
    unzFile archive = unzOpen2_64(archivePath.c_str(), &fileFunctions);
    if (!archive)
        return false;

    unz_global_info64 global{};
    bool success = unzGetGlobalInfo64(archive, &global) == UNZ_OK && global.number_entry <= MaximumFiles;
    unsigned long long expanded = 0;
    int result = success && global.number_entry ? unzGoToFirstFile(archive) : UNZ_END_OF_LIST_OF_FILE;
    for (unsigned long long index = 0; success && index != global.number_entry; ++index)
    {
        unz_file_info64 info{};
        std::array<char, 1024> name{};
        result = unzGetCurrentFileInfo64(archive, &info, name.data(), static_cast<unsigned long>(name.size()),
            nullptr, 0, nullptr, 0);
        const size_t nameLength = strnlen_s(name.data(), name.size());
        if (result != UNZ_OK || nameLength == name.size())
        {
            success = false;
            break;
        }

        std::string archiveName(name.data(), nameLength);
        std::ranges::replace(archiveName, '\\', '/');
        const bool directory = !archiveName.empty() && archiveName.back() == '/';
        if (directory)
            archiveName.pop_back();
        const std::wstring wideName = utf8_to_wide(archiveName);
        if (!safe_relative_path(wideName) || info.uncompressed_size > MaximumExpandedBytes - expanded)
        {
            success = false;
            break;
        }
        expanded += info.uncompressed_size;

        const std::filesystem::path relative(wideName);
        if (directory)
        {
            std::error_code error;
            std::filesystem::create_directories(stage / relative, error);
            success = !error;
        }
        else
        {
            std::wstring key = relative.generic_wstring();
            std::ranges::transform(key, key.begin(),
                [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
            success = extractedFiles.insert(key).second &&
                write_zip_entry(archive, stage / relative, info.uncompressed_size);
        }

        if (success && index + 1 != global.number_entry)
            success = unzGoToNextFile(archive) == UNZ_OK;
    }
    unzClose(archive);
    return success;
}

bool parse_size(std::string_view value, unsigned long long& result)
{
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size();
}

std::optional<Manifest> parse_manifest(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    Manifest manifest;
    std::string line;
    if (!std::getline(input, line) || line != "schema=" + std::string(ManifestSchema))
        return std::nullopt;
    if (!std::getline(input, line) || !line.starts_with("version="))
        return std::nullopt;
    manifest.version = utf8_to_wide(std::string_view(line).substr(8));
    if (!valid_version(manifest.version))
        return std::nullopt;

    std::set<std::wstring, std::less<>> unique;
    unsigned long long total = 0;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (first != 64 || second == std::string::npos)
            return std::nullopt;

        PayloadFile file;
        file.hash = line.substr(0, first);
        if (!parse_size(std::string_view(line).substr(first + 1, second - first - 1), file.size))
            return std::nullopt;
        file.relativePath = utf8_to_wide(std::string_view(line).substr(second + 1));
        if (!safe_relative_path(file.relativePath.wstring()) || file.size > MaximumExpandedBytes - total ||
            !std::ranges::all_of(file.hash, [](unsigned char character) { return std::isxdigit(character) != 0; }))
        {
            return std::nullopt;
        }
        std::wstring key = file.relativePath.generic_wstring();
        std::ranges::transform(key, key.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
        if (!unique.insert(key).second)
            return std::nullopt;
        total += file.size;
        manifest.files.push_back(std::move(file));
    }
    if (manifest.files.empty() || manifest.files.size() > MaximumFiles)
        return std::nullopt;
    return manifest;
}

bool verify_stage(const std::filesystem::path& stage, const Manifest& manifest,
    const std::set<std::wstring, std::less<>>& extractedFiles)
{
    if (extractedFiles.size() != manifest.files.size() + 1 || !extractedFiles.contains(L"update-manifest.txt"))
        return false;
    for (const PayloadFile& file : manifest.files)
    {
        const std::filesystem::path source = stage / file.relativePath;
        std::error_code error;
        if (!std::filesystem::is_regular_file(source, error) || error ||
            std::filesystem::file_size(source, error) != file.size || error || sha256_file(source) != file.hash)
        {
            return false;
        }
    }
    return true;
}

std::vector<std::filesystem::path> read_paths(const std::filesystem::path& path)
{
    std::vector<std::filesystem::path> result;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::filesystem::path value = utf8_to_wide(line);
        if (!line.empty() && safe_relative_path(value.wstring()))
            result.push_back(value);
    }
    return result;
}

bool write_lines(const std::filesystem::path& path, const std::vector<std::filesystem::path>& values)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    for (const auto& value : values)
        output << wide_to_utf8(value.generic_wstring()) << "\r\n";
    return output.good();
}

bool write_text(const std::filesystem::path& path, std::string_view value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
}

std::filesystem::path next_backup_directory(const std::filesystem::path& gameDirectory, std::wstring_view version)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[128]{};
    swprintf_s(name, L"%04u-%02u-%02u_%02u-%02u-%02u_%.*s", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, static_cast<int>(version.size()), version.data());
    const std::filesystem::path root = gameDirectory / L".dead-air-x64" / L"backups";
    std::filesystem::path result = root / name;
    for (unsigned int suffix = 1; std::filesystem::exists(result); ++suffix)
        result = root / (std::wstring(name) + L"_" + std::to_wstring(suffix));
    return result;
}

std::wstring current_version(const std::filesystem::path& gameDirectory)
{
    std::ifstream input(gameDirectory / L".dead-air-x64" / L"port-version.txt", std::ios::binary);
    std::string value;
    std::getline(input, value);
    if (!value.empty() && value.back() == '\r')
        value.pop_back();
    const std::wstring version = utf8_to_wide(value);
    return valid_version(version) ? version : L"unknown-x64";
}

void append_unique(std::vector<std::filesystem::path>& values, const std::filesystem::path& value)
{
    if (std::ranges::none_of(values, [&](const auto& existing)
        { return _wcsicmp(existing.c_str(), value.c_str()) == 0; }))
    {
        values.push_back(value);
    }
}

std::optional<std::filesystem::path> create_backup(const std::filesystem::path& gameDirectory,
    const std::vector<std::filesystem::path>& incoming, std::vector<std::filesystem::path>& scope)
{
    const std::filesystem::path control = gameDirectory / L".dead-air-x64";
    std::vector<std::filesystem::path> current = read_paths(control / L"managed-files.txt");
    if (current.empty())
    {
        current = read_paths(control / L"runtime-files.txt");
        append_unique(current, L"database/xtra_dead_air_x64.xdb0");
    }
    scope = current;
    for (const auto& file : incoming)
        append_unique(scope, file);

    const std::wstring version = current_version(gameDirectory);
    const std::filesystem::path backup = next_backup_directory(gameDirectory, version);
    const std::filesystem::path files = backup / L"files";
    std::error_code error;
    std::filesystem::create_directories(files, error);
    if (error)
        return std::nullopt;

    std::vector<std::filesystem::path> present;
    for (const auto& relative : scope)
    {
        const std::filesystem::path source = gameDirectory / relative;
        if (!std::filesystem::is_regular_file(source, error))
        {
            error.clear();
            continue;
        }
        const std::filesystem::path destination = files / relative;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error || !std::filesystem::copy_file(source, destination,
                std::filesystem::copy_options::overwrite_existing, error) || error)
        {
            return std::nullopt;
        }
        present.push_back(relative);
    }

    if (!write_lines(backup / L"present-files.txt", present) ||
        !write_lines(backup / L"restore-scope.txt", scope) ||
        !write_lines(backup / L"managed-files.txt", current) ||
        !write_text(backup / L"port-version.txt", wide_to_utf8(version)) ||
        !write_text(backup / L"snapshot-kind.txt", "refined-version"))
    {
        return std::nullopt;
    }
    return backup;
}

bool copy_atomically(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
        return false;
    const std::filesystem::path temporary = destination.wstring() + L".dar-update";
    DeleteFileW(temporary.c_str());
    if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE))
        return false;
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool restore_backup(const std::filesystem::path& gameDirectory, const std::filesystem::path& backup,
    const std::vector<std::filesystem::path>& scope)
{
    for (const auto& relative : scope)
        DeleteFileW((gameDirectory / relative).c_str());
    for (const auto& relative : read_paths(backup / L"present-files.txt"))
    {
        if (!copy_atomically(backup / L"files" / relative, gameDirectory / relative))
            return false;
    }
    const std::filesystem::path control = gameDirectory / L".dead-air-x64";
    return copy_atomically(backup / L"managed-files.txt", control / L"managed-files.txt") &&
        copy_atomically(backup / L"port-version.txt", control / L"port-version.txt");
}

bool apply_payload(const std::filesystem::path& gameDirectory, const std::filesystem::path& stage,
    const Manifest& manifest, const std::vector<std::filesystem::path>& scope)
{
    for (const PayloadFile& file : manifest.files)
    {
        if (!copy_atomically(stage / file.relativePath, gameDirectory / file.relativePath))
            return false;
    }
    for (const auto& oldFile : scope)
    {
        if (std::ranges::none_of(manifest.files, [&](const PayloadFile& incoming)
            { return _wcsicmp(incoming.relativePath.c_str(), oldFile.c_str()) == 0; }))
        {
            DeleteFileW((gameDirectory / oldFile).c_str());
        }
    }
    return true;
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

bool run_and_wait(const std::filesystem::path& executable, const std::wstring& parameters,
    const std::filesystem::path& workingDirectory)
{
    std::wstring command = quote_argument(executable.wstring()) + L" " + parameters;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, workingDirectory.c_str(), &startup, &process))
    {
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 5 * 60 * 1000);
    DWORD exitCode = ERROR_TIMEOUT;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0;
}

std::optional<std::wstring> read_restart_command(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return std::nullopt;
    const std::streamsize bytes = input.tellg();
    if (bytes <= static_cast<std::streamsize>(sizeof(wchar_t)) || bytes > 64 * 1024 || bytes % sizeof(wchar_t))
        return std::nullopt;
    input.seekg(0);
    std::wstring command(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
    input.read(reinterpret_cast<char*>(command.data()), bytes);
    if (!input || command.back())
        return std::nullopt;
    command.pop_back();
    return command;
}

bool launch_command(std::wstring command, const std::filesystem::path& workingDirectory)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            workingDirectory.c_str(), &startup, &process))
    {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int finish_update(const Arguments& arguments)
{
    if (!wait_for_process(arguments.waitPid))
        return 20;
    const auto command = read_restart_command(arguments.restartCommand);
    if (!command)
        return 21;
    const std::filesystem::path gameDirectory = arguments.cache.parent_path().parent_path().parent_path();
    std::error_code error;
    std::filesystem::remove_all(arguments.cache, error);
    if (error)
        return 22;
    std::filesystem::remove(arguments.cache.parent_path(), error);
    error.clear();
    return launch_command(*command, gameDirectory) ? 0 : 23;
}

bool start_finish_process(const std::filesystem::path& gameDirectory, const std::filesystem::path& cache,
    const std::filesystem::path& restartCommand)
{
    const std::filesystem::path updater = gameDirectory / L"DeadAirUpdater.exe";
    std::wstring parameters = L"--finish --cache " + quote_argument(cache.wstring()) +
        L" --wait-pid " + std::to_wstring(GetCurrentProcessId()) +
        L" --restart-command " + quote_argument(restartCommand.wstring());
    std::wstring command = quote_argument(updater.wstring()) + L" " + parameters;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(updater.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, gameDirectory.c_str(), &startup, &process))
    {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int apply_update(const Arguments& arguments)
{
    if (!wait_for_process(arguments.waitPid))
        return 10;
    if (!std::filesystem::is_directory(arguments.gameDirectory) ||
        !std::filesystem::is_regular_file(arguments.archive) || !hash_matches(arguments.archive, arguments.digest))
    {
        return 11;
    }

    const std::filesystem::path cache = arguments.archive.parent_path();
    const std::filesystem::path stage = cache / L"stage";
    std::error_code error;
    std::filesystem::remove_all(stage, error);
    error.clear();
    std::filesystem::create_directories(stage, error);
    if (error)
        return 12;

    std::set<std::wstring, std::less<>> extracted;
    if (!extract_archive(arguments.archive, stage, extracted))
        return 13;
    const auto manifest = parse_manifest(stage / L"update-manifest.txt");
    if (!manifest || manifest->version != arguments.version || !verify_stage(stage, *manifest, extracted))
        return 14;

    std::vector<std::filesystem::path> incoming;
    incoming.reserve(manifest->files.size());
    for (const PayloadFile& file : manifest->files)
        incoming.push_back(file.relativePath);

    std::vector<std::filesystem::path> scope;
    const auto backup = create_backup(arguments.gameDirectory, incoming, scope);
    if (!backup)
        return 15;
    if (!apply_payload(arguments.gameDirectory, stage, *manifest, scope))
    {
        restore_backup(arguments.gameDirectory, *backup, scope);
        return 16;
    }

    const std::filesystem::path maintenance =
        arguments.gameDirectory / L".dead-air-x64" / L"Dead-Air-Refined-Maintenance.exe";
    const std::wstring maintenanceArguments = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /BACKUP=no /TARGET=" +
        quote_argument(arguments.gameDirectory.wstring());
    if (!run_and_wait(maintenance, maintenanceArguments, arguments.gameDirectory))
    {
        restore_backup(arguments.gameDirectory, *backup, scope);
        return 17;
    }

    if (!start_finish_process(arguments.gameDirectory, cache, arguments.restartCommand))
        return 18;
    return 0;
}
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int)
{
    const auto arguments = parse_arguments();
    if (!arguments)
    {
        MessageBoxW(nullptr, L"Параметры запуска средства обновления недействительны.",
            L"Dead Air: Refined", MB_OK | MB_ICONERROR);
        return 1;
    }

    const int result = arguments->finish ? finish_update(*arguments) : apply_update(*arguments);
    if (result)
    {
        wchar_t message[256]{};
        swprintf_s(message, L"Не удалось завершить обновление. Код ошибки: %d.", result);
        MessageBoxW(nullptr, message, L"Dead Air: Refined", MB_OK | MB_ICONERROR);
    }
    return result;
}
