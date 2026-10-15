// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

// AMD::FidelityFXLoader: the ffx:: C++ helpers link against the delay-loaded loader import library.
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <string>

#include <ffx_api.hpp>
#include <dx12/ffx_api_dx12.hpp>
#include <ffx_upscale.hpp>

int main()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    // The delay-load helper binds to the copy already loaded by its absolute path.
    if (!LoadLibraryExW((dir + L"amd_fidelityfx_loader_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
    {
        std::printf("FAIL loader not found next to the exe\n");
        return 1;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        std::printf("FAIL D3D12 device\n");
        return 1;
    }
    ffx::QueryDescGetVersions query{};
    uint64_t count = 0;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = device;
    query.outputCount = &count;
    const ffx::ReturnCode rc = ffx::Query(query);
    std::printf("%s ffx::Query upscaler versions: %llu\n", (rc == ffx::ReturnCode::Ok && count) ? "OK  " : "FAIL", static_cast<unsigned long long>(count));
    device->Release();
    return (rc == ffx::ReturnCode::Ok && count) ? 0 : 1;
}
