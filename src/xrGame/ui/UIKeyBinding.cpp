#include "pch_script.h"
#include "UIKeyBinding.h"
#include "UIXmlInit.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "UIEditKeyBind.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Buttons/UIBtnHint.h"
#include "xrUICore/Cursor/UICursor.h"
#include "xrUICore/ui_base.h"
#include "xrEngine/StringTable/StringTable.h"
#include "xrEngine/xr_level_controller.h"

CUIKeyBinding::CUIKeyBinding()
    : CUIWindow(CUIKeyBinding::GetDebugType()),
      m_header{ "Game actions header", "Primary bindings header", "Secondary bindings header" },
      m_frame( "Frame" )
{
    for (auto& header : m_header)
        AttachChild(&header);

    AttachChild(&m_frame);
}

void CUIKeyBinding::InitFromXml(CUIXml& xml_doc, LPCSTR path)
{
    CUIXmlInit::InitWindow(xml_doc, path, 0, this);
    string256 buf;
    m_scroll_wnd = xr_new<CUIScrollView>();
    m_scroll_wnd->SetAutoDelete(true);
    AttachChild(m_scroll_wnd);
    CUIXmlInit::InitScrollView(xml_doc, strconcat(sizeof(buf), buf, path, ":scroll_view"), 0, m_scroll_wnd);

    m_isGamepadBinds = !!(xml_doc.ReadAttribInt(path, 0, "gamepad_bind", 0));
    CUIXmlInit::InitFrameWindow(xml_doc, strconcat(sizeof(buf), buf, path, ":frame"), 0, &m_frame);
    CUIXmlInit::InitFrameLine(xml_doc, strconcat(sizeof(buf), buf, path, ":header_1"), 0, &m_header[0]);
    CUIXmlInit::InitFrameLine(xml_doc, strconcat(sizeof(buf), buf, path, ":header_2"), 0, &m_header[1]);
    if (!m_isGamepadBinds)
        CUIXmlInit::InitFrameLine(xml_doc, strconcat(sizeof(buf), buf, path, ":header_3"), 0, &m_header[2]);

    FillUpList(xml_doc, path);
}

