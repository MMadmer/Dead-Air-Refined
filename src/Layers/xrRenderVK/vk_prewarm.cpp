// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The pipeline prewarm of xrVulkan. Every graphics pipeline a context builds is journaled by content - the content
// hashes of its shaders, the D3D11 descriptions of its states and the few values a pipeline still holds - next to the
// pipeline cache. As a level's shaders come in, background threads build the journaled pipelines whose shaders exist:
// with graphics pipeline libraries that compiles the shader parts early and hands the optimized link to the optimizer,
// so the first draw finds the pipeline made instead of compiling its parts mid-frame. The cache holds the driver's
// binaries and goes with a driver update; the journal outlives it, and the release ships one (the QA sweep's).
//
// The states are made again through the device, as the game makes them, and must hash to what the pipeline had: a
// journal of another build of the layer, damaged or foreign, builds nothing. A pipeline the prewarm built enters the
// journal's ageing only when a context draws with it (PipelineCache::Graphics).
#include "vk_layer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_set>

namespace xrvk
{
namespace
{
constexpr u32 kJournalMagic = 0x4A505256; // "VRPJ"
constexpr u32 kJournalVersion = 2;
constexpr size_t kMaxJournalBytes = 64ull << 20;
// Pipelines no context drew for this many sessions are left out of the next save.
constexpr u32 kMaxAge = 48;
constexpr size_t kHardCap = 32768;
constexpr u32 kMaxThreads = 6;

enum : u32
{
    kHasLayout = 1,
    kHasBlend = 2,
    kHasRaster = 4,
};

// The fixed part of a journaled pipeline, raw in the file (the version and its size guard the layout).
struct Fixed
{
    u64 key;
    u64 shaders[5]; // vs hs ds gs ps content hashes, 0 = no stage
    u64 layoutHash, blendHash, rasterHash;
    u32 objects;    // kHas*: the state objects it had
    u32 features;   // KeyFeatures of the device that built it: another set keys the same draws otherwise
    D3D11_BLEND_DESC blend;
    D3D11_RASTERIZER_DESC raster;
    u32 sampleMask;
    u32 topologyClass;
    u32 patchPoints;
    u32 numRTVs;
    VkFormat rtvFormats[kMaxRTVs];
    VkFormat depthFormat, stencilFormat;
    u32 samples;
    u32 alphaToCoverage;
    u32 shadingRate;
    u32 stamp; // the session a context last drew with it; 0 = only the release's journal has it
};

// D3D11_INPUT_ELEMENT_DESC with its name.
struct InputElement
{
    std::string name;
    u32 index, format, slot, offset, slotClass, stepRate;
};

struct Entry
{
    Fixed fixed;
    std::vector<InputElement> elements;
};

struct JournalHeader
{
    u32 magic;
    u32 version;
    u32 fixedSize;
    u32 generation; // sessions that opened the journal
    u32 count;
    u32 reserved;
    u64 hash; // of what follows
};

thread_local bool t_prewarm = false;
std::mutex g_nameLock;

// The device features that decide what a pipeline is keyed by.
u32 KeyFeatures(const Device* d)
{
    const bool flags[] = {d->vertexInputDynamic, d->eds3Blend, d->eds3Polygon, d->eds3DepthClamp, d->eds3SampleMask,
        d->eds3AlphaToCoverage, d->eds3Samples, d->patchControlPointsDynamic, d->topologyUnrestricted};
    u32 bits = 0;
    for (u32 i = 0; i < std::size(flags); ++i)
        bits |= u32(flags[i]) << i;
    return bits;
}

// The attachment formats a context can have: those of the DXGI formats and their views.
bool KnownFormat(VkFormat format)
{
    static const std::unordered_set<u32> known = [] {
        std::unordered_set<u32> formats{u32(VK_FORMAT_UNDEFINED)};
        for (u32 i = 0; i < 192; ++i)
        {
            formats.insert(u32(Format(DXGI_FORMAT(i)).format));
            formats.insert(u32(ViewFormat(DXGI_FORMAT(i), false)));
            formats.insert(u32(ViewFormat(DXGI_FORMAT(i), true)));
        }
        return formats;
    }();
    return known.count(u32(format)) != 0;
}

// What the file says the pipeline was, within what a context can ask for.
bool Plausible(const Fixed& f)
{
    const u32 topology = f.topologyClass;
    if (topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
        topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
        return false;
    if (f.numRTVs > kMaxRTVs || f.patchPoints > 32 || !f.samples || f.samples > 16 || (f.samples & (f.samples - 1)))
        return false;
    for (const VkFormat format : f.rtvFormats)
    {
        if (!KnownFormat(format))
            return false;
    }
    return KnownFormat(f.depthFormat) && KnownFormat(f.stencilFormat);
}

// ---- live shaders ---------------------------------------------------------------------------------

struct Registry
{
    std::mutex lock;
    std::unordered_multimap<u64, ShaderRef> shaders;
};

Registry& Shaders()
{
    // Never destroyed: shader objects may go while statics are destroyed.
    static Registry* registry = new Registry();
    return *registry;
}

// With the registry's lock held.
bool Alive(Registry& registry, Device* device, u64 hash)
{
    for (auto [it, end] = registry.shaders.equal_range(hash); it != end; ++it)
    {
        if (it->second.device == device && it->second.refs->load(std::memory_order_relaxed))
            return true;
    }
    return false;
}

// A reference on a live shader of the device with this content; false when there is none (any more).
bool Retain(Device* device, u64 hash, ShaderRef& out)
{
    Registry& registry = Shaders();
    std::lock_guard lock(registry.lock);
    for (auto [it, end] = registry.shaders.equal_range(hash); it != end; ++it)
    {
        if (it->second.device != device)
            continue;
        // A shader whose last reference went is being destroyed: never revived.
        ULONG refs = it->second.refs->load(std::memory_order_relaxed);
        while (refs && !it->second.refs->compare_exchange_weak(refs, refs + 1, std::memory_order_acq_rel))
        {
        }
        if (refs)
        {
            out = it->second;
            return true;
        }
    }
    return false;
}

// ---- the file -------------------------------------------------------------------------------------

bool ReadBytes(const std::wstring& path, std::vector<u8>& out)
{
    out.clear();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    bool ok = size > 0 && size_t(size) <= kMaxJournalBytes;
    if (ok)
    {
        out.resize(size_t(size));
        ok = std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    return ok;
}

// Aside and renamed over the old one: a crash never leaves a torn journal.
bool WriteBytes(const std::wstring& path, const std::vector<u8>& data)
{
    const std::wstring temp = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temp.c_str(), L"wb") || !file)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    std::fclose(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

struct Reader
{
    const u8* p;
    size_t left;
    bool Bytes(void* out, size_t size)
    {
        if (size > left)
            return false;
        std::memcpy(out, p, size);
        p += size;
        left -= size;
        return true;
    }
    bool U32(u32& out) { return Bytes(&out, sizeof(out)); }
};

void Put(std::vector<u8>& out, const void* data, size_t size)
{
    const u8* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

void PutU32(std::vector<u8>& out, u32 value) { Put(out, &value, sizeof(value)); }

// Into `out`. The release's pipelines come in unseen (stamp 0) and never replace this machine's.
bool Parse(const u8* data, size_t size, std::unordered_map<u64, Entry>& out, u32* generation, bool shipped)
{
    JournalHeader header;
    if (size < sizeof(header) || size > kMaxJournalBytes)
        return false;
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != kJournalMagic || header.version != kJournalVersion || header.fixedSize != sizeof(Fixed) ||
        header.hash != Hash64(data + sizeof(header), size - sizeof(header)))
        return false;
    Reader reader{data + sizeof(header), size - sizeof(header)};
    for (u32 i = 0; i < header.count && out.size() < kHardCap; ++i)
    {
        Entry e;
        u32 count = 0;
        if (!reader.Bytes(&e.fixed, sizeof(e.fixed)) || !reader.U32(count) || count > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)
            return false;
        e.elements.resize(count);
        for (InputElement& element : e.elements)
        {
            u32 length = 0;
            if (!reader.U32(length) || !length || length > 256 || length > reader.left)
                return false;
            element.name.assign(reinterpret_cast<const char*>(reader.p), length);
            reader.p += length;
            reader.left -= length;
            if (!reader.U32(element.index) || !reader.U32(element.format) || !reader.U32(element.slot) ||
                !reader.U32(element.offset) || !reader.U32(element.slotClass) || !reader.U32(element.stepRate))
                return false;
        }
        if (!Plausible(e.fixed))
            return false;
        if (shipped)
        {
            e.fixed.stamp = 0;
            out.try_emplace(e.fixed.key, std::move(e));
        }
        else
            out.insert_or_assign(e.fixed.key, std::move(e));
    }
    if (generation)
        *generation = header.generation;
    return true;
}

std::vector<u8> Serialize(const std::unordered_map<u64, Entry>& entries, u32 generation)
{
    std::vector<u8> out(sizeof(JournalHeader));
    u32 count = 0;
    for (const auto& [key, e] : entries)
    {
        // The release's pipelines no context drew here stay in the release's file; this machine's go when none drew
        // them for kMaxAge sessions (another device's features included: a switch back finds them).
        if (!e.fixed.stamp || generation - e.fixed.stamp > kMaxAge)
            continue;
        Put(out, &e.fixed, sizeof(e.fixed));
        PutU32(out, u32(e.elements.size()));
        for (const InputElement& element : e.elements)
        {
            PutU32(out, u32(element.name.size()));
            Put(out, element.name.data(), element.name.size());
            const u32 fields[] = {element.index, element.format, element.slot, element.offset, element.slotClass, element.stepRate};
            Put(out, fields, sizeof(fields));
        }
        ++count;
    }
    JournalHeader header{kJournalMagic, kJournalVersion, u32(sizeof(Fixed)), generation, count, 0, 0};
    header.hash = Hash64(out.data() + sizeof(header), out.size() - sizeof(header));
    std::memcpy(out.data(), &header, sizeof(header));
    return out;
}

// A COM reference released at the end of the scope.
template <class T>
struct Held
{
    T* object = nullptr;
    ~Held()
    {
        if (object)
            object->Release();
    }
};
} // namespace

bool OnPrewarmThread() { return t_prewarm; }

std::string ShaderName(const Shader* shader)
{
    std::lock_guard lock(g_nameLock);
    return shader->name;
}

void SetShaderName(Shader* shader, const char* name)
{
    std::lock_guard lock(g_nameLock);
    shader->name = name ? name : "";
}

void PublishShader(const ShaderRef& ref)
{
    Registry& registry = Shaders();
    std::lock_guard lock(registry.lock);
    registry.shaders.emplace(ref.shader->hash, ref);
}

void WithdrawShader(const Shader* shader)
{
    Registry& registry = Shaders();
    std::lock_guard lock(registry.lock);
    for (auto [it, end] = registry.shaders.equal_range(shader->hash); it != end; ++it)
    {
        if (it->second.shader == shader)
        {
            registry.shaders.erase(it);
            return;
        }
    }
}

void WithdrawShaders(const Device* device)
{
    Registry& registry = Shaders();
    std::lock_guard lock(registry.lock);
    for (auto it = registry.shaders.begin(); it != registry.shaders.end();)
        it = it->second.device == device ? registry.shaders.erase(it) : std::next(it);
}

struct PipelineCache::Journal
{
    std::wstring path; // empty: this session only
    u32 features = 0;
    std::mutex lock;   // entries, generation, dirty, shippedMerged, queued
    std::unordered_map<u64, Entry> entries;
    u32 generation = 1;
    bool dirty = false;
    bool shippedMerged = false;
    std::unordered_set<u64> queued; // handed to the prewarm this session
    std::mutex saveLock;            // one writer of the file

    std::mutex queueLock; // queue, busy, stop, the pass and its counters' reset
    std::condition_variable wake;
    std::deque<u64> queue;
    std::vector<std::thread> threads;
    u32 busy = 0;
    bool stop = false;
    // The pass: from the queue's last empty moment.
    std::chrono::steady_clock::time_point passStart;
    u32 passQueued = 0;
    std::atomic<u32> built{0}, ready{0}, missing{0}, refused{0};
};

void PipelineCache::OpenJournal()
{
    m_journal = new (std::nothrow) Journal();
    if (!m_journal)
        return;
    m_journal->features = KeyFeatures(m_device);
    if (m_path.empty())
        return;
    try
    {
        // pipelines.bin -> pipelines.journal
        std::wstring path = m_path;
        const size_t dot = path.find_last_of(L'.');
        const size_t slash = path.find_last_of(L"\\/");
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
            path.resize(dot);
        path += L".journal";
        m_journal->path = path;
        std::vector<u8> file;
        u32 generation = 0;
        if (ReadBytes(path, file))
        {
            if (Parse(file.data(), file.size(), m_journal->entries, &generation, false))
                Log("* pipeline journal: %zu pipelines", m_journal->entries.size());
            else
            {
                m_journal->entries.clear();
                generation = 0;
                Log("~ pipeline journal unreadable, starting empty");
            }
        }
        m_journal->generation = generation + 1;
    }
    catch (...)
    {
        m_journal->entries.clear();
    }
}

void PipelineCache::CloseJournal()
{
    Journal* j = m_journal;
    if (!j)
        return;
    {
        std::lock_guard lock(j->queueLock);
        j->stop = true;
        j->queue.clear();
    }
    j->wake.notify_all();
    for (std::thread& thread : j->threads)
    {
        if (thread.joinable())
            thread.join();
    }
    j->threads.clear();
    WithdrawShaders(m_device);
}

void PipelineCache::FreeJournal()
{
    delete m_journal;
    m_journal = nullptr;
}

void PipelineCache::Record(const GraphicsPipelineDesc& desc, u64 hash)
{
    Journal* j = m_journal;
    if (!j)
        return;
    try
    {
        std::lock_guard lock(j->lock);
        if (const auto it = j->entries.find(hash); it != j->entries.end())
        {
            if (it->second.fixed.stamp != j->generation || it->second.fixed.features != j->features)
            {
                it->second.fixed.stamp = j->generation;
                it->second.fixed.features = j->features;
                j->dirty = true;
            }
            return;
        }
        if (j->entries.size() >= kHardCap)
            return;
        Entry e;
        Fixed& f = e.fixed;
        std::memset(&f, 0, sizeof(f));
        f.key = hash;
        f.features = j->features;
        const Shader* stages[5] = {desc.vs, desc.hs, desc.ds, desc.gs, desc.ps};
        for (u32 i = 0; i < 5; ++i)
            f.shaders[i] = stages[i] ? stages[i]->hash : 0;
        if (const InputLayout* l = desc.inputLayout)
        {
            f.objects |= kHasLayout;
            f.layoutHash = l->hash;
            for (const D3D11_INPUT_ELEMENT_DESC& d : l->desc11)
                e.elements.push_back({d.SemanticName ? d.SemanticName : "", d.SemanticIndex, u32(d.Format), d.InputSlot,
                    d.AlignedByteOffset, u32(d.InputSlotClass), d.InstanceDataStepRate});
        }
        if (desc.blend)
        {
            f.objects |= kHasBlend;
            f.blendHash = desc.blend->hash;
            f.blend = desc.blend->desc11;
        }
        if (desc.raster)
        {
            f.objects |= kHasRaster;
            f.rasterHash = desc.raster->hash;
            f.raster = desc.raster->desc11;
        }
        f.sampleMask = desc.sampleMask;
        f.topologyClass = u32(desc.topologyClass);
        f.patchPoints = desc.patchPoints;
        f.numRTVs = (std::min)(desc.numRTVs, kMaxRTVs);
        std::copy(std::begin(desc.rtvFormats), std::end(desc.rtvFormats), f.rtvFormats);
        f.depthFormat = desc.depthFormat;
        f.stencilFormat = desc.stencilFormat;
        f.samples = desc.samples;
        f.alphaToCoverage = desc.alphaToCoverage;
        f.shadingRate = desc.shadingRate;
        f.stamp = j->generation;
        j->entries.emplace(hash, std::move(e));
        j->dirty = true;
    }
    catch (...)
    {
    }
}

void PipelineCache::SaveJournal()
{
    Journal* j = m_journal;
    if (!j || j->path.empty())
        return;
    try
    {
        std::lock_guard save(j->saveLock);
        std::vector<u8> data;
        {
            std::lock_guard lock(j->lock);
            if (!j->dirty)
                return;
            j->dirty = false;
            data = Serialize(j->entries, j->generation);
        }
        if (!WriteBytes(j->path, data))
        {
            Log("~ pipeline journal could not be written (error %lu)", GetLastError());
            std::lock_guard lock(j->lock);
            j->dirty = true;
        }
    }
    catch (...)
    {
    }
}

void PipelineCache::Prewarm(const void* shipped, size_t shippedSize)
{
    Journal* j = m_journal;
    if (!j || !m_device)
        return;
    try
    {
        std::vector<u64> keys;
        {
            std::lock_guard lock(j->lock);
            if (!j->shippedMerged)
            {
                j->shippedMerged = true;
                if (shipped && shippedSize)
                {
                    const size_t before = j->entries.size();
                    if (Parse(static_cast<const u8*>(shipped), shippedSize, j->entries, nullptr, true))
                        Log("* pipeline journal of the release: %zu pipelines new here", j->entries.size() - before);
                    else
                        Log("~ pipeline journal of the release unreadable");
                }
            }
            // The pipelines of this device's features whose every stage exists now: the level's (and the HUD's, the
            // actors'...) shaders.
            Registry& registry = Shaders();
            std::lock_guard shaders(registry.lock);
            for (const auto& [key, e] : j->entries)
            {
                if (!e.fixed.shaders[0] || e.fixed.features != j->features || j->queued.count(key))
                    continue;
                bool alive = true;
                for (const u64 hash : e.fixed.shaders)
                {
                    if (hash && !Alive(registry, m_device, hash))
                    {
                        alive = false;
                        break;
                    }
                }
                if (alive)
                    keys.push_back(key);
            }
        }
        {
            std::shared_lock lock(m_lock);
            keys.erase(std::remove_if(keys.begin(), keys.end(), [&](u64 key) { return m_pipelines.count(key) != 0; }), keys.end());
        }
        Log("* pipeline prewarm: %zu pipelines queued, %u built on demand so far", keys.size(), m_builtOnDemand.load());
        if (keys.empty())
            return;
        {
            std::lock_guard lock(j->lock);
            j->queued.insert(keys.begin(), keys.end());
        }
        std::lock_guard lock(j->queueLock);
        if (j->stop)
            return;
        if (j->queue.empty() && !j->busy)
        {
            j->passStart = std::chrono::steady_clock::now();
            j->passQueued = 0;
            for (std::atomic<u32>* counter : {&j->built, &j->ready, &j->missing, &j->refused})
                counter->store(0);
        }
        j->passQueued += u32(keys.size());
        j->queue.insert(j->queue.end(), keys.begin(), keys.end());
        if (j->threads.empty())
        {
            // Beside the main thread, the texture uploads and the optimizer.
            const u32 cores = std::thread::hardware_concurrency();
            const u32 count = std::clamp(cores > 2 ? cores - 2 : 1u, 1u, kMaxThreads);
            for (u32 i = 0; i < count; ++i)
                j->threads.emplace_back([this] { PrewarmLoop(); });
        }
        j->wake.notify_all();
    }
    catch (...)
    {
    }
}

void PipelineCache::PrewarmLoop()
{
    // Below the game's threads: filler work while the level loads and plays.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"xrVulkan pipeline prewarm");
    t_prewarm = true;
    Journal* j = m_journal;
    for (;;)
    {
        u64 key = 0;
        {
            std::unique_lock lock(j->queueLock);
            j->wake.wait(lock, [&] { return j->stop || !j->queue.empty(); });
            if (j->stop)
                return;
            key = j->queue.front();
            j->queue.pop_front();
            ++j->busy;
        }
        PrewarmOne(key);
        // The pass's counters are read under the lock that resets them for the next pass. Nothing is saved here: a
        // pass may end in play, where reading the cache would hold up the contexts' pipelines; the precache's end and
        // the shutdown save.
        u32 counts[5]{};
        double ms = 0;
        {
            std::lock_guard lock(j->queueLock);
            if (--j->busy || !j->queue.empty() || j->stop)
                continue;
            counts[0] = j->passQueued;
            counts[1] = j->built.load();
            counts[2] = j->ready.load();
            counts[3] = j->missing.load();
            counts[4] = j->refused.load();
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - j->passStart).count();
        }
        Log("* pipeline prewarm: %u pipelines in %.0f ms (%u built, %u made already, %u without their shaders, %u refused)",
            counts[0], ms, counts[1], counts[2], counts[3], counts[4]);
    }
}

void PipelineCache::PrewarmOne(u64 key)
{
    Journal* j = m_journal;
    ShaderRef refs[5];
    try
    {
        Entry e;
        {
            std::lock_guard lock(j->lock);
            const auto it = j->entries.find(key);
            if (it == j->entries.end())
                return;
            e = it->second;
        }
        {
            std::shared_lock lock(m_lock);
            if (m_pipelines.count(key))
            {
                ++j->ready;
                return;
            }
        }
        bool alive = true;
        for (u32 i = 0; i < 5 && alive; ++i)
            alive = !e.fixed.shaders[i] || Retain(m_device, e.fixed.shaders[i], refs[i]);
        if (!alive)
        {
            // Gone since it was queued: a later pass may find them again.
            ++j->missing;
            std::lock_guard lock(j->lock);
            j->queued.erase(key);
        }
        else
        {
            const Fixed& f = e.fixed;
            // The states as the game makes them: the device checks the descriptions and hashes them its own way.
            Held<ID3D11InputLayout> layout;
            Held<ID3D11BlendState> blend;
            Held<ID3D11RasterizerState> raster;
            bool made = true;
            if (f.objects & kHasLayout)
            {
                std::vector<D3D11_INPUT_ELEMENT_DESC> elements(e.elements.size());
                for (size_t i = 0; i < elements.size(); ++i)
                {
                    const InputElement& s = e.elements[i];
                    elements[i] = {s.name.c_str(), s.index, DXGI_FORMAT(s.format), s.slot, s.offset,
                        D3D11_INPUT_CLASSIFICATION(s.slotClass), s.stepRate};
                }
                // The input signature is the DXIL twin's.
                const Shader* vs = refs[0].shader;
                made = !vs->dxil.empty() &&
                    SUCCEEDED(m_device->CreateInputLayout(elements.data(), UINT(elements.size()), vs->dxil.data(),
                        vs->dxil.size(), &layout.object)) &&
                    static_cast<InputLayout*>(layout.object)->hash == f.layoutHash;
            }
            if (made && (f.objects & kHasBlend))
                made = SUCCEEDED(m_device->CreateBlendState(&f.blend, &blend.object)) &&
                    static_cast<BlendState*>(blend.object)->hash == f.blendHash;
            if (made && (f.objects & kHasRaster))
                made = SUCCEEDED(m_device->CreateRasterizerState(&f.raster, &raster.object)) &&
                    static_cast<RasterizerState*>(raster.object)->hash == f.rasterHash;
            GraphicsPipelineDesc d;
            d.vs = refs[0].shader;
            d.hs = refs[1].shader;
            d.ds = refs[2].shader;
            d.gs = refs[3].shader;
            d.ps = refs[4].shader;
            d.inputLayout = static_cast<InputLayout*>(layout.object);
            d.blend = static_cast<BlendState*>(blend.object);
            d.raster = static_cast<RasterizerState*>(raster.object);
            d.sampleMask = f.sampleMask;
            d.topologyClass = VkPrimitiveTopology(f.topologyClass);
            d.patchPoints = f.patchPoints;
            d.numRTVs = f.numRTVs;
            std::copy(std::begin(f.rtvFormats), std::end(f.rtvFormats), d.rtvFormats);
            d.depthFormat = f.depthFormat;
            d.stencilFormat = f.stencilFormat;
            d.samples = f.samples;
            d.alphaToCoverage = f.alphaToCoverage != 0;
            d.shadingRate = f.shadingRate != 0;
            // The layout from the stages' set layouts, in the context's order.
            const SetLayout* sets[kGraphicsStages]{};
            const Shader* order[kGraphicsStages] = {d.vs, d.ps, d.gs, d.hs, d.ds};
            for (u32 i = 0; i < kGraphicsStages; ++i)
                sets[i] = order[i] ? order[i]->setLayout : nullptr;
            const PipelineLayout* pipelineLayout = made ? m_device->layouts.Graphics(sets) : nullptr;
            // The same key or nothing: another build of the layer keys its pipelines otherwise.
            if (pipelineLayout && HashCombine(d.Hash(), pipelineLayout->hash) == key && Graphics(d, key, pipelineLayout))
                ++j->built;
            else
                ++j->refused;
        }
    }
    catch (...)
    {
        ++j->refused;
    }
    // Outside every lock: the last reference destroys the shader, which withdraws it from the registry.
    for (ShaderRef& ref : refs)
    {
        if (ref.object)
            ref.object->Release();
    }
}
} // namespace xrvk
