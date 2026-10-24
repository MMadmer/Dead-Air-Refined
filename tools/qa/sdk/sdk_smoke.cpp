// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

// Smoke test of the vendored SDKs: FSR 3.1.2 DX11 static libraries (context create, three
// dispatches, destroy), the FFX DX12 loader and its providers, Streamline through the
// delay-loaded interposer.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_security.h>
#include <sl_helpers.h>

using Microsoft::WRL::ComPtr;

int headers_dx11_marker();
int headers_dx12_marker();
void TestFfxDx12(IDXGIAdapter1* adapter);
std::wstring ExeDir();
void Check(bool ok, const char* what);

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\') + 1);
}

static int failures = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    ComPtr<ID3D11Texture2D> tex;
    device->CreateTexture2D(&desc, nullptr, &tex);
    return tex;
}

static void TestFsr3Dx11()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);
    Check(SUCCEEDED(hr), "D3D11 device");
    if (FAILED(hr))
        return;

    const size_t scratchSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
    std::vector<unsigned char> scratch(scratchSize);
    FfxFsr3UpscalerContextDescription desc{};
    FfxErrorCode err = ffxGetInterfaceDX11(&desc.backendInterface, ffxGetDeviceDX11_Fsr31(device.Get()), scratch.data(), scratchSize, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    Check(err == FFX_OK, "ffxGetInterfaceDX11");
    desc.flags = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize = { 1280, 720 };
    desc.maxUpscaleSize = { 1920, 1080 };
    FfxFsr3UpscalerContext fsr{};
    err = ffxFsr3UpscalerContextCreate(&fsr, &desc);
    std::printf("     ffxFsr3UpscalerContextCreate = 0x%x (scratch %zu bytes)\n", err, scratchSize);
    Check(err == FFX_OK, "ffxFsr3UpscalerContextCreate (FSR 3.1.2, DX11, every pass pipeline)");
    if (err != FFX_OK)
        return;

    auto color = MakeTexture(device.Get(), 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto depth = MakeTexture(device.Get(), 1280, 720, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    auto motion = MakeTexture(device.Get(), 1280, 720, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    auto output = MakeTexture(device.Get(), 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    Check(color && depth && motion && output, "D3D11 test textures");

    FfxFsr3UpscalerDispatchDescription dispatch{};
    dispatch.commandList = ffxGetCommandListDX11(context.Get());
    dispatch.color = ffxGetResourceDX11_Fsr31(color.Get(), GetFfxResourceDescriptionDX11(color.Get()), L"color", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ffxGetResourceDX11_Fsr31(depth.Get(), GetFfxResourceDescriptionDX11(depth.Get()), L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxGetResourceDX11_Fsr31(motion.Get(), GetFfxResourceDescriptionDX11(motion.Get()), L"motion", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = ffxGetResourceDX11_Fsr31(output.Get(), GetFfxResourceDescriptionDX11(output.Get()), L"output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = { 0.0f, 0.0f };
    dispatch.motionVectorScale = { -0.5f * 1280.0f, 0.5f * 720.0f };
    dispatch.renderSize = { 1280, 720 };
    dispatch.upscaleSize = { 1920, 1080 };
    dispatch.enableSharpening = true;
    dispatch.sharpness = 0.3f;
    dispatch.frameTimeDelta = 16.6f;
    dispatch.preExposure = 1.0f;
    dispatch.reset = true;
    dispatch.cameraNear = 0.2f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.0f;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    for (int frame = 0; frame < 3 && err == FFX_OK; ++frame)
    {
        err = ffxFsr3UpscalerContextDispatch(&fsr, &dispatch);
        dispatch.reset = false;
    }
    context->Flush();
    Check(err == FFX_OK, "ffxFsr3UpscalerContextDispatch x3");
    Check(device->GetDeviceRemovedReason() == S_OK, "device alive after dispatch");

    err = ffxFsr3UpscalerContextDestroy(&fsr);
    Check(err == FFX_OK, "ffxFsr3UpscalerContextDestroy");
}

static void SlLog(sl::LogType type, const char* msg)
{
    if (type != sl::LogType::eInfo)
        std::printf("     [sl] %s", msg);
}

static void TestStreamline(IDXGIAdapter1* adapter)
{
    const std::wstring dir = ExeDir();
    const std::wstring interposer = dir + L"sl.interposer.dll";
    Check(sl::security::verifyEmbeddedSignature(interposer.c_str()), "sl.interposer.dll NVIDIA signature");
    HMODULE module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Check(module != nullptr, "LoadLibrary sl.interposer.dll (absolute path, before the delay-load binds)");
    if (!module)
        return;

    const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    const wchar_t* pluginPaths[] = { dir.c_str() };
    sl::Preferences prefs{};
    prefs.logLevel = sl::LogLevel::eDefault;
    prefs.logMessageCallback = SlLog;
    prefs.pathsToPlugins = pluginPaths;
    prefs.numPathsToPlugins = 1;
    prefs.pathToLogsAndData = nullptr;
    // No OTA downloads from a smoke test.
    prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = 4;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "1.6.3";
    // A test-only project id; the game passes its own.
    prefs.projectId = "6f0f3e0a-2f7c-4a55-9b7e-3d2b8a4c1e90";
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result res = slInit(prefs);
    std::printf("     slInit = %s\n", sl::getResultAsStr(res));
    Check(res == sl::Result::eOk, "slInit (delay-loaded sl.interposer.lib)");
    if (res != sl::Result::eOk)
        return;

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    sl::AdapterInfo info{};
    info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
    info.deviceLUIDSizeInBytes = sizeof(LUID);
    const struct { sl::Feature id; const char* name; } list[] = {
        { sl::kFeatureDLSS, "DLSS" }, { sl::kFeatureDLSS_G, "DLSS-G" }, { sl::kFeatureReflex, "Reflex" }, { sl::kFeaturePCL, "PCL" } };
    for (const auto& f : list)
    {
        const sl::Result supported = slIsFeatureSupported(f.id, info);
        sl::FeatureRequirements req{};
        const sl::Result reqRes = slGetFeatureRequirements(f.id, req);
        std::printf("     %-7s supported=%s requirements=%s flags=0x%x driver %u.%u (needs %u.%u)\n", f.name, sl::getResultAsStr(supported),
            sl::getResultAsStr(reqRes), static_cast<unsigned>(req.flags), req.driverVersionDetected.major, req.driverVersionDetected.minor,
            req.driverVersionRequired.major, req.driverVersionRequired.minor);
    }
    Check(slShutdown() == sl::Result::eOk, "slShutdown");
}

int main()
{
    std::printf("header sets compile: %d %d\n", headers_dx11_marker(), headers_dx12_marker());
    TestFsr3Dx11();

    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
    {
        std::printf("FAIL no DXGI 1.6 adapter\n");
        return 1;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::printf("adapter: %ls\n", desc.Description);

    TestFfxDx12(adapter.Get());
    TestStreamline(adapter.Get());

    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
