// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UpdateDelta.h"

#ifdef XR_PLATFORM_WINDOWS
#include "xrContentSync/ContentHash.h"

#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
constexpr u32 SignatureLocal = 0x04034b50;
constexpr u32 SignatureCentral = 0x02014b50;
constexpr u32 SignatureEnd = 0x06054b50;
constexpr u32 SignatureEnd64 = 0x06064b50;
constexpr u32 SignatureLocator64 = 0x07064b50;
constexpr u64 Mask32 = 0xffffffffull;

// The end of the directory record and the longest comment it may carry.
constexpr u64 TailBytes = 22 + 65535;
// An archive of a thousand files has a directory of about a hundred kilobytes; anything far past
// that is not an update archive.
constexpr u64 MaximumDirectoryBytes = 8 * 1024 * 1024;
constexpr u64 MaximumEntries = 4096;
constexpr u64 MaximumManifestBytes = 1024 * 1024;
// The applier refuses more than this many files, so a release with more is not one it can take.
constexpr u32 MaximumFiles = 1024;
// Records closer together than this are fetched in one request, gap and all.
constexpr u64 RangeGap = 256 * 1024;
constexpr unsigned Attempts = 5;
constexpr pcstr ManifestName = "update-manifest.txt";
constexpr std::string_view SchemaFull = "schema=dead-air-refined.update/1";

u16 le16(const u8* data) { return static_cast<u16>(data[0] | data[1] << 8); }
u32 le32(const u8* data) { return static_cast<u32>(le16(data)) | static_cast<u32>(le16(data + 2)) << 16; }
u64 le64(const u8* data) { return static_cast<u64>(le32(data)) | static_cast<u64>(le32(data + 4)) << 32; }

void put16(std::vector<u8>& out, u16 value)
{
    out.push_back(static_cast<u8>(value));
    out.push_back(static_cast<u8>(value >> 8));
}

void put32(std::vector<u8>& out, u32 value)
{
    put16(out, static_cast<u16>(value));
    put16(out, static_cast<u16>(value >> 16));
}

bool sleep_unless_stopped(unsigned milliseconds, const std::atomic_bool& stop)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 100)
    {
        if (stop.load(std::memory_order_acquire))
            return false;
        Sleep(100);
    }
    return !stop.load(std::memory_order_acquire);
}

std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

// The applier's own rule (DeadAirUpdater.cpp, safe_relative_path), so a manifest it would refuse
// is refused here before anything is downloaded for it.
bool safe_path(std::string_view path)
{
    if (path.empty() || path.front() == '/' || path.find_first_of(":\\") != std::string_view::npos)
        return false;
    while (!path.empty())
    {
        const size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            return false;
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    }
    return true;
}

