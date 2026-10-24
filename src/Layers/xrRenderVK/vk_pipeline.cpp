// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Pipelines of xrVulkan. Everything the device lets be dynamic is dynamic (extended dynamic state 1-3,
// dynamic vertex input), so a pipeline is keyed by shaders, attachment formats and the few states left.
//
// With VK_EXT_graphics_pipeline_library the four parts of a pipeline (vertex input, pre-rasterization,
// fragment shader, fragment output) are compiled once each and fast-linked per combination - a new
// combination costs a link, not a compile - while a worker thread links the optimized pipeline in the
// background and swaps it in (the stutter fix of DXVK, done natively). Without the extension pipelines are
// compiled whole. Either way the driver's binaries persist in a VkPipelineCache file.
#include "vk_layer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>

namespace xrvk
{
namespace
{
constexpr u32 kCacheMagic = 0x43505247; // "GRPC"
constexpr u32 kCacheVersion = 1;
constexpr size_t kMaxCacheFileBytes = 512ull << 20;

bool ReadWholeFile(const std::wstring& path, std::vector<u8>& out)
{
    out.clear();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    bool ok = size > 0 && size_t(size) <= kMaxCacheFileBytes;
    if (ok)
    {
        out.resize(size_t(size));
        ok = std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const u8* data, size_t size)
{
    // Next to the target, then renamed over it: a crash mid-write never leaves a torn cache.
    const std::wstring temp = path + L".tmp";
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            std::wstring dir = path.substr(0, slash);
            for (size_t i = 0; i < dir.size(); ++i)
            {
                if (dir[i] == L'\\' || dir[i] == L'/')
                    CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
            }
            CreateDirectoryW(dir.c_str(), nullptr);
        }
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, temp.c_str(), L"wb") || !file)
        return false;
    const bool ok = std::fwrite(data, 1, size, file) == size;
    std::fclose(file);
    if (!ok)
    {
        DeleteFileW(temp.c_str());
        return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

struct CacheHeader
{
    u32 magic;
    u32 version;
    u32 vendor;
    u32 device;
    u32 driver;
    u8 uuid[VK_UUID_SIZE];
    u64 size;
    u64 hash;
};

// ---- dynamic states, per pipeline part ------------------------------------------------------------

struct DynamicStates
{
    VkDynamicState states[40];
    u32 count = 0;
    void Add(VkDynamicState s) { states[count++] = s; }
};

void VertexInputStates(Device* d, DynamicStates& s)
{
    s.Add(d->vertexInputDynamic ? VK_DYNAMIC_STATE_VERTEX_INPUT_EXT : VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
    s.Add(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
    s.Add(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE);
}

// mesh: a mesh shader pipeline, which has no tessellation state to make dynamic.
void PreRasterStates(Device* d, DynamicStates& s, bool mesh = false)
{
    s.Add(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
    s.Add(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
    s.Add(VK_DYNAMIC_STATE_DEPTH_BIAS);
    s.Add(VK_DYNAMIC_STATE_CULL_MODE);
    s.Add(VK_DYNAMIC_STATE_FRONT_FACE);
    s.Add(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE);
    s.Add(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE);
    if (d->eds3Polygon)
        s.Add(VK_DYNAMIC_STATE_POLYGON_MODE_EXT);
    if (d->eds3DepthClamp)
        s.Add(VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT);
    if (d->eds3DepthClip)
        s.Add(VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT);
    if (d->patchControlPointsDynamic && !mesh)
        s.Add(VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT);
    if (d->caps.shadingRate)
        s.Add(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR);
}

void FragmentStates(Device* d, DynamicStates& s)
{
    s.Add(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE);
    s.Add(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
    s.Add(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP);
    s.Add(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE);
    s.Add(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE);
    s.Add(VK_DYNAMIC_STATE_STENCIL_OP);
    s.Add(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
    s.Add(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
    s.Add(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
    if (d->caps.shadingRate)
        s.Add(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR);
    if (d->eds3SampleMask)
        s.Add(VK_DYNAMIC_STATE_SAMPLE_MASK_EXT);
    if (d->eds3AlphaToCoverage)
        s.Add(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
    if (d->eds3Samples)
        s.Add(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT);
}

void OutputStates(Device* d, DynamicStates& s)
{
    s.Add(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
    if (d->eds3Blend)
    {
        s.Add(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
        s.Add(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
        s.Add(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    }
    if (d->eds3SampleMask)
        s.Add(VK_DYNAMIC_STATE_SAMPLE_MASK_EXT);
    if (d->eds3AlphaToCoverage)
        s.Add(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
    if (d->eds3Samples)
        s.Add(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT);
}

void AllStates(Device* d, DynamicStates& s, bool mesh)
{
    if (!mesh)
        VertexInputStates(d, s);
    DynamicStates parts[3];
    PreRasterStates(d, parts[0], mesh);
    FragmentStates(d, parts[1]);
    OutputStates(d, parts[2]);
    for (const DynamicStates& p : parts)
    {
        for (u32 i = 0; i < p.count; ++i)
        {
            bool present = false;
            for (u32 j = 0; j < s.count && !present; ++j)
                present = s.states[j] == p.states[i];
            if (!present)
                s.Add(p.states[i]);
        }
    }
}

// ---- state blocks ---------------------------------------------------------------------------------

// The static vertex input of a layout as the vertex shader reads it.
struct VertexInput
{
    VkVertexInputBindingDescription bindings[kMaxVBs];
    VkVertexInputAttributeDescription attributes[32];
    u32 bindingCount = 0, attributeCount = 0;
};

void BuildVertexInput(const InputLayout* layout, const Shader* vs, VertexInput& out)
{
    out = {};
    if (!layout || !vs)
        return;
    u32 slotsUsed = 0;
    for (const ShaderInterface::Var& in : vs->iface.inputs)
    {
        for (const InputLayout::Element& e : layout->elements)
        {
            if (e.index != in.index || e.semantic != in.semantic)
                continue;
            if (out.attributeCount < 32)
            {
                VkVertexInputAttributeDescription& a = out.attributes[out.attributeCount++];
                a.location = in.location;
                a.binding = e.slot;
                a.format = e.format;
                a.offset = e.offset;
                slotsUsed |= 1u << e.slot;
            }
            break;
        }
    }
    for (u32 slot = 0; slot < kMaxVBs; ++slot)
    {
        if (!(slotsUsed & (1u << slot)))
            continue;
        VkVertexInputBindingDescription& b = out.bindings[out.bindingCount++];
        b.binding = slot;
        b.stride = 0; // dynamic (vkCmdBindVertexBuffers2)
        b.inputRate = layout->slotInstance[slot] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }
}

VkPrimitiveTopology ClassRepresentative(VkPrimitiveTopology topology)
{
    switch (topology)
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

// Shader stages with their inputs relinked to the stage before them (semantic matching like D3D11).
struct StageSet
{
    VkPipelineShaderStageCreateInfo stages[5];
    u32 count = 0;
    std::vector<VkShaderModule> temporary;
    Device* device = nullptr;
    ~StageSet()
    {
        for (VkShaderModule m : temporary)
            g_vk.vkDestroyShaderModule(device->vk, m, nullptr);
    }
    bool Add(Shader* shader, const Shader* producer)
    {
        if (!shader)
            return true;
        VkPipelineShaderStageCreateInfo& s = stages[count++];
        s = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        s.stage = VkStageOf(shader->stage, shader->mesh);
        s.module = shader->module;
        s.pName = shader->entry.c_str();
        std::vector<u32> relinked;
        if (producer && RelinkInputs(*producer, *shader, relinked))
        {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = relinked.size() * 4;
            info.pCode = relinked.data();
            VkShaderModule module = VK_NULL_HANDLE;
            if (g_vk.vkCreateShaderModule(device->vk, &info, nullptr, &module) != VK_SUCCESS)
                return false;
            temporary.push_back(module);
            s.module = module;
        }
        return true;
    }
};

// The producer whose outputs `consumer` reads, along the D3D11 stage order.
const Shader* ProducerOf(const GraphicsPipelineDesc& d, const Shader* consumer)
{
    const Shader* chain[5] = {d.vs, d.hs, d.ds, d.gs, d.ps};
    const Shader* previous = nullptr;
    for (const Shader* s : chain)
    {
        if (!s)
            continue;
        if (s == consumer)
            return previous;
        previous = s;
    }
    return nullptr;
}

u64 SignatureHash(const Shader* producer)
{
    if (!producer)
        return 0;
    u64 h = 0x51u;
    for (const ShaderInterface::Var& o : producer->iface.outputs)
    {
        h = Hash64(o.semantic.data(), o.semantic.size(), h);
        const u32 f[3] = {o.index, o.location, u32(o.patch)};
        h = Hash64(f, sizeof(f), h);
    }
    return h;
}
} // namespace

bool GraphicsPipelineDesc::operator==(const GraphicsPipelineDesc& o) const
{
    return vs == o.vs && hs == o.hs && ds == o.ds && gs == o.gs && ps == o.ps && inputLayout == o.inputLayout &&
        !std::memcmp(vbStrides, o.vbStrides, sizeof(vbStrides)) && blend == o.blend && raster == o.raster &&
        sampleMask == o.sampleMask && topologyClass == o.topologyClass && patchPoints == o.patchPoints &&
        numRTVs == o.numRTVs && !std::memcmp(rtvFormats, o.rtvFormats, sizeof(rtvFormats)) &&
        depthFormat == o.depthFormat && stencilFormat == o.stencilFormat && samples == o.samples &&
        alphaToCoverage == o.alphaToCoverage && shadingRate == o.shadingRate;
}

u64 GraphicsPipelineDesc::Content() const
{
    u64 h = 0x5c0e7u;
    const Shader* shaders[5] = {vs, hs, ds, gs, ps};
    for (const Shader* s : shaders)
        h = HashCombine(h, s ? s->hash : 0);
    h = HashCombine(h, inputLayout ? inputLayout->hash : 0);
    h = HashCombine(h, blend ? blend->hash : 0);
    return HashCombine(h, raster ? raster->hash : 0);
}

u64 GraphicsPipelineDesc::Hash() const
{
    // Content only: shader objects created again from the same code reuse the pipeline.
    u64 h = 0x9a7e11u;
    const Shader* shaders[5] = {vs, hs, ds, gs, ps};
    for (const Shader* s : shaders)
        h = HashCombine(h, s ? s->hash : 0);
    h = HashCombine(h, inputLayout ? inputLayout->hash : 0);
    h = HashCombine(h, blend ? blend->hash : 0);
    h = HashCombine(h, raster ? raster->hash : 0);
    const u32 fields[] = {sampleMask, u32(topologyClass), patchPoints, numRTVs, u32(depthFormat), u32(stencilFormat),
        samples, u32(alphaToCoverage), u32(shadingRate)};
    h = Hash64(fields, sizeof(fields), h);
    return Hash64(rtvFormats, sizeof(VkFormat) * numRTVs, h);
}

// ============================================================================================
// PipelineCache
// ============================================================================================

struct PipelineCache::Worker
{
    std::thread thread;
    std::mutex lock;
    std::condition_variable wake;
    struct Job
    {
        u64 key;
        VkPipeline libraries[4];
        u32 libraryCount;
        const PipelineLayout* layout;
        bool shadingRate;
    };
    std::deque<Job> jobs;
    bool stop = false;
};

bool PipelineCache::Init(Device* device, const wchar_t* path)
{
    m_device = device;
    m_path = path ? path : L"";
    OpenJournal();
    std::vector<u8> initial;
    if (!m_path.empty())
    {
        std::vector<u8> file;
        if (ReadWholeFile(m_path, file) && file.size() > sizeof(CacheHeader))
        {
            CacheHeader header;
            std::memcpy(&header, file.data(), sizeof(header));
            const u8* payload = file.data() + sizeof(header);
            // A different GPU or driver: start empty (the driver would refuse the blob anyway).
            if (header.magic == kCacheMagic && header.version == kCacheVersion && header.vendor == device->props.vendorID &&
                header.device == device->props.deviceID && header.driver == device->props.driverVersion &&
                !std::memcmp(header.uuid, device->props.pipelineCacheUUID, VK_UUID_SIZE) &&
                header.size == file.size() - sizeof(header) && header.hash == Hash64(payload, size_t(header.size)))
                initial.assign(payload, payload + header.size);
        }
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = initial.size();
    info.pInitialData = initial.empty() ? nullptr : initial.data();
    VkResult result = g_vk.vkCreatePipelineCache(device->vk, &info, nullptr, &m_cache);
    if (result != VK_SUCCESS && !initial.empty())
    {
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        result = g_vk.vkCreatePipelineCache(device->vk, &info, nullptr, &m_cache);
    }
    if (result != VK_SUCCESS)
    {
        Log("~ vkCreatePipelineCache failed: %s (pipelines compile without a cache)", ResultText(result));
        m_cache = VK_NULL_HANDLE;
    }
    m_loadedBytes = initial.size();
    if (!initial.empty())
        Log("* pipeline cache: %llu KB from the last session", static_cast<unsigned long long>(initial.size() >> 10));

    if (device->caps.pipelineLibrary)
    {
        m_worker = new (std::nothrow) Worker();
        if (m_worker)
        {
            try
            {
                m_worker->thread = std::thread([this] { WorkerLoop(); });
            }
            catch (...)
            {
                delete m_worker;
                m_worker = nullptr;
            }
        }
    }
    return true;
}

void PipelineCache::Shutdown()
{
    // The prewarm first: it hands the optimizer work.
    CloseJournal();
    if (m_worker)
    {
        {
            std::lock_guard lock(m_worker->lock);
            m_worker->stop = true;
            m_worker->jobs.clear();
        }
        m_worker->wake.notify_all();
        if (m_worker->thread.joinable())
            m_worker->thread.join();
        delete m_worker;
        m_worker = nullptr;
    }
    Save();
    std::unique_lock lock(m_lock);
    for (auto& [key, pipeline] : m_pipelines)
    {
        if (pipeline)
            g_vk.vkDestroyPipeline(m_device->vk, pipeline, nullptr);
    }
    for (VkPipeline pipeline : m_retired)
        g_vk.vkDestroyPipeline(m_device->vk, pipeline, nullptr);
    for (auto* map : {&m_vertexInput, &m_preRaster, &m_fragment, &m_output})
    {
        for (auto& [key, pipeline] : *map)
        {
            if (pipeline)
                g_vk.vkDestroyPipeline(m_device->vk, pipeline, nullptr);
        }
        map->clear();
    }
    m_pipelines.clear();
    m_retired.clear();
    if (m_cache)
        g_vk.vkDestroyPipelineCache(m_device->vk, m_cache, nullptr);
    m_cache = VK_NULL_HANDLE;
    FreeJournal();
}

void PipelineCache::Save()
{
    SaveJournal();
    if (!m_cache || m_path.empty() || !m_device)
        return;
    // The main thread at a precache's end and a prewarm pass's end may both write the file.
    static std::mutex saving;
    std::lock_guard save(saving);
    // Pipelines built meanwhile on other threads grow the cache between the two calls (VK_INCOMPLETE): room for them,
    // and asked again.
    size_t size = 0;
    std::vector<u8> file;
    for (u32 attempt = 0;; ++attempt)
    {
        if (attempt == 4)
        {
            Log("~ pipeline cache not saved: it kept growing while it was read");
            return;
        }
        if (g_vk.vkGetPipelineCacheData(m_device->vk, m_cache, &size, nullptr) != VK_SUCCESS || !size)
            return;
        size += size / 8 + (256u << 10);
        file.resize(sizeof(CacheHeader) + size);
        const VkResult result = g_vk.vkGetPipelineCacheData(m_device->vk, m_cache, &size, file.data() + sizeof(CacheHeader));
        if (result == VK_SUCCESS)
            break;
        if (result != VK_INCOMPLETE)
            return;
    }
    file.resize(sizeof(CacheHeader) + size);
    CacheHeader header{};
    header.magic = kCacheMagic;
    header.version = kCacheVersion;
    header.vendor = m_device->props.vendorID;
    header.device = m_device->props.deviceID;
    header.driver = m_device->props.driverVersion;
    std::memcpy(header.uuid, m_device->props.pipelineCacheUUID, VK_UUID_SIZE);
    header.size = size;
    header.hash = Hash64(file.data() + sizeof(CacheHeader), size);
    std::memcpy(file.data(), &header, sizeof(header));
    if (WriteWholeFile(m_path, file.data(), file.size()))
        Log("* pipeline cache saved: %llu KB, %u pipelines this session (%.1f ms compiling)",
            static_cast<unsigned long long>(size >> 10), m_count.load(), double(m_compileNs.load()) / 1e6);
}

void PipelineCache::Forget(Shader*)
{
    // Pipelines are keyed by content: nothing refers to the shader object.
}

VkPipeline PipelineCache::Graphics(const GraphicsPipelineDesc& desc, u64 hash, const PipelineLayout* layout)
{
    const bool prewarm = OnPrewarmThread();
    {
        std::shared_lock lock(m_lock);
        if (const auto it = m_pipelines.find(hash); it != m_pipelines.end())
        {
            if (prewarm || !m_unconfirmedCount.load(std::memory_order_relaxed) || !m_unconfirmed.count(hash))
                return it->second;
        }
    }
    // A prewarmed pipeline's first draw (or a miss).
    if (!prewarm && m_unconfirmedCount.load(std::memory_order_relaxed))
    {
        std::unique_lock lock(m_lock);
        if (m_unconfirmed.erase(hash))
        {
            m_unconfirmedCount.store(u32(m_unconfirmed.size()), std::memory_order_relaxed);
            const VkPipeline pipeline = m_pipelines[hash];
            lock.unlock();
            Record(desc, hash);
            return pipeline;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool linked = false;
    if (m_device->caps.pipelineLibrary && !desc.vs->mesh)
    {
        pipeline = LinkGraphics(desc, hash, layout);
        linked = pipeline != VK_NULL_HANDLE;
    }
    if (!pipeline)
        pipeline = CreateGraphics(desc, layout);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    m_compileNs.fetch_add(u64(ns), std::memory_order_relaxed);
    m_device->Count(Device::StatPipelines);
    {
        std::unique_lock lock(m_lock);
        // The prewarm's failure is not kept: what a context asks for later may still build (a damaged journal, memory
        // the loading's threads took).
        if (!pipeline && prewarm)
        {
            const auto it = m_pipelines.find(hash);
            return it != m_pipelines.end() ? it->second : VK_NULL_HANDLE;
        }
        const auto [it, inserted] = m_pipelines.emplace(hash, pipeline);
        if (!inserted)
        {
            // Another thread made the same one meanwhile: keep the first, drop ours.
            if (pipeline && pipeline != it->second)
                g_vk.vkDestroyPipeline(m_device->vk, pipeline, nullptr);
            pipeline = it->second;
        }
        else if (pipeline)
        {
            m_count.fetch_add(1, std::memory_order_relaxed);
            // Built ahead: the journal learns at its first draw that it is still drawn.
            if (prewarm)
            {
                m_unconfirmed.insert(hash);
                m_unconfirmedCount.store(u32(m_unconfirmed.size()), std::memory_order_relaxed);
            }
        }
    }
    (void)linked;
    if (!prewarm)
    {
        m_builtOnDemand.fetch_add(1, std::memory_order_relaxed);
        if (pipeline)
            Record(desc, hash);
    }
    return pipeline;
}

VkPipeline PipelineCache::CreateGraphics(const GraphicsPipelineDesc& d, const PipelineLayout* layout)
{
    Device* dev = m_device;
    const bool mesh = d.vs && d.vs->mesh;
    StageSet stages;
    stages.device = dev;
    if (mesh)
    {
        // Task (GS slot) then mesh (VS slot), the fragment shader reads the mesh shader's outputs.
        if (!stages.Add(d.gs, nullptr) || !stages.Add(d.vs, nullptr) || !stages.Add(d.ps, d.vs))
            return VK_NULL_HANDLE;
    }
    else
    {
        const Shader* chain[5] = {d.vs, d.hs, d.ds, d.gs, d.ps};
        for (const Shader* s : chain)
        {
            if (s && !stages.Add(const_cast<Shader*>(s), ProducerOf(d, s)))
                return VK_NULL_HANDLE;
        }
    }

    VertexInput vertexInput;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!dev->vertexInputDynamic && !mesh)
    {
        BuildVertexInput(d.inputLayout, d.vs, vertexInput);
        vi.vertexBindingDescriptionCount = vertexInput.bindingCount;
        vi.pVertexBindingDescriptions = vertexInput.bindings;
        vi.vertexAttributeDescriptionCount = vertexInput.attributeCount;
        vi.pVertexAttributeDescriptions = vertexInput.attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = ClassRepresentative(d.topologyClass);
    VkPipelineTessellationStateCreateInfo tess{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    tess.patchControlPoints = (std::max)(1u, d.patchPoints);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = d.raster ? d.raster->polygon : VK_POLYGON_MODE_FILL;
    rs.depthClampEnable = d.raster && d.raster->depthClamp ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.f;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
    if (dev->depthClipEnable && !dev->eds3DepthClip)
    {
        clip.depthClipEnable = rs.depthClampEnable ? VK_FALSE : VK_TRUE;
        rs.pNext = &clip;
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = dev->eds3Samples ? VK_SAMPLE_COUNT_1_BIT : VkSampleCountFlagBits((std::max)(1u, d.samples));
    ms.sampleShadingEnable = d.ps && d.ps->iface.sampleShading && dev->shaderFeatures.sampleRate ? VK_TRUE : VK_FALSE;
    ms.minSampleShading = 1.f;
    const VkSampleMask sampleMask[2] = {d.sampleMask, d.sampleMask};
    ms.pSampleMask = sampleMask;
    ms.alphaToCoverageEnable = d.alphaToCoverage ? VK_TRUE : VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachments[kMaxRTVs]{};
    for (u32 i = 0; i < d.numRTVs; ++i)
    {
        if (d.blend)
            attachments[i] = d.blend->attachments[i];
        else
            attachments[i].colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.numRTVs;
    cb.pAttachments = attachments;
    DynamicStates dyn;
    AllStates(dev, dyn, mesh);
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = dyn.count;
    dynamic.pDynamicStates = dyn.states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = d.numRTVs;
    rendering.pColorAttachmentFormats = d.rtvFormats;
    rendering.depthAttachmentFormat = d.depthFormat;
    rendering.stencilAttachmentFormat = d.stencilFormat;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    if (d.shadingRate)
        info.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    info.stageCount = stages.count;
    info.pStages = stages.stages;
    info.pVertexInputState = mesh ? nullptr : &vi;
    info.pInputAssemblyState = mesh ? nullptr : &ia;
    info.pTessellationState = d.hs ? &tess : nullptr;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dynamic;
    info.layout = layout->layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkCreateGraphicsPipelines(dev->vk, m_cache, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        LogOnce(reinterpret_cast<const void*>(uintptr_t(d.Hash())), "! a graphics pipeline was refused (%s): vs %s, ps %s",
            ResultText(result), d.vs ? ShaderName(d.vs).c_str() : "-", d.ps ? ShaderName(d.ps).c_str() : "-");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// ---- graphics pipeline libraries --------------------------------------------------------------

VkPipeline PipelineCache::Library(std::unordered_map<u64, VkPipeline>& map, u64 key,
    const std::function<VkPipeline()>& create)
{
    {
        std::shared_lock lock(m_lock);
        if (const auto it = map.find(key); it != map.end())
            return it->second;
    }
    VkPipeline created = create();
    std::unique_lock lock(m_lock);
    const auto [it, inserted] = map.emplace(key, created);
    if (!inserted && created && created != it->second)
        g_vk.vkDestroyPipeline(m_device->vk, created, nullptr);
    return it->second;
}

VkPipeline PipelineCache::LinkGraphics(const GraphicsPipelineDesc& d, u64 hash, const PipelineLayout* layout)
{
    Device* dev = m_device;
    const VkPipelineCreateFlags libraryFlags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
        VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT |
        (d.shadingRate ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0);
    const u32 rate = d.shadingRate ? 1u : 0u;

    // Vertex input interface.
    u64 viKey = HashCombine(0x71u, u64(ClassRepresentative(d.topologyClass)));
    if (!dev->vertexInputDynamic)
    {
        viKey = HashCombine(viKey, d.inputLayout ? d.inputLayout->hash : 0);
        viKey = HashCombine(viKey, d.vs ? d.vs->hash : 0);
    }
    const VkPipeline vertexInput = Library(m_vertexInput, viKey, [&]() -> VkPipeline {
        VertexInput input;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (!dev->vertexInputDynamic)
        {
            BuildVertexInput(d.inputLayout, d.vs, input);
            vi.vertexBindingDescriptionCount = input.bindingCount;
            vi.pVertexBindingDescriptions = input.bindings;
            vi.vertexAttributeDescriptionCount = input.attributeCount;
            vi.pVertexAttributeDescriptions = input.attributes;
        }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = ClassRepresentative(d.topologyClass);
        DynamicStates dyn;
        VertexInputStates(dev, dyn);
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = dyn.count;
        dynamic.pDynamicStates = dyn.states;
        VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
        lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib};
        info.flags = libraryFlags;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pDynamicState = &dynamic;
        VkPipeline p = VK_NULL_HANDLE;
        return g_vk.vkCreateGraphicsPipelines(dev->vk, m_cache, 1, &info, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
    });

    // Pre-rasterization shaders: VS, HS, DS, GS with their relinked inputs and their own sets.
    u64 prKey = 0x9e3u;
    {
        const Shader* pre[4] = {d.vs, d.hs, d.ds, d.gs};
        for (const Shader* s : pre)
            prKey = HashCombine(prKey, s ? s->hash : 0);
        const u32 f[4] = {dev->eds3Polygon ? 0u : u32(d.raster ? d.raster->polygon : 0),
            dev->eds3DepthClamp ? 0u : u32(d.raster && d.raster->depthClamp), dev->patchControlPointsDynamic ? 0u : d.patchPoints,
            rate};
        prKey = Hash64(f, sizeof(f), prKey);
    }
    const VkPipeline preRaster = Library(m_preRaster, prKey, [&]() -> VkPipeline {
        StageSet stages;
        stages.device = dev;
        const Shader* pre[4] = {d.vs, d.hs, d.ds, d.gs};
        for (const Shader* s : pre)
        {
            if (s && !stages.Add(const_cast<Shader*>(s), ProducerOf(d, s)))
                return VK_NULL_HANDLE;
        }
        const PipelineLayout* partial = PartialLayout(layout, true);
        VkPipelineTessellationStateCreateInfo tess{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        tess.patchControlPoints = (std::max)(1u, d.patchPoints);
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = d.raster ? d.raster->polygon : VK_POLYGON_MODE_FILL;
        rs.depthClampEnable = d.raster && d.raster->depthClamp ? VK_TRUE : VK_FALSE;
        rs.lineWidth = 1.f;
        VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
        if (dev->depthClipEnable && !dev->eds3DepthClip)
        {
            clip.depthClipEnable = rs.depthClampEnable ? VK_FALSE : VK_TRUE;
            rs.pNext = &clip;
        }
        DynamicStates dyn;
        PreRasterStates(dev, dyn);
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = dyn.count;
        dynamic.pDynamicStates = dyn.states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &rendering};
        lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib};
        info.flags = libraryFlags;
        info.stageCount = stages.count;
        info.pStages = stages.stages;
        info.pTessellationState = d.hs ? &tess : nullptr;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pDynamicState = &dynamic;
        info.layout = partial ? partial->layout : VK_NULL_HANDLE;
        VkPipeline p = VK_NULL_HANDLE;
        return partial && g_vk.vkCreateGraphicsPipelines(dev->vk, m_cache, 1, &info, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
    });

    // Fragment shader (or none: depth-only passes still need the part).
    const Shader* fragmentProducer = d.ps ? ProducerOf(d, d.ps) : nullptr;
    u64 fsKey = HashCombine(0xf5u, d.ps ? d.ps->hash : 0);
    {
        const u32 f[4] = {dev->eds3Samples ? 0u : d.samples, dev->eds3SampleMask ? 0u : d.sampleMask,
            dev->eds3AlphaToCoverage ? 0u : u32(d.alphaToCoverage), rate};
        fsKey = Hash64(f, sizeof(f), fsKey);
        fsKey = HashCombine(fsKey, SignatureHash(fragmentProducer));
    }
    const VkPipeline fragment = Library(m_fragment, fsKey, [&]() -> VkPipeline {
        StageSet stages;
        stages.device = dev;
        if (d.ps && !stages.Add(d.ps, fragmentProducer))
            return VK_NULL_HANDLE;
        const PipelineLayout* partial = PartialLayout(layout, false);
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = dev->eds3Samples ? VK_SAMPLE_COUNT_1_BIT : VkSampleCountFlagBits((std::max)(1u, d.samples));
        ms.sampleShadingEnable = d.ps && d.ps->iface.sampleShading && dev->shaderFeatures.sampleRate ? VK_TRUE : VK_FALSE;
        ms.minSampleShading = 1.f;
        const VkSampleMask mask[2] = {d.sampleMask, d.sampleMask};
        ms.pSampleMask = mask;
        ms.alphaToCoverageEnable = d.alphaToCoverage ? VK_TRUE : VK_FALSE;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        DynamicStates dyn;
        FragmentStates(dev, dyn);
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = dyn.count;
        dynamic.pDynamicStates = dyn.states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &rendering};
        lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib};
        info.flags = libraryFlags;
        info.stageCount = stages.count;
        info.pStages = stages.stages;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pDynamicState = &dynamic;
        info.layout = partial ? partial->layout : VK_NULL_HANDLE;
        VkPipeline p = VK_NULL_HANDLE;
        return partial && g_vk.vkCreateGraphicsPipelines(dev->vk, m_cache, 1, &info, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
    });

    // Fragment output: attachment formats and the blend state where it is not dynamic.
    u64 foKey = 0xf0u;
    {
        const u32 f[8] = {d.numRTVs, u32(d.depthFormat), u32(d.stencilFormat), dev->eds3Samples ? 0u : d.samples,
            dev->eds3SampleMask ? 0u : d.sampleMask, dev->eds3AlphaToCoverage ? 0u : u32(d.alphaToCoverage), rate,
            u32(d.ps && d.ps->iface.sampleShading)};
        foKey = Hash64(f, sizeof(f), foKey);
        foKey = Hash64(d.rtvFormats, sizeof(VkFormat) * d.numRTVs, foKey);
        if (!dev->eds3Blend)
            foKey = HashCombine(foKey, d.blend ? d.blend->hash : 0);
    }
    const VkPipeline output = Library(m_output, foKey, [&]() -> VkPipeline {
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = dev->eds3Samples ? VK_SAMPLE_COUNT_1_BIT : VkSampleCountFlagBits((std::max)(1u, d.samples));
        ms.sampleShadingEnable = d.ps && d.ps->iface.sampleShading && dev->shaderFeatures.sampleRate ? VK_TRUE : VK_FALSE;
        ms.minSampleShading = 1.f;
        const VkSampleMask mask[2] = {d.sampleMask, d.sampleMask};
        ms.pSampleMask = mask;
        ms.alphaToCoverageEnable = d.alphaToCoverage ? VK_TRUE : VK_FALSE;
        VkPipelineColorBlendAttachmentState attachments[kMaxRTVs]{};
        for (u32 i = 0; i < d.numRTVs; ++i)
        {
            if (d.blend && !dev->eds3Blend)
                attachments[i] = d.blend->attachments[i];
            else
                attachments[i].colorWriteMask = 0xF;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = d.numRTVs;
        cb.pAttachments = attachments;
        DynamicStates dyn;
        OutputStates(dev, dyn);
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = dyn.count;
        dynamic.pDynamicStates = dyn.states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = d.numRTVs;
        rendering.pColorAttachmentFormats = d.rtvFormats;
        rendering.depthAttachmentFormat = d.depthFormat;
        rendering.stencilAttachmentFormat = d.stencilFormat;
        VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &rendering};
        lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib};
        info.flags = libraryFlags;
        info.pMultisampleState = &ms;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dynamic;
        VkPipeline p = VK_NULL_HANDLE;
        return g_vk.vkCreateGraphicsPipelines(dev->vk, m_cache, 1, &info, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
    });

    if (!vertexInput || !preRaster || !fragment || !output)
    {
        static constexpr char kFormat[] = "~ a pipeline library was refused: pipelines compile whole";
        LogOnce(kFormat, kFormat);
        return VK_NULL_HANDLE;
    }
    const VkPipeline libraries[4] = {vertexInput, preRaster, fragment, output};
    VkPipelineLibraryCreateInfoKHR link{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    link.libraryCount = 4;
    link.pLibraries = libraries;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &link};
    info.flags = d.shadingRate ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0;
    info.layout = layout->layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (g_vk.vkCreateGraphicsPipelines(dev->vk, m_cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    // The optimized pipeline is linked in the background and replaces this one.
    if (m_worker)
    {
        std::lock_guard lock(m_worker->lock);
        Worker::Job job{hash, {vertexInput, preRaster, fragment, output}, 4, layout, d.shadingRate};
        m_worker->jobs.push_back(job);
        m_worker->wake.notify_one();
    }
    return pipeline;
}

void PipelineCache::WorkerLoop()
{
    // Below the render thread: the optimized links are filler work (DOOM compiles its manifest the same way).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"xrVulkan pipeline optimizer");
    for (;;)
    {
        Worker::Job job;
        {
            std::unique_lock lock(m_worker->lock);
            m_worker->wake.wait(lock, [&] { return m_worker->stop || !m_worker->jobs.empty(); });
            if (m_worker->stop)
                return;
            job = m_worker->jobs.front();
            m_worker->jobs.pop_front();
        }
        VkPipelineLibraryCreateInfoKHR link{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
        link.libraryCount = job.libraryCount;
        link.pLibraries = job.libraries;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &link};
        info.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
            (job.shadingRate ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0);
        info.layout = job.layout->layout;
        VkPipeline optimized = VK_NULL_HANDLE;
        if (g_vk.vkCreateGraphicsPipelines(m_device->vk, m_cache, 1, &info, nullptr, &optimized) != VK_SUCCESS || !optimized)
            continue;
        std::unique_lock lock(m_lock);
        auto it = m_pipelines.find(job.key);
        if (it == m_pipelines.end())
        {
            g_vk.vkDestroyPipeline(m_device->vk, optimized, nullptr);
            continue;
        }
        // The fast-linked one may still be bound or in flight: it stays until the device goes.
        if (it->second)
            m_retired.push_back(it->second);
        it->second = optimized;
        m_generation.fetch_add(1, std::memory_order_release);
    }
}

const PipelineLayout* PipelineCache::PartialLayout(const PipelineLayout* full, bool preRaster)
{
    // Independent sets: a library sees only its own stages' set layouts.
    const SetLayout* sets[kGraphicsStages]{};
    for (u32 i = 0; i < kGraphicsStages; ++i)
    {
        const bool fragmentSet = i == SetOf(Stage::PS);
        if (fragmentSet != !preRaster)
            continue;
        sets[i] = full->sets[i];
    }
    return m_device->layouts.Graphics(sets, true);
}

VkPipeline PipelineCache::Compute(Shader* cs, const PipelineLayout* layout)
{
    const u64 key = HashCombine(0xc0de5u, cs->hash);
    {
        std::shared_lock lock(m_lock);
        if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
            return it->second;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = cs->module;
    info.stage.pName = cs->entry.c_str();
    info.layout = layout->layout;
    const auto start = std::chrono::steady_clock::now();
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkCreateComputePipelines(m_device->vk, m_cache, 1, &info, nullptr, &pipeline);
    m_compileNs.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
        std::memory_order_relaxed);
    if (result != VK_SUCCESS)
    {
        LogOnce(reinterpret_cast<const void*>(uintptr_t(key)), "! a compute pipeline was refused (%s): %s", ResultText(result),
            ShaderName(cs).c_str());
        pipeline = VK_NULL_HANDLE;
    }
    std::unique_lock lock(m_lock);
    const auto [it, inserted] = m_pipelines.emplace(key, pipeline);
    if (!inserted && pipeline && pipeline != it->second)
        g_vk.vkDestroyPipeline(m_device->vk, pipeline, nullptr);
    else if (inserted && pipeline)
        m_count.fetch_add(1, std::memory_order_relaxed);
    return it->second;
}
} // namespace xrvk
