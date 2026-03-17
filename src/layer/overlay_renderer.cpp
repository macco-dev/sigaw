#include <vulkan/vulkan.h>
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
#include <string>
#include <string_view>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <png.h>

#include "overlay_layout.h"
#include "overlay_visibility.h"
#include "shm_reader.h"
#include "../common/config.h"
#include "../common/protocol.h"

extern "C" {
struct SigawOverlayContext;
SigawOverlayContext* sigaw_overlay_create(VkDevice, VkPhysicalDevice, VkInstance, uint32_t,
                                          VkQueue, PFN_vkGetPhysicalDeviceMemoryProperties,
                                          VkFormat, uint32_t, uint32_t);
void sigaw_overlay_destroy(SigawOverlayContext*);
void sigaw_overlay_resize(SigawOverlayContext*, VkFormat, uint32_t, uint32_t);
int sigaw_overlay_render(SigawOverlayContext*, VkQueue, VkImage, VkFormat,
                         uint32_t, uint32_t, uint32_t, const VkSemaphore*,
                         VkSemaphore, VkFence);
}

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
        if (face_) {
            FT_Done_Face(face_);
        }
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
        if (lib_) {
            FT_Done_FreeType(lib_);
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
    PFN_vkGetPhysicalDeviceMemoryProperties get_phys_props = nullptr;
    VkFormat         fmt = VK_FORMAT_UNDEFINED;
    uint32_t         sw = 0, sh = 0;

    VkCommandPool    pool = VK_NULL_HANDLE;
    VkCommandBuffer  cmd  = VK_NULL_HANDLE;
    VkFence          fen  = VK_NULL_HANDLE;
    VkBuffer         sbuf = VK_NULL_HANDLE;
    VkDeviceMemory   smem = VK_NULL_HANDLE;
    void*            sptr = nullptr;
    VkDeviceSize     ssz  = 0;

    sigaw::ShmReader shm;
    sigaw::Config    cfg;
    PBuf             panel;
    TextSystem       text;
    AvatarStore      avatars;
    std::chrono::steady_clock::time_point next_cfg_refresh = {};
    std::filesystem::file_time_type       cfg_mtime = {};
    bool                                  cfg_has_mtime = false;
    bool             debug = false;
    bool             logged_first_render = false;
    bool             ok = false;
};

static bool supports_staging_copy_format(VkFormat fmt)
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

/* ---- Panel rendering ---- */

static void refresh_config(Ctx& ctx, bool force = false)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < ctx.next_cfg_refresh) {
        return;
    }

    ctx.next_cfg_refresh = now + std::chrono::milliseconds(250);

    const auto path = sigaw::Config::config_path();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return;
    }

    if (!exists) {
        ctx.cfg = sigaw::Config::load();
        ctx.cfg_has_mtime = false;
        return;
    }

    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return;
    }

    if (force || !ctx.cfg_has_mtime || mtime != ctx.cfg_mtime) {
        ctx.cfg = sigaw::Config::load();
        ctx.cfg_mtime = mtime;
        ctx.cfg_has_mtime = true;
    }
}

static void build_panel(const SigawState& vs, const sigaw::Config& cfg,
                        PBuf& pb, TextSystem& text, AvatarStore& avatars)
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

    const int header_h = show_header ? (m.header_height + m.header_gap) : 0;
    const int row_count = static_cast<int>(visible);
    const int rows_h = row_count * m.row_height + std::max(0, row_count - 1) * m.row_gap;
    const int more_h = more_label.empty() ? 0 : (m.row_gap + m.row_height);
    const int panel_content_w = pill_column_w + avatar_column_w;
    const uint32_t panel_w = static_cast<uint32_t>(m.outer_pad * 2 + std::max(panel_content_w, m.row_height));
    const uint32_t panel_h = static_cast<uint32_t>(m.outer_pad * 2 + header_h + rows_h + more_h);
    pb.init(panel_w, panel_h);

    const RGBA bubble_bg = apply_opacity(mk(24, 24, 27, 246), cfg.opacity);
    const RGBA bubble_spk = apply_opacity(mk(32, 33, 37, 250), std::clamp(cfg.opacity + 0.04f, 0.0f, 1.0f));
    const RGBA bubble_muted = apply_opacity(mk(26, 26, 30, 248), cfg.opacity);
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
        if (u.speaking) {
            pb.ring(row.avatar_cx, row.avatar_cy, m.avatar_radius + std::max(2, m.avatar_stroke + 1),
                    std::max(2, m.avatar_stroke + 1), speaking);
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
        const int header_x = m.outer_pad + pill_column_w - header_w;
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
        const int row_x = m.outer_pad + pill_column_w - pill_w;
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
        const int row_x = m.outer_pad + pill_column_w - more_w;
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
}

} /* anon */

