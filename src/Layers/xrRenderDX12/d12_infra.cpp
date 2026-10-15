// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Infrastructure of xrD3D12: logging, hashing, format helpers, the d3d12.dll loader, fences,
// deferred release, descriptor heaps and rings, sampler tables, null descriptors, upload and
// buffer memory, creation-time uploads, root signatures, the PSO cache and query heaps.
#include "d12_layer.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <unordered_set>

namespace xrd3d12
{
namespace
{
constexpr u64 kMB = 1ull << 20;
constexpr u64 kUploadBudgetBytes = 512 * kMB;  // dynamic upload memory in flight before a warning
constexpr u64 kInitAutoFlushBytes = 256 * kMB; // pending creation-time copies before a forced submit
constexpr size_t kMaxFreeUploadPages = 128;    // pooled dynamic pages kept after a spike (256 MB)
constexpr size_t kMaxFreeDedicatedPages = 8;   // pooled oversized pages (4 MB dynamic VB renames, video)
constexpr u64 kMaxPooledDedicatedBytes = 64ull << 20;
constexpr size_t kMaxFreeInitPages = 4;        // pooled creation-time pages (128 MB)
constexpr u64 kPageAlignment = 64 * 1024;
constexpr u32 kQueriesPerHeap = 4096;
constexpr u64 kZeroCbSize = 64 * 1024;
constexpr u64 kPsoHashVersion = 1; // bump when the PSO description built from the same key changes
constexpr u64 kComputeKeySalt = 0x0000435330313264ull;
constexpr u64 kNativeComputeKeySalt = 0x4e61744353303031ull; // native pipelines never share engine keys
constexpr u64 kNativeStreamKeySalt = 0x4e61745374723031ull;
constexpr LONGLONG kMaxLibraryFileBytes = 1ll << 30;

u64 AlignUp(u64 value, u64 alignment)
{
    if (alignment <= 1)
        return value;
    if (!(alignment & (alignment - 1)))
        return (value + alignment - 1) & ~(alignment - 1);
    return (value + alignment - 1) / alignment * alignment;
}

const void* KeyOf(u64 value) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(value)); }

// ---- logging ----------------------------------------------------------------------------------

std::atomic<LogSink> g_logSink{nullptr};
std::mutex g_logLock;
std::mutex g_onceLock;

// Never destroyed: LogOnce may still run while other statics are torn down.
std::unordered_set<const void*>& OnceKeys()
{
    static auto* keys = new std::unordered_set<const void*>();
    return *keys;
}

void WriteLine(const char* fmt, va_list args)
{
    constexpr char kPrefix[] = "[d3d12] ";
    constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
    // The engine log colours a line by its first character: a leading "! ", "~ " or "* " marker
    // of the message goes in front of the prefix.
    constexpr size_t kMarkerLength = 2;
    char text[2048];
    const int written = std::vsnprintf(text, sizeof(text), fmt ? fmt : "", args);
    size_t textLength = written < 0 ? 0 : (std::min)(size_t(written), sizeof(text) - 1);
    size_t marker = 0;
    if (textLength >= kMarkerLength && (text[0] == '!' || text[0] == '~' || text[0] == '*') && text[1] == ' ')
        marker = kMarkerLength;

    // Room for the text, then '\n' and the terminator for OutputDebugString.
    char line[sizeof(text) + kPrefixLength + 2];
    size_t length = 0;
    std::memcpy(line, text, marker);
    length += marker;
    std::memcpy(line + length, kPrefix, kPrefixLength);
    length += kPrefixLength;
    std::memcpy(line + length, text + marker, textLength - marker);
    length += textLength - marker;
    while (length > kPrefixLength + marker && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        --length;
    line[length] = 0;

    const LogSink sink = g_logSink.load(std::memory_order_acquire);
    try
    {
        std::lock_guard lock(g_logLock);
        if (sink)
            sink(line);
        line[length] = '\n';
        line[length + 1] = 0;
        OutputDebugStringA(line);
    }
    catch (...)
    {
    }
}

// ---- memory helpers ---------------------------------------------------------------------------

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

D3D12_RESOURCE_DESC BufferDesc(u64 size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

// Committed buffer in the state its heap requires; `cpu` (optional) receives a persistent mapping.
HRESULT CreateCommittedBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, u64 size, ID3D12Resource** resource, u8** cpu)
{
    *resource = nullptr;
    if (cpu)
        *cpu = nullptr;
    const D3D12_HEAP_PROPERTIES props = HeapProperties(type);
    const D3D12_RESOURCE_DESC desc = BufferDesc(size);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (type == D3D12_HEAP_TYPE_UPLOAD || type == D3D12_HEAP_TYPE_GPU_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (type == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    HRESULT hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(resource));
    if (FAILED(hr))
    {
        *resource = nullptr;
        return hr;
    }
    if (cpu)
    {
        // Upload memory is never read by the CPU; readback memory is read anywhere.
        const D3D12_RANGE nothing{0, 0};
        void* mapped = nullptr;
        hr = (*resource)->Map(0, type == D3D12_HEAP_TYPE_READBACK ? nullptr : &nothing, &mapped);
        if (FAILED(hr))
        {
            (*resource)->Release();
            *resource = nullptr;
            return hr;
        }
        *cpu = static_cast<u8*>(mapped);
    }
    return S_OK;
}

UploadPage* CreateUploadPage(ID3D12Device* device, u64 size, HRESULT* result = nullptr)
{
    auto* page = new (std::nothrow) UploadPage;
    if (!page)
    {
        if (result)
            *result = E_OUTOFMEMORY;
        return nullptr;
    }
    const HRESULT hr = CreateCommittedBuffer(device, D3D12_HEAP_TYPE_UPLOAD, size, &page->resource, &page->cpu);
    if (result)
        *result = hr;
    if (FAILED(hr))
    {
        delete page;
        return nullptr;
    }
    page->gpu = page->resource->GetGPUVirtualAddress();
    page->size = size;
    return page;
}

void DestroyUploadPage(UploadPage* page)
{
    if (!page)
        return;
    if (page->resource)
        page->resource->Release();
    delete page;
}

// ---- descriptors ------------------------------------------------------------------------------

bool GrowCpuHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 blockSize, u32 increment,
    std::vector<ID3D12DescriptorHeap*>& blocks, std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& free)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = blockSize;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* heap = nullptr;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr))
    {
        Log("CPU descriptor heap (type %d, %u descriptors) failed (0x%08X)", int(type), blockSize, unsigned(hr));
        return false;
    }
    try
    {
        // The free list can never hold more than every descriptor: Free() then never allocates.
        blocks.reserve(blocks.size() + 1);
        free.reserve((blocks.size() + 1) * size_t(blockSize));
    }
    catch (...)
    {
        heap->Release();
        return false;
    }
    blocks.push_back(heap);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = heap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = blockSize; i-- > 0;)
        free.push_back({base.ptr + SIZE_T(i) * increment});
    return true;
}

// ---- pipelines --------------------------------------------------------------------------------

std::mutex g_libraryLock; // ID3D12PipelineLibrary loads of one name must not race

D3D12_BLEND_DESC DefaultBlend()
{
    D3D12_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    for (D3D12_RENDER_TARGET_BLEND_DESC& rt : desc.RenderTarget)
    {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = UINT8(D3D12_COLOR_WRITE_ENABLE_ALL);
    }
    return desc;
}

D3D12_RASTERIZER_DESC DefaultRaster()
{
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_BACK;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = 0;
    desc.DepthBiasClamp = 0.0f;
    desc.SlopeScaledDepthBias = 0.0f;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

D3D12_DEPTH_STENCIL_DESC DefaultDepth()
{
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.BackFace = desc.FrontFace;
    return desc;
}

D3D12_SHADER_BYTECODE Bytecode(const Shader* shader)
{
    if (!shader || shader->bytecode.empty())
        return {};
    return {shader->bytecode.data(), shader->bytecode.size()};
}

void PipelineName(u64 key, wchar_t (&name)[17])
{
    std::swprintf(name, 17, L"%016llx", static_cast<unsigned long long>(key));
}

bool HasStencil(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        format == DXGI_FORMAT_R24G8_TYPELESS || format == DXGI_FORMAT_R32G8X24_TYPELESS;
}

bool ReadWholeFile(const std::wstring& path, std::vector<u8>& out)
{
    out.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < kMaxLibraryFileBytes;
    if (ok)
    {
        try
        {
            out.resize(size_t(size.QuadPart));
            DWORD read = 0;
            ok = ReadFile(file, out.data(), DWORD(size.QuadPart), &read, nullptr) && read == DWORD(size.QuadPart);
        }
        catch (...)
        {
            ok = false;
        }
    }
    CloseHandle(file);
    if (!ok)
        out.clear();
    return ok;
}

void CreateParentDirectories(const std::wstring& path)
{
    for (size_t pos = path.find_first_of(L"\\/", 1); pos != std::wstring::npos; pos = path.find_first_of(L"\\/", pos + 1))
        CreateDirectoryW(path.substr(0, pos).c_str(), nullptr);
}

bool WriteWholeFile(const std::wstring& path, const u8* data, size_t size)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, DWORD(size), &written, nullptr) && written == DWORD(size);
    CloseHandle(file);
    return ok;
}

// ---- root signatures --------------------------------------------------------------------------

struct RootLayout
{
    std::array<D3D12_DESCRIPTOR_RANGE1, 32> ranges{};
    std::array<D3D12_ROOT_PARAMETER1, 32> params{};
    u32 rangeCount = 0;
    u32 paramCount = 0;

    void RootCbv(u32 index, u32 reg, D3D12_SHADER_VISIBILITY visibility)
    {
        D3D12_ROOT_PARAMETER1& p = params[index];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        p.ShaderVisibility = visibility;
        paramCount = (std::max)(paramCount, index + 1);
    }

    void Table(u32 index, D3D12_DESCRIPTOR_RANGE_TYPE type, u32 base, u32 count, D3D12_SHADER_VISIBILITY visibility)
    {
        D3D12_DESCRIPTOR_RANGE1& r = ranges[rangeCount++];
        r.RangeType = type;
        r.NumDescriptors = count;
        r.BaseShaderRegister = base;
        r.RegisterSpace = 0;
        // D3D11 tables are written partially and change between draws of one list. Sampler
        // ranges accept no DATA_* flag.
        r.Flags = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
            ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
            : D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        r.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER1& p = params[index];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = &r;
        p.ShaderVisibility = visibility;
        paramCount = (std::max)(paramCount, index + 1);
    }

    // Root CBVs b0..b(rootCbvs-1), a CBV table for the rest up to b13, SRV and sampler tables.
    void Stage(u32 cbv0, u32 rootCbvs, u32 cbvTable, u32 srv, u32 smp, D3D12_SHADER_VISIBILITY visibility)
    {
        for (u32 i = 0; i < rootCbvs; ++i)
            RootCbv(cbv0 + i, i, visibility);
        Table(cbvTable, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, rootCbvs, kMaxCBs - rootCbvs, visibility);
        Table(srv, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, kMaxSRVs, visibility);
        Table(smp, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, kMaxSamplers, visibility);
    }
};

