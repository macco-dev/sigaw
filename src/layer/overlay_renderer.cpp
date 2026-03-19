#include <vulkan/vulkan.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <png.h>

#include "overlay_animation.h"
#include "overlay_layout.h"
#include "overlay_preview.h"
#include "overlay_runtime.h"
#include "overlay_visibility.h"
#include "vk_dispatch.h"
#include "../common/config.h"
#include "../common/config_watcher.h"
#include "../common/protocol.h"
#include "shm_reader.h"

extern "C" {
struct SigawOverlayContext;
SigawOverlayContext* sigaw_overlay_create(VkDevice, VkPhysicalDevice, VkInstance, uint32_t,
                                          VkQueue, const SigawVulkanDispatch*,
                                          PFN_vkGetPhysicalDeviceMemoryProperties,
                                          VkFormat, uint32_t, uint32_t);
void sigaw_overlay_destroy(SigawOverlayContext*);
void sigaw_overlay_resize(SigawOverlayContext*, VkFormat, uint32_t, uint32_t);
VkSemaphore sigaw_overlay_acquire_present_semaphore(SigawOverlayContext*);
void sigaw_overlay_recycle_present_semaphore(SigawOverlayContext*, VkSemaphore);
void sigaw_overlay_discard_present_semaphore(SigawOverlayContext*, VkSemaphore);
int sigaw_overlay_render(SigawOverlayContext*, VkQueue, VkImage, VkFormat,
                         uint32_t, uint32_t, uint32_t, const VkSemaphore*,
                         VkSemaphore, VkFence);
}

namespace {

thread_local const SigawVulkanDispatch* g_sigaw_vk_dispatch = nullptr;

class ScopedDispatch {
public:
    explicit ScopedDispatch(const SigawVulkanDispatch* dispatch)
        : previous_(g_sigaw_vk_dispatch) {
        g_sigaw_vk_dispatch = dispatch;
    }

    ~ScopedDispatch() {
        g_sigaw_vk_dispatch = previous_;
    }

private:
    const SigawVulkanDispatch* previous_;
};

static inline const SigawVulkanDispatch& sigaw_current_vk_dispatch() {
    assert(g_sigaw_vk_dispatch != nullptr);
    return *g_sigaw_vk_dispatch;
}

} // namespace

#define vkAllocateCommandBuffers sigaw_current_vk_dispatch().AllocateCommandBuffers
#define vkAllocateDescriptorSets sigaw_current_vk_dispatch().AllocateDescriptorSets
#define vkAllocateMemory sigaw_current_vk_dispatch().AllocateMemory
#define vkBeginCommandBuffer sigaw_current_vk_dispatch().BeginCommandBuffer
#define vkBindBufferMemory sigaw_current_vk_dispatch().BindBufferMemory
#define vkBindImageMemory sigaw_current_vk_dispatch().BindImageMemory
#define vkCmdBeginRenderPass sigaw_current_vk_dispatch().CmdBeginRenderPass
#define vkCmdBindDescriptorSets sigaw_current_vk_dispatch().CmdBindDescriptorSets
#define vkCmdBindPipeline sigaw_current_vk_dispatch().CmdBindPipeline
#define vkCmdCopyBufferToImage sigaw_current_vk_dispatch().CmdCopyBufferToImage
#define vkCmdCopyImageToBuffer sigaw_current_vk_dispatch().CmdCopyImageToBuffer
#define vkCmdDraw sigaw_current_vk_dispatch().CmdDraw
#define vkCmdEndRenderPass sigaw_current_vk_dispatch().CmdEndRenderPass
#define vkCmdPipelineBarrier sigaw_current_vk_dispatch().CmdPipelineBarrier
#define vkCmdPushConstants sigaw_current_vk_dispatch().CmdPushConstants
#define vkCmdSetScissor sigaw_current_vk_dispatch().CmdSetScissor
#define vkCmdSetViewport sigaw_current_vk_dispatch().CmdSetViewport
#define vkCreateBuffer sigaw_current_vk_dispatch().CreateBuffer
#define vkCreateCommandPool sigaw_current_vk_dispatch().CreateCommandPool
#define vkCreateDescriptorPool sigaw_current_vk_dispatch().CreateDescriptorPool
#define vkCreateDescriptorSetLayout sigaw_current_vk_dispatch().CreateDescriptorSetLayout
#define vkCreateFence sigaw_current_vk_dispatch().CreateFence
#define vkCreateFramebuffer sigaw_current_vk_dispatch().CreateFramebuffer
#define vkCreateGraphicsPipelines sigaw_current_vk_dispatch().CreateGraphicsPipelines
#define vkCreateImage sigaw_current_vk_dispatch().CreateImage
#define vkCreateImageView sigaw_current_vk_dispatch().CreateImageView
#define vkCreatePipelineLayout sigaw_current_vk_dispatch().CreatePipelineLayout
#define vkCreateRenderPass sigaw_current_vk_dispatch().CreateRenderPass
#define vkCreateSampler sigaw_current_vk_dispatch().CreateSampler
#define vkCreateSemaphore sigaw_current_vk_dispatch().CreateSemaphore
#define vkCreateShaderModule sigaw_current_vk_dispatch().CreateShaderModule
#define vkDestroyBuffer sigaw_current_vk_dispatch().DestroyBuffer
#define vkDestroyCommandPool sigaw_current_vk_dispatch().DestroyCommandPool
#define vkDestroyDescriptorPool sigaw_current_vk_dispatch().DestroyDescriptorPool
#define vkDestroyDescriptorSetLayout sigaw_current_vk_dispatch().DestroyDescriptorSetLayout
#define vkDestroyFence sigaw_current_vk_dispatch().DestroyFence
#define vkDestroyFramebuffer sigaw_current_vk_dispatch().DestroyFramebuffer
#define vkDestroyImage sigaw_current_vk_dispatch().DestroyImage
#define vkDestroyImageView sigaw_current_vk_dispatch().DestroyImageView
#define vkDestroyPipeline sigaw_current_vk_dispatch().DestroyPipeline
#define vkDestroyPipelineLayout sigaw_current_vk_dispatch().DestroyPipelineLayout
#define vkDestroyRenderPass sigaw_current_vk_dispatch().DestroyRenderPass
#define vkDestroySampler sigaw_current_vk_dispatch().DestroySampler
#define vkDestroySemaphore sigaw_current_vk_dispatch().DestroySemaphore
#define vkDestroyShaderModule sigaw_current_vk_dispatch().DestroyShaderModule
#define vkDeviceWaitIdle sigaw_current_vk_dispatch().DeviceWaitIdle
#define vkEndCommandBuffer sigaw_current_vk_dispatch().EndCommandBuffer
#define vkFreeMemory sigaw_current_vk_dispatch().FreeMemory
#define vkGetBufferMemoryRequirements sigaw_current_vk_dispatch().GetBufferMemoryRequirements
#define vkGetImageMemoryRequirements sigaw_current_vk_dispatch().GetImageMemoryRequirements
#define vkMapMemory sigaw_current_vk_dispatch().MapMemory
#define vkQueueSubmit sigaw_current_vk_dispatch().QueueSubmit
#define vkResetCommandBuffer sigaw_current_vk_dispatch().ResetCommandBuffer
#define vkResetFences sigaw_current_vk_dispatch().ResetFences
#define vkUnmapMemory sigaw_current_vk_dispatch().UnmapMemory
#define vkUpdateDescriptorSets sigaw_current_vk_dispatch().UpdateDescriptorSets
#define vkWaitForFences sigaw_current_vk_dispatch().WaitForFences

namespace {

/* ---- Pixel types and blending ---- */

struct RGBA { uint8_t r, g, b, a; };

static inline RGBA mk(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return {r, g, b, a};
}

static inline RGBA apply_opacity(RGBA color, float opacity) {
    const float alpha = std::clamp(opacity, 0.0f, 1.0f);
    color.a = static_cast<uint8_t>(std::clamp(color.a * alpha, 0.0f, 255.0f));
    return color;
}

static inline RGBA alpha_blend(RGBA dst, RGBA src) {
    float sa = src.a * (1.0f / 255.0f);
    float da = 1.0f - sa;
    return {
        (uint8_t)(src.r * sa + dst.r * da),
        (uint8_t)(src.g * sa + dst.g * da),
        (uint8_t)(src.b * sa + dst.b * da),
        (uint8_t)std::min(255.0f, src.a + dst.a * da),
    };
}

/* ---- Pixel buffer with drawing primitives ---- */

struct PBuf {
    std::vector<RGBA> p;
    uint32_t w = 0, h = 0;

    static uint32_t to_unorm10(uint8_t v) {
        return ((uint32_t)v * 1023u + 127u) / 255u;
    }

    static uint32_t to_unorm2(uint8_t v) {
        return ((uint32_t)v * 3u + 127u) / 255u;
    }

    static uint8_t from_unorm10(uint32_t v) {
        return (uint8_t)((v * 255u + 511u) / 1023u);
    }

    static uint8_t from_unorm2(uint32_t v) {
        return (uint8_t)((v * 255u + 1u) / 3u);
    }

    void init(uint32_t ww, uint32_t hh) {
        w = ww; h = hh;
        p.assign(w * h, {0,0,0,0});
    }

    inline void put(int x, int y, RGBA c) {
        if ((unsigned)x < w && (unsigned)y < h)
            p[y * w + x] = alpha_blend(p[y * w + x], c);
    }

    void fill(int x0, int y0, int rw, int rh, RGBA c) {
        int ax = std::max(0, x0), ay = std::max(0, y0);
        int bx = std::min((int)w, x0+rw), by = std::min((int)h, y0+rh);
        for (int y = ay; y < by; y++)
            for (int x = ax; x < bx; x++)
                put(x, y, c);
    }

