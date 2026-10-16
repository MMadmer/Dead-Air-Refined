// Engine.cpp: implementation of the CEngine class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Engine.h"

#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"

#include "xrCore/xr_token.h"

extern xr_vector<xr_token> VidQualityToken;

struct SoundProcessor final : public pureFrame
{
    void OnFrame() override
    {
        GEnv.Sound->update(Device.vCameraPosition, Device.vCameraDirection, Device.vCameraTop, Device.vCameraRight);
    }
} g_sound_processor;

struct SoundRenderer final : public pureFrame
{
    void OnFrame() override
    {
        GEnv.Sound->render();
    }
} g_sound_renderer;

CEngine Engine;

CEngine::CEngine() {}
CEngine::~CEngine() {}

namespace
{
bool IsRenderModeAvailable(pcstr mode)
{
    return std::any_of(VidQualityToken.begin(), VidQualityToken.end(),
        [mode](const xr_token& token) { return token.name && !xr_stricmp(token.name, mode); });
}

// DirectX 11 is the default and the fallback: DirectX 12 runs only when it was chosen.
pcstr DefaultRenderMode()
{
    if (IsRenderModeAvailable("renderer_r4"))
        return "renderer_r4";
    // Hardware below feature level 11 gets the best of what the list offers.
    const auto best = std::find_if(VidQualityToken.rbegin(), VidQualityToken.rend(),
        [](const xr_token& token) -> bool { return token.name; });
    return best != VidQualityToken.rend() ? best->name : nullptr;
}

void ExecuteRenderMode(pcstr mode)
{
    if (!mode)
        return;
    string64 cmd;
    xr_sprintf(cmd, "renderer %s", mode);
    Console->Execute(cmd);
}

// Reads the mode named by the "renderer" line of the config without executing anything.
class CCC_ReadSavedRenderer final : public CCC_LoadCFG_custom
{
public:
    string64 mode{};

    CCC_ReadSavedRenderer() : CCC_LoadCFG_custom("renderer ") {}

    bool allow(pcstr cmd) override
    {
        if (CCC_LoadCFG_custom::allow(cmd))
            sscanf(cmd + xr_strlen("renderer "), "%63s", mode);
        return false;
    }
};
} // namespace

void CheckAndSetupRenderer()
{
    if (GEnv.isDedicatedServer)
    {
        Console->Execute("renderer renderer_r1");
        return;
    }

    // The -rgl key is gone together with the OpenGL renderer: the mode list is built from
    // the renderer modules, R4 (DirectX 11), R5 (DirectX 12) and R6 (Vulkan).
    if (strstr(Core.Params, "-r6"))
    {
        if (IsRenderModeAvailable("renderer_r6"))
            Console->Execute("renderer renderer_r6");
        else
        {
            Msg("! -r6: Vulkan is not available on this system, starting DirectX 11");
            ExecuteRenderMode(DefaultRenderMode());
        }
    }
    else if (strstr(Core.Params, "-r5"))
    {
        if (IsRenderModeAvailable("renderer_r5"))
            Console->Execute("renderer renderer_r5");
        else
        {
            Msg("! -r5: DirectX 12 is not available on this system, starting DirectX 11");
            ExecuteRenderMode(DefaultRenderMode());
        }
    }
    else if (strstr(Core.Params, "-r4"))
        Console->Execute("renderer renderer_r4");
    else if (strstr(Core.Params, "-r3"))
        Console->Execute("renderer renderer_r3");
    else if (strstr(Core.Params, "-r2.5"))
        Console->Execute("renderer renderer_r2.5");
    else if (strstr(Core.Params, "-r2a"))
        Console->Execute("renderer renderer_r2a");
    else if (strstr(Core.Params, "-r2"))
        Console->Execute("renderer renderer_r2");
    else if (strstr(Core.Params, "-r1"))
        Console->Execute("renderer renderer_r1");
    else
    {
        // The player's API choice (the Video tab) lives in user.ltx, which runs only after the
        // renderer is selected: read that one line now. A mode the list does not offer here -
        // DirectX 12 on hardware that fails the probe, an old "renderer_r2" - falls back to the
        // default, and the next save writes that over it.
        renderer_allow_override = true;
        CCC_ReadSavedRenderer saved;
        saved.Execute(Console->ConfigFile);
        pcstr mode = DefaultRenderMode();
        if (saved.mode[0])
        {
            if (IsRenderModeAvailable(saved.mode))
                mode = saved.mode;
            else
                Msg("! Renderer [%s] is not available on this system, using [%s]", saved.mode,
                    mode ? mode : "none");
        }
        ExecuteRenderMode(mode);
    }
}

extern void msCreate(pcstr name);

void CEngine::Initialize(GameModule* game, const RendererModules& modules)
{
    ZoneScoped;
#ifdef DEBUG
    msCreate("game");
#endif

    eQuit = Event.Handler_Attach("KERNEL:quit", this);

    Device.seqFrame.Add(this, REG_PRIORITY_HIGH + 1000);

    Device.seqFrame.Add(&g_sound_processor, REG_PRIORITY_NORMAL - 1000); // Place it after Level update
    Device.seqFrameMT.Add(&g_sound_renderer);

    External.CreateRendererList(modules);
    CheckAndSetupRenderer();

    External.Initialize(game);
    Sheduler.Initialize();
}

void CEngine::Destroy()
{
    ZoneScoped;

    Sheduler.Destroy();
    External.Destroy();
    Event._destroy();

    Event.Handler_Detach(eQuit, this);

    Device.seqFrameMT.Remove(&g_sound_processor);
    Device.seqFrame.Remove(&g_sound_processor);
    Device.seqFrame.Remove(this);
}

void CEngine::OnEvent(EVENT E, u64 P1, u64 P2)
{
    if (E == eQuit)
    {
        if (pInput != nullptr)
            pInput->GrabInput(false);

        SDL_Event quit = { SDL_QUIT };
        SDL_PushEvent(&quit);
    }
}

void CEngine::OnFrame()
{
    Event.OnFrame();
}
