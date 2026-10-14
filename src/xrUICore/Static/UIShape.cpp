// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch.hpp"
#include "UIShape.h"

#include "Include/xrRender/UIRender.h"
#include "Include/xrRender/UIShader.h"
#include "xrUICore/XML/xrUIXmlParser.h"

namespace
{
using Kind = SUIShapeStyle::Kind;

constexpr std::pair<pcstr, Kind> kinds[] = {{"box", Kind::Box}, {"star", Kind::Star}, {"search", Kind::Search},
    {"check", Kind::Check}, {"cross", Kind::Cross}, {"chevron", Kind::Chevron}, {"gasmask", Kind::GasMask},
    {"trefoil", Kind::Trefoil}, {"grid", Kind::Grid}, {"pistol", Kind::Pistol}, {"helmet", Kind::Helmet},
    {"cutlery", Kind::Cutlery}, {"medkit", Kind::Medkit}, {"gem", Kind::Gem}, {"gear", Kind::Gear},
    {"sort", Kind::Sort}, {"biohazard", Kind::Biohazard}, {"bolt", Kind::Bolt}, {"flame", Kind::Flame},
    {"psi", Kind::Psi}, {"claw", Kind::Claw}, {"bullet", Kind::Bullet}, {"impact", Kind::Impact},
    {"burst", Kind::Burst}, {"heart", Kind::Heart}, {"drop", Kind::Drop}, {"runner", Kind::Runner},
    {"weight", Kind::Weight}, {"clock", Kind::Clock}, {"wrench", Kind::Wrench}, {"crosshair", Kind::Crosshair},
    {"hand", Kind::Hand}, {"gauge", Kind::Gauge}, {"shield", Kind::Shield}, {"cartridge", Kind::Cartridge},
    {"moon", Kind::Moon}, {"upgrade", Kind::Upgrade}, {"pin", Kind::Pin}, {"keyboard", Kind::Keyboard}, {"scales", Kind::Scales}};
static_assert(std::size(kinds) == size_t(Kind::Count), "every kind has a name");

Fvector4 shape_color(u32 argb)
{
    return Fvector4().set(color_get_R(argb) / 255.f, color_get_G(argb) / 255.f, color_get_B(argb) / 255.f,
        color_get_A(argb) / 255.f);
}

// The attributes of one node, and which of them it gives.
enum Field : u16
{
    fKind = 1 << 0,
    fFill = 1 << 1,
    fLine = 1 << 2,
    fBorder = 1 << 3,
    fRadius = 1 << 4,
    fSoft = 1 << 5,
    fFraction = 1 << 6,
    fSegment = 1 << 7,
    fGap = 1 << 8,
    fText = SUIShapeSkin::TextSet,
};

struct SPartial
{
    SUIShapeStyle style;
    u32 text{};
    u16 mask{};
};

SPartial read_partial(const CUIXml& xml, pcstr path, size_t index)
{
    SPartial p;
    if (cpcstr v = xml.ReadAttrib(path, index, "kind", nullptr))
    {
        p.style.kind = UIShape::ParseKind(v, Kind::Box);
        p.mask |= fKind;
    }
    const auto color = [&](pcstr name, u32& out, Field field)
    {
        if (cpcstr v = xml.ReadAttrib(path, index, name, nullptr))
        {
            out = UIShape::ParseColor(v, out);
            p.mask |= field;
        }
    };
    color("fill", p.style.fill, fFill);
    color("line", p.style.line, fLine);
    color("text", p.text, fText);
    const auto number = [&](pcstr name, float& out, Field field)
    {
        if (xml.ReadAttrib(path, index, name, nullptr))
        {
            out = xml.ReadAttribFlt(path, index, name, out);
            p.mask |= field;
        }
    };
    number("border", p.style.border, fBorder);
    number("radius", p.style.radius, fRadius);
    number("soft", p.style.soft, fSoft);
    number("fraction", p.style.fraction, fFraction);
    number("segment", p.style.segment, fSegment);
    number("gap", p.style.gap, fGap);
    return p;
}

void apply(SUIShapeStyle& style, u32& text, const SPartial& p, u16 mask)
{
    mask &= p.mask;
    if (mask & fKind)
        style.kind = p.style.kind;
    if (mask & fFill)
        style.fill = p.style.fill;
    if (mask & fLine)
        style.line = p.style.line;
    if (mask & fBorder)
        style.border = p.style.border;
    if (mask & fRadius)
        style.radius = p.style.radius;
    if (mask & fSoft)
        style.soft = p.style.soft;
    if (mask & fFraction)
        style.fraction = p.style.fraction;
    if (mask & fSegment)
        style.segment = p.style.segment;
    if (mask & fGap)
        style.gap = p.style.gap;
    if (mask & fText)
        text = p.text;
}

constexpr pcstr state_names[SUIShapeSkin::Count] = {"normal", "highlight", "pushed", "checked", "disabled"};

// =================================================================================================

struct STheme
{
    xr_map<shared_str, SUIShapeSkin> skins;
    xr_map<shared_str, u32> colors;
    bool loaded{};
};

STheme& theme()
{
    static STheme instance;
    return instance;
}

void load_theme_file(CUIXml& xml)
{
    STheme& t = theme();
    // Colours first: the styles of the same file name them.
    for (size_t i = 0, count = xml.GetNodesNum(xml.GetRoot(), "color"); i < count; ++i)
    {
        const shared_str name = xml.ReadAttrib("color", i, "name", "");
        if (name.size())
            t.colors[name] = UIShape::ParseColor(xml.ReadAttrib("color", i, "value", nullptr), 0xFFFFFFFF);
    }
    for (size_t i = 0, count = xml.GetNodesNum(xml.GetRoot(), "style"); i < count; ++i)
    {
        const shared_str name = xml.ReadAttrib("style", i, "name", "");
        if (!name.size())
            continue;
        SUIShapeSkin skin;
        if (cpcstr base = xml.ReadAttrib("style", i, "base", nullptr))
        {
            if (const auto it = t.skins.find(base); it != t.skins.end())
                skin = it->second;
            else
                Msg("! [ui] theme style [%s] in [%s] names an unknown base [%s]", name.c_str(), xml.m_xml_file_name, base);
        }
        UIShape::ReadSkinNode(xml, "style", i, skin);
        t.skins[name] = skin;
    }
}

void load_theme()
{
    STheme& t = theme();
    t.loaded = true;
    t.skins.clear();
    t.colors.clear();

    CUIXml xml;
    if (xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_theme.xml", false))
        load_theme_file(xml);

    // A mod's ui_theme_<name>.xml comes after, in name order: it replaces what it names.
    FS_FileSet files;
    string_path mask;
    FS.file_list(files, "$game_config$", FS_ListFiles,
        strconcat(sizeof(mask), mask, UI_PATH, DELIMITER "ui_theme_*.xml"));
    for (const auto& file : files)
    {
        string_path name, ext;
        _splitpath(file.name.c_str(), nullptr, nullptr, name, ext);
        xr_strcat(name, ext);
        CUIXml modXml;
        if (modXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, name, false))
            load_theme_file(modXml);
    }
}

STheme& loaded_theme()
{
    if (!theme().loaded)
        load_theme();
    return theme();
}
} // namespace

