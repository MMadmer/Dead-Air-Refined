// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "CommunityHtmlCanvas.h"

#if defined(XR_PLATFORM_WINDOWS)
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

namespace
{
template <class T>
void release(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

std::wstring widen(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(std::max(length, 0)), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), length);
    return out;
}

xr_string narrow(std::wstring_view wide)
{
    if (wide.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    xr_string out(size_t(std::max(length, 0)), '\0');
    if (length > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), length, nullptr, nullptr);
    return out;
}

D2D1_RECT_F rect(const litehtml::position& p) { return D2D1::RectF(p.x, p.y, p.x + p.width, p.y + p.height); }

D2D1_COLOR_F color(const litehtml::web_color& c)
{
    return D2D1::ColorF(c.red / 255.f, c.green / 255.f, c.blue / 255.f, c.alpha / 255.f);
}

// CSS gives each corner its own radii, Direct2D one pair: the largest of them, which keeps a circle a circle
D2D1_ROUNDED_RECT rounded(const litehtml::position& box, const litehtml::border_radiuses& r)
{
    const float x = std::max({ r.top_left_x, r.top_right_x, r.bottom_right_x, r.bottom_left_x });
    const float y = std::max({ r.top_left_y, r.top_right_y, r.bottom_right_y, r.bottom_left_y });
    return D2D1::RoundedRect(rect(box), std::min(x, box.width * 0.5f), std::min(y, box.height * 0.5f));
}

bool any_radius(const litehtml::border_radiuses& r)
{
    return r.top_left_x > 0 || r.top_right_x > 0 || r.bottom_right_x > 0 || r.bottom_left_x > 0;
}

// a data: URI's bytes; false for any other address
bool data_uri(std::string_view uri, xr_vector<u8>& bytes)
{
    if (uri.substr(0, 5) != "data:")
        return false;
    const size_t comma = uri.find(',');
    if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
        return false;
    bytes.clear();
    u32 bits = 0;
    int count = 0;
    for (const char c : uri.substr(comma + 1))
    {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else if (c == '=')
            break;
        else
            continue; // whitespace a page may wrap the data in
        bits = (bits << 6) | u32(v);
        if ((count += 6) >= 8)
        {
            count -= 8;
            bytes.push_back(u8(bits >> count));
        }
    }
    return !bytes.empty();
}
} // namespace

struct CommunityHtmlCanvas::Font
{
    IDWriteTextFormat* format{};
    bool underline{};
    bool strike{};
};

CommunityHtmlCanvas::CommunityHtmlCanvas()
{
    // WIC wants COM; the process joins its multithreaded apartment whatever the thread's own is
    // (Windows 8's CoIncrementMTAUsage: the engine's headers are set for 7, so it is looked up).
    static const bool mta = []
    {
        using Increment = HRESULT(WINAPI*)(void**);
        HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto increment = combase ? reinterpret_cast<Increment>(GetProcAddress(combase, "CoIncrementMTAUsage")) : nullptr;
        void* cookie = nullptr;
        return increment && SUCCEEDED(increment(&cookie));
    }();
    UNUSED(mta);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&m_dwrite))) ||
        FAILED(m_dwrite->GetSystemFontCollection(&m_fonts)) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wic))))
    {
        Msg("! [community] Direct2D, DirectWrite or WIC is not available");
        release(m_wic);
        release(m_fonts);
        release(m_dwrite);
        release(m_d2d);
    }
}

CommunityHtmlCanvas::~CommunityHtmlCanvas()
{
    for (auto& [_, picture] : m_pictures)
    {
        release(picture.bitmap);
        release(picture.source);
    }
    release(m_wic);
    release(m_fonts);
    release(m_dwrite);
    release(m_d2d);
}

void CommunityHtmlCanvas::SetViewport(float width, float height, float scale)
{
    m_width = width;
    m_height = height;
    m_scale = scale;
}

