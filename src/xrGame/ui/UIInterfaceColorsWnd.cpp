// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIInterfaceColorsWnd.h"
#include "UIColorPicker.h"
#include "UIHelper.h"
#include "UIXmlInit.h"
#include "UIInventoryUtilities.h"
#include "UIGameCustom.h"
#include "HUDManager.h"
#include "Level.h"
#include "xrUICore/Buttons/UIBtnHint.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_input.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/ComboBox/UIComboBox.h"
#include "xrUICore/Static/UIShape.h"
#include "xrUICore/Static/UIStatic.h"

namespace
{
constexpr pcstr seeds[2] = { "main", "accent" };
constexpr pcstr commands[2] = { "ui_color_main", "ui_color_accent" };
// the preview follows a drag at this pace: a template is a few hundred windows
constexpr u32 PreviewPace = 60;

std::unique_ptr<SUIShapeSkin> swatch(u32 color)
{
    auto skin = std::make_unique<SUIShapeSkin>();
    SUIShapeStyle& s = skin->looks[SUIShapeSkin::Normal];
    s.fill = color | 0xFF000000;
    s.line = color_rgba(232, 230, 224, 200);
    s.border = 1.f;
    s.radius = 2.f;
    for (u8 look = SUIShapeSkin::Highlight; look < SUIShapeSkin::Count; ++look)
        skin->looks[look] = s;
    return skin;
}
} // namespace

CUIInterfaceColorsWnd::CUIInterfaceColorsWnd() : CUIDialogWnd("CUIInterfaceColorsWnd") {}
CUIInterfaceColorsWnd::~CUIInterfaceColorsWnd()
{
    // the game quit over the window: what was tried is not the player's choice (user.ltx is saved after)
    if (m_trying)
        Restore();
}

bool CUIInterfaceColorsWnd::Init()
{
    if (!m_xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_interface_colors.xml", false))
        return false;

    SetWndRect(Frect().set(0.f, 0.f, UI_BASE_WIDTH, UI_BASE_HEIGHT));
    // the shade over the menu, the window's plate, its texts
    for (pcstr node : { "backdrop", "frame", "preview" })
    {
        auto* plate = xr_new<CUIWindow>(node);
        plate->SetAutoDelete(true);
        AttachChild(plate);
        CUIXmlInit::InitWindow(m_xml, node, 0, plate);
        if (!xr_strcmp(node, "preview"))
            m_preview = plate;
    }
    UIHelper::CreateStatic(m_xml, "title", this);
    UIHelper::CreateStatic(m_xml, "subtitle", this, false);
    UIHelper::CreateStatic(m_xml, "template_caption", this, false);

    // the templates, by the drop-down
    m_template = xr_new<CUIComboBox>();
    m_template->SetAutoDelete(true);
    AttachChild(m_template);
    CUIXmlInit::InitComboBox(m_xml, "template", 0, m_template);
    XML_NODE templates = m_xml.NavigateToNode("templates", 0);
    const XML_NODE root = m_xml.GetLocalRoot();
    m_xml.SetLocalRoot(templates);
    for (int i = 0, count = int(m_xml.GetNodesNum(templates, "template")); i < count; ++i)
        m_template->AddItem_(StringTable().translate(m_xml.ReadAttrib("template", i, "caption", "")).c_str(), i);
    m_xml.SetLocalRoot(root);
    m_template->SetItemIDX(0);
    Register(m_template);
    AddCallback(m_template, LIST_ITEM_SELECT, CUIWndCallback::void_function(this, &CUIInterfaceColorsWnd::OnTemplate));

    // main and accent: which one the picker sets, each with its colour
    for (u8 i = 0; i < 2; ++i)
    {
        string64 node;
        xr_sprintf(node, "target_%s", seeds[i]);
        m_targets[i] = UIHelper::Create3tButton(m_xml, node, this);
        m_targets[i]->SetButtonAsSwitch(true);
        Register(m_targets[i]);
        AddCallback(m_targets[i], BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIInterfaceColorsWnd::OnTarget));
        xr_strcat(node, "_swatch");
        m_swatches[i] = xr_new<CUIWindow>(node);
        m_swatches[i]->SetAutoDelete(true);
        AttachChild(m_swatches[i]);
        CUIXmlInit::InitWindow(m_xml, node, 0, m_swatches[i]);
    }

    m_picker = xr_new<CUIColorPicker>();
    m_picker->SetAutoDelete(true);
    AttachChild(m_picker);
    m_picker->InitPicker(m_xml, "picker");
    m_picker->onChange = [this](u32 color) { OnPicked(color); };

    m_reset = UIHelper::Create3tButton(m_xml, "btn_reset", this);
    m_cancel = UIHelper::Create3tButton(m_xml, "btn_cancel", this);
    m_ok = UIHelper::Create3tButton(m_xml, "btn_ok", this);
    for (auto [button, handler] : { std::pair{ m_reset, &CUIInterfaceColorsWnd::OnReset },
             std::pair{ m_cancel, &CUIInterfaceColorsWnd::OnCancel }, std::pair{ m_ok, &CUIInterfaceColorsWnd::OnOk } })
    {
        Register(button);
        AddCallback(button, BUTTON_CLICKED, CUIWndCallback::void_function(this, handler));
    }
    return true;
}

