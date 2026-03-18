#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "common/config.h"
#include "common/protocol.h"
#include "layer/overlay_animation.h"
#include "layer/overlay_preview.h"

namespace {

using sigaw::preview::Image;

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

struct AvatarSpec {
    uint64_t user_id = 0;
    std::string avatar_hash;
    Color primary;
    Color secondary;
};

struct SceneUser {
    uint64_t user_id = 0;
    std::string username;
    std::string avatar_hash;
    bool speaking = false;
    bool self_mute = false;
    bool self_deaf = false;
    bool server_mute = false;
    bool server_deaf = false;
};

struct SceneChatMessage {
    uint64_t message_id = 0;
    uint64_t author_id = 0;
    uint64_t observed_at_ms = 0;
    std::string author_name;
    std::string content;
};

enum class BackgroundStyle {
    Ember,
    Verdant,
    Dusk,
};

struct Scene {
    std::string detail_filename;
    std::string channel_name;
    sigaw::Config config;
    BackgroundStyle background = BackgroundStyle::Ember;
    std::vector<SceneUser> users;
    std::vector<SceneChatMessage> chat_messages;
    uint64_t preview_now_ms = 10'000;
};

struct Rect {
    int x = 0;
    int y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return {r, g, b, a};
}

Color mix(Color a, Color b, float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    auto lerp = [clamped](uint8_t lhs, uint8_t rhs) {
        return static_cast<uint8_t>(
            std::lround(static_cast<float>(lhs) +
                        (static_cast<float>(rhs) - static_cast<float>(lhs)) * clamped)
        );
    };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

Color alpha_blend(Color dst, Color src) {
    const float sa = static_cast<float>(src.a) / 255.0f;
    const float da = 1.0f - sa;
    return {
        static_cast<uint8_t>(std::lround(src.r * sa + dst.r * da)),
        static_cast<uint8_t>(std::lround(src.g * sa + dst.g * da)),
        static_cast<uint8_t>(std::lround(src.b * sa + dst.b * da)),
        static_cast<uint8_t>(std::lround(std::min(255.0f, src.a + dst.a * da))),
    };
}

void init_image(Image& image, uint32_t width, uint32_t height, Color fill = rgba(0, 0, 0, 255)) {
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<size_t>(width) * height * 4u);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        image.rgba[i * 4 + 0] = fill.r;
        image.rgba[i * 4 + 1] = fill.g;
        image.rgba[i * 4 + 2] = fill.b;
        image.rgba[i * 4 + 3] = fill.a;
    }
}

void put_pixel(Image& image, int x, int y, Color color) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= image.width || static_cast<uint32_t>(y) >= image.height) {
        return;
    }

    const size_t idx = (static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 4u;
    const Color blended = alpha_blend(
        {image.rgba[idx + 0], image.rgba[idx + 1], image.rgba[idx + 2], image.rgba[idx + 3]},
        color
    );
    image.rgba[idx + 0] = blended.r;
    image.rgba[idx + 1] = blended.g;
    image.rgba[idx + 2] = blended.b;
    image.rgba[idx + 3] = blended.a;
}

void fill_gradient(Image& image, Color tl, Color tr, Color bl, Color br) {
    for (uint32_t y = 0; y < image.height; ++y) {
        const float v = image.height > 1 ? static_cast<float>(y) / static_cast<float>(image.height - 1) : 0.0f;
        const Color left = mix(tl, bl, v);
        const Color right = mix(tr, br, v);
        for (uint32_t x = 0; x < image.width; ++x) {
            const float u = image.width > 1 ? static_cast<float>(x) / static_cast<float>(image.width - 1) : 0.0f;
            const Color pixel = mix(left, right, u);
            const size_t idx = (static_cast<size_t>(y) * image.width + x) * 4u;
            image.rgba[idx + 0] = pixel.r;
            image.rgba[idx + 1] = pixel.g;
            image.rgba[idx + 2] = pixel.b;
            image.rgba[idx + 3] = pixel.a;
        }
    }
}

void add_rect(Image& image, int x, int y, int w, int h, Color color) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(static_cast<int>(image.width), x + w);
    const int y1 = std::min(static_cast<int>(image.height), y + h);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            put_pixel(image, xx, yy, color);
        }
    }
}

