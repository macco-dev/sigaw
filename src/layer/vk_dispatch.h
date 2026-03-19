#ifndef SIGAW_VK_DISPATCH_H
#define SIGAW_VK_DISPATCH_H

#include <string.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIGAW_VK_DEVICE_DISPATCH_FUNCTIONS(X) \
    X(PFN_vkAllocateCommandBuffers, AllocateCommandBuffers, "vkAllocateCommandBuffers") \
    X(PFN_vkAllocateDescriptorSets, AllocateDescriptorSets, "vkAllocateDescriptorSets") \
    X(PFN_vkAllocateMemory, AllocateMemory, "vkAllocateMemory") \
    X(PFN_vkBeginCommandBuffer, BeginCommandBuffer, "vkBeginCommandBuffer") \
    X(PFN_vkBindBufferMemory, BindBufferMemory, "vkBindBufferMemory") \
    X(PFN_vkBindImageMemory, BindImageMemory, "vkBindImageMemory") \
    X(PFN_vkCmdBeginRenderPass, CmdBeginRenderPass, "vkCmdBeginRenderPass") \
    X(PFN_vkCmdBindDescriptorSets, CmdBindDescriptorSets, "vkCmdBindDescriptorSets") \
    X(PFN_vkCmdBindPipeline, CmdBindPipeline, "vkCmdBindPipeline") \
    X(PFN_vkCmdCopyBufferToImage, CmdCopyBufferToImage, "vkCmdCopyBufferToImage") \
    X(PFN_vkCmdCopyImageToBuffer, CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") \
    X(PFN_vkCmdDraw, CmdDraw, "vkCmdDraw") \
    X(PFN_vkCmdEndRenderPass, CmdEndRenderPass, "vkCmdEndRenderPass") \
    X(PFN_vkCmdPipelineBarrier, CmdPipelineBarrier, "vkCmdPipelineBarrier") \
    X(PFN_vkCmdPushConstants, CmdPushConstants, "vkCmdPushConstants") \
    X(PFN_vkCmdSetScissor, CmdSetScissor, "vkCmdSetScissor") \
    X(PFN_vkCmdSetViewport, CmdSetViewport, "vkCmdSetViewport") \
    X(PFN_vkCreateBuffer, CreateBuffer, "vkCreateBuffer") \
    X(PFN_vkCreateCommandPool, CreateCommandPool, "vkCreateCommandPool") \
    X(PFN_vkCreateDescriptorPool, CreateDescriptorPool, "vkCreateDescriptorPool") \
    X(PFN_vkCreateDescriptorSetLayout, CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout") \
    X(PFN_vkCreateFence, CreateFence, "vkCreateFence") \
    X(PFN_vkCreateFramebuffer, CreateFramebuffer, "vkCreateFramebuffer") \
    X(PFN_vkCreateGraphicsPipelines, CreateGraphicsPipelines, "vkCreateGraphicsPipelines") \
    X(PFN_vkCreateImage, CreateImage, "vkCreateImage") \
    X(PFN_vkCreateImageView, CreateImageView, "vkCreateImageView") \
    X(PFN_vkCreatePipelineLayout, CreatePipelineLayout, "vkCreatePipelineLayout") \
    X(PFN_vkCreateRenderPass, CreateRenderPass, "vkCreateRenderPass") \
    X(PFN_vkCreateSampler, CreateSampler, "vkCreateSampler") \
    X(PFN_vkCreateSemaphore, CreateSemaphore, "vkCreateSemaphore") \
    X(PFN_vkCreateShaderModule, CreateShaderModule, "vkCreateShaderModule") \
    X(PFN_vkDestroyBuffer, DestroyBuffer, "vkDestroyBuffer") \
    X(PFN_vkDestroyCommandPool, DestroyCommandPool, "vkDestroyCommandPool") \
    X(PFN_vkDestroyDescriptorPool, DestroyDescriptorPool, "vkDestroyDescriptorPool") \
    X(PFN_vkDestroyDescriptorSetLayout, DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") \
    X(PFN_vkDestroyFence, DestroyFence, "vkDestroyFence") \
    X(PFN_vkDestroyFramebuffer, DestroyFramebuffer, "vkDestroyFramebuffer") \
    X(PFN_vkDestroyImage, DestroyImage, "vkDestroyImage") \
    X(PFN_vkDestroyImageView, DestroyImageView, "vkDestroyImageView") \
    X(PFN_vkDestroyPipeline, DestroyPipeline, "vkDestroyPipeline") \
    X(PFN_vkDestroyPipelineLayout, DestroyPipelineLayout, "vkDestroyPipelineLayout") \
    X(PFN_vkDestroyRenderPass, DestroyRenderPass, "vkDestroyRenderPass") \
    X(PFN_vkDestroySampler, DestroySampler, "vkDestroySampler") \
    X(PFN_vkDestroySemaphore, DestroySemaphore, "vkDestroySemaphore") \
    X(PFN_vkDestroyShaderModule, DestroyShaderModule, "vkDestroyShaderModule") \
    X(PFN_vkDeviceWaitIdle, DeviceWaitIdle, "vkDeviceWaitIdle") \
    X(PFN_vkEndCommandBuffer, EndCommandBuffer, "vkEndCommandBuffer") \
    X(PFN_vkFreeMemory, FreeMemory, "vkFreeMemory") \
    X(PFN_vkGetBufferMemoryRequirements, GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements") \
    X(PFN_vkGetImageMemoryRequirements, GetImageMemoryRequirements, "vkGetImageMemoryRequirements") \
    X(PFN_vkMapMemory, MapMemory, "vkMapMemory") \
    X(PFN_vkQueueSubmit, QueueSubmit, "vkQueueSubmit") \
    X(PFN_vkResetCommandBuffer, ResetCommandBuffer, "vkResetCommandBuffer") \
    X(PFN_vkResetFences, ResetFences, "vkResetFences") \
    X(PFN_vkUnmapMemory, UnmapMemory, "vkUnmapMemory") \
    X(PFN_vkUpdateDescriptorSets, UpdateDescriptorSets, "vkUpdateDescriptorSets") \
    X(PFN_vkWaitForFences, WaitForFences, "vkWaitForFences")

