// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Device: ID3D11Device over ID3D12Device - creation and teardown, frames, state object caches,
// input layout validation, device removal diagnostics, and the creation part of d12_api.h.
#include "d12_layer.h"

#include <d3d12sdklayers.h>

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

namespace xrd3d12
{
namespace
{
constexpr u32 kImmediateContextIndex = 255;

// LogOnce keys.
const char kKeyProgramType = 0;
const char kKeyReflect = 0;
const char kKeySignature = 0;
const char kKeyInputSlot = 0;

// ---- device registry ------------------------------------------------------------------------

// Init progress: a stage is recorded right before its Init call, so a failed Init and Shutdown
// tear down exactly what was set up (whatever Init was attempted gets its Shutdown).
enum InitStage : u32
{
    StageNone = 0,
    StageDevice,
    StageQueue,
    StageFence,
    StageCpuSrv,
    StageCpuRtv,
    StageCpuDsv,
    StageCpuSampler,
    StageGpuRing,
    StageSamplerTables,
    StageUploadPages,
    StageBufferPages,
    StageDynamicFirstVersions,
    StageNulls,
    StageInitUploader,
    StageRootSignatures,
    StagePipelines,
    StageQueryHeaps,
    StageImmediate,
    StageReady,
};

struct DeviceRecord
{
    Device* device;
    u32 stage;
    u32 nextDeferred; // index handed to the next deferred context
};

std::mutex g_devicesLock;
std::vector<DeviceRecord> g_devices;

DeviceRecord* FindRecord(const Device* device) // g_devicesLock held
{
    for (DeviceRecord& record : g_devices)
    {
        if (record.device == device)
            return &record;
    }
    return nullptr;
}

void SetStage(const Device* device, u32 stage)
{
    std::lock_guard lock(g_devicesLock);
    if (DeviceRecord* record = FindRecord(device))
        record->stage = stage;
}

// ---- COM helpers ----------------------------------------------------------------------------

template <class Body>
HRESULT Guarded(Body&& body) noexcept
{
    try
    {
        return body();
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        return E_FAIL;
    }
}

// Hands a freshly created object (one reference) to the caller; a null out pointer means the
// call only validated its arguments (D3D11: S_FALSE).
template <class Object, class Iface>
HRESULT HandOut(Object* object, Iface** out)
{
    if (!object)
        return E_FAIL;
    if (!out)
    {
        object->Release();
        return S_FALSE;
    }
    *out = object;
    return S_OK;
}

void Unsupported(const char* what)
{
    LogOnce(what, "! %s is not supported by the Direct3D 12 renderer", what);
}

// ---- DXBC containers ------------------------------------------------------------------------

constexpr u32 FourCC(char a, char b, char c, char d)
{
    return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

u32 Read32(const u8* p)
{
    u32 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// A chunk of a DXBC container: a whole shader or a signature-only blob (D3DGetInputSignatureBlob).
const u8* FindChunk(const void* blob, size_t size, u32 fourcc, u32& chunkSize)
{
    const u8* bytes = static_cast<const u8*>(blob);
    if (size < 32 || std::memcmp(bytes, "DXBC", 4) != 0)
        return nullptr;
    const u32 count = Read32(bytes + 28);
    if (count > (size - 32) / 4)
        return nullptr;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 offset = Read32(bytes + 32 + 4 * i);
        if (offset > size - 8)
            continue;
        const u32 length = Read32(bytes + offset + 4);
        if (length > size - 8 - offset)
            continue;
        if (Read32(bytes + offset) == fourcc)
        {
            chunkSize = length;
            return bytes + offset + 8;
        }
    }
    return nullptr;
}

struct SignatureInput
{
    const char* name;
    u32 nameLength;
    u32 index;
    u32 systemValue; // D3D_NAME
};
constexpr u32 kMaxSignatureInputs = 64;

// Parsed by hand: D3DReflect refuses the signature-only blobs the engine passes to
// CreateInputLayout (E_INVALIDARG).
bool ReadInputSignature(const void* blob, size_t size, SignatureInput* out, u32& count)
{
    count = 0;
    u32 length = 0;
    u32 stride = 24; // ISGN: name offset, semantic index, system value, component type, register, masks
    u32 field = 0;
    const u8* chunk = FindChunk(blob, size, FourCC('I', 'S', 'G', 'N'), length);
    if (!chunk)
    {
        // ISG1 (SM 5.1) prefixes every element with its stream and appends the min precision.
        chunk = FindChunk(blob, size, FourCC('I', 'S', 'G', '1'), length);
        stride = 32;
        field = 4;
    }
    constexpr u32 kFirstElement = 8;
    if (!chunk || length < kFirstElement)
        return false;
    const u32 elements = Read32(chunk);
    if (elements > kMaxSignatureInputs || elements > (length - kFirstElement) / stride)
        return false;
    for (u32 i = 0; i < elements; ++i)
    {
        const u8* element = chunk + kFirstElement + i * stride;
        const u32 nameOffset = Read32(element + field);
        if (nameOffset >= length)
            return false;
        const char* name = reinterpret_cast<const char*>(chunk + nameOffset);
        const void* end = std::memchr(name, 0, length - nameOffset);
        if (!end)
            return false;
        out[i].name = name;
        out[i].nameLength = u32(static_cast<const char*>(end) - name);
        out[i].index = Read32(element + field + 4);
        out[i].systemValue = Read32(element + field + 8);
    }
    count = elements;
    return true;
}

// Program type of the shader token stream (D3D10_SB_TOKENIZED_PROGRAM_TYPE in bits 16..31); DXIL's
// program header numbers the kinds the same way (13 mesh, 14 amplification).
bool ProgramType(const void* blob, size_t size, u32& type, bool& dxil)
{
    u32 length = 0;
    dxil = false;
    const u8* chunk = FindChunk(blob, size, FourCC('S', 'H', 'E', 'X'), length);
    if (!chunk)
        chunk = FindChunk(blob, size, FourCC('S', 'H', 'D', 'R'), length);
    if (!chunk)
    {
        chunk = FindChunk(blob, size, FourCC('D', 'X', 'I', 'L'), length);
        dxil = chunk;
    }
    if (!chunk || length < 4)
        return false;
    type = Read32(chunk) >> 16;
    return true;
}

u32 ExpectedProgramType(Stage stage)
{
    switch (stage)
    {
    case Stage::PS: return 0;
    case Stage::VS: return 1;
    case Stage::GS: return 2;
    case Stage::HS: return 3;
    case Stage::DS: return 4;
    case Stage::CS: return 5;
    default: return ~0u;
    }
}

// Mesh shaders come through CreateVertexShader, amplification shaders through CreateGeometryShader.
bool KindFitsStage(u32 kind, Stage stage)
{
    return kind == ExpectedProgramType(stage) || (stage == Stage::VS && kind == kKindMesh) ||
        (stage == Stage::GS && kind == kKindAmplification);
}

char Upper(char c)
{
    return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
}

bool SemanticEquals(const char* a, const char* b, u32 bLength)
{
    u32 i = 0;
    for (; i < bLength; ++i)
    {
        if (!a[i] || Upper(a[i]) != Upper(b[i]))
            return false;
    }
    return !a[i];
}

// Semantic matching is case-insensitive, so the hash is too.
u64 HashSemantic(u64 seed, const char* name)
{
    const u64 length = std::strlen(name);
    u64 hash = Hash64(&length, sizeof(length), seed);
    char buffer[64];
    size_t used = 0;
    for (const char* p = name; *p; ++p)
    {
        buffer[used++] = Upper(*p);
        if (used == sizeof(buffer))
        {
            hash = Hash64(buffer, used, hash);
            used = 0;
        }
    }
    return Hash64(buffer, used, hash);
}

// ---- state object keys and conversions ------------------------------------------------------
// Keys are built field by field: the descs have padding bytes, and GetDesc must return exactly
// what the engine passed (its caches compare GetDesc with the desc they built).

constexpr u64 kSeedBlend = 0x9e3779b97f4a7c15ull;
constexpr u64 kSeedDepth = 0xc2b2ae3d27d4eb4full;
constexpr u64 kSeedRaster = 0x165667b19e3779f9ull;
constexpr u64 kSeedSampler = 0x27d4eb2f165667c5ull;
constexpr u64 kSeedInputLayout = 0x85ebca77c2b2ae63ull;

u32 Bits(float value)
{
    return std::bit_cast<u32>(value);
}

using BlendKey11 = std::array<u32, 2 + 8 * 8>;
using BlendKey12 = std::array<u32, 2 + 8 * 10>;
using DepthKey = std::array<u32, 14>;
using RasterKey11 = std::array<u32, 10>;
using RasterKey12 = std::array<u32, 11>;
using SamplerKey = std::array<u32, 13>;

BlendKey11 KeyOf(const D3D11_BLEND_DESC& d)
{
    BlendKey11 k{};
    k[0] = u32(d.AlphaToCoverageEnable);
    k[1] = u32(d.IndependentBlendEnable);
    for (u32 i = 0; i < 8; ++i)
    {
        const D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[i];
        u32* p = &k[2 + i * 8];
        p[0] = u32(rt.BlendEnable);
        p[1] = u32(rt.SrcBlend);
        p[2] = u32(rt.DestBlend);
        p[3] = u32(rt.BlendOp);
        p[4] = u32(rt.SrcBlendAlpha);
        p[5] = u32(rt.DestBlendAlpha);
        p[6] = u32(rt.BlendOpAlpha);
        p[7] = u32(rt.RenderTargetWriteMask);
    }
    return k;
}

BlendKey12 KeyOf(const D3D12_BLEND_DESC& d)
{
    BlendKey12 k{};
    k[0] = u32(d.AlphaToCoverageEnable);
    k[1] = u32(d.IndependentBlendEnable);
    for (u32 i = 0; i < 8; ++i)
    {
        const D3D12_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[i];
        u32* p = &k[2 + i * 10];
        p[0] = u32(rt.BlendEnable);
        p[1] = u32(rt.LogicOpEnable);
        p[2] = u32(rt.SrcBlend);
        p[3] = u32(rt.DestBlend);
        p[4] = u32(rt.BlendOp);
        p[5] = u32(rt.SrcBlendAlpha);
        p[6] = u32(rt.DestBlendAlpha);
        p[7] = u32(rt.BlendOpAlpha);
        p[8] = u32(rt.LogicOp);
        p[9] = u32(rt.RenderTargetWriteMask);
    }
    return k;
}

// Disabled blending is normalised: D3D11 accepts zeroed factors there, D3D12 validates them, and
// equal effective states then share one PSO.
void Convert(const D3D11_BLEND_DESC& d, D3D12_BLEND_DESC& o)
{
    std::memset(&o, 0, sizeof(o));
    o.AlphaToCoverageEnable = d.AlphaToCoverageEnable;
    o.IndependentBlendEnable = d.IndependentBlendEnable;
    for (u32 i = 0; i < 8; ++i)
    {
        const D3D11_RENDER_TARGET_BLEND_DESC& s = d.RenderTarget[d.IndependentBlendEnable ? i : 0];
        D3D12_RENDER_TARGET_BLEND_DESC& t = o.RenderTarget[i];
        t.BlendEnable = s.BlendEnable;
        t.LogicOpEnable = FALSE;
        t.LogicOp = D3D12_LOGIC_OP_NOOP;
        t.RenderTargetWriteMask = s.RenderTargetWriteMask;
        if (s.BlendEnable)
        {
            t.SrcBlend = D3D12_BLEND(s.SrcBlend);
            t.DestBlend = D3D12_BLEND(s.DestBlend);
            t.BlendOp = D3D12_BLEND_OP(s.BlendOp);
            t.SrcBlendAlpha = D3D12_BLEND(s.SrcBlendAlpha);
            t.DestBlendAlpha = D3D12_BLEND(s.DestBlendAlpha);
            t.BlendOpAlpha = D3D12_BLEND_OP(s.BlendOpAlpha);
        }
        else
        {
            t.SrcBlend = D3D12_BLEND_ONE;
            t.DestBlend = D3D12_BLEND_ZERO;
            t.BlendOp = D3D12_BLEND_OP_ADD;
            t.SrcBlendAlpha = D3D12_BLEND_ONE;
            t.DestBlendAlpha = D3D12_BLEND_ZERO;
            t.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
    }
}

template <class Desc>
DepthKey DepthKeyOf(const Desc& d)
{
    return {u32(d.DepthEnable), u32(d.DepthWriteMask), u32(d.DepthFunc), u32(d.StencilEnable),
        u32(d.StencilReadMask), u32(d.StencilWriteMask), u32(d.FrontFace.StencilFailOp),
        u32(d.FrontFace.StencilDepthFailOp), u32(d.FrontFace.StencilPassOp), u32(d.FrontFace.StencilFunc),
        u32(d.BackFace.StencilFailOp), u32(d.BackFace.StencilDepthFailOp), u32(d.BackFace.StencilPassOp),
        u32(d.BackFace.StencilFunc)};
}

DepthKey KeyOf(const D3D11_DEPTH_STENCIL_DESC& d)
{
    return DepthKeyOf(d);
}

void Convert(const D3D11_DEPTH_STENCILOP_DESC& s, D3D12_DEPTH_STENCILOP_DESC& t)
{
    t.StencilFailOp = D3D12_STENCIL_OP(s.StencilFailOp);
    t.StencilDepthFailOp = D3D12_STENCIL_OP(s.StencilDepthFailOp);
    t.StencilPassOp = D3D12_STENCIL_OP(s.StencilPassOp);
    t.StencilFunc = D3D12_COMPARISON_FUNC(s.StencilFunc);
}

// Disabled depth or stencil parts are normalised (D3D11 accepts a zeroed desc).
void Convert(const D3D11_DEPTH_STENCIL_DESC& d, D3D12_DEPTH_STENCIL_DESC& o)
{
    std::memset(&o, 0, sizeof(o));
    o.DepthEnable = d.DepthEnable;
    o.DepthWriteMask = d.DepthEnable ? D3D12_DEPTH_WRITE_MASK(d.DepthWriteMask) : D3D12_DEPTH_WRITE_MASK_ZERO;
    o.DepthFunc = d.DepthEnable ? D3D12_COMPARISON_FUNC(d.DepthFunc) : D3D12_COMPARISON_FUNC_LESS;
    o.StencilEnable = d.StencilEnable;
    if (d.StencilEnable)
    {
        o.StencilReadMask = d.StencilReadMask;
        o.StencilWriteMask = d.StencilWriteMask;
        Convert(d.FrontFace, o.FrontFace);
        Convert(d.BackFace, o.BackFace);
    }
    else
    {
        o.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        o.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
            D3D12_COMPARISON_FUNC_ALWAYS};
        o.FrontFace = keep;
        o.BackFace = keep;
    }
}

RasterKey11 KeyOf(const D3D11_RASTERIZER_DESC& d)
{
    return {u32(d.FillMode), u32(d.CullMode), u32(d.FrontCounterClockwise), u32(d.DepthBias), Bits(d.DepthBiasClamp),
        Bits(d.SlopeScaledDepthBias), u32(d.DepthClipEnable), u32(d.ScissorEnable), u32(d.MultisampleEnable),
        u32(d.AntialiasedLineEnable)};
}

RasterKey12 KeyOf(const D3D12_RASTERIZER_DESC& d)
{
    return {u32(d.FillMode), u32(d.CullMode), u32(d.FrontCounterClockwise), u32(d.DepthBias), Bits(d.DepthBiasClamp),
        Bits(d.SlopeScaledDepthBias), u32(d.DepthClipEnable), u32(d.MultisampleEnable), u32(d.AntialiasedLineEnable),
        u32(d.ForcedSampleCount), u32(d.ConservativeRaster)};
}

void Convert(const D3D11_RASTERIZER_DESC& d, D3D12_RASTERIZER_DESC& o)
{
    std::memset(&o, 0, sizeof(o));
    o.FillMode = D3D12_FILL_MODE(d.FillMode);
    o.CullMode = D3D12_CULL_MODE(d.CullMode);
    o.FrontCounterClockwise = d.FrontCounterClockwise;
    o.DepthBias = d.DepthBias;
    o.DepthBiasClamp = d.DepthBiasClamp;
    o.SlopeScaledDepthBias = d.SlopeScaledDepthBias;
    o.DepthClipEnable = d.DepthClipEnable;
    o.MultisampleEnable = d.MultisampleEnable;
    o.AntialiasedLineEnable = d.AntialiasedLineEnable;
    o.ForcedSampleCount = 0;
    o.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
}

SamplerKey KeyOf(const D3D11_SAMPLER_DESC& d)
{
    return {u32(d.Filter), u32(d.AddressU), u32(d.AddressV), u32(d.AddressW), Bits(d.MipLODBias), d.MaxAnisotropy,
        u32(d.ComparisonFunc), Bits(d.BorderColor[0]), Bits(d.BorderColor[1]), Bits(d.BorderColor[2]),
        Bits(d.BorderColor[3]), Bits(d.MinLOD), Bits(d.MaxLOD)};
}

// D3D11 accepts a zero comparison function and anisotropy on non-comparison, non-anisotropic
// samplers; D3D12 validates both, and its descriptor LOD bias range is narrower.
void Convert(const D3D11_SAMPLER_DESC& d, D3D12_SAMPLER_DESC& o)
{
    std::memset(&o, 0, sizeof(o));
    o.Filter = D3D12_FILTER(d.Filter);
    o.AddressU = D3D12_TEXTURE_ADDRESS_MODE(d.AddressU);
    o.AddressV = D3D12_TEXTURE_ADDRESS_MODE(d.AddressV);
    o.AddressW = D3D12_TEXTURE_ADDRESS_MODE(d.AddressW);
    o.MipLODBias = (std::min)((std::max)(d.MipLODBias, D3D12_MIP_LOD_BIAS_MIN), D3D12_MIP_LOD_BIAS_MAX);
    o.MaxAnisotropy = (std::min)((std::max)(d.MaxAnisotropy, 1u), u32(D3D12_REQ_MAXANISOTROPY));
    o.ComparisonFunc = d.ComparisonFunc ? D3D12_COMPARISON_FUNC(d.ComparisonFunc) : D3D12_COMPARISON_FUNC_NEVER;
    for (u32 i = 0; i < 4; ++i)
        o.BorderColor[i] = d.BorderColor[i];
    o.MinLOD = d.MinLOD;
    o.MaxLOD = d.MaxLOD;
}

// Looks up or creates a state object. The cache keeps the creation reference, the caller gets
// its own. A hash collision (different key) yields an uncached object.
template <class State, class Iface, class Key, class Create>
HRESULT CachedState(std::mutex& lock, std::unordered_map<u64, State*>& cache, const Key& key, u64 seed, Create&& create,
    Iface** out)
{
    const u64 hash = Hash64(key.data(), key.size() * sizeof(u32), seed);
    return Guarded([&]() -> HRESULT {
        std::lock_guard guard(lock);
        bool collision = false;
        if (const auto it = cache.find(hash); it != cache.end())
        {
            if (KeyOf(it->second->desc11) == key)
            {
                if (!out)
                    return S_FALSE;
                it->second->AddRef();
                *out = it->second;
                return S_OK;
            }
            collision = true;
        }
        if (!out)
            return S_FALSE;
        State* state = nullptr;
        const HRESULT hr = create(state);
        if (FAILED(hr))
            return hr;
        if (!collision)
        {
            try
            {
                cache.emplace(hash, state);
            }
            catch (...)
            {
                state->Release();
                throw;
            }
            state->AddRef();
        }
        *out = state;
        return S_OK;
    });
}

template <class State>
void ReleaseAll(std::unordered_map<u64, State*>& cache)
{
    for (auto& entry : cache)
        entry.second->Release();
    cache.clear();
}

// ---- object creation templates ----------------------------------------------------------------

template <class Object, class Iface, class Desc>
HRESULT CreateResource(Device* device, const Desc* desc, const D3D11_SUBRESOURCE_DATA* data, Iface** out)
{
    if (out)
        *out = nullptr;
    if (!desc)
        return E_INVALIDARG;
    return Guarded([&]() -> HRESULT {
        Object* object = nullptr;
        const HRESULT hr = Object::Create(device, *desc, data, &object);
        if (FAILED(hr))
            return hr;
        return HandOut(object, out);
    });
}

template <class Object, class Iface, class Desc>
HRESULT CreateView(Device* device, ID3D11Resource* resource, const Desc* desc, Iface** out)
{
    if (out)
        *out = nullptr;
    if (!resource)
        return E_INVALIDARG;
    return Guarded([&]() -> HRESULT {
        Object* object = nullptr;
        const HRESULT hr = Object::Create(device, resource, desc, &object);
        if (FAILED(hr))
            return hr;
        return HandOut(object, out);
    });
}

template <class Object, class Iface>
HRESULT CreateShader(Device* device, Stage stage, const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage,
    Iface** out)
{
    if (out)
        *out = nullptr;
    if (!bytecode || !size)
        return E_INVALIDARG;
    if (linkage)
    {
        Unsupported("shader class linkage");
        return E_NOTIMPL;
    }
    u32 type = 0;
    bool dxil = false;
    const bool typed = ProgramType(bytecode, size, type, dxil);
    if (typed && !KindFitsStage(type, stage))
    {
        LogOnce(&kKeyProgramType, "! a shader was created through the call of another stage (program type %u)", type);
        return E_INVALIDARG;
    }
    return Guarded([&]() -> HRESULT {
        Object* shader = new Object(device);
        try
        {
            const u8* bytes = static_cast<const u8*>(bytecode);
            shader->bytecode.assign(bytes, bytes + size);
            shader->kind = typed ? u8(type) : u8(ExpectedProgramType(stage));
            shader->dxil = dxil;
            if (!shader->Reflect())
            {
                LogOnce(&kKeyReflect, "! shader reflection failed: the shader is refused");
                shader->Release();
                return E_INVALIDARG;
            }
            // Reflection of stripped DXIL has no semantic names: the signature part has them.
            if (dxil && stage == Stage::VS && shader->kind == kKindVertex)
            {
                SignatureInput inputs[kMaxSignatureInputs];
                u32 count = 0;
                shader->inputs.clear();
                if (ReadInputSignature(bytecode, size, inputs, count))
                {
                    for (u32 i = 0; i < count; ++i)
                    {
                        if (inputs[i].systemValue != D3D_NAME_UNDEFINED)
                            continue;
                        std::string name(inputs[i].name, inputs[i].nameLength);
                        for (char& c : name)
                            c = Upper(c);
                        shader->inputs.emplace_back(std::move(name), inputs[i].index);
                    }
                }
            }
        }
        catch (...)
        {
            shader->Release();
            throw;
        }
        return HandOut(shader, out);
    });
}

// ---- diagnostics ----------------------------------------------------------------------------

const char* RemovalText(HRESULT reason)
{
    switch (reason)
    {
    case S_OK: return "not removed";
    case DXGI_ERROR_DEVICE_HUNG: return "GPU hung (a command took too long or faulted)";
    case DXGI_ERROR_DEVICE_REMOVED: return "device removed (driver update, crash or physical removal)";
    case DXGI_ERROR_DEVICE_RESET: return "device reset (badly formed commands)";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "driver internal error";
    case DXGI_ERROR_INVALID_CALL: return "invalid call";
    case E_OUTOFMEMORY: return "out of memory";
    default: return "unknown";
    }
}

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    static const char* const names[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
        "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles",
        "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView",
        "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission",
        "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
        "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand",
        "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
        "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier",
        "BeginCommandList", "DispatchGraph", "SetProgram"};
    const u32 index = u32(op);
    return index < std::size(names) ? names[index] : "(unknown op)";
}

const wchar_t* NameOr(const wchar_t* name)
{
    return name ? name : L"(unnamed)";
}

void LogAllocations(const char* what, const D3D12_DRED_ALLOCATION_NODE* node)
{
    for (u32 shown = 0; node && shown < 16; node = node->pNext, ++shown)
        Log("! DRED:   %s allocation '%ls' (type %d)", what, NameOr(node->ObjectNameW), int(node->AllocationType));
}

// Debug layer messages: each kind is logged a few times, then suppressed.
constexpr u32 kMessageRepeatLimit = 8;
std::mutex g_messageLock;
std::unordered_map<int, u32> g_messageCounts;
std::vector<u64> g_messageBuffer; // u64 keeps D3D12_MESSAGE aligned

void Reason(char* out, size_t size, const char* fmt, ...)
{
    if (!out || !size)
        return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out, size, fmt, args);
    va_end(args);
}

void FeatureLevelText(D3D_FEATURE_LEVEL level, char (&out)[8])
{
    std::snprintf(out, sizeof(out), "%u_%u", (unsigned(level) >> 12) & 0xF, (unsigned(level) >> 8) & 0xF);
}
} // namespace

