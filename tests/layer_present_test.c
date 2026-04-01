#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

struct SigawOverlayContext;

#include "layer/layer_entry.c"

typedef struct {
    uint32_t wait_count;
    VkSemaphore wait_sems[4];
} PresentCapture;

typedef struct {
    uint32_t wait_count;
    VkSemaphore signal_sem;
} RenderCapture;

static PresentCapture g_present_capture;
static RenderCapture g_render_capture[4];
static uint32_t g_render_capture_count = 0;
static VkSemaphore g_next_present_semaphore = VK_NULL_HANDLE;
static VkSemaphore g_recycled_present_sems[8];
static uint32_t g_recycled_present_sem_count = 0;
static VkSemaphore g_discarded_present_sems[8];
static uint32_t g_discarded_present_sem_count = 0;
static VkResult g_queue_present_result = VK_SUCCESS;
static VkResult g_queue_present_results[4];
static uint32_t g_queue_present_results_count = 0;
static VkResult g_acquire_next_image_result = VK_SUCCESS;
static uint32_t g_acquire_next_image_index = 0;
static uint32_t g_device_wait_idle_call_count = 0;
static VkDevice g_device_wait_idle_last_device = VK_NULL_HANDLE;
static int g_require_wait_before_discard = 0;
static int g_discard_before_wait_detected = 0;
static uint32_t g_device_proc_mask = 0;
static uint32_t g_overlay_render_fail_call = 0;
static uint32_t g_overlay_render_call_count = 0;

enum {
    FAKE_DEVICE_PROC_ACQUIRE_NEXT_IMAGE2 = 1u << 0,
    FAKE_DEVICE_PROC_ACQUIRE_NEXT_IMAGE2_KHR = 1u << 1,
};

static VkDevice fake_device(uintptr_t value) {
    return (VkDevice)value;
}

static VkQueue fake_queue(uintptr_t value) {
    return (VkQueue)value;
}

static VkSwapchainKHR fake_swapchain(uintptr_t value) {
    return (VkSwapchainKHR)value;
}

static VkImage fake_image(uintptr_t value) {
    return (VkImage)value;
}

SigawOverlayContext* sigaw_overlay_create(VkDevice device, VkPhysicalDevice phys_device,
                                          VkInstance instance, uint32_t queue_family,
                                          VkQueue queue,
                                          const SigawVulkanDispatch* dispatch,
                                          PFN_vkGetPhysicalDeviceMemoryProperties get_phys_props,
                                          VkFormat format,
                                          uint32_t width, uint32_t height)
{
    (void)device;
    (void)phys_device;
    (void)instance;
    (void)queue_family;
    (void)queue;
    (void)dispatch;
    (void)get_phys_props;
    (void)format;
    (void)width;
    (void)height;
    return (SigawOverlayContext*)0x1;
}

void sigaw_overlay_destroy(SigawOverlayContext* ctx) {
    (void)ctx;
}

void sigaw_overlay_resize(SigawOverlayContext* ctx, VkFormat format,
                          uint32_t width, uint32_t height)
{
    (void)ctx;
    (void)format;
    (void)width;
    (void)height;
}

VkSemaphore sigaw_overlay_acquire_present_semaphore(SigawOverlayContext* ctx) {
    (void)ctx;
    return g_next_present_semaphore;
}

void sigaw_overlay_recycle_present_semaphore(SigawOverlayContext* ctx,
                                             VkSemaphore semaphore) {
    (void)ctx;
    if (g_recycled_present_sem_count < 8) {
        g_recycled_present_sems[g_recycled_present_sem_count++] = semaphore;
    }
}

void sigaw_overlay_discard_present_semaphore(SigawOverlayContext* ctx,
                                             VkSemaphore semaphore) {
    (void)ctx;
    if (g_require_wait_before_discard && g_device_wait_idle_call_count == 0) {
        g_discard_before_wait_detected = 1;
    }
    if (g_discarded_present_sem_count < 8) {
        g_discarded_present_sems[g_discarded_present_sem_count++] = semaphore;
    }
}

int sigaw_overlay_render(SigawOverlayContext* ctx, VkQueue queue,
                         VkImage target_image, VkFormat format,
                         uint32_t width, uint32_t height,
                         uint32_t wait_sem_count,
                         const VkSemaphore* wait_sems,
                         VkSemaphore signal_sem,
                         VkFence fence)
{
    (void)ctx;
    (void)queue;
    (void)target_image;
    (void)format;
    (void)width;
    (void)height;
    (void)wait_sems;
    (void)fence;

    if (g_render_capture_count < 4) {
        g_render_capture[g_render_capture_count].wait_count = wait_sem_count;
        g_render_capture[g_render_capture_count].signal_sem = signal_sem;
        g_render_capture_count++;
    }

    g_overlay_render_call_count++;
    if (g_overlay_render_fail_call != 0 &&
        g_overlay_render_call_count == g_overlay_render_fail_call) {
        return 0;
    }

    return 1;
}