const SUIShapeSkin* UITheme::Skin(pcstr name)
{
    if (!name || !name[0])
        return nullptr;
    STheme& t = loaded_theme();
    const auto it = t.skins.find(name);
    return it != t.skins.end() ? &it->second : nullptr;
}

bool UITheme::Color(pcstr name, u32& color)
{
    if (!name || !name[0])
        return false;
    STheme& t = loaded_theme();
    const auto it = t.colors.find(name);
    if (it == t.colors.end())
        return false;
    color = it->second;
    return true;
}

void UITheme::Reset() { theme().loaded = false; }

// =================================================================================================

bool UIShape::FindKind(pcstr text, SUIShapeStyle::Kind& kind)
{
    for (const auto& [name, k] : kinds)
    {
        if (text && 0 == xr_stricmp(text, name))
        {
            kind = k;
            return true;
        }
    }
    return false;
}

SUIShapeStyle::Kind UIShape::ParseKind(pcstr text, SUIShapeStyle::Kind fallback)
{
    SUIShapeStyle::Kind kind;
    if (FindKind(text, kind))
        return kind;
    if (text)
        Msg("! [ui] unknown shape kind [%s]", text);
    return fallback;
}

u32 UIShape::ParseColor(pcstr text, u32 fallback)
{
    if (!text)
        return fallback;
    if (text[0] == '$')
    {
        u32 color;
        if (UITheme::Color(text + 1, color))
            return color;
        Msg("! [ui] unknown theme colour [%s]", text);
        return fallback;
    }
    if (text[0] != '#')
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
    const SPartial p = read_partial(xml, path, index);
    u32 text{};
    apply(style, text, p, u16(-1));
}

