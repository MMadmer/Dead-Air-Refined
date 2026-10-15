// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_catalog.h"
#include "xrCore/LocatorAPI.h"

#include <bcrypt.h>

#include <algorithm>
#include <mutex>

namespace XMS
{
namespace
{
// ---- small text helpers ----------------------------------------------------------------------

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

xr_string lower(std::string_view text)
{
    xr_string out(text);
    for (char& c : out)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
        std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(u8(x)) == tolower(u8(y)); });
}

xr_vector<std::string_view> split_commas(std::string_view text)
{
    xr_vector<std::string_view> out;
    while (true)
    {
        const size_t comma = text.find(',');
        out.push_back(trim(text.substr(0, comma)));
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix(comma + 1);
    }
    return out;
}

bool parse_u64(std::string_view text, u64& out)
{
    text = trim(text);
    if (text.empty() || text.size() > 19)
        return false;
    u64 value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + u64(c - '0');
    }
    out = value;
    return true;
}

bool module_id_ok(std::string_view id)
{
    if (id.empty() || id.size() > 64)
        return false;
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

// One to four numbers, the MOD_UPDATES 1.2 ordering; anything after the numbers is ignored.
struct Version
{
    u32 parts[4]{};
    bool valid{};
};

Version parse_version(std::string_view text)
{
    Version v;
    text = trim(text);
    size_t part = 0;
    size_t i = 0;
    while (part < 4 && i < text.size() && text[i] >= '0' && text[i] <= '9')
    {
        u32 value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
            value = value * 10 + u32(text[i++] - '0');
        v.parts[part++] = value;
        v.valid = true;
        if (i < text.size() && text[i] == '.' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9')
            ++i;
        else
            break;
    }
    return v;
}

bool same_version(std::string_view a, std::string_view b)
{
    const Version x = parse_version(a), y = parse_version(b);
    return x.valid && y.valid && std::equal(std::begin(x.parts), std::end(x.parts), std::begin(y.parts));
}

// Walks an ini text: callback(section, key, value), section in lower case. A ';' starts a
// comment only at the start of a line - reasons and mirror lists are prose.
template <typename Callback>
void walk_ini(std::string_view text, const Callback& cb)
{
    xr_string section;
    while (!text.empty())
    {
        const size_t end = text.find('\n');
        std::string_view line = trim(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.empty() || line.front() == ';')
            continue;
        if (line.front() == '[')
        {
            const size_t close = line.find(']');
            if (close != std::string_view::npos)
                section = lower(line.substr(1, close - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = trim(line.substr(0, eq));
        if (!key.empty())
            cb(section, key, trim(line.substr(eq + 1)));
    }
}

xr_string hex(const u8* data, size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    xr_string out(size * 2, '0');
    for (size_t i = 0; i != size; ++i)
    {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    return out;
}

// ---- files -----------------------------------------------------------------------------------

bool read_file(const xr_string& path, size_t limit, xr_string& out)
{
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && u64(size.QuadPart) <= limit;
    if (ok)
    {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(file, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(file);
    return ok;
}

// Written beside and moved over, so a crash leaves the old file or the new one.
bool write_file(const xr_string& path, std::string_view data)
{
    const size_t slash = path.find_last_of("\\/");
    if (slash != xr_string::npos)
        CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
    const xr_string temporary = path + ".tmp";
    const HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = data.empty() || (WriteFile(file, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size());
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(temporary.c_str());
        return false;
    }
    return true;
}

// ---- state -----------------------------------------------------------------------------------

struct CatalogState
{
    std::mutex lock;
    bool evaluated{};
    xr_string cacheRoot; // modules\.catalog\ with the delimiter
    u64 serial{};
    bool latched{};
    xr_vector<std::pair<xr_string, xr_string>> installed; // id, date
    std::shared_ptr<const Catalog::Index> current;

    bool protectedMode{};
    xr_string protectedReason;
    bool catalogEnabled{};

    // real, lower case, with a trailing delimiter
    xr_string gameRoot;
    xr_string appDataRoot;
    xr_vector<xr_string> writeDenied;
};

CatalogState& cst()
{
    static CatalogState state;
    return state;
}

void save_state_locked(CatalogState& s)
{
    if (s.cacheRoot.empty())
        return;
    xr_string text = "; Written by the game. MOD_CATALOG.md 9.1 and 14.\n[catalog]\n";
    text += "serial = " + xr_string(std::to_string(s.serial).c_str()) + "\n";
    text += s.latched ? "protect = 1\n" : "protect = 0\n";
    text += "\n[installed]\n";
    for (const auto& [id, date] : s.installed)
        text += id + " = " + date + "\n";
    if (!write_file(s.cacheRoot + "state.ltx", text))
        Msg("! XMS catalog: could not write %sstate.ltx", s.cacheRoot.c_str());
}

void load_state_locked(CatalogState& s)
{
    xr_string text;
    if (!read_file(s.cacheRoot + "state.ltx", 64 * 1024, text))
        return;
    walk_ini(text, [&s](const xr_string& section, std::string_view key, std::string_view value) {
        if (section == "catalog")
        {
            if (key == "serial")
                parse_u64(value, s.serial);
            else if (key == "protect")
                s.latched = value == "1";
        }
        else if (section == "installed" && module_id_ok(key))
            s.installed.emplace_back(xr_string(key), xr_string(value));
    });
}

xr_string today()
{
    SYSTEMTIME now{};
    GetSystemTime(&now);
    string32 text{};
    xr_sprintf(text, "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    return text;
}

// ---- paths for protected mode ------------------------------------------------------------------

bool reserved_name(std::string_view component)
{
    const size_t dot = component.find('.');
    const xr_string base = lower(component.substr(0, dot));
    static constexpr pcstr names[] = {"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"};
    for (pcstr name : names)
        if (base == name)
            return true;
    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9')
        return true;
    return false;
}

// The real, lower-case path: absolute, through every link, with UNC, device paths, streams
// and device names refused on the way.
bool real_path(pcstr path, xr_string& out, xr_string& why)
{
    if (!path || !*path)
    {
        why = "empty path";
        return false;
    }
    if ((path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/'))
    {
        why = "network or device path";
        return false;
    }
    char full[1024]{};
    const DWORD length = GetFullPathNameA(path, sizeof(full), full, nullptr);
    if (!length || length >= sizeof(full))
    {
        why = "path cannot be resolved";
        return false;
    }
    xr_string absolute(full, length);
    if (absolute.size() < 3 || absolute[1] != ':' || absolute.find(':', 2) != xr_string::npos)
    {
        why = absolute.find(':', 2) != xr_string::npos ? "alternate data stream" : "not a local path";
        return false;
    }
    for (size_t start = 3; start < absolute.size();)
    {
        const size_t end = absolute.find('\\', start);
        const std::string_view component(absolute.data() + start, (end == xr_string::npos ? absolute.size() : end) - start);
        if (!component.empty() && reserved_name(component))
        {
            why = "device name";
            return false;
        }
        if (end == xr_string::npos)
            break;
        start = end + 1;
    }

    // the deepest part that exists is asked for its real location; what does not exist yet
    // cannot be a link and is appended as it is
    xr_string existing = absolute;
    xr_string tail;
    while (GetFileAttributesA(existing.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        const size_t slash = existing.find_last_of('\\');
        if (slash == xr_string::npos || slash < 2)
        {
            why = "path cannot be resolved";
            return false;
        }
        tail = existing.substr(slash) + tail;
        existing.resize(slash == 2 ? 3 : slash);
    }
    const HANDLE handle = CreateFileA(existing.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        why = "path cannot be opened";
        return false;
    }
    char resolved[1024]{};
    const DWORD got = GetFinalPathNameByHandleA(handle, resolved, sizeof(resolved), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (!got || got >= sizeof(resolved))
    {
        why = "path cannot be resolved";
        return false;
    }
    std::string_view real(resolved, got);
    if (real.starts_with("\\\\?\\UNC\\"))
    {
        why = "network path";
        return false;
    }
    if (real.starts_with("\\\\?\\"))
        real.remove_prefix(4);
    out = lower(real);
    if (!out.empty() && out.back() == '\\' && !tail.empty())
        out.pop_back();
    out += lower(tail);
    return true;
}

bool under(const xr_string& path, const xr_string& root) { return !root.empty() && path.starts_with(root); }

xr_string real_root(pcstr path)
{
    xr_string out, why;
    if (!real_path(path, out, why))
        return {};
    if (!out.empty() && out.back() != '\\')
        out += '\\';
    return out;
}
} // namespace

// ---- crypto ------------------------------------------------------------------------------------

namespace Catalog
{
bool Sha256(const void* data, size_t size, u8 digest[32])
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    const u8* bytes = static_cast<const u8*>(data);
    while (ok && size)
    {
        const ULONG piece = ULONG(std::min<size_t>(size, 1u << 30));
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(bytes), piece, 0));
        bytes += piece;
        size -= piece;
    }
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, 32, 0));
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

xr_string Sha256Hex(const void* data, size_t size)
{
    u8 digest[32];
    return Sha256(data, size, digest) ? hex(digest, 32) : xr_string();
}

bool Base64UrlDecode(std::string_view text, xr_vector<u8>& out)
{
    out.clear();
    while (!text.empty() && text.back() == '=')
        text.remove_suffix(1);
    u32 buffer = 0;
    int bits = 0;
    for (const char c : text)
    {
        int value;
        if (c >= 'A' && c <= 'Z')
            value = c - 'A';
        else if (c >= 'a' && c <= 'z')
            value = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            value = c - '0' + 52;
        else if (c == '-')
            value = 62;
        else if (c == '_')
            value = 63;
        else
            return false;
        buffer = (buffer << 6) | u32(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(u8(buffer >> bits));
        }
    }
    // leftover bits must be the zero padding of the last character
    return bits < 6 && !(buffer & ((1u << bits) - 1));
}

xr_string Base64UrlEncode(const u8* data, size_t size)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    xr_string out;
    u32 buffer = 0;
    int bits = 0;
    for (size_t i = 0; i != size; ++i)
    {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 6)
        {
            bits -= 6;
            out += alphabet[(buffer >> bits) & 63];
        }
    }
    if (bits)
        out += alphabet[(buffer << (6 - bits)) & 63];
    return out;
}

bool ParsePublicKey(std::string_view text, PublicKey& out)
{
    out = {};
    text = trim(text);
    if (!text.starts_with("p256:"))
        return false;
    xr_vector<u8> bytes;
    if (!Base64UrlDecode(text.substr(5), bytes) || bytes.size() != 64)
        return false;
    std::copy(bytes.begin(), bytes.end(), out.xy);
    u8 digest[32];
    if (!Sha256(out.xy, 64, digest))
        return false;
    out.id = hex(digest, 8);
    out.valid = true;
    return true;
}

bool VerifyP256(const PublicKey& key, const void* message, size_t size, const u8 signature[64])
{
    if (!key.valid)
        return false;
    u8 digest[32];
    if (!Sha256(message, size, digest))
        return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    struct
    {
        BCRYPT_ECCKEY_BLOB header;
        u8 xy[64];
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    std::copy(std::begin(key.xy), std::end(key.xy), blob.xy);
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &handle,
        reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0));
    if (ok)
    {
        ok = BCRYPT_SUCCESS(
            BCryptVerifySignature(handle, nullptr, digest, 32, const_cast<PUCHAR>(signature), 64, 0));
        BCryptDestroyKey(handle);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

// ---- signed text -------------------------------------------------------------------------------

bool SplitSigned(std::string_view text, Signed& out, xr_string& error)
{
    out = {};
    constexpr std::string_view header = "[signature]";
    size_t at = std::string_view::npos;
    for (size_t from = 0; (from = text.find(header, from)) != std::string_view::npos; from += header.size())
    {
        if (from == 0 || text[from - 1] != '\n')
            continue;
        const std::string_view rest = text.substr(from + header.size());
        if (!rest.empty() && rest.front() != '\n' && rest.front() != '\r')
            continue;
        if (at != std::string_view::npos)
        {
            error = "more than one [signature] section";
            return false;
        }
        at = from;
    }
    if (at == std::string_view::npos)
    {
        error = "not signed";
        return false;
    }
    out.body = text.substr(0, at);

    bool bad = false;
    xr_string sigText;
    std::string_view rest = text.substr(at + header.size());
    while (!rest.empty())
    {
        const size_t end = rest.find('\n');
        const std::string_view line = trim(rest.substr(0, end));
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (line.empty())
            continue;
        const size_t eq = line.find('=');
        const std::string_view key = eq == std::string_view::npos ? line : trim(line.substr(0, eq));
        const std::string_view value = eq == std::string_view::npos ? std::string_view{} : trim(line.substr(eq + 1));
        if (key == "key" && out.keyText.empty())
            out.keyText = value;
        else if (key == "sig" && sigText.empty())
            sigText = value;
        else
            bad = true;
    }
    const size_t colon = sigText.find(':');
    xr_vector<u8> bytes;
    if (bad || colon != 16 || !Base64UrlDecode(std::string_view(sigText).substr(colon + 1), bytes) || bytes.size() != 64)
    {
        error = bad ? "unexpected content after [signature]" : "malformed signature";
        return false;
    }
    out.keyId = lower(std::string_view(sigText).substr(0, colon));
    std::copy(bytes.begin(), bytes.end(), out.signature);
    return true;
}

bool VerifySigned(std::string_view text, const PublicKey& key, xr_string& error)
{
    Signed parts;
    if (!SplitSigned(text, parts, error))
        return false;
    if (parts.keyId != key.id)
    {
        error = "signed by key " + parts.keyId + ", expected " + key.id;
        return false;
    }
    if (!VerifyP256(key, parts.body.data(), parts.body.size(), parts.signature))
    {
        error = "signature does not match";
        return false;
    }
    return true;
}

// ---- keys and mirrors ------------------------------------------------------------------------

namespace
{
// The maintainer keys of the Dead Air: Refined mod catalog. A key here is trusted to decide
// what the Mod Browser lists; changing this list is a game release (MOD_CATALOG 5.2).
constexpr pcstr kMaintainerKeys[] = {
    // cbb2fc6aba32e6fd, made 2026-09-25
    "p256:YSQfOZfLMyrjfNHP2RQNBeWzNSKH3yOqSfVCNb5ufluCjgrWrZMjbFAPTERwToM_MyC5zsQ38PLyP9yC8fcFdg",
};

constexpr pcstr kMirrors[] = {
    "https://mmadmer.github.io/darf-mods/",
    "https://cdn.jsdelivr.net/gh/MMadmer/darf-mods@main/public/",
    "https://raw.githubusercontent.com/MMadmer/darf-mods/main/public/",
};

xr_string environment(pcstr name)
{
    char value[1024]{};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length && length < sizeof(value) ? xr_string(value, length) : xr_string();
}
} // namespace

bool QaMode() { return strstr(Core.Params, "-qa_catalog") != nullptr; }

const xr_vector<PublicKey>& MaintainerKeys()
{
    static const xr_vector<PublicKey> keys = [] {
        xr_vector<PublicKey> out;
        const auto add = [&out](std::string_view text) {
            PublicKey key;
            if (ParsePublicKey(text, key))
                out.push_back(key);
        };
        if (QaMode())
            add(environment("DAR_QA_CATALOG_KEY"));
        else
            for (pcstr text : kMaintainerKeys)
                add(text);
        return out;
    }();
    return keys;
}

const xr_vector<xr_string>& BuiltInMirrors()
{
    static const xr_vector<xr_string> mirrors = [] {
        xr_vector<xr_string> out;
        if (QaMode())
        {
            // loopback only: the QA override redirects where bytes come from, nothing else
            const xr_string base = environment("DAR_QA_CATALOG_BASE");
            if ((base.starts_with("http://127.0.0.1:") || base.starts_with("http://localhost:")) && base.back() == '/')
                out.push_back(base);
        }
        else
            for (pcstr mirror : kMirrors)
                out.emplace_back(mirror);
        return out;
    }();
    return mirrors;
}

// ---- the index ---------------------------------------------------------------------------------

const Listing* Index::Find(std::string_view id) const
{
    for (const Listing& listing : mods)
        if (listing.id == id)
            return &listing;
    return nullptr;
}

const Listing* Index::FindWithdrawn(std::string_view id) const
{
    for (const Listing& listing : withdrawn)
        if (listing.id == id)
            return &listing;
    return nullptr;
}

const Revocation* Index::Revoked(std::string_view id, std::string_view version, std::string_view keyId) const
{
    for (const Revocation& r : revoked)
    {
        const bool subject = r.subject == id || (!keyId.empty() && r.subject.starts_with("key:") &&
            std::string_view(r.subject).substr(4) == keyId);
        if (subject && (r.version == "*" || same_version(r.version, version)))
            return &r;
    }
    return nullptr;
}

namespace
{
bool mirror_ok(std::string_view url)
{
    return url.starts_with("https://") && url.size() > 9 && url.size() <= 256 && url.back() == '/' &&
        url.find_first_of(" \t\"<>\\") == std::string_view::npos;
}

bool parse_index_body(std::string_view text, Index& out, xr_string& error)
{
    out = {};
    bool bad = false;
    walk_ini(text, [&](const xr_string& section, std::string_view key, std::string_view value) {
        if (section == "catalog")
        {
            u64 number = 0;
            if (key == "format" && parse_u64(value, number))
                out.format = u32(number);
            else if (key == "serial")
                parse_u64(value, out.serial);
            else if (key == "issued")
                out.issued = value;
            else if (key == "mirrors")
                for (std::string_view mirror : split_commas(value))
                    if (mirror_ok(mirror))
                        out.mirrors.emplace_back(mirror);
        }
        else if (section == "virustotal")
        {
            u64 number = 0;
            if (key == "trusted")
            {
                for (std::string_view name : split_commas(value))
                    if (!name.empty())
                        out.vt.trusted.push_back(lower(name));
            }
            else if (parse_u64(value, number) && number < 1000)
            {
                if (key == "block_trusted")
                    out.vt.blockTrusted = u32(number);
                else if (key == "warn_trusted")
                    out.vt.warnTrusted = u32(number);
                else if (key == "block_others")
                    out.vt.blockOthers = u32(number);
                else if (key == "warn_others")
                    out.vt.warnOthers = u32(number);
            }
        }
        else if (section == "reviews")
        {
            u64 number = 0;
            // the QA inbox is a loopback mock (-qa_catalog), like the QA mirrors
            const bool loopback = QaMode() && (value.starts_with("http://127.0.0.1:") || value.starts_with("http://localhost:"));
            if (key == "inbox_url" && (value.starts_with("https://") || loopback) && value.size() <= 512)
                out.reviews.inboxUrl = value;
            else if (key == "inbox_field" && !value.empty() && value.size() <= 64)
                out.reviews.inboxField = value;
            else if (key == "salt" && value.size() <= 128)
                out.reviews.salt = value;
            else if (key == "identity_bits" && parse_u64(value, number) && number <= 40)
                out.reviews.identityBits = u32(number);
            else if (key == "review_bits" && parse_u64(value, number) && number <= 40)
                out.reviews.reviewBits = u32(number);
        }
        else if (section == "mods" || section == "withdrawn")
        {
            const bool withdrawn = section == "withdrawn";
            const auto fields = split_commas(value);
            Listing listing;
            listing.id = lower(key);
            if (fields.size() >= 3)
            {
                listing.github = fields[0];
                listing.minVersion = fields[2];
                if (fields.size() >= 4)
                    listing.date = fields[3];
            }
            if (fields.size() < 3 || !module_id_ok(listing.id) || !ValidGithubRepo(listing.github.c_str()) ||
                !ParsePublicKey(fields[1], listing.key) || !parse_version(listing.minVersion).valid)
            {
                Msg("! XMS catalog: %s [%s] is malformed - skipped", withdrawn ? "withdrawal" : "listing",
                    listing.id.c_str());
                return;
            }
            // one id, one entry: a module both listed and withdrawn is as ambiguous as one listed twice
            if (out.Find(listing.id) || out.FindWithdrawn(listing.id))
            {
                bad = true;
                return;
            }
            (withdrawn ? out.withdrawn : out.mods).push_back(std::move(listing));
        }
        else if (section == "revoked")
        {
            const size_t comma = value.find(',');
            Revocation r;
            r.subject = lower(key);
            r.version = xr_string(trim(value.substr(0, comma)));
            r.reason = comma == std::string_view::npos ? xr_string() : xr_string(trim(value.substr(comma + 1)));
            if (!r.version.empty())
                out.revoked.push_back(std::move(r));
        }
    });
    if (bad)
    {
        error = "a module is listed twice";
        return false;
    }
    if (out.format != SupportedFormat)
    {
        error = "format";
        return false;
    }
    if (!out.serial)
    {
        error = "no serial";
        return false;
    }
    return true;
}
} // namespace

bool ReadIndex(std::string_view text, Index& out, xr_string& error)
{
    if (text.size() > MaximumIndexBytes)
    {
        error = "index too large";
        return false;
    }
    Signed parts;
    if (!SplitSigned(text, parts, error))
        return false;
    const PublicKey* signer = nullptr;
    for (const PublicKey& key : MaintainerKeys())
        if (key.id == parts.keyId)
            signer = &key;
    if (!signer)
    {
        error = "signed by an unknown key " + parts.keyId;
        return false;
    }
    if (!VerifyP256(*signer, parts.body.data(), parts.body.size(), parts.signature))
    {
        error = "signature does not match";
        return false;
    }
    return parse_index_body(parts.body, out, error);
}

// ---- state -------------------------------------------------------------------------------------

xr_string CacheRoot()
{
    std::lock_guard guard(cst().lock);
    return cst().cacheRoot;
}

std::shared_ptr<const Index> Current()
{
    std::lock_guard guard(cst().lock);
    return cst().current;
}

u64 AcceptedSerial()
{
    std::lock_guard guard(cst().lock);
    return cst().serial;
}

bool Accept(std::string_view verifiedText, const Index& index, xr_string& error)
{
    CatalogState& s = cst();
    std::lock_guard guard(s.lock);
    if (s.cacheRoot.empty())
    {
        error = "the module root is not known";
        return false;
    }
    if (index.serial < s.serial)
    {
        error = "serial " + xr_string(std::to_string(index.serial).c_str()) + " is older than the accepted " +
            xr_string(std::to_string(s.serial).c_str());
        return false;
    }
    if (index.serial > s.serial || !s.current)
    {
        if (!write_file(s.cacheRoot + "index.ltx", verifiedText))
        {
            error = "could not write the catalog cache";
            return false;
        }
        s.serial = index.serial;
        save_state_locked(s);
    }
    s.current = std::make_shared<const Index>(index);
    return true;
}

bool InstalledFromBrowser(pcstr id)
{
    std::lock_guard guard(cst().lock);
    for (const auto& entry : cst().installed)
        if (entry.first == id)
            return true;
    return false;
}

void MarkInstalled(pcstr id, bool installed)
{
    CatalogState& s = cst();
    std::lock_guard guard(s.lock);
    const auto found = std::ranges::find_if(s.installed, [id](const auto& entry) { return entry.first == id; });
    if (installed == (found != s.installed.end()))
        return;
    if (installed)
        s.installed.emplace_back(id, today());
    else
        s.installed.erase(found);
    save_state_locked(s);
}

bool Listed(const Module& m)
{
    const std::shared_ptr<const Index> index = Current();
    const Listing* listing = index ? index->Find(m.id) : nullptr;
    return listing && !m.update_github.empty() && iequals(listing->github, m.update_github);
}

bool Withdrawn(const Module& m)
{
    const std::shared_ptr<const Index> index = Current();
    const Listing* listing = index ? index->FindWithdrawn(m.id) : nullptr;
    return listing && !m.update_github.empty() && iequals(listing->github, m.update_github);
}

bool IsCatalogModule(const Module& m) { return InstalledFromBrowser(m.id.c_str()) || Listed(m); }

xr_string RevocationReason(const Module& m)
{
    const std::shared_ptr<const Index> index = Current();
    if (!index)
        return {};
    // a withdrawal keeps the id bound to its repository and key, so a revocation reaches the module
    // just the same: one made after the withdrawal as well
    const Listing* listing = index->Find(m.id);
    if (!listing)
        listing = index->FindWithdrawn(m.id);
    if (!listing || m.update_github.empty() || !iequals(listing->github, m.update_github))
        return {};
    const Revocation* r = index->Revoked(m.id, m.version, listing->key.id);
    return r ? (r->reason.empty() ? xr_string("no reason given") : r->reason) : xr_string();
}

bool ProtectedMode() { return cst().protectedMode; }

xr_string ProtectedReason()
{
    std::lock_guard guard(cst().lock);
    return cst().protectedReason;
}

bool ProtectionLatched()
{
    std::lock_guard guard(cst().lock);
    return cst().latched;
}

bool CatalogModuleEnabled()
{
    std::lock_guard guard(cst().lock);
    return cst().catalogEnabled;
}

bool SetProtectionLatched(bool on, xr_string& error)
{
    CatalogState& s = cst();
    std::lock_guard guard(s.lock);
    if (s.cacheRoot.empty())
    {
        error = "the module root is not known";
        return false;
    }
    if (!on && s.catalogEnabled)
    {
        error = "a module from the catalog is enabled";
        return false;
    }
    s.latched = on;
    save_state_locked(s);
    return true;
}

void Begin(pcstr game_root)
{
    CatalogState& s = cst();
    std::lock_guard guard(s.lock);
    if (!s.cacheRoot.empty())
        return;
    s.cacheRoot = xr_string(game_root) + "modules\\.catalog\\";
    load_state_locked(s);
}

void Evaluate(xr_vector<Module>& modules, pcstr game_root, pcstr app_data_root)
{
    CatalogState& s = cst();
    Begin(game_root);
    {
        std::lock_guard guard(s.lock);
        if (s.evaluated)
            return;
        s.evaluated = true;
        s.gameRoot = real_root(game_root);
        s.appDataRoot = real_root(app_data_root);
        for (pcstr denied : {"modules", "mods", "gamedata\\scripts"})
            if (!s.gameRoot.empty())
                s.writeDenied.push_back(s.gameRoot + denied + "\\");
    }

    xr_string text, error;
    Index index;
    const xr_string cached = CacheRoot() + "index.ltx";
    if (read_file(cached, MaximumIndexBytes, text))
    {
        if (ReadIndex(text, index, error) && index.serial >= AcceptedSerial())
        {
            std::lock_guard guard(s.lock);
            s.current = std::make_shared<const Index>(index);
        }
        else
            Msg("! XMS catalog: the cached catalog is not usable (%s)", error.empty() ? "older serial" : error.c_str());
    }

    bool catalogEnabled = false;
    xr_string firstCatalogModule;
    for (Module& m : modules)
    {
        const xr_string revoked = RevocationReason(m);
        if (!revoked.empty() && !m.disabled)
        {
            m.disabled = true;
            m.disable_reason = "withdrawn from the mod catalog: " + revoked;
        }
        if (!m.disabled && IsCatalogModule(m))
        {
            if (!catalogEnabled)
                firstCatalogModule = m.id;
            catalogEnabled = true;
        }
    }

    std::lock_guard guard(s.lock);
    s.catalogEnabled = catalogEnabled;
    if (catalogEnabled && !s.latched)
    {
        s.latched = true;
        save_state_locked(s);
    }
    s.protectedMode = s.latched;
    if (catalogEnabled)
        s.protectedReason = "module [" + firstCatalogModule + "] is from the mod catalog";
    else if (s.latched)
        s.protectedReason = "switched on in the Mods window, or a catalog module was enabled before";
    else
        s.protectedReason = "no module from the mod catalog";
    Msg("* XMS catalog: protected mode %s (%s)%s", s.protectedMode ? "ON" : "off", s.protectedReason.c_str(),
        s.current ? "" : ", no catalog cached yet");
}
} // namespace Catalog

// ---- protected mode checks ---------------------------------------------------------------------

namespace Protect
{
bool Allow(pcstr path, Access access, xr_string& why)
{
    if (!Catalog::ProtectedMode())
        return true;
    xr_string real;
    if (!real_path(path, real, why))
        return false;

    const CatalogState& s = cst();
    if (!under(real, s.gameRoot) && !under(real, s.appDataRoot))
    {
        why = "outside the game and its data folder";
        return false;
    }
    if (access == Access::Read)
        return true;

    for (const xr_string& denied : s.writeDenied)
        if (under(real + '\\', denied))
        {
            why = "the game's modules and scripts are not writable";
            return false;
        }
    const size_t slash = real.find_last_of('\\');
    const std::string_view name = std::string_view(real).substr(slash == xr_string::npos ? 0 : slash + 1);
    if (name == "fsgame.ltx" || name == "user.ltx")
    {
        why = "the game's start-up configuration is not writable";
        return false;
    }
    // Explorer reads it when the folder is opened, network icon paths included
    if (name == "desktop.ini")
    {
        why = "a script may not write desktop.ini";
        return false;
    }
    if (access == Access::WriteDir)
        return true;

    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return true;
    const std::string_view extension = name.substr(dot + 1);
    static constexpr std::string_view allowed[] = {"txt", "log", "ltx", "xml", "json", "csv", "ini", "cfg", "dat",
        "sav", "scop", "scoc", "scov", "tmp", "bak", "md"};
    if (std::ranges::find(allowed, extension) != std::end(allowed))
        return true;
    why = "a script may not write ." + xr_string(extension) + " files";
    return false;
}

namespace
{
thread_local Origin origin = Origin::Other;
}

OriginScope::OriginScope(Origin value) : previous(origin) { origin = value; }
OriginScope::~OriginScope() { origin = previous; }
Origin CurrentOrigin() { return origin; }

bool AllowConsole(pcstr command, xr_string& why)
{
    if (!Catalog::ProtectedMode() || !command || origin == Origin::Player)
        return true;
    const std::string_view text = trim(command);
    // a value with a line break would become a line of its own in user.ltx, and user.ltx runs
    // as the player's at the next start
    if (text.find_first_of("\r\n") != std::string_view::npos)
    {
        why = "a console command is one line";
        return false;
    }
    const size_t space = text.find_first_of(" \t");
    const std::string_view name = text.substr(0, space);
    const std::string_view args = space == std::string_view::npos ? std::string_view() : trim(text.substr(space));

    // which modules are installed, enabled and updated is the player's business; xms_catalog is
    // here too, for its reviews and for "after", which would run a command later as the player's
    for (const std::string_view managing :
        {"xms_install", "xms_remove", "xms_remove_cancel", "xms_enable", "xms_disable", "xms_update", "xms_catalog"})
        if (iequals(name, managing))
        {
            why = "only the player may install, remove, switch or update modules";
            return false;
        }

    // xms_mods <command> presses the Mods window's buttons; opening the window is anybody's
    if (iequals(name, "xms_mods") && !args.empty())
    {
        why = "only the player may use the Mods window";
        return false;
    }

    const bool path = args.find_first_of("\\/:") != std::string_view::npos;
    // cfg_load reads a file of commands, and each of them is checked in turn; the stock scripts
    // load one from the game's configs by its path, so any path the read rule allows is fine -
    // a network path never is (reading one hands the player's credentials to its server)
    if (iequals(name, "cfg_load") && path)
    {
        // the name cfg_load itself makes of it: cut at the last dot, then .ltx
        xr_string file(args);
        if (const size_t dot = file.find_last_of('.'); dot != xr_string::npos)
            file.resize(dot);
        file += ".ltx";
        return Allow(file.c_str(), Access::Read, why);
    }
    // the commands that turn their argument into a file name to write or load keep it a name
    const bool dots = !args.empty() && args.find_first_not_of('.') == std::string_view::npos;
    for (const std::string_view naming : {"cfg_save", "save", "save_silent", "load", "demo_record", "demo_play"})
        if (iequals(name, naming) && (path || dots))
        {
            why = "only the player may give this command a path";
            return false;
        }

    // a single-player game connects to itself; another address is a server nobody vetted
    if (iequals(name, "start"))
    {
        const xr_string lowered = lower(args);
        for (size_t at = lowered.find("client("); at != xr_string::npos; at = lowered.find("client(", at + 1))
            if (lowered.compare(at, 17, "client(localhost)") != 0)
            {
                why = "only the player may connect the game to another computer";
                return false;
            }
    }
    return true;
}

void Refused(pcstr what, pcstr why, pcstr where)
{
    // a refused command may carry line breaks; the log keeps one line per refusal
    xr_string subject(what ? what : "");
    for (size_t at = subject.find_first_of("\r\n"); at != xr_string::npos; at = subject.find_first_of("\r\n", at + 2))
        subject.replace(at, 1, subject[at] == '\n' ? "\\n" : "\\r");
    if (where && *where)
        Msg("! [protect] %s refused: %s (%s)", subject.c_str(), why, where);
    else
        Msg("! [protect] %s refused: %s", subject.c_str(), why);
}
} // namespace Protect
} // namespace XMS
