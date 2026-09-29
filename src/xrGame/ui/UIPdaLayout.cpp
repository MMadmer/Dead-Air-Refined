// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIPdaLayout.h"
#include "UIPdaWnd.h"
#include "UIMapWnd.h"
#include "UIXmlInit.h"
#include "UIInventoryUtilities.h"
#include "xms_pda.h"

#include "Level.h"
#include "Actor.h"
#include "GametaskManager.h"
#include "GameTask.h"
#include "map_location.h"
#include "map_spot.h"
#include "UIMap.h"

#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Buttons/UIBtnHint.h"
#include "xrUICore/Cursor/UICursor.h"
#include "xrScriptEngine/script_engine.hpp"

namespace
{
// The fill texture: a white block in the corner of the shipped sheet. Every vertex samples
// its middle, so any shape comes out in exactly the vertex colour.
constexpr pcstr kSheet = "ui" DELIMITER "ui_xms_pda";
constexpr float kWhiteU = 8.f / 512.f;
constexpr float kWhiteV = 8.f / 512.f;
constexpr u32 kFormatPeriodMs = 250;

ui_shader& fill_shader()
{
    // created on first use, after the render device is up; lives for the process
    static ui_shader* shader = nullptr;
    if (!shader)
    {
        shader = xr_new<ui_shader>();
        (*shader)->create("hud" DELIMITER "default", kSheet);
    }
    return *shader;
}

CGameFont* font_by_name(const xr_string& name)
{
    const auto& f = UI().Font();
    if (name == "letterica16") return f.pFontLetterica16Russian;
    if (name == "letterica25") return f.pFontLetterica25;
    if (name == "graffiti19") return f.pFontGraffiti19Russian;
    if (name == "graffiti22") return f.pFontGraffiti22Russian;
    if (name == "graffiti32") return f.pFontGraffiti32Russian;
    if (name == "graffiti50") return f.pFontGraffiti50Russian;
    if (name == "arial_14") return f.pFontArial14;
    if (name == "medium") return f.pFontMedium;
    if (name == "small") return f.pFontStat;
    if (name == "di") return f.pFontDI;
    return f.pFontLetterica18Russian;
}

ETextAlignment h_align(char a)
{
    return a == 'c' ? CGameFont::alCenter : a == 'r' ? CGameFont::alRight : CGameFont::alLeft;
}

EVTextAlignment v_align(char a) { return a == 't' ? valTop : a == 'b' ? valBottom : valCenter; }

// ---- shapes ----------------------------------------------------------------------
// A rounded rectangle (ring_w == 0) or its outline, in UI (virtual) coordinates. The
// corners are circles in SCREEN pixels, as CUIStatic::DrawRoundedTexture draws them.
void draw_shape(const Frect& abs, float radius, u32 color, float ring_w)
{
    if (color_get_A(color) == 0)
        return;
    Fvector2 lt, rb;
    UI().ClientToScreenScaled(lt, abs.left, abs.top);
    UI().ClientToScreenScaled(rb, abs.right, abs.bottom);
    const float w = rb.x - lt.x, h = rb.y - lt.y;
    if (w <= 0.f || h <= 0.f)
        return;
    const float rad = _min(UI().ClientToScreenScaledY(radius), _min(w, h) * 0.5f);
    const float ring = ring_w > 0.f ? _max(1.f, UI().ClientToScreenScaledY(ring_w)) : 0.f;
    const u32 segments = rad < 1.f ? 0 : u32(clampr(rad * 0.5f, 3.f, 12.f));

    // outline of a rounded rect inset by `inset`, clockwise from the top right corner
    const auto outline = [&](float inset, xr_vector<Fvector2>& pts) {
        pts.clear();
        const float r = _max(rad - inset, 0.f);
        const Fvector2 centers[4] = {{rb.x - inset - r, lt.y + inset + r}, {rb.x - inset - r, rb.y - inset - r},
            {lt.x + inset + r, rb.y - inset - r}, {lt.x + inset + r, lt.y + inset + r}};
        for (u32 c = 0; c < 4; ++c)
        {
            const float base = -PI_DIV_2 + PI_DIV_2 * float(c);
            if (!segments)
            {
                pts.emplace_back(Fvector2().set(centers[c].x + r * _cos(base + PI_DIV_4) * 1.41421356f,
                    centers[c].y + r * _sin(base + PI_DIV_4) * 1.41421356f));
                continue;
            }
            for (u32 s = 0; s <= segments; ++s)
            {
                const float a = base + PI_DIV_2 * float(s) / float(segments);
                pts.emplace_back(Fvector2().set(centers[c].x + r * _cos(a), centers[c].y + r * _sin(a)));
            }
        }
    };

    static xr_vector<Fvector2> outer, inner;
    outline(0.f, outer);
    GEnv.UIRender->SetShader(*fill_shader());
    const auto push = [&](const Fvector2& p) { GEnv.UIRender->PushPoint(p.x, p.y, 0.f, color, kWhiteU, kWhiteV); };

    if (ring <= 0.f)
    {
        const Fvector2 center{(lt.x + rb.x) * 0.5f, (lt.y + rb.y) * 0.5f};
        GEnv.UIRender->StartPrimitive(u32(outer.size()) * 3, IUIRender::ptTriList, UI().m_currentPointType);
        for (size_t i = 0; i < outer.size(); ++i)
        {
            push(center);
            push(outer[i]);
            push(outer[(i + 1) % outer.size()]);
        }
        GEnv.UIRender->FlushPrimitive();
        return;
    }

    outline(_min(ring, _min(w, h) * 0.5f), inner);
    GEnv.UIRender->StartPrimitive(u32(outer.size()) * 6, IUIRender::ptTriList, UI().m_currentPointType);
    for (size_t i = 0; i < outer.size(); ++i)
    {
        const size_t j = (i + 1) % outer.size();
        push(outer[i]);
        push(outer[j]);
        push(inner[i]);
        push(inner[i]);
        push(outer[j]);
        push(inner[j]);
    }
    GEnv.UIRender->FlushPrimitive();
}

// ---- reading the Lua item list ---------------------------------------------------
using luabind::object;

bool lua_type_is(const object& o, int t) { return luabind::type(o) == t; }

xr_string get_str(const object& t, pcstr key)
{
    const object v = t[key];
    return lua_type_is(v, LUA_TSTRING) ? xr_string(luabind::object_cast<pcstr>(v)) : xr_string();
}

float get_num(const object& t, pcstr key, float def)
{
    const object v = t[key];
    return lua_type_is(v, LUA_TNUMBER) ? luabind::object_cast<float>(v) : def;
}

bool get_color(const object& t, pcstr key, u32& out)
{
    const object v = t[key];
    if (!lua_type_is(v, LUA_TNUMBER))
        return false;
    const double d = luabind::object_cast<double>(v);
    out = d <= 0.0 ? 0u : d >= 4294967295.0 ? 0xFFFFFFFFu : u32(d);
    return true;
}

void get_list(const object& t, pcstr key, xr_vector<xr_string>& out)
{
    out.clear();
    const object v = t[key];
    if (!lua_type_is(v, LUA_TTABLE))
        return;
    for (int i = 1;; ++i)
    {
        const object s = v[i];
        if (!lua_type_is(s, LUA_TSTRING))
            break;
        out.emplace_back(luabind::object_cast<pcstr>(s));
    }
}

bool starts_with(const xr_string& s, pcstr prefix, xr_string* rest = nullptr)
{
    const size_t n = xr_strlen(prefix);
    if (s.size() < n || 0 != s.compare(0, n, prefix))
        return false;
    if (rest)
        *rest = s.substr(n);
    return true;
}
} // namespace

