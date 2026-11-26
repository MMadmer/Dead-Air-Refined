// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// Names of the whole game two owners take (docs/dead-air/XMS_ARCHITECTURE.md 13.1): the rules that decide who
// keeps one, with nothing of the engine in them. The game asks them (the string table, the modes, the game
// graph, the spawn composer) and their own cases ask them standing alone (tools/qa/ltx).

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace XMS::Names
{
constexpr size_t None = size_t(-1);

// A fold takes files in order and a later definition of an id stands over an earlier one (the string table).
// `owners` maps an id to the file that defined it last and that file's owner; the game's files (owner 0) are not
// recorded, since a module over the game is what the fold is for. Answers the file whose definition this one
// stands over when that file is another module's, None otherwise.
template <class Map, class Id>
size_t Redefines(Map& owners, const Id& id, size_t file, unsigned owner)
{
    if (!owner)
        return None;
    const auto [it, fresh] = owners.try_emplace(id, file, owner);
    if (fresh)
        return None;
    const auto [was_file, was_owner] = it->second;
    it->second = {file, owner};
    return was_owner != owner ? was_file : None;
}

// The ids several owners declare, in load order (a mode a module brings): the first owner's first declaration of an
// id is the one that counts. `ids[i]` are owner i's ids as it declares them; the answer, per owner and declaration,
// is the owner whose declaration counts - `i` itself both for the one that counts and for a repeat of it, which
// `counts` tells apart.
struct Declaration
{
    size_t keeper{None};
    bool counts{false};
};

template <class Lists>
std::vector<std::vector<Declaration>> FirstDeclarations(const Lists& ids)
{
    std::vector<std::vector<Declaration>> out(ids.size());
    for (size_t owner = 0; owner < ids.size(); ++owner)
    {
        out[owner].resize(ids[owner].size());
        for (size_t n = 0; n < ids[owner].size(); ++n)
        {
            Declaration& d = out[owner][n];
            for (size_t earlier = 0; earlier <= owner && d.keeper == None; ++earlier)
            {
                const size_t until = earlier == owner ? n : ids[earlier].size();
                for (size_t k = 0; k < until; ++k)
                    if (ids[earlier][k] == ids[owner][n])
                    {
                        d.keeper = earlier;
                        break;
                    }
            }
            d.counts = d.keeper == None;
            if (d.counts)
                d.keeper = owner;
        }
    }
    return out;
}

inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

inline bool SameNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i]))
            return false;
    return true;
}

// Of the entries that bring one name, the one whose path `served(name)` gives goes first (a level whose files the
// game opens); the others keep their order, and so does every name nobody else brings.
template <class Entries, class NameOf, class PathOf, class Served>
void ServedFirst(Entries& entries, NameOf name_of, PathOf path_of, Served served)
{
    for (size_t i = 1; i < entries.size(); ++i)
    {
        size_t first = 0;
        while (first < i && !(name_of(entries[first]) == name_of(entries[i])))
            ++first;
        if (first == i)
            continue;
        const std::string path = served(name_of(entries[i]));
        if (path.empty() || !SameNoCase(path, path_of(entries[i])))
            continue;
        auto moved = std::move(entries[i]);
        for (size_t k = i; k > first; --k)
            entries[k] = std::move(entries[k - 1]);
        entries[first] = std::move(moved);
    }
}

inline std::string_view Trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// The section a line of an ini text opens ("[name]" or "[name]:parents"), or nothing for any other line.
inline bool SectionLine(std::string_view line, std::string_view& name)
{
    line = Trimmed(line.substr(0, line.find(';')));
    if (line.empty() || line.front() != '[')
        return false;
    const size_t close = line.find(']');
    name = Trimmed(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
    return true;
}

// The story name the scripts register an object under, from its custom data
// (story_objects.check_spawn_ini_for_story_id): the first line of [story_object], "story_id = <name>" or a bare
// "<name>". `has_section` says the custom data has the section, whatever it holds - the object's config section
// is then not asked for a story_id.
inline std::string StoryNameIn(std::string_view custom, bool& has_section)
{
    has_section = false;
    bool inside = false;
    while (!custom.empty())
    {
        const size_t eol = custom.find('\n');
        const std::string_view line = custom.substr(0, eol);
        custom = eol == std::string_view::npos ? std::string_view() : custom.substr(eol + 1);
        std::string_view section;
        if (SectionLine(line, section))
        {
            if (inside)
                return std::string();
            inside = SameNoCase(section, "story_object");
            has_section = has_section || inside;
            continue;
        }
        const std::string_view text = Trimmed(line.substr(0, line.find(';')));
        if (!inside || text.empty())
            continue;
        const size_t eq = text.find('=');
        const std::string_view key = Trimmed(text.substr(0, eq));
        const std::string_view value = eq == std::string_view::npos ? std::string_view() : Trimmed(text.substr(eq + 1));
        return std::string(key == "story_id" && !value.empty() ? value : key);
    }
    return std::string();
}

// The custom data without its [story_object] section: every other line stays as it was.
inline std::string WithoutStorySection(std::string_view custom)
{
    std::string out;
    bool inside = false;
    while (!custom.empty())
    {
        const size_t eol = custom.find('\n');
        const std::string_view line = custom.substr(0, eol == std::string_view::npos ? custom.size() : eol + 1);
        custom = eol == std::string_view::npos ? std::string_view() : custom.substr(eol + 1);
        std::string_view section;
        if (SectionLine(line, section))
            inside = SameNoCase(section, "story_object");
        if (!inside)
            out.append(line);
    }
    return out;
}
} // namespace XMS::Names
