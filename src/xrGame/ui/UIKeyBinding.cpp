#include "StdAfx.h"
#include "UIKeyBinding.h"
#include "UIXmlInit.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "UIEditKeyBind.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrEngine/xr_level_controller.h"

CUIKeyBinding::CUIKeyBinding()
    : CUIWindow(CUIKeyBinding::GetDebugType()),
      m_header{ "Game actions header", "Primary bindings header", "Secondary bindings header" },
      m_frame( "Frame" )
{
    for (auto& header : m_header)
        AttachChild(&header);

    AttachChild(&m_frame);

    pInput->RegisterKeyMapChangeWatcher(this, REG_PRIORITY_NORMAL);
}

CUIKeyBinding::~CUIKeyBinding()
{
    pInput->RemoveKeyMapChangeWatcher(this);
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

void CUIKeyBinding::OnKeyMapChanged()
{
    for (CUIWindow* item : m_scroll_wnd->Items())
    {
        for (CUIWindow* wnd : item->GetChildWndList())
        {
            CUIEditKeyBind* keybind = smart_cast<CUIEditKeyBind*>(wnd);
            if (!keybind)
                continue;

            keybind->SetValue(); // re-apply
        }
    }
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
