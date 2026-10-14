#pragma once

class CUIWeightLoad;
class UIHint;

class CUIWeightBar final : public CUIWindow
{
    CUIStatic* m_BottomInfo{};
    CUIStatic* m_Weight{};
    CUIStatic* m_WeightMax{};
    // the player's load against the limits of his movement (<path>_weight_bar, optional)
    CUIWeightLoad* m_Load{};
    float m_Weight_end_x{};

public:
    CUIStatic* m_BagWnd{};
    CUIStatic* m_BagWnd2{};

public:
    CUIWeightBar() : CUIWindow("CUIWeightBar") {}
    void init_from_xml(CUIXml& uiXml, pcstr path);
    // the hint of the load bar
    void SetHintWnd(UIHint* hint);
    void UpdateData(float weight);
    void UpdateData(CInventoryOwner* pInvOwner);
    pcstr GetDebugType() override { return "CUIWeightBar"; }
};