litehtml::position CommunityHtmlCanvas::Measure(const litehtml::document::ptr& doc)
{
    m_measuring = true;
    m_measured_any = false;
    m_measured = {};
    doc->draw(0, 0, 0, nullptr);
    m_measuring = false;
    return m_measured_any ? m_measured : litehtml::position{};
}

bool CommunityHtmlCanvas::measure(const litehtml::position& box)
{
    if (!m_measuring)
        return false;
    if (box.width > 0 && box.height > 0)
    {
        if (!m_measured_any)
            m_measured = box;
        else
        {
            const float left = std::min(m_measured.x, box.x);
            const float top = std::min(m_measured.y, box.y);
            const float right = std::max(m_measured.right(), box.right());
            const float bottom = std::max(m_measured.bottom(), box.bottom());
            m_measured = litehtml::position(left, top, right - left, bottom - top);
        }
        m_measured_any = true;
    }
    return true;
}

bool CommunityHtmlCanvas::Rasterize(const litehtml::document::ptr& doc, const litehtml::position& area, u32 width,
    u32 height, xr_vector<u8>& rgba)
{
    if (!Valid() || !width || !height)
        return false;

    IWICBitmap* surface = nullptr;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(m_wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &surface)) ||
        FAILED(m_d2d->CreateWicBitmapRenderTarget(surface, props, &m_target)) ||
        FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &m_brush)))
    {
        release(m_brush);
        release(m_target);
        release(surface);
        return false;
    }

    m_target->BeginDraw();
    m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
    // a transparent picture: grey antialiasing, ClearType's colour fringes would show over the menu
    m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    m_target->SetTransform(D2D1::Matrix3x2F::Translation(-area.x, -area.y) * D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    doc->draw(0, 0, 0, nullptr);
    while (m_clips)
        del_clip();
    const bool drawn = SUCCEEDED(m_target->EndDraw());

    for (auto& [_, picture] : m_pictures)
        release(picture.bitmap);
    release(m_brush);
    release(m_target);

    // premultiplied BGRA to straight RGBA, as the interface's textures blend
    bool read = false;
    IWICBitmapLock* lock = nullptr;
    const WICRect all{ 0, 0, INT(width), INT(height) };
    if (drawn && SUCCEEDED(surface->Lock(&all, WICBitmapLockRead, &lock)))
    {
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        if (SUCCEEDED(lock->GetStride(&stride)) && SUCCEEDED(lock->GetDataPointer(&size, &data)))
        {
            rgba.resize(size_t(width) * height * 4);
            for (u32 y = 0; y < height; ++y)
            {
                const BYTE* src = data + size_t(y) * stride;
                u8* dst = rgba.data() + size_t(y) * width * 4;
                for (u32 x = 0; x < width; ++x, src += 4, dst += 4)
                {
                    const u32 a = src[3];
                    dst[0] = a ? u8(std::min(255u, (src[2] * 255u + a / 2) / a)) : 0;
                    dst[1] = a ? u8(std::min(255u, (src[1] * 255u + a / 2) / a)) : 0;
                    dst[2] = a ? u8(std::min(255u, (src[0] * 255u + a / 2) / a)) : 0;
                    dst[3] = u8(a);
                }
            }
            read = true;
        }
        lock->Release();
    }
    surface->Release();
    return read;
}