static_assert(GfxRoot::VS_CBV_TABLE == GfxRoot::VS_CBV0 + kRootCBVsVS && GfxRoot::PS_CBV_TABLE == GfxRoot::PS_CBV0 + kRootCBVsPS);
static_assert(GfxRoot::GS_CBV_TABLE == GfxRoot::GS_CBV0 + kRootCBVsOther && GfxRoot::HS_CBV_TABLE == GfxRoot::HS_CBV0 + kRootCBVsOther &&
    GfxRoot::DS_CBV_TABLE == GfxRoot::DS_CBV0 + kRootCBVsOther);
static_assert(CsRoot::CBV_TABLE == CsRoot::CBV0 + kRootCBVsCS && GfxRoot::Count == GfxRoot::DS_SMP + 1 && CsRoot::Count == CsRoot::SMP + 1);

ID3D12RootSignature* BuildRootSignature(ID3D12Device* device, const RootLayout& layout, D3D12_ROOT_SIGNATURE_FLAGS flags,
    bool version11, const wchar_t* name)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = E_NOTIMPL;
    if (version11 && g_d3d12.SerializeVersionedRootSignature)
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = layout.paramCount;
        desc.Desc_1_1.pParameters = layout.params.data();
        desc.Desc_1_1.NumStaticSamplers = 0;
        desc.Desc_1_1.pStaticSamplers = nullptr;
        desc.Desc_1_1.Flags = flags;
        hr = g_d3d12.SerializeVersionedRootSignature(&desc, &blob, &errors);
    }
    else if (g_d3d12.SerializeRootSignature)
    {
        // Version 1.0 defaults already treat descriptors as volatile.
        std::array<D3D12_DESCRIPTOR_RANGE, 32> ranges{};
        std::array<D3D12_ROOT_PARAMETER, 32> params{};
        for (u32 i = 0; i < layout.paramCount; ++i)
        {
            const D3D12_ROOT_PARAMETER1& src = layout.params[i];
            D3D12_ROOT_PARAMETER& dst = params[i];
            dst.ParameterType = src.ParameterType;
            dst.ShaderVisibility = src.ShaderVisibility;
            if (src.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            {
                const D3D12_DESCRIPTOR_RANGE1& r = src.DescriptorTable.pDescriptorRanges[0];
                ranges[i] = {r.RangeType, r.NumDescriptors, r.BaseShaderRegister, r.RegisterSpace, r.OffsetInDescriptorsFromTableStart};
                dst.DescriptorTable.NumDescriptorRanges = 1;
                dst.DescriptorTable.pDescriptorRanges = &ranges[i];
            }
            else
            {
                dst.Descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
                dst.Descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
            }
        }
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = layout.paramCount;
        desc.pParameters = params.data();
        desc.Flags = flags;
        hr = g_d3d12.SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors);
    }
    if (FAILED(hr) || !blob)
    {
        if (errors)
            Log("root signature (version %s) serialization failed (0x%08X): %.*s", version11 ? "1.1" : "1.0", unsigned(hr),
                int(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
        else
            Log("root signature (version %s) serialization failed (0x%08X)", version11 ? "1.1" : "1.0", unsigned(hr));
        if (errors)
            errors->Release();
        if (blob)
            blob->Release();
        return nullptr;
    }
    if (errors)
        errors->Release();
    ID3D12RootSignature* signature = nullptr;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature));
    blob->Release();
    if (FAILED(hr))
    {
        Log("CreateRootSignature (version %s) failed (0x%08X)", version11 ? "1.1" : "1.0", unsigned(hr));
        return nullptr;
    }
    signature->SetName(name);
    return signature;
}
} // namespace

// ============================================================================================
// Logging, hashing, formats
// ============================================================================================

void SetLogSink(LogSink sink)
{
    g_logSink.store(sink, std::memory_order_release);
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteLine(fmt, args);
    va_end(args);
}

void LogOnce(const void* key, const char* fmt, ...)
{
    bool first = true;
    try
    {
        std::lock_guard lock(g_onceLock);
        first = OnceKeys().insert(key).second;
    }
    catch (...)
    {
    }
    if (!first)
        return;
    va_list args;
    va_start(args, fmt);
    WriteLine(fmt, args);
    va_end(args);
}

// xxHash64 (Yann Collet's algorithm, little-endian reads).
namespace
{
constexpr u64 kPrime1 = 0x9E3779B185EBCA87ull;
constexpr u64 kPrime2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 kPrime3 = 0x165667B19E3779F9ull;
constexpr u64 kPrime4 = 0x85EBCA77C2B2AE63ull;
constexpr u64 kPrime5 = 0x27D4EB2F165667C5ull;

u64 Read64(const u8* p)
{
    u64 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

u32 Read32(const u8* p)
{
    u32 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

u64 Round(u64 acc, u64 input)
{
    acc += input * kPrime2;
    acc = std::rotl(acc, 31);
    return acc * kPrime1;
}

u64 MergeRound(u64 acc, u64 value)
{
    acc ^= Round(0, value);
    return acc * kPrime1 + kPrime4;
}
} // namespace

u64 Hash64(const void* data, size_t size, u64 seed)
{
    if (!data)
        size = 0;
    const u8* p = static_cast<const u8*>(data);
    const u8* const end = p + size;
    u64 h;
    if (size >= 32)
    {
        const u8* const limit = end - 32;
        u64 v1 = seed + kPrime1 + kPrime2;
        u64 v2 = seed + kPrime2;
        u64 v3 = seed;
        u64 v4 = seed - kPrime1;
        do
        {
            v1 = Round(v1, Read64(p));
            v2 = Round(v2, Read64(p + 8));
            v3 = Round(v3, Read64(p + 16));
            v4 = Round(v4, Read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = MergeRound(h, v1);
        h = MergeRound(h, v2);
        h = MergeRound(h, v3);
        h = MergeRound(h, v4);
    }
    else
        h = seed + kPrime5;
    h += u64(size);
    while (end - p >= 8)
    {
        h ^= Round(0, Read64(p));
        h = std::rotl(h, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (end - p >= 4)
    {
        h ^= u64(Read32(p)) * kPrime1;
        h = std::rotl(h, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end)
    {
        h ^= u64(*p) * kPrime5;
        h = std::rotl(h, 11) * kPrime1;
        ++p;
    }
    h ^= h >> 33;
    h *= kPrime2;
    h ^= h >> 29;
    h *= kPrime3;
    h ^= h >> 32;
    return h;
}

bool IsDepthFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return true;
    default:
        return false;
    }
}

u32 PlaneCount(ID3D12Device* device, DXGI_FORMAT format)
{
    // The answer depends on the format only; 0 = not asked yet.
    static std::atomic<u8> cache[256]{};
    const u32 index = u32(format);
    if (index < 256)
    {
        if (const u8 cached = cache[index].load(std::memory_order_relaxed))
            return cached;
    }
    if (!device || format == DXGI_FORMAT_UNKNOWN)
        return 1;
    D3D12_FEATURE_DATA_FORMAT_INFO info{};
    info.Format = format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) || !info.PlaneCount)
        return 1;
    if (index < 256)
        cache[index].store(info.PlaneCount, std::memory_order_relaxed);
    return info.PlaneCount;
}

DXGI_FORMAT TypelessOf(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return DXGI_FORMAT_R32G32B32_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
        return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
        return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return DXGI_FORMAT_BC1_TYPELESS;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return DXGI_FORMAT_BC2_TYPELESS;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return DXGI_FORMAT_BC3_TYPELESS;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return DXGI_FORMAT_BC4_TYPELESS;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        return DXGI_FORMAT_BC5_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
        return DXGI_FORMAT_BC6H_TYPELESS;
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return DXGI_FORMAT_BC7_TYPELESS;
    default:
        return format;
    }
}

bool IsBlockCompressed(DXGI_FORMAT format)
{
    return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) ||
        (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

D3D12Api g_d3d12;

bool D3D12Api::Load()
{
    static std::once_flag once;
    static bool loaded = false;
    try
    {
        std::call_once(once, [] {
            const HMODULE module = LoadLibraryW(L"d3d12.dll");
            if (!module)
            {
                Log("d3d12.dll is not available (error %lu)", GetLastError());
                return;
            }
            g_d3d12.CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice"));
            g_d3d12.GetDebugInterface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(module, "D3D12GetDebugInterface"));
            g_d3d12.SerializeVersionedRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(
                GetProcAddress(module, "D3D12SerializeVersionedRootSignature"));
            g_d3d12.SerializeRootSignature =
                reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(module, "D3D12SerializeRootSignature"));
            loaded = g_d3d12.CreateDevice && g_d3d12.SerializeRootSignature;
            if (!loaded)
                Log("d3d12.dll lacks D3D12CreateDevice or D3D12SerializeRootSignature");
        });
    }
    catch (...)
    {
        return false;
    }
    if (this != &g_d3d12)
        *this = g_d3d12;
    return loaded;
}

// ============================================================================================
// Synchronisation
// ============================================================================================

bool FenceTimeline::Init(ID3D12Device* device)
{
    if (!device)
        return false;
    const HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr))
    {
        m_fence = nullptr;
        Log("CreateFence failed (0x%08X)", unsigned(hr));
        return false;
    }
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_event)
    {
        Log("CreateEvent for the fence failed (error %lu)", GetLastError());
        m_fence->Release();
        m_fence = nullptr;
        return false;
    }
    m_next.store(1, std::memory_order_release);
    m_completed.store(0, std::memory_order_release);
    return true;
}

