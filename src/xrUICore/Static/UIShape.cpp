// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch.hpp"
#include "UIShape.h"

#include "Include/xrRender/UIRender.h"
#include "Include/xrRender/UIShader.h"
#include "xrUICore/XML/xrUIXmlParser.h"

namespace
{
Fvector4 shape_color(u32 argb)
{
    return Fvector4().set(color_get_R(argb) / 255.f, color_get_G(argb) / 255.f, color_get_B(argb) / 255.f,
        color_get_A(argb) / 255.f);
}

SUIShapeStyle::Kind shape_kind(pcstr text, SUIShapeStyle::Kind fallback)
{
    using Kind = SUIShapeStyle::Kind;
    static constexpr std::pair<pcstr, Kind> kinds[] = {{"box", Kind::Box}, {"star", Kind::Star}, {"search", Kind::Search},
        {"check", Kind::Check}, {"cross", Kind::Cross}, {"chevron", Kind::Chevron}, {"gasmask", Kind::GasMask}};
    for (const auto& [name, kind] : kinds)
        if (text && 0 == xr_stricmp(text, name))
            return kind;
    return fallback;
}
} // namespace

u32 UIShape::ParseColor(pcstr text, u32 fallback)
{
    if (!text || text[0] != '#')
        return fallback;
    const size_t digits = xr_strlen(text + 1);
    if (digits != 6 && digits != 8)
        return fallback;
    char* end = nullptr;
    const unsigned long value = strtoul(text + 1, &end, 16);
    if (!end || *end)
        return fallback;
    const u32 rgba = digits == 6 ? static_cast<u32>(value) << 8 | 0xff : static_cast<u32>(value);
    return color_rgba(rgba >> 24, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff, rgba & 0xff);
}

void UIShape::Read(const CUIXml& xml, pcstr path, size_t index, SUIShapeStyle& style)
{
    style.kind = shape_kind(xml.ReadAttrib(path, index, "kind", nullptr), style.kind);
    style.fill = ParseColor(xml.ReadAttrib(path, index, "fill", nullptr), style.fill);
    style.line = ParseColor(xml.ReadAttrib(path, index, "line", nullptr), style.line);
    style.border = xml.ReadAttribFlt(path, index, "border", style.border);
    style.radius = xml.ReadAttribFlt(path, index, "radius", style.radius);
    style.soft = xml.ReadAttribFlt(path, index, "soft", style.soft);
    style.fraction = xml.ReadAttribFlt(path, index, "fraction", style.fraction);
}

void UIShape::Draw(ui_shader& shader, const Frect& rect, const SUIShapeStyle& style)
{
    // the shader draws in screen pixels; a 3D surface (pttLIT) has none
    if (!style.Visible() || UI().m_currentPointType != IUIRender::pttTL)
        return;
    if (!shader->inited())
        shader->create("xms_ui_shape", "ui\\ui_common");

    // Whole pixels: a one-pixel border lands on one row of pixels, not across two.
    const float x0 = std::round(UI().ClientToScreenScaledX(rect.x1));
    const float y0 = std::round(UI().ClientToScreenScaledY(rect.y1));
    const float x1 = std::round(UI().ClientToScreenScaledX(rect.x2));
    const float y1 = std::round(UI().ClientToScreenScaledY(rect.y2));
    if (x1 <= x0 || y1 <= y0)
        return;
    // Thicknesses follow the height alone: a 16:9 layout squeezes its x units, not its lines.
    const float unit = UI().ClientToScreenScaledY(1.f);
    const float border = style.border > 0.f ? std::max(1.f, std::round(style.border * unit)) : 0.f;
    const float radius = std::round(style.radius * unit);
    const float soft = std::max(1.f, style.soft * unit);
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    const bool gasMask = style.kind == SUIShapeStyle::Kind::GasMask;
    // the fade of a soft edge lies outside the rectangle, and so does a gas mask's glow
    const float margin = std::ceil(soft) + 1.f + (gasMask ? border : 0.f);

    GEnv.UIRender->SetShader(*shader);
    GEnv.UIRender->SetShapeParams(Fvector4().set((x1 - x0) * 0.5f, (y1 - y0) * 0.5f, radius, border),
        shape_color(style.line), Fvector4().set(static_cast<float>(style.kind),
            gasMask ? static_cast<float>(style.badge) : style.fraction, soft, gasMask ? style.badgeScale : 0.f));
    GEnv.UIRender->StartPrimitive(6, IUIRender::ptTriList, IUIRender::pttTL);
    // Positions go in half a pixel back, which stub_notransform_t adds again; the texture
    // coordinate is the position relative to the centre, so a pixel's centre reads x + 0.5.
    const auto point = [&](float x, float y) { GEnv.UIRender->PushPoint(x - 0.5f, y - 0.5f, 0.f, style.fill, x - cx, y - cy); };
    const float l = x0 - margin, t = y0 - margin, r = x1 + margin, b = y1 + margin;
    point(l, t);
    point(r, t);
    point(r, b);
    point(l, t);
    point(r, b);
    point(l, b);
    GEnv.UIRender->FlushPrimitive();
}

