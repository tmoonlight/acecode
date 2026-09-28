#pragma once
#include "tui/chat_line_measure.hpp"
#include "tui/message_render_cache.hpp"
#include <ftxui/screen/box.hpp>
#include <cstddef>
#include <vector>

namespace acecode { struct TuiState; }
namespace acecode::tui {
// Owns all chat measurement data. Layout writes and event/animation reads hold
// TuiState::mu, matching the original frame and scroll synchronization.
class ChatViewport {
public:
    ChatViewport() = default;
    ChatViewport(const ChatViewport&) = delete;
    ChatViewport& operator=(const ChatViewport&) = delete;
    int rows() const;
    void reset(const TuiState& state);
    void invalidate(int index);
    void sync_from_layout(const TuiState& state);
    void clamp_focus(TuiState& state) const;
    int scroll_by_lines(TuiState& state, int delta_lines) const;

    ftxui::Box chat_box;
    std::vector<ftxui::Box> message_layout_boxes;
    std::vector<char> message_layout_valid;
    std::vector<std::size_t> message_layout_revisions;
    std::vector<int> message_layout_widths;
    std::vector<ChatLineMeasure> message_line_measures;
    std::vector<int> message_line_counts;
    std::vector<int> message_spacer_rows_after;
    int message_line_count_width = 0;
    MessageRenderCache message_render_cache;
private:
    void rebuild_counts(const TuiState& state);
};
} // namespace acecode::tui
