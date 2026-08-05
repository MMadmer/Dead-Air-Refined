//-----------------------------------------------------------------------------
// File: x_ray.cpp
//
// Programmers:
// Oles - Oles Shishkovtsov
// AlexMX - Alexander Maksimchuk
//-----------------------------------------------------------------------------
#include "stdafx.h"

#include "x_ray.h"

#include "embedded_resources_management.h"

#include "xrCore/Threading/TaskManager.hpp"
#include "xrNetServer/NET_AuthCheck.h"

#include "IGame_Persistent.h"
#include "LightAnimLibrary.h"
#include "XR_IOConsole.h"

#if defined(XR_PLATFORM_WINDOWS)
#include "AccessibilityShortcuts.hpp"
#include "Text_Console.h"
#else
#define CTextConsole CConsole
#pragma todo("Implement text console or it's alternative")
#endif

#ifdef XR_PLATFORM_WINDOWS
#include <locale>

#include "DiscordGameSDK/discord.h"
#define USE_DISCORD_INTEGRATION

#include "xrCore/Text/StringConversion.hpp"
#endif

// global variables
constexpr size_t MAX_WINDOW_EVENTS = 32;

#ifdef USE_DISCORD_INTEGRATION
constexpr discord::ClientId DISCORD_APP_ID = 421286728695939072;
#endif

ENGINE_API CInifile* pGameIni = nullptr;
ENGINE_API bool CallOfPripyatMode = false;
ENGINE_API bool ClearSkyMode = false;
ENGINE_API bool ShadowOfChernobylMode = false;

ENGINE_API string512 g_sLaunchOnExit_params{};
ENGINE_API string512 g_sLaunchOnExit_app{};
ENGINE_API string_path g_sLaunchWorkingFolder{};

namespace
{
constexpr size_t MAX_STARTUP_PROFILE_ENTRIES = 32;

struct StartupProfileEntry
{
    pcstr stage{};
    std::chrono::steady_clock::time_point time{};
};

struct StartupProfileState
{
    std::chrono::steady_clock::time_point start{};
    std::array<StartupProfileEntry, MAX_STARTUP_PROFILE_ENTRIES> entries{};
    size_t count{};
    size_t reportedCount{};
    bool started{};
    bool menuRendered{};
    bool finished{};
};

StartupProfileState startupProfile;

struct PathIncludePred
{
private:
    const xr_auth_strings_t* ignored;

public:
    explicit PathIncludePred(const xr_auth_strings_t* ignoredPaths) : ignored(ignoredPaths) {}
    bool IsIncluded(pcstr path)
    {
        if (!ignored)
            return true;

        return allow_to_include_path(*ignored, path);
    }
};
}

void StartupProfileBegin()
{
    startupProfile = {};
    startupProfile.start = std::chrono::steady_clock::now();
    startupProfile.started = true;
}

void StartupProfileCheckpoint(pcstr stage)
{
    if (!startupProfile.started || startupProfile.finished || startupProfile.count == startupProfile.entries.size())
        return;

    startupProfile.entries[startupProfile.count++] = { stage, std::chrono::steady_clock::now() };

    // Logging becomes available after Core initializes, so the second checkpoint also reports SDL initialization.
    if (startupProfile.count < 2)
        return;

    while (startupProfile.reportedCount < startupProfile.count)
    {
        const size_t index = startupProfile.reportedCount++;
        const auto previous = index ? startupProfile.entries[index - 1].time : startupProfile.start;
        const auto stageElapsed =
            std::chrono::duration<double, std::milli>(startupProfile.entries[index].time - previous);
        const auto totalElapsed =
            std::chrono::duration<double, std::milli>(startupProfile.entries[index].time - startupProfile.start);
        Msg("* Startup checkpoint: %7.2f ms stage, %7.2f ms total - %s",
            stageElapsed.count(), totalElapsed.count(), startupProfile.entries[index].stage);
    }
}

void StartupProfileMenuRendered()
{
    startupProfile.menuRendered = true;
}