void CUIKeyBinding::FillUpList(CUIXml& xml_doc_ui, LPCSTR path_ui)
{
    string256 buf;
    CUIXml xml_doc;
    xml_doc.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, m_isGamepadBinds ? "ui_keybinding_gamepad.xml" : "ui_keybinding.xml");

    const bool addMapBinding = !m_isGamepadBinds && !IsActionExist("map", xml_doc);
    const int groupsCount = xml_doc.GetNodesNum("", 0, "group");

    const auto addCommand = [&](LPCSTR commandId, LPCSTR command)
    {
        CUIStatic* item = xr_new<CUIStatic>(commandId);
        CUIXmlInit::InitStatic(xml_doc_ui, strconcat(sizeof(buf), buf, path_ui, ":scroll_view:item_key"), 0, item);
        item->TextItemControl()->SetTextST(commandId);
        m_scroll_wnd->AddWindow(item, true);

        // The string "<caption id>_hint", where there is one, is the row's hover hint (see Update).
        string256 hint;
        strconcat(hint, commandId, "_hint");
        if (StringTable().has_translation(hint))
            m_rowHints.emplace_back(item, hint);

#ifdef DEBUG
        if (kNOTBINDED == ActionNameToId(command))
        {
            Msg("action [%s] not exist. update data", command);
            return;
        }
#endif

        float itemWidth = m_header[1].GetWidth() - 3.0f;
        float itemPos = m_header[1].GetWndPos().x;
        CUIEditKeyBind* editKB = m_isGamepadBinds ? xr_new<CUIEditKeyBind>(false, true) : xr_new<CUIEditKeyBind>(true);
        editKB->SetAutoDelete(true);
        editKB->InitKeyBind(Fvector2().set(itemPos, 0.0f), Fvector2().set(itemWidth, item->GetWndSize().y));
        editKB->AssignProps(command, m_isGamepadBinds ? "key_binding_gamepad" : "key_binding");
        item->AttachChild(editKB);

        if (!m_isGamepadBinds)
        {
            itemWidth = m_header[2].GetWidth() - 3.0f;
            itemPos = m_header[2].GetWndPos().x;
            editKB = xr_new<CUIEditKeyBind>(false);
            editKB->SetAutoDelete(true);
            editKB->InitKeyBind(Fvector2().set(itemPos, 0.0f), Fvector2().set(itemWidth, item->GetWndSize().y));
            editKB->AssignProps(command, "key_binding");
            item->AttachChild(editKB);
        }
    };

    for (int i = 0; i < groupsCount; ++i)
    {
        // add group
        shared_str grp_name = xml_doc.ReadAttrib("group", i, "name");
        R_ASSERT(xr_strlen(grp_name));

        CUIStatic* item = xr_new<CUIStatic>(grp_name.c_str());
        CUIXmlInit::InitStatic(xml_doc_ui, strconcat(sizeof(buf), buf, path_ui, ":scroll_view:item_group"), 0, item);
        item->TextItemControl()->SetTextST(grp_name.c_str());
        m_scroll_wnd->AddWindow(item, true);

        // add group items
        int commandsCount = xml_doc.GetNodesNum("group", i, "command");
        XML_NODE tab_node = xml_doc.NavigateToNode("group", i);
        xml_doc.SetLocalRoot(tab_node);

        for (int j = 0; j < commandsCount; ++j)
        {
            shared_str command_id = xml_doc.ReadAttrib("command", j, "id");
            shared_str exe = xml_doc.ReadAttrib("command", j, "exe");
            addCommand(command_id.c_str(), exe.c_str());
        }
        xml_doc.SetLocalRoot(xml_doc.GetRoot());

        if (addMapBinding && grp_name == "kb_grp_inventory")
            addCommand("kb_map", "map");
    }
#ifdef DEBUG
    CheckStructure(xml_doc);
#endif
}

void CUIKeyBinding::Update()
{
    // A row scrolled out of sight still lies under the cursor somewhere on the tab and, never drawn,
    // would hold the one static hint the controls around the list use: the rows offer their hints
    // only while the cursor is over the visible part of the list.
    if (m_scroll_wnd)
    {
        Frect view;
        m_scroll_wnd->GetAbsoluteRect(view);
        Fvector2 cursor = GetUICursor().GetCursorPosition();
        const bool offered = GetUICursor().IsVisible() && view.in(cursor);
        if (offered != m_rowHintsOffered)
        {
            m_rowHintsOffered = offered;
            for (auto& [row, hint] : m_rowHints)
            {
                row->m_stat_hint_text = offered ? hint : shared_str{};
                if (!offered && g_statHint->Owner() == row)
                    g_statHint->Discard();
            }
        }
    }
    CUIWindow::Update();
}

