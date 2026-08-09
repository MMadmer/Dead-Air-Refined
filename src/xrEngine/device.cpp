#include "stdafx.h"

#include "Render.h"

#include "xrCore/FS_impl.h"
#include "xrCore/Threading/TaskManager.hpp"

#include "XR_IOConsole.h"
#include "xr_input.h"

#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "x_ray.h"

#include "xrScriptEngine/script_space.hpp"

#include <SDL.h>
#include <unordered_map>

ENGINE_API CRenderDevice Device;
ENGINE_API CLoadScreenRenderer load_screen_renderer;

ENGINE_API bool g_bRendering = false;

ENGINE_API bool g_bBenchmark = false;
string512 g_sBenchmarkName;

// 0 means no limit at all, which is what the "off" position of the FPS lock option selects.
int ps_fps_limit = 0;
// Retained for existing user.ltx files; frame pacing uses the global limit in every game state.
int ps_fps_limit_in_menu = 60;
extern int g_pause_in_background;

bool g_bLoaded = false;
ref_light precache_light = 0;

using namespace xray;

namespace
{
void destroy_precache_light()
{
    if (!precache_light)
        return;

    precache_light->set_active(false);
    precache_light.destroy();
}
}

bool CRenderDevice::RenderBegin()
{
    if (GEnv.isDedicatedServer)
        return true;

    ZoneScoped;

    switch (GEnv.Render->GetDeviceState())
    {
    case DeviceState::Normal: break;
    case DeviceState::Lost:
        // If the device was lost, do not render until we get it back
        Sleep(33);
        return false;

    case DeviceState::NeedReset:
        // Check if the device is ready to be reset
        Reset();
        return false;

    default: R_ASSERT(0);
    }
    GEnv.Render->Begin();
    g_bRendering = true;

    return true;
}

void CRenderDevice::Clear() { GEnv.Render->Clear(); }

void CRenderDevice::RenderEnd(void)
{
    if (GEnv.isDedicatedServer)
        return;

    ZoneScoped;
    if (dwPrecacheFrame)
    {
        GEnv.Sound->set_master_volume(0.f);
        dwPrecacheFrame--;
        if (!dwPrecacheFrame)
        {
            GEnv.Render->updateGamma();
            destroy_precache_light();
            GEnv.Sound->set_master_volume(1.f);
            GEnv.Render->ResourcesDestroyNecessaryTextures();
            Memory.mem_compact();
            Msg("* MEMORY USAGE: %d K", Memory.mem_usage() / 1024);
            Msg("* End of synchronization A[%d] R[%d]", b_is_Active, b_is_Ready);
            FIND_CHUNK_COUNTER_FLUSH();
            if (g_pGamePersistent->GameType() == 1 && !psDeviceFlags.test(rsAlwaysActive)) // haCk
            {
                const Uint32 flags = SDL_GetWindowFlags(m_sdlWnd);
                if ((flags & SDL_WINDOW_INPUT_FOCUS) == 0)
                    Pause(true, true, true, "application start");
            }
        }
    }
    // end scene
    g_bRendering = false;
    GEnv.Render->End();
    StartupProfileFinishAfterPresent();

    vCameraPositionSaved = vCameraPosition;
    vCameraDirectionSaved = vCameraDirection;
    vCameraTopSaved = vCameraTop;
    vCameraRightSaved = vCameraRight;

    mFullTransformSaved = mFullTransform;
    mViewSaved = mView;
    mProjectSaved = mProject;
}

void CRenderDevice::PreCache(u32 amount, bool wait_user_input)
{
    if (GEnv.isDedicatedServer)
        amount = 0;
    else if (GEnv.Render->GetForceGPU_REF())
        amount = 0;

    dwPrecacheFrame = dwPrecacheTotal = amount;
    if (!amount)
    {
        destroy_precache_light();
        return;
    }
    if (!precache_light && g_pGameLevel && g_loading_events.empty())
    {
        precache_light = GEnv.Render->light_create();
        precache_light->set_shadow(false);
        precache_light->set_position(vCameraPosition);
        precache_light->set_color(255, 255, 255);
        precache_light->set_range(5.0f);
        precache_light->set_active(true);
    }
    if (amount && !load_screen_renderer.IsActive())
    {
        load_screen_renderer.Start(wait_user_input);
    }
}

