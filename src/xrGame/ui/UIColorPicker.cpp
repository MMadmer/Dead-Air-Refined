// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIColorPicker.h"
#include "UIHelper.h"
#include "UIXmlInit.h"
#include "xrUICore/EditBox/UIEditBox.h"
#include "xrUICore/Static/UIShape.h"
#include "xrUICore/XML/xrUIXmlParser.h"

namespace
{
constexpr float Tau = 6.2831853f;

float to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f; }

u32 rgb_of(float r, float g, float b)
{
    const auto byte = [](float v) { return u32(iFloor(std::clamp(v, 0.f, 1.f) * 255.f + .5f)); };
    return color_rgba(byte(r), byte(g), byte(b), 255);
}

u32 hsv_of(float h, float s, float v)
{
    h = (h - std::floor(h)) * 6.f;
    const float f = h - std::floor(h);
    const float p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
    switch (int(h) % 6)
    {
    case 0: return rgb_of(v, t, p);
    case 1: return rgb_of(q, v, p);
    case 2: return rgb_of(p, v, t);
    case 3: return rgb_of(p, q, v);
    case 4: return rgb_of(t, p, v);
    default: return rgb_of(v, p, q);
    }
}

// x, y, width, height of a node, relative to the picker
Frect rect_of(CUIXml& xml, pcstr path)
{
    Frect r;
    r.x1 = xml.ReadAttribFlt(path, 0, "x", 0.f);
    r.y1 = xml.ReadAttribFlt(path, 0, "y", 0.f);
    r.x2 = r.x1 + xml.ReadAttribFlt(path, 0, "width", 0.f);
    r.y2 = r.y1 + xml.ReadAttribFlt(path, 0, "height", 0.f);
    return r;
}

SUIShapeStyle paint(SUIShapeStyle::Kind kind, u32 fill, u32 line, float fraction, float radius)
{
    SUIShapeStyle s;
    s.kind = kind;
    s.fill = fill;
    s.line = line;
    s.fraction = fraction;
    s.radius = radius;
    return s;
}

SUIShapeStyle box(u32 fill, u32 line, float border, float radius)
{
    SUIShapeStyle s;
    s.fill = fill;
    s.line = line;
    s.border = border;
    s.radius = radius;
    return s;
}

// a marker of the picker: a light ring round a dark one, readable over any colour
void marker(const Frect& r, float radius)
{
    UIShape::Draw(r, box(0, color_rgba(0, 0, 0, 200), 3.f, radius));
    UIShape::Draw(r, box(0, color_rgba(255, 255, 255, 255), 1.5f, radius));
}

constexpr pcstr row_nodes[] = { "row_r", "row_g", "row_b", "row_h", "row_s", "row_v" };
} // namespace

CUIColorPicker::CUIColorPicker() : CUIWindow("CUIColorPicker") {}

void CUIColorPicker::InitPicker(CUIXml& xml, pcstr path)
{
    CUIXmlInit::InitWindow(xml, path, 0, this);
    string256 node;
    const auto at = [&](pcstr part) { return strconcat(node, path, ":", part); };
    m_wheel = rect_of(xml, at("wheel"));
    m_sat = rect_of(xml, at("saturation"));
    m_val = rect_of(xml, at("value"));
    m_oldRect = rect_of(xml, at("old"));
    m_newRect = rect_of(xml, at("new"));
    UIHelper::CreateStatic(xml, at("old_caption"), this, false);
    UIHelper::CreateStatic(xml, at("new_caption"), this, false);
    for (u8 i = 0; i < RowCount; ++i)
    {
        string256 row;
        strconcat(row, path, ":", row_nodes[i]);
        UIHelper::CreateStatic(xml, strconcat(node, row, ":caption"), this, false);
        m_tracks[i] = rect_of(xml, strconcat(node, row, ":track"));
        m_fields[i] = UIHelper::CreateEditBox(xml, strconcat(node, row, ":field"), this);
    }
    UIHelper::CreateStatic(xml, at("hex_linear:caption"), this, false);
    m_hexLinear = UIHelper::CreateEditBox(xml, at("hex_linear:field"), this);
    UIHelper::CreateStatic(xml, at("hex_srgb:caption"), this, false);
    m_hexSrgb = UIHelper::CreateEditBox(xml, at("hex_srgb:field"), this);
    RefreshFields();
}

