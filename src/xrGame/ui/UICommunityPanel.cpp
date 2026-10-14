// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UICommunityPanel.h"
#include "RemoteConfig.h"

int g_dar_community_links = 1;

#if defined(XR_PLATFORM_WINDOWS)
#include "CommunityHtmlCanvas.h"
#include "xrEngine/da_media.h"
#include "xrUICore/Cursor/UICursor.h"
#include "xrUICore/Static/UIStaticItem.h"

#include <shellapi.h>

#include <thread>

namespace
{
// The page's height in CSS pixels on any screen: positions stay where they are as the resolution changes.
constexpr float PageHeight = 768.f;

// the address of the link `element` is in, empty when none
xr_string link_of(litehtml::element::ptr element)
{
    for (; element; element = element->parent())
    {
        const char* tag = element->get_tagName();
        if (tag && !xr_stricmp(tag, "a"))
        {
            const char* href = element->get_attr("href");
            return href ? href : "";
        }
    }
    return {};
}

// The browser, for https:// only: the page cannot run a program or open a file.
void open_in_browser(const xr_string& url)
{
    if (url.compare(0, 8, "https://") || url.find_first_of(" \"<>\\^`{|}") != xr_string::npos)
    {
        Msg("! [community] a link that is not an https:// address is not opened: %s", url.c_str());
        return;
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), int(url.size()), nullptr, 0);
    std::wstring wide(size_t(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), int(url.size()), wide.data(), length);
    Msg("* [community] opening %s", url.c_str());
    // the shell may take a while to start a browser: not on the frame
    std::thread([wide]
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com))
            CoUninitialize();
    }).detach();
}
} // namespace

struct CUICommunityPanel::Impl
{
    std::unique_ptr<CommunityHtmlCanvas> canvas;
    litehtml::document::ptr doc;
    u32 revision{ u32(-1) };

    // the screen the page was laid out for, and its scale: screen pixels to a CSS pixel
    u32 screen_w{}, screen_h{};
    float scale{ 1.f };
    float page_w{};

    bool dirty{ true };
    Irect pixels{}; // where the picture lies on the screen
    xr_string id; // its name for da_media, a new one for a new size
    u32 serial{};
    CUIStaticItem item;
    bool shown{};

    Fvector2 cursor{ -1.f, -1.f }; // CSS pixels
    bool over{};
    xr_string pressed; // the link the left button went down on

    ~Impl() { drop_picture(); }

    void drop_picture()
    {
        if (id.empty())
            return;
        item.GetShader()->destroy();
        da_media_forget(id.c_str());
        id.clear();
        shown = false;
    }

    // the page's viewport for this screen; litehtml reads it once, so a page made before it sees it again
    void viewport()
    {
        screen_w = Device.dwWidth;
        screen_h = Device.dwHeight;
        scale = float(screen_h) / PageHeight;
        page_w = float(screen_w) / scale;
        canvas->SetViewport(page_w, PageHeight, scale);
    }

    void layout()
    {
        viewport();
        doc->media_changed();
        doc->render(page_w);
        dirty = true;
    }

    // the page anew when RemoteConfig has another; nothing when it has none
    void take_page()
    {
        if (RemoteConfig::Revision(RemoteConfig::Document::Menu) == revision)
            return;
        u32 now = 0;
        const xr_string text = RemoteConfig::Text(RemoteConfig::Document::Menu, &now);
        revision = now;
        doc.reset();
        drop_picture();
        pressed.clear();
        over = false;
        cursor.set(-1.f, -1.f); // what the resting cursor is over is asked anew
        if (text.empty())
            return;
        if (!canvas)
            canvas = std::make_unique<CommunityHtmlCanvas>();
        if (!canvas->Valid())
            return;
        viewport();
        doc = litehtml::document::createFromString(litehtml::estring(text.c_str()), canvas.get());
        if (!doc)
        {
            Msg("! [community] the page does not parse");
            return;
        }
        doc->render(page_w);
        dirty = true;
    }

    void hover(const Fvector2& at)
    {
        if (at.similar(cursor, EPS_L))
            return;
        cursor = at;
        litehtml::position::vector boxes;
        if (doc->on_mouse_over(at.x, at.y, at.x, at.y, boxes))
        {
            doc->render(page_w);
            dirty = true;
        }
        over = !link_of(std::const_pointer_cast<litehtml::element>(doc->get_over_element())).empty();
    }

