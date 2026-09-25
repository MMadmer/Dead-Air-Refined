// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "ModCatalog.h"

#ifdef XR_PLATFORM_WINDOWS
#include "ModHttp.h"

#include "xrCore/XMS/xms_catalog.h"
#include "xrCore/XMS/xms_core.h"
#include "xrEngine/XR_IOConsole.h"

#include <bcrypt.h>
#include <ncrypt.h>
#include <shellapi.h>
#include <wincred.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
using ModCatalog::Entry;
using ModCatalog::PostState;
using ModCatalog::Snapshot;
using ModCatalog::Status;
using ModCatalog::Verdict;

constexpr size_t CardsLimit = 16 * 1024 * 1024;
constexpr size_t SmallFileLimit = 4 * 1024 * 1024;
constexpr size_t ThumbnailLimit = 180 * 1024;

// ---- text ----------------------------------------------------------------------------------------

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

bool parse_u64(std::string_view text, u64& value)
{
    text = trim(text);
    if (text.empty() || text.size() > 19)
        return false;
    u64 out = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        out = out * 10 + u64(c - '0');
    }
    value = out;
    return true;
}

xr_string lower(std::string_view text)
{
    xr_string out(text);
    for (char& c : out)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_hex64(std::string_view text)
{
    return text.size() == 64 && std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// one line, the rest of the text behind it
std::string_view next_line(std::string_view& text)
{
    const size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    return line;
}

std::string_view next_field(std::string_view& line)
{
    while (!line.empty() && line.front() == ' ')
        line.remove_prefix(1);
    const size_t space = line.find(' ');
    const std::string_view field = line.substr(0, space);
    line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
    return field;
}

// Review text is shown as plain text: no control characters but line breaks, no UI colour codes.
xr_string plain_text(std::string_view text, bool lineBreaks)
{
    xr_string out;
    out.reserve(text.size());
    for (size_t i = 0; i != text.size(); ++i)
    {
        const u8 c = static_cast<u8>(text[i]);
        if (c == '\n' && lineBreaks)
            out += '\n';
        else if (c == '%' && i + 1 < text.size() && text[i + 1] == 'c')
            ++i;
        else if (c >= 0x20 && c != 0x7F)
            out += static_cast<char>(c);
        else if (c == '\t')
            out += ' ';
    }
    return out;
}

xr_string utf8_of(std::wstring_view wide)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    xr_string out(static_cast<size_t>(std::max(length, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), length, nullptr, nullptr);
    return out;
}

xr_string today_utc()
{
    SYSTEMTIME now{};
    GetSystemTime(&now);
    string64 text{};
    xr_sprintf(text, "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return text;
}

xr_string date_of_unix(u64 seconds)
{
    const time_t time = static_cast<time_t>(seconds);
    tm utc{};
    if (gmtime_s(&utc, &time) != 0)
        return {};
    string32 text{};
    xr_sprintf(text, "%04d-%02d-%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    return text;
}

xr_string environment(pcstr name)
{
    char value[1024]{};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length && length < sizeof(value) ? xr_string(value, length) : xr_string();
}

bool loopback_http(std::string_view url)
{
    return url.starts_with("http://127.0.0.1:") || url.starts_with("http://localhost:");
}

// ---- files ---------------------------------------------------------------------------------------

std::filesystem::path cache_path(std::string_view name)
{
    return std::filesystem::path(XMS::Catalog::CacheRoot().c_str()) / std::filesystem::path(xr_string(name).c_str());
}

bool read_file(const std::filesystem::path& path, size_t limit, xr_string& out)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > limit)
        return false;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out.assign(static_cast<size_t>(size), '\0');
    file.read(out.data(), static_cast<std::streamsize>(size));
    return static_cast<u64>(file.gcount()) == size;
}

bool write_file(const std::filesystem::path& path, std::string_view data)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

// ---- thumbnails (MOD_CATALOG 6.1) -----------------------------------------------------------------

// A DDS of 256 x 256, DXT1 or DXT5, one level or the full chain, of exactly the size those give.
bool valid_thumbnail(std::string_view bytes)
{
    if (bytes.size() < 128 || bytes.size() > ThumbnailLimit || !bytes.starts_with("DDS "))
        return false;
    const auto u32_at = [&bytes](size_t offset) {
        u32 value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        return value;
    };
    const u32 headerSize = u32_at(4), height = u32_at(12), width = u32_at(16), mips = u32_at(28);
    const u32 formatSize = u32_at(76), formatFlags = u32_at(80);
    const std::string_view fourcc = bytes.substr(84, 4);
    if (headerSize != 124 || width != 256 || height != 256 || formatSize != 32 || !(formatFlags & 0x4))
        return false;
    const u32 block = fourcc == "DXT1" ? 8 : fourcc == "DXT5" ? 16 : 0;
    const u32 levels = mips ? mips : 1;
    if (!block || (levels != 1 && levels != 9))
        return false;
    size_t expected = 128;
    for (u32 level = 0, side = 256; level != levels; ++level, side = std::max<u32>(side / 2, 1))
        expected += size_t(std::max<u32>((side + 3) / 4, 1)) * std::max<u32>((side + 3) / 4, 1) * block;
    return bytes.size() == expected;
}

// ---- a SHA-256 of our own for the proofs of work: the prefix is hashed once, every attempt
// hashes the nonce on top of it. Everything that verifies uses CNG (XMS::Catalog::Sha256).

struct Sha
{
    u32 h[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 block[64]{};
    size_t used{};
    u64 length{};

    static u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const u8* data)
    {
        static constexpr u32 k[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c,
            0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        u32 w[64];
        for (int i = 0; i != 16; ++i)
            w[i] = (u32(data[i * 4]) << 24) | (u32(data[i * 4 + 1]) << 16) | (u32(data[i * 4 + 2]) << 8) | u32(data[i * 4 + 3]);
        for (int i = 16; i != 64; ++i)
        {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i != 64; ++i)
        {
            const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = hh + s1 + ch + k[i] + w[i];
            const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }

    void update(const void* data, size_t size)
    {
        const u8* bytes = static_cast<const u8*>(data);
        length += size;
        while (size)
        {
            const size_t take = std::min(size, 64 - used);
            std::memcpy(block + used, bytes, take);
            used += take;
            bytes += take;
            size -= take;
            if (used == 64)
            {
                compress(block);
                used = 0;
            }
        }
    }

    void finish(u8 out[32])
    {
        const u64 bits = length * 8;
        const u8 pad = 0x80;
        update(&pad, 1);
        const u8 zero = 0;
        while (used != 56)
            update(&zero, 1);
        u8 size[8];
        for (int i = 0; i != 8; ++i)
            size[i] = static_cast<u8>(bits >> (56 - i * 8));
        update(size, 8);
        for (int i = 0; i != 8; ++i)
        {
            out[i * 4] = static_cast<u8>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<u8>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<u8>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<u8>(h[i]);
        }
    }
};

u32 leading_zero_bits(const u8 digest[32])
{
    u32 bits = 0;
    for (int i = 0; i != 32; ++i)
    {
        if (!digest[i])
        {
            bits += 8;
            continue;
        }
        for (u8 mask = 0x80; mask && !(digest[i] & mask); mask >>= 1)
            ++bits;
        break;
    }
    return bits;
}

// The smallest-effort nonce search there is: every thread walks its own residue class, the first
// hit stops them all. False when stopped from outside.
bool proof_of_work(std::string_view prefix, u32 bits, u64& nonce, const std::atomic_bool& stop)
{
    Sha base;
    base.update(prefix.data(), prefix.size());
    const u32 threads = std::clamp<u32>(std::thread::hardware_concurrency(), 1, 16);
    std::atomic_bool found{};
    std::atomic<u64> result{};
    xr_vector<std::thread> workers;
    for (u32 t = 0; t != threads; ++t)
    {
        workers.emplace_back([&, t] {
            char digits[24];
            u8 digest[32];
            for (u64 candidate = t; !found.load(std::memory_order_relaxed) && !stop.load(std::memory_order_relaxed);
                 candidate += threads)
            {
                const int length = snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(candidate));
                Sha attempt = base;
                attempt.update(digits, static_cast<size_t>(length));
                attempt.finish(digest);
                if (leading_zero_bits(digest) >= bits && !found.exchange(true))
                    result.store(candidate);
            }
        });
    }
    for (std::thread& worker : workers)
        worker.join();
    if (!found.load())
        return false;
    nonce = result.load();
    // the answer is checked the way the catalog will check it
    const xr_string message = xr_string(prefix) + std::to_string(nonce).c_str();
    u8 digest[32];
    return XMS::Catalog::Sha256(message.data(), message.size(), digest) && leading_zero_bits(digest) >= bits;
}

// ---- a small JSON reader for VirusTotal's answers ----------------------------------------------------

struct Json
{
    enum class Type : u8
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    } type{Type::Null};
    bool boolean{};
    double number{};
    xr_string text;
    xr_vector<Json> items;
    xr_vector<std::pair<xr_string, Json>> members;

    const Json* Get(std::string_view key) const
    {
        for (const auto& [name, value] : members)
            if (name == key)
                return &value;
        return nullptr;
    }
};

class JsonReader
{
public:
    explicit JsonReader(std::string_view text) : m_text(text) {}

    bool Parse(Json& out)
    {
        if (!Value(out, 0))
            return false;
        Space();
        return m_at == m_text.size();
    }

private:
    void Space()
    {
        while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\r' || m_text[m_at] == '\n'))
            ++m_at;
    }

    bool Literal(std::string_view word)
    {
        if (m_text.substr(m_at, word.size()) != word)
            return false;
        m_at += word.size();
        return true;
    }

    bool String(xr_string& out)
    {
        if (m_at >= m_text.size() || m_text[m_at] != '"')
            return false;
        ++m_at;
        while (m_at < m_text.size())
        {
            const char c = m_text[m_at++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (m_at >= m_text.size())
                return false;
            const char escape = m_text[m_at++];
            switch (escape)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                if (m_at + 4 > m_text.size())
                    return false;
                const u32 code = strtoul(xr_string(m_text.substr(m_at, 4)).c_str(), nullptr, 16);
                m_at += 4;
                wchar_t wide[2] = {static_cast<wchar_t>(code), 0};
                out += utf8_of(std::wstring_view(wide, 1));
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool Value(Json& out, int depth)
    {
        if (depth > 64)
            return false;
        Space();
        if (m_at >= m_text.size())
            return false;
        const char c = m_text[m_at];
        if (c == '{')
        {
            out.type = Json::Type::Object;
            ++m_at;
            Space();
            if (m_at < m_text.size() && m_text[m_at] == '}')
                return ++m_at, true;
            while (true)
            {
                Space();
                auto& [name, value] = out.members.emplace_back();
                if (!String(name))
                    return false;
                Space();
                if (m_at >= m_text.size() || m_text[m_at++] != ':' || !Value(value, depth + 1))
                    return false;
                Space();
                if (m_at < m_text.size() && m_text[m_at] == ',')
                {
                    ++m_at;
                    continue;
                }
                return m_at < m_text.size() && m_text[m_at++] == '}';
            }
        }
        if (c == '[')
        {
            out.type = Json::Type::Array;
            ++m_at;
            Space();
            if (m_at < m_text.size() && m_text[m_at] == ']')
                return ++m_at, true;
            while (true)
            {
                if (!Value(out.items.emplace_back(), depth + 1))
                    return false;
                Space();
                if (m_at < m_text.size() && m_text[m_at] == ',')
                {
                    ++m_at;
                    continue;
                }
                return m_at < m_text.size() && m_text[m_at++] == ']';
            }
        }
        if (c == '"')
        {
            out.type = Json::Type::String;
            return String(out.text);
        }
        if (Literal("true"))
            return out.type = Json::Type::Bool, out.boolean = true, true;
        if (Literal("false"))
            return out.type = Json::Type::Bool, out.boolean = false, true;
        if (Literal("null"))
            return out.type = Json::Type::Null, true;
        const size_t start = m_at;
        while (m_at < m_text.size() && std::string_view("+-0123456789.eE").find(m_text[m_at]) != std::string_view::npos)
            ++m_at;
        if (start == m_at)
            return false;
        out.type = Json::Type::Number;
        out.number = strtod(xr_string(m_text.substr(start, m_at - start)).c_str(), nullptr);
        return true;
    }

    std::string_view m_text;
    size_t m_at{};
};

// ---- the service ---------------------------------------------------------------------------------------

struct ReviewsSlot
{
    std::shared_ptr<const ModCatalog::Reviews> reviews;
};

struct CatalogService
{
    ~CatalogService() { Stop(); }

    void Stop()
    {
        {
            std::lock_guard lock(mutex);
            stop.store(true, std::memory_order_release);
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void(ModHttp::Session&)>> jobs;
    std::thread worker;
    std::atomic_bool stop{};
    // jobs queued or running: the deferred console commands wait for none
    std::atomic<u32> pending{};

    std::shared_ptr<const Snapshot> snapshot = std::make_shared<Snapshot>();
    bool refreshing{};
    bool loadedCache{};
    xr_string activeMirror;
    // the raw catalog files the snapshot was built from
    xr_string cards, vt, ratings;
    // VirusTotal answers asked with the player's key, per module id
    std::unordered_map<std::string, ModCatalog::VirusTotal> liveVt;
    xr_vector<xr_string> vtBusy;
    // when the last four VirusTotal requests went out; worker thread only
    std::deque<std::chrono::steady_clock::time_point> vtSent;
    // thumbnails: sha256 -> downloaded file, waiting for the UI thread; sha256 -> texture name
    xr_vector<std::pair<xr_string, std::filesystem::path>> toMount;
    std::unordered_map<std::string, xr_string> mounted;
    xr_vector<xr_string> thumbnailsWanted;
    std::unordered_map<std::string, xr_string> installedThumbnails;
    std::unordered_map<std::string, ReviewsSlot> reviews;
    ModCatalog::PostStatus post;
    // console commands waiting for a finished load, with who asked (main thread only)
    xr_vector<std::pair<xr_string, XMS::Protect::Origin>> deferred;
};

CatalogService& service()
{
    static CatalogService instance;
    return instance;
}

void worker_loop()
{
    CatalogService& s = service();
    ModHttp::Session session;
    while (true)
    {
        std::function<void(ModHttp::Session&)> job;
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait(lock, [&s] { return s.stop.load() || !s.jobs.empty(); });
            if (s.stop.load())
                return;
            job = std::move(s.jobs.front());
            s.jobs.pop_front();
        }
        job(session);
        s.pending.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void enqueue(std::function<void(ModHttp::Session&)> job)
{
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    if (s.stop.load())
        return;
    s.pending.fetch_add(1, std::memory_order_acq_rel);
    s.jobs.push_back(std::move(job));
    if (!s.worker.joinable())
        s.worker = std::thread(worker_loop);
    s.wake.notify_one();
}

xr_vector<xr_string> mirrors()
{
    xr_vector<xr_string> out;
    CatalogService& s = service();
    {
        std::lock_guard lock(s.mutex);
        if (!s.activeMirror.empty())
            out.push_back(s.activeMirror);
    }
    for (const xr_string& mirror : XMS::Catalog::BuiltInMirrors())
        if (std::ranges::find(out, mirror) == out.end())
            out.push_back(mirror);
    if (!XMS::Catalog::QaMode())
        if (const auto index = XMS::Catalog::Current())
            for (const xr_string& mirror : index->mirrors)
                if (std::ranges::find(out, mirror) == out.end())
                    out.push_back(mirror);
    return out;
}

// One file of the catalog from the first mirror that has it.
ModHttp::Fetch fetch_from_mirrors(ModHttp::Session& session, std::string_view name, size_t limit, xr_string& body, xr_string& error)
{
    ModHttp::Fetch last = ModHttp::Fetch::Failed;
    for (const xr_string& mirror : mirrors())
    {
        body.clear();
        last = ModHttp::FetchText(session, ModHttp::Widen(mirror + xr_string(name)), limit, body, error, &service().stop);
        if (last != ModHttp::Fetch::Failed)
            return last;
    }
    return last;
}

// ---- building the snapshot -------------------------------------------------------------------------

Verdict judge(const XMS::Catalog::VtPolicy& policy, u32 trustedMalicious, u32 trustedSuspicious, u32 othersMalicious)
{
    if (trustedMalicious >= policy.blockTrusted || othersMalicious >= policy.blockOthers)
        return Verdict::Block;
    if (trustedSuspicious >= policy.warnTrusted || othersMalicious >= policy.warnOthers)
        return Verdict::Warn;
    return Verdict::Clean;
}

// How decisive a package's verdict is: block, warn, not scanned or no result, clean.
int vt_rank(Verdict verdict)
{
    switch (verdict)
    {
    case Verdict::Block: return 3;
    case Verdict::Warn: return 2;
    case Verdict::Unknown: return 1;
    case Verdict::Clean: return 0;
    }
    return 1;
}

// A release from the results of its packages (sha256, result), as ModCatalog::VirusTotal describes.
ModCatalog::VirusTotal vt_combine(const xr_vector<std::pair<xr_string, ModCatalog::VirusTotal>>& packages)
{
    ModCatalog::VirusTotal out;
    int decisive = -1;
    xr_string oldest;
    bool unknown = false;
    for (const auto& [sha, result] : packages)
    {
        if (vt_rank(result.verdict) > decisive)
        {
            decisive = vt_rank(result.verdict);
            out = result;
            out.package = sha;
        }
        unknown = unknown || result.unknown;
        if (!result.date.empty() && (oldest.empty() || result.date < oldest))
            oldest = result.date;
    }
    out.date = oldest;
    out.unknown = unknown;
    return out;
}

std::shared_ptr<Snapshot> build_snapshot(Status status, xr_string error)
{
    CatalogService& s = service();
    auto out = std::make_shared<Snapshot>();
    out->status = status;
    out->error = std::move(error);
    const std::shared_ptr<const XMS::Catalog::Index> index = XMS::Catalog::Current();
    if (!index)
        return out;
    out->serial = index->serial;
    out->reviewsWritable = !index->reviews.inboxUrl.empty() && !index->reviews.inboxField.empty() &&
        (index->reviews.inboxUrl.starts_with("https://") || (XMS::Catalog::QaMode() && loopback_http(index->reviews.inboxUrl)));

    xr_string cards, vt, ratings;
    {
        std::lock_guard lock(s.mutex);
        cards = s.cards;
        vt = s.vt;
        ratings = s.ratings;
    }

    for (const XMS::Catalog::Listing& listing : index->mods)
    {
        Entry& entry = out->entries.emplace_back();
        entry.id = listing.id;
        entry.github = listing.github;
        entry.listed = listing.date;
        entry.minVersion = listing.minVersion;
        if (const XMS::Catalog::Revocation* revoked = index->Revoked(listing.id, "", listing.key.id))
            entry.revoked = revoked->reason.empty() ? xr_string("-") : revoked->reason;
    }

    // cards: each one on its author's signature alone
    std::string_view rest(cards);
    if (next_line(rest) == "xms-cards 1")
    {
        while (!rest.empty())
        {
            std::string_view header = next_line(rest);
            if (header.empty())
                continue;
            u64 length = 0;
            if (next_field(header) != "card")
                break;
            const std::string_view id = next_field(header);
            if (!parse_u64(next_field(header), length) || length > rest.size())
                break;
            const std::string_view bytes = rest.substr(0, static_cast<size_t>(length));
            rest.remove_prefix(static_cast<size_t>(length));
            const XMS::Catalog::Listing* listing = index->Find(id);
            const auto entry = std::ranges::find(out->entries, xr_string(id), &Entry::id);
            if (!listing || entry == out->entries.end())
                continue;
            ModRelease::Descriptor descriptor;
            xr_string why;
            if (!ModRelease::ParseDescriptor(bytes, descriptor, why) || descriptor.id != id ||
                !XMS::Catalog::VerifySigned(bytes, listing->key, why) ||
                ModRelease::ParseVersion(descriptor.version) < ModRelease::ParseVersion(listing->minVersion))
            {
                Msg("! [catalog] the card of [%s] is not used: %s", xr_string(id).c_str(), why.empty() ? "below the listed version" : why.c_str());
                continue;
            }
            if (const XMS::Catalog::Revocation* revoked = index->Revoked(id, descriptor.version, listing->key.id))
                entry->revoked = revoked->reason.empty() ? xr_string("-") : revoked->reason;
            entry->release = std::move(descriptor);
            entry->hasCard = true;
        }
    }

    // VirusTotal: the catalog's word, then the player's own
    std::unordered_map<std::string, ModCatalog::VirusTotal> byPackage;
    rest = vt;
    if (next_line(rest) == "xms-vt 1")
    {
        while (!rest.empty())
        {
            std::string_view line = next_line(rest);
            const std::string_view sha = next_field(line);
            ModCatalog::VirusTotal result;
            result.date = next_field(line);
            u64 engines = 0, tm = 0, ts = 0, om = 0;
            if (!is_hex64(sha) || !parse_u64(next_field(line), engines) || !parse_u64(next_field(line), tm) ||
                !parse_u64(next_field(line), ts) || !parse_u64(next_field(line), om))
                continue;
            result.engines = u32(engines);
            result.trustedMalicious = u32(tm);
            result.trustedSuspicious = u32(ts);
            result.othersMalicious = u32(om);
            const std::string_view names = trim(line);
            result.flaggedBy = names == "-" ? xr_string() : plain_text(names, false);
            result.verdict = judge(index->vt, result.trustedMalicious, result.trustedSuspicious, result.othersMalicious);
            byPackage[std::string(sha)] = std::move(result);
        }
    }

    std::unordered_map<std::string, ModCatalog::Rating> byModule;
    rest = ratings;
    if (next_line(rest) == "xms-ratings 1")
    {
        while (!rest.empty())
        {
            std::string_view line = next_line(rest);
            const std::string_view id = next_field(line);
            ModCatalog::Rating rating;
            u64 values[7]{};
            bool ok = !id.empty();
            for (u64& value : values)
                ok = ok && parse_u64(next_field(line), value);
            if (!ok)
                continue;
            rating.count = u32(values[0]);
            rating.average100 = u32(std::min<u64>(values[1], 500));
            for (int i = 0; i != 5; ++i)
                rating.stars[i] = u32(values[2 + i]);
            byModule[std::string(id)] = rating;
        }
    }

    std::lock_guard lock(s.mutex);
    for (Entry& entry : out->entries)
    {
        if (const auto rating = byModule.find(entry.id.c_str()); rating != byModule.end())
            entry.rating = rating->second;
        // every package of the release: a clean first part says nothing about the second
        if (entry.hasCard && !entry.release.packages.empty())
        {
            xr_vector<std::pair<xr_string, ModCatalog::VirusTotal>> packages;
            for (const ModRelease::Asset& package : entry.release.packages)
            {
                const auto result = byPackage.find(package.sha256.c_str());
                packages.emplace_back(package.sha256,
                    result != byPackage.end() ? result->second : ModCatalog::VirusTotal{});
            }
            entry.vt = vt_combine(packages);
        }
        if (const auto live = s.liveVt.find(entry.id.c_str()); live != s.liveVt.end())
            entry.vt = live->second;
        if (entry.hasCard && entry.release.card.thumbnail.present)
            if (const auto texture = s.mounted.find(entry.release.card.thumbnail.sha256.c_str()); texture != s.mounted.end())
                entry.thumbnailTexture = texture->second;
    }
    return out;
}

void publish(std::shared_ptr<Snapshot> next)
{
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    next->generation = s.snapshot->generation + 1;
    s.snapshot = std::move(next);
}

// the snapshot again with what changed around it (a thumbnail, a VirusTotal answer)
void republish()
{
    const std::shared_ptr<const Snapshot> current = ModCatalog::Current();
    publish(build_snapshot(current->status, current->error));
}

void load_cache()
{
    CatalogService& s = service();
    xr_string cards, vt, ratings;
    read_file(cache_path("cards.txt"), CardsLimit, cards);
    read_file(cache_path("vt.txt"), SmallFileLimit, vt);
    read_file(cache_path("ratings.txt"), SmallFileLimit, ratings);
    std::lock_guard lock(s.mutex);
    s.cards = std::move(cards);
    s.vt = std::move(vt);
    s.ratings = std::move(ratings);
    s.loadedCache = true;
}

void run_refresh(ModHttp::Session& session)
{
    CatalogService& s = service();
    bool cached = false;
    {
        std::lock_guard lock(s.mutex);
        cached = s.loadedCache;
    }
    if (!cached)
    {
        load_cache();
        publish(build_snapshot(Status::Loading, {}));
    }

    xr_string lastError = "no mirror is configured";
    xr_string mirror;
    for (const xr_string& candidate : mirrors())
    {
        xr_string body, error;
        const ModHttp::Fetch fetched =
            ModHttp::FetchText(session, ModHttp::Widen(candidate + "index.ltx"), XMS::Catalog::MaximumIndexBytes, body, error, &s.stop);
        if (fetched != ModHttp::Fetch::Ok)
        {
            lastError = candidate + "index.ltx: " + (fetched == ModHttp::Fetch::NotFound ? xr_string("not found") : error);
            continue;
        }
        XMS::Catalog::Index index;
        if (!XMS::Catalog::ReadIndex(body, index, error))
        {
            lastError = candidate + "index.ltx: " + (error == "format" ? xr_string("the game needs an update to read this catalog") : error);
            Msg("! [catalog] %s", lastError.c_str());
            continue;
        }
        if (!XMS::Catalog::Accept(body, index, error))
        {
            // an older publication on a stale mirror: another mirror may have the current one
            lastError = candidate + ": " + error;
            Msg("~ [catalog] %s", lastError.c_str());
            continue;
        }
        mirror = candidate;
        break;
    }

    if (mirror.empty())
    {
        const bool haveIndex = XMS::Catalog::Current() != nullptr;
        Msg("! [catalog] the catalog could not be loaded - %s%s", lastError.c_str(), haveIndex ? " (the cached catalog is shown)" : "");
        publish(build_snapshot(haveIndex ? Status::Offline : Status::Failed, lastError));
        std::lock_guard lock(s.mutex);
        s.refreshing = false;
        return;
    }
    {
        std::lock_guard lock(s.mutex);
        s.activeMirror = mirror;
    }

    // the rest from the same place; a file a mirror does not have is simply empty
    const auto fetch_optional = [&](std::string_view name, size_t limit, xr_string& out) {
        xr_string body, error;
        const ModHttp::Fetch fetched = fetch_from_mirrors(session, name, limit, body, error);
        if (fetched == ModHttp::Fetch::Ok)
        {
            out = std::move(body);
            write_file(cache_path(name), out);
            return true;
        }
        if (fetched == ModHttp::Fetch::NotFound)
        {
            out.clear();
            write_file(cache_path(name), out);
            return true;
        }
        Msg("~ [catalog] %s: %s (the cached copy is kept)", xr_string(name).c_str(), error.c_str());
        return false;
    };
    xr_string cards, vt, ratings;
    {
        std::lock_guard lock(s.mutex);
        cards = s.cards;
        vt = s.vt;
        ratings = s.ratings;
    }
    fetch_optional("cards.txt", CardsLimit, cards);
    fetch_optional("vt.txt", SmallFileLimit, vt);
    fetch_optional("ratings.txt", SmallFileLimit, ratings);
    {
        std::lock_guard lock(s.mutex);
        s.cards = std::move(cards);
        s.vt = std::move(vt);
        s.ratings = std::move(ratings);
        s.refreshing = false;
    }
    const auto snapshot = build_snapshot(Status::Ready, {});
    Msg("* [catalog] serial %llu from %s: %zu module(s)", static_cast<unsigned long long>(snapshot->serial), mirror.c_str(),
        snapshot->entries.size());
    publish(snapshot);
}

// ---- thumbnails ------------------------------------------------------------------------------------------

void run_thumbnail(ModHttp::Session& session, const xr_string& sha, u64 size)
{
    CatalogService& s = service();
    const std::filesystem::path file = cache_path("thumbs") / (xr_string(sha) + ".dds").c_str();
    xr_string bytes;
    bool ready = read_file(file, ThumbnailLimit, bytes) && bytes.size() == size &&
        XMS::Catalog::Sha256Hex(bytes.data(), bytes.size()) == sha && valid_thumbnail(bytes);
    if (!ready)
    {
        xr_string error;
        bytes.clear();
        if (fetch_from_mirrors(session, "thumbs/" + sha + ".dds", ThumbnailLimit, bytes, error) == ModHttp::Fetch::Ok &&
            bytes.size() == size && XMS::Catalog::Sha256Hex(bytes.data(), bytes.size()) == sha && valid_thumbnail(bytes))
            ready = write_file(file, bytes);
    }
    std::lock_guard lock(s.mutex);
    if (ready)
        s.toMount.emplace_back(sha, file);
}

// ---- reviews --------------------------------------------------------------------------------------------------

void run_reviews(ModHttp::Session& session, const xr_string& id)
{
    auto reviews = std::make_shared<ModCatalog::Reviews>();
    reviews->id = id;
    xr_string body, error;
    const ModHttp::Fetch fetched = fetch_from_mirrors(session, "reviews/" + id + ".txt", SmallFileLimit, body, error);
    if (fetched == ModHttp::Fetch::Failed)
    {
        reviews->status = Status::Failed;
        reviews->error = error;
    }
    else
    {
        reviews->status = Status::Ready;
        std::string_view rest(body);
        if (fetched == ModHttp::Fetch::Ok && next_line(rest) == "xms-reviews 1")
        {
            while (!rest.empty())
            {
                std::string_view header = next_line(rest);
                if (header.empty())
                    continue;
                if (next_field(header) != "review")
                    break;
                ModCatalog::Review review;
                review.keyId = next_field(header);
                u64 stars = 0, nameLength = 0, textLength = 0;
                if (!parse_u64(next_field(header), stars))
                    break;
                review.date = next_field(header);
                review.version = next_field(header);
                if (!parse_u64(next_field(header), nameLength) || !parse_u64(next_field(header), textLength) ||
                    nameLength + textLength > rest.size())
                    break;
                review.name = plain_text(rest.substr(0, static_cast<size_t>(nameLength)), false);
                review.text = plain_text(rest.substr(static_cast<size_t>(nameLength), static_cast<size_t>(textLength)), true);
                rest.remove_prefix(static_cast<size_t>(nameLength + textLength));
                if (!rest.empty() && rest.front() == '\n')
                    rest.remove_prefix(1);
                review.stars = u32(std::clamp<u64>(stars, 1, 5));
                reviews->list.push_back(std::move(review));
            }
        }
    }
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    s.reviews[id.c_str()].reviews = std::move(reviews);
}

// ---- VirusTotal ----------------------------------------------------------------------------------------------

constexpr wchar_t VtCredential[] = L"DeadAirRefined:VirusTotal";

xr_string vt_key()
{
    if (XMS::Catalog::QaMode())
        return environment("DAR_QA_VT_KEY");
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(VtCredential, CRED_TYPE_GENERIC, 0, &credential))
        return {};
    xr_string key(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    return key;
}

xr_string vt_base()
{
    if (XMS::Catalog::QaMode())
    {
        const xr_string base = environment("DAR_QA_VT_BASE");
        return loopback_http(base) ? base : xr_string();
    }
    return "https://www.virustotal.com/";
}

// The free API takes 4 requests a minute: the first four go at once, every later one waits until
// the one four before it is a little over a minute old. Worker thread only; false when stopping.
bool vt_pace()
{
    CatalogService& s = service();
    if (s.vtSent.size() >= 4)
    {
        const auto due = s.vtSent.front() + std::chrono::seconds(61);
        if (const auto now = std::chrono::steady_clock::now(); now < due)
            Msg("~ [catalog] VirusTotal takes 4 requests a minute: the next one in %lld s",
                static_cast<long long>(std::chrono::ceil<std::chrono::seconds>(due - now).count()));
        while (std::chrono::steady_clock::now() < due)
        {
            if (s.stop.load(std::memory_order_acquire))
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        s.vtSent.pop_front();
    }
    s.vtSent.push_back(std::chrono::steady_clock::now());
    return true;
}

// The report of one package. A package VirusTotal has never seen is an answer too (unknown); false
// when there is none to go by - a refused key, the quota, the network, an answer that is no report.
bool vt_ask(ModHttp::Session& session, const xr_string& key, const xr_string& base, const xr_string& id,
    const xr_string& sha, const XMS::Catalog::VtPolicy& policy, ModCatalog::VirusTotal& result)
{
    xr_string body, error;
    DWORD status = 0;
    const std::wstring headers = L"x-apikey: " + ModHttp::Widen(key) + L"\r\nAccept: application/json\r\n";
    const bool asked = ModHttp::Exchange(session, L"GET", ModHttp::Widen(base + "api/v3/files/" + sha), headers, {},
        SmallFileLimit, body, status, error);
    if (asked && status == 404)
    {
        result.unknown = true;
        return true;
    }
    if (!asked || status != 200)
    {
        Msg("! [catalog] VirusTotal did not answer for [%s]: %s", id.c_str(),
            !asked                               ? error.c_str() :
                (status == 401 || status == 403) ? "the key was refused" :
                status == 429                    ? "too many requests, try again in a minute" :
                                                   ("HTTP " + xr_string(std::to_string(status).c_str())).c_str());
        return false;
    }
    Json root;
    const Json* attributes = nullptr;
    if (JsonReader(body).Parse(root))
        if (const Json* data = root.Get("data"))
            attributes = data->Get("attributes");
    const Json* results = attributes ? attributes->Get("last_analysis_results") : nullptr;
    if (!results || results->type != Json::Type::Object)
    {
        Msg("! [catalog] VirusTotal's answer for [%s] is not a report", id.c_str());
        return false;
    }
    xr_vector<xr_string> flagged;
    for (const auto& [engine, verdict] : results->members)
    {
        const Json* category = verdict.Get("category");
        if (!category || category->type != Json::Type::String)
            continue;
        const xr_string& value = category->text;
        if (value != "malicious" && value != "suspicious" && value != "undetected" && value != "harmless")
            continue;
        ++result.engines;
        const bool trusted = std::ranges::find(policy.trusted, lower(engine)) != policy.trusted.end();
        if (value == "malicious")
        {
            (trusted ? result.trustedMalicious : result.othersMalicious)++;
            flagged.push_back(plain_text(engine, false));
        }
        else if (value == "suspicious" && trusted)
        {
            ++result.trustedSuspicious;
            flagged.push_back(plain_text(engine, false));
        }
    }
    for (const xr_string& name : flagged)
        result.flaggedBy += (result.flaggedBy.empty() ? "" : ", ") + name;
    if (const Json* date = attributes->Get("last_analysis_date"); date && date->type == Json::Type::Number)
        result.date = date_of_unix(static_cast<u64>(date->number));
    result.verdict = judge(policy, result.trustedMalicious, result.trustedSuspicious, result.othersMalicious);
    return true;
}

void run_vt(ModHttp::Session& session, const xr_string& id)
{
    CatalogService& s = service();
    const auto finish = [&s, &id](const ModCatalog::VirusTotal* result) {
        {
            std::lock_guard lock(s.mutex);
            if (result)
                s.liveVt[id.c_str()] = *result;
            s.vtBusy.erase(std::remove(s.vtBusy.begin(), s.vtBusy.end(), id), s.vtBusy.end());
        }
        republish();
    };

    const std::shared_ptr<const Snapshot> snapshot = ModCatalog::Current();
    const Entry* entry = snapshot->Find(id);
    const std::shared_ptr<const XMS::Catalog::Index> index = XMS::Catalog::Current();
    const xr_string key = vt_key();
    const xr_string base = vt_base();
    if (!entry || !entry->hasCard || entry->release.packages.empty() || !index || key.empty() || base.empty())
    {
        finish(nullptr);
        return;
    }
    // every package, not the first alone: a release split in parts is as good as its worst part.
    // Copied - the snapshot may be replaced while a request waits for its turn.
    const xr_vector<ModRelease::Asset> packages = entry->release.packages;
    xr_vector<std::pair<xr_string, ModCatalog::VirusTotal>> results;
    for (size_t n = 0; n != packages.size(); ++n)
    {
        ModCatalog::VirusTotal result;
        if (!vt_pace() || !vt_ask(session, key, base, id, packages[n].sha256, index->vt, result))
        {
            finish(nullptr);
            return;
        }
        if (packages.size() > 1)
            Msg("* [catalog] VirusTotal for [%s] package %zu of %zu: %u engine(s), %u trusted malicious, "
                "%u trusted suspicious, %u other malicious%s",
                id.c_str(), n + 1, packages.size(), result.engines, result.trustedMalicious, result.trustedSuspicious,
                result.othersMalicious, result.unknown ? " (never scanned)" : "");
        results.emplace_back(packages[n].sha256, std::move(result));
    }
    ModCatalog::VirusTotal result = vt_combine(results);
    result.live = true;
    if (packages.size() == 1)
        Msg("* [catalog] VirusTotal for [%s]: %u engine(s), %u trusted malicious, %u trusted suspicious, "
            "%u other malicious%s",
            id.c_str(), result.engines, result.trustedMalicious, result.trustedSuspicious, result.othersMalicious,
            result.unknown ? " (never scanned)" : "");
    else
    {
        static constexpr pcstr verdicts[] = {"not scanned", "clean", "warn", "block"};
        const auto decisive = std::ranges::find(packages, result.package, &ModRelease::Asset::sha256);
        Msg("* [catalog] VirusTotal for [%s]: %s, decided by package %zu of %zu%s", id.c_str(),
            verdicts[static_cast<int>(result.verdict)], static_cast<size_t>(decisive - packages.begin()) + 1,
            packages.size(), result.unknown ? "; VirusTotal has never seen one of them" : "");
    }
    finish(&result);
}

// ---- reviews: identity, hardware, proofs, the post ------------------------------------------------------------------

xr_string hex_of(const u8* data, size_t size)
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

// SMBIOS type 1 system UUID and Windows' MachineGuid, salted by the catalog: one computer, one
// value, and neither value recoverable from it (MOD_CATALOG 12.1).
xr_string hardware_hash(const xr_string& salt)
{
    xr_string uuid;
    const UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size > 8)
    {
        xr_vector<u8> table(size);
        if (GetSystemFirmwareTable('RSMB', 0, table.data(), size) == size)
        {
            u32 length = 0;
            std::memcpy(&length, table.data() + 4, 4);
            const u8* data = table.data() + 8;
            const u8* end = data + std::min<size_t>(length, size - 8);
            while (data + 4 <= end)
            {
                const u8 type = data[0], formatted = data[1];
                if (formatted < 4 || data + formatted > end)
                    break;
                if (type == 1 && formatted >= 0x19)
                {
                    uuid = hex_of(data + 8, 16);
                    break;
                }
                const u8* strings = data + formatted;
                while (strings + 1 < end && (strings[0] || strings[1]))
                    ++strings;
                data = strings + 2;
            }
        }
    }
    wchar_t guid[128]{};
    HKEY cryptography{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
            &cryptography) == ERROR_SUCCESS)
    {
        DWORD type = 0, bytes = sizeof(guid) - sizeof(wchar_t);
        if (RegQueryValueExW(cryptography, L"MachineGuid", nullptr, &type, reinterpret_cast<LPBYTE>(guid), &bytes) !=
                ERROR_SUCCESS ||
            type != REG_SZ)
            guid[0] = 0;
        RegCloseKey(cryptography);
    }
    const xr_string message = "xms-hw|" + salt + "|" + uuid + "|" + utf8_of(guid);
    return XMS::Catalog::Sha256Hex(message.data(), message.size());
}

class Identity
{
public:
    ~Identity()
    {
        if (m_key)
            NCryptFreeObject(m_key);
    }

    bool Open(xr_string& error)
    {
        NCRYPT_PROV_HANDLE provider{};
        if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
        {
            error = "the key storage of Windows is not available";
            return false;
        }
        const wchar_t* name = XMS::Catalog::QaMode() ? L"DeadAirRefined Review Identity QA" : L"DeadAirRefined Review Identity";
        SECURITY_STATUS status = NCryptOpenKey(provider, &m_key, name, 0, 0);
        if (status != ERROR_SUCCESS)
        {
            status = NCryptCreatePersistedKey(provider, &m_key, NCRYPT_ECDSA_P256_ALGORITHM, name, 0, 0);
            if (status == ERROR_SUCCESS)
                status = NCryptFinalizeKey(m_key, 0);
        }
        NCryptFreeObject(provider);
        if (status != ERROR_SUCCESS)
        {
            error = "the review identity could not be created";
            return false;
        }
        struct
        {
            BCRYPT_ECCKEY_BLOB header;
            u8 xy[64];
        } blob{};
        DWORD got = 0;
        if (NCryptExportKey(m_key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, reinterpret_cast<PBYTE>(&blob), sizeof(blob), &got, 0) !=
                ERROR_SUCCESS ||
            got != sizeof(blob) || blob.header.cbKey != 32)
        {
            error = "the review identity cannot be read";
            return false;
        }
        m_public = "p256:" + XMS::Catalog::Base64UrlEncode(blob.xy, 64);
        XMS::Catalog::PublicKey parsed;
        XMS::Catalog::ParsePublicKey(m_public, parsed);
        m_id = parsed.id;
        return true;
    }

    bool Sign(std::string_view message, u8 signature[64]) const
    {
        u8 digest[32];
        DWORD got = 0;
        return XMS::Catalog::Sha256(message.data(), message.size(), digest) &&
            NCryptSignHash(m_key, nullptr, digest, 32, signature, 64, &got, 0) == ERROR_SUCCESS && got == 64;
    }

    const xr_string& Public() const { return m_public; }
    const xr_string& Id() const { return m_id; }

private:
    NCRYPT_KEY_HANDLE m_key{};
    xr_string m_public;
    xr_string m_id;
};

void set_post(PostState state, xr_string error = {})
{
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    s.post.state = state;
    s.post.error = std::move(error);
}

// The identity's proof of work is paid once: the nonce is kept next to the catalog, where no
// script can reach it.
bool identity_proof(const Identity& identity, u32 bits, xr_string& nonce)
{
    const std::filesystem::path file = cache_path("identity.ltx");
    xr_string stored;
    if (read_file(file, 4096, stored))
    {
        std::string_view rest(stored);
        while (!rest.empty())
        {
            std::string_view line = next_line(rest);
            const size_t eq = line.find('=');
            if (eq != std::string_view::npos && trim(line.substr(0, eq)) == identity.Id())
            {
                nonce = trim(line.substr(eq + 1));
                const xr_string message = "xms-identity|" + identity.Public() + "|" + nonce;
                u8 digest[32];
                if (XMS::Catalog::Sha256(message.data(), message.size(), digest) && leading_zero_bits(digest) >= bits)
                    return true;
            }
        }
    }
    set_post(PostState::Identity);
    u64 value = 0;
    if (!proof_of_work("xms-identity|" + identity.Public() + "|", bits, value, service().stop))
        return false;
    nonce = std::to_string(value).c_str();
    write_file(file, "; the review identity's proof of work (MOD_CATALOG 12.1)\n" + identity.Id() + " = " + nonce + "\n");
    return true;
}

struct ReviewDraft
{
    xr_string id;
    xr_string version;
    u32 stars{};
    xr_string text;
    xr_string name;
};

void run_post(ModHttp::Session& session, const ReviewDraft& draft)
{
    const std::shared_ptr<const XMS::Catalog::Index> index = XMS::Catalog::Current();
    if (!index || index->reviews.inboxUrl.empty() || index->reviews.inboxField.empty())
    {
        set_post(PostState::Failed, "reviews cannot be written in this catalog");
        return;
    }
    Identity identity;
    xr_string error, identityNonce;
    if (!identity.Open(error))
    {
        set_post(PostState::Failed, error);
        return;
    }
    if (!identity_proof(identity, index->reviews.identityBits, identityNonce))
    {
        set_post(PostState::Failed, "cancelled");
        return;
    }
    set_post(PostState::Working);

    const xr_string text = XMS::Catalog::Base64UrlEncode(reinterpret_cast<const u8*>(draft.text.data()), draft.text.size());
    xr_string body = "xms-review 1\n";
    body += "mod = " + draft.id + "\n";
    body += "version = " + draft.version + "\n";
    body += "stars = " + xr_string(std::to_string(draft.stars).c_str()) + "\n";
    body += "date = " + today_utc() + "\n";
    body += "name = " + draft.name + "\n";
    body += "text = " + text + "\n";
    body += "hardware = " + hardware_hash(index->reviews.salt) + "\n";
    body += "identity = " + identity.Public() + "\n";
    body += "identity_pow = " + identityNonce + "\n";
    u64 nonce = 0;
    if (!proof_of_work("xms-review|" + body, index->reviews.reviewBits, nonce, service().stop))
    {
        set_post(PostState::Failed, "cancelled");
        return;
    }
    body += "pow = " + xr_string(std::to_string(nonce).c_str()) + "\n";
    u8 signature[64];
    if (!identity.Sign(body, signature))
    {
        set_post(PostState::Failed, "the review could not be signed");
        return;
    }
    body += "[signature]\nkey = " + identity.Public() + "\nsig = " + identity.Id() + ":" +
        XMS::Catalog::Base64UrlEncode(signature, 64) + "\n";

    set_post(PostState::Sending);
    const xr_string form = index->reviews.inboxField + "=" + ModHttp::UrlEncode(body);
    xr_string response;
    DWORD status = 0;
    if (!ModHttp::Exchange(session, L"POST", ModHttp::Widen(index->reviews.inboxUrl),
            L"Content-Type: application/x-www-form-urlencoded\r\n", form, 1024 * 1024, response, status, error) ||
        status < 200 || status >= 400)
    {
        set_post(PostState::Failed, error.empty() ? "HTTP " + xr_string(std::to_string(status).c_str()) : error);
        Msg("! [catalog] the review of [%s] was not sent: %s", draft.id.c_str(), service().post.error.c_str());
        return;
    }
    Msg("* [catalog] review of [%s] %s sent (%u star(s), identity %s)", draft.id.c_str(), draft.version.c_str(), draft.stars,
        identity.Id().c_str());
    set_post(PostState::Sent);
}
} // namespace

// ---- public -------------------------------------------------------------------------------------------------------

const ModCatalog::Entry* ModCatalog::Snapshot::Find(std::string_view id) const
{
    const auto found = std::ranges::find(entries, xr_string(id), &Entry::id);
    return found == entries.end() ? nullptr : &*found;
}

void ModCatalog::Refresh()
{
    CatalogService& s = service();
    {
        std::lock_guard lock(s.mutex);
        if (s.refreshing)
            return;
        s.refreshing = true;
    }
    enqueue(run_refresh);
}

std::shared_ptr<const Snapshot> ModCatalog::Current()
{
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    return s.snapshot;
}

void ModCatalog::WantThumbnail(pcstr id)
{
    const std::shared_ptr<const Snapshot> snapshot = Current();
    const Entry* entry = snapshot->Find(id);
    if (!entry || !entry->hasCard || !entry->release.card.thumbnail.present || !entry->thumbnailTexture.empty())
        return;
    const xr_string sha = entry->release.card.thumbnail.sha256;
    const u64 size = entry->release.card.thumbnail.size;
    CatalogService& s = service();
    {
        std::lock_guard lock(s.mutex);
        if (std::ranges::find(s.thumbnailsWanted, sha) != s.thumbnailsWanted.end())
            return;
        s.thumbnailsWanted.push_back(sha);
    }
    enqueue([sha, size](ModHttp::Session& session) { run_thumbnail(session, sha, size); });
}

void ModCatalog::MountThumbnails()
{
    CatalogService& s = service();
    xr_vector<std::pair<xr_string, std::filesystem::path>> ready;
    {
        std::lock_guard lock(s.mutex);
        ready.swap(s.toMount);
    }
    if (ready.empty())
        return;
    for (const auto& [sha, file] : ready)
    {
        const xr_string relative = "textures\\xms_thumbs\\" + sha + ".dds";
        if (!XMS::MountFile(relative.c_str(), file.string().c_str()))
            continue;
        std::lock_guard lock(s.mutex);
        s.mounted[sha.c_str()] = "xms_thumbs\\" + sha;
    }
    republish();
}

xr_string ModCatalog::InstalledThumbnail(const XMS::Module& module)
{
    if (module.thumbnail.empty())
        return {};
    CatalogService& s = service();
    {
        std::lock_guard lock(s.mutex);
        if (const auto known = s.installedThumbnails.find(module.id.c_str()); known != s.installedThumbnails.end())
            return known->second;
    }
    const xr_string physical = module.root + module.thumbnail;
    xr_string bytes, texture;
    if (read_file(std::filesystem::path(physical.c_str()), ThumbnailLimit, bytes) && valid_thumbnail(bytes))
    {
        // named by content, as the catalog's are: a module id has dots, and the texture loader
        // takes what follows the last one for an extension
        const xr_string sha = XMS::Catalog::Sha256Hex(bytes.data(), bytes.size());
        const xr_string relative = "textures\\xms_thumbs\\" + sha + ".dds";
        if (XMS::MountFile(relative.c_str(), physical.c_str()))
            texture = "xms_thumbs\\" + sha;
    }
    else
        Msg("~ [catalog] module [%s]: %s is not a thumbnail (256 x 256 DXT1/DXT5 DDS)", module.id.c_str(), module.thumbnail.c_str());
    std::lock_guard lock(s.mutex);
    s.installedThumbnails[module.id.c_str()] = texture;
    return texture;
}

void ModCatalog::WantReviews(pcstr id)
{
    CatalogService& s = service();
    const xr_string key(id);
    {
        std::lock_guard lock(s.mutex);
        const auto found = s.reviews.find(key.c_str());
        if (found != s.reviews.end() && found->second.reviews && found->second.reviews->status == Status::Loading)
            return;
        auto loading = std::make_shared<Reviews>();
        loading->id = key;
        loading->status = Status::Loading;
        if (found != s.reviews.end() && found->second.reviews)
            loading->list = found->second.reviews->list;
        s.reviews[key.c_str()].reviews = std::move(loading);
    }
    enqueue([key](ModHttp::Session& session) { run_reviews(session, key); });
}

std::shared_ptr<const ModCatalog::Reviews> ModCatalog::GetReviews(pcstr id)
{
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    const auto found = s.reviews.find(id);
    return found == s.reviews.end() ? nullptr : found->second.reviews;
}

bool ModCatalog::HasVirusTotalKey() { return !vt_key().empty(); }

bool ModCatalog::SetVirusTotalKey(pcstr key, xr_string& error)
{
    const xr_string value = lower(trim(key ? key : ""));
    if (!is_hex64(value))
    {
        error = "a VirusTotal key is 64 hexadecimal characters";
        return false;
    }
    if (XMS::Catalog::QaMode())
    {
        error = "the QA run takes its key from DAR_QA_VT_KEY";
        return false;
    }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(VtCredential);
    credential.UserName = const_cast<LPWSTR>(L"Dead Air Refined");
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0))
    {
        error = "the key could not be stored";
        return false;
    }
    return true;
}

void ModCatalog::ForgetVirusTotalKey()
{
    if (!XMS::Catalog::QaMode())
        CredDeleteW(VtCredential, CRED_TYPE_GENERIC, 0);
}

void ModCatalog::CheckVirusTotal(pcstr id)
{
    CatalogService& s = service();
    const xr_string key(id);
    {
        std::lock_guard lock(s.mutex);
        if (std::ranges::find(s.vtBusy, key) != s.vtBusy.end())
            return;
        s.vtBusy.push_back(key);
    }
    enqueue([key](ModHttp::Session& session) { run_vt(session, key); });
}

bool ModCatalog::VirusTotalBusy(pcstr id)
{
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    return std::ranges::find(s.vtBusy, xr_string(id)) != s.vtBusy.end();
}

Verdict ModCatalog::Judge(u32 trustedMalicious, u32 trustedSuspicious, u32 othersMalicious)
{
    const std::shared_ptr<const XMS::Catalog::Index> index = XMS::Catalog::Current();
    return judge(index ? index->vt : XMS::Catalog::VtPolicy{}, trustedMalicious, trustedSuspicious, othersMalicious);
}

size_t ModCatalog::TextLength(std::string_view utf8)
{
    utf8 = trim(utf8);
    return static_cast<size_t>(std::ranges::count_if(utf8, [](char c) { return (static_cast<u8>(c) & 0xC0) != 0x80; }));
}

bool ModCatalog::PostReview(pcstr id, u32 stars, const xr_string& text, const xr_string& name, xr_string& error)
{
    const std::shared_ptr<const Snapshot> snapshot = Current();
    const XMS::Module* module = id ? XMS::FindModule(id) : nullptr;
    const xr_string body(trim(text));
    const size_t length = TextLength(body);
    if (!snapshot->reviewsWritable)
        error = "reviews cannot be written in this catalog";
    else if (!module || !snapshot->Find(id))
        error = "only an installed module of the catalog can be reviewed";
    else if (stars < 1 || stars > 5)
        error = "pick from one to five stars";
    else if (length < ReviewMinimum || length > ReviewMaximum)
        error = "a review is 30 to 2000 characters long";
    if (!error.empty())
        return false;

    CatalogService& s = service();
    {
        std::lock_guard lock(s.mutex);
        if (s.post.state == PostState::Identity || s.post.state == PostState::Working || s.post.state == PostState::Sending)
        {
            error = "a review is being sent";
            return false;
        }
        s.post = {};
        s.post.state = PostState::Working;
        s.post.id = id;
    }
    ReviewDraft draft;
    draft.id = id;
    draft.version = module->version;
    draft.stars = stars;
    draft.text = plain_text(body, true);
    xr_string shortName;
    for (size_t i = 0, count = 0; i < name.size() && count < ReviewNameMaximum; ++count)
    {
        size_t step = 1;
        const u8 c = static_cast<u8>(name[i]);
        if (c >= 0xF0)
            step = 4;
        else if (c >= 0xE0)
            step = 3;
        else if (c >= 0xC0)
            step = 2;
        shortName += name.substr(i, step);
        i += step;
    }
    draft.name = xr_string(trim(plain_text(shortName, false)));
    enqueue([draft](ModHttp::Session& session) { run_post(session, draft); });
    return true;
}

ModCatalog::PostStatus ModCatalog::GetPostStatus()
{
    CatalogService& s = service();
    std::lock_guard lock(s.mutex);
    return s.post;
}

bool ModCatalog::OpenVirusTotalReport(pcstr id)
{
    const std::shared_ptr<const Snapshot> snapshot = Current();
    const Entry* entry = snapshot->Find(id);
    if (!entry || !entry->hasCard || entry->release.packages.empty())
        return false;
    // the report of the package the verdict comes from: one part of a split release can be flagged alone
    const xr_string& sha = is_hex64(entry->vt.package) ? entry->vt.package : entry->release.packages.front().sha256;
    if (!is_hex64(sha))
        return false;
    // built from the hash alone: nothing a card says reaches the command line
    const std::wstring url = L"https://www.virustotal.com/gui/file/" + ModHttp::Widen(sha);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool ModCatalog::OpenWebsite(pcstr id)
{
    const std::shared_ptr<const Snapshot> snapshot = Current();
    const Entry* entry = snapshot->Find(id);
    // the allow-list of the manifest holds for a card too (MOD_CATALOG 7)
    if (!entry || !entry->hasCard || !XMS::ValidWebsite(entry->release.card.website.c_str()))
        return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
               nullptr, L"open", ModHttp::Widen(entry->release.card.website).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

void ModCatalog::Tick()
{
    MountThumbnails();
    CatalogService& s = service();
    if (s.deferred.empty())
        return;
    const Status status = Current()->status;
    bool loading = false;
    {
        std::lock_guard lock(s.mutex);
        loading = s.refreshing;
    }
    if (loading || status == Status::Idle || status == Status::Loading || s.pending.load(std::memory_order_acquire))
        return;
    // one per frame: a command that queues work (a VirusTotal lookup) is done before the next runs
    const auto [command, origin] = s.deferred.front();
    s.deferred.erase(s.deferred.begin());
    // run as whoever asked: the player's "after" stays the player's (MOD_CATALOG 9.2)
    XMS::Protect::OriginScope scope(origin);
    Console->Execute(command.c_str());
}

void ModCatalog::RunWhenLoaded(const xr_string& command)
{
    service().deferred.emplace_back(command, XMS::Protect::CurrentOrigin());
}

void ModCatalog::Shutdown() { service().Stop(); }
#else
namespace ModCatalog
{
const Entry* Snapshot::Find(std::string_view) const { return nullptr; }
void Refresh() {}
std::shared_ptr<const Snapshot> Current() { return std::make_shared<Snapshot>(); }
void WantThumbnail(pcstr) {}
void MountThumbnails() {}
void Tick() {}
void RunWhenLoaded(const xr_string&) {}
xr_string InstalledThumbnail(const XMS::Module&) { return {}; }
void WantReviews(pcstr) {}
std::shared_ptr<const Reviews> GetReviews(pcstr) { return nullptr; }
bool HasVirusTotalKey() { return false; }
bool SetVirusTotalKey(pcstr, xr_string& error) { error = "not supported"; return false; }
void ForgetVirusTotalKey() {}
void CheckVirusTotal(pcstr) {}
bool VirusTotalBusy(pcstr) { return false; }
Verdict Judge(u32, u32, u32) { return Verdict::Unknown; }
size_t TextLength(std::string_view) { return 0; }
bool PostReview(pcstr, u32, const xr_string&, const xr_string&, xr_string& error) { error = "not supported"; return false; }
PostStatus GetPostStatus() { return {}; }
bool OpenVirusTotalReport(pcstr) { return false; }
bool OpenWebsite(pcstr) { return false; }
void Shutdown() {}
}
#endif