void StartupProfileFinishAfterPresent()
{
    if (!startupProfile.started || !startupProfile.menuRendered || startupProfile.finished)
        return;

    StartupProfileCheckpoint("First menu frame presented");
    startupProfile.finished = true;

    const auto total =
        std::chrono::duration<double, std::milli>(startupProfile.entries[startupProfile.count - 1].time - startupProfile.start);
    Msg("* Startup profile total: %.2f ms", total.count());
    FlushLog();
}

template <typename T>
void InitConfig(T& config, pcstr name, bool fatal = true,
    bool readOnly = true, bool loadAtStart = true, bool saveAtEnd = true,
    u32 sectCount = 0, const CInifile::allow_include_func_t& allowIncludeFunc = nullptr)
{
    string_path fname;
    FS.update_path(fname, "$game_config$", name);
    config = xr_new<CInifile>(fname, readOnly, loadAtStart, saveAtEnd, sectCount, allowIncludeFunc);

    CHECK_OR_EXIT(config->section_count() || !fatal,
        make_string("Cannot find file %s.\nReinstalling application may fix this problem.", fname));
}

// XXX: make it more fancy
// некрасиво слишком
void set_shoc_mode()
{
    CallOfPripyatMode = false;
    ShadowOfChernobylMode = true;
    ClearSkyMode = false;
}

void set_cs_mode()
{
    CallOfPripyatMode = false;
    ShadowOfChernobylMode = false;
    ClearSkyMode = true;
}

void set_cop_mode()
{
    CallOfPripyatMode = true;
    ShadowOfChernobylMode = false;
    ClearSkyMode = false;
}

void set_free_mode()
{
    CallOfPripyatMode = false;
    ShadowOfChernobylMode = false;
    ClearSkyMode = false;
}

void InitSettings()
{
    ZoneScoped;

    InitConfig(pSettings, "system.ltx");
    InitConfig(pSettingsOpenXRay, "openxray.ltx", false, true, true, false);
    InitConfig(pGameIni, "game.ltx");

    if (strstr(Core.Params, "-shoc") || strstr(Core.Params, "-soc"))
        set_shoc_mode();
    else if (strstr(Core.Params, "-cs"))
        set_cs_mode();
    else if (strstr(Core.Params, "-cop"))
        set_cop_mode();
    else if (strstr(Core.Params, "-unlock_game_mode"))
        set_free_mode();
    else
    {
        pcstr gameMode = READ_IF_EXISTS(pSettingsOpenXRay, r_string, "compatibility", "game_mode", "cop");
        if (xr_strcmpi("cop", gameMode) == 0)
            set_cop_mode();
        else if (xr_strcmpi("cs", gameMode) == 0)
            set_cs_mode();
        else if (xr_strcmpi("shoc", gameMode) == 0 || xr_strcmpi("soc", gameMode) == 0)
            set_shoc_mode();
        else if (xr_strcmpi("unlock", gameMode) == 0)
            set_free_mode();
    }
}

void InitializeSettingsAuth()
{
    static std::mutex settingsAuthMutex;
    std::lock_guard guard{ settingsAuthMutex };
    if (pSettingsAuth)
        return;

    xr_auth_strings_t ignoredPaths, checkedPaths;
    fill_auth_check_params(ignoredPaths, checkedPaths); //TODO port xrNetServer to Linux
    PathIncludePred includePred(&ignoredPaths);
    CInifile::allow_include_func_t includeFilter;
    includeFilter.bind(&includePred, &PathIncludePred::IsIncluded);
    InitConfig(pSettingsAuth, "system.ltx", true, true, true, false, 0, includeFilter);
}

void InitConsole()
{
    ZoneScoped;

    if (GEnv.isDedicatedServer)
        Console = xr_new<CTextConsole>();
    else
        Console = xr_new<CConsole>();

    Console->Initialize();
    xr_strcpy(Console->ConfigFile, "user.ltx");
    if (strstr(Core.Params, "-ltx "))
    {
        string64 c_name;
        sscanf(strstr(Core.Params, "-ltx ") + strlen("-ltx "), "%[^ ] ", c_name);
        xr_strcpy(Console->ConfigFile, c_name);
    }
}

