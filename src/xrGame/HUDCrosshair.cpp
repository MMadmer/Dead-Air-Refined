// HUDCrosshair.cpp:  крестик прицела, отображающий текущую дисперсию
//
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"

#include "HUDCrosshair.h"
#include "xrUICore/ui_base.h"

CHUDCrosshair::CHUDCrosshair()
{
    hShader->create("hud" DELIMITER "crosshair");
    radius = 0;
}

CHUDCrosshair::~CHUDCrosshair() {}
void CHUDCrosshair::Load()
{
    //все размеры в процентах от длины экрана
    //длина крестика
    cross_length_perc = pSettings->r_float(HUD_CURSOR_SECTION, "cross_length");
    min_radius_perc = pSettings->r_float(HUD_CURSOR_SECTION, "min_radius");
    max_radius_perc = pSettings->r_float(HUD_CURSOR_SECTION, "max_radius");
    cross_color = pSettings->r_fcolor(HUD_CURSOR_SECTION, "cross_color").get();
}

//выставляет radius от min_radius до max_radius
void CHUDCrosshair::SetDispersion(float disp)
{
    Fvector4 r;
    Fvector R = {VIEWPORT_NEAR * _sin(disp), 0.f, VIEWPORT_NEAR};
    Device.mProject.transform(r, R);

    Fvector2 scr_size{ float(Device.dwWidth), float(Device.dwHeight) };
    float radius_pixels = _abs(r.x) * scr_size.x / 2.0f;
    target_radius = radius_pixels;
}

#ifdef DEBUG
void CHUDCrosshair::SetFirstBulletDispertion(float fbdisp)
{
    Fvector4 r;
    Fvector R = {VIEWPORT_NEAR * _sin(fbdisp), 0.f, VIEWPORT_NEAR};
    Device.mProject.transform(r, R);

    Fvector2 scr_size{ float(Device.dwWidth), float(Device.dwHeight) };
    fb_radius = _abs(r.x) * scr_size.x / 2.0f;
}

BOOL g_bDrawFirstBulletCrosshair = FALSE;

void CHUDCrosshair::OnRenderFirstBulletDispertion()
{
    VERIFY(g_bRendering);

    Fvector2 scr_size{ float(Device.dwWidth), float(Device.dwHeight) };
    Fvector2 center{ scr_size.x / 2.0f, scr_size.y / 2.0f };

    GEnv.UIRender->StartPrimitive(10, IUIRender::ptLineList, UI().m_currentPointType);

    u32 fb_cross_color = color_rgba(255, 0, 0, 255); // red

    float cross_length = /*cross_length_perc*/ 0.008f * scr_size.x;
    float min_radius = min_radius_perc * scr_size.x;
    float max_radius = max_radius_perc * scr_size.x;

    clamp(target_radius, min_radius, max_radius);

    float x_min = min_radius + fb_radius;
    float x_max = x_min + cross_length;

    float y_min = x_min;
    float y_max = x_max;

    // 0
    GEnv.UIRender->PushPoint(center.x, center.y + y_min, 0, fb_cross_color, 0, 0);
    GEnv.UIRender->PushPoint(center.x, center.y + y_max, 0, fb_cross_color, 0, 0);
    // 1
    GEnv.UIRender->PushPoint(center.x, center.y - y_min, 0, fb_cross_color, 0, 0);
    GEnv.UIRender->PushPoint(center.x, center.y - y_max, 0, fb_cross_color, 0, 0);
    // 2
    GEnv.UIRender->PushPoint(center.x + x_min, center.y, 0, fb_cross_color, 0, 0);
    GEnv.UIRender->PushPoint(center.x + x_max, center.y, 0, fb_cross_color, 0, 0);
    // 3
    GEnv.UIRender->PushPoint(center.x - x_min, center.y, 0, fb_cross_color, 0, 0);
    GEnv.UIRender->PushPoint(center.x - x_max, center.y, 0, fb_cross_color, 0, 0);

    // point
    GEnv.UIRender->PushPoint(center.x - 0.5f, center.y, 0, fb_cross_color, 0, 0);
    GEnv.UIRender->PushPoint(center.x + 0.5f, center.y, 0, fb_cross_color, 0, 0);

    // render
    GEnv.UIRender->SetShader(*hShader);
    GEnv.UIRender->FlushPrimitive();
}
#endif