    void paint()
    {
        dirty = false;
        // what is drawn, within the screen
        litehtml::position area = canvas->Measure(doc);
        const float left = std::max(area.x, 0.f), top = std::max(area.y, 0.f);
        const float right = std::min(area.right(), page_w), bottom = std::min(area.bottom(), PageHeight);
        area = litehtml::position(left, top, right - left, bottom - top);
        if (area.width <= 0 || area.height <= 0)
        {
            drop_picture();
            return;
        }
        // whole screen pixels round what is drawn
        const Irect px{ iFloor(area.x * scale), iFloor(area.y * scale), iCeil(area.right() * scale),
            iCeil(area.bottom() * scale) };
        const u32 w = u32(px.width()), h = u32(px.height());
        const litehtml::position snapped(px.x1 / scale, px.y1 / scale, w / scale, h / scale);
        xr_vector<u8> rgba;
        if (!canvas->Rasterize(doc, snapped, w, h, rgba))
        {
            drop_picture();
            return;
        }

        string64 name;
        xr_sprintf(name, "community_%u_%ux%u", serial, w, h);
        if (id != name)
        {
            drop_picture();
            ++serial;
            xr_sprintf(name, "community_%u_%ux%u", serial, w, h);
            id = name;
            da_media_publish(id.c_str(), w, h, rgba.data());
            item.CreateShader((xr_string(DA_MEDIA_PICTURE) + id).c_str(), "hud" DELIMITER "default");
        }
        else
            da_media_publish(id.c_str(), w, h, rgba.data());
        pixels = px;
        shown = true;
    }
};

CUICommunityPanel::CUICommunityPanel() : m(std::make_unique<Impl>()) {}
CUICommunityPanel::~CUICommunityPanel() = default;

void CUICommunityPanel::Draw()
{
    m->take_page();
    if (!m->doc)
        return;
    if (m->screen_w != Device.dwWidth || m->screen_h != Device.dwHeight)
        m->layout();

    // the cursor on the page: the interface's units to screen pixels to CSS pixels
    const Fvector2 ui = GetUICursor().GetCursorPosition();
    Fvector2 screen;
    UI().ClientToScreenScaled(screen, ui.x, ui.y);
    m->hover({ screen.x / m->scale, screen.y / m->scale });

    if (m->dirty)
        m->paint();
    if (!m->shown)
        return;

    // the picture pixel for pixel: its screen rectangle back into the interface's units
    Fvector2 unit;
    UI().ClientToScreenScaled(unit, 1.f, 1.f);
    m->item.SetTextureRect(Frect().set(0.f, 0.f, float(m->pixels.width()), float(m->pixels.height())));
    m->item.SetPos(m->pixels.x1 / unit.x, m->pixels.y1 / unit.y);
    m->item.SetSize(Fvector2{ m->pixels.width() / unit.x, m->pixels.height() / unit.y });
    m->item.SetTextureColor(0xFFFFFFFF);
    m->item.Render();
}

void CUICommunityPanel::Hide()
{
    if (!m->doc)
        return;
    litehtml::position::vector boxes;
    if (m->doc->on_mouse_leave(boxes))
    {
        m->doc->render(m->page_w);
        m->dirty = true;
    }
    m->cursor.set(-1.f, -1.f);
    m->over = false;
    m->pressed.clear();
}

bool CUICommunityPanel::OnMousePress()
{
    if (!m->doc || !m->shown || !m->over)
        return false;
    litehtml::position::vector boxes;
    if (m->doc->on_lbutton_down(m->cursor.x, m->cursor.y, m->cursor.x, m->cursor.y, boxes))
    {
        m->doc->render(m->page_w);
        m->dirty = true;
    }
    m->pressed = link_of(std::const_pointer_cast<litehtml::element>(m->doc->get_over_element()));
    return true;
}

bool CUICommunityPanel::OnMouseRelease()
{
    if (!m->doc || m->pressed.empty())
        return false;
    litehtml::position::vector boxes;
    if (m->doc->on_lbutton_up(m->cursor.x, m->cursor.y, m->cursor.x, m->cursor.y, boxes))
    {
        m->doc->render(m->page_w);
        m->dirty = true;
    }
    // the link the button went down and up on, anywhere on it
    if (link_of(std::const_pointer_cast<litehtml::element>(m->doc->get_over_element())) == m->pressed)
        open_in_browser(m->pressed);
    m->pressed.clear();
    return true;
}
#else
struct CUICommunityPanel::Impl
{
};
CUICommunityPanel::CUICommunityPanel() : m(std::make_unique<Impl>()) {}
CUICommunityPanel::~CUICommunityPanel() = default;
void CUICommunityPanel::Draw() {}
void CUICommunityPanel::Hide() {}
bool CUICommunityPanel::OnMousePress() { return false; }
bool CUICommunityPanel::OnMouseRelease() { return false; }
#endif