void CRenderDevice::CalcFrameStats()
{
    stats.RenderTotal.FrameEnd();
    do
    {
        // calc FPS & TPS
        if (fTimeDeltaReal <= EPS_S)
            break;
        const float fps = 1.f / fTimeDeltaReal;
        // if (Engine.External.tune_enabled) vtune.update (fps);
        constexpr float fOne = 0.3f;
        constexpr float fInv = 1.0f - fOne;
        stats.fFPS = fInv * stats.fFPS + fOne * fps;
        if (stats.RenderTotal.result > EPS_S)
        {
            const u32 renderedPolys = GEnv.Render->GetCacheStatPolys();
            stats.fTPS = fInv * stats.fTPS + fOne * float(renderedPolys) / (stats.RenderTotal.result * 1000.f);
            stats.fRFPS = fInv * stats.fRFPS + fOne * 1000.f / stats.RenderTotal.result;
        }
    } while (false);
    stats.RenderTotal.FrameStart();
}

int g_svDedicateServerUpdateReate = 100;

ENGINE_API xr_list<LOADING_EVENT> g_loading_events;

bool CRenderDevice::BeforeFrame()
{
    ZoneScoped;

    if (!b_is_Ready)
    {
        Sleep(100);
        return false;
    }

    if (psDeviceFlags.test(rsStatistic))
        g_bEnableStatGather = true; // XXX: why not use either rsStatistic or g_bEnableStatGather?
    else
        g_bEnableStatGather = false;

    if (!g_loading_events.empty())
    {
        if (g_loading_events.front()())
            g_loading_events.pop_front();
        g_pGamePersistent->LoadDraw();
        return false;
    }

    return true;
}

void CRenderDevice::OnCameraUpdated()
{
    static u32 frame{ u32(-1) };
    if (frame == dwFrame)
        return;

    ZoneScoped;

    // Precache
    if (dwPrecacheFrame)
    {
        const float factor = float(dwPrecacheFrame) / float(dwPrecacheTotal);
        const float angle = PI_MUL_2 * factor;
        vCameraDirection.set(_sin(angle), 0, _cos(angle));
        vCameraDirection.normalize();
        vCameraTop.set(0, 1, 0);
        vCameraRight.crossproduct(vCameraTop, vCameraDirection);
        mView.build_camera_dir(vCameraPosition, vCameraDirection, vCameraTop);
    }

    // Matrices
    mInvView.invert(mView);
    mFullTransform.mul(mProject, mView);
    mInvFullTransform.invert_44(mFullTransform);
    GEnv.Render->OnCameraUpdated();
    GEnv.Render->SetCacheXform(mView, mProject);

    frame = dwFrame;
}