// ============================================================================================
// IDXGIDevice shim
// ============================================================================================

class Device::DxgiDevice final : public IDXGIDevice
{
public:
    explicit DxgiDevice(Device* owner) : m_owner(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_POINTER;
        if (riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIObject))
        {
            *object = static_cast<IDXGIDevice*>(this);
            AddRef();
            return S_OK;
        }
        // IUnknown and ID3D11Device resolve to the device (COM identity).
        return m_owner->QueryInterface(riid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        return Guarded([&] { return m_owner->m_private.Set(guid, size, data); });
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
    {
        return Guarded([&] { return m_owner->m_private.SetInterface(guid, data); });
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        return Guarded([&] { return m_owner->m_private.Get(guid, size, data); });
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override
    {
        if (!parent)
            return E_POINTER;
        *parent = nullptr;
        if (!m_owner->adapter)
            return E_FAIL;
        return m_owner->adapter->QueryInterface(riid, parent);
    }

    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** adapter) override
    {
        if (!adapter)
            return E_INVALIDARG;
        *adapter = m_owner->adapter;
        if (!*adapter)
            return E_FAIL;
        (*adapter)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
        IDXGISurface** surface) override
    {
        if (surface)
            *surface = nullptr;
        Unsupported("IDXGIDevice::CreateSurface");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const* resources, DXGI_RESIDENCY* status, UINT count) override
    {
        if ((count && !resources) || (count && !status))
            return E_INVALIDARG;
        for (UINT i = 0; i < count; ++i)
            status[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override
    {
        return priority < -7 || priority > 7 ? E_INVALIDARG : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* priority) override
    {
        if (!priority)
            return E_POINTER;
        *priority = 0;
        return S_OK;
    }

private:
    Device* m_owner;
};

// ============================================================================================
// IUnknown
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device))
    {
        *object = static_cast<ID3D11Device*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IDXGIDevice) && m_dxgiDevice)
    {
        *object = static_cast<IDXGIDevice*>(m_dxgiDevice);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Device::AddRef()
{
    return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE Device::Release()
{
    const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0)
    {
        // A transient AddRef/Release pair during teardown must not reach zero a second time.
        m_refs.store(0x40000000, std::memory_order_relaxed);
        Shutdown();
        delete this;
    }
    return refs;
}

// ============================================================================================
// Resources, views, queries
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::CreateBuffer(const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** buffer)
{
    return CreateResource<Buffer>(this, desc, data, buffer);
}

HRESULT STDMETHODCALLTYPE Device::CreateTexture1D(const D3D11_TEXTURE1D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture1D** texture)
{
    return CreateResource<Texture1D>(this, desc, data, texture);
}

HRESULT STDMETHODCALLTYPE Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** texture)
{
    return CreateResource<Texture2D>(this, desc, data, texture);
}

