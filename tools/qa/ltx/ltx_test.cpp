// What a module's config patches and items say, without the game: tools\qa\ltx\Run-LtxQa.ps1.
// The .ltxp reader with #file (src/xrCore/XMS/xms_ltxp.cpp), the rule a change follows into the
// sections that inherit it (xms_inherit.h) and what an .itemasset asks of the configs (xms_items.cpp).

#include "xrCore/XMS/xms_inherit.h"
#include "xrCore/XMS/xms_items.h"
#include "xrCore/XMS/xms_ltxp.h"
#include "xrCore/XMS/xms_tables.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

using Directives = std::vector<XMS::LtxDirective>;

Directives parse(const char* text)
{
    Directives out;
    XMS::ParseLtxp(text, std::strlen(text), out);
    return out;
}

bool is(const XMS::LtxDirective& d, XMS::LtxDirective::Op op, const char* file, const char* section, const char* key = "",
    const char* value = "")
{
    return d.op == op && d.file == file && d.section == section && d.key == key && d.value == value;
}

// a config of sections with parents, as the engine keeps it
struct Config
{
    struct Section
    {
        std::vector<std::string> parents;
        std::set<std::string> own;
        std::map<std::string, std::string> data;
    };
    std::map<std::string, Section> sections;
    std::map<std::string, std::vector<std::string>> children_of;

    // what reading `[name] : parents` and its own lines does: the parents' keys copied, a later one wins
    void read(const std::string& name, const std::vector<std::string>& parents, const std::map<std::string, std::string>& lines)
    {
        Section s;
        s.parents = parents;
        for (const std::string& p : parents)
        {
            for (const auto& [k, v] : sections[p].data)
                s.data[k] = v;
            children_of[p].push_back(name);
        }
        for (const auto& [k, v] : lines)
        {
            s.data[k] = v;
            s.own.insert(k);
        }
        sections[name] = s;
    }
    void set(const std::string& name, const std::string& key, const std::string& value)
    {
        sections[name].data[key] = value;
        if (!sections[name].parents.empty())
            sections[name].own.insert(key);
        XMS::PropagateKey(*this, name, key);
    }
    void remove(const std::string& name, const std::string& key)
    {
        sections[name].data.erase(key);
        if (!sections[name].parents.empty())
            sections[name].own.insert(key);
        XMS::PropagateKey(*this, name, key);
    }
    std::string value(const std::string& name, const std::string& key)
    {
        const auto& data = sections[name].data;
        const auto it = data.find(key);
        return it == data.end() ? "<none>" : it->second;
    }

    // the model PropagateKey asks
    using Name = std::string;
    using Node = Section;
    using Value = std::string;
    const std::vector<std::string>* children(const Name& n) const
    {
        const auto it = children_of.find(n);
        return it == children_of.end() ? nullptr : &it->second;
    }
    Node* find(const Name& n)
    {
        const auto it = sections.find(n);
        return it == sections.end() ? nullptr : &it->second;
    }
    const std::vector<std::string>& parents(const Node& s) const { return s.parents; }
    bool own(const Node& s, const Name& key) const { return s.own.count(key) != 0; }
    bool get(const Node& s, const Name& key, Value& v) const
    {
        const auto it = s.data.find(key);
        if (it == s.data.end())
            return false;
        v = it->second;
        return true;
    }
    void put(Node& s, const Name& key, const Value& v, const Node&) { s.data[key] = v; }
    void take(Node& s, const Name& key) { s.data.erase(key); }
};

bool plan(const char* text, XMS::ItemPlan& out, std::string& why, const char* file = "items\\my_item.itemasset")
{
    XMS::LuaValue v;
    std::string error;
    if (!XMS::ReadLuaData(text, std::strlen(text), v, error))
    {
        why = error;
        return false;
    }
    return XMS::PlanItem(v, "qa", file, out, why);
}

const XMS::LtxDirective* find(const Directives& list, XMS::LtxDirective::Op op, const char* section, const char* key,
    const char* file = nullptr)
{
    for (const XMS::LtxDirective& d : list)
        if (d.op == op && d.section == section && d.key == key && (!file || d.file == file))
            return &d;
    return nullptr;
}

