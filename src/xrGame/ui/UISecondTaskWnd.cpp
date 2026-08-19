////////////////////////////////////////////////////////////////////////////
//	Module 		: UISecondTaskWnd.cpp
//	Created 	: 30.05.2008
//	Author		: Evgeniy Sokolov
//	Description : UI Secondary Task Wnd class impl
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "UISecondTaskWnd.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "UIXmlInit.h"
#include "UIHelper.h"
#include "xrUICore/Windows/UIFrameWindow.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Buttons/UICheckButton.h"
#include "xrUICore/Windows/UIFrameLineWnd.h"
#include "xrUICore/ScrollBar/UIFixedScrollBar.h"
#include "xrUICore/Hint/UIHint.h"
#include "GameTaskDefs.h"
#include "GameTask.h"
#include "map_location.h"
#include "UIInventoryUtilities.h"
#include "Level.h"
#include "GametaskManager.h"
#include "Actor.h"

// geometry of one sub-objective row, in the same base units the task layout uses
static constexpr float OBJECTIVE_ROW_H = 16.0f;
static constexpr float OBJECTIVE_INDENT = 32.0f;
static constexpr float OBJECTIVE_DOT_SIZE = 11.0f;
static constexpr float OBJECTIVE_MARK_SIZE = 13.0f;

UITaskListWnd::UITaskListWnd() : CUIWindow("UITaskListWnd") {}

void UITaskListWnd::RelayoutItems() const
{
    if (m_list)
        m_list->ForceUpdate();
}

void UITaskListWnd::init_from_xml(CUIXml& xml, LPCSTR path)
{
    VERIFY(hint_wnd);
    CUIXmlInit::InitWindow(xml, path, 0, this);

    const XML_NODE stored_root = xml.GetLocalRoot();
    const XML_NODE tmpl_root = xml.NavigateToNode(path, 0);
    xml.SetLocalRoot(tmpl_root);

    m_background = UIHelper::CreateFrameWindow(xml, "background_frame", this);
    m_caption = UIHelper::CreateStatic(xml, "t_caption", this);
    //	m_counter    = UIHelper::CreateStatic( xml, "t_counter", this );

    m_bt_close = UIHelper::Create3tButton(xml, "btn_close", this);
    m_bt_close->SetAccelerator(kUI_BACK, false, 2);

    Register(m_bt_close);
    AddCallback(m_bt_close, BUTTON_CLICKED, CUIWndCallback::void_function(this, &UITaskListWnd::OnBtnClose));
    UI().Focus().UnregisterFocusable(m_bt_close);

    m_list = xr_new<CUIScrollView>();
    m_list->SetAutoDelete(true);
    AttachChild(m_list);
    CUIXmlInit::InitScrollView(xml, "task_list", 0, m_list);
    m_orig_h = GetHeight();

    m_list->SetWindowName("---second_task_list");
    m_list->m_sort_function = +[](CUIWindow* left, CUIWindow* right) -> bool
    {
        const auto* lpi = smart_cast<UITaskListWndItem*>(left);
        const auto* rpi = smart_cast<UITaskListWndItem*>(right);
        VERIFY(lpi && rpi);
        return lpi->get_priority_task() > rpi->get_priority_task();
    };

    xml.SetLocalRoot(stored_root);
}

bool UITaskListWnd::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (inherited::OnMouseAction(x, y, mouse_action))
    {
        return true;
    }
    return true;
}

void UITaskListWnd::OnMouseScroll(float iDirection)
{
    if (int(iDirection) == WINDOW_MOUSE_WHEEL_UP)
        m_list->ScrollBar()->TryScrollDec();
    else if (int(iDirection) == WINDOW_MOUSE_WHEEL_DOWN)
        m_list->ScrollBar()->TryScrollInc();
}

void UITaskListWnd::Show(bool status)
{
    inherited::Show(status);

    auto& focus = UI().Focus();

    if (status)
    {
        UpdateList();
        GetMessageTarget()->SetKeyboardCapture(this, true);
        focus.LockToWindow(this);

        if (pInput->IsCurrentInputTypeController())
        {
            if (m_list->Empty())
            {
                focus.SetFocused(nullptr);
                UI().GetUICursor().WarpToWindow(m_list, true);
            }
            else
            {
                const auto item = static_cast<UITaskListWndItem*>(m_list->Items()[0]);
                item->Focus();
            }
        }
    }
    else
    {
        if (GetMessageTarget()->GetKeyboardCapturer() == this)
            GetMessageTarget()->SetKeyboardCapture(nullptr, true);
        if (focus.GetLocker() == this)
            focus.Unlock();
        GetMessageTarget()->SendMessage(GetMessageTarget(), WINDOW_KEYBOARD_CAPTURE_LOST, this);
    }

    GetMessageTarget()->SendMessage(this, PDA_TASK_HIDE_HINT, nullptr);
    Enable(status); // hack to prevent m_bt_close intercepting quit from PDA itself
}

