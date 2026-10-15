// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

// FFX DX12 through the loader DLL: providers and one upscaler context.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <ffx_framegeneration.h>

using Microsoft::WRL::ComPtr;

std::wstring ExeDir();
void Check(bool ok, const char* what);

static void ListVersions(const ffxFunctions& ffx, ID3D12Device* device, uint64_t descType, const char* what)
{
    uint64_t count = 0;
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = descType;
    query.device = device;
    query.outputCount = &count;
    ffxReturnCode_t rc = ffx.Query(nullptr, &query.header);
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    query.versionIds = ids.data();
    query.versionNames = names.data();
    if (count)
        rc = ffx.Query(nullptr, &query.header);
    std::printf("     %s providers: %llu (rc %u):", what, static_cast<unsigned long long>(count), rc);
    for (uint64_t i = 0; i < count; ++i)
        std::printf(" [%s]", names[i]);
    std::printf("\n");
    Check(rc == FFX_API_RETURN_OK && count > 0, what);
}

void TestFfxDx12(IDXGIAdapter1* adapter)
{
    const std::wstring loaderPath = ExeDir() + L"amd_fidelityfx_loader_dx12.dll";
    HMODULE loader = LoadLibraryExW(loaderPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Check(loader != nullptr, "LoadLibrary amd_fidelityfx_loader_dx12.dll (absolute path)");
    if (!loader)
        return;
    ffxFunctions ffx{};
    ffxLoadFunctions(&ffx, loader);
    Check(ffx.CreateContext && ffx.DestroyContext && ffx.Configure && ffx.Query && ffx.Dispatch, "ffxLoadFunctions: 5 entry points");

    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    Check(SUCCEEDED(hr), "D3D12 device");
    if (FAILED(hr))
        return;
    ListVersions(ffx, device.Get(), FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, "FFX upscaler");
    ListVersions(ffx, device.Get(), FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, "FFX frame generation");

    // A real upscaler context through the loader: proves the signed provider DLLs load.
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device.Get();
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.maxRenderSize = { 1280, 720 };
    create.maxUpscaleSize = { 1920, 1080 };
    create.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    ffxContext context = nullptr;
    ffxReturnCode_t rc = ffx.CreateContext(&context, &create.header, nullptr);
    std::printf("     ffxCreateContext(upscale) rc %u\n", rc);
    Check(rc == FFX_API_RETURN_OK, "ffxCreateContext upscale (DX12)");
    if (rc == FFX_API_RETURN_OK)
    {
        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffx.Query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
            std::printf("     active upscaler provider: %s\n", provider.versionName);
        Check(ffx.DestroyContext(&context, nullptr) == FFX_API_RETURN_OK, "ffxDestroyContext upscale");
    }
}

