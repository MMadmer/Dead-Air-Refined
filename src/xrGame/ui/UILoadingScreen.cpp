////////////////////////////////////////////////////////////////////////////
//  Created     : 19.06.2018
//  Authors     : Xottab_DUTY (OpenXRay project)
//                FozeSt
//                Unfainthful
//
//  Copyright (C) GSC Game World - 2018
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "UILoadingScreen.h"
#include "UILoadingScreenHardcoded.h"

#include "UIHelper.h"
#include "xrUICore/XML/UITextureMaster.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"
#include "xrEngine/da_media.h"
#include "xrEngine/da_loading_presenter.h"

UILoadingScreen::UILoadingScreen()
    : CUIWindow("UILoadingScreen"),
      loadingProgress(nullptr), loadingProgressPercent(nullptr),
      loadingLogo(nullptr),     loadingStage(nullptr),
      loadingHeader(nullptr),   loadingTipNumber(nullptr), loadingTip(nullptr)
{
    alwaysShowStage = false;
    UILoadingScreen::Initialize();
}

void UILoadingScreen::Initialize()
{
    CUIXml uiXml;
    const bool loaded = uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_mm_loading_screen.xml", false);

    // It was hardcoded even harder anyway. (search in history for deleted dxApplicationRender class)
    // Hardcoded XML is more flexible, so:
    if (!loaded) // then just use preset we have
    {
        // First, process textures description for loading screen (just in case)
        uiXml.Set(GetLoadingScreenTexturesDescr());
        CUITextureMaster::ParseShTexInfo(uiXml, false);
        uiXml.ClearInternal(); // cleanup

        // And then, set loading screen itself
        uiXml.Set(GetLoadingScreenXML());
    }

    pcstr xmlNode = "background";

    string64 temp;
    strconcat(temp, "background_", StringTable().GetCurrentLanguage().c_str());
    if (uiXml.NavigateToNode(temp))
        xmlNode = temp;

    // A layout may leave the bar out and show the progress its own way: a shader of its background
    // reads xms_ui_progress.
    if (uiXml.ReadAttribInt("loading_progress", 0, "under_background", 1))
    {
        loadingProgress = UIHelper::CreateProgressBar(uiXml, "loading_progress", this, false);
        CUIXmlInit::InitWindow(uiXml, xmlNode, 0, this);
    }
    else
    {
        CUIXmlInit::InitWindow(uiXml, xmlNode, 0, this);
        loadingProgress = UIHelper::CreateProgressBar(uiXml, "loading_progress", this, false);
    }

    alwaysShowStage = uiXml.ReadAttribInt("loading_stage", 0, "always_show");

    xmlNode = "loading_logo";
    strconcat(temp, "loading_logo_", StringTable().GetCurrentLanguage().c_str());
    if (uiXml.NavigateToNode(temp))
        xmlNode = temp;

    loadingLogo = UIHelper::CreateStatic(uiXml, xmlNode, this, false);

    loadingProgressPercent = UIHelper::CreateStatic(uiXml, "loading_progress_percent", this, false);
    loadingStage = UIHelper::CreateStatic(uiXml, "loading_stage", this, false);
    loadingHeader = UIHelper::CreateStatic(uiXml, "loading_header", this, false);
    loadingTipNumber = UIHelper::CreateStatic(uiXml, "loading_tip_number", this, false);
    loadingTip = UIHelper::CreateStatic(uiXml, "loading_tip", this, false);
}

void UILoadingScreen::Update(const int stagesCompleted, const int stagesTotal)
{
    ScopeLock scope(&loadingLock);

    g_da_ui_shader.progress = stagesTotal > 0 ? std::clamp(float(stagesCompleted) / float(stagesTotal), 0.f, 1.f) : 0.f;
    da_loading_presenter_progress(g_da_ui_shader.progress);
    CUIWindow::Update();
}