// ================================================================================ items

CUIPdaItem::CUIPdaItem(CUIPdaLayout& owner, const CUIPdaLayout::SItem& item)
    : CUIWindow("CUIPdaItem"), m_owner(owner), m_item(item)
{
}

bool CUIPdaItem::Clickable() const
{
    return (m_item.kind == "button" || m_item.kind == "row") && (!m_item.action.empty() || m_item.kind == "row");
}

void CUIPdaItem::InitParts()
{
    // The parts are placed in canvas units and scaled per axis like everything else of the
    // layout: a square icon stays square in 16:9, where the virtual width is stretched.
    // docs/pda/PDA_FORMAT.md, "Buttons", is the same rule; the editor's preview mirrors it.
    const float scale = m_owner.Scale();
    const Fvector2 k = m_owner.Scale2();
    const Fvector2 size = GetWndSize();
    const float W = size.x / k.x, H = size.y / k.y;
    const auto rect = [&](float x1, float y1, float x2, float y2) { return Frect().set(x1 * k.x, y1 * k.y, x2 * k.x, y2 * k.y); };
    const bool button = m_item.kind == "button";

    if (m_item.kind == "image" && !m_item.texture.empty())
    {
        m_image = xr_new<CUIStatic>("pda_image");
        m_image->SetAutoDelete(true);
        AttachChild(m_image);
        m_image->SetWndRect(Frect().set(0.f, 0.f, size.x, size.y));
        m_image->InitTexture(m_item.texture.c_str(), false);
        m_image->SetStretchTexture(m_item.stretch);
        m_image->SetTextureColor(m_item.tint);
        if (m_item.radius > 0.f)
            m_image->SetTextureRounding(UI().ClientToScreenScaledY(m_item.radius * scale));
    }

    // an icon over a centred caption: the pair is stacked as a column
    const bool stacked = button && !m_item.icon.empty() && m_item.has_text && m_item.align == 'c' && m_item.valign != 'c';
    float text_left = 0.f;
    if (button && !m_item.icon.empty())
    {
        const float icon = _min(m_item.icon_size, _min(W, H));
        const bool alone = !m_item.has_text;
        const float x = alone || stacked ? (W - icon) * 0.5f : 12.f;
        const float y = stacked ? H * 0.5f - icon + 2.f : (H - icon) * 0.5f;
        m_image = xr_new<CUIStatic>("pda_icon");
        m_image->SetAutoDelete(true);
        AttachChild(m_image);
        m_image->SetWndRect(rect(x, y, x + icon, y + icon));
        m_image->InitTexture(m_item.icon.c_str(), false);
        m_image->SetStretchTexture(true);
        m_image->SetTextureColor(m_item.icon_color);
        if (!alone && !stacked)
            text_left = x + icon + 8.f;
    }

    if (m_item.kind == "text" || (button && m_item.has_text))
    {
        m_label = xr_new<CUIStatic>("pda_label");
        m_label->SetAutoDelete(true);
        AttachChild(m_label);
        const float pad = button && m_item.align != 'c' ? 12.f : 0.f;
        // a caption under an icon sits in the lower half
        const Frect r = rect(_max(pad, text_left), stacked ? H * 0.5f + 2.f : 0.f, W - pad, H);
        m_label->SetWndRect(r);
        m_label->TextureOff();
        m_label->SetFont(font_by_name(m_item.font));
        m_label->SetTextColor(m_item.text_color);
        m_label->SetTextAlignment(h_align(m_item.align));
        m_label->SetVTextAlignment(stacked ? valTop : v_align(m_item.valign));
        m_label->SetTextComplexMode(m_item.wrap);
        m_label->SetEllipsis(!m_item.wrap);
    }

    if (m_item.kind == "list")
    {
        m_rows = xr_new<CUIScrollView>();
        m_rows->SetAutoDelete(true);
        AttachChild(m_rows);
        m_rows->SetWndRect(Frect().set(0.f, 0.f, size.x, size.y));
        m_rows->InitScrollView();
        if (!m_item.empty.empty())
        {
            m_empty = xr_new<CUIStatic>("pda_list_empty");
            m_empty->SetAutoDelete(true);
            AttachChild(m_empty);
            m_empty->SetWndRect(rect(0.f, 0.f, W, _min(H, 60.f)));
            m_empty->TextureOff();
            m_empty->SetFont(font_by_name(m_item.font));
            m_empty->SetTextColor(m_item.text_color);
            m_empty->SetTextAlignment(CGameFont::alCenter);
            m_empty->SetVTextAlignment(valCenter);
            m_empty->SetText(m_item.empty.c_str());
            m_empty->Show(false);
        }
    }
}