void FenceTimeline::Shutdown()
{
    if (m_event)
    {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    if (m_fence)
    {
        m_fence->Release();
        m_fence = nullptr;
    }
}

u64 FenceTimeline::Signal(ID3D12CommandQueue* queue)
{
    try
    {
        std::lock_guard lock(m_signalLock);
        const u64 value = m_next.load(std::memory_order_relaxed);
        const HRESULT hr = queue && m_fence ? queue->Signal(m_fence, value) : E_POINTER;
        if (FAILED(hr))
        {
            static constexpr char kFormat[] = "ID3D12CommandQueue::Signal failed (0x%08X)";
            LogOnce(kFormat, kFormat, unsigned(hr));
        }
        // The value is consumed even on failure: a removed device completes everything (UINT64_MAX).
        m_next.store(value + 1, std::memory_order_release);
        return value;
    }
    catch (...)
    {
        return m_next.load(std::memory_order_acquire) - 1;
    }
}

u64 FenceTimeline::Completed()
{
    u64 cached = m_completed.load(std::memory_order_acquire);
    if (!m_fence)
        return cached;
    const u64 value = m_fence->GetCompletedValue();
    if (value == UINT64_MAX && cached != UINT64_MAX)
    {
        static constexpr char kFormat[] = "fence reports UINT64_MAX: the device was removed";
        LogOnce(kFormat, kFormat);
    }
    while (value > cached && !m_completed.compare_exchange_weak(cached, value, std::memory_order_acq_rel))
    {
    }
    return value > cached ? value : cached;
}

void FenceTimeline::Wait(u64 value)
{
    if (!m_fence || value <= m_completed.load(std::memory_order_acquire) || value <= Completed())
        return;
    if (value >= m_next.load(std::memory_order_acquire))
    {
        static constexpr char kFormat[] = "wait for fence value %llu that was never signalled (next %llu), skipped";
        LogOnce(kFormat, kFormat, static_cast<unsigned long long>(value),
            static_cast<unsigned long long>(m_next.load(std::memory_order_acquire)));
        return;
    }
    try
    {
        std::lock_guard lock(m_waitLock);
        while (Completed() < value)
        {
            if (FAILED(m_fence->SetEventOnCompletion(value, m_event)))
                return;
            // The timeout re-reads the fence: a removed device may never set the event.
            WaitForSingleObject(m_event, 1000);
        }
    }
    catch (...)
    {
    }
}

void DeferredReleaser::Release(IUnknown* object)
{
    if (!object)
        return;
    if (!m_fence)
    {
        object->Release();
        return;
    }
    try
    {
        std::lock_guard lock(m_lock);
        m_entries.push_back({m_fence->NextValue(), object, {}});
    }
    catch (...)
    {
        // Releasing now could free memory the GPU still reads: leaking is the safe failure.
        static constexpr char kFormat[] = "deferred release queue out of memory, object leaked";
        LogOnce(kFormat, kFormat);
    }
}

void DeferredReleaser::Defer(std::function<void()>&& action)
{
    if (!action)
        return;
    try
    {
        if (!m_fence)
        {
            action();
            return;
        }
        std::lock_guard lock(m_lock);
        m_entries.push_back({m_fence->NextValue(), nullptr, std::move(action)});
    }
    catch (...)
    {
        static constexpr char kFormat[] = "deferred action failed or could not be queued";
        LogOnce(kFormat, kFormat);
    }
}

void DeferredReleaser::Collect(bool all)
{
    // Entries run outside the lock (they may queue more); a nested Collect on the same thread
    // would clobber the scratch list.
    thread_local std::vector<Entry> ready;
    thread_local bool collecting = false;
    if (collecting)
        return;
    collecting = true;
    ready.clear();
    try
    {
        const u64 done = all || !m_fence ? UINT64_MAX : m_fence->Completed();
        std::lock_guard lock(m_lock);
        ready.reserve(m_entries.size());
        size_t keep = 0;
        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            if (m_entries[i].fence <= done)
                ready.push_back(std::move(m_entries[i]));
            else
            {
                if (keep != i)
                    m_entries[keep] = std::move(m_entries[i]);
                ++keep;
            }
        }
        m_entries.erase(m_entries.begin() + ptrdiff_t(keep), m_entries.end());
    }
    catch (...)
    {
    }
    for (Entry& entry : ready)
    {
        if (entry.object)
            entry.object->Release();
        if (entry.action)
        {
            try
            {
                entry.action();
            }
            catch (...)
            {
            }
        }
    }
    ready.clear();
    collecting = false;
}

// ============================================================================================
// Descriptors
// ============================================================================================

bool CpuDescriptorHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 blockSize)
{
    if (!device)
        return false;
    m_device = device;
    m_type = type;
    m_blockSize = (std::max)(blockSize, 1u);
    m_increment = device->GetDescriptorHandleIncrementSize(type);
    std::lock_guard lock(m_lock);
    return GrowCpuHeap(m_device, m_type, m_blockSize, m_increment, m_blocks, m_free);
}

void CpuDescriptorHeap::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (ID3D12DescriptorHeap* heap : m_blocks)
        heap->Release();
    m_blocks.clear();
    m_free.clear();
    m_device = nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorHeap::Allocate()
{
    try
    {
        std::lock_guard lock(m_lock);
        if (m_free.empty() && (!m_device || !GrowCpuHeap(m_device, m_type, m_blockSize, m_increment, m_blocks, m_free)))
            return {};
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = m_free.back();
        m_free.pop_back();
        return handle;
    }
    catch (...)
    {
        return {};
    }
}

void CpuDescriptorHeap::Free(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (!handle.ptr)
        return;
    try
    {
        std::lock_guard lock(m_lock);
        m_free.push_back(handle);
    }
    catch (...)
    {
    }
}

bool GpuDescriptorRing::Init(ID3D12Device* device, FenceTimeline* fence, u32 capacity)
{
    if (!device || !fence || !capacity)
        return false;
    m_fence = fence;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = E_FAIL;
    // Drivers below the documented tier limits get a smaller ring rather than no renderer.
    for (u32 size = capacity; size >= 65536 || size == capacity; size /= 2)
    {
        desc.NumDescriptors = size;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
        if (SUCCEEDED(hr))
        {
            m_capacity = size;
            break;
        }
        m_heap = nullptr;
        if (size < 2)
            break;
    }
    if (!m_heap)
    {
        Log("shader-visible descriptor heap of %u descriptors failed (0x%08X)", capacity, unsigned(hr));
        return false;
    }
    if (m_capacity != capacity)
        Log("shader-visible descriptor ring: %u descriptors instead of %u", m_capacity, capacity);
    m_heap->SetName(L"xrD3D12 descriptor ring");
    m_cpuBase = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuBase = m_heap->GetGPUDescriptorHandleForHeapStart();
    m_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // The persistent bindless range goes first, so heap index = bindless index.
    m_base = (std::min)(kBindlessCapacity, m_capacity / 8);
    m_head = m_base;
    try
    {
        m_marks.reserve(256);
    }
    catch (...)
    {
    }
    return true;
}

void GpuDescriptorRing::Shutdown()
{
    std::lock_guard lock(m_lock);
    if (m_heap)
    {
        m_heap->Release();
        m_heap = nullptr;
    }
    m_marks.clear();
    m_head = 0;
    m_base = 0;
    m_capacity = 0;
}

u32 GpuDescriptorRing::AcquireChunk(u32 count, u64 frame)
{
    if (!m_heap || !count)
        return 0;
    if (count > m_capacity - m_base)
    {
        static constexpr char kFormat[] = "descriptor request of %u exceeds the ring (%u)";
        LogOnce(kFormat, kFormat, count, m_capacity - m_base);
        return m_base; // a ring slot, never the bindless range
    }
    try
    {
        std::unique_lock lock(m_lock);
        for (;;)
        {
            const u64 done = m_fence->Completed();
            size_t retired = 0;
            while (retired < m_marks.size() && m_marks[retired].fence && m_marks[retired].fence <= done)
                ++retired;
            if (retired)
                m_marks.erase(m_marks.begin(), m_marks.begin() + ptrdiff_t(retired));

            // Contiguous only: a tail shorter than the request is skipped.
            const u32 start = u64(m_head) + count > m_capacity ? m_base : m_head;
            const u32 end = start + count;
            const FrameMark* blocking = nullptr;
            for (const FrameMark& mark : m_marks)
            {
                if (mark.fence && mark.fence <= done)
                    continue;
                if (mark.begin < end && start < mark.end)
                {
                    blocking = &mark;
                    break;
                }
            }
            if (blocking && blocking->fence)
            {
                const u64 wait = blocking->fence;
                lock.unlock();
                m_fence->Wait(wait);
                lock.lock();
                continue;
            }
            if (blocking)
            {
                // The ring wrapped into frames that have not even ended: nothing to wait for.
                static constexpr char kFormat[] = "descriptor ring overrun inside unfinished frames (frame %llu, %u descriptors)";
                LogOnce(kFormat, kFormat, static_cast<unsigned long long>(frame), m_capacity);
            }

            if (!m_marks.empty() && m_marks.back().frame == frame && !m_marks.back().fence && m_marks.back().end == start)
                m_marks.back().end = end;
            else
                m_marks.push_back({frame, start, end, 0});
            m_head = end;
            return start;
        }
    }
    catch (...)
    {
        static constexpr char kFormat[] = "descriptor ring bookkeeping out of memory";
        LogOnce(kFormat, kFormat);
        return m_base;
    }
}

void GpuDescriptorRing::EndFrame(u64 frame, u64 fenceValue)
{
    std::lock_guard lock(m_lock);
    // Chunks taken for a frame after it ended belong to the next EndFrame (still conservative).
    for (FrameMark& mark : m_marks)
    {
        if (!mark.fence && mark.frame <= frame)
            mark.fence = fenceValue;
    }
}

u32 DescriptorAllocator::Allocate(u32 count, u64 frame)
{
    if (!m_ring)
        return 0;
    if (frame != m_frame)
    {
        m_frame = frame;
        m_next = 0;
        m_end = 0;
    }
    if (u64(m_next) + count > m_end)
    {
        const u32 size = (std::max)(count, kDescriptorChunk);
        m_next = m_ring->AcquireChunk(size, frame);
        m_end = m_next + size;
    }
    const u32 index = m_next;
    m_next += count;
    return index;
}

bool SamplerTableCache::Init(ID3D12Device* device, Device* owner)
{
    if (!device)
        return false;
    m_device = device;
    m_owner = owner;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    desc.NumDescriptors = kSamplerHeapCapacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr))
    {
        m_heap = nullptr;
        Log("shader-visible sampler heap failed (0x%08X)", unsigned(hr));
        return false;
    }
    m_heap->SetName(L"xrD3D12 sampler tables");
    m_cpuBase = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuBase = m_heap->GetGPUDescriptorHandleForHeapStart();
    m_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    m_used = 0;
    try
    {
        m_tables.reserve(512);
    }
    catch (...)
    {
    }
    return true;
}

void SamplerTableCache::Shutdown()
{
    std::unique_lock lock(m_lock);
    if (m_heap)
    {
        m_heap->Release();
        m_heap = nullptr;
    }
    for (const auto& [heap, frame] : m_retired)
        heap->Release();
    m_retired.clear();
    m_tables.clear();
    m_used = 0;
}