HRESULT STDMETHODCALLTYPE Device::CreateTexture3D(const D3D11_TEXTURE3D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture3D** texture)
{
    return CreateResource<Texture3D>(this, desc, data, texture);
}

HRESULT STDMETHODCALLTYPE Device::CreateShaderResourceView(ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** view)
{
    return CreateView<ShaderResourceView>(this, resource, desc, view);
}

HRESULT STDMETHODCALLTYPE Device::CreateUnorderedAccessView(ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, ID3D11UnorderedAccessView** view)
{
    return CreateView<UnorderedAccessView>(this, resource, desc, view);
}

HRESULT STDMETHODCALLTYPE Device::CreateRenderTargetView(ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** view)
{
    return CreateView<RenderTargetView>(this, resource, desc, view);
}

HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilView(ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, ID3D11DepthStencilView** view)
{
    return CreateView<DepthStencilView>(this, resource, desc, view);
}

HRESULT STDMETHODCALLTYPE Device::CreateQuery(const D3D11_QUERY_DESC* desc, ID3D11Query** query)
{
    if (query)
        *query = nullptr;
    if (!desc)
        return E_INVALIDARG;
    return Guarded([&]() -> HRESULT {
        Query* object = nullptr;
        const HRESULT hr = Query::Create(this, *desc, &object);
        if (FAILED(hr))
            return hr;
        return HandOut(object, query);
    });
}