void UIShape::ReadSkinNode(const CUIXml& xml, pcstr path, size_t index, SUIShapeSkin& skin)
{
    // The node's own attributes are normal's, and every state's that does not set them itself.
    const SPartial own = read_partial(xml, path, index);
    apply(skin.looks[SUIShapeSkin::Normal], skin.text[SUIShapeSkin::Normal], own, u16(-1));
    skin.set[SUIShapeSkin::Normal] |= own.mask;
    for (u8 look = SUIShapeSkin::Highlight; look < SUIShapeSkin::Count; ++look)
        apply(skin.looks[look], skin.text[look], own, u16(~skin.set[look]));

    string512 node;
    for (u8 look = SUIShapeSkin::Highlight; look < SUIShapeSkin::Count; ++look)
    {
        xr_sprintf(node, "%s:%s", path, state_names[look]);
        if (!xml.NavigateToNode(node, index))
            continue;
        const SPartial state = read_partial(xml, node, index);
        apply(skin.looks[look], skin.text[look], state, u16(-1));
        skin.set[look] |= state.mask;
    }
}

std::unique_ptr<SUIShapeSkin> UIShape::ReadSkin(const CUIXml& xml, pcstr path, size_t index)
{
    cpcstr attribute = xml.ReadAttrib(path, index, "shape", nullptr);
    string512 child;
    xr_sprintf(child, "%s:shape", path);
    const bool hasChild = xml.NavigateToNode(child, index) != nullptr;
    if (!attribute && !hasChild)
        return nullptr;

    auto skin = std::make_unique<SUIShapeSkin>();
    cpcstr name = hasChild ? xml.ReadAttrib(child, index, "style", attribute) : attribute;
    if (name)
    {
        if (const SUIShapeSkin* base = UITheme::Skin(name))
            *skin = *base;
        else
            Msg("! [ui] [%s] in [%s] names an unknown shape style [%s]", path, xml.m_xml_file_name, name);
    }
    if (hasChild)
        ReadSkinNode(xml, child, index, *skin);
    return skin;
}

void UIShape::ReadLooks(
    const CUIXml& xml, pcstr path, size_t index, SUIShapeStyle (&looks)[LookCount], u32 (&text)[LookCount], u32 defaultText)
{
    string512 node{};
    xr_sprintf(node, "%s:normal", path);
    Read(xml, node, index, looks[0]);
    text[0] = ParseColor(xml.ReadAttrib(node, index, "text", nullptr), defaultText);
    for (size_t look = 1; look != LookCount; ++look)
    {
        looks[look] = looks[0];
        text[look] = text[0];
        xr_sprintf(node, "%s:%s", path, state_names[look]);
        if (!xml.NavigateToNode(node, index))
            continue;
        Read(xml, node, index, looks[look]);
        text[look] = ParseColor(xml.ReadAttrib(node, index, "text", nullptr), text[look]);
    }
}

