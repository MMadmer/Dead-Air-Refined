// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The network rule itself (docs/dead-air/MOD_CATALOG.md 10.4), with nothing of the engine in it: the
// game asks it of a module's files (xms_netmark.cpp) and the rule's own cases ask it standing alone
// (tools/qa/ltx). The mod catalog and whatever builds modules hold the same rule: change all or none.

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace XMS::NetRule
{
inline bool word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

// Lua text without its comments, in lower case. Strings stay - code is passed around as text -
// and so does every line break, so a place in the result is a line of the file.
inline std::string code_of(std::string_view text)
{
    const size_t n = text.size();
    std::string out;
    out.reserve(n);
    // the level of the long bracket that opens at `at` ([[ is 0, [=[ is 1), or -1
    const auto long_open = [&](size_t at) {
        size_t j = at + 1;
        int level = 0;
        while (j < n && text[j] == '=')
        {
            ++j;
            ++level;
        }
        return j < n && text[j] == '[' ? level : -1;
    };
    // the place after the bracket of `level` that closes, or the end
    const auto long_close = [&](size_t from, int level) {
        for (size_t j = from; j < n; ++j)
        {
            if (text[j] != ']')
                continue;
            size_t k = j + 1;
            int l = 0;
            while (k < n && text[k] == '=')
            {
                ++k;
                ++l;
            }
            if (l == level && k < n && text[k] == ']')
                return k + 1;
        }
        return n;
    };
    const auto keep = [&](size_t from, size_t to) {
        for (size_t k = from; k < to; ++k)
            out += lower(text[k]);
    };
    size_t i = 0;
    while (i < n)
    {
        const char c = text[i];
        if (c == '-' && i + 1 < n && text[i + 1] == '-')
        {
            const size_t j = i + 2;
            const int level = j < n && text[j] == '[' ? long_open(j) : -1;
            size_t end = n;
            if (level >= 0)
                end = long_close(j + 2 + level, level);
            else if (const size_t line = text.find('\n', j); line != std::string_view::npos)
                end = line;
            for (size_t k = i; k < end; ++k)
                if (text[k] == '\n')
                    out += '\n';
            out += ' ';
            i = end;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            size_t j = i + 1;
            while (j < n && text[j] != c && text[j] != '\n')
                j += text[j] == '\\' && j + 1 < n ? 2 : 1;
            if (j < n && text[j] == c)
                ++j;
            j = std::min(j, n);
            keep(i, j);
            i = j;
            continue;
        }
        if (c == '[')
        {
            if (const int level = long_open(i); level >= 0)
            {
                const size_t end = long_close(i + 2 + level, level);
                keep(i, end);
                i = end;
                continue;
            }
        }
        out += lower(c);
        ++i;
    }
    return out;
}

// What reaches the network from a script, by its name: the libraries of the system and of Lua,
// and the programs a command line fetches with. Whole words, any case.
inline constexpr std::string_view kWords[] = {"winhttp", "wininet", "urlmon", "ws2_32", "wsock32", "luasocket",
    "socket.core", "socket.http", "curl", "wget", "bitsadmin", "invoke-webrequest", "invoke-restmethod", "webclient",
    "urldownloadtofile"};

// The first place the code works with the network, or npos.
inline size_t first_use(const std::string& code)
{
    size_t first = std::string::npos;
    // the game's own API: xms.http, xms . http, xms["http"]
    for (size_t at = code.find("xms"); at != std::string::npos; at = code.find("xms", at + 1))
    {
        if (at && word_char(code[at - 1]))
            continue;
        size_t j = at + 3;
        while (j < code.size() && code[j] && strchr(" \t.[]\"'", code[j]))
            ++j;
        if (j == at + 3 || code.compare(j, 4, "http") != 0)
            continue;
        if (j + 4 < code.size() && word_char(code[j + 4]))
            continue;
        first = at;
        break;
    }
    for (const std::string_view word : kWords)
        for (size_t at = code.find(word); at != std::string::npos && at < first; at = code.find(word, at + 1))
        {
            const size_t end = at + word.size();
            if ((at && word_char(code[at - 1])) || (end < code.size() && word_char(code[end])))
                continue;
            first = at;
            break;
        }
    return first;
}

// Whether Lua text works with the network; `line` gets the first line that does, from 1.
inline bool UsesNetwork(std::string_view text, unsigned* line = nullptr)
{
    const std::string code = code_of(text);
    const size_t at = first_use(code);
    if (at == std::string::npos)
        return false;
    if (line)
        *line = 1 + unsigned(std::count(code.begin(), code.begin() + at, '\n'));
    return true;
}
} // namespace XMS::NetRule