HRESULT STDMETHODCALLTYPE Device::CreatePredicate(const D3D11_QUERY_DESC*, ID3D11Predicate** predicate)
{
    if (predicate)
        *predicate = nullptr;
    Unsupported("CreatePredicate");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Device::CreateCounter(const D3D11_COUNTER_DESC*, ID3D11Counter** counter)
{
    if (counter)
        *counter = nullptr;
    Unsupported("CreateCounter");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Device::OpenSharedResource(HANDLE, REFIID, void** object)
{
    if (object)
        *object = nullptr;
    Unsupported("OpenSharedResource");
    return E_NOTIMPL;
}

// ============================================================================================
// Input layouts
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void* bytecode,
    SIZE_T size, ID3D11InputLayout** layout)
{
    if (layout)
        *layout = nullptr;
    // D3D11 refuses an empty layout too (a shader without IA inputs draws with none bound).
    if (!count || !elements || count > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT || !bytecode || !size)
        return E_INVALIDARG;

    SignatureInput inputs[kMaxSignatureInputs];
    u32 inputCount = 0;
    if (!ReadInputSignature(bytecode, size, inputs, inputCount))
    {
        LogOnce(&kKeySignature, "! CreateInputLayout: the shader blob has no readable input signature");
        return E_INVALIDARG;
    }

    // The same refusals as the D3D11 runtime: the engine retries or drops the draw on E_INVALIDARG.
    for (UINT i = 0; i < count; ++i)
    {
        const D3D11_INPUT_ELEMENT_DESC& e = elements[i];
        if (!e.SemanticName || !*e.SemanticName)
            return E_INVALIDARG;
        if (e.InputSlot >= kMaxVBs)
        {
            LogOnce(&kKeyInputSlot, "! CreateInputLayout: input slot %u is beyond the %u supported", e.InputSlot, kMaxVBs);
            return E_INVALIDARG;
        }
        if (e.InputSlotClass == D3D11_INPUT_PER_VERTEX_DATA)
        {
            if (e.InstanceDataStepRate)
                return E_INVALIDARG;
        }
        else if (e.InputSlotClass != D3D11_INPUT_PER_INSTANCE_DATA)
        {
            return E_INVALIDARG;
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{e.Format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        if (FAILED(d12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER))
        {
            return E_INVALIDARG;
        }
        for (UINT j = 0; j < i; ++j)
        {
            if (elements[j].SemanticIndex == e.SemanticIndex &&
                SemanticEquals(elements[j].SemanticName, e.SemanticName, u32(std::strlen(e.SemanticName))))
            {
                return E_INVALIDARG;
            }
        }
    }
    // Every signature element that is not a system value needs an element, read or not; component
    // counts and types may differ (missing components read as 0,0,0,1).
    for (u32 i = 0; i < inputCount; ++i)
    {
        if (inputs[i].systemValue != D3D_NAME_UNDEFINED)
            continue;
        bool found = false;
        for (UINT j = 0; j < count && !found; ++j)
            found = elements[j].SemanticIndex == inputs[i].index && SemanticEquals(elements[j].SemanticName, inputs[i].name, inputs[i].nameLength);
        if (!found)
            return E_INVALIDARG;
    }
    if (!layout)
        return S_FALSE;

    return Guarded([&]() -> HRESULT {
        InputLayout* object = new InputLayout(this);
        try
        {
            // Names first: the element pointers must not see the vector grow.
            object->names.reserve(count);
            for (UINT i = 0; i < count; ++i)
                object->names.emplace_back(elements[i].SemanticName);
            object->elements.resize(count);
            u64 hash = Hash64(&count, sizeof(count), kSeedInputLayout);
            for (UINT i = 0; i < count; ++i)
            {
                const D3D11_INPUT_ELEMENT_DESC& s = elements[i];
                D3D12_INPUT_ELEMENT_DESC& t = object->elements[i];
                t.SemanticName = object->names[i].c_str();
                t.SemanticIndex = s.SemanticIndex;
                t.Format = s.Format;
                t.InputSlot = s.InputSlot;
                t.AlignedByteOffset = s.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? D3D12_APPEND_ALIGNED_ELEMENT : s.AlignedByteOffset;
                t.InputSlotClass = s.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                                                      : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                t.InstanceDataStepRate = s.InstanceDataStepRate;
                hash = HashSemantic(hash, s.SemanticName);
                const u32 fields[6] = {t.SemanticIndex, u32(t.Format), t.InputSlot, t.AlignedByteOffset, u32(t.InputSlotClass),
                    t.InstanceDataStepRate};
                hash = Hash64(fields, sizeof(fields), hash);
            }
            object->hash = hash;
        }
        catch (...)
        {
            object->Release();
            throw;
        }
        return HandOut(object, layout);
    });
}

// ============================================================================================
// Shaders
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::CreateVertexShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader)
{
    return CreateShader<VertexShader>(this, Stage::VS, bytecode, size, linkage, shader);
}

HRESULT STDMETHODCALLTYPE Device::CreateGeometryShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11GeometryShader** shader)
{
    return CreateShader<GeometryShader>(this, Stage::GS, bytecode, size, linkage, shader);
}

HRESULT STDMETHODCALLTYPE Device::CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT,
    const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader** shader)
{
    if (shader)
        *shader = nullptr;
    Unsupported("stream output");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Device::CreatePixelShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader)
{
    return CreateShader<PixelShader>(this, Stage::PS, bytecode, size, linkage, shader);
}

