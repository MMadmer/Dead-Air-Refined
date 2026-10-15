// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The DirectX 12 renderer module. It is the R4 renderer built against xrD3D12 (see
// docs/dead-air/DX12_RENDERER.md): one mode, offered where the first adapter can run D3D12.
#include "stdafx.h"

#include "Layers/xrRender/dxRenderFactory.h"
#include "Layers/xrRender/dxUIRender.h"
#include "Layers/xrRender/dxDebugRender.h"
#include "Layers/xrRender/D3DUtils.h"

#include "Include/xrRender/xrRender.h"

namespace xray::render::RENDER_NAMESPACE
{
// Id 7: scripts compare renderer ids, 6 was OpenGL and anything from 4 up takes the R3+ paths.
constexpr pcstr RENDERER_R5_MODE = "renderer_r5"; // id 7

// r2_test_hw.cpp: whether adapter 0 passes xrd3d12::ProbeAdapter (cached per hardware).
bool xrRender_test_d3d12();

class R5RendererModule final : public RendererModule
{
    xr_vector<std::pair<pcstr, int>> modes;

public:
    const xr_vector<std::pair<pcstr, int>>& ObtainSupportedModes() override
    {
        ZoneScoped;

        if (modes.empty() && xrRender_test_d3d12())
            modes.emplace_back(RENDERER_R5_MODE, 7);
        return modes;
    }

    bool CheckGameRequirements() override
    {
        // Check if shaders are available
        if (!FS.exist("$game_shaders$", RImplementation.getShaderPath()))
        {
            Log("~ No shaders found for xrRender_R5");
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
} static s_r5_module;

RendererModule* GetRendererModule()
{
    return &s_r5_module;
}
} // namespace xray::render::RENDER_NAMESPACE
