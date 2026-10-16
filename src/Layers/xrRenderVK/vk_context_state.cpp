// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Context, part 1: the D3D11 binding API with D3D11's hazard rules, and the preparation of draws and
// dispatches - requirements batched into one barrier call, rendering begun lazily, the pipeline looked up
// only when pipeline state changed, every dynamic state applied by group, per-stage descriptor sets with
// dynamic constant buffer offsets (a renamed constant buffer costs an offset, not a descriptor write) and
// identical sets reused within a frame.
#include "vk_layer.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace xrvk
{
namespace
{
template <class... Args>
void Once(const char* fmt, Args... args)
{
    LogOnce(fmt, fmt, args...);
}

bool ClampRange(UINT& start, UINT& num, u32 limit, const char* what)
{
    if (u64(start) + num <= limit)
        return num != 0;
    LogOnce(what, "%s slots %u..%llu are beyond the layer limit %u; clamped", what, start, u64(start) + num - 1, limit);
    if (start >= limit)
        return false;
    num = limit - start;
    return true;
}

template <class T>
bool Assign(T*& slot, T* value)
{
    if (slot == value)
        return false;
    if (value)
        value->AddRef();
    T* old = slot;
    slot = value;
    if (old)
        old->Release();
    return true;
}

inline View* AsView(ID3D11ShaderResourceView* v) { return static_cast<ShaderResourceView*>(v); }
inline View* AsView(ID3D11RenderTargetView* v) { return static_cast<RenderTargetView*>(v); }
inline View* AsView(ID3D11DepthStencilView* v) { return static_cast<DepthStencilView*>(v); }
inline View* AsView(ID3D11UnorderedAccessView* v) { return static_cast<UnorderedAccessView*>(v); }
inline Resource* AsResource(ID3D11Buffer* b) { return static_cast<Buffer*>(b); }

inline bool Overlaps(const SubRange& a, const SubRange& b)
{
    return a.firstMip < b.firstMip + b.mipCount && b.firstMip < a.firstMip + a.mipCount &&
        a.firstLayer < b.firstLayer + b.layerCount && b.firstLayer < a.firstLayer + a.layerCount;
}

inline bool Aliases(View* v, Resource* target, const SubRange& range)
{
    return v->resource == target && Overlaps(v->range, range);
}

// A DSV writes unless both its planes are read-only (or the format has only the read-only one).
bool DsvWrites(const View* v)
{
    const bool depthRO = (v->dsvFlags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
    const bool stencilRO = (v->dsvFlags & D3D11_DSV_READ_ONLY_STENCIL) != 0 || !(v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    return !(depthRO && stencilRO);
}

// The layout of a depth-stencil attachment by its read-only planes (storage-capable images: GENERAL).
VkImageLayout DepthLayout(const View* v)
{
    if (v->resource->bindFlags & D3D11_BIND_UNORDERED_ACCESS)
        return VK_IMAGE_LAYOUT_GENERAL;
    const bool stencil = (v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    const bool depthRO = (v->dsvFlags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
    const bool stencilRO = (v->dsvFlags & D3D11_DSV_READ_ONLY_STENCIL) != 0 || !stencil;
    if (depthRO && stencilRO)
        return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    if (depthRO)
        return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    if (stencilRO && stencil)
        return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
}

VkPipelineStageFlags2 StageMask(Stage stage, bool mesh)
{
    switch (stage)
    {
    case Stage::VS: return mesh ? VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    case Stage::HS: return VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
    case Stage::DS: return VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
    case Stage::GS: return mesh ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT : VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
    case Stage::PS: return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    default: return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
}

VkPrimitiveTopology VkTopology(D3D11_PRIMITIVE_TOPOLOGY t, u32& patchPoints)
{
    patchPoints = 0;
    switch (t)
    {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default:
        if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
        {
            patchPoints = u32(t) - u32(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1;
            return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        }
        return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}

bool IsStrip(VkPrimitiveTopology t)
{
    return t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
        t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}

VkPrimitiveTopology TopologyClass(VkPrimitiveTopology t)
{
    switch (t)
    {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// D3D11 defaults of the null state objects.
const RasterizerState* DefaultRaster()
{
    static RasterizerState* state = [] {
        auto* s = static_cast<RasterizerState*>(::operator new(sizeof(RasterizerState)));
        std::memset(static_cast<void*>(s), 0, sizeof(RasterizerState));
        s->polygon = VK_POLYGON_MODE_FILL;
        s->cull = VK_CULL_MODE_BACK_BIT;
        s->front = VK_FRONT_FACE_CLOCKWISE;
        return s;
    }();
    return state;
}

struct DepthDefaults
{
    bool depthTest = true, depthWrite = true, stencilTest = false;
    VkCompareOp compare = VK_COMPARE_OP_LESS;
    VkStencilOpState face{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
};

void SetConstantBuffers(Context& c, Stage stage, UINT start, UINT num, ID3D11Buffer* const* buffers)
{
    if (!ClampRange(start, num, kMaxCBs, "constant buffer"))
        return;
    StageBindings& sb = c.stages[u32(stage)];
    for (UINT i = 0; i < num; ++i)
    {
        if (Assign(sb.cb[start + i], buffers ? buffers[i] : nullptr) && sb.shader &&
            (sb.shader->iface.cbMask & (1u << (start + i))))
            sb.dirty = true;
    }
}

void SetShaderResources(Context& c, Stage stage, UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    if (!ClampRange(start, num, kMaxSRVs, "shader resource"))
        return;
    StageBindings& sb = c.stages[u32(stage)];
    for (UINT i = 0; i < num; ++i)
    {
        ID3D11ShaderResourceView* view = views ? views[i] : nullptr;
        ID3D11ShaderResourceView*& slot = sb.srv[start + i];
        if (view == slot)
            continue;
        // D3D11 binds null instead of an input that aliases a bound output.
        if (view)
        {
            View* v = AsView(view);
            if (c.IsBoundAsOutput(v->resource, v->range))
                view = nullptr;
        }
        if (Assign(slot, view))
        {
            if (sb.shader && (sb.shader->iface.srvMask & (1u << (start + i))))
            {
                sb.dirty = true;
                if (stage == Stage::CS)
                    c.applied.computeHazardsDirty = true;
                else
                    c.applied.hazardsDirty = true;
            }
        }
    }
}

void SetSamplers(Context& c, Stage stage, UINT start, UINT num, ID3D11SamplerState* const* samplers)
{
    if (!ClampRange(start, num, kMaxSamplers, "sampler"))
        return;
    StageBindings& sb = c.stages[u32(stage)];
    for (UINT i = 0; i < num; ++i)
    {
        if (Assign(sb.samplers[start + i], samplers ? samplers[i] : nullptr) && sb.shader &&
            (sb.shader->iface.samplerMask & (1u << (start + i))))
            sb.dirty = true;
    }
}

void SetShader(Context& c, Stage stage, ID3D11DeviceChild* shader, UINT numInstances)
{
    if (numInstances)
        Once("shader class instances are not supported; ignored");
    StageBindings& sb = c.stages[u32(stage)];
    if (sb.shaderIface == shader)
        return;
    Shader* next = Shader::From(shader, stage);
    sb.shader = next;
    Assign(sb.shaderIface, shader);
    sb.dirty = true;
    sb.offsetsDirty = true;
    if (stage == Stage::CS)
    {
        c.applied.computeHazardsDirty = true;
        c.applied.computeSetBound = false;
    }
    else
    {
        c.applied.pipelineDirty = true;
        c.applied.hazardsDirty = true;
        if (stage == Stage::VS)
            c.applied.vertexInputDirty = true;
    }
}

template <class T>
void GetRange(T* const* src, u32 limit, UINT start, UINT num, T** out)
{
    if (!out)
        return;
    for (UINT i = 0; i < num; ++i)
    {
        const u64 slot = u64(start) + i;
        T* p = slot < limit ? src[slot] : nullptr;
        if (p)
            p->AddRef();
        out[i] = p;
    }
}

template <class Iface>
void GetShader(const StageBindings& sb, Iface** shader, UINT* numInstances)
{
    if (shader)
    {
        *shader = static_cast<Iface*>(sb.shaderIface);
        if (*shader)
            (*shader)->AddRef();
    }
    if (numInstances)
        *numInstances = 0;
}

template <class T>
void GetInterface(T* src, T** out)
{
    if (!out)
        return;
    if (src)
        src->AddRef();
    *out = src;
}

void UnbindCsUavsOf(Context& c, Resource* resource, const SubRange& range)
{
    Resource* target = resource;
    for (ID3D11UnorderedAccessView*& slot : c.csUavs)
    {
        if (slot && Aliases(AsView(slot), target, range))
        {
            ID3D11UnorderedAccessView* old = slot;
            slot = nullptr;
            old->Release();
            c.stages[u32(Stage::CS)].dirty = true;
            c.applied.computeHazardsDirty = true;
        }
    }
}

void UnbindGraphicsOutputsOf(Context& c, Resource* resource, const SubRange& range)
{
    Resource* target = resource;
    for (ID3D11RenderTargetView*& slot : c.rtvs)
    {
        if (slot && Aliases(AsView(slot), target, range))
        {
            ID3D11RenderTargetView* old = slot;
            slot = nullptr;
            old->Release();
            c.applied.pipelineDirty = true;
            c.applied.hazardsDirty = true;
        }
    }
    for (ID3D11UnorderedAccessView*& slot : c.psUavs)
    {
        if (slot && Aliases(AsView(slot), target, range))
        {
            ID3D11UnorderedAccessView* old = slot;
            slot = nullptr;
            old->Release();
            c.stages[u32(Stage::PS)].dirty = true;
            c.applied.hazardsDirty = true;
        }
    }
}

// ---- descriptor sets ------------------------------------------------------------------------------

struct SetWriter
{
    DescriptorData data[96];
    u32 count = 0;
    u32 offsets[kMaxCBs]{};
    u32 offsetCount = 0;
};

VkImageLayout SrvLayout(Context& c, Resource* r)
{
    // A depth texture sampled while bound as a read-only DSV stays in the DSV's layout.
    if (c.dsv && (r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
    {
        View* d = AsView(c.dsv);
        if (d->resource == r)
            return DepthLayout(d);
    }
    return Context::ReadLayout(r);
}

// Fills the descriptors of `layout` from the stage's bindings (the uav array for CS/PS outputs).
void FillSet(Context& c, StageBindings& sb, const SetLayout* layout, ID3D11UnorderedAccessView* const* uavs, SetWriter& w)
{
    Device* dev = c.device();
    const NullResources& nulls = dev->nulls;
    const bool nullOk = dev->nullDescriptor;
    w.count = 0;
    w.offsetCount = 0;
    for (const SetBinding& b : layout->bindings)
    {
        const VkDescriptorType type = VkTypeOf(b.type);
        for (u32 e = 0; e < b.count; ++e)
        {
            DescriptorData& d = w.data[w.count++];
            std::memset(&d, 0, sizeof(d));
            const u32 slot = b.slot + e;
            switch (type)
            {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            {
                Resource* r = b.cls == kClassCB && sb.cb[slot] ? AsResource(sb.cb[slot]) : nullptr;
                VkBuffer buffer = r ? r->BufferHandle() : VK_NULL_HANDLE;
                if (buffer)
                {
                    const VkDeviceSize range = (std::min)(VkDeviceSize(r->byteWidth), VkDeviceSize(dev->limits.maxUniformBufferRange));
                    d.buffer = {buffer, 0, range};
                    w.offsets[w.offsetCount++] = u32(r->BufferBase());
                    sb.cbAppliedBuffer[slot] = buffer;
                    sb.cbAppliedRange[slot] = range;
                }
                else
                {
                    d.buffer = {nulls.buffer, 0, 64 * 1024};
                    w.offsets[w.offsetCount++] = 0;
                    if (b.cls == kClassCB)
                    {
                        sb.cbAppliedBuffer[slot] = VK_NULL_HANDLE;
                        sb.cbAppliedRange[slot] = 0;
                    }
                }
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            {
                SamplerState* s = b.cls == kClassSampler && sb.samplers[slot] ? static_cast<SamplerState*>(sb.samplers[slot]) : nullptr;
                d.image.sampler = s ? s->sampler : nulls.sampler;
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            {
                const bool storage = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                View* v = nullptr;
                if (!storage && b.cls == kClassSRV && sb.srv[slot])
                    v = AsView(sb.srv[slot]);
                else if (storage && b.cls == kClassUAV && uavs && uavs[slot])
                    v = AsView(uavs[slot]);
                VkImageView view = VK_NULL_HANDLE;
                if (v && v->resource->IsImage())
                {
                    view = v->ImageViewFor(VkImageViewType(b.viewType));
                    if (!view)
                        Once("~ a view does not match the dimension its shader declares; it reads as null");
                }
                if (view)
                {
                    Resource* r = v->resource;
                    d.image.imageView = view;
                    d.image.imageLayout = storage ? VK_IMAGE_LAYOUT_GENERAL : SrvLayout(c, r);
                }
                else
                {
                    d.image.imageView = nullOk ? VK_NULL_HANDLE : nulls.ImageFor(VkImageViewType(b.viewType), storage);
                    d.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                }
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            {
                const bool storage = type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                View* v = nullptr;
                if (!storage && b.cls == kClassSRV && sb.srv[slot])
                    v = AsView(sb.srv[slot]);
                else if (storage && b.cls == kClassUAV && uavs && uavs[slot])
                    v = AsView(uavs[slot]);
                if (v && v->texelView)
                    d.texel = v->texelView;
                else
                    d.texel = nullOk ? VK_NULL_HANDLE : (storage ? nulls.storageTexel : nulls.texel);
                break;
            }
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            {
                View* v = nullptr;
                if (b.cls == kClassSRV && sb.srv[slot])
                    v = AsView(sb.srv[slot]);
                else if (b.cls == kClassUAV && uavs && uavs[slot])
                    v = AsView(uavs[slot]);
                if (v && v->resource->IsBuffer())
                {
                    Resource* r = v->resource;
                    d.buffer = {r->BufferHandle(), r->BufferBase() + v->bufferOffset, v->bufferRange};
                }
                else
                    d.buffer = {nullOk ? VK_NULL_HANDLE : nulls.buffer, 0, nullOk ? VK_WHOLE_SIZE : 64 * 1024};
                break;
            }
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            {
                View* v = b.cls == kClassSRV && sb.srv[slot] ? AsView(sb.srv[slot]) : nullptr;
                d.accel = v ? v->accel : VK_NULL_HANDLE;
                break;
            }
            default:
                break;
            }
        }
    }
}

// The set of a stage: reused when this frame already wrote one with the same contents.
VkDescriptorSet WriteSet(Context& c, const SetLayout* layout, SetWriter& w)
{
    Device* dev = c.device();
    if (!layout->update)
        return dev->emptySet;
    const u64 frame = dev->frame.load(std::memory_order_acquire);
    if (c.setCacheFrame != frame)
    {
        c.setCache.clear();
        c.setCacheFrame = frame;
    }
    const u64 key = Hash64(w.data, w.count * sizeof(DescriptorData), layout->hash);
    for (auto it = c.setCache.rbegin(); it != c.setCache.rend() && it - c.setCache.rbegin() < 64; ++it)
    {
        if (it->key == key)
            return it->set;
    }
    const VkDescriptorSet set = c.descriptors.Allocate(layout, frame);
    if (!set)
        return VK_NULL_HANDLE;
    g_vk.vkUpdateDescriptorSetWithTemplate(dev->vk, set, layout->update, w.data);
    c.Stat(Device::StatSets);
    if (c.setCache.size() >= 4096)
        c.setCache.erase(c.setCache.begin(), c.setCache.begin() + 1024);
    c.setCache.push_back({key, set});
    return set;
}

// Whether a stage's constant buffers moved: a new buffer means a new set, a new offset only a rebind.
void CheckConstantBuffers(StageBindings& sb)
{
    if (!sb.shader || sb.dirty)
        return;
    for (u32 m = sb.shader->iface.cbMask; m; m &= m - 1)
    {
        const u32 slot = u32(std::countr_zero(m));
        Resource* r = sb.cb[slot] ? AsResource(sb.cb[slot]) : nullptr;
        const VkBuffer buffer = r ? r->BufferHandle() : VK_NULL_HANDLE;
        if (buffer != sb.cbAppliedBuffer[slot])
        {
            sb.dirty = true;
            return;
        }
    }
    sb.offsetsDirty = true;
}
} // namespace

// ================================================================================================
// Layout policy
// ================================================================================================

VkImageLayout Context::ReadLayout(const Resource* r)
{
    return (r->bindFlags & D3D11_BIND_UNORDERED_ACCESS) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
}

VkImageLayout Context::TransferLayout(const Resource* r, bool dst)
{
    if (r->bindFlags & D3D11_BIND_UNORDERED_ACCESS)
        return VK_IMAGE_LAYOUT_GENERAL;
    return dst ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

// ================================================================================================
// Hazards
// ================================================================================================

void Context::UnbindSrvsOf(Resource* resource, const SubRange& range)
{
    Resource* target = resource;
    for (u32 s = 0; s < kStageCount; ++s)
    {
        StageBindings& sb = stages[s];
        for (ID3D11ShaderResourceView*& slot : sb.srv)
        {
            if (slot && Aliases(AsView(slot), target, range))
            {
                ID3D11ShaderResourceView* old = slot;
                slot = nullptr;
                old->Release();
                sb.dirty = true;
            }
        }
    }
    applied.hazardsDirty = true;
    applied.computeHazardsDirty = true;
}

bool Context::IsBoundAsOutput(Resource* resource, const SubRange& range) const
{
    Resource* target = resource;
    const u32 n = (std::min)(numRTVs, kMaxRTVs);
    for (u32 i = 0; i < n; ++i)
    {
        if (rtvs[i] && Aliases(AsView(rtvs[i]), target, range))
            return true;
    }
    if (dsv)
    {
        View* v = AsView(dsv);
        if (DsvWrites(v) && v->resource == target && Overlaps(v->range, range))
            return true;
    }
    for (ID3D11UnorderedAccessView* uav : psUavs)
    {
        if (uav && Aliases(AsView(uav), target, range))
            return true;
    }
    for (ID3D11UnorderedAccessView* uav : csUavs)
    {
        if (uav && Aliases(AsView(uav), target, range))
            return true;
    }
    return false;
}

void Context::ReleaseBindings()
{
    for (StageBindings& sb : stages)
    {
        for (ID3D11Buffer*& b : sb.cb)
            Assign(b, static_cast<ID3D11Buffer*>(nullptr));
        for (ID3D11ShaderResourceView*& v : sb.srv)
            Assign(v, static_cast<ID3D11ShaderResourceView*>(nullptr));
        for (ID3D11SamplerState*& s : sb.samplers)
            Assign(s, static_cast<ID3D11SamplerState*>(nullptr));
        Assign(sb.shaderIface, static_cast<ID3D11DeviceChild*>(nullptr));
        sb.shader = nullptr;
        sb.dirty = true;
        sb.offsetsDirty = true;
    }
    Assign(inputLayout, static_cast<ID3D11InputLayout*>(nullptr));
    for (VertexBufferBinding& vb : vbs)
    {
        Assign(vb.buffer, static_cast<ID3D11Buffer*>(nullptr));
        vb.stride = vb.offset = 0;
    }
    Assign(indexBuffer, static_cast<ID3D11Buffer*>(nullptr));
    indexFormat = DXGI_FORMAT_R16_UINT;
    indexOffset = 0;
    topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Assign(blendState, static_cast<ID3D11BlendState*>(nullptr));
    blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3] = 1.f;
    sampleMask = 0xffffffff;
    Assign(depthState, static_cast<ID3D11DepthStencilState*>(nullptr));
    stencilRef = 0;
    Assign(rasterState, static_cast<ID3D11RasterizerState*>(nullptr));
    numViewports = numScissors = 0;
    for (ID3D11RenderTargetView*& r : rtvs)
        Assign(r, static_cast<ID3D11RenderTargetView*>(nullptr));
    numRTVs = 0;
    Assign(dsv, static_cast<ID3D11DepthStencilView*>(nullptr));
    for (ID3D11UnorderedAccessView*& u : csUavs)
        Assign(u, static_cast<ID3D11UnorderedAccessView*>(nullptr));
    for (ID3D11UnorderedAccessView*& u : psUavs)
        Assign(u, static_cast<ID3D11UnorderedAccessView*>(nullptr));
    shadingRate = 0;
    shadingRateImage = false;
    rateCombiner = 0;
    Assign(rateImage, static_cast<ID3D11Resource*>(nullptr));
    uavOverlap = 0;
    m_targetsDirty = true;
}

// ================================================================================================
// Draw and dispatch preparation
// ================================================================================================

bool Context::PrepareDraw(bool indexed, bool mesh)
{
    if (m_device->lost)
        return false;
    StageBindings& vsb = stages[u32(Stage::VS)];
    Shader* vs = vsb.shader;
    if (!vs || vs->mesh != mesh)
    {
        if (!vs)
            Once("~ a draw without a vertex shader was skipped");
        return false;
    }
    if (!numViewports)
    {
        Once("~ a draw without a viewport was skipped (D3D11 rasterizes nothing)");
        return false;
    }
    if (!Cmd())
        return false;
    Device* dev = m_device;

    // ---- targets: a change ends the rendering ----
    if (m_targetsDirty)
    {
        const RenderingKey key = CurrentTargets();
        if (m_rendering && !(key == m_renderingKey))
            EndRendering();
        m_targetsDirty = false;
    }

    // ---- clears of views that are not this rendering's attachments run first ----
    if (!pendingClears.empty())
    {
        bool foreign = false;
        for (const PendingClear& pc : pendingClears)
        {

            const View* v = pc.view;
            bool isTarget = v == (dsv ? AsView(dsv) : nullptr);
            for (u32 i = 0; i < numRTVs && !isTarget; ++i)
                isTarget = rtvs[i] && AsView(rtvs[i]) == v;
            foreign |= !isTarget;
        }
        if (foreign || m_rendering)
        {
            // Inside a rendering the attachment clears become vkCmdClearAttachments (FlushPendingClears
            // knows); outside, everything is cleared now except what the next rendering folds in.
            if (foreign)
                EndRendering();
            FlushPendingClears(nullptr);
        }
    }

    // ---- requirements of everything the draw reads and writes ----
    if (applied.hazardsDirty || !m_rendering)
    {
        // Attachments only when a rendering begins: draws inside one keep rasterization order.
        if (!m_rendering)
        {
            for (u32 i = 0; i < numRTVs; ++i)
            {
                if (!rtvs[i])
                    continue;
                View* v = AsView(rtvs[i]);
                Resource* r = v->resource;
                const VkImageLayout layout = (r->bindFlags & D3D11_BIND_UNORDERED_ACCESS) ? VK_IMAGE_LAYOUT_GENERAL :
                                                                                             VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
                RequireImage(r, v->range, layout, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            }
            if (dsv)
            {
                View* v = AsView(dsv);
                VkAccessFlags2 access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                if (DsvWrites(v))
                    access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                RequireImage(v->resource, v->range, DepthLayout(v),
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, access);
            }
            if (shadingRateImage && rateImage && dev->caps.shadingRate)
            {
                Resource* r = Resource::From(rateImage);
                if (r && r->IsImage() && (r->imageInfo.usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR))
                    RequireImageAll(r, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
                        VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
            }
        }
        const Stage graphics[5] = {Stage::VS, Stage::HS, Stage::DS, Stage::GS, Stage::PS};
        for (Stage s : graphics)
        {
            StageBindings& sb = stages[u32(s)];
            if (!sb.shader)
                continue;
            const VkPipelineStageFlags2 mask = StageMask(s, mesh);
            for (u32 m = sb.shader->iface.srvMask; m; m &= m - 1)
            {
                const u32 slot = u32(std::countr_zero(m));
                if (!sb.srv[slot])
                    continue;
                View* v = AsView(sb.srv[slot]);
                Resource* r = v->resource;
                if (r->IsBuffer())
                {
                    if (r->gpuWritten)
                        RequireBuffer(r, mask, VK_ACCESS_2_SHADER_READ_BIT);
                }
                else
                    RequireImage(r, v->range, SrvLayout(*this, r), mask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        }
        if (Shader* ps = stages[u32(Stage::PS)].shader)
        {
            for (u32 m = ps->iface.uavMask; m; m &= m - 1)
            {
                const u32 slot = u32(std::countr_zero(m));
                if (!psUavs[slot])
                    continue;
                View* v = AsView(psUavs[slot]);
                Resource* r = v->resource;
                constexpr VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                // Inside BeginUavOverlap draws do not wait for each other's UAV writes.
                if (uavOverlap && m_rendering)
                    continue;
                if (r->IsBuffer())
                    RequireBuffer(r, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, rw);
                else
                    RequireImage(r, v->range, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, rw);
            }
        }
        if (!mesh)
        {
            for (const VertexBufferBinding& vb : vbs)
            {
                if (vb.buffer && AsResource(vb.buffer)->gpuWritten)
                    RequireBuffer(AsResource(vb.buffer), VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
            }
            if (indexed && indexBuffer && AsResource(indexBuffer)->gpuWritten)
                RequireBuffer(AsResource(indexBuffer), VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
        }
        applied.hazardsDirty = false;
    }
    if (BarriersPending())
    {
        // Barriers cannot go inside a rendering: it ends and begins again (LOAD keeps the contents).
        if (m_rendering)
            EndRendering();
        FlushBarriers();
    }
    if (!m_rendering && !BeginRendering())
        return false;

    // ---- pipeline ----
    u32 patchPoints = 0;
    const VkPrimitiveTopology vkTopology = mesh ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VkTopology(topology, patchPoints);
    if (!mesh && vkTopology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
    {
        Once("~ a draw without a primitive topology was skipped");
        return false;
    }
    if (!mesh && TopologyClass(vkTopology) != TopologyClass(applied.vkTopology) && !dev->topologyUnrestricted)
        applied.pipelineDirty = true;
    if (!mesh && patchPoints != applied.patchPoints && !dev->patchControlPointsDynamic)
        applied.pipelineDirty = true;
    if (applied.pipelineDirty || applied.pipelineGeneration != dev->pipelines.Generation() || applied.meshPipeline != mesh)
    {
        GraphicsPipelineDesc d;
        d.vs = vs;
        d.hs = stages[u32(Stage::HS)].shader;
        d.ds = stages[u32(Stage::DS)].shader;
        d.gs = stages[u32(Stage::GS)].shader;
        d.ps = stages[u32(Stage::PS)].shader;
        if (!dev->vertexInputDynamic && !mesh)
            d.inputLayout = inputLayout ? static_cast<InputLayout*>(inputLayout) : nullptr;
        if (!dev->eds3Blend)
            d.blend = blendState ? static_cast<BlendState*>(blendState) : nullptr;
        if (!(dev->eds3Polygon && dev->eds3DepthClamp))
            d.raster = rasterState ? static_cast<RasterizerState*>(rasterState) : nullptr;
        d.sampleMask = dev->eds3SampleMask ? 0xffffffff : sampleMask;
        d.alphaToCoverage = !dev->eds3AlphaToCoverage && blendState && static_cast<BlendState*>(blendState)->alphaToCoverage;
        d.topologyClass = dev->topologyUnrestricted ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : TopologyClass(vkTopology);
        if (mesh)
            d.topologyClass = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        d.patchPoints = dev->patchControlPointsDynamic ? (vkTopology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? 3 : 0) : patchPoints;
        if (d.hs && !dev->patchControlPointsDynamic)
            d.patchPoints = patchPoints;
        d.numRTVs = numRTVs;
        u32 samples = 1;
        for (u32 i = 0; i < numRTVs; ++i)
        {
            if (rtvs[i])
            {
                View* v = AsView(rtvs[i]);
                d.rtvFormats[i] = v->viewInfo.format ? v->viewInfo.format : v->resource->imageInfo.format;
                samples = v->resource->samples;
            }
            else
                d.rtvFormats[i] = VK_FORMAT_UNDEFINED;
        }
        if (dsv)
        {
            View* v = AsView(dsv);
            Resource* r = v->resource;
            if (r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
                d.depthFormat = r->imageInfo.format;
            if (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
                d.stencilFormat = r->imageInfo.format;
            samples = r->samples;
        }
        d.samples = dev->eds3Samples ? 1 : samples;
        d.shadingRate = m_renderingKey.rate != VK_NULL_HANDLE;
        // The pipeline layout from the stages' set layouts.
        const SetLayout* sets[kGraphicsStages]{};
        const Stage order[kGraphicsStages] = {Stage::VS, Stage::PS, Stage::GS, Stage::HS, Stage::DS};
        for (u32 i = 0; i < kGraphicsStages; ++i)
        {
            const Shader* s = stages[u32(order[i])].shader;
            sets[i] = s ? s->setLayout : nullptr;
        }
        const PipelineLayout* layout = dev->layouts.Graphics(sets);
        if (!layout)
            return false;
        const u64 hash = HashCombine(d.Hash(), layout->hash);
        const VkPipeline pipeline = dev->pipelines.Graphics(d, hash, layout);
        if (!pipeline)
            return false;
        if (pipeline != applied.pipeline)
        {
            g_vk.vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            Stat(Device::StatPipelineBinds);
            applied.pipeline = pipeline;
            // Mesh and vertex pipelines differ in what they make dynamic.
            if (applied.meshPipeline != mesh)
            {
                applied.vertexInputDirty = true;
                applied.topologyDirty = true;
            }
        }
        if (layout != applied.layout)
        {
            applied.layout = layout;
            applied.setsBound = false;
        }
        applied.pipelineHash = hash;
        applied.pipelineGeneration = dev->pipelines.Generation();
        applied.meshPipeline = mesh;
        applied.pipelineDirty = false;
    }

    // ---- dynamic state ----
    const RasterizerState* rs = rasterState ? static_cast<RasterizerState*>(rasterState) : DefaultRaster();
    if (applied.viewportDirty)
    {
        VkViewport vps[kMaxViewports];
        VkRect2D rects[kMaxViewports];
        const u32 n = numViewports;
        for (u32 i = 0; i < n; ++i)
        {
            const D3D11_VIEWPORT& v = viewports[i];
            // Negative height: D3D's y-down viewport with D3D's clip space (no shader flip).
            float w = v.Width, h = v.Height;
            bool empty = w <= 0.f || h <= 0.f;
            if (empty)
                w = h = 1.f;
            vps[i] = {v.TopLeftX, v.TopLeftY + h, w, -h, std::clamp(v.MinDepth, 0.f, 1.f), std::clamp(v.MaxDepth, 0.f, 1.f)};
            if (rs->scissor)
            {
                const D3D11_RECT r = i < numScissors ? scissors[i] : D3D11_RECT{0, 0, 0, 0};
                const LONG left = (std::max)(LONG(0), r.left), top = (std::max)(LONG(0), r.top);
                rects[i] = {{left, top}, {u32((std::max)(LONG(0), r.right - left)), u32((std::max)(LONG(0), r.bottom - top))}};
            }
            else
                rects[i] = {{0, 0}, {32767u, 32767u}};
            if (empty)
                rects[i] = {{0, 0}, {0, 0}};
        }
        g_vk.vkCmdSetViewportWithCount(m_cmd, n, vps);
        g_vk.vkCmdSetScissorWithCount(m_cmd, n, rects);
        applied.viewportDirty = false;
    }
    if (applied.rasterDirty)
    {
        g_vk.vkCmdSetCullMode(m_cmd, rs->cull);
        g_vk.vkCmdSetFrontFace(m_cmd, rs->front);
        // Dynamic in every pipeline (D3D11 has no rasterizer discard): off, but it must be set.
        g_vk.vkCmdSetRasterizerDiscardEnable(m_cmd, VK_FALSE);
        g_vk.vkCmdSetDepthBiasEnable(m_cmd, rs->depthBias ? VK_TRUE : VK_FALSE);
        if (rs->depthBias)
            g_vk.vkCmdSetDepthBias(m_cmd, float(rs->desc11.DepthBias), dev->depthBiasClamp ? rs->desc11.DepthBiasClamp : 0.f,
                rs->desc11.SlopeScaledDepthBias);
        if (dev->eds3Polygon)
            g_vk.vkCmdSetPolygonModeEXT(m_cmd, rs->polygon);
        if (dev->eds3DepthClamp)
            g_vk.vkCmdSetDepthClampEnableEXT(m_cmd, rs->depthClamp ? VK_TRUE : VK_FALSE);
        if (dev->eds3DepthClip)
            g_vk.vkCmdSetDepthClipEnableEXT(m_cmd, rs->depthClamp ? VK_FALSE : VK_TRUE);
        applied.rasterDirty = false;
    }
    if (applied.depthDirty)
    {
        const DepthStencilState* ds = depthState ? static_cast<DepthStencilState*>(depthState) : nullptr;
        static const DepthDefaults defaults;
        const bool hasDepth = dsv != nullptr;
        const bool test = (ds ? ds->depthTest : defaults.depthTest) && hasDepth;
        g_vk.vkCmdSetDepthTestEnable(m_cmd, test ? VK_TRUE : VK_FALSE);
        g_vk.vkCmdSetDepthWriteEnable(m_cmd, test && (ds ? ds->depthWrite : defaults.depthWrite) ? VK_TRUE : VK_FALSE);
        g_vk.vkCmdSetDepthCompareOp(m_cmd, ds ? ds->depthCompare : defaults.compare);
        g_vk.vkCmdSetDepthBoundsTestEnable(m_cmd, VK_FALSE);
        const bool stencil = ds && ds->stencilTest && hasDepth;
        g_vk.vkCmdSetStencilTestEnable(m_cmd, stencil ? VK_TRUE : VK_FALSE);
        const VkStencilOpState& front = ds ? ds->front : defaults.face;
        const VkStencilOpState& back = ds ? ds->back : defaults.face;
        g_vk.vkCmdSetStencilOp(m_cmd, VK_STENCIL_FACE_FRONT_BIT, front.failOp, front.passOp, front.depthFailOp, front.compareOp);
        g_vk.vkCmdSetStencilOp(m_cmd, VK_STENCIL_FACE_BACK_BIT, back.failOp, back.passOp, back.depthFailOp, back.compareOp);
        g_vk.vkCmdSetStencilCompareMask(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, front.compareMask);
        g_vk.vkCmdSetStencilWriteMask(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, front.writeMask);
        applied.depthDirty = false;
    }
    if (applied.stencilRefDirty)
    {
        g_vk.vkCmdSetStencilReference(m_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilRef & 0xFF);
        applied.stencilRefDirty = false;
    }
    if (applied.blendFactorDirty)
    {
        g_vk.vkCmdSetBlendConstants(m_cmd, blendFactor);
        applied.blendFactorDirty = false;
    }
    if (applied.blendDirty && dev->eds3Blend && numRTVs)
    {
        const BlendState* bs = blendState ? static_cast<BlendState*>(blendState) : nullptr;
        VkBool32 enables[kMaxRTVs];
        VkColorBlendEquationEXT equations[kMaxRTVs];
        VkColorComponentFlags masks[kMaxRTVs];
        for (u32 i = 0; i < numRTVs; ++i)
        {
            if (bs)
            {
                const VkPipelineColorBlendAttachmentState& a = bs->attachments[i];
                enables[i] = a.blendEnable;
                equations[i] = {a.srcColorBlendFactor, a.dstColorBlendFactor, a.colorBlendOp, a.srcAlphaBlendFactor,
                    a.dstAlphaBlendFactor, a.alphaBlendOp};
                masks[i] = a.colorWriteMask;
            }
            else
            {
                enables[i] = VK_FALSE;
                equations[i] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                    VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
                masks[i] = 0xF;
            }
        }
        g_vk.vkCmdSetColorBlendEnableEXT(m_cmd, 0, numRTVs, enables);
        g_vk.vkCmdSetColorBlendEquationEXT(m_cmd, 0, numRTVs, equations);
        g_vk.vkCmdSetColorWriteMaskEXT(m_cmd, 0, numRTVs, masks);
        applied.blendDirty = false;
    }
    if (applied.multisampleDirty)
    {
        u32 samples = 1;
        for (u32 i = 0; i < numRTVs; ++i)
        {
            if (rtvs[i])
                samples = AsView(rtvs[i])->resource->samples;
        }
        if (dsv)
            samples = AsView(dsv)->resource->samples;
        if (dev->eds3Samples)
            g_vk.vkCmdSetRasterizationSamplesEXT(m_cmd, VkSampleCountFlagBits(samples));
        if (dev->eds3SampleMask)
        {
            const VkSampleMask mask = sampleMask;
            g_vk.vkCmdSetSampleMaskEXT(m_cmd, VkSampleCountFlagBits(samples), &mask);
        }
        if (dev->eds3AlphaToCoverage)
        {
            const bool a2c = blendState && static_cast<BlendState*>(blendState)->alphaToCoverage;
            g_vk.vkCmdSetAlphaToCoverageEnableEXT(m_cmd, a2c ? VK_TRUE : VK_FALSE);
        }
        applied.multisampleDirty = false;
    }
    if (!mesh)
    {
        const bool restart = indexed && IsStrip(vkTopology);
        if (applied.topologyDirty || vkTopology != applied.vkTopology || restart != applied.restart)
        {
            g_vk.vkCmdSetPrimitiveTopology(m_cmd, vkTopology);
            g_vk.vkCmdSetPrimitiveRestartEnable(m_cmd, restart ? VK_TRUE : VK_FALSE);
            applied.vkTopology = vkTopology;
            applied.restart = restart;
        }
        if (dev->patchControlPointsDynamic && vkTopology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
            (applied.topologyDirty || patchPoints != applied.patchPoints))
            g_vk.vkCmdSetPatchControlPointsEXT(m_cmd, patchPoints);
        applied.patchPoints = patchPoints;
        applied.topologyDirty = false;
    }
    if (applied.rateDirty && dev->caps.shadingRate)
    {
        // D3D12 rate values: log2(width) << 2 | log2(height), the same encoding as a rate image.
        const VkExtent2D size{1u << ((shadingRate >> 2) & 3), 1u << (shadingRate & 3)};
        // D3D12 combiners map 1:1 (SUM of log2 rates is MUL); without non-trivial ops the image replaces.
        VkFragmentShadingRateCombinerOpKHR image = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        if (m_renderingKey.rate)
        {
            static constexpr VkFragmentShadingRateCombinerOpKHR kOps[] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR,
                VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR};
            image = rateCombiner < std::size(kOps) ? kOps[rateCombiner] : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
            if (image > VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR &&
                !dev->shadingRateProperties.fragmentShadingRateNonTrivialCombinerOps)
                image = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
        }
        const VkFragmentShadingRateCombinerOpKHR ops[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, image};
        g_vk.vkCmdSetFragmentShadingRateKHR(m_cmd, &size, ops);
        applied.rateDirty = false;
    }

    // ---- vertex input ----
    if (!mesh)
    {
        InputLayout* il = inputLayout ? static_cast<InputLayout*>(inputLayout) : nullptr;
        if (dev->vertexInputDynamic)
        {
            u64 key = HashCombine(il ? il->hash : 0, vs->hash);
            for (u32 i = 0; i < kMaxVBs; ++i)
            {
                if (il && (il->slotMask & (1u << i)))
                    key = HashCombine(key, vbs[i].stride);
            }
            if (applied.vertexInputDirty || key != applied.vertexInputKey)
            {
                VkVertexInputBindingDescription2EXT bindings[kMaxVBs];
                VkVertexInputAttributeDescription2EXT attributes[32];
                u32 bindingCount = 0, attributeCount = 0, used = 0;
                if (il)
                {
                    for (const ShaderInterface::Var& in : vs->iface.inputs)
                    {
                        for (const InputLayout::Element& e : il->elements)
                        {
                            if (e.index != in.index || e.semantic != in.semantic)
                                continue;
                            if (attributeCount < 32)
                            {
                                attributes[attributeCount++] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                    nullptr, in.location, e.slot, e.format, e.offset};
                                used |= 1u << e.slot;
                            }
                            break;
                        }
                    }
                    for (u32 slot = 0; slot < kMaxVBs; ++slot)
                    {
                        if (!(used & (1u << slot)))
                            continue;
                        bindings[bindingCount++] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, slot,
                            vbs[slot].stride, il->slotInstance[slot] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
                    }
                }
                g_vk.vkCmdSetVertexInputEXT(m_cmd, bindingCount, bindings, attributeCount, attributes);
                applied.vertexInputKey = key;
                applied.vertexInputDirty = false;
            }
        }
        // Vertex buffers (versions of dynamic buffers move their offset).
        VkBuffer buffers[kMaxVBs];
        VkDeviceSize offsets[kMaxVBs];
        VkDeviceSize strides[kMaxVBs];
        u32 count = 0;
        bool changed = applied.vbDirty;
        for (u32 i = 0; i < kMaxVBs; ++i)
        {
            const VertexBufferBinding& vb = vbs[i];
            Resource* r = vb.buffer ? AsResource(vb.buffer) : nullptr;
            buffers[i] = r ? r->BufferHandle() : VK_NULL_HANDLE;
            offsets[i] = r ? r->BufferBase() + vb.offset : 0;
            strides[i] = vb.stride;
            if (!buffers[i])
            {
                buffers[i] = dev->nullDescriptor ? VK_NULL_HANDLE : dev->nulls.buffer;
                offsets[i] = 0;
            }
            if (vb.buffer)
                count = i + 1;
            changed |= buffers[i] != applied.vbBuffers[i] || offsets[i] != applied.vbOffsets[i];
        }
        if (changed && count)
        {
            g_vk.vkCmdBindVertexBuffers2(m_cmd, 0, count, buffers, offsets, nullptr,
                dev->vertexInputDynamic ? nullptr : strides);
            std::memcpy(applied.vbBuffers, buffers, sizeof(buffers));
            std::memcpy(applied.vbOffsets, offsets, sizeof(offsets));
        }
        applied.vbDirty = false;
        if (indexed)
        {
            Resource* r = indexBuffer ? AsResource(indexBuffer) : nullptr;
            if (!r)
            {
                Once("~ an indexed draw without an index buffer was skipped");
                return false;
            }
            const VkBuffer buffer = r->BufferHandle();
            const VkDeviceSize offset = r->BufferBase() + indexOffset;
            if (applied.ibDirty || buffer != applied.ibBuffer || offset != applied.ibOffset)
            {
                g_vk.vkCmdBindIndexBuffer(m_cmd, buffer, offset,
                    indexFormat == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                applied.ibBuffer = buffer;
                applied.ibOffset = offset;
                applied.ibDirty = false;
            }
        }
    }

    // ---- descriptor sets: one bind call for every set that changed ----
    const PipelineLayout* layout = applied.layout;
    const Stage order[kGraphicsStages] = {Stage::VS, Stage::PS, Stage::GS, Stage::HS, Stage::DS};
    u32 lo = kGraphicsStages, hi = 0;
    for (u32 i = 0; i < layout->setCount; ++i)
    {
        StageBindings& sb = stages[u32(order[i])];
        if (!sb.shader)
        {
            if (!applied.setsBound || applied.sets[i] != dev->emptySet)
            {
                applied.sets[i] = dev->emptySet;
                sb.dynamicCount = 0;
                lo = (std::min)(lo, i);
                hi = (std::max)(hi, i + 1);
            }
            continue;
        }
        CheckConstantBuffers(sb);
        if (sb.dirty || !applied.setsBound)
        {
            SetWriter w;
            FillSet(*this, sb, sb.shader->setLayout, order[i] == Stage::PS ? psUavs : nullptr, w);
            const VkDescriptorSet set = WriteSet(*this, sb.shader->setLayout, w);
            if (!set)
                return false;
            sb.set = set;
            std::memcpy(sb.dynamicOffsets, w.offsets, sizeof(u32) * w.offsetCount);
            sb.dynamicCount = w.offsetCount;
            sb.dirty = false;
            sb.offsetsDirty = false;
            applied.sets[i] = set;
            lo = (std::min)(lo, i);
            hi = (std::max)(hi, i + 1);
        }
        else if (sb.offsetsDirty)
        {
            // Same buffers, new versions: only the dynamic offsets change.
            u32 n = 0;
            bool moved = false;
            for (const SetBinding& b : sb.shader->setLayout->bindings)
            {
                if (VkTypeOf(b.type) != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                    continue;
                for (u32 e = 0; e < b.count; ++e, ++n)
                {
                    const u32 slot = b.slot + e;
                    Resource* r = b.cls == kClassCB && sb.cb[slot] ? AsResource(sb.cb[slot]) : nullptr;
                    const u32 offset = r ? u32(r->BufferBase()) : 0;
                    moved |= offset != sb.dynamicOffsets[n];
                    sb.dynamicOffsets[n] = offset;
                }
            }
            sb.offsetsDirty = false;
            if (moved)
            {
                Stat(Device::StatOffsetsOnly);
                lo = (std::min)(lo, i);
                hi = (std::max)(hi, i + 1);
            }
        }
    }
    if (lo < hi)
    {
        u32 offsets[kMaxCBs * kGraphicsStages];
        u32 offsetCount = 0;
        for (u32 i = lo; i < hi; ++i)
        {
            const StageBindings& sb = stages[u32(order[i])];
            if (!sb.shader)
                continue;
            std::memcpy(offsets + offsetCount, sb.dynamicOffsets, sizeof(u32) * sb.dynamicCount);
            offsetCount += sb.dynamicCount;
        }
        g_vk.vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, lo, hi - lo,
            applied.sets + lo, offsetCount, offsets);
        applied.setsBound = true;
    }

    // Draws with PS UAVs: the next draw or dispatch reads after them.
    if (const Shader* ps = stages[u32(Stage::PS)].shader; ps && ps->iface.uavMask)
    {
        applied.uavWritten = true;
        applied.hazardsDirty = true;
        applied.computeHazardsDirty = true;
    }
    Stat(Device::StatDraws);
    return true;
}

bool Context::PrepareDispatch()
{
    if (m_device->lost)
        return false;
    StageBindings& sb = stages[u32(Stage::CS)];
    Shader* cs = sb.shader;
    if (!cs)
    {
        Once("~ a dispatch without a compute shader was skipped");
        return false;
    }
    if (!Cmd())
        return false;
    Device* dev = m_device;
    EndRendering();
    if (!pendingClears.empty())
        FlushPendingClears(nullptr);

    if (applied.computeHazardsDirty)
    {
        for (u32 m = cs->iface.srvMask; m; m &= m - 1)
        {
            const u32 slot = u32(std::countr_zero(m));
            if (!sb.srv[slot])
                continue;
            View* v = AsView(sb.srv[slot]);
            Resource* r = v->resource;
            if (r->IsBuffer())
            {
                if (r->gpuWritten)
                    RequireBuffer(r, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            }
            else
                RequireImage(r, v->range, ReadLayout(r), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        for (u32 m = cs->iface.uavMask; m; m &= m - 1)
        {
            const u32 slot = u32(std::countr_zero(m));
            if (!csUavs[slot])
                continue;
            View* v = AsView(csUavs[slot]);
            Resource* r = v->resource;
            constexpr VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            if (r->IsBuffer())
                RequireBuffer(r, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
            else
                RequireImage(r, v->range, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
        }
        // Without UAVs nothing a later dispatch reads changed: the next one may skip this pass.
        applied.computeHazardsDirty = cs->iface.uavMask != 0 && !uavOverlap;
    }
    FlushBarriers();

    if (!cs->computeLayout)
        return false;
    const VkPipeline pipeline = dev->pipelines.Compute(cs, cs->computeLayout);
    if (!pipeline)
        return false;
    if (pipeline != applied.computePipeline)
    {
        g_vk.vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        applied.computePipeline = pipeline;
        if (applied.computeLayout != cs->computeLayout)
        {
            applied.computeLayout = cs->computeLayout;
            applied.computeSetBound = false;
            applied.computeBindlessBound = false;
            applied.computePushDirty = true;
        }
    }
    CheckConstantBuffers(sb);
    bool bind = !applied.computeSetBound;
    if (sb.dirty || !applied.computeSetBound)
    {
        SetWriter w;
        FillSet(*this, sb, cs->setLayout, csUavs, w);
        const VkDescriptorSet set = WriteSet(*this, cs->setLayout, w);
        if (!set)
            return false;
        sb.set = set;
        std::memcpy(sb.dynamicOffsets, w.offsets, sizeof(u32) * w.offsetCount);
        sb.dynamicCount = w.offsetCount;
        sb.dirty = false;
        sb.offsetsDirty = false;
        bind = true;
    }
    else if (sb.offsetsDirty)
    {
        u32 n = 0;
        for (const SetBinding& b : cs->setLayout->bindings)
        {
            if (VkTypeOf(b.type) != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                continue;
            for (u32 e = 0; e < b.count; ++e, ++n)
            {
                Resource* r = b.cls == kClassCB && sb.cb[b.slot + e] ? AsResource(sb.cb[b.slot + e]) : nullptr;
                const u32 offset = r ? u32(r->BufferBase()) : 0;
                bind |= offset != sb.dynamicOffsets[n];
                sb.dynamicOffsets[n] = offset;
            }
        }
        sb.offsetsDirty = false;
    }
    if (bind)
    {
        g_vk.vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs->computeLayout->layout, 0, 1, &sb.set,
            sb.dynamicCount, sb.dynamicOffsets);
        applied.computeSet = sb.set;
        applied.computeSetBound = true;
    }
    // The RT lane: the device's bindless set and the push constants (vk_raytracing.cpp).
    if (cs->iface.bindless && !applied.computeBindlessBound && dev->bindless.set)
    {
        g_vk.vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs->computeLayout->layout, kComputeBindlessSet, 1,
            &dev->bindless.set, 0, nullptr);
        applied.computeBindlessBound = true;
    }
    if (cs->iface.pushConstants && applied.computePushDirty)
    {
        g_vk.vkCmdPushConstants(m_cmd, cs->computeLayout->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, cs->iface.pushConstants,
            computePush);
        applied.computePushDirty = false;
    }
    if (cs->iface.uavMask)
    {
        applied.uavWritten = true;
        applied.hazardsDirty = true;
    }
    Stat(Device::StatDispatches);
    return true;
}

// ================================================================================================
// Draws and dispatches
// ================================================================================================

void STDMETHODCALLTYPE Context::DrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex)
{
    if (indexCount && PrepareDraw(true))
        g_vk.vkCmdDrawIndexed(m_cmd, indexCount, 1, startIndex, baseVertex, 0);
}

void STDMETHODCALLTYPE Context::Draw(UINT vertexCount, UINT startVertex)
{
    if (vertexCount && PrepareDraw(false))
        g_vk.vkCmdDraw(m_cmd, vertexCount, 1, startVertex, 0);
}

void STDMETHODCALLTYPE Context::DrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex,
    INT baseVertex, UINT startInstance)
{
    if (indexCountPerInstance && instanceCount && PrepareDraw(true))
        g_vk.vkCmdDrawIndexed(m_cmd, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
}

void STDMETHODCALLTYPE Context::DrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex,
    UINT startInstance)
{
    if (vertexCountPerInstance && instanceCount && PrepareDraw(false))
        g_vk.vkCmdDraw(m_cmd, vertexCountPerInstance, instanceCount, startVertex, startInstance);
}

void STDMETHODCALLTYPE Context::Dispatch(UINT x, UINT y, UINT z)
{
    if (x && y && z && PrepareDispatch())
        g_vk.vkCmdDispatch(m_cmd, x, y, z);
}

void STDMETHODCALLTYPE Context::DrawAuto()
{
    Once("! DrawAuto (stream output) is not supported; the draw is skipped");
}

namespace
{
// The bytes of an argument buffer and the barrier that orders the writes that produced them.
bool ArgumentBuffer(Context& c, ID3D11Buffer* args, UINT offset, UINT bytes, VkBuffer& buffer, VkDeviceSize& at)
{
    if (!args)
        return false;
    Resource* r = AsResource(args);
    if (u64(offset) + bytes > r->byteWidth || (offset & 3))
        return false;
    buffer = r->BufferHandle();
    at = r->BufferBase() + offset;
    if (r->gpuWritten)
        c.RequireBuffer(r, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    return buffer != VK_NULL_HANDLE;
}
} // namespace

void STDMETHODCALLTYPE Context::DrawIndexedInstancedIndirect(ID3D11Buffer* args, UINT offset)
{
    VkBuffer buffer;
    VkDeviceSize at;
    if (!Cmd() || !ArgumentBuffer(*this, args, offset, 20, buffer, at))
        return;
    // A barrier for the arguments ends the rendering inside PrepareDraw.
    applied.hazardsDirty = true;
    if (PrepareDraw(true))
        g_vk.vkCmdDrawIndexedIndirect(m_cmd, buffer, at, 1, 20);
}

void STDMETHODCALLTYPE Context::DrawInstancedIndirect(ID3D11Buffer* args, UINT offset)
{
    VkBuffer buffer;
    VkDeviceSize at;
    if (!Cmd() || !ArgumentBuffer(*this, args, offset, 16, buffer, at))
        return;
    applied.hazardsDirty = true;
    if (PrepareDraw(false))
        g_vk.vkCmdDrawIndirect(m_cmd, buffer, at, 1, 16);
}

void STDMETHODCALLTYPE Context::DispatchIndirect(ID3D11Buffer* args, UINT offset)
{
    VkBuffer buffer;
    VkDeviceSize at;
    if (!Cmd() || !ArgumentBuffer(*this, args, offset, 12, buffer, at))
        return;
    if (PrepareDispatch())
        g_vk.vkCmdDispatchIndirect(m_cmd, buffer, at);
}

void Context::IndirectCount(bool indexed, ID3D11Buffer* args, UINT offset, UINT maxCount, ID3D11Buffer* count, UINT countOffset)
{
    if (!maxCount)
        return;
    const u32 stride = indexed ? 20 : 16;
    VkBuffer buffer, countBuffer = VK_NULL_HANDLE;
    VkDeviceSize at, countAt = 0;
    if (!Cmd() || !ArgumentBuffer(*this, args, offset, stride * maxCount, buffer, at))
        return;
    if (count && !ArgumentBuffer(*this, count, countOffset, 4, countBuffer, countAt))
        return;
    applied.hazardsDirty = true;
    if (!PrepareDraw(indexed))
        return;
    if (countBuffer)
    {
        if (indexed)
            g_vk.vkCmdDrawIndexedIndirectCount(m_cmd, buffer, at, countBuffer, countAt, maxCount, stride);
        else
            g_vk.vkCmdDrawIndirectCount(m_cmd, buffer, at, countBuffer, countAt, maxCount, stride);
    }
    else if (indexed)
        g_vk.vkCmdDrawIndexedIndirect(m_cmd, buffer, at, maxCount, stride);
    else
        g_vk.vkCmdDrawIndirect(m_cmd, buffer, at, maxCount, stride);
}

// ================================================================================================
// Input assembler
// ================================================================================================

void STDMETHODCALLTYPE Context::IASetInputLayout(ID3D11InputLayout* layout)
{
    if (Assign(inputLayout, layout))
    {
        applied.vertexInputDirty = true;
        if (!m_device->vertexInputDynamic)
            applied.pipelineDirty = true;
    }
}

void STDMETHODCALLTYPE Context::IASetVertexBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers, const UINT* strides,
    const UINT* offsets)
{
    if (!ClampRange(start, num, kMaxVBs, "vertex buffer"))
        return;
    for (UINT i = 0; i < num; ++i)
    {
        VertexBufferBinding& vb = vbs[start + i];
        ID3D11Buffer* b = buffers ? buffers[i] : nullptr;
        const UINT stride = strides ? strides[i] : 0;
        const UINT offset = offsets ? offsets[i] : 0;
        if (Assign(vb.buffer, b))
        {
            applied.vbDirty = true;
            applied.hazardsDirty = true;
        }
        if (vb.stride != stride)
        {
            vb.stride = stride;
            applied.vbDirty = true;
            applied.vertexInputDirty = true;
        }
        if (vb.offset != offset)
        {
            vb.offset = offset;
            applied.vbDirty = true;
        }
    }
}

void STDMETHODCALLTYPE Context::IASetIndexBuffer(ID3D11Buffer* buffer, DXGI_FORMAT format, UINT offset)
{
    bool changed = Assign(indexBuffer, buffer);
    if (changed)
        applied.hazardsDirty = true;
    if (format != indexFormat || offset != indexOffset)
        changed = true;
    indexFormat = format;
    indexOffset = offset;
    if (changed)
        applied.ibDirty = true;
}

void STDMETHODCALLTYPE Context::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY newTopology)
{
    topology = newTopology;
}

void STDMETHODCALLTYPE Context::IAGetInputLayout(ID3D11InputLayout** layout)
{
    GetInterface(inputLayout, layout);
}

void STDMETHODCALLTYPE Context::IAGetVertexBuffers(UINT start, UINT num, ID3D11Buffer** buffers, UINT* strides, UINT* offsets)
{
    for (UINT i = 0; i < num; ++i)
    {
        const u64 slot = u64(start) + i;
        const VertexBufferBinding* vb = slot < kMaxVBs ? &vbs[slot] : nullptr;
        if (buffers)
        {
            buffers[i] = vb ? vb->buffer : nullptr;
            if (buffers[i])
                buffers[i]->AddRef();
        }
        if (strides)
            strides[i] = vb ? vb->stride : 0;
        if (offsets)
            offsets[i] = vb ? vb->offset : 0;
    }
}

void STDMETHODCALLTYPE Context::IAGetIndexBuffer(ID3D11Buffer** buffer, DXGI_FORMAT* format, UINT* offset)
{
    GetInterface(indexBuffer, buffer);
    if (format)
        *format = indexFormat;
    if (offset)
        *offset = indexOffset;
}

void STDMETHODCALLTYPE Context::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* outTopology)
{
    if (outTopology)
        *outTopology = topology;
}

// ================================================================================================
// Shader stages
// ================================================================================================

void STDMETHODCALLTYPE Context::VSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) { SetConstantBuffers(*this, Stage::VS, start, num, buffers); }
void STDMETHODCALLTYPE Context::HSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) { SetConstantBuffers(*this, Stage::HS, start, num, buffers); }
void STDMETHODCALLTYPE Context::DSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) { SetConstantBuffers(*this, Stage::DS, start, num, buffers); }
void STDMETHODCALLTYPE Context::GSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) { SetConstantBuffers(*this, Stage::GS, start, num, buffers); }
void STDMETHODCALLTYPE Context::PSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) { SetConstantBuffers(*this, Stage::PS, start, num, buffers); }
void STDMETHODCALLTYPE Context::CSSetConstantBuffers(UINT start, UINT num, ID3D11Buffer* const* buffers) { SetConstantBuffers(*this, Stage::CS, start, num, buffers); }

void STDMETHODCALLTYPE Context::VSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) { SetShaderResources(*this, Stage::VS, start, num, views); }
void STDMETHODCALLTYPE Context::HSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) { SetShaderResources(*this, Stage::HS, start, num, views); }
void STDMETHODCALLTYPE Context::DSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) { SetShaderResources(*this, Stage::DS, start, num, views); }
void STDMETHODCALLTYPE Context::GSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) { SetShaderResources(*this, Stage::GS, start, num, views); }
void STDMETHODCALLTYPE Context::PSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) { SetShaderResources(*this, Stage::PS, start, num, views); }
void STDMETHODCALLTYPE Context::CSSetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView* const* views) { SetShaderResources(*this, Stage::CS, start, num, views); }

void STDMETHODCALLTYPE Context::VSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) { SetSamplers(*this, Stage::VS, start, num, samplers); }
void STDMETHODCALLTYPE Context::HSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) { SetSamplers(*this, Stage::HS, start, num, samplers); }
void STDMETHODCALLTYPE Context::DSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) { SetSamplers(*this, Stage::DS, start, num, samplers); }
void STDMETHODCALLTYPE Context::GSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) { SetSamplers(*this, Stage::GS, start, num, samplers); }
void STDMETHODCALLTYPE Context::PSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) { SetSamplers(*this, Stage::PS, start, num, samplers); }
void STDMETHODCALLTYPE Context::CSSetSamplers(UINT start, UINT num, ID3D11SamplerState* const* samplers) { SetSamplers(*this, Stage::CS, start, num, samplers); }

void STDMETHODCALLTYPE Context::VSSetShader(ID3D11VertexShader* shader, ID3D11ClassInstance* const*, UINT numInst) { SetShader(*this, Stage::VS, shader, numInst); }
void STDMETHODCALLTYPE Context::HSSetShader(ID3D11HullShader* shader, ID3D11ClassInstance* const*, UINT numInst) { SetShader(*this, Stage::HS, shader, numInst); }
void STDMETHODCALLTYPE Context::DSSetShader(ID3D11DomainShader* shader, ID3D11ClassInstance* const*, UINT numInst) { SetShader(*this, Stage::DS, shader, numInst); }
void STDMETHODCALLTYPE Context::GSSetShader(ID3D11GeometryShader* shader, ID3D11ClassInstance* const*, UINT numInst) { SetShader(*this, Stage::GS, shader, numInst); }
void STDMETHODCALLTYPE Context::PSSetShader(ID3D11PixelShader* shader, ID3D11ClassInstance* const*, UINT numInst) { SetShader(*this, Stage::PS, shader, numInst); }
void STDMETHODCALLTYPE Context::CSSetShader(ID3D11ComputeShader* shader, ID3D11ClassInstance* const*, UINT numInst) { SetShader(*this, Stage::CS, shader, numInst); }

void STDMETHODCALLTYPE Context::CSSetUnorderedAccessViews(UINT start, UINT num, ID3D11UnorderedAccessView* const* uavs,
    const UINT* counts)
{
    if (!ClampRange(start, num, kMaxUAVs, "compute unordered access"))
        return;
    if (counts && uavs)
    {
        // D3D11 ignores the count of a UAV without a hidden counter (the engine passes zeros for all).
        for (UINT i = 0; i < num; ++i)
        {
            if (counts[i] == UINT(-1) || !uavs[i])
                continue;
            D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
            uavs[i]->GetDesc(&d);
            if (d.ViewDimension == D3D11_UAV_DIMENSION_BUFFER &&
                (d.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)))
            {
                Once("! UAV initial counts are ignored (append/counter UAVs are not supported)");
                break;
            }
        }
    }
    for (UINT i = 0; i < num; ++i)
    {
        ID3D11UnorderedAccessView* uav = uavs ? uavs[i] : nullptr;
        if (!Assign(csUavs[start + i], uav))
            continue;
        stages[u32(Stage::CS)].dirty = true;
        applied.computeHazardsDirty = true;
        if (uav)
        {
            // D3D11 unbinds every input and graphics output that aliases a new compute output.
            View* v = AsView(uav);
            UnbindSrvsOf(v->resource, v->range);
            UnbindGraphicsOutputsOf(*this, v->resource, v->range);
            m_targetsDirty = true;
        }
    }
}

void STDMETHODCALLTYPE Context::VSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) { GetRange(stages[u32(Stage::VS)].cb, kMaxCBs, start, num, buffers); }
void STDMETHODCALLTYPE Context::HSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) { GetRange(stages[u32(Stage::HS)].cb, kMaxCBs, start, num, buffers); }
void STDMETHODCALLTYPE Context::DSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) { GetRange(stages[u32(Stage::DS)].cb, kMaxCBs, start, num, buffers); }
void STDMETHODCALLTYPE Context::GSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) { GetRange(stages[u32(Stage::GS)].cb, kMaxCBs, start, num, buffers); }
void STDMETHODCALLTYPE Context::PSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) { GetRange(stages[u32(Stage::PS)].cb, kMaxCBs, start, num, buffers); }
void STDMETHODCALLTYPE Context::CSGetConstantBuffers(UINT start, UINT num, ID3D11Buffer** buffers) { GetRange(stages[u32(Stage::CS)].cb, kMaxCBs, start, num, buffers); }

void STDMETHODCALLTYPE Context::VSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) { GetRange(stages[u32(Stage::VS)].srv, kMaxSRVs, start, num, views); }
void STDMETHODCALLTYPE Context::HSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) { GetRange(stages[u32(Stage::HS)].srv, kMaxSRVs, start, num, views); }
void STDMETHODCALLTYPE Context::DSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) { GetRange(stages[u32(Stage::DS)].srv, kMaxSRVs, start, num, views); }
void STDMETHODCALLTYPE Context::GSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) { GetRange(stages[u32(Stage::GS)].srv, kMaxSRVs, start, num, views); }
void STDMETHODCALLTYPE Context::PSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) { GetRange(stages[u32(Stage::PS)].srv, kMaxSRVs, start, num, views); }
void STDMETHODCALLTYPE Context::CSGetShaderResources(UINT start, UINT num, ID3D11ShaderResourceView** views) { GetRange(stages[u32(Stage::CS)].srv, kMaxSRVs, start, num, views); }