litehtml::uint_ptr CommunityHtmlCanvas::create_font(const litehtml::font_description& descr, const litehtml::document*,
    litehtml::font_metrics* fm)
{
    if (!Valid())
        return 0;

    // the first family of the list the system has
    std::wstring family = L"Segoe UI";
    IDWriteFontFamily* found = nullptr;
    std::string_view families = descr.family;
    while (!families.empty())
    {
        const size_t comma = families.find(',');
        std::string_view name = families.substr(0, comma);
        families = comma == std::string_view::npos ? std::string_view{} : families.substr(comma + 1);
        while (!name.empty() && (name.front() == ' ' || name.front() == '"' || name.front() == '\''))
            name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '"' || name.back() == '\''))
            name.remove_suffix(1);
        const std::wstring wide = widen(name);
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (!wide.empty() && SUCCEEDED(m_fonts->FindFamilyName(wide.c_str(), &index, &exists)) && exists &&
            SUCCEEDED(m_fonts->GetFontFamily(index, &found)))
        {
            family = wide;
            break;
        }
    }
    if (!found)
    {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(m_fonts->FindFamilyName(family.c_str(), &index, &exists)) && exists)
            m_fonts->GetFontFamily(index, &found);
    }

    const auto weight = DWRITE_FONT_WEIGHT(std::clamp(descr.weight, 1, 999));
    const auto style = descr.style == litehtml::font_style_italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    const float size = std::max(descr.size, 1.f);

    auto* font = xr_new<Font>();
    font->underline = (descr.decoration_line & litehtml::text_decoration_line_underline) != 0;
    font->strike = (descr.decoration_line & litehtml::text_decoration_line_line_through) != 0;
    if (FAILED(m_dwrite->CreateTextFormat(family.c_str(), m_fonts, weight, style, DWRITE_FONT_STRETCH_NORMAL, size,
            L"ru-ru", &font->format)))
    {
        release(found);
        xr_delete(font);
        return 0;
    }
    font->format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (fm)
    {
        fm->font_size = size;
        fm->ascent = size * 0.9f;
        fm->descent = size * 0.25f;
        fm->x_height = size * 0.5f;
        IDWriteFont* face = nullptr;
        if (found && SUCCEEDED(found->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, &face)))
        {
            DWRITE_FONT_METRICS metrics{};
            face->GetMetrics(&metrics);
            const float em = size / float(std::max<UINT16>(metrics.designUnitsPerEm, 1));
            fm->ascent = metrics.ascent * em;
            fm->descent = metrics.descent * em;
            fm->x_height = metrics.xHeight * em;
            // DirectWrite's line carries the gap above the text; litehtml's line starts at the ascent
            font->format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fm->ascent + fm->descent, fm->ascent);
            face->Release();
        }
        fm->height = fm->ascent + fm->descent;
        fm->ch_width = text_width("0", litehtml::uint_ptr(font));
        fm->draw_spaces = descr.style == litehtml::font_style_italic || descr.decoration_line != litehtml::text_decoration_line_none;
        fm->sub_shift = size / 5;
        fm->super_shift = size / 3;
    }
    release(found);
    return litehtml::uint_ptr(font);
}

void CommunityHtmlCanvas::delete_font(litehtml::uint_ptr hFont)
{
    auto* font = reinterpret_cast<Font*>(hFont);
    if (!font)
        return;
    release(font->format);
    xr_delete(font);
}

litehtml::pixel_t CommunityHtmlCanvas::text_width(const char* text, litehtml::uint_ptr hFont)
{
    auto* font = reinterpret_cast<Font*>(hFont);
    if (!font || !text || !*text)
        return 0;
    const std::wstring wide = widen(text);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(m_dwrite->CreateTextLayout(wide.c_str(), UINT32(wide.size()), font->format, 1e6f, 1e6f, &layout)))
        return 0;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    layout->Release();
    return metrics.widthIncludingTrailingWhitespace;
}

void CommunityHtmlCanvas::draw_text(litehtml::uint_ptr, const char* text, litehtml::uint_ptr hFont,
    litehtml::web_color c, const litehtml::position& pos)
{
    auto* font = reinterpret_cast<Font*>(hFont);
    if (measure(pos) || !m_target || !font || !text || !*text)
        return;
    const std::wstring wide = widen(text);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(m_dwrite->CreateTextLayout(wide.c_str(), UINT32(wide.size()), font->format, 1e6f, 1e6f, &layout)))
        return;
    const DWRITE_TEXT_RANGE all{ 0, UINT32(wide.size()) };
    if (font->underline)
        layout->SetUnderline(TRUE, all);
    if (font->strike)
        layout->SetStrikethrough(TRUE, all);
    m_target->DrawTextLayout(D2D1::Point2F(pos.x, pos.y), layout, brush(c));
    layout->Release();
}

