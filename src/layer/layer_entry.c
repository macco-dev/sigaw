/*
 * Sigaw - Discord Voice Overlay for Linux
 * Copyright (c) 2026 Macco
 * SPDX-License-Identifier: MIT
 *
 * layer_entry.c - Vulkan implicit layer dispatch table
 *
 * This file provides the C-linkage entry points that the Vulkan loader
 * calls. It chains to the next layer/driver for all functions, and
 * intercepts the ones we need to render the overlay.
 *
 * Architecture follows the Vulkan Layer Factory pattern:
 * https://github.com/LunarG/VulkanTools/tree/main/layer_factory
 */

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "swapchain_bookkeeping.h"
#include "vk_dispatch.h"
#include "wine_policy.h"

#if defined(__GNUC__) || defined(__clang__)
#define SIGAW_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define SIGAW_LAYER_EXPORT
#endif

/* Forward declarations for C++ overlay logic */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SigawOverlayContext SigawOverlayContext;

SigawOverlayContext* sigaw_overlay_create(VkDevice device, VkPhysicalDevice phys_device,
                                          VkInstance instance, uint32_t queue_family,
                                          VkQueue queue,
                                          const SigawVulkanDispatch* dispatch,
                                          PFN_vkGetPhysicalDeviceMemoryProperties get_phys_props,
                                          VkFormat format,
                                          uint32_t width, uint32_t height);
void sigaw_overlay_destroy(SigawOverlayContext* ctx);
void sigaw_overlay_resize(SigawOverlayContext* ctx, VkFormat format,
                          uint32_t width, uint32_t height);
VkSemaphore sigaw_overlay_acquire_present_semaphore(SigawOverlayContext* ctx);
void sigaw_overlay_recycle_present_semaphore(SigawOverlayContext* ctx,
                                             VkSemaphore semaphore);
void sigaw_overlay_discard_present_semaphore(SigawOverlayContext* ctx,
                                             VkSemaphore semaphore);
int sigaw_overlay_render(SigawOverlayContext* ctx, VkQueue queue,
                         VkImage target_image, VkFormat format,
                         uint32_t width, uint32_t height,
                         uint32_t wait_sem_count,
                         const VkSemaphore* wait_sems,
                         VkSemaphore signal_sem, VkFence fence);

#ifdef __cplusplus
}
#endif

/* -------------------------------------------------------------------------- */
/*  Dispatch table storage                                                     */
/* -------------------------------------------------------------------------- */

/*
 * We keep per-instance and per-device dispatch tables so we can chain
 * calls to the next layer. This is the standard Vulkan layer pattern.
 */

#define MAX_INSTANCES 8
#define MAX_DEVICES   8
#define MAX_DEVICE_QUEUES 16
#define MAX_INSTANCE_PHYSICAL_DEVICES 16

typedef struct {
    VkInstance                instance;
    PFN_vkGetInstanceProcAddr get_proc;
    PFN_vkDestroyInstance     destroy_instance;
    PFN_vkEnumeratePhysicalDevices enum_phys;
    PFN_vkGetPhysicalDeviceProperties get_phys_props;
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_props;
    VkPhysicalDevice          phys_devices[MAX_INSTANCE_PHYSICAL_DEVICES];
    uint32_t                  phys_device_count;
} InstanceData;

typedef struct {
    VkDevice                    device;
    VkPhysicalDevice            phys_device;
    VkInstance                  instance;
    PFN_vkGetDeviceProcAddr     get_proc;
    PFN_vkDestroyDevice         destroy_device;
    PFN_vkCreateSwapchainKHR    create_swapchain;
    PFN_vkDestroySwapchainKHR   destroy_swapchain;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images;
    PFN_vkQueuePresentKHR       queue_present;
    PFN_vkQueueSubmit           queue_submit;
    PFN_vkDeviceWaitIdle        device_wait_idle;
    PFN_vkAcquireNextImageKHR   acquire_next_image;
    PFN_vkAcquireNextImage2KHR  acquire_next_image2;
    uint32_t                    gfx_queue_family;
    VkQueue                     gfx_queue;
    VkQueue                     queues[MAX_DEVICE_QUEUES];
    uint32_t                    queue_families[MAX_DEVICE_QUEUES];
    uint32_t                    queue_count;
    uint32_t                    overlay_queue_family;
    SigawVulkanDispatch         dispatch;
    SigawWinePolicy             wine_policy;
    int                         under_wine;
    int                         overlay_disabled;
    int                         overlay_log_emitted;
    SigawOverlayContext*        overlay_ctx;
} DeviceData;

typedef SigawSwapchainData SwapchainData;

static InstanceData  g_instances[MAX_INSTANCES];
static int           g_instance_count = 0;

static DeviceData    g_devices[MAX_DEVICES];
static int           g_device_count = 0;

static SwapchainData g_swapchains[MAX_DEVICES * 4]; /* multiple swapchains per device */
static int           g_swapchain_count = 0;
static int           g_swapchain_full_warned = 0;

