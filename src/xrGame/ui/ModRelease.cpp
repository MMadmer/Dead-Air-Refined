#include "StdAfx.h"
#include "ModRelease.h"

#ifdef XR_PLATFORM_WINDOWS
#include <contrib/minizip/unzip.h>

#include <array>
#include <charconv>
#include <fstream>

namespace
{
std::string_view mod_trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// callback(section, key, value); sections arrive lowercased, ';' starts a comment
template <typename Callback>
void mod_parse_ini(std::string_view text, const Callback& callback)
{
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);

    xr_string section;
    while (!text.empty())
    {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

        line = mod_trim(line.substr(0, line.find(';')));
        if (line.empty())
            continue;
        if (line.front() == '[')
        {
            const size_t close = line.find(']');
            section = close == std::string_view::npos ? xr_string() : xr_string(mod_trim(line.substr(1, close - 1)));
            xr_strlwr(section);
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = mod_trim(line.substr(0, equals));
        if (!key.empty())
            callback(std::string_view(section), key, mod_trim(line.substr(equals + 1)));
    }
}

bool mod_parse_number(std::string_view text, u64& value)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && !text.empty();
}

bool mod_all_of(std::string_view text, std::string_view extra)
{
    return std::ranges::all_of(text, [extra](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            extra.find(c) != std::string_view::npos;
    });
}