void UITaskListWnd::OnFocusReceive()
{
    inherited::OnFocusReceive();
    GetMessageTarget()->SendMessage(this, PDA_TASK_HIDE_HINT, nullptr);
}

void UITaskListWnd::OnFocusLost()
{
    inherited::OnFocusLost();
    GetMessageTarget()->SendMessage(this, PDA_TASK_HIDE_HINT, nullptr);
}

void UITaskListWnd::Update()
{
    inherited::Update();

    // The list was only ever built when it was shown - and closing the PDA does not hide it,
    // it just stops drawing it. So a task finished while the player was out in the world stayed
    // on this list, greyed out by its own state, until the list was closed and opened again.
    // The manager stamps a frame whenever a task is given or changes state; that is the cue.
    if (IsShown())
    {
        const u32 frame = Level().GameTaskManager().ActualFrame();
        if (frame != m_actual_frame)
            UpdateList();
    }
}

void UITaskListWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    GetMessageTarget()->SendMessage(pWnd, msg, pData);
    inherited::SendMessage(pWnd, msg, pData);
    CUIWndCallback::OnEvent(pWnd, msg, pData);
}

void UITaskListWnd::OnBtnClose(CUIWindow* w, void* d)
{
    Show(false);
    m_bt_close->SetButtonState(CUIButton::BUTTON_NORMAL);
}

void UITaskListWnd::UpdateList()
{
    m_actual_frame = Level().GameTaskManager().ActualFrame();
    const int prev_scroll_pos = m_list->GetCurrentScrollPos();

    m_list->Clear();

    u32 count_for_check = 0;
    const vGameTasks& tasks = Level().GameTaskManager().GetGameTasks();

    for (const auto& key : tasks)
    {
        CGameTask* task = key.game_task;

        if (!task || task->GetTaskState() != eTaskStateInProgress)
            continue;
        if (m_show_only_secondary_tasks && task->GetTaskType() == eTaskTypeStoryline)
            continue;

        auto* item = xr_new<UITaskListWndItem>();
        if (item->init_task(task, this))
        {
            m_list->AddWindow(item, true);
            ++count_for_check;
        }
    } // for
    m_list->SetScrollPos(prev_scroll_pos);
}

/*
void UITaskListWnd::UpdateCounter()
{
    u32  m_progress_task_count = Level().GameTaskManager().GetTaskCount( eTaskStateInProgress );
    CGameTask* act_task = Level().GameTaskManager().ActiveTask();
    u32 task2_index     = Level().GameTaskManager().GetTaskIndex( act_task, eTaskStateInProgress );

    string32 buf;
    xr_sprintf( buf, sizeof(buf), "%d / %d", task2_index, m_progress_task_count );
    m_counter->SetText( buf );
}
*/
// - -----------------------------------------------------------------------------------------------

UITaskListWndItem::UITaskListWndItem() : CUIWindow("UITaskListWndItem")
{
     m_color_states[0] = u32(-1);
     m_color_states[1] = u32(-1);
     m_color_states[2] = u32(-1);
}

IC u32 UITaskListWndItem::get_priority_task() const
{
    VERIFY(m_task);
    return m_task->m_priority;
}

bool UITaskListWndItem::init_task(CGameTask* task, UITaskListWnd* parent)
{
    VERIFY(task);
    if (!task)
    {
        return false;
    }
    m_task = task;
    m_parent = parent;
    SetMessageTarget(parent);

    CUIXml xml;
    xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, PDA_TASK_XML);

    CUIXmlInit::InitWindow(xml, "second_task_wnd:task_item", 0, this);

    m_name = UIHelper::Create3tButton(xml, "second_task_wnd:task_item:name", this);
    m_bt_view = UIHelper::CreateCheck(xml, "second_task_wnd:task_item:btn_view", this, false);
    m_st_story = UIHelper::CreateStatic(xml, "second_task_wnd:task_item:st_story", this, false);
    m_bt_focus = UIHelper::Create3tButton(xml, "second_task_wnd:task_item:btn_focus", this);

    m_color_states[stt_activ] = CUIXmlInit::GetColor(xml, "second_task_wnd:task_item:activ", 0, u32(-1));
    m_color_states[stt_unread] = CUIXmlInit::GetColor(xml, "second_task_wnd:task_item:unread", 0, u32(-1));
    m_color_states[stt_read] = CUIXmlInit::GetColor(xml, "second_task_wnd:task_item:read", 0, u32(-1));

    m_min_h = GetHeight();
    build_objectives(xml);
    update_view();

    if (m_bt_view)
        UI().Focus().UnregisterFocusable(m_bt_view);
    UI().Focus().UnregisterFocusable(m_bt_focus);
    return true;
}