static VkResult VKAPI_CALL fake_queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
    (void)queue;
    g_present_capture.wait_count = info->waitSemaphoreCount;
    memset(g_present_capture.wait_sems, 0, sizeof(g_present_capture.wait_sems));
    for (uint32_t i = 0; i < info->waitSemaphoreCount && i < 4; ++i) {
        g_present_capture.wait_sems[i] = info->pWaitSemaphores[i];
    }
    if (info->pResults != NULL) {
        for (uint32_t i = 0; i < info->swapchainCount; ++i) {
            info->pResults[i] =
                (i < g_queue_present_results_count)
                    ? g_queue_present_results[i]
                    : g_queue_present_result;
        }
    }
    return g_queue_present_result;
}

static VkResult VKAPI_CALL fake_acquire_next_image(VkDevice device,
                                                   VkSwapchainKHR swapchain,
                                                   uint64_t timeout,
                                                   VkSemaphore semaphore,
                                                   VkFence fence,
                                                   uint32_t* pImageIndex) {
    (void)device;
    (void)swapchain;
    (void)timeout;
    (void)semaphore;
    (void)fence;
    if (pImageIndex != NULL) {
        *pImageIndex = g_acquire_next_image_index;
    }
    return g_acquire_next_image_result;
}

static VkResult VKAPI_CALL fake_acquire_next_image2(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex)
{
    (void)device;
    (void)pAcquireInfo;
    if (pImageIndex != NULL) {
        *pImageIndex = g_acquire_next_image_index;
    }
    return g_acquire_next_image_result;
}

static PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc(VkDevice device, const char* pName) {
    (void)device;
    if (strcmp(pName, "vkAcquireNextImage2") == 0 &&
        (g_device_proc_mask & FAKE_DEVICE_PROC_ACQUIRE_NEXT_IMAGE2) != 0) {
        return (PFN_vkVoidFunction)fake_acquire_next_image2;
    }
    if (strcmp(pName, "vkAcquireNextImage2KHR") == 0 &&
        (g_device_proc_mask & FAKE_DEVICE_PROC_ACQUIRE_NEXT_IMAGE2_KHR) != 0) {
        return (PFN_vkVoidFunction)fake_acquire_next_image2;
    }
    return NULL;
}

static VkResult VKAPI_CALL fake_device_wait_idle(VkDevice device) {
    g_device_wait_idle_call_count++;
    g_device_wait_idle_last_device = device;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_destroy_swapchain(VkDevice device,
                                              VkSwapchainKHR swapchain,
                                              const VkAllocationCallbacks* pAllocator) {
    (void)device;
    (void)swapchain;
    (void)pAllocator;
}

static void reset_state(void) {
    for (int i = 0; i < g_swapchain_count; ++i) {
        free(g_swapchains[i].images);
        g_swapchains[i].images = NULL;
        free(g_swapchains[i].present_wait_sems);
        g_swapchains[i].present_wait_sems = NULL;
    }
    memset(g_instances, 0, sizeof(g_instances));
    memset(g_devices, 0, sizeof(g_devices));
    memset(g_swapchains, 0, sizeof(g_swapchains));
    memset(&g_present_capture, 0, sizeof(g_present_capture));
    memset(g_render_capture, 0, sizeof(g_render_capture));
    memset(g_recycled_present_sems, 0, sizeof(g_recycled_present_sems));
    memset(g_discarded_present_sems, 0, sizeof(g_discarded_present_sems));
    g_instance_count = 0;
    g_device_count = 0;
    g_swapchain_count = 0;
    g_swapchain_full_warned = 0;
    g_render_capture_count = 0;
    g_next_present_semaphore = VK_NULL_HANDLE;
    g_recycled_present_sem_count = 0;
    g_discarded_present_sem_count = 0;
    g_queue_present_result = VK_SUCCESS;
    memset(g_queue_present_results, 0, sizeof(g_queue_present_results));
    g_queue_present_results_count = 0;
    g_acquire_next_image_result = VK_SUCCESS;
    g_acquire_next_image_index = 0;
    g_device_wait_idle_call_count = 0;
    g_device_wait_idle_last_device = VK_NULL_HANDLE;
    g_require_wait_before_discard = 0;
    g_discard_before_wait_detected = 0;
    g_device_proc_mask =
        FAKE_DEVICE_PROC_ACQUIRE_NEXT_IMAGE2 | FAKE_DEVICE_PROC_ACQUIRE_NEXT_IMAGE2_KHR;
    g_overlay_render_fail_call = 0;
    g_overlay_render_call_count = 0;
}

static DeviceData* add_device(VkDevice device, VkQueue queue, uint32_t queue_family) {
    DeviceData* dev = &g_devices[g_device_count++];
    memset(dev, 0, sizeof(*dev));
    dev->device = device;
    dev->get_proc = fake_get_device_proc;
    dev->queue_present = fake_queue_present;
    dev->device_wait_idle = fake_device_wait_idle;
    dev->destroy_swapchain = fake_destroy_swapchain;
    dev->acquire_next_image = fake_acquire_next_image;
    dev->acquire_next_image2 = fake_acquire_next_image2;
    dev->queue_count = 1;
    dev->queues[0] = queue;
    dev->queue_families[0] = queue_family;
    dev->overlay_queue_family = queue_family;
    dev->overlay_ctx = (SigawOverlayContext*)0x1;
    return dev;
}

static SwapchainData* add_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                                    VkFormat format, VkImage* images,
                                    uint32_t image_count)
{
    SwapchainData* sc = &g_swapchains[g_swapchain_count++];
    memset(sc, 0, sizeof(*sc));
    sc->device = device;
    sc->swapchain = swapchain;
    sc->format = format;
    sc->width = 1920;
    sc->height = 1080;
    sc->images = (VkImage*)calloc(image_count, sizeof(VkImage));
    memcpy(sc->images, images, sizeof(VkImage) * image_count);
    sc->image_count = image_count;
    sc->present_wait_sems = (VkSemaphore*)calloc(image_count, sizeof(VkSemaphore));
    return sc;
}