    void line(int x0, int y0, int x1, int y1, int thickness, RGBA c) {
        const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        const int radius = std::max(0, thickness / 2);
        if (steps == 0) {
            if (radius > 0) {
                disc(x0, y0, radius, c);
            } else {
                put(x0, y0, c);
            }
            return;
        }

        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const int x = static_cast<int>(std::lround(x0 + (x1 - x0) * t));
            const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * t));
            if (radius > 0) {
                disc(x, y, radius, c);
            } else {
                put(x, y, c);
            }
        }
    }

    /* Rounded rect with AA edges using corner SDF. */
    void rrect(int rx, int ry, int rw, int rh, int rad, RGBA c) {
        int ax = std::max(0, rx), ay = std::max(0, ry);
        int bx = std::min((int)w, rx+rw), by = std::min((int)h, ry+rh);
        for (int y = ay; y < by; y++) {
            for (int x = ax; x < bx; x++) {
                int lx = x - rx, ly = y - ry;
                int cdx = 0, cdy = 0;
                if      (lx < rad)      cdx = rad - lx;
                else if (lx >= rw - rad) cdx = lx - (rw - rad - 1);
                if      (ly < rad)      cdy = rad - ly;
                else if (ly >= rh - rad) cdy = ly - (rh - rad - 1);

                if (cdx > 0 && cdy > 0) {
                    float d = sqrtf((float)(cdx*cdx + cdy*cdy));
                    if (d > rad + 0.5f) continue;
                    if (d > rad - 0.5f) {
                        RGBA ac = c;
                        ac.a = (uint8_t)(c.a * (rad + 0.5f - d));
                        put(x, y, ac);
                        continue;
                    }
                }
                put(x, y, c);
            }
        }
    }

    /* Filled circle with AA. */
    void disc(int cx, int cy, int r, RGBA c) {
        float rf = (float)r;
        for (int y = cy-r-1; y <= cy+r+1; y++) {
            for (int x = cx-r-1; x <= cx+r+1; x++) {
                float dx = (float)(x-cx), dy = (float)(y-cy);
                float d = sqrtf(dx*dx+dy*dy);
                if (d > rf+0.5f) continue;
                RGBA ac = c;
                if (d > rf-0.5f) ac.a = (uint8_t)(c.a * (rf+0.5f-d));
                put(x, y, ac);
            }
        }
    }

    /* Ring (circle outline) with AA. */
    void ring(int cx, int cy, int r, int th, RGBA c) {
        float ro = (float)r, ri = (float)(r-th);
        for (int y = cy-r-2; y <= cy+r+2; y++) {
            for (int x = cx-r-2; x <= cx+r+2; x++) {
                float dx = (float)(x-cx), dy = (float)(y-cy);
                float d = sqrtf(dx*dx+dy*dy);
                if (d > ro+0.5f || d < ri-0.5f) continue;
                float a = 1.0f;
                if (d > ro-0.5f) a = ro+0.5f-d;
                if (d < ri+0.5f) a = std::min(a, d-ri+0.5f);
                a = std::max(0.0f, std::min(1.0f, a));
                RGBA ac = c; ac.a = (uint8_t)(c.a * a);
                put(x, y, ac);
            }
        }
    }

    void blit_alpha(int x0, int y0, const uint8_t* alpha, int aw, int ah, RGBA c) {
        for (int y = 0; y < ah; ++y) {
            for (int x = 0; x < aw; ++x) {
                const uint8_t a = alpha[y * aw + x];
                if (!a) {
                    continue;
                }
                RGBA px = c;
                px.a = static_cast<uint8_t>((uint32_t)px.a * a / 255u);
                put(x0 + x, y0 + y, px);
            }
        }
    }

    static RGBA sample_rgba(const std::vector<RGBA>& src, int sw, int sh, float u, float v) {
        if (src.empty() || sw <= 0 || sh <= 0) {
            return {0, 0, 0, 0};
        }

        const float sx = std::clamp(u, 0.0f, 1.0f) * (sw - 1);
        const float sy = std::clamp(v, 0.0f, 1.0f) * (sh - 1);
        const int x0 = static_cast<int>(sx);
        const int y0 = static_cast<int>(sy);
        const int x1 = std::min(sw - 1, x0 + 1);
        const int y1 = std::min(sh - 1, y0 + 1);
        const float tx = sx - x0;
        const float ty = sy - y0;

        auto mix = [](float a, float b, float t) {
            return a + (b - a) * t;
        };

        const RGBA c00 = src[y0 * sw + x0];
        const RGBA c10 = src[y0 * sw + x1];
        const RGBA c01 = src[y1 * sw + x0];
        const RGBA c11 = src[y1 * sw + x1];

        RGBA out = {};
        out.r = static_cast<uint8_t>(mix(mix(c00.r, c10.r, tx), mix(c01.r, c11.r, tx), ty));
        out.g = static_cast<uint8_t>(mix(mix(c00.g, c10.g, tx), mix(c01.g, c11.g, tx), ty));
        out.b = static_cast<uint8_t>(mix(mix(c00.b, c10.b, tx), mix(c01.b, c11.b, tx), ty));
        out.a = static_cast<uint8_t>(mix(mix(c00.a, c10.a, tx), mix(c01.a, c11.a, tx), ty));
        return out;
    }

    void circle_image(int cx, int cy, int diameter, const std::vector<RGBA>& src,
                      int sw, int sh, float opacity = 1.0f) {
        const float r = diameter * 0.5f;
        for (int y = static_cast<int>(std::floor(-r - 1)); y <= static_cast<int>(std::ceil(r + 1)); ++y) {
            for (int x = static_cast<int>(std::floor(-r - 1)); x <= static_cast<int>(std::ceil(r + 1)); ++x) {
                const float fx = x + 0.5f;
                const float fy = y + 0.5f;
                const float d = std::sqrt(fx * fx + fy * fy);
                if (d > r + 0.5f) {
                    continue;
                }

                float edge = 1.0f;
                if (d > r - 0.5f) {
                    edge = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                }

                RGBA sample = sample_rgba(src, sw, sh, (fx + r) / (diameter - 1),
                                          (fy + r) / (diameter - 1));
                sample.a = static_cast<uint8_t>(
                    std::clamp(sample.a * edge * std::clamp(opacity, 0.0f, 1.0f), 0.0f, 255.0f)
                );
                put(cx + x, cy + y, sample);
            }
        }
    }

    /* Line-art mute icon for inline pill status markers. */
    void icon_mute(int cx, int cy, int r, RGBA c) {
        const int stroke = std::max(1, r / 3);
        const int body_w = std::max(3, r);
        const int body_h = std::max(5, r + 2);
        rrect(cx - body_w / 2, cy - body_h / 2, body_w, body_h - 1,
              std::max(1, body_w / 2), mk(c.r, c.g, c.b, c.a / 2));
        line(cx, cy + body_h / 2 - 1, cx, cy + r + 1, stroke, c);
        line(cx - r / 2, cy + r + 1, cx + r / 2, cy + r + 1, stroke, c);
        line(cx - r - 1, cy - r + 1, cx + r + 1, cy + r + 1, std::max(2, stroke), c);
    }

    /* Headset-style deaf icon to pair with the mute icon inside pills. */
    void icon_deaf(int cx, int cy, int r, RGBA c) {
        const int stroke = std::max(1, r / 3);
        for (int dx = -r; dx <= r; ++dx) {
            const int dy = static_cast<int>(std::lround(std::sqrt(std::max(0, r * r - dx * dx))));
            for (int t = -stroke / 2; t <= stroke / 2; ++t) {
                put(cx + dx, cy - dy + t, c);
            }
        }
        fill(cx - r, cy - r / 4, stroke, r + stroke, c);
        fill(cx + r - stroke + 1, cy - r / 4, stroke, r + stroke, c);
        line(cx - r - 1, cy - r + 1, cx + r + 1, cy + r + 1, std::max(2, stroke), c);
    }

    static RGBA load_pixel(const uint8_t* src, VkFormat fmt) {
        if (fmt == VK_FORMAT_B8G8R8A8_UNORM ||
            fmt == VK_FORMAT_B8G8R8A8_SRGB) {
            return {src[2], src[1], src[0], src[3]};
        }
        if (fmt == VK_FORMAT_R8G8B8A8_UNORM ||
            fmt == VK_FORMAT_R8G8B8A8_SRGB) {
            return {src[0], src[1], src[2], src[3]};
        }

        const uint32_t packed = *(const uint32_t*)src;
        const uint32_t c0 = packed & 0x3ffu;
        const uint32_t c1 = (packed >> 10) & 0x3ffu;
        const uint32_t c2 = (packed >> 20) & 0x3ffu;
        const uint32_t a = (packed >> 30) & 0x3u;
        if (fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            return {from_unorm10(c0), from_unorm10(c1), from_unorm10(c2), from_unorm2(a)};
        }
        return {from_unorm10(c2), from_unorm10(c1), from_unorm10(c0), from_unorm2(a)};
    }

    static void store_pixel(uint8_t* dst, VkFormat fmt, RGBA c) {
        if (fmt == VK_FORMAT_B8G8R8A8_UNORM ||
            fmt == VK_FORMAT_B8G8R8A8_SRGB) {
            dst[0] = c.b;
            dst[1] = c.g;
            dst[2] = c.r;
            dst[3] = c.a;
            return;
        }
        if (fmt == VK_FORMAT_R8G8B8A8_UNORM ||
            fmt == VK_FORMAT_R8G8B8A8_SRGB) {
            dst[0] = c.r;
            dst[1] = c.g;
            dst[2] = c.b;
            dst[3] = c.a;
            return;
        }

        const uint32_t r = to_unorm10(c.r);
        const uint32_t g = to_unorm10(c.g);
        const uint32_t b = to_unorm10(c.b);
        const uint32_t a = to_unorm2(c.a);
        if (fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            *(uint32_t*)dst = r | (g << 10) | (b << 20) | (a << 30);
        } else {
            *(uint32_t*)dst = b | (g << 10) | (r << 20) | (a << 30);
        }
    }

    void composite_over(void* dst, VkFormat fmt, uint32_t out_w, uint32_t out_h) const {
        auto* out = (uint8_t*)dst;
        for (uint32_t y = 0; y < out_h; y++) {
            for (uint32_t x = 0; x < out_w; x++) {
                const RGBA src = p[y * w + x];
                if (src.a == 0) continue;
                uint8_t* px = out + ((size_t)y * out_w + x) * 4;
                const RGBA dst_px = load_pixel(px, fmt);
                store_pixel(px, fmt, alpha_blend(dst_px, src));
            }
        }
    }
};

/* ---- Avatar color palette (Discord defaults) ---- */
static RGBA avatar_color(uint64_t uid) {
    static const RGBA pal[] = {
        {88,101,242,255}, {87,242,135,255}, {254,231,92,255},
        {235,69,158,255}, {237,66,69,255},
    };
    return pal[uid % 5];
}

static RGBA mix_rgba(RGBA a, RGBA b, float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    auto lerp = [&](uint8_t lhs, uint8_t rhs) -> uint8_t {
        return static_cast<uint8_t>(lhs + (rhs - lhs) * clamped);
    };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

static std::vector<uint32_t> utf8_codepoints(std::string_view text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size(); ) {
        const uint8_t ch = static_cast<uint8_t>(text[i]);
        if (ch < 0x80) {
            out.push_back(ch);
            ++i;
            continue;
        }

        uint32_t cp = 0;
        size_t extra = 0;
        if ((ch & 0xE0) == 0xC0) {
            cp = ch & 0x1Fu;
            extra = 1;
        } else if ((ch & 0xF0) == 0xE0) {
            cp = ch & 0x0Fu;
            extra = 2;
        } else if ((ch & 0xF8) == 0xF0) {
            cp = ch & 0x07u;
            extra = 3;
        } else {
            out.push_back('?');
            ++i;
            continue;
        }

        if (i + extra >= text.size()) {
            out.push_back('?');
            break;
        }

        bool valid = true;
        for (size_t j = 1; j <= extra; ++j) {
            const uint8_t part = static_cast<uint8_t>(text[i + j]);
            if ((part & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (part & 0x3Fu);
        }

        out.push_back(valid ? cp : static_cast<uint32_t>('?'));
        i += valid ? (extra + 1) : 1;
    }

    if (out.empty()) {
        out.push_back('?');
    }
    return out;
}

static std::string avatar_monogram(const char* name) {
    if (!name || !*name) {
        return "?";
    }

    auto cps = utf8_codepoints(name);
    if (cps.empty()) {
        return "?";
    }

    uint32_t cp = cps.front();
    if (cp >= 'a' && cp <= 'z') {
        cp -= ('a' - 'A');
    }

    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
        return out;
    }

    if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return out;
    }

    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return out;
    }

    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    return out;
}

static std::filesystem::path find_asset_path(const std::filesystem::path& rel) {
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("SIGAW_ASSET_DIR"); env && *env) {
        candidates.emplace_back(std::filesystem::path(env) / rel);
    }
#ifdef SIGAW_SOURCE_DIR
    candidates.emplace_back(std::filesystem::path(SIGAW_SOURCE_DIR) / "assets" / rel);
#endif
#ifdef SIGAW_DATA_DIR
    candidates.emplace_back(std::filesystem::path(SIGAW_DATA_DIR) / rel);
#endif

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