bool has(const Directives& list, XMS::LtxDirective::Op op, const char* key, const char* value)
{
    for (const XMS::LtxDirective& d : list)
        if (d.op == op && d.key == key && d.value == value)
            return true;
    return false;
}

bool table(XMS::TableKind kind, const char* text, XMS::TablePlan& out, std::string& why)
{
    XMS::LuaValue v;
    std::string error;
    if (!XMS::ReadLuaData(text, std::strlen(text), v, error))
    {
        why = error;
        return false;
    }
    return XMS::PlanTable(kind, v, out, why);
}

bool table_refused(XMS::TableKind kind, const char* text, const char* why_starts)
{
    XMS::TablePlan p;
    std::string why;
    const bool ok = table(kind, text, p, why);
    if (ok || why.rfind(why_starts, 0) != 0)
        std::printf("     got: %s\n", ok ? "(planned)" : why.c_str());
    return !ok && why.rfind(why_starts, 0) == 0;
}

bool refused(const char* text, const char* why_starts)
{
    XMS::ItemPlan p;
    std::string why;
    const bool ok = plan(text, p, why);
    if (ok || why.rfind(why_starts, 0) != 0)
        std::printf("     got: %s\n", ok ? "(planned)" : why.c_str());
    return !ok && why.rfind(why_starts, 0) == 0;
}
} // namespace

