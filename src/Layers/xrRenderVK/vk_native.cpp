// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The D3D11-path extensions of vk_api.h: indirect draws with a GPU count, mesh draws, variable rate
// shading and the UAV overlap hint. Engine draws keep using the D3D11 API; these change how they execute.
#include "vk_layer.h"

namespace xrvk
{
namespace
{
Context* ContextOf(ID3D11DeviceContext* context)
{
    if (!context)
        return nullptr;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    const bool ours = IsLayerDevice(device);
    if (device)
        device->Release();
    return ours ? static_cast<Context*>(context) : nullptr;
}
} // namespace

void DrawIndexedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count, UINT countOffset)
{
    if (Context* c = ContextOf(context))
        c->IndirectCount(true, args, offset, maxCount, count, countOffset);
}

void DrawInstancedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count, UINT countOffset)
{
    if (Context* c = ContextOf(context))
        c->IndirectCount(false, args, offset, maxCount, count, countOffset);
}

bool DispatchMesh(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    Context* c = ContextOf(context);
    if (!c || !c->device()->caps.meshShaders || !x || !y || !z)
        return false;
    if (!c->PrepareDraw(false, true))
        return false;
    g_vk.vkCmdDrawMeshTasksEXT(c->Cmd(), x, y, z);
    return true;
}

void SetShadingRate(ID3D11DeviceContext* context, UINT baseRate, UINT imageCombiner, ID3D11Resource* rateImage)
{
    Context* c = ContextOf(context);
    if (!c || !c->device()->caps.shadingRate)
        return;
    const bool useImage = imageCombiner != 0 && rateImage;
    if (c->shadingRate != baseRate || c->rateCombiner != imageCombiner)
    {
        c->shadingRate = baseRate;
        c->rateCombiner = imageCombiner;
        c->applied.rateDirty = true;
    }
    ID3D11Resource* image = useImage ? rateImage : nullptr;
    if (c->shadingRateImage != useImage || c->rateImage != image)
    {
        if (image)
            image->AddRef();
        if (c->rateImage)
            c->rateImage->Release();
        c->rateImage = image;
        c->shadingRateImage = useImage && image;
        // The rate image is an attachment of the rendering: a new one begins a new rendering.
        c->MarkTargetsDirty();
        c->applied.rateDirty = true;
        c->applied.pipelineDirty = true;
        c->applied.hazardsDirty = true;
    }
}

void BeginUavOverlap(ID3D11DeviceContext* context)
{
    if (Context* c = ContextOf(context))
        ++c->uavOverlap;
}

void EndUavOverlap(ID3D11DeviceContext* context)
{
    Context* c = ContextOf(context);
    if (!c || !c->uavOverlap)
        return;
    if (--c->uavOverlap == 0)
    {
        // D3D11 serialization again: the next UAV access waits for what the overlapped run wrote.
        c->applied.hazardsDirty = true;
        c->applied.computeHazardsDirty = true;
    }
}

void UavBarrier(ID3D11DeviceContext* context, ID3D11Resource* /*resource*/)
{
    Context* c = ContextOf(context);
    if (!c)
        return;
    // Folded into the merged memory barrier the next draw or dispatch flushes.
    c->memoryBarrier.srcStageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    c->memoryBarrier.srcAccessMask |= VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    c->memoryBarrier.dstStageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    c->memoryBarrier.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    c->memoryBarrierPending = true;
    c->applied.hazardsDirty = true;
    c->applied.computeHazardsDirty = true;
}

bool GetNativeDevice(ID3D11Device* device, NativeDeviceInfo& out, void** getDeviceProcAddr)
{
    out = {};
    if (!IsLayerDevice(device))
        return false;
    const Device* d = static_cast<const Device*>(device);
    out.instance = d->instance;
    out.physicalDevice = d->physical;
    out.device = d->vk;
    out.graphicsFamily = d->graphicsFamily;
    out.computeFamily = d->computeFamily != ~0u ? d->computeFamily : d->graphicsFamily;
    out.presentQueueIndex = d->presentQueueIndex;
    out.presentQueueCount = d->presentQueueCount;
    if (getDeviceProcAddr)
        *getDeviceProcAddr = reinterpret_cast<void*>(g_vk.vkGetDeviceProcAddr);
    return true;
}

