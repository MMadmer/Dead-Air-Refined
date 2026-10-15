// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "ContentManifest.h"

#include "ContentHash.h"

#include <algorithm>
#include <charconv>
#include <fstream>

namespace
{
// The grammar is deliberately unforgiving: exact prefixes, no whitespace tolerance, fixed
// field counts. A manifest is machine-written by the packaging script and consumed by code
// that deletes files on its authority, so "be liberal in what you accept" is the wrong
// instinct here - a typo must fail loudly at build time, not be interpreted at runtime.

bool next_line(std::string_view& text, std::string_view& line)
{
    if (text.empty())
        return false;
    const size_t end = text.find('\n');
    line = text.substr(0, end == std::string_view::npos ? text.size() : end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    return true;
}

bool take_prefixed(std::string_view line, std::string_view prefix, std::string& out)
{
    if (!line.starts_with(prefix))
        return false;
    out.assign(line.substr(prefix.size()));
    return !out.empty();
}

bool parse_u64(std::string_view value, std::uint64_t& out)
{
    if (value.empty())
        return false;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    return error == std::errc{} && end == value.data() + value.size();
}

// Splits on tabs and requires exactly `count` fields - not "at least", so a stray tab in a
// path is caught here rather than producing a plausible-looking wrong name.
bool split_tabs(std::string_view line, std::vector<std::string_view>& fields, size_t count)
{
    fields.clear();
    size_t start = 0;
    while (true)
    {
        const size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos)
        {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
        if (fields.size() > count)
            return false;
    }
    return fields.size() == count;
}

bool valid_version(std::string_view value)
{
    size_t component = 0;
    size_t digits = 0;
    for (const char character : value)
    {
        if (character >= '0' && character <= '9')
        {
            ++digits;
            continue;
        }
        if (character != '.' || !digits || component == 2)
            return false;
        ++component;
        digits = 0;
    }
    return component == 2 && digits != 0;
}
}

namespace ContentManifest
{
bool IsSha256Hex(std::string_view value)
{
    // Lowercase only, and enforced rather than merely accepted. Consumers compare hashes with
    // ==, so tolerating uppercase here would produce a value that matches nothing and reports
    // itself as a corrupt bundle - a failure that looks like data loss and is a typo.
    if (value.size() != 64)
        return false;
    return std::ranges::all_of(value, [](unsigned char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool SplitBundleName(std::string_view name, BundleName& parts)
{
    constexpr std::string_view prefix = "xtra_dead_air_x64_content_";
    constexpr std::string_view suffix = ".xdb0";
    if (!name.starts_with(prefix) || !name.ends_with(suffix))
        return false;

    std::string_view body = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());

    // Trailing "_<16 hex>" is the file hash prefix.
    if (body.size() < 17 || body[body.size() - 17] != '_')
        return false;
    const std::string_view hash16 = body.substr(body.size() - 16);
    if (!std::ranges::all_of(hash16, [](unsigned char c)
        { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return false;
    body.remove_suffix(17);

    // Before it, "_<two digits>" is the shard.
    if (body.size() < 3 || body[body.size() - 3] != '_')
        return false;
    const std::string_view shard = body.substr(body.size() - 2);
    if (!std::ranges::all_of(shard, [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return false;
    body.remove_suffix(3);

    // What remains is the group: lowercase letters, digits and underscores, never empty.
    if (body.empty() || !std::ranges::all_of(body, [](unsigned char c)
        { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
        return false;

    parts.group = body;
    parts.shard = shard;
    parts.hash16 = hash16;
    return true;
}

const Bundle* Manifest::FindByName(std::string_view name) const
{
    const auto it = std::ranges::find_if(bundles, [&](const Bundle& b) { return b.name == name; });
    return it == bundles.end() ? nullptr : &*it;
}

const Bundle* Manifest::FindByHash(std::string_view hash) const
{
    const auto it = std::ranges::find_if(bundles, [&](const Bundle& b) { return b.hash == hash; });
    return it == bundles.end() ? nullptr : &*it;
}

const Bundle* Manifest::FindBySlot(std::string_view group, std::string_view shard) const
{
    const auto it = std::ranges::find_if(bundles, [&](const Bundle& b)
    {
        BundleName parts;
        return SplitBundleName(b.name, parts) && parts.group == group && parts.shard == shard;
    });
    return it == bundles.end() ? nullptr : &*it;
}

bool Manifest::ContentIdMatches() const
{
    std::vector<const Bundle*> sorted;
    sorted.reserve(bundles.size());
    for (const Bundle& bundle : bundles)
        sorted.push_back(&bundle);
    // Ordinal, because the publisher sorts ordinally too. A culture-aware sort disagrees with
    // it on underscores and digits, and the two only have to differ once for every installed
    // player to be told their manifest was tampered with.
    std::ranges::sort(sorted, [](const Bundle* a, const Bundle* b) { return a->name < b->name; });

    std::string blob;
    for (const Bundle* bundle : sorted)
    {
        blob += bundle->name;
        blob += '\n';
        blob += bundle->hash;
        blob += '\n';
    }
    return ContentHash::Buffer(blob.data(), blob.size()) == contentId;
}

bool Parse(std::string_view text, Manifest& manifest, std::string& error)
{
    error.clear();
    if (text.size() > MaximumManifestBytes)
    {
        error = "manifest is implausibly large";
        return false;
    }

    Manifest parsed;
    std::string_view line;

    if (!next_line(text, line) || line != std::string("schema=") + SchemaContent)
    {
        error = "first line must be schema=" + std::string(SchemaContent);
        return false;
    }
    if (!next_line(text, line) || !take_prefixed(line, "version=", parsed.version) ||
        !valid_version(parsed.version))
    {
        error = "second line must be a valid version=MAJOR.MINOR.PATCH";
        return false;
    }
    if (!next_line(text, line) || !take_prefixed(line, "content-id=", parsed.contentId) ||
        !IsSha256Hex(parsed.contentId))
    {
        error = "third line must be content-id=<64 lowercase hex>";
        return false;
    }
    if (!next_line(text, line) || !take_prefixed(line, "repo=", parsed.repo))
    {
        error = "fourth line must be repo=<owner/name>";
        return false;
    }
    if (!next_line(text, line) || line != "[bundles]")
    {
        error = "fifth line must be [bundles]";
        return false;
    }

    std::vector<std::string_view> fields;
    bool inDeltas = false;
    while (next_line(text, line))
    {
        if (line.empty())
            continue;
        if (line == "[deltas]")
        {
            if (inDeltas)
            {
                error = "[deltas] appears twice";
                return false;
            }
            inDeltas = true;
            continue;
        }

        if (!inDeltas)
        {
            if (!split_tabs(line, fields, 4))
            {
                error = "a [bundles] row needs exactly 4 tab-separated fields";
                return false;
            }
            Bundle bundle;
            bundle.hash.assign(fields[0]);
            bundle.name.assign(fields[2]);
            bundle.releaseTag.assign(fields[3]);
            if (!IsSha256Hex(bundle.hash) || !parse_u64(fields[1], bundle.size) ||
                !IsBundleName(bundle.name) || bundle.releaseTag.empty() || bundle.size == 0)
            {
                error = "invalid [bundles] row: " + std::string(line);
                return false;
            }
            if (parsed.FindByName(bundle.name))
            {
                error = "duplicate bundle: " + bundle.name;
                return false;
            }
            if (parsed.bundles.size() >= MaximumBundles)
            {
                error = "too many bundles";
                return false;
            }
            parsed.bundles.push_back(std::move(bundle));
            continue;
        }

        // The rows of the published deltas chunk maps replaced. The schema is frozen, so the
        // section is still read; nothing acts on what it lists.
    }

    if (parsed.bundles.empty())
    {
        error = "manifest declares no bundles";
        return false;
    }

    manifest = std::move(parsed);
    return true;
}

bool ParseFile(const std::wstring& path, Manifest& manifest, std::string& error)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
    {
        error = "content manifest is missing";
        return false;
    }
    const std::streamsize size = input.tellg();
    if (size < 0 || static_cast<size_t>(size) > MaximumManifestBytes)
    {
        error = "content manifest is unreadable or implausibly large";
        return false;
    }
    std::string text(static_cast<size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(text.data(), size) && size != 0)
    {
        error = "content manifest could not be read";
        return false;
    }
    return Parse(text, manifest, error);
}
}