void CommunityHtmlCanvas::draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker& marker)
{
    if (measure(marker.pos) || !m_target)
        return;
    const float size = std::min(marker.pos.width, marker.pos.height) * 0.4f;
    const float cx = marker.pos.x + marker.pos.width * 0.5f;
    const float cy = marker.pos.y + marker.pos.height * 0.5f;
    switch (marker.marker_type)
    {
    case litehtml::list_style_type_disc:
        m_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), size, size), brush(marker.color));
        break;
    case litehtml::list_style_type_circle:
        m_target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), size, size), brush(marker.color), 1.f);
        break;
    case litehtml::list_style_type_square:
        m_target->FillRectangle(D2D1::RectF(cx - size, cy - size, cx + size, cy + size), brush(marker.color));
        break;
    default: break;
    }
}

void CommunityHtmlCanvas::load_image(const char* src, const char*, bool)
{
    if (!Valid() || !src || m_pictures.count(src))
        return;
    Picture& picture = m_pictures[src];

    xr_vector<u8> bytes;
    if (!data_uri(src, bytes))
    {
        Msg("! [community] a picture that is not a data: URI is not shown (%.60s)", src);
        return;
    }
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(m_wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(bytes.data(), DWORD(bytes.size()))) &&
        SUCCEEDED(m_wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(m_wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f,
            WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(m_wic->CreateBitmapFromSource(converter, WICBitmapCacheOnLoad, &picture.source)))
    {
        UINT w = 0, h = 0;
        picture.source->GetSize(&w, &h);
        picture.width = w;
        picture.height = h;
    }
    else
        Msg("! [community] a picture of the page does not decode");
    release(converter);
    release(frame);
    release(decoder);
    release(stream);
}

void CommunityHtmlCanvas::get_image_size(const char* src, const char*, litehtml::size& sz)
{
    const auto it = src ? m_pictures.find(src) : m_pictures.end();
    sz.width = it != m_pictures.end() ? litehtml::pixel_t(it->second.width) : 0;
    sz.height = it != m_pictures.end() ? litehtml::pixel_t(it->second.height) : 0;
}

ID2D1Bitmap* CommunityHtmlCanvas::bitmap(Picture& picture)
{
    if (!picture.bitmap && picture.source && m_target)
        m_target->CreateBitmapFromWicBitmap(picture.source, nullptr, &picture.bitmap);
    return picture.bitmap;
}

ID2D1SolidColorBrush* CommunityHtmlCanvas::brush(const litehtml::web_color& c)
{
    m_brush->SetColor(color(c));
    return m_brush;
}

template <typename Draw>
void CommunityHtmlCanvas::in_layer(const litehtml::background_layer& layer, Draw&& draw)
{
    m_target->PushAxisAlignedClip(rect(layer.clip_box), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ID2D1RoundedRectangleGeometry* shape = nullptr;
    ID2D1Layer* mask = nullptr;
    if (any_radius(layer.border_radius) &&
        SUCCEEDED(m_d2d->CreateRoundedRectangleGeometry(rounded(layer.border_box, layer.border_radius), &shape)) &&
        SUCCEEDED(m_target->CreateLayer(&mask)))
        m_target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape), mask);
    draw();
    if (mask)
        m_target->PopLayer();
    release(mask);
    release(shape);
    m_target->PopAxisAlignedClip();
}

