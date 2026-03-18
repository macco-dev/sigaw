#include <cstdlib>
#include <iostream>
#include <string>

#include "common/config.h"
#include "common/overlay_frame_shm.h"

namespace {

struct FrameEnvGuard {
    explicit FrameEnvGuard(std::string value) : value_(std::move(value)) {
        setenv("SIGAW_FRAME_SHM_NAME", value_.c_str(), 1);
    }

    ~FrameEnvGuard() {
        unsetenv("SIGAW_FRAME_SHM_NAME");
    }

    std::string value_;
};

bool test_overlay_anchor_round_trip() {
    const sigaw::OverlayPosition positions[] = {
        sigaw::OverlayPosition::TopLeft,
        sigaw::OverlayPosition::TopRight,
        sigaw::OverlayPosition::BottomLeft,
        sigaw::OverlayPosition::BottomRight,
    };

    for (const auto position : positions) {
        if (sigaw::overlay_position(sigaw::overlay_anchor(position)) != position) {
            std::cerr << "overlay anchor round trip failed\n";
            return false;
        }
    }
    return true;
}

bool test_overlay_frame_name_override() {
    FrameEnvGuard guard("/sigaw-frame-test");
    if (sigaw::Config::overlay_frame_memory_name() != guard.value_) {
        std::cerr << "SIGAW_FRAME_SHM_NAME override was not honored\n";
        return false;
    }
    return true;
}

bool test_hidden_snapshot_is_empty() {
    sigaw::OverlayFrameSnapshot snapshot;
    snapshot.position = sigaw::OverlayPosition::BottomLeft;
    snapshot.margin_px = 24;
    snapshot.width = 10;
    snapshot.height = 10;
    snapshot.stride = 40;

    if (!snapshot.empty()) {
        std::cerr << "hidden snapshot should report empty\n";
        return false;
    }

    snapshot.visible = true;
    snapshot.rgba.assign(10u * 10u * 4u, 255u);
    if (snapshot.empty()) {
        std::cerr << "visible snapshot with pixels should not report empty\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_overlay_anchor_round_trip()) {
        return 1;
    }
    if (!test_overlay_frame_name_override()) {
        return 1;
    }
    if (!test_hidden_snapshot_is_empty()) {
        return 1;
    }
    return 0;
}
