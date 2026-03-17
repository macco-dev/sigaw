#ifndef SIGAW_SWAPCHAIN_BOOKKEEPING_H
#define SIGAW_SWAPCHAIN_BOOKKEEPING_H

#include <vulkan/vulkan.h>
#include <stdlib.h>

typedef struct {
    VkSwapchainKHR swapchain;
    VkDevice       device;
    VkFormat       format;
    uint32_t       width;
    uint32_t       height;
    VkImage*       images;
    uint32_t       image_count;
} SigawSwapchainData;

static inline SigawSwapchainData* sigaw_find_swapchain(SigawSwapchainData* swapchains,
                                                       int count,
                                                       VkSwapchainKHR swapchain)
{
    for (int i = 0; i < count; ++i) {
        if (swapchains[i].swapchain == swapchain) {
            return &swapchains[i];
        }
    }
    return NULL;
}

static inline void sigaw_clear_swapchain_slot(SigawSwapchainData* sc) {
    if (!sc) {
        return;
    }

    free(sc->images);
    sc->images = NULL;
    sc->image_count = 0;
    sc->swapchain = VK_NULL_HANDLE;
    sc->device = VK_NULL_HANDLE;
    sc->format = VK_FORMAT_UNDEFINED;
    sc->width = 0;
    sc->height = 0;
}

static inline void sigaw_compact_swapchain_tail(SigawSwapchainData* swapchains, int* count) {
    while (*count > 0 && swapchains[*count - 1].swapchain == VK_NULL_HANDLE) {
        --(*count);
    }
}

static inline void sigaw_release_swapchain(SigawSwapchainData* swapchains,
                                           int* count,
                                           SigawSwapchainData* sc)
{
    sigaw_clear_swapchain_slot(sc);
    sigaw_compact_swapchain_tail(swapchains, count);
}

static inline void sigaw_release_swapchains_for_device(SigawSwapchainData* swapchains,
                                                       int* count,
                                                       VkDevice device)
{
    for (int i = 0; i < *count; ++i) {
        if (swapchains[i].swapchain != VK_NULL_HANDLE &&
            swapchains[i].device == device) {
            sigaw_clear_swapchain_slot(&swapchains[i]);
        }
    }
    sigaw_compact_swapchain_tail(swapchains, count);
}

static inline SigawSwapchainData* sigaw_allocate_swapchain_slot(SigawSwapchainData* swapchains,
                                                                int* count,
                                                                int capacity)
{
    for (int i = 0; i < *count; ++i) {
        if (swapchains[i].swapchain == VK_NULL_HANDLE) {
            return &swapchains[i];
        }
    }

    if (*count < capacity) {
        return &swapchains[(*count)++];
    }

    return NULL;
}

#endif /* SIGAW_SWAPCHAIN_BOOKKEEPING_H */