void STDMETHODCALLTYPE Context::VSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) { GetRange(stages[u32(Stage::VS)].samplers, kMaxSamplers, start, num, samplers); }
void STDMETHODCALLTYPE Context::HSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) { GetRange(stages[u32(Stage::HS)].samplers, kMaxSamplers, start, num, samplers); }
void STDMETHODCALLTYPE Context::DSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) { GetRange(stages[u32(Stage::DS)].samplers, kMaxSamplers, start, num, samplers); }
void STDMETHODCALLTYPE Context::GSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) { GetRange(stages[u32(Stage::GS)].samplers, kMaxSamplers, start, num, samplers); }
void STDMETHODCALLTYPE Context::PSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) { GetRange(stages[u32(Stage::PS)].samplers, kMaxSamplers, start, num, samplers); }
void STDMETHODCALLTYPE Context::CSGetSamplers(UINT start, UINT num, ID3D11SamplerState** samplers) { GetRange(stages[u32(Stage::CS)].samplers, kMaxSamplers, start, num, samplers); }

void STDMETHODCALLTYPE Context::VSGetShader(ID3D11VertexShader** shader, ID3D11ClassInstance**, UINT* numInst) { GetShader(stages[u32(Stage::VS)], shader, numInst); }
void STDMETHODCALLTYPE Context::HSGetShader(ID3D11HullShader** shader, ID3D11ClassInstance**, UINT* numInst) { GetShader(stages[u32(Stage::HS)], shader, numInst); }
void STDMETHODCALLTYPE Context::DSGetShader(ID3D11DomainShader** shader, ID3D11ClassInstance**, UINT* numInst) { GetShader(stages[u32(Stage::DS)], shader, numInst); }
void STDMETHODCALLTYPE Context::GSGetShader(ID3D11GeometryShader** shader, ID3D11ClassInstance**, UINT* numInst) { GetShader(stages[u32(Stage::GS)], shader, numInst); }
void STDMETHODCALLTYPE Context::PSGetShader(ID3D11PixelShader** shader, ID3D11ClassInstance**, UINT* numInst) { GetShader(stages[u32(Stage::PS)], shader, numInst); }
void STDMETHODCALLTYPE Context::CSGetShader(ID3D11ComputeShader** shader, ID3D11ClassInstance**, UINT* numInst) { GetShader(stages[u32(Stage::CS)], shader, numInst); }