u32 CUIColorPicker::Color() const { return hsv_of(m_h, m_s, m_v); }

void CUIColorPicker::SetRgb(u32 color)
{
    const float r = color_get_R(color) / 255.f, g = color_get_G(color) / 255.f, b = color_get_B(color) / 255.f;
    const float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), d = mx - mn;
    m_v = mx;
    m_s = mx > 0.f ? d / mx : 0.f;
    // a grey keeps the hue it had
    if (d > 0.f)
    {
        const float h = mx == r ? (g - b) / d : (mx == g ? 2.f + (b - r) / d : 4.f + (r - g) / d);
        m_h = h / 6.f - std::floor(h / 6.f);
    }
}

void CUIColorPicker::SetColor(u32 color)
{
    SetRgb(color);
    RefreshFields();
}

Fvector3 CUIColorPicker::Linear() const
{
    const u32 c = Color();
    return { to_linear(color_get_R(c) / 255.f), to_linear(color_get_G(c) / 255.f), to_linear(color_get_B(c) / 255.f) };
}

void CUIColorPicker::SetLinear(const Fvector3& rgb)
{
    SetRgb(rgb_of(to_srgb(std::clamp(rgb.x, 0.f, 1.f)), to_srgb(std::clamp(rgb.y, 0.f, 1.f)), to_srgb(std::clamp(rgb.z, 0.f, 1.f))));
}

void CUIColorPicker::Changed()
{
    RefreshFields();
    if (onChange)
        onChange(Color());
}

void CUIColorPicker::RefreshFields()
{
    const Fvector3 lin = Linear();
    const u32 srgb = Color();
    string64 text;
    const auto set = [&](CUIEditBox* field, pcstr value)
    {
        // a field the player is typing into keeps what he types
        if (field && !field->IsInputFocused())
            field->SetText(value);
    };
    const float values[RowCount] = { lin.x, lin.y, lin.z, m_h * 360.f, m_s, m_v };
    for (u8 i = 0; i < RowCount; ++i)
    {
        xr_sprintf(text, i == RowH ? "%.1f" : "%.4f", values[i]);
        set(m_fields[i], text);
    }
    const u32 linear = rgb_of(lin.x, lin.y, lin.z);
    xr_sprintf(text, "%02X%02X%02XFF", color_get_R(linear), color_get_G(linear), color_get_B(linear));
    set(m_hexLinear, text);
    xr_sprintf(text, "%02X%02X%02XFF", color_get_R(srgb), color_get_G(srgb), color_get_B(srgb));
    set(m_hexSrgb, text);
}

void CUIColorPicker::Commit(CUIEditBox* field)
{
    pcstr text = field->GetText();
    for (u8 i = 0; i < RowCount; ++i)
    {
        if (field != m_fields[i])
            continue;
        char* end = nullptr;
        const float value = float(strtod(text, &end));
        if (end == text)
            break;
        if (i <= RowB)
        {
            Fvector3 lin = Linear();
            (&lin.x)[i] = value;
            SetLinear(lin);
        }
        else if (i == RowH)
            m_h = std::clamp(value, 0.f, 360.f) / 360.f;
        else if (i == RowS)
            m_s = std::clamp(value, 0.f, 1.f);
        else
            m_v = std::clamp(value, 0.f, 1.f);
        Changed();
        return;
    }
    if (field == m_hexLinear || field == m_hexSrgb)
    {
        // six digits or eight (the alpha, which a seed has not), a pasted '#' before them
        if (*text == '#')
            ++text;
        const size_t length = xr_strlen(text);
        char* end = nullptr;
        const u32 value = u32(strtoul(text, &end, 16));
        if ((length == 6 || length == 8) && end && !*end)
        {
            const u32 rgb = length == 8 ? value >> 8 : value;
            const float r = ((rgb >> 16) & 0xFF) / 255.f, g = ((rgb >> 8) & 0xFF) / 255.f, b = (rgb & 0xFF) / 255.f;
            if (field == m_hexLinear)
                SetLinear({ r, g, b });
            else
                SetRgb(rgb_of(r, g, b));
            Changed();
            return;
        }
    }
    // what does not read as a number comes back as it was
    RefreshFields();
}