static std::filesystem::path find_shader_path(std::string_view name) {
    std::vector<std::filesystem::path> candidates;
#ifdef SIGAW_BUILD_DIR
    candidates.emplace_back(std::filesystem::path(SIGAW_BUILD_DIR) / std::string(name));
#endif
#ifdef SIGAW_DATA_DIR
    candidates.emplace_back(std::filesystem::path(SIGAW_DATA_DIR) / "shaders" / std::string(name));
#endif

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

class FontFaceCache {
public:
    struct Metrics {
        int ascender = 0;
        int descender = 0;
        int height = 0;
    };

    struct Glyph {
        int left = 0;
        int top = 0;
        int advance = 0;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> alpha;
    };

    FontFaceCache() = default;
    ~FontFaceCache() {
        reset();
    }

    void reset() {
        if (face_) {
            FT_Done_Face(face_);
            face_ = nullptr;
        }
        sizes_.clear();
    }

    bool load(FT_Library lib, const std::filesystem::path& path) {
        if (face_) {
            return true;
        }
        if (path.empty()) {
            return false;
        }
        if (FT_New_Face(lib, path.string().c_str(), 0, &face_) != 0) {
            face_ = nullptr;
            return false;
        }
        FT_Select_Charmap(face_, FT_ENCODING_UNICODE);
        return true;
    }

    const Metrics& metrics(int px) {
        return size_cache(px).metrics;
    }

    const Glyph* glyph(int px, uint32_t codepoint) {
        auto& cache = size_cache(px);
        const auto it = cache.glyphs.find(codepoint);
        if (it != cache.glyphs.end()) {
            return &it->second;
        }

        if (!face_) {
            return nullptr;
        }

        FT_Set_Pixel_Sizes(face_, 0, std::max(1, px));
        if (FT_Load_Char(face_, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            return nullptr;
        }

        const FT_GlyphSlot slot = face_->glyph;
        Glyph glyph;
        glyph.left = slot->bitmap_left;
        glyph.top = slot->bitmap_top;
        glyph.advance = static_cast<int>(slot->advance.x >> 6);
        glyph.width = static_cast<int>(slot->bitmap.width);
        glyph.height = static_cast<int>(slot->bitmap.rows);
        glyph.alpha.resize(glyph.width * glyph.height);
        for (int row = 0; row < glyph.height; ++row) {
            std::memcpy(glyph.alpha.data() + row * glyph.width,
                        slot->bitmap.buffer + row * slot->bitmap.pitch,
                        static_cast<size_t>(glyph.width));
        }

        auto [inserted, _] = cache.glyphs.emplace(codepoint, std::move(glyph));
        return &inserted->second;
    }

private:
    struct SizeCache {
        bool initialized = false;
        Metrics metrics;
        std::unordered_map<uint32_t, Glyph> glyphs;
    };

    SizeCache& size_cache(int px) {
        auto& cache = sizes_[std::max(1, px)];
        if (cache.initialized || !face_) {
            return cache;
        }

        FT_Set_Pixel_Sizes(face_, 0, std::max(1, px));
        cache.metrics.ascender = static_cast<int>(face_->size->metrics.ascender >> 6);
        cache.metrics.descender = std::max(0, static_cast<int>(-(face_->size->metrics.descender >> 6)));
        cache.metrics.height = static_cast<int>(face_->size->metrics.height >> 6);
        if (cache.metrics.height <= 0) {
            cache.metrics.height = cache.metrics.ascender + cache.metrics.descender;
        }
        cache.initialized = true;
        return cache;
    }

    FT_Face                             face_ = nullptr;
    std::unordered_map<int, SizeCache>  sizes_;
};

enum class FontRole {
    Regular,
    Semibold,
};

class TextSystem {
public:
    ~TextSystem() {
        regular_face_.reset();
        semibold_face_.reset();
        if (lib_) {
            FT_Done_FreeType(lib_);
            lib_ = nullptr;
        }
    }

    bool ready() {
        if (ready_) {
            return true;
        }

        if (FT_Init_FreeType(&lib_) != 0) {
            return false;
        }

        auto regular = find_asset_path("fonts/NotoSans-Medium.ttf");
        auto semibold = find_asset_path("fonts/NotoSans-Bold.ttf");
        if (regular.empty() || semibold.empty()) {
            regular = find_asset_path("fonts/SourceSans3-Regular.otf");
            semibold = find_asset_path("fonts/SourceSans3-Semibold.otf");
        }
        if (!regular_face_.load(lib_, regular) || !semibold_face_.load(lib_, semibold)) {
            regular_face_.reset();
            semibold_face_.reset();
            FT_Done_FreeType(lib_);
            lib_ = nullptr;
            return false;
        }

        ready_ = true;
        return true;
    }

    int measure(FontRole role, int px, std::string_view text) {
        if (!ready()) {
            return 0;
        }
        int width = 0;
        for (uint32_t cp : utf8_codepoints(text)) {
            if (const auto* glyph = face(role).glyph(px, cp)) {
                width += glyph->advance;
            }
        }
        return width;
    }

    int draw(PBuf& pb, FontRole role, int px, int x, int baseline_y,
             std::string_view text, RGBA color, int max_width = INT_MAX) {
        if (!ready()) {
            return 0;
        }

        auto cps = utf8_codepoints(text);
        const auto ellipsis = utf8_codepoints("...");
        const int ellipsis_w = measure_codepoints(role, px, ellipsis);

        int draw_count = static_cast<int>(cps.size());
        if (max_width != INT_MAX && measure_codepoints(role, px, cps) > max_width) {
            int used = 0;
            draw_count = 0;
            for (uint32_t cp : cps) {
                const int advance = advance_for(role, px, cp);
                if (draw_count > 0 && used + advance + ellipsis_w > max_width) {
                    break;
                }
                if (draw_count == 0 && advance > max_width) {
                    break;
                }
                used += advance;
                ++draw_count;
            }
        }

        int pen_x = x;
        for (int i = 0; i < draw_count; ++i) {
            pen_x += draw_glyph(pb, role, px, cps[i], pen_x, baseline_y, color);
        }

        if (draw_count < static_cast<int>(cps.size())) {
            for (uint32_t cp : ellipsis) {
                pen_x += draw_glyph(pb, role, px, cp, pen_x, baseline_y, color);
            }
        }

        return pen_x - x;
    }

    FontFaceCache::Metrics metrics(FontRole role, int px) {
        if (!ready()) {
            return {};
        }
        return face(role).metrics(px);
    }

private:
    FontFaceCache& face(FontRole role) {
        return role == FontRole::Semibold ? semibold_face_ : regular_face_;
    }

    int measure_codepoints(FontRole role, int px, const std::vector<uint32_t>& cps) {
        int width = 0;
        for (uint32_t cp : cps) {
            width += advance_for(role, px, cp);
        }
        return width;
    }

    int advance_for(FontRole role, int px, uint32_t cp) {
        if (const auto* glyph = face(role).glyph(px, cp)) {
            return glyph->advance;
        }
        return std::max(1, px / 2);
    }

    int draw_glyph(PBuf& pb, FontRole role, int px, uint32_t cp, int pen_x,
                   int baseline_y, RGBA color) {
        if (const auto* glyph = face(role).glyph(px, cp)) {
            if (!glyph->alpha.empty()) {
                pb.blit_alpha(pen_x + glyph->left, baseline_y - glyph->top,
                              glyph->alpha.data(), glyph->width, glyph->height, color);
            }
            return glyph->advance;
        }
        return std::max(1, px / 2);
    }

    FT_Library    lib_ = nullptr;
    bool          ready_ = false;
    FontFaceCache regular_face_;
    FontFaceCache semibold_face_;
};

class AvatarStore {
public:
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<RGBA> pixels;
        std::filesystem::file_time_type mtime = {};
    };

    const Image* get(uint64_t user_id, const char* avatar_hash) {
        if (!avatar_hash || !*avatar_hash) {
            return nullptr;
        }

        const auto path = sigaw::Config::avatar_cache_path(user_id, avatar_hash);
        if (path.empty()) {
            return nullptr;
        }
        const auto key = path.string();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            const auto now = std::chrono::steady_clock::now();
            const auto miss = misses_.find(key);
            if (miss != misses_.end() &&
                now - miss->second < std::chrono::milliseconds(500)) {
                return nullptr;
            }
            misses_[key] = now;
            images_.erase(key);
            return nullptr;
        }

        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return nullptr;
        }

        auto found = images_.find(key);
        if (found != images_.end() && found->second.mtime == mtime) {
            return &found->second;
        }

        Image decoded;
        if (!load_png(path, decoded)) {
            misses_[key] = std::chrono::steady_clock::now();
            return nullptr;
        }

        decoded.mtime = mtime;
        auto [it, _] = images_.insert_or_assign(key, std::move(decoded));
        misses_.erase(key);
        return &it->second;
    }

private:
    static bool load_png(const std::filesystem::path& path, Image& out) {
        png_image image = {};
        image.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_file(&image, path.string().c_str())) {
            return false;
        }

        image.format = PNG_FORMAT_RGBA;
        if (image.width > 1024 || image.height > 1024) {
            png_image_free(&image);
            return false;
        }
        std::vector<uint8_t> bytes(PNG_IMAGE_SIZE(image));
        if (!png_image_finish_read(&image, nullptr, bytes.data(), 0, nullptr)) {
            png_image_free(&image);
            return false;
        }

        out.width = static_cast<int>(image.width);
        out.height = static_cast<int>(image.height);
        out.pixels.resize(static_cast<size_t>(out.width) * out.height);
        for (size_t i = 0; i < out.pixels.size(); ++i) {
            out.pixels[i] = {
                bytes[i * 4 + 0],
                bytes[i * 4 + 1],
                bytes[i * 4 + 2],
                bytes[i * 4 + 3],
            };
        }
        png_image_free(&image);
        return true;
    }

    std::unordered_map<std::string, Image>                                  images_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>  misses_;
};

/* ---- Global overlay state ---- */

struct Ctx {
    VkDevice         dev = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkInstance       inst = VK_NULL_HANDLE;
    VkQueue          q = VK_NULL_HANDLE;
    uint32_t         qf = 0;
    SigawVulkanDispatch dispatch = {};
    PFN_vkGetPhysicalDeviceMemoryProperties get_phys_props = nullptr;
    VkFormat         fmt = VK_FORMAT_UNDEFINED;
    uint32_t         sw = 0, sh = 0;

    VkCommandPool    pool = VK_NULL_HANDLE;
    VkCommandBuffer  cmd  = VK_NULL_HANDLE;
    VkFence          fen  = VK_NULL_HANDLE;
    std::vector<VkSemaphore> present_sems;
    std::vector<VkSemaphore> available_present_sems;
    VkBuffer         sbuf = VK_NULL_HANDLE;
    VkDeviceMemory   smem = VK_NULL_HANDLE;
    void*            sptr = nullptr;
    VkDeviceSize     ssz  = 0;
    VkImage          overlay_image = VK_NULL_HANDLE;
    VkDeviceMemory   overlay_mem = VK_NULL_HANDLE;
    VkImageView      overlay_view = VK_NULL_HANDLE;
    VkImageLayout    overlay_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler        sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet  desc_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass     render_pass = VK_NULL_HANDLE;
    VkPipeline       pipeline = VK_NULL_HANDLE;
    uint32_t         overlay_width = 0;
    uint32_t         overlay_height = 0;
    uint64_t         uploaded_sequence = 0;

    struct TargetView {
        VkImageView  view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t     width = 0;
        uint32_t     height = 0;
        VkFormat     format = VK_FORMAT_UNDEFINED;
    };
    std::unordered_map<VkImage, TargetView> target_views;