static int test_single_overlay_submit_forwards_layer_semaphore(void) {
    reset_state();

    VkImage images[] = { fake_image(0x100) };
    VkSemaphore app_wait = (VkSemaphore)0x200;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = { fake_swapchain(0x300) };
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x400);
    const VkDevice device = fake_device(0x500);

    add_device(device, queue, 7);
    add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x600;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "single overlay present should succeed\n");
        return 0;
    }
    if (g_render_capture_count != 1) {
        fprintf(stderr, "expected exactly one overlay render call\n");
        return 0;
    }
    if (g_render_capture[0].wait_count != 1 ||
        g_render_capture[0].signal_sem != g_next_present_semaphore) {
        fprintf(stderr, "single overlay render should consume app waits and signal the layer semaphore\n");
        return 0;
    }
    if (g_present_capture.wait_count != 1 ||
        g_present_capture.wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "forwarded present should wait on the layer semaphore\n");
        return 0;
    }
    return 1;
}

static int test_wine_auto_single_overlay_uses_wait_free_present(void) {
    reset_state();

    VkImage images[] = { fake_image(0x115) };
    VkSemaphore app_wait = (VkSemaphore)0x204;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = { fake_swapchain(0x315) };
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x412);
    const VkDevice device = fake_device(0x511);

    DeviceData* dev = add_device(device, queue, 27);
    dev->under_wine = 1;
    dev->wine_policy = SIGAW_WINE_POLICY_AUTO;
    SwapchainData* sc =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x611;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "Wine auto single present should succeed\n");
        return 0;
    }
    if (g_render_capture_count != 1) {
        fprintf(stderr, "Wine auto single present should submit exactly one overlay render\n");
        return 0;
    }
    if (g_render_capture[0].wait_count != 1 ||
        g_render_capture[0].signal_sem != VK_NULL_HANDLE) {
        fprintf(stderr, "Wine auto single render should consume app waits without signaling a layer semaphore\n");
        return 0;
    }
    if (g_present_capture.wait_count != 0) {
        fprintf(stderr, "Wine auto single present should forward with no wait semaphores\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 0 ||
        g_discarded_present_sem_count != 0) {
        fprintf(stderr, "Wine auto single present should not recycle or discard layer semaphores\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "Wine auto single present should not track a layer semaphore on the swapchain image\n");
        return 0;
    }
    return 1;
}

static int test_acquire_next_image_recycles_present_semaphore(void) {
    reset_state();

    VkImage images[] = { fake_image(0x104) };
    const VkSwapchainKHR swapchains[] = { fake_swapchain(0x304) };
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x403);
    const VkDevice device = fake_device(0x503);

    add_device(device, queue, 12);
    SwapchainData* sc =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x603;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "present before acquire should succeed\n");
        return 0;
    }
    if (sc->present_wait_sems == NULL ||
        sc->present_wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "presented image should retain the layer semaphore until reacquired\n");
        return 0;
    }

    uint32_t image_index = UINT32_MAX;
    if (sigaw_AcquireNextImageKHR(
            device,
            swapchains[0],
            0,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            &image_index
        ) != VK_SUCCESS) {
        fprintf(stderr, "acquire next image should succeed\n");
        return 0;
    }
    if (image_index != 0) {
        fprintf(stderr, "acquire next image should return the requested image index\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 1 ||
        g_recycled_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "acquire next image should recycle the pending layer semaphore\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "reacquired image should release its pending layer semaphore ownership\n");
        return 0;
    }
    return 1;
}

static int test_acquire_next_image2_recycles_present_semaphore(void) {
    reset_state();

    VkImage images[] = { fake_image(0x105) };
    const VkSwapchainKHR swapchain = fake_swapchain(0x305);
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x404);
    const VkDevice device = fake_device(0x504);

    add_device(device, queue, 13);
    SwapchainData* sc = add_swapchain(device, swapchain, VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x604;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "present before acquire2 should succeed\n");
        return 0;
    }

    const VkAcquireNextImageInfoKHR acquire = {
        .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
        .swapchain = swapchain,
        .timeout = 0,
        .semaphore = VK_NULL_HANDLE,
        .fence = VK_NULL_HANDLE,
        .deviceMask = 1,
    };
    uint32_t image_index = UINT32_MAX;
    if (sigaw_AcquireNextImage2KHR(device, &acquire, &image_index) != VK_SUCCESS) {
        fprintf(stderr, "acquire next image2 should succeed\n");
        return 0;
    }
    if (image_index != 0) {
        fprintf(stderr, "acquire next image2 should return the requested image index\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 1 ||
        g_recycled_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "acquire next image2 should recycle the pending layer semaphore\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "reacquired image2 should clear its pending layer semaphore ownership\n");
        return 0;
    }
    return 1;
}