void CUIPdaItem::SetLabel(pcstr text)
{
    if (m_label)
        m_label->SetText(text);
}

void CUIPdaItem::SetEmptyShown(bool v)
{
    if (m_empty)
        m_empty->Show(v);
}

u32 CUIPdaItem::BackColor() const
{
    const bool selected = m_item.kind == "row" ? m_row_selected : m_owner.IsSelected(this);
    if (Clickable() && m_pressed && CursorOverWindow() && m_item.has_press)
        return m_item.press;
    if (selected && m_item.has_selected)
        return m_item.selected;
    if (Clickable() && CursorOverWindow() && m_item.has_hover)
        return m_item.hover;
    return m_item.color;
}

void CUIPdaItem::DrawShape(u32 color, bool border) const
{
    Frect abs;
    GetAbsoluteRect(abs);
    // a row's window reaches over the gap below it; its card does not
    if (m_item.kind == "row")
        abs.bottom = abs.top + m_item.row_h * m_owner.Scale();
    const float scale = m_owner.Scale();
    if (!border)
        draw_shape(abs, m_item.radius * scale, color, 0.f);
    else if (m_item.has_border)
        draw_shape(abs, m_item.radius * scale, m_item.border, _max(m_item.border_w, 0.5f) * scale);
}

void CUIPdaItem::Draw()
{
    // the fill goes under the children, the outline over them (a frame stays a frame)
    const bool shaped = m_item.kind != "text" && m_item.kind != "image";
    if (shaped)
        DrawShape(BackColor(), false);

    // the per-state text and icon colours follow the same state as the background
    if (m_item.kind == "button")
    {
        const bool selected = m_owner.IsSelected(this);
        const bool hover = Clickable() && CursorOverWindow();
        if (m_label)
        {
            u32 c = m_item.text_color;
            if (selected && m_item.has_text_selected)
                c = m_item.text_selected;
            else if (hover && m_item.has_text_hover)
                c = m_item.text_hover;
            m_label->SetTextColor(c);
        }
        if (m_image)
            m_image->SetTextureColor(selected && m_item.has_icon_selected ? m_item.icon_selected : m_item.icon_color);
    }

    inherited::Draw();

    if (shaped)
        DrawShape(0, true);
    if (g_btnHint->Owner() == this)
        g_btnHint->Draw_();
}

void CUIPdaItem::Update()
{
    inherited::Update();

    if (m_item.hint.empty() || !CursorOverWindow() || !g_btnHint->Vacant() ||
        Device.dwTimeContinual <= FocusReceiveTime() + 700)
        return;

    // the stock button's placement: above the cursor, flipped to stay on screen
    g_btnHint->SetHintText(this, m_item.hint.c_str());
    const Fvector2 c_pos = GetUICursor().GetCursorPosition();
    const Frect vis{0.f, 0.f, UI_BASE_WIDTH, UI_BASE_HEIGHT};
    const auto inside = [&](const Frect& r) { return vis.x1 < r.x1 && vis.x2 > r.x2 && vis.y1 < r.y1 && vis.y2 > r.y2; };
    Frect r;
    r.set(0.f, 0.f, g_btnHint->GetWidth(), g_btnHint->GetHeight());
    r.add(c_pos.x, c_pos.y);
    r.sub(0.f, r.height());
    if (!inside(r))
        r.sub(r.width(), 0.f);
    if (!inside(r))
        r.add(0.f, r.height());
    if (!inside(r))
        r.add(r.width(), 45.f);
    g_btnHint->SetWndPos(r.lt);
}

void CUIPdaItem::OnFocusLost()
{
    inherited::OnFocusLost();
    m_pressed = false;
    if (g_btnHint->Owner() == this)
        g_btnHint->Discard();
}

void CUIPdaItem::Show(bool status)
{
    inherited::Show(status);
    if (!status && g_btnHint->Owner() == this)
        g_btnHint->Discard();
}

bool CUIPdaItem::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    // children first: a button inside a group or a row gets its own click
    if (inherited::OnMouseAction(x, y, mouse_action))
        return true;
    const bool click = mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_LBUTTON_UP ||
        mouse_action == WINDOW_LBUTTON_DB_CLICK || mouse_action == WINDOW_RBUTTON_DOWN;
    if (!Clickable())
        return click && (m_item.kind == "rect" || m_item.kind == "group") && color_get_A(m_item.color) > 0;
    if (m_item.kind == "row" && y > m_item.row_h * m_owner.Scale())
        return false;

    switch (mouse_action)
    {
    case WINDOW_LBUTTON_DOWN:
    case WINDOW_LBUTTON_DB_CLICK:
        m_pressed = true;
        return true;
    case WINDOW_LBUTTON_UP:
        if (m_pressed)
        {
            m_pressed = false;
            m_owner.OnItemClick(this);
        }
        return true;
    default: break;
    }
    return false;
}

// ================================================================================ layout

CUIPdaLayout::CUIPdaLayout(CUIPdaWnd& pda) : m_pda(pda) {}

