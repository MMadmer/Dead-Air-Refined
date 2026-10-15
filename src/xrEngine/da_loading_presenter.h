// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include <memory>

// The loading screen's own presenter, the way the movie player of a modern engine shows one: a thread with a
// device and a window of its own, laid over the game's window, draws the loading screen at the display's pace
// whatever the game does meanwhile - a level's long steps, a device reset, a script that takes seconds. The game
// lays the screen out as always (ui_mm_loading_screen*.xml, the built-in layouts, a mod's); what does not move it
// draws itself into pictures (IRender::RenderUIPicture), what moves - a video with its sound, a GIF, a Theora
// video, a shader of a mod's, the bar, the per cent - is drawn here. Where no window can lie over the game's
// (exclusive fullscreen) and with loading_screen_async 0, the game draws the loading screen itself between its
// steps.

// straight-alpha RGBA, rows of the rectangle's width
using DaPicture = std::shared_ptr<const xr_vector<u32>>;

struct DaLoadingLayer
{
    enum class Kind : u8
    {
        Picture, // drawn by the game: the pixels of rect
        Media, // a video (with its sound), a GIF or a Theora video of `texture`; `pixels` until it plays
        Shader, // a static drawn by a mod's shader `shader` over `texture`; `pixels` where it cannot be built here
        Fill, // the progress bar's fill at 100 %: shown up to the progress, cut the way the bar cuts it
        Percent, // the per cent text: `percents`, the pictures of 0..100 %
    };

    Kind kind{};
    u32 id{}; // the same item across the publishes of one loading
    u32 revision{}; // its pictures changed
    Irect rect{}; // screen pixels: left, top, right, bottom
    DaPicture pixels;
    shared_str texture;
    shared_str shader;
    Fvector4 uv{ 0.f, 0.f, 1.f, 1.f };
    u32 color{ 0xFFFFFFFF };

    // Fill: the bar's rectangle in screen pixels, its orientation (CUIProgressBar::EOrientMode), and a tint by the
    // progress (from min through middle to max) where the bar has one
    Frect bar{};
    u8 orient{};
    bool tinted{};
    bool tint_middle{};
    Fcolor tint_min{}, tint_mid{}, tint_max{};

    // Percent: 101 pictures of rect
    std::shared_ptr<const xr_vector<DaPicture>> percents;
};

using DaLoadingScene = xr_vector<DaLoadingLayer>;

extern ENGINE_API int psLoadingScreenAsync;
// loading_screen_capture <frames>: the presenter saves its own frames (QA)
extern ENGINE_API int psLoadingScreenCapture;

// The presenter can show the loading screen now: it is on, and there is a window to lie over.
ENGINE_API bool da_loading_presenter_usable();
// The layers of the screen, from the main thread; the first publish of a loading shows the window.
ENGINE_API void da_loading_presenter_publish(const DaLoadingScene& scene);
// The loading's progress, 0..1, as it comes.
ENGINE_API void da_loading_presenter_progress(float progress);
// The game shows its own frames again: the window goes, a video's sound fades out.
ENGINE_API void da_loading_presenter_hide();
ENGINE_API bool da_loading_presenter_shown();
// The application's end: the thread and its device go.
void da_loading_presenter_shutdown();
