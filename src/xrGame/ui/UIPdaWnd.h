#pragma once
#include "UIDialogWnd.h"
#include "encyclopedia_article_defs.h"

class CInventoryOwner;
class CUIFrameLineWnd;
class CUI3tButton;
class CUITabControl;
class CUIStatic;
class CUIXml;
class CUIFrameWindow;
class UIHint;

class CUIMapWnd;
class CUITaskWnd;
class CUIFactionWarWnd;
class CUIActorInfoWnd;
class CUIRankingWnd;
class CUILogsWnd;
class CUIAnimatedStatic;
class UIHint;

class CUIPdaWnd final : public CUIDialogWnd
{
    typedef CUIDialogWnd inherited;

protected:
    CUITabControl* UITabControl;
    CUI3tButton* m_btn_close;

    CUIStatic* UIMainPdaFrame;
    CUIStatic* UINoice;

    CUIStatic* m_caption;
    shared_str m_caption_const;
    CUIStatic* m_clock;

    // Текущий активный диалог
    CUIWindow* m_pActiveDialog;
    shared_str m_sActiveSection;

    UIHint* m_hint_wnd;

public:
    // Поддиалоги PDA
    CUIMapWnd* pUIMapWnd;
    CUITaskWnd* pUITaskWnd;
    CUIFactionWarWnd* pUIFactionWarWnd;
    CUIActorInfoWnd* pUIActorInfo;
    CUIRankingWnd* pUIRankingWnd;
    CUILogsWnd* pUILogsWnd;

    virtual void Reset();

public:
    CUIPdaWnd();
    virtual ~CUIPdaWnd();

    virtual void Init();

    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = NULL);

    virtual void Draw();
    virtual void Update();
    virtual void Show(bool status);
    // 3D PDA: with the presenter up these ARE the toggle. The P key's Lua handler calls
    // ShowDialog straight on this window, which is already shown render-only in that
    // state - without the override the call was a silent no-op and the key went dead.
    void ShowDialog(bool bDoHideIndicators) override;
    void HideDialog() override;
    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action)
    {
        CUIDialogWnd::OnMouseAction(x, y, mouse_action);
        return true;
    } // always true because StopAnyMove() == false
    virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action);
    bool OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action) override;

    UIHint* get_hint_wnd() const { return m_hint_wnd; }
    void DrawHint();

    void SetActiveCaption();
    void SetCaption(pcstr text);
    void Show_SecondTaskWnd(bool status);
    void Show_MapWnd(bool status);
    void Show_ContactsWnd(bool status);

    void SetActiveDialog(CUIWindow* wnd) { m_pActiveDialog = wnd; }
    CUIWindow* GetActiveDialog() const { return m_pActiveDialog; }
    pcstr GetActiveSection() const { return m_sActiveSection.c_str(); }
    void SetActiveSubdialog(const shared_str& section);
    CUITabControl* GetTabControl() const { return UITabControl; }

    bool StopAnyMove() override { return false; }
    bool NeedCursor() const override;
    void UpdatePda();
    void UpdateRankingWnd();

    // 3D PDA. The RT pass raises the in-pass flag, draws this dialog into $user$ui and
    // stamps the frame; the ordinary fullscreen Draw() then suppresses itself for the rest
    // of the same frame. The guard means "rasterized to RT THIS frame", never "3D enabled" -
    // a frame without the RT pass falls back to the plain 2D dialog by construction.
    void MarkRasterizedToRT();
    void SetInRTPass(bool b) { m_in_rt_pass = b; }
    bool RasterizedToRT() const;
    // The device face sub-rect inside the UI canvas, in 0..1 UV of $user$ui - the screen
    // shader maps its mesh UVs through this, so layout stays data (pda.xml vs pda_16.xml,
    // modded layouts) instead of being baked into the model.
    bool GetScreenRectUV(Fvector4& uv) const;

private:
    u32 m_rt_frame = u32(-1);
    bool m_in_rt_pass = false;

public:

    pcstr GetDebugType() override { return "CUIPdaWnd"; }
};
