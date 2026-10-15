#pragma once

#include "xr_3da/resource.h"

#ifdef XR_PLATFORM_WINDOWS
#   include <SDL_syswm.h>
#endif

#ifdef XR_PLATFORM_WINDOWS
inline HANDLE ExtractImage(int idx, UINT type)
{
    ZoneScoped;
    return LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(idx),
        type, 0, 0, LR_CREATEDIBSECTION);
}

inline void ExtractAndSetWindowIcon(SDL_Window* wnd, int iconIdx)
{
    ZoneScoped;

    const HICON icon = (HICON)ExtractImage(iconIdx, IMAGE_ICON);

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    R_ASSERT2(SDL_GetWindowWMInfo(wnd, &info), SDL_GetError());

    const HWND hwnd = info.info.win.window;
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
}
#else
inline void ExtractAndSetWindowIcon(SDL_Window* wnd, int iconIdx)
{
    ZoneScoped;
    SDL_Surface* surface = nullptr;
    switch (iconIdx)
    {
    case IDI_ICON_COP:
        surface = SDL_LoadBMP("icon_cop.bmp");
        break;
    case IDI_ICON_CS:
        surface = SDL_LoadBMP("icon_cs.bmp");
        break;
    case IDI_ICON_SOC:
        surface = SDL_LoadBMP("icon_soc.bmp");
        break;
    }
    if (surface)
        SDL_SetWindowIcon(wnd, surface);
}
#endif // XR_PLATFORM_WINDOWS