void CUIColorPicker::FinishInput(bool keep)
{
    if (m_drag != PartNone)
    {
        m_drag = PartNone;
        if (CUIWindow* parent = GetParent())
            parent->SetCapture(this, false);
    }
    for (CUIEditBox* field : { m_fields[RowR], m_fields[RowG], m_fields[RowB], m_fields[RowH], m_fields[RowS],
             m_fields[RowV], m_hexLinear, m_hexSrgb })
    {
        if (!field || !field->IsInputFocused())
            continue;
        field->CaptureFocus(false);
        SetKeyboardCapture(field, false);
        if (keep)
            Commit(field);
    }
    RefreshFields();
}

void CUIColorPicker::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    CUIWindow::SendMessage(pWnd, msg, pData);
    // the mouse taken elsewhere mid-drag
    if (msg == WINDOW_MOUSE_CAPTURE_LOST)
        m_drag = PartNone;
    if (msg == EDIT_TEXT_COMMIT)
    {
        if (auto* field = smart_cast<CUIEditBox*>(pWnd))
            Commit(field);
    }
    else if (msg == EDIT_TEXT_CANCEL)
        RefreshFields();
}

void CUIColorPicker::Drag(const Fvector2& at)
{
    if (m_drag == PartWheel)
    {
        // the wheel is a circle on the screen: its half sizes in the interface's units differ across
        const float rx = m_wheel.width() * 0.5f, ry = m_wheel.height() * 0.5f;
        const float dx = (at.x - (m_wheel.x1 + rx)) / rx, dy = (at.y - (m_wheel.y1 + ry)) / ry;
        m_h = std::atan2(-dy, dx) / Tau;
        m_h -= std::floor(m_h);
        m_s = std::min(std::sqrt(dx * dx + dy * dy), 1.f);
    }
    else if (m_drag == PartSaturation)
        m_s = 1.f - std::clamp((at.y - m_sat.y1) / m_sat.height(), 0.f, 1.f);
    else if (m_drag == PartValue)
        m_v = 1.f - std::clamp((at.y - m_val.y1) / m_val.height(), 0.f, 1.f);
    else if (m_drag >= PartTrack)
    {
        const u8 row = m_drag - PartTrack;
        const Frect& track = m_tracks[row];
        const float t = std::clamp((at.x - track.x1) / track.width(), 0.f, 1.f);
        if (row <= RowB)
        {
            Fvector3 lin = Linear();
            (&lin.x)[row] = t;
            SetLinear(lin);
        }
        else if (row == RowH)
            m_h = std::min(t, 0.9999f);
        else if (row == RowS)
            m_s = t;
        else
            m_v = t;
    }
    Changed();
}

bool CUIColorPicker::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    const Fvector2 at{ x, y };
    if (m_drag != PartNone)
    {
        if (mouse_action == WINDOW_MOUSE_MOVE)
            Drag(at);
        else if (mouse_action == WINDOW_LBUTTON_UP)
        {
            m_drag = PartNone;
            if (CUIWindow* parent = GetParent())
                parent->SetCapture(this, false);
        }
        return true;
    }

    if (mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_LBUTTON_DB_CLICK)
    {
        const float rx = m_wheel.width() * 0.5f, ry = m_wheel.height() * 0.5f;
        const float dx = (x - (m_wheel.x1 + rx)) / rx, dy = (y - (m_wheel.y1 + ry)) / ry;
        // a slider takes a click a little round its track too
        const auto close_to = [&](const Frect& r) { return x >= r.x1 - 2.f && x <= r.x2 + 2.f && y >= r.y1 - 6.f && y <= r.y2 + 6.f; };
        if (dx * dx + dy * dy <= 1.f)
            m_drag = PartWheel;
        else if (m_sat.in(x, y))
            m_drag = PartSaturation;
        else if (m_val.in(x, y))
            m_drag = PartValue;
        else
        {
            for (u8 i = 0; i < RowCount; ++i)
            {
                if (close_to(m_tracks[i]))
                    m_drag = PartTrack + i;
            }
        }
        if (m_drag != PartNone)
        {
            // the drag follows the mouse out of the picker
            if (CUIWindow* parent = GetParent())
                parent->SetCapture(this, true);
            Drag(at);
            return true;
        }
    }
    return CUIWindow::OnMouseAction(x, y, mouse_action);
}