static int test_acquire_next_image2_core_recycles_present_semaphore(void) {
    reset_state();

    VkImage images[] = { fake_image(0x10a) };
    const VkSwapchainKHR swapchain = fake_swapchain(0x30a);
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x409);
    const VkDevice device = fake_device(0x509);

    add_device(device, queue, 18);
    SwapchainData* sc = add_swapchain(device, swapchain, VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x60a;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "present before core acquire2 should succeed\n");
        return 0;
    }

    PFN_vkAcquireNextImage2KHR acquire_next_image2 =
        (PFN_vkAcquireNextImage2KHR)sigaw_GetDeviceProcAddr(device, "vkAcquireNextImage2");
    if (acquire_next_image2 == NULL) {
        fprintf(stderr, "core acquire next image2 should be intercepted\n");
        return 0;
    }

    const VkAcquireNextImageInfoKHR acquire = {
        .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
        .swapchain = swapchain,
        .timeout = 0,
        .semaphore = VK_NULL_HANDLE,
        .fence = VK_NULL_HANDLE,
        .deviceMask = 1,
    };
    uint32_t image_index = UINT32_MAX;
    if (acquire_next_image2(device, &acquire, &image_index) != VK_SUCCESS) {
        fprintf(stderr, "core acquire next image2 should succeed\n");
        return 0;
    }
    if (image_index != 0) {
        fprintf(stderr, "core acquire next image2 should return the requested image index\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 1 ||
        g_recycled_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "core acquire next image2 should recycle the pending layer semaphore\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "core reacquire should clear pending layer semaphore ownership\n");
        return 0;
    }
    return 1;
}

static int test_wine_auto_multi_overlay_uses_wait_free_present(void) {
    reset_state();

    VkImage images_a[] = { fake_image(0x116) };
    VkImage images_b[] = { fake_image(0x117) };
    VkSemaphore app_wait = (VkSemaphore)0x205;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = {
        fake_swapchain(0x316),
        fake_swapchain(0x317),
    };
    const uint32_t image_indices[] = { 0, 0 };
    const VkQueue queue = fake_queue(0x413);
    const VkDevice device = fake_device(0x512);

    DeviceData* dev = add_device(device, queue, 28);
    dev->under_wine = 1;
    dev->wine_policy = SIGAW_WINE_POLICY_AUTO;
    SwapchainData* sc_a =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images_a, 1);
    SwapchainData* sc_b =
        add_swapchain(device, swapchains[1], VK_FORMAT_B8G8R8A8_UNORM, images_b, 1);
    g_next_present_semaphore = (VkSemaphore)0x612;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 2,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "Wine auto multi-present should succeed\n");
        return 0;
    }
    if (g_render_capture_count != 2) {
        fprintf(stderr, "Wine auto multi-present should submit two overlay renders\n");
        return 0;
    }
    if (g_render_capture[0].wait_count != 1 ||
        g_render_capture[0].signal_sem != VK_NULL_HANDLE) {
        fprintf(stderr, "Wine auto multi-present should consume app waits on the first render without signaling a layer semaphore\n");
        return 0;
    }
    if (g_render_capture[1].wait_count != 0 ||
        g_render_capture[1].signal_sem != VK_NULL_HANDLE) {
        fprintf(stderr, "Wine auto multi-present should submit later renders without waits or layer semaphores\n");
        return 0;
    }
    if (g_present_capture.wait_count != 0) {
        fprintf(stderr, "Wine auto multi-present should forward with no wait semaphores\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 0 ||
        g_discarded_present_sem_count != 0) {
        fprintf(stderr, "Wine auto multi-present should not recycle or discard layer semaphores\n");
        return 0;
    }
    if (sc_a->present_wait_sems[0] != VK_NULL_HANDLE ||
        sc_b->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "Wine auto multi-present should not track layer semaphores on swapchain images\n");
        return 0;
    }
    return 1;
}

static int test_multi_swapchain_overlay_only_signals_last_submit(void) {
    reset_state();

    VkImage images_a[] = { fake_image(0x101) };
    VkImage images_b[] = { fake_image(0x102) };
    VkSemaphore app_wait = (VkSemaphore)0x201;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = {
        fake_swapchain(0x301),
        fake_swapchain(0x302),
    };
    const uint32_t image_indices[] = { 0, 0 };
    const VkQueue queue = fake_queue(0x401);
    const VkDevice device = fake_device(0x501);

    add_device(device, queue, 9);
    add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images_a, 1);
    add_swapchain(device, swapchains[1], VK_FORMAT_B8G8R8A8_UNORM, images_b, 1);
    g_next_present_semaphore = (VkSemaphore)0x601;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 2,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "multi-swapchain overlay present should succeed\n");
        return 0;
    }
    if (g_render_capture_count != 2) {
        fprintf(stderr, "expected two overlay render calls for two swapchains\n");
        return 0;
    }
    if (g_render_capture[0].wait_count != 1 ||
        g_render_capture[0].signal_sem != VK_NULL_HANDLE) {
        fprintf(stderr, "first overlay submit should preserve app waits and not signal present\n");
        return 0;
    }
    if (g_render_capture[1].wait_count != 0 ||
        g_render_capture[1].signal_sem != g_next_present_semaphore) {
        fprintf(stderr, "last overlay submit should signal the layer semaphore\n");
        return 0;
    }
    if (g_present_capture.wait_count != 1 ||
        g_present_capture.wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "forwarded present should wait on the final overlay semaphore\n");
        return 0;
    }
    return 1;
}