void STDMETHODCALLTYPE Context::CSGetUnorderedAccessViews(UINT start, UINT num, ID3D11UnorderedAccessView** uavs)
{
    GetRange(csUavs, kMaxUAVs, start, num, uavs);
}

// ================================================================================================
// Rasterizer
// ================================================================================================

void STDMETHODCALLTYPE Context::RSSetState(ID3D11RasterizerState* state)
{
    if (state == rasterState)
        return;
    const RasterizerState* before = rasterState ? static_cast<RasterizerState*>(rasterState) : DefaultRaster();
    Assign(rasterState, state);
    const RasterizerState* after = rasterState ? static_cast<RasterizerState*>(rasterState) : DefaultRaster();
    applied.rasterDirty = true;
    if (before->scissor != after->scissor)
        applied.viewportDirty = true;
    if ((!m_device->eds3Polygon && before->polygon != after->polygon) ||
        (!m_device->eds3DepthClamp && before->depthClamp != after->depthClamp))
        applied.pipelineDirty = true;
}

void STDMETHODCALLTYPE Context::RSSetViewports(UINT num, const D3D11_VIEWPORT* newViewports)
{
    if (!newViewports)
        num = 0;
    if (num > kMaxViewports)
    {
        Once("more viewports than the layer supports; clamped");
        num = kMaxViewports;
    }
    if (num == numViewports && (!num || std::memcmp(viewports, newViewports, num * sizeof(D3D11_VIEWPORT)) == 0))
        return;
    if (num)
        std::memcpy(viewports, newViewports, num * sizeof(D3D11_VIEWPORT));
    numViewports = num;
    applied.viewportDirty = true;
}