static void UpdateViewports()
{
    // Update and Render additional Platform Windows
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void CRenderDevice::DoRender()
{
    if (GEnv.isDedicatedServer)
        return;

    ZoneScoped;

    CStatTimer renderTotalReal;
    renderTotalReal.FrameStart();
    renderTotalReal.Begin();
    if (b_is_Active && RenderBegin())
    {
        {
            ZoneScopedN("Render process");
            seqRender.Process(); // all rendering is done here
        }

        CalcFrameStats();
        Statistic->Show();

        ImGui::Render();
        if (ImDrawData* drawData = ImGui::GetDrawData())
            m_imgui_render->Render(drawData);
        UpdateViewports();

        RenderEnd(); // Present goes here
    }
    else
    {
        UpdateViewports();
    }
    renderTotalReal.End();
    renderTotalReal.FrameEnd();
    stats.RenderTotal.accum = renderTotalReal.accum;
}

void CRenderDevice::ProcessFrame()
{
    ZoneScoped;

    if (!BeforeFrame())
        return;

    FrameMove();

    OnCameraUpdated();

    {
        std::lock_guard lock(seqParallelMutex);
        seqParallelProcessing.clear();
        seqParallelProcessing.swap(seqParallel);
    }

    const auto& processSeqParallel = TaskScheduler->AddTask([this]
    {
        ZoneScopedN("ProcessParallelSequence");

        xr_vector<const void*> concurrentKeys;
        std::unordered_map<const void*, size_t> groupIndices;
        xr_vector<Task*> concurrentTasks;
        concurrentKeys.reserve(seqParallelProcessing.size());
        groupIndices.reserve(seqParallelProcessing.size());
        concurrentTasks.reserve(seqParallelProcessing.size());
        size_t concurrentBegin = 0;
        bool concurrentBatchActive = false;

        const auto flushConcurrentTasks =
            [this, &concurrentKeys, &groupIndices, &concurrentTasks, &concurrentBegin,
                &concurrentBatchActive](size_t concurrentEnd)
        {
            if (!concurrentBatchActive)
                return;

            for (const void* key : concurrentKeys)
            {
                concurrentTasks.push_back(&TaskScheduler->AddTask(
                    [this, key, concurrentBegin, concurrentEnd]
                    {
                        ZoneScopedN("ConcurrentFrameGroup");
                        for (size_t index = concurrentBegin; index < concurrentEnd; ++index)
                        {
                            const ParallelFrameTask& frameTask = seqParallelProcessing[index];
                            if (frameTask.concurrencyKey == key)
                                frameTask.callback();
                        }
                    }));
            }

            for (Task* task : concurrentTasks)
                TaskScheduler->Wait(*task);
            concurrentKeys.clear();
            groupIndices.clear();
            concurrentTasks.clear();
            concurrentBatchActive = false;
        };

        for (size_t index = 0; index < seqParallelProcessing.size(); ++index)
        {
            const ParallelFrameTask& frameTask = seqParallelProcessing[index];
            if (frameTask.concurrencyKey)
            {
                if (!concurrentBatchActive)
                {
                    concurrentBegin = index;
                    concurrentBatchActive = true;
                }

                if (groupIndices.emplace(frameTask.concurrencyKey, concurrentKeys.size()).second)
                    concurrentKeys.push_back(frameTask.concurrencyKey);
                continue;
            }

            // Serial entries preserve the dependency barriers of the legacy queue.
            flushConcurrentTasks(index);
            frameTask.callback();
        }

        flushConcurrentTasks(seqParallelProcessing.size());
        seqParallelProcessing.clear();
        seqFrameMT.Process();
    });

    DoRender();

    TaskScheduler->Wait(processSeqParallel);

    const int configuredLimit = GEnv.isDedicatedServer ? g_svDedicateServerUpdateReate : ps_fps_limit;

    // Zero disables frame pacing entirely; the limiter used to clamp to 1 fps minimum, so an
    // "unlimited" setting still slept for its own frame budget every frame.
    static CTimerBase::Clock::time_point pacingDeadline{};
    static int pacedLimit = 0;

    if (configuredLimit > 0)
    {
        // Preserve sub-millisecond frame intervals instead of truncating 1000 / FPS.
        const auto frameDuration = std::chrono::duration_cast<CTimerBase::Clock::duration>(
            std::chrono::duration<double>(1.0 / configuredLimit));

        // Pace against the previous deadline instead of this frame's start: a frame that ran over
        // budget is then followed by a shorter wait, so the rate holds the target instead of
        // drifting below it (every overrun used to be lost for good). Re-sync when the limit
        // changes or a whole frame has already been missed - after a level load or an alt-tab the
        // engine must not sprint to catch up on a backlog.
        const auto frameEnd = CTimerBase::Clock::now();
        if (pacedLimit != configuredLimit || pacingDeadline + frameDuration < frameEnd)
        {
            pacedLimit = configuredLimit;
            pacingDeadline = frameEnd;
        }

        pacingDeadline += frameDuration;
        const auto deadline = pacingDeadline;

        // Waiting out the whole budget with sleep_until misses the target badly: the thread wakes
        // up to a scheduler tick late, so a 120 fps budget (8.3 ms) turned into ~9.3 ms frames,
        // i.e. 107 fps. Wait on a high-resolution timer instead and spin out the last fraction.
        // The reserve follows the wake-up latency this machine really shows, so the spin stays as
        // short as it can be while still absorbing the overshoot.
        static auto spinReserve = std::chrono::microseconds(600);
        constexpr auto reserveMin = std::chrono::microseconds(200);
        constexpr auto reserveMax = std::chrono::microseconds(2000);

        const auto waitTarget = deadline - spinReserve;
        const auto beforeWait = CTimerBase::Clock::now();
        if (waitTarget > beforeWait)
        {
#if defined(XR_PLATFORM_WINDOWS)
            static const HANDLE pacingTimer = []
            {
                // The high-resolution flag needs Windows 10 1803; the coarse timer is still better
                // than sleep_until because it does not round the wait up to a tick.
                if (HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
                    return timer;
                return CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            }();

            if (pacingTimer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    waitTarget - beforeWait).count() / 100); // negative = relative, 100 ns units
                if (SetWaitableTimerEx(pacingTimer, &due, 0, nullptr, nullptr, nullptr, 0))
                    WaitForSingleObject(pacingTimer, INFINITE);
            }
#else
            std::this_thread::sleep_until(waitTarget);
#endif
            const auto late = std::chrono::duration_cast<std::chrono::microseconds>(
                CTimerBase::Clock::now() - waitTarget);
            spinReserve = std::clamp(std::max(late + std::chrono::microseconds(150),
                spinReserve - std::chrono::microseconds(20)), reserveMin, reserveMax);
        }

        // Busy-wait the remainder: yielding here would hand the core to a worker thread for a
        // whole quantum and overshoot far worse than the reserve being burned.
        while (CTimerBase::Clock::now() < deadline)
            YieldProcessor();
    }

    if (!b_is_Active)
        Sleep(1);
}