void UILoadingScreen::Draw()
{
    ScopeLock scope(&loadingLock);

    // The loading moves in steps; what shows it eases after them (half the way in about 0.1 s), in
    // every frame drawn, between the steps too (IGame_Persistent::LoadPump).
    const u32 now = da_media_time();
    const float dt = std::min(float(now - m_eased_at) * 0.001f, 0.25f);
    m_eased_at = now;
    float& eased = g_da_ui_shader.progress_eased;
    eased += (g_da_ui_shader.progress - eased) * (1.f - std::exp(-dt * 7.f));
    if (loadingProgress)
        loadingProgress->ForceSetProgressPos(eased * loadingProgress->GetRange_max());
    if (loadingProgressPercent)
    {
        string16 buf;
        xr_sprintf(buf, "%.0f%%", eased * 100.f);
        loadingProgressPercent->SetText(buf);
    }

    CUIWindow::Draw();
}

void UILoadingScreen::SetLevelLogo(const char* name)
{
    ScopeLock scope(&loadingLock);

    if (loadingLogo)
        loadingLogo->InitTexture(name);
    m_dirty = true;
}

void UILoadingScreen::SetStageTitle(const char* title)
{
    // Only if enabled by user or forced to be displayed by XML
    // And if exist at all
    if ((psActorFlags.test(AF_LOADING_STAGES) || alwaysShowStage) && loadingStage)
    {
        ScopeLock scope(&loadingLock);

        loadingStage->SetText(title);
        m_dirty = true;
    }
}

void UILoadingScreen::SetStageTip(const char* header, const char* tipNumber, const char* tip)
{
    ScopeLock scope(&loadingLock);

    if (loadingHeader)
        loadingHeader->SetText(header);
    if (loadingTipNumber)
        loadingTipNumber->SetText(tipNumber);
    if (loadingTip)
        loadingTip->SetText(tip);
    m_dirty = true;
}

void UILoadingScreen::Show(bool show)
{
    // A loading starts its bar and its shaders' clock from nothing.
    if (show && !IsShown())
    {
        g_da_ui_shader.opened = m_eased_at = da_media_time();
        g_da_ui_shader.progress = g_da_ui_shader.progress_eased = 0.f;
        da_loading_presenter_progress(0.f);
        m_published.clear();
        m_dirty = true;
    }
    CUIWindow::Show(show);
    if (!show)
    {
        // the game's frames are the screen again
        da_loading_presenter_hide();
        if (loadingLogo)
            loadingLogo->GetStaticItem()->GetShader()->destroy();
        if (loadingStage)
            loadingStage->SetText(nullptr);
        SetStageTip(nullptr, nullptr, nullptr);
    }
}

bool UILoadingScreen::IsShown() const
{
    return CUIWindow::IsShown();
}