void add_radial_glow(Image& image, float cx, float cy, float radius, Color color) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int x1 = std::min(static_cast<int>(image.width), static_cast<int>(std::ceil(cx + radius)));
    const int y1 = std::min(static_cast<int>(image.height), static_cast<int>(std::ceil(cy + radius)));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) - cx;
            const float dy = (static_cast<float>(y) + 0.5f) - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= radius) {
                continue;
            }
            const float t = 1.0f - dist / radius;
            Color glow = color;
            glow.a = static_cast<uint8_t>(std::lround(static_cast<float>(color.a) * t * t));
            put_pixel(image, x, y, glow);
        }
    }
}

void add_hill_layer(Image& image, float baseline, float amp_a, float freq_a, float phase_a,
                    float amp_b, float freq_b, float phase_b, Color color) {
    for (uint32_t x = 0; x < image.width; ++x) {
        const float xf = static_cast<float>(x);
        const float top =
            baseline +
            std::sin(xf * freq_a + phase_a) * amp_a +
            std::sin(xf * freq_b + phase_b) * amp_b;
        const int y0 = std::max(0, static_cast<int>(std::floor(top)));
        for (uint32_t y = static_cast<uint32_t>(y0); y < image.height; ++y) {
            put_pixel(image, static_cast<int>(x), static_cast<int>(y), color);
        }
    }
}

void add_panel_bars(Image& image, int x, int y, const std::vector<int>& widths, int bar_h, int gap, Color color) {
    int yy = y;
    for (int width : widths) {
        add_rect(image, x, yy, width, bar_h, color);
        yy += bar_h + gap;
    }
}

void render_background(Image& image, BackgroundStyle style) {
    switch (style) {
        case BackgroundStyle::Ember:
            fill_gradient(
                image,
                rgba(8, 19, 34),
                rgba(19, 52, 80),
                rgba(16, 26, 40),
                rgba(199, 106, 63)
            );
            add_radial_glow(image, image.width * 0.82f, image.height * 0.13f, image.height * 0.17f,
                            rgba(255, 160, 97, 92));
            add_radial_glow(image, image.width * 0.17f, image.height * 0.18f, image.height * 0.16f,
                            rgba(87, 200, 255, 58));
            add_hill_layer(image, image.height * 0.53f, image.height * 0.06f, 0.0042f, 0.0f,
                           image.height * 0.03f, 0.0105f, 1.4f, rgba(14, 33, 49, 210));
            add_hill_layer(image, image.height * 0.67f, image.height * 0.08f, 0.0031f, 0.4f,
                           image.height * 0.04f, 0.0118f, 2.1f, rgba(36, 23, 16, 235));
            add_panel_bars(
                image,
                static_cast<int>(image.width * 0.09f),
                static_cast<int>(image.height * 0.67f),
                {520, 300},
                static_cast<int>(image.height * 0.012f),
                static_cast<int>(image.height * 0.015f),
                rgba(230, 244, 255, 38)
            );
            break;
        case BackgroundStyle::Verdant:
            fill_gradient(
                image,
                rgba(5, 20, 15),
                rgba(14, 69, 53),
                rgba(14, 42, 31),
                rgba(130, 183, 111)
            );
            add_radial_glow(image, image.width * 0.85f, image.height * 0.15f, image.height * 0.16f,
                            rgba(255, 231, 160, 124));
            add_hill_layer(image, image.height * 0.58f, image.height * 0.07f, 0.0037f, 0.3f,
                           image.height * 0.025f, 0.0125f, 0.9f, rgba(11, 31, 23, 180));
            add_hill_layer(image, image.height * 0.73f, image.height * 0.06f, 0.0033f, 1.4f,
                           image.height * 0.03f, 0.0109f, 2.4f, rgba(20, 45, 29, 232));
            add_panel_bars(
                image,
                static_cast<int>(image.width * 0.77f),
                static_cast<int>(image.height * 0.56f),
                {520, 260, 380},
                static_cast<int>(image.height * 0.011f),
                static_cast<int>(image.height * 0.013f),
                rgba(236, 248, 255, 30)
            );
            break;
        case BackgroundStyle::Dusk:
            fill_gradient(
                image,
                rgba(11, 16, 34),
                rgba(44, 56, 95),
                rgba(18, 24, 42),
                rgba(109, 77, 58)
            );
            add_radial_glow(image, image.width * 0.86f, image.height * 0.10f, image.height * 0.14f,
                            rgba(255, 226, 191, 84));
            add_hill_layer(image, image.height * 0.49f, image.height * 0.08f, 0.0034f, 0.1f,
                           image.height * 0.03f, 0.0102f, 1.8f, rgba(17, 24, 45, 178));
            add_hill_layer(image, image.height * 0.66f, image.height * 0.09f, 0.0029f, 1.0f,
                           image.height * 0.035f, 0.0116f, 2.6f, rgba(31, 25, 21, 235));
            add_panel_bars(
                image,
                static_cast<int>(image.width * 0.79f),
                static_cast<int>(image.height * 0.38f),
                {560, 320},
                static_cast<int>(image.height * 0.011f),
                static_cast<int>(image.height * 0.013f),
                rgba(241, 244, 255, 32)
            );
            break;
    }
}