    sigaw::overlay::Runtime runtime;
    bool             prefer_gpu_composite = std::getenv("SIGAW_GPU_COMPOSITE") != nullptr;
    bool             logged_first_render = false;
    bool             logged_copy_path = false;
    bool             ok = false;
};

static bool supports_overlay_format(VkFormat fmt)
{
    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return true;
        default:
            return false;
    }
}

static uint32_t find_mem(Ctx& ctx, VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags f) {
    if (!ctx.get_phys_props) {
        return UINT32_MAX;
    }

    VkPhysicalDeviceMemoryProperties mp;
    ctx.get_phys_props(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    return UINT32_MAX;
}

static std::vector<uint32_t> read_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint32_t> code(static_cast<size_t>(size + 3) / 4u, 0u);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) {
        return {};
    }
    return code;
}

static VkShaderModule create_shader_module(VkDevice device, const std::filesystem::path& path) {
    const auto code = read_spirv(path);
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

static void destroy_target_views(Ctx& ctx) {
    for (auto& [_, target] : ctx.target_views) {
        if (target.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(ctx.dev, target.framebuffer, nullptr);
        }
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx.dev, target.view, nullptr);
        }
    }
    ctx.target_views.clear();
}

struct OverlayPushConstants {
    float surface_size[2];
    float panel_rect[4];
};

static bool ensure_staging_capacity(Ctx& ctx, size_t bytes) {
    if (ctx.sbuf != VK_NULL_HANDLE && bytes <= static_cast<size_t>(ctx.ssz)) {
        return true;
    }

    if (ctx.sptr != nullptr) {
        vkUnmapMemory(ctx.dev, ctx.smem);
        ctx.sptr = nullptr;
    }
    if (ctx.sbuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(ctx.dev, ctx.sbuf, nullptr);
        ctx.sbuf = VK_NULL_HANDLE;
    }
    if (ctx.smem != VK_NULL_HANDLE) {
        vkFreeMemory(ctx.dev, ctx.smem, nullptr);
        ctx.smem = VK_NULL_HANDLE;
    }

    ctx.ssz = std::max<VkDeviceSize>(2 * 1024 * 1024, static_cast<VkDeviceSize>(bytes));

    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = ctx.ssz;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx.dev, &bi, nullptr, &ctx.sbuf) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mr = {};
    vkGetBufferMemoryRequirements(ctx.dev, ctx.sbuf, &mr);

    VkMemoryAllocateInfo ma = {};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = find_mem(
        ctx, ctx.pdev, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    if (ma.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(ctx.dev, &ma, nullptr, &ctx.smem) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(ctx.dev, ctx.sbuf, ctx.smem, 0);
    if (vkMapMemory(ctx.dev, ctx.smem, 0, ctx.ssz, 0, &ctx.sptr) != VK_SUCCESS) {
        return false;
    }
    return true;
}

static void destroy_overlay_texture(Ctx& ctx) {
    if (ctx.overlay_view != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx.dev, ctx.overlay_view, nullptr);
        ctx.overlay_view = VK_NULL_HANDLE;
    }
    if (ctx.overlay_image != VK_NULL_HANDLE) {
        vkDestroyImage(ctx.dev, ctx.overlay_image, nullptr);
        ctx.overlay_image = VK_NULL_HANDLE;
    }
    if (ctx.overlay_mem != VK_NULL_HANDLE) {
        vkFreeMemory(ctx.dev, ctx.overlay_mem, nullptr);
        ctx.overlay_mem = VK_NULL_HANDLE;
    }
    ctx.overlay_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ctx.overlay_width = 0;
    ctx.overlay_height = 0;
    ctx.uploaded_sequence = 0;
}

static bool ensure_overlay_texture(Ctx& ctx, uint32_t width, uint32_t height) {
    if (ctx.overlay_image != VK_NULL_HANDLE &&
        ctx.overlay_width == width &&
        ctx.overlay_height == height) {
        return true;
    }

    destroy_overlay_texture(ctx);

    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {width, height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.dev, &ii, nullptr, &ctx.overlay_image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mr = {};
    vkGetImageMemoryRequirements(ctx.dev, ctx.overlay_image, &mr);

    VkMemoryAllocateInfo ma = {};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = find_mem(ctx, ctx.pdev, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX) {
        ma.memoryTypeIndex = find_mem(
            ctx, ctx.pdev, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
    }
    if (ma.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(ctx.dev, &ma, nullptr, &ctx.overlay_mem) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(ctx.dev, ctx.overlay_image, ctx.overlay_mem, 0);

    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = ctx.overlay_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx.dev, &vi, nullptr, &ctx.overlay_view) != VK_SUCCESS) {
        return false;
    }

    ctx.overlay_width = width;
    ctx.overlay_height = height;
    return true;
}

static bool ensure_descriptor_resources(Ctx& ctx) {
    if (ctx.sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        if (vkCreateSampler(ctx.dev, &si, nullptr, &ctx.sampler) != VK_SUCCESS) {
            return false;
        }
    }

    if (ctx.desc_layout == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo li = {};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(ctx.dev, &li, nullptr, &ctx.desc_layout) != VK_SUCCESS) {
            return false;
        }
    }

    if (ctx.desc_pool == VK_NULL_HANDLE) {
        VkDescriptorPoolSize pool_size = {};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 1;

        VkDescriptorPoolCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(ctx.dev, &pi, nullptr, &ctx.desc_pool) != VK_SUCCESS) {
            return false;
        }
    }

    if (ctx.desc_set == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = ctx.desc_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &ctx.desc_layout;
        if (vkAllocateDescriptorSets(ctx.dev, &ai, &ctx.desc_set) != VK_SUCCESS) {
            return false;
        }
    }

    if (ctx.pipeline_layout == VK_NULL_HANDLE) {
        const VkPushConstantRange push_range = {
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(OverlayPushConstants)
        };
        VkPipelineLayoutCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &ctx.desc_layout;
        pi.pushConstantRangeCount = 1;
        pi.pPushConstantRanges = &push_range;
        if (vkCreatePipelineLayout(ctx.dev, &pi, nullptr, &ctx.pipeline_layout) != VK_SUCCESS) {
            return false;
        }
    }

    if (ctx.overlay_view != VK_NULL_HANDLE && ctx.desc_set != VK_NULL_HANDLE) {
        VkDescriptorImageInfo image = {};
        image.sampler = ctx.sampler;
        image.imageView = ctx.overlay_view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = ctx.desc_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(ctx.dev, 1, &write, 0, nullptr);
    }

    return true;
}

static bool ensure_pipeline(Ctx& ctx, VkFormat target_format) {
    if (ctx.pipeline != VK_NULL_HANDLE &&
        ctx.render_pass != VK_NULL_HANDLE &&
        ctx.fmt == target_format) {
        return true;
    }

    destroy_target_views(ctx);
    if (ctx.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx.dev, ctx.pipeline, nullptr);
        ctx.pipeline = VK_NULL_HANDLE;
    }
    if (ctx.render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(ctx.dev, ctx.render_pass, nullptr);
        ctx.render_pass = VK_NULL_HANDLE;
    }
    ctx.fmt = target_format;

    const auto vert_path = find_shader_path("overlay_quad.vert.spv");
    const auto frag_path = find_shader_path("overlay_quad.frag.spv");
    if (vert_path.empty() || frag_path.empty()) {
        return false;
    }

    VkShaderModule vert = create_shader_module(ctx.dev, vert_path);
    VkShaderModule frag = create_shader_module(ctx.dev, frag_path);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) {
            vkDestroyShaderModule(ctx.dev, vert, nullptr);
        }
        if (frag != VK_NULL_HANDLE) {
            vkDestroyShaderModule(ctx.dev, frag, nullptr);
        }
        return false;
    }

    VkAttachmentDescription attachment = {};
    attachment.format = target_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rpi = {};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &attachment;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &subpass;
    if (vkCreateRenderPass(ctx.dev, &rpi, nullptr, &ctx.render_pass) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.dev, vert, nullptr);
        vkDestroyShaderModule(ctx.dev, frag, nullptr);
        return false;
    }

    if (!ensure_descriptor_resources(ctx)) {
        vkDestroyShaderModule(ctx.dev, vert, nullptr);
        vkDestroyShaderModule(ctx.dev, frag, nullptr);
        return false;
    }

    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend = {};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend = {};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;

    const VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo gp = {};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vertex_input;
    gp.pInputAssemblyState = &input_assembly;
    gp.pViewportState = &viewport;
    gp.pRasterizationState = &raster;
    gp.pMultisampleState = &multisample;
    gp.pColorBlendState = &color_blend;
    gp.pDynamicState = &dynamic;
    gp.layout = ctx.pipeline_layout;
    gp.renderPass = ctx.render_pass;
    gp.subpass = 0;

    const bool ok =
        vkCreateGraphicsPipelines(ctx.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &ctx.pipeline) == VK_SUCCESS;
    vkDestroyShaderModule(ctx.dev, vert, nullptr);
    vkDestroyShaderModule(ctx.dev, frag, nullptr);
    return ok;
}

static Ctx::TargetView* ensure_target_view(Ctx& ctx, VkImage target_image,
                                           VkFormat format,
                                           uint32_t width, uint32_t height)
{
    auto& target = ctx.target_views[target_image];
    if (target.view != VK_NULL_HANDLE &&
        target.framebuffer != VK_NULL_HANDLE &&
        target.format == format &&
        target.width == width &&
        target.height == height) {
        return &target;
    }

    if (target.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(ctx.dev, target.framebuffer, nullptr);
        target.framebuffer = VK_NULL_HANDLE;
    }
    if (target.view != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx.dev, target.view, nullptr);
        target.view = VK_NULL_HANDLE;
    }

    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = target_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx.dev, &vi, nullptr, &target.view) != VK_SUCCESS) {
        return nullptr;
    }

    VkFramebufferCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = ctx.render_pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &target.view;
    fi.width = width;
    fi.height = height;
    fi.layers = 1;
    if (vkCreateFramebuffer(ctx.dev, &fi, nullptr, &target.framebuffer) != VK_SUCCESS) {
        vkDestroyImageView(ctx.dev, target.view, nullptr);
        target.view = VK_NULL_HANDLE;
        return nullptr;
    }

    target.width = width;
    target.height = height;
    target.format = format;
    return &target;
}

/* ---- Panel rendering ---- */

static uint64_t monotonic_ms_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

static float chat_message_opacity(const SigawChatMessage& message, uint64_t now_ms) {
    constexpr uint64_t hold_ms = 10000;
    constexpr uint64_t fade_ms = 4000;
    constexpr uint64_t total_lifetime_ms = hold_ms + fade_ms;

    const uint64_t age = now_ms > message.observed_at_ms
        ? now_ms - message.observed_at_ms
        : 0u;
    if (age >= total_lifetime_ms) {
        return 0.0f;
    }
    if (age <= hold_ms) {
        return 1.0f;
    }

    return std::clamp(
        1.0f - static_cast<float>(age - hold_ms) / static_cast<float>(fade_ms),
        0.0f,
        1.0f
    );
}

