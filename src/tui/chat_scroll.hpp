#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "tui/vertical_scroll.hpp"

namespace acecode::tui {

inline int chat_line_count_at(const std::vector<int>& line_counts, int index) {
    if (index < 0 || index >= static_cast<int>(line_counts.size())) {
        return 1;
    }
    return line_counts[index] > 0 ? line_counts[index] : 1;
}

inline int chat_spacer_rows_after_at(
    const std::vector<int>& spacer_rows_after,
    int index) {
    if (index < 0 || index >= static_cast<int>(spacer_rows_after.size())) {
        return 0;
    }
    return std::max(0, spacer_rows_after[index]);
}

inline int update_chat_line_count_estimate(int previous, int measured) {
    if (measured <= 0) {
        return previous > 0 ? previous : 1;
    }
    if (previous <= 0) {
        return measured;
    }
    return std::max(previous, measured);
}

inline int clamp_chat_line_offset(int offset, int line_count) {
    const int rows = line_count > 0 ? line_count : 1;
    return std::clamp(offset, 0, rows - 1);
}

inline int chat_tail_line_offset(const std::vector<int>& line_counts,
                                 int message_index) {
    return chat_line_count_at(line_counts, message_index) - 1;
}

inline bool is_chat_tail_position(int focus_index,
                                  int line_offset,
                                  int message_count,
                                  const std::vector<int>& line_counts) {
    if (message_count <= 0 || focus_index != message_count - 1) {
        return false;
    }
    return line_offset >= chat_tail_line_offset(line_counts, focus_index);
}

inline int chat_transcript_display_rows(const std::vector<int>& line_counts,
                                        int message_count,
                                        const std::vector<int>& spacer_rows_after) {
    if (message_count <= 0) {
        return 0;
    }

    int rows = 0;
    for (int i = 0; i < message_count; ++i) {
        rows += chat_line_count_at(line_counts, i);
        rows += chat_spacer_rows_after_at(spacer_rows_after, i);
    }
    return rows;
}

inline int chat_max_scroll_top_row(const std::vector<int>& line_counts,
                                   int message_count,
                                   int viewport_rows,
                                   const std::vector<int>& spacer_rows_after) {
    if (message_count <= 0 || viewport_rows <= 0) {
        return 0;
    }

    const int transcript_rows =
        chat_transcript_display_rows(
            line_counts, message_count, spacer_rows_after);
    return std::max(0, transcript_rows - viewport_rows);
}

inline int clamp_chat_scroll_top_row(int scroll_top_row,
                                     const std::vector<int>& line_counts,
                                     int message_count,
                                     int viewport_rows,
                                     const std::vector<int>& spacer_rows_after) {
    return std::clamp(scroll_top_row, 0,
                      chat_max_scroll_top_row(line_counts, message_count,
                                              viewport_rows,
                                              spacer_rows_after));
}

inline int chat_display_row_for_focus(const std::vector<int>& line_counts,
                                      int message_count,
                                      int focus_index,
                                      int line_offset,
                                      const std::vector<int>& spacer_rows_after) {
    if (message_count <= 0) {
        return 0;
    }

    focus_index = std::clamp(focus_index, 0, message_count - 1);
    int row = 0;
    for (int i = 0; i < focus_index; ++i) {
        row += chat_line_count_at(line_counts, i);
        row += chat_spacer_rows_after_at(spacer_rows_after, i);
    }
    row += clamp_chat_line_offset(line_offset,
                                  chat_line_count_at(line_counts,
                                                     focus_index));
    return row;
}

inline std::pair<int, int> chat_focus_from_display_row(
    const std::vector<int>& line_counts,
    int message_count,
    int display_row,
    const std::vector<int>& spacer_rows_after) {
    if (message_count <= 0) {
        return {-1, 0};
    }

    const int transcript_rows =
        chat_transcript_display_rows(
            line_counts, message_count, spacer_rows_after);
    if (transcript_rows <= 0) {
        return {-1, 0};
    }
    display_row = std::clamp(display_row, 0, transcript_rows - 1);

    int row = 0;
    for (int i = 0; i < message_count; ++i) {
        const int lines = chat_line_count_at(line_counts, i);
        if (display_row < row + lines) {
            return {i, display_row - row};
        }
        row += lines;

        // Spacer rows are not real message content. Map them to the previous
        // message's tail so commands that act on the focused message remain
        // stable while the viewport top crosses a turn boundary.
        const int spacer_rows =
            chat_spacer_rows_after_at(spacer_rows_after, i);
        if (display_row < row + spacer_rows) {
            return {i, lines - 1};
        }
        row += spacer_rows;
    }

    const int last = message_count - 1;
    return {last, chat_tail_line_offset(line_counts, last)};
}

inline int chat_frame_focus_y_for_scroll_top(int scroll_top_row,
                                             int viewport_rows) {
    return vertical_frame_focus_y_for_scroll_top(
        scroll_top_row, viewport_rows);
}

using ChatScrollbarThumbGeometry = VerticalScrollbarThumbGeometry;

inline ChatScrollbarThumbGeometry chat_scrollbar_thumb_geometry(
    int track_y_min,
    int track_height,
    const std::vector<int>& line_counts,
    int message_count,
    int viewport_rows,
    int scroll_top_row,
    const std::vector<int>& spacer_rows_after) {
    const int content_rows =
        chat_transcript_display_rows(
            line_counts, message_count, spacer_rows_after);
    return vertical_scrollbar_thumb_geometry(
        track_y_min, track_height, content_rows, viewport_rows,
        scroll_top_row);
}

inline int chat_scrollbar_grab_offset_2x(
    int mouse_y,
    const ChatScrollbarThumbGeometry& geometry) {
    return vertical_scrollbar_grab_offset_2x(mouse_y, geometry);
}

inline int chat_scrollbar_y_to_top_row_with_grab(
    int mouse_y,
    int track_y_min,
    const ChatScrollbarThumbGeometry& geometry,
    int grab_offset_2x) {
    return vertical_scrollbar_y_to_top_row_with_grab(
        mouse_y, track_y_min, geometry, grab_offset_2x);
}

inline int chat_scrollbar_y_to_top_row(
    int mouse_y,
    int track_y_min,
    int track_height,
    const std::vector<int>& line_counts,
    int message_count,
    int viewport_rows,
    const std::vector<int>& spacer_rows_after) {
    const int content_rows =
        chat_transcript_display_rows(
            line_counts, message_count, spacer_rows_after);
    return vertical_scrollbar_y_to_top_row(
        mouse_y, track_y_min, track_height, content_rows, viewport_rows);
}

inline int chat_bottom_anchor_top_padding_rows(
    const std::vector<int>& line_counts,
    int message_count,
    int viewport_rows,
    const std::vector<int>& spacer_rows_after) {
    if (message_count <= 0 || viewport_rows <= 0) {
        return 0;
    }

    const int transcript_rows =
        chat_transcript_display_rows(
            line_counts, message_count, spacer_rows_after);
    if (transcript_rows >= viewport_rows) {
        return 0;
    }
    return viewport_rows - transcript_rows;
}

inline bool is_chat_mouse_target(int mouse_x,
                                 int mouse_y,
                                 int chat_x_min,
                                 int chat_y_min,
                                 int chat_x_max,
                                 int chat_y_max,
                                 bool is_wheel_event) {
    if (chat_x_min > chat_x_max || chat_y_min > chat_y_max) {
        return false;
    }
    if (mouse_x < chat_x_min || mouse_x > chat_x_max) {
        return false;
    }
    if (mouse_y >= chat_y_min && mouse_y <= chat_y_max) {
        return true;
    }

    // FTXUI TerminalOutput mode can briefly subtract a stale frame origin when
    // reusing the same terminal. Wheel events then arrive above the chat box
    // even though the pointer is over the chat transcript.
    return is_wheel_event && mouse_y < chat_y_min;
}

} // namespace acecode::tui