void STDMETHODCALLTYPE Context::RSSetScissorRects(UINT num, const D3D11_RECT* rects)
{
    if (!rects)
        num = 0;
    if (num > kMaxViewports)
    {
        Once("more scissor rects than the layer supports; clamped");
        num = kMaxViewports;
    }
    if (num == numScissors && (!num || std::memcmp(scissors, rects, num * sizeof(D3D11_RECT)) == 0))
        return;
    if (num)
        std::memcpy(scissors, rects, num * sizeof(D3D11_RECT));
    numScissors = num;
    applied.viewportDirty = true;
}

void STDMETHODCALLTYPE Context::RSGetState(ID3D11RasterizerState** state)
{
    GetInterface(rasterState, state);
}

void STDMETHODCALLTYPE Context::RSGetViewports(UINT* num, D3D11_VIEWPORT* outViewports)
{
    if (!num)
        return;
    if (outViewports)
    {
        for (UINT i = 0; i < *num; ++i)
            outViewports[i] = i < numViewports ? viewports[i] : D3D11_VIEWPORT{};
        *num = (std::min)(*num, numViewports);
    }
    else
        *num = numViewports;
}

void STDMETHODCALLTYPE Context::RSGetScissorRects(UINT* num, D3D11_RECT* rects)
{
    if (!num)
        return;
    if (rects)
    {
        for (UINT i = 0; i < *num; ++i)
            rects[i] = i < numScissors ? scissors[i] : D3D11_RECT{};
        *num = (std::min)(*num, numScissors);
    }
    else
        *num = numScissors;
}

