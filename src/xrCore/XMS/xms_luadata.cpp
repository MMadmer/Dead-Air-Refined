// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "xms_luadata.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace XMS
{
namespace
{
class DataReader
{
    static constexpr int kMaxDepth = 32;

    const char* p;
    const char* end;
    int line{1};
    LuaString problem;

    bool fail(const char* what)
    {
        if (problem.empty())
        {
            char where[32];
            std::snprintf(where, sizeof where, "line %d: ", line);
            problem = LuaString(where) + what;
        }
        return false;
    }

    static bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
    static bool digit(char c) { return c >= '0' && c <= '9'; }

    // [[ or [=[ and so on at p: moves past it and says how many '='
    bool long_open(size_t& level)
    {
        if (p >= end || *p != '[')
            return false;
        const char* q = p + 1;
        while (q < end && *q == '=')
            ++q;
        if (q >= end || *q != '[')
            return false;
        level = size_t(q - p - 1);
        p = q + 1;
        return true;
    }

    bool long_body(size_t level, LuaString& out)
    {
        if (p < end && *p == '\r')
            ++p;
        if (p < end && *p == '\n')
        {
            ++line;
            ++p;
        }
        for (const char* start = p; p < end; ++p)
        {
            if (*p == '\n')
                ++line;
            if (*p != ']' || size_t(end - p) < level + 2)
                continue;
            size_t i = 1;
            while (i <= level && p[i] == '=')
                ++i;
            if (i == level + 1 && p[i] == ']')
            {
                out.assign(start, p);
                p += level + 2;
                return true;
            }
        }
        return fail("a long string or comment is not closed");
    }

    bool skip()
    {
        for (;;)
        {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            {
                if (*p == '\n')
                    ++line;
                ++p;
            }
            if (end - p < 2 || p[0] != '-' || p[1] != '-')
                return true;
            p += 2;
            size_t level = 0;
            if (long_open(level))
            {
                LuaString ignored;
                if (!long_body(level, ignored))
                    return false;
                continue;
            }
            while (p < end && *p != '\n')
                ++p;
        }
    }

    bool read_quoted(LuaString& out)
    {
        const char quote = *p++;
        while (p < end && *p != quote)
        {
            char c = *p++;
            if (c == '\n')
                return fail("a string is not closed on its line");
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (p >= end)
                break;
            c = *p++;
            switch (c)
            {
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'v': out += '\v'; break;
            case '\n':
                ++line;
                out += '\n';
                break;
            case 'z':
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                {
                    if (*p == '\n')
                        ++line;
                    ++p;
                }
                break;
            case 'x':
            {
                int value = 0, count = 0;
                for (; count < 2 && p < end && std::isxdigit((unsigned char)*p); ++count, ++p)
                    value = value * 16 + (digit(*p) ? *p - '0' : (std::tolower((unsigned char)*p) - 'a' + 10));
                if (count != 2)
                    return fail("a \\x escape takes two hex digits");
                out += char(value);
                break;
            }
            default:
                if (digit(c))
                {
                    int value = c - '0';
                    for (int count = 1; count < 3 && p < end && digit(*p); ++count, ++p)
                        value = value * 10 + (*p - '0');
                    if (value > 255)
                        return fail("a decimal escape is above 255");
                    out += char(value);
                }
                else
                    out += c;
            }
        }
        if (p >= end)
            return fail("a string is not closed");
        ++p;
        return true;
    }

    bool read_number(LuaValue& out)
    {
        const bool negative = *p == '-';
        if (negative)
        {
            ++p;
            if (!skip())
                return false;
        }
        if (p >= end || !(digit(*p) || (*p == '.' && p + 1 < end && digit(p[1]))))
            return fail("only plain values can be written here: no expressions or calls");
        // the text ends with a zero, so strtod cannot run off it
        char* stop = nullptr;
        const double value = std::strtod(p, &stop);
        if (stop == p)
            return fail("this is not a number");
        p = stop;
        out.type = LuaValue::Number;
        out.n = negative ? -value : value;
        return true;
    }

    bool read_table(LuaValue& out, int depth)
    {
        if (depth > kMaxDepth)
            return fail("tables are nested too deep");
        ++p;
        out.type = LuaValue::Table;
        for (;;)
        {
            if (!skip())
                return false;
            if (p >= end)
                return fail("a table is not closed");
            if (*p == '}')
            {
                ++p;
                return true;
            }

            LuaString key;
            bool named = false;
            size_t level = 0;
            const char* probe = p;
            if (*p == '[' && !long_open(level))
            {
                ++p;
                LuaValue index;
                if (!read_value(index, depth + 1) || !skip())
                    return false;
                if (p >= end || *p != ']')
                    return fail("']' is missing after a key");
                ++p;
                if (!skip())
                    return false;
                if (p >= end || *p != '=')
                    return fail("'=' is missing after a key");
                ++p;
                if (index.type == LuaValue::String)
                {
                    key = index.s;
                    named = true;
                }
                else if (index.type == LuaValue::Number)
                {
                    if (index.n != double(out.arr.size() + 1))
                    {
                        char text[64];
                        std::snprintf(text, sizeof text, "%g", index.n);
                        key = text;
                        named = true;
                    }
                }
                else
                    return fail("a key is a string or a number");
            }
            else if (*probe == '[')
                p = probe; // a long string as a value: long_open moved past its bracket
            else if (ident_start(*p))
            {
                const int line_was = line;
                const char* word = p;
                while (p < end && ident_char(*p))
                    ++p;
                const char* word_end = p;
                if (!skip())
                    return false;
                if (p < end && *p == '=' && !(p + 1 < end && p[1] == '='))
                {
                    ++p;
                    key.assign(word, word_end);
                    named = true;
                }
                else
                {
                    p = word;
                    line = line_was;
                }
            }

            LuaValue value;
            if (!read_value(value, depth + 1))
                return false;
            if (!named)
                out.arr.emplace_back(static_cast<LuaValue&&>(value));
            else if (value.type != LuaValue::Nil)
                out.set(key, static_cast<LuaValue&&>(value));

            if (!skip())
                return false;
            if (p < end && (*p == ',' || *p == ';'))
                ++p;
            else if (p >= end || *p != '}')
                return fail("only plain values can be written here: no expressions or calls");
        }
    }

    bool read_value(LuaValue& out, int depth)
    {
        if (!skip())
            return false;
        if (p >= end)
            return fail("a value is missing");
        const char c = *p;
        if (c == '{')
            return read_table(out, depth);
        if (c == '"' || c == '\'')
        {
            out.type = LuaValue::String;
            return read_quoted(out.s);
        }
        if (c == '[')
        {
            size_t level = 0;
            if (!long_open(level))
                return fail("only plain values can be written here: no expressions or calls");
            out.type = LuaValue::String;
            return long_body(level, out.s);
        }
        if (digit(c) || c == '.' || c == '-')
            return read_number(out);
        if (ident_start(c))
        {
            const char* word = p;
            while (p < end && ident_char(*p))
                ++p;
            const LuaString name(word, p);
            if (name == "true" || name == "false")
            {
                out.type = LuaValue::Bool;
                out.b = name == "true";
                return true;
            }
            if (name == "nil")
                return true;
        }
        return fail("only plain values can be written here: no expressions or calls");
    }

public:
    DataReader(const char* text, size_t size) : p(text), end(text + size) {}

    const LuaString& error() const { return problem; }

    bool read(LuaValue& out)
    {
        if (end - p >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3;
        if (!skip())
            return false;
        if (end - p >= 6 && 0 == std::strncmp(p, "return", 6) && !(end - p > 6 && ident_char(p[6])))
            p += 6;
        if (!read_value(out, 0) || !skip())
            return false;
        if (p < end && *p == ';')
        {
            ++p;
            if (!skip())
                return false;
        }
        return p >= end || fail("only one value can be returned");
    }
};
} // namespace

bool ReadLuaData(const char* text, size_t size, LuaValue& out, LuaString& error)
{
    out = LuaValue();
    DataReader reader(text, size);
    const bool ok = reader.read(out);
    error = reader.error();
    return ok;
}

bool UpgradeLuaData(LuaValue& asset, const char* key, int current, const LuaDataStep* steps, bool required,
    LuaString& why)
{
    char text[160];
    const LuaValue* header = asset.type == LuaValue::Table ? asset.get(key) : nullptr;
    if (!header && !required && asset.type == LuaValue::Table)
        return true;
    if (!header || header->type != LuaValue::Number || header->n < 1.0 || header->n != double(int(header->n)))
    {
        std::snprintf(text, sizeof text, "the header '%s = %d' is missing", key, current);
        why = text;
        return false;
    }
    int version = int(header->n);
    if (version > current)
    {
        std::snprintf(text, sizeof text, "made for a newer game (version %d, this game reads up to %d)", version, current);
        why = text;
        return false;
    }
    for (; version < current; ++version)
        if (steps && steps[version - 1])
            steps[version - 1](asset);
    LuaValue now;
    now.type = LuaValue::Number;
    now.n = double(current);
    asset.set(key, static_cast<LuaValue&&>(now));
    return true;
}
} // namespace XMS