CUIPdaLayout::~CUIPdaLayout()
{
    if (XmsPda::Live() == this)
        XmsPda::SetLive(nullptr);
    if (g_btnHint && g_btnHint->Owner())
        g_btnHint->Discard();
    for (CUIWindow* w : {m_under, m_over})
    {
        if (!w)
            continue;
        if (CUIWindow* parent = w->GetParent())
            parent->DetachChild(w);
        xr_delete(w);
    }
}

bool CUIPdaLayout::ReadItems(const object& list, xr_vector<SItem>& out, xr_string& err)
{
    out.clear();
    if (!lua_type_is(list, LUA_TTABLE))
    {
        err = "no item list";
        return false;
    }
    for (int i = 1;; ++i)
    {
        const object t = list[i];
        if (lua_type_is(t, LUA_TNIL))
            break;
        if (!lua_type_is(t, LUA_TTABLE))
        {
            err = "an item is not a table";
            return false;
        }
        SItem it;
        it.kind = get_str(t, "kind");
        const xr_string layer = get_str(t, "layer");
        it.layer = layer == "page" ? lrPage : layer == "overlay" ? lrOverlay : lrChrome;
        it.page = get_str(t, "page");
        it.parent = int(get_num(t, "parent", 0.f));
        it.id = get_str(t, "id");
        const float x = get_num(t, "x", 0.f), y = get_num(t, "y", 0.f);
        it.rect.set(x, y, x + get_num(t, "w", 0.f), y + get_num(t, "h", 0.f));
        {
            const object v = t["visible"];
            it.visible = !lua_type_is(v, LUA_TBOOLEAN) || luabind::object_cast<bool>(v);
        }
        get_color(t, "color", it.color);
        it.has_hover = get_color(t, "hover", it.hover);
        it.has_press = get_color(t, "press", it.press);
        it.has_selected = get_color(t, "selected", it.selected);
        it.has_border = get_color(t, "border", it.border);
        get_color(t, "tint", it.tint);
        it.radius = get_num(t, "radius", 0.f);
        it.border_w = get_num(t, "border_w", 2.f);
        it.texture = get_str(t, "texture");
        {
            const object v = t["stretch"];
            it.stretch = !lua_type_is(v, LUA_TBOOLEAN) || luabind::object_cast<bool>(v);
        }
        {
            const object v = t["text"];
            it.has_text = lua_type_is(v, LUA_TSTRING);
            if (it.has_text)
                it.text = luabind::object_cast<pcstr>(v);
        }
        it.font = get_str(t, "font");
        it.hint = get_str(t, "hint");
        get_color(t, "text_color", it.text_color);
        it.has_text_hover = get_color(t, "text_hover", it.text_hover);
        it.has_text_selected = get_color(t, "text_selected", it.text_selected);
        const xr_string align = get_str(t, "align"), valign = get_str(t, "valign");
        it.align = align.empty() ? 'l' : align[0];
        it.valign = valign.empty() ? 'c' : valign[0];
        {
            const object v = t["wrap"];
            it.wrap = lua_type_is(v, LUA_TBOOLEAN) && luabind::object_cast<bool>(v);
        }
        it.icon = get_str(t, "icon");
        it.icon_size = get_num(t, "icon_size", 24.f);
        get_color(t, "icon_color", it.icon_color);
        it.has_icon_selected = get_color(t, "icon_selected", it.icon_selected);
        get_list(t, "action", it.action);
        get_list(t, "selected_on", it.selected_on);
        it.row_h = get_num(t, "row_h", 0.f);
        it.gap = get_num(t, "gap", 0.f);
        it.empty = get_str(t, "empty");
        it.row = int(get_num(t, "row", 0.f));
        it.zoom = get_str(t, "zoom");
        if (it.parent < 0 || it.parent >= i)
        {
            err = "an item names a parent after itself";
            return false;
        }
        out.emplace_back(std::move(it));
    }
    return true;
}

void CUIPdaLayout::Scaled(const Frect& canvas, Frect& out) const
{
    out.set(canvas.x1 * m_scale.x, canvas.y1 * m_scale.y, canvas.x2 * m_scale.x, canvas.y2 * m_scale.y);
}

