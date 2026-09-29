// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Async compute of xrD3D12 (docs/dead-air/dx12-features/infra.md 3.3, async.md 3.3): the compute
// queue, jobs between a fork and a join of the immediate context, and compute recording of
// deferred contexts.
//
// A fork closes the immediate context's list and queues a marker. Context::Submit executes the direct
// work before the marker, signals the cross fence, makes the compute queue wait for it and runs the job
// there; the job's end signals the compute fence. A join marker makes the direct queue wait for that
// value. Every job is joined before the frame's final signal (Present), so the frame fence covers the
// compute work and frame-tagged memory (descriptor chunks, upload pages, allocators) needs no
// knowledge of the second queue. Without a compute queue a job runs in order on the direct queue.
// The queue is made by the first job: a session whose frames never take the job path has none.
#include "d12_layer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xrd3d12
{
namespace
{
constexpr u32 kStampPairs = 64;
constexpr u32 kNoStamps = ~0u;
constexpr u32 kAll = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
constexpr size_t kNameSize = 32;

constexpr D3D12_RESOURCE_STATES kReadStates = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
    D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
constexpr D3D12_RESOURCE_STATES kComputeStates = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_DEST |
    D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

bool ReadOnly(D3D12_RESOURCE_STATES s)
{
    return s != D3D12_RESOURCE_STATE_COMMON && !(u32(s) & ~u32(kReadStates));
}

bool ComputeLegal(D3D12_RESOURCE_STATES s)
{
    return !(u32(s) & ~u32(kComputeStates));
}

Device* LayerDevice(ID3D11Device* device)
{
    return IsLayerDevice(device) ? Device::From(device) : nullptr;
}

// The COM object behind a Resource (jobs hold references like the engine's bindings do).
ID3D11Resource* ComOf(Resource* r)
{
    switch (r->dimension)
    {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return static_cast<ID3D11Buffer*>(static_cast<Buffer*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return static_cast<ID3D11Texture1D*>(static_cast<Texture1D*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return static_cast<ID3D11Texture2D*>(static_cast<Texture2D*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return static_cast<ID3D11Texture3D*>(static_cast<Texture3D*>(r));
    default: return nullptr;
    }
}

// The engine's debug name of a resource, or its address.
std::string DebugName(Resource* r)
{
    std::string name;
    if (ID3D11Resource* object = ComOf(r))
    {
        UINT size = 0;
        if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &size, nullptr)) && size)
        {
            name.resize(size);
            if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name.data())))
                name.clear();
            while (!name.empty() && !name.back())
                name.pop_back();
        }
    }
    // Unnamed (Release): the address plus what identifies it in a log.
    if (name.empty())
    {
        char text[128];
        const D3D12_RESOURCE_DESC& d = r->nativeDesc;
        if (r->IsBuffer())
            std::snprintf(text, sizeof(text), "resource %p (buffer %llu bytes, bind 0x%x)", static_cast<void*>(r),
                static_cast<unsigned long long>(r->byteWidth), r->bindFlags);
        else
            std::snprintf(text, sizeof(text), "resource %p (texture %llux%ux%u, format %d, bind 0x%x)", static_cast<void*>(r),
                static_cast<unsigned long long>(d.Width), d.Height, d.DepthOrArraySize, int(r->format), r->bindFlags);
        name = text;
    }
    return name;
}

// A pooled compute (or, without a compute queue, direct) list for native jobs.
struct Recorder
{
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList4* list4 = nullptr; // weak: the list's own reference keeps it
    DescriptorAllocator descriptors;
    UploadAllocator upload;
    bool busy = false;
    u64 computeDone = 0; // compute fence value after its last run (0 = none)
    u64 directDone = 0;  // direct fence value after its last in-order run
};

// A tracked resource a job touches: the state the direct queue hands it over in and gets it back in.
struct Use
{
    Resource* resource = nullptr;
    ID3D11Resource* object = nullptr; // referenced until the job is recycled
    u32 subresource = kAll;
    D3D12_RESOURCE_STATES entry = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES exit = D3D12_RESOURCE_STATE_COMMON;
    bool shared = false; // only read and not transitioned: the direct queue may keep reading it
};
} // namespace

class AsyncJob
{
public:
    enum State : u8
    {
        Free,
        Recording, // BeginAsyncCompute .. EndAsyncCompute
        Closed,    // ready to fork
        Forked,    // queued after the direct work; loans active
        Joined,    // the direct queue waits for it (or it ran in order)
    };
    std::atomic<u8> state{Free};
    char name[kNameSize]{};
    Device* device = nullptr;
    Recorder* recorder = nullptr;              // native jobs
    ID3D12GraphicsCommandList* list = nullptr; // closed list waiting for its submission
    Context* listOwner = nullptr;              // the deferred context that gets `list` back
    bool inOrder = false;                      // runs on the direct queue (no compute queue, a DIRECT list)
    std::vector<Use> uses;
    std::vector<Resource*> readbacks;          // staging copies in the list (referenced)
    u32 stamps = kNoStamps;                    // timestamp pair
    u64 done = 0;                              // compute fence value after it
    u64 directDone = 0;                        // direct fence value after it (in order)
    u64 forkOrder = 0;                         // a join covers every job forked before
    u64 frame = 0;                             // Device::frame it was begun in (its frame memory)
    u32 pendingMarkers = 0;                    // queued entries Submit has not reached yet
    bool joinCalled = false;                   // the caller is done with the pointer
};