void CUIInterfaceColorsWnd::Show(bool status)
{
    if (status && !IsShown())
    {
        // the colours the window opened with: Cancel puts them back
        for (u8 i = 0; i < 2; ++i)
        {
            UITheme::Seed(seeds[i], m_seed[i]);
            m_before[i] = m_seed[i];
            m_chosenBefore[i] = UITheme::SeedChosen(seeds[i]);
        }
        m_trying = true;
        m_picker->FinishInput(false);
        SelectTarget(0);
        BuildPreview();
    }
    else if (!status && IsShown())
    {
        // closed without Apply, however: the colours it opened with come back
        m_picker->FinishInput(false);
        if (m_trying)
            Restore();
    }
    inherited::Show(status);
}

void CUIInterfaceColorsWnd::Restore()
{
    m_trying = false;
    for (u8 i = 0; i < 2; ++i)
    {
        if (m_chosenBefore[i])
            UITheme::SetSeed(seeds[i], m_before[i]);
        else
            UITheme::ResetSeed(seeds[i]);
    }
}

void CUIInterfaceColorsWnd::SelectTarget(u8 target)
{
    m_target = target;
    m_picker->SetOldColor(m_before[target]);
    m_picker->SetColor(m_seed[target]);
}

void CUIInterfaceColorsWnd::OnPicked(u32 color)
{
    m_seed[m_target] = color;
    // the theme is read again with it: the template shows it, the game's windows keep theirs until Apply
    UITheme::SetSeed(seeds[m_target], color);
    m_previewDirty = true;
}

void CUIInterfaceColorsWnd::Update()
{
    inherited::Update();
    for (u8 i = 0; i < 2; ++i)
    {
        if (!m_targets[i]->CursorOverWindow())
            m_targets[i]->SetButtonState(i == m_target ? CUIButton::BUTTON_PUSHED : CUIButton::BUTTON_NORMAL);
        m_swatches[i]->SetShapeSkin(swatch(m_seed[i]));
    }
    if (m_previewDirty && Device.dwTimeContinual - m_previewBuiltAt >= PreviewPace)
        BuildPreview();
}

