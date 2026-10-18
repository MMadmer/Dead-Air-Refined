// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Device: ID3D11Device over a Vulkan 1.3 device - instance and device creation, queues, frames, state
// object caches, shaders, input layout validation, capabilities and the creation part of vk_api.h.
#include "vk_layer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace xrvk
{
namespace
{
constexpr u32 kImmediateContextIndex = 255;

// ---- device registry ------------------------------------------------------------------------

struct DeviceRecord
{
    Device* device;
    bool ready;
    u32 nextDeferred;
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
    LogOnce(what, "! %s is not supported by the Vulkan renderer", what);
}

void Reason(char* out, size_t size, const char* fmt, ...)
{
    if (!out || !size)
        return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out, size, fmt, args);
    va_end(args);
}

// ---- DXBC containers (the DXIL twin and signature blobs) ----------------------------------------

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

const u8* FindChunk(const void* blob, size_t size, u32 fourcc, u32& chunkSize)
{
    const u8* bytes = static_cast<const u8*>(blob);
    if (!bytes || size < 32 || std::memcmp(bytes, "DXBC", 4) != 0)
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

bool ReadInputSignature(const void* blob, size_t size, SignatureInput* out, u32& count)
{
    count = 0;
    u32 length = 0;
    u32 stride = 24;
    u32 field = 0;
    const u8* chunk = FindChunk(blob, size, FourCC('I', 'S', 'G', 'N'), length);
    if (!chunk)
    {
        // ISG1 (SM 5.1+, DXIL) prefixes every element with its stream and appends the min precision.
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

// ---- state object keys and conversions --------------------------------------------------------
// Keys field by field: the descs have padding, and GetDesc must return exactly what the engine passed.

constexpr u64 kSeedBlend = 0x9e3779b97f4a7c15ull;
constexpr u64 kSeedDepth = 0xc2b2ae3d27d4eb4full;
constexpr u64 kSeedRaster = 0x165667b19e3779f9ull;
constexpr u64 kSeedSampler = 0x27d4eb2f165667c5ull;
constexpr u64 kSeedInputLayout = 0x85ebca77c2b2ae63ull;

u32 Bits(float value)
{
    return std::bit_cast<u32>(value);
}

std::array<u32, 2 + 8 * 8> KeyOf(const D3D11_BLEND_DESC& d)
{
    std::array<u32, 2 + 8 * 8> k{};
    k[0] = d.AlphaToCoverageEnable;
    k[1] = d.IndependentBlendEnable;
    for (u32 i = 0; i < 8; ++i)
    {
        const D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[i];
        u32* p = &k[2 + i * 8];
        p[0] = r.BlendEnable;
        p[1] = r.SrcBlend;
        p[2] = r.DestBlend;
        p[3] = r.BlendOp;
        p[4] = r.SrcBlendAlpha;
        p[5] = r.DestBlendAlpha;
        p[6] = r.BlendOpAlpha;
        p[7] = r.RenderTargetWriteMask;
    }
    return k;
}

std::array<u32, 14> KeyOf(const D3D11_DEPTH_STENCIL_DESC& d)
{
    return {u32(d.DepthEnable), u32(d.DepthWriteMask), u32(d.DepthFunc), u32(d.StencilEnable), d.StencilReadMask,
        d.StencilWriteMask, u32(d.FrontFace.StencilFailOp), u32(d.FrontFace.StencilDepthFailOp),
        u32(d.FrontFace.StencilPassOp), u32(d.FrontFace.StencilFunc), u32(d.BackFace.StencilFailOp),
        u32(d.BackFace.StencilDepthFailOp), u32(d.BackFace.StencilPassOp), u32(d.BackFace.StencilFunc)};
}

std::array<u32, 10> KeyOf(const D3D11_RASTERIZER_DESC& d)
{
    return {u32(d.FillMode), u32(d.CullMode), u32(d.FrontCounterClockwise), u32(d.DepthBias), Bits(d.DepthBiasClamp),
        Bits(d.SlopeScaledDepthBias), u32(d.DepthClipEnable), u32(d.ScissorEnable), u32(d.MultisampleEnable),
        u32(d.AntialiasedLineEnable)};
}

std::array<u32, 13> KeyOf(const D3D11_SAMPLER_DESC& d)
{
    return {u32(d.Filter), u32(d.AddressU), u32(d.AddressV), u32(d.AddressW), Bits(d.MipLODBias), d.MaxAnisotropy,
        u32(d.ComparisonFunc), Bits(d.BorderColor[0]), Bits(d.BorderColor[1]), Bits(d.BorderColor[2]),
        Bits(d.BorderColor[3]), Bits(d.MinLOD), Bits(d.MaxLOD)};
}

VkBlendFactor BlendFactor(D3D11_BLEND blend, bool alpha)
{
    switch (blend)
    {
    case D3D11_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3D11_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3D11_BLEND_SRC_COLOR: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case D3D11_BLEND_INV_SRC_COLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D11_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D11_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D11_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case D3D11_BLEND_INV_DEST_COLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D11_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D11_BLEND_BLEND_FACTOR: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D11_BLEND_INV_BLEND_FACTOR:
        return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D11_BLEND_SRC1_COLOR: return alpha ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D11_BLEND_INV_SRC1_COLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D11_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(D3D11_BLEND_OP op)
{
    switch (op)
    {
    case D3D11_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3D11_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3D11_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
    case D3D11_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp CompareOp(D3D11_COMPARISON_FUNC func)
{
    switch (func)
    {
    case D3D11_COMPARISON_NEVER: return VK_COMPARE_OP_NEVER;
    case D3D11_COMPARISON_LESS: return VK_COMPARE_OP_LESS;
    case D3D11_COMPARISON_EQUAL: return VK_COMPARE_OP_EQUAL;
    case D3D11_COMPARISON_LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case D3D11_COMPARISON_GREATER: return VK_COMPARE_OP_GREATER;
    case D3D11_COMPARISON_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case D3D11_COMPARISON_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp StencilOp(D3D11_STENCIL_OP op)
{
    switch (op)
    {
    case D3D11_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
    case D3D11_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case D3D11_STENCIL_OP_INCR_SAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_DECR_SAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
    case D3D11_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case D3D11_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode AddressMode(D3D11_TEXTURE_ADDRESS_MODE mode)
{
    switch (mode)
    {
    case D3D11_TEXTURE_ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3D11_TEXTURE_ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case D3D11_TEXTURE_ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

template <class State, class Key, class Create, class Iface>
HRESULT CachedState(std::mutex& lock, std::unordered_map<u64, State*>& cache, const Key& key, u64 seed, Create&& create,
    Iface** out)
{
    const u64 hash = Hash64(key.data(), key.size() * sizeof(key[0]), seed);
    return Guarded([&]() -> HRESULT {
        std::lock_guard guard(lock);
        if (const auto it = cache.find(hash); it != cache.end())
        {
            if (out)
            {
                it->second->AddRef();
                *out = it->second;
                return S_OK;
            }
            return S_FALSE;
        }
        State* object = nullptr;
        const HRESULT hr = create(object);
        if (FAILED(hr) || !object)
            return FAILED(hr) ? hr : E_FAIL;
        // The cache keeps one reference: identical descriptions return the same object (D3D11 does too).
        cache.emplace(hash, object);
        if (!out)
            return S_FALSE;
        object->AddRef();
        *out = object;
        return S_OK;
    });
}

template <class State>
void ReleaseAll(std::unordered_map<u64, State*>& cache)
{
    for (auto& [hash, state] : cache)
        state->Release();
    cache.clear();
}

// ---- extensions and features ------------------------------------------------------------------

struct ExtensionList
{
    std::vector<VkExtensionProperties> list;
    bool Has(const char* name) const
    {
        for (const VkExtensionProperties& e : list)
        {
            if (!std::strcmp(e.extensionName, name))
                return true;
        }
        return false;
    }
};

// The VkBool32 members of the 1.2 / 1.3 feature structures by name: what an SDK asks for (ExternalRequirements).
struct FeatureField
{
    const char* name;
    size_t offset;
};
#define XRVK_F12(n) {#n, offsetof(VkPhysicalDeviceVulkan12Features, n)}
const FeatureField kFeatures12[] = {XRVK_F12(samplerMirrorClampToEdge), XRVK_F12(drawIndirectCount),
    XRVK_F12(storageBuffer8BitAccess), XRVK_F12(uniformAndStorageBuffer8BitAccess), XRVK_F12(storagePushConstant8),
    XRVK_F12(shaderBufferInt64Atomics), XRVK_F12(shaderSharedInt64Atomics), XRVK_F12(shaderFloat16), XRVK_F12(shaderInt8),
    XRVK_F12(descriptorIndexing), XRVK_F12(shaderInputAttachmentArrayDynamicIndexing),
    XRVK_F12(shaderUniformTexelBufferArrayDynamicIndexing), XRVK_F12(shaderStorageTexelBufferArrayDynamicIndexing),
    XRVK_F12(shaderUniformBufferArrayNonUniformIndexing), XRVK_F12(shaderSampledImageArrayNonUniformIndexing),
    XRVK_F12(shaderStorageBufferArrayNonUniformIndexing), XRVK_F12(shaderStorageImageArrayNonUniformIndexing),
    XRVK_F12(shaderInputAttachmentArrayNonUniformIndexing), XRVK_F12(shaderUniformTexelBufferArrayNonUniformIndexing),
    XRVK_F12(shaderStorageTexelBufferArrayNonUniformIndexing), XRVK_F12(descriptorBindingUniformBufferUpdateAfterBind),
    XRVK_F12(descriptorBindingSampledImageUpdateAfterBind), XRVK_F12(descriptorBindingStorageImageUpdateAfterBind),
    XRVK_F12(descriptorBindingStorageBufferUpdateAfterBind), XRVK_F12(descriptorBindingUniformTexelBufferUpdateAfterBind),
    XRVK_F12(descriptorBindingStorageTexelBufferUpdateAfterBind), XRVK_F12(descriptorBindingUpdateUnusedWhilePending),
    XRVK_F12(descriptorBindingPartiallyBound), XRVK_F12(descriptorBindingVariableDescriptorCount),
    XRVK_F12(runtimeDescriptorArray), XRVK_F12(samplerFilterMinmax), XRVK_F12(scalarBlockLayout),
    XRVK_F12(imagelessFramebuffer), XRVK_F12(uniformBufferStandardLayout), XRVK_F12(shaderSubgroupExtendedTypes),
    XRVK_F12(separateDepthStencilLayouts), XRVK_F12(hostQueryReset), XRVK_F12(timelineSemaphore),
    XRVK_F12(bufferDeviceAddress), XRVK_F12(bufferDeviceAddressCaptureReplay), XRVK_F12(bufferDeviceAddressMultiDevice),
    XRVK_F12(vulkanMemoryModel), XRVK_F12(vulkanMemoryModelDeviceScope),
    XRVK_F12(vulkanMemoryModelAvailabilityVisibilityChains), XRVK_F12(shaderOutputViewportIndex),
    XRVK_F12(shaderOutputLayer), XRVK_F12(subgroupBroadcastDynamicId)};
#undef XRVK_F12
#define XRVK_F13(n) {#n, offsetof(VkPhysicalDeviceVulkan13Features, n)}
const FeatureField kFeatures13[] = {XRVK_F13(robustImageAccess), XRVK_F13(inlineUniformBlock),
    XRVK_F13(descriptorBindingInlineUniformBlockUpdateAfterBind), XRVK_F13(pipelineCreationCacheControl),
    XRVK_F13(privateData), XRVK_F13(shaderDemoteToHelperInvocation), XRVK_F13(shaderTerminateInvocation),
    XRVK_F13(subgroupSizeControl), XRVK_F13(computeFullSubgroups), XRVK_F13(synchronization2),
    XRVK_F13(textureCompressionASTC_HDR), XRVK_F13(shaderZeroInitializeWorkgroupMemory), XRVK_F13(dynamicRendering),
    XRVK_F13(shaderIntegerDotProduct), XRVK_F13(maintenance4)};
#undef XRVK_F13

// Turns the named member on in `enabled` where `supported` has it; false when the device lacks it.
template <typename T, size_t N>
bool EnableFeature(const FeatureField (&table)[N], const char* name, const T& supported, T& enabled)
{
    for (const FeatureField& f : table)
    {
        if (std::strcmp(f.name, name))
            continue;
        const VkBool32 have = *reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&supported) + f.offset);
        if (have)
            *reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&enabled) + f.offset) = VK_TRUE;
        return have != VK_FALSE;
    }
    return false;
}

bool Listed(const std::vector<const char*>& names, const char* name)
{
    return std::any_of(names.begin(), names.end(), [name](const char* n) { return !std::strcmp(n, name); });
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (!data || !data->pMessage)
        return VK_FALSE;
    // Known noise of the loader and of layers the user installed (overlays).
    if (data->pMessageIdName && std::strstr(data->pMessageIdName, "Loader"))
        return VK_FALSE;
    // A message that repeats every draw is logged three times.
    static std::mutex lock;
    static std::unordered_map<int32_t, u32> seen;
    u32 count;
    {
        std::lock_guard guard(lock);
        count = ++seen[data->messageIdNumber];
    }
    if (count > 3)
        return VK_FALSE;
    const char marker = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? '!' : '~';
    Log("%c [validation] %s", marker, data->pMessage);
    return VK_FALSE;
}

std::string VersionText(u32 version)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
        VK_API_VERSION_PATCH(version));
    return text;
}

// The physical device of the adapter the engine chose (by LUID), else the first discrete GPU.
VkPhysicalDevice PickPhysicalDevice(VkInstance instance, const LUID& luid)
{
    u32 count = 0;
    g_vk.vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count)
        g_vk.vkEnumeratePhysicalDevices(instance, &count, devices.data());
    const bool wantLuid = luid.LowPart || luid.HighPart;
    VkPhysicalDevice discrete = VK_NULL_HANDLE, any = VK_NULL_HANDLE;
    for (VkPhysicalDevice physical : devices)
    {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        g_vk.vkGetPhysicalDeviceProperties2(physical, &props);
        if (props.properties.apiVersion < VK_API_VERSION_1_3)
            continue;
        if (wantLuid && id.deviceLUIDValid && !std::memcmp(id.deviceLUID, &luid, sizeof(LUID)))
            return physical;
        if (!any)
            any = physical;
        if (!discrete && props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            discrete = physical;
    }
    return discrete ? discrete : any;
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
        if (count && (!resources || !status))
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
    if ((riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIObject)) && m_dxgiDevice)
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

namespace
{
template <class Object, class Desc, class Iface>
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

template <class Object, class Desc, class Iface>
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
    ShaderContainerView view;
    if (!ParseShaderContainer(bytecode, size, view))
    {
        static constexpr char kFormat[] = "! a shader without SPIR-V was created (FXC bytecode reaches the Vulkan "
                                          "renderer only by mistake): refused";
        LogOnce(kFormat, kFormat);
        return E_INVALIDARG;
    }
    return Guarded([&]() -> HRESULT {
        Object* shader = new Object(device);
        try
        {
            if (view.dxil)
            {
                const u8* bytes = static_cast<const u8*>(view.dxil);
                shader->dxil.assign(bytes, bytes + view.dxilSize);
            }
            std::string error;
            if (!PrepareSpirv(view.spirv, view.spirvWords, stage, view.bindings, view.bindingCount, shader->spirv,
                    shader->iface, shader->entry, shader->mesh, error))
            {
                Log("! a shader was refused: %s", error.c_str());
                shader->Release();
                return E_INVALIDARG;
            }
            if (shader->mesh && !device->caps.meshShaders)
            {
                shader->Release();
                return E_INVALIDARG;
            }
            // The RT lane's shaders need the bindless set (a device with ray queries).
            if (shader->iface.bindless && !device->bindless.layout)
            {
                static constexpr char kFormat[] = "! a shader reads the bindless set the device lacks: refused";
                LogOnce(&kFormat, kFormat);
                shader->Release();
                return E_INVALIDARG;
            }
            const std::string missing = MissingCapabilities(device, shader->spirv);
            if (!missing.empty())
            {
                static constexpr char kFormat[] = "! a shader needs capabilities the device lacks (%s): refused";
                LogOnce(&kFormat, kFormat, missing.c_str());
                shader->Release();
                return E_INVALIDARG;
            }
            shader->hash = Hash64(shader->spirv.data(), shader->spirv.size() * 4, 0x5badu + u64(stage));
            shader->setLayout = device->layouts.Set(stage, shader->iface.bindings);
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = shader->spirv.size() * 4;
            info.pCode = shader->spirv.data();
            const VkResult result = g_vk.vkCreateShaderModule(device->vk, &info, nullptr, &shader->module);
            if (!shader->setLayout || result != VK_SUCCESS)
            {
                Log("! vkCreateShaderModule failed: %s", ResultText(result));
                shader->Release();
                return E_INVALIDARG;
            }
            if (stage == Stage::CS)
                shader->computeLayout =
                    device->layouts.Compute(shader->setLayout, shader->iface.bindless, shader->iface.pushConstants);
        }
        catch (...)
        {
            shader->Release();
            throw;
        }
        return HandOut(shader, out);
    });
}
} // namespace

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
    Unsupported("predication");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Device::CreateCounter(const D3D11_COUNTER_DESC*, ID3D11Counter** counter)
{
    if (counter)
        *counter = nullptr;
    Unsupported("counters");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Device::OpenSharedResource(HANDLE, REFIID, void** object)
{
    if (object)
        *object = nullptr;
    Unsupported("shared resources");
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
    if (!count || !elements || count > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT || !bytecode || !size)
        return E_INVALIDARG;

    // The engine passes the DXIL twin's signature (D3DGetInputSignatureBlob) or the container itself.
    const void* signature = bytecode;
    size_t signatureSize = size;
    ShaderContainerView view;
    if (ParseShaderContainer(bytecode, size, view) && view.dxil)
    {
        signature = view.dxil;
        signatureSize = view.dxilSize;
    }
    SignatureInput inputs[kMaxSignatureInputs];
    u32 inputCount = 0;
    if (!ReadInputSignature(signature, signatureSize, inputs, inputCount))
    {
        static constexpr char kFormat[] = "! CreateInputLayout: the shader blob has no readable input signature";
        LogOnce(kFormat, kFormat);
        return E_INVALIDARG;
    }
    for (UINT i = 0; i < count; ++i)
    {
        const D3D11_INPUT_ELEMENT_DESC& e = elements[i];
        if (!e.SemanticName || !*e.SemanticName || e.InputSlot >= kMaxVBs)
            return E_INVALIDARG;
        if (e.InputSlotClass == D3D11_INPUT_PER_VERTEX_DATA)
        {
            if (e.InstanceDataStepRate)
                return E_INVALIDARG;
        }
        else if (e.InputSlotClass != D3D11_INPUT_PER_INSTANCE_DATA)
            return E_INVALIDARG;
        const FormatInfo& fmt = Format(e.Format);
        if (fmt.format == VK_FORMAT_UNDEFINED || fmt.compressed || fmt.depth)
            return E_INVALIDARG;
        VkFormatProperties props{};
        g_vk.vkGetPhysicalDeviceFormatProperties(physical, fmt.format, &props);
        if (!(props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
            return E_INVALIDARG;
        for (UINT j = 0; j < i; ++j)
        {
            if (elements[j].SemanticIndex == e.SemanticIndex &&
                SemanticEquals(elements[j].SemanticName, e.SemanticName, u32(std::strlen(e.SemanticName))))
                return E_INVALIDARG;
        }
    }
    for (u32 i = 0; i < inputCount; ++i)
    {
        if (inputs[i].systemValue != D3D_NAME_UNDEFINED)
            continue;
        bool found = false;
        for (UINT j = 0; j < count && !found; ++j)
            found = elements[j].SemanticIndex == inputs[i].index &&
                SemanticEquals(elements[j].SemanticName, inputs[i].name, inputs[i].nameLength);
        if (!found)
            return E_INVALIDARG;
    }
    if (!layout)
        return S_FALSE;

    return Guarded([&]() -> HRESULT {
        InputLayout* object = new InputLayout(this);
        try
        {
            u32 slotOffset[kMaxVBs]{};
            object->elements.resize(count);
            u64 hash = Hash64(&count, sizeof(count), kSeedInputLayout);
            for (UINT i = 0; i < count; ++i)
            {
                const D3D11_INPUT_ELEMENT_DESC& s = elements[i];
                InputLayout::Element& t = object->elements[i];
                t.semantic = s.SemanticName;
                for (char& c : t.semantic)
                    c = Upper(c);
                t.index = s.SemanticIndex;
                t.format = Format(s.Format).format;
                t.slot = s.InputSlot;
                const u32 bytes = Format(s.Format).blockBytes;
                if (s.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT)
                {
                    const u32 align = (std::min)(4u, bytes ? bytes : 4u);
                    t.offset = (slotOffset[t.slot] + align - 1) / align * align;
                }
                else
                    t.offset = s.AlignedByteOffset;
                slotOffset[t.slot] = t.offset + bytes;
                t.instance = s.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA;
                t.stepRate = s.InstanceDataStepRate;
                object->slotMask |= 1u << t.slot;
                object->slotInstance[t.slot] = t.instance;
                object->slotStepRate[t.slot] = t.instance ? t.stepRate : 1;
                hash = Hash64(t.semantic.data(), t.semantic.size(), hash);
                const u32 fields[6] = {t.index, u32(t.format), t.slot, t.offset, u32(t.instance), t.stepRate};
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
            object->alphaToCoverage = desc->AlphaToCoverageEnable != FALSE;
            for (u32 i = 0; i < kMaxRTVs; ++i)
            {
                const D3D11_RENDER_TARGET_BLEND_DESC& r = desc->RenderTarget[desc->IndependentBlendEnable ? i : 0];
                VkPipelineColorBlendAttachmentState& a = object->attachments[i];
                a.blendEnable = r.BlendEnable ? VK_TRUE : VK_FALSE;
                a.srcColorBlendFactor = BlendFactor(r.SrcBlend, false);
                a.dstColorBlendFactor = BlendFactor(r.DestBlend, false);
                a.colorBlendOp = BlendOp(r.BlendOp);
                a.srcAlphaBlendFactor = BlendFactor(r.SrcBlendAlpha, true);
                a.dstAlphaBlendFactor = BlendFactor(r.DestBlendAlpha, true);
                a.alphaBlendOp = BlendOp(r.BlendOpAlpha);
                a.colorWriteMask = r.RenderTargetWriteMask & 0xF;
                if (!a.blendEnable)
                {
                    // Factors of a disabled attachment do not matter: one key for all of them.
                    a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                    a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
                }
            }
            object->hash = Hash64(object->attachments, sizeof(object->attachments), kSeedBlend + object->alphaToCoverage);
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
            object->depthTest = desc->DepthEnable != FALSE;
            object->depthWrite = object->depthTest && desc->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
            object->depthCompare = object->depthTest ? CompareOp(desc->DepthFunc) : VK_COMPARE_OP_ALWAYS;
            object->stencilTest = desc->StencilEnable != FALSE;
            auto face = [&](const D3D11_DEPTH_STENCILOP_DESC& s, VkStencilOpState& t) {
                t.failOp = StencilOp(s.StencilFailOp);
                t.passOp = StencilOp(s.StencilPassOp);
                t.depthFailOp = StencilOp(s.StencilDepthFailOp);
                t.compareOp = CompareOp(s.StencilFunc);
                t.compareMask = desc->StencilReadMask;
                t.writeMask = desc->StencilWriteMask;
                t.reference = 0;
            };
            face(desc->FrontFace, object->front);
            face(desc->BackFace, object->back);
            const auto key = KeyOf(*desc);
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
            object->polygon = desc->FillMode == D3D11_FILL_WIREFRAME ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
            object->cull = desc->CullMode == D3D11_CULL_NONE ? VK_CULL_MODE_NONE :
                desc->CullMode == D3D11_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
            // With the negative-height viewport, D3D's winding maps 1:1.
            object->front = desc->FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
            object->depthBias = desc->DepthBias != 0 || desc->SlopeScaledDepthBias != 0.f;
            object->depthClamp = !desc->DepthClipEnable;
            object->scissor = desc->ScissorEnable != FALSE;
            // Pipeline-relevant fields only (cull, front face, bias, scissor are dynamic).
            const u32 key[2] = {u32(object->polygon), u32(object->depthClamp)};
            object->hash = Hash64(key, sizeof(key), kSeedRaster);
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
            const u32 filter = u32(desc->Filter);
            const bool anisotropic = (filter & 0x40) != 0;
            const bool comparison = (filter & 0x180) == 0x80;
            const u32 reduction = (filter >> 7) & 3; // 0 standard, 1 comparison, 2 minimum, 3 maximum
            VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            info.magFilter = anisotropic || (filter & 0x04) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            info.minFilter = anisotropic || (filter & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            info.mipmapMode = anisotropic || (filter & 0x01) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = AddressMode(desc->AddressU);
            info.addressModeV = AddressMode(desc->AddressV);
            info.addressModeW = AddressMode(desc->AddressW);
            info.mipLodBias = std::clamp(desc->MipLODBias, -limits.maxSamplerLodBias, limits.maxSamplerLodBias);
            info.anisotropyEnable = anisotropic && desc->MaxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
            info.maxAnisotropy = std::clamp(float(desc->MaxAnisotropy), 1.f, limits.maxSamplerAnisotropy);
            info.compareEnable = comparison ? VK_TRUE : VK_FALSE;
            info.compareOp = CompareOp(desc->ComparisonFunc);
            info.minLod = desc->MinLOD;
            info.maxLod = desc->MaxLOD >= 1000.f ? VK_LOD_CLAMP_NONE : desc->MaxLOD;
            // Standard border colours where they match; a custom one only for others.
            const float* c = desc->BorderColor;
            VkSamplerCustomBorderColorCreateInfoEXT custom{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
            if (c[0] == 0.f && c[1] == 0.f && c[2] == 0.f)
                info.borderColor = c[3] == 0.f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            else if (c[0] == 1.f && c[1] == 1.f && c[2] == 1.f && c[3] == 1.f)
                info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            else if (customBorderColor)
            {
                info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
                std::memcpy(custom.customBorderColor.float32, c, sizeof(float) * 4);
                custom.format = VK_FORMAT_UNDEFINED;
                info.pNext = &custom;
            }
            else
                info.borderColor = c[3] >= 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            VkSamplerReductionModeCreateInfo reductionInfo{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
            if (reduction >= 2)
            {
                reductionInfo.reductionMode = reduction == 2 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
                reductionInfo.pNext = info.pNext;
                info.pNext = &reductionInfo;
            }
            VkSampler sampler = VK_NULL_HANDLE;
            const VkResult result = g_vk.vkCreateSampler(vk, &info, nullptr, &sampler);
            if (result != VK_SUCCESS)
            {
                Log("! vkCreateSampler failed: %s", ResultText(result));
                return E_OUTOFMEMORY;
            }
            SamplerState* object = nullptr;
            try
            {
                object = new SamplerState(this);
            }
            catch (...)
            {
                g_vk.vkDestroySampler(vk, sampler, nullptr);
                throw;
            }
            object->desc11 = *desc;
            object->sampler = sampler;
            object->id = nextSamplerId.fetch_add(1, std::memory_order_relaxed);
            out = object;
            return S_OK;
        },
        state);
}

void SamplerState::Destroy()
{
    Device* device = m_device;
    const VkSampler handle = sampler;
    device->DescriptorTargetGone();
    device->releaser.Defer([device, handle] { g_vk.vkDestroySampler(device->vk, handle, nullptr); });
    delete this;
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
    const FormatInfo& fmt = Format(format);
    if (fmt.format == VK_FORMAT_UNDEFINED)
        return E_FAIL;
    VkFormatProperties props{};
    g_vk.vkGetPhysicalDeviceFormatProperties(physical, fmt.format, &props);
    const VkFormatFeatureFlags opt = props.optimalTilingFeatures;
    const VkFormatFeatureFlags buf = props.bufferFeatures;
    UINT s = 0;
    // A typeless depth format answers for its texture use; its typed views for sampling.
    if (opt & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
    {
        s |= D3D11_FORMAT_SUPPORT_TEXTURE1D | D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TEXTURE3D |
            D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_MIP |
            D3D11_FORMAT_SUPPORT_CPU_LOCKABLE | D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;
        if (opt & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            s |= D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_SHADER_GATHER;
        if (fmt.depth)
            s |= D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON | D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON |
                D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    }
    if (fmt.depth && fmt.format == VK_FORMAT_D32_SFLOAT_S8_UINT && format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
        format != DXGI_FORMAT_R32G8X24_TYPELESS)
        s |= D3D11_FORMAT_SUPPORT_TEXTURE2D; // D24S8 stand-in
    if (opt & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
        s |= D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
            D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    if (opt & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
        s |= D3D11_FORMAT_SUPPORT_BLENDABLE;
    if (opt & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        s |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
            D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    if ((opt & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (opt & VK_FORMAT_FEATURE_BLIT_DST_BIT) &&
        (opt & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        s |= D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
    if (opt & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
        s |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (buf & (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT))
        s |= D3D11_FORMAT_SUPPORT_BUFFER;
    if (buf & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT)
        s |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (buf & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)
        s |= D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER;
    if (format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT)
        s |= D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        s |= D3D11_FORMAT_SUPPORT_DISPLAY;
    *support = s;
    return s ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE Device::CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels)
{
    if (!levels)
        return E_INVALIDARG;
    *levels = 0;
    if (samples == 1)
    {
        *levels = 1;
        return S_OK;
    }
    if (!samples || samples > 64 || (samples & (samples - 1)))
        return S_OK;
    const FormatInfo& fmt = Format(format);
    if (fmt.format == VK_FORMAT_UNDEFINED)
        return E_INVALIDARG;
    VkImageFormatProperties props{};
    const VkImageUsageFlags usage = fmt.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (g_vk.vkGetPhysicalDeviceImageFormatProperties(physical, fmt.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            usage | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &props) != VK_SUCCESS)
        return S_OK;
    if (props.sampleCounts & samples)
        *levels = 1;
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
        static_cast<D3D11_FEATURE_DATA_DOUBLES*>(data)->DoublePrecisionFloatShaderOps = shaderFeatures.float64;
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
        const FormatInfo& fmt = Format(out->InFormat);
        if (fmt.format == VK_FORMAT_UNDEFINED)
            return E_FAIL;
        VkFormatProperties props{};
        g_vk.vkGetPhysicalDeviceFormatProperties(physical, fmt.format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)
            out->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
                D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && shaderFeatures.readWithoutFormat)
            out->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
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
        // What this layer implements: no 11.1 context API.
        auto* out = static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS*>(data);
        std::memset(out, 0, sizeof(*out));
        out->SAD4ShaderInstructions = TRUE;
        out->ExtendedDoublesShaderInstructions = shaderFeatures.float64;
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

// ============================================================================================
// Private data, loss
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
    return lost ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
}

void Device::ReportLost(VkResult result)
{
    if (result == VK_SUCCESS)
        return;
    if (result == VK_ERROR_DEVICE_LOST)
    {
        if (!lost)
            Log("! the Vulkan device was lost (GPU hang or driver reset)");
        lost = true;
        return;
    }
    LogOnce(reinterpret_cast<const void*>(uintptr_t(0x1000 + int(result))), "! a Vulkan call failed: %s", ResultText(result));
}

void Device::Name(VkObjectType type, u64 handle, const char* name)
{
    if (!debugUtils || !handle || !name || !g_vk.vkSetDebugUtilsObjectNameEXT)
        return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    g_vk.vkSetDebugUtilsObjectNameEXT(vk, &info);
}

// ============================================================================================
// Creation
// ============================================================================================

HRESULT Device::Init(const DeviceCreateInfo& info)
{
    createInfo = info;
    disabledFeatures = info.disabledFeatures;
    stats = info.stats;
    if (info.adapter)
    {
        adapter = info.adapter;
        adapter->AddRef();
    }
    {
        std::lock_guard lock(g_devicesLock);
        g_devices.push_back({this, false, 0});
    }
    if (!g_vk.LoadLoader())
    {
        Log("! vulkan-1.dll is not available");
        return DXGI_ERROR_UNSUPPORTED;
    }
    // An SDK sharing the device (Streamline) tells what the instance and the device must have.
    ExternalRequirements external;
    if (info.externalRequirements)
        info.externalRequirements(external);

    // ---- instance ----
    u32 loaderVersion = VK_API_VERSION_1_0;
    if (g_vk.vkEnumerateInstanceVersion)
        g_vk.vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3)
    {
        Log("! the Vulkan loader is %s, 1.3 is required", VersionText(loaderVersion).c_str());
        return DXGI_ERROR_UNSUPPORTED;
    }
    ExtensionList instanceExtensions;
    {
        u32 count = 0;
        g_vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        instanceExtensions.list.resize(count);
        g_vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, instanceExtensions.list.data());
    }
    std::vector<const char*> iext = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME})
    {
        if (!instanceExtensions.Has(name))
        {
            Log("! the Vulkan instance lacks %s", name);
            return DXGI_ERROR_UNSUPPORTED;
        }
    }
    for (const char* name : {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
             VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME})
    {
        if (instanceExtensions.Has(name))
            iext.push_back(name);
    }
    debugUtils = instanceExtensions.Has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils)
        iext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    for (const char* name : external.instanceExtensions)
    {
        if (Listed(iext, name))
            continue;
        if (instanceExtensions.Has(name))
            iext.push_back(name);
        else
            Log("~ the Vulkan instance lacks %s (asked for by an SDK)", name);
    }
    surfaceMaintenance = instanceExtensions.Has(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
        instanceExtensions.Has(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

    std::vector<const char*> layers;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    const VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    if (info.validation)
    {
        u32 count = 0;
        g_vk.vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        g_vk.vkEnumerateInstanceLayerProperties(&count, available.data());
        bool found = false;
        for (const VkLayerProperties& l : available)
            found |= !std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
        if (found)
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            if (info.syncValidation)
            {
                validationFeatures.enabledValidationFeatureCount = 1;
                validationFeatures.pEnabledValidationFeatures = &syncValidation;
            }
        }
        else
            Log("~ -vkdebug: VK_LAYER_KHRONOS_validation is not installed (Vulkan SDK)");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Dead Air: Refined";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 1, 6, 0);
    app.pEngineName = "Grizzly";
    app.engineVersion = VK_MAKE_API_VERSION(0, 1, 6, 0);
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = u32(iext.size());
    ici.ppEnabledExtensionNames = iext.data();
    ici.enabledLayerCount = u32(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (validationFeatures.enabledValidationFeatureCount)
        ici.pNext = &validationFeatures;
    VkResult result = g_vk.vkCreateInstance(&ici, nullptr, &instance);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreateInstance failed: %s", ResultText(result));
        return DXGI_ERROR_UNSUPPORTED;
    }
    g_vk.LoadInstance(instance);
    if (!layers.empty() && g_vk.vkCreateDebugUtilsMessengerEXT)
    {
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        m.pfnUserCallback = &DebugMessage;
        g_vk.vkCreateDebugUtilsMessengerEXT(instance, &m, nullptr, &messenger);
    }

    // ---- physical device ----
    physical = PickPhysicalDevice(instance, info.adapterLuid);
    if (!physical)
    {
        Log("! no Vulkan 1.3 device");
        return DXGI_ERROR_UNSUPPORTED;
    }
    ExtensionList ext;
    {
        u32 count = 0;
        g_vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        ext.list.resize(count);
        g_vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, ext.list.data());
    }
    if (!ext.Has(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    {
        Log("! the device has no VK_KHR_swapchain");
        return DXGI_ERROR_UNSUPPORTED;
    }

    // Properties.
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p11};
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &p12};
    VkPhysicalDeviceExtendedDynamicState3PropertiesEXT pEds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pGpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR pVrs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    VkPhysicalDeviceMeshShaderPropertiesEXT pMesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    VkPhysicalDeviceCustomBorderColorPropertiesEXT pBorder{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR pAccel{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p13};
    void** tail = &p11.pNext;
    auto chainProps = [&](bool present, auto& s) {
        if (!present)
            return;
        *tail = &s;
        tail = &s.pNext;
    };
    const bool hasEds3 = ext.Has(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    const bool hasGpl = ext.Has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && ext.Has(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    const bool hasVrs = ext.Has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    const bool hasMesh = ext.Has(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    const bool hasBorder = ext.Has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    chainProps(hasEds3, pEds3);
    chainProps(hasGpl, pGpl);
    chainProps(hasVrs, pVrs);
    chainProps(hasMesh, pMesh);
    chainProps(hasBorder, pBorder);
    chainProps(ext.Has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME), pAccel);
    g_vk.vkGetPhysicalDeviceProperties2(physical, &props2);
    props = props2.properties;
    limits = props.limits;
    if (props.apiVersion < VK_API_VERSION_1_3)
    {
        Log("! %s offers Vulkan %s, 1.3 is required (update the driver)", props.deviceName, VersionText(props.apiVersion).c_str());
        return DXGI_ERROR_UNSUPPORTED;
    }

    // Features: query what exists, then enable the subset the layer uses.
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f11};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT fEds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT fEds2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fVertexInput{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fGpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT fRobust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fBorder{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT fDepthClip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT fPriority{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT fPageable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fVrs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT fMesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fAccel{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR fRayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR fPresentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR fPresentWait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fInterlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceMultiDrawFeaturesEXT fMultiDraw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT fSwapMaint{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};

    const bool hasEds2 = ext.Has(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    const bool hasVertexInput = ext.Has(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    const bool hasRobust = ext.Has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    const bool hasDepthClip = ext.Has(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    const bool hasPriority = ext.Has(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
    const bool hasPageable = hasPriority && ext.Has(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
    const bool hasBudget = ext.Has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    const bool hasAccel = ext.Has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && ext.Has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
        ext.Has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool hasPresentWait = ext.Has(VK_KHR_PRESENT_ID_EXTENSION_NAME) && ext.Has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    const bool hasInterlock = ext.Has(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    const bool hasMultiDraw = ext.Has(VK_EXT_MULTI_DRAW_EXTENSION_NAME);
    const bool hasStencilExport = ext.Has(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    const bool hasPushDescriptor = ext.Has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    const bool hasSwapMaint = surfaceMaintenance && ext.Has(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    const bool hasMutableSwapchain = ext.Has(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
    const bool hasExecutables = ext.Has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);

    tail = &f11.pNext;
    auto chain = [&](bool present, auto& s) {
        if (!present)
            return;
        *tail = &s;
        tail = &s.pNext;
    };
    chain(hasEds3, fEds3);
    chain(hasEds2, fEds2);
    chain(hasVertexInput, fVertexInput);
    chain(hasGpl, fGpl);
    chain(hasRobust, fRobust);
    chain(hasBorder, fBorder);
    chain(hasDepthClip, fDepthClip);
    chain(hasPriority, fPriority);
    chain(hasPageable, fPageable);
    chain(hasVrs, fVrs);
    chain(hasMesh, fMesh);
    chain(hasAccel, fAccel);
    chain(hasAccel, fRayQuery);
    chain(hasPresentWait, fPresentId);
    chain(hasPresentWait, fPresentWait);
    chain(hasInterlock, fInterlock);
    chain(hasMultiDraw, fMultiDraw);
    chain(hasSwapMaint, fSwapMaint);
    g_vk.vkGetPhysicalDeviceFeatures2(physical, &features2);

    const VkPhysicalDeviceFeatures have = features2.features;
    char missing[256]{};
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore || !f12.hostQueryReset ||
        !f12.scalarBlockLayout || !f11.shaderDrawParameters || !have.independentBlend || !have.samplerAnisotropy ||
        !have.textureCompressionBC || !have.fragmentStoresAndAtomics || !have.drawIndirectFirstInstance ||
        !have.shaderStorageImageReadWithoutFormat || !have.shaderStorageImageWriteWithoutFormat)
    {
        Reason(missing, sizeof(missing), "%s%s%s%s%s%s%s%s%s%s%s%s", f13.dynamicRendering ? "" : "dynamicRendering ",
            f13.synchronization2 ? "" : "synchronization2 ", f12.timelineSemaphore ? "" : "timelineSemaphore ",
            f12.hostQueryReset ? "" : "hostQueryReset ", f12.scalarBlockLayout ? "" : "scalarBlockLayout ",
            f11.shaderDrawParameters ? "" : "shaderDrawParameters ", have.independentBlend ? "" : "independentBlend ",
            have.samplerAnisotropy ? "" : "samplerAnisotropy ", have.textureCompressionBC ? "" : "textureCompressionBC ",
            have.fragmentStoresAndAtomics ? "" : "fragmentStoresAndAtomics ",
            have.drawIndirectFirstInstance ? "" : "drawIndirectFirstInstance ",
            have.shaderStorageImageReadWithoutFormat && have.shaderStorageImageWriteWithoutFormat ? "" : "storageImageWithoutFormat");
        Log("! %s lacks features the Vulkan renderer needs: %s", props.deviceName, missing);
        return DXGI_ERROR_UNSUPPORTED;
    }

    // Enabled set. Core features: the D3D11 feature level 11_1 equivalents.
    VkPhysicalDeviceFeatures& en = features2.features;
    const VkPhysicalDeviceFeatures supported = have;
    en = VkPhysicalDeviceFeatures{};
    en.independentBlend = VK_TRUE;
    en.samplerAnisotropy = VK_TRUE;
    en.textureCompressionBC = VK_TRUE;
    en.fragmentStoresAndAtomics = VK_TRUE;
    en.drawIndirectFirstInstance = VK_TRUE;
    en.geometryShader = supported.geometryShader;
    en.tessellationShader = supported.tessellationShader;
    en.depthClamp = supported.depthClamp;
    en.depthBiasClamp = supported.depthBiasClamp;
    en.fillModeNonSolid = supported.fillModeNonSolid;
    en.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    en.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    en.shaderImageGatherExtended = supported.shaderImageGatherExtended;
    en.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
    en.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
    en.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
    en.shaderClipDistance = supported.shaderClipDistance;
    en.shaderCullDistance = supported.shaderCullDistance;
    en.imageCubeArray = supported.imageCubeArray;
    en.multiViewport = supported.multiViewport;
    en.sampleRateShading = supported.sampleRateShading;
    en.dualSrcBlend = supported.dualSrcBlend;
    en.multiDrawIndirect = supported.multiDrawIndirect;
    en.shaderFloat64 = supported.shaderFloat64;
    en.shaderInt64 = supported.shaderInt64;
    en.shaderInt16 = supported.shaderInt16;
    en.shaderResourceMinLod = supported.shaderResourceMinLod;
    en.depthBounds = supported.depthBounds;
    en.fullDrawIndexUint32 = supported.fullDrawIndexUint32;
    en.shaderUniformBufferArrayDynamicIndexing = supported.shaderUniformBufferArrayDynamicIndexing;
    en.shaderSampledImageArrayDynamicIndexing = supported.shaderSampledImageArrayDynamicIndexing;
    en.shaderStorageBufferArrayDynamicIndexing = supported.shaderStorageBufferArrayDynamicIndexing;
    en.shaderStorageImageArrayDynamicIndexing = supported.shaderStorageImageArrayDynamicIndexing;
    en.shaderStorageImageMultisample = supported.shaderStorageImageMultisample;
    en.pipelineStatisticsQuery = VK_FALSE;

    // 1.1 / 1.2 / 1.3: keep what the layer uses.
    const VkPhysicalDeviceVulkan11Features s11 = f11;
    const VkPhysicalDeviceVulkan12Features s12 = f12;
    const VkPhysicalDeviceVulkan13Features s13 = f13;
    void* const next11 = f11.pNext;
    f11 = VkPhysicalDeviceVulkan11Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, next11};
    f11.shaderDrawParameters = VK_TRUE;
    f11.storageBuffer16BitAccess = s11.storageBuffer16BitAccess;
    f11.uniformAndStorageBuffer16BitAccess = s11.uniformAndStorageBuffer16BitAccess;
    void* const next12 = f12.pNext;
    f12 = VkPhysicalDeviceVulkan12Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, next12};
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.uniformBufferStandardLayout = s12.uniformBufferStandardLayout;
    f12.shaderFloat16 = s12.shaderFloat16;
    f12.shaderInt8 = s12.shaderInt8;
    f12.samplerFilterMinmax = s12.samplerFilterMinmax;
    f12.shaderOutputLayer = s12.shaderOutputLayer;
    f12.shaderOutputViewportIndex = s12.shaderOutputViewportIndex;
    f12.separateDepthStencilLayouts = s12.separateDepthStencilLayouts;
    f12.imagelessFramebuffer = VK_FALSE;
    f12.drawIndirectCount = s12.drawIndirectCount;
    f12.descriptorIndexing = s12.descriptorIndexing;
    f12.shaderSampledImageArrayNonUniformIndexing = s12.shaderSampledImageArrayNonUniformIndexing;
    f12.shaderStorageBufferArrayNonUniformIndexing = s12.shaderStorageBufferArrayNonUniformIndexing;
    f12.runtimeDescriptorArray = s12.runtimeDescriptorArray;
    f12.descriptorBindingPartiallyBound = s12.descriptorBindingPartiallyBound;
    f12.descriptorBindingSampledImageUpdateAfterBind = s12.descriptorBindingSampledImageUpdateAfterBind;
    f12.descriptorBindingVariableDescriptorCount = s12.descriptorBindingVariableDescriptorCount;
    f12.bufferDeviceAddress = hasAccel && s12.bufferDeviceAddress;
    f12.shaderBufferInt64Atomics = s12.shaderBufferInt64Atomics;
    f12.vulkanMemoryModel = s12.vulkanMemoryModel;
    f12.vulkanMemoryModelDeviceScope = s12.vulkanMemoryModelDeviceScope;
    f12.subgroupBroadcastDynamicId = s12.subgroupBroadcastDynamicId;
    void* const next13 = f13.pNext;
    f13 = VkPhysicalDeviceVulkan13Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, next13};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = s13.maintenance4;
    f13.shaderDemoteToHelperInvocation = s13.shaderDemoteToHelperInvocation;
    f13.subgroupSizeControl = s13.subgroupSizeControl;
    f13.computeFullSubgroups = s13.computeFullSubgroups;
    f13.shaderTerminateInvocation = s13.shaderTerminateInvocation;
    f13.shaderIntegerDotProduct = s13.shaderIntegerDotProduct;
    // SDKs sharing the device keep their data on its objects (vkCreatePrivateDataSlot: AMD FidelityFX, Streamline).
    f13.privateData = s13.privateData;

    // Extensions: what they bring minus what -vk_no_* switches off.
    std::vector<const char*> dext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const bool eds3Wanted = hasEds3 && !(disabledFeatures & NativeFeatureDynamicState3);
    if (eds3Wanted)
    {
        const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT s = fEds3;
        fEds3 = VkPhysicalDeviceExtendedDynamicState3FeaturesEXT{fEds3.sType, fEds3.pNext};
        fEds3.extendedDynamicState3PolygonMode = s.extendedDynamicState3PolygonMode;
        fEds3.extendedDynamicState3DepthClampEnable = s.extendedDynamicState3DepthClampEnable;
        fEds3.extendedDynamicState3SampleMask = s.extendedDynamicState3SampleMask;
        fEds3.extendedDynamicState3AlphaToCoverageEnable = s.extendedDynamicState3AlphaToCoverageEnable;
        fEds3.extendedDynamicState3ColorBlendEnable = s.extendedDynamicState3ColorBlendEnable;
        fEds3.extendedDynamicState3ColorBlendEquation = s.extendedDynamicState3ColorBlendEquation;
        fEds3.extendedDynamicState3ColorWriteMask = s.extendedDynamicState3ColorWriteMask;
        fEds3.extendedDynamicState3RasterizationSamples = s.extendedDynamicState3RasterizationSamples;
        fEds3.extendedDynamicState3DepthClipEnable = s.extendedDynamicState3DepthClipEnable && hasDepthClip;
        // Blend state is only dynamic as a whole: enable, equation and write mask together.
        eds3Blend = s.extendedDynamicState3ColorBlendEnable && s.extendedDynamicState3ColorBlendEquation &&
            s.extendedDynamicState3ColorWriteMask;
        eds3Polygon = s.extendedDynamicState3PolygonMode;
        eds3DepthClamp = s.extendedDynamicState3DepthClampEnable;
        eds3SampleMask = s.extendedDynamicState3SampleMask;
        eds3AlphaToCoverage = s.extendedDynamicState3AlphaToCoverageEnable;
        eds3Samples = s.extendedDynamicState3RasterizationSamples;
        eds3DepthClip = fEds3.extendedDynamicState3DepthClipEnable;
        topologyUnrestricted = pEds3.dynamicPrimitiveTopologyUnrestricted;
        dext.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    }
    else
        fEds3 = VkPhysicalDeviceExtendedDynamicState3FeaturesEXT{fEds3.sType, fEds3.pNext};
    if (hasEds2 && fEds2.extendedDynamicState2PatchControlPoints)
    {
        const bool patch = fEds2.extendedDynamicState2PatchControlPoints;
        fEds2 = VkPhysicalDeviceExtendedDynamicState2FeaturesEXT{fEds2.sType, fEds2.pNext};
        fEds2.extendedDynamicState2 = VK_TRUE;
        fEds2.extendedDynamicState2PatchControlPoints = patch;
        patchControlPointsDynamic = patch;
        dext.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    }
    else
        fEds2 = VkPhysicalDeviceExtendedDynamicState2FeaturesEXT{fEds2.sType, fEds2.pNext};
    vertexInputDynamic = hasVertexInput && fVertexInput.vertexInputDynamicState &&
        !(disabledFeatures & NativeFeatureVertexInput);
    if (vertexInputDynamic)
        dext.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    else
        fVertexInput.vertexInputDynamicState = VK_FALSE;
    const bool gpl = hasGpl && fGpl.graphicsPipelineLibrary && pGpl.graphicsPipelineLibraryFastLinking &&
        !(disabledFeatures & NativeFeaturePipelineLibrary);
    if (gpl)
    {
        dext.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        dext.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    }
    else
        fGpl.graphicsPipelineLibrary = VK_FALSE;
    nullDescriptor = hasRobust && fRobust.nullDescriptor;
    if (nullDescriptor)
    {
        fRobust.robustBufferAccess2 = VK_FALSE;
        fRobust.robustImageAccess2 = VK_FALSE;
        dext.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }
    else
        fRobust = VkPhysicalDeviceRobustness2FeaturesEXT{fRobust.sType, fRobust.pNext};
    customBorderColor = hasBorder && fBorder.customBorderColors && fBorder.customBorderColorWithoutFormat;
    if (customBorderColor)
        dext.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    else
        fBorder = VkPhysicalDeviceCustomBorderColorFeaturesEXT{fBorder.sType, fBorder.pNext};
    depthClipEnable = hasDepthClip && fDepthClip.depthClipEnable;
    if (depthClipEnable)
        dext.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    else
        fDepthClip.depthClipEnable = VK_FALSE;
    memoryPriority = hasPriority && fPriority.memoryPriority;
    if (memoryPriority)
    {
        dext.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        // Lets the driver page whole low-priority blocks out first under VRAM pressure.
        if (hasPageable && fPageable.pageableDeviceLocalMemory)
            dext.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
        else
            fPageable.pageableDeviceLocalMemory = VK_FALSE;
    }
    else
    {
        fPriority.memoryPriority = VK_FALSE;
        fPageable.pageableDeviceLocalMemory = VK_FALSE;
    }
    if (hasBudget)
        dext.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    caps.memoryBudget = hasBudget;
    const bool vrs = hasVrs && fVrs.pipelineFragmentShadingRate && fVrs.attachmentFragmentShadingRate &&
        !(disabledFeatures & NativeFeatureShadingRate);
    if (vrs)
    {
        fVrs.primitiveFragmentShadingRate = VK_FALSE;
        dext.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    }
    else
        fVrs = VkPhysicalDeviceFragmentShadingRateFeaturesKHR{fVrs.sType, fVrs.pNext};
    const bool mesh = hasMesh && fMesh.meshShader && fMesh.taskShader && !(disabledFeatures & NativeFeatureMeshShaders);
    if (mesh)
    {
        fMesh.multiviewMeshShader = VK_FALSE;
        fMesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
        fMesh.meshShaderQueries = VK_FALSE;
        dext.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }
    else
        fMesh = VkPhysicalDeviceMeshShaderFeaturesEXT{fMesh.sType, fMesh.pNext};
    // The RT lane binds by device address and through the bindless set (descriptor indexing with updates after bind).
    const bool bindlessIndexing = s12.runtimeDescriptorArray && s12.descriptorBindingPartiallyBound &&
        s12.descriptorBindingSampledImageUpdateAfterBind && s12.descriptorBindingStorageBufferUpdateAfterBind &&
        s12.descriptorBindingUpdateUnusedWhilePending && s12.shaderSampledImageArrayNonUniformIndexing &&
        s12.shaderStorageBufferArrayNonUniformIndexing && p12.maxPerStageDescriptorUpdateAfterBindSampledImages >= 4096 &&
        p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= 4096;
    const bool rayQuery = hasAccel && fAccel.accelerationStructure && fRayQuery.rayQuery && s12.bufferDeviceAddress &&
        en.shaderInt64 && bindlessIndexing && pAccel.minAccelerationStructureScratchOffsetAlignment <= 256;
    if (rayQuery)
    {
        f12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        f12.descriptorIndexing = s12.descriptorIndexing;
        fAccel.accelerationStructureCaptureReplay = VK_FALSE;
        fAccel.accelerationStructureIndirectBuild = VK_FALSE;
        fAccel.accelerationStructureHostCommands = VK_FALSE;
        dext.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        dext.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        dext.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    else
    {
        fAccel = VkPhysicalDeviceAccelerationStructureFeaturesKHR{fAccel.sType, fAccel.pNext};
        fRayQuery = VkPhysicalDeviceRayQueryFeaturesKHR{fRayQuery.sType, fRayQuery.pNext};
        f12.bufferDeviceAddress = VK_FALSE;
    }
    caps.presentWait = hasPresentWait && fPresentId.presentId && fPresentWait.presentWait;
    if (caps.presentWait)
    {
        dext.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        dext.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }
    else
    {
        fPresentId.presentId = VK_FALSE;
        fPresentWait.presentWait = VK_FALSE;
    }
    caps.fragmentShaderInterlock = hasInterlock && fInterlock.fragmentShaderPixelInterlock;
    if (caps.fragmentShaderInterlock)
    {
        fInterlock.fragmentShaderShadingRateInterlock = VK_FALSE;
        dext.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    }
    else
        fInterlock = VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT{fInterlock.sType, fInterlock.pNext};
    caps.multiDraw = hasMultiDraw && fMultiDraw.multiDraw && !(disabledFeatures & NativeFeatureMultiDraw);
    if (caps.multiDraw)
        dext.push_back(VK_EXT_MULTI_DRAW_EXTENSION_NAME);
    else
        fMultiDraw.multiDraw = VK_FALSE;
    swapchainMaintenance = hasSwapMaint && fSwapMaint.swapchainMaintenance1;
    if (swapchainMaintenance)
        dext.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    else
        fSwapMaint.swapchainMaintenance1 = VK_FALSE;
    if (hasStencilExport)
        dext.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    pushDescriptors = hasPushDescriptor;
    if (pushDescriptors)
        dext.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    mutableSwapchain = hasMutableSwapchain;
    if (mutableSwapchain)
        dext.push_back(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
    if (hasExecutables && info.validation)
        dext.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);

    // What an SDK sharing the device asked for. VK_EXT_buffer_device_address stays off: it may not be enabled
    // with the core bufferDeviceAddress feature, which serves the SDK the same.
    for (const char* name : external.deviceExtensions)
    {
        if (!std::strcmp(name, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
        {
            f12.bufferDeviceAddress = s12.bufferDeviceAddress;
            continue;
        }
        if (Listed(dext, name))
            continue;
        if (ext.Has(name))
            dext.push_back(name);
        else
            Log("~ %s lacks %s (asked for by an SDK)", props.deviceName, name);
    }
    for (const char* name : external.features12)
    {
        if (!EnableFeature(kFeatures12, name, s12, f12))
            Log("~ %s lacks the Vulkan 1.2 feature %s (asked for by an SDK)", props.deviceName, name);
    }
    for (const char* name : external.features13)
    {
        if (!EnableFeature(kFeatures13, name, s13, f13))
            Log("~ %s lacks the Vulkan 1.3 feature %s (asked for by an SDK)", props.deviceName, name);
    }

    // ---- queues ----
    u32 familyCount = 0;
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    graphicsFamily = ~0u;
    for (u32 i = 0; i < familyCount; ++i)
    {
        const VkQueueFlags f = families[i].queueFlags;
        if ((f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT) && graphicsFamily == ~0u)
            graphicsFamily = i;
        else if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && computeFamily == ~0u)
            computeFamily = i;
        else if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) && transferFamily == ~0u)
            transferFamily = i;
    }
    if (graphicsFamily == ~0u)
    {
        Log("! the device has no graphics queue");
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (disabledFeatures & NativeFeatureAsyncCompute)
        computeFamily = ~0u;
    if (disabledFeatures & NativeFeatureTransferQueue)
        transferFamily = ~0u;
    // The graphics queue first, async compute equal (it should overlap, not starve), uploads below. An SDK's
    // extra queues follow the layer's own in their family (as many as the family has).
    static constexpr float kPriorities[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    const float transferPriority = 0.5f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    auto addQueue = [&](u32 family, u32 count, const float* priority) {
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = (std::min)({count, families[family].queueCount, u32(std::size(kPriorities))});
        q.pQueuePriorities = priority;
        queues.push_back(q);
        return q.queueCount;
    };
    NativeDeviceInfo native;
    native.graphicsFamily = native.computeFamily = graphicsFamily;
    native.graphicsQueueIndex = 1;
    // Without an async compute family the SDK's compute queues come after its graphics ones.
    const u32 computeHome = computeFamily != ~0u ? computeFamily : graphicsFamily;
    // A presenter's queues come last in the graphics family.
    const u32 presentBase = 1 + external.graphicsQueues + (computeHome == graphicsFamily ? external.computeQueues : 0);
    const u32 graphicsCount = addQueue(graphicsFamily, presentBase + info.presentQueues, kPriorities);
    presentQueueIndex = presentBase;
    presentQueueCount = graphicsCount > presentBase ? (std::min)(graphicsCount - presentBase, info.presentQueues) : 0;
    native.presentQueueIndex = presentQueueIndex;
    native.presentQueueCount = presentQueueCount;
    if (presentQueueCount < info.presentQueues)
        Log("~ the graphics family has %u queue(s): %u of %u for a presenter", graphicsCount, presentQueueCount,
            info.presentQueues);
    if (computeFamily != ~0u)
    {
        addQueue(computeFamily, 1 + external.computeQueues, kPriorities);
        native.computeFamily = computeFamily;
        native.computeQueueIndex = 1;
    }
    else
        native.computeQueueIndex = 1 + external.graphicsQueues;
    if (graphicsCount < 1 + external.graphicsQueues)
        Log("~ the graphics family has %u queue(s): an SDK asked for %u more", graphicsCount, external.graphicsQueues);
    if (transferFamily != ~0u)
        addQueue(transferFamily, 1, &transferPriority);

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features2};
    dci.queueCreateInfoCount = u32(queues.size());
    dci.pQueueCreateInfos = queues.data();
    dci.enabledExtensionCount = u32(dext.size());
    dci.ppEnabledExtensionNames = dext.data();
    result = g_vk.vkCreateDevice(physical, &dci, nullptr, &vk);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreateDevice failed: %s", ResultText(result));
        return DXGI_ERROR_UNSUPPORTED;
    }
    g_vk.LoadDevice(vk);
    g_vk.vkGetDeviceQueue(vk, graphicsFamily, 0, &graphicsQueue);
    if (computeFamily != ~0u)
        g_vk.vkGetDeviceQueue(vk, computeFamily, 0, &computeQueue);
    if (transferFamily != ~0u)
        g_vk.vkGetDeviceQueue(vk, transferFamily, 0, &transferQueue);
    InitFormatSupport(physical, !(disabledFeatures & NativeFeatureDepth24));
    if (info.deviceCreated)
    {
        native.instance = instance;
        native.physicalDevice = physical;
        native.device = vk;
        info.deviceCreated(native);
    }
    if (info.deviceProc)
    {
        // An SDK's proxies in place of the driver's calls (they forward to the driver).
        auto take = [&](const char* name, auto& fn) {
            if (void* proc = info.deviceProc(name))
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(proc);
        };
        take("vkCreateSwapchainKHR", g_vk.vkCreateSwapchainKHR);
        take("vkDestroySwapchainKHR", g_vk.vkDestroySwapchainKHR);
        take("vkGetSwapchainImagesKHR", g_vk.vkGetSwapchainImagesKHR);
        take("vkAcquireNextImageKHR", g_vk.vkAcquireNextImageKHR);
        take("vkQueuePresentKHR", g_vk.vkQueuePresentKHR);
        take("vkDeviceWaitIdle", g_vk.vkDeviceWaitIdle);
        take("vkCreateImage", g_vk.vkCreateImage);
        take("vkDestroyImage", g_vk.vkDestroyImage);
    }

    // ---- what the device enabled, as the rest of the layer reads it ----
    shaderFeatures.geometry = en.geometryShader;
    shaderFeatures.tessellation = en.tessellationShader;
    shaderFeatures.float16 = f12.shaderFloat16;
    shaderFeatures.float64 = en.shaderFloat64;
    shaderFeatures.int64 = en.shaderInt64;
    shaderFeatures.int64Atomics = f12.shaderBufferInt64Atomics;
    shaderFeatures.int16 = en.shaderInt16;
    shaderFeatures.int8 = f12.shaderInt8;
    shaderFeatures.clipDistance = en.shaderClipDistance;
    shaderFeatures.cullDistance = en.shaderCullDistance;
    shaderFeatures.cubeArray = en.imageCubeArray;
    shaderFeatures.sampleRate = en.sampleRateShading;
    shaderFeatures.minLod = en.shaderResourceMinLod;
    shaderFeatures.storageExtended = en.shaderStorageImageExtendedFormats;
    shaderFeatures.geometryStreams = false;
    shaderFeatures.readWithoutFormat = en.shaderStorageImageReadWithoutFormat;
    shaderFeatures.writeWithoutFormat = en.shaderStorageImageWriteWithoutFormat;
    shaderFeatures.multiViewport = en.multiViewport;
    shaderFeatures.outputLayer = f12.shaderOutputLayer;
    shaderFeatures.outputViewport = f12.shaderOutputViewportIndex;
    shaderFeatures.drawParameters = true;
    shaderFeatures.rayQuery = rayQuery;
    shaderFeatures.stencilExport = hasStencilExport;
    shaderFeatures.viewportLayer = f12.shaderOutputLayer && f12.shaderOutputViewportIndex;
    shaderFeatures.fullyCovered = false;
    shaderFeatures.mesh = mesh;
    shaderFeatures.interlock = caps.fragmentShaderInterlock;
    cubeArray = en.imageCubeArray;
    depthBiasClamp = en.depthBiasClamp;
    preciseOcclusion = en.occlusionQueryPrecise;
    labels = debugUtils && (info.validation || info.labels);

    caps.apiVersion = props.apiVersion;
    caps.driverVersion = props.driverVersion;
    caps.vendorId = props.vendorID;
    caps.deviceId = props.deviceID;
    std::snprintf(caps.deviceName, sizeof(caps.deviceName), "%s", props.deviceName);
    std::snprintf(caps.driverName, sizeof(caps.driverName), "%s", p12.driverName);
    std::snprintf(caps.driverInfo, sizeof(caps.driverInfo), "%s", p12.driverInfo);
    caps.asyncCompute = computeQueue != VK_NULL_HANDLE;
    caps.transferQueue = transferQueue != VK_NULL_HANDLE;
    caps.dynamicState3 = eds3Blend && eds3Polygon;
    caps.vertexInputDynamic = vertexInputDynamic;
    caps.pipelineLibrary = gpl;
    caps.pushDescriptors = pushDescriptors;
    caps.nullDescriptors = nullDescriptor;
    caps.shadingRate = vrs;
    caps.shadingRateTexel = vrs ? (std::max)(pVrs.minFragmentShadingRateAttachmentTexelSize.width, 8u) : 0;
    caps.shadingRateLarge = vrs && pVrs.maxFragmentSize.width >= 4 && pVrs.maxFragmentSize.height >= 4;
    caps.meshShaders = mesh;
    caps.rayQuery = rayQuery;
    caps.float16 = f12.shaderFloat16;
    caps.int16 = en.shaderInt16;
    caps.int64Atomics = f12.shaderBufferInt64Atomics;
    caps.subgroupSize = p11.subgroupSize;
    caps.subgroupSizeMin = p13.minSubgroupSize ? p13.minSubgroupSize : p11.subgroupSize;
    caps.subgroupSizeMax = p13.maxSubgroupSize ? p13.maxSubgroupSize : p11.subgroupSize;
    {
        // Wave intrinsics (WaveActiveSum, WavePrefixSum, ballots) in pixel and compute shaders.
        constexpr VkSubgroupFeatureFlags needed = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
            VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
            VK_SUBGROUP_FEATURE_QUAD_BIT;
        constexpr VkShaderStageFlags stagesNeeded = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        caps.subgroupArithmetic = (p11.subgroupSupportedOperations & needed) == needed &&
            (p11.subgroupSupportedStages & stagesNeeded) == stagesNeeded;
    }
    caps.timestampPeriod = limits.timestampPeriod;
    meshProperties = pMesh;
    shadingRateProperties = pVrs;

    // ---- infrastructure ----
    if (!timeline.Init(vk))
        return E_FAIL;
    releaser.Init(this, &timeline);
    if (!memory.Init(this))
        return E_FAIL;
    uploadPages.Init(this, MemoryClass::Upload);
    readbackPages.Init(this, MemoryClass::Readback);
    {
        const bool transfer = transferQueue != VK_NULL_HANDLE;
        if (!init.Init(this, transfer ? transferFamily : graphicsFamily, transfer ? transferQueue : graphicsQueue,
                transfer ? &transferQueueLock : &graphicsQueueLock, transfer))
            return E_FAIL;
        // Clears and depth uploads cannot run on a transfer queue: their own uploader on the graphics queue.
        if (transfer && !initGraphics.Init(this, graphicsFamily, graphicsQueue, &graphicsQueueLock, false))
            return E_FAIL;
    }
    creationHeap = new (std::nothrow) DynamicHeap(this);
    if (!creationHeap)
        return E_OUTOFMEMORY;
    if (!layouts.Init(this))
        return E_FAIL;
    // The RT lane's bindless set (a device without it traces nothing).
    if (caps.rayQuery)
    {
        bindless.capacity = (std::min)({65536u, p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
            p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers, p12.maxDescriptorSetUpdateAfterBindSampledImages,
            p12.maxDescriptorSetUpdateAfterBindStorageBuffers});
        if (!InitBindless())
        {
            Log("! the bindless set could not be created: no ray tracing");
            caps.rayQuery = false;
            shaderFeatures.rayQuery = false;
        }
    }
    {
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (g_vk.vkCreateDescriptorPool(vk, &pool, nullptr, &emptyPool) != VK_SUCCESS)
            return E_FAIL;
        const VkDescriptorSetLayout empty = layouts.Empty()->layout;
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = emptyPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &empty;
        if (g_vk.vkAllocateDescriptorSets(vk, &alloc, &emptySet) != VK_SUCCESS)
            return E_FAIL;
    }
    if (!nulls.Init(this))
    {
        Log("! null resources could not be created");
        return E_FAIL;
    }
    pipelines.Init(this, info.pipelineCachePath);
    queryPools.Init(this);
    try
    {
        m_dxgiDevice = new DxgiDevice(this);
        immediate = new Context(this, false, kImmediateContextIndex);
    }
    catch (...)
    {
        return E_OUTOFMEMORY;
    }
    {
        std::lock_guard lock(g_devicesLock);
        if (DeviceRecord* record = FindRecord(this))
            record->ready = true;
    }

    // ---- the line that tells a log reader what this session runs on ----
    Log("* %s, Vulkan %s, driver %s %s", props.deviceName, VersionText(props.apiVersion).c_str(), p12.driverName,
        p12.driverInfo);
    Log("* queues: graphics %u%s%s, memory: %s, dynamic state: EDS3 %s, vertex input %s, pipelines: %s",
        graphicsFamily, caps.asyncCompute ? ", async compute" : "", caps.transferQueue ? ", transfer" : "",
        caps.rebar ? "resizable BAR" : "system memory for dynamic data", caps.dynamicState3 ? "yes" : "no",
        vertexInputDynamic ? "yes" : "no", gpl ? "libraries with fast linking" : "monolithic");
    Log("* features: null descriptors %s, push descriptors %s, VRS %s, mesh shaders %s, ray query %s, present wait %s, "
        "multi draw %s, D24S8 %s",
        nullDescriptor ? "yes" : "no", pushDescriptors ? "yes" : "no", vrs ? "yes" : "no", mesh ? "yes" : "no",
        rayQuery ? "yes" : "no", caps.presentWait ? "yes" : "no", caps.multiDraw ? "yes" : "no",
        Format(DXGI_FORMAT_D24_UNORM_S8_UINT).format == VK_FORMAT_D24_UNORM_S8_UINT ? "native" : "as D32S8");
    return S_OK;
}

void Device::Shutdown()
{
    {
        std::lock_guard lock(g_devicesLock);
        if (DeviceRecord* record = FindRecord(this))
            record->ready = false;
    }
    if (vk)
    {
        if (immediate)
        {
            // The last frame's work and the creation-time copies.
            WaitIdle();
        }
        g_vk.vkDeviceWaitIdle(vk);
        pipelines.Shutdown();
        if (immediate)
        {
            Context* context = immediate;
            immediate = nullptr;
            context->Release();
        }
        {
            std::lock_guard lock(stateLock);
            ReleaseAll(blendStates);
            ReleaseAll(depthStates);
            ReleaseAll(rasterStates);
            ReleaseAll(samplerStates);
        }
        init.Shutdown();
        initGraphics.Shutdown();
        if (creationHeap && creationHeap->Orphan())
            delete creationHeap;
        creationHeap = nullptr;
        releaser.Collect(true);
        queryPools.Shutdown();
        ShutdownBindless();
        nulls.Shutdown(this);
        if (emptyPool)
            g_vk.vkDestroyDescriptorPool(vk, emptyPool, nullptr);
        emptyPool = VK_NULL_HANDLE;
        emptySet = VK_NULL_HANDLE;
        layouts.Shutdown();
        uploadPages.Shutdown();
        readbackPages.Shutdown();
        releaser.Collect(true);
        memory.Shutdown();
        timeline.Shutdown(vk);
        g_vk.vkDestroyDevice(vk, nullptr);
        vk = VK_NULL_HANDLE;
    }
    if (messenger && g_vk.vkDestroyDebugUtilsMessengerEXT)
        g_vk.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    messenger = VK_NULL_HANDLE;
    if (instance)
        g_vk.vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
    delete m_dxgiDevice;
    m_dxgiDevice = nullptr;
    if (adapter)
        adapter->Release();
    adapter = nullptr;
    std::lock_guard lock(g_devicesLock);
    for (size_t i = 0; i < g_devices.size(); ++i)
    {
        if (g_devices[i].device == this)
        {
            g_devices.erase(g_devices.begin() + i);
            break;
        }
    }
}

// ============================================================================================
// Frames
// ============================================================================================

void Device::EndFrame(u64 lastValue)
{
    const u64 current = frame.load(std::memory_order_relaxed);
    uploadPages.EndFrame(current, lastValue);
    readbackPages.EndFrame(current, lastValue);
    frameValues[current % kFramesInFlight] = lastValue;
    const u64 next = current + 1;
    frame.store(next, std::memory_order_release);
    // The slot of the new frame holds the value of frame (next - kFramesInFlight): once it passed, pools
    // and memory tagged kFramesInFlight + 1 frames ago are idle.
    if (next > kFramesInFlight)
    {
        // -vkstats: the time the CPU waits here for the GPU (high = GPU-bound, near zero = the GPU waits for us).
        const auto waitStart = stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        timeline.Wait(vk, frameValues[next % kFramesInFlight]);
        if (stats)
            Count(StatFrameWaitUs, u64(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - waitStart).count()));
    }
    uploadPages.Recycle();
    readbackPages.Recycle();
    releaser.Collect(false);
    if (stats && next % kStatsFrames == 0)
    {
        u64 s[StatCount];
        for (u32 i = 0; i < StatCount; ++i)
            s[i] = statCounters[i].exchange(0, std::memory_order_relaxed) / kStatsFrames;
        Log("* stats per frame: %llu draws, %llu dispatches, %llu pipeline binds (%llu new per %u frames), %llu sets, "
            "%llu offset-only rebinds, %llu barriers in %llu batches, %llu renderings, %llu maps, %llu submissions, "
            "%llu creation-time upload flushes (%llu KB), %llu submissions waiting for them, %llu set cache restarts per %u frames",
            s[StatDraws], s[StatDispatches], s[StatPipelineBinds], s[StatPipelines] * kStatsFrames, kStatsFrames, s[StatSets],
            s[StatOffsetsOnly], s[StatBarriers], s[StatBarrierBatches], s[StatRenderings], s[StatMaps], s[StatSubmits],
            s[StatInitFlushes], s[StatInitKB], s[StatInitWaits], s[StatSetFlushes] * kStatsFrames, kStatsFrames);
        Log("* sync per frame: %llu query polls not ready, %llu submissions for queries, %llu for staging maps, "
            "%llu staging map waits, %.2f ms waiting for the GPU at the frame limit",
            s[StatQueryNotReady], s[StatQuerySubmits], s[StatMapSubmits], s[StatMapWaits], s[StatFrameWaitUs] / 1000.0);
        // Per engine scope, the most barriers first.
        {
            std::lock_guard lock(scopeLock);
            std::vector<std::pair<std::string, std::array<u64, StatCount>>> scopes(scopeStats.begin(), scopeStats.end());
            scopeStats.clear();
            std::sort(scopes.begin(), scopes.end(), [](const auto& a, const auto& b) {
                return a.second[StatBarrierBatches] + a.second[StatRenderings] > b.second[StatBarrierBatches] + b.second[StatRenderings];
            });
            for (size_t i = 0; i < scopes.size() && i < 24; ++i)
            {
                const auto& v = scopes[i].second;
                Log("*   scope %s: %llu draws, %llu dispatches, %llu barriers in %llu batches (%llu layout changes, %llu of depth), "
                    "%llu renderings, %llu pipeline binds, %llu sets, %llu submissions, %llu query polls not ready",
                    scopes[i].first.c_str(), v[StatDraws] / kStatsFrames,
                    v[StatDispatches] / kStatsFrames, v[StatBarriers] / kStatsFrames, v[StatBarrierBatches] / kStatsFrames,
                    v[StatLayoutChanges] / kStatsFrames, v[StatDepthLayoutChanges] / kStatsFrames, v[StatRenderings] / kStatsFrames,
                    v[StatPipelineBinds] / kStatsFrames, v[StatSets] / kStatsFrames, v[StatSubmits] / kStatsFrames,
                    v[StatQueryNotReady] / kStatsFrames);
            }
        }
        // Video memory: what the layer holds per heap against the driver's budget (an overcommit pages to system memory).
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        if (caps.memoryBudget)
            props.pNext = &budgets;
        g_vk.vkGetPhysicalDeviceMemoryProperties2(physical, &props);
        for (u32 h = 0; h < props.memoryProperties.memoryHeapCount; ++h)
        {
            const VkMemoryHeap& heap = props.memoryProperties.memoryHeaps[h];
            Log("* memory heap %u%s: layer %llu MB, process %llu MB, budget %llu MB, size %llu MB", h,
                (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : "", memory.BytesInUse(h) >> 20,
                caps.memoryBudget ? budgets.heapUsage[h] >> 20 : 0ull, caps.memoryBudget ? budgets.heapBudget[h] >> 20 : 0ull,
                heap.size >> 20);
        }
    }
}

void Device::WaitIdle()
{
    if (!vk)
        return;
    if (immediate)
    {
        const u64 value = immediate->Submit();
        timeline.Wait(vk, value);
    }
    init.Flush();
    init.timeline().Wait(vk, init.timeline().NextValue() - 1);
    std::lock_guard lock(graphicsQueueLock);
    g_vk.vkQueueWaitIdle(graphicsQueue);
}

// ============================================================================================
// Public API (vk_api.h)
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

bool ProbeAdapter(const LUID& adapterLuid, char* reason, size_t reasonSize)
{
    Reason(reason, reasonSize, "%s", "");
    if (!g_vk.LoadLoader())
    {
        Reason(reason, reasonSize, "vulkan-1.dll is not available");
        return false;
    }
    u32 loaderVersion = VK_API_VERSION_1_0;
    if (g_vk.vkEnumerateInstanceVersion)
        g_vk.vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3)
    {
        Reason(reason, reasonSize, "the Vulkan loader is %s, 1.3 is required", VersionText(loaderVersion).c_str());
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (g_vk.vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
    {
        Reason(reason, reasonSize, "vkCreateInstance failed");
        return false;
    }
    // A throw-away table: the probe must not disturb a live device's function pointers.
    Api api = g_vk;
    api.LoadInstance(instance);
    bool ok = false;
    {
        const Api saved = g_vk;
        g_vk = api;
        const VkPhysicalDevice physical = PickPhysicalDevice(instance, adapterLuid);
        if (!physical)
            Reason(reason, reasonSize, "no Vulkan 1.3 device");
        else
        {
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
            g_vk.vkGetPhysicalDeviceFeatures2(physical, &features);
            const VkPhysicalDeviceFeatures& core = features.features;
            ok = f13.dynamicRendering && f13.synchronization2 && f12.timelineSemaphore && f12.hostQueryReset &&
                f12.scalarBlockLayout && core.textureCompressionBC && core.shaderStorageImageReadWithoutFormat &&
                core.shaderStorageImageWriteWithoutFormat;
            if (!ok)
                Reason(reason, reasonSize,
                    "the device lacks dynamic rendering, synchronization2, timeline semaphores or format-less storage images");
        }
        g_vk.vkDestroyInstance(instance, nullptr);
        g_vk = saved;
    }
    return ok;
}

bool IsLayerDevice(ID3D11Device* device)
{
    if (!device)
        return false;
    std::lock_guard lock(g_devicesLock);
    for (const DeviceRecord& record : g_devices)
    {
        if (static_cast<ID3D11Device*>(record.device) == device)
            return record.ready;
    }
    return false;
}

void SavePipelineCache(ID3D11Device* device)
{
    if (IsLayerDevice(device))
        Device::From(device)->pipelines.Save();
}

const NativeCaps* GetNativeCaps(ID3D11Device* device)
{
    return IsLayerDevice(device) ? &Device::From(device)->caps : nullptr;
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
            context->device()->timeline.Wait(context->device()->vk, value);
    }
    catch (...)
    {
        Log("! Flush failed");
    }
}
} // namespace xrvk