struct DeviceAsync
{
    ID3D12CommandQueue* queue = nullptr; // COMPUTE; null = jobs run in order on the direct queue
    FenceTimeline cross;                 // direct -> compute
    FenceTimeline compute;               // compute -> direct
    u64 crossValue = 0;
    u64 computeFrequency = 0;
    ID3D12QueryHeap* stampHeap = nullptr;
    ID3D12Resource* stampReadback = nullptr;
    const u64* stampData = nullptr;
    std::mutex lock; // pools, stamps, times, graveyard
    std::vector<AsyncJob*> jobs;
    std::vector<Recorder*> recorders;
    bool stampBusy[kStampPairs]{};
    u32 stampNext = 0;
    struct PendingStamps
    {
        u32 pair = kNoStamps;
        char name[kNameSize]{};
        bool compute = false;
        u64 fence = 0;
    };
    std::vector<PendingStamps> pendingStamps;
    std::vector<std::pair<std::string, float>> times; // job name -> last GPU milliseconds
    // Lists of dead deferred contexts, released when the compute work that ran them finished.
    std::vector<std::pair<u64, ID3D12GraphicsCommandList*>> graveyard;
    u64 frameCompute[kFramesInFlight]{};
    // Render thread: jobs from their fork until they are recycled, in fork order.
    std::vector<AsyncJob*> open;
    u64 forks = 0;
};

namespace
{
// The compute queue and its fences, made for the first job (render thread, before anything reads the
// queue for that job). false: jobs run in order on the direct queue - switched off, or the creation
// failed (not tried again).
bool AsyncQueue(Device& dev, DeviceAsync& as)
{
    if (as.queue)
        return true;
    if (!dev.caps.asyncCompute)
        return false;
    // The plain device creates it: frame generation hooks only the present queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ID3D12CommandQueue* queue = nullptr;
    HRESULT hr = dev.d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr) && (!as.cross.Init(dev.d12) || !as.compute.Init(dev.d12)))
        hr = E_FAIL;
    if (FAILED(hr))
    {
        Log("! async compute: no compute queue (0x%08X); jobs run in order on the direct queue", unsigned(hr));
        if (queue)
            queue->Release();
        as.cross.Shutdown();
        as.compute.Shutdown();
        dev.caps.asyncCompute = false;
        return false;
    }
    queue->SetName(L"xrD3D12 compute");
    if (FAILED(queue->GetTimestampFrequency(&as.computeFrequency)))
        as.computeFrequency = 0;
    as.queue = queue;
    dev.computeQueue = queue;
    Log("* async compute: compute queue made for the first job");
    return true;
}

u32 AcquireStamps(DeviceAsync& as) // as.lock held
{
    if (!as.stampHeap)
        return kNoStamps;
    for (u32 i = 0; i < kStampPairs; ++i)
    {
        const u32 pair = (as.stampNext + i) % kStampPairs;
        if (!as.stampBusy[pair])
        {
            as.stampBusy[pair] = true;
            as.stampNext = pair + 1;
            return pair;
        }
    }
    return kNoStamps;
}

void StampBegin(DeviceAsync& as, ID3D12GraphicsCommandList* list, u32 pair)
{
    if (pair != kNoStamps)
        list->EndQuery(as.stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, pair * 2);
}

void StampEnd(DeviceAsync& as, ID3D12GraphicsCommandList* list, u32 pair)
{
    if (pair == kNoStamps)
        return;
    list->EndQuery(as.stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, pair * 2 + 1);
    list->ResolveQueryData(as.stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, pair * 2, 2, as.stampReadback, u64(pair) * 16);
}

AsyncJob* AcquireJob(DeviceAsync& as, const char* name) // as.lock held
{
    AsyncJob* job = nullptr;
    for (AsyncJob* candidate : as.jobs)
    {
        if (candidate->state.load(std::memory_order_acquire) == AsyncJob::Free)
        {
            job = candidate;
            break;
        }
    }
    if (!job)
    {
        job = new AsyncJob();
        try
        {
            as.jobs.push_back(job);
        }
        catch (...)
        {
            delete job;
            throw;
        }
    }
    std::snprintf(job->name, sizeof(job->name), "%s", name ? name : "job");
    job->recorder = nullptr;
    job->list = nullptr;
    job->listOwner = nullptr;
    job->inOrder = false;
    job->uses.clear();
    job->readbacks.clear();
    job->stamps = kNoStamps;
    job->done = job->directDone = job->forkOrder = 0;
    job->pendingMarkers = 0;
    job->joinCalled = false;
    job->state.store(AsyncJob::Recording, std::memory_order_release);
    return job;
}

