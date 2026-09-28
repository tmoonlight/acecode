#include "chat_viewport.hpp"
#include "message_render_revision.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat_scroll.hpp"
#include "tui/chat_message_spacing.hpp"

namespace acecode::tui {
int ChatViewport::rows() const {
    return chat_box.y_max >= chat_box.y_min
        ? chat_box.y_max - chat_box.y_min + 1
        : 0;
}

void ChatViewport::rebuild_counts(const TuiState& state) {
    message_line_counts = acecode::tui::chat_line_counts_from_measures(
        message_line_measures,
        static_cast<int>(state.conversation.size()));
    message_spacer_rows_after =
        acecode::tui::chat_message_spacer_rows_after(state.conversation);
}

void ChatViewport::reset(const TuiState& state) {
    const auto n_msgs = state.conversation.size();
    message_layout_boxes.assign(n_msgs, ftxui::Box{});
    message_layout_valid.assign(n_msgs, 0);
    message_layout_revisions.assign(n_msgs, 0);
    message_layout_widths.assign(n_msgs, 0);
    acecode::tui::resize_chat_line_measures(
        message_line_measures, static_cast<int>(n_msgs));
    acecode::tui::invalidate_chat_line_measures(message_line_measures);
    rebuild_counts(state);
    message_render_cache.resize(n_msgs);
    message_render_cache.invalidate_all();
}

void ChatViewport::invalidate(int index) {
    acecode::tui::invalidate_chat_line_measure(
        message_line_measures, index);
    if (index >= 0 &&
        index < static_cast<int>(message_line_counts.size())) {
        message_line_counts[static_cast<std::size_t>(index)] = 1;
    }
    if (index >= 0 &&
        index < static_cast<int>(message_layout_valid.size())) {
        message_layout_valid[static_cast<std::size_t>(index)] = 0;
    }
    message_render_cache.invalidate(static_cast<std::size_t>(index));
}

void ChatViewport::sync_from_layout(const TuiState& state) {
    const size_t n_msgs = state.conversation.size();
    const int current_message_width = chat_box.x_max >= chat_box.x_min
        ? chat_box.x_max - chat_box.x_min + 1
        : 0;
    const bool line_count_width_changed =
        current_message_width > 0 &&
        current_message_width != message_line_count_width;
    acecode::tui::resize_chat_line_measures(
        message_line_measures, static_cast<int>(n_msgs));
    message_render_cache.ensure_size(n_msgs);
    if (line_count_width_changed) {
        acecode::tui::invalidate_chat_line_measures(
            message_line_measures);
        message_line_count_width = current_message_width;
    }
    for (size_t i = 0; i < n_msgs; ++i) {
        const std::size_t revision = tui::message_render_revision(
            state.conversation[i], state.transcript_expanded);
        const bool has_valid_layout =
            !line_count_width_changed &&
            i < message_layout_boxes.size() &&
            i < message_layout_valid.size() &&
            i < message_layout_revisions.size() &&
            i < message_layout_widths.size() &&
            message_layout_valid[i] != 0 &&
            message_layout_revisions[i] == revision &&
            message_layout_widths[i] == current_message_width &&
            message_layout_boxes[i].y_max >=
                message_layout_boxes[i].y_min;
        const int measured_rows = has_valid_layout
            ? message_layout_boxes[i].y_max -
                  message_layout_boxes[i].y_min + 1
            : 0;
        acecode::tui::sync_chat_line_measure(
            message_line_measures[i],
            has_valid_layout,
            measured_rows,
            current_message_width,
            revision);
    }
    rebuild_counts(state);
}

void ChatViewport::clamp_focus(TuiState& state) const {
    const int viewport_rows = rows();
    if (state.conversation.empty()) {
        state.chat_focus_index = -1;
        state.chat_line_offset = 0;
        state.chat_scroll_top_row = 0;
        state.chat_follow_tail = true;
        return;
    }

    int message_count = static_cast<int>(state.conversation.size());
    int last = message_count - 1;
    const int max_scroll_top =
        acecode::tui::chat_max_scroll_top_row(message_line_counts,
                                              message_count,
                                              viewport_rows,
                                              message_spacer_rows_after);
    if (state.chat_follow_tail) {
        state.chat_focus_index = last;
        state.chat_line_offset =
            acecode::tui::chat_tail_line_offset(message_line_counts, last);
        state.chat_scroll_top_row = max_scroll_top;
        return;
    }

    state.chat_scroll_top_row =
        acecode::tui::clamp_chat_scroll_top_row(state.chat_scroll_top_row,
                                                message_line_counts,
                                                message_count,
                                                viewport_rows,
                                                message_spacer_rows_after);
    auto [idx, off] = acecode::tui::chat_focus_from_display_row(
        message_line_counts, message_count, state.chat_scroll_top_row,
        message_spacer_rows_after);
    state.chat_focus_index = idx;
    state.chat_line_offset = off;
    state.chat_follow_tail = state.chat_scroll_top_row >= max_scroll_top;
}

int ChatViewport::scroll_by_lines(TuiState& state, int delta_lines) const {
    const int viewport_rows = rows();
    if (state.conversation.empty()) return 0;
    const int message_count = static_cast<int>(state.conversation.size());
    int last_msg = message_count - 1;
    const int max_scroll_top =
        acecode::tui::chat_max_scroll_top_row(message_line_counts,
                                              message_count,
                                              viewport_rows,
                                              message_spacer_rows_after);
    if (state.chat_follow_tail) {
        state.chat_focus_index = last_msg;
        state.chat_line_offset =
            acecode::tui::chat_tail_line_offset(message_line_counts,
                                                last_msg);
        state.chat_scroll_top_row = max_scroll_top;
    }

    const int before = acecode::tui::clamp_chat_scroll_top_row(
        state.chat_scroll_top_row, message_line_counts, message_count,
        viewport_rows, message_spacer_rows_after);
    const int after = acecode::tui::clamp_chat_scroll_top_row(
        before + delta_lines, message_line_counts, message_count,
        viewport_rows, message_spacer_rows_after);
    state.chat_scroll_top_row = after;
    const int actual = after - before;

    if (after >= max_scroll_top) {
        state.chat_focus_index = last_msg;
        state.chat_line_offset =
            acecode::tui::chat_tail_line_offset(message_line_counts,
                                                last_msg);
        state.chat_follow_tail = true;
    } else {
        auto [idx, off] = acecode::tui::chat_focus_from_display_row(
            message_line_counts, message_count, after,
            message_spacer_rows_after);
        state.chat_focus_index = idx;
        state.chat_line_offset = off;
        state.chat_follow_tail = false;
    }
    return actual;
}

} // namespace acecode::tui