static int test_wine_force_preserves_layer_present_semaphore_path(void) {
    reset_state();

    VkImage images[] = { fake_image(0x118) };
    VkSemaphore app_wait = (VkSemaphore)0x206;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = { fake_swapchain(0x318) };
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x414);
    const VkDevice device = fake_device(0x513);

    DeviceData* dev = add_device(device, queue, 29);
    dev->under_wine = 1;
    dev->wine_policy = SIGAW_WINE_POLICY_FORCE;
    SwapchainData* sc =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x613;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "Wine force single present should succeed\n");
        return 0;
    }
    if (g_render_capture_count != 1) {
        fprintf(stderr, "Wine force single present should submit exactly one overlay render\n");
        return 0;
    }
    if (g_render_capture[0].wait_count != 1 ||
        g_render_capture[0].signal_sem != g_next_present_semaphore) {
        fprintf(stderr, "Wine force should keep the layer semaphore signaling path\n");
        return 0;
    }
    if (g_present_capture.wait_count != 1 ||
        g_present_capture.wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "Wine force should forward present waiting on the layer semaphore\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "Wine force should continue tracking the layer semaphore on the presented image\n");
        return 0;
    }
    return 1;
}

static int test_multi_present_tracks_every_swapchain_image(void) {
    reset_state();

    VkImage images_a[] = { fake_image(0x10b) };
    VkImage images_b[] = { fake_image(0x10c) };
    const VkSwapchainKHR swapchains[] = {
        fake_swapchain(0x30b),
        fake_swapchain(0x30c),
    };
    const uint32_t image_indices[] = { 0, 0 };
    const VkQueue queue = fake_queue(0x40a);
    const VkDevice device = fake_device(0x50a);

    add_device(device, queue, 19);
    SwapchainData* sc_a =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images_a, 1);
    SwapchainData* sc_b =
        add_swapchain(device, swapchains[1], VK_FORMAT_R16G16B16A16_SFLOAT, images_b, 1);
    g_next_present_semaphore = (VkSemaphore)0x60b;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 2,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "multi-present should succeed\n");
        return 0;
    }
    if (sc_a->present_wait_sems[0] != g_next_present_semaphore ||
        sc_b->present_wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "forwarded present semaphore should be tracked on every swapchain image\n");
        return 0;
    }

    uint32_t image_index = UINT32_MAX;
    if (sigaw_AcquireNextImageKHR(
            device,
            swapchains[0],
            0,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            &image_index
        ) != VK_SUCCESS) {
        fprintf(stderr, "first swapchain reacquire should succeed\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 0) {
        fprintf(stderr, "layer semaphore should stay pending while another swapchain still tracks it\n");
        return 0;
    }
    if (sc_a->present_wait_sems[0] != VK_NULL_HANDLE ||
        sc_b->present_wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "only the reacquired swapchain image should release its tracking slot\n");
        return 0;
    }

    if (sigaw_AcquireNextImageKHR(
            device,
            swapchains[1],
            0,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            &image_index
        ) != VK_SUCCESS) {
        fprintf(stderr, "second swapchain reacquire should succeed\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 1 ||
        g_recycled_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "layer semaphore should recycle once the last tracked swapchain image is reacquired\n");
        return 0;
    }
    if (sc_b->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "final reacquire should clear the last tracked swapchain image slot\n");
        return 0;
    }
    return 1;
}

static int test_wine_disable_bypasses_overlay_and_preserves_present_waits(void) {
    reset_state();

    VkImage images[] = { fake_image(0x119) };
    VkSemaphore app_wait = (VkSemaphore)0x207;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = { fake_swapchain(0x319) };
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x415);
    const VkDevice device = fake_device(0x514);

    DeviceData* dev = add_device(device, queue, 30);
    dev->under_wine = 1;
    dev->wine_policy = SIGAW_WINE_POLICY_DISABLE;
    dev->overlay_ctx = NULL;
    SwapchainData* sc =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images, 1);

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "Wine disable present should succeed\n");
        return 0;
    }
    if (g_render_capture_count != 0) {
        fprintf(stderr, "Wine disable should bypass overlay rendering\n");
        return 0;
    }
    if (!dev->overlay_disabled) {
        fprintf(stderr, "Wine disable should mark the overlay disabled for the process\n");
        return 0;
    }
    if (g_present_capture.wait_count != 1 ||
        g_present_capture.wait_sems[0] != app_wait) {
        fprintf(stderr, "Wine disable should preserve the application's original present wait chain\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "Wine disable should not track layer semaphores on the swapchain image\n");
        return 0;
    }
    return 1;
}