static std::vector<std::string>
wrap_text_lines(TextSystem& text, FontRole role, int px,
                std::string_view input, int max_width, int max_lines = 0)
{
    std::vector<std::string> lines;
    if (input.empty() || max_width <= 0) {
        return lines;
    }

    std::string current;
    std::string word;
    const auto flush_word = [&]() {
        if (word.empty()) {
            return;
        }

        if (current.empty()) {
            current = word;
            word.clear();
            return;
        }

        const std::string candidate = current + " " + word;
        if (text.measure(role, px, candidate) <= max_width) {
            current = candidate;
        } else {
            lines.push_back(current);
            current = word;
        }
        word.clear();
    };

    for (char ch : input) {
        if (ch == ' ') {
            flush_word();
            continue;
        }
        word.push_back(ch);
    }
    flush_word();
    if (!current.empty()) {
        lines.push_back(current);
    }

    if (max_lines > 0 && static_cast<int>(lines.size()) > max_lines) {
        std::string tail = lines[max_lines - 1];
        for (size_t i = static_cast<size_t>(max_lines); i < lines.size(); ++i) {
            tail += " " + lines[i];
        }
        lines.resize(static_cast<size_t>(max_lines));
        lines.back() = std::move(tail);
    }

    return lines;
}

static void build_panel(const SigawState& vs, const sigaw::Config& cfg,
                        PBuf& pb, TextSystem& text, AvatarStore& avatars,
                        const std::unordered_map<uint64_t, float>& speaking_times_ms,
                        uint64_t now_ms)
{
    if (!text.ready()) {
        pb.init(0, 0);
        return;
    }

    const auto m = sigaw::overlay::metrics(cfg.scale);
    const auto visible_indices = sigaw::overlay::select_visible_user_indices(vs, (uint32_t)cfg.max_visible);
    const uint32_t visible = static_cast<uint32_t>(visible_indices.size());
    if (visible == 0) {
        pb.init(0, 0);
        return;
    }

    const bool show_header = cfg.show_channel && vs.header.channel_name_len > 0;
    const std::string channel_name =
        show_header ? std::string(vs.header.channel_name, vs.header.channel_name_len) : std::string();

    const auto name_metrics = text.metrics(FontRole::Semibold, m.name_font_px);
    const auto header_metrics = text.metrics(FontRole::Regular, m.header_font_px);
    const auto chat_metrics = text.metrics(FontRole::Regular, m.chat_font_px);
    const auto chat_author_metrics = text.metrics(FontRole::Semibold, m.chat_font_px);
    const auto mono_metrics = text.metrics(FontRole::Semibold, m.monogram_font_px);
    const int header_w = show_header
        ? sigaw::overlay::header_width_for_text(
              std::min(text.measure(FontRole::Regular, m.header_font_px, channel_name), m.max_text_width), m
          )
        : 0;

    std::vector<int> pill_widths;
    pill_widths.reserve(visible + 1);
    int pill_column_w = header_w;
    int avatar_column_w = 0;

    auto is_muted = [](const SigawUser& user) {
        return user.self_mute || user.server_mute;
    };
    auto is_deaf = [](const SigawUser& user) {
        return user.self_deaf || user.server_deaf;
    };
    auto status_icon_count = [&](const SigawUser& user) {
        return (is_muted(user) ? 1 : 0) + (is_deaf(user) ? 1 : 0);
    };

    auto row_pill_width = [&](std::string_view label) -> int {
        if (cfg.compact) {
            return m.row_height;
        }
        return sigaw::overlay::pill_width_for_text(
            std::min(text.measure(FontRole::Semibold, m.name_font_px, label), m.max_text_width), m
        );
    };

    auto user_row_layout = [&](int x, int y, int pill_w) {
        return cfg.compact
            ? sigaw::overlay::layout_compact_row(x, y, pill_w, m)
            : sigaw::overlay::layout_row(x, y, pill_w, m);
    };

    for (uint32_t i = 0; i < visible; ++i) {
        const auto& user = vs.users[visible_indices[i]];
        const int pill_w = cfg.compact
            ? row_pill_width(user.username)
            : sigaw::overlay::pill_width_for_text(
                  std::min(text.measure(FontRole::Semibold, m.name_font_px, user.username), m.max_text_width),
                  m, status_icon_count(user)
              );
        pill_widths.push_back(pill_w);
        pill_column_w = std::max(pill_column_w, pill_w);
        avatar_column_w = std::max(avatar_column_w, user_row_layout(0, 0, pill_w).total_w - pill_w);
    }

    std::string more_label;
    if (visible < vs.user_count) {
        more_label = "+" + std::to_string(vs.user_count - visible) + " more";
        const int more_w = sigaw::overlay::pill_width_for_text(
            std::min(text.measure(FontRole::Regular, m.header_font_px, more_label), m.max_text_width), m
        );
        pill_widths.push_back(more_w);
        pill_column_w = std::max(pill_column_w, more_w);
    }

    struct ChatRow {
        const SigawChatMessage* message = nullptr;
        float opacity = 0.0f;
        int width = 0;
        int height = 0;
        std::vector<std::string> content_lines;
    };

    std::vector<ChatRow> chat_rows;
    if (cfg.show_voice_channel_chat && cfg.max_visible_chat_messages > 0 && vs.chat_count > 0) {
        const uint32_t visible_chat = std::min(
            static_cast<uint32_t>(cfg.max_visible_chat_messages),
            std::min(vs.chat_count, static_cast<uint32_t>(SIGAW_MAX_CHAT_MESSAGES))
        );
        const uint32_t start = vs.chat_count > visible_chat ? vs.chat_count - visible_chat : 0;
        const int chat_block_w = std::max(pill_column_w + avatar_column_w, m.chat_max_text_width);
        const int chat_text_w = std::max(0, chat_block_w - m.chat_pad_x * 2);
        constexpr int max_chat_lines = 3;

        for (uint32_t i = start; i < vs.chat_count; ++i) {
            const auto& message = vs.chat_messages[i];
            if (message.author_name_len == 0 || message.content_len == 0) {
                continue;
            }

            const float opacity = chat_message_opacity(message, now_ms);
            if (opacity <= 0.0f) {
                continue;
            }

            const std::string_view author(message.author_name, message.author_name_len);
            const std::string_view content(message.content, message.content_len);
            auto content_lines = wrap_text_lines(
                text, FontRole::Regular, m.chat_font_px, content, chat_text_w, max_chat_lines
            );
            const int body_lines_h =
                static_cast<int>(content_lines.size()) * chat_metrics.height +
                std::max(0, static_cast<int>(content_lines.size()) - 1) * m.chat_line_gap;
            const int text_h = chat_author_metrics.height +
                (content_lines.empty() ? 0 : (m.chat_line_gap + body_lines_h));
            const int row_h = std::max(
                m.chat_row_height,
                text_h + m.chat_pad_y * 2
            );
            chat_rows.push_back(ChatRow{
                &message,
                opacity,
                chat_block_w,
                row_h,
                std::move(content_lines)
            });
        }
    }

    const int header_h = show_header ? (m.header_height + m.header_gap) : 0;
    const int row_count = static_cast<int>(visible);
    const int rows_h = row_count * m.row_height + std::max(0, row_count - 1) * m.row_gap;
    const int more_h = more_label.empty() ? 0 : (m.row_gap + m.row_height);
    int chat_block_w = 0;
    int chat_rows_h = 0;
    for (size_t i = 0; i < chat_rows.size(); ++i) {
        chat_block_w = std::max(chat_block_w, chat_rows[i].width);
        chat_rows_h += chat_rows[i].height;
        if (i + 1 < chat_rows.size()) {
            chat_rows_h += m.chat_row_gap;
        }
    }
    const int chat_h = chat_rows.empty() ? 0 : (m.chat_stack_gap + chat_rows_h);
    const int roster_content_w = pill_column_w + avatar_column_w;
    const int panel_content_w = std::max(roster_content_w, chat_block_w);
    const bool align_right =
        cfg.position == sigaw::OverlayPosition::TopRight ||
        cfg.position == sigaw::OverlayPosition::BottomRight;
    const int roster_origin_x = m.outer_pad + (align_right ? panel_content_w - roster_content_w : 0);
    const int chat_origin_x = m.outer_pad + (align_right ? panel_content_w - chat_block_w : 0);
    const uint32_t panel_w = static_cast<uint32_t>(m.outer_pad * 2 + std::max(panel_content_w, m.row_height));
    const uint32_t panel_h = static_cast<uint32_t>(m.outer_pad * 2 + header_h + rows_h + more_h + chat_h);
    pb.init(panel_w, panel_h);

    const RGBA bubble_bg = apply_opacity(mk(24, 24, 27, 246), cfg.opacity);
    const RGBA bubble_spk = apply_opacity(mk(32, 33, 37, 250), std::clamp(cfg.opacity + 0.04f, 0.0f, 1.0f));
    const RGBA bubble_muted = apply_opacity(mk(26, 26, 30, 248), cfg.opacity);
    const RGBA chat_bg = apply_opacity(mk(21, 22, 26, 234), cfg.opacity);
    const RGBA header_bg = apply_opacity(mk(24, 26, 30, 232), cfg.opacity);
    const RGBA base_text = mk(255, 255, 255, 255);
    const RGBA fg = apply_opacity(base_text, std::clamp(cfg.opacity + 0.08f, 0.0f, 1.0f));
    const RGBA dim = apply_opacity(mix_rgba(base_text, mk(176, 181, 188, 255), 0.55f), cfg.opacity);
    const RGBA shadow = apply_opacity(mk(0, 0, 0, 26), std::clamp(cfg.opacity * 0.45f, 0.0f, 1.0f));
    const RGBA speaking = apply_opacity(mk(35, 165, 89, 255), std::clamp(cfg.opacity + 0.10f, 0.0f, 1.0f));
    const RGBA muted = apply_opacity(mk(242, 63, 67, 255), std::clamp(cfg.opacity + 0.10f, 0.0f, 1.0f));
    const RGBA avatar_stroke = apply_opacity(mk(255, 255, 255, 50), cfg.opacity);
    const RGBA badge_bg = apply_opacity(mk(16, 18, 24, 228), cfg.opacity);
    const RGBA muted_text = apply_opacity(mix_rgba(base_text, mk(168, 173, 182, 255), 0.68f), cfg.opacity);
    const RGBA status_icon = apply_opacity(mix_rgba(muted_text, muted, 0.14f),
                                           std::clamp(cfg.opacity + 0.04f, 0.0f, 1.0f));
    const RGBA chat_author = apply_opacity(base_text, std::clamp(cfg.opacity + 0.10f, 0.0f, 1.0f));
    const RGBA chat_body = apply_opacity(mix_rgba(base_text, mk(196, 200, 208, 255), 0.30f), cfg.opacity);

    auto centered_baseline = [](int box_y, int box_h, const FontFaceCache::Metrics& metrics) {
        return box_y + (box_h - metrics.height) / 2 + metrics.ascender;
    };

    auto draw_avatar = [&](const SigawUser& u, const sigaw::overlay::RowLayout& row) {
        const auto* image = cfg.show_avatars ? avatars.get(u.user_id, u.avatar_hash) : nullptr;
        if (image) {
            pb.circle_image(row.avatar_cx, row.avatar_cy, m.avatar_size,
                            image->pixels, image->width, image->height, 1.0f);
        } else {
            const RGBA fallback = apply_opacity(avatar_color(u.user_id), std::clamp(cfg.opacity + 0.12f, 0.0f, 1.0f));
            pb.disc(row.avatar_cx, row.avatar_cy, m.avatar_radius, fallback);
            const std::string mono = avatar_monogram(u.username);
            const int mono_w = text.measure(FontRole::Semibold, m.monogram_font_px, mono);
            const int baseline = row.avatar_cy - mono_metrics.height / 2 + mono_metrics.ascender;
            text.draw(pb, FontRole::Semibold, m.monogram_font_px,
                      row.avatar_cx - mono_w / 2, baseline, mono, fg);
        }

        pb.ring(row.avatar_cx, row.avatar_cy, m.avatar_radius, m.avatar_stroke, avatar_stroke);
        const auto speak_it = speaking_times_ms.find(u.user_id);
        const float ring_alpha = speak_it == speaking_times_ms.end()
            ? 0.0f
            : sigaw::overlay::speaking_ring_alpha(speak_it->second);
        if (ring_alpha > 0.0f) {
            RGBA speaking_ring = speaking;
            speaking_ring.a = static_cast<uint8_t>(
                std::clamp(static_cast<float>(speaking_ring.a) * ring_alpha, 0.0f, 255.0f)
            );
            pb.ring(row.avatar_cx, row.avatar_cy, m.avatar_radius + std::max(2, m.avatar_stroke + 1),
                    std::max(2, m.avatar_stroke + 1), speaking_ring);
        }

        if (cfg.compact && (u.self_deaf || u.server_deaf || u.self_mute || u.server_mute)) {
            pb.disc(row.badge_cx, row.badge_cy, m.badge_radius, badge_bg);
            if (u.self_deaf || u.server_deaf) {
                pb.icon_deaf(row.badge_cx, row.badge_cy, std::max(3, m.badge_radius - 1), muted);
            } else {
                pb.icon_mute(row.badge_cx, row.badge_cy, std::max(3, m.badge_radius - 1), muted);
            }
        }
    };

    auto draw_status_icons = [&](const SigawUser& user, const sigaw::overlay::RowLayout& row) {
        if (cfg.compact) {
            return;
        }

        const bool show_mute = is_muted(user);
        const bool show_deaf = is_deaf(user);
        const int icon_count = (show_mute ? 1 : 0) + (show_deaf ? 1 : 0);
        if (icon_count == 0) {
            return;
        }

        int x = row.pill_x + row.pill_w - m.pill_pad_right - m.status_icon_size / 2;
        const int cy = row.pill_y + row.pill_h / 2;
        const int radius = std::max(3, m.status_icon_size / 2);

        if (show_deaf) {
            pb.icon_deaf(x, cy, radius, status_icon);
            x -= m.status_icon_size + m.status_icon_gap;
        }
        if (show_mute) {
            pb.icon_mute(x, cy, radius, status_icon);
        }
    };

    int y = m.outer_pad;
    if (show_header) {
        const int header_x = roster_origin_x + pill_column_w - header_w;
        pb.rrect(header_x + m.shadow_offset, y + m.shadow_offset,
                 header_w, m.header_height, m.header_radius, shadow);
        pb.rrect(header_x, y, header_w, m.header_height, m.header_radius, header_bg);
        text.draw(pb, FontRole::Regular, m.header_font_px,
                  header_x + m.header_pad_x,
                  centered_baseline(y, m.header_height, header_metrics),
                  channel_name, dim, m.max_text_width);
        y += m.header_height + m.header_gap;
    }

    auto draw_row = [&](const SigawUser* user, std::string_view label, int pill_w, bool active_bg, int row_y) {
        const int row_x = roster_origin_x + pill_column_w - pill_w;
        const auto row = user ? user_row_layout(row_x, row_y, pill_w)
                              : sigaw::overlay::layout_row(row_x, row_y, pill_w, m);
        const int icons = (!cfg.compact && user) ? status_icon_count(*user) : 0;
        const RGBA label_fg = (user && icons > 0) ? muted_text : fg;
        const RGBA pill_bg = active_bg ? bubble_spk : ((user && icons > 0) ? bubble_muted : bubble_bg);
        if (m.shadow_offset > 0 && shadow.a > 0) {
            pb.rrect(row.pill_x + m.shadow_offset, row.pill_y + m.shadow_offset,
                     row.pill_w, row.pill_h, m.pill_radius, shadow);
        }
        pb.rrect(row.pill_x, row.pill_y, row.pill_w, row.pill_h, m.pill_radius, pill_bg);
        if (!cfg.compact) {
            text.draw(pb, FontRole::Semibold, m.name_font_px,
                      row.pill_x + m.pill_pad_x,
                      centered_baseline(row.pill_y, row.pill_h, name_metrics),
                      label, label_fg, sigaw::overlay::text_max_width_for_pill(row.pill_w, m, icons));
        }
        if (user) {
            draw_status_icons(*user, row);
            draw_avatar(*user, row);
        }
    };

    for (uint32_t i = 0; i < visible; ++i) {
        const auto& user = vs.users[visible_indices[i]];
        const int row_y = y + static_cast<int>(i) * (m.row_height + m.row_gap);
        draw_row(&user, user.username, pill_widths[i], user.speaking, row_y);
    }

    if (!more_label.empty()) {
        const int row_y = y + static_cast<int>(visible) * (m.row_height + m.row_gap);
        const int more_w = pill_widths.back();
        const int row_x = roster_origin_x + pill_column_w - more_w;
        const auto row = sigaw::overlay::layout_row(row_x, row_y, more_w, m);
        if (m.shadow_offset > 0 && shadow.a > 0) {
            pb.rrect(row.pill_x + m.shadow_offset, row.pill_y + m.shadow_offset,
                     row.pill_w, row.pill_h, m.pill_radius, shadow);
        }
        pb.rrect(row.pill_x, row.pill_y, row.pill_w, row.pill_h, m.pill_radius, bubble_bg);
        text.draw(pb, FontRole::Regular, m.header_font_px,
                  row.pill_x + m.pill_pad_x,
                  centered_baseline(row.pill_y, row.pill_h, header_metrics),
                  more_label, dim, sigaw::overlay::text_max_width_for_pill(row.pill_w, m));
    }

    if (!chat_rows.empty()) {
        int chat_y = y + rows_h + more_h + m.chat_stack_gap;

        for (const auto& row : chat_rows) {
            const int row_x = chat_origin_x;
            const int row_w = row.width;
            const RGBA row_shadow = apply_opacity(shadow, row.opacity);
            const RGBA row_bg = apply_opacity(chat_bg, row.opacity);
            const RGBA row_author = apply_opacity(chat_author, row.opacity);
            const RGBA row_body = apply_opacity(chat_body, row.opacity);
            if (m.shadow_offset > 0 && row_shadow.a > 0) {
                pb.rrect(row_x + m.shadow_offset, chat_y + m.shadow_offset,
                         row_w, row.height, m.chat_radius, row_shadow);
            }
            pb.rrect(row_x, chat_y, row_w, row.height, m.chat_radius, row_bg);

            const auto& message = *row.message;
            const std::string_view author(message.author_name, message.author_name_len);
            const int available_w = std::max(0, row_w - m.chat_pad_x * 2);
            int text_y = chat_y + m.chat_pad_y;
            const int author_baseline = text_y + chat_author_metrics.ascender;
            text.draw(
                pb,
                FontRole::Semibold,
                m.chat_font_px,
                row_x + m.chat_pad_x,
                author_baseline,
                author,
                row_author,
                available_w
            );

            text_y += chat_author_metrics.height + m.chat_line_gap;
            for (const auto& line : row.content_lines) {
                text.draw(
                    pb,
                    FontRole::Regular,
                    m.chat_font_px,
                    row_x + m.chat_pad_x,
                    text_y + chat_metrics.ascender,
                    line,
                    row_body,
                    available_w
                );
                text_y += chat_metrics.height + m.chat_line_gap;
            }

            chat_y += row.height + m.chat_row_gap;
        }
    }
}

