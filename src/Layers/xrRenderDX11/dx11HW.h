#pragma once

#include "xrCore/ModuleLookup.hpp"

#include "Layers/xrRender/HWCaps.h"
#include "Layers/xrRender/stats_manager.h"

#include <SDL.h>

namespace xray::render::RENDER_NAMESPACE
{
class CHW
    : public pureAppActivate,
      public pureAppDeactivate
{
public:
    CHW();
    ~CHW();

    void CreateD3D();
    void DestroyD3D();

    void CreateDevice(SDL_Window* sdlWnd);
    void DestroyDevice();

    void Reset();

    void SetPrimaryAttributes(u32& windowFlags);

    std::pair<u32, u32> GetSurfaceSize() const;

    bool CheckFormatSupport(DXGI_FORMAT format, u32 feature) const;
    DXGI_FORMAT SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT formats[], size_t count) const;
    template <size_t count>
    inline DXGI_FORMAT SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT (&formats)[count]) const
    {
        return SelectFormat(feature, formats, count);
    }
    bool UsingFlipPresentationModel() const;
    DeviceState GetDeviceState();

public:
    void BeginScene();
    void EndScene();
    void Present();

public:
    void OnAppActivate() override;
    void OnAppDeactivate() override;

private:
    bool CreateSwapChain(HWND hwnd);
    bool CreateSwapChain2(HWND hwnd);
    bool UpdateChainDesc();

    // Set when the swap chain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, which every
    // windowed present without vsync then has to repeat.
    bool m_tearingSupported{};

    bool ThisInstanceIsGlobal() const;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool HandleDwmMessage(UINT message, LPARAM lParam, LRESULT& result);
    void InstallWindowProc(HWND hwnd);
    void RestoreWindowProc();

    void UpdateBackgroundPreview();
    void SetBackgroundPreviewEnabled(bool enabled);
    bool EnsureBackgroundPreviewResources(ID3D11Texture2D* source);
    void ReleaseBackgroundPreviewResources();
    HBITMAP CreateScaledBackgroundPreview(u32 maxWidth, u32 maxHeight) const;

public:
    ICF ID3DDeviceContext* get_context(u32 context_id)
    {
        VERIFY(context_id < R__NUM_CONTEXTS);
        return d3d_contexts_pool[context_id];
    }

public:
    static constexpr auto IMM_CTX_ID = R__NUM_PARALLEL_CONTEXTS;

    CHWCaps Caps;

    u32 BackBufferCount{};
    u32 CurrentBackBuffer{};

    ID3DDevice* pDevice = nullptr; // render device

    D3D_DRIVER_TYPE m_DriverType;

    IDXGIFactory1* m_pFactory = nullptr;
    IDXGIAdapter1* m_pAdapter = nullptr; // pD3D equivalent
    IDXGISwapChain* m_pSwapChain = nullptr;
    D3D_FEATURE_LEVEL FeatureLevel;
    bool Valid = true;
    bool ComputeShadersSupported{};
    bool DoublePrecisionFloatShaderOps{};
    bool SAD4ShaderInstructions{};
    bool ExtendedDoublesShaderInstructions{};

    ID3DDeviceContext* d3d_contexts_pool[R__NUM_CONTEXTS]{};

    bool DX10Only = false;
#ifdef HAS_DX11_2
    IDXGISwapChain2* m_pSwapChain2 = nullptr;
#endif
#ifdef HAS_DX11_3
    ID3D11Device3* pDevice3 = nullptr;
#endif
    ID3D11DeviceContext1* pContext1 = nullptr;

    using D3DCompileFunc = decltype(&D3DCompile);
    D3DCompileFunc D3DCompile = nullptr;

#if !defined(_MAYA_EXPORT)
    stats_manager stats_manager;
#endif
    TracyD3D11Ctx profiler_ctx{}; // TODO: this should be one per d3d11 context
private:
    enum class PresentTestState : u8
    {
        None,
        Occluded,
        DeviceReset,
        DeviceRemoved
    };

    DXGI_SWAP_CHAIN_DESC m_ChainDesc; // DevPP equivalent
    PresentTestState presentTestState{};

    HWND outputWindow{};
    WNDPROC previousWindowProc{};
    bool backgroundPreviewEnabled{};
    ID3D11Texture2D* backgroundPreviewStaging{};
    ID3D11Query* backgroundPreviewReady{};
    HBITMAP backgroundPreviewBitmap{};
    void* backgroundPreviewBits{};
    DXGI_FORMAT backgroundPreviewFormat{DXGI_FORMAT_UNKNOWN};
    u32 backgroundPreviewWidth{};
    u32 backgroundPreviewHeight{};
    bool backgroundPreviewCopyPending{};
    u64 backgroundPreviewLastCapture{};
    u64 backgroundPreviewRequestedUntil{};

    XRay::Module hD3DCompiler;
    XRay::Module hDXGI;
    XRay::Module hD3D;
};

extern ECORE_API CHW HW;
} // namespace xray::render::RENDER_NAMESPACE