static int test_failed_present_signaller_preserves_original_waits(void) {
    reset_state();

    VkImage images_a[] = { fake_image(0x113) };
    VkImage images_b[] = { fake_image(0x114) };
    VkSemaphore app_wait = (VkSemaphore)0x203;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = {
        fake_swapchain(0x313),
        fake_swapchain(0x314),
    };
    const uint32_t image_indices[] = { 0, 0 };
    const VkQueue queue = fake_queue(0x411);
    const VkDevice device = fake_device(0x510);

    add_device(device, queue, 26);
    SwapchainData* sc_a =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images_a, 1);
    SwapchainData* sc_b =
        add_swapchain(device, swapchains[1], VK_FORMAT_B8G8R8A8_UNORM, images_b, 1);
    g_next_present_semaphore = (VkSemaphore)0x610;
    g_overlay_render_fail_call = 2;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 2,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "present should still forward when the late signaling submit fails\n");
        return 0;
    }
    if (g_present_capture.wait_count != 1 ||
        g_present_capture.wait_sems[0] != app_wait) {
        fprintf(stderr, "late signaling submit failure should preserve the original present waits\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 1 ||
        g_recycled_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "failed signaling submit should recycle the unused layer semaphore\n");
        return 0;
    }
    if (g_discarded_present_sem_count != 0) {
        fprintf(stderr, "late signaling submit failure should not discard the recycled layer semaphore\n");
        return 0;
    }
    if (sc_a->present_wait_sems[0] != VK_NULL_HANDLE ||
        sc_b->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "failed signaling submit should not leave tracked present semaphores behind\n");
        return 0;
    }
    return 1;
}

static int test_mixed_multi_present_tracks_successful_swapchains_only(void) {
    reset_state();

    VkImage images_a[] = { fake_image(0x10e) };
    VkImage images_b[] = { fake_image(0x10f) };
    const VkSwapchainKHR swapchains[] = {
        fake_swapchain(0x30e),
        fake_swapchain(0x30f),
    };
    const uint32_t image_indices[] = { 0, 0 };
    VkResult present_results[] = {
        VK_SUCCESS,
        VK_ERROR_OUT_OF_DATE_KHR,
    };
    const VkQueue queue = fake_queue(0x40c);
    const VkDevice device = fake_device(0x50c);

    add_device(device, queue, 21);
    SwapchainData* sc_a =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images_a, 1);
    SwapchainData* sc_b =
        add_swapchain(device, swapchains[1], VK_FORMAT_B8G8R8A8_UNORM, images_b, 1);
    g_next_present_semaphore = (VkSemaphore)0x60d;
    g_queue_present_result = VK_ERROR_OUT_OF_DATE_KHR;
    g_queue_present_results[0] = VK_SUCCESS;
    g_queue_present_results[1] = VK_ERROR_OUT_OF_DATE_KHR;
    g_queue_present_results_count = 2;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 2,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
        .pResults = present_results,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_ERROR_OUT_OF_DATE_KHR) {
        fprintf(stderr, "mixed multi-present should surface the aggregate error\n");
        return 0;
    }
    if (g_discarded_present_sem_count != 0) {
        fprintf(stderr, "mixed multi-present should not discard a semaphore still needed by successful swapchains\n");
        return 0;
    }
    if (sc_a->present_wait_sems[0] != g_next_present_semaphore ||
        sc_b->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "mixed multi-present should only track successful swapchain images\n");
        return 0;
    }

    uint32_t image_index = UINT32_MAX;
    if (sigaw_AcquireNextImageKHR(
            device,
            swapchains[0],
            0,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            &image_index
        ) != VK_SUCCESS) {
        fprintf(stderr, "successful swapchain reacquire should succeed\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 1 ||
        g_recycled_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "tracked mixed-present semaphore should recycle after the successful swapchain is reacquired\n");
        return 0;
    }
    return 1;
}

static int test_ambiguous_multi_present_without_results_retains_layer_semaphore(void) {
    reset_state();

    VkImage images_a[] = { fake_image(0x110) };
    VkImage images_b[] = { fake_image(0x111) };
    const VkSwapchainKHR swapchains[] = {
        fake_swapchain(0x310),
        fake_swapchain(0x311),
    };
    const uint32_t image_indices[] = { 0, 0 };
    const VkQueue queue = fake_queue(0x40d);
    const VkDevice device = fake_device(0x50d);

    add_device(device, queue, 22);
    SwapchainData* sc_a =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images_a, 1);
    SwapchainData* sc_b =
        add_swapchain(device, swapchains[1], VK_FORMAT_B8G8R8A8_UNORM, images_b, 1);
    g_next_present_semaphore = (VkSemaphore)0x60e;
    g_queue_present_result = VK_ERROR_OUT_OF_DATE_KHR;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 2,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_ERROR_OUT_OF_DATE_KHR) {
        fprintf(stderr, "ambiguous multi-present should surface the aggregate error\n");
        return 0;
    }
    if (g_discarded_present_sem_count != 0) {
        fprintf(stderr, "ambiguous multi-present should retain the layer semaphore conservatively\n");
        return 0;
    }
    if (sc_a->present_wait_sems[0] != g_next_present_semaphore ||
        sc_b->present_wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "ambiguous multi-present should track the semaphore on every swapchain image\n");
        return 0;
    }
    return 1;
}