int SubmitToQueue(ID3D11Device* device, Queue queue, UINT count, const void* submits, void* fence)
{
    if (!IsLayerDevice(device))
        return VK_ERROR_INITIALIZATION_FAILED;
    Device* d = Device::From(device);
    const bool compute = queue == Queue::Compute && d->computeQueue;
    std::lock_guard lock(compute ? d->computeQueueLock : d->graphicsQueueLock);
    const VkResult result = g_vk.vkQueueSubmit(compute ? d->computeQueue : d->graphicsQueue, count,
        static_cast<const VkSubmitInfo*>(submits), static_cast<VkFence>(fence));
    if (result == VK_ERROR_DEVICE_LOST)
        d->ReportLost(result);
    return result;
}

void* NativeQueue(ID3D11Device* device, Queue queue)
{
    if (!IsLayerDevice(device))
        return nullptr;
    const Device* d = Device::From(device);
    return queue == Queue::Compute && d->computeQueue ? d->computeQueue : d->graphicsQueue;
}

bool PrepareNativeImage(ID3D11DeviceContext* context, ID3D11Resource* resource, bool write, NativeImage& out)
{
    out = {};
    Context* c = ContextOf(context);
    Resource* r = resource ? Resource::From(resource) : nullptr;
    if (!c || c->IsDeferred() || !r || !r->IsImage() || !r->image || c->device()->lost)
        return false;
    // Clears folded into later load operations land now: none may follow the SDK's write.
    c->EndRendering();
    c->FlushPendingClears(nullptr);
    const VkImageLayout layout = write ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkAccessFlags2 access = write ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT :
                                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    c->RequireImageAll(r, layout, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, access);

    if (!r->nativeView)
    {
        const bool depth = (r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = r->image;
        info.viewType = r->imageInfo.imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        info.format = r->imageInfo.format;
        info.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : r->aspect, 0, 1, 0, 1};
        if (g_vk.vkCreateImageView(c->device()->vk, &info, nullptr, &r->nativeView) != VK_SUCCESS)
        {
            r->nativeView = VK_NULL_HANDLE;
            return false;
        }
    }
    out.image = r->image;
    out.memory = r->memory.memory;
    out.view = r->nativeView;
    out.layout = UINT(layout);
    out.format = UINT(r->imageInfo.format);
    out.width = r->width;
    out.height = r->height;
    out.mipLevels = r->mipLevels;
    out.arrayLayers = r->imageInfo.arrayLayers;
    out.createFlags = UINT(r->imageInfo.flags);
    out.usage = UINT(r->imageInfo.usage);
    return true;
}

void* BeginNative(ID3D11DeviceContext* context)
{
    Context* c = ContextOf(context);
    if (!c || c->IsDeferred() || c->device()->lost)
        return nullptr;
    const VkCommandBuffer cmd = c->Cmd();
    if (!cmd)
        return nullptr;
    c->EndRendering();
    c->FlushPendingClears(nullptr);
    c->FlushBarriers();
    return cmd;
}

void EndNative(ID3D11DeviceContext* context)
{
    if (Context* c = ContextOf(context))
        c->InvalidateApplied();
}

void* PersistentData(ID3D11Buffer* buffer)
{
    if (!buffer)
        return nullptr;
    ID3D11Device* device = nullptr;
    buffer->GetDevice(&device);
    const bool ours = IsLayerDevice(device);
    if (device)
        device->Release();
    if (!ours)
        return nullptr;
    Resource* r = Resource::From(buffer);
    return r->usage == D3D11_USAGE_STAGING && (r->cpuAccess & D3D11_CPU_ACCESS_WRITE) ? r->memory.cpu : nullptr;
}

bool QueryVideoMemory(ID3D11Device* device, UINT64& budget, UINT64& usage)
{
    budget = usage = 0;
    if (!IsLayerDevice(device))
        return false;
    Device* d = Device::From(device);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    if (d->caps.memoryBudget)
        props.pNext = &budgets;
    g_vk.vkGetPhysicalDeviceMemoryProperties2(d->physical, &props);
    for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i)
    {
        if (!(props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            continue;
        budget += d->caps.memoryBudget ? budgets.heapBudget[i] : props.memoryProperties.memoryHeaps[i].size;
        usage += d->caps.memoryBudget ? budgets.heapUsage[i] : 0;
    }
    return budget != 0;
}
} // namespace xrvk
