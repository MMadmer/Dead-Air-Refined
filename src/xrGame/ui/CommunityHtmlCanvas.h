// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#if defined(XR_PLATFORM_WINDOWS)
// the file system's _import_ (Common/FSMacros.hpp) is one of litehtml's names
#pragma push_macro("_import_")
#undef _import_
#include <litehtml.h>
#pragma pop_macro("_import_")

struct ID2D1Factory;
struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;
struct ID2D1Bitmap;
struct IDWriteFactory;
struct IDWriteFontCollection;
struct IDWriteTextFormat;
struct IWICImagingFactory;
struct IWICBitmap;

// litehtml's drawing for the menu's community panel (UICommunityPanel): Direct2D and DirectWrite into a
// picture in memory, its transparency kept. Lengths are CSS pixels; `scale` screen pixels make one.
// Pictures come only as data: URIs - the page is one document - and links open only to https://.
class CommunityHtmlCanvas final : public litehtml::document_container
{
public:
    CommunityHtmlCanvas();
    ~CommunityHtmlCanvas() override;

    bool Valid() const { return m_d2d && m_dwrite && m_wic; }
    void SetViewport(float width, float height, float scale);

    // The part of the page something is drawn on: the boxes of every draw of `doc`, CSS pixels.
    litehtml::position Measure(const litehtml::document::ptr& doc);

    // The page's part `area` (CSS pixels) into `width` x `height` RGBA pixels, top row first.
    bool Rasterize(const litehtml::document::ptr& doc, const litehtml::position& area, u32 width, u32 height,
        xr_vector<u8>& rgba);

    // litehtml::document_container
    litehtml::uint_ptr create_font(const litehtml::font_description& descr, const litehtml::document* doc,
        litehtml::font_metrics* fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont, litehtml::web_color color,
        const litehtml::position& pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override { return pt * 96.f / 72.f; }
    litehtml::pixel_t get_default_font_size() const override { return 16.f; }
    const char* get_default_font_name() const override { return "Segoe UI"; }
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override;
    void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override;
    void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer, const std::string& url,
        const std::string& base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
        const litehtml::web_color& color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
        const litehtml::background_layer::linear_gradient& gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
        const litehtml::background_layer::radial_gradient& gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
        const litehtml::background_layer::conic_gradient& gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos,
        bool root) override;
    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<litehtml::document>&, const litehtml::element::ptr&) override {}
    // the panel opens the link itself (UICommunityPanel): the button went down and up on it
    void on_anchor_click(const char*, const litehtml::element::ptr&) override {}
    void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(litehtml::string& text, litehtml::text_transform tt) override;
    void import_css(litehtml::string&, const litehtml::string&, litehtml::string&) override {}
    void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position& viewport) const override;
    litehtml::element::ptr create_element(const char*, const litehtml::string_map&,
        const std::shared_ptr<litehtml::document>&) override
    {
        return nullptr;
    }
    void get_media_features(litehtml::media_features& media) const override;
    void get_language(litehtml::string& language, litehtml::string& culture) const override;

private:
    struct Font;
    struct Picture
    {
        IWICBitmap* source{};
        ID2D1Bitmap* bitmap{}; // of the render target being drawn
        u32 width{};
        u32 height{};
    };

    // the box drawn into: measured, or clipped to the layer's rounded border box while `draw` runs
    bool measure(const litehtml::position& box);
    template <typename Draw>
    void in_layer(const litehtml::background_layer& layer, Draw&& draw);
    ID2D1SolidColorBrush* brush(const litehtml::web_color& color);
    ID2D1Bitmap* bitmap(Picture& picture);

    ID2D1Factory* m_d2d{};
    IDWriteFactory* m_dwrite{};
    IDWriteFontCollection* m_fonts{};
    IWICImagingFactory* m_wic{};

    ID2D1RenderTarget* m_target{};
    ID2D1SolidColorBrush* m_brush{};
    u32 m_clips{};

    bool m_measuring{};
    bool m_measured_any{};
    litehtml::position m_measured;

    float m_width{};
    float m_height{};
    float m_scale{ 1.f };

    xr_map<xr_string, Picture> m_pictures;
};
#endif