namespace
{
// One thing the loading screen draws, in its order: what the game draws into a picture, or what the presenter
// draws itself because it moves.
struct ScreenItem
{
    DaLoadingLayer::Kind kind{};
    bool text{}; // a static's text: a picture of its own, made again when the text changes
    std::function<void()> draw;
    Frect window{}; // UI units
    CUIStatic* st{};
    CUIProgressBar* bar{};
    xr_string signature;
};

Irect to_screen(const Frect& r)
{
    Fvector2 lt, rb;
    UI().ClientToScreenScaled(lt, r.left, r.top);
    UI().ClientToScreenScaled(rb, r.right, r.bottom);
    Irect out;
    out.set(iFloor(lt.x), iFloor(lt.y), iCeil(rb.x), iCeil(rb.y));
    return out;
}

xr_string describe(const Frect& r, u32 color, pcstr what)
{
    string512 buf;
    xr_sprintf(buf, "%s %.2f %.2f %.2f %.2f %08x;", what, r.left, r.top, r.right, r.bottom, color);
    return buf;
}

// A static's texture that moves - a video, a GIF, a Theora video - or one drawn by a shader of a mod's.
DaLoadingLayer::Kind texture_kind(CUIStatic* st)
{
    const CUIStaticItem* item = st->GetStaticItem();
    if (!item->TextureName().size())
        return DaLoadingLayer::Kind::Picture;
    const pcstr shader = item->ShaderName().c_str();
    if (shader && xr_strcmp(shader, "hud" DELIMITER "default") && xr_strcmp(shader, "hud" DELIMITER "movie"))
        return DaLoadingLayer::Kind::Shader;
    string_path fn;
    switch (da_media_resolve(item->TextureName().c_str(), fn))
    {
    case DaMediaKind::Gif:
    case DaMediaKind::Video:
    case DaMediaKind::Theora: return DaLoadingLayer::Kind::Media;
    default: return DaLoadingLayer::Kind::Picture;
    }
}

void collect(CUIWindow* parent, CUIStatic* percent, xr_vector<ScreenItem>& items)
{
    for (CUIWindow* w : parent->GetChildWndList())
    {
        if (!w->IsShown() || w->GetCustomDraw())
            continue;
        Frect r;
        w->GetAbsoluteRect(r);

        // A bar draws its track and its fill and nothing of its children (CUIProgressBar::Draw).
        if (auto* bar = smart_cast<CUIProgressBar*>(w))
        {
            items.push_back({ DaLoadingLayer::Kind::Picture, false,
                [bar] {
                    if (!bar->IsShownBackground())
                        return;
                    Frect b;
                    bar->GetAbsoluteRect(b);
                    UI().PushScissor(b);
                    bar->m_UIBackgroundItem.Draw();
                    UI().PopScissor();
                },
                r, nullptr, bar, describe(r, bar->m_UIBackgroundItem.GetTextureColor(), "track") });
            Frect fill;
            bar->m_UIProgressItem.GetAbsoluteRect(fill);
            items.push_back({ DaLoadingLayer::Kind::Fill, false,
                [bar] {
                    // the whole fill, untinted: the presenter cuts and tints it by the progress
                    const u32 color = bar->m_UIProgressItem.GetTextureColor();
                    Fcolor a, b, c;
                    bool middle;
                    if (bar->GetFillTint(a, b, c, middle))
                        bar->m_UIProgressItem.SetTextureColor(0xFFFFFFFF);
                    bar->m_UIProgressItem.Draw();
                    bar->m_UIProgressItem.SetTextureColor(color);
                },
                fill, nullptr, bar, describe(fill, 0, "fill") });
            continue;
        }

        if (auto* st = smart_cast<CUIStatic*>(w))
        {
            items.push_back({ texture_kind(st), false, [st] { st->DrawTexture(); }, r, st, nullptr,
                describe(r, st->GetTextureColor(), st->GetStaticItem()->TextureName().c_str()) });
            collect(st, percent, items);
            items.push_back({ st == percent ? DaLoadingLayer::Kind::Percent : DaLoadingLayer::Kind::Picture, true,
                [st] {
                    st->DrawText();
                    UI().RenderFont();
                },
                r, st, nullptr, describe(r, st->GetTextColor(), st->GetText() ? st->GetText() : "") });
            continue;
        }

        if (w->GetShapeSkin())
            items.push_back({ DaLoadingLayer::Kind::Picture, false, [w] { w->DrawShapeSkinAlone(); }, r, nullptr, nullptr,
                describe(r, 0, "skin") });
        collect(w, percent, items);
    }
}

DaPicture rasterize(const Irect& rect, const std::function<void()>& draw)
{
    auto pixels = std::make_shared<xr_vector<u32>>();
    if (!GEnv.Render->RenderUIPicture(rect, draw, *pixels))
        return {};
    return pixels;
}

Irect clamp_to_screen(Irect r)
{
    r.set(std::max(r.left, 0), std::max(r.top, 0), std::min(r.right, int(Device.dwWidth)), std::min(r.bottom, int(Device.dwHeight)));
    return r;
}

bool still(const ScreenItem& item) { return item.kind == DaLoadingLayer::Kind::Picture && !item.text; }
} // namespace