ID3D12DescriptorHeap* SamplerTableCache::Current(u32& generation)
{
    std::shared_lock lock(m_lock);
    generation = m_generation.load(std::memory_order_relaxed);
    return m_heap;
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerTableCache::Get(const u32* ids, const D3D12_CPU_DESCRIPTOR_HANDLE* cpu, u32 count)
{
    u32 generation = 0;
    return Get(ids, cpu, count, generation);
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerTableCache::Get(const u32* ids, const D3D12_CPU_DESCRIPTOR_HANDLE* cpu, u32 count,
    u32& generation)
{
    generation = m_generation.load(std::memory_order_acquire);
    if (!m_heap || !ids || !cpu || !count)
        return m_gpuBase;
    count = (std::min)(count, kMaxSamplers);
    const u64 key = Hash64(ids, count * sizeof(u32), count);
    try
    {
        {
            std::shared_lock lock(m_lock);
            generation = m_generation.load(std::memory_order_relaxed);
            if (const auto it = m_tables.find(key); it != m_tables.end())
                return it->second;
        }
        std::unique_lock lock(m_lock);
        generation = m_generation.load(std::memory_order_relaxed);
        if (const auto it = m_tables.find(key); it != m_tables.end())
            return it->second;
        // Every table keeps room for a full s0..s15 range inside the heap.
        if (m_used + kMaxSamplers > kSamplerHeapCapacity)
        {
            // Lists recorded so far keep the old heap; it goes once none of them can run any more.
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            desc.NumDescriptors = kSamplerHeapCapacity;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ID3D12DescriptorHeap* heap = nullptr;
            const u64 frame = m_owner ? m_owner->frame.load(std::memory_order_acquire) : 0;
            if (SUCCEEDED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
            {
                try
                {
                    m_retired.emplace_back(m_heap, frame);
                }
                catch (...)
                {
                    heap->Release();
                    return m_gpuBase;
                }
                heap->SetName(L"xrD3D12 sampler tables");
                m_heap = heap;
                m_cpuBase = m_heap->GetCPUDescriptorHandleForHeapStart();
                m_gpuBase = m_heap->GetGPUDescriptorHandleForHeapStart();
                generation = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                static constexpr char kFormat[] = "sampler heap full (%u tables): continuing in a new heap";
                LogOnce(kFormat, kFormat, u32(m_tables.size()));
            }
            else
            {
                // No memory for another heap. Overwriting this one is not safe: lists recorded on
                // other threads or waiting for their submission still point into it. New sampler
                // sets fall back to the first table until memory frees up.
                static constexpr char kFormat[] = "sampler heap full (%u tables) and no memory for a new one: new sampler sets use the first table";
                LogOnce(kFormat, kFormat, u32(m_tables.size()));
                return m_gpuBase;
            }
            m_tables.clear();
            m_used = 0;
        }
        const u32 first = m_used;
        for (u32 i = 0; i < count; ++i)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE dst{m_cpuBase.ptr + SIZE_T(first + i) * m_increment};
            m_device->CopyDescriptorsSimple(1, dst, cpu[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        m_used += count;
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu{m_gpuBase.ptr + u64(first) * m_increment};
        try
        {
            m_tables.emplace(key, gpu);
        }
        catch (...)
        {
        }
        return gpu;
    }
    catch (...)
    {
        return m_gpuBase;
    }
}

void SamplerTableCache::EndFrame(u64 frame)
{
    std::unique_lock lock(m_lock);
    for (size_t i = 0; i < m_retired.size();)
    {
        const auto [heap, retiredIn] = m_retired[i];
        if (retiredIn + kFramesInFlight + 1 > frame)
        {
            ++i;
            continue;
        }
        if (m_owner)
            m_owner->releaser.Release(heap);
        else
            heap->Release();
        m_retired.erase(m_retired.begin() + ptrdiff_t(i));
    }
}

bool NullDescriptors::Init(Device* device)
{
    if (!device || !device->d12)
        return false;
    ID3D12Device* d12 = device->d12;

    BufferPageAllocator::Range zero;
    if (!device->dynamicFirstVersions.Allocate(kZeroCbSize, zero) || !zero.cpu)
    {
        Log("null descriptors: no upload memory for the zero constant buffer");
        return false;
    }
    std::memset(zero.cpu, 0, size_t(kZeroCbSize));
    zeroCb = zero.gpu;

    for (u32 dim = D3D12_SRV_DIMENSION_BUFFER; dim <= D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; ++dim)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION(dim);
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (desc.ViewDimension)
        {
        case D3D12_SRV_DIMENSION_BUFFER:
            desc.Buffer.NumElements = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1D:
            desc.Texture1D.MipLevels = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            desc.Texture1DArray.MipLevels = 1;
            desc.Texture1DArray.ArraySize = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            desc.Texture2D.MipLevels = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            desc.Texture2DArray.MipLevels = 1;
            desc.Texture2DArray.ArraySize = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            desc.Texture2DMSArray.ArraySize = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            desc.Texture3D.MipLevels = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            desc.TextureCube.MipLevels = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            desc.TextureCubeArray.MipLevels = 1;
            desc.TextureCubeArray.NumCubes = 1;
            break;
        default:
            break;
        }
        srv[dim] = device->cpuSrv.Allocate();
        if (!srv[dim].ptr)
            return false;
        d12->CreateShaderResourceView(nullptr, &desc, srv[dim]);
    }
    srv[D3D12_SRV_DIMENSION_UNKNOWN] = srv[D3D12_SRV_DIMENSION_TEXTURE2D];
    // An unbound acceleration structure slot is an empty one (location 0): ray queries miss.
    constexpr u32 kAs = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv[kAs] = srv[D3D12_SRV_DIMENSION_TEXTURE2D];
    if (device->caps.raytracingTier)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = device->cpuSrv.Allocate();
        if (!handle.ptr)
            return false;
        d12->CreateShaderResourceView(nullptr, &desc, handle);
        srv[kAs] = handle;
    }

    // UAV dimensions without a dedicated null view (UNKNOWN, the multisampled ones of newer SDKs)
    // share the Texture2D one.
    const D3D12_UAV_DIMENSION uavDims[] = {D3D12_UAV_DIMENSION_BUFFER, D3D12_UAV_DIMENSION_TEXTURE1D,
        D3D12_UAV_DIMENSION_TEXTURE1DARRAY, D3D12_UAV_DIMENSION_TEXTURE2D, D3D12_UAV_DIMENSION_TEXTURE2DARRAY,
        D3D12_UAV_DIMENSION_TEXTURE3D};
    for (const D3D12_UAV_DIMENSION dim : uavDims)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.ViewDimension = dim;
        switch (dim)
        {
        case D3D12_UAV_DIMENSION_BUFFER:
            desc.Buffer.NumElements = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            desc.Texture1DArray.ArraySize = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            desc.Texture2DArray.ArraySize = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            desc.Texture3D.WSize = 1;
            break;
        default:
            break;
        }
        uav[dim] = device->cpuSrv.Allocate();
        if (!uav[dim].ptr)
            return false;
        d12->CreateUnorderedAccessView(nullptr, nullptr, &desc, uav[dim]);
    }
    for (D3D12_CPU_DESCRIPTOR_HANDLE& handle : uav)
    {
        if (!handle.ptr)
            handle = uav[D3D12_UAV_DIMENSION_TEXTURE2D];
    }

    // A CBV cannot be null: it views the zero buffer instead.
    cbv = device->cpuSrv.Allocate();
    if (!cbv.ptr)
        return false;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbDesc{};
    cbDesc.BufferLocation = zeroCb;
    cbDesc.SizeInBytes = UINT(kZeroCbSize);
    d12->CreateConstantBufferView(&cbDesc, cbv);

    // D3D11's default sampler state.
    sampler = device->cpuSampler.Allocate();
    if (!sampler.ptr)
        return false;
    D3D12_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.BorderColor[0] = samplerDesc.BorderColor[1] = samplerDesc.BorderColor[2] = samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;
    d12->CreateSampler(&samplerDesc, sampler);

    rtv = device->cpuRtv.Allocate();
    if (!rtv.ptr)
        return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    d12->CreateRenderTargetView(nullptr, &rtvDesc, rtv);
    return true;
}

// ============================================================================================
// Memory
// ============================================================================================

bool UploadPagePool::Init(ID3D12Device* device, FenceTimeline* fence)
{
    if (!device || !fence)
        return false;
    m_device = device;
    m_fence = fence;
    try
    {
        m_used.reserve(256);
        m_free.reserve(kMaxFreeUploadPages + kMaxFreeDedicatedPages);
    }
    catch (...)
    {
    }
    return true;
}

void UploadPagePool::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (UploadPage* page : m_free)
        DestroyUploadPage(page);
    for (UploadPage* page : m_used)
        DestroyUploadPage(page);
    m_free.clear();
    m_used.clear();
    m_frameFences.clear();
    m_inFlight.store(0);
}

UploadPage* UploadPagePool::Acquire(u64 minSize, u64 frame)
{
    if (!m_device)
        return nullptr;
    const bool dedicated = minSize > kUploadPageSize;
    const u64 wanted = dedicated ? AlignUp(minSize, kPageAlignment) : kUploadPageSize;
    UploadPage* page = nullptr;
    try
    {
        {
            // Oversized pages are reused when they fit without wasting more than half.
            std::lock_guard lock(m_lock);
            for (size_t i = m_free.size(); i-- > 0;)
            {
                UploadPage* candidate = m_free[i];
                const bool fits = dedicated ? candidate->dedicated && candidate->size >= wanted && candidate->size <= 2 * wanted
                                            : !candidate->dedicated;
                if (!fits)
                    continue;
                page = candidate;
                m_free[i] = m_free.back();
                m_free.pop_back();
                break;
            }
        }
        if (!page)
        {
            HRESULT hr = S_OK;
            page = CreateUploadPage(m_device, wanted, &hr);
            if (!page)
            {
                static constexpr char kFormat[] = "upload page of %llu bytes failed (0x%08X)";
                LogOnce(kFormat, kFormat, static_cast<unsigned long long>(minSize), unsigned(hr));
                return nullptr;
            }
            page->dedicated = dedicated;
        }
        page->frame = frame;
        page->fence = 0;
        {
            std::lock_guard lock(m_lock);
            m_used.push_back(page);
        }
    }
    catch (...)
    {
        DestroyUploadPage(page);
        return nullptr;
    }
    const u64 inFlight = m_inFlight.fetch_add(page->size) + page->size;
    if (inFlight > kUploadBudgetBytes)
    {
        static constexpr char kFormat[] = "dynamic upload memory in flight exceeds %llu MB (%llu MB): frames are not ending";
        LogOnce(kFormat, kFormat, static_cast<unsigned long long>(kUploadBudgetBytes / kMB),
            static_cast<unsigned long long>(inFlight / kMB));
    }
    return page;
}

void UploadPagePool::EndFrame(u64 frame, u64 fenceValue)
{
    {
        std::lock_guard lock(m_lock);
        // Pages taken for a frame after it ended get the next frame's fence (still conservative).
        for (UploadPage* page : m_used)
        {
            if (!page->fence && page->frame <= frame)
                page->fence = fenceValue;
        }
    }
    Recycle();
}

void UploadPagePool::Recycle()
{
    if (!m_fence)
        return;
    const u64 done = m_fence->Completed();
    try
    {
        std::lock_guard lock(m_lock);
        m_free.reserve(kMaxFreeUploadPages + kMaxFreeDedicatedPages);
        size_t freeDedicated = 0;
        for (const UploadPage* page : m_free)
            freeDedicated += page->dedicated ? 1 : 0;
        size_t freeStandard = m_free.size() - freeDedicated;
        size_t keep = 0;
        for (size_t i = 0; i < m_used.size(); ++i)
        {
            UploadPage* page = m_used[i];
            if (page->fence && page->fence <= done)
            {
                m_inFlight.fetch_sub(page->size);
                // The reserved capacity bounds both kinds, so the push never allocates.
                const bool pooled = page->dedicated
                    ? freeDedicated < kMaxFreeDedicatedPages && page->size <= kMaxPooledDedicatedBytes
                    : freeStandard < kMaxFreeUploadPages;
                if (pooled && m_free.size() < m_free.capacity())
                {
                    m_free.push_back(page);
                    ++(page->dedicated ? freeDedicated : freeStandard);
                }
                else
                    DestroyUploadPage(page);
            }
            else
                m_used[keep++] = page;
        }
        m_used.resize(keep);
    }
    catch (...)
    {
    }
}

UploadAlloc UploadAllocator::Allocate(u64 size, u64 alignment, u64 frame)
{
    if (!m_pool)
        return {};
    if (frame != m_frame)
    {
        m_frame = frame;
        m_page = nullptr;
    }
    if (size > kUploadPageSize)
    {
        UploadPage* page = m_pool->Acquire(size, frame);
        if (!page)
            return {};
        return {page->resource, 0, page->gpu, page->cpu, size};
    }
    u64 offset = m_page ? AlignUp(m_offset, alignment) : 0;
    if (!m_page || offset + size > m_page->size)
    {
        m_page = m_pool->Acquire(size, frame);
        if (!m_page)
            return {};
        offset = 0;
    }
    m_offset = offset + size;
    return {m_page->resource, offset, m_page->gpu + offset, m_page->cpu + offset, size};
}

namespace
{
bool TakeRange(std::vector<std::pair<u64, u64>>& free, u64 size, u64& offset)
{
    for (auto it = free.begin(); it != free.end(); ++it)
    {
        if (it->second < size)
            continue;
        offset = it->first;
        it->first += size;
        it->second -= size;
        if (!it->second)
            free.erase(it);
        return true;
    }
    return false;
}
} // namespace

bool BufferPageAllocator::Init(ID3D12Device* device, D3D12_HEAP_TYPE heapType)
{
    if (!device)
        return false;
    m_device = device;
    m_heapType = heapType;
    return true;
}

void BufferPageAllocator::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (Page& page : m_pages)
    {
        if (page.resource)
            page.resource->Release();
    }
    m_pages.clear();
}