bool mod_valid_id(std::string_view id)
{
    return !id.empty() && std::ranges::all_of(id, [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

bool mod_valid_asset_name(std::string_view name)
{
    if (name.size() < 5 || name.size() > 128 || name.front() == '.' || !mod_all_of(name, "-._"))
        return false;
    const std::string_view extension = name.substr(name.size() - 4);
    return extension[0] == '.' && (extension[1] | 0x20) == 'z' && (extension[2] | 0x20) == 'i' &&
        (extension[3] | 0x20) == 'p';
}

bool mod_reserved_device(std::string_view component)
{
    xr_string base(component.substr(0, component.find('.')));
    xr_strlwr(base);
    if (base == "con" || base == "prn" || base == "aux" || base == "nul")
        return true;
    return base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' &&
        base[3] <= '9';
}

std::wstring mod_utf8_to_wide(std::string_view text)
{
    if (text.empty())
        return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// minizip's static library ships without its Win32 backend, and the stdio one takes a narrow
// path. These five callbacks are all it needs to read an archive by its wide path instead.
voidpf ZCALLBACK mod_zip_open(voidpf, const void* filename, int)
{
    const HANDLE file = CreateFileW(static_cast<const wchar_t*>(filename), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return file == INVALID_HANDLE_VALUE ? nullptr : file;
}

uLong ZCALLBACK mod_zip_read(voidpf, voidpf stream, void* buffer, uLong size)
{
    DWORD received = 0;
    return ReadFile(stream, buffer, size, &received, nullptr) ? received : 0;
}

uLong ZCALLBACK mod_zip_write(voidpf, voidpf, const void*, uLong) { return 0; }

ZPOS64_T ZCALLBACK mod_zip_tell(voidpf, voidpf stream)
{
    LARGE_INTEGER position{};
    return SetFilePointerEx(stream, {}, &position, FILE_CURRENT) ? static_cast<ZPOS64_T>(position.QuadPart) :
                                                                   static_cast<ZPOS64_T>(-1);
}

long ZCALLBACK mod_zip_seek(voidpf, voidpf stream, ZPOS64_T offset, int origin)
{
    const DWORD method = origin == ZLIB_FILEFUNC_SEEK_CUR ? FILE_CURRENT :
        origin == ZLIB_FILEFUNC_SEEK_END                  ? FILE_END :
                                                            FILE_BEGIN;
    LARGE_INTEGER distance;
    distance.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(stream, distance, nullptr, method) ? 0 : -1;
}

int ZCALLBACK mod_zip_close(voidpf, voidpf stream) { return CloseHandle(stream) ? 0 : -1; }
int ZCALLBACK mod_zip_error(voidpf, voidpf) { return 0; }

bool mod_write_entry(unzFile archive, const std::filesystem::path& target, u64 declaredSize,
    const std::atomic_bool& cancel, xr_string& error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(target.parent_path(), directoryError);
    if (directoryError || unzOpenCurrentFile(archive) != UNZ_OK)
    {
        error = directoryError ? "could not create a folder of the package" : "unsupported or damaged package entry";
        return false;
    }

    // CREATE_NEW: the staging folder starts empty, so an existing file is a path the release
    // names twice - under whatever spelling the file system considers the same
    const HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool success = file != INVALID_HANDLE_VALUE;
    if (!success)
        error = GetLastError() == ERROR_FILE_EXISTS ? "the release names a path twice" : "could not create a package file";

    u64 total = 0;
    std::array<u8, 64 * 1024> buffer{};
    while (success)
    {
        if (cancel.load(std::memory_order_acquire))
        {
            error = "cancelled";
            success = false;
            break;
        }
        const int received = unzReadCurrentFile(archive, buffer.data(), static_cast<unsigned>(buffer.size()));
        if (received < 0)
        {
            error = "damaged package entry";
            success = false;
            break;
        }
        if (!received)
            break;
        DWORD written = 0;
        total += static_cast<u32>(received);
        success = total <= declaredSize &&
            WriteFile(file, buffer.data(), static_cast<DWORD>(received), &written, nullptr) &&
            written == static_cast<DWORD>(received);
        if (!success)
            error = total > declaredSize ? "a package entry is larger than it declares" : "could not write a package file";
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);

    // the close is where minizip reports a CRC mismatch
    const bool closed = unzCloseCurrentFile(archive) == UNZ_OK;
    if (success && (!closed || total != declaredSize))
    {
        error = "damaged package entry";
        success = false;
    }
    return success;
}
}

bool ModRelease::ValidModuleId(std::string_view id) { return mod_valid_id(id); }

ModRelease::Version ModRelease::ParseVersion(std::string_view text)
{
    Version version;
    text = mod_trim(text);
    size_t count = 0;
    while (count < std::size(version.parts))
    {
        u32 part = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), part);
        if (error != std::errc{})
            break;
        version.parts[count++] = part;
        text.remove_prefix(static_cast<size_t>(end - text.data()));
        if (text.empty() || text.front() != '.')
            break;
        text.remove_prefix(1);
    }
    version.valid = count != 0;
    return version;
}

u64 ModRelease::Descriptor::DownloadBytes() const
{
    u64 total = 0;
    for (const Package& package : packages)
        total += package.size;
    return total;
}

bool ModRelease::ParseDescriptor(std::string_view text, Descriptor& out, xr_string& error)
{
    out = {};
    if (text.size() > MaximumDescriptorBytes)
    {
        error = "descriptor is too large";
        return false;
    }

    u64 schema = 0;
    bool malformed = false;
    mod_parse_ini(text, [&](std::string_view section, std::string_view key, std::string_view value)
    {
        if (section == "release")
        {
            if (key == "schema")
                malformed |= !mod_parse_number(value, schema);
            else if (key == "id")
                out.id = value;
            else if (key == "version")
                out.version = value;
            else if (key == "requires_game")
                out.requiresGame = value;
            else if (key == "files")
                malformed |= !mod_parse_number(value, out.files);
            else if (key == "unpacked")
                malformed |= !mod_parse_number(value, out.unpacked);
        }
        else if (section == "packages")
        {
            Package package;
            package.name = key;
            const size_t comma = value.find(',');
            const std::string_view digest = comma == std::string_view::npos ? std::string_view() : mod_trim(value.substr(comma + 1));
            package.sha256 = digest;
            xr_strlwr(package.sha256);
            malformed |= !mod_parse_number(mod_trim(value.substr(0, comma)), package.size);
            out.packages.emplace_back(std::move(package));
        }
    });

    out.schema = static_cast<u32>(std::min<u64>(schema, std::numeric_limits<u32>::max()));
    if (!out.schema || !mod_valid_id(out.id) || out.version.size() > 32 || !mod_all_of(out.version, "._+-") ||
        !ParseVersion(out.version).valid)
    {
        error = "descriptor has no valid schema, id or version";
        return false;
    }
    if (out.schema > SupportedSchema)
        return true;

    if (malformed || (!out.requiresGame.empty() && !ParseVersion(out.requiresGame).valid))
    {
        error = "descriptor holds a malformed value";
        return false;
    }
    if (out.packages.empty() || out.packages.size() > MaximumPackages)
    {
        error = "descriptor names no package";
        return false;
    }
    for (size_t index = 0; index != out.packages.size(); ++index)
    {
        const Package& package = out.packages[index];
        const bool duplicate = std::ranges::any_of(out.packages.begin(), out.packages.begin() + index,
            [&](const Package& other) { return 0 == xr_stricmp(other.name.c_str(), package.name.c_str()); });
        const bool digest = package.sha256.size() == 64 &&
            std::ranges::all_of(package.sha256, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        if (duplicate || !digest || !package.size || !mod_valid_asset_name(package.name))
        {
            error = "descriptor names an invalid package";
            return false;
        }
    }
    return true;
}

bool ModRelease::SafeRelativePath(std::string_view path)
{
    if (path.empty())
        return false;
    while (!path.empty())
    {
        const size_t slash = path.find('/');
        const std::string_view component = path.substr(0, slash);
        path.remove_prefix(slash == std::string_view::npos ? path.size() : slash + 1);
        if (component.empty() || component == "." || component == ".." || component.back() == '.' ||
            component.back() == ' ' || mod_reserved_device(component))
            return false;
        const bool clean = std::ranges::all_of(component, [](char c)
        {
            return static_cast<u8>(c) >= 0x20 && std::string_view("<>:\"|?*\\").find(c) == std::string_view::npos;
        });
        if (!clean)
            return false;
    }
    return true;
}

bool ModRelease::Unpack(const std::filesystem::path& archivePath, std::string_view moduleId,
    const std::filesystem::path& destination, u64 fileLimit, u64 byteLimit, UnpackTotals& totals,
    const std::atomic_bool& cancel, xr_string& error)
{
    zlib_filefunc64_def functions{};
    functions.zopen64_file = mod_zip_open;
    functions.zread_file = mod_zip_read;
    functions.zwrite_file = mod_zip_write;
    functions.ztell64_file = mod_zip_tell;
    functions.zseek64_file = mod_zip_seek;
    functions.zclose_file = mod_zip_close;
    functions.zerror_file = mod_zip_error;

    const unzFile archive = unzOpen2_64(archivePath.c_str(), &functions);
    if (!archive)
    {
        error = "package is not a ZIP archive";
        return false;
    }

    xr_string prefix = "modules/";
    prefix.append(moduleId).append("/");

    unz_global_info64 global{};
    bool success = unzGetGlobalInfo64(archive, &global) == UNZ_OK;
    if (!success)
        error = "package is not a ZIP archive";

    int step = success && global.number_entry ? unzGoToFirstFile(archive) : UNZ_END_OF_LIST_OF_FILE;
    for (ZPOS64_T index = 0; success && index != global.number_entry; ++index)
    {
        unz_file_info64 info{};
        std::array<char, 1024> rawName{};
        success = step == UNZ_OK &&
            unzGetCurrentFileInfo64(archive, &info, rawName.data(), static_cast<uLong>(rawName.size() - 1), nullptr, 0,
                nullptr, 0) == UNZ_OK &&
            info.size_filename < rawName.size();
        if (!success)
        {
            error = "damaged package directory";
            break;
        }

        xr_string name(rawName.data());
        std::ranges::replace(name, '\\', '/');
        const bool folder = !name.empty() && name.back() == '/';
        if (folder)
            name.pop_back();

        // "modules" and "modules/<id>" themselves are legitimate folder entries
        const std::string_view prefixFolder(prefix.data(), prefix.size() - 1);
        const bool container = folder &&
            (0 == xr_stricmp(name.c_str(), "modules") || 0 == xr_stricmp(name.c_str(), xr_string(prefixFolder).c_str()));
        if (!container)
        {
            const bool ascii = std::ranges::all_of(name, [](char c) { return static_cast<u8>(c) < 0x80; });
            const bool inside = name.size() > prefix.size() && 0 == _strnicmp(name.c_str(), prefix.c_str(), prefix.size());
            const std::string_view relative = inside ? std::string_view(name).substr(prefix.size()) : std::string_view();
            const std::wstring wide = mod_utf8_to_wide(relative);
            if (!inside || !SafeRelativePath(relative) || wide.empty() || (!ascii && !(info.flag & (1u << 11))))
            {
                error = inside ? "package holds an unsafe path" : "package holds a path outside modules/<id>/";
                success = false;
                break;
            }
            if (info.flag & 1u)
            {
                error = "package is encrypted";
                success = false;
                break;
            }

            std::filesystem::path target = destination / std::filesystem::path(wide).make_preferred();
            if (folder)
            {
                std::error_code directoryError;
                std::filesystem::create_directories(target, directoryError);
                success = !directoryError;
                if (!success)
                    error = "could not create a folder of the package";
            }
            else
            {
                ++totals.files;
                totals.bytes += info.uncompressed_size;
                if ((fileLimit && totals.files > fileLimit) || (byteLimit && totals.bytes > byteLimit))
                {
                    error = "release is larger than its descriptor declares";
                    success = false;
                    break;
                }
                success = mod_write_entry(archive, target, info.uncompressed_size, cancel, error);
            }
        }

        if (success && index + 1 != global.number_entry)
            step = unzGoToNextFile(archive);
    }

    unzClose(archive);
    return success;
}

bool ModRelease::ReadManifestIdentity(const std::filesystem::path& manifest, xr_string& id, xr_string& version)
{
    std::ifstream file(manifest, std::ios::binary);
    if (!file)
        return false;
    xr_string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    id.clear();
    version.clear();
    mod_parse_ini(text, [&](std::string_view section, std::string_view key, std::string_view value)
    {
        if (section != "module")
            return;
        if (key == "id")
        {
            id = value;
            xr_strlwr(id);
        }
        else if (key == "version")
            version = value;
    });
    return !id.empty();
}
#endif
