#include <iostream>

#include "layer/overlay_layout.h"

int main() {
    const auto m = sigaw::overlay::metrics(1.0f);
    const auto row = sigaw::overlay::layout_row(12, 20, 180, m);
    const auto compact = sigaw::overlay::layout_compact_row(12, 20, 180, m);

    if (row.avatar_cx - m.avatar_radius != row.pill_x + row.pill_w + m.avatar_gap) {
        std::cerr << "avatar should sit after the configured gap\n";
        return 1;
    }

    if (row.total_w != 180 + m.avatar_gap + m.avatar_size + m.avatar_visual_bleed) {
        std::cerr << "separated row total width mismatch\n";
        return 1;
    }

    const int text_width = 120;
    const int icon_count = 2;
    const int pill_w = sigaw::overlay::pill_width_for_text(text_width, m, icon_count);
    if (pill_w <= text_width + m.pill_pad_x + m.pill_pad_right) {
        std::cerr << "pill width should reserve inline status icon space\n";
        return 1;
    }

    const int expected_status = sigaw::overlay::status_cluster_width(icon_count, m);
    if (pill_w != text_width + m.pill_pad_x + m.pill_pad_right + expected_status) {
        std::cerr << "pill width should include status cluster width\n";
        return 1;
    }

    if (sigaw::overlay::text_max_width_for_pill(pill_w, m, icon_count) != text_width) {
        std::cerr << "pill text width helper should round-trip measured text\n";
        return 1;
    }

    if (compact.avatar_cx >= compact.pill_x + compact.pill_w + m.avatar_radius) {
        std::cerr << "compact avatar should remain attached to the pill\n";
        return 1;
    }

    if (compact.avatar_cx - m.avatar_radius >= compact.pill_x + compact.pill_w) {
        std::cerr << "compact avatar should still overlap the pill\n";
        return 1;
    }

    if (compact.total_w != 180 + (m.avatar_size - m.avatar_overlap) + m.avatar_visual_bleed) {
        std::cerr << "compact row total width should include visual bleed\n";
        return 1;
    }

    const auto scaled = sigaw::overlay::metrics(1.5f);
    if (scaled.avatar_size <= m.avatar_size || scaled.row_height <= m.row_height ||
        scaled.avatar_gap <= m.avatar_gap) {
        std::cerr << "scaled metrics should grow with scale\n";
        return 1;
    }

    return 0;
}