bool same_hash(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && std::ranges::equal(left, right,
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

// [begin, end) of the archive into memory.
bool fetch_range(ModHttp::Session& session, const std::wstring& url, u64 begin, u64 end, std::vector<u8>& out,
    xr_string& error, const std::atomic_bool& stop)
{
    out.clear();
    for (unsigned failures = 0; out.size() < end - begin;)
    {
        if (failures == Attempts || (failures && !sleep_unless_stopped(500u << failures, stop)) ||
            stop.load(std::memory_order_acquire))
        {
            if (stop.load(std::memory_order_acquire))
                error = "cancelled";
            return false;
        }
        const u64 from = begin + out.size();
        ModHttp::Request request;
        if (!request.Send(session, url, from, end, error))
        {
            ++failures;
            continue;
        }
        u64 start = 0;
        if (request.Status() != HTTP_STATUS_PARTIAL_CONTENT || !request.RangeStart(start) || start != from)
        {
            // A server that ignores ranges is answered with the whole archive instead, by the caller.
            error = request.Status() == HTTP_STATUS_OK ? "the server does not answer ranged requests" :
                                                         "HTTP " + xr_string(std::to_string(request.Status()).c_str());
            if (request.Status() == HTTP_STATUS_OK || request.Status() == HTTP_STATUS_NOT_FOUND)
                return false;
            ++failures;
            continue;
        }
        std::array<u8, 64 * 1024> buffer{};
        while (out.size() < end - begin)
        {
            DWORD received = 0;
            const DWORD want = static_cast<DWORD>(std::min<u64>(buffer.size(), end - begin - out.size()));
            if (stop.load(std::memory_order_acquire) || !request.Read(buffer.data(), want, received) || !received)
            {
                error = "the download was interrupted";
                ++failures;
                break;
            }
            out.insert(out.end(), buffer.begin(), buffer.begin() + received);
            failures = 0;
        }
    }
    // What an earlier attempt failed with no longer describes anything.
    error.clear();
    return true;
}

struct CentralEntry
{
    xr_string name;
    u64 header{};
    u64 packed{};
    u64 size{};
    u32 crc{};
    u16 method{};
    u16 flags{};
    u16 time{};
    u16 date{};
};

bool parse_directory(const u8* data, size_t size, u64 count, xr_vector<CentralEntry>& entries, xr_string& error)
{
    size_t offset = 0;
    for (u64 index = 0; index != count; ++index)
    {
        if (size - offset < 46 || le32(data + offset) != SignatureCentral)
        {
            error = "the archive directory is damaged";
            return false;
        }
        const u8* record = data + offset;
        CentralEntry entry;
        entry.flags = le16(record + 8);
        entry.method = le16(record + 10);
        entry.time = le16(record + 12);
        entry.date = le16(record + 14);
        entry.crc = le32(record + 16);
        entry.packed = le32(record + 20);
        entry.size = le32(record + 24);
        const u16 nameLength = le16(record + 28);
        const u16 extraLength = le16(record + 30);
        const u16 commentLength = le16(record + 32);
        entry.header = le32(record + 42);
        if (size - offset < 46ull + nameLength + extraLength + commentLength)
        {
            error = "the archive directory is damaged";
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(record + 46), nameLength);

        // ZIP64 values replace exactly the fields saturated at 32 bits, in this order.
        for (size_t extra = 46 + nameLength; extra + 4 <= 46ull + nameLength + extraLength;)
        {
            const u16 id = le16(record + extra);
            const u16 length = le16(record + extra + 2);
            if (extra + 4 + length > 46ull + nameLength + extraLength)
                break;
            if (id == 0x0001)
            {
                size_t field = extra + 4;
                const size_t fieldEnd = field + length;
                for (u64* value : {&entry.size, &entry.packed, &entry.header})
                {
                    if (*value != Mask32)
                        continue;
                    if (field + 8 > fieldEnd)
                        break;
                    *value = le64(record + field);
                    field += 8;
                }
            }
            extra += 4 + length;
        }

        // Encrypted, or packed some way the applier's minizip cannot read either.
        if (entry.flags & 1 || (entry.method != 0 && entry.method != 8))
        {
            error = "the archive holds an entry this client cannot read: " + entry.name;
            return false;
        }
        entries.push_back(std::move(entry));
        offset += 46ull + nameLength + extraLength + commentLength;
    }
    return true;
}

// Reads one local record off a byte stream and checks what it carries. Survives a dropped
// connection: what it has taken so far is kept, and the next request starts at the byte after.
class RecordReader
{
public:
    explicit RecordReader(const CentralEntry& entry) : m_entry(entry) {}
    ~RecordReader()
    {
        if (m_inflating)
            inflateEnd(&m_stream);
    }
    RecordReader(const RecordReader&) = delete;
    RecordReader& operator=(const RecordReader&) = delete;

    bool Open(xr_string& error)
    {
        if (!m_digest.Open())
        {
            error = "the hashing provider could not start";
            return false;
        }
        if (m_entry.method == 8)
        {
            if (inflateInit2(&m_stream, -MAX_WBITS) != Z_OK)
            {
                error = "could not start unpacking";
                return false;
            }
            m_inflating = true;
        }
        m_output.resize(256 * 1024);
        return true;
    }

    // Bytes of the record still to come before the data is complete.
    u64 Needed() const
    {
        if (m_header.size() < 30)
            return 30 - m_header.size();
        if (m_header.size() < m_headerLength)
            return m_headerLength - m_header.size();
        return m_entry.packed - m_taken;
    }

    bool Done() const { return m_header.size() >= 30 && m_header.size() == m_headerLength && m_taken == m_entry.packed; }

    // Feeds the next bytes of the record. `data` receives the stored bytes as they pass, so the
    // caller can copy them into the patch untouched.
    bool Feed(const u8* bytes, size_t count, const std::function<bool(const u8*, size_t)>& data, xr_string& error)
    {
        while (count)
        {
            if (m_header.size() < 30 || m_header.size() < m_headerLength)
            {
                const size_t take = static_cast<size_t>(std::min<u64>(count, Needed()));
                m_header.insert(m_header.end(), bytes, bytes + take);
                bytes += take;
                count -= take;
                if (m_header.size() == 30)
                {
                    if (le32(m_header.data()) != SignatureLocal)
                    {
                        error = "the archive record of " + m_entry.name + " is damaged";
                        return false;
                    }
                    m_headerLength = 30ull + le16(m_header.data() + 26) + le16(m_header.data() + 28);
                }
                if (m_header.size() == m_headerLength && (le16(m_header.data() + 26) != m_entry.name.size() ||
                    std::memcmp(m_header.data() + 30, m_entry.name.data(), m_entry.name.size()) != 0))
                {
                    error = "the archive record of " + m_entry.name + " names another file";
                    return false;
                }
                continue;
            }

            const size_t take = static_cast<size_t>(std::min<u64>(count, m_entry.packed - m_taken));
            if (!take)
                break;
            if (!data(bytes, take) || !Unpack(bytes, take, error))
                return false;
            m_taken += take;
            bytes += take;
            count -= take;
        }
        return true;
    }

    bool Finish(std::string_view sha256, u64 size, xr_string& error)
    {
        // Output can still sit inside the inflater after the last input byte.
        if (m_inflating && !m_ended && !Unpack(nullptr, 0, error))
            return false;
        const bool whole = (!m_inflating || m_ended) && m_written == size && m_written == m_entry.size &&
            m_crc == m_entry.crc && same_hash(m_digest.Finish(), sha256);
        if (!whole)
            error = m_entry.name + " does not match the manifest";
        return whole;
    }

private:
    bool Unpack(const u8* bytes, size_t count, xr_string& error)
    {
        if (!m_inflating)
            return Take(bytes, count, error);

        m_stream.next_in = const_cast<Bytef*>(bytes);
        m_stream.avail_in = static_cast<uInt>(count);
        do
        {
            m_stream.next_out = m_output.data();
            m_stream.avail_out = static_cast<uInt>(m_output.size());
            const int result = inflate(&m_stream, Z_NO_FLUSH);
            // No progress is possible until more input arrives - the last call used it all up and
            // filled the output exactly. Not damage.
            if (result == Z_BUF_ERROR && !m_stream.avail_in)
                break;
            if (result != Z_OK && result != Z_STREAM_END)
            {
                error = m_entry.name + " is damaged in the archive";
                return false;
            }
            if (!Take(m_output.data(), m_output.size() - m_stream.avail_out, error))
                return false;
            m_ended = result == Z_STREAM_END;
        } while (!m_ended && (m_stream.avail_in || !m_stream.avail_out));
        if (m_stream.avail_in)
        {
            error = m_entry.name + " is damaged in the archive";
            return false;
        }
        return true;
    }

    bool Take(const u8* bytes, size_t count, xr_string& error)
    {
        m_written += count;
        if (m_written > m_entry.size)
        {
            error = m_entry.name + " is larger than the archive declares";
            return false;
        }
        m_crc = static_cast<u32>(crc32(m_crc, bytes, static_cast<uInt>(count)));
        return m_digest.Append(bytes, count);
    }

    const CentralEntry& m_entry;
    std::vector<u8> m_header;
    u64 m_headerLength{};
    u64 m_taken{};
    u64 m_written{};
    u32 m_crc{};
    z_stream m_stream{};
    bool m_inflating{};
    bool m_ended{};
    ContentHash::Stream m_digest;
    std::vector<u8> m_output;
};

struct Span
{
    size_t first{};
    size_t last{};
    u64 begin{};
    u64 end{};
};

// Consecutive missing records, merged across gaps cheaper to download than to skip with a new
// request.
xr_vector<Span> spans_of(const xr_vector<UpdateDelta::Entry>& missing)
{
    xr_vector<Span> spans;
    for (size_t index = 0; index != missing.size(); ++index)
    {
        if (!spans.empty() && missing[index].header <= spans.back().end + RangeGap)
        {
            spans.back().last = index;
            spans.back().end = std::max(spans.back().end, missing[index].end);
            continue;
        }
        spans.push_back({index, index, missing[index].header, missing[index].end});
    }
    return spans;
}

bool parse_manifest(std::string_view text, const xr_string& version, xr_vector<std::array<std::string_view, 3>>& rows,
    std::string_view& body, xr_string& error)
{
    const auto next_line = [&](std::string_view& line)
    {
        if (text.empty())
            return false;
        const size_t end = text.find('\n');
        line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        return true;
    };

    std::string_view line;
    if (!next_line(line) || line != SchemaFull || !next_line(line) || line != "version=" + std::string(version.c_str()))
    {
        error = "the archive's manifest is not the full manifest of " + version;
        return false;
    }
    body = text;
    while (next_line(line))
    {
        if (line.empty())
            continue;
        const size_t first = line.find('\t');
        const size_t second = first == std::string_view::npos ? first : line.find('\t', first + 1);
        if (first != 64 || second == std::string_view::npos)
        {
            error = "the archive's manifest is malformed";
            return false;
        }
        rows.push_back({line.substr(0, first), line.substr(first + 1, second - first - 1), line.substr(second + 1)});
    }
    if (rows.empty() || rows.size() > MaximumFiles)
    {
        error = "the archive's manifest declares an impossible file count";
        return false;
    }
    return true;
}
}

bool UpdateDelta::Prepare(ModHttp::Session& session, const std::wstring& url, u64 archiveSize, const xr_string& version,
    const std::filesystem::path& root, Plan& plan, xr_string& error, const std::atomic_bool& stop)
{
    plan = {};
    plan.version = version;

    // The end of the directory, read from the tail of the archive.
    const u64 tailStart = archiveSize > TailBytes ? archiveSize - TailBytes : 0;
    std::vector<u8> tail;
    if (archiveSize < 22 || !fetch_range(session, url, tailStart, archiveSize, tail, error, stop))
        return false;

    size_t end = tail.size() - 22;
    for (;; --end)
    {
        if (le32(tail.data() + end) == SignatureEnd && end + 22 + le16(tail.data() + end + 20) == tail.size())
            break;
        if (!end)
        {
            error = "the archive has no directory";
            return false;
        }
    }
    u64 entries = le16(tail.data() + end + 10);
    u64 directorySize = le32(tail.data() + end + 12);
    u64 directoryOffset = le32(tail.data() + end + 16);
    if (entries == 0xffff || directorySize == Mask32 || directoryOffset == Mask32)
    {
        std::vector<u8> record;
        if (end < 20 || le32(tail.data() + end - 20) != SignatureLocator64 ||
            !fetch_range(session, url, le64(tail.data() + end - 12), le64(tail.data() + end - 12) + 56, record, error, stop) ||
            le32(record.data()) != SignatureEnd64)
        {
            if (error.empty())
                error = "the archive's ZIP64 directory is damaged";
            return false;
        }
        entries = le64(record.data() + 32);
        directorySize = le64(record.data() + 40);
        directoryOffset = le64(record.data() + 48);
    }
    if (entries > MaximumEntries || directorySize > MaximumDirectoryBytes || directoryOffset > archiveSize ||
        directorySize > archiveSize - directoryOffset)
    {
        error = "the archive directory is out of bounds";
        return false;
    }

    std::vector<u8> directory;
    if (directoryOffset >= tailStart && directoryOffset + directorySize <= archiveSize)
        directory.assign(tail.begin() + static_cast<ptrdiff_t>(directoryOffset - tailStart),
            tail.begin() + static_cast<ptrdiff_t>(directoryOffset - tailStart + directorySize));
    else if (!fetch_range(session, url, directoryOffset, directoryOffset + directorySize, directory, error, stop))
        return false;

    xr_vector<CentralEntry> central;
    if (!parse_directory(directory.data(), directory.size(), entries, central, error))
        return false;

    // Where each record ends: where the next one starts, or the directory for the last.
    std::ranges::sort(central, {}, &CentralEntry::header);
    std::unordered_map<std::string, size_t> byName;
    for (size_t index = 0; index != central.size(); ++index)
    {
        if (central[index].header >= directoryOffset || !byName.emplace(central[index].name.c_str(), index).second)
        {
            error = "the archive directory is damaged";
            return false;
        }
    }
    const auto record_end = [&](size_t index) { return index + 1 != central.size() ? central[index + 1].header : directoryOffset; };

    const auto manifestEntry = byName.find(ManifestName);
    if (manifestEntry == byName.end())
    {
        error = "the archive holds no manifest";
        return false;
    }
    const CentralEntry& manifestRecord = central[manifestEntry->second];
    const u64 manifestEnd = record_end(manifestEntry->second);
    if (manifestRecord.size > MaximumManifestBytes || manifestEnd - manifestRecord.header > MaximumManifestBytes + 65536)
    {
        error = "the archive's manifest is too large";
        return false;
    }

    std::vector<u8> record;
    if (!fetch_range(session, url, manifestRecord.header, manifestEnd, record, error, stop))
        return false;
    const u64 dataStart = record.size() >= 30 ? 30ull + le16(record.data() + 26) + le16(record.data() + 28) : 0;
    if (!dataStart || le32(record.data()) != SignatureLocal || le16(record.data() + 26) != manifestRecord.name.size() ||
        std::memcmp(record.data() + 30, manifestRecord.name.data(), manifestRecord.name.size()) != 0 ||
        dataStart + manifestRecord.packed > record.size())
    {
        error = "the archive's manifest record is damaged";
        return false;
    }

    // Nothing else describes the manifest, so its archive CRC is what it is checked by.
    xr_string manifest;
    manifest.resize(static_cast<size_t>(manifestRecord.size));
    if (manifestRecord.method == 8)
    {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        {
            error = "could not start unpacking";
            return false;
        }
        stream.next_in = record.data() + dataStart;
        stream.avail_in = static_cast<uInt>(manifestRecord.packed);
        stream.next_out = reinterpret_cast<Bytef*>(manifest.data());
        stream.avail_out = static_cast<uInt>(manifest.size());
        const int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (result != Z_STREAM_END || stream.avail_out)
        {
            error = "the archive's manifest is damaged";
            return false;
        }
    }
    else if (manifestRecord.packed == manifestRecord.size)
        std::memcpy(manifest.data(), record.data() + dataStart, manifest.size());
    if (static_cast<u32>(crc32(0, reinterpret_cast<const Bytef*>(manifest.data()), static_cast<uInt>(manifest.size()))) !=
        manifestRecord.crc)
    {
        error = "the archive's manifest is damaged";
        return false;
    }

    xr_vector<std::array<std::string_view, 3>> rows;
    std::string_view body;
    if (!parse_manifest(manifest, version, rows, body, error))
        return false;
    plan.rows.assign(body.data(), body.size());
    plan.files = static_cast<u32>(rows.size());

    // What the installation already holds is what it hashes to; nothing is taken on trust.
    for (const auto& [hash, sizeText, path] : rows)
    {
        if (stop.load(std::memory_order_acquire))
        {
            error = "cancelled";
            return false;
        }
        u64 size = 0;
        const auto [sizeEnd, sizeError] = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size);
        const auto found = byName.find(std::string(path));
        if (sizeError != std::errc{} || sizeEnd != sizeText.data() + sizeText.size() || !safe_path(path) ||
            found == byName.end() || central[found->second].size != size)
        {
            error = "the archive does not hold " + xr_string(path.data(), path.size()) + " as its manifest declares";
            return false;
        }

        const std::filesystem::path local = root / utf8_to_wide(path);
        std::error_code sizeCheck;
        if (std::filesystem::is_regular_file(local, sizeCheck) && std::filesystem::file_size(local, sizeCheck) == size &&
            !sizeCheck && same_hash(ContentHash::File(local.wstring(), stop), hash))
        {
            continue;
        }

        const CentralEntry& entry = central[found->second];
        if (entry.packed >= Mask32 || entry.size >= Mask32)
        {
            error = "the archive holds a file too large for a patch: " + entry.name;
            return false;
        }
        Entry missing;
        missing.path = entry.name;
        missing.sha256.assign(hash.data(), hash.size());
        missing.size = size;
        missing.header = entry.header;
        missing.end = record_end(found->second);
        missing.packed = entry.packed;
        missing.crc = entry.crc;
        missing.method = entry.method;
        missing.flags = entry.flags;
        missing.time = entry.time;
        missing.date = entry.date;
        plan.missing.push_back(std::move(missing));
    }
    for (const CentralEntry& entry : central)
    {
        if (!entry.name.empty() && entry.name.back() == '/' && safe_path(std::string_view(entry.name.c_str(), entry.name.size() - 1)))
            plan.directories.push_back(entry.name);
    }

    std::ranges::sort(plan.missing, {}, &Entry::header);
    for (const Span& span : spans_of(plan.missing))
        plan.downloadBytes += span.end - span.begin;
    return true;
}