static int test_failed_present_discards_layer_semaphore(void) {
    reset_state();

    VkImage images[] = { fake_image(0x106) };
    const VkSwapchainKHR swapchains[] = { fake_swapchain(0x306) };
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x405);
    const VkDevice device = fake_device(0x505);

    add_device(device, queue, 14);
    SwapchainData* sc =
        add_swapchain(device, swapchains[0], VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x605;
    g_queue_present_result = VK_ERROR_OUT_OF_DATE_KHR;
    g_require_wait_before_discard = 1;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_ERROR_OUT_OF_DATE_KHR) {
        fprintf(stderr, "failed present should surface the driver error\n");
        return 0;
    }
    if (g_device_wait_idle_call_count != 1 ||
        g_device_wait_idle_last_device != device) {
        fprintf(stderr, "failed present should wait for device idle before discarding the layer semaphore\n");
        return 0;
    }
    if (g_discard_before_wait_detected) {
        fprintf(stderr, "failed present discarded the layer semaphore before waiting for present completion\n");
        return 0;
    }
    if (g_discarded_present_sem_count != 1 ||
        g_discarded_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "failed present should discard the signaled layer semaphore\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 0) {
        fprintf(stderr, "failed present should not recycle a semaphore that never reached present\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "failed present should not leave swapchain ownership of the discarded semaphore\n");
        return 0;
    }
    return 1;
}

static int test_queue_family_switch_waits_before_clearing_present_tracking(void) {
    reset_state();

    VkImage images[] = { fake_image(0x112) };
    const VkSwapchainKHR swapchain = fake_swapchain(0x312);
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue_a = fake_queue(0x40e);
    const VkQueue queue_b = fake_queue(0x40f);
    const VkDevice device = fake_device(0x50e);

    DeviceData* dev = add_device(device, queue_a, 23);
    dev->queue_count = 2;
    dev->queues[1] = queue_b;
    dev->queue_families[1] = 24;

    SwapchainData* sc =
        add_swapchain(device, swapchain, VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x60f;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue_a, &present) != VK_SUCCESS) {
        fprintf(stderr, "initial present before queue-family switch should succeed\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "initial present should leave a tracked semaphore behind\n");
        return 0;
    }

    g_require_wait_before_discard = 1;
    if (sigaw_QueuePresentKHR(queue_b, &present) != VK_SUCCESS) {
        fprintf(stderr, "queue-family switch present should still forward successfully\n");
        return 0;
    }
    if (g_device_wait_idle_call_count != 1 ||
        g_device_wait_idle_last_device != device) {
        fprintf(stderr, "queue-family switch should wait for the device before clearing tracked semaphores\n");
        return 0;
    }
    if (g_discard_before_wait_detected) {
        fprintf(stderr, "tracked present semaphores were discarded before waiting for old presents to finish\n");
        return 0;
    }
    if (g_discarded_present_sem_count != 1 ||
        g_discarded_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "queue-family switch should discard the old tracked semaphore after waiting idle\n");
        return 0;
    }
    if (sc->present_wait_sems[0] != VK_NULL_HANDLE) {
        fprintf(stderr, "queue-family switch should clear tracked swapchain ownership after teardown\n");
        return 0;
    }
    return 1;
}

static int test_destroy_swapchain_discards_tracked_present_semaphore(void) {
    reset_state();

    VkImage images[] = { fake_image(0x10d) };
    const VkSwapchainKHR swapchain = fake_swapchain(0x30d);
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x40b);
    const VkDevice device = fake_device(0x50b);

    add_device(device, queue, 20);
    add_swapchain(device, swapchain, VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x60c;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "present before destroy should succeed\n");
        return 0;
    }

    g_require_wait_before_discard = 1;
    sigaw_DestroySwapchainKHR(device, swapchain, NULL);

    if (g_device_wait_idle_call_count != 1 ||
        g_device_wait_idle_last_device != device) {
        fprintf(stderr, "destroy swapchain should wait for tracked presents before discarding semaphores\n");
        return 0;
    }
    if (g_discard_before_wait_detected) {
        fprintf(stderr, "destroy swapchain discarded a tracked semaphore before waiting for present completion\n");
        return 0;
    }
    if (g_discarded_present_sem_count != 1 ||
        g_discarded_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "destroy swapchain should discard the tracked layer semaphore\n");
        return 0;
    }
    if (g_recycled_present_sem_count != 0) {
        fprintf(stderr, "destroy swapchain should not recycle a semaphore that will never be reacquired\n");
        return 0;
    }
    if (g_swapchain_count != 0) {
        fprintf(stderr, "destroy swapchain should release the tracked swapchain slot\n");
        return 0;
    }
    return 1;
}

