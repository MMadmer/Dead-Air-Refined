#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
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
#include <thread>
#include <vector>

extern "C"
{
#include "contrib/minizip/ioapi.h"
#include "contrib/minizip/iowin32.h"
#include "contrib/minizip/unzip.h"
}

// The content system's shared core, compiled straight into this executable. It depends on
// nothing but the standard library and the OS, which is the entire reason it can be: the game
// and this program must never disagree about a manifest, a hash or a latch, and the only way
// to guarantee that is to give them the same translation units.
#include "xrContentSync/ContentCommit.h"
#include "xrContentSync/ContentDownload.h"
#include "xrContentSync/ContentHash.h"
#include "xrContentSync/ContentManifest.h"
#include "xrContentSync/ContentPaths.h"
#include "xrContentSync/ContentResolver.h"
#include "xrContentSync/ContentState.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

namespace
{
// Two archive shapes share one applier.
//   /1 - the full payload: every managed file of the target version is packed.
//   /2 - a patch: the manifest still lists EVERY file of the target version, but only the
//        ones that differ from the base version are packed. A file the manifest declares and
//        the archive omits must already sit in the installation with exactly that hash - so
//        the end state is identical to a full install, and a mismatch is caught before the
//        first byte is written. Schema /1 is deliberately left untouched: an older installed
//        updater must keep applying full archives.
constexpr std::string_view ManifestSchemaFull = "dead-air-refined.update/1";
constexpr std::string_view ManifestSchemaPatch = "dead-air-refined.update/2";
constexpr size_t MaximumFiles = 1024;
constexpr unsigned long long MaximumExpandedBytes = 1024ull * 1024 * 1024;

// What this invocation is for. The mode is decided before any option is demanded, because the
// content modes are launched standalone - without --wait-pid or --restart-command - and a
// parser that insisted on those would answer a content command line with a modal error box
// that nobody can see under a silent install.
enum class Mode
{
    Apply,
    Finish,
    ContentPlan,
    ContentFetch,
    ContentCommit
};

struct Arguments
{
    Mode mode{Mode::Apply};
    std::filesystem::path gameDirectory;
    std::filesystem::path archive;
    std::filesystem::path restartCommand;
    std::filesystem::path cache;
    // Touched by the caller to cancel a running fetch. Polled, rather than signalled, so the
    // Inno wizard can create it with nothing but a file write.
    std::filesystem::path cancelFlag;
    std::wstring version;
    std::wstring digest;
    DWORD waitPid{};
    // Appended to an ordinary apply command line. An updater from before the content system
    // simply never reads the token, which is what lets 1.3.5 apply the 1.4.0 payload without
    // knowing content exists - the launched 1.4.0 then repairs what is missing.
    bool commitContent{};
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
    // Set for schema /2. `base` is the version the delta was cut against - informational:
    // the per-file hash check below is what actually decides whether the patch fits.
    bool patch{};
    std::wstring base;
    std::vector<PayloadFile> files;
};

std::wstring lower_key(const std::filesystem::path& path)
{
    std::wstring key = path.generic_wstring();
    std::ranges::transform(key, key.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
    return key;
}

// Scans the whole span rather than returning on the first hit, and refuses a value that is
// itself an option. With five modes assembling command lines, `--archive --version 1.4.0`
// silently yielding an archive path of "--version", or a duplicated --game-dir quietly
// resolving in favour of the first, stops being theoretical.
std::optional<std::wstring> value_after(std::span<wchar_t*> arguments, std::wstring_view name)
{
    std::optional<std::wstring> found;
    for (size_t index = 1; index < arguments.size(); ++index)
    {
        if (name != arguments[index])
            continue;
        if (index + 1 >= arguments.size() || std::wstring_view(arguments[index + 1]).starts_with(L"--"))
            return std::nullopt;
        if (found)
            return std::nullopt;
        found = arguments[index + 1];
    }
    return found;
}

bool has_flag(std::span<wchar_t*> arguments, std::wstring_view name)
{
    for (size_t index = 1; index < arguments.size(); ++index)
    {
        if (name == arguments[index])
            return true;
    }
    return false;
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

    // Mode first, options second. --finish is checked before the content modes so an
    // old-shaped command line behaves bit for bit as it always has.
    if (has_flag(arguments, L"--finish"))
        result.mode = Mode::Finish;
    else if (has_flag(arguments, L"--content-plan"))
        result.mode = Mode::ContentPlan;
    else if (has_flag(arguments, L"--content-fetch"))
        result.mode = Mode::ContentFetch;
    else if (has_flag(arguments, L"--content-commit") && !value_after(arguments, L"--archive"))
        result.mode = Mode::ContentCommit;

    if (result.mode == Mode::ContentPlan || result.mode == Mode::ContentFetch ||
        result.mode == Mode::ContentCommit)
    {
        // Content modes take a game directory and nothing else. In particular they never take
        // a manifest path: the trust root is the installed manifest at its pinned location,
        // and accepting one on a command line would let anything nominate what "complete"
        // means.
        const auto gameDirectory = value_after(arguments, L"--game-dir");
        if (!gameDirectory || gameDirectory->empty())
        {
            LocalFree(raw);
            return std::nullopt;
        }
        result.gameDirectory = *gameDirectory;
        if (const auto cancel = value_after(arguments, L"--cancel-flag"))
            result.cancelFlag = *cancel;
        LocalFree(raw);
        return result;
    }

    const auto waitPid = value_after(arguments, L"--wait-pid");
    const auto restart = value_after(arguments, L"--restart-command");
    if (!waitPid || !restart || !parse_unsigned(*waitPid, result.waitPid))
    {
        LocalFree(raw);
        return std::nullopt;
    }
    result.restartCommand = *restart;

    if (result.mode == Mode::Finish)
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
        result.commitContent = has_flag(arguments, L"--content-commit");
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

// The one hashing implementation in the tree lives in xrContentSync. A second one here is
// exactly the drift the shared library exists to prevent.
bool hash_matches(const std::filesystem::path& path, std::wstring_view expected)
{
    static const std::atomic_bool never{};
    const std::string actual = ContentHash::File(path.wstring(), never);
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

    const auto next_line = [&input](std::string& value)
    {
        if (!std::getline(input, value))
            return false;
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        return true;
    };

    Manifest manifest;
    std::string line;
    if (!next_line(line))
        return std::nullopt;
    if (line == "schema=" + std::string(ManifestSchemaPatch))
        manifest.patch = true;
    else if (line != "schema=" + std::string(ManifestSchemaFull))
        return std::nullopt;
    if (!next_line(line) || !line.starts_with("version="))
        return std::nullopt;
    manifest.version = utf8_to_wide(std::string_view(line).substr(8));
    if (!valid_version(manifest.version))
        return std::nullopt;
    if (manifest.patch)
    {
        if (!next_line(line) || line != "kind=patch")
            return std::nullopt;
        if (!next_line(line) || !line.starts_with("base="))
            return std::nullopt;
        manifest.base = utf8_to_wide(std::string_view(line).substr(5));
        if (!valid_version(manifest.base) || manifest.base == manifest.version)
            return std::nullopt;
    }

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
        if (!unique.insert(lower_key(file.relativePath)).second)
            return std::nullopt;
        total += file.size;
        manifest.files.push_back(std::move(file));
    }
    if (manifest.files.empty() || manifest.files.size() > MaximumFiles)
        return std::nullopt;
    return manifest;
}

bool matches_payload(const std::filesystem::path& path, const PayloadFile& file)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;
    if (std::filesystem::file_size(path, error) != file.size || error)
        return false;

    // ContentHash emits lowercase hex. The UPDATE manifest parser accepts either case - unlike
    // the content one, which enforces lowercase on parse - so compare case-insensitively: an
    // uppercase manifest hash used to match nothing at all, and a patch would report itself
    // unpatchable against a perfectly good installation.
    static const std::atomic_bool never{};
    const std::string actual = ContentHash::File(path.wstring(), never);
    return actual.size() == file.hash.size() &&
        std::ranges::equal(actual, file.hash, [](unsigned char left, unsigned char right)
        { return std::tolower(left) == std::tolower(right); });
}

// Every manifest entry must be accounted for before anything is touched. A full archive has
// to carry all of them; a patch may leave one out only when the installation already holds
// that exact file. `unpatchable` reports the second case failing, which is the one worth
// telling the client about: the answer is to fetch the full archive instead.
bool verify_stage(const std::filesystem::path& stage, const std::filesystem::path& gameDirectory,
    const Manifest& manifest, const std::set<std::wstring, std::less<>>& extractedFiles, bool& unpatchable)
{
    unpatchable = false;
    if (!extractedFiles.contains(L"update-manifest.txt"))
        return false;
    if (!manifest.patch && extractedFiles.size() != manifest.files.size() + 1)
        return false;

    size_t packed = 0;
    for (const PayloadFile& file : manifest.files)
    {
        if (extractedFiles.contains(lower_key(file.relativePath)))
        {
            ++packed;
            if (!matches_payload(stage / file.relativePath, file))
                return false;
        }
        else if (!manifest.patch)
        {
            return false;
        }
        else if (!matches_payload(gameDirectory / file.relativePath, file))
        {
            unpatchable = true;
            return false;
        }
    }
    // Nothing may ride along that the manifest does not declare.
    return extractedFiles.size() == packed + 1;
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
    // Compare through lower_key: managed-files.txt spells separators as backslashes and the
    // manifest as forward slashes, so a raw _wcsicmp on the native string sees one file as
    // two - which quietly widened the backup scope and, worse, let the payload GC below miss
    // a file it was supposed to keep.
    const std::wstring key = lower_key(value);
    if (std::ranges::none_of(values, [&](const auto& existing) { return lower_key(existing) == key; }))
        values.push_back(value);
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

    // Content bundles are never managed files, and this is where that stops being a rule on
    // paper. Everything in `scope` that the incoming manifest does not name gets deleted, and
    // the backup could not restore a bundle because the update manifest never declared it -
    // so one stray bundle name in managed-files.txt would be permanent, silent data loss.
    std::erase_if(current, [](const std::filesystem::path& path)
    { return ContentManifest::IsBundleName(path.filename().string()); });

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
    const Manifest& manifest, const std::vector<std::filesystem::path>& scope,
    const std::set<std::wstring, std::less<>>& extractedFiles)
{
    for (const PayloadFile& file : manifest.files)
    {
        // A patch omits everything that is already correct on disk - verify_stage proved it,
        // so those files are simply left alone.
        if (!extractedFiles.contains(lower_key(file.relativePath)))
            continue;
        if (!copy_atomically(stage / file.relativePath, gameDirectory / file.relativePath))
            return false;
    }
    for (const auto& oldFile : scope)
    {
        const std::wstring oldKey = lower_key(oldFile);
        if (std::ranges::none_of(manifest.files, [&](const PayloadFile& incoming)
            { return lower_key(incoming.relativePath) == oldKey; }))
        {
            DeleteFileW((gameDirectory / oldFile).c_str());
        }
    }
    return true;
}

bool synchronize_payload(const std::filesystem::path& gameDirectory, const std::filesystem::path& stage,
    const std::filesystem::path& backupFiles, const Manifest& manifest,
    const std::set<std::wstring, std::less<>>& extractedFiles)
{
    for (const PayloadFile& file : manifest.files)
    {
        const std::filesystem::path destination = gameDirectory / file.relativePath;
        if (matches_payload(destination, file))
            continue;
        // The maintenance installer removed a file it does not know about. A packed file
        // comes back from the stage; one the patch left out was identical before the update,
        // so the snapshot taken a moment ago holds exactly the right bytes.
        const std::filesystem::path source = extractedFiles.contains(lower_key(file.relativePath))
            ? stage / file.relativePath
            : backupFiles / file.relativePath;
        if (!copy_atomically(source, destination))
            return false;
    }

    for (const PayloadFile& file : manifest.files)
    {
        if (!matches_payload(gameDirectory / file.relativePath, file))
            return false;
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

// ------------------------------------------------------------------------------------------
// Content modes
//
// These run standalone, usually under a silent installer, so they never open a dialog and
// never wait on another process: a five-minute WaitForSingleObject is fatal next to a fetch
// that legitimately takes an hour. Everything they know reaches the caller through an exit
// code and content-fetch-result.txt.

constexpr int ContentExitOk = 0;
constexpr int ContentExitFailed = 26;   // the work could not be completed
constexpr int ContentExitUnusable = 27; // the installation cannot be worked on at all

// The assets repository is pinned here, in shipped code. A manifest's own `repo=` line is
// informational: taking the download host from a downloaded file would let whoever wrote it
// choose where the next gigabytes come from.
constexpr std::string_view AssetsRepository = "MMadmer/Dead-Air-Refined_Assets";

// Keep at most this much finished content in the cache. Sized so a full set plus its
// predecessor survives, which is what makes an update that reverts cost nothing.
constexpr std::uint64_t ContentCacheCapBytes = 6ull * 1024 * 1024 * 1024;

struct ContentSession
{
    ContentPaths::Layout paths;
    ContentManifest::Manifest manifest;
};

// QA only, and deliberately narrow. It redirects where the bytes come from and nothing else:
// every hash still gates the commit, and there is no companion switch that skips the fetch.
// Loopback only, so a stray environment variable on a player's machine cannot point the
// downloader at somebody else's server.
std::string qa_content_base()
{
    wchar_t value[512]{};
    const DWORD length = GetEnvironmentVariableW(L"DAR_QA_CONTENT_BASE", value, static_cast<DWORD>(std::size(value)));
    if (!length || length >= std::size(value))
        return {};

    // Judged on the parsed host, never on a prefix: "http://127.0.0.1:@evil.example/" starts
    // with the right characters and points somewhere else entirely.
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
    return "http://" + wide_to_utf8(host) + ":" + std::to_string(parts.nPort);
}

void write_content_result(const ContentPaths::Layout& paths, int code, std::string_view message)
{
    // Open-write-close, deliberately: the wizard's poll loop keys off the file's timestamp, and
    // a handle held open across a long fetch does not reliably move it.
    std::error_code error;
    std::filesystem::create_directories(paths.Cache(), error);
    // Keyed rather than positional: the installer parses this, and a bare number on line one is
    // the kind of format that survives exactly until someone adds a field.
    std::string body = "exit=";
    body += std::to_string(code);
    body += "\nmessage=";
    body.append(message);
    body += "\n";
    write_text(paths.FetchResult(), body);
}

// A fetch may be asked to acquire content for a version that is not installed yet - that is the
// whole shape of a fresh install, where the payload has not been written when the download has
// to start. The installer stages the manifest from the Setup payload at a FIXED path under the
// cache and this finds it there; it is never named on a command line, so nothing external can
// nominate what "complete" means.
//
// A commit is the opposite case: it finalises the version that is actually installed, so it
// only ever reads the pinned manifest.
bool open_content_session(const Arguments& arguments, ContentSession& session, std::string& error,
    bool acceptPending)
{
    if (arguments.gameDirectory.empty())
    {
        error = "no game directory was given";
        return false;
    }
    session.paths.root = arguments.gameDirectory;

    bool parsed = false;
    if (acceptPending)
    {
        const std::filesystem::path pending = session.paths.Cache() / L"pending-manifest.txt";
        std::error_code exists;
        if (std::filesystem::is_regular_file(pending, exists) && !exists)
            parsed = ContentManifest::ParseFile(pending.wstring(), session.manifest, error);
    }
    if (!parsed)
        parsed = ContentManifest::ParseFile(session.paths.Manifest().wstring(), session.manifest, error);
    if (!parsed)
        return false;

    // The same test the engine applies at startup. Without it the two disagree about whether an
    // installation is workable: this program would spend an hour fetching content the game will
    // then refuse to look at.
    if (!session.manifest.ContentIdMatches())
    {
        error = "the content manifest has been modified - its content-id does not match its bundles";
        return false;
    }
    return true;
}

// Reports what would have to happen, and whether there is room to do it. Writes nothing to the
// installation - the installer calls this before it has touched anything.
int content_plan(const Arguments& arguments)
{
    ContentSession session;
    std::string error;
    if (!open_content_session(arguments, session, error, true))
    {
        write_content_result(session.paths, ContentExitUnusable, "content manifest: " + error);
        return ContentExitUnusable;
    }

    ContentResolver::Options options;
    options.verifyHashes = false; // a plan is a stat pass; the fetch verifies what it writes
    ContentResolver::Plan plan;
    if (!ContentResolver::Resolve(session.manifest, session.paths, options, plan, error))
    {
        write_content_result(session.paths, ContentExitUnusable, error);
        return ContentExitUnusable;
    }

    const std::uint64_t required = ContentResolver::RequiredFreeBytes(plan);
    const std::uint64_t free = ContentResolver::FreeBytes(session.paths.root);
    const std::string message = "bundles=" + std::to_string(session.manifest.bundles.size()) +
        " missing=" + std::to_string(plan.jobs.size()) +
        " fetch=" + std::to_string(plan.bytesToFetch) +
        " cached=" + std::to_string(plan.bytesToMove) +
        " required=" + std::to_string(required) +
        " free=" + std::to_string(free);

    // A zero from FreeBytes means "could not determine", which is not the same as "full" and
    // must not fail an install on a volume the API simply would not answer for.
    if (free && free < required)
    {
        write_content_result(session.paths, ContentExitUnusable, "not enough free space; " + message);
        return ContentExitUnusable;
    }
    write_content_result(session.paths, ContentExitOk, message);
    return ContentExitOk;
}

// Downloads everything missing into the cache and verifies it there. Installs nothing: putting
// bytes into the database directory is the commit's job.
int content_fetch(const Arguments& arguments)
{
    ContentSession session;
    std::string error;
    if (!open_content_session(arguments, session, error, true))
    {
        write_content_result(session.paths, ContentExitUnusable, "content manifest: " + error);
        return ContentExitUnusable;
    }

    // Held for the process lifetime. The wizard polls it to tell "still working" from "died
    // without writing a result", which a progress file alone cannot distinguish.
    const HANDLE liveness = CreateMutexW(nullptr, TRUE, ContentPaths::FetchMutexName);

    std::error_code createError;
    std::filesystem::create_directories(session.paths.Cache(), createError);
    write_text(session.paths.Cache() / L"README.txt",
        "Dead Air: Refined keeps downloaded content here while it is being installed.\r\n"
        "It is safe to delete when the game is not running; the files will be fetched again.\r\n");

    std::atomic_bool cancel{false};
    std::thread watchdog;
    if (!arguments.cancelFlag.empty())
    {
        watchdog = std::thread([&]
        {
            while (!cancel.load(std::memory_order_acquire))
            {
                std::error_code flagError;
                if (std::filesystem::exists(arguments.cancelFlag, flagError))
                {
                    cancel.store(true, std::memory_order_release);
                    return;
                }
                Sleep(200);
            }
        });
    }

    ContentResolver::Options resolveOptions;
    resolveOptions.verifyHashes = false;
    resolveOptions.cancel = &cancel;
    ContentResolver::Plan plan;
    int code = ContentExitOk;
    std::string message;

    if (!ContentResolver::Resolve(session.manifest, session.paths, resolveOptions, plan, error))
    {
        code = ContentExitUnusable;
        message = error;
    }
    else if (plan.Complete())
    {
        message = "nothing to fetch";
    }
    else
    {
        const std::uint64_t required = ContentResolver::RequiredFreeBytes(plan);
        const std::uint64_t free = ContentResolver::FreeBytes(session.paths.root);
        if (free && free < required)
        {
            code = ContentExitUnusable;
            message = "not enough free space: " + std::to_string(required) + " bytes needed, " +
                std::to_string(free) + " available";
        }
        else
        {
            ContentDownload::Options options;
            options.repo = std::string(AssetsRepository);
            options.qaBaseUrl = qa_content_base();
            options.cancel = &cancel;
            options.onProgress = [&](const ContentDownload::Progress& progress)
            {
                // Written on every tick whether or not a byte moved, and through a temporary so
                // the installer never reads a torn line. The installer's stall detector keys off
                // this file's timestamp, so a heartbeat that only ticked on progress would
                // declare a slow but healthy transfer dead.
                const std::filesystem::path final = session.paths.FetchProgress();
                const std::filesystem::path temporary = final.wstring() + L".tmp";
                if (write_text(temporary, std::to_string(progress.done) + "\t" +
                        std::to_string(progress.total) + "\n"))
                {
                    MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
            };

            const ContentDownload::Result result = ContentDownload::Fetch(plan, session.paths, options);
            if (!result.ok)
            {
                code = ContentExitFailed;
                message = result.error;
            }
            else
            {
                message = "fetched " + std::to_string(result.fetched) + " bytes";
            }
        }
    }

    cancel.store(true, std::memory_order_release);
    if (watchdog.joinable())
        watchdog.join();

    write_content_result(session.paths, code, message);
    if (liveness)
    {
        ReleaseMutex(liveness);
        CloseHandle(liveness);
    }
    return code;
}

// Moves verified cache files into the database directory and retires what the manifest no
// longer declares. Idempotent: arriving with the work already done is success, not a second
// commit - the game may well have done it already in its own process.
int content_commit(const Arguments& arguments)
{
    ContentSession session;
    std::string error;
    if (!open_content_session(arguments, session, error, false))
    {
        write_content_result(session.paths, ContentExitUnusable, "content manifest: " + error);
        return ContentExitUnusable;
    }

    ContentResolver::Options options;
    options.verifyHashes = false;
    ContentResolver::Plan plan;
    if (!ContentResolver::Resolve(session.manifest, session.paths, options, plan, error))
    {
        write_content_result(session.paths, ContentExitUnusable, error);
        return ContentExitUnusable;
    }
    if (plan.Complete() && plan.obsolete.empty())
    {
        ContentState::ClearLatch(session.paths.Latch());
        write_content_result(session.paths, ContentExitOk, "already complete");
        return ContentExitOk;
    }

    const ContentCommit::Result result = ContentCommit::Run(session.manifest, plan, session.paths, {});
    if (!result.ok)
    {
        write_content_result(session.paths, ContentExitFailed, result.error);
        return ContentExitFailed;
    }

    ContentResolver::CollectCache(session.paths, session.manifest, plan, ContentCacheCapBytes);
    write_content_result(session.paths, ContentExitOk,
        "installed " + std::to_string(result.installed) + ", retired " + std::to_string(result.demoted));
    return ContentExitOk;
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
    bool unpatchable = false;
    if (!manifest || manifest->version != arguments.version ||
        !verify_stage(stage, arguments.gameDirectory, *manifest, extracted, unpatchable))
    {
        // A patch that does not fit this installation is not a failure of the release - the
        // full archive still applies. Leave a note so the client stops offering the patch for
        // this version instead of looping the player through the same rejection.
        if (unpatchable)
        {
            std::error_code markerError;
            std::filesystem::create_directories(arguments.gameDirectory / L".dead-air-x64", markerError);
            write_text(arguments.gameDirectory / L".dead-air-x64" / L"patch-rejected.txt",
                wide_to_utf8(arguments.version));
            return 24;
        }
        return 14;
    }

    std::vector<std::filesystem::path> incoming;
    incoming.reserve(manifest->files.size());
    for (const PayloadFile& file : manifest->files)
        incoming.push_back(file.relativePath);

    std::vector<std::filesystem::path> scope;
    const auto backup = create_backup(arguments.gameDirectory, incoming, scope);
    if (!backup)
        return 15;
    if (!apply_payload(arguments.gameDirectory, stage, *manifest, scope, extracted))
    {
        restore_backup(arguments.gameDirectory, *backup, scope);
        return 16;
    }

    // Run the installer outside its destination so Inno Setup can safely refresh its uninstall
    // data. A patch only carries it when it changed, so fall back to the copy just applied -
    // either way it runs from the cache, never from inside the installation.
    const std::filesystem::path stagedMaintenance =
        stage / L".dead-air-x64" / L"Dead-Air-Refined-Maintenance.exe";
    const std::filesystem::path maintenance = cache / L"Dead-Air-Refined-Maintenance.exe";
    std::error_code maintenanceError;
    const std::filesystem::path maintenanceSource = std::filesystem::is_regular_file(stagedMaintenance, maintenanceError)
        ? stagedMaintenance
        : arguments.gameDirectory / L".dead-air-x64" / L"Dead-Air-Refined-Maintenance.exe";
    if (!copy_atomically(maintenanceSource, maintenance))
    {
        restore_backup(arguments.gameDirectory, *backup, scope);
        return 25;
    }
    const std::filesystem::path maintenanceLog = cache / L"maintenance.log";
    const std::wstring maintenanceArguments = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /BACKUP=no /TARGET=" +
        quote_argument(arguments.gameDirectory.wstring()) + L" /LOG=" + quote_argument(maintenanceLog.wstring());
    if (!run_and_wait(maintenance, maintenanceArguments, arguments.gameDirectory))
    {
        restore_backup(arguments.gameDirectory, *backup, scope);
        return 17;
    }

    // Inno Setup may remove payload files that are absent from its maintenance-only file table.
    if (!synchronize_payload(arguments.gameDirectory, stage, *backup / L"files", *manifest, extracted))
    {
        restore_backup(arguments.gameDirectory, *backup, scope);
        return 18;
    }

    // The payload is in place, which means the manifest under .dead-air-x64 is now the TARGET
    // version's. Committing content here is a fallback, not a dependency: the game normally
    // does this in its own process before arming the update, and arriving with the work
    // already done returns success without touching anything.
    if (arguments.commitContent)
    {
        Arguments commit;
        commit.mode = Mode::ContentCommit;
        commit.gameDirectory = arguments.gameDirectory;

        // A failure here is NOT an update failure. The runtime update is complete and correct;
        // what did not happen is a content commit that the game does perfectly well on its own,
        // and that has its own recovery channel - the latch is still set, so the next launch
        // reports an incomplete installation and repairs it. Rolling the whole update back
        // would turn a self-healing state into a lost one.
        if (content_commit(commit) != ContentExitOk)
            Sleep(0);
    }

    DeleteFileW((arguments.gameDirectory / L".dead-air-x64" / L"patch-rejected.txt").c_str());

    // The update succeeded, so the pre-update snapshot has served its purpose. Nothing reads
    // it afterwards and nothing prunes it, so `backups\` grew by the full managed set on
    // every single update - the restore path only ever needs the snapshot of the update that
    // is currently in flight.
    std::error_code snapshotError;
    std::filesystem::remove_all(*backup, snapshotError);

    if (!start_finish_process(arguments.gameDirectory, cache, arguments.restartCommand))
        return 19;
    return 0;
}
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int)
{
    const auto arguments = parse_arguments();
    if (!arguments)
    {
        // Parsing failed, so there is no mode to consult - ask the raw command line instead. A
        // malformed content invocation must still fail silently with a code, because the only
        // thing watching it is a poll loop.
        const std::wstring_view line(GetCommandLineW());
        const bool silent = line.find(L"--content-") != std::wstring_view::npos;
        if (!silent)
        {
            MessageBoxW(nullptr, L"Параметры запуска средства обновления недействительны.",
                L"Dead Air: Refined", MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    int result = 0;
    switch (arguments->mode)
    {
    case Mode::Finish: result = finish_update(*arguments); break;
    case Mode::ContentPlan: result = content_plan(*arguments); break;
    case Mode::ContentFetch: result = content_fetch(*arguments); break;
    case Mode::ContentCommit: result = content_commit(*arguments); break;
    default: result = apply_update(*arguments); break;
    }

    // Only the update flow talks to the player through a dialog. A modal box under a silent
    // install or a silent fetch is an invisible hang, not an error message, so the content
    // modes report through their result file and their exit code alone.
    if (result && (arguments->mode == Mode::Apply || arguments->mode == Mode::Finish))
    {
        wchar_t message[256]{};
        swprintf_s(message, L"Не удалось завершить обновление. Код ошибки: %d.", result);
        MessageBoxW(nullptr, message, L"Dead Air: Refined", MB_OK | MB_ICONERROR);
    }
    return result;
}
