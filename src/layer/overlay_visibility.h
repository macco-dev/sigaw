#ifndef SIGAW_OVERLAY_VISIBILITY_H
#define SIGAW_OVERLAY_VISIBILITY_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../common/protocol.h"

namespace sigaw::overlay {

inline std::vector<uint32_t> select_visible_user_indices(const SigawState& state,
                                                         uint32_t max_visible)
{
    const uint32_t visible_count = std::min(max_visible, state.user_count);
    std::vector<uint32_t> indices;
    indices.reserve(visible_count);

    for (uint32_t i = 0; i < visible_count; ++i) {
        indices.push_back(i);
    }

    if (visible_count == 0 || visible_count >= state.user_count) {
        return indices;
    }

    for (uint32_t i = visible_count; i < state.user_count; ++i) {
        if (!state.users[i].speaking) {
            continue;
        }

        bool already_visible = false;
        for (const auto selected : indices) {
            if (selected == i) {
                already_visible = true;
                break;
            }
        }
        if (already_visible) {
            continue;
        }

        for (size_t slot = indices.size(); slot-- > 0; ) {
            if (state.users[indices[slot]].speaking) {
                continue;
            }

            indices[slot] = i;
            break;
        }
    }

    std::sort(indices.begin(), indices.end());
    return indices;
}

} /* namespace sigaw::overlay */

#endif /* SIGAW_OVERLAY_VISIBILITY_H */