static void clear_swapchain_present_tracking(DeviceData* dev, SwapchainData* sc);
static void clear_device_present_tracking(DeviceData* dev);
static void wait_for_swapchain_present_tracking(DeviceData* dev, SwapchainData* sc);
static void wait_for_device_present_tracking(DeviceData* dev);
static void recycle_swapchain_present_semaphore(DeviceData* dev,
                                                SwapchainData* sc,
                                                uint32_t image_index);
static void discard_swapchain_present_semaphore(DeviceData* dev,
                                                SwapchainData* sc,
                                                uint32_t image_index);
static void track_swapchain_present_semaphore(DeviceData* dev,
                                              SwapchainData* sc,
                                              uint32_t image_index,
                                              VkSemaphore semaphore);
static PFN_vkAcquireNextImage2KHR resolve_acquire_next_image2(
    PFN_vkGetDeviceProcAddr get_device_proc,
    VkDevice device);

/* Lookup helpers */
static InstanceData* find_instance(VkInstance inst) {
    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i].instance == inst) return &g_instances[i];
    }
    return NULL;
}

static DeviceData* find_device(VkDevice dev) {
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].device == dev) return &g_devices[i];
    }
    return NULL;
}

static DeviceData* find_device_for_queue(VkQueue queue, uint32_t* queue_family) {
    for (int i = 0; i < g_device_count; i++) {
        for (uint32_t j = 0; j < g_devices[i].queue_count; j++) {
            if (g_devices[i].queues[j] == queue) {
                if (queue_family) {
                    *queue_family = g_devices[i].queue_families[j];
                }
                return &g_devices[i];
            }
        }
    }
    return NULL;
}

static SwapchainData* find_swapchain(VkSwapchainKHR sc) {
    return sigaw_find_swapchain(g_swapchains, g_swapchain_count, sc);
}

static void cache_instance_physical_devices(InstanceData* data) {
    if (!data || !data->enum_phys || data->instance == VK_NULL_HANDLE) {
        return;
    }

    data->phys_device_count = 0;

    uint32_t count = 0;
    if (data->enum_phys(data->instance, &count, NULL) != VK_SUCCESS || count == 0) {
        return;
    }

    if (count > MAX_INSTANCE_PHYSICAL_DEVICES) {
        count = MAX_INSTANCE_PHYSICAL_DEVICES;
    }

    VkPhysicalDevice phys[MAX_INSTANCE_PHYSICAL_DEVICES];
    if (data->enum_phys(data->instance, &count, phys) != VK_SUCCESS) {
        return;
    }

    memcpy(data->phys_devices, phys, sizeof(VkPhysicalDevice) * count);
    data->phys_device_count = count;
}

static InstanceData* find_instance_for_physical_device(VkPhysicalDevice phys_device) {
    for (int i = 0; i < g_instance_count; i++) {
        InstanceData* data = &g_instances[i];
        if (data->instance == VK_NULL_HANDLE) {
            continue;
        }

        if (data->phys_device_count == 0) {
            cache_instance_physical_devices(data);
        }

        for (uint32_t j = 0; j < data->phys_device_count; ++j) {
            if (data->phys_devices[j] == phys_device) {
                return data;
            }
        }
    }

    return NULL;
}

static void log_overlay_message(DeviceData* dev, const char* message) {
    if (!dev || dev->overlay_log_emitted || !message) {
        return;
    }

    fprintf(stderr, "%s\n", message);
    dev->overlay_log_emitted = 1;
}

static void disable_overlay(DeviceData* dev, const char* message) {
    if (!dev) {
        return;
    }

    if (dev->overlay_ctx) {
        wait_for_device_present_tracking(dev);
        clear_device_present_tracking(dev);
        sigaw_overlay_destroy(dev->overlay_ctx);
        dev->overlay_ctx = NULL;
    }

    dev->overlay_queue_family = UINT32_MAX;
    dev->overlay_disabled = 1;
    log_overlay_message(dev, message);
}

static void handle_overlay_init_failure(DeviceData* dev, const char* message) {
    if (!dev) {
        return;
    }

    if (dev->under_wine && dev->wine_policy == SIGAW_WINE_POLICY_AUTO) {
        disable_overlay(
            dev,
            "[sigaw] Wine/Proton detected and overlay initialization failed; disabling overlay for this process. Set SIGAW_WINE_POLICY=force to keep retrying."
        );
        return;
    }

    log_overlay_message(dev, message);
}

static int overlay_supports_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return 1;
        default:
            return 0;
    }
}

static int device_tracks_present_wait_semaphore(VkDevice device,
                                                VkSemaphore semaphore) {
    if (device == VK_NULL_HANDLE || semaphore == VK_NULL_HANDLE) {
        return 0;
    }

    for (int i = 0; i < g_swapchain_count; ++i) {
        SwapchainData* sc = &g_swapchains[i];
        if (sc->swapchain == VK_NULL_HANDLE || sc->device != device ||
            !sc->present_wait_sems) {
            continue;
        }

        for (uint32_t image_index = 0; image_index < sc->image_count; ++image_index) {
            if (sc->present_wait_sems[image_index] == semaphore) {
                return 1;
            }
        }
    }

    return 0;
}

