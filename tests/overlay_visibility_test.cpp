#include <iostream>
#include <vector>

#include "layer/overlay_visibility.h"

namespace {

SigawState make_state(std::initializer_list<int> speaking_indices, uint32_t user_count) {
    SigawState state = {};
    state.user_count = user_count;
    uint32_t index = 0;
    for (; index < user_count; ++index) {
        state.users[index].user_id = index + 1;
    }

    for (const int speaking_index : speaking_indices) {
        if (speaking_index >= 0 && static_cast<uint32_t>(speaking_index) < user_count) {
            state.users[speaking_index].speaking = 1;
        }
    }

    return state;
}

bool same_indices(const std::vector<uint32_t>& actual,
                  std::initializer_list<uint32_t> expected)
{
    return actual == std::vector<uint32_t>(expected);
}

} // namespace

int main() {
    {
        const auto state = make_state({1}, 5);
        const auto indices = sigaw::overlay::select_visible_user_indices(state, 4);
        if (!same_indices(indices, {0, 1, 2, 3})) {
            std::cerr << "visible window should stay unchanged when no hidden speaker exists\n";
            return 1;
        }
    }

    {
        const auto state = make_state({5}, 8);
        const auto indices = sigaw::overlay::select_visible_user_indices(state, 4);
        if (!same_indices(indices, {0, 1, 2, 5})) {
            std::cerr << "hidden speaker should replace the last visible non-speaker\n";
            return 1;
        }
    }

    {
        const auto state = make_state({4, 6}, 8);
        const auto indices = sigaw::overlay::select_visible_user_indices(state, 4);
        if (!same_indices(indices, {0, 1, 4, 6})) {
            std::cerr << "multiple hidden speakers should each displace visible non-speakers\n";
            return 1;
        }
    }

    {
        const auto state = make_state({0, 1, 2, 3, 5}, 7);
        const auto indices = sigaw::overlay::select_visible_user_indices(state, 4);
        if (!same_indices(indices, {0, 1, 2, 3})) {
            std::cerr << "existing visible speakers should keep their slots when window is full of speakers\n";
            return 1;
        }
    }

    return 0;
}
