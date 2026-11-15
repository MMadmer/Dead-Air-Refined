// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xms_characters.h"
#include "xms_core.h"
#include "xms_luadata.h"
#include "xrCore/xr_ini.h"

#include <cstdarg>

namespace XMS
{
namespace
{
// ---- the asset --------------------------------------------------------------
// A character is plain data written in Lua's syntax, read by the module data reader.

using Value = LuaValue;

// No step yet: version 1 is the only one, and a character without the header is version 1.
constexpr int kCharacterVersion = 1;
const LuaDataStep* const kCharacterSteps = nullptr;

bool read_file(pcstr path, xr_string& out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(std::max(0l, size)));
    const size_t got = size > 0 ? fread(out.data(), 1, size_t(size), f) : 0;
    fclose(f);
    out.resize(got);
    return true;
}

// ---- text --------------------------------------------------------------------

xr_string format(pcstr pattern, ...)
{
    string1024 buffer;
    va_list args;
    va_start(args, pattern);
    vsnprintf(buffer, sizeof buffer, pattern, args);
    va_end(args);
    buffer[sizeof buffer - 1] = 0;
    return buffer;
}

// The asset is UTF-8 when a tool wrote it and may be Windows-1251 when a person did; everything
// the game reads is cp1251.
xr_string game_text(const xr_string& text)
{
    const int length = int(text.size());
    const int wide_length = length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0) : 0;
    if (wide_length <= 0)
        return text;
    xr_vector<wchar_t> utf16(static_cast<size_t>(wide_length));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, utf16.data(), wide_length);
    const int ansi_length = WideCharToMultiByte(1251, 0, utf16.data(), wide_length, nullptr, 0, "?", nullptr);
    if (ansi_length <= 0)
        return text;
    xr_string ansi(size_t(ansi_length), '\0');
    WideCharToMultiByte(1251, 0, utf16.data(), wide_length, ansi.data(), ansi_length, "?", nullptr);
    return ansi;
}

xr_string xml(const xr_string& text)
{
    xr_string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return game_text(out);
}

xr_string trimmed(const xr_string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == xr_string::npos)
        return xr_string();
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// A name goes into ltx and xml lines as it is: nothing in it may start another one.
bool plain_name(const xr_string& text)
{
    for (const char c : text)
        if (u8(c) < 0x20 || strchr("[]=;<>&\"", c))
            return false;
    return true;
}

