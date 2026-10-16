// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The Vulkan entry points of xrVulkan, resolved at runtime: xrEngine.exe imports every renderer module
// statically and must still start where vulkan-1.dll is missing. Global functions come from the loader,
// instance functions from vkGetInstanceProcAddr, device functions from vkGetDeviceProcAddr (no loader
// trampoline on the hot path). Extension functions are null when the extension was not enabled.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

namespace xrvk
{
// clang-format off
#define XRVK_GLOBAL_FUNCTIONS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)

#define XRVK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFormatProperties2) \
    X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkCreateWin32SurfaceKHR) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilities2KHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceFragmentShadingRatesKHR) \
    X(vkCreateDebugUtilsMessengerEXT) \
    X(vkDestroyDebugUtilsMessengerEXT)

#define XRVK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkQueueSubmit) \
    X(vkQueueSubmit2) \
    X(vkQueueWaitIdle) \
    X(vkDeviceWaitIdle) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkFlushMappedMemoryRanges) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetBufferMemoryRequirements2) \
    X(vkGetImageMemoryRequirements2) \
    X(vkGetImageSubresourceLayout) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkResetFences) \
    X(vkGetFenceStatus) \
    X(vkWaitForFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkGetSemaphoreCounterValue) \
    X(vkWaitSemaphores) \
    X(vkSignalSemaphore) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkGetQueryPoolResults) \
    X(vkResetQueryPool) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkCreateBufferView) \
    X(vkDestroyBufferView) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineCache) \
    X(vkDestroyPipelineCache) \
    X(vkGetPipelineCacheData) \
    X(vkMergePipelineCaches) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateDescriptorUpdateTemplate) \
    X(vkDestroyDescriptorUpdateTemplate) \
    X(vkUpdateDescriptorSetWithTemplate) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdSetViewportWithCount) \
    X(vkCmdSetScissorWithCount) \
    X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetCullMode) \
    X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) \
    X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthBoundsTestEnable) \
    X(vkCmdSetStencilTestEnable) \
    X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetPrimitiveRestartEnable) \
    X(vkCmdSetPatchControlPointsEXT) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindVertexBuffers2) \
    X(vkCmdPushConstants) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdDrawIndirect) \
    X(vkCmdDrawIndexedIndirect) \
    X(vkCmdDrawIndirectCount) \
    X(vkCmdDrawIndexedIndirectCount) \
    X(vkCmdDispatch) \
    X(vkCmdDispatchIndirect) \
    X(vkCmdCopyBuffer) \
    X(vkCmdCopyImage) \
    X(vkCmdBlitImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdUpdateBuffer) \
    X(vkCmdFillBuffer) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdClearAttachments) \
    X(vkCmdResolveImage) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdBeginQuery) \
    X(vkCmdEndQuery) \
    X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp2) \
    X(vkCmdCopyQueryPoolResults) \
    X(vkCmdExecuteCommands) \
    X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCmdPushDescriptorSetKHR) \
    X(vkCmdPushDescriptorSetWithTemplateKHR) \
    X(vkSetDebugUtilsObjectNameEXT) \
    X(vkCmdBeginDebugUtilsLabelEXT) \
    X(vkCmdEndDebugUtilsLabelEXT) \
    X(vkCmdInsertDebugUtilsLabelEXT) \
    X(vkCmdSetVertexInputEXT) \
    X(vkCmdSetColorBlendEnableEXT) \
    X(vkCmdSetColorBlendEquationEXT) \
    X(vkCmdSetColorWriteMaskEXT) \
    X(vkCmdSetPolygonModeEXT) \
    X(vkCmdSetDepthClampEnableEXT) \
    X(vkCmdSetAlphaToCoverageEnableEXT) \
    X(vkCmdSetSampleMaskEXT) \
    X(vkCmdSetRasterizationSamplesEXT) \
    X(vkCmdSetDepthClipEnableEXT) \
    X(vkCmdSetFragmentShadingRateKHR) \
    X(vkCmdDrawMeshTasksEXT) \
    X(vkCmdDrawMeshTasksIndirectCountEXT) \
    X(vkCmdDrawMultiEXT) \
    X(vkCmdDrawMultiIndexedEXT) \
    X(vkWaitForPresentKHR) \
    X(vkGetBufferDeviceAddress) \
    X(vkGetPipelineExecutablePropertiesKHR) \
    X(vkGetPipelineExecutableStatisticsKHR)
// clang-format on

struct Api
{
#define XRVK_DECLARE(name) PFN_##name name = nullptr;
    XRVK_GLOBAL_FUNCTIONS(XRVK_DECLARE)
    XRVK_INSTANCE_FUNCTIONS(XRVK_DECLARE)
    XRVK_DEVICE_FUNCTIONS(XRVK_DECLARE)
#undef XRVK_DECLARE
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

    // vulkan-1.dll and the global functions. Thread-safe, idempotent; false when Vulkan is not available.
    bool LoadLoader();
    // Instance functions of `instance` (functions of extensions not enabled stay null).
    void LoadInstance(VkInstance instance);
    // Device functions of `device`.
    void LoadDevice(VkDevice device);
};

// One table per process: the layer creates one instance and one device at a time (a second CreateDevice
// after a teardown reloads it).
extern Api g_vk;

// Result text for logs ("VK_ERROR_DEVICE_LOST").
const char* ResultText(VkResult result);
} // namespace xrvk
