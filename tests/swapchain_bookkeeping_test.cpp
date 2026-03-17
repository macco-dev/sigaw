#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "layer/swapchain_bookkeeping.h"

namespace {

VkDevice fake_device(uintptr_t value) {
    return reinterpret_cast<VkDevice>(value);
}

VkSwapchainKHR fake_swapchain(uintptr_t value) {
    return reinterpret_cast<VkSwapchainKHR>(value);
}

bool test_reuses_freed_middle_slot() {
    SigawSwapchainData swapchains[3] = {};
    int count = 0;

    auto* first = sigaw_allocate_swapchain_slot(swapchains, &count, 3);
    if (first) {
        first->swapchain = fake_swapchain(1);
    }
    auto* second = sigaw_allocate_swapchain_slot(swapchains, &count, 3);
    if (second) {
        second->swapchain = fake_swapchain(2);
    }
    auto* third = sigaw_allocate_swapchain_slot(swapchains, &count, 3);
    if (third) {
        third->swapchain = fake_swapchain(3);
    }
    if (!first || !second || !third || count != 3) {
        std::cerr << "failed to allocate initial swapchain slots: "
                  << "first=" << first << " second=" << second
                  << " third=" << third << " count=" << count << "\n";
        return false;
    }

    second->images = static_cast<VkImage*>(std::malloc(sizeof(VkImage)));
    second->image_count = 1;

    sigaw_release_swapchain(swapchains, &count, second);
    if (count != 3) {
        std::cerr << "releasing a middle slot should preserve the occupied tail\n";
        return false;
    }

    auto* reused = sigaw_allocate_swapchain_slot(swapchains, &count, 3);
    if (reused != &swapchains[1]) {
        std::cerr << "allocator should reuse the first free middle slot\n";
        return false;
    }

    return true;
}

bool test_device_release_clears_owned_swapchains_and_compacts() {
    SigawSwapchainData swapchains[4] = {};
    int count = 0;

    auto* first = sigaw_allocate_swapchain_slot(swapchains, &count, 4);
    if (first) {
        first->swapchain = fake_swapchain(10);
    }
    auto* second = sigaw_allocate_swapchain_slot(swapchains, &count, 4);
    if (second) {
        second->swapchain = fake_swapchain(11);
    }
    auto* third = sigaw_allocate_swapchain_slot(swapchains, &count, 4);
    if (third) {
        third->swapchain = fake_swapchain(12);
    }
    if (!first || !second || !third) {
        std::cerr << "failed to allocate swapchain slots for device cleanup test\n";
        return false;
    }

    first->device = fake_device(100);
    first->images = static_cast<VkImage*>(std::malloc(sizeof(VkImage)));
    first->image_count = 1;

    second->device = fake_device(200);
    second->images = static_cast<VkImage*>(std::malloc(sizeof(VkImage)));
    second->image_count = 1;

    third->device = fake_device(100);
    third->images = static_cast<VkImage*>(std::malloc(sizeof(VkImage)));
    third->image_count = 1;

    sigaw_release_swapchains_for_device(swapchains, &count, fake_device(100));

    if (sigaw_find_swapchain(swapchains, count, fake_swapchain(10)) != nullptr ||
        sigaw_find_swapchain(swapchains, count, fake_swapchain(12)) != nullptr) {
        std::cerr << "device cleanup should remove all swapchains owned by that device\n";
        return false;
    }

    if (sigaw_find_swapchain(swapchains, count, fake_swapchain(11)) == nullptr) {
        std::cerr << "device cleanup should preserve swapchains owned by other devices\n";
        return false;
    }

    if (count != 2) {
        std::cerr << "device cleanup should compact trailing free slots\n";
        return false;
    }

    sigaw_release_swapchains_for_device(swapchains, &count, fake_device(200));
    if (count != 0) {
        std::cerr << "releasing the remaining device should compact the table to zero\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_reuses_freed_middle_slot()) {
        return 1;
    }

    if (!test_device_release_clears_owned_swapchains_and_compacts()) {
        return 1;
    }

    return 0;
}