void composite(Image& dst, const Image& src, int x0, int y0) {
    for (uint32_t y = 0; y < src.height; ++y) {
        for (uint32_t x = 0; x < src.width; ++x) {
            const int dx = x0 + static_cast<int>(x);
            const int dy = y0 + static_cast<int>(y);
            if (dx < 0 || dy < 0 || static_cast<uint32_t>(dx) >= dst.width || static_cast<uint32_t>(dy) >= dst.height) {
                continue;
            }
            const size_t src_idx = (static_cast<size_t>(y) * src.width + x) * 4u;
            put_pixel(
                dst,
                dx,
                dy,
                {src.rgba[src_idx + 0], src.rgba[src_idx + 1], src.rgba[src_idx + 2], src.rgba[src_idx + 3]}
            );
        }
    }
}

Rect detail_crop_rect(const sigaw::preview::Placement& placement, uint32_t panel_w, uint32_t panel_h,
                      uint32_t screen_w, uint32_t screen_h,
                      sigaw::OverlayPosition position) {
    const uint32_t near_margin_x = std::max<uint32_t>(28u, panel_w / 12u);
    const uint32_t near_margin_y = std::max<uint32_t>(28u, panel_h / 12u);
    const uint32_t far_margin_x = std::max<uint32_t>(140u, panel_w);
    const uint32_t far_margin_y = std::max<uint32_t>(110u, panel_h / 4u);

    const uint32_t min_size = std::max(panel_w + near_margin_x + far_margin_x,
                                       panel_h + near_margin_y + far_margin_y);
    const uint32_t crop_size = std::min(std::min(screen_w, screen_h), min_size);

    int x = 0;
    int y = 0;
    switch (position) {
        case sigaw::OverlayPosition::TopLeft:
            x = placement.x - static_cast<int>(near_margin_x);
            y = placement.y - static_cast<int>(near_margin_y);
            break;
        case sigaw::OverlayPosition::TopRight:
            x = placement.x + static_cast<int>(panel_w) + static_cast<int>(near_margin_x) -
                static_cast<int>(crop_size);
            y = placement.y - static_cast<int>(near_margin_y);
            break;
        case sigaw::OverlayPosition::BottomLeft:
            x = placement.x - static_cast<int>(near_margin_x);
            y = placement.y + static_cast<int>(panel_h) + static_cast<int>(near_margin_y) -
                static_cast<int>(crop_size);
            break;
        case sigaw::OverlayPosition::BottomRight:
            x = placement.x + static_cast<int>(panel_w) + static_cast<int>(near_margin_x) -
                static_cast<int>(crop_size);
            y = placement.y + static_cast<int>(panel_h) + static_cast<int>(near_margin_y) -
                static_cast<int>(crop_size);
            break;
    }

    x = std::max(0, std::min(x, static_cast<int>(screen_w - crop_size)));
    y = std::max(0, std::min(y, static_cast<int>(screen_h - crop_size)));
    return {x, y, crop_size, crop_size};
}