void CRenderDevice::ProcessEvent(const SDL_Event& event)
{
    ZoneScoped;

    switch (event.type)
    {
    case SDL_DISPLAYEVENT:
    {
        switch (event.display.type)
        {
        case SDL_DISPLAYEVENT_ORIENTATION:
        case SDL_DISPLAYEVENT_CONNECTED:
        case SDL_DISPLAYEVENT_DISCONNECTED:
            CleanupVideoModes();
            FillVideoModes();
            if (event.display.display == psDeviceMode.Monitor && event.display.type != SDL_DISPLAYEVENT_CONNECTED)
                Reset();
            else
                UpdateWindowProps();
            break;
        } // switch (event.display.type)
        break;
    }
    case SDL_WINDOWEVENT:
    {
        const auto window = SDL_GetWindowFromID(event.window.windowID);
        if (!window)
            break;
        ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(window);
        if (!viewport)
            break;

        switch (event.window.event)
        {
        case SDL_WINDOWEVENT_MOVED:
        {
            if (window == m_sdlWnd)
            {
                UpdateWindowRects();
            }
            if (viewport)
                viewport->PlatformRequestMove = true;
            break;
        }

        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
            psDeviceMode.Monitor = event.window.data1;
            break;

        case SDL_WINDOWEVENT_RESIZED:
            if (window == m_sdlWnd)
                UpdateWindowRects();
            break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        {
            if (window == m_sdlWnd)
            {
                UpdateWindowRects();

                // Exclusive mode changes are already applied through Reset; ignore stale SDL window sizes.
                if (psDeviceMode.WindowStyle == rsFullscreen)
                    break;

                if (static_cast<int>(psDeviceMode.Width) == event.window.data1 &&
                    static_cast<int>(psDeviceMode.Height) == event.window.data2)
                    break; // we don't need to reset device if resolution wasn't really changed

                psDeviceMode.Width = event.window.data1;
                psDeviceMode.Height = event.window.data2;

                Reset();
            }
            if (viewport)
                viewport->PlatformRequestResize = true;

            break;
        }

        case SDL_WINDOWEVENT_CLOSE:
        {
            if (viewport)
                viewport->PlatformRequestClose = true;

            if (window == m_sdlWnd)
            {
                Engine.Event.Defer("KERNEL:disconnect");
                Engine.Event.Defer("KERNEL:quit");
            }
            break;
        }
        } // switch (event.window.event)
    }
    } // switch (event.type)

    editor().ProcessEvent(event);
}

void CRenderDevice::Run()
{
    ZoneScoped;

    g_bLoaded = false;
    Log("Starting engine...");

    // Startup timers and calculate timer delta
    dwTimeGlobal = 0;
    Timer_MM_Delta = 0;
    {
        const u32 time_mm = CPU::GetTicks();
        while (CPU::GetTicks() == time_mm)
            ; // wait for next tick
        const u32 time_system = CPU::GetTicks();
        const u32 time_local = TimerAsync();
        Timer_MM_Delta = time_system - time_local;
    }

    SDL_HideWindow(m_sdlWnd); // workaround for SDL bug
    UpdateWindowProps();
    SDL_ShowWindow(m_sdlWnd);
    SDL_RaiseWindow(m_sdlWnd);
}