// A recorder whose last run the GPU finished, with its list open on a reset allocator.
Recorder* AcquireRecorder(Device& dev, DeviceAsync& as) // as.lock held
{
    const D3D12_COMMAND_LIST_TYPE type = as.queue ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT;
    for (Recorder* r : as.recorders)
    {
        if (r->busy || r->type != type || (r->computeDone && !as.compute.IsComplete(r->computeDone)) ||
            (r->directDone && !dev.fence.IsComplete(r->directDone)))
            continue;
        if (FAILED(r->allocator->Reset()) || FAILED(r->list->Reset(r->allocator, nullptr)))
        {
            Log("! async compute: a job recorder could not be reset");
            return nullptr;
        }
        r->busy = true;
        return r;
    }
    auto* r = new Recorder();
    r->type = type;
    HRESULT hr = dev.d12->CreateCommandAllocator(type, IID_PPV_ARGS(&r->allocator));
    if (SUCCEEDED(hr))
        hr = dev.d12->CreateCommandList(0, type, r->allocator, nullptr, IID_PPV_ARGS(&r->list));
    if (FAILED(hr))
    {
        Log("! async compute: no job recorder (0x%08X)", unsigned(hr));
        if (r->allocator)
            r->allocator->Release();
        delete r;
        return nullptr;
    }
    r->list->SetName(L"xrD3D12 async job");
    if (SUCCEEDED(r->list->QueryInterface(IID_PPV_ARGS(&r->list4))) && r->list4)
        r->list4->Release();
    else
        r->list4 = nullptr;
    r->descriptors.Init(&dev.gpuRing);
    r->upload.Init(&dev.uploadPages);
    try
    {
        as.recorders.push_back(r);
    }
    catch (...)
    {
        r->list->Release();
        r->allocator->Release();
        delete r;
        throw;
    }
    r->busy = true;
    return r;
}

void AddUse(AsyncJob& job, Resource* resource, u32 subresource, D3D12_RESOURCE_STATES entry, D3D12_RESOURCE_STATES exit, bool shared)
{
    Use use;
    use.resource = resource->Actual();
    use.object = ComOf(use.resource);
    use.subresource = subresource;
    use.entry = entry;
    use.exit = exit;
    use.shared = shared;
    job.uses.push_back(use);
    if (use.object)
        use.object->AddRef();
}

// Back to the pool: references dropped, the list returned, the timestamps queued for reading once the
// GPU ran them. Render thread (d12_api.h: jobs live on it).
void Recycle(Device& dev, AsyncJob* job)
{
    DeviceAsync& as = *dev.async;
    for (Use& use : job->uses)
    {
        if (use.resource->lentTo == job)
        {
            use.resource->lentTo = nullptr;
            use.resource->lentShared = false;
        }
        if (use.object)
            use.object->Release();
    }
    job->uses.clear();
    for (Resource* r : job->readbacks)
    {
        if (r->lentTo == job)
            r->lentTo = nullptr;
    }
    ReleaseStagingReferences(job->readbacks);
    // A list that never reached the queue.
    if (job->list && !job->recorder)
    {
        if (job->listOwner)
            job->listOwner->ReturnList(job->list);
        else
            job->list->Release();
    }
    job->list = nullptr;
    job->listOwner = nullptr;
    {
        std::lock_guard lock(as.lock);
        if (job->recorder)
            job->recorder->busy = false;
        job->recorder = nullptr;
        if (job->stamps != kNoStamps)
        {
            const bool ran = job->done || job->directDone;
            if (ran)
            {
                DeviceAsync::PendingStamps pending;
                pending.pair = job->stamps;
                std::memcpy(pending.name, job->name, sizeof(pending.name));
                pending.compute = job->done != 0;
                pending.fence = job->done ? job->done : job->directDone;
                try
                {
                    as.pendingStamps.push_back(pending);
                }
                catch (...)
                {
                    as.stampBusy[job->stamps] = false;
                }
            }
            else
                as.stampBusy[job->stamps] = false;
        }
        job->stamps = kNoStamps;
        job->state.store(AsyncJob::Free, std::memory_order_release);
    }
    const auto it = std::find(as.open.begin(), as.open.end(), job);
    if (it != as.open.end())
        as.open.erase(it);
}

void TryRecycle(Device& dev, AsyncJob* job)
{
    if (job->state.load(std::memory_order_acquire) == AsyncJob::Joined && job->joinCalled && !job->pendingMarkers)
        Recycle(dev, job);
}

// The last GPU times of finished jobs (timestamps of both ends, the frequency of the queue it ran on).
void ReadStamps(Device& dev, DeviceAsync& as) // as.lock held
{
    for (size_t i = 0; i < as.pendingStamps.size();)
    {
        const DeviceAsync::PendingStamps& p = as.pendingStamps[i];
        if (!(p.compute ? as.compute.IsComplete(p.fence) : dev.fence.IsComplete(p.fence)))
        {
            ++i;
            continue;
        }
        const u64 begin = as.stampData ? as.stampData[p.pair * 2] : 0;
        const u64 end = as.stampData ? as.stampData[p.pair * 2 + 1] : 0;
        const u64 frequency = p.compute ? as.computeFrequency : dev.timestampFrequency;
        const float ms = frequency && end > begin ? float(double(end - begin) * 1000.0 / double(frequency)) : 0.0f;
        const auto it = std::find_if(as.times.begin(), as.times.end(), [&](const auto& t) { return t.first == p.name; });
        if (it != as.times.end())
            it->second = ms;
        else
        {
            try
            {
                as.times.emplace_back(p.name, ms);
            }
            catch (...)
            {
            }
        }
        as.stampBusy[p.pair] = false;
        as.pendingStamps[i] = as.pendingStamps.back();
        as.pendingStamps.pop_back();
    }
}
} // namespace

