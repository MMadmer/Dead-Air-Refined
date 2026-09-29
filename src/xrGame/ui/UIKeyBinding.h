#pragma once
#include "xrUICore/Windows/UIWindow.h"

#include "xrUICore/Windows/UIFrameWindow.h"
#include "xrUICore/Windows/UIFrameLineWnd.h"
#include "xrUICore/EditBox/UIEditBox.h"

class CUIXml;
class CUIScrollView;
class CUIStatic;

class CUIKeyBinding final : public CUIWindow
{
public:
    CUIKeyBinding();
    void InitFromXml(CUIXml& xml_doc, LPCSTR path);
    void Update() override;
    pcstr GetDebugType() override { return "CUIKeyBinding"; }

protected:
    void FillUpList(CUIXml& xml_doc, LPCSTR path);
    bool IsActionExist(LPCSTR action, CUIXml& xml_doc);

#ifdef DEBUG
    void CheckStructure(CUIXml& xml_doc);
#endif

protected:
    bool m_isGamepadBinds{};

    std::array<CUIFrameLineWnd, 3> m_header;
    CUIFrameWindow m_frame;
    CUIScrollView* m_scroll_wnd{};

    // Action rows with a hint, and whether the rows offer it right now (see Update).
    xr_vector<std::pair<CUIStatic*, shared_str>> m_rowHints;
    bool m_rowHintsOffered{};

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};