namespace
{
struct SScreenShape
{
    float x0, y0, x1, y1;
    float border, radius, soft, margin;
};

// The pixel rectangle and the pixel lengths of a shape on `rect`, or false when it covers none.
bool to_screen(const Frect& rect, const SUIShapeStyle& style, SScreenShape& out)
{
    // Whole pixels: a one-pixel border lands on one row of pixels, not across two.
    out.x0 = std::round(UI().ClientToScreenScaledX(rect.x1));
    out.y0 = std::round(UI().ClientToScreenScaledY(rect.y1));
    out.x1 = std::round(UI().ClientToScreenScaledX(rect.x2));
    out.y1 = std::round(UI().ClientToScreenScaledY(rect.y2));
    if (out.x1 <= out.x0 || out.y1 <= out.y0)
        return false;
    // Thicknesses follow the height alone: a 16:9 layout squeezes its x units, not its lines.
    const float unit = UI().ClientToScreenScaledY(1.f);
    out.border = style.border > 0.f ? std::max(1.f, std::round(style.border * unit)) : 0.f;
    out.radius = std::round(style.radius * unit);
    out.soft = std::max(1.f, style.soft * unit);
    // the fade of a soft edge lies outside the rectangle, and so does a pictogram's glow
    out.margin = std::ceil(out.soft) + 1.f + (style.Pictogram() ? out.border : 0.f);
    return true;
}

void set_params(const SScreenShape& s, const SUIShapeStyle& style)
{
    const float unit = UI().ClientToScreenScaledY(1.f);
    // mode.y and mode.w: a star's fraction, a box's segment period and gap, a gas mask's badge
    Fvector4 mode;
    if (style.kind == Kind::Box)
        mode.set(0.f, style.segment > 0.f ? style.segment * unit : 0.f, s.soft, style.gap * unit);
    else if (style.kind == Kind::GasMask)
        mode.set(float(style.kind), float(style.badge), s.soft, style.badgeScale);
    else
        mode.set(float(style.kind), style.fraction, s.soft, 0.f);
    GEnv.UIRender->SetShapeParams(Fvector4().set((s.x1 - s.x0) * 0.5f, (s.y1 - s.y0) * 0.5f, s.radius, s.border),
        shape_color(style.line), mode);
}

void push_quad(float cx, float cy, float hx, float hy, float margin, u32 fill)
{
    // Positions go in half a pixel back, which stub_notransform_t adds again; the texture
    // coordinate is the position relative to the centre, so a pixel's centre reads x + 0.5.
    const auto point = [&](float x, float y) { GEnv.UIRender->PushPoint(x - 0.5f, y - 0.5f, 0.f, fill, x - cx, y - cy); };
    const float l = cx - hx - margin, t = cy - hy - margin, r = cx + hx + margin, b = cy + hy + margin;
    point(l, t);
    point(r, t);
    point(r, b);
    point(l, t);
    point(r, b);
    point(l, b);
}

void ensure_created(ui_shader& shader)
{
    if (!shader->inited())
        shader->create("xms_ui_shape", "ui\\ui_common");
}
} // namespace

void UIShape::Draw(ui_shader& shader, const Frect& rect, const SUIShapeStyle& style)
{
    // the shader draws in screen pixels; a 3D surface (pttLIT) has none
    if (!style.Visible() || UI().m_currentPointType != IUIRender::pttTL)
        return;
    ensure_created(shader);

    SScreenShape s;
    if (!to_screen(rect, style, s))
        return;

    GEnv.UIRender->SetShader(*shader);
    set_params(s, style);
    GEnv.UIRender->StartPrimitive(6, IUIRender::ptTriList, IUIRender::pttTL);
    push_quad((s.x0 + s.x1) * 0.5f, (s.y0 + s.y1) * 0.5f, (s.x1 - s.x0) * 0.5f, (s.y1 - s.y0) * 0.5f, s.margin, style.fill);
    GEnv.UIRender->FlushPrimitive();
}

void UIShape::Draw(const Frect& rect, const SUIShapeStyle& style) { Draw(UI().ShapeShader(), rect, style); }

void UIShape::DrawBatch(const Fvector2& size, const xr_vector<Fvector2>& centres, const xr_vector<u32>* colors,
    const SUIShapeStyle& style)
{
    if (centres.empty() || UI().m_currentPointType != IUIRender::pttTL || (!colors && !style.Visible()))
        return;
    ui_shader& shader = UI().ShapeShader();
    ensure_created(shader);

    // One pixel size for all of them: every box is the same shape, only its centre moves.
    SScreenShape s;
    if (!to_screen(Frect().set(0.f, 0.f, size.x, size.y), style, s))
        return;
    const float hx = (s.x1 - s.x0) * 0.5f;
    const float hy = (s.y1 - s.y0) * 0.5f;

    GEnv.UIRender->SetShader(*shader);
    set_params(s, style);
    GEnv.UIRender->StartPrimitive(u32(centres.size() * 6), IUIRender::ptTriList, IUIRender::pttTL);
    for (size_t i = 0; i < centres.size(); ++i)
    {
        // the corner on a whole pixel, or an odd border would straddle two rows
        const float cx = std::round(UI().ClientToScreenScaledX(centres[i].x) - hx) + hx;
        const float cy = std::round(UI().ClientToScreenScaledY(centres[i].y) - hy) + hy;
        push_quad(cx, cy, hx, hy, s.margin, colors ? (*colors)[i] : style.fill);
    }
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

void CUIShapeButton::InitLooks(const CUIXml& xml, pcstr path, size_t index)
{
    UIShape::ReadLooks(xml, path, index, looks, textColors, TextItemControl()->GetTextColor());
    icon = UIShape::ParseKind(xml.ReadAttrib(path, index, "icon", nullptr), SUIShapeStyle::Kind::Box);
    iconSize = xml.ReadAttribFlt(path, index, "icon_size", 0.f);
}
