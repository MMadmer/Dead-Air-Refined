#pragma once
#include "xrUICore/Windows/UIFrameWindow.h"

class CUIStatic;
class CUIFrameLineWnd;

class XRUICORE_API CUIButtonHint final : public CUIFrameWindow
{
    CUIWindow* m_ownerWnd{};

    CUIStatic* m_text;
    CUIFrameLineWnd* m_border{};

    bool m_enabledOnFrame{};
    // The last frame the owner drew itself with the hint.
    u32 m_ownerDrawFrame{};

public:
    CUIButtonHint();

    CUIWindow* Owner() const { return m_ownerWnd; }
    // Free to take: nobody holds it, or the holder has stopped drawing. A window hidden or closed
    // under the cursor never loses focus, so it would never discard the hint and would block every
    // other one.
    bool Vacant() const;
    void Discard() { m_ownerWnd = nullptr; }
    void OnRender();
    void Draw_();
    void SetHintText(CUIWindow* w, LPCSTR text);
    // the text for the owner, beside the cursor and within the screen
    void ShowFor(CUIWindow* owner, pcstr text);

    pcstr GetDebugType() override { return "CUIButtonHint"; }
};

XRUICORE_API extern CUIButtonHint* g_btnHint;
XRUICORE_API extern CUIButtonHint* g_statHint;