extern ENGINE_API bool g_bRendering;
void CHUDCrosshair::OnRender()
{
    VERIFY(g_bRendering);

    Fvector2 scr_size{ float(Device.dwWidth), float(Device.dwHeight) };
    Fvector2 center{ scr_size.x / 2.0f, scr_size.y / 2.0f };

    // hud_crosshair_dot replaces the four strokes with a centre dot, plus a ring that carries
    // the spread the strokes would have shown. With the dynamic crosshair off the spread never
    // moves, so the ring says nothing and is left out.
    const bool dot_reticle = !!psHUD_Flags.test(HUD_CROSSHAIR_DOT);

    float cross_length = cross_length_perc * scr_size.x;
    float min_radius = min_radius_perc * scr_size.x;
    float max_radius = max_radius_perc * scr_size.x;

    clamp(target_radius, min_radius, max_radius);

    float x_min = min_radius + radius;
    float x_max = x_min + cross_length;

    float y_min = x_min;
    float y_max = x_max;

    if (dot_reticle)
    {
        constexpr u32 dot_segments = 16;
        constexpr u32 ring_segments = 48;

        // A line list would give a one pixel dash, so the dot is a triangle fan. Its radius
        // follows the screen width and never falls below a pixel, otherwise it disappears at
        // high modes. Half alpha keeps it from covering what the player is aiming at.
        const float dot_radius = _max(1.0f, scr_size.x * 0.0008f);
        const u32 dot_color = subst_alpha(cross_color, 128);

        GEnv.UIRender->StartPrimitive(dot_segments * 3, IUIRender::ptTriList, UI().m_currentPointType);

        for (u32 segment = 0; segment < dot_segments; ++segment)
        {
            const float from = PI_MUL_2 * float(segment) / float(dot_segments);
            const float to = PI_MUL_2 * float(segment + 1) / float(dot_segments);

            GEnv.UIRender->PushPoint(center.x, center.y, 0, dot_color, 0, 0);
            GEnv.UIRender->PushPoint(
                center.x + dot_radius * _cos(from), center.y + dot_radius * _sin(from), 0, dot_color, 0, 0);
            GEnv.UIRender->PushPoint(
                center.x + dot_radius * _cos(to), center.y + dot_radius * _sin(to), 0, dot_color, 0, 0);
        }

        GEnv.UIRender->SetShader(*hShader);
        GEnv.UIRender->FlushPrimitive();

        // The ring sits where the strokes start, so it grows and shrinks exactly like they do.
        if (psHUD_Flags.test(HUD_CROSSHAIR_DYNAMIC) && x_min > dot_radius)
        {
            GEnv.UIRender->StartPrimitive(ring_segments * 2, IUIRender::ptLineList, UI().m_currentPointType);

            for (u32 segment = 0; segment < ring_segments; ++segment)
            {
                const float from = PI_MUL_2 * float(segment) / float(ring_segments);
                const float to = PI_MUL_2 * float(segment + 1) / float(ring_segments);

                GEnv.UIRender->PushPoint(
                    center.x + x_min * _cos(from), center.y + x_min * _sin(from), 0, dot_color, 0, 0);
                GEnv.UIRender->PushPoint(
                    center.x + x_min * _cos(to), center.y + x_min * _sin(to), 0, dot_color, 0, 0);
            }

            GEnv.UIRender->SetShader(*hShader);
            GEnv.UIRender->FlushPrimitive();
        }
    }
    else
    {
        GEnv.UIRender->StartPrimitive(8, IUIRender::ptLineList, UI().m_currentPointType);

        // 0
        GEnv.UIRender->PushPoint(center.x, center.y + y_min, 0, cross_color, 0, 0);
        GEnv.UIRender->PushPoint(center.x, center.y + y_max, 0, cross_color, 0, 0);
        // 1
        GEnv.UIRender->PushPoint(center.x, center.y - y_min, 0, cross_color, 0, 0);
        GEnv.UIRender->PushPoint(center.x, center.y - y_max, 0, cross_color, 0, 0);
        // 2
        GEnv.UIRender->PushPoint(center.x + x_min, center.y, 0, cross_color, 0, 0);
        GEnv.UIRender->PushPoint(center.x + x_max, center.y, 0, cross_color, 0, 0);
        // 3
        GEnv.UIRender->PushPoint(center.x - x_min, center.y, 0, cross_color, 0, 0);
        GEnv.UIRender->PushPoint(center.x - x_max, center.y, 0, cross_color, 0, 0);

        // render
        GEnv.UIRender->SetShader(*hShader);
        GEnv.UIRender->FlushPrimitive();
    }

    if (!fsimilar(target_radius, radius))
    {
        // here was crosshair innertion emulation
        radius = target_radius;
    };
#ifdef DEBUG
    if (g_bDrawFirstBulletCrosshair)
        OnRenderFirstBulletDispertion();
#endif
}
