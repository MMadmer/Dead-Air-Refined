#ifndef __X_RAY_H__
#define __X_RAY_H__

#include <mutex>
#include <array>

#include "xrEngine/Engine.h"

struct SDL_Window;
struct SDL_Surface;

namespace discord
{
class Core;
}

ENGINE_API void InitializeSettingsAuth();
ENGINE_API void StartupProfileBegin();
ENGINE_API void StartupProfileCheckpoint(pcstr stage);
ENGINE_API void StartupProfileMenuRendered();
ENGINE_API void StartupProfileFinishAfterPresent();

// definition
class ENGINE_API CApplication final
{
    SDL_Window* m_window{};
    std::thread m_splash_thread;
    std::atomic_bool m_should_exit;

    SDL_Surface* m_surface;

private:
    std::mutex m_discord_lock;
    discord::Core* m_discord_core{};
    bool m_discord_initialization_attempted{};

private:
    GameModule* m_game_module{};

private:
    void SplashProc();

    void ShowSplash(bool topmost);
    void HideSplash();

    void InitializeDiscord();
    void UpdateDiscordStatus(bool initializeIfNeeded);

public:
    // Other
    CApplication(pcstr commandLine, GameModule* game, const std::array<RendererModule*, 2>& modules);
    ~CApplication();

    int Run();
};

#endif //__XR_BASE_H__
