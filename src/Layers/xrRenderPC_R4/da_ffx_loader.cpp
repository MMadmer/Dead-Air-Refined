// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#ifdef XR_D3D12
#include "da_ffx_loader.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
const ffxFunctions* da_ffx_load()
{
    static ffxFunctions functions{};

    // By absolute path: neither the working directory nor the DLL search order is ours to trust.
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash || wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"amd_fidelityfx_loader_dx12.dll"))
    {
        Msg("! [FFX] the path of amd_fidelityfx_loader_dx12.dll does not fit: AMD FSR is off");
        return nullptr;
    }

    const HMODULE module = LoadLibraryW(path);
    if (!module)
    {
        Msg("! [FFX] amd_fidelityfx_loader_dx12.dll cannot be loaded (error %u): AMD FSR is off", u32(GetLastError()));
        return nullptr;
    }
    ffxLoadFunctions(&functions, module);
    if (!functions.CreateContext || !functions.DestroyContext || !functions.Configure || !functions.Query ||
        !functions.Dispatch)
    {
        Msg("! [FFX] amd_fidelityfx_loader_dx12.dll lacks ffx-api entry points: AMD FSR is off");
        return nullptr;
    }
    Msg("* [FFX] AMD FSR SDK loader ready");
    return &functions;
}
} // namespace

const ffxFunctions* da_ffx()
{
    // Contexts and the provider DLLs' threads may outlive any owner: the module stays loaded.
    static const ffxFunctions* functions = da_ffx_load();
    return functions;
}

u32 da_ffx_providers(u64 createDescType, ID3D12Device* device, pcstr what)
{
    const ffxFunctions* ffx = da_ffx();
    if (!ffx || !device)
        return 0;

    struct answer
    {
        u64 type;
        ID3D12Device* device;
        u32 count;
    };
    static answer answers[8];
    static u32 answer_count = 0;
    for (u32 i = 0; i < answer_count; ++i)
    {
        if (answers[i].type == createDescType && answers[i].device == device)
            return answers[i].count;
    }

    // The names live in the loader's memory: printed right away, never kept.
    constexpr u32 capacity = 8;
    uint64_t count = capacity;
    uint64_t ids[capacity]{};
    const char* names[capacity]{};
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = createDescType;
    query.device = device;
    query.outputCount = &count;
    query.versionIds = ids;
    query.versionNames = names;
    const ffxReturnCode_t code = ffx->Query(nullptr, &query.header);

    const u32 found = code == FFX_API_RETURN_OK ? u32(std::min<uint64_t>(count, capacity)) : 0;
    if (code != FFX_API_RETURN_OK)
        Msg("! [FFX] %s: the provider query failed (%u)", what, u32(code));
    else if (!found)
        Msg("! [FFX] %s: no provider runs on this adapter", what);
    else
    {
        string256 list{};
        for (u32 i = 0; i < found; ++i)
        {
            if (i)
                xr_strcat(list, ", ");
            xr_strcat(list, names[i] ? names[i] : "?");
        }
        Msg("* [FFX] %s providers: %s", what, list);
    }

    if (answer_count < std::size(answers))
        answers[answer_count++] = { createDescType, device, found };
    return found;
}
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_D3D12