bool CUIPdaLayout::Build(const XMS::Module& owner, CUIWindow* host, const Frect& screen)
{
    luabind::functor<object> build;
    if (!GEnv.ScriptEngine->functor("xms_pda.build", build))
    {
        Msg("! XMS PDA: xms_pda.build is missing - the stock PDA stays");
        return false;
    }
    object result;
    try
    {
        result = build(owner.id.c_str(), owner.pda_layout.c_str());
    }
    catch (...)
    {
        Msg("! XMS PDA: [%s] %s: the layout script failed - the stock PDA stays", owner.id.c_str(),
            owner.pda_layout.c_str());
        return false;
    }
    if (!lua_type_is(result, LUA_TTABLE))
        return false; // xms_pda.build logged why

    m_module = owner.id;
    m_id = get_str(result, "id");
    m_title = get_str(result, "title");
    {
        const object c = result["canvas"];
        if (lua_type_is(c, LUA_TTABLE))
            m_canvas.set(_max(get_num(c, "w", 960.f), 1.f), _max(get_num(c, "h", 680.f), 1.f));
    }
    {
        const object d = result["device"];
        if (lua_type_is(d, LUA_TTABLE))
        {
            const object n = d["noise"], c = d["close"];
            m_noise = !lua_type_is(n, LUA_TBOOLEAN) || luabind::object_cast<bool>(n);
            m_close = !lua_type_is(c, LUA_TBOOLEAN) || luabind::object_cast<bool>(c);
        }
    }
    m_start = get_str(result, "start");
    {
        const object k = result["keys"];
        if (lua_type_is(k, LUA_TTABLE))
        {
            m_key_map = get_str(k, "map");
            m_key_tasks = get_str(k, "tasks");
            m_key_contacts = get_str(k, "contacts");
        }
    }

    {
        const object v = result["vars"];
        if (lua_type_is(v, LUA_TTABLE))
            for (luabind::iterator it(v), end; it != end; ++it)
                if (lua_type_is(it.key(), LUA_TSTRING) && lua_type_is(*it, LUA_TSTRING))
                    m_defaults[luabind::object_cast<pcstr>(it.key())] = luabind::object_cast<pcstr>(*it);
    }

    xr_vector<SItem> items;
    xr_string err;
    if (!ReadItems(result["items"], items, err))
    {
        Msg("! XMS PDA: [%s] %s: %s - the stock PDA stays", owner.id.c_str(), owner.pda_layout.c_str(), err.c_str());
        return false;
    }

    m_host = host;
    m_screen = screen;
    m_scale.set(screen.width() / m_canvas.x, screen.height() / m_canvas.y);

    m_under = xr_new<CUIWindow>("xms_pda_under");
    m_under->SetAutoDelete(false);
    m_under->SetWndRect(screen);
    host->AttachChild(m_under);

    m_chrome = xr_new<CUIWindow>("xms_pda_chrome");
    m_chrome->SetAutoDelete(true);
    m_chrome->SetWndRect(Frect().set(0.f, 0.f, screen.width(), screen.height()));
    m_under->AttachChild(m_chrome);

    {
        const object pages = result["pages"];
        for (int i = 1; lua_type_is(pages, LUA_TTABLE); ++i)
        {
            const object p = pages[i];
            if (!lua_type_is(p, LUA_TTABLE))
                break;
            SPage page;
            page.id = get_str(p, "id");
            page.title = get_str(p, "title");
            page.stock = get_str(p, "stock");
            page.root = xr_new<CUIWindow>("xms_pda_page");
            page.root->SetAutoDelete(true);
            page.root->SetWndRect(Frect().set(0.f, 0.f, screen.width(), screen.height()));
            page.root->Show(false);
            m_under->AttachChild(page.root);
            m_pages.emplace_back(std::move(page));
        }
    }

    m_over = xr_new<CUIWindow>("xms_pda_overlay");
    m_over->SetAutoDelete(false);
    m_over->SetWndRect(screen);
    host->AttachChild(m_over);

    CreateItems(items, nullptr);
    XmsPda::SetLive(this);
    RefreshTexts(true);

    Msg("* XMS PDA: layout [%s] of module [%s]: %u page(s), %u item(s)", m_id.c_str(), m_module.c_str(),
        u32(m_pages.size()), u32(m_items.size()));
    return true;
}

// Creates windows for items whose parent indices point into the same list. `list_parent`
// is the list the items are rows of (null for a whole layout).
CUIWindow* CUIPdaLayout::CreateItems(const xr_vector<SItem>& items, CUIWindow* list_parent)
{
    xr_vector<CUIWindow*> windows(items.size(), nullptr);
    for (size_t i = 0; i < items.size(); ++i)
    {
        const SItem& it = items[i];
        CUIWindow* parent = nullptr;
        if (it.parent > 0)
            parent = windows[it.parent - 1];
        else if (list_parent)
            parent = list_parent;
        else if (it.layer == lrOverlay)
            parent = m_over;
        else if (it.layer == lrPage)
        {
            for (const SPage& p : m_pages)
                if (p.id == it.page)
                    parent = p.root;
        }
        else
            parent = m_chrome;
        if (!parent)
            continue; // the parent could not be made; neither can its children

        if (it.kind == "map")
        {
            CUIPdaItem* frame = CreateItem(it, parent);
            windows[i] = frame;
            if (!m_map)
            {
                m_map = CreateMap(it, frame);
                m_map_page = it.page;
                m_map_global = it.zoom == "global";
            }
            continue;
        }
        // the rows of a list live in its scroll view, one after another
        CUIPdaItem* list = smart_cast<CUIPdaItem*>(parent);
        if (it.kind == "row" && list && list->Rows())
        {
            CUIPdaItem* row = xr_new<CUIPdaItem>(*this, it);
            row->Item().row_h = it.rect.height();
            Frect r;
            Scaled(Frect().set(0.f, 0.f, it.rect.width(), it.rect.height() + list->Item().gap), r);
            row->SetWndRect(r);
            row->InitParts();
            list->Rows()->AddWindow(row, true);
            m_items.push_back(row);
            windows[i] = row;
            continue;
        }
        windows[i] = CreateItem(it, parent);
    }

    // a list shows its "nothing here" line while it has no rows
    for (CUIPdaItem* item : m_items)
        if (item->Rows())
            item->SetEmptyShown(item->Rows()->Empty());
    return nullptr;
}

CUIPdaItem* CUIPdaLayout::CreateItem(const SItem& item, CUIWindow* parent)
{
    CUIPdaItem* w = xr_new<CUIPdaItem>(*this, item);
    w->SetAutoDelete(true);
    Frect r;
    Scaled(item.rect, r);
    w->SetWndRect(r);
    parent->AttachChild(w);
    w->InitParts();
    if (!item.visible)
        w->Show(false);
    m_items.push_back(w);
    if (item.has_text && strchr(item.text.c_str(), '{'))
        m_dynamic.push_back(w);
    else if (item.has_text)
        w->SetLabel(item.text.c_str());
    return w;
}

