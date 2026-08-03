#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
constexpr u32 HARDWARE_CACHE_MAGIC = 0x31434852;
constexpr u32 HARDWARE_CACHE_VERSION = 1;

bool GetHardwareSignature(u32& signature)
{
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi)
        return false;

    using CreateFactoryFunction = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory =
        reinterpret_cast<CreateFactoryFunction>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!createFactory)
    {
        FreeLibrary(dxgi);
        return false;
    }

    IDXGIFactory1* factory{};
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    {
        FreeLibrary(dxgi);
        return false;
    }

    signature = crc32(&HARDWARE_CACHE_VERSION, sizeof(HARDWARE_CACHE_VERSION));
    u32 adapterCount = 0;
    bool valid = true;
    for (UINT index = 0;; ++index)
    {
        IDXGIAdapter1* adapter{};
        const HRESULT enumerationResult = factory->EnumAdapters1(index, &adapter);
        if (enumerationResult == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(enumerationResult) || !adapter)
        {
            valid = false;
            break;
        }

        DXGI_ADAPTER_DESC1 description{};
        LARGE_INTEGER driverVersion{};
        valid = SUCCEEDED(adapter->GetDesc1(&description));
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion);
        adapter->Release();
        if (!valid)
            break;

        signature = crc32(&description, sizeof(description), signature);
        signature = crc32(&driverVersion, sizeof(driverVersion), signature);
        ++adapterCount;
    }

    factory->Release();
    FreeLibrary(dxgi);
    if (!valid || !adapterCount)
        return false;

    signature = crc32(&adapterCount, sizeof(adapterCount), signature);
    return true;
}

bool LoadHardwareCache(pcstr cachePath, u32 signature, BOOL& result)
{
    IReader* reader = FS.r_open(cachePath);
    if (!reader)
        return false;

    bool valid = reader->elapsed() == static_cast<intptr_t>(sizeof(u32) * 4);
    u32 cachedResult = 0;
    if (valid)
    {
        valid = reader->r_u32() == HARDWARE_CACHE_MAGIC;
        valid = valid && reader->r_u32() == HARDWARE_CACHE_VERSION;
        valid = valid && reader->r_u32() == signature;
        cachedResult = reader->r_u32();
        valid = valid && cachedResult <= 2 && reader->eof();
    }
    FS.r_close(reader);

    if (!valid)
        return false;

    result = static_cast<BOOL>(cachedResult);
    return true;
}

void SaveHardwareCache(pcstr cachePath, u32 signature, BOOL result)
{
    CMemoryWriter writer;
    writer.w_u32(HARDWARE_CACHE_MAGIC);
    writer.w_u32(HARDWARE_CACHE_VERSION);
    writer.w_u32(signature);
    writer.w_u32(static_cast<u32>(result));

    xr_string temporaryPath = cachePath;
    temporaryPath += ".tmp";
    if (writer.save_to(temporaryPath.c_str()))
        FS.file_rename(temporaryPath.c_str(), cachePath, true);
}
}

class DX11TestHelper
{
    SDL_Window* m_window = nullptr;
    CHW m_hw;

public:
    DX11TestHelper()
    {
        m_window = SDL_CreateWindow("TestDX11Window", 0, 0, 1, 1, SDL_WINDOW_HIDDEN);
        if (!m_window)
        {
            Log("~ Cannot create helper window for DirectX 11 test:", SDL_GetError());
            return;
        }
        m_hw.CreateDevice(m_window);
    }

    bool Successful() const
    {
        return m_window && m_hw.Valid;
    }

    const CHW& GetHW() const { return m_hw; }

    ~DX11TestHelper()
    {
        m_hw.DestroyDevice();
        SDL_DestroyWindow(m_window);
    }
};


BOOL xrRender_test_hw()
{
    ZoneScoped;

    string_path cachePath;
    FS.update_path(cachePath, "$app_data_root$", "render-hardware.cache");
    u32 signature = 0;
    BOOL result = FALSE;
    const bool signatureAvailable = GetHardwareSignature(signature);
    if (signatureAvailable && LoadHardwareCache(cachePath, signature, result))
        return result;

    const DX11TestHelper helper;
    if (!helper.Successful())
        return FALSE;

    const auto level = helper.GetHW().FeatureLevel;
    if (level >= D3D_FEATURE_LEVEL_11_0)
        result = TRUE + TRUE; // XXX: remove hack
    else if (level >= D3D_FEATURE_LEVEL_10_0)
        result = TRUE;

    if (signatureAvailable)
        SaveHardwareCache(cachePath, signature, result);
    return result;
}
} // namespace xray::render::RENDER_NAMESPACE