void destroySettings()
{
    ZoneScoped;
    auto s = const_cast<CInifile**>(&pSettings);
    xr_delete(*s);

    auto sa = const_cast<CInifile**>(&pSettingsAuth);
    xr_delete(*sa);

    auto so = const_cast<CInifile**>(&pSettingsOpenXRay);
    xr_delete(*so);

    xr_delete(pGameIni);
}

void destroyConsole()
{
    ZoneScoped;
    Console->Execute("cfg_save");
    Console->Destroy();
    xr_delete(Console);
}

void execUserScript()
{
    ZoneScoped;
    Console->Execute("default_controls");
    Console->ExecuteScript(Console->ConfigFile);
}

constexpr pcstr FRAME_MARK_APPLICATION_STARTUP = "Application startup";
constexpr pcstr FRAME_MARK_APPLICATION_SHUTDOWN = "Application shutdown";
constexpr pcstr FRAME_MARK_APPLICATION_RUN = "Application run";

CApplication::CApplication(pcstr commandLine, GameModule* game, const std::array<RendererModule*, 2>& modules)
{
    TracySetProgramName("OpenXRay");
    Threading::SetCurrentThreadName("Primary thread");
    FrameMarkStart(FRAME_MARK_APPLICATION_STARTUP);

    if (strstr(commandLine, "-dedicated"))
        GEnv.isDedicatedServer = true;

    xrDebug::Initialize(commandLine);
    {
        ZoneScopedN("SDL_Init");
        u32 flags = SDL_INIT_VIDEO;
        if (!strstr(commandLine, "-no_gamepad"))
            flags |= SDL_INIT_GAMECONTROLLER;
        R_ASSERT3(SDL_Init(flags) == 0, "Unable to initialize SDL", SDL_GetError());
    }
    StartupProfileCheckpoint("SDL initialized");

#ifdef XR_PLATFORM_WINDOWS
    AccessibilityShortcuts shortcuts;
    if (!GEnv.isDedicatedServer)
        shortcuts.Disable();
#endif

    if (!strstr(commandLine, "-nosplash"))
    {
        const bool topmost = !strstr(commandLine, "-splashnotop");
        ShowSplash(topmost);
    }

    SDL_StopTextInput(); // It's enabled by default for some reason, we don't want it
    const auto& inputTask = TaskManager::AddTask([]
    {
        const bool captureInput = !strstr(Core.Params, "-i");
        pInput = xr_new<CInput>(captureInput);
    });

    std::atomic_bool soundConfigurationReady{};
    const auto& initializeSound = TaskManager::AddTask([&soundConfigurationReady]
    {
        Engine.Sound.CreateDevicesList();
        soundConfigurationReady.wait(false, std::memory_order_acquire);
        Engine.Sound.Create();
    });

    pcstr fsltx = "-fsltx ";
    string_path fsgame = "";
    if (strstr(commandLine, fsltx))
    {
        const size_t sz = xr_strlen(fsltx);
        sscanf(strstr(commandLine, fsltx) + sz, "%[^ ] ", fsgame);
    }

    Core.Initialize("OpenXRay", commandLine, true, *fsgame ? fsgame : nullptr);
    StartupProfileCheckpoint("Filesystem initialized");

    InitSettings();
    StartupProfileCheckpoint("Settings initialized");
    // Adjust player & computer name for Asian
    if (pSettings->line_exist("string_table", "no_native_input"))
    {
        xr_strcpy(Core.UserName, sizeof(Core.UserName), "Player");
        xr_strcpy(Core.CompName, sizeof(Core.CompName), "Computer");
    }

    Device.InitializeImGui();
    TaskScheduler->Wait(inputTask);
    InitConsole();
    StartupProfileCheckpoint("Input, ImGui and console initialized");

    Engine.Initialize(game, modules);
    StartupProfileCheckpoint("Renderer selected");
    Device.Initialize();
    StartupProfileCheckpoint("Application window initialized");

    Console->OnDeviceInitialize();

    execUserScript();
    StartupProfileCheckpoint("User configuration applied");

    soundConfigurationReady.store(true, std::memory_order_release);
    soundConfigurationReady.notify_one();
    StartupProfileCheckpoint("Sound initialization released");

    // ...command line for auto start
    pcstr startArgs = strstr(Core.Params, "-start ");
    pcstr loadArgs = strstr(Core.Params, "-load ");
    if (startArgs || loadArgs)
        TaskScheduler->Wait(initializeSound);

    if (startArgs)
        Console->Execute(startArgs + 1);
    if (loadArgs)
        Console->Execute(loadArgs + 1);

    // Initialize APP
    const auto& createLightAnim = TaskScheduler->AddTask([]
    {
        LALib.OnCreate();
    });

    Device.Create();
    StartupProfileCheckpoint("Render device created");
    TaskScheduler->Wait(initializeSound);
    TaskScheduler->Wait(createLightAnim);
    StartupProfileCheckpoint("Sound and light animations initialized");

    if (game)
    {
        m_game_module = game;
        g_pGamePersistent = game->create_persistent();
        R_ASSERT(g_pGamePersistent);
    }
    StartupProfileCheckpoint("Game persistent created");
    if (g_pGamePersistent)
        g_pGamePersistent->OnAppStart();
    else
        Console->Show();
    StartupProfileCheckpoint("Application startup completed");

    FrameMarkEnd(FRAME_MARK_APPLICATION_STARTUP);
}