bool BufferPageAllocator::Allocate(u64 size, Range& out)
{
    out = Range{};
    if (!m_device)
        return false;
    const u64 aligned = AlignUp((std::max)(size, u64(1)), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const bool mapped = m_heapType == D3D12_HEAP_TYPE_UPLOAD || m_heapType == D3D12_HEAP_TYPE_GPU_UPLOAD;
    if (aligned > kBufferSuballocLimit)
    {
        ID3D12Resource* resource = nullptr;
        u8* cpu = nullptr;
        const HRESULT hr = CreateCommittedBuffer(m_device, m_heapType, aligned, &resource, mapped ? &cpu : nullptr);
        if (FAILED(hr))
        {
            Log("dedicated buffer of %llu bytes failed (0x%08X)", static_cast<unsigned long long>(aligned), unsigned(hr));
            return false;
        }
        out.resource = resource;
        out.offset = 0;
        out.size = aligned;
        out.gpu = resource->GetGPUVirtualAddress();
        out.cpu = cpu;
        out.page = ~0u;
        return true;
    }
    try
    {
        std::lock_guard lock(m_lock);
        u64 offset = 0;
        for (u32 index = 0; index < u32(m_pages.size()); ++index)
        {
            Page& page = m_pages[index];
            if (!TakeRange(page.free, aligned, offset))
                continue;
            out.resource = page.resource;
            out.offset = offset;
            out.size = aligned;
            out.gpu = page.gpu + offset;
            out.cpu = page.cpu ? page.cpu + offset : nullptr;
            out.page = index;
            return true;
        }

        Page page;
        const HRESULT hr = CreateCommittedBuffer(m_device, m_heapType, kBufferPageSize, &page.resource, mapped ? &page.cpu : nullptr);
        if (FAILED(hr))
        {
            Log("buffer page of %llu MB failed (0x%08X)", static_cast<unsigned long long>(kBufferPageSize / kMB), unsigned(hr));
            return false;
        }
        page.gpu = page.resource->GetGPUVirtualAddress();
        try
        {
            page.free.push_back({aligned, kBufferPageSize - aligned});
            m_pages.push_back(std::move(page));
        }
        catch (...)
        {
            page.resource->Release();
            throw;
        }
        const Page& added = m_pages.back();
        out.resource = added.resource;
        out.offset = 0;
        out.size = aligned;
        out.gpu = added.gpu;
        out.cpu = added.cpu;
        out.page = u32(m_pages.size() - 1);
        return true;
    }
    catch (...)
    {
        out = Range{};
        return false;
    }
}

void BufferPageAllocator::Free(const Range& range)
{
    if (!range.resource)
        return;
    if (range.page == ~0u)
    {
        range.resource->Release();
        return;
    }
    try
    {
        std::lock_guard lock(m_lock);
        if (range.page >= m_pages.size())
            return;
        auto& free = m_pages[range.page].free;
        const u64 offset = range.offset;
        const u64 size = range.size;
        const auto next = std::lower_bound(free.begin(), free.end(), offset,
            [](const std::pair<u64, u64>& entry, u64 value) { return entry.first < value; });
        const bool mergePrev = next != free.begin() && (next - 1)->first + (next - 1)->second == offset;
        const bool mergeNext = next != free.end() && offset + size == next->first;
        if (mergePrev && mergeNext)
        {
            (next - 1)->second += size + next->second;
            free.erase(next);
        }
        else if (mergePrev)
            (next - 1)->second += size;
        else if (mergeNext)
        {
            next->first = offset;
            next->second += size;
        }
        else
            free.insert(next, {offset, size});
    }
    catch (...)
    {
        static constexpr char kFormat[] = "buffer page free list out of memory, range leaked";
        LogOnce(kFormat, kFormat);
    }
}

namespace
{
// Returns completed staging pages to the pool and hands out one for the open list.
UploadPage* TakeStagingPage(Device* device, FenceTimeline& fence, std::vector<UploadPage*>& inFlight,
    std::vector<UploadPage*>& freePages)
{
    const u64 done = fence.Completed();
    size_t keep = 0;
    for (size_t i = 0; i < inFlight.size(); ++i)
    {
        UploadPage* page = inFlight[i];
        if (page->fence <= done)
        {
            // Capacity is reserved up to the limit, so this push never allocates.
            if (page->dedicated || freePages.size() >= kMaxFreeInitPages || freePages.size() >= freePages.capacity())
                DestroyUploadPage(page);
            else
                freePages.push_back(page);
        }
        else
            inFlight[keep++] = page;
    }
    inFlight.resize(keep);
    if (!freePages.empty())
    {
        UploadPage* page = freePages.back();
        freePages.pop_back();
        return page;
    }
    HRESULT hr = S_OK;
    UploadPage* page = CreateUploadPage(device->d12, kInitUploadPageSize, &hr);
    if (!page)
        Log("creation-time upload page failed (0x%08X)", unsigned(hr));
    return page;
}

void CopyRows(u8* dst, u64 dstRowPitch, u64 dstSlicePitch, const u8* src, u64 srcRowPitch, u64 srcSlicePitch, u64 rowSize,
    u32 rowCount, u32 depth)
{
    for (u32 z = 0; z < depth; ++z)
    {
        u8* dstSlice = dst + z * dstSlicePitch;
        const u8* srcSlice = src + z * srcSlicePitch;
        if (srcRowPitch == dstRowPitch)
        {
            // One copy per slice; the last row is only rowSize long in the caller's memory.
            std::memcpy(dstSlice, srcSlice, size_t(u64(rowCount - 1) * dstRowPitch + rowSize));
            continue;
        }
        for (u32 y = 0; y < rowCount; ++y)
            std::memcpy(dstSlice + y * dstRowPitch, srcSlice + y * srcRowPitch, size_t(rowSize));
    }
}
} // namespace

bool InitUploader::Init(Device* device, ID3D12CommandQueue* queue, FenceTimeline* fence, D3D12_COMMAND_LIST_TYPE type)
{
    if (!device || !device->d12 || !queue || !fence)
        return false;
    m_device = device;
    m_queue = queue;
    m_fence = fence;
    m_type = type;
    for (ID3D12CommandAllocator*& allocator : m_allocators)
    {
        const HRESULT hr = device->d12->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
        if (FAILED(hr))
        {
            allocator = nullptr;
            Log("init uploader: CreateCommandAllocator failed (0x%08X)", unsigned(hr));
            return false;
        }
    }
    const HRESULT hr = device->d12->CreateCommandList(0, type, m_allocators[0], nullptr, IID_PPV_ARGS(&m_list));
    if (FAILED(hr))
    {
        m_list = nullptr;
        Log("init uploader: CreateCommandList failed (0x%08X)", unsigned(hr));
        return false;
    }
    m_list->SetName(CopyQueue() ? L"xrD3D12 creation-time uploads (copy queue)" : L"xrD3D12 creation-time uploads");
    m_list->Close();
    m_open = false;
    m_allocatorIndex = 0;
    for (u64& fence : m_allocatorFence)
        fence = 0;
    try
    {
        m_freePages.reserve(kMaxFreeInitPages);
        m_pagesInList.reserve(16);
        m_pagesInFlight.reserve(64);
        m_finalBarriers.reserve(256);
    }
    catch (...)
    {
    }
    return true;
}

void InitUploader::Shutdown()
{
    std::lock_guard lock(m_lock);
    if (m_list)
    {
        if (m_open)
            m_list->Close();
        m_list->Release();
        m_list = nullptr;
    }
    m_open = false;
    for (ID3D12CommandAllocator*& allocator : m_allocators)
    {
        if (allocator)
            allocator->Release();
        allocator = nullptr;
    }
    for (UploadPage* page : m_pagesInList)
        DestroyUploadPage(page);
    for (UploadPage* page : m_pagesInFlight)
        DestroyUploadPage(page);
    for (UploadPage* page : m_freePages)
        DestroyUploadPage(page);
    m_pagesInList.clear();
    m_pagesInFlight.clear();
    m_freePages.clear();
    m_finalBarriers.clear();
    for (ID3D12Resource* target : m_targetsInList)
        target->Release();
    m_targetsInList.clear();
    ReleaseCompletedTargets(true);
    m_page = nullptr;
    m_pageOffset = 0;
    m_pendingBytes = 0;
}

void InitUploader::KeepTarget(ID3D12Resource* resource)
{
    // Consecutive copies into one buffer page need one reference.
    if (!m_targetsInList.empty() && m_targetsInList.back() == resource)
        return;
    try
    {
        m_targetsInList.push_back(resource);
        resource->AddRef();
    }
    catch (...)
    {
        static constexpr char kFormat[] = "init uploader: a copy target is not referenced (out of memory)";
        LogOnce(kFormat, kFormat);
    }
}

void InitUploader::ReleaseCompletedTargets(bool all)
{
    const u64 done = all ? UINT64_MAX : m_fence->Completed();
    size_t keep = 0;
    for (size_t i = 0; i < m_targetsInFlight.size(); ++i)
    {
        if (m_targetsInFlight[i].first <= done)
            m_targetsInFlight[i].second->Release();
        else
            m_targetsInFlight[keep++] = m_targetsInFlight[i];
    }
    m_targetsInFlight.resize(keep);
}

// Caller holds m_lock. Leaves m_open false when the list cannot be opened.
void InitUploader::EnsureList()
{
    if (m_open || !m_list)
        return;
    ReleaseCompletedTargets(false);
    const u32 index = m_allocatorIndex;
    m_fence->Wait(m_allocatorFence[index]);
    ID3D12CommandAllocator* allocator = m_allocators[index];
    HRESULT hr = allocator->Reset();
    if (SUCCEEDED(hr))
        hr = m_list->Reset(allocator, nullptr);
    if (FAILED(hr))
    {
        static constexpr char kFormat[] = "init uploader: command list reset failed (0x%08X)";
        LogOnce(kFormat, kFormat, unsigned(hr));
        return;
    }
    m_open = true;
}

// Caller holds m_lock.
UploadAlloc InitUploader::AllocateStaging(u64 size, u64 alignment)
{
    if (size > kInitUploadPageSize)
    {
        HRESULT hr = S_OK;
        UploadPage* page = CreateUploadPage(m_device->d12, AlignUp(size, kPageAlignment), &hr);
        if (!page)
        {
            Log("creation-time upload of %llu bytes: staging buffer failed (0x%08X)", static_cast<unsigned long long>(size), unsigned(hr));
            return {};
        }
        page->dedicated = true;
        try
        {
            m_pagesInList.push_back(page);
        }
        catch (...)
        {
            DestroyUploadPage(page);
            return {};
        }
        return {page->resource, 0, page->gpu, page->cpu, size};
    }
    u64 offset = m_page ? AlignUp(m_pageOffset, alignment) : 0;
    if (!m_page || offset + size > m_page->size)
    {
        UploadPage* page = TakeStagingPage(m_device, *m_fence, m_pagesInFlight, m_freePages);
        if (!page)
            return {};
        try
        {
            m_pagesInList.push_back(page);
        }
        catch (...)
        {
            DestroyUploadPage(page);
            return {};
        }
        m_page = page;
        offset = 0;
    }
    m_pageOffset = offset + size;
    return {m_page->resource, offset, m_page->gpu + offset, m_page->cpu + offset, size};
}

void InitUploader::UploadBuffer(ID3D12Resource* dst, u64 dstOffset, const void* data, u64 size)
{
    if (!dst || !data || !size)
        return;
    bool flush = false;
    try
    {
        std::lock_guard lock(m_lock);
        EnsureList();
        if (!m_open)
            return;
        const UploadAlloc staging = AllocateStaging(size, 16);
        if (!staging.cpu)
            return;
        std::memcpy(staging.cpu, data, size_t(size));
        // The destination page is in COMMON and promotes to COPY_DEST implicitly.
        m_list->CopyBufferRegion(dst, dstOffset, staging.resource, staging.offset, size);
        KeepTarget(dst);
        m_pendingBytes += size;
        flush = m_pendingBytes > kInitAutoFlushBytes;
    }
    catch (...)
    {
        static constexpr char kFormat[] = "init uploader: buffer upload failed (out of memory)";
        LogOnce(kFormat, kFormat);
    }
    if (flush)
        Flush();
}

void InitUploader::UploadTexture(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, u32 firstSubresource,
    u32 subresourceCount, const D3D11_SUBRESOURCE_DATA* data, D3D12_RESOURCE_STATES finalState)
{
    if (!resource || !data || !subresourceCount)
        return;
    bool flush = false;
    try
    {
        // A description with MipLevels 0 means "full chain": the resource knows the real count.
        const D3D12_RESOURCE_DESC actual = desc.MipLevels ? desc : resource->GetDesc();
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
        std::vector<UINT> rowCounts(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        UINT64 total = 0;
        m_device->d12->GetCopyableFootprints(&actual, firstSubresource, subresourceCount, 0, layouts.data(), rowCounts.data(),
            rowSizes.data(), &total);
        if (!total || total == UINT64_MAX)
        {
            static constexpr char kFormat[] = "init uploader: no copyable footprints for a %ux%u texture (format %d)";
            LogOnce(kFormat, kFormat, unsigned(actual.Width), actual.Height, int(actual.Format));
            return;
        }

        std::lock_guard lock(m_lock);
        EnsureList();
        if (!m_open)
            return;
        const UploadAlloc staging = AllocateStaging(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!staging.cpu)
            return;
        for (u32 i = 0; i < subresourceCount; ++i)
        {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[i];
            const u32 rows = rowCounts[i];
            const u32 depth = layout.Footprint.Depth;
            const u64 rowSize = rowSizes[i];
            const u64 dstRowPitch = layout.Footprint.RowPitch;
            u8* dst = staging.cpu + layout.Offset;
            const D3D11_SUBRESOURCE_DATA& src = data[i];
            if (src.pSysMem && rows)
            {
                // D3D11 ignores the pitches where they carry no information (1D rows, 2D slices).
                const u64 srcRowPitch = src.SysMemPitch ? src.SysMemPitch : rowSize;
                const u64 srcSlicePitch = depth > 1 && src.SysMemSlicePitch ? src.SysMemSlicePitch : srcRowPitch * rows;
                CopyRows(dst, dstRowPitch, dstRowPitch * rows, static_cast<const u8*>(src.pSysMem), srcRowPitch, srcSlicePitch,
                    rowSize, rows, depth);
            }
            else
                std::memset(dst, 0, size_t(dstRowPitch * rows * depth));

            D3D12_TEXTURE_COPY_LOCATION dstLocation{};
            dstLocation.pResource = resource;
            dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLocation.SubresourceIndex = firstSubresource + i;
            D3D12_TEXTURE_COPY_LOCATION srcLocation{};
            srcLocation.pResource = staging.resource;
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLocation.PlacedFootprint = layout;
            srcLocation.PlacedFootprint.Offset += staging.offset;
            m_list->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
        }
        KeepTarget(resource);

        // Transitions are batched at Flush, after every copy of the list. A copy list has none: the
        // texture decays to COMMON at the end of the copy queue's submission.
        if (!CopyQueue() && finalState != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            const u32 arraySize = actual.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : actual.DepthOrArraySize;
            const u32 all = actual.MipLevels * arraySize * PlaneCount(m_device->d12, actual.Format);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = finalState;
            if (!firstSubresource && subresourceCount >= all)
            {
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                m_finalBarriers.push_back(barrier);
            }
            else
            {
                for (u32 i = 0; i < subresourceCount; ++i)
                {
                    barrier.Transition.Subresource = firstSubresource + i;
                    m_finalBarriers.push_back(barrier);
                }
            }
        }
        m_pendingBytes += total;
        flush = m_pendingBytes > kInitAutoFlushBytes;
    }
    catch (...)
    {
        static constexpr char kFormat[] = "init uploader: texture upload failed (out of memory)";
        LogOnce(kFormat, kFormat);
    }
    if (flush)
        Flush();
}

void InitUploader::Flush()
{
    try
    {
        std::lock_guard lock(m_lock);
        if (!m_open)
            return;
        if (!m_finalBarriers.empty())
        {
            m_list->ResourceBarrier(UINT(m_finalBarriers.size()), m_finalBarriers.data());
            m_finalBarriers.clear();
        }
        const HRESULT hr = m_list->Close();
        m_open = false;
        if (SUCCEEDED(hr))
        {
            ID3D12CommandList* lists[] = {m_list};
            m_queue->ExecuteCommandLists(1, lists);
        }
        else
        {
            static constexpr char kFormat[] = "init uploader: closing the list failed (0x%08X), uploads lost";
            LogOnce(kFormat, kFormat, unsigned(hr));
        }
        const u64 fence = m_fence->Signal(m_queue);
        // Queued inside m_lock: a Submit that may read these resources flushes first and blocks here
        // until the wait is in the direct queue. The frame fence then covers the copies, so releases
        // tagged with it stay safe.
        if (m_queue != m_device->queue)
        {
            const HRESULT waitHr = m_device->queue->Wait(m_fence->Native(), fence);
            if (FAILED(waitHr))
            {
                static constexpr char kFormat[] = "init uploader: the direct queue cannot wait for the copy queue (0x%08X)";
                LogOnce(kFormat, kFormat, unsigned(waitHr));
                m_fence->Wait(fence);
            }
        }
        m_statBytes.fetch_add(m_pendingBytes, std::memory_order_relaxed);
        m_statSubmissions.fetch_add(1, std::memory_order_relaxed);
        m_allocatorFence[m_allocatorIndex] = fence;
        m_allocatorIndex = (m_allocatorIndex + 1) % (kFramesInFlight + 1);
        m_page = nullptr;
        m_pageOffset = 0;
        m_pendingBytes = 0;
        for (UploadPage* page : m_pagesInList)
            page->fence = fence;
        try
        {
            m_targetsInFlight.reserve(m_targetsInFlight.size() + m_targetsInList.size());
            for (ID3D12Resource* target : m_targetsInList)
                m_targetsInFlight.emplace_back(fence, target);
        }
        catch (...)
        {
            // No room to track them: wait until the copies are done and let go now.
            m_fence->Wait(fence);
            for (ID3D12Resource* target : m_targetsInList)
                target->Release();
        }
        m_targetsInList.clear();
        try
        {
            m_pagesInFlight.insert(m_pagesInFlight.end(), m_pagesInList.begin(), m_pagesInList.end());
        }
        catch (...)
        {
            // No room to track them: wait until the GPU is done and drop them now.
            m_fence->Wait(fence);
            for (UploadPage* page : m_pagesInList)
                DestroyUploadPage(page);
        }
        m_pagesInList.clear();
    }
    catch (...)
    {
    }
}

bool InitUploader::Pending()
{
    std::lock_guard lock(m_lock);
    return m_open;
}

// ============================================================================================
// Pipelines
// ============================================================================================

bool RootSignatures::Init(ID3D12Device* device)
{
    if (!device)
        return false;
    D3D12_FEATURE_DATA_ROOT_SIGNATURE feature{};
    feature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    const bool version11 = g_d3d12.SerializeVersionedRootSignature &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &feature, sizeof(feature))) &&
        feature.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1;

    RootLayout gfx;
    gfx.Stage(GfxRoot::VS_CBV0, kRootCBVsVS, GfxRoot::VS_CBV_TABLE, GfxRoot::VS_SRV, GfxRoot::VS_SMP, D3D12_SHADER_VISIBILITY_VERTEX);
    gfx.Stage(GfxRoot::PS_CBV0, kRootCBVsPS, GfxRoot::PS_CBV_TABLE, GfxRoot::PS_SRV, GfxRoot::PS_SMP, D3D12_SHADER_VISIBILITY_PIXEL);
    gfx.Table(GfxRoot::PS_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, kMaxUAVs, D3D12_SHADER_VISIBILITY_PIXEL);
    gfx.Stage(GfxRoot::GS_CBV0, kRootCBVsOther, GfxRoot::GS_CBV_TABLE, GfxRoot::GS_SRV, GfxRoot::GS_SMP, D3D12_SHADER_VISIBILITY_GEOMETRY);
    gfx.Stage(GfxRoot::HS_CBV0, kRootCBVsOther, GfxRoot::HS_CBV_TABLE, GfxRoot::HS_SRV, GfxRoot::HS_SMP, D3D12_SHADER_VISIBILITY_HULL);
    gfx.Stage(GfxRoot::DS_CBV0, kRootCBVsOther, GfxRoot::DS_CBV_TABLE, GfxRoot::DS_SRV, GfxRoot::DS_SMP, D3D12_SHADER_VISIBILITY_DOMAIN);

    RootLayout cs;
    for (u32 i = 0; i < kRootCBVsCS; ++i)
        cs.RootCbv(CsRoot::CBV0 + i, i, D3D12_SHADER_VISIBILITY_ALL);
    cs.Table(CsRoot::CBV_TABLE, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, kRootCBVsCS, kMaxCBs - kRootCBVsCS, D3D12_SHADER_VISIBILITY_ALL);
    cs.Table(CsRoot::SRV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, kMaxSRVs, D3D12_SHADER_VISIBILITY_ALL);
    cs.Table(CsRoot::UAV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, kMaxUAVs, D3D12_SHADER_VISIBILITY_ALL);
    cs.Table(CsRoot::SMP, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, kMaxSamplers, D3D12_SHADER_VISIBILITY_ALL);

    const D3D12_ROOT_SIGNATURE_FLAGS gfxFlags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    bool used11 = version11;
    graphics = BuildRootSignature(device, gfx, gfxFlags, version11, L"xrD3D12 graphics root signature");
    compute = BuildRootSignature(device, cs, D3D12_ROOT_SIGNATURE_FLAG_NONE, version11, L"xrD3D12 compute root signature");
    if (version11 && (!graphics || !compute))
    {
        Shutdown();
        used11 = false;
        graphics = BuildRootSignature(device, gfx, gfxFlags, false, L"xrD3D12 graphics root signature");
        compute = BuildRootSignature(device, cs, D3D12_ROOT_SIGNATURE_FLAG_NONE, false, L"xrD3D12 compute root signature");
    }
    if (!graphics || !compute)
    {
        Shutdown();
        return false;
    }
    Log("root signatures: version %s", used11 ? "1.1" : "1.0");
    InitMesh(device, used11); // [dx12:mesh] hook
    return true;
}