void CUIInterfaceColorsWnd::BuildPreview()
{
    m_previewDirty = false;
    m_previewBuiltAt = Device.dwTimeContinual;
    if (m_shown)
    {
        m_preview->DetachChild(m_shown);
        m_shown = nullptr;
    }

    const int index = std::max(int(m_template->GetSelectedIDX()), 0);
    const XML_NODE root = m_xml.GetLocalRoot();
    m_xml.SetLocalRoot(m_xml.NavigateToNode("templates", 0));
    auto* tpl = xr_new<CUIWindow>("template");
    tpl->SetAutoDelete(true);
    CUIXmlInit::InitWindow(m_xml, "template", index, tpl);

    // the items: the icon of the section's own, its cells' plate under it and its number over it
    const float kx = UI().get_current_kx();
    m_xml.SetLocalRoot(m_xml.NavigateToNode("template", index));
    for (int i = 0, count = int(m_xml.GetNodesNum(m_xml.GetLocalRoot(), "item")); i < count; ++i)
    {
        const shared_str section = m_xml.ReadAttrib("item", i, "section", "");
        if (!section.size() || !pSettings->section_exist(section) || !pSettings->line_exist(section, "inv_grid_x"))
            continue;
        const float cell = m_xml.ReadAttribFlt("item", i, "cell", 41.f);
        const float gw = pSettings->r_float(section, "inv_grid_width"), gh = pSettings->r_float(section, "inv_grid_height");
        const float gx = pSettings->r_float(section, "inv_grid_x"), gy = pSettings->r_float(section, "inv_grid_y");
        const bool turn = m_xml.ReadAttribInt("item", i, "turn", 0) != 0;
        const float fitW = m_xml.ReadAttribFlt("item", i, "fit_w", 0.f), fitH = m_xml.ReadAttribFlt("item", i, "fit_h", 0.f);

        // the footprint: turned upright as a slot shows a long one, shrunk into fit_w x fit_h and centred there
        float fw = (turn ? gh : gw) * cell * kx, fh = (turn ? gw : gh) * cell;
        float scale = 1.f;
        if (fitW > 0.f && fitH > 0.f)
            scale = std::min({ 1.f, fitW / fw, fitH / fh });
        fw *= scale;
        fh *= scale;
        Frect r;
        r.x1 = m_xml.ReadAttribFlt("item", i, "x", 0.f) + (fitW > 0.f ? (fitW - fw) / 2.f : 0.f);
        r.y1 = m_xml.ReadAttribFlt("item", i, "y", 0.f) + (fitH > 0.f ? (fitH - fh) / 2.f : 0.f);
        r.x2 = r.x1 + fw;
        r.y2 = r.y1 + fh;

        if (const SUIShapeSkin* plate = UITheme::Skin(m_xml.ReadAttrib("item", i, "plate", nullptr)))
        {
            auto* under = xr_new<CUIWindow>("plate");
            under->SetAutoDelete(true);
            under->SetWndRect(r);
            under->SetShapeSkin(std::make_unique<SUIShapeSkin>(*plate));
            tpl->AttachChild(under);
        }
        auto* icon = xr_new<CUIStatic>("icon");
        icon->SetAutoDelete(true);
        icon->SetShader(InventoryUtilities::GetItemIconsShader(section));
        icon->SetTextureRect(Frect().set(gx * 50.f, gy * 50.f, (gx + gw) * 50.f, (gy + gh) * 50.f));
        icon->SetStretchTexture(true);
        if (turn)
        {
            // a turned icon is laid out unturned in the screen's proportions (the turn applies kx) around the centre
            Fvector2 centre;
            r.getcenter(centre);
            const float w = gw * cell * scale, h = gh * cell * scale;
            icon->SetWndRect(Frect().set(centre.x - w / 2.f, centre.y - h / 2.f, centre.x + w / 2.f, centre.y + h / 2.f));
            icon->EnableHeading(true);
            icon->SetHeading(PI_DIV_2);
        }
        else
            icon->SetWndRect(r);
        tpl->AttachChild(icon);

        if (cpcstr count_text = m_xml.ReadAttrib("item", i, "count", nullptr))
        {
            auto* badge = xr_new<CUIStatic>("count");
            badge->SetAutoDelete(true);
            badge->SetWndRect(Frect().set(r.x1 + 2.f * kx, r.y2 - 15.f, r.x1 + 26.f * kx, r.y2 - 2.f));
            if (const SUIShapeSkin* look = UITheme::Skin("badge"))
                badge->SetShapeSkin(std::make_unique<SUIShapeSkin>(*look));
            badge->SetFont(UI().Font().pFontLetterica16Russian);
            badge->SetTextAlignment(CGameFont::alCenter);
            badge->SetVTextAlignment(valCenter);
            u32 text = 0xFFFFFFFF;
            UITheme::Color("text", text);
            badge->SetTextColor(text);
            badge->SetText(count_text);
            tpl->AttachChild(badge);
        }
    }
    // what floats over the items: hints, menus
    if (m_xml.NavigateToNode("over", 0))
    {
        auto* over = xr_new<CUIWindow>("over");
        over->SetAutoDelete(true);
        CUIXmlInit::InitWindow(m_xml, "over", 0, over);
        tpl->AttachChild(over);
    }
    m_xml.SetLocalRoot(root);

    // inside the preview's frame
    constexpr float inset = 6.f;
    tpl->SetWndPos({ inset * kx, inset });
    m_preview->AttachChild(tpl);
    m_shown = tpl;
}

