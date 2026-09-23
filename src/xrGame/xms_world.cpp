// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch_script.h"

// XMS world rules - see xms_world.h and docs/dead-air/MODDING.md, "World rules".

#include "xms_world.h"
#include "xms_game.h"
#include "level_changer.h"
#include "Level.h"
#include "ai_space.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path_storage.h"
#include "xrScriptEngine/script_engine.hpp"

extern xr_vector<CLevelChanger*> g_lchangers;

namespace XmsWorld
{
namespace
{
using Rules = XMS::WorldRules;

// engine-owned .scov blob 0x584DFF02, next to the NQ state at 0xFF01
constexpr u16 kWorldNs = 0xFF02;
constexpr u8 kBlobVersion = 1;
constexpr pcstr kClosedInvitation = "st_xms_transition_closed";
constexpr pcstr kDefaultInvitation = "level_changer_invitation";
// a navmesh cell this far from a drawn waypoint still counts as its place
constexpr float kPathSnap = 15.f;

struct RuleSet
{
    xr_string module;
    bool keep[Rules::Count]{true, true, true, true, true};
    xr_vector<xr_string> levels; // empty = everywhere
};

struct State
{
    xr_vector<RuleSet> sets;
    // quests' runtime word on a rule, for the whole world: 0 unset, 1 remove, 2 keep
    u8 overrides[Rules::Count]{};
    xr_vector<xr_string> area;          // empty = anywhere
    xr_vector<xr_string> locked_levels;
    xr_vector<xr_string> locked;        // level changer names
    xr_vector<xr_string> unlocked;      // explicit opens, beating every lock
    bool start_placed{false};
    xr_string start_module, start_level;
    // this process only
    bool new_game{false};
    bool faction_jump_armed{false};
    bool console_jump{false};
    // destinations of explicitly opened changers the player has met on this level: the
    // author opened that way, so the area does not close it again at the server
    xr_vector<xr_string> reachable;
};

State& st()
{
    static State s;
    return s;
}

bool same(pcstr a, pcstr b) { return a && b && 0 == _stricmp(a, b); }

bool contains(const xr_vector<xr_string>& list, pcstr value)
{
    if (!value || !value[0])
        return false;
    for (const xr_string& item : list)
        if (same(item.c_str(), value))
            return true;
    return false;
}

void add_unique(xr_vector<xr_string>& list, pcstr value)
{
    if (value && value[0] && !contains(list, value))
        list.emplace_back(value);
}

void remove_item(xr_vector<xr_string>& list, pcstr value)
{
    for (auto it = list.begin(); it != list.end();)
        it = same(it->c_str(), value) ? list.erase(it) : it + 1;
}

bool transition_rules()
{
    const State& s = st();
    return !s.area.empty() || !s.locked_levels.empty() || !s.locked.empty() || !s.unlocked.empty();
}

// ---- persistence -------------------------------------------------------------------
void write_list(CMemoryWriter& w, const xr_vector<xr_string>& list)
{
    w.w_u32(u32(list.size()));
    for (const xr_string& item : list)
        w.w_stringZ(item.c_str());
}

bool read_list(IReader& r, xr_vector<xr_string>& list)
{
    if (r.elapsed() < int(sizeof(u32)))
        return false;
    const u32 count = r.r_u32();
    for (u32 i = 0; i < count; ++i)
    {
        if (r.eof())
            return false;
        shared_str item;
        r.r_stringZ(item);
        list.emplace_back(item.c_str() ? item.c_str() : "");
    }
    return true;
}

// Every change goes to the pending blob at once, so whatever the next save captures
// is the world as it is. A session with no world at all erases the chunk instead of
// writing an empty one into every save.
void write_blob()
{
    const State& s = st();
    const bool empty = s.sets.empty() && !transition_rules() && !s.start_placed &&
        std::all_of(std::begin(s.overrides), std::end(s.overrides), [](u8 v) { return v == 0; });
    if (empty)
    {
        XmsGame::SetPendingBlob(kWorldNs, nullptr, 0);
        return;
    }
    CMemoryWriter w;
    w.w_u8(kBlobVersion);
    w.w_u32(u32(s.sets.size()));
    for (const RuleSet& set : s.sets)
    {
        w.w_stringZ(set.module.c_str());
        u8 keep_bits = 0;
        for (u32 i = 0; i < Rules::Count; ++i)
            if (set.keep[i])
                keep_bits |= u8(1u << i);
        w.w_u8(keep_bits);
        write_list(w, set.levels);
    }
    for (u32 i = 0; i < Rules::Count; ++i)
        w.w_u8(s.overrides[i]);
    write_list(w, s.area);
    write_list(w, s.locked_levels);
    write_list(w, s.locked);
    write_list(w, s.unlocked);
    w.w_u8(s.start_placed ? 1 : 0);
    w.w_stringZ(s.start_module.c_str());
    w.w_stringZ(s.start_level.c_str());
    XmsGame::SetPendingBlob(kWorldNs, w.pointer(), w.size());
}

bool read_blob(const xr_vector<u8>& blob, State& out)
{
    if (blob.empty())
        return false;
    IReader r(const_cast<u8*>(blob.data()), int(blob.size()));
    if (r.r_u8() != kBlobVersion)
        return false;
    const u32 sets = r.r_u32();
    for (u32 i = 0; i < sets; ++i)
    {
        if (r.eof())
            return false;
        RuleSet set;
        shared_str module;
        r.r_stringZ(module);
        set.module = module.c_str() ? module.c_str() : "";
        const u8 keep_bits = r.r_u8();
        for (u32 k = 0; k < Rules::Count; ++k)
            set.keep[k] = 0 != (keep_bits & (1u << k));
        if (!read_list(r, set.levels))
            return false;
        out.sets.push_back(std::move(set));
    }
    for (u32 i = 0; i < Rules::Count; ++i)
        out.overrides[i] = r.eof() ? 0 : r.r_u8();
    if (!read_list(r, out.area) || !read_list(r, out.locked_levels) || !read_list(r, out.locked) ||
        !read_list(r, out.unlocked))
        return false;
    if (!r.eof())
    {
        out.start_placed = r.r_u8() != 0;
        shared_str module, level;
        r.r_stringZ(module);
        r.r_stringZ(level);
        out.start_module = module.c_str() ? module.c_str() : "";
        out.start_level = level.c_str() ? level.c_str() : "";
    }
    return true;
}

// ---- graph helpers --------------------------------------------------------------
pcstr level_of_vertex(u16 game_vertex_id)
{
    if (!ai().get_game_graph())
        return nullptr;
    const CGameGraph& graph = ai().game_graph();
    if (!graph.valid_vertex_id(game_vertex_id))
        return nullptr;
    const GameGraph::_LEVEL_ID id = graph.vertex(game_vertex_id)->level_id();
    return graph.header().level_exist(id) ? graph.header().level(id).name().c_str() : nullptr;
}

pcstr current_level()
{
    return g_pGameLevel ? Level().name().c_str() : nullptr;
}

void report_session(pcstr why)
{
    const State& s = st();
    if (s.sets.empty() && !transition_rules() && !s.start_placed)
        return;
    xr_string text;
    Describe(text);
    Msg("* XMS world (%s):%s", why, text.c_str());
}

// The Lua half (xms_world.script) filters what the game's scripts spawn by wrapping
// sim_board, smart_terrain and xr_effects. It goes in here, once the session knows it
// HAS a world - never from the xms bootstrap: the first script engine is built while the
// game persistent itself is being constructed, and loading xr_effects then creates its
// particles against a persistent that does not exist yet (an access violation in
// CPS_Instance). alife restarts the engine for every game and resolves the world after
// that, before a single object registers, which is exactly when the filters are needed.
// A game with no world hooks nothing at all.
void install_lua_filters()
{
    if (!Declared() || !GEnv.ScriptEngine)
        return;
    lua_State* L = GEnv.ScriptEngine->lua();
    if (!L)
        return;
    static constexpr pcstr install = "if xms_world and xms_world.install then xms_world.install() end";
    if (0 != luaL_loadbuffer(L, install, xr_strlen(install), "@xms_world_install") || 0 != lua_pcall(L, 0, 0, 0))
    {
        Msg("! XMS world: the Lua filters could not be installed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}
} // namespace

// =================================================================================== session
void ResolveForNewGame(const CInifile* options)
{
    State& s = st();
    s = State();
    s.new_game = true;

    for (const XMS::Module& m : XMS::Modules())
    {
        if (!m.world.declared || !XMS::ModuleApplies(m))
            continue;
        RuleSet set;
        set.module = m.id;
        set.levels = m.world.levels;
        for (u32 i = 0; i < Rules::Count; ++i)
        {
            set.keep[i] = m.world.keep[i];
            // the author offered this one on the new-game screen: the player's word counts
            if (!m.world.player_choice[i] || !options)
                continue;
            string128 key;
            xr_sprintf(key, "new_game_xmsw_%s", Rules::Key(i));
            if (!options->line_exist("character_creation", key))
                continue;
            pcstr value = options->r_string("character_creation", key);
            set.keep[i] = !(value && (0 == xr_strcmp(value, "0") || 0 == _stricmp(value, "false")));
        }
        s.sets.push_back(std::move(set));
        for (const xr_string& level : m.world.area)
            add_unique(s.area, level.c_str());
        for (const xr_string& level : m.world.locked_levels)
            add_unique(s.locked_levels, level.c_str());
        for (const xr_string& name : m.world.locked)
            add_unique(s.locked, name.c_str());
    }
    write_blob();
    report_session("new game");
    install_lua_filters();
}

void OnLoaded(const xr_vector<u8>* blob)
{
    State& s = st();
    s = State();
    if (blob && !blob->empty())
    {
        State loaded;
        if (read_blob(*blob, loaded))
            s = std::move(loaded);
        else
            Msg("! XMS world: the save's world state is unreadable - the game runs without world rules");
    }
    s.new_game = false;
    s.faction_jump_armed = false;
    write_blob();
    report_session("loaded");
    install_lua_filters();
}

bool Declared() { return !st().sets.empty() || transition_rules(); }

bool Keep(u32 rule, pcstr level)
{
    if (rule >= Rules::Count)
        return true;
    const State& s = st();
    if (s.overrides[rule])
        return s.overrides[rule] == 2;
    for (const RuleSet& set : s.sets)
    {
        if (set.keep[rule])
            continue;
        if (set.levels.empty() || contains(set.levels, level))
            return false;
    }
    return true;
}

void SetOverride(u32 rule, int value)
{
    if (rule >= Rules::Count)
        return;
    st().overrides[rule] = value < 0 ? 0 : (value ? 2 : 1);
    write_blob();
}

// =================================================================================== transitions
bool InArea(pcstr level)
{
    const State& s = st();
    return s.area.empty() || contains(s.area, level);
}

void SetAreaLevel(pcstr level, bool in_area)
{
    if (!level || !level[0])
        return;
    State& s = st();
    if (in_area)
        add_unique(s.area, level);
    else
        remove_item(s.area, level);
    write_blob();
    RefreshOnline();
}

bool TransitionOpen(pcstr changer, pcstr level, pcstr dest_level)
{
    const State& s = st();
    if (contains(s.unlocked, changer))
        return true;
    if (contains(s.locked, changer))
        return false;
    if (contains(s.locked_levels, level))
        return false;
    if (!s.area.empty() && dest_level && !contains(s.area, dest_level))
        return false;
    return true;
}

void SetTransitionLock(pcstr changer, bool locked)
{
    if (!changer || !changer[0])
        return;
    State& s = st();
    if (locked)
    {
        remove_item(s.unlocked, changer);
        add_unique(s.locked, changer);
    }
    else
    {
        remove_item(s.locked, changer);
        add_unique(s.unlocked, changer);
    }
    write_blob();
    RefreshOnline();
}

void SetLevelLock(pcstr level, bool locked)
{
    if (!level || !level[0])
        return;
    State& s = st();
    if (locked)
        add_unique(s.locked_levels, level);
    else
        remove_item(s.locked_levels, level);
    write_blob();
    RefreshOnline();
}

void Apply(CLevelChanger& changer)
{
    if (!transition_rules())
        return;
    pcstr dest = level_of_vertex(changer.DestinationVertex());
    const bool open = TransitionOpen(changer.cName().c_str(), current_level(), dest);
    if (open && dest && contains(st().unlocked, changer.cName().c_str()))
        add_unique(st().reachable, dest);
    changer.EnableLevelChanger(open);
    if (!open)
        changer.SetLevelChangerInvitationStr(kClosedInvitation);
    else if (changer.LevelChangerInvitationStr() == kClosedInvitation)
        changer.SetLevelChangerInvitationStr(kDefaultInvitation);
}

void RefreshOnline()
{
    for (CLevelChanger* changer : g_lchangers)
        if (changer)
            Apply(*changer);
}

bool LevelChangeAllowed(u16 dest_game_vertex_id)
{
    State& s = st();
    if (s.console_jump)
    {
        s.console_jump = false;
        return true;
    }
    if (s.area.empty())
        return true;
    pcstr level = level_of_vertex(dest_game_vertex_id);
    return !level || contains(s.area, level) || contains(s.reachable, level);
}

void AllowNextLevelChange() { st().console_jump = true; }

// =================================================================================== start
void NoteStartPlaced(pcstr module_id, pcstr level)
{
    State& s = st();
    s.start_placed = true;
    s.start_module = module_id ? module_id : "";
    s.start_level = level ? level : "";
    // a loaded save already stands where it stands - only a new game has a faction jump coming
    if (s.new_game)
        s.faction_jump_armed = true;
    write_blob();
}

bool ConsumeFactionJump()
{
    State& s = st();
    if (!s.faction_jump_armed)
        return false;
    s.faction_jump_armed = false;
    return true;
}

void SettleStart() { st().faction_jump_armed = false; }

// =================================================================================== paths
void RegisterModulePaths(pcstr level_name)
{
    if (!level_name || !XMS::Active())
        return;
    CPatrolPathStorage* storage = ai().get_patrol_paths();
    if (!storage || !ai().get_level_graph() || !ai().get_cross_table() || !ai().get_game_graph())
        return;

    u32 registered = 0, refused = 0;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (!XMS::ModuleApplies(m))
            continue;
        xr_vector<xr_string> files;
        string_path dir;
        xr_sprintf(dir, "%sspawn" DELIMITER, m.root.c_str());
        XMS::ListFiles(dir, "*.xspawn", files);
        for (const xr_string& file : files)
        {
            xr_vector<XMS::IniRecord> records;
            XMS::ReadIniRecords(file.c_str(), records);

            // [path:<name>] sections, kept in file order
            struct PathSection
            {
                xr_string name;
                xr_vector<std::pair<xr_string, xr_string>> keys;
                pcstr value(pcstr key) const
                {
                    for (const auto& [k, v] : keys)
                        if (0 == xr_strcmp(k.c_str(), key))
                            return v.c_str();
                    return nullptr;
                }
            };
            xr_vector<PathSection> paths;
            for (XMS::IniRecord& record : records)
            {
                if (0 != strncmp(record.section.c_str(), "path:", 5))
                    continue;
                const xr_string name = record.section.substr(5);
                if (paths.empty() || paths.back().name != name)
                    paths.push_back({name, {}});
                paths.back().keys.emplace_back(std::move(record.key), std::move(record.value));
            }

            for (const PathSection& p : paths)
            {
                if (!same(p.value("level"), level_name))
                    continue;
                if (pcstr mode = p.value("mode"))
                    if (!XMS::ModeActive(mode))
                        continue;
                const int count = p.value("points") ? atoi(p.value("points")) : 0;
                xr_vector<CPatrolPathStorage::RuntimePoint> points;
                for (int i = 0; i < count; ++i)
                {
                    string64 key;
                    xr_sprintf(key, "p%d", i);
                    Fvector position;
                    pcstr text = p.value(key);
                    if (!text || 3 != sscanf(text, "%f , %f , %f", &position.x, &position.y, &position.z))
                        break;
                    CPatrolPathStorage::RuntimePoint point{position, 0, shared_str()};
                    xr_sprintf(key, "p%d_flags", i);
                    if (pcstr flags = p.value(key))
                        point.flags = u32(strtoul(flags, nullptr, 10));
                    xr_sprintf(key, "p%d_name", i);
                    if (pcstr point_name = p.value(key))
                        point.name = point_name;
                    points.push_back(point);
                }
                if (int(points.size()) != count || points.empty())
                {
                    Msg("! XMS: [%s] path %s: %d point(s) declared, %u readable - skipped", m.id.c_str(),
                        p.name.c_str(), count, u32(points.size()));
                    ++refused;
                    continue;
                }
                // "a-b:w, b-c:w" - a link per pair, the weight is the stock probability
                xr_vector<CPatrolPathStorage::RuntimeLink> links;
                if (pcstr text = p.value("links"))
                {
                    xr_string buffer = text;
                    for (char* item = strtok(&buffer[0], ","); item; item = strtok(nullptr, ","))
                    {
                        u32 from = 0, to = 0;
                        float weight = 1.f;
                        const int got = sscanf(item, " %u - %u : %f", &from, &to, &weight);
                        if (got >= 2)
                            links.push_back({from, to, got == 3 ? weight : 1.f});
                    }
                }
                if (storage->set_runtime_path(p.name.c_str(), points, links, kPathSnap, ai().get_level_graph(),
                        ai().get_cross_table(), ai().get_game_graph()))
                    ++registered;
                else
                {
                    Msg("! XMS: [%s] path %s on %s not registered - the name is the level's own, or a point "
                        "is more than %.0f m from the navmesh",
                        m.id.c_str(), p.name.c_str(), level_name, kPathSnap);
                    ++refused;
                }
            }
        }
    }
    if (registered || refused)
        Msg("* XMS: module waypoint paths on %s: %u registered, %u refused", level_name, registered, refused);
}

// =================================================================================== report
void Describe(xr_string& out)
{
    const State& s = st();
    out.clear();
    auto list = [](const xr_vector<xr_string>& items) {
        xr_string text;
        for (const xr_string& item : items)
        {
            if (!text.empty())
                text += ", ";
            text += item;
        }
        return text.empty() ? xr_string("-") : text;
    };
    for (const RuleSet& set : s.sets)
    {
        out += "\n  rules of ";
        out += set.module;
        out += " on ";
        out += set.levels.empty() ? xr_string("every level") : list(set.levels);
        out += ":";
        for (u32 i = 0; i < Rules::Count; ++i)
        {
            out += " ";
            out += Rules::Key(i);
            out += set.keep[i] ? "=keep" : "=REMOVE";
        }
    }
    for (u32 i = 0; i < Rules::Count; ++i)
        if (s.overrides[i])
        {
            out += "\n  quest override: ";
            out += Rules::Key(i);
            out += s.overrides[i] == 2 ? "=keep" : "=REMOVE";
        }
    out += "\n  area: ";
    out += s.area.empty() ? xr_string("every level") : list(s.area);
    out += "\n  locked levels: " + list(s.locked_levels);
    out += "\n  locked transitions: " + list(s.locked);
    out += "\n  opened transitions: " + list(s.unlocked);
    if (s.start_placed)
    {
        out += "\n  start: ";
        out += s.start_level;
        out += " (module ";
        out += s.start_module;
        out += ")";
    }
}

// =================================================================================== lua
namespace
{
u32 rule_of(pcstr key) { return Rules::FromKey(key); }

bool lua_keep(pcstr rule, pcstr level) { return Keep(rule_of(rule), level && level[0] ? level : nullptr); }
void lua_set_rule(pcstr rule, int value) { SetOverride(rule_of(rule), value); }
bool lua_in_area(pcstr level) { return InArea(level); }
void lua_area_level(pcstr level, bool in_area) { SetAreaLevel(level, in_area); }
void lua_transition_lock(pcstr changer, bool locked) { SetTransitionLock(changer, locked); }
void lua_level_lock(pcstr level, bool locked) { SetLevelLock(level, locked); }
bool lua_declared() { return Declared(); }
void lua_settle() { SettleStart(); }

// For a changer that is online the verdict is the one the player meets; for one that is
// not, the lock lists alone speak (its destination is not known without the object).
bool lua_transition_open(pcstr changer)
{
    for (CLevelChanger* c : g_lchangers)
        if (c && same(c->cName().c_str(), changer))
            return c->IsLevelChangerEnabled();
    return TransitionOpen(changer, nullptr, nullptr);
}

pcstr lua_describe()
{
    static xr_string text;
    Describe(text);
    return text.c_str();
}

// The rules a mode's module offers the player on the new-game screen, with the author's
// defaults: { { key = "npc_quests", keep = false }, ... } in rule order.
luabind::object lua_choices(lua_State* L, pcstr mode_id)
{
    luabind::object out = luabind::newtable(L);
    int n = 0;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled || !m.world.declared)
            continue;
        bool provides = false;
        for (const XMS::ProvidedMode& p : m.provides_modes)
            if (same(p.id.c_str(), mode_id))
                provides = true;
        if (!provides && !same(m.mode.c_str(), mode_id))
            continue;
        for (u32 i = 0; i < Rules::Count; ++i)
        {
            if (!m.world.player_choice[i])
                continue;
            luabind::object row = luabind::newtable(L);
            row["key"] = Rules::Key(i);
            row["keep"] = m.world.keep[i];
            row["module"] = m.id.c_str();
            out[++n] = row;
        }
    }
    return out;
}
} // namespace

void ScriptRegister(lua_State* L)
{
    using namespace luabind;
    module(L)
    [
        def("xms_native_world_keep", &lua_keep),
        def("xms_native_world_set_rule", &lua_set_rule),
        def("xms_native_world_in_area", &lua_in_area),
        def("xms_native_world_area_level", &lua_area_level),
        def("xms_native_world_transition_lock", &lua_transition_lock),
        def("xms_native_world_level_lock", &lua_level_lock),
        def("xms_native_world_transition_open", &lua_transition_open),
        def("xms_native_world_declared", &lua_declared),
        def("xms_native_world_settle", &lua_settle),
        def("xms_native_world_describe", &lua_describe),
        def("xms_native_world_choices", &lua_choices)
    ];
}
}
