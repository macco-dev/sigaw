#include <cstring>
#include <iostream>
#include <unordered_map>

#include "common/config.h"
#include "layer/overlay_preview.h"

namespace {

SigawState sample_state() {
    SigawState state = {};
    state.user_count = 1;
    state.users[0].user_id = 7;
    std::strncpy(state.users[0].username, "Alpha", sizeof(state.users[0].username) - 1);
    return state;
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
    return 0;
}