// ================================================================================================
// Device: queue, fences, pools
// ================================================================================================

void Device::AsyncInit()
{
    try
    {
        async = new DeviceAsync();
    }
    catch (...)
    {
        async = nullptr;
        Log("! async compute: out of memory; jobs are unavailable");
        return;
    }
    DeviceAsync& as = *async;

    // Timestamps of the jobs: one heap, a pair per job.
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kStampPairs * 2;
    if (SUCCEEDED(d12->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&as.stampHeap))))
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kStampPairs * 16;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr;
        if (SUCCEEDED(d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&as.stampReadback))) &&
            SUCCEEDED(as.stampReadback->Map(0, nullptr, &mapped)))
            as.stampData = static_cast<const u64*>(mapped);
    }
    if (!as.stampData)
    {
        Log("! async compute: no timestamp heap; job times read 0");
        if (as.stampReadback)
            as.stampReadback->Release();
        if (as.stampHeap)
            as.stampHeap->Release();
        as.stampReadback = nullptr;
        as.stampHeap = nullptr;
    }

    // Every D3D12 device has compute queues; this one is made by the first job (AsyncQueue).
    caps.asyncCompute = !(disabledFeatures & NativeFeatureAsyncCompute);
}

void Device::AsyncShutdown()
{
    if (!async)
        return;
    DeviceAsync& as = *async;
    for (AsyncJob* job : as.jobs)
    {
        for (Use& use : job->uses)
        {
            use.resource->lentTo = nullptr;
            if (use.object)
                use.object->Release();
        }
        job->uses.clear();
        ReleaseStagingReferences(job->readbacks);
        if (job->list && !job->recorder)
        {
            if (job->listOwner)
                job->listOwner->ReturnList(job->list);
            else
                job->list->Release();
        }
        delete job;
    }
    as.jobs.clear();
    as.open.clear();
    for (Recorder* r : as.recorders)
    {
        r->list->Release();
        r->allocator->Release();
        delete r;
    }
    as.recorders.clear();
    for (auto& [fence, list] : as.graveyard)
        list->Release();
    as.graveyard.clear();
    if (as.stampReadback)
    {
        as.stampReadback->Unmap(0, nullptr);
        as.stampReadback->Release();
    }
    if (as.stampHeap)
        as.stampHeap->Release();
    if (as.queue)
    {
        as.cross.Shutdown();
        as.compute.Shutdown();
        as.queue->Release();
    }
    computeQueue = nullptr;
    delete async;
    async = nullptr;
}

void Device::AsyncWaitIdle()
{
    if (async && async->queue)
        async->compute.Wait(async->compute.Signal(async->queue));
}

void Device::AsyncEndFrame(u64 frame, u64 next)
{
    if (!async)
        return;
    DeviceAsync& as = *async;
    // Old frames' compute work is done too, whatever the joins did (frames forced without Present).
    if (as.queue)
    {
        as.frameCompute[frame % kFramesInFlight] = as.compute.NextValue() - 1;
        if (next > kFramesInFlight)
            as.compute.Wait(as.frameCompute[next % kFramesInFlight]);
    }
    std::lock_guard lock(as.lock);
    ReadStamps(*this, as);
    for (size_t i = 0; i < as.graveyard.size();)
    {
        if (as.compute.IsComplete(as.graveyard[i].first))
        {
            as.graveyard[i].second->Release();
            as.graveyard[i] = as.graveyard.back();
            as.graveyard.pop_back();
        }
        else
            ++i;
    }
}

void Device::AsyncFrameBoundary()
{
    if (!async || !immediate)
        return;
    DeviceAsync& as = *async;
    try
    {
        const std::vector<AsyncJob*> open = as.open;
        for (AsyncJob* job : open)
        {
            if (job->state.load(std::memory_order_acquire) == AsyncJob::Forked)
            {
                static constexpr char kFormat[] = "! async job %s not joined, joined at Present";
                LogOnce(kFormat, kFormat, job->name);
                immediate->JoinJob(job, false);
            }
        }
        // The caller's pointers expire with the frame.
        const std::vector<AsyncJob*> joined = as.open;
        for (AsyncJob* job : joined)
        {
            job->joinCalled = true;
            TryRecycle(*this, job);
        }
        // Ended and never forked: dropped.
        std::vector<AsyncJob*> stale;
        {
            std::lock_guard lock(as.lock);
            for (AsyncJob* job : as.jobs)
            {
                if (job->state.load(std::memory_order_acquire) == AsyncJob::Closed)
                    stale.push_back(job);
            }
        }
        for (AsyncJob* job : stale)
        {
            static constexpr char kFormat[] = "! async job %s was ended but never forked: dropped at Present";
            LogOnce(kFormat, kFormat, job->name);
            Recycle(*this, job);
        }
    }
    catch (const std::bad_alloc&)
    {
        Log("! async compute: out of memory at a frame boundary");
    }
}

bool Device::AsyncJobsPending()
{
    if (!async)
        return false;
    std::lock_guard lock(async->lock);
    return std::any_of(async->jobs.begin(), async->jobs.end(), [](const AsyncJob* job) {
        const u8 state = job->state.load(std::memory_order_acquire);
        return state != AsyncJob::Free && state != AsyncJob::Joined;
    });
}