Image crop_image(const Image& src, const Rect& rect) {
    Image out;
    if (rect.width == 0 || rect.height == 0) {
        return out;
    }

    init_image(out, rect.width, rect.height, rgba(0, 0, 0, 0));
    for (uint32_t y = 0; y < rect.height; ++y) {
        for (uint32_t x = 0; x < rect.width; ++x) {
            const int sx = rect.x + static_cast<int>(x);
            const int sy = rect.y + static_cast<int>(y);
            if (sx < 0 || sy < 0 || static_cast<uint32_t>(sx) >= src.width || static_cast<uint32_t>(sy) >= src.height) {
                continue;
            }

            const size_t src_idx = (static_cast<size_t>(sy) * src.width + static_cast<uint32_t>(sx)) * 4u;
            const size_t dst_idx = (static_cast<size_t>(y) * rect.width + x) * 4u;
            out.rgba[dst_idx + 0] = src.rgba[src_idx + 0];
            out.rgba[dst_idx + 1] = src.rgba[src_idx + 1];
            out.rgba[dst_idx + 2] = src.rgba[src_idx + 2];
            out.rgba[dst_idx + 3] = src.rgba[src_idx + 3];
        }
    }
    return out;
}

void copy_truncated(char* dst, size_t cap, std::string_view src) {
    if (cap == 0) {
        return;
    }
    const size_t n = std::min(cap - 1, src.size());
    std::copy_n(src.data(), n, dst);
    dst[n] = '\0';
}

SigawState build_state(const Scene& scene) {
    SigawState state = {};
    state.header.magic = SIGAW_MAGIC;
    state.header.version = SIGAW_VERSION;
    state.header.sequence = 1;
    copy_truncated(state.header.channel_name, sizeof(state.header.channel_name), scene.channel_name);
    state.header.channel_name_len = static_cast<uint32_t>(
        std::char_traits<char>::length(state.header.channel_name)
    );
    state.user_count = static_cast<uint32_t>(std::min<size_t>(scene.users.size(), SIGAW_MAX_USERS));

    for (uint32_t i = 0; i < state.user_count; ++i) {
        const auto& user = scene.users[i];
        auto& dst = state.users[i];
        dst.user_id = user.user_id;
        copy_truncated(dst.username, sizeof(dst.username), user.username);
        copy_truncated(dst.avatar_hash, sizeof(dst.avatar_hash), user.avatar_hash);
        dst.speaking = user.speaking ? 1 : 0;
        dst.self_mute = user.self_mute ? 1 : 0;
        dst.self_deaf = user.self_deaf ? 1 : 0;
        dst.server_mute = user.server_mute ? 1 : 0;
        dst.server_deaf = user.server_deaf ? 1 : 0;
    }

    state.chat_count = static_cast<uint32_t>(
        std::min<size_t>(scene.chat_messages.size(), SIGAW_MAX_CHAT_MESSAGES)
    );
    for (uint32_t i = 0; i < state.chat_count; ++i) {
        const auto& message = scene.chat_messages[i];
        auto& dst = state.chat_messages[i];
        dst.message_id = message.message_id;
        dst.author_id = message.author_id;
        dst.observed_at_ms = message.observed_at_ms;
        copy_truncated(dst.author_name, sizeof(dst.author_name), message.author_name);
        dst.author_name_len = static_cast<uint32_t>(
            std::char_traits<char>::length(dst.author_name)
        );
        copy_truncated(dst.content, sizeof(dst.content), message.content);
        dst.content_len = static_cast<uint32_t>(
            std::char_traits<char>::length(dst.content)
        );
    }
    return state;
}

bool render_avatar_png(const AvatarSpec& avatar) {
    Image image;
    init_image(image, 96, 96, avatar.primary);
    fill_gradient(
        image,
        mix(avatar.primary, rgba(255, 255, 255), 0.18f),
        avatar.secondary,
        avatar.primary,
        mix(avatar.secondary, rgba(0, 0, 0), 0.15f)
    );
    add_radial_glow(image, 24.0f, 20.0f, 28.0f, rgba(255, 255, 255, 72));
    add_rect(image, 0, 60, 96, 14, rgba(255, 255, 255, 20));
    add_rect(image, 0, 74, 96, 22, rgba(0, 0, 0, 16));

    const auto path = sigaw::Config::avatar_cache_path(avatar.user_id, avatar.avatar_hash);
    if (path.empty()) {
        return false;
    }
    return sigaw::preview::write_png(path, image);
}