HRESULT STDMETHODCALLTYPE Device::CreateHullShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11HullShader** shader)
{
    return CreateShader<HullShader>(this, Stage::HS, bytecode, size, linkage, shader);
}

HRESULT STDMETHODCALLTYPE Device::CreateDomainShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11DomainShader** shader)
{
    return CreateShader<DomainShader>(this, Stage::DS, bytecode, size, linkage, shader);
}

HRESULT STDMETHODCALLTYPE Device::CreateComputeShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11ComputeShader** shader)
{
    return CreateShader<ComputeShader>(this, Stage::CS, bytecode, size, linkage, shader);
}

HRESULT STDMETHODCALLTYPE Device::CreateClassLinkage(ID3D11ClassLinkage** linkage)
{
    if (linkage)
        *linkage = nullptr;
    Unsupported("CreateClassLinkage");
    return E_NOTIMPL;
}

// ============================================================================================
// State objects
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::CreateBlendState(const D3D11_BLEND_DESC* desc, ID3D11BlendState** state)
{
    if (state)
        *state = nullptr;
    if (!desc)
        return E_INVALIDARG;
    return CachedState(stateLock, blendStates, KeyOf(*desc), kSeedBlend,
        [&](BlendState*& out) -> HRESULT {
            BlendState* object = new BlendState(this);
            object->desc11 = *desc;
            Convert(*desc, object->desc12);
            const BlendKey12 key = KeyOf(object->desc12);
            object->hash = Hash64(key.data(), key.size() * sizeof(u32), kSeedBlend);
            out = object;
            return S_OK;
        },
        state);
}

HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* desc, ID3D11DepthStencilState** state)
{
    if (state)
        *state = nullptr;
    if (!desc)
        return E_INVALIDARG;
    return CachedState(stateLock, depthStates, KeyOf(*desc), kSeedDepth,
        [&](DepthStencilState*& out) -> HRESULT {
            DepthStencilState* object = new DepthStencilState(this);
            object->desc11 = *desc;
            Convert(*desc, object->desc12);
            const DepthKey key = DepthKeyOf(object->desc12);
            object->hash = Hash64(key.data(), key.size() * sizeof(u32), kSeedDepth);
            out = object;
            return S_OK;
        },
        state);
}

HRESULT STDMETHODCALLTYPE Device::CreateRasterizerState(const D3D11_RASTERIZER_DESC* desc, ID3D11RasterizerState** state)
{
    if (state)
        *state = nullptr;
    if (!desc)
        return E_INVALIDARG;
    return CachedState(stateLock, rasterStates, KeyOf(*desc), kSeedRaster,
        [&](RasterizerState*& out) -> HRESULT {
            RasterizerState* object = new RasterizerState(this);
            object->desc11 = *desc;
            Convert(*desc, object->desc12);
            object->scissorEnable = desc->ScissorEnable != FALSE;
            // Without ScissorEnable: states differing only in it share a PSO.
            const RasterKey12 key = KeyOf(object->desc12);
            object->hash = Hash64(key.data(), key.size() * sizeof(u32), kSeedRaster);
            out = object;
            return S_OK;
        },
        state);
}

HRESULT STDMETHODCALLTYPE Device::CreateSamplerState(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** state)
{
    if (state)
        *state = nullptr;
    if (!desc || desc->MaxAnisotropy > u32(D3D11_REQ_MAXANISOTROPY))
        return E_INVALIDARG;
    return CachedState(stateLock, samplerStates, KeyOf(*desc), kSeedSampler,
        [&](SamplerState*& out) -> HRESULT {
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu = cpuSampler.Allocate();
            if (!cpu.ptr)
                return E_OUTOFMEMORY;
            SamplerState* object = nullptr;
            try
            {
                object = new SamplerState(this);
            }
            catch (...)
            {
                cpuSampler.Free(cpu);
                throw;
            }
            object->desc11 = *desc;
            object->cpu = cpu;
            object->id = nextSamplerId.fetch_add(1, std::memory_order_relaxed);
            D3D12_SAMPLER_DESC native;
            Convert(*desc, native);
            d12->CreateSampler(&native, cpu);
            out = object;
            return S_OK;
        },
        state);
}

// ============================================================================================
// Contexts
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::CreateDeferredContext(UINT, ID3D11DeviceContext** context)
{
    if (!context)
        return E_INVALIDARG;
    *context = nullptr;
    return Guarded([&]() -> HRESULT {
        u32 index = 0;
        {
            std::lock_guard lock(g_devicesLock);
            if (DeviceRecord* record = FindRecord(this))
                index = record->nextDeferred++;
        }
        *context = new Context(this, true, index);
        return S_OK;
    });
}

void STDMETHODCALLTYPE Device::GetImmediateContext(ID3D11DeviceContext** context)
{
    if (!context)
        return;
    *context = immediate;
    if (immediate)
        immediate->AddRef();
}

// ============================================================================================
// Capabilities
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::CheckFormatSupport(DXGI_FORMAT format, UINT* support)
{
    if (!support)
        return E_INVALIDARG;
    *support = 0;
    // D3D12_FORMAT_SUPPORT1 has the bit values of D3D11_FORMAT_SUPPORT.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT data{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    if (FAILED(d12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data))))
        return E_FAIL;
    *support = UINT(data.Support1);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Device::CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels)
{
    if (!levels)
        return E_INVALIDARG;
    *levels = 0;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS data{format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
    const HRESULT hr = d12->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &data, sizeof(data));
    if (FAILED(hr))
        return hr;
    *levels = data.NumQualityLevels;
    return S_OK;
}

void STDMETHODCALLTYPE Device::CheckCounterInfo(D3D11_COUNTER_INFO* info)
{
    if (!info)
        return;
    info->LastDeviceDependentCounter = D3D11_COUNTER(0);
    info->NumSimultaneousCounters = 0;
    info->NumDetectableParallelUnits = 1;
}

HRESULT STDMETHODCALLTYPE Device::CheckCounter(const D3D11_COUNTER_DESC*, D3D11_COUNTER_TYPE*, UINT*, LPSTR, UINT*, LPSTR, UINT*,
    LPSTR, UINT*)
{
    Unsupported("CheckCounter");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Device::CheckFeatureSupport(D3D11_FEATURE feature, void* data, UINT size)
{
    if (!data)
        return E_INVALIDARG;
    switch (feature)
    {
    case D3D11_FEATURE_THREADING:
    {
        if (size != sizeof(D3D11_FEATURE_DATA_THREADING))
            return E_INVALIDARG;
        auto* out = static_cast<D3D11_FEATURE_DATA_THREADING*>(data);
        out->DriverConcurrentCreates = TRUE;
        out->DriverCommandLists = TRUE;
        return S_OK;
    }
    case D3D11_FEATURE_DOUBLES:
    {
        if (size != sizeof(D3D11_FEATURE_DATA_DOUBLES))
            return E_INVALIDARG;
        static_cast<D3D11_FEATURE_DATA_DOUBLES*>(data)->DoublePrecisionFloatShaderOps = options.DoublePrecisionFloatShaderOps;
        return S_OK;
    }
    case D3D11_FEATURE_FORMAT_SUPPORT:
    {
        if (size != sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT))
            return E_INVALIDARG;
        auto* out = static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT*>(data);
        return CheckFormatSupport(out->InFormat, &out->OutFormatSupport);
    }
    case D3D11_FEATURE_FORMAT_SUPPORT2:
    {
        if (size != sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT2))
            return E_INVALIDARG;
        auto* out = static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT2*>(data);
        out->OutFormatSupport2 = 0;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT native{out->InFormat, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        if (FAILED(d12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &native, sizeof(native))))
            return E_FAIL;
        // Equal bits up to TILED and MULTIPLANE_OVERLAY; D3D12's 0x8000 is SAMPLER_FEEDBACK, not SHAREABLE.
        out->OutFormatSupport2 = UINT(native.Support2) & (0x3FFu | D3D11_FORMAT_SUPPORT2_MULTIPLANE_OVERLAY);
        return S_OK;
    }
    case D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS:
    {
        if (size != sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS))
            return E_INVALIDARG;
        static_cast<D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS*>(data)->ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x = TRUE;
        return S_OK;
    }
    case D3D11_FEATURE_D3D11_OPTIONS:
    {
        if (size != sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS))
            return E_INVALIDARG;
        // What this layer implements, not what the hardware could: no 11.1 context API.
        auto* out = static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS*>(data);
        std::memset(out, 0, sizeof(*out));
        out->OutputMergerLogicOp = options.OutputMergerLogicOp;
        out->SAD4ShaderInstructions = TRUE;
        out->ExtendedDoublesShaderInstructions = options.DoublePrecisionFloatShaderOps;
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