bool valid_id(const xr_string& id)
{
    if (id.empty() || id.size() > 48 || (id[0] >= '0' && id[0] <= '9'))
        return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

xr_string text_in(const Value& v, pcstr lang)
{
    if (v.type == Value::String)
        return v.s;
    if (v.type != Value::Table)
        return xr_string();
    for (pcstr wanted : {lang, "rus"})
        if (const Value* x = v.get(wanted))
            if (x->type == Value::String)
                return x->s;
    for (const Value& x : v.vals)
        if (x.type == Value::String)
            return x.s;
    return xr_string();
}

void languages_of(const Value& v, xr_vector<xr_string>& out)
{
    if (v.type != Value::Table)
        return;
    for (size_t i = 0; i < v.keys.size(); ++i)
    {
        bool usable = v.vals[i].type == Value::String && !v.keys[i].empty() && v.keys[i].size() <= 16;
        for (const char c : v.keys[i])
            usable = usable && c >= 'a' && c <= 'z';
        if (usable && std::find(out.begin(), out.end(), v.keys[i]) == out.end())
            out.push_back(v.keys[i]);
    }
}

bool text_empty(const Value& v)
{
    if (v.type == Value::String)
        return trimmed(v.s).empty();
    if (v.type != Value::Table)
        return true;
    for (const Value& x : v.vals)
        if (x.type == Value::String && !trimmed(x.s).empty())
            return false;
    return true;
}

// ---- the character -----------------------------------------------------------

struct Supply
{
    xr_string section;
    int count{1}; // 0 takes an inherited row away
};

struct StockRow
{
    xr_string section;
    int count{1};
    float chance{1.f};
};

struct TradeDecl
{
    bool declared{false};
    bool off{false}; // trade = false: not a trader, whatever the base is
    xr_vector<xr_string> sells, buys;
    xr_vector<StockRow> stock;
    float markup{1.f};
    float pays{0.3f};
    int restock{24};
    bool trader() const { return declared && !off; }
};

// Empty means unset, not the default: a default written here would override what the base says.
struct Character
{
    xr_string file; // inside the module, for the log
    xr_string id;
    xr_string base;
    bool abstract_{false};
    Value name, bio;
    xr_string community, visual, icon, start_dialog, spec_rank;
    int rank{0}, reputation{0};
    bool has_rank{false}, has_reputation{false};
    bool has_money{false}, money_infinite{false};
    int money_min{0}, money_max{0};
    xr_vector<Supply> supplies;
    TradeDecl trade;
    bool squad{true}, has_squad{false};
};

void names_of(const Value* list, xr_vector<xr_string>& out)
{
    if (!list)
        return;
    if (list->type == Value::String)
    {
        if (!list->s.empty())
            out.push_back(list->s);
        return;
    }
    for (const Value& v : list->arr)
        if (v.type == Value::String && !v.s.empty())
            out.push_back(v.s);
}

void parse_character(const Value& v, Character& c)
{
    c.id = v.text("id");
    c.base = v.text("base");
    c.abstract_ = v.flag("abstract", false);
    if (const Value* n = v.get("name"))
        c.name = *n;
    if (const Value* b = v.get("bio"))
        c.bio = *b;
    c.community = v.text("community");
    c.visual = v.text("visual");
    c.icon = v.text("icon");
    c.start_dialog = v.text("start_dialog");
    c.spec_rank = v.text("spec_rank");
    if (const Value* s = v.get("squad"); s && s->type == Value::Bool)
    {
        c.squad = s->b;
        c.has_squad = true;
    }
    if (const Value* r = v.get("rank"); r && r->type == Value::Number)
    {
        c.rank = int(r->n);
        c.has_rank = true;
    }
    if (const Value* r = v.get("reputation"); r && r->type == Value::Number)
    {
        c.reputation = int(r->n);
        c.has_reputation = true;
    }
    if (const Value* m = v.get("money"); m && m->type == Value::Table)
    {
        c.has_money = true;
        c.money_min = int(m->number("min", 0.0));
        c.money_max = _max(int(m->number("max", double(c.money_min))), c.money_min);
        c.money_infinite = m->flag("infinite", false);
    }
    if (const Value* list = v.get("supplies"))
        for (const Value& row : list->arr)
        {
            Supply item;
            if (row.type == Value::String)
                item.section = row.s;
            else if (row.type == Value::Table)
            {
                item.section = row.text("section");
                item.count = _max(int(row.number("count", 1.0)), 0);
            }
            if (!item.section.empty())
                c.supplies.push_back(item);
        }
    if (const Value* t = v.get("trade"))
    {
        if (t->type == Value::Bool && !t->b)
        {
            c.trade.declared = true;
            c.trade.off = true;
        }
        else if (t->type == Value::Table)
        {
            c.trade.declared = true;
            names_of(t->get("sells"), c.trade.sells);
            names_of(t->get("buys"), c.trade.buys);
            c.trade.markup = float(t->number("markup", 1.0));
            c.trade.pays = float(t->number("pays", 0.3));
            c.trade.restock = int(t->number("restock", 24.0));
            if (const Value* stock = t->get("stock"))
                for (const Value& row : stock->arr)
                {
                    StockRow r;
                    if (row.type == Value::String)
                        r.section = row.s;
                    else if (row.type == Value::Table)
                    {
                        r.section = row.text("section");
                        r.count = int(row.number("count", 1.0));
                        r.chance = float(row.number("chance", 1.0));
                    }
                    c.trade.stock.push_back(r);
                }
        }
    }
}

// `src` wins wherever it says something; the kit is merged, a repeated section takes the new count.
void apply_over(Character& dst, const Character& src)
{
    if (src.name.type != Value::Nil)
        dst.name = src.name;
    if (src.bio.type != Value::Nil)
        dst.bio = src.bio;
    for (auto [to, from] : {std::pair{&dst.community, &src.community}, {&dst.visual, &src.visual}, {&dst.icon, &src.icon},
             {&dst.start_dialog, &src.start_dialog}, {&dst.spec_rank, &src.spec_rank}})
        if (!from->empty())
            *to = *from;
    if (src.has_rank)
    {
        dst.rank = src.rank;
        dst.has_rank = true;
    }
    if (src.has_reputation)
    {
        dst.reputation = src.reputation;
        dst.has_reputation = true;
    }
    if (src.has_money)
    {
        dst.has_money = true;
        dst.money_min = src.money_min;
        dst.money_max = src.money_max;
        dst.money_infinite = src.money_infinite;
    }
    if (src.has_squad)
    {
        dst.squad = src.squad;
        dst.has_squad = true;
    }
    // a shop is one thing: a character that says anything about trade says all of it
    if (src.trade.declared)
        dst.trade = src.trade;
    for (const Supply& row : src.supplies)
    {
        const auto same = std::find_if(dst.supplies.begin(), dst.supplies.end(),
            [&row](const Supply& have) { return 0 == _stricmp(have.section.c_str(), row.section.c_str()); });
        if (same != dst.supplies.end())
            same->count = row.count;
        else
            dst.supplies.push_back(row);
    }
}

const Character* by_id(const xr_vector<Character>& all, const xr_string& id)
{
    for (const Character& c : all)
        if (c.id == id)
            return &c;
    return nullptr;
}

void resolve(pcstr module_id, xr_vector<Character>& all)
{
    const xr_vector<Character> raw = all;
    for (Character& c : all)
    {
        if (!c.base.empty())
        {
            xr_vector<const Character*> chain;
            bool broken = false;
            for (xr_string next = c.base; !next.empty();)
            {
                const Character* b = by_id(raw, next);
                if (next == c.id || !b || chain.size() > 32 || std::find(chain.begin(), chain.end(), b) != chain.end())
                {
                    Msg("! XMS: [%s] character '%s': the base '%s' cannot be followed - it keeps only what it says itself",
                        module_id, c.id.c_str(), next.c_str());
                    broken = true;
                    break;
                }
                chain.push_back(b);
                next = b->base;
            }
            if (!broken)
            {
                Character merged;
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    apply_over(merged, **it);
                apply_over(merged, c);
                merged.file = c.file;
                merged.id = c.id;
                merged.base = c.base;
                merged.abstract_ = c.abstract_;
                c = std::move(merged);
            }
        }
        c.supplies.erase(std::remove_if(c.supplies.begin(), c.supplies.end(), [](const Supply& s) { return s.count < 1; }),
            c.supplies.end());
        if (c.community.empty())
            c.community = "stalker";
        if (c.spec_rank.empty())
            c.spec_rank = "novice";
    }
}

// The game stops on a character it cannot make sense of, so one that would be is left out and said.
bool usable(pcstr module_id, const Character& c)
{
    pcstr why = nullptr;
    if (!valid_id(c.id))
        why = "the id is a-z, 0-9 and _, up to 48, not starting with a digit";
    else if (text_empty(c.name))
        why = "it has no name";
    else if (c.visual.empty())
        why = "it has no visual";
    else if (!plain_name(c.community) || !plain_name(c.visual) || !plain_name(c.icon) || !plain_name(c.start_dialog) ||
        !plain_name(c.spec_rank))
        why = "a name in it holds characters no game file can carry";
    for (const Supply& s : c.supplies)
        if (!plain_name(s.section))
            why = "an item of its kit holds characters no game file can carry";
    for (const StockRow& s : c.trade.stock)
        if (s.section.empty() || !plain_name(s.section))
            why = "an item of its stock holds characters no game file can carry";
    if (why)
        Msg("! XMS: [%s] %s: left out - %s", module_id, c.file.c_str(), why);
    return !why;
}

// ---- files the engine's loaders open -------------------------------------------
// The loaders take files by name and a trader's config goes through scripts, so what is built
// is published where they look. It is rewritten only when it changes: the time of a file is part
// of the string table's cache signature.

struct Published
{
    xr_string root;
    xr_set<xr_string> wanted;
};

Published& published()
{
    static Published state;
    if (state.root.empty())
    {
        string_path root;
        FS.update_path(root, "$app_data_root$", "xms_generated" DELIMITER);
        state.root = root;
    }
    return state;
}

xr_string lowered(xr_string text)
{
    xr_strlwr(text.data());
    return text;
}

void publish(const xr_string& relative, const xr_string& text)
{
    Published& state = published();
    const xr_string path = state.root + relative;
    for (size_t slash = path.find('\\', 3); slash != xr_string::npos; slash = path.find('\\', slash + 1))
        CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
    xr_string now;
    if (!read_file(path.c_str(), now) || now != text)
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
        {
            Msg("! XMS: cannot write %s", path.c_str());
            return;
        }
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
    state.wanted.insert(lowered(path));
    if (!MountFile(relative.c_str(), path.c_str()))
        Msg("! XMS: cannot publish %s", relative.c_str());
}

// ---- the trade configs -----------------------------------------------------------

struct PresetItem
{
    xr_string section, value; // value empty: a bare key
};

struct Preset
{
    xr_string name;
    xr_vector<PresetItem> items;
};

// Every section of trade_presets.ltx that lists items is a category; one that only inherits
// ("[medic]:unused,trash,...") is a recipe made of them, not one itself.
const xr_vector<Preset>& presets()
{
    static xr_vector<Preset> list;
    static bool read = false;
    if (read)
        return list;
    read = true;

    IReader* reader = FS.exist("$game_config$", "misc\\trade\\trade_presets.ltx") ?
        FS.r_open("$game_config$", "misc\\trade\\trade_presets.ltx") :
        nullptr;
    if (!reader)
        return list;
    const xr_string text(static_cast<pcstr>(reader->pointer()), reader->length());
    FS.r_close(reader);

    bool collecting = false;
    for (size_t pos = 0; pos < text.size();)
    {
        size_t eol = text.find('\n', pos);
        if (eol == xr_string::npos)
            eol = text.size();
        xr_string line = trimmed(text.substr(pos, eol - pos));
        pos = eol + 1;
        const size_t comment = line.find(';');
        if (comment != xr_string::npos)
            line = trimmed(line.substr(0, comment));
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            collecting = !(close != xr_string::npos && line.find(':', close) != xr_string::npos);
            if (collecting)
            {
                list.emplace_back();
                list.back().name = line.substr(1, close == xr_string::npos ? xr_string::npos : close - 1);
            }
            continue;
        }
        if (!collecting)
            continue;
        PresetItem item;
        const size_t eq = line.find('=');
        item.section = trimmed(eq == xr_string::npos ? line : line.substr(0, eq));
        if (eq != xr_string::npos)
            item.value = trimmed(line.substr(eq + 1));
        if (!item.section.empty())
            list.back().items.push_back(item);
    }
    return list;
}