CApplication::~CApplication()
{
    FrameMarkStart(FRAME_MARK_APPLICATION_SHUTDOWN);

    if (g_pGamePersistent)
    {
        Device.PreCache(0, false);
        g_pGamePersistent->OnAppEnd();
    }

    if (m_game_module)
        m_game_module->destroy_persistent(g_pGamePersistent);

    Engine.Event.Dump();

    xr_delete(pInput);
    destroySettings();

    LALib.OnDestroy();

    destroyConsole();

    Device.CleanupVideoModes();
    Device.DestroyImGui();
    Engine.Sound.Destroy();

    Device.Destroy();
    Engine.Destroy();

#ifdef USE_DISCORD_INTEGRATION
    discord::Core::Destroy(&m_discord_core);
#endif

    // check for need to execute something external
    if (/*xr_strlen(g_sLaunchOnExit_params) && */ xr_strlen(g_sLaunchOnExit_app))
    {
#if defined(XR_PLATFORM_WINDOWS)
        // CreateProcess need to return results to next two structures
        STARTUPINFO si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        // We use CreateProcess to setup working folder
        pcstr tempDir = xr_strlen(g_sLaunchWorkingFolder) ? g_sLaunchWorkingFolder : nullptr;
        CreateProcess(g_sLaunchOnExit_app, g_sLaunchOnExit_params, nullptr, nullptr, FALSE, 0, nullptr, tempDir, &si, &pi);
#endif
    }

    Core._destroy();
    {
        ZoneScopedN("SDL_Quit");
        SDL_Quit();
    }

    xrDebug::Finalize();
    FrameMarkEnd(FRAME_MARK_APPLICATION_SHUTDOWN);
}

int CApplication::Run()
{
    HideSplash();
    Device.Run();

    while (!SDL_QuitRequested()) // SDL_PumpEvents is here
    {
        FrameMarkStart(FRAME_MARK_APPLICATION_RUN);
        bool canCallActivate = false;
        bool shouldActivate = false;

        SDL_Event events[MAX_WINDOW_EVENTS];
        const int count = SDL_PeepEvents(events, MAX_WINDOW_EVENTS,
            SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT);

        for (int i = 0; i < count; ++i)
        {
            const SDL_Event event = events[i];

            switch (event.type)
            {
            case SDL_WINDOWEVENT:
            {
                const auto window = SDL_GetWindowFromID(event.window.windowID);

                switch (event.window.event)
                {
                case SDL_WINDOWEVENT_SHOWN:
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                case SDL_WINDOWEVENT_RESTORED:
                case SDL_WINDOWEVENT_MAXIMIZED:
                    if (window != Device.m_sdlWnd)
                        Device.OnWindowActivate(window, true);
                    else
                    {
                        canCallActivate = true;
                        shouldActivate = true;
                    }
                    continue;

                case SDL_WINDOWEVENT_HIDDEN:
                case SDL_WINDOWEVENT_FOCUS_LOST:
                case SDL_WINDOWEVENT_MINIMIZED:
                    if (window != Device.m_sdlWnd)
                        Device.OnWindowActivate(window, false);
                    else
                    {
                        canCallActivate = true;
                        shouldActivate = false;
                    }
                    continue;
                } // switch (event.window.event)
            }
            } // switch (event.type)

            // Only process event in Device
            // if it wasn't processed in the switch above
            Device.ProcessEvent(event);
        } // for (int i = 0; i < count; ++i)

        // Workaround for screen blinking when there's too much timeouts
        if (canCallActivate)
        {
            Device.OnWindowActivate(Device.m_sdlWnd, shouldActivate);
        }

        Device.ProcessFrame();

        UpdateDiscordStatus(true);
        FrameMarkEnd(FRAME_MARK_APPLICATION_RUN);
    } // while (!SDL_QuitRequested())

    Device.Shutdown();

    return 0;
}

