#include <gtest/gtest.h>
#include "tui/chat/chat_viewport.hpp"
#include "tui/chat/message_render_revision.hpp"
#include "tui/tui_state.hpp"

TEST(ChatViewport, ScrollUsesVisibleLinesAndReentersTailAtTheBoundary) {
    // 长消息按显示行滚动;滚回底部后重新开启流式跟随。
    acecode::TuiState state;
    state.conversation = {{"user", "one", false}, {"assistant", "two", false}};
    acecode::tui::ChatViewport viewport;
    viewport.chat_box = {0, 79, 0, 2};
    viewport.message_line_counts = {4, 5};
    viewport.message_spacer_rows_after = {0, 0};
    viewport.clamp_focus(state);
    EXPECT_EQ(state.chat_scroll_top_row, 6);
    EXPECT_TRUE(state.chat_follow_tail);
    EXPECT_EQ(viewport.scroll_by_lines(state, -4), -4);
    EXPECT_EQ(state.chat_scroll_top_row, 2);
    EXPECT_EQ(state.chat_focus_index, 0);
    EXPECT_EQ(state.chat_line_offset, 2);
    EXPECT_FALSE(state.chat_follow_tail);
    EXPECT_EQ(viewport.scroll_by_lines(state, 100), 4);
    EXPECT_EQ(state.chat_scroll_top_row, 6);
    EXPECT_TRUE(state.chat_follow_tail);
    viewport.chat_box.y_max = 19;
    viewport.clamp_focus(state);
    EXPECT_EQ(state.chat_scroll_top_row, 0);
    EXPECT_TRUE(state.chat_follow_tail);
}

TEST(ChatViewport, ChangedWidthAndExpansionDiscardOldHeightMeasurements) {
    // 旧宽度的反射高度不能复用;全局展开也必须触发重测量。
    acecode::TuiState state;
    state.conversation = {{"assistant", "text", false}};
    acecode::tui::ChatViewport viewport;
    viewport.chat_box = {0, 79, 0, 2};
    viewport.reset(state);
    viewport.sync_from_layout(state); // The first frame establishes the width.
    viewport.message_layout_boxes[0] = {0, 79, 0, 7};
    viewport.message_layout_valid[0] = 1;
    viewport.message_layout_widths[0] = 80;
    viewport.message_layout_revisions[0] =
        acecode::tui::message_render_revision(state.conversation[0], false);
    viewport.sync_from_layout(state);
    ASSERT_EQ(viewport.message_line_counts, (std::vector<int>{8}));
    viewport.chat_box.x_max = 39;
    viewport.sync_from_layout(state);
    EXPECT_EQ(viewport.message_line_counts, (std::vector<int>{1}));
    viewport.message_layout_widths[0] = 40;
    viewport.sync_from_layout(state);
    EXPECT_EQ(viewport.message_line_counts, (std::vector<int>{8}));
    state.transcript_expanded = true;
    viewport.sync_from_layout(state);
    EXPECT_EQ(viewport.message_line_counts, (std::vector<int>{1}));
}

TEST(ChatViewport, TranscriptReplacementDropsStaleGeometryAndResetsEmptyFocus) {
    // resume/clear 替换转录后,旧消息的布局不能继续影响空会话的焦点。
    acecode::TuiState state;
    state.conversation = {{"assistant", "text", false}};
    acecode::tui::ChatViewport viewport;
    viewport.reset(state);
    viewport.message_layout_valid[0] = 1;
    viewport.message_line_counts[0] = 12;
    state.conversation.clear();
    viewport.reset(state);
    state.chat_focus_index = 10;
    state.chat_line_offset = 7;
    state.chat_scroll_top_row = 9;
    state.chat_follow_tail = false;
    viewport.clamp_focus(state);
    EXPECT_TRUE(viewport.message_layout_boxes.empty());
    EXPECT_TRUE(viewport.message_line_counts.empty());
    EXPECT_EQ(state.chat_focus_index, -1);
    EXPECT_EQ(state.chat_line_offset, 0);
    EXPECT_EQ(state.chat_scroll_top_row, 0);
    EXPECT_TRUE(state.chat_follow_tail);
}
