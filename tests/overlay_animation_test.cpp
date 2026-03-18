#include <cmath>
#include <iostream>

#include "layer/overlay_animation.h"

namespace {

bool approx(float actual, float expected, float epsilon = 0.001f) {
    return std::fabs(actual - expected) <= epsilon;
}

} // namespace

int main() {
    using sigaw::overlay::advance_speaking_time;
    using sigaw::overlay::speaking_fade_ms;
    using sigaw::overlay::speaking_ring_alpha;

    {
        float progress = 0.0f;
        for (int i = 0; i < 7; ++i) {
            progress = advance_speaking_time(progress, true, 16.0f);
        }

        if (!approx(progress, speaking_fade_ms) || !approx(speaking_ring_alpha(progress), 1.0f)) {
            std::cerr << "speaking fade-in should reach full opacity after roughly 100ms\n";
            return 1;
        }
    }

    {
        float progress = speaking_fade_ms;
        progress = advance_speaking_time(progress, false, 16.0f);
        if (!approx(progress, speaking_fade_ms - 16.0f)) {
            std::cerr << "speaking fade-out should decrease by the frame delta\n";
            return 1;
        }

        for (int i = 0; i < 6; ++i) {
            progress = advance_speaking_time(progress, false, 16.0f);
        }

        if (!approx(progress, 0.0f) || !approx(speaking_ring_alpha(progress), 0.0f)) {
            std::cerr << "speaking fade-out should clamp to zero opacity\n";
            return 1;
        }
    }

    {
        const float full = advance_speaking_time(90.0f, true, 1000.0f);
        const float empty = advance_speaking_time(10.0f, false, 1000.0f);
        if (!approx(full, speaking_fade_ms) || !approx(empty, 0.0f)) {
            std::cerr << "speaking progress should clamp to the fade window\n";
            return 1;
        }
    }

    {
        const float unchanged = advance_speaking_time(48.0f, true, 0.0f);
        if (!approx(unchanged, 48.0f)) {
            std::cerr << "zero delta should not change speaking progress\n";
            return 1;
        }
    }

    return 0;
}