std::vector<AvatarSpec> avatar_specs() {
    return {
        {101, "alex", rgba(108, 186, 255), rgba(56, 111, 247)},
        {102, "morgan", rgba(255, 182, 110), rgba(214, 84, 47)},
        {103, "priya", rgba(126, 224, 176), rgba(45, 141, 105)},
        {104, "devon", rgba(255, 220, 124), rgba(198, 127, 31)},
        {105, "sara", rgba(255, 199, 229), rgba(190, 77, 131)},
        {106, "linh", rgba(189, 210, 255), rgba(92, 113, 241)},
        {107, "theo", rgba(255, 203, 159), rgba(214, 99, 59)},
        {108, "miki", rgba(164, 242, 215), rgba(49, 155, 119)},
        {109, "erin", rgba(195, 205, 255), rgba(96, 122, 255)},
        {110, "noah", rgba(255, 220, 146), rgba(212, 123, 62)},
    };
}

std::vector<Scene> build_scenes() {
    std::vector<Scene> scenes;

    Scene standard;
    standard.detail_filename = "overlay-standard-detail.png";
    standard.channel_name = "squad comms";
    standard.background = BackgroundStyle::Ember;
    standard.config.position = sigaw::OverlayPosition::TopRight;
    standard.config.scale = 2.35f;
    standard.config.opacity = 0.82f;
    standard.config.show_avatars = true;
    standard.config.show_channel = true;
    standard.config.compact = false;
    standard.config.max_visible = 4;
    standard.users = {
        {101, "Alex", "alex", true, false, false, false, false},
        {102, "Morgan", "morgan", false, true, true, false, false},
        {103, "Priya", "priya", false, false, false, false, false},
        {104, "Devon", "devon", false, false, false, false, false},
        {105, "Sara", "sara", false, false, false, false, false},
        {110, "Noah", "noah", false, false, false, false, false},
    };
    scenes.push_back(std::move(standard));

    Scene chat;
    chat.detail_filename = "overlay-chat-detail.png";
    chat.channel_name = "squad comms";
    chat.background = BackgroundStyle::Ember;
    chat.config.position = sigaw::OverlayPosition::TopRight;
    chat.config.scale = 2.35f;
    chat.config.opacity = 0.82f;
    chat.config.show_avatars = true;
    chat.config.show_channel = true;
    chat.config.show_voice_channel_chat = true;
    chat.config.compact = false;
    chat.config.max_visible = 4;
    chat.config.max_visible_chat_messages = 2;
    chat.users = {
        {101, "Alex", "alex", true, false, false, false, false},
        {102, "Morgan", "morgan", false, true, true, false, false},
        {103, "Priya", "priya", false, false, false, false, false},
        {104, "Devon", "devon", false, false, false, false, false},
        {105, "Sara", "sara", false, false, false, false, false},
        {110, "Noah", "noah", false, false, false, false, false},
    };
    chat.chat_messages = {
        {401, 102, chat.preview_now_ms, "Morgan", "Breach on the left, I can't type fast enough"},
        {402, 103, chat.preview_now_ms, "Priya", "Copy that, covering the stairs"},
    };
    scenes.push_back(std::move(chat));

    Scene compact;
    compact.detail_filename = "overlay-compact-detail.png";
    compact.channel_name = "party";
    compact.background = BackgroundStyle::Verdant;
    compact.config.position = sigaw::OverlayPosition::BottomLeft;
    compact.config.scale = 2.55f;
    compact.config.opacity = 0.82f;
    compact.config.show_avatars = true;
    compact.config.show_channel = true;
    compact.config.compact = true;
    compact.config.max_visible = 4;
    compact.users = {
        {104, "Devon", "devon", true, false, false, false, false},
        {109, "Erin", "erin", false, true, false, false, false},
        {108, "Miki", "miki", false, false, false, false, false},
        {105, "Sara", "sara", false, false, true, false, false},
    };
    scenes.push_back(std::move(compact));

    Scene overflow;
    overflow.detail_filename = "overlay-overflow-detail.png";
    overflow.channel_name = "raid room";
    overflow.background = BackgroundStyle::Dusk;
    overflow.config.position = sigaw::OverlayPosition::TopLeft;
    overflow.config.scale = 2.2f;
    overflow.config.opacity = 0.80f;
    overflow.config.show_avatars = true;
    overflow.config.show_channel = true;
    overflow.config.compact = false;
    overflow.config.max_visible = 5;
    overflow.users = {
        {106, "Linh", "linh", false, false, false, false, false},
        {107, "Theo", "theo", true, false, false, false, false},
        {108, "Miki", "miki", false, false, false, false, false},
        {104, "Devon", "devon", false, true, false, false, false},
        {105, "Sara", "sara", false, false, false, false, false},
        {101, "Alex", "alex", false, false, false, false, false},
        {102, "Morgan", "morgan", false, false, false, false, false},
        {103, "Priya", "priya", false, false, false, false, false},
    };
    scenes.push_back(std::move(overflow));

    return scenes;
}

