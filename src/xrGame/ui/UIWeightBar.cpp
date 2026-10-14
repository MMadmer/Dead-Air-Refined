#include "StdAfx.h"
#include "UIWeightBar.h"
#include "UIHelper.h"
#include "UIInventoryUtilities.h"
#include "UIXmlInit.h"
#include "Actor.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "xrUICore/Hint/UIHint.h"
#include "xrUICore/Static/UIShape.h"
#include "xrUICore/XML/xrUIXmlParser.h"

// The player's load drawn against the limits of Dead Air's movement: up to the weight he carries
// without strain (MaxCarryWeight), then the overload up to the weight he still walks under at full
// pace (MaxWalkWeight), where the bar ends - a full bar is the full overload, the slowdown past it.
// The overload's stretch of the track is tinted with its colour, a mark stands at its start, and the
// load takes the colour of its zone.
class CUIWeightLoad final : public UIHintWindow
{
public:
    bool Init(CUIXml& xml, pcstr path)
    {
        if (!xml.NavigateToNode(path, 0))
            return false;
        CUIXmlInit::InitWindow(xml, path, 0, this);
        set_hint_delay(xml.ReadAttribInt(path, 0, "hint_delay", 600));
        string256 node;
        const auto look = [&](pcstr child, SUIShapeStyle& style, pcstr fallback)
        {
            strconcat(node, path, ":", child);
            if (const auto skin = UIShape::ReadSkin(xml, node, 0))
                style = skin->looks[SUIShapeSkin::Normal];
            else if (const SUIShapeSkin* theme = UITheme::Skin(fallback))
                style = theme->looks[SUIShapeSkin::Normal];
        };
        look("track", m_track, "load_track");
        look("fill", m_fill, "load_fill");
        look("mark", m_mark, "load_mark");
        constexpr pcstr zones[] = { "normal", "overload", "slow" };
        constexpr pcstr fallbacks[] = { "$good", "$warn", "$bad" };
        for (u32 i = 0; i < 3; ++i)
        {
            strconcat(node, path, ":", zones[i]);
            m_zone[i] = UIShape::ParseColor(xml.ReadAttrib(node, 0, "color", fallbacks[i]), 0xFFFFFFFF);
        }
        m_tint = xml.ReadAttribFlt(path, 0, "zone_tint", 0.22f);
        return true;
    }

    void SetLoad(float weight, float carry, float walk)
    {
        m_weight = weight;
        // the overload cannot start past the end of the bar, whatever bonuses a mod gives
        m_walk = std::max(walk, 0.001f);
        m_carry = std::min(carry, m_walk);
        if (!fsimilar(carry, m_hint_carry) || !fsimilar(walk, m_hint_walk) || !fsimilar(weight, m_hint_weight))
        {
            m_hint_carry = carry;
            m_hint_walk = walk;
            m_hint_weight = weight;
            string512 text;
            xr_sprintf(text, StringTable().translate("st_da_weight_hint").c_str(), weight, m_carry, m_walk);
            set_hint_text(text);
        }
    }

    void Draw() override
    {
        Frect r;
        GetAbsoluteRect(r);
        const auto at = [&](float kg) { return r.x1 + r.width() * clampr(kg / m_walk, 0.0f, 1.0f); };

        UIShape::Draw(r, m_track);
        // the overload's stretch
        if (r.x2 > at(m_carry))
        {
            SUIShapeStyle zone = m_track;
            zone.fill = (m_zone[1] & 0x00FFFFFF) | (u32(clampr(m_tint, 0.0f, 1.0f) * 255.0f) << 24);
            zone.radius = 0.0f;
            UIShape::Draw(Frect().set(at(m_carry), r.y1, r.x2, r.y2), zone);
        }

        SUIShapeStyle fill = m_fill;
        fill.fill = m_zone[m_weight >= m_walk ? 2 : (m_weight > m_carry ? 1 : 0)];
        if (m_weight > 0.0f)
            UIShape::Draw(Frect().set(r.x1, r.y1, at(m_weight), r.y2), fill);

        // where the overload starts, a little taller than the bar
        if (m_carry < m_walk)
        {
            const float half = 0.75f * UI().get_current_kx();
            const float x = at(m_carry);
            UIShape::Draw(Frect().set(x - half, r.y1 - 2.0f, x + half, r.y2 + 2.0f), m_mark);
        }
    }