static sigaw::preview::Placement panel_placement(sigaw::OverlayPosition position,
                                                 uint32_t margin,
                                                 uint32_t panel_w, uint32_t panel_h,
                                                 uint32_t screen_w, uint32_t screen_h)
{
    int x = 0;
    int y = 0;
    switch (position) {
        case sigaw::OverlayPosition::TopLeft:
            x = static_cast<int>(margin);
            y = static_cast<int>(margin);
            break;
        case sigaw::OverlayPosition::TopRight:
            x = static_cast<int>(screen_w) - static_cast<int>(panel_w) - static_cast<int>(margin);
            y = static_cast<int>(margin);
            break;
        case sigaw::OverlayPosition::BottomLeft:
            x = static_cast<int>(margin);
            y = static_cast<int>(screen_h) - static_cast<int>(panel_h) - static_cast<int>(margin);
            break;
        case sigaw::OverlayPosition::BottomRight:
            x = static_cast<int>(screen_w) - static_cast<int>(panel_w) - static_cast<int>(margin);
            y = static_cast<int>(screen_h) - static_cast<int>(panel_h) - static_cast<int>(margin);
            break;
    }

    x = std::max(0, std::min(x, static_cast<int>(screen_w) - static_cast<int>(panel_w)));
    y = std::max(0, std::min(y, static_cast<int>(screen_h) - static_cast<int>(panel_h)));
    return {x, y};
}

static sigaw::preview::Placement panel_placement(const sigaw::Config& cfg,
                                                 uint32_t panel_w, uint32_t panel_h,
                                                 uint32_t screen_w, uint32_t screen_h)
{
    return panel_placement(
        cfg.position,
        static_cast<uint32_t>(sigaw::overlay::scaled_px(cfg.scale, 16)),
        panel_w,
        panel_h,
        screen_w,
        screen_h
    );
}

static bool render_panel_rgba_internal(const SigawState& state, const sigaw::Config& cfg,
                                       const std::unordered_map<uint64_t, float>& speaking_times_ms,
                                       sigaw::preview::Image& out,
                                       uint64_t now_ms)
{
    if (!cfg.visible) {
        out = {};
        return false;
    }

    TextSystem text;
    AvatarStore avatars;
    PBuf panel;
    build_panel(
        state,
        cfg,
        panel,
        text,
        avatars,
        speaking_times_ms,
        now_ms != 0 ? now_ms : monotonic_ms_now()
    );
    if (panel.p.empty() || panel.w == 0 || panel.h == 0) {
        out = {};
        return false;
    }

    out.width = panel.w;
    out.height = panel.h;
    out.rgba.resize(static_cast<size_t>(panel.w) * panel.h * 4u);
    for (size_t i = 0; i < panel.p.size(); ++i) {
        out.rgba[i * 4 + 0] = panel.p[i].r;
        out.rgba[i * 4 + 1] = panel.p[i].g;
        out.rgba[i * 4 + 2] = panel.p[i].b;
        out.rgba[i * 4 + 3] = panel.p[i].a;
    }
    return true;
}

static bool write_png_internal(const std::filesystem::path& path, const sigaw::preview::Image& image)
{
    if (image.width == 0 || image.height == 0 ||
        image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4u) {
        return false;
    }

    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    png.width = image.width;
    png.height = image.height;
    png.format = PNG_FORMAT_RGBA;
    return png_image_write_to_file(
        &png, path.string().c_str(), 0, image.rgba.data(), 0, nullptr
    ) != 0;
}

} /* anon */

namespace sigaw::preview {

bool render_panel_rgba(const SigawState& state, const sigaw::Config& cfg,
                       const std::unordered_map<uint64_t, float>& speaking_times_ms,
                       Image& out,
                       uint64_t now_ms)
{
    return render_panel_rgba_internal(state, cfg, speaking_times_ms, out, now_ms);
}

Placement place_panel(const sigaw::Config& cfg, uint32_t panel_w, uint32_t panel_h,
                      uint32_t screen_w, uint32_t screen_h)
{
    return panel_placement(cfg, panel_w, panel_h, screen_w, screen_h);
}

bool write_png(const std::filesystem::path& path, const Image& image)
{
    return write_png_internal(path, image);
}

} /* namespace sigaw::preview */