void CRenderDevice::Shutdown()
{
    ZoneScoped;
    seqAppEnd.Process();
}

u32 app_inactive_time = 0;
u32 app_inactive_time_start = 0;

void CRenderDevice::FrameMove()
{
    ZoneScoped;

    dwFrame++;
    Core.dwFrame = dwFrame;
    dwTimeContinual = TimerMM.GetElapsed_ms() - app_inactive_time;

    fTimeDeltaReal = Timer.GetElapsed_sec();
    if (!_valid(fTimeDeltaReal))
        fTimeDeltaReal = EPS_S + EPS_S;
    Timer.Start(); // previous frame

    if (psDeviceFlags.test(rsConstantFPS))
    {
        // 20ms = 50fps
        // fTimeDelta = 0.020f;
        // fTimeGlobal += 0.020f;
        // dwTimeDelta = 20;
        // dwTimeGlobal += 20;
        // 33ms = 30fps
        fTimeDelta = 0.033f;
        fTimeGlobal += 0.033f;
        dwTimeDelta = 33;
        dwTimeGlobal += 33;
    }
    else
    {
        if (Paused())
            fTimeDelta = 0.0f;
        else
        {
            fTimeDelta = 0.1f * fTimeDelta + 0.9f * fTimeDeltaReal; // smooth random system activity - worst case ~7% error
            clamp(fTimeDelta, EPS_S + EPS_S, .1f); // limit to 10fps minimum
        }
        fTimeGlobal = TimerGlobal.GetElapsed_sec();
        const u32 _old_global = dwTimeGlobal;
        dwTimeGlobal = TimerGlobal.GetElapsed_ms();
        dwTimeDelta = dwTimeGlobal - _old_global;
    }
    ImGui::GetIO().DeltaTime = fTimeDeltaReal;

    m_imgui_render->Frame();
    ImGui::NewFrame();

    // Frame move
    stats.EngineTotal.FrameStart();
    stats.EngineTotal.Begin();
    // TODO: HACK to test loading screen.
    // if(!g_bLoaded)

    seqFrame.Process();

    g_bLoaded = true;
    // else
    // seqFrame.Process(rp_Frame);
    stats.EngineTotal.End();
    stats.EngineTotal.FrameEnd();

    ImGui::EndFrame();
}

ENGINE_API bool bShowPauseString = true;

void CRenderDevice::Pause(bool bOn, bool bTimer, bool bSound, [[maybe_unused]] pcstr reason)
{
    static int snd_emitters_ = -1;
    if (g_bBenchmark || GEnv.isDedicatedServer)
        return;

    if (bOn)
    {
        if (!Paused())
        {
            if (editor_mode())
                bShowPauseString = false;
#ifdef DEBUG
            else if (xr_strcmp(reason, "li_pause_key_no_clip") == 0)
                bShowPauseString = false;
#endif
            else
                bShowPauseString = true;
        }
        if (bTimer && (!g_pGamePersistent || g_pGamePersistent->CanBePaused()))
        {
            g_pauseMngr().Pause(true);
#ifdef DEBUG
            if (xr_strcmp(reason, "li_pause_key_no_clip") == 0)
                TimerGlobal.Pause(false);
#endif
        }
        if (bSound && GEnv.Sound)
            snd_emitters_ = GEnv.Sound->pause_emitters(true);
    }
    else
    {
        if (bTimer && g_pauseMngr().Paused())
        {
            fTimeDelta = EPS_S + EPS_S;
            g_pauseMngr().Pause(false);
        }
        if (bSound)
        {
            if (snd_emitters_ > 0) // avoid crash
                snd_emitters_ = GEnv.Sound->pause_emitters(false);
            else
            {
#ifdef DEBUG
                Log("GEnv.Sound->pause_emitters underflow");
#endif
            }
        }
    }
}

bool CRenderDevice::Paused() { return g_pauseMngr().Paused(); }

