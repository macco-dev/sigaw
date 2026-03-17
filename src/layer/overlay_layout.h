#ifndef SIGAW_OVERLAY_LAYOUT_H
#define SIGAW_OVERLAY_LAYOUT_H

#include <algorithm>
#include <cmath>

namespace sigaw::overlay {

inline int scaled_px(float scale, int base) {
    return std::max(1, static_cast<int>(std::lround(base * std::max(scale, 0.75f))));
}

struct Metrics {
    int outer_pad = 0;
    int row_height = 0;
    int row_gap = 0;
    int pill_pad_x = 0;
    int pill_pad_right = 0;
    int pill_radius = 0;
    int avatar_size = 0;
    int avatar_radius = 0;
    int avatar_overlap = 0;
    int avatar_gap = 0;
    int avatar_stroke = 0;
    int avatar_visual_bleed = 0;
    int badge_radius = 0;
    int status_icon_size = 0;
    int status_icon_gap = 0;
    int status_cluster_pad = 0;
    int shadow_offset = 0;
    int header_height = 0;
    int header_gap = 0;
    int header_radius = 0;
    int header_pad_x = 0;
    int max_text_width = 0;
    int name_font_px = 0;
    int header_font_px = 0;
    int monogram_font_px = 0;
};

inline Metrics metrics(float scale) {
    Metrics m;
    m.outer_pad = scaled_px(scale, 6);
    m.row_height = scaled_px(scale, 34);
    m.row_gap = scaled_px(scale, 6);
    m.pill_pad_x = scaled_px(scale, 12);
    m.pill_pad_right = m.pill_pad_x;
    m.pill_radius = scaled_px(scale, 8);
    m.avatar_size = scaled_px(scale, 32);
    m.avatar_radius = m.avatar_size / 2;
    m.avatar_overlap = m.avatar_size / 2 + scaled_px(scale, 2);  /* ~half overlaps the pill */
    m.avatar_gap = scaled_px(scale, 4);
    m.avatar_stroke = std::max(1, scaled_px(scale, 2));
    m.avatar_visual_bleed = std::max(3, m.avatar_stroke + 2);
    m.badge_radius = scaled_px(scale, 6);
    m.status_icon_size = scaled_px(scale, 12);
    m.status_icon_gap = scaled_px(scale, 5);
    m.status_cluster_pad = scaled_px(scale, 8);
    m.shadow_offset = 0;
    m.header_height = scaled_px(scale, 22);
    m.header_gap = scaled_px(scale, 6);
    m.header_radius = m.header_height / 2;    /* Fully rounded header too */
    m.header_pad_x = scaled_px(scale, 10);
    m.max_text_width = scaled_px(scale, 200);
    m.name_font_px = scaled_px(scale, 13);
    m.header_font_px = scaled_px(scale, 11);
    m.monogram_font_px = scaled_px(scale, 13);
    return m;
}

struct RowLayout {
    int pill_x = 0;
    int pill_y = 0;
    int pill_w = 0;
    int pill_h = 0;
    int avatar_cx = 0;
    int avatar_cy = 0;
    int badge_cx = 0;
    int badge_cy = 0;
    int total_w = 0;
};

inline RowLayout layout_row(int x, int y, int pill_w, const Metrics& m) {
    RowLayout row;
    row.pill_x = x;
    row.pill_y = y;
    row.pill_w = pill_w;
    row.pill_h = m.row_height;
    row.avatar_cx = x + pill_w + m.avatar_gap + m.avatar_radius;
    row.avatar_cy = y + m.row_height / 2;
    row.badge_cx = row.avatar_cx + m.avatar_radius - m.badge_radius + std::max(1, m.avatar_stroke);
    row.badge_cy = row.avatar_cy + m.avatar_radius - m.badge_radius + std::max(1, m.avatar_stroke);
    row.total_w = pill_w + m.avatar_gap + m.avatar_size + m.avatar_visual_bleed;
    return row;
}

inline RowLayout layout_compact_row(int x, int y, int pill_w, const Metrics& m) {
    RowLayout row;
    row.pill_x = x;
    row.pill_y = y;
    row.pill_w = pill_w;
    row.pill_h = m.row_height;
    row.avatar_cx = x + pill_w - m.avatar_overlap + m.avatar_radius;
    row.avatar_cy = y + m.row_height / 2;
    row.badge_cx = row.avatar_cx + m.avatar_radius - m.badge_radius + std::max(1, m.avatar_stroke);
    row.badge_cy = row.avatar_cy + m.avatar_radius - m.badge_radius + std::max(1, m.avatar_stroke);
    row.total_w = pill_w + (m.avatar_size - m.avatar_overlap) + m.avatar_visual_bleed;
    return row;
}

inline int status_cluster_width(int icon_count, const Metrics& m) {
    if (icon_count <= 0) {
        return 0;
    }

    return m.status_cluster_pad +
           icon_count * m.status_icon_size +
           std::max(0, icon_count - 1) * m.status_icon_gap;
}

inline int pill_width_for_text(int text_width, const Metrics& m, int status_icons = 0) {
    /* Text area with pill padding and optional inline status icons. */
    return std::max(m.row_height,
                    text_width + m.pill_pad_x + m.pill_pad_right +
                    status_cluster_width(status_icons, m));
}

inline int text_max_width_for_pill(int pill_w, const Metrics& m, int status_icons = 0) {
    /* Available text width after pill padding and status icon reserve. */
    return std::max(0, pill_w - m.pill_pad_x - m.pill_pad_right -
                           status_cluster_width(status_icons, m));
}

inline int header_width_for_text(int text_width, const Metrics& m) {
    return std::max(m.header_height, text_width + m.header_pad_x * 2);
}

} /* namespace sigaw::overlay */

#endif /* SIGAW_OVERLAY_LAYOUT_H */