bool parse_size(std::string_view text, uint32_t& width, uint32_t& height) {
    const size_t sep = text.find('x');
    if (sep == std::string_view::npos) {
        return false;
    }

    try {
        const int parsed_w = std::stoi(std::string(text.substr(0, sep)));
        const int parsed_h = std::stoi(std::string(text.substr(sep + 1)));
        if (parsed_w <= 0 || parsed_h <= 0) {
            return false;
        }
        width = static_cast<uint32_t>(parsed_w);
        height = static_cast<uint32_t>(parsed_h);
        return true;
    } catch (...) {
        return false;
    }
}

void print_usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " [--output-dir PATH] [--size WIDTHxHEIGHT]\n";
}

} /* anon */

int main(int argc, char** argv) {
    std::filesystem::path output_dir = std::filesystem::current_path() / "assets" / "screenshots";
    uint32_t screen_width = 3840;
    uint32_t screen_height = 2160;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--output-dir") {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            output_dir = argv[++i];
            continue;
        }
        if (arg == "--size") {
            if (i + 1 >= argc || !parse_size(argv[++i], screen_width, screen_height)) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        print_usage(argv[0]);
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        std::cerr << "failed to create output directory: " << output_dir << "\n";
        return 1;
    }

    const auto temp_cache = std::filesystem::temp_directory_path() /
                            ("sigaw-screenshot-cache-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(temp_cache, ec);
    std::filesystem::create_directories(temp_cache, ec);
    if (ec) {
        std::cerr << "failed to create temporary cache directory: " << temp_cache << "\n";
        return 1;
    }

    if (::setenv("XDG_CACHE_HOME", temp_cache.string().c_str(), 1) != 0) {
        std::cerr << "failed to set XDG_CACHE_HOME\n";
        std::filesystem::remove_all(temp_cache, ec);
        return 1;
    }

    bool ok = true;
    for (const auto& avatar : avatar_specs()) {
        if (!render_avatar_png(avatar)) {
            std::cerr << "failed to render avatar cache image for " << avatar.avatar_hash << "\n";
            ok = false;
            break;
        }
    }

    if (ok) {
        for (const auto& scene : build_scenes()) {
            const SigawState state = build_state(scene);
            std::unordered_map<uint64_t, float> speaking_times_ms;
            for (const auto& user : scene.users) {
                if (user.speaking) {
                    speaking_times_ms[user.user_id] = sigaw::overlay::speaking_fade_ms;
                }
            }

            Image panel;
            if (!sigaw::preview::render_panel_rgba(
                    state, scene.config, speaking_times_ms, panel, scene.preview_now_ms
                )) {
                std::cerr << "failed to render overlay panel for " << scene.detail_filename << "\n";
                ok = false;
                break;
            }

            Image screen;
            init_image(screen, screen_width, screen_height);
            render_background(screen, scene.background);
            const auto placement = sigaw::preview::place_panel(
                scene.config, panel.width, panel.height, screen.width, screen.height
            );
            composite(screen, panel, placement.x, placement.y);

            const auto detail = crop_image(
                screen,
                detail_crop_rect(
                    placement, panel.width, panel.height,
                    screen.width, screen.height, scene.config.position
                )
            );
            const auto detail_path = output_dir / scene.detail_filename;
            if (!sigaw::preview::write_png(detail_path, detail)) {
                std::cerr << "failed to write detail screenshot: " << detail_path << "\n";
                ok = false;
                break;
            }
            std::cout << "wrote " << detail_path << "\n";
        }
    }

    std::filesystem::remove_all(temp_cache, ec);
    return ok ? 0 : 1;
}