void CommunityHtmlCanvas::draw_image(litehtml::uint_ptr, const litehtml::background_layer& layer, const std::string& url,
    const std::string&)
{
    if (measure(layer.clip_box) || !m_target)
        return;
    const auto it = m_pictures.find(url.c_str());
    ID2D1Bitmap* picture = it != m_pictures.end() ? bitmap(it->second) : nullptr;
    if (!picture)
        return;

    in_layer(layer, [&]
    {
        const litehtml::position& origin = layer.origin_box;
        if (origin.width <= 0 || origin.height <= 0)
            return;
        const bool repeat_x = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_x;
        const bool repeat_y = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_y;
        const litehtml::position& clip = layer.clip_box;
        float x0 = origin.x, y0 = origin.y;
        if (repeat_x)
            x0 -= std::ceil((origin.x - clip.x) / origin.width) * origin.width;
        if (repeat_y)
            y0 -= std::ceil((origin.y - clip.y) / origin.height) * origin.height;
        for (float y = y0; y < (repeat_y ? clip.bottom() : y0 + 1); y += origin.height)
            for (float x = x0; x < (repeat_x ? clip.right() : x0 + 1); x += origin.width)
                m_target->DrawBitmap(picture, D2D1::RectF(x, y, x + origin.width, y + origin.height), 1.f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    });
}

void CommunityHtmlCanvas::draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer& layer,
    const litehtml::web_color& c)
{
    if (c.alpha == 0 || measure(layer.border_box) || !m_target)
        return;
    in_layer(layer, [&] { m_target->FillRectangle(rect(layer.border_box), brush(c)); });
}

void CommunityHtmlCanvas::draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer& layer,
    const litehtml::background_layer::linear_gradient& gradient)
{
    if (measure(layer.border_box) || !m_target || gradient.color_points.empty())
        return;
    xr_vector<D2D1_GRADIENT_STOP> stops;
    for (const auto& point : gradient.color_points)
        stops.push_back({ point.offset, color(point.color) });
    ID2D1GradientStopCollection* collection = nullptr;
    ID2D1LinearGradientBrush* paint = nullptr;
    if (SUCCEEDED(m_target->CreateGradientStopCollection(stops.data(), UINT32(stops.size()), &collection)) &&
        SUCCEEDED(m_target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(gradient.start.x, gradient.start.y), D2D1::Point2F(gradient.end.x, gradient.end.y)), collection, &paint)))
        in_layer(layer, [&] { m_target->FillRectangle(rect(layer.border_box), paint); });
    release(paint);
    release(collection);
}

void CommunityHtmlCanvas::draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer& layer,
    const litehtml::background_layer::radial_gradient& gradient)
{
    if (measure(layer.border_box) || !m_target || gradient.color_points.empty())
        return;
    xr_vector<D2D1_GRADIENT_STOP> stops;
    for (const auto& point : gradient.color_points)
        stops.push_back({ point.offset, color(point.color) });
    ID2D1GradientStopCollection* collection = nullptr;
    ID2D1RadialGradientBrush* paint = nullptr;
    if (SUCCEEDED(m_target->CreateGradientStopCollection(stops.data(), UINT32(stops.size()), &collection)) &&
        SUCCEEDED(m_target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
            D2D1::Point2F(gradient.position.x, gradient.position.y), D2D1::Point2F(0, 0), gradient.radius.x,
            gradient.radius.y), collection, &paint)))
        in_layer(layer, [&] { m_target->FillRectangle(rect(layer.border_box), paint); });
    release(paint);
    release(collection);
}

// Direct2D has no conic gradient: its first colour stands for it
void CommunityHtmlCanvas::draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
    const litehtml::background_layer::conic_gradient& gradient)
{
    if (!gradient.color_points.empty())
        draw_solid_fill(hdc, layer, gradient.color_points.front().color);
}

