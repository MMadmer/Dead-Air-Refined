// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "ModRelease.h"

#ifdef XR_PLATFORM_WINDOWS
#include "xrContentSync/ContentHash.h"

#include <zlib.h>

#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <vector>

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

std::string_view mod_skip_bom(std::string_view text)
{
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    return text;
}

// The first ';' outside double quotes - a quoted value is prose and may hold one.
size_t mod_comment_start(std::string_view line)
{
    bool quoted = false;
    for (size_t i = 0; i != line.size(); ++i)
    {
        if (quoted && line[i] == '\\' && i + 1 < line.size())
            ++i;
        else if (line[i] == '"')
            quoted = !quoted;
        else if (line[i] == ';' && !quoted)
            return i;
    }
    return std::string_view::npos;
}

// MOD_UPDATES 1.1: a value in double quotes knows \n, \" and \\; anything else is taken as written.
xr_string mod_text(std::string_view value)
{
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return xr_string(value);
    value = value.substr(1, value.size() - 2);
    xr_string out;
    out.reserve(value.size());
    for (size_t i = 0; i != value.size(); ++i)
    {
        if (value[i] == '\\' && i + 1 < value.size())
        {
            const char next = value[++i];
            out += next == 'n' ? '\n' : next;
        }
        else
            out += value[i];
    }
    return out;
}