bool has_name(const xr_vector<xr_string>& list, const xr_string& name)
{
    return std::find(list.begin(), list.end(), name) != list.end();
}

// One trader's config in the game's own format: what it buys, what it sells, the stock, and the
// prices through `discounts`, the scale the game's traders use over the presets' item factors.
xr_string trade_config(const xr_string& gid, const TradeDecl& t, float default_buy_factor)
{
    xr_string s;
    s += "[trader]\r\n";
    s += "buy_condition             = " + gid + "_buy\r\n";
    s += "sell_condition            = " + gid + "_sell\r\n";
    s += "buy_supplies              = " + gid + "_stock\r\n";
    s += "buy_item_condition_factor = 0.5\r\n";
    s += format("resupply_timeout          = %d\r\n", t.restock * 3600);
    s += "discounts                 = " + gid + "_prices\r\n\r\n";

    s += "[" + gid + "_prices]\r\n";
    s += format("buy                       = %g\r\n", t.pays / default_buy_factor);
    s += format("sell                      = %g\r\n\r\n", t.markup);

    // An item can sit in several presets (the barman's price table repeats the mutant parts and
    // the meat), so each is decided once, over every category it is in: on when any of them is.
    struct Item
    {
        bool sold{false}, bought{false};
        xr_string factor;
    };
    xr_map<xr_string, Item> items;
    xr_vector<xr_string> order;
    for (const Preset& preset : presets())
    {
        const bool sold = has_name(t.sells, preset.name), bought = has_name(t.buys, preset.name);
        for (const PresetItem& it : preset.items)
        {
            auto found = items.find(it.section);
            if (found == items.end())
            {
                order.push_back(it.section);
                found = items.emplace(it.section, Item()).first;
            }
            Item& x = found->second;
            x.sold = x.sold || sold;
            if (bought && (!x.bought || x.factor.empty()))
                x.factor = it.value;
            x.bought = x.bought || bought;
        }
    }
    const auto stocked = [&t](const xr_string& section)
    {
        return std::any_of(t.stock.begin(), t.stock.end(),
            [&section](const StockRow& r) { return 0 == _stricmp(r.section.c_str(), section.c_str()); });
    };

    // selling: what no sold category holds is off (a bare key); the stock always goes, at the markup
    s += "[" + gid + "_sell]\r\n";
    for (const xr_string& section : order)
        if (!items[section].sold && !stocked(section))
            s += section + "\r\n";
    for (const StockRow& r : t.stock)
        s += r.section + " = 1, 1\r\n";
    s += "\r\n";

    // buying: what no bought category holds is off; the rest keeps the factor the presets give
    s += "[" + gid + "_buy]\r\n";
    for (const xr_string& section : order)
    {
        const Item& x = items[section];
        if (!x.bought)
            s += section + "\r\n";
        else if (!x.factor.empty())
            s += section + " = " + x.factor + "\r\n";
    }
    s += "\r\n";

    s += "[" + gid + "_stock]\r\n";
    for (const StockRow& r : t.stock)
        s += r.section + format(" = %d, %g\r\n", r.count, r.chance);
    return s;
}
} // namespace