typedef struct SigawVulkanDispatch {
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
#define SIGAW_VK_DISPATCH_FIELD(type, field, symbol) type field;
    SIGAW_VK_DEVICE_DISPATCH_FUNCTIONS(SIGAW_VK_DISPATCH_FIELD)
#undef SIGAW_VK_DISPATCH_FIELD
} SigawVulkanDispatch;

static inline void sigaw_vk_dispatch_clear(SigawVulkanDispatch* dispatch) {
    if (dispatch) {
        memset(dispatch, 0, sizeof(*dispatch));
    }
}

static inline int sigaw_vk_dispatch_complete(const SigawVulkanDispatch* dispatch) {
    if (!dispatch || !dispatch->GetPhysicalDeviceMemoryProperties) {
        return 0;
    }

#define SIGAW_VK_DISPATCH_CHECK(type, field, symbol) \
    if (!dispatch->field) {                          \
        return 0;                                    \
    }
    SIGAW_VK_DEVICE_DISPATCH_FUNCTIONS(SIGAW_VK_DISPATCH_CHECK)
#undef SIGAW_VK_DISPATCH_CHECK

    return 1;
}

static inline int sigaw_vk_dispatch_resolve(SigawVulkanDispatch* dispatch,
                                            PFN_vkGetInstanceProcAddr get_instance_proc,
                                            VkInstance instance,
                                            PFN_vkGetDeviceProcAddr get_device_proc,
                                            VkDevice device) {
    if (!dispatch) {
        return 0;
    }

    sigaw_vk_dispatch_clear(dispatch);

    if (get_instance_proc && instance != VK_NULL_HANDLE) {
        dispatch->GetPhysicalDeviceMemoryProperties =
            (PFN_vkGetPhysicalDeviceMemoryProperties)
                get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties");
    }

    if (get_device_proc && device != VK_NULL_HANDLE) {
#define SIGAW_VK_DISPATCH_RESOLVE(type, field, symbol) \
        dispatch->field = (type)get_device_proc(device, symbol);
        SIGAW_VK_DEVICE_DISPATCH_FUNCTIONS(SIGAW_VK_DISPATCH_RESOLVE)
#undef SIGAW_VK_DISPATCH_RESOLVE
    }

    return sigaw_vk_dispatch_complete(dispatch);
}

#ifdef __cplusplus
}
#endif

#endif /* SIGAW_VK_DISPATCH_H */
