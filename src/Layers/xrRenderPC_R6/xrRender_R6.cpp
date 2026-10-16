// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The Vulkan renderer module. It is the R4 renderer built against xrVulkan (see
// docs/dead-air/VULKAN_RENDERER.md): one mode, offered where the adapter runs Vulkan 1.3 with what the
// layer needs.
#include "stdafx.h"

#include "Layers/xrRender/dxRenderFactory.h"
#include "Layers/xrRender/dxUIRender.h"
#include "Layers/xrRender/dxDebugRender.h"
#include "Layers/xrRender/D3DUtils.h"

#include "Include/xrRender/xrRender.h"

namespace xray::render::RENDER_NAMESPACE
{
// Id 8: scripts compare renderer ids; 6 was OpenGL, 7 is DirectX 12, anything from 4 up takes the R3+ paths.
constexpr pcstr RENDERER_R6_MODE = "renderer_r6"; // id 8

// r2_test_hw.cpp: whether the engine's adapter passes xrvk::ProbeAdapter (cached per hardware).
bool xrRender_test_vulkan();

class R6RendererModule final : public RendererModule
{
    xr_vector<std::pair<pcstr, int>> modes;

public:
    const xr_vector<std::pair<pcstr, int>>& ObtainSupportedModes() override
    {
        ZoneScoped;

        if (modes.empty() && xrRender_test_vulkan())
            modes.emplace_back(RENDERER_R6_MODE, 8);
        return modes;
    }

    bool CheckGameRequirements() override
    {
        if (!FS.exist("$game_shaders$", RImplementation.getShaderPath()))
        {
            Log("~ No shaders found for xrRender_R6");
            return false;
        }
        return true;
    }

    void SetupEnv(pcstr mode) override
    {
        ZoneScoped;
        UNUSED(mode);

        // What R4's own mode sets up: a feature level 11 device and the advanced post-processing.
        ps_r2_sun_static = false;
        ps_r2_advanced_pp = true;

        GEnv.Render = &RImplementation;
        GEnv.RenderFactory = &RenderFactoryImpl;
        GEnv.DU = &DUImpl;
        GEnv.UIRender = &UIRenderImpl;
#ifdef DEBUG
        GEnv.DRender = &DebugRenderImpl;
        rdebug_render->Register();
#endif
        xrRender_initconsole();
    }

    void ClearEnv() override
    {
        modes.clear();

        if (GEnv.Render == &RImplementation)
        {
            GEnv.Render = nullptr;
            GEnv.RenderFactory = nullptr;
            GEnv.DU = nullptr;
            GEnv.UIRender = nullptr;
            GEnv.DRender = nullptr;
#ifdef DEBUG
            rdebug_render->Unregister();
#endif
        }
    }
} static s_r6_module;

RendererModule* GetRendererModule()
{
    return &s_r6_module;
}
} // namespace xray::render::RENDER_NAMESPACE