    pcstr GetDebugType() override { return "CUIWeightLoad"; }

private:
    SUIShapeStyle m_track, m_fill, m_mark;
    u32 m_zone[3]{};
    float m_tint{ 0.22f };
    float m_weight{}, m_carry{}, m_walk{ 1.0f };
    float m_hint_weight{ -1.0f }, m_hint_carry{ -1.0f }, m_hint_walk{ -1.0f };
};

void CUIWeightBar::init_from_xml(CUIXml& uiXml, pcstr path)
{
    string64 buf;

    xr_sprintf(buf, "%s_weight_caption", path);
    m_BottomInfo = UIHelper::CreateStatic(uiXml, buf, this);

    xr_sprintf(buf, "%s_weight", path);
    m_Weight = UIHelper::CreateStatic(uiXml, buf, this);

    xr_sprintf(buf, "%s_weight_max", path);
    m_WeightMax = UIHelper::CreateStatic(uiXml, buf, this, false);

    xr_sprintf(buf, "%s_weight_bar", path);
    m_Load = xr_new<CUIWeightLoad>();
    if (m_Load->Init(uiXml, buf))
    {
        m_Load->SetAutoDelete(true);
        AttachChild(m_Load);
    }
    else
        xr_delete(m_Load);
    {
        const auto* weightLabel = m_WeightMax ? m_WeightMax : m_Weight;
        m_Weight_end_x = weightLabel->GetWndPos().x;
    }
    m_BottomInfo->AdjustWidthToText();
}

void CUIWeightBar::SetHintWnd(UIHint* hint)
{
    if (m_Load)
        m_Load->set_hint_wnd(hint);
}

void CUIWeightBar::UpdateData(float weight)
{
    // a weight without a limit (a body, a box, a trader's goods) has no load to show
    if (m_Load)
        m_Load->Show(false);
    if (!m_Weight || !m_BottomInfo)
        return;

    LPCSTR kg_str = StringTable().translate("st_kg").c_str();

    string64 buf;
    xr_sprintf(buf, "%.1f %s", weight, kg_str);
    m_Weight->SetText(buf);
    m_Weight->AdjustWidthToText();
    m_BottomInfo->AdjustWidthToText();

    Fvector2 pos = m_Weight->GetWndPos();
    pos.x = m_Weight_end_x - m_Weight->GetWndSize().x - 5.0f;
    m_Weight->SetWndPos(pos);
    pos.x = pos.x - m_BottomInfo->GetWndSize().x - 5.0f;
    m_BottomInfo->SetWndPos(pos);
}

void CUIWeightBar::UpdateData(CInventoryOwner* pInvOwner)
{
    if (m_BagWnd)
        InventoryUtilities::UpdateWeight(*m_BagWnd, pInvOwner, true);

    if (m_BagWnd2)
        InventoryUtilities::UpdateWeight(*m_BagWnd2, pInvOwner, false);

    if (m_Load)
    {
        // the player alone has limits to his movement
        const CActor* actor = smart_cast<CActor*>(pInvOwner);
        m_Load->Show(actor != nullptr);
        if (actor)
            m_Load->SetLoad(pInvOwner->inventory().CalcTotalWeight(), actor->MaxCarryWeight(), actor->MaxWalkWeight());
    }

    if (!m_Weight || !m_WeightMax || !m_BottomInfo)
        return;
    InventoryUtilities::UpdateWeightStr(*m_Weight, *m_WeightMax, pInvOwner);

    m_Weight->AdjustWidthToText();
    m_WeightMax->AdjustWidthToText();
    m_BottomInfo->AdjustWidthToText();

    Fvector2 pos = m_Weight->GetWndPos();
    pos.x = m_WeightMax->GetWndPos().x - m_Weight->GetWndSize().x - 5.0f;
    m_Weight->SetWndPos(pos);
    pos.x = pos.x - m_BottomInfo->GetWndSize().x - 5.0f;
    m_BottomInfo->SetWndPos(pos);
}