void RootSignatures::Shutdown()
{
    ShutdownMesh(); // [dx12:mesh] hook
    if (graphics)
        graphics->Release();
    if (compute)
        compute->Release();
    graphics = nullptr;
    compute = nullptr;
}

u64 GraphicsPsoDesc::Hash() const
{
    u64 values[25];
    u32 n = 0;
    values[n++] = vs ? vs->hash : 0;
    values[n++] = hs ? hs->hash : 0;
    values[n++] = ds ? ds->hash : 0;
    values[n++] = gs ? gs->hash : 0;
    values[n++] = ps ? ps->hash : 0;
    values[n++] = inputLayout ? inputLayout->hash : 0;
    values[n++] = blend ? blend->hash : 0;
    values[n++] = depth ? depth->hash : 0;
    values[n++] = raster ? raster->hash : 0;
    const u32 rtvCount = (std::min)(numRTVs, kMaxRTVs);
    values[n++] = sampleMask;
    values[n++] = u64(topology);
    values[n++] = u64(stripCut);
    values[n++] = rtvCount;
    values[n++] = u64(dsvFormat);
    values[n++] = sample.Count;
    values[n++] = sample.Quality;
    for (u32 i = 0; i < kMaxRTVs; ++i)
        values[n++] = i < rtvCount ? u64(rtvFormats[i]) : 0;
    // Folded in only for mesh pipelines: the keys of vertex pipelines stay those of the library.
    if (meshKind)
        values[n++] = meshKind;
    return Hash64(values, n * sizeof(u64), kPsoHashVersion);
}