namespace
{
// The key list for a script that lays it out itself (the Controls page): the groups of
// ui_keybinding.xml in order, { name = <caption id>, actions = { { caption = <caption id>,
// action = <action> }, ... } } each. An action the engine does not know is left out, and the PDA
// map joins the backpack group when the file has no row for it, as in FillUpList.
luabind::object key_binding_actions(lua_State* L)
{
    CUIXml xml_doc;
    xml_doc.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_keybinding.xml");

    struct key_group
    {
        shared_str name;
        xr_vector<std::pair<shared_str, shared_str>> actions;
    };
    xr_vector<key_group> groups;
    bool hasMap = false, hasDebugMenu = false;

    const int groupsCount = static_cast<int>(xml_doc.GetNodesNum("", 0, "group"));
    for (int i = 0; i < groupsCount; ++i)
    {
        key_group& group = groups.emplace_back();
        group.name = xml_doc.ReadAttrib("group", i, "name");

        const int commandsCount = static_cast<int>(xml_doc.GetNodesNum("group", i, "command"));
        xml_doc.SetLocalRoot(xml_doc.NavigateToNode("group", i));
        for (int j = 0; j < commandsCount; ++j)
        {
            const shared_str action = xml_doc.ReadAttrib("command", j, "exe");
            hasMap |= action == "map";
            hasDebugMenu |= action == "debug_menu";
            if (ActionNameToPtr(action.c_str(), true))
                group.actions.emplace_back(xml_doc.ReadAttrib("command", j, "id"), action);
        }
        xml_doc.SetLocalRoot(xml_doc.GetRoot());
    }

    if (!hasMap && ActionNameToPtr("map", true))
    {
        for (key_group& group : groups)
        {
            if (group.name == "kb_grp_inventory")
                group.actions.emplace_back("kb_map", "map");
        }
    }

    // The debug menu's key (Options -> Game -> Other turns the menu on) joins the common group the same way.
    if (!hasDebugMenu && ActionNameToPtr("debug_menu", true))
    {
        for (key_group& group : groups)
        {
            if (group.name == "kb_grp_common")
                group.actions.emplace_back("kb_debug_menu", "debug_menu");
        }
    }

    luabind::object result = luabind::newtable(L);
    int groupIndex = 1;
    for (const key_group& group : groups)
    {
        if (group.actions.empty())
            continue;

        luabind::object actions = luabind::newtable(L);
        int actionIndex = 1;
        for (const auto& [caption, action] : group.actions)
        {
            luabind::object entry = luabind::newtable(L);
            entry["caption"] = caption.c_str();
            entry["action"] = action.c_str();
            actions[actionIndex++] = entry;
        }

        luabind::object entry = luabind::newtable(L);
        entry["name"] = group.name.c_str();
        entry["actions"] = actions;
        result[groupIndex++] = entry;
    }
    return result;
}
} // namespace

void CUIKeyBinding::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        def("key_binding_actions", &key_binding_actions)
    ];
}

#ifdef DEBUG
void CUIKeyBinding::CheckStructure(CUIXml& xml_doc)
{
    bool first = true;

    for (int i = 0; true; ++i)
    {
        if (cpcstr action_name = actions[i].action_name)
        {
            if (!IsActionExist(action_name, xml_doc))
            {
                CUIStatic* item = nullptr;
                if (first)
                {
                    item = xr_new<CUIStatic>("First action");
                    item->SetWndPos(Fvector2().set(0, 0));
                    item->SetWndSize(Fvector2().set(m_scroll_wnd->GetWndSize().x, 20.0f));
                    item->SetText("NEXT ITEMS NOT DESCRIBED IN COMMAND DESC LIST");
                    first = false;
                    item->SetAutoDelete(true);
                    m_scroll_wnd->AddWindow(item, true);
                }

                item = xr_new<CUIStatic>(action_name);
                item->SetWndPos(Fvector2().set(0, 0));
                item->SetWndSize(Fvector2().set(m_scroll_wnd->GetWndSize().x, 20.0f));
                item->SetText(action_name);
                item->SetAutoDelete(true);
                m_scroll_wnd->AddWindow(item, true);
            }
        }
        else
            break;
    }
}
#endif

bool CUIKeyBinding::IsActionExist(LPCSTR action, CUIXml& xml_doc)
{
    bool ret = false;
    int groupsCount = xml_doc.GetNodesNum("", 0, "group");

    for (int i = 0; i < groupsCount; ++i)
    {
        // add group items
        int commandsCount = xml_doc.GetNodesNum("group", i, "command");
        XML_NODE tab_node = xml_doc.NavigateToNode("group", i);
        xml_doc.SetLocalRoot(tab_node);

        for (int j = 0; j < commandsCount; ++j)
        {
            // first field of list item
            shared_str command_id = xml_doc.ReadAttrib("command", j, "exe");
            if (0 == xr_strcmp(action, command_id.c_str()))
            {
                ret = true;
                break;
            }
        }
        xml_doc.SetLocalRoot(xml_doc.GetRoot());
        if (ret)
            break;
    }
    return ret;
}