// A task can be a single line ("bring five pistols") or a list of steps, each with its own
// text and its own map spot. The engine has carried the steps forever (CGameTask keeps a
// vector of SGameTaskObjective); nothing drew them, so a multi-step task looked like a
// one-line task with the steps invisible. One row per step, under the task that owns them.
void UITaskListWndItem::build_objectives(CUIXml& xml)
{
    const TASK_OBJECTIVE_ID count = m_task->GetObjectivesCount(true);
    m_objectives.reserve(count);

    for (TASK_OBJECTIVE_ID i = 1; i <= count; ++i)
    {
        SObjectiveRow row;
        row.idx = i;

        row.dot = xr_new<CUIStatic>("objective_state");
        row.dot->SetAutoDelete(true);
        // the task's own icon at a smaller size: it is the one texture this layout is
        // guaranteed to have, since the row above draws it
        row.dot->InitTexture("ui_inGame2_PDA_icon_Secondary_mission", false);
        row.dot->SetStretchTexture(true);
        row.dot->SetWndSize(Fvector2().set(OBJECTIVE_DOT_SIZE, OBJECTIVE_DOT_SIZE));
        AttachChild(row.dot);

        row.text = xr_new<CUIStatic>("objective_text");
        row.text->SetAutoDelete(true);
        row.text->SetFont(UI().Font().pFontLetterica16Russian);
        // take whatever font the task's own line uses when the layout offers one
        CUIXmlInit::InitText(xml, "second_task_wnd:task_item:name:text", 0, row.text->TextItemControl());
        // whatever the task's line does with alignment, a list of steps reads left to right
        row.text->TextItemControl()->SetTextAlignment(CGameFont::alLeft);
        row.text->SetTextComplexMode(true);
        row.text->SetText("");
        AttachChild(row.text);

        row.marker = xr_new<CUIStatic>("objective_marker");
        row.marker->SetAutoDelete(true);
        row.marker->InitTexture("ui_inGame2_pda_center_on_mission_button_e", false);
        row.marker->SetStretchTexture(true);
        row.marker->SetWndSize(Fvector2().set(OBJECTIVE_MARK_SIZE, OBJECTIVE_MARK_SIZE));
        AttachChild(row.marker);

        m_objectives.emplace_back(row);
    }
}

// Places the visible rows under the task's line and answers with the height they took.
float UITaskListWndItem::layout_objectives(float top)
{
    if (m_objectives.empty())
        return 0.0f;

    const float width = GetWidth();
    const float text_x = OBJECTIVE_INDENT + OBJECTIVE_DOT_SIZE + 6.0f;
    const float mark_x = _max(text_x, width - OBJECTIVE_MARK_SIZE - 2.0f);
    float y = top;

    const TASK_OBJECTIVE_ID live = m_task->GetObjectivesCount(true);
    for (SObjectiveRow& row : m_objectives)
    {
        // the rows were built from the task; never index past what it holds now
        if (row.idx > live)
        {
            row.dot->Show(false);
            row.text->Show(false);
            row.marker->Show(false);
            continue;
        }
        SGameTaskObjective& objective = m_task->Objective(row.idx);

        // Collapsed, not just transparent: a hidden step is skipped before anything is
        // placed, so the rows below it move up and the item ends shorter.
        if (objective.m_hidden || !objective.m_Title.size())
        {
            row.dot->Show(false);
            row.text->Show(false);
            row.marker->Show(false);
            continue;
        }

        row.text->SetWndPos(Fvector2().set(text_x, y));
        row.text->SetWidth(mark_x - text_x - 4.0f);
        row.text->SetTextST(objective.m_Title.c_str());
        row.text->AdjustHeightToText();
        const float row_h = _max(OBJECTIVE_ROW_H, row.text->GetHeight());

        u32 color = m_color_states[stt_unread];
        switch (objective.GetTaskState())
        {
        case eTaskStateCompleted: color = m_color_states[stt_read]; break;
        case eTaskStateFail: color = color_rgba(150, 80, 80, 255); break;
        default:
            if (m_task->ActiveObjectiveIdx() == row.idx)
                color = m_color_states[stt_activ];
            break;
        }
        row.text->SetTextColor(color);

        row.dot->Show(true);
        row.dot->SetWndPos(Fvector2().set(OBJECTIVE_INDENT, y + (row_h - OBJECTIVE_DOT_SIZE) * 0.5f));
        row.dot->SetTextureColor(color);

        row.text->Show(true);

        // the step marks a place on the map only when it names a target of its own
        CMapLocation* ml = objective.LinkedMapLocation();
        row.marker->Show(ml && ml->SpotEnabled());
        row.marker->SetWndPos(Fvector2().set(mark_x, y + (row_h - OBJECTIVE_MARK_SIZE) * 0.5f));

        y += row_h + 2.0f;
    }

    return y - top;
}

