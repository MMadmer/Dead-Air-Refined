// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Windows/UIWindow.h"
#include "xrUICore/Callbacks/UIWndCallback.h"
#include "xrUICore/Static/UIShapeStyle.h"

class CInventoryItem;
class CUIXml;
class CUIScrollView;
class CUIStatic;
class CUI3tButton;
class CGameFont;

// Items of one kind compared with each other, the winner decided by weights rather than by a
// count of won rows. What a kind is compared on, how much each parameter counts to keep the item
// and to sell it, and how much each part of a weapon matters is data:
// configs\dead_air_x64_item_compare.ltx, then every configs\item_compare_*.ltx a mod ships, in
// name order (a line replaces the line of that name; "remove" takes it out).
namespace item_compare
{
enum class EKind : u8
{
    None,
    Firearm,
    Melee,
    Outfit,
    Helmet,
    Backpack,
    Artefact,
    Ammo,
    Grenade,
    Addon,
    Detector,
    Food,
    Medicine,
    Misc,
    Count
};

enum EProfile : u8
{
    Use, // the one to keep
    Sale, // the one to sell
    ProfileCount
};

struct SParam
{
    shared_str id;
    shared_str caption; // a string id
    // an engine source (condition, damage, prot_burn, ...), key:<line of the item's section>, or
    // script:<module>.<function> called with the item's game object
    shared_str source;
    float weight[ProfileCount]{};
    float scale{ 1.0f }; // what is shown is value * scale
    shared_str format; // num, int, pct, kg, money, sec, mps, deg, yes, faults
    bool higher{ true }; // a bigger value is better
};

struct SCell
{
    float value{}; // what the score reads
    float shown{}; // what the row shows: a fault count where the score reads the penalty
    float norm{}; // 0..1 of the best
    bool known{};
};

struct SResult
{
    EKind kind{ EKind::None };
    EProfile profile{ Use };
    xr_vector<CInventoryItem*> items;
    xr_vector<const SParam*> rows; // the parameters some item has
    xr_vector<xr_vector<SCell>> cells; // [row][item]
    xr_vector<float> score; // [item]: 0..100
    float weight_sum{};
    int winner{ -1 };
    // the rows the winner gained most on, by weight, best first
    xr_vector<const SParam*> reasons;
};

using price_function = std::function<u32(CInventoryItem&)>;

EKind kind_of(CInventoryItem& item);
// the string id of a kind's name
pcstr kind_caption(EKind kind);
// the most items one comparison takes
u32 max_items();
SResult evaluate(const xr_vector<CInventoryItem*>& items, EProfile profile, const price_function& price);
xr_string format_value(const SParam& param, float shown);
// read the data again on the next use
void reset();
} // namespace item_compare

// The comparison over the actor menu: a column per item, a row per parameter, the best and the
// worst of each row coloured, the weighted score with the winner on top, and every item against
// every other. Its place, size and looks are the compare_window node of the actor_menu layout,
// or of actor_menu_compare.xml when the layout has none.
class CUIItemCompareWnd final : public CUIWindow, public CUIWndCallback
{
    using inherited = CUIWindow;

public:
    CUIItemCompareWnd();
    ~CUIItemCompareWnd() override;

    bool Init(CUIXml& actor_menu_xml);
    void Open(const xr_vector<CInventoryItem*>& items, item_compare::price_function price);
    void Close();

    bool OnMouseAction(float x, float y, EUIMessages mouse_action) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override;
    void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = nullptr) override;
    void Draw() override;
    void Update() override;

    pcstr GetDebugType() override { return "CUIItemCompareWnd"; }

private:
    struct SText
    {
        CGameFont* font{};
        u32 color{ 0xFFFFFFFF };
    };

    bool InitFrom(CUIXml& xml);
    void ReadText(CUIXml& xml, pcstr node, SText& text);
    void SetProfile(item_compare::EProfile profile);
    void OnTab(CUIWindow* w, void*);
    void OnClose(CUIWindow*, void*);
    void Rebuild();
    CUIStatic* AddText(CUIWindow* parent, pcstr text, const Frect& rect, const SText& look, u32 align, bool complex = false);
    CUIStatic* AddShape(CUIWindow* parent, const Frect& rect, const SUIShapeSkin* skin);
    CUIStatic* AddIcon(CUIWindow* parent, CInventoryItem& item, const Frect& box);

    xr_vector<u16> m_ids;
    item_compare::price_function m_price;
    item_compare::EProfile m_profile{ item_compare::Use };

    CUIStatic* m_title{};
    CUI3tButton* m_close{};
    CUI3tButton* m_tabs[item_compare::ProfileCount]{};
    CUIWindow* m_header{};
    CUIScrollView* m_list{};
    CUIStatic* m_footer{};

    SUIShapeStyle m_shade;
    std::unique_ptr<SUIShapeSkin> m_column_skin, m_winner_skin, m_row_skin, m_bar_skin, m_track_skin, m_badge_skin,
        m_rule_skin;
    SText m_name, m_score, m_label, m_weight, m_value, m_section;
    u32 m_best_color{ 0xFF86B36B };
    u32 m_worst_color{ 0xFFD8574B };
    float m_row_height{ 19.0f };
    float m_label_width{ 190.0f };
    float m_column_width{ 130.0f };
    float m_icon_height{ 44.0f };
    float m_padding{ 6.0f };
};
