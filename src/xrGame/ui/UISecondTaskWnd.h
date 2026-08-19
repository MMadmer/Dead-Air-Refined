////////////////////////////////////////////////////////////////////////////
//	Module 		: UISecondTaskWnd.h
//	Created 	: 30.05.2008
//	Author		: Evgeniy Sokolov
//	Description : UI Secondary Task Wnd class
////////////////////////////////////////////////////////////////////////////

#ifndef UI_SECOND_TASK_WND_H_INCLUDED
#define UI_SECOND_TASK_WND_H_INCLUDED

#include "xrUICore/Windows/UIWindow.h"
#include "xrUICore/Callbacks/UIWndCallback.h"
#include "GameTaskDefs.h"

#define PDA_TASK_XML "pda_tasks.xml"

class CUIXml;
class CUIFrameWindow;
class CUIScrollView;
class CUIStatic;
class CUI3tButton;
class CUICheckButton;
class CUIFrameLineWnd;
class CGameTask;
class SGameTaskObjective;
class CUIXml;
class UIHint;

class UITaskListWnd final : public CUIWindow, public CUIWndCallback
{
private:
    typedef CUIWindow inherited;

public:
    UITaskListWnd();

    void init_from_xml(CUIXml& xml, LPCSTR path);

    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
    virtual void OnMouseScroll(float iDirection);
    virtual void Show(bool status);
    virtual void OnFocusReceive();
    virtual void OnFocusLost();
    virtual void Update();
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData);

    void ShowOnlySecondaryTasks(bool mode) { m_show_only_secondary_tasks = mode; }

    void UpdateList();

    // an item whose height changed asks the list to place the items below it again
    void RelayoutItems() const;

    pcstr GetDebugType() override { return "UITaskListWnd"; }

protected:
    void OnBtnClose(CUIWindow* w, void* d);
    //			void	UpdateCounter		();

public:
    UIHint* hint_wnd{};

private: // m_
    CUIFrameWindow* m_background{};
    CUIScrollView* m_list{};

    CUIStatic* m_caption{};
    //	CUIStatic*			m_counter{};
    CUI3tButton* m_bt_close{};

    float m_orig_h{};
    bool m_show_only_secondary_tasks{};
    u32 m_actual_frame{};   // the task manager's, to notice tasks changing while this sits open
}; // class UITaskListWnd

// -------------------------------------------------------------------------------------------------

class UITaskListWndItem final : public CUIWindow
{
private:
    typedef CUIWindow inherited;

public:
    UITaskListWndItem();

    bool init_task(CGameTask* task, UITaskListWnd* parent);
    IC u32 get_priority_task() const;

    virtual void OnFocusReceive();
    virtual void OnFocusLost();
    virtual void Update();
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData);
    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
    bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override;

    void Focus() const;

private:
    void hide_hint();
    void update_view();
    void update_visible_map_spot();

    // sub-objectives: the steps of the task, one row each under its own task
    void build_objectives(CUIXml& xml);
    float layout_objectives(float top);
    SGameTaskObjective* objective_at(float x, float y) const;

public:
    bool show_hint_can{};
    bool show_hint{};

private:
    CGameTask* m_task{};
    CUI3tButton* m_name{};
    CUICheckButton* m_bt_view{};
    CUIStatic* m_st_story{};
    CUI3tButton* m_bt_focus{};

    // one row per step of the task; a hidden step keeps its row objects but is
    // shown nowhere and adds no height, exactly like a collapsed widget
    struct SObjectiveRow
    {
        TASK_OBJECTIVE_ID idx{};
        CUIStatic* dot{};
        CUIStatic* text{};
        CUIStatic* marker{};
    };
    xr_vector<SObjectiveRow> m_objectives;
    UITaskListWnd* m_parent{};
    float m_min_h{};
    TASK_OBJECTIVE_ID m_hot_row{};   // the step under the cursor, for its hint
    u32 m_hot_since{};

    enum
    {
        stt_activ = 0,
        stt_unread,
        stt_read,
        stt_count
    };
    u32 m_color_states[stt_count];
}; // class UITaskListWndItem

#endif // UI_SECOND_TASK_WND_H_INCLUDED