bool BuildCharacters(const Module& m, const CInifile& settings, CharacterConfigs& out)
{
    xr_vector<xr_string> files;
    ListFilesRelative(m.root.c_str(), "*.npcasset", true, files);
    if (files.empty())
        return false;

    xr_vector<Character> all;
    for (const xr_string& file : files)
    {
        xr_string text;
        if (!read_file((m.root + file).c_str(), text))
        {
            Msg("! XMS: [%s] %s: cannot be read", m.id.c_str(), file.c_str());
            continue;
        }
        Value value;
        xr_string error;
        if (!ReadLuaData(text.c_str(), text.size(), value, error) || value.type != Value::Table)
        {
            Msg("! XMS: [%s] %s: left out - %s", m.id.c_str(), file.c_str(),
                error.empty() ? "a character file returns one table" : error.c_str());
            continue;
        }
        if (!UpgradeLuaData(value, "npc", kCharacterVersion, kCharacterSteps, false, error))
        {
            Msg("! %s\\%s: %s", m.id.c_str(), file.c_str(), error.c_str());
            continue;
        }
        Character c;
        parse_character(value, c);
        c.file = file;
        // the file name is the id when the file does not name one, so a renamed file renames the character
        if (c.id.empty())
        {
            const size_t slash = file.find_last_of('\\');
            xr_string name = slash == xr_string::npos ? file : file.substr(slash + 1);
            name.erase(name.find_last_of('.'));
            for (char& ch : name)
            {
                ch = char(tolower(u8(ch)));
                if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
                    ch = '_';
            }
            c.id = name;
        }
        if (by_id(all, c.id))
        {
            Msg("! XMS: [%s] %s: left out - another character has the id '%s'", m.id.c_str(), file.c_str(), c.id.c_str());
            continue;
        }
        all.emplace_back(std::move(c));
    }
    std::sort(all.begin(), all.end(), [](const Character& a, const Character& b) { return a.id < b.id; });

    resolve(m.id.c_str(), all);
    // templates exist for the others to inherit from: never a character in the game
    all.erase(std::remove_if(all.begin(), all.end(),
                  [&m](const Character& c) { return c.abstract_ || !usable(m.id.c_str(), c); }),
        all.end());
    if (all.empty())
        return false;

    const xr_string& mod = m.id;
    const float buy_factor = settings.section_exist("trade") && settings.line_exist("trade", "buy_price_factor_friendly") ?
        _max(settings.r_float("trade", "buy_price_factor_friendly"), 0.0001f) :
        0.05f;

    xr_string desc = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n\r\n<xml>\r\n";
    xr_string prof = desc;
    xr_string& ltx = out.sections;
    for (const Character& c : all)
    {
        const xr_string gid = mod + "_" + c.id;

        desc += "\t<specific_character id=\"" + xml(gid) + "\" no_random=\"1\">\r\n";
        desc += "\t\t<name>" + xml(gid + "_name") + "</name>\r\n";
        if (!text_empty(c.bio))
            desc += "\t\t<bio>" + xml(gid + "_bio") + "</bio>\r\n";
        if (!c.icon.empty())
            desc += "\t\t<icon>" + xml(c.icon) + "</icon>\r\n";
        desc += "\t\t<class>" + xml(gid) + "</class>\r\n";
        // community, rank and reputation are the three the engine asserts on while it loads profiles
        desc += "\t\t<community>" + xml(c.community) + "</community>\r\n";
        desc += "\t\t<visual>" + xml(c.visual) + "</visual>\r\n";
        desc += format("\t\t<rank>%d</rank>\r\n", c.has_rank ? c.rank : 0);
        desc += format("\t\t<reputation>%d</reputation>\r\n", c.has_reputation ? c.reputation : 0);
        if (c.has_money)
            desc += format("\t\t<money min=\"%d\" max=\"%d\" infinitive=\"%d\"/>\r\n", c.money_min, c.money_max, c.money_infinite ? 1 : 0);
        // "[spawn] \n" then one line per entry is the engine's own format; a count only above one
        desc += "\t\t<supplies>\r\n\t\t\t[spawn] \\n\r\n";
        for (const Supply& s : c.supplies)
        {
            desc += "\t\t\t" + xml(s.section);
            if (s.count != 1)
                desc += format(" = %d", s.count);
            desc += " \\n\r\n";
        }
        desc += "\t\t</supplies>\r\n";
        if (!c.start_dialog.empty())
            desc += "\t\t<start_dialog>" + xml(c.start_dialog) + "</start_dialog>\r\n";
        desc += "\t</specific_character>\r\n";

        // The profile names the character outright: `class` alone would let the engine pick a
        // random character of that class, and a quest's NPC has to be the one it was written for.
        prof += "\t<character id=\"" + xml(gid) + "\">\r\n";
        prof += "\t\t<class>" + xml(gid) + "</class>\r\n";
        prof += "\t\t<specific_character>" + xml(gid) + "</specific_character>\r\n";
        prof += "\t</character>\r\n";

        ltx += "[" + gid + "]:stalker\r\n";
        ltx += "$spawn                = \"npc\\" + mod + "\\" + c.id + "\"\r\n";
        ltx += "character_profile     = " + gid + "\r\n";
        ltx += "community             = " + c.community + "\r\n";
        if (!c.spec_rank.empty())
            ltx += "spec_rank             = " + c.spec_rank + "\r\n";
        if (c.trade.trader())
        {
            const xr_string config = "misc\\trade\\" + mod + "\\" + c.id + ".ltx";
            ltx += "xms_trade             = " + config + "\r\n";
            publish("configs\\" + config, trade_config(gid, c.trade, buy_factor));
        }
        ltx += "\r\n";
        if (c.squad)
        {
            ltx += "[" + gid + "_squad]:online_offline_group\r\n";
            ltx += "faction               = " + c.community + "\r\n";
            ltx += "npc                   = " + gid + "\r\n\r\n";
        }
    }
    desc += "</xml>\r\n";
    prof += "</xml>\r\n";

    out.profiles = "npc_profile_" + mod;
    out.characters = "character_desc_" + mod;
    publish("configs\\gameplay\\" + out.characters + ".xml", desc);
    publish("configs\\gameplay\\" + out.profiles + ".xml", prof);

    // one table per language anything is named in, and always the game's two: a language a
    // character says nothing in falls back to its Russian, so no string is ever missing
    xr_vector<xr_string> langs{"rus", "eng"};
    for (const Character& c : all)
    {
        languages_of(c.name, langs);
        languages_of(c.bio, langs);
    }
    for (const xr_string& lang : langs)
    {
        xr_string st = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n\r\n<string_table>\r\n";
        for (const Character& c : all)
        {
            const xr_string gid = mod + "_" + c.id;
            st += "\t<string id=\"" + xml(gid + "_name") + "\">\r\n\t\t<text>" + xml(text_in(c.name, lang.c_str())) +
                "</text>\r\n\t</string>\r\n";
            if (!text_empty(c.bio))
                st += "\t<string id=\"" + xml(gid + "_bio") + "\">\r\n\t\t<text>" + xml(text_in(c.bio, lang.c_str())) +
                    "</text>\r\n\t</string>\r\n";
        }
        st += "</string_table>\r\n";
        publish("configs\\text\\" + lang + "\\" + mod + "_npc.xml", st);
    }

    out.count = u32(all.size());
    Msg("* XMS: [%s] %u character(s) read from the module", mod.c_str(), out.count);
    return true;
}

void PublishGenerated(const xr_string& relative, const xr_string& text) { publish(relative, text); }
xr_string GameText(const xr_string& text) { return game_text(text); }
xr_string XmlText(const xr_string& text) { return xml(text); }
xr_string TextIn(const LuaValue& text, pcstr lang) { return text_in(text, lang); }
void LanguagesOf(const LuaValue& text, xr_vector<xr_string>& out) { languages_of(text, out); }

void FinishCharacters()
{
    Published& state = published();
    xr_vector<xr_string> files;
    ListFilesRelative(state.root.c_str(), "*", true, files);
    for (const xr_string& file : files)
    {
        const xr_string path = state.root + file;
        if (state.wanted.count(lowered(path)))
            continue;
        DeleteFileA(path.c_str());
        // a folder that is left with nothing goes too; one with files in it refuses
        for (size_t slash = file.find_last_of('\\'); slash != xr_string::npos; slash = file.find_last_of('\\', slash - 1))
        {
            if (!RemoveDirectoryA((state.root + file.substr(0, slash)).c_str()) || !slash)
                break;
        }
    }
}
} // namespace XMS
