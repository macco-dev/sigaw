#include <cstring>
#include <cstdint>
#include <iostream>
#include <unordered_map>

#include "common/config.h"
#include "layer/overlay_preview.h"

namespace {

SigawState sample_state() {
    SigawState state = {};
    state.header.magic = SIGAW_MAGIC;
    state.header.version = SIGAW_VERSION;
    state.user_count = 1;
    state.users[0].user_id = 7;
    std::strncpy(state.users[0].username, "Alpha", sizeof(state.users[0].username) - 1);
    return state;
}

void add_chat_message(SigawState& state,
                      uint64_t id,
                      uint64_t observed_at_ms,
                      const char* author,
                      const char* content)
{
    if (state.chat_count >= SIGAW_MAX_CHAT_MESSAGES) {
        return;
    }

    auto& message = state.chat_messages[state.chat_count++];
    message.message_id = id;
    message.author_id = id + 1000;
    message.observed_at_ms = observed_at_ms;
    std::strncpy(message.author_name, author, sizeof(message.author_name) - 1);
    message.author_name_len = std::strlen(message.author_name);
    std::strncpy(message.content, content, sizeof(message.content) - 1);
    message.content_len = std::strlen(message.content);
}

uint64_t alpha_sum(const sigaw::preview::Image& image) {
    uint64_t sum = 0;
    for (size_t i = 3; i < image.rgba.size(); i += 4) {
        sum += image.rgba[i];
    }
    return sum;
}

bool test_hidden_overlay_returns_empty_image() {
    sigaw::Config cfg;
    cfg.visible = false;

    sigaw::preview::Image image;
    if (sigaw::preview::render_panel_rgba(sample_state(), cfg, {}, image)) {
        std::cerr << "hidden overlay should not render preview pixels\n";
        return false;
    }
    if (image.width != 0 || image.height != 0 || !image.rgba.empty()) {
        std::cerr << "hidden overlay should leave the preview image empty\n";
        return false;
    }
    return true;
}

bool test_empty_voice_state_returns_empty_image() {
    sigaw::Config cfg;
    cfg.visible = true;

    sigaw::preview::Image image;
    if (sigaw::preview::render_panel_rgba(SigawState{}, cfg, {}, image)) {
        std::cerr << "empty voice state should not render preview pixels\n";
        return false;
    }
    return true;
}

bool test_panel_placement_clamps_to_small_surface() {
    sigaw::Config cfg;
    cfg.visible = true;
    cfg.show_avatars = false;

    sigaw::preview::Image image;
    if (!sigaw::preview::render_panel_rgba(sample_state(), cfg, {}, image)) {
        std::cerr << "expected a rendered preview panel for placement test\n";
        return false;
    }

    const auto placement = sigaw::preview::place_panel(cfg, image.width, image.height, 8, 8);
    if (placement.x != 0 || placement.y != 0) {
        std::cerr << "panel placement should clamp to the top-left corner on tiny surfaces\n";
        return false;
    }
    return true;
}

bool test_render_and_placement_are_stable() {
    sigaw::Config cfg;
    cfg.visible = true;
    cfg.show_avatars = false;

    sigaw::preview::Image first;
    sigaw::preview::Image second;
    if (!sigaw::preview::render_panel_rgba(sample_state(), cfg, {}, first) ||
        !sigaw::preview::render_panel_rgba(sample_state(), cfg, {}, second)) {
        std::cerr << "expected repeated preview rendering to succeed\n";
        return false;
    }

    const auto first_place = sigaw::preview::place_panel(cfg, first.width, first.height, 1920, 1080);
    const auto second_place = sigaw::preview::place_panel(cfg, second.width, second.height, 1920, 1080);

    if (first.width != second.width || first.height != second.height) {
        std::cerr << "repeated preview rendering should keep the same panel size\n";
        return false;
    }
    if (first_place.x != second_place.x || first_place.y != second_place.y) {
        std::cerr << "repeated preview placement should keep the same origin\n";
        return false;
    }
    return true;
}

bool test_chat_rows_increase_panel_height() {
    sigaw::Config cfg;
    cfg.visible = true;
    cfg.show_avatars = false;
    cfg.show_voice_channel_chat = true;
    cfg.max_visible_chat_messages = 2;

    const uint64_t now_ms = 10'000;
    const auto base = sample_state();
    auto with_chat = base;
    add_chat_message(with_chat, 11, now_ms, "Bravo", "I can't type right now");
    add_chat_message(with_chat, 12, now_ms, "Charlie", "Need a quick revive");

    sigaw::preview::Image base_image;
    sigaw::preview::Image chat_image;
    if (!sigaw::preview::render_panel_rgba(base, cfg, {}, base_image, now_ms) ||
        !sigaw::preview::render_panel_rgba(with_chat, cfg, {}, chat_image, now_ms)) {
        std::cerr << "expected preview rendering with chat rows to succeed\n";
        return false;
    }

    if (chat_image.height <= base_image.height) {
        std::cerr << "chat rows should increase panel height\n";
        return false;
    }

    return true;
}

bool test_chat_rows_render_in_compact_mode() {
    sigaw::Config cfg;
    cfg.visible = true;
    cfg.compact = true;
    cfg.show_avatars = false;
    cfg.show_voice_channel_chat = true;

    auto state = sample_state();
    add_chat_message(state, 21, 20'000, "Delta", "Holding the point");

    sigaw::preview::Image image;
    if (!sigaw::preview::render_panel_rgba(state, cfg, {}, image, 20'000)) {
        std::cerr << "expected compact preview rendering with chat rows to succeed\n";
        return false;
    }

    if (image.width == 0 || image.height == 0) {
        std::cerr << "compact chat render should produce non-empty output\n";
        return false;
    }

    return true;
}

bool test_chat_rows_expand_panel_width_for_wrap() {
    sigaw::Config cfg;
    cfg.visible = true;
    cfg.show_avatars = false;
    cfg.show_voice_channel_chat = true;
    cfg.max_visible_chat_messages = 2;

    const uint64_t now_ms = 40'000;
    const auto base = sample_state();
    auto with_chat = base;
    add_chat_message(with_chat, 41, now_ms, "Foxtrot", "This should stay aligned with the roster");
    add_chat_message(with_chat, 42, now_ms, "Golf", "Even when the message is much longer than the name");

    sigaw::preview::Image base_image;
    sigaw::preview::Image chat_image;
    if (!sigaw::preview::render_panel_rgba(base, cfg, {}, base_image, now_ms) ||
        !sigaw::preview::render_panel_rgba(with_chat, cfg, {}, chat_image, now_ms)) {
        std::cerr << "expected preview rendering with constrained chat width to succeed\n";
        return false;
    }

    if (chat_image.width <= base_image.width) {
        std::cerr << "chat rows should be allowed more width than the roster pills\n";
        return false;
    }

    return true;
}

bool test_chat_rows_fade_and_expire() {
    sigaw::Config cfg;
    cfg.visible = true;
    cfg.show_avatars = false;
    cfg.show_voice_channel_chat = true;

    const uint64_t now_ms = 30'000;
    const auto base = sample_state();
    auto state = base;
    add_chat_message(state, 31, now_ms, "Echo", "Typing through voice chat");

    sigaw::preview::Image initial;
    sigaw::preview::Image faded;
    sigaw::preview::Image expired;
    sigaw::preview::Image base_image;

    if (!sigaw::preview::render_panel_rgba(state, cfg, {}, initial, now_ms) ||
        !sigaw::preview::render_panel_rgba(state, cfg, {}, faded, now_ms + 12'000) ||
        !sigaw::preview::render_panel_rgba(state, cfg, {}, expired, now_ms + 15'000) ||
        !sigaw::preview::render_panel_rgba(base, cfg, {}, base_image, now_ms + 15'000)) {
        std::cerr << "expected deterministic chat fade preview rendering to succeed\n";
        return false;
    }

    if (initial.height <= base_image.height) {
        std::cerr << "initial chat render should be taller than the base panel\n";
        return false;
    }
    if (faded.height != initial.height) {
        std::cerr << "faded chat render should keep the chat row present\n";
        return false;
    }
    if (expired.height != base_image.height) {
        std::cerr << "expired chat render should collapse back to the base panel height\n";
        return false;
    }
    if (!(alpha_sum(initial) > alpha_sum(faded) && alpha_sum(faded) > alpha_sum(expired))) {
        std::cerr << "chat fade should reduce total alpha before expiring entirely\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_hidden_overlay_returns_empty_image()) {
        return 1;
    }
    if (!test_empty_voice_state_returns_empty_image()) {
        return 1;
    }
    if (!test_panel_placement_clamps_to_small_surface()) {
        return 1;
    }
    if (!test_render_and_placement_are_stable()) {
        return 1;
    }
    if (!test_chat_rows_increase_panel_height()) {
        return 1;
    }
    if (!test_chat_rows_render_in_compact_mode()) {
        return 1;
    }
    if (!test_chat_rows_expand_panel_width_for_wrap()) {
        return 1;
    }
    if (!test_chat_rows_fade_and_expire()) {
        return 1;
    }
    return 0;
}