namespace sigaw::overlay {

struct Runtime::Impl {
    sigaw::ShmReader shm;
    sigaw::Config cfg;
    sigaw::ConfigWatcher cfg_watch;
    PBuf panel;
    TextSystem text;
    AvatarStore avatars;
    std::unordered_map<uint64_t, float> speaking_times_ms;
    std::chrono::steady_clock::time_point last_speaking_tick = {};
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> cached_rgba;
    uint32_t cached_width = 0;
    uint32_t cached_height = 0;
    uint64_t frame_sequence = 0;
    bool debug = std::getenv("SIGAW_DEBUG") != nullptr;

    void refresh_config(bool force = false) {
        if (force) {
            cfg = sigaw::Config::load_for_executable(sigaw::Config::current_executable_basename());
            cfg_watch.sync();
            return;
        }

        if (cfg_watch.consume_change()) {
            cfg = sigaw::Config::load_for_executable(sigaw::Config::current_executable_basename());
        }
    }

    void reset_speaking_animation() {
        speaking_times_ms.clear();
        last_speaking_tick = {};
    }

    float speaking_frame_delta_ms(std::chrono::steady_clock::time_point now) {
        float dt_ms = 0.0f;
        if (last_speaking_tick != std::chrono::steady_clock::time_point{}) {
            dt_ms = std::chrono::duration<float, std::milli>(now - last_speaking_tick).count();
        }
        last_speaking_tick = now;
        return std::clamp(dt_ms, 0.0f, sigaw::overlay::speaking_fade_ms);
    }

    void update_speaking_animation(const SigawState& vs, float dt_ms) {
        for (uint32_t i = 0; i < vs.user_count; ++i) {
            const auto& user = vs.users[i];
            auto& progress_ms = speaking_times_ms[user.user_id];
            progress_ms = sigaw::overlay::advance_speaking_time(
                progress_ms,
                user.speaking != 0,
                dt_ms
            );
        }

        for (auto it = speaking_times_ms.begin(); it != speaking_times_ms.end();) {
            bool present = false;
            for (uint32_t i = 0; i < vs.user_count; ++i) {
                if (vs.users[i].user_id == it->first) {
                    present = true;
                    break;
                }
            }

            if (!present) {
                it = speaking_times_ms.erase(it);
                continue;
            }

            ++it;
        }
    }

    PreparedFrame prepare(uint32_t surface_width, uint32_t surface_height) {
        PreparedFrame out;

        refresh_config();
        if (surface_width == 0 || surface_height == 0) {
            return out;
        }

        if (!cfg.visible) {
            return out;
        }

        SigawState vs = {};
        const bool have_voice_state = shm.read(vs);
        const auto now = std::chrono::steady_clock::now();
        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
        );
        if (debug && !have_voice_state) {
            fprintf(stderr, "[sigaw] Debug: shared memory read unavailable, rendering fallback panel\n");
        }

        if (have_voice_state) {
            update_speaking_animation(vs, speaking_frame_delta_ms(now));
        } else if (!shm.is_connected()) {
            reset_speaking_animation();
        }

        build_panel(vs, cfg, panel, text, avatars, speaking_times_ms, now_ms);
        if (panel.p.empty() || panel.w == 0 || panel.h == 0) {
            return out;
        }

        const auto placement = panel_placement(cfg, panel.w, panel.h, surface_width, surface_height);
        const uint32_t crop_w = std::min(panel.w, surface_width - static_cast<uint32_t>(placement.x));
        const uint32_t crop_h = std::min(panel.h, surface_height - static_cast<uint32_t>(placement.y));
        if (crop_w == 0 || crop_h == 0) {
            return out;
        }

        rgba.resize(static_cast<size_t>(crop_w) * crop_h * 4u);
        for (uint32_t y = 0; y < crop_h; ++y) {
            for (uint32_t x = 0; x < crop_w; ++x) {
                const auto& src = panel.p[static_cast<size_t>(y) * panel.w + x];
                const size_t idx = (static_cast<size_t>(y) * crop_w + x) * 4u;
                rgba[idx + 0] = src.r;
                rgba[idx + 1] = src.g;
                rgba[idx + 2] = src.b;
                rgba[idx + 3] = src.a;
            }
        }

        out.placement = placement;
        out.rgba = rgba.data();
        out.width = crop_w;
        out.height = crop_h;
        out.byte_size = rgba.size();
        const bool pixels_changed =
            cached_width != crop_w ||
            cached_height != crop_h ||
            cached_rgba.size() != rgba.size() ||
            cached_rgba.empty() ||
            std::memcmp(cached_rgba.data(), rgba.data(), rgba.size()) != 0;
        if (pixels_changed) {
            cached_rgba = rgba;
            cached_width = crop_w;
            cached_height = crop_h;
            out.sequence = ++frame_sequence;
            out.changed = true;
        } else {
            out.sequence = frame_sequence;
            out.changed = false;
        }
        return out;
    }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {
    impl_->refresh_config(true);
}

Runtime::~Runtime() = default;

Runtime::Runtime(Runtime&&) noexcept = default;

Runtime& Runtime::operator=(Runtime&&) noexcept = default;

PreparedFrame Runtime::prepare(uint32_t surface_width, uint32_t surface_height) {
    return impl_->prepare(surface_width, surface_height);
}

bool Runtime::debug_enabled() const {
    return impl_ && impl_->debug;
}

} /* namespace sigaw::overlay */

static void composite_frame_over(void* dst, VkFormat fmt,
                                 const sigaw::overlay::PreparedFrame& frame)
{
    auto* out = static_cast<uint8_t*>(dst);
    for (uint32_t y = 0; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const size_t idx = (static_cast<size_t>(y) * frame.width + x) * 4u;
            const RGBA src = {
                frame.rgba[idx + 0],
                frame.rgba[idx + 1],
                frame.rgba[idx + 2],
                frame.rgba[idx + 3],
            };
            if (src.a == 0) {
                continue;
            }

            uint8_t* px = out + idx;
            const RGBA dst_px = PBuf::load_pixel(px, fmt);
            PBuf::store_pixel(px, fmt, alpha_blend(dst_px, src));
        }
    }
}

static int render_overlay_copy(Ctx& c, VkQueue queue, VkImage target_image, VkFormat format,
                               const sigaw::overlay::PreparedFrame& frame,
                               int px, int py,
                               uint32_t wait_sem_count, const VkSemaphore* wait_sems,
                               VkSemaphore signal_sem)
{
    const uint32_t cw = frame.width;
    const uint32_t ch = frame.height;
    const size_t bytes = frame.byte_size;

    if (!ensure_staging_capacity(c, bytes) || bytes > static_cast<size_t>(c.ssz)) {
        return 0;
    }

    vkWaitForFences(c.dev, 1, &c.fen, VK_TRUE, UINT64_MAX);
    vkResetFences(c.dev, 1, &c.fen);
    vkResetCommandBuffer(c.cmd, 0);

    VkCommandBufferBeginInfo beg = {};
    beg.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c.cmd, &beg);

    VkImageMemoryBarrier bar = {};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = target_image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    VkBufferImageCopy reg = {};
    reg.bufferRowLength = cw;
    reg.bufferImageHeight = ch;
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageOffset = {px, py, 0};
    reg.imageExtent = {cw, ch, 1};
    vkCmdCopyImageToBuffer(c.cmd, target_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.sbuf, 1, &reg);

    vkEndCommandBuffer(c.cmd);

    VkSubmitInfo sub = {};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &c.cmd;
    VkPipelineStageFlags wait_stages[8] = {};
    if (wait_sem_count > 8) {
        wait_sem_count = 8;
    }
    for (uint32_t i = 0; i < wait_sem_count; ++i) {
        wait_stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (wait_sem_count != 0 && wait_sems != nullptr) {
        sub.waitSemaphoreCount = wait_sem_count;
        sub.pWaitSemaphores = wait_sems;
        sub.pWaitDstStageMask = wait_stages;
    }
    if (vkQueueSubmit(queue, 1, &sub, c.fen) != VK_SUCCESS) {
        return 0;
    }

    vkWaitForFences(c.dev, 1, &c.fen, VK_TRUE, UINT64_MAX);
    composite_frame_over(c.sptr, format, frame);

    vkResetFences(c.dev, 1, &c.fen);
    vkResetCommandBuffer(c.cmd, 0);
    vkBeginCommandBuffer(c.cmd, &beg);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    vkCmdCopyBufferToImage(c.cmd, c.sbuf, target_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    vkEndCommandBuffer(c.cmd);

    sub.waitSemaphoreCount = 0;
    sub.pWaitSemaphores = nullptr;
    sub.pWaitDstStageMask = nullptr;
    if (signal_sem != VK_NULL_HANDLE) {
        sub.signalSemaphoreCount = 1;
        sub.pSignalSemaphores = &signal_sem;
    } else {
        sub.signalSemaphoreCount = 0;
        sub.pSignalSemaphores = nullptr;
    }
    if (vkQueueSubmit(queue, 1, &sub, c.fen) != VK_SUCCESS) {
        return 0;
    }
    if (signal_sem == VK_NULL_HANDLE) {
        vkWaitForFences(c.dev, 1, &c.fen, VK_TRUE, UINT64_MAX);
    }
    return 1;
}

/* ======================================================================== */
/*  C API                                                                    */
/* ======================================================================== */

static VkSemaphore create_present_semaphore(Ctx& ctx)
{
    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(ctx.dev, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    ctx.present_sems.push_back(semaphore);
    return semaphore;
}

static void erase_present_semaphore(std::vector<VkSemaphore>& semaphores,
                                    VkSemaphore semaphore)
{
    semaphores.erase(
        std::remove(semaphores.begin(), semaphores.end(), semaphore),
        semaphores.end()
    );
}

static void destroy_overlay_context(Ctx* ctx, bool wait_idle)
{
    if (!ctx) {
        return;
    }

    if (wait_idle && ctx->dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx->dev);
    }

    destroy_target_views(*ctx);

    if (ctx->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx->dev, ctx->pipeline, nullptr);
        ctx->pipeline = VK_NULL_HANDLE;
    }
    if (ctx->render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(ctx->dev, ctx->render_pass, nullptr);
        ctx->render_pass = VK_NULL_HANDLE;
    }
    if (ctx->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx->dev, ctx->pipeline_layout, nullptr);
        ctx->pipeline_layout = VK_NULL_HANDLE;
    }
    if (ctx->desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(ctx->dev, ctx->desc_pool, nullptr);
        ctx->desc_pool = VK_NULL_HANDLE;
    }
    if (ctx->desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(ctx->dev, ctx->desc_layout, nullptr);
        ctx->desc_layout = VK_NULL_HANDLE;
    }
    if (ctx->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(ctx->dev, ctx->sampler, nullptr);
        ctx->sampler = VK_NULL_HANDLE;
    }
    destroy_overlay_texture(*ctx);
    if (ctx->sptr != nullptr) {
        vkUnmapMemory(ctx->dev, ctx->smem);
        ctx->sptr = nullptr;
    }
    if (ctx->sbuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(ctx->dev, ctx->sbuf, nullptr);
        ctx->sbuf = VK_NULL_HANDLE;
    }
    if (ctx->smem != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->dev, ctx->smem, nullptr);
        ctx->smem = VK_NULL_HANDLE;
    }
    if (ctx->fen != VK_NULL_HANDLE) {
        vkDestroyFence(ctx->dev, ctx->fen, nullptr);
        ctx->fen = VK_NULL_HANDLE;
    }
    for (VkSemaphore semaphore : ctx->present_sems) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(ctx->dev, semaphore, nullptr);
        }
    }
    ctx->available_present_sems.clear();
    ctx->present_sems.clear();
    if (ctx->pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(ctx->dev, ctx->pool, nullptr);
        ctx->pool = VK_NULL_HANDLE;
    }
    ctx->ok = false;
}