CUIMapWnd* CUIPdaLayout::CreateMap(const SItem& item, CUIWindow* parent)
{
    const Fvector2 size = parent->GetWndSize();
    // the hint card of the stock task map, scaled like the stock 16:9 layout scales it
    const float kx = UI().is_widescreen() ? 0.8f : 1.f;
    string4096 text;
    xr_sprintf(text,
        "<w><map_wnd scroll_enable=\"0\" map_move_step=\"11\">"
        "<main_wnd x=\"0\" y=\"0\" width=\"%.1f\" height=\"%.1f\"/>"
        "<level_frame x=\"0\" y=\"0\" width=\"%.1f\" height=\"%.1f\"/>"
        "<map_hint_item x=\"0\" y=\"0\" width=\"%.0f\" height=\"200\" stretch=\"1\">"
        "<texture>ui_icons_PDA_tooltips</texture>"
        "<simple_text x=\"%.0f\" y=\"12\" width=\"%.0f\" height=\"192\" complex_mode=\"1\"><text align=\"l\" font=\"letterica18\"/></simple_text>"
        "<t_icon x=\"%.0f\" y=\"15\" width=\"%.0f\" height=\"35\"/>"
        "<t_caption x=\"%.0f\" y=\"12\" width=\"%.0f\" height=\"18\" complex_mode=\"1\"><text align=\"l\" font=\"letterica18\"/></t_caption>"
        "<t_time x=\"%.0f\" y=\"38\" width=\"%.0f\" height=\"18\"><text align=\"l\" font=\"letterica16\" r=\"170\" g=\"170\" b=\"170\"/></t_time>"
        "<t_time_rem x=\"%.0f\" y=\"58\" width=\"%.0f\" height=\"18\"><text align=\"l\" font=\"letterica16\" r=\"170\" g=\"170\" b=\"170\"/></t_time_rem>"
        "<t_hint_text x=\"%.0f\" y=\"75\" width=\"%.0f\" height=\"18\" complex_mode=\"1\"><text align=\"l\" font=\"letterica18\"/></t_hint_text>"
        "</map_hint_item></map_wnd></w>",
        size.x, size.y, size.x, size.y, 300.f * kx, 8.f * kx, 185.f * kx, 8.f * kx, 62.f * kx, 80.f * kx, 190.f * kx,
        80.f * kx, 190.f * kx, 80.f * kx, 190.f * kx, 8.f * kx, 275.f * kx);

    CUIXml xml;
    if (!xml.Set(text, false))
    {
        Msg("! XMS PDA: the map slot could not be described");
        return nullptr;
    }
    CUIMapWnd* map = xr_new<CUIMapWnd>(m_pda.get_hint_wnd());
    if (!map->InitFromXml(xml, "map_wnd", false))
    {
        xr_delete(map);
        Msg("! XMS PDA: the map could not be made");
        return nullptr;
    }
    map->SetAutoDelete(true);
    parent->AttachChild(map);
    // not CUIMapWnd::Show: that one centres on the actor, and the PDA is built before there is one
    map->CUIWindow::Show(false);
    return map;
}

CUIPdaItem* CUIPdaLayout::FindItem(pcstr id) const
{
    if (!id || !id[0])
        return nullptr;
    for (CUIPdaItem* item : m_items)
        if (item->Item().id == id)
            return item;
    return nullptr;
}

void CUIPdaLayout::Forget(CUIWindow* w)
{
    // a list's rows are going away: drop every window under `w` from the registries
    const auto under = [w](CUIWindow* item) {
        for (CUIWindow* p = item; p; p = p->GetParent())
            if (p == w)
                return true;
        return false;
    };
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(), under), m_items.end());
    m_dynamic.erase(std::remove_if(m_dynamic.begin(), m_dynamic.end(), under), m_dynamic.end());
}

// ---- pages ----------------------------------------------------------------------

void CUIPdaLayout::ApplyPage()
{
    const SPage* current = nullptr;
    for (const SPage& p : m_pages)
    {
        const bool on = p.id == m_page;
        p.root->Show(on && p.stock.empty());
        if (on)
            current = &p;
    }
    ShowMap(m_shown && m_page == m_map_page && m_stock.empty());
    if (current && !current->stock.empty())
        ShowStock(current->stock.c_str());
    else if (!m_stock.empty())
        HideStock();
    m_under->Show(m_stock.empty());
    RefreshTexts(true);
}

bool CUIPdaLayout::Page(pcstr id)
{
    for (const SPage& p : m_pages)
    {
        if (p.id != id)
            continue;
        if (m_page != id)
            m_prev_page = m_page;
        m_page = id;
        if (m_shown)
            ApplyPage();
        return true;
    }
    return false;
}

void CUIPdaLayout::ShowStock(pcstr section)
{
    m_stock = section;
    if (!m_shown)
        return;
    ShowMap(false);
    m_pda.ShowStockSection(section);
    m_under->Show(false);
    RaiseOverlay();
}

void CUIPdaLayout::HideStock()
{
    m_stock.clear();
    m_pda.HideStockSection();
    m_under->Show(true);
}

void CUIPdaLayout::RaiseOverlay()
{
    // the stock section was attached after the layout; the overlay goes back on top
    if (m_host && m_over && m_host->IsChild(m_over))
    {
        m_host->DetachChild(m_over);
        m_host->AttachChild(m_over);
    }
}

void CUIPdaLayout::StockAction(pcstr section)
{
    // a stock page of the layout that shows this section is that page
    for (const SPage& p : m_pages)
        if (p.stock == section)
        {
            Page(p.id.c_str());
            return;
        }
    if (m_page.size())
        m_prev_page = m_page;
    ShowStock(section);
}

void CUIPdaLayout::OpenStock(pcstr section)
{
    if (0 == xr_strcmp(section, "eptTasks") || 0 == xr_strcmp(section, "eptMap"))
        OpenKey("map");
    else if (0 == xr_strcmp(section, "eptContacts"))
        OpenKey("contacts");
    else
        StockAction(section);
}

void CUIPdaLayout::OpenKey(pcstr key)
{
    const xr_string& action = 0 == xr_strcmp(key, "map") ? m_key_map :
        0 == xr_strcmp(key, "contacts")                   ? m_key_contacts :
                                                            m_key_tasks;
    if (!action.empty())
        RunAction(action, nullptr);
}