// ================================================================================================
// Output merger
// ================================================================================================

namespace
{
void BindTargets(Context& c, UINT num, ID3D11RenderTargetView* const* newRtvs, ID3D11DepthStencilView* newDsv, bool& changedOut)
{
    if (num > kMaxRTVs)
    {
        Once("more render targets than D3D11 allows; clamped");
        num = kMaxRTVs;
    }
    bool changed = false;
    for (u32 i = 0; i < kMaxRTVs; ++i)
    {
        ID3D11RenderTargetView* rtv = i < num && newRtvs ? newRtvs[i] : nullptr;
        if (!Assign(c.rtvs[i], rtv))
            continue;
        changed = true;
        if (rtv)
        {
            View* v = AsView(rtv);
            c.UnbindSrvsOf(v->resource, v->range);
            UnbindCsUavsOf(c, v->resource, v->range);
        }
    }
    if (Assign(c.dsv, newDsv))
    {
        changed = true;
        if (newDsv && DsvWrites(AsView(newDsv)))
        {
            View* v = AsView(newDsv);
            c.UnbindSrvsOf(v->resource, v->range);
        }
    }
    if (c.numRTVs != num)
        changed = true;
    c.numRTVs = num;
    changedOut = changed;
}
} // namespace

void STDMETHODCALLTYPE Context::OMSetRenderTargets(UINT num, ID3D11RenderTargetView* const* newRtvs, ID3D11DepthStencilView* newDsv)
{
    bool changed = false;
    BindTargets(*this, num, newRtvs, newDsv, changed);
    if (!changed)
        return;
    m_targetsDirty = true;
    applied.pipelineDirty = true;
    applied.hazardsDirty = true;
    applied.depthDirty = true;     // depth test follows whether a DSV is bound
    applied.blendDirty = true;     // attachment count
    applied.multisampleDirty = true;
}