// The step under the cursor, or nothing when the cursor is not over a row.
SGameTaskObjective* UITaskListWndItem::objective_at(float x, float y) const
{
    const TASK_OBJECTIVE_ID live = m_task->GetObjectivesCount(true);
    for (const SObjectiveRow& row : m_objectives)
    {
        if (!row.text->IsShown() || row.idx > live)
            continue;

        Frect r;
        row.text->GetAbsoluteRect(r);
        // the whole row answers, not only the glyphs: the dot and the marker are part of it
        r.x1 -= OBJECTIVE_INDENT;
        r.x2 += OBJECTIVE_MARK_SIZE + 6.0f;
        if (r.in(x, y))
            return &m_task->Objective(row.idx);
    }
    return nullptr;
}

void UITaskListWndItem::hide_hint()
{
    show_hint_can = false;
    show_hint = false;
    GetMessageTarget()->SendMessage(this, PDA_TASK_HIDE_HINT, nullptr);
}

void UITaskListWndItem::Update()
{
    inherited::Update();
    update_view();

    if (m_task && m_name->CursorOverWindow() && show_hint_can)
    {
        if (Device.dwTimeGlobal > (m_name->FocusReceiveTime() + 700 * Device.time_factor()))
        {
            show_hint = true;
            GetMessageTarget()->SendMessage(this, PDA_TASK_SHOW_HINT, (void*)static_cast<SGameTaskObjective*>(m_task));
            return;
        }
    }

    // a step shows only its title in the list, so its description lives in the same hint the
    // task uses - hovering the row is the only place it can be read at all
    if (m_task && !m_objectives.empty())
    {
        const auto [cx, cy] = UI().GetUICursor().GetCursorPosition();
        SGameTaskObjective* hot = CursorOverWindow() ? objective_at(cx, cy) : nullptr;
        const TASK_OBJECTIVE_ID idx = hot ? hot->GetID() : ROOT_TASK_OBJECTIVE;
        if (idx != m_hot_row)
        {
            m_hot_row = idx;
            m_hot_since = Device.dwTimeGlobal;
        }
        else if (hot && hot->m_Description.size() &&
                 Device.dwTimeGlobal > (m_hot_since + u32(700 * Device.time_factor())))
        {
            show_hint = true;
            GetMessageTarget()->SendMessage(this, PDA_TASK_SHOW_HINT, (void*)hot);
        }
    }
}

void UITaskListWndItem::update_view()
{
    VERIFY(m_task);
    CMapLocation* ml = m_task->LinkedMapLocation();

    if (ml && ml->SpotEnabled())
    {
        if (m_bt_view)
            m_bt_view->SetCheck(false);
        else
            m_bt_focus->Show(true);
    }
    else
    {
        if (m_bt_view)
            m_bt_view->SetCheck(true);
        else
            m_bt_focus->Show(false);
    }

    if (m_st_story)
    {
        if (m_task->GetTaskType() == eTaskTypeStoryline)
            m_st_story->InitTexture("ui_inGame2_PDA_icon_Primary_mission");
        else
            m_st_story->InitTexture("ui_inGame2_PDA_icon_Secondary_mission");
    }

    m_name->TextItemControl()->SetTextST(m_task->m_Title.c_str());
    m_name->AdjustHeightToText();
    const float name_bottom = m_name->GetWndPos().y + m_name->GetHeight();

    const float rows_h = layout_objectives(name_bottom + 2.0f);
    float h1 = name_bottom + (rows_h > 0.0f ? rows_h + 8.0f : 10.0f);
    h1 = _max(h1, m_min_h);
    if (!fsimilar(h1, GetHeight()))
    {
        // a step that finished, or one that was just hidden, changes how tall this item is;
        // the list has to place the items below it again or they overlap
        SetHeight(h1);
        if (m_parent)
            m_parent->RelayoutItems();
    }

    const CGameTask* storyTask = Level().GameTaskManager().ActiveTask(eTaskTypeStoryline);
    const CGameTask* additionalTask = Level().GameTaskManager().ActiveTask(eTaskTypeAdditional);

    if (m_task == storyTask || m_task == additionalTask)
    {
        m_name->SetStateTextColor(m_color_states[stt_activ], S_Enabled);
    }
    else if (m_task->m_read)
    {
        m_name->SetStateTextColor(m_color_states[stt_read], S_Enabled);
    }
    else
    {
        m_name->SetStateTextColor(m_color_states[stt_unread], S_Enabled);
    }
}

