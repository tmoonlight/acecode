#include <gtest/gtest.h>
#include "tui/render/frame_layout.hpp"
#include "tui/chat/message_render_revision.hpp"
#include "tui/model/status_line.hpp"
#include "tui/model/turn_lifecycle_rules.hpp"
#include "tui/overlays/rewind_picker_model.hpp"
#include <chrono>

TEST(TuiFrameLayout, PreviousWidthWinsAndFallbackPreservesSidebarThreshold) {
    // 首帧没有 chat_box 时按窗口回退,后续帧必须使用上次真实分配宽度。
    struct Case { int width; bool conhost; int previous; bool sidebar; int markdown; };
    const Case cases[] = {
        {200, false, 0, true, 142}, {200, false, 80, true, 74},
        {120, false, 0, false, 108}, {121, false, 0, true, 63},
        {200, true, 0, false, 188}, {5, false, 0, false, 20},
    };
    for (const auto& item : cases) {
        const auto result = acecode::tui::compute_frame_layout(item.width, item.conhost, item.previous);
        EXPECT_EQ(result.show_regular_sidebar, item.sidebar);
        EXPECT_EQ(result.markdown_render_width, item.markdown);
    }
}
TEST(TuiMessageRevision, GlobalExpansionInvalidatesGeometryButTextUsesItsOwnCacheKey) {
    // Ctrl+O 必须使高度缓存失效;正文哈希继续由独立缓存键负责。
    acecode::TuiState::Message message{"assistant", "first", false};
    const auto original = acecode::tui::message_render_revision(message, false);
    message.content = "changed text";
    EXPECT_EQ(acecode::tui::message_render_revision(message, false), original);
    EXPECT_NE(acecode::tui::message_render_revision(message, true), original);
    message.expanded = true;
    EXPECT_NE(acecode::tui::message_render_revision(message, false), original);
}
TEST(TuiStatusLine, RepeatedTransientMessagesKeepOriginalSavedStatus) {
    // 两秒内再次弹出提示要延后恢复,不能把上一条临时文案当成原始状态。
    using namespace std::chrono;
    const steady_clock::time_point now(seconds(10));
    acecode::TuiState state;
    state.status_line = "ready";
    acecode::tui::set_transient_status_line_locked(state, "copied", now);
    acecode::tui::set_transient_status_line_locked(state, "pasted", now + seconds(1));
    EXPECT_EQ(state.status_line_saved, "ready");
    EXPECT_EQ(state.status_line, "pasted");
    EXPECT_EQ(state.status_line_clear_at, now + seconds(3));
}
TEST(TuiTurnRules, InterruptedAndNonfinalOutputDoNotNotify) {
    // 通知只针对已完成且有最终正文的回合,失败和用户中断不会发出。
    using acecode::tui::should_notify_turn_completion;
    EXPECT_TRUE(should_notify_turn_completion(false, "completed", true, true, true, true));
    EXPECT_FALSE(should_notify_turn_completion(true, "completed", true, true, true, true));
    EXPECT_FALSE(should_notify_turn_completion(false, "error", true, true, true, true));
    EXPECT_FALSE(should_notify_turn_completion(false, "completed", true, false, true, true));
    EXPECT_FALSE(should_notify_turn_completion(false, "completed", false, true, true, true));
    EXPECT_FALSE(should_notify_turn_completion(false, "completed", true, true, false, true));
    EXPECT_FALSE(should_notify_turn_completion(false, "completed", true, true, true, false));
}
TEST(TuiTurnRules, DoneRowRequiresInitializedClockAndOneWholeSecond) {
    // 瞬时回合没有 Done 伪行;开始时间未初始化也不显示巨大的耗时。
    using namespace std::chrono;
    const steady_clock::time_point start(seconds(10));
    EXPECT_FALSE(acecode::tui::turn_done_seconds({}, start));
    EXPECT_FALSE(acecode::tui::turn_done_seconds(start, start - seconds(1)));
    EXPECT_FALSE(acecode::tui::turn_done_seconds(start, start + milliseconds(999)));
    EXPECT_EQ(acecode::tui::turn_done_seconds(start, start + seconds(1)), 1);
}
TEST(TuiRewindModes, SwitchingTargetRemovesCodeModesAndResetsSelection) {
    // 选中不支持代码恢复的消息后,旧目标的 Code 选项不能残留。
    acecode::TuiState state;
    acecode::TuiState::RewindItem item;
    item.can_restore_code = true;
    acecode::tui::populate_rewind_modes_locked(state, item);
    ASSERT_EQ(state.rewind_modes.size(), 4u);
    state.rewind_mode_selected = 2;
    item.can_restore_code = false;
    acecode::tui::populate_rewind_modes_locked(state, item);
    ASSERT_EQ(state.rewind_modes.size(), 2u);
    EXPECT_EQ(state.rewind_mode_selected, 0);
    EXPECT_EQ(state.rewind_modes[0].mode, acecode::TuiState::RewindRestoreMode::ConversationOnly);
    EXPECT_EQ(state.rewind_modes[1].mode, acecode::TuiState::RewindRestoreMode::NeverMind);
}