void CUIPdaLayout::OnShow()
{
    m_shown = true;
    if (m_page.empty())
        m_page = m_start;
    // a stock section opened by an action outside any page is not reopened
    if (!m_stock.empty())
    {
        bool page_stock = false;
        for (const SPage& p : m_pages)
            page_stock |= p.id == m_page && p.stock == m_stock;
        if (!page_stock)
            m_stock.clear();
    }
    ApplyPage();
}

void CUIPdaLayout::OnHide()
{
    m_shown = false;
    ShowMap(false);
    if (g_btnHint->Owner())
        g_btnHint->Discard();
}

void CUIPdaLayout::ShowMap(bool on)
{
    // CUIMapWnd::Show attaches the level maps and centres on the actor: once per change,
    // and never without an actor
    if (!m_map || on == m_map->IsShown())
        return;
    if (on && !Level().CurrentEntity())
        return;
    m_map->Show(on);
    // the first time only: afterwards the player's own zoom and scroll stay
    if (on && !m_map_opened)
    {
        m_map_opened = true;
        if (m_map_global)
            m_map->ViewGlobalMap();
    }
}

void CUIPdaLayout::DrawHint()
{
    if (m_map && m_map->IsShown())
        m_map->DrawHint();
}

// ---- state ------------------------------------------------------------------------

bool CUIPdaLayout::IsSelected(const CUIPdaItem* item) const
{
    const auto& on = item->Item().selected_on;
    return std::find(on.begin(), on.end(), m_page) != on.end();
}

bool CUIPdaLayout::ShowItem(pcstr id, int mode)
{
    CUIPdaItem* item = FindItem(id);
    if (!item)
        return false;
    const bool show = mode < 0 ? !item->IsShown() : mode > 0;
    item->Show(show);
    if (item->Item().kind == "map")
        ShowMap(show && m_shown && m_page == m_map_page && m_stock.empty());
    return true;
}

bool CUIPdaLayout::SetText(pcstr id, pcstr text)
{
    CUIPdaItem* item = FindItem(id);
    if (!item || (item->Item().kind != "text" && item->Item().kind != "button"))
        return false;
    item->Item().text = text ? text : "";
    item->Item().has_text = true;
    const bool dynamic = strchr(item->Item().text.c_str(), '{') != nullptr;
    const auto it = std::find(m_dynamic.begin(), m_dynamic.end(), item);
    if (dynamic && it == m_dynamic.end())
        m_dynamic.push_back(item);
    else if (!dynamic && it != m_dynamic.end())
        m_dynamic.erase(it);
    xr_string shown;
    Format(item->Item().text.c_str(), shown);
    item->SetLabel(shown.c_str());
    return true;
}

bool CUIPdaLayout::SetRows(pcstr id, const object& items)
{
    CUIPdaItem* list = FindItem(id);
    if (!list || !list->Rows())
        return false;
    xr_vector<SItem> rows;
    xr_string err;
    if (!ReadItems(items, rows, err))
    {
        Msg("! XMS PDA: rows of [%s]: %s", id, err.c_str());
        return false;
    }
    // the old rows leave the registries before the scroll view deletes them
    for (CUIWindow* row : list->Rows()->Items())
        Forget(row);
    list->Rows()->Clear();
    CreateItems(rows, list);
    RefreshTexts(true);
    return true;
}

void CUIPdaLayout::Format(pcstr raw, xr_string& out) const
{
    out.clear();
    if (!raw)
        return;
    for (pcstr p = raw; *p;)
    {
        pcstr close = *p == '{' ? strchr(p, '}') : nullptr;
        if (!close)
        {
            out += *p++;
            continue;
        }
        const xr_string name(p + 1, close);
        xr_string value;
        bool known = true;
        CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
        if (name == "time")
            value = GetGameTimeAsString(InventoryUtilities::etpTimeToMinutes).c_str();
        else if (name == "date")
            value = GetGameDateAsString(InventoryUtilities::edpDateToDay, '.', true).c_str();
        else if (name == "actor")
            value = actor ? actor->Name() : "";
        else if (name == "money")
        {
            string32 buf;
            xr_sprintf(buf, "%u", actor ? actor->get_money() : 0);
            value = buf;
        }
        else if (name == "rank")
            value = actor ? InventoryUtilities::GetRankAsText(actor->Rank()) : "";
        else if (name == "community")
            value = actor ? StringTable().translate(actor->CharacterInfo().Community().id()).c_str() : "";
        else if (name == "level")
            value = StringTable().translate(Level().name()).c_str();
        else if (name == "task")
        {
            CGameTask* task = Level().GameTaskManager().ActiveTask();
            value = task ? StringTable().translate(task->m_Title).c_str() : "";
        }
        else if (name == "title")
            value = m_title;
        else if (name == "page")
        {
            for (const SPage& pg : m_pages)
                if (pg.id == m_page)
                    value = pg.title;
        }
        else if (!XmsPda::GetVar(name.c_str(), value) && !DefaultVar(name.c_str(), value))
            known = false;
        if (known)
            out += value;
        else
            out.append(p, close + 1);
        p = close + 1;
    }
}

bool CUIPdaLayout::DefaultVar(pcstr name, xr_string& out) const
{
    const auto it = name ? m_defaults.find(name) : m_defaults.end();
    if (it == m_defaults.end())
        return false;
    out = it->second;
    return true;
}

void CUIPdaLayout::RefreshTexts(bool force)
{
    if (!force && Device.dwTimeGlobal < m_next_format)
        return;
    m_next_format = Device.dwTimeGlobal + kFormatPeriodMs;
    xr_string shown;
    for (CUIPdaItem* item : m_dynamic)
    {
        if (!force && !item->IsShown())
            continue;
        Format(item->Item().text.c_str(), shown);
        item->SetLabel(shown.c_str());
    }
}

void CUIPdaLayout::Update() { RefreshTexts(false); }

// ---- actions ----------------------------------------------------------------------