void Device::AsyncForgetContext(Context* context)
{
    if (!async)
        return;
    std::lock_guard lock(async->lock);
    for (AsyncJob* job : async->jobs)
    {
        if (job->listOwner == context)
            job->listOwner = nullptr;
    }
}

void Device::AsyncDropStamps(u32 pair)
{
    if (!async || pair >= kStampPairs)
        return;
    std::lock_guard lock(async->lock);
    async->stampBusy[pair] = false;
}

// ---- Context::Submit ------------------------------------------------------------------------------

void Device::AsyncSubmitFork(AsyncJob* job, bool& crossStale)
{
    DeviceAsync& as = *async;
    if (crossStale || !as.crossValue)
    {
        as.crossValue = as.cross.Signal(queue);
        crossStale = false;
    }
    as.queue->Wait(as.cross.Native(), as.crossValue);
    if (job->list)
    {
        ID3D12CommandList* list = job->list;
        as.queue->ExecuteCommandLists(1, &list);
    }
    job->done = as.compute.Signal(as.queue);
    if (job->recorder)
        job->recorder->computeDone = job->done;
    else if (job->list)
    {
        // A pooled list of a deferred context can be reset once executed; its allocator waits for the frame.
        if (job->listOwner)
            job->listOwner->ReturnList(job->list);
        else
        {
            std::lock_guard lock(as.lock);
            try
            {
                as.graveyard.emplace_back(job->done, job->list);
            }
            catch (...)
            {
                as.compute.Wait(job->done);
                job->list->Release();
            }
        }
    }
    job->list = nullptr;
    job->listOwner = nullptr;
    --job->pendingMarkers;
    TryRecycle(*this, job);
}

void Device::AsyncSubmitJoin(AsyncJob* job)
{
    if (job->done)
        queue->Wait(async->compute.Native(), job->done);
    --job->pendingMarkers;
    TryRecycle(*this, job);
}

void Device::AsyncSubmitInOrder(AsyncJob* job, ID3D12GraphicsCommandList* list, Context* owner, u64 fenceValue)
{
    job->directDone = fenceValue;
    if (job->recorder)
        job->recorder->directDone = fenceValue;
    else if (owner)
        owner->ReturnList(list);
    else
    {
        try
        {
            releaser.Release(list);
        }
        catch (...)
        {
            // Releasing now could free a list the GPU still runs: leaking is the safe failure.
        }
    }
    job->list = nullptr;
    job->listOwner = nullptr;
    --job->pendingMarkers;
    TryRecycle(*this, job);
}

// ================================================================================================
// Context: fork, join, guard, compute recording
// ================================================================================================