bool UpdateDelta::Build(ModHttp::Session& session, const std::wstring& url, const Plan& plan,
    pcstr baseVersion, const std::filesystem::path& archive, xr_string& digest, std::atomic<u64>& progress,
    xr_string& error, const std::atomic_bool& stop)
{
    std::error_code fileError;
    std::filesystem::create_directories(archive.parent_path(), fileError);
    const HANDLE file = CreateFileW(archive.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "could not create the update archive";
        return false;
    }

    ContentHash::Stream whole;
    u64 written = 0;
    bool ok = whole.Open();
    const auto write = [&](const void* bytes, size_t count)
    {
        DWORD done = 0;
        if (ok && !(WriteFile(file, bytes, static_cast<DWORD>(count), &done, nullptr) && done == count &&
                whole.Append(bytes, count)))
        {
            ok = false;
            error = "could not write the update archive";
        }
        written += count;
        return ok;
    };

    struct Written
    {
        xr_string name;
        u64 header{};
        u64 packed{};
        u64 size{};
        u32 crc{};
        u16 method{};
        u16 flags{};
        u16 time{};
        u16 date{};
        bool directory{};
    };
    xr_vector<Written> directory;

    // The records of the patch carry the release's own sizes, CRC and stored bytes; only the
    // offsets are new. Bit 11 (UTF-8 names) is the one flag worth keeping, and with the sizes in
    // the header there is no data descriptor.
    const auto local_header = [&](Written& record)
    {
        record.header = written;
        std::vector<u8> header;
        put32(header, SignatureLocal);
        put16(header, record.method == 8 ? 20 : 10);
        put16(header, record.flags);
        put16(header, record.method);
        put16(header, record.time);
        put16(header, record.date);
        put32(header, record.crc);
        put32(header, static_cast<u32>(record.packed));
        put32(header, static_cast<u32>(record.size));
        put16(header, static_cast<u16>(record.name.size()));
        put16(header, 0);
        header.insert(header.end(), record.name.begin(), record.name.end());
        return write(header.data(), header.size());
    };

    for (const xr_string& name : plan.directories)
    {
        Written record;
        record.name = name;
        record.directory = true;
        if (!local_header(record))
            break;
        directory.push_back(std::move(record));
    }

    // The missing records, span by span. A dropped connection resumes at the byte it stopped at,
    // inside a record if need be: the reader keeps its state and the patch its bytes.
    const xr_vector<Span> spans = spans_of(plan.missing);
    std::wstring direct;
    for (const Span& span : spans)
    {
        if (!ok)
            break;
        size_t next = span.first;
        u64 position = span.begin;
        std::unique_ptr<RecordReader> reader;
        std::unique_ptr<CentralEntry> central;
        for (unsigned failures = 0; ok && next <= span.last;)
        {
            if (failures == Attempts || (failures && !sleep_unless_stopped(500u << failures, stop)) ||
                stop.load(std::memory_order_acquire))
            {
                ok = false;
                if (error.empty() || stop.load(std::memory_order_acquire))
                    error = stop.load(std::memory_order_acquire) ? "cancelled" : "the download kept failing";
                break;
            }

            ModHttp::Request request;
            const bool viaDirect = !direct.empty();
            if (!request.Send(session, viaDirect ? direct : url, position, span.end, error))
            {
                direct.clear();
                ++failures;
                continue;
            }
            u64 start = 0;
            if (request.Status() != HTTP_STATUS_PARTIAL_CONTENT || !request.RangeStart(start) || start != position)
            {
                error = "HTTP " + xr_string(std::to_string(request.Status()).c_str());
                if (viaDirect)
                    direct.clear();
                else if (request.Status() == HTTP_STATUS_OK || request.Status() == HTTP_STATUS_NOT_FOUND)
                {
                    ok = false;
                    break;
                }
                else
                    ++failures;
                continue;
            }
            if (!viaDirect)
                direct = request.FinalUrl();

            std::vector<u8> buffer(256 * 1024);
            while (ok && next <= span.last && position < span.end)
            {
                DWORD received = 0;
                const DWORD want = static_cast<DWORD>(std::min<u64>(buffer.size(), span.end - position));
                if (stop.load(std::memory_order_acquire) || !request.Read(buffer.data(), want, received) || !received)
                {
                    error = "the download was interrupted";
                    ++failures;
                    break;
                }
                failures = 0;
                progress.fetch_add(received, std::memory_order_acq_rel);

                const u8* bytes = buffer.data();
                size_t count = received;
                while (ok && count && next <= span.last)
                {
                    const UpdateDelta::Entry& entry = plan.missing[next];
                    // The gap before a record, or the tail after one, is skipped.
                    if (position < entry.header || (reader && reader->Done()))
                    {
                        const u64 skipTo = reader && reader->Done() ? entry.end : entry.header;
                        const size_t skip = static_cast<size_t>(std::min<u64>(count, skipTo - position));
                        bytes += skip;
                        count -= skip;
                        position += skip;
                        if (reader && reader->Done() && position == entry.end)
                        {
                            reader.reset();
                            central.reset();
                            ++next;
                        }
                        continue;
                    }

                    if (!reader)
                    {
                        central = std::make_unique<CentralEntry>();
                        central->name = entry.path;
                        central->header = entry.header;
                        central->packed = entry.packed;
                        central->size = entry.size;
                        central->crc = entry.crc;
                        central->method = entry.method;
                        central->flags = entry.flags & 0x0800;
                        central->time = entry.time;
                        central->date = entry.date;
                        reader = std::make_unique<RecordReader>(*central);
                        Written record{entry.path, 0, entry.packed, entry.size, entry.crc, entry.method,
                            static_cast<u16>(entry.flags & 0x0800), entry.time, entry.date, false};
                        if (!reader->Open(error) || !local_header(record))
                        {
                            ok = false;
                            break;
                        }
                        directory.push_back(std::move(record));
                    }

                    const size_t take = static_cast<size_t>(std::min<u64>(count, reader->Needed()));
                    if (!reader->Feed(bytes, take, [&](const u8* data, size_t size) { return write(data, size); }, error))
                    {
                        ok = false;
                        break;
                    }
                    bytes += take;
                    count -= take;
                    position += take;
                    if (reader->Done() && !reader->Finish(entry.sha256, entry.size, error))
                    {
                        ok = false;
                        break;
                    }
                    if (reader->Done() && position == entry.end)
                    {
                        reader.reset();
                        central.reset();
                        ++next;
                    }
                }
            }
            // A record that ends exactly where the span ends is finished by the loop above; one
            // whose tail runs past it cannot be, so it is finished here.
            if (ok && next == span.last && reader && reader->Done() && position == span.end)
            {
                reader.reset();
                central.reset();
                ++next;
            }
        }
    }

    // The manifest of a patch: the release's file set, cut against the installed version.
    if (ok)
    {
        xr_string text = "schema=dead-air-refined.update/2\nversion=" + plan.version + "\nkind=patch\nbase=" +
            xr_string(baseVersion) + "\n" + plan.rows;
        Written record;
        record.name = ManifestName;
        record.size = record.packed = text.size();
        record.crc = static_cast<u32>(crc32(0, reinterpret_cast<const Bytef*>(text.data()), static_cast<uInt>(text.size())));
        if (local_header(record) && write(text.data(), text.size()))
            directory.push_back(std::move(record));
    }

    if (ok)
    {
        const u64 directoryOffset = written;
        std::vector<u8> central;
        for (const Written& record : directory)
        {
            put32(central, SignatureCentral);
            put16(central, 20);
            put16(central, record.method == 8 ? 20 : 10);
            put16(central, record.flags);
            put16(central, record.method);
            put16(central, record.time);
            put16(central, record.date);
            put32(central, record.crc);
            put32(central, static_cast<u32>(record.packed));
            put32(central, static_cast<u32>(record.size));
            put16(central, static_cast<u16>(record.name.size()));
            put16(central, 0);
            put16(central, 0);
            put16(central, 0);
            put16(central, 0);
            put32(central, record.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
            put32(central, static_cast<u32>(record.header));
            central.insert(central.end(), record.name.begin(), record.name.end());
        }
        std::vector<u8> end;
        put32(end, SignatureEnd);
        put16(end, 0);
        put16(end, 0);
        put16(end, static_cast<u16>(directory.size()));
        put16(end, static_cast<u16>(directory.size()));
        put32(end, static_cast<u32>(central.size()));
        put32(end, static_cast<u32>(directoryOffset));
        put16(end, 0);
        write(central.data(), central.size()) && write(end.data(), end.size());
    }

    FlushFileBuffers(file);
    CloseHandle(file);
    if (ok)
        digest = whole.Finish().c_str();
    if (!ok || digest.empty())
    {
        if (error.empty())
            error = "could not finish the update archive";
        DeleteFileW(archive.c_str());
        return false;
    }
    return true;
}
#endif