static int test_destroy_device_waits_before_clearing_present_tracking(void) {
    reset_state();

    VkImage images[] = { fake_image(0x115) };
    const VkSwapchainKHR swapchain = fake_swapchain(0x315);
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x412);
    const VkDevice device = fake_device(0x511);

    add_device(device, queue, 27);
    add_swapchain(device, swapchain, VK_FORMAT_B8G8R8A8_UNORM, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x611;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "present before destroy device should succeed\n");
        return 0;
    }

    g_require_wait_before_discard = 1;
    sigaw_DestroyDevice(device, NULL);

    if (g_device_wait_idle_call_count != 1 ||
        g_device_wait_idle_last_device != device) {
        fprintf(stderr, "destroy device should wait for tracked presents before clearing semaphores\n");
        return 0;
    }
    if (g_discard_before_wait_detected) {
        fprintf(stderr, "destroy device discarded a tracked semaphore before waiting for present completion\n");
        return 0;
    }
    if (g_discarded_present_sem_count != 1 ||
        g_discarded_present_sems[0] != g_next_present_semaphore) {
        fprintf(stderr, "destroy device should discard the tracked layer semaphore after waiting\n");
        return 0;
    }
    if (g_swapchain_count != 0) {
        fprintf(stderr, "destroy device should release tracked swapchains\n");
        return 0;
    }
    if (find_device(device) != NULL) {
        fprintf(stderr, "destroy device should clear the device bookkeeping slot\n");
        return 0;
    }
    return 1;
}

static int test_get_device_proc_addr_hides_unsupported_acquire_next_image2_names(void) {
    reset_state();

    const VkQueue queue = fake_queue(0x410);
    const VkDevice device = fake_device(0x50f);

    add_device(device, queue, 25);
    g_device_proc_mask = FAKE_DEVICE_PROC_ACQUIRE_NEXT_IMAGE2_KHR;

    if (sigaw_GetDeviceProcAddr(device, "vkAcquireNextImage2") != NULL) {
        fprintf(stderr, "core acquire next image2 should stay hidden when the next layer does not expose it\n");
        return 0;
    }
    if (sigaw_GetDeviceProcAddr(device, "vkAcquireNextImage2KHR") == NULL) {
        fprintf(stderr, "KHR acquire next image2 should still be intercepted when the next layer exposes it\n");
        return 0;
    }

    g_device_proc_mask = 0;
    if (sigaw_GetDeviceProcAddr(device, "vkAcquireNextImage2KHR") != NULL) {
        fprintf(stderr, "KHR acquire next image2 should stay hidden when unsupported by the next layer\n");
        return 0;
    }
    return 1;
}

static int test_unmodified_present_when_no_swapchain_is_renderable(void) {
    reset_state();

    VkImage images[] = { fake_image(0x103) };
    VkSemaphore app_wait = (VkSemaphore)0x202;
    const VkSemaphore app_waits[] = { app_wait };
    const VkSwapchainKHR swapchains[] = { fake_swapchain(0x303) };
    const uint32_t image_indices[] = { 0 };
    const VkQueue queue = fake_queue(0x402);
    const VkDevice device = fake_device(0x502);

    add_device(device, queue, 11);
    add_swapchain(device, swapchains[0], VK_FORMAT_R16G16B16A16_SFLOAT, images, 1);
    g_next_present_semaphore = (VkSemaphore)0x602;

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = app_waits,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = image_indices,
    };

    if (sigaw_QueuePresentKHR(queue, &present) != VK_SUCCESS) {
        fprintf(stderr, "unmodified present should still succeed\n");
        return 0;
    }
    if (g_render_capture_count != 0) {
        fprintf(stderr, "unsupported swapchain format should skip overlay rendering\n");
        return 0;
    }
    if (g_present_capture.wait_count != 1 ||
        g_present_capture.wait_sems[0] != app_wait) {
        fprintf(stderr, "present wait chain should remain unchanged when no overlay work is submitted\n");
        return 0;
    }
    return 1;
}

int main(void) {
    if (!test_single_overlay_submit_forwards_layer_semaphore()) {
        return 1;
    }
    if (!test_wine_auto_single_overlay_uses_wait_free_present()) {
        return 1;
    }
    if (!test_acquire_next_image_recycles_present_semaphore()) {
        return 1;
    }
    if (!test_acquire_next_image2_recycles_present_semaphore()) {
        return 1;
    }
    if (!test_acquire_next_image2_core_recycles_present_semaphore()) {
        return 1;
    }
    if (!test_wine_auto_multi_overlay_uses_wait_free_present()) {
        return 1;
    }
    if (!test_multi_swapchain_overlay_only_signals_last_submit()) {
        return 1;
    }
    if (!test_wine_force_preserves_layer_present_semaphore_path()) {
        return 1;
    }
    if (!test_multi_present_tracks_every_swapchain_image()) {
        return 1;
    }
    if (!test_wine_disable_bypasses_overlay_and_preserves_present_waits()) {
        return 1;
    }
    if (!test_failed_present_signaller_preserves_original_waits()) {
        return 1;
    }
    if (!test_mixed_multi_present_tracks_successful_swapchains_only()) {
        return 1;
    }
    if (!test_ambiguous_multi_present_without_results_retains_layer_semaphore()) {
        return 1;
    }
    if (!test_failed_present_discards_layer_semaphore()) {
        return 1;
    }
    if (!test_queue_family_switch_waits_before_clearing_present_tracking()) {
        return 1;
    }
    if (!test_destroy_swapchain_discards_tracked_present_semaphore()) {
        return 1;
    }
    if (!test_destroy_device_waits_before_clearing_present_tracking()) {
        return 1;
    }
    if (!test_get_device_proc_addr_hides_unsupported_acquire_next_image2_names()) {
        return 1;
    }
    if (!test_unmodified_present_when_no_swapchain_is_renderable()) {
        return 1;
    }
    return 0;
}