// callback(section, key, value); sections arrive lowercased, ';' starts a comment
template <typename Callback>
void mod_parse_ini(std::string_view text, const Callback& callback)
{
    text = mod_skip_bom(text);
    xr_string section;
    while (!text.empty())
    {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

        line = mod_trim(line.substr(0, mod_comment_start(line)));
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

bool mod_valid_asset_name(std::string_view name, std::string_view extension)
{
    if (name.size() <= extension.size() || name.size() > 128 || name.front() == '.' || !mod_all_of(name, "-._"))
        return false;
    const std::string_view tail = name.substr(name.size() - extension.size());
    return std::ranges::equal(tail, extension, [](char a, char b) { return (a | 0x20) == b; });
}

bool mod_valid_digest(xr_string& digest)
{
    xr_strlwr(digest);
    return digest.size() == 64 &&
        std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// "<name>, <size>, <sha256>" or, for a [packages] line, "<size>, <sha256>" behind its key
bool mod_parse_asset(std::string_view name, std::string_view sizeAndDigest, std::string_view extension,
    ModRelease::Asset& asset)
{
    const size_t comma = sizeAndDigest.find(',');
    if (comma == std::string_view::npos)
        return false;
    asset.name = name;
    asset.sha256 = mod_trim(sizeAndDigest.substr(comma + 1));
    return mod_parse_number(mod_trim(sizeAndDigest.substr(0, comma)), asset.size) && asset.size &&
        mod_valid_digest(asset.sha256) && mod_valid_asset_name(name, extension);
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

bool mod_valid_utf8(std::string_view text)
{
    return text.empty() ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

// next space-separated field of an index record; empty when the line has run out
std::string_view mod_next_field(std::string_view& line)
{
    const size_t space = line.find(' ');
    const std::string_view field = line.substr(0, space);
    line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
    return field;
}
}

namespace
{
// "<path>, <size>, <sha256>"
bool mod_parse_file_ref(std::string_view value, ModRelease::FileRef& out)
{
    const size_t first = value.find(',');
    const size_t second = first == std::string_view::npos ? first : value.find(',', first + 1);
    if (second == std::string_view::npos)
        return false;
    out.path = mod_trim(value.substr(0, first));
    out.sha256 = mod_trim(value.substr(second + 1));
    out.present = mod_parse_number(mod_trim(value.substr(first + 1, second - first - 1)), out.size) &&
        mod_valid_digest(out.sha256) && ModRelease::SafeRelativePath(out.path);
    return out.present;
}

void mod_parse_card_line(std::string_view key, std::string_view value, ModRelease::Card& card)
{
    card.present = true;
    if (key == "name")
        card.name = mod_text(value);
    else if (key == "name@eng")
        card.nameEng = mod_text(value);
    else if (key == "author")
        card.author = mod_text(value);
    else if (key == "author@eng")
        card.authorEng = mod_text(value);
    else if (key == "description")
        card.description = mod_text(value);
    else if (key == "description@eng")
        card.descriptionEng = mod_text(value);
    else if (key == "website")
        card.website = value;
    else if (key == "mode")
        card.mode = value;
    else if (key == "mode_title")
        card.modeTitle = mod_text(value);
    else if (key == "mode_title@eng")
        card.modeTitleEng = mod_text(value);
    else if (key == "tags")
    {
        const auto& vocabulary = ModRelease::TagVocabulary();
        std::string_view rest = value;
        while (!rest.empty())
        {
            const size_t comma = rest.find(',');
            xr_string tag(mod_trim(rest.substr(0, comma)));
            xr_strlwr(tag);
            rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
            if (std::ranges::find(vocabulary, tag) != vocabulary.end() && std::ranges::find(card.tags, tag) == card.tags.end())
                card.tags.push_back(tag);
        }
    }
    else if (key == "thumbnail")
        mod_parse_file_ref(value, card.thumbnail);
    else if (key == "manifest")
        mod_parse_file_ref(value, card.manifest);
}
}

const xr_vector<xr_string>& ModRelease::TagVocabulary()
{
    static const xr_vector<xr_string> tags = {"story", "quests", "gameplay", "weapons", "equipment", "npcs",
        "mutants", "locations", "graphics", "audio", "interface", "fixes"};
    return tags;
}

bool ModRelease::CatalogPathAllowed(std::string_view path, xr_string& why)
{
    if (!SafeRelativePath(path))
    {
        why = "unsafe path";
        return false;
    }
    for (std::string_view rest = path; !rest.empty();)
    {
        const size_t slash = rest.find('/');
        if (rest.front() == '.')
        {
            why = "a dot-named file or folder";
            return false;
        }
        rest.remove_prefix(slash == std::string_view::npos ? rest.size() : slash + 1);
    }
    const size_t slash = path.rfind('/');
    const std::string_view name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return true;
    xr_string extension(name.substr(dot + 1));
    xr_strlwr(extension);
    static constexpr std::string_view allowed[] = {"ltx", "ltxp", "xml", "xmlp", "script", "txt", "md", "json",
        "csv", "ini", "cfg", "seq", "nqasset", "behasset", "pdaasset", "xspawn", "xcform", "dds", "thm", "ogg", "ogm",
        "ogf", "omf", "skl", "skls", "anm", "ppe", "xr", "ps", "vs", "gs", "hs", "ds", "cs", "h", "hlsl", "s", "spawn",
        "graph", "ai", "gct", "cform", "details", "env_mod", "fog_vol", "game", "geom", "geomx", "hom", "ps_static",
        "snd_env", "snd_static", "som", "sectors", "wallmarks", "lanims"};
    if (std::ranges::find(allowed, std::string_view(extension)) != std::end(allowed))
        return true;
    why = "." + extension + " files are not allowed in a catalog module";
    return false;
}

bool ModRelease::CatalogFileAllowed(const std::filesystem::path& file, xr_string& why)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        why = "cannot read the file";
        return false;
    }
    std::array<char, 64> head{};
    stream.read(head.data(), head.size());
    const size_t got = static_cast<size_t>(stream.gcount());
    const std::string_view bytes(head.data(), got);
    const auto starts = [&bytes](std::string_view magic) { return bytes.starts_with(magic); };

    // MZ alone is two letters; a program is MZ whose e_lfanew points at a PE header
    if (starts("MZ") && got >= 0x40)
    {
        u32 offset = 0;
        std::memcpy(&offset, head.data() + 0x3C, sizeof(offset));
        char signature[4]{};
        stream.clear();
        stream.seekg(offset);
        stream.read(signature, 4);
        if (stream.gcount() == 4 && std::string_view(signature, 4) == std::string_view("PE\0\0", 4))
        {
            why = "a Windows program or library";
            return false;
        }
    }
    struct Magic
    {
        std::string_view bytes;
        pcstr what;
    };
    static const Magic magics[] = {
        {std::string_view("\x7F" "ELF", 4), "a Linux program"},
        {std::string_view("\xFE\xED\xFA\xCE", 4), "a macOS program"},
        {std::string_view("\xFE\xED\xFA\xCF", 4), "a macOS program"},
        {std::string_view("\xCE\xFA\xED\xFE", 4), "a macOS program"},
        {std::string_view("\xCF\xFA\xED\xFE", 4), "a macOS program"},
        {std::string_view("\xCA\xFE\xBA\xBE", 4), "a Java class or a universal binary"},
        {std::string_view("PK\x03\x04", 4), "a ZIP archive"},
        {std::string_view("PK\x05\x06", 4), "a ZIP archive"},
        {std::string_view("Rar!\x1A\x07", 6), "a RAR archive"},
        {std::string_view("7z\xBC\xAF\x27\x1C", 6), "a 7-Zip archive"},
        {std::string_view("MSCF", 4), "a CAB archive"},
        {std::string_view("\x1F\x8B", 2), "a GZIP archive"},
        {std::string_view("\xFD" "7zXZ", 5), "an XZ archive"},
        {std::string_view("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8), "an OLE container"},
        {std::string_view("L\0\0\0\x01\x14\x02\0", 8), "a Windows shortcut"},
        {std::string_view("\0asm", 4), "WebAssembly"},
        {std::string_view("\x1BLua", 4), "Lua bytecode"},
        {std::string_view("\x1BLJ", 3), "LuaJIT bytecode"},
    };
    for (const Magic& magic : magics)
        if (starts(magic.bytes))
        {
            why = magic.what;
            return false;
        }
    return true;
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

u64 ModRelease::Descriptor::PackageBytes() const
{
    u64 total = 0;
    for (const Asset& package : packages)
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
    bool indexed = false;
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
            else if (key == "index")
            {
                const size_t comma = value.find(',');
                indexed = comma != std::string_view::npos &&
                    mod_parse_asset(mod_trim(value.substr(0, comma)), value.substr(comma + 1), ".files", out.index);
                malformed |= !indexed;
            }
        }
        else if (section == "packages")
        {
            Asset package;
            malformed |= !mod_parse_asset(key, value, ".zip", package);
            out.packages.emplace_back(std::move(package));
        }
        else if (section == "card")
            mod_parse_card_line(key, value, out.card);
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
    if (!indexed || out.packages.empty() || out.packages.size() > MaximumPackages ||
        out.index.size > MaximumIndexBytes)
    {
        error = "descriptor names no file index or no package";
        return false;
    }
    for (size_t index = 0; index != out.packages.size(); ++index)
    {
        const bool duplicate = std::ranges::any_of(out.packages.begin(), out.packages.begin() + index,
            [&](const Asset& other) { return 0 == xr_stricmp(other.name.c_str(), out.packages[index].name.c_str()); });
        if (duplicate)
        {
            error = "descriptor names a package twice";
            return false;
        }
    }
    return true;
}

bool ModRelease::SafeRelativePath(std::string_view path)
{
    if (path.empty() || path.size() > 1024)
        return false;
    while (!path.empty())
    {
        const size_t slash = path.find('/');
        const std::string_view component = path.substr(0, slash);
        // a trailing slash would leave an empty last component: a file path has none
        if (slash != std::string_view::npos && slash + 1 == path.size())
            return false;
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

bool ModRelease::ParseFileIndex(std::string_view text, const Descriptor& descriptor, FileIndex& out, xr_string& error)
{
    out = {};
    text = mod_skip_bom(text);

    xr_vector<u32> packs; // pack number - 1 -> index into descriptor.packages
    bool headed = false;
    while (!text.empty())
    {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;

        const std::string_view keyword = mod_next_field(line);
        if (!headed)
        {
            u64 version = 0;
            if (keyword != "xms-files" || !mod_parse_number(mod_next_field(line), version) || !version)
            {
                error = "file index has no header";
                return false;
            }
            if (version > SupportedIndexVersion)
            {
                error = "file index is newer than this game";
                return false;
            }
            headed = true;
        }
        else if (keyword == "pack")
        {
            const auto package = std::ranges::find(descriptor.packages, xr_string(line), &Asset::name);
            const u32 position = static_cast<u32>(package - descriptor.packages.begin());
            if (package == descriptor.packages.end() || std::ranges::find(packs, position) != packs.end())
            {
                error = "file index names a package the descriptor does not";
                return false;
            }
            packs.push_back(position);
        }
        else if (keyword == "file")
        {
            FileEntry entry;
            entry.sha256 = mod_next_field(line);
            u64 pack = 0, method = 0;
            const bool numbers = mod_parse_number(mod_next_field(line), entry.size) &&
                mod_parse_number(mod_next_field(line), pack) && mod_parse_number(mod_next_field(line), entry.offset) &&
                mod_parse_number(mod_next_field(line), entry.packed) && mod_parse_number(mod_next_field(line), method);
            entry.path = line;
            if (!numbers || !mod_valid_digest(entry.sha256) || !pack || pack > packs.size() || (method != 0 && method != 8) ||
                !SafeRelativePath(entry.path) || !mod_valid_utf8(entry.path))
            {
                error = "file index holds a malformed or unsafe record";
                return false;
            }
            entry.package = packs[pack - 1];
            entry.deflated = method == 8;
            const u64 packageSize = descriptor.packages[entry.package].size;
            if (entry.offset > packageSize || entry.packed > packageSize - entry.offset ||
                (!entry.deflated && entry.packed != entry.size))
            {
                error = "file index points outside its package";
                return false;
            }
            out.unpacked += entry.size;
            out.files.emplace_back(std::move(entry));
        }
    }

    // the file system would not tell two spellings of one name apart, so neither does this
    xr_vector<xr_string> names;
    names.reserve(out.files.size());
    for (const FileEntry& entry : out.files)
        xr_strlwr(names.emplace_back(entry.path));
    std::ranges::sort(names);
    if (std::ranges::adjacent_find(names) != names.end())
    {
        error = "file index names a path twice";
        return false;
    }
    if (!std::ranges::binary_search(names, xr_string("mod.ltx")))
    {
        error = "file index holds no mod.ltx";
        return false;
    }
    if ((descriptor.files && descriptor.files != out.files.size()) ||
        (descriptor.unpacked && descriptor.unpacked != out.unpacked))
    {
        error = "file index does not match the totals of its descriptor";
        return false;
    }
    return true;
}

struct ModRelease::FileSink::Impl
{
    HANDLE file{INVALID_HANDLE_VALUE};
    std::filesystem::path path;
    ContentHash::Stream digest;
    z_stream stream{};
    bool inflating{};
    bool ended{};
    xr_string sha256;
    u64 size{};
    u64 written{};
    u64 packedLeft{};
    std::vector<u8> buffer;
};

ModRelease::FileSink::FileSink() : m_impl(std::make_unique<Impl>()) {}

ModRelease::FileSink::~FileSink() { Discard(); }

void ModRelease::FileSink::Discard()
{
    Impl& impl = *m_impl;
    if (impl.inflating)
        inflateEnd(&impl.stream);
    impl.inflating = false;
    if (impl.file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(impl.file);
        DeleteFileW(impl.path.c_str());
    }
    impl.file = INVALID_HANDLE_VALUE;
}

bool ModRelease::FileSink::Open(const std::filesystem::path& target, const FileEntry& entry, xr_string& error)
{
    Discard();
    Impl& impl = *m_impl;
    impl.path = target;
    impl.sha256 = entry.sha256;
    impl.size = entry.size;
    impl.written = 0;
    impl.packedLeft = entry.packed;
    impl.ended = !entry.deflated;

    std::error_code directoryError;
    std::filesystem::create_directories(target.parent_path(), directoryError);
    impl.digest = ContentHash::Stream();
    if (directoryError || !impl.digest.Open())
    {
        error = "could not create a folder of the module";
        return false;
    }
    if (entry.deflated)
    {
        impl.stream = {};
        // raw deflate, as a ZIP entry stores it
        if (inflateInit2(&impl.stream, -MAX_WBITS) != Z_OK)
        {
            error = "could not start unpacking";
            return false;
        }
        impl.inflating = true;
        impl.buffer.resize(256 * 1024);
    }

    impl.file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (impl.file == INVALID_HANDLE_VALUE)
    {
        error = GetLastError() == ERROR_FILE_EXISTS ? "the release names a path twice" : "could not create a module file";
        return false;
    }
    return true;
}

bool ModRelease::FileSink::Append(const void* data, size_t size, xr_string& error)
{
    Impl& impl = *m_impl;
    const auto write = [&](const void* bytes, size_t count)
    {
        DWORD written = 0;
        impl.written += count;
        return impl.written <= impl.size && impl.digest.Append(bytes, count) &&
            WriteFile(impl.file, bytes, static_cast<DWORD>(count), &written, nullptr) && written == count;
    };

    if (size > impl.packedLeft)
    {
        error = "a file is larger than the index declares";
        return false;
    }
    impl.packedLeft -= size;

    if (!impl.inflating)
    {
        if (write(data, size))
            return true;
        error = "could not write a module file";
        return false;
    }

    impl.stream.next_in = const_cast<Bytef*>(static_cast<const Bytef*>(data));
    impl.stream.avail_in = static_cast<uInt>(size);
    while (impl.stream.avail_in && !impl.ended)
    {
        impl.stream.next_out = impl.buffer.data();
        impl.stream.avail_out = static_cast<uInt>(impl.buffer.size());
        const int result = inflate(&impl.stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END)
        {
            error = "a file of the package is damaged";
            return false;
        }
        if (!write(impl.buffer.data(), impl.buffer.size() - impl.stream.avail_out))
        {
            error = impl.written > impl.size ? "a file is larger than the index declares" : "could not write a module file";
            return false;
        }
        impl.ended = result == Z_STREAM_END;
    }
    if (impl.stream.avail_in)
    {
        error = "a file of the package is damaged";
        return false;
    }
    return true;
}

bool ModRelease::FileSink::Finish(xr_string& error)
{
    Impl& impl = *m_impl;
    const bool whole = impl.file != INVALID_HANDLE_VALUE && !impl.packedLeft && impl.ended && impl.written == impl.size &&
        impl.digest.Finish() == impl.sha256.c_str();
    if (!whole)
    {
        error = "a file does not match the index";
        Discard();
        return false;
    }
    if (impl.inflating)
        inflateEnd(&impl.stream);
    impl.inflating = false;
    CloseHandle(impl.file);
    impl.file = INVALID_HANDLE_VALUE;
    return true;
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