void CUIColorPicker::Draw()
{
    Fvector2 o;
    GetAbsolutePos(o);
    const auto abs = [&](const Frect& r) { return Frect().set(r.x1 + o.x, r.y1 + o.y, r.x2 + o.x, r.y2 + o.y); };
    const float kx = UI().get_current_kx();
    const u32 frame = color_rgba(232, 230, 224, 90);

    // the wheel, its rim, the marker at the hue and the saturation
    const Frect wheel = abs(m_wheel);
    UIShape::Draw(wheel, paint(SUIShapeStyle::Kind::HueWheel, 0xFFFFFFFF, 0, m_v, 0.f));
    UIShape::Draw(wheel, box(0, frame, 1.f, wheel.height() * 0.5f));
    {
        const float rx = wheel.width() * 0.5f, ry = wheel.height() * 0.5f;
        const float cx = wheel.x1 + rx + std::cos(m_h * Tau) * m_s * rx;
        const float cy = wheel.y1 + ry - std::sin(m_h * Tau) * m_s * ry;
        marker(Frect().set(cx - 6.f * kx, cy - 6.f, cx + 6.f * kx, cy + 6.f), 6.f);
    }

    // saturation and value, top to bottom, each with its mark
    const Frect sat = abs(m_sat), val = abs(m_val);
    UIShape::Draw(sat, paint(SUIShapeStyle::Kind::Gradient, hsv_of(m_h, 1.f, m_v), hsv_of(m_h, 0.f, m_v), 1.f, 2.f));
    UIShape::Draw(val, paint(SUIShapeStyle::Kind::Gradient, hsv_of(m_h, m_s, 1.f), color_rgba(0, 0, 0, 255), 1.f, 2.f));
    for (const auto& [bar, t] : { std::pair{ sat, 1.f - m_s }, std::pair{ val, 1.f - m_v } })
    {
        UIShape::Draw(bar, box(0, frame, 1.f, 2.f));
        const float y = bar.y1 + t * bar.height();
        marker(Frect().set(bar.x1 - 2.f * kx, y - 3.f, bar.x2 + 2.f * kx, y + 3.f), 1.f);
    }

    // the old colour and the new
    UIShape::Draw(abs(m_oldRect), box(m_old | 0xFF000000, frame, 1.f, 2.f));
    UIShape::Draw(abs(m_newRect), box(Color(), frame, 1.f, 2.f));

    // a slider per row: what the row's value does to the colour, from its least to its most
    const Fvector3 lin = Linear();
    for (u8 i = 0; i < RowCount; ++i)
    {
        const Frect track = abs(m_tracks[i]);
        float t;
        if (i <= RowB)
        {
            Fvector3 lo = lin, hi = lin;
            (&lo.x)[i] = 0.f;
            (&hi.x)[i] = 1.f;
            // blended in linear space, as the value runs: the colour under the mark is the colour
            UIShape::Draw(track, paint(SUIShapeStyle::Kind::Gradient, rgb_of(to_srgb(lo.x), to_srgb(lo.y), to_srgb(lo.z)),
                rgb_of(to_srgb(hi.x), to_srgb(hi.y), to_srgb(hi.z)), 2.f, 2.f));
            t = (&lin.x)[i];
        }
        else if (i == RowH)
        {
            UIShape::Draw(track, paint(SUIShapeStyle::Kind::HueBar, 0xFFFFFFFF, 0, 0.f, 2.f));
            t = m_h;
        }
        else if (i == RowS)
        {
            UIShape::Draw(track, paint(SUIShapeStyle::Kind::Gradient, hsv_of(m_h, 0.f, m_v), hsv_of(m_h, 1.f, m_v), 0.f, 2.f));
            t = m_s;
        }
        else
        {
            UIShape::Draw(track, paint(SUIShapeStyle::Kind::Gradient, color_rgba(0, 0, 0, 255), hsv_of(m_h, m_s, 1.f), 0.f, 2.f));
            t = m_v;
        }
        UIShape::Draw(track, box(0, frame, 1.f, 2.f));
        const float x = track.x1 + std::clamp(t, 0.f, 1.f) * track.width();
        marker(Frect().set(x - 2.5f * kx, track.y1 - 3.f, x + 2.5f * kx, track.y2 + 3.f), 1.f);
    }

    CUIWindow::Draw();
}