// ============================================================================================
// Private data, removal
// ============================================================================================

HRESULT STDMETHODCALLTYPE Device::GetPrivateData(REFGUID guid, UINT* size, void* data)
{
    return Guarded([&] { return m_private.Get(guid, size, data); });
}

HRESULT STDMETHODCALLTYPE Device::SetPrivateData(REFGUID guid, UINT size, const void* data)
{
    return Guarded([&] { return m_private.Set(guid, size, data); });
}

HRESULT STDMETHODCALLTYPE Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* data)
{
    return Guarded([&] { return m_private.SetInterface(guid, data); });
}

HRESULT STDMETHODCALLTYPE Device::GetDeviceRemovedReason()
{
    return d12 ? d12->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
}

void Device::ReportRemoval(HRESULT hr)
{
    if (std::atomic_ref<bool>(removed).exchange(true))
        return;
    const HRESULT reason = d12 ? d12->GetDeviceRemovedReason() : hr;
    Log("! device lost: 0x%08X, removed reason 0x%08X: %s", unsigned(hr), unsigned(reason), RemovalText(reason));
    DrainDebugMessages();
    if (!d12)
        return;
    if (!createInfo.dred)
    {
        Log("* start with -dx12dred to log the commands the GPU was executing");
        return;
    }

    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(d12->QueryInterface(IID_PPV_ARGS(&dred))) || !dred)
    {
        Log("! DRED: no data (not supported by this Windows version)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
    {
        u32 suspects = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node && suspects < 32; node = node->pNext)
        {
            const u32 total = node->BreadcrumbCount;
            const u32 done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            // Lists that ran to the end are not the culprit.
            if (done >= total)
                continue;
            ++suspects;
            const D3D12_AUTO_BREADCRUMB_OP* history = node->pCommandHistory;
            Log("! DRED: list '%ls' on '%ls': %u of %u ops completed, last completed %s, first unfinished %s",
                NameOr(node->pCommandListDebugNameW), NameOr(node->pCommandQueueDebugNameW), done, total,
                history && done ? OpName(history[done - 1]) : "(none)", history ? OpName(history[done]) : "(unknown)");
        }
        if (!suspects)
            Log("! DRED: every recorded command list completed");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA)
    {
        Log("! DRED: page fault at GPU address 0x%016llX", static_cast<unsigned long long>(fault.PageFaultVA));
        LogAllocations("existing", fault.pHeadExistingAllocationNode);
        LogAllocations("recently freed", fault.pHeadRecentFreedAllocationNode);
    }
    dred->Release();
}

void Device::DrainDebugMessages()
{
    if (!m_infoQueue)
        return;
    try
    {
        std::lock_guard lock(g_messageLock);
        const UINT64 count = m_infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T length = 0;
            if (FAILED(m_infoQueue->GetMessage(i, nullptr, &length)) || !length)
                continue;
            g_messageBuffer.resize((length + sizeof(u64) - 1) / sizeof(u64));
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(g_messageBuffer.data());
            if (FAILED(m_infoQueue->GetMessage(i, message, &length)) || !message->pDescription)
                continue;
            const char* prefix;
            const char* severity;
            switch (message->Severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: prefix = "!"; severity = "CORRUPTION"; break;
            case D3D12_MESSAGE_SEVERITY_ERROR: prefix = "!"; severity = "ERROR"; break;
            case D3D12_MESSAGE_SEVERITY_WARNING: prefix = "~"; severity = "WARNING"; break;
            case D3D12_MESSAGE_SEVERITY_MESSAGE: prefix = "*"; severity = "MESSAGE"; break;
            default: continue;
            }
            const u32 seen = ++g_messageCounts[int(message->ID)];
            if (seen > kMessageRepeatLimit)
                continue;
            // "debug layer <SEVERITY> #<id>" is what tools/qa/d3d12layer counts as a failure.
            Log("%s debug layer %s #%d: %s%s", prefix, severity, int(message->ID), message->pDescription,
                seen == kMessageRepeatLimit ? " (repeats; further ones are not logged)" : "");
        }
    }
    catch (...)
    {
    }
    m_infoQueue->ClearStoredMessages();
}

// ============================================================================================
// Lifetime
// ============================================================================================