void CommunityHtmlCanvas::draw_borders(litehtml::uint_ptr, const litehtml::borders& borders,
    const litehtml::position& draw_pos, bool)
{
    if (measure(draw_pos) || !m_target)
        return;
    const auto visible = [](const litehtml::border& b)
    {
        return b.width > 0 && b.style != litehtml::border_style_none && b.style != litehtml::border_style_hidden &&
            b.color.alpha > 0;
    };
    const litehtml::border* sides[] = { &borders.left, &borders.top, &borders.right, &borders.bottom };
    const auto same = [&](const litehtml::border& b)
    {
        return b.width == borders.top.width && b.color == borders.top.color && visible(b) == visible(borders.top);
    };

    // one width and colour all round: a stroke that follows the radii; else each side as a band
    if (same(borders.left) && same(borders.right) && same(borders.bottom))
    {
        if (!visible(borders.top))
            return;
        const float w = borders.top.width;
        litehtml::position inner = draw_pos;
        inner.x += w * 0.5f;
        inner.y += w * 0.5f;
        inner.width -= w;
        inner.height -= w;
        litehtml::border_radiuses r = borders.radius;
        for (float* v : { &r.top_left_x, &r.top_left_y, &r.top_right_x, &r.top_right_y, &r.bottom_right_x,
                 &r.bottom_right_y, &r.bottom_left_x, &r.bottom_left_y })
            *v = std::max(*v - w * 0.5f, 0.f);
        m_target->DrawRoundedRectangle(rounded(inner, r), brush(borders.top.color), w);
        return;
    }
    const litehtml::position& p = draw_pos;
    if (visible(*sides[0]))
        m_target->FillRectangle(D2D1::RectF(p.x, p.y, p.x + borders.left.width, p.bottom()), brush(borders.left.color));
    if (visible(*sides[1]))
        m_target->FillRectangle(D2D1::RectF(p.x, p.y, p.right(), p.y + borders.top.width), brush(borders.top.color));
    if (visible(*sides[2]))
        m_target->FillRectangle(D2D1::RectF(p.right() - borders.right.width, p.y, p.right(), p.bottom()), brush(borders.right.color));
    if (visible(*sides[3]))
        m_target->FillRectangle(D2D1::RectF(p.x, p.bottom() - borders.bottom.width, p.right(), p.bottom()), brush(borders.bottom.color));
}

void CommunityHtmlCanvas::transform_text(litehtml::string& text, litehtml::text_transform tt)
{
    if (text.empty() || tt == litehtml::text_transform_none)
        return;
    std::wstring wide = widen(text);
    if (tt == litehtml::text_transform_capitalize)
        CharUpperBuffW(wide.data(), 1);
    else if (tt == litehtml::text_transform_uppercase)
        CharUpperBuffW(wide.data(), DWORD(wide.size()));
    else if (tt == litehtml::text_transform_lowercase)
        CharLowerBuffW(wide.data(), DWORD(wide.size()));
    const xr_string out = narrow(wide);
    text.assign(out.c_str(), out.size());
}

void CommunityHtmlCanvas::set_clip(const litehtml::position& pos, const litehtml::border_radiuses&)
{
    if (m_measuring || !m_target)
        return;
    m_target->PushAxisAlignedClip(rect(pos), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ++m_clips;
}

void CommunityHtmlCanvas::del_clip()
{
    if (m_measuring || !m_target || !m_clips)
        return;
    m_target->PopAxisAlignedClip();
    --m_clips;
}

void CommunityHtmlCanvas::get_viewport(litehtml::position& viewport) const
{
    viewport = litehtml::position(0, 0, m_width, m_height);
}

void CommunityHtmlCanvas::get_media_features(litehtml::media_features& media) const
{
    media.type = litehtml::media_type_screen;
    media.width = media.device_width = m_width;
    media.height = media.device_height = m_height;
    media.color = 8;
    media.color_index = 0;
    media.monochrome = 0;
    media.resolution = 96.f;
}

void CommunityHtmlCanvas::get_language(litehtml::string& language, litehtml::string& culture) const
{
    language = "ru";
    culture.clear();
}
#endif