void Context::ForkJob(AsyncJob* job)
{
    if (!job)
        return;
    if (m_deferred)
    {
        static constexpr char kFormat[] = "! ForkAsyncCompute on a deferred context ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    // The fork would close the list native code is recording into.
    if (m_inNative)
    {
        static constexpr char kFormat[] = "! ForkAsyncCompute inside a native section ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    if (job->state.load(std::memory_order_acquire) == AsyncJob::Recording)
    {
        static constexpr char kFormat[] = "! async job %s forked before EndAsyncCompute: ended at the fork";
        LogOnce(kFormat, kFormat, job->name);
        EndAsyncCompute(job);
    }
    if (job->state.load(std::memory_order_acquire) != AsyncJob::Closed)
    {
        static constexpr char kFormat[] = "! async job %s forked twice or after it was dropped; ignored";
        LogOnce(kFormat, kFormat, job->name);
        return;
    }
    DeviceAsync& as = *m_device->async;
    try
    {
        // Its descriptors and upload memory carry the frame it was begun in and are reused once that
        // frame's fence passed: a job forked in a later frame is dropped, not run. It counts as joined, so
        // the caller's join (or Present) recycles it and the pointer stays valid until then.
        if (job->frame != m_device->frame.load(std::memory_order_acquire))
        {
            static constexpr char kFormat[] = "! async job %s forked in a later frame than it was begun: its work is dropped";
            LogOnce(kFormat, kFormat, job->name);
            if (job->recorder)
                job->list = nullptr;
            as.open.push_back(job);
            job->state.store(AsyncJob::Joined, std::memory_order_release);
            return;
        }
        // Entry states on the direct queue. Buffers decay to COMMON at the job's boundaries and promote on
        // either queue (their declared states are not used); exclusive textures go to exactly the state
        // the job starts from.
        for (const Use& use : job->uses)
        {
            if (!use.resource->IsBuffer())
                RequireExact(use.resource, use.subresource, use.entry, !use.shared);
        }
        job->forkOrder = ++as.forks;
        as.open.push_back(job);
        m_device->Count(Device::StatForks);

        if (job->inOrder)
        {
            // No compute queue: the job's list runs here, in a submission call of its own.
            if (job->list)
            {
                QueueJobEntry(job, false, job->list, job->listOwner);
                job->pendingMarkers = 1;
                m_device->DecayTrackedBuffers();
            }
            else
                FlushBarriers();
            for (const Use& use : job->uses)
            {
                if (!use.shared && !use.resource->IsBuffer())
                    AdoptState(use.resource, use.subresource, use.exit);
            }
            for (Resource* r : job->readbacks)
                r->gpuWritePending = true;
            pendingReadback.insert(pendingReadback.end(), job->readbacks.begin(), job->readbacks.end());
            job->readbacks.clear();
            job->state.store(AsyncJob::Joined, std::memory_order_release);
            return;
        }

        QueueJobEntry(job, false, nullptr, nullptr);
        job->pendingMarkers = 1;
        m_device->DecayTrackedBuffers();
        // Loans: a later job takes one over (the compute queue runs jobs in order); a loan stays
        // read-shared only while every holder only reads.
        for (const Use& use : job->uses)
        {
            Resource* r = use.resource;
            if (r->lentTo != job)
            {
                r->lentShared = !r->lentTo || r->lentShared;
                r->lentTo = job;
            }
        }
        for (const Use& use : job->uses)
            use.resource->lentShared = use.resource->lentShared && use.shared;
        for (Resource* r : job->readbacks)
        {
            r->lentTo = job;
            r->lentShared = false;
        }
        job->state.store(AsyncJob::Forked, std::memory_order_release);
    }
    catch (const std::bad_alloc&)
    {
        Log("! async job %s: out of memory at the fork", job->name);
    }
}

void Context::JoinJob(AsyncJob* job, bool guard)
{
    if (!job)
        return;
    if (m_deferred)
    {
        static constexpr char kFormat[] = "! JoinAsyncCompute on a deferred context ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    // The join would close the list native code is recording into; Present joins the job instead.
    if (m_inNative)
    {
        static constexpr char kFormat[] = "! JoinAsyncCompute inside a native section ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    Device& dev = *m_device;
    DeviceAsync& as = *dev.async;
    if (!guard)
        job->joinCalled = true;
    switch (job->state.load(std::memory_order_acquire))
    {
    case AsyncJob::Joined:
        TryRecycle(dev, job);
        return;
    case AsyncJob::Forked: break;
    case AsyncJob::Free:
    {
        static constexpr char kFormat[] = "! JoinAsyncCompute of a recycled job ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    default:
    {
        if (!guard)
        {
            static constexpr char kFormat[] = "! async job %s joined without a fork: its work is dropped";
            LogOnce(kFormat, kFormat, job->name);
            Recycle(dev, job);
        }
        return;
    }
    }
    try
    {
        QueueJobEntry(job, true, nullptr, nullptr);
        ++job->pendingMarkers;
        m_device->DecayTrackedBuffers();
        m_device->Count(Device::StatJoins);
        if (guard)
            m_device->Count(Device::StatAutoJoins);
        // One compute queue in order: every job forked up to this one is done with it.
        const std::vector<AsyncJob*> open = as.open;
        for (AsyncJob* j : open)
        {
            if (j->forkOrder > job->forkOrder || j->state.load(std::memory_order_acquire) != AsyncJob::Forked)
                continue;
            for (const Use& use : j->uses)
            {
                if (!use.shared && !use.resource->IsBuffer())
                    AdoptState(use.resource, use.subresource, use.exit);
            }
            for (const Use& use : j->uses)
            {
                if (use.resource->lentTo == j)
                {
                    use.resource->lentTo = nullptr;
                    use.resource->lentShared = false;
                }
            }
            // Staging copies get the fence of the submission that carries the join.
            for (Resource* r : j->readbacks)
            {
                if (r->lentTo == j)
                    r->lentTo = nullptr;
                r->gpuWritePending = true;
            }
            pendingReadback.insert(pendingReadback.end(), j->readbacks.begin(), j->readbacks.end());
            j->readbacks.clear();
            j->state.store(AsyncJob::Joined, std::memory_order_release);
            if (j != job)
                TryRecycle(dev, j);
        }
    }
    catch (const std::bad_alloc&)
    {
        Log("! async job %s: out of memory at the join", job->name);
    }
}

void Context::JoinLent(Resource* resource)
{
    Resource* r = resource ? resource->Actual() : nullptr;
    AsyncJob* job = r ? r->lentTo : nullptr;
    if (!job || m_deferred)
        return;
    if (m_inNative)
    {
        static constexpr char kFormat[] = "! a native section uses %s, lent to async job %s: join the job before the section";
        LogOnce(kFormat, kFormat, DebugName(r).c_str(), job->name);
        return;
    }
    static constexpr char kFormat[] = "async job %s joined early: the direct queue needs %s";
    LogOnce(kFormat, kFormat, job->name, DebugName(r).c_str());
    JoinJob(job, true);
    // Never left dangling, whatever state the job was in.
    if (r->lentTo == job)
    {
        r->lentTo = nullptr;
        r->lentShared = false;
    }
}

void Context::AsyncRecordingClosing(ID3D12GraphicsCommandList* list, CommandList* out)
{
    if (DeviceAsync* as = m_device->async; as && asyncStamps != kNoStamps)
    {
        StampEnd(*as, list, asyncStamps);
        if (out)
            out->asyncStamps = asyncStamps;
        else
            m_device->AsyncDropStamps(asyncStamps);
    }
    asyncStamps = kNoStamps;
    recordCompute = false;
}

// ================================================================================================
// Public API (d12_api.h)
// ================================================================================================

AsyncJob* BeginAsyncCompute(ID3D11Device* device, const char* name, NativeScope& scope)
{
    scope = NativeScope{};
    Device* dev = LayerDevice(device);
    if (!dev || !dev->async || dev->removed)
        return nullptr;
    DeviceAsync& as = *dev->async;
    // The recorder's list type and the job's queue follow the compute queue.
    AsyncQueue(*dev, as);
    AsyncJob* job = nullptr;
    Recorder* recorder = nullptr;
    u32 stamps = kNoStamps;
    try
    {
        std::lock_guard lock(as.lock);
        recorder = AcquireRecorder(*dev, as);
        if (!recorder)
            return nullptr;
        job = AcquireJob(as, name);
        stamps = AcquireStamps(as);
    }
    catch (const std::bad_alloc&)
    {
        if (recorder)
            recorder->busy = false;
        Log("! BeginAsyncCompute: out of memory");
        return nullptr;
    }
    ID3D12GraphicsCommandList* list = recorder->list;
    job->device = dev;
    job->frame = dev->frame.load(std::memory_order_acquire);
    job->recorder = recorder;
    job->inOrder = !as.queue;
    job->stamps = stamps;
    StampBegin(as, list, stamps);
    u32 generation = 0;
    ID3D12DescriptorHeap* heaps[2] = {dev->gpuRing.Heap(), dev->samplerTables.Current(generation)};
    list->SetDescriptorHeaps(heaps[1] ? 2 : 1, heaps);
    scope.list = list;
    scope.list4 = recorder->list4;
    scope.heap = dev->gpuRing.Heap();
    scope.bindless = dev->gpuRing.Gpu(0);
    return job;
}

void AsyncUse(AsyncJob* job, ID3D11Resource* resource, D3D12_RESOURCE_STATES entry, D3D12_RESOURCE_STATES exit)
{
    Resource* r = Resource::From(resource);
    if (!job || !r || job->state.load(std::memory_order_acquire) != AsyncJob::Recording)
        return;
    r = r->Actual();
    // Pages and upload memory need no hand-off.
    if (!r->native)
        return;
    // Only read, in and out: lent read-shared in a widened read state, so the direct queue keeps sampling
    // it in pixel shaders while the job runs (compute reads accept PIXEL|NON_PIXEL).
    const bool shared = entry == exit && ReadOnly(entry);
    const u32 checked = u32(entry | exit) & ~u32(shared ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : 0);
    if (!ComputeLegal(static_cast<D3D12_RESOURCE_STATES>(checked)))
    {
        static constexpr char kFormat[] = "! AsyncUse: state 0x%X -> 0x%X is not compute-legal; the use is ignored";
        LogOnce(kFormat, kFormat, unsigned(entry), unsigned(exit));
        return;
    }
    const D3D12_RESOURCE_STATES state = shared ? static_cast<D3D12_RESOURCE_STATES>(entry | kStateShaderRead) : entry;
    try
    {
        AddUse(*job, r, kAll, state, shared ? state : exit, shared);
    }
    catch (const std::bad_alloc&)
    {
        Log("! AsyncUse: out of memory; the resource is not handed over");
    }
}

DescriptorSpan AsyncDescriptors(AsyncJob* job, UINT count)
{
    if (!job || !job->recorder || !count || job->state.load(std::memory_order_acquire) != AsyncJob::Recording)
        return {};
    Device* dev = job->device;
    // A quarter of the ring at most, like AllocateDescriptors.
    const u32 limit = (dev->gpuRing.Capacity() - dev->gpuRing.Base()) / 4;
    if (count > limit)
    {
        static constexpr char kFormat[] = "! AsyncDescriptors: %u descriptors exceed the per-request limit (%u)";
        LogOnce(kFormat, kFormat, count, limit);
        return {};
    }
    const u32 index = job->recorder->descriptors.Allocate(count, dev->frame.load(std::memory_order_relaxed));
    DescriptorSpan span;
    span.cpu = dev->gpuRing.Cpu(index);
    span.gpu = dev->gpuRing.Gpu(index);
    span.index = index;
    span.count = count;
    span.increment = dev->gpuRing.Increment();
    return span;
}

UploadSpan AsyncUpload(AsyncJob* job, UINT64 size, UINT64 alignment)
{
    if (!job || !job->recorder || !size || job->state.load(std::memory_order_acquire) != AsyncJob::Recording)
        return {};
    Device* dev = job->device;
    const UploadAlloc alloc =
        job->recorder->upload.Allocate(size, alignment ? alignment : 1, dev->frame.load(std::memory_order_acquire));
    if (!alloc.cpu)
        return {};
    UploadSpan span;
    span.cpu = alloc.cpu;
    span.gpu = alloc.gpu;
    span.resource = alloc.resource;
    span.offset = alloc.offset;
    return span;
}

void EndAsyncCompute(AsyncJob* job)
{
    if (!job || !job->recorder || job->state.load(std::memory_order_acquire) != AsyncJob::Recording)
        return;
    ID3D12GraphicsCommandList* list = job->recorder->list;
    StampEnd(*job->device->async, list, job->stamps);
    const HRESULT hr = list->Close();
    if (FAILED(hr))
    {
        // Nothing to run: the fork and join still keep their order and hand-offs.
        Log("! async job %s: its list does not close (0x%08X); the work is dropped", job->name, unsigned(hr));
        job->list = nullptr;
    }
    else
        job->list = list;
    job->state.store(AsyncJob::Closed, std::memory_order_release);
}

void DiscardAsyncCompute(AsyncJob* job)
{
    if (!job)
        return;
    switch (job->state.load(std::memory_order_acquire))
    {
    case AsyncJob::Recording:
        if (job->recorder)
            job->recorder->list->Close();
        job->list = nullptr;
        [[fallthrough]];
    case AsyncJob::Closed:
        // A recorder's list never reached the GPU: its allocator is reusable at once.
        if (job->recorder)
            job->list = nullptr;
        Recycle(*job->device, job);
        break;
    default:
    {
        static constexpr char kFormat[] = "! DiscardAsyncCompute of a forked or recycled job ignored (%s)";
        LogOnce(kFormat, kFormat, job->name);
        break;
    }
    }
}

void ForkAsyncCompute(ID3D11DeviceContext* immediate, AsyncJob* job)
{
    if (immediate)
        static_cast<Context*>(immediate)->ForkJob(job);
}

void JoinAsyncCompute(ID3D11DeviceContext* immediate, AsyncJob* job)
{
    if (immediate)
        static_cast<Context*>(immediate)->JoinJob(job, false);
}

float AsyncComputeMs(ID3D11Device* device, const char* name)
{
    Device* dev = LayerDevice(device);
    if (!dev || !dev->async || !name)
        return 0.0f;
    std::lock_guard lock(dev->async->lock);
    for (const auto& [key, ms] : dev->async->times)
    {
        if (key == name)
            return ms;
    }
    return 0.0f;
}

bool BeginAsyncRecording(ID3D11DeviceContext* deferred)
{
    if (!deferred)
        return false;
    Context* c = static_cast<Context*>(deferred);
    Device* dev = c->device();
    if (!c->IsDeferred())
    {
        static constexpr char kFormat[] = "! BeginAsyncRecording on the immediate context refused";
        LogOnce(kFormat, kFormat);
        return false;
    }
    if (!dev->async || dev->removed)
        return false;
    if (c->recordCompute)
    {
        static constexpr char kFormat[] = "! BeginAsyncRecording refused: the deferred context is still recording a job (FinishCommandList first)";
        LogOnce(kFormat, kFormat);
        return false;
    }
    if (c->HasOpenList())
    {
        static constexpr char kFormat[] = "! BeginAsyncRecording refused: the deferred context has recorded work (call it after FinishCommandList)";
        LogOnce(kFormat, kFormat);
        return false;
    }
    // The list opened below is a COMPUTE one only with the compute queue.
    AsyncQueue(*dev, *dev->async);
    c->recordCompute = true;
    try
    {
        ID3D12GraphicsCommandList* list = c->List();
        if (!list)
        {
            c->recordCompute = false;
            return false;
        }
        {
            std::lock_guard lock(dev->async->lock);
            c->asyncStamps = AcquireStamps(*dev->async);
        }
        StampBegin(*dev->async, list, c->asyncStamps);
    }
    catch (const std::bad_alloc&)
    {
        c->recordCompute = false;
        return false;
    }
    return true;
}

AsyncJob* AsyncJobFromCommandList(ID3D11Device* device, const char* name, ID3D11CommandList* list)
{
    Device* dev = LayerDevice(device);
    if (!dev || !dev->async || !list)
        return nullptr;
    CommandList* cl = static_cast<CommandList*>(list);
    if (!cl->list)
    {
        static constexpr char kFormat[] = "! AsyncJobFromCommandList: the list was executed already";
        LogOnce(kFormat, kFormat);
        return nullptr;
    }
    DeviceAsync& as = *dev->async;
    AsyncJob* job = nullptr;
    try
    {
        {
            std::lock_guard lock(as.lock);
            job = AcquireJob(as, name);
        }
        job->device = dev;
        job->frame = dev->frame.load(std::memory_order_acquire);
        job->list = cl->list;
        job->listOwner = cl->recordedBy;
        // A DIRECT list (no compute queue, or not a compute recording) runs in order on the direct queue.
        job->inOrder = cl->list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE;
        job->stamps = cl->asyncStamps;
        cl->asyncStamps = kNoStamps;
        cl->list = nullptr;
        job->readbacks.swap(cl->pendingReadback);
        job->uses.reserve(cl->requirements.size());
        for (const CommandList::Requirement& req : cl->requirements)
        {
            // Only read and never transitioned by the list: lent read-shared in a widened read state the
            // direct queue keeps sampling in; otherwise exclusive from its exact first state to its last.
            const bool shared = !req.transitioned && ReadOnly(req.first);
            const D3D12_RESOURCE_STATES entry = shared ? static_cast<D3D12_RESOURCE_STATES>(req.first | kStateShaderRead) : req.first;
            AddUse(*job, req.resource, req.subresource, entry, shared ? entry : req.last, shared);
        }
        job->state.store(AsyncJob::Closed, std::memory_order_release);
        return job;
    }
    catch (const std::bad_alloc&)
    {
        Log("! AsyncJobFromCommandList: out of memory");
        if (job)
            Recycle(*dev, job);
        return nullptr;
    }
}
} // namespace xrd3d12