void CUIShape::Draw()
{
    Frect rect;
    GetAbsoluteRect(rect);
    UIShape::Draw(m_shader, rect, style);
    CUIWindow::Draw();
}

void CUIShapeScrollView::Draw()
{
    CUIScrollView::Draw();

    const float content = m_pad->GetHeight();
    const float visible = GetHeight();
    if (content <= visible + 0.5f)
        return;
    Frect rect;
    GetAbsoluteRect(rect);
    const float kx = UI().ClientToScreenScaledY(1.f) / std::max(UI().ClientToScreenScaledX(1.f), 0.001f);
    const Frect bar = Frect().set(rect.x2 - barWidth * kx, rect.y1, rect.x2, rect.y2);
    UIShape::Draw(m_shader, bar, track);
    const float top = rect.y1 + visible * std::max(0.f, -m_pad->GetWndPos().y) / content;
    const float length = std::max(visible * visible / content, 16.f);
    UIShape::Draw(m_shader, Frect().set(bar.x1, top, bar.x2, std::min(top + length, rect.y2)), thumb);
}

void CUIShapeScrollView::InitLooks(const CUIXml& xml, pcstr path, size_t index)
{
    string512 node{};
    xr_sprintf(node, "%s:track", path);
    UIShape::Read(xml, node, index, track);
    xr_sprintf(node, "%s:thumb", path);
    UIShape::Read(xml, node, index, thumb);
    barWidth = xml.ReadAttribFlt(path, index, "bar_width", barWidth);
    // the stock bar keeps the range and is never drawn
    SetScrollBarHidden(true);
}

CUIShapeButton::CUIShapeButton()
{
    // the looks draw the button; a texture never does
    TextureOff();
}

CUIShapeButton::Look CUIShapeButton::CurrentLook() const
{
    if (!IsEnabled())
        return lookDisabled;
    if (GetButtonState() == BUTTON_PUSHED)
        return lookPushed;
    if (checked)
        return lookChecked;
    return CursorOverWindow() || UI().Focus().GetFocused() == this ? lookHighlight : lookNormal;
}

void CUIShapeButton::Draw()
{
    const Look look = CurrentLook();
    Frect rect;
    GetAbsoluteRect(rect);
    UIShape::Draw(m_shader, rect, looks[look]);
    if (textColors[look] >> 24)
        TextItemControl()->SetTextColor(textColors[look]);

    if (icon != SUIShapeStyle::Kind::Box && iconSize > 0.f)
    {
        // the glyph sits where the caption's indent leaves room, in the caption's colour
        SUIShapeStyle glyph;
        glyph.kind = icon;
        glyph.fill = TextItemControl()->GetTextColor();
        glyph.border = 1.5f;
        // square on screen: layout x units are narrower than y units in a 16:9 layout
        const float kx = UI().ClientToScreenScaledY(1.f) / std::max(UI().ClientToScreenScaledX(1.f), 0.001f);
        const float inset = (rect.height() - iconSize) * 0.5f;
        const float left = rect.x1 + inset * kx;
        const float top = rect.y1 + inset;
        UIShape::Draw(m_shader, Frect().set(left, top, left + iconSize * kx, top + iconSize), glyph);
    }
    CUIButton::Draw();
}

void UIShape::ReadLooks(
    const CUIXml& xml, pcstr path, size_t index, SUIShapeStyle (&looks)[LookCount], u32 (&text)[LookCount], u32 defaultText)
{
    static constexpr pcstr names[LookCount] = {"normal", "highlight", "pushed", "checked", "disabled"};
    string512 node{};
    xr_sprintf(node, "%s:normal", path);
    Read(xml, node, index, looks[0]);
    text[0] = ParseColor(xml.ReadAttrib(node, index, "text", nullptr), defaultText);
    for (size_t look = 1; look != LookCount; ++look)
    {
        looks[look] = looks[0];
        text[look] = text[0];
        xr_sprintf(node, "%s:%s", path, names[look]);
        if (!xml.NavigateToNode(node, index))
            continue;
        Read(xml, node, index, looks[look]);
        text[look] = ParseColor(xml.ReadAttrib(node, index, "text", nullptr), text[look]);
    }
}

void CUIShapeButton::InitLooks(const CUIXml& xml, pcstr path, size_t index)
{
    UIShape::ReadLooks(xml, path, index, looks, textColors, TextItemControl()->GetTextColor());
    icon = shape_kind(xml.ReadAttrib(path, index, "icon", nullptr), SUIShapeStyle::Kind::Box);
    iconSize = xml.ReadAttribFlt(path, index, "icon_size", 0.f);
}