// The screen's layers for the loading screen's own presenter: a run of what does not move drawn by the game into
// one picture, a text into a picture of its own (it changes), what moves handed over as it is. A layer keeps its
// place, and its picture while its description stays the same.
bool UILoadingScreen::Publish()
{
    ScopeLock scope(&loadingLock);
    m_dirty = false;

    xr_vector<ScreenItem> items;
    if (GetShapeSkin())
    {
        Frect r;
        GetAbsoluteRect(r);
        items.push_back({ DaLoadingLayer::Kind::Picture, false, [this] { DrawShapeSkinAlone(); }, r, nullptr, nullptr,
            describe(r, 0, "skin") });
    }
    collect(this, loadingProgressPercent, items);

    // the layers: items [first, last), a run of still pictures one, anything else one of its own
    xr_vector<std::pair<size_t, size_t>> runs;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (still(items[i]) && !runs.empty() && runs.back().second == i && still(items[runs.back().first]))
            runs.back().second = i + 1;
        else
            runs.emplace_back(i, i + 1);
    }
    if (m_published.size() != runs.size())
        m_published.assign(runs.size(), {});

    DaLoadingScene scene;
    scene.reserve(runs.size());
    for (size_t l = 0; l < runs.size(); ++l)
    {
        const auto [first, last] = runs[l];
        const ScreenItem& head = items[first];
        DaLoadingLayer layer;
        layer.kind = head.kind;
        layer.id = u32(l);

        xr_string signature;
        Frect window = head.window;
        for (size_t i = first; i < last; ++i)
        {
            signature += items[i].signature;
            const Frect& w = items[i].window;
            window.set(std::min(window.left, w.left), std::min(window.top, w.top), std::max(window.right, w.right),
                std::max(window.bottom, w.bottom));
        }
        Irect rect = to_screen(window);
        // a text may reach past its window: a margin round it, a few lines below
        if (head.text)
            rect.set(rect.left - 8, rect.top - 8, rect.right + 8, rect.bottom + std::max(48, rect.height()));
        rect = clamp_to_screen(rect);
        layer.rect = rect;

        Published& was = m_published[l];
        const bool same = was.signature == signature && (was.pixels || was.percents);
        if (!same)
        {
            was.signature = signature;
            was.revision = ++m_revisions;
            was.pixels.reset();
            was.percents.reset();
            if (head.kind == DaLoadingLayer::Kind::Percent)
            {
                // every per cent the bar can show, drawn by the game once
                auto pictures = std::make_shared<xr_vector<DaPicture>>();
                pictures->reserve(101);
                const xr_string text = head.st->GetText() ? head.st->GetText() : "";
                for (int p = 0; p <= 100; ++p)
                {
                    string16 buf;
                    xr_sprintf(buf, "%d%%", p);
                    head.st->SetText(buf);
                    pictures->push_back(rasterize(rect, head.draw));
                }
                head.st->SetText(text.c_str());
                was.percents = pictures;
            }
            else
            {
                const auto draw = [&items, first, last] {
                    for (size_t i = first; i < last; ++i)
                        items[i].draw();
                };
                was.pixels = rasterize(rect, draw);
                if (!was.pixels && rect.width() > 0 && rect.height() > 0)
                    return false;
            }
        }
        layer.revision = was.revision;
        layer.pixels = was.pixels;
        layer.percents = was.percents;

        if (head.kind == DaLoadingLayer::Kind::Media || head.kind == DaLoadingLayer::Kind::Shader)
        {
            // Where and how the static draws its texture (CUIStaticItem::RenderInternal says it, into the
            // interface's shader state, as the picture above was drawn) and what of: the presenter draws it the same.
            if (!same)
            {
                was.uv = g_da_ui_shader.uv;
                was.drawn.set(iFloor(g_da_ui_shader.rect.x), iFloor(g_da_ui_shader.rect.y), iCeil(g_da_ui_shader.rect.z),
                    iCeil(g_da_ui_shader.rect.w));
            }
            const CUIStaticItem* item = head.st->GetStaticItem();
            layer.texture = item->TextureName();
            layer.shader = item->ShaderName();
            layer.color = item->GetTextureColor();
            layer.uv = was.uv;
            layer.rect = was.drawn;
        }
        else if (head.kind == DaLoadingLayer::Kind::Fill)
        {
            Frect bar;
            head.bar->GetAbsoluteRect(bar);
            const Irect b = to_screen(bar);
            layer.bar.set(float(b.left), float(b.top), float(b.right), float(b.bottom));
            layer.orient = head.bar->GetOrientMode();
            layer.tinted = head.bar->GetFillTint(layer.tint_min, layer.tint_mid, layer.tint_max, layer.tint_middle);
        }
        scene.push_back(std::move(layer));
    }

    da_loading_presenter_publish(scene);
    return true;
}
