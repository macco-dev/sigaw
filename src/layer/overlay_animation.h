#ifndef SIGAW_OVERLAY_ANIMATION_H
#define SIGAW_OVERLAY_ANIMATION_H

#include <algorithm>

namespace sigaw::overlay {

inline constexpr float speaking_fade_ms = 100.0f;

inline float advance_speaking_time(float current_ms, bool speaking, float dt_ms) {
    const float clamped_current = std::clamp(current_ms, 0.0f, speaking_fade_ms);
    const float clamped_dt = std::max(0.0f, dt_ms);
    const float next = speaking ? (clamped_current + clamped_dt) : (clamped_current - clamped_dt);
    return std::clamp(next, 0.0f, speaking_fade_ms);
}

inline float speaking_ring_alpha(float current_ms) {
    if (speaking_fade_ms <= 0.0f) {
        return current_ms > 0.0f ? 1.0f : 0.0f;
    }
    return std::clamp(current_ms / speaking_fade_ms, 0.0f, 1.0f);
}

} /* namespace sigaw::overlay */

#endif /* SIGAW_OVERLAY_ANIMATION_H */