void STDMETHODCALLTYPE Context::OMSetRenderTargetsAndUnorderedAccessViews(UINT newNumRTVs, ID3D11RenderTargetView* const* newRtvs,
    ID3D11DepthStencilView* newDsv, UINT uavStart, UINT numUAVs, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    if (newNumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        OMSetRenderTargets(newNumRTVs, newRtvs, newDsv);
    if (numUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
        return;
    (void)counts;
    // The call replaces the whole UAV set: slots outside [uavStart, uavStart + numUAVs) are unbound.
    ID3D11UnorderedAccessView* next[kMaxUAVs]{};
    UINT first = uavStart, count = numUAVs;
    if (ClampRange(first, count, kMaxUAVs, "OM unordered access"))
    {
        for (UINT i = 0; i < count; ++i)
            next[first + i] = uavs ? uavs[i] : nullptr;
    }
    for (u32 slot = 0; slot < kMaxUAVs; ++slot)
    {
        if (!Assign(psUavs[slot], next[slot]))
            continue;
        stages[u32(Stage::PS)].dirty = true;
        applied.hazardsDirty = true;
        if (next[slot])
        {
            View* v = AsView(next[slot]);
            UnbindSrvsOf(v->resource, v->range);
            UnbindCsUavsOf(*this, v->resource, v->range);
        }
    }
}

void STDMETHODCALLTYPE Context::OMSetBlendState(ID3D11BlendState* state, const FLOAT factor[4], UINT mask)
{
    if (Assign(blendState, state))
    {
        applied.blendDirty = true;
        applied.multisampleDirty = true; // alpha to coverage
        if (!m_device->eds3Blend || !m_device->eds3AlphaToCoverage)
            applied.pipelineDirty = true;
    }
    static const FLOAT kDefaultFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const FLOAT* f = factor ? factor : kDefaultFactor;
    if (std::memcmp(blendFactor, f, sizeof(blendFactor)) != 0)
    {
        std::memcpy(blendFactor, f, sizeof(blendFactor));
        applied.blendFactorDirty = true;
    }
    if (mask != sampleMask)
    {
        sampleMask = mask;
        applied.multisampleDirty = true;
        if (!m_device->eds3SampleMask)
            applied.pipelineDirty = true;
    }
}

void STDMETHODCALLTYPE Context::OMSetDepthStencilState(ID3D11DepthStencilState* state, UINT ref)
{
    if (Assign(depthState, state))
        applied.depthDirty = true;
    if (ref != stencilRef)
    {
        stencilRef = ref;
        applied.stencilRefDirty = true;
    }
}

void STDMETHODCALLTYPE Context::OMGetRenderTargets(UINT num, ID3D11RenderTargetView** outRtvs, ID3D11DepthStencilView** outDsv)
{
    GetRange(rtvs, kMaxRTVs, 0, num, outRtvs);
    GetInterface(dsv, outDsv);
}

void STDMETHODCALLTYPE Context::OMGetRenderTargetsAndUnorderedAccessViews(UINT num, ID3D11RenderTargetView** outRtvs,
    ID3D11DepthStencilView** outDsv, UINT uavStart, UINT numUAVs, ID3D11UnorderedAccessView** uavs)
{
    GetRange(rtvs, kMaxRTVs, 0, num, outRtvs);
    GetInterface(dsv, outDsv);
    GetRange(psUavs, kMaxUAVs, uavStart, numUAVs, uavs);
}

void STDMETHODCALLTYPE Context::OMGetBlendState(ID3D11BlendState** state, FLOAT factor[4], UINT* mask)
{
    GetInterface(blendState, state);
    if (factor)
        std::memcpy(factor, blendFactor, sizeof(blendFactor));
    if (mask)
        *mask = sampleMask;
}

void STDMETHODCALLTYPE Context::OMGetDepthStencilState(ID3D11DepthStencilState** state, UINT* ref)
{
    GetInterface(depthState, state);
    if (ref)
        *ref = stencilRef;
}

// ================================================================================================
// Stream output, predication, context type
// ================================================================================================

void STDMETHODCALLTYPE Context::SOSetTargets(UINT num, ID3D11Buffer* const* targets, const UINT*)
{
    if (!targets)
        return;
    for (UINT i = 0; i < num; ++i)
    {
        if (targets[i])
        {
            Once("stream output is not supported; SOSetTargets ignored");
            return;
        }
    }
}

void STDMETHODCALLTYPE Context::SOGetTargets(UINT num, ID3D11Buffer** targets)
{
    if (!targets)
        return;
    for (UINT i = 0; i < num; ++i)
        targets[i] = nullptr;
}

void STDMETHODCALLTYPE Context::SetPredication(ID3D11Predicate* predicate, BOOL)
{
    if (predicate)
        Once("predication is not supported; draws run unconditionally");
}

void STDMETHODCALLTYPE Context::GetPredication(ID3D11Predicate** predicate, BOOL* value)
{
    if (predicate)
        *predicate = nullptr;
    if (value)
        *value = FALSE;
}

D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE Context::GetType()
{
    return m_deferred ? D3D11_DEVICE_CONTEXT_DEFERRED : D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

void STDMETHODCALLTYPE Context::SetResourceMinLOD(ID3D11Resource*, FLOAT)
{
    Once("SetResourceMinLOD is not supported; ignored");
}

FLOAT STDMETHODCALLTYPE Context::GetResourceMinLOD(ID3D11Resource*)
{
    return 0.f;
}
} // namespace xrvk