bool PipelineCache::Init(Device* device, const wchar_t* libraryPath)
{
    if (!device || !device->d12)
        return false;
    m_device = device;
    m_path = libraryPath ? libraryPath : L"";
    m_dirty = false;
    if (!device->d12_1 || m_path.empty())
    {
        Log("pipeline library: %s, pipelines are not persisted", device->d12_1 ? "no path" : "ID3D12Device1 unavailable");
        return true;
    }

    HRESULT hr = S_OK;
    try
    {
        if (ReadWholeFile(m_path, m_libraryBlob))
        {
            hr = device->d12_1->CreatePipelineLibrary(m_libraryBlob.data(), m_libraryBlob.size(), IID_PPV_ARGS(&m_library));
            if (FAILED(hr))
            {
                // Another driver, another GPU or a damaged file: start over.
                m_library = nullptr;
                Log("pipeline library rejected (0x%08X), starting empty", unsigned(hr));
                m_libraryBlob.clear();
                m_libraryBlob.shrink_to_fit();
                DeleteFileW(m_path.c_str());
            }
        }
    }
    catch (...)
    {
        m_libraryBlob.clear();
    }
    if (!m_library)
    {
        hr = device->d12_1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_library));
        if (FAILED(hr))
        {
            m_library = nullptr;
            Log("pipeline library unsupported (0x%08X), pipelines are not persisted", unsigned(hr));
        }
    }
    else
        Log("pipeline library loaded (%llu KB)", static_cast<unsigned long long>(m_libraryBlob.size() / 1024));
    return true;
}

void PipelineCache::Shutdown()
{
    bool dirty = false;
    {
        std::shared_lock lock(m_lock);
        dirty = m_dirty;
    }
    if (dirty)
        Save();
    {
        std::unique_lock lock(m_lock);
        for (auto& [key, pso] : m_pipelines)
        {
            if (pso)
                pso->Release();
        }
        m_pipelines.clear();
        m_dirty = false;
    }
    if (m_library)
    {
        m_library->Release();
        m_library = nullptr;
    }
    m_libraryBlob.clear();
    m_libraryBlob.shrink_to_fit();
    m_count.store(0);
}

void PipelineCache::Save()
{
    if (!m_library || m_path.empty())
        return;
    try
    {
        std::vector<u8> data;
        {
            std::lock_guard library(g_libraryLock);
            {
                std::unique_lock lock(m_lock);
                m_dirty = false;
            }
            const SIZE_T size = m_library->GetSerializedSize();
            if (!size || size >= SIZE_T(kMaxLibraryFileBytes))
            {
                if (size)
                    Log("pipeline library too large to save (%llu MB)", static_cast<unsigned long long>(size / kMB));
                return;
            }
            data.resize(size);
            const HRESULT hr = m_library->Serialize(data.data(), size);
            if (FAILED(hr))
            {
                Log("pipeline library serialization failed (0x%08X)", unsigned(hr));
                return;
            }
        }
        // Written aside and renamed, so a crash never leaves a truncated library behind.
        CreateParentDirectories(m_path);
        const std::wstring temp = m_path + L".tmp";
        if (!WriteWholeFile(temp, data.data(), data.size()) ||
            !MoveFileExW(temp.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            Log("pipeline library could not be written (error %lu)", GetLastError());
            DeleteFileW(temp.c_str());
            return;
        }
        Log("pipeline library saved: %u pipelines, %llu KB", m_count.load(), static_cast<unsigned long long>(data.size() / 1024));
    }
    catch (...)
    {
        Log("pipeline library save failed (out of memory)");
    }
}

ID3D12PipelineState* PipelineCache::Graphics(const GraphicsPsoDesc& desc, u64 hash)
{
    if (!m_device)
        return nullptr;
    try
    {
        {
            std::shared_lock lock(m_lock);
            if (const auto it = m_pipelines.find(hash); it != m_pipelines.end())
                return it->second;
        }
        // The runtime refuses pipelines that mix DXIL and DXBC stages: refused here, by name.
        {
            const Shader* stages[] = {desc.vs, desc.hs, desc.ds, desc.gs, desc.ps};
            const Shader* dxil = nullptr;
            const Shader* dxbc = nullptr;
            for (const Shader* s : stages)
            {
                if (s)
                    (s->dxil ? dxil : dxbc) = s;
            }
            if (dxil && dxbc)
            {
                static const char* const kStageNames[] = {"vs", "hs", "ds", "gs", "ps", "cs"};
                m_device->Count(Device::StatMixedPso);
                LogOnce(KeyOf(hash), "! graphics pipeline %016llx mixes DXIL (%s '%s') and DXBC (%s '%s') stages: refused",
                    static_cast<unsigned long long>(hash), kStageNames[u32(dxil->stage)], dxil->name.c_str(),
                    kStageNames[u32(dxbc->stage)], dxbc->name.c_str());
                return Insert(hash, nullptr, nullptr);
            }
        }
        if (desc.meshKind)
            return GraphicsMesh(desc, hash); // [dx12:mesh] hook

        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = m_device->rootSignatures.graphics;
        d.VS = Bytecode(desc.vs);
        d.PS = Bytecode(desc.ps);
        d.DS = Bytecode(desc.ds);
        d.HS = Bytecode(desc.hs);
        d.GS = Bytecode(desc.gs);
        d.BlendState = desc.blend ? desc.blend->desc12 : DefaultBlend();
        d.SampleMask = desc.sampleMask;
        d.RasterizerState = desc.raster ? desc.raster->desc12 : DefaultRaster();
        d.DepthStencilState = desc.depth ? desc.depth->desc12 : DefaultDepth();
        // D3D11 tests nothing without a depth buffer and no stencil without a stencil plane;
        // D3D12 validation rejects such states, so they are dropped from the pipeline.
        if (desc.dsvFormat == DXGI_FORMAT_UNKNOWN)
            d.DepthStencilState.DepthEnable = FALSE;
        if (!HasStencil(desc.dsvFormat))
            d.DepthStencilState.StencilEnable = FALSE;
        if (desc.inputLayout && !desc.inputLayout->elements.empty())
        {
            d.InputLayout.pInputElementDescs = desc.inputLayout->elements.data();
            d.InputLayout.NumElements = UINT(desc.inputLayout->elements.size());
        }
        d.IBStripCutValue = desc.stripCut;
        d.PrimitiveTopologyType = desc.topology;
        // Must equal the number of RTVs the context binds (D3D12 refuses extra ones, even null).
        d.NumRenderTargets = (std::min)(desc.numRTVs, kMaxRTVs);
        for (u32 i = 0; i < d.NumRenderTargets; ++i)
            d.RTVFormats[i] = desc.rtvFormats[i];
        d.DSVFormat = desc.dsvFormat;
        d.SampleDesc = desc.sample;
        d.NodeMask = 0;
        d.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        wchar_t name[17];
        PipelineName(hash, name);
        ID3D12PipelineState* pso = nullptr;
        bool loaded = false;
        if (m_library)
        {
            std::lock_guard library(g_libraryLock);
            loaded = SUCCEEDED(m_library->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pso)));
            if (!loaded)
                pso = nullptr;
        }
        if (!pso)
        {
            // Created outside every lock: other threads keep drawing with cached pipelines.
            const HRESULT hr = m_device->d12->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
            if (FAILED(hr))
            {
                pso = nullptr;
                LogOnce(KeyOf(hash),
                    "graphics pipeline %016llx refused (0x%08X): vs %016llx ps %016llx gs %016llx hs %016llx ds %016llx, "
                    "layout %016llx, topology %d, %u RTVs (%d %d %d %d), DSV %d, samples %u/%u",
                    static_cast<unsigned long long>(hash), unsigned(hr),
                    static_cast<unsigned long long>(desc.vs ? desc.vs->hash : 0),
                    static_cast<unsigned long long>(desc.ps ? desc.ps->hash : 0),
                    static_cast<unsigned long long>(desc.gs ? desc.gs->hash : 0),
                    static_cast<unsigned long long>(desc.hs ? desc.hs->hash : 0),
                    static_cast<unsigned long long>(desc.ds ? desc.ds->hash : 0),
                    static_cast<unsigned long long>(desc.inputLayout ? desc.inputLayout->hash : 0), int(desc.topology),
                    desc.numRTVs, int(desc.rtvFormats[0]), int(desc.rtvFormats[1]), int(desc.rtvFormats[2]),
                    int(desc.rtvFormats[3]), int(desc.dsvFormat), desc.sample.Count, desc.sample.Quality);
                if (desc.vs && desc.ps && (!desc.vs->name.empty() || !desc.ps->name.empty()))
                    LogOnce(KeyOf(hash ^ 1), "graphics pipeline %016llx: vs '%s', ps '%s'", static_cast<unsigned long long>(hash),
                        desc.vs->name.c_str(), desc.ps->name.c_str());
            }
        }
        return Insert(hash, pso, loaded ? nullptr : name);
    }
    catch (...)
    {
        return nullptr;
    }
}