static int swapchain_has_tracked_present_wait_semaphores(const SwapchainData* sc) {
    if (!sc || !sc->present_wait_sems) {
        return 0;
    }

    for (uint32_t image_index = 0; image_index < sc->image_count; ++image_index) {
        if (sc->present_wait_sems[image_index] != VK_NULL_HANDLE) {
            return 1;
        }
    }

    return 0;
}

static int device_has_tracked_present_wait_semaphores(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return 0;
    }

    for (int i = 0; i < g_swapchain_count; ++i) {
        SwapchainData* sc = &g_swapchains[i];
        if (sc->swapchain == VK_NULL_HANDLE || sc->device != device ||
            !sc->present_wait_sems) {
            continue;
        }

        for (uint32_t image_index = 0; image_index < sc->image_count; ++image_index) {
            if (sc->present_wait_sems[image_index] != VK_NULL_HANDLE) {
                return 1;
            }
        }
    }

    return 0;
}

static void wait_for_swapchain_present_tracking(DeviceData* dev, SwapchainData* sc) {
    if (!dev || !dev->device_wait_idle) {
        return;
    }

    if (!swapchain_has_tracked_present_wait_semaphores(sc)) {
        return;
    }

    (void)dev->device_wait_idle(dev->device);
}

static void wait_for_device_present_tracking(DeviceData* dev) {
    if (!dev || !dev->device_wait_idle) {
        return;
    }

    if (!device_has_tracked_present_wait_semaphores(dev->device)) {
        return;
    }

    /* These semaphores stay alive until the present operation itself is done,
     * which can outlive the overlay submit fence that created them. */
    (void)dev->device_wait_idle(dev->device);
}

static void release_tracked_present_semaphore(DeviceData* dev,
                                              VkDevice device,
                                              VkSemaphore semaphore,
                                              int discard) {
    if (device_tracks_present_wait_semaphore(device, semaphore)) {
        return;
    }

    if ((!dev || dev->device != device) && device != VK_NULL_HANDLE) {
        dev = find_device(device);
    }
    if (!dev || !dev->overlay_ctx) {
        return;
    }

    if (discard) {
        sigaw_overlay_discard_present_semaphore(dev->overlay_ctx, semaphore);
    } else {
        sigaw_overlay_recycle_present_semaphore(dev->overlay_ctx, semaphore);
    }
}

static void clear_swapchain_present_tracking(DeviceData* dev, SwapchainData* sc) {
    if (!sc || !sc->present_wait_sems) {
        return;
    }

    for (uint32_t image_index = 0; image_index < sc->image_count; ++image_index) {
        discard_swapchain_present_semaphore(dev, sc, image_index);
    }
}

static void clear_device_present_tracking(DeviceData* dev) {
    if (!dev) {
        return;
    }

    for (int i = 0; i < g_swapchain_count; ++i) {
        if (g_swapchains[i].swapchain != VK_NULL_HANDLE &&
            g_swapchains[i].device == dev->device) {
            clear_swapchain_present_tracking(dev, &g_swapchains[i]);
        }
    }
}

static void recycle_swapchain_present_semaphore(DeviceData* dev,
                                                SwapchainData* sc,
                                                uint32_t image_index) {
    if (!sc || !sc->present_wait_sems || image_index >= sc->image_count) {
        return;
    }

    VkSemaphore semaphore = sc->present_wait_sems[image_index];
    if (semaphore == VK_NULL_HANDLE) {
        return;
    }

    sc->present_wait_sems[image_index] = VK_NULL_HANDLE;
    release_tracked_present_semaphore(dev, sc->device, semaphore, 0);
}

static void discard_swapchain_present_semaphore(DeviceData* dev,
                                                SwapchainData* sc,
                                                uint32_t image_index) {
    if (!sc || !sc->present_wait_sems || image_index >= sc->image_count) {
        return;
    }

    VkSemaphore semaphore = sc->present_wait_sems[image_index];
    if (semaphore == VK_NULL_HANDLE) {
        return;
    }

    sc->present_wait_sems[image_index] = VK_NULL_HANDLE;
    release_tracked_present_semaphore(dev, sc->device, semaphore, 1);
}

static void track_swapchain_present_semaphore(DeviceData* dev,
                                              SwapchainData* sc,
                                              uint32_t image_index,
                                              VkSemaphore semaphore) {
    if (!sc || !sc->present_wait_sems || image_index >= sc->image_count ||
        semaphore == VK_NULL_HANDLE) {
        return;
    }

    if (sc->present_wait_sems[image_index] == semaphore) {
        return;
    }

    if (sc->present_wait_sems[image_index] != VK_NULL_HANDLE) {
        recycle_swapchain_present_semaphore(dev, sc, image_index);
    }

    sc->present_wait_sems[image_index] = semaphore;
}