void CApplication::ShowSplash(bool topmost)
{
    if (m_window)
        return;

    ZoneScoped;

    m_surface = std::move(ExtractSplashScreen());
    if (!m_surface)
    {
        Log("~ Couldn't create surface from image:", SDL_GetError());
        return;
    }

    Uint32 flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;

    if (topmost)
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;

    m_window = SDL_CreateWindow("OpenXRay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, m_surface->w, m_surface->h, flags);
    SDL_ShowWindow(m_window);

    m_splash_thread = Threading::RunThread("Splash Thread", &CApplication::SplashProc, this);
    SDL_PumpEvents();
}

constexpr u32 SPLASH_FRAMERATE = 30;

void CApplication::SplashProc()
{
    {
        ZoneScopedN("Update splash image");
        const auto current = SDL_GetWindowSurface(m_window);
        SDL_BlitSurface(m_surface, nullptr, current, nullptr);
        SDL_UpdateWindowSurface(m_window);
    }
    while (!m_should_exit.load(std::memory_order_acquire))
    {
        UpdateDiscordStatus(false);
        Sleep(SPLASH_FRAMERATE);
    }
}

void CApplication::HideSplash()
{
    if (!m_window)
        return;

    ZoneScoped;

    m_should_exit.store(true, std::memory_order_release);
    m_splash_thread.join();

    SDL_DestroyWindow(m_window);
    m_window = nullptr;

    SDL_FreeSurface(m_surface);
}

void CApplication::InitializeDiscord()
{
#ifdef USE_DISCORD_INTEGRATION
    ZoneScoped;
    discord::Core* core{};
    discord::Core::Create(DISCORD_APP_ID, discord::CreateFlags::NoRequireDiscord, &core);

#   ifndef MASTER_GOLD
    if (core)
    {
        const auto level = xrDebug::DebuggerIsPresent() ? discord::LogLevel::Debug : discord::LogLevel::Info;
        core->SetLogHook(level, [](discord::LogLevel level, pcstr message)
        {
            switch (level)
            {
            case discord::LogLevel::Error: Log("!", message); break;
            case discord::LogLevel::Warn:  Log("~", message); break;
            case discord::LogLevel::Info:  Log("*", message); break;
            case discord::LogLevel::Debug: Log("#", message); break;
            }
        });
    }
#   endif

    if (core)
    {
        const std::locale locale("");

        discord::Activity activity{};
        activity.SetType(discord::ActivityType::Playing);
        activity.SetApplicationId(DISCORD_APP_ID);
        activity.SetState(StringToUTF8(Core.ApplicationTitle, locale).c_str());
        activity.GetAssets().SetLargeImage("logo");
        core->ActivityManager().UpdateActivity(activity, nullptr);

        std::lock_guard guard{ m_discord_lock };
        m_discord_core = core;
    }
#endif
}

void CApplication::UpdateDiscordStatus(bool initializeIfNeeded)
{
#ifdef USE_DISCORD_INTEGRATION
    if (initializeIfNeeded && !m_discord_initialization_attempted)
    {
        m_discord_initialization_attempted = true;
        InitializeDiscord();
    }

    if (!m_discord_core)
        return;

    ZoneScoped;
    std::lock_guard guard{ m_discord_lock };
    m_discord_core->RunCallbacks();
#endif
}