void CUIInterfaceColorsWnd::OnTemplate(CUIWindow*, void*) { BuildPreview(); }

void CUIInterfaceColorsWnd::OnTarget(CUIWindow* window, void*)
{
    for (u8 i = 0; i < 2; ++i)
    {
        if (window == m_targets[i])
            SelectTarget(i);
    }
}

void CUIInterfaceColorsWnd::OnReset(CUIWindow*, void*)
{
    // the theme's own colours, the template with them
    for (u8 i = 0; i < 2; ++i)
    {
        UITheme::ResetSeed(seeds[i]);
        UITheme::DefaultSeed(seeds[i], m_seed[i]);
    }
    m_picker->SetColor(m_seed[m_target]);
    BuildPreview();
}

void CUIInterfaceColorsWnd::Close(bool apply)
{
    CUIComboBox::CloseOpenList();
    // a value being typed counts when applied
    m_picker->FinishInput(apply);
    if (apply)
    {
        m_trying = false;
        for (u8 i = 0; i < 2; ++i)
        {
            // kept in user.ltx; a seed set back to the theme's own is "default", so a theme's update reaches it
            u32 own = 0;
            UITheme::DefaultSeed(seeds[i], own);
            string64 command;
            if ((own & 0x00FFFFFF) == (m_seed[i] & 0x00FFFFFF))
                xr_sprintf(command, "%s default", commands[i]);
            else
                xr_sprintf(command, "%s #%02X%02X%02X", commands[i], color_get_R(m_seed[i]), color_get_G(m_seed[i]),
                    color_get_B(m_seed[i]));
            Console->Execute(command);
        }
    }
    if (m_shown)
    {
        m_preview->DetachChild(m_shown);
        m_shown = nullptr;
    }
    HideDialog();

    if (apply)
    {
        Console->Execute("cfg_save");
        // the hints keep the look they were made with
        if (g_btnHint)
        {
            xr_delete(g_btnHint);
            xr_delete(g_statHint);
            g_btnHint = xr_new<CUIButtonHint>();
            g_statHint = xr_new<CUIButtonHint>();
        }
        // the game's windows over a level are made again in the new colours, the talk window too (built once)
        if (g_pGameLevel && Level().pHUD)
        {
            HUD().OnUIReset();
            if (CUIGameCustom* game = CurrentGameUI())
                game->ReinitDialogs();
        }
    }
}

void CUIInterfaceColorsWnd::OnCancel(CUIWindow*, void*) { Close(false); }
void CUIInterfaceColorsWnd::OnOk(CUIWindow*, void*) { Close(true); }

void CUIInterfaceColorsWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIInterfaceColorsWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    // a field or an open drop-down takes the keys first, Escape included
    if (inherited::OnKeyboardAction(dik, keyboardAction))
        return true;
    if (keyboardAction == WINDOW_KEY_PRESSED && (IsBinded(kQUIT, dik) || IsBinded(kUI_BACK, dik, EKeyContext::UI)))
    {
        Close(false);
        return true;
    }
    return false;
}