static PFN_vkAcquireNextImage2KHR resolve_acquire_next_image2(
    PFN_vkGetDeviceProcAddr get_device_proc,
    VkDevice device)
{
    if (!get_device_proc) {
        return NULL;
    }

    PFN_vkAcquireNextImage2KHR acquire_next_image2 =
        (PFN_vkAcquireNextImage2KHR)get_device_proc(device, "vkAcquireNextImage2");
    if (!acquire_next_image2) {
        acquire_next_image2 =
            (PFN_vkAcquireNextImage2KHR)get_device_proc(device, "vkAcquireNextImage2KHR");
    }

    return acquire_next_image2;
}

static int present_result_keeps_wait_semaphore(VkResult result) {
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

static int present_call_keeps_wait_semaphore_for_swapchain(
    const VkPresentInfoKHR* pPresentInfo,
    VkResult aggregate_result,
    uint32_t swapchain_index)
{
    if (!pPresentInfo || swapchain_index >= pPresentInfo->swapchainCount) {
        return 0;
    }

    if (present_result_keeps_wait_semaphore(aggregate_result)) {
        return 1;
    }

    if (pPresentInfo->pResults) {
        return present_result_keeps_wait_semaphore(
            pPresentInfo->pResults[swapchain_index]
        );
    }

    /* Without per-swapchain results we can only distinguish the single
     * swapchain case from ambiguous multi-swapchain failure. */
    return pPresentInfo->swapchainCount > 1;
}

/* -------------------------------------------------------------------------- */
/*  Intercepted Vulkan functions                                               */
/* -------------------------------------------------------------------------- */

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                     const VkAllocationCallbacks* pAllocator,
                     VkInstance* pInstance)
{
    /* Walk the pNext chain to find the layer link info */
    VkLayerInstanceCreateInfo* chain_info =
        (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chain_info &&
           !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
             chain_info->function == VK_LAYER_LINK_INFO)) {
        chain_info = (VkLayerInstanceCreateInfo*)chain_info->pNext;
    }
    if (!chain_info) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr get_proc =
        chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    /* Advance the chain for the next layer */
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create_fn =
        (PFN_vkCreateInstance)get_proc(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult result = create_fn(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    /* Store instance data */
    if (g_instance_count < MAX_INSTANCES) {
        InstanceData* data = &g_instances[g_instance_count++];
        data->instance = *pInstance;
        data->get_proc = get_proc;
        data->destroy_instance =
            (PFN_vkDestroyInstance)get_proc(*pInstance, "vkDestroyInstance");
        data->enum_phys =
            (PFN_vkEnumeratePhysicalDevices)get_proc(*pInstance, "vkEnumeratePhysicalDevices");
        data->get_phys_props =
            (PFN_vkGetPhysicalDeviceProperties)get_proc(*pInstance, "vkGetPhysicalDeviceProperties");
        data->get_mem_props =
            (PFN_vkGetPhysicalDeviceMemoryProperties)get_proc(*pInstance, "vkGetPhysicalDeviceMemoryProperties");
        data->get_queue_props =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)get_proc(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
        data->phys_device_count = 0;
        cache_instance_physical_devices(data);
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
sigaw_DestroyInstance(VkInstance instance,
                      const VkAllocationCallbacks* pAllocator)
{
    InstanceData* data = find_instance(instance);
    if (data && data->destroy_instance) {
        data->destroy_instance(instance, pAllocator);
        data->instance = VK_NULL_HANDLE;
        data->phys_device_count = 0;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo* pCreateInfo,
                   const VkAllocationCallbacks* pAllocator,
                   VkDevice* pDevice)
{
    /* Walk the pNext chain */
    VkLayerDeviceCreateInfo* chain_info =
        (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chain_info &&
           !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
             chain_info->function == VK_LAYER_LINK_INFO)) {
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
    }
    if (!chain_info) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr get_instance_proc =
        chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr get_device_proc =
        chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    /* Find the instance that owns this physical device so instance-level
     * dispatch resolves against the correct loader chain. */
    InstanceData* inst_data = find_instance_for_physical_device(physicalDevice);
    VkInstance inst = inst_data ? inst_data->instance : VK_NULL_HANDLE;
    if (inst == VK_NULL_HANDLE) {
        for (int i = 0; i < g_instance_count; i++) {
            if (g_instances[i].instance != VK_NULL_HANDLE) {
                inst = g_instances[i].instance;
                inst_data = &g_instances[i];
                break;
            }
        }
    }

    PFN_vkCreateDevice create_fn =
        (PFN_vkCreateDevice)get_instance_proc(inst, "vkCreateDevice");
    VkResult result = create_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;

    /* Find a graphics queue family */
    uint32_t gfx_family = 0;
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
        /* We'll just use the first queue family requested */
        gfx_family = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
        break;
    }

    if (g_device_count < MAX_DEVICES) {
        DeviceData* data = &g_devices[g_device_count++];
        data->device = *pDevice;
        data->phys_device = physicalDevice;
        data->instance = inst;
        data->get_proc = get_device_proc;
        data->gfx_queue_family = gfx_family;
        data->queue_count = 0;
        data->overlay_queue_family = UINT32_MAX;
        sigaw_vk_dispatch_clear(&data->dispatch);
        sigaw_vk_dispatch_resolve(&data->dispatch, get_instance_proc, inst, get_device_proc, *pDevice);
        data->wine_policy = sigaw_wine_policy_from_env();
        data->under_wine = sigaw_is_wine_environment();
        data->overlay_disabled = 0;
        data->overlay_log_emitted = 0;
        data->overlay_ctx = NULL;

        data->destroy_device =
            (PFN_vkDestroyDevice)get_device_proc(*pDevice, "vkDestroyDevice");
        data->create_swapchain =
            (PFN_vkCreateSwapchainKHR)get_device_proc(*pDevice, "vkCreateSwapchainKHR");
        data->destroy_swapchain =
            (PFN_vkDestroySwapchainKHR)get_device_proc(*pDevice, "vkDestroySwapchainKHR");
        data->get_swapchain_images =
            (PFN_vkGetSwapchainImagesKHR)get_device_proc(*pDevice, "vkGetSwapchainImagesKHR");
        data->queue_present =
            (PFN_vkQueuePresentKHR)get_device_proc(*pDevice, "vkQueuePresentKHR");
        data->queue_submit =
            (PFN_vkQueueSubmit)get_device_proc(*pDevice, "vkQueueSubmit");
        data->device_wait_idle =
            (PFN_vkDeviceWaitIdle)get_device_proc(*pDevice, "vkDeviceWaitIdle");
        data->acquire_next_image =
            (PFN_vkAcquireNextImageKHR)get_device_proc(*pDevice, "vkAcquireNextImageKHR");
        data->acquire_next_image2 = resolve_acquire_next_image2(get_device_proc, *pDevice);

        /* Get the graphics queue */
        PFN_vkGetDeviceQueue get_queue =
            (PFN_vkGetDeviceQueue)get_device_proc(*pDevice, "vkGetDeviceQueue");
        PFN_vkGetDeviceQueue2 get_queue2 =
            (PFN_vkGetDeviceQueue2)get_device_proc(*pDevice, "vkGetDeviceQueue2");
        if (get_queue) {
            for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
                const VkDeviceQueueCreateInfo* qci = &pCreateInfo->pQueueCreateInfos[i];
                for (uint32_t j = 0; j < qci->queueCount &&
                                     data->queue_count < MAX_DEVICE_QUEUES; j++) {
                    VkQueue queue = VK_NULL_HANDLE;
                    if (qci->flags == 0) {
                        get_queue(*pDevice, qci->queueFamilyIndex, j, &queue);
                    } else if (get_queue2) {
                        VkDeviceQueueInfo2 qi = {
                            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                            .pNext = NULL,
                            .flags = qci->flags,
                            .queueFamilyIndex = qci->queueFamilyIndex,
                            .queueIndex = j,
                        };
                        get_queue2(*pDevice, &qi, &queue);
                    }

                    if (queue) {
                        data->queues[data->queue_count] = queue;
                        data->queue_families[data->queue_count] = qci->queueFamilyIndex;
                        data->queue_count++;
                    }
                }
            }
        }
        if (data->queue_count > 0) {
            data->gfx_queue = data->queues[0];
            data->gfx_queue_family = data->queue_families[0];
        } else if (get_queue) {
            get_queue(*pDevice, gfx_family, 0, &data->gfx_queue);
        }

    }

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
sigaw_DestroyDevice(VkDevice device,
                    const VkAllocationCallbacks* pAllocator)
{
    DeviceData* data = find_device(device);
    if (data) {
        wait_for_device_present_tracking(data);
        clear_device_present_tracking(data);
        sigaw_release_swapchains_for_device(g_swapchains, &g_swapchain_count, device);
        if (data->overlay_ctx) {
            sigaw_overlay_destroy(data->overlay_ctx);
            data->overlay_ctx = NULL;
        }
        data->overlay_queue_family = UINT32_MAX;
        data->overlay_disabled = 0;
        data->overlay_log_emitted = 0;
        if (data->destroy_device) {
            data->destroy_device(device, pAllocator);
        }
        data->device = VK_NULL_HANDLE;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_CreateSwapchainKHR(VkDevice device,
                         const VkSwapchainCreateInfoKHR* pCreateInfo,
                         const VkAllocationCallbacks* pAllocator,
                         VkSwapchainKHR* pSwapchain)
{
    DeviceData* dev = find_device(device);
    if (!dev || !dev->create_swapchain)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = dev->create_swapchain(device, pCreateInfo, pAllocator, pSwapchain);
    if (result != VK_SUCCESS) return result;

    /* Record swapchain info */
    SwapchainData* sc = sigaw_allocate_swapchain_slot(
        g_swapchains,
        &g_swapchain_count,
        (int)(sizeof(g_swapchains) / sizeof(g_swapchains[0]))
    );
    if (sc) {
        sc->swapchain = *pSwapchain;
        sc->device = device;
        sc->format = pCreateInfo->imageFormat;
        sc->width  = pCreateInfo->imageExtent.width;
        sc->height = pCreateInfo->imageExtent.height;

        /* Get swapchain images */
        dev->get_swapchain_images(device, *pSwapchain, &sc->image_count, NULL);
        sc->images = (VkImage*)calloc(sc->image_count, sizeof(VkImage));
        dev->get_swapchain_images(device, *pSwapchain, &sc->image_count, sc->images);
        sc->present_wait_sems = (VkSemaphore*)calloc(sc->image_count, sizeof(VkSemaphore));

        /* Keep renderer dimensions in sync once the overlay is active. */
        if (dev->overlay_ctx) {
            sigaw_overlay_resize(dev->overlay_ctx, sc->format, sc->width, sc->height);
        }
    } else if (!g_swapchain_full_warned) {
        fprintf(stderr, "[sigaw] Swapchain bookkeeping table full; overlay tracking disabled for new swapchains\n");
        g_swapchain_full_warned = 1;
    }

    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
sigaw_DestroySwapchainKHR(VkDevice device,
                          VkSwapchainKHR swapchain,
                          const VkAllocationCallbacks* pAllocator)
{
    DeviceData* dev = find_device(device);
    if (!dev || !dev->destroy_swapchain) {
        return;
    }

    SwapchainData* sc = find_swapchain(swapchain);
    if (sc) {
        wait_for_swapchain_present_tracking(dev, sc);
    }

    dev->destroy_swapchain(device, swapchain, pAllocator);

    if (sc) {
        clear_swapchain_present_tracking(dev, sc);
        sigaw_release_swapchain(g_swapchains, &g_swapchain_count, sc);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_AcquireNextImageKHR(VkDevice device,
                          VkSwapchainKHR swapchain,
                          uint64_t timeout,
                          VkSemaphore semaphore,
                          VkFence fence,
                          uint32_t* pImageIndex)
{
    DeviceData* dev = find_device(device);
    if (!dev || !dev->acquire_next_image) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result =
        dev->acquire_next_image(device, swapchain, timeout, semaphore, fence, pImageIndex);
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && pImageIndex != NULL) {
        SwapchainData* sc = find_swapchain(swapchain);
        if (sc) {
            recycle_swapchain_present_semaphore(dev, sc, *pImageIndex);
        }
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_acquire_next_image2_common(VkDevice device,
                                 const VkAcquireNextImageInfoKHR* pAcquireInfo,
                                 uint32_t* pImageIndex)
{
    DeviceData* dev = find_device(device);
    if (!dev || !dev->acquire_next_image2 || !pAcquireInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = dev->acquire_next_image2(device, pAcquireInfo, pImageIndex);
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && pImageIndex != NULL) {
        SwapchainData* sc = find_swapchain(pAcquireInfo->swapchain);
        if (sc) {
            recycle_swapchain_present_semaphore(dev, sc, *pImageIndex);
        }
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_AcquireNextImage2KHR(VkDevice device,
                           const VkAcquireNextImageInfoKHR* pAcquireInfo,
                           uint32_t* pImageIndex)
{
    return sigaw_acquire_next_image2_common(device, pAcquireInfo, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_AcquireNextImage2(VkDevice device,
                        const VkAcquireNextImageInfoKHR* pAcquireInfo,
                        uint32_t* pImageIndex)
{
    return sigaw_acquire_next_image2_common(device, pAcquireInfo, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL
sigaw_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    /*
     * Main hook: for each swapchain being presented, render the voice
     * overlay onto the swapchain image before it goes to the display.
     * The overlay renderer (overlay_renderer.cpp) handles:
     *   1. Reading voice state from shared memory
     *   2. CPU-rendering the panel with font glyphs and shapes
     *   3. Uploading to a staging buffer
     *   4. Recording a cmd buffer to copy onto the swapchain image
     *   5. Submitting with proper synchronization
     */

    uint32_t queue_family = UINT32_MAX;
    DeviceData* dev = find_device_for_queue(queue, &queue_family);
    VkPresentInfoKHR present_info = *pPresentInfo;
    int overlay_rendered = 0;
    VkSemaphore present_wait_semaphore = VK_NULL_HANDLE;
    int present_wait_submitted = 0;

    if (!dev && pPresentInfo->swapchainCount > 0) {
        SwapchainData* sc = find_swapchain(pPresentInfo->pSwapchains[0]);
        if (sc) {
            dev = find_device(sc->device);
        }
    }

    if (dev && !dev->overlay_ctx &&
        pPresentInfo->swapchainCount > 0) {
        SwapchainData* sc = find_swapchain(pPresentInfo->pSwapchains[0]);
        InstanceData* inst_data = sc ? find_instance(dev->instance) : NULL;
        if (dev->under_wine && dev->wine_policy == SIGAW_WINE_POLICY_DISABLE) {
            disable_overlay(
                dev,
                "[sigaw] Wine/Proton detected; overlay disabled by SIGAW_WINE_POLICY=disable"
            );
        } else if (sc && inst_data && queue_family != UINT32_MAX &&
                   !dev->overlay_disabled) {
            if (!sigaw_vk_dispatch_complete(&dev->dispatch)) {
                handle_overlay_init_failure(
                    dev,
                    "[sigaw] Overlay dispatch initialization incomplete; continuing without overlay"
                );
            } else {
            dev->overlay_ctx =
                sigaw_overlay_create(dev->device, dev->phys_device, dev->instance,
                                     queue_family, queue,
                                     &dev->dispatch,
                                     inst_data->get_mem_props,
                                     sc->format, sc->width, sc->height);
            if (dev->overlay_ctx) {
                dev->overlay_queue_family = queue_family;
            } else {
                handle_overlay_init_failure(
                    dev,
                    "[sigaw] Overlay initialization failed; continuing without overlay"
                );
            }
            }
        }
    } else if (dev && dev->overlay_ctx && queue_family != UINT32_MAX &&
               dev->overlay_queue_family != UINT32_MAX &&
               dev->overlay_queue_family != queue_family) {
        SwapchainData* sc = NULL;
        InstanceData* inst_data = NULL;

        if (pPresentInfo->swapchainCount > 0) {
            sc = find_swapchain(pPresentInfo->pSwapchains[0]);
        }
        if (sc) {
            inst_data = find_instance(dev->instance);
        }

        wait_for_device_present_tracking(dev);
        clear_device_present_tracking(dev);
        sigaw_overlay_destroy(dev->overlay_ctx);
        dev->overlay_ctx = NULL;
        dev->overlay_queue_family = UINT32_MAX;

        if (sc && inst_data) {
            dev->overlay_ctx =
                sigaw_overlay_create(dev->device, dev->phys_device, dev->instance,
                                     queue_family, queue,
                                     &dev->dispatch,
                                     inst_data->get_mem_props,
                                     sc->format, sc->width, sc->height);
            if (dev->overlay_ctx) {
                dev->overlay_queue_family = queue_family;
            } else {
                handle_overlay_init_failure(
                    dev,
                    "[sigaw] Overlay reinitialization failed; continuing without overlay"
                );
            }
        }
    }

    if (dev && dev->overlay_ctx && queue_family != UINT32_MAX &&
        dev->overlay_queue_family == queue_family) {
        /* The final overlay submit must signal a semaphore for the forwarded
         * present call; waiting on a host fence alone is not enough for all
         * Vulkan loader / Wine paths. */
        uint32_t last_render_index = UINT32_MAX;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            SwapchainData* sc = find_swapchain(pPresentInfo->pSwapchains[i]);
            if (!sc || !sc->images || !overlay_supports_format(sc->format)) {
                continue;
            }

            const uint32_t img_idx = pPresentInfo->pImageIndices[i];
            if (img_idx < sc->image_count) {
                last_render_index = i;
            }
        }

        int can_render_overlay = 1;
        if (last_render_index != UINT32_MAX) {
            present_wait_semaphore =
                sigaw_overlay_acquire_present_semaphore(dev->overlay_ctx);
            if (present_wait_semaphore == VK_NULL_HANDLE) {
                can_render_overlay = 0;
            }
        }

        if (can_render_overlay) {
            for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
                SwapchainData* sc = find_swapchain(pPresentInfo->pSwapchains[i]);
                if (sc && sc->images && overlay_supports_format(sc->format)) {
                    uint32_t img_idx = pPresentInfo->pImageIndices[i];
                    if (img_idx < sc->image_count) {
                        const uint32_t wait_count =
                            overlay_rendered ? 0u : pPresentInfo->waitSemaphoreCount;
                        const VkSemaphore* wait_sems =
                            overlay_rendered ? NULL : pPresentInfo->pWaitSemaphores;
                        const VkSemaphore signal_sem =
                            (i == last_render_index) ? present_wait_semaphore : VK_NULL_HANDLE;
                        if (sigaw_overlay_render(
                            dev->overlay_ctx, queue,
                            sc->images[img_idx], sc->format,
                            sc->width, sc->height,
                            wait_count, wait_sems,
                            signal_sem, VK_NULL_HANDLE
                        )) {
                            overlay_rendered = 1;
                            if (signal_sem != VK_NULL_HANDLE) {
                                present_wait_submitted = 1;
                            }
                        } else if (signal_sem != VK_NULL_HANDLE) {
                            sigaw_overlay_recycle_present_semaphore(
                                dev->overlay_ctx,
                                signal_sem
                            );
                            present_wait_semaphore = VK_NULL_HANDLE;
                        }
                    }
                }
            }
        }
    }

    if (!present_wait_submitted && present_wait_semaphore != VK_NULL_HANDLE &&
        dev && dev->overlay_ctx) {
        sigaw_overlay_recycle_present_semaphore(dev->overlay_ctx, present_wait_semaphore);
        present_wait_semaphore = VK_NULL_HANDLE;
    }

    /* Chain to the real present */
    if (dev && dev->queue_present) {
        if (overlay_rendered) {
            if (present_wait_semaphore != VK_NULL_HANDLE) {
                present_info.waitSemaphoreCount = 1;
                present_info.pWaitSemaphores = &present_wait_semaphore;
            }
            VkResult result = dev->queue_present(queue, &present_info);
            if (present_wait_submitted && present_wait_semaphore != VK_NULL_HANDLE) {
                int tracked_present = 0;
                for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
                    if (!present_call_keeps_wait_semaphore_for_swapchain(
                            &present_info,
                            result,
                            i
                        )) {
                        continue;
                    }

                    SwapchainData* sc = find_swapchain(pPresentInfo->pSwapchains[i]);
                    if (!sc) {
                        continue;
                    }

                    const uint32_t image_index = pPresentInfo->pImageIndices[i];
                    if (image_index < sc->image_count) {
                        track_swapchain_present_semaphore(
                            dev,
                            sc,
                            image_index,
                            present_wait_semaphore
                        );
                        tracked_present = 1;
                    }
                }

                if (!tracked_present) {
                    /* The present consumed the semaphore as a wait target;
                     * vkDestroySemaphore requires all referencing batches to
                     * have completed, so drain the queue first. */
                    if (dev->device_wait_idle) {
                        dev->device_wait_idle(dev->device);
                    }
                    sigaw_overlay_discard_present_semaphore(
                        dev->overlay_ctx,
                        present_wait_semaphore
                    );
                }
            }
            return result;
        }
        return dev->queue_present(queue, pPresentInfo);
    }

    return VK_ERROR_INITIALIZATION_FAILED;
}

/* -------------------------------------------------------------------------- */
/*  Layer dispatch (GetInstanceProcAddr / GetDeviceProcAddr)                    */
/* -------------------------------------------------------------------------- */

#define INTERCEPT(vk_name, sigaw_name) \
    if (strcmp(pName, #vk_name) == 0) return (PFN_vkVoidFunction)sigaw_##sigaw_name

SIGAW_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
sigaw_GetInstanceProcAddr(VkInstance instance, const char* pName)
{
    /* Functions we intercept */
    INTERCEPT(vkCreateInstance, CreateInstance);
    INTERCEPT(vkDestroyInstance, DestroyInstance);
    INTERCEPT(vkCreateDevice, CreateDevice);
    INTERCEPT(vkGetInstanceProcAddr, GetInstanceProcAddr);

    /* Chain to next layer */
    InstanceData* data = find_instance(instance);
    if (data && data->get_proc) {
        return data->get_proc(instance, pName);
    }

    return NULL;
}

SIGAW_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
sigaw_GetDeviceProcAddr(VkDevice device, const char* pName)
{
    DeviceData* data = find_device(device);
    if (strcmp(pName, "vkDestroyDevice") == 0) {
        return (PFN_vkVoidFunction)sigaw_DestroyDevice;
    }
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        return (PFN_vkVoidFunction)sigaw_CreateSwapchainKHR;
    }
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) {
        return (PFN_vkVoidFunction)sigaw_DestroySwapchainKHR;
    }
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
        return (PFN_vkVoidFunction)sigaw_AcquireNextImageKHR;
    }
    if (strcmp(pName, "vkAcquireNextImage2") == 0) {
        if (data && data->get_proc && data->get_proc(device, pName)) {
            return (PFN_vkVoidFunction)sigaw_AcquireNextImage2;
        }
        return NULL;
    }
    if (strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
        if (data && data->get_proc && data->get_proc(device, pName)) {
            return (PFN_vkVoidFunction)sigaw_AcquireNextImage2KHR;
        }
        return NULL;
    }
    if (strcmp(pName, "vkQueuePresentKHR") == 0) {
        return (PFN_vkVoidFunction)sigaw_QueuePresentKHR;
    }
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return (PFN_vkVoidFunction)sigaw_GetDeviceProcAddr;
    }

    /* Chain to next layer */
    if (data && data->get_proc) {
        return data->get_proc(device, pName);
    }

    return NULL;
}

#undef INTERCEPT

/* -------------------------------------------------------------------------- */
/*  Layer negotiation (required by the Vulkan loader)                          */
/* -------------------------------------------------------------------------- */

SIGAW_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
sigaw_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
{
    assert(pVersionStruct != NULL);
    assert(pVersionStruct->sType == LAYER_NEGOTIATE_INTERFACE_STRUCT);

    /* We support interface version 2 */
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    }

    pVersionStruct->pfnGetInstanceProcAddr = sigaw_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr   = sigaw_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;

    return VK_SUCCESS;
}

SIGAW_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    return sigaw_GetInstanceProcAddr(instance, pName);
}

SIGAW_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    return sigaw_GetDeviceProcAddr(device, pName);
}

SIGAW_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
{
    return sigaw_NegotiateLoaderLayerInterfaceVersion(pVersionStruct);
}