HRESULT Device::Init(const DeviceCreateInfo& info)
{
    return Guarded([&]() -> HRESULT {
        createInfo = info;
        // The caller's string does not outlive Init; the pipeline cache keeps its own copy.
        createInfo.pipelineCachePath = nullptr;
        {
            std::lock_guard lock(g_devicesLock);
            g_devices.push_back({this, StageNone, 0});
        }
        const auto fail = [](const char* what, HRESULT hr) {
            Log("! device creation failed: %s (0x%08X)", what, unsigned(hr));
            return hr;
        };

        if (!info.adapter)
            return fail("no adapter", E_INVALIDARG);
        if (!g_d3d12.Load())
            return fail("d3d12.dll is not available", DXGI_ERROR_UNSUPPORTED);

        // Both must be set before the device exists (enabling the debug layer later removes it).
        const bool debugLayer = info.debugLayer || info.gpuValidation;
        if (debugLayer)
        {
            ID3D12Debug* debug = nullptr;
            if (SUCCEEDED(g_d3d12.GetDebugInterface(IID_PPV_ARGS(&debug))) && debug)
            {
                debug->EnableDebugLayer();
                if (info.gpuValidation)
                {
                    ID3D12Debug1* debug1 = nullptr;
                    if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))) && debug1)
                    {
                        debug1->SetEnableGPUBasedValidation(TRUE);
                        debug1->Release();
                        Log("* GPU-based validation on");
                    }
                }
                debug->Release();
                Log("* debug layer on");
            }
            else
            {
                Log("! the debug layer is not available (install the Graphics Tools optional feature)");
            }
        }
        if (info.dred)
        {
            ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
            if (SUCCEEDED(g_d3d12.GetDebugInterface(IID_PPV_ARGS(&dred))) && dred)
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->Release();
                Log("* DRED on");
            }
            else
            {
                Log("! DRED is not available on this Windows version");
            }
        }

        // PIX, RenderDoc and Nsight inject these before the device exists.
        markers = debugLayer || GetModuleHandleW(L"WinPixGpuCapturer.dll") || GetModuleHandleW(L"renderdoc.dll") ||
            GetModuleHandleW(L"Nvda.Graphics.Interception.dll");
        if (markers)
            Log("* annotation events on");
        stats = info.stats;

        adapter = info.adapter;
        adapter->AddRef();
        SetStage(this, StageDevice);
        HRESULT hr = g_d3d12.CreateDevice(info.adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
        if (FAILED(hr) || !d12)
            return fail("no Direct3D 12 device with feature level 11_0", FAILED(hr) ? hr : E_FAIL);
        m_dxgiDevice = new DxgiDevice(this);

        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels{UINT(std::size(levels)), levels, D3D_FEATURE_LEVEL_11_0};
        if (SUCCEEDED(d12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels))))
            m_featureLevel = featureLevels.MaxSupportedFeatureLevel;
        hr = d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        if (FAILED(hr))
            return fail("D3D12_OPTIONS query", hr);
        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
        {
            Log("! the adapter has resource binding tier 1; the Direct3D 12 renderer needs tier 2");
            return DXGI_ERROR_UNSUPPORTED;
        }
        if (FAILED(d12->QueryInterface(IID_PPV_ARGS(&d12_1))))
            d12_1 = nullptr;
        if (FAILED(d12->QueryInterface(IID_PPV_ARGS(&d12_2))))
            d12_2 = nullptr;
        if (FAILED(d12->QueryInterface(IID_PPV_ARGS(&d12_5))))
            d12_5 = nullptr;
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
        if (info.gpuUploadHeaps &&
            SUCCEEDED(d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16))) &&
            options16.GPUUploadHeapSupported)
            dynamicHeapType = D3D12_HEAP_TYPE_GPU_UPLOAD;

        {
            DXGI_ADAPTER_DESC1 desc{};
            char level[8];
            FeatureLevelText(m_featureLevel, level);
            if (FAILED(info.adapter->GetDesc1(&desc)))
                desc.Description[0] = L'\0';
            Log("* device: %ls, feature level %s, resource binding tier %d, resource heap tier %d, typed UAV loads %d, "
                "dynamic buffers in %s memory",
                desc.Description, level, int(options.ResourceBindingTier), int(options.ResourceHeapTier),
                int(options.TypedUAVLoadAdditionalFormats),
                dynamicHeapType == D3D12_HEAP_TYPE_GPU_UPLOAD ? "video" : "system");
        }

        if (debugLayer && SUCCEEDED(d12->QueryInterface(IID_PPV_ARGS(&m_infoQueue))) && m_infoQueue)
        {
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            // Benign by design: D3D11 clears have no optimized clear values, persistent maps use
            // null ranges, buffers ignore their initial state, every new pipeline misses the library
            // once, and D3D11 pixel shaders may write targets that are not bound (discarded).
            D3D12_MESSAGE_ID ids[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE, D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
                D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE, D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED,
                D3D12_MESSAGE_ID_LOADPIPELINE_NAMENOTFOUND, D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET};
            D3D12_MESSAGE_SEVERITY severities[] = {D3D12_MESSAGE_SEVERITY_INFO};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = UINT(std::size(ids));
            filter.DenyList.pIDList = ids;
            filter.DenyList.NumSeverities = UINT(std::size(severities));
            filter.DenyList.pSeverityList = severities;
            m_infoQueue->PushStorageFilter(&filter);
        }

        if (info.deviceCreated)
            info.deviceCreated(d12);

        // Capabilities of the feature work; the compiler decides `sm` together with the device. Its
        // pipeline self-check creates objects, so it comes after the device-created hook.
        disabledFeatures = info.disabledFeatures;
        QueryNativeCaps(d12, info.adapter, caps);
        if (!(disabledFeatures & NativeFeatureSm6) && caps.shaderModel >= D3D_SHADER_MODEL_6_0)
            caps.dxcShaderModel = InitShaderCompiler(d12);
        MaskNativeCaps(caps, disabledFeatures);

        SetStage(this, StageQueue);
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = info.createQueue ? info.createQueue(d12, &queueDesc, &queue) : d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
        if (FAILED(hr) || !queue)
            return fail("command queue", FAILED(hr) ? hr : E_FAIL);
        queue->SetName(L"xrD3D12 direct");

        SetStage(this, StageFence);
        if (!fence.Init(d12) || !initFence.Init(d12))
            return fail("fence", E_FAIL);
        releaser.Init(&fence);

        SetStage(this, StageCpuSrv);
        if (!cpuSrv.Init(d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096))
            return fail("CBV/SRV/UAV descriptor heap", E_OUTOFMEMORY);
        SetStage(this, StageCpuRtv);
        if (!cpuRtv.Init(d12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 512))
            return fail("RTV descriptor heap", E_OUTOFMEMORY);
        SetStage(this, StageCpuDsv);
        if (!cpuDsv.Init(d12, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256))
            return fail("DSV descriptor heap", E_OUTOFMEMORY);
        SetStage(this, StageCpuSampler);
        if (!cpuSampler.Init(d12, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024))
            return fail("sampler descriptor heap", E_OUTOFMEMORY);
        SetStage(this, StageGpuRing);
        if (!gpuRing.Init(d12, &fence, kDescriptorRingCapacity))
            return fail("shader-visible descriptor ring", E_OUTOFMEMORY);
        SetStage(this, StageSamplerTables);
        if (!samplerTables.Init(d12, this))
            return fail("shader-visible sampler heap", E_OUTOFMEMORY);
        SetStage(this, StageUploadPages);
        if (!uploadPages.Init(d12, &fence))
            return fail("upload pages", E_OUTOFMEMORY);
        SetStage(this, StageBufferPages);
        if (!bufferPages.Init(d12, D3D12_HEAP_TYPE_DEFAULT))
            return fail("buffer pages", E_OUTOFMEMORY);
        SetStage(this, StageDynamicFirstVersions);
        if (!dynamicFirstVersions.Init(d12, dynamicHeapType))
            return fail("dynamic buffer pages", E_OUTOFMEMORY);
        SetStage(this, StageNulls);
        if (!nulls.Init(this))
            return fail("null descriptors", E_OUTOFMEMORY);
        // The bindless range below the ring; without it native passes lose only bindless tables.
        if (bindless.Init(this, gpuRing.Base()))
            caps.bindlessCapacity = bindless.Capacity();
        NativeInit();
        if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
            adapter3 = nullptr;
        SetStage(this, StageInitUploader);
        if (!init.Init(this, queue, &initFence, D3D12_COMMAND_LIST_TYPE_DIRECT))
            return fail("initial data uploader", E_OUTOFMEMORY);
        if (info.copyQueueUploads)
            CopyQueueInit();
        else
            Log("* native: creation-time uploads on the direct queue (-dx12_no_copy_queue)");
        SetStage(this, StageRootSignatures);
        if (!rootSignatures.Init(d12))
            return fail("root signatures", E_FAIL);
        SetStage(this, StagePipelines);
        if (!pipelines.Init(this, info.pipelineCachePath))
            return fail("pipeline cache", E_FAIL);
        SetStage(this, StageQueryHeaps);
        if (!queryHeaps.Init(d12))
            return fail("query heaps", E_OUTOFMEMORY);
        if (FAILED(queue->GetTimestampFrequency(&timestampFrequency)))
            timestampFrequency = 0;
        AsyncInit();
        VrsInit(); // [dx12:vrs] hook
        LogNativeCaps(caps, disabledFeatures, gpuRing.Capacity() - gpuRing.Base());

        SetStage(this, StageImmediate);
        immediate = new Context(this, false, kImmediateContextIndex);

        SetStage(this, StageReady);
        return S_OK;
    });
}

void Device::Shutdown()
{
    u32 stage = StageNone;
    {
        std::lock_guard lock(g_devicesLock);
        for (auto it = g_devices.begin(); it != g_devices.end(); ++it)
        {
            if (it->device == this)
            {
                stage = it->stage;
                g_devices.erase(it);
                break;
            }
        }
    }

    // Past StageFence the fence initialised and the releaser is bound to it.
    if (stage > StageFence && queue)
        WaitIdle();
    if (immediate)
    {
        immediate->ClearState();
        immediate->Release();
        immediate = nullptr;
    }
    // Jobs let go of the resources they hold before the releaser drains.
    AsyncShutdown();
    if (stage >= StagePipelines)
        pipelines.Shutdown();

    // Moved out first: an object's teardown must be free to use the device without the lock.
    std::unordered_map<u64, BlendState*> blends;
    std::unordered_map<u64, DepthStencilState*> depths;
    std::unordered_map<u64, RasterizerState*> rasters;
    std::unordered_map<u64, SamplerState*> samplers;
    {
        std::lock_guard lock(stateLock);
        blends.swap(blendStates);
        depths.swap(depthStates);
        rasters.swap(rasterStates);
        samplers.swap(samplerStates);
    }
    ReleaseAll(blends);
    ReleaseAll(depths);
    ReleaseAll(rasters);
    ReleaseAll(samplers);

    if (stage > StageFence)
        releaser.Collect(true);
    VrsShutdown();     // [dx12:vrs] hook
    MeshShutdown();    // [dx12:mesh] hook
    TexheapShutdown(); // [dx12:ds] hook
    NativeShutdown();
    if (stage >= StageQueryHeaps)
        queryHeaps.Shutdown();
    if (stage >= StageRootSignatures)
        rootSignatures.Shutdown();
    if (stage >= StageInitUploader)
    {
        CopyQueueShutdown();
        init.Shutdown();
    }
    if (stage >= StageDynamicFirstVersions)
        dynamicFirstVersions.Shutdown();
    if (stage >= StageBufferPages)
        bufferPages.Shutdown();
    if (stage >= StageUploadPages)
        uploadPages.Shutdown();
    if (stage >= StageSamplerTables)
        samplerTables.Shutdown();
    if (stage >= StageGpuRing)
        gpuRing.Shutdown();
    if (stage >= StageCpuSampler)
        cpuSampler.Shutdown();
    if (stage >= StageCpuDsv)
        cpuDsv.Shutdown();
    if (stage >= StageCpuRtv)
        cpuRtv.Shutdown();
    if (stage >= StageCpuSrv)
        cpuSrv.Shutdown();
    if (stage >= StageFence)
    {
        initFence.Shutdown();
        fence.Shutdown();
    }

    if (queue)
    {
        queue->Release();
        queue = nullptr;
    }
    DrainDebugMessages();
    if (m_infoQueue)
    {
        m_infoQueue->Release();
        m_infoQueue = nullptr;
    }
    if (d12_1)
    {
        d12_1->Release();
        d12_1 = nullptr;
    }
    if (d12_2)
    {
        d12_2->Release();
        d12_2 = nullptr;
    }
    if (d12_5)
    {
        d12_5->Release();
        d12_5 = nullptr;
    }
    if (d12)
    {
        d12->Release();
        d12 = nullptr;
    }
    if (adapter)
    {
        adapter->Release();
        adapter = nullptr;
    }
    delete m_dxgiDevice;
    m_dxgiDevice = nullptr;
}

