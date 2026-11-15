// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "xms_ltxp.h"

#include <cstring>

namespace XMS
{
namespace ltxp
{
bool blank(char c) { return c == ' ' || c == '\t'; }

// The trim the patch reader always had: spaces and tabs in front, and a CR at the end too.
LuaString trim(const char* begin, const char* end)
{
    while (begin < end && blank(*begin))
        ++begin;
    while (end > begin && (blank(end[-1]) || end[-1] == '\r'))
        --end;
    return LuaString(begin, size_t(end - begin));
}

LuaString lower(LuaString text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return text;
}

// `#file <path>`: true with the config's path, empty for `#file` alone; `nowhere` for a path that
// names no config under configs (it climbs out, or has a drive in it)
bool file_line(const LuaString& text, LuaString& path, bool& nowhere)
{
    if (text.size() < 5 || text[0] != '#')
        return false;
    for (size_t i = 0; i < 4; ++i)
        if ((text[i + 1] | 0x20) != "file"[i])
            return false;
    if (text.size() > 5 && !blank(text[5]))
        return false;
    const LuaString written = trim(text.data() + 5, text.data() + text.size());
    path = ConfigPath(written);
    nowhere = !written.empty() && (path.empty() || path.find(':') != LuaString::npos);
    return true;
}
} // namespace ltxp

LuaString ConfigPath(const LuaString& path)
{
    LuaVector<LuaString> parts;
    LuaString part;
    const auto flush = [&]() -> bool
    {
        if (part == "..")
        {
            if (parts.empty())
                return false;
            parts.pop_back();
        }
        else if (!part.empty() && part != ".")
            parts.push_back(part);
        part.clear();
        return true;
    };
    for (char c : ltxp::lower(path))
    {
        if (c == '/' || c == '\\')
        {
            if (!flush())
                return LuaString();
        }
        else
            part += c;
    }
    if (!flush())
        return LuaString();
    LuaString out;
    for (const LuaString& p : parts)
    {
        if (!out.empty())
            out += '\\';
        out += p;
    }
    return out;
}

void ParseLtxp(const char* text, size_t size, LuaVector<LtxDirective>& out)
{
    unsigned block = 0;
    for (const LtxDirective& d : out)
        block = d.block > block ? d.block : block;

    LuaString file, section;
    unsigned current = 0;
    // after a #file that names no config nothing is read until the next #file: it must not land
    // on the system config instead
    bool nowhere = false;
    const auto emit = [&](LtxDirective::Op op, const LuaString& key, const LuaString& value)
    {
        if (nowhere)
            return;
        LtxDirective d;
        d.op = op;
        d.block = current;
        d.file = file;
        d.section = section;
        d.key = key;
        d.value = value;
        out.push_back(d);
    };

    const char* end = text + size;
    if (const void* zero = std::memchr(text, 0, size))
        end = static_cast<const char*>(zero);
    for (const char* line = text; line < end;)
    {
        const char* next = static_cast<const char*>(std::memchr(line, '\n', size_t(end - line)));
        const char* stop = next ? next : end;
        // ';' cuts the line wherever it stands, inside quotes too, as it always did
        if (const void* comment = std::memchr(line, ';', size_t(stop - line)))
            stop = static_cast<const char*>(comment);
        const LuaString t = ltxp::trim(line, stop);
        line = next ? next + 1 : end;
        if (t.empty())
            continue;

        LuaString path;
        if (ltxp::file_line(t, path, nowhere))
        {
            file = path;
            section.clear();
            current = 0;
            continue;
        }

        if (t[0] == '[')
        {
            if (nowhere)
                continue;
            const size_t close = t.find(']');
            if (close == LuaString::npos)
                continue;
            LuaString name = ltxp::trim(t.data() + 1, t.data() + close);
            const LuaString rest = ltxp::trim(t.data() + close + 1, t.data() + t.size());
            LtxDirective d;
            d.file = file;
            d.block = current = ++block;
            if (!name.empty() && name[0] == '!')
            {
                d.op = LtxDirective::RemoveSection;
                name = ltxp::trim(name.data() + 1, name.data() + name.size());
            }
            else
            {
                d.op = LtxDirective::Section;
                if (!rest.empty() && rest[0] == ':')
                    d.parents = ltxp::trim(rest.data() + 1, rest.data() + rest.size());
            }
            d.section = section = ltxp::lower(name);
            out.push_back(d);
            continue;
        }

        const size_t eq = t.find('=');
        if (eq != LuaString::npos)
        {
            const LuaString key = ltxp::trim(t.data(), t.data() + eq);
            const LuaString value = ltxp::trim(t.data() + eq + 1, t.data() + t.size());
            if (!key.empty() && key[0] == '+')
                emit(LtxDirective::Add, ltxp::trim(key.data() + 1, key.data() + key.size()), value);
            else if (!key.empty() && key[0] == '-')
                emit(LtxDirective::Drop, ltxp::trim(key.data() + 1, key.data() + key.size()), value);
            else if (!key.empty())
                emit(LtxDirective::Set, key, value);
        }
        else if (t[0] == '!')
            emit(LtxDirective::RemoveKey, ltxp::trim(t.data() + 1, t.data() + t.size()), LuaString());
    }
}
} // namespace XMS
