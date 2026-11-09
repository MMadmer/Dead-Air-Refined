// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIXmsScreenLayers.h"
#include "CommunityHtmlCanvas.h"

#include "xrEngine/da_media.h"
#include "xrUICore/Static/UIStatic.h"

#include <fstream>

namespace
{
u32 s_serial = 0;

bool read_text(const xr_string& path, xr_string& out)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file)
        return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}
} // namespace

CUIXmsScreenLayers::CUIXmsScreenLayers() : CUIWindow("CUIXmsScreenLayers")
{
    SetWndRect(Frect().set(0.f, 0.f, UI_BASE_WIDTH, UI_BASE_HEIGHT));
}

CUIXmsScreenLayers::~CUIXmsScreenLayers() { Clear(); }

void CUIXmsScreenLayers::Clear()
{
    DetachAll();
    for (const xr_string& id : m_pictures)
        da_media_forget(id.c_str());
    m_pictures.clear();
    m_heard.clear();
    m_module = nullptr;
    m_screen = nullptr;
}

void CUIXmsScreenLayers::Build(const XMS::Module& module, const XMS::ScreenDecl& screen, bool video_sound)
{
    Clear();
    m_module = &module;
    m_screen = &screen;
    m_video_sound = video_sound;
    m_width = Device.dwWidth;
    m_height = Device.dwHeight;
    for (const XMS::ScreenLayer& layer : screen.layers)
        AddLayer(module, layer, video_sound);
}

void CUIXmsScreenLayers::FollowDisplay()
{
    if (m_screen && (m_width != Device.dwWidth || m_height != Device.dwHeight))
        Build(*m_module, *m_screen, m_video_sound);
}

bool CUIXmsScreenLayers::Silent(const CUIWindow* window) const
{
    return m_screen && window->GetParent() == this && std::find(m_heard.begin(), m_heard.end(), window) == m_heard.end();
}

void CUIXmsScreenLayers::AddLayer(const XMS::Module& module, const XMS::ScreenLayer& layer, bool video_sound)
{
    using Layer = XMS::ScreenLayer;
    if (layer.kind == Layer::Empty)
        return;

    Frect place;
    place.set(layer.x * 0.01f * UI_BASE_WIDTH, layer.y * 0.01f * UI_BASE_HEIGHT, (layer.x + layer.w) * 0.01f * UI_BASE_WIDTH,
        (layer.y + layer.h) * 0.01f * UI_BASE_HEIGHT);

    auto* st = xr_new<CUIStatic>("xms screen layer");
    st->SetAutoDelete(true);
    st->SetStretchTexture(true);

    switch (layer.kind)
    {
    case Layer::Texture:
    {
        st->InitTextureEx(layer.name.c_str(), "hud" DELIMITER "default");
        u32 w = 0, h = 0;
        if (layer.fit != Layer::Stretch && da_media_size(layer.name.c_str(), w, h) && w && h)
        {
            const float box_w = place.width() / UI_BASE_WIDTH * float(Device.dwWidth);
            const float box_h = place.height() / UI_BASE_HEIGHT * float(Device.dwHeight);
            if (layer.fit == Layer::Cover)
            {
                const float k = std::max(box_w / float(w), box_h / float(h));
                const float shown_w = box_w / k, shown_h = box_h / k;
                st->SetTextureRect(Frect().set((float(w) - shown_w) * 0.5f, (float(h) - shown_h) * 0.5f, (float(w) + shown_w) * 0.5f,
                    (float(h) + shown_h) * 0.5f));
            }
            else
            {
                const float k = std::min(box_w / float(w), box_h / float(h));
                const float inset_x = (box_w - float(w) * k) * 0.5f / float(Device.dwWidth) * UI_BASE_WIDTH;
                const float inset_y = (box_h - float(h) * k) * 0.5f / float(Device.dwHeight) * UI_BASE_HEIGHT;
                place.set(place.left + inset_x, place.top + inset_y, place.right - inset_x, place.bottom - inset_y);
                st->SetTextureRect(Frect().set(0.f, 0.f, float(w), float(h)));
            }
        }
        if (video_sound && layer.sound)
            m_heard.push_back(st);
        break;
    }
    case Layer::Shader:
        st->InitTextureEx(layer.base.empty() ? "ui" DELIMITER "ui_common" : layer.base.c_str(), layer.name.c_str());
        break;
    case Layer::Page:
        if (!AddPage(module, layer, st, place))
        {
            xr_delete(st);
            return;
        }
        break;
    default: break;
    }

    st->SetWndRect(place);
    st->SetTextureColor(color_argb(iFloor(layer.opacity * 255.f + 0.5f), 255, 255, 255));
    AttachChild(st);
}

bool CUIXmsScreenLayers::AddPage(const XMS::Module& module, const XMS::ScreenLayer& layer, CUIStatic* st, const Frect& place)
{
#if defined(XR_PLATFORM_WINDOWS)
    const u32 w = u32(iCeil(place.width() / UI_BASE_WIDTH * float(Device.dwWidth)));
    const u32 h = u32(iCeil(place.height() / UI_BASE_HEIGHT * float(Device.dwHeight)));
    if (!w || !h)
        return false;

    // a page of the player's language first: night.eng.html next to night.html
    const xr_string whole = module.root + layer.file;
    const size_t dot = whole.rfind('.');
    xr_string text;
    if (!read_text(whole.substr(0, dot) + "." + StringTable().GetCurrentLanguage().c_str() + whole.substr(dot), text) && !read_text(whole, text))
    {
        Msg("! [screens] %s: the page cannot be read", whole.c_str());
        return false;
    }

    CommunityHtmlCanvas canvas;
    if (!canvas.Valid())
        return false;
    // a CSS pixel is a pixel of a 1080-line screen, as on the menu's community panel
    const float scale = float(Device.dwHeight) / 1080.f;
    const float css_w = float(w) / scale, css_h = float(h) / scale;
    canvas.SetViewport(css_w, css_h, scale);
    canvas.SetLocalFolder(whole.substr(0, whole.rfind('\\') + 1).c_str());
    const litehtml::document::ptr doc = litehtml::document::createFromString(litehtml::estring(text.c_str()), &canvas);
    if (!doc)
    {
        Msg("! [screens] %s: the page does not parse", whole.c_str());
        return false;
    }
    doc->render(css_w);

    xr_vector<u8> rgba;
    if (!canvas.Rasterize(doc, litehtml::position(0.f, 0.f, css_w, css_h), w, h, rgba))
        return false;

    string64 id;
    xr_sprintf(id, "xms_screen_%u_%ux%u", ++s_serial, w, h);
    da_media_publish(id, w, h, rgba.data());
    m_pictures.emplace_back(id);
    st->CreateShader((xr_string(DA_MEDIA_PICTURE) + id).c_str(), "hud" DELIMITER "default");
    return true;
#else
    return false;
#endif
}