// ============================================================================================
// Frames
// ============================================================================================

void Device::EndFrame(u64 lastFenceValue)
{
    const u64 current = frame.load(std::memory_order_relaxed);
    gpuRing.EndFrame(current, lastFenceValue);
    uploadPages.EndFrame(current, lastFenceValue);
    frameFences[current % kFramesInFlight] = lastFenceValue;
    const u64 next = current + 1;
    frame.store(next, std::memory_order_release);
    // The slot of the new frame still holds the fence of frame (next - kFramesInFlight): once it
    // passed, allocators and memory tagged kFramesInFlight + 1 frames ago are idle.
    if (next > kFramesInFlight)
        fence.Wait(frameFences[next % kFramesInFlight]);
    AsyncEndFrame(current, next);
    uploadPages.Recycle();
    samplerTables.EndFrame(next);
    releaser.Collect(false);
    bindless.EndFrame(current, lastFenceValue);
    if (m_infoQueue)
        DrainDebugMessages();
    if (stats && next % kStatsFrames == 0)
    {
        u64 s[StatCount];
        for (u32 i = 0; i < StatCount; ++i)
            s[i] = statCounters[i].exchange(0, std::memory_order_relaxed) / (i < StatNativeSections ? kStatsFrames : 1);
        Log("* stats per frame: %llu draws, %llu PSO sets, %llu root CBVs, %llu SRV tables (%llu descriptors), "
            "%llu CBV tables, %llu sampler tables, %llu maps, %llu barriers, %llu submissions",
            s[StatDraws], s[StatPso], s[StatRootCbv], s[StatSrvTables], s[StatSrvDescriptors], s[StatCbvTables],
            s[StatSamplerTables], s[StatMaps], s[StatBarriers], s[StatSubmits]);
        // Counts of the feature work, only when something used it: sums over the interval (rare events).
        u64 features = 0;
        for (u32 i = StatNativeSections; i < StatCount; ++i)
            features += s[i];
        if (features)
            Log("* stats per %u frames: %llu native sections, %llu forks, %llu joins (%llu by the guard), %llu mixed PSOs, "
                "%llu indirect draws, %llu shading rate sets, %llu mesh dispatches",
                kStatsFrames, s[StatNativeSections], s[StatForks], s[StatJoins], s[StatAutoJoins], s[StatMixedPso], s[StatIndirect],
                s[StatShadingRate], s[StatMeshDispatches]);
        // Creation-time uploads per queue, only when something was uploaded.
        u64 directBytes = 0, directSubmissions = 0, copyBytes = 0, copySubmissions = 0;
        init.TakeStats(directBytes, directSubmissions);
        if (copyQueue)
            initCopy.TakeStats(copyBytes, copySubmissions);
        if (directSubmissions || copySubmissions)
            Log("* stats per %u frames: creation-time uploads %llu MB in %llu submissions on the direct queue, %llu MB in %llu "
                "on the copy queue",
                kStatsFrames, static_cast<unsigned long long>(directBytes >> 20), static_cast<unsigned long long>(directSubmissions),
                static_cast<unsigned long long>(copyBytes >> 20), static_cast<unsigned long long>(copySubmissions));
    }
}

void Device::ForceFrameBoundary()
{
    AsyncFrameBoundary();
    EndFrame(immediate->Submit());
}

void Device::WaitIdle()
{
    if (!queue)
        return;
    // Pending creation-time copies go first: the releaser must not free what they still read. The
    // direct queue waits for copy-queue uploads, so its signal covers them.
    FlushUploads();
    fence.Wait(fence.Signal(queue));
    AsyncWaitIdle();
}

void Device::FlushUploads()
{
    if (copyQueue)
        initCopy.Flush();
    init.Flush();
}

bool Device::UploadsPending()
{
    return (copyQueue && initCopy.Pending()) || init.Pending();
}

void Device::CopyQueueInit()
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ID3D12CommandQueue* created = nullptr;
    HRESULT hr = d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&created));
    if (SUCCEEDED(hr) && !copyFence.Init(d12))
        hr = E_FAIL;
    if (SUCCEEDED(hr))
    {
        created->SetName(L"xrD3D12 copy");
        if (!initCopy.Init(this, created, &copyFence, D3D12_COMMAND_LIST_TYPE_COPY))
            hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr))
    {
        initCopy.Shutdown();
        copyFence.Shutdown();
        if (created)
            created->Release();
        Log("* native: copy queue failed (0x%08X), creation-time uploads stay on the direct queue", unsigned(hr));
        return;
    }
    copyQueue = created;
    Log("* native: creation-time uploads on a copy queue");
}

// Shutdown, after WaitIdle: the direct queue waited for every copy-queue upload.
void Device::CopyQueueShutdown()
{
    if (!copyQueue)
        return;
    initCopy.Shutdown();
    copyFence.Shutdown();
    copyQueue->Release();
    copyQueue = nullptr;
}

// ============================================================================================
// Public API (d12_api.h)
// ============================================================================================

HRESULT CreateDevice(const DeviceCreateInfo& info, ID3D11Device** device, ID3D11DeviceContext** immediate,
    D3D_FEATURE_LEVEL* featureLevel)
{
    if (device)
        *device = nullptr;
    if (immediate)
        *immediate = nullptr;
    if (!device)
        return E_INVALIDARG;
    if (info.log)
        SetLogSink(info.log);

    Device* created = nullptr;
    try
    {
        created = new Device();
    }
    catch (...)
    {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = created->Init(info);
    if (FAILED(hr))
    {
        created->Release();
        return hr;
    }
    if (featureLevel)
        *featureLevel = created->GetFeatureLevel();
    if (immediate)
        created->GetImmediateContext(immediate);
    *device = created;
    return S_OK;
}

bool ProbeAdapter(IDXGIAdapter1* adapter, char* reason, size_t reasonSize)
{
    return ProbeAdapterCaps(adapter, reason, reasonSize, nullptr);
}

bool ProbeAdapterCaps(IDXGIAdapter1* adapter, char* reason, size_t reasonSize, NativeCaps* caps)
{
    if (caps)
        *caps = NativeCaps{};
    Reason(reason, reasonSize, "%s", "");
    if (!adapter)
    {
        Reason(reason, reasonSize, "no adapter");
        return false;
    }
    if (!g_d3d12.Load())
    {
        Reason(reason, reasonSize, "d3d12.dll is not available");
        return false;
    }
    ID3D12Device* device = nullptr;
    const HRESULT hr = g_d3d12.CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device)
    {
        Reason(reason, reasonSize, "no Direct3D 12 device with feature level 11_0 (0x%08X)", unsigned(hr));
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS probe{};
    const HRESULT oh = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &probe, sizeof(probe));
    if (caps && SUCCEEDED(oh))
        QueryNativeCaps(device, adapter, *caps);
    device->Release();
    if (FAILED(oh))
    {
        Reason(reason, reasonSize, "the device options cannot be queried (0x%08X)", unsigned(oh));
        return false;
    }
    if (probe.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
    {
        Reason(reason, reasonSize, "resource binding tier 1 (tier 2 is required)");
        return false;
    }
    return true;
}

bool IsLayerDevice(ID3D11Device* device)
{
    if (!device)
        return false;
    std::lock_guard lock(g_devicesLock);
    for (const DeviceRecord& record : g_devices)
    {
        if (static_cast<ID3D11Device*>(record.device) == device)
            return record.stage == StageReady;
    }
    return false;
}

void SavePipelineCache(ID3D11Device* device)
{
    if (IsLayerDevice(device))
        Device::From(device)->pipelines.Save();
}

ID3D12Device* NativeDevice(ID3D11Device* device)
{
    return IsLayerDevice(device) ? Device::From(device)->d12 : nullptr;
}

ID3D12CommandQueue* NativeQueue(ID3D11Device* device)
{
    return IsLayerDevice(device) ? Device::From(device)->queue : nullptr;
}

void Flush(ID3D11DeviceContext* immediate, bool wait)
{
    if (!immediate)
        return;
    Context* context = static_cast<Context*>(immediate);
    if (context->IsDeferred())
        return;
    try
    {
        const u64 value = context->Submit();
        if (wait)
            context->device()->fence.Wait(value);
    }
    catch (...)
    {
        Log("! Flush failed");
    }
}
} // namespace xrd3d12