void CUIPdaLayout::OnItemClick(CUIPdaItem* item)
{
    // a row is the list's selection from now on
    if (item->Item().kind == "row")
    {
        if (CUIWindow* pad = item->GetParent())
            for (CUIWindow* w : pad->GetChildWndList())
                if (CUIPdaItem* row = smart_cast<CUIPdaItem*>(w))
                    row->SetRowSelected(row == item);
    }
    // copied: an action may rebuild the list the item lives in
    const xr_vector<xr_string> actions = item->Item().action;
    for (const xr_string& a : actions)
        RunAction(a, item);
}

bool CUIPdaLayout::Click(pcstr id, int row)
{
    CUIPdaItem* item = FindItem(id);
    if (!item)
        return false;
    if (row <= 0)
    {
        OnItemClick(item);
        return true;
    }
    if (!item->Rows())
        return false;
    for (CUIWindow* w : item->Rows()->Items())
        if (CUIPdaItem* r = smart_cast<CUIPdaItem*>(w); r && r->Item().row == row)
        {
            OnItemClick(r);
            return true;
        }
    return false;
}

void CUIPdaLayout::RunAction(const xr_string& action, CUIPdaItem* item)
{
    xr_string arg;
    if (starts_with(action, "page:", &arg))
        Page(arg.c_str());
    else if (action == "back")
    {
        if (!m_stock.empty() && (m_prev_page.empty() || m_prev_page == m_page))
        {
            HideStock();
            ApplyPage();
        }
        else if (!m_prev_page.empty())
            Page(m_prev_page.c_str());
    }
    else if (action == "close")
        m_pda.HideDialog();
    else if (starts_with(action, "show:", &arg))
        ShowItem(arg.c_str(), 1);
    else if (starts_with(action, "hide:", &arg))
        ShowItem(arg.c_str(), 0);
    else if (starts_with(action, "toggle:", &arg))
        ShowItem(arg.c_str(), -1);
    else if (starts_with(action, "stock:", &arg))
        StockAction(arg.c_str());
    else if (starts_with(action, "map:", &arg))
    {
        if (!m_map)
            return;
        if (m_page != m_map_page)
            Page(m_map_page.c_str());
        if (arg == "actor")
            m_map->ViewActor();
        else if (arg == "zoom_in")
            m_map->ViewZoomIn();
        else if (arg == "zoom_out")
            m_map->ViewZoomOut();
        else if (arg == "global")
            m_map->ViewGlobalMap();
    }
    else if (starts_with(action, "var:", &arg))
    {
        const size_t eq = arg.find('=');
        if (eq != xr_string::npos)
        {
            xr_string value;
            Format(arg.substr(eq + 1).c_str(), value);
            XmsPda::SetVar(arg.substr(0, eq).c_str(), value.c_str());
            RefreshTexts(true);
        }
    }
    else
    {
        // signal: and lua: belong to the scripts
        luabind::functor<void> fn;
        if (GEnv.ScriptEngine->functor("xms_pda.on_action", fn))
        {
            try
            {
                fn(action.c_str(), item ? item->Item().id.c_str() : "", item ? item->Item().row : 0);
            }
            catch (...)
            {
                Msg("! XMS PDA: action [%s] failed", action.c_str());
            }
        }
    }
}

void CUIPdaLayout::Describe(xr_string& out) const
{
    string512 line;
    xr_sprintf(line, "layout %s (module %s), page %s%s%s, map %s\n", m_id.c_str(), m_module.c_str(), m_page.c_str(),
        m_stock.empty() ? "" : ", stock ", m_stock.c_str(), m_map ? (m_map->IsShown() ? "shown" : "hidden") : "none");
    out += line;
    for (const SPage& p : m_pages)
    {
        xr_sprintf(line, "page %s%s%s %s\n", p.id.c_str(), p.stock.empty() ? "" : " stock=", p.stock.c_str(),
            p.root->IsShown() ? "shown" : "hidden");
        out += line;
    }
    for (const CUIPdaItem* item : m_items)
    {
        const SItem& it = item->Item();
        if (it.id.empty() && it.kind != "row")
            continue;
        Frect abs;
        item->GetAbsoluteRect(abs);
        bool visible = item->IsShown();
        for (const CUIWindow* p = item->GetParent(); p && visible; p = p->GetParent())
            visible = p->IsShown();
        string32 row;
        row[0] = 0;
        if (it.kind == "row")
            xr_sprintf(row, "#%d", it.row);
        xr_sprintf(line, "item %s %s%s %s %.0f,%.0f,%.0f,%.0f\n", it.kind.c_str(), it.id.c_str(), row,
            visible ? "visible" : "hidden", abs.x1, abs.y1, abs.width(), abs.height());
        out += line;
    }

    // What the map slot shows: a location's spot is a child of the level map only while the
    // slot's rect holds it (CMapLocation::UpdateSpot); a pointer is the arrow to one outside.
    if (!m_map || !m_map->IsShown() || !g_pGameLevel)
        return;
    const auto found = m_map->GameMaps().find(Level().name());
    if (found == m_map->GameMaps().end() || !found->second->IsShown())
        return;
    for (CUIWindow* child : found->second->GetChildWndList())
    {
        CMapSpot* spot = smart_cast<CMapSpot*>(child);
        CMapLocation* location = spot ? spot->MapLocation() : nullptr;
        if (!location)
            continue;
        const bool pointer = !!smart_cast<CMapSpotPointer*>(spot);
        if (!pointer && spot != location->LevelMapSpot() && spot != location->complex_spot())
            continue; // a task border drawn around a spot
        Frect abs;
        spot->GetAbsoluteRect(abs);
        Fvector2 center;
        abs.getcenter(center);
        const pcstr hint = location->GetHint();
        xr_sprintf(line, "spot %u %s %.0f,%.0f %s\n", location->ObjectID(), pointer ? "pointer" : "shown", center.x,
            center.y, hint ? hint : "");
        out += line;
    }
}