int main()
{
    // ---- the .ltxp reader -------------------------------------------------------------------
    {
        const Directives d = parse("[WPN_AK74]   ; a comment\r\ncost = 3000\r\n+ammo_class = ammo_5.45x39_ap\r\n"
                                   "-ammo_class=ammo_5.45x39_fmj\r\n!immunities\r\n[!wpn_obsolete]\r\nx = 1\r\n"
                                   "[madmer.wpn_gold] : wpn_ak74, wpn_ak74_sounds\r\n#include \"a.ltx\"\r\n");
        expect(d.size() == 8, "every directive of a plain patch is read");
        expect(d.size() == 8 && is(d[0], XMS::LtxDirective::Section, "", "wpn_ak74") && d[0].block == 1,
            "a header is a section, its name in lower case");
        expect(d.size() == 8 && is(d[1], XMS::LtxDirective::Set, "", "wpn_ak74", "cost", "3000") && d[1].block == 1,
            "a key under it");
        expect(d.size() == 8 && is(d[2], XMS::LtxDirective::Add, "", "wpn_ak74", "ammo_class", "ammo_5.45x39_ap"),
            "+key adds to a list");
        expect(d.size() == 8 && is(d[3], XMS::LtxDirective::Drop, "", "wpn_ak74", "ammo_class", "ammo_5.45x39_fmj"),
            "-key drops from one");
        expect(d.size() == 8 && is(d[4], XMS::LtxDirective::RemoveKey, "", "wpn_ak74", "immunities"), "!key");
        expect(d.size() == 8 && is(d[5], XMS::LtxDirective::RemoveSection, "", "wpn_obsolete") && d[6].block == d[5].block,
            "[!section], and the keys after it stay in its block (which changes nothing)");
        expect(d.size() == 8 && d[7].op == XMS::LtxDirective::Section && d[7].parents == "wpn_ak74, wpn_ak74_sounds",
            "a new section with its parents; an #include line is nothing");
    }
    {
        const Directives d = parse("[a]\nk = 1\n#file misc\\death_generic.ltx ; loot\nlost = 1\n[item_count]\nmy_item = 0, 1\n"
                                   "#FILE Misc/Trade/./Trade_Military.ltx\n[supplies_1]\nmy_item = 1, 0.5\n#file\nlost_too = 1\n[b]\nk = 2\n");
        expect(d.size() == 10, "#file lines are no directives of their own");
        expect(d.size() == 10 && d[2].file == "misc\\death_generic.ltx" && d[2].block == 0 && d[2].key == "lost",
            "a key after #file and before a header is under none");
        expect(d.size() == 10 && is(d[3], XMS::LtxDirective::Section, "misc\\death_generic.ltx", "item_count"),
            "#file sends the directives after it to that config");
        expect(d.size() == 10 && is(d[4], XMS::LtxDirective::Set, "misc\\death_generic.ltx", "item_count", "my_item", "0, 1"),
            "and its keys");
        expect(d.size() == 10 && is(d[5], XMS::LtxDirective::Section, "misc\\trade\\trade_military.ltx", "supplies_1"),
            "case and slashes do not matter in #file");
        expect(d.size() == 10 && d[7].file.empty() && d[7].block == 0, "#file alone goes back to the system config");
        expect(d.size() == 10 && is(d[9], XMS::LtxDirective::Set, "", "b", "k", "2"), "where headers work as before");
    }
    {
        Directives d = parse("[a]\nk = 1\n");
        XMS::ParseLtxp("[b]\n", 4, d);
        expect(d.size() == 3 && d[2].block == 2, "a second file's blocks go on from the first's");
    }
    {
        const Directives d = parse("#file ..\\outside.ltx\n[x]\nk = 1\n#file C:\\game\\x.ltx\n[y]\nk = 1\n!k\n#file misc\\a.ltx\n[z]\nk = 2\n");
        expect(d.size() == 2 && is(d[0], XMS::LtxDirective::Section, "misc\\a.ltx", "z"),
            "after a #file that names no config nothing is read until the next #file - never onto the system config");
    }
    expect(XMS::ConfigPath("Misc//Trade\\.\\X.LTX") == "misc\\trade\\x.ltx", "a config's path: lower case, one '\\'");
    expect(XMS::ConfigPath("\\a\\..\\b.ltx") == "b.ltx", "'..' is resolved");
    expect(XMS::ConfigPath("..\\b.ltx").empty(), "a path that climbs out is none");

    // ---- a change reaches what inherits ---------------------------------------------------------------
    {
        Config c;
        c.read("wpn_ak74", {}, {{"ammo_class", "a, b"}, {"cost", "100"}});
        c.read("wpn_ak74_kobra", {"wpn_ak74"}, {{"scope_status", "2"}});
        c.read("wpn_ak74_ps01", {"wpn_ak74"}, {{"ammo_class", "a"}});
        c.read("wpn_ak74_ps01_x", {"wpn_ak74_ps01"}, {});
        c.read("wpn_ak74_kobra_x", {"wpn_ak74_kobra"}, {});
        c.set("wpn_ak74", "ammo_class", "a,b,c");
        expect(c.value("wpn_ak74_kobra", "ammo_class") == "a,b,c", "a child that does not say the key takes the change");
        expect(c.value("wpn_ak74_kobra_x", "ammo_class") == "a,b,c", "so does its child, through the chain");
        expect(c.value("wpn_ak74_ps01", "ammo_class") == "a", "a child that says the key keeps its own");
        expect(c.value("wpn_ak74_ps01_x", "ammo_class") == "a", "and so does what inherits from it");
        c.set("wpn_ak74", "silencer_status", "2");
        expect(c.value("wpn_ak74_kobra_x", "silencer_status") == "2", "a key the parent did not have reaches them too");
        c.set("wpn_ak74_kobra", "cost", "200");
        c.set("wpn_ak74", "cost", "300");
        expect(c.value("wpn_ak74_kobra", "cost") == "200" && c.value("wpn_ak74_kobra_x", "cost") == "200",
            "a key a directive set on the child is its own from then on");
    }
    {
        Config c;
        c.read("p1", {}, {{"k", "1"}});
        c.read("p2", {}, {{"k", "2"}});
        c.read("x", {"p1", "p2"}, {});
        expect(c.value("x", "k") == "2", "a later parent wins when the section is read");
        c.set("p1", "k", "10");
        expect(c.value("x", "k") == "2", "a change of an earlier parent does not beat a later one");
        c.set("p2", "k", "20");
        expect(c.value("x", "k") == "20", "a change of the last parent with the key reaches it");
        c.remove("p2", "k");
        expect(c.value("x", "k") == "10", "a key taken away falls back to an earlier parent's");
        c.remove("p1", "k");
        expect(c.value("x", "k") == "<none>", "or goes, when no parent has it");
    }
    {
        Config c;
        c.read("a", {}, {{"k", "1"}});
        c.read("b", {"a"}, {});
        c.remove("b", "k");
        c.set("a", "k", "2");
        expect(c.value("b", "k") == "<none>", "a key a directive took away does not come back with the parent's change");
    }

    // ---- an item ---------------------------------------------------------------------------------
    {
        XMS::ItemPlan p;
        std::string why;
        const bool ok = plan(R"(return {
  item = 1,
  id = "sil_diy",
  base = { "wpn_sil_pbs1" },
  name = { rus = "Глушитель", eng = "Silencer" },
  description = { rus = "..." },
  icon = { texture = [[ui\we_icons]], x = 3, y = 0, w = 2, h = 1 },
  set = { cost = 1200, inv_weight = 0.4, quest_item = false, hands_position = { 0.02, -0.01, 0.1 } },
  hud = { base = "wpn_addon_silencer_hud", set = { item_visual = [[a\b]] } },
  sections = { { name = "wpn_sil_diy_sounds", base = "base_sounds", set = { snd_shoot = [[weapons\sil\diy_shot]] } },
               { edit = "wpn_pm", add = { scopes = "sil_diy" } } },
  scene = { like = "vodka", sound = [[interface\item_usage\vodka_drink]], time = 7200, helmet = false, fit = "cut",
            hud = { base = "item_ea_vodka_hud", set = { item_visual = [[x\y]] } }, set = { my_mod_scene = true } },
  editor = { note = "the game reads none of this" },
})", p, why);
        expect(ok, "a new item is planned");
        if (!ok)
            std::printf("     why: %s\n", why.c_str());
        expect(!p.edit && p.section == "qa_sil_diy" && p.parents == "wpn_sil_pbs1", "its section is <module>_<id>, over its base");
        expect(has(p.own, XMS::LtxDirective::Set, "cost", "1200") && has(p.own, XMS::LtxDirective::Set, "inv_weight", "0.4"),
            "a number without needless decimals");
        expect(has(p.own, XMS::LtxDirective::Set, "quest_item", "false"), "a boolean as false");
        expect(has(p.own, XMS::LtxDirective::Set, "hands_position", "0.02, -0.01, 0.1"), "a list joined by ', '");
        expect(has(p.own, XMS::LtxDirective::Set, "inv_name", "qa_sil_diy_name") &&
                has(p.own, XMS::LtxDirective::Set, "inv_name_short", "qa_sil_diy_name") &&
                has(p.own, XMS::LtxDirective::Set, "description", "qa_sil_diy_descr"),
            "the strings are the game's ids; the short name is the name when left out");
        expect(p.texts.size() == 2 && p.texts[0].id == "qa_sil_diy_name", "the texts to publish");
        expect(has(p.own, XMS::LtxDirective::Set, "icons_texture", "ui\\we_icons") &&
                has(p.own, XMS::LtxDirective::Set, "inv_grid_x", "3") && has(p.own, XMS::LtxDirective::Set, "inv_grid_width", "2"),
            "the icon in the game's form");
        expect(p.hud && p.hud_parents == "wpn_addon_silencer_hud" && has(p.own, XMS::LtxDirective::Set, "hud", "qa_sil_diy_hud") &&
                has(p.hud_ops, XMS::LtxDirective::Set, "item_visual", "a\\b"),
            "the hud section, and the item names it");
        expect(p.sections.size() == 2 && p.sections[0].make && p.sections[0].parents == "base_sounds" && !p.sections[1].make &&
                has(p.sections[1].ops, XMS::LtxDirective::Add, "scopes", "sil_diy"),
            "a section it makes and one it changes");
        const auto* scene = find(p.lists, XMS::LtxDirective::Section, "qa_sil_diy", "", "items\\items\\animations_settings.ltx");
        expect(scene && scene->parents == "vodka", "scene: the item's section over the scene it is like");
        expect(find(p.lists, XMS::LtxDirective::Set, "qa_sil_diy", "snd", "items\\items\\animations_settings.ltx") &&
                find(p.lists, XMS::LtxDirective::Set, "qa_sil_diy", "tm", "items\\items\\animations_settings.ltx") &&
                find(p.lists, XMS::LtxDirective::Set, "qa_sil_diy", "helm", "items\\items\\animations_settings.ltx"),
            "scene: the keys under the game's names");
        const auto* anm = find(p.lists, XMS::LtxDirective::Set, "qa_sil_diy", "anm", "items\\items\\animations_settings.ltx");
        expect(anm && anm->value == "item_ea_qa_sil_diy", "scene: its own hud gives the scene its animation");
        const auto* hud = find(p.lists, XMS::LtxDirective::Section, "item_ea_qa_sil_diy_hud", "", "");
        expect(hud && hud->parents == "item_ea_vodka_hud", "scene: the hud section in the system config");
        const auto* fit = find(p.lists, XMS::LtxDirective::Set, "qa_sil_diy", "fit", "items\\items\\animations_settings.ltx");
        expect(fit && fit->value == "cut", "scene: how a long cycle fits the scene");
        const auto* own = find(p.lists, XMS::LtxDirective::Set, "qa_sil_diy", "my_mod_scene", "items\\items\\animations_settings.ltx");
        expect(own && own->value == "true", "scene: a key of its own, as the scene's section holds it");
        expect(p.foreign.empty(), "an item that says only what it is has nothing of another file's");
        bool headed = true;
        unsigned block = 0;
        for (const XMS::LtxDirective& d : p.lists)
        {
            if (d.op == XMS::LtxDirective::Section)
                block = d.block;
            else
                headed = headed && d.block == block && d.block != 0;
        }
        expect(headed, "every line of the lists is under its own header");
    }
    {
        XMS::ItemPlan p;
        std::string why;
        const bool ok = plan(R"(return {
  item = 1,
  edit = "WPN_AK74",
  set   = { snd_shoot = [[weapons\ak74\ak74_shoot]] },
  unset = { "snd_shoot1" },
  add   = { ammo_class = { "ammo_5.45x39_ap" } },
  drop  = { ammo_class = "ammo_5.45x39_fmj" },
  hud   = { set = { anm_reload = "ak74_reload, ak74_reload_w" }, unset = { "anm_bore" } },
  icon  = "icons/wpn_ak74.png",
})", p, why);
        expect(ok && p.edit && p.section == "wpn_ak74", "a change names the section it changes");
        expect(p.own.size() == 4 && p.own[0].op == XMS::LtxDirective::Set && p.own[1].op == XMS::LtxDirective::RemoveKey &&
                p.own[2].op == XMS::LtxDirective::Add && p.own[2].value == "ammo_5.45x39_ap" &&
                p.own[3].op == XMS::LtxDirective::Drop,
            "set, unset, add, drop, in that order");
        expect(p.hud && p.hud_ops.size() == 2 && !has(p.own, XMS::LtxDirective::Set, "hud", "wpn_ak74_hud"),
            "a change's hud changes the one the item names");
        expect(p.icon_picture, "a picture for an icon is what only a built module turns into the game's form");
    }
    {
        XMS::ItemPlan p;
        std::string why;
        const bool ok = plan("return { item = 1, base = 'vodka', scene = 'vodka', abstract = false }", p, why, "x\\My-Drink.itemasset");
        expect(ok && p.section == "qa_my_drink", "the file name is the id when the file names none");
        const auto* scene = find(p.lists, XMS::LtxDirective::Section, "qa_my_drink", "", "items\\items\\animations_settings.ltx");
        expect(scene && scene->parents == "vodka", "a scene by the item it is like");
        XMS::ItemPlan t;
        expect(plan("return { item = 1, id = 'mixin', abstract = true, set = { a = 1 }, name = 'x', scene = 'vodka' }", t, why) &&
                t.abstract_ && t.texts.empty() && t.lists.empty() && t.own.size() == 1,
            "a template is its section and nothing else");
        XMS::ItemPlan f;
        expect(plan("return { item = 1, base = 'a', sold = { { by = 'trade_military' } }, loot = { count = 1 }, "
                    "craft = { { at = 'kit', parts = { 'a' } } }, lists = { { section = 's' } } }", f, why) &&
                f.foreign.size() == 4 && f.lists.empty(),
            "what another file owns - stock, loot, a recipe, a line of a config - is named and never read from an item");
    }
    expect(refused("return { item = 1, edit = 'wpn_ak74', id = 'x' }", "a change names its section with edit alone"),
        "a change that also names a new item is refused");
    expect(refused("return { item = 1, id = 'x' }", "a new item starts from a base"), "a new item without a base is refused");
    expect(refused("return { item = 1, id = '1x', base = 'a' }", "the id is"), "an id that starts with a digit is refused");
    expect(refused("return { item = 1, section = 'Bad Name', base = 'a' }", "the section [bad name] is not"),
        "a section that is not a-z, 0-9, _, . and - is refused");
    expect(refused("return { item = 1, section = string.rep and 'x' or 'y', base = 'a' }", "line 1"),
        "an expression is no data (the reader says so)");
    expect(refused(("return { item = 1, base = 'a', section = '" + std::string(64, 'a') + "' }").c_str(), "the section ["),
        "a section of 64 characters is refused");
    expect(refused("return { item = 1, base = 'a', set = { cost = 'a\\nb' } }", "the value of cost holds characters"),
        "a value with a line break is refused");
    expect(refused("return { item = 1, base = 'a', set = { ['a b'] = 1 } }", "'a b' cannot be a key"),
        "a key that is no name is refused");
    expect(refused("return { item = 1, base = 'a', set = { x = { y = 1 } } }", "the value of x is no"),
        "a value that is no value is refused");
    expect(refused("return { item = 1, edit = 'x', hud = { base = 'y' } }", "hud of a change"),
        "a change's hud takes no base");

    // ---- a trader, a loot table, recipes -------------------------------------------------------------
    {
        XMS::TablePlan p;
        std::string why;
        const bool ok = table(XMS::TableKind::Trade, R"(return {
  trade = 1,
  trader = "Trade_Military",
  stock = {
    { item = "QA_Sil_Diy", count = 1, chance = 0.5 },
    { item = "qa_ammo", supplies = { "supplies_1", "Supplies_2" }, count = 40 },
    { item = "vodka", count = 0 },
  },
})", p, why);
        expect(ok && p.rows == 3 && p.recipes.empty(), "a trader's stock is planned");
        if (!ok)
            std::printf("     why: %s\n", why.c_str());
        const auto* one = find(p.lists, XMS::LtxDirective::Set, "supplies_1", "qa_sil_diy", "misc\\trade\\trade_military.ltx");
        expect(one && one->value == "1, 0.5", "a row is the item's line of the trader's first stock, in lower case");
        const auto* two = find(p.lists, XMS::LtxDirective::Set, "supplies_2", "qa_ammo", "misc\\trade\\trade_military.ltx");
        expect(two && two->value == "40, 1", "in every stock section it names, the chance 1 when left out");
        expect(find(p.lists, XMS::LtxDirective::RemoveKey, "supplies_1", "vodka", "misc\\trade\\trade_military.ltx"),
            "a count of 0 takes the item off the shelf");
        XMS::TablePlan q;
        expect(table(XMS::TableKind::Trade, "return { trade = 1, trader = [[misc\\trade\\trade_barman.ltx]], stock = { { item = 'a' } } }", q, why) &&
                find(q.lists, XMS::LtxDirective::Set, "supplies_1", "a", "misc\\trade\\trade_barman.ltx"),
            "a trader by its config's path");
        bool headed = true;
        unsigned block = 0;
        for (const XMS::LtxDirective& d : p.lists)
        {
            if (d.op == XMS::LtxDirective::Section)
                block = d.block;
            else
                headed = headed && d.block == block && d.block != 0;
        }
        expect(headed, "every line of a table is under its own header");
    }
    {
        XMS::TablePlan p;
        std::string why;
        const bool ok = table(XMS::TableKind::Loot,
            "return { loot = 1, drops = { { item = 'qa_sil_diy', count = { 0, 1 }, chance = 0.01, by = { Bandit = 0.03 } } } }", p, why);
        expect(ok && p.rows == 1, "a loot table is planned");
        const auto* count = find(p.lists, XMS::LtxDirective::Set, "item_count", "qa_sil_diy", "misc\\death_generic.ltx");
        expect(count && count->value == "0, 1", "loot: its line of [item_count]");
        const auto* base = find(p.lists, XMS::LtxDirective::Set, "base", "qa_sil_diy", "misc\\death_items_by_communities.ltx");
        expect(base && base->value == "0.01", "loot: its chance in [base], which every community inherits");
        const auto* bandit = find(p.lists, XMS::LtxDirective::Set, "bandit", "qa_sil_diy", "misc\\death_items_by_communities.ltx");
        expect(bandit && bandit->value == "0.03", "loot: a community's own chance");
    }
    {
        XMS::TablePlan p;
        std::string why;
        const bool ok = table(XMS::TableKind::Recipes,
            "return { recipe = 1, recipes = { { makes = 'QA_Sil_Diy', at = 'itm_repairkit_tier_1', "
            "parts = { 'jar', 'textile_patch_m', 'rope', 'flashlight_broken' } }, { makes = 'bread', count = 2, at = 'kit', parts = 'jar' } } }",
            p, why);
        expect(ok && p.recipes.size() == 2 && p.lists.empty(), "recipes are planned");
        expect(ok && p.recipes[0].makes == "qa_sil_diy" && p.recipes[0].at == "itm_repairkit_tier_1" && p.recipes[0].count == 1 &&
                p.recipes[0].parts.size() == 4 && p.recipes[1].count == 2 && p.recipes[1].parts.size() == 1,
            "a recipe: what it makes, where, how many, from what");
    }
    expect(table_refused(XMS::TableKind::Trade, "return { trade = 1, stock = {} }", "a trader file names the trader"),
        "a trader file without its trader is refused");
    expect(table_refused(XMS::TableKind::Trade, "return { trade = 1, trader = '..\\\\x.ltx', stock = {} }", "trader is a trader's config"),
        "a trader that climbs out of the configs is refused");
    expect(table_refused(XMS::TableKind::Trade, "return { trade = 1, trader = 't', stock = { { count = 1 } } }", "a row of stock names its item"),
        "a stock row without its item is refused");
    expect(table_refused(XMS::TableKind::Trade, "return { trade = 1, trader = 't', stock = { { item = 'a', count = -1 } } }", "stock.count"),
        "a count below 0 is refused");
    expect(table_refused(XMS::TableKind::Loot, "return { loot = 1, drops = { { item = 'a', count = 'many' } } }", "drops.count"),
        "a count that is no number is refused");
    expect(table_refused(XMS::TableKind::Recipes,
               "return { recipe = 1, recipes = { { makes = 'x', at = 'kit', parts = { 'a', 'b', 'c', 'd', 'e' } } } }",
               "a recipe takes one to four parts"),
        "a recipe of five parts is refused");
    expect(table_refused(XMS::TableKind::Recipes, "return { recipe = 1, recipes = { { at = 'kit', parts = { 'a' } } } }",
               "a recipe names what it makes"),
        "a recipe that makes nothing is refused");

    XMS::LuaValue n;
    n.type = XMS::LuaValue::Number;
    std::string text;
    n.n = 1e-5;
    expect(XMS::ItemValueText(n, text) && text == "1e-05", "a small number keeps its digits");
    n.n = -3;
    expect(XMS::ItemValueText(n, text) && text == "-3", "a whole number is written whole");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