/* ======================================================================== */
/*  C API                                                                    */
/* ======================================================================== */

static void destroy_overlay_context(Ctx* ctx, bool wait_idle)
{
    if (!ctx) {
        return;
    }

    if (wait_idle && ctx->dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx->dev);
    }
    if (ctx->sptr) {
        vkUnmapMemory(ctx->dev, ctx->smem);
        ctx->sptr = nullptr;
    }
    if (ctx->sbuf) {
        vkDestroyBuffer(ctx->dev, ctx->sbuf, nullptr);
        ctx->sbuf = VK_NULL_HANDLE;
    }
    if (ctx->smem) {
        vkFreeMemory(ctx->dev, ctx->smem, nullptr);
        ctx->smem = VK_NULL_HANDLE;
    }
    if (ctx->fen) {
        vkDestroyFence(ctx->dev, ctx->fen, nullptr);
        ctx->fen = VK_NULL_HANDLE;
    }
    if (ctx->pool) {
        vkDestroyCommandPool(ctx->dev, ctx->pool, nullptr);
        ctx->pool = VK_NULL_HANDLE;
    }
    ctx->shm.close();
    ctx->ok = false;
}

SigawOverlayContext* sigaw_overlay_create(VkDevice device, VkPhysicalDevice phys_device,
                                          VkInstance instance, uint32_t queue_family,
                                          VkQueue queue,
                                          PFN_vkGetPhysicalDeviceMemoryProperties get_phys_props,
                                          VkFormat format,
                                          uint32_t width, uint32_t height)
{
    auto* ctx = new Ctx();
    auto& c = *ctx;
    c.dev = device;
    c.pdev = phys_device;
    c.inst = instance;
    c.qf = queue_family;
    c.q = queue;
    c.get_phys_props = get_phys_props;
    c.fmt = format;
    c.sw = width;
    c.sh = height;
    c.debug = std::getenv("SIGAW_DEBUG") != nullptr;
    c.logged_first_render = false;
    refresh_config(c, true);

    VkCommandPoolCreateInfo pi = {};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = queue_family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device, &pi, nullptr, &c.pool);

    VkCommandBufferAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = c.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &ai, &c.cmd);

    VkFenceCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &fi, nullptr, &c.fen);

    c.ssz = 2 * 1024 * 1024; /* 2MB staging */
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = c.ssz;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bi, nullptr, &c.sbuf);

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, c.sbuf, &mr);
    VkMemoryAllocateInfo ma = {};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = find_mem(c, phys_device, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX) {
        fprintf(stderr, "[sigaw] Failed to find host-visible memory type for overlay staging buffer\n");
        destroy_overlay_context(ctx, false);
        delete ctx;
        return nullptr;
    }
    vkAllocateMemory(device, &ma, nullptr, &c.smem);
    vkBindBufferMemory(device, c.sbuf, c.smem, 0);
    vkMapMemory(device, c.smem, 0, c.ssz, 0, &c.sptr);

    c.ok = true;
    fprintf(stderr, "[sigaw] Overlay initialized (%ux%u fmt=%d)\n", width, height, format);
    return reinterpret_cast<SigawOverlayContext*>(ctx);
}