void UITaskListWndItem::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    if (pWnd == m_bt_focus)
    {
        if (msg == BUTTON_DOWN)
        {
            GetMessageTarget()->SendMessage(this, PDA_TASK_SET_TARGET_MAP, (void*)static_cast<SGameTaskObjective*>(m_task));
        }
    }
    if (pWnd == m_bt_view && m_bt_view)
    {
        if (m_bt_view->GetCheck() && msg == BUTTON_CLICKED)
        {
            GetMessageTarget()->SendMessage(this, PDA_TASK_HIDE_MAP_SPOT, (void*)static_cast<SGameTaskObjective*>(m_task));
            return;
        }
        if (!m_bt_view->GetCheck() && msg == BUTTON_CLICKED)
        {
            GetMessageTarget()->SendMessage(this, PDA_TASK_SHOW_MAP_SPOT, (void*)static_cast<SGameTaskObjective*>(m_task));
            return;
        }
    }

    if (pWnd == m_name)
    {
        if (msg == BUTTON_DOWN)
        {
            Level().GameTaskManager().SetActiveTask(m_task);
            return;
        }

        if (msg == WINDOW_LBUTTON_DB_CLICK)
        {
            GetMessageTarget()->SendMessage(this, PDA_TASK_SET_TARGET_MAP, (void*)static_cast<SGameTaskObjective*>(m_task));
        }
    }

    inherited::SendMessage(pWnd, msg, pData);
}

bool UITaskListWndItem::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (inherited::OnMouseAction(x, y, mouse_action))
    {
        // return true;
    }

    switch (mouse_action)
    {
    case WINDOW_LBUTTON_DOWN:
    case WINDOW_RBUTTON_DOWN:
    case BUTTON_DOWN:
    {
        hide_hint();
        break;
    }
    } // switch

    // a step answers for itself: clicking it makes it the current one and points the map
    // at its own spot, so a task with several places is navigable step by step
    if (mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_LBUTTON_DB_CLICK)
    {
        if (SGameTaskObjective* objective = objective_at(x, y))
        {
            Level().GameTaskManager().SetActiveTask(m_task);
            m_task->SetActiveObjective(objective->GetID());
            GetMessageTarget()->SendMessage(this, PDA_TASK_SET_TARGET_MAP, (void*)objective);
        }
    }

    return true;
}

bool UITaskListWndItem::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    if (inherited::OnKeyboardAction(dik, keyboard_action))
        return true;

    if (CursorOverWindow())
    {
        const auto [x, y] = UI().GetUICursor().GetCursorPosition();

        switch (GetBindedAction(dik, EKeyContext::UI))
        {
        case kUI_ACCEPT:
            m_name->OnMouseAction(x, y, WINDOW_LBUTTON_DOWN);
            return true;
        case kUI_ACTION_1:
            m_bt_focus->OnMouseAction(x, y, WINDOW_LBUTTON_DOWN);
            return true;
        case kUI_ACTION_2:
            if (m_bt_view)
                m_bt_view->OnMouseAction(x, y, WINDOW_LBUTTON_DOWN);
            return true;
        }
    }

    return false;
}

void UITaskListWndItem::Focus() const
{
    UI().Focus().SetFocused(m_name);
}

void UITaskListWndItem::OnFocusReceive()
{
    inherited::OnFocusReceive();
    hide_hint();
    show_hint_can = true;
}

void UITaskListWndItem::OnFocusLost()
{
    inherited::OnFocusLost();
    hide_hint();
}