SigawOverlayContext* sigaw_overlay_create(VkDevice device, VkPhysicalDevice phys_device,
                                          VkInstance instance, uint32_t queue_family,
                                          VkQueue queue,
                                          const SigawVulkanDispatch* dispatch,
                                          PFN_vkGetPhysicalDeviceMemoryProperties get_phys_props,
                                          VkFormat format,
                                          uint32_t width, uint32_t height)
{
    if (!dispatch || !sigaw_vk_dispatch_complete(dispatch)) {
        return nullptr;
    }

    auto* ctx = new Ctx();
    auto& c = *ctx;
    c.dev = device;
    c.pdev = phys_device;
    c.inst = instance;
    c.qf = queue_family;
    c.q = queue;
    c.dispatch = *dispatch;
    c.get_phys_props = get_phys_props;
    c.fmt = format;
    c.sw = width;
    c.sh = height;
    c.logged_first_render = false;

    ScopedDispatch scoped(&c.dispatch);

    VkCommandPoolCreateInfo pi = {};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = queue_family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device, &pi, nullptr, &c.pool) != VK_SUCCESS) {
        destroy_overlay_context(ctx, false);
        delete ctx;
        return nullptr;
    }

    VkCommandBufferAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = c.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &ai, &c.cmd) != VK_SUCCESS) {
        destroy_overlay_context(ctx, false);
        delete ctx;
        return nullptr;
    }

    VkFenceCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fi, nullptr, &c.fen) != VK_SUCCESS) {
        destroy_overlay_context(ctx, false);
        delete ctx;
        return nullptr;
    }

    if (!ensure_staging_capacity(c, 2 * 1024 * 1024) ||
        (c.prefer_gpu_composite && !ensure_descriptor_resources(c))) {
        destroy_overlay_context(ctx, false);
        delete ctx;
        return nullptr;
    }

    c.ok = true;
    fprintf(stderr, "[sigaw] Overlay initialized (%ux%u fmt=%d)\n", width, height, format);
    return reinterpret_cast<SigawOverlayContext*>(ctx);
}

void sigaw_overlay_destroy(SigawOverlayContext* handle) {
    auto* ctx = reinterpret_cast<Ctx*>(handle);
    if (!ctx) {
        return;
    }

    ScopedDispatch scoped(&ctx->dispatch);
    destroy_overlay_context(ctx, true);
    delete ctx;
    fprintf(stderr, "[sigaw] Overlay shutdown\n");
}

void sigaw_overlay_resize(SigawOverlayContext* handle, VkFormat format,
                          uint32_t width, uint32_t height) {
    auto* ctx = reinterpret_cast<Ctx*>(handle);
    if (!ctx) {
        return;
    }

    ScopedDispatch scoped(&ctx->dispatch);
    if (ctx->fmt != format) {
        ctx->fmt = format;
        destroy_target_views(*ctx);
    }
    ctx->sw = width;
    ctx->sh = height;
}

VkSemaphore sigaw_overlay_acquire_present_semaphore(SigawOverlayContext* handle) {
    auto* ctx = reinterpret_cast<Ctx*>(handle);
    if (!ctx || !ctx->ok) {
        return VK_NULL_HANDLE;
    }

    ScopedDispatch scoped(&ctx->dispatch);
    if (ctx->available_present_sems.empty()) {
        const VkSemaphore semaphore = create_present_semaphore(*ctx);
        if (semaphore == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
        return semaphore;
    }

    const VkSemaphore semaphore = ctx->available_present_sems.back();
    ctx->available_present_sems.pop_back();
    return semaphore;
}

void sigaw_overlay_recycle_present_semaphore(SigawOverlayContext* handle,
                                             VkSemaphore semaphore) {
    auto* ctx = reinterpret_cast<Ctx*>(handle);
    if (!ctx || !ctx->ok || semaphore == VK_NULL_HANDLE) {
        return;
    }

    if (std::find(ctx->present_sems.begin(), ctx->present_sems.end(), semaphore) ==
        ctx->present_sems.end()) {
        return;
    }
    if (std::find(ctx->available_present_sems.begin(),
                  ctx->available_present_sems.end(),
                  semaphore) != ctx->available_present_sems.end()) {
        return;
    }

    ctx->available_present_sems.push_back(semaphore);
}

void sigaw_overlay_discard_present_semaphore(SigawOverlayContext* handle,
                                             VkSemaphore semaphore) {
    auto* ctx = reinterpret_cast<Ctx*>(handle);
    if (!ctx || !ctx->ok || semaphore == VK_NULL_HANDLE) {
        return;
    }

    if (std::find(ctx->present_sems.begin(), ctx->present_sems.end(), semaphore) ==
        ctx->present_sems.end()) {
        return;
    }

    ScopedDispatch scoped(&ctx->dispatch);
    if (ctx->fen != VK_NULL_HANDLE) {
        vkWaitForFences(ctx->dev, 1, &ctx->fen, VK_TRUE, UINT64_MAX);
    }
    erase_present_semaphore(ctx->available_present_sems, semaphore);
    erase_present_semaphore(ctx->present_sems, semaphore);
    vkDestroySemaphore(ctx->dev, semaphore, nullptr);
}

int sigaw_overlay_render(SigawOverlayContext* handle, VkQueue queue,
                         VkImage target_image, VkFormat format,
                         uint32_t width, uint32_t height,
                         uint32_t wait_sem_count,
                         const VkSemaphore* wait_sems,
                         VkSemaphore signal_sem,
                         VkFence /* fence */)
{
    auto* ctx = reinterpret_cast<Ctx*>(handle);
    if (!ctx) {
        return 0;
    }

    ScopedDispatch scoped(&ctx->dispatch);
    auto& c = *ctx;
    if (!c.ok) return 0;

    if (!supports_overlay_format(format)) {
        static VkFormat logged_format = VK_FORMAT_UNDEFINED;
        if (logged_format != format) {
            fprintf(stderr,
                    "[sigaw] Unsupported swapchain format %d; skipping overlay rendering for this process\n",
                    format);
            logged_format = format;
        }
        return 0;
    }

    const auto frame = c.runtime.prepare(width, height);
    if (frame.empty()) return 0;

    const int px = frame.placement.x;
    const int py = frame.placement.y;
    const uint32_t cw = frame.width;
    const uint32_t ch = frame.height;
    const size_t bytes = frame.byte_size;

    if (c.runtime.debug_enabled() && !c.logged_first_render) {
        fprintf(stderr,
                "[sigaw] Debug: first render submit panel=%ux%u fmt=%d target=%ux%u at=%d,%d\n",
                cw, ch, format, width, height, px, py);
        c.logged_first_render = true;
    }

    if (!c.prefer_gpu_composite) {
        if (c.runtime.debug_enabled() && !c.logged_copy_path) {
            fprintf(stderr,
                    "[sigaw] Debug: using safe Vulkan transfer composite path; set SIGAW_GPU_COMPOSITE=1 to opt into the quad path\n");
            c.logged_copy_path = true;
        }
        return render_overlay_copy(
            c, queue, target_image, format, frame, px, py, wait_sem_count, wait_sems, signal_sem
        );
    }

    const bool needs_upload =
        frame.changed ||
        c.uploaded_sequence != frame.sequence ||
        c.overlay_width != cw ||
        c.overlay_height != ch;

    if (needs_upload) {
        if (!ensure_staging_capacity(c, bytes) ||
            !ensure_overlay_texture(c, cw, ch) ||
            !ensure_descriptor_resources(c)) {
            return 0;
        }
        std::memcpy(c.sptr, frame.rgba, bytes);
    }

    if (!ensure_pipeline(c, format)) {
        return 0;
    }
    auto* target = ensure_target_view(c, target_image, format, width, height);
    if (target == nullptr) {
        return 0;
    }

    vkWaitForFences(c.dev, 1, &c.fen, VK_TRUE, UINT64_MAX);
    vkResetFences(c.dev, 1, &c.fen);
    vkResetCommandBuffer(c.cmd, 0);

    VkCommandBufferBeginInfo beg = {};
    beg.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c.cmd, &beg);

    if (needs_upload) {
        VkImageMemoryBarrier texture_barrier = {};
        texture_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        texture_barrier.srcAccessMask = c.overlay_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            ? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT)
            : static_cast<VkAccessFlags>(0u);
        texture_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        texture_barrier.oldLayout = c.overlay_layout;
        texture_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        texture_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        texture_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        texture_barrier.image = c.overlay_image;
        texture_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(
            c.cmd,
            c.overlay_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &texture_barrier
        );

        VkBufferImageCopy upload = {};
        upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        upload.imageExtent = {cw, ch, 1};
        vkCmdCopyBufferToImage(
            c.cmd,
            c.sbuf,
            c.overlay_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &upload
        );

        texture_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        texture_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        texture_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        texture_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(
            c.cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &texture_barrier
        );
        c.overlay_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkImageMemoryBarrier target_barrier = {};
    target_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    target_barrier.srcAccessMask = 0;
    target_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    target_barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    target_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    target_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    target_barrier.image = target_image;
    target_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(
        c.cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &target_barrier
    );

    VkRenderPassBeginInfo rp = {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = c.render_pass;
    rp.framebuffer = target->framebuffer;
    rp.renderArea.extent = {width, height};
    vkCmdBeginRenderPass(c.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {{0, 0}, {width, height}};
    vkCmdSetViewport(c.cmd, 0, 1, &viewport);
    vkCmdSetScissor(c.cmd, 0, 1, &scissor);
    vkCmdBindPipeline(c.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
    vkCmdBindDescriptorSets(
        c.cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        c.pipeline_layout,
        0,
        1,
        &c.desc_set,
        0,
        nullptr
    );

    const OverlayPushConstants push = {
        {static_cast<float>(width), static_cast<float>(height)},
        {static_cast<float>(px), static_cast<float>(py),
         static_cast<float>(cw), static_cast<float>(ch)}
    };
    vkCmdPushConstants(
        c.cmd,
        c.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(push),
        &push
    );
    vkCmdDraw(c.cmd, 4, 1, 0, 0);
    vkCmdEndRenderPass(c.cmd);

    target_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    target_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    target_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(
        c.cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &target_barrier
    );

    vkEndCommandBuffer(c.cmd);

    VkSubmitInfo sub = {};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &c.cmd;
    VkPipelineStageFlags wait_stages[8] = {};
    if (wait_sem_count > 8) wait_sem_count = 8;
    for (uint32_t i = 0; i < wait_sem_count; ++i) {
        wait_stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (wait_sem_count != 0 && wait_sems != nullptr) {
        sub.waitSemaphoreCount = wait_sem_count;
        sub.pWaitSemaphores = wait_sems;
        sub.pWaitDstStageMask = wait_stages;
    }
    if (signal_sem) { sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &signal_sem; }
    else { sub.signalSemaphoreCount = 0; sub.pSignalSemaphores = nullptr; }
    if (vkQueueSubmit(queue, 1, &sub, c.fen) != VK_SUCCESS) {
        return 0;
    }
    c.uploaded_sequence = frame.sequence;
    if (!signal_sem) {
        vkWaitForFences(c.dev, 1, &c.fen, VK_TRUE, UINT64_MAX);
    }
    return 1;
}
