// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch_script.h"

#include "xms_pda.h"
#include "xms_game.h"
#include "ui/UIPdaLayout.h"
#include "ui/UIPdaWnd.h"
#include "UIGameCustom.h"
#include "xrScriptEngine/script_engine.hpp"

namespace
{
// engine-owned save blob: the {placeholder} values scripts set (see kCoreBlobs in xms_game.cpp)
constexpr u16 kVarsNs = 0xFF03;
constexpr u8 kVarsVersion = 1;

CUIPdaLayout* g_live = nullptr;

struct VarStore
{
    xr_map<xr_string, xr_string> values;
    const void* source = nullptr; // the blob the values were read from
    size_t source_size = 0;
};

VarStore& store()
{
    static VarStore v;
    return v;
}

// The staged blob is the truth - a new game clears it, a load replaces it - so the cache
// follows it instead of a session event of its own.
void sync_vars()
{
    VarStore& v = store();
    const xr_vector<u8>* blob = XmsGame::GetLoadedBlob(kVarsNs);
    const void* data = blob && !blob->empty() ? blob->data() : nullptr;
    const size_t size = blob ? blob->size() : 0;
    if (data == v.source && size == v.source_size)
        return;
    v.values.clear();
    v.source = data;
    v.source_size = size;
    if (!data || size < 5)
        return;
    IReader r(const_cast<void*>(data), int(size));
    if (r.r_u8() != kVarsVersion)
        return;
    const u32 count = r.r_u32();
    for (u32 i = 0; i < count && !r.eof(); ++i)
    {
        shared_str name, value;
        r.r_stringZ(name);
        r.r_stringZ(value);
        v.values[name.c_str()] = value.c_str();
    }
}

void write_vars()
{
    VarStore& v = store();
    if (v.values.empty())
        XmsGame::SetPendingBlob(kVarsNs, nullptr, 0);
    else
    {
        CMemoryWriter w;
        w.w_u8(kVarsVersion);
        w.w_u32(u32(v.values.size()));
        for (const auto& [name, value] : v.values)
        {
            w.w_stringZ(name.c_str());
            w.w_stringZ(value.c_str());
        }
        XmsGame::SetPendingBlob(kVarsNs, w.pointer(), w.size());
    }
    // the pending blob is what sync_vars compares against from now on
    const xr_vector<u8>* blob = XmsGame::GetLoadedBlob(kVarsNs);
    v.source = blob && !blob->empty() ? blob->data() : nullptr;
    v.source_size = blob ? blob->size() : 0;
}

bool valid_name(pcstr name)
{
    if (!name || !name[0])
        return false;
    for (pcstr p = name; *p; ++p)
        if (!(isalnum(u8(*p)) || *p == '_'))
            return false;
    return true;
}

CUIPdaWnd* pda_window()
{
    CUIGameCustom* ui = CurrentGameUI();
    return ui ? &ui->GetPdaMenu() : nullptr;
}

// ---- lua ------------------------------------------------------------------------------

pcstr pda_active() { return g_live ? g_live->Id() : nullptr; }

bool pda_set(pcstr name, pcstr value)
{
    if (!valid_name(name))
        return false;
    XmsPda::SetVar(name, value);
    return true;
}

luabind::object pda_get(lua_State* L, pcstr name)
{
    xr_string value;
    if (!XmsPda::GetVar(name, value) && !(g_live && g_live->DefaultVar(name, value)))
        return luabind::object();
    return luabind::object(L, value.c_str());
}

bool pda_show(pcstr id, int mode) { return g_live && g_live->ShowItem(id, mode); }
bool pda_page(pcstr id) { return g_live && g_live->Page(id); }
pcstr pda_current() { return g_live ? g_live->CurrentPage() : nullptr; }
bool pda_text(pcstr id, pcstr text) { return g_live && g_live->SetText(id, text); }
bool pda_rows(pcstr id, luabind::object items) { return g_live && g_live->SetRows(id, items); }
bool pda_click(pcstr id, int row) { return g_live && g_live->Click(id, row); }

// Opens the PDA (on a page, when one is named) - "a new order came in".
bool pda_open(pcstr page)
{
    CUIPdaWnd* pda = pda_window();
    if (!pda || !g_live)
        return false;
    if (page && page[0] && !g_live->Page(page))
        return false;
    if (!pda->IsShown())
        pda->ShowDialog(true);
    return true;
}

void pda_close()
{
    if (CUIPdaWnd* pda = pda_window(); pda && pda->IsShown())
        pda->HideDialog();
}

pcstr pda_describe()
{
    static xr_string out;
    out.clear();
    if (g_live)
        g_live->Describe(out);
    else if (const XMS::Module* m = XmsPda::Owner())
    {
        out = "declared by ";
        out += m->id;
        out += " (";
        out += m->pda_layout;
        out += "), not built";
    }
    else
        out = "no module layout";
    return out.c_str();
}

luabind::object pda_source(lua_State* L)
{
    const XMS::Module* m = XmsPda::Owner();
    if (!m)
        return luabind::object();
    luabind::object t = luabind::newtable(L);
    t["module"] = m->id.c_str();
    t["path"] = m->pda_layout.c_str();
    return t;
}
} // namespace

namespace XmsPda
{
const XMS::Module* Owner()
{
    const XMS::Module* owner = nullptr;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled || m.pda_layout.empty() || !XMS::ModuleApplies(m))
            continue;
        if (owner)
        {
            // once per pair and process: the PDA is rebuilt on every level load
            static xr_set<xr_string> reported;
            const xr_string key = owner->id + ">" + m.id;
            if (reported.insert(key).second)
            {
                Msg("! XMS PDA: modules [%s] and [%s] both bring a PDA layout - [%s] wins (later in the load order)",
                    owner->id.c_str(), m.id.c_str(), m.id.c_str());
                XMS::AddConflict(XMS::ConflictKind::Other, "PDA layout", owner->id.c_str(), m.id.c_str());
            }
        }
        owner = &m;
    }
    return owner;
}

void SetLive(CUIPdaLayout* layout) { g_live = layout; }
CUIPdaLayout* Live() { return g_live; }

void SetVar(pcstr name, pcstr value)
{
    if (!valid_name(name))
        return;
    sync_vars();
    VarStore& v = store();
    if (value)
        v.values[name] = value;
    else
        v.values.erase(name);
    write_vars();
}

bool GetVar(pcstr name, xr_string& value)
{
    if (!name)
        return false;
    sync_vars();
    const auto& values = store().values;
    const auto it = values.find(name);
    if (it == values.end())
        return false;
    value = it->second;
    return true;
}

void Vars(xr_vector<std::pair<xr_string, xr_string>>& out)
{
    sync_vars();
    out.assign(store().values.begin(), store().values.end());
}

void ScriptRegister(lua_State* L)
{
    using namespace luabind;
    module(L)
    [
        def("xms_native_pda_active", &pda_active),
        def("xms_native_pda_source", &pda_source),
        def("xms_native_pda_set", &pda_set),
        def("xms_native_pda_get", &pda_get),
        def("xms_native_pda_show", &pda_show),
        def("xms_native_pda_page", &pda_page),
        def("xms_native_pda_current", &pda_current),
        def("xms_native_pda_text", &pda_text),
        def("xms_native_pda_rows", &pda_rows),
        def("xms_native_pda_click", &pda_click),
        def("xms_native_pda_open", &pda_open),
        def("xms_native_pda_close", &pda_close),
        def("xms_native_pda_describe", &pda_describe)
    ];
}
} // namespace XmsPda