void sigaw_overlay_destroy(SigawOverlayContext* handle) {
    auto* ctx = reinterpret_cast<Ctx*>(handle);
    if (!ctx) {
        return;
    }

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

    ctx->fmt = format;
    ctx->sw = width;
    ctx->sh = height;
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

    auto& c = *ctx;
    refresh_config(c);
    if (!c.ok || !c.cfg.visible) return 0;

    if (!supports_staging_copy_format(format)) {
        static VkFormat logged_format = VK_FORMAT_UNDEFINED;
        if (logged_format != format) {
            fprintf(stderr,
                    "[sigaw] Unsupported swapchain format %d; skipping overlay rendering for this process\n",
                    format);
            logged_format = format;
        }
        return 0;
    }

    SigawState vs = {};
    const bool have_voice_state = c.shm.read(vs);
    if (c.debug && !have_voice_state) {
        fprintf(stderr, "[sigaw] Debug: shared memory read unavailable, rendering fallback panel\n");
    }

    build_panel(vs, c.cfg, c.panel, c.text, c.avatars);
    if (c.panel.p.empty()) return 0;

    int margin = sigaw::overlay::scaled_px(c.cfg.scale, 16);
    int px = 0, py = 0;
    switch (c.cfg.position) {
        case sigaw::OverlayPosition::TopLeft:     px = margin; py = margin; break;
        case sigaw::OverlayPosition::TopRight:    px = (int)width-(int)c.panel.w-margin; py = margin; break;
        case sigaw::OverlayPosition::BottomLeft:  px = margin; py = (int)height-(int)c.panel.h-margin; break;
        case sigaw::OverlayPosition::BottomRight: px = (int)width-(int)c.panel.w-margin;
                                                   py = (int)height-(int)c.panel.h-margin; break;
    }
    px = std::max(0, std::min(px, (int)width-(int)c.panel.w));
    py = std::max(0, std::min(py, (int)height-(int)c.panel.h));
    uint32_t cw = std::min(c.panel.w, width-(uint32_t)px);
    uint32_t ch = std::min(c.panel.h, height-(uint32_t)py);
    if (!cw || !ch) return 0;
    const size_t bytes = (size_t)cw * ch * 4;
    if (bytes > (size_t)c.ssz) return 0;

    if (c.debug && !c.logged_first_render) {
        fprintf(stderr,
                "[sigaw] Debug: first render submit panel=%ux%u users=%u pos=%d fmt=%d target=%ux%u\n",
                c.panel.w, c.panel.h, vs.user_count, (int)c.cfg.position, format, width, height);
        c.logged_first_render = true;
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
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    VkBufferImageCopy reg = {};
    reg.bufferRowLength = cw; reg.bufferImageHeight = ch;
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageOffset = {px, py, 0};
    reg.imageExtent = {cw, ch, 1};
    vkCmdCopyImageToBuffer(c.cmd, target_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.sbuf, 1, &reg);

    vkEndCommandBuffer(c.cmd);

    VkSubmitInfo sub = {};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1; sub.pCommandBuffers = &c.cmd;
    VkPipelineStageFlags wait_stages[8] = {};
    if (wait_sem_count > 8) wait_sem_count = 8;
    for (uint32_t i = 0; i < wait_sem_count; ++i) {
        wait_stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (wait_sem_count && wait_sems) {
        sub.waitSemaphoreCount = wait_sem_count;
        sub.pWaitSemaphores = wait_sems;
        sub.pWaitDstStageMask = wait_stages;
    }
    if (vkQueueSubmit(queue, 1, &sub, c.fen) != VK_SUCCESS) {
        return 0;
    }

    vkWaitForFences(c.dev, 1, &c.fen, VK_TRUE, UINT64_MAX);
    c.panel.composite_over(c.sptr, format, cw, ch);

    vkResetFences(c.dev, 1, &c.fen);
    vkResetCommandBuffer(c.cmd, 0);

    vkBeginCommandBuffer(c.cmd, &beg);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    vkCmdCopyBufferToImage(c.cmd, c.sbuf, target_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    vkEndCommandBuffer(c.cmd);

    sub.waitSemaphoreCount = 0;
    sub.pWaitSemaphores = nullptr;
    sub.pWaitDstStageMask = nullptr;
    if (signal_sem) { sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &signal_sem; }
    else { sub.signalSemaphoreCount = 0; sub.pSignalSemaphores = nullptr; }
    if (vkQueueSubmit(queue, 1, &sub, c.fen) != VK_SUCCESS) {
        return 0;
    }
    if (!signal_sem) {
        vkWaitForFences(c.dev, 1, &c.fen, VK_TRUE, UINT64_MAX);
    }
    return 1;
}