ID3D12PipelineState* PipelineCache::Compute(Shader* cs)
{
    if (!m_device || !cs)
        return nullptr;
    const u64 key = HashCombine(cs->hash, kComputeKeySalt);
    try
    {
        {
            std::shared_lock lock(m_lock);
            if (cs->computePso)
                return cs->computePso;
            if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
                return it->second;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = m_device->rootSignatures.compute;
        d.CS = Bytecode(cs);
        d.NodeMask = 0;
        d.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        wchar_t name[17];
        PipelineName(key, name);
        ID3D12PipelineState* pso = nullptr;
        bool loaded = false;
        if (m_library)
        {
            std::lock_guard library(g_libraryLock);
            loaded = SUCCEEDED(m_library->LoadComputePipeline(name, &d, IID_PPV_ARGS(&pso)));
            if (!loaded)
                pso = nullptr;
        }
        if (!pso)
        {
            const HRESULT hr = m_device->d12->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
            if (FAILED(hr))
            {
                pso = nullptr;
                LogOnce(KeyOf(key), "compute pipeline %016llx (cs %016llx) refused (0x%08X)", static_cast<unsigned long long>(key),
                    static_cast<unsigned long long>(cs->hash), unsigned(hr));
            }
        }
        pso = Insert(key, pso, loaded ? nullptr : name);
        std::unique_lock lock(m_lock);
        cs->computePso = pso;
        return pso;
    }
    catch (...)
    {
        return nullptr;
    }
}

ID3D12PipelineState* PipelineCache::NativeCompute(u64 key, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, bool persist)
{
    if (!m_device)
        return nullptr;
    key = HashCombine(key, kNativeComputeKeySalt);
    try
    {
        {
            std::shared_lock lock(m_lock);
            if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
                return it->second;
        }
        wchar_t name[17];
        PipelineName(key, name);
        ID3D12PipelineState* pso = nullptr;
        bool loaded = false;
        if (m_library && persist)
        {
            std::lock_guard library(g_libraryLock);
            loaded = SUCCEEDED(m_library->LoadComputePipeline(name, &desc, IID_PPV_ARGS(&pso)));
            if (!loaded)
                pso = nullptr;
        }
        if (!pso)
        {
            const HRESULT hr = m_device->d12->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
            if (FAILED(hr))
            {
                pso = nullptr;
                LogOnce(KeyOf(key), "! native compute pipeline %016llx refused (0x%08X)", static_cast<unsigned long long>(key), unsigned(hr));
            }
        }
        return Insert(key, pso, loaded || !persist ? nullptr : name);
    }
    catch (...)
    {
        return nullptr;
    }
}

ID3D12PipelineState* PipelineCache::NativeStream(u64 key, const D3D12_PIPELINE_STATE_STREAM_DESC& desc, bool persist)
{
    if (!m_device || !m_device->d12_2)
        return nullptr;
    key = HashCombine(key, kNativeStreamKeySalt);
    try
    {
        {
            std::shared_lock lock(m_lock);
            if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
                return it->second;
        }
        wchar_t name[17];
        PipelineName(key, name);
        ID3D12PipelineState* pso = nullptr;
        bool loaded = false;
        ID3D12PipelineLibrary1* library1 = nullptr;
        if (m_library && persist && SUCCEEDED(m_library->QueryInterface(IID_PPV_ARGS(&library1))) && library1)
        {
            {
                std::lock_guard library(g_libraryLock);
                loaded = SUCCEEDED(library1->LoadPipeline(name, &desc, IID_PPV_ARGS(&pso)));
            }
            library1->Release();
            if (!loaded)
                pso = nullptr;
        }
        if (!pso)
        {
            const HRESULT hr = m_device->d12_2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
            if (FAILED(hr))
            {
                pso = nullptr;
                LogOnce(KeyOf(key), "! native stream pipeline %016llx refused (0x%08X)", static_cast<unsigned long long>(key), unsigned(hr));
            }
        }
        return Insert(key, pso, loaded || !persist ? nullptr : name);
    }
    catch (...)
    {
        return nullptr;
    }
}

ID3D12PipelineState* PipelineCache::Insert(u64 key, ID3D12PipelineState* pso, const wchar_t* storeName)
{
    // Failures are cached as null, so a refused state is not rebuilt at every draw.
    {
        std::unique_lock lock(m_lock);
        if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
        {
            if (pso)
                pso->Release();
            return it->second;
        }
        try
        {
            m_pipelines.emplace(key, pso);
        }
        catch (...)
        {
            if (pso)
                pso->Release();
            return nullptr;
        }
        if (pso)
            m_count.fetch_add(1);
    }
    // Outside m_lock: Save() takes the library lock first.
    if (pso && storeName && m_library)
    {
        bool stored = false;
        {
            std::lock_guard library(g_libraryLock);
            stored = SUCCEEDED(m_library->StorePipeline(storeName, pso));
        }
        if (stored)
        {
            std::unique_lock lock(m_lock);
            m_dirty = true;
        }
    }
    return pso;
}

// ============================================================================================
// Queries
// ============================================================================================

bool QueryHeapPool::Init(ID3D12Device* device)
{
    if (!device)
        return false;
    m_device = device;
    return true;
}

void QueryHeapPool::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (std::vector<Heap>* heaps : {&m_occlusion, &m_timestamp})
    {
        for (Heap& heap : *heaps)
        {
            if (heap.readback)
                heap.readback->Release();
            if (heap.heap)
                heap.heap->Release();
        }
        heaps->clear();
    }
}

QueryHeapPool::Slot QueryHeapPool::Allocate(D3D12_QUERY_HEAP_TYPE type)
{
    Slot slot;
    if (!m_device || (type != D3D12_QUERY_HEAP_TYPE_OCCLUSION && type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP))
        return slot;
    try
    {
        std::lock_guard lock(m_lock);
        std::vector<Heap>& heaps = type == D3D12_QUERY_HEAP_TYPE_OCCLUSION ? m_occlusion : m_timestamp;
        Heap* heap = nullptr;
        for (Heap& candidate : heaps)
        {
            if (!candidate.free.empty())
            {
                heap = &candidate;
                break;
            }
        }
        if (!heap)
        {
            Heap fresh;
            D3D12_QUERY_HEAP_DESC desc{};
            desc.Type = type;
            desc.Count = kQueriesPerHeap;
            HRESULT hr = m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&fresh.heap));
            u8* cpu = nullptr;
            if (SUCCEEDED(hr))
                hr = CreateCommittedBuffer(m_device, D3D12_HEAP_TYPE_READBACK, u64(kQueriesPerHeap) * sizeof(u64), &fresh.readback, &cpu);
            else
                fresh.heap = nullptr;
            if (FAILED(hr))
            {
                if (fresh.heap)
                    fresh.heap->Release();
                static constexpr char kFormat[] = "query heap (type %d) failed (0x%08X)";
                LogOnce(kFormat, kFormat, int(type), unsigned(hr));
                return slot;
            }
            fresh.mapped = reinterpret_cast<u64*>(cpu);
            try
            {
                // Slot 0 comes out first; Free() never grows the list past this capacity.
                fresh.free.resize(kQueriesPerHeap);
                for (u32 i = 0; i < kQueriesPerHeap; ++i)
                    fresh.free[i] = kQueriesPerHeap - 1 - i;
                heaps.push_back(std::move(fresh));
            }
            catch (...)
            {
                fresh.readback->Release();
                fresh.heap->Release();
                throw;
            }
            heap = &heaps.back();
        }
        slot.heap = heap->heap;
        slot.index = heap->free.back();
        heap->free.pop_back();
        slot.readback = heap->readback;
        slot.result = heap->mapped + slot.index;
    }
    catch (...)
    {
        slot = Slot{};
    }
    return slot;
}

void QueryHeapPool::Free(D3D12_QUERY_HEAP_TYPE type, const Slot& slot)
{
    if (!slot.heap)
        return;
    try
    {
        std::lock_guard lock(m_lock);
        std::vector<Heap>& heaps = type == D3D12_QUERY_HEAP_TYPE_OCCLUSION ? m_occlusion : m_timestamp;
        for (Heap& heap : heaps)
        {
            if (heap.heap == slot.heap)
            {
                heap.free.push_back(slot.index);
                return;
            }
        }
    }
    catch (...)
    {
    }
}
} // namespace xrd3d12
