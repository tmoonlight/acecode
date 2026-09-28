#pragma once

#include <algorithm>

namespace acecode::tui {

inline int vertical_max_scroll_top_row(int content_rows, int viewport_rows) {
    if (content_rows <= 0 || viewport_rows <= 0) {
        return 0;
    }
    return std::max(0, content_rows - viewport_rows);
}

inline int clamp_vertical_scroll_top_row(int scroll_top_row,
                                         int content_rows,
                                         int viewport_rows) {
    return std::clamp(
        scroll_top_row, 0,
        vertical_max_scroll_top_row(content_rows, viewport_rows));
}

inline int vertical_scroll_top_row_by_lines(int scroll_top_row,
                                            int delta_lines,
                                            int content_rows,
                                            int viewport_rows) {
    return clamp_vertical_scroll_top_row(
        scroll_top_row + delta_lines, content_rows, viewport_rows);
}

// FTXUI frames center the requested focus coordinate inside the viewport. Move
// the focus down by half of the external dimension so the requested document
// row lands at the viewport's top edge.
inline int vertical_frame_focus_y_for_scroll_top(int scroll_top_row,
                                                  int viewport_rows) {
    const int top = std::max(0, scroll_top_row);
    const int external_dim = std::max(0, viewport_rows - 1);
    return top + external_dim / 2;
}

struct VerticalScrollbarThumbGeometry {
    int max_top_row = 0;
    int scroll_range_2x = 0;
    int thumb_size_2x = 0;
    int thumb_top_2x = 0;
};

inline VerticalScrollbarThumbGeometry vertical_scrollbar_thumb_geometry(
    int track_y_min,
    int track_height,
    int content_rows,
    int viewport_rows,
    int scroll_top_row) {
    VerticalScrollbarThumbGeometry out;
    out.thumb_top_2x = 2 * track_y_min;

    const int track_2x = std::max(0, 2 * track_height);
    if (track_2x <= 0) {
        return out;
    }

    out.max_top_row =
        vertical_max_scroll_top_row(content_rows, viewport_rows);
    if (content_rows <= 0 || out.max_top_row <= 0) {
        out.thumb_size_2x = track_2x;
        return out;
    }

    const int min_thumb_2x = std::min(6, track_2x);
    int thumb_size = static_cast<int>(
        static_cast<long long>(2 * track_height) * track_height /
        content_rows);
    thumb_size = std::clamp(thumb_size, min_thumb_2x, track_2x);
    out.thumb_size_2x = thumb_size;
    out.scroll_range_2x = track_2x - thumb_size;

    const int clamped_top = clamp_vertical_scroll_top_row(
        scroll_top_row, content_rows, viewport_rows);
    if (out.scroll_range_2x > 0) {
        out.thumb_top_2x += static_cast<int>(
            static_cast<long long>(out.scroll_range_2x) * clamped_top /
            out.max_top_row);
    }
    return out;
}

inline int vertical_scrollbar_grab_offset_2x(
    int mouse_y,
    const VerticalScrollbarThumbGeometry& geometry) {
    if (geometry.scroll_range_2x <= 0 || geometry.thumb_size_2x <= 0) {
        return 0;
    }

    const int mouse_2x = 2 * mouse_y;
    if (mouse_2x >= geometry.thumb_top_2x &&
        mouse_2x <= geometry.thumb_top_2x + geometry.thumb_size_2x) {
        return std::clamp(mouse_2x - geometry.thumb_top_2x,
                          0, geometry.thumb_size_2x);
    }

    // Clicking the track outside the thumb centers the thumb on the pointer.
    return geometry.thumb_size_2x / 2;
}

inline int vertical_scrollbar_y_to_top_row_with_grab(
    int mouse_y,
    int track_y_min,
    const VerticalScrollbarThumbGeometry& geometry,
    int grab_offset_2x) {
    if (geometry.max_top_row <= 0 || geometry.scroll_range_2x <= 0) {
        return 0;
    }

    int thumb_top_rel_2x =
        2 * mouse_y - grab_offset_2x - 2 * track_y_min;
    thumb_top_rel_2x =
        std::clamp(thumb_top_rel_2x, 0, geometry.scroll_range_2x);
    return static_cast<int>(
        static_cast<long long>(thumb_top_rel_2x) * geometry.max_top_row /
        geometry.scroll_range_2x);
}

inline int vertical_scrollbar_y_to_top_row(int mouse_y,
                                           int track_y_min,
                                           int track_height,
                                           int content_rows,
                                           int viewport_rows) {
    const int max_top =
        vertical_max_scroll_top_row(content_rows, viewport_rows);
    if (max_top <= 0 || track_height <= 1) {
        return 0;
    }

    int rel = mouse_y - track_y_min;
    rel = std::clamp(rel, 0, track_height - 1);
    return static_cast<int>(
        static_cast<long long>(rel) * max_top / (track_height - 1));
}

} // namespace acecode::tui