void CRenderDevice::OnWindowActivate(SDL_Window* window, bool activated)
{
    ZoneScoped;

    if (editor().GetState() == editor::ide::visible_state::full)
    {
        if (window != m_sdlWnd)
        {
            if (activated)
                editor().OnAppActivate();
            else
                editor().OnAppDeactivate();
        }
        return;
    }

    if (!GEnv.isDedicatedServer && activated)
        pInput->GrabInput(true);
    else
        pInput->GrabInput(false);

    const bool active = activated || !g_pause_in_background || psDeviceFlags.test(rsAlwaysActive);
    b_is_Active = active;

    // Keep worker tasks running while full background execution is enabled.
    if (active != b_is_InFocus)
    {
        b_is_InFocus = active;
        if (b_is_InFocus)
        {
            TaskScheduler->Pause(false);
            seqAppActivate.Process();
            app_inactive_time += TimerMM.GetElapsed_ms() - app_inactive_time_start;
        }
        else
        {
            app_inactive_time_start = TimerMM.GetElapsed_ms();
            seqAppDeactivate.Process();
            TaskScheduler->Pause(true);
        }
    }
}

void CRenderDevice::time_factor(const float time_factor)
{
    Timer.time_factor(time_factor);
    TimerGlobal.time_factor(time_factor);
    if (!strstr(Core.Params, "-sound_constant_speed"))
        psSoundTimeFactor = time_factor; //--#SM+#--
}

void CRenderDevice::AddSeqFrame(pureFrame* f, bool mt)
{
    if (mt)
        seqFrameMT.Add(f, REG_PRIORITY_HIGH);
    else
        seqFrame.Add(f, REG_PRIORITY_LOW);
}

void CRenderDevice::RemoveSeqFrame(pureFrame* f)
{
    seqFrameMT.Remove(f);
    seqFrame.Remove(f);
}

void CRenderDevice::script_register(lua_State* luaState)
{
    using namespace luabind;
    module(luaState)
    [
        class_<CRenderDevice>("render_device")
            .def_readonly("width", &CRenderDevice::dwWidth)
            .def_readonly("height", &CRenderDevice::dwHeight)
            .def_readonly("time_delta", &CRenderDevice::dwTimeDelta)
            .def_readonly("f_time_delta", &CRenderDevice::fTimeDelta)
            .def_readonly("cam_pos", &CRenderDevice::vCameraPosition)
            .def_readonly("cam_dir", &CRenderDevice::vCameraDirection)
            .def_readonly("cam_top", &CRenderDevice::vCameraTop)
            .def_readonly("cam_right", &CRenderDevice::vCameraRight)
            //			.def_readonly("view",					&CRenderDevice::mView)
            //			.def_readonly("projection",				&CRenderDevice::mProject)
            //			.def_readonly("full_transform",			&CRenderDevice::mFullTransform)
            .def_readonly("fov", &CRenderDevice::fFOV)
            .def_readonly("aspect_ratio", &CRenderDevice::fASPECT)
            .def_readonly("precache_frame", &CRenderDevice::dwPrecacheFrame)
            .def_readonly("frame", &CRenderDevice::dwFrame)
            .def("time_global", +[](const CRenderDevice* self)
            {
                return (self->dwTimeGlobal);
            })
            .def("is_paused", +[](CRenderDevice* device)
            {
                return device->Paused();
            })
            .def("pause", +[](CRenderDevice* device, bool b)
            {
                device->Pause(b, TRUE, FALSE, "set_device_paused_script");
            }),

        def("app_ready", +[]()
        {
            return g_pGamePersistent->IsLoaded();
        }),
        def("device", +[]()
        {
            return &Device;
        }),
        def("time_global", +[]()
        {
            return Device.dwTimeGlobal;
        }),
        def("time_global_async", +[]()
        {
            return Device.TimerAsync_MMT();
        })
    ];
};

void CLoadScreenRenderer::Start(bool b_user_input)
{
    Device.seqFrame.Add(this, 0);
    Device.seqRender.Add(this, 0);
    m_registered = true;
    m_need_user_input = b_user_input;

    g_pGamePersistent->ShowLoadingScreen(true);
    g_pGamePersistent->LoadBegin();
}

void CLoadScreenRenderer::Stop()
{
    if (!m_registered)
        return;
    Device.seqFrame.Remove(this);
    Device.seqRender.Remove(this);

    m_registered = false;
    m_need_user_input = false;

    g_pGamePersistent->ShowLoadingScreen(false);
    g_pGamePersistent->LoadEnd();
}

void CLoadScreenRenderer::OnFrame()
{
    g_pGamePersistent->LoadStage(false);
}

void CLoadScreenRenderer::OnRender()
{
    g_pGamePersistent->load_draw_internal();
}
